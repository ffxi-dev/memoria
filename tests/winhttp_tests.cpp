// WinHTTP against a server on loopback: a request in flight ends at its next step once cancelled, and every thread that
// owns one joins within the step's timeout. Windows only.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <Windows.h>
#include <bcrypt.h>

#include "core/worker.h"
#include "plugin/live.h"
#include "plugin/winhttp.h"
#include "support.h"

#include <future>

using namespace memoria;
using namespace memoria::test;
using namespace std::chrono_literals;

namespace
{

// WinHTTP rounds a request's timeouts up to whole 4 s ticks (measured: 300 ms and 3 s both wait 4 s, 6 s waits 8 s), so a
// request that times out here does so after 4 s.
constexpr plugin::HttpTimeouts SECOND{ .resolve = 1s, .connect = 1s, .send = 1s, .receive = 1s };
constexpr auto                 TIMED_OUT = 6s; // one 4 s tick, and slack

using Serve = std::function<void(SOCKET peer, const std::string& request, const std::atomic<bool>& done)>;

// Takes connections on a loopback port and hands each, its request read, to serve on a thread of its own.
class Server
{
public:
    explicit Server(Serve serve)
    : serve_(std::move(serve))
    {
        WSADATA wsa{};
        REQUIRE(::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{ .sin_family = AF_INET };
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        int length{ sizeof(addr) };
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ::ntohs(addr.sin_port);
        ::listen(listener_, 4);
        thread_ = std::thread([this]
                              {
                                  while (true)
                                  {
                                      const SOCKET peer{ ::accept(listener_, nullptr, nullptr) };
                                      if (peer == INVALID_SOCKET)
                                      {
                                          return;
                                      }
                                      ++connections;
                                      peers_.emplace_back([this, peer]
                                                          {
                                                              std::string            request;
                                                              std::array<char, 4096> buffer{};
                                                              while (request.find("\r\n\r\n") == std::string::npos)
                                                              {
                                                                  const int n{ ::recv(peer, buffer.data(), static_cast<int>(buffer.size()), 0) };
                                                                  if (n <= 0)
                                                                  {
                                                                      break;
                                                                  }
                                                                  request.append(buffer.data(), static_cast<size_t>(n));
                                                              }
                                                              ++requests;
                                                              serve_(peer, request, done);
                                                              ::closesocket(peer);
                                                          });
                                  }
                              });
    }

    ~Server()
    {
        done = true;
        ::closesocket(listener_);
        thread_.join();
        for (auto& peer : peers_)
        {
            peer.join();
        }
        ::WSACleanup();
    }

    Server(const Server&)                    = delete;
    auto operator=(const Server&) -> Server& = delete;

    [[nodiscard]] auto url() const -> std::string
    {
        return std::format("http://127.0.0.1:{}", port_);
    }

    auto waitForRequest() const -> bool
    {
        return waitFor([this]
                       {
                           return requests > 0;
                       });
    }

    std::atomic<int>  connections{ 0 };
    std::atomic<int>  requests{ 0 };
    std::atomic<bool> done{ false };

private:
    Serve                    serve_;
    SOCKET                   listener_{ INVALID_SOCKET };
    uint16_t                 port_{ 0 };
    std::thread              thread_;
    std::vector<std::thread> peers_;
};

// Answers nothing until the test ends.
auto stall(SOCKET, const std::string&, const std::atomic<bool>& done) -> void
{
    while (!done)
    {
        std::this_thread::sleep_for(10ms);
    }
}

// Sends the head, then the body one byte every gap.
auto trickle(const std::string& head, const std::string& body, const std::chrono::milliseconds gap) -> Serve
{
    return [=](const SOCKET peer, const std::string&, const std::atomic<bool>& done)
    {
        ::send(peer, head.data(), static_cast<int>(head.size()), 0);
        for (const char c : body)
        {
            std::this_thread::sleep_for(gap);
            if (done || ::send(peer, &c, 1, 0) <= 0)
            {
                return;
            }
        }
    };
}

auto receiveAll(const SOCKET peer, const size_t n) -> std::optional<std::string>
{
    std::string out(n, '\0');
    for (size_t at = 0; at < n;)
    {
        const int got{ ::recv(peer, out.data() + at, static_cast<int>(n - at), 0) };
        if (got <= 0)
        {
            return std::nullopt;
        }
        at += static_cast<size_t>(got);
    }
    return out;
}

struct Frame
{
    int         opcode{};
    std::string payload;
};

// One frame from the client, which masks every frame it sends.
auto receiveFrame(const SOCKET peer) -> std::optional<Frame>
{
    const auto head = receiveAll(peer, 2);
    if (!head)
    {
        return std::nullopt;
    }
    uint64_t length{ static_cast<uint8_t>((*head)[1]) & 0x7FU };
    if (length >= 126)
    {
        const auto more = receiveAll(peer, length == 126 ? 2 : 8);
        if (!more)
        {
            return std::nullopt;
        }
        length = 0;
        for (const char c : *more)
        {
            length = length << 8 | static_cast<uint8_t>(c);
        }
    }
    const auto mask    = receiveAll(peer, 4);
    auto       payload = receiveAll(peer, static_cast<size_t>(length));
    if (!mask || !payload)
    {
        return std::nullopt;
    }
    for (size_t i = 0; i < payload->size(); ++i)
    {
        (*payload)[i] = static_cast<char>((*payload)[i] ^ (*mask)[i % 4]);
    }
    return Frame{ .opcode = static_cast<uint8_t>((*head)[0]) & 0x0F, .payload = std::move(*payload) };
}

auto base64(const std::span<const uint8_t> in) -> std::string
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        const auto n = std::min<size_t>(3, in.size() - i);
        uint32_t   v{ static_cast<uint32_t>(in[i]) << 16 };
        v |= n > 1 ? static_cast<uint32_t>(in[i + 1]) << 8 : 0;
        v |= n > 2 ? static_cast<uint32_t>(in[i + 2]) : 0;
        for (size_t k = 0; k < 4; ++k)
        {
            out += k <= n ? digits[(v >> (18 - 6 * k)) & 63] : '=';
        }
    }
    return out;
}

