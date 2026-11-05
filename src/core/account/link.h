#pragma once

#include "core/http.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
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

struct Linked
{
    std::string token;
    std::string account;
};

// Asks Mogsafe for a code the player enters on the link page, and a secret handle to poll with.
auto startLink(Http& http, const std::string& server) -> std::expected<LinkCode, std::string>;

// Empty while the player has not approved yet; an error once the code expired or was refused.
auto pollLink(Http& http, const std::string& server, const std::string& poll) -> std::expected<std::optional<Linked>, std::string>;

auto loadLink(const std::filesystem::path& path) -> std::optional<Linked>;
auto saveLink(const std::filesystem::path& path, const Linked& link) -> bool;

// A recording's page on Mogsafe, opened on the range fromMs to toMs (Unix ms), as Mogsafe's capture page reads ?from=&to=.
auto captureLink(const std::string& server, const std::string& id, uint64_t fromMs, uint64_t toMs) -> std::string;

using LinkBack = std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)>;

// Deletes the link only while it holds this token: one saved since, even mid-call, stays. linkBack puts back a token
// taken by mistake, as a hard link; tests make it fail.
auto removeLink(const std::filesystem::path& path, const std::string& token, const LinkBack& linkBack = {}) -> bool;

// Whether any client sharing the install ran /memoria upload off.
auto paused(const std::filesystem::path& config) -> bool;

// True while uploads are not paused and the install's token file still holds token.
auto sharing(const std::filesystem::path& config, const std::string& token) -> bool;

} // namespace memoria::account
