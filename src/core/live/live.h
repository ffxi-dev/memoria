#pragma once

#include "core/http.h"

#include <xicap/xicap.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace memoria::live
{

struct Link
{
    std::string token;
    std::string url;
};

// A 401 deletes installToken from config.
auto start(Http& http, const std::string& server, const std::filesystem::path& config, const std::string& installToken) -> std::expected<Link, std::string>;

auto hello(std::string_view character, std::string_view world, std::string_view clientVersion, std::string_view agent) -> std::string;

// Mogsafe's {"viewers": n}; nothing for any other message.
auto viewers(std::string_view message) -> std::optional<int>;

// Redacted as a capture with that chat setting stores it. Little-endian: version 1 (u8), channel (u8), flags (u16), Unix
// ms (u64), length (u32), then the packet. Nothing for a game packet under 4 bytes, which has no header to redact by.
auto frame(const xicap::WritePacket& packet, xicap::Chat chat) -> std::optional<std::vector<uint8_t>>;

// Stops at the first failed send or a stop request; true if every message went.
auto sendAll(const std::vector<std::vector<uint8_t>>& messages, const std::stop_token& stop, const std::function<bool(std::span<const uint8_t>)>& send) -> bool;

// Frames from the writer thread to the live thread. Takes nothing while closed; past maxBytes, drops and counts.
class Feed
{
public:
    static constexpr size_t MESSAGE_BYTES = size_t{ 64 } * 1024;

    explicit Feed(size_t maxBytes = size_t{ 1024 } * 1024);

    auto push(const xicap::WritePacket& packet, xicap::Chat chat) -> void;

    // These act for the link that claimed the feed last, so an old one still closing changes nothing.
    auto claim() -> uint64_t;                                       // closes the feed and returns the new owner's id
    auto setOpen(uint64_t owner, bool on) -> void;                  // on while someone watches; off empties the feed
    auto take(uint64_t owner) -> std::vector<std::vector<uint8_t>>; // frames back to back, at most MESSAGE_BYTES a message unless one frame is larger
    auto takeDropped(uint64_t owner) -> uint64_t;

private:
    std::atomic<bool>                 open_{ false };
    std::mutex                        mutex_;
    std::vector<std::vector<uint8_t>> messages_;
    size_t                            bytes_{ 0 };
    size_t                            maxBytes_;
    uint64_t                          dropped_{ 0 };
    uint64_t                          owner_{ 0 };
};

} // namespace memoria::live
