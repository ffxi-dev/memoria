#include "core/chat.h"

#include <nlohmann/json.hpp>

#include <format>

namespace memoria
{

auto fetchSaveChat(Http& http, const std::string& server, const std::string& installToken) -> std::expected<bool, std::string>
{
    const auto res = http.request("GET", server + "/v1/install/save-chat", { { "Authorization", "Bearer " + installToken } }, {});
    if (!res)
    {
        return std::unexpected(res.error());
    }
    const auto body = nlohmann::json::parse(res->body, nullptr, false);
    if (res->status != 200 || !body.is_object() || !body.contains("save_chat") || !body["save_chat"].is_boolean())
    {
        return std::unexpected(std::format("Mogsafe answered {}", res->status));
    }
    return body["save_chat"].get<bool>();
}

} // namespace memoria
