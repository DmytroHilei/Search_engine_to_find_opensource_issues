#include "notify.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "http.h"
#include "util.h"

/*
 * ntfy push.
 *
 * Two invariants this file exists to hold:
 *   1. every header value is pure printable ASCII with no CR or LF -- GitHub
 *      titles are full of emoji and CJK, and a newline in a header value is
 *      header injection (CLAUDE.md rule 5);
 *   2. --dry-run never issues an HTTP request. The dry-run branch prints and
 *      returns before anything touches http.h, and notify_http_calls below lets
 *      the test assert that structurally rather than by inspection.
 */

/* Test hook, deliberately absent from notify.h: counts every http_perform_one()
 * this module attempts. tests/test_notify.c declares it extern. */
unsigned long notify_http_calls;

/* ntfy truncates long titles anyway, and a phone shows far less than this. */
#define NOTIFY_TITLE_MAX 250

/* Exposed (not in notify.h) so the sanitising test can drive the real thing. */
size_t notify_build_title(char *dst, size_t dstlen, const char *repo, const char *title);

static int notify_topic_is_placeholder(void)
{
    return strcmp(NTFY_TOPIC, "REPLACE_ME_WITH_RANDOM_HEX") == 0;
}

/*
 * ascii_sanitize() already folds every byte outside 0x20..0x7e -- CR and LF
 * included -- into a single space, so this should never fire. It stays because
 * header injection is not an invariant worth holding in another translation
 * unit, and the check costs one pass over 250 bytes.
 */
static void strip_crlf(char *s)
{
    for (; *s != '\0'; s++)
        if (*s == '\r' || *s == '\n')
            *s = ' ';
}

size_t notify_build_title(char *dst, size_t dstlen, const char *repo, const char *title)
{
    char raw[1024];
    size_t n;

    if (dst == NULL || dstlen == 0)
        return 0;
    snprintf(raw, sizeof raw, "[%s] %s",
             repo != NULL ? repo : "?",
             (title != NULL && title[0] != '\0') ? title : "(untitled)");
    n = ascii_sanitize(dst, dstlen, raw);
    strip_crlf(dst);
    return n;
}

/* Case-insensitive substring, ASCII only. Labels are ASCII in practice. */
static int label_has(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);

    if (hay == NULL)
        return 0;
    for (; *hay != '\0'; hay++)
        if (ascii_strncasecmp(hay, needle, nl) == 0)
            return 1;
    return 0;
}

/* ntfy tag names are emoji shortcodes; every one below is a literal, so the
 * header stays ASCII by construction. */
static void derive_tags(const issue_t *is, int prio, char *dst, size_t dstlen)
{
    const char *kind = "mag";
    int i;

    for (i = 0; i < is->n_labels && i < GH_MAX_LABELS; i++) {
        const char *l = is->labels[i];

        if (label_has(l, "bug") || label_has(l, "crash") || label_has(l, "segfault")) {
            kind = "bug";
            break;
        }
        if (label_has(l, "perf")) {
            kind = "zap";
            break;
        }
        if (label_has(l, "good first issue") || label_has(l, "help wanted")) {
            kind = "wave";
            break;
        }
    }
    snprintf(dst, dstlen, "%s%s", kind, prio >= 5 ? ",rocket" : "");
}

int notify_priority(int llm_score)
{
    /* 0..10 -> 1..5, clamped at both ends: a model that answers 47 must not
     * produce Priority 24. */
    static const int MAP[11] = { 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 };

    if (llm_score < 0)
        return MAP[0];
    if (llm_score > 10)
        return MAP[10];
    return MAP[llm_score];
}

