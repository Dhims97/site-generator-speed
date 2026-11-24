// build.c — the 8-step build pipeline, ported from v1's run() in solve.js.
// Output order, report wording and every value shape match the reference.
#include "sitegen.h"

// ------------------------------------------------------------------ helpers
static JVal *jget_u16(JVal *obj, U16 key) {
  if (!obj || obj->t != J_OBJ) return NULL;
  for (size_t i = 0; i < obj->u.o.n; i++) {
    if (u16_eq(obj->u.o.k[i], key)) return obj->u.o.v[i];
  }
  return NULL;
}

static void report_push(Build *b, U16 line) {
  AGROW(b->a, b->report, b->nreport, b->cap_report, U16);
  b->report[b->nreport++] = line;
}

static void report_push_fmt(Build *b, const char *pre, U16 mid, const char *post) {
  U16Buf out;
  ub_init(&out, b->a);
  ub_put_ascii(&out, pre);
  ub_put_u16(&out, mid);
  ub_put_ascii(&out, post);
  report_push(b, ub_finish(&out));
}

static void broken_add(Build *b, U16 line) {
  for (size_t i = 0; i < b->nbroken; i++) {
    if (u16_eq(b->broken[i], line)) return; // a Set: first occurrence only
  }
  AGROW(b->a, b->broken, b->nbroken, b->cap_broken, U16);
  b->broken[b->nbroken++] = line;
}

static int out_has(Build *b, U16 path) {
  for (size_t i = 0; i < b->nout; i++) {
    if (u16_eq(b->out[i].path, path)) return 1;
  }
  return 0;
}

static void out_put(Build *b, U16 path, U16 content) {
  AGROW(b->a, b->out, b->nout, b->cap_out, OutEnt);
  b->out[b->nout].path = path;
  b->out[b->nout].content = content;
  b->nout++;
}

static U16 dir_of(U16 path) {
  long slash = u16_last_index_of_ch(path, '/', (long)path.n - 1);
  if (slash == -1) return u16_empty();
  return u16_slice(path, 0, (size_t)slash);
}

static int is_ascii_letter(uint16_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_ascii_digit(uint16_t c) { return c >= '0' && c <= '9'; }

// resolvePath: returns 1 with *out set, or 0 when it climbs out of the root.
static int resolve_path(Arena *a, U16 dir, U16 rel, U16 *out) {
  // Fast path: no '..', no './', not '.', no trailing '/.', no '//', no trailing '/'.
  int fast = 1;
  if (u16_index_of(rel, u16_from_ascii(a, ".."), 0) != -1) fast = 0;
  if (u16_index_of(rel, u16_from_ascii(a, "./"), 0) != -1) fast = 0;
  if (u16_eq_ascii(rel, ".")) fast = 0;
  if (u16_ends_with(rel, u16_from_ascii(a, "/."))) fast = 0;
  if (u16_index_of(rel, u16_from_ascii(a, "//"), 0) != -1) fast = 0;
  if (rel.n > 0 && rel.s[rel.n - 1] == '/') fast = 0;
  if (fast) {
    if (dir.n == 0) { *out = rel; return 1; }
    U16Buf b;
    ub_init(&b, a);
    ub_put_u16(&b, dir);
    ub_put(&b, '/');
    ub_put_u16(&b, rel);
    *out = ub_finish(&b);
    return 1;
  }
  // Slow path: split dir and rel on '/', walk the segments.
  U16 *parts = NULL;
  size_t nparts = 0, cap_parts = 0;
  if (dir.n > 0) {
    size_t start = 0;
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.s[i] == '/') {
        AGROW(a, parts, nparts, cap_parts, U16);
        parts[nparts++] = u16_slice(dir, start, i);
        start = i + 1;
      }
    }
  }
  size_t start = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.s[i] == '/') {
      U16 seg = u16_slice(rel, start, i);
      start = i + 1;
      if (seg.n == 0 || u16_eq_ascii(seg, ".")) continue;
      if (u16_eq_ascii(seg, "..")) {
        if (nparts == 0) return 0;
        nparts--;
      } else {
        AGROW(a, parts, nparts, cap_parts, U16);
        parts[nparts++] = seg;
      }
    }
  }
  U16Buf b;
  ub_init(&b, a);
  for (size_t i = 0; i < nparts; i++) {
    if (i) ub_put(&b, '/');
    ub_put_u16(&b, parts[i]);
  }
  *out = ub_finish(&b);
  return 1;
}

