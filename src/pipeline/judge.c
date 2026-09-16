#include "pipeline/judge.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "yyjson.h"

#include "config.h"
#include "net/http.h"
#include "core/util.h"

/*
 * One backend is selected by JUDGE_MODE; the unselected ones are #if'd out so
 * they cost nothing. What is *not* guarded is the pure-JSON layer -- prompt
 * assembly, the verdict schema, envelope extraction and verdict application --
 * because none of it touches a backend and all of it is what the fixture tests
 * exercise. A test binary must be able to parse an Anthropic envelope in a
 * JUDGE_LOCAL build, so those two extractors are shared and non-static.
 */

#define ANTHROPIC_TOOL_NAME "report_verdicts"
#define ANTHROPIC_MAX_TOKENS 4096

/*
 * yyjson scratch. One buffer per judge_batch() call, re-initialised before each
 * batch: a bump arena never hands memory back mid-cycle, so N batches must not
 * cost N pools. Exhaustion drops the batch, exactly like an unparseable reply.
 */
#define JUDGE_POOL_BYTES   (256u * 1024u)
#define JUDGE_EXTRACT_POOL (32u * 1024u)

/*
 * Deliberately non-static and absent from judge.h: the fixture tests need both
 * extractors in any JUDGE_MODE build, and judge.h is a frozen contract.
 */
int judge_extract_ollama_verdicts(arena_t *a, const char *json, size_t json_len,
                                  const char **out, size_t *out_len);
int judge_extract_anthropic_verdicts(arena_t *a, const char *json, size_t json_len,
                                     const char **out, size_t *out_len);

/* Result of judge_ollama_window(). Same visibility, same reason. */
#define JUDGE_WINDOW_OK        0
#define JUDGE_WINDOW_NEAR      1   /* fits, but within OLLAMA_CTX_WARN_PCT of the edge */
#define JUDGE_WINDOW_TRUNCATED 2   /* Ollama cut the prompt; the verdicts are unsafe */
#define JUDGE_WINDOW_UNKNOWN   3   /* no token counts in the reply to judge by */
int judge_ollama_window(const char *json, size_t json_len, size_t prompt_bytes,
                        long *used_tokens);

#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID
static int ollama_window_gate(const http_resp_t *resp, const char *system,
                              const char *prompt);
#endif

static const char ELISION[] = "\n[...]\n";

/*
 * Context-window tallies for one judge_batch() call, summarised once at its
 * end. Per batch they would repeat the same line up to 26 times a cycle, which
 * is how a real warning gets scrolled past. Single-threaded, so file scope is
 * safe; judge_batch() zeroes them on entry.
 */
static int  g_win_truncated;
static int  g_win_near;
static long g_win_peak;

/* Points at OLLAMA_MODEL unless --model replaced it; see judge_set_model(). */
static const char *g_model = OLLAMA_MODEL;

void judge_set_model(const char *name)
{
    if (name != NULL && name[0] != '\0')
        g_model = name;
}

_Static_assert(LLM_BODY_TRUNC > LLM_BODY_HEAD + (int)sizeof ELISION,
               "LLM_BODY_TRUNC must leave room for LLM_BODY_HEAD plus the elision marker");
_Static_assert(LLM_BATCH_SIZE > 0, "LLM_BATCH_SIZE must be positive");
_Static_assert(LLM_WHY_MAX > 0, "LLM_WHY_MAX must be positive");

/*
 * Every line here was measured by rescoring the same 60 real issues off a live
 * board, qwen3:8b at batch 4. Change it the same way, not by reading it.
 *
 * The shape of the rubric is what was bought. "8-10: concrete, reproducible,
 * actionable right now" describes a good issue, and describing does not
 * separate: it is true of nearly every open bug in these repos, so the judge
 * kept 40 of the 60 and put 14 of them in the 9-10 band -- a band reserved for
 * money, on a set containing no paid issue at all. Naming what must be PRESENT
 * IN THE PAYLOAD separates. With the three criteria, the prior that ordinary is
 * 4-5, and payment moved out of the model's hands, the same 60 issues come back
 * 27 kept, 8 at the top of the unpaid range, none above it, and zero reasons
 * claiming a bounty that does not exist (it was 14 of 60).
 *
 * Two things that look like improvements and are not, both tried here:
 *
 *   - Naming the criteria (REPRODUCER / LOCATION / ROUTE) so the model could
 *     cite them. It answered "REPRODUCER, LOCATION, ROUTE" for 59 of 60 rows.
 *     A name in this prompt is a phrase to echo, which is also why step 1 no
 *     longer calls its band "a cash bounty".
 *   - "Set keep=false only for the 0-2 band", to stop ~19 of 60 being zeroed.
 *     keep=false went to 57 of 60. Making the flag more salient made the model
 *     reach for it.
 */
#define JUDGE_PROMPT_HEAD                                                         \
    "You triage GitHub issues for one developer. Answer only through the "        \
    "structured verdict list.\n"                                                  \
    "Developer profile: "

/* The profile is spliced in between HEAD and TAIL at judge_init(), because it
 * comes from the user's config file now and is not a literal to concatenate. */
#define JUDGE_PROMPT_TAIL                                                         \
    "\n"                                                                          \
    "Score 0-10 for how much this developer wants this issue on their phone now, "\
    "in two steps.\n"                                                             \
    "STEP 1. Read the `payment:` line of the issue. It was computed from the "    \
    "title and the labels before you saw it and it is authoritative -- do not "   \
    "re-derive it from the body, and do not reason that the project runs a "      \
    "bounty programme or that the work is valuable.\n"                            \
    "  payment: YES ..... score 10, and you are done.\n"                          \
    "  payment: none .... go to step 2, and do not use the words paid or "        \
    "bounty about this issue at all.\n"                                           \
    "STEP 2. Unpaid, so the score is 0-8 and cannot be 9 or 10 however good the " \
    "work is. Most open issues in these repositories are ordinary bug reports. "  \
    "Ordinary is 4-5, and scoring everything 7 is the failure here: the score "   \
    "has to separate, not approve. Count how many of these three the payload "    \
    "actually shows -- if you cannot point at the text that shows one, it is "    \
    "absent:\n"                                                                   \
    "  - a failing case, command, input, stack trace or measured number, not "    \
    "merely a description of the symptom;\n"                                      \
    "  - the name of the file, kernel, function, flag or API it lives in;\n"      \
    "  - a stated cause, a proposed fix, or a design the maintainers accepted.\n" \
    "  0-2  noise: bots, dependency bumps, typos, docs, other platforms\n"        \
    "  3-5  on-topic project, ordinary report: it shows none or one of them\n"    \
    "  6-7  two of them, in this developer's areas\n"                             \
    "  8    all three, in this developer's areas\n"                               \
    "Then read the `last activity:` line, which is also computed for you. An "    \
    "issue marked ABANDONED has had no activity in a year: whatever it shows, "   \
    "nobody is working on it or reviewing it, so score it 5 at most. A thin "     \
    "issue somebody replied to yesterday is worth more than a perfect one "       \
    "everybody stopped answering.\n"                                              \
    "Between two defensible scores in step 2, give the lower one.\n"              \
    "Set keep=false for anything you would not push to them at all.\n"            \
    "`why` is required either way, including for a paid issue: say what the "     \
    "work is, never that it is paid -- the board already shows that.\n"           \
    "`why` is the notification body: one clause, at most 12 words, no markdown, "  \
    "no preamble, no field names and no labelled list. Write what a colleague "    \
    "would say in passing -- \"fp32 pow returns +inf for |exp|>16\", \"no "        \
    "reproducer, symptom only\". Never restate the criteria above as your "        \
    "answer. State only what the payload shows.\n"                                 \
    "Return exactly one verdict per listed index, using the 0-based index given."

/*
 * Whether an issue is paid is a substring search, and the model is bad at it in
 * both directions. Rescoring one real board, an 8B judge put pytorch#59515 --
 * labels "module: cuda", "triaged", no money anywhere -- at 9 for an "unclaimed,
 * unassigned cash bounty" it invented, while reading "[Bounty] Outline of NVIDIA
 * e2e full FP16 matmul" as unpaid and scoring a genuine tinygrad bounty 8. One
 * of those costs an evening and the other is the failure this program exists to
 * prevent, so the answer is computed here, passed into the prompt, and enforced
 * on the way back out.
 *
 * The band edges belong next to the prompt that defines them, not in config.h:
 * changing either one without changing JUDGE_SYSTEM_PROMPT's step 1 and step 2
 * produces a rubric that contradicts itself.
 */
