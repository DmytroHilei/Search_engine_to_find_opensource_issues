#include "net/http.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <curl/curl.h>

#include "core/arena.h"
#include "core/util.h"

/*
 * One CURLM per batch, HTTP/2 multiplexed over a single connection per host.
 * Bodies land in the arena, which has no realloc: the write callback appends to
 * a chunk list and the chunks are flattened into one contiguous NUL-terminated
 * buffer when the transfer completes. Content-Length (when present) sizes the
 * first chunk so the common case is one chunk and zero copies.
 */

#define BODY_CHUNK_MIN 8192u
/* A hostile Content-Length must not be able to drain the arena on its own. */
#define BODY_HINT_MAX  (1u << 20)
#define POLL_TIMEOUT_MS 1000

typedef struct body_chunk {
    struct body_chunk *next;
    char *data;
    size_t len;
    size_t cap;
} body_chunk_t;

typedef struct {
    arena_t *a;
    const http_req_t *req;
    http_resp_t *resp;
    CURL *eh;
    struct curl_slist *hdrs;
    body_chunk_t *head;
    body_chunk_t *tail;
    size_t total;              /* bytes accumulated across all chunks */
    size_t hint;               /* Content-Length hint, 0 when unknown */
    int in_use;
    int oom;                   /* arena refused an allocation: fail this one */
    char errbuf[CURL_ERROR_SIZE];
} xfer_t;

static int g_init;

/* ---------------------------------------------------------------- helpers */

static void bounded_copy(char *dst, size_t dstlen, const char *src, size_t n)
{
    if (dst == NULL || dstlen == 0)
        return;
    if (n > dstlen - 1)
        n = dstlen - 1;          /* a hostile header truncates, never overflows */
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Matches one header line against `name` case-insensitively -- names are not
 * normalised over the wire and HTTP/2 lowercases them -- and hands back the
 * value with leading whitespace and trailing CRLF/whitespace stripped.
 */
static int hdr_value(const char *line, size_t len, const char *name,
                     const char **val, size_t *vlen)
{
    size_t nl = strlen(name);
    size_t i;
    size_t end;

    if (len <= nl || line[nl] != ':')
        return 0;
    if (ascii_strncasecmp(line, name, nl) != 0)
        return 0;

    i = nl + 1;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;

    end = len;
    while (end > i && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                       line[end - 1] == ' ' || line[end - 1] == '\t'))
        end--;

    *val = line + i;
    *vlen = end - i;
    return 1;
}

/* Decimal parse of a bounded, non-NUL-terminated value. -1 when not a number. */
static long parse_long(const char *s, size_t n)
{
    long v = 0;
    size_t i = 0;
    int neg = 0;

    if (n == 0)
        return -1;
    if (s[0] == '-' || s[0] == '+') {
        neg = (s[0] == '-');
        i = 1;
    }
    if (i >= n || s[i] < '0' || s[i] > '9')
        return -1;               /* e.g. Retry-After as an HTTP-date */
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
        if (v > (long)((1u << 31) - 1) / 10)
            return -1;           /* absurd value; treat as absent */
        v = v * 10 + (s[i] - '0');
    }
    return neg ? -v : v;
}

static void hdr_reset(xfer_t *x)
{
    x->resp->etag[0] = '\0';
    x->resp->link[0] = '\0';
    x->resp->oauth_scopes[0] = '\0';
    x->resp->retry_after = -1;
    x->resp->rl_remaining = -1;
    x->resp->rl_reset = -1;
    x->hint = 0;
}

/* ------------------------------------------------------------- body chunks */

