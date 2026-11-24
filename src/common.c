// common.c — arena, U16 strings, UTF-8, builders, JS whitespace set.
#include "sitegen.h"
#include <sys/mman.h>

// The arena is a bump allocator over a reserved mmap region: the base
// address NEVER moves (realloc would invalidate every U16 view and every
// U16Buf storage pointer into it), and pages are committed on demand.
#define ARENA_RESERVE (1u << 30)     // 1 GiB of virtual address space
#define ARENA_COMMIT (64u << 20)     // committed in 64 MiB chunks

static void arena_commit(Arena *a, size_t bytes) {
  if (mprotect(a->end, bytes, PROT_READ | PROT_WRITE) != 0) die("cannot commit arena");
  a->end += bytes;
}

void die(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fputs("sitegen: fatal: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

void arena_init(Arena *a, size_t cap) {
  (void)cap;
  a->base = mmap(NULL, ARENA_RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (a->base == MAP_FAILED) die("cannot reserve arena");
  a->ptr = a->base;
  a->end = a->base;
  a->limit = a->base + ARENA_RESERVE;
  arena_commit(a, ARENA_COMMIT);
}

void *arena_alloc(Arena *a, size_t n, size_t align) {
  uintptr_t p = (uintptr_t)a->ptr;
  uintptr_t aligned = (p + (align - 1)) & ~(uintptr_t)(align - 1);
  if (aligned + n > (uintptr_t)a->end) {
    size_t need = (aligned + n) - (uintptr_t)a->base;
    size_t target = (need + ARENA_COMMIT - 1) / ARENA_COMMIT * ARENA_COMMIT;
    if (a->base + target > a->limit) die("arena exhausted");
    arena_commit(a, target - (size_t)(a->end - a->base));
  }
  a->ptr = (uint8_t *)(aligned + n);
  return (void *)aligned;
}

size_t arena_used(Arena *a) { return (size_t)(a->ptr - a->base); }

// ------------------------------------------------------------------ U16
int u16_cmp(U16 a, U16 b) {
  size_t n = a.n < b.n ? a.n : b.n;
  for (size_t i = 0; i < n; i++) {
    if (a.s[i] != b.s[i]) return a.s[i] < b.s[i] ? -1 : 1;
  }
  if (a.n == b.n) return 0;
  return a.n < b.n ? -1 : 1;
}

int u16_eq(U16 a, U16 b) { return a.n == b.n && (a.n == 0 || memcmp(a.s, b.s, a.n * 2) == 0); }

int u16_eq_ascii(U16 a, const char *ascii) {
  size_t n = strlen(ascii);
  if (a.n != n) return 0;
  for (size_t i = 0; i < n; i++) if (a.s[i] != (uint8_t)ascii[i]) return 0;
  return 1;
}

int u16_starts_with(U16 s, U16 p) {
  return s.n >= p.n && (p.n == 0 || memcmp(s.s, p.s, p.n * 2) == 0);
}

int u16_ends_with(U16 s, U16 suf) {
  return s.n >= suf.n && (suf.n == 0 || memcmp(s.s + (s.n - suf.n), suf.s, suf.n * 2) == 0);
}

long u16_index_of(U16 s, U16 needle, size_t from) {
  if (needle.n == 0) return (long)(from <= s.n ? from : s.n);
  if (needle.n > s.n || from > s.n - needle.n) return -1;
  for (size_t i = from; i + needle.n <= s.n; i++) {
    if (s.s[i] == needle.s[0] && memcmp(s.s + i, needle.s, needle.n * 2) == 0) return (long)i;
  }
  return -1;
}

long u16_index_of_ch(U16 s, uint16_t c, size_t from) {
  for (size_t i = from; i < s.n; i++) if (s.s[i] == c) return (long)i;
  return -1;
}

long u16_last_index_of_ch(U16 s, uint16_t c, long from) {
  if (s.n == 0) return -1;
  long i = from < 0 ? (long)s.n - 1 : (from >= (long)s.n ? (long)s.n - 1 : from);
  for (; i >= 0; i--) if (s.s[i] == c) return i;
  return -1;
}

U16 u16_slice(U16 s, size_t a, size_t b) {
  if (a > s.n) a = s.n;
  if (b > s.n) b = s.n;
  if (b < a) b = a;
  return u16_make(s.s + a, b - a);
}

U16 u16_dup(Arena *a, U16 s) {
  uint16_t *p = ARENA_ARR(a, uint16_t, s.n + 1);
  if (s.n) memcpy(p, s.s, s.n * 2);
  p[s.n] = 0;
  return u16_make(p, s.n);
}

U16 u16_from_ascii(Arena *a, const char *s) {
  size_t n = strlen(s);
  uint16_t *p = ARENA_ARR(a, uint16_t, n + 1);
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)s[i];
  p[n] = 0;
  return u16_make(p, n);
}

int u16_is_ascii(U16 s) {
  for (size_t i = 0; i < s.n; i++) if (s.s[i] > 0x7F) return 0;
  return 1;
}

// ------------------------------------------------------------------ UTF-8
size_t utf8_decode(const uint8_t *p, const uint8_t *end, uint32_t *cp) {
  if (p >= end) return 0;
  uint8_t c = p[0];
  if (c < 0x80) { *cp = c; return 1; }
  if (c < 0xC2) return 0; // overlong / stray continuation
  if (c < 0xE0) {
    if (end - p < 2 || (p[1] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
    return 2;
  }
  if (c < 0xF0) {
    if (end - p < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if (*cp < 0x800) return 0; // overlong
    if (*cp >= 0xD800 && *cp <= 0xDFFF) return 0; // surrogate in UTF-8
    return 3;
  }
  if (c < 0xF5) {
    if (end - p < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    if (*cp < 0x10000 || *cp > 0x10FFFF) return 0;
    return 4;
  }
  return 0;
}

size_t utf8_encode(uint32_t cp, uint8_t out[4]) {
  if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
  if (cp < 0x800) {
    out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2;
  }
  if (cp < 0x10000) {
    out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3;
  }
  out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
  out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); return 4;
}

uint32_t u16_codepoint_at(U16 s, size_t i, size_t *adv) {
  uint16_t c = s.s[i];
  if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.n) {
    uint16_t d = s.s[i + 1];
    if (d >= 0xDC00 && d <= 0xDFFF) {
      *adv = 2;
      return 0x10000u + ((uint32_t)(c - 0xD800) << 10) + (d - 0xDC00);
    }
  }
  *adv = 1;
  return c;
}

// JS \s, by code unit: White_Space + line terminators.
int is_js_space(uint16_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 ||
         c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// Unicode General Category M, as a sorted range table for binary
// search (the same set the reference's /\p{M}/u strips in slugify).
static const struct { uint32_t lo, hi; } MARK_RANGES[] = {
  {0x300, 0x36F},
  {0x483, 0x489},
  {0x591, 0x5BD},
  {0x5BF, 0x5BF},
  {0x5C1, 0x5C2},
  {0x5C4, 0x5C7},
  {0x610, 0x61A},
  {0x64B, 0x65F},
  {0x670, 0x670},
  {0x6D6, 0x6DC},
  {0x6DF, 0x6E4},
  {0x6E7, 0x6E8},
  {0x6EA, 0x6ED},
  {0x711, 0x711},
  {0x730, 0x74A},
  {0x7A6, 0x7B0},
  {0x7EB, 0x7F3},
  {0x816, 0x819},
  {0x81B, 0x823},
  {0x825, 0x827},
  {0x829, 0x82D},
  {0x859, 0x85B},
  {0x8D3, 0x902},
  {0x93A, 0x93C},
  {0x941, 0x948},
  {0x94D, 0x94D},
  {0x951, 0x957},
  {0x962, 0x963},
  {0x981, 0x981},
  {0x9BC, 0x9BC},
  {0x9C1, 0x9C4},
  {0x9CD, 0x9CD},
  {0x9E2, 0x9E3},
  {0xA01, 0xA02},
  {0xA3C, 0xA3C},
  {0xA41, 0xA42},
  {0xA47, 0xA48},
  {0xA4B, 0xA4D},
  {0xA51, 0xA51},
  {0xA70, 0xA71},
  {0xA75, 0xA75},
  {0xA81, 0xA82},
  {0xABC, 0xABC},
  {0xAC1, 0xAC5},
  {0xAC7, 0xAC8},
  {0xACD, 0xACD},
  {0xAE2, 0xAE3},
  {0xB01, 0xB01},
  {0xB3C, 0xB3C},
  {0xB3F, 0xB3F},
  {0xB41, 0xB44},
  {0xB4D, 0xB4D},
  {0xB56, 0xB56},
  {0xB62, 0xB63},
  {0xB82, 0xB82},
  {0xBC0, 0xBC0},
  {0xBCD, 0xBCD},
  {0xC00, 0xC00},
  {0xC3E, 0xC40},
  {0xC46, 0xC48},
  {0xC4A, 0xC4D},
  {0xC55, 0xC56},
  {0xC62, 0xC63},
  {0xC81, 0xC81},
  {0xCBC, 0xCBC},
  {0xCBF, 0xCBF},
  {0xCC6, 0xCC6},
  {0xCCC, 0xCCD},
  {0xCE2, 0xCE3},
  {0xD01, 0xD01},
  {0xD3B, 0xD3C},
  {0xD41, 0xD44},
  {0xD4D, 0xD4D},
  {0xD62, 0xD63},
  {0xDCA, 0xDCA},
  {0xDD2, 0xDD4},
  {0xDD6, 0xDD6},
  {0xE31, 0xE31},
  {0xE34, 0xE3A},
  {0xE47, 0xE4E},
  {0xEB1, 0xEB1},
  {0xEB4, 0xEB9},
  {0xEBB, 0xEBC},
  {0xEC8, 0xECD},
  {0xF18, 0xF19},
  {0xF35, 0xF35},
  {0xF37, 0xF37},
  {0xF39, 0xF39},
  {0xF71, 0xF7E},
  {0xF80, 0xF84},
  {0xF86, 0xF87},
  {0xF8D, 0xF97},
  {0xF99, 0xFBC},
  {0xFC6, 0xFC6},
  {0x102D, 0x1030},
  {0x1032, 0x1037},
  {0x1039, 0x103A},
  {0x103D, 0x103E},
  {0x1058, 0x1059},
  {0x105E, 0x1060},
  {0x1071, 0x1074},
  {0x1082, 0x1082},
  {0x1085, 0x1086},
  {0x108D, 0x108D},
  {0x109D, 0x109D},
  {0x1160, 0x11FF},
  {0x135D, 0x135F},
  {0x1712, 0x1714},
  {0x1732, 0x1734},
  {0x1752, 0x1753},
  {0x1772, 0x1773},
  {0x17B4, 0x17B5},
  {0x17B7, 0x17BD},
  {0x17C6, 0x17C6},
  {0x17C9, 0x17D3},
  {0x17DD, 0x17DD},
  {0x180B, 0x180E},
  {0x18A9, 0x18A9},
  {0x1920, 0x1922},
  {0x1927, 0x1928},
  {0x1932, 0x1932},
  {0x1939, 0x193B},
  {0x1A17, 0x1A18},
  {0x1A1B, 0x1A1B},
  {0x1A56, 0x1A56},
  {0x1A58, 0x1A5E},
  {0x1A60, 0x1A60},
  {0x1A62, 0x1A62},
  {0x1A65, 0x1A6C},
  {0x1A73, 0x1A7C},
  {0x1A7F, 0x1A7F},
  {0x1AB0, 0x1AC0},
  {0x1B00, 0x1B03},
  {0x1B34, 0x1B34},
  {0x1B36, 0x1B3A},
  {0x1B3C, 0x1B3C},
  {0x1B42, 0x1B42},
  {0x1B6B, 0x1B73},
  {0x1B80, 0x1B81},
  {0x1BA2, 0x1BA5},
  {0x1BA8, 0x1BA9},
  {0x1BAB, 0x1BAD},
  {0x1BE6, 0x1BE6},
  {0x1BE8, 0x1BE9},
  {0x1BED, 0x1BED},
  {0x1BEF, 0x1BF1},
  {0x1C2C, 0x1C33},
  {0x1C36, 0x1C37},
  {0x1CD0, 0x1CD2},
  {0x1CD4, 0x1CE0},
  {0x1CE2, 0x1CE8},
  {0x1CED, 0x1CED},
  {0x1CF4, 0x1CF4},
  {0x1CF8, 0x1CF9},
  {0x1DC0, 0x1DF5},
  {0x1DFC, 0x1DFF},
  {0x200C, 0x200D},
  {0x20D0, 0x20F0},
  {0x2CEF, 0x2CF1},
  {0x2D7F, 0x2D7F},
  {0x2DE0, 0x2DFF},
  {0x302A, 0x302D},
  {0x3099, 0x309A},
  {0xA66F, 0xA672},
  {0xA674, 0xA67D},
  {0xA69E, 0xA69F},
  {0xA6F0, 0xA6F1},
  {0xA802, 0xA802},
  {0xA806, 0xA806},
  {0xA80B, 0xA80B},
  {0xA825, 0xA826},
  {0xA8C4, 0xA8C4},
  {0xA8E0, 0xA8F1},
  {0xA926, 0xA92D},
  {0xA947, 0xA951},
  {0xA980, 0xA982},
  {0xA9B3, 0xA9B3},
  {0xA9B6, 0xA9B9},
  {0xA9BC, 0xA9BC},
  {0xA9E5, 0xA9E5},
  {0xAA29, 0xAA2E},
  {0xAA31, 0xAA32},
  {0xAA35, 0xAA36},
  {0xAA43, 0xAA43},
  {0xAA4C, 0xAA4C},
  {0xAA7C, 0xAA7C},
  {0xAAB0, 0xAAB0},
  {0xAAB2, 0xAAB4},
  {0xAAB7, 0xAAB8},
  {0xAABE, 0xAABF},
  {0xAAC1, 0xAAC1},
  {0xAAEC, 0xAAED},
  {0xAAF6, 0xAAF6},
  {0xABE5, 0xABE5},
  {0xABE8, 0xABE8},
  {0xABED, 0xABED},
  {0xFB1E, 0xFB1E},
  {0xFE00, 0xFE0F},
  {0xFE20, 0xFE2F},
  {0x101FD, 0x101FD},
  {0x102E0, 0x102E0},
  {0x10376, 0x1037A},
  {0x10A01, 0x10A03},
  {0x10A05, 0x10A06},
  {0x10A0C, 0x10A0F},
  {0x10A38, 0x10A3A},
  {0x10A3F, 0x10A3F},
  {0x10AE5, 0x10AE6},
  {0x11001, 0x11001},
  {0x11038, 0x11046},
  {0x1107F, 0x11081},
  {0x110B3, 0x110B6},
  {0x110B9, 0x110BA},
  {0x11100, 0x11102},
  {0x11127, 0x1112B},
  {0x1112D, 0x11134},
  {0x11173, 0x11173},
  {0x11180, 0x11181},
  {0x111B6, 0x111BE},
  {0x111CA, 0x111CC},
  {0x1122F, 0x11231},
  {0x11234, 0x11234},
  {0x11236, 0x11237},
  {0x112DF, 0x112DF},
  {0x112E3, 0x112EA},
  {0x11300, 0x11301},
  {0x1133C, 0x1133C},
  {0x11340, 0x11340},
  {0x11366, 0x1136C},
  {0x11370, 0x11374},
  {0x11438, 0x1143F},
  {0x11442, 0x11444},
  {0x11446, 0x11446},
  {0x114B3, 0x114B8},
  {0x114BA, 0x114BA},
  {0x114BF, 0x114C0},
  {0x114C2, 0x114C3},
  {0x115B2, 0x115B5},
  {0x115BC, 0x115BD},
  {0x115BF, 0x115C0},
  {0x115DC, 0x115DD},
  {0x11633, 0x1163A},
  {0x1163D, 0x1163D},
  {0x1163F, 0x11640},
  {0x116AB, 0x116AB},
  {0x116AD, 0x116AD},
  {0x116B0, 0x116B5},
  {0x116B7, 0x116B7},
  {0x1171D, 0x1171F},
  {0x11722, 0x11725},
  {0x11727, 0x1172B},
  {0x1182F, 0x11837},
  {0x11839, 0x1183A},
  {0x1193B, 0x1193C},
  {0x1193E, 0x1193E},
  {0x11943, 0x11943},
  {0x119D4, 0x119D7},
  {0x119DA, 0x119DB},
  {0x119E0, 0x119E0},
  {0x11A01, 0x11A0A},
  {0x11A33, 0x11A38},
  {0x11A3B, 0x11A3E},
  {0x11A47, 0x11A47},
  {0x11A51, 0x11A56},
  {0x11A59, 0x11A5B},
  {0x11A8A, 0x11A96},
  {0x11A98, 0x11A99},
  {0x11C30, 0x11C36},
  {0x11C38, 0x11C3D},
  {0x11C3F, 0x11C3F},
  {0x11C92, 0x11CA7},
  {0x11CAA, 0x11CB0},
  {0x11CB2, 0x11CB3},
  {0x11CB5, 0x11CB6},
  {0x16AF0, 0x16AF4},
  {0x16B30, 0x16B36},
  {0x16F8F, 0x16F92},
  {0x1BC9D, 0x1BC9E},
  {0x1BCA0, 0x1BCA3},
  {0x1D167, 0x1D169},
  {0x1D173, 0x1D182},
  {0x1D185, 0x1D18B},
  {0x1D1AA, 0x1D1AD},
  {0x1D242, 0x1D244},
  {0x1DA00, 0x1DA36},
  {0x1DA3B, 0x1DA6C},
  {0x1DA75, 0x1DA75},
  {0x1DA84, 0x1DA84},
  {0x1DA9B, 0x1DA9F},
  {0x1DAA1, 0x1DAAF},
  {0x1E000, 0x1E006},
  {0x1E008, 0x1E018},
  {0x1E01B, 0x1E021},
  {0x1E023, 0x1E024},
  {0x1E026, 0x1E02A},
  {0x1E8D0, 0x1E8D6},
  {0x1E944, 0x1E94A},
  {0xE0100, 0xE01EF},
};

int is_combining_mark(uint32_t cp) {
  size_t lo = 0, hi = sizeof(MARK_RANGES) / sizeof(MARK_RANGES[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < MARK_RANGES[mid].lo) hi = mid;
    else if (cp > MARK_RANGES[mid].hi) lo = mid + 1;
    else return 1;
  }
  return 0;
}

// ------------------------------------------------------------------ U16Buf
void ub_init(U16Buf *b, Arena *a) { b->a = a; b->s = NULL; b->n = 0; b->cap = 0; }

void ub_reserve(U16Buf *b, size_t extra) {
  if (b->n + extra > b->cap) {
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->n + extra) nc *= 2;
    uint16_t *ns = ARENA_ARR(b->a, uint16_t, nc);
    if (b->s) memcpy(ns, b->s, b->n * 2);
    b->s = ns;
    b->cap = nc;
  }
}

void ub_put_n(U16Buf *b, const uint16_t *s, size_t n) {
  if (n == 0) return;
  ub_reserve(b, n);
  memcpy(b->s + b->n, s, n * 2);
  b->n += n;
}

void ub_put_u16(U16Buf *b, U16 s) { ub_put_n(b, s.s, s.n); }

void ub_put_ascii(U16Buf *b, const char *s) {
  while (*s) ub_put(b, (uint8_t)*s++);
}

void ub_put_int(U16Buf *b, int64_t v) {
  char tmp[24];
  int len = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
  for (int i = 0; i < len; i++) ub_put(b, (uint8_t)tmp[i]);
}

void ub_truncate(U16Buf *b, size_t n) { if (n < b->n) b->n = n; }

U16 ub_finish(U16Buf *b) {
  ub_reserve(b, 1);
  b->s[b->n] = 0;
  return u16_make(b->s, b->n);
}

// ------------------------------------------------------------------ ByteBuf
void bb_init(ByteBuf *b, Arena *a) { b->a = a; b->s = NULL; b->n = 0; b->cap = 0; }

void bb_reserve(ByteBuf *b, size_t extra) {
  if (b->n + extra > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->n + extra) nc *= 2;
    uint8_t *ns = ARENA_ARR(b->a, uint8_t, nc);
    if (b->s) memcpy(ns, b->s, b->n);
    b->s = ns;
    b->cap = nc;
  }
}

void bb_put_n(ByteBuf *b, const void *s, size_t n) {
  if (n == 0) return;
  bb_reserve(b, n);
  memcpy(b->s + b->n, s, n);
  b->n += n;
}

void bb_put_ascii(ByteBuf *b, const char *s) { bb_put_n(b, s, strlen(s)); }

void bb_put_int(ByteBuf *b, int64_t v) {
  char tmp[24];
  int len = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
  bb_put_n(b, tmp, (size_t)len);
}

void bb_put_utf8_cp(ByteBuf *b, uint32_t cp) {
  uint8_t out[4];
  bb_put_n(b, out, utf8_encode(cp, out));
}

// JSON.stringify string escaping, with surrounding quotes. Lone surrogates
// are escaped as \uXXXX, exactly like well-formed JSON.stringify.
// Run-based: plain runs are written as UTF-8 directly (one reserve per
// run), so a 7 MB answers.json costs a few thousand calls, not millions.
void bb_put_json_string(ByteBuf *b, U16 s) {
  static const char HEX[] = "0123456789abcdef";
  bb_put(b, '"');
  size_t i = 0;
  while (i < s.n) {
    // Find the next unit that needs an escape.
    size_t j = i;
    while (j < s.n) {
      uint16_t c = s.s[j];
      if (c == '"' || c == '\\' || c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) break;
      j++;
    }
    // Write the plain run s[i..j) as UTF-8, directly into the buffer.
    if (j > i) {
      bb_reserve(b, (j - i) * 3); // worst case: 3 bytes per unit
      for (size_t k = i; k < j; k++) {
        uint16_t c = s.s[k];
        if (c < 0x80) {
          b->s[b->n++] = (uint8_t)c;
        } else if (c < 0x800) {
          b->s[b->n++] = (uint8_t)(0xC0 | (c >> 6));
          b->s[b->n++] = (uint8_t)(0x80 | (c & 0x3F));
        } else {
          b->s[b->n++] = (uint8_t)(0xE0 | (c >> 12));
          b->s[b->n++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
          b->s[b->n++] = (uint8_t)(0x80 | (c & 0x3F));
        }
      }
    }
    if (j >= s.n) break;
    uint16_t c = s.s[j];
    if (c == '"') bb_put_n(b, "\\\"", 2);
    else if (c == '\\') bb_put_n(b, "\\\\", 2);
    else if (c == '\b') bb_put_n(b, "\\b", 2);
    else if (c == '\f') bb_put_n(b, "\\f", 2);
    else if (c == '\n') bb_put_n(b, "\\n", 2);
    else if (c == '\r') bb_put_n(b, "\\r", 2);
    else if (c == '\t') bb_put_n(b, "\\t", 2);
    else if (c < 0x20) {
      char esc[6] = {'\\', 'u', '0', '0', HEX[(c >> 4) & 0xF], HEX[c & 0xF]};
      bb_put_n(b, esc, 6);
    } else if (c >= 0xD800 && c <= 0xDBFF) {
      // High surrogate: pair with a following low surrogate, else escape lone.
      if (j + 1 < s.n && s.s[j + 1] >= 0xDC00 && s.s[j + 1] <= 0xDFFF) {
        uint32_t cp = 0x10000u + ((uint32_t)(c - 0xD800) << 10) + (s.s[j + 1] - 0xDC00);
        bb_put_utf8_cp(b, cp);
        j++;
      } else {
        char esc[6] = {'\\', 'u', HEX[(c >> 12) & 0xF], HEX[(c >> 8) & 0xF], HEX[(c >> 4) & 0xF], HEX[c & 0xF]};
        bb_put_n(b, esc, 6);
      }
    } else {
      // Lone low surrogate.
      char esc[6] = {'\\', 'u', HEX[(c >> 12) & 0xF], HEX[(c >> 8) & 0xF], HEX[(c >> 4) & 0xF], HEX[c & 0xF]};
      bb_put_n(b, esc, 6);
    }
    i = j + 1;
  }
  bb_put(b, '"');
}
