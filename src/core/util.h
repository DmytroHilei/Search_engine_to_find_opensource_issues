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

/* Returns a required env var, or NULL. Logs nothing -- callers report. */
const char *env_or_null(const char *name);

#endif /* UTIL_H */