#define JUDGE_UNPAID_CAP  8    /* top of step 2 -- unpaid cannot outrank money */
#define JUDGE_PAID_FLOOR  9    /* step 1 says 10; 9 leaves room to disagree     */

/*
 * Staleness, the same shape: an objective fact about the payload that the model
 * cannot be asked to compute, because it means doing date arithmetic against
 * today in its head.
 *
 * It exists because the rubric's three criteria measure how well an issue is
 * WRITTEN, and a well-written issue stays well-written after everyone has
 * stopped caring about it. On a saturated 200-row board, 106 rows sat at 8 and
 * the age distribution of a 45-row sample was bimodal -- p25 fourteen days, p75
 * 1652 days -- with 16 of 45 untouched for over a year. Those read identically
 * to the fresh ones through the three criteria: reproducer, file, proposed fix,
 * all present, all still true, nobody home.
 *
 * A year of silence in a repo this active means abandoned or blocked, and for
 * the one question this program answers -- what can I usefully pick up now --
 * that is worse than a thin issue somebody replied to yesterday. Demoted rather
 * than dropped: it stays visible below the live work instead of crowding it.
 *
 * Paid issues never reach this. Step 1 short-circuits them, and a bounty is
 * still a bounty if the thread has been quiet -- tinygrad's are years old.
 */
#define JUDGE_STALE_DAYS  365
#define JUDGE_STALE_CAP   5    /* below LLM_SCORE_MIN: off the board, not deleted */

