#ifndef GITHUB_H
#define GITHUB_H

#include <stddef.h>

#include "arena.h"
#include "state.h"

#define GH_MAX_LABELS 16

/*
 * One candidate issue. All strings are arena-allocated and live until the
 * cycle's arena_reset(). Pull requests never reach this struct -- they are
 * dropped during parse.
 */
typedef struct {
    long long id;
    int number;
    const char *repo;             /* "owner/repo" */
    const char *title;
    const char *body;             /* may be "" -- GitHub sends null bodies */
    const char *html_url;
    const char *author;           /* user.login */
    const char *updated_at;       /* RFC3339 */
    const char *labels[GH_MAX_LABELS];
    int n_labels;
    int comments;

    int kw_score;                 /* filled by prefilter_score() */
    int llm_score;                /* filled by judge_batch(), -1 until then */
    const char *why;              /* filled by judge_batch(), NULL until then */
} issue_t;

/*
 * Fetches every configured repo concurrently, honouring per-repo ETags and
 * since-watermarks. Appends parsed non-PR issues to an arena-allocated array.
 *
 * A 304 is a success path: nothing to parse, nothing to log, watermark
 * untouched. Watermarks are staged, not committed -- call gh_commit_watermarks()
 * only after the cycle (including notification) has fully succeeded.
 *
 * Returns 0 on success, negative on a setup failure. Per-repo transport errors
 * are logged and skipped, leaving that repo's state untouched.
 */
int gh_fetch_all(arena_t *a, state_t *st, const char *const *repos, size_t n_repos,
                 issue_t **out, size_t *n_out);

/* Promotes the watermarks staged by the last gh_fetch_all() into `st`. */
void gh_commit_watermarks(state_t *st);

/*
 * Parses one issues-array JSON payload. Exposed for the fixture tests.
 * `repo` tags each issue. Returns the number appended, negative on malformed
 * JSON. `newest_updated` receives the max updated_at seen (including PRs).
 */
int gh_parse_issues(arena_t *a, const char *json, size_t json_len, const char *repo,
                    issue_t *out, size_t out_cap, char *newest_updated, size_t nu_len);

#endif /* GITHUB_H */