// RFC 6455's Sec-WebSocket-Accept for the key in the client's upgrade request.
auto acceptFor(const std::string& request) -> std::string
{
    constexpr std::string_view name = "Sec-WebSocket-Key: ";
    const auto                 at   = request.find(name) + name.size();
    const auto                 text = request.substr(at, request.find("\r\n", at) - at) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<uint8_t, 20>    digest{};
    ::BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), digest.data(), static_cast<ULONG>(digest.size()));
    return base64(digest);
}

auto since(const std::chrono::steady_clock::time_point start) -> std::chrono::milliseconds
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace

TEST_CASE("a cancelled request stops reading a slow body at its next read")
{
    const std::string body(100, 'x');
    Server            server(trickle(std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n", body.size()), body, 200ms));
    plugin::WinHttp   http(SECOND);
    auto              answer = std::async(std::launch::async, [&]
                                          {
                                 return http.request("GET", server.url() + "/", {}, {});
                                          });
    REQUIRE(server.waitForRequest());
    std::this_thread::sleep_for(300ms);
    const auto start = std::chrono::steady_clock::now();
    http.cancel();
    const auto res = answer.get();
    CHECK(since(start) < 1s); // the next byte, not the 20 s the body takes
    REQUIRE(!res);
    CHECK(res.error() == "stopped");
}

TEST_CASE("a cancelled request waiting for its answer ends at its timeout, and none goes after it")
{
    Server          server([](SOCKET, const std::string&, const std::atomic<bool>&)
                           {
                           });
    plugin::WinHttp http(SECOND);
    Server          stalled(stall);
    auto            answer = std::async(std::launch::async, [&]
                                        {
                                 return http.request("GET", stalled.url() + "/", {}, {});
                                        });
    REQUIRE(stalled.waitForRequest());
    const auto start = std::chrono::steady_clock::now();
    http.cancel();
    CHECK(!answer.get());
    CHECK(since(start) < TIMED_OUT);
    CHECK(!http.request("GET", server.url() + "/", {}, {}));
    CHECK(server.connections == 0); // refused before it connects
}

TEST_CASE("a live link stopped during a slow start answer ends within its timeout")
{
    Server     server(trickle("HTTP/1.1 201 Created\r\nContent-Length: 12\r\nConnection: close\r\n\r\n{", std::string(10, ' ') + "}", 450ms));
    const auto dir = tempDir();
    live::Feed feed;
    const auto quiet = [](const std::string&)
    {
    };
    auto link = std::make_unique<plugin::Live>(plugin::LiveOptions{ .server = server.url(), .config = dir, .installToken = "t0ken", .hello = "{}", .post = quiet, .open = quiet }, feed);
    REQUIRE(server.waitForRequest());
    const auto start = std::chrono::steady_clock::now();
    link.reset();
    CHECK(since(start) < 5s); // its 4 s timeout, and slack; the answer itself takes 5 s
    fs::remove_all(dir);
}

TEST_CASE("an uploader, an importer and the worker each join within their requests' timeout")
{
    Server     server(stall);
    const auto dir = tempDir();
    REQUIRE(!saveLink(dir / "config", { .token = "t0ken", .account = "player" }));

    SECTION("the uploader")
    {
        append(dir / "a.xicap", std::string(500, 'x'));
        auto up = std::make_unique<Uploader>(UploadOptions{ .dir = dir, .server = server.url(), .config = dir / "config" }, std::make_unique<plugin::WinHttp>(SECOND));
        up->start();
        REQUIRE(server.waitForRequest());
        const auto start = std::chrono::steady_clock::now();
        up.reset();
        CHECK(since(start) < TIMED_OUT);
    }
    SECTION("the importer")
    {
        const auto folder = dir / "captures" / "s" / "Hoa";
        fs::create_directories(folder / "PacketLogger");
        std::ofstream(folder / "manifest.txt") << "StartTime = 2026-08-04 23:54:00\n";
        std::ofstream(folder / "PacketLogger" / "full.log") << "log";
        fs::last_write_time(folder / "PacketLogger" / "full.log", fs::file_time_type::clock::now() - std::chrono::minutes(20));
        auto importer = std::make_unique<Importer>(ImportOptions{ .captures = dir / "captures", .uploaded = dir / "uploaded", .state = dir / "state", .server = server.url(), .config = dir / "config" },
                                                   std::make_unique<plugin::WinHttp>(SECOND));
        importer->start();
        REQUIRE(server.waitForRequest());
        const auto start = std::chrono::steady_clock::now();
        importer.reset();
        CHECK(since(start) < TIMED_OUT);
    }
    SECTION("the worker")
    {
        auto worker = std::make_unique<Worker>();
        worker->post([&](const std::stop_token& stop)
                     {
                         plugin::WinHttp          http(SECOND);
                         const std::stop_callback cancel(stop, [&]
                                                         {
                                                             http.cancel();
                                                         });
                         std::ignore = http.request("GET", server.url() + "/", {}, {});
                     });
        REQUIRE(server.waitForRequest());
        const auto start = std::chrono::steady_clock::now();
        worker.reset();
        CHECK(since(start) < TIMED_OUT);
    }
    fs::remove_all(dir);
}

TEST_CASE("a live link upgrades, streams, stays connected while nobody watches, then closes and joins")
{
    std::atomic<int>  upgrades{ 0 };
    std::atomic<bool> hello{ false };
    std::atomic<int>  closeCode{ 0 };
    Server            server([&](const SOCKET peer, const std::string& request, const std::atomic<bool>&)
                             {
                      if (request.starts_with("POST /v1/live "))
                      {
                          const std::string body = R"({"token":"l1ve","url":"https://lens.test/live","expires":1790000000000})";
                          const auto        res  = std::format("HTTP/1.1 201 Created\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
                          ::send(peer, res.data(), static_cast<int>(res.size()), 0);
                          return;
                      }
                      ++upgrades;
                      const auto res = std::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n", acceptFor(request));
                      ::send(peer, res.data(), static_cast<int>(res.size()), 0);
                      while (const auto frame = receiveFrame(peer))
                      {
                          if (frame->opcode == 1 && frame->payload == "{}")
                          {
                              hello = true;
                          }
                          if (frame->opcode == 8) // the close: answer it, as Mogsafe does
                          {
                              closeCode = frame->payload.size() >= 2 ? static_cast<uint8_t>(frame->payload[0]) << 8 | static_cast<uint8_t>(frame->payload[1]) : 0;
                              const std::array<char, 4> reply{ static_cast<char>(0x88), 2, frame->payload.size() >= 2 ? frame->payload[0] : '\0', frame->payload.size() >= 2 ? frame->payload[1] : '\0' };
                              ::send(peer, reply.data(), static_cast<int>(reply.size()), 0);
                              return;
                          }
                      }
                             });
    const auto        dir = tempDir();
    REQUIRE(!saveLink(dir, { .token = "t0ken", .account = "player" })); // the link lives while the install shares this token
    live::Feed               feed;
    std::mutex               mutex;
    std::vector<std::string> said;
    auto                     link    = std::make_unique<plugin::Live>(plugin::LiveOptions{
                                                                          .server       = server.url(),
                                                                          .config       = dir,
                                                                          .installToken = "t0ken",
                                                                          .hello        = "{}",
                                                                          .post         = [&](std::string text)
                                                                          {
                                                       const std::scoped_lock lock(mutex);
                                                       said.push_back(std::move(text));
                                                                          },
                                                                          .open = [](const std::string&)
                                                                          {
                                                                          }, // opens no browser
                                                                      },
                                                                      feed);
    const bool               greeted = waitFor([&]
                                               {
                                     return hello.load();
                                               });
    {
        const std::scoped_lock lock(mutex);
        for (const auto& text : said)
        {
            UNSCOPED_INFO(text);
        }
        UNSCOPED_INFO("upgrades " << upgrades << ", status " << link->status());
        REQUIRE(greeted);
    }
    CHECK(link->status() == "waiting for a viewer");
    std::this_thread::sleep_for(TIMED_OUT); // longer than any of its timeouts: a receive that timed out would reconnect
    CHECK(link->status() == "waiting for a viewer");
    CHECK(upgrades == 1);
    const auto start = std::chrono::steady_clock::now();
    link.reset();
    CHECK(since(start) < 5s);
    CHECK(closeCode == 1001); // unload: going away, which leaves the link to expire
    fs::remove_all(dir);
}

#endif
