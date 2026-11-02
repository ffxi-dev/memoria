#include "core/captain.h"

#include "core/link.h"
#include "core/seed_file.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace memoria
{
namespace
{

constexpr auto STATE_FILE = "imports.txt";
constexpr auto MAX_WAIT   = std::chrono::milliseconds(std::chrono::minutes(5));

constexpr auto CRC_TABLE = []
{
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
        {
            c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}();

auto base64(std::string_view in) -> std::string
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t   v = static_cast<uint8_t>(in[i]) << 16;
        const auto n = std::min<size_t>(3, in.size() - i);
        if (n > 1)
        {
            v |= static_cast<uint8_t>(in[i + 1]) << 8;
        }
        if (n > 2)
        {
            v |= static_cast<uint8_t>(in[i + 2]);
        }
        for (size_t k = 0; k < 4; ++k)
        {
            out += k <= n ? digits[(v >> (18 - 6 * k)) & 63] : '=';
        }
    }
    return out;
}

auto trim(std::string s) -> std::string
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.'))
    {
        s.pop_back();
    }
    return s;
}

} // namespace

auto findCaptain(const std::filesystem::path& root, std::chrono::seconds quiet) -> std::vector<CaptainCapture>
{
    namespace fs = std::filesystem;
    std::vector<CaptainCapture> out;
    std::error_code             ec;
    const auto                  now = fs::file_time_type::clock::now();
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        const auto log = it->path() / "PacketLogger" / "full.log";
        if (!it->is_directory(ec) || !fs::is_regular_file(it->path() / "manifest.txt", ec) || !fs::is_regular_file(log, ec))
        {
            continue;
        }
        it.disable_recursion_pending();
        const auto changed = fs::last_write_time(log, ec);
        out.push_back({
            .dir       = it->path(),
            .folder    = it->path().lexically_relative(root).generic_string(),
            .character = it->path().filename().string(),
            .size      = fs::file_size(log, ec),
            .growing   = ec || now - changed < quiet,
        });
    }
    std::ranges::sort(out, {}, &CaptainCapture::folder);
    return out;
}

