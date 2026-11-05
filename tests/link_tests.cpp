#include "support.h"

using namespace memoria;
using namespace memoria::test;

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
