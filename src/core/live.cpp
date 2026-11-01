#include "core/live.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cstring>
#include <format>
#include <utility>

namespace memoria
{
namespace
{

static_assert(std::endian::native == std::endian::little, "frames are little-endian");

template <typename T>
auto append(std::vector<uint8_t>& out, T value) -> void
{
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

} // namespace

auto startLive(Http& http, const std::string& server, const std::string& installToken) -> std::expected<LiveLink, std::string>
{
    const auto res = http.request("POST", server + "/v1/live", { { "Authorization", "Bearer " + installToken } }, {});
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto body = nlohmann::json::parse(res->body, nullptr, false);
    if (res->status != 201 || !body.is_object() || body.value("token", "").empty() || !body.value("url", "").starts_with("https://"))
    {
        return std::unexpected(std::format("Mogsafe answered {}", res->status));
    }
    return LiveLink{ .token = body.value("token", ""), .url = body.value("url", "") };
}

auto liveHello(std::string_view character, std::string_view world, std::string_view clientVersion, std::string_view agent) -> std::string
{
    return nlohmann::json{ { "hello", { { "character", character }, { "world", world }, { "client_version", clientVersion }, { "agent", agent } } } }.dump();
}

auto liveViewers(std::string_view message) -> std::optional<int>
{
    const auto json = nlohmann::json::parse(message, nullptr, false);
    if (!json.is_object() || !json.contains("viewers") || !json["viewers"].is_number_integer())
    {
        return std::nullopt;
    }
    return json["viewers"].get<int>();
}

auto liveFrame(const xicap::OutPacket& packet) -> std::vector<uint8_t>
{
    const auto  stored = xicap::applyPolicy(xicap::defaultExclusionPolicy(), packet); // the policy Recorder gives its writers
    const auto* data   = reinterpret_cast<const uint8_t*>(stored.data.data());

    std::vector<uint8_t> frame;
    frame.reserve(16 + stored.data.size());
    append<uint8_t>(frame, 1);
    append<uint8_t>(frame, packet.channel);
    append<uint16_t>(frame, stored.flags);
    append<uint64_t>(frame, packet.timestamp);
    append<uint32_t>(frame, static_cast<uint32_t>(stored.data.size()));
    frame.insert(frame.end(), data, data + stored.data.size());
    return frame;
}

LiveFeed::LiveFeed(size_t maxBytes)
: maxBytes_(maxBytes)
{
}

auto LiveFeed::push(const xicap::OutPacket& packet) -> void
{
    if (!open_)
    {
        return;
    }
    std::vector<uint8_t> frame;
    try
    {
        frame = liveFrame(packet);
    }
    catch (const std::exception&) // a packet the policy refuses is a gap for viewers, never an error in the capture
    {
        std::lock_guard lock(mutex_);
        dropped_++;
        return;
    }
    std::lock_guard lock(mutex_);
    if (!open_)
    {
        return;
    }
    if (bytes_ + frame.size() > maxBytes_)
    {
        dropped_++;
        return;
    }
    bytes_ += frame.size();
    if (messages_.empty() || messages_.back().size() + frame.size() > MESSAGE_BYTES)
    {
        messages_.push_back(std::move(frame));
    }
    else
    {
        messages_.back().insert(messages_.back().end(), frame.begin(), frame.end());
    }
}

auto LiveFeed::open(bool on) -> void
{
    std::lock_guard lock(mutex_);
    open_ = on;
    if (!on)
    {
        messages_.clear();
        bytes_ = 0;
    }
}

auto LiveFeed::take() -> std::vector<std::vector<uint8_t>>
{
    std::lock_guard lock(mutex_);
    bytes_ = 0;
    return std::exchange(messages_, {});
}

auto LiveFeed::takeDropped() -> uint64_t
{
    std::lock_guard lock(mutex_);
    return std::exchange(dropped_, 0);
}

} // namespace memoria
