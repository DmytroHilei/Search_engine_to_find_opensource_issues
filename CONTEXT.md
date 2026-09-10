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

Two things persist, both under `$XDG_STATE_HOME/issuewatch/`:

- `etags` — small text file, `owner/repo\tETag\twatermark_rfc3339` per line.
  Rewritten atomically (write temp, `rename(2)`).
- `seen.bin` — fixed-size mmap'd open-addressing hash set of
  `u64 = hash(issue_id, updated_at)`. `SEEN_CAPACITY` slots (65536 → 512 KB),
  overwriting oldest on collision. Never grows, no compaction, no sqlite.

Keying on `(id, updated_at)` means a genuinely updated issue can re-notify;
keying on `id` alone means it never does. `NOTIFY_ON_UPDATE` picks which.

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

```c
#ifndef CONFIG_H
#define CONFIG_H

/* ---- scheduling ---- */
#define POLL_INTERVAL_SEC      (3 * 3600)
#define POLL_JITTER_SEC        600

/* ---- repos to watch ---- */
#define WATCHED_REPOS \
    X("ggml-org/llama.cpp")     \
    X("NVIDIA/cutlass")         \
    X("triton-lang/triton")     \
    X("pytorch/pytorch")        \
    X("openai/tiktoken")
/* used as:  #define X(r) r,   static const char *REPOS[] = { WATCHED_REPOS }; */

/* ---- what you care about (goes into the LLM system prompt) ---- */
#define USER_PROFILE \
    "C/C++ and CUDA developer, HPC and GPU kernels. Interested in: "        \
    "performance regressions, kernel optimization, numerical correctness, " \
    "memory/allocator bugs, build system issues on Linux. "                 \
    "Not interested in: docs, typos, Windows/macOS-only, JS/Python "        \
    "packaging, dependency bumps, feature requests without a design."

/* ---- keyword prefilter ---- */
typedef struct { const char *term; int weight; int label_only; } kw_t;
static const kw_t KEYWORDS[] = {
    /* high signal labels */
    { "good first issue",   6, 1 },
    { "help wanted",        5, 1 },
    { "performance",        4, 0 },
    /* topical */
    { "cuda",               5, 0 },
    { "kernel",             3, 0 },
    { "race condition",     4, 0 },
    { "memory leak",        4, 0 },
    { "segfault",           4, 0 },
    { "regression",         3, 0 },
    { "undefined behavior", 4, 0 },
    /* negative */
    { "dependabot",       -10, 0 },
    { "[bot]",            -10, 0 },
    { "bump version",      -8, 0 },
    { "typo",              -6, 0 },
    { "translation",       -6, 0 },
};
#define KW_SCORE_MIN           6
#define KW_COUNT_CAP           3   /* stop rewarding the same word repeating */
#define KW_LABEL_MULTIPLIER    2

/* ---- judging ---- */
#define JUDGE_LOCAL  0
#define JUDGE_API    1
#define JUDGE_HYBRID 2
#define JUDGE_MODE             JUDGE_LOCAL

#define OLLAMA_URL   "http://127.0.0.1:11434/api/chat"
#define OLLAMA_MODEL "qwen3:4b"
#define OLLAMA_NUM_GPU         999   /* 0 = pure CPU */
#define ANTHROPIC_MODEL        "claude-haiku-4-5-20251001"

#define LLM_BATCH_SIZE         8
#define LLM_BODY_TRUNC         1200
#define LLM_SCORE_MIN          6     /* 0..10 from the model */
#define LLM_TIMEOUT_SEC        120

/* ---- notifications ---- */
#define NTFY_SERVER            "https://ntfy.sh"
#define NTFY_TOPIC             "REPLACE_ME_WITH_RANDOM_HEX"
#define NOTIFY_MAX_PER_CYCLE   10
#define NOTIFY_ON_UPDATE       0

/* ---- resources ---- */
#define SEEN_CAPACITY          65536
#define ARENA_SIZE             (4u << 20)
#define HTTP_MAX_CONCURRENT    8
#define RL_RESERVE             100

#endif /* CONFIG_H */
```

## 12. Secrets

Never in `config.h`. Read from the environment at startup:

- `GH_TOKEN` — required.
- `ANTHROPIC_API_KEY` — required only for `JUDGE_API` / `JUDGE_HYBRID`.
- `NTFY_TOKEN` — optional, only for a self-hosted ntfy with auth.

Fail fast with a clear message if a required one is missing. In the systemd
unit use `EnvironmentFile=%h/.config/issuewatch/env` with mode `0600`.

## 13. Open decisions left to you

- Whether comments on an issue should re-trigger (`NOTIFY_ON_UPDATE`).
- Whether to add a `--dry-run` that prints to stdout instead of pushing —
  strongly recommended for tuning keyword weights, which will take a few
  iterations before the signal-to-noise is right.
- Whether discovery mode is worth building at all. Probably not until watch
  mode has been running for a month.
