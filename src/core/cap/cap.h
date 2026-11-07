#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace memoria::cap
{

struct Cap
{
    std::string           id;
    std::string           name;
    std::string           stopWith; // the command that stops it on this host
    uint64_t              startMs{};
    std::filesystem::path recording; // the recording it started in
};

// What follows "cap start": the name, then the path from the first word that holds a slash.
struct StartArgs
{
    std::string name;
    std::string path;
};

auto parseStart(std::string_view text) -> StartArgs;

// 12 characters of URL-safe base64, from Mogsafe's id alphabet.
auto newId() -> std::string;

// cap_20261107-0736
auto defaultName(uint64_t nowMs) -> std::string;

auto startMarker(const Cap& cap) -> std::string;
auto stopMarker(const Cap& cap) -> std::string;

// A folder gets memoria.xicap; a path that ends in .xicap is the file itself.
auto fileIn(const std::filesystem::path& given) -> std::filesystem::path;

// <name>_<character>_<UTC yyyymmdd-hhmm>.xicap in dir, with -2, -3 and on while the name is taken.
auto freeFile(const std::filesystem::path& dir, std::string_view name, std::string_view character, uint64_t nowMs) -> std::filesystem::path;

// 12s, 4m 12s, 1h 5m
auto lengthText(uint64_t ms) -> std::string;

auto page(std::string_view server, const Cap& cap) -> std::string;

} // namespace memoria::cap
