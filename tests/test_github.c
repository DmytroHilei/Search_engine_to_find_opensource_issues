/*
 * github.c parse tests. Fixtures only -- no network, per CLAUDE.md. Run from
 * the repo root; fixture paths are relative to it.
 */

#include <stdlib.h>
#include <string.h>

#include "../tests/test_util.h"

#include "core/arena.h"
#include "config.h"
#include "net/github.h"

#define TEST_ARENA (2u << 20)

/* 0x5a canary so an out_cap overrun is visible rather than merely wrong. */
static void poison(issue_t *buf, size_t n)
{
    memset(buf, 0x5a, n * sizeof *buf);
}

/*
 * Strict UTF-8 check, written independently of github.c's truncation logic so
 * the two cannot agree on the same mistake. Rejects over-long forms, surrogates
 * and out-of-range codepoints as well as a sequence cut short at the end.
 */
static int utf8_is_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    while (i < len) {
        unsigned char c = p[i];
        size_t need, k;
        unsigned long cp;

        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            cp = c & 0x07u;
        } else {
            return 0;                   /* continuation or invalid lead byte */
        }

        if (i + need >= len)
            return 0;                   /* sequence runs off the end */
        for (k = 1; k <= need; k++) {
            if ((p[i + k] & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (unsigned long)(p[i + k] & 0x3Fu);
        }
        if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000))
            return 0;                   /* over-long encoding */
        if (cp > 0x10FFFFul || (cp >= 0xD800ul && cp <= 0xDFFFul))
            return 0;
        i += need + 1;
    }
    return 1;
}

static int is_poisoned(const issue_t *slot)
{
    const unsigned char *p = (const unsigned char *)slot;
    size_t i;

    for (i = 0; i < sizeof *slot; i++)
        if (p[i] != 0x5a)
            return 0;
    return 1;
}

static void test_basic_fields(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;
    int n;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    json = fixture_read("tests/fixtures/issues_basic.json", &len);
    poison(out, 8);

    n = gh_parse_issues(&a, json, len, "ggml-org/llama.cpp", out, 8, nu, sizeof nu);
    CHECK_EQ(n, 3);

    CHECK_EQ(out[0].id, 2001);
    CHECK_EQ(out[0].number, 512);
    CHECK_EQ(out[0].comments, 7);
    CHECK_STREQ(out[0].repo, "ggml-org/llama.cpp");
    CHECK_STREQ(out[0].title, "CUDA kernel regression in flash attention after 0.4.2");
    CHECK_STREQ(out[0].html_url, "https://github.com/ggml-org/llama.cpp/issues/512");
    CHECK_STREQ(out[0].author, "alice-dev");
    CHECK_STREQ(out[0].updated_at, "2026-09-09T18:22:04Z");
    CHECK(strstr(out[0].body, "1.9 TFLOP/s") != NULL);
    CHECK_EQ(out[0].n_labels, 2);
    CHECK_STREQ(out[0].labels[0], "bug");
    CHECK_STREQ(out[0].labels[1], "performance");

    /* Downstream stages own these; the parser must leave them in a known state. */
    CHECK_EQ(out[0].kw_score, 0);
    CHECK_EQ(out[0].llm_score, -1);
    CHECK(out[0].why == NULL);

    CHECK_EQ(out[1].id, 2002);
    CHECK_STREQ(out[1].author, "bob");
    CHECK_EQ(out[1].comments, 0);
    CHECK_EQ(out[1].n_labels, 1);

    CHECK_EQ(out[2].id, 2003);
    CHECK_EQ(out[2].number, 508);
    CHECK_EQ(out[2].n_labels, 0);           /* empty labels array */
    CHECK_STREQ(out[2].updated_at, "2026-09-08T07:41:59Z");

    CHECK(is_poisoned(&out[3]));

    free(json);
    arena_destroy(&a);
}

static void test_pull_requests_are_filtered(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;
    int n, i;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    json = fixture_read("tests/fixtures/issues_with_prs.json", &len);
    poison(out, 8);

    n = gh_parse_issues(&a, json, len, "NVIDIA/cutlass", out, 8, nu, sizeof nu);

    /* 5 objects in, 2 of them pull requests: one a real object, one JSON null. */
    CHECK_EQ(n, 3);
    for (i = 0; i < n && i < 8; i++) {
        CHECK(out[i].id != 3101);           /* "pull_request": {...} */
        CHECK(out[i].id != 3103);           /* "pull_request": null  */
        CHECK(strstr(out[i].html_url, "/pull/") == NULL);
    }
    CHECK_EQ(out[0].id, 3102);
    CHECK_EQ(out[1].id, 3104);
    CHECK_EQ(out[2].id, 3105);
    CHECK(is_poisoned(&out[3]));

    free(json);
    arena_destroy(&a);
}

