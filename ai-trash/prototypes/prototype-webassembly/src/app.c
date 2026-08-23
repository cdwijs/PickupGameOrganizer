// The exported surface. Everything the JS shell asks the module to do lands
// here: strings in, strings out, through the arena.
//
// The whole account model lives on this side — the plaintext format, the
// [group1] section, the vault and its merge rules, and the "try every blob"
// sign-in. JavaScript only carries the results to and from the DOM and
// localStorage.
#include "base.h"
#include "sha256.h"
#include "aesgcm.h"
#include "p256.h"
#include "json.h"
#include "roster.h"

// Same as prototype-minimal, byte for byte: the two prototypes can read each
// other's vaults.
#define KDF_NAME "PBKDF2-SHA256"
#define KDF_ITERATIONS 310000u
#define SALT_BYTES 16
#define NONCE_BYTES 12
#define TAG_BYTES 16
#define READABLE_PREFIX "Readable: "
#define USER_DATA_BODY "This is a placeholder for the user data"
#define GROUP_SECTION "group1"
#define GROUP_KEY_LABEL "ECDSA P-256"

// ---- memory --------------------------------------------------------------

__attribute__((export_name("wasm_alloc")))
u8 *wasm_alloc(int n) { return (u8 *)arena_alloc((usize)n + 1); }

__attribute__((export_name("wasm_reset")))
void wasm_reset(void) { arena_reset(); }

__attribute__((export_name("wasm_used")))
int wasm_used(void) { return (int)arena_used(); }

// ---- plaintext -----------------------------------------------------------

// "Readable: <user>\n<body>\n\n[group1]\nalg: …\npublic: …\nprivate: …"
static char *make_plaintext(const char *username, const u8 *spki, const u8 *pkcs8) {
  Builder b;
  b_init(&b, 1024);
  b_str(&b, READABLE_PREFIX);
  b_str(&b, username);
  b_char(&b, '\n');
  b_str(&b, USER_DATA_BODY);
  b_str(&b, "\n\n[" GROUP_SECTION "]\nalg: " GROUP_KEY_LABEL "\npublic: ");
  hex_encode(&b, spki, P256_SPKI_LEN);
  b_str(&b, "\nprivate: ");
  hex_encode(&b, pkcs8, P256_PKCS8_LEN);
  return b_done(&b);
}

// The username on the marker line, or NULL when this is not one of ours —
// which is how a wrong password is told apart from a right one, since AES-GCM
// has already rejected the truly wrong keys by then.
static char *plaintext_username(const char *text) {
  const char *head = READABLE_PREFIX;
  usize hl = str_len(head);
  for (usize i = 0; i < hl; i++) {
    if (text[i] != head[i]) return NULL;
  }
  usize end = hl;
  while (text[end] && text[end] != '\n') end++;
  usize start = hl;
  while (start < end && text[start] == ' ') start++;
  usize stop = end;
  while (stop > start && (text[stop - 1] == ' ' || text[stop - 1] == '\r')) stop--;
  if (stop == start) return NULL;
  char *out = (char *)arena_alloc(stop - start + 1);
  if (!out) return NULL;
  mem_copy(out, text + start, stop - start);
  out[stop - start] = 0;
  return out;
}

__attribute__((export_name("plaintext_user")))
const char *export_plaintext_user(const char *text) {
  char *u = plaintext_username(text);
  return u ? u : "";
}

// ---- records -------------------------------------------------------------

typedef struct {
  char id[68];
  u8 salt[SALT_BYTES];
  u8 data[8192];
  usize data_len;
} Record;

static int hex_id(const char *s) {
  usize n = str_len(s);
  if (n < 4 || n > 64) return 0;
  for (usize i = 0; i < n; i++) {
    char c = s[i];
    int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return 0;
  }
  return 1;
}

