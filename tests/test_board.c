#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "core/board.h"
#include "core/state.h"
#include "core/util.h"

#include "../tests/test_util.h"

/*
 * Everything runs inside a mkdtemp'd directory: these tests write board.tsv and
 * seen.bin, and must never go near the real state dir.
 */
static char g_root[256];              /* $XDG_STATE_HOME */
static char g_dir[320];               /* $XDG_STATE_HOME/issuewatch */

static const char *const TEST_REPOS[] = {
    "tenstorrent/tt-metal",
    "tinygrad/tinygrad",
};
#define N_TEST_REPOS (sizeof TEST_REPOS / sizeof TEST_REPOS[0])

static void sandbox_setup(void)
{
    const char *tmp = getenv("TMPDIR");
    char tpl[256];

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";

    snprintf(tpl, sizeof tpl, "%s/issuewatch-board-XXXXXX", tmp);
    if (mkdtemp(tpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(2);
    }
    snprintf(g_root, sizeof g_root, "%s", tpl);
    snprintf(g_dir, sizeof g_dir, "%s/issuewatch", g_root);

    if (setenv("XDG_STATE_HOME", g_root, 1) != 0) {
        fprintf(stderr, "setenv failed\n");
        exit(2);
    }
    mkdir(g_dir, 0700);
}

static void board_path(char *out, size_t outlen)
{
    snprintf(out, outlen, "%s/board.tsv", g_dir);
}

/*
 * seen.bin goes too: it outlives a state_close(), so a key marked in one test
 * would silently make board_merge() skip that id in the next.
 */
static void sandbox_wipe(void)
{
    char path[512];

    board_path(path, sizeof path);
    unlink(path);
    snprintf(path, sizeof path, "%s/board.tsv.tmp", g_dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/seen.bin", g_dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/etags", g_dir);
    unlink(path);
}

static void sandbox_teardown(void)
{
    sandbox_wipe();
    rmdir(g_dir);
    rmdir(g_root);
}

static void write_board(const char *contents)
{
    char path[512];
    FILE *f;

    board_path(path, sizeof path);
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs(contents, f);
    fclose(f);
}

/* The board is keyed by id, so a test that wants one back must look it up. */
static const board_entry_t *find(const board_t *b, long long id)
{
    size_t i;

    for (i = 0; i < b->n; i++) {
        if (b->entries[i].id == id)
            return &b->entries[i];
    }
    return NULL;
}

static void seed(board_entry_t *e, long long id, int score, const char *title)
{
    memset(e, 0, sizeof *e);
    e->id = id;
    e->number = (int)id;
    e->llm_score = score;
    e->kw_score = score * 3;
    snprintf(e->repo, sizeof e->repo, "tenstorrent/tt-metal");
    snprintf(e->updated_at, sizeof e->updated_at, "2026-09-10T09:41:00Z");
    snprintf(e->etag, sizeof e->etag, "W/\"e%lld\"", id);
    snprintf(e->title, sizeof e->title, "%s", title);
    snprintf(e->why, sizeof e->why, "unclaimed bounty");
    snprintf(e->html_url, sizeof e->html_url,
             "https://github.com/tenstorrent/tt-metal/issues/%lld", id);
}

/* Strict UTF-8 validity, so a truncation test can prove the cut landed on a
 * character boundary rather than merely on a plausible byte. */
static int utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    while (*p != '\0') {
        size_t need, i;

        if (*p < 0x80)
            need = 1;
        else if ((*p & 0xe0) == 0xc0)
            need = 2;
        else if ((*p & 0xf0) == 0xe0)
            need = 3;
        else if ((*p & 0xf8) == 0xf0)
            need = 4;
        else
            return 0;

        for (i = 1; i < need; i++) {
            if ((p[i] & 0xc0) != 0x80)
                return 0;
        }
        p += need;
    }
    return 1;
}

static void test_missing_file_is_empty(void)
{
    board_t b;
    char path[512];
    struct stat sb;

    sandbox_wipe();

    CHECK_EQ(board_open(&b, g_dir), 0);
    CHECK_EQ(b.n, 0);
    CHECK_EQ(b.cap, BOARD_MAX);
    CHECK_EQ(b.dirty, 0);
    CHECK(b.entries != NULL);
    CHECK_STREQ(b.dir, g_dir);

    /* Clean board, so the flush is a no-op and must not create the file. */
    CHECK_EQ(board_flush(&b), 0);
    board_path(path, sizeof path);
    CHECK(stat(path, &sb) != 0);

    board_close(&b);
    CHECK(b.entries == NULL);
    CHECK_EQ(b.n, 0);
}

