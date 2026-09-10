/*
 * github.c parse tests. Fixtures only -- no network, per CLAUDE.md. Run from
 * the repo root; fixture paths are relative to it.
 */

#include <stdlib.h>
#include <string.h>

#include "../tests/test_util.h"

#include "core/arena.h"
#include "core/board.h"
#include "config.h"
#include "net/github.h"
#include "net/http.h"

#define TEST_ARENA (2u << 20)

/*
 * The re-check drop table, deliberately absent from github.h. gh_recheck() as a
 * whole needs a network and cannot run here, so the decision it makes per
 * response is factored out and driven directly against recorded payloads --
 * same precedent as notify_build_title() in notify.c.
 */
extern int gh_recheck_apply(const http_resp_t *r, board_entry_t *e);

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

/*
 * Assignment is what lets the board drop a bounty somebody else already holds,
 * so every shape GitHub uses to say "nobody" has to parse as unassigned rather
 * than as a crash or a false positive.
 */
static void test_assignment_is_parsed(void)
{
    arena_t a;
    issue_t out[8];
    char nu[32];
    char *json;
    size_t len;
    int n;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    json = fixture_read("tests/fixtures/issues_assignees.json", &len);
    poison(out, 8);

    n = gh_parse_issues(&a, json, len, "tenstorrent/tt-metal", out, 8, nu, sizeof nu);

    /* 7 objects in, the last a pull request -- assigned, and still dropped. */
    CHECK_EQ(n, 6);
    CHECK_STREQ(nu, "2026-09-10T14:00:00Z");

    /* One entry in `assignees`. */
    CHECK_EQ(out[0].id, 5001);
    CHECK_EQ(out[0].assigned, 1);

    /* The everyday unassigned payload: "assignee": null and "assignees": []. */
    CHECK_EQ(out[1].id, 5002);
    CHECK_EQ(out[1].assigned, 0);

    /* Explicit null with no array at all. */
    CHECK_EQ(out[2].id, 5003);
    CHECK_EQ(out[2].assigned, 0);

    /* No array, so the legacy single-assignee field decides. */
    CHECK_EQ(out[3].id, 5004);
    CHECK_EQ(out[3].assigned, 1);

    /* More than one holder; the first hit is enough. */
    CHECK_EQ(out[4].id, 5005);
    CHECK_EQ(out[4].assigned, 1);

    /* Both keys missing entirely. */
    CHECK_EQ(out[5].id, 5006);
    CHECK_EQ(out[5].assigned, 0);

    CHECK(is_poisoned(&out[6]));
    free(json);

    /* The pre-existing fixtures carry no assignment keys: all unassigned. */
    json = fixture_read("tests/fixtures/issues_basic.json", &len);
    CHECK_EQ(gh_parse_issues(&a, json, len, "r/r", out, 8, nu, sizeof nu), 3);
    CHECK_EQ(out[0].assigned, 0);
    CHECK_EQ(out[1].assigned, 0);
    CHECK_EQ(out[2].assigned, 0);
    free(json);

    arena_destroy(&a);
}

/*
 * Wrong types on the assignment keys. GitHub does not emit these, but the
 * parser reads untrusted network input and none of them may fault. A junk value
 * must read as unassigned: a false "assigned" silently throws away an open
 * bounty, whereas a false "unassigned" costs one re-check next cycle.
 */
