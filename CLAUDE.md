# CLAUDE.md

Instructions for Claude Code agents working in this repo.
Read `CONTEXT.md` first — it holds the design rationale and the GitHub API
gotchas. This file is rules, not background.

## Project

`issuewatch` — a small C daemon. Polls GitHub issues for a fixed repo list,
prefilters with a weighted keyword automaton, judges survivors with an LLM,
and keeps a ranked board of what is still open and still worth doing --
published to a secret gist, with one summary push to ntfy per cycle. Runs every
~3 hours. Single-threaded.

## Non-negotiables

- **C11.** No C++, no C++ headers, no `-std=gnu++`.
- **Tuning lives in `src/config.h` as macros; the personal layer lives in a
  config file.** The split is by kind, and it is the whole rule:

  - `config.h` holds anything with a *right answer* — batch sizes, context
    window, thresholds, timeouts, retention. These were tuned against a measured
    corpus on specific hardware, and a knob nobody re-measures is worse than no
    knob. No runtime override, no env var, no flag.
  - `core/userconf.c` reads the settings that differ per person and have no
    right answer: `repo`, `profile`, `keyword`/`label-keyword`, `ntfy-topic`,
    `gist-id`. Default `$XDG_CONFIG_HOME/issuewatch/config`, or `--config PATH`.
    `config.h` still holds the *defaults* for each, key by key, so an absent or
    partial file still runs. `src/config.example` is the tracked template and is
    verified against those defaults by `tests/test_userconf.c` — update it in
    the same commit as any `WATCHED_REPOS`, `USER_PROFILE` or `KEYWORDS` edit.

  Do not move a `config.h` tunable into the file because it would be convenient;
  that is a design change, so ask. Adding a *setting* to the file means adding a
  key to `apply_line()`, a default in `userconf_defaults()`, a line in
  `config.example` and a test — an unknown key is a hard error, deliberately, so
  a typo cannot silently leave the daemon watching the author's repositories.

  `--model NAME` is the one argv tuning exception and stays narrow: it overrides
  `OLLAMA_MODEL` only, because which model fits is bounded by the GPU in the
  machine and "does this fit my card" is answered by trying it. Do not grow argv
  into a general flag surface.
- **Single-threaded.** Concurrency is `curl_multi` + `curl_multi_poll`. Do not
  add `pthread_create`. If you think you need a thread, you are solving the
  wrong problem.
- **No allocation in the hot path.** One arena per cycle, bump-pointer,
  `arena_reset()` at cycle end. `malloc` only during startup and for libcurl's
  own internals.
- **No secrets in tracked files.** `GH_TOKEN`, `ANTHROPIC_API_KEY`,
  `NTFY_TOKEN` come from `getenv`. Never write a token into `config.h`, a test
  fixture, a log line, or a commit message.
  `NTFY_TOPIC` counts as a secret even though it looks like a setting: on
  ntfy.sh the topic name *is* the authentication, world-readable and
  world-writable, so a real one in this public repo lets any reader push
  notifications to the user's phone. `GIST_ID` is milder — a secret gist is
  unlisted, not private — but still names a page that was not published
  deliberately. Both come from the user's config file, which is untracked and
  should be `chmod 600`; `config.h` and `src/config.example` keep only the
  `REPLACE_ME_*` placeholders, and `notify_init()` and `gist_init()` hard-fail
  on those rather than publishing to an address someone else can read.

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
    fileio.c/h    the atomic rewrite: temp file, fsync, rename, fsync dir
    state.c/h     mmap seen-set, etag/watermark file
    userconf.c/h  the personal config file: repos, profile, keywords, secrets
    board.c/h     the ranked board: load, merge, rank, evict, board.tsv
  net/            everything that speaks HTTP
    http.c/h      curl_multi wrapper, gzip, HTTP/2 multiplex, rate-limit headers
    github.c/h    issue fetch, ETag cache, since-watermark, pagination, PR filter
                  plus gh_recheck(), which is what keeps the board honest
    notify.c/h    ntfy POST, header sanitising, per-cycle cap, cycle summary
    gist.c/h      PATCH the board into one secret gist
  pipeline/       what is worth reporting
    prefilter.c/h Aho-Corasick build + score
    judge.c/h     LLM batching; backends behind one `judge_batch()` interface
    render.c/h    board -> Markdown
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
   `rename(2)`. A power cut must not leave a truncated etag cache. Use
   `fileio_atomic_write()`; do not write that dance a third time.
