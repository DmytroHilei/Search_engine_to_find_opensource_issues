#include "net/github.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "yyjson.h"

#include "config.h"
#include "net/http.h"
#include "core/util.h"

/*
 * GitHub issue fetch.
 *
 * Two rules dominate the shape of this file and both are easy to get wrong:
 *
 *   1. Pull requests come back from /repos/{o}/{r}/issues too, and the only
 *      reliable discriminator is the presence of the `pull_request` KEY -- its
 *      value is sometimes JSON null. See gh_parse_issues().
 *   2. Neither the watermark nor the ETag may be persisted until the whole
 *      cycle, notification included, has succeeded. See the long comment above
 *      gh_commit_watermarks().
 */

#define GH_URL_MAX 1024

/*
 * Shared by the issue fetch and the board re-check so the two can never drift
 * into presenting different Accept or API-version headers to the same API.
 */
static const char *const gh_base_hdrs[] = {
    "Accept: application/vnd.github+json",
    "X-GitHub-Api-Version: 2022-11-28",
    "User-Agent: " GH_USER_AGENT,
};
#define GH_N_BASE_HDRS (sizeof gh_base_hdrs / sizeof gh_base_hdrs[0])

/*
 * Staged per-repo state, parallel to state_t::repos and indexed the same way.
 * Module-static because the commit happens in a separate call, after notify.
 */
static char g_stage_wm[STATE_REPO_MAX][32];
static char g_stage_etag[STATE_REPO_MAX][HTTP_ETAG_MAX];
static int  g_stage_valid[STATE_REPO_MAX];
/*
 * The backfill cursor stages separately from the watermark: a repo can have a
 * failed delta fetch and a clean sweep in the same cycle, or the reverse, and
 * collapsing them would let one failure discard the other's progress. Both are
 * still committed together, after the cycle has fully succeeded (rule 4).
 */
static int  g_stage_bf[STATE_REPO_MAX];
static int  g_stage_bf_round[STATE_REPO_MAX];
static int  g_stage_bf_valid[STATE_REPO_MAX];

/* Per-repo scratch for one gh_fetch_all() call. Lives in the cycle arena. */
typedef struct {
    size_t idx;                     /* index into st->repos */
    const char *repo;               /* "owner/repo", owned by state_t */
    int want_next;                  /* a further page was requested */
    int fetched;                    /* at least one 200 parsed cleanly */
    int no_stage;                   /* something went wrong -- do not advance */
    size_t n_kept;                  /* issues this repo has taken from the buffer */
    char next_url[GH_URL_MAX];
    char newest[32];                /* running max updated_at, seeded from state */
    char etag[HTTP_ETAG_MAX];       /* page-1 ETag; "" when absent */
    int have_etag;
} gh_ctx_t;

/* Bounded copy. Avoids snprintf's -Wformat-truncation noise on equal-sized bufs. */
static void str_copy(char *dst, size_t dstlen, const char *src)
{
    size_t n;

    if (dst == NULL || dstlen == 0)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstlen)
        n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Length to keep when cutting `s` to at most `max` bytes, moved back to a UTF-8
 * character boundary so the tail is never a half-written codepoint. A split
 * sequence would be re-serialised into the LLM request JSON later and yyjson
 * would reject the whole batch over one invalid continuation byte.
 */
static size_t utf8_trunc_len(const char *s, size_t len, size_t max)
{
    size_t cut, back, seq;
    unsigned char lead;

    if (len <= max)
        return len;

    /*
     * s[max] is the first byte we are dropping. If it is a continuation byte
     * (10xxxxxx) it belongs to a character that starts before the cut, so walk
     * back to that character's lead byte. A valid sequence is at most 4 bytes,
     * hence at most 3 steps.
     */
    cut = max;
    for (back = 0; back < 3 && cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80; back++)
        cut--;

    if (cut == max)
        return max;                     /* already on a boundary */
    if (((unsigned char)s[cut] & 0xC0) == 0x80)
        return max;                     /* not UTF-8 at all -- cut flat */

    lead = (unsigned char)s[cut];
    if (lead < 0x80)
        seq = 1;
    else if ((lead & 0xE0) == 0xC0)
        seq = 2;
    else if ((lead & 0xF0) == 0xE0)
        seq = 3;
    else if ((lead & 0xF8) == 0xF0)
        seq = 4;
    else
        return cut;                     /* invalid lead byte -- drop it */

    /* Keep the character only when all of it fits below the cap. */
    return cut + seq <= max ? cut + seq : cut;
}

/*
 * str_copy() for text that may be multi-byte. A GitHub title routinely exceeds
 * board_entry_t::title, and a flat cut lands mid-sequence often enough to be a
 * certainty rather than a risk: the clipped codepoint reaches gist.c, yyjson
 * refuses to encode invalid UTF-8, and the whole cycle's publish is dropped over
 * one truncated title. Every bounded copy into a board entry goes through here.
 */
