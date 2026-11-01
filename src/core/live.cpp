#include "core/live.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <span>
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

// xicap applies its policy inside Writer::write and exports only the rules, so this follows its writer.cpp; a test checks it against what the writer stores.
auto liveFrame(const xicap::OutPacket& packet) -> std::vector<uint8_t>
{
    std::span  data{ reinterpret_cast<const uint8_t*>(packet.data.data()), packet.data.size() };
    const bool game   = packet.channel == std::to_underlying(xicap::Channel::Game);
    const auto opcode = game && data.size() >= 2 ? static_cast<uint16_t>((data[0] | data[1] << 8) & 0x1FF) : packet.opcode;
    auto       flags  = static_cast<uint16_t>(packet.provenance & xicap::PacketFlag::Provenance);
    if (packet.direction == xicap::PacketDirection::ClientToServer)
    {
        flags |= xicap::PacketFlag::ClientToServer;
    }

    std::vector<uint8_t> redacted;
    const auto&          rules = xicap::defaultExclusionPolicy().rules; // the policy Recorder gives its writers
    const auto           rule  = std::ranges::find_if(rules, [&](const xicap::ExclusionRule& r)
                                                      {
                                               return r.channel == packet.channel && r.opcode == opcode &&
                                                      (r.direction == xicap::ExclusionDirection::Both || std::to_underlying(r.direction) == std::to_underlying(packet.direction));
                                                      });
    if (rule != rules.end() && rule->action == xicap::ExclusionAction::Stub)
    {
        data = data.first((std::min)(data.size(), sizeof(xicap::FFXIPacketHeader)));
        flags |= xicap::PacketFlag::Excluded;
    }
    else if (rule != rules.end())
    {
        redacted.assign(data.begin(), data.end());
        for (const auto& range : rule->ranges)
        {
            if (range.offset < redacted.size())
            {
                std::fill_n(redacted.begin() + range.offset, (std::min)(uint64_t{ range.length }, uint64_t{ redacted.size() - range.offset }), uint8_t{ 0 });
            }
        }
        data = redacted;
        flags |= xicap::PacketFlag::Redacted;
    }

    std::vector<uint8_t> frame;
    frame.reserve(16 + data.size());
    append<uint8_t>(frame, 1);
    append<uint8_t>(frame, packet.channel);
    append<uint16_t>(frame, flags);
    append<uint64_t>(frame, packet.timestamp);
    append<uint32_t>(frame, static_cast<uint32_t>(data.size()));
    frame.insert(frame.end(), data.begin(), data.end());
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
    auto            frame = liveFrame(packet);
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