static void test_newest_updated_includes_prs(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);

    json = fixture_read("tests/fixtures/issues_with_prs.json", &len);
    CHECK_EQ(gh_parse_issues(&a, json, len, "NVIDIA/cutlass", out, 8, nu, sizeof nu), 3);
    /*
     * The newest object in the payload is a pull request. It must still set the
     * watermark, or a PR-only repo refetches the same page every cycle.
     */
    CHECK_STREQ(nu, "2026-09-10T09:00:00Z");
    CHECK(strcmp(nu, out[0].updated_at) > 0);
    free(json);

    json = fixture_read("tests/fixtures/issues_basic.json", &len);
    CHECK_EQ(gh_parse_issues(&a, json, len, "ggml-org/llama.cpp", out, 8, nu, sizeof nu), 3);
    CHECK_STREQ(nu, "2026-09-09T18:22:04Z");
    free(json);

    /* A buffer too small for a timestamp must be left empty, not overflowed. */
    json = fixture_read("tests/fixtures/issues_basic.json", &len);
    {
        char tiny[4] = { 'x', 'x', 'x', 'x' };
        CHECK_EQ(gh_parse_issues(&a, json, len, "r/r", out, 8, tiny, sizeof tiny), 3);
        CHECK_STREQ(tiny, "");
    }
    free(json);

    arena_destroy(&a);
}

static void test_edge_cases(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;
    int n;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    json = fixture_read("tests/fixtures/issues_edge.json", &len);

    n = gh_parse_issues(&a, json, len, "triton-lang/triton", out, 8, nu, sizeof nu);
    CHECK_EQ(n, 4);
    CHECK_STREQ(nu, "2026-09-10T12:00:00Z");

    /* "body": null normalises to "", never NULL -- the prefilter scans it. */
    CHECK_EQ(out[0].id, 4001);
    CHECK(out[0].body != NULL);
    CHECK_STREQ(out[0].body, "");

    /* Emoji and CJK survive byte-for-byte; sanitising is notify.c's job. */
    CHECK_STREQ(out[0].title,
                "\U0001F525 CUDA \u30AB\u30FC\u30CD\u30EB segfault on sm_90 "
                "\u2014 \u4E2D\u6587\u6D4B\u8BD5");

    /* 20 labels in the fixture, capped at GH_MAX_LABELS. */
    CHECK_EQ(out[0].n_labels, GH_MAX_LABELS);
    CHECK_STREQ(out[0].labels[0], "cuda");
    CHECK_STREQ(out[0].labels[1], "performance");
    CHECK_STREQ(out[0].labels[GH_MAX_LABELS - 1], "label-16");

    /* Quotes and backslashes come out unescaped. */
    CHECK_EQ(out[1].id, 4002);
    CHECK_STREQ(out[1].title, "He said \"use \\n\" and the parser exploded");
    CHECK_EQ(out[1].n_labels, 0);

    /*
     * 2880 bytes is far past LLM_BODY_TRUNC but under GH_BODY_MAX, so it is
     * kept whole: the prefilter scans it, and judge.c truncates for the prompt.
     */
    CHECK_EQ(out[2].id, 4003);
    CHECK_EQ((long long)strlen(out[2].body), 2880);
    CHECK_STREQ(out[2].body + 2880 - 14, "kernel_launch\n");

    /*
     * 9007 bytes, with GH_BODY_MAX landing on the 2nd byte of a 3-byte U+2603.
     * The cut walks back to the character's lead byte: 7 ASCII + 2728 complete
     * snowmen = 8191 bytes kept.
     */
    CHECK_EQ(out[3].id, 4004);
    CHECK(strlen(out[3].body) <= (size_t)GH_BODY_MAX);
    CHECK_EQ((long long)strlen(out[3].body), 8191);
    CHECK(utf8_is_valid(out[3].body, strlen(out[3].body)));
    CHECK_EQ(memcmp(out[3].body, "PANIC: \xe2\x98\x83\xe2\x98\x83", 13), 0);
    /* The kept tail is a whole character, not two thirds of one. */
    CHECK_EQ(memcmp(out[3].body + 8191 - 3, "\xe2\x98\x83", 3), 0);

    free(json);
    arena_destroy(&a);
}

/*
 * The cap has to land cleanly whichever byte of a multi-byte character it falls
 * on, so drive every alignment for a 3-byte and a 4-byte codepoint.
 */
