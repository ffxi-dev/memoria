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

// Creates a file holding text, flushed to disk, failing when anything is already there. A secret one only the user
// running the game can open.
auto createNew(const std::filesystem::path& path, bool secret = false, std::string_view text = {}) -> bool;

// Renames from to to, failing when to exists: unlike std::filesystem::rename, it never replaces a file.
auto moveNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) -> bool;

// Writes text to path's .tmp, flushes it to disk, and renames it over path, so neither a crash nor a power cut leaves a
// half-written path. Why not, if a step failed: a failure before the rename leaves path as it was; one after it (the
// folder's own flush, on POSIX) leaves path new but maybe not yet on disk.
[[nodiscard]] auto replaceFile(const std::filesystem::path& path, std::string_view text) -> std::optional<std::string>;

// The SHA-256 of a file's first size bytes, in hex: the file may still be growing. Nothing if it cannot be read or stop
// is requested.
auto sha256File(const std::filesystem::path& path, uint64_t size, const std::stop_token& stop) -> std::optional<std::string>;

// Tests set it to make a step of replaceFile fail; empty in the game.
inline std::function<bool(SaveStep)> failSave;

} // namespace memoria
