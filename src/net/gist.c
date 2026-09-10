#include "net/gist.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "yyjson.h"

#include "config.h"
#include "net/http.h"
#include "core/util.h"

/*
 * Gist publish: PATCH /gists/{id} with the rendered board.
 *
 * Two invariants this file exists to hold:
 *   1. the request body is built by yyjson, never by concatenation. The board
 *      is Markdown built from issue titles -- quotes, backslashes, newlines,
 *      pipe tables, emoji and CJK -- and hand-rolled escaping of that is a
 *      guaranteed corruption bug, silent right up until a title contains a `"`;
 *   2. --dry-run issues no HTTP request. The dry-run branch prints and returns
 *      before anything touches http.h, and gist_http_calls below lets the test
 *      assert that structurally rather than by inspection.
 */

/* Test hook, deliberately absent from gist.h: counts every http_perform_one()
 * this module attempts. tests/test_gist.c declares it extern. */
unsigned long gist_http_calls;

/* Envelope, key strings and yyjson's own bookkeeping. Dwarfed by the content. */
#define GIST_POOL_OVERHEAD (8u * 1024u)

/*
 * A board is BOARD_MAX rows of bounded fields, so a few hundred KB is already
 * far past anything render_board() can emit. The bound is here so an absurd
 * argument fails as an error rather than as a pool size computation.
 */
#define GIST_MD_MAX (4u * 1024u * 1024u)

/* Exposed (not in gist.h) so the escaping test can drive the real builder. */
const char *gist_build_body(arena_t *a, const char *markdown, size_t *len_out);

static int gist_id_is_placeholder(void)
{
    return strcmp(GIST_ID, "REPLACE_ME_WITH_GIST_ID") == 0;
}

/*
 * Builds {"files":{"<GIST_FILENAME>":{"content":"<markdown>"}}}.
 *
 * Returns an arena-backed, NUL-terminated JSON string, or NULL. NULL means drop
 * the publish -- never a truncated board, because a half-written gist reads as a
 * complete one and would quietly claim bounties had vanished.
 */
const char *gist_build_body(arena_t *a, const char *markdown, size_t *len_out)
{
    yyjson_alc alc;
    yyjson_mut_doc *doc;
    yyjson_mut_val *root, *files, *file;
    void *pool;
    size_t md_len, pool_bytes, body_len;
    char *body;

    if (len_out != NULL)
        *len_out = 0;
    if (a == NULL || markdown == NULL)
        return NULL;

    md_len = strlen(markdown);
    if (md_len > GIST_MD_MAX) {
        LOGE("gist: refusing to publish a %zu byte board", md_len);
        return NULL;
    }

    /*
     * Six bytes out per byte in is the worst case JSON string escaping can
     * reach (\u00XX for a control character), so a legitimate board can never
     * fail to encode for want of scratch. The arena is 96 MB; this is noise.
     */
    pool_bytes = md_len * 6u + GIST_POOL_OVERHEAD;
    pool = arena_alloc(a, pool_bytes, 16);
    if (pool == NULL || !yyjson_alc_pool_init(&alc, pool, pool_bytes)) {
        LOGE("gist: no room in the cycle arena for a %zu byte JSON scratch pool",
             pool_bytes);
        return NULL;
    }

    /*
     * No yyjson_mut_doc_free(): the doc and the rendered body both live in the
     * arena-backed pool above and are released with the cycle.
     */
    doc = yyjson_mut_doc_new(&alc);
    if (doc == NULL)
        return NULL;

    root  = yyjson_mut_obj(doc);
    files = yyjson_mut_obj(doc);
    file  = yyjson_mut_obj(doc);
    if (root == NULL || files == NULL || file == NULL)
        return NULL;

    /* Non-copying adds: the filename is a literal and `markdown` outlives the
     * request, so the pool holds structure only until the body is written. */
    if (!yyjson_mut_obj_add_str(doc, file, "content", markdown) ||
        !yyjson_mut_obj_add_val(doc, files, GIST_FILENAME, file) ||
        !yyjson_mut_obj_add_val(doc, root, "files", files))
        return NULL;
    yyjson_mut_doc_set_root(doc, root);

    body = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, &alc, &body_len, NULL);
    if (body == NULL) {
        /* Also the path an invalid UTF-8 sequence takes: yyjson refuses to
         * encode one, and a mangled board is worse than a skipped publish. */
        LOGE("gist: the board did not encode into the %zu byte JSON scratch pool",
             pool_bytes);
        return NULL;
    }

    if (len_out != NULL)
        *len_out = body_len;
    return body;
}

