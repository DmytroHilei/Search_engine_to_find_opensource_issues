#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "core/state.h"
#include "core/util.h"

#include "../tests/test_util.h"

/*
 * These tests must never touch the real user state directory, so XDG_STATE_HOME
 * is redirected into a per-pid temp dir before anything calls state_open().
 */
static char g_root[256];              /* $XDG_STATE_HOME */
static char g_dir[320];               /* $XDG_STATE_HOME/issuewatch */

static const char *const TEST_REPOS[] = {
    "ggml-org/llama.cpp",
    "NVIDIA/cutlass",
    "triton-lang/triton",
};
#define N_TEST_REPOS (sizeof TEST_REPOS / sizeof TEST_REPOS[0])

static void sandbox_setup(void)
{
    const char *tmp = getenv("TMPDIR");

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";

    snprintf(g_root, sizeof g_root, "%s/issuewatch-test-%ld", tmp, (long)getpid());
    snprintf(g_dir, sizeof g_dir, "%s/issuewatch", g_root);

    if (setenv("XDG_STATE_HOME", g_root, 1) != 0) {
        fprintf(stderr, "setenv failed\n");
        exit(2);
    }
    /* state_open() mkdir -p's g_dir itself; the tests that pre-seed an etags
     * file need it to exist first. */
    mkdir(g_root, 0700);
    mkdir(g_dir, 0700);
}

static void sandbox_wipe(void)
{
    char path[512];

    snprintf(path, sizeof path, "%s/etags", g_dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/etags.tmp", g_dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/seen.bin", g_dir);
    unlink(path);
}

static void sandbox_teardown(void)
{
    sandbox_wipe();
    rmdir(g_dir);
    rmdir(g_root);
}

static void write_etags(const char *contents)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/etags", g_dir);
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs(contents, f);
    fclose(f);
}

static void test_key(void)
{
    uint64_t a, b;

    a = state_key(1234567, "2026-09-10T09:41:00Z");
    b = state_key(1234567, "2026-09-10T09:41:00Z");

    /* Stable across calls, or dedup breaks on the next cycle. */
    CHECK(a == b);
    /* 0 is the empty-slot sentinel in seen.bin and must never be a real key. */
    CHECK(a != 0);
    CHECK(state_key(0, "2026-09-10T09:41:00Z") != 0);
    CHECK(state_key(-1, NULL) != 0);

    CHECK(state_key(1234568, "2026-09-10T09:41:00Z") != a);

#if NOTIFY_ON_UPDATE
    /* updated_at participates: an edited issue can notify again. */
    CHECK(state_key(1234567, "2026-09-10T10:00:00Z") != a);
#else
    /* updated_at is ignored: an issue notifies exactly once, ever. */
    CHECK(state_key(1234567, "2026-09-10T10:00:00Z") == a);
    CHECK(state_key(1234567, NULL) == a);
#endif
}

static void test_open_creates_dir(void)
{
    state_t st;
    struct stat sb;
    char path[512];

    sandbox_wipe();
    rmdir(g_dir);

    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(st.n_repos, N_TEST_REPOS);
    CHECK(st.repos != NULL);
    CHECK(st.seen != NULL);
    CHECK(st.seen_fd >= 0);
    CHECK_STREQ(st.dir, g_dir);

    CHECK_EQ(stat(g_dir, &sb), 0);
    CHECK_EQ(sb.st_mode & 07777, 0700);

    /* One entry per configured repo, in the configured order. */
    CHECK_STREQ(st.repos[0].repo, TEST_REPOS[0]);
    CHECK_STREQ(st.repos[2].repo, TEST_REPOS[2]);
    /* Unseen repos start with no ETag and a lookback watermark. */
    CHECK_STREQ(st.repos[0].etag, "");
    CHECK(strlen(st.repos[0].watermark) == 20);
    CHECK(st.repos[0].watermark[19] == 'Z');

    CHECK(state_repo(&st, "NVIDIA/cutlass") == &st.repos[1]);
    CHECK(state_repo(&st, "not/configured") == NULL);
    CHECK(state_repo(&st, NULL) == NULL);

    CHECK_EQ(state_close(&st), 0);
    CHECK(st.repos == NULL);
    CHECK(st.seen == NULL);

    snprintf(path, sizeof path, "%s/seen.bin", g_dir);
    CHECK_EQ(stat(path, &sb), 0);
    CHECK_EQ(sb.st_size, (off_t)(SEEN_CAPACITY * sizeof(uint64_t)));
}

