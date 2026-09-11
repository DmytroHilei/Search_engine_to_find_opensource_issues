/*
 * judge.c: verdict parsing, body truncation, compaction/sort, and envelope
 * extraction. Fixtures only -- no network (CLAUDE.md testing rules).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tests/test_util.h"

#include "core/arena.h"
#include "config.h"
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

/* Only built for the Ollama-backed modes; the API path constrains output with a
 * forced tool call instead of a grammar. */
#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID
extern const char *judge_render_format_schema(arena_t *a, size_t n);
#endif

static arena_t g_arena;

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
 * The memory-safety test. i=99, i=-3 and i=2^31 against a heap array of exactly
 * two issues: under ASan any of those writes trips a redzone.
 */
static void test_parse_index_out_of_range(void)
{
    issue_t *is = calloc(2, sizeof *is);
    char *json;
    size_t len;
    int r;

    CHECK(is != NULL);
    if (is == NULL)
        return;
    issues_reset(is, 2);

    json = fixture_read("tests/fixtures/verdicts_bad.json", &len);
    r = judge_parse_verdicts(&g_arena, json, len, is, 2);

    CHECK_EQ(r, 1);                     /* only the i=0 verdict is in range */
    CHECK_EQ(is[0].llm_score, 7);
    CHECK_EQ(is[1].llm_score, -1);      /* untouched */
    CHECK(is[1].why == NULL);

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
    TEST_RUN(test_parse_index_out_of_range);
    TEST_RUN(test_parse_malformed);
    TEST_RUN(test_truncate_short);
    TEST_RUN(test_truncate_long);
    TEST_RUN(test_truncate_utf8_boundary);
    TEST_RUN(test_apply_compacts_and_sorts);
    TEST_RUN(test_extract_ollama_envelope);
    TEST_RUN(test_extract_anthropic_envelope);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
