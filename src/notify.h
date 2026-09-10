#ifndef NOTIFY_H
#define NOTIFY_H

#include <stddef.h>

#include "arena.h"
#include "github.h"
#include "state.h"

/*
 * ntfy push. Headers must be pure ASCII -- GitHub titles carry emoji and CJK
 * constantly, and non-ASCII in a header silently truncates or 400s.
 */

int notify_init(void);

/*
 * Sends at most NOTIFY_MAX_PER_CYCLE issues, highest llm_score first, skipping
 * anything already in the seen-set and marking what it sends. Returns the number
 * sent, negative on a setup failure.
 *
 * With dry_run non-zero this prints to stdout and MUST NOT issue any HTTP
 * request. That invariant is asserted by the tests.
 */
int notify_cycle(arena_t *a, state_t *st, issue_t *issues, size_t n, int dry_run);

/* Maps an llm_score (0..10) onto an ntfy Priority (1..5). */
int notify_priority(int llm_score);

#endif /* NOTIFY_H */
