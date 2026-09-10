# CONTEXT.md — `issuewatch`

Design context for a small C daemon that polls GitHub issues across a fixed repo
list, prefilters by weighted keywords, judges survivors with an LLM, and pushes
notifications to ntfy.

This file is background/rationale. Build rules and conventions live in `CLAUDE.md`.

---

## 1. What it does

```
                 ┌───────────── every POLL_INTERVAL_SEC ─────────────┐
                 │                                                   │
  config.h ──▶ [poller] ──▶ [json] ──▶ [prefilter] ──▶ [judge] ──▶ [notify]
   (repos,      libcurl     yyjson     Aho–Corasick    Ollama /     ntfy.sh
    keywords,   multi       insitu     weighted        Anthropic    HTTP POST
    weights)    HTTP/2      parse      score           batched
                 │                          │               │
                 └── etag cache ────────────┴── seen-set ────┘
                     (state file)               (mmap ring)
```

One process. Single-threaded. No thread pool — concurrency comes from
`curl_multi`, which is enough for tens of repos.

## 2. Realistic scope note

You cannot "search all issues on GitHub". There is no cheap firehose. Two
practical modes, and the design supports both:

- **Watch mode (primary).** A fixed list of repos in `config.h`, polled with
  `since=`. Cost is O(repos), fully under your control. This is what you
  actually want.
- **Discovery mode (optional, off by default).** `GET /search/issues?q=...` to
  find repos you don't know about yet. Separate and much stricter rate limit
  (30 req/min authenticated), results capped at 1000, and ranking is
  GitHub's, not yours. Treat as a weekly job, not a 2-hour one.

## 3. GitHub API specifics that matter

Endpoint per repo:

```
GET /repos/{owner}/{repo}/issues
    ?state=open
    &since={RFC3339 watermark}
    &sort=updated
    &direction=desc
    &per_page=100
```

Non-obvious things that will bite an implementer:

| Issue | Handling |
| --- | --- |
| **PRs are returned as issues.** Every PR appears in this endpoint. | Skip any object with a `pull_request` key present. |
| **`since` filters on `updated_at`, not `created_at`.** | So you re-see old issues that got a new comment. That's usually desirable; dedup handles the noise. |
| **Unauthenticated = 60 req/hr.** | Always send `Authorization: Bearer $GH_TOKEN`. Authenticated = 5000 req/hr. A fine-grained PAT with public-repo read is enough; no scopes needed for public repos. |
| **`304 Not Modified` does not count against the rate limit.** | This is the single biggest optimization. Store the `ETag` per repo, send `If-None-Match`. Steady state is nearly free. |
| **Secondary rate limits** return 403 with `Retry-After`. | Honour it. Don't retry blind. |
| **Pagination via `Link` header.** | With `sort=updated&direction=desc` you can stop paginating as soon as an item's `updated_at` <= watermark. Usually page 1 only. |
| Rate budget headers | `X-RateLimit-Remaining`, `X-RateLimit-Reset`. Back off when remaining < `RL_RESERVE`. |

## 4. Transport: libcurl, not `curl(1)`

You said "via curl". Use **libcurl's multi interface**, not `fork`+`exec` of the
binary. Shelling out costs a process spawn and a fresh TLS handshake per repo,
and makes ETag handling awkward. With `curl_multi`:

- `CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` → all repos share **one** HTTP/2
  connection to `api.github.com`. One TLS handshake per cycle, total.
- `CURLMOPT_MAX_HOST_CONNECTIONS = 1` to force that.
- `CURLOPT_ACCEPT_ENCODING = "gzip"` → GitHub gzips JSON, roughly 5x smaller.
- `curl_multi_poll()` blocks in `epoll` — zero CPU while waiting.

## 5. Prefilter: Aho–Corasick, not a plain hash map

A token hash map works but can't match phrases. Half the useful terms are
phrases: `good first issue`, `memory leak`, `race condition`, `help wanted`.
An n-gram hash map handles that only by exploding the key space.

Aho–Corasick scans `title + body + labels` in one pass, O(n + matches),
matching all dictionary entries including multi-word ones. Automaton is built
once at startup from the static `KEYWORDS[]` table. For a few hundred terms it
is a few tens of KB — irrelevant to your RAM budget.

