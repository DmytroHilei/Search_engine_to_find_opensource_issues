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

---

# Approved plan: ranked dashboard (not yet implemented)

Approved 2026-09-10, no code written yet. Everything above still governs; this
section is *what to build next*, not a new rule. If it ever contradicts the
rules above, the rules win.

## Why

The daemon pushes up to `NOTIFY_MAX_PER_CYCLE` (10) individual ntfy
notifications per cycle, 8 cycles a day — up to 80/day, ordered chronologically
by the phone. `notify_priority()` already computes 1-5 but only changes how
*loud* one notification is; it orders nothing, so a 5 from 06:00 sits below a 2
from 09:00.

The deeper problem: a push is an *event*, and what the user wants is *state*.
The seen-set is permanent, so an issue glanced at over breakfast is gone forever
even though the bounty is still unclaimed at 15:00. For Tenstorrent that is the
whole game — bounties are assigned within 2-4 days and the value is entirely in
catching them while open.

Outcome: one summary push per cycle, plus a ranked board of everything still
open, re-ranked each cycle, reachable from an iPhone over the internet with no
cable, no Bluetooth and no hosting to operate.

## Decisions (user-confirmed — do not relitigate)

| Fork | Choice |
| --- | --- |
| Delivery | Secret GitHub Gist, PATCHed with the existing `GH_TOKEN` |
| Pushes | One summary push per cycle; `Click:` opens the board |
| Retention | Keep until GitHub says closed **or** assigned |

Gist wins because it reuses `GH_TOKEN` and `net/http.c` and adds no dependency,
no hosting and no new secret. Cost: the token needs `gist` scope, and the gist
id goes in `config.h`.

## Architecture

Three new files, one per existing group — **no fourth group**, so this does not
trip the "adding a group is a design change" rule. Dependencies still point
`pipeline/` → `net/` → `core/`.

```
core/board.c/h      persistence + merge + rank. No network, no presentation.
pipeline/render.c/h board_t -> Markdown. Policy, no I/O.
net/gist.c/h        PATCH /gists/{id}. Takes a rendered string.
```

`main.c` orchestrates; it is the only caller that sees all three.

### Memory

Board entries outlive the cycle arena, so they cannot live in it. Use a fixed
array `calloc`d **once at startup**, exactly like `st->repos` in `state.c` —
startup allocation is permitted, hot-path allocation is not.

```c
#define BOARD_MAX 200

typedef struct {
    long long id;
    char  repo[STATE_REPO_MAX];
    int   number;
    int   llm_score, kw_score;
    char  first_seen[32];      /* RFC3339, when it entered the board */
    char  updated_at[32];
    char  etag[HTTP_ETAG_MAX]; /* per-issue, makes re-checks free */
    char  title[256];
    char  why[LLM_WHY_MAX + 1];
    char  html_url[256];
    int   assigned;
} board_entry_t;
```

~700 B/entry x 200 ~= 140 KB resident. Bounded forever; on overflow evict the
lowest `llm_score`, oldest `first_seen`.

### Persistence

New file `board.tsv` next to `etags`, same atomic discipline as `state_flush()`
— temp file in the same dir, `fflush` → `fsync` → `rename(2)` → directory
`fsync` (rule 9). Consider factoring that dance out of `state.c` into a shared
`core/` helper rather than copying it.

**Escaping is the trap.** Titles carry tabs, newlines, emoji and CJK. On write,
escape `\` `\t` `\n` in `title`/`why`/`html_url`; unescape on read. A round-trip
test over adversarial titles is mandatory. Parse failures skip the line and
never abort — same posture as `load_etags()`.

## The re-check pass (no precedent in the codebase)

The main fetch is `state=open&since=<watermark>` (`github.c`) — deltas only.
**Nothing today would ever tell us an issue closed or got assigned**, so without
this the board silently rots into a list of claimed bounties.

Rejected: flipping the main fetch to `state=all`. Repos like pytorch close
issues constantly; that traffic would blow past the 900-issue cap and the arena.

Do instead, in `gh_recheck()` (new, in `net/github.c`):

1. Take board entries **not** already refreshed by this cycle's fetch.
2. `GET /repos/{o}/{r}/issues/{n}` for each, with `If-None-Match` from the
   stored per-issue ETag, batched through the existing `http_perform_batch()`.
3. `304` → unchanged, keep; costs no rate limit (rule 2).
4. `200` → re-read `state` and `assignees`; drop when closed or assigned,
   otherwise refresh `updated_at`/`etag`/`title`.
5. `404` → deleted or transferred; drop.
6. Transport failure → **keep the entry unchanged**. A network blip must not
   silently empty the board.

Budget: <=200 conditional GETs x 8 cycles/day against 5000/hr authenticated,
most returning 304. Honour `RL_RESERVE` as everywhere else.

### `issue_t` needs one new field

`gh_parse_issues()` does not extract assignment. Add `int assigned` (from
`assignees` non-empty, falling back to `assignee` non-null) to `issue_t` in
`net/github.h`, populated in the parse loop. Add a fixture covering assigned /
unassigned / `null`.

## Ranking and rendering

`board_rank()`: `llm_score` desc, then `first_seen` desc so a fresh 8 outranks a
stale 8, then `id` asc as a deterministic tiebreak — `qsort` is not stable, same
reasoning as `verdict_cmp()` in `judge.c`.

`render_board()` emits Markdown (a gist renders Markdown; it does **not** serve
HTML):

```markdown
# issuewatch — 9 open · updated 2026-09-10 14:02 UTC