/* Case-insensitive substring, ASCII. needle is a literal, never user input. */
static int ci_contains(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);

    if (hay == NULL)
        return 0;
    for (; *hay != '\0'; hay++) {
        if (ascii_strncasecmp(hay, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

/*
 * A currency symbol against an actual figure: "$500", "$ 2,000". The digit is
 * the whole point -- a bare '$' matches "$HOME expansion in the build script"
 * and "PS1 $ prompt", and this predicate now sets a score floor, so a false
 * positive puts a shell-quoting bug at the top of the board.
 */
static int has_amount(const char *s)
{
    size_t i;

    if (s == NULL)
        return 0;
    for (i = 0; s[i] != '\0'; i++) {
        size_t j = i + 1;

        if (s[i] != '$')
            continue;
        while (s[j] == ' ')
            j++;
        if (s[j] >= '0' && s[j] <= '9')
            return 1;
    }
    return 0;
}

/*
 * The same evidence JUDGE_SYSTEM_PROMPT tells the model to read: title and
 * labels only, never the body -- a body is full of people saying they would pay
 * for a fix.
 *
 * The word list is short on purpose. "reward" and "grant" are absent for the
 * reason config.h gives about the keyword table: in these repos they mean reward
 * functions and permission grants far more often than money. "usd" and "eur" are
 * absent because as bare substrings they fire on ordinary words -- "eur" is
 * inside "neural" and "heuristic", which is most of this corpus.
 *
 * "gsoc" was here and was removed, which is worth spelling out because it looks
 * like money and is not. OpenCV keeps a permanent GSoC IDEA LIST, so the label
 * marks "somebody could propose this one summer", not claimable cash: it needs
 * an accepted student inside a seasonal programme. With gsoc in this list, 16 of
 * the 19 rows in the top band of a real 200-row board were OpenCV idea entries
 * -- "Julia Bindings for OpenCV", "HEIF format support", one filed in 2023 --
 * outranking every genuine bounty. It stays a prefilter keyword, so such issues
 * still reach the judge; it just no longer forces the band reserved for money.
 */
/*
 * Days since the issue was last touched, or -1 when that cannot be determined:
 * an unparseable or absent updated_at must read as "unknown", never as "stale",
 * because the cap is a demotion and a parse bug would quietly empty the board.
 */
static long issue_idle_days(const issue_t *is, time_t now)
{
    time_t updated;

    if (is->updated_at == NULL || iso8601_parse(is->updated_at, &updated) != 0)
        return -1;
    if (updated > now)
        return 0;                          /* clock skew, not a stale issue */
    return (long)((now - updated) / (24 * 60 * 60));
}

static int payload_shows_payment(const issue_t *is)
{
    static const char *const WORD[] = { "bounty", "prize", "stipend" };
    size_t w;
    int i;

    if (has_amount(is->title))
        return 1;
    for (w = 0; w < sizeof WORD / sizeof WORD[0]; w++) {
        if (ci_contains(is->title, WORD[w]))
            return 1;
        for (i = 0; i < is->n_labels; i++) {
            if (ci_contains(is->labels[i], WORD[w]) || has_amount(is->labels[i]))
                return 1;
        }
    }
    return 0;
}

#define SCREEN_PROMPT_HEAD                                                        \
    "You are a cheap first-pass filter over GitHub issues.\n"                     \
    "Developer profile: "

#define SCREEN_PROMPT_TAIL                                                        \
    "\n"                                                                          \
    "For each listed index answer keep=true only if this developer might "        \
    "plausibly care. Be generous: a second, stronger model scores the "           \
    "survivors. Reject only obvious noise -- bots, dependency bumps, typos, "     \
    "docs-only changes, other platforms."

/*
 * Assembled once at judge_init() and never freed: they live for the process,
 * are read by every batch, and the cycle arena is reset under them. malloc is
 * allowed here because this is startup, not the hot path.
 */
static char *g_judge_prompt;
static char *g_screen_prompt;
static const char *g_profile = USER_PROFILE;

void judge_set_profile(const char *profile)
{
    if (profile != NULL && profile[0] != '\0')
        g_profile = profile;
}

static char *prompt_join(const char *head, const char *profile, const char *tail)
{
    size_t n = strlen(head) + strlen(profile) + strlen(tail) + 1;
    char *p = malloc(n);

    if (p == NULL)
        return NULL;
    snprintf(p, n, "%s%s%s", head, profile, tail);
    return p;
}

/*
 * The profile competes with the issues for OLLAMA_NUM_CTX, and an overrun is
 * silent: the model simply stops seeing the last issue in the batch. Warning at
 * startup is the only place a user can connect the cause to the effect.
 */
static void warn_if_profile_crowds_the_context(void)
{
    size_t sys_bytes = strlen(g_judge_prompt);
    size_t budget = (size_t)OLLAMA_NUM_CTX * 3;   /* ~3 bytes per token, English */

    if (sys_bytes * 3 > budget)
        LOGW("judge: the system prompt is %zu bytes, over a third of the ~%zu "
             "byte budget at num_ctx %d. A long profile pushes issues out of "
             "the window and the model stops seeing the end of each batch.",
             sys_bytes, budget, OLLAMA_NUM_CTX);
}

/* ------------------------------------------------------------------ UTF-8 */

static int is_utf8_cont(unsigned char c)
{
    return (c & 0xc0u) == 0x80u;
}

/*
 * Smallest offset >= start that begins a codepoint. The other direction lives
 * in core/util.c as utf8_trunc_len(); only the tail needs this one, because the
 * head goes through text_trunc_clean().
 */
static size_t utf8_ceil(const char *s, size_t len, size_t start)
{
    while (start < len && is_utf8_cont((unsigned char)s[start]))
        start++;
    return start;
}

/* --------------------------------------------------------- string builder */

typedef struct {
    char  *buf;
    size_t cap;      /* includes the NUL slot */
    size_t len;
    int    overflow;
} sbuf_t;

static void sb_puts(sbuf_t *sb, const char *s)
{
    size_t n;

    if (sb->overflow || s == NULL)
        return;
    n = strlen(s);
    if (n + 1 > sb->cap - sb->len) {
        sb->overflow = 1;
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(sbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    size_t room;
    int n;

    if (sb->overflow)
        return;
    room = sb->cap - sb->len;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        sb->buf[sb->len] = '\0';
        sb->overflow = 1;
        return;
    }
    sb->len += (size_t)n;
}

/* ------------------------------------------------------------ body / prompt */

const char *judge_truncate_body(arena_t *a, const char *body)
{
    const size_t mark_len = sizeof ELISION - 1;
    size_t len, head, keep_tail, tail_start, out_len;
    char *out;

    if (body == NULL)
        return "";
    len = strlen(body);
    if (len <= (size_t)LLM_BODY_TRUNC)
        return body;                       /* already short enough: no copy */
    if (a == NULL)
        return body;

    /* The head ends at an elision marker, so stopping it mid-sentence hands the
     * model a fragment to reason from. Back it up to the last full sentence. */
    head       = text_trunc_clean(body, len, (size_t)LLM_BODY_HEAD);
    keep_tail  = (size_t)LLM_BODY_TRUNC - (size_t)LLM_BODY_HEAD - mark_len;
    tail_start = utf8_ceil(body, len, len - keep_tail);

    out_len = head + mark_len + (len - tail_start);
    out = arena_alloc(a, out_len + 1, 1);
    if (out == NULL) {
        LOGE("judge: arena exhausted truncating a %zu byte body", len);
        return NULL;
    }
    memcpy(out, body, head);
    memcpy(out + head, ELISION, mark_len);
    memcpy(out + head + mark_len, body + tail_start, len - tail_start);
    out[out_len] = '\0';
    return out;
}

static const char **truncate_bodies(arena_t *a, const issue_t *issues, size_t n)
{
    const char **out;
    size_t i;

    out = arena_alloc(a, n * sizeof *out, 0);
    if (out == NULL) {
        LOGE("judge: arena exhausted allocating %zu truncated bodies", n);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        out[i] = judge_truncate_body(a, issues[i].body);
        if (out[i] == NULL)
            return NULL;
    }
    return out;
}

static const char *build_batch_prompt(arena_t *a, const issue_t *issues, size_t n,
                                      const char **bodies)
{
    sbuf_t sb;
    size_t cap = 512;
    size_t i;
    time_t now = time(NULL);
    int k;

    for (i = 0; i < n; i++) {
        cap += 192;                        /* framing and the numeric fields */
        cap += strlen(issues[i].repo != NULL ? issues[i].repo : "");
        cap += strlen(issues[i].title != NULL ? issues[i].title : "");
        cap += strlen(bodies[i]);
        for (k = 0; k < issues[i].n_labels && k < GH_MAX_LABELS; k++)
            cap += strlen(issues[i].labels[k] != NULL ? issues[i].labels[k] : "") + 2;
    }

    sb.buf = arena_alloc(a, cap, 1);
    if (sb.buf == NULL) {
        LOGE("judge: arena exhausted building a %zu byte prompt", cap);
        return NULL;
    }
    sb.cap = cap;
    sb.len = 0;
    sb.overflow = 0;
    sb.buf[0] = '\0';

    sb_printf(&sb, "%zu candidate issues follow. Indices are 0-based.\n", n);
    for (i = 0; i < n; i++) {
        sb_printf(&sb, "\n=== [%zu] %s#%d\ntitle: %s\n", i,
                  issues[i].repo != NULL ? issues[i].repo : "?",
                  issues[i].number,
                  issues[i].title != NULL ? issues[i].title : "(untitled)");
        if (issues[i].n_labels > 0) {
            sb_puts(&sb, "labels:");
            for (k = 0; k < issues[i].n_labels && k < GH_MAX_LABELS; k++)
                sb_printf(&sb, " %s;",
                          issues[i].labels[k] != NULL ? issues[i].labels[k] : "");
            sb_puts(&sb, "\n");
        }
        /* Step 1 of the rubric is a substring search, so it is answered here
         * rather than asked. Left to the model it is unreliable in exactly the
         * cases that matter: an 8B judge read "[Bounty] Outline of ..." as
         * unpaid and scored the real tinygrad bounty 8, while inventing a
         * bounty programme for three tt-metal issues that had none. */
        {
            long idle = issue_idle_days(&issues[i], now);
            char age[64];

            if (idle < 0)
                snprintf(age, sizeof age, "unknown");
            else
                snprintf(age, sizeof age, "%ld day%s ago%s", idle,
                         idle == 1 ? "" : "s",
                         idle > JUDGE_STALE_DAYS ? " -- ABANDONED" : "");
            sb_printf(&sb, "payment: %s\nlast activity: %s\ncomments: %d\nbody:\n%s\n",
                      payload_shows_payment(&issues[i])
                          ? "YES -- title or labels name money"
                          : "none in title or labels",
                      age, issues[i].comments, bodies[i]);
        }
    }

    if (sb.overflow) {
        LOGE("judge: prompt size estimate was short -- dropping the batch");
        return NULL;
    }
    return sb.buf;
}

/* ------------------------------------------------------------------ schema */

static yyjson_mut_val *sprop(yyjson_mut_doc *d, const char *type, const char *desc)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);

    if (o == NULL)
        return NULL;
    if (!yyjson_mut_obj_add_str(d, o, "type", type))
        return NULL;
    if (desc != NULL && !yyjson_mut_obj_add_str(d, o, "description", desc))
        return NULL;
    return o;
}

/* {"i":int,"keep":bool,"score":0..10,"why":string} */
static yyjson_mut_val *verdict_item_schema(yyjson_mut_doc *d)
{
    yyjson_mut_val *item  = yyjson_mut_obj(d);
    yyjson_mut_val *props = yyjson_mut_obj(d);
    yyjson_mut_val *req   = yyjson_mut_arr(d);
    yyjson_mut_val *pi, *pk, *ps, *pw;

    if (item == NULL || props == NULL || req == NULL)
        return NULL;

    pi = sprop(d, "integer", "0-based index of the issue in the list");
    pk = sprop(d, "boolean", "false to drop the issue entirely");
    ps = sprop(d, "integer", "relevance to the developer profile, 0-10");
    pw = sprop(d, "string", "at most 12 words justifying the score");
    if (pi == NULL || pk == NULL || ps == NULL || pw == NULL)
        return NULL;
    if (!yyjson_mut_obj_add_int(d, ps, "minimum", 0) ||
        !yyjson_mut_obj_add_int(d, ps, "maximum", 10))
        return NULL;

    if (!yyjson_mut_obj_add_val(d, props, "i", pi) ||
        !yyjson_mut_obj_add_val(d, props, "keep", pk) ||
        !yyjson_mut_obj_add_val(d, props, "score", ps) ||
        !yyjson_mut_obj_add_val(d, props, "why", pw))
        return NULL;

    if (!yyjson_mut_arr_add_str(d, req, "i") ||
        !yyjson_mut_arr_add_str(d, req, "keep") ||
        !yyjson_mut_arr_add_str(d, req, "score") ||
        !yyjson_mut_arr_add_str(d, req, "why"))
        return NULL;

    if (!yyjson_mut_obj_add_str(d, item, "type", "object") ||
        !yyjson_mut_obj_add_val(d, item, "properties", props) ||
        !yyjson_mut_obj_add_val(d, item, "required", req) ||
        !yyjson_mut_obj_add_bool(d, item, "additionalProperties", false))
        return NULL;

    return item;
}

#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID

/*
 * An array of exactly `n` items -- the length is the point, not decoration.
 *
 * Without the bound an empty array satisfies the schema, and that is the exit a
 * small model takes: qwen3:4b returned exactly 8 verdicts for a batch of 8 in
 * 4 of 9 trials, the other five coming back as three empty arrays, one array of
 * one, and one overrun past the last index. Every short answer is candidates
 * silently discarded -- lost to the sampler, not to LLM_SCORE_MIN. Pinning the
 * length made it 9 of 9, because the grammar stops the model closing the array
 * early rather than asking it not to.
 *
 * Bounding the index with minimum/maximum was tried first and is worse than
 * useless: llama.cpp's schema-to-GBNF conversion handles integer ranges badly
 * enough that every reply came back empty. Constrain the count, not the value.
 */
static yyjson_mut_val *array_of(yyjson_mut_doc *d, yyjson_mut_val *item, size_t n)
{
    yyjson_mut_val *arr = yyjson_mut_obj(d);

    if (arr == NULL || item == NULL)
        return NULL;
    if (!yyjson_mut_obj_add_str(d, arr, "type", "array") ||
        !yyjson_mut_obj_add_val(d, arr, "items", item) ||
        !yyjson_mut_obj_add_uint(d, arr, "minItems", (uint64_t)n) ||
        !yyjson_mut_obj_add_uint(d, arr, "maxItems", (uint64_t)n))
        return NULL;
    return arr;
}

/*
 * POST /api/chat with a JSON Schema in "format" (CLAUDE.md rule 6). The reply
 * body is left in `resp`; the caller digs the array out of message.content.
 */
static int ollama_post(arena_t *a, void *pool, const char *model,
                       const char *system, const char *user,
                       yyjson_mut_val *(*schema)(yyjson_mut_doc *, size_t),
                       size_t n_items, http_resp_t *resp)
{
    static const char *const hdrs[] = { "Content-Type: application/json", NULL };
    yyjson_alc alc;
    yyjson_mut_doc *doc;
    yyjson_mut_val *root, *msgs, *sys_m, *usr_m, *opts, *fmt;
    http_req_t req;
    char *body;
    size_t body_len;

    if (!yyjson_alc_pool_init(&alc, pool, JUDGE_POOL_BYTES))
        return -1;
    /*
     * No yyjson_mut_doc_free(): every byte of this doc, including the rendered
     * request, lives in the arena-backed pool above. It is re-initialised for
     * the next batch and released with the cycle arena.
     */
    doc = yyjson_mut_doc_new(&alc);
    if (doc == NULL)
        return -1;

    root   = yyjson_mut_obj(doc);
    msgs   = yyjson_mut_arr(doc);
    opts   = yyjson_mut_obj(doc);
    sys_m  = yyjson_mut_obj(doc);
    usr_m  = yyjson_mut_obj(doc);
    fmt    = schema(doc, n_items);
    if (root == NULL || msgs == NULL || opts == NULL || sys_m == NULL ||
        usr_m == NULL || fmt == NULL)
        return -1;

    if (!yyjson_mut_obj_add_str(doc, sys_m, "role", "system") ||
        !yyjson_mut_obj_add_str(doc, sys_m, "content", system) ||
        !yyjson_mut_obj_add_str(doc, usr_m, "role", "user") ||
        !yyjson_mut_obj_add_str(doc, usr_m, "content", user) ||
        !yyjson_mut_arr_add_val(msgs, sys_m) ||
        !yyjson_mut_arr_add_val(msgs, usr_m))
        return -1;

    /* temperature 0 so the same batch scores the same twice -- not a knob.
     * num_ctx is sent rather than inherited: it is a server-side default the
     * environment can change, and the judge degrades sharply when the prompt
     * approaches it. See OLLAMA_NUM_CTX. */
    /* num_gpu is only sent when pinned. Omitting it lets Ollama fit the layers
     * into whatever VRAM is actually free; see OLLAMA_NUM_GPU for the 500 that
     * sending 999 turned into. */
    if (OLLAMA_NUM_GPU >= 0 &&
        !yyjson_mut_obj_add_int(doc, opts, "num_gpu", OLLAMA_NUM_GPU))
        return -1;
    if (!yyjson_mut_obj_add_int(doc, opts, "num_ctx", OLLAMA_NUM_CTX) ||
        !yyjson_mut_obj_add_int(doc, opts, "temperature", 0))
        return -1;

    /* think:false -- qwen3 is a hybrid-reasoning model and its <think> block
     * would be pure cost here; the schema already forces the output shape. */
    if (!yyjson_mut_obj_add_str(doc, root, "model", model) ||
        !yyjson_mut_obj_add_bool(doc, root, "stream", false) ||
        !yyjson_mut_obj_add_bool(doc, root, "think", false) ||
        !yyjson_mut_obj_add_val(doc, root, "format", fmt) ||
        !yyjson_mut_obj_add_val(doc, root, "options", opts) ||
        !yyjson_mut_obj_add_val(doc, root, "messages", msgs))
        return -1;
    yyjson_mut_doc_set_root(doc, root);

    body = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, &alc, &body_len, NULL);
    if (body == NULL) {
        LOGE("judge: ollama request outgrew the %u byte JSON scratch pool",
             (unsigned)JUDGE_POOL_BYTES);
        return -1;
    }

    memset(&req, 0, sizeof req);
    req.url         = OLLAMA_URL;
    req.method      = "POST";
    req.headers     = hdrs;
    req.body        = body;
    req.body_len    = body_len;
    req.timeout_sec = LLM_TIMEOUT_SEC;

    memset(resp, 0, sizeof *resp);
    if (http_perform_one(a, &req, resp) < 0) {
        LOGW("judge: ollama request could not be issued");
        return -1;
    }
    if (resp->status != 200) {
        LOGW("judge: ollama replied %ld (%s)", resp->status,
             resp->err_msg != NULL ? resp->err_msg : "no detail");
        return -1;
    }
    return 0;
}

#endif /* LOCAL || HYBRID */

#if JUDGE_MODE == JUDGE_LOCAL

/* Ollama "format": exactly one full verdict per issue in the batch. */
static yyjson_mut_val *ollama_verdict_schema(yyjson_mut_doc *d, size_t n)
{
    return array_of(d, verdict_item_schema(d), n);
}

/*
 * Exposed (not in judge.h) so the schema test can drive the real builder, the
 * way gist.c exposes gist_build_body(). The array bound is the whole of the
 * short-batch fix and nothing downstream can detect its absence: a schema that
 * quietly allows an empty reply produces a thin board, never an error.
 */
const char *judge_render_format_schema(arena_t *a, size_t n)
{
    yyjson_alc alc;
    yyjson_mut_doc *doc;
    yyjson_mut_val *fmt;
    void *pool;

    if (a == NULL)
        return NULL;
    pool = arena_alloc(a, JUDGE_POOL_BYTES, 16);
    if (pool == NULL || !yyjson_alc_pool_init(&alc, pool, JUDGE_POOL_BYTES))
        return NULL;
    doc = yyjson_mut_doc_new(&alc);
    if (doc == NULL)
        return NULL;
    fmt = ollama_verdict_schema(doc, n);
    if (fmt == NULL)
        return NULL;
    yyjson_mut_doc_set_root(doc, fmt);
    return yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, &alc, NULL, NULL);
}

