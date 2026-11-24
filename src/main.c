#define _POSIX_C_SOURCE 200809L // clock_gettime, open/read/write
// main.c — the native entry point. Reads <input>/cases.json, answers every
// case with build_run, writes <output>/answers.json (JSON.stringify).
// Usage: sitegen [input_dir] [output_dir]
//   argv wins; else MAKEFUN_INPUT_DIR / MAKEFUN_OUTPUT_DIR; else /input /output.
// Silent on success (exit 0); SITEGEN_BENCH=1 prints timing to stderr.
#include "sitegen.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint8_t *read_file(const char *path, size_t *len_out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("cannot open %s", path);
  struct stat st;
  if (fstat(fd, &st) != 0) die("cannot stat %s", path);
  size_t len = (size_t)st.st_size;
  uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
  if (!buf) die("out of memory");
  size_t off = 0;
  while (off < len) {
    ssize_t r = read(fd, buf + off, len - off);
    if (r < 0) die("cannot read %s", path);
    if (r == 0) break;
    off += (size_t)r;
  }
  close(fd);
  *len_out = off;
  return buf;
}

static void write_file(const char *path, const uint8_t *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("cannot open %s for writing", path);
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, data + off, len - off);
    if (w < 0) die("cannot write %s", path);
    off += (size_t)w;
  }
  if (close(fd) != 0) die("cannot close %s", path);
}

static JVal *jstr(Arena *a, U16 s) {
  JVal *v = ARENA_NEW(a, JVal);
  v->t = J_STR;
  v->u.s = s;
  return v;
}

static JVal *jobj_new(Arena *a) {
  JVal *v = ARENA_NEW(a, JVal);
  v->t = J_OBJ;
  v->u.o.k = NULL;
  v->u.o.v = NULL;
  v->u.o.n = 0;
  v->u.o.cap = 0;
  return v;
}

// JVal objects keep keys and values in two parallel arrays sharing one cap.
static void jobj_grow(Arena *a, JVal *obj) {
  size_t nc = obj->u.o.cap ? obj->u.o.cap * 2 : 8;
  U16 *nk = ARENA_ARR(a, U16, nc);
  JVal **nv = ARENA_ARR(a, JVal *, nc);
  if (obj->u.o.n) {
    memcpy(nk, obj->u.o.k, sizeof(U16) * obj->u.o.n);
    memcpy(nv, obj->u.o.v, sizeof(JVal *) * obj->u.o.n);
  }
  obj->u.o.k = nk;
  obj->u.o.v = nv;
  obj->u.o.cap = nc;
}

static void jobj_put(Arena *a, JVal *obj, U16 key, JVal *val) {
  if (obj->u.o.n == obj->u.o.cap) jobj_grow(a, obj);
  obj->u.o.k[obj->u.o.n] = key;
  obj->u.o.v[obj->u.o.n] = val;
  obj->u.o.n++;
}

static JVal *jarr_new(Arena *a) {
  JVal *v = ARENA_NEW(a, JVal);
  v->t = J_ARR;
  v->u.a.v = NULL;
  v->u.a.n = 0;
  v->u.a.cap = 0;
  return v;
}

static void jarr_push(Arena *a, JVal *arr, JVal *val) {
  AGROW(a, arr->u.a.v, arr->u.a.n, arr->u.a.cap, JVal *);
  arr->u.a.v[arr->u.a.n++] = val;
}

static char *join_path(Arena *a, const char *dir, const char *file) {
  size_t dl = strlen(dir), fl = strlen(file);
  int slash = dl > 0 && dir[dl - 1] != '/';
  char *p = ARENA_ARR(a, char, dl + (size_t)slash + fl + 1);
  memcpy(p, dir, dl);
  size_t n = dl;
  if (slash) p[n++] = '/';
  memcpy(p + n, file, fl);
  p[n + fl] = 0;
  return p;
}

int main(int argc, char **argv) {
  const char *input_dir = argc > 1 ? argv[1] : getenv("MAKEFUN_INPUT_DIR");
  if (!input_dir) input_dir = "/input";
  const char *output_dir = argc > 2 ? argv[2] : getenv("MAKEFUN_OUTPUT_DIR");
  if (!output_dir) output_dir = "/output";
  int bench = getenv("SITEGEN_BENCH") != NULL;

  struct timespec t0, t1, tp0, tp1, tb0, tb1, tw0, tw1;
  double parse_ms = 0, build_ms = 0, write_ms = 0;
  if (bench) clock_gettime(CLOCK_MONOTONIC, &t0);

  Arena arena;
  arena_init(&arena, 64u << 20);

  char *cases_path = join_path(&arena, input_dir, "cases.json");
  size_t input_len = 0;
  uint8_t *input_data = read_file(cases_path, &input_len);

  if (bench) clock_gettime(CLOCK_MONOTONIC, &tp0);
  JVal *cases = json_parse(&arena, input_data, input_len);
  if (bench) { clock_gettime(CLOCK_MONOTONIC, &tp1); parse_ms = (tp1.tv_sec - tp0.tv_sec) * 1000.0 + (tp1.tv_nsec - tp0.tv_nsec) / 1e6; }
  if (!cases || cases->t != J_ARR) die("cases.json is not an array");
  size_t ncases = cases->u.a.n;

  JVal *answers = jarr_new(&arena);
  if (bench) clock_gettime(CLOCK_MONOTONIC, &tb0);
  for (size_t i = 0; i < ncases; i++) {
    JVal *input = jget(cases->u.a.v[i], "input");
    if (!input) die("case %zu has no input", i);
    OutEnt *out = NULL;
    size_t nout = 0;
    build_run(&arena, input, &out, &nout);
    JVal *ans = jobj_new(&arena);
    for (size_t k = 0; k < nout; k++) {
      jobj_put(&arena, ans, out[k].path, jstr(&arena, out[k].content));
    }
    jarr_push(&arena, answers, ans);
  }

  if (bench) { clock_gettime(CLOCK_MONOTONIC, &tb1); build_ms = (tb1.tv_sec - tb0.tv_sec) * 1000.0 + (tb1.tv_nsec - tb0.tv_nsec) / 1e6; }
  ByteBuf bb;
  bb_init(&bb, &arena);
  if (bench) clock_gettime(CLOCK_MONOTONIC, &tw0);
  json_write_value(&bb, answers);
  if (bench) { clock_gettime(CLOCK_MONOTONIC, &tw1); write_ms = (tw1.tv_sec - tw0.tv_sec) * 1000.0 + (tw1.tv_nsec - tw0.tv_nsec) / 1e6; }

  char *answers_path = join_path(&arena, output_dir, "answers.json");
  write_file(answers_path, bb.s, bb.n);

  if (bench) {
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    // CLOCK_PROCESS_CPUTIME_ID is already the process's total CPU time.
    struct timespec tc;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &tc);
    double cpu_ms = tc.tv_sec * 1000.0 + tc.tv_nsec / 1e6;
    fprintf(stderr, "sitegen: %zu cases, %zu input bytes -> %zu output bytes, %.1f ms wall, %.1f ms cpu, arena %.1f MiB\n",
            ncases, input_len, bb.n, ms, cpu_ms, arena_used(&arena) / 1048576.0);
    fprintf(stderr, "sitegen: phases parse %.1f ms, build %.1f ms, write %.1f ms\n", parse_ms, build_ms, write_ms);
  }
  return 0;
}
