#ifndef AESGCM_H
#define AESGCM_H
#include "base.h"

#define AES_RK_BYTES (15 * 16)

void aes256_expand(const u8 key[32], u8 rk[AES_RK_BYTES]);
void aes256_encrypt_block(const u8 rk[AES_RK_BYTES], const u8 in[16], u8 out[16]);

void aes256_gcm_encrypt(const u8 key[32], const u8 iv[12],
                        const u8 *aad, usize aadlen,
                        const u8 *pt, usize ptlen, u8 *ct, u8 tag[16]);
// Returns 1 when the tag verifies and `pt` was written, 0 otherwise.
int aes256_gcm_decrypt(const u8 key[32], const u8 iv[12],
                       const u8 *aad, usize aadlen,
                       const u8 *ct, usize ctlen, const u8 tag[16], u8 *pt);
#endif
