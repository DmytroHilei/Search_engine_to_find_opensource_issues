# issuewatch

Polls GitHub issues across a fixed repo list, prefilters with a weighted keyword
automaton, judges the survivors with an LLM, and keeps a ranked board of what is
still open and still worth doing — published as a secret gist, with one summary
push to your phone per cycle. One C11 process, single-threaded, ~1–2 MB idle.

Design rationale is in [CONTEXT.md](CONTEXT.md). Agent build rules are in
[CLAUDE.md](CLAUDE.md).

## Requirements

| What | Needed for | Size |
| --- | --- | --- |
| C11 compiler, `make`, `pkg-config` | building | — |
| `libcurl` ≥ 7.62 with headers (HTTP/2 preferred) | building; `curl_multi_poll` is the I/O loop | — |
| yyjson | JSON | vendored in `third_party/`, nothing to install |
| [Ollama](https://ollama.com/download) | `JUDGE_LOCAL` / `JUDGE_HYBRID` only | — |
| `qwen3:8b` weights (`OLLAMA_MODEL`) | the default local judge | 5.2 GB disk, ~6 GB loaded |
| `qwen3:4b` weights (`OLLAMA_MODEL_SMALL`) | optional, a card that cannot hold the 8B | 2.5 GB disk |
| `qwen3:1.7b` weights (`OLLAMA_SCREEN_MODEL`) | `JUDGE_HYBRID` only | ~1.4 GB disk |
| `gh` CLI | optional: supplies `GH_TOKEN`, creates the gist | — |

One command installs what is missing and reports what is already there:

```sh
make setup-check        # report only: installs nothing, pulls nothing
make setup              # install packages, set up Ollama, pull weights
```

It uses `apt`, `dnf` or `pacman` (with `sudo`, visibly) for the build packages,
and skips anything already present, so it is safe to re-run.

**The weights are optional**, and `WEIGHTS` picks them:

```sh
make setup                     # auto: exactly what JUDGE_MODE needs
make setup WEIGHTS=none        # nothing -- e.g. you judge through JUDGE_API
make setup WEIGHTS=small       # the 4B only, for a smaller card
make setup WEIGHTS=all         # default and small (+ screen model if hybrid)
```

`auto` reads `JUDGE_MODE` and the model names out of `src/config.h` through the
preprocessor, so it always pulls what the binary will actually ask for:
`JUDGE_API` needs no weights at all, `JUDGE_LOCAL` needs `OLLAMA_MODEL`,
`JUDGE_HYBRID` adds `OLLAMA_SCREEN_MODEL`. After `WEIGHTS=small`, run the daemon
with `--model qwen3:4b` — the binary still defaults to the 8B.

Ollama itself is not installed for you: its official installer runs as root, so
that step stays yours. A **rootless** install (binary in `~/.local/bin`) has no
service, and `make setup` gives it one — `systemd/ollama.service` as a user
unit. Without it the server is whatever you last typed into a terminal, and a
reboot silently ends it: the next cycle fails every judge batch with `Couldn't
connect to server` and publishes nothing new.

### VRAM is shared with your desktop

The judge does not demand the whole model on the GPU: `OLLAMA_NUM_GPU -1` lets
Ollama put what fits on the card and the rest on the CPU, a little slower
rather than failing outright. It matters on a laptop: an X11 session on a
hybrid-GPU machine can render the whole desktop on the discrete card —
measured at 2.5 GB of an 8 GB RTX 5060 for Xorg, gnome-shell and a browser.
If your laptop also has an integrated GPU, logging in with the **Wayland**
session (gear icon on the GDM login screen) moves the desktop onto it and
hands that memory back to the judge.

## Build

```sh
make                    # -O2 -march=native
make debug              # -O0 -g3 -Werror, ASan + UBSan
make test               # build and run every tests/test_*.c
```

## Configure

Two layers, split by whether the setting has a right answer.

**Your part is one text file.** Copy the template, edit it, done — no rebuild:

```sh
install -Dm600 src/config.example ~/.config/issuewatch/config
$EDITOR ~/.config/issuewatch/config
```

It holds the five things that differ per person:

| Setting | What it is |
| --- | --- |
| `repo owner/name` | the repos you actually care about, one per line |
| `profile <text>` | what you work on. Goes into the LLM system prompt and is the single biggest lever on result quality |
| `keyword <weight> <term...>` | the prefilter table. `label-keyword` for label-only terms; the term is the rest of the line, so phrases need no quoting, and a negative weight pushes noise *below* the gate |
| `ntfy-topic` | **generate your own** with `openssl rand -hex 16`. Not a label — the whole of the authentication; see below |
| `gist-id` | where the board is published |

`# ` comments, blank lines ignored. An unknown setting is an **error**, not a
silent no-op — a typo that did nothing would leave you watching someone else's
repositories while the log said everything was fine. Any section you leave out
keeps its built-in default; any section you use replaces that default entirely,
so to drop one repo, list the ones you want.

`--config PATH` overrides the location. `chmod 600` matters: this file holds
your ntfy topic.

Keywords and profile have to move together. The prefilter runs *before* the
LLM, so an issue under `KW_SCORE_MIN` is dropped without ever being judged — set
a profile about Rust and WASM while the keyword table still scores `cuda` and
`matmul`, and you get an empty board with nothing in the log to explain it.

**Everything else stays in [src/config.h](src/config.h)** as compile-time
macros: batch size, context window, score thresholds, timeouts, retention,
resource caps. These were tuned against a measured corpus on specific hardware
and have a right answer; a knob nobody re-measures is worse than no knob. Edit
the header and rebuild. The only argv tuning flag is `--model NAME`, because
which model fits is bounded by the GPU in the machine.

### Creating the gist

Once, then paste the hex id from the URL (not the whole URL) into `gist-id`:

```sh
printf '# issuewatch\n' > /tmp/issuewatch-board.md
gh gist create -d issuewatch /tmp/issuewatch-board.md
```

Secret is `gh gist create`'s default; `--public` is the opt-out. Seed it under
the name `GIST_FILENAME` already uses — the publish PATCHes that one file, so a
differently named seed is not replaced, it is joined.

Your `GH_TOKEN` needs `gist` scope for this, which a fine-grained PAT cannot
grant — use a classic token with `gist` plus public-repo read. Leave `gist-id`
unset and the board simply does not publish; the daemon still runs, and
`--dry-run` still prints the board to stdout.

### Why the topic matters

A public ntfy.sh topic is readable *and writable* by anyone who knows its name.
The read side leaks only public GitHub data, but the write side means anyone who
guesses your topic can push arbitrary notifications to your phone — any title,
any body, `Priority: 5` so it breaks through Focus. Use a long random topic, or
self-host ntfy with access control and set `NTFY_TOKEN`. `notify_init()` refuses
to run on the placeholder.

*Knows* includes reading it in a repo: a real topic committed anywhere public is
a published credential. That is why it lives in your config file, outside the
checkout, and why `src/config.example` ships only placeholders.

## Secrets

Tokens are never in a file at all. Read from the environment at startup:

| Variable | Required |
| --- | --- |
| `GH_TOKEN` | always — needs `gist` scope to publish the board, so a classic token; a fine-grained PAT with public-repo read is enough only with `gist-id` left unset |
| `ANTHROPIC_API_KEY` | only for `JUDGE_API` / `JUDGE_HYBRID` |
| `NTFY_TOKEN` | only for a self-hosted ntfy with auth |

### Getting `GH_TOKEN`

If the `gh` CLI is already logged in, borrow its token rather than minting a
second one:

```sh

export GH_TOKEN="$(gh auth token)"
```

Check the scopes first — `gh auth status` prints them, and publishing the board
needs `gist`:

```sh
gh auth status            # look for 'gist' in "Token scopes"
gh auth refresh -s gist   # add it if missing
```

Put that same `export` line in `~/.bashrc` to get it in every new shell. It
stores the *instruction to ask `gh`*, not the token itself, so the token stays
in the system keyring and `gh auth refresh` is picked up with no edit.

Without `gh`, create a **classic** token at
<https://github.com/settings/tokens> with `public_repo` + `gist` and export it
directly. A fine-grained PAT cannot write gists, so it works only with
`GIST_ID` left unset.

`export` matters: a bare `GH_TOKEN=...` is a shell variable and child processes
never see it, so `./issuewatch` fails with "GH_TOKEN is not set" while `echo
$GH_TOKEN` looks fine.

systemd reads none of this — see the timer section for `~/.config/issuewatch/env`.

## Run

```sh
./issuewatch --dry-run      # print to stdout, send nothing. Tune with this.
./issuewatch --oneshot      # one cycle, exit. What the timer runs.
./issuewatch --runs 45      # 45 cycles 10s apart. Covers the whole backfill.
./issuewatch --daemon       # sleep-loop. Supported, but see below.
```

### The judge model

`qwen3:8b` by default, which needs about 7.2 GB of VRAM with `OLLAMA_NUM_CTX`
— it fits an 8 GB card with roughly 0.5 GB to spare and nothing else on the
GPU. On a smaller card:

```sh
./issuewatch --model qwen3:4b
```

That fits in ~4 GB and is the only supported alternative, but understand what
you are trading. Benchmarked against tinygrad's three open, unassigned
bounties:

| | pairing errors | real bounties kept | VRAM |
| --- | --- | --- | --- |
| `qwen3:8b` | 0 of 8 | **3 of 3** (10, 9, 8) | ~7.2 GB |
| `qwen3:4b` | 1 of 2 | **0 of 3** | ~4 GB |

4b scored every real bounty 0, reasoning "bounty is claimed and unassigned" —
false and self-contradictory at once. It will run, and it will miss the thing
you built this for.

Neither model survives a batch of 8: at `LLM_BATCH_SIZE 8` even 8b returned 6
of 8 verdicts describing a *different* issue in the batch, so each issue was
published beside another one's reasoning. 4 is the tested value; raising it
needs that cross-contamination check re-run, not a glance at the scores.

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
install puts the binary at `~/.local/bin/ollama` with no service — `make setup`
installs `systemd/ollama.service` as a user unit for exactly this case. It sets
the idle unload window **there**, on the server, which is the only place it
works; in `systemd/issuewatch.service` it does nothing:

```sh
systemctl --user status ollama     # after make setup
```

`--oneshot` will simply fail its judge batches if the server is not up, publish
the board it already had, and leave the watermark alone for a retry.
