#pragma once

#include "core/http.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace memoria
{

// The install's config folder, which every client sharing the install reads: the link's token, the pause, the
// daily reminder, and upload.ini.

struct Linked
{
    std::string token;
    std::string account;
};

auto loadLink(const std::filesystem::path& config) -> std::optional<Linked>;

// Written whole to a file only this user can open, flushed to disk, then renamed over the old token. Why not, if it was
// not.
[[nodiscard]] auto saveLink(const std::filesystem::path& config, const Linked& link) -> std::optional<std::string>;

using LinkBack = std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)>;

// What every Mogsafe call says when Mogsafe refuses the install's token (a 401), after removeLink.
inline constexpr auto REFUSED = "Mogsafe no longer accepts this install";

// expect() for a call made with the install's token: a 401 also deletes that token, as removeLink does, and is REFUSED.
auto expect(std::expected<HttpResponse, std::string> res, int status, const std::filesystem::path& config, const std::string& token) -> std::expected<HttpResponse, std::string>;

// Deletes the link only while it holds this token: one saved since, even mid-call, stays. True when it deleted it.
// linkBack puts back a token taken by mistake, as a hard link; tests make it fail.
auto removeLink(const std::filesystem::path& config, const std::string& token, const LinkBack& linkBack = {}) -> bool;

// /memoria unlink: deletes whatever token is there. Why not, if one is still there.
[[nodiscard]] auto unlink(const std::filesystem::path& config) -> std::optional<std::string>;

// Whether any client sharing the install ran /memoria upload off.
auto paused(const std::filesystem::path& config) -> bool;

// /memoria upload off and on. Why not, if the pause did not change.
[[nodiscard]] auto setPaused(const std::filesystem::path& config, bool on) -> std::optional<std::string>;

// True while uploads are not paused and the install's token file still holds token.
auto sharing(const std::filesystem::path& config, const std::string& token) -> bool;

// True at most once a day, across restarts: the first caller each day claims the daily unlinked reminder.
auto claimDailyReminder(const std::filesystem::path& config) -> bool;

// upload.ini's "server = https://..." line, which points Memoria at another Mogsafe.
auto serverIn(const std::filesystem::path& config) -> std::optional<std::string>;

} // namespace memoria