static void test_seen_roundtrip(void)
{
    state_t st;
    uint64_t k1, k2;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);

    k1 = state_key(4242, "2026-09-10T09:41:00Z");
    k2 = state_key(4243, "2026-09-10T09:41:00Z");

    CHECK_EQ(state_seen(&st, k1), 0);
    state_mark_seen(&st, k1);
    CHECK_EQ(state_seen(&st, k1), 1);

    /* A key that was never marked must read as unseen. */
    CHECK_EQ(state_seen(&st, k2), 0);

    /* Marking twice is idempotent, not a second slot. */
    state_mark_seen(&st, k1);
    CHECK_EQ(state_seen(&st, k1), 1);

    /* Key 0 can never be stored, so it can never read back as seen. */
    state_mark_seen(&st, 0);
    CHECK_EQ(state_seen(&st, 0), 0);

    CHECK_EQ(state_close(&st), 0);
}

static void test_seen_survives_reopen(void)
{
    state_t st;
    uint64_t k1, k2;

    sandbox_wipe();
    k1 = state_key(777, "2026-09-10T09:41:00Z");
    k2 = state_key(778, "2026-09-10T09:41:00Z");

    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    state_mark_seen(&st, k1);
    CHECK_EQ(state_close(&st), 0);

    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(state_seen(&st, k1), 1);
    CHECK_EQ(state_seen(&st, k2), 0);
    CHECK_EQ(state_close(&st), 0);
}

static void test_seen_collision_overwrites_oldest(void)
{
    state_t st;
    uint64_t keys[10];
    size_t i;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);

    /* All of these land on the same initial slot. */
    for (i = 0; i < 10; i++)
        keys[i] = 12345 + (uint64_t)SEEN_CAPACITY * i;

    for (i = 0; i < 8; i++)
        state_mark_seen(&st, keys[i]);
    for (i = 0; i < 8; i++)
        CHECK_EQ(state_seen(&st, keys[i]), 1);

    /* The 9th finds no free slot within the probe window, so it takes the
     * initial slot back from the oldest resident. */
    state_mark_seen(&st, keys[8]);
    CHECK_EQ(state_seen(&st, keys[8]), 1);
    CHECK_EQ(state_seen(&st, keys[0]), 0);
    CHECK_EQ(state_seen(&st, keys[7]), 1);

    CHECK_EQ(state_close(&st), 0);
}

static void test_flush_roundtrip(void)
{
    state_t st;
    repo_state_t *rs;
    char etags_path[512];
    struct stat sb;

    sandbox_wipe();
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);

    rs = state_repo(&st, "NVIDIA/cutlass");
    CHECK(rs != NULL);
    if (rs != NULL) {
        snprintf(rs->etag, sizeof rs->etag, "W/\"abc123def456\"");
        snprintf(rs->watermark, sizeof rs->watermark, "2026-09-10T09:41:00Z");
        rs->dirty = 1;
    }
    CHECK_EQ(state_flush(&st), 0);

    /* Nothing dirty now: a second flush must succeed trivially. */
    CHECK_EQ(state_flush(&st), 0);

    /* The temp file must not survive a successful rename. */
    snprintf(etags_path, sizeof etags_path, "%s/etags.tmp", g_dir);
    CHECK(stat(etags_path, &sb) != 0);

    snprintf(etags_path, sizeof etags_path, "%s/etags", g_dir);
    CHECK_EQ(stat(etags_path, &sb), 0);
    CHECK_EQ(state_close(&st), 0);

    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    rs = state_repo(&st, "NVIDIA/cutlass");
    CHECK(rs != NULL);
    if (rs != NULL) {
        CHECK_STREQ(rs->etag, "W/\"abc123def456\"");
        CHECK_STREQ(rs->watermark, "2026-09-10T09:41:00Z");
        CHECK_EQ(rs->dirty, 0);
    }
    /* Untouched repos came back with their empty ETag. */
    rs = state_repo(&st, "ggml-org/llama.cpp");
    CHECK(rs != NULL);
    if (rs != NULL)
        CHECK_STREQ(rs->etag, "");
    CHECK_EQ(state_close(&st), 0);
}

