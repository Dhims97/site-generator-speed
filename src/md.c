// md.c — front matter and the Markdown subset, ported from v1's solve.js
// (which follows makefun-site's lib/frontmatter.js and lib/markdown.js).
// Every regex is hand-rolled with the reference's exact semantics; strings
// are U16 views into the source, so lexing allocates almost nothing.
#include "sitegen.h"

// ------------------------------------------------------------------ helpers
static int is_lower_or_underscore(uint16_t c) { return (c >= 'a' && c <= 'z') || c == '_'; }
static int is_digit(uint16_t c) { return c >= '0' && c <= '9'; }

// U16 equality against a NUL-terminated ASCII literal.
static int u16_is(U16 s, const char *lit) { return u16_eq_ascii(s, lit); }

// Trim trailing spaces/tabs (JS /[ \t]+$/ on the raw value).
static U16 trim_trailing_ws(U16 s) {
  while (s.n > 0 && (s.s[s.n - 1] == ' ' || s.s[s.n - 1] == '\t')) s.n--;
  return s;
}

// JS String.trim() over the \s set.
static U16 trim_ws(U16 s) {
  size_t a = 0, b = s.n;
  while (a < b && is_js_space(s.s[a])) a++;
  while (b > a && is_js_space(s.s[b - 1])) b--;
  return u16_slice(s, a, b);
}

// ------------------------------------------------------------------ front matter
static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

// fmStringOf: returns 1 and fills *value, or 0 and fills *err.
static int fm_string_of(Arena *a, U16 raw, U16 *value, U16 *err) {
  (void)a;
  if (raw.n > 0 && raw.s[0] == '"') {
    if (raw.n < 2 || raw.s[raw.n - 1] != '"') {
      *err = u16_from_ascii(a, "a quoted string is not closed");
      return 0;
    }
    U16Buf out;
    ub_init(&out, a);
    for (size_t i = 1; i + 1 < raw.n; i++) {
      uint16_t c = raw.s[i];
      if (c == '\\') {
        uint16_t n = raw.s[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.n - 1) {
          *err = u16_from_ascii(a, "a quoted string holds an escape other than \\\" or \\\\");
          return 0;
        }
        ub_put(&out, n);
        i++;
      } else if (c == '"') {
        *err = u16_from_ascii(a, "a quoted string holds a stray quote");
        return 0;
      } else {
        ub_put(&out, c);
      }
    }
    *value = ub_finish(&out);
    return 1;
  }
  if (raw.n > 0 && raw.s[0] == '[') {
    *err = u16_from_ascii(a, "a bare string opens on [");
    return 0;
  }
  *value = raw;
  return 1;
}

// Builds `key "<k>": <msg>` into an arena U16.
static U16 keyed_error(Arena *a, U16 key, const char *msg) {
  U16Buf out;
  ub_init(&out, a);
  ub_put_ascii(&out, "key \"");
  ub_put_u16(&out, key);
  ub_put_ascii(&out, "\": ");
  ub_put_ascii(&out, msg);
  return ub_finish(&out);
}