int gist_init(void)
{
    if (gist_id_is_placeholder() || GIST_ID[0] == '\0') {
        LOGE("gist: GIST_ID is still the placeholder. Create the gist once with "
             "`gh gist create --secret -d issuewatch board.md`, put the hex id "
             "from its URL in src/config.h and rebuild. GH_TOKEN must be a "
             "classic token with the `gist` scope -- a fine-grained PAT cannot "
             "write gists.");
        return -1;
    }
    LOGI("gist: publishing the board to %s/%s", GIST_WEB_BASE, GIST_ID);
    return 0;
}

int gist_publish(arena_t *a, const char *markdown, int dry_run)
{
    const char *hdrs[8];
    const char *tok, *body, *url;
    http_req_t req;
    http_resp_t resp;
    size_t body_len = 0, nh = 0, i;

    if (a == NULL || markdown == NULL || markdown[0] == '\0') {
        /* An empty board is a render failure, not "nothing to report": one
         * publish would replace a good gist with a blank page. */
        LOGE("gist: nothing to publish, leaving the gist as it stands");
        return -EINVAL;
    }

    if (dry_run) {
        /*
         * Returns before any http call -- that is the whole point of the branch,
         * and tests/test_gist.c asserts gist_http_calls stays 0. The board is
         * printed whole rather than summarised: --dry-run is how the rendering
         * gets eyeballed before a gist id is ever configured.
         */
        printf("[dry-run] gist %s/%s file %s, %zu bytes, not published:\n%s\n",
               GIST_WEB_BASE, GIST_ID, GIST_FILENAME, strlen(markdown), markdown);
        fflush(stdout);
        return 0;
    }

    /* main() blocks a real run at gist_init(); this is the belt to that braces,
     * since PATCHing the placeholder id would 404 and read as a scope problem. */
    if (gist_id_is_placeholder()) {
        LOGE("gist: GIST_ID is still the placeholder; refusing to PATCH it");
        return -EINVAL;
    }

    tok = env_or_null("GH_TOKEN");
    if (tok == NULL || tok[0] == '\0') {
        LOGE("gist: GH_TOKEN is unset or empty; the board cannot be published.");
        return -EACCES;
    }

    body = gist_build_body(a, markdown, &body_len);
    if (body == NULL)
        return -ENOMEM;

    url = arena_printf(a, "%s/%s", GIST_API_BASE, GIST_ID);
    hdrs[nh++] = arena_printf(a, "Authorization: Bearer %s", tok);
    hdrs[nh++] = "Accept: application/vnd.github+json";
    hdrs[nh++] = "X-GitHub-Api-Version: 2022-11-28";
    hdrs[nh++] = "User-Agent: " GH_USER_AGENT;
    hdrs[nh++] = "Content-Type: application/json";
    hdrs[nh] = NULL;

    if (url == NULL) {
        LOGE("gist: arena exhausted building the gist URL");
        return -ENOMEM;
    }
    for (i = 0; i < nh; i++) {
        if (hdrs[i] == NULL) {
            LOGE("gist: arena exhausted building the request headers");
            return -ENOMEM;
        }
    }

    memset(&req, 0, sizeof req);
    req.url         = url;
    req.method      = "PATCH";
    req.headers     = hdrs;
    req.body        = body;
    req.body_len    = body_len;
    req.timeout_sec = GIST_TIMEOUT_SEC;

    memset(&resp, 0, sizeof resp);
    gist_http_calls++;                      /* the --dry-run invariant hangs off this */
    if (http_perform_one(a, &req, &resp) < 0) {
        LOGW("gist: transport failure publishing the board");
        return -EIO;
    }

    if (resp.status == 404 || resp.status == 403) {
        /*
         * GitHub hides an unauthorised gist behind a 404 rather than a 403, so
         * the status alone cannot separate "wrong id" from "token has no gist
         * scope" -- which is exactly the mistake a first run makes. The scope
         * list off the response is the only thing that tells the two apart.
         */
        LOGE("gist: GitHub replied %ld for gist %s. This almost always means "
             "GH_TOKEN lacks the `gist` scope; the token presents [%s]. Gists "
             "need a classic token with `gist` checked -- a fine-grained PAT "
             "cannot write them. Otherwise GIST_ID is wrong.",
             resp.status, GIST_ID,
             resp.oauth_scopes[0] != '\0' ? resp.oauth_scopes : "no scopes reported");
        return -EACCES;
    }
    if (resp.status < 200 || resp.status >= 300) {
        LOGW("gist: GitHub replied %ld publishing the board", resp.status);
        return -EIO;
    }

    LOGI("gist: board published, %zu bytes to %s/%s", body_len, GIST_WEB_BASE, GIST_ID);
    return 0;
}
