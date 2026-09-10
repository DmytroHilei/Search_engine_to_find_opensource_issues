#include "pipeline/render.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "core/util.h"

/*
 * Everything here is presentation. The one non-obvious constraint is that the
 * strings coming in are untrusted: a GitHub title is whatever the reporter
 * typed, and the `why` is whatever the model emitted. Both land inside a GFM
 * table cell, where a bare '|' silently eats the rest of the row and a bare
 * newline ends the table outright -- so every field goes through an escaper,
 * and every escaper is bounded by the fixed array it reads from rather than
 * trusting a NUL to arrive.
 */

/*
 * A score bucket, not a user tunable, so it stays out of config.h. 8+ is the
 * "drop what you are doing" band the plan cares about; LLM_SCORE_MIN is the
 * threshold to be pushed at all, and anything under it can only be on the board
 * as a leftover, so it gets the quiet dot.
 */
#define RENDER_HOT_SCORE 8

/*
 * Worst-case growth per input byte: '&' -> "&amp;" is five, every other escape
 * is shorter. Used only to size the buffer, and deliberately pessimistic --
 * a short estimate costs the whole publish (see the overflow check below).
 */
#define ESC_MAX     5
#define URL_ESC_MAX 3   /* one byte -> "%XX" */

/* --------------------------------------------------------- string builder */

/* Same shape as judge.c's builder: bounded, sticky-overflow, never partial. */
typedef struct {
    char  *buf;
    size_t cap;      /* includes the NUL slot */
    size_t len;
    int    overflow;
} sbuf_t;

static void sb_putc(sbuf_t *sb, char c)
{
    if (sb->overflow)
        return;
    if (sb->len + 2 > sb->cap) {
        sb->overflow = 1;
        return;
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

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

/* ---------------------------------------------------------------- escaping */

/*
 * Markdown-safe text for inside a table cell. `max` bounds the read because the
 * caller hands us a fixed char array, not a promise of a NUL.
 *
 * Multi-byte UTF-8 is copied through untouched: every byte of a continuation
 * sequence has the high bit set, so it can never collide with an ASCII
 * metacharacter, and emoji and CJK render fine in a gist body (CLAUDE.md rule 5
 * is about ntfy *headers*).
 */
static void sb_put_md_text(sbuf_t *sb, const char *s, size_t max)
{
    size_t i;

    for (i = 0; i < max && s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        /* A newline does not break one cell, it terminates the whole table. */
        case '\n':
        case '\r':
        case '\t':
            sb_putc(sb, ' ');
            break;
        /* The cell separator. GFM honours a backslash escape here. */
        case '|':
            sb_puts(sb, "\\|");
            break;
        /* We emit a literal <br> ourselves, so raw HTML in a title must not
         * survive as markup -- a title containing <img> would otherwise be
         * rendered by the gist. */
        case '&':
            sb_puts(sb, "&amp;");
            break;
        case '<':
            sb_puts(sb, "&lt;");
            break;
        case '>':
            sb_puts(sb, "&gt;");
            break;
        /* An unbalanced backtick opens a code span that swallows the rest of
         * the row; brackets break out of the link text; * and _ turn the row
         * into accidental emphasis; a trailing backslash would escape our own
         * delimiter. */
        case '`':
        case '[':
        case ']':
        case '*':
        case '_':
        case '\\':
            sb_putc(sb, '\\');
            sb_putc(sb, (char)c);
            break;
        default:
            if (c < 0x20u || c == 0x7fu)
                sb_putc(sb, ' ');   /* other controls are invisible noise */
            else
                sb_putc(sb, (char)c);
            break;
        }
    }
}

/*
 * A link destination. Only the characters that would end the destination early
 * are percent-encoded; '%' is deliberately passed through so an already-encoded
 * GitHub URL is not double-encoded into a dead link.
 */
static void sb_put_md_url(sbuf_t *sb, const char *s, size_t max)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;

    for (i = 0; i < max && s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c <= 0x20u || c == 0x7fu || c == '(' || c == ')' || c == '<' ||
            c == '>' || c == '"' || c == '`' || c == '\\' || c == '|') {
            sb_putc(sb, '%');
            sb_putc(sb, HEX[(c >> 4) & 0xfu]);
            sb_putc(sb, HEX[c & 0xfu]);
        } else {
            sb_putc(sb, (char)c);
        }
    }
}

/* ------------------------------------------------------------------- cells */

static const char *score_dot(int score)
{
    if (score >= RENDER_HOT_SCORE)
        return "🟢";
    if (score >= LLM_SCORE_MIN)
        return "🟡";
    return "⚪";
}

/*
 * "tenstorrent/tt-metal" -> "tt-metal". The board is only ever opened on a
 * phone, where the owner half is width nobody reads.
 */
static const char *repo_short(const char *repo, size_t max, size_t *len_out)
{
    size_t len = strnlen(repo, max);
    size_t start = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        if (repo[i] == '/')
            start = i + 1;
    }
    *len_out = len - start;
    return repo + start;
}

