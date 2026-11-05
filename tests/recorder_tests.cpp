#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include "support.h"

using namespace memoria;
using namespace memoria::test;

TEST_CASE("timestamps follow the monotonic clock, not the wall clock")
{
    uint64_t    wall{ 1'790'000'000'000 };
    uint64_t    steady{ 5'000 };
    const Clock clock([&]
                      {
                          return wall;
                      },
                      [&]
                      {
                          return steady;
                      });
    CHECK(clock.nowMs() == 1'790'000'000'000);
    steady += 250;
    CHECK(clock.nowMs() == 1'790'000'000'250);
    wall -= 2'000; // Windows corrects the PC's clock
    steady += 100;
    CHECK(clock.nowMs() == 1'790'000'000'350);
}

TEST_CASE("the queue drops packets past its limit, never markers")
{
    EventQueue queue(3 * (sizeof(Event) + 100));
    CHECK(queue.push(packet(100)));
    CHECK(queue.push(packet(100)));
    CHECK(queue.push(packet(100)));
    CHECK_FALSE(queue.push(packet(100)));
    CHECK(queue.push(Event{ .kind = EventKind::Login }));
    CHECK(queue.takeDropped() == 1);
    CHECK(queue.takeDropped() == 0);

    std::vector<Event> out;
    queue.drain(out);
    REQUIRE(out.size() == 4);
    CHECK(out.back().kind == EventKind::Login);
    CHECK_FALSE(queue.push(packet(100))); // the writer still holds that batch
    queue.drain(out);                     // and frees it here
    CHECK(out.empty());
    CHECK(queue.push(packet(100)));
    CHECK(queue.push(packet(100)));
    queue.drain(out);
    CHECK(out.size() == 2);
    CHECK(queue.push(packet(100))); // two in hand, one queued: full
    CHECK_FALSE(queue.push(packet(100)));
}

TEST_CASE("a recording is xicap format 5, complete")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.logout(T0 + 2);
        rec.stop();
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 1);
    CHECK(readAll(files[0])[8] == 5);
    const auto read = readCapture(files[0]);
    CHECK(read.packets == 1);
    CHECK(read.complete);
    fs::remove_all(dir);
}

