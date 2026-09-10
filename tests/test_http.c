/*
 * http.c unit tests. No network: the Link-header walker is pure, and the
 * transfer tests use URLs libcurl rejects before it opens a socket, which still
 * exercises the multi loop, slot reuse and the "every slot gets a verdict" rule.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../tests/test_util.h"

#include "arena.h"
#include "http.h"

#define GH_LINK \
    "<https://api.github.com/repositories/1300192/issues?page=2>; rel=\"next\", " \
    "<https://api.github.com/repositories/1300192/issues?page=515>; rel=\"last\""

/*
 * Runs the parser on a heap copy so ASan's redzone catches any read past the
 * terminating NUL of a truncated header.
 */
static int link_next_heap(const char *hdr, char *out, size_t outlen)
{
    char *copy = NULL;
    int rc;

    if (hdr != NULL) {
        copy = strdup(hdr);
        if (copy == NULL)
            exit(2);
    }
    rc = http_link_next(copy, out, outlen);
    free(copy);
    return rc;
}

static void test_link_next_basic(void)
{
    char url[256];

    CHECK_EQ(link_next_heap(GH_LINK, url, sizeof url), 1);
    CHECK_STREQ(url, "https://api.github.com/repositories/1300192/issues?page=2");
}

static void test_link_next_middle(void)
{
    char url[256];
    const char *h =
        "<https://api.github.com/x?page=1>; rel=\"prev\", "
        "<https://api.github.com/x?page=3>; rel=\"next\", "
        "<https://api.github.com/x?page=9>; rel=\"last\"";

    CHECK_EQ(link_next_heap(h, url, sizeof url), 1);
    CHECK_STREQ(url, "https://api.github.com/x?page=3");
}

static void test_link_next_absent(void)
{
    char url[256];
    const char *h =
        "<https://api.github.com/x?page=1>; rel=\"prev\", "
        "<https://api.github.com/x?page=9>; rel=\"last\"";

    memset(url, 'Z', sizeof url);
    CHECK_EQ(link_next_heap(h, url, sizeof url), 0);
    CHECK_STREQ(url, "");                     /* always defined, even on miss */
}

static void test_link_next_empty_inputs(void)
{
    char url[64];

    memset(url, 'Z', sizeof url);
    CHECK_EQ(http_link_next(NULL, url, sizeof url), 0);
    CHECK_STREQ(url, "");

    CHECK_EQ(link_next_heap("", url, sizeof url), 0);
    CHECK_EQ(link_next_heap("   ", url, sizeof url), 0);
    CHECK_EQ(link_next_heap(",,,", url, sizeof url), 0);
    CHECK_EQ(link_next_heap(";", url, sizeof url), 0);

    /* A zero-length destination must be refused, not written to. */
    CHECK_EQ(http_link_next(GH_LINK, url, 0), 0);
    CHECK_EQ(http_link_next(GH_LINK, NULL, 16), 0);
}

