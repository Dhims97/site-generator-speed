// sitegen — native C port of the v1 solve.js (makefun-site 1.0.0 drop-in).
// All strings are UTF-16 code units (U16), exactly like JS strings: every
// comparison, slice and index is by code unit, so sorting and regex-equivalent
// behaviour match the reference byte for byte. Everything allocates from one
// bump arena; the process does a single run and exits, so nothing is freed.
#ifndef SITEGEN_H
#define SITEGEN_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

_Noreturn void die(const char *fmt, ...);

// ------------------------------------------------------------------ arena
typedef struct { uint8_t *base, *ptr, *end, *limit; } Arena; // end = committed, limit = reserved
void arena_init(Arena *a, size_t cap);
void *arena_alloc(Arena *a, size_t n, size_t align);
size_t arena_used(Arena *a); // bytes handed out so far (high-water mark)
#define ARENA_NEW(a, T) ((T *)arena_alloc((a), sizeof(T), _Alignof(T)))
#define ARENA_ARR(a, T, n) ((T *)arena_alloc((a), sizeof(T) * (n), _Alignof(T)))
// Grow an arena array: (arr) may be NULL, (n) is the used count, (cap) capacity.
#define AGROW(a, arr, n, cap, T) do { \
    if ((n) == (cap)) { size_t nc = (cap) ? (cap) * 2 : 8; \
      T *na = ARENA_ARR((a), T, nc); \
      if (arr) memcpy(na, (arr), sizeof(T) * (n)); \
      (arr) = na; (cap) = nc; } } while (0)

// ------------------------------------------------------------------ U16
// A JS string: UTF-16 code units, length in units. Views (slices) share storage.
typedef struct { const uint16_t *s; size_t n; } U16;
static inline U16 u16_make(const uint16_t *s, size_t n) { U16 v; v.s = s; v.n = n; return v; }
static inline U16 u16_empty(void) { U16 v; v.s = (const uint16_t *)""; v.n = 0; return v; }
int u16_cmp(U16 a, U16 b);              // code-unit order (JS <)
int u16_eq(U16 a, U16 b);
int u16_eq_ascii(U16 a, const char *ascii);
int u16_starts_with(U16 s, U16 p);
int u16_ends_with(U16 s, U16 suf);
long u16_index_of(U16 s, U16 needle, size_t from);       // -1 when absent
long u16_index_of_ch(U16 s, uint16_t c, size_t from);    // -1 when absent
long u16_last_index_of_ch(U16 s, uint16_t c, long from); // -1 when absent; from<0 = from end
U16 u16_slice(U16 s, size_t a, size_t b);                // [a,b), clamped
U16 u16_dup(Arena *a, U16 s);                            // owned copy (+NUL sentinel)
// ASCII helpers (inputs that are ASCII by construction)
U16 u16_from_ascii(Arena *a, const char *s);
int u16_is_ascii(U16 s);

// UTF-8 <-> UTF-16
size_t utf8_decode(const uint8_t *p, const uint8_t *end, uint32_t *cp); // bytes used, 0 = invalid
size_t utf8_encode(uint32_t cp, uint8_t out[4]);
uint32_t u16_codepoint_at(U16 s, size_t i, size_t *adv); // code point at unit i (surrogates paired)
int is_js_space(uint16_t c);        // JS \s set, by code unit
int is_combining_mark(uint32_t cp); // Unicode General Category M (slugify strip set)

// ------------------------------------------------------------------ builders
// Growable UTF-16 buffer (arena-backed; old storage is abandoned on growth).
typedef struct { uint16_t *s; size_t n, cap; Arena *a; } U16Buf;
void ub_init(U16Buf *b, Arena *a);
void ub_reserve(U16Buf *b, size_t extra); // out-of-line grow path
// Single-unit append, inlined: the reserve check is one branch, and the
// grow path is taken only when the buffer is actually full.
static inline void ub_put(U16Buf *b, uint16_t c) {
  if (b->n + 1 > b->cap) ub_reserve(b, 1);
  b->s[b->n++] = c;
}
void ub_put_n(U16Buf *b, const uint16_t *s, size_t n);
void ub_put_u16(U16Buf *b, U16 s);
void ub_put_ascii(U16Buf *b, const char *s);
void ub_put_int(U16Buf *b, int64_t v);
void ub_truncate(U16Buf *b, size_t n);
U16 ub_finish(U16Buf *b); // owned U16 (+NUL sentinel)

// Growable byte buffer (arena-backed), for answers.json.
typedef struct { uint8_t *s; size_t n, cap; Arena *a; } ByteBuf;
void bb_init(ByteBuf *b, Arena *a);
void bb_reserve(ByteBuf *b, size_t extra); // out-of-line grow path
static inline void bb_put(ByteBuf *b, uint8_t c) {
  if (b->n + 1 > b->cap) bb_reserve(b, 1);
  b->s[b->n++] = c;
}
void bb_put_n(ByteBuf *b, const void *s, size_t n);
void bb_put_ascii(ByteBuf *b, const char *s);
void bb_put_int(ByteBuf *b, int64_t v);
void bb_put_utf8_cp(ByteBuf *b, uint32_t cp);
void bb_put_json_string(ByteBuf *b, U16 s); // with quotes, JSON.stringify escaping

