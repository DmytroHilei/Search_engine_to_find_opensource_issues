#ifndef CONFIG_H
#define CONFIG_H

/*
 * Tuning, and the DEFAULTS for everything personal. Compile-time constants, all
 * of them: these are tuned against a measured corpus and a specific GPU, and a
 * number nobody re-measures is worse than no knob at all.
 *
 * What a user actually changes does not live here any more. The repo list, the
 * developer profile, the keyword table and the two credentials are read at
 * startup from a text file -- see src/config.example and core/userconf.h. The
 * values below are what that file falls back to, key by key, so an absent or
 * partial config still runs.
 *
 * Secrets never live here. GH_TOKEN / ANTHROPIC_API_KEY / NTFY_TOKEN come from
 * the environment; NTFY_TOPIC and GIST_ID come from the config file and keep
 * only placeholders below. An ntfy.sh topic name IS the authentication --
 * anyone who reads it can push to the phone -- and this repo is public, so
 * notify_init() and gist_init() both refuse to run on a placeholder rather than
 * publishing to an address someone else can read.
 */

/* ---- scheduling ---- */
#define POLL_INTERVAL_SEC      (3 * 3600)
#define POLL_JITTER_SEC        600
/*
 * Gap between cycles under `--runs N`, which runs N cycles back to back and
 * exits. That mode exists for the rolling backfill: one repo is swept per
 * cycle, so covering all of WATCHED_REPOS takes dozens of cycles and waiting
 * POLL_INTERVAL_SEC between them means days. `--runs 40` does it over lunch.
 *
 * Short, but deliberately not zero: each cycle re-reads ETags and re-checks the
 * board, and hammering the API with no pause is what secondary rate limits are
 * for. Ten seconds is far below POLL_INTERVAL_SEC and far above nothing.
 */
#define RUNS_DELAY_SEC         10
/* Upper bound on --runs, so a typo'd `--runs 100000` is rejected at argv rather
 * than spending the rate-limit budget discovering it. */
#define RUNS_MAX               500

/*
 * ---- repos to watch ----
 *
 * Ordered by why they are here, not alphabetically. Cost is O(repos) and ETags
 * make a quiet repo nearly free, so breadth is cheap; relevance is not.
 *
 * Tier 1, pays cash. Tenstorrent bounties run $1k-35k and are overwhelmingly
 * numerical-correctness and kernel-perf work. Every open one is assigned within
 * roughly 2-4 days of creation, and a PR only counts if you were assigned
 * first -- so the entire value of watching these is catching them early.
 * tinygrad pays $200-2k and usually has several unclaimed.
 *
 * Tier 2, pays reputation. High-visibility C/C++/GPU projects where landing
 * work compounds into invited paid work later. OpenSSL is here because it
 * carries ~200 help-wanted issues and very little competition.
 */
#define WATCHED_REPOS \
    X("tenstorrent/tt-metal")       \
    X("tenstorrent/tt-blacksmith")  \
    X("tinygrad/tinygrad")          \
    X("exo-explore/exo")            \
    X("ggml-org/llama.cpp")         \
    X("ggml-org/whisper.cpp")       \
    X("openssl/openssl")            \
    X("pytorch/pytorch")            \
    X("triton-lang/triton")         \
    X("NVIDIA/cutlass")             \
    X("NVIDIA/cccl")                \
    X("Dao-AILab/flash-attention")  \
    X("vllm-project/vllm")          \
    X("ROCm/composable_kernel")     \
    X("apache/tvm")                 \
    X("opencv/opencv")

/*
 * The X-list expands into a real array only where CONFIG_WANT_REPOS is defined
 * before the include. Unconditional definitions here would land a private copy
 * in every translation unit and trip -Wunused-const-variable under -Werror.
 */
#ifdef CONFIG_WANT_REPOS
#define X(r) r,
static const char *const REPOS[] = { WATCHED_REPOS };
#undef X
#define N_REPOS (sizeof REPOS / sizeof REPOS[0])
#endif

/*
 * ---- what you care about (goes into the LLM system prompt) ----
 *
 * Three things are being valued here, not one. A paid bounty scores highest,
 * but unpaid work that builds reputation or is simply interesting must NOT be
 * discarded -- an earlier profile that optimised for money alone would have
 * thrown away exactly the work that compounds into paid work later. The
 * explicit ranking is what stops the model collapsing all three into "is
 * there money attached".
 */
