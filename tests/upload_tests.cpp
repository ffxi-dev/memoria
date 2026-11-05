#include "support.h"

using namespace memoria;
using namespace memoria::test;

TEST_CASE("an upload follows a growing file and closes once it is complete")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    auto        up = uploader(dir, server);

    append(file, std::string(100, 'x'));
    CHECK(!up->uploadAll());
    CHECK(server.creates == 0); // no header yet

    append(file, std::string(400, 'x'));
    CHECK(!up->uploadAll());
    CHECK(!up->uploadAll()); // all sent; the close is refused: no trailer yet
    append(file, std::string(50, 'x') + TRAILER);
    CHECK(!up->uploadAll()); // sends the rest
    CHECK(!up->uploadAll()); // closes
    REQUIRE(server.creates == 1);
    const auto& u = server.uploads.begin()->second;
    CHECK(u.closed);
    CHECK(u.data == readAll(file));
    const auto status = up->status();
    REQUIRE(status.size() == 1);
    CHECK(status[0].sent == 574);
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
    CHECK(!sender->uploadAll());
    append(file, std::string(100, 'x'));
    const auto status = other->status();
    REQUIRE(status.size() == 1);
    CHECK(status[0].name == "a.xicap");
    CHECK(status[0].sent == 500);
    CHECK(status[0].size == 600);
}

TEST_CASE("a recording's Mogsafe id is the end of its upload's URL, in every client sharing the install")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    auto        sender = uploader(dir, server);
    auto        other  = uploader(dir, server);
    CHECK(sender->recordingId("a.xicap").empty()); // no upload yet
    append(dir / "a.xicap", std::string(500, 'x'));
    CHECK(!sender->uploadAll());
    CHECK(sender->recordingId("a.xicap") == "1");
    CHECK(other->recordingId("a.xicap") == "1"); // read from uploads.txt
    CHECK(sender->recordingId("b.xicap").empty());
    fs::remove_all(dir);
}

TEST_CASE("a restart resumes the same upload")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    append(file, std::string(500, 'x'));
    CHECK(!uploader(dir, server)->uploadAll());
    append(file, std::string(300, 'x'));
    CHECK(!uploader(dir, server)->uploadAll());
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
        append(file, std::string(size_t{ 3 } * 512 * 1024, 'x'));
        CHECK_THROWS(uploader(dir, server)->uploadAll()); // no save at the end of the pass, as in a crash
        server.crashAtPatch = -1;
        CHECK(!uploader(dir, server)->uploadAll());
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
    CHECK(up->uploadAll().has_value());
    CHECK(!up->saveError().empty());
    CHECK(up->uploadAll().has_value()); // the URL in memory does not skip the save
    CHECK(server.creates == 1);
    CHECK(server.received == 0);
    SECTION("the save works again")
    {
        fs::remove(dir / "uploads.txt.tmp");
        CHECK(!up->uploadAll());
        CHECK(up->saveError().empty());
        CHECK(server.creates == 1);
    }
    SECTION("a restart")
    {
        up.reset();
        fs::remove(dir / "uploads.txt.tmp");
        CHECK(!uploader(dir, server)->uploadAll());
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
    CHECK(!up->uploadAll());

    server.down = true;
    append(file, std::string(100, 'x'));
    CHECK(up->uploadAll().has_value());
    CHECK(up->status()[0].error.find("refused") != std::string::npos);

    server.down = false;
    server.uploads.clear();
    CHECK(!up->uploadAll()); // 404, and a close answers 404: Mogsafe holds none of it
    CHECK(!up->uploadAll()); // a new upload of the whole file
    CHECK(server.creates == 2);
    CHECK(server.uploads.rbegin()->second.data == readAll(file));
    CHECK(up->status()[0].error.empty());
}

