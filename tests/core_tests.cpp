#include "core/clock.h"
#include "core/event_queue.h"
#include "core/recorder.h"
#include "core/seed_file.h"

#include <catch2/catch_test_macros.hpp>
#include <xicap/reader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace memoria;

namespace
{

auto tempDir() -> fs::path
{
    static std::atomic<int> counter{ 0 };
    auto                    dir = fs::temp_directory_path() / ("memoria-test-" + std::to_string(counter++));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

auto packet(size_t size) -> Event
{
    return Event{ .kind = Event::Kind::Packet, .data = std::vector<uint8_t>(size, 1) };
}

} // namespace

TEST_CASE("timestamps follow the monotonic clock, not the wall clock")
{
    uint64_t wall   = 1'790'000'000'000;
    uint64_t steady = 5'000;
    Clock    clock([&]
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
    CHECK(queue.push(Event{ .kind = Event::Kind::Login }));
    CHECK(queue.takeDropped() == 1);
    CHECK(queue.takeDropped() == 0);

    std::vector<Event> out;
    queue.drain(out);
    REQUIRE(out.size() == 4);
    CHECK(out.back().kind == Event::Kind::Login);
    CHECK(queue.push(packet(100))); // draining frees the room
}

TEST_CASE("the install key is created once and never replaced")
{
    const auto dir  = tempDir();
    const auto path = dir / "config" / "memoria" / "key";

    const auto first = loadOrCreateSeed(path);
    REQUIRE(first.has_value());
    CHECK(fs::file_size(path) == 32);
    const auto second = loadOrCreateSeed(path);
    REQUIRE(second.has_value());
    CHECK(*second == *first);

    std::ofstream(path, std::ios::binary | std::ios::trunc) << "short";
    CHECK_FALSE(loadOrCreateSeed(path).has_value());
    CHECK(fs::file_size(path) == 5); // left alone, not replaced

    fs::remove(path);
    fs::create_directory(path); // something there that is not a file
    CHECK_FALSE(loadOrCreateSeed(path).has_value());
    fs::remove_all(dir);
}

namespace
{

constexpr uint64_t T0 = 1'790'000'000'000ULL;

auto options(const fs::path& dir) -> RecorderOptions
{
    RecorderOptions o;
    o.dir = dir;
    o.seed.fill(7);
    o.captureAgent  = "Memoria test";
    o.clientVersion = "test-client";
    o.flushEvery    = std::chrono::milliseconds(20);
    o.drainEvery    = std::chrono::milliseconds(5);
    return o;
}

auto who(const char* name, uint32_t id) -> Login
{
    Login l{};
    l.character.contentId   = id;
    l.character.characterId = id;
    l.character.worldId     = 42;
    std::memcpy(l.character.characterName.data(), name, std::strlen(name));
    l.network = xicap::Network::Retail;
    return l;
}

auto gamePacket(uint64_t t, uint16_t opcode = 0x015) -> Event
{
    std::vector<uint8_t> data(16, 0xAB);
    const auto           hdr = static_cast<uint16_t>((4 << 9) | opcode);
    data[0]                  = static_cast<uint8_t>(hdr);
    data[1]                  = static_cast<uint8_t>(hdr >> 8);
    return Event{ .timestamp = t, .data = std::move(data) };
}

auto captures(const fs::path& dir) -> std::vector<fs::path>
{
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".xicap")
        {
            out.push_back(e.path());
        }
    }
    std::ranges::sort(out, {}, [](const fs::path& p)
                      {
                          return fs::last_write_time(p);
                      });
    return out;
}

struct Read
{
    std::vector<xicap::MarkerKind> markers;
    uint64_t                       packets{};
    bool                           complete{};
    bool                           signedToEnd{};
    xicap::StartContext            start{};
    xicap::CaptureId               id{};
    xicap::CaptureId               previous{};
    std::string                    name;
};

auto readCapture(const fs::path& path) -> Read
{
    xicap::Reader     r(path);
    Read              out{ .complete = r.complete(), .start = r.startContext(), .id = r.captureId(), .previous = r.previousCaptureId() };
    xicap::ReadPacket p{};
    while (r.next(p))
    {
        out.packets++;
        for (const auto& m : p.markers)
        {
            out.markers.push_back(m.kind);
        }
    }
    out.signedToEnd = r.verify().signedToEnd;
    out.name        = r.character().characterName.data();
    return out;
}

} // namespace

TEST_CASE("a relog on the same character continues the capture")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Nouurs", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.packet(gamePacket(T0 + 2));
        rec.logout(T0 + 3);
        rec.login(T0 + 10'000, who("Nouurs", 1));
        rec.packet(gamePacket(T0 + 10'001));
        rec.stop();
        CHECK(rec.status().packets == 3);
        CHECK(rec.status().error.empty());
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 1);
    const auto c = readCapture(files[0]);
    CHECK(c.complete);
    CHECK(c.signedToEnd);
    CHECK(c.packets == 3);
    CHECK(c.name == "Nouurs");
    CHECK(c.start == xicap::StartContext::Login);
    CHECK(c.markers == std::vector{ xicap::MarkerKind::Login, xicap::MarkerKind::Logout, xicap::MarkerKind::Login });
    CHECK(files[0].filename().string().find("-Nouurs-") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("another character starts a capture that continues the first")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Nouurs", 1));
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
    CHECK(first.name == "Nouurs");
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
        rec.login(T0, who("Nouurs", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.stop();
        cut = readCapture(captures(dir)[0]).id;
    }
    // A crash leaves the state file saying the capture is still open.
    {
        std::ofstream state(dir / "state.txt", std::ios::trunc);
        for (const auto b : cut)
        {
            state << std::format("{:02x}", b);
        }
        state << " open\n";
    }
    {
        Login late      = who("Nouurs", 1);
        late.midSession = true; // the client came back already in a zone
        Recorder rec(options(dir));
        rec.login(T0 + 60'000, late);
        rec.packet(gamePacket(T0 + 60'001));
        rec.stop();
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 2);
    const auto after = readCapture(files[1]);
    CHECK(after.previous == cut);
    CHECK(after.start == xicap::StartContext::MidSession);
    CHECK(after.markers.empty()); // no login seen: none marked
    fs::remove_all(dir);
}

TEST_CASE("packets before any login are not recorded, and the flush timer writes while open")
{
    const auto dir = tempDir();
    Recorder   rec(options(dir));
    rec.packet(gamePacket(T0));
    rec.login(T0 + 1, who("Nouurs", 1));
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
        rec.login(T0, who("Nouurs", 1));
        for (int i = 0; i < 10; ++i)
        {
            rec.packet(gamePacket(T0 + 1 + i));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        rec.packet(gamePacket(T0 + 100)); // carries the note
        rec.stop();
        CHECK(rec.status().dropped > 0);
    }
    xicap::Reader     r(captures(dir)[0]);
    xicap::ReadPacket p{};
    bool              noted = false;
    while (r.next(p))
    {
        for (const auto& m : p.markers)
        {
            noted = noted || m.text.starts_with("dropped ");
        }
    }
    CHECK(noted);
    fs::remove_all(dir);
}
