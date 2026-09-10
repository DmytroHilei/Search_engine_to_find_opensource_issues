#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "core/arena.h"

/*
 * curl_multi wrapper. All response memory is arena-allocated, so a cycle's
 * traffic disappears with one arena_reset().
 */

#define HTTP_ETAG_MAX 128
#define HTTP_LINK_MAX 1024
#define HTTP_SCOPES_MAX 256

typedef struct {
    const char *url;
    const char *method;           /* "GET" or "POST" */
    const char *const *headers;   /* NULL-terminated "Name: value" list, or NULL */
    const char *body;             /* POST payload, or NULL */
    size_t body_len;
    long timeout_sec;
    void *user;                   /* opaque caller tag, copied to the response */
} http_req_t;

typedef struct {
    long status;                  /* HTTP status, 0 if the transfer failed */
    char *body;                   /* arena-allocated, NUL-terminated; may be NULL */
    size_t body_len;
    char etag[HTTP_ETAG_MAX];     /* response ETag, "" when absent */
    char link[HTTP_LINK_MAX];     /* raw Link header, "" when absent */
    /*
     * X-OAuth-Scopes, "" when absent. GitHub answers a token that is missing a
     * scope with an opaque 404 rather than a 403, so this is the only thing that
     * lets a caller say "your token has [x], it needs [y]" instead of leaving
     * the user to guess whether the id is wrong or the token is.
     */
    char oauth_scopes[HTTP_SCOPES_MAX];
    long retry_after;             /* seconds, -1 when absent */
    long rl_remaining;            /* X-RateLimit-Remaining, -1 when absent */
    long rl_reset;                /* X-RateLimit-Reset epoch, -1 when absent */
    int curl_err;                 /* CURLE_* ; 0 on success */
    const char *err_msg;          /* arena-allocated detail, or NULL */
    void *user;                   /* copied from the request */
} http_resp_t;

int http_global_init(void);
void http_global_cleanup(void);

/*
 * Runs `n` requests concurrently over one multiplexed HTTP/2 connection per
 * host. `resps` must have room for `n` entries; each is filled in, including
 * on transfer failure (status 0, curl_err set). Returns 0 if every transfer
 * reached a verdict, negative on a setup failure.
 */
int http_perform_batch(arena_t *a, const http_req_t *reqs, size_t n,
                       http_resp_t *resps, int max_concurrent);

/* Blocking single request. Same response contract as the batch form. */
int http_perform_one(arena_t *a, const http_req_t *req, http_resp_t *resp);

/*
 * Parses a Link header, copying the rel="next" URL into `out`. Returns 1 when
 * one was found, 0 when not. Walks the header -- never regex it.
 */
int http_link_next(const char *link_header, char *out, size_t outlen);

#endif /* HTTP_H */
