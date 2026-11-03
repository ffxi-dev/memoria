#include "core/captain.h"
#include "core/clock.h"
#include "core/ed25519.h"
#include "core/event_queue.h"
#include "core/link.h"
#include "core/live.h"
#include "core/recorder.h"
#include "core/seed_file.h"
#include "core/uploader.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <xicap/constants.h>
#include <xicap/reader.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <latch>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace memoria;

namespace
{

auto tempDir() -> fs::path
{
    // ctest runs each test in its own process: a counter alone would repeat names.
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

TEST_CASE("clients starting together on a fresh install all get the key on disk")
{
    for (int round = 0; round < 20; ++round)
    {
        const auto                                                 dir  = tempDir();
        const auto                                                 path = dir / "key";
        std::vector<std::expected<xicap::SecretSeed, std::string>> seeds(8);
        {
            std::latch                go(static_cast<std::ptrdiff_t>(seeds.size()));
            std::vector<std::jthread> clients;
            for (auto& seed : seeds)
            {
                clients.emplace_back([&]
                                     {
                                         go.arrive_and_wait();
                                         seed = loadOrCreateSeed(path);
                                     });
            }
        }
        const auto onDisk = loadOrCreateSeed(path);
        REQUIRE(onDisk.has_value());
        for (const auto& seed : seeds)
        {
            if (!seed)
            {
                FAIL(seed.error());
            }
            CHECK(*seed == *onDisk);
        }
        CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 1); // no temporary file left
        fs::remove_all(dir);
    }
}

namespace
{

constexpr uint64_t T0 = 1'790'000'000'000ULL;

auto options(const fs::path& dir) -> RecorderOptions
{
    RecorderOptions o;
    o.dir = dir;
    o.seed.fill(7);
    o.captureAgent = "Memoria test";
    o.flushEvery   = std::chrono::milliseconds(20);
    o.drainEvery   = std::chrono::milliseconds(5);
    return o;
}

auto who(const char* name, uint32_t id) -> Login
{
    Login l{};
    l.character.contentId   = id;
    l.character.characterId = id;
    l.character.worldId     = 42;
    std::memcpy(l.character.characterName.data(), name, std::strlen(name));
    l.network       = xicap::Network::Retail;
    l.clientVersion = "30261006_0";
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

TEST_CASE("a start that missed the content id continues on a login that has it")
{
    const auto dir = tempDir();
    {
        Login late               = who("Nouurs", 1);
        late.character.contentId = 0;
        late.character.worldId   = 0;
        late.midSession          = true;
        Recorder rec(options(dir));
        rec.login(T0, late);
        rec.packet(gamePacket(T0 + 1));
        rec.login(T0 + 10'000, who("Nouurs", 1));
        rec.packet(gamePacket(T0 + 10'001));
        Login other               = who("Nouurs", 1);
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

TEST_CASE("a session is a second, complete capture of the same packets")
{
    const auto dir     = tempDir();
    const auto session = dir / "captain" / "session one" / "memoria.xicap";
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Nouurs", 1));
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
    CHECK(part.name == "Nouurs");
    CHECK(part.previous == main.id);
    CHECK(part.start == xicap::StartContext::MidSession);

    xicap::Reader     r(session);
    xicap::ReadPacket p{};
    size_t            packets = 0;
    std::string       notes;
    while (r.next(p))
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
        rec.login(T0, who("Nouurs", 1));
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
        rec.login(T0, who("Nouurs", 1));
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
        rec.login(T0, who("Nouurs", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.sessionStart(T0 + 2, session);
        for (int i = 0; i < 200 && rec.status().session.empty(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(!rec.status().session.empty());
        // From here the session's file writes to /dev/full.
        const int full = ::open("/dev/full", O_WRONLY);
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
        rec.login(T0 + 5, who("Nouurs", 1));
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
        xicap::Reader     r(captures(dir)[0]); // closed before the folder goes: Windows keeps an open file
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

auto fromBase64(std::string_view in) -> std::string
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    uint32_t                   v    = 0;
    int                        bits = 0;
    for (const char c : in)
    {
        if (c == '=')
        {
            break;
        }
        v = v << 6 | static_cast<uint32_t>(digits.find(c));
        if ((bits += 6) >= 8)
        {
            out += static_cast<char>(v >> (bits -= 8) & 0xFF);
        }
    }
    return out;
}

// Mogsafe's upload protocol in memory. Like the real one, a close wants the file's hash and a complete capture: here, one ending in the trailer magic XCPF.
// A captain import instead names its log's CRC-32 and size, which a later import of the same log meets as a 409.
class FakeMogsafe final : public Http
{
public:
    struct Upload
    {
        std::vector<uint8_t>               data;
        bool                               closed{};
        bool                               cutOff{}; // ended by the sweep: every request answers 410
        std::map<std::string, std::string> meta;
    };

    std::map<std::string, Upload>           uploads;
    std::map<std::string, std::string>      imported;    // capture id by "crc32:size"
    std::set<std::string>                   replaceable; // imported captures an upload with a search log replaces
    std::map<std::string, std::vector<int>> forced;      // answers by method, given before anything else
    std::set<std::thread::id>               threads;
    std::string                             token = "t0ken";
    bool                                    down{};
    bool                                    crashAfterClose{}; // the game dies once a close is accepted, before its answer
    int                                     creates{};
    int                                     requests{};
    int                                     patches{};
    uint64_t                                received{};        // bytes accepted by appends
    int                                     crashAtPatch = -1; // the game dies during this PATCH, before any answer
    std::function<void()>                   onPatch;
    std::function<void()>                   onCreate;

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        ++requests;
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
        threads.insert(std::this_thread::get_id());
        if (get("Authorization") != "Bearer " + token)
        {
            return HttpResponse{ .status = 401 };
        }
        if (auto& f = forced[method]; !f.empty())
        {
            const auto status = f.front();
            f.erase(f.begin());
            return HttpResponse{ .status = status, .body = "forced\n" };
        }
        if (method == "POST" && url == "https://mogsafe.test/v1/uploads/")
        {
            std::map<std::string, std::string> meta;
            std::istringstream                 fields(get("Upload-Metadata"));
            for (std::string field; std::getline(fields, field, ',');)
            {
                meta[field.substr(0, field.find(' '))] = fromBase64(field.substr(field.find(' ') + 1));
            }
            if (meta["format"] == "captain" && meta["manifest"].find("StartTime") == std::string::npos)
            {
                return HttpResponse{ .status = 400, .body = "manifest.txt has no StartTime\n" };
            }
            std::string replaces;
            if (const auto it = imported.find(meta["crc32"] + ":" + meta["size"]); it != imported.end())
            {
                if (!meta.contains("search_bytes") || !replaceable.contains(it->second))
                {
                    return HttpResponse{ .status = 409, .headers = { { "mogsafe-capture", it->second } }, .body = "this log is already in Mogsafe as " + it->second };
                }
                replaces = it->second;
            }
            const auto path = std::format("/v1/uploads/{}", ++creates);
            uploads[path]   = { .meta = meta };
            if (onCreate)
            {
                onCreate();
            }
            HttpResponse res{ .status = 201, .headers = { { "location", path } } };
            if (!replaces.empty())
            {
                res.headers.emplace("mogsafe-replaces", replaces);
            }
            return res;
        }
        const auto path    = url.substr(std::string("https://mogsafe.test").size());
        const bool isClose = path.ends_with("/close");
        const auto it      = uploads.find(isClose ? path.substr(0, path.size() - 6) : path);
        if (it == uploads.end())
        {
            return HttpResponse{ .status = 404 };
        }
        auto& u = it->second;
        if (u.cutOff || u.closed && !isClose)
        {
            return HttpResponse{ .status = 410, .headers = { { "mogsafe-ended", u.cutOff ? "cut_off" : "complete" } } };
        }
        if (method == "HEAD")
        {
            return HttpResponse{ .status = 200, .headers = { { "upload-offset", std::to_string(u.data.size()) } } };
        }
        if (method == "PATCH")
        {
            if (patches++ == crashAtPatch)
            {
                throw std::runtime_error("the game crashed");
            }
            if (get("Upload-Offset") != std::to_string(u.data.size()))
            {
                return HttpResponse{ .status = 409 };
            }
            u.data.insert(u.data.end(), body.begin(), body.end());
            received += body.size();
            if (onPatch)
            {
                onPatch();
            }
            return HttpResponse{ .status = 204 };
        }
        if (!u.meta["crc32"].empty())
        {
            if (std::string(body.begin(), body.end()) != "{}")
            {
                return HttpResponse{ .status = 400 };
            }
            u.closed                                         = true;
            imported[u.meta["crc32"] + ":" + u.meta["size"]] = std::format("cap{}", it->first.back());
            if (crashAfterClose)
            {
                throw std::runtime_error("the game crashed");
            }
            return HttpResponse{ .status = 200 };
        }
        const auto want = std::format(R"({{"sha256":"{}"}})", picosha2::hash256_hex_string(u.data));
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
    if (!fs::exists(dir / "config" / "token"))
    {
        saveLink(dir / "config" / "token", { .token = "t0ken", .account = "sruon" });
    }
    UploadOptions options;
    options.dir    = dir;
    options.server = "https://mogsafe.test";
    options.config = dir / "config";
    options.idle   = std::chrono::milliseconds(0);
    return std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
}

} // namespace

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
    CHECK(status[0].sent == 554);
    CHECK(status[0].error.empty());
}

TEST_CASE("a client that is not uploading reports what the uploading one sent")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    auto        sender = uploader(dir, server);
    auto        other  = uploader(dir, server);
    CHECK(other->status().empty());

    append(file, std::string(500, 'x'));
    CHECK(!sender->pass());
    append(file, std::string(100, 'x'));
    const auto status = other->status();
    CHECK(!other->uploading());
    REQUIRE(status.size() == 1);
    CHECK(status[0].name == "a.xicap");
    CHECK(status[0].sent == 500);
    CHECK(status[0].size == 600);
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

TEST_CASE("a crash during the first pass resumes the upload it created")
{
    for (const int crashAt : { 0, 1 }) // before the first chunk is accepted, and after it
    {
        const auto  dir  = tempDir();
        const auto  file = dir / "a.xicap";
        FakeMogsafe server;
        server.crashAtPatch = crashAt;
        append(file, std::string(3 * 512 * 1024, 'x'));
        CHECK_THROWS(uploader(dir, server)->pass()); // no save at the end of the pass, as in a crash
        server.crashAtPatch = -1;
        CHECK(!uploader(dir, server)->pass());
        CHECK(server.creates == 1);
        CHECK(server.uploads.begin()->second.data == readAll(file));
        fs::remove_all(dir);
    }
}

TEST_CASE("no byte goes until the upload's URL is saved, so a restart after a failed save sends nothing twice")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    auto        up = uploader(dir, server);
    append(file, std::string(500, 'x'));
    fs::create_directory(dir / "uploads.txt.tmp");
    CHECK(up->pass().has_value());
    CHECK(!up->saveError().empty());
    CHECK(up->pass().has_value()); // the URL in memory does not skip the save
    CHECK(server.creates == 1);
    CHECK(server.received == 0);
    SECTION("the save works again")
    {
        fs::remove(dir / "uploads.txt.tmp");
        CHECK(!up->pass());
        CHECK(up->saveError().empty());
        CHECK(server.creates == 1);
    }
    SECTION("a restart")
    {
        up.reset();
        fs::remove(dir / "uploads.txt.tmp");
        CHECK(!uploader(dir, server)->pass());
        CHECK(server.creates == 2); // the first upload holds nothing
    }
    CHECK(server.received == 500);
    CHECK(server.uploads.rbegin()->second.data == readAll(file));
    fs::remove_all(dir);
}

TEST_CASE("outages only delay, and an upload the server never kept starts over")
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

    server.down = false;
    server.uploads.clear();
    CHECK(!up->pass()); // 404, and a close answers 404: Mogsafe holds none of it
    CHECK(!up->pass()); // a new upload of the whole file
    CHECK(server.creates == 2);
    CHECK(server.uploads.rbegin()->second.data == readAll(file));
    CHECK(up->status()[0].error.empty());
}

namespace
{

// Mogsafe's link endpoints in memory. start checks the signature as Mogsafe does; the test plays the player who approves.
class FakeLink final : public Http
{
public:
    enum class Code
    {
        Pending,
        Approved,
        Expired,
        Refused,
    };

    Code        state = Code::Pending;
    std::string signedKey;

    auto request([[maybe_unused]] const std::string& method, const std::string& url, [[maybe_unused]] const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        const auto json = nlohmann::json::parse(std::string(body.begin(), body.end()));
        if (url.ends_with("/v1/link/start"))
        {
            const auto key     = json.at("key").get<std::string>();
            const auto message = std::format("memoria-link\n{}\n{}", key, json.at("time").get<uint64_t>());
            const auto sig     = fromHex(json.at("sig").get<std::string>());
            const auto pub     = fromHex(key);
            if (sig.size() != 64 || pub.size() != 32 || crypto_ed25519_check(sig.data(), pub.data(), reinterpret_cast<const uint8_t*>(message.data()), message.size()) != 0)
            {
                return HttpResponse{ .status = 400 };
            }
            signedKey = key;
            return HttpResponse{ .status = 201, .body = R"({"code":"ABCD-2345","poll":"p0ll","url":"https://mogsafe.test/link?code=ABCD-2345","interval":1,"expires":600})" };
        }
        if (json.at("poll") != "p0ll")
        {
            return HttpResponse{ .status = 410 };
        }
        switch (state)
        {
            case Code::Pending:
                return HttpResponse{ .status = 202, .body = R"({"status":"pending"})" };
            case Code::Approved:
                return HttpResponse{ .status = 200, .body = R"({"token":"t0ken","account":"sruon"})" };
            case Code::Expired:
                return HttpResponse{ .status = 410, .body = R"({"status":"expired"})" };
            case Code::Refused:
                return HttpResponse{ .status = 403, .body = R"({"status":"refused","reason":"linking is not open to you yet"})" };
        }
        return HttpResponse{ .status = 500 };
    }

private:
    static auto fromHex(const std::string& hex) -> std::vector<uint8_t>
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
        {
            out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
        return out;
    }
};

} // namespace

TEST_CASE("a link starts with a signature by the install key and ends with a token")
{
    FakeLink          mogsafe;
    xicap::SecretSeed seed{};
    seed.fill(7);

    const auto code = startLink(mogsafe, "https://mogsafe.test", seed, 1'790'000'000);
    REQUIRE(code.has_value());
    CHECK(code->code == "ABCD-2345");
    CHECK(code->interval == std::chrono::seconds(1));

    // The key Mogsafe saw is the one xicap signs captures with.
    std::array<uint8_t, 64> secret{};
    std::array<uint8_t, 32> pub{};
    auto                    copy = seed;
    crypto_ed25519_key_pair(secret.data(), pub.data(), copy.data());
    CHECK(mogsafe.signedKey == picosha2::bytes_to_hex_string(pub));

    const auto pending = pollLink(mogsafe, "https://mogsafe.test", code->poll);
    REQUIRE(pending.has_value());
    CHECK(!pending->has_value());

    mogsafe.state     = FakeLink::Code::Approved;
    const auto linked = pollLink(mogsafe, "https://mogsafe.test", code->poll);
    REQUIRE(linked.has_value());
    REQUIRE(linked->has_value());
    CHECK((*linked)->token == "t0ken");
    CHECK((*linked)->account == "sruon");
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
    REQUIRE(saveLink(path, { .token = "t0ken", .account = "sruon" }));
    const auto link = loadLink(path);
    REQUIRE(link);
    CHECK(link->token == "t0ken");
    CHECK(link->account == "sruon");
    fs::remove_all(dir);
}

TEST_CASE("only the refused token is deleted")
{
    const auto dir  = tempDir();
    const auto path = dir / "token";
    CHECK(!removeLink(path, "t0ken"));
    REQUIRE(saveLink(path, { .token = "n3w", .account = "sruon" }));
    CHECK(!removeLink(path, "t0ken"));
    REQUIRE(loadLink(path));
    CHECK(loadLink(path)->token == "n3w");
    CHECK(removeLink(path, "n3w"));
    CHECK(!fs::exists(path));
    CHECK(fs::is_empty(dir));
    fs::remove_all(dir);
}

TEST_CASE("an uploader stops without waiting for a request in flight")
{
    // A request that hangs, as one can through an outage, until cancelled.
    class Stuck final : public Http
    {
    public:
        std::atomic<bool> waiting{ false };

        auto request(const std::string&, const std::string&, const HttpHeaders&, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            std::unique_lock lock(mutex_);
            waiting = true;
            wake_.wait_for(lock, std::chrono::seconds(60), [this]
                           {
                               return cancelled_;
                           });
            return std::unexpected("cancelled");
        }

        auto cancel() -> void override
        {
            {
                std::lock_guard lock(mutex_);
                cancelled_ = true;
            }
            wake_.notify_all();
        }

    private:
        std::mutex              mutex_;
        std::condition_variable wake_;
        bool                    cancelled_{ false };
    };
    const auto dir = tempDir();
    saveLink(dir / "config" / "token", { .token = "t0ken", .account = "sruon" });
    append(dir / "a.xicap", std::string(500, 'x'));
    auto  stuck = std::make_unique<Stuck>();
    auto& http  = *stuck;
    auto  up    = std::make_unique<Uploader>(UploadOptions{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config" }, std::move(stuck));
    up->start();
    for (int i = 0; i < 500 && !http.waiting; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(http.waiting);
    const auto before = std::chrono::steady_clock::now();
    up.reset();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(5));
    fs::remove_all(dir);
}

TEST_CASE("a refused token is forgotten, and sending stops until a new link")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    server.token = "revoked";
    auto up      = uploader(dir, server);
    append(dir / "a.xicap", std::string(500, 'x'));
    CHECK(up->pass().has_value());
    CHECK(up->unauthorized());
    CHECK(!up->unauthorized()); // said once
    CHECK(!fs::exists(dir / "config" / "token"));

    const auto requests = server.requests;
    CHECK(!up->pass());
    CHECK(server.requests == requests);

    saveLink(dir / "config" / "token", { .token = "revoked", .account = "sruon" });
    CHECK(!up->pass());
    CHECK(server.uploads.begin()->second.data == readAll(dir / "a.xicap"));
    fs::remove_all(dir);
}

TEST_CASE("pause, unlink, and a new link govern every client sharing the install")
{
    const auto  dir    = tempDir();
    const auto  file   = dir / "a.xicap";
    const auto  token  = dir / "config" / "token";
    const auto  paused = dir / "config" / "paused";
    FakeMogsafe server;
    auto        first  = uploader(dir, server);
    auto        second = uploader(dir, server);

    append(file, std::string(500, 'x'));
    std::ofstream{ paused }; // /memoria upload off in a client that is not uploading
    CHECK(!first->pass());
    first.reset(); // the uploading client exits, and the other takes the upload mutex
    CHECK(!second->pass());
    CHECK(server.requests == 0);

    fs::remove(paused);
    CHECK(!second->pass());
    CHECK(server.uploads.begin()->second.data.size() == 500);

    fs::remove(token); // /memoria unlink in another client
    append(file, std::string(100, 'x'));
    CHECK(!second->pass());
    CHECK(server.uploads.begin()->second.data.size() == 500);

    server.token = "t1"; // /memoria link in another client
    saveLink(token, { .token = "t1", .account = "sruon" });
    CHECK(!second->pass());
    CHECK(server.uploads.begin()->second.data == readAll(file));
    fs::remove_all(dir);
}

TEST_CASE("a refusal of an old token keeps the token another client just saved")
{
    // Another client links again while this one's request with the old token is in flight.
    class Relink final : public Http
    {
    public:
        Relink(FakeMogsafe& to, fs::path token)
        : to_(to)
        , token_(std::move(token))
        {
        }

        auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
        {
            if (to_.token == "t0ken")
            {
                to_.token = "t1";
                saveLink(token_, { .token = "t1", .account = "sruon" });
            }
            return to_.request(method, url, headers, body);
        }

    private:
        FakeMogsafe& to_;
        fs::path     token_;
    };
    const auto  dir = tempDir();
    FakeMogsafe server;
    saveLink(dir / "config" / "token", { .token = "t0ken", .account = "sruon" });
    Uploader up(UploadOptions{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config", .idle = std::chrono::milliseconds(0) }, std::make_unique<Relink>(server, dir / "config" / "token"));
    append(dir / "a.xicap", std::string(500, 'x'));
    CHECK(up.pass().has_value()); // 401 for the old token
    CHECK(!up.unauthorized());
    REQUIRE(loadLink(dir / "config" / "token"));
    CHECK(loadLink(dir / "config" / "token")->token == "t1");
    CHECK(!up.pass());
    CHECK(server.uploads.begin()->second.data == readAll(dir / "a.xicap"));
    fs::remove_all(dir);
}

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
            uint32_t length = 0;
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

auto outPacket(const Event& e) -> xicap::OutPacket
{
    return { .timestamp = e.timestamp, .direction = e.clientToServer ? xicap::PacketDirection::ClientToServer : xicap::PacketDirection::ServerToClient, .data = std::as_bytes(std::span{ e.data }), .channel = e.channel, .opcode = e.opcode, .provenance = e.provenance };
}

} // namespace

TEST_CASE("a live frame is version, channel, flags, time, length, then the packet")
{
    auto e           = gamePacket(0x0102030405060708ULL, 0x015);
    e.clientToServer = true;
    e.provenance     = xicap::PacketFlag::Injected;
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
    CHECK(liveFrame(outPacket(e)) == want);
}

TEST_CASE("live frames hold what the capture holds, so chat goes out stubbed")
{
    const auto dir = tempDir();
    {
        Recorder   rec(options(dir));
        const auto owner = rec.live().claim();
        rec.live().open(owner, true);
        rec.login(T0, who("Nouurs", 1));
        rec.packet(gamePacket(T0 + 1, 0x017)); // chat to the client
        auto said           = gamePacket(T0 + 2, 0x0B5);
        said.clientToServer = true;
        rec.packet(std::move(said));
        rec.packet(gamePacket(T0 + 3, 0x015));
        rec.packet({ .timestamp = T0 + 4, .clientToServer = true, .channel = 1, .opcode = 0x05, .data = std::vector<uint8_t>(40, 0xCD) });
        rec.stop();

        const auto live = frames(rec.live().take(owner));
        REQUIRE(live.size() == 4);
        CHECK(live[0].data.size() == 4);
        CHECK((live[0].flags & xicap::PacketFlag::Excluded) != 0);
        CHECK(live[1].data.size() == 4);
        CHECK(live[1].flags == (xicap::PacketFlag::Excluded | xicap::PacketFlag::ClientToServer));
        CHECK(live[2].data.size() == 16);
        CHECK(live[3].channel == 1);

        xicap::Reader     r(captures(dir)[0]);
        xicap::ReadPacket p{};
        for (const auto& f : live)
        {
            REQUIRE(r.next(p));
            CHECK(f.channel == p.channel);
            CHECK(f.flags == p.flags);
            CHECK(f.timestamp == p.timestamp);
            CHECK(std::ranges::equal(std::as_bytes(std::span{ f.data }), p.data));
        }
    }
    fs::remove_all(dir);
}

TEST_CASE("the live feed batches frames into messages and takes nothing while closed")
{
    LiveFeed   feed;
    const auto owner = feed.claim();
    feed.push(outPacket(gamePacket(T0)));
    CHECK(feed.take(owner).empty());

    feed.open(owner, true);
    for (int i = 0; i < 3; ++i)
    {
        feed.push(outPacket(gamePacket(T0 + i)));
    }
    auto messages = feed.take(owner);
    REQUIRE(messages.size() == 1);
    CHECK(frames(messages).size() == 3);
    CHECK(feed.take(owner).empty());

    // 5,000 frames of 32 bytes do not fit one 64 KiB message.
    for (int i = 0; i < 5000; ++i)
    {
        feed.push(outPacket(gamePacket(T0 + i)));
    }
    messages = feed.take(owner);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].size() <= LiveFeed::MESSAGE_BYTES);
    CHECK(frames(messages).size() == 5000);

    feed.push(outPacket(gamePacket(T0)));
    feed.open(owner, false);
    CHECK(feed.take(owner).empty());
}

TEST_CASE("a full live feed drops packets and counts them")
{
    LiveFeed   feed(3 * 32);
    const auto owner = feed.claim();
    feed.open(owner, true);
    for (int i = 0; i < 5; ++i)
    {
        feed.push(outPacket(gamePacket(T0 + i)));
    }
    CHECK(feed.takeDropped(owner) == 2);
    CHECK(feed.takeDropped(owner) == 0);
    CHECK(frames(feed.take(owner)).size() == 3);
    feed.push(outPacket(gamePacket(T0)));
    CHECK(feed.takeDropped(owner) == 0);

    const std::vector<std::byte> stub(2); // a game packet the policy cannot apply to
    feed.push({ .timestamp = T0, .direction = xicap::PacketDirection::ServerToClient, .data = stub });
    CHECK(feed.takeDropped(owner) == 1);
}

TEST_CASE("a live link stopped late cannot close the feed of the one that replaced it")
{
    LiveFeed   feed;
    const auto old = feed.claim();
    feed.open(old, true);
    const auto now = feed.claim(); // /memoria live off, then /memoria live, while the old send still hangs
    feed.open(now, true);
    feed.open(old, false); // the old link's send returns, and it closes what it thinks is its feed
    feed.push(outPacket(gamePacket(T0)));
    CHECK(feed.take(old).empty());
    CHECK(frames(feed.take(now)).size() == 1);
}

TEST_CASE("a live link comes from Mogsafe with the install's token")
{
    struct Live final : Http
    {
        std::string auth;
        int         status = 201;

        auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            CHECK(method == "POST");
            CHECK(url == "https://mogsafe.test/v1/live");
            auth = headers.at(0).second;
            return HttpResponse{ .status = status, .body = R"({"token":"l1ve","url":"https://lens.sruon.dev/live#l1ve","expires":1790000000000})" };
        }
    } mogsafe;
    const auto link = startLive(mogsafe, "https://mogsafe.test", "t0ken");
    REQUIRE(link.has_value());
    CHECK(mogsafe.auth == "Bearer t0ken");
    CHECK(link->token == "l1ve");
    CHECK(link->url == "https://lens.sruon.dev/live#l1ve");

    mogsafe.status = 401;
    CHECK(startLive(mogsafe, "https://mogsafe.test", "t0ken").error() == "Mogsafe answered 401");

    CHECK(liveViewers(R"({"viewers":2})") == 2);
    CHECK(!liveViewers(R"({"sender":true})"));
    CHECK(!liveViewers("not json"));
    CHECK(nlohmann::json::parse(liveHello("Nouurs", "Asura", "30261006_0", "Memoria 0.6.0")) ==
          nlohmann::json::parse(R"({"hello":{"character":"Nouurs","world":"Asura","client_version":"30261006_0","agent":"Memoria 0.6.0"}})"));
}

namespace
{

constexpr auto MANIFEST = "StartTime = 2026-08-04 23:54:00\n";

// A folder as captain's PacketLogger leaves it, last written age ago.
auto captainFolder(const fs::path& root, const std::string& rel, const std::string& log, const std::string& manifest = MANIFEST, std::chrono::minutes age = std::chrono::minutes(20)) -> fs::path
{
    const auto dir = root / rel;
    fs::create_directories(dir / "PacketLogger");
    std::ofstream(dir / "manifest.txt", std::ios::binary) << manifest;
    std::ofstream(dir / "PacketLogger" / "full.log", std::ios::binary) << log;
    fs::last_write_time(dir / "PacketLogger" / "full.log", fs::file_time_type::clock::now() - age);
    return dir;
}

auto noise(size_t n) -> std::string
{
    std::mt19937 random(42);
    std::string  out(n, '\0');
    for (auto& c : out)
    {
        c = static_cast<char>(random());
    }
    return out;
}

auto unzstd(const std::vector<uint8_t>& in) -> std::string
{
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> d(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    std::string                                          out;
    std::vector<char>                                    buf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer                                        input{ in.data(), in.size(), 0 };
    for (size_t left = 1; left != 0;)
    {
        ZSTD_outBuffer o{ buf.data(), buf.size(), 0 };
        left = ZSTD_decompressStream(d.get(), &o, &input);
        if (ZSTD_isError(left) || (o.pos == 0 && input.pos == input.size))
        {
            return "<not a whole zstd frame>";
        }
        out.append(buf.data(), o.pos);
    }
    return out;
}

// <dir>/ashita holds captain's folders and Memoria's state; <dir>/config the install's token.
struct Install
{
    fs::path                 dir = tempDir();
    fs::path                 captures{ dir / "ashita" / "addons" / "captain" / "captures" };
    fs::path                 uploaded{ dir / "ashita" / "addons" / "captain" / "captures-uploaded" };
    fs::path                 state{ dir / "ashita" / "captures" / "memoria" };
    fs::path                 config{ dir / "config" };
    std::vector<std::string> said;
    std::mutex               mutex;

    Install()
    {
        fs::create_directories(captures);
        saveLink(config / "token", { .token = "t0ken", .account = "sruon" });
    }

    ~Install()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    Install(const Install&)                    = delete;
    auto operator=(const Install&) -> Install& = delete;

    auto options(std::chrono::milliseconds retry = std::chrono::milliseconds(1)) -> ImportOptions
    {
        ImportOptions options{ .captures = captures, .uploaded = uploaded, .state = state, .server = "https://mogsafe.test", .config = config, .retry = retry };
        options.say = [this](std::string text)
        {
            std::lock_guard lock(mutex);
            said.push_back(std::move(text));
        };
        return options;
    }

    auto importer(FakeMogsafe& server, std::chrono::milliseconds retry = std::chrono::milliseconds(1)) -> std::unique_ptr<Importer>
    {
        return std::make_unique<Importer>(options(retry), std::make_unique<Forward>(server));
    }

    auto saidAny(std::string_view text) -> bool
    {
        std::lock_guard lock(mutex);
        return std::ranges::any_of(said, [&](const auto& s)
                                   {
                                       return s.find(text) != std::string::npos;
                                   });
    }
};

auto waitFor(const std::function<bool()>& done) -> bool
{
    for (int i = 0; i < 1000 && !done(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

} // namespace

TEST_CASE("captain captures are found at any depth, and not inside one")
{
    Install in;
    captainFolder(in.captures, "Mining/test_2026-8-4/Audithor", "log");
    captainFolder(in.captures, "2026-8-4_23_54/Audithor", "log");
    captainFolder(in.captures, "2026-8-4_23_54/Audithor/inner/Nested", "log"); // inside a match: not searched
    fs::create_directories(in.captures / "2026-8-5/Audithor");                 // a session made with Memoria
    std::ofstream(in.captures / "2026-8-5/Audithor/manifest.txt") << MANIFEST;
    std::ofstream(in.captures / "2026-8-5/Audithor/memoria.xicap") << "x";
    fs::create_directories(in.captures / "tracker"); // an addon's own data
    std::ofstream(in.captures / "tracker/data.json") << "{}";

    const auto found = findCaptain(in.captures);
    REQUIRE(found.size() == 2);
    CHECK(found[0].folder == "2026-8-4_23_54/Audithor");
    CHECK(found[1].folder == "Mining/test_2026-8-4/Audithor");
    CHECK(found[1].character == "Audithor");
    CHECK(found[1].size == 3);
    CHECK(!found[1].growing);
    CHECK(findCaptain(in.dir / "no such folder").empty());
}

TEST_CASE("a full.log changed in the last 10 minutes is left for later")
{
    Install in;
    captainFolder(in.captures, "a/Audithor", "log", MANIFEST, std::chrono::minutes(2));
    captainFolder(in.captures, "b/Audithor", "log", MANIFEST, std::chrono::minutes(11));
    const auto found = findCaptain(in.captures);
    REQUIRE(found.size() == 2);
    CHECK(found[0].growing);
    CHECK(!found[1].growing);

    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/2 a/Audithor: still being written; skipped."));
    CHECK(fs::exists(in.captures / "a/Audithor"));
    CHECK(fs::exists(in.uploaded / "b/Audithor"));
    CHECK(server.creates == 1);
}

TEST_CASE("CRC-32 matches what Mogsafe's import page computes")
{
    Install           in;
    std::atomic<bool> stop{ false };
    std::string       log;
    for (int i = 0; i < 1000; ++i)
    {
        log += "[2026-08-04 23:54:01] Incoming packet 0x00A\n|  00 01 02 03 |\n";
    }
    std::ofstream(in.dir / "check", std::ios::binary) << "123456789";
    std::ofstream(in.dir / "full.log", std::ios::binary) << log;
    // The values Mogsafe's zip.js crc32Of, which browse.html calls, gives for the same bytes.
    CHECK(crc32File(in.dir / "check", stop)->crc == 0xcbf43926);
    const auto crc = crc32File(in.dir / "full.log", stop);
    REQUIRE(crc);
    CHECK(crc->crc == 0x94175684);
    CHECK(crc->size == 61000);
    CHECK(!crc32File(in.dir / "missing", stop));
    stop = true;
    CHECK(!crc32File(in.dir / "full.log", stop));
}

TEST_CASE("a captain import sends full.log compressed with its manifest, then moves the folder")
{
    Install    in;
    const auto log = std::string(5000, 'p') + noise(3000);
    const auto dir = captainFolder(in.captures, "2026-8-4_23_54/Audithor", log);
    std::ofstream(dir / "memoria.xicap") << "not sent";
    fs::create_directories(dir / "PacketLogger" / "incoming");
    std::ofstream(dir / "PacketLogger" / "incoming" / "0x00A.log") << "not sent";
    FakeMogsafe server;
    in.importer(server)->run();

    REQUIRE(server.creates == 1);
    auto& u = server.uploads.begin()->second;
    CHECK(u.closed);
    CHECK(unzstd(u.data) == log);
    CHECK(u.meta["format"] == "captain");
    CHECK(u.meta["manifest"] == MANIFEST);
    CHECK(u.meta["character"] == "Audithor");
    CHECK(u.meta["folder"] == "2026-8-4_23_54/Audithor");
    CHECK(u.meta["size"] == "8000");
    CHECK(u.meta["crc32"].size() == 8);
    CHECK(!u.meta.contains("zip"));

    CHECK(!fs::exists(in.captures / "2026-8-4_23_54")); // emptied parent removed
    CHECK(fs::exists(in.captures));
    CHECK(fs::exists(in.uploaded / "2026-8-4_23_54/Audithor/PacketLogger/incoming/0x00A.log"));
    CHECK(!Importer::pending(in.state));
    CHECK(!fs::exists(in.state / "imports"));
    CHECK(in.said.front() == "Uploading 1 captain capture (7.8 KB) in the background.");
    CHECK(in.saidAny("Captain 1/1 2026-8-4_23_54/Audithor: uploaded."));
    CHECK(in.said.back() == "Captain import done: 1 uploaded, 0 already in Vault, 0 refused. Uploaded folders are in addons/captain/captures-uploaded.");

    // Moved folders are never looked at again.
    const auto requests = server.requests;
    in.importer(server)->run();
    CHECK(server.requests == requests);
    CHECK(in.saidAny("No captain captures to upload."));
}

TEST_CASE("a log already in Vault sends no bytes and moves")
{
    Install    in;
    const auto log = std::string("[2026-08-04] x\n");
    captainFolder(in.captures, "s/Audithor", log);
    FakeMogsafe       server;
    std::atomic<bool> stop{ false };
    server.imported[std::format("{:08x}:{}", crc32File(in.captures / "s/Audithor/PacketLogger/full.log", stop)->crc, log.size())] = "cap7";
    in.importer(server)->run();
    CHECK(server.creates == 0);
    CHECK(server.received == 0);
    CHECK(fs::exists(in.uploaded / "s/Audithor"));
    CHECK(in.saidAny("Captain 1/1 s/Audithor: already in Vault (cap7)."));
}

TEST_CASE("a refused capture stays where it is and the next one goes")
{
    Install in;
    captainFolder(in.captures, "a/Audithor", "log a", "Character = Audithor\n"); // no StartTime
    captainFolder(in.captures, "b/Audithor", "log b");
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/2 a/Audithor: refused: manifest.txt has no StartTime."));
    CHECK(fs::exists(in.captures / "a/Audithor"));
    CHECK(fs::exists(in.uploaded / "b/Audithor"));

    // A first append Mogsafe refuses (422) is a refusal too.
    captainFolder(in.captures, "d/Audithor", "log d");
    server.forced["PATCH"] = { 422 };
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 2/2 d/Audithor: refused: forced."));
    CHECK(fs::exists(in.captures / "d/Audithor"));
    CHECK(in.said.back().starts_with("Captain import done: 0 uploaded, 0 already in Vault, 2 refused."));
}

TEST_CASE("an upload Mogsafe lost, at HEAD or at an append, starts over")
{
    for (const auto& [method, status] : { std::pair{ "HEAD", 404 }, std::pair{ "PATCH", 410 } })
    {
        Install    in;
        const auto log = noise(3000);
        captainFolder(in.captures, "s/Audithor", log);
        FakeMogsafe server;
        server.forced[method] = { status };
        in.importer(server)->run();
        CHECK(server.creates == 2);
        CHECK(unzstd(server.uploads.rbegin()->second.data) == log);
        CHECK(fs::exists(in.uploaded / "s/Audithor"));
    }
}

TEST_CASE("server errors back off, and the import never gives up")
{
    Install    in;
    const auto log = noise(3000);
    captainFolder(in.captures, "s/Audithor", log);
    FakeMogsafe server;
    server.forced["POST"] = { 503, 500, 502 }; // creates
    server.forced["HEAD"] = { 502 };
    const auto before     = std::chrono::steady_clock::now();
    in.importer(server, std::chrono::milliseconds(20))->run();
    CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(20 + 40 + 80 + 160));
    CHECK(unzstd(server.uploads.begin()->second.data) == log);
    CHECK(fs::exists(in.uploaded / "s/Audithor"));
}

TEST_CASE("a refused token stops the import, unlinks, and the import resumes after a new link")
{
    Install in;
    captainFolder(in.captures, "s/Audithor", "log");
    FakeMogsafe server;
    server.token = "revoked";
    auto imp     = in.importer(server);
    imp->run();
    CHECK(imp->unauthorized());
    CHECK(!fs::exists(in.config / "token"));
    CHECK(in.saidAny("this install is not linked"));
    CHECK(fs::exists(in.captures / "s/Audithor"));
    CHECK(Importer::pending(in.state));

    server.token = "t1";
    saveLink(in.config / "token", { .token = "t1", .account = "sruon" });
    in.importer(server)->run();
    CHECK(fs::exists(in.uploaded / "s/Audithor"));
    CHECK(!Importer::pending(in.state));
}

TEST_CASE("an unlinked import sends nothing")
{
    Install in;
    captainFolder(in.captures, "s/Audithor", "log");
    fs::remove(in.config / "token");
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(server.requests == 0);
    CHECK(fs::exists(in.captures / "s/Audithor"));
}

TEST_CASE("a pause stops the import between chunks until uploads are on")
{
    Install    in;
    const auto log = noise(3 << 20); // three appends once compressed
    captainFolder(in.captures, "s/Audithor", log);
    FakeMogsafe      server;
    std::atomic<int> patches{ 0 };
    server.onPatch = [&]
    {
        if (++patches == 1)
        {
            std::ofstream{ in.config / "paused" };
        }
    };
    auto imp = in.importer(server);
    imp->start();
    REQUIRE(waitFor([&]
                    {
                        return patches > 0;
                    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(patches == 1);
    CHECK(!imp->finished());
    const auto p = imp->progress();
    CHECK(p.index == 1);
    CHECK(p.count == 1);
    CHECK(p.character == "Audithor");
    CHECK(p.sent == Tus::CHUNK_SIZE);
    CHECK(p.size > 2 * Tus::CHUNK_SIZE);

    fs::remove(in.config / "paused");
    CHECK(waitFor([&]
                  {
                      return imp->finished();
                  }));
    CHECK(unzstd(server.uploads.begin()->second.data) == log);
}

TEST_CASE("an import resumes from each point a crash can leave it at")
{
    const auto log = noise(3 << 20);
    SECTION("asked for, before the first create")
    {
        Install in;
        captainFolder(in.captures, "s/Audithor", log);
        fs::create_directories(in.state);
        std::ofstream{ in.state / "imports.txt" };
        CHECK(Importer::pending(in.state));
        FakeMogsafe server;
        in.importer(server)->run();
        CHECK(server.creates == 1);
        CHECK(fs::exists(in.uploaded / "s/Audithor"));
    }
    SECTION("after the create, before the first byte, and after some appends")
    {
        for (const int crashAt : { 0, 1 })
        {
            Install in;
            captainFolder(in.captures, "s/Audithor", log);
            FakeMogsafe server;
            server.crashAtPatch = crashAt;
            CHECK_THROWS(in.importer(server)->run());
            CHECK(Importer::pending(in.state));
            server.crashAtPatch = -1;
            in.importer(server)->run();
            CHECK(server.creates == 1);
            const auto& u = server.uploads.begin()->second;
            CHECK(server.received == u.data.size()); // no byte twice
            CHECK(unzstd(u.data) == log);
            CHECK(fs::exists(in.uploaded / "s/Audithor"));
            CHECK(!Importer::pending(in.state));
        }
    }
    SECTION("closed, before the move")
    {
        Install in;
        captainFolder(in.captures, "s/Audithor", log);
        FakeMogsafe server;
        server.crashAfterClose = true;
        CHECK_THROWS(in.importer(server)->run());
        CHECK(fs::exists(in.captures / "s/Audithor"));
        server.crashAfterClose = false;
        const auto received    = server.received;
        in.importer(server)->run();
        CHECK(server.received == received);
        CHECK(server.creates == 1);
        CHECK(in.saidAny("s/Audithor: already in Vault (cap1)."));
        CHECK(fs::exists(in.uploaded / "s/Audithor"));
        CHECK(fs::is_empty(in.state));
    }
}

namespace
{

auto searchFolder(const fs::path& dir, const std::string& search) -> void
{
    fs::create_directories(dir / "SearchLog");
    std::ofstream(dir / "SearchLog" / "search.log", std::ios::binary) << search;
}

// The two zstd frames an upload with a search log holds: search.log's, then full.log's.
auto searchParts(const FakeMogsafe::Upload& u) -> std::pair<std::string, std::string>
{
    const auto n = static_cast<size_t>(std::stoull(u.meta.at("search_bytes")));
    if (n > u.data.size())
    {
        return {};
    }
    const auto mid = u.data.begin() + static_cast<std::ptrdiff_t>(n);
    return { unzstd(std::vector<uint8_t>(u.data.begin(), mid)), unzstd(std::vector<uint8_t>(mid, u.data.end())) };
}

} // namespace

TEST_CASE("a folder's search log goes ahead of its full.log in the same upload, and an empty one does not")
{
    Install    in;
    const auto log    = noise(3000);
    const auto search = std::string("[2026-10-21 18:44:13.740] Outgoing search packet 0x15\n") + noise(2000);
    searchFolder(captainFolder(in.captures, "a/Hoa", log), search);
    searchFolder(captainFolder(in.captures, "b/Ijs", log + "b"), "");
    FakeMogsafe server;
    in.importer(server)->run();

    REQUIRE(server.creates == 2);
    const auto& u = server.uploads.at("/v1/uploads/1");
    CHECK(u.closed);
    std::atomic<bool> stop{ false };
    CHECK(u.meta.at("search_crc32") == std::format("{:08x}", crc32File(in.uploaded / "a/Hoa/SearchLog/search.log", stop)->crc));
    CHECK(u.meta.at("search_size") == std::to_string(search.size()));
    CHECK(!u.meta.contains("search_encoding"));
    CHECK(searchParts(u) == std::pair{ search, log });
    CHECK(in.said.front() == std::format("Uploading 2 captain captures ({}) in the background.", sizeText(2 * log.size() + 1 + search.size())));
    CHECK(in.saidAny("Captain 1/2 a/Hoa: uploaded with its search log."));

    const auto& empty = server.uploads.at("/v1/uploads/2");
    CHECK(!empty.meta.contains("search_bytes"));
    CHECK(unzstd(empty.data) == log + "b");
    CHECK(in.saidAny("Captain 2/2 b/Ijs: uploaded."));
    CHECK(!fs::exists(in.state / "imports"));
}

TEST_CASE("a folder in Vault without its search log replaces its capture; without one it stays in Vault")
{
    Install    in;
    const auto log = noise(3000);
    searchFolder(captainFolder(in.captures, "a/Hoa", log), "search");
    captainFolder(in.captures, "b/Ijs", log + "b");
    FakeMogsafe       server;
    std::atomic<bool> stop{ false };
    const auto        key = [&](const std::string& rel)
    {
        return std::format("{:08x}:{}", crc32File(in.captures / rel / "PacketLogger" / "full.log", stop)->crc, fs::file_size(in.captures / rel / "PacketLogger" / "full.log"));
    };
    server.imported[key("a/Hoa")] = "cap7";
    server.imported[key("b/Ijs")] = "cap8";
    server.replaceable            = { "cap7", "cap8" };
    in.importer(server)->run();
    CHECK(server.creates == 1);
    CHECK(searchParts(server.uploads.at("/v1/uploads/1")).first == "search");
    CHECK(in.saidAny("Captain 1/2 a/Hoa: uploaded with its search log, replacing cap7."));
    CHECK(in.saidAny("Captain 2/2 b/Ijs: already in Vault (cap8)."));
    CHECK(fs::exists(in.uploaded / "a/Hoa/SearchLog/search.log"));
    CHECK(fs::exists(in.uploaded / "b/Ijs"));
}

TEST_CASE("an import with a search log resumes across its two parts, and starts over when the search stage is gone")
{
    const auto log    = noise(3 << 20);
    const auto search = noise((1 << 20) + 300'000); // past the first 1 MiB append
    for (const int crashAt : { 0, 1, 2 })           // before the first byte, inside the search part, inside full.log's
    {
        Install in;
        searchFolder(captainFolder(in.captures, "s/Hoa", log), search);
        FakeMogsafe server;
        server.crashAtPatch = crashAt;
        CHECK_THROWS(in.importer(server)->run());
        CHECK(Importer::pending(in.state));
        server.crashAtPatch = -1;
        in.importer(server)->run();
        CHECK(server.creates == 1);
        const auto& u = server.uploads.begin()->second;
        CHECK(server.received == u.data.size()); // no byte twice
        CHECK(searchParts(u) == std::pair{ search, log });
        CHECK(fs::exists(in.uploaded / "s/Hoa"));
        CHECK(!Importer::pending(in.state));
    }

    // Created, then the staged files are lost before the first byte: the search part's length may differ, so a new upload goes.
    Install in;
    searchFolder(captainFolder(in.captures, "s/Hoa", log), search);
    FakeMogsafe server;
    server.crashAtPatch = 0;
    CHECK_THROWS(in.importer(server)->run());
    fs::remove_all(in.state / "imports");
    server.crashAtPatch = -1;
    in.importer(server)->run();
    CHECK(server.creates == 2);
    CHECK(searchParts(server.uploads.at("/v1/uploads/2")) == std::pair{ search, log });
    CHECK(fs::exists(in.uploaded / "s/Hoa"));
}

TEST_CASE("an import whose state cannot be saved sends nothing and goes on at the next start")
{
    Install    in;
    const auto log = noise(3 << 20);
    captainFolder(in.captures, "s/Audithor", log);
    fs::create_directories(in.state / "imports.txt.tmp");
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain import stopped: cannot write "));
    CHECK(server.requests == 0);
    fs::remove(in.state / "imports.txt.tmp");

    server.onCreate = [&]
    {
        fs::create_directory(in.state / "imports.txt.tmp");
    };
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/1 s/Audithor: cannot write "));
    CHECK(server.creates == 1);
    CHECK(server.received == 0);
    CHECK(Importer::pending(in.state));
    CHECK(fs::exists(in.captures / "s/Audithor"));

    server.onCreate = nullptr;
    fs::remove(in.state / "imports.txt.tmp");
    in.importer(server)->run(); // the plugin resumes at the next start
    CHECK(server.creates == 2);
    const auto& u = server.uploads.rbegin()->second;
    CHECK(server.received == u.data.size());
    CHECK(unzstd(u.data) == log);
    CHECK(fs::exists(in.uploaded / "s/Audithor"));
    CHECK(!Importer::pending(in.state));
}

TEST_CASE("an uploaded folder moves to the same path under captures-uploaded")
{
    Install in;
    captainFolder(in.captures, "Mining/test/Audithor", "a");
    captainFolder(in.captures, "Mining/test/Nouurs", "b");
    auto to = moveAside(in.captures, in.uploaded, in.captures / "Mining/test/Audithor");
    REQUIRE(to);
    CHECK(*to == in.uploaded / "Mining/test/Audithor");
    CHECK(fs::exists(in.captures / "Mining/test/Nouurs")); // the parent still holds another capture

    captainFolder(in.captures, "Mining/test/Audithor", "c"); // the same path again: a clash
    to = moveAside(in.captures, in.uploaded, in.captures / "Mining/test/Audithor");
    REQUIRE(to);
    CHECK(to->filename() == "Audithor-2");
    CHECK(fs::exists(in.uploaded / "Mining/test/Audithor/manifest.txt"));

    REQUIRE(moveAside(in.captures, in.uploaded, in.captures / "Mining/test/Nouurs"));
    CHECK(fs::is_empty(in.captures));
}

TEST_CASE("a folder that cannot move stays, and the next import moves it")
{
    Install in;
    captainFolder(in.captures, "s/Audithor", "log");
    std::ofstream{ in.uploaded }; // a file where the folder goes
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/1 s/Audithor: in Vault, but its folder could not move: "));
    CHECK(fs::exists(in.captures / "s/Audithor"));
    CHECK(in.said.back().starts_with("Captain import done: 0 uploaded, 0 already in Vault, 0 refused, 1 in Vault but not moved."));

    fs::remove(in.uploaded);
    in.importer(server)->run();
    CHECK(in.saidAny("already in Vault (cap1)."));
    CHECK(fs::exists(in.uploaded / "s/Audithor"));
}

namespace
{

// Hands requests to a fake, except that one method hangs until cancelled, as a request can through an outage.
class Blocking final : public Http
{
public:
    Blocking(FakeMogsafe& to, std::string method)
    : to_(to)
    , method_(std::move(method))
    {
    }

    std::atomic<bool>            waiting{ false };
    std::atomic<std::thread::id> thread;

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        if (method != method_)
        {
            return to_.request(method, url, headers, body);
        }
        std::unique_lock lock(mutex_);
        thread  = std::this_thread::get_id();
        waiting = true;
        wake_.wait_for(lock, std::chrono::seconds(60), [this]
                       {
                           return cancelled_;
                       });
        return std::unexpected("cancelled");
    }

    auto cancel() -> void override
    {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        wake_.notify_all();
    }

private:
    FakeMogsafe&            to_;
    std::string             method_;
    std::mutex              mutex_;
    std::condition_variable wake_;
    bool                    cancelled_{ false };
};

} // namespace

TEST_CASE("the import runs on its own thread, and stops at once mid-append and mid-compression")
{
    Install     in;
    FakeMogsafe server;
    captainFolder(in.captures, "s/Audithor", noise(3 << 20));
    {
        auto  blocking = std::make_unique<Blocking>(server, "PATCH");
        auto& http     = *blocking;
        auto  imp      = std::make_unique<Importer>(in.options(), std::move(blocking));
        auto  before   = std::chrono::steady_clock::now();
        imp->start();
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
        REQUIRE(waitFor([&]
                        {
                            return http.waiting.load();
                        }));
        CHECK(http.thread.load() != std::this_thread::get_id());
        CHECK(!server.threads.empty());
        CHECK(!server.threads.contains(std::this_thread::get_id()));
        before = std::chrono::steady_clock::now();
        imp.reset();
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
        CHECK(Importer::pending(in.state));
    }

    // Large enough to compress for a while; stop once its .tmp appears.
    fs::rename(in.captures / "s", in.dir / "s");
    captainFolder(in.captures, "t/Audithor", noise(64 << 20));
    auto       imp = std::make_unique<Importer>(in.options(), std::make_unique<Forward>(server));
    const auto tmp = [&]
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(in.state / "imports", ec))
        {
            if (e.path().extension() == ".tmp")
            {
                return true;
            }
        }
        return false;
    };
    imp->start();
    for (int i = 0; i < 5000 && !tmp(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    REQUIRE(tmp());
    const auto before = std::chrono::steady_clock::now();
    imp.reset();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
    CHECK(!tmp()); // a stopped compression leaves no partial file
}

TEST_CASE("the import reminder shows once per install, to one client")
{
    Install in;
    CHECK(!remindImport(in.config, in.captures)); // nothing waiting
    CHECK(!fs::exists(in.config / "import-reminded"));

    captainFolder(in.captures, "s/Audithor", "log");
    fs::rename(in.config / "token", in.dir / "token");
    CHECK(!remindImport(in.config, in.captures)); // not linked
    fs::rename(in.dir / "token", in.config / "token");

    for (int round = 0; round < 5; ++round) // two clients logging in together, then later logins and restarts
    {
        std::atomic<int> shown{ 0 };
        {
            std::latch                go(2);
            std::vector<std::jthread> clients;
            for (int i = 0; i < 2; ++i)
            {
                clients.emplace_back([&]
                                     {
                                         go.arrive_and_wait();
                                         shown += remindImport(in.config, in.captures) ? 1 : 0;
                                     });
            }
        }
        CHECK(shown == (round == 0 ? 1 : 0));
    }
}

#ifdef _WIN32
TEST_CASE("a second client sharing the install does not import while the first does")
{
    Install     in;
    FakeMogsafe first;
    FakeMogsafe second;
    captainFolder(in.captures, "s/Audithor", noise(3 << 20));
    const auto mutex   = std::format("Local\\Memoria-import-test-{}", std::random_device{}());
    auto       options = in.options();
    options.mutex      = mutex;
    auto  blocking     = std::make_unique<Blocking>(first, "PATCH");
    auto& http         = *blocking;
    auto  running      = std::make_unique<Importer>(options, std::move(blocking));
    running->start();
    REQUIRE(waitFor([&]
                    {
                        return http.waiting.load();
                    }));

    for (const bool asked : { true, false }) // /memoria import, then a resume at load
    {
        Install other;
        auto    otherOptions = other.options();
        otherOptions.mutex   = mutex;
        Importer imp(otherOptions, std::make_unique<Forward>(second));
        imp.start(asked);
        REQUIRE(waitFor([&]
                        {
                            return imp.finished();
                        }));
        CHECK(other.saidAny("Another client sharing this install is importing.") == asked);
    }
    CHECK(second.requests == 0);

    running.reset(); // the first client unloads, which frees the run for the next
    Importer next(options, std::make_unique<Forward>(first));
    next.start(false);
    REQUIRE(waitFor([&]
                    {
                        return next.finished();
                    }));
    CHECK(fs::exists(in.uploaded / "s/Audithor"));
}
#endif

TEST_CASE("a file left by a killed client whose upload the server ended is kept and never sent again")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    append(file, std::string(500, 'x'));
    CHECK(!uploader(dir, server)->pass());
    append(file, std::string(100, 'x')); // written before the kill, never sent

    server.uploads.begin()->second.cutOff = true;
    auto up                               = uploader(dir, server);
    CHECK(!up->pass()); // 410
    CHECK(up->ended("a.xicap"));
    CHECK(!up->pass());
    CHECK(!uploader(dir, server)->pass()); // a restart remembers
    CHECK(server.creates == 1);
    CHECK(server.received == 500);
    CHECK(fs::file_size(file) == 600);
    const auto status = up->status();
    REQUIRE(status.size() == 1);
    CHECK(status[0].sent == status[0].size); // nothing left to send
    CHECK(status[0].error.empty());
    CHECK(uploader(dir, server)->ended("a.xicap")); // another client reads it from disk
    fs::remove_all(dir);
}

TEST_CASE("a lost close answer for a capture the server holds complete counts as closed")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    append(file, std::string(500, 'x') + "XCPF");
    CHECK(!uploader(dir, server)->pass());
    server.uploads.begin()->second.closed = true; // closed, its answer lost
    auto up                               = uploader(dir, server);
    CHECK(!up->pass());
    CHECK(!up->ended("a.xicap"));
    CHECK(up->status()[0].sent == 504);
    CHECK(server.creates == 1);
    fs::remove_all(dir);
}

TEST_CASE("a recording whose upload the server ended goes on in a new capture that names it, and sends nothing twice")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    auto        up   = uploader(dir, server);
    int         told = 0;
    auto        opts = options(dir);
    opts.ended       = [&](const fs::path& capture)
    {
        if (!up->ended(capture.filename().string()))
        {
            return false;
        }
        ++told;
        return true;
    };
    Recorder rec(opts);
    rec.login(T0, who("Nouurs", 1));
    rec.packet(gamePacket(T0 + 1));
    fs::path first;
    REQUIRE(waitFor([&]
                    {
                        first = rec.status().capture;
                        std::error_code ec;
                        return !first.empty() && fs::file_size(first, ec) >= xicap::HEADER_SIZE;
                    }));
    CHECK(!up->pass());
    REQUIRE(server.creates == 1);
    auto& cut = server.uploads.begin()->second;
    REQUIRE(!cut.data.empty());

    cut.cutOff     = true; // a network gap longer than the sweep's wait
    const auto had = cut.data.size();
    rec.packet(gamePacket(T0 + 2));
    REQUIRE(waitFor([&]
                    {
                        return rec.status().packets == 2;
                    }));
    CHECK(!up->pass()); // 410
    REQUIRE(waitFor([&]
                    {
                        return rec.status().capture.empty();
                    }));
    rec.packet(gamePacket(T0 + 3));
    REQUIRE(waitFor([&]
                    {
                        return !rec.status().capture.empty();
                    }));
    const auto second = rec.status().capture;
    rec.stop();
    CHECK(!up->pass()); // sends the new capture
    CHECK(!up->pass()); // closes it

    CHECK(told == 1);
    CHECK(second != first);
    CHECK(server.creates == 2);
    CHECK(cut.data.size() == had);
    CHECK(server.uploads.rbegin()->second.data == readAll(second));
    CHECK(server.uploads.rbegin()->second.closed);
    const auto old = readCapture(first);
    const auto now = readCapture(second);
    CHECK(old.complete);
    CHECK(old.packets == 2);
    CHECK(now.packets == 1);
    CHECK(now.previous == old.id);
    CHECK(now.start == xicap::StartContext::MidSession);
    fs::remove_all(dir);
}
