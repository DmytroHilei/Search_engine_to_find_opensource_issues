/*
 * judge.c: verdict parsing, body truncation, compaction/sort, and envelope
 * extraction. Fixtures only -- no network (CLAUDE.md testing rules).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../tests/test_util.h"

#include "core/arena.h"
#include "config.h"
#include "core/util.h"
#include "net/github.h"
#include "pipeline/judge.h"

/*
 * Not in judge.h -- that header is a frozen contract. judge.c exports the two
 * envelope extractors so both can be exercised whatever JUDGE_MODE was built.
 */
extern int judge_extract_ollama_verdicts(arena_t *a, const char *json, size_t json_len,
                                         const char **out, size_t *out_len);
extern int judge_extract_anthropic_verdicts(arena_t *a, const char *json, size_t json_len,
                                            const char **out, size_t *out_len);

/* Mirrors the result codes in judge.c; also header-less by design. */
#define JUDGE_WINDOW_OK        0
#define JUDGE_WINDOW_NEAR      1
#define JUDGE_WINDOW_TRUNCATED 2
#define JUDGE_WINDOW_UNKNOWN   3
extern int judge_ollama_window(const char *json, size_t json_len, size_t prompt_bytes,
                               long *used_tokens);

/* Only built for the Ollama-backed modes; the API path constrains output with a
 * forced tool call instead of a grammar. */
#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID
extern const char *judge_render_format_schema(arena_t *a, size_t n);
#endif

static arena_t g_arena;

/*
 * Stamps a just-now updated_at. issues_reset() leaves it NULL, which reads as
 * "unknown age" and is correct for tests about anything else -- but a test
 * asserting a normal score should not depend on that reading.
 */
static void fresh(issue_t *is, size_t n)
{
    static char now[32];
    size_t i;

    if (now[0] == '\0')
        iso8601_format(time(NULL), now, sizeof now);
    for (i = 0; i < n; i++)
        is[i].updated_at = now;
}

static void issues_reset(issue_t *is, size_t n)
{
    size_t i;

    memset(is, 0, n * sizeof *is);
    for (i = 0; i < n; i++) {
        is[i].id = (long long)(1000 + i);
        is[i].number = (int)(i + 1);
        is[i].repo = "ggml-org/llama.cpp";
        is[i].title = "placeholder";
        is[i].body = "";
        is[i].llm_score = -1;
        is[i].why = NULL;
    }
}

/* Full UTF-8 validity walk: no lone continuation bytes, no truncated tail. */
static int utf8_valid(const char *s, size_t len)
{
    size_t i = 0;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need, k;

        if (c < 0x80u)
            need = 0;
        else if ((c & 0xe0u) == 0xc0u)
            need = 1;
        else if ((c & 0xf0u) == 0xe0u)
            need = 2;
        else if ((c & 0xf8u) == 0xf0u)
            need = 3;
        else
            return 0;                   /* a lone 0x80..0xbf, or 0xf8+ */

        if (need > 0 && i + need >= len)
            return 0;                   /* sequence runs off the end */
        for (k = 1; k <= need; k++)
            if (((unsigned char)s[i + k] & 0xc0u) != 0x80u)
                return 0;
        i += need + 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ tests */

#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID
/*
 * The format schema must pin the verdict array to the batch size. Measured on
 * qwen3:4b with a batch of 8: unbounded returned exactly 8 verdicts in 4 of 9
 * trials (three empty arrays, one of length 1, one overrun), bounded in 9 of 9.
 * A short reply is silently discarded candidates, so the bound is asserted here
 * rather than trusted to survive a refactor of the schema builders.
 */
static void test_format_schema_pins_the_array_length(void)
{
    const char *s;

    s = judge_render_format_schema(&g_arena, 8);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"minItems\":8") != NULL);
    CHECK(strstr(s, "\"maxItems\":8") != NULL);
    /* The item shape must survive alongside the bound. */
    CHECK(strstr(s, "\"type\":\"array\"") != NULL);
    CHECK(strstr(s, "\"why\"") != NULL);

    /* It tracks the batch, rather than being LLM_BATCH_SIZE baked in: a final
     * short batch must ask for its own length, not the full one. */
    s = judge_render_format_schema(&g_arena, 3);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"minItems\":3") != NULL);
    CHECK(strstr(s, "\"maxItems\":3") != NULL);
    CHECK(strstr(s, "\"minItems\":8") == NULL);

    CHECK(judge_render_format_schema(NULL, 8) == NULL);
}
#endif

