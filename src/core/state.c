#include "core/state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "core/util.h"

/*
 * Persistence: a text `etags` file rewritten atomically, and a fixed-size
 * mmap'd open-addressing set in `seen.bin`.
 *
 * seen.bin is created by ftruncate(), so a fresh file is all zeroes. That
 * makes 0 the "empty slot" sentinel, which in turn means state_key() must
 * never return 0 -- see the fold at the bottom of the function.
 */

/* Probe depth (SEEN_PROBE, config.h) is bounded on purpose: the set never grows
 * and never compacts, so a long chain must cost a fixed number of loads, not an
 * unbounded walk over 65536 slots. */

#define SEEN_BYTES ((size_t)SEEN_CAPACITY * sizeof(uint64_t))

#define ETAGS_NAME     "etags"
#define ETAGS_TMP_NAME "etags.tmp"
#define SEEN_NAME      "seen.bin"

/* A valid line always fits; anything longer is garbage by definition. */
#define ETAGS_LINE_MAX (STATE_REPO_MAX + HTTP_ETAG_MAX + 64)

static int path_join(char *out, size_t outlen, const char *dir, const char *name)
{
    int n = snprintf(out, outlen, "%s/%s", dir, name);

    if (n < 0 || (size_t)n >= outlen)
        return -ENAMETOOLONG;
    return 0;
}

static int resolve_dir(char *out, size_t outlen)
{
    const char *xdg = env_or_null("XDG_STATE_HOME");
    const char *home;
    int n;

    if (xdg != NULL) {
        n = snprintf(out, outlen, "%s/issuewatch", xdg);
    } else {
        home = env_or_null("HOME");
        if (home == NULL)
            return -ENOENT;
        n = snprintf(out, outlen, "%s/.local/state/issuewatch", home);
    }

    if (n < 0 || (size_t)n >= outlen)
        return -ENAMETOOLONG;
    return 0;
}

/* mkdir -p, 0700 all the way down: the state dir holds nothing secret, but
 * neither does anything else need to read it. */
static int mkdir_p(const char *path)
{
    char tmp[STATE_PATH_MAX];
    size_t i, len;
    int n;

    n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return -ENAMETOOLONG;
    len = (size_t)n;

    for (i = 1; i <= len; i++) {
        char c;

        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;

        c = tmp[i];
        tmp[i] = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -errno;
        tmp[i] = c;
    }
    return 0;
}

repo_state_t *state_repo(state_t *st, const char *repo)
{
    size_t i;

    if (st == NULL || st->repos == NULL || repo == NULL)
        return NULL;

    for (i = 0; i < st->n_repos; i++) {
        if (strcmp(st->repos[i].repo, repo) == 0)
            return &st->repos[i];
    }
    return NULL;
}

/*
 * Best-effort load. Every parse failure is a skipped line, never a fatal:
 * losing an ETag costs one conditional request, losing the daemon costs the
 * whole cycle. Repos in the file that are no longer configured are dropped.
 */
static void load_etags(state_t *st)
{
    char path[STATE_PATH_MAX];
    char line[ETAGS_LINE_MAX];
    FILE *f;
    unsigned long lineno = 0;

    if (path_join(path, sizeof path, st->dir, ETAGS_NAME) != 0)
        return;

    f = fopen(path, "r");
    if (f == NULL) {
        if (errno != ENOENT)
            LOGW("state: cannot read %s: %s", path, strerror(errno));
        return;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        char *tab1, *tab2, *etag, *watermark;
        repo_state_t *rs;
        size_t len;
        time_t ignored;

        lineno++;
        len = strlen(line);

        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        } else if (!feof(f)) {
            int c;

            /* Overlong line: drop it and the rest of the physical line. */
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
            LOGW("state: etags line %lu too long, skipped", lineno);
            continue;
        }

        if (len == 0)
            continue;

        tab1 = strchr(line, '\t');
        if (tab1 == NULL) {
            LOGW("state: etags line %lu malformed, skipped", lineno);
            continue;
        }
        *tab1 = '\0';
        tab2 = strchr(tab1 + 1, '\t');
        if (tab2 == NULL) {
            LOGW("state: etags line %lu truncated, skipped", lineno);
            continue;
        }
        *tab2 = '\0';
        etag = tab1 + 1;
        watermark = tab2 + 1;

        if (line[0] == '\0' || strlen(line) >= STATE_REPO_MAX ||
            strlen(etag) >= HTTP_ETAG_MAX || strlen(watermark) >= 32) {
            LOGW("state: etags line %lu has an oversized field, skipped", lineno);
            continue;
        }
        if (watermark[0] != '\0' && iso8601_parse(watermark, &ignored) != 0) {
            LOGW("state: etags line %lu has a bad watermark, skipped", lineno);
            continue;
        }

        rs = state_repo(st, line);
        if (rs == NULL)
            continue; /* no longer in REPOS[]: drop it silently */

        /* Lengths are bounded above, so these cannot truncate. */
        snprintf(rs->etag, sizeof rs->etag, "%s", etag);
        snprintf(rs->watermark, sizeof rs->watermark, "%s", watermark);
        rs->dirty = 0;
    }

    if (ferror(f))
        LOGW("state: error reading %s", path);
    fclose(f);
}

