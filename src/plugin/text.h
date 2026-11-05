#pragma once

#include "plugin/enums/color.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace memoria::plugin
{

inline auto paint(const Color c, const std::string_view text) -> std::string
{
    return std::format("\x1E{}{}\x1E\x01", static_cast<char>(c), text);
}

// 1,234,567
auto countText(uint64_t n) -> std::string;

// 45 s, 12 min, 3 h 5 min
auto durationText(uint64_t seconds) -> std::string;

// The rest of a typed command after word's first use, without the spaces and quotes around it: a path may hold spaces.
auto restAfter(std::string_view line, std::string_view word) -> std::string_view;

// The game and Ashita's commands speak Shift-JIS; captures hold UTF-8.
auto utf8FromShiftJis(std::string_view text) -> std::string;

} // namespace memoria::plugin
