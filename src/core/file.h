#pragma once

#include <filesystem>

namespace memoria
{

// Creates an empty file, failing when anything is already there. A secret one only the user running the game can open.
auto createNew(const std::filesystem::path& path, bool secret = false) -> bool;

// Whether only the user running the game can open path: what createNew gives a secret.
auto ownerOnly(const std::filesystem::path& path) -> bool;

} // namespace memoria