TEST_CASE("a file Mogsafe keeps refusing never holds the recording's upload past Mogsafe's 5-minute silence limit")
{
    // A file uploaded under another account answers 403 to every request. Mogsafe ends a recording that sends nothing
    // for 5 minutes, so failures may slow the uploader to at most 12 passes' wait: a minute at the default 5 s.
    const auto  dir = tempDir();
    FakeMogsafe server;
    append(dir / "old.xicap", std::string(500, 'x'));
    CHECK(!uploader(dir, server)->uploadAll());
    append(dir / "old.xicap", std::string(100, 'x'));
    server.foreign.insert(server.uploads.begin()->first);

    std::mutex                                         mutex;
    std::vector<std::chrono::steady_clock::time_point> sent;
    server.onPatch = [&]
    {
        const std::scoped_lock lock(mutex);
        sent.push_back(std::chrono::steady_clock::now());
    };
    UploadOptions options{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config", .every = std::chrono::milliseconds(10), .idle = std::chrono::hours(1) };
    auto          up    = std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
    const auto    start = std::chrono::steady_clock::now();
    up->start();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4))
    {
        append(dir / "live.xicap", std::string(500, 'y')); // the recording grows
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    up.reset();
    REQUIRE(sent.size() > 2);
    auto longest = std::chrono::steady_clock::duration{};
    for (size_t i = 1; i < sent.size(); ++i)
    {
        longest = std::max(longest, sent[i] - sent[i - 1]);
    }
    CHECK(longest < std::chrono::milliseconds(600)); // 12 passes of 10 ms, and slack
    fs::remove_all(dir);
}

TEST_CASE("an upload's URL keeps Mogsafe's HTTPS origin, whatever Location says")
{
    // Behind its Cloudflare tunnel, Mogsafe's tusd sees plain HTTP and answers a create with an http:// Location.
    for (const auto* origin : { "http://mogsafe.test", "https://elsewhere.test", "" })
    {
        INFO(origin);
        const auto  dir = tempDir();
        FakeMogsafe server;
        server.origin = origin;
        append(dir / "a.xicap", std::string(500, 'x') + TRAILER);
        auto up = uploader(dir, server);
        CHECK(!up->uploadAll());
        CHECK(!up->uploadAll());
        CHECK(server.strayed.empty());
        CHECK(server.uploads.begin()->second.closed);
        fs::remove_all(dir);
    }
    {
        // A URL saved before the rule.
        const auto  dir = tempDir();
        FakeMogsafe server;
        append(dir / "a.xicap", std::string(500, 'x'));
        CHECK(!uploader(dir, server)->uploadAll());
        std::ofstream(dir / "uploads.txt", std::ios::trunc) << "a.xicap\thttp://mogsafe.test/v1/uploads/1\t0\t0\t500\n";
        append(dir / "a.xicap", TRAILER);
        auto up = uploader(dir, server);
        CHECK(!up->uploadAll());
        CHECK(!up->uploadAll());
        CHECK(server.strayed.empty());
        CHECK(server.creates == 1);
        CHECK(server.uploads.begin()->second.closed);
        fs::remove_all(dir);
    }
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
                const std::scoped_lock lock(mutex_);
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
    saveLink(dir / "config", { .token = "t0ken", .account = "player" });
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
    CHECK(up->uploadAll().has_value());
    CHECK(up->unauthorized());
    CHECK(!up->unauthorized()); // said once
    CHECK(!fs::exists(dir / "config" / "token"));

    const auto requests = server.requests;
    CHECK(!up->uploadAll());
    CHECK(server.requests == requests);

    saveLink(dir / "config", { .token = "revoked", .account = "player" });
    CHECK(!up->uploadAll());
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
    touch(paused); // /memoria upload off in a client that is not uploading
    CHECK(!first->uploadAll());
    first.reset(); // the uploading client exits, and the other takes the upload mutex
    CHECK(!second->uploadAll());
    CHECK(server.requests == 0);

    fs::remove(paused);
    CHECK(!second->uploadAll());
    CHECK(server.uploads.begin()->second.data.size() == 500);

    fs::remove(token); // /memoria unlink in another client
    append(file, std::string(100, 'x'));
    CHECK(!second->uploadAll());
    CHECK(server.uploads.begin()->second.data.size() == 500);

    server.token = "t1"; // /memoria link in another client
    saveLink(dir / "config", { .token = "t1", .account = "player" });
    CHECK(!second->uploadAll());
    CHECK(server.uploads.begin()->second.data == readAll(file));
    fs::remove_all(dir);
}

