// OpenTS/Westwood Blowfish implementation used for encrypted MIX headers.
#pragma once

#include <climits>

class TsBlowfish {
public:
    TsBlowfish();
    ~TsBlowfish();

    void submit_key(const void *key, int length);
    int encrypt(const void *plaintext, int length, void *ciphertext);
    int decrypt(const void *ciphertext, int length, void *plaintext);

private:
    void process_block(const void *plaintext, void *ciphertext, const unsigned int *ptable);
    void sub_key_encrypt(unsigned int &left, unsigned int &right);

    enum { ROUNDS = 16, BYTES_PER_BLOCK = 8, MAX_KEY_LENGTH = 56 };
    bool keyed_;
    unsigned int p_encrypt_[ROUNDS + 2];
    unsigned int p_decrypt_[ROUNDS + 2];
    unsigned int bf_s_[4][UCHAR_MAX + 1];

    static const unsigned int p_init_[ROUNDS + 2];
    static const unsigned int s_init_[4][UCHAR_MAX + 1];
};
