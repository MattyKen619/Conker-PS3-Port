// posix_stubs.c - POSIX functions that libstdc++'s std::filesystem references but the PS3's
// newlib/PSL1GHT does not provide. The PS3 file systems have no symlinks or permission bits, so
// these report "not supported", which std::filesystem turns into a normal error_code.
#include <errno.h>
#include <limits.h>
#include <sys/types.h>

int symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char *path, char *buf, size_t size) {
    (void)path; (void)buf; (void)size;
    errno = EINVAL;  // "not a symbolic link"
    return -1;
}

long pathconf(const char *path, int name) {
    (void)path; (void)name;
    return 1024;  // used only to size getcwd's buffer
}

int fchmod(int fd, mode_t mode) {
    (void)fd; (void)mode;
    return 0;  // no permission bits on the PS3: treat as success
}

// remove(): the C standard says it deletes files *and* empty directories, and std::filesystem's
// remove()/remove_all() rely on that. newlib's remove() only calls unlink(), which the PS3 kernel
// refuses for directories ("Is a directory"), so fall back to rmdir() for those.
extern int unlink(const char *path);
extern int rmdir(const char *path);

int remove(const char *path) {
    if (unlink(path) == 0) return 0;
    int saved = errno;
    if (rmdir(path) == 0) return 0;
    errno = saved;
    return -1;
}

// getpagesize(): declared by newlib, not implemented. The PPU uses 4KB pages for normal memory.
int getpagesize(void) { return 4096; }

// stdio locking: newlib's stdio here is built without its own locking, so these are no-ops.
#include <stdio.h>
void flockfile(FILE *f) { (void)f; }
void funlockfile(FILE *f) { (void)f; }

// sysconf(): only the queries the runtime and libstdc++ make.
#include <unistd.h>
long sysconf(int name) {
#ifdef _SC_PAGESIZE
    if (name == _SC_PAGESIZE) return 4096;
#endif
#ifdef _SC_NPROCESSORS_ONLN
    if (name == _SC_NPROCESSORS_ONLN) return 2;
#endif
#ifdef _SC_NPROCESSORS_CONF
    if (name == _SC_NPROCESSORS_CONF) return 2;
#endif
    errno = EINVAL;
    return -1;
}
