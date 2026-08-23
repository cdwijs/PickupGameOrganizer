#ifndef P256_H
#define P256_H
#include "base.h"

#define P256_SPKI_LEN 91
#define P256_PKCS8_LEN 138

int p256_scalar_mult_base(const u8 d_be[32], u8 x_be[32], u8 y_be[32]);
int p256_point_on_curve(const u8 x_be[32], const u8 y_be[32]);
int p256_keygen(const u8 *candidates, int count, u8 d_be[32], u8 x_be[32], u8 y_be[32]);
int p256_spki(const u8 x_be[32], const u8 y_be[32], u8 out[P256_SPKI_LEN]);
int p256_pkcs8(const u8 d_be[32], const u8 x_be[32], const u8 y_be[32], u8 out[P256_PKCS8_LEN]);
#endif
