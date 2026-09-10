#ifndef TEST_UTIL_H
#define TEST_UTIL_H

/*
 * Plain assertions, no framework. Each tests/test_*.c is its own binary with
 * its own main(); `make test` builds and runs them all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures;
static int t_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        t_checks++;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            t_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(got, want)                                                    \
    do {                                                                       \
        long long g_ = (long long)(got), w_ = (long long)(want);               \
        t_checks++;                                                            \
        if (g_ != w_) {                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s == %lld, want %lld\n",           \
                    __FILE__, __LINE__, #got, g_, w_);                         \
            t_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_STREQ(got, want)                                                 \
    do {                                                                       \
        const char *g_ = (got), *w_ = (want);                                  \
        t_checks++;                                                            \
        if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0) {                  \
            fprintf(stderr, "  FAIL %s:%d: %s == \"%s\", want \"%s\"\n",        \
                    __FILE__, __LINE__, #got, g_ ? g_ : "(null)",              \
                    w_ ? w_ : "(null)");                                       \
            t_failures++;                                                      \
        }                                                                      \
    } while (0)

#define TEST_RUN(fn)                                                           \
    do {                                                                       \
        fprintf(stderr, "- %s\n", #fn);                                        \
        fn();                                                                  \
    } while (0)

#define TEST_REPORT()                                                          \
    do {                                                                       \
        fprintf(stderr, "%s: %d checks, %d failures\n",                        \
                __FILE__, t_checks, t_failures);                               \
        return t_failures == 0 ? 0 : 1;                                        \
    } while (0)

/* Reads a fixture file into a malloc'd NUL-terminated buffer. Exits on failure. */
static inline char *fixture_read(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (f == NULL) {
        fprintf(stderr, "cannot open fixture %s\n", path);
        exit(2);
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fprintf(stderr, "cannot size fixture %s\n", path);
        exit(2); // what does fseek, ftell and rewind do?
    }
    rewind(f);

    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read fixture %s\n", path);
        exit(2);
    }
    buf[n] = '\0';
    fclose(f);

    if (len_out != NULL)
        *len_out = (size_t)n;
    return buf;
}

#endif /* TEST_UTIL_H */
