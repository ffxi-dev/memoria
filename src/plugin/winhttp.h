#pragma once

#include "core/http.h"

#include <memory>
#include <string_view>

namespace memoria
{

struct CloseInternetHandle
{
    auto operator()(void* handle) const -> void;
};
using InternetHandle = std::unique_ptr<void, CloseInternetHandle>; // HINTERNET

auto wide(std::string_view ascii) -> std::wstring;

struct OpenRequest
{
    InternetHandle connection;
    InternetHandle request;
};

// Connects and opens a request for url; the error names the step that failed.
auto openRequest(void* session, std::string_view method, const std::string& url) -> std::expected<OpenRequest, std::string>;

// Requests block the uploader's thread, for at most the timeouts set in the constructor.
class WinHttp final : public Http
{
public:
    explicit WinHttp(const std::string& agent);

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override;

private:
    InternetHandle session_;
};

} // namespace memoria