TEST_CASE("a refusal of an old token keeps the token another client just saved")
{
    // Another client links again while this one's request with the old token is in flight.
    class Relink final : public Http
    {
    public:
        Relink(FakeMogsafe& to, fs::path config)
        : to_(to)
        , config_(std::move(config))
        {
        }

        auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
        {
            if (to_.token == "t0ken")
            {
                to_.token = "t1";
                saveLink(config_, { .token = "t1", .account = "player" });
            }
            return to_.request(method, url, headers, body);
        }

    private:
        FakeMogsafe& to_;
        fs::path     config_;
    };
    const auto  dir = tempDir();
    FakeMogsafe server;
    saveLink(dir / "config", { .token = "t0ken", .account = "player" });
    Uploader up(UploadOptions{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config", .idle = std::chrono::milliseconds(0) }, std::make_unique<Relink>(server, dir / "config"));
    append(dir / "a.xicap", std::string(500, 'x'));
    CHECK(up.uploadAll().has_value()); // 401 for the old token
    CHECK(!up.unauthorized());
    REQUIRE(loadLink(dir / "config"));
    CHECK(loadLink(dir / "config")->token == "t1");
    CHECK(!up.uploadAll());
    CHECK(server.uploads.begin()->second.data == readAll(dir / "a.xicap"));
    fs::remove_all(dir);
}

TEST_CASE("a capture Mogsafe refused stays refused, with its reason, after a restart and in another client")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    append(file, std::string(500, 'x') + TRAILER);
    auto up = uploader(dir, server);
    CHECK(!up->uploadAll());         // sends
    server.forced["POST"] = { 422 }; // the close: refused for good
    CHECK(!up->uploadAll());
    REQUIRE(up->status()[0].refused);
    up.reset();
    auto again = uploader(dir, server);
    CHECK(!again->uploadAll());
    auto status = again->status();
    REQUIRE(status.size() == 1);
    CHECK(status[0].refused);
    CHECK(status[0].error == "forced");
    auto other = uploader(dir, server); // a client that is not uploading reads the state file
    status     = other->status();
    CHECK(status[0].refused);
    CHECK(status[0].error == "forced");
    fs::remove_all(dir);
}

TEST_CASE("at unload the last bytes and the trailer reach Mogsafe, and the game waits no longer than allowed")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    saveLink(dir / "config", { .token = "t0ken", .account = "player" });
    append(file, std::string(500, 'x'));
    // Passes an hour apart and a close only after an hour idle: only finish() can send the end in time.
    UploadOptions options{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config", .every = std::chrono::hours(1), .idle = std::chrono::hours(1) };
    auto          up = std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
    up->start();
    REQUIRE(waitFor([&]
                    {
                        const auto status = up->status();
                        return !status.empty() && status[0].sent == 500;
                    }));
    append(file, std::string(100, 'y') + TRAILER); // the recorder's last frame and trailer, at unload
    const auto before = std::chrono::steady_clock::now();
    up->finish({ "a.xicap" }, std::chrono::seconds(3));
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(3));
    up.reset();
    REQUIRE(server.uploads.size() == 1);
    CHECK(server.uploads.begin()->second.data == readAll(file));
    CHECK(server.uploads.begin()->second.closed);
    CHECK(server.creates == 1);
    fs::remove_all(dir);
}