// fmValueOf: returns 1 and fills *value, or 0 and fills *err (already keyed).
static int fm_value_of(Arena *a, U16 key, U16 raw, U16 *value, int *is_bool, int *bool_val,
                       U16 **tags_out, size_t *ntags_out, U16 *err) {
  *is_bool = 0;
  *tags_out = NULL;
  *ntags_out = 0;
  if (u16_is(key, "title") || u16_is(key, "summary")) {
    U16 v, e;
    if (!fm_string_of(a, raw, &v, &e)) {
      // `key "<k>": <inner message>`
      U16Buf out;
      ub_init(&out, a);
      ub_put_ascii(&out, "key \"");
      ub_put_u16(&out, key);
      ub_put_ascii(&out, "\": ");
      ub_put_u16(&out, e);
      *err = ub_finish(&out);
      return 0;
    }
    *value = v;
    return 1;
  }
  if (u16_is(key, "date")) {
    // /^(\d{4})-(\d{2})-(\d{2})$/
    int ok = raw.n == 10 && is_digit(raw.s[0]) && is_digit(raw.s[1]) && is_digit(raw.s[2]) &&
             is_digit(raw.s[3]) && raw.s[4] == '-' && is_digit(raw.s[5]) && is_digit(raw.s[6]) &&
             raw.s[7] == '-' && is_digit(raw.s[8]) && is_digit(raw.s[9]);
    int y = 0, mo = 0, d = 0;
    if (ok) {
      y = (raw.s[0] - '0') * 1000 + (raw.s[1] - '0') * 100 + (raw.s[2] - '0') * 10 + (raw.s[3] - '0');
      mo = (raw.s[5] - '0') * 10 + (raw.s[6] - '0');
      d = (raw.s[8] - '0') * 10 + (raw.s[9] - '0');
      if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in(y, mo)) ok = 0;
    }
    if (!ok) {
      *err = keyed_error(a, key, "not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return 0;
    }
    *value = raw;
    return 1;
  }
  if (u16_is(key, "tags")) {
    if (raw.n < 2 || raw.s[0] != '[' || raw.s[raw.n - 1] != ']') {
      *err = keyed_error(a, key, "not a list [a, b]");
      return 0;
    }
    U16 inner = u16_slice(raw, 1, raw.n - 1);
    if (trim_ws(inner).n == 0) {
      *tags_out = NULL;
      *ntags_out = 0;
      return 1;
    }
    // Split on ','; each tag trimmed; empty or holding " [ ] is an error.
    // (Tags are views into the source text.)
    U16 *tags = NULL;
    size_t ntags = 0, cap = 0;
    size_t start = 0;
    for (size_t i = 0; i <= inner.n; i++) {
      if (i == inner.n || inner.s[i] == ',') {
        U16 t = trim_ws(u16_slice(inner, start, i));
        if (t.n == 0) {
          *err = keyed_error(a, key, "an empty tag, or one holding \" [ or ]");
          return 0;
        }
        for (size_t k = 0; k < t.n; k++) {
          if (t.s[k] == '"' || t.s[k] == '[' || t.s[k] == ']') {
            *err = keyed_error(a, key, "an empty tag, or one holding \" [ or ]");
            return 0;
          }
        }
        AGROW(a, tags, ntags, cap, U16);
        tags[ntags++] = t;
        start = i + 1;
      }
    }
    *tags_out = tags;
    *ntags_out = ntags;
    return 1;
  }
  if (u16_is(key, "draft")) {
    if (u16_is(raw, "true")) { *is_bool = 1; *bool_val = 1; return 1; }
    if (u16_is(raw, "false")) { *is_bool = 1; *bool_val = 0; return 1; }
    *err = keyed_error(a, key, "true or false");
    return 0;
  }
  if (u16_is(key, "layout")) {
    // /^[a-z0-9-]+$/
    int ok = raw.n > 0;
    for (size_t i = 0; ok && i < raw.n; i++) {
      uint16_t c = raw.s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
    }
    if (!ok) {
      *err = keyed_error(a, key, "not a template name");
      return 0;
    }
    *value = raw;
    return 1;
  }
  if (u16_is(key, "slug")) {
    // /^[a-z0-9]+(?:-[a-z0-9]+)*$/ and length <= 64
    int ok = raw.n > 0 && raw.n <= 64;
    if (ok) {
      // segments of [a-z0-9]+ joined by single '-'
      size_t i = 0;
      while (ok && i < raw.n) {
        if (!((raw.s[i] >= 'a' && raw.s[i] <= 'z') || (raw.s[i] >= '0' && raw.s[i] <= '9'))) { ok = 0; break; }
        while (i < raw.n && ((raw.s[i] >= 'a' && raw.s[i] <= 'z') || (raw.s[i] >= '0' && raw.s[i] <= '9'))) i++;
        if (i < raw.n) {
          if (raw.s[i] != '-') { ok = 0; break; }
          i++;
          if (i >= raw.n) { ok = 0; break; } // trailing '-'
        }
      }
    }
    if (!ok) {
      *err = keyed_error(a, key, "not a slug");
      return 0;
    }
    *value = raw;
    return 1;
  }
  // Unknown key (unreachable: keys are checked before this is called).
  {
    U16Buf out;
    ub_init(&out, a);
    ub_put_ascii(&out, "unknown key \"");
    ub_put_u16(&out, key);
    ub_put_ascii(&out, "\"");
    *err = ub_finish(&out);
    return 0;
  }
}

static const char *const FM_KEYS[] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};

static int fm_key_known(U16 key) {
  for (size_t i = 0; i < sizeof(FM_KEYS) / sizeof(FM_KEYS[0]); i++) {
    if (u16_eq_ascii(key, FM_KEYS[i])) return 1;
  }
  return 0;
}