auto crc32File(const std::filesystem::path& path, const std::atomic<bool>& stop) -> std::optional<Crc32>
{
    std::ifstream     in(path, std::ios::binary);
    std::vector<char> buf(Tus::CHUNK_SIZE);
    Crc32             result;
    uint32_t          crc = 0xFFFFFFFF;
    while (in)
    {
        if (stop)
        {
            return std::nullopt;
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<size_t>(in.gcount());
        for (size_t i = 0; i < n; ++i)
        {
            crc = CRC_TABLE[(crc ^ static_cast<uint8_t>(buf[i])) & 0xFF] ^ (crc >> 8);
        }
        result.size += n;
    }
    if (!in.eof())
    {
        return std::nullopt;
    }
    result.crc = crc ^ 0xFFFFFFFF;
    return result;
}

auto moveAside(const std::filesystem::path& root, const std::filesystem::path& uploaded, const std::filesystem::path& dir) -> std::expected<std::filesystem::path, std::string>
{
    namespace fs        = std::filesystem;
    const auto      rel = dir.lexically_relative(root);
    std::error_code ec;
    auto            target = uploaded / rel;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
    {
        return std::unexpected(ec.message());
    }
    for (int n = 2; fs::exists(target, ec); ++n)
    {
        target = uploaded / rel;
        target += std::format("-{}", n);
    }
    fs::rename(dir, target, ec);
    if (ec)
    {
        return std::unexpected(ec.message());
    }
    // remove() refuses a folder that is not empty, which ends the climb.
    for (auto p = dir.parent_path(); p.native().size() > root.native().size() && fs::remove(p, ec); p = p.parent_path())
    {
    }
    return target;
}

auto remindImport(const std::filesystem::path& config, const std::filesystem::path& root) -> bool
{
    std::error_code ec;
    if (!loadLink(config / "token") || std::filesystem::exists(config / "import-reminded", ec))
    {
        return false;
    }
    const auto found = findCaptain(root);
    return std::ranges::any_of(found, [](const auto& c)
                               {
                                   return !c.growing;
                               }) &&
           createNew(config / "import-reminded");
}

auto sizeText(uint64_t bytes) -> std::string
{
    constexpr std::array units{ "KB", "MB", "GB" };
    if (bytes < 1024)
    {
        return std::format("{} B", bytes);
    }
    auto   value = static_cast<double>(bytes) / 1024.0;
    size_t unit  = 0;
    for (; value >= 1024 && unit + 1 < units.size(); ++unit)
    {
        value /= 1024;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

Importer::Importer(ImportOptions options, std::unique_ptr<Http> http)
: options_(std::move(options))
, http_(std::move(http))
, tus_(options_.server, options_.config, *http_)
{
}

Importer::~Importer()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    http_->cancel();
    wake_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

auto Importer::pending(const std::filesystem::path& state) -> bool
{
    std::error_code ec;
    return std::filesystem::exists(state / STATE_FILE, ec);
}

auto Importer::start(bool asked) -> void
{
    thread_ = std::thread([this, asked]
                          {
#ifdef _WIN32
                              ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                              // Taken on this thread, which releases it: Windows hands a mutex whose thread died to the next client.
                              const auto mutex = options_.mutex.empty() ? nullptr : ::CreateMutexA(nullptr, FALSE, options_.mutex.c_str());
                              const auto taken = mutex ? ::WaitForSingleObject(mutex, 0) : WAIT_OBJECT_0;
                              if (taken != WAIT_OBJECT_0 && taken != WAIT_ABANDONED)
                              {
                                  if (asked)
                                  {
                                      say("Another client sharing this install is importing.");
                                  }
                                  ::CloseHandle(mutex);
                                  finished_ = true;
                                  return;
                              }
#endif
                              try
                              {
                                  run();
                              }
                              catch (const std::exception& error)
                              {
                                  say(std::format("Captain import stopped: {}", error.what()));
                              }
#ifdef _WIN32
                              if (mutex)
                              {
                                  ::ReleaseMutex(mutex);
                                  ::CloseHandle(mutex);
                              }
#endif
                              finished_ = true;
                          });
}

auto Importer::run() -> void
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(options_.state, ec);
    load();
    if (const auto error = save()) // its existence marks the run as asked for until it ends
    {
        say(std::format("Captain import stopped: {}.", *error));
        return;
    }

    auto found = findCaptain(options_.captures, options_.quiet);
    count_     = found.size();
    if (found.empty())
    {
        say("No captain captures to upload.");
    }
    else
    {
        uint64_t total = 0;
        for (const auto& c : found)
        {
            total += c.growing ? 0 : c.size;
        }
        say(std::format("Uploading {} captain capture{} ({}) in the background.", found.size(), found.size() == 1 ? "" : "s", sizeText(total)));
    }

    std::map<Outcome, size_t> tally;
    for (size_t i = 0; i < found.size(); ++i)
    {
        const auto c = found[i];
        {
            std::lock_guard lock(mutex_);
            character_ = c.character;
        }
        index_            = i + 1;
        sent_             = 0;
        size_             = 0;
        const auto prefix = std::format("Captain {}/{} {}: ", i + 1, found.size(), c.folder);
        const auto result = c.growing ? Outcome::Skipped : upload(c, prefix);
        if (c.growing)
        {
            say(prefix + "still being written; skipped.");
        }
        if (result == Outcome::Stopped)
        {
            return; // imports.txt stays, so the next start or link resumes
        }
        if (result == Outcome::Failed)
        {
            found.clear(); // no summary: the error ended the run
            break;
        }
        ++tally[result];
    }

    std::string summary = std::format("Captain import done: {} uploaded, {} already in Vault, {} refused", tally[Outcome::Uploaded], tally[Outcome::InVault], tally[Outcome::Refused]);
    if (tally[Outcome::Skipped] > 0)
    {
        summary += std::format(", {} skipped", tally[Outcome::Skipped]);
    }
    if (tally[Outcome::NotMoved] > 0)
    {
        summary += std::format(", {} in Vault but not moved", tally[Outcome::NotMoved]);
    }
    if (!found.empty())
    {
        say(summary + ". Uploaded folders are in addons/captain/captures-uploaded.");
    }
    fs::remove(options_.state / STATE_FILE, ec);
    fs::remove(options_.state / "imports", ec); // only once empty
}

auto Importer::upload(const CaptainCapture& c, const std::string& prefix) -> Outcome
{
    namespace fs   = std::filesystem;
    const auto log = c.dir / "PacketLogger" / "full.log";
    auto       e   = entries_[c.folder];
    if (e.crc.empty() || e.size != c.size)
    {
        const auto crc = crc32File(log, stopping_);
        if (!crc)
        {
            entries_.erase(c.folder);
            if (stopping_)
            {
                return Outcome::Stopped;
            }
            say(prefix + "cannot read its full.log; skipped.");
            return Outcome::Skipped;
        }
        e = { .crc = std::format("{:08x}", crc->crc), .size = crc->size };
    }
    std::ostringstream manifest;
    manifest << std::ifstream(c.dir / "manifest.txt", std::ios::binary).rdbuf(); // closed at once: Windows will not move a folder with a file open

    const auto staged = options_.state / "imports" / std::format("{}-{}.zst", e.crc, e.size);
    const auto forget = [&]
    {
        entries_.erase(c.folder);
        save();
        std::error_code ec;
        fs::remove(staged, ec);
    };
    const auto refused = [&](const HttpResponse& res)
    {
        forget();
        say(prefix + std::format("refused: {}.", trim(res.body).empty() ? std::to_string(res.status) : trim(res.body)));
        return Outcome::Refused;
    };
    const auto metadata = std::format("format {},manifest {},character {},crc32 {},size {},folder {}", base64("captain"), base64(manifest.str()), base64(c.character), base64(e.crc), base64(std::to_string(e.size)), base64(c.folder));

    auto backoff = options_.retry;
    while (ready())
    {
        const auto again = [&]
        {
            wait(backoff);
            backoff = std::min<std::chrono::milliseconds>(backoff * 2, MAX_WAIT);
        };
        if (e.url.empty())
        {
            const auto res = tus_.request("POST", options_.server + "/v1/uploads/", { { "Upload-Defer-Length", "1" }, { "Upload-Metadata", metadata } });
            if (res && res->status == 201)
            {
                e.url              = tus_.location(*res);
                entries_[c.folder] = e;
                // Saved before the first byte, so a crash resumes this upload. On failure imports.txt stays for the next start or link.
                if (const auto error = save())
                {
                    say(prefix + *error + ". Import stopped; it goes on at the next start or link.");
                    return Outcome::Stopped;
                }
            }
            else if (res && res->status == 409)
            {
                forget();
                return moved(c, prefix, std::format("already in Vault ({}).", header(*res, "mogsafe-capture")), Outcome::InVault);
            }
            else if (res && res->status >= 400 && res->status < 500)
            {
                return refused(*res);
            }
            else
            {
                again();
                continue;
            }
        }

        const auto head = tus_.request("HEAD", e.url, {});
        if (head && (head->status == 404 || head->status == 410))
        {
            // Closed after an answer that was lost, or gone: a new create says which.
            e.url.clear();
            entries_[c.folder] = e;
            save();
            again();
            continue;
        }
        const auto offset = head && head->status == 200 ? parseU64(header(*head, "upload-offset")) : std::nullopt;
        if (!offset)
        {
            again();
            continue;
        }

        std::error_code ec;
        if (!fs::exists(staged, ec))
        {
            if (*offset > 0)
            {
                // Another compression of the log may not match the bytes Mogsafe holds.
                e.url.clear();
                continue;
            }
            if (const auto error = stage(log, staged))
            {
                if (stopping_)
                {
                    return Outcome::Stopped;
                }
                say(prefix + "cannot stage the log: " + *error + ". Import stopped.");
                return Outcome::Failed;
            }
        }
        const auto total = fs::file_size(staged, ec);
        size_            = total;
        sent_            = *offset;
        if (*offset < total)
        {
            const auto sent = tus_.append(staged, e.url, *offset, total, [this](uint64_t at)
                                          {
                                              sent_ = at;
                                              return !stopping_ && tus_.allowed();
                                          });
            if (!sent)
            {
                again();
                continue;
            }
            sent_ = sent->offset;
            if (sent->offset > *offset)
            {
                backoff = options_.retry;
            }
            if (sent->answer && sent->answer->status == 410)
            {
                e.url.clear();
                entries_[c.folder] = e;
                save();
                continue;
            }
            if (sent->answer && sent->answer->status == 422)
            {
                return refused(*sent->answer);
            }
            if (sent->answer)
            {
                again();
            }
            continue; // paused, stopped, or done: the next HEAD tells
        }

        const std::string body  = "{}";
        const auto        close = tus_.request("POST", e.url + "/close", { { "Content-Type", "application/json" } }, std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
        if (close && close->status == 200)
        {
            forget();
            return moved(c, prefix, "uploaded.", Outcome::Uploaded);
        }
        if (close && close->status >= 400 && close->status < 500 && close->status != 409 && close->status != 423)
        {
            return refused(*close);
        }
        again();
    }
    if (!stopping_)
    {
        say("Captain import stopped: this install is not linked. It goes on after /memoria link.");
    }
    return Outcome::Stopped;
}

auto Importer::moved(const CaptainCapture& c, const std::string& prefix, const std::string& done, Outcome outcome) -> Outcome
{
    const auto to = moveAside(options_.captures, options_.uploaded, c.dir);
    if (!to)
    {
        say(prefix + "in Vault, but its folder could not move: " + to.error() + ".");
        return Outcome::NotMoved;
    }
    say(prefix + done);
    return outcome;
}

// zstd level 3 on one thread, to a .tmp renamed when whole: a crash leaves a whole file or none.
auto Importer::stage(const std::filesystem::path& log, const std::filesystem::path& out) -> std::optional<std::string>
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    auto       tmp  = fs::path(out).concat(".tmp");
    const auto fail = [&](std::string why)
    {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return std::optional(std::move(why));
    };

    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, 3);
    std::ifstream     in(log, std::ios::binary);
    std::vector<char> inBuf(ZSTD_CStreamInSize());
    std::vector<char> outBuf(ZSTD_CStreamOutSize());
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!in || !o)
        {
            return fail("cannot open it");
        }
        for (bool last = false; !last;)
        {
            if (stopping_)
            {
                o.close();
                return fail("stopped");
            }
            in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
            if (in.bad())
            {
                o.close();
                return fail("cannot read it");
            }
            last = in.eof();
            ZSTD_inBuffer input{ inBuf.data(), static_cast<size_t>(in.gcount()), 0 };
            for (bool done = false; !done;)
            {
                ZSTD_outBuffer output{ outBuf.data(), outBuf.size(), 0 };
                const auto     remaining = ZSTD_compressStream2(cctx.get(), &output, &input, last ? ZSTD_e_end : ZSTD_e_continue);
                if (ZSTD_isError(remaining))
                {
                    o.close();
                    return fail(ZSTD_getErrorName(remaining));
                }
                o.write(outBuf.data(), static_cast<std::streamsize>(output.pos));
                done = last ? remaining == 0 : input.pos == input.size;
            }
        }
        o.close();
        if (!o)
        {
            return fail("cannot write " + tmp.string());
        }
    }
    fs::rename(tmp, out, ec);
    if (ec)
    {
        return fail(ec.message());
    }
    return std::nullopt;
}

