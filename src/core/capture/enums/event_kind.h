#pragma once

#include <cstdint>

namespace memoria::capture
{

enum class EventKind : uint8_t
{
    Packet,
    Login,
    Logout,
    Note,
    SessionStart,
    SessionStop,
};

} // namespace memoria::capture