/*
 * The reason this increment has tests at all. Every one of these titles breaks
 * a naive TSV writer in a different way.
 */
static void test_roundtrip_adversarial(void)
{
    static const char *const titles[] = {
        "plain ascii title",
        "tab\there and\tthere",
        "newline\nin the middle",
        "windows\\path\\to\\kernel",
        "literal backslash-t: a\\tb",
        "trailing backslash \\",
        "\xf0\x9f\x94\xa5 bf16 matmul NaN on RDNA4",
        "\xe8\xa1\x8c\xe5\x88\x97\xe7\xa9\x8d\xe3\x81\xae\xe7\xb2\xbe\xe5\xba\xa6",
        "mixed \t\n\\ \xf0\x9f\x92\xa1 \xd0\xba\xd0\xb8\xd1\x80\xd0\xb8\xd0\xbb",
        "\r carriage return",
    };
    const size_t n_titles = sizeof titles / sizeof titles[0];
    board_entry_t in;
    board_t b;
    state_t st;
    size_t i, n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    for (i = 0; i < n_titles; i++) {
        seed(&in, (long long)(i + 1), 7, titles[i]);
        if (i == 0)
            in.why[0] = '\0';               /* an empty field is its own trap */
        if (i == 1)
            snprintf(in.why, sizeof in.why, "why\twith\ttabs\nand a newline");
        if (i == 2)
            snprintf(in.html_url, sizeof in.html_url,
                     "https://example.invalid/a\\b?q=1\tx");
        in.assigned = (i % 2 == 0);
        CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-10T12:00:00Z", &n_new), 0);
        CHECK_EQ(n_new, 1);
    }
    CHECK_EQ(b.n, n_titles);
    CHECK_EQ(b.dirty, 1);
    CHECK_EQ(board_flush(&b), 0);
    CHECK_EQ(b.dirty, 0);
    board_close(&b);

    CHECK_EQ(board_open(&b, g_dir), 0);
    CHECK_EQ(b.n, n_titles);
    CHECK_EQ(b.dirty, 0);

    for (i = 0; i < n_titles; i++) {
        const board_entry_t *e = find(&b, (long long)(i + 1));

        CHECK(e != NULL);
        if (e == NULL)
            continue;

        seed(&in, (long long)(i + 1), 7, titles[i]);
        CHECK_STREQ(e->title, in.title);
        CHECK_STREQ(e->repo, in.repo);
        CHECK_STREQ(e->etag, in.etag);
        CHECK_STREQ(e->updated_at, in.updated_at);
        CHECK_STREQ(e->first_seen, "2026-09-10T12:00:00Z");
        CHECK_EQ(e->number, in.number);
        CHECK_EQ(e->llm_score, 7);
        CHECK_EQ(e->kw_score, 21);
        CHECK_EQ(e->assigned, (i % 2 == 0) ? 1 : 0);
        /* fresh is not persisted: a reloaded entry has not been seen this cycle. */
        CHECK_EQ(e->fresh, 0);

        if (i == 0)
            CHECK_STREQ(e->why, "");
        else if (i == 1)
            CHECK_STREQ(e->why, "why\twith\ttabs\nand a newline");
        else
            CHECK_STREQ(e->why, "unclaimed bounty");

        if (i == 2)
            CHECK_STREQ(e->html_url, "https://example.invalid/a\\b?q=1\tx");
        else
            CHECK_STREQ(e->html_url, in.html_url);
    }

    /* "a\\tb" must come back as backslash-t, never as a tab. */
    {
        const board_entry_t *e = find(&b, 5);

        CHECK(e != NULL);
        if (e != NULL)
            CHECK(strchr(e->title, '\t') == NULL);
    }

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_utf8_truncation(void)
{
    char line[1024];
    char title[512];
    board_t b;
    size_t i, len = 0;

    sandbox_wipe();

    /* One ASCII byte then 3-byte characters, so the byte-wise cut at
     * BOARD_TITLE_MAX - 1 lands in the middle of a character. */
    title[len++] = 'x';
    for (i = 0; i < 120; i++) {
        memcpy(title + len, "\xe8\xa1\x8c", 3);
        len += 3;
    }
    title[len] = '\0';
    CHECK(len > BOARD_TITLE_MAX);

    snprintf(line, sizeof line,
             "42\ttenstorrent/tt-metal\t7\t9\t27\t2026-09-01T00:00:00Z\t"
             "2026-09-02T00:00:00Z\tW/\"x\"\t%s\twhy\thttps://e.invalid/1\t0\n",
             title);
    write_board(line);

    CHECK_EQ(board_open(&b, g_dir), 0);
    CHECK_EQ(b.n, 1);
    if (b.n == 1) {
        size_t got = strlen(b.entries[0].title);

        CHECK(got < BOARD_TITLE_MAX);
        /* 1 + 3k, so the largest that fits under 255 bytes is 1 + 84*3 = 253. */
        CHECK_EQ(got, 253);
        CHECK(utf8_valid(b.entries[0].title));
        CHECK_EQ(memcmp(b.entries[0].title, title, got), 0);
    }
    board_close(&b);
}

static void test_garbage_lines_skipped(void)
{
    char big[4096];
    board_t b;
    FILE *f;
    char path[512];
    size_t i;

    sandbox_wipe();

    for (i = 0; i < sizeof big - 1; i++)
        big[i] = 'x';
    big[sizeof big - 1] = '\0';

    board_path(path, sizeof path);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f == NULL)
        return;

    fputs("no tabs at all\n", f);
    fputs("1\ttenstorrent/tt-metal\t1\t9\t27\t2026-09-01T00:00:00Z\n", f);
    fputs("2\ttenstorrent/tt-metal\t2\t9\t27\t2026-09-01T00:00:00Z\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tt\tw\tu\t0\textra\n", f);
    fputs("notanumber\ttenstorrent/tt-metal\t3\t9\t27\t2026-09-01T00:00:00Z\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tt\tw\tu\t0\n", f);
    fputs("0\ttenstorrent/tt-metal\t4\t9\t27\t2026-09-01T00:00:00Z\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tt\tw\tu\t0\n", f);
    fputs("5\t\t5\t9\t27\t2026-09-01T00:00:00Z\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tt\tw\tu\t0\n", f);
    fputs("6\ttenstorrent/tt-metal\t6\t9\t27\tnot-a-timestamp\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tt\tw\tu\t0\n", f);
    fputs("7\ttenstorrent/tt-metal\t7\t9\t27\t2026-09-01T00:00:00Z\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tbad\\qescape\tw\tu\t0\n", f);
    fputs("8\ttenstorrent/tt-metal\t8\tnine\t27\t2026-09-01T00:00:00Z\t"
          "2026-09-02T00:00:00Z\tW/\"a\"\tt\tw\tu\t0\n", f);
    fprintf(f, "9\ttenstorrent/tt-metal\t9\t9\t27\t2026-09-01T00:00:00Z\t"
               "2026-09-02T00:00:00Z\tW/\"a\"\t%s\tw\tu\t0\n", big);
    fputs("\n", f);
    fputs("10\ttenstorrent/tt-metal\t10\t8\t24\t2026-09-03T00:00:00Z\t"
          "\tW/\"ok\"\tkept title\tkept why\thttps://e.invalid/10\t1\n", f);
    /* A duplicate id would leave board_drop() removing only one of the pair. */
    fputs("10\ttenstorrent/tt-metal\t10\t1\t1\t2026-09-04T00:00:00Z\t"
          "\tW/\"dup\"\tdup\tdup\thttps://e.invalid/dup\t0\n", f);
    fclose(f);

    /* Garbage is skipped, never fatal -- and the overlong line at id 9 must not
     * take the good line after it down with it. */
    CHECK_EQ(board_open(&b, g_dir), 0);
    CHECK_EQ(b.n, 1);
    CHECK_EQ(b.dirty, 0);
    if (b.n == 1) {
        CHECK_EQ(b.entries[0].id, 10);
        CHECK_STREQ(b.entries[0].title, "kept title");
        CHECK_STREQ(b.entries[0].why, "kept why");
        CHECK_STREQ(b.entries[0].etag, "W/\"ok\"");
        CHECK_STREQ(b.entries[0].updated_at, "");   /* empty is legal */
        CHECK_EQ(b.entries[0].assigned, 1);
    }
    board_close(&b);
}

