#include "support.h"

#include "core/cap/calls.h"
#include "core/cap/caps.h"

using namespace memoria;
using namespace memoria::test;

namespace
{

auto notes(const fs::path& path) -> std::vector<std::string>
{
    std::vector<std::string> out;
    xicap::Reader            r(path);
    for (const auto& p : r)
    {
        for (const auto& m : p.markers)
        {
            if (m.kind == xicap::MarkerKind::Note)
            {
                out.push_back(m.text);
            }
        }
    }
    return out;
}

auto loggedIn(const fs::path& dir) -> std::unique_ptr<Recorder>
{
    auto rec = std::make_unique<Recorder>(options(dir));
    rec->login(T0, who("Kalnor", 1));
    rec->packet(gamePacket(T0 + 1));
    REQUIRE(waitFor([&]
                    {
                        return !rec->status().capture.empty();
                    }));
    return rec;
}

// Mogsafe's capture calls: answers in order, then 200.
struct CapServer final : Http
{
    struct Call
    {
        std::string    url;
        std::string    auth;
        nlohmann::json body;
    };

    std::mutex        mutex;
    std::vector<Call> calls;
    std::vector<int>  answers;

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        const std::scoped_lock lock{ mutex };
        calls.push_back({ .url = method + " " + url, .auth = headers.at(0).second, .body = nlohmann::json::parse(body.begin(), body.end()) });
        if (answers.empty())
        {
            return HttpResponse{ .status = 200 };
        }
        const auto status = answers.front();
        answers.erase(answers.begin());
        return HttpResponse{ .status = status };
    }

    auto count() -> size_t
    {
        const std::scoped_lock lock{ mutex };
        return calls.size();
    }

    auto at(const size_t i) -> Call
    {
        const std::scoped_lock lock{ mutex };
        return calls.at(i);
    }
};

struct ToCapServer final : Http
{
    explicit ToCapServer(CapServer& to)
    : to_{ to }
    {
    }

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        return to_.request(method, url, headers, body);
    }

private:
    CapServer& to_;
};

auto aCap(std::string id, std::string name, const fs::path& recording) -> cap::Cap
{
    return { .id = std::move(id), .name = std::move(name), .stopWith = "/memoria cap stop", .startMs = T0, .recording = recording };
}

} // namespace

TEST_CASE("cap start takes a name, then a path from the first word that holds a slash")
{
    using cap::parseStart;
    CHECK(parseStart("crabs").name == "crabs");
    CHECK(parseStart("crabs").path.empty());
    CHECK(parseStart("  crab fight  ").name == "crab fight");
    CHECK(parseStart("").name.empty());
    const auto captain = parseStart("crabs E:/Ashita v4/addons/captain/captures/crabs_2026-11-6_23_36/Kalnor/");
    CHECK(captain.name == "crabs");
    CHECK(captain.path == "E:/Ashita v4/addons/captain/captures/crabs_2026-11-6_23_36/Kalnor/");
    const auto unnamed = parseStart(R"("C:\Program Files\caps\mine.xicap")");
    CHECK(unnamed.name.empty());
    CHECK(unnamed.path == R"(C:\Program Files\caps\mine.xicap)");
    CHECK(parseStart(R"("crab fight" C:/x/)").name == "crab fight");
}

