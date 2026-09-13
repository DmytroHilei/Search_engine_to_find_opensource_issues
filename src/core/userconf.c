/*
 * The one translation unit that instantiates config.h's X-lists, which is why
 * both guards are defined here and nowhere else now. Before this file, main.c
 * asked for REPOS[] and prefilter.c asked for KEYWORDS[]; both now receive
 * whatever userconf_load() resolved, so the defaults have exactly one home.
 */
#define CONFIG_WANT_REPOS
#define CONFIG_WANT_KEYWORDS

#include "core/userconf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/state.h"
#include "core/util.h"

/*
 * Parse state. Repos and keywords accumulate into fixed arrays sized by the
 * UCONF_MAX_* caps and are copied into the arena once, at the end: a file that
 * fails on line 40 must not leave a half-applied config behind, and the arena
 * cannot free what a partial parse already took from it.
 */
typedef struct {
    const char *repos[UCONF_MAX_REPOS];
    size_t n_repos;
    kw_t keywords[UCONF_MAX_KEYWORDS];
    size_t n_keywords;
    char profile[UCONF_MAX_PROFILE];
    size_t profile_len;
    const char *ntfy_topic;
    const char *gist_id;
    int saw_repo;
    int saw_keyword;
} parse_t;

void userconf_defaults(userconf_t *out)
{
    if (out == NULL)
        return;
    out->repos      = REPOS;
    out->n_repos    = N_REPOS;
    out->profile    = USER_PROFILE;
    out->keywords   = KEYWORDS;
    out->n_keywords = N_KEYWORDS;
    out->ntfy_topic = NTFY_TOPIC;
    out->gist_id    = GIST_ID;
    out->path       = NULL;
}

/* ------------------------------------------------------------------ tokens */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *skip_space(char *s)
{
    while (*s != '\0' && is_space(*s))
        s++;
    return s;
}

static void rstrip(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && is_space(s[n - 1]))
        s[--n] = '\0';
}

/*
 * Splits off the leading word, NUL-terminating it in place, and returns the
 * rest with leading space skipped. The rest is returned verbatim from there on:
 * a profile line or a repo name must keep its own internal spacing.
 */
static char *take_word(char *s, char **word)
{
    char *end = s;

    while (*end != '\0' && !is_space(*end))
        end++;
    if (*end != '\0') {
        *end = '\0';
        end++;
    }
    *word = s;
    return skip_space(end);
}

/* ------------------------------------------------------------------- rules */

static int add_repo(parse_t *p, char *val, const char *name, int line)
{
    const char *slash = strchr(val, '/');

    if (val[0] == '\0') {
        LOGE("%s:%d: repo needs an owner/name", name, line);
        return -EINVAL;
    }
    /* Caught here rather than as a 404 three minutes into the first cycle. */
    if (slash == NULL || slash == val || slash[1] == '\0' ||
        strchr(slash + 1, '/') != NULL) {
        LOGE("%s:%d: repo wants exactly one slash, as owner/name: '%s'",
             name, line, val);
        return -EINVAL;
    }
    if (strlen(val) >= STATE_REPO_MAX) {
        LOGE("%s:%d: repo name longer than %d bytes: '%s'",
             name, line, STATE_REPO_MAX - 1, val);
        return -EINVAL;
    }
    if (p->n_repos >= UCONF_MAX_REPOS) {
        LOGE("%s:%d: more than %d repos", name, line, UCONF_MAX_REPOS);
        return -EINVAL;
    }
    p->repos[p->n_repos++] = val;
    return 0;
}

/*
 * "keyword <weight> <term...>" -- the weight leads and the term is the whole
 * rest of the line, because half the useful terms are phrases: "good first
 * issue", "race condition", "help wanted". A term-first form cannot express
 * those without quoting, and label-only is its own key rather than a trailing
 * flag for the same reason -- a suffix would be ambiguous against a term that
 * happens to end in the flag word.
 */
static int add_keyword(parse_t *p, char *val, const char *name, int line,
                       int label_only)
{
    const char *key = label_only ? "label-keyword" : "keyword";
    char *weight_s;
    char *end;
    long weight;

    val = take_word(val, &weight_s);
    rstrip(val);

    if (weight_s[0] == '\0' || val[0] == '\0') {
        LOGE("%s:%d: %s wants a weight then a term, as: %s 24 good first issue",
             name, line, key, key);
        return -EINVAL;
    }
    errno = 0;
    weight = strtol(weight_s, &end, 10);
    /* Negative is not an error but a feature: the shipped table uses -10 for
     * `dependabot` and `[bot]` so a bot issue is pushed below KW_SCORE_MIN
     * rather than merely failing to be pushed above it. Zero is rejected
     * because it does nothing, which is never what someone meant to type. */
    if (errno != 0 || *end != '\0' || weight == 0 ||
        weight < -1000 || weight > 1000) {
        LOGE("%s:%d: %s weight comes first and wants -1000..1000 and not 0, "
             "got '%s'", name, line, key, weight_s);
        return -EINVAL;
    }
    if (p->n_keywords >= UCONF_MAX_KEYWORDS) {
        LOGE("%s:%d: more than %d keywords", name, line, UCONF_MAX_KEYWORDS);
        return -EINVAL;
    }

    p->keywords[p->n_keywords].term = val;
    p->keywords[p->n_keywords].weight = (int)weight;
    p->keywords[p->n_keywords].label_only = label_only;
    p->n_keywords++;
    return 0;
}

