#pragma once

#include <string>
#include <string_view>

namespace memoria
{

// The game and Ashita's commands speak Shift-JIS; captures hold UTF-8.
auto utf8FromShiftJis(std::string_view text) -> std::string;

} // namespace memoria