int render_age(char *dst, size_t dstlen, const char *first_seen, time_t now)
{
    long long delta;
    time_t then;
    int n;

    if (dst == NULL || dstlen == 0)
        return -1;
    dst[0] = '\0';

    /*
     * A bad stamp still has to produce a cell. Refusing to render the whole
     * board because one row carries a malformed date would trade a cosmetic
     * defect for a missed bounty, so the caller ignores this return and prints
     * whatever landed in `dst`.
     */
    if (first_seen == NULL || first_seen[0] == '\0' ||
        iso8601_parse(first_seen, &then) != 0) {
        if (dstlen < 2)
            return -1;
        dst[0] = '?';
        dst[1] = '\0';
        return -1;
    }

    delta = (long long)now - (long long)then;
    /* Negative means clock skew between us and GitHub, not a future issue. */
    if (delta < 60)
        n = snprintf(dst, dstlen, "just now");
    else if (delta < 3600)
        n = snprintf(dst, dstlen, "%lldm", delta / 60);
    else if (delta < 86400)
        n = snprintf(dst, dstlen, "%lldh", delta / 3600);
    else
        n = snprintf(dst, dstlen, "%lldd", delta / 86400);

    if (n < 0 || (size_t)n >= dstlen) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------- board */

static size_t estimate_cap(const board_t *b, size_t n)
{
    size_t cap = 512;               /* header, table framing, empty-board line */
    size_t i;

    for (i = 0; i < n; i++) {
        const board_entry_t *e = &b->entries[i];

        cap += 128;                 /* row framing, index, score, dot, age */
        cap += ESC_MAX * strnlen(e->repo, sizeof e->repo);
        cap += ESC_MAX * strnlen(e->title, sizeof e->title);
        cap += ESC_MAX * strnlen(e->why, sizeof e->why);
        cap += URL_ESC_MAX * strnlen(e->html_url, sizeof e->html_url);
    }
    return cap;
}

static void render_row(sbuf_t *sb, const board_entry_t *e, size_t rank, time_t now)
{
    const char *repo;
    size_t repo_len;
    char age[32];

    (void)render_age(age, sizeof age, e->first_seen, now);
    repo = repo_short(e->repo, sizeof e->repo, &repo_len);

    sb_printf(sb, "| %zu | %d %s | %s | ", rank, e->llm_score,
              score_dot(e->llm_score), age);
    sb_put_md_text(sb, repo, repo_len);
    sb_puts(sb, " | ");

    /*
     * An entry with no URL still belongs on the board -- it is a real open
     * bounty, just one we cannot deep-link -- so it degrades to plain text
     * rather than to "[title]()", which renders as a dead link.
     */
    if (e->html_url[0] != '\0') {
        sb_putc(sb, '[');
        if (e->title[0] != '\0')
            sb_put_md_text(sb, e->title, sizeof e->title);
        else
            sb_puts(sb, "(untitled)");
        sb_puts(sb, "](");
        sb_put_md_url(sb, e->html_url, sizeof e->html_url);
        sb_putc(sb, ')');
    } else if (e->title[0] != '\0') {
        sb_put_md_text(sb, e->title, sizeof e->title);
    } else {
        sb_puts(sb, "(untitled)");
    }

    /*
     * <br> is the only way to get a second line into a GFM table cell, and the
     * judge's one-clause reason is the entire product of the LLM leg -- without
     * it the board is a list of titles the user must open to triage.
     */
    if (e->why[0] != '\0') {
        sb_puts(sb, "<br>*");
        sb_put_md_text(sb, e->why, sizeof e->why);
        sb_putc(sb, '*');
    }
    sb_puts(sb, " |\n");
}

const char *render_board(arena_t *a, const board_t *b, time_t now)
{
    struct tm tm;
    char stamp[64];
    sbuf_t sb;
    size_t cap, n, i;

    if (a == NULL || b == NULL)
        return NULL;

    n = (b->entries == NULL) ? 0 : b->n;
    if (n > b->cap)
        n = b->cap;                 /* a nonsense count must not read past the slots */

    if (gmtime_r(&now, &tm) == NULL)
        return NULL;
    if (strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M UTC", &tm) == 0)
        return NULL;

    cap = estimate_cap(b, n);
    sb.buf = arena_alloc(a, cap, 1);
    if (sb.buf == NULL) {
        LOGE("render: arena exhausted building a %zu byte board", cap);
        return NULL;
    }
    sb.cap = cap;
    sb.len = 0;
    sb.overflow = 0;
    sb.buf[0] = '\0';

    sb_printf(&sb, "# issuewatch — %zu open · updated %s\n", n, stamp);

    if (n == 0) {
        /*
         * Still a document, never an empty file: the user who opens the gist
         * and sees a blank page cannot tell "nothing is open" from "the daemon
         * is broken".
         */
        sb_puts(&sb, "\nNothing open right now.\n");
    } else {
        sb_puts(&sb, "\n| # | score | age | repo | issue |\n");
        sb_puts(&sb, "|---|-------|-----|------|-------|\n");
        for (i = 0; i < n; i++)
            render_row(&sb, &b->entries[i], i + 1, now);
    }

    /*
     * A short estimate is a bug, but the failure mode is what matters: half a
     * board reads as "these are all the open bounties" and the user stops
     * looking. Drop the publish instead (render.h documents the contract).
     */
    if (sb.overflow) {
        LOGE("render: board size estimate of %zu bytes was short -- no publish", cap);
        return NULL;
    }
    return sb.buf;
}
