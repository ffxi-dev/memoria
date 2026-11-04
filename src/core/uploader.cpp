#include "core/uploader.h"

#include <picosha2.h>
#include <xicap/constants.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <utility>

namespace memoria
{
namespace
{

constexpr uint64_t CHUNK_SIZE = Tus::CHUNK_SIZE;
constexpr auto     STATE_FILE = "uploads.txt";
constexpr auto     MAX_WAIT   = std::chrono::minutes(5);

// A refusal's body as one line of the state file.
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

auto readRange(const std::filesystem::path& path, uint64_t offset, uint64_t n) -> std::optional<std::vector<uint8_t>>
{
    std::ifstream        in(path, std::ios::binary);
    std::vector<uint8_t> buf(n);
    if (!in.seekg(static_cast<std::streamoff>(offset)) || !in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n)))
    {
        return std::nullopt;
    }
    return buf;
}

// Only the first size bytes: the file may still be growing.
auto sha256(const std::filesystem::path& path, uint64_t size, const std::atomic<bool>& stopping) -> std::optional<std::string>
{
    picosha2::hash256_one_by_one hasher;
    for (uint64_t offset = 0; offset < size; offset += CHUNK_SIZE)
    {
        if (stopping)
        {
            return std::nullopt;
        }
        const auto chunk = readRange(path, offset, std::min(size - offset, CHUNK_SIZE));
        if (!chunk)
        {
            return std::nullopt;
        }
        hasher.process(chunk->begin(), chunk->end());
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
        std::lock_guard lock(wakeMutex_);
        stopping_ = true;
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
        bool failed = true;
        try
        {
            uploading_ = !options_.mayRun || options_.mayRun();
            failed     = uploading_ && pass().has_value();
        }
        catch (const std::exception&) // out of memory, or the folder went away
        {
        }
        wait = failed ? std::min<std::chrono::milliseconds>(wait * 2, MAX_WAIT) : options_.every;

        std::unique_lock lock(wakeMutex_);
        if (wake_.wait_for(lock, wait, [this]
                           {
                               return stopping_.load();
                           }))
        {
            return;
        }
    }
}

auto Uploader::pass() -> std::optional<std::string>
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
        if (stopping_ || !allowed())
        {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".xicap")
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        File       f;
        {
            std::lock_guard lock(mutex_);
            f = files_[name];
        }
        const auto error = step(entry.path(), f);
        if (error)
        {
            f.error = *error;
            first   = first.value_or(name + ": " + *error);
        }
        else
        {
            f.error.clear();
        }
        std::lock_guard lock(mutex_);
        files_[name] = f;
    }
    if (const auto error = save())
    {
        first = first.value_or(*error);
    }
    return first;
}