static void test_merge_upsert_preserves_first_seen(void)
{
    board_entry_t in;
    board_t b;
    state_t st;
    const board_entry_t *e;
    size_t n_new = 99;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    seed(&in, 1234, 5, "original title");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-01T00:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 1);
    CHECK_EQ(b.n, 1);

    board_clear_fresh(&b);
    CHECK_EQ(b.entries[0].fresh, 0);

    seed(&in, 1234, 9, "rescored title");
    snprintf(in.updated_at, sizeof in.updated_at, "2026-09-05T08:00:00Z");
    snprintf(in.etag, sizeof in.etag, "W/\"newer\"");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-05T00:00:00Z", &n_new), 0);
    /* An upsert is not a new entry. */
    CHECK_EQ(n_new, 0);
    CHECK_EQ(b.n, 1);

    e = find(&b, 1234);
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_STREQ(e->first_seen, "2026-09-01T00:00:00Z");
        CHECK_STREQ(e->updated_at, "2026-09-05T08:00:00Z");
        CHECK_STREQ(e->title, "rescored title");
        CHECK_STREQ(e->etag, "W/\"newer\"");
        CHECK_EQ(e->llm_score, 9);
        CHECK_EQ(e->fresh, 1);
    }

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_merge_skips_seen(void)
{
    board_entry_t in[3];
    board_t b;
    state_t st;
    size_t n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    seed(&in[0], 11, 8, "first");
    seed(&in[1], 22, 8, "already notified");
    seed(&in[2], 33, 8, "third");

    state_mark_seen(&st, state_key(in[1].id, in[1].updated_at));

    CHECK_EQ(board_merge(&b, &st, in, 3, "2026-09-10T12:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 2);
    CHECK_EQ(b.n, 2);
    CHECK(find(&b, 11) != NULL);
    CHECK(find(&b, 33) != NULL);
    /* Dropped as assigned or closed once, so it must not flap back. */
    CHECK(find(&b, 22) == NULL);

    /* n_new is reported even when nothing lands, and a zero-length merge is
     * legal rather than an argument error. */
    CHECK_EQ(board_merge(&b, &st, NULL, 0, "2026-09-10T12:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 0);
    CHECK_EQ(b.n, 2);

    /* n_new is optional. */
    CHECK_EQ(board_merge(&b, &st, in, 3, "2026-09-10T12:00:00Z", NULL), 0);
    CHECK_EQ(b.n, 2);

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_eviction_picks_worst(void)
{
    board_entry_t in;
    board_t b;
    state_t st;
    size_t i, n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    /* Fill it: same score, same first_seen, so the id tiebreak alone decides
     * which row is last -- and therefore which one goes. */
    for (i = 0; i < BOARD_MAX; i++) {
        seed(&in, (long long)(i + 1), 5, "filler");
        CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-01T00:00:00Z", &n_new), 0);
    }
    CHECK_EQ(b.n, BOARD_MAX);

    seed(&in, 5000, 9, "a real bounty");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-02T00:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 1);
    CHECK_EQ(b.n, BOARD_MAX);
    CHECK(find(&b, 5000) != NULL);
    CHECK(find(&b, BOARD_MAX) == NULL);          /* the largest id, so last */
    CHECK(find(&b, BOARD_MAX - 1) != NULL);

    /* A newcomer that is itself the worst row must leave the board alone. */
    seed(&in, 5001, 1, "not worth a slot");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-03T00:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 0);
    CHECK_EQ(b.n, BOARD_MAX);
    CHECK(find(&b, 5001) == NULL);
    CHECK(find(&b, BOARD_MAX - 1) != NULL);

    /* Equal on every leg of the comparison is still not better, so it loses. */
    seed(&in, 6000, 5, "tied but younger id");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-01T00:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 0);
    CHECK(find(&b, 6000) == NULL);

    /* Same score but newer, which outranks the stale filler and gets in. */
    seed(&in, 7000, 5, "fresher");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-09T00:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 1);
    CHECK(find(&b, 7000) != NULL);
    CHECK(find(&b, BOARD_MAX - 1) == NULL);

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_rank_order_and_tiebreaks(void)
{
    board_entry_t in[2];
    board_t b;
    state_t st;
    size_t n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    /* id 3 is a stale 9; ids 1 and 2 are fresh 9s tied on first_seen; id 4 is a
     * fresh 3 and must still lose to every 9. */
    seed(&in[0], 3, 9, "stale nine");
    CHECK_EQ(board_merge(&b, &st, in, 1, "2026-09-01T00:00:00Z", &n_new), 0);
    seed(&in[0], 2, 9, "fresh nine b");
    seed(&in[1], 1, 9, "fresh nine a");
    CHECK_EQ(board_merge(&b, &st, in, 2, "2026-09-05T00:00:00Z", &n_new), 0);
    seed(&in[0], 4, 3, "fresh three");
    CHECK_EQ(board_merge(&b, &st, in, 1, "2026-09-09T00:00:00Z", &n_new), 0);
    CHECK_EQ(b.n, 4);

    board_rank(&b);
    CHECK_EQ(b.entries[0].id, 1);   /* fresh 9, lower id wins the tie */
    CHECK_EQ(b.entries[1].id, 2);
    CHECK_EQ(b.entries[2].id, 3);   /* stale 9 */
    CHECK_EQ(b.entries[3].id, 4);   /* score beats recency */

    /* qsort is not stable, so ranking an already-ranked board must not move a
     * single row -- that is the whole point of the id leg. */
    board_rank(&b);
    CHECK_EQ(b.entries[0].id, 1);
    CHECK_EQ(b.entries[1].id, 2);
    CHECK_EQ(b.entries[2].id, 3);
    CHECK_EQ(b.entries[3].id, 4);

    /* Ranking is a pure reordering and survives a flush/reload unchanged. */
    CHECK_EQ(board_flush(&b), 0);
    board_close(&b);
    CHECK_EQ(board_open(&b, g_dir), 0);
    CHECK_EQ(b.n, 4);
    board_rank(&b);
    CHECK_EQ(b.entries[0].id, 1);
    CHECK_EQ(b.entries[3].id, 4);

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_drop(void)
{
    board_entry_t in[3];
    board_t b;
    state_t st;
    size_t n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    seed(&in[0], 1, 9, "a");
    seed(&in[1], 2, 8, "b");
    seed(&in[2], 3, 7, "c");
    CHECK_EQ(board_merge(&b, &st, in, 3, "2026-09-01T00:00:00Z", &n_new), 0);
    CHECK_EQ(n_new, 3);

    CHECK_EQ(board_drop(&b, 2), 1);
    CHECK_EQ(b.n, 2);
    /* The survivors keep their order. */
    CHECK_EQ(b.entries[0].id, 1);
    CHECK_EQ(b.entries[1].id, 3);

    CHECK_EQ(board_drop(&b, 2), 0);
    CHECK_EQ(board_drop(&b, 999), 0);
    CHECK_EQ(b.n, 2);

    /* Dropping the last slot must not walk off the end. */
    CHECK_EQ(board_drop(&b, 3), 1);
    CHECK_EQ(board_drop(&b, 1), 1);
    CHECK_EQ(b.n, 0);

    CHECK_EQ(board_flush(&b), 0);
    board_close(&b);

    CHECK_EQ(board_open(&b, g_dir), 0);
    CHECK_EQ(b.n, 0);
    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_expire(void)
{
    board_entry_t in;
    board_t b;
    state_t st;
    char old_iso[32], recent_iso[32], edge_iso[32];
    time_t now = 1789000000;   /* fixed, so the test does not drift with wallclock */
    size_t n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    CHECK_EQ(iso8601_format(now - (BOARD_STALE_DAYS + 5) * 24 * 3600, old_iso,
                            sizeof old_iso), 0);
    CHECK_EQ(iso8601_format(now - 24 * 3600, recent_iso, sizeof recent_iso), 0);
    CHECK_EQ(iso8601_format(now - BOARD_STALE_DAYS * 24 * 3600, edge_iso,
                            sizeof edge_iso), 0);

    seed(&in, 1, 9, "long stale");
    CHECK_EQ(board_merge(&b, &st, &in, 1, old_iso, &n_new), 0);
    seed(&in, 2, 9, "still fresh");
    CHECK_EQ(board_merge(&b, &st, &in, 1, recent_iso, &n_new), 0);
    seed(&in, 3, 9, "exactly at the cutoff");
    CHECK_EQ(board_merge(&b, &st, &in, 1, edge_iso, &n_new), 0);
    CHECK_EQ(b.n, 3);

    CHECK_EQ(board_expire(&b, now), 2);
    CHECK_EQ(b.n, 1);
    CHECK_EQ(b.entries[0].id, 2);
    CHECK_EQ(b.dirty, 1);

    /* Nothing left to expire, so the board is untouched. */
    CHECK_EQ(board_expire(&b, now), 0);
    CHECK_EQ(b.n, 1);

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_clear_fresh(void)
{
    board_entry_t in[2];
    board_t b;
    state_t st;
    size_t n_new = 0;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    seed(&in[0], 1, 9, "a");
    seed(&in[1], 2, 8, "b");
    CHECK_EQ(board_merge(&b, &st, in, 2, "2026-09-01T00:00:00Z", &n_new), 0);
    CHECK_EQ(b.entries[0].fresh, 1);
    CHECK_EQ(b.entries[1].fresh, 1);

    board_clear_fresh(&b);
    CHECK_EQ(b.entries[0].fresh, 0);
    CHECK_EQ(b.entries[1].fresh, 0);
    /* fresh is not persisted, so clearing it cannot make the board dirty. */
    CHECK_EQ(b.dirty, 1);

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

static void test_bad_args(void)
{
    board_entry_t in;
    board_t b;
    state_t st;
    char toolong[STATE_PATH_MAX + 16];

    sandbox_wipe();
    memset(toolong, 'x', sizeof toolong - 1);
    toolong[sizeof toolong - 1] = '\0';

    CHECK(board_open(NULL, g_dir) < 0);
    CHECK(board_open(&b, NULL) < 0);
    CHECK(board_open(&b, "") < 0);
    CHECK(board_open(&b, toolong) < 0);

    CHECK(board_flush(NULL) < 0);
    CHECK(board_merge(NULL, NULL, NULL, 0, "2026-09-01T00:00:00Z", NULL) < 0);
    CHECK_EQ(board_drop(NULL, 1), 0);
    CHECK_EQ(board_expire(NULL, 0), 0);
    board_rank(NULL);
    board_clear_fresh(NULL);
    board_close(NULL);

    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(board_open(&b, g_dir), 0);

    seed(&in, 1, 9, "a");
    CHECK(board_merge(&b, NULL, &in, 1, "2026-09-01T00:00:00Z", NULL) < 0);
    CHECK(board_merge(&b, &st, &in, 1, NULL, NULL) < 0);
    /* A first_seen that will not parse can never be aged out, so it is refused
     * at the door rather than stored. */
    CHECK(board_merge(&b, &st, &in, 1, "yesterday", NULL) < 0);
    CHECK(board_merge(&b, &st, NULL, 1, "2026-09-01T00:00:00Z", NULL) < 0);
    CHECK_EQ(b.n, 0);

    /* An id of 0 is the calloc'd empty value, not an issue. */
    seed(&in, 0, 9, "no id");
    CHECK_EQ(board_merge(&b, &st, &in, 1, "2026-09-01T00:00:00Z", NULL), 0);
    CHECK_EQ(b.n, 0);

    board_close(&b);
    CHECK_EQ(state_close(&st), 0);
}

int main(void)
{
    sandbox_setup();
    /* The garbage-line test feeds in bad input on purpose; its warnings are
     * expected output, not a failure. */
    log_set_level(LOG_ERR);

    TEST_RUN(test_missing_file_is_empty);
    TEST_RUN(test_roundtrip_adversarial);
    TEST_RUN(test_utf8_truncation);
    TEST_RUN(test_garbage_lines_skipped);
    TEST_RUN(test_merge_upsert_preserves_first_seen);
    TEST_RUN(test_merge_skips_seen);
    TEST_RUN(test_eviction_picks_worst);
    TEST_RUN(test_rank_order_and_tiebreaks);
    TEST_RUN(test_drop);
    TEST_RUN(test_expire);
    TEST_RUN(test_clear_fresh);
    TEST_RUN(test_bad_args);

    sandbox_teardown();
    TEST_REPORT();
}
