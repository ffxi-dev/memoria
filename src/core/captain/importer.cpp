#include "core/captain/importer.h"

#include "core/account/link.h"
#include "core/captain/compress.h"
#include "core/units.h"

#include <algorithm>
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

namespace memoria::captain
{
namespace
{

constexpr auto STATE_FILE = "imports.txt";
constexpr auto MAX_WAIT   = std::chrono::milliseconds(std::chrono::minutes(5));

auto base64(const std::string_view in) -> std::string
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t   v{ static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16 };
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

Importer::Importer(ImportOptions options, std::unique_ptr<Http> http)
: options_(std::move(options))
, http_(std::move(http))
, tus_(options_.server, options_.config, *http_)
{
}

Importer::~Importer()
{
    {
        const std::scoped_lock lock(mutex_);
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

auto Importer::start(const bool asked) -> void
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
        uint64_t total{ 0 };
        for (const auto& c : found)
        {
            total += c.growing ? 0 : c.size + c.searchSize;
        }
        say(std::format("Uploading {} captain capture{} ({}) in the background.", found.size(), found.size() == 1 ? "" : "s", sizeText(total)));
    }

    std::map<Outcome, size_t> tally;
    for (size_t i = 0; i < found.size(); ++i)
    {
        const auto c = found[i];
        {
            const std::scoped_lock lock(mutex_);
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

    std::string summary{ std::format("Captain import done: {} uploaded, {} already in Vault, {} refused", tally[Outcome::Uploaded], tally[Outcome::InVault], tally[Outcome::Refused]) };
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

    // A search log goes ahead of full.log in the same upload. It is staged alone before the create, which gives its length,
    // then the upload's staged file is that stage followed by full.log's.
    const auto searchLog    = c.dir / "SearchLog" / "search.log";
    const auto staged       = options_.state / "imports" / std::format("{}-{}.zst", e.crc, e.size);
    const auto searchStaged = options_.state / "imports" / std::format("{}-{}.search.zst", e.crc, e.size);
    const auto forget       = [&]
    {
        entries_.erase(c.folder);
        save();
        std::error_code ec;
        fs::remove(staged, ec);
        fs::remove(searchStaged, ec);
    };
    const auto refused = [&](const HttpResponse& res)
    {
        forget();
        say(prefix + std::format("refused: {}.", trim(res.body).empty() ? std::to_string(res.status) : trim(res.body)));
        return Outcome::Refused;
    };
    const auto  metadata = std::format("format {},manifest {},character {},crc32 {},size {},folder {}", base64("captain"), base64(manifest.str()), base64(c.character), base64(e.crc), base64(std::to_string(e.size)), base64(c.folder));
    std::string replaces; // the capture this upload gives its search log, as the create said

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
            std::string search;
            if (c.searchSize > 0)
            {
                const auto crc = crc32File(searchLog, stopping_);
                if (!crc)
                {
                    if (stopping_)
                    {
                        return Outcome::Stopped;
                    }
                    forget();
                    say(prefix + "cannot read its search.log; skipped.");
                    return Outcome::Skipped;
                }
                std::error_code ec;
                if (!fs::exists(searchStaged, ec))
                {
                    fs::remove(staged, ec); // a stage of full.log without this search stage ahead of it
                    if (const auto error = compressLog(searchLog, searchStaged, {}, stopping_))
                    {
                        if (stopping_)
                        {
                            return Outcome::Stopped;
                        }
                        say(prefix + "cannot stage the search log: " + *error + ". Import stopped.");
                        return Outcome::Failed;
                    }
                }
                search = std::format(",search_bytes {},search_crc32 {},search_size {}", base64(std::to_string(fs::file_size(searchStaged, ec))), base64(std::format("{:08x}", crc->crc)), base64(std::to_string(crc->size)));
            }
            const auto res = tus_.request("POST", options_.server + "/v1/uploads/", { { "Upload-Defer-Length", "1" }, { "Upload-Metadata", metadata + search } });
            if (res && res->status == 201)
            {
                replaces           = header(*res, "mogsafe-replaces");
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
            // Another compression of either log may not match the bytes Mogsafe holds, or the search part's length it was given.
            if (*offset > 0 || (c.searchSize > 0 && !fs::exists(searchStaged, ec)))
            {
                e.url.clear();
                continue;
            }
            if (const auto error = compressLog(log, staged, c.searchSize > 0 ? searchStaged : fs::path{}, stopping_))
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

        constexpr std::string_view body  = "{}";
        const auto                 close = tus_.request("POST", e.url + "/close", { { "Content-Type", "application/json" } }, std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
        if (close && close->status == 200)
        {
            forget();
            const auto done = [&]() -> std::string
            {
                if (!replaces.empty())
                {
                    return std::format("uploaded with its search log, replacing {}.", replaces);
                }
                if (c.searchSize > 0)
                {
                    return "uploaded with its search log.";
                }
                return "uploaded.";
            }();
            return moved(c, prefix, done, Outcome::Uploaded);
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

auto Importer::moved(const CaptainCapture& c, const std::string& prefix, const std::string& done, const Outcome outcome) -> Outcome
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

auto Importer::ready() -> bool
{
    while (!stopping_)
    {
        if (tus_.allowed())
        {
            return true;
        }
        // The token decides, not the paused file, which /memoria upload on may delete between the two reads.
        if (!account::loadLink(options_.config / "token"))
        {
            return false;
        }
        wait(options_.retry);
    }
    return false;
}

auto Importer::wait(const std::chrono::milliseconds time) -> void
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
        std::string        folder;
        std::string        crc;
        std::string        size;
        std::string        url;
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
    const std::scoped_lock lock(mutex_);
    return { .index = index_, .count = count_, .character = character_, .sent = sent_, .size = size_ };
}

auto Importer::unauthorized() -> bool
{
    return tus_.unauthorized();
}

} // namespace memoria::captain
