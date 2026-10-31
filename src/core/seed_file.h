#pragma once

#include <xicap/types.h>

#include <expected>
#include <filesystem>
#include <string>

namespace memoria
{

// The install's signing seed, created on first start. A file that exists but cannot be read is an error, never replaced: an install keeps one key.
auto loadOrCreateSeed(const std::filesystem::path& path) -> std::expected<xicap::SecretSeed, std::string>;

} // namespace memoria
