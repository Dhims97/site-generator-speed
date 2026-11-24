// tpl.c — the template dialect, ported from v1's solve.js (tokenize,
// parseTemplate, lookup, textOf, truthy, renderNodes). All error strings
// are byte-identical to the reference's TemplateError messages.
#include "sitegen.h"

// ------------------------------------------------------------------ values
Value v_undef(void) { Value v; v.t = V_UNDEF; return v; }
Value v_null(void) { Value v; v.t = V_NULL; return v; }
Value v_bool(int b) { Value v; v.t = V_BOOL; v.u.b = !!b; return v; }
Value v_int(int64_t i) { Value v; v.t = V_INT; v.u.i = i; return v; }
Value v_str(U16 s) { Value v; v.t = V_STR; v.u.s = s; return v; }
Value v_arr(Value *items, size_t n) { Value v; v.t = V_ARR; v.u.a.items = items; v.u.a.n = n; return v; }
Value v_obj(Obj *o) { Value v; v.t = V_OBJ; v.u.o = o; return v; }

Obj *obj_new(Arena *a) {
  Obj *o = ARENA_NEW(a, Obj);
  o->a = a;
  o->n = 0;
  o->cap = 8;
  o->names = ARENA_ARR(a, const char *, o->cap);
  o->vals = ARENA_ARR(a, Value, o->cap);
  return o;
}

// Parallel-array growth: names and vals share one capacity.
static void obj_grow(Obj *o);
static void scope_grow(Scope *s);

void obj_put(Obj *o, const char *name, Value v) {
  for (size_t i = 0; i < o->n; i++) {
    if (strcmp(o->names[i], name) == 0) { o->vals[i] = v; return; }
  }
  if (o->n == o->cap) obj_grow(o);
  o->names[o->n] = name;
  o->vals[o->n] = v;
  o->n++;
}

Value *obj_get(Obj *o, const char *name) {
  if (!o) return NULL;
  for (size_t i = 0; i < o->n; i++) {
    if (strcmp(o->names[i], name) == 0) return &o->vals[i];
  }
  return NULL;
}

Scope *scope_new(Arena *a, Scope *parent) {
  Scope *s = ARENA_NEW(a, Scope);
  s->a = a;
  s->names = NULL;
  s->vals = NULL;
  s->n = 0;
  s->cap = 0;
  s->parent = parent;
  return s;
}

static Value *scope_get(Scope *s, const char *name) {
  for (size_t i = 0; i < s->n; i++) {
    if (strcmp(s->names[i], name) == 0) return &s->vals[i];
  }
  return NULL;
}

void scope_set(Scope *s, const char *name, Value v) {
  Value *old = scope_get(s, name);
  if (old) { *old = v; return; }
  if (s->n == s->cap) scope_grow(s);
  s->names[s->n] = name;
  s->vals[s->n] = v;
  s->n++;
}

static void obj_grow(Obj *o) {
  size_t nc = o->cap ? o->cap * 2 : 8;
  const char **nn = ARENA_ARR(o->a, const char *, nc);
  Value *nv = ARENA_ARR(o->a, Value, nc);
  if (o->n) {
    memcpy(nn, o->names, sizeof(const char *) * o->n);
    memcpy(nv, o->vals, sizeof(Value) * o->n);
  }
  o->names = nn;
  o->vals = nv;
  o->cap = nc;
}

static void scope_grow(Scope *s) {
  size_t nc = s->cap ? s->cap * 2 : 8;
  const char **nn = ARENA_ARR(s->a, const char *, nc);
  Value *nv = ARENA_ARR(s->a, Value, nc);
  if (s->n) {
    memcpy(nn, s->names, sizeof(const char *) * s->n);
    memcpy(nv, s->vals, sizeof(Value) * s->n);
  }
  s->names = nn;
  s->vals = nv;
  s->cap = nc;
}

