#include "core/arena.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bump allocator. Every refusal is a NULL return plus a `failures` bump: this
 * runs on sizes derived from untrusted network payloads, so an oversized
 * request must be an ordinary "no", never an abort and never a silent grow.
 */

#define ARENA_DEFAULT_ALIGN _Alignof(max_align_t)

int arena_init(arena_t *a, size_t cap)
{
    if (a == NULL || cap == 0)
        return -EINVAL;

    memset(a, 0, sizeof *a);
    a->base = malloc(cap);
    if (a->base == NULL)
        return -ENOMEM;
    a->cap = cap;
    return 0;
}

void *arena_alloc(arena_t *a, size_t size, size_t align)
{
    uintptr_t cur;
    size_t misalign, pad;

    if (a == NULL || a->base == NULL)
        return NULL;

    if (align == 0)
        align = ARENA_DEFAULT_ALIGN;

    /* Validate rather than assert: a bad alignment is a caller bug, but a
     * daemon that has been running for a week should refuse, not die. */
    if ((align & (align - 1)) != 0) {
        a->failures++;
        return NULL;
    }

    cur = (uintptr_t)a->base + (uintptr_t)a->used;
    misalign = (size_t)(cur & (uintptr_t)(align - 1));
    pad = (misalign != 0) ? align - misalign : 0;

    /* Both checks are subtractions against the remaining space, never
     * `used + pad + size`: that sum wraps on a hostile `size` and a wrap here
     * is a heap overflow, not a style nit. */
    if (pad > a->cap - a->used) {
        a->failures++;
        return NULL;
    }
    if (size > a->cap - a->used - pad) {
        a->failures++;
        return NULL;
    }

    a->used += pad + size;
    return a->base + (a->used - size);
}

void *arena_calloc(arena_t *a, size_t count, size_t size)
{
    void *p;
    size_t total;

    if (a == NULL)
        return NULL;

    if (count != 0 && size > SIZE_MAX / count) {
        a->failures++;
        return NULL;
    }
    total = count * size;

    p = arena_alloc(a, total, 0);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

char *arena_strndup(arena_t *a, const char *s, size_t n)
{
    char *p;
    size_t len;

    if (a == NULL || s == NULL)
        return NULL;

    /* Hand-rolled instead of strnlen(): `s` may legitimately be shorter than
     * `n`, and reading past its NUL would be out of bounds. */
    for (len = 0; len < n && s[len] != '\0'; len++)
        ;

    if (len == SIZE_MAX) {
        a->failures++;
        return NULL;
    }

    p = arena_alloc(a, len + 1, 1);
    if (p == NULL)
        return NULL;

    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *arena_strdup(arena_t *a, const char *s)
{
    if (s == NULL)
        return NULL;
    return arena_strndup(a, s, SIZE_MAX - 1);
}

char *arena_printf(arena_t *a, const char *fmt, ...)
{
    va_list ap;
    char *p;
    size_t saved;
    int need, wrote;

    if (a == NULL || fmt == NULL)
        return NULL;

    /* Size first, then write: the arena is only bumped once the exact length
     * is known, so a refusal leaves `used` untouched. */
    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (need < 0) {
        a->failures++;
        return NULL;
    }

    saved = a->used;
    p = arena_alloc(a, (size_t)need + 1, 1);
    if (p == NULL)
        return NULL;

    va_start(ap, fmt);
    wrote = vsnprintf(p, (size_t)need + 1, fmt, ap);
    va_end(ap);

    if (wrote < 0 || wrote > need) {
        /* Nothing else can have allocated in between, so rewinding is safe. */
        a->used = saved;
        a->failures++;
        return NULL;
    }
    return p;
}

void arena_reset(arena_t *a)
{
    if (a == NULL)
        return;

    if (a->used > a->peak)
        a->peak = a->used;
    a->used = 0;

    /* Deliberately not memset: zeroing 4 MB every cycle is exactly the cost
     * the arena exists to avoid. Callers must initialise what they allocate;
     * arena_calloc() is there for the ones that want zeroes. */
}

void arena_destroy(arena_t *a)
{
    if (a == NULL)
        return;

    free(a->base);
    memset(a, 0, sizeof *a);
}