static void test_body_cap_utf8_boundaries(void)
{
    const char *chars[2] = { "\xe2\x98\x83", "\xf0\x9f\x94\xa5" };  /* U+2603, U+1F525 */
    arena_t a;
    issue_t out[2];
    char nu[32];
    size_t ci, pad;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);

    for (ci = 0; ci < 2; ci++) {
        size_t clen = strlen(chars[ci]);

        for (pad = 0; pad < 4; pad++) {
            size_t want = (size_t)GH_BODY_MAX + 64;
            size_t blen = 0, jlen = 0, cap = want + 512;
            char *body = (char *)malloc(cap);
            char *json = (char *)malloc(cap + 256);
            int n;

            CHECK(body != NULL && json != NULL);
            if (body == NULL || json == NULL)
                exit(2);

            memset(body, 'x', pad);
            blen = pad;
            while (blen + clen <= want) {
                memcpy(body + blen, chars[ci], clen);
                blen += clen;
            }
            body[blen] = '\0';
            CHECK(blen > (size_t)GH_BODY_MAX);

            /* No JSON metacharacters in the body, so no escaping is needed. */
            jlen = (size_t)snprintf(json, cap + 256,
                                    "[{\"id\":7,\"number\":7,\"title\":\"t\","
                                    "\"html_url\":\"u\",\"updated_at\":"
                                    "\"2026-05-05T05:05:05Z\",\"body\":\"%s\"}]",
                                    body);

            n = gh_parse_issues(&a, json, jlen, "o/r", out, 2, nu, sizeof nu);
            CHECK_EQ(n, 1);
            if (n == 1) {
                size_t kept = strlen(out[0].body);

                CHECK(kept <= (size_t)GH_BODY_MAX);
                /* At most one character is sacrificed to reach the boundary. */
                CHECK(kept + clen > (size_t)GH_BODY_MAX);
                CHECK(utf8_is_valid(out[0].body, kept));
                /* Head preserved byte-for-byte -- truncation only drops a tail. */
                CHECK_EQ(memcmp(out[0].body, body, kept), 0);
            }

            free(body);
            free(json);
        }
    }

    arena_destroy(&a);
}

static void test_out_cap_is_respected(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    json = fixture_read("tests/fixtures/issues_basic.json", &len);

    poison(out, 8);
    CHECK_EQ(gh_parse_issues(&a, json, len, "r/r", out, 2, nu, sizeof nu), 2);
    CHECK_EQ(out[0].id, 2001);
    CHECK_EQ(out[1].id, 2002);
    CHECK(is_poisoned(&out[2]));

    poison(out, 8);
    CHECK_EQ(gh_parse_issues(&a, json, len, "r/r", out, 0, nu, sizeof nu), 0);
    CHECK(is_poisoned(&out[0]));
    /*
     * Nothing was stored, so nothing may be claimed as processed: an advanced
     * watermark here would skip all three issues forever (CLAUDE.md rule 4).
     */
    CHECK_STREQ(nu, "");

    free(json);
    arena_destroy(&a);
}

static void test_malformed_input(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;
    int n;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);

    /* Not JSON at all. */
    CHECK(gh_parse_issues(&a, "}{ not json", 11, "r/r", out, 8, nu, sizeof nu) < 0);
    /* Empty payload. */
    CHECK(gh_parse_issues(&a, "", 0, "r/r", out, 8, nu, sizeof nu) < 0);
    /* GitHub's error envelope is a top-level object, not an array. */
    {
        const char *env = "{\"message\":\"Not Found\",\"status\":\"404\"}";
        CHECK(gh_parse_issues(&a, env, strlen(env), "r/r", out, 8, nu, sizeof nu) < 0);
    }
    /* A JSON scalar at the root. */
    CHECK(gh_parse_issues(&a, "42", 2, "r/r", out, 8, nu, sizeof nu) < 0);
    /* NULL payload must not be dereferenced. */
    CHECK(gh_parse_issues(&a, NULL, 0, "r/r", out, 8, nu, sizeof nu) < 0);

    /* An empty array is valid and yields nothing. */
    CHECK_EQ(gh_parse_issues(&a, "[]", 2, "r/r", out, 8, nu, sizeof nu), 0);
    CHECK_STREQ(nu, "");

    /* Junk elements and items missing required fields are skipped, not fatal. */
    {
        const char *mixed =
            "[123, null, \"str\", [], "
            "{\"id\":1,\"number\":2,\"updated_at\":\"2026-01-01T00:00:00Z\"},"
            "{\"id\":9,\"number\":9,\"title\":\"ok\",\"html_url\":\"u\","
            "\"updated_at\":\"2026-01-02T00:00:00Z\",\"labels\":[1,{},{\"name\":7},"
            "{\"name\":\"real\"}],\"user\":42,\"comments\":\"lots\"}]";
        poison(out, 8);
        n = gh_parse_issues(&a, mixed, strlen(mixed), "r/r", out, 8, nu, sizeof nu);
        CHECK_EQ(n, 1);
        CHECK_EQ(out[0].id, 9);
        CHECK_STREQ(out[0].author, "");     /* "user" is not an object */
        CHECK_EQ(out[0].comments, 0);       /* "comments" is not a number */
        CHECK_EQ(out[0].n_labels, 1);       /* only the well-formed label */
        CHECK_STREQ(out[0].labels[0], "real");
        /* The field-less item never counted towards the watermark. */
        CHECK_STREQ(nu, "2026-01-02T00:00:00Z");
    }

    /* Truncated transfer: half a valid fixture. */
    json = fixture_read("tests/fixtures/issues_with_prs.json", &len);
    CHECK(gh_parse_issues(&a, json, len / 2, "r/r", out, 8, nu, sizeof nu) < 0);
    CHECK(gh_parse_issues(&a, json, 1, "r/r", out, 8, nu, sizeof nu) < 0);
    /* len - 2 drops the closing bracket: the array never terminates. */
    CHECK(gh_parse_issues(&a, json, len - 2, "r/r", out, 8, nu, sizeof nu) < 0);
    free(json);

    arena_destroy(&a);
}

