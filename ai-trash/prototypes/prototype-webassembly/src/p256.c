// ECDSA P-256 key generation, written out in full: 256-bit modular arithmetic,
// curve arithmetic, and the DER encodings WebCrypto expects.
//
// Montgomery multiplication rather than the Solinas fast reduction that P-256
// is famous for. Solinas is faster and much easier to get subtly wrong; this
// runs once per account, so uniform beats clever.
//
// NOT a hardened implementation. Scalar multiplication uses double-and-always-
// add with a constant-time select, so the *choice* of result does not branch on
// the key bits — but point addition still branches on the infinity case, there
// is no scalar blinding, and nothing here defends against what an optimising
// compiler or a cache does. Fine for a prototype that generates a throwaway
// keypair; not fine for a key that matters.
#include "p256.h"
#include "base.h"

#define L 8  // 32-bit limbs, little-endian: fe[0] is the least significant

typedef u32 fe[L];

// p = 2^256 - 2^224 + 2^192 + 2^96 - 1
static const fe P = {
  0xffffffffu, 0xffffffffu, 0xffffffffu, 0x00000000u,
  0x00000000u, 0x00000000u, 0x00000001u, 0xffffffffu,
};
// n, the order of G
static const fe N = {
  0xfc632551u, 0xf3b9cac2u, 0xa7179e84u, 0xbce6faadu,
  0xffffffffu, 0xffffffffu, 0x00000000u, 0xffffffffu,
};
// b of y^2 = x^3 - 3x + b
static const fe B = {
  0x27d2604bu, 0x3bce3c3eu, 0xcc53b0f6u, 0x651d06b0u,
  0x769886bcu, 0xb3ebbd55u, 0xaa3a93e7u, 0x5ac635d8u,
};
static const fe GX = {
  0xd898c296u, 0xf4a13945u, 0x2deb33a0u, 0x77037d81u,
  0x63a440f2u, 0xf8bce6e5u, 0xe12c4247u, 0x6b17d1f2u,
};
static const fe GY = {
  0x37bf51f5u, 0xcbb64068u, 0x6b315eceu, 0x2bce3357u,
  0x7c0f9e16u, 0x8ee7eb4au, 0xfe1a7f9bu, 0x4fe342e2u,
};

// -p^-1 mod 2^32. The low limb of p is 0xffffffff = -1 mod 2^32, so the
// inverse is -1 and its negation is 1.
#define N0 1u

// R^2 mod p, computed at init rather than hardcoded — one less constant that
// can be silently wrong.
static fe R2;
static fe ONE_MONT;   // R mod p, which is 1 in Montgomery form
static int ready = 0;

// ---- plain big-integer helpers -------------------------------------------

static void fe_zero(fe a) { for (int i = 0; i < L; i++) a[i] = 0; }
static void fe_copy(fe a, const fe b) { for (int i = 0; i < L; i++) a[i] = b[i]; }

static int fe_is_zero(const fe a) {
  u32 acc = 0;
  for (int i = 0; i < L; i++) acc |= a[i];
  return acc == 0;
}

