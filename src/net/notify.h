#ifndef NOTIFY_H
#define NOTIFY_H

#include <stddef.h>

#include "core/arena.h"
#include "core/board.h"
#include "net/github.h"
#include "core/state.h"

/*
 * ntfy push. Headers must be pure ASCII -- GitHub titles carry emoji and CJK
 * constantly, and non-ASCII in a header silently truncates or 400s.
 */

/*
 * `topic` and `gist_id` come from the user's config file; NULL or "" keeps
 * config.h's value. Both must outlive the process -- the permanent arena does.
 * `gist_id` is here because the summary push carries a Click: link to the board.
 *
 * Fails when no real topic was set: an ntfy topic IS the authentication, so
 * there is no safe default to fall back to.
 */
int notify_init(const char *topic, const char *gist_id);

/*
 * Sends at most NOTIFY_MAX_PER_CYCLE issues, highest llm_score first, skipping
 * anything already in the seen-set and marking what it sends. Returns the number
 * sent, negative on a setup failure.
 *
 * With dry_run non-zero this prints to stdout and MUST NOT issue any HTTP
 * request. That invariant is asserted by the tests.
 */
int notify_cycle(arena_t *a, state_t *st, issue_t *issues, size_t n, int dry_run);

/*
 * One push for the whole cycle: how many entries are new, what the top one is,
 * and a Click: that opens the board. This is what NOTIFY_SUMMARY_ONLY buys --
 * the phone stops being a feed and becomes a doorbell.
 *
 * `n_new` is board_merge()'s count. Sends nothing and returns 0 when it is
 * zero: a cycle that found nothing is not worth a buzz. Returns 1 when a push
 * went out, negative on failure. Same --dry-run contract as notify_cycle().
 */
int notify_summary(arena_t *a, const board_t *b, size_t n_new, int dry_run);

/* Maps an llm_score (0..10) onto an ntfy Priority (1..5). */
int notify_priority(int llm_score);

#endif /* NOTIFY_H */
