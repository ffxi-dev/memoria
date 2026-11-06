#pragma once

#include "core/enums/save_step.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace memoria
{

// Fails if path exists. Only the user running the game can open a secret file.
auto createNew(const std::filesystem::path& path, bool secret = false, std::string_view text = {}) -> bool;

// Fails if to exists, where std::filesystem::rename replaces it.
auto moveNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) -> bool;

// Writes path.tmp, flushes it and renames it over path, so a power cut leaves the old file or the new one. After a
// failed POSIX folder flush, path is new but may not be on disk; any earlier failure leaves it unchanged.
[[nodiscard]] auto replaceFile(const std::filesystem::path& path, std::string_view text) -> std::optional<std::string>;

// Hashes the first size bytes: the file may still be growing.
auto sha256File(const std::filesystem::path& path, uint64_t size, const std::stop_token& stop) -> std::optional<std::string>;

// Tests set it to fail a step of replaceFile.
inline std::function<bool(SaveStep)> failSave;

} // namespace memoria