Scoring:

```
score = Σ (weight_i × min(count_i, KW_COUNT_CAP))
```

Negative weights are the important half: `dependabot`, `[bot]`, `typo`,
`translation`, `bump version`. They cheaply kill the noise floor.

Match on **label names too**, weighted higher than body text — GitHub labels are
curated and are the highest-signal field in the whole payload.

Gate: `score >= KW_SCORE_MIN` to reach the LLM. Expect this to drop 85–95% of
traffic. This is the main GPU saver, more than any inference-level tuning.

## 6. Judging: three modes

Selected at compile time by `JUDGE_MODE`.

### `JUDGE_LOCAL`
HTTP to a local **Ollama** server (`http://127.0.0.1:11434/api/chat`).

Ollama over raw `llama-server` specifically because of your GPU concern:
Ollama unloads the model from VRAM after `OLLAMA_KEEP_ALIVE` (default 5 min) of
idle. Polls are 2–3 hours apart, so VRAM sits at **0** for ~99% of wall time and
the model reloads on demand. `llama-server` pins VRAM forever unless you manage
its lifecycle yourself. Set `OLLAMA_KEEP_ALIVE=2m` in the unit file.

Model sizing, Q4_K_M:

| Model | VRAM | Notes |
| --- | --- | --- |
| Qwen3-1.7B | ~1.5 GB | Enough for a binary relevant/not screen |
| Qwen3-4B-Instruct | ~3 GB | Good default; handles the reason string |
| Gemma3-4B | ~3.5 GB | Alternative |
| Qwen3-8B | ~5.5 GB | Only if you're unhappy with 4B precision |

If there is no usable GPU, `num_gpu: 0` and a 1.7B model on CPU is still fine —
you are judging maybe 20–40 issues every 3 hours, not serving traffic.

### `JUDGE_API`
Anthropic Messages API, `claude-haiku-*`. Key from `ANTHROPIC_API_KEY`. Use when
you don't want the model resident at all, or when local judgement is too coarse.

### `JUDGE_HYBRID`
Local 1.7B does a binary keep/drop screen; only survivors go to the API for
scoring + reason. Cheapest in API tokens, costs a local model load.

### Prompt shape (all modes)

**Batch.** One request per `LLM_BATCH_SIZE` (8 is a good default) issues, not one
per issue. Numbered list in, JSON array out. Cuts request overhead ~8x and lets
the model compare candidates against each other.

**Constrain the output.** Ollama takes `"format": <json-schema>`; the Anthropic
API takes a tool definition. Either way the model cannot emit prose, so parsing
never fails and output tokens stay tiny. Do not parse free-form text.

Response schema:

```json
[{"i": 0, "keep": true, "score": 7, "why": "CUDA kernel perf regression, unassigned"}]
```

`why` capped at ~12 words — it becomes the ntfy notification body.

**Truncate the body** to `LLM_BODY_TRUNC` (1200 chars is plenty). Issue bodies
routinely contain 40 KB of stack traces or `nvidia-smi` dumps. Keep the first
~900 and last ~300 chars — the tail often holds the actual question.

The interest profile (`USER_PROFILE` string in `config.h`) goes in the system
prompt: what you work on, what kind of task you want, what to ignore.

## 7. Dedup state

Three things persist, all under `$XDG_STATE_HOME/issuewatch/`:

- `etags` — small text file, `owner/repo\tETag\twatermark_rfc3339` per line.
- `seen.bin` — fixed-size mmap'd open-addressing hash set of
  `u64 = hash(issue_id, updated_at)`. `SEEN_CAPACITY` slots (65536 → 512 KB),
  overwriting oldest on collision. Never grows, no compaction, no sqlite.
- `board.tsv` — the ranked board, one entry per line. See section 14.

Keying on `(id, updated_at)` means a genuinely updated issue can re-notify;
keying on `id` alone means it never does. `NOTIFY_ON_UPDATE` picks which.

Both text files are rewritten by the same atomic dance — temp file in the same
directory, `fflush`, `fsync`, `rename(2)`, then `fsync` the directory so the
rename itself is durable. It lives in `core/fileio.c` behind an emit callback
rather than being written twice: it is the only durability guarantee the program
makes, and two copies of it would eventually disagree.

