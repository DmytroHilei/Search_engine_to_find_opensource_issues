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

/* ---- repos to watch ---- */
#define WATCHED_REPOS \
    X("ggml-org/llama.cpp")     \
    X("NVIDIA/cutlass")         \
    X("triton-lang/triton")     \
    X("pytorch/pytorch")        \
    X("openai/tiktoken")

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

/* ---- what you care about (goes into the LLM system prompt) ---- */
#define USER_PROFILE \
    "C/C++ and CUDA developer, HPC and GPU kernels. Interested in: "        \
    "performance regressions, kernel optimization, numerical correctness, " \
    "memory/allocator bugs, build system issues on Linux. "                 \
    "Not interested in: docs, typos, Windows/macOS-only, JS/Python "        \
    "packaging, dependency bumps, feature requests without a design."

/* ---- keyword prefilter ---- */
typedef struct {
    const char *term;
    int weight;
    int label_only;
} kw_t;

#ifdef CONFIG_WANT_KEYWORDS
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
#define N_KEYWORDS (sizeof KEYWORDS / sizeof KEYWORDS[0])
#endif

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
 * 100 issues/repo), not by the steady state. 4 MB is comfortably under that.
 * In --daemon mode this is released between cycles, so it is not idle RSS.
 */
#define ARENA_SIZE             (16u << 20)  /* reset every cycle */
#define PERM_ARENA_SIZE        (4u << 20)   /* automaton; lives for the process */
#define HTTP_MAX_CONCURRENT    8
#define RL_RESERVE             100

#endif /* CONFIG_H */
