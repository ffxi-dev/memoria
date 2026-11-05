#include "core/tus.h"

#include "core/install.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <utility>
#include <vector>

namespace memoria
{

Tus::Tus(std::string server, std::filesystem::path config, Http& http)
: server_(std::move(server))
, config_(std::move(config))
, http_(http)
{
}

auto Tus::allowed() -> bool
{
    if (paused(config_))
    {
        return false;
    }
    const auto link = loadLink(config_);
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
        if (removeLink(config_, token_))
        {
            unauthorized_ = true;
        }
        token_.clear();
        return std::unexpected(REFUSED);
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

auto Tus::takeUnauthorized() -> bool
{
    return unauthorized_.exchange(false);
}

auto Tus::token() const -> const std::string&
{
    return token_;
}

} // namespace memoria
