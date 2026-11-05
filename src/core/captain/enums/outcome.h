#pragma once

#include <cstdint>

namespace memoria::captain
{

// How one captain capture's import ended.
enum class Outcome : uint8_t
{
    Uploaded,
    InVault,
    Refused,
    Skipped,
    NotMoved,
    Stopped,
    Failed,
};

} // namespace memoria::captain
