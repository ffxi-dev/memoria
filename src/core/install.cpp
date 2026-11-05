#include "core/install.h"

#include "core/file.h"

#include <chrono>
#include <format>
#include <fstream>
#include <random>
#include <string_view>
#include <thread>
#include <tuple>

namespace memoria
{
namespace
{

constexpr auto TOKEN           = "token";
constexpr auto PAUSED          = "paused";
constexpr auto IMPORT_REMINDED = "import-reminded";
constexpr auto REMINDED        = "reminded"; // the date of the last daily reminder

// Two lines: the token, then the account it belongs to.
auto readLink(const std::filesystem::path& path) -> std::optional<Linked>
{
    std::ifstream in(path);
    Linked        link;
    if (!std::getline(in, link.token) || link.token.empty())
    {
        return std::nullopt;
    }
    std::getline(in, link.account);
    return link;
}

// A name beside path that no other client picks at the same time.
auto beside(const std::filesystem::path& path, const std::string_view suffix) -> std::filesystem::path
{
    std::random_device random;
    return std::filesystem::path(path).concat(std::format(".{:08x}{:08x}.{}", random(), random(), suffix));
}

} // namespace

auto loadLink(const std::filesystem::path& config) -> std::optional<Linked>
{
    return readLink(config / TOKEN);
}

auto saveLink(const std::filesystem::path& config, const Linked& link) -> std::optional<std::string>
{
    std::error_code ec;
    std::filesystem::create_directories(config, ec);
    const auto path = config / TOKEN;
    const auto temp = beside(path, "new");
    if (!createNew(temp, true, std::format("{}\n{}\n", link.token, link.account)))
    {
        std::filesystem::remove(temp, ec);
        return std::format("cannot write {}", temp.string());
    }
    // Another client sharing the install may be reading the old token: Windows refuses the rename until it closes it.
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        std::filesystem::rename(temp, path, ec);
        if (!ec)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::filesystem::remove(temp, ec);
    return std::format("cannot replace {}", path.string());
}

auto removeLink(const std::filesystem::path& config, const std::string& token, const LinkBack& linkBack) -> bool
{
    const auto      path  = config / TOKEN;
    const auto      taken = beside(path, "old");
    std::error_code ec;
    std::filesystem::rename(path, taken, ec);
    if (ec)
    {
        return false;
    }
    const auto link = readLink(taken);
    const bool matched{ link && link->token == token };
    if (!matched)
    {
        // A hard link fails where a token was saved since the rename, so the newer one stays. Where links fail for another
        // reason, a move that never replaces a file puts the taken token back; if that fails too, it stays in its file.
        const bool linked{ linkBack ? linkBack(taken, path) : (std::filesystem::create_hard_link(taken, path, ec), !ec) };
        if (!linked)
        {
            std::ignore = moveNoReplace(taken, path);
            return false;
        }
    }
    std::filesystem::remove(taken, ec);
    return matched;
}

auto unlink(const std::filesystem::path& config) -> std::optional<std::string>
{
    std::error_code ec;
    if (!std::filesystem::remove(config / TOKEN, ec) && ec)
    {
        return std::format("cannot delete {}: {}", (config / TOKEN).string(), ec.message());
    }
    return std::nullopt;
}

auto paused(const std::filesystem::path& config) -> bool
{
    std::error_code ec;
    return std::filesystem::exists(config / PAUSED, ec) || ec;
}

auto setPaused(const std::filesystem::path& config, const bool on) -> std::optional<std::string>
{
    const auto      path = config / PAUSED;
    std::error_code ec;
    if (on && !std::ofstream{ path })
    {
        return std::format("cannot create {}", path.string());
    }
    if (!on && !std::filesystem::remove(path, ec) && ec)
    {
        return std::format("cannot delete {}: {}", path.string(), ec.message());
    }
    return std::nullopt;
}

auto sharing(const std::filesystem::path& config, const std::string& token) -> bool
{
    const auto link = loadLink(config);
    return !paused(config) && link && link->token == token;
}

auto markImportReminded(const std::filesystem::path& config) -> bool
{
    return createNew(config / IMPORT_REMINDED);
}

auto claimDailyReminder(const std::filesystem::path& config) -> bool
{
    const auto  today = std::format("{:%F}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    std::string last;
    std::getline(std::ifstream(config / REMINDED), last);
    return last != today && !replaceFile(config / REMINDED, today);
}

auto importReminded(const std::filesystem::path& config) -> bool
{
    std::error_code ec;
    return std::filesystem::exists(config / IMPORT_REMINDED, ec);
}

auto serverIn(const std::filesystem::path& config) -> std::optional<std::string>
{
    const auto trim = [](std::string_view s)
    {
        const auto first = s.find_first_not_of(" \t\r");
        return first == std::string_view::npos ? std::string{} : std::string(s.substr(first, s.find_last_not_of(" \t\r/") - first + 1));
    };
    std::ifstream in(config / "upload.ini");
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == "server" && trim(line.substr(eq + 1)).starts_with("https://"))
        {
            return trim(line.substr(eq + 1));
        }
    }
    return std::nullopt;
}

} // namespace memoria