// -1, 0, 1
static int fe_cmp(const fe a, const fe b) {
  for (int i = L - 1; i >= 0; i--) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

// Returns the borrow, so it doubles as a "was a < b" test.
static u32 fe_sub_raw(fe r, const fe a, const fe b) {
  u64 borrow = 0;
  for (int i = 0; i < L; i++) {
    u64 d = (u64)a[i] - (u64)b[i] - borrow;
    r[i] = (u32)d;
    borrow = (d >> 63) & 1u;
  }
  return (u32)borrow;
}

static u32 fe_add_raw(fe r, const fe a, const fe b) {
  u64 carry = 0;
  for (int i = 0; i < L; i++) {
    u64 s = (u64)a[i] + (u64)b[i] + carry;
    r[i] = (u32)s;
    carry = s >> 32;
  }
  return (u32)carry;
}

// Copy b into r when flag is 1, leave r alone when flag is 0, without
// branching on flag.
static void fe_cmov(fe r, const fe b, u32 flag) {
  u32 mask = (u32)0 - (flag & 1u);
  for (int i = 0; i < L; i++) r[i] = (r[i] & ~mask) | (b[i] & mask);
}

// ---- arithmetic mod p ----------------------------------------------------

static void fe_add(fe r, const fe a, const fe b) {
  u32 carry = fe_add_raw(r, a, b);
  fe t;
  u32 borrow = fe_sub_raw(t, r, P);
  // Reduce when the sum overflowed 2^256 or is already >= p.
  fe_cmov(r, t, carry | (borrow ^ 1u));
}

static void fe_sub(fe r, const fe a, const fe b) {
  u32 borrow = fe_sub_raw(r, a, b);
  fe t;
  fe_add_raw(t, r, P);
  fe_cmov(r, t, borrow);
}

static void fe_half_add(fe r, const fe a) { fe_add(r, a, a); }

// CIOS Montgomery multiplication: r = a*b*R^-1 mod p.
static void fe_mul(fe r, const fe a, const fe b) {
  u32 t[L + 2];
  for (int i = 0; i < L + 2; i++) t[i] = 0;

  for (int i = 0; i < L; i++) {
    u64 carry = 0;
    for (int j = 0; j < L; j++) {
      u64 s = (u64)t[j] + (u64)a[i] * (u64)b[j] + carry;
      t[j] = (u32)s;
      carry = s >> 32;
    }
    u64 s = (u64)t[L] + carry;
    t[L] = (u32)s;
    t[L + 1] = (u32)(s >> 32);

    u32 m = t[0] * N0;
    u64 c2 = 0;
    {
      u64 s0 = (u64)t[0] + (u64)m * (u64)P[0];
      c2 = s0 >> 32;  // t[0] is now zero by construction and gets shifted out
    }
    for (int j = 1; j < L; j++) {
      u64 s2 = (u64)t[j] + (u64)m * (u64)P[j] + c2;
      t[j - 1] = (u32)s2;
      c2 = s2 >> 32;
    }
    u64 s3 = (u64)t[L] + c2;
    t[L - 1] = (u32)s3;
    t[L] = (u32)(t[L + 1] + (u32)(s3 >> 32));
    t[L + 1] = 0;
  }

  fe out;
  for (int i = 0; i < L; i++) out[i] = t[i];
  fe red;
  u32 borrow = fe_sub_raw(red, out, P);
  // Subtract p when the extra limb is set, or when the value already fits
  // above p.
  fe_cmov(out, red, (t[L] != 0) | (borrow ^ 1u));
  fe_copy(r, out);
}

static void fe_sqr(fe r, const fe a) { fe_mul(r, a, a); }

// a^-1 mod p by Fermat: a^(p-2). Square-and-multiply straight off the bits of
// p-2, which is public, so the branch pattern leaks nothing.
static void fe_inv(fe r, const fe a) {
  fe exp;
  fe two;
  fe_zero(two);
  two[0] = 2;
  fe_sub_raw(exp, P, two);  // p - 2, no borrow: p > 2

  fe result;
  fe_copy(result, ONE_MONT);
  fe base_;
  fe_copy(base_, a);
  for (int i = 0; i < 256; i++) {
    if ((exp[i >> 5] >> (i & 31)) & 1u) fe_mul(result, result, base_);
    fe_sqr(base_, base_);
  }
  fe_copy(r, result);
}

static void to_mont(fe r, const fe a) { fe_mul(r, a, R2); }

static void from_mont(fe r, const fe a) {
  fe one;
  fe_zero(one);
  one[0] = 1;
  fe_mul(r, a, one);
}

// R^2 mod p = 2^512 mod p, reached by doubling 1 five hundred and twelve times.
// Slow and obviously correct, which is the point.
static void init_constants(void) {
  if (ready) return;
  fe x;
  fe_zero(x);
  x[0] = 1;
  for (int i = 0; i < 512; i++) fe_half_add(x, x);
  fe_copy(R2, x);

  fe one;
  fe_zero(one);
  one[0] = 1;
  to_mont(ONE_MONT, one);
  ready = 1;
}

// ---- curve points, Jacobian (X : Y : Z), all coordinates in Montgomery form
//
// Z == 0 is the point at infinity.

typedef struct { fe X, Y, Z; } Point;

static void point_zero(Point *p) {
  fe_zero(p->X); fe_zero(p->Y); fe_zero(p->Z);
  p->X[0] = 1; p->Y[0] = 1;  // any non-zero, Z decides
  to_mont(p->X, p->X);
  to_mont(p->Y, p->Y);
}

static int point_is_zero(const Point *p) { return fe_is_zero(p->Z); }

// dbl-2001-b, the a == -3 shortcut.
static void point_double(Point *r, const Point *p) {
  if (point_is_zero(p)) { *r = *p; return; }
  fe delta, gamma, beta, alpha, t0, t1, t2;

  fe_sqr(delta, p->Z);
  fe_sqr(gamma, p->Y);
  fe_mul(beta, p->X, gamma);

  fe_sub(t0, p->X, delta);
  fe_add(t1, p->X, delta);
  fe_mul(t2, t0, t1);
  fe_add(alpha, t2, t2);
  fe_add(alpha, alpha, t2);          // 3*(X-delta)*(X+delta)

  fe_sqr(t0, alpha);
  fe_add(t1, beta, beta);
  fe_add(t1, t1, t1);                // 4*beta
  fe_sub(t0, t0, t1);
  fe_sub(t0, t0, t1);                // alpha^2 - 8*beta
  fe X3;
  fe_copy(X3, t0);

  fe_add(t2, p->Y, p->Z);
  fe_sqr(t2, t2);
  fe_sub(t2, t2, gamma);
  fe_sub(t2, t2, delta);             // (Y+Z)^2 - gamma - delta
  fe Z3;
  fe_copy(Z3, t2);

  fe_sub(t0, t1, X3);                // 4*beta - X3
  fe_mul(t0, alpha, t0);
  fe_sqr(t1, gamma);
  fe_add(t1, t1, t1);
  fe_add(t1, t1, t1);
  fe_add(t1, t1, t1);                // 8*gamma^2
  fe_sub(t0, t0, t1);

  fe_copy(r->X, X3);
  fe_copy(r->Y, t0);
  fe_copy(r->Z, Z3);
}

// add-2007-bl
static void point_add(Point *r, const Point *p, const Point *q) {
  if (point_is_zero(p)) { *r = *q; return; }
  if (point_is_zero(q)) { *r = *p; return; }

  fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t0, t1;
  fe_sqr(z1z1, p->Z);
  fe_sqr(z2z2, q->Z);
  fe_mul(u1, p->X, z2z2);
  fe_mul(u2, q->X, z1z1);
  fe_mul(t0, q->Z, z2z2);
  fe_mul(s1, p->Y, t0);
  fe_mul(t0, p->Z, z1z1);
  fe_mul(s2, q->Y, t0);

  fe_sub(h, u2, u1);
  fe_sub(rr, s2, s1);

  if (fe_is_zero(h)) {
    // Same x: either the same point (double it) or opposite (infinity).
    if (fe_is_zero(rr)) { point_double(r, p); return; }
    point_zero(r);
    return;
  }

  fe_add(t0, h, h);
  fe_sqr(i, t0);
  fe_mul(j, h, i);
  fe_add(rr, rr, rr);
  fe_mul(v, u1, i);

  fe_sqr(t0, rr);
  fe_sub(t0, t0, j);
  fe_sub(t0, t0, v);
  fe_sub(t0, t0, v);                 // X3
  fe X3;
  fe_copy(X3, t0);

  fe_sub(t1, v, X3);
  fe_mul(t1, rr, t1);
  fe_mul(t0, s1, j);
  fe_add(t0, t0, t0);
  fe_sub(t1, t1, t0);                // Y3

  fe_add(t0, p->Z, q->Z);
  fe_sqr(t0, t0);
  fe_sub(t0, t0, z1z1);
  fe_sub(t0, t0, z2z2);
  fe_mul(t0, t0, h);                 // Z3

  fe_copy(r->X, X3);
  fe_copy(r->Y, t1);
  fe_copy(r->Z, t0);
}

static void point_cmov(Point *r, const Point *b, u32 flag) {
  fe_cmov(r->X, b->X, flag);
  fe_cmov(r->Y, b->Y, flag);
  fe_cmov(r->Z, b->Z, flag);
}

// k*G, MSB first, adding on every bit and selecting the result. The add is
// performed whatever the bit says, so the arithmetic does not depend on the
// key — see the caveat at the top of the file for what this does and does not
// buy.
static void scalar_mult_g(Point *out, const fe k) {
  Point g, acc, sum;
  fe_copy(g.X, GX);
  fe_copy(g.Y, GY);
  to_mont(g.X, g.X);
  to_mont(g.Y, g.Y);
  fe_zero(g.Z);
  g.Z[0] = 1;
  to_mont(g.Z, g.Z);

  point_zero(&acc);
  for (int i = 255; i >= 0; i--) {
    point_double(&acc, &acc);
    point_add(&sum, &acc, &g);
    point_cmov(&acc, &sum, (k[i >> 5] >> (i & 31)) & 1u);
  }
  *out = acc;
}

// Jacobian to affine: x = X/Z^2, y = Y/Z^3.
static void point_affine(const Point *p, fe x, fe y) {
  fe zinv, t;
  fe_inv(zinv, p->Z);
  fe_sqr(t, zinv);
  fe_mul(x, p->X, t);
  fe_mul(t, t, zinv);
  fe_mul(y, p->Y, t);
  from_mont(x, x);
  from_mont(y, y);
}

// ---- serialisation -------------------------------------------------------

static void fe_from_be(fe r, const u8 in[32]) {
  for (int i = 0; i < L; i++) {
    int off = 32 - 4 * (i + 1);
    r[i] = ((u32)in[off] << 24) | ((u32)in[off + 1] << 16)
         | ((u32)in[off + 2] << 8) | (u32)in[off + 3];
  }
}

static void fe_to_be(const fe a, u8 out[32]) {
  for (int i = 0; i < L; i++) {
    int off = 32 - 4 * (i + 1);
    out[off] = (u8)(a[i] >> 24);
    out[off + 1] = (u8)(a[i] >> 16);
    out[off + 2] = (u8)(a[i] >> 8);
    out[off + 3] = (u8)a[i];
  }
}

// ---- public entry points -------------------------------------------------

int p256_scalar_mult_base(const u8 d_be[32], u8 x_be[32], u8 y_be[32]) {
  init_constants();
  fe d;
  fe_from_be(d, d_be);
  if (fe_is_zero(d) || fe_cmp(d, N) >= 0) return 0;
  Point q;
  scalar_mult_g(&q, d);
  if (point_is_zero(&q)) return 0;
  fe x, y;
  point_affine(&q, x, y);
  fe_to_be(x, x_be);
  fe_to_be(y, y_be);
  return 1;
}

// y^2 == x^3 - 3x + b, with everything in Montgomery form. Used as a self
// check on the hardcoded curve constants and on any point produced here.
int p256_point_on_curve(const u8 x_be[32], const u8 y_be[32]) {
  init_constants();
  fe x, y, lhs, rhs, t, three;
  fe_from_be(x, x_be);
  fe_from_be(y, y_be);
  to_mont(x, x);
  to_mont(y, y);

  fe_sqr(lhs, y);
  fe_sqr(t, x);
  fe_mul(rhs, t, x);            // x^3
  fe_zero(three);
  three[0] = 3;
  to_mont(three, three);
  fe_mul(t, three, x);          // 3x
  fe_sub(rhs, rhs, t);
  fe bm;
  fe_copy(bm, B);
  to_mont(bm, bm);
  fe_add(rhs, rhs, bm);
  return fe_cmp(lhs, rhs) == 0;
}

// Pick the first candidate scalar in [1, n-1]. The host hands over several so
// this is rejection sampling, not a reduction: reducing a uniform 256-bit
// value mod n biases the low scalars, and while the bias is far too small to
// matter here, rejection costs nothing when n is this close to 2^256.
int p256_keygen(const u8 *candidates, int count, u8 d_be[32], u8 x_be[32], u8 y_be[32]) {
  init_constants();
  for (int i = 0; i < count; i++) {
    const u8 *cand = candidates + i * 32;
    fe d;
    fe_from_be(d, cand);
    if (fe_is_zero(d) || fe_cmp(d, N) >= 0) continue;
    Point q;
    scalar_mult_g(&q, d);
    if (point_is_zero(&q)) continue;
    fe x, y;
    point_affine(&q, x, y);
    mem_copy(d_be, cand, 32);
    fe_to_be(x, x_be);
    fe_to_be(y, y_be);
    return 1;
  }
  return 0;
}

// ---- DER -----------------------------------------------------------------
//
// Written as the exact byte layout rather than a general encoder: every length
// here is fixed, and WebCrypto validates the result on import, which is the
// test that matters.

// SEQUENCE { SEQUENCE { OID ecPublicKey, OID prime256v1 }, BIT STRING }
static const u8 SPKI_PREFIX[] = {
  0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
  0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04,
};

int p256_spki(const u8 x_be[32], const u8 y_be[32], u8 out[P256_SPKI_LEN]) {
  usize n = sizeof(SPKI_PREFIX);
  mem_copy(out, SPKI_PREFIX, n);
  mem_copy(out + n, x_be, 32);
  mem_copy(out + n + 32, y_be, 32);
  return P256_SPKI_LEN;  // 27 + 64 == 91
}

// PrivateKeyInfo { version, AlgorithmIdentifier, OCTET STRING { ECPrivateKey } }
static const u8 PKCS8_PREFIX[] = {
  0x30, 0x81, 0x87, 0x02, 0x01, 0x00,
  0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
  0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07,
  0x04, 0x6d, 0x30, 0x6b, 0x02, 0x01, 0x01, 0x04, 0x20,
};
static const u8 PKCS8_MID[] = { 0xa1, 0x44, 0x03, 0x42, 0x00, 0x04 };

int p256_pkcs8(const u8 d_be[32], const u8 x_be[32], const u8 y_be[32],
               u8 out[P256_PKCS8_LEN]) {
  usize n = sizeof(PKCS8_PREFIX);
  mem_copy(out, PKCS8_PREFIX, n);
  mem_copy(out + n, d_be, 32);
  n += 32;
  mem_copy(out + n, PKCS8_MID, sizeof(PKCS8_MID));
  n += sizeof(PKCS8_MID);
  mem_copy(out + n, x_be, 32);
  mem_copy(out + n + 32, y_be, 32);
  return P256_PKCS8_LEN;  // 35 + 32 + 6 + 64 == 137 + 1 header byte == 138
}
