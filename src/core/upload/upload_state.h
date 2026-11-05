#pragma once

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
    bool                                  closed{};
    bool                                  ended{};   // Mogsafe answered 410: cut off or refused
    bool                                  cutOff{};  // ended as cut off: Vault keeps the bytes sent before
    bool                                  rescued{}; // its tail went up as a -rest.xicap capture of its own
    uint64_t                              refused{}; // size at the last refused close
    uint64_t                              sent{};
    std::string                           reason;       // why Mogsafe refused it for good
    bool                                  restFailed{}; // writing the tail failed: tried again after a restart
    uint64_t                              size{};
    std::chrono::steady_clock::time_point seen{}; // when size last changed
    std::string                           error;
};

using UploadFiles = std::map<std::string, UploadFile>; // by file name

auto readUploads(const std::filesystem::path& path) -> UploadFiles;

// Why the file was not replaced, if it was not.
auto writeUploads(const std::filesystem::path& path, const UploadFiles& files) -> std::optional<std::string>;

} // namespace memoria::upload
