// json.c — JSON parser (UTF-8 in, UTF-16 values out) and JSON.stringify writer.
// Numbers: only integers occur in this competition's input (site.per_page,
// site.recent); a non-integer number is a fatal error, never silently rounded.
#include "sitegen.h"

typedef struct {
  Arena *a;
  const uint8_t *p, *end;
} Parser;

static void skip_ws(Parser *ps) {
  while (ps->p < ps->end) {
    uint8_t c = *ps->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
    else break;
  }
}

static int hexval(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static uint16_t parse_hex4(Parser *ps) {
  if (ps->end - ps->p < 4) die("JSON: truncated \\u escape");
  int v = 0;
  for (int i = 0; i < 4; i++) {
    int h = hexval(ps->p[i]);
    if (h < 0) die("JSON: bad \\u escape");
    v = (v << 4) | h;
  }
  ps->p += 4;
  return (uint16_t)v;
}

static U16 parse_string(Parser *ps) {
  // ps->p points at the opening quote.
  U16Buf b;
  ub_init(&b, ps->a);
  ps->p++; // skip "
  while (ps->p < ps->end) {
    uint8_t c = *ps->p;
    if (c == '"') {
      ps->p++;
      return ub_finish(&b);
    }
    if (c == '\\') {
      ps->p++;
      if (ps->p >= ps->end) die("JSON: truncated escape");
      uint8_t e = *ps->p++;
      switch (e) {
        case '"': ub_put(&b, '"'); break;
        case '\\': ub_put(&b, '\\'); break;
        case '/': ub_put(&b, '/'); break;
        case 'b': ub_put(&b, '\b'); break;
        case 'f': ub_put(&b, '\f'); break;
        case 'n': ub_put(&b, '\n'); break;
        case 'r': ub_put(&b, '\r'); break;
        case 't': ub_put(&b, '\t'); break;
        case 'u': {
          uint16_t u = parse_hex4(ps);
          // A high surrogate followed by \uDC00-\uDFFF pairs, exactly like
          // JSON.parse: a lone surrogate stays a lone code unit.
          if (u >= 0xD800 && u <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
            const uint8_t *save = ps->p;
            ps->p += 2;
            uint16_t u2 = parse_hex4(ps);
            if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
              ub_put(&b, u);
              ub_put(&b, u2);
            } else {
              ub_put(&b, u);
              ps->p = save; // not a pair: rewind, the \u is parsed next round
            }
          } else {
            ub_put(&b, u);
          }
          break;
        }
        default: die("JSON: bad escape \\%c", e);
      }
      continue;
    }
    if (c < 0x20) die("JSON: raw control character in string");
    uint32_t cp;
    size_t n = utf8_decode(ps->p, ps->end, &cp);
    if (n == 0) die("JSON: invalid UTF-8 in string");
    ps->p += n;
    if (cp < 0x10000) {
      ub_put(&b, (uint16_t)cp);
    } else {
      cp -= 0x10000;
      ub_put(&b, (uint16_t)(0xD800 + (cp >> 10)));
      ub_put(&b, (uint16_t)(0xDC00 + (cp & 0x3FF)));
    }
  }
  die("JSON: unterminated string");
}

static JVal *parse_value(Parser *ps);

static JVal *new_val(Parser *ps, JType t) {
  JVal *v = ARENA_NEW(ps->a, JVal);
  v->t = t;
  memset(&v->u, 0, sizeof v->u);
  return v;
}

static JVal *parse_number(Parser *ps) {
  const uint8_t *start = ps->p;
  if (ps->p < ps->end && *ps->p == '-') ps->p++;
  if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') die("JSON: bad number");
  if (*ps->p == '0') ps->p++;
  else while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
  if (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E')) {
    die("JSON: non-integer number not supported in this input");
  }
  // Integer (may be negative; values here are small, but parse fully).
  int neg = start[0] == '-';
  int64_t val = 0;
  for (const uint8_t *q = start + (neg ? 1 : 0); q < ps->p; q++) {
    val = val * 10 + (*q - '0');
    if (val > (int64_t)4e18) die("JSON: integer too large");
  }
  JVal *v = new_val(ps, J_INT);
  v->u.i = neg ? -val : val;
  return v;
}

