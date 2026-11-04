#include "plugin/text.h"

#include <Windows.h>

namespace memoria
{

auto count(uint64_t n) -> std::string
{
    auto text = std::to_string(n);
    for (auto at = static_cast<std::ptrdiff_t>(text.size()) - 3; at > 0; at -= 3)
    {
        text.insert(static_cast<size_t>(at), ",");
    }
    return text;
}

auto duration(uint64_t seconds) -> std::string
{
    if (seconds < 60)
    {
        return std::format("{} s", seconds);
    }
    if (seconds < 3600)
    {
        return std::format("{} min", seconds / 60);
    }
    return std::format("{} h {} min", seconds / 3600, seconds / 60 % 60);
}

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
