// Freestanding helpers. There is no libc in this build: no malloc, no
// string.h, no printf. Everything the other modules need lives here.
#ifndef BASE_H
#define BASE_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;
typedef unsigned long usize;  // size_t on wasm32

#define NULL ((void *)0)

void *mem_copy(void *dst, const void *src, usize n);
void *mem_set(void *dst, int v, usize n);
int mem_cmp(const void *a, const void *b, usize n);
// Constant time in the length of the buffer: no early exit on the first
// differing byte. Used for the GCM tag, where an early exit leaks where the
// forgery failed.
int mem_equal_ct(const void *a, const void *b, usize n);
usize str_len(const char *s);
int str_eq(const char *a, const char *b);

// ---- arena ---------------------------------------------------------------
//
// One bump allocator for everything. The host resets it between operations,
// which is the whole memory management story: nothing is ever freed
// individually, and no operation outlives the next reset.
void arena_reset(void);
void *arena_alloc(usize n);
usize arena_used(void);

// ---- growable string -----------------------------------------------------
//
// Appends into the arena. Because the arena is a bump allocator, a builder
// only stays contiguous while nothing else allocates during its lifetime —
// so a builder is always used to completion before the next one starts.
typedef struct {
  char *buf;
  usize len;
  usize cap;
} Builder;

void b_init(Builder *b, usize cap);
void b_char(Builder *b, char c);
void b_str(Builder *b, const char *s);
void b_bytes(Builder *b, const char *s, usize n);
void b_u32(Builder *b, u32 v);
// JSON string body, quotes included, with the escapes JSON demands.
void b_json_str(Builder *b, const char *s);
char *b_done(Builder *b);

// ---- hex -----------------------------------------------------------------
void hex_encode(Builder *b, const u8 *bytes, usize n);
// Returns the byte count, or -1 when the text is not clean hex.
int hex_decode(const char *hex, usize hexlen, u8 *out, usize out_cap);

#endif
