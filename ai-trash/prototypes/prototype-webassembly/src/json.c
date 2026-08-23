// Just enough JSON for the vault: arrays, flat objects, strings, integers,
// true/false/null. Everything lands in the arena, so nothing is freed.
//
// The pasted vault comes from a human with a clipboard, so this has to reject
// malformed input cleanly rather than wander off the end of the buffer.
#include "json.h"
#include "base.h"

typedef struct {
  const char *p;
  const char *end;
  int ok;
} Cur;

static void skip_ws(Cur *c) {
  while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static JVal *jval_new(int type) {
  JVal *v = (JVal *)arena_alloc(sizeof(JVal));
  if (!v) return NULL;
  v->type = type;
  v->num = 0;
  v->str = (char *)"";
  v->key = (char *)"";
  v->first = NULL;
  v->next = NULL;
  return v;
}

static int hex4(const char *p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = v * 16 + d;
  }
  return v;
}

static char *parse_string(Cur *c) {
  if (c->p >= c->end || *c->p != '"') { c->ok = 0; return NULL; }
  c->p++;
  Builder b;
  b_init(&b, 64);
  while (c->p < c->end && *c->p != '"') {
    char ch = *c->p;
    if (ch == '\\') {
      c->p++;
      if (c->p >= c->end) { c->ok = 0; return NULL; }
      switch (*c->p) {
        case 'n': b_char(&b, '\n'); break;
        case 't': b_char(&b, '\t'); break;
        case 'r': b_char(&b, '\r'); break;
        case 'b': b_char(&b, '\b'); break;
        case 'f': b_char(&b, '\f'); break;
        case '/': b_char(&b, '/'); break;
        case '"': b_char(&b, '"'); break;
        case '\\': b_char(&b, '\\'); break;
        case 'u': {
          if (c->end - c->p < 5) { c->ok = 0; return NULL; }
          int cp = hex4(c->p + 1);
          if (cp < 0) { c->ok = 0; return NULL; }
          c->p += 4;
          // Enough of UTF-8 to carry anything that is not a surrogate pair;
          // nothing this app writes uses \u at all.
          if (cp < 0x80) {
            b_char(&b, (char)cp);
          } else if (cp < 0x800) {
            b_char(&b, (char)(0xc0 | (cp >> 6)));
            b_char(&b, (char)(0x80 | (cp & 0x3f)));
          } else {
            b_char(&b, (char)(0xe0 | (cp >> 12)));
            b_char(&b, (char)(0x80 | ((cp >> 6) & 0x3f)));
            b_char(&b, (char)(0x80 | (cp & 0x3f)));
          }
          break;
        }
        default: c->ok = 0; return NULL;
      }
      c->p++;
    } else {
      b_char(&b, ch);
      c->p++;
    }
  }
  if (c->p >= c->end) { c->ok = 0; return NULL; }
  c->p++;  // closing quote
  return b_done(&b);
}

static JVal *parse_value(Cur *c);

static JVal *parse_array(Cur *c) {
  JVal *arr = jval_new(J_ARR);
  c->p++;  // [
  skip_ws(c);
  if (c->p < c->end && *c->p == ']') { c->p++; return arr; }
  JVal *tail = NULL;
  for (;;) {
    JVal *item = parse_value(c);
    if (!c->ok) return NULL;
    if (tail) tail->next = item; else arr->first = item;
    tail = item;
    skip_ws(c);
    if (c->p < c->end && *c->p == ',') { c->p++; skip_ws(c); continue; }
    if (c->p < c->end && *c->p == ']') { c->p++; return arr; }
    c->ok = 0;
    return NULL;
  }
}

static JVal *parse_object(Cur *c) {
  JVal *obj = jval_new(J_OBJ);
  c->p++;  // {
  skip_ws(c);
  if (c->p < c->end && *c->p == '}') { c->p++; return obj; }
  JVal *tail = NULL;
  for (;;) {
    skip_ws(c);
    char *key = parse_string(c);
    if (!c->ok) return NULL;
    skip_ws(c);
    if (c->p >= c->end || *c->p != ':') { c->ok = 0; return NULL; }
    c->p++;
    JVal *val = parse_value(c);
    if (!c->ok) return NULL;
    val->key = key;
    if (tail) tail->next = val; else obj->first = val;
    tail = val;
    skip_ws(c);
    if (c->p < c->end && *c->p == ',') { c->p++; continue; }
    if (c->p < c->end && *c->p == '}') { c->p++; return obj; }
    c->ok = 0;
    return NULL;
  }
}

static JVal *parse_value(Cur *c) {
  skip_ws(c);
  if (c->p >= c->end) { c->ok = 0; return NULL; }
  char ch = *c->p;
  if (ch == '{') return parse_object(c);
  if (ch == '[') return parse_array(c);
  if (ch == '"') {
    JVal *v = jval_new(J_STR);
    v->str = parse_string(c);
    if (!c->ok) return NULL;
    return v;
  }
  if (ch == 't' && c->end - c->p >= 4 && c->p[1] == 'r' && c->p[2] == 'u' && c->p[3] == 'e') {
    c->p += 4;
    JVal *v = jval_new(J_BOOL);
    v->num = 1;
    return v;
  }
  if (ch == 'f' && c->end - c->p >= 5 && c->p[1] == 'a') {
    c->p += 5;
    return jval_new(J_BOOL);
  }
  if (ch == 'n' && c->end - c->p >= 4 && c->p[1] == 'u') {
    c->p += 4;
    return jval_new(J_NULL);
  }
  if (ch == '-' || (ch >= '0' && ch <= '9')) {
    long long sign = 1;
    if (ch == '-') { sign = -1; c->p++; }
    long long v = 0;
    int digits = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
      v = v * 10 + (*c->p - '0');
      c->p++;
      digits++;
      if (digits > 18) { c->ok = 0; return NULL; }
    }
    if (!digits) { c->ok = 0; return NULL; }
    // A fraction or exponent is valid JSON but never appears in this format;
    // consume and ignore the digits so the parse does not stall.
    if (c->p < c->end && (*c->p == '.' || *c->p == 'e' || *c->p == 'E')) {
      c->p++;
      while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '+' || *c->p == '-')) c->p++;
    }
    JVal *n = jval_new(J_NUM);
    n->num = sign * v;
    return n;
  }
  c->ok = 0;
  return NULL;
}

JVal *json_parse(const char *text, usize len) {
  Cur c = { text, text + len, 1 };
  JVal *v = parse_value(&c);
  if (!c.ok || !v) return NULL;
  skip_ws(&c);
  if (c.p != c.end) return NULL;  // trailing junk is a bad paste, not a value
  return v;
}

JVal *json_get(const JVal *obj, const char *key) {
  if (!obj || obj->type != J_OBJ) return NULL;
  for (JVal *m = obj->first; m; m = m->next) {
    if (str_eq(m->key, key)) return m;
  }
  return NULL;
}

const char *json_str(const JVal *obj, const char *key, const char *fallback) {
  JVal *v = json_get(obj, key);
  return (v && v->type == J_STR) ? v->str : fallback;
}

long long json_num(const JVal *obj, const char *key, long long fallback) {
  JVal *v = json_get(obj, key);
  return (v && v->type == J_NUM) ? v->num : fallback;
}

int json_count(const JVal *arr) {
  int n = 0;
  if (!arr) return 0;
  for (JVal *m = arr->first; m; m = m->next) n++;
  return n;
}
