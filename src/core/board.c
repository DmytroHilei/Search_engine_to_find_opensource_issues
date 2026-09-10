#include "core/board.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "core/fileio.h"
#include "core/state.h"
#include "core/util.h"

/*
 * A flat array of at most BOARD_MAX entries plus a TSV file beside the etag
 * cache. Two things here are load-bearing and easy to get wrong:
 *
 *  - Escaping. GitHub titles carry tabs, newlines, backslashes, emoji and CJK.
 *    A raw tab shifts every following field by one and a raw newline splits one
 *    entry into two half-entries, so title/why/html_url are escaped on write
 *    and unescaped on read. Multi-byte UTF-8 passes through byte for byte; only
 *    truncation is UTF-8 aware, because half a codepoint renders as a
 *    replacement box in the published board.
 *
 *  - Load posture. Same as load_etags(), for the same reason: a bad line costs
 *    one entry, a fatal parse costs the whole daemon. Every failure below is a
 *    skipped line and a LOGW.
 */

/* HTTP_ETAG_MAX and LLM_WHY_MAX are the sizes the rest of the program will hand
 * us; a board field narrower than its source would silently clip an ETag (which
 * turns every re-check into a full GET) or a reason. core/ does not include
 * net/, so HTTP_ETAG_MAX arrives transitively through core/state.h. */
_Static_assert(BOARD_WHY_MAX > LLM_WHY_MAX,
               "board why field must hold LLM_WHY_MAX bytes plus its NUL");
_Static_assert(BOARD_ETAG_MAX >= HTTP_ETAG_MAX,
               "board etag field must hold a full HTTP ETag");

#define BOARD_NAME     "board.tsv"
#define BOARD_TMP_NAME "board.tsv.tmp"

#define BOARD_FIELDS 12

/*
 * Worst case for one line: every text field full and every byte of it escaped
 * to two, plus the fixed fields, the separators and the newline. A longer line
 * cannot have come from emit_entry() and is garbage by definition.
 */
#define BOARD_LINE_MAX 2048

/* ------------------------------------------------------------------- text */

/*
 * Length of the longest prefix of `s` (at most `len` bytes) that does not end
 * mid-sequence. Truncating a title is fine; truncating it three bytes into a
 * CJK character is not -- the gist renders the remainder as a replacement box.
 * A string that is not valid UTF-8 is left exactly as long as it was: this
 * trims a known-good encoding, it does not validate one.
 */
static size_t utf8_trim(const char *s, size_t len)
{
    size_t start = len, seen = 0, need;
    unsigned char c;

    if (s == NULL || len == 0)
        return 0;

    /* A lead byte is followed by at most three continuation bytes. */
    while (start > 0 && seen < 3 && ((unsigned char)s[start - 1] & 0xc0) == 0x80) {
        start--;
        seen++;
    }
    if (start == 0)
        return len;

    c = (unsigned char)s[start - 1];
    if (c < 0x80)
        return len;
    else if ((c & 0xe0) == 0xc0)
        need = 2;
    else if ((c & 0xf0) == 0xe0)
        need = 3;
    else if ((c & 0xf8) == 0xf0)
        need = 4;
    else
        return len;

    return (len - start + 1 == need) ? len : start - 1;
}

/*
 * `srcmax` bounds the read as well as the write: sources are fixed arrays
 * inside a caller-owned board_entry_t, and one that was filled without a
 * terminator must not send strlen() off the end of it.
 */