static int ollama_full_batch(arena_t *a, void *pool, issue_t *issues, size_t n,
                             const char **bodies)
{
    http_resp_t resp;
    const char *prompt, *verdicts;
    size_t vlen;
    int applied;

    prompt = build_batch_prompt(a, issues, n, bodies);
    if (prompt == NULL)
        return -1;
    if (ollama_post(a, pool, g_model, g_judge_prompt, prompt,
                    ollama_verdict_schema, n, &resp) < 0)
        return -1;
    if (ollama_window_gate(&resp, g_judge_prompt, prompt) < 0)
        return -1;
    if (judge_extract_ollama_verdicts(a, resp.body, resp.body_len, &verdicts, &vlen) < 0)
        return -1;

    applied = judge_parse_verdicts(a, verdicts, vlen, issues, n);
    if (applied < 0)
        LOGW("judge: ollama verdict array did not parse");
    return applied;
}

#endif /* LOCAL */

#if JUDGE_MODE == JUDGE_HYBRID

/* Screening schema: keep/drop only. The 1.7B is not asked to score. */
static yyjson_mut_val *screen_item_schema(yyjson_mut_doc *d)
{
    yyjson_mut_val *item  = yyjson_mut_obj(d);
    yyjson_mut_val *props = yyjson_mut_obj(d);
    yyjson_mut_val *req   = yyjson_mut_arr(d);
    yyjson_mut_val *pi, *pk;

    if (item == NULL || props == NULL || req == NULL)
        return NULL;
    pi = sprop(d, "integer", "0-based index of the issue in the list");
    pk = sprop(d, "boolean", "true if the developer might plausibly care");
    if (pi == NULL || pk == NULL)
        return NULL;
    if (!yyjson_mut_obj_add_val(d, props, "i", pi) ||
        !yyjson_mut_obj_add_val(d, props, "keep", pk) ||
        !yyjson_mut_arr_add_str(d, req, "i") ||
        !yyjson_mut_arr_add_str(d, req, "keep") ||
        !yyjson_mut_obj_add_str(d, item, "type", "object") ||
        !yyjson_mut_obj_add_val(d, item, "properties", props) ||
        !yyjson_mut_obj_add_val(d, item, "required", req) ||
        !yyjson_mut_obj_add_bool(d, item, "additionalProperties", false))
        return NULL;
    return item;
}

static yyjson_mut_val *screen_schema(yyjson_mut_doc *d, size_t n)
{
    return array_of(d, screen_item_schema(d), n);
}

