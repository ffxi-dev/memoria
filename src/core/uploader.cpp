#include "core/uploader.h"

#include "core/link.h"

#include <picosha2.h>
#include <xicap/constants.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <utility>

namespace memoria
{
namespace
{

constexpr uint64_t CHUNK_SIZE = 1 << 20;
constexpr auto     STATE_FILE = "uploads.txt";
constexpr auto     MAX_WAIT   = std::chrono::minutes(5);

auto parseU64(std::string_view s) -> std::optional<uint64_t>
{
    uint64_t v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && end == s.data() + s.size() ? std::optional(v) : std::nullopt;
}

auto header(const HttpResponse& res, const std::string& name) -> std::string
{
    const auto it = res.headers.find(name);
    return it == res.headers.end() ? std::string{} : it->second;
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
auto sha256(const std::filesystem::path& path, uint64_t size) -> std::optional<std::string>
{
    picosha2::hash256_one_by_one hasher;
    for (uint64_t offset = 0; offset < size; offset += CHUNK_SIZE)
    {
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
{
}

Uploader::~Uploader()
{
    {
        std::lock_guard lock(wakeMutex_);
        stopping_ = true;
    }
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
        else if (f.refused != f.size)
        {
            f.error.clear(); // a refusal stays until the file changes
        }
        std::lock_guard lock(mutex_);
        files_[name] = f;
    }
    save();
    return first;
}

auto Uploader::step(const std::filesystem::path& path, File& f) -> std::optional<std::string>
{
    if (f.closed)
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
        f.url     = header(*res, "location");
        f.refused = 0;
        f.sent    = 0;
        if (f.url.starts_with('/'))
        {
            f.url = options_.server.substr(0, options_.server.find('/', options_.server.find("://") + 3)) + f.url;
        }
    }

    const auto res = request("HEAD", f.url, {});
    if (!res)
    {
        return res.error();
    }
    if (res->status == 404)
    {
        // Archived after a close whose answer was lost, or gone: a close with the same hash tells which.
        close(path, f, size);
        if (!f.closed)
        {
            f.url.clear();
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
    while (offset < size && !stopping_ && allowed())
    {
        const auto n    = std::min(size - offset, CHUNK_SIZE);
        const auto data = readRange(path, offset, n);
        if (!data)
        {
            return "cannot read the capture";
        }
        const auto res = request("PATCH", f.url, { { "Upload-Offset", std::to_string(offset) }, { "Content-Type", "application/offset+octet-stream" } }, *data);
        if (!res)
        {
            return res.error();
        }
        if (res->status == 410)
        {
            f.url.clear(); // cut off or closed on the server: the whole file goes again as a new capture
            return std::nullopt;
        }
        if (res->status != 204)
        {
            return std::format("append at {}: {}", offset, res->status);
        }
        offset += n;
        f.sent = offset;
    }
    return std::nullopt;
}

auto Uploader::close(const std::filesystem::path& path, File& f, uint64_t size) -> std::optional<std::string>
{
    const auto hash = sha256(path, size);
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
        case 422: // refused for good: keep the file, stop sending it
            f.refused = size;
            f.error   = res->body;
            return std::nullopt;
        default:
            return std::format("close: {}", res->status);
    }
}

// Read before each file and chunk: another client sharing the install may have paused, unlinked, or linked again.
auto Uploader::allowed() -> bool
{
    std::error_code ec;
    if (std::filesystem::exists(options_.config / "paused", ec) || ec)
    {
        return false;
    }
    const auto link = loadLink(options_.config / "token");
    token_          = link ? link->token : std::string{};
    return !token_.empty();
}

auto Uploader::request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    headers.emplace_back("Tus-Resumable", "1.0.0");
    headers.emplace_back("Authorization", "Bearer " + token_);
    auto res = http_->request(method, url, headers, body);
    if (res && res->status == 401)
    {
        // A client that linked again since this token was read saved a new one: only the refused token goes.
        const auto path = options_.config / "token";
        if (const auto link = loadLink(path); link && link->token == token_)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            unauthorized_ = true;
        }
        token_.clear();
        return std::unexpected("Mogsafe refused this install's token");
    }
    return res;
}

// One line per file: name, url, closed, refused, and bytes the server holds, separated by tabs. Lines from before the last field read as nothing sent.
auto Uploader::saved() const -> std::map<std::string, File>
{
    std::map<std::string, File> files;
    std::ifstream               in(options_.dir / STATE_FILE);
    std::string                 line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string        name, url, closed, refused, sent;
        if (std::getline(fields, name, '\t') && std::getline(fields, url, '\t') && std::getline(fields, closed, '\t') && std::getline(fields, refused, '\t'))
        {
            std::getline(fields, sent);
            files[name] = { .url = url, .closed = closed == "1", .refused = parseU64(refused).value_or(0), .sent = parseU64(sent).value_or(0) };
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

auto Uploader::save() -> void
{
    const auto tmp = options_.dir / std::format("{}.tmp", STATE_FILE);
    {
        std::ofstream   out(tmp, std::ios::trunc);
        std::lock_guard lock(mutex_);
        for (const auto& [name, f] : files_)
        {
            out << std::format("{}\t{}\t{}\t{}\t{}\n", name, f.url, f.closed ? 1 : 0, f.refused, f.sent);
        }
        if (!out)
        {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, options_.dir / STATE_FILE, ec);
}

auto Uploader::unauthorized() -> bool
{
    return unauthorized_.exchange(false);
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
        const auto& f    = files[entry.path().filename().string()];
        const auto  size = fromDisk || f.size == 0 ? entry.file_size(ec) : f.size;
        out.push_back({ .name = entry.path().filename().string(), .sent = f.closed ? size : f.sent, .size = size, .refused = !f.closed && f.refused == size && !f.error.empty(), .error = f.error });
    }
    return out;
}

} // namespace memoria
