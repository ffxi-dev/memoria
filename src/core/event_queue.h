#pragma once

#include <xicap/types.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace memoria
{

// Who logged in, as the plugin read it at the zone-in after character select.
struct Login
{
    xicap::Character character;
    uint16_t         worldId{}; // 0 when unknown; the capture keeps only the world's name
    xicap::Network   network    = xicap::Network::Unknown;
    bool             midSession = false; // the plugin loaded after the login
    std::string      clientVersion;      // as the client told the lobby
};

struct Event
{
    enum class Kind : uint8_t
    {
        Packet,
        Login,
        Logout,
        Note,
        SessionStart,
        SessionStop,
    };

    Kind                 kind = Kind::Packet;
    uint64_t             timestamp{};
    bool                 clientToServer{};
    uint8_t              channel{};
    uint16_t             opcode{}; // off the game channel; game packets carry their own
    uint16_t             provenance{};
    std::vector<uint8_t> data;
    std::string          text; // a note, or a session's path
    std::optional<Login> login;
};

// Hands events from the game thread to the writer thread without waiting on it. Past maxBytes, counting the batch the writer still holds, packets are dropped and counted; other events are always kept, so logins still rotate.
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
    size_t             bytes_{ 0 }; // queued, and taken_
    size_t             taken_{ 0 }; // in the batch the last drain handed out
    size_t             maxBytes_;
    uint64_t           dropped_{ 0 };
};

} // namespace memoria
