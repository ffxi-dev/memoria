#pragma once

#include "core/http.h"

#include <expected>
#include <string>

namespace memoria::account
{

// A linked account chooses on Mogsafe's Memoria page whether its captures save chat. Unlinked installs never do.
auto fetchSaveChat(Http& http, const std::string& server, const std::string& installToken) -> std::expected<bool, std::string>;

} // namespace memoria::account