TEST_CASE("at unload a hung connection holds the game for the time allowed, no longer")
{
    class Hung final : public Http
    {
    public:
        auto request(const std::string&, const std::string&, const HttpHeaders&, std::span<const uint8_t>) -> std::expected<HttpResponse, std::string> override
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(60), [this]
                           {
                               return cancelled_;
                           });
            return std::unexpected("cancelled");
        }

        auto cancel() -> void override
        {
            {
                const std::scoped_lock lock(mutex_);
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
    saveLink(dir / "config", { .token = "t0ken", .account = "player" });
    append(dir / "a.xicap", std::string(500, 'x') + TRAILER);
    auto up = std::make_unique<Uploader>(UploadOptions{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config" }, std::make_unique<Hung>());
    up->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the first pass hangs in its create
    const auto before = std::chrono::steady_clock::now();
    up->finish({ "a.xicap" }, std::chrono::milliseconds(300));
    up.reset();
    const auto took = std::chrono::steady_clock::now() - before;
    CHECK(took >= std::chrono::milliseconds(300));
    CHECK(took < std::chrono::milliseconds(1500));
    fs::remove_all(dir);
}

TEST_CASE("an upload Mogsafe refused at its end is not counted as missing from Vault")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    append(dir / "a.xicap", std::string(500, 'x'));
    auto up               = uploader(dir, server);
    server.forced["HEAD"] = { 410 }; // Mogsafe-Ended: refused, a recording it already holds
    CHECK(!up->uploadAll());
    CHECK(!up->uploadAll());
    CHECK(up->ended("a.xicap"));
    const auto status = up->status();
    REQUIRE(status.size() == 1);
    CHECK(!status[0].cutOff);
    CHECK(status[0].sent == status[0].size);
    fs::remove_all(dir);
}

namespace
{

// A complete capture of three packets, the first alone in its frame. Returns where that frame ends.
auto twoFrames(const fs::path& path) -> uint64_t
{
    {
        xicap::Writer w(path, { .character = { .name = "Kalnor" } });
        w.write(outPacket(gamePacket(T0 + 1)));
        w.flush();
        w.write(outPacket(gamePacket(T0 + 2)));
        w.write(outPacket(gamePacket(T0 + 3)));
        w.close();
    }
    xicap::Reader r(path);
    xicap::Packet p;
    r.next(p);
    return r.bytesRead();
}

} // namespace

TEST_CASE("the tail of a recording Mogsafe cut off goes up as a new capture, once")
{
    const auto dir    = tempDir();
    const auto file   = dir / "a.xicap";
    const auto source = dir / "source" / "a.xicap";
    fs::create_directories(source.parent_path());
    const auto  held  = static_cast<std::ptrdiff_t>(twoFrames(source));
    const auto  bytes = readAll(source);
    FakeMogsafe server;
    append(file, std::string(bytes.begin(), bytes.begin() + held));
    CHECK(!uploader(dir, server)->uploadAll());
    append(file, std::string(bytes.begin() + held, bytes.end())); // a long outage, or a killed client, then the file was closed
    server.uploads.begin()->second.cutOff = true;

    auto up = uploader(dir, server);
    CHECK(!up->uploadAll()); // 410: ended
    CHECK(!up->uploadAll()); // writes a-rest.xicap
    CHECK(!up->uploadAll()); // sends it
    CHECK(!up->uploadAll()); // closes it
    const auto rest = dir / "a-rest.xicap";
    REQUIRE(fs::exists(rest));
    const auto written = fs::last_write_time(rest);
    CHECK(!uploader(dir, server)->uploadAll()); // a restart writes nothing again
    CHECK(fs::last_write_time(rest) == written);
    const auto tail = readCapture(rest);
    CHECK(tail.complete);
    CHECK(tail.packets == 2);
    CHECK(tail.previous == readCapture(file).id);
    CHECK(tail.name == "Kalnor");
    CHECK(server.creates == 2);
    CHECK(std::ssize(server.uploads.begin()->second.data) == held); // the cut-off file never goes again
    CHECK(uploader(dir, server)->ended("a.xicap"));                 // another client reads it from disk
    CHECK(server.uploads.rbegin()->second.data == readAll(rest));
    CHECK(server.uploads.rbegin()->second.closed);
    for (const auto& s : uploader(dir, server)->status())
    {
        CHECK(!s.cutOff); // nothing left only on this PC
        CHECK(s.sent == s.size);
    }
    fs::remove_all(dir);
}

TEST_CASE("a tail that cannot be written stays on this PC and counts as not in Vault, and a refused upload has none")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    append(dir / "a.xicap", std::string(500, 'x'));
    append(dir / "b.xicap", std::string(500, 'x'));
    CHECK(!uploader(dir, server)->uploadAll());
    append(dir / "a.xicap", std::string(100, 'y'));
    append(dir / "b.xicap", std::string(100, 'y'));
    server.uploads.begin()->second.cutOff = true;
    server.foreignEnd                     = server.uploads.rbegin()->first; // b: Mogsafe-Ended: refused
    auto up                               = uploader(dir, server);
    CHECK(!up->uploadAll());
    CHECK(up->uploadAll().has_value()); // not a capture: xicap cannot read it
    CHECK(!up->uploadAll());            // tried once a run, not every pass
    const auto status = up->status();
    REQUIRE(status.size() == 2);
    CHECK(status[0].cutOff);
    CHECK(status[0].sent == 500);
    CHECK(!status[1].cutOff);
    CHECK(!fs::exists(dir / "a-rest.xicap"));
    CHECK(!fs::exists(dir / "a-rest.tmp"));
    fs::remove_all(dir);
}

TEST_CASE("a lost close answer for a capture the server holds complete counts as closed")
{
    const auto  dir  = tempDir();
    const auto  file = dir / "a.xicap";
    FakeMogsafe server;
    append(file, std::string(500, 'x') + TRAILER);
    CHECK(!uploader(dir, server)->uploadAll());
    server.uploads.begin()->second.closed = true; // closed, its answer lost
    auto up                               = uploader(dir, server);
    CHECK(!up->uploadAll());
    CHECK(!up->ended("a.xicap"));
    CHECK(up->status()[0].sent == 524);
    CHECK(server.creates == 1);
    fs::remove_all(dir);
}

TEST_CASE("a recording whose upload the server ended goes on in a new capture that names it, and sends nothing twice")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    auto        up = uploader(dir, server);
    int         told{ 0 };
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
    rec.login(T0, who("Kalnor", 1));
    rec.packet(gamePacket(T0 + 1));
    fs::path first;
    REQUIRE(waitFor([&]
                    {
                        first = rec.status().capture;
                        std::error_code ec;
                        if (first.empty() || fs::file_size(first, ec) < xicap::HEADER_SIZE)
                        {
                            return false;
                        }
                        // The first frame must be whole on disk, not only its start, or the tail would hold it too.
                        try
                        {
                            return readCapture(first).packets == 1;
                        }
                        catch (const std::exception&)
                        {
                            return false;
                        }
                    }));
    CHECK(!up->uploadAll());
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
    CHECK(!up->uploadAll()); // 410
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
    for (int i = 0; i < 3; ++i)
    {
        CHECK(!up->uploadAll()); // the new capture and the first one's tail: written, sent, closed
    }

    CHECK(told == 1);
    CHECK(second != first);
    CHECK(server.creates == 3);
    CHECK(cut.data.size() == had);
    const auto old  = readCapture(first);
    const auto now  = readCapture(second);
    const auto tail = readCapture(fs::path(first).replace_filename(first.stem().string() + "-rest.xicap"));
    CHECK(old.complete);
    CHECK(old.packets == 2);
    CHECK(tail.packets == 1); // the packet Mogsafe refused
    CHECK(tail.previous == old.id);
    CHECK(now.packets == 1);
    CHECK(now.previous == old.id);
    CHECK(std::ranges::all_of(server.uploads | std::views::drop(1) | std::views::values, &FakeMogsafe::Upload::closed));
    CHECK(!now.atLogin);
    fs::remove_all(dir);
}

