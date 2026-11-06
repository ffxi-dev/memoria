#pragma once

#include "core/http.h"
#include "core/install.h"

#include <xicap/enums/chat.h>

#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace memoria::account
{

// A 401 deletes installToken from config.
auto fetchSaveChat(Http& http, const std::string& server, const std::filesystem::path& config, const std::string& installToken) -> std::expected<bool, std::string>;

// Mogsafe's answer, tied to the token it was asked with: once the install's token changes, captures redact until Mogsafe
// answers for the new one.
class ChatChoice
{
public:
    [[nodiscard]] auto chat(const std::optional<Linked>& link) const -> xicap::Chat;

    // Drops an answer for a token replaced mid-call, and asks again for the new one.
    auto refresh(Http& http, const std::string& server, const std::filesystem::path& config) -> void;

private:
    mutable std::mutex mutex_;
    std::string        token_;
    bool               saveChat_{ false };
};

} // namespace memoria::account