static void test_link_next_malformed(void)
{
    char url[256];

    /* Unterminated <: no closing angle bracket anywhere. */
    CHECK_EQ(link_next_heap("<https://api.github.com/x?page=2", url, sizeof url), 0);
    CHECK_EQ(link_next_heap("<", url, sizeof url), 0);
    /* Missing < entirely. */
    CHECK_EQ(link_next_heap("https://api.github.com/x>; rel=\"next\"",
                            url, sizeof url), 0);
    /* Truncated mid-parameter. */
    CHECK_EQ(link_next_heap("<https://x>; rel=\"nex", url, sizeof url), 0);
    CHECK_EQ(link_next_heap("<https://x>; rel=", url, sizeof url), 0);
    CHECK_EQ(link_next_heap("<https://x>;", url, sizeof url), 0);
    CHECK_EQ(link_next_heap("<https://x>", url, sizeof url), 0);
    /* A good link-value after a broken one still parses. */
    CHECK_EQ(link_next_heap("garbage, <https://x?p=2>; rel=\"next\"",
                            url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=2");
    /* An empty URL is legal to copy out; it must not read past the brackets. */
    CHECK_EQ(link_next_heap("<>; rel=\"next\"", url, sizeof url), 1);
    CHECK_STREQ(url, "");
}

static void test_link_next_quoting(void)
{
    char url[256];

    CHECK_EQ(link_next_heap("<https://x?p=2>; rel='next'", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=2");

    CHECK_EQ(link_next_heap("<https://x?p=3>; rel=next", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=3");

    CHECK_EQ(link_next_heap("<https://x?p=4>; rel=NEXT", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=4");

    /* Whitespace anywhere it is allowed, and then some. */
    CHECK_EQ(link_next_heap("  \t <https://x?p=5>  ;\t rel \t = \t \"next\" \t ,"
                            " <https://x?p=6>; rel=\"last\"", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=5");

    /* Extra parameters around the one that matters. */
    CHECK_EQ(link_next_heap("<https://x?p=7>; type=\"text/html\"; rel=\"next\"; "
                            "title=\"page 7\"", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=7");
}

static void test_link_next_rel_is_not_substring(void)
{
    char url[256];

    memset(url, 'Z', sizeof url);
    CHECK_EQ(link_next_heap("<https://x?p=2>; rel=\"nextpage\"", url, sizeof url), 0);
    CHECK_STREQ(url, "");
    CHECK_EQ(link_next_heap("<https://x?p=2>; rel=\"prevnext\"", url, sizeof url), 0);
    CHECK_EQ(link_next_heap("<https://x?p=2>; rel=nextish", url, sizeof url), 0);
    /* "next" inside some other parameter is not a rel. */
    CHECK_EQ(link_next_heap("<https://x?p=2>; title=\"next\"; rel=\"last\"",
                            url, sizeof url), 0);
    /* Nor is it a rel when it only appears in the URL. */
    CHECK_EQ(link_next_heap("<https://x/next>; rel=\"prev\"", url, sizeof url), 0);

    /* A quoted comma must not split the link-value, so this rel is one token. */
    CHECK_EQ(link_next_heap("<https://x?p=1>; rel=\"a,next\", "
                            "<https://x?p=2>; rel=\"next\"", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=2");

    /* A space-separated rel list does contain "next". */
    CHECK_EQ(link_next_heap("<https://x?p=8>; rel=\"prev next\"", url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=8");
}

/*
 * A URL that does not fit fails closed: 0 and an empty `out`, never a truncated
 * URL a caller would then fetch, and never a write past the buffer.
 */
static void test_link_next_does_not_fit(void)
{
    struct {
        char buf[16];
        char canary[16];
    } s;
    size_t i;
    int rc;

    memset(&s, 0x5a, sizeof s);
    rc = link_next_heap("<https://api.github.com/repositories/1/issues?page=2>; "
                        "rel=\"next\"", s.buf, sizeof s.buf);
    CHECK_EQ(rc, 0);
    CHECK_STREQ(s.buf, "");
    for (i = 0; i < sizeof s.canary; i++)
        CHECK_EQ(s.canary[i], 0x5a);

    /* Room for the terminator only. */
    memset(&s, 0x5a, sizeof s);
    CHECK_EQ(link_next_heap(GH_LINK, s.buf, 1), 0);
    CHECK_EQ(s.buf[0], '\0');
    CHECK_EQ(s.buf[1], 0x5a);

    /* Exactly fits: 15 URL bytes plus the terminator. */
    memset(&s, 0x5a, sizeof s);
    CHECK_EQ(link_next_heap("<https://x?p=421>; rel=\"next\"", s.buf, sizeof s.buf), 1);
    CHECK_STREQ(s.buf, "https://x?p=421");
    CHECK_EQ(strlen(s.buf), sizeof s.buf - 1);
    for (i = 0; i < sizeof s.canary; i++)
        CHECK_EQ(s.canary[i], 0x5a);

    /* One byte too long for the same buffer. */
    memset(&s, 0x5a, sizeof s);
    CHECK_EQ(link_next_heap("<https://x?p=4210>; rel=\"next\"", s.buf, sizeof s.buf), 0);
    CHECK_STREQ(s.buf, "");
    for (i = 0; i < sizeof s.canary; i++)
        CHECK_EQ(s.canary[i], 0x5a);

    /* An empty URL still fits a 1-byte destination. */
    memset(&s, 0x5a, sizeof s);
    CHECK_EQ(link_next_heap("<>; rel=\"next\"", s.buf, 1), 1);
    CHECK_EQ(s.buf[0], '\0');
    CHECK_EQ(s.buf[1], 0x5a);
}

static void test_link_next_long_header(void)
{
    char big[HTTP_LINK_MAX * 2];
    char url[256];
    size_t off = 0;
    int i;

    /* Many link-values, "next" last: the walker must not stop early. */
    for (i = 0; i < 40; i++)
        off += (size_t)snprintf(big + off, sizeof big - off,
                                "<https://x?p=%d>; rel=\"page%d\", ", i, i);
    snprintf(big + off, sizeof big - off, "<https://x?p=last>; rel=\"next\"");

    CHECK_EQ(link_next_heap(big, url, sizeof url), 1);
    CHECK_STREQ(url, "https://x?p=last");
}

/* ---- transfer plumbing, offline ------------------------------------------ */

/* Rejected by libcurl before any socket is opened, so these never touch a wire. */
#define BAD_URL_A "iw-no-such-scheme://example.invalid/a"
#define BAD_URL_B "iw-no-such-scheme://example.invalid/b"

static void test_perform_bad_args(void)
{
    arena_t a;
    http_req_t req;
    http_resp_t resp;

    CHECK_EQ(arena_init(&a, 64 * 1024), 0);

    memset(&req, 0, sizeof req);
    CHECK(http_perform_batch(NULL, &req, 1, &resp, 1) < 0);
    CHECK(http_perform_batch(&a, NULL, 1, &resp, 1) < 0);
    CHECK(http_perform_batch(&a, &req, 1, NULL, 1) < 0);
    CHECK(http_perform_one(&a, NULL, &resp) < 0);
    CHECK(http_perform_one(&a, &req, NULL) < 0);
    /* An empty batch is a no-op success. */
    CHECK_EQ(http_perform_batch(&a, NULL, 0, NULL, 4), 0);

    arena_destroy(&a);
}

static void test_perform_fills_every_slot(void)
{
    arena_t a;
    http_req_t reqs[5];
    http_resp_t resps[5];
    size_t i;

    CHECK_EQ(arena_init(&a, 256 * 1024), 0);
    CHECK_EQ(http_global_init(), 0);
    CHECK_EQ(http_global_init(), 0);           /* idempotent */

    memset(reqs, 0, sizeof reqs);
    memset(resps, 0xaa, sizeof resps);         /* nothing may be left uninitialised */
    for (i = 0; i < 5; i++) {
        reqs[i].url = (i % 2) ? BAD_URL_A : BAD_URL_B;
        reqs[i].method = "GET";
        reqs[i].timeout_sec = 5;
        reqs[i].user = (void *)(i + 1);        /* distinct non-NULL tags */
    }
    /* max_concurrent < n forces the refill path and slot reuse. */
    CHECK_EQ(http_perform_batch(&a, reqs, 5, resps, 2), 0);

    for (i = 0; i < 5; i++) {
        CHECK_EQ(resps[i].status, 0);
        CHECK(resps[i].curl_err != 0);
        CHECK(resps[i].err_msg != NULL);
        CHECK_STREQ(resps[i].etag, "");
        CHECK_STREQ(resps[i].link, "");
        CHECK_EQ(resps[i].retry_after, -1);
        CHECK_EQ(resps[i].rl_remaining, -1);
        CHECK_EQ(resps[i].rl_reset, -1);
        CHECK(resps[i].user == (void *)(i + 1));
    }

    http_global_cleanup();
    http_global_cleanup();                     /* idempotent */
    arena_destroy(&a);
}

static void test_perform_one_failure(void)
{
    arena_t a;
    http_req_t req;
    http_resp_t resp;

    CHECK_EQ(arena_init(&a, 64 * 1024), 0);
    CHECK_EQ(http_global_init(), 0);

    memset(&req, 0, sizeof req);
    memset(&resp, 0xaa, sizeof resp);
    req.url = BAD_URL_A;
    req.timeout_sec = 5;
    req.user = &a;

    CHECK_EQ(http_perform_one(&a, &req, &resp), 0);
    CHECK_EQ(resp.status, 0);
    CHECK(resp.curl_err != 0);
    CHECK(resp.err_msg != NULL);
    CHECK(resp.body == NULL);
    CHECK_EQ(resp.body_len, 0);
    CHECK(resp.user == &a);

    http_global_cleanup();
    arena_destroy(&a);
}

/* A full arena must fail the transfer, not crash. */
static void test_perform_arena_exhausted(void)
{
    arena_t a;
    http_req_t req;
    http_resp_t resp;

    CHECK_EQ(arena_init(&a, 64), 0);           /* too small for the slot table */
    CHECK_EQ(http_global_init(), 0);

    memset(&req, 0, sizeof req);
    memset(&resp, 0xaa, sizeof resp);
    req.url = BAD_URL_A;
    req.timeout_sec = 5;

    CHECK(http_perform_one(&a, &req, &resp) < 0);
    CHECK_EQ(resp.status, 0);
    CHECK(resp.body == NULL);
    CHECK_EQ(resp.retry_after, -1);

    http_global_cleanup();
    arena_destroy(&a);
}

/*
 * The body accumulator is the interesting half of the transport, so exercise it
 * over file://, which libcurl serves without touching a socket.
 */
static char *make_temp_body(char *path, size_t pathlen, size_t nbytes)
{
    const char *dir = getenv("TMPDIR");
    char *buf;
    FILE *f;
    int fd;
    size_t i;

    snprintf(path, pathlen, "%s/issuewatch_http_XXXXXX",
             (dir != NULL && dir[0] == '/') ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        exit(2);

    buf = malloc(nbytes + 1);
    if (buf == NULL)
        exit(2);
    for (i = 0; i < nbytes; i++)
        buf[i] = (char)('a' + (i % 26));      /* printable, so strlen == nbytes */
    buf[nbytes] = '\0';

    f = fdopen(fd, "wb");
    if (f == NULL || fwrite(buf, 1, nbytes, f) != nbytes || fclose(f) != 0)
        exit(2);
    return buf;
}

/* Larger than the Content-Length hint cap, so the chunk list really chains. */
#define BIG_BODY (1536u * 1024u)

static void test_body_multichunk(void)
{
    arena_t a;
    http_req_t req;
    http_resp_t resp;
    char path[256];
    char url[320];
    char *want;

    want = make_temp_body(path, sizeof path, BIG_BODY);
    snprintf(url, sizeof url, "file://%s", path);

    CHECK_EQ(arena_init(&a, 8u << 20), 0);
    CHECK_EQ(http_global_init(), 0);

    memset(&req, 0, sizeof req);
    memset(&resp, 0xaa, sizeof resp);
    req.url = url;
    req.method = "GET";
    req.timeout_sec = 30;

    CHECK_EQ(http_perform_one(&a, &req, &resp), 0);
    CHECK_EQ(resp.curl_err, 0);
    CHECK_EQ(resp.body_len, BIG_BODY);
    CHECK(resp.body != NULL);
    if (resp.body != NULL) {
        CHECK_EQ(strlen(resp.body), BIG_BODY);          /* NUL-terminated */
        CHECK_EQ(memcmp(resp.body, want, BIG_BODY), 0); /* chunks in order */
    }
    /* No header of ours is present on a file:// response. */
    CHECK_STREQ(resp.etag, "");
    CHECK_EQ(resp.retry_after, -1);
    CHECK_EQ(resp.rl_remaining, -1);

    http_global_cleanup();
    arena_destroy(&a);
    unlink(path);
    free(want);
}

/* A body that outgrows the arena must fail that transfer, not the process. */
static void test_body_arena_exhausted(void)
{
    arena_t a;
    http_req_t req;
    http_resp_t resp;
    char path[256];
    char url[320];
    char *want;

    want = make_temp_body(path, sizeof path, 256u * 1024u);
    snprintf(url, sizeof url, "file://%s", path);

    CHECK_EQ(arena_init(&a, 16 * 1024), 0);
    CHECK_EQ(http_global_init(), 0);

    memset(&req, 0, sizeof req);
    memset(&resp, 0xaa, sizeof resp);
    req.url = url;
    req.timeout_sec = 30;

    CHECK_EQ(http_perform_one(&a, &req, &resp), 0);
    CHECK(resp.curl_err != 0);
    CHECK(resp.body == NULL);
    CHECK_EQ(resp.body_len, 0);
    CHECK_STREQ(resp.err_msg, "arena exhausted");
    CHECK(a.failures > 0);

    http_global_cleanup();
    arena_destroy(&a);
    unlink(path);
    free(want);
}

/* A small body fits one chunk and is handed back without a second copy. */
static void test_body_small(void)
{
    arena_t a;
    http_req_t req;
    http_resp_t resp;
    char path[256];
    char url[320];
    char *want;

    want = make_temp_body(path, sizeof path, 11);
    snprintf(url, sizeof url, "file://%s", path);

    CHECK_EQ(arena_init(&a, 64 * 1024), 0);
    CHECK_EQ(http_global_init(), 0);

    memset(&req, 0, sizeof req);
    memset(&resp, 0xaa, sizeof resp);
    req.url = url;
    req.timeout_sec = 30;

    CHECK_EQ(http_perform_one(&a, &req, &resp), 0);
    CHECK_EQ(resp.curl_err, 0);
    CHECK_EQ(resp.body_len, 11);
    CHECK_STREQ(resp.body, "abcdefghijk");

    http_global_cleanup();
    arena_destroy(&a);
    unlink(path);
    free(want);
}

int main(void)
{
    TEST_RUN(test_link_next_basic);
    TEST_RUN(test_link_next_middle);
    TEST_RUN(test_link_next_absent);
    TEST_RUN(test_link_next_empty_inputs);
    TEST_RUN(test_link_next_malformed);
    TEST_RUN(test_link_next_quoting);
    TEST_RUN(test_link_next_rel_is_not_substring);
    TEST_RUN(test_link_next_does_not_fit);
    TEST_RUN(test_link_next_long_header);
    TEST_RUN(test_perform_bad_args);
    TEST_RUN(test_perform_fills_every_slot);
    TEST_RUN(test_perform_one_failure);
    TEST_RUN(test_perform_arena_exhausted);
    TEST_RUN(test_body_small);
    TEST_RUN(test_body_multichunk);
    TEST_RUN(test_body_arena_exhausted);
    TEST_REPORT();
}