// FM_LINE /^([a-z_]+):(?: (.*))?$/: fills key/raw, returns 1 on match.
static int fm_line_match(U16 line, U16 *key, U16 *raw) {
  size_t i = 0;
  while (i < line.n && is_lower_or_underscore(line.s[i])) i++;
  if (i == 0) return 0;
  if (i >= line.n || line.s[i] != ':') return 0;
  *key = u16_slice(line, 0, i);
  if (i + 1 < line.n && line.s[i + 1] == ' ') {
    U16 r = u16_slice(line, i + 2, line.n);
    // (.*) does not match line terminators: U+2028/U+2029 would fail the regex.
    for (size_t k = 0; k < r.n; k++) {
      if (r.s[k] == 0x2028 || r.s[k] == 0x2029) return 0;
    }
    *raw = trim_trailing_ws(r);
  } else {
    *raw = u16_empty();
  }
  return 1;
}

U16 parse_front_matter(Arena *a, U16 text, FMData *fm, U16 *body) {
  memset(fm, 0, sizeof *fm);
  // `---` alone, or `---\n` at the start; anything else is 'no front matter'.
  static const uint16_t DASH3[3] = {'-', '-', '-'};
  if (!(text.n == 3 && memcmp(text.s, DASH3, 6) == 0) &&
      !(text.n >= 4 && text.s[0] == '-' && text.s[1] == '-' && text.s[2] == '-' && text.s[3] == '\n')) {
    return u16_from_ascii(a, "no front matter");
  }
  // Find the closing line: a line that is exactly '---'.
  long close_start = -1; // index of the '\n' just before the closing '---'
  size_t from = 4;
  for (;;) {
    // search for "\n---" starting at `from`
    long idx = -1;
    for (size_t i = from; i + 4 <= text.n; i++) {
      if (text.s[i] == '\n' && text.s[i + 1] == '-' && text.s[i + 2] == '-' && text.s[i + 3] == '-') {
        idx = (long)i;
        break;
      }
    }
    if (idx == -1) break;
    size_t after = (size_t)idx + 4;
    if (after == text.n || text.s[after] == '\n') { close_start = idx; break; }
    from = after;
  }
  if (close_start == -1) return u16_from_ascii(a, "the front matter is not closed");
  U16 region = u16_slice(text, 4, (size_t)close_start);
  // Split the region into lines (views; no copying).
  // Iterate lines directly to avoid building an array.
  size_t line_no = 0; // 0-based within region; file line is line_no + 2
  size_t pos = 0;
  // Track which keys were seen, for the "given twice" check.
  int seen_title = 0, seen_date = 0, seen_tags = 0, seen_draft = 0, seen_layout = 0, seen_slug = 0, seen_summary = 0;
  for (;;) {
    size_t eol = pos;
    while (eol < region.n && region.s[eol] != '\n') eol++;
    U16 line = u16_slice(region, pos, eol);
    int is_last = (eol >= region.n);
    // Skip blank lines, whitespace-only lines, and '#' comment lines.
    int skip = 0;
    if (line.n == 0) skip = 1;
    else if (line.s[0] == ' ' || line.s[0] == '\t') {
      skip = 1;
      for (size_t k = 0; k < line.n; k++) {
        if (line.s[k] != ' ' && line.s[k] != '\t') { skip = 0; break; }
      }
    } else if (line.s[0] == '#') skip = 1;
    if (!skip) {
      U16 key, raw;
      if (!fm_line_match(line, &key, &raw)) {
        U16Buf out;
        ub_init(&out, a);
        ub_put_ascii(&out, "line ");
        ub_put_int(&out, (int64_t)(line_no + 2));
        ub_put_ascii(&out, " is not \"key: value\"");
        return ub_finish(&out);
      }
      if (!fm_key_known(key)) {
        U16Buf out;
        ub_init(&out, a);
        ub_put_ascii(&out, "unknown key \"");
        ub_put_u16(&out, key);
        ub_put_ascii(&out, "\"");
        return ub_finish(&out);
      }
      int seen = 0;
      if (u16_is(key, "title")) { seen = seen_title; seen_title = 1; }
      else if (u16_is(key, "date")) { seen = seen_date; seen_date = 1; }
      else if (u16_is(key, "tags")) { seen = seen_tags; seen_tags = 1; }
      else if (u16_is(key, "draft")) { seen = seen_draft; seen_draft = 1; }
      else if (u16_is(key, "layout")) { seen = seen_layout; seen_layout = 1; }
      else if (u16_is(key, "slug")) { seen = seen_slug; seen_slug = 1; }
      else if (u16_is(key, "summary")) { seen = seen_summary; seen_summary = 1; }
      if (seen) {
        U16Buf out;
        ub_init(&out, a);
        ub_put_ascii(&out, "key \"");
        ub_put_u16(&out, key);
        ub_put_ascii(&out, "\" is given twice");
        return ub_finish(&out);
      }
      if (raw.n == 0) {
        U16Buf out;
        ub_init(&out, a);
        ub_put_ascii(&out, "key \"");
        ub_put_u16(&out, key);
        ub_put_ascii(&out, "\" has no value");
        return ub_finish(&out);
      }
      U16 value, err;
      int is_bool = 0, bool_val = 0;
      U16 *tags_out = NULL;
      size_t ntags_out = 0;
      if (!fm_value_of(a, key, raw, &value, &is_bool, &bool_val, &tags_out, &ntags_out, &err)) {
        return err;
      }
      if (u16_is(key, "title")) { fm->has_title = 1; fm->title = value; }
      else if (u16_is(key, "date")) { fm->has_date = 1; fm->date = value; }
      else if (u16_is(key, "tags")) { fm->has_tags = 1; fm->tags = tags_out; fm->ntags = ntags_out; }
      else if (u16_is(key, "draft")) { fm->has_draft = 1; fm->draft = bool_val; }
      else if (u16_is(key, "layout")) { fm->has_layout = 1; fm->layout = value; }
      else if (u16_is(key, "slug")) { fm->has_slug = 1; fm->slug = value; }
      else if (u16_is(key, "summary")) { fm->has_summary = 1; fm->summary = value; }
    }
    if (is_last) break;
    pos = eol + 1;
    line_no++;
    // A trailing empty line after the final '\n' is not a line: the region
    // split of "a\n" is ["a"], not ["a", ""]. If pos == region.n after
    // consuming the newline, stop (that empty tail is not a line).
    if (pos >= region.n) break;
  }
  if (!fm->has_title) return u16_from_ascii(a, "no title");
  size_t body_start = (size_t)close_start + 4;
  if (body_start >= text.n) *body = u16_empty();
  else if (text.s[body_start] == '\n') *body = u16_slice(text, body_start + 1, text.n);
  else *body = u16_empty();
  return u16_empty();
}