#define USER_PROFILE \
    "C/C++ and CUDA developer: HPC, GPU kernels, compilers, systems. "        \
    "Rank candidates by, in order of weight:\n"                              \
    "(1) PAID. An explicit cash bounty, prize, grant or stipend, and nobody "  \
    "holding it yet -- a pull request only counts when you hold the "         \
    "assignment, so a claimed bounty is worth little. Outranks everything "   \
    "below it; the `payment:` line of each issue settles whether it applies.\n" \
    "(2) REPUTATION. Unpaid work in a high-visibility project where landing " \
    "a patch is durable credibility: kernel or compiler internals, a "        \
    "correctness fix with a regression test, a measurable performance win. "  \
    "Worth 6-8 with no money attached, but earn it on the three marks -- "    \
    "a high-visibility repository is not by itself a reason to score high.\n" \
    "(3) INTEREST. Genuinely novel or difficult problems -- unusual "         \
    "hardware, emulators, numerical analysis, allocator and memory-model "    \
    "work -- that are worth doing for their own sake. Worth 6-7, same "       \
    "condition.\n"                                                           \
    "Strong topical signals: numerical correctness (fp32/fp16 accuracy, "     \
    "overflow, NaN, rounding, INT_MIN edge cases), kernel and matmul "        \
    "optimisation, tensor cores, memory/allocator bugs, race conditions, "    \
    "undefined behaviour, Linux build and toolchain breakage.\n"              \
    "Ignore: docs and typos, translations, dependency bumps, Windows- or "    \
    "macOS-only problems, JS/Python packaging, support questions, and "       \
    "feature requests with no design attached. An issue that is merely a "    \
    "bug report with no route to a fix is not interesting either."

/* ---- keyword prefilter ---- */
typedef struct {
    const char *term;
    int weight;
    int label_only;
} kw_t;

#ifdef CONFIG_WANT_KEYWORDS
/*
 * Weights encode the priority order in USER_PROFILE: money, then reputation,
 * then interest. "bounty" alone clears KW_SCORE_MIN on its own, deliberately --
 * a funded issue must always reach the judge even when nothing else matches.
 *
 * Terms are matched as literal substrings, case-insensitively, with no word
 * boundaries. That rules out short tokens that hide inside ordinary words:
 * "nan" would fire on "maintenance" and "finance", so numerical signal is
 * carried by "numerical"/"precision"/"rounding" instead.
 */
static const kw_t KEYWORDS[] = {
    /* (1) paid -- must clear KW_SCORE_MIN unaided, so a funded issue always
     * reaches the judge even when nothing else about it matches. */
    { "bounty",            24, 0 },
    { "gsoc",              24, 0 },
    { "stipend",           24, 0 },
    { "prize",             24, 0 },
    /* "reward" is deliberately weak and "grant" is absent: in ML repos they
     * mean reward functions and granting permissions far more often than money. */
    { "reward",             3, 0 },

    /* (2) reputation -- curated labels, the highest-signal field in the payload.
     * Doubled by KW_LABEL_MULTIPLIER, so "good first issue" as a label clears
     * the gate on its own and "help wanted" needs one topical term with it. */
    { "good first issue",  12, 1 },
    { "help wanted",       10, 1 },
    { "mentorship",         8, 0 },
    { "performance",        4, 0 },

    /* (3) interest -- topical signal, matched against the real bounty titles */
    { "cuda",               5, 0 },
    { "kernel",             3, 0 },
    { "numerical",          4, 0 },
    { "precision",          4, 0 },
    { "overflow",           4, 0 },
    { "rounding",           4, 0 },
    { "fp32",               4, 0 },
    { "fp16",               3, 0 },
    { "tensor core",        4, 0 },
    { "matmul",             4, 0 },
    { "allocator",          4, 0 },
    { "emulator",           3, 0 },
    { "throughput",         3, 0 },
    { "optimization",       3, 0 },
    { "optimisation",       3, 0 },
    { "sanitizer",          3, 0 },
    { "use-after-free",     4, 0 },
    { "data race",          4, 0 },
    { "race condition",     4, 0 },
    { "memory leak",        4, 0 },
    { "segfault",           4, 0 },
    { "regression",         3, 0 },
    { "undefined behavior", 4, 0 },

    /* negative -- the cheap way to kill the noise floor */
    { "dependabot",       -10, 0 },
    { "[bot]",            -10, 0 },
    { "bump version",      -8, 0 },
    { "typo",              -6, 0 },
    { "translation",       -6, 0 },
    { "changelog",         -5, 0 },
    { "readme",            -4, 0 },
};
#define N_KEYWORDS (sizeof KEYWORDS / sizeof KEYWORDS[0])
#endif

