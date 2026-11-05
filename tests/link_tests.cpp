#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "support.h"

using namespace memoria;
using namespace memoria::test;

namespace
{

// Wine, which runs these tests on Linux, keeps no DACL on a file: the permission checks run on Windows itself.
auto keepsPermissions() -> bool
{
#ifdef _WIN32
    return ::GetProcAddress(::GetModuleHandleA("ntdll.dll"), "wine_get_version") == nullptr;
#else
    return true;
#endif
}

} // namespace

namespace
{

// Mogsafe's link endpoints in memory; the test plays the player who approves.
class FakeLink final : public Http
{
public:
    enum class Code : uint8_t
    {
        Pending,
        Approved,
        Expired,
        Refused,
    };

    Code        state{ Code::Pending };
    std::string started; // the body of the last start

    auto request([[maybe_unused]] const std::string& method, const std::string& url, [[maybe_unused]] const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        if (url.ends_with("/v1/link/start"))
        {
            started = std::string(body.begin(), body.end());
            return HttpResponse{ .status = 201, .body = R"({"code":"ABCD-2345","poll":"p0ll","url":"https://mogsafe.test/link?code=ABCD-2345","interval":1,"expires":600})" };
        }
        if (nlohmann::json::parse(std::string(body.begin(), body.end())).at("poll") != "p0ll")
        {
            return HttpResponse{ .status = 410 };
        }
        switch (state)
        {
            case Code::Pending:
                return HttpResponse{ .status = 202, .body = R"({"status":"pending"})" };
            case Code::Approved:
                return HttpResponse{ .status = 200, .body = R"({"token":"t0ken","account":"player"})" };
            case Code::Expired:
                return HttpResponse{ .status = 410, .body = R"({"status":"expired"})" };
            case Code::Refused:
                return HttpResponse{ .status = 403, .body = R"({"status":"refused","reason":"linking is not open to you yet"})" };
        }
        return HttpResponse{ .status = 500 };
    }
};

} // namespace

TEST_CASE("a link starts with a code and a handle, and polls with the handle until a token comes")
{
    FakeLink   mogsafe;
    const auto code = startLink(mogsafe, "https://mogsafe.test");
    REQUIRE(code.has_value());
    CHECK(mogsafe.started == "{}");
    CHECK(code->code == "ABCD-2345");
    CHECK(code->poll == "p0ll");
    CHECK(code->interval == std::chrono::seconds(1));

    const auto pending = pollLink(mogsafe, "https://mogsafe.test", code->poll);
    REQUIRE(pending.has_value());
    CHECK(!pending->has_value());

    mogsafe.state     = FakeLink::Code::Approved;
    const auto linked = pollLink(mogsafe, "https://mogsafe.test", code->poll);
    REQUIRE(linked.has_value());
    REQUIRE(linked->has_value());
    CHECK((*linked)->token == "t0ken");
    CHECK((*linked)->account == "player");
    CHECK(!pollLink(mogsafe, "https://mogsafe.test", "another").has_value()); // a handle Mogsafe never issued
}

TEST_CASE("an expired or refused link says why")
{
    FakeLink mogsafe;
    mogsafe.state = FakeLink::Code::Expired;
    CHECK(pollLink(mogsafe, "https://mogsafe.test", "p0ll").error() == "the code expired");
    mogsafe.state = FakeLink::Code::Refused;
    CHECK(pollLink(mogsafe, "https://mogsafe.test", "p0ll").error() == "linking is not open to you yet");
}

TEST_CASE("the link is kept across restarts")
{
    const auto dir  = tempDir();
    const auto path = dir / "config" / "memoria" / "token";
    CHECK(!loadLink(path));
    REQUIRE(saveLink(path, { .token = "t0ken", .account = "player" }));
    const auto link = loadLink(path);
    REQUIRE(link);
    CHECK(link->token == "t0ken");
    CHECK(link->account == "player");
    CHECK((!keepsPermissions() || ownerOnly(path)));
    REQUIRE(saveLink(path, { .token = "n3w", .account = "player" })); // replaced whole, still private
    CHECK(loadLink(path)->token == "n3w");
    CHECK((!keepsPermissions() || ownerOnly(path)));
    CHECK(std::distance(fs::directory_iterator(path.parent_path()), fs::directory_iterator{}) == 1);
    fs::remove_all(dir);
}

TEST_CASE("a link that cannot be written says so")
{
    const auto dir = tempDir();
    touch(dir / "config"); // a file where the folder should be
    CHECK_FALSE(saveLink(dir / "config" / "token", { .token = "t0ken", .account = "player" }));
    fs::remove_all(dir);
}

TEST_CASE("live goes on only while the install shares: not paused, and the same token")
{
    const auto dir    = tempDir();
    const auto config = dir / "config";
    REQUIRE(saveLink(config / "token", { .token = "t0ken", .account = "player" }));
    CHECK(sharing(config, "t0ken"));
    touch(config / "paused"); // /memoria upload off, in any client
    CHECK_FALSE(sharing(config, "t0ken"));
    fs::remove(config / "paused");
    CHECK(sharing(config, "t0ken"));
    REQUIRE(saveLink(config / "token", { .token = "t1", .account = "player" })); // linked again
    CHECK_FALSE(sharing(config, "t0ken"));
    fs::remove(config / "token"); // /memoria unlink
    CHECK_FALSE(sharing(config, "t1"));
    fs::remove_all(dir);
}

TEST_CASE("a session's link opens the recording's page on its range, as Mogsafe reads it")
{
    CHECK(captureLink("https://mogsafe.test", "kQ3x9", 1'790'000'000'000, 1'790'000'060'500) == "https://mogsafe.test/captures/kQ3x9?from=1790000000000&to=1790000060500");
}

TEST_CASE("only the refused token is deleted")
{
    const auto dir  = tempDir();
    const auto path = dir / "token";
    CHECK(!removeLink(path, "t0ken"));
    REQUIRE(saveLink(path, { .token = "n3w", .account = "player" }));
    CHECK(!removeLink(path, "t0ken"));
    REQUIRE(loadLink(path));
    CHECK(loadLink(path)->token == "n3w");
    CHECK(removeLink(path, "n3w"));
    CHECK(!fs::exists(path));
    CHECK(fs::is_empty(dir));
    fs::remove_all(dir);
}

TEST_CASE("a link code that would poll at once or never expire is refused")
{
    struct Start final : Http
    {
        std::string body;

        auto request(const std::string&, const std::string&, const HttpHeaders&, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            return HttpResponse{ .status = 201, .body = body };
        }
    } mogsafe;
    mogsafe.body = R"({"code":"A","poll":"p","url":"https://mogsafe.test/link","interval":5,"expires":600})";
    CHECK(startLink(mogsafe, "https://mogsafe.test").has_value());
    for (const auto* bad : { R"({"code":"A","interval":0})", R"({"code":"A","interval":-5})", R"({"code":"A","expires":0})" })
    {
        INFO(bad);
        mogsafe.body = bad;
        CHECK(!startLink(mogsafe, "https://mogsafe.test").has_value());
    }
}

TEST_CASE("a token taken by mistake goes back without replacing one saved meanwhile, where hard links fail")
{
    const auto dir  = tempDir();
    const auto path = dir / "token";
    const auto olds = [&]
    {
        return std::ranges::count_if(fs::directory_iterator(dir), [](const auto& e)
                                     {
                                         return e.path().extension() == ".old";
                                     });
    };
    REQUIRE(saveLink(path, { .token = "a", .account = "player" }));
    CHECK(!removeLink(path, "t0ken", [](const fs::path&, const fs::path&)
                      {
                          return false; // no hard links here
                      }));
    REQUIRE(loadLink(path));
    CHECK(loadLink(path)->token == "a");
    CHECK(olds() == 0);

    CHECK(!removeLink(path, "t0ken", [&](const fs::path&, const fs::path&)
                      {
                          saveLink(path, { .token = "n3w", .account = "player" }); // another client links meanwhile
                          return false;
                      }));
    REQUIRE(loadLink(path));
    CHECK(loadLink(path)->token == "n3w");
    CHECK(olds() == 1); // the taken token stays in its file
    fs::remove_all(dir);
}

TEST_CASE("an approval that comes with a stop is dropped, so no token is published after an unlink")
{
    struct Poll final : Http
    {
        std::function<HttpResponse()> answer;

        auto request(const std::string&, const std::string&, const HttpHeaders&, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            return answer();
        }
    } mogsafe;
    const LinkCode code{ .code = "A", .poll = "p", .url = "https://mogsafe.test/link", .interval = std::chrono::seconds(0), .expires = std::chrono::seconds(60) };
    const auto     approved = []
    {
        return HttpResponse{ .status = 200, .body = R"({"token":"t0ken","account":"player"})" };
    };

    std::stop_source unlink;
    int              polls{ 0 };
    mogsafe.answer = [&]
    {
        if (++polls == 2)
        {
            unlink.request_stop(); // /memoria unlink while this poll is in flight
            return approved();
        }
        return HttpResponse{ .status = 202 };
    };
    const auto stopped = waitForApproval(mogsafe, "https://mogsafe.test", code, unlink.get_token());
    REQUIRE(!stopped);
    CHECK(stopped.error() == "stopped");

    mogsafe.answer   = approved;
    const auto later = waitForApproval(mogsafe, "https://mogsafe.test", code, std::stop_source{}.get_token());
    REQUIRE(later);
    CHECK(later->token == "t0ken");

    mogsafe.answer = []
    {
        return HttpResponse{ .status = 202 };
    };
    const auto expired = waitForApproval(mogsafe, "https://mogsafe.test", { .poll = "p", .interval = std::chrono::seconds(0), .expires = std::chrono::seconds(0) }, std::stop_source{}.get_token());
    REQUIRE(!expired);
    CHECK(expired.error() == "the code expired");
}