static void id_from_random(char *dst, const u8 *rnd) {
  static const char HEXD[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) {
    dst[i * 2] = HEXD[rnd[i] >> 4];
    dst[i * 2 + 1] = HEXD[rnd[i] & 15];
  }
  dst[16] = 0;
}

// One entry of the stored array. Anything unparseable is dropped rather than
// thrown, so a single bad record cannot lock the app out.
static int record_from_json(const JVal *o, Record *r) {
  if (!o || o->type != J_OBJ) return 0;
  const char *salt = json_str(o, "salt", NULL);
  const char *data = json_str(o, "data", NULL);
  if (!salt) salt = json_str(o, "saltHex", NULL);
  if (!data) data = json_str(o, "dataHex", NULL);
  if (!salt || !data) return 0;

  const char *kdf = json_str(o, "kdf", NULL);
  if (kdf && !str_eq(kdf, KDF_NAME)) return -1;              // wrong KDF
  long long iters = json_num(o, "iterations", KDF_ITERATIONS);
  if (iters != (long long)KDF_ITERATIONS) return -2;         // wrong iterations

  if (hex_decode(salt, str_len(salt), r->salt, SALT_BYTES) != SALT_BYTES) return -3;
  int n = hex_decode(data, str_len(data), r->data, sizeof(r->data));
  if (n < NONCE_BYTES + TAG_BYTES) return -4;
  r->data_len = (usize)n;

  const char *id = json_str(o, "id", "");
  if (hex_id(id)) {
    usize l = str_len(id);
    for (usize i = 0; i < l; i++) {
      char c = id[i];
      r->id[i] = (c >= 'A' && c <= 'F') ? (char)(c + 32) : c;
    }
    r->id[l] = 0;
  } else {
    r->id[0] = 0;  // caller assigns one
  }
  return 1;
}

static void record_to_json(Builder *b, const Record *r, int pretty) {
  const char *nl = pretty ? "\n    " : "";
  b_str(b, pretty ? "  {" : "{");
  b_str(b, nl);
  b_str(b, "\"v\": 2,");
  b_str(b, pretty ? nl : " ");
  b_str(b, "\"kdf\": \"" KDF_NAME "\",");
  b_str(b, pretty ? nl : " ");
  b_str(b, "\"iterations\": ");
  b_u32(b, KDF_ITERATIONS);
  b_str(b, ",");
  b_str(b, pretty ? nl : " ");
  b_str(b, "\"id\": ");
  b_json_str(b, r->id);
  b_str(b, ",");
  b_str(b, pretty ? nl : " ");
  b_str(b, "\"salt\": \"");
  hex_encode(b, r->salt, SALT_BYTES);
  b_str(b, "\",");
  b_str(b, pretty ? nl : " ");
  b_str(b, "\"data\": \"");
  hex_encode(b, r->data, r->data_len);
  b_str(b, "\"");
  b_str(b, pretty ? "\n  }" : "}");
}

// The vault as stored and as shown are the same shape; only the whitespace
// differs, exactly as in the JS.
static char *records_to_text(const Record *recs, int n, int pretty) {
  Builder b;
  b_init(&b, (usize)n * 1024 + 64);
  b_str(&b, pretty ? "[\n" : "[");
  for (int i = 0; i < n; i++) {
    if (i) b_str(&b, pretty ? ",\n" : ",");
    record_to_json(&b, &recs[i], pretty);
  }
  b_str(&b, pretty ? "\n]" : "]");
  return b_done(&b);
}

#define MAX_RECORDS 128
static Record recs[MAX_RECORDS];
static Record pasted[MAX_RECORDS];

// Load the stored vault. Bad entries are skipped silently, which is what
// loadUsers() does.
static int load_vault(const char *json) {
  if (!json || !json[0]) return 0;
  JVal *root = json_parse(json, str_len(json));
  if (!root || root->type != J_ARR) return 0;
  int n = 0;
  for (JVal *m = root->first; m && n < MAX_RECORDS; m = m->next) {
    if (record_from_json(m, &recs[n]) == 1 && recs[n].id[0]) n++;
  }
  return n;
}

