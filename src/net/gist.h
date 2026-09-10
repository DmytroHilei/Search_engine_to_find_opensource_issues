#ifndef GIST_H
#define GIST_H

#include "core/arena.h"

/*
 * Publishes the rendered board to a secret GitHub Gist with the same GH_TOKEN
 * and the same net/http.c as everything else -- no new dependency, no new
 * secret, nothing to host. The gist is the dashboard; the ntfy push is only a
 * pointer to it.
 *
 * The token needs `gist` scope. Without it the PATCH comes back as an opaque
 * 404, so the scope check exists to say so in words.
 */

/*
 * Fails when GIST_ID is still the placeholder, exactly like notify_init() on
 * NTFY_TOPIC. main() may tolerate that under --dry-run, where nothing is sent.
 */
int gist_init(void);

/*
 * PATCHes GIST_FILENAME in GIST_ID with `markdown`. Returns 0 on success,
 * negative on failure -- and a failure must NOT advance the watermark, because
 * publishing is now the cycle's primary output.
 *
 * With dry_run non-zero this prints the board to stdout and MUST NOT issue any
 * HTTP request; tests/test_gist.c asserts that structurally via gist_http_calls.
 */
int gist_publish(arena_t *a, const char *markdown, int dry_run);

#endif /* GIST_H */
