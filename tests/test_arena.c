#include <stdint.h>
#include <string.h>

#include "core/arena.h"

#include "../tests/test_util.h"

static void test_init_destroy(void)
{
    arena_t a;

    CHECK_EQ(arena_init(&a, 4096), 0);
    CHECK(a.base != NULL);
    CHECK_EQ(a.cap, 4096);
    CHECK_EQ(a.used, 0);
    CHECK_EQ(a.peak, 0);
    CHECK_EQ(a.failures, 0);
    arena_destroy(&a);
    CHECK(a.base == NULL);

    /* A zero-sized arena is a caller bug, not a 0-byte allocation. */
    CHECK(arena_init(&a, 0) < 0);
    CHECK(arena_init(NULL, 16) < 0);
}

static void test_alignment(void)
{
    arena_t a;
    size_t aligns[] = { 1, 2, 4, 8, 16, 32, 64 };
    size_t i;

    CHECK_EQ(arena_init(&a, 4096), 0);

    /* Odd sizes in between so each request starts misaligned. */
    for (i = 0; i < sizeof aligns / sizeof aligns[0]; i++) {
        void *p;

        CHECK(arena_alloc(&a, 3, 1) != NULL);
        p = arena_alloc(&a, 8, aligns[i]);
        CHECK(p != NULL);
        CHECK_EQ((uintptr_t)p % aligns[i], 0);
    }

    /* align 0 means alignof(max_align_t): good enough for any scalar. */
    CHECK(arena_alloc(&a, 1, 1) != NULL);
    CHECK_EQ((uintptr_t)arena_alloc(&a, 8, 0) % _Alignof(max_align_t), 0);

    /* Non-power-of-two is refused, not asserted on. */
    CHECK_EQ(a.failures, 0);
    CHECK(arena_alloc(&a, 8, 3) == NULL);
    CHECK_EQ(a.failures, 1);

    arena_destroy(&a);
}

static void test_distinct_regions(void)
{
    arena_t a;
    char *p, *q;

    CHECK_EQ(arena_init(&a, 4096), 0);
    p = arena_alloc(&a, 16, 1);
    q = arena_alloc(&a, 16, 1);
    CHECK(p != NULL && q != NULL);
    CHECK(q >= p + 16);

    memset(p, 'a', 16);
    memset(q, 'b', 16);
    CHECK_EQ(p[15], 'a');
    CHECK_EQ(q[0], 'b');
    arena_destroy(&a);
}

static void test_exhaustion(void)
{
    arena_t a;
    size_t used_before;

    CHECK_EQ(arena_init(&a, 128), 0);
    CHECK(arena_alloc(&a, 64, 1) != NULL);
    used_before = a.used;

    CHECK(arena_alloc(&a, 4096, 1) == NULL);
    CHECK_EQ(a.failures, 1);
    /* A refusal must not consume space, or the arena bleeds out under load. */
    CHECK_EQ(a.used, used_before);

    CHECK(arena_alloc(&a, 65, 1) == NULL);
    CHECK_EQ(a.failures, 2);

    /* Still usable afterwards: exhaustion is not a poisoned state. */
    CHECK(arena_alloc(&a, 64, 1) != NULL);
    CHECK_EQ(a.used, 128);
    arena_destroy(&a);
}

static void test_overflow_refused(void)
{
    arena_t a;

    CHECK_EQ(arena_init(&a, 1024), 0);
    CHECK(arena_alloc(&a, 1, 1) != NULL); /* leave used non-zero and misaligned */

    /* used + pad + size wraps to something small if computed naively. */
    CHECK(arena_alloc(&a, SIZE_MAX, 1) == NULL);
    CHECK(arena_alloc(&a, SIZE_MAX, 64) == NULL);
    CHECK(arena_alloc(&a, SIZE_MAX - 8, 16) == NULL);
    CHECK_EQ(a.failures, 3);
    CHECK_EQ(a.used, 1);

    /* count * size wraps too. */
    CHECK(arena_calloc(&a, SIZE_MAX / 2, 4) == NULL);
    CHECK(arena_calloc(&a, (SIZE_MAX / 8) + 1, 8) == NULL);
    CHECK_EQ(a.failures, 5);
    CHECK_EQ(a.used, 1);

    arena_destroy(&a);
}

