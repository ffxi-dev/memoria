#pragma once

#include <cstdint>
#include <expected>
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

// The one call the uploader needs. The plugin uses WinHTTP; tests use a fake
// Mogsafe. A transport failure is an error; any HTTP status is a response.
class Http
{
public:
    virtual ~Http() = default;

    virtual auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> = 0;
};

} // namespace memoria
