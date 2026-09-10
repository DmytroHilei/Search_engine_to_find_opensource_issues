#include "core/fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/util.h"

/* Both callers' directory paths are bounded by STATE_PATH_MAX; this is the same
 * bound expressed without dragging state.h in for a constant. */
#define FILEIO_PATH_MAX 512

int fileio_path_join(char *out, size_t outlen, const char *dir, const char *name)
{
    int n;

    if (out == NULL || outlen == 0 || dir == NULL || name == NULL)
        return -EINVAL;

    n = snprintf(out, outlen, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= outlen)
        return -ENAMETOOLONG;
    return 0;
}

int fileio_atomic_write(const char *dir, const char *name, const char *tmp_name,
                        fileio_emit_fn emit, void *user)
{
    char final_path[FILEIO_PATH_MAX];
    char tmp_path[FILEIO_PATH_MAX];
    FILE *f = NULL;
    int fd = -1, dir_fd, rc;

    if (dir == NULL || name == NULL || tmp_name == NULL || emit == NULL)
        return -EINVAL;

    rc = fileio_path_join(final_path, sizeof final_path, dir, name);
    if (rc != 0)
        return rc;
    rc = fileio_path_join(tmp_path, sizeof tmp_path, dir, tmp_name);
    if (rc != 0)
        return rc;

    /* Temp file in the same directory so rename(2) is atomic. */
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -errno;

    f = fdopen(fd, "w");
    if (f == NULL) {
        rc = -errno;
        close(fd);
        goto out;
    }

    rc = emit(f, user);
    if (rc != 0)
        goto out;

    /* fflush before fsync: a short write buffered in stdio would otherwise be
     * fsync'd as a truncated file and then renamed over a good one. */
    if (fflush(f) != 0 || ferror(f)) {
        rc = -EIO;
        goto out;
    }
    if (fsync(fd) != 0) {
        rc = -errno;
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        rc = -EIO;
        goto out;
    }
    f = NULL;

    if (rename(tmp_path, final_path) != 0) {
        rc = -errno;
        goto out;
    }

    /* The rename itself only becomes durable once the directory is synced. */
    dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        if (fsync(dir_fd) != 0)
            LOGW("fileio: fsync on %s failed: %s", dir, strerror(errno));
        close(dir_fd);
    }

out:
    if (f != NULL)
        fclose(f);
    /* A failed write leaves the previous good file in place; the half-written
     * temp is the only casualty and must not be left to be renamed later. */
    if (rc != 0)
        unlink(tmp_path);
    return rc;
}
