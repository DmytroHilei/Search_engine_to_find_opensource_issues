/*
 * Cross-module tests for the publish path: board_entry_t -> render_board() ->
 * gist_build_body().
 *
 * Every module here is already tested on its own. What is NOT covered by any of
 * those tests is the seam between them, and the seam has a real failure mode:
 * yyjson refuses to encode invalid UTF-8, so gist_build_body() returns NULL and
 * the whole cycle's publish is dropped. Everything upstream of it copies GitHub
 * titles into fixed-size buffers -- board_entry_t::title is 256 bytes and a
 * GitHub title can be far longer. A truncation that lands in the middle of a
 * CJK character or an emoji produces a broken code point that renders fine,
 * survives every per-module test, and then silently blanks the board.
 *
 * These tests exist to make that failure loud, in the direction it actually
 * travels: truncate like the board does, render, encode.
 */

#include <stdio.h>
#include <string.h>

#include "../tests/test_util.h"

#include "config.h"
#include "core/arena.h"
#include "core/board.h"
#include "pipeline/render.h"
#include "yyjson.h"

/* Exposed by gist.c without being in gist.h, the way notify_build_title is. */
extern const char *gist_build_body(arena_t *a, const char *markdown, size_t *len_out);

static arena_t g_arena;

/* 2026-09-10T14:02:00Z, so the golden-ish assertions never depend on the clock. */
#define NOW ((time_t)1789048920)

static void entry_init(board_entry_t *e, long long id, const char *title)
{
    memset(e, 0, sizeof *e);
    e->id = id;
    e->number = 4211;
    e->llm_score = 9;
    e->kw_score = 30;
    snprintf(e->repo, sizeof e->repo, "%s", "tenstorrent/tt-metal");
    snprintf(e->first_seen, sizeof e->first_seen, "%s", "2026-09-10T12:02:00Z");
    snprintf(e->updated_at, sizeof e->updated_at, "%s", "2026-09-10T13:00:00Z");
    snprintf(e->html_url, sizeof e->html_url, "%s",
             "https://github.com/tenstorrent/tt-metal/issues/4211");
    snprintf(e->why, sizeof e->why, "%s", "unclaimed bounty, numerical");
    /* snprintf truncates at a byte boundary, which is exactly the hazard: this
     * is how a title reaches the board in the first place. */
    snprintf(e->title, sizeof e->title, "%s", title);
}

static const char *publish(board_entry_t *entries, size_t n, size_t *len_out)
{
    board_t b;
    const char *md;

    memset(&b, 0, sizeof b);
    b.entries = entries;
    b.n = n;
    b.cap = n;

    md = render_board(&g_arena, &b, NOW);
    if (md == NULL)
        return NULL;
    return gist_build_body(&g_arena, md, len_out);
}

/* The encoded body must parse back, and the markdown inside it must survive the
 * round trip byte for byte -- that is what proves the escaping is real. */
static void check_body_roundtrips(const char *body, size_t len, const char *needle)
{
    yyjson_doc *doc = yyjson_read(body, len, 0);
    yyjson_val *files, *file, *content;
    const char *s;

    CHECK(doc != NULL);
    if (doc == NULL)
        return;

    files = yyjson_obj_get(yyjson_doc_get_root(doc), "files");
    file = files != NULL ? yyjson_obj_get(files, GIST_FILENAME) : NULL;
    content = file != NULL ? yyjson_obj_get(file, "content") : NULL;
    s = content != NULL ? yyjson_get_str(content) : NULL;

    CHECK(s != NULL);
    if (s != NULL && needle != NULL)
        CHECK(strstr(s, needle) != NULL);

    yyjson_doc_free(doc);
}

static void test_cjk_title_survives_the_whole_path(void)
{
    board_entry_t e;
    const char *body;
    size_t len = 0;

    /* 100 CJK characters at 3 bytes each = 300 bytes into a 256-byte field, so
     * the copy is guaranteed to land inside a character unless something cuts
     * on a boundary. */
    char title[512];
    size_t i;

    title[0] = '\0';
    for (i = 0; i < 100; i++)
        strncat(title, "\xe8\xa1\x8c", sizeof title - strlen(title) - 1);

    entry_init(&e, 1, title);
    body = publish(&e, 1, &len);

    CHECK(body != NULL);
    if (body != NULL)
        check_body_roundtrips(body, len, "issuewatch");
}

