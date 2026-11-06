#pragma once

#include "core/live/live.h"
#include "plugin/enums/live_state.h"
#include "plugin/winhttp.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace memoria::plugin
{

struct LiveOptions
{
    std::string                      server;
    std::filesystem::path            config; // a pause or unlink by any client sharing the install ends the link
    std::string                      installToken;
    std::string                      hello;
    std::function<void(std::string)> post; // a chat line, from the live thread
    std::function<void(std::string)> open; // shows the link's page; the session runs it on its worker, since a browser can take seconds to start
};

struct Socket
{
    InternetHandle connection;
    InternetHandle ws;
    int            status{};
    std::string    error;
};

// Asks Mogsafe for a link, opens it, and streams the feed over a WebSocket while anyone watches, reconnecting until
// stopped.
class Live
{
public:
    Live(LiveOptions options, live::Feed& feed);
    ~Live(); // aborts, then joins: the game waits for the step in flight to finish or time out, then the WebSocket close

    Live(const Live&)                    = delete;
    Live(Live&&)                         = delete;
    auto operator=(const Live&) -> Live& = delete;
    auto operator=(Live&&) -> Live&      = delete;

    auto stop() -> void;  // closes with 1000 "off", which ends the link; with the socket down, the link expires
    auto abort() -> void; // at unload: as stop, without "off"
    auto reopen() -> void;
    auto setHello(std::string hello) -> void; // sent on each connect, and at once while connected

    [[nodiscard]] auto finished() const -> bool;
    [[nodiscard]] auto status() const -> std::string;

private:
    auto run(const std::stop_token& stop) -> void;
    auto upgrade(void* session, const std::string& url, const std::string& headers, const std::stop_token& stop) -> Socket;
    auto stream(const std::stop_token& stop, InternetHandle ws) -> uint16_t;
    auto receive(void* ws) -> void;
    auto wait(const std::stop_token& stop, std::chrono::milliseconds time) -> void;
    auto helloNow() const -> std::string;

    std::string                      server_;
    std::filesystem::path            config_;
    std::string                      installToken_;
    live::Feed&                      feed_;
    uint64_t                         owner_; // the id the feed gave this link
    std::function<void(std::string)> post_;
    std::function<void(std::string)> open_;

    mutable std::mutex          mutex_;
    std::condition_variable_any wake_;
    std::string                 hello_;
    std::string                 url_;
    std::string                 error_; // why the last connection failed
    bool                        reopen_{ false };
    bool                        closed_{ false }; // the server ended the connection
    uint16_t                    closeCode_{ 0 };

    std::atomic<LiveState> state_{ LiveState::Starting };
    std::atomic<int>       viewers_{ 0 };
    std::atomic<uint64_t>  dropped_{ 0 };
    std::atomic<bool>      finished_{ false };
    std::atomic<bool>      aborted_{ false };
    WinHttp                http_{ QUICK_TIMEOUTS }; // also bounds each socket step: nothing cancels a call from another thread, so unload waits for the step in flight
    std::jthread           thread_;                 // last: it starts using the members above
};

} // namespace memoria::plugin
