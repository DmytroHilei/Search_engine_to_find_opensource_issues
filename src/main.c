#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/arena.h"
#include "config.h"
#include "core/board.h"
#include "net/gist.h"
#include "net/github.h"
#include "net/http.h"
#include "pipeline/judge.h"
#include "net/notify.h"
#include "pipeline/prefilter.h"
#include "pipeline/render.h"
#include "core/state.h"
#include "core/userconf.h"
#include "core/util.h"

typedef enum { MODE_ONESHOT, MODE_DAEMON } run_mode_t;

static volatile sig_atomic_t g_stop;

/*
 * The watched list, resolved from the config file at startup and then read-only
 * for the life of the process. File scope rather than a run_cycle() parameter
 * because it is fixed for the whole run: threading it through five call frames
 * would suggest it can differ between cycles, and it cannot.
 */
static const char *const *g_repos;
static size_t g_n_repos;

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
            "usage: %s [--oneshot | --daemon] [--runs N] [--model NAME]\n"
            "                  [--config PATH] [--dry-run] [-v] [-q]\n"
            "\n"
            "  --oneshot   run one poll cycle and exit (default)\n"
            "  --daemon    loop every POLL_INTERVAL_SEC; prefer a systemd timer\n"
            "  --runs N    run N cycles %ds apart, then exit (1..%d). The backfill\n"
            "              sweeps one repo per cycle, so this is how you cover\n"
            "              every watched repo without waiting days for the timer.\n"
            "  --config P  read repos, profile, keywords and credentials from P\n"
            "              (default %s). Everything else lives in src/config.h.\n"
            "  --dry-run   print notifications to stdout, send nothing\n"
            "  --model N   Ollama model to judge with (default %s).\n"
            "              %s needs ~7.2 GB of VRAM; on a smaller card try\n"
            "              %s, which fits in ~4 GB but scores real bounties 0.\n"
            "  -v          debug logging   -q  errors only\n"
            "\n"
            "environment: GH_TOKEN (required), ANTHROPIC_API_KEY (JUDGE_API,\n"
            "JUDGE_HYBRID), NTFY_TOKEN (optional, self-hosted ntfy auth)\n",
            argv0, RUNS_DELAY_SEC, RUNS_MAX, "$XDG_CONFIG_HOME/issuewatch/config",
            OLLAMA_MODEL, OLLAMA_MODEL, OLLAMA_MODEL_SMALL);
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
 * Turns this cycle's survivors into board entries and merges them in. Split out
 * because a failed judge must not take the board phase down with it -- see
 * run_cycle().
 */
static int merge_survivors(arena_t *cycle, state_t *st, board_t *board,
                           const issue_t *issues, size_t judged, const char *now_iso,
                           size_t *n_new)
{
    board_entry_t *cand;
    size_t i;

    if (judged == 0)
        return 0;

    cand = arena_calloc(cycle, judged, sizeof *cand);
    if (cand == NULL) {
        LOGE("arena exhausted building board candidates");
        return -1;
    }
    for (i = 0; i < judged; i++)
        gh_issue_to_board(&issues[i], &cand[i]);

    return board_merge(board, st, cand, judged, now_iso, n_new);
}

/*
 * One poll cycle. Everything transient comes from `cycle`, which the caller
 * resets afterwards -- that reset is the only deallocation in the program.
 *
 * The board phase runs even when the fetch found nothing. A quiet cycle is
 * exactly when the board is most likely to be wrong: no new issues arrived, but
 * the ones already on it have aged, and some of them have been claimed by
 * somebody else since the last look.
 */