static void test_parse_ok(void)
{
    issue_t is[3];
    char *json;
    size_t len;
    int r;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 3);

    r = judge_parse_verdicts(&g_arena, json, len, is, 3);
    CHECK_EQ(r, 3);
    CHECK_EQ(is[0].llm_score, 8);
    CHECK_STREQ(is[0].why, "CUDA kernel perf regression on sm_90, unassigned");
    /* keep=false must land below LLM_SCORE_MIN whatever score the model sent. */
    CHECK_EQ(is[1].llm_score, 0);
    CHECK(is[1].llm_score < LLM_SCORE_MIN);
    CHECK_EQ(is[2].llm_score, 6);
    CHECK_STREQ(is[2].why, "Allocator double free during multi-GPU teardown");

    free(json);
}

/* The `why` string must be an arena copy: the yyjson doc is freed inside
 * judge_parse_verdicts(), and the source buffer belongs to the caller. */
static void test_why_is_copied(void)
{
    issue_t is[3];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 3);
    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 3), 3);

    memset(json, 'X', len);
    free(json);

    CHECK_STREQ(is[0].why, "CUDA kernel perf regression on sm_90, unassigned");
    CHECK_STREQ(is[2].why, "Allocator double free during multi-GPU teardown");
}

/*
 * The memory-safety test, and now also the contract test for ignoring "i".
 *
 * The fixture carries i=99, i=-3 and i=2^31 against a heap array of exactly two
 * issues: under ASan any write driven by those trips a redzone. Verdicts land
 * by array position instead, which is bounded by `n` by construction, so a
 * hostile "i" is not merely rejected -- it is never consulted.
 *
 * The first two elements therefore apply to issues 0 and 1 in order, and the
 * two beyond the batch size are dropped. Before this, a model that shifted its
 * own indices silently attached each verdict to the wrong issue.
 */
/*
 * The prompt asks for 12 words and the model overshoots: on one real board 40
 * of 60 rows sat at exactly LLM_WHY_MAX, every one cut mid-word. `why` is the
 * notification body, so it stops at a sentence when there is one and at a word
 * otherwise -- and in neither case with a dangling space.
 */
static void test_why_stops_on_a_boundary(void)
{
    issue_t is[2];
    char *json;
    size_t len, w0, w1;

    json = fixture_read("tests/fixtures/verdicts_long_why.json", &len);
    issues_reset(is, 2);
    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 2), 2);
    free(json);

    w0 = strlen(is[0].why);
    w1 = strlen(is[1].why);

    CHECK(w0 <= (size_t)LLM_WHY_MAX);
    CHECK(w1 <= (size_t)LLM_WHY_MAX);
    /*
     * One sentence ends below the cap, at byte 75, and it ends on "src1." --
     * an identifier ending in a digit, which a boundary rule keyed on the byte
     * before the mark would refuse to see. The second sentence ends at 103 and
     * is past the cap, so it goes entirely rather than half.
     */
    CHECK_EQ(w0, 75u);
    CHECK_EQ(is[0].why[w0 - 1], '.');
    CHECK(strstr(is[0].why, "non-contiguous src1.") != NULL);
    CHECK(strstr(is[0].why, "failing case") == NULL);
    /* No terminator anywhere, so it falls back to the last whole word. */
    CHECK(w1 > (size_t)LLM_WHY_MAX / 2);
    CHECK(is[1].why[w1 - 1] != ' ');
    CHECK_STREQ(is[1].why + w1 - 7, "enabled");
}

/*
 * The top band asserts a fact about the payload, so parse checks it. The first
 * two verdicts are the real failure this exists for: an 8B judge rescoring a
 * live board put pytorch#59515 at 9 for an "unclaimed, unassigned cash bounty"
 * it had invented, on an issue whose only labels were "module: cuda" and
 * "triaged". Both must land at JUDGE_UNPAID_CAP; the two carrying real evidence
 * must be left alone, one via the title and one via a label.
 */
