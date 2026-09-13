#define CONFIG_WANT_KEYWORDS

/*
 * Prefilter tests. Pure logic, no I/O, so this leans hard on the automaton:
 * phrases, case folding, overlap, the count cap, negative weights, label_only,
 * literal bracket bytes, non-ASCII opacity and a pathological body length.
 *
 * Expected scores are written against the KEYWORDS[] table in src/config.h.
 * They are spelled out as weight arithmetic so a table edit fails loudly here
 * rather than silently changing the gate.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "config.h"
#include "net/github.h"
#include "pipeline/prefilter.h"

#include "../tests/test_util.h"

static arena_t g_arena;
static ac_t *g_ac;

/* Weights lifted from config.h's KEYWORDS[], named so the maths below reads. */
/* "bounty" is calibrated to equal KW_SCORE_MIN exactly, so a funded issue
 * always clears the gate unaided. Tests below use it to build issues that pass
 * regardless of how the gate is later re-tuned. */
#define W_BOUNTY            24
#define W_READ_ME           (-4)
#define W_GOOD_FIRST_ISSUE  12
#define W_HELP_WANTED       10
#define W_PERFORMANCE        4
#define W_CUDA               5
#define W_KERNEL             3
#define W_RACE_CONDITION     4
#define W_MEMORY_LEAK        4
#define W_SEGFAULT           4
#define W_REGRESSION         3
#define W_UNDEF_BEHAVIOR     4
#define W_DEPENDABOT       (-10)
#define W_BOT              (-10)
#define W_BUMP_VERSION      (-8)
#define W_TYPO              (-6)
#define W_TRANSLATION       (-6)

#define LABEL(x) ((x) * KW_LABEL_MULTIPLIER)

static issue_t mk_issue(const char *title, const char *body)
{
    issue_t iss;

    memset(&iss, 0, sizeof iss);
    iss.title = title;
    iss.body = body;
    iss.llm_score = -1;
    return iss;
}

/*
 * The whole justification for Aho-Corasick: a token hash map cannot award a
 * phrase, and awards it wrongly if it splits into tokens.
 */
static void test_phrase_matches_as_a_unit(void)
{
    CHECK_EQ(prefilter_score_text(g_ac, "good first issue", 1), LABEL(W_GOOD_FIRST_ISSUE));

    /* Neither half, nor a near-miss with a word removed, awards the phrase. */
    CHECK_EQ(prefilter_score_text(g_ac, "issue", 1), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "good", 1), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "first", 1), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "good issue", 1), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "first good issue", 1), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "good  first  issue", 1), 0);

    /* Embedded in a longer label still matches -- the phrase is a byte run. */
    CHECK_EQ(prefilter_score_text(g_ac, "a good first issue here", 1),
             LABEL(W_GOOD_FIRST_ISSUE));

    /* Same for the other multi-word terms, which are not label_only. */
    CHECK_EQ(prefilter_score_text(g_ac, "hit a race condition", 0), W_RACE_CONDITION);
    CHECK_EQ(prefilter_score_text(g_ac, "race  condition", 0), 0);
    /* Filler words here must not themselves be KEYWORDS[] terms, or this stops
     * isolating the phrase -- "allocator" used to sit here and became a term. */
    CHECK_EQ(prefilter_score_text(g_ac, "memory leak in the parser", 0), W_MEMORY_LEAK);
    CHECK_EQ(prefilter_score_text(g_ac, "memory", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "leak", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "undefined behavior in the shift", 0),
             W_UNDEF_BEHAVIOR);
}

static void test_case_insensitive(void)
{
    CHECK_EQ(prefilter_score_text(g_ac, "cuda", 0), W_CUDA);
    CHECK_EQ(prefilter_score_text(g_ac, "CUDA", 0), W_CUDA);
    CHECK_EQ(prefilter_score_text(g_ac, "Cuda", 0), W_CUDA);
    CHECK_EQ(prefilter_score_text(g_ac, "cUdA", 0), W_CUDA);

    /* Case folding must survive across a phrase boundary too. */
    CHECK_EQ(prefilter_score_text(g_ac, "GOOD First Issue", 1), LABEL(W_GOOD_FIRST_ISSUE));
    CHECK_EQ(prefilter_score_text(g_ac, "Race Condition", 0), W_RACE_CONDITION);

    /* Mid-token hits are intentional: cudaMemcpy is a CUDA issue. */
    CHECK_EQ(prefilter_score_text(g_ac, "cudaMemcpyAsync returns 700", 0), W_CUDA);
    CHECK_EQ(prefilter_score_text(g_ac, "Kernels are slow", 0), W_KERNEL);
}

