#pragma once

#include <xicap/types.h>

#include <expected>
#include <filesystem>
#include <string>

namespace memoria
{

// The install's Ed25519 signing seed, created on first start and never
// replaced: a file that exists but cannot be read is an error, so an install
// never splits its captures across two keys.
auto loadOrCreateSeed(const std::filesystem::path& path) -> std::expected<xicap::SecretSeed, std::string>;

} // namespace memoria