// ---- account -------------------------------------------------------------

// Random layout the host must supply: 8 id + 16 salt + 12 nonce + 8*32 scalar
// candidates.
#define RND_ID 0
#define RND_SALT 8
#define RND_NONCE 24
#define RND_SCALARS 36
#define RND_NEEDED (RND_SCALARS + 32 * 8)

__attribute__((export_name("rnd_needed")))
int rnd_needed(void) { return RND_NEEDED; }

static char *error_json(const char *msg) {
  Builder b;
  b_init(&b, 128);
  b_str(&b, "{\"error\":");
  b_json_str(&b, msg);
  b_char(&b, '}');
  return b_done(&b);
}

// Create an account: fresh keypair, fresh salt, PBKDF2 from the password, and
// the whole plaintext sealed under AES-GCM. Returns the new vault alongside
// the plaintext, because the keypair is random and rebuilding the text later
// would invent a different one.
__attribute__((export_name("account_create")))
const char *account_create(const char *vault_json, const char *username,
                           const char *password, const u8 *rnd) {
  int n = load_vault(vault_json);
  if (n >= MAX_RECORDS) return error_json("the vault is full");

  u8 d[32], x[32], y[32];
  if (!p256_keygen(rnd + RND_SCALARS, 8, d, x, y)) return error_json("key generation failed");
  if (!p256_point_on_curve(x, y)) return error_json("generated an off-curve point");
  u8 spki[P256_SPKI_LEN], pkcs8[P256_PKCS8_LEN];
  p256_spki(x, y, spki);
  p256_pkcs8(d, x, y, pkcs8);

  char *text = make_plaintext(username, spki, pkcs8);
  usize tlen = str_len(text);

  Record *r = &recs[n];
  mem_copy(r->salt, rnd + RND_SALT, SALT_BYTES);
  id_from_random(r->id, rnd + RND_ID);

  u8 key[32];
  pbkdf2_sha256((const u8 *)password, str_len(password), r->salt, SALT_BYTES,
                KDF_ITERATIONS, key);

  if (tlen + NONCE_BYTES + TAG_BYTES > sizeof(r->data)) return error_json("plaintext too large");
  mem_copy(r->data, rnd + RND_NONCE, NONCE_BYTES);
  aes256_gcm_encrypt(key, r->data, NULL, 0, (const u8 *)text, tlen,
                     r->data + NONCE_BYTES, r->data + NONCE_BYTES + tlen);
  r->data_len = NONCE_BYTES + tlen + TAG_BYTES;
  n++;

  char *vault = records_to_text(recs, n, 0);
  Builder b;
  b_init(&b, tlen + str_len(vault) + 256);
  b_str(&b, "{\"vault\":");
  b_json_str(&b, vault);
  b_str(&b, ",\"id\":");
  b_json_str(&b, r->id);
  b_str(&b, ",\"username\":");
  b_json_str(&b, username);
  b_str(&b, ",\"text\":");
  b_json_str(&b, text);
  b_char(&b, '}');
  return b_done(&b);
}

// Open one blob. Returns the plaintext, or NULL when the tag fails or the
// plaintext is not one of ours.
static char *unlock_record(const Record *r, const char *password) {
  u8 key[32];
  pbkdf2_sha256((const u8 *)password, str_len(password), r->salt, SALT_BYTES,
                KDF_ITERATIONS, key);
  usize ctlen = r->data_len - NONCE_BYTES - TAG_BYTES;
  u8 *pt = (u8 *)arena_alloc(ctlen + 1);
  if (!pt) return NULL;
  if (!aes256_gcm_decrypt(key, r->data, NULL, 0,
                          r->data + NONCE_BYTES, ctlen,
                          r->data + NONCE_BYTES + ctlen, pt)) {
    return NULL;
  }
  pt[ctlen] = 0;
  return plaintext_username((const char *)pt) ? (char *)pt : NULL;
}

