#include "core/tus.h"

#include "core/account/link.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <utility>
#include <vector>

namespace memoria
{

auto parseU64(const std::string_view s) -> std::optional<uint64_t>
{
    uint64_t v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && end == s.data() + s.size() ? std::optional(v) : std::nullopt;
}

Tus::Tus(std::string server, std::filesystem::path config, Http& http)
: server_(std::move(server))
, config_(std::move(config))
, http_(http)
{
}

auto Tus::allowed() -> bool
{
    if (account::paused(config_))
    {
        return false;
    }
    const auto link = account::loadLink(config_ / "token");
    token_          = link ? link->token : std::string{};
    return !token_.empty();
}

auto Tus::request(const std::string& method, const std::string& url, HttpHeaders headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    headers.emplace_back("Tus-Resumable", "1.0.0");
    headers.emplace_back("Authorization", "Bearer " + token_);
    auto res = http_.request(method, onServer(url), headers, body);
    if (res && res->status == 401)
    {
        if (account::removeLink(config_ / "token", token_))
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

// Only the path: behind its tunnel Mogsafe names an http:// Location, and the token goes nowhere but HTTPS to Mogsafe.
auto Tus::onServer(std::string_view url) const -> std::string
{
    if (const auto scheme = url.find("://"); scheme != std::string_view::npos)
    {
        url.remove_prefix((std::min)(url.find('/', scheme + 3), url.size()));
    }
    return server_.substr(0, server_.find('/', server_.find("://") + 3)) + std::string(url);
}

auto Tus::append(const std::filesystem::path& path, const std::string& url, uint64_t offset, const uint64_t size, const std::function<bool(uint64_t)>& go) -> std::expected<Appended, std::string>
{
    std::ifstream        in(path, std::ios::binary);
    std::vector<uint8_t> data;
    while (offset < size && go(offset))
    {
        const auto n = std::min(size - offset, CHUNK_SIZE);
        data.resize(n);
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

auto Tus::token() const -> const std::string&
{
    return token_;
}

} // namespace memoria
