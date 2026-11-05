#include "support.h"

using namespace memoria;
using namespace memoria::test;

TEST_CASE("a linked install asks Mogsafe whether its account saves chat")
{
    struct Chat final : Http
    {
        std::string auth;
        int         status{ 200 };
        std::string body{ R"({"save_chat":true})" };

        auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            CHECK(method == "GET");
            CHECK(url == "https://mogsafe.test/v1/install/save-chat");
            auth = headers.at(0).second;
            return HttpResponse{ .status = status, .body = body };
        }
    } mogsafe;
    CHECK(fetchSaveChat(mogsafe, "https://mogsafe.test", "t0ken") == true);
    CHECK(mogsafe.auth == "Bearer t0ken");
    mogsafe.body = R"({"save_chat":false})";
    CHECK(fetchSaveChat(mogsafe, "https://mogsafe.test", "t0ken") == false);
    for (const auto* bad : { R"({"save_chat":"yes"})", R"({})", "not json" })
    {
        mogsafe.body = bad;
        CHECK_FALSE(fetchSaveChat(mogsafe, "https://mogsafe.test", "t0ken"));
    }
    mogsafe.body   = R"({"save_chat":true})";
    mogsafe.status = 401;
    CHECK_FALSE(fetchSaveChat(mogsafe, "https://mogsafe.test", "t0ken"));
}

