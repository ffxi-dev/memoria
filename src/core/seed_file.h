#pragma once

#include <xicap/types.h>

#include <expected>
#include <filesystem>
#include <string>

namespace memoria
{

// The install's signing seed, created on first start. A file that exists but cannot be read is an error, never replaced: an install keeps one key.
auto loadOrCreateSeed(const std::filesystem::path& path) -> std::expected<xicap::SecretSeed, std::string>;

// Creates an empty file, failing when anything is already there. A secret one only the user running the game can open.
auto createNew(const std::filesystem::path& path, bool secret = false) -> bool;

// Whether only the user running the game can open path: what createNew gives a secret.
auto ownerOnly(const std::filesystem::path& path) -> bool;

} // namespace memoria
