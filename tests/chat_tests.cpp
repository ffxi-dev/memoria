#include "support.h"

using namespace memoria;
using namespace memoria::test;

TEST_CASE("a linked install asks Mogsafe whether its account saves chat")
{
    struct Chat final : Http
    {
        std::string auth;
        int         status = 200;
        std::string body   = R"({"save_chat":true})";

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
    opts.policy = [&]
    {
        return policyFor(saveChat);
    };
    {
        Recorder rec(opts);
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        REQUIRE(waitFor([&]
                        {
                            return !rec.status().capture.empty();
                        }));
        saveChat = true; // the open capture keeps its policy
        rec.packet(gamePacket(T0 + 2));
        rec.login(T0 + 10'000, who("Mirabeth", 2));
        rec.packet(gamePacket(T0 + 10'001));
        rec.stop();
    }
    std::set<std::string> policies;
    for (const auto& path : captures(dir))
    {
        policies.insert(xicap::Reader(path).metadata().policy.name);
    }
    CHECK(policies == std::set<std::string>{ "xicap-default", "save-chat" });
    fs::remove_all(dir);
}

TEST_CASE("a capture that saves chat keeps it whole, in the file and the live frames; otherwise its text is zeroed")
{
    for (const bool saveChat : { false, true })
    {
        const auto dir  = tempDir();
        auto       opts = options(dir);
        opts.policy     = [saveChat]
        {
            return policyFor(saveChat);
        };
        {
            Recorder rec(opts);
            rec.login(T0, who("Kalnor", 1));
            auto said           = gamePacket(T0 + 1, 0x0B5); // chat, from the client
            said.clientToServer = true;
            rec.packet(std::move(said));
            rec.stop();
        }
        const auto zeroed = static_cast<std::ptrdiff_t>(saveChat ? 0 : 10); // the text, from 0x06
        {
            xicap::Reader r(captures(dir)[0]);
            for (const auto& p : r)
            {
                CHECK(std::ranges::count(p.data, std::byte{ 0 }) == zeroed);
            }
        }
        const auto e     = gamePacket(T0 + 1, 0x0B5);
        const auto frame = liveFrame({ .timestamp = e.timestamp, .direction = xicap::PacketDirection::ClientToServer, .data = std::as_bytes(std::span{ e.data }) }, policyFor(saveChat));
        CHECK(std::ranges::count(frame | std::views::drop(16), 0) == zeroed);
        fs::remove_all(dir);
    }
}