static void utf8_copy(char *dst, size_t dstlen, const char *src)
{
    size_t n;

    if (dst == NULL || dstlen == 0)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    n = utf8_trunc_len(src, strlen(src), dstlen - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Returns the string value of `key`, or NULL when absent, null, or non-string. */
static const char *obj_str(yyjson_val *obj, const char *key, size_t *len_out)
{
    yyjson_val *v = yyjson_obj_get(obj, key);

    if (!yyjson_is_str(v))
        return NULL;
    if (len_out != NULL)
        *len_out = yyjson_get_len(v);
    return yyjson_get_str(v);
}

/*
 * Whether somebody already holds the issue.
 *
 * `assignees` is authoritative; `assignee` is the pre-2016 single-holder field
 * GitHub still emits beside it and is the only signal when a payload omits the
 * array. Both arrive as JSON null or [] constantly, and neither means assigned.
 *
 * Only a user object carrying a login counts, and anything malformed reads as
 * unassigned on purpose: the caller drops assigned issues, so inferring
 * "assigned" from junk would silently discard a bounty that was still open,
 * while erring the other way costs one conditional re-check next cycle.
 */
static int issue_is_assigned(yyjson_val *item)
{
    yyjson_val *arr = yyjson_obj_get(item, "assignees");

    if (yyjson_is_arr(arr)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(arr);
        yyjson_val *v;

        while ((v = yyjson_arr_iter_next(&it)) != NULL) {
            if (obj_str(v, "login", NULL) != NULL)
                return 1;
        }
        /* An empty array is GitHub stating "nobody" -- not a missing answer. */
        return 0;
    }

    return obj_str(yyjson_obj_get(item, "assignee"), "login", NULL) != NULL;
}

int gh_parse_issues(arena_t *a, const char *json, size_t json_len, const char *repo,
                    issue_t *out, size_t out_cap, char *newest_updated, size_t nu_len)
{
    yyjson_doc *doc = NULL;
    yyjson_val *root;
    yyjson_val *item;
    yyjson_arr_iter it;
    size_t n = 0;
    int rc;

    if (newest_updated != NULL && nu_len > 0)
        newest_updated[0] = '\0';

    if (a == NULL || json == NULL || repo == NULL || (out == NULL && out_cap > 0))
        return -EINVAL;

    /*
     * Not YYJSON_READ_INSITU: the payload is const here (and shared with the
     * caller's logging), and insitu would rewrite it in place.
     */
    doc = yyjson_read(json, json_len, YYJSON_READ_NOFLAG);
    if (doc == NULL) {
        rc = -EINVAL;
        goto out;
    }

    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        /* A bare object here is GitHub's error envelope, e.g. {"message":...}. */
        rc = -EINVAL;
        goto out;
    }

    it = yyjson_arr_iter_with(root);
    while ((item = yyjson_arr_iter_next(&it)) != NULL) {
        const char *title, *url, *upd, *body, *login;
        size_t title_len = 0, url_len = 0, upd_len = 0, body_len = 0, login_len = 0;
        yyjson_val *pr, *bodyv, *labels;
        issue_t *is;

        if (!yyjson_is_obj(item))
            continue;

        /*
         * Stop before touching newest_updated. Advancing the watermark past an
         * item we had no room to store would silently drop it forever.
         */
        if (n >= out_cap)
            break;

        upd = obj_str(item, "updated_at", &upd_len);
        title = obj_str(item, "title", &title_len);
        url = obj_str(item, "html_url", &url_len);

        /*
         * CLAUDE.md rule 1. Key presence, not truthiness: GitHub sends
         * "pull_request": null on some responses and that is still a PR.
         * yyjson_obj_get() returns the null value node, not NULL, for a key
         * that exists with a null value -- which is exactly what we need.
         */
        pr = yyjson_obj_get(item, "pull_request");

        /* Required fields missing -- drop the item without letting it count. */
        if (pr == NULL && (title == NULL || url == NULL || upd == NULL))
            continue;

        /*
         * GitHub emits fixed-width RFC3339 UTC ("2026-09-10T09:41:00Z"), so a
         * plain byte compare orders these correctly and no parsing is needed.
         * Pull requests deliberately count towards the max: a repo whose only
         * recent activity is PRs must still advance its watermark, or every
         * cycle refetches the identical page. Items we could not account for
         * do not count -- the watermark must never step over unprocessed work.
         */
        if (upd != NULL && newest_updated != NULL && nu_len > 0 && upd_len < nu_len &&
            strcmp(upd, newest_updated) > 0)
            str_copy(newest_updated, nu_len, upd);

        if (pr != NULL)
            continue;

        is = &out[n];
        memset(is, 0, sizeof *is);

        is->id = (long long)yyjson_get_sint(yyjson_obj_get(item, "id"));
        is->number = yyjson_get_int(yyjson_obj_get(item, "number"));
        is->comments = yyjson_get_int(yyjson_obj_get(item, "comments"));
        /* The board drops these: a bounty only pays whoever was assigned first. */
        is->assigned = issue_is_assigned(item);
        is->kw_score = 0;
        is->llm_score = -1;         /* prefilter/judge fill these in later */
        is->why = NULL;

        /* The yyjson doc dies at `out:`, so every string must be copied out. */
        bodyv = yyjson_obj_get(item, "body");
        if (yyjson_is_str(bodyv)) {
            body = yyjson_get_str(bodyv);
            body_len = yyjson_get_len(bodyv);
        } else {
            body = "";              /* GitHub sends null for an empty body */
            body_len = 0;
        }

        /*
         * GH_BODY_MAX is an arena-safety cap, not a prompt-size one, and the
         * distinction matters twice over.
         *
         * Why cap at all: bodies routinely carry 40 KB of stack traces or
         * nvidia-smi dumps. A few hundred of those exhaust a cycle's arena, and
         * the -ENOMEM path below then skips the repo without staging -- so the
         * next cycle refetches the identical oversized page and fails the same
         * way. That repo would never make progress again, silently. Truncating
         * keeps it live; the -ENOMEM path stays as the backstop for the cases
         * the cap does not cover.
         *
         * Why the cap is ~7x LLM_BODY_TRUNC and not equal to it: the prefilter
         * scans this text to compute kw_score, so cutting it down to the LLM's
         * 1200-byte budget here would silently change scoring for every long
         * issue. judge.c does its own, much tighter head/tail truncation on the
         * way into the prompt, where it belongs.
         */
        body_len = utf8_trunc_len(body, body_len, GH_BODY_MAX);

        login = obj_str(yyjson_obj_get(item, "user"), "login", &login_len);
        if (login == NULL) {
            login = "";
            login_len = 0;
        }

        is->repo = arena_strdup(a, repo);
        is->title = arena_strndup(a, title, title_len);
        is->html_url = arena_strndup(a, url, url_len);
        is->updated_at = arena_strndup(a, upd, upd_len);
        is->body = arena_strndup(a, body, body_len);
        is->author = arena_strndup(a, login, login_len);

        if (is->repo == NULL || is->title == NULL || is->html_url == NULL ||
            is->updated_at == NULL || is->body == NULL || is->author == NULL) {
            rc = -ENOMEM;
            goto out;
        }

        labels = yyjson_obj_get(item, "labels");
        if (yyjson_is_arr(labels)) {
            yyjson_arr_iter lit = yyjson_arr_iter_with(labels);
            yyjson_val *lv;

            while ((lv = yyjson_arr_iter_next(&lit)) != NULL) {
                const char *name;
                size_t name_len = 0;

                if (is->n_labels >= GH_MAX_LABELS)
                    break;          /* cap, not an error: labels are a long tail */
                name = obj_str(lv, "name", &name_len);
                if (name == NULL)
                    continue;
                is->labels[is->n_labels] = arena_strndup(a, name, name_len);
                if (is->labels[is->n_labels] == NULL) {
                    rc = -ENOMEM;
                    goto out;
                }
                is->n_labels++;
            }
        }

        n++;
    }

    rc = (int)n;

out:
    yyjson_doc_free(doc);
    return rc;
}

/*
 * WHY STAGING EXISTS, AND WHY THE ETAG IS STAGED WITH IT
 *
 * CLAUDE.md rule 4: a watermark may only advance once the repo's whole cycle
 * -- prefilter, judge, notify -- has succeeded. A crash between fetch and
 * notify must re-process, not skip. So gh_fetch_all() writes nothing into
 * state_t; it records the intended values here and main.c calls this after the
 * notification step.
 *
 * The ETag is the subtle half. It looks like a pure cache hint that is safe to
 * persist immediately, but it is not: If-None-Match is evaluated against the
 * *response body*, not against our watermark. Persist a fresh ETag while the
 * watermark stays behind and the next cycle gets a 304 for a page whose issues
 * were never notified -- the 304 tells us "nothing changed", we parse nothing,
 * and those issues are lost permanently rather than retried. ETag and
 * watermark must therefore move as one unit: both land here, or neither does.
 */
void gh_commit_watermarks(state_t *st)
{
    size_t i, n;

    if (st == NULL || st->repos == NULL)
        return;

    n = st->n_repos < STATE_REPO_MAX ? st->n_repos : STATE_REPO_MAX;
    for (i = 0; i < n; i++) {
        if (g_stage_bf_valid[i]) {
            st->repos[i].backfill_page  = g_stage_bf[i];
            st->repos[i].backfill_round = g_stage_bf_round[i];
            st->repos[i].dirty = 1;
            g_stage_bf_valid[i] = 0;
        }
        if (!g_stage_valid[i])
            continue;
        str_copy(st->repos[i].watermark, sizeof st->repos[i].watermark, g_stage_wm[i]);
        str_copy(st->repos[i].etag, sizeof st->repos[i].etag, g_stage_etag[i]);
        st->repos[i].dirty = 1;
        g_stage_valid[i] = 0;       /* a commit consumes the staging */
    }
}

/*
 * Picks the repo furthest behind on coverage: lexicographic minimum of
 * (backfill_round, backfill_page). Returns its index, or n on no candidate.
 *
 * Round before page is what keeps the rotation fair -- see repo_state_t. Ties
 * go to the lower index, which only matters on the very first cycle when every
 * repo is at (0, 0).
 */
static size_t backfill_pick(const state_t *st, size_t n)
{
    size_t i, best = n;

    for (i = 0; i < n; i++) {
        const repo_state_t *r = &st->repos[i];
        const repo_state_t *b;

        if (best == n) {
            best = i;
            continue;
        }
        b = &st->repos[best];
        if (r->backfill_round < b->backfill_round ||
            (r->backfill_round == b->backfill_round &&
             r->backfill_page < b->backfill_page))
            best = i;
    }
    return best;
}

/*
 * One rotation step of the rolling backfill: walks GH_BACKFILL_PAGES pages of
 * one repo's open+unassigned backlog and appends what it parses to `out`.
 *
 * Ordered created-ascending, not updated-descending. The cursor is a page
 * number, so the ordering underneath it has to be stable: sorted by update time
 * the pages reshuffle whenever anyone comments on anything, and a cursor of 7
 * would mean a different slice every cycle -- issues would be skipped and
 * re-read at random. Creation order never changes.
 *
 * assignee=none is server-side, so the sweep never spends a page on issues
 * gh_drop_assigned() would discard anyway. A short page means the backlog ran
 * out: the round increments and the cursor wraps to 1, which is what makes
 * coverage repeat rather than stop.
 *
 * Returns the number of issues appended. Any failure returns what it already
 * has and leaves the cursor unstaged, so the same pages are re-walked next
 * cycle -- re-reading a page is free, skipping one hides an issue for a whole
 * rotation.
 */
static size_t gh_backfill_sweep(arena_t *a, state_t *st, const char *auth,
                                issue_t *out, size_t cap)
{
    const char **hdrs;
    http_req_t *reqs;
    http_resp_t *resps;
    repo_state_t *rs;
    size_t n_repos, idx, n_req = 0, appended = 0, i, h;
    int first_page, short_page = 0, failed = 0;

    if (a == NULL || st == NULL || st->repos == NULL || out == NULL || cap == 0)
        return 0;

    n_repos = st->n_repos < STATE_REPO_MAX ? st->n_repos : STATE_REPO_MAX;
    idx = backfill_pick(st, n_repos);
    if (idx >= n_repos)
        return 0;

    rs = &st->repos[idx];
    first_page = rs->backfill_page > 0 ? rs->backfill_page : 1;
    /* Wrap before the wall rather than spending a request to be told about it. */
    if (first_page > GH_BACKFILL_LAST_PAGE)
        first_page = 1;

    /*
     * Coverage, not just this step. The sweep visits one repo per cycle, so
     * "has the backfill reached tinygrad yet" is a question about the whole
     * rotation and a per-cycle line cannot answer it. Without this the only way
     * to know was to read the etag file by hand.
     */
    {
        size_t swept = 0;

        for (i = 0; i < n_repos; i++)
            if (st->repos[i].backfill_round > 0)
                swept++;
        LOGI("backfill: sweeping %s from page %d (%zu/%zu repos fully swept "
             "at least once)", rs->repo, first_page, swept, n_repos);
    }

    hdrs  = arena_calloc(a, GH_N_BASE_HDRS + 2, sizeof *hdrs);
    reqs  = arena_calloc(a, GH_BACKFILL_PAGES, sizeof *reqs);
    resps = arena_calloc(a, GH_BACKFILL_PAGES, sizeof *resps);
    if (hdrs == NULL || reqs == NULL || resps == NULL) {
        LOGW("backfill: arena exhausted setting up the sweep of %s", rs->repo);
        return 0;
    }

    hdrs[0] = auth;
    for (h = 0; h < GH_N_BASE_HDRS; h++)
        hdrs[h + 1] = gh_base_hdrs[h];
    hdrs[GH_N_BASE_HDRS + 1] = NULL;

    for (i = 0; i < (size_t)GH_BACKFILL_PAGES; i++) {
        char *url = arena_printf(a,
            "%s/repos/%s/issues?state=open&assignee=none&sort=created"
            "&direction=asc&per_page=%d&page=%d",
            GH_API_BASE, rs->repo, GH_PER_PAGE, first_page + (int)i);

        if (url == NULL) {
            LOGW("backfill: arena exhausted building the %s page URLs", rs->repo);
            break;
        }
        reqs[n_req].url         = url;
        reqs[n_req].method      = "GET";
        reqs[n_req].headers     = hdrs;
        reqs[n_req].timeout_sec = GH_HTTP_TIMEOUT_SEC;
        n_req++;
    }
    if (n_req == 0)
        return 0;

    if (http_perform_batch(a, reqs, n_req, resps, (int)n_req) < 0) {
        LOGW("backfill: sweep of %s failed to run; cursor unchanged", rs->repo);
        return 0;
    }

    /*
     * In page order, so a short page is recognised as the end of the backlog
     * rather than as a hole. A failure stops the walk: pages after it cannot be
     * trusted to have been seen, and the cursor must not move past them.
     */
    for (i = 0; i < n_req; i++) {
        http_resp_t *r = &resps[i];
        char newest[32] = "";
        int parsed;

        if (r->status != 200) {
            /*
             * 422 is the offset-pagination wall, not an error to retry: GitHub
             * refuses `page=` past 10000 items. Treat it exactly like the end
             * of the backlog so the cursor wraps. Retrying would re-request the
             * same rejected page every cycle, and the repo would keep round 0
             * and so stay the pick, stalling the rotation for every repo.
             */
            if (r->status == 422) {
                LOGI("backfill: %s reached the offset-pagination limit at page "
                     "%d; wrapping (issues past it need the delta fetch)",
                     rs->repo, first_page + (int)i);
                short_page = 1;
                break;
            }
            if (r->status == 0)
                LOGW("backfill: %s page %d transport failure", rs->repo,
                     first_page + (int)i);
            else
                LOGW("backfill: %s page %d replied %ld", rs->repo,
                     first_page + (int)i, r->status);
            failed = 1;
            break;
        }
        if (r->rl_remaining >= 0 && r->rl_remaining < RL_RESERVE) {
            LOGW("backfill: rate-limit reserve reached (%ld left), stopping the sweep",
                 r->rl_remaining);
            failed = 1;
            break;
        }

        parsed = gh_parse_issues(a, r->body != NULL ? r->body : "",
                                 r->body != NULL ? r->body_len : 0, rs->repo,
                                 out + appended, cap - appended,
                                 newest, sizeof newest);
        if (parsed < 0) {
            LOGW("backfill: %s page %d unparseable (%d)", rs->repo,
                 first_page + (int)i, parsed);
            failed = 1;
            break;
        }

        appended += (size_t)parsed;
        if (appended >= cap) {
            LOGW("backfill: buffer full during the %s sweep; cursor unchanged",
                 rs->repo);
            failed = 1;
            break;
        }

        /*
         * End of the backlog is the absence of a `next` link, not a short
         * `parsed`. PRs are filtered out during parsing, so a perfectly full
         * page of 100 can parse to 40 issues -- treating that as the end would
         * wrap the cursor early and leave most of the backlog unvisited, which
         * is the exact bug this function exists to fix.
         */
        {
            char next_url[GH_URL_MAX];

            if (r->link[0] == '\0' ||
                http_link_next(r->link, next_url, sizeof next_url) != 1) {
                short_page = 1;
                break;
            }
        }
    }

    if (failed && appended == 0)
        return 0;

    /*
     * Staged, never written here: the cursor commits with the watermarks after
     * notification and publication have succeeded (rule 4). A failure part-way
     * leaves it alone entirely, so the sweep repeats rather than skips.
     */
    if (!failed && idx < STATE_REPO_MAX) {
        if (short_page) {
            g_stage_bf[idx]       = 1;
            g_stage_bf_round[idx] = rs->backfill_round + 1;
            LOGI("backfill: %s backlog complete (round %d), wrapping",
                 rs->repo, g_stage_bf_round[idx]);
        } else {
            g_stage_bf[idx]       = first_page + (int)n_req;
            g_stage_bf_round[idx] = rs->backfill_round;
        }
        g_stage_bf_valid[idx] = 1;
    }

    LOGI("backfill: %s pages %d-%d, %zu issue(s)", rs->repo, first_page,
         first_page + (int)n_req - 1, appended);
    return appended;
}

/* Maps repos[i] onto its state_t slot, preferring the identity mapping. */
static repo_state_t *repo_slot(state_t *st, const char *const *repos, size_t i)
{
    if (i < st->n_repos && st->repos != NULL &&
        strcmp(st->repos[i].repo, repos[i]) == 0)
        return &st->repos[i];
    return state_repo(st, repos[i]);
}

int gh_fetch_all(arena_t *a, state_t *st, const char *const *repos, size_t n_repos,
                 issue_t **out, size_t *n_out)
{
    const char *const *base_hdrs = gh_base_hdrs;
    const size_t n_base = GH_N_BASE_HDRS;

    const char *token;
    const char *auth;
    const char **page_hdrs;
    gh_ctx_t *ctx = NULL;
    http_req_t *reqs = NULL;
    http_resp_t *resps = NULL;
    issue_t *issues = NULL;
    size_t cap, room, n_issues = 0, n_ctx = 0, n_req = 0, i, h;
    long rl_low = -1;
    int page, rc = 0, fatal = 0, stop = 0;

    if (out != NULL)
        *out = NULL;
    if (n_out != NULL)
        *n_out = 0;
    if (a == NULL || st == NULL || repos == NULL || out == NULL || n_out == NULL)
        return -EINVAL;

    /*
     * A new fetch invalidates anything a previous cycle staged but never
     * committed. Done before any early return so a stale staging can never be
     * committed on the back of a later cycle.
     */
    memset(g_stage_valid, 0, sizeof g_stage_valid);
    memset(g_stage_bf_valid, 0, sizeof g_stage_bf_valid);

    /* Secrets come from the environment only, and never reach a log line. */
    token = env_or_null("GH_TOKEN");
    if (token == NULL || token[0] == '\0') {
        LOGE("GH_TOKEN is unset or empty. Unauthenticated polling is capped at "
             "60 req/hr and will not work; export a fine-grained PAT with "
             "public-repo read access.");
        return -EACCES;
    }

    if (n_repos == 0)
        return 0;

    /*
     * A whole-cycle ceiling, no longer n_repos * GH_PER_PAGE. That old shape
     * made the buffer one shared pot, so tt-metal and pytorch drained it every
     * cycle and whatever was parsed after them got nothing -- which is how an
     * unassigned tinygrad bounty stayed invisible. GH_REPO_MAX_ISSUES below is
     * the per-repo share that stops that; this is only the arena guard.
     */
    cap = (size_t)GH_MAX_ISSUES_PER_CYCLE;
    issues = arena_calloc(a, cap, sizeof *issues);
    ctx = arena_calloc(a, n_repos, sizeof *ctx);
    reqs = arena_calloc(a, n_repos, sizeof *reqs);
    resps = arena_calloc(a, n_repos, sizeof *resps);
    auth = arena_printf(a, "Authorization: Bearer %s", token);
    page_hdrs = arena_calloc(a, n_base + 2, sizeof *page_hdrs);
    if (issues == NULL || ctx == NULL || reqs == NULL || resps == NULL ||
        auth == NULL || page_hdrs == NULL) {
        LOGE("arena exhausted while setting up the GitHub fetch");
        return -ENOMEM;
    }

    /* Shared header set for page 2+, which carries no If-None-Match of its own. */
    page_hdrs[0] = auth;
    for (h = 0; h < n_base; h++)
        page_hdrs[h + 1] = base_hdrs[h];
    page_hdrs[n_base + 1] = NULL;

    for (i = 0; i < n_repos; i++) {
        repo_state_t *rs = repo_slot(st, repos, i);
        gh_ctx_t *c;
        const char **hdrs;
        size_t idx;

        if (rs == NULL) {
            LOGW("%s: not present in the state file; skipping", repos[i]);
            continue;
        }
        idx = (size_t)(rs - st->repos);
        if (idx >= STATE_REPO_MAX || idx >= st->n_repos) {
            LOGW("%s: state slot out of range; skipping", repos[i]);
            continue;
        }

        c = &ctx[n_ctx];
        c->idx = idx;
        c->repo = rs->repo;
        /* Seed with the stored watermark so a staged value can never go backwards. */
        str_copy(c->newest, sizeof c->newest, rs->watermark);

        hdrs = arena_calloc(a, n_base + 3, sizeof *hdrs);
        if (hdrs == NULL) {
            LOGE("arena exhausted while building request headers");
            return -ENOMEM;
        }
        h = 0;
        hdrs[h++] = auth;
        for (size_t b = 0; b < n_base; b++)
            hdrs[h++] = base_hdrs[b];
        if (rs->etag[0] != '\0') {
            /* The whole point of the ETag cache: a 304 costs no rate-limit unit. */
            hdrs[h] = arena_printf(a, "If-None-Match: %s", rs->etag);
            if (hdrs[h] == NULL) {
                LOGE("arena exhausted while building request headers");
                return -ENOMEM;
            }
            h++;
        }
        hdrs[h] = NULL;

        if (rs->watermark[0] != '\0')
            reqs[n_req].url = arena_printf(a,
                "%s/repos/%s/issues?state=open&since=%s&sort=updated"
                "&direction=desc&per_page=%d",
                GH_API_BASE, rs->repo, rs->watermark, GH_PER_PAGE);
        else
            reqs[n_req].url = arena_printf(a,
                "%s/repos/%s/issues?state=open&sort=updated"
                "&direction=desc&per_page=%d",
                GH_API_BASE, rs->repo, GH_PER_PAGE);
        if (reqs[n_req].url == NULL) {
            LOGE("arena exhausted while building request URLs");
            return -ENOMEM;
        }

        reqs[n_req].method = "GET";
        reqs[n_req].headers = hdrs;
        reqs[n_req].body = NULL;
        reqs[n_req].body_len = 0;
        reqs[n_req].timeout_sec = GH_HTTP_TIMEOUT_SEC;
        reqs[n_req].user = c;
        n_req++;
        n_ctx++;
    }

    /*
     * Every repo goes out in a single batch so curl multiplexes them over one
     * HTTP/2 connection: one TLS handshake per cycle. Later rounds are the
     * pagination tail and are almost always empty.
     */
    for (page = 0; page < GH_MAX_PAGES && n_req > 0; page++) {
        rc = http_perform_batch(a, reqs, n_req, resps, HTTP_MAX_CONCURRENT);
        if (rc < 0) {
            LOGE("http_perform_batch failed (%d); no state will be advanced", rc);
            goto done;
        }

        for (i = 0; i < n_req; i++) {
            http_resp_t *r = &resps[i];
            gh_ctx_t *c = (gh_ctx_t *)r->user;
            char newest[32];
            const char *wm;
            int parsed;

            if (c == NULL)
                continue;
            c->want_next = 0;       /* re-armed below only if a next page exists */

            if (r->rl_remaining >= 0 && (rl_low < 0 || r->rl_remaining < rl_low))
                rl_low = r->rl_remaining;

            if (r->status == 0) {
                LOGW("%s: transport failure (curl %d: %s); state untouched",
                     c->repo, r->curl_err, r->err_msg != NULL ? r->err_msg : "?");
                continue;
            }

            switch (r->status) {
            case 200:
                break;
            case 304:
                /*
                 * CLAUDE.md rule 2: this is the success path, and the common
                 * one in steady state. Nothing to parse, nothing worth an INFO
                 * line, watermark and ETag left exactly as they are.
                 */
                LOGD("%s: 304 not modified", c->repo);
                continue;
            case 401:
                LOGE("%s: 401 Unauthorized -- GH_TOKEN is invalid, expired, or "
                     "lacks read access. Aborting the cycle; nothing will be "
                     "committed. Fix the token before the next run.", c->repo);
                fatal = 1;
                stop = 1;
                continue;
            case 403:
            case 429:
                /*
                 * CLAUDE.md rule 8. Secondary rate limits arrive as 403 with
                 * Retry-After. Honour it by dropping this repo for the cycle
                 * rather than retrying blind -- and do not sleep the whole
                 * process on one background repo's account. The next cycle is
                 * hours away and the stored ETag makes the retry nearly free.
                 */
                if (r->retry_after >= 0)
                    LOGW("%s: HTTP %ld with Retry-After %lds -- skipping this "
                         "repo for the cycle, state untouched",
                         c->repo, r->status, r->retry_after);
                else
                    LOGW("%s: HTTP %ld (rate limited or forbidden) -- skipping "
                         "this repo for the cycle, state untouched",
                         c->repo, r->status);
                stop = 1;           /* stop issuing new pages; do not hammer */
                continue;
            case 404:
                LOGW("%s: 404 -- deleted, renamed, or private to this token; "
                     "skipping", c->repo);
                continue;
            default:
                LOGW("%s: unexpected HTTP %ld -- skipping, state untouched",
                     c->repo, r->status);
                continue;
            }

            /* Page 1's ETag is the one If-None-Match must replay next cycle. */
            if (page == 0 && r->etag[0] != '\0') {
                str_copy(c->etag, sizeof c->etag, r->etag);
                c->have_etag = 1;
            }

            /*
             * Room is the smaller of what the cycle has left and what this repo
             * has left of its own share. The per-repo half is the fairness rule:
             * without it one busy repo spends the whole buffer.
             */
            room = cap - n_issues;
            if (c->n_kept < (size_t)GH_REPO_MAX_ISSUES) {
                size_t repo_room = (size_t)GH_REPO_MAX_ISSUES - c->n_kept;

                if (repo_room < room)
                    room = repo_room;
            } else {
                room = 0;
            }

            parsed = gh_parse_issues(a, r->body != NULL ? r->body : "",
                                     r->body != NULL ? r->body_len : 0, c->repo,
                                     issues + n_issues, room,
                                     newest, sizeof newest);
            if (parsed < 0) {
                LOGW("%s: unparseable issues payload (%d) -- skipping, state "
                     "untouched", c->repo, parsed);
                c->no_stage = 1;
                continue;
            }

            n_issues += (size_t)parsed;
            c->n_kept += (size_t)parsed;
            c->fetched = 1;
            if (newest[0] != '\0' && strcmp(newest, c->newest) > 0)
                str_copy(c->newest, sizeof c->newest, newest);

            if (n_issues == cap || c->n_kept >= (size_t)GH_REPO_MAX_ISSUES) {
                /*
                 * We dropped issues we could not store. Refusing to stage the
                 * watermark makes the next cycle re-see them (rule 4) instead
                 * of skipping them for good.
                 */
                LOGW("%s: hit the %s cap (%zu kept); not advancing the watermark "
                     "so the remainder is re-processed next cycle", c->repo,
                     n_issues == cap ? "cycle" : "per-repo", c->n_kept);
                c->no_stage = 1;
                continue;
            }

            /*
             * Early pagination stop (rule 3): with sort=updated&direction=desc
             * the first item at or below the watermark means every later page
             * is too. Otherwise ask http_link_next() -- the Link header is
             * walked, never regexed.
             */
            wm = st->repos[c->idx].watermark;
            if (parsed > 0 && wm[0] != '\0' &&
                strcmp(issues[n_issues - 1].updated_at, wm) <= 0)
                continue;
            if (r->link[0] != '\0' &&
                http_link_next(r->link, c->next_url, sizeof c->next_url) == 1)
                c->want_next = 1;
        }

        if (fatal)
            break;

        /* CLAUDE.md rule 8: stop spending requests once the reserve is reached. */
        if (rl_low >= 0 && rl_low < RL_RESERVE) {
            LOGW("GitHub rate budget down to %ld (reserve %d) -- stopping this "
                 "cycle early; remaining repos and pages are skipped",
                 rl_low, RL_RESERVE);
            break;
        }
        if (stop)
            break;

        n_req = 0;
        for (i = 0; i < n_ctx; i++) {
            if (!ctx[i].want_next || ctx[i].no_stage)
                continue;
            reqs[n_req].url = arena_strdup(a, ctx[i].next_url);
            if (reqs[n_req].url == NULL) {
                LOGW("arena exhausted; dropping the remaining pages");
                ctx[i].no_stage = 1;
                break;
            }
            reqs[n_req].method = "GET";
            reqs[n_req].headers = page_hdrs;
            reqs[n_req].body = NULL;
            reqs[n_req].body_len = 0;
            reqs[n_req].timeout_sec = GH_HTTP_TIMEOUT_SEC;
            reqs[n_req].user = &ctx[i];
            n_req++;
        }
        if (n_req > 0 && page + 1 >= GH_MAX_PAGES)
            LOGW("hit the %d page cap; the tail is left for the next cycle",
                 GH_MAX_PAGES);
    }

done:
    /*
     * Stage, never commit. On a fatal auth error stage nothing at all: the
     * cycle produced no trustworthy view of any repo.
     */
    if (!fatal) {
        for (i = 0; i < n_ctx; i++) {
            gh_ctx_t *c = &ctx[i];

            if (!c->fetched || c->no_stage)
                continue;
            str_copy(g_stage_wm[c->idx], sizeof g_stage_wm[c->idx], c->newest);
            if (c->have_etag)
                str_copy(g_stage_etag[c->idx], sizeof g_stage_etag[c->idx], c->etag);
            else
                str_copy(g_stage_etag[c->idx], sizeof g_stage_etag[c->idx],
                         st->repos[c->idx].etag);
            g_stage_valid[c->idx] = 1;
        }
    }

    /*
     * The rolling sweep runs last, on whatever the delta fetch left. It is
     * deliberately not gated on the delta succeeding: a repo whose delta failed
     * is exactly one whose backlog is worth walking, and the two advance
     * independent cursors.
     */
    if (n_issues < cap)
        n_issues += gh_backfill_sweep(a, st, auth, issues + n_issues, cap - n_issues);

    /*
     * Per-repo breakdown, at DEBUG so a healthy cycle stays three lines.
     *
     * Nothing used to say how the shared buffer was divided up, which is how a
     * starved repo looked identical to a quiet one: tinygrad contributing zero
     * issues because tt-metal had taken the whole pot read exactly like
     * tinygrad having nothing new. `304` is the same shape of invisible -- a
     * repo can be silent for a week and that is correct, but only if you can
     * see it was asked.
     */
    if (log_get_level() >= LOG_DEBUG) {
        for (i = 0; i < n_ctx; i++) {
            gh_ctx_t *c = &ctx[i];

            LOGD("  %-28s %3zu issue(s)%s%s", c->repo, c->n_kept,
                 c->fetched ? "" : " (not modified)",
                 c->no_stage ? " [capped, watermark held]" : "");
        }
    }
    if (rl_low >= 0)
        LOGI("github: rate limit %ld remaining (reserve %d)", rl_low, RL_RESERVE);

    *out = issues;
    *n_out = n_issues;

    if (rc < 0)
        return rc;
    if (fatal)
        return -EACCES;

    LOGI("github: %zu issue(s) from %zu repo(s)", n_issues, n_ctx);
    return 0;
}

/* --------------------------------------------------------------- the board */

size_t gh_drop_assigned(issue_t *issues, size_t n)
{
    size_t i, k = 0;

    if (issues == NULL)
        return 0;
    for (i = 0; i < n; i++)
        if (!issues[i].assigned)
            issues[k++] = issues[i];
    return k;
}

void gh_issue_to_board(const issue_t *is, board_entry_t *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof *out);
    if (is == NULL)
        return;

    out->id        = is->id;
    out->number    = is->number;
    out->llm_score = is->llm_score;
    out->kw_score  = is->kw_score;
    out->assigned  = is->assigned;

    /*
     * Every one of these can overrun its slot -- titles most of all -- so all of
     * them go through utf8_copy() rather than snprintf. A repo name is ASCII and
     * a timestamp is fixed-width, but routing them through the same helper costs
     * nothing and removes the question of which fields were safe to cut flat.
     */
    utf8_copy(out->repo, sizeof out->repo, is->repo);
    utf8_copy(out->title, sizeof out->title, is->title);
    utf8_copy(out->html_url, sizeof out->html_url, is->html_url);
    utf8_copy(out->why, sizeof out->why, is->why);
    utf8_copy(out->updated_at, sizeof out->updated_at, is->updated_at);

    /*
     * first_seen belongs to board_merge(): only the board knows whether this id
     * is arriving for the first time or is an existing row being re-scored, and
     * stamping it here would reset the age of everything every cycle.
     *
     * etag stays empty on purpose. There is no per-issue ETag in a list payload;
     * gh_recheck() records the one the first conditional GET returns, and until
     * then that GET simply costs a rate-limit unit.
     */
}

