#ifndef SHA256_H
#define SHA256_H
#include "base.h"

typedef struct {
  u32 h[8];
  u64 len;
  usize fill;
  u8 buf[64];
} Sha256;

typedef struct {
  Sha256 inner;
  Sha256 outer;
} Hmac;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const u8 *p, usize n);
void sha256_final(Sha256 *s, u8 out[32]);
void sha256(const u8 *p, usize n, u8 out[32]);

void hmac_sha256_init(Hmac *h, const u8 *key, usize keylen);
void hmac_sha256_update(Hmac *h, const u8 *p, usize n);
void hmac_sha256_final(Hmac *h, u8 out[32]);
void hmac_sha256(const u8 *key, usize keylen, const u8 *msg, usize msglen, u8 out[32]);

void pbkdf2_sha256(const u8 *pw, usize pwlen, const u8 *salt, usize saltlen,
                   u32 iterations, u8 out[32]);
#endif
