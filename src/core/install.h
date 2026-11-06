#pragma once

#include "core/http.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace memoria
{

struct Linked
{
    std::string token;
    std::string account;
};

auto loadLink(const std::filesystem::path& config) -> std::optional<Linked>;

// Writes an owner-only file and renames it over the token, so other clients never read half a token.
[[nodiscard]] auto saveLink(const std::filesystem::path& config, const Linked& link) -> std::optional<std::string>;

using LinkBack = std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)>;

// The error for a 401 on the install's token.
inline constexpr auto REFUSED = "Mogsafe no longer accepts this install";

// As expect(), but a 401 also removes token and returns REFUSED.
auto expect(std::expected<HttpResponse, std::string> res, int status, const std::filesystem::path& config, const std::string& token) -> std::expected<HttpResponse, std::string>;

// Deletes the link if it holds token; one saved since, even mid-call, stays. linkBack restores a token taken by
// mistake, as a hard link unless a test makes it fail.
auto removeLink(const std::filesystem::path& config, const std::string& token, const LinkBack& linkBack = {}) -> bool;

// Deletes the token, whichever it holds.
[[nodiscard]] auto unlink(const std::filesystem::path& config) -> std::optional<std::string>;

auto paused(const std::filesystem::path& config) -> bool;

[[nodiscard]] auto setPaused(const std::filesystem::path& config, bool on) -> std::optional<std::string>;

// Not paused, and the token file still holds token.
auto sharing(const std::filesystem::path& config, const std::string& token) -> bool;

// True for the first caller each day, across restarts and clients.
auto claimDailyReminder(const std::filesystem::path& config) -> bool;

// From upload.ini's "server = https://..." line.
auto serverIn(const std::filesystem::path& config) -> std::optional<std::string>;

} // namespace memoria
