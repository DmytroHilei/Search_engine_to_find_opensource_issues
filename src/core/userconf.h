#ifndef USERCONF_H
#define USERCONF_H

#include <stddef.h>

#include "core/arena.h"
#include "config.h"

/*
 * The personal layer: the handful of settings that differ between one user and
 * the next, read from a plain text file at startup so that running this on your
 * own repos does not mean editing C and rebuilding.
 *
 * Everything else stays in config.h as a compile-time macro, deliberately.
 * `num_ctx`, batch sizes, timeouts and thresholds are tuned against a measured
 * corpus and a specific GPU; exposing them would invite changes nobody
 * re-measures. What lives here is instead "who am I and what do I watch", which
 * cannot be tuned in advance because it is different for everybody:
 *
 *     repos       which repositories to poll
 *     profile     the developer description the LLM scores against
 *     keywords    the prefilter table -- see the note below on why it must
 *                 travel with the profile rather than stay compiled in
 *     ntfy-topic  } credentials: publishing either hands someone a capability
 *     gist-id     }
 *
 * Keywords are here because the prefilter runs BEFORE the judge: an issue that
 * does not clear KW_SCORE_MIN never reaches the LLM at all. A user who sets a
 * profile about Rust and WASM but inherits a keyword table scoring `cuda` and
 * `matmul` would get an empty board and no indication why, which is a worse
 * failure than having to edit two things.
 *
 * Absent file, or absent key within it, means the compiled-in default from
 * config.h. A loaded config is fully resolved: no field is ever NULL.
 */

typedef struct {
    const char *const *repos;
    size_t n_repos;
    const char *profile;         /* substituted into the LLM system prompts */
    const kw_t *keywords;
    size_t n_keywords;
    const char *ntfy_topic;
    const char *gist_id;

    const char *path;            /* the file actually read, or NULL for defaults */
} userconf_t;

/* Hard limits. Generous enough not to be felt, small enough to bound the parse. */
#define UCONF_MAX_REPOS     256
#define UCONF_MAX_KEYWORDS  512
#define UCONF_MAX_LINE      4096
#define UCONF_MAX_PROFILE   4096

/*
 * Resolves the config into `out`, reading `path` when it is non-NULL and
 * otherwise the default location ($XDG_CONFIG_HOME, else $HOME/.config, then
 * "issuewatch/config"). Strings are copied into `perm` and live as long as it.
 *
 * A missing file is success: `out` gets the compiled-in defaults. A file that
 * exists but does not parse is a failure, because the alternative is a daemon
 * that silently watches the author's repositories instead of the user's.
 */
int userconf_load(arena_t *perm, const char *path, userconf_t *out);

/*
 * Fills `out` with the compiled-in defaults and nothing else. Exposed so tests
 * and callers that skip the file can share one definition of "default".
 */
void userconf_defaults(userconf_t *out);

/*
 * Parses `text` as the contents of a config file. Same result as
 * userconf_load() on a file holding `text`; `name` appears in diagnostics.
 * Exposed for the tests, which have no business writing files.
 */
int userconf_parse(arena_t *perm, const char *text, const char *name, userconf_t *out);

#endif /* USERCONF_H */