static int body_append(xfer_t *x, const char *data, size_t n)
{
    body_chunk_t *c = x->tail;
    size_t cap;

    if (n == 0)
        return 0;

    if (c == NULL || c->cap - c->len < n) {
        cap = n;
        if (x->hint > x->total && x->hint - x->total > cap)
            cap = x->hint - x->total;
        if (cap < BODY_CHUNK_MIN)
            cap = BODY_CHUNK_MIN;
        /* Each new chunk covers everything so far: chunk count stays O(log n). */
        if (cap < x->total)
            cap = x->total;
        cap += 1;                /* room for the terminator, so flatten can be
                                  * skipped when one chunk holds the whole body */

        c = arena_alloc(x->a, sizeof *c, 0);
        if (c == NULL)
            return -1;
        c->next = NULL;
        c->len = 0;
        c->cap = cap;
        c->data = arena_alloc(x->a, cap, 1);
        if (c->data == NULL)
            return -1;

        if (x->tail == NULL)
            x->head = c;
        else
            x->tail->next = c;
        x->tail = c;
    }

    memcpy(c->data + c->len, data, n);
    c->len += n;
    x->total += n;
    return 0;
}

static void body_finish(xfer_t *x)
{
    http_resp_t *r = x->resp;
    body_chunk_t *c;
    char *out;
    size_t off = 0;

    if (x->head != NULL && x->head->next == NULL && x->head->cap > x->head->len) {
        x->head->data[x->head->len] = '\0';
        r->body = x->head->data;
        r->body_len = x->head->len;
        return;
    }

    out = arena_alloc(x->a, x->total + 1, 1);
    if (out == NULL) {
        x->oom = 1;
        return;
    }
    for (c = x->head; c != NULL; c = c->next) {
        memcpy(out + off, c->data, c->len);
        off += c->len;
    }
    out[off] = '\0';
    r->body = out;
    r->body_len = off;
}

/* ---------------------------------------------------------------- callbacks */

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    xfer_t *x = ud;
    size_t n = size * nmemb;

    if (body_append(x, ptr, n) != 0) {
        x->oom = 1;
        return 0;                /* aborts the transfer with CURLE_WRITE_ERROR */
    }
    return n;
}

static size_t on_header(char *buf, size_t size, size_t nitems, void *ud)
{
    xfer_t *x = ud;
    size_t n = size * nitems;
    const char *v;
    size_t vlen;

    if (n >= 5 && ascii_strncasecmp(buf, "HTTP/", 5) == 0) {
        /* A status line means a fresh response: an interim 1xx or a proxy
         * CONNECT precedes the real one, so drop whatever it carried. */
        hdr_reset(x);
        return n;
    }

    if (hdr_value(buf, n, "ETag", &v, &vlen))
        bounded_copy(x->resp->etag, sizeof x->resp->etag, v, vlen);
    else if (hdr_value(buf, n, "Link", &v, &vlen))
        bounded_copy(x->resp->link, sizeof x->resp->link, v, vlen);
    /* hdr_value() anchors at the start of the line, so GitHub's similarly named
     * X-Accepted-OAuth-Scopes cannot land here by accident. */
    else if (hdr_value(buf, n, "X-OAuth-Scopes", &v, &vlen))
        bounded_copy(x->resp->oauth_scopes, sizeof x->resp->oauth_scopes, v, vlen);
    else if (hdr_value(buf, n, "Retry-After", &v, &vlen))
        x->resp->retry_after = parse_long(v, vlen);
    else if (hdr_value(buf, n, "X-RateLimit-Remaining", &v, &vlen))
        x->resp->rl_remaining = parse_long(v, vlen);
    else if (hdr_value(buf, n, "X-RateLimit-Reset", &v, &vlen))
        x->resp->rl_reset = parse_long(v, vlen);
    else if (hdr_value(buf, n, "Content-Length", &v, &vlen)) {
        long cl = parse_long(v, vlen);

        /* Only a sizing hint: with gzip this is the compressed length. */
        if (cl > 0 && (size_t)cl <= BODY_HINT_MAX)
            x->hint = (size_t)cl;
    }

    return n;
}

/* ------------------------------------------------------------- one transfer */

static void resp_init(http_resp_t *r, const http_req_t *q)
{
    memset(r, 0, sizeof *r);
    r->retry_after = -1;
    r->rl_remaining = -1;
    r->rl_reset = -1;
    r->user = (q != NULL) ? q->user : NULL;
}

