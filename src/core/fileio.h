#ifndef FILEIO_H
#define FILEIO_H

#include <stdio.h>

/*
 * The atomic-rewrite dance, in one place: temp file in the same directory,
 * fflush, fsync, rename(2), fsync the directory. Both persistent files (the
 * etag cache and the board) go through here -- a power cut must never leave
 * either one truncated, and that is not an invariant worth reimplementing per
 * call site.
 */

/*
 * Writes the file's entire contents to `f`. Returns 0 on success, negative on
 * failure -- a failure leaves the temp file behind unrenamed, so the previous
 * good file survives untouched.
 */
typedef int (*fileio_emit_fn)(FILE *f, void *user);

int fileio_path_join(char *out, size_t outlen, const char *dir, const char *name);

/*
 * `tmp_name` must name a file in the same directory as `name`: rename(2) is
 * only atomic within one filesystem. Returns 0, or negative errno.
 */
int fileio_atomic_write(const char *dir, const char *name, const char *tmp_name,
                        fileio_emit_fn emit, void *user);

#endif /* FILEIO_H */
