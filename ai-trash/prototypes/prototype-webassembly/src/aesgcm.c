// AES-256 and GCM (FIPS 197, SP 800-38D).
//
// Byte-oriented rather than T-table: the blobs here are a few hundred bytes,
// so the table version would buy nothing measurable and would cost the cache
// footprint that makes table-driven AES a side-channel problem in the first
// place. GHASH is the plain shift-and-xor multiplication for the same reason —
// no precomputed tables of H.
#include "aesgcm.h"
#include "base.h"

static const u8 SBOX[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

// x^i in GF(2^8) for the key schedule.
static const u8 RCON[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

// AES-256: 14 rounds, 15 round keys of 16 bytes.
#define NR 14
#define RK_BYTES ((NR + 1) * 16)

void aes256_expand(const u8 key[32], u8 rk[RK_BYTES]) {
  mem_copy(rk, key, 32);
  // 8 words of key, expanding to 60 words.
  for (int i = 8; i < 60; i++) {
    u8 t[4];
    mem_copy(t, rk + (i - 1) * 4, 4);
    if (i % 8 == 0) {
      u8 tmp = t[0];
      t[0] = (u8)(SBOX[t[1]] ^ RCON[i / 8]);
      t[1] = SBOX[t[2]];
      t[2] = SBOX[t[3]];
      t[3] = SBOX[tmp];
    } else if (i % 8 == 4) {
      // The extra SubWord at the halfway point is what makes AES-256's
      // schedule different from AES-128's.
      for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
    }
    for (int j = 0; j < 4; j++) rk[i * 4 + j] = (u8)(rk[(i - 8) * 4 + j] ^ t[j]);
  }
}

static u8 xtime(u8 a) { return (u8)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }

void aes256_encrypt_block(const u8 rk[RK_BYTES], const u8 in[16], u8 out[16]) {
  u8 s[16];
  for (int i = 0; i < 16; i++) s[i] = (u8)(in[i] ^ rk[i]);

  for (int round = 1; round <= NR; round++) {
    for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];

    // ShiftRows — the state is column-major, so row r moves left by r.
    u8 t[16];
    for (int c = 0; c < 4; c++) {
      for (int r = 0; r < 4; r++) t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
    }
    mem_copy(s, t, 16);

    if (round != NR) {
      for (int c = 0; c < 4; c++) {
        u8 *p = s + c * 4;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        u8 x = (u8)(a0 ^ a1 ^ a2 ^ a3);
        p[0] = (u8)(a0 ^ x ^ xtime((u8)(a0 ^ a1)));
        p[1] = (u8)(a1 ^ x ^ xtime((u8)(a1 ^ a2)));
        p[2] = (u8)(a2 ^ x ^ xtime((u8)(a2 ^ a3)));
        p[3] = (u8)(a3 ^ x ^ xtime((u8)(a3 ^ a0)));
      }
    }
    for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
  }
  mem_copy(out, s, 16);
}

// ---- GHASH ---------------------------------------------------------------
//
// Multiplication in GF(2^128) with the GCM bit order: bit 0 of byte 0 is the
// most significant coefficient, and the reduction polynomial is
// x^128 + x^7 + x^2 + x + 1, applied as 0xe1 into the top byte on a right
// shift.
static void gf_mul(u8 x[16], const u8 y[16]) {
  u8 z[16];
  u8 v[16];
  mem_set(z, 0, 16);
  mem_copy(v, y, 16);
  for (int i = 0; i < 128; i++) {
    int bit = (x[i >> 3] >> (7 - (i & 7))) & 1;
    if (bit) {
      for (int j = 0; j < 16; j++) z[j] ^= v[j];
    }
    int lsb = v[15] & 1;
    for (int j = 15; j > 0; j--) v[j] = (u8)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
    v[0] >>= 1;
    if (lsb) v[0] ^= 0xe1;
  }
  mem_copy(x, z, 16);
}

static void ghash_blocks(u8 acc[16], const u8 h[16], const u8 *data, usize len) {
  usize full = len / 16;
  for (usize i = 0; i < full; i++) {
    for (int j = 0; j < 16; j++) acc[j] ^= data[i * 16 + j];
    gf_mul(acc, h);
  }
  usize rest = len % 16;
  if (rest) {
    for (usize j = 0; j < rest; j++) acc[j] ^= data[full * 16 + j];
    gf_mul(acc, h);
  }
}

static void put_be64(u8 *p, u64 v) {
  for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (56 - i * 8));
}

// Counter blocks: J0 = IV || 0x00000001 for a 12-byte IV, and the keystream
// starts at J0+1. Only 12-byte IVs are supported, which is what WebCrypto's
// default and this app both use.
static void gcm_crypt(const u8 rk[RK_BYTES], const u8 iv[12],
                      const u8 *in, usize len, u8 *out) {
  u8 ctr[16], ks[16];
  mem_copy(ctr, iv, 12);
  ctr[12] = 0; ctr[13] = 0; ctr[14] = 0; ctr[15] = 1;
  usize done = 0;
  while (done < len) {
    // Increment the 32-bit counter first: block 1 of the keystream uses J0+1.
    for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; }
    aes256_encrypt_block(rk, ctr, ks);
    usize take = len - done < 16 ? len - done : 16;
    for (usize i = 0; i < take; i++) out[done + i] = (u8)(in[done + i] ^ ks[i]);
    done += take;
  }
}

static void gcm_tag(const u8 rk[RK_BYTES], const u8 h[16], const u8 iv[12],
                    const u8 *aad, usize aadlen, const u8 *ct, usize ctlen,
                    u8 tag[16]) {
  u8 acc[16];
  mem_set(acc, 0, 16);
  ghash_blocks(acc, h, aad, aadlen);
  ghash_blocks(acc, h, ct, ctlen);
  u8 lens[16];
  put_be64(lens, (u64)aadlen * 8u);
  put_be64(lens + 8, (u64)ctlen * 8u);
  for (int j = 0; j < 16; j++) acc[j] ^= lens[j];
  gf_mul(acc, h);

  u8 j0[16], s[16];
  mem_copy(j0, iv, 12);
  j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
  aes256_encrypt_block(rk, j0, s);
  for (int j = 0; j < 16; j++) tag[j] = (u8)(acc[j] ^ s[j]);
}

void aes256_gcm_encrypt(const u8 key[32], const u8 iv[12],
                        const u8 *aad, usize aadlen,
                        const u8 *pt, usize ptlen, u8 *ct, u8 tag[16]) {
  u8 rk[RK_BYTES], h[16], zero[16];
  aes256_expand(key, rk);
  mem_set(zero, 0, 16);
  aes256_encrypt_block(rk, zero, h);
  gcm_crypt(rk, iv, pt, ptlen, ct);
  gcm_tag(rk, h, iv, aad, aadlen, ct, ptlen, tag);
}

int aes256_gcm_decrypt(const u8 key[32], const u8 iv[12],
                       const u8 *aad, usize aadlen,
                       const u8 *ct, usize ctlen, const u8 tag[16], u8 *pt) {
  u8 rk[RK_BYTES], h[16], zero[16], want[16];
  aes256_expand(key, rk);
  mem_set(zero, 0, 16);
  aes256_encrypt_block(rk, zero, h);
  gcm_tag(rk, h, iv, aad, aadlen, ct, ctlen, want);
  // Verify before releasing any plaintext, and compare without an early exit.
  if (!mem_equal_ct(want, tag, 16)) return 0;
  gcm_crypt(rk, iv, ct, ctlen, pt);
  return 1;
}