static char *hit_json(const Record *r, const char *text) {
  char *user = plaintext_username(text);
  Builder b;
  b_init(&b, str_len(text) + 256);
  b_str(&b, "{\"id\":");
  b_json_str(&b, r->id);
  b_str(&b, ",\"username\":");
  b_json_str(&b, user ? user : "");
  b_str(&b, ",\"text\":");
  b_json_str(&b, text);
  b_char(&b, '}');
  return b_done(&b);
}

// Sign in: try every blob with this password and keep the one whose encrypted
// username matches. One key derivation per stored blob, no shortcut — the
// username is inside the ciphertext, so there is nothing public to look up on.
__attribute__((export_name("account_signin")))
const char *account_signin(const char *vault_json, const char *username, const char *password) {
  int n = load_vault(vault_json);
  for (int i = 0; i < n; i++) {
    char *text = unlock_record(&recs[i], password);
    if (!text) continue;
    char *user = plaintext_username(text);
    if (!user) continue;
    // Case-insensitive, and the stored spelling is the one that wins.
    usize a = 0;
    int same = 1;
    for (;; a++) {
      char c1 = user[a], c2 = username[a];
      char l1 = (c1 >= 'A' && c1 <= 'Z') ? (char)(c1 + 32) : c1;
      char l2 = (c2 >= 'A' && c2 <= 'Z') ? (char)(c2 + 32) : c2;
      if (l1 != l2) { same = 0; break; }
      if (!c1) break;
    }
    if (same) return hit_json(&recs[i], text);
  }
  return error_json("nomatch");
}

// Unlock one named blob, for a session that knows who it is but has no key.
__attribute__((export_name("account_unlock")))
const char *account_unlock(const char *vault_json, const char *id, const char *password) {
  int n = load_vault(vault_json);
  for (int i = 0; i < n; i++) {
    if (!str_eq(recs[i].id, id)) continue;
    char *text = unlock_record(&recs[i], password);
    if (!text) return error_json("badpassword");
    return hit_json(&recs[i], text);
  }
  return error_json("gone");
}

// ---- vault ---------------------------------------------------------------

__attribute__((export_name("vault_count")))
int vault_count(const char *vault_json) { return load_vault(vault_json); }

__attribute__((export_name("vault_has")))
int vault_has(const char *vault_json, const char *id) {
  int n = load_vault(vault_json);
  for (int i = 0; i < n; i++) {
    if (str_eq(recs[i].id, id)) return 1;
  }
  return 0;
}

__attribute__((export_name("vault_text")))
const char *vault_text(const char *vault_json) {
  int n = load_vault(vault_json);
  if (!n) return "";
  return records_to_text(recs, n, 1);
}

__attribute__((export_name("vault_delete")))
const char *vault_delete(const char *vault_json, const char *id) {
  int n = load_vault(vault_json);
  int out = 0;
  for (int i = 0; i < n; i++) {
    if (str_eq(recs[i].id, id)) continue;
    if (out != i) recs[out] = recs[i];
    out++;
  }
  return records_to_text(recs, out, 0);
}

