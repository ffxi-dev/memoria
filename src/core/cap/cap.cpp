#include "core/cap/cap.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <random>

namespace memoria::cap
{
namespace
{

constexpr std::string_view ID_DIGITS{ "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" };
constexpr size_t           ID_SIZE{ 12 };

auto minute(const uint64_t ms) -> std::chrono::sys_time<std::chrono::minutes>
{
    return std::chrono::floor<std::chrono::minutes>(std::chrono::sys_time<std::chrono::milliseconds>{ std::chrono::milliseconds{ ms } });
}

auto safe(std::string text) -> std::string
{
    std::ranges::replace_if(text, [](const unsigned char c)
                            {
                                return !std::isalnum(c);
                            },
                            '_');
    return text.empty() ? "unknown" : text;
}

} // namespace

auto parseStart(const std::string_view text) -> StartArgs
{
    StartArgs args;
    for (size_t at{ text.find_first_not_of(' ') }; at != std::string_view::npos; at = text.find_first_not_of(' ', at))
    {
        const auto end  = (std::min)(text.find(' ', at), text.size());
        const auto word = text.substr(at, end - at);
        if (word.find_first_of("/\\") != std::string_view::npos)
        {
            const auto rest = text.substr(at);
            const auto from = rest.find_first_not_of('"');
            args.path       = std::string{ rest.substr(from, rest.find_last_not_of(" \"") - from + 1) };
            break;
        }
        args.name += (args.name.empty() ? "" : " ") + std::string{ word };
        at = end;
    }
    std::erase(args.name, '"');
    return args;
}

auto newId() -> std::string
{
    std::random_device                    random;
    std::uniform_int_distribution<size_t> pick{ 0, ID_DIGITS.size() - 1 };
    std::string                           id(ID_SIZE, ' ');
    std::ranges::generate(id, [&]
                          {
                              return ID_DIGITS[pick(random)];
                          });
    return id;
}

auto defaultName(const uint64_t nowMs) -> std::string
{
    return std::format("cap_{:%Y%m%d-%H%M}", minute(nowMs));
}

auto startMarker(const Cap& cap) -> std::string
{
    return std::format("cap start {} id={}", cap.name, cap.id);
}

auto stopMarker(const Cap& cap) -> std::string
{
    return std::format("cap stop id={}", cap.id);
}

auto fileIn(const std::filesystem::path& given) -> std::filesystem::path
{
    return given.extension() == ".xicap" ? given : given / "memoria.xicap";
}

auto freeFile(const std::filesystem::path& dir, const std::string_view name, const std::string_view character, const uint64_t nowMs) -> std::filesystem::path
{
    const auto      stem = std::format("{}_{}_{:%Y%m%d-%H%M}", safe(std::string{ name }), safe(std::string{ character }), minute(nowMs));
    auto            path = dir / (stem + ".xicap");
    std::error_code ec;
    for (int n{ 2 }; std::filesystem::exists(path, ec); ++n)
    {
        path = dir / std::format("{}-{}.xicap", stem, n);
    }
    return path;
}

auto lengthText(const uint64_t ms) -> std::string
{
    const auto s = ms / 1000;
    if (s >= 3600)
    {
        return std::format("{}h {}m", s / 3600, s / 60 % 60);
    }
    if (s >= 60)
    {
        return std::format("{}m {}s", s / 60, s % 60);
    }
    return std::format("{}s", s);
}

auto page(const std::string_view server, const Cap& cap) -> std::string
{
    return std::format("{}/captures/{}", server, cap.id);
}

} // namespace memoria::cap
