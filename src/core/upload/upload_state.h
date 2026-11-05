#pragma once

#include "core/upload/enums/stage.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace memoria::upload
{

// One capture's upload, as uploads.txt keeps it. The fields after reason live only in memory.
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

// Notes when the file's size last changed, which tells when it went quiet.
auto watch(UploadFile& f, uint64_t size, std::chrono::steady_clock::time_point now) -> void;

auto readUploads(const std::filesystem::path& path) -> UploadFiles;

// Why the file was not replaced, if it was not.
[[nodiscard]] auto writeUploads(const std::filesystem::path& path, const UploadFiles& files) -> std::optional<std::string>;

} // namespace memoria::upload
