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
Those three are committed. The next two are credentials, so they go in
`src/config.local.h`, which is gitignored — `config.h` includes it if present
and only defines placeholders for what it did not set:

```sh
cp src/config.local.h.example src/config.local.h
```

4. `NTFY_TOPIC` — **replace the default.** Generate one with
   `openssl rand -hex 16`. This one is not a label, it is the whole of the
   authentication; see below.
5. `GIST_ID` — the board is published here. Create one secret gist, once, and
   paste its id (the hex from the URL, not the whole URL):

   ```sh
   printf '# issuewatch\n' > /tmp/issuewatch-board.md
   gh gist create -d issuewatch /tmp/issuewatch-board.md
   ```

   Secret is `gh gist create`'s default; `--public` is the opt-out. Seed it
   under the name `GIST_FILENAME` already uses — the publish PATCHes that one
   file, so a differently named seed is not replaced, it is joined.

   Your `GH_TOKEN` needs `gist` scope for this, which a fine-grained PAT cannot
   grant — use a classic token with `gist` plus public-repo read. Leave
   `GIST_ID` at the placeholder and the board simply does not publish; the
   daemon still runs, and `--dry-run` still prints the board to stdout.

### Why the topic matters

A public ntfy.sh topic is readable *and writable* by anyone who knows its name.
The read side leaks only public GitHub data, but the write side means anyone who
guesses your topic can push arbitrary notifications to your phone — any title,
any body, `Priority: 5` so it breaks through Focus. Use a long random topic, or
self-host ntfy with access control and set `NTFY_TOKEN`. `notify_init()` refuses
to run with the shipped placeholder.

*Knows* includes reading it here: this repo is public, so a real topic committed
to `config.h` is a published credential. That is why it lives in the untracked
`src/config.local.h` instead. Nothing enforces this but the gitignore — check
`git diff --cached` before a push that touches configuration.

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
    fileio.c/h    the atomic rewrite both state files share
    state.c/h     mmap'd seen-set, etag/watermark file
    board.c/h     the ranked board: load, merge, rank, evict, board.tsv

  net/            everything that speaks HTTP
    http.c/h      curl_multi wrapper: HTTP/2, gzip, rate-limit headers
    github.c/h    issue fetch, ETag cache, watermark, pagination, PR filter,
                  and the per-issue re-check that keeps the board honest
    notify.c/h    ntfy POST, ASCII header sanitising, cycle summary
    gist.c/h      PATCH the board into one secret gist

  pipeline/       decides which issues are worth your attention
    prefilter.c/h hand-written Aho-Corasick build + weighted score
    judge.c/h     LLM batching; three backends behind judge_batch()
    render.c/h    board -> Markdown

third_party/yyjson/
tests/            plain assertions, recorded JSON fixtures, no framework
systemd/          user service + timer
build/            all objects, dep files and test binaries (gitignored)
```

`build/` mirrors the source tree: `src/net/http.c` compiles to
`build/src/net/http.o`, and test binaries land in `build/tests/`. `make clean`
is just `rm -r build`.

## Next steps

The daemon is complete and tested. What is left is configuration and one
judgement call, in the order you should do them.

**1. Set `NTFY_TOPIC`.** `openssl rand -hex 16`. `notify_init()` refuses to run
with the placeholder, and for good reason — see above.

**2. Create the gist and set `GIST_ID`.**

```sh
printf '# issuewatch\n' > /tmp/issuewatch-board.md
gh gist create -d issuewatch /tmp/issuewatch-board.md
```

Take the hex id from the URL. The token needs `gist` scope, which means a
**classic** token — a fine-grained PAT cannot write gists. Get this wrong and
the PATCH fails with a bare `404` that looks exactly like a wrong id; the error
message prints the scopes your token actually presents, so read it.

**3. Pick a judge model that can count.** This is the real open question, and
there is measured evidence for it below.

`qwen3:4b` was run against 107 real prefiltered issues in 14 batches:

| Verdicts returned | Batches |
| --- | --- |
| 8 of 8 | 6 |
| 7 of 8 | 4 |
| 4 of 8 | 2 |
| 3 of 8 | 1 |
| 0 of 8 | 1 |

Half the batches came back short, and every missing verdict is a candidate
silently discarded — lost to the model, not to your thresholds. Worse, several
`why` strings arrived attached to the wrong issue: `judge_parse_verdicts()`
applies each verdict by the model's own bounds-checked `"i"` field, so the
mapping is right and the model's indices are wrong.

Two ways out, cheapest first: `OLLAMA_MODEL "qwen3:8b"` (~5.5 GB, fits an 8 GB
card with the batch), or `JUDGE_MODE JUDGE_API` with Haiku, which is what the
batching was designed around. Re-run and compare the table above before
trusting the scores.

**4. Then a real cycle.**

```sh
GH_TOKEN=$(gh auth token) ./issuewatch --oneshot
```

Open the gist on your phone. Run it a second time and confirm the entries
persist, the ages increment, and anything you close disappears from the board.

### If you installed Ollama without root

The official installer needs root and sets up a system service. A rootless
install puts the binary at `~/.local/bin/ollama` with no service, so start the
server yourself and set the idle unload window **there** — not in
`systemd/issuewatch.service`, where it does nothing:

```sh
OLLAMA_KEEP_ALIVE=2m ollama serve
```

`--oneshot` will simply fail its judge batches if the server is not up, publish
the board it already had, and leave the watermark alone for a retry.
