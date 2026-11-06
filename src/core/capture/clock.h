#pragma once

#include <cstdint>
#include <functional>

namespace memoria::capture
{

// Reads the wall clock once and adds monotonic time to it, so changing the PC's clock does not move timestamps.
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
