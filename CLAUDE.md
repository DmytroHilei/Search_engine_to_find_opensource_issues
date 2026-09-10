# CLAUDE.md

Instructions for Claude Code agents working in this repo.
Read `CONTEXT.md` first — it holds the design rationale and the GitHub API
gotchas. This file is rules, not background.

## Project

`issuewatch` — a small C daemon. Polls GitHub issues for a fixed repo list,
prefilters with a weighted keyword automaton, judges survivors with an LLM,
pushes hits to ntfy. Runs every ~3 hours. Single-threaded.

## Non-negotiables

- **C11.** No C++, no C++ headers, no `-std=gnu++`.
- **All user configuration lives in `src/config.h`** as preprocessor macros.
  There is no runtime config file, no CLI flags for tuning, no env-var
  overrides for behaviour. Env vars are for *secrets only*.
- **Single-threaded.** Concurrency is `curl_multi` + `curl_multi_poll`. Do not
  add `pthread_create`. If you think you need a thread, you are solving the
  wrong problem.
- **No allocation in the hot path.** One arena per cycle, bump-pointer,
  `arena_reset()` at cycle end. `malloc` only during startup and for libcurl's
  own internals.
- **No secrets in tracked files.** `GH_TOKEN`, `ANTHROPIC_API_KEY`,
  `NTFY_TOKEN` come from `getenv`. Never write a token into `config.h`, a test
  fixture, a log line, or a commit message.

## Dependencies

Keep this list closed. Adding one requires an explicit decision, not an agent's
judgement call.

| Dep | Why | Source |
| --- | --- | --- |
| libcurl (≥ 7.62, needs `curl_multi_poll`) | HTTP | system, `pkg-config libcurl` |
| yyjson | JSON parse + build | vendored in `third_party/yyjson/`, 2 files |

Aho–Corasick is written by hand in `src/pipeline/prefilter.c`. Do not pull in a
matching library for ~200 patterns.

## Layout

```
src/
  main.c          argv, mode dispatch (--oneshot | --daemon | --dry-run), signals
  config.h        ALL tunables. The only file the user edits.
  core/           memory and durability; no network, no policy
    arena.c/h     bump allocator
    util.c/h      logging, RFC3339, hashing, ASCII sanitising
    state.c/h     mmap seen-set, atomic etag/watermark file rewrite
  net/            everything that speaks HTTP
    http.c/h      curl_multi wrapper, gzip, HTTP/2 multiplex, rate-limit headers
    github.c/h    issue fetch, ETag cache, since-watermark, pagination, PR filter
    notify.c/h    ntfy POST, header sanitising, per-cycle cap
  pipeline/       what is worth reporting
    prefilter.c/h Aho-Corasick build + score
    judge.c/h     LLM batching; backends behind one `judge_batch()` interface
third_party/yyjson/
build/            every .o, .d and test binary, mirroring the source tree
Makefile
```

Includes are path-qualified from `src/` (`-Isrc`): write
`#include "net/http.h"`, not `#include "http.h"`. A new file goes in the group
whose description already covers it; adding a fourth group is a design change,
so ask first. Dependencies point `pipeline/` → `net/` → `core/`; do not make
`core/` include from `net/` or `pipeline/`.

Nothing is ever written into `src/` or `tests/` by the build. Objects go to
`build/src/...`, test binaries to `build/tests/`, the flags stamp to
`build/.buildflags`. If you add a build rule, `mkdir -p $(@D)` in the recipe
rather than committing a placeholder directory.

## Build

```sh
make            # -O2 -march=native -Wall -Wextra -Wpedantic
make debug      # -O0 -g3 -fsanitize=address,undefined
make run        # ./issuewatch --dry-run
```

`-Werror` is on for `make debug`. Warnings are bugs. Do not silence one with a
cast or a `(void)x` unless the variable is genuinely unused by design.

## Correctness rules an agent will get wrong by default

1. **Filter out pull requests.** Every PR appears in `/repos/{o}/{r}/issues`.
   Skip objects where the `pull_request` key exists. This is the single most
   common mistake with this endpoint.
2. **`304 Not Modified` is a success path, not an error.** It means nothing
   changed. Advance nothing, log nothing, move on.
3. **Never parse the `Link` header with a regex.** Walk it, or just stop
   paginating when `updated_at <= watermark` given `sort=updated&direction=desc`.
4. **Only advance the watermark after the whole repo's cycle succeeded**,
   including notification. A crash mid-cycle must re-process, not skip.
5. **ntfy headers must be ASCII.** GitHub issue titles contain emoji and CJK.
   Strip or RFC 2047-encode before setting `Title:`. Non-ASCII in a header
   silently truncates or 400s.
6. **Constrain LLM output with a schema** (`format` for Ollama, a tool
   definition for Anthropic). Never regex prose out of a completion. If the
   response doesn't parse, drop the batch and log — do not retry in a loop.
7. **Truncate issue bodies** to `LLM_BODY_TRUNC` before prompting. Keep head
   and tail, drop the middle.
8. **Honour `Retry-After` and `X-RateLimit-Remaining`.** Stop issuing requests
   when remaining drops below `RL_RESERVE`.
9. **Write state atomically**: temp file in the same directory, `fsync`,
   `rename(2)`. A power cut must not leave a truncated etag cache.

## Style

- 4 spaces, no tabs. 100 columns.
- `snake_case` for functions and variables, `SCREAMING_CASE` for macros,
  `typedef struct {...} thing_t;`.
- Functions return `int`: `0` on success, negative errno-style on failure.
  Out-params by pointer. No global error state.
- Every `.c` file has one `#include` of its own header first, then system, then
  third-party, then local.
- Comments explain *why*, not *what*. Do not narrate the code.
- No `goto` except the single-exit cleanup idiom (`goto out;`), which is fine
  and preferred over nested frees.

## Testing

- `tests/` uses plain assertions and a `make test` target. No framework.
- Network code is tested against recorded JSON fixtures in `tests/fixtures/`,
  not against live GitHub. Add a fixture when you add a parse path.
- **Always run `make debug && make test` under ASan/UBSan before declaring
  done.** The parser handles untrusted network input; a missing bounds check
  here is a real bug, not a style nit.
- `--dry-run` must never send an HTTP POST to ntfy. Verify this holds after any
  change to `notify.c`.

## What to do when the design is unclear

Ask. Do not invent a config format, add a dependency, introduce threading, or
switch to sqlite for state because it seemed easier. The constraints above are
deliberate; if one genuinely blocks a requirement, say so and stop rather than
routing around it.
