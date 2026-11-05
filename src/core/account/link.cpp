#include "core/account/link.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <tuple>

namespace memoria::account
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

auto startLink(Http& http, const std::string& server) -> std::expected<LinkCode, std::string>
try
{
    const auto res = post(http, server + "/v1/link/start", nlohmann::json::object());
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto& [status, body] = *res;
    if (status != 201)
    {
        return std::unexpected(std::format("Mogsafe answered {}", status));
    }
    if (!body.is_object())
    {
        return std::unexpected(MALFORMED);
    }
    LinkCode code{
        .code     = body.value("code", ""),
        .poll     = body.value("poll", ""),
        .url      = body.value("url", ""),
        .interval = std::chrono::seconds(body.value("interval", 5)),
        .expires  = std::chrono::seconds(body.value("expires", 600)),
    };
    if (code.interval <= std::chrono::seconds(0) || code.expires <= std::chrono::seconds(0))
    {
        return std::unexpected(MALFORMED); // a poll every 0 s would hammer it
    }
    return code;
}
catch (const nlohmann::json::exception&) // a field of the wrong type
{
    return std::unexpected(MALFORMED);
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
    return std::unexpected(MALFORMED);
}

auto waitForApproval(Http& http, const std::string& server, const LinkCode& code, const std::stop_token& stop) -> std::expected<Linked, std::string>
{
    std::mutex                  mutex;
    std::condition_variable_any wake;
    const auto                  until = std::chrono::steady_clock::now() + code.expires;
    while (std::chrono::steady_clock::now() < until)
    {
        std::unique_lock lock(mutex);
        std::ignore = wake.wait_for(lock, stop, code.interval, []
                                    {
                                        return false;
                                    });
        if (stop.stop_requested())
        {
            return std::unexpected("stopped");
        }
        const auto result = pollLink(http, server, code.poll);
        if (stop.stop_requested())
        {
            return std::unexpected("stopped");
        }
        if (!result)
        {
            return std::unexpected(result.error());
        }
        if (*result)
        {
            return **result;
        }
    }
    return std::unexpected("the code expired");
}

} // namespace memoria::account
