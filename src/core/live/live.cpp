#include "core/live/live.h"

#include "core/install.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <tuple>
#include <utility>

namespace memoria::live
{
namespace
{

static_assert(std::endian::native == std::endian::little, "frames are little-endian");

template <typename T>
auto append(std::vector<uint8_t>& out, const T value) -> void
{
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

} // namespace

auto start(Http& http, const std::string& server, const std::filesystem::path& config, const std::string& installToken) -> std::expected<Link, std::string>
try
{
    auto res = http.request("POST", server + "/v1/live", { { "Authorization", "Bearer " + installToken } }, {});
    if (res && res->status == 401)
    {
        std::ignore = removeLink(config, installToken);
        return std::unexpected(REFUSED);
    }
    res = expect(std::move(res), 201);
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto body = nlohmann::json::parse(res->body, nullptr, false);
    if (!body.is_object() || body.value("token", "").empty() || !body.value("url", "").starts_with("https://"))
    {
        return std::unexpected(MALFORMED);
    }
    return Link{ .token = body.value("token", ""), .url = body.value("url", "") };
}
catch (const nlohmann::json::exception&) // a field of the wrong type
{
    return std::unexpected(MALFORMED);
}

auto hello(const std::string_view character, const std::string_view world, const std::string_view clientVersion, const std::string_view agent) -> std::string
{
    return nlohmann::json{ { "hello", { { "character", character }, { "world", world }, { "client_version", clientVersion }, { "agent", agent } } } }.dump();
}

auto viewers(const std::string_view message) -> std::optional<int>
{
    const auto json = nlohmann::json::parse(message, nullptr, false);
    if (!json.is_object() || !json.contains("viewers") || !json["viewers"].is_number_integer())
    {
        return std::nullopt;
    }
    return json["viewers"].get<int>();
}

auto frame(const xicap::WritePacket& packet, const xicap::Chat chat) -> std::optional<std::vector<uint8_t>>
{
    auto data  = std::vector<uint8_t>(packet.data.begin(), packet.data.end());
    auto flags = static_cast<uint16_t>((packet.provenance & xicap::PacketFlag::Provenance) | std::to_underlying(packet.direction));
    if (packet.channel == xicap::Channel::Game)
    {
        if (data.size() < 4)
        {
            return std::nullopt;
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

Feed::Feed(const size_t maxBytes)
: maxBytes_(maxBytes)
{
}

auto Feed::push(const xicap::WritePacket& packet, const xicap::Chat chat) -> void
{
    if (!open_)
    {
        return;
    }
    auto                   bytes = frame(packet, chat);
    const std::scoped_lock lock(mutex_);
    if (!bytes)
    {
        dropped_++; // a gap for viewers
        return;
    }
    if (!open_)
    {
        return;
    }
    if (bytes_ + bytes->size() > maxBytes_)
    {
        dropped_++;
        return;
    }
    bytes_ += bytes->size();
    if (messages_.empty() || messages_.back().size() + bytes->size() > MESSAGE_BYTES)
    {
        messages_.push_back(std::move(*bytes));
    }
    else
    {
        messages_.back().insert(messages_.back().end(), bytes->begin(), bytes->end());
    }
}

auto Feed::claim() -> uint64_t
{
    const std::scoped_lock lock(mutex_);
    open_ = false;
    messages_.clear();
    bytes_   = 0;
    dropped_ = 0;
    return ++owner_;
}

auto Feed::setOpen(const uint64_t owner, const bool on) -> void
{
    const std::scoped_lock lock(mutex_);
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

auto Feed::take(const uint64_t owner) -> std::vector<std::vector<uint8_t>>
{
    const std::scoped_lock lock(mutex_);
    if (owner != owner_)
    {
        return {};
    }
    bytes_ = 0;
    return std::exchange(messages_, {});
}

auto Feed::takeDropped(const uint64_t owner) -> uint64_t
{
    const std::scoped_lock lock(mutex_);
    if (owner != owner_)
    {
        return 0;
    }
    return std::exchange(dropped_, 0);
}

auto sendAll(const std::vector<std::vector<uint8_t>>& messages, const std::stop_token& stop, const std::function<bool(std::span<const uint8_t>)>& send) -> bool
{
    return std::ranges::all_of(messages, [&](const std::vector<uint8_t>& message)
                               {
                                   return !stop.stop_requested() && send(message);
                               });
}

} // namespace memoria::live
