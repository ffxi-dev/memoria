#include "plugin/packets.h"

#include <cstring>

namespace memoria::plugin::packets
{
namespace
{

constexpr size_t LOGIN_UNIQUE_NO = 4;
constexpr size_t LOGIN_NAME      = 132;
constexpr size_t LOGIN_NAME_SIZE = 16;

} // namespace

auto character(const uint8_t* login, const uint32_t size) -> xicap::Character
{
    xicap::Character c;
    if (size >= LOGIN_NAME + LOGIN_NAME_SIZE)
    {
        std::memcpy(&c.characterId, login + LOGIN_UNIQUE_NO, sizeof(c.characterId));
        const auto* name = reinterpret_cast<const char*>(login + LOGIN_NAME);
        c.name.assign(name, strnlen(name, LOGIN_NAME_SIZE));
    }
    return c;
}

auto leavesGame(const uint8_t logoutState) -> bool
{
    constexpr uint8_t ZONECHANGE = 2;
    constexpr uint8_t MYROOM     = 3;
    constexpr uint8_t CANCEL     = 4;
    return logoutState != 0 && logoutState != ZONECHANGE && logoutState != MYROOM && logoutState != CANCEL;
}

} // namespace memoria::plugin::packets