The seen-set gained a second job when the board arrived. It still suppresses
repeat notifications, but it is now also where `gh_recheck()` records an entry it
dropped as closed or assigned — otherwise the next `since=` delta would re-add
that issue, the next re-check would drop it again, and it would flap on and off
the board forever.

## 8. Notifications: ntfy

```
POST https://ntfy.sh/{NTFY_TOPIC}
Title: [owner/repo] Issue title
Priority: 4          # derived from LLM score
Tags: bug,rocket
Click: https://github.com/owner/repo/issues/123
Markdown: yes

<why string>
```

Body is plain text; headers must be ASCII (RFC 2047-encode or strip non-ASCII in
titles, GitHub issue titles contain emoji constantly).

**Security caveat, worth taking seriously:** public ntfy.sh topics are readable
and writable by anyone who knows the topic name. There is no auth on the free
tier by default. Use a long random topic (`openssl rand -hex 16`), or
self-host `ntfy` with access control. Since the payload is public GitHub issue
data the leak is mild, but the *write* side means anyone can spam your phone.

iPhone: the official ntfy iOS app subscribes to a topic and gets APNs pushes.
No bot, no Telegram, no Apple developer account. `Click:` opens the issue
directly. This is the correct answer to your requirement.

Cap at `NOTIFY_MAX_PER_CYCLE` (e.g. 10), sorted by score descending, so a repo
that had a bad night can't dump 200 pushes on you.

That cap turned out to be the wrong shape of answer — ten pushes a cycle is
still eighty a day, and a push is an event that cannot be re-read. With
`NOTIFY_SUMMARY_ONLY` the per-issue pushes collapse into one summary per cycle
whose `Click:` opens the ranked board. See section 14.

## 9. Scheduling — and a recommendation you should take

You asked for a daemon. Support it, but the better default is a **systemd user
timer** running the binary with `--oneshot`:

```ini
# ~/.config/systemd/user/issuewatch.timer
[Timer]
OnBootSec=2min
OnUnitActiveSec=3h
RandomizedDelaySec=10min
Persistent=true
```

Between runs the process does not exist. RSS is literally 0, not "small". It
survives reboots, gets restart/logging/backoff for free, and `Persistent=true`
catches up a missed run if the laptop was asleep. A `sleep()` loop cannot do any
of that.

Keep `--daemon` as an option anyway. If used: `clock_nanosleep(CLOCK_MONOTONIC,
TIMER_ABSTIME)` so it doesn't drift, and call `malloc_trim(0)` at the end of
each cycle so glibc returns freed arenas to the kernel — idle RSS drops to
~1–2 MB instead of holding the cycle peak.

## 10. Optimization checklist

Ranked by actual impact, not by how clever they sound.

1. **ETag / `If-None-Match`** — 304s cost no body and no rate limit. Biggest win.
2. **Keyword prefilter before the LLM** — removes ~90% of inference work.
3. **`since=` watermark** — bounds the payload to what changed.
4. **Batch the LLM** (8 issues/request) — 8x fewer round trips and prefills.
5. **Ollama keep-alive unload** — 0 VRAM between cycles.
6. **HTTP/2 multiplexing** — one TLS handshake per cycle for all repos.
7. **gzip** — ~5x less bytes on the wire.
8. **Body truncation** — bounds prompt length; stops a 40 KB log blowing the batch.
9. **Constrained JSON decode** — fewer output tokens, zero parse retries.
10. **Arena allocator**, reset (not freed) per cycle — no malloc churn in the
    hot path. One `arena_reset()` call, everything is bump-pointer.
11. **Early pagination stop** on `updated_at <= watermark`.
12. **`curl_multi_poll()`** — blocks in epoll, zero CPU while idle.
13. **`malloc_trim(0)`** after each cycle, daemon mode only.
14. Build with `-O2 -march=native`. `-O3` buys nothing here; the workload is
    I/O and inference bound.

Things deliberately **not** done, because they'd be cargo cult at this scale:
thread pools, io_uring, a custom allocator beyond the arena, SIMD keyword
matching, an embedded database.

## 11. `config.h` — the whole configuration surface

Read [src/config.h](src/config.h). It is the only file you edit, every macro in
it carries the comment explaining what it is for, and several of the numbers
there are tuned against measured payloads rather than guessed.

