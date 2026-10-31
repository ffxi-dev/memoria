#pragma once

#include "core/http.h"

namespace memoria
{

// Requests block the uploader's thread, for at most the timeouts set in the constructor.
class WinHttp final : public Http
{
public:
    explicit WinHttp(const std::string& agent);
    ~WinHttp() override;

    WinHttp(const WinHttp&)                    = delete;
    auto operator=(const WinHttp&) -> WinHttp& = delete;

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override;

private:
    void* session_{ nullptr }; // HINTERNET
};

} // namespace memoria
