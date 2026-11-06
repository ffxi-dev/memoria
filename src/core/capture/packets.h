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

// Empty if the packet is too short.
auto character(const uint8_t* login, uint32_t size) -> xicap::Character;

// modified, when given, is the packet as the game gets it.
auto provenance(bool injected, bool blocked, const uint8_t* data, const uint8_t* modified, uint32_t size) -> uint16_t;

// Zone changes, the Mog House and a cancelled logout keep the character in the game.
auto leavesGame(uint8_t logoutState) -> bool;

} // namespace memoria::capture::packets