static void test_calloc_zeroes(void)
{
    arena_t a;
    unsigned char *p;
    size_t i;

    CHECK_EQ(arena_init(&a, 4096), 0);

    /* Dirty the region first so a zeroed read cannot be luck. */
    p = arena_alloc(&a, 256, 1);
    CHECK(p != NULL);
    memset(p, 0xab, 256);
    arena_reset(&a);

    p = arena_calloc(&a, 32, 4);
    CHECK(p != NULL);
    for (i = 0; i < 32 * 4; i++)
        CHECK_EQ(p[i], 0);

    CHECK(arena_calloc(&a, 0, 8) != NULL); /* zero elements is not a failure */
    CHECK_EQ(a.failures, 0);
    arena_destroy(&a);
}

static void test_reset_and_peak(void)
{
    arena_t a;

    CHECK_EQ(arena_init(&a, 256), 0);
    CHECK(arena_alloc(&a, 200, 1) != NULL);
    CHECK_EQ(a.used, 200);

    arena_reset(&a);
    CHECK_EQ(a.used, 0);
    CHECK_EQ(a.peak, 200);

    /* The full capacity is available again after a reset. */
    CHECK(arena_alloc(&a, 256, 1) != NULL);
    arena_reset(&a);
    CHECK_EQ(a.peak, 256);

    /* A smaller cycle must not lower the high-water mark. */
    CHECK(arena_alloc(&a, 8, 1) != NULL);
    arena_reset(&a);
    CHECK_EQ(a.peak, 256);
    CHECK_EQ(a.failures, 0);

    arena_destroy(&a);
}

static void test_strdup(void)
{
    arena_t a;
    char *p;

    CHECK_EQ(arena_init(&a, 256), 0);

    p = arena_strdup(&a, "owner/repo");
    CHECK_STREQ(p, "owner/repo");

    p = arena_strndup(&a, "truncate me", 8);
    CHECK_STREQ(p, "truncate");

    /* n longer than the string stops at the NUL, it does not read past it. */
    p = arena_strndup(&a, "short", 64);
    CHECK_STREQ(p, "short");

    p = arena_strndup(&a, "abc", 0);
    CHECK_STREQ(p, "");

    CHECK(arena_strdup(&a, NULL) == NULL);
    CHECK(arena_strndup(&a, NULL, 4) == NULL);

    /* Exhaustion returns NULL rather than a partial copy. */
    CHECK(arena_strdup(&a, "x") != NULL);
    while (arena_alloc(&a, 16, 1) != NULL)
        ;
    CHECK(arena_strdup(&a, "no room left") == NULL);
    CHECK(a.failures > 0);

    arena_destroy(&a);
}

static void test_printf(void)
{
    arena_t a;
    char *p;
    size_t used_before, failures_before;

    CHECK_EQ(arena_init(&a, 4096), 0);

    p = arena_printf(&a, "%s/%s#%d", "ggml-org", "llama.cpp", 1234);
    CHECK_STREQ(p, "ggml-org/llama.cpp#1234");

    /* Round trip of a string long enough to need the sizing pass. */
    p = arena_printf(&a, "%s", "0123456789012345678901234567890123456789"
                                "0123456789012345678901234567890123456789");
    CHECK(p != NULL);
    CHECK_EQ(strlen(p), 80);
    CHECK_EQ(p[79], '9');
    CHECK_EQ(p[80], '\0');

    p = arena_printf(&a, "%s", "");
    CHECK_STREQ(p, "");

    /* No room: NULL, and the arena is not left partially bumped. */
    while (arena_alloc(&a, 64, 1) != NULL)
        ;
    used_before = a.used;
    failures_before = a.failures;
    CHECK(arena_printf(&a, "%s-%s", "way", "too long for what is left") == NULL);
    CHECK_EQ(a.used, used_before);
    CHECK_EQ(a.failures, failures_before + 1);

    arena_destroy(&a);
}

static void test_null_arena(void)
{
    /* Nothing here may crash: callers check for NULL, not for a live arena. */
    CHECK(arena_alloc(NULL, 8, 8) == NULL);
    CHECK(arena_calloc(NULL, 2, 8) == NULL);
    CHECK(arena_strdup(NULL, "x") == NULL);
    CHECK(arena_strndup(NULL, "x", 1) == NULL);
    CHECK(arena_printf(NULL, "%d", 1) == NULL);
    arena_reset(NULL);
    arena_destroy(NULL);
}

int main(void)
{
    TEST_RUN(test_init_destroy);
    TEST_RUN(test_alignment);
    TEST_RUN(test_distinct_regions);
    TEST_RUN(test_exhaustion);
    TEST_RUN(test_overflow_refused);
    TEST_RUN(test_calloc_zeroes);
    TEST_RUN(test_reset_and_peak);
    TEST_RUN(test_strdup);
    TEST_RUN(test_printf);
    TEST_RUN(test_null_arena);
    TEST_REPORT();
}