static void release_xfer(xfer_t *x)
{
    if (x->eh != NULL) {
        curl_easy_cleanup(x->eh);
        x->eh = NULL;
    }
    if (x->hdrs != NULL) {
        curl_slist_free_all(x->hdrs);
        x->hdrs = NULL;
    }
    x->in_use = 0;
}

static int start_xfer(CURLM *cm, xfer_t *x, arena_t *a, const http_req_t *req,
                     http_resp_t *resp)
{
    int bad = 0;
    size_t i;

    memset(x, 0, sizeof *x);
    x->a = a;
    x->req = req;
    x->resp = resp;
    x->in_use = 1;

    if (req->url == NULL)
        return -EINVAL;

    x->eh = curl_easy_init();
    if (x->eh == NULL)
        return -ENOMEM;

    if (req->headers != NULL) {
        for (i = 0; req->headers[i] != NULL; i++) {
            struct curl_slist *nl = curl_slist_append(x->hdrs, req->headers[i]);

            if (nl == NULL) {
                release_xfer(x);
                return -ENOMEM;
            }
            x->hdrs = nl;
        }
    }

    bad |= curl_easy_setopt(x->eh, CURLOPT_URL, req->url) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_ACCEPT_ENCODING, "gzip") != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_HTTP_VERSION,
                            (long)CURL_HTTP_VERSION_2TLS) != CURLE_OK;
    /* Wait for the shared h2 connection instead of racing a second one open. */
    bad |= curl_easy_setopt(x->eh, CURLOPT_PIPEWAIT, 1L) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_NOSIGNAL, 1L) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_WRITEFUNCTION, on_write) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_WRITEDATA, x) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_HEADERFUNCTION, on_header) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_HEADERDATA, x) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_ERRORBUFFER, x->errbuf) != CURLE_OK;
    bad |= curl_easy_setopt(x->eh, CURLOPT_PRIVATE, x) != CURLE_OK;

    if (req->timeout_sec > 0)
        bad |= curl_easy_setopt(x->eh, CURLOPT_TIMEOUT, req->timeout_sec) != CURLE_OK;
    if (x->hdrs != NULL)
        bad |= curl_easy_setopt(x->eh, CURLOPT_HTTPHEADER, x->hdrs) != CURLE_OK;

    if (req->body != NULL) {
        /* POSTFIELDS is not copied; the arena keeps it alive for the batch. */
        bad |= curl_easy_setopt(x->eh, CURLOPT_POSTFIELDS, req->body) != CURLE_OK;
        bad |= curl_easy_setopt(x->eh, CURLOPT_POSTFIELDSIZE_LARGE,
                                (curl_off_t)req->body_len) != CURLE_OK;
    }
    if (req->method != NULL && ascii_strncasecmp(req->method, "GET", 4) != 0) {
        if (ascii_strncasecmp(req->method, "POST", 5) == 0) {
            if (req->body == NULL)
                bad |= curl_easy_setopt(x->eh, CURLOPT_POST, 1L) != CURLE_OK;
        } else {
            bad |= curl_easy_setopt(x->eh, CURLOPT_CUSTOMREQUEST, req->method) != CURLE_OK;
        }
    }

    if (bad) {
        release_xfer(x);
        return -EINVAL;
    }
    if (curl_multi_add_handle(cm, x->eh) != CURLM_OK) {
        release_xfer(x);
        return -EIO;
    }
    return 0;
}

