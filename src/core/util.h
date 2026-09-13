#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Logging. Everything goes to stderr; systemd captures it. */
typedef enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 } log_level_t;

void log_set_level(log_level_t lvl);
log_level_t log_get_level(void);
void log_msg(log_level_t lvl, const char *fmt, ...);

#define LOGE(...) log_msg(LOG_ERR,  __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)

/* Formats `t` as RFC3339 UTC ("2026-09-10T09:41:00Z") into `buf`. */
int iso8601_format(time_t t, char *buf, size_t buflen);

/* Parses RFC3339 UTC. Returns 0 and sets *out, or negative on a malformed input. */
int iso8601_parse(const char *s, time_t *out);

/* FNV-1a. Used for the seen-set key and nothing security-sensitive. */
uint64_t hash64(const void *data, size_t len);

/* Case-insensitive ASCII compare of the first `n` bytes. */
int ascii_strncasecmp(const char *a, const char *b, size_t n);

/*
 * Copies `src` into `dst` keeping only printable ASCII (0x20..0x7e), collapsing
 * anything else to a single space. Always NUL-terminates. Returns bytes written.
 * ntfy headers are ASCII-only and GitHub titles are full of emoji.
 */
size_t ascii_sanitize(char *dst, size_t dstlen, const char *src);

/*
 * Length to keep when cutting `s` to at most `max` bytes, moved back to a UTF-8
 * character boundary so the tail is never a half-written codepoint. A split
 * sequence is invalid UTF-8, yyjson refuses to encode it, and one clipped title
 * takes down the whole cycle's publish rather than mangling one field.
 */
size_t utf8_trunc_len(const char *s, size_t len, size_t max);

/*
 * utf8_trunc_len() that also refuses to stop mid-sentence: it backs up to the
 * last sentence terminator in the kept span, or failing that to the last word
 * boundary, and trims the trailing whitespace it leaves behind.
 *
 * A cut is only accepted when it keeps more than half of `max`, so a lone "."
 * near the start cannot collapse the text to nothing; below that the word
 * boundary is tried, and a run with no break at all still cuts flat. A mark is
 * a sentence end only when whitespace follows it, which is what keeps "3.8.0"
 * and "ttnn.conv1d" -- version numbers and dotted identifiers, most of what
 * these strings contain -- from being mistaken for one.
 */
size_t text_trunc_clean(const char *s, size_t len, size_t max);

/* Returns a required env var, or NULL. Logs nothing -- callers report. */
const char *env_or_null(const char *name);

#endif /* UTIL_H */
