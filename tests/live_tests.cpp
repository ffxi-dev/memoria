#include "support.h"

using namespace memoria;
using namespace memoria::test;

namespace
{

struct Frame
{
    uint8_t              channel{};
    uint16_t             flags{};
    uint64_t             timestamp{};
    std::vector<uint8_t> data;
};

auto frames(const std::vector<std::vector<uint8_t>>& messages) -> std::vector<Frame>
{
    std::vector<Frame> out;
    for (const auto& m : messages)
    {
        for (size_t at = 0; at < m.size();)
        {
            REQUIRE(m[at] == 1);
            Frame    f{ .channel = m[at + 1] };
            uint32_t length{ 0 };
            std::memcpy(&f.flags, &m[at + 2], 2);
            std::memcpy(&f.timestamp, &m[at + 4], 8);
            std::memcpy(&length, &m[at + 12], 4);
            f.data.assign(m.begin() + static_cast<std::ptrdiff_t>(at + 16), m.begin() + static_cast<std::ptrdiff_t>(at + 16 + length));
            out.push_back(std::move(f));
            at += 16 + length;
        }
    }
    return out;
}

} // namespace

TEST_CASE("a live frame is version, channel, flags, time, length, then the packet")
{
    auto e       = gamePacket(0x0102030405060708ULL, 0x015);
    e.direction  = xicap::PacketDirection::ClientToServer;
    e.provenance = xicap::PacketFlag::Injected;
    e.data.resize(6);
    const std::vector<uint8_t> want{
        0x01,
        0x00,
        0x03,
        0x00, // version 1, game channel, from the client and injected
        0x08,
        0x07,
        0x06,
        0x05,
        0x04,
        0x03,
        0x02,
        0x01, // Unix ms
        0x06,
        0x00,
        0x00,
        0x00, // length
        0x15,
        0x08,
        0xAB,
        0xAB,
        0xAB,
        0xAB,
    };
    CHECK(live::frame(outPacket(e), xicap::Chat::Redact) == want);
}

TEST_CASE("live frames hold what the capture holds, so chat goes out zeroed")
{
    const auto dir = tempDir();
    {
        Recorder   rec(options(dir));
        const auto owner = rec.live().claim();
        rec.live().setOpen(owner, true);
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1, 0x017)); // chat to the client
        auto said      = gamePacket(T0 + 2, 0x0B5);
        said.direction = xicap::PacketDirection::ClientToServer;
        rec.packet(std::move(said));
        rec.packet(gamePacket(T0 + 3, 0x015));
        rec.packet({ .timestamp = T0 + 4, .direction = xicap::PacketDirection::ClientToServer, .channel = xicap::Channel::Search, .opcode = 0x05, .data = std::vector<uint8_t>(40, 0xCD) });
        rec.stop();

        const auto live = frames(rec.live().take(owner));
        REQUIRE(live.size() == 4);
        CHECK(live[0].flags == xicap::PacketFlag::Redacted);
        CHECK(live[1].flags == (xicap::PacketFlag::Redacted | xicap::PacketFlag::ClientToServer));
        CHECK(live[1].data.size() == 16);
        CHECK(std::ranges::count(live[1].data, 0) == 10); // the text, from 0x06
        CHECK(live[2].flags == 0);
        CHECK(live[3].channel == 1);

        xicap::Reader r(captures(dir)[0]);
        xicap::Packet p;
        for (const auto& f : live)
        {
            REQUIRE(r.next(p));
            CHECK(f.channel == std::to_underlying(p.channel));
            CHECK(f.flags == p.flags);
            CHECK(f.timestamp == p.timestamp);
            CHECK(std::ranges::equal(f.data, p.data));
        }
    }
    fs::remove_all(dir);
}

TEST_CASE("the live feed batches frames into messages and takes nothing while closed")
{
    live::Feed feed;
    const auto owner = feed.claim();
    feed.push(outPacket(gamePacket(T0)), xicap::Chat::Redact);
    CHECK(feed.take(owner).empty());

    feed.setOpen(owner, true);
    for (int i = 0; i < 3; ++i)
    {
        feed.push(outPacket(gamePacket(T0 + i)), xicap::Chat::Redact);
    }
    auto messages = feed.take(owner);
    REQUIRE(messages.size() == 1);
    CHECK(frames(messages).size() == 3);
    CHECK(feed.take(owner).empty());

    // 5,000 frames of 32 bytes do not fit one 64 KiB message.
    for (int i = 0; i < 5000; ++i)
    {
        feed.push(outPacket(gamePacket(T0 + i)), xicap::Chat::Redact);
    }
    messages = feed.take(owner);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].size() <= live::Feed::MESSAGE_BYTES);
    CHECK(frames(messages).size() == 5000);

    feed.push(outPacket(gamePacket(T0)), xicap::Chat::Redact);
    feed.setOpen(owner, false);
    CHECK(feed.take(owner).empty());
}

