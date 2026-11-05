#pragma once

#include <xicap/types.h>

#include <cstddef>
#include <cstdint>

namespace memoria::capture::packets
{

// GP_SERV_COMMAND_LOGIN (0x00A) and GP_SERV_COMMAND_LOGOUT (0x00B), after atom0s's XiPackets. Offsets count the
// packet's 4-byte header.
constexpr uint16_t LOGIN        = 0x00A;
constexpr uint16_t LOGOUT       = 0x00B;
constexpr size_t   LOGOUT_STATE = 4;

// The character id and name a login packet names; empty if the packet is too short.
auto character(const uint8_t* login, uint32_t size) -> xicap::Character;

// The provenance flags of a packet the host saw: injected by an addon, blocked from the game, or changed on its way
// (modified, when given, is the packet as the game gets it).
auto provenance(bool injected, bool blocked, const uint8_t* data, const uint8_t* modified, uint32_t size) -> uint16_t;

// Whether a logout's LogoutState takes the character out of the game. Zone changes, the Mog House, and a cancelled
// logout do not.
auto leavesGame(uint8_t logoutState) -> bool;

} // namespace memoria::capture::packets
