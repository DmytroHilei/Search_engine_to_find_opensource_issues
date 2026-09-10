/*
 * render.c: board_t -> Markdown. Pure policy, so there is nothing to fake --
 * the board is built by hand here rather than through board_open(), which keeps
 * this binary independent of board.c's persistence layer.
 *
 * The golden test is the point of the file: the board's layout is a contract
 * with a phone screen, and a diff in it should be a deliberate edit, not a
 * side effect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../tests/test_util.h"

#include "config.h"
#include "core/arena.h"
#include "core/board.h"
#include "core/util.h"
#include "pipeline/render.h"

static arena_t g_arena;

#define SLOTS 8
static board_entry_t g_slots[SLOTS];
static board_t g_board;

static void board_reset(size_t n)
{
    memset(g_slots, 0, sizeof g_slots);
    g_board.entries = g_slots;
    g_board.n = n;
    g_board.cap = SLOTS;
    g_board.dirty = 0;
    g_board.dir[0] = '\0';
}

static void set_entry(board_entry_t *e, long long id, const char *repo, int number,
                      int llm_score, const char *first_seen, const char *title,
                      const char *why, const char *url)
{
    memset(e, 0, sizeof *e);
    e->id = id;
    e->number = number;
    e->llm_score = llm_score;
    snprintf(e->repo, sizeof e->repo, "%s", repo);
    snprintf(e->first_seen, sizeof e->first_seen, "%s", first_seen);
    snprintf(e->title, sizeof e->title, "%s", title);
    snprintf(e->why, sizeof e->why, "%s", why);
    snprintf(e->html_url, sizeof e->html_url, "%s", url);
}

static time_t at(const char *iso)
{
    time_t t = 0;

    if (iso8601_parse(iso, &t) != 0) {
        fprintf(stderr, "test bug: unparseable stamp %s\n", iso);
        exit(2);
    }
    return t;
}

static size_t count_char(const char *s, char c)
{
    size_t n = 0;

    for (; *s != '\0'; s++) {
        if (*s == c)
            n++;
    }
    return n;
}

/* Cell separators only: a '|' that survived escaping is preceded by a backslash. */
static size_t count_bare_pipes(const char *s)
{
    size_t n = 0;
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == '|' && (i == 0 || s[i - 1] != '\\'))
            n++;
    }
    return n;
}

/* ------------------------------------------------------------------ golden */

static void test_golden(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    const char *got;

    /*
     * Deliberately mixed: one hot entry with a reason, one mid entry with none,
     * one below LLM_SCORE_MIN, an unqualified repo name, and an underscore in a
     * title so the escaper is exercised by the golden output too.
     */
    static const char *WANT =
        "# issuewatch — 3 open · updated 2026-09-10 14:02 UTC\n"
        "\n"
        "| # | score | age | repo | issue |\n"
        "|---|-------|-----|------|-------|\n"
        "| 1 | 9 🟢 | 2h | tt-metal | "
        "[bf16 matmul NaN on RDNA4]"
        "(https://github.com/tenstorrent/tt-metal/issues/4211)"
        "<br>*reproducible numerics bug, bounty open* |\n"
        "| 2 | 7 🟡 | 3d | llama.cpp | "
        "[Metal backend crashes on Q4\\_K\\_M]"
        "(https://github.com/ggml-org/llama.cpp/issues/91) |\n"
        "| 3 | 5 ⚪ | just now | pytorch | "
        "[docs typo](https://github.com/pytorch/pytorch/issues/7)"
        "<br>*low value, other platform* |\n";

    board_reset(3);
    set_entry(&g_slots[0], 1, "tenstorrent/tt-metal", 4211, 9,
              "2026-09-10T12:02:00Z", "bf16 matmul NaN on RDNA4",
              "reproducible numerics bug, bounty open",
              "https://github.com/tenstorrent/tt-metal/issues/4211");
    set_entry(&g_slots[1], 2, "ggml-org/llama.cpp", 91, 7,
              "2026-09-07T14:02:00Z", "Metal backend crashes on Q4_K_M", "",
              "https://github.com/ggml-org/llama.cpp/issues/91");
    set_entry(&g_slots[2], 3, "pytorch", 7, 5,
              "2026-09-10T14:01:30Z", "docs typo", "low value, other platform",
              "https://github.com/pytorch/pytorch/issues/7");

    got = render_board(&g_arena, &g_board, now);
    CHECK(got != NULL);
    CHECK_STREQ(got, WANT);
}

/* ------------------------------------------------------------------- empty */