This section used to embed a copy of that file. The copy went stale within two
commits — it still showed `KW_SCORE_MIN 6` after the gate was retuned to 24
against a real 179-issue sample, the pre-bounty repo list, and a 4 MB
`ARENA_SIZE` that a real first run exhausted. A second copy of a file that
changes is a liability, not documentation, so there is now exactly one.


## 12. Secrets

Never in `config.h`. Read from the environment at startup:

- `GH_TOKEN` — required.
- `ANTHROPIC_API_KEY` — required only for `JUDGE_API` / `JUDGE_HYBRID`.
- `NTFY_TOKEN` — optional, only for a self-hosted ntfy with auth.

Fail fast with a clear message if a required one is missing. In the systemd
unit use `EnvironmentFile=%h/.config/issuewatch/env` with mode `0600`.

## 13. Open decisions left to you

Settled since this was written:

- **`--dry-run` exists** and is the tuning path. It prints and writes nothing:
  no ntfy POST, no gist PATCH, no `board.tsv`, no watermark. Each module proves
  that with a call counter its tests assert on, rather than by inspection.
- **Delivery is a secret gist**, not a hosted page (section 14).
- **Retention is closed-or-assigned**, not "seen".

Still open:

- Whether comments on an issue should re-trigger (`NOTIFY_ON_UPDATE`). It
  matters less than it did: under `NOTIFY_SUMMARY_ONLY` the board carries an
  issue whether or not a push repeats.
- Whether discovery mode is worth building at all. Probably not until watch
  mode has been running for a month.
- Whether `BOARD_MAX` of 200 is the right size. Nothing has come close to it.

## 14. The ranked board

Sections 7 and 8 describe a program that sends *events*. Ten pushes a cycle,
eight cycles a day, ordered by whenever they happened to arrive, and a seen-set
that is permanent — so an issue glanced at over breakfast is gone forever even
though the bounty on it is still unclaimed at 15:00. For the Tenstorrent repos
that is the entire game: every open bounty is assigned within 2-4 days, and a
pull request only counts if you held the assignment first.

What the user actually wants is *state*: one board, re-ranked every cycle,
showing what is still open and still worth doing. The pushes collapse into a
single summary whose `Click:` opens it.

### Why a gist

A dashboard has to be reachable from a phone over the internet, with no cable,
no Bluetooth pairing and nothing to operate. The options were a static site
(needs hosting), a self-hosted page (needs a box and a domain), an ntfy message
with the whole board in it (unreadable, and re-sent whole every cycle), or a
secret GitHub Gist PATCHed over the API.

The gist wins on the dependency ledger, which is the ledger this project cares
about: it reuses `GH_TOKEN`, reuses `net/http.c`, adds no dependency, no
hosting, and no new secret. It renders Markdown on the phone for free. The cost
is two things, both accepted: the token needs `gist` scope, and the gist id
lives in `config.h`.

### Retention: closed or assigned, not "seen"

An entry leaves the board when GitHub says it is closed or somebody is assigned
to it. Not when the user has looked at it — looking at something does not make
it done.

That rule is what forces the re-check pass, and the re-check pass has no
precedent elsewhere in the program. The main fetch is
`state=open&since=<watermark>`: deltas only, so **nothing in it would ever tell
us that an issue closed or got assigned**. Without a second pass the board
quietly rots into a list of bounties somebody else is already being paid for.

Flipping the main fetch to `state=all` was rejected. pytorch alone closes issues
constantly; that traffic would blow past both the 900-issue cap and the cycle
arena, to learn one bit about at most 200 issues.

So: one conditional `GET /repos/{o}/{r}/issues/{n}` per board entry that this
cycle's fetch did not already refresh, batched through the existing multiplexed
path, each carrying the per-issue ETag stored on the entry. A `304` is the
common case and costs no rate limit. The budget is <=200 conditional GETs eight
times a day against 5000/hr authenticated — noise.

The drop rules matter more than they look:

| Result | Action | Why |
| --- | --- | --- |
| `304` | keep | nothing changed; free |
| `200`, still open and unassigned | refresh | score, title, etag, updated_at |
| `200`, closed or assigned | drop, mark seen | somebody else has it |
| `404` | drop | deleted or transferred |
| transport failure | **keep unchanged** | a network blip must not empty the board |

