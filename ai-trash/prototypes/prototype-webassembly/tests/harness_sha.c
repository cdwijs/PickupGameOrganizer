// Test harness: exports the primitives so node can drive them directly.
#include "base.h"
#include "sha256.h"
static u8 scratch[8192];
__attribute__((export_name("buf"))) u8 *buf(void) { return scratch; }
__attribute__((export_name("t_sha256")))
void t_sha256(int len) { u8 d[32]; sha256(scratch, (usize)len, d); mem_copy(scratch + 4096, d, 32); }
__attribute__((export_name("t_hmac")))
void t_hmac(int klen, int mlen) {
  u8 d[32]; hmac_sha256(scratch, (usize)klen, scratch + 1024, (usize)mlen, d);
  mem_copy(scratch + 4096, d, 32);
}
__attribute__((export_name("t_pbkdf2")))
void t_pbkdf2(int pwlen, int saltlen, int iters) {
  u8 d[32]; pbkdf2_sha256(scratch, (usize)pwlen, scratch + 1024, (usize)saltlen, (u32)iters, d);
  mem_copy(scratch + 4096, d, 32);
}
