#include "base.h"
#include "p256.h"
static u8 s[4096];
__attribute__((export_name("buf"))) u8 *buf(void) { return s; }
// 0: d/candidates | 512: x | 544: y | 1024: der
__attribute__((export_name("t_mult")))  int t_mult(void)  { return p256_scalar_mult_base(s, s + 512, s + 544); }
__attribute__((export_name("t_curve"))) int t_curve(void) { return p256_point_on_curve(s + 512, s + 544); }
__attribute__((export_name("t_keygen"))) int t_keygen(int n) { return p256_keygen(s, n, s + 576, s + 512, s + 544); }
__attribute__((export_name("t_spki")))  int t_spki(void)  { return p256_spki(s + 512, s + 544, s + 1024); }
__attribute__((export_name("t_pkcs8"))) int t_pkcs8(void) { return p256_pkcs8(s + 576, s + 512, s + 544, s + 1024); }
