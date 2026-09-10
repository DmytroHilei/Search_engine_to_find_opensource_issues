#define CONFIG_WANT_REPOS

#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "config.h"
#include "github.h"
#include "http.h"
#include "judge.h"
#include "notify.h"
#include "prefilter.h"
#include "state.h"
#include "util.h"

typedef enum { MODE_ONESHOT, MODE_DAEMON } run_mode_t;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    /* No SA_RESTART: clock_nanosleep must return EINTR so a TERM lands promptly. */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* A dead ntfy or Ollama connection must not take the process down. */
    signal(SIGPIPE, SIG_IGN);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--oneshot | --daemon] [--dry-run] [-v] [-q]\n"
            "\n"
            "  --oneshot   run one poll cycle and exit (default)\n"
            "  --daemon    loop every POLL_INTERVAL_SEC; prefer a systemd timer\n"
            "  --dry-run   print notifications to stdout, send nothing\n"
            "  -v          debug logging   -q  errors only\n"
            "\n"
            "environment: GH_TOKEN (required), ANTHROPIC_API_KEY (JUDGE_API,\n"
            "JUDGE_HYBRID), NTFY_TOKEN (optional, self-hosted ntfy auth)\n",
            argv0);
}

/*
 * Committing state is what makes a cycle irreversible, so a dry run must not do
 * it. --dry-run exists to tune keyword weights over repeated passes across the
 * same issues (CONTEXT.md section 13); advancing the watermark would move
 * `since=` past them and hand every tuning pass a different, smaller sample.
 * This mirrors notify.c, which does not mark the seen-set on a dry run either.
 */
static int commit_cycle(state_t *st, int dry_run)
{
    if (dry_run) {
        LOGD("dry-run: leaving watermarks and etags untouched");
        return 0;
    }
    gh_commit_watermarks(st);
    return state_flush(st);
}

/*
 * One poll cycle. Everything transient comes from `cycle`, which the caller
 * resets afterwards -- that reset is the only deallocation in the program.
 */
static int run_cycle(arena_t *cycle, state_t *st, const ac_t *ac, int dry_run)
{
    issue_t *issues = NULL;
    size_t n = 0, kept, judged;
    int sent;

    if (gh_fetch_all(cycle, st, REPOS, N_REPOS, &issues, &n) != 0) {
        LOGE("fetch failed, abandoning cycle");
        return -1;
    }
    LOGI("fetched %zu candidate issues", n);

    if (n == 0) {
        /* Nothing changed anywhere -- all 304s. Still commit, so the
         * watermarks advance past a quiet interval. */
        return commit_cycle(st, dry_run);
    }

    kept = prefilter_apply(ac, issues, n);
    LOGI("prefilter kept %zu/%zu", kept, n);
    if (kept == 0)
        return commit_cycle(st, dry_run);

    if (judge_batch(cycle, issues, kept) != 0) {
        /* Every batch failed: the model is down or unreachable. Do NOT advance
         * the watermark -- these issues must be re-judged next cycle. */
        LOGE("judge failed for every batch, not advancing watermarks");
        return -1;
    }

    judged = judge_apply(issues, kept);
    LOGI("judge kept %zu/%zu", judged, kept);

    sent = notify_cycle(cycle, st, issues, judged, dry_run);
    if (sent < 0) {
        LOGE("notify failed, not advancing watermarks");
        return -1;
    }
    LOGI("notified %d", sent);

    /* Only now, after the whole cycle including notification succeeded, may the
     * watermarks and ETags move. A crash before this point re-processes; a
     * commit before this point would silently skip issues forever. */
    return commit_cycle(st, dry_run);
}

/* Absolute-time sleep so the schedule cannot drift, interruptible by a signal. */
static void sleep_until_next(void)
{
    struct timespec ts;
    long jitter = 0;

    if (POLL_JITTER_SEC > 0)
        jitter = random() % (POLL_JITTER_SEC + 1);

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return;
    ts.tv_sec += POLL_INTERVAL_SEC + jitter;

    LOGI("sleeping %ld s", (long)POLL_INTERVAL_SEC + jitter);
    while (!g_stop) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

        if (rc == 0)
            return;
        if (rc == EINTR)
            continue;   /* a signal; g_stop decides whether to bail */
        LOGW("clock_nanosleep: %s", strerror(rc));
        return;
    }
}

