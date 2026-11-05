#pragma once

#include <cstdint>

namespace memoria::plugin
{

enum class LiveState : uint8_t
{
    Starting,
    Connected,
    Reconnecting,
};

} // namespace memoria::plugin
