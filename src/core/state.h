#ifndef STATE_H
#define STATE_H

#include <stddef.h>
#include <stdint.h>

#include "net/http.h"

/*
 * Two files under $XDG_STATE_HOME/issuewatch/ (falling back to ~/.local/state):
 *   etags     - "owner/repo\tETag\twatermark" per line, rewritten atomically
 *   seen.bin  - fixed-size mmap'd open-addressing set of u64 keys
 */

#define STATE_REPO_MAX 128
#define STATE_PATH_MAX 512

typedef struct {
    char repo[STATE_REPO_MAX];
    char etag[HTTP_ETAG_MAX];
    char watermark[32];           /* RFC3339, "" when never fetched */
    /*
     * Next page of this repo's open+unassigned backlog for the rolling backfill
     * to walk, 1-based. 0 means "not started". It only resets to 1 when a sweep
     * runs off the end of the backlog, which is what makes the coverage
     * eventually repeat rather than stop.
     *
     * Separate from `watermark` on purpose: the watermark tracks recency and
     * only moves forward, while this tracks coverage and wraps.
     */
    int backfill_page;
    /*
     * Completed sweeps of this repo's backlog. Selection is (round, page)
     * lexicographic, and this field is the half that keeps it fair: page alone
     * would mean ROCm/composable_kernel, whose whole backlog is one page, wraps
     * to 1 immediately and is then permanently the minimum -- swept every cycle
     * while tt-metal sits at page 17 and is never chosen again.
     */
    int backfill_round;
    int dirty;
} repo_state_t;

typedef struct {
    repo_state_t *repos;          /* one entry per REPOS[], in that order */
    size_t n_repos;
    uint64_t *seen;               /* mmap'd, SEEN_CAPACITY slots */
    int seen_fd;
    /*
     * Set for --dry-run. state_open() marks every freshly seeded repo dirty, so
     * without this the teardown flush writes an etag file that the cycle itself
     * carefully refused to write -- a dry run would leave watermarks behind
     * anyway, just via a different path.
     */
    int read_only;
    char dir[STATE_PATH_MAX];
} state_t;

/*
 * Creates the state directory, loads the etag file, maps seen.bin. Repos absent
 * from the file get an empty ETag and a GH_FIRST_RUN_LOOKBACK watermark.
 * `repos` is the compile-time REPOS[] array.
 */
int state_open(state_t *st, const char *const *repos, size_t n_repos);

/* Flushes if dirty, unmaps, closes. */
int state_close(state_t *st);

/*
 * Lookup by "owner/repo". NULL when the repo is not in the configured list.
 *
 * Guaranteed to return a pointer INTO st->repos (never a copy), so callers may
 * recover the slot index as `rs - st->repos`. github.c relies on this to index
 * its parallel staging arrays; any reimplementation must preserve it.
 */
repo_state_t *state_repo(state_t *st, const char *repo);

/*
 * Atomic rewrite: temp file in the same directory, fsync, rename(2). A power
 * cut must never leave a truncated etag cache.
 */
int state_flush(state_t *st);

/* The dedup key. NOTIFY_ON_UPDATE decides whether updated_at participates. */
uint64_t state_key(long long issue_id, const char *updated_at);

/* 1 if the key has been notified before, 0 otherwise. */
int state_seen(const state_t *st, uint64_t key);

/* Inserts, overwriting the resident key on collision. Never grows. */
void state_mark_seen(state_t *st, uint64_t key);

#endif /* STATE_H */
