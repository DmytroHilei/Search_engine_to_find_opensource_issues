/*
 * notify.c: priority clamping, ASCII header sanitising, the per-cycle cap, the
 * seen-set skip, and the invariant that --dry-run issues no HTTP request.
 *
 * No network: every notify_cycle() call here runs with dry_run=1, which returns
 * before touching http.h. notify_http_calls is asserted to prove it.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tests/test_util.h"

#include "core/arena.h"
#include "config.h"
#include "net/github.h"
#include "net/notify.h"
#include "core/state.h"

/* Both are exported by notify.c and deliberately absent from notify.h. */
extern unsigned long notify_http_calls;
extern size_t notify_build_title(char *dst, size_t dstlen, const char *repo,
                                 const char *title);

static arena_t g_arena;

/*
 * A state_t built by hand rather than by state_open(): the seen-set contract is
 * "SEEN_CAPACITY u64 slots at st->seen", and a test must not touch
 * $XDG_STATE_HOME or mmap a file.
 */
static int fake_state(state_t *st)
{
    memset(st, 0, sizeof *st);
    st->seen = calloc(SEEN_CAPACITY, sizeof *st->seen);
    st->seen_fd = -1;
    return st->seen != NULL ? 0 : -1;
}

static void fake_state_free(state_t *st)
{
    free(st->seen);
    st->seen = NULL;
}

static void fill_issues(issue_t *is, size_t n)
{
    size_t i;

    memset(is, 0, n * sizeof *is);
    for (i = 0; i < n; i++) {
        is[i].id = (long long)(90000 + i);
        is[i].number = (int)(i + 1);
        is[i].repo = "ggml-org/llama.cpp";
        is[i].title = "CUDA graph capture regression";
        is[i].body = "";
        is[i].html_url = "https://github.com/ggml-org/llama.cpp/issues/1";
        is[i].updated_at = "2026-09-10T07:00:00Z";
        is[i].llm_score = 8;
        is[i].why = "perf regression, reproducer attached";
        is[i].kw_score = 12;
    }
}