| # | score | age | repo | issue |
|---|-------|-----|------|-------|
| 1 | 9 🟢 | 2h | tt-metal | [bf16 matmul NaN on RDNA4](url) |
```

Emoji is fine here — this is a *body*. Rule 5's ASCII constraint applies only to
ntfy headers. Render into the cycle arena via the existing `sbuf_t` pattern in
`judge.c`, and treat overflow as "drop the publish", never a truncated board.

## Cycle order in `run_cycle()`

```
gh_fetch_all → prefilter → judge_batch → judge_apply
board_merge(board, issues, judged)       # upsert by id
gh_recheck(board)                        # drop closed/assigned
board_rank(board)
render_board(board) → markdown
gist_publish(markdown)                   # skipped on --dry-run
notify_summary(...)                      # POST skipped on --dry-run
board_flush(board)                       # skipped on --dry-run
commit_cycle(st, dry_run)                # unchanged
```

**Failure semantics.** A failed gist publish must **not** advance the watermark
— publishing is now the primary output, so rule 4 covers it the way it covers
notification. `board_merge` is keyed by id and therefore idempotent, so a re-run
after failure is safe.

**`--dry-run` must print the rendered board to stdout and issue zero writes** —
no gist PATCH, no ntfy POST, no `board.tsv`. Mirror the existing
`notify_http_calls` hook in `notify.c` with a `gist_http_calls` counter so the
test asserts this structurally rather than by inspection.

## `config.h` additions

```c
#define GIST_ID              "REPLACE_ME_WITH_GIST_ID"
#define GIST_API_BASE        "https://api.github.com/gists"
#define GIST_FILENAME        "issuewatch-board.md"
#define BOARD_MAX            200
#define BOARD_STALE_DAYS     30    /* safety net if a re-check never resolves */
#define NOTIFY_SUMMARY_ONLY  1
```

`GIST_ID` gets placeholder-guarded at startup exactly like `NTFY_TOPIC`: fatal
for a real run, tolerated under `--dry-run`. Cheap extra: read `X-OAuth-Scopes`
off the first GitHub response and warn loudly if `gist` is absent — otherwise
the first PATCH fails with an opaque 404.

## Increments (each builds clean and green; each is one commit)

1. `core/board.c/h` — struct, load/flush with escaping, merge, rank, evict.
   Tests only, no behaviour change.
2. `issue_t.assigned` + parse + fixtures.
3. `gh_recheck()` — conditional GETs, drop rules, failure posture.
4. `pipeline/render.c/h` + golden-output test.
5. `net/gist.c/h` + `gist_http_calls` dry-run invariant.
6. Notify summary mode (`NOTIFY_SUMMARY_ONLY`), `Click:` → gist URL.
7. `config.h`, `CONTEXT.md`, `README.md`.

## Verification

- `make debug && make test` under ASan/UBSan. Baseline is **1,045 checks, 0
  failures**; every increment keeps it green with `-Werror`.
- New tests: board round-trip with tab/newline/emoji/CJK titles; eviction at
  `BOARD_MAX`; rank ordering incl. tiebreaks; re-check drop rules across
  200/304/404/transport-failure; render golden output; `gist_http_calls == 0`
  under `--dry-run`.
- Live: `XDG_STATE_HOME=<scratch> ./issuewatch --oneshot --dry-run -v` with
  `GH_TOKEN=$(gh auth token)`, so real user state is never touched. Confirm the
  board renders, `board.tsv` is **not** written, and no PATCH is issued.
- Then one real `--oneshot`; open the gist on the phone; confirm the summary
  push's `Click:` lands on it.
- Second cycle: entries persist, ages increment, a manually-closed test issue
  disappears.

## Blockers to clear first

- **Ollama is not installed** (`command -v ollama` fails, no `~/.ollama`), so
  the judge leg has never executed end-to-end. The board is only as good as
  `llm_score` — resolve this before the dashboard means anything.
- `NTFY_TOPIC` is still the placeholder, which blocks any non-dry run.
- `CONTEXT.md` section 11 embeds a **stale copy of `config.h`** — it still shows
  `KW_SCORE_MIN 6`, the old repo list, and no `GH_BODY_MAX`. Refresh it or
  replace it with a pointer to the real file.
