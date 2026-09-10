#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/*
 * Bump allocator. One arena lives for the whole process (automaton, state
 * tables), one is reset at the end of every poll cycle. Nothing is ever freed
 * individually.
 */

typedef struct {
    unsigned char *base;
    size_t cap;
    size_t used;
    size_t peak;      /* high-water mark across resets, for logging */
    size_t failures;  /* allocations refused because the arena was full */
} arena_t;

/* 0 on success, negative errno on failure. */
int arena_init(arena_t *a, size_t cap);

/*
 * Returns NULL when the arena is exhausted -- callers must check. `align` must
 * be a power of two; pass 0 for the default (alignof(max_align_t)).
 */
void *arena_alloc(arena_t *a, size_t size, size_t align);

/* Zero-filled variant. Returns NULL on exhaustion. */
void *arena_calloc(arena_t *a, size_t count, size_t size);

/* Copies at most `n` bytes and NUL-terminates. NULL on exhaustion. */
char *arena_strndup(arena_t *a, const char *s, size_t n);

/* Copies a NUL-terminated string. NULL on exhaustion or if `s` is NULL. */
char *arena_strdup(arena_t *a, const char *s);

/* printf into the arena. NULL on exhaustion. */
char *arena_printf(arena_t *a, const char *fmt, ...);

/* Rewinds to empty. Memory is retained, not returned to the OS. */
void arena_reset(arena_t *a);

void arena_destroy(arena_t *a);

#endif /* ARENA_H */
