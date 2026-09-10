/*
 * gist.c: the JSON body's escaping, the placeholder guard, the rejection of an
 * empty board, and the invariant that --dry-run issues no HTTP request.
 *
 * No network: every gist_publish() call here runs with dry_run=1, which returns
 * before touching http.h. gist_http_calls is asserted to prove it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tests/test_util.h"

#include "yyjson.h"

#include "core/arena.h"
#include "config.h"
#include "net/gist.h"

/* Both are exported by gist.c and deliberately absent from gist.h. */
extern unsigned long gist_http_calls;
extern const char *gist_build_body(arena_t *a, const char *markdown, size_t *len_out);

static arena_t g_arena;

/*
 * Everything a rendered board can carry that naive escaping gets wrong: a
 * literal double quote, a backslash, a backslash-n that is text rather than a
 * newline, a real newline, a tab, a control byte, emoji, CJK, and a Markdown
 * table row whose URL has query separators in it.
 */
static const char ADVERSARIAL_MD[] =
    "# issuewatch \xe2\x80\x94 3 open \xc2\xb7 updated 2026-09-10 14:02 UTC\n"
    "\n"
    "| # | score | age | repo | issue |\n"
    "|---|-------|-----|------|-------|\n"
    "| 1 | 9 \xf0\x9f\x9f\xa2 | 2h | tt-metal | [bf16 \"matmul\" NaN](https://x/y?a=1&b=2) |\n"
    "| 2 | 8 \xf0\x9f\x9f\xa1 | 5h | pytorch | [C:\\path\\not\\newline \\n here](https://z) |\n"
    "| 3 | 7 | 1d | llama.cpp | [\xe6\xa0\xb8\xe5\xbf\x83\xe6\xba\xa2\xe5\x87\xba \xf0\x9f\x90\x9b](https://q) |\n"
    "\ttab-indented trailer with a \x01 control byte\n";

/* Pulls files/<GIST_FILENAME>/content out of a built body, or NULL. */
static const char *content_of(yyjson_doc *doc)
{
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *files = yyjson_obj_get(root, "files");
    yyjson_val *file = yyjson_obj_get(files, GIST_FILENAME);

    return yyjson_get_str(yyjson_obj_get(file, "content"));
}

/* ------------------------------------------------------------------ tests */

/*
 * The reason this module uses yyjson at all: the board is Markdown full of
 * quotes, backslashes and multi-byte codepoints, and a round trip is the only
 * honest proof that none of it was mangled on the way out.
 */
static void test_body_escapes_and_round_trips(void)
{
    const char *body;
    size_t body_len = 0;
    yyjson_doc *doc;
    const char *content;

    body = gist_build_body(&g_arena, ADVERSARIAL_MD, &body_len);
    CHECK(body != NULL);
    if (body == NULL)
        return;

    /* Length is the string length, and the body is a NUL-terminated object. */
    CHECK_EQ(body_len, strlen(body));
    CHECK_EQ(body[0], '{');

    /* A raw newline or tab in the body would mean the escaping never happened. */
    CHECK(strchr(body, '\n') == NULL);
    CHECK(strchr(body, '\t') == NULL);

    doc = yyjson_read(body, body_len, 0);
    CHECK(doc != NULL);
    if (doc == NULL)
        return;

    content = content_of(doc);
    CHECK(content != NULL);
    if (content != NULL) {
        /* The whole point: what comes back is byte-for-byte what went in. */
        CHECK_STREQ(content, ADVERSARIAL_MD);
        CHECK_EQ(strlen(content), sizeof ADVERSARIAL_MD - 1);
    }

    yyjson_doc_free(doc);
}