/* Fills keep[0..n) from a screening reply. Returns the number of decisions. */
static int parse_screen(const char *json, size_t json_len, unsigned char *keep, size_t n)
{
    yyjson_doc *doc;
    yyjson_val *root, *v;
    yyjson_arr_iter it;
    int applied = 0;

    if (json == NULL)
        return -1;
    doc = yyjson_read(json, json_len, 0);
    if (doc == NULL)
        return -1;
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root) || !yyjson_arr_iter_init(root, &it)) {
        yyjson_doc_free(doc);
        return -1;
    }
    while ((v = yyjson_arr_iter_next(&it)) != NULL) {
        yyjson_val *vi = yyjson_obj_get(v, "i");
        yyjson_val *vk = yyjson_obj_get(v, "keep");
        long long idx;

        if (!yyjson_is_int(vi))
            continue;
        idx = (long long)yyjson_get_sint(vi);
        if (idx < 0 || (unsigned long long)idx >= (unsigned long long)n) {
            LOGW("judge: screen index %lld out of range for %zu issues", idx, n);
            continue;
        }
        keep[idx] = (unsigned char)(yyjson_is_bool(vk) ? yyjson_get_bool(vk) : 1);
        applied++;
    }
    yyjson_doc_free(doc);
    return applied;
}

static int ollama_screen(arena_t *a, void *pool, issue_t *issues, size_t n,
                         const char **bodies, unsigned char *keep)
{
    http_resp_t resp;
    const char *prompt, *verdicts;
    size_t vlen;

    prompt = build_batch_prompt(a, issues, n, bodies);
    if (prompt == NULL)
        return -1;
    if (ollama_post(a, pool, OLLAMA_SCREEN_MODEL, g_screen_prompt, prompt,
                    screen_schema, n, &resp) < 0)
        return -1;
    if (ollama_window_gate(&resp, g_screen_prompt, prompt) < 0)
        return -1;
    if (judge_extract_ollama_verdicts(a, resp.body, resp.body_len, &verdicts, &vlen) < 0)
        return -1;
    return parse_screen(verdicts, vlen, keep, n);
}

#endif /* HYBRID */

#if JUDGE_MODE == JUDGE_API || JUDGE_MODE == JUDGE_HYBRID

/*
 * Anthropic Messages API, shape taken from the claude-api skill (curl/raw HTTP
 * reference), not from memory:
 *
 *   POST https://api.anthropic.com/v1/messages
 *   content-type: application/json
 *   x-api-key: $ANTHROPIC_API_KEY
 *   anthropic-version: 2023-06-01
 *   {"model":..., "max_tokens":..., "system":"...",
 *    "tools":[{"name":"report_verdicts","description":"...","strict":true,
 *              "input_schema":{"type":"object",
 *                "properties":{"verdicts":{"type":"array","items":{...}}},
 *                "required":["verdicts"],"additionalProperties":false}}],
 *    "tool_choice":{"type":"tool","name":"report_verdicts"},
 *    "messages":[{"role":"user","content":"<numbered list>"}]}
 *
 * The reply carries a tool_use content block whose `input` is already
 * structured, so nothing is ever regexed out of prose. `strict:true` plus a
 * forced tool_choice is what constrains the output; Haiku 4.5 supports both.
 */
static yyjson_mut_val *anthropic_tool(yyjson_mut_doc *d)
{
    yyjson_mut_val *tool   = yyjson_mut_obj(d);
    yyjson_mut_val *schema = yyjson_mut_obj(d);
    yyjson_mut_val *props  = yyjson_mut_obj(d);
    yyjson_mut_val *req    = yyjson_mut_arr(d);
    yyjson_mut_val *arr    = yyjson_mut_obj(d);
    yyjson_mut_val *item   = verdict_item_schema(d);

    if (tool == NULL || schema == NULL || props == NULL || req == NULL ||
        arr == NULL || item == NULL)
        return NULL;

    if (!yyjson_mut_obj_add_str(d, arr, "type", "array") ||
        !yyjson_mut_obj_add_val(d, arr, "items", item) ||
        !yyjson_mut_obj_add_val(d, props, "verdicts", arr) ||
        !yyjson_mut_arr_add_str(d, req, "verdicts") ||
        !yyjson_mut_obj_add_str(d, schema, "type", "object") ||
        !yyjson_mut_obj_add_val(d, schema, "properties", props) ||
        !yyjson_mut_obj_add_val(d, schema, "required", req) ||
        !yyjson_mut_obj_add_bool(d, schema, "additionalProperties", false))
        return NULL;

    if (!yyjson_mut_obj_add_str(d, tool, "name", ANTHROPIC_TOOL_NAME) ||
        !yyjson_mut_obj_add_str(d, tool, "description",
                                "Report one triage verdict for every numbered "
                                "issue. Call this exactly once.") ||
        !yyjson_mut_obj_add_bool(d, tool, "strict", true) ||
        !yyjson_mut_obj_add_val(d, tool, "input_schema", schema))
        return NULL;
    return tool;
}

static int anthropic_batch(arena_t *a, void *pool, issue_t *issues, size_t n,
                           const char **bodies)
{
    yyjson_alc alc;
    yyjson_mut_doc *doc;
    yyjson_mut_val *root, *tools, *tool, *choice, *msgs, *usr_m;
    const char *prompt, *verdicts, *key, *key_hdr;
    http_req_t req;
    http_resp_t resp;
    char *body;
    size_t body_len, vlen;
    int applied;

    key = env_or_null("ANTHROPIC_API_KEY");
    if (key == NULL) {
        LOGE("judge: ANTHROPIC_API_KEY disappeared from the environment");
        return -1;
    }

    prompt = build_batch_prompt(a, issues, n, bodies);
    if (prompt == NULL)
        return -1;

    /* The key only ever reaches the arena and libcurl -- never a log line. */
    key_hdr = arena_printf(a, "x-api-key: %s", key);
    if (key_hdr == NULL) {
        LOGE("judge: arena exhausted building the Anthropic auth header");
        return -1;
    }

    if (!yyjson_alc_pool_init(&alc, pool, JUDGE_POOL_BYTES))
        return -1;
    doc = yyjson_mut_doc_new(&alc);          /* pool-backed: never freed here */
    if (doc == NULL)
        return -1;

    root   = yyjson_mut_obj(doc);
    tools  = yyjson_mut_arr(doc);
    msgs   = yyjson_mut_arr(doc);
    usr_m  = yyjson_mut_obj(doc);
    choice = yyjson_mut_obj(doc);
    tool   = anthropic_tool(doc);
    if (root == NULL || tools == NULL || msgs == NULL || usr_m == NULL ||
        choice == NULL || tool == NULL)
        return -1;

    if (!yyjson_mut_arr_add_val(tools, tool) ||
        !yyjson_mut_obj_add_str(doc, choice, "type", "tool") ||
        !yyjson_mut_obj_add_str(doc, choice, "name", ANTHROPIC_TOOL_NAME) ||
        !yyjson_mut_obj_add_str(doc, usr_m, "role", "user") ||
        !yyjson_mut_obj_add_str(doc, usr_m, "content", prompt) ||
        !yyjson_mut_arr_add_val(msgs, usr_m))
        return -1;

    if (!yyjson_mut_obj_add_str(doc, root, "model", ANTHROPIC_MODEL) ||
        !yyjson_mut_obj_add_int(doc, root, "max_tokens", ANTHROPIC_MAX_TOKENS) ||
        !yyjson_mut_obj_add_str(doc, root, "system", g_judge_prompt) ||
        !yyjson_mut_obj_add_val(doc, root, "tools", tools) ||
        !yyjson_mut_obj_add_val(doc, root, "tool_choice", choice) ||
        !yyjson_mut_obj_add_val(doc, root, "messages", msgs))
        return -1;
    yyjson_mut_doc_set_root(doc, root);

    body = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, &alc, &body_len, NULL);
    if (body == NULL) {
        LOGE("judge: Anthropic request outgrew the %u byte JSON scratch pool",
             (unsigned)JUDGE_POOL_BYTES);
        return -1;
    }

    {
        const char *const hdrs[] = {
            "content-type: application/json",
            key_hdr,
            "anthropic-version: " ANTHROPIC_VERSION,
            NULL
        };

        memset(&req, 0, sizeof req);
        req.url         = ANTHROPIC_URL;
        req.method      = "POST";
        req.headers     = hdrs;
        req.body        = body;
        req.body_len    = body_len;
        req.timeout_sec = LLM_TIMEOUT_SEC;

        memset(&resp, 0, sizeof resp);
        if (http_perform_one(a, &req, &resp) < 0) {
            LOGW("judge: Anthropic request could not be issued");
            return -1;
        }
    }

    if (resp.status != 200) {
        LOGW("judge: Anthropic replied %ld (%s)", resp.status,
             resp.err_msg != NULL ? resp.err_msg : "no detail");
        /* Fall through: the error envelope names the cause, which is worth a log. */
    }
    if (judge_extract_anthropic_verdicts(a, resp.body, resp.body_len, &verdicts, &vlen) < 0)
        return -1;

    applied = judge_parse_verdicts(a, verdicts, vlen, issues, n);
    if (applied < 0)
        LOGW("judge: Anthropic verdict array did not parse");
    return applied;
}