/*
 * THE RE-CHECK PASS
 *
 * The main fetch is state=open&since=<watermark>: deltas only, so nothing in it
 * would ever report that an issue closed or that somebody took it. Without a
 * second pass the board rots into a list of bounties already being paid to
 * someone else. Flipping the main fetch to state=all was rejected -- pytorch
 * alone closes enough issues to blow past both the issue cap and the arena, to
 * learn one bit about at most BOARD_MAX rows.
 *
 * The asymmetry in the drop table is deliberate and is the whole design: a wrong
 * "keep" costs one conditional GET next cycle, while a wrong "drop" silently
 * deletes a live bounty. So only an unambiguous answer from GitHub drops a row;
 * every failure, timeout and unexpected status keeps it.
 */

/*
 * Requests per round. One round is one curl_multi, so a small round buys
 * fine-grained rate-limit checks at the price of re-handshaking TLS. Rate limit
 * can only be read from a response, so the round size is exactly how far past
 * RL_RESERVE this pass can overshoot -- 50 against a 5000/hr budget and a
 * reserve of 100 is well inside the noise, and caps a full board at 4 rounds.
 */
#define GH_RECHECK_ROUND 50

/*
 * Exposed but deliberately absent from github.h: the drop table is the risky
 * part of this increment and it has to be reachable from tests/test_github.c
 * without a network. Same precedent as notify_build_title() in notify.c.
 *
 * Returns 1 when the entry must leave the board, 0 when it stays. On the
 * refresh path it updates `e` in place; on every keep path it leaves `e`
 * untouched, which is what makes a transport failure a no-op.
 */