TEST_CASE("each capture takes the chat choice it opens with")
{
    const auto        dir  = tempDir();
    auto              opts = options(dir);
    std::atomic<bool> saveChat{ false };
    opts.chat = [&]
    {
        return saveChat ? xicap::Chat::Keep : xicap::Chat::Redact;
    };
    const auto said = [](const uint64_t t)
    {
        auto e      = gamePacket(t, 0x0B5); // chat, from the client
        e.direction = xicap::PacketDirection::ClientToServer;
        return e;
    };
    {
        Recorder rec(opts);
        rec.login(T0, who("Kalnor", 1));
        rec.packet(said(T0 + 1));
        REQUIRE(waitFor([&]
                        {
                            return !rec.status().capture.empty();
                        }));
        saveChat = true; // the open capture keeps its choice
        rec.packet(said(T0 + 2));
        rec.login(T0 + 10'000, who("Mirabeth", 2));
        rec.packet(said(T0 + 10'001));
        rec.stop();
    }
    std::set<std::pair<std::string, bool>> redacted;
    for (const auto& path : captures(dir))
    {
        xicap::Reader r(path);
        for (const auto& p : r)
        {
            redacted.emplace(r.metadata().character.name, (p.flags & xicap::PacketFlag::Redacted) != 0);
        }
    }
    CHECK(redacted == std::set<std::pair<std::string, bool>>{ { "Kalnor", true }, { "Mirabeth", false } });
    fs::remove_all(dir);
}

TEST_CASE("a capture that saves chat keeps it whole, in the file and the live frames; otherwise its text is zeroed")
{
    for (const bool saveChat : { false, true })
    {
        const auto dir  = tempDir();
        auto       opts = options(dir);
        opts.chat       = [saveChat]
        {
            return saveChat ? xicap::Chat::Keep : xicap::Chat::Redact;
        };
        {
            Recorder rec(opts);
            rec.login(T0, who("Kalnor", 1));
            auto said      = gamePacket(T0 + 1, 0x0B5); // chat, from the client
            said.direction = xicap::PacketDirection::ClientToServer;
            rec.packet(std::move(said));
            rec.stop();
        }
        const auto zeroed = static_cast<std::ptrdiff_t>(saveChat ? 0 : 10); // the text, from 0x06
        {
            xicap::Reader r(captures(dir)[0]);
            for (const auto& p : r)
            {
                CHECK(std::ranges::count(p.data, uint8_t{ 0 }) == zeroed);
            }
        }
        const auto e     = gamePacket(T0 + 1, 0x0B5);
        const auto frame = live::frame({ .timestamp = e.timestamp, .direction = xicap::PacketDirection::ClientToServer, .data = e.data }, saveChat ? xicap::Chat::Keep : xicap::Chat::Redact);
        CHECK(std::ranges::count(frame | std::views::drop(16), 0) == zeroed);
        fs::remove_all(dir);
    }
}

TEST_CASE("a capture keeps chat only while the token that chose it is the install's")
{
    const auto dir   = tempDir();
    const auto token = dir / "token";
    struct Account final : Http
    {
        std::map<std::string, bool> saveChat; // by token
        std::function<void()>       whileAsked;
        int                         asked{};

        auto request(const std::string&, const std::string&, const HttpHeaders& headers, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            ++asked;
            if (whileAsked)
            {
                std::exchange(whileAsked, {})();
            }
            const auto it = saveChat.find(headers.at(0).second.substr(7)); // "Bearer "
            return HttpResponse{ .status = 200, .body = it != saveChat.end() && it->second ? R"({"save_chat":true})" : R"({"save_chat":false})" };
        }
    } mogsafe;
    mogsafe.saveChat = { { "a", true }, { "b", false } };
    ChatChoice choice;
    CHECK(choice.chat(std::nullopt) == xicap::Chat::Redact);

    saveLink(token, { .token = "a", .account = "one" });
    choice.refresh(mogsafe, "https://mogsafe.test", token);
    CHECK(choice.chat(loadLink(token)) == xicap::Chat::Keep);

    SECTION("an unlink, or a link to another account, redacts before any answer")
    {
        fs::remove(token);
        CHECK(choice.chat(loadLink(token)) == xicap::Chat::Redact);
        saveLink(token, { .token = "b", .account = "two" });
        CHECK(choice.chat(loadLink(token)) == xicap::Chat::Redact);
    }
    SECTION("an answer for a token replaced while it was asked is dropped, and the new token is asked")
    {
        mogsafe.saveChat["a"] = true;
        mogsafe.saveChat["c"] = true;
        mogsafe.whileAsked    = [&]
        {
            saveLink(token, { .token = "b", .account = "two" }); // relinked to an account that cuts chat
        };
        mogsafe.asked = 0;
        choice.refresh(mogsafe, "https://mogsafe.test", token);
        CHECK(mogsafe.asked == 2);
        CHECK(choice.chat(loadLink(token)) == xicap::Chat::Redact);
    }
    SECTION("an answer that comes after an unlink is dropped")
    {
        mogsafe.whileAsked = [&]
        {
            fs::remove(token);
        };
        choice.refresh(mogsafe, "https://mogsafe.test", token);
        saveLink(token, { .token = "a2", .account = "one" });
        CHECK(choice.chat(loadLink(token)) == xicap::Chat::Redact);
    }
    fs::remove_all(dir);
}

TEST_CASE("every chat range is zeroed in the capture, the session file and the live frames, unless the account saves chat")
{
    struct Range
    {
        uint16_t               opcode;
        xicap::PacketDirection direction;
        size_t                 offset;
        size_t                 length; // 0: to the end
    };
    constexpr auto toClient   = xicap::PacketDirection::ServerToClient;
    constexpr auto fromClient = xicap::PacketDirection::ClientToServer;
    const auto     ranges     = std::array{
        Range{ 0x017, toClient, 0x17, 0 },
        Range{ 0x0B5, fromClient, 0x06, 0 },
        Range{ 0x0B6, fromClient, 0x15, 0 },
        Range{ 0x0B7, fromClient, 0x15, 0 },
        Range{ 0x0CC, toClient, 0x08, 128 },
        Range{ 0x0E2, fromClient, 0x10, 128 },
    };
    constexpr size_t SIZE = 256;
    const auto       sent = [&](const Range& r)
    {
        std::vector<uint8_t> data(SIZE, 0xAB);
        const auto           hdr = static_cast<uint16_t>(((SIZE / 4) << 9) | r.opcode);
        data[0]                  = static_cast<uint8_t>(hdr);
        data[1]                  = static_cast<uint8_t>(hdr >> 8);
        data[2]                  = 0;
        data[3]                  = 0;
        return data;
    };
    const auto zeroed = [&](const Range& r, const std::span<const uint8_t> data, const bool kept)
    {
        const auto end = r.length == 0 ? SIZE : r.offset + r.length;
        for (size_t i = 4; i < SIZE; ++i)
        {
            const bool text{ i >= r.offset && i < end };
            if (data[i] != (text && !kept ? 0 : 0xAB))
            {
                return false;
            }
        }
        return data.size() == SIZE;
    };
    for (const bool saveChat : { false, true })
    {
        INFO("saveChat " << saveChat);
        const auto dir  = tempDir();
        auto       opts = options(dir);
        opts.chat       = [saveChat]
        {
            return saveChat ? xicap::Chat::Keep : xicap::Chat::Redact;
        };
        std::vector<std::vector<uint8_t>> live;
        {
            Recorder   rec(opts);
            const auto owner = rec.live().claim();
            rec.live().open(owner, true);
            rec.login(T0, who("Kalnor", 1));
            rec.sessionStart(T0, dir / "sessions" / "s.xicap");
            for (size_t i = 0; i < ranges.size(); ++i)
            {
                rec.packet({ .timestamp = T0 + 1 + i, .direction = ranges[i].direction, .data = sent(ranges[i]) });
            }
            rec.sessionStop(T0 + 100);
            rec.stop();
            live = rec.live().take(owner);
        }
        for (const auto& path : { captures(dir)[0], dir / "sessions" / "s.xicap" })
        {
            INFO(path.string());
            xicap::Reader r(path);
            size_t        i{ 0 };
            for (const auto& p : r)
            {
                REQUIRE(i < ranges.size());
                CHECK(zeroed(ranges[i], p.data, saveChat));
                CHECK(((p.flags & xicap::PacketFlag::Redacted) != 0) == !saveChat);
                ++i;
            }
            CHECK(i == ranges.size());
        }
        std::vector<uint8_t> frames;
        for (const auto& m : live)
        {
            frames.insert(frames.end(), m.begin(), m.end());
        }
        REQUIRE(frames.size() == ranges.size() * (16 + SIZE));
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            CHECK(zeroed(ranges[i], std::span(frames).subspan(i * (16 + SIZE) + 16, SIZE), saveChat));
        }
        fs::remove_all(dir);
    }
}