/*
 * Overlapping matches: "segfault" ends on the same byte "typo" starts on
 * ("segfaultypo" -- the shared 't'). A naive restart-at-match-end scanner drops
 * the second hit; the failure links must find it.
 *
 * Note: today's KEYWORDS[] happens to contain no pair where one term is a
 * proper suffix of another, so the out_link chain cannot be exercised by a
 * nested pair from the real table. It is implemented and is what would report
 * such a pair; the overlap cases below are what the table can actually prove.
 */
static void test_overlapping_matches(void)
{
    CHECK_EQ(prefilter_score_text(g_ac, "segfaultypo", 0), W_SEGFAULT + W_TYPO);

    /* Same trick across a label_only boundary: "help wanted" + "dependabot". */
    CHECK_EQ(prefilter_score_text(g_ac, "help wantedependabot", 1),
             LABEL(W_HELP_WANTED) + LABEL(W_DEPENDABOT));
    /* In body text the label_only half drops out and only the bot penalty lands. */
    CHECK_EQ(prefilter_score_text(g_ac, "help wantedependabot", 0), W_DEPENDABOT);

    /* Adjacent, non-overlapping terms both fire in the single pass. */
    CHECK_EQ(prefilter_score_text(g_ac, "cuda kernel performance regression", 0),
             W_CUDA + W_KERNEL + W_PERFORMANCE + W_REGRESSION);
}

static void test_count_cap(void)
{
    char buf[256];
    int i;
    size_t n = 0;

    for (i = 0; i < KW_COUNT_CAP + 5; i++) {
        memcpy(buf + n, "cuda ", 5);
        n += 5;
    }
    buf[n] = '\0';

    CHECK_EQ(prefilter_score_text(g_ac, buf, 0), W_CUDA * KW_COUNT_CAP);

    /* One below the cap is not clamped. */
    CHECK_EQ(prefilter_score_text(g_ac, "cuda cuda", 0), W_CUDA * 2);

    /* Negative weights are capped the same way -- noise cannot run away either. */
    CHECK_EQ(prefilter_score_text(g_ac, "typo typo typo typo typo typo", 0),
             W_TYPO * KW_COUNT_CAP);
}

static void test_negative_weights_drive_below_gate(void)
{
    issue_t iss = mk_issue("Bump actions/checkout from 3 to 4", "Signed-off-by: dependabot");
    int score = prefilter_score(g_ac, &iss);

    CHECK(score < 0);
    CHECK(score < KW_SCORE_MIN);
    CHECK_EQ(score, W_DEPENDABOT);

    iss = mk_issue("dependabot: bump version of curl", "");
    score = prefilter_score(g_ac, &iss);
    CHECK_EQ(score, W_DEPENDABOT + W_BUMP_VERSION);
    CHECK(score < KW_SCORE_MIN);

    /* Negatives must be able to cancel out a genuinely positive title. */
    iss = mk_issue("cuda kernel typo in a comment", "translation of the docs");
    score = prefilter_score(g_ac, &iss);
    CHECK_EQ(score, W_CUDA + W_KERNEL + W_TYPO + W_TRANSLATION);
    CHECK(score < KW_SCORE_MIN);
}

static void test_label_only_terms(void)
{
    issue_t iss;

    /* From a label: counted, and multiplied. */
    CHECK_EQ(prefilter_score_text(g_ac, "good first issue", 1), LABEL(W_GOOD_FIRST_ISSUE));
    CHECK_EQ(prefilter_score_text(g_ac, "help wanted", 1), LABEL(W_HELP_WANTED));

    /* From body text: exactly zero, no matter how many times it appears. */
    CHECK_EQ(prefilter_score_text(g_ac, "good first issue", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "help wanted", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "help wanted help wanted help wanted", 0), 0);

    iss = mk_issue("help wanted with this", "marked good first issue by a maintainer");
    CHECK_EQ(prefilter_score(g_ac, &iss), 0);

    iss.labels[0] = "good first issue";
    iss.n_labels = 1;
    CHECK_EQ(prefilter_score(g_ac, &iss), LABEL(W_GOOD_FIRST_ISSUE));
}

