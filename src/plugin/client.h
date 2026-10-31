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

} // namespace memoria::client
