#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

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

inline auto paint(Color c, std::string_view text) -> std::string
{
    return std::format("\x1E{}{}\x1E\x01", static_cast<char>(c), text);
}

// 1,234,567
auto count(uint64_t n) -> std::string;

// 45 s, 12 min, 3 h 5 min
auto duration(uint64_t seconds) -> std::string;

// The game and Ashita's commands speak Shift-JIS; captures hold UTF-8.
auto utf8FromShiftJis(std::string_view text) -> std::string;

} // namespace memoria