static void test_empty_board_is_still_a_document(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    const char *got;

    static const char *WANT =
        "# issuewatch — 0 open · updated 2026-09-10 14:02 UTC\n"
        "\n"
        "Nothing open right now.\n";

    board_reset(0);
    got = render_board(&g_arena, &g_board, now);
    CHECK(got != NULL);
    CHECK_STREQ(got, WANT);

    /* A blank page is indistinguishable from a broken daemon. */
    CHECK(got != NULL && got[0] != '\0');
}

/* ---------------------------------------------------------------- escaping */

static void test_markdown_injection_is_neutralised(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    const char *got;

    board_reset(1);
    set_entry(&g_slots[0], 1, "org/repo", 1, 9, "2026-09-10T13:02:00Z",
              "a | b\nsecond [x] `code` *bold* _u_ <script> & \\ 🚀 行列積 done",
              "why | with [brackets] and\nnewline",
              "https://github.com/org/repo/issues/1");

    got = render_board(&g_arena, &g_board, now);
    CHECK(got != NULL);
    if (got == NULL)
        return;

    /* The two failures that destroy the table outright. */
    CHECK(strstr(got, "a \\| b") != NULL);
    CHECK(strstr(got, "b second") != NULL);        /* newline became a space */

    /* One row means header + separator + row; a stray newline would add lines. */
    CHECK_EQ(count_char(got, '\n'), 5);
    /* 6 separators per line across 3 table lines, and nothing leaked. */
    CHECK_EQ(count_bare_pipes(got), 18);

    /* Link text, code spans and emphasis must not break out of the cell. */
    CHECK(strstr(got, "\\[x\\]") != NULL);
    CHECK(strstr(got, "\\`code\\`") != NULL);
    CHECK(strstr(got, "\\*bold\\*") != NULL);
    CHECK(strstr(got, "\\_u\\_") != NULL);
    CHECK(strstr(got, "\\\\") != NULL);

    /* We emit a literal <br>, so raw markup in a title must be inert. */
    CHECK(strstr(got, "&lt;script&gt;") != NULL);
    CHECK(strstr(got, "<script>") == NULL);
    CHECK(strstr(got, "&amp;") != NULL);

    /* Emoji and CJK are fine in a body -- rule 5 is about ntfy headers. */
    CHECK(strstr(got, "🚀") != NULL);
    CHECK(strstr(got, "行列積") != NULL);

    /* The why cell goes through the same escaper. */
    CHECK(strstr(got, "why \\| with \\[brackets\\] and newline") != NULL);
}

static void test_title_and_url_edges(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    const char *got;

    board_reset(3);
    /* A destination with characters that would end it early. */
    set_entry(&g_slots[0], 1, "org/a", 1, 9, "2026-09-10T13:02:00Z", "spaces",
              "", "https://x.invalid/a b(c)<d>");
    /* Already percent-encoded: must not be double-encoded into a dead link. */
    set_entry(&g_slots[1], 2, "org/b", 2, 9, "2026-09-10T13:02:00Z", "encoded",
              "", "https://x.invalid/a%2Fb");
    /* No URL and no title at all. */
    set_entry(&g_slots[2], 3, "org/c", 3, 9, "2026-09-10T13:02:00Z", "", "", "");

    got = render_board(&g_arena, &g_board, now);
    CHECK(got != NULL);
    if (got == NULL)
        return;

    CHECK(strstr(got, "(https://x.invalid/a%20b%28c%29%3Cd%3E)") != NULL);
    CHECK(strstr(got, "(https://x.invalid/a%2Fb)") != NULL);
    CHECK(strstr(got, "%252F") == NULL);
    /* No destination degrades to plain text, never to an empty dead link. */
    CHECK(strstr(got, "| (untitled) |") != NULL);
    CHECK(strstr(got, "]()") == NULL);
}

static void test_score_dots(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    const char *got;

    board_reset(3);
    set_entry(&g_slots[0], 1, "o/r", 1, 10, "2026-09-10T13:02:00Z", "hot", "",
              "https://x.invalid/1");
    set_entry(&g_slots[1], 2, "o/r", 2, 6, "2026-09-10T13:02:00Z", "warm", "",
              "https://x.invalid/2");
    set_entry(&g_slots[2], 3, "o/r", 3, 0, "2026-09-10T13:02:00Z", "cold", "",
              "https://x.invalid/3");

    got = render_board(&g_arena, &g_board, now);
    CHECK(got != NULL);
    if (got == NULL)
        return;
    CHECK(strstr(got, "| 10 🟢 |") != NULL);
    CHECK(strstr(got, "| 6 🟡 |") != NULL);
    CHECK(strstr(got, "| 0 ⚪ |") != NULL);
}

/* --------------------------------------------------------------------- age */