static void test_unpaid_cannot_reach_the_top_band(void)
{
    issue_t is[4];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_paid_claim.json", &len);
    issues_reset(is, 4);

    is[0].title = "Conv1d with large batch size and half precision returns incorrect results";
    is[0].labels[0] = "module: cuda";
    is[0].labels[1] = "module: correctness (silent)";
    is[0].n_labels = 2;

    is[1].title = "Heap buffer overflow via unvalidated n_vocab";   /* no evidence */

    is[2].title = "Rewrite the fp16 reduction path";                /* label carries it */
    is[2].labels[0] = "bounty";
    is[2].n_labels = 1;

    is[3].title = "[$500 Bounty] mul_mat is wrong for non-contiguous src1";

    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 4), 4);
    free(json);

    CHECK_EQ(is[0].llm_score, 8);        /* 10, invented -> capped */
    CHECK_EQ(is[1].llm_score, 8);        /*  9, invented -> capped */
    CHECK_EQ(is[2].llm_score, 9);        /*  9, `bounty` label -- untouched */
    CHECK_EQ(is[3].llm_score, 9);        /*  8, but "[$500 Bounty]" -> floored */
}

/*
 * The other direction, and the one that matters more: a paid issue the model
 * wanted to discard. Observed live at batch=1, an 8B judge answered keep=false
 * on a tinygrad bounty with the reason "payment: YES" -- a verdict contradicting
 * its own stated evidence. keep=false zeroes a score, so without the floor that
 * issue leaves the pipeline entirely and the user never hears about it.
 */
static void test_a_paid_issue_survives_keep_false(void)
{
    issue_t is[2];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 2);
    is[1].title = "[Bounty] Outline of NVIDIA e2e full FP16 matmul speed";

    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 2), 2);
    free(json);

    CHECK_EQ(is[0].llm_score, 8);        /* unpaid, keep=true, at the cap */
    CHECK_EQ(is[1].llm_score, 9);        /* keep=false, score 1 -- floored */
    CHECK(is[1].llm_score >= LLM_SCORE_MIN);
}

/*
 * has_amount() sets a floor now, so a bare '$' must not match. Both of these
 * are ordinary systems-programming titles, and both would have been floored to
 * the top of the board by a naive substring search for "$".
 */
/*
 * GSoC is not a bounty. OpenCV keeps a permanent idea list, so the label marks
 * "somebody could propose this one summer", not claimable money -- it needs an
 * accepted student inside a seasonal programme. When it counted as payment, 16
 * of the 19 rows in the top band of a real 200-row board were OpenCV idea
 * entries, outranking every genuine bounty.
 */
static void test_gsoc_is_not_payment(void)
{
    issue_t is[2];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 2);

    is[0].title = "GSOC: Dynamic CUDA Support in OpenCV DNN";
    is[1].title = "Add support for libcamera";
    is[1].labels[0] = "GSoC";
    is[1].labels[1] = "feature";
    is[1].n_labels = 2;
    fresh(is, 2);

    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 2), 2);
    free(json);

    /* Scored on merit in step 2, not floored into the money band. */
    CHECK_EQ(is[0].llm_score, 8);        /* keep=true, score 8 */
    CHECK_EQ(is[1].llm_score, 0);        /* keep=false */
}

/*
 * Staleness. The three criteria measure how well an issue is WRITTEN, and a
 * well-written issue stays well-written after everyone stopped caring: on a
 * saturated board, 16 of a 45-row sample of the top unpaid band had been idle
 * over a year. They are demoted below LLM_SCORE_MIN, not deleted.
 */
static void test_an_abandoned_issue_is_demoted(void)
{
    issue_t is[3];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 3);
    fresh(is, 3);
    is[0].updated_at = "2019-01-01T00:00:00Z";      /* years idle */

    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 3), 3);
    free(json);

    CHECK_EQ(is[0].llm_score, 5);        /* was 8 */
    CHECK_EQ(is[2].llm_score, 6);        /* fresh, untouched */
}

/*
 * Two ways the cap must not fire. An unparseable timestamp reads as unknown,
 * never as stale -- a parse bug would otherwise quietly empty the board. And a
 * paid issue never reaches the check at all: tinygrad's bounties are years old
 * and still the most valuable rows on the board.
 */
