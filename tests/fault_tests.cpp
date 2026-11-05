#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include "support.h"

using namespace memoria;
using namespace memoria::test;

// Faults a player's PC can throw at a recording: each test says what Memoria must keep.

TEST_CASE("a capture cut off at any byte reads to its last whole frame, and uploads whole")
{
    const auto dir = tempDir();
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        for (uint64_t i = 0; i < 6; ++i)
        {
            rec.packet(gamePacket(T0 + 1 + i * 3));
            rec.packet(gamePacket(T0 + 2 + i * 3));
            rec.logout(T0 + 3 + i * 3); // ends the frame
            rec.login(T0 + 3 + i * 3, who("Kalnor", 1));
        }
        rec.stop();
    }
    const auto bytes = readAll(captures(dir)[0]);
    const auto cut   = dir / "cut" / "a.xicap";
    fs::create_directories(cut.parent_path());
    const auto write = [&](size_t n)
    {
        std::ofstream(cut, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(n));
    };
    uint64_t most{ 0 };
    bool     opened{ false };
    for (size_t n = CAPTURE_HEADER_SIZE; n < bytes.size(); ++n)
    {
        write(n);
        uint64_t packets{ 0 };
        try
        {
            xicap::Reader r(cut);
            opened = true;
            CHECK_FALSE(r.complete());
            packets = static_cast<uint64_t>(std::ranges::distance(r));
        }
        catch (const xicap::Error& error)
        {
            INFO(n << ": " << error.what());
            CHECK_FALSE(opened); // a cut that reads its start reads at every longer cut too
        }
        CHECK(packets >= most); // a longer cut never reads less
        most = std::max(most, packets);
    }
    CHECK(opened);
    CHECK(most == 12); // a cut inside the trailer still reads every packet

    // Killed mid-frame: the uploader sends every byte, and stops at a close Mogsafe refuses until the file changes.
    write(CAPTURE_HEADER_SIZE + (bytes.size() - CAPTURE_HEADER_SIZE) / 2);
    FakeMogsafe server;
    auto        up = uploader(cut.parent_path(), server);
    CHECK(!up->pass());
    CHECK(!up->pass()); // close refused: no trailer
    const auto requests = server.requests;
    CHECK(!up->pass());
    CHECK(server.requests == requests + 1); // a HEAD, no close
    CHECK(server.uploads.begin()->second.data == readAll(cut));
    CHECK(up->status()[0].sent == up->status()[0].size);
    fs::remove_all(dir);
}

TEST_CASE("an append cut off mid-body resumes from the bytes the server kept")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    server.cutAtPatch = 1;
    append(file, noise(3 * Tus::CHUNK_SIZE + 77));
    auto up = uploader(dir, server);
    CHECK(up->pass().has_value());
    CHECK(server.uploads.begin()->second.data.size() == Tus::CHUNK_SIZE * 3 / 2);
    CHECK(!up->pass());
    CHECK(server.creates == 1);
    CHECK(server.uploads.begin()->second.data == readAll(file));
    fs::remove_all(dir);
}

TEST_CASE("any failed answer at any step is retried, and the upload ends whole")
{
    for (const auto* method : { "POST", "HEAD", "PATCH" })
    {
        for (const int status : { 0, 408, 429, 500, 502, 503 }) // 0: no answer at all
        {
            INFO(method << " " << status);
            const auto  dir  = tempDir();
            const auto  file = dir / "a.xicap";
            FakeMogsafe server;
            auto        up = uploader(dir, server);
            append(file, std::string(500, 'x'));
            if (status == 0)
            {
                server.down = true;
            }
            else
            {
                server.forced[method] = { status, status };
            }
            CHECK(up->pass().has_value());
            server.down = false;
            for (int i = 0; i < 3; ++i)
            {
                std::ignore = up->pass();
            }
            append(file, TRAILER);
            if (status != 0)
            {
                server.forced["POST"] = { status }; // the close fails once too
            }
            for (int i = 0; i < 4; ++i)
            {
                std::ignore = up->pass();
            }
            CHECK(server.creates == 1);
            REQUIRE(server.uploads.size() == 1);
            CHECK(server.uploads.begin()->second.data == readAll(file));
            CHECK(server.uploads.begin()->second.closed);
            CHECK(!up->pass());
            fs::remove_all(dir);
        }
    }
}

