#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace memoria
{

// 512 B, 1.5 KB, 2.0 MB, 3.1 GB.
auto sizeText(uint64_t bytes) -> std::string;

// Fails on trailing characters, which from_chars alone accepts.
auto parseU64(std::string_view s) -> std::optional<uint64_t>;

} // namespace memoria
