#pragma once

#include <cstdint>
#include <functional>

namespace memoria::capture
{

// The wall clock read once, then a monotonic clock: a change to the PC's clock never moves packet timestamps.
class Clock
{
public:
    using Source = std::function<uint64_t()>;

    Clock();
    Clock(const Source& wallMs, Source steadyMs);

    [[nodiscard]] auto nowMs() const -> uint64_t; // since the Unix epoch, UTC

private:
    Source   steadyMs_;
    uint64_t startWallMs_;
    uint64_t startSteadyMs_;
};

} // namespace memoria::capture
