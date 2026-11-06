#pragma once

#include "core/http.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace memoria::plugin
{

struct CloseInternetHandle
{
    auto operator()(void* handle) const -> void;
};
using InternetHandle = std::unique_ptr<void, CloseInternetHandle>; // HINTERNET

auto wide(std::string_view ascii) -> std::wstring;

// Blocks while Windows starts the browser: call it off the game thread.
auto browse(const std::string& url) -> void;

auto openSession() -> InternetHandle;

struct OpenRequest
{
    InternetHandle connection;
    InternetHandle request;
};

// The error names the step that failed.
auto openRequest(void* session, std::string_view method, const std::string& url) -> std::expected<OpenRequest, std::string>;

// WinHTTP's limits for each step of a request.
struct HttpTimeouts
{
    std::chrono::milliseconds resolve;
    std::chrono::milliseconds connect;
    std::chrono::milliseconds send; // each write of the body, at most WRITE_SIZE bytes
    std::chrono::milliseconds receive;
};

inline constexpr size_t WRITE_SIZE = size_t{ 64 } * 1024;

// A 64 KiB write takes about 4 s at 128 kbit/s, the slowest line a player records on; 32 s leaves room for congestion.
// Mogsafe answers a close after it has hashed and stored the whole capture.
inline constexpr HttpTimeouts UPLOAD_TIMEOUTS{ .resolve = std::chrono::seconds(12), .connect = std::chrono::seconds(12), .send = std::chrono::seconds(32), .receive = std::chrono::seconds(60) };

// Linking, the chat choice and live: a few hundred bytes each way. WinHTTP counts timeouts in whole 4 s steps.
inline constexpr HttpTimeouts QUICK_TIMEOUTS{ .resolve = std::chrono::seconds(4), .connect = std::chrono::seconds(4), .send = std::chrono::seconds(4), .receive = std::chrono::seconds(4) };

// False if WinHTTP refused them, which leaves requests unbounded.
auto setTimeouts(void* session, const HttpTimeouts& timeouts) -> bool;

// cancel sets a flag: the request in flight stops at its next step, write or read, and its own thread closes its
// handles.
class WinHttp final : public Http
{
public:
    explicit WinHttp(const HttpTimeouts& timeouts);

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override;
    auto cancel() -> void override;

private:
    InternetHandle    session_;
    std::atomic<bool> cancelled_{ false };
};

} // namespace memoria::plugin
