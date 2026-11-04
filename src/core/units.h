#pragma once

#include <cstdint>
#include <string>

namespace memoria
{

// 512 B, 1.5 KB, 2.0 MB, 3.1 GB.
auto sizeText(uint64_t bytes) -> std::string;

} // namespace memoria