static void test_label_multiplier(void)
{
    issue_t iss;

    CHECK_EQ(prefilter_score_text(g_ac, "cuda", 1),
             W_CUDA * KW_LABEL_MULTIPLIER);
    CHECK_EQ(prefilter_score_text(g_ac, "cuda", 1),
             prefilter_score_text(g_ac, "cuda", 0) * KW_LABEL_MULTIPLIER);

    iss = mk_issue("", "");
    iss.labels[0] = "performance";
    iss.labels[1] = "cuda";
    iss.n_labels = 2;
    CHECK_EQ(prefilter_score(g_ac, &iss), LABEL(W_PERFORMANCE) + LABEL(W_CUDA));

    /*
     * The cap is across the whole issue and label hits fill it first: one label
     * hit plus five body hits at KW_COUNT_CAP == 3 gives 2*1 label + 2 body.
     */
    iss = mk_issue("cuda cuda", "cuda cuda cuda");
    iss.labels[0] = "cuda";
    iss.n_labels = 1;
    CHECK_EQ(prefilter_score(g_ac, &iss),
             W_CUDA * (KW_LABEL_MULTIPLIER * 1 + (KW_COUNT_CAP - 1)));
}

/* Pattern bytes are literal, brackets included -- nothing is a metacharacter. */
static void test_bracket_literal(void)
{
    CHECK_EQ(prefilter_score_text(g_ac, "[bot]", 0), W_BOT);
    CHECK_EQ(prefilter_score_text(g_ac, "opened by renovate[bot] again", 0), W_BOT);
    CHECK_EQ(prefilter_score_text(g_ac, "dependabot[bot]", 0), W_DEPENDABOT + W_BOT);

    /* The brackets are required: none of these are the pattern. */
    CHECK_EQ(prefilter_score_text(g_ac, "bot", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "[bot", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "bot]", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "[BOT]", 0), W_BOT);
    CHECK_EQ(prefilter_score_text(g_ac, "[b0t]", 0), 0);
}

/* Bytes >= 0x80 are opaque: no crash, no fold, no false match. */
static void test_non_ascii(void)
{
    issue_t iss;

    CHECK_EQ(prefilter_score_text(g_ac, "\xf0\x9f\x9a\x80 \xf0\x9f\x90\x9b", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "\xd0\xba\xd1\x83\xd0\xb4\xd0\xb0", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "\xc3\xa9\xc3\xa8\xc3\xaa\xc3\xab", 0), 0);
    /* Latin-1 high bytes, i.e. not valid UTF-8 at all. */
    CHECK_EQ(prefilter_score_text(g_ac, "\xff\xfe\xfd\x80\x81", 0), 0);

    /* A real term surrounded by non-ASCII still matches, and only once. */
    CHECK_EQ(prefilter_score_text(g_ac, "\xf0\x9f\x9a\x80 CUDA \xe3\x82\xab", 0), W_CUDA);

    iss = mk_issue("\xf0\x9f\x90\x9b segfault in \xe6\xa0\xb8 kernel",
                   "\xe4\xb8\xad\xe6\x96\x87 memory leak \xf0\x9f\x92\xa5");
    iss.labels[0] = "\xf0\x9f\x94\xa5 performance";
    iss.n_labels = 1;
    CHECK_EQ(prefilter_score(g_ac, &iss),
             W_SEGFAULT + W_KERNEL + W_MEMORY_LEAK + LABEL(W_PERFORMANCE));
}

