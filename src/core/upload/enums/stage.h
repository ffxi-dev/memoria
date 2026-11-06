#pragma once

#include <cstdint>

namespace memoria::upload
{

// uploads.txt's state column.
enum class Stage : uint8_t
{
    Open,    // still sending
    Closed,  // whole on Mogsafe
    Ended,   // 410, refused: not sent again
    CutOff,  // 410, kept the bytes sent; the tail waits for its own capture
    Rescued, // cut off; the tail went up as -rest.xicap
};

} // namespace memoria::upload
