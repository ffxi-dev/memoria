#pragma once

#include "core/http.h"

#include <xicap/policy.h>

#include <expected>
#include <string>

namespace memoria
{

// A linked account chooses on Mogsafe's Memoria page whether its captures save chat. Unlinked installs never do.
auto fetchSaveChat(Http& http, const std::string& server, const std::string& installToken) -> std::expected<bool, std::string>;

// The policy for a capture, its session file and its live frames: xicap's default, or none when chat is saved.
auto policyFor(bool saveChat) -> xicap::ExclusionPolicy;

} // namespace memoria
