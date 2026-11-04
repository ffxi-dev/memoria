#include "core/link.h"

#include "core/ed25519.h"

#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <array>
#include <format>
#include <fstream>
#include <random>

namespace memoria
{
namespace
{

auto post(Http& http, const std::string& url, const nlohmann::json& body) -> std::expected<std::pair<int, nlohmann::json>, std::string>
{
    const auto text = body.dump();
    const auto res  = http.request("POST", url, { { "Content-Type", "application/json" } }, { reinterpret_cast<const uint8_t*>(text.data()), text.size() });
    if (!res)
    {
        return std::unexpected(res.error());
    }
    return std::pair{ res->status, nlohmann::json::parse(res->body, nullptr, false) };
}

} // namespace

auto startLink(Http& http, const std::string& server, const xicap::SecretSeed& seed, uint64_t unixSeconds) -> std::expected<LinkCode, std::string>
try
{
    std::array<uint8_t, 64> secret{};
    std::array<uint8_t, 32> publicKey{};
    auto                    copy = seed; // Monocypher wipes the seed it is given
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), copy.data());

    const auto              key     = picosha2::bytes_to_hex_string(publicKey);
    const auto              message = std::format("memoria-link\n{}\n{}", key, unixSeconds);
    std::array<uint8_t, 64> signature{};
    crypto_ed25519_sign(signature.data(), secret.data(), reinterpret_cast<const uint8_t*>(message.data()), message.size());
    crypto_wipe(secret.data(), secret.size());

    const auto res = post(http, server + "/v1/link/start", { { "key", key }, { "time", unixSeconds }, { "sig", picosha2::bytes_to_hex_string(signature) } });
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto& [status, body] = *res;
    if (status != 201 || !body.is_object())
    {
        return std::unexpected(std::format("Mogsafe answered {}", status));
    }
    return LinkCode{
        .code     = body.value("code", ""),
        .poll     = body.value("poll", ""),
        .url      = body.value("url", ""),
        .interval = std::chrono::seconds(body.value("interval", 5)),
        .expires  = std::chrono::seconds(body.value("expires", 600)),
    };
}
catch (const nlohmann::json::exception&) // a field of the wrong type
{
    return std::unexpected("Mogsafe sent a malformed answer");
}

auto pollLink(Http& http, const std::string& server, const std::string& poll) -> std::expected<std::optional<Linked>, std::string>
try
{
    const auto res = post(http, server + "/v1/link/poll", { { "poll", poll } });
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto& [status, body] = *res;
    switch (status)
    {
        case 200:
            if (!body.is_object() || body.value("token", "").empty())
            {
                return std::unexpected("Mogsafe sent no token");
            }
            return Linked{ .token = body.value("token", ""), .account = body.value("account", "") };
        case 202:
            return std::nullopt;
        case 410:
            return std::unexpected("the code expired");
        case 403:
            return std::unexpected(body.is_object() ? body.value("reason", "refused") : "refused");
        default:
            return std::unexpected(std::format("Mogsafe answered {}", status));
    }
}
catch (const nlohmann::json::exception&) // a field of the wrong type
{
    return std::unexpected("Mogsafe sent a malformed answer");
}

// Two lines: the token, then the account it belongs to.
auto loadLink(const std::filesystem::path& path) -> std::optional<Linked>
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

auto saveLink(const std::filesystem::path& path, const Linked& link) -> bool
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << link.token << '\n'
        << link.account << '\n';
    out.close(); // a full disk shows here, not before
    return static_cast<bool>(out);
}

auto removeLink(const std::filesystem::path& path, const std::string& token) -> bool
{
    std::random_device random;
    const auto         taken = std::filesystem::path(path).concat(std::format(".{:08x}{:08x}.old", random(), random()));
    std::error_code    ec;
    std::filesystem::rename(path, taken, ec);
    if (ec)
    {
        return false;
    }
    const auto link    = loadLink(taken);
    const bool refused = link && link->token == token;
    if (!refused)
    {
        // A hard link fails where a token was saved since the rename, so the newer one stays.
        std::filesystem::create_hard_link(taken, path, ec);
    }
    std::filesystem::remove(taken, ec);
    return refused;
}

auto sharing(const std::filesystem::path& config, const std::string& token) -> bool
{
    std::error_code ec;
    if (std::filesystem::exists(config / "paused", ec) || ec)
    {
        return false;
    }
    const auto link = loadLink(config / "token");
    return link && link->token == token;
}

auto captureLink(const std::string& server, const std::string& id, uint64_t fromMs, uint64_t toMs) -> std::string
{
    return std::format("{}/captures/{}?from={}&to={}", server, id, fromMs, toMs);
}

} // namespace memoria
