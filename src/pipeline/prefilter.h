#ifndef PREFILTER_H
#define PREFILTER_H

#include <stddef.h>

#include "core/arena.h"
#include "net/github.h"

/*
 * Hand-written Aho-Corasick over the static KEYWORDS[] table. Built once at
 * startup into the permanent arena; matching allocates nothing.
 *
 * Phrases are the reason this is not a hash map: "good first issue",
 * "race condition" and "memory leak" all have to match as units.
 */

typedef struct ac_automaton ac_t;

/* Builds the automaton from KEYWORDS[]. 0 on success, negative on failure. */
int prefilter_init(arena_t *perm, ac_t **out);

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
