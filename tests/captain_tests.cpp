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

auto unzstd(const std::vector<uint8_t>& in) -> std::string
{
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> d(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    std::string                                                out;
    std::vector<char>                                          buf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer                                              input{ .src = in.data(), .size = in.size(), .pos = 0 };
    for (size_t left = 1; left != 0;)
    {
        ZSTD_outBuffer o{ .dst = buf.data(), .size = buf.size(), .pos = 0 };
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
    fs::path                 dir{ tempDir() };
    fs::path                 captures{ dir / "ashita" / "addons" / "captain" / "captures" };
    fs::path                 uploaded{ dir / "ashita" / "addons" / "captain" / "captures-uploaded" };
    fs::path                 state{ dir / "ashita" / "captures" / "memoria" };
    fs::path                 config{ dir / "config" };
    std::vector<std::string> said;
    std::mutex               mutex;

    Install()
    {
        fs::create_directories(captures);
        REQUIRE(!saveLink(config, { .token = "t0ken", .account = "player" }));
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
        options.post = [this](std::string text)
        {
            const std::scoped_lock lock(mutex);
            said.push_back(std::move(text));
        };
        return options;
    }

    auto importer(FakeMogsafe& server, std::chrono::milliseconds retry = std::chrono::milliseconds(1)) -> std::unique_ptr<Importer>
    {
        return std::make_unique<Importer>(options(retry), std::make_unique<Forward>(server));
    }

    auto saidAny(const std::string_view text) -> bool
    {
        const std::scoped_lock lock(mutex);
        return std::ranges::any_of(said, [&](const auto& s)
                                   {
                                       return s.find(text) != std::string::npos;
                                   });
    }
};

} // namespace

TEST_CASE("captain captures are found at any depth, and not inside one")
{
    const Install in;
    captainFolder(in.captures, "Mining/test_2026-8-4/Mirabeth", "log");
    captainFolder(in.captures, "2026-8-4_23_54/Mirabeth", "log");
    captainFolder(in.captures, "2026-8-4_23_54/Mirabeth/inner/Nested", "log"); // inside a match: not searched
    fs::create_directories(in.captures / "2026-8-5/Mirabeth");                 // a session made with Memoria
    std::ofstream(in.captures / "2026-8-5/Mirabeth/manifest.txt") << MANIFEST;
    std::ofstream(in.captures / "2026-8-5/Mirabeth/memoria.xicap") << "x";
    fs::create_directories(in.captures / "tracker"); // an addon's own data
    std::ofstream(in.captures / "tracker/data.json") << "{}";

    const auto found = findCaptures(in.captures);
    REQUIRE(found.size() == 2);
    CHECK(found[0].folder == "2026-8-4_23_54/Mirabeth");
    CHECK(found[1].folder == "Mining/test_2026-8-4/Mirabeth");
    CHECK(found[1].character == "Mirabeth");
    CHECK(found[1].size == 3);
    CHECK(!found[1].growing);
    CHECK(findCaptures(in.dir / "no such folder").empty());
}

TEST_CASE("a full.log changed in the last 10 minutes is left for later")
{
    Install in;
    captainFolder(in.captures, "a/Mirabeth", "log", MANIFEST, std::chrono::minutes(2));
    captainFolder(in.captures, "b/Mirabeth", "log", MANIFEST, std::chrono::minutes(11));
    const auto found = findCaptures(in.captures);
    REQUIRE(found.size() == 2);
    CHECK(found[0].growing);
    CHECK(!found[1].growing);

    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/2 a/Mirabeth: still being written; skipped."));
    CHECK(fs::exists(in.captures / "a/Mirabeth"));
    CHECK(fs::exists(in.uploaded / "b/Mirabeth"));
    CHECK(server.creates == 1);
}

TEST_CASE("CRC-32 matches what Mogsafe's import page computes")
{
    const Install    in;
    std::stop_source stop; // NOLINT(misc-const-correctness): request_stop is const in libstdc++ only
    std::string      log;
    for (int i = 0; i < 1000; ++i)
    {
        log += "[2026-08-04 23:54:01] Incoming packet 0x00A\n|  00 01 02 03 |\n";
    }
    std::ofstream(in.dir / "check", std::ios::binary) << "123456789";
    std::ofstream(in.dir / "full.log", std::ios::binary) << log;
    // The values Mogsafe's zip.js crc32Of, which browse.html calls, gives for the same bytes.
    CHECK(crc32File(in.dir / "check")->crc == 0xcbf43926);
    const auto crc = crc32File(in.dir / "full.log");
    REQUIRE(crc);
    CHECK(crc->crc == 0x94175684);
    CHECK(crc->size == 61000);
    CHECK(!crc32File(in.dir / "missing"));
    stop.request_stop();
    CHECK(!crc32File(in.dir / "full.log", stop.get_token()));
}

TEST_CASE("a captain import sends full.log compressed with its manifest, then moves the folder")
{
    Install    in;
    const auto log = std::string(5000, 'p') + noise(3000);
    const auto dir = captainFolder(in.captures, "2026-8-4_23_54/Mirabeth", log);
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
    CHECK(u.meta["character"] == "Mirabeth");
    CHECK(u.meta["folder"] == "2026-8-4_23_54/Mirabeth");
    CHECK(u.meta["size"] == "8000");
    CHECK(u.meta["crc32"].size() == 8);
    CHECK(!u.meta.contains("zip"));

    CHECK(!fs::exists(in.captures / "2026-8-4_23_54")); // emptied parent removed
    CHECK(fs::exists(in.captures));
    CHECK(fs::exists(in.uploaded / "2026-8-4_23_54/Mirabeth/PacketLogger/incoming/0x00A.log"));
    CHECK(!Importer::pending(in.state));
    CHECK(!fs::exists(in.state / "imports"));
    CHECK(in.said.front() == "Uploading 1 captain capture (7.8 KB) in the background.");
    CHECK(in.saidAny("Captain 1/1 2026-8-4_23_54/Mirabeth: uploaded."));
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
    captainFolder(in.captures, "s/Mirabeth", log);
    FakeMogsafe server;
    server.imported[std::format("{:08x}:{}", crc32File(in.captures / "s/Mirabeth/PacketLogger/full.log")->crc, log.size())] = "cap7";
    in.importer(server)->run();
    CHECK(server.creates == 0);
    CHECK(server.received == 0);
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
    CHECK(in.saidAny("Captain 1/1 s/Mirabeth: already in Vault (cap7)."));
}

TEST_CASE("a refused capture stays where it is and the next one goes")
{
    Install in;
    captainFolder(in.captures, "a/Mirabeth", "log a", "Character = Mirabeth\n"); // no StartTime
    captainFolder(in.captures, "b/Mirabeth", "log b");
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/2 a/Mirabeth: refused: manifest.txt has no StartTime."));
    CHECK(fs::exists(in.captures / "a/Mirabeth"));
    CHECK(fs::exists(in.uploaded / "b/Mirabeth"));

    // A first append Mogsafe refuses (422) is a refusal too.
    captainFolder(in.captures, "d/Mirabeth", "log d");
    server.forced["PATCH"] = { 422 };
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 2/2 d/Mirabeth: refused: forced."));
    CHECK(fs::exists(in.captures / "d/Mirabeth"));
    CHECK(in.said.back().starts_with("Captain import done: 0 uploaded, 0 already in Vault, 2 refused."));
}

TEST_CASE("an upload Mogsafe lost, at HEAD or at an append, starts over")
{
    for (const auto& [method, status] : { std::pair{ "HEAD", 404 }, std::pair{ "PATCH", 410 } })
    {
        Install    in;
        const auto log = noise(3000);
        captainFolder(in.captures, "s/Mirabeth", log);
        FakeMogsafe server;
        server.forced[method] = { status };
        in.importer(server)->run();
        CHECK(server.creates == 2);
        CHECK(unzstd(server.uploads.rbegin()->second.data) == log);
        CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
    }
}

TEST_CASE("server errors back off, and the import never gives up")
{
    Install    in;
    const auto log = noise(3000);
    captainFolder(in.captures, "s/Mirabeth", log);
    FakeMogsafe server;
    server.forced["POST"] = { 503, 500, 502 }; // creates
    server.forced["HEAD"] = { 502 };
    const auto before     = std::chrono::steady_clock::now();
    in.importer(server, std::chrono::milliseconds(20))->run();
    CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(20 + 40 + 80 + 160));
    CHECK(unzstd(server.uploads.begin()->second.data) == log);
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
}