static void test_assignment_wrong_types(void)
{
    static const struct {
        const char *frag;
        int want;
    } cases[] = {
        { "\"assignees\":{}",                                  0 },
        { "\"assignees\":\"nobody\"",                          0 },
        { "\"assignees\":7",                                   0 },
        { "\"assignees\":null",                                0 },
        { "\"assignees\":[null,7,\"x\",[]]",                   0 },
        { "\"assignees\":[{}]",                                0 },
        { "\"assignees\":[{\"login\":42}]",                    0 },
        { "\"assignees\":[null,{\"login\":\"z\"}]",            1 },
        { "\"assignee\":\"kai\"",                              0 },
        { "\"assignee\":[]",                                   0 },
        { "\"assignee\":{}",                                   0 },
        { "\"assignee\":{\"id\":7}",                           0 },
        /* Not a shape GitHub produces; the array is authoritative regardless. */
        { "\"assignees\":[],\"assignee\":{\"login\":\"kai\"}", 0 },
        { "\"assignees\":null,\"assignee\":{\"login\":\"k\"}", 1 },
    };
    const size_t n_cases = sizeof cases / sizeof cases[0];
    arena_t a;
    issue_t out[2];
    char nu[32];
    size_t i;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);

    for (i = 0; i < n_cases; i++) {
        char json[512];
        int jlen;

        jlen = snprintf(json, sizeof json,
                        "[{\"id\":%zu,\"number\":1,\"title\":\"t\",\"html_url\":\"u\","
                        "\"updated_at\":\"2026-01-01T00:00:00Z\",%s}]",
                        i + 1, cases[i].frag);
        CHECK(jlen > 0 && (size_t)jlen < sizeof json);

        poison(out, 2);
        CHECK_EQ(gh_parse_issues(&a, json, (size_t)jlen, "r/r", out, 2, nu, sizeof nu), 1);
        CHECK_EQ(out[0].assigned, cases[i].want);
        CHECK(is_poisoned(&out[1]));
    }

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

/* ------------------------------------------------------- board conversion */

/* A board entry as gh_recheck() would find it: on the board, not yet refreshed. */
static void seed_entry(board_entry_t *e)
{
    memset(e, 0, sizeof *e);
    e->id = 7001;
    e->number = 4242;
    e->llm_score = 8;
    e->kw_score = 14;
    memcpy(e->repo, "tenstorrent/tt-metal", sizeof "tenstorrent/tt-metal");
    memcpy(e->first_seen, "2026-09-08T06:00:00Z", sizeof "2026-09-08T06:00:00Z");
    memcpy(e->updated_at, "2026-09-09T10:00:00Z", sizeof "2026-09-09T10:00:00Z");
    memcpy(e->etag, "W/\"deadbeef\"", sizeof "W/\"deadbeef\"");
    memcpy(e->title, "bf16 matmul NaN", sizeof "bf16 matmul NaN");
    memcpy(e->why, "unclaimed bounty", sizeof "unclaimed bounty");
    memcpy(e->html_url, "https://github.com/tenstorrent/tt-metal/issues/4242",
           sizeof "https://github.com/tenstorrent/tt-metal/issues/4242");
}

static void test_issue_to_board_carries_fields(void)
{
    board_entry_t e;
    issue_t is;

    memset(&is, 0, sizeof is);
    is.id = 9100;
    is.number = 77;
    is.repo = "ggml-org/llama.cpp";
    is.title = "CUDA kernel regression";
    is.html_url = "https://github.com/ggml-org/llama.cpp/issues/77";
    is.updated_at = "2026-09-10T08:00:00Z";
    is.kw_score = 12;
    is.llm_score = 7;
    is.why = "open perf bug with a repro";
    is.assigned = 1;

    memset(&e, 0x5a, sizeof e);
    gh_issue_to_board(&is, &e);

    CHECK_EQ(e.id, 9100);
    CHECK_EQ(e.number, 77);
    CHECK_EQ(e.kw_score, 12);
    CHECK_EQ(e.llm_score, 7);
    CHECK_EQ(e.assigned, 1);
    CHECK_STREQ(e.repo, "ggml-org/llama.cpp");
    CHECK_STREQ(e.title, "CUDA kernel regression");
    CHECK_STREQ(e.html_url, "https://github.com/ggml-org/llama.cpp/issues/77");
    CHECK_STREQ(e.updated_at, "2026-09-10T08:00:00Z");
    CHECK_STREQ(e.why, "open perf bug with a repro");

    /* board_merge() owns first_seen, and the ETag only exists once a re-check
     * has asked for the issue on its own. Both must be left empty here. */
    CHECK_STREQ(e.first_seen, "");
    CHECK_STREQ(e.etag, "");
    CHECK_EQ(e.fresh, 0);

    /* A NULL issue zeroes rather than scribbles, and NULL out must not fault. */
    memset(&e, 0x5a, sizeof e);
    gh_issue_to_board(NULL, &e);
    CHECK_EQ(e.id, 0);
    CHECK_STREQ(e.title, "");
    gh_issue_to_board(&is, NULL);

    /* A judge that never ran leaves why NULL; that must land as "", not a crash. */
    is.why = NULL;
    gh_issue_to_board(&is, &e);
    CHECK_STREQ(e.why, "");
}

