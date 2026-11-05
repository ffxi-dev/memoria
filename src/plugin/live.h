#pragma once

#include "core/live/live.h"
#include "plugin/enums/live_state.h"
#include "plugin/interruptible.h"
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

// Opens an https URL in the default browser. Blocks while Windows starts it, so call it off the game thread.
auto browse(const std::string& url) -> void;

struct Socket
{
    InternetHandle connection;
    InternetHandle ws;
    int            status{};
    std::string    error;
};

// /memoria live: asks Mogsafe for a link, opens it, and streams the feed over a WinHTTP WebSocket while anyone watches, reconnecting until stopped.
class Live
{
public:
    // config holds the token and paused files: a pause or unlink in any client sharing the install ends the link.
    Live(std::string server, std::filesystem::path config, std::string installToken, live::Feed& feed, std::string hello, std::function<void(std::string)> post);
    ~Live(); // aborts, then joins

    Live(const Live&)                    = delete;
    Live(Live&&)                         = delete;
    auto operator=(const Live&) -> Live& = delete;
    auto operator=(Live&&) -> Live&      = delete;

    auto stop() -> void;  // closes with 1000 "off", which ends the link, reconnecting once to send it if the socket is down
    auto abort() -> void; // at unload: closes the connection in flight at once, and sends no "off"
    auto reopen() -> void;
    auto setHello(std::string hello) -> void; // sent on each connect, and at once while connected

    [[nodiscard]] auto finished() const -> bool;
    [[nodiscard]] auto status() const -> std::string;

private:
    auto run(const std::stop_token& stop) -> void;
    auto upgrade(void* session, const std::string& url, const std::string& headers) -> Socket;
    auto sendOff(void* session, const std::string& url, const std::string& headers) -> void;
    auto letGo(InternetHandle& handle) -> void; // a handle abort() closed is released, not closed twice
    auto stream(const std::stop_token& stop, InternetHandle ws) -> uint16_t;
    auto wait(const std::stop_token& stop, std::chrono::milliseconds time) -> void;
    auto helloNow() const -> std::string;

    std::string                      server_;
    std::filesystem::path            config_;
    std::string                      installToken_;
    live::Feed&                      feed_;
    uint64_t                         owner_; // the id the feed gave this link
    std::function<void(std::string)> post_;

    mutable std::mutex          mutex_;
    std::condition_variable_any wake_;
    std::string                 hello_;
    std::string                 url_;
    std::string                 error_; // why the last connection failed
    bool                        reopen_{ false };
    bool                        closed_{ false }; // the server ended the connection
    uint16_t                    closeCode_{ 0 };
    bool                        offSent_{ false }; // live thread only

    std::atomic<LiveState> state_{ LiveState::Starting };
    std::atomic<int>       viewers_{ 0 };
    std::atomic<uint64_t>  dropped_{ 0 };
    std::atomic<bool>      finished_{ false };
    WinHttp                http_;
    Interruptible          calls_;  // every WinHTTP call that waits on the network, so abort() can end it
    std::jthread           thread_; // last: started once the rest is ready
};

} // namespace memoria::plugin