TEST_CASE("a full live feed drops packets and counts them")
{
    live::Feed feed(size_t{ 3 } * 32);
    const auto owner = feed.claim();
    feed.setOpen(owner, true);
    for (int i = 0; i < 5; ++i)
    {
        feed.push(outPacket(gamePacket(T0 + i)), xicap::Chat::Redact);
    }
    CHECK(feed.takeDropped(owner) == 2);
    CHECK(feed.takeDropped(owner) == 0);
    CHECK(frames(feed.take(owner)).size() == 3);
    feed.push(outPacket(gamePacket(T0)), xicap::Chat::Redact);
    CHECK(feed.takeDropped(owner) == 0);

    const std::vector<uint8_t> stub(2); // a game packet too short to frame
    feed.push({ .timestamp = T0, .direction = xicap::PacketDirection::ServerToClient, .data = stub }, xicap::Chat::Redact);
    CHECK(feed.takeDropped(owner) == 1);
}

TEST_CASE("a live link stopped late cannot close the feed of the one that replaced it")
{
    live::Feed feed;
    const auto old = feed.claim();
    feed.setOpen(old, true);
    const auto now = feed.claim(); // /memoria live off, then /memoria live, while the old send still hangs
    feed.setOpen(now, true);
    feed.setOpen(old, false); // the old link's send returns, and it closes what it thinks is its feed
    feed.push(outPacket(gamePacket(T0)), xicap::Chat::Redact);
    CHECK(feed.take(old).empty());
    CHECK(frames(feed.take(now)).size() == 1);
}

TEST_CASE("a malformed answer from Mogsafe is an error, not an exception that ends the game")
{
    // Link and live run on threads of their own: an exception that escapes one ends the client.
    class Canned final : public Http
    {
    public:
        HttpResponse answer;

        auto request(const std::string&, const std::string&, const HttpHeaders&, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            return answer;
        }
    };
    Canned http;
    for (const auto* body : { R"({"code":null})", R"({"code":"A","interval":"5"})", R"({"token":{}})", R"({"token":"t","account":7})", R"({"token":1,"url":"https://x"})", R"({"token":"t","url":[]})" })
    {
        INFO(body);
        for (const int status : { 200, 201 })
        {
            http.answer = { .status = status, .body = body };
            CHECK_NOTHROW(std::ignore = startLink(http, "https://mogsafe.test"));
            CHECK_NOTHROW(std::ignore = pollLink(http, "https://mogsafe.test", "p"));
            CHECK_NOTHROW(std::ignore = live::start(http, "https://mogsafe.test", {}, "t0ken"));
        }
    }
    http.answer = { .status = 201, .body = R"({"code":null})" };
    CHECK(!startLink(http, "https://mogsafe.test").has_value());
}

TEST_CASE("a live link comes from Mogsafe with the install's token")
{
    struct Live final : Http
    {
        std::string auth;
        int         status{ 201 };

        auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            CHECK(method == "POST");
            CHECK(url == "https://mogsafe.test/v1/live");
            auth = headers.at(0).second;
            return HttpResponse{ .status = status, .body = R"({"token":"l1ve","url":"https://lens.example/live#l1ve","expires":1790000000000})" };
        }
    } mogsafe;
    const auto dir = tempDir();
    REQUIRE(!saveLink(dir, { .token = "t0ken", .account = "player" }));
    const auto link = live::start(mogsafe, "https://mogsafe.test", dir, "t0ken");
    REQUIRE(link.has_value());
    CHECK(mogsafe.auth == "Bearer t0ken");
    CHECK(link->token == "l1ve");
    CHECK(link->url == "https://lens.example/live#l1ve");

    mogsafe.status = 500;
    CHECK(live::start(mogsafe, "https://mogsafe.test", dir, "t0ken").error() == "Mogsafe answered 500");
    CHECK(loadLink(dir));
    mogsafe.status = 401; // a refused token is deleted, as uploads do
    CHECK(live::start(mogsafe, "https://mogsafe.test", dir, "t0ken").error() == "Mogsafe no longer accepts this install");
    CHECK(!loadLink(dir));
    fs::remove_all(dir);

    CHECK(live::viewers(R"({"viewers":2})") == 2);
    CHECK(!live::viewers(R"({"sender":true})"));
    CHECK(!live::viewers("not json"));
    CHECK(nlohmann::json::parse(live::hello("Kalnor", "Asura", "30261006_0", "Memoria 0.6.0")) ==
          nlohmann::json::parse(R"({"hello":{"character":"Kalnor","world":"Asura","client_version":"30261006_0","agent":"Memoria 0.6.0"}})"));
}

TEST_CASE("a stop sends nothing more of a batch of frames")
{
    const std::vector<std::vector<uint8_t>> batch{ { 1 }, { 2 }, { 3 } };
    std::stop_source                        stop;
    std::vector<uint8_t>                    sent;
    CHECK(!live::sendAll(batch, stop.get_token(), [&](const std::span<const uint8_t> message)
                         {
                             sent.push_back(message[0]);
                             if (message[0] == 1)
                             {
                                 stop.request_stop(); // /memoria live off, or unload, while the first frame goes out
                             }
                             return true;
                         }));
    CHECK(sent == std::vector<uint8_t>{ 1 });

    sent.clear();
    CHECK(live::sendAll(batch, std::stop_source{}.get_token(), [&](const std::span<const uint8_t> message)
                        {
                            sent.push_back(message[0]);
                            return true;
                        }));
    CHECK(sent == std::vector<uint8_t>{ 1, 2, 3 });
}
