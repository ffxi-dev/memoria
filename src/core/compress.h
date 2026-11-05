#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

namespace memoria
{

// Compresses log with zstd level 3 into out, after head's bytes if head is given. Writes out's .tmp and renames it when
// whole, so a crash leaves a whole file or none. Why not, if it failed or stop was set.
auto compressLog(const std::filesystem::path& log, const std::filesystem::path& out, const std::filesystem::path& head, const std::atomic<bool>& stop) -> std::optional<std::string>;

} // namespace memoria
