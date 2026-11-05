#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace memoria::captain
{

// A folder captain's PacketLogger wrote: manifest.txt and PacketLogger/full.log, and SearchLog/search.log when SearchLog ran.
struct CaptainCapture
{
    std::filesystem::path dir;
    std::string           folder;       // relative to captain's captures folder, with forward slashes
    std::string           character;    // the folder's name
    uint64_t              size{};       // of full.log
    uint64_t              searchSize{}; // of SearchLog/search.log; 0 when there is none, which sends none
    bool                  growing{};    // full.log changed within the quiet time, or a log's size could not be read: PacketLogger may still be writing it
};

// Every captain capture under root, at any depth, in path order. A match's own subfolders are not searched. Stops early,
// with what it found so far, once stop is requested.
auto findCaptures(const std::filesystem::path& root, std::chrono::seconds quiet = std::chrono::minutes(10), const std::stop_token& stop = {}) -> std::vector<CaptainCapture>;

struct Crc32
{
    uint32_t crc{};
    uint64_t size{};
};

// CRC-32 (IEEE, as zip uses) of a file; nothing if it cannot be read or stop is requested.
auto crc32File(const std::filesystem::path& path, const std::stop_token& stop = {}) -> std::optional<Crc32>;

// Moves dir, under root, to the same relative path under uploaded, with -2, -3... on a clash, then removes the folders it leaves empty up to root.
auto moveAside(const std::filesystem::path& root, const std::filesystem::path& uploaded, const std::filesystem::path& dir) -> std::expected<std::filesystem::path, std::string>;

// True for one caller only, ever, per install: while linked and a capture waits, the first to create config's import-reminded file wins.
auto claimImportReminder(const std::filesystem::path& config, const std::filesystem::path& root, const std::stop_token& stop = {}) -> bool;

} // namespace memoria::captain
