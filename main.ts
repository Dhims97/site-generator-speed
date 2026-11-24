// Site Generator Speed v2: a native C binary first, the pure-JS solver as fallback.
// The harness runs `node /bot/main.ts` (node22). It reads every case from
// /input/cases.json (a JSON array of {id, input}), answers each one alone,
// and writes /output/answers.json: a JSON array of the answers, one per case
// and in the cases' order. Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// The native binary (bin/sitegen-x86_64 or bin/sitegen-aarch64, picked by
// process.arch) is a static, self-contained port of solve.js: same output
// tree, byte for byte, only faster. GitHub does not preserve the executable
// bit on push, so the binary is copied to a temp dir and chmodded before it
// is run. If the binary is missing, the wrong architecture, or exits
// non-zero for any reason, the entry falls back to solve.js (the v1 pure-JS
// solver, byte-identical, slower) so the entry still answers every case.

import { readFileSync, writeFileSync, copyFileSync, chmodSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { setFlagsFromString } from 'node:v8';

// V8's incremental marking off, set before solve.js is imported so all of
// its work runs under it (the rules: the house baseline runs with
// --no-incremental-marking, and an entry may tune V8's flags too).
setFlagsFromString('--no-incremental-marking');

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const here = dirname(fileURLToPath(import.meta.url));

/** Runs the prebuilt native binary. Returns true only if it exited 0. */
function tryNative(): boolean {
  const arch = process.arch;
  const name = arch === 'x64' ? 'sitegen-x86_64' : arch === 'arm64' ? 'sitegen-aarch64' : null;
  if (name === null) return false;
  let dir: string;
  let bin: string;
  try {
    dir = mkdtempSync(join(tmpdir(), 'sitegen-'));
    bin = join(dir, name);
    copyFileSync(join(here, 'bin', name), bin);
    chmodSync(bin, 0o755);
  } catch {
    return false;
  }
  let ok = false;
  try {
    const r = spawnSync(bin, [INPUT_DIR, OUTPUT_DIR], { stdio: 'inherit' });
    ok = r.error === undefined && r.status === 0;
  } catch {
    ok = false;
  }
  try {
    rmSync(dir, { recursive: true, force: true });
  } catch {
    // The temp dir is best-effort cleanup; a leftover harms nothing.
  }
  return ok;
}

if (!tryNative()) {
  // Fallback: the pure-JS v1 solver (byte-identical, slower).
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(readFileSync(join(INPUT_DIR, 'cases.json'), 'utf8')) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(join(OUTPUT_DIR, 'answers.json'), JSON.stringify(answers));
}