int gh_recheck_apply(const http_resp_t *r, board_entry_t *e);

int gh_recheck_apply(const http_resp_t *r, board_entry_t *e)
{
    yyjson_doc *doc = NULL;
    yyjson_val *root;
    const char *state, *title, *upd;
    int drop = 0;

    if (r == NULL || e == NULL)
        return 0;

    /*
     * status 0 is a transport failure -- DNS, timeout, a dropped connection.
     * Keeping is not politeness here: a laptop that closed its lid mid-cycle
     * would otherwise wake up and empty the entire board in one pass.
     */
    if (r->status == 0)
        return 0;
    if (r->status == 304)
        return 0;                       /* rule 2: unchanged, and free */
    if (r->status == 404)
        return 1;                       /* deleted or transferred to another repo */
    if (r->status != 200)
        return 0;                       /* 403/429/5xx: no verdict, so no change */

    doc = yyjson_read(r->body != NULL ? r->body : "", r->body_len, YYJSON_READ_NOFLAG);
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        /* A 200 we cannot parse is not GitHub saying "closed". Keep it. */
        yyjson_doc_free(doc);
        return 0;
    }

    /*
     * Anything that is not the string "open" is closed as far as the board is
     * concerned. A missing or non-string `state` reads as open on purpose: the
     * conservative direction is the one that costs a re-check, not a bounty.
     */
    state = obj_str(root, "state", NULL);
    if (state != NULL && strcmp(state, "open") != 0)
        drop = 1;
    if (issue_is_assigned(root)) {
        e->assigned = 1;                /* recorded even though the row is going */
        drop = 1;
    }

    /*
     * updated_at is refreshed even on the drop path: the caller turns it into
     * the seen-set key, and under NOTIFY_ON_UPDATE that key is per-version, so
     * marking the stale one would let the next fetch re-add the row.
     */
    upd = obj_str(root, "updated_at", NULL);
    if (upd != NULL)
        utf8_copy(e->updated_at, sizeof e->updated_at, upd);

    if (!drop) {
        title = obj_str(root, "title", NULL);
        if (title != NULL)
            utf8_copy(e->title, sizeof e->title, title);
        /* Store the ETag so every later re-check of this row is a free 304. */
        if (r->etag[0] != '\0')
            str_copy(e->etag, sizeof e->etag, r->etag);
        e->assigned = 0;
    }

    /* Scores are the judge's; a re-check never touches llm_score or kw_score. */

    yyjson_doc_free(doc);
    return drop;
}

