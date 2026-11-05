#include "core/captain/captain.h"

#include "core/install.h"
#include "core/tus.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>

namespace memoria::captain
{
namespace
{

constexpr auto CRC_TABLE = []
{
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n)
    {
        uint32_t c{ n };
        for (int k = 0; k < 8; ++k)
        {
            c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}();

} // namespace

auto findCaptain(const std::filesystem::path& root, const std::chrono::seconds quiet, const std::stop_token& stop) -> std::vector<CaptainCapture>
{
    namespace fs = std::filesystem;
    std::vector<CaptainCapture> out;
    std::error_code             ec;
    const auto                  now    = fs::file_time_type::clock::now();
    const auto                  sizeOf = [](const fs::path& path) -> std::optional<uint64_t>
    {
        std::error_code failed;
        const auto      size = fs::file_size(path, failed);
        return failed ? std::nullopt : std::optional<uint64_t>(size);
    };
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && !stop.stop_requested() && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code probe; // the walk's own error_code says only whether the walk can go on
        const auto      log = it->path() / "PacketLogger" / "full.log";
        if (!it->is_directory(probe) || !fs::is_regular_file(it->path() / "manifest.txt", probe) || !fs::is_regular_file(log, probe))
        {
            continue;
        }
        it.disable_recursion_pending();
        const auto changed    = fs::last_write_time(log, probe);
        const bool quietLong  = !probe && now - changed >= quiet;
        const auto search     = it->path() / "SearchLog" / "search.log";
        const auto size       = sizeOf(log);
        const auto searchSize = fs::is_regular_file(search, probe) ? sizeOf(search) : std::optional<uint64_t>(0);
        out.push_back({
            .dir        = it->path(),
            .folder     = it->path().lexically_relative(root).generic_string(),
            .character  = it->path().filename().string(),
            .size       = size.value_or(0),
            .searchSize = searchSize.value_or(0),
            .growing    = !quietLong || !size || !searchSize,
        });
    }
    std::ranges::sort(out, {}, &CaptainCapture::folder);
    return out;
}

auto crc32File(const std::filesystem::path& path, const std::stop_token& stop) -> std::optional<Crc32>
{
    std::ifstream     in(path, std::ios::binary);
    std::vector<char> buf(Tus::CHUNK_SIZE);
    Crc32             result;
    uint32_t          crc{ 0xFFFFFFFF };
    while (in)
    {
        if (stop.stop_requested())
        {
            return std::nullopt;
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<size_t>(in.gcount());
        for (size_t i = 0; i < n; ++i)
        {
            crc = CRC_TABLE[(crc ^ static_cast<uint8_t>(buf[i])) & 0xFF] ^ (crc >> 8);
        }
        result.size += n;
    }
    if (!in.eof())
    {
        return std::nullopt;
    }
    result.crc = crc ^ 0xFFFFFFFF;
    return result;
}

auto moveAside(const std::filesystem::path& root, const std::filesystem::path& uploaded, const std::filesystem::path& dir) -> std::expected<std::filesystem::path, std::string>
{
    namespace fs        = std::filesystem;
    const auto      rel = dir.lexically_relative(root);
    std::error_code ec;
    auto            target = uploaded / rel;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
    {
        return std::unexpected(ec.message());
    }
    for (int n = 2; fs::exists(target, ec); ++n)
    {
        target = uploaded / rel;
        target += std::format("-{}", n);
    }
    fs::rename(dir, target, ec);
    if (ec)
    {
        return std::unexpected(ec.message());
    }
    // remove() refuses a folder that is not empty, which ends the climb.
    for (auto p = dir.parent_path(); p.native().size() > root.native().size() && fs::remove(p, ec); p = p.parent_path())
    {
    }
    return target;
}

auto remindImport(const std::filesystem::path& config, const std::filesystem::path& root, const std::stop_token& stop) -> bool
{
    if (!loadLink(config) || importReminded(config))
    {
        return false;
    }
    const auto found = findCaptain(root, std::chrono::minutes(10), stop);
    return !stop.stop_requested() && std::ranges::any_of(found, [](const auto& c)
                                                         {
                                                             return !c.growing;
                                                         }) &&
           claimImportReminder(config);
}

} // namespace memoria::captain