/* Each `profile` line contributes one line of the prompt, blank ones included:
 * the default profile is a ranked list whose line structure the model reads. */
static int add_profile(parse_t *p, const char *val, const char *name, int line)
{
    size_t len = strlen(val);

    if (p->profile_len + len + 1 >= sizeof p->profile) {
        LOGE("%s:%d: profile is longer than %d bytes. It goes into every LLM "
             "request, so it competes with the issues for OLLAMA_NUM_CTX.",
             name, line, UCONF_MAX_PROFILE);
        return -EINVAL;
    }
    memcpy(p->profile + p->profile_len, val, len);
    p->profile_len += len;
    p->profile[p->profile_len++] = '\n';
    p->profile[p->profile_len] = '\0';
    return 0;
}

static int apply_line(parse_t *p, char *line, const char *name, int lineno)
{
    char *key;
    char *val;

    val = take_word(line, &key);

    if (strcmp(key, "repo") == 0) {
        rstrip(val);
        p->saw_repo = 1;
        return add_repo(p, val, name, lineno);
    }
    if (strcmp(key, "keyword") == 0) {
        p->saw_keyword = 1;
        return add_keyword(p, val, name, lineno, 0);
    }
    if (strcmp(key, "label-keyword") == 0) {
        p->saw_keyword = 1;
        return add_keyword(p, val, name, lineno, 1);
    }
    if (strcmp(key, "profile") == 0) {
        rstrip(val);
        return add_profile(p, val, name, lineno);
    }
    if (strcmp(key, "ntfy-topic") == 0) {
        rstrip(val);
        p->ntfy_topic = val;
        return 0;
    }
    if (strcmp(key, "gist-id") == 0) {
        rstrip(val);
        p->gist_id = val;
        return 0;
    }

    /*
     * Rejected rather than ignored. A silently dropped line reads as "my repo
     * is configured and the program is broken", and the cost of being wrong is
     * a whole cycle spent polling somebody else's repositories.
     */
    LOGE("%s:%d: unknown setting '%s'. Valid: repo, keyword, label-keyword, "
         "profile, ntfy-topic, gist-id", name, lineno, key);
    return -EINVAL;
}

/* -------------------------------------------------------------- resolution */

static int commit(arena_t *perm, parse_t *p, const char *name, userconf_t *out)
{
    userconf_defaults(out);

    if (p->saw_repo) {
        const char **v = arena_alloc(perm, p->n_repos * sizeof *v, 0);
        size_t i;

        if (v == NULL)
            return -ENOMEM;
        for (i = 0; i < p->n_repos; i++) {
            v[i] = arena_strdup(perm, p->repos[i]);
            if (v[i] == NULL)
                return -ENOMEM;
        }
        out->repos = v;
        out->n_repos = p->n_repos;
    }

    if (p->saw_keyword) {
        kw_t *v = arena_alloc(perm, p->n_keywords * sizeof *v, 0);
        size_t i;

        if (v == NULL)
            return -ENOMEM;
        for (i = 0; i < p->n_keywords; i++) {
            v[i] = p->keywords[i];
            v[i].term = arena_strdup(perm, p->keywords[i].term);
            if (v[i].term == NULL)
                return -ENOMEM;
        }
        out->keywords = v;
        out->n_keywords = p->n_keywords;
    }

    if (p->profile_len > 0) {
        /* The trailing newline of the last line: the prompt supplies its own. */
        while (p->profile_len > 0 && p->profile[p->profile_len - 1] == '\n')
            p->profile[--p->profile_len] = '\0';
        out->profile = arena_strdup(perm, p->profile);
        if (out->profile == NULL)
            return -ENOMEM;
    }
    if (p->ntfy_topic != NULL) {
        out->ntfy_topic = arena_strdup(perm, p->ntfy_topic);
        if (out->ntfy_topic == NULL)
            return -ENOMEM;
    }
    if (p->gist_id != NULL) {
        out->gist_id = arena_strdup(perm, p->gist_id);
        if (out->gist_id == NULL)
            return -ENOMEM;
    }

    out->path = arena_strdup(perm, name);
    if (out->path == NULL)
        return -ENOMEM;

    if (out->n_repos == 0) {
        LOGE("%s: no repos left to watch", name);
        return -EINVAL;
    }
    if (out->n_keywords == 0) {
        LOGE("%s: no keywords left, so the prefilter would drop every issue", name);
        return -EINVAL;
    }
    return 0;
}