TEST_CASE("an import that keeps failing says why while it retries")
{
    Install in;
    captainFolder(in.captures, "s/Mirabeth", noise(3000));
    FakeMogsafe server;
    server.forced["POST"] = { 503, 503, 503, 503, 503, 503 }; // creates
    const auto imp        = in.importer(server, std::chrono::milliseconds(50));
    imp->start();
    REQUIRE(waitFor([&]
                    {
                        return imp->progress().error == "create: 503";
                    }));
}

TEST_CASE("a refused token stops the import, unlinks, and the import resumes after a new link")
{
    Install in;
    captainFolder(in.captures, "s/Mirabeth", "log");
    FakeMogsafe server;
    server.token = "revoked";
    auto imp     = in.importer(server);
    imp->run();
    CHECK(imp->takeUnauthorized());
    CHECK(!fs::exists(in.config / "token"));
    CHECK(in.saidAny("this install is not linked"));
    CHECK(fs::exists(in.captures / "s/Mirabeth"));
    CHECK(Importer::pending(in.state));

    server.token = "t1";
    REQUIRE(!saveLink(in.config, { .token = "t1", .account = "player" }));
    in.importer(server)->run();
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
    CHECK(!Importer::pending(in.state));
}

TEST_CASE("an unlinked import sends nothing")
{
    Install in;
    captainFolder(in.captures, "s/Mirabeth", "log");
    fs::remove(in.config / "token");
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(server.requests == 0);
    CHECK(fs::exists(in.captures / "s/Mirabeth"));
}