static int notify_send(arena_t *a, const issue_t *is, int prio)
{
    char title[NOTIFY_TITLE_MAX + 1];
    char click[512];
    char tags[96];
    const char *hdrs[8];
    const char *tok, *body;
    http_req_t req;
    http_resp_t resp;
    size_t nh = 0, i;

    notify_build_title(title, sizeof title, is->repo, is->title);
    ascii_sanitize(click, sizeof click, is->html_url != NULL ? is->html_url : "");
    strip_crlf(click);
    derive_tags(is, prio, tags, sizeof tags);

    hdrs[nh++] = "Content-Type: text/plain; charset=utf-8";
    hdrs[nh++] = arena_printf(a, "Title: %s", title);
    hdrs[nh++] = arena_printf(a, "Priority: %d", prio);
    hdrs[nh++] = arena_printf(a, "Tags: %s", tags);
    /* A space in the sanitised URL means the original was not ASCII, so the
     * link would be wrong. Drop the header rather than send a broken Click. */
    if (click[0] != '\0' && strchr(click, ' ') == NULL)
        hdrs[nh++] = arena_printf(a, "Click: %s", click);
    hdrs[nh++] = "Markdown: yes";

    tok = env_or_null("NTFY_TOKEN");
    if (tok != NULL)                        /* self-hosted ntfy with auth only */
        hdrs[nh++] = arena_printf(a, "Authorization: Bearer %s", tok);
    hdrs[nh] = NULL;

    for (i = 0; i < nh; i++) {
        if (hdrs[i] == NULL) {
            LOGE("notify: arena exhausted building headers for %s#%d",
                 is->repo != NULL ? is->repo : "?", is->number);
            return -1;
        }
    }

    body = (is->why != NULL && is->why[0] != '\0') ? is->why : title;

    memset(&req, 0, sizeof req);
    req.url = arena_printf(a, "%s/%s", NTFY_SERVER, NTFY_TOPIC);
    if (req.url == NULL) {
        LOGE("notify: arena exhausted building the ntfy URL");
        return -1;
    }
    req.method      = "POST";
    req.headers     = hdrs;
    req.body        = body;
    req.body_len    = strlen(body);
    req.timeout_sec = NOTIFY_TIMEOUT_SEC;

    memset(&resp, 0, sizeof resp);
    notify_http_calls++;                    /* the --dry-run invariant hangs off this */
    if (http_perform_one(a, &req, &resp) < 0) {
        LOGW("notify: transport failure pushing %s#%d",
             is->repo != NULL ? is->repo : "?", is->number);
        return -1;
    }
    if (resp.status < 200 || resp.status >= 300) {
        LOGW("notify: ntfy replied %ld for %s#%d", resp.status,
             is->repo != NULL ? is->repo : "?", is->number);
        return -1;
    }
    return 0;
}

int notify_init(void)
{
    if (notify_topic_is_placeholder() || NTFY_TOPIC[0] == '\0') {
        /*
         * Hard failure, not a warning. A public ntfy.sh topic is world-readable
         * AND world-writable, so the shipped placeholder means anyone who reads
         * this repo can push to the user's phone. main() may choose to tolerate
         * this in --dry-run, where nothing is ever sent.
         */
        LOGE("notify: NTFY_TOPIC is still the placeholder. ntfy.sh topics are "
             "world-readable and world-writable -- set NTFY_TOPIC in src/config.h "
             "to `openssl rand -hex 16` output and rebuild.");
        return -1;
    }
    LOGI("notify: pushing to %s, at most %d per cycle", NTFY_SERVER, NOTIFY_MAX_PER_CYCLE);
    return 0;
}

int notify_cycle(arena_t *a, state_t *st, issue_t *issues, size_t n, int dry_run)
{
    size_t i;
    int sent = 0;

    if (a == NULL || st == NULL || (n > 0 && issues == NULL))
        return -1;

    /* `issues` arrives already compacted and sorted by judge_apply(), so the
     * first NOTIFY_MAX_PER_CYCLE unseen entries are the highest-scoring ones. */
    for (i = 0; i < n && sent < NOTIFY_MAX_PER_CYCLE; i++) {
        uint64_t key = state_key(issues[i].id, issues[i].updated_at);
        int prio = notify_priority(issues[i].llm_score);

        if (state_seen(st, key))
            continue;

        if (dry_run) {
            char title[NOTIFY_TITLE_MAX + 1];
            /*
             * Returns before any http call -- that is the whole point of the
             * branch, and tests/test_notify.c asserts notify_http_calls stays 0.
             *
             * Deliberately NOT state_mark_seen(): --dry-run exists to tune
             * keyword weights (CONTEXT.md 13), which takes several passes over
             * the same issues. Marking them here would make the first real run
             * silent about everything the user had only ever seen on stdout.
             *
             * The title goes through the same sanitiser as a real push, so what
             * is printed here is what the phone would have shown -- and one
             * candidate stays one line.
             */
            notify_build_title(title, sizeof title, issues[i].repo, issues[i].title);
            printf("[dry-run] prio=%d llm=%d kw=%d #%d\n"
                   "          %s\n"
                   "          why: %s\n"
                   "          %s\n",
                   prio, issues[i].llm_score, issues[i].kw_score, issues[i].number,
                   title,
                   issues[i].why != NULL ? issues[i].why : "(no reason)",
                   issues[i].html_url != NULL ? issues[i].html_url : "");
            sent++;
            continue;
        }

        /* Not marked on failure: a transport error should re-notify next cycle,
         * not vanish into the seen-set. */
        if (notify_send(a, &issues[i], prio) < 0)
            continue;
        state_mark_seen(st, key);
        sent++;
    }

    if (dry_run)
        fflush(stdout);
    return sent;
}