static int seen_open(state_t *st)
{
    char path[STATE_PATH_MAX];
    void *m;
    int fd, rc;

    rc = path_join(path, sizeof path, st->dir, SEEN_NAME);
    if (rc != 0)
        return rc;

    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -errno;

    /* Fixed capacity forever: a fresh file is zero-filled, an existing one is
     * already this size, so this is a no-op after the first run. */
    if (ftruncate(fd, (off_t)SEEN_BYTES) != 0) {
        rc = -errno;
        close(fd);
        return rc;
    }

    m = mmap(NULL, SEEN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        rc = -errno;
        close(fd);
        return rc;
    }

    st->seen = m;
    st->seen_fd = fd;
    return 0;
}

int state_open(state_t *st, const char *const *repos, size_t n_repos)
{
    char watermark[32];
    size_t i;
    int rc;

    if (st == NULL || repos == NULL || n_repos == 0)
        return -EINVAL;

    memset(st, 0, sizeof *st);
    st->seen_fd = -1;

    rc = resolve_dir(st->dir, sizeof st->dir);
    if (rc != 0)
        return rc;
    rc = mkdir_p(st->dir);
    if (rc != 0)
        return rc;

    /* Startup allocation, which CLAUDE.md permits; the hot path never does. */
    st->repos = calloc(n_repos, sizeof *st->repos);
    if (st->repos == NULL)
        return -ENOMEM;
    st->n_repos = n_repos;

    if (iso8601_format(time(NULL) - GH_FIRST_RUN_LOOKBACK, watermark,
                       sizeof watermark) != 0) {
        rc = -EINVAL;
        goto out;
    }

    for (i = 0; i < n_repos; i++) {
        if (repos[i] == NULL || repos[i][0] == '\0' ||
            strlen(repos[i]) >= STATE_REPO_MAX) {
            LOGE("state: REPOS[%zu] is empty or longer than %d bytes", i,
                 STATE_REPO_MAX - 1);
            rc = -EINVAL;
            goto out;
        }
        snprintf(st->repos[i].repo, sizeof st->repos[i].repo, "%s", repos[i]);
        st->repos[i].etag[0] = '\0';
        snprintf(st->repos[i].watermark, sizeof st->repos[i].watermark, "%s",
                 watermark);
        /* Dirty so the first-run baseline reaches disk even if the cycle
         * finds nothing: a lookback recomputed from scratch on every start
         * would re-scan the same week forever. */
        st->repos[i].dirty = 1;
    }

    load_etags(st);

    rc = seen_open(st);
    if (rc != 0)
        goto out;

    return 0;

out:
    free(st->repos);
    memset(st, 0, sizeof *st);
    st->seen_fd = -1;
    return rc;
}

