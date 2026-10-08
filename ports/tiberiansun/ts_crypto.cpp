#include "ts_crypto.h"

#include "ts_blowfish.h"

#include <string.h>

namespace {

constexpr unsigned LIMBS = 10;  // 320 bits, least-significant limb first.
struct big_uint {
    uint32_t limb[LIMBS];
};

// DER payload from OpenTS's Keys[] PublicKey.  It is intentionally embedded
// here rather than loading a private key or any game data from the card: only
// the public fast key is needed to unwrap a MIX's per-file Blowfish key.
static const uint8_t s_modulus_be[40] = {
    0x51, 0xBC, 0xDA, 0x08, 0x6D, 0x39, 0xFC, 0xE4, 0x56, 0x51,
    0x60, 0xD6, 0x51, 0x71, 0x3F, 0xA2, 0xE8, 0xAA, 0x54, 0xFA,
    0x66, 0x82, 0xB0, 0x4A, 0xAB, 0xDD, 0x0E, 0x6A, 0xF8, 0xB0,
    0xC1, 0xE6, 0xD1, 0xFB, 0x4F, 0x3D, 0xAA, 0x43, 0x7F, 0x15,
};

static big_uint modulus()
{
    big_uint result{};
    for (unsigned i = 0; i < sizeof(s_modulus_be); ++i) {
        const unsigned little_index = static_cast<unsigned>(sizeof(s_modulus_be) - 1 - i);
        result.limb[little_index / 4] |= static_cast<uint32_t>(s_modulus_be[i]) << ((little_index % 4) * 8);
    }
    return result;
}

static int compare(const big_uint &left, const big_uint &right)
{
    for (unsigned i = LIMBS; i-- != 0;) {
        if (left.limb[i] < right.limb[i]) return -1;
        if (left.limb[i] > right.limb[i]) return 1;
    }
    return 0;
}

static big_uint subtract(const big_uint &left, const big_uint &right)
{
    big_uint result{};
    uint64_t borrow = 0;
    for (unsigned i = 0; i < LIMBS; ++i) {
        const uint64_t value = static_cast<uint64_t>(left.limb[i]) - right.limb[i] - borrow;
        result.limb[i] = static_cast<uint32_t>(value);
        borrow = (static_cast<uint64_t>(left.limb[i]) < static_cast<uint64_t>(right.limb[i]) + borrow) ? 1 : 0;
    }
    return result;
}

// Both inputs are below m.  The m-b form avoids a 321-bit temporary.
static big_uint add_mod(const big_uint &left, const big_uint &right, const big_uint &m)
{
    const big_uint m_minus_right = subtract(m, right);
    if (compare(left, m_minus_right) >= 0) {
        return subtract(left, m_minus_right);
    }
    big_uint result{};
    uint64_t carry = 0;
    for (unsigned i = 0; i < LIMBS; ++i) {
        const uint64_t value = static_cast<uint64_t>(left.limb[i]) + right.limb[i] + carry;
        result.limb[i] = static_cast<uint32_t>(value);
        carry = value >> 32;
    }
    return result;
}

static big_uint multiply_mod(const big_uint &left, const big_uint &right, const big_uint &m)
{
    big_uint result{};
    big_uint addend = left;
    for (unsigned limb = 0; limb < LIMBS; ++limb) {
        uint32_t bits = right.limb[limb];
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((bits & 1U) != 0) {
                result = add_mod(result, addend, m);
            }
            bits >>= 1;
            addend = add_mod(addend, addend, m);
        }
    }
    return result;
}

static big_uint rsa_decrypt_block(const uint8_t encrypted[40])
{
    const big_uint m = modulus();
    big_uint base{};
    memcpy(base.limb, encrypted, sizeof(base.limb));
    while (compare(base, m) >= 0) {
        base = subtract(base, m);
    }

    big_uint result{};
    result.limb[0] = 1;
    uint32_t exponent = 65537;
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            result = multiply_mod(result, base, m);
        }
        exponent >>= 1;
        if (exponent != 0) {
            base = multiply_mod(base, base, m);
        }
    }
    return result;
}

}  // namespace

bool ts_decrypt_mix_key(const uint8_t *encrypted, size_t encrypted_size, uint8_t key[56])
{
    if (encrypted == nullptr || key == nullptr || encrypted_size < 80) {
        return false;
    }
    uint8_t plain[78];
    for (unsigned block = 0; block < 2; ++block) {
        const big_uint decoded = rsa_decrypt_block(encrypted + block * 40);
        memcpy(plain + block * 39, decoded.limb, 39);
    }
    memcpy(key, plain, 56);
    return true;
}

bool ts_decrypt_mix_blocks(const uint8_t key[56], uint8_t *data, size_t size)
{
    if (key == nullptr || data == nullptr || (size & 7U) != 0 || size > 0x7FFFFFFFU) {
        return false;
    }
    TsBlowfish blowfish;
    blowfish.submit_key(key, 56);
    return blowfish.decrypt(data, static_cast<int>(size), data) == static_cast<int>(size);
}
