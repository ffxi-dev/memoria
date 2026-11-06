#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace memoria
{

struct HttpResponse
{
    int                                status{};
    std::map<std::string, std::string> headers; // names in lower case
    std::string                        body;
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// For a body that does not match its status.
inline constexpr auto MALFORMED = "Mogsafe sent a malformed answer";

// Empty if missing; name in lower case.
inline auto header(const HttpResponse& res, const std::string& name) -> std::string
{
    const auto it = res.headers.find(name);
    return it == res.headers.end() ? std::string{} : it->second;
}

// Any status but the expected one becomes an error.
inline auto expect(std::expected<HttpResponse, std::string> res, const int status) -> std::expected<HttpResponse, std::string>
{
    if (res && res->status != status)
    {
        return std::unexpected(std::format("Mogsafe answered {}", res->status));
    }
    return res;
}

// Any HTTP status is a response; an error means the transport failed.
class Http
{
public:
    virtual ~Http() = default;

    virtual auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> = 0;

    // From another thread: ends the request in flight and fails later ones.
    virtual auto cancel() -> void
    {
    }
};

} // namespace memoria
