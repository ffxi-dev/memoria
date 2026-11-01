#pragma once

#include "core/http.h"

#include <xicap/types.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace memoria
{

struct LinkCode
{
    std::string          code;
    std::string          poll;
    std::string          url;
    std::chrono::seconds interval{ 5 };
    std::chrono::seconds expires{ 600 };
};

struct Linked
{
    std::string token;
    std::string account;
};

// Asks Mogsafe for a code, proving with the install's key that this install holds it.
auto startLink(Http& http, const std::string& server, const xicap::SecretSeed& seed, uint64_t unixSeconds) -> std::expected<LinkCode, std::string>;

// Empty while the player has not approved yet; an error once the code expired or was refused.
auto pollLink(Http& http, const std::string& server, const std::string& poll) -> std::expected<std::optional<Linked>, std::string>;

auto loadLink(const std::filesystem::path& path) -> std::optional<Linked>;
auto saveLink(const std::filesystem::path& path, const Linked& link) -> bool;

// Deletes the link only while it holds this token: one saved since, even mid-call, stays.
auto removeLink(const std::filesystem::path& path, const std::string& token) -> bool;

} // namespace memoria
