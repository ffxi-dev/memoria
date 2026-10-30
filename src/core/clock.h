#pragma once

#include <cstdint>
#include <functional>

namespace memoria
{

// Packet timestamps: the wall clock read once, then a monotonic clock, so a
// change to the PC's clock never moves them backwards or makes them jump.
class Clock
{
public:
    using Source = std::function<uint64_t()>;

    Clock();
    Clock(Source wallMs, Source steadyMs);

    // Milliseconds since the Unix epoch, in UTC.
    [[nodiscard]] auto nowMs() const -> uint64_t;

private:
    Source   steadyMs_;
    uint64_t startWallMs_;
    uint64_t startSteadyMs_;
};

} // namespace memoria
