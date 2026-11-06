#pragma once

#include "core/http.h"
#include "core/install.h"

#include <chrono>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>

namespace memoria::account
{

struct LinkCode
{
    std::string          code;
    std::string          poll;
    std::string          url;
    std::chrono::seconds interval{ 5 };
    std::chrono::seconds expires{ 600 };
};

// Asks Mogsafe for a code the player enters on the link page, and a secret handle to poll with.
auto startLink(Http& http, const std::string& server) -> std::expected<LinkCode, std::string>;

// Empty while the player has not approved yet; an error once the code expired or was refused.
auto pollLink(Http& http, const std::string& server, const std::string& poll) -> std::expected<std::optional<Linked>, std::string>;

// A stop during a poll wins over the approval it brings, so the caller publishes no token after a stop.
auto waitForApproval(Http& http, const std::string& server, const LinkCode& code, const std::stop_token& stop) -> std::expected<Linked, std::string>;

} // namespace memoria::account