static void finish_xfer(xfer_t *x, CURLcode res)
{
    http_resp_t *r = x->resp;
    long code = 0;

    if (curl_easy_getinfo(x->eh, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK)
        r->status = code;
    r->curl_err = (int)res;

    /* A successful transfer always gets a body, "" included, so callers can
     * treat it as a string. A failure that produced no bytes gets NULL. */
    if (!x->oom && (res == CURLE_OK || x->total > 0))
        body_finish(x);

    if (x->oom) {
        r->body = NULL;
        r->body_len = 0;
        r->err_msg = arena_strdup(x->a, "arena exhausted");
        if (r->curl_err == CURLE_OK)
            r->curl_err = (int)CURLE_OUT_OF_MEMORY;
    } else if (res != CURLE_OK) {
        x->errbuf[sizeof x->errbuf - 1] = '\0';
        r->err_msg = arena_strdup(x->a, x->errbuf[0] != '\0' ? x->errbuf
                                                            : curl_easy_strerror(res));
    }

    if (r->curl_err != 0)
        LOGD("http %s failed: %s", x->req->url, r->err_msg != NULL ? r->err_msg : "?");
    else
        LOGD("http %s -> %ld (%zu bytes)", x->req->url, r->status, r->body_len);
}

/* ------------------------------------------------------------------ public */

int http_global_init(void)
{
    if (g_init)
        return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return -EIO;
    g_init = 1;
    return 0;
}

void http_global_cleanup(void)
{
    if (!g_init)
        return;
    curl_global_cleanup();
    g_init = 0;
}

int http_perform_batch(arena_t *a, const http_req_t *reqs, size_t n,
                       http_resp_t *resps, int max_concurrent)
{
    CURLM *cm = NULL;
    xfer_t *slots = NULL;
    size_t nslots;
    size_t next = 0;
    size_t inflight = 0;
    size_t i;
    int still_running = 0;
    int rc = 0;

    if (a == NULL || (n > 0 && (reqs == NULL || resps == NULL)))
        return -EINVAL;

    for (i = 0; i < n; i++)
        resp_init(&resps[i], &reqs[i]);
    if (n == 0)
        return 0;

    if (max_concurrent < 1)
        max_concurrent = 1;
    nslots = (size_t)max_concurrent;
    if (nslots > n)
        nslots = n;

    slots = arena_calloc(a, nslots, sizeof *slots);
    if (slots == NULL)
        return -ENOMEM;

    cm = curl_multi_init();
    if (cm == NULL)
        return -ENOMEM;

    /* One h2 connection per host, shared by every transfer in the batch. */
    if (curl_multi_setopt(cm, CURLMOPT_PIPELINING, (long)CURLPIPE_MULTIPLEX) != CURLM_OK ||
        curl_multi_setopt(cm, CURLMOPT_MAX_HOST_CONNECTIONS, 1L) != CURLM_OK) {
        rc = -EIO;
        goto out;
    }

    for (;;) {
        CURLMsg *msg;
        int msgs_left = 0;
        int numfds = 0;

        while (inflight < nslots && next < n) {
            xfer_t *x = NULL;

            for (i = 0; i < nslots; i++) {
                if (!slots[i].in_use) {
                    x = &slots[i];
                    break;
                }
            }
            if (x == NULL)
                break;

            if (start_xfer(cm, x, a, &reqs[next], &resps[next]) != 0) {
                /* A setup failure is this transfer's verdict, not the batch's. */
                resps[next].curl_err = (int)CURLE_FAILED_INIT;
                resps[next].err_msg = arena_strdup(a, "request setup failed");
                x->in_use = 0;
                next++;
                continue;
            }
            next++;
            inflight++;
        }

        if (inflight == 0)
            break;

        if (curl_multi_perform(cm, &still_running) != CURLM_OK) {
            rc = -EIO;
            goto out;
        }

        while ((msg = curl_multi_info_read(cm, &msgs_left)) != NULL) {
            char *priv = NULL;
            xfer_t *x;

            if (msg->msg != CURLMSG_DONE)
                continue;
            if (curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv) != CURLE_OK ||
                priv == NULL) {
                /* Cannot map the handle back to a slot: `inflight` would never
                 * drain, so give up instead of spinning. */
                curl_multi_remove_handle(cm, msg->easy_handle);
                rc = -EIO;
                goto out;
            }

            x = (xfer_t *)priv;
            finish_xfer(x, msg->data.result);
            curl_multi_remove_handle(cm, x->eh);
            release_xfer(x);
            inflight--;
        }

        if (inflight < nslots && next < n)
            continue;            /* refill before going back to sleep */

        if (still_running > 0 &&
            curl_multi_poll(cm, NULL, 0, POLL_TIMEOUT_MS, &numfds) != CURLM_OK) {
            rc = -EIO;
            goto out;
        }
    }

out:
    for (i = 0; i < nslots; i++) {
        if (!slots[i].in_use)
            continue;
        if (slots[i].resp != NULL && slots[i].resp->curl_err == 0) {
            slots[i].resp->curl_err = (int)CURLE_ABORTED_BY_CALLBACK;
            slots[i].resp->err_msg = arena_strdup(a, "batch aborted");
        }
        if (slots[i].eh != NULL)
            curl_multi_remove_handle(cm, slots[i].eh);
        release_xfer(&slots[i]);
    }
    curl_multi_cleanup(cm);
    return rc;
}

