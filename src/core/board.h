#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <time.h>

#include "core/state.h"

/*
 * The ranked board: everything still open and still worth doing, carried across
 * cycles in board.tsv next to the etag cache.
 *
 * A push is an event and is gone once glanced at; the board is state and is
 * not. That is the whole reason this file exists -- a Tenstorrent bounty seen
 * at 06:00 is still unclaimed at 15:00, and the seen-set alone would have
 * buried it forever.
 *
 * Entries outlive the cycle arena, so they cannot live in it: board_open()
 * makes one calloc of BOARD_MAX slots and nothing ever reallocates. Same
 * discipline as state_t::repos -- startup allocation is permitted, hot-path
 * allocation is not.
 */

#define BOARD_TITLE_MAX 256
#define BOARD_URL_MAX   256
#define BOARD_WHY_MAX   128   /* > LLM_WHY_MAX; board.c static-asserts that */
#define BOARD_ETAG_MAX  128   /* == HTTP_ETAG_MAX; board.c static-asserts that */
#define BOARD_TIME_MAX  32    /* RFC3339 */

typedef struct {
    long long id;                       /* GitHub issue id; the merge key */
    char repo[STATE_REPO_MAX];
    int number;
    int llm_score;
    int kw_score;
    char first_seen[BOARD_TIME_MAX];    /* when it entered the board */
    char updated_at[BOARD_TIME_MAX];
    char etag[BOARD_ETAG_MAX];          /* per-issue, so a re-check costs nothing */
    char title[BOARD_TITLE_MAX];
    char why[BOARD_WHY_MAX];
    char html_url[BOARD_URL_MAX];
    int assigned;                       /* someone else holds it: drop it */
    int fresh;                          /* refreshed this cycle; NOT persisted */
} board_entry_t;

typedef struct {
    board_entry_t *entries;             /* BOARD_MAX slots, calloc'd at open */
    size_t n;
    size_t cap;
    int dirty;
    char dir[STATE_PATH_MAX];
} board_t;

/*
 * Allocates the slots and loads board.tsv from `dir` if it exists. A missing
 * file is an empty board, not an error; a malformed line is skipped, never
 * fatal -- same posture as load_etags(), for the same reason.
 */
int board_open(board_t *b, const char *dir);

/* Frees the slots. Does NOT flush: publishing decides that, not teardown. */
void board_close(board_t *b);

/*
 * Atomic rewrite of board.tsv (temp file, fsync, rename). No-op when clean.
 * Titles carry tabs, newlines, emoji and CJK, so title/why/html_url are escaped
 * on write and unescaped on read -- that round trip is the trap here.
 */
int board_flush(board_t *b);

/*
 * Upserts `in` by id. An existing entry keeps its first_seen and takes the new
 * score, title, updated_at and etag; a new entry is added with first_seen set
 * to `now_iso`. Ids already in the seen-set are skipped -- that is how an entry
 * dropped as assigned or closed stays gone instead of flapping back next cycle.
 * Every touched entry gets fresh = 1. *n_new, when non-NULL, receives the count
 * of entries that were not already on the board. Negative on a bad argument.
 */
int board_merge(board_t *b, const state_t *st, const board_entry_t *in, size_t n,
                const char *now_iso, size_t *n_new);

/* Removes the entry with `id`. Returns 1 if one went, 0 if there was none. */
int board_drop(board_t *b, long long id);

/*
 * llm_score desc, then first_seen desc so a fresh 8 outranks a stale 8, then id
 * asc as a deterministic tiebreak -- qsort is not stable, and a board that
 * reshuffles equal rows every cycle reads as noise.
 */
void board_rank(board_t *b);

/*
 * Drops entries whose first_seen is older than BOARD_STALE_DAYS. Safety net
 * only: an entry should leave because GitHub says closed or assigned. Returns
 * the number dropped.
 */
size_t board_expire(board_t *b, time_t now);

/* Clears every fresh flag. Called at the top of a cycle's merge. */
void board_clear_fresh(board_t *b);

#endif /* BOARD_H */