static void test_null_and_empty_safe(void)
{
    issue_t iss;

    CHECK_EQ(prefilter_score_text(g_ac, NULL, 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, NULL, 1), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "", 0), 0);
    CHECK_EQ(prefilter_score_text(g_ac, "", 1), 0);
    CHECK_EQ(prefilter_score_text(NULL, "cuda", 0), 0);

    iss = mk_issue(NULL, NULL);
    CHECK_EQ(prefilter_score(g_ac, &iss), 0);

    /* A NULL label among real ones must be skipped, not dereferenced. */
    iss = mk_issue(NULL, NULL);
    iss.labels[0] = NULL;
    iss.labels[1] = "cuda";
    iss.labels[2] = NULL;
    iss.n_labels = 3;
    CHECK_EQ(prefilter_score(g_ac, &iss), LABEL(W_CUDA));

    CHECK_EQ(prefilter_score(g_ac, NULL), 0);
    CHECK_EQ(prefilter_score(NULL, &iss), 0);

    /* n_labels beyond the array must be clamped, not trusted. */
    iss = mk_issue("", "");
    iss.labels[0] = "cuda";
    iss.n_labels = GH_MAX_LABELS + 99;
    CHECK_EQ(prefilter_score(g_ac, &iss), LABEL(W_CUDA));

    CHECK_EQ(prefilter_apply(g_ac, NULL, 4), 0);
    CHECK_EQ(prefilter_apply(NULL, &iss, 1), 0);
}

static void test_apply_compacts_stably(void)
{
    issue_t arr[6];
    size_t kept;

    /* 0,2,5 pass the gate; 1,3,4 do not. The survivors lead with "bounty" so
     * they stay above the gate no matter how KW_SCORE_MIN is re-tuned. */
    arr[0] = mk_issue("bounty: cuda kernel regression", "");     /* 24+5+3+3 */
    arr[1] = mk_issue("typo in README", "");                     /* -6-4 */
    arr[2] = mk_issue("bounty: memory leak and segfault", "");   /* 24+4+4 */
    arr[3] = mk_issue("dependabot bump version", "");            /* -18 */
    arr[4] = mk_issue("nothing interesting here", "");           /* 0 */
    arr[5] = mk_issue("bounty: undefined behavior in cuda", ""); /* 24+4+5 */
    arr[0].id = 100;
    arr[1].id = 101;
    arr[2].id = 102;
    arr[3].id = 103;
    arr[4].id = 104;
    arr[5].id = 105;

    kept = prefilter_apply(g_ac, arr, 6);
    CHECK_EQ(kept, 3);
    CHECK_EQ(arr[0].id, 100);
    CHECK_EQ(arr[1].id, 102);
    CHECK_EQ(arr[2].id, 105);

    /* Scores are written through, and every survivor clears the gate. */
    CHECK_EQ(arr[0].kw_score, W_BOUNTY + W_CUDA + W_KERNEL + W_REGRESSION);
    CHECK_EQ(arr[1].kw_score, W_BOUNTY + W_MEMORY_LEAK + W_SEGFAULT);
    CHECK_EQ(arr[2].kw_score, W_BOUNTY + W_UNDEF_BEHAVIOR + W_CUDA);
    CHECK(arr[0].kw_score >= KW_SCORE_MIN);
    CHECK(arr[1].kw_score >= KW_SCORE_MIN);
    CHECK(arr[2].kw_score >= KW_SCORE_MIN);
}

static void test_apply_edge_cases(void)
{
    issue_t arr[3];
    issue_t one;

    /* Empty array. */
    CHECK_EQ(prefilter_apply(g_ac, arr, 0), 0);

    /* Nothing survives. */
    arr[0] = mk_issue("typo", "");
    arr[1] = mk_issue("translation update", "");
    arr[2] = mk_issue("", "");
    CHECK_EQ(prefilter_apply(g_ac, arr, 3), 0);
    /* Topical-but-unfunded must also fail: every watched repo is a GPU project,
     * so "cuda kernel" alone is the noise floor here, not a signal. */
    CHECK_EQ(arr[0].kw_score, W_TYPO);
    CHECK_EQ(arr[1].kw_score, W_TRANSLATION);
    CHECK_EQ(arr[2].kw_score, 0);

    /* Everything survives -- the no-move path. */
    arr[0] = mk_issue("bounty: cuda cuda", "");
    arr[1] = mk_issue("bounty: segfault in the kernel", "");
    arr[2] = mk_issue("bounty: race condition and a memory leak", "");
    CHECK_EQ(prefilter_apply(g_ac, arr, 3), 3);

    /* The gate is inclusive: "bounty" scores exactly KW_SCORE_MIN and is kept.
     * Asserting the equality too, so re-tuning the gate without re-tuning the
     * bounty weight fails here rather than silently dropping funded issues. */
    one = mk_issue("bounty", "");
    CHECK_EQ(prefilter_score(g_ac, &one), W_BOUNTY);
    CHECK_EQ(W_BOUNTY, KW_SCORE_MIN);
    CHECK_EQ(prefilter_apply(g_ac, &one, 1), 1);

    /* Just below the gate is dropped. */
    one = mk_issue("cuda kernel performance", "");
    CHECK(prefilter_score(g_ac, &one) < KW_SCORE_MIN);
    CHECK_EQ(prefilter_apply(g_ac, &one, 1), 0);
}

