#pragma once

#include "core/http.h"

#include <xicap/writer.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Live viewing, as vault's openspec/changes/add-memoria-live/design.md, Protocol, defines it.
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

// The frame of a packet as the capture stores it.
auto liveFrame(const xicap::OutPacket& packet) -> std::vector<uint8_t>;

// Frames from the writer thread to the live thread. Takes nothing while closed; past maxBytes, drops and counts.
class LiveFeed
{
public:
    static constexpr size_t MESSAGE_BYTES = 64 * 1024;

    explicit LiveFeed(size_t maxBytes = 1024 * 1024);

    auto push(const xicap::OutPacket& packet) -> void;
    auto open(bool on) -> void;                       // on while someone watches; off empties the feed
    auto take() -> std::vector<std::vector<uint8_t>>; // frames back to back, at most MESSAGE_BYTES a message unless one frame is larger
    auto takeDropped() -> uint64_t;

private:
    std::atomic<bool>                 open_{ false };
    std::mutex                        mutex_;
    std::vector<std::vector<uint8_t>> messages_;
    size_t                            bytes_{ 0 };
    size_t                            maxBytes_;
    uint64_t                          dropped_{ 0 };
};

} // namespace memoria