static void copy_text(char *dst, size_t dstlen, const char *src, size_t srcmax)
{
    size_t len;

    if (dst == NULL || dstlen == 0)
        return;

    len = (src != NULL) ? strnlen(src, srcmax) : 0;
    if (len >= dstlen)
        len = utf8_trim(src, dstlen - 1);
    if (len > 0)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

static int emit_escaped(FILE *f, const char *s, size_t srcmax)
{
    size_t i, len;

    len = (s != NULL) ? strnlen(s, srcmax) : 0;
    for (i = 0; i < len; i++) {
        int r;

        switch (s[i]) {
        case '\\': r = fputs("\\\\", f); break;
        case '\t': r = fputs("\\t", f);  break;
        case '\n': r = fputs("\\n", f);  break;
        /* Not strictly required to keep the format parseable, but a bare CR in
         * a text file is a trap for every tool that ever reads it. */
        case '\r': r = fputs("\\r", f);  break;
        default:   r = fputc((unsigned char)s[i], f); break;
        }
        if (r == EOF)
            return -EIO;
    }
    return 0;
}

/*
 * Reverses emit_escaped(). An unknown escape means the line was not written by
 * this program, so the line is rejected rather than guessed at. Over-long input
 * truncates on a UTF-8 boundary: losing the tail of a title beats losing the
 * entry it belongs to.
 */
static int unescape_into(char *dst, size_t dstlen, const char *src)
{
    size_t len = 0;
    int truncated = 0;

    if (dst == NULL || dstlen == 0 || src == NULL)
        return -EINVAL;

    for (; *src != '\0'; src++) {
        char c = *src;

        if (c == '\\') {
            switch (*++src) {
            case '\\': c = '\\'; break;
            case 't':  c = '\t'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            default:   return -EILSEQ;   /* also catches a trailing backslash */
            }
        }

        if (len + 1 < dstlen)
            dst[len++] = c;
        else
            truncated = 1;
    }

    if (truncated)
        len = utf8_trim(dst, len);
    dst[len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ order */

/*
 * Rank order, best first: llm_score desc, then first_seen desc so a fresh 8
 * outranks a stale 8, then id asc. qsort is not stable, so without the id leg
 * two equal rows would swap places every cycle and the board would read as
 * churn -- same reasoning as verdict_cmp() in judge.c.
 *
 * first_seen is fixed-width RFC3339 UTC, so a byte compare is a time compare
 * and cannot fail the way a re-parse could.
 */
static int board_cmp(const void *pa, const void *pb)
{
    const board_entry_t *x = pa;
    const board_entry_t *y = pb;
    int c;

    if (x->llm_score != y->llm_score)
        return x->llm_score < y->llm_score ? 1 : -1;
    c = strcmp(x->first_seen, y->first_seen);
    if (c != 0)
        return c < 0 ? 1 : -1;
    if (x->id != y->id)
        return x->id < y->id ? -1 : 1;
    return 0;
}

/* The slot board_rank() would put last, which is exactly the one to evict. */
static size_t board_worst(const board_t *b)
{
    size_t i, worst = 0;

    for (i = 1; i < b->n; i++) {
        if (board_cmp(&b->entries[i], &b->entries[worst]) > 0)
            worst = i;
    }
    return worst;
}

static board_entry_t *board_find(board_t *b, long long id)
{
    size_t i;

    for (i = 0; i < b->n; i++) {
        if (b->entries[i].id == id)
            return &b->entries[i];
    }
    return NULL;
}

/*
 * Everything a caller owns. first_seen and fresh are deliberately absent: the
 * board owns those, and preserving first_seen across an upsert is what keeps an
 * ageing entry from ranking as if it had just arrived.
 */
static void entry_assign(board_entry_t *dst, const board_entry_t *src)
{
    dst->id = src->id;
    dst->number = src->number;
    dst->llm_score = src->llm_score;
    dst->kw_score = src->kw_score;
    dst->assigned = src->assigned ? 1 : 0;

    copy_text(dst->repo, sizeof dst->repo, src->repo, sizeof src->repo);
    copy_text(dst->updated_at, sizeof dst->updated_at, src->updated_at,
              sizeof src->updated_at);
    copy_text(dst->etag, sizeof dst->etag, src->etag, sizeof src->etag);
    copy_text(dst->title, sizeof dst->title, src->title, sizeof src->title);
    copy_text(dst->why, sizeof dst->why, src->why, sizeof src->why);
    copy_text(dst->html_url, sizeof dst->html_url, src->html_url,
              sizeof src->html_url);
}

/* ------------------------------------------------------------------- load */

static int parse_ll(const char *s, long long *out)
{
    char *end;
    long long v;

    if (s == NULL || *s == '\0')
        return -EINVAL;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0')
        return -EINVAL;

    *out = v;
    return 0;
}

static int parse_int(const char *s, int *out)
{
    long long v;

    if (parse_ll(s, &v) != 0 || v < INT_MIN || v > INT_MAX)
        return -EINVAL;

    *out = (int)v;
    return 0;
}

/* Splits on tabs in place. Fails unless the line has exactly `want` fields --
 * one field too many means an unescaped tab reached the file. */
static int split_tabs(char *line, char **out, size_t want)
{
    size_t k = 0;
    char *p = line;

    for (;;) {
        char *tab = strchr(p, '\t');

        if (k >= want)
            return -EINVAL;
        out[k++] = p;
        if (tab == NULL)
            break;
        *tab = '\0';
        p = tab + 1;
    }
    return (k == want) ? 0 : -EINVAL;
}

static int parse_line(char *line, board_entry_t *e)
{
    char *fields[BOARD_FIELDS];
    long long id;
    time_t ignored;
    int rc;

    rc = split_tabs(line, fields, BOARD_FIELDS);
    if (rc != 0)
        return rc;

    if (parse_ll(fields[0], &id) != 0 || id <= 0)
        return -EINVAL;
    if (fields[1][0] == '\0' || strlen(fields[1]) >= STATE_REPO_MAX)
        return -EINVAL;
    if (strlen(fields[7]) >= BOARD_ETAG_MAX)
        return -EINVAL;

    memset(e, 0, sizeof *e);
    e->id = id;

    if (parse_int(fields[2], &e->number) != 0 ||
        parse_int(fields[3], &e->llm_score) != 0 ||
        parse_int(fields[4], &e->kw_score) != 0 ||
        parse_int(fields[11], &e->assigned) != 0)
        return -EINVAL;
    e->assigned = e->assigned ? 1 : 0;

    /* first_seen must parse or board_expire() can never age this row out, which
     * is precisely the row the safety net exists for. updated_at may be empty:
     * an entry that has never been re-checked has nothing to put there. */
    if (strlen(fields[5]) >= BOARD_TIME_MAX || iso8601_parse(fields[5], &ignored) != 0)
        return -EINVAL;
    if (strlen(fields[6]) >= BOARD_TIME_MAX)
        return -EINVAL;
    if (fields[6][0] != '\0' && iso8601_parse(fields[6], &ignored) != 0)
        return -EINVAL;

    memcpy(e->repo, fields[1], strlen(fields[1]) + 1);
    memcpy(e->first_seen, fields[5], strlen(fields[5]) + 1);
    memcpy(e->updated_at, fields[6], strlen(fields[6]) + 1);
    memcpy(e->etag, fields[7], strlen(fields[7]) + 1);

    if (unescape_into(e->title, sizeof e->title, fields[8]) != 0 ||
        unescape_into(e->why, sizeof e->why, fields[9]) != 0 ||
        unescape_into(e->html_url, sizeof e->html_url, fields[10]) != 0)
        return -EILSEQ;

    return 0;
}

static void board_load(board_t *b)
{
    char path[STATE_PATH_MAX];
    char line[BOARD_LINE_MAX];
    FILE *f;
    unsigned long lineno = 0, dropped = 0;

    if (fileio_path_join(path, sizeof path, b->dir, BOARD_NAME) != 0)
        return;

    f = fopen(path, "r");
    if (f == NULL) {
        /* A board that has never been published is an empty board. */
        if (errno != ENOENT)
            LOGW("board: cannot read %s: %s", path, strerror(errno));
        return;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        board_entry_t e;
        size_t len;

        lineno++;
        len = strlen(line);

        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        } else if (!feof(f)) {
            int c;

            /* Drain the rest of the physical line, or its tail would be read as
             * the next entry and one bad line would cost two. */
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
            LOGW("board: line %lu too long, skipped", lineno);
            continue;
        }

        if (len == 0)
            continue;

        if (parse_line(line, &e) != 0) {
            LOGW("board: line %lu malformed, skipped", lineno);
            continue;
        }
        /* A duplicated id would leave board_drop() removing only one of them. */
        if (board_find(b, e.id) != NULL) {
            LOGW("board: line %lu duplicates id %lld, skipped", lineno, e.id);
            continue;
        }
        if (b->n >= b->cap) {
            dropped++;
            continue;
        }

        b->entries[b->n++] = e;
    }

    if (dropped > 0)
        LOGW("board: %s holds more than %d entries, %lu dropped", path, BOARD_MAX,
             dropped);
    if (ferror(f))
        LOGW("board: error reading %s", path);
    fclose(f);
}

/* ------------------------------------------------------------------- api */

int board_open(board_t *b, const char *dir)
{
    if (b == NULL || dir == NULL || dir[0] == '\0')
        return -EINVAL;
    if (strlen(dir) >= STATE_PATH_MAX)
        return -ENAMETOOLONG;

    memset(b, 0, sizeof *b);
    memcpy(b->dir, dir, strlen(dir) + 1);

    /* One calloc for the life of the process, exactly like state_t::repos:
     * entries outlive the cycle arena, and nothing here ever reallocates. */
    b->entries = calloc(BOARD_MAX, sizeof *b->entries);
    if (b->entries == NULL) {
        memset(b, 0, sizeof *b);
        return -ENOMEM;
    }
    b->cap = BOARD_MAX;

    board_load(b);
    b->dirty = 0;   /* what was just read is what is already on disk */
    return 0;
}

void board_close(board_t *b)
{
    if (b == NULL)
        return;

    free(b->entries);
    memset(b, 0, sizeof *b);
}

static int emit_entry(FILE *f, const board_entry_t *e)
{
    if (fprintf(f, "%lld\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t", e->id, e->repo, e->number,
                e->llm_score, e->kw_score, e->first_seen, e->updated_at, e->etag) < 0)
        return -EIO;

    if (emit_escaped(f, e->title, sizeof e->title) != 0)
        return -EIO;
    if (fputc('\t', f) == EOF)
        return -EIO;
    if (emit_escaped(f, e->why, sizeof e->why) != 0)
        return -EIO;
    if (fputc('\t', f) == EOF)
        return -EIO;
    if (emit_escaped(f, e->html_url, sizeof e->html_url) != 0)
        return -EIO;

    if (fprintf(f, "\t%d\n", e->assigned ? 1 : 0) < 0)
        return -EIO;
    return 0;
}

static int emit_board(FILE *f, void *user)
{
    const board_t *b = user;
    size_t i;

    for (i = 0; i < b->n; i++) {
        int rc = emit_entry(f, &b->entries[i]);

        if (rc != 0)
            return rc;
    }
    return 0;
}

int board_flush(board_t *b)
{
    int rc;

    if (b == NULL || b->entries == NULL || b->dir[0] == '\0')
        return -EINVAL;
    if (!b->dirty)
        return 0;

    rc = fileio_atomic_write(b->dir, BOARD_NAME, BOARD_TMP_NAME, emit_board, b);
    if (rc != 0)
        return rc;

    b->dirty = 0;
    return 0;
}

int board_merge(board_t *b, const state_t *st, const board_entry_t *in, size_t n,
                const char *now_iso, size_t *n_new)
{
    size_t i, added = 0;
    time_t ignored;

    if (n_new != NULL)
        *n_new = 0;
    if (b == NULL || b->entries == NULL || st == NULL || now_iso == NULL)
        return -EINVAL;
    if (in == NULL && n > 0)
        return -EINVAL;
    /* Rejected here rather than stored: an unparseable first_seen is a row that
     * board_expire() can never age out and board_rank() cannot order. */
    if (strlen(now_iso) >= BOARD_TIME_MAX || iso8601_parse(now_iso, &ignored) != 0)
        return -EINVAL;

    for (i = 0; i < n; i++) {
        const board_entry_t *src = &in[i];
        board_entry_t cand, *dst;
        size_t worst;

        if (src->id <= 0)
            continue;
        /* Already notified means already dealt with. Without this an entry
         * dropped as closed or assigned would flap straight back onto the board
         * the next time it appeared in a delta. */
        if (state_seen(st, state_key(src->id, src->updated_at)))
            continue;

        dst = board_find(b, src->id);
        if (dst != NULL) {
            entry_assign(dst, src);
            dst->fresh = 1;
            b->dirty = 1;
            continue;
        }

        memset(&cand, 0, sizeof cand);
        entry_assign(&cand, src);
        memcpy(cand.first_seen, now_iso, strlen(now_iso) + 1);
        cand.fresh = 1;

        if (b->n < b->cap) {
            b->entries[b->n++] = cand;
        } else {
            worst = board_worst(b);
            /* A newcomer that is itself the worst row must not displace a
             * better one -- otherwise a cycle of 2s would empty the board. */
            if (board_cmp(&cand, &b->entries[worst]) >= 0)
                continue;
            b->entries[worst] = cand;
        }

        added++;
        b->dirty = 1;
    }

    if (n_new != NULL)
        *n_new = added;
    return 0;
}

int board_drop(board_t *b, long long id)
{
    size_t i;

    if (b == NULL || b->entries == NULL)
        return 0;

    for (i = 0; i < b->n; i++) {
        if (b->entries[i].id != id)
            continue;

        /* Order is preserved so a drop cannot reshuffle the ranked board. */
        memmove(&b->entries[i], &b->entries[i + 1],
                (b->n - i - 1) * sizeof *b->entries);
        b->n--;
        b->dirty = 1;
        return 1;
    }
    return 0;
}

void board_rank(board_t *b)
{
    if (b == NULL || b->entries == NULL || b->n < 2)
        return;

    /* Not a dirty-ing operation: the order is recomputed before every publish,
     * so persisting it would buy a rewrite of board.tsv every cycle for nothing. */
    qsort(b->entries, b->n, sizeof *b->entries, board_cmp);
}

size_t board_expire(board_t *b, time_t now)
{
    time_t cutoff;
    size_t i, keep = 0, dropped = 0;

    if (b == NULL || b->entries == NULL)
        return 0;

    cutoff = now - (time_t)BOARD_STALE_DAYS * 24 * 3600;

    for (i = 0; i < b->n; i++) {
        time_t seen;

        /* A row whose first_seen will not parse can never be aged out by date,
         * which is the one thing this net is here to prevent. */
        if (iso8601_parse(b->entries[i].first_seen, &seen) == 0 && seen > cutoff) {
            if (keep != i)
                b->entries[keep] = b->entries[i];
            keep++;
        } else {
            dropped++;
        }
    }

    if (dropped > 0) {
        b->n = keep;
        b->dirty = 1;
    }
    return dropped;
}

void board_clear_fresh(board_t *b)
{
    size_t i;

    if (b == NULL || b->entries == NULL)
        return;

    /* fresh is never persisted, so this leaves the board clean. */
    for (i = 0; i < b->n; i++)
        b->entries[i].fresh = 0;
}
