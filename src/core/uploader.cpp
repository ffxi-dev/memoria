#include "core/uploader.h"

#include "core/sha256.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

namespace memoria
{
namespace
{

constexpr uint64_t HEADER_SIZE = 384;     // xicap format 3; Mogsafe refuses a first append without it
constexpr uint64_t CHUNK_SIZE  = 1 << 20; // one request stays short on a slow uplink
constexpr auto     STATE_FILE  = "uploads.txt";
constexpr auto     MAX_WAIT    = std::chrono::minutes(5);

auto parseU64(std::string_view s) -> std::optional<uint64_t>
{
    uint64_t v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size())
    {
        return std::nullopt;
    }
    return v;
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
    thread_ = std::thread([this]
                          {
                              auto wait = options_.every;
                              while (true)
                              {
                                  bool failed = true;
                                  try
                                  {
                                      failed = (!options_.mayRun || options_.mayRun()) && pass().has_value();
                                  }
                                  catch (const std::exception&) // out of memory, or the folder went away
                                  {
                                  }
                                  // Mogsafe down: wait twice as long after each failed pass, up to five minutes.
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
                          });
}

auto Uploader::pass() -> std::optional<std::string>
{
    if (!loaded_)
    {
        load();
        loaded_ = true;
    }
    std::optional<std::string> first;
    std::error_code            ec;
    for (const auto& entry : std::filesystem::directory_iterator(options_.dir, ec))
    {
        if (stopping_)
        {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".xicap")
        {
            continue;
        }
        File f;
        {
            std::lock_guard lock(mutex_);
            f = files_[entry.path().filename().string()];
        }
        const auto error = step(entry.path(), f);
        if (error)
        {
            f.error = *error;
        }
        else if (f.refused != f.size)
        {
            f.error.clear(); // a refusal stays until the file changes
        }
        {
            std::lock_guard lock(mutex_);
            files_[entry.path().filename().string()] = f;
        }
        if (error && !first)
        {
            first = entry.path().filename().string() + ": " + *error;
        }
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
    if (size < HEADER_SIZE)
    {
        return std::nullopt; // the writer has not written the header yet
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
        if (f.url.starts_with("/"))
        {
            // A path only: resolve it against the server.
            const auto host = options_.server.find('/', options_.server.find("://") + 3);
            f.url           = options_.server.substr(0, host) + f.url;
        }
    }

    const auto res = request("HEAD", f.url, {});
    if (!res)
    {
        return res.error();
    }
    if (res->status == 404)
    {
        // Archived after a close whose answer was lost, or gone: a close with
        // the same hash says which.
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
    while (offset < size && !stopping_)
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
            f.url.clear(); // cut off or closed on the server: send the whole file as a new capture
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

// Sends the file's hash. Mogsafe refuses while the capture has no footer; the
// uploader then waits for the file to change.
auto Uploader::close(const std::filesystem::path& path, File& f, uint64_t size) -> std::optional<std::string>
{
    const auto sum = fileSha256(path, size);
    if (!sum)
    {
        return "cannot read the capture";
    }
    const std::string body = std::format(R"({{"sha256":"{}"}})", *sum);
    const auto        res  = request("POST", f.url + "/close", { { "Content-Type", "application/json" } }, { reinterpret_cast<const uint8_t*>(body.data()), body.size() });
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
        case 409: // not finished yet, or bytes missing
            f.refused = size;
            return std::nullopt;
        case 422: // the archive refuses this capture: keep the file, stop sending it
            f.refused = size;
            f.error   = res->body;
            return std::nullopt;
        default:
            return std::format("close: {}", res->status); // transient: try again on the next pass
    }
}

auto Uploader::request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    headers.emplace_back("Tus-Resumable", "1.0.0");
    headers.insert(headers.end(), options_.credentials.begin(), options_.credentials.end());
    return http_->request(method, url, headers, body);
}

// uploads.txt: one line per file, "<name>\t<url>\t<closed>\t<refused>".
auto Uploader::load() -> void
{
    std::ifstream in(options_.dir / STATE_FILE);
    std::string   line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string        name, url, closed, refused;
        if (std::getline(fields, name, '\t') && std::getline(fields, url, '\t') && std::getline(fields, closed, '\t') && std::getline(fields, refused))
        {
            std::lock_guard lock(mutex_);
            auto&           f = files_[name];
            f.url             = url;
            f.closed          = closed == "1";
            f.refused         = parseU64(refused).value_or(0);
        }
    }
}

auto Uploader::save() -> void
{
    const auto tmp = options_.dir / (std::string(STATE_FILE) + ".tmp");
    {
        std::ofstream   out(tmp, std::ios::trunc);
        std::lock_guard lock(mutex_);
        for (const auto& [name, f] : files_)
        {
            out << name << '\t' << f.url << '\t' << (f.closed ? 1 : 0) << '\t' << f.refused << '\n';
        }
        if (!out)
        {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, options_.dir / STATE_FILE, ec);
}

auto Uploader::status() const -> std::vector<UploadStatus>
{
    std::lock_guard           lock(mutex_);
    std::vector<UploadStatus> out;
    for (const auto& [name, f] : files_)
    {
        out.push_back({ .name = name, .sent = f.sent, .size = f.size, .closed = f.closed, .error = f.error });
    }
    return out;
}

} // namespace memoria