TEST_CASE("an upload thread never spins: a dead server or an ended upload costs a few requests a second at most")
{
    enum class Fault : uint8_t
    {
        Down,
        Error,
        Ended,
        Duplicate,
    };
    for (const auto fault : { Fault::Down, Fault::Error, Fault::Ended, Fault::Duplicate })
    {
        INFO(static_cast<int>(fault));
        const auto  dir = tempDir();
        FakeMogsafe server;
        append(dir / "a.xicap", std::string(500, 'x'));
        CHECK(!uploader(dir, server)->pass());
        append(dir / "a.xicap", std::string(100, 'x'));
        auto& u = server.uploads.begin()->second;
        switch (fault)
        {
            case Fault::Down:
                server.down = true;
                break;
            case Fault::Error:
                server.forced["HEAD"] = std::vector(1000, 500);
                break;
            case Fault::Ended:
                u.cutOff = true;
                break;
            case Fault::Duplicate: // Mogsafe refuses, at its first append, a capture it holds, then ends the upload
                server.forced["PATCH"] = { 409 };
                break;
        }
        UploadOptions options{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config", .every = std::chrono::milliseconds(20), .idle = std::chrono::milliseconds(0) };
        auto          up = std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
        if (fault == Fault::Duplicate)
        {
            CHECK(up->pass().has_value()); // the 409
            u.cutOff = true;
        }
        const auto before = server.requests;
        up->start();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        up.reset();
        const auto requests = server.requests - before;
        CHECK(requests <= 30);
        if (fault == Fault::Ended || fault == Fault::Duplicate)
        {
            CHECK(requests <= 1); // one 410, then never again
            CHECK(u.data.size() == 500);
        }
        fs::remove_all(dir);
    }
}

TEST_CASE("a server holding more than the file on disk gets nothing, and no request loops")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    auto        up = uploader(dir, server);
    append(file, std::string(600, 'x'));
    CHECK(!up->pass());
    fs::resize_file(file, 500); // a power cut lost the file's tail, already sent
    CHECK(!up->pass());         // its close is refused
    const auto requests = server.requests;
    CHECK(!up->pass());
    CHECK(server.requests == requests + 1); // a HEAD
    CHECK(server.uploads.begin()->second.data.size() == 600);
    fs::remove_all(dir);
}

TEST_CASE("timestamps that step back or leap ahead lose no packet")
{
    const auto         dir  = tempDir();
    constexpr uint64_t DAYS = 50ULL * 24 * 3600 * 1000; // past 2^32 ms: a PC asleep for 50 days
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        for (const auto t : { T0 + 10, T0 + 5, T0 + 5, T0 + DAYS, T0 + DAYS + 1, T0 + 1 })
        {
            rec.packet(gamePacket(t));
        }
        rec.stop();
    }
    xicap::Reader r(captures(dir)[0]);
    uint64_t      last{ 0 };
    int           n{ 0 };
    for (const auto& p : r)
    {
        CHECK(p.timestamp >= last);
        last = p.timestamp;
        ++n;
    }
    CHECK(n == 6);
    CHECK(r.complete());
    CHECK(last == T0 + DAYS + 1);
}

TEST_CASE("a three-day session stays one capture, whole")
{
    const auto         dir     = tempDir();
    constexpr uint64_t PACKETS = 50'000;
    constexpr uint64_t STEP    = 3ULL * 24 * 3600 * 1000 / PACKETS; // past two midnights
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        for (uint64_t i = 0; i < PACKETS; ++i)
        {
            rec.packet(gamePacket(T0 + 1 + i * STEP, static_cast<uint16_t>(0x00D + i % 64)));
        }
        rec.stop();
        CHECK(rec.status().dropped == 0);
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 1);
    const auto read = readCapture(files[0]);
    CHECK(read.packets == PACKETS);
    CHECK(read.complete);
    fs::remove_all(dir);
}

TEST_CASE("sessions: a stop without a start, a start over a running one, and a character change")
{
    const auto dir = tempDir();
    const auto a   = dir / "sessions" / "a.xicap";
    const auto b   = dir / "sessions" / "b.xicap";
    {
        Recorder rec(options(dir));
        rec.login(T0, who("Kalnor", 1));
        rec.sessionStop(T0 + 1); // nothing to stop
        rec.packet(gamePacket(T0 + 2));
        rec.sessionStart(T0 + 3, a);
        rec.packet(gamePacket(T0 + 4));
        rec.sessionStart(T0 + 5, b); // ends a
        rec.packet(gamePacket(T0 + 6));
        rec.login(T0 + 7, who("Mule", 2)); // ends b with Kalnor' capture
        rec.packet(gamePacket(T0 + 8));
        rec.sessionStop(T0 + 9);
        rec.stop();
        CHECK(rec.status().error.empty());
        CHECK(rec.status().sessionError.empty());
    }
    const auto first  = readCapture(a);
    const auto second = readCapture(b);
    CHECK(first.complete);
    CHECK(second.complete);
    CHECK(first.packets == 1);
    CHECK(second.packets == 1);
    const auto files = captures(dir);
    REQUIRE(files.size() == 2);
    auto kalnor = readCapture(files[0]);
    auto mule   = readCapture(files[1]);
    if (kalnor.name != "Kalnor")
    {
        std::swap(kalnor, mule);
    }
    CHECK(kalnor.packets == 3);
    CHECK(mule.packets == 1);
    CHECK(first.previous == kalnor.id);
    CHECK(second.previous == kalnor.id);
    fs::remove_all(dir);
}