static int run_cycle(arena_t *cycle, state_t *st, board_t *board, const ac_t *ac,
                     int dry_run)
{
    char now_iso[32];
    issue_t *issues = NULL;
    const char *markdown;
    time_t now = time(NULL);
    time_t t_start = now, t_phase, t_fetch = 0, t_judge = 0;
    size_t n = 0, kept = 0, judged = 0, n_new = 0, expired;
    int sent, dropped, judge_failed = 0;

    if (iso8601_format(now, now_iso, sizeof now_iso) != 0) {
        LOGE("cannot format the current time");
        return -1;
    }

    t_phase = time(NULL);
    if (gh_fetch_all(cycle, st, g_repos, g_n_repos, &issues, &n) != 0) {
        LOGE("fetch failed, abandoning cycle");
        return -1;
    }
    t_fetch = time(NULL) - t_phase;
    LOGI("fetched %zu candidate issues", n);
    t_phase = time(NULL);

    if (n > 0) {
        /* Before the prefilter, not after: an issue somebody already holds is
         * worth nothing regardless of score, so it should cost no LLM tokens. */
        size_t open_n = gh_drop_assigned(issues, n);

        if (open_n != n)
            LOGI("dropped %zu already-assigned issue(s)", n - open_n);
        n = open_n;
    }

    if (n > 0) {
        kept = prefilter_apply(ac, issues, n);
        LOGI("prefilter kept %zu/%zu", kept, n);
    }

    if (kept > 0) {
        if (judge_batch(cycle, issues, kept) != 0) {
            /*
             * Every batch failed: the model is down or unreachable. The
             * watermark must not move -- these issues have to be re-judged next
             * cycle -- but the board phase still runs. A judge that is down for
             * a day would otherwise leave the board advertising bounties that
             * were claimed hours ago, which is the exact failure it exists to
             * prevent.
             */
            LOGE("judge failed for every batch, not advancing watermarks");
            judge_failed = 1;
        } else {
            /* Before judge_apply(), which compacts the sub-threshold ones away
             * and takes the distribution with them. */
            judge_log_scores(issues, kept);
            judged = judge_apply(issues, kept);
            LOGI("judge kept %zu/%zu", judged, kept);
        }
        t_judge = time(NULL) - t_phase;
    }

    board_clear_fresh(board);
    if (merge_survivors(cycle, st, board, issues, judged, now_iso, &n_new) < 0)
        return -1;

    /* The only thing in the program that can learn an issue was closed or
     * claimed. Transport failures here keep entries, so a blip cannot empty
     * the board. */
    dropped = gh_recheck(cycle, board, st, dry_run);
    if (dropped < 0) {
        LOGE("re-check failed to run, not advancing watermarks");
        return -1;
    }
    expired = board_expire(board, now);
    board_rank(board);
    LOGI("board: %zu open, %zu new, %d dropped, %zu expired", board->n, n_new,
         dropped, expired);

    markdown = render_board(cycle, board, now);
    if (markdown == NULL) {
        /* Never publish a truncated board: it would read as "these are all the
         * open bounties", which is worse than yesterday's board. */
        LOGE("render overflowed, skipping the publish");
        return -1;
    }

    if (gist_publish(cycle, markdown, dry_run) < 0) {
        LOGE("gist publish failed, not advancing watermarks");
        return -1;
    }

    if (NOTIFY_SUMMARY_ONLY)
        sent = notify_summary(cycle, board, n_new, dry_run);
    else
        sent = notify_cycle(cycle, st, issues, judged, dry_run);
    if (sent < 0) {
        LOGE("notify failed, not advancing watermarks");
        return -1;
    }
    LOGI("notified %d", sent);

    /*
     * One line that says what the cycle did end to end, because the per-phase
     * lines above are interleaved with warnings and a reader weeks later is
     * asking one question: why did or did not my phone buzz. Timings are here
     * too -- the judge dominates a cycle by an order of magnitude, and that is
     * only obvious when the numbers sit next to each other.
     */
    LOGI("cycle: %zu fetched -> %zu prefiltered -> %zu judged -> board %zu "
         "(+%zu new, -%d dropped, -%zu expired) -> %d push(es) in %llds "
         "(fetch %llds, judge %llds)%s",
         n, kept, judged, board->n, n_new, dropped, expired, sent,
         (long long)(time(NULL) - t_start), (long long)t_fetch,
         (long long)t_judge, dry_run ? " [dry-run, nothing written]" : "");

    if (!dry_run) {
        int rc = board_flush(board);

        if (rc != 0) {
            LOGE("board flush failed: %s", strerror(-rc));
            return -1;
        }
    }

    /* A judge outage published a correct board but must still re-judge, so the
     * watermark stays where it was. */
    if (judge_failed)
        return -1;

    /* Only now, after the whole cycle including the publish succeeded, may the
     * watermarks and ETags move. A crash before this point re-processes; a
     * commit before this point would silently skip issues forever. */
    return commit_cycle(st, dry_run);
}