#endif /* API || HYBRID */

#if JUDGE_MODE == JUDGE_HYBRID

/*
 * Local 1.7B keeps/drops, the API scores the survivors. The survivors are
 * copied into a compact array because judge_parse_verdicts() applies verdicts by
 * index -- the model must see 0..m-1, not the holes the screen punched.
 */
static int hybrid_batch(arena_t *a, void *pool, issue_t *issues, size_t n,
                        const char **bodies)
{
    unsigned char *keep;
    issue_t *surv;
    const char **surv_bodies;
    size_t *map;
    size_t i, j, m = 0;
    int applied;

    keep = arena_calloc(a, n, 1);
    if (keep == NULL) {
        LOGE("judge: arena exhausted allocating the screen result");
        return -1;
    }
    if (ollama_screen(a, pool, issues, n, bodies, keep) < 0) {
        /*
         * The screen is the gate in this mode, so a broken gate means an
         * unjudged batch -- not a silent fallback to paid tokens. The issues
         * keep llm_score = -1 and reappear next cycle, because the watermark
         * only advances after a fully successful cycle (CLAUDE.md rule 4).
         */
        LOGW("judge: local screen failed -- dropping this batch of %zu", n);
        return -1;
    }

    for (i = 0; i < n; i++)
        if (keep[i])
            m++;
    LOGD("judge: screen kept %zu of %zu", m, n);
    if (m == 0)
        return 0;                          /* a legitimate all-noise batch */

    surv        = arena_alloc(a, m * sizeof *surv, 0);
    surv_bodies = arena_alloc(a, m * sizeof *surv_bodies, 0);
    map         = arena_alloc(a, m * sizeof *map, 0);
    if (surv == NULL || surv_bodies == NULL || map == NULL) {
        LOGE("judge: arena exhausted compacting %zu screen survivors", m);
        return -1;
    }
    for (i = 0, j = 0; i < n; i++) {
        if (!keep[i])
            continue;
        surv[j]        = issues[i];
        surv_bodies[j] = bodies[i];
        map[j]         = i;
        j++;
    }

    applied = anthropic_batch(a, pool, surv, m, surv_bodies);
    if (applied < 0)
        return -1;
    for (j = 0; j < m; j++) {
        issues[map[j]].llm_score = surv[j].llm_score;
        issues[map[j]].why       = surv[j].why;
    }
    return applied;
}

#endif /* HYBRID */

/* ------------------------------------------------------- envelope extraction */

/*
 * Classifies one Ollama reply against OLLAMA_NUM_CTX. `prompt_bytes` is the
 * system prompt plus the user prompt as sent; `used_tokens`, when non-NULL,
 * receives prompt_eval_count + eval_count.
 *
 * Truncation is detected by ratio, not count, because the count is taken after
 * the cut -- see OLLAMA_TRUNC_BYTES_PER_TOKEN for the measurements. Missing or
 * zero counts are UNKNOWN, never TRUNCATED: an Ollama that stops reporting them
 * must not make every batch look cut and silently empty the board.
 */
int judge_ollama_window(const char *json, size_t json_len, size_t prompt_bytes,
                        long *used_tokens)
{
    yyjson_doc *doc;
    yyjson_val *root, *pv, *ev;
    long prompt_tok = 0, out_tok = 0;
    int rc = JUDGE_WINDOW_UNKNOWN;

    if (used_tokens != NULL)
        *used_tokens = 0;
    if (json == NULL)
        return JUDGE_WINDOW_UNKNOWN;

    doc = yyjson_read(json, json_len, 0);
    if (doc == NULL)
        return JUDGE_WINDOW_UNKNOWN;
    root = yyjson_doc_get_root(doc);
    pv = yyjson_obj_get(root, "prompt_eval_count");
    ev = yyjson_obj_get(root, "eval_count");
    if (yyjson_is_int(pv))
        prompt_tok = (long)yyjson_get_sint(pv);
    if (yyjson_is_int(ev))
        out_tok = (long)yyjson_get_sint(ev);
    yyjson_doc_free(doc);

    if (prompt_tok <= 0)
        return JUDGE_WINDOW_UNKNOWN;
    if (out_tok < 0)
        out_tok = 0;
    if (used_tokens != NULL)
        *used_tokens = prompt_tok + out_tok;

    if (prompt_bytes > (size_t)prompt_tok * (size_t)OLLAMA_TRUNC_BYTES_PER_TOKEN)
        rc = JUDGE_WINDOW_TRUNCATED;
    else if ((prompt_tok + out_tok) * 100 > (long)OLLAMA_NUM_CTX * OLLAMA_CTX_WARN_PCT)
        rc = JUDGE_WINDOW_NEAR;
    else
        rc = JUDGE_WINDOW_OK;
    return rc;
}

#if JUDGE_MODE == JUDGE_LOCAL || JUDGE_MODE == JUDGE_HYBRID
/*
 * Runs judge_ollama_window() on a reply and folds the result into the cycle's
 * tallies. Returns -1 when the batch must be dropped.
 */
static int ollama_window_gate(const http_resp_t *resp, const char *system,
                              const char *prompt)
{
    size_t bytes = strlen(system) + strlen(prompt);
    long used;
    int w = judge_ollama_window(resp->body, resp->body_len, bytes, &used);

    if (used > g_win_peak)
        g_win_peak = used;
    if (w == JUDGE_WINDOW_TRUNCATED) {
        g_win_truncated++;
        LOGD("judge: %zu prompt bytes came back as only %ld tokens -- truncated",
             bytes, used);
        return -1;
    }
    if (w == JUDGE_WINDOW_NEAR)
        g_win_near++;
    return 0;
}
#endif

int judge_extract_ollama_verdicts(arena_t *a, const char *json, size_t json_len,
                                  const char **out, size_t *out_len)
{
    yyjson_doc *doc;
    yyjson_val *root, *err, *msg, *content;
    char *copy;
    size_t slen;
    int rc = -1;

    if (a == NULL || out == NULL || out_len == NULL)
        return -1;
    *out = NULL;
    *out_len = 0;
    if (json == NULL)
        return -1;

    doc = yyjson_read(json, json_len, 0);
    if (doc == NULL) {
        LOGW("judge: ollama reply is not JSON");
        return -1;
    }
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        LOGW("judge: ollama reply root is not an object");
        goto out;
    }

    err = yyjson_obj_get(root, "error");
    if (err != NULL) {
        LOGW("judge: ollama error: %s",
             yyjson_is_str(err) ? yyjson_get_str(err) : "(non-string)");
        goto out;
    }

    msg = yyjson_obj_get(root, "message");
    content = yyjson_obj_get(msg, "content");
    if (!yyjson_is_str(content)) {
        LOGW("judge: ollama reply has no message.content string");
        goto out;
    }

    /*
     * Double encoding, and it is not a bug: /api/chat always returns the
     * completion as a *string*, so a "format"-constrained array arrives as the
     * text of an array and needs a second parse pass.
     */
    slen = yyjson_get_len(content);
    copy = arena_strndup(a, yyjson_get_str(content), slen);
    if (copy == NULL) {
        LOGE("judge: arena exhausted copying a %zu byte verdict array", slen);
        goto out;
    }
    *out = copy;
    *out_len = slen;
    rc = 0;
out:
    yyjson_doc_free(doc);
    return rc;
}