static void test_age_buckets(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    char buf[32];

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-10T14:02:00Z", now), 0);
    CHECK_STREQ(buf, "just now");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-10T14:01:01Z", now), 0);
    CHECK_STREQ(buf, "just now");                  /* 59s */

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-10T14:01:00Z", now), 0);
    CHECK_STREQ(buf, "1m");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-10T13:02:01Z", now), 0);
    CHECK_STREQ(buf, "59m");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-10T13:02:00Z", now), 0);
    CHECK_STREQ(buf, "1h");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-09T14:02:01Z", now), 0);
    CHECK_STREQ(buf, "23h");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-09T14:02:00Z", now), 0);
    CHECK_STREQ(buf, "1d");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-07T14:02:00Z", now), 0);
    CHECK_STREQ(buf, "3d");

    CHECK_EQ(render_age(buf, sizeof buf, "2026-06-12T14:02:00Z", now), 0);
    CHECK_STREQ(buf, "90d");

    /* Our clock behind GitHub's is skew, not an issue filed in the future. */
    CHECK_EQ(render_age(buf, sizeof buf, "2026-09-10T15:02:00Z", now), 0);
    CHECK_STREQ(buf, "just now");
}

static void test_age_bad_input(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    char buf[32];

    /* Negative, but the cell is still printable -- render_board relies on that. */
    CHECK(render_age(buf, sizeof buf, "not-a-date", now) < 0);
    CHECK_STREQ(buf, "?");

    CHECK(render_age(buf, sizeof buf, "", now) < 0);
    CHECK_STREQ(buf, "?");

    CHECK(render_age(buf, sizeof buf, NULL, now) < 0);
    CHECK_STREQ(buf, "?");

    CHECK(render_age(buf, sizeof buf, "2026-09", now) < 0);
    CHECK_STREQ(buf, "?");

    CHECK(render_age(NULL, sizeof buf, "2026-09-10T13:02:00Z", now) < 0);
    CHECK(render_age(buf, 0, "2026-09-10T13:02:00Z", now) < 0);

    /* Too small to hold the answer: empty, never truncated. */
    CHECK(render_age(buf, 2, "2026-09-10T14:02:00Z", now) < 0);
    CHECK_STREQ(buf, "");
}

static void test_bad_age_still_renders_the_board(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    const char *got;

    board_reset(1);
    set_entry(&g_slots[0], 1, "o/r", 1, 9, "garbage", "still here", "",
              "https://x.invalid/1");

    got = render_board(&g_arena, &g_board, now);
    CHECK(got != NULL);
    CHECK(got != NULL && strstr(got, "| 9 🟢 | ? | r |") != NULL);
}

/* ---------------------------------------------------------------- overflow */

static void test_overflow_returns_null_not_a_truncated_board(void)
{
    time_t now = at("2026-09-10T14:02:00Z");
    arena_t tiny;

    /*
     * Half a board reads as "these are all the open bounties", which is worse
     * than publishing nothing -- render.h makes that the contract, so prove it
     * rather than trusting the estimate.
     */
    CHECK_EQ(arena_init(&tiny, 64), 0);

    board_reset(1);
    set_entry(&g_slots[0], 1, "o/r", 1, 9, "2026-09-10T13:02:00Z", "title", "why",
              "https://x.invalid/1");
    CHECK(render_board(&tiny, &g_board, now) == NULL);

    /* Even the empty document does not fit, and must not come back partial. */
    board_reset(0);
    CHECK(render_board(&tiny, &g_board, now) == NULL);

    arena_destroy(&tiny);
}

static void test_bad_args(void)
{
    time_t now = at("2026-09-10T14:02:00Z");

    board_reset(1);
    set_entry(&g_slots[0], 1, "o/r", 1, 9, "2026-09-10T13:02:00Z", "t", "", "u");

    CHECK(render_board(NULL, &g_board, now) == NULL);
    CHECK(render_board(&g_arena, NULL, now) == NULL);

    /* A count past the slots must clamp, not read off the end. */
    g_board.n = SLOTS + 100;
    CHECK(render_board(&g_arena, &g_board, now) != NULL);

    board_reset(1);
    g_board.entries = NULL;
    CHECK(render_board(&g_arena, &g_board, now) != NULL);
}

int main(void)
{
    if (arena_init(&g_arena, ARENA_SIZE) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }

    TEST_RUN(test_golden);
    TEST_RUN(test_empty_board_is_still_a_document);
    TEST_RUN(test_markdown_injection_is_neutralised);
    TEST_RUN(test_title_and_url_edges);
    TEST_RUN(test_score_dots);
    TEST_RUN(test_age_buckets);
    TEST_RUN(test_age_bad_input);
    TEST_RUN(test_bad_age_still_renders_the_board);
    TEST_RUN(test_overflow_returns_null_not_a_truncated_board);
    TEST_RUN(test_bad_args);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