/*
 * The bug this test exists for: a plain snprintf cut lands mid-character on a
 * CJK or emoji title, gist.c hands the clipped byte to yyjson, yyjson refuses to
 * encode invalid UTF-8, and one long title costs the whole cycle's publish.
 */
static void test_issue_to_board_truncates_on_utf8_boundaries(void)
{
    /* U+30AB (3 bytes), U+1F525 (4 bytes), U+00E9 (2 bytes). */
    static const char *const chars[3] = { "\xe3\x82\xab", "\xf0\x9f\x94\xa5", "\xc3\xa9" };
    static const int counts[3] = { 100, 80, 200 };
    board_entry_t e;
    issue_t is;
    size_t ci;

    for (ci = 0; ci < 3; ci++) {
        size_t clen = strlen(chars[ci]);
        size_t n = (size_t)counts[ci];
        char *big = (char *)malloc(n * clen + 1);
        size_t k, len;

        CHECK(big != NULL);
        if (big == NULL)
            exit(2);
        for (k = 0; k < n; k++)
            memcpy(big + k * clen, chars[ci], clen);
        big[n * clen] = '\0';
        CHECK(n * clen > sizeof e.title);

        memset(&is, 0, sizeof is);
        is.id = 1;
        is.repo = big;              /* every bounded field, not just the title */
        is.title = big;
        is.html_url = big;
        is.why = big;
        is.updated_at = big;

        gh_issue_to_board(&is, &e);

        len = strlen(e.title);
        CHECK(len < sizeof e.title);
        CHECK(utf8_is_valid(e.title, len));
        /* At most one character is sacrificed to reach the boundary. */
        CHECK(len + clen >= sizeof e.title);
        /* Truncation only ever drops a tail: the head is byte-for-byte intact. */
        CHECK_EQ(memcmp(e.title, big, len), 0);
        CHECK_EQ(len % clen, 0);

        CHECK(utf8_is_valid(e.repo, strlen(e.repo)));
        CHECK(utf8_is_valid(e.html_url, strlen(e.html_url)));
        CHECK(utf8_is_valid(e.why, strlen(e.why)));
        CHECK(utf8_is_valid(e.updated_at, strlen(e.updated_at)));
        CHECK(strlen(e.why) < sizeof e.why);
        CHECK(strlen(e.html_url) < sizeof e.html_url);
        CHECK(strlen(e.repo) < sizeof e.repo);

        free(big);
    }
}

/* ------------------------------------------------------ the re-check table */

/* One recorded response. `body` stays owned by the caller. */
static void resp_from(http_resp_t *r, long status, char *body, size_t len,
                      const char *etag)
{
    memset(r, 0, sizeof *r);
    r->status = status;
    r->body = body;
    r->body_len = len;
    r->retry_after = -1;
    r->rl_remaining = -1;
    r->rl_reset = -1;
    if (etag != NULL)
        memcpy(r->etag, etag, strlen(etag) + 1);
}

/* Every keep path must leave the entry byte-for-byte as it was. */
static int entry_unchanged(const board_entry_t *e)
{
    board_entry_t ref;

    seed_entry(&ref);
    return memcmp(e, &ref, sizeof ref) == 0;
}

