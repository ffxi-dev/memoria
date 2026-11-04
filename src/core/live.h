#pragma once

#include "core/http.h"

#include <xicap/policy.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace memoria
{

struct LiveLink
{
    std::string token;
    std::string url;
};

// Asks Mogsafe for a live link with the install's token.
auto startLive(Http& http, const std::string& server, const std::string& installToken) -> std::expected<LiveLink, std::string>;

auto liveHello(std::string_view character, std::string_view world, std::string_view clientVersion, std::string_view agent) -> std::string;

// Mogsafe's {"viewers": n}; nothing for any other message.
auto liveViewers(std::string_view message) -> std::optional<int>;

// One packet as live viewers get it, stored as a capture written with policy stores it. Little-endian: version 1 (u8),
// channel (u8), flags (u16), timestamp in Unix ms (u64), length (u32), then the packet.
auto liveFrame(const xicap::OutPacket& packet, const xicap::ExclusionPolicy& policy) -> std::vector<uint8_t>;

// Frames from the writer thread to the live thread. Takes nothing while closed; past maxBytes, drops and counts.
class LiveFeed
{
public:
    static constexpr size_t MESSAGE_BYTES = 64 * 1024;

    explicit LiveFeed(size_t maxBytes = 1024 * 1024);

    auto push(const xicap::OutPacket& packet, const xicap::ExclusionPolicy& policy) -> void;

    // The rest belong to the live link that claimed the feed last: one stopped but still closing changes nothing.
    auto claim() -> uint64_t;                                       // closes the feed and returns the new owner's id
    auto open(uint64_t owner, bool on) -> void;                     // on while someone watches; off empties the feed
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

} // namespace memoria