10. **A transport failure keeps a board entry.** `gh_recheck()` drops an entry
    only on positive evidence -- GitHub saying closed, assigned, or 404. A
    timeout, a 5xx or a rate-limit stop keeps it. Anything else means a network
    blip silently empties the board, which is the one failure it exists to
    prevent.
11. **`--dry-run` writes nothing.** No ntfy POST, no gist PATCH, no `board.tsv`,
    no watermark, no etag file, no seen-set marking. Every module that can write
    carries a counter its tests assert on (`notify_http_calls`,
    `gist_http_calls`, `state_t::read_only`). Check that after touching any of
    them -- the teardown path has already broken this invariant once.
12. **Truncate on a UTF-8 boundary, never on a byte.** Board fields are fixed
    arrays and GitHub titles are full of emoji and CJK. A clipped code point is
    invalid UTF-8, yyjson refuses to encode it, and the whole cycle publishes
    nothing. `utf8_trunc_len()` in `core/util.h` exists for this, and
    `text_trunc_clean()` beside it does the same job for prose that a person
    reads -- it backs up to the last sentence or word so a notification body
    does not stop mid-token. `render.c` drops malformed sequences as a backstop
    -- do not treat that as permission to emit them.
13. **A failed publish must not advance the watermark.** The gist is the
    cycle's primary output now, so rule 4 covers it exactly as it covers
    notification. A render that overflows is a failure, not a short board: a
    truncated board reads as "this is everything open".
14. **Whether an issue is paid is decided in C, not by the model.**
    `payload_shows_payment()` reads the title and labels, the answer goes into
    the prompt as the `payment:` line, and `judge_parse_verdicts()` enforces it
    on the way back: unpaid is capped at `JUDGE_UNPAID_CAP`, paid is floored at
    `JUDGE_PAID_FLOOR` even against `keep=false`. Both directions are load-
    bearing and both were observed live -- an 8B judge invented a bounty for an
    issue labelled only "module: cuda", and scored tinygrad #3039 zero while its
    own reason read "$500 bounty for parallel scan". Do not relax either one
    into a prompt instruction; that is where they started.
15. **Re-measure the judge prompt, never just read it.** The rubric in
    `JUDGE_SYSTEM_PROMPT` is tuned against a rescore of ~60 real issues at
    `LLM_BATCH_SIZE`, and edits that read as clarifications have twice collapsed
    it -- naming the criteria made the model echo the names for 59 of 60 rows,
    and one sentence about `keep=false` took rejections from 19 of 60 to 57 of
    60. The comment above the macro lists what was tried.

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
- `tests/test_pipeline.c` tests the *seam*, not a module: it truncates a field
  the way a caller does, renders it, then encodes it as a gist body. It exists
  because a bug lived exactly there while every per-module suite passed -- one
  side produced output that looked right and the other only ever received
  well-formed input. Extend it when you add a stage to the publish path.
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

# The ranked board

Built and merged. The rationale, the drop-rule table and the failure modes are
in `CONTEXT.md` section 14 -- read that before changing any of it, because most
of what looks arbitrary there was paid for.

Cycle order in `run_cycle()`, which is the part worth knowing by heart:

```
gh_fetch_all -> prefilter -> judge_batch -> judge_apply
board_merge        upsert by id; skips ids in the seen-set
gh_recheck         conditional GET per stale entry; drops closed/assigned/404
board_expire, board_rank
render_board       NULL means skip the publish, never publish partial
gist_publish       skipped on --dry-run
notify_summary     one push; NOTIFY_SUMMARY_ONLY picks this or notify_cycle
board_flush        skipped on --dry-run
commit_cycle       watermarks last, and only if everything above worked
```

The board phase runs even when the fetch returns nothing. A quiet cycle is
exactly when the board is most likely to be lying: nothing new arrived, but the
rows on it have aged and some were claimed an hour ago. A judge outage is the
one failure that does not stop the publish -- it stops the watermark instead.