static void test_staleness_never_fires_on_unknown_or_paid(void)
{
    issue_t is[3];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 3);
    fresh(is, 3);

    is[0].updated_at = NULL;
    is[1].updated_at = "not a timestamp";
    is[2].updated_at = "2019-01-01T00:00:00Z";
    is[2].title = "[Bounty] years old and still unclaimed";

    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 3), 3);
    free(json);

    CHECK_EQ(is[0].llm_score, 8);        /* unknown age: untouched */
    CHECK_EQ(is[1].llm_score, 0);        /* keep=false, unrelated */
    CHECK_EQ(is[2].llm_score, 9);        /* paid: floored, never demoted */
}

static void test_a_bare_dollar_is_not_an_amount(void)
{
    issue_t is[2];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 2);
    is[0].title = "$HOME is not expanded in the generated build script";
    is[1].title = "PS1 prompt: literal $ breaks the test harness";

    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 2), 2);
    free(json);

    CHECK_EQ(is[0].llm_score, 8);        /* not floored */
    CHECK_EQ(is[1].llm_score, 0);        /* keep=false still wins when unpaid */
}

/* The cap is a ceiling, not a floor: it must never lift a low score. */
static void test_the_cap_only_lowers(void)
{
    issue_t is[3];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 3);
    CHECK_EQ(judge_parse_verdicts(&g_arena, json, len, is, 3), 3);
    free(json);

    CHECK_EQ(is[0].llm_score, 8);
    CHECK_EQ(is[1].llm_score, 0);        /* keep=false still wins */
    CHECK_EQ(is[2].llm_score, 6);
}

static void test_parse_ignores_model_index(void)
{
    issue_t *is = calloc(2, sizeof *is);
    char *json;
    size_t len;
    int r;

    CHECK(is != NULL);
    if (is == NULL)
        return;
    issues_reset(is, 2);
    /* The fixture scores both 9. Payment evidence keeps the unpaid-score cap
     * out of a test that is about which issue a verdict lands on. */
    is[0].title = "[Bounty] verdict index handling";
    is[1].title = "[Bounty] verdict index handling";

    json = fixture_read("tests/fixtures/verdicts_bad.json", &len);
    r = judge_parse_verdicts(&g_arena, json, len, is, 2);

    /* Two positions exist, so two verdicts land and the tail is ignored. */
    CHECK_EQ(r, 2);
    CHECK_EQ(is[0].llm_score, 9);       /* element 0, whatever its "i" claimed */
    CHECK_EQ(is[1].llm_score, 9);       /* element 1 */
    CHECK(is[0].why != NULL);
    CHECK(is[1].why != NULL);
    /* Position 0 took the FIRST element, not the one claiming i=0. */
    if (is[0].why != NULL)
        CHECK(strstr(is[0].why, "past the end") != NULL);

    free(json);
    free(is);
}

static void test_parse_malformed(void)
{
    issue_t is[3];
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/verdicts_ok.json", &len);
    issues_reset(is, 3);

    /* Truncated mid-object. */
    CHECK(judge_parse_verdicts(&g_arena, json, 45, is, 3) < 0);
    /* Truncated mid-string. */
    CHECK(judge_parse_verdicts(&g_arena, json, 70, is, 3) < 0);
    /* Empty, prose, and a non-array root. */
    CHECK(judge_parse_verdicts(&g_arena, "", 0, is, 3) < 0);
    CHECK(judge_parse_verdicts(&g_arena, "Sure! Here are the verdicts:", 28, is, 3) < 0);
    CHECK(judge_parse_verdicts(&g_arena, "{\"i\":0}", 7, is, 3) < 0);
    /* Nothing above may have written anything. */
    CHECK_EQ(is[0].llm_score, -1);
    CHECK_EQ(is[2].llm_score, -1);

    free(json);
}

static void test_truncate_short(void)
{
    const char *body = "one line, well under the cap";
    const char *out = judge_truncate_body(&g_arena, body);

    CHECK(out != NULL);
    CHECK_STREQ(out, body);
    CHECK(strstr(out, "[...]") == NULL);

    /* A NULL body is the GitHub null-body case, not an error. */
    CHECK_STREQ(judge_truncate_body(&g_arena, NULL), "");
}