// ------------------------------------------------------------------ taken sets
typedef struct { U16 dir; U16 *slugs; size_t n, cap; } TakenDir;

static TakenDir *taken_in(Arena *a, TakenDir **dirs, size_t *ndirs, size_t *cap_dirs, U16 dir) {
  for (size_t i = 0; i < *ndirs; i++) {
    if (u16_eq((*dirs)[i].dir, dir)) return &(*dirs)[i];
  }
  AGROW(a, *dirs, *ndirs, *cap_dirs, TakenDir);
  TakenDir *d = &(*dirs)[(*ndirs)++];
  d->dir = dir;
  d->slugs = NULL;
  d->n = 0;
  d->cap = 0;
  if (dir.n == 0) {
    // RESERVED = ['posts', 'tags', 'page']
    static const char *RES[] = {"posts", "tags", "page"};
    for (size_t k = 0; k < 3; k++) {
      AGROW(a, d->slugs, d->n, d->cap, U16);
      d->slugs[d->n++] = u16_from_ascii(a, RES[k]);
    }
  }
  return d;
}

static U16 unique_slug_taken(Arena *a, U16 slug, TakenDir *d) {
  U16 out = slug;
  int suffix = 2;
  for (;;) {
    int taken = 0;
    for (size_t i = 0; i < d->n; i++) {
      if (u16_eq(d->slugs[i], out)) { taken = 1; break; }
    }
    if (!taken) break;
    U16Buf b;
    ub_init(&b, a);
    ub_put_u16(&b, slug);
    ub_put(&b, '-');
    ub_put_int(&b, suffix++);
    out = ub_finish(&b);
  }
  AGROW(a, d->slugs, d->n, d->cap, U16);
  d->slugs[d->n++] = out;
  return out;
}

// ------------------------------------------------------------------ link fn
typedef struct { Build *b; Source *s; U16 dir; } LinkCtx;

static Source *by_source(Build *b, U16 path) {
  for (size_t i = 0; i < b->nsources; i++) {
    if (u16_eq(b->sources[i].path, path)) return &b->sources[i];
  }
  return NULL;
}

static int static_has(Build *b, U16 path) {
  for (size_t i = 0; i < b->nstatics; i++) {
    if (u16_eq(b->statics[i].path, path)) return 1;
  }
  return 0;
}

static U16 build_link(void *ctx, U16 target, Arena *a) {
  LinkCtx *c = ctx;
  Build *b = c->b;
  if (target.n > 0 && (target.s[0] == '/' || target.s[0] == '#')) return target;
  // /^[A-Za-z][A-Za-z0-9+.-]*:/
  if (target.n >= 2 && is_ascii_letter(target.s[0])) {
    size_t i = 1;
    while (i < target.n &&
           (is_ascii_letter(target.s[i]) || is_ascii_digit(target.s[i]) || target.s[i] == '+' ||
            target.s[i] == '-' || target.s[i] == '.')) {
      i++;
    }
    if (i < target.n && target.s[i] == ':') return target;
  }
  long hash = u16_index_of_ch(target, '#', 0);
  U16 rel = hash == -1 ? target : u16_slice(target, 0, (size_t)hash);
  U16 frag = hash == -1 ? u16_empty() : u16_slice(target, (size_t)hash, target.n);
  U16 path;
  int have_path = 0;
  if (rel.n > 0) have_path = resolve_path(a, c->dir, rel, &path);
  if (have_path) {
    Source *hit = by_source(b, path);
    if (hit) {
      U16Buf out;
      ub_init(&out, a);
      ub_put_u16(&out, hit->url);
      ub_put_u16(&out, frag);
      return ub_finish(&out);
    }
    if (static_has(b, path)) {
      U16Buf out;
      ub_init(&out, a);
      ub_put_u16(&out, b->base);
      ub_put(&out, '/');
      ub_put_u16(&out, path);
      ub_put_u16(&out, frag);
      return ub_finish(&out);
    }
  }
  // Broken: `broken-link <source path> -> <target>`
  U16Buf out;
  ub_init(&out, a);
  ub_put_ascii(&out, "broken-link ");
  ub_put_u16(&out, c->s->path);
  ub_put_ascii(&out, " -> ");
  ub_put_u16(&out, target);
  broken_add(b, ub_finish(&out));
  return target;
}

// ------------------------------------------------------------------ sorting
static int cmp_u16(const void *x, const void *y) {
  return u16_cmp(*(const U16 *)x, *(const U16 *)y);
}