static void test_recheck_keeps_on_304(void)
{
    board_entry_t e;
    http_resp_t r;

    seed_entry(&e);
    /* A 304 carries no body at all, and costs no rate-limit unit. */
    resp_from(&r, 304, NULL, 0, "W/\"deadbeef\"");
    CHECK_EQ(gh_recheck_apply(&r, &e), 0);
    CHECK(entry_unchanged(&e));
}

static void test_recheck_refreshes_on_200_open(void)
{
    board_entry_t e;
    http_resp_t r;
    char *json;
    size_t len, tlen;

    json = fixture_read("tests/fixtures/issue_one_open.json", &len);
    seed_entry(&e);
    resp_from(&r, 200, json, len, "W/\"cafef00d\"");

    CHECK_EQ(gh_recheck_apply(&r, &e), 0);

    CHECK_STREQ(e.updated_at, "2026-09-10T11:30:00Z");
    CHECK_STREQ(e.etag, "W/\"cafef00d\"");
    CHECK_EQ(e.assigned, 0);

    /*
     * A 333-byte emoji-and-CJK title into a 256-byte field. The cut has to land
     * on a character boundary or the next publish is dropped whole.
     */
    tlen = strlen(e.title);
    CHECK(tlen < sizeof e.title);
    CHECK(utf8_is_valid(e.title, tlen));
    CHECK_EQ(memcmp(e.title, "\xf0\x9f\x94\xa5 \xe3\x82\xab", 8), 0);

    /* The judge owns the scores and first_seen is the row's age: untouched. */
    CHECK_EQ(e.llm_score, 8);
    CHECK_EQ(e.kw_score, 14);
    CHECK_STREQ(e.first_seen, "2026-09-08T06:00:00Z");
    CHECK_STREQ(e.repo, "tenstorrent/tt-metal");
    CHECK_EQ(e.number, 4242);

    free(json);
}

static void test_recheck_drops_closed_and_assigned(void)
{
    board_entry_t e;
    http_resp_t r;
    char *json;
    size_t len;

    /* Closed but unassigned -- nobody holds it, and it is still gone. */
    json = fixture_read("tests/fixtures/issue_one_closed.json", &len);
    seed_entry(&e);
    resp_from(&r, 200, json, len, "W/\"newetag\"");
    CHECK_EQ(gh_recheck_apply(&r, &e), 1);
    CHECK_EQ(e.assigned, 0);
    /* The caller turns updated_at into the seen-set key, so the drop path has to
     * hand it the version GitHub just reported, not the stale one. */
    CHECK_STREQ(e.updated_at, "2026-09-10T12:05:00Z");
    /* Nothing else is worth refreshing on a row that is leaving. */
    CHECK_STREQ(e.title, "bf16 matmul NaN");
    CHECK_STREQ(e.etag, "W/\"deadbeef\"");
    free(json);

    /* Open but assigned -- the case the whole board exists to catch. */
    json = fixture_read("tests/fixtures/issue_one_assigned.json", &len);
    seed_entry(&e);
    resp_from(&r, 200, json, len, "W/\"newetag\"");
    CHECK_EQ(gh_recheck_apply(&r, &e), 1);
    CHECK_EQ(e.assigned, 1);
    CHECK_STREQ(e.updated_at, "2026-09-10T13:15:00Z");
    free(json);
}

static void test_recheck_drops_on_404(void)
{
    board_entry_t e;
    http_resp_t r;
    const char *envelope = "{\"message\":\"Not Found\",\"status\":\"404\"}";

    seed_entry(&e);
    resp_from(&r, 404, (char *)envelope, strlen(envelope), NULL);
    CHECK_EQ(gh_recheck_apply(&r, &e), 1);
    /* Deleted or transferred; the error envelope is never parsed for state. */
    CHECK_STREQ(e.updated_at, "2026-09-09T10:00:00Z");
    CHECK_STREQ(e.title, "bf16 matmul NaN");
}