TEST_CASE("an upload another account made is skipped while this token stays, and tried again after a new link")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    append(dir / "old.xicap", std::string(500, 'x'));
    auto up = uploader(dir, server);
    CHECK(!up->uploadAll());
    append(dir / "old.xicap", std::string(100, 'x'));
    server.foreign.insert(server.uploads.begin()->first);
    CHECK(!up->uploadAll()); // a 403: not a failure, so no backoff for the other files
    REQUIRE(up->status().size() == 1);
    CHECK(up->status()[0].foreign);
    const auto requests = server.requests;
    CHECK(!up->uploadAll());
    CHECK(server.requests == requests); // the same token asks nothing
    server.foreign.clear();
    server.token = "n3w";
    saveLink(dir / "config", { .token = "n3w", .account = "owner" });
    CHECK(!up->uploadAll());
    CHECK(server.requests > requests);
    CHECK(!up->status()[0].foreign);
    CHECK(server.uploads.begin()->second.data.size() == 600);
    fs::remove_all(dir);
}

TEST_CASE("a capture Mogsafe refused for good costs no request")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    append(dir / "a.xicap", std::string(500, 'x') + TRAILER);
    auto up = uploader(dir, server);
    CHECK(!up->uploadAll());
    server.forced["POST"] = { 422 };
    CHECK(!up->uploadAll());
    REQUIRE(up->status()[0].refused);
    const auto requests = server.requests;
    CHECK(!up->uploadAll());
    CHECK(!up->uploadAll());
    CHECK(server.requests == requests);
    fs::remove_all(dir);
}