int judge_extract_anthropic_verdicts(arena_t *a, const char *json, size_t json_len,
                                     const char **out, size_t *out_len)
{
    yyjson_doc *doc;
    yyjson_val *root, *err, *content, *blk, *verdicts = NULL;
    yyjson_arr_iter it;
    yyjson_alc alc;
    void *pool;
    char *txt;
    size_t txt_len;
    int rc = -1;

    if (a == NULL || out == NULL || out_len == NULL)
        return -1;
    *out = NULL;
    *out_len = 0;
    if (json == NULL)
        return -1;

    doc = yyjson_read(json, json_len, 0);
    if (doc == NULL) {
        LOGW("judge: Anthropic reply is not JSON");
        return -1;
    }
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        LOGW("judge: Anthropic reply root is not an object");
        goto out;
    }

    err = yyjson_obj_get(root, "error");
    if (err != NULL) {
        yyjson_val *ty = yyjson_obj_get(err, "type");
        yyjson_val *ms = yyjson_obj_get(err, "message");

        LOGW("judge: Anthropic error %s: %s",
             yyjson_is_str(ty) ? yyjson_get_str(ty) : "(unknown)",
             yyjson_is_str(ms) ? yyjson_get_str(ms) : "(no message)");
        goto out;
    }

    content = yyjson_obj_get(root, "content");
    if (!yyjson_is_arr(content) || !yyjson_arr_iter_init(content, &it)) {
        LOGW("judge: Anthropic reply has no content array");
        goto out;
    }
    while ((blk = yyjson_arr_iter_next(&it)) != NULL) {
        yyjson_val *ty = yyjson_obj_get(blk, "type");
        yyjson_val *nm, *in, *v;

        if (!yyjson_is_str(ty) || strcmp(yyjson_get_str(ty), "tool_use") != 0)
            continue;
        nm = yyjson_obj_get(blk, "name");
        if (!yyjson_is_str(nm) || strcmp(yyjson_get_str(nm), ANTHROPIC_TOOL_NAME) != 0)
            continue;
        in = yyjson_obj_get(blk, "input");
        v = yyjson_obj_get(in, "verdicts");
        if (yyjson_is_arr(v)) {
            verdicts = v;
            break;
        }
    }
    if (verdicts == NULL) {
        LOGW("judge: no %s tool_use block in the Anthropic reply", ANTHROPIC_TOOL_NAME);
        goto out;
    }

    /*
     * tool_use `input` is already structured, so this re-serialise is only here
     * because judge_parse_verdicts() takes JSON text. Rendered into an
     * arena-backed pool so there is nothing to free.
     */
    pool = arena_alloc(a, JUDGE_EXTRACT_POOL, 16);
    if (pool == NULL || !yyjson_alc_pool_init(&alc, pool, JUDGE_EXTRACT_POOL)) {
        LOGE("judge: arena exhausted extracting the Anthropic verdicts");
        goto out;
    }
    txt = yyjson_val_write_opts(verdicts, YYJSON_WRITE_NOFLAG, &alc, &txt_len, NULL);
    if (txt == NULL) {
        LOGW("judge: verdict array outgrew the %u byte extraction pool",
             (unsigned)JUDGE_EXTRACT_POOL);
        goto out;
    }
    *out = txt;
    *out_len = txt_len;
    rc = 0;
out:
    yyjson_doc_free(doc);
    return rc;
}

/* ---------------------------------------------------------------- verdicts */

int judge_parse_verdicts(arena_t *a, const char *json, size_t json_len,
                         issue_t *issues, size_t n)
{
    yyjson_doc *doc;
    yyjson_val *root, *v;
    yyjson_arr_iter it;
    /*
     * Separate from `applied`, which only counts verdicts that landed. Position
     * must advance for every array element, including one skipped as malformed
     * -- otherwise a single bad verdict shifts every issue after it by one,
     * which is the exact misalignment this change exists to remove.
     */
    size_t pos = 0;
    int applied = 0;
    int stale = 0;
    time_t now = time(NULL);
    int rc;

    if (a == NULL || json == NULL || (n > 0 && issues == NULL))
        return -1;

    doc = yyjson_read(json, json_len, 0);
    if (doc == NULL)
        return -1;
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root) || !yyjson_arr_iter_init(root, &it)) {
        yyjson_doc_free(doc);
        return -1;
    }

    rc = 0;
    while ((v = yyjson_arr_iter_next(&it)) != NULL) {
        yyjson_val *vi = yyjson_obj_get(v, "i");
        yyjson_val *vk = yyjson_obj_get(v, "keep");
        yyjson_val *vs = yyjson_obj_get(v, "score");
        yyjson_val *vw = yyjson_obj_get(v, "why");
        long long idx, score;
        int keep;
        char *why = NULL;

        /*
         * Position in the array is the index, not the model's own "i".
         *
         * "i" is generated text and it is wrong often enough to matter: a real
         * board published `why` strings describing the PREVIOUS issue in the
         * batch, three rows running, alongside the scores that belonged with
         * them -- so the board recommended issues on the strength of a
         * different issue's reasoning. The `index N out of range for a batch of
         * N` warning is the same fault where it happens to be detectable.
         *
         * Position is structural instead: ollama_verdict_schema() pins the
         * array to exactly n items via minItems/maxItems, so the k-th verdict
         * is the verdict for the k-th issue by construction. A model that
         * answers out of order would defeat this, but every observed reply is
         * in order, and trusting "i" mis-assigns outright.
         */
        idx = (long long)pos++;
        if ((unsigned long long)idx >= (unsigned long long)n) {
            LOGW("judge: more verdicts than the %zu issues asked about -- "
                 "ignoring the tail", n);
            break;
        }
        /*
         * Kept as a disagreement signal only. It costs nothing and it is how
         * this fault became visible in the first place; if it starts firing on
         * every batch, the model has stopped answering in order and position is
         * no longer safe either.
         */
        if (yyjson_is_int(vi)) {
            long long claimed = (long long)yyjson_get_sint(vi);

            if (claimed != idx)
                LOGW("judge: verdict %lld claims index %lld -- using position",
                     idx, claimed);
        }

        keep = yyjson_is_bool(vk) ? (yyjson_get_bool(vk) ? 1 : 0) : 1;
        score = yyjson_is_int(vs) ? (long long)yyjson_get_sint(vs) : 0;
        if (score < 0)
            score = 0;
        if (score > 10)
            score = 10;
        if (!keep)
            score = 0;                     /* below LLM_SCORE_MIN: dropped */

        /*
         * Both directions are enforced, because the model gets both wrong and
         * the two errors are not symmetric in cost. An invented bounty puts a
         * lie at the top of the board; a missed one is a paid, unassigned issue
         * the user never hears about, which is the whole reason this runs.
         *
         * The floor overrides keep=false deliberately. A model that answered
         * keep=false with "payment: YES" as its reason has contradicted the
         * payload, not made a judgement, and that is a verdict observed live.
         */
        if (payload_shows_payment(&issues[idx])) {
            if (score < JUDGE_PAID_FLOOR) {
                LOGW("judge: %s#%d is paid per its title or labels but scored "
                     "%lld -- raised to %d (model said: %s)", issues[idx].repo,
                     issues[idx].number, score, JUDGE_PAID_FLOOR,
                     yyjson_is_str(vw) ? yyjson_get_str(vw) : "(no reason)");
                score = JUDGE_PAID_FLOOR;
            }
        } else {
            long idle;

            if (score > JUDGE_UNPAID_CAP) {
                LOGW("judge: %s#%d scored %lld with no payment in title or "
                     "labels -- capped at %d (model said: %s)", issues[idx].repo,
                     issues[idx].number, score, JUDGE_UNPAID_CAP,
                     yyjson_is_str(vw) ? yyjson_get_str(vw) : "(no reason)");
                score = JUDGE_UNPAID_CAP;
            }
            /* After the unpaid cap, so the log reports the score the issue
             * would actually have held had it not gone quiet. */
            idle = issue_idle_days(&issues[idx], now);
            if (idle > JUDGE_STALE_DAYS && score > JUDGE_STALE_CAP) {
                LOGD("judge: %s#%d scored %lld but has been idle %ld days -- "
                     "capped at %d", issues[idx].repo, issues[idx].number,
                     score, idle, JUDGE_STALE_CAP);
                stale++;
                score = JUDGE_STALE_CAP;
            }
        }

        if (yyjson_is_str(vw)) {
            size_t wlen = yyjson_get_len(vw);

            /* The prompt asks for 12 words and the model overshoots often:
             * 40 of 60 rows on a real board sat at exactly LLM_WHY_MAX, every
             * one of them cut mid-word. This is the notification body, so it
             * stops at the last full sentence or word instead. */
            wlen = text_trunc_clean(yyjson_get_str(vw), wlen, (size_t)LLM_WHY_MAX);
            /* Arena copy: the doc dies at the bottom of this function, so a
             * borrowed pointer would be a dangling read from notify.c. */
            why = arena_strndup(a, yyjson_get_str(vw), wlen);
            if (why == NULL) {
                LOGE("judge: arena exhausted copying a verdict reason");
                rc = -1;
                break;
            }
        }

        issues[idx].llm_score = (int)score;
        issues[idx].why = why != NULL ? why : "";
        applied++;
    }

    yyjson_doc_free(doc);
    /* Per-issue detail is LOGD; the count is not, because a batch where most
     * issues were demoted is the signal that the backfill has reached a repo's
     * dead backlog, and that is worth seeing without -v. */
    if (stale > 0)
        LOGI("judge: %d of %d demoted to %d, idle over %d days",
             stale, applied, JUDGE_STALE_CAP, JUDGE_STALE_DAYS);
    return rc < 0 ? rc : applied;
}