/*
 * The rule that matters most in practice: a laptop that lost its network
 * mid-cycle must not wake up and empty the whole board in one pass. The body
 * here says "closed" precisely to prove it is never consulted on a status 0.
 */
static void test_recheck_keeps_on_transport_failure(void)
{
    board_entry_t e;
    http_resp_t r;
    char *json;
    size_t len;

    json = fixture_read("tests/fixtures/issue_one_closed.json", &len);
    seed_entry(&e);
    resp_from(&r, 0, json, len, NULL);
    r.curl_err = 28;                        /* CURLE_OPERATION_TIMEDOUT */
    CHECK_EQ(gh_recheck_apply(&r, &e), 0);
    CHECK(entry_unchanged(&e));

    /* An empty-bodied timeout is the ordinary shape and must behave the same. */
    seed_entry(&e);
    resp_from(&r, 0, NULL, 0, NULL);
    r.curl_err = 6;                         /* CURLE_COULDNT_RESOLVE_HOST */
    CHECK_EQ(gh_recheck_apply(&r, &e), 0);
    CHECK(entry_unchanged(&e));

    free(json);
}

/* Anything without a clear verdict keeps the row: a wrong keep costs one
 * conditional GET, a wrong drop deletes a live bounty. */
static void test_recheck_keeps_on_other_statuses(void)
{
    static const long statuses[] = { 301, 401, 403, 410, 422, 429, 500, 502, 503 };
    const size_t n = sizeof statuses / sizeof statuses[0];
    board_entry_t e;
    http_resp_t r;
    char *json;
    size_t len, i;

    json = fixture_read("tests/fixtures/issue_one_closed.json", &len);
    for (i = 0; i < n; i++) {
        seed_entry(&e);
        resp_from(&r, statuses[i], json, len, "W/\"newetag\"");
        CHECK_EQ(gh_recheck_apply(&r, &e), 0);
        CHECK(entry_unchanged(&e));
    }
    free(json);
}

/*
 * A 200 whose body we cannot make sense of is not GitHub saying "closed", and a
 * missing or junk `state` reads as open on purpose -- the conservative direction
 * is the one that costs a re-check rather than a bounty.
 */
static void test_recheck_malformed_200_keeps(void)
{
    static const char *const keeps[] = {
        "",                                     /* empty body */
        "}{ not json",
        "[]",                                   /* an array, not an issue object */
        "42",
        "null",
        "{\"state\":7}",                        /* wrong type */
        "{\"state\":null}",
        "{}",                                   /* no state key at all */
    };
    const size_t n = sizeof keeps / sizeof keeps[0];
    board_entry_t e;
    http_resp_t r;
    size_t i;

    for (i = 0; i < n; i++) {
        seed_entry(&e);
        resp_from(&r, 200, (char *)keeps[i], strlen(keeps[i]), NULL);
        CHECK_EQ(gh_recheck_apply(&r, &e), 0);
    }

    /* A body with no state but a real title still refreshes: it is a 200. */
    {
        const char *ok = "{\"title\":\"renamed\",\"updated_at\":\"2026-09-11T00:00:00Z\"}";

        seed_entry(&e);
        resp_from(&r, 200, (char *)ok, strlen(ok), "W/\"fresh\"");
        CHECK_EQ(gh_recheck_apply(&r, &e), 0);
        CHECK_STREQ(e.title, "renamed");
        CHECK_STREQ(e.updated_at, "2026-09-11T00:00:00Z");
        CHECK_STREQ(e.etag, "W/\"fresh\"");
    }

    /* Every state GitHub can send that is not "open" ends the row. */
    {
        const char *closed = "{\"state\":\"closed\"}";

        seed_entry(&e);
        resp_from(&r, 200, (char *)closed, strlen(closed), NULL);
        CHECK_EQ(gh_recheck_apply(&r, &e), 1);
    }

    /* NULL arguments must not be dereferenced, and mean "keep". */
    seed_entry(&e);
    CHECK_EQ(gh_recheck_apply(NULL, &e), 0);
    CHECK(entry_unchanged(&e));
    resp_from(&r, 200, NULL, 0, NULL);
    CHECK_EQ(gh_recheck_apply(&r, NULL), 0);
}

