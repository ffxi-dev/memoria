#include "core/units.h"

#include <array>
#include <format>

namespace memoria
{

auto sizeText(const uint64_t bytes) -> std::string
{
    constexpr std::array units{ "KB", "MB", "GB" };
    if (bytes < 1024)
    {
        return std::format("{} B", bytes);
    }
    auto   value = static_cast<double>(bytes) / 1024.0;
    size_t unit{ 0 };
    for (; value >= 1024 && unit + 1 < units.size(); ++unit)
    {
        value /= 1024;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

} // namespace memoria