static void test_truncate_long(void)
{
    size_t n = 40000, i, olen;
    char *body = malloc(n + 1);
    const char *out;

    CHECK(body != NULL);
    if (body == NULL)
        return;
    for (i = 0; i < n; i++)
        body[i] = (char)('a' + (i % 26));
    memcpy(body, "HEAD-MARKER", 11);
    memcpy(body + n - 11, "TAIL-MARKER", 11);
    body[n] = '\0';

    out = judge_truncate_body(&g_arena, body);
    CHECK(out != NULL);
    if (out == NULL) {
        free(body);
        return;
    }
    olen = strlen(out);

    CHECK(olen <= (size_t)LLM_BODY_TRUNC);
    CHECK(strstr(out, "[...]") != NULL);
    /* Head preserved verbatim for LLM_BODY_HEAD bytes (pure ASCII input). */
    CHECK(memcmp(out, body, (size_t)LLM_BODY_HEAD) == 0);
    /* Tail preserved: the last bytes of the body survive the elision. */
    CHECK(memcmp(out + olen - 11, body + n - 11, 11) == 0);

    free(body);
}

/*
 * Both cut points land mid-sequence: one ASCII byte then 3-byte codepoints, so
 * offset LLM_BODY_HEAD is 2 bytes into a character. A lone continuation byte
 * would make the whole request invalid JSON.
 */
static void test_truncate_utf8_boundary(void)
{
    size_t n = 30000, w = 0, olen, head_len, tail_off;
    char *body = malloc(n + 8);
    const char *out, *mark;

    CHECK(body != NULL);
    if (body == NULL)
        return;
    body[w++] = '.';
    while (w + 3 <= n) {                    /* U+20AC EURO SIGN */
        body[w++] = (char)0xe2;
        body[w++] = (char)0x82;
        body[w++] = (char)0xac;
    }
    body[w] = '\0';

    CHECK_EQ((size_t)((LLM_BODY_HEAD - 1) % 3), 2u);   /* mid-sequence by design */

    out = judge_truncate_body(&g_arena, body);
    CHECK(out != NULL);
    if (out == NULL) {
        free(body);
        return;
    }
    olen = strlen(out);
    mark = strstr(out, "\n[...]\n");

    CHECK(olen <= (size_t)LLM_BODY_TRUNC);
    CHECK(mark != NULL);
    CHECK(utf8_valid(out, olen));
    if (mark != NULL) {
        head_len = (size_t)(mark - out);
        tail_off = head_len + strlen("\n[...]\n");

        /* Each side of the elision must be valid UTF-8 on its own -- that is
         * what proves neither cut point split a sequence, rather than one cut
         * accidentally repairing the other. */
        CHECK(utf8_valid(out, head_len));
        CHECK(utf8_valid(out + tail_off, olen - tail_off));
        /* The head was walked back off the mid-sequence byte at LLM_BODY_HEAD. */
        CHECK(head_len < (size_t)LLM_BODY_HEAD);
        CHECK(((unsigned char)out[tail_off] & 0xc0u) != 0x80u);
    }

    free(body);
}

static void test_apply_compacts_and_sorts(void)
{
    issue_t a[6], b[6];
    size_t k, i;

    issues_reset(a, 6);
    a[0].id = 10; a[0].llm_score = -1;                 /* never judged */
    a[1].id = 11; a[1].llm_score = LLM_SCORE_MIN;      /* exactly at the gate */
    a[2].id = 12; a[2].llm_score = 10;
    a[3].id = 13; a[3].llm_score = LLM_SCORE_MIN - 1;  /* just under */
    a[4].id = 14; a[4].llm_score = 10;                 /* ties with id 12 */
    a[5].id = 15; a[5].llm_score = 7;

    k = judge_apply(a, 6);
    CHECK_EQ(k, 4);
    CHECK_EQ(a[0].llm_score, 10);
    CHECK_EQ(a[0].id, 12);              /* tiebreak: lower id first */
    CHECK_EQ(a[1].llm_score, 10);
    CHECK_EQ(a[1].id, 14);
    CHECK_EQ(a[2].llm_score, 7);
    CHECK_EQ(a[3].llm_score, LLM_SCORE_MIN);

    /* Same set, different input order -> byte-identical output order. */
    issues_reset(b, 6);
    b[0].id = 14; b[0].llm_score = 10;
    b[1].id = 15; b[1].llm_score = 7;
    b[2].id = 13; b[2].llm_score = LLM_SCORE_MIN - 1;
    b[3].id = 12; b[3].llm_score = 10;
    b[4].id = 10; b[4].llm_score = -1;
    b[5].id = 11; b[5].llm_score = LLM_SCORE_MIN;

    CHECK_EQ(judge_apply(b, 6), 4);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(b[i].id, a[i].id);
        CHECK_EQ(b[i].llm_score, a[i].llm_score);
    }

    CHECK_EQ(judge_apply(a, 0), 0);
    CHECK_EQ(judge_apply(NULL, 3), 0);
}

