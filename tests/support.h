#pragma once

#include "core/account/chat.h"
#include "core/account/link.h"
#include "core/captain/captain.h"
#include "core/captain/importer.h"
#include "core/capture/clock.h"
#include "core/capture/event_queue.h"
#include "core/capture/recorder.h"
#include "core/file.h"
#include "core/live/live.h"
#include "core/units.h"
#include "core/upload/uploader.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <picosha2.h>
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

namespace fs = std::filesystem;

namespace memoria::test
{

using namespace memoria::account;
using namespace memoria::capture;
using namespace memoria::captain;
using namespace memoria::upload;

// Creates path as an empty file, or empties it.
inline auto touch(const fs::path& path) -> void
{
    const std::ofstream file(path);
}

// Whether only the user running the tests can open path: what createNew gives a secret.
inline auto ownerOnly(const fs::path& path) -> bool
{
#ifdef _WIN32
    const auto free = [](void* p)
    {
        ::LocalFree(p);
    };
    HANDLE token{ nullptr };
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    DWORD size{ 0 };
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    wchar_t*               me{ nullptr };
    if (size > 0 && ::GetTokenInformation(token, TokenUser, user.data(), size, &size))
    {
        ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &me);
    }
    ::CloseHandle(token);
    const std::unique_ptr<wchar_t, decltype(free)> ownedMe(me, free);
    PACL                                           dacl{ nullptr };
    PSECURITY_DESCRIPTOR                           sd{ nullptr };
    if (me == nullptr || ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS)
    {
        return false;
    }
    const std::unique_ptr<void, decltype(free)> ownedSd(sd, free);
    SECURITY_DESCRIPTOR_CONTROL                 control{ 0 };
    DWORD                                       revision{ 0 };
    if (dacl == nullptr || !::GetSecurityDescriptorControl(sd, &control, &revision) || (control & SE_DACL_PROTECTED) == 0)
    {
        return false;
    }
    for (DWORD i = 0; i < dacl->AceCount; ++i)
    {
        void* ace{ nullptr };
        if (!::GetAce(dacl, i, &ace) || static_cast<ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE)
        {
            return false;
        }
        wchar_t* text{ nullptr };
        ::ConvertSidToStringSidW(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, &text);
        const std::unique_ptr<wchar_t, decltype(free)> name(text, free);
        if (!text || std::wstring_view(me) != text)
        {
            return false;
        }
    }
    return true;
#else
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && (st.st_mode & 077) == 0;
#endif
}

inline auto tempDir() -> fs::path
{
    // ctest runs each test in its own process: a counter alone would repeat names.
    static const auto       run = std::random_device{}();
    static std::atomic<int> counter{ 0 };
    auto                    dir = fs::temp_directory_path() / (std::format("memoria-test-{}-{}", run, counter++));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

inline auto readAll(const fs::path& path) -> std::vector<uint8_t>
{
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), {} };
}

inline auto packet(const size_t size) -> Event
{
    return Event{ .kind = EventKind::Packet, .data = std::vector<uint8_t>(size, 1) };
}

inline constexpr uint64_t T0 = 1'790'000'000'000ULL;

inline auto options(const fs::path& dir) -> RecorderOptions
{
    RecorderOptions o;
    o.dir          = dir;
    o.captureAgent = "Memoria test";
    o.flushEvery   = std::chrono::milliseconds(20);
    o.drainEvery   = std::chrono::milliseconds(5);
    return o;
}

inline auto who(const char* name, const uint32_t id) -> Login
{
    return {
        .character     = { .contentId = id, .characterId = id, .name = name, .world = "Asura" },
        .worldId       = 42,
        .serverKind    = xicap::ServerKind::Retail,
        .clientVersion = "30261006_0",
    };
}

inline auto gamePacket(const uint64_t t, uint16_t opcode = 0x015) -> Event
{
    std::vector<uint8_t> data(16, 0xAB);
    const auto           hdr = static_cast<uint16_t>((4 << 9) | opcode);
    data[0]                  = static_cast<uint8_t>(hdr);
    data[1]                  = static_cast<uint8_t>(hdr >> 8);
    return Event{ .timestamp = t, .data = std::move(data) };
}

inline auto captures(const fs::path& dir) -> std::vector<fs::path>
{
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".xicap")
        {
            out.push_back(e.path());
        }
    }
    // By name, which starts with the capture's first timestamp: two files closed within the clock's tick share a write time.
    std::ranges::sort(out);
    return out;
}

struct Read
{
    std::vector<xicap::MarkerKind> markers;
    uint64_t                       packets{};
    bool                           complete{};
    bool                           atLogin{}; // a login marker comes before the first packet
    xicap::CaptureId               id{};
    xicap::CaptureId               previous{};
    std::string                    name;
};

