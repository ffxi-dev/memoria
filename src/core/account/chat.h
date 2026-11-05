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

// A linked account chooses on Mogsafe's Memoria page whether its captures save chat. Unlinked installs never do. A token
// Mogsafe refuses is deleted from config, as uploads do.
auto fetchSaveChat(Http& http, const std::string& server, const std::filesystem::path& config, const std::string& installToken) -> std::expected<bool, std::string>;

// The account's choice, kept with the token Mogsafe gave it for. A capture keeps chat only while that token is the
// install's: after an unlink, a revoked token, or a link to another account, captures redact until Mogsafe says otherwise.
class ChatChoice
{
public:
    [[nodiscard]] auto chat(const std::optional<Linked>& link) const -> xicap::Chat;

    // Asks Mogsafe about the install's token in config. An answer for a token replaced while it was asked is dropped, and
    // the new token is asked in turn.
    auto refresh(Http& http, const std::string& server, const std::filesystem::path& config) -> void;

private:
    mutable std::mutex mutex_;
    std::string        token_;
    bool               saveChat_{ false };
};

} // namespace memoria::account
