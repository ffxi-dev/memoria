#pragma once

#include <cstdint>
#include <string>

namespace memoria::client
{

// The first match of an Ashita byte pattern in FFXiMain.dll, or 0.
auto find(const char* pattern) -> uintptr_t;

// The search server's traffic: player search, the auction house, and the
// linkshell and party lists. FFXiMain encrypts each request and decrypts each
// reply itself, so these hooks see plaintext. Credits: captain's SearchLog.
namespace search
{

using Sink = void (*)(bool clientToServer, const uint8_t* data, uint32_t size);

// Hooks the decrypt and next-page call sites. Returns why not, or empty.
auto install(Sink sink) -> std::string;

// Each frame: the client resets the connection's encrypt pointer whenever it
// reconnects, so it is pointed back at the hook here.
auto tick() -> void;

auto remove() -> void;

} // namespace search

// The client's version as it tells the lobby, "30261006_0": FFXiMain copies
// it from its lobby context into packet 0x26 at character select. The call
// that builds that packet goes through a hook that reads it on the way.
namespace lobby
{

auto install() -> std::string; // why not, or empty
auto version() -> std::string; // empty until character select
auto remove() -> void;

} // namespace lobby

} // namespace memoria::client