typedef struct { JVal *file; size_t idx; } FileEnt;
static int cmp_file_ent(const void *x, const void *y) {
  const FileEnt *a = x, *b = y;
  U16 pa = jget(a->file, "path")->u.s;
  U16 pb = jget(b->file, "path")->u.s;
  int c = u16_cmp(pa, pb);
  if (c) return c;
  return a->idx < b->idx ? -1 : a->idx > b->idx ? 1 : 0;
}

static int cmp_source_ptr(const void *x, const void *y) {
  Source *a = *(Source *const *)x, *b = *(Source *const *)y;
  // posts newest first, equal dates by slug
  int c = u16_cmp(b->fm.date, a->fm.date);
  if (c) return c;
  return u16_cmp(a->slug, b->slug);
}

static int cmp_tag_ptr(const void *x, const void *y) {
  Tag *a = *(Tag *const *)x, *b = *(Tag *const *)y;
  if (a->nposts != b->nposts) return a->nposts > b->nposts ? -1 : 1;
  return u16_cmp(a->slug, b->slug);
}

// ------------------------------------------------------------------ values
// Build the template Value objects for one source's card / page / links.

static Value card_value(Build *b, Source *s);
static Value link_value(Build *b, Source *s);

static Value link_value(Build *b, Source *s) {
  Arena *a = b->a;
  if (!s) return v_null();
  Obj *o = obj_new(a);
  obj_put(o, "title", v_str(s->fm.title));
  obj_put(o, "url", v_str(s->url));
  obj_put(o, "date", v_str(s->fm.has_date ? s->fm.date : u16_empty()));
  obj_put(o, "date_long", v_str(s->fm.has_date ? long_date(a, s->fm.date) : u16_empty()));
  return v_obj(o);
}

static Value card_value(Build *b, Source *s) {
  Arena *a = b->a;
  if (s->card_obj) return v_obj(s->card_obj);
  Obj *o = obj_new(a);
  obj_put(o, "title", v_str(s->fm.title));
  obj_put(o, "url", v_str(s->url));
  obj_put(o, "date", v_str(s->fm.has_date ? s->fm.date : u16_empty()));
  obj_put(o, "date_long", v_str(s->fm.has_date ? long_date(a, s->fm.date) : u16_empty()));
  obj_put(o, "summary", v_str(s->fm.has_summary ? s->fm.summary : u16_empty()));
  obj_put(o, "excerpt", v_str(s->excerpt));
  int words = words_of(s->body);
  int rm = (words + 199) / 200;
  if (rm < 1) rm = 1;
  obj_put(o, "reading_minutes", v_int(rm));
  Value *tags = ARENA_ARR(a, Value, s->ntag_refs);
  for (size_t i = 0; i < s->ntag_refs; i++) {
    Obj *t = obj_new(a);
    obj_put(t, "name", v_str(s->tag_refs[i]->name));
    obj_put(t, "slug", v_str(s->tag_refs[i]->slug));
    obj_put(t, "url", v_str(s->tag_refs[i]->url));
    tags[i] = v_obj(t);
  }
  obj_put(o, "tags", v_arr(tags, s->ntag_refs));
  s->card_obj = o;
  return v_obj(o);
}

static Value page_value(Build *b, Source *s) {
  Arena *a = b->a;
  Obj *o = obj_new(a);
  // ...card (spread): copy the card's entries in order.
  Value card = card_value(b, s);
  Obj *co = card.u.o;
  for (size_t i = 0; i < co->n; i++) obj_put(o, co->names[i], co->vals[i]);
  obj_put(o, "slug", v_str(s->slug));
  obj_put(o, "content", v_str(s->html));
  Value *toc = ARENA_ARR(a, Value, s->ntoc);
  for (size_t i = 0; i < s->ntoc; i++) {
    Obj *h = obj_new(a);
    obj_put(h, "level", v_int(s->toc[i].level));
    obj_put(h, "id", v_str(s->toc[i].id));
    obj_put(h, "html", v_str(s->toc[i].html));
    toc[i] = v_obj(h);
  }
  obj_put(o, "toc", v_arr(toc, s->ntoc));
  obj_put(o, "newer", link_value(b, s->newer));
  obj_put(o, "older", link_value(b, s->older));
  obj_put(o, "is_post", v_bool(s->post));
  return v_obj(o);
}