/* The shape GitHub's PATCH /gists/{id} expects, and nothing else. */
static void test_body_shape(void)
{
    const char *body;
    size_t body_len = 0;
    yyjson_doc *doc;
    yyjson_val *root, *files, *file;

    body = gist_build_body(&g_arena, "# board\n", &body_len);
    CHECK(body != NULL);
    if (body == NULL)
        return;

    doc = yyjson_read(body, body_len, 0);
    CHECK(doc != NULL);
    if (doc == NULL)
        return;

    root = yyjson_doc_get_root(doc);
    CHECK(yyjson_is_obj(root));
    CHECK_EQ(yyjson_obj_size(root), 1);

    files = yyjson_obj_get(root, "files");
    CHECK(yyjson_is_obj(files));
    CHECK_EQ(yyjson_obj_size(files), 1);

    /* Keyed by the configured filename -- a wrong key silently creates a second
     * file in the gist instead of updating the board. */
    file = yyjson_obj_get(files, GIST_FILENAME);
    CHECK(yyjson_is_obj(file));
    CHECK_EQ(yyjson_obj_size(file), 1);
    CHECK_STREQ(yyjson_get_str(yyjson_obj_get(file, "content")), "# board\n");

    yyjson_doc_free(doc);
}

/* Degenerate inputs must not crash, and must not produce a body. */
static void test_build_rejects_bad_args(void)
{
    size_t len = 12345;

    CHECK(gist_build_body(&g_arena, NULL, &len) == NULL);
    CHECK_EQ(len, 0);                       /* the out-param is cleared, not stale */
    CHECK(gist_build_body(NULL, "# board\n", &len) == NULL);
    /* An empty markdown still builds a valid document; gist_publish() is the
     * layer that refuses to send it. */
    CHECK(gist_build_body(&g_arena, "", NULL) != NULL);
}

/*
 * The invariant CLAUDE.md and the plan both demand: --dry-run must issue zero
 * HTTP requests. Asserted through the http_perform_one() counter in gist.c, not
 * by reading the code.
 */
static void test_dry_run_publishes_nothing(void)
{
    unsigned long before = gist_http_calls;

    CHECK_EQ(gist_publish(&g_arena, "# board\n\n| 1 | 9 | 2h | tt-metal | x |\n", 1), 0);
    CHECK_EQ(gist_http_calls, before);

    /* Repeatable, and an adversarial board takes the same branch. */
    CHECK_EQ(gist_publish(&g_arena, ADVERSARIAL_MD, 1), 0);
    CHECK_EQ(gist_http_calls, before);
}

/*
 * An empty or missing board is a render failure, not "nothing to report":
 * publishing it would blank a gist that still holds unclaimed bounties. Rejected
 * in both modes, and never counted as an HTTP attempt.
 */
static void test_publish_rejects_empty_board(void)
{
    unsigned long before = gist_http_calls;

    CHECK(gist_publish(&g_arena, NULL, 1) < 0);
    CHECK(gist_publish(&g_arena, "", 1) < 0);
    CHECK(gist_publish(NULL, "# board\n", 1) < 0);
    /* dry_run=0 too: the argument check precedes every network decision, so this
     * still reaches no socket. */
    CHECK(gist_publish(&g_arena, NULL, 0) < 0);
    CHECK(gist_publish(&g_arena, "", 0) < 0);

    CHECK_EQ(gist_http_calls, before);
}

/*
 * The shipped config.h carries the placeholder id, so gist_init() must refuse:
 * PATCHing it would 404 and read as a missing token scope. This is also why no
 * test here can reach the real publish path -- main() blocks a non-dry run at
 * init.
 */
static void test_init_rejects_placeholder_id(void)
{
    if (strcmp(GIST_ID, "REPLACE_ME_WITH_GIST_ID") == 0)
        CHECK(gist_init() < 0);
    else
        CHECK_EQ(gist_init(), 0);
}

int main(void)
{
    if (arena_init(&g_arena, ARENA_SIZE) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }

    TEST_RUN(test_body_escapes_and_round_trips);
    TEST_RUN(test_body_shape);
    TEST_RUN(test_build_rejects_bad_args);
    TEST_RUN(test_dry_run_publishes_nothing);
    TEST_RUN(test_publish_rejects_empty_board);
    TEST_RUN(test_init_rejects_placeholder_id);

    /* Belt and braces: nothing in this binary may have issued a request. */
    CHECK_EQ(gist_http_calls, 0);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
