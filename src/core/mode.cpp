#include "core/mode.h"

#include <nlohmann/json.hpp>
#include <xicap/writer.h>

#include <format>
#include <fstream>
#include <string>

namespace memoria
{

auto modeName(Mode mode) -> std::string_view
{
    switch (mode)
    {
        case Mode::Everything:
            return "everything";
        case Mode::Anonymous:
            return "anonymous";
        default:
            return "default";
    }
}

auto parseMode(std::string_view text) -> std::optional<Mode>
{
    for (const auto mode : { Mode::Default, Mode::Everything, Mode::Anonymous })
    {
        if (text == modeName(mode))
        {
            return mode;
        }
    }
    return std::nullopt;
}

auto loadMode(const std::filesystem::path& path) -> Mode
{
    std::string text;
    std::getline(std::ifstream(path), text);
    return parseMode(text) == Mode::Everything ? Mode::Everything : Mode::Default;
}

auto saveMode(const std::filesystem::path& path, Mode mode) -> bool
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << modeName(mode) << '\n';
    out.close();
    return static_cast<bool>(out);
}

auto policyFor(Mode mode) -> xicap::ExclusionPolicy
{
    if (mode == Mode::Everything)
    {
        return { .name = "everything", .version = 1, .rules = {} };
    }
    return xicap::defaultExclusionPolicy(); // anonymous mode's rewriting hooks in here once it is built
}

auto fetchMode(Http& http, const std::string& server, const std::string& installToken) -> std::expected<AccountMode, std::string>
{
    const auto res = http.request("GET", server + "/v1/install/mode", { { "Authorization", "Bearer " + installToken } }, {});
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto body = nlohmann::json::parse(res->body, nullptr, false);
    const auto mode = body.is_object() ? parseMode(body.value("mode", "")) : std::nullopt;
    if (res->status != 200 || !mode)
    {
        return std::unexpected(std::format("Mogsafe answered {}", res->status));
    }
    return AccountMode{ .mode = *mode, .pseudonymKey = body.value("pseudonym_key", "") };
}

} // namespace memoria
