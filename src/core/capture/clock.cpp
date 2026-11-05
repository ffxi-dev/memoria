#include "core/capture/clock.h"

#include <chrono>
#include <utility>

namespace memoria::capture
{
namespace
{

template <typename C>
auto millisecondsOf() -> uint64_t
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(C::now().time_since_epoch()).count());
}

} // namespace

Clock::Clock()
: Clock(millisecondsOf<std::chrono::system_clock>, millisecondsOf<std::chrono::steady_clock>)
{
}

Clock::Clock(const Source wallMs, Source steadyMs)
: steadyMs_(std::move(steadyMs))
, startWallMs_(wallMs())
, startSteadyMs_(steadyMs_())
{
}

auto Clock::nowMs() const -> uint64_t
{
    return startWallMs_ + (steadyMs_() - startSteadyMs_);
}

} // namespace memoria::capture
