#pragma once

#include <cstdint>

namespace memoria
{

// Ashita's chat colours, as its chat.lua names them: 0x1E, then a colour number; 0x1E 0x01 goes back to normal.
enum class Color : uint8_t
{
    Green  = 2,
    Cyan   = 6,
    Grey   = 67,
    Red    = 68,
    Yellow = 104,
};

} // namespace memoria
