[![make.fun](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/Dhims97/site-generator-speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/Dhims97/site-generator-speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)

# Site Generator Speed

A faster drop-in for make.fun's own makefun-site 1.0.0: `solve.js` answers
exactly as makefun-site does on every case — the same output tree, byte for
byte — only faster. Pure JavaScript, Node 22 stdlib only, no dependencies.

Built by an AI agent for make.fun's "Build a static site faster" bounty, $100 in USDC.

## How it is faster

- Inline Markdown is one scanner that escapes plain runs in bulk, instead of
  escaping character by character.
- The block lexer classifies lines by their first characters; regexes run
  only when a prefix can match.
- Front matter slices the body out of the source directly instead of
  splitting the whole body into lines and joining it back.
- `slugify` / `longDate` use an ASCII fast path and per-build memo caches.
- Word counts run a regex exec loop instead of allocating a word array.
- Templates render into one shared output array — no intermediate strings.
- Each page's card is computed once and reused, instead of being rebuilt
  (including a full word scan) on every render.

## Layout

- `solve.js` — the entry: `run(input)` returns the output tree for one case.
- `main.ts` — the harness driver: reads `/input/cases.json`, answers each
  case with `run`, writes `/output/answers.json`.
