#include "base.h"
#include "aesgcm.h"
static u8 s[16384];
__attribute__((export_name("buf"))) u8 *buf(void) { return s; }
// layout: 0 key(32) | 64 iv(12) | 128 aad | 1024 pt/ct | 8192 out | 12288 tag
__attribute__((export_name("t_block")))
void t_block(void) { u8 rk[AES_RK_BYTES]; aes256_expand(s, rk); aes256_encrypt_block(rk, s + 64, s + 8192); }
__attribute__((export_name("t_enc")))
void t_enc(int aadlen, int ptlen) {
  aes256_gcm_encrypt(s, s + 64, s + 128, (usize)aadlen, s + 1024, (usize)ptlen, s + 8192, s + 12288);
}
__attribute__((export_name("t_dec")))
int t_dec(int aadlen, int ctlen) {
  return aes256_gcm_decrypt(s, s + 64, s + 128, (usize)aadlen, s + 1024, (usize)ctlen, s + 12288, s + 8192);
}