/* Ollama double-encodes: message.content is a STRING holding the array. */
/*
 * Overflowing OLLAMA_NUM_CTX does not fail: Ollama cuts the prompt to half the
 * window, keeps the tail, and reports the token count AFTER the cut. Measured at
 * batch 8 / num_ctx 4096 -- `prompt=4545 new=2050` for a 15249-byte prompt --
 * and 17 of 26 identifiable verdicts then described another issue in the batch.
 * The ratio of bytes sent to tokens counted is the tell: 3.32-3.69 across
 * fifteen real batches of 4, 7.44-7.60 when truncated.
 */
static void test_window_detects_truncation(void)
{
    char *json;
    size_t len;
    long used = -1;

    json = fixture_read("tests/fixtures/ollama_reply_truncated.json", &len);
    CHECK_EQ(judge_ollama_window(json, len, 15249, &used), JUDGE_WINDOW_TRUNCATED);
    CHECK_EQ(used, 2050 + 110);
    free(json);
}

/* The real batches of 4 at both ends of their measured range must pass. */
static void test_window_passes_real_batches(void)
{
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/ollama_reply.json", &len);   /* 1412 tokens */
    CHECK_EQ(judge_ollama_window(json, len, 1412 * 332 / 100, NULL), JUDGE_WINDOW_OK);
    CHECK_EQ(judge_ollama_window(json, len, 1412 * 369 / 100, NULL), JUDGE_WINDOW_OK);
    /* Exactly on the threshold is not truncation: the comparison is strict. */
    CHECK_EQ(judge_ollama_window(json, len, 1412 * (size_t)OLLAMA_TRUNC_BYTES_PER_TOKEN,
                                 NULL), JUDGE_WINDOW_OK);
    free(json);
}

/* Fits, but the reply is counted too: 3400 in + 150 out is past 85% of 4096. */
static void test_window_warns_near_the_edge(void)
{
    char *json;
    size_t len;

    /* Guards the fixture, not the code: retuning either macro should say so here
     * rather than as a baffling OK. */
    CHECK((3400 + 150) * 100 > OLLAMA_NUM_CTX * OLLAMA_CTX_WARN_PCT);
    json = fixture_read("tests/fixtures/ollama_reply_near.json", &len);
    CHECK_EQ(judge_ollama_window(json, len, 3400 * 35 / 10, NULL), JUDGE_WINDOW_NEAR);
    free(json);
}

/*
 * No counts must never read as truncated. The drop is load-bearing, so an
 * Ollama that stopped reporting prompt_eval_count would otherwise throw away
 * every batch and quietly empty the board.
 */
static void test_window_without_counts_is_unknown(void)
{
    char *json;
    size_t len;
    long used = -1;

    json = fixture_read("tests/fixtures/ollama_reply_no_counts.json", &len);
    CHECK_EQ(judge_ollama_window(json, len, 1000000, &used), JUDGE_WINDOW_UNKNOWN);
    CHECK_EQ(used, 0);
    free(json);

    CHECK_EQ(judge_ollama_window(NULL, 0, 1000, NULL), JUDGE_WINDOW_UNKNOWN);
    CHECK_EQ(judge_ollama_window("not json", 8, 1000, NULL), JUDGE_WINDOW_UNKNOWN);
}

