#include "core/tus.h"

#include "core/link.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <utility>
#include <vector>

namespace memoria
{

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

auto replaceFile(const std::filesystem::path& path, std::string_view text) -> std::optional<std::string>
{
    const auto tmp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
        {
            return std::format("cannot write {}", tmp.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        return std::format("cannot save {}: {}", path.filename().string(), ec.message());
    }
    return std::nullopt;
}

Tus::Tus(std::string server, std::filesystem::path config, Http& http)
: server_(std::move(server))
, config_(std::move(config))
, http_(http)
{
}

auto Tus::allowed() -> bool
{
    std::error_code ec;
    if (std::filesystem::exists(config_ / "paused", ec) || ec)
    {
        return false;
    }
    const auto link = loadLink(config_ / "token");
    token_          = link ? link->token : std::string{};
    return !token_.empty();
}

auto Tus::request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    headers.emplace_back("Tus-Resumable", "1.0.0");
    headers.emplace_back("Authorization", "Bearer " + token_);
    auto res = http_.request(method, onServer(url), headers, body);
    if (res && res->status == 401)
    {
        if (removeLink(config_ / "token", token_))
        {
            unauthorized_ = true;
        }
        token_.clear();
        return std::unexpected("Mogsafe refused this install's token");
    }
    return res;
}

auto Tus::location(const HttpResponse& res) const -> std::string
{
    return onServer(header(res, "location"));
}

// Only the path: behind its tunnel Mogsafe names an http:// Location, and the token goes nowhere but HTTPS to Mogsafe,
// even for a URL saved before this rule.
auto Tus::onServer(std::string_view url) const -> std::string
{
    if (const auto scheme = url.find("://"); scheme != std::string_view::npos)
    {
        url.remove_prefix((std::min)(url.find('/', scheme + 3), url.size()));
    }
    return server_.substr(0, server_.find('/', server_.find("://") + 3)) + std::string(url);
}

auto Tus::append(const std::filesystem::path& path, const std::string& url, uint64_t offset, uint64_t size, const std::function<bool(uint64_t)>& go) -> std::expected<Appended, std::string>
{
    std::ifstream in(path, std::ios::binary);
    while (offset < size && go(offset))
    {
        const auto           n = std::min(size - offset, CHUNK_SIZE);
        std::vector<uint8_t> data(n);
        if (!in.seekg(static_cast<std::streamoff>(offset)) || !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n)))
        {
            return std::unexpected("cannot read " + path.filename().string());
        }
        auto res = request("PATCH", url, { { "Upload-Offset", std::to_string(offset) }, { "Content-Type", "application/offset+octet-stream" } }, data);
        if (!res)
        {
            return std::unexpected(res.error());
        }
        if (res->status != 204)
        {
            return Appended{ .offset = offset, .answer = std::move(*res) };
        }
        offset += n;
    }
    return Appended{ .offset = offset };
}

auto Tus::unauthorized() -> bool
{
    return unauthorized_.exchange(false);
}

} // namespace memoria