inline auto readCapture(const fs::path& path) -> Read
{
    xicap::Reader r(path);
    const auto&   meta = r.metadata();
    Read          out{ .id = meta.captureId, .previous = meta.previousCaptureId, .name = meta.character.name };
    for (const auto& p : r)
    {
        out.atLogin = out.atLogin || (out.packets == 0 && std::ranges::find(p.markers, xicap::MarkerKind::Login, &xicap::Marker::kind) != p.markers.end());
        out.packets++;
        for (const auto& m : p.markers)
        {
            out.markers.push_back(m.kind);
        }
    }
    out.complete = r.complete();
    return out;
}

// Stands in for a capture's trailer.
inline const auto TRAILER = std::string("XCPF") + std::string(20, 't');

inline auto fromBase64(const std::string_view in) -> std::string
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    uint32_t                   v{ 0 };
    int                        bits{ 0 };
    for (const char c : in)
    {
        if (c == '=')
        {
            break;
        }
        v = v << 6 | static_cast<uint32_t>(digits.find(c));
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out += static_cast<char>(v >> bits & 0xFF);
        }
    }
    return out;
}

// Mogsafe's upload protocol in memory. Like the real one, a close wants the file's hash and a complete capture: here, one
// whose last 24 bytes start with the trailer magic XCPF.
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
    std::set<std::string>                   foreign;     // uploads another account made: every request answers 403
    std::set<std::thread::id>               threads;
    std::string                             token{ "t0ken" };
    std::string                             foreignEnd; // an upload that answers 410 with Mogsafe-Ended: refused
    std::string                             origin;     // put before a create's Location: tusd behind a tunnel names http://
    std::vector<std::string>                strayed;    // URLs asked for anywhere but https://mogsafe.test
    bool                                    down{};
    bool                                    crashAfterClose{}; // the game dies once a close is accepted, before its answer
    int                                     creates{};
    int                                     requests{};
    int                                     patches{};
    uint64_t                                received{};         // bytes accepted by appends
    int                                     crashAtPatch{ -1 }; // the game dies during this PATCH, before any answer
    int                                     cutAtPatch{ -1 };   // this PATCH keeps half its body, then the connection drops
    std::function<void()>                   onPatch;
    std::function<void()>                   onCreate;

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        ++requests;
        if (!url.starts_with("https://mogsafe.test/"))
        {
            strayed.push_back(url);
            return std::unexpected("not Mogsafe");
        }
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
            HttpResponse res{ .status = 201, .headers = { { "location", origin + path } } };
            if (!replaces.empty())
            {
                res.headers.emplace("mogsafe-replaces", replaces);
            }
            return res;
        }
        const auto path = url.substr(std::string("https://mogsafe.test").size());
        const bool isClose{ path.ends_with("/close") };
        const auto it = uploads.find(isClose ? path.substr(0, path.size() - 6) : path);
        if (it == uploads.end())
        {
            return HttpResponse{ .status = 404 };
        }
        auto& u = it->second;
        if (it->first == foreignEnd)
        {
            return HttpResponse{ .status = 410, .headers = { { "mogsafe-ended", "refused" } } };
        }
        if (foreign.contains(it->first))
        {
            return HttpResponse{ .status = 403 };
        }
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
            const auto patch = patches++;
            if (patch == crashAtPatch)
            {
                throw std::runtime_error("the game crashed");
            }
            if (get("Upload-Offset") != std::to_string(u.data.size()))
            {
                return HttpResponse{ .status = 409 };
            }
            if (patch == cutAtPatch)
            {
                u.data.insert(u.data.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(body.size() / 2));
                received += body.size() / 2;
                return std::unexpected("connection reset");
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
        const bool complete{ u.data.size() >= TRAILER.size() && std::string(u.data.end() - static_cast<std::ptrdiff_t>(TRAILER.size()), u.data.end()).starts_with("XCPF") };
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

    auto request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string> override
    {
        return to_.request(method, url, headers, body);
    }

private:
    FakeMogsafe& to_;
};

inline auto append(const fs::path& path, const std::string& bytes) -> void
{
    std::ofstream(path, std::ios::binary | std::ios::app) << bytes;
}

inline auto uploader(const fs::path& dir, FakeMogsafe& server) -> std::unique_ptr<Uploader>
{
    if (!fs::exists(dir / "config" / "token"))
    {
        REQUIRE(!saveLink(dir / "config", { .token = "t0ken", .account = "player" }));
    }
    UploadOptions options;
    options.dir    = dir;
    options.server = "https://mogsafe.test";
    options.config = dir / "config";
    options.idle   = std::chrono::milliseconds(0);
    return std::make_unique<Uploader>(std::move(options), std::make_unique<Forward>(server));
}

inline auto outPacket(const Event& e) -> xicap::WritePacket
{
    return { .timestamp = e.timestamp, .direction = e.direction, .provenance = e.provenance, .channel = e.channel, .opcode = e.opcode, .data = e.data };
}

inline auto noise(const size_t n) -> std::string
{
    std::mt19937 random(42);
    std::string  out(n, '\0');
    for (auto& c : out)
    {
        c = static_cast<char>(random());
    }
    return out;
}

inline auto waitFor(const std::function<bool()>& done) -> bool
{
    for (int i = 0; i < 1000 && !done(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

} // namespace memoria::test