// lookup(scope, path): walks up the scope chain for the first segment, then
// descends through objects — and, exactly like the reference's
// Object.hasOwn on an array, through an array's 'length' or a canonical
// index. Returns V_UNDEF when any step is missing.
Value lookup(Scope *s, char **path, size_t npath) {
  Scope *cur = s;
  Value *v = NULL;
  while (cur) {
    v = scope_get(cur, path[0]);
    if (v) break;
    cur = cur->parent;
  }
  if (!cur) return v_undef();
  Value cur_v = *v;
  for (size_t k = 1; k < npath; k++) {
    if (cur_v.t == V_OBJ) {
      Value *next = obj_get(cur_v.u.o, path[k]);
      if (!next) return v_undef();
      cur_v = *next;
    } else if (cur_v.t == V_ARR) {
      if (strcmp(path[k], "length") == 0) {
        cur_v = v_int((int64_t)cur_v.u.a.n);
      } else {
        const char *p = path[k];
        if (!*p) return v_undef();
        int all_digits = 1;
        for (const char *q = p; *q; q++) {
          if (*q < '0' || *q > '9') { all_digits = 0; break; }
        }
        if (!all_digits) return v_undef();
        if (p[0] == '0' && p[1] != 0) return v_undef(); // '01' is not an index
        long idx = strtol(p, NULL, 10);
        if (idx < 0 || (size_t)idx >= cur_v.u.a.n) return v_undef();
        cur_v = cur_v.u.a.items[idx];
      }
    } else {
      return v_undef();
    }
  }
  return cur_v;
}

U16 value_text(Arena *a, Value v) {
  switch (v.t) {
    case V_STR: return v.u.s;
    case V_INT: {
      U16Buf b;
      ub_init(&b, a);
      ub_put_int(&b, v.u.i);
      return ub_finish(&b);
    }
    case V_BOOL: return u16_from_ascii(a, v.u.b ? "true" : "false");
    case V_ARR: {
      U16Buf b;
      ub_init(&b, a);
      for (size_t i = 0; i < v.u.a.n; i++) {
        if (i) ub_put_ascii(&b, ", ");
        ub_put_u16(&b, value_text(a, v.u.a.items[i]));
      }
      return ub_finish(&b);
    }
    default: return u16_empty(); // undefined, null, object
  }
}

int value_truthy(Value v) {
  switch (v.t) {
    case V_ARR: return v.u.a.n > 0;
    case V_UNDEF: case V_NULL: return 0;
    case V_BOOL: return v.u.b;
    case V_STR: return v.u.s.n > 0;
    case V_INT: return v.u.i != 0;
    case V_OBJ: return 1;
  }
  return 0;
}

// ------------------------------------------------------------------ errors
// `template "NAME": <msg>`
static U16 tpl_err(Arena *a, U16 name, U16 msg) {
  U16Buf out;
  ub_init(&out, a);
  ub_put_ascii(&out, "template \"");
  ub_put_u16(&out, name);
  ub_put_ascii(&out, "\": ");
  ub_put_u16(&out, msg);
  return ub_finish(&out);
}

static U16 tpl_err_ascii(Arena *a, U16 name, const char *msg) {
  return tpl_err(a, name, u16_from_ascii(a, msg));
}