TEST_CASE("a pause stops the import between chunks until uploads are on")
{
    Install    in;
    const auto log = noise(3 << 20); // three appends once compressed
    captainFolder(in.captures, "s/Mirabeth", log);
    FakeMogsafe      server;
    std::atomic<int> patches{ 0 };
    server.onPatch = [&]
    {
        if (++patches == 1)
        {
            touch(in.config / "paused");
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
    CHECK(p.character == "Mirabeth");
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
        captainFolder(in.captures, "s/Mirabeth", log);
        fs::create_directories(in.state);
        touch(in.state / "imports.txt");
        CHECK(Importer::pending(in.state));
        FakeMogsafe server;
        in.importer(server)->run();
        CHECK(server.creates == 1);
        CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
    }
    SECTION("after the create, before the first byte, and after some appends")
    {
        for (const int crashAt : { 0, 1 })
        {
            Install in;
            captainFolder(in.captures, "s/Mirabeth", log);
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
            CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
            CHECK(!Importer::pending(in.state));
        }
    }
    SECTION("closed, before the move")
    {
        Install in;
        captainFolder(in.captures, "s/Mirabeth", log);
        FakeMogsafe server;
        server.crashAfterClose = true;
        CHECK_THROWS(in.importer(server)->run());
        CHECK(fs::exists(in.captures / "s/Mirabeth"));
        server.crashAfterClose = false;
        const auto received    = server.received;
        in.importer(server)->run();
        CHECK(server.received == received);
        CHECK(server.creates == 1);
        CHECK(in.saidAny("s/Mirabeth: already in Vault (cap1)."));
        CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
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
    CHECK(u.meta.at("search_crc32") == std::format("{:08x}", crc32File(in.uploaded / "a/Hoa/SearchLog/search.log")->crc));
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
    FakeMogsafe server;
    const auto  key = [&](const std::string& rel)
    {
        return std::format("{:08x}:{}", crc32File(in.captures / rel / "PacketLogger" / "full.log")->crc, fs::file_size(in.captures / rel / "PacketLogger" / "full.log"));
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
    captainFolder(in.captures, "s/Mirabeth", log);
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
    CHECK(in.saidAny("Captain 1/1 s/Mirabeth: cannot write "));
    CHECK(server.creates == 1);
    CHECK(server.received == 0);
    CHECK(Importer::pending(in.state));
    CHECK(fs::exists(in.captures / "s/Mirabeth"));

    server.onCreate = nullptr;
    fs::remove(in.state / "imports.txt.tmp");
    in.importer(server)->run(); // the plugin resumes at the next start
    CHECK(server.creates == 2);
    const auto& u = server.uploads.rbegin()->second;
    CHECK(server.received == u.data.size());
    CHECK(unzstd(u.data) == log);
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
    CHECK(!Importer::pending(in.state));
}

TEST_CASE("an uploaded folder moves to the same path under captures-uploaded")
{
    const Install in;
    captainFolder(in.captures, "Mining/test/Mirabeth", "a");
    captainFolder(in.captures, "Mining/test/Kalnor", "b");
    auto to = moveAside(in.captures, in.uploaded, in.captures / "Mining/test/Mirabeth");
    REQUIRE(to);
    CHECK(*to == in.uploaded / "Mining/test/Mirabeth");
    CHECK(fs::exists(in.captures / "Mining/test/Kalnor")); // the parent still holds another capture

    captainFolder(in.captures, "Mining/test/Mirabeth", "c"); // the same path again: a clash
    to = moveAside(in.captures, in.uploaded, in.captures / "Mining/test/Mirabeth");
    REQUIRE(to);
    CHECK(to->filename() == "Mirabeth-2");
    CHECK(fs::exists(in.uploaded / "Mining/test/Mirabeth/manifest.txt"));

    REQUIRE(moveAside(in.captures, in.uploaded, in.captures / "Mining/test/Kalnor"));
    CHECK(fs::is_empty(in.captures));
}

TEST_CASE("a folder that cannot move stays, and the next import moves it")
{
    Install in;
    captainFolder(in.captures, "s/Mirabeth", "log");
    touch(in.uploaded); // a file where the folder goes
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/1 s/Mirabeth: in Vault, but its folder could not move: "));
    CHECK(fs::exists(in.captures / "s/Mirabeth"));
    CHECK(in.said.back().starts_with("Captain import done: 0 uploaded, 0 already in Vault, 0 refused, 1 in Vault but not moved."));

    fs::remove(in.uploaded);
    in.importer(server)->run();
    CHECK(in.saidAny("already in Vault (cap1)."));
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
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

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
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
            const std::scoped_lock lock(mutex_);
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
    captainFolder(in.captures, "s/Mirabeth", noise(3 << 20));
    {
        auto  blocking = std::make_unique<Blocking>(server, "PATCH");
        auto& http     = *blocking;
        auto  imp      = std::make_unique<Importer>(in.options(), std::move(blocking));
        auto  before   = std::chrono::steady_clock::now();
        imp->start();
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
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
    captainFolder(in.captures, "t/Mirabeth", noise(64 << 20));
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
    CHECK(!claimImportReminder(in.config, in.captures)); // nothing waiting
    CHECK(!fs::exists(in.config / "import-reminded"));

    captainFolder(in.captures, "s/Mirabeth", "log");
    fs::rename(in.config / "token", in.dir / "token");
    CHECK(!claimImportReminder(in.config, in.captures)); // not linked
    fs::rename(in.dir / "token", in.config / "token");

    for (int round = 0; round < 5; ++round) // two clients logging in together, then later logins and restarts
    {
        std::atomic<int> shown{ 0 };
        {
            std::latch                go(2);
            std::vector<std::jthread> clients;
            clients.reserve(2);
            for (int i = 0; i < 2; ++i)
            {
                clients.emplace_back([&]
                                     {
                                         go.arrive_and_wait();
                                         shown += claimImportReminder(in.config, in.captures) ? 1 : 0;
                                     });
            }
        }
        CHECK(shown == (round == 0 ? 1 : 0));
    }
}

TEST_CASE("a client whose turn another holds imports nothing, and says so only when asked")
{
    Install     in;
    FakeMogsafe server;
    captainFolder(in.captures, "s/Mirabeth", noise(3000));
    for (const bool asked : { true, false }) // /memoria import, then a resume at load
    {
        Install other;
        auto    options = other.options();
        options.mayRun  = []
        {
            return false;
        };
        Importer imp(options, std::make_unique<Forward>(server));
        imp.start(asked);
        REQUIRE(waitFor([&]
                        {
                            return imp.finished();
                        }));
        CHECK(other.saidAny("Another client sharing this install is importing.") == asked);
    }
    CHECK(server.requests == 0);

    Importer next(in.options(), std::make_unique<Forward>(server)); // the turn comes
    next.start(false);
    REQUIRE(waitFor([&]
                    {
                        return next.finished();
                    }));
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
}

TEST_CASE("a log that cannot be staged stops the import and keeps its place for the next start")
{
    Install    in;
    const auto log = noise(3 << 20);
    captainFolder(in.captures, "s/Mirabeth", log);
    const auto crc = crc32File(in.captures / "s/Mirabeth/PacketLogger/full.log");
    REQUIRE(crc);
    const auto blocker = in.state / "imports" / std::format("{:08x}-{}.zst.tmp", crc->crc, crc->size);
    fs::create_directories(blocker);
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("cannot stage the log"));
    CHECK(server.received == 0);
    CHECK(Importer::pending(in.state));
    fs::remove(blocker);
    in.importer(server)->run();
    CHECK(server.creates == 1); // the upload the first run created
    CHECK(server.received == server.uploads.begin()->second.data.size());
    CHECK(fs::exists(in.uploaded / "s/Mirabeth"));
    CHECK(!Importer::pending(in.state));
}

TEST_CASE("a search log changed after an interrupted create goes up as it is now, with metadata that matches")
{
    Install    in;
    const auto log = noise(1 << 20);
    const auto dir = captainFolder(in.captures, "s/Hoa", log);
    searchFolder(dir, "old search");
    FakeMogsafe server;
    server.down   = true; // the create never answers
    auto importer = in.importer(server);
    importer->start();
    REQUIRE(waitFor([&]
                    {
                        std::error_code ec;
                        for (const auto& e : fs::directory_iterator(in.state / "imports", ec))
                        {
                            if (e.path().string().ends_with(".search.zst"))
                            {
                                return true;
                            }
                        }
                        return false;
                    }));
    importer.reset(); // the client stops
    const std::string now = "new search, longer than the old one";
    searchFolder(dir, now);
    server.down = false;
    in.importer(server)->run();
    REQUIRE(server.uploads.size() == 1);
    const auto& u = server.uploads.begin()->second;
    CHECK(searchParts(u) == std::pair{ now, log });
    CHECK(u.meta.at("search_crc32") == std::format("{:08x}", crc32File(in.uploaded / "s/Hoa/SearchLog/search.log")->crc));
    CHECK(u.meta.at("search_size") == std::to_string(now.size()));
}

TEST_CASE("a manifest.txt larger than captain writes is refused for its folder, and the next one goes")
{
    Install in;
    captainFolder(in.captures, "a/Hoa", noise(1000), std::string("StartTime = 1\n") + std::string(size_t{ 100 } * 1024, 'm'));
    captainFolder(in.captures, "b/Ijs", noise(1000) + "b");
    FakeMogsafe server;
    in.importer(server)->run();
    CHECK(in.saidAny("Captain 1/2 a/Hoa: its manifest.txt is over"));
    CHECK(server.creates == 1);
    CHECK(fs::exists(in.captures / "a/Hoa"));
    CHECK(fs::exists(in.uploaded / "b/Ijs"));
}

TEST_CASE("a new create never sends a stage an earlier one made, with or without a search log now")
{
    for (const bool removed : { true, false })
    {
        INFO("search.log " << (removed ? "removed" : "emptied"));
        Install           in;
        const std::string log = "full log data";
        const auto        dir = captainFolder(in.captures, "s/Hoa", log);
        searchFolder(dir, "old search data");
        FakeMogsafe server;
        server.crashAtPatch = 0;
        CHECK_THROWS(in.importer(server)->run());
        REQUIRE(server.creates == 1);
        server.uploads.clear(); // the upload expired
        server.crashAtPatch = -1;
        if (removed)
        {
            fs::remove(dir / "SearchLog" / "search.log");
        }
        else
        {
            searchFolder(dir, "");
        }
        in.importer(server)->run();
        REQUIRE(server.creates == 2);
        const auto& u = server.uploads.at("/v1/uploads/2");
        CHECK(!u.meta.contains("search_bytes"));
        CHECK(unzstd(u.data) == log);
    }
}

TEST_CASE("an old stage that cannot be removed stops the import before a new create")
{
    Install           in;
    const std::string log = "full log data";
    const auto        dir = captainFolder(in.captures, "s/Hoa", log);
    FakeMogsafe       server;
    server.crashAtPatch = 0;
    CHECK_THROWS(in.importer(server)->run());
    server.uploads.clear();
    server.crashAtPatch = -1;
    const auto crc      = crc32File(dir / "PacketLogger" / "full.log");
    const auto stage    = in.state / "imports" / std::format("{:08x}-{}.zst", crc->crc, crc->size);
    REQUIRE(fs::remove(stage));
    fs::create_directories(stage / "inside"); // a folder that is not empty cannot be removed
    in.importer(server)->run();
    CHECK(in.saidAny("cannot remove an old stage"));
    CHECK(server.creates == 1);
    CHECK(Importer::pending(in.state));
    fs::remove_all(stage);
    in.importer(server)->run();
    CHECK(server.creates == 2);
    CHECK(unzstd(server.uploads.at("/v1/uploads/2").data) == log);
}
