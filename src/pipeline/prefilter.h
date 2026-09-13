#ifndef PREFILTER_H
#define PREFILTER_H

#include <stddef.h>

#include "core/arena.h"
#include "core/userconf.h"
#include "net/github.h"

/*
 * Hand-written Aho-Corasick over a keyword table. Built once at startup into
 * the permanent arena; matching allocates nothing.
 *
 * Phrases are the reason this is not a hash map: "good first issue",
 * "race condition" and "memory leak" all have to match as units.
 */

typedef struct ac_automaton ac_t;

/*
 * Builds the automaton from `kws`, which is config.h's KEYWORDS[] unless the
 * user's config file replaced it. 0 on success, negative on failure.
 *
 * The table is borrowed, not copied: it has to outlive the automaton, which
 * both the static default and userconf's arena-allocated copy do. Per-term
 * scratch for scoring is sized from `n_kws` here, so that scoring stays
 * allocation-free now that the count is not a compile-time constant.
 */
int prefilter_init(arena_t *perm, const kw_t *kws, size_t n_kws, ac_t **out);

/*
 * Scores title + body + labels in one pass:
 *   score = sum over terms of weight * min(count, KW_COUNT_CAP)
 * with label hits multiplied by KW_LABEL_MULTIPLIER, and label_only terms
 * counted in the label field alone. Matching is ASCII case-insensitive.
 */
int prefilter_score(const ac_t *ac, const issue_t *iss);

/*
 * Scores in place and compacts the array to those with kw_score >= KW_SCORE_MIN.
 * Returns the surviving count.
 */
size_t prefilter_apply(const ac_t *ac, issue_t *issues, size_t n);

/* Scores a bare string against the automaton. For tests. */
int prefilter_score_text(const ac_t *ac, const char *text, int is_label);

#endif /* PREFILTER_H */
