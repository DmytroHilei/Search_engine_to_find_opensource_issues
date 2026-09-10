#ifndef JUDGE_H
#define JUDGE_H

#include <stddef.h>

#include "core/arena.h"
#include "net/github.h"

/*
 * LLM scoring. One backend is selected at compile time by JUDGE_MODE; all of
 * them sit behind judge_batch().
 *
 * Output is schema-constrained (Ollama "format", Anthropic tool definition) so
 * the reply is always parseable JSON. A batch whose reply does not parse is
 * dropped and logged -- never retried in a loop, never regexed out of prose.
 */

/* Validates that the selected backend's credentials exist. Negative on failure. */
int judge_init(void);

/*
 * Scores `issues` in groups of LLM_BATCH_SIZE, filling llm_score and why.
 * Issues in a dropped batch keep llm_score = -1 and are discarded by the caller.
 * Returns 0 when at least one batch succeeded, negative when all failed.
 */
int judge_batch(arena_t *a, issue_t *issues, size_t n);

/*
 * Compacts `issues` to those with llm_score >= LLM_SCORE_MIN and sorts them by
 * score descending. Returns the surviving count.
 */
size_t judge_apply(issue_t *issues, size_t n);

/*
 * Truncates `body` to LLM_BODY_TRUNC keeping LLM_BODY_HEAD leading bytes and the
 * tail, with an elision marker between -- the tail usually holds the actual
 * question, the middle is a 40 KB stack trace. Arena-allocated result.
 * Exposed for tests.
 */
const char *judge_truncate_body(arena_t *a, const char *body);

/*
 * Parses a backend reply of the form
 *   [{"i":0,"keep":true,"score":7,"why":"..."}]
 * applying results to `issues` by index. Returns the number applied, negative
 * on malformed JSON. Exposed for the fixture tests.
 */
int judge_parse_verdicts(arena_t *a, const char *json, size_t json_len,
                         issue_t *issues, size_t n);

#endif /* JUDGE_H */