static int is_printable_ascii(const char *s)
{
    for (; *s != '\0'; s++)
        if ((unsigned char)*s < 0x20u || (unsigned char)*s > 0x7eu)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ tests */

static void test_priority_range_and_clamp(void)
{
    int s;

    /* Both ends clamped: a model answering -9 or 47 must stay inside 1..5. */
    CHECK_EQ(notify_priority(INT_MIN), 1);
    CHECK_EQ(notify_priority(-9), 1);
    CHECK_EQ(notify_priority(47), 5);
    CHECK_EQ(notify_priority(INT_MAX), 5);

    CHECK_EQ(notify_priority(0), 1);
    CHECK_EQ(notify_priority(2), 1);
    CHECK_EQ(notify_priority(3), 2);
    CHECK_EQ(notify_priority(4), 2);
    CHECK_EQ(notify_priority(5), 3);
    CHECK_EQ(notify_priority(6), 3);
    CHECK_EQ(notify_priority(7), 4);
    CHECK_EQ(notify_priority(8), 4);
    CHECK_EQ(notify_priority(9), 5);
    CHECK_EQ(notify_priority(10), 5);

    /* Monotonic and in range across the whole domain. */
    for (s = -20; s < 40; s++) {
        int p = notify_priority(s);

        CHECK(p >= 1 && p <= 5);
        CHECK(p >= notify_priority(s - 1));
    }
}

static void test_title_is_ascii_and_single_line(void)
{
    char title[512];

    /* Emoji, CJK, a bare LF, a bare CR and a CRLF pair. */
    notify_build_title(title, sizeof title, "ggml-org/llama.cpp",
                       "\xf0\x9f\x90\x9b CUDA \xe6\xa0\xb8\xe5\xbf\x83 crash\n"
                       "Priority: 5\rX-Injected: yes\r\ndone");

    CHECK(is_printable_ascii(title));
    CHECK(strchr(title, '\n') == NULL);
    CHECK(strchr(title, '\r') == NULL);
    CHECK(strstr(title, "[ggml-org/llama.cpp]") == title);
    /* The ASCII words survive; only the non-ASCII bytes are folded away. */
    CHECK(strstr(title, "CUDA") != NULL);
    CHECK(strstr(title, "done") != NULL);
    /* No stray high bytes from the multi-byte sequences. */
    CHECK(strstr(title, "\xf0") == NULL);

    /* Bound: a pathologically long title cannot blow past the buffer. */
    {
        char small[32];
        char huge[4096];

        memset(huge, 'T', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        CHECK(notify_build_title(small, sizeof small, "o/r", huge) < sizeof small);
        CHECK(is_printable_ascii(small));
    }

    /* Degenerate inputs must not crash or emit non-ASCII. */
    notify_build_title(title, sizeof title, NULL, NULL);
    CHECK(is_printable_ascii(title));
    CHECK(title[0] != '\0');
}

/* CONTEXT.md 8: a repo that had a bad night must not dump 200 pushes. */
static void test_cycle_cap(void)
{
    issue_t is[NOTIFY_MAX_PER_CYCLE + 7];
    size_t n = sizeof is / sizeof is[0];
    state_t st;
    unsigned long before = notify_http_calls;

    CHECK_EQ(fake_state(&st), 0);
    if (st.seen == NULL)
        return;
    fill_issues(is, n);

    CHECK_EQ(notify_cycle(&g_arena, &st, is, n, 1), NOTIFY_MAX_PER_CYCLE);
    CHECK_EQ(notify_http_calls, before);

    fake_state_free(&st);
}

static void test_seen_is_skipped(void)
{
    issue_t is[3];
    state_t st;
    uint64_t k0, k2;
    unsigned long before = notify_http_calls;

    CHECK_EQ(fake_state(&st), 0);
    if (st.seen == NULL)
        return;
    fill_issues(is, 3);

    k0 = state_key(is[0].id, is[0].updated_at);
    k2 = state_key(is[2].id, is[2].updated_at);
    state_mark_seen(&st, k0);
    state_mark_seen(&st, k2);
    CHECK_EQ(state_seen(&st, k0), 1);

    /* Only the one unseen issue is a candidate. */
    CHECK_EQ(notify_cycle(&g_arena, &st, is, 3, 1), 1);
    CHECK_EQ(notify_http_calls, before);

    fake_state_free(&st);
}

/*
 * The invariant CLAUDE.md demands: --dry-run must never POST. Asserted through
 * the http_perform_one() counter in notify.c, not by reading the code.
 *
 * Also pins the documented choice that a dry run does NOT mark the seen-set --
 * --dry-run exists for keyword tuning, which means several passes over the same
 * issues, and marking here would silence the first real run.
 */
static void test_dry_run_sends_nothing(void)
{
    issue_t is[4];
    state_t st;
    uint64_t key;
    unsigned long before = notify_http_calls;

    CHECK_EQ(fake_state(&st), 0);
    if (st.seen == NULL)
        return;
    fill_issues(is, 4);
    key = state_key(is[0].id, is[0].updated_at);

    CHECK_EQ(notify_cycle(&g_arena, &st, is, 4, 1), 4);
    CHECK_EQ(notify_http_calls, before);        /* no HTTP path was taken */
    CHECK_EQ(state_seen(&st, key), 0);          /* and nothing was marked seen */

    /* Repeatable: a second dry run reports the same four, not zero. */
    CHECK_EQ(notify_cycle(&g_arena, &st, is, 4, 1), 4);
    CHECK_EQ(notify_http_calls, before);

    /* Nothing to send is not an error. */
    CHECK_EQ(notify_cycle(&g_arena, &st, is, 0, 1), 0);
    CHECK_EQ(notify_cycle(&g_arena, &st, NULL, 0, 1), 0);
    CHECK(notify_cycle(&g_arena, NULL, is, 4, 1) < 0);
    CHECK_EQ(notify_http_calls, before);

    fake_state_free(&st);
}

/*
 * The shipped config.h still carries the placeholder topic, so notify_init()
 * must refuse: a public ntfy.sh topic is world-writable, and a default one lets
 * anyone spam the user's phone. This is also why no test here can reach the real
 * send path -- main() blocks a non-dry-run at init.
 */
static void test_init_rejects_placeholder_topic(void)
{
    if (strcmp(NTFY_TOPIC, "REPLACE_ME_WITH_RANDOM_HEX") == 0)
        CHECK(notify_init() < 0);
    else
        CHECK_EQ(notify_init(), 0);
}

int main(void)
{
    if (arena_init(&g_arena, ARENA_SIZE) != 0) {
        fprintf(stderr, "arena_init failed\n");
        return 2;
    }

    TEST_RUN(test_priority_range_and_clamp);
    TEST_RUN(test_title_is_ascii_and_single_line);
    TEST_RUN(test_cycle_cap);
    TEST_RUN(test_seen_is_skipped);
    TEST_RUN(test_dry_run_sends_nothing);
    TEST_RUN(test_init_rejects_placeholder_topic);

    /* Belt and braces: nothing in this binary may have issued a request. */
    CHECK_EQ(notify_http_calls, 0);

    arena_destroy(&g_arena);
    TEST_REPORT();
}
