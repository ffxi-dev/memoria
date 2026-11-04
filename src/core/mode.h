#pragma once

#include "core/http.h"

#include <xicap/pseudonym.h>
#include <xicap/types.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace memoria
{

// What Memoria records. A linked install follows its account's mode, set on Mogsafe's Memoria page; an unlinked one
// keeps its own in config/memoria/mode, set with /memoria mode.
enum class Mode
{
    Default,    // the library's default policy: chat, tells and linkshell board posts cut out
    Everything, // no policy: every packet whole, other players' chat included
    Anonymous,  // Default, with names and player ids replaced by pseudonyms
};

auto modeName(Mode mode) -> std::string_view;
auto parseMode(std::string_view text) -> std::optional<Mode>;

// An unlinked install's mode; Default when there is none, or what is there is not one it can choose.
auto loadMode(const std::filesystem::path& path) -> Mode;
auto saveMode(const std::filesystem::path& path, Mode mode) -> bool;

// The exclusion policy a mode's captures and live frames use.
auto policyFor(Mode mode) -> xicap::ExclusionPolicy;

struct AccountMode
{
    Mode        mode = Mode::Default;
    std::string pseudonymKey; // for anonymous mode's pseudonyms; never printed
};

// The key an anonymous capture's pseudonyms are made with: the account's from Mogsafe, so imports agree, or one derived
// from the install's seed when there is none. Nothing outside anonymous mode.
auto pseudonymKeyFor(const AccountMode& mode, const xicap::SecretSeed& seed) -> std::optional<xicap::PseudonymKey>;

// Asks Mogsafe for the linked account's mode with the install's token.
auto fetchMode(Http& http, const std::string& server, const std::string& installToken) -> std::expected<AccountMode, std::string>;

} // namespace memoria
