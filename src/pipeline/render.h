#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <time.h>

#include "core/arena.h"
#include "core/board.h"

/*
 * board_t -> Markdown. Policy, no I/O: what the board looks like is decided
 * here, where it goes is net/gist.c's problem.
 *
 * A gist renders Markdown and does not serve HTML, so this emits Markdown and
 * nothing else. Emoji is fine in a body -- CLAUDE.md rule 5 constrains ntfy
 * *headers* to ASCII, not this.
 */

/*
 * Renders the whole board into `a` and returns it, NUL-terminated. `now` dates
 * the header and drives the age column.
 *
 * Returns NULL when the arena is exhausted or the buffer would overflow. A
 * truncated board is worse than no publish at all -- it would read as "these
 * are the only open bounties" -- so the caller must treat NULL as "skip this
 * cycle's publish", never as "publish what we have".
 */
const char *render_board(arena_t *a, const board_t *b, time_t now);

/*
 * "2h", "3d", "just now" -- a compact age from two RFC3339 stamps. Exposed for
 * the golden-output test, which must not depend on the wall clock.
 */
int render_age(char *dst, size_t dstlen, const char *first_seen, time_t now);

#endif /* RENDER_H */
