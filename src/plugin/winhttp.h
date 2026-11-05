#pragma once

#include "core/http.h"

#include <memory>
#include <mutex>
#include <string_view>

namespace memoria::plugin
{

struct CloseInternetHandle
{
    auto operator()(void* handle) const -> void;
};
using InternetHandle = std::unique_ptr<void, CloseInternetHandle>; // HINTERNET

auto wide(std::string_view ascii) -> std::wstring;

// A WinHTTP session that names Memoria and its version as the user agent.
auto openSession() -> InternetHandle;

struct OpenRequest
{
    InternetHandle connection;
    InternetHandle request;
};

// Connects and opens a request for url; the error names the step that failed.
auto openRequest(void* session, std::string_view method, const std::string& url) -> std::expected<OpenRequest, std::string>;

// Requests block their thread, for at most the timeouts set in the constructor or until cancel.
class WinHttp final : public Http
{
public:
    WinHttp();

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override;
    auto cancel() -> void override;

private:
    InternetHandle session_;
    std::mutex     mutex_;
    void*          request_{ nullptr }; // the request in flight, which cancel closes
    bool           cancelled_{ false };
};

} // namespace memoria::plugin