/*
 * Absolute-time sleep of `secs`, interruptible by a signal. Absolute rather
 * than relative so a signal that interrupts it cannot extend the deadline --
 * a relative sleep restarted after each EINTR drifts by however long the
 * handler took, every time.
 *
 * Returns 0 if it slept the whole span, -1 if g_stop was set.
 */
static int sleep_seconds(long secs)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    ts.tv_sec += secs;

    while (!g_stop) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

        if (rc == 0)
            return 0;
        if (rc == EINTR)
            continue;   /* a signal; g_stop decides whether to bail */
        LOGW("clock_nanosleep: %s", strerror(rc));
        return 0;
    }
    return -1;
}

/* The daemon's inter-cycle sleep: the poll interval plus jitter, so every
 * install does not hit the API on the same second. */
static void sleep_until_next(void)
{
    long jitter = 0;

    if (POLL_JITTER_SEC > 0)
        jitter = random() % (POLL_JITTER_SEC + 1);

    LOGI("sleeping %ld s", (long)POLL_INTERVAL_SEC + jitter);
    (void)sleep_seconds(POLL_INTERVAL_SEC + jitter);
}

int main(int argc, char **argv)
{
    run_mode_t mode = MODE_ONESHOT;
    int dry_run = 0;
    long runs = 1, done = 0, failed = 0;
    const char *model = NULL;
    const char *conf_path = NULL;
    userconf_t cfg;
    int rc = EXIT_FAILURE, cycle_rc;
    arena_t perm = {0}, cycle = {0};
    state_t st = {0};
    board_t board = {0};
    ac_t *ac = NULL;
    int state_ready = 0, http_ready = 0, board_ready = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--oneshot") == 0)      mode = MODE_ONESHOT;
        else if (strcmp(argv[i], "--daemon") == 0)  mode = MODE_DAEMON;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--model needs a name, e.g. " OLLAMA_MODEL_SMALL "\n");
                return EXIT_FAILURE;
            }
            model = argv[++i];          /* argv outlives the process */
        }
        else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--config needs a path\n");
                return EXIT_FAILURE;
            }
            conf_path = argv[++i];
        }
        else if (strcmp(argv[i], "--runs") == 0) {
            char *end;

            if (i + 1 >= argc) {
                fprintf(stderr, "--runs needs a count\n");
                return EXIT_FAILURE;
            }
            errno = 0;
            runs = strtol(argv[++i], &end, 10);
            if (errno != 0 || *end != '\0' || runs < 1 || runs > RUNS_MAX) {
                fprintf(stderr, "--runs wants 1..%d, got '%s'\n", RUNS_MAX, argv[i]);
                return EXIT_FAILURE;
            }
        }
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

    /* An argv contradiction, so it fails here rather than after opening state
     * and discovering GH_TOKEN is missing -- the wrong error for the mistake. */
    if (mode == MODE_DAEMON && runs != 1) {
        fprintf(stderr, "--runs and --daemon mean different things; pick one\n");
        return EXIT_FAILURE;
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

    /*
     * Before every init below: the config supplies the repo list, the profile
     * spliced into the prompts, the keyword table and both credentials, so
     * nothing that consumes them may run first. A file that exists but does not
     * parse stops the process -- the alternative is silently polling the
     * author's repositories instead of the user's.
     */
    if (userconf_load(&perm, conf_path, &cfg) != 0) {
        LOGE("config unusable; fix it or delete it to fall back to the built-in");
        goto out;
    }

    g_repos = cfg.repos;
    g_n_repos = cfg.n_repos;

    /* Before judge_init(), which assembles the prompts and logs the model. */
    judge_set_model(model);
    judge_set_profile(cfg.profile);
    if (judge_init() != 0) {
        LOGE("judge backend unavailable");
        goto out;
    }

    /* A default ntfy topic is world-writable: anyone who guesses it can push to
     * the user's phone. Fatal for a real run, tolerable when only printing. */
    if (notify_init(cfg.ntfy_topic, cfg.gist_id) != 0) {
        if (!dry_run) {
            LOGE("notify backend unusable; set ntfy-topic in your config file");
            goto out;
        }
        LOGW("notify backend unusable, continuing because --dry-run");
    }

    /* Same posture as notify_init(): a placeholder GIST_ID is fatal for a real
     * run, because the board is the output, but --dry-run only prints it. */
    if (gist_init(cfg.gist_id) != 0) {
        if (!dry_run) {
            LOGE("board publishing unusable; set gist-id in your config file");
            goto out;
        }
        LOGW("board publishing unusable, continuing because --dry-run");
    }

    if (http_global_init() != 0) {
        LOGE("http_global_init failed");
        goto out;
    }
    http_ready = 1;

    if (prefilter_init(&perm, cfg.keywords, cfg.n_keywords, &ac) != 0) {
        LOGE("prefilter_init failed (perm arena too small?)");
        goto out;
    }

    if (state_open(&st, cfg.repos, cfg.n_repos) != 0) {
        LOGE("state_open failed");
        goto out;
    }
    state_ready = 1;
    /* Belt to commit_cycle()'s braces: that skips the flush a cycle asks for,
     * this stops state_close() writing the seeded watermarks on the way out. */
    st.read_only = dry_run;

    /* Shares the state directory: board.tsv sits next to etags and seen.bin,
     * and gets the same atomic-rewrite treatment. */
    if (board_open(&board, st.dir) != 0) {
        LOGE("board_open failed");
        goto out;
    }
    board_ready = 1;

    if (runs > 1)
        LOGI("issuewatch starting: %zu repos, %ld runs %ds apart%s",
             cfg.n_repos, runs, RUNS_DELAY_SEC, dry_run ? ", dry-run" : "");
    else
        LOGI("issuewatch starting: %zu repos, mode=%s%s", cfg.n_repos,
             mode == MODE_DAEMON ? "daemon" : "oneshot", dry_run ? ", dry-run" : "");

    for (;;) {
        cycle_rc = run_cycle(&cycle, &st, &board, ac, dry_run);
        /* Log after the reset: arena_reset is what folds `used` into `peak`,
         * so reading peak first reports the previous cycle's high-water mark. */
        arena_reset(&cycle);
        LOGD("cycle arena: %zu peak of %zu, %zu refused allocations",
             cycle.peak, cycle.cap, cycle.failures);

        if (mode == MODE_ONESHOT) {
            if (cycle_rc != 0)
                failed++;
            done++;
            /*
             * Report a failure if ANY cycle failed, not just the last. A burst
             * is usually advancing the backfill rotation unattended, and
             * exiting 0 because the final cycle happened to be clean would hide
             * that a repo in the middle never got swept.
             */
            if (done >= runs || g_stop) {
                if (runs > 1)
                    LOGI("runs: %ld of %ld cycle(s) completed, %ld failed",
                         done, runs, failed);
                rc = (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
                break;
            }
            /*
             * No arena_destroy/malloc_trim here, unlike the daemon path below:
             * that exists to keep idle RSS at zero across a multi-hour sleep,
             * and releasing 96 MB only to fault it straight back in ten seconds
             * later is pure cost. The reset above is enough.
             */
            LOGI("runs: cycle %ld of %ld done, next in %ds", done, runs,
                 RUNS_DELAY_SEC);
            if (sleep_seconds(RUNS_DELAY_SEC) != 0) {
                LOGI("runs: interrupted after %ld of %ld cycle(s), %ld failed",
                     done, runs, failed);
                rc = (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
                break;
            }
            continue;
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
    if (board_ready)
        board_close(&board);
    if (state_ready)
        state_close(&st);
    if (http_ready)
        http_global_cleanup();
    arena_destroy(&cycle);
    arena_destroy(&perm);
    return rc;
}