static void test_garbage_lines_skipped(void)
{
    state_t st;
    repo_state_t *rs;
    char big[STATE_REPO_MAX + HTTP_ETAG_MAX + 256];
    char line[sizeof big + 64];
    size_t i;

    sandbox_wipe();

    for (i = 0; i < sizeof big - 1; i++)
        big[i] = 'x';
    big[sizeof big - 1] = '\0';
    snprintf(line, sizeof line, "%s\tetag\t2026-09-10T09:41:00Z\n", big);

    /* Every line but the last is unusable in a different way. */
    write_etags("this line has no tabs at all\n");
    {
        char path[512];
        FILE *f;

        snprintf(path, sizeof path, "%s/etags", g_dir);
        f = fopen(path, "a");
        CHECK(f != NULL);
        if (f != NULL) {
            fputs("ggml-org/llama.cpp\tonly-two-fields\n", f);      /* truncated */
            fputs("\t\t\n", f);                                     /* empty repo */
            fputs("\n", f);                                         /* blank */
            fputs("triton-lang/triton\tetag\tnot-a-timestamp\n", f);/* bad watermark */
            fputs(line, f);                                         /* overlong */
            fputs("gone/repo\tW/\"stale\"\t2026-01-01T00:00:00Z\n", f); /* dropped */
            fputs("NVIDIA/cutlass\tW/\"good\"\t2026-09-10T09:41:00Z\n", f);
            fclose(f);
        }
    }

    /* Garbage is skipped, never fatal. */
    CHECK_EQ(state_open(&st, TEST_REPOS, N_TEST_REPOS), 0);
    CHECK_EQ(st.n_repos, N_TEST_REPOS);

    rs = state_repo(&st, "NVIDIA/cutlass");
    CHECK(rs != NULL);
    if (rs != NULL) {
        CHECK_STREQ(rs->etag, "W/\"good\"");
        CHECK_STREQ(rs->watermark, "2026-09-10T09:41:00Z");
    }

    /* The repos whose lines were rejected kept their first-run defaults. */
    rs = state_repo(&st, "ggml-org/llama.cpp");
    CHECK(rs != NULL);
    if (rs != NULL) {
        CHECK_STREQ(rs->etag, "");
        CHECK(strlen(rs->watermark) == 20);
    }
    rs = state_repo(&st, "triton-lang/triton");
    CHECK(rs != NULL);
    if (rs != NULL) {
        CHECK_STREQ(rs->etag, "");
        CHECK(strcmp(rs->watermark, "not-a-timestamp") != 0);
    }

    /* A repo no longer configured is dropped, not resurrected. */
    CHECK(state_repo(&st, "gone/repo") == NULL);

    CHECK_EQ(state_close(&st), 0);
}

static void test_bad_args(void)
{
    state_t st;

    CHECK(state_open(NULL, TEST_REPOS, N_TEST_REPOS) < 0);
    CHECK(state_open(&st, NULL, N_TEST_REPOS) < 0);
    CHECK(state_open(&st, TEST_REPOS, 0) < 0);
    CHECK(state_close(NULL) < 0);
    CHECK(state_flush(NULL) < 0);
    CHECK_EQ(state_seen(NULL, 1), 0);
    state_mark_seen(NULL, 1);
}

int main(void)
{
    sandbox_setup();
    /* Warnings here are expected: the garbage-line test feeds in bad input. */
    log_set_level(LOG_ERR);

    TEST_RUN(test_key);
    TEST_RUN(test_open_creates_dir);
    TEST_RUN(test_seen_roundtrip);
    TEST_RUN(test_seen_survives_reopen);
    TEST_RUN(test_seen_collision_overwrites_oldest);
    TEST_RUN(test_flush_roundtrip);
    TEST_RUN(test_garbage_lines_skipped);
    TEST_RUN(test_bad_args);

    sandbox_teardown();
    TEST_REPORT();
}
