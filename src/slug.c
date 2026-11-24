// slug.c — vendor text.js, exact: HTML escapes, slugify (NFKD + mark strip),
// long date, word count.
//
// Slugify's Unicode table is generated, not hand-curated: the reference does
// normalize('NFKD') then strips /\p{M}/, then keeps only [A-Za-z0-9], so a
// code point matters only when its NFKD expansion holds an ASCII letter or
// digit. nfkd-table.inc lists every such code point (U+0080..U+10FFFF,
// except surrogates and the fullwidth range, which is a range rule), with
// its full NFKD expansion, generated from Node's own normalize('NFKD').
// Anything not in the table is a gap either way. Verified against Node's
// slugify over every code point (see the differential test).
#include "sitegen.h"

// One NFKD expansion: up to 6 code points (the longest NFKD expansion
// that holds an ASCII letter or digit).
typedef struct { uint32_t cp; uint32_t out[6]; uint8_t n; } Decomp;

#include "nfkd-table.inc"

static const Decomp *find_decomp(uint32_t cp) {
  // Fullwidth ASCII (U+FF01-U+FF5E) is a range rule, not a table entry.
  if (cp >= 0xFF01 && cp <= 0xFF5E) return NULL; // handled by the caller
  // Binary search: DECOMPS is sorted by code point.
  size_t lo = 0, hi = sizeof(DECOMPS) / sizeof(DECOMPS[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (DECOMPS[mid].cp < cp) lo = mid + 1;
    else if (DECOMPS[mid].cp > cp) hi = mid;
    else return &DECOMPS[mid];
  }
  return NULL;
}

U16 slugify(Arena *a, U16 text) {
  U16Buf out;
  ub_init(&out, a);
  int gap = 0;
  size_t i = 0;
  while (i < text.n) {
    size_t adv;
    uint32_t cp = u16_codepoint_at(text, i, &adv);
    i += adv;
    // NFKD expansion of this code point (1-3 code points).
    uint32_t exp[6];
    int nexp = 0;
    if (cp < 0x80) {
      exp[0] = cp;
      nexp = 1;
    } else if (cp >= 0xFF01 && cp <= 0xFF5E) {
      exp[0] = cp - 0xFEE0; // fullwidth ASCII -> ASCII
      nexp = 1;
    } else {
      const Decomp *d = find_decomp(cp);
      if (d) {
        for (int k = 0; k < d->n; k++) exp[nexp++] = d->out[k];
      } else {
        exp[0] = cp;
        nexp = 1;
      }
    }
    for (int k = 0; k < nexp; k++) {
      uint32_t e = exp[k];
      if (is_combining_mark(e)) continue; // \p{M} stripped
      int keep = -1; // -1 = gap, else the ASCII unit to keep
      if (e >= 'A' && e <= 'Z') keep = (int)e + 32;
      else if ((e >= 'a' && e <= 'z') || (e >= '0' && e <= '9')) keep = (int)e;
      if (keep < 0) {
        if (out.n > 0) gap = 1;
        continue;
      }
      if (gap) {
        ub_put(&out, '-');
        gap = 0;
      }
      ub_put(&out, (uint16_t)keep);
    }
  }
  if (out.n > 64) ub_truncate(&out, 64);
  while (out.n > 0 && out.s[out.n - 1] == '-') out.n--;
  if (out.n == 0) return u16_from_ascii(a, "untitled");
  return ub_finish(&out);
}

static const char *const MONTHS[] = {
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December",
};

U16 long_date(Arena *a, U16 date) {
  // "YYYY-MM-DD", validated by front matter; d drops its leading zero.
  int m = (date.s[5] - '0') * 10 + (date.s[6] - '0');
  int d = (date.s[8] - '0') * 10 + (date.s[9] - '0');
  U16Buf out;
  ub_init(&out, a);
  ub_put_int(&out, d);
  ub_put(&out, ' ');
  ub_put_ascii(&out, MONTHS[m - 1]);
  ub_put(&out, ' ');
  ub_put_n(&out, date.s, 4); // the year, as written
  return ub_finish(&out);
}

int words_of(U16 text) {
  int n = 0;
  int in_word = 0;
  for (size_t i = 0; i < text.n; i++) {
    if (is_js_space(text.s[i])) {
      in_word = 0;
    } else if (!in_word) {
      n++;
      in_word = 1;
    }
  }
  return n;
}

static int has_escapable(U16 text) {
  for (size_t i = 0; i < text.n; i++) {
    uint16_t c = text.s[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return 1;
  }
  return 0;
}

void escape_html_into(U16Buf *out, U16 text) {
  size_t run = 0;
  for (size_t i = 0; i < text.n; i++) {
    uint16_t c = text.s[i];
    const char *rep = NULL;
    switch (c) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = "&#39;"; break;
    }
    if (!rep) continue;
    if (i > run) ub_put_n(out, text.s + run, i - run);
    ub_put_ascii(out, rep);
    run = i + 1;
  }
  if (text.n > run) ub_put_n(out, text.s + run, text.n - run);
}

U16 escape_html(Arena *a, U16 text) {
  if (!has_escapable(text)) return text;
  U16Buf out;
  ub_init(&out, a);
  escape_html_into(&out, text);
  return ub_finish(&out);
}
