#include "core/seed_file.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <random>

namespace memoria
{

auto loadOrCreateSeed(const std::filesystem::path& path) -> std::expected<xicap::SecretSeed, std::string>
{
    xicap::SecretSeed seed{};
    std::error_code   ec;
    if (std::filesystem::exists(path, ec))
    {
        std::ifstream in(path, std::ios::binary);
        if (std::filesystem::file_size(path, ec) != seed.size() || !in.read(reinterpret_cast<char*>(seed.data()), seed.size()))
        {
            return std::unexpected(std::format("cannot read the signing key {}; recording is off until it is fixed", path.string()));
        }
        return seed;
    }
    if (ec)
    {
        return std::unexpected(std::format("cannot check for the signing key {}: {}", path.string(), ec.message()));
    }

    std::random_device random;
    std::ranges::generate(seed, [&]
                          {
                              return static_cast<uint8_t>(random());
                          });

    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = std::filesystem::path(path).concat(".new");
    if (!std::ofstream(temp, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(seed.data()), seed.size()))
    {
        return std::unexpected(std::format("cannot write the signing key {}", temp.string()));
    }
    // A hard link fails when the key exists: of two clients starting together, one creates it and the other reads it.
    std::filesystem::create_hard_link(temp, path, ec);
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    if (ec && std::filesystem::exists(path, ignored))
    {
        return loadOrCreateSeed(path);
    }
    if (ec)
    {
        return std::unexpected(std::format("cannot save the signing key {}: {}", path.string(), ec.message()));
    }
    return seed;
}

} // namespace memoria