// ------------------------------------------------------------------ build
void build_run(Arena *a, JVal *input, OutEnt **out, size_t *nout) {
  Build b;
  memset(&b, 0, sizeof b);
  b.a = a;
  b.input = input;

  // Templates, in code-unit order of name; a template that fails to parse
  // is reported and left out of the map.
  JVal *templates_obj = jget(input, "templates");
  size_t ntemplates = templates_obj ? templates_obj->u.o.n : 0;
  U16 *tnames = ARENA_ARR(a, U16, ntemplates ? ntemplates : 1);
  for (size_t i = 0; i < ntemplates; i++) tnames[i] = templates_obj->u.o.k[i];
  qsort(tnames, ntemplates, sizeof(U16), cmp_u16);
  TplMap *maps = ARENA_ARR(a, TplMap, ntemplates ? ntemplates : 1);
  size_t nmaps = 0;
  for (size_t i = 0; i < ntemplates; i++) {
    JVal *src = jget_u16(templates_obj, tnames[i]);
    // A non-string template value tokenizes to an empty body in the
    // reference (src.length is undefined); the generator always sends
    // strings, so this is only a defensive match.
    U16 tsrc = (src && src->t == J_STR) ? src->u.s : u16_empty();
    U16 err;
    size_t nn = 0;
    TNode **nodes = parse_template(a, tnames[i], tsrc, &nn, &err);
    if (!nodes) {
      report_push(&b, err);
      continue;
    }
    maps[nmaps].name = tnames[i];
    maps[nmaps].nodes = nodes;
    maps[nmaps].nnodes = nn;
    nmaps++;
  }
  b.templates = maps;
  b.ntemplates = nmaps;

  // base_url with trailing slashes stripped.
  JVal *site_obj = jget(input, "site");
  U16 base = jget(site_obj, "base_url")->u.s;
  while (base.n > 0 && base.s[base.n - 1] == '/') base.n--;
  b.base = base;

  // 1. Sources, in code-unit order of path; line ends made "\n".
  JVal *files = jget(input, "files");
  size_t nfiles = files ? files->u.a.n : 0;
  FileEnt *fents = ARENA_ARR(a, FileEnt, nfiles ? nfiles : 1);
  for (size_t i = 0; i < nfiles; i++) {
    fents[i].file = files->u.a.v[i];
    fents[i].idx = i;
  }
  qsort(fents, nfiles, sizeof(FileEnt), cmp_file_ent);
  Source *all = ARENA_ARR(a, Source, nfiles ? nfiles : 1);
  size_t nall = 0;
  for (size_t i = 0; i < nfiles; i++) {
    JVal *f = fents[i].file;
    U16 path = jget(f, "path")->u.s;
    U16 content = jget(f, "content")->u.s;
    if (u16_ends_with(path, u16_from_ascii(a, ".md"))) {
      Source *s = &all[nall++];
      memset(s, 0, sizeof *s);
      s->path = path;
      // \r normalisation: \r\n? -> \n
      if (u16_index_of_ch(content, '\r', 0) == -1) {
        s->text = content;
      } else {
        U16Buf t;
        ub_init(&t, a);
        for (size_t k = 0; k < content.n; k++) {
          if (content.s[k] == '\r') {
            ub_put(&t, '\n');
            if (k + 1 < content.n && content.s[k + 1] == '\n') k++;
          } else {
            ub_put(&t, content.s[k]);
          }
        }
        s->text = ub_finish(&t);
      }
    } else {
      // statics keep insertion order (the file order after the sort)
      AGROW(a, b.statics, b.nstatics, b.cap_statics, StaticFile);
      b.statics[b.nstatics].path = path;
      b.statics[b.nstatics].content = content;
      b.nstatics++;
    }
  }
  b.all_sources = all;
  b.nall_sources = nall;

  // 2. Front matter, in source order; refusals and drafts reported, in order.
  Source *kept = ARENA_ARR(a, Source, nall ? nall : 1);
  size_t nkept = 0;
  for (size_t i = 0; i < nall; i++) {
    Source *s = &all[i];
    U16 body;
    U16 err = parse_front_matter(a, s->text, &s->fm, &body);
    if (err.n != 0) {
      U16Buf m;
      ub_init(&m, a);
      ub_put_ascii(&m, "refused ");
      ub_put_u16(&m, s->path);
      ub_put_ascii(&m, ": ");
      ub_put_u16(&m, err);
      report_push(&b, ub_finish(&m));
      continue;
    }
    s->body = body;
    s->post = u16_starts_with(s->path, u16_from_ascii(a, "posts/"));
    U16 own;
    int has_own = 0;
    if (u16_starts_with(s->path, u16_from_ascii(a, "tags/"))) { own = u16_from_ascii(a, "tags"); has_own = 1; }
    else if (u16_starts_with(s->path, u16_from_ascii(a, "page/"))) { own = u16_from_ascii(a, "page"); has_own = 1; }
    if (has_own) {
      U16Buf m;
      ub_init(&m, a);
      ub_put_ascii(&m, "refused ");
      ub_put_u16(&m, s->path);
      ub_put_ascii(&m, ": ");
      ub_put_u16(&m, own);
      ub_put_ascii(&m, "/ is the generator's own");
      report_push(&b, ub_finish(&m));
      continue;
    }
    if (s->post && !s->fm.has_date) {
      report_push_fmt(&b, "refused ", s->path, ": a post needs a date");
      continue;
    }
    U16 layout = s->fm.has_layout ? s->fm.layout
                                  : u16_from_ascii(a, s->post ? "post" : "page");
    size_t dummy = 0;
    if (!tpl_lookup(maps, nmaps, layout, &dummy)) {
      U16Buf m;
      ub_init(&m, a);
      ub_put_ascii(&m, "refused ");
      ub_put_u16(&m, s->path);
      ub_put_ascii(&m, ": layout \"");
      ub_put_u16(&m, layout);
      ub_put_ascii(&m, "\" is no template that parses");
      report_push(&b, ub_finish(&m));
      continue;
    }
    s->layout = layout;
    if (s->fm.has_draft && s->fm.draft) {
      report_push_fmt(&b, "draft ", s->path, "");
      continue;
    }
    kept[nkept++] = *s;
  }
  b.sources = kept;
  b.nsources = nkept;

  // 3. Permalinks.
  TakenDir *dirs = NULL;
  size_t ndirs = 0, cap_dirs = 0;
  for (size_t i = 0; i < nkept; i++) {
    Source *s = &kept[i];
    U16 dir = s->post ? u16_from_ascii(a, "posts") : dir_of(s->path);
    U16 slug = s->fm.has_slug ? s->fm.slug : slugify(a, s->fm.title);
    TakenDir *d = taken_in(a, &dirs, &ndirs, &cap_dirs, dir);
    s->slug = unique_slug_taken(a, slug, d);
    U16Buf od;
    ub_init(&od, a);
    if (dir.n > 0) {
      ub_put_u16(&od, dir);
      ub_put(&od, '/');
    }
    ub_put_u16(&od, s->slug);
    s->out_dir = ub_finish(&od);
    U16Buf u;
    ub_init(&u, a);
    ub_put_u16(&u, b.base);
    ub_put(&u, '/');
    ub_put_u16(&u, s->out_dir);
    ub_put(&u, '/');
    s->url = ub_finish(&u);
  }

  // 4. Markdown, links resolved.
  for (size_t i = 0; i < nkept; i++) {
    Source *s = &kept[i];
    LinkCtx ctx;
    ctx.b = &b;
    ctx.s = s;
    ctx.dir = dir_of(s->path);
    MdResult md = render_markdown(a, s->body, build_link, &ctx);
    s->html = md.html;
    s->toc = md.toc;
    s->ntoc = md.ntoc;
    s->excerpt = md.excerpt;
  }

  // 5. Collections: posts newest first, equal dates by slug.
  Source **posts = ARENA_ARR(a, Source *, nkept ? nkept : 1);
  size_t nposts = 0;
  for (size_t i = 0; i < nkept; i++) {
    if (kept[i].post) posts[nposts++] = &kept[i];
  }
  qsort(posts, nposts, sizeof(Source *), cmp_source_ptr);
  for (size_t k = 0; k < nposts; k++) {
    posts[k]->newer = k == 0 ? NULL : posts[k - 1];
    posts[k]->older = k == nposts - 1 ? NULL : posts[k + 1];
  }
  b.posts = posts;
  b.nposts = nposts;

  // 6. Tags.
  Tag **by_slug = NULL; // insertion-ordered tag list (the bySlug map)
  size_t nby_slug = 0, cap_by_slug = 0;
  // ownTags(s): tag names in first-seen order, deduped by slug.
  for (size_t pi = 0; pi < nposts; pi++) {
    Source *p = posts[pi];
    U16 *seen_slugs = NULL;
    size_t nseen = 0, cap_seen = 0;
    U16 *names = NULL;
    size_t nnames = 0, cap_names = 0;
    if (p->fm.has_tags) {
      for (size_t t = 0; t < p->fm.ntags; t++) {
        U16 slug = slugify(a, p->fm.tags[t]);
        int dup = 0;
        for (size_t k = 0; k < nseen; k++) {
          if (u16_eq(seen_slugs[k], slug)) { dup = 1; break; }
        }
        if (dup) continue;
        AGROW(a, seen_slugs, nseen, cap_seen, U16);
        seen_slugs[nseen++] = slug;
        AGROW(a, names, nnames, cap_names, U16);
        names[nnames++] = p->fm.tags[t];
      }
    }
    Tag **refs = ARENA_ARR(a, Tag *, nnames ? nnames : 1);
    size_t nrefs = 0;
    for (size_t t = 0; t < nnames; t++) {
      U16 slug = slugify(a, names[t]);
      Tag *tag = NULL;
      for (size_t k = 0; k < nby_slug; k++) {
        if (u16_eq(by_slug[k]->slug, slug)) { tag = by_slug[k]; break; }
      }
      if (!tag) {
        tag = ARENA_NEW(a, Tag);
        tag->name = names[t];
        tag->slug = slug;
        U16Buf u;
        ub_init(&u, a);
        ub_put_u16(&u, b.base);
        ub_put_ascii(&u, "/tags/");
        ub_put_u16(&u, slug);
        ub_put(&u, '/');
        tag->url = ub_finish(&u);
        tag->posts = NULL;
        tag->nposts = 0;
        tag->cap_posts = 0;
        AGROW(a, by_slug, nby_slug, cap_by_slug, Tag *);
        by_slug[nby_slug++] = tag;
      }
      AGROW(a, tag->posts, tag->nposts, tag->cap_posts, Source *);
      tag->posts[tag->nposts++] = p;
      refs[nrefs++] = tag;
    }
    p->tag_refs = refs;
    p->ntag_refs = nrefs;
  }
  for (size_t i = 0; i < nkept; i++) {
    Source *s = &kept[i];
    if (s->post) continue;
    U16 *seen_slugs = NULL;
    size_t nseen = 0, cap_seen = 0;
    U16 *names = NULL;
    size_t nnames = 0, cap_names = 0;
    if (s->fm.has_tags) {
      for (size_t t = 0; t < s->fm.ntags; t++) {
        U16 slug = slugify(a, s->fm.tags[t]);
        int dup = 0;
        for (size_t k = 0; k < nseen; k++) {
          if (u16_eq(seen_slugs[k], slug)) { dup = 1; break; }
        }
        if (dup) continue;
        AGROW(a, seen_slugs, nseen, cap_seen, U16);
        seen_slugs[nseen++] = slug;
        AGROW(a, names, nnames, cap_names, U16);
        names[nnames++] = s->fm.tags[t];
      }
    }
    Tag **refs = ARENA_ARR(a, Tag *, nnames ? nnames : 1);
    size_t nrefs = 0;
    for (size_t t = 0; t < nnames; t++) {
      U16 slug = slugify(a, names[t]);
      Tag *tag = NULL;
      for (size_t k = 0; k < nby_slug; k++) {
        if (u16_eq(by_slug[k]->slug, slug)) { tag = by_slug[k]; break; }
      }
      if (!tag) {
        // A page-only tag: not in the tag list, url ''.
        tag = ARENA_NEW(a, Tag);
        tag->name = names[t];
        tag->slug = slug;
        tag->url = u16_empty();
        tag->posts = NULL;
        tag->nposts = 0;
        tag->cap_posts = 0;
      }
      refs[nrefs++] = tag;
    }
    s->tag_refs = refs;
    s->ntag_refs = nrefs;
  }
  qsort(by_slug, nby_slug, sizeof(Tag *), cmp_tag_ptr);
  b.tags = by_slug;
  b.ntags = nby_slug;

  // 7. Layouts.
  Obj *site = obj_new(a);
  obj_put(site, "title", v_str(jget(site_obj, "title")->u.s));
  obj_put(site, "base_url", v_str(b.base));
  Value *all_tags = ARENA_ARR(a, Value, nby_slug ? nby_slug : 1);
  for (size_t i = 0; i < nby_slug; i++) {
    Obj *t = obj_new(a);
    obj_put(t, "name", v_str(by_slug[i]->name));
    obj_put(t, "slug", v_str(by_slug[i]->slug));
    obj_put(t, "url", v_str(by_slug[i]->url));
    obj_put(t, "count", v_int((int64_t)by_slug[i]->nposts));
    all_tags[i] = v_obj(t);
  }
  int64_t recent_n = jget(site_obj, "recent")->u.i;
  // posts.slice(0, recent), with JS slice's negative-end rule.
  int64_t rend = recent_n;
  if (rend < 0) rend = (int64_t)nposts + rend;
  if (rend < 0) rend = 0;
  if (rend > (int64_t)nposts) rend = (int64_t)nposts;
  Value *recent = ARENA_ARR(a, Value, (size_t)(rend > 0 ? rend : 0) ? (size_t)rend : 1);
  for (int64_t i = 0; i < rend; i++) recent[i] = card_value(&b, posts[i]);

  Scope *top = scope_new(a, NULL);
  scope_set(top, "site", v_obj(site));
  scope_set(top, "tags", v_arr(all_tags, nby_slug));
  scope_set(top, "recent", v_arr(recent, (size_t)rend));
  scope_set(top, "page", v_undef());
  scope_set(top, "posts", v_undef());
  scope_set(top, "pager", v_undef());
  scope_set(top, "tag", v_undef());

  // Each render below sets the four mutable top-scope keys from its vars
  // (or undefined) and calls render_nodes directly.
  for (size_t i = 0; i < nkept; i++) {
    Source *s = &kept[i];
    size_t tn = 0;
    TNode **nodes = tpl_lookup(maps, nmaps, s->layout, &tn);
    if (!nodes) continue; // step 2 already guaranteed the layout parses
    scope_set(top, "page", page_value(&b, s));
    scope_set(top, "posts", v_undef());
    scope_set(top, "pager", v_undef());
    scope_set(top, "tag", v_undef());
    U16Buf out;
    ub_init(&out, a);
    render_nodes(a, nodes, tn, top, maps, nmaps, 0, &out);
    U16Buf p;
    ub_init(&p, a);
    ub_put_u16(&p, s->out_dir);
    ub_put_ascii(&p, "/index.html");
    out_put(&b, ub_finish(&p), ub_finish(&out));
  }
  int64_t per = jget(site_obj, "per_page")->u.i;
  int64_t npages = 1;
  if (per >= 1) {
    npages = ((int64_t)nposts + per - 1) / per;
    if (npages < 1) npages = 1;
  }
  for (int64_t k = 1; k <= npages; k++) {
    size_t tn = 0;
    TNode **nodes = tpl_lookup(maps, nmaps, u16_from_ascii(a, "index"), &tn);
    if (!nodes) break; // no index template: no index pages at all
    int64_t from = (k - 1) * per;
    int64_t to = k * per;
    if (to > (int64_t)nposts) to = (int64_t)nposts;
    if (from > to) from = to;
    Value *cards = ARENA_ARR(a, Value, (size_t)(to - from > 0 ? to - from : 0) ? (size_t)(to - from) : 1);
    for (int64_t j = from; j < to; j++) cards[j - from] = card_value(&b, posts[j]);
    Obj *pager = obj_new(a);
    obj_put(pager, "number", v_int(k));
    obj_put(pager, "total", v_int(npages));
    U16Buf u;
    ub_init(&u, a);
    ub_put_u16(&u, b.base);
    ub_put(&u, '/');
    if (k != 1) {
      ub_put_ascii(&u, "page/");
      ub_put_int(&u, k);
      ub_put(&u, '/');
    }
    U16 url_k = ub_finish(&u);
    obj_put(pager, "url", v_str(url_k));
    if (k == 1) {
      obj_put(pager, "newer_url", v_str(u16_empty()));
    } else {
      U16Buf nu;
      ub_init(&nu, a);
      ub_put_u16(&nu, b.base);
      ub_put_ascii(&nu, k - 1 == 1 ? "/" : "/page/");
      if (k - 1 != 1) {
        ub_put_int(&nu, k - 1);
        ub_put(&nu, '/');
      }
      obj_put(pager, "newer_url", v_str(ub_finish(&nu)));
    }
    if (k == npages) {
      obj_put(pager, "older_url", v_str(u16_empty()));
    } else {
      U16Buf ou;
      ub_init(&ou, a);
      ub_put_u16(&ou, b.base);
      ub_put_ascii(&ou, "/page/");
      ub_put_int(&ou, k + 1);
      ub_put(&ou, '/');
      obj_put(pager, "older_url", v_str(ub_finish(&ou)));
    }
    scope_set(top, "page", v_undef());
    scope_set(top, "posts", v_arr(cards, (size_t)(to - from)));
    scope_set(top, "pager", v_obj(pager));
    scope_set(top, "tag", v_undef());
    U16Buf out;
    ub_init(&out, a);
    render_nodes(a, nodes, tn, top, maps, nmaps, 0, &out);
    U16Buf p;
    ub_init(&p, a);
    if (k == 1) {
      ub_put_ascii(&p, "index.html");
    } else {
      ub_put_ascii(&p, "page/");
      ub_put_int(&p, k);
      ub_put_ascii(&p, "/index.html");
    }
    out_put(&b, ub_finish(&p), ub_finish(&out));
  }
  for (size_t i = 0; i < nby_slug; i++) {
    Tag *t = by_slug[i];
    size_t tn = 0;
    TNode **nodes = tpl_lookup(maps, nmaps, u16_from_ascii(a, "tag"), &tn);
    if (!nodes) break; // no tag template: no tag pages at all
    Obj *tag = obj_new(a);
    obj_put(tag, "name", v_str(t->name));
    obj_put(tag, "slug", v_str(t->slug));
    obj_put(tag, "url", v_str(t->url));
    obj_put(tag, "count", v_int((int64_t)t->nposts));
    Value *cards = ARENA_ARR(a, Value, t->nposts ? t->nposts : 1);
    for (size_t j = 0; j < t->nposts; j++) cards[j] = card_value(&b, t->posts[j]);
    obj_put(tag, "posts", v_arr(cards, t->nposts));
    scope_set(top, "page", v_undef());
    scope_set(top, "posts", v_undef());
    scope_set(top, "pager", v_undef());
    scope_set(top, "tag", v_obj(tag));
    U16Buf out;
    ub_init(&out, a);
    render_nodes(a, nodes, tn, top, maps, nmaps, 0, &out);
    U16Buf p;
    ub_init(&p, a);
    ub_put_ascii(&p, "tags/");
    ub_put_u16(&p, t->slug);
    ub_put_ascii(&p, "/index.html");
    out_put(&b, ub_finish(&p), ub_finish(&out));
  }
  {
    size_t tn = 0;
    TNode **nodes = tpl_lookup(maps, nmaps, u16_from_ascii(a, "tags"), &tn);
    if (nodes) {
      scope_set(top, "page", v_undef());
      scope_set(top, "posts", v_undef());
      scope_set(top, "pager", v_undef());
      scope_set(top, "tag", v_undef());
      U16Buf out;
      ub_init(&out, a);
      render_nodes(a, nodes, tn, top, maps, nmaps, 0, &out);
      out_put(&b, u16_from_ascii(a, "tags/index.html"), ub_finish(&out));
    }
  }
  for (size_t i = 0; i < b.nstatics; i++) {
    U16 path = b.statics[i].path;
    if (out_has(&b, path)) {
      U16Buf m;
      ub_init(&m, a);
      ub_put_ascii(&m, "shadowed ");
      ub_put_u16(&m, path);
      ub_put_ascii(&m, ": a built page has its path");
      report_push(&b, ub_finish(&m));
    } else {
      out_put(&b, path, b.statics[i].content);
    }
  }

  // 8. The report.
  size_t nlines = b.nreport + b.nbroken;
  U16 *lines = ARENA_ARR(a, U16, nlines ? nlines : 1);
  size_t nl = 0;
  for (size_t i = 0; i < b.nreport; i++) lines[nl++] = b.report[i];
  for (size_t i = 0; i < b.nbroken; i++) lines[nl++] = b.broken[i];
  qsort(lines, nl, sizeof(U16), cmp_u16);
  size_t npages_count = 0;
  for (size_t i = 0; i < nkept; i++) if (!kept[i].post) npages_count++;
  U16Buf rep;
  ub_init(&rep, a);
  for (size_t i = 0; i < nl; i++) {
    ub_put_u16(&rep, lines[i]);
    ub_put(&rep, '\n');
  }
  ub_put_ascii(&rep, "built ");
  ub_put_int(&rep, (int64_t)npages_count);
  ub_put_ascii(&rep, " pages, ");
  ub_put_int(&rep, (int64_t)nposts);
  ub_put_ascii(&rep, " posts, ");
  ub_put_int(&rep, (int64_t)nby_slug);
  ub_put_ascii(&rep, " tags and ");
  ub_put_int(&rep, (int64_t)b.nstatics);
  ub_put_ascii(&rep, " static files\n");
  out_put(&b, u16_from_ascii(a, "build-report.txt"), ub_finish(&rep));

  *out = b.out;
  *nout = b.nout;
}
