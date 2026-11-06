#pragma once

#include "core/capture/event_queue.h"

#include <cstdint>
#include <optional>
#include <string>

namespace memoria::plugin::client
{

auto find(const char* pattern) -> uintptr_t; // first match in FFXiMain.dll, or 0

// From the zone server's address; Unknown before the client has one.
auto serverKind() -> xicap::ServerKind;

// Search server traffic in plaintext: FFXiMain encrypts and decrypts it itself (credits: captain's SearchLog).
namespace search
{

using Sink = void (*)(xicap::PacketDirection direction, const uint8_t* data, uint32_t size);

[[nodiscard]] auto install(Sink sink) -> std::optional<std::string>;
auto               tick() -> void; // each frame: a reconnect resets the encrypt pointer
auto               remove() -> void;

} // namespace search

// The client version sent to the lobby in packet 0x26, e.g. "30261006_0", read from FFXiMain's lobby context.
namespace lobby
{

auto version() -> std::string; // empty before the first login

// Fills who's content id, world id and world name from the lobby context, which outlives the login, if the selected
// character is who.
[[nodiscard]] auto fillCharacter(capture::Login& who) -> std::optional<std::string>;

} // namespace lobby

} // namespace memoria::plugin::client
