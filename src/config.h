#ifndef CONFIG_H
#define CONFIG_H

/*
 * The only file a user edits. Everything here is a compile-time constant.
 * Secrets never live here -- see GH_TOKEN / ANTHROPIC_API_KEY / NTFY_TOKEN,
 * which are read from the environment at startup.
 */

/* ---- scheduling ---- */
#define POLL_INTERVAL_SEC      (3 * 3600)
#define POLL_JITTER_SEC        600

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
    X("ggml-org/llama.cpp")         \
    X("openssl/openssl")            \
    X("pytorch/pytorch")            \
    X("triton-lang/triton")         \
    X("NVIDIA/cutlass")             \
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
    "(1) PAID. An explicit cash bounty, prize, grant or stipend. Score 9-10 " \
    "if the bounty is unclaimed and unassigned; a claimed bounty is worth "   \
    "little because a pull request only counts when you hold the "           \
    "assignment.\n"                                                          \
    "(2) REPUTATION. Unpaid work in a high-visibility project where landing " \
    "a patch is durable credibility: kernel or compiler internals, a "        \
    "correctness fix with a regression test, a measurable performance win. "  \
    "Score 6-8 even with no money attached.\n"                               \
    "(3) INTEREST. Genuinely novel or difficult problems -- unusual "         \
    "hardware, emulators, numerical analysis, allocator and memory-model "    \
    "work -- that are worth doing for their own sake. Score 6-7.\n"           \
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
#define OLLAMA_MODEL "qwen3:4b"
#define OLLAMA_SCREEN_MODEL    "qwen3:1.7b"  /* JUDGE_HYBRID screening pass */
#define OLLAMA_NUM_GPU         999   /* 0 = pure CPU */
#define ANTHROPIC_URL          "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION      "2023-06-01"
#define ANTHROPIC_MODEL        "claude-haiku-4-5-20251001"

#define LLM_BATCH_SIZE         8
#define LLM_BODY_TRUNC         1200
#define LLM_BODY_HEAD          900   /* head/tail split of a truncated body */
#define LLM_SCORE_MIN          6     /* 0..10 from the model */
#define LLM_TIMEOUT_SEC        120
#define LLM_WHY_MAX            96    /* bytes kept from the model's reason */

/* ---- notifications ---- */
#define NTFY_SERVER            "https://ntfy.sh"
#define NTFY_TOPIC             "REPLACE_ME_WITH_RANDOM_HEX"
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
 * do, gists need a classic token with the gist scope. Create the gist once
 * (any content), then paste its id here:
 *
 *     gh gist create --secret -d issuewatch board.md
 *
 * GIST_ID is the hex id from the URL, not the whole URL.
 */
#define GIST_ID                "REPLACE_ME_WITH_GIST_ID"
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
#define GH_FIRST_RUN_LOOKBACK  (7 * 24 * 3600)  /* watermark for an unseen repo */
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