// ------------------------------------------------------------------ markdown
typedef enum { TK_CODE, TK_HEADING, TK_HR, TK_QUOTE, TK_LIST, TK_PARA } TkKind;
typedef struct {
  TkKind kind;
  U16 lang;                 // code
  U16 *lines; size_t nlines; // code, quote (views)
  int depth; U16 text;      // heading
  int ordered; long start;  // list
  U16 *items; size_t nitems; // list (joined item text)
  U16 ptext;                // paragraph
} Token;

static int is_blank_line(U16 line) {
  if (line.n == 0) return 1;
  if (line.s[0] != ' ' && line.s[0] != '\t') return 0;
  for (size_t i = 0; i < line.n; i++) {
    if (line.s[i] != ' ' && line.s[i] != '\t') return 0;
  }
  return 1;
}

// FENCE_OPEN /^```([A-Za-z0-9_+-]*)[ \t]*$/: returns 1, fills *lang.
static int fence_open_match(U16 line, U16 *lang) {
  if (line.n < 3 || line.s[0] != '`' || line.s[1] != '`' || line.s[2] != '`') return 0;
  size_t i = 3;
  while (i < line.n) {
    uint16_t c = line.s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') i++;
    else break;
  }
  *lang = u16_slice(line, 3, i);
  while (i < line.n && (line.s[i] == ' ' || line.s[i] == '\t')) i++;
  return i == line.n;
}

static int fence_close_match(U16 line) {
  if (line.n < 3 || line.s[0] != '`' || line.s[1] != '`' || line.s[2] != '`') return 0;
  size_t i = 3;
  while (i < line.n && (line.s[i] == ' ' || line.s[i] == '\t')) i++;
  return i == line.n;
}

