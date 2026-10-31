#include "core/clock.h"
#include "core/event_queue.h"
#include "core/recorder.h"
#include "core/seed_file.h"
#include "core/sha256.h"
#include "core/uploader.h"

#include <catch2/catch_test_macros.hpp>
#include <xicap/reader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace memoria;

namespace
{

auto tempDir() -> fs::path
{
    // ctest runs each test in its own process, so the name needs more than a counter.
    static const auto       run = std::random_device{}();
    static std::atomic<int> counter{ 0 };
    auto                    dir = fs::temp_directory_path() / (std::format("memoria-test-{}-{}", run, counter++));
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
    // A crash leaves the character's state file saying the capture is still
    // open. Another client in the same install, playing another character, has
    // its own.
    {
        std::ofstream state(dir / "state-1.txt", std::ios::trunc);
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
    bool noted = false;
    {
        xicap::Reader     r(captures(dir)[0]); // closed before the folder goes: Windows will not delete an open file
        xicap::ReadPacket p{};
        while (r.next(p))
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

namespace
{

// Mogsafe's upload protocol in memory: tus with deferred length, then a close
// that wants the file's hash and, like the real one, a complete capture (here:
// ending with the trailer magic XCPF).
class FakeMogsafe final : public Http
{
public:
    struct Upload
    {
        std::vector<uint8_t> data;
        bool                 closed{};
        bool                 gone{};
    };

    std::map<std::string, Upload> uploads;
    bool                          down{};
    int                           creates{};

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        if (down)
        {
            return std::unexpected("connection refused");
        }
        const auto get = [&](std::string_view name)
        {
            for (const auto& [k, v] : headers)
            {
                if (k == name)
                {
                    return v;
                }
            }
            return std::string{};
        };
        if (get("CF-Access-Client-Secret") != "secret")
        {
            return HttpResponse{ .status = 403 };
        }
        if (method == "POST" && url == "https://mogsafe.test/v1/uploads/")
        {
            const auto path = std::format("/v1/uploads/{}", ++creates);
            uploads[path]   = {};
            return HttpResponse{ .status = 201, .headers = { { "location", path } } };
        }
        const auto path    = url.substr(std::string("https://mogsafe.test").size());
        const bool isClose = path.ends_with("/close");
        const auto it      = uploads.find(isClose ? path.substr(0, path.size() - 6) : path);
        if (it == uploads.end() || it->second.gone)
        {
            return HttpResponse{ .status = method == "PATCH" ? 410 : 404 };
        }
        auto& u = it->second;
        if (method == "HEAD")
        {
            return u.closed ? HttpResponse{ .status = 404 } : HttpResponse{ .status = 200, .headers = { { "upload-offset", std::to_string(u.data.size()) } } };
        }
        if (method == "PATCH")
        {
            if (u.closed || get("Upload-Offset") != std::to_string(u.data.size()))
            {
                return HttpResponse{ .status = u.closed ? 410 : 409 };
            }
            u.data.insert(u.data.end(), body.begin(), body.end());
            return HttpResponse{ .status = 204 };
        }
        Sha256 sha;
        sha.update(u.data);
        const auto want = std::format(R"({{"sha256":"{}"}})", sha.hex());
        if (std::string(body.begin(), body.end()) != want)
        {
            return HttpResponse{ .status = 409 };
        }
        const bool complete = u.data.size() >= 4 && std::string(u.data.end() - 4, u.data.end()) == "XCPF";
        if (!complete)
        {
            return HttpResponse{ .status = 409 };
        }
        u.closed = true;
        return HttpResponse{ .status = 200 };
    }
};

// Hands requests to a fake that outlives the uploader.
class Forward final : public Http
{
public:
    explicit Forward(FakeMogsafe& to)
    : to_(to)
    {
    }

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        return to_.request(method, url, headers, body);
    }

private:
    FakeMogsafe& to_;
};

auto append(const fs::path& path, const std::string& bytes) -> void
{
    std::ofstream(path, std::ios::binary | std::ios::app) << bytes;
}

auto readAll(const fs::path& path) -> std::vector<uint8_t>
{
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), {} };
}

auto uploader(const fs::path& dir, FakeMogsafe& server) -> std::unique_ptr<Uploader>
{
    UploadOptions options;
    options.dir         = dir;
    options.server      = "https://mogsafe.test";
    options.credentials = { { "CF-Access-Client-Secret", "secret" } };
    options.idle        = std::chrono::milliseconds(0);
    return std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
}

} // namespace

TEST_CASE("sha256 matches the FIPS 180-4 examples")
{
    Sha256 abc;
    abc.update({ reinterpret_cast<const uint8_t*>("abc"), 3 });
    CHECK(abc.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256            longer;
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    longer.update({ reinterpret_cast<const uint8_t*>(two.data()), two.size() });
    CHECK(longer.hex() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("an upload follows a growing file and closes once it is complete")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    auto        up = uploader(dir, server);

    append(file, std::string(100, 'x'));
    CHECK(!up->pass());
    CHECK(server.creates == 0); // no header yet

    append(file, std::string(400, 'x'));
    CHECK(!up->pass());
    CHECK(!up->pass()); // all sent; the close is refused: no footer yet
    append(file, std::string(50, 'x') + "XCPF");
    CHECK(!up->pass()); // sends the rest
    CHECK(!up->pass()); // closes
    REQUIRE(server.creates == 1);
    const auto& u = server.uploads.begin()->second;
    CHECK(u.closed);
    CHECK(u.data == readAll(file));
    const auto status = up->status();
    REQUIRE(status.size() == 1);
    CHECK(status[0].closed);
    CHECK(status[0].sent == 554);
    CHECK(status[0].error.empty());
}

TEST_CASE("a restart resumes the same upload")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    append(file, std::string(500, 'x'));
    CHECK(!uploader(dir, server)->pass());
    append(file, std::string(300, 'x'));
    CHECK(!uploader(dir, server)->pass());
    CHECK(server.creates == 1);
    CHECK(server.uploads.begin()->second.data == readAll(file));
}

TEST_CASE("an upload the server cut off starts over, and outages only delay")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    auto        up = uploader(dir, server);
    append(file, std::string(500, 'x'));
    CHECK(!up->pass());

    server.down = true;
    append(file, std::string(100, 'x'));
    CHECK(up->pass().has_value());
    CHECK(up->status()[0].error.find("refused") != std::string::npos);

    server.down                         = false;
    server.uploads.begin()->second.gone = true;
    CHECK(!up->pass()); // 410: forgets the upload
    CHECK(!up->pass()); // a new upload of the whole file
    CHECK(server.creates == 2);
    CHECK(server.uploads.rbegin()->second.data == readAll(file));
    CHECK(up->status()[0].error.empty());
}
