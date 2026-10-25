#pragma once

#include <stddef.h>
#include <stdint.h>

// The OpenTS fast key has a 320-bit modulus.  A 56-byte Blowfish key is RSA
// wrapped in two 39-byte plaintext blocks, giving an 80-byte encrypted key.
bool ts_decrypt_mix_key(const uint8_t *encrypted, size_t encrypted_size, uint8_t key[56]);
bool ts_decrypt_mix_blocks(const uint8_t key[56], uint8_t *data, size_t size);