/*
 * Tuned against real payloads from the repos above, not guessed. Every repo
 * here is a GPU/systems project, so "cuda", "kernel" and "performance" appear
 * almost everywhere and a low gate passes half the traffic: at 6 the prefilter
 * kept 58% of a real 179-issue sample, against the 5-15% CONTEXT.md targets.
 * Re-tune with the same method after any KEYWORDS[] edit -- these weights are
 * only meaningful relative to this number.
 */
#define KW_SCORE_MIN           24
#define KW_COUNT_CAP           3   /* stop rewarding the same word repeating */
#define KW_LABEL_MULTIPLIER    2

/* ---- judging ---- */
#define JUDGE_LOCAL  0
#define JUDGE_API    1
#define JUDGE_HYBRID 2
#define JUDGE_MODE             JUDGE_LOCAL

#define OLLAMA_URL   "http://127.0.0.1:11434/api/chat"
/*
 * Default judge. qwen3:8b at Q4 is ~5.6 GB loaded and needs roughly 7.2 GB of
 * VRAM with OLLAMA_NUM_CTX -- it fits an 8 GB card with about 0.5 GB to spare
 * and nothing else running on the GPU.
 *
 * It is the default because 4b gets the one question that matters wrong.
 * Benchmarked on tinygrad's three open unassigned bounties, 4b scored all three
 * 0 ("bounty is claimed and unassigned", which is both false and
 * self-contradictory) while 8b scored them 10, 9 and 8 and named them correctly
 * as unclaimed. A judge that cannot recognise an unclaimed bounty is not doing
 * the job this daemon exists for.
 *
 * OLLAMA_MODEL_SMALL is kept working and selectable with --model for anyone on
 * a smaller card; it is not merely untested, it is known worse. Neither model
 * survives a batch of 8 -- see LLM_BATCH_SIZE.
 */
#define OLLAMA_MODEL           "qwen3:8b"
#define OLLAMA_MODEL_SMALL     "qwen3:4b"
#define OLLAMA_SCREEN_MODEL    "qwen3:1.7b"  /* JUDGE_HYBRID screening pass */
#define OLLAMA_NUM_GPU         999   /* 0 = pure CPU */
/*
 * Pinned rather than inherited. Ollama's own default is 4096 today, but it is a
 * server-side setting that OLLAMA_CONTEXT_LENGTH can change out from under this
 * daemon -- and the judge's behaviour depends on it sharply, so leaving it to
 * the environment is exactly the kind of implicit override config.h exists to
 * prevent.
 *
 * 4096, not larger, on measurement: at 8192 the same batch of four scored every
 * issue 0, where 4096 graded them 9/8/7/6. Bigger is not better here.
 */
#define OLLAMA_NUM_CTX         4096
#define ANTHROPIC_URL          "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION      "2023-06-01"
#define ANTHROPIC_MODEL        "claude-haiku-4-5-20251001"

/*
 * Issues per judge request. Was 8, which qwen3:4b cannot hold apart: measured
 * on eight real tt-metal issues at ~3400 prompt tokens, 8 of 8 verdicts came
 * back describing a DIFFERENT issue in the batch, and the board published each
 * issue next to another one's reasoning. At 4 the same issues cross-reference
 * zero times and score sensibly (9, 8, 7, 6 against four 0s).
 *
 * Cost is twice the requests -- ~21 batches instead of 11 for a typical cycle,
 * so the judge phase roughly doubles. It dominates the cycle either way, and a
 * cycle has three hours.
 *
 * Raising this needs the cross-contamination test re-run, not just a glance at
 * the scores: the failure is silent and produces a board that looks fine.
 */
#define LLM_BATCH_SIZE         4
#define LLM_BODY_TRUNC         1200
#define LLM_BODY_HEAD          900   /* head/tail split of a truncated body */
#define LLM_SCORE_MIN          6     /* 0..10 from the model */
#define LLM_TIMEOUT_SEC        120
#define LLM_WHY_MAX            96    /* bytes kept from the model's reason */

/* ---- notifications ---- */
#define NTFY_SERVER            "https://ntfy.sh"
/* Credential, not a label. Set `ntfy-topic` in your config file; see the top of
 * this one. `openssl rand -hex 16`. */