// headingOf: null (depth 0) when not a heading; else depth + text.
static int heading_match(U16 line, int *depth, U16 *text) {
  if (line.n == 0 || line.s[0] != '#') return 0;
  size_t i = 0;
  while (i < line.n && line.s[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  if (i >= line.n || (line.s[i] != ' ' && line.s[i] != '\t')) return 0;
  size_t j = i;
  while (j < line.n && (line.s[j] == ' ' || line.s[j] == '\t')) j++;
  size_t k = line.n;
  while (k > j && (line.s[k - 1] == ' ' || line.s[k - 1] == '\t')) k--;
  if (k <= j) return 0; // m[2] === ''
  *depth = (int)i;
  *text = u16_slice(line, j, k);
  return 1;
}

static int is_hr_line(U16 line) {
  if (u16_is(line, "---") || u16_is(line, "***")) return 1;
  if (line.n == 0) return 0;
  uint16_t c0 = line.s[0];
  if (c0 != '-' && c0 != '*') return 0;
  if (line.n < 3) return 0;
  if (c0 == '-' && !(line.s[1] == '-' && line.s[2] == '-')) return 0;
  if (c0 == '*' && !(line.s[1] == '*' && line.s[2] == '*')) return 0;
  // slice(3) must be one or more spaces/tabs, nothing else.
  if (line.n == 3) return 0;
  for (size_t i = 3; i < line.n; i++) {
    if (line.s[i] != ' ' && line.s[i] != '\t') return 0;
  }
  return 1;
}

static int is_fence_open_line(U16 line) {
  U16 lang;
  return line.n >= 1 && line.s[0] == '`' && line.n >= 3 && line.s[1] == '`' && line.s[2] == '`' &&
         fence_open_match(line, &lang);
}

static int ends_paragraph(U16 line) {
  int depth;
  U16 text;
  return is_blank_line(line) || is_fence_open_line(line) || heading_match(line, &depth, &text) || is_hr_line(line);
}

static U16 trim_line(U16 line) {
  size_t a = 0, b = line.n;
  while (a < b && (line.s[a] == ' ' || line.s[a] == '\t')) a++;
  while (b > a && (line.s[b - 1] == ' ' || line.s[b - 1] == '\t')) b--;
  return u16_slice(line, a, b);
}

// Split src into lines (views). Returns the count; lines are written into
// the arena array *out (n+1 slots: a trailing empty line after a final '\n'
// is NOT a line — JS "a\n".split('\n') is ["a", ""], so it IS a line!
// JS split keeps the trailing empty string. Keep it: n = newlines + 1.
static size_t split_lines(Arena *a, U16 src, U16 **out) {
  size_t n = 1;
  for (size_t i = 0; i < src.n; i++) if (src.s[i] == '\n') n++;
  U16 *lines = ARENA_ARR(a, U16, n);
  size_t li = 0, start = 0;
  for (size_t i = 0; i <= src.n; i++) {
    if (i == src.n || src.s[i] == '\n') {
      lines[li++] = u16_slice(src, start, i);
      start = i + 1;
    }
  }
  *out = lines;
  return n;
}

static Token *lex(Arena *a, U16 src, size_t *ntokens) {
  U16 *lines;
  size_t n = split_lines(a, src, &lines);
  Token *toks = NULL;
  size_t nt = 0, cap = 0;
  size_t i = 0;
  while (i < n) {
    U16 line = lines[i];
    if (is_blank_line(line)) { i++; continue; }
    if (line.n >= 1 && line.s[0] == '`' && line.n >= 3 && line.s[1] == '`' && line.s[2] == '`') {
      U16 lang;
      if (fence_open_match(line, &lang)) {
        U16 *body = NULL;
        size_t nb = 0, bcap = 0;
        i++;
        while (i < n && !fence_close_match(lines[i])) {
          AGROW(a, body, nb, bcap, U16);
          body[nb++] = lines[i++];
        }
        i++; // skip the closing fence (or run past the end)
        AGROW(a, toks, nt, cap, Token);
        toks[nt].kind = TK_CODE;
        toks[nt].lang = lang;
        toks[nt].lines = body;
        toks[nt].nlines = nb;
        nt++;
        continue;
      }
    }
    int depth = 0;
    U16 htext;
    if (heading_match(line, &depth, &htext)) {
      AGROW(a, toks, nt, cap, Token);
      toks[nt].kind = TK_HEADING;
      toks[nt].depth = depth;
      toks[nt].text = htext;
      nt++;
      i++;
      continue;
    }
    if (is_hr_line(line)) {
      AGROW(a, toks, nt, cap, Token);
      toks[nt].kind = TK_HR;
      nt++;
      i++;
      continue;
    }
    if (line.n >= 1 && line.s[0] == '>') {
      U16 *body = NULL;
      size_t nb = 0, bcap = 0;
      while (i < n && lines[i].n >= 1 && lines[i].s[0] == '>') {
        U16 l = lines[i++];
        // strip /^> ?/
        if (l.n >= 2 && l.s[1] == ' ') l = u16_slice(l, 2, l.n);
        else l = u16_slice(l, 1, l.n);
        U16 t = trim_line(l);
        if (t.n == 0) continue;
        AGROW(a, body, nb, bcap, U16);
        body[nb++] = t;
      }
      AGROW(a, toks, nt, cap, Token);
      toks[nt].kind = TK_QUOTE;
      toks[nt].lines = body;
      toks[nt].nlines = nb;
      nt++;
      continue;
    }
    uint16_t c0 = line.n >= 1 ? line.s[0] : 0;
    // BULLET /^[-*] (.*)$/ or ORDERED /^(\d{1,9})\. (.*)$/
    int is_bullet = 0, is_ordered = 0;
    long ostart = 0;
    if (c0 == '-' || c0 == '*') {
      if (line.n >= 2 && line.s[1] == ' ') { is_bullet = 1; }
    } else if (c0 >= '0' && c0 <= '9') {
      size_t d = 0;
      while (d < line.n && line.s[d] >= '0' && line.s[d] <= '9') d++;
      if (d >= 1 && d <= 9 && d + 1 < line.n && line.s[d] == '.' && line.s[d + 1] == ' ') {
        is_ordered = 1;
        long v = 0;
        for (size_t k = 0; k < d; k++) v = v * 10 + (line.s[k] - '0');
        ostart = v;
      }
    }
    if (is_bullet || is_ordered) {
      // Items: each item is a list of trimmed lines, joined with '\n'.
      U16 *items = NULL;
      size_t nitems = 0, icap = 0;
      U16Buf cur;
      ub_init(&cur, a);
      int have_cur = 0;
      while (i < n) {
        U16 l = lines[i];
        int m = 0;
        U16 itext;
        if (is_bullet) {
          if (l.n >= 2 && (l.s[0] == '-' || l.s[0] == '*') && l.s[1] == ' ') {
            m = 1;
            itext = u16_slice(l, 2, l.n);
          }
        } else {
          size_t d = 0;
          while (d < l.n && l.s[d] >= '0' && l.s[d] <= '9') d++;
          if (d >= 1 && d <= 9 && d + 1 < l.n && l.s[d] == '.' && l.s[d + 1] == ' ') {
            m = 1;
            itext = u16_slice(l, d + 2, l.n);
          }
        }
        if (m) {
          if (have_cur) {
            AGROW(a, items, nitems, icap, U16);
            items[nitems++] = ub_finish(&cur);
            ub_init(&cur, a);
          }
          ub_put_u16(&cur, trim_line(itext));
          have_cur = 1;
          i++;
        } else {
          // continuation: /^ {2,}\S/ — two or more spaces, then a
          // non-whitespace char
          size_t p = 0;
          while (p < l.n && l.s[p] == ' ') p++;
          if (p >= 2 && p < l.n && !is_js_space(l.s[p])) {
            ub_put(&cur, '\n');
            ub_put_u16(&cur, trim_line(l));
            i++;
          } else break;
        }
      }
      if (have_cur) {
        AGROW(a, items, nitems, icap, U16);
        items[nitems++] = ub_finish(&cur);
      }
      AGROW(a, toks, nt, cap, Token);
      toks[nt].kind = TK_LIST;
      toks[nt].ordered = is_ordered;
      toks[nt].start = ostart;
      toks[nt].items = items;
      toks[nt].nitems = nitems;
      nt++;
      continue;
    }
    // Paragraph.
    U16Buf body;
    ub_init(&body, a);
    ub_put_u16(&body, trim_line(line));
    i++;
    while (i < n && !ends_paragraph(lines[i])) {
      ub_put(&body, '\n');
      ub_put_u16(&body, trim_line(lines[i++]));
    }
    AGROW(a, toks, nt, cap, Token);
    toks[nt].kind = TK_PARA;
    toks[nt].ptext = ub_finish(&body);
    nt++;
  }
  *ntokens = nt;
  return toks;
}

// ------------------------------------------------------------------ inline
static const char ESCAPABLE_CHARS[] = "\\`*_[]()#+-.!>";

static int is_escapable(uint16_t c) {
  return strchr(ESCAPABLE_CHARS, (char)c) != NULL;
}

static int has_ws(U16 s) {
  for (size_t i = 0; i < s.n; i++) if (is_js_space(s.s[i])) return 1;
  return 0;
}

// findUnescaped(s, what, from): what is a 1-2 unit literal.
static long find_unescaped(U16 s, const uint16_t *what, size_t wlen, size_t from) {
  if (wlen == 0 || from > s.n) return -1;
  for (size_t j = from; j + wlen <= s.n; j++) {
    if (s.s[j] == '\\' && j + 1 < s.n && is_escapable(s.s[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s.s + j, what, wlen * 2) == 0) return (long)j;
  }
  return -1;
}

static long find_single_star(U16 s, size_t from) {
  for (size_t j = from; j < s.n; j++) {
    uint16_t c = s.s[j];
    if (c == '\\' && j + 1 < s.n && is_escapable(s.s[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.s[j + 1] == '*') {
      j++;
      continue;
    }
    return (long)j;
  }
  return -1;
}

static void inline_render(Arena *a, U16 s, LinkFn link, void *link_ctx, U16Buf *out) {
  size_t n = s.n;
  size_t run_start = 0;
  size_t pos = 0;
  while (pos < n) {
    // Find the next special char (\ ` * [).
    size_t j = pos;
    while (j < n && s.s[j] != '\\' && s.s[j] != '`' && s.s[j] != '*' && s.s[j] != '[') j++;
    if (j >= n) break;
    uint16_t c = s.s[j];
    if (c == '\\') {
      if (j + 1 < n && is_escapable(s.s[j + 1])) {
        if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
        ub_put(out, s.s[j + 1]); // escapable chars need no HTML escaping
        pos = run_start = j + 2;
      } else {
        pos = j + 1; // literal backslash stays in the run
      }
      continue;
    }
    if (c == '`') {
      long k = u16_index_of_ch(s, '`', j + 1);
      if (k > (long)j + 1) {
        if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
        ub_put_ascii(out, "<code>");
        escape_html_into(out, u16_slice(s, j + 1, (size_t)k));
        ub_put_ascii(out, "</code>");
        pos = run_start = (size_t)k + 1;
      } else {
        if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
        ub_put(out, '`');
        pos = run_start = j + 1;
      }
      continue;
    }
    if (c == '*') {
      if (j + 1 < n && s.s[j + 1] == '*') {
        static const uint16_t BB[2] = {'*', '*'};
        long k = find_unescaped(s, BB, 2, j + 2);
        if (k > (long)j + 2) {
          if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
          ub_put_ascii(out, "<strong>");
          inline_render(a, u16_slice(s, j + 2, (size_t)k), link, link_ctx, out);
          ub_put_ascii(out, "</strong>");
          pos = run_start = (size_t)k + 2;
        } else {
          if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
          ub_put_ascii(out, "**");
          pos = run_start = j + 2;
        }
        continue;
      }
      long k = find_single_star(s, j + 1);
      if (k > (long)j + 1) {
        if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
        ub_put_ascii(out, "<em>");
        inline_render(a, u16_slice(s, j + 1, (size_t)k), link, link_ctx, out);
        ub_put_ascii(out, "</em>");
        pos = run_start = (size_t)k + 1;
      } else {
        if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
        ub_put(out, '*');
        pos = run_start = j + 1;
      }
      continue;
    }
    // '[': a link, or a literal '['.
    static const uint16_t RB[1] = {']'};
    long k = find_unescaped(s, RB, 1, j + 1);
    long t = -1;
    if (k != -1 && (size_t)k + 1 < n && s.s[k + 1] == '(') t = u16_index_of_ch(s, ')', (size_t)k + 2);
    U16 target = u16_empty();
    if (t != -1) target = u16_slice(s, (size_t)k + 2, (size_t)t);
    if (target.n > 0 && !has_ws(target)) {
      if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
      ub_put_ascii(out, "<a href=\"");
      U16 href = link(link_ctx, target, a);
      escape_html_into(out, href);
      ub_put_ascii(out, "\">");
      inline_render(a, u16_slice(s, j + 1, (size_t)k), link, link_ctx, out);
      ub_put_ascii(out, "</a>");
      pos = run_start = (size_t)t + 1;
    } else {
      if (j > run_start) escape_html_into(out, u16_slice(s, run_start, j));
      ub_put(out, '[');
      pos = run_start = j + 1;
    }
  }
  if (run_start < n) escape_html_into(out, u16_slice(s, run_start, n));
}

// ------------------------------------------------------------------ render
static int slug_taken(U16 *set, size_t n, U16 slug) {
  for (size_t i = 0; i < n; i++) if (u16_eq(set[i], slug)) return 1;
  return 0;
}

// uniqueSlug against a set (linear; sets are small). Appends to the set.
static U16 unique_slug(Arena *a, U16 slug, U16 **set, size_t *n, size_t *cap) {
  U16 out = slug;
  int suffix = 2;
  while (slug_taken(*set, *n, out)) {
    U16Buf b;
    ub_init(&b, a);
    ub_put_u16(&b, slug);
    ub_put(&b, '-');
    ub_put_int(&b, suffix++);
    out = ub_finish(&b);
  }
  AGROW(a, *set, *n, *cap, U16);
  (*set)[(*n)++] = out;
  return out;
}

MdResult render_markdown(Arena *a, U16 src, LinkFn link, void *link_ctx) {
  size_t ntokens = 0;
  Token *toks = lex(a, src, &ntokens);
  U16 *ids = NULL;
  size_t nids = 0, cap_ids = 0;
  U16Buf html;
  ub_init(&html, a);
  TocItem *toc = NULL;
  size_t ntoc = 0, cap_toc = 0;
  U16 excerpt = u16_empty();
  int have_excerpt = 0;
  for (size_t ti = 0; ti < ntokens; ti++) {
    Token *t = &toks[ti];
    switch (t->kind) {
      case TK_HEADING: {
        U16 slug = slugify(a, t->text);
        U16 id = unique_slug(a, slug, &ids, &nids, &cap_ids);
        U16Buf hb;
        ub_init(&hb, a);
        inline_render(a, t->text, link, link_ctx, &hb);
        U16 hhtml = ub_finish(&hb);
        ub_put_ascii(&html, "<h");
        ub_put_int(&html, t->depth);
        ub_put_ascii(&html, " id=\"");
        ub_put_u16(&html, id);
        ub_put_ascii(&html, "\">");
        ub_put_u16(&html, hhtml);
        ub_put_ascii(&html, "</h");
        ub_put_int(&html, t->depth);
        ub_put_ascii(&html, ">\n");
        if (t->depth == 2 || t->depth == 3) {
          AGROW(a, toc, ntoc, cap_toc, TocItem);
          toc[ntoc].level = t->depth;
          toc[ntoc].id = id;
          toc[ntoc].html = hhtml;
          ntoc++;
        }
        break;
      }
      case TK_CODE: {
        ub_put_ascii(&html, "<pre><code");
        if (t->lang.n > 0) {
          ub_put_ascii(&html, " class=\"language-");
          escape_html_into(&html, t->lang);
          ub_put(&html, '"');
        }
        ub_put(&html, '>');
        if (t->nlines > 0) {
          U16Buf body;
          ub_init(&body, a);
          for (size_t k = 0; k < t->nlines; k++) {
            if (k) ub_put(&body, '\n');
            ub_put_u16(&body, t->lines[k]);
          }
          U16 joined = ub_finish(&body);
          escape_html_into(&html, joined);
          ub_put(&html, '\n');
        }
        ub_put_ascii(&html, "</code></pre>\n");
        break;
      }
      case TK_HR:
        ub_put_ascii(&html, "<hr>\n");
        break;
      case TK_QUOTE: {
        if (t->nlines == 0) {
          ub_put_ascii(&html, "<blockquote>\n</blockquote>\n");
        } else {
          U16Buf joined;
          ub_init(&joined, a);
          for (size_t k = 0; k < t->nlines; k++) {
            if (k) ub_put(&joined, '\n');
            ub_put_u16(&joined, t->lines[k]);
          }
          U16 jt = ub_finish(&joined);
          ub_put_ascii(&html, "<blockquote>\n<p>");
          inline_render(a, jt, link, link_ctx, &html);
          ub_put_ascii(&html, "</p>\n</blockquote>\n");
        }
        break;
      }
      case TK_LIST: {
        const char *tag = t->ordered ? "ol" : "ul";
        if (t->ordered && t->start != 1) {
          ub_put_ascii(&html, "<ol start=\"");
          ub_put_int(&html, t->start);
          ub_put_ascii(&html, "\">\n");
        } else {
          ub_put_ascii(&html, "<");
          ub_put_ascii(&html, tag);
          ub_put_ascii(&html, ">\n");
        }
        for (size_t k = 0; k < t->nitems; k++) {
          ub_put_ascii(&html, "<li>");
          inline_render(a, t->items[k], link, link_ctx, &html);
          ub_put_ascii(&html, "</li>\n");
        }
        ub_put_ascii(&html, "</");
        ub_put_ascii(&html, tag);
        ub_put_ascii(&html, ">\n");
        break;
      }
      case TK_PARA: {
        U16Buf pb;
        ub_init(&pb, a);
        inline_render(a, t->ptext, link, link_ctx, &pb);
        U16 phtml = ub_finish(&pb);
        if (!have_excerpt) {
          excerpt = phtml;
          have_excerpt = 1;
        }
        ub_put_ascii(&html, "<p>");
        ub_put_u16(&html, phtml);
        ub_put_ascii(&html, "</p>\n");
        break;
      }
    }
  }
  MdResult r;
  r.html = ub_finish(&html);
  r.toc = toc;
  r.ntoc = ntoc;
  r.excerpt = have_excerpt ? excerpt : u16_empty();
  return r;
}