int gh_recheck(arena_t *a, board_t *b, state_t *st, int dry_run)
{
    const char *token, *auth;
    http_req_t *reqs = NULL;
    http_resp_t *resps = NULL;
    board_entry_t **todo = NULL;
    long long *drop_ids = NULL;
    size_t n_todo = 0, n_drop = 0, done = 0, i;
    int stop = 0, checked = 0, dropped = 0;

    if (a == NULL || b == NULL || b->entries == NULL || st == NULL)
        return -EINVAL;
    if (b->n == 0)
        return 0;

    /*
     * Only rows this cycle's fetch did not already refresh. A fresh row was in
     * the delta minutes ago; spending a request to ask again would double the
     * cost of the pass for nothing.
     */
    todo = arena_calloc(a, b->n, sizeof *todo);
    drop_ids = arena_calloc(a, b->n, sizeof *drop_ids);
    reqs = arena_calloc(a, b->n, sizeof *reqs);
    resps = arena_calloc(a, b->n, sizeof *resps);
    if (todo == NULL || drop_ids == NULL || reqs == NULL || resps == NULL) {
        LOGE("arena exhausted while setting up the board re-check");
        return -ENOMEM;
    }

    for (i = 0; i < b->n; i++) {
        board_entry_t *e = &b->entries[i];

        if (e->fresh || e->id <= 0)
            continue;
        if (e->number <= 0 || e->repo[0] == '\0') {
            /* No addressable issue URL. Nothing to ask, so nothing to conclude. */
            LOGW("board: entry %lld has no repo/number to re-check; keeping", e->id);
            continue;
        }
        todo[n_todo++] = e;
    }
    if (n_todo == 0)
        return 0;

    token = env_or_null("GH_TOKEN");
    if (token == NULL || token[0] == '\0') {
        LOGE("GH_TOKEN is unset or empty; the board cannot be re-checked. Every "
             "entry is kept as it stands.");
        return -EACCES;
    }

    auth = arena_printf(a, "Authorization: Bearer %s", token);
    if (auth == NULL) {
        LOGE("arena exhausted while building the re-check auth header");
        return -ENOMEM;
    }

    /*
     * Rounds rather than one 200-request batch, so RL_RESERVE can actually be
     * honoured: the remaining budget only ever arrives on a response.
     */
    while (done < n_todo && !stop) {
        size_t n_req = 0, base = done;
        long rl_low = -1;
        int rc;

        while (n_req < GH_RECHECK_ROUND && done < n_todo) {
            board_entry_t *e = todo[done];
            const char **hdrs;
            size_t h = 0, k;

            hdrs = arena_calloc(a, GH_N_BASE_HDRS + 3, sizeof *hdrs);
            if (hdrs == NULL) {
                LOGE("arena exhausted while building re-check headers");
                stop = 1;
                break;
            }
            hdrs[h++] = auth;
            for (k = 0; k < GH_N_BASE_HDRS; k++)
                hdrs[h++] = gh_base_hdrs[k];
            if (e->etag[0] != '\0') {
                /* The point of storing a per-issue ETag: this reply is a 304 and
                 * costs no rate-limit unit at all. */
                hdrs[h] = arena_printf(a, "If-None-Match: %s", e->etag);
                if (hdrs[h] == NULL) {
                    LOGE("arena exhausted while building re-check headers");
                    stop = 1;
                    break;
                }
                h++;
            }
            hdrs[h] = NULL;

            reqs[n_req].url = arena_printf(a, "%s/repos/%s/issues/%d",
                                           GH_API_BASE, e->repo, e->number);
            if (reqs[n_req].url == NULL) {
                LOGE("arena exhausted while building re-check URLs");
                stop = 1;
                break;
            }
            reqs[n_req].method = "GET";
            reqs[n_req].headers = hdrs;
            reqs[n_req].body = NULL;
            reqs[n_req].body_len = 0;
            reqs[n_req].timeout_sec = GH_HTTP_TIMEOUT_SEC;
            reqs[n_req].user = e;
            n_req++;
            done++;
        }

        if (n_req == 0)
            break;

        rc = http_perform_batch(a, reqs, n_req, resps, HTTP_MAX_CONCURRENT);
        if (rc < 0) {
            /*
             * A setup failure means this round produced no verdicts at all. The
             * rows it covered are kept, exactly as a per-request failure would
             * leave them, and the pass gives up rather than retrying blind.
             */
            LOGW("board re-check: http_perform_batch failed (%d); %zu entr(ies) "
                 "kept unchecked", rc, n_todo - base);
            break;
        }

        for (i = 0; i < n_req; i++) {
            http_resp_t *r = &resps[i];
            board_entry_t *e = (board_entry_t *)r->user;

            if (e == NULL)
                continue;
            checked++;

            if (r->rl_remaining >= 0 && (rl_low < 0 || r->rl_remaining < rl_low))
                rl_low = r->rl_remaining;
            if ((r->status == 403 || r->status == 429) && r->retry_after >= 0) {
                /* CLAUDE.md rule 8. The next cycle is hours away and the stored
                 * ETags make the retry nearly free, so back off rather than
                 * sleeping the whole single-threaded process here. */
                LOGW("board re-check: HTTP %ld with Retry-After %lds -- stopping "
                     "the pass, remaining entries kept", r->status, r->retry_after);
                stop = 1;
            }

            if (r->status == 0)
                LOGD("board re-check: %s#%d transport failure (curl %d); kept",
                     e->repo, e->number, r->curl_err);

            if (!gh_recheck_apply(r, e)) {
                if (r->status == 200)
                    b->dirty = 1;       /* title/etag/updated_at may have moved */
                continue;
            }

            /*
             * Deferred, not applied here: board_drop() compacts the array, and
             * the pending requests in this and later rounds hold pointers into
             * it. Collecting ids and dropping once at the end keeps every one of
             * those pointers valid without a second index to maintain.
             */
            drop_ids[n_drop++] = e->id;
            LOGD("board re-check: dropping %s#%d (HTTP %ld%s)", e->repo, e->number,
                 r->status, e->assigned ? ", assigned" : "");

            if (!dry_run) {
                /*
                 * The seen-set is what stops an assigned issue from being
                 * re-added by the next fetch and dropped again by the next
                 * re-check, flapping on and off the board forever. Skipped under
                 * --dry-run: the set is mmap'd, so marking it writes to disk, and
                 * a dry run writes nothing (same reason notify.c does not mark).
                 */
                state_mark_seen(st, state_key(e->id, e->updated_at));
            }
        }

        /* CLAUDE.md rule 8: stop spending requests once the reserve is reached. */
        if (rl_low >= 0 && rl_low < RL_RESERVE) {
            LOGW("board re-check: GitHub rate budget down to %ld (reserve %d) -- "
                 "stopping; %zu entr(ies) kept unchecked",
                 rl_low, RL_RESERVE, n_todo - done);
            stop = 1;
        }
    }

    for (i = 0; i < n_drop; i++)
        dropped += board_drop(b, drop_ids[i]);

    if (checked > 0)
        LOGI("board re-check: %d checked, %d dropped, %zu remain", checked, dropped,
             b->n);
    return dropped;
}