// Waits out a pause; false once stopping or unlinked.
auto Importer::ready() -> bool
{
    while (!stopping_)
    {
        if (tus_.allowed())
        {
            return true;
        }
        // The token decides, not the paused file, which /memoria upload on may delete between the two reads.
        if (!loadLink(options_.config / "token"))
        {
            return false;
        }
        wait(options_.retry);
    }
    return false;
}

auto Importer::wait(std::chrono::milliseconds time) -> void
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, time, [this]
                   {
                       return stopping_.load();
                   });
}

auto Importer::say(const std::string& text) -> void
{
    if (options_.say)
    {
        options_.say(text);
    }
}

// One line per capture in flight: folder, CRC-32, size, and upload URL, separated by tabs.
auto Importer::load() -> void
{
    std::ifstream in(options_.state / STATE_FILE);
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream fields(line);
        std::string        folder, crc, size, url;
        if (std::getline(fields, folder, '\t') && std::getline(fields, crc, '\t') && std::getline(fields, size, '\t') && std::getline(fields, url))
        {
            entries_[folder] = { .crc = crc, .size = parseU64(size).value_or(0), .url = url };
        }
    }
}

auto Importer::save() -> std::optional<std::string>
{
    std::string text;
    for (const auto& [folder, e] : entries_)
    {
        if (!e.url.empty())
        {
            text += std::format("{}\t{}\t{}\t{}\n", folder, e.crc, e.size, e.url);
        }
    }
    return replaceFile(options_.state / STATE_FILE, text);
}

auto Importer::finished() const -> bool
{
    return finished_;
}

auto Importer::progress() const -> ImportProgress
{
    std::lock_guard lock(mutex_);
    return { .index = index_, .count = count_, .character = character_, .sent = sent_, .size = size_ };
}

auto Importer::unauthorized() -> bool
{
    return tus_.unauthorized();
}

} // namespace memoria