auto Uploader::step(const std::filesystem::path& path, File& f) -> std::optional<std::string>
{
    if (f.closed || f.ended)
    {
        return std::nullopt;
    }
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return ec.message();
    }
    const auto now = std::chrono::steady_clock::now();
    if (size != f.size || f.seen == std::chrono::steady_clock::time_point{})
    {
        f.size = size;
        f.seen = now;
    }
    if (size < xicap::HEADER_SIZE) // Mogsafe refuses a first append without the whole header
    {
        return std::nullopt;
    }
    if (f.url.empty())
    {
        const auto res = request("POST", options_.server + "/v1/uploads/", { { "Upload-Defer-Length", "1" } });
        if (!res)
        {
            return res.error();
        }
        if (res->status != 201)
        {
            return std::format("create: {}", res->status);
        }
        f.url     = tus_.location(*res);
        f.refused = 0;
        f.reason.clear();
        f.sent = 0;
        {
            std::lock_guard lock(mutex_);
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

    const auto res = request("HEAD", f.url, {});
    if (!res)
    {
        return res.error();
    }
    if (res->status == 410)
    {
        end(f, *res);
        return std::nullopt;
    }
    if (res->status == 404)
    {
        // Archived after a close whose answer was lost, or never kept: a close with the same hash tells which.
        if (const auto error = close(path, f, size))
        {
            return error;
        }
        if (!f.closed && !f.ended)
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
    if (now - f.seen >= options_.idle && f.refused != size)
    {
        return close(path, f, size);
    }
    return std::nullopt;
}

auto Uploader::send(const std::filesystem::path& path, File& f, uint64_t offset, uint64_t size) -> std::optional<std::string>
{
    const auto sent = tus_.append(path, f.url, offset, size, [this](uint64_t)
                                  {
                                      return !stopping_ && allowed();
                                  });
    if (!sent)
    {
        return sent.error();
    }
    f.sent = sent->offset;
    if (sent->answer && sent->answer->status == 410)
    {
        end(f, *sent->answer);
        return std::nullopt;
    }
    if (sent->answer)
    {
        return std::format("append at {}: {}", sent->offset, sent->answer->status);
    }
    return std::nullopt;
}

auto Uploader::close(const std::filesystem::path& path, File& f, uint64_t size) -> std::optional<std::string>
{
    const auto hash = sha256(path, size, stopping_);
    if (!hash)
    {
        return "cannot read the capture";
    }
    const auto body = std::format(R"({{"sha256":"{}"}})", *hash);
    const auto res  = request("POST", f.url + "/close", { { "Content-Type", "application/json" } }, std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    if (!res)
    {
        return res.error();
    }
    switch (res->status)
    {
        case 200:
            f.closed = true;
            f.error.clear();
            return std::nullopt;
        case 409: // no footer yet, or bytes missing: wait for the file to change
            f.refused = size;
            return std::nullopt;
        case 404: // Mogsafe never kept it; the caller decides
            return std::nullopt;
        case 410:
            end(f, *res);
            return std::nullopt;
        case 422: // refused for good: keep the file, stop sending it
            f.refused = size;
            f.reason  = oneLine(res->body);
            return std::nullopt;
        default:
            return std::format("close: {}", res->status);
    }
}

// Mogsafe answered 410. A complete capture whose close answer was lost is closed; any other ended upload is dropped,
// and its file never sent again: a recording still running goes on in a new file and upload.
auto Uploader::end(File& f, const HttpResponse& res) -> void
{
    if (header(res, "mogsafe-ended") == "complete")
    {
        f.closed = true;
        return;
    }
    f.ended  = true;
    f.cutOff = header(res, "mogsafe-ended") != "refused" && !header(res, "mogsafe-ended").empty();
    f.url.clear();
}

auto Uploader::allowed() -> bool
{
    return tus_.allowed();
}

auto Uploader::request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    return tus_.request(method, url, std::move(headers), body);
}

// One line per file: name, url, closed (1, 0, cut_off, or ended for refused), refused, bytes the server holds, and why Mogsafe refused it, separated by tabs.
auto Uploader::saved() const -> std::map<std::string, File>
{
    std::map<std::string, File> files;
    std::ifstream               in(options_.dir / STATE_FILE);
    std::string                 line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string        name, url, closed, refused, sent, reason;
        if (std::getline(fields, name, '\t') && std::getline(fields, url, '\t') && std::getline(fields, closed, '\t') && std::getline(fields, refused, '\t'))
        {
            std::getline(fields, sent, '\t');
            std::getline(fields, reason);
            files[name] = { .url = url, .closed = closed == "1", .ended = closed == "ended" || closed == "cut_off", .cutOff = closed == "cut_off", .refused = parseU64(refused).value_or(0), .reason = reason, .sent = parseU64(sent).value_or(0) };
        }
    }
    return files;
}

auto Uploader::load() -> void
{
    auto            files = saved();
    std::lock_guard lock(mutex_);
    files_ = std::move(files);
}

auto Uploader::save() -> std::optional<std::string>
{
    std::string text;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, f] : files_)
        {
            text += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", name, f.url, f.closed ? "1" : f.cutOff ? "cut_off"
                                                                                      : f.ended    ? "ended"
                                                                                                   : "0",
                                f.refused,
                                f.sent,
                                f.reason);
        }
    }
    const auto error = replaceFile(options_.dir / STATE_FILE, text);
    dirty_           = error.has_value();
    std::lock_guard lock(mutex_);
    saveError_ = error.value_or("");
    return error;
}

auto Uploader::saveError() const -> std::string
{
    std::lock_guard lock(mutex_);
    return saveError_;
}

auto Uploader::unauthorized() -> bool
{
    return tus_.unauthorized();
}

auto Uploader::ended(const std::string& name) const -> bool
{
    {
        std::lock_guard lock(mutex_);
        if (uploading_ && loaded_)
        {
            const auto it = files_.find(name);
            return it != files_.end() && it->second.ended;
        }
    }
    const auto files = saved(); // another client sharing the folder sends it
    const auto it    = files.find(name);
    return it != files.end() && it->second.ended;
}

auto Uploader::uploading() const -> bool
{
    return uploading_;
}

auto Uploader::status() const -> std::vector<UploadStatus>
{
    // Until this client has scanned the folder itself, or while another client sends, the state file and the files on disk are the truth.
    std::map<std::string, File> files;
    {
        std::lock_guard lock(mutex_);
        if (uploading_ && loaded_)
        {
            files = files_;
        }
    }
    const bool fromDisk = files.empty();
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
        const auto& f       = files[entry.path().filename().string()];
        const auto  size    = fromDisk || f.size == 0 ? entry.file_size(ec) : f.size;
        const bool  refused = !f.closed && f.refused == size && !f.reason.empty();
        out.push_back({ .name = entry.path().filename().string(), .sent = f.closed || (f.ended && !f.cutOff) ? size : f.sent, .size = size, .refused = refused, .ended = f.cutOff, .error = refused ? f.reason : f.error });
    }
    return out;
}

} // namespace memoria
