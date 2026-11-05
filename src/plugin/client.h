#pragma once

#include "core/event_queue.h"

#include <cstdint>
#include <string>

namespace memoria::client
{

auto find(const char* pattern) -> uintptr_t; // first match in FFXiMain.dll, or 0

// Retail or private, from the zone server's address; unknown before the client has one.
auto network() -> xicap::Network;

// Search server traffic in plaintext: FFXiMain encrypts requests and decrypts replies itself (credits: captain's SearchLog).
namespace search
{

using Sink = void (*)(bool clientToServer, const uint8_t* data, uint32_t size);

auto install(Sink sink) -> std::string; // why not, or empty
auto tick() -> void;                    // each frame: a reconnect resets the encrypt pointer
auto remove() -> void;

} // namespace search

// The version the client sends the lobby in packet 0x26, e.g. "30261006_0", read from FFXiMain's lobby context as the packet is built.
namespace lobby
{

auto version() -> std::string; // from the same copy of the lobby context; empty before the first login

// Fills who's content id, world id, and world name from FFXiMain's copy of the lobby context, which outlives the login, if the selected character is who's character id.
auto character(Login& who) -> std::string; // why not, or empty

} // namespace lobby

} // namespace memoria::client