// Merge pasted records: the same id replaces (the same account, re-encrypted),
// an identical salt+blob pair is a no-op, anything else is added.
__attribute__((export_name("vault_ingest")))
const char *vault_ingest(const char *vault_json, const char *text, const u8 *rnd, int rnd_len) {
  usize tlen = str_len(text);
  usize start = 0;
  while (start < tlen && (text[start] == ' ' || text[start] == '\n'
                          || text[start] == '\t' || text[start] == '\r')) start++;
  if (start >= tlen) return error_json("nothing pasted");

  JVal *root = json_parse(text + start, tlen - start);
  if (!root) return error_json("not valid JSON");

  int pn = 0;
  if (root->type == J_ARR) {
    for (JVal *m = root->first; m && pn < MAX_RECORDS; m = m->next) {
      int rc = record_from_json(m, &pasted[pn]);
      if (rc == -1) return error_json("unsupported kdf");
      if (rc == -2) return error_json("unsupported iteration count");
      if (rc == -3) return error_json("salt must be 16 bytes");
      if (rc == -4) return error_json("encrypted data is too short");
      if (rc != 1) return error_json("not a record object");
      pn++;
    }
    if (!pn) return error_json("no records in there");
  } else if (root->type == J_OBJ) {
    int rc = record_from_json(root, &pasted[0]);
    if (rc == -1) return error_json("unsupported kdf");
    if (rc == -2) return error_json("unsupported iteration count");
    if (rc == -3) return error_json("salt must be 16 bytes");
    if (rc == -4) return error_json("encrypted data is too short");
    if (rc != 1) return error_json("not a record object");
    pn = 1;
  } else {
    return error_json("not a record object");
  }

  int n = load_vault(vault_json);
  int added = 0, replaced = 0, rnd_at = 0;

  for (int i = 0; i < pn; i++) {
    Record *p = &pasted[i];
    if (!p->id[0]) {
      // No usable id in the paste: mint one from the host's randomness.
      if (rnd_at + 8 > rnd_len) return error_json("not enough randomness");
      id_from_random(p->id, rnd + rnd_at);
      rnd_at += 8;
    }
    int found = -1;
    for (int j = 0; j < n; j++) {
      if (str_eq(recs[j].id, p->id)) { found = j; break; }
    }
    if (found >= 0) {
      int same = recs[found].data_len == p->data_len
        && mem_cmp(recs[found].salt, p->salt, SALT_BYTES) == 0
        && mem_cmp(recs[found].data, p->data, p->data_len) == 0;
      if (same) continue;  // identical pair, nothing to do
      recs[found] = *p;
      replaced++;
      continue;
    }
    if (n >= MAX_RECORDS) return error_json("the vault is full");
    recs[n++] = *p;
    added++;
  }

  char *vault = records_to_text(recs, n, 0);
  Builder b;
  b_init(&b, str_len(vault) + 128);
  b_str(&b, "{\"vault\":");
  b_json_str(&b, vault);
  b_str(&b, ",\"added\":");
  b_u32(&b, (u32)added);
  b_str(&b, ",\"replaced\":");
  b_u32(&b, (u32)replaced);
  b_str(&b, ",\"total\":");
  b_u32(&b, (u32)n);
  b_char(&b, '}');
  return b_done(&b);
}

// ---- roster --------------------------------------------------------------

__attribute__((export_name("roster_parse")))
int export_roster_parse(const char *text, const char *username) {
  return roster_load(text, username);
}

__attribute__((export_name("roster_info")))
const char *export_roster_info(int idx) {
  Builder b;
  b_init(&b, 256);
  b_str(&b, "{\"date\":");
  b_json_str(&b, roster_date(idx));
  b_str(&b, ",\"weekday\":");
  b_json_str(&b, roster_weekday(idx));
  b_str(&b, ",\"time\":");
  b_json_str(&b, roster_time());
  b_str(&b, ",\"count\":");
  b_u32(&b, (u32)roster_filled(idx));
  b_str(&b, ",\"going\":");
  b_str(&b, roster_going(idx) ? "true" : "false");
  b_char(&b, '}');
  return b_done(&b);
}

__attribute__((export_name("roster_flip")))
void export_roster_flip(int idx) { roster_toggle(idx); }

__attribute__((export_name("roster_out")))
const char *export_roster_out(void) { return roster_text(); }

__attribute__((export_name("display_name")))
const char *export_display_name(const char *username) {
  char *out = (char *)arena_alloc(128);
  if (!out) return "";
  roster_display_name(out, 128, username);
  return out;
}