// ------------------------------------------------------------------ JSON
typedef enum { J_NULL, J_BOOL, J_INT, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
  JType t;
  union {
    int b;
    int64_t i;
    U16 s;
    struct { JVal **v; size_t n, cap; } a;
    struct { U16 *k; JVal **v; size_t n, cap; } o;
  } u;
};
JVal *json_parse(Arena *a, const uint8_t *data, size_t len); // dies on malformed input
JVal *jget(JVal *obj, const char *key); // NULL when missing / not an object
void json_write_value(ByteBuf *out, JVal *v); // JSON.stringify semantics

// ------------------------------------------------------------------ text
U16 slugify(Arena *a, U16 text);        // vendor text.js slugify, exact
U16 long_date(Arena *a, U16 date);      // "YYYY-MM-DD" -> "D Month YYYY"
int words_of(U16 text);                 // runs of non-whitespace (JS \s)
void escape_html_into(U16Buf *out, U16 text);
U16 escape_html(Arena *a, U16 text);

// ------------------------------------------------------------------ front matter
typedef struct {
  int has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
  U16 title, date, layout, slug, summary;
  int draft;
  U16 *tags; size_t ntags;
} FMData;
// Returns an empty U16 on success (fm + *body filled), else an arena error message.
U16 parse_front_matter(Arena *a, U16 text, FMData *fm, U16 *body);

// ------------------------------------------------------------------ markdown
typedef struct { int level; U16 id; U16 html; } TocItem;
typedef struct { U16 html; TocItem *toc; size_t ntoc; U16 excerpt; } MdResult;
typedef U16 (*LinkFn)(void *ctx, U16 target, Arena *a);
MdResult render_markdown(Arena *a, U16 src, LinkFn link, void *link_ctx);

// ------------------------------------------------------------------ templates
typedef enum { V_UNDEF, V_NULL, V_BOOL, V_INT, V_STR, V_ARR, V_OBJ } VType;
typedef struct Value Value;
typedef struct Obj Obj;
struct Obj { const char **names; Value *vals; size_t n, cap; Arena *a; };
struct Value {
  VType t;
  union {
    int b;
    int64_t i;
    U16 s;
    struct { Value *items; size_t n; } a;
    Obj *o;
  } u;
};
Value v_undef(void);
Value v_null(void);
Value v_bool(int b);
Value v_int(int64_t i);
Value v_str(U16 s);
Value v_arr(Value *items, size_t n);
Value v_obj(Obj *o);
Obj *obj_new(Arena *a);
void obj_put(Obj *o, const char *name, Value v); // replaces or appends
Value *obj_get(Obj *o, const char *name);        // NULL when absent
U16 value_text(Arena *a, Value v);               // textOf
int value_truthy(Value v);

typedef struct Scope Scope;
struct Scope { const char **names; Value *vals; size_t n, cap; Scope *parent; Arena *a; };
Scope *scope_new(Arena *a, Scope *parent);
void scope_set(Scope *s, const char *name, Value v);
Value lookup(Scope *s, char **path, size_t npath);

typedef enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE } NKind;
typedef struct TNode TNode;
struct TNode {
  NKind kind;
  U16 text;               // N_TEXT
  char **path; size_t npath; // N_VAR/N_RAW/N_IF/N_FOR (ASCII segments)
  int not;                // N_IF
  char *for_name;         // N_FOR (ASCII)
  TNode **body; size_t nbody;       // N_FOR
  TNode **then_b; size_t nthen;     // N_IF
  TNode **else_b; size_t nelse; int has_else; // N_IF
  U16 inc_name;           // N_INCLUDE
};
// Parses a template; returns NULL + *err (arena message) on TemplateError.
TNode **parse_template(Arena *a, U16 name, U16 src, size_t *nnodes, U16 *err);

typedef struct TplMap TplMap;
struct TplMap { U16 name; TNode **nodes; size_t nnodes; };
TNode **tpl_lookup(TplMap *maps, size_t n, U16 name, size_t *nnodes);
// Renders nodes into out; returns void. include depth starts at 0.
void render_nodes(Arena *a, TNode **nodes, size_t nnodes, Scope *scope,
                  TplMap *maps, size_t nmaps, int depth, U16Buf *out);

// ------------------------------------------------------------------ build
typedef struct Source Source;
typedef struct Tag Tag;
struct Tag { U16 name, slug, url; Source **posts; size_t nposts, cap_posts; };
struct Source {
  U16 path;
  U16 text;      // \r-normalised source text (.md only)
  FMData fm;
  U16 body;
  int post;
  U16 layout;
  int kept;
  U16 slug, out_dir, url;
  U16 html, excerpt;
  TocItem *toc; size_t ntoc;
  Source *newer, *older;
  Tag **tag_refs; size_t ntag_refs;
  Obj *card_obj; // memoised card
  Obj *page_obj; // memoised page
};
typedef struct { U16 path; U16 content; } OutEnt;
typedef struct { U16 path; U16 content; } StaticFile;
typedef struct {
  Arena *a;
  JVal *input;
  U16 base;
  TplMap *templates; size_t ntemplates;
  U16 *report; size_t nreport, cap_report;
  Source *sources; size_t nsources;       // kept sources, path order
  Source *all_sources; size_t nall_sources;
  StaticFile *statics; size_t nstatics, cap_statics;
  Source **posts; size_t nposts, cap_posts;
  Tag **tags; size_t ntags, cap_tags;
  U16 *broken; size_t nbroken, cap_broken; // insertion-ordered set
  OutEnt *out; size_t nout, cap_out;
} Build;
void build_run(Arena *a, JVal *input, OutEnt **out, size_t *nout);

#endif