int userconf_parse(arena_t *perm, const char *text, const char *name, userconf_t *out)
{
    parse_t p;
    const char *s;
    int lineno = 0;
    int rc = 0;

    if (perm == NULL || text == NULL || out == NULL)
        return -EINVAL;
    if (name == NULL)
        name = "config";

    memset(&p, 0, sizeof p);

    for (s = text; *s != '\0' || lineno == 0; ) {
        char buf[UCONF_MAX_LINE];
        const char *nl = strchr(s, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - s) : strlen(s);
        char *start;

        lineno++;
        if (len >= sizeof buf) {
            LOGE("%s:%d: line longer than %d bytes", name, lineno,
                 UCONF_MAX_LINE - 1);
            return -EINVAL;
        }
        memcpy(buf, s, len);
        buf[len] = '\0';
        s = (nl != NULL) ? nl + 1 : s + len;

        start = skip_space(buf);
        if (*start == '\0' || *start == '#')
            continue;

        /*
         * apply_line() keeps pointers into `buf`, so the values must be copied
         * out before the next iteration reuses it. p.repos/p.keywords hold
         * those pointers, which is why the copy into the arena cannot wait for
         * commit() -- do it here, per line.
         */
        {
            char *dup = arena_strdup(perm, start);

            if (dup == NULL)
                return -ENOMEM;
            rc = apply_line(&p, dup, name, lineno);
            if (rc != 0)
                return rc;
        }
        if (nl == NULL)
            break;
    }

    return commit(perm, &p, name, out);
}

/* ------------------------------------------------------------------- files */

static char *default_path(arena_t *perm)
{
    const char *xdg = env_or_null("XDG_CONFIG_HOME");
    const char *home;

    if (xdg != NULL)
        return arena_printf(perm, "%s/issuewatch/config", xdg);
    home = env_or_null("HOME");
    if (home == NULL)
        return NULL;
    return arena_printf(perm, "%s/.config/issuewatch/config", home);
}

/*
 * The file now holds the ntfy topic, and an ntfy topic is a capability: anyone
 * who can read it can push to the phone. Not fatal -- a shared machine is the
 * user's call to make -- but it must not pass unremarked.
 */
static void warn_if_readable(const char *path)
{
    struct stat sb;

    if (stat(path, &sb) != 0)
        return;
    if ((sb.st_mode & (S_IRGRP | S_IROTH)) != 0)
        LOGW("%s is readable by group or others and holds your ntfy topic, "
             "which is the only thing protecting your phone: chmod 600 it", path);
}

int userconf_load(arena_t *perm, const char *path, userconf_t *out)
{
    char *text;
    long size;
    size_t got;
    FILE *f;
    int explicit_path;
    int rc;

    if (perm == NULL || out == NULL)
        return -EINVAL;

    explicit_path = (path != NULL);
    if (path == NULL) {
        path = default_path(perm);
        if (path == NULL) {
            LOGW("neither XDG_CONFIG_HOME nor HOME is set; using built-in config");
            userconf_defaults(out);
            return 0;
        }
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        /*
         * The default location simply being absent is the normal first run. An
         * explicit --config that is not there is a typo, and falling back would
         * run the built-in repo list under the appearance of having honoured
         * the flag -- the same silent-wrong-config failure an unknown key is
         * rejected to prevent.
         */
        if (explicit_path) {
            LOGE("%s: cannot open the config given with --config", path);
            return -ENOENT;
        }
        userconf_defaults(out);
        LOGI("no config at %s, using the built-in %zu repos and profile. "
             "Copy src/config.example there to change that.", path, out->n_repos);
        return 0;
    }

    warn_if_readable(path);

    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        LOGE("%s: cannot determine size", path);
        fclose(f);
        return -EIO;
    }
    rewind(f);
    if (size > (long)(UCONF_MAX_LINE * UCONF_MAX_KEYWORDS)) {
        LOGE("%s: implausibly large for a config file (%ld bytes)", path, size);
        fclose(f);
        return -EFBIG;
    }

    text = arena_alloc(perm, (size_t)size + 1, 1);
    if (text == NULL) {
        fclose(f);
        return -ENOMEM;
    }
    got = fread(text, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        LOGE("%s: short read", path);
        return -EIO;
    }
    text[size] = '\0';

    rc = userconf_parse(perm, text, path, out);
    if (rc != 0)
        return rc;

    LOGI("config %s: %zu repos, %zu keywords, %zu byte profile",
         path, out->n_repos, out->n_keywords, strlen(out->profile));
    return 0;
}