// JSON.stringify of a string, appended to a U16Buf (quotes included).
static void ub_put_json_quoted(U16Buf *out, U16 s) {
  static const char HEX[] = "0123456789abcdef";
  ub_put(out, '"');
  for (size_t i = 0; i < s.n; i++) {
    uint16_t c = s.s[i];
    if (c == '"') ub_put_ascii(out, "\\\"");
    else if (c == '\\') ub_put_ascii(out, "\\\\");
    else if (c == '\b') ub_put_ascii(out, "\\b");
    else if (c == '\f') ub_put_ascii(out, "\\f");
    else if (c == '\n') ub_put_ascii(out, "\\n");
    else if (c == '\r') ub_put_ascii(out, "\\r");
    else if (c == '\t') ub_put_ascii(out, "\\t");
    else if (c < 0x20) {
      ub_put_ascii(out, "\\u00");
      ub_put(out, HEX[(c >> 4) & 0xF]);
      ub_put(out, HEX[c & 0xF]);
    } else if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < s.n && s.s[i + 1] >= 0xDC00 && s.s[i + 1] <= 0xDFFF) {
        ub_put(out, c);
        ub_put(out, s.s[i + 1]);
        i++;
      } else {
        ub_put_ascii(out, "\\u");
        ub_put(out, HEX[(c >> 12) & 0xF]);
        ub_put(out, HEX[(c >> 8) & 0xF]);
        ub_put(out, HEX[(c >> 4) & 0xF]);
        ub_put(out, HEX[c & 0xF]);
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      ub_put_ascii(out, "\\u");
      ub_put(out, HEX[(c >> 12) & 0xF]);
      ub_put(out, HEX[(c >> 8) & 0xF]);
      ub_put(out, HEX[(c >> 4) & 0xF]);
      ub_put(out, HEX[c & 0xF]);
    } else {
      ub_put(out, c);
    }
  }
  ub_put(out, '"');
}

static U16 trim_u16(U16 s) {
  size_t a = 0, b = s.n;
  while (a < b && is_js_space(s.s[a])) a++;
  while (b > a && is_js_space(s.s[b - 1])) b--;
  return u16_slice(s, a, b);
}