static void test_extract_ollama_envelope(void)
{
    issue_t is[3];
    const char *verdicts = NULL;
    size_t vlen = 0, len;
    char *json;

    json = fixture_read("tests/fixtures/ollama_reply.json", &len);
    CHECK_EQ(judge_extract_ollama_verdicts(&g_arena, json, len, &verdicts, &vlen), 0);
    CHECK(verdicts != NULL);
    if (verdicts != NULL) {
        CHECK_EQ(verdicts[0], '[');
        issues_reset(is, 3);
        CHECK_EQ(judge_parse_verdicts(&g_arena, verdicts, vlen, is, 3), 3);
        CHECK_EQ(is[0].llm_score, 8);
        CHECK_EQ(is[1].llm_score, 0);
        CHECK_EQ(is[2].llm_score, 6);
    }
    free(json);

    /* An envelope with no message.content is a dropped batch, not a crash. */
    CHECK(judge_extract_ollama_verdicts(&g_arena, "{\"done\":true}", 13,
                                        &verdicts, &vlen) < 0);
    CHECK(judge_extract_ollama_verdicts(&g_arena, "not json", 8, &verdicts, &vlen) < 0);
    CHECK(judge_extract_ollama_verdicts(&g_arena, NULL, 0, &verdicts, &vlen) < 0);
}

/* Anthropic delivers the verdicts as structured tool_use input. */
static void test_extract_anthropic_envelope(void)
{
    issue_t is[3];
    const char *verdicts = NULL;
    size_t vlen = 0, len;
    char *json;

    json = fixture_read("tests/fixtures/anthropic_reply.json", &len);
    CHECK_EQ(judge_extract_anthropic_verdicts(&g_arena, json, len, &verdicts, &vlen), 0);
    CHECK(verdicts != NULL);
    if (verdicts != NULL) {
        CHECK_EQ(verdicts[0], '[');
        issues_reset(is, 3);
        CHECK_EQ(judge_parse_verdicts(&g_arena, verdicts, vlen, is, 3), 3);
        CHECK_EQ(is[0].llm_score, 8);
        CHECK_STREQ(is[0].why, "CUDA kernel perf regression on sm_90, unassigned");
        CHECK_EQ(is[1].llm_score, 0);
        CHECK_EQ(is[2].llm_score, 6);
    }
    free(json);

    /* A text-only reply (no tool_use) and an API error envelope both drop. */
    {
        const char *text_only =
            "{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}";
        const char *err =
            "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
            "\"message\":\"Overloaded\"}}";

        CHECK(judge_extract_anthropic_verdicts(&g_arena, text_only, strlen(text_only),
                                               &verdicts, &vlen) < 0);
        CHECK(judge_extract_anthropic_verdicts(&g_arena, err, strlen(err),
                                               &verdicts, &vlen) < 0);
    }
}

int main(void)
{
    if (arena_init(&g_arena, ARENA_SIZE) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }

#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID
    TEST_RUN(test_format_schema_pins_the_array_length);
#endif
    TEST_RUN(test_parse_ok);
    TEST_RUN(test_why_is_copied);
    TEST_RUN(test_why_stops_on_a_boundary);
    TEST_RUN(test_unpaid_cannot_reach_the_top_band);
    TEST_RUN(test_a_paid_issue_survives_keep_false);
    TEST_RUN(test_gsoc_is_not_payment);
    TEST_RUN(test_an_abandoned_issue_is_demoted);
    TEST_RUN(test_staleness_never_fires_on_unknown_or_paid);
    TEST_RUN(test_a_bare_dollar_is_not_an_amount);
    TEST_RUN(test_the_cap_only_lowers);
    TEST_RUN(test_parse_ignores_model_index);
    TEST_RUN(test_parse_malformed);
    TEST_RUN(test_truncate_short);
    TEST_RUN(test_truncate_long);
    TEST_RUN(test_truncate_utf8_boundary);
    TEST_RUN(test_apply_compacts_and_sorts);
    TEST_RUN(test_window_detects_truncation);
    TEST_RUN(test_window_passes_real_batches);
    TEST_RUN(test_window_warns_near_the_edge);
    TEST_RUN(test_window_without_counts_is_unknown);
    TEST_RUN(test_extract_ollama_envelope);
    TEST_RUN(test_extract_anthropic_envelope);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