/*
 * gh_recheck() itself needs a network, so only its setup paths run here. Each
 * one has to leave the board exactly as it found it -- refusing to run is not a
 * reason to lose rows.
 */
static void test_recheck_setup_failures(void)
{
    board_entry_t slots[2];
    board_t b;
    state_t st;
    arena_t a;

    CHECK_EQ(arena_init(&a, TEST_ARENA), 0);
    memset(&st, 0, sizeof st);

    seed_entry(&slots[0]);
    seed_entry(&slots[1]);
    slots[1].id = 7002;
    memset(&b, 0, sizeof b);
    b.entries = slots;
    b.n = 2;
    b.cap = 2;

    CHECK(gh_recheck(NULL, &b, &st, 1) < 0);
    CHECK(gh_recheck(&a, NULL, &st, 1) < 0);
    CHECK(gh_recheck(&a, &b, NULL, 1) < 0);
    {
        board_t empty = b;

        empty.entries = NULL;
        CHECK(gh_recheck(&a, &empty, &st, 1) < 0);
    }

    /* An empty board needs no token and issues nothing. */
    {
        board_t none = b;

        none.n = 0;
        CHECK_EQ(gh_recheck(&a, &none, &st, 1), 0);
    }

    /* Everything already refreshed by this cycle's fetch: nothing to ask. */
    slots[0].fresh = 1;
    slots[1].fresh = 1;
    unsetenv("GH_TOKEN");
    CHECK_EQ(gh_recheck(&a, &b, &st, 1), 0);
    CHECK_EQ(b.n, 2);

    /* A stale row with no token is a setup failure, and keeps every entry. */
    slots[0].fresh = 0;
    CHECK(gh_recheck(&a, &b, &st, 1) < 0);
    CHECK_EQ(b.n, 2);
    CHECK_EQ(b.dirty, 0);
    CHECK(entry_unchanged(&slots[0]));

    /* An entry with nothing to build a URL from is kept, not dropped. */
    slots[0].number = 0;
    slots[1].fresh = 0;
    slots[1].repo[0] = '\0';
    CHECK_EQ(gh_recheck(&a, &b, &st, 1), 0);
    CHECK_EQ(b.n, 2);

    arena_destroy(&a);
}

int main(void)
{
    TEST_RUN(test_basic_fields);
    TEST_RUN(test_assignment_is_parsed);
    TEST_RUN(test_assignment_wrong_types);
    TEST_RUN(test_pull_requests_are_filtered);
    TEST_RUN(test_newest_updated_includes_prs);
    TEST_RUN(test_edge_cases);
    TEST_RUN(test_body_cap_utf8_boundaries);
    TEST_RUN(test_out_cap_is_respected);
    TEST_RUN(test_malformed_input);
    TEST_RUN(test_strings_are_arena_copies);
    TEST_RUN(test_token_is_required);
    TEST_RUN(test_issue_to_board_carries_fields);
    TEST_RUN(test_issue_to_board_truncates_on_utf8_boundaries);
    TEST_RUN(test_recheck_keeps_on_304);
    TEST_RUN(test_recheck_refreshes_on_200_open);
    TEST_RUN(test_recheck_drops_closed_and_assigned);
    TEST_RUN(test_recheck_drops_on_404);
    TEST_RUN(test_recheck_keeps_on_transport_failure);
    TEST_RUN(test_recheck_keeps_on_other_statuses);
    TEST_RUN(test_recheck_malformed_200_keeps);
    TEST_RUN(test_recheck_setup_failures);
    TEST_REPORT();
}
