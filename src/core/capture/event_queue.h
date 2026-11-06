#pragma once

#include "core/capture/enums/event_kind.h"

#include <xicap/types.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace memoria::capture
{

// Read by the plugin at the zone-in after character select.
struct Login
{
    xicap::Character  character;
    uint16_t          worldId{}; // 0 when unknown; for matching logins
    xicap::ServerKind serverKind{ xicap::ServerKind::Unknown };
    bool              midSession{ false }; // the plugin loaded after the login
    std::string       clientVersion;       // as the client told the lobby
};

struct Event
{
    EventKind              kind{ EventKind::Packet };
    uint64_t               timestamp{};
    xicap::PacketDirection direction{ xicap::PacketDirection::ServerToClient };
    xicap::Channel         channel{ xicap::Channel::Game };
    uint16_t               opcode{}; // off the game channel; game packets carry their own
    uint16_t               provenance{};
    std::vector<uint8_t>   data;
    std::string            text; // a note, or a session's path
    std::optional<Login>   login;
};

// From the game thread to the writer thread. Past maxBytes, counting the batch the writer holds, packets are dropped;
// other events are kept, so logins still rotate captures.
class EventQueue
{
public:
    explicit EventQueue(size_t maxBytes);

    auto push(Event event) -> bool; // false when the packet was dropped
    auto drain(std::vector<Event>& out) -> void;
    auto takeDropped() -> uint64_t;

private:
    std::mutex         mutex_;
    std::vector<Event> events_;
    size_t             bytes_{ 0 }; // queued plus taken_
    size_t             taken_{ 0 }; // in the batch the last drain handed out
    size_t             maxBytes_;
    uint64_t           dropped_{ 0 };
};

} // namespace memoria::capture
