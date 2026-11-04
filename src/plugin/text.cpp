#include "plugin/text.h"

#include <Windows.h>

namespace memoria
{

auto utf8FromShiftJis(std::string_view text) -> std::string
{
    constexpr UINT SHIFT_JIS = 932;
    const auto     size      = static_cast<int>(text.size());
    std::wstring   wide(::MultiByteToWideChar(SHIFT_JIS, 0, text.data(), size, nullptr, 0), L'\0');
    ::MultiByteToWideChar(SHIFT_JIS, 0, text.data(), size, wide.data(), static_cast<int>(wide.size()));
    std::string utf8(::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return utf8;
}

} // namespace memoria
