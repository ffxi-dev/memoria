#include "core/seed_file.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <random>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace memoria
{

// Not std::ios::noreplace: Wine, which runs the tests, has no fopen "x" mode.
auto createNew(const std::filesystem::path& path) -> bool
{
#ifdef _WIN32
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
    {
        return false;
    }
    _close(fd);
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
    {
        return false;
    }
    ::close(fd);
#endif
    return true;
}

auto loadOrCreateSeed(const std::filesystem::path& path) -> std::expected<xicap::SecretSeed, std::string>
{
    xicap::SecretSeed seed{};
    std::error_code   ec;
    if (std::filesystem::exists(path, ec))
    {
        // Wine refuses to open a new key for the moment its creator removes the temporary link to it.
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            std::ifstream in(path, std::ios::binary);
            if (std::filesystem::file_size(path, ec) == seed.size() && in.read(reinterpret_cast<char*>(seed.data()), seed.size()))
            {
                return seed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::unexpected(std::format("cannot read the signing key {}; recording is off until it is fixed", path.string()));
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
    // Each client starting together writes its own file, so the one whose link wins is the seed it returns.
    const auto temp = std::filesystem::path(path).concat(std::format(".{:08x}{:08x}.new", random(), random()));
    {
        if (!createNew(temp))
        {
            return std::unexpected(std::format("cannot create {}", temp.string()));
        }
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(seed.data()), seed.size());
        out.close();
        if (!out)
        {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return std::unexpected(std::format("cannot write the signing key {}", temp.string()));
        }
    }
    // A hard link fails when the key exists: of two clients starting together, one creates it and the other reads it.
    std::filesystem::create_hard_link(temp, path, ec);
    std::error_code ignored;
    // Windows refuses to delete a name of a file another client has open to read the key: try until it lets go.
    for (int attempt = 0; attempt < 100 && !std::filesystem::remove(temp, ignored) && std::filesystem::exists(temp, ignored); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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
