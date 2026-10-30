#include "core/clock.h"
#include "core/event_queue.h"
#include "core/seed_file.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

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