TEST_CASE("a relog on the same character continues the capture")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.packet(gamePacket(T0 + 2));
        rec.logout(T0 + 3);
        rec.login(T0 + 10'000, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 10'001));
        rec.stop();
        CHECK(rec.status().packets == 3);
        CHECK(rec.status().error.empty());
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 1);
    const auto c = readCapture(files[0]);
    CHECK(c.complete);
    CHECK(c.packets == 3);
    CHECK(c.name == "Kalnor");
    CHECK(c.atLogin);
    CHECK(c.markers == std::vector{ xicap::MarkerKind::Login, xicap::MarkerKind::Logout, xicap::MarkerKind::Login });
    CHECK(files[0].filename().string().find("-Kalnor-") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("a start that missed the content id continues on a login that has it")
{
    const auto dir = tempDir();
    {
        Login late{ who("Kalnor", 1) };
        late.character.contentId = 0;
        late.worldId             = 0;
        late.midSession          = true;
        Recorder rec(options(dir));
        rec.login(T0, late);
        rec.packet(gamePacket(T0 + 1));
        rec.login(T0 + 10'000, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 10'001));
        Login other{ who("Kalnor", 1) };
        other.character.contentId = 7; // same character id and name on another account
        rec.login(T0 + 20'000, other);
        rec.packet(gamePacket(T0 + 20'001));
        rec.stop();
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 2);
    CHECK(readCapture(files[0]).markers == std::vector{ xicap::MarkerKind::Login });
    CHECK(readCapture(files[0]).packets == 2);
    fs::remove_all(dir);
}

TEST_CASE("the same character id and name on another world starts a new capture")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        Login moved{ who("Kalnor", 1) };
        moved.worldId = 43;
        rec.login(T0 + 10'000, moved);
        rec.packet(gamePacket(T0 + 10'001));
        rec.stop();
    }
    CHECK(captures(dir).size() == 2);
    fs::remove_all(dir);
}

TEST_CASE("another character starts a capture that continues the first")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.logout(T0 + 2);
        rec.login(T0 + 5'000, who("Mule", 2));
        rec.packet(gamePacket(T0 + 5'001));
        rec.stop();
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 2);
    const auto first  = readCapture(files[0]);
    const auto second = readCapture(files[1]);
    CHECK(first.name == "Kalnor");
    CHECK(second.name == "Mule");
    CHECK(first.complete);
    CHECK(second.previous == first.id);
    CHECK(first.previous == xicap::CaptureId{});
    fs::remove_all(dir);
}

TEST_CASE("a start after a crash continues the capture the crash cut off")
{
    const auto       dir = tempDir();
    xicap::CaptureId cut{};
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.stop();
        cut = readCapture(captures(dir)[0]).id;
    }
    // A crash leaves the character's state file saying the capture is open. Another character, in another client, has its own.
    {
        std::ofstream state(dir / "state-1.txt", std::ios::trunc);
        for (const auto b : cut)
        {
            state << std::format("{:02x}", b);
        }
        state << " open\n";
    }
    {
        Login late{ who("Kalnor", 1) };
        late.midSession = true; // the client came back already in a zone
        Recorder rec(options(dir));
        rec.login(T0 + 60'000, late);
        rec.packet(gamePacket(T0 + 60'001));
        rec.stop();
    }
    {
        Recorder rec(options(dir));
        rec.login(T0 + 120'000, who("Mule", 2));
        rec.packet(gamePacket(T0 + 120'001));
        rec.stop();
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 3);
    const auto after = readCapture(files[1]);
    CHECK(after.previous == cut);
    CHECK(readCapture(files[2]).previous == xicap::CaptureId{}); // Mule never crashed
    CHECK(!after.atLogin);
    CHECK(after.markers.empty()); // no login seen: none marked
    fs::remove_all(dir);
}

TEST_CASE("a session is a second, complete capture of the same packets")
{
    const auto dir     = tempDir();
    const auto session = dir / "captain" / "session one" / "memoria.xicap";
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.sessionStart(T0 + 2, session);
        rec.note(T0 + 2, "cap start");
        rec.packet(gamePacket(T0 + 3));
        rec.packet(gamePacket(T0 + 4));
        rec.note(T0 + 5, "cap stop");
        rec.sessionStop(T0 + 5);
        rec.packet(gamePacket(T0 + 6));
        rec.stop();
    }
    const auto main = readCapture(captures(dir)[0]);
    const auto part = readCapture(session);
    CHECK(part.complete);
    CHECK(part.name == "Kalnor");
    CHECK(part.previous == main.id);
    CHECK(!part.atLogin);

    xicap::Reader r(session);
    size_t        packets{ 0 };
    std::string   notes;
    for (const auto& p : r)
    {
        ++packets;
        for (const auto& m : p.markers)
        {
            notes += m.text + ";";
        }
    }
    CHECK(packets == 2);
    CHECK(notes == "cap start;");
}

TEST_CASE("a session never replaces a file, the main capture least of all")
{
    const auto dir      = tempDir();
    const auto existing = dir / "yesterday" / "memoria.xicap";
    fs::create_directories(existing.parent_path());
    std::ofstream(existing) << "yesterday's session";
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        for (int i = 0; i < 200 && rec.status().capture.empty(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rec.sessionStart(T0 + 2, existing);
        rec.packet(gamePacket(T0 + 3));
        rec.sessionStart(T0 + 4, rec.status().capture);
        rec.packet(gamePacket(T0 + 5));
        rec.stop();
        CHECK(rec.status().error.empty());
        CHECK(rec.status().sessionError.find("exists") != std::string::npos);
    }
    std::string text;
    std::getline(std::ifstream(existing), text);
    CHECK(text == "yesterday's session");
    const auto files = captures(dir);
    REQUIRE(files.size() == 1);
    const auto main = readCapture(files[0]);
    CHECK(main.complete);
    CHECK(main.packets == 3);
    fs::remove_all(dir);
}

TEST_CASE("a session that cannot start leaves the main capture recording")
{
    const auto dir = tempDir();
    std::ofstream(dir / "blocker") << "a file, not a folder";
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.sessionStart(T0 + 2, dir / "blocker" / "memoria.xicap");
        rec.packet(gamePacket(T0 + 3));
        rec.stop();
        CHECK(rec.status().error.empty());
        CHECK(!rec.status().sessionError.empty());
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 1);
    const auto main = readCapture(files[0]);
    CHECK(main.complete);
    CHECK(main.packets == 2);
    fs::remove_all(dir);
}

#ifdef __linux__
TEST_CASE("a session write that fails leaves the main capture recording")
{
    const auto dir     = tempDir();
    const auto session = dir / "session" / "memoria.xicap";
    auto       opts    = options(dir);
    opts.flushEvery    = std::chrono::hours(1); // only the logout's marker writes the session
    {
        Recorder rec(opts);
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.sessionStart(T0 + 2, session);
        for (int i = 0; i < 200 && rec.status().session.empty(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(!rec.status().session.empty());
        // From here the session's file writes to /dev/full.
        const int full{ ::open("/dev/full", O_WRONLY) };
        REQUIRE(full >= 0);
        for (const auto& fd : fs::directory_iterator("/proc/self/fd"))
        {
            std::error_code ec;
            if (fs::read_symlink(fd.path(), ec) == fs::canonical(session))
            {
                ::dup2(full, std::stoi(fd.path().filename().string()));
            }
        }
        ::close(full);
        rec.packet(gamePacket(T0 + 3));
        rec.logout(T0 + 4);
        rec.login(T0 + 5, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 6));
        rec.stop();
        CHECK(rec.status().error.empty());
        CHECK(!rec.status().sessionError.empty());
    }
    const auto main = readCapture(captures(dir)[0]);
    CHECK(main.complete);
    CHECK(main.packets == 3);
    CHECK(main.markers == std::vector{ xicap::MarkerKind::Login, xicap::MarkerKind::Logout, xicap::MarkerKind::Login });
    fs::remove_all(dir);
}

#endif

TEST_CASE("packets before any login are not recorded, and the flush timer writes while open")
{
    const auto dir = tempDir();
    Recorder   rec(options(dir));
    rec.packet(gamePacket(T0));
    rec.login(T0 + 1, who("Kalnor", 1));
    rec.packet(gamePacket(T0 + 2));
    // Without a stop, the 20 ms flush gets the frame to disk.
    for (int i = 0; i < 100 && (captures(dir).empty() || fs::file_size(captures(dir)[0]) == 0); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(captures(dir).size() == 1);
    CHECK(fs::file_size(captures(dir)[0]) > 0);
    CHECK(rec.status().capture == captures(dir)[0]);
    rec.stop();
    CHECK(readCapture(captures(dir)[0]).packets == 1);
    fs::remove_all(dir);
}

TEST_CASE("dropped packets leave a note in the capture")
{
    const auto dir  = tempDir();
    auto       opts = options(dir);
    opts.queueBytes = 3 * (sizeof(Event) + 16);
    opts.drainEvery = std::chrono::milliseconds(200); // let the queue fill
    {
        Recorder rec(opts);
        rec.login(T0, who("Kalnor", 1));
        for (int i = 0; i < 10; ++i)
        {
            rec.packet(gamePacket(T0 + 1 + i));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // one drain takes the full batch, the next frees it
        rec.packet(gamePacket(T0 + 100));                            // carries the note
        rec.stop();
        CHECK(rec.status().dropped > 0);
    }
    bool noted{ false };
    {
        xicap::Reader r(captures(dir)[0]); // closed before the folder goes: Windows keeps an open file
        for (const auto& p : r)
        {
            for (const auto& m : p.markers)
            {
                noted = noted || m.text.starts_with("dropped ");
            }
        }
    }
    CHECK(noted);
    fs::remove_all(dir);
}
