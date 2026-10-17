/* GLib 2.88 tests file existence with faccessat(F_OK, AT_EACCESS |
 * AT_SYMLINK_NOFOLLOW). musl returns EINVAL for this combination on kernels
 * without faccessat2 (Linux < 5.8). fstatat implements the same existence
 * check, including dangling symlinks and directory traversal permissions.
 * Apply only to the local desktop through LD_PRELOAD, never globally.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (mode == F_OK && flags == (AT_EACCESS | AT_SYMLINK_NOFOLLOW)) {
        struct stat info;
        return fstatat(dirfd, path, &info, AT_SYMLINK_NOFOLLOW);
    }
    int (*original)(int, const char *, int, int) = dlsym(RTLD_NEXT, "faccessat");
    if (!original) {
        errno = ENOSYS;
        return -1;
    }
    return original(dirfd, path, mode, flags);
}
