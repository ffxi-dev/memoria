#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace memoria
{

// SHA-256 (FIPS 180-4), for the hash Mogsafe checks when an upload closes.
class Sha256
{
public:
    Sha256();
    auto update(std::span<const uint8_t> data) -> void;
    auto hex() -> std::string; // finishes the hash

private:
    auto block(const uint8_t* p) -> void;

    std::array<uint32_t, 8> state_;
    std::array<uint8_t, 64> buffer_{};
    size_t                  used_{ 0 };
    uint64_t                bytes_{ 0 };
};

// The SHA-256 of a file's first size bytes, in lower-case hex.
auto fileSha256(const std::filesystem::path& path, uint64_t size) -> std::optional<std::string>;

} // namespace memoria
