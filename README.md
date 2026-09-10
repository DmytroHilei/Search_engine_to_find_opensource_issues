# issuewatch

Polls GitHub issues across a fixed repo list, prefilters with a weighted keyword
automaton, judges the survivors with an LLM, and keeps a ranked board of what is
still open and still worth doing — published as a secret gist, with one summary
push to your phone per cycle. One C11 process, single-threaded, ~1–2 MB idle.

Design rationale is in [CONTEXT.md](CONTEXT.md). Agent build rules are in
[CLAUDE.md](CLAUDE.md).

## Build

```sh
make                    # -O2 -march=native
make debug              # -O0 -g3 -Werror, ASan + UBSan
make test               # build and run every tests/test_*.c
```

Needs `libcurl` ≥ 7.62 (for `curl_multi_poll`) with HTTP/2, and `pkg-config`.
yyjson is vendored in `third_party/yyjson/`; there is nothing else to install.

On Debian/Ubuntu: `apt install libcurl4-openssl-dev pkg-config`.

## Configure

Everything tunable lives in [src/config.h](src/config.h) as preprocessor macros —
watched repos, keyword weights, judge backend, thresholds, resource caps. There
is no runtime config file and no CLI flags for tuning; edit the header and
rebuild.

At minimum, before a real run:

1. `WATCHED_REPOS` — the repos you actually care about.
2. `USER_PROFILE` — what you work on. This goes into the LLM system prompt and
   is the single biggest lever on result quality.
3. `KEYWORDS[]` — expect a few tuning iterations. Use `--dry-run` for that.
4. `NTFY_TOPIC` — **replace the default.** Generate one with
   `openssl rand -hex 16`.
5. `GIST_ID` — the board is published here. Create one secret gist, once, and
   paste its id (the hex from the URL, not the whole URL):

   ```sh
   gh gist create --secret -d issuewatch board.md
   ```

   Your `GH_TOKEN` needs `gist` scope for this, which a fine-grained PAT cannot
   grant — use a classic token with `gist` plus public-repo read. Leave
   `GIST_ID` at the placeholder and the board simply does not publish; the
   daemon still runs, and `--dry-run` still prints the board to stdout.

### Why the topic matters

A public ntfy.sh topic is readable *and writable* by anyone who knows its name.
The read side leaks only public GitHub data, but the write side means anyone who
guesses your topic can push arbitrary notifications to your phone. Use a long
random topic, or self-host ntfy with access control and set `NTFY_TOKEN`.
`notify_init()` refuses to run with the shipped placeholder.

## Secrets

Never in `config.h`. Read from the environment at startup:

| Variable | Required |
| --- | --- |
| `GH_TOKEN` | always — needs `gist` scope to publish the board, so a classic token; a fine-grained PAT with public-repo read is enough only with `GIST_ID` left unset |
| `ANTHROPIC_API_KEY` | only for `JUDGE_API` / `JUDGE_HYBRID` |
| `NTFY_TOKEN` | only for a self-hosted ntfy with auth |

## Run

```sh
./issuewatch --dry-run      # print to stdout, send nothing. Tune with this.
./issuewatch --oneshot      # one cycle, exit. What the timer runs.
./issuewatch --daemon       # sleep-loop. Supported, but see below.
```

### Prefer the timer over the daemon

```sh
mkdir -p ~/.config/systemd/user ~/.config/issuewatch
install -m 0600 /dev/null ~/.config/issuewatch/env   # then add GH_TOKEN=...
cp systemd/issuewatch.{service,timer} ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now issuewatch.timer
systemctl --user list-timers issuewatch.timer
```

Between runs the process does not exist, so idle RSS is 0 rather than "small".
You also get restart backoff, journal logging, and `Persistent=true`, which
catches up a run missed while the laptop was asleep. `--daemon` uses
`clock_nanosleep(TIMER_ABSTIME)` so it does not drift, and `malloc_trim(0)` after
each cycle so glibc returns the cycle's peak to the kernel — but it cannot give
you any of the rest.

## The board

Each cycle writes a ranked Markdown board to your secret gist and sends **one**
summary push whose `Click:` opens it. Entries stay until GitHub says the issue
is closed or somebody is assigned to it — not until you have looked at them.

That last part is the whole point. A push is an event: glance at it over
breakfast and it is gone, even though the bounty is still unclaimed at 15:00.
The board is state, so it is still there at 15:00, re-ranked, with the age of
each entry visible.

Keeping it honest costs one conditional `GET` per entry per cycle — a `304` for
almost all of them, which costs no rate limit — because the main `since=` fetch
returns deltas and would never tell us an issue had closed or been claimed.
See [CONTEXT.md](CONTEXT.md) section 14 for the drop rules and why
`state=all` was rejected.

Set `NOTIFY_SUMMARY_ONLY` to `0` to go back to one push per issue.

## How the cost is kept down

Ranked by actual impact:

1. **ETag / `If-None-Match`** — a 304 costs no body and no rate-limit quota, so
   the steady state is nearly free.
2. **Keyword prefilter before the LLM** — drops 85–95% of traffic, which saves
   far more than any inference-level tuning.
3. **`since=` watermark** — bounds each payload to what changed.
4. **Batched judging** — `LLM_BATCH_SIZE` issues per request, so 8x fewer round
   trips, and the model can compare candidates against each other.
5. **Ollama keep-alive unload** — 0 VRAM between cycles.
6. **HTTP/2 multiplexing** — one TLS handshake per cycle for every repo.

## Layout

Sources are grouped by role. Includes are path-qualified from `src/`, so
`#include "net/github.h"` tells you which group a dependency comes from.

```
src/
  main.c          argv, mode dispatch, signals, cycle orchestration
  config.h        ALL tunables. The only file you edit.

  core/           no network, no policy -- memory and durability
    arena.c/h     bump allocator, reset once per cycle
    util.c/h      logging, RFC3339, hashing, ASCII sanitising
    state.c/h     mmap'd seen-set, atomic etag/watermark rewrite

  net/            everything that speaks HTTP
    http.c/h      curl_multi wrapper: HTTP/2, gzip, rate-limit headers
    github.c/h    issue fetch, ETag cache, watermark, pagination, PR filter
    notify.c/h    ntfy POST, ASCII header sanitising, per-cycle cap

  pipeline/       decides which issues are worth your attention
    prefilter.c/h hand-written Aho-Corasick build + weighted score
    judge.c/h     LLM batching; three backends behind judge_batch()

third_party/yyjson/
tests/            plain assertions, recorded JSON fixtures, no framework
systemd/          user service + timer
build/            all objects, dep files and test binaries (gitignored)
```

`build/` mirrors the source tree: `src/net/http.c` compiles to
`build/src/net/http.o`, and test binaries land in `build/tests/`. `make clean`
is just `rm -r build`.
