#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char directory[] = "/tmp/desktop-compat-check.XXXXXX";
    if (!mkdtemp(directory)) return 1;
    int dirfd = open(directory, O_RDONLY | O_DIRECTORY);
    int file = openat(dirfd, "file", O_CREAT | O_WRONLY, 0600);
    if (dirfd < 0 || file < 0) return 1;
    close(file);
    if (symlinkat("missing", dirfd, "dangling")) return 1;
    int flags = AT_EACCESS | AT_SYMLINK_NOFOLLOW;
    int regular = faccessat(dirfd, "file", F_OK, flags);
    int dangling = faccessat(dirfd, "dangling", F_OK, flags);
    int missing = faccessat(dirfd, "missing", F_OK, flags);
    int missing_errno = errno;
    int plain = faccessat(dirfd, "file", R_OK, 0);
    int executable = faccessat(dirfd, "file", X_OK, 0);
    int execute_errno = errno;
    unlinkat(dirfd, "file", 0);
    unlinkat(dirfd, "dangling", 0);
    close(dirfd);
    rmdir(directory);
    printf("regular=%d dangling=%d missing=%d errno=%d read=%d execute=%d errno=%d\n",
           regular, dangling, missing, missing_errno, plain, executable, execute_errno);
    return !(regular == 0 && dangling == 0 && missing == -1 && missing_errno == ENOENT
             && plain == 0 && executable == -1 && execute_errno == EACCES);
}
