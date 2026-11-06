#pragma once

#include "core/upload/enums/stage.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace memoria::upload
{

// A line of uploads.txt. The fields after reason are not saved.
struct UploadFile
{
    std::string                           url;
    Stage                                 stage{ Stage::Open };
    uint64_t                              refusedAt{}; // the size at the last refused close
    uint64_t                              sent{};
    std::string                           reason;       // why Mogsafe refused it for good
    bool                                  restFailed{}; // writing the tail failed: tried again after a restart
    uint64_t                              size{};
    std::chrono::steady_clock::time_point seen{}; // when size last changed
    std::string                           error;
    std::string                           deniedFor; // the token Mogsafe answered 403 for: another account's upload
};

using UploadFiles = std::map<std::string, UploadFile>; // by file name

// Returns the file's size and notes in f when it last changed.
auto watch(const std::filesystem::path& path, UploadFile& f) -> std::expected<uint64_t, std::string>;

auto readUploads(const std::filesystem::path& path) -> UploadFiles;

[[nodiscard]] auto writeUploads(const std::filesystem::path& path, const UploadFiles& files) -> std::optional<std::string>;

} // namespace memoria::upload
