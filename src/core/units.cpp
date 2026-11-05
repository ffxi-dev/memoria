#include "core/units.h"

#include <array>
#include <charconv>
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

auto parseU64(const std::string_view s) -> std::optional<uint64_t>
{
    uint64_t v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && end == s.data() + s.size() ? std::optional(v) : std::nullopt;
}

} // namespace memoria
