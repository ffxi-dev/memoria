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

// WinHTTP in the plugin, a fake Mogsafe in the tests. Any HTTP status is a response; only a transport failure is an error.
class Http
{
public:
    virtual ~Http() = default;

    virtual auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> = 0;
};

} // namespace memoria
