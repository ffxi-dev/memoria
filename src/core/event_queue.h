#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace memoria
{

// What the game thread hands the writer thread, in the order it happened.
struct Event
{
    enum class Kind : uint8_t
    {
        Packet,
        Login,
        Logout,
        Note,
        SessionStart, // data: the session file's path, UTF-8
        SessionStop,
    };

    Kind                 kind = Kind::Packet;
    uint64_t             timestamp{};
    bool                 clientToServer{};
    uint8_t              channel{};
    uint16_t             opcode{};     // off the game channel; game packets carry their own
    uint16_t             provenance{}; // xicap::PacketFlag::Injected, Blocked, Modified, FirstInChunk
    std::vector<uint8_t> data;         // packet bytes, or a note's text
};

// The hand-off between the game thread and the writer thread. Pushing copies
// under a short lock and never waits on the writer. Past maxBytes, packets are
// dropped and counted; markers are always kept, so logins still rotate.
class EventQueue
{
public:
    explicit EventQueue(size_t maxBytes);

    // False when the packet was dropped.
    auto push(Event event) -> bool;

    // Moves everything queued into out, oldest first.
    auto drain(std::vector<Event>& out) -> void;

    // Packets dropped since the last call.
    auto takeDropped() -> uint64_t;

private:
    std::mutex         mutex_;
    std::vector<Event> events_;
    size_t             bytes_{ 0 };
    size_t             maxBytes_;
    uint64_t           dropped_{ 0 };
};

} // namespace memoria