static int is_ident_start(uint16_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_ident_char(uint16_t c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

// pathOf: validates T_PATH after trimming; on success fills *out_path with
// arena NUL-terminated ASCII segments. Returns 1, or 0 with *err set.
static int path_of(Arena *a, U16 name, U16 text, char ***out_path, size_t *out_n, U16 *err) {
  U16 t = trim_u16(text);
  // T_PATH: [A-Za-z_][A-Za-z0-9_]* (.[A-Za-z_][A-Za-z0-9_]*)*
  int ok = t.n > 0;
  if (ok) {
    size_t i = 0;
    for (;;) {
      if (i >= t.n || !is_ident_start(t.s[i])) { ok = 0; break; }
      i++;
      while (i < t.n && is_ident_char(t.s[i])) i++;
      if (i == t.n) break;
      if (t.s[i] != '.') { ok = 0; break; }
      i++;
      if (i >= t.n) { ok = 0; break; } // trailing '.'
    }
  }
  if (!ok) {
    U16Buf out;
    ub_init(&out, a);
    ub_put_ascii(&out, "template \"");
    ub_put_u16(&out, name);
    ub_put_ascii(&out, "\": ");
    ub_put_json_quoted(&out, t);
    ub_put_ascii(&out, " is not a path");
    *err = ub_finish(&out);
    return 0;
  }
  // Split on '.' into arena C strings.
  size_t nseg = 1;
  for (size_t i = 0; i < t.n; i++) if (t.s[i] == '.') nseg++;
  char **segs = ARENA_ARR(a, char *, nseg);
  size_t si = 0, start = 0;
  for (size_t i = 0; i <= t.n; i++) {
    if (i == t.n || t.s[i] == '.') {
      size_t len = i - start;
      char *s = ARENA_ARR(a, char, len + 1);
      for (size_t k = 0; k < len; k++) s[k] = (char)t.s[start + k];
      s[len] = 0;
      segs[si++] = s;
      start = i + 1;
    }
  }
  *out_path = segs;
  *out_n = nseg;
  return 1;
}

// ------------------------------------------------------------------ tokenizer
typedef enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG } TokKind;
typedef struct {
  TokKind kind;
  U16 text;              // TK_TEXT: the literal text
  char **path; size_t npath; // TK_VAR/TK_RAW
  U16 tag;               // TK_TAG: trimmed tag text
} Tok;

static int all_space_tab(U16 s) {
  for (size_t i = 0; i < s.n; i++) {
    if (s.s[i] != ' ' && s.s[i] != '\t') return 0;
  }
  return 1;
}

// Tokenizes a template; returns 1, or 0 with *err set (TemplateError).
static int tokenize(Arena *a, U16 name, U16 src, Tok **out_toks, size_t *out_n, U16 *err) {
  Tok *toks = NULL;
  size_t nt = 0, cap = 0;
  U16Buf text;
  ub_init(&text, a);
  size_t i = 0, n = src.n;
  while (i < n) {
    long open = u16_index_of_ch(src, '{', i);
    if (open == -1 || (size_t)open == n - 1) {
      ub_put_u16(&text, u16_slice(src, i, n));
      break;
    }
    uint16_t next = src.s[open + 1];
    if (next != '{' && next != '%') {
      ub_put_u16(&text, u16_slice(src, i, (size_t)open + 1));
      i = (size_t)open + 1;
      continue;
    }
    ub_put_u16(&text, u16_slice(src, i, (size_t)open));
    if (next == '{') {
      int raw = (size_t)open + 2 < n && src.s[open + 2] == '{';
      size_t from = (size_t)open + (raw ? 3 : 2);
      // indexOf('}}}' / '}}', from)
      long close = -1;
      for (size_t j = from; j + (raw ? 3u : 2u) <= n; j++) {
        if (raw) {
          if (src.s[j] == '}' && src.s[j + 1] == '}' && src.s[j + 2] == '}') { close = (long)j; break; }
        } else {
          if (src.s[j] == '}' && src.s[j + 1] == '}') { close = (long)j; break; }
        }
      }
      if (close == -1) {
        *err = tpl_err_ascii(a, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        return 0;
      }
      if (text.n > 0) {
        AGROW(a, toks, nt, cap, Tok);
        toks[nt].kind = TK_TEXT;
        toks[nt].text = ub_finish(&text);
        ub_init(&text, a);
        nt++;
      }
      char **path = NULL;
      size_t npath = 0;
      if (!path_of(a, name, u16_slice(src, from, (size_t)close), &path, &npath, err)) return 0;
      AGROW(a, toks, nt, cap, Tok);
      toks[nt].kind = raw ? TK_RAW : TK_VAR;
      toks[nt].path = path;
      toks[nt].npath = npath;
      nt++;
      i = (size_t)close + (raw ? 3 : 2);
      continue;
    }
    // '{%'
    long close = -1;
    for (size_t j = (size_t)open + 2; j + 2 <= n; j++) {
      if (src.s[j] == '%' && src.s[j + 1] == '}') { close = (long)j; break; }
    }
    if (close == -1) {
      *err = tpl_err_ascii(a, name, "a {% is not closed");
      return 0;
    }
    size_t end = (size_t)close + 2;
    // lineStart = lastIndexOf('\n', open-1) + 1; with open == 0 the JS
    // call clamps to position 0, which cannot be '\n' (it is '{'), so -1.
    long ls = open == 0 ? -1 : u16_last_index_of_ch(src, '\n', (long)open - 1);
    size_t line_start = (size_t)(ls + 1);
    long nl = u16_index_of_ch(src, '\n', end);
    size_t line_end = nl == -1 ? n : (size_t)nl;
    int standalone = all_space_tab(u16_slice(src, line_start, (size_t)open)) &&
                     all_space_tab(u16_slice(src, end, line_end));
    if (standalone) {
      // Remove the whitespace before the tag on this line from the text.
      text.n -= (size_t)open - line_start;
    }
    if (text.n > 0) {
      AGROW(a, toks, nt, cap, Tok);
      toks[nt].kind = TK_TEXT;
      toks[nt].text = ub_finish(&text);
      ub_init(&text, a);
      nt++;
    }
    AGROW(a, toks, nt, cap, Tok);
    toks[nt].kind = TK_TAG;
    toks[nt].tag = trim_u16(u16_slice(src, (size_t)open + 2, (size_t)close));
    nt++;
    i = standalone ? (nl == -1 ? n : (size_t)nl + 1) : end;
  }
  if (text.n > 0) {
    AGROW(a, toks, nt, cap, Tok);
    toks[nt].kind = TK_TEXT;
    toks[nt].text = ub_finish(&text);
    nt++;
  }
  *out_toks = toks;
  *out_n = nt;
  return 1;
}

// ------------------------------------------------------------------ parser
typedef struct { TNode **items; size_t n, cap; } Body;
static void body_push(Arena *a, Body *b, TNode *node) {
  AGROW(a, b->items, b->n, b->cap, TNode *);
  b->items[b->n++] = node;
}

static TNode *node_new(Arena *a, NKind kind) {
  TNode *n = ARENA_NEW(a, TNode);
  memset(n, 0, sizeof *n);
  n->kind = kind;
  return n;
}

// A stack frame: the node being built (NULL for the root) and the body its
// children are currently appended to. For an if frame the body switches
// between then_b and else_b when an else is seen.
typedef struct {
  TNode *node;
  Body *body;
} Frame;

// Matches T_FOR /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/ on a tag.
// Fills *name (arena C string) and the path text (untrimmed remainder).
static int tag_for(U16 tag, U16 *name_out, U16 *path_out) {
  // 'for ' + ident + ' in ' + \S+
  if (tag.n < 4 || tag.s[0] != 'f' || tag.s[1] != 'o' || tag.s[2] != 'r' || tag.s[3] != ' ') return 0;
  size_t i = 4;
  if (i >= tag.n || !is_ident_start(tag.s[i])) return 0;
  size_t start = i;
  i++;
  while (i < tag.n && is_ident_char(tag.s[i])) i++;
  *name_out = u16_slice(tag, start, i);
  // ' in '
  if (i + 4 > tag.n || tag.s[i] != ' ' || tag.s[i + 1] != 'i' || tag.s[i + 2] != 'n' || tag.s[i + 3] != ' ') return 0;
  i += 4;
  if (i >= tag.n) return 0;
  for (size_t k = i; k < tag.n; k++) {
    if (is_js_space(tag.s[k])) return 0;
  }
  *path_out = u16_slice(tag, i, tag.n);
  return 1;
}

// Matches T_IF /^if (not )?(\S+)$/ on a tag. Fills *not and the path text.
// ('if not' alone means path 'not', not negated — the regex backtracks.)
static int tag_if(U16 tag, int *not_out, U16 *path_out) {
  if (tag.n < 3 || tag.s[0] != 'i' || tag.s[1] != 'f' || tag.s[2] != ' ') return 0;
  size_t i = 3;
  int not = 0;
  // 'not ' (with the trailing space) means negated.
  if (i + 4 <= tag.n && tag.s[i] == 'n' && tag.s[i + 1] == 'o' && tag.s[i + 2] == 't' && tag.s[i + 3] == ' ') {
    not = 1;
    i += 4;
  }
  if (i >= tag.n) return 0;
  for (size_t k = i; k < tag.n; k++) {
    if (is_js_space(tag.s[k])) return 0;
  }
  *not_out = not;
  *path_out = u16_slice(tag, i, tag.n);
  return 1;
}

// Matches T_INCLUDE /^include "([a-z0-9-]+)"$/ on a tag.
static int tag_include(U16 tag, U16 *name_out) {
  static const char PRE[] = "include \"";
  if (tag.n < 10) return 0;
  for (size_t k = 0; k < 9; k++) if (tag.s[k] != (uint8_t)PRE[k]) return 0;
  size_t i = 9;
  size_t start = i;
  while (i < tag.n) {
    uint16_t c = tag.s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') i++;
    else break;
  }
  if (i == start) return 0;
  if (i + 1 != tag.n || tag.s[i] != '"') return 0;
  *name_out = u16_slice(tag, start, i);
  return 1;
}

// `{% <tag> %} is not a tag`
static U16 not_a_tag_err(Arena *a, U16 name, U16 tag) {
  U16Buf out;
  ub_init(&out, a);
  ub_put_ascii(&out, "template \"");
  ub_put_u16(&out, name);
  ub_put_ascii(&out, "\": {% ");
  ub_put_u16(&out, tag);
  ub_put_ascii(&out, " %} is not a tag");
  return ub_finish(&out);
}

TNode **parse_template(Arena *a, U16 name, U16 src, size_t *nnodes, U16 *err) {
  Tok *toks = NULL;
  size_t ntoks = 0;
  if (!tokenize(a, name, src, &toks, &ntoks, err)) return NULL;
  Body root;
  memset(&root, 0, sizeof root);
  Frame *stack = NULL;
  size_t nstack = 0, cap_stack = 0;
  AGROW(a, stack, nstack, cap_stack, Frame);
  stack[nstack].node = NULL;
  stack[nstack].body = &root;
  nstack++;
  for (size_t ti = 0; ti < ntoks; ti++) {
    Tok *tok = &toks[ti];
    Frame *top = &stack[nstack - 1];
    if (tok->kind == TK_TEXT) {
      TNode *n = node_new(a, N_TEXT);
      n->text = tok->text;
      body_push(a, top->body, n);
    } else if (tok->kind == TK_VAR || tok->kind == TK_RAW) {
      TNode *n = node_new(a, tok->kind == TK_VAR ? N_VAR : N_RAW);
      n->path = tok->path;
      n->npath = tok->npath;
      body_push(a, top->body, n);
    } else {
      U16 tag = tok->tag;
      U16 fname, fpath;
      int not;
      U16 inc;
      if (tag_for(tag, &fname, &fpath)) {
        TNode *n = node_new(a, N_FOR);
        // The loop variable name is an ASCII identifier: copy to a C string.
        char *nm = ARENA_ARR(a, char, fname.n + 1);
        for (size_t k = 0; k < fname.n; k++) nm[k] = (char)fname.s[k];
        nm[fname.n] = 0;
        n->for_name = nm;
        if (!path_of(a, name, fpath, &n->path, &n->npath, err)) return NULL;
        body_push(a, top->body, n);
        Body *b = ARENA_NEW(a, Body);
        memset(b, 0, sizeof *b);
        AGROW(a, stack, nstack, cap_stack, Frame);
        stack[nstack].node = n;
        stack[nstack].body = b;
        nstack++;
        // Remember the body struct on the node when the frame closes:
        // store it in the node's body pointer slot via the frame. The
        // frame's body IS the node's body list; on pop we copy it out.
        // (The assignment happens in the endfor branch below.)
      } else if (tag_if(tag, &not, &fpath)) {
        TNode *n = node_new(a, N_IF);
        n->not = not;
        if (!path_of(a, name, fpath, &n->path, &n->npath, err)) return NULL;
        body_push(a, top->body, n);
        Body *b = ARENA_NEW(a, Body);
        memset(b, 0, sizeof *b);
        AGROW(a, stack, nstack, cap_stack, Frame);
        stack[nstack].node = n;
        stack[nstack].body = b;
        nstack++;
      } else if (u16_eq_ascii(tag, "else")) {
        if (top->node == NULL || top->node->kind != N_IF || top->node->has_else) {
          *err = tpl_err_ascii(a, name, "an else outside an if");
          return NULL;
        }
        top->node->has_else = 1;
        // The then-branch is finished: move it onto the node, and point
        // the frame at a fresh else body.
        top->node->then_b = top->body->items;
        top->node->nthen = top->body->n;
        Body *b = ARENA_NEW(a, Body);
        memset(b, 0, sizeof *b);
        top->body = b;
      } else if (u16_eq_ascii(tag, "endfor") || u16_eq_ascii(tag, "endif")) {
        int want_for = u16_eq_ascii(tag, "endfor");
        if (top->node == NULL || top->node->kind != (want_for ? N_FOR : N_IF)) {
          U16Buf m;
          ub_init(&m, a);
          ub_put_ascii(&m, "an ");
          ub_put_u16(&m, tag);
          ub_put_ascii(&m, " with no open ");
          ub_put_ascii(&m, want_for ? "for" : "if");
          *err = tpl_err(a, name, ub_finish(&m));
          return NULL;
        }
        // Move the finished body onto the node.
        if (want_for) {
          top->node->body = top->body->items;
          top->node->nbody = top->body->n;
        } else if (top->node->has_else) {
          top->node->else_b = top->body->items;
          top->node->nelse = top->body->n;
        } else {
          top->node->then_b = top->body->items;
          top->node->nthen = top->body->n;
        }
        nstack--;
      } else if (tag_include(tag, &inc)) {
        TNode *n = node_new(a, N_INCLUDE);
        n->inc_name = inc;
        body_push(a, top->body, n);
      } else {
        *err = not_a_tag_err(a, name, tag);
        return NULL;
      }
    }
  }
  if (nstack > 1) {
    TNode *open = stack[nstack - 1].node;
    U16Buf m;
    ub_init(&m, a);
    ub_put_ascii(&m, "a ");
    ub_put_ascii(&m, open->kind == N_FOR ? "for" : "if");
    ub_put_ascii(&m, " is not closed");
    *err = tpl_err(a, name, ub_finish(&m));
    return NULL;
  }
  *nnodes = root.n;
  return root.items;
}

TNode **tpl_lookup(TplMap *maps, size_t n, U16 name, size_t *nnodes) {
  for (size_t i = 0; i < n; i++) {
    if (u16_eq(maps[i].name, name)) {
      *nnodes = maps[i].nnodes;
      return maps[i].nodes;
    }
  }
  return NULL;
}

// ------------------------------------------------------------------ renderer
void render_nodes(Arena *a, TNode **nodes, size_t nnodes, Scope *scope,
                  TplMap *maps, size_t nmaps, int depth, U16Buf *out) {
  for (size_t ni = 0; ni < nnodes; ni++) {
    TNode *n = nodes[ni];
    switch (n->kind) {
      case N_TEXT:
        ub_put_u16(out, n->text);
        break;
      case N_VAR: {
        Value v = lookup(scope, n->path, n->npath);
        escape_html_into(out, value_text(a, v));
        break;
      }
      case N_RAW: {
        Value v = lookup(scope, n->path, n->npath);
        ub_put_u16(out, value_text(a, v));
        break;
      }
      case N_IF: {
        Value v = lookup(scope, n->path, n->npath);
        int truth = value_truthy(v);
        TNode **branch = NULL;
        size_t nbranch = 0;
        if (truth != n->not) { branch = n->then_b; nbranch = n->nthen; }
        else if (n->has_else) { branch = n->else_b; nbranch = n->nelse; }
        if (branch) render_nodes(a, branch, nbranch, scope, maps, nmaps, depth, out);
        break;
      }
      case N_FOR: {
        Value xs = lookup(scope, n->path, n->npath);
        if (xs.t != V_ARR) break;
        // One child scope per loop; its vars are mutated per element.
        // An element named "loop" stays shadowed by the loop record itself.
        Scope *child = scope_new(a, scope);
        Value loop_rec;
        Obj *lo = obj_new(a);
        obj_put(lo, "index", v_int(0));
        obj_put(lo, "first", v_bool(0));
        obj_put(lo, "last", v_bool(0));
        obj_put(lo, "length", v_int((int64_t)xs.u.a.n));
        loop_rec = v_obj(lo);
        int is_loop_name = strcmp(n->for_name, "loop") == 0;
        if (!is_loop_name) scope_set(child, n->for_name, v_null());
        scope_set(child, "loop", loop_rec);
        for (size_t k = 0; k < xs.u.a.n; k++) {
          if (!is_loop_name) scope_set(child, n->for_name, xs.u.a.items[k]);
          obj_put(lo, "index", v_int((int64_t)k + 1));
          obj_put(lo, "first", v_bool(k == 0));
          obj_put(lo, "last", v_bool(k == xs.u.a.n - 1));
          render_nodes(a, n->body, n->nbody, child, maps, nmaps, depth, out);
        }
        break;
      }
      case N_INCLUDE: {
        size_t tn = 0;
        TNode **t = tpl_lookup(maps, nmaps, n->inc_name, &tn);
        if (t && depth < 8) render_nodes(a, t, tn, scope, maps, nmaps, depth + 1, out);
        break;
      }
    }
  }
}
