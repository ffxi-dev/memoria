#pragma once

#include <cstddef>
#include <cstdint>

// Monocypher's Ed25519, compiled into xicap. Declared here rather than included: its folder holds a file named VERSION, which shadows <version> on Windows.
extern "C"
{
    void crypto_ed25519_key_pair(uint8_t secret_key[64], uint8_t public_key[32], uint8_t seed[32]);
    void crypto_ed25519_sign(uint8_t signature[64], const uint8_t secret_key[64], const uint8_t* message, size_t message_size);
    int  crypto_ed25519_check(const uint8_t signature[64], const uint8_t public_key[32], const uint8_t* message, size_t message_size);
    void crypto_wipe(void* secret, size_t size);
}