/*
 * Score descending, then id ascending. qsort is not stable, so the tiebreak is
 * what keeps the NOTIFY_MAX_PER_CYCLE cut deterministic across runs.
 */
static int verdict_cmp(const void *pa, const void *pb)
{
    const issue_t *x = pa;
    const issue_t *y = pb;

    if (x->llm_score != y->llm_score)
        return x->llm_score < y->llm_score ? 1 : -1;
    if (x->id != y->id)
        return x->id < y->id ? -1 : 1;
    return 0;
}

void judge_log_scores(const issue_t *issues, size_t n)
{
    int hist[12];                       /* 0..10, plus [11] for "no verdict" */
    char line[160];
    size_t i, off = 0, kept = 0;
    int s;

    if (issues == NULL || n == 0)
        return;

    memset(hist, 0, sizeof hist);
    for (i = 0; i < n; i++) {
        s = issues[i].llm_score;
        if (s < 0)
            hist[11]++;
        else
            hist[s > 10 ? 10 : s]++;
        if (s >= LLM_SCORE_MIN)
            kept++;
    }

    for (s = 0; s <= 10 && off < sizeof line - 12; s++) {
        int w;

        if (hist[s] == 0)
            continue;
        w = snprintf(line + off, sizeof line - off, "%s%d:%d",
                     off > 0 ? " " : "", s, hist[s]);
        if (w < 0 || (size_t)w >= sizeof line - off)
            break;
        off += (size_t)w;
    }
    if (off == 0)
        snprintf(line, sizeof line, "(none)");

    /*
     * The histogram, not just the count. "judge kept 0/74" cannot tell you
     * whether the model scored everything a 5 -- one short of LLM_SCORE_MIN,
     * so the threshold is wrong -- or a 0, so the candidates genuinely were
     * noise. Those call for opposite responses and the bare count hides which.
     */
    LOGI("judge scores: %s  (>=%d kept: %zu of %zu)", line, LLM_SCORE_MIN, kept, n);
    if (hist[11] > 0)
        LOGW("judge: %d issue(s) came back with no verdict at all", hist[11]);
}

size_t judge_apply(issue_t *issues, size_t n)
{
    size_t i, k = 0;

    if (issues == NULL)
        return 0;
    for (i = 0; i < n; i++)
        if (issues[i].llm_score >= LLM_SCORE_MIN)
            issues[k++] = issues[i];
    if (k > 1)
        qsort(issues, k, sizeof issues[0], verdict_cmp);
    return k;
}

/* ------------------------------------------------------------------ driver */

int judge_init(void)
{
    /* Before any backend check: a build that cannot assemble its own prompt has
     * nothing to send, whichever backend it would have sent it to. */
    g_judge_prompt = prompt_join(JUDGE_PROMPT_HEAD, g_profile, JUDGE_PROMPT_TAIL);
    g_screen_prompt = prompt_join(SCREEN_PROMPT_HEAD, g_profile, SCREEN_PROMPT_TAIL);
    if (g_judge_prompt == NULL || g_screen_prompt == NULL) {
        LOGE("judge: out of memory assembling the system prompt");
        return -1;
    }
    warn_if_profile_crowds_the_context();

#if JUDGE_MODE == JUDGE_LOCAL
    /*
     * No reachability probe: Ollama unloads the model between cycles by design
     * (OLLAMA_KEEP_ALIVE), so a probe here would either wake a cold model for
     * nothing or fail against a server that would have been fine 20 s later.
     */
    LOGI("judge: local backend, %s via %s", g_model, OLLAMA_URL);
    return 0;
#else
    if (env_or_null("ANTHROPIC_API_KEY") == NULL) {
        LOGE("judge: ANTHROPIC_API_KEY is unset -- this build needs it "
             "(JUDGE_MODE is JUDGE_API or JUDGE_HYBRID)");
        return -1;
    }
#if JUDGE_MODE == JUDGE_HYBRID
    LOGI("judge: hybrid backend, %s screens for %s", OLLAMA_SCREEN_MODEL, ANTHROPIC_MODEL);
#else
    LOGI("judge: API backend, %s", ANTHROPIC_MODEL);
#endif
    return 0;
#endif
}

int judge_batch(arena_t *a, issue_t *issues, size_t n)
{
    const char **bodies;
    void *pool;
    size_t off;
    int ok = 0, dropped = 0;

    if (a == NULL || (n > 0 && issues == NULL))
        return -1;
    if (n == 0)
        return 0;

    pool = arena_alloc(a, JUDGE_POOL_BYTES, 16);
    if (pool == NULL) {
        LOGE("judge: no room in the cycle arena for the %u byte JSON scratch pool",
             (unsigned)JUDGE_POOL_BYTES);
        return -1;
    }
    bodies = truncate_bodies(a, issues, n);
    if (bodies == NULL)
        return -1;

    g_win_truncated = 0;
    g_win_near = 0;
    g_win_peak = 0;

    /* One request per LLM_BATCH_SIZE issues: 8x fewer round trips, and the
     * model gets to compare the candidates against each other. */
    for (off = 0; off < n; off += (size_t)LLM_BATCH_SIZE) {
        size_t m = n - off < (size_t)LLM_BATCH_SIZE ? n - off : (size_t)LLM_BATCH_SIZE;
        int r;

#if JUDGE_MODE == JUDGE_LOCAL
        r = ollama_full_batch(a, pool, issues + off, m, bodies + off);
#elif JUDGE_MODE == JUDGE_API
        r = anthropic_batch(a, pool, issues + off, m, bodies + off);
#elif JUDGE_MODE == JUDGE_HYBRID
        r = hybrid_batch(a, pool, issues + off, m, bodies + off);
#else
#error "JUDGE_MODE must be JUDGE_LOCAL, JUDGE_API or JUDGE_HYBRID"
#endif
        if (r < 0) {
            /* Dropped, logged, not retried (CLAUDE.md rule 6). These issues
             * keep llm_score = -1 and judge_apply() discards them. */
            dropped++;
            LOGW("judge: dropped the batch at offset %zu (%zu issues)", off, m);
        } else {
            ok++;
            LOGD("judge: batch at offset %zu produced %d verdicts", off, r);
        }
    }

    LOGI("judge: %zu issues, %d batches judged, %d dropped", n, ok, dropped);

    /*
     * The cause is almost always the same: a longer `profile` in the user's
     * config, or LLM_BATCH_SIZE raised without OLLAMA_NUM_CTX. Both are named,
     * because the one who hits this edited one of them and should not have to
     * read judge.c to find out which knobs are involved.
     */
    if (g_win_truncated > 0)
        LOGE("judge: %d batch(es) DROPPED -- the prompt overflowed OLLAMA_NUM_CTX "
             "(%d) and Ollama silently cut it, which makes the model score issues "
             "it cannot see. Shorten `profile` in your config, or lower "
             "LLM_BATCH_SIZE / raise OLLAMA_NUM_CTX in src/config.h",
             g_win_truncated, OLLAMA_NUM_CTX);
    else if (g_win_near > 0)
        LOGW("judge: %d batch(es) used over %d%% of OLLAMA_NUM_CTX (peak %ld of %d "
             "tokens). A slightly longer profile or body will start truncating "
             "prompts, and the verdicts go wrong without an error",
             g_win_near, OLLAMA_CTX_WARN_PCT, g_win_peak, OLLAMA_NUM_CTX);
    else if (g_win_peak > 0)
        LOGD("judge: context peak %ld of %d tokens", g_win_peak, OLLAMA_NUM_CTX);
    return ok > 0 ? 0 : -1;
}
