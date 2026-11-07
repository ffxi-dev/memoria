#pragma once

#include <cstdint>

namespace memoria::cap
{

// What one call to Mogsafe came to.
enum class Outcome : uint8_t
{
    Done,
    Retry,
    Drop,
};

} // namespace memoria::cap
