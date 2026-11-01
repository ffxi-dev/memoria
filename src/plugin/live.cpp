#include "plugin/live.h"

#include "plugin/memoria.h"
#include "plugin/winhttp.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <array>
#include <format>
#include <memory>
#include <utility>

namespace memoria
{
namespace
{

using namespace std::chrono_literals;

constexpr auto TICK        = 100ms; // how long frames wait to go out together
constexpr auto BACKOFF_MAX = 60s;

// Mogsafe's close codes for a sender that must not come back.
constexpr uint16_t REPLACED = 1008; // another client took the link
constexpr uint16_t ENDED    = 4000; // the link's 12 hours ran out

struct Socket
{
    InternetHandle connection;
    InternetHandle ws;
    int            status{};
    std::string    error;
};

auto upgrade(HINTERNET session, const std::string& url, const std::string& token) -> Socket
{
    Socket s;
    auto   open = openRequest(session, "GET", url);
    if (!open)
    {
        s.error = open.error();
        return s;
    }
    s.connection        = std::move(open->connection);
    const auto& req     = open->request;
    const auto  headers = wide("Authorization: Bearer " + token);
    if (!WinHttpSetOption(req.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) || !WinHttpSendRequest(req.get(), headers.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr))
    {
        s.error = std::format("WinHTTP error {}", GetLastError());
        return s;
    }
    DWORD status = 0;
    DWORD len    = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    s.status = static_cast<int>(status);
    if (status != HTTP_STATUS_SWITCH_PROTOCOLS)
    {
        s.error = std::format("Mogsafe answered {}", status);
        return s;
    }
    // A receive waits as long as nobody joins or leaves; Mogsafe's pings and WinHTTP's keepalive hold the line, and stop() closes it.
    DWORD forever = 0;
    WinHttpSetOption(req.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof(forever));
    s.ws.reset(WinHttpWebSocketCompleteUpgrade(req.get(), 0));
    if (!s.ws)
    {
        s.error = std::format("WinHTTP error {}", GetLastError());
        return s;
    }
    WinHttpSetOption(s.ws.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof(forever));
    return s;
}

auto sendBytes(HINTERNET ws, WINHTTP_WEB_SOCKET_BUFFER_TYPE type, std::span<const uint8_t> data) -> bool
{
    return WinHttpWebSocketSend(ws, type, const_cast<uint8_t*>(data.data()), static_cast<DWORD>(data.size())) == NO_ERROR;
}

auto sendText(HINTERNET ws, std::string_view text) -> bool
{
    return sendBytes(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, { reinterpret_cast<const uint8_t*>(text.data()), text.size() });
}

} // namespace

auto browse(const std::string& url) -> void
{
    if (!url.starts_with("https://"))
    {
        return;
    }
    const auto com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ::ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(com))
    {
        ::CoUninitialize();
    }
}

Live::Live(std::string server, std::string installToken, LiveFeed& feed, std::string hello, std::function<void(std::string)> post)
: server_(std::move(server))
, installToken_(std::move(installToken))
, feed_(feed)
, owner_(feed.claim())
, post_(std::move(post))
, hello_(std::move(hello))
, thread_([this](const std::stop_token& stop)
          {
              run(stop);
          })
{
}

Live::~Live()
{
    stop();
}

auto Live::stop() -> void
{
    thread_.request_stop();
    http_.cancel();
}

auto Live::reopen() -> void
{
    {
        std::lock_guard lock(mutex_);
        reopen_ = true;
    }
    wake_.notify_all();
}

auto Live::setHello(std::string hello) -> void
{
    std::lock_guard lock(mutex_);
    hello_ = std::move(hello);
}

auto Live::finished() const -> bool
{
    return finished_;
}

auto Live::status() const -> std::string
{
    std::string text;
    switch (state_)
    {
        case State::Starting:
            text = "connecting";
            break;
        case State::Connected:
            text = viewers_ > 0 ? std::format("{} watching", viewers_.load()) : "waiting for a viewer";
            break;
        case State::Reconnecting:
        {
            std::lock_guard lock(mutex_);
            text = "reconnecting (" + error_ + ")";
            break;
        }
    }
    return dropped_ > 0 ? std::format("{} · {} packets dropped", text, dropped_.load()) : text;
}