static void test_strings_are_arena_copies(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;
    int n;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    json = fixture_read("tests/fixtures/issues_edge.json", &len);

    n = gh_parse_issues(&a, json, len, "triton-lang/triton", out, 8, nu, sizeof nu);
    CHECK_EQ(n, 4);

    /*
     * The yyjson doc is freed before gh_parse_issues() returns, and the caller
     * is free to reuse the response buffer. Scribble over it and free it: a
     * borrowed pointer shows up here as wrong content, or as a use-after-free
     * under ASan.
     */
    memset(json, 'Z', len);
    free(json);

    CHECK_STREQ(out[0].repo, "triton-lang/triton");
    CHECK_STREQ(out[0].body, "");
    CHECK_STREQ(out[0].labels[0], "cuda");
    CHECK_STREQ(out[0].labels[GH_MAX_LABELS - 1], "label-16");
    CHECK_STREQ(out[0].updated_at, "2026-09-10T12:00:00Z");
    CHECK_STREQ(out[1].title, "He said \"use \\n\" and the parser exploded");
    CHECK_STREQ(out[1].html_url, "https://github.com/triton-lang/triton/issues/11");
    CHECK_STREQ(out[2].author, "judy");
    CHECK_EQ((long long)strlen(out[2].body), 2880);

    arena_destroy(&a);
}

/*
 * No network in the tests, so gh_fetch_all() is only exercised on the path that
 * refuses to run at all. state_t is a plain struct, so it can be built by hand
 * without touching the state files.
 */
static void test_token_is_required(void)
{
    arena_t a;
    repo_state_t rs;
    state_t st;
    issue_t *issues = (issue_t *)0x1;
    size_t n = 99;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    memset(&rs, 0, sizeof rs);
    memcpy(rs.repo, "ggml-org/llama.cpp", sizeof "ggml-org/llama.cpp");
    memcpy(rs.watermark, "2026-09-01T00:00:00Z", sizeof "2026-09-01T00:00:00Z");
    memset(&st, 0, sizeof st);
    st.repos = &rs;
    st.n_repos = 1;

    {
        const char *repos[1];
        repos[0] = rs.repo;
        unsetenv("GH_TOKEN");
        CHECK(gh_fetch_all(&a, &st, repos, 1, &issues, &n) < 0);
        CHECK(issues == NULL);
        CHECK_EQ(n, 0);
    }

    /* The refusal must not have touched the repo's state. */
    CHECK_STREQ(rs.watermark, "2026-09-01T00:00:00Z");
    CHECK_EQ(rs.dirty, 0);

    /*
     * CLAUDE.md rule 4: committing is only ever legal after a fetch staged
     * something. With nothing staged this is a no-op -- in particular it must
     * not mark the repo dirty and write a bogus watermark to disk.
     */
    gh_commit_watermarks(&st);
    CHECK_STREQ(rs.watermark, "2026-09-01T00:00:00Z");
    CHECK_STREQ(rs.etag, "");
    CHECK_EQ(rs.dirty, 0);

    gh_commit_watermarks(NULL);              /* must not crash */

    arena_destroy(&a);
}

int main(void)
{
    TEST_RUN(test_basic_fields);
    TEST_RUN(test_pull_requests_are_filtered);
    TEST_RUN(test_newest_updated_includes_prs);
    TEST_RUN(test_edge_cases);
    TEST_RUN(test_body_cap_utf8_boundaries);
    TEST_RUN(test_out_cap_is_respected);
    TEST_RUN(test_malformed_input);
    TEST_RUN(test_strings_are_arena_copies);
    TEST_RUN(test_token_is_required);
    TEST_REPORT();
}