#ifndef NTFY_TOPIC
#define NTFY_TOPIC             "REPLACE_ME_WITH_RANDOM_HEX"
#endif
#define NOTIFY_MAX_PER_CYCLE   10
#define NOTIFY_ON_UPDATE       0
#define NOTIFY_TIMEOUT_SEC     20
/*
 * 1: one summary push per cycle whose Click: opens the board, which is the
 * point of the board -- ten pushes a cycle, eight cycles a day, is the noise
 * this replaces. 0: the old behaviour, one push per issue up to
 * NOTIFY_MAX_PER_CYCLE, which is still what you want if you never set up a gist.
 */
#define NOTIFY_SUMMARY_ONLY    1

/* ---- ranked board ---- */
/*
 * The board is published as a secret GitHub Gist: it reuses GH_TOKEN and
 * net/http.c, adds no dependency and nothing to host. Cost is that the token
 * needs `gist` scope on top of public-repo read -- a fine-grained PAT will not
 * do, gists need a classic token with the gist scope. Create the gist once,
 * seeded under GIST_FILENAME -- the publish PATCHes that one file, so a seed
 * under any other name is added alongside the board rather than replaced by it:
 *
 *     printf '# issuewatch\n' > /tmp/issuewatch-board.md
 *     gh gist create -d issuewatch /tmp/issuewatch-board.md
 *
 * The id is the hex from that URL, not the whole URL, and it goes in your
 * config file rather than here: a secret gist is unlisted, not private, so the
 * id is the only thing keeping the board off a public repo page.
 */
#ifndef GIST_ID
#define GIST_ID                "REPLACE_ME_WITH_GIST_ID"
#endif
#define GIST_API_BASE          "https://api.github.com/gists"
#define GIST_WEB_BASE          "https://gist.github.com"
#define GIST_FILENAME          "issuewatch-board.md"
#define GIST_TIMEOUT_SEC       30

/*
 * Entries are a fixed calloc made once at startup (~700 B each, so 200 is
 * ~140 KB resident for the life of the process). On overflow the lowest
 * llm_score goes first, oldest first_seen breaking the tie.
 */
#define BOARD_MAX              200
/*
 * Safety net only. An entry should leave the board because GitHub says it is
 * closed or assigned; this drops one whose re-check has somehow never resolved,
 * so a repo that stops answering cannot pin a stale row there forever.
 */
#define BOARD_STALE_DAYS       30

/* ---- github ---- */
#define GH_API_BASE            "https://api.github.com"
#define GH_USER_AGENT          "issuewatch/0.1"
#define GH_PER_PAGE            100
#define GH_MAX_PAGES           10    /* hard stop; early-exit normally hits first */
#define GH_HTTP_TIMEOUT_SEC    30
/*
 * Watermark for a repo never fetched before. This is a *delta* window, not a
 * coverage guarantee: an issue last touched before it is invisible to the delta
 * fetch forever after, because the watermark only ever moves forward. tinygrad
 * #3039 -- an open, unassigned, labelled bounty -- was last updated 11 days ago
 * and so was never once fetched. The backfill below is what covers that gap;
 * do not try to fix it by enlarging this, which only moves the cliff.
 */
#define GH_FIRST_RUN_LOOKBACK  (7 * 24 * 3600)
/*
 * Fairness cap for the delta fetch. Previously the whole cycle shared
 * n_repos * GH_PER_PAGE slots, so two busy repos (tt-metal and pytorch, every
 * time) consumed the budget and every repo parsed after them got nothing. The
 * per-repo cap is what stops a noisy neighbour starving tinygrad.
 */
#define GH_REPO_MAX_ISSUES     200
/*
 * Whole-cycle ceiling, delta and backfill together. ~8.5 KB per issue against a
 * 96 MB arena, so this is ~21 MB and leaves the judge its pools.
 *
 * Sized so the backfill still gets its full GH_BACKFILL_PAGES after a busy
 * delta: the sweep runs last on whatever is left, and at 1500 a heavy delta
 * could squeeze a 1000-issue sweep down to a few pages -- which would quietly
 * slow the rotation rather than fail, the worst shape of a limit.
 */