static JVal *parse_value(Parser *ps) {
  skip_ws(ps);
  if (ps->p >= ps->end) die("JSON: unexpected end of input");
  uint8_t c = *ps->p;
  if (c == '"') {
    JVal *v = new_val(ps, J_STR);
    v->u.s = parse_string(ps);
    return v;
  }
  if (c == '{') {
    ps->p++;
    JVal *v = new_val(ps, J_OBJ);
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
    for (;;) {
      skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != '"') die("JSON: expected object key");
      U16 key = parse_string(ps);
      skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') die("JSON: expected ':' in object");
      ps->p++;
      JVal *val = parse_value(ps);
      if (v->u.o.n == v->u.o.cap) {
        size_t nc = v->u.o.cap ? v->u.o.cap * 2 : 8;
        U16 *nk = ARENA_ARR(ps->a, U16, nc);
        JVal **nv = ARENA_ARR(ps->a, JVal *, nc);
        if (v->u.o.k) memcpy(nk, v->u.o.k, sizeof(U16) * v->u.o.n);
        if (v->u.o.v) memcpy(nv, v->u.o.v, sizeof(JVal *) * v->u.o.n);
        v->u.o.k = nk;
        v->u.o.v = nv;
        v->u.o.cap = nc;
      }
      v->u.o.k[v->u.o.n] = key;
      v->u.o.v[v->u.o.n] = val;
      v->u.o.n++;
      skip_ws(ps);
      if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
      if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
      die("JSON: expected ',' or '}' in object");
    }
  }
  if (c == '[') {
    ps->p++;
    JVal *v = new_val(ps, J_ARR);
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
    for (;;) {
      JVal *el = parse_value(ps);
      AGROW(ps->a, v->u.a.v, v->u.a.n, v->u.a.cap, JVal *);
      v->u.a.v[v->u.a.n++] = el;
      skip_ws(ps);
      if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
      if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
      die("JSON: expected ',' or ']' in array");
    }
  }
  if (c == 't') {
    if (ps->end - ps->p < 4 || memcmp(ps->p, "true", 4) != 0) die("JSON: bad literal");
    ps->p += 4;
    JVal *v = new_val(ps, J_BOOL);
    v->u.b = 1;
    return v;
  }
  if (c == 'f') {
    if (ps->end - ps->p < 5 || memcmp(ps->p, "false", 5) != 0) die("JSON: bad literal");
    ps->p += 5;
    JVal *v = new_val(ps, J_BOOL);
    v->u.b = 0;
    return v;
  }
  if (c == 'n') {
    if (ps->end - ps->p < 4 || memcmp(ps->p, "null", 4) != 0) die("JSON: bad literal");
    ps->p += 4;
    return new_val(ps, J_NULL);
  }
  if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
  die("JSON: unexpected character %c", c);
}

JVal *json_parse(Arena *a, const uint8_t *data, size_t len) {
  Parser ps = {a, data, data + len};
  JVal *v = parse_value(&ps);
  skip_ws(&ps);
  if (ps.p != ps.end) die("JSON: trailing data after value");
  return v;
}

JVal *jget(JVal *obj, const char *key) {
  if (!obj || obj->t != J_OBJ) return NULL;
  for (size_t i = 0; i < obj->u.o.n; i++) {
    if (u16_eq_ascii(obj->u.o.k[i], key)) return obj->u.o.v[i];
  }
  return NULL;
}

void json_write_value(ByteBuf *out, JVal *v) {
  switch (v->t) {
    case J_NULL: bb_put_ascii(out, "null"); break;
    case J_BOOL: bb_put_ascii(out, v->u.b ? "true" : "false"); break;
    case J_INT: bb_put_int(out, v->u.i); break;
    case J_STR: bb_put_json_string(out, v->u.s); break;
    case J_ARR: {
      bb_put(out, '[');
      for (size_t i = 0; i < v->u.a.n; i++) {
        if (i) bb_put(out, ',');
        json_write_value(out, v->u.a.v[i]);
      }
      bb_put(out, ']');
      break;
    }
    case J_OBJ: {
      bb_put(out, '{');
      for (size_t i = 0; i < v->u.o.n; i++) {
        if (i) bb_put(out, ',');
        bb_put_json_string(out, v->u.o.k[i]);
        bb_put(out, ':');
        json_write_value(out, v->u.o.v[i]);
      }
      bb_put(out, '}');
      break;
    }
  }
}
