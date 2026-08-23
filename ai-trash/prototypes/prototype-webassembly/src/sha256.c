// SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (FIPS 180-4, RFC 2104, RFC 2898).
//
// prototype-minimal gets these from WebCrypto. Here they are the real thing in
// C, which is also the reason sign-in takes a moment: 310 000 iterations is
// 620 000 compression functions, and this is a plain portable implementation
// with no SIMD.
#include "sha256.h"
#include "base.h"

static const u32 K[64] = {
  0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
  0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
  0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
  0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
  0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
  0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
  0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
  0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
  0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
  0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
  0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(Sha256 *s, const u8 *p) {
  u32 w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16)
         | ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    u32 ch = (e & f) ^ (~e & g);
    u32 t1 = h + S1 + ch + K[i] + w[i];
    u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    u32 maj = (a & b) ^ (a & c) ^ (b & c);
    u32 t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_init(Sha256 *s) {
  s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
  s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
  s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
  s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
  s->len = 0;
  s->fill = 0;
}

void sha256_update(Sha256 *s, const u8 *p, usize n) {
  s->len += n;
  while (n) {
    usize take = 64 - s->fill;
    if (take > n) take = n;
    mem_copy(s->buf + s->fill, p, take);
    s->fill += take;
    p += take;
    n -= take;
    if (s->fill == 64) {
      sha256_block(s, s->buf);
      s->fill = 0;
    }
  }
}

void sha256_final(Sha256 *s, u8 out[32]) {
  u64 bits = s->len * 8u;
  u8 pad = 0x80;
  sha256_update(s, &pad, 1);
  u8 zero = 0;
  while (s->fill != 56) sha256_update(s, &zero, 1);
  u8 len[8];
  for (int i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - i * 8));
  sha256_update(s, len, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (u8)(s->h[i] >> 24);
    out[i * 4 + 1] = (u8)(s->h[i] >> 16);
    out[i * 4 + 2] = (u8)(s->h[i] >> 8);
    out[i * 4 + 3] = (u8)s->h[i];
  }
}

void sha256(const u8 *p, usize n, u8 out[32]) {
  Sha256 s;
  sha256_init(&s);
  sha256_update(&s, p, n);
  sha256_final(&s, out);
}

void hmac_sha256_init(Hmac *h, const u8 *key, usize keylen) {
  u8 k[64];
  mem_set(k, 0, 64);
  if (keylen > 64) {
    sha256(key, keylen, k);
  } else {
    mem_copy(k, key, keylen);
  }
  u8 pad[64];
  for (int i = 0; i < 64; i++) pad[i] = (u8)(k[i] ^ 0x36);
  sha256_init(&h->inner);
  sha256_update(&h->inner, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = (u8)(k[i] ^ 0x5c);
  sha256_init(&h->outer);
  sha256_update(&h->outer, pad, 64);
}

void hmac_sha256_update(Hmac *h, const u8 *p, usize n) {
  sha256_update(&h->inner, p, n);
}

void hmac_sha256_final(Hmac *h, u8 out[32]) {
  u8 ih[32];
  sha256_final(&h->inner, ih);
  sha256_update(&h->outer, ih, 32);
  sha256_final(&h->outer, out);
}

void hmac_sha256(const u8 *key, usize keylen, const u8 *msg, usize msglen, u8 out[32]) {
  Hmac h;
  hmac_sha256_init(&h, key, keylen);
  hmac_sha256_update(&h, msg, msglen);
  hmac_sha256_final(&h, out);
}

// PBKDF2 with dkLen == 32, which is the only size this app asks for: one
// block, so no outer loop over blocks. The inner HMAC key is the password
// throughout, so the padded key blocks could be hoisted — left plain because
// the win is small next to 620 000 compressions and the clarity is worth more.
void pbkdf2_sha256(const u8 *pw, usize pwlen, const u8 *salt, usize saltlen,
                   u32 iterations, u8 out[32]) {
  u8 block[4] = {0, 0, 0, 1};
  u8 u[32], t[32];

  Hmac h;
  hmac_sha256_init(&h, pw, pwlen);
  hmac_sha256_update(&h, salt, saltlen);
  hmac_sha256_update(&h, block, 4);
  hmac_sha256_final(&h, u);
  mem_copy(t, u, 32);

  for (u32 i = 1; i < iterations; i++) {
    hmac_sha256(pw, pwlen, u, 32, u);
    for (int j = 0; j < 32; j++) t[j] ^= u[j];
  }
  mem_copy(out, t, 32);
}