TEST_CASE("an unload during a long cut-off recovery returns at once, and the next run writes the tail")
{
    const auto dir    = tempDir();
    const auto file   = dir / "a.xicap";
    const auto source = dir / "source" / "a.xicap";
    fs::create_directories(source.parent_path());
    {
        xicap::Writer w(source, { .character = { .name = "Kalnor" } });
        for (uint64_t i = 0; i < 1'500'000; ++i)
        {
            w.write(outPacket(gamePacket(T0 + i)));
        }
        w.close();
    }
    const auto  bytes = readAll(source);
    FakeMogsafe server;
    append(file, std::string(bytes.begin(), bytes.begin() + 200));
    CHECK(!uploader(dir, server)->uploadAll());
    append(file, std::string(bytes.begin() + 200, bytes.end()));
    server.uploads.begin()->second.cutOff = true;

    UploadOptions options{ .dir = dir, .server = "https://mogsafe.test", .config = dir / "config", .every = std::chrono::milliseconds(10), .idle = std::chrono::milliseconds(0) };
    auto          up = std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
    up->start();
    REQUIRE(waitFor([&]
                    {
                        return fs::exists(dir / "a-rest.tmp");
                    }));
    const auto before = std::chrono::steady_clock::now();
    up.reset();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    CHECK(!fs::exists(dir / "a-rest.tmp"));
    CHECK(!fs::exists(dir / "a-rest.xicap"));
    CHECK(readUploads(dir / "uploads.txt")["a.xicap"].stage != Stage::Rescued);

    auto again = uploader(dir, server);
    CHECK(!again->uploadAll()); // writes the tail
    CHECK(fs::exists(dir / "a-rest.xicap"));
    fs::remove_all(dir);
}

TEST_CASE("a recording keeps its Vault id once its upload closes or the server ends it, here and after a restart")
{
    const auto  dir = tempDir();
    FakeMogsafe server;
    auto        up = uploader(dir, server);
    append(dir / "a.xicap", std::string(500, 'x') + TRAILER); // a character change closed it
    append(dir / "b.xicap", std::string(500, 'y'));           // the server will end it, and the recorder rotate
    REQUIRE(!up->uploadAll());
    REQUIRE(!up->uploadAll());
    const auto a = up->recordingId("a.xicap");
    const auto b = up->recordingId("b.xicap");
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());
    for (auto& [path, u] : server.uploads)
    {
        u.cutOff = !u.closed;
    }
    append(dir / "b.xicap", std::string(100, 'y'));
    CHECK(!up->uploadAll()); // 410: ended
    CHECK(up->ended("b.xicap"));
    CHECK(up->recordingId("a.xicap") == a);
    CHECK(up->recordingId("b.xicap") == b);
    up.reset();
    const auto other = uploader(dir, server); // a restart, or another client, reads the state file
    CHECK(other->recordingId("a.xicap") == a);
    CHECK(other->recordingId("b.xicap") == b);
    fs::remove_all(dir);
}

TEST_CASE("each upload stage survives the state file")
{
    const auto  dir = tempDir();
    UploadFiles files;
    for (const auto stage : { Stage::Open, Stage::Closed, Stage::Ended, Stage::CutOff, Stage::Rescued })
    {
        files[std::format("{}.xicap", std::to_underlying(stage))] = { .url = "https://mogsafe.test/v1/uploads/1", .stage = stage, .refusedAt = 7, .sent = 5, .reason = "too old" };
    }
    REQUIRE(!writeUploads(dir / "uploads.txt", files));
    const auto read = readUploads(dir / "uploads.txt");
    REQUIRE(read.size() == files.size());
    for (const auto& [name, f] : files)
    {
        INFO(name);
        CHECK(read.at(name).stage == f.stage);
        CHECK(read.at(name).refusedAt == 7);
        CHECK(read.at(name).sent == 5);
        CHECK(read.at(name).reason == "too old");
    }
    fs::remove_all(dir);
}