int state_flush(state_t *st)
{
    char final_path[STATE_PATH_MAX];
    char tmp_path[STATE_PATH_MAX];
    FILE *f = NULL;
    size_t i;
    int fd = -1, dir_fd, rc = 0, dirty = 0;

    if (st == NULL || st->repos == NULL)
        return -EINVAL;

    for (i = 0; i < st->n_repos; i++) {
        if (st->repos[i].dirty)
            dirty = 1;
    }
    if (!dirty)
        return 0;

    rc = path_join(final_path, sizeof final_path, st->dir, ETAGS_NAME);
    if (rc != 0)
        return rc;
    rc = path_join(tmp_path, sizeof tmp_path, st->dir, ETAGS_TMP_NAME);
    if (rc != 0)
        return rc;

    /* Temp file in the same directory so rename(2) is atomic. */
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -errno;

    f = fdopen(fd, "w");
    if (f == NULL) {
        rc = -errno;
        close(fd);
        goto out;
    }

    for (i = 0; i < st->n_repos; i++) {
        if (fprintf(f, "%s\t%s\t%s\n", st->repos[i].repo, st->repos[i].etag,
                    st->repos[i].watermark) < 0) {
            rc = -EIO;
            goto out;
        }
    }

    /* fflush before fsync: a short write buffered in stdio would otherwise be
     * fsync'd as a truncated file and then renamed over a good one. */
    if (fflush(f) != 0 || ferror(f)) {
        rc = -EIO;
        goto out;
    }
    if (fsync(fd) != 0) {
        rc = -errno;
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        rc = -EIO;
        goto out;
    }
    f = NULL;

    if (rename(tmp_path, final_path) != 0) {
        rc = -errno;
        goto out;
    }

    /* The rename itself only becomes durable once the directory is synced. */
    dir_fd = open(st->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        if (fsync(dir_fd) != 0)
            LOGW("state: fsync on %s failed: %s", st->dir, strerror(errno));
        close(dir_fd);
    }

    for (i = 0; i < st->n_repos; i++)
        st->repos[i].dirty = 0;

out:
    if (f != NULL)
        fclose(f);
    if (rc != 0)
        unlink(tmp_path);
    return rc;
}

int state_close(state_t *st)
{
    int rc = 0, r;

    if (st == NULL)
        return -EINVAL;

    if (st->repos != NULL) {
        r = state_flush(st);
        if (r != 0) {
            LOGE("state: final flush failed: %s", strerror(-r));
            rc = r;
        }
    }

    if (st->seen != NULL) {
        if (msync(st->seen, SEEN_BYTES, MS_SYNC) != 0 && rc == 0)
            rc = -errno;
        if (munmap(st->seen, SEEN_BYTES) != 0 && rc == 0)
            rc = -errno;
    }
    if (st->seen_fd >= 0)
        close(st->seen_fd);

    free(st->repos);
    memset(st, 0, sizeof *st);
    st->seen_fd = -1; /* 0 is a real descriptor; -1 is the empty marker */
    return rc;
}

uint64_t state_key(long long issue_id, const char *updated_at)
{
    char buf[64];
    uint64_t k;
    size_t len;
    int n;

#if NOTIFY_ON_UPDATE
    /* Keying on (id, updated_at) lets an issue that gained a comment or an
     * edit notify again; keying on id alone means it never does. */
    n = snprintf(buf, sizeof buf, "%lld|%s", issue_id,
                 updated_at != NULL ? updated_at : "");
#else
    (void)updated_at;
    n = snprintf(buf, sizeof buf, "%lld", issue_id);
#endif

    if (n < 0)
        return 1;
    len = ((size_t)n < sizeof buf) ? (size_t)n : sizeof buf - 1;

    k = hash64(buf, len);

    /* seen.bin is a zero-filled mmap, so slot value 0 means "empty". A key of
     * 0 would read back as an empty slot and never dedup anything, so fold it
     * to 1 -- one wasted collision every 2^64 issues. */
    return k != 0 ? k : 1;
}

int state_seen(const state_t *st, uint64_t key)
{
    size_t slot, i;

    if (st == NULL || st->seen == NULL || key == 0)
        return 0;

    slot = (size_t)(key % (uint64_t)SEEN_CAPACITY);
    for (i = 0; i < SEEN_PROBE; i++) {
        uint64_t v = st->seen[(slot + i) % SEEN_CAPACITY];

        if (v == key)
            return 1;
        if (v == 0)
            return 0; /* an empty slot ends the probe chain */
    }
    return 0;
}

void state_mark_seen(state_t *st, uint64_t key)
{
    size_t slot, i;

    if (st == NULL || st->seen == NULL || key == 0)
        return;

    slot = (size_t)(key % (uint64_t)SEEN_CAPACITY);
    for (i = 0; i < SEEN_PROBE; i++) {
        size_t at = (slot + i) % SEEN_CAPACITY;

        if (st->seen[at] == 0 || st->seen[at] == key) {
            st->seen[at] = key;
            return;
        }
    }

    /* Chain full: overwrite the oldest resident, per CONTEXT.md section 7. The
     * set never grows and never compacts; the cost of a lost entry is one
     * duplicate notification, which is cheaper than unbounded state. */
    st->seen[slot] = key;
}