#ifdef __linux__
// Points every descriptor open on path at /dev/full: from here its writes fail as on a full disk.
static auto fill(const fs::path& path) -> void
{
    const int full{ ::open("/dev/full", O_WRONLY) };
    REQUIRE(full >= 0);
    for (const auto& fd : fs::directory_iterator("/proc/self/fd"))
    {
        std::error_code ec;
        if (fs::read_symlink(fd.path(), ec) == fs::canonical(path))
        {
            ::dup2(full, std::stoi(fd.path().filename().string()));
        }
    }
    ::close(full);
}

TEST_CASE("a full disk is said once, ends the session with a reason, and counts the packets it lost")
{
    const auto               dir  = tempDir();
    auto                     opts = options(dir);
    std::mutex               mutex;
    std::vector<std::string> said;
    opts.failed = [&](const std::string& why)
    {
        std::lock_guard lock(mutex);
        said.push_back(why);
    };
    Recorder rec(opts);
    rec.login(T0, who("Kalnor", 1));
    rec.packet(gamePacket(T0 + 1));
    rec.sessionStart(T0 + 2, dir / "session" / "memoria.xicap");
    REQUIRE(waitFor([&]
                    {
                        return !rec.status().session.empty();
                    }));
    const auto broken = rec.status().capture;
    fill(broken);
    for (uint64_t i = 0; i < 5; ++i)
    {
        rec.packet(gamePacket(T0 + 3 + i));
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // past a flush each
    }
    REQUIRE(waitFor([&]
                    {
                        return rec.status().capture.empty();
                    }));
    CHECK(!rec.status().error.empty());
    CHECK(!rec.status().sessionError.empty());
    CHECK(rec.status().dropped >= 4); // packets after the failure went nowhere

    rec.login(T0 + 100, who("Mule", 2)); // a new file on a disk with room again
    rec.packet(gamePacket(T0 + 101));
    REQUIRE(waitFor([&]
                    {
                        return rec.status().error.empty();
                    }));
    rec.stop();
    {
        std::lock_guard lock(mutex);
        CHECK(said.size() == 1);
    }
    fs::remove_all(dir);
}
#endif

#ifdef __linux__
TEST_CASE("a capture whose close fails is still named by the next one, after a restart")
{
    const auto       dir  = tempDir();
    auto             opts = options(dir);
    xicap::CaptureId cut{};
    opts.flushEvery = std::chrono::hours(1); // only the close writes
    {
        Recorder rec(opts);
        rec.login(T0, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 1));
        rec.packet(gamePacket(T0 + 6'000)); // over the live preset's 5 s a frame: the first frame reaches disk
        fs::path path;
        REQUIRE(waitFor([&]
                        {
                            path = rec.status().capture;
                            std::error_code ec;
                            return !path.empty() && fs::file_size(path, ec) > CAPTURE_HEADER_SIZE;
                        }));
        cut = readCapture(path).id;
        const int full{ ::open("/dev/full", O_WRONLY) };
        REQUIRE(full >= 0);
        for (const auto& fd : fs::directory_iterator("/proc/self/fd"))
        {
            std::error_code ec;
            if (fs::read_symlink(fd.path(), ec) == fs::canonical(path))
            {
                ::dup2(full, std::stoi(fd.path().filename().string()));
            }
        }
        ::close(full);
        rec.stop(); // the trailer never lands
        CHECK(!rec.status().error.empty());
    }
    {
        Recorder rec(options(dir));
        rec.login(T0 + 60'000, who("Kalnor", 1));
        rec.packet(gamePacket(T0 + 60'001));
        rec.stop();
    }
    const auto files = captures(dir);
    REQUIRE(files.size() == 2);
    const auto first = readCapture(files[0]).id == cut ? readCapture(files[1]) : readCapture(files[0]);
    CHECK(first.previous == cut);
    fs::remove_all(dir);
}
#endif
