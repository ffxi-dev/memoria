#include "core/account/chat.h"

#include <nlohmann/json.hpp>

#include <format>
#include <tuple>
#include <utility>

namespace memoria::account
{

auto fetchSaveChat(Http& http, const std::string& server, const std::filesystem::path& config, const std::string& installToken) -> std::expected<bool, std::string>
{
    auto res = http.request("GET", server + "/v1/install/save-chat", { { "Authorization", "Bearer " + installToken } }, {});
    if (res && res->status == 401)
    {
        std::ignore = removeLink(config, installToken);
        return std::unexpected(REFUSED);
    }
    res = expect(std::move(res), 200);
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto body = nlohmann::json::parse(res->body, nullptr, false);
    if (!body.is_object() || !body.contains("save_chat") || !body["save_chat"].is_boolean())
    {
        return std::unexpected(MALFORMED);
    }
    return body["save_chat"].get<bool>();
}

auto ChatChoice::chat(const std::optional<Linked>& link) const -> xicap::Chat
{
    const std::scoped_lock lock(mutex_);
    return link && saveChat_ && link->token == token_ ? xicap::Chat::Keep : xicap::Chat::Redact;
}

auto ChatChoice::refresh(Http& http, const std::string& server, const std::filesystem::path& config) -> void
{
    // Bounded: a token replaced on every answer is someone relinking in a loop, and the next refresh catches up.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const auto asked = loadLink(config);
        if (!asked)
        {
            return;
        }
        const auto saveChat = fetchSaveChat(http, server, config, asked->token);
        const auto now      = loadLink(config);
        if (!now || now->token != asked->token)
        {
            continue;
        }
        if (saveChat)
        {
            const std::scoped_lock lock(mutex_);
            token_    = asked->token;
            saveChat_ = *saveChat;
        }
        return;
    }
}

} // namespace memoria::account