/*
 * Worst case for failure-link walking: a huge run of one byte, plus a huge run
 * of a term that keeps re-entering the trie. Must terminate quickly and must
 * not recurse -- the scan is a flat loop, so the stack is untouched.
 */
static void test_pathological_body(void)
{
    const size_t n = 100u * 1024u;
    char *big = malloc(n + 1);
    issue_t iss;
    size_t i;

    CHECK(big != NULL);
    if (big == NULL)
        return;

    memset(big, 'a', n);
    big[n] = '\0';
    iss = mk_issue("cuda", big);
    CHECK_EQ(prefilter_score(g_ac, &iss), W_CUDA);

    /* A byte that is a live prefix in the trie ('c' -> "cuda"). */
    memset(big, 'c', n);
    big[n] = '\0';
    CHECK_EQ(prefilter_score_text(g_ac, big, 0), 0);

    /* 25k real matches, clamped by the cap and costing no allocation. */
    for (i = 0; i + 4 <= n; i += 4)
        memcpy(big + i, "cuda", 4);
    big[i] = '\0';
    CHECK_EQ(prefilter_score_text(g_ac, big, 0), W_CUDA * KW_COUNT_CAP);

    /* A run of the longest phrase's first byte, then the phrase itself. */
    memset(big, 'u', n);
    memcpy(big + n - 18, "undefined behavior", 18);
    big[n] = '\0';
    CHECK_EQ(prefilter_score_text(g_ac, big, 0), W_UNDEF_BEHAVIOR);

    free(big);
}

int main(void)
{
    arena_t tiny;
    ac_t *probe;

    if (arena_init(&g_arena, ARENA_SIZE) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }
    if (prefilter_init(&g_arena, KEYWORDS, N_KEYWORDS, &g_ac) != 0 || g_ac == NULL) {
        fprintf(stderr, "prefilter_init failed\n");
        return 2;
    }

    /* Bad arguments are rejected, not dereferenced. */
    probe = g_ac;
    CHECK(prefilter_init(NULL, KEYWORDS, N_KEYWORDS, &probe) < 0);
    CHECK(prefilter_init(&g_arena, KEYWORDS, N_KEYWORDS, NULL) < 0);
    CHECK(prefilter_init(&g_arena, NULL, 0, &probe) < 0);
    CHECK(prefilter_init(&g_arena, KEYWORDS, 0, &probe) < 0);

    /* An exhausted arena must yield a negative return and a NULL automaton. */
    if (arena_init(&tiny, 64) == 0) {
        probe = g_ac;
        CHECK(prefilter_init(&tiny, KEYWORDS, N_KEYWORDS, &probe) < 0);
        CHECK(probe == NULL);
        arena_destroy(&tiny);
    }

    TEST_RUN(test_phrase_matches_as_a_unit);
    TEST_RUN(test_case_insensitive);
    TEST_RUN(test_overlapping_matches);
    TEST_RUN(test_count_cap);
    TEST_RUN(test_negative_weights_drive_below_gate);
    TEST_RUN(test_label_only_terms);
    TEST_RUN(test_label_multiplier);
    TEST_RUN(test_bracket_literal);
    TEST_RUN(test_non_ascii);
    TEST_RUN(test_null_and_empty_safe);
    TEST_RUN(test_apply_compacts_stably);
    TEST_RUN(test_apply_edge_cases);
    TEST_RUN(test_pathological_body);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
