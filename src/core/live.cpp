#include "core/live.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cstring>
#include <format>
#include <stdexcept>
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
try
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
catch (const nlohmann::json::exception&) // a field of the wrong type
{
    return std::unexpected("Mogsafe sent a malformed answer");
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

auto liveFrame(const xicap::WritePacket& packet, const xicap::Chat chat) -> std::vector<uint8_t>
{
    auto data  = std::vector<uint8_t>(packet.data.begin(), packet.data.end());
    auto flags = static_cast<uint16_t>((packet.provenance & xicap::PacketFlag::Provenance) | std::to_underlying(packet.direction));
    if (packet.channel == xicap::Channel::Game)
    {
        if (data.size() < 4)
        {
            throw std::invalid_argument("a game packet is at least 4 bytes");
        }
        if (chat == xicap::Chat::Redact && xicap::redact(data, packet.direction))
        {
            flags |= xicap::PacketFlag::Redacted;
        }
    }

    std::vector<uint8_t> frame;
    frame.reserve(16 + data.size());
    append<uint8_t>(frame, 1);
    append<uint8_t>(frame, std::to_underlying(packet.channel));
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

auto LiveFeed::push(const xicap::WritePacket& packet, const xicap::Chat chat) -> void
{
    if (!open_)
    {
        return;
    }
    std::vector<uint8_t> frame;
    try
    {
        frame = liveFrame(packet, chat);
    }
    catch (const std::exception&) // a packet too short to frame is a gap for viewers, never an error in the capture
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

auto LiveFeed::claim() -> uint64_t
{
    std::lock_guard lock(mutex_);
    open_ = false;
    messages_.clear();
    bytes_   = 0;
    dropped_ = 0;
    return ++owner_;
}

auto LiveFeed::open(uint64_t owner, bool on) -> void
{
    std::lock_guard lock(mutex_);
    if (owner != owner_)
    {
        return;
    }
    open_ = on;
    if (!on)
    {
        messages_.clear();
        bytes_ = 0;
    }
}

auto LiveFeed::take(uint64_t owner) -> std::vector<std::vector<uint8_t>>
{
    std::lock_guard lock(mutex_);
    if (owner != owner_)
    {
        return {};
    }
    bytes_ = 0;
    return std::exchange(messages_, {});
}

auto LiveFeed::takeDropped(uint64_t owner) -> uint64_t
{
    std::lock_guard lock(mutex_);
    if (owner != owner_)
    {
        return 0;
    }
    return std::exchange(dropped_, 0);
}

} // namespace memoria
