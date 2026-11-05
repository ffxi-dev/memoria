#pragma once

#include <cstdint>

namespace memoria::upload
{

// Where one capture's upload stands, as uploads.txt's state column keeps it.
enum class Stage : uint8_t
{
    Open,    // still sending
    Closed,  // whole in Vault
    Ended,   // Mogsafe answered 410 and refused it: never sent again
    CutOff,  // Mogsafe answered 410 and kept the bytes sent so far; the tail waits for its own capture
    Rescued, // cut off, and the tail went up as a -rest.xicap capture of its own
};

} // namespace memoria::upload
