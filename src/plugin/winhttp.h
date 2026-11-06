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

// Opens an https URL in the default browser. Blocks while Windows starts it, so call it off the game thread.
auto browse(const std::string& url) -> void;

// A WinHTTP session that names Memoria and its version as the user agent.
auto openSession() -> InternetHandle;

struct OpenRequest
{
    InternetHandle connection;
    InternetHandle request;
};

// Connects and opens a request for url; the error names the step that failed.
auto openRequest(void* session, std::string_view method, const std::string& url) -> std::expected<OpenRequest, std::string>;

// How long each step of a request may wait on the network: WinHTTP's own limits, which nothing cuts short.
struct HttpTimeouts
{
    std::chrono::milliseconds resolve;
    std::chrono::milliseconds connect;
    std::chrono::milliseconds send; // each write of the body, at most WRITE_SIZE bytes
    std::chrono::milliseconds receive;
};

inline constexpr size_t WRITE_SIZE = size_t{ 64 } * 1024;

// Uploads. A 64 KiB write takes about 4 s at 128 kbit/s, the slowest line a player records on, so 32 s
// leaves room for a congested one; Mogsafe answers a close only once it has hashed and stored the whole capture.
inline constexpr HttpTimeouts UPLOAD_TIMEOUTS{ .resolve = std::chrono::seconds(12), .connect = std::chrono::seconds(12), .send = std::chrono::seconds(32), .receive = std::chrono::seconds(60) };

// Linking and the chat choice: a few hundred bytes each way, which Mogsafe answers at once. 4 s is WinHTTP's tick: it
// counts a request's timeouts in whole 4 s steps.
inline constexpr HttpTimeouts QUICK_TIMEOUTS{ .resolve = std::chrono::seconds(4), .connect = std::chrono::seconds(4), .send = std::chrono::seconds(4), .receive = std::chrono::seconds(4) };

// Sets a session's timeouts; false if WinHTTP refused them, which leaves no bound to rely on.
auto setTimeouts(void* session, const HttpTimeouts& timeouts) -> bool;

// Requests block their thread for at most one step's timeout at a time. cancel only signals: the request in flight
// stops at its next step, between writes, or between reads of the answer, and its own thread closes its handles.
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
