#include "judge.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

#include "config.h"
#include "http.h"
#include "util.h"

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

static const char ELISION[] = "\n[...]\n";

_Static_assert(LLM_BODY_TRUNC > LLM_BODY_HEAD + (int)sizeof ELISION,
               "LLM_BODY_TRUNC must leave room for LLM_BODY_HEAD plus the elision marker");
_Static_assert(LLM_BATCH_SIZE > 0, "LLM_BATCH_SIZE must be positive");
_Static_assert(LLM_WHY_MAX > 0, "LLM_WHY_MAX must be positive");

#define JUDGE_SYSTEM_PROMPT                                                       \
    "You triage GitHub issues for one developer. Answer only through the "        \
    "structured verdict list.\n"                                                  \
    "Developer profile: " USER_PROFILE "\n"                                       \
    "Score 0-10 for how much this developer wants this issue on their phone now:\n"\
    "  0-2  noise: bots, dependency bumps, typos, docs, other platforms\n"        \
    "  3-5  on-topic project, but not this developer's kind of work\n"            \
    "  6-7  relevant, worth opening\n"                                            \
    "  8-10 strong match: concrete, reproducible, actionable right now\n"          \
    "Set keep=false for anything you would not push to them at all.\n"            \
    "`why` is one clause of at most 12 words saying what makes it (ir)relevant -- "\
    "it is the notification body, so no markdown and no preamble.\n"              \
    "Return exactly one verdict per listed index, using the 0-based index given."

#define SCREEN_SYSTEM_PROMPT                                                      \
    "You are a cheap first-pass filter over GitHub issues.\n"                     \
    "Developer profile: " USER_PROFILE "\n"                                       \
    "For each listed index answer keep=true only if this developer might "        \
    "plausibly care. Be generous: a second, stronger model scores the "           \
    "survivors. Reject only obvious noise -- bots, dependency bumps, typos, "     \
    "docs-only changes, other platforms."

/* ------------------------------------------------------------------ UTF-8 */

static int is_utf8_cont(unsigned char c)
{
    return (c & 0xc0u) == 0x80u;
}

/*
 * Largest cut <= max that does not land inside a multi-byte sequence. A lone
 * continuation byte is invalid UTF-8 and yyjson refuses to encode it, which
 * would take down the whole request rather than mangle one body.
 */
static size_t utf8_floor(const char *s, size_t max)
{
    while (max > 0 && is_utf8_cont((unsigned char)s[max]))
        max--;
    return max;
}

/* Smallest offset >= start that begins a codepoint. */
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

    head       = utf8_floor(body, (size_t)LLM_BODY_HEAD);
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
        sb_printf(&sb, "comments: %d\nbody:\n%s\n", issues[i].comments, bodies[i]);
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

static yyjson_mut_val *array_of(yyjson_mut_doc *d, yyjson_mut_val *item)
{
    yyjson_mut_val *arr = yyjson_mut_obj(d);

    if (arr == NULL || item == NULL)
        return NULL;
    if (!yyjson_mut_obj_add_str(d, arr, "type", "array") ||
        !yyjson_mut_obj_add_val(d, arr, "items", item))
        return NULL;
    return arr;
}

/*
 * POST /api/chat with a JSON Schema in "format" (CLAUDE.md rule 6). The reply
 * body is left in `resp`; the caller digs the array out of message.content.
 */
static int ollama_post(arena_t *a, void *pool, const char *model,
                       const char *system, const char *user,
                       yyjson_mut_val *(*schema)(yyjson_mut_doc *),
                       http_resp_t *resp)
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
    fmt    = schema(doc);
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

    /* temperature 0 so the same batch scores the same twice -- not a knob. */
    if (!yyjson_mut_obj_add_int(doc, opts, "num_gpu", OLLAMA_NUM_GPU) ||
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

/* Ollama "format": a bare array of full verdicts. */
static yyjson_mut_val *ollama_verdict_schema(yyjson_mut_doc *d)
{
    return array_of(d, verdict_item_schema(d));
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
    if (ollama_post(a, pool, OLLAMA_MODEL, JUDGE_SYSTEM_PROMPT, prompt,
                    ollama_verdict_schema, &resp) < 0)
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

static yyjson_mut_val *screen_schema(yyjson_mut_doc *d)
{
    return array_of(d, screen_item_schema(d));
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
    if (ollama_post(a, pool, OLLAMA_SCREEN_MODEL, SCREEN_SYSTEM_PROMPT, prompt,
                    screen_schema, &resp) < 0)
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
        !yyjson_mut_obj_add_str(doc, root, "system", JUDGE_SYSTEM_PROMPT) ||
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
    int applied = 0;
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

        if (!yyjson_is_int(vi)) {
            LOGW("judge: verdict without an integer \"i\" -- skipped");
            continue;
        }
        idx = (long long)yyjson_get_sint(vi);
        /*
         * The bounds check that matters: a model asked about 8 issues will
         * occasionally answer about issue 99, and writing there corrupts
         * whatever follows the batch in the arena.
         */
        if (idx < 0 || (unsigned long long)idx >= (unsigned long long)n) {
            LOGW("judge: verdict index %lld out of range for a batch of %zu", idx, n);
            continue;
        }

        keep = yyjson_is_bool(vk) ? (yyjson_get_bool(vk) ? 1 : 0) : 1;
        score = yyjson_is_int(vs) ? (long long)yyjson_get_sint(vs) : 0;
        if (score < 0)
            score = 0;
        if (score > 10)
            score = 10;
        if (!keep)
            score = 0;                     /* below LLM_SCORE_MIN: dropped */

        if (yyjson_is_str(vw)) {
            size_t wlen = yyjson_get_len(vw);

            if (wlen > (size_t)LLM_WHY_MAX)
                wlen = utf8_floor(yyjson_get_str(vw), (size_t)LLM_WHY_MAX);
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
#if JUDGE_MODE == JUDGE_LOCAL
    /*
     * No reachability probe: Ollama unloads the model between cycles by design
     * (OLLAMA_KEEP_ALIVE), so a probe here would either wake a cold model for
     * nothing or fail against a server that would have been fine 20 s later.
     */
    LOGI("judge: local backend, %s via %s", OLLAMA_MODEL, OLLAMA_URL);
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
    return ok > 0 ? 0 : -1;
}
