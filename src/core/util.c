#include "core/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static log_level_t g_level = LOG_INFO;

static const char *const LVL_NAME[] = { "ERR", "WRN", "INF", "DBG" };

void log_set_level(log_level_t lvl)
{
    g_level = lvl;
}

log_level_t log_get_level(void)
{
    return g_level;
}

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    char ts[32];
    va_list ap;

    if (lvl > g_level)
        return;

    if (iso8601_format(time(NULL), ts, sizeof ts) != 0)
        ts[0] = '\0';

    fprintf(stderr, "%s %s ", ts, LVL_NAME[lvl]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int iso8601_format(time_t t, char *buf, size_t buflen)
{
    struct tm tm;

    if (buflen < 21 || gmtime_r(&t, &tm) == NULL)
        return -1;
    if (strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        return -1;
    return 0;
}

int iso8601_parse(const char *s, time_t *out)
{
    struct tm tm;
    int y, mo, d, h, mi, sec;

    if (s == NULL || out == NULL)
        return -1;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return -1;

    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = sec;

    /* timegm is the point: GitHub timestamps are UTC, the local zone is noise. */
    *out = timegm(&tm);
    return *out == (time_t)-1 ? -1 : 0;
}

uint64_t hash64(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ull;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int ascii_strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + 32);
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == '\0')
            return 0; //makes sense if we skip iteration - but why do we jump out of function if ca = '\0'?
    }
    return 0;
}

size_t ascii_sanitize(char *dst, size_t dstlen, const char *src)
{
    size_t w = 0;
    int pending_space = 0;

    if (dstlen == 0)
        return 0;
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    for (; *src != '\0' && w + 1 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;

        if (c >= 0x20 && c <= 0x7e) {
            if (pending_space) {
                dst[w++] = ' ';
                pending_space = 0;
                if (w + 1 >= dstlen)
                    break;
            }
            dst[w++] = (char)c;
        } else if (w > 0) {
            /* Collapse any run of non-ASCII (a multi-byte emoji, a newline)
             * into one space, and never emit a leading one. */
            pending_space = 1;
        }
    }

    dst[w] = '\0';
    return w;
}

size_t utf8_trunc_len(const char *s, size_t len, size_t max)
{
    size_t cut, back, seq;
    unsigned char lead;

    if (s == NULL || len <= max)
        return s == NULL ? 0 : len;

    /*
     * s[max] is the first byte being dropped. If it is a continuation byte
     * (10xxxxxx) it belongs to a character that starts before the cut, so walk
     * back to that character's lead byte. A valid sequence is at most 4 bytes.
     */
    cut = max;
    for (back = 0; back < 3 && cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80; back++)
        cut--;

    if (cut == max)
        return max;                     /* already on a boundary */
    if (((unsigned char)s[cut] & 0xC0) == 0x80)
        return max;                     /* not UTF-8 at all -- cut flat */

    lead = (unsigned char)s[cut];
    if (lead < 0x80)
        seq = 1;
    else if ((lead & 0xE0) == 0xC0)
        seq = 2;
    else if ((lead & 0xF0) == 0xE0)
        seq = 3;
    else if ((lead & 0xF8) == 0xF0)
        seq = 4;
    else
        return cut;                     /* invalid lead byte -- drop it */

    /* Keep the character only when all of it fits below the cap. */
    return cut + seq <= max ? cut + seq : cut;
}

static int is_space_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/*
 * True when s[i] ends a sentence rather than sitting inside a token. Requiring
 * whitespace (or end of input) after the mark is the whole test, and it is
 * enough for both false positives this corpus actually produces: "CUDA 12.4"
 * and "conv_config.activation" are each followed by a non-space.
 *
 * What must NOT be added is a check on the byte before the mark. An earlier
 * version rejected a dot preceded by a digit, to catch a trailing "12.", and it
 * silently broke the common case instead -- "wrong results for src1." and
 * "fails on sm_90." are ordinary sentence ends, and identifiers here end in a
 * digit constantly.
 */
static int is_sentence_end(const char *s, size_t len, size_t i)
{
    if (s[i] != '.' && s[i] != '!' && s[i] != '?')
        return 0;
    return i + 1 >= len || is_space_byte(s[i + 1]);
}

size_t text_trunc_clean(const char *s, size_t len, size_t max)
{
    size_t cut, floor, i;

    if (s == NULL)
        return 0;
    if (len <= max)
        return len;

    cut = utf8_trunc_len(s, len, max);
    /* Below this a "cleaner" cut costs more text than the ragged edge does. */
    floor = max / 2;

    for (i = cut; i > floor; i--) {
        if (is_sentence_end(s, len, i - 1))
            return i;
    }
    for (i = cut; i > floor; i--) {
        if (is_space_byte(s[i - 1])) {
            while (i > 0 && is_space_byte(s[i - 1]))
                i--;
            return i;
        }
    }
    return cut;
}

const char *env_or_null(const char *name)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : NULL;
}