int http_perform_one(arena_t *a, const http_req_t *req, http_resp_t *resp)
{
    if (req == NULL || resp == NULL)
        return -EINVAL;
    return http_perform_batch(a, req, 1, resp, 1);
}

/* ---------------------------------------------------------------- Link hdr */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* `rel` is a space-separated list of link types; "next" must match a whole one. */
static int rel_has_next(const char *s, const char *end)
{
    while (s < end) {
        const char *t;

        while (s < end && (*s == ' ' || *s == '\t'))
            s++;
        t = s;
        while (s < end && *s != ' ' && *s != '\t')
            s++;
        if (s - t == 4 && ascii_strncasecmp(t, "next", 4) == 0)
            return 1;
    }
    return 0;
}

int http_link_next(const char *link_header, char *out, size_t outlen)
{
    const char *p = link_header;

    if (out == NULL || outlen == 0)
        return 0;
    out[0] = '\0';
    if (p == NULL)
        return 0;

    while (*p != '\0') {
        const char *url;
        size_t url_len;
        int is_next = 0;

        p = skip_ws(p);
        while (*p == ',') {
            p++;
            p = skip_ws(p);
        }
        if (*p == '\0')
            break;
        if (*p != '<') {
            /* Malformed link-value: drop it and try the next comma-separated one. */
            while (*p != '\0' && *p != ',')
                p++;
            continue;
        }

        url = ++p;
        while (*p != '\0' && *p != '>')
            p++;
        if (*p != '>')
            return 0;            /* truncated header; nothing further is parsable */
        url_len = (size_t)(p - url);
        p++;

        /* Parameters of this link-value, up to the next unquoted comma. */
        while (*p != '\0') {
            const char *name;
            size_t name_len;
            const char *val;
            const char *val_end;

            p = skip_ws(p);
            if (*p == ',') {
                p++;
                break;
            }
            if (*p != ';') {
                if (*p == '\0')
                    break;
                p++;             /* junk between parameters */
                continue;
            }
            p = skip_ws(p + 1);

            name = p;
            while (*p != '\0' && *p != '=' && *p != ';' && *p != ',' &&
                   *p != ' ' && *p != '\t')
                p++;
            name_len = (size_t)(p - name);

            p = skip_ws(p);
            if (*p != '=')
                continue;        /* parameter without a value */
            p = skip_ws(p + 1);

            if (*p == '"' || *p == '\'') {
                char q = *p++;

                val = p;
                while (*p != '\0' && *p != q)
                    p++;
                val_end = p;
                if (*p == q)
                    p++;
            } else {
                val = p;
                while (*p != '\0' && *p != ';' && *p != ',' &&
                       *p != ' ' && *p != '\t')
                    p++;
                val_end = p;
            }

            if (name_len == 3 && ascii_strncasecmp(name, "rel", 3) == 0 &&
                rel_has_next(val, val_end))
                is_next = 1;
        }

        if (is_next) {
            /*
             * Fail closed rather than truncate: the only caller is pagination,
             * and a truncated URL is not the one GitHub sent -- it would fetch
             * the wrong page and look like success. Ending pagination a page
             * early is recoverable on the next cycle; a wrong fetch is a silent
             * correctness bug. out[] is cleared so a caller that ignores the
             * return value cannot pick up stale bytes either.
             */
            if (url_len > outlen - 1) {
                out[0] = '\0';
                return 0;
            }
            bounded_copy(out, outlen, url, url_len);
            return 1;
        }
    }

    return 0;
}