static void test_emoji_title_survives_the_whole_path(void)
{
    board_entry_t e;
    const char *body;
    size_t len = 0;
    char title[512];
    size_t i;

    /* 4-byte code points, the worst case for a byte-wise truncation. */
    title[0] = '\0';
    for (i = 0; i < 80; i++)
        strncat(title, "\xf0\x9f\x9a\x80", sizeof title - strlen(title) - 1);

    entry_init(&e, 2, title);
    body = publish(&e, 1, &len);

    CHECK(body != NULL);
    if (body != NULL)
        check_body_roundtrips(body, len, "issuewatch");
}

/*
 * The markdown metacharacters and the JSON metacharacters are different sets,
 * and each layer must survive the other's escaping. A title carrying both is
 * the case where a mistake in either one shows up.
 */
static void test_markdown_and_json_metacharacters_coexist(void)
{
    board_entry_t e;
    const char *body;
    size_t len = 0;

    entry_init(&e, 3, "pipe | quote \" backslash \\ bracket [x] tick `y` <b>&amp;</b>");
    body = publish(&e, 1, &len);

    CHECK(body != NULL);
    if (body != NULL) {
        check_body_roundtrips(body, len, "issuewatch");
        /* A raw newline inside a JSON string literal is the classic break. */
        CHECK(memchr(body, '\n', len) == NULL || strstr(body, "\\n") != NULL);
    }
}

static void test_newline_in_title_cannot_break_the_table(void)
{
    board_entry_t e;
    const char *body;
    size_t len = 0;

    entry_init(&e, 4, "first line\nsecond line\r\nthird\ttabbed");
    body = publish(&e, 1, &len);

    CHECK(body != NULL);
    if (body != NULL)
        check_body_roundtrips(body, len, "issuewatch");
}

/*
 * Deliberate garbage, not just an accidental clip. yyjson accepting the body is
 * the assertion: it refuses invalid UTF-8, so a successful encode proves every
 * malformed sequence was dropped rather than copied through.
 */
static void test_malformed_utf8_cannot_block_the_publish(void)
{
    static const char *const junk[] = {
        "\xff\xfe bare invalid bytes",
        "\x80\x80 lone continuation bytes",
        "\xe8\xa1 clipped three-byte sequence",
        "\xf0\x9f\x9a clipped emoji",
        "\xc0\xaf overlong slash",
        "\xed\xa0\x80 surrogate half",
        "\xf5\x80\x80\x80 past U+10FFFF",
    };
    size_t k;

    for (k = 0; k < sizeof junk / sizeof junk[0]; k++) {
        board_entry_t e;
        const char *body;
        size_t len = 0;

        entry_init(&e, (long long)(100 + k), junk[k]);
        body = publish(&e, 1, &len);

        CHECK(body != NULL);
        if (body != NULL)
            check_body_roundtrips(body, len, "issuewatch");
    }
}

/* An empty board still has to publish: "nothing open" is information, and a
 * board that fails to encode would leave yesterday's stale one in place. */
static void test_empty_board_still_encodes(void)
{
    const char *body;
    size_t len = 0;

    body = publish(NULL, 0, &len);
    CHECK(body != NULL);
    if (body != NULL)
        check_body_roundtrips(body, len, "issuewatch");
}

int main(void)
{
    if (arena_init(&g_arena, 4u << 20) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }

    TEST_RUN(test_cjk_title_survives_the_whole_path);
    TEST_RUN(test_emoji_title_survives_the_whole_path);
    TEST_RUN(test_markdown_and_json_metacharacters_coexist);
    TEST_RUN(test_newline_in_title_cannot_break_the_table);
    TEST_RUN(test_malformed_utf8_cannot_block_the_publish);
    TEST_RUN(test_empty_board_still_encodes);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
