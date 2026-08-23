// The roster parser and rewriter, ported line for line from prototype-minimal.
//
// The JS version leans on regular expressions; there is no regex here, so each
// pattern is a hand-written scanner. The grammar is small enough that this is
// clearer than it sounds:
//
//   🗓️ Friday 07.08.2026     a date line: emoji, optional weekday, dd.mm.yyyy
//   01. Alice                a player line: number, dot, optional name
//   03.                      an empty slot
//
// Everything else is carried through untouched, so the round trip only rewrites
// the player lines it recognises.
#include "roster.h"
#include "base.h"

#define MAX_LINES 4096
#define MAX_BLOCKS 64
#define MAX_PLAYERS 64

typedef struct {
  const char *ptr;
  usize len;
} Slice;

typedef struct {
  int date_idx;
  char weekday[32];
  char date[16];
  int first_player;   // -1 when the block has no player lines
  int last_player;
  int player_count;
  char players[MAX_PLAYERS][96];
} Block;

static Slice lines[MAX_LINES];
static int line_count;
static Block blocks[MAX_BLOCKS];
static int block_count;
static char cur_user[64];
static char raw_text[131072];
static usize raw_len;

// ---- little helpers ------------------------------------------------------

static int is_space(char c) { return c == ' ' || c == '\t'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive over ASCII, which is what the JS toLowerCase() comparison
// amounts to for the names this handles.
static int eq_ci(const char *a, const char *b) {
  usize i = 0;
  while (a[i] && b[i] && lower(a[i]) == lower(b[i])) i++;
  return a[i] == 0 && b[i] == 0;
}

static void copy_trimmed(char *dst, usize cap, const char *src, usize len) {
  usize start = 0;
  while (start < len && (is_space(src[start]) || src[start] == '\r')) start++;
  usize end = len;
  while (end > start && (is_space(src[end - 1]) || src[end - 1] == '\r')) end--;
  usize n = end - start;
  if (n > cap - 1) n = cap - 1;
  mem_copy(dst, src + start, n);
  dst[n] = 0;
}

// ---- name shaping --------------------------------------------------------

#define APP_SUFFIX " (app)"

// "cedric" -> "Cedric", "jan-piet" -> "Jan-Piet". Each run between spaces,
// apostrophes and hyphens gets its first letter raised and the rest left as
// typed, so "McKay" survives.
//
// The JS calls toUpperCase(), which knows every alphabet. This handles ASCII
// and the Latin-1 letters (é -> É) that actually turn up in names here; any
// other script is left alone. That difference is in the README.
void roster_capitalize(char *dst, usize cap, const char *name) {
  usize o = 0;
  int at_start = 1;
  for (usize i = 0; name[i] && o + 3 < cap; i++) {
    u8 c = (u8)name[i];
    if (c == ' ' || c == '\'' || c == '-') {
      at_start = 1;
      dst[o++] = (char)c;
      continue;
    }
    if (at_start) {
      if (c >= 'a' && c <= 'z') {
        dst[o++] = (char)(c - 32);
      } else if (c == 0xc3 && (u8)name[i + 1] >= 0xa0 && (u8)name[i + 1] <= 0xbe
                 && (u8)name[i + 1] != 0xb7) {
        // Latin-1 supplement: the lower-case block sits 0x20 above the upper.
        dst[o++] = (char)c;
        dst[o++] = (char)((u8)name[++i] - 0x20);
      } else {
        dst[o++] = (char)c;
      }
      at_start = 0;
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
}

void roster_display_name(char *dst, usize cap, const char *username) {
  roster_capitalize(dst, cap, username);
  usize n = str_len(dst);
  const char *suffix = APP_SUFFIX;
  for (usize i = 0; suffix[i] && n + 1 < cap; i++) dst[n++] = suffix[i];
  dst[n] = 0;
}

// A slot belongs to the user when it is the bare name or the name plus the
// "(app)" tag, either way ignoring case.
static int is_same_user(const char *slot, const char *username) {
  if (!username[0]) return 0;
  if (eq_ci(slot, username)) return 1;
  char tagged[96];
  usize n = str_len(username);
  if (n > 80) return 0;
  mem_copy(tagged, username, n);
  const char *suffix = APP_SUFFIX;
  usize m = 0;
  while (suffix[m]) { tagged[n + m] = suffix[m]; m++; }
  tagged[n + m] = 0;
  return eq_ci(slot, tagged);
}

// ---- line patterns -------------------------------------------------------

// 🗓 is F0 9F 97 93, sometimes followed by the variation selector EF B8 8F.
static int match_date_line(const char *s, usize len, char *weekday, char *date) {
  usize i = 0;
  while (i < len && is_space(s[i])) i++;
  if (len - i < 4) return 0;
  if ((u8)s[i] != 0xf0 || (u8)s[i + 1] != 0x9f || (u8)s[i + 2] != 0x97 || (u8)s[i + 3] != 0x93) return 0;
  i += 4;
  if (len - i >= 3 && (u8)s[i] == 0xef && (u8)s[i + 1] == 0xb8 && (u8)s[i + 2] == 0x8f) i += 3;
  while (i < len && is_space(s[i])) i++;

  usize w = 0;
  while (i < len && is_alpha(s[i]) && w < 31) weekday[w++] = s[i++];
  weekday[w] = 0;
  while (i < len && is_space(s[i])) i++;

  // dd.mm.yyyy with 1-2, 1-2 and 2-4 digits.
  usize start = i;
  usize d1 = 0;
  while (i < len && is_digit(s[i]) && d1 < 2) { i++; d1++; }
  if (d1 == 0 || i >= len || s[i] != '.') return 0;
  i++;
  usize d2 = 0;
  while (i < len && is_digit(s[i]) && d2 < 2) { i++; d2++; }
  if (d2 == 0 || i >= len || s[i] != '.') return 0;
  i++;
  usize d3 = 0;
  while (i < len && is_digit(s[i]) && d3 < 4) { i++; d3++; }
  if (d3 < 2) return 0;
  usize dlen = i - start;
  if (dlen > 15) return 0;
  mem_copy(date, s + start, dlen);
  date[dlen] = 0;

  while (i < len && (is_space(s[i]) || s[i] == '\r')) i++;
  return i == len;
}

// "01. Name" — number, dot, at most one space, then the rest trimmed.
static int match_player_line(const char *s, usize len, char *name, usize cap) {
  usize i = 0;
  while (i < len && is_space(s[i])) i++;
  usize digits = 0;
  while (i < len && is_digit(s[i]) && digits < 3) { i++; digits++; }
  if (digits == 0 || i >= len || s[i] != '.') return 0;
  i++;
  if (i < len && s[i] == ' ') i++;
  copy_trimmed(name, cap, s + i, len - i);
  return 1;
}

// ---- parse / rewrite -----------------------------------------------------

int roster_load(const char *text, const char *username) {
  raw_len = str_len(text);
  if (raw_len > sizeof(raw_text) - 1) raw_len = sizeof(raw_text) - 1;
  mem_copy(raw_text, text, raw_len);
  raw_text[raw_len] = 0;

  usize n = str_len(username);
  if (n > sizeof(cur_user) - 1) n = sizeof(cur_user) - 1;
  mem_copy(cur_user, username, n);
  cur_user[n] = 0;

  line_count = 0;
  block_count = 0;

  usize start = 0;
  for (usize i = 0; i <= raw_len && line_count < MAX_LINES; i++) {
    if (i == raw_len || raw_text[i] == '\n') {
      usize end = i;
      // split(/\r?\n/) drops the carriage return, so a CRLF file comes out as
      // LF. Keeping the \r would be more faithful to the input and less
      // faithful to prototype-minimal, which is the thing being matched.
      if (end > start && raw_text[end - 1] == '\r') end--;
      lines[line_count].ptr = raw_text + start;
      lines[line_count].len = end - start;
      line_count++;
      start = i + 1;
    }
  }

  Block *cur = NULL;
  for (int i = 0; i < line_count; i++) {
    char weekday[32], date[16];
    if (match_date_line(lines[i].ptr, lines[i].len, weekday, date)) {
      if (block_count >= MAX_BLOCKS) { cur = NULL; continue; }
      cur = &blocks[block_count++];
      cur->date_idx = i;
      usize wl = str_len(weekday);
      mem_copy(cur->weekday, weekday, wl + 1);
      usize dl = str_len(date);
      mem_copy(cur->date, date, dl + 1);
      cur->first_player = -1;
      cur->last_player = -1;
      cur->player_count = 0;
      continue;
    }
    if (!cur) continue;
    char name[96];
    if (match_player_line(lines[i].ptr, lines[i].len, name, sizeof(name))) {
      if (cur->first_player < 0) cur->first_player = i;
      cur->last_player = i;
      if (cur->player_count < MAX_PLAYERS) {
        usize nl = str_len(name);
        mem_copy(cur->players[cur->player_count], name, nl + 1);
        cur->player_count++;
      }
    }
  }

  // Rewrite a bare "<username>" slot to the tagged form, exactly as
  // normalizeUserSlots does, so the output always marks the signed-in user.
  if (cur_user[0]) {
    char tagged[96];
    roster_display_name(tagged, sizeof(tagged), cur_user);
    for (int b = 0; b < block_count; b++) {
      for (int i = 0; i < blocks[b].player_count; i++) {
        if (is_same_user(blocks[b].players[i], cur_user) && !str_eq(blocks[b].players[i], tagged)) {
          usize tl = str_len(tagged);
          mem_copy(blocks[b].players[i], tagged, tl + 1);
        }
      }
    }
  }
  return block_count;
}

int roster_block_count(void) { return block_count; }

int roster_filled(int idx) {
  if (idx < 0 || idx >= block_count) return 0;
  int n = 0;
  for (int i = 0; i < blocks[idx].player_count; i++) {
    if (blocks[idx].players[i][0]) n++;
  }
  return n;
}

int roster_going(int idx) {
  if (idx < 0 || idx >= block_count || !cur_user[0]) return 0;
  for (int i = 0; i < blocks[idx].player_count; i++) {
    if (is_same_user(blocks[idx].players[i], cur_user)) return 1;
  }
  return 0;
}

const char *roster_date(int idx) {
  return (idx >= 0 && idx < block_count) ? blocks[idx].date : "";
}

// Sakamoto's day-of-week, standing in for `new Date(y, m-1, d).getDay()`.
static const char *WEEKDAY_SHORT[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static int day_of_week(int y, int m, int d) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// The weekday written in the roster wins; otherwise it is computed from the
// date, the same order of preference as the JS.
const char *roster_weekday(int idx) {
  static char out[8];
  if (idx < 0 || idx >= block_count) return "";
  Block *b = &blocks[idx];
  if (b->weekday[0]) {
    usize n = str_len(b->weekday);
    if (n > 3) n = 3;
    mem_copy(out, b->weekday, n);
    out[n] = 0;
    return out;
  }
  int d = 0, m = 0, y = 0, part = 0, val = 0, digits = 0;
  for (usize i = 0; ; i++) {
    char c = b->date[i];
    if (is_digit(c)) { val = val * 10 + (c - '0'); digits++; continue; }
    if (part == 0) d = val;
    else if (part == 1) m = val;
    else y = val;
    if (!c) break;
    part++;
    if (digits == 0) return "";
    val = 0;
    digits = 0;
  }
  if (y < 100) y += 2000;
  if (m < 1 || m > 12 || d < 1 || d > 31) return "";
  const char *w = WEEKDAY_SHORT[day_of_week(y, m, d)];
  usize n = str_len(w);
  mem_copy(out, w, n + 1);
  return out;
}

void roster_toggle(int idx) {
  if (idx < 0 || idx >= block_count || !cur_user[0]) return;
  Block *b = &blocks[idx];
  char tagged[96];
  roster_display_name(tagged, sizeof(tagged), cur_user);

  if (roster_going(idx)) {
    // Empty the slot but keep it, so the numbering does not shift.
    for (int i = 0; i < b->player_count; i++) {
      if (is_same_user(b->players[i], cur_user)) b->players[i][0] = 0;
    }
    return;
  }
  for (int i = 0; i < b->player_count; i++) {
    if (!b->players[i][0]) {
      usize tl = str_len(tagged);
      mem_copy(b->players[i], tagged, tl + 1);
      return;
    }
  }
  if (b->player_count < MAX_PLAYERS) {
    usize tl = str_len(tagged);
    mem_copy(b->players[b->player_count], tagged, tl + 1);
    b->player_count++;
  }
}

// Splice the player lines back in, leaving every other line byte for byte as
// it arrived. Blocks are walked back to front so the earlier line indices stay
// valid while later ones are replaced.
const char *roster_text(void) {
  Builder out;
  b_init(&out, raw_len + 1024);

  int i = 0;
  while (i < line_count) {
    // Does a block's player region start here? If so the whole region — which
    // may include blank lines between the numbered ones — is replaced by the
    // current player list, however many lines that now takes.
    int blk = -1;
    for (int b = 0; b < block_count; b++) {
      if (blocks[b].first_player == i) { blk = b; break; }
    }
    if (blk < 0) {
      b_bytes(&out, lines[i].ptr, lines[i].len);
      if (i != line_count - 1) b_char(&out, '\n');
      i++;
      continue;
    }
    Block *bl = &blocks[blk];
    for (int p = 0; p < bl->player_count; p++) {
      if (p) b_char(&out, '\n');
      int num = p + 1;
      b_char(&out, (char)('0' + (num / 10) % 10));
      b_char(&out, (char)('0' + num % 10));
      b_str(&out, ". ");
      // An empty slot keeps its trailing space: the JS writes `${n}. ` for a
      // blank name.
      b_str(&out, bl->players[p]);
    }
    // The separator belongs to the last line the region consumed, not to each
    // of them.
    if (bl->last_player != line_count - 1) b_char(&out, '\n');
    i = bl->last_player + 1;
  }
  return b_done(&out);
}

// "🕖 19.00 ~ 21:00" -> "19:00". Accepts . or : as the separator and any of
// ~ - – — between the two times.
const char *roster_time(void) {
  static char out[8];
  const char *s = raw_text;
  usize n = raw_len;
  for (usize i = 0; i + 4 < n; i++) {
    if (!is_digit(s[i])) continue;
    if (i > 0 && is_digit(s[i - 1])) continue;
    usize j = i;
    int h = 0, hd = 0;
    while (j < n && is_digit(s[j]) && hd < 2) { h = h * 10 + (s[j] - '0'); j++; hd++; }
    if (j >= n || (s[j] != ':' && s[j] != '.')) continue;
    j++;
    int md = 0;
    int mm = 0;
    while (j < n && is_digit(s[j]) && md < 2) { mm = mm * 10 + (s[j] - '0'); j++; md++; }
    if (md != 2) continue;
    usize k = j;
    while (k < n && is_space(s[k])) k++;
    // ~ or -, or the UTF-8 en/em dashes E2 80 93 / E2 80 94.
    if (k < n && (s[k] == '~' || s[k] == '-')) k++;
    else if (k + 2 < n && (u8)s[k] == 0xe2 && (u8)s[k + 1] == 0x80
             && ((u8)s[k + 2] == 0x93 || (u8)s[k + 2] == 0x94)) k += 3;
    else continue;
    while (k < n && is_space(s[k])) k++;
    int d2 = 0;
    while (k < n && is_digit(s[k]) && d2 < 2) { k++; d2++; }
    if (d2 == 0 || k >= n || (s[k] != ':' && s[k] != '.')) continue;
    k++;
    int d3 = 0;
    while (k < n && is_digit(s[k]) && d3 < 2) { k++; d3++; }
    if (d3 != 2) continue;

    usize o = 0;
    if (h >= 10) out[o++] = (char)('0' + h / 10);
    out[o++] = (char)('0' + h % 10);
    out[o++] = ':';
    out[o++] = (char)('0' + mm / 10);
    out[o++] = (char)('0' + mm % 10);
    out[o] = 0;
    return out;
  }
  return "";
}
