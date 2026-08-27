#include "base.h"

void *mem_copy(void *dst, const void *src, usize n) {
  u8 *d = (u8 *)dst;
  const u8 *s = (const u8 *)src;
  for (usize i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void *mem_set(void *dst, int v, usize n) {
  u8 *d = (u8 *)dst;
  for (usize i = 0; i < n; i++) d[i] = (u8)v;
  return dst;
}

int mem_cmp(const void *a, const void *b, usize n) {
  const u8 *x = (const u8 *)a;
  const u8 *y = (const u8 *)b;
  for (usize i = 0; i < n; i++) {
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  }
  return 0;
}

int mem_equal_ct(const void *a, const void *b, usize n) {
  const u8 *x = (const u8 *)a;
  const u8 *y = (const u8 *)b;
  u8 diff = 0;
  for (usize i = 0; i < n; i++) diff |= (u8)(x[i] ^ y[i]);
  return diff == 0;
}

usize str_len(const char *s) {
  usize n = 0;
  while (s[n]) n++;
  return n;
}

int str_eq(const char *a, const char *b) {
  usize i = 0;
  while (a[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

// clang lowers struct copies and array initialisers to these, so a freestanding
// build has to provide them even though nothing here calls them by name. A
// hosted build has them already — prototype-minimal-qt compiles these sources
// against a libc and defines PROTO_HOSTED so they are not defined twice.
#ifndef PROTO_HOSTED

void *memcpy(void *d, const void *s, usize n) { return mem_copy(d, s, n); }
void *memset(void *d, int v, usize n) { return mem_set(d, v, n); }
void *memmove(void *dst, const void *src, usize n) {
  u8 *d = (u8 *)dst;
  const u8 *s = (const u8 *)src;
  if (d == s || n == 0) return dst;
  if (d < s) {
    for (usize i = 0; i < n; i++) d[i] = s[i];
  } else {
    for (usize i = n; i > 0; i--) d[i - 1] = s[i - 1];
  }
  return dst;
}

#endif  // PROTO_HOSTED

// ---- arena ---------------------------------------------------------------
//
// 24 MB. The largest thing that passes through is a pasted vault plus its
// decoded bytes; the roster text is a few kilobytes. wasm memory grows in
// 64 KiB pages and this is reserved as one static block so there is no
// grow-and-relocate to coordinate with the host's views into memory.
#define ARENA_SIZE (24u * 1024u * 1024u)
static u8 arena[ARENA_SIZE];
static usize arena_at = 0;

void arena_reset(void) { arena_at = 0; }
usize arena_used(void) { return arena_at; }

void *arena_alloc(usize n) {
  usize aligned = (n + 7u) & ~7u;
  if (aligned > ARENA_SIZE - arena_at) return NULL;  // caller checks
  void *p = &arena[arena_at];
  arena_at += aligned;
  return p;
}

// ---- builder -------------------------------------------------------------

void b_init(Builder *b, usize cap) {
  if (cap < 64) cap = 64;
  b->buf = (char *)arena_alloc(cap);
  b->len = 0;
  b->cap = b->buf ? cap : 0;
}

// Growing means allocating a fresh block and copying: the arena cannot extend
// in place once anything else has been allocated behind it. Callers size their
// builders generously to keep this rare.
static void b_grow(Builder *b, usize need) {
  if (b->len + need + 1 <= b->cap) return;
  usize cap = b->cap ? b->cap * 2 : 64;
  while (cap < b->len + need + 1) cap *= 2;
  char *next = (char *)arena_alloc(cap);
  if (!next) return;
  mem_copy(next, b->buf, b->len);
  b->buf = next;
  b->cap = cap;
}

void b_char(Builder *b, char c) {
  b_grow(b, 1);
  if (b->len + 1 >= b->cap) return;
  b->buf[b->len++] = c;
}

void b_bytes(Builder *b, const char *s, usize n) {
  b_grow(b, n);
  if (b->len + n >= b->cap) return;
  mem_copy(b->buf + b->len, s, n);
  b->len += n;
}

void b_str(Builder *b, const char *s) { b_bytes(b, s, str_len(s)); }

void b_u32(Builder *b, u32 v) {
  char tmp[12];
  int i = 0;
  if (v == 0) tmp[i++] = '0';
  while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i) b_char(b, tmp[--i]);
}

void b_json_str(Builder *b, const char *s) {
  b_char(b, '"');
  for (usize i = 0; s[i]; i++) {
    u8 c = (u8)s[i];
    switch (c) {
      case '"': b_str(b, "\\\""); break;
      case '\\': b_str(b, "\\\\"); break;
      case '\n': b_str(b, "\\n"); break;
      case '\r': b_str(b, "\\r"); break;
      case '\t': b_str(b, "\\t"); break;
      default:
        if (c < 0x20) {
          // \u00XX — the only control characters JSON will not take raw.
          static const char *hexd = "0123456789abcdef";
          b_str(b, "\\u00");
          b_char(b, hexd[c >> 4]);
          b_char(b, hexd[c & 15]);
        } else {
          // UTF-8 passes through untouched: the emoji in the roster and the
          // usernames are already valid UTF-8 and JSON takes them as they are.
          b_char(b, (char)c);
        }
    }
  }
  b_char(b, '"');
}

char *b_done(Builder *b) {
  if (!b->buf) return (char *)"";
  b->buf[b->len] = 0;
  return b->buf;
}

// ---- hex -----------------------------------------------------------------

static const char HEXD[] = "0123456789abcdef";

void hex_encode(Builder *b, const u8 *bytes, usize n) {
  b_grow(b, n * 2);
  for (usize i = 0; i < n; i++) {
    b_char(b, HEXD[bytes[i] >> 4]);
    b_char(b, HEXD[bytes[i] & 15]);
  }
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int hex_decode(const char *hex, usize hexlen, u8 *out, usize out_cap) {
  if (hexlen % 2) return -1;
  usize n = hexlen / 2;
  if (n > out_cap) return -1;
  for (usize i = 0; i < n; i++) {
    int hi = hex_val(hex[i * 2]);
    int lo = hex_val(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (u8)((hi << 4) | lo);
  }
  return (int)n;
}