TEST_CASE("a capture's id is 12 characters of Mogsafe's alphabet, and its names and lengths read as chat shows them")
{
    const auto id = cap::newId();
    CHECK(id.size() == 12);
    CHECK(id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
    CHECK(cap::newId() != id);
    CHECK(cap::defaultName(T0) == "cap_20260921-1413");
    CHECK(cap::lengthText(12'000) == "12s");
    CHECK(cap::lengthText(252'900) == "4m 12s");
    CHECK(cap::lengthText(3'900'000) == "1h 5m");
    const auto c = aCap("abcdefghijkl", "crabs", {});
    CHECK(cap::page("https://mogsafe.test", c) == "https://mogsafe.test/captures/abcdefghijkl");
    CHECK(cap::startMarker(c) == "cap start crabs id=abcdefghijkl");
    CHECK(cap::stopMarker(c) == "cap stop id=abcdefghijkl");
    CHECK(cap::fileIn("E:/captures/crabs/Kalnor/") == fs::path("E:/captures/crabs/Kalnor/memoria.xicap"));
    CHECK(cap::fileIn("E:/caps/mine.xicap") == fs::path("E:/caps/mine.xicap"));
}

TEST_CASE("the caps folder takes a free name")
{
    const auto dir = tempDir();
    CHECK(cap::freeFile(dir, "crab fight", "Kalnor", T0) == dir / "crab_fight_Kalnor_20260921-1413.xicap");
    touch(dir / "crabs_Kalnor_20260921-1413.xicap");
    touch(dir / "crabs_Kalnor_20260921-1413-2.xicap");
    CHECK(cap::freeFile(dir, "crabs", "Kalnor", T0) == dir / "crabs_Kalnor_20260921-1413-3.xicap");
    CHECK(cap::freeFile(dir, "crabs", "", T0) == dir / "crabs_unknown_20260921-1413.xicap");
    fs::remove_all(dir);
}

TEST_CASE("one capture at a time, marked in the recording and copied to the caps folder")
{
    const auto  dir = tempDir();
    std::string id;
    {
        const auto rec = loggedIn(dir);
        cap::Caps  caps{ dir / "caps", "/cap stop" };
        const auto started = caps.start(*rec, "crabs", T0 + 2, "Kalnor");
        REQUIRE(started);
        id = started->id;
        CHECK(started->name == "crabs");
        CHECK(started->recording == rec->status().capture);
        rec->packet(gamePacket(T0 + 3));
        // Typed with no folder, so captain did not start it: its own stop works anywhere.
        CHECK(caps.start(*rec, "other", T0 + 4, "Kalnor").error() == "A capture is running: crabs. /memoria cap stop first.");
        CHECK(caps.running()->name == "crabs");
        CHECK(caps.stop(*rec, T0 + 5)->name == "crabs");
        rec->packet(gamePacket(T0 + 6));
        CHECK(!caps.stop(*rec, T0 + 7));
        rec->stop();
    }
    REQUIRE(captures(dir).size() == 1); // the copy is not in the folder that uploads
    CHECK(notes(captures(dir)[0]) == std::vector<std::string>{ "cap start crabs id=" + id, "cap stop id=" + id });
    const auto copy = readCapture(dir / "caps" / "crabs_Kalnor_20260921-1413.xicap");
    CHECK(copy.complete);
    CHECK(copy.packets == 1);
    fs::remove_all(dir);
}

TEST_CASE("captain's capture goes to its folder, its refusal names /cap stop, and a logout ends it with no stop marker")
{
    const auto  dir    = tempDir();
    const auto  folder = dir / "captain" / "crabs_2026-9-21_7_13" / "Kalnor";
    std::string id;
    {
        const auto rec = loggedIn(dir);
        cap::Caps  caps{ dir / "caps", "/cap stop" };
        const auto started = caps.start(*rec, "crabs " + folder.generic_string() + "/", T0 + 2, "Kalnor");
        REQUIRE(started);
        id = started->id;
        rec->packet(gamePacket(T0 + 3));
        CHECK(caps.start(*rec, "", T0 + 4, "Kalnor").error() == "A capture is running: crabs. /cap stop first.");
        rec->logout(T0 + 5);
        CHECK(caps.end(*rec, T0 + 5)->id == id);
        CHECK(!caps.running());
        rec->packet(gamePacket(T0 + 6));
        const auto unnamed = caps.start(*rec, "", T0 + 7, "Kalnor");
        REQUIRE(unnamed);
        CHECK(unnamed->name == "cap_20260921-1413");
        CHECK(unnamed->id != id);
        rec->stop();
    }
    CHECK(notes(captures(dir)[0]).front() == "cap start crabs id=" + id);
    CHECK(std::ranges::count(notes(captures(dir)[0]), "cap stop id=" + id) == 0);
    CHECK(readCapture(folder / "memoria.xicap").packets == 1);
    CHECK(!fs::exists(dir / "caps" / "crabs_Kalnor_20260921-1413.xicap"));
    fs::remove_all(dir);
}

TEST_CASE("a capture's start waits for its recording's id, and its stop follows")
{
    const auto dir = tempDir();
    REQUIRE(!saveLink(dir, { .token = "t0ken", .account = "player" }));
    CapServer   server;
    std::mutex  idMutex;
    std::string recordingId;
    cap::Calls  calls{ cap::CallOptions{
                           .server      = "https://mogsafe.test",
                           .config      = dir,
                           .recordingId = [&](const std::string& file)
                           {
                              const std::scoped_lock lock{ idMutex };
                              return file == "rec.xicap" ? recordingId : std::string{};
                          },
                          .every = std::chrono::milliseconds(5),
                          .tries = 1000,
                      },
                      std::make_unique<ToCapServer>(server) };
    const auto c = aCap("abcdefghijkl", "crabs", dir / "rec.xicap");
    calls.started(c, 7);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(server.count() == 0);
    {
        const std::scoped_lock lock{ idMutex };
        recordingId = "R3cording123";
    }
    REQUIRE(waitFor([&]
                    {
                        return server.count() == 1;
                    }));
    calls.stopped(c, T0 + 252'900);
    REQUIRE(waitFor([&]
                    {
                        return server.count() == 2;
                    }));
    const auto start = server.at(0);
    CHECK(start.url == "POST https://mogsafe.test/v1/caps");
    CHECK(start.auth == "Bearer t0ken");
    CHECK(start.body == nlohmann::json{ { "id", "abcdefghijkl" }, { "name", "crabs" }, { "recording", "R3cording123" }, { "character_id", 7 }, { "start_ms", T0 } });
    const auto stop = server.at(1);
    CHECK(stop.url == "POST https://mogsafe.test/v1/caps/abcdefghijkl/stop");
    CHECK(stop.body == nlohmann::json{ { "end_ms", T0 + 252'900 } });
    fs::remove_all(dir);
}

TEST_CASE("a refused start is dropped, a capture cut short sends no stop, and an unlinked install sends nothing")
{
    const auto dir = tempDir();
    REQUIRE(!saveLink(dir, { .token = "t0ken", .account = "player" }));
    CapServer server;
    server.answers = { 409 }; // another install's id
    cap::Calls calls{ cap::CallOptions{
                          .server      = "https://mogsafe.test",
                          .config      = dir,
                          .recordingId = [](const std::string&)
                          {
                              return std::string{ "R3cording123" };
                          },
                          .every = std::chrono::milliseconds(5),
                          .tries = 1000,
                      },
                      std::make_unique<ToCapServer>(server) };
    const auto refused = aCap("refusedrefus", "a", dir / "rec.xicap");
    const auto cut     = aCap("cutcutcutcut", "b", dir / "rec.xicap");
    const auto next    = aCap("nextnextnext", "c", dir / "rec.xicap");
    calls.started(refused, 7);
    calls.started(cut, 7);
    calls.ended(cut);
    calls.started(next, 7);
    REQUIRE(waitFor([&]
                    {
                        return server.count() == 3;
                    }));
    CHECK(server.at(0).body["id"] == "refusedrefus");
    CHECK(server.at(1).body["id"] == "cutcutcutcut");
    CHECK(server.at(2).body["id"] == "nextnextnext");
    calls.stopped(next, T0 + 1);
    REQUIRE(waitFor([&]
                    {
                        return server.count() == 4;
                    }));
    CHECK(server.at(3).url.ends_with("/v1/caps/nextnextnext/stop"));

    REQUIRE(!memoria::unlink(dir));
    calls.started(aCap("unlinkedunli", "d", dir / "rec.xicap"), 7);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(server.count() == 4);
    fs::remove_all(dir);
}
