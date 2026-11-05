#include "core/upload/uploader.h"

#include "core/units.h"

#include <picosha2.h>
#include <xicap/xicap.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <utility>

namespace memoria::upload
{
namespace
{

constexpr auto STATE_FILE = "uploads.txt";
// Failures slow the uploader to at most 12 passes' wait, a minute at the default 5 s. Mogsafe ends a recording that
// sends nothing for 5 minutes, and one file it keeps refusing fails every pass.
constexpr int MAX_WAIT_PASSES = 12;

// A refusal's body, kept as one line of the state file.
auto oneLine(std::string text) -> std::string
{
    std::ranges::replace_if(text, [](char c)
                            {
                                return c == '\t' || c == '\r' || c == '\n';
                            },
                            ' ');
    const auto last = text.find_last_not_of(' ');
    return last == std::string::npos ? std::string{} : text.substr(0, last + 1);
}

// Only the first size bytes: the file may still be growing.
auto sha256(const std::filesystem::path& path, const uint64_t size, const std::stop_token& stop) -> std::optional<std::string>
{
    std::ifstream                in(path, std::ios::binary);
    std::vector<char>            chunk(Tus::CHUNK_SIZE);
    picosha2::hash256_one_by_one hasher;
    for (uint64_t left = size; left > 0;)
    {
        const auto n = std::min<uint64_t>(left, chunk.size());
        if (stop.stop_requested() || !in.read(chunk.data(), static_cast<std::streamsize>(n)))
        {
            return std::nullopt;
        }
        hasher.process(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        left -= n;
    }
    hasher.finish();
    return picosha2::get_hash_hex_string(hasher);
}

} // namespace

Uploader::Uploader(UploadOptions options, std::unique_ptr<Http> http)
: options_(std::move(options))
, http_(std::move(http))
, tus_(options_.server, options_.config, *http_)
{
}

Uploader::~Uploader()
{
    {
        const std::scoped_lock lock(wakeMutex_);
        stop_.request_stop();
    }
    http_->cancel();
    wake_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

auto Uploader::start() -> void
{
    thread_ = std::thread(&Uploader::run, this);
}

auto Uploader::run() -> void
{
    auto wait = options_.every;
    while (true)
    {
        bool failed{ false };
        try
        {
            uploading_ = !options_.mayRun || options_.mayRun();
            failed     = uploading_ && uploadAll().has_value();
        }
        catch (const std::exception&) // out of memory, or the folder went away
        {
            failed = true;
        }
        wait = failed ? std::min(wait * 2, options_.every * MAX_WAIT_PASSES) : options_.every;

        std::unique_lock lock(wakeMutex_);
        if (wake_.wait_for(lock, wait, [this]
                           {
                               return stop_.stop_requested() || finishing_;
                           }))
        {
            if (finishing_ && !stop_.stop_requested())
            {
                const auto names = finish_;
                lock.unlock();
                for (const auto& name : names)
                {
                    try
                    {
                        sendLast(name);
                    }
                    catch (const std::exception&) // NOLINT(bugprone-empty-catch)
                    {
                        // What the last pass could not send goes at the next start.
                    }
                }
                lock.lock();
                finished_ = true;
                wake_.notify_all();
            }
            return;
        }
    }
}

auto Uploader::finish(std::vector<std::string> names, const std::chrono::milliseconds limit) -> void
{
    if (!thread_.joinable())
    {
        return;
    }
    std::unique_lock lock(wakeMutex_);
    finish_    = std::move(names);
    finishing_ = true;
    wake_.notify_all();
    wake_.wait_for(lock, limit, [this]
                   {
                       return finished_;
                   });
}

// Up to three steps: the last bytes, then the close the trailer allows. The game waits on it, so finish() bounds it.
auto Uploader::sendLast(const std::string& name) -> void
{
    if (!uploading_ || !tus_.allowed())
    {
        return; // another client sharing the folder sends it
    }
    if (!loaded_)
    {
        load();
        loaded_ = true;
    }
    UploadFile f;
    {
        const std::scoped_lock lock(mutex_);
        f = files_[name];
    }
    last_ = true;
    for (int i = 0; i < 3 && f.stage == Stage::Open && !stop_.stop_requested(); ++i)
    {
        if (step(options_.dir / name, f, std::chrono::milliseconds(0)))
        {
            break;
        }
    }
    {
        const std::scoped_lock lock(mutex_);
        files_[name] = f;
    }
    std::ignore = save();
}

auto Uploader::uploadAll() -> std::optional<std::string>
{
    uploading_ = true;
    if (!loaded_)
    {
        load();
        loaded_ = true; // status() trusts files_ once this is set
    }
    std::optional<std::string> first;
    std::error_code            ec;
    for (const auto& entry : std::filesystem::directory_iterator(options_.dir, ec))
    {
        if (stop_.stop_requested() || finishing_ || !tus_.allowed())
        {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".xicap")
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        UploadFile f;
        {
            const std::scoped_lock lock(mutex_);
            f = files_[name];
        }
        const auto error = step(entry.path(), f, options_.idle);
        if (error)
        {
            f.error = *error;
            first   = first.value_or(name + ": " + *error);
        }
        else
        {
            f.error.clear();
        }
        const std::scoped_lock lock(mutex_);
        files_[name] = f;
    }
    if (const auto error = save())
    {
        first = first.value_or(*error);
    }
    return first;
}

auto Uploader::step(const std::filesystem::path& path, UploadFile& f, const std::chrono::milliseconds idle) -> std::optional<std::string>
{
    if (f.stage != Stage::Open)
    {
        return f.stage == Stage::CutOff ? writeRest(path, f) : std::nullopt;
    }
    if (!f.deniedFor.empty())
    {
        if (f.deniedFor == tus_.token())
        {
            return std::nullopt;
        }
        f.deniedFor.clear(); // another link: its account may own the upload
    }
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return ec.message();
    }
    const auto now = std::chrono::steady_clock::now();
    watch(f, size, now);
    if (size < xicap::HEADER_SIZE || (!f.reason.empty() && f.refusedAt == size))
    {
        return std::nullopt;
    }
    if (f.url.empty())
    {
        const auto res = tus_.request("POST", "/v1/uploads/", { { "Upload-Defer-Length", "1" } });
        if (!res)
        {
            return res.error();
        }
        if (res->status != 201)
        {
            return std::format("create: {}", res->status);
        }
        f.url       = tus_.location(*res);
        f.refusedAt = 0;
        f.reason.clear();
        f.sent = 0;
        {
            const std::scoped_lock lock(mutex_);
            files_[path.filename().string()] = f;
        }
        dirty_ = true;
    }
    // No byte goes until the state on disk holds every URL in memory, or a crash would send the file again as a new upload.
    if (dirty_)
    {
        if (const auto error = save())
        {
            return error;
        }
    }

    const auto res = tus_.request("HEAD", f.url, {});
    if (!res)
    {
        return res.error();
    }
    if (res->status == 410)
    {
        applyEnded(f, *res);
        return std::nullopt;
    }
    if (res->status == 403)
    {
        f.deniedFor = tus_.token();
        return std::nullopt;
    }
    if (res->status == 404)
    {
        // Archived after a close whose answer was lost, or never kept: a close with the same hash tells which.
        if (const auto error = close(path, f, size))
        {
            return error;
        }
        if (f.stage == Stage::Open)
        {
            f.url.clear(); // Mogsafe holds none of it, so the whole file goes again
        }
        return std::nullopt;
    }
    if (res->status != 200)
    {
        return std::format("offset: {}", res->status);
    }
    const auto offset = parseU64(header(*res, "upload-offset"));
    if (!offset)
    {
        return "offset: no Upload-Offset";
    }
    f.sent = *offset;

    if (*offset < size)
    {
        return send(path, f, *offset, size);
    }
    if (now - f.seen >= idle && f.refusedAt != size)
    {
        return close(path, f, size);
    }
    return std::nullopt;
}

auto Uploader::send(const std::filesystem::path& path, UploadFile& f, const uint64_t offset, const uint64_t size) -> std::optional<std::string>
{
    const auto sent = tus_.append(path, f.url, offset, size, [this](uint64_t)
                                  {
                                      return !stop_.stop_requested() && (!finishing_ || last_) && tus_.allowed(); // finish() ends a pass, not its own sending
                                  });
    if (!sent)
    {
        return sent.error();
    }
    f.sent = sent->offset;
    if (sent->answer && sent->answer->status == 410)
    {
        applyEnded(f, *sent->answer);
        return std::nullopt;
    }
    if (sent->answer)
    {
        return std::format("append at {}: {}", sent->offset, sent->answer->status);
    }
    return std::nullopt;
}

auto Uploader::close(const std::filesystem::path& path, UploadFile& f, const uint64_t size) -> std::optional<std::string>
{
    const auto hash = sha256(path, size, stop_.get_token());
    if (!hash)
    {
        return "cannot read the capture";
    }
    const auto body = std::format(R"({{"sha256":"{}"}})", *hash);
    const auto res  = tus_.request("POST", f.url + "/close", { { "Content-Type", "application/json" } }, std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    if (!res)
    {
        return res.error();
    }
    switch (res->status)
    {
        case 200:
            f.stage = Stage::Closed;
            f.error.clear();
            return std::nullopt;
        case 409: // no trailer yet, or bytes missing: wait for the file to change
            f.refusedAt = size;
            return std::nullopt;
        case 404: // Mogsafe never kept it; the caller decides
            return std::nullopt;
        case 410:
            applyEnded(f, *res);
            return std::nullopt;
        case 422: // refused for good: keep the file, stop sending it
            f.refusedAt = size;
            f.reason    = oneLine(res->body);
            return std::nullopt;
        default:
            return std::format("close: {}", res->status);
    }
}

auto Uploader::saved() const -> UploadFiles
{
    return readUploads(options_.dir / STATE_FILE);
}

auto Uploader::load() -> void
{
    auto                   files = saved();
    const std::scoped_lock lock(mutex_);
    files_ = std::move(files);
}

auto Uploader::save() -> std::optional<std::string>
{
    UploadFiles files;
    {
        const std::scoped_lock lock(mutex_);
        files = files_;
    }
    const auto error = writeUploads(options_.dir / STATE_FILE, files);
    dirty_           = error.has_value();
    const std::scoped_lock lock(mutex_);
    saveError_ = error.value_or("");
    return error;
}

auto Uploader::saveError() const -> std::string
{
    const std::scoped_lock lock(mutex_);
    return saveError_;
}

auto Uploader::unauthorized() -> bool
{
    return tus_.unauthorized();
}

auto Uploader::file(const std::string& name) const -> UploadFile
{
    {
        const std::scoped_lock lock(mutex_);
        if (uploading_ && loaded_)
        {
            const auto it = files_.find(name);
            return it == files_.end() ? UploadFile{} : it->second;
        }
    }
    auto files = saved(); // another client sharing the folder sends it
    return files[name];
}

auto Uploader::ended(const std::string& name) const -> bool
{
    const auto stage = file(name).stage;
    return stage != Stage::Open && stage != Stage::Closed;
}

auto Uploader::recordingId(const std::string& name) const -> std::string
{
    const auto url = file(name).url;
    return url.substr(url.find_last_of('/') + 1);
}

auto Uploader::status() const -> std::vector<UploadStatus>
{
    // Until this client has scanned the folder itself, or while another client sends, the state file and the files on disk are the truth.
    UploadFiles files;
    {
        const std::scoped_lock lock(mutex_);
        if (uploading_ && loaded_)
        {
            files = files_;
        }
    }
    const bool fromDisk{ files.empty() };
    if (fromDisk)
    {
        files = saved();
    }
    std::vector<UploadStatus> out;
    std::error_code           ec;
    for (const auto& entry : std::filesystem::directory_iterator(options_.dir, ec))
    {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".xicap")
        {
            continue;
        }
        const auto& f    = files[entry.path().filename().string()];
        const auto  size = fromDisk || f.size == 0 ? entry.file_size(ec) : f.size;
        const bool  refused{ f.stage != Stage::Closed && f.refusedAt == size && !f.reason.empty() };
        const bool  sending{ f.stage == Stage::Open || f.stage == Stage::CutOff };
        out.push_back({ .name = entry.path().filename().string(), .sent = sending ? f.sent : size, .size = size, .refused = refused, .cutOff = f.stage == Stage::CutOff, .foreign = !f.deniedFor.empty(), .error = refused ? f.reason : f.error });
    }
    return out;
}

} // namespace memoria::upload
