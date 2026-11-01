#pragma once

#include "core/live.h"
#include "plugin/winhttp.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace memoria
{

// Opens an https URL in the default browser. Blocks while Windows starts it, so call it off the game thread.
auto browse(const std::string& url) -> void;

// /memoria live: asks Mogsafe for a link, opens it, and streams the feed over a WinHTTP WebSocket while anyone watches, reconnecting until stopped.
class Live
{
public:
    Live(std::string server, std::string installToken, LiveFeed& feed, std::string hello, std::function<void(std::string)> post);
    ~Live(); // joins: stop() first to end the link without waiting

    Live(const Live&)                    = delete;
    auto operator=(const Live&) -> Live& = delete;

    auto stop() -> void; // closes with 1000 "off", which ends the link, reconnecting once to send it if the socket is down
    auto reopen() -> void;
    auto setHello(std::string hello) -> void; // sent on each connect, and at once while connected

    [[nodiscard]] auto finished() const -> bool;
    [[nodiscard]] auto status() const -> std::string;

private:
    enum class State : uint8_t
    {
        Starting,
        Connected,
        Reconnecting,
    };

    auto run(const std::stop_token& stop) -> void;
    auto stream(const std::stop_token& stop, InternetHandle ws) -> uint16_t;
    auto wait(const std::stop_token& stop, std::chrono::milliseconds time) -> void;
    auto helloNow() const -> std::string;

    std::string                      server_;
    std::string                      installToken_;
    LiveFeed&                        feed_;
    uint64_t                         owner_; // of the feed
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

    std::atomic<State>    state_{ State::Starting };
    std::atomic<int>      viewers_{ 0 };
    std::atomic<uint64_t> dropped_{ 0 };
    std::atomic<bool>     finished_{ false };
    WinHttp               http_{ "Memoria/" MEMORIA_VERSION_TEXT };
    std::jthread          thread_; // last: started once the rest is ready
};

} // namespace memoria