auto Live::run(const std::stop_token& stop) -> void
{
    const auto link = startLive(http_, server_, installToken_);
    if (!link || stop.stop_requested())
    {
        if (!link)
        {
            post_(paint(Color::Red, "Cannot go live: " + link.error()));
        }
        finished_ = true;
        return;
    }
    {
        std::lock_guard lock(mutex_);
        url_ = link->url;
    }
    browse(link->url);
    post_(paint(Color::Green, "Live.") + " Anyone with the link can watch until " + paint(Color::Cyan, "/memoria live off") + ". Nothing is sent while nobody watches.");
    post_(link->url);

    const InternetHandle session(WinHttpOpen(L"Memoria/" MEMORIA_VERSION_TEXT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session)
    {
        WinHttpSetTimeouts(session.get(), 10'000, 10'000, 10'000, 10'000);
    }
    const auto url     = server_ + "/v1/live/send?token=" + link->token;
    auto       backoff = std::chrono::milliseconds(1s);
    while (!stop.stop_requested())
    {
        auto s = upgrade(session.get(), url, installToken_);
        if (s.ws)
        {
            state_          = State::Connected;
            backoff         = 1s;
            const auto code = stream(stop, std::move(s.ws));
            if (stop.stop_requested())
            {
                break;
            }
            if (code == REPLACED || code == ENDED)
            {
                post_(code == REPLACED ? paint(Color::Yellow, "Live moved to another client.") : paint(Color::Yellow, "The live link ended.") + " Run " + paint(Color::Cyan, "/memoria live") + " for a new one.");
                break;
            }
            s.error = "Mogsafe closed the connection";
        }
        else if (s.status == 401 || s.status == 404)
        {
            post_(s.status == 401 ? paint(Color::Yellow, "Live off: Vault no longer accepts this install.") + " Run " + paint(Color::Cyan, "/memoria link") + "."
                                  : paint(Color::Yellow, "The live link ended.") + " Run " + paint(Color::Cyan, "/memoria live") + " for a new one.");
            break;
        }
        {
            std::lock_guard lock(mutex_);
            error_ = s.error;
        }
        state_ = State::Reconnecting;
        wait(stop, backoff);
        backoff = (std::min)(backoff * 2, std::chrono::milliseconds(BACKOFF_MAX));
    }
    finished_ = true;
}

// Closes the WebSocket. Returns the code Mogsafe closed with, if it did.
auto Live::stream(const std::stop_token& stop, InternetHandle ws) -> uint16_t
{
    std::string hello;
    {
        std::lock_guard lock(mutex_);
        closed_    = false;
        closeCode_ = 0;
        hello      = hello_;
    }
    std::jthread receiver;
    if (sendText(ws.get(), hello))
    {
        receiver = std::jthread([this, h = ws.get()]
                                {
                                    std::array<char, 4096> buffer{};
                                    std::string            text;
                                    while (true)
                                    {
                                        DWORD                          read = 0;
                                        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                                        if (WinHttpWebSocketReceive(h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type) != NO_ERROR)
                                        {
                                            break;
                                        }
                                        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                                        {
                                            USHORT code = 0;
                                            DWORD  len  = 0;
                                            WinHttpWebSocketQueryCloseStatus(h, &code, buffer.data(), static_cast<DWORD>(buffer.size()), &len);
                                            std::lock_guard lock(mutex_);
                                            closeCode_ = code;
                                            break;
                                        }
                                        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                                        {
                                            text.append(buffer.data(), read);
                                        }
                                        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                                        {
                                            if (const auto n = liveViewers(text))
                                            {
                                                viewers_ = *n;
                                            }
                                            text.clear();
                                        }
                                    }
                                    {
                                        std::lock_guard lock(mutex_);
                                        closed_ = true;
                                    }
                                    wake_.notify_all();
                                });

        bool open = false;
        while (!stop.stop_requested())
        {
            wait(stop, TICK);
            {
                std::lock_guard lock(mutex_);
                if (closed_)
                {
                    break;
                }
            }
            if (const bool watched = viewers_ > 0; watched != open)
            {
                feed_.open(owner_, watched);
                open = watched;
            }
            bool ok = true;
            if (const auto dropped = feed_.takeDropped(owner_); dropped > 0)
            {
                dropped_ += dropped;
                ok = sendText(ws.get(), std::format(R"({{"dropped":{}}})", dropped));
            }
            for (const auto& message : feed_.take(owner_))
            {
                ok = ok && sendBytes(ws.get(), WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, message);
            }
            if (!ok)
            {
                break;
            }
        }
    }
    feed_.open(owner_, false);
    viewers_ = 0;
    if (stop.stop_requested())
    {
        constexpr std::string_view off = "off";
        WinHttpWebSocketShutdown(ws.get(), WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, const_cast<char*>(off.data()), static_cast<DWORD>(off.size()));
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, 3s, [this]
                       {
                           return closed_;
                       });
    }
    ws.reset(); // ends a receive still waiting
    if (receiver.joinable())
    {
        receiver.join();
    }
    std::lock_guard lock(mutex_);
    closed_ = false;
    return closeCode_;
}

// Waits on the live thread, opening the link again when asked; a closed connection ends the wait early.
auto Live::wait(const std::stop_token& stop, std::chrono::milliseconds time) -> void
{
    const auto       until = std::chrono::steady_clock::now() + time;
    std::unique_lock lock(mutex_);
    while (wake_.wait_until(lock, stop, until, [this]
                            {
                                return reopen_ || closed_;
                            }) &&
           reopen_)
    {
        reopen_        = false;
        const auto url = url_;
        lock.unlock();
        browse(url);
        lock.lock();
    }
}

} // namespace memoria