#define GH_MAX_ISSUES_PER_CYCLE 2500
/*
 * ---- rolling backfill ----
 *
 * The delta fetch answers "what changed since last cycle". It cannot answer
 * "what is open and unclaimed right now", which is the actual question, and an
 * issue that was already old when the daemon first ran is never in a delta.
 *
 * So one repo per cycle gets a few pages of its open+unassigned backlog walked
 * oldest-first, with the page cursor persisted in the etag file. Around the
 * rotation it goes, a few pages at a time, until every repo's backlog has been
 * seen; then it wraps and re-sweeps, which is how a bounty unassigned last
 * month still reaches the board. Cost is bounded and tiny: GH_BACKFILL_PAGES
 * requests per cycle regardless of how far behind it is.
 *
 * assignee=none is applied server-side -- it roughly halves the pages to walk
 * and matches gh_drop_assigned(), so the backfill never spends a request on an
 * issue the intake filter would throw away anyway.
 */
/*
 * Pages per cycle, for one repo. At 3 the rotation needed ~120 cycles -- about
 * 15 days at POLL_INTERVAL_SEC -- for every repo to finish one pass, and over
 * half of that was pytorch and vllm alone against the 99-page wall below. 10
 * brings it to roughly 5 days for 7 more requests a cycle, against a measured
 * 4870 of rate-limit headroom. The ceiling on it is GH_MAX_ISSUES_PER_CYCLE,
 * not the API: 10 pages is up to 1000 issues, which the sweep must have room
 * for after the delta fetch has taken its share.
 */
#define GH_BACKFILL_PAGES      10
#define GH_BACKFILL_REPOS      1     /* repos swept per cycle */
/*
 * Last page `page=` can address. GitHub serves offset pagination only to 10000
 * items and answers 422 past it -- "please use cursor based pagination" -- so
 * page 100 is a wall, not a short page. Measured: page 99 answers, page 100
 * does not, on every repo large enough to reach it.
 *
 * The sweep wraps here rather than pushing into the 422, because a failure
 * leaves the cursor unstaged and the same pages would be retried forever; and
 * since a stalled repo keeps backfill_round at 0 it would stay the minimum of
 * (round, page) and be picked every cycle, taking the whole rotation down with
 * it. pytorch alone has over 8000 open unassigned issues and reaches this.
 *
 * The cost is honest and bounded: issues past 10000 in creation order are not
 * reachable by the backfill. They are still fetched by the delta whenever they
 * are touched. Covering them properly means cursor pagination and an opaque
 * cursor in the state file instead of an int.
 */
#define GH_BACKFILL_LAST_PAGE  99
/*
 * Sanity bound on a cursor read back from disk, not a depth limit: the sweep
 * stops when GitHub returns a short page, long before this. It exists so a
 * corrupted digit cannot send the fetch to page 2000000000.
 */
#define GH_BACKFILL_MAX_PAGE   100000
/*
 * Cap on the issue body we retain. Bodies routinely carry 40 KB of stack trace,
 * and a full first-run page of 100 of them across 5 repos can exhaust the cycle
 * arena -- which would skip that repo every cycle forever, not just once. The
 * prefilter scans this text, so it must stay well above LLM_BODY_TRUNC.
 */
#define GH_BODY_MAX            8192

/* ---- resources ---- */
#define SEEN_CAPACITY          65536
#define SEEN_PROBE             8     /* collision chain before overwriting */
/*
 * The cycle arena holds every raw HTTP response plus every parsed issue for the
 * whole cycle at once, so its peak is driven by a first run (7-day lookback,
 * 100 issues/repo), not by the steady state.
 *
 * Measured, not guessed: a first-run page is ~600-950 KB per active repo, so
 * the current list is ~6 MB of raw JSON. http.c accumulates into a chunk list
 * and then flattens, which transiently holds both copies, and the parse then
 * copies every retained string out again. 16 MB exhausted the arena on a real
 * 9-repo first run -- four repos were skipped and the judge could not allocate
 * its scratch pool. A measured first run then peaked at 52 MB, so 64 MB was
 * only 22% clear; 96 MB keeps the margin wide enough that a heavier-than-usual
 * page cannot reintroduce the same stall.
 *
 * In --daemon mode this is released between cycles, so it is not idle RSS.
 */
#define ARENA_SIZE             (96u << 20)  /* reset every cycle */
#define PERM_ARENA_SIZE        (4u << 20)   /* automaton; lives for the process */
#define HTTP_MAX_CONCURRENT    8
#define RL_RESERVE             100

#endif /* CONFIG_H */
