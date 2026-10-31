#include "core/seed_file.h"

#include <fstream>
#include <random>
#include <system_error>

namespace memoria
{

auto loadOrCreateSeed(const std::filesystem::path& path) -> std::expected<xicap::SecretSeed, std::string>
{
    xicap::SecretSeed seed{};
    std::error_code   ec;
    if (std::filesystem::exists(path, ec))
    {
        std::ifstream in(path, std::ios::binary);
        if (!in || std::filesystem::file_size(path, ec) != seed.size() || ec)
        {
            return std::unexpected("cannot read the signing key " + path.string() + "; recording is off until it is fixed");
        }
        in.read(reinterpret_cast<char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        if (!in)
        {
            return std::unexpected("cannot read the signing key " + path.string());
        }
        return seed;
    }
    if (ec)
    {
        return std::unexpected("cannot check for the signing key " + path.string() + ": " + ec.message());
    }

    std::random_device random; // the OS's cryptographic source on Windows and Linux
    for (size_t i = 0; i < seed.size(); i += 4)
    {
        const uint32_t word = random();
        for (size_t b = 0; b < 4; ++b)
        {
            seed[i + b] = static_cast<uint8_t>(word >> (8 * b));
        }
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = std::filesystem::path(path).concat(".new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        if (!out)
        {
            return std::unexpected("cannot write the signing key " + temp.string());
        }
    }
    // A hard link fails when the key exists: of two clients starting together,
    // one creates the key and the other reads it.
    std::filesystem::create_hard_link(temp, path, ec);
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    if (ec && !std::filesystem::exists(path, ignored))
    {
        return std::unexpected("cannot save the signing key " + path.string() + ": " + ec.message());
    }
    return ec ? loadOrCreateSeed(path) : std::expected<xicap::SecretSeed, std::string>(seed);
}

} // namespace memoria