int main(int argc, char **argv)
{
    run_mode_t mode = MODE_ONESHOT;
    int dry_run = 0;
    int rc = EXIT_FAILURE, cycle_rc;
    arena_t perm = {0}, cycle = {0};
    state_t st = {0};
    ac_t *ac = NULL;
    int state_ready = 0, http_ready = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--oneshot") == 0)      mode = MODE_ONESHOT;
        else if (strcmp(argv[i], "--daemon") == 0)  mode = MODE_DAEMON;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (strcmp(argv[i], "-v") == 0)        log_set_level(LOG_DEBUG);
        else if (strcmp(argv[i], "-q") == 0)        log_set_level(LOG_ERR);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    install_signals();
    srandom((unsigned)(time(NULL) ^ (long)getpid()));

    if (env_or_null("GH_TOKEN") == NULL) {
        LOGE("GH_TOKEN is not set. Unauthenticated GitHub is 60 req/hr and the "
             "ETag path needs a token; refusing to start.");
        return EXIT_FAILURE;
    }

    if (arena_init(&perm, PERM_ARENA_SIZE) != 0 ||
        arena_init(&cycle, ARENA_SIZE) != 0) {
        LOGE("arena_init failed");
        goto out;
    }

    if (judge_init() != 0) {
        LOGE("judge backend unavailable");
        goto out;
    }

    /* A default ntfy topic is world-writable: anyone who guesses it can push to
     * the user's phone. Fatal for a real run, tolerable when only printing. */
    if (notify_init() != 0) {
        if (!dry_run) {
            LOGE("notify backend unusable; set NTFY_TOPIC in config.h");
            goto out;
        }
        LOGW("notify backend unusable, continuing because --dry-run");
    }

    if (http_global_init() != 0) {
        LOGE("http_global_init failed");
        goto out;
    }
    http_ready = 1;

    if (prefilter_init(&perm, &ac) != 0) {
        LOGE("prefilter_init failed (perm arena too small?)");
        goto out;
    }

    if (state_open(&st, REPOS, N_REPOS) != 0) {
        LOGE("state_open failed");
        goto out;
    }
    state_ready = 1;

    LOGI("issuewatch starting: %zu repos, mode=%s%s", (size_t)N_REPOS,
         mode == MODE_DAEMON ? "daemon" : "oneshot", dry_run ? ", dry-run" : "");

    for (;;) {
        cycle_rc = run_cycle(&cycle, &st, ac, dry_run);
        /* Log after the reset: arena_reset is what folds `used` into `peak`,
         * so reading peak first reports the previous cycle's high-water mark. */
        arena_reset(&cycle);
        LOGD("cycle arena: %zu peak of %zu, %zu refused allocations",
             cycle.peak, cycle.cap, cycle.failures);

        if (mode == MODE_ONESHOT) {
            rc = (cycle_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
            break;
        }
        if (g_stop) {
            rc = EXIT_SUCCESS;
            break;
        }

        /*
         * Release the arena outright rather than just resetting it. malloc_trim
         * cannot return a live allocation, so a reset-only arena would keep the
         * cycle's whole ARENA_SIZE resident for the entire multi-hour sleep --
         * exactly the idle RSS that CONTEXT.md section 9 claims we avoid. Freeing
         * it first is what makes the trim meaningful.
         */
        arena_destroy(&cycle);
        malloc_trim(0);

        sleep_until_next();
        if (g_stop) {
            rc = EXIT_SUCCESS;
            break;
        }

        if (arena_init(&cycle, ARENA_SIZE) != 0) {
            LOGE("arena_init failed after sleep");
            goto out;
        }
    }

    if (g_stop)
        LOGI("signal received, shutting down");

out:
    if (state_ready)
        state_close(&st);
    if (http_ready)
        http_global_cleanup();
    arena_destroy(&cycle);
    arena_destroy(&perm);
    return rc;
}
