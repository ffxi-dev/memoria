#pragma once

#include "core/http.h"
#include "core/tus.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace memoria
{

// A folder captain's PacketLogger wrote: manifest.txt and PacketLogger/full.log.
struct CaptainCapture
{
    std::filesystem::path dir;
    std::string           folder;    // relative to captain's captures folder, with forward slashes
    std::string           character; // the folder's name
    uint64_t              size{};    // of full.log
    bool                  growing{}; // full.log changed within the quiet time: PacketLogger may still be writing it
};

// Every captain capture under root, at any depth, in path order. A match's own subfolders are not searched.
auto findCaptain(const std::filesystem::path& root, std::chrono::seconds quiet = std::chrono::minutes(10)) -> std::vector<CaptainCapture>;

struct Crc32
{
    uint32_t crc{};
    uint64_t size{};
};

// CRC-32 (IEEE, as zip uses) of a file; nothing if it cannot be read or stop is set.
auto crc32File(const std::filesystem::path& path, const std::atomic<bool>& stop) -> std::optional<Crc32>;

// Moves dir, under root, to the same relative path under uploaded, with -2, -3... on a clash, then removes the folders it leaves empty up to root.
auto moveAside(const std::filesystem::path& root, const std::filesystem::path& uploaded, const std::filesystem::path& dir) -> std::expected<std::filesystem::path, std::string>;

// True for one caller only, ever, per install: while linked and a capture waits, the first to create config's import-reminded file wins.
auto remindImport(const std::filesystem::path& config, const std::filesystem::path& root) -> bool;

auto sizeText(uint64_t bytes) -> std::string;

struct ImportOptions
{
    std::filesystem::path            captures; // <Ashita>/addons/captain/captures
    std::filesystem::path            uploaded; // <Ashita>/addons/captain/captures-uploaded
    std::filesystem::path            state;    // <Ashita>/captures/memoria: imports.txt and the staged logs
    std::string                      server;
    std::filesystem::path            config;
    std::function<void(std::string)> say;
    std::chrono::milliseconds        retry{ 5000 };
    std::chrono::seconds             quiet{ std::chrono::minutes(10) };
};

struct ImportProgress
{
    size_t      index{}; // from 1; 0 while looking
    size_t      count{};
    std::string character;
    uint64_t    sent{};
    uint64_t    size{}; // of the staged log; 0 until staged
};

// /memoria import: uploads captain's captures to Mogsafe as captain imports, one at a time, and moves each one Mogsafe holds to captures-uploaded.
class Importer
{
public:
    Importer(ImportOptions options, std::unique_ptr<Http> http);
    ~Importer(); // cancels the request in flight and joins

    Importer(const Importer&)                    = delete;
    auto operator=(const Importer&) -> Importer& = delete;

    // An import was asked for and not finished.
    static auto pending(const std::filesystem::path& state) -> bool;

    auto start() -> void; // run() on its own thread, below normal priority
    auto run() -> void;

    [[nodiscard]] auto finished() const -> bool;
    [[nodiscard]] auto progress() const -> ImportProgress;
    [[nodiscard]] auto unauthorized() -> bool; // true once when Mogsafe refused the token, which is then deleted

private:
    enum class Outcome : uint8_t
    {
        Uploaded,
        InVault,
        Refused,
        Skipped,
        NotMoved,
        Stopped,
        Failed,
    };

    struct Entry
    {
        std::string crc; // 8 hex digits
        uint64_t    size{};
        std::string url;
    };

    auto upload(const CaptainCapture& c, const std::string& prefix) -> Outcome;
    auto moved(const CaptainCapture& c, const std::string& prefix, const std::string& done, Outcome outcome) -> Outcome;
    auto stage(const std::filesystem::path& log, const std::filesystem::path& out) -> std::optional<std::string>;
    auto ready() -> bool;
    auto wait(std::chrono::milliseconds time) -> void;
    auto say(const std::string& text) -> void;
    auto load() -> void;
    auto save() -> void;

    ImportOptions                options_;
    std::unique_ptr<Http>        http_;
    Tus                          tus_;
    std::map<std::string, Entry> entries_; // by folder; importer thread only

    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    std::string             character_;

    std::atomic<size_t>   index_{ 0 };
    std::atomic<size_t>   count_{ 0 };
    std::atomic<uint64_t> sent_{ 0 };
    std::atomic<uint64_t> size_{ 0 };
    std::atomic<bool>     stopping_{ false };
    std::atomic<bool>     finished_{ false };
    std::thread           thread_;
};

} // namespace memoria