Dropped ids go into the existing seen-set, which is what stops an assigned issue
from being re-added by the next fetch and dropped again on the next re-check,
flapping on and off the board forever.

### Failure semantics

Publishing is now the cycle's primary output, so rule 4 covers it the way it
covered notification: a failed gist PATCH must not advance the watermark. The
merge is keyed by issue id and is therefore idempotent, so re-running a failed
cycle is safe rather than duplicative.

A judge outage is the one failure that does *not* stop the publish. If every LLM
batch fails, no new entries are merged and the watermark stays put so those
issues are re-judged — but the re-check, the ranking and the publish still run.
A model that is down for a day would otherwise leave the board advertising
bounties that were claimed hours ago, which is the exact failure the board
exists to prevent.

`--dry-run` prints the rendered board and writes nothing at all: no gist PATCH,
no ntfy POST, no `board.tsv`, no watermark. That is asserted structurally by a
call counter in each module, not by reading the code.

### The file format trap

`board.tsv` sits next to the etag cache and is rewritten by the same atomic
dance (`core/fileio.c`: temp file, fsync, rename). It is TSV, and GitHub issue
titles contain tabs, newlines, backslashes, emoji and CJK — so title, why and
url are escaped on write and unescaped on read, and the round trip over
adversarial titles is a required test, not a nice-to-have. A line that fails to
parse is skipped, never fatal: losing one row costs one row, aborting the load
costs the whole board.

As built, a row is 12 tab-separated fields — `id`, `repo`, `number`,
`llm_score`, `kw_score`, `first_seen`, `updated_at`, `etag`, `title`, `why`,
`html_url`, `assigned` — and a line with any other field count is rejected, so
an unescaped tab that reached the file is caught rather than silently shifting
every column. `\r` is escaped alongside `\ \t \n`: not needed for
parseability, but a bare CR in a text file is a trap for whatever reads it next.
Oversized text truncates (a clipped title loses a tail); a structural problem —
a bad id, an unparseable `first_seen`, an unknown escape — skips the row.

### The seam that actually broke

Every module above was tested on its own and all of them passed. The bug was in
none of them.

`board_entry_t::title` is 256 bytes. A byte-wise truncation of a title made of
4-byte emoji lands mid-character three times out of four — 255 is 63×4+3 — and
leaves half a code point in the field. It renders fine. It persists fine. Then
yyjson, which refuses to encode invalid UTF-8, returns NULL for the gist body,
`gist_publish()` reports failure, and the cycle publishes **nothing at all**. A
CJK title divides evenly into 255 and would never have shown it.

Two fixes, because one of them is a backstop and neither is sufficient alone:
whoever fills a board entry truncates on a UTF-8 boundary (`utf8_trunc_len()`
already existed in `github.c` for exactly this), and `render.c` validates each
sequence and drops what is not one — rejecting overlongs, surrogates and
anything past U+10FFFF on the way. The cost of a clipped title is now one
missing character in one row instead of an empty board.

The general lesson is worth keeping: the per-module tests could not see this,
because render's output looked correct and gist's input was always well-formed.
`tests/test_pipeline.c` exists to test the seam in the direction data actually
travels — truncate the way a caller does, render, then encode.

### What the phone gets

```
Title:    3 new, 9 open: [tenstorrent/tt-metal] bf16 matmul NaN on RDNA4
Priority: 5
Click:    https://gist.github.com/<GIST_ID>
```

Counts first, because that is what is legible on a lock screen. The title goes
through the same ASCII sanitiser as every other header — emoji folded to a
space — while the Markdown body keeps them. That is the rule 5 split: headers
are ASCII, bodies are not.

A cycle that found nothing new sends no push at all. The board is still
republished, so ages and claimed-bounty drops stay current without a buzz.

### The 404 you will hit first

Gists need a classic token with `gist` scope; a fine-grained PAT cannot grant
it. Without the scope the PATCH returns a bare `404` that reads exactly like a
wrong gist id. `http.c` therefore captures `X-OAuth-Scopes` and the failure path
prints the scopes the token actually presents, so the message names the real
problem instead of sending you to check the id.
