#pragma once

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace memoria::captain
{

// Compresses log with zstd level 3 into out, after head's bytes if head is given. Writes out's .tmp and renames it when
// whole, so a crash leaves a whole file or none. Why not, if it failed or stop was requested.
[[nodiscard]] auto compressLog(const std::filesystem::path& log, const std::filesystem::path& out, const std::filesystem::path& head, const std::stop_token& stop) -> std::optional<std::string>;

} // namespace memoria::captain
