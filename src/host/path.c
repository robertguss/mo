#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "host/files.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

void mo_host_close(int descriptor) {
    MO_ASSERT(descriptor >= 0);
    /* Never retry close: Linux/macOS have released the descriptor even when
     * interrupted. Cleanup cannot report delayed I/O errors; writes do. */
    int result = close(descriptor);
    MO_ASSERT(result == 0 || errno != EBADF);
}

static MO_MUST_USE struct mo_file_error kind(enum mo_path_mode mode, const struct stat *st,
                                             bool trailing_slash) {
    if ((mode == PATH_FOLDER || mode == PATH_MAKE_FOLDER || trailing_slash) &&
        !S_ISDIR(st->st_mode))
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"};
    if ((mode == PATH_READ || mode == PATH_WRITE || mode == PATH_APPEND) && S_ISDIR(st->st_mode))
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "is a folder"};
    if ((mode == PATH_READ || mode == PATH_WRITE || mode == PATH_APPEND) && !S_ISREG(st->st_mode))
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "not a regular file"};
    return (struct mo_file_error){.status = FILE_OK};
}

static MO_MUST_USE int flags(enum mo_path_mode mode) {
    if (mode == PATH_WRITE || mode == PATH_APPEND)
        return O_WRONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK | (mode == PATH_APPEND ? O_APPEND : 0);
    return O_RDONLY | O_CLOEXEC | O_NONBLOCK;
}

#if defined(__linux__)
static MO_MUST_USE struct mo_file_error kernel(int root, const char *path, enum mo_path_mode mode,
                                               struct mo_path_result *out, bool *handled) {
    *handled = false;
    if (mode == PATH_MAKE_FOLDER)
        return (struct mo_file_error){.status = FILE_OK};
    char parent[MO_LIMIT_HOST_PATH_SIZE];
    const char *lookup = path[0] == '\0' ? "." : path;
    if (mode == PATH_REMOVE) {
        memcpy(parent, lookup, strlen(lookup) + 1);
        char *slash = strrchr(parent, '/');
        const char *leaf = slash == NULL ? parent : slash + 1;
        memcpy(out->name, leaf, strlen(leaf) + 1);
        if (slash != NULL)
            *slash = '\0';
        lookup = slash == NULL ? "." : parent;
    }
    int open_flags = flags(mode);
    if (mode == PATH_EXISTS || mode == PATH_FOLDER)
        open_flags = O_PATH | O_CLOEXEC;
    if (mode == PATH_REMOVE)
        open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    struct open_how how = {.flags = (uint64_t)(unsigned)open_flags,
                           .mode = mode == PATH_WRITE || mode == PATH_APPEND ? 0666 : 0,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS};
    long opened;
    do {
        opened = syscall(SYS_openat2, root, lookup, &how, sizeof how);
    } while (opened < 0 && errno == EINTR);
    if (opened < 0) {
        int number = errno;
        if (number == ENOSYS || number == EINVAL || number == EAGAIN)
            return (struct mo_file_error){.status = FILE_OK};
        *handled = true;
        return mo_host_errno(number);
    }
    *handled = true;
    MO_ASSERT(opened <= INT_MAX);
    int fd = (int)opened;
    int inspected = mode == PATH_REMOVE ? fstatat(fd, out->name, &out->stat, AT_SYMLINK_NOFOLLOW)
                                        : fstat(fd, &out->stat);
    struct mo_file_error problem =
        inspected == 0 ? kind(mode, &out->stat, false) : mo_host_errno(errno);
    if (problem.status == FILE_OK && mode == PATH_FOLDER) {
        int directory = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            problem = mo_host_errno(errno);
        mo_host_close(fd);
        fd = directory;
    }
    if (problem.status != FILE_OK) {
        if (fd >= 0)
            mo_host_close(fd);
        return problem;
    }
    out->descriptor = fd;
    return problem;
}
#endif

/* Directory-object authority, not an atomic namespace snapshot. Each opened
 * directory is authorized until this lookup ends; another process can move
 * it meanwhile. POSIX cannot prevent that reparenting. Link '..' pops these
 * recorded ancestors, never the object's possibly changed OS parent. */
static MO_MUST_USE struct mo_file_error portable(int root, const char *path, enum mo_path_mode mode,
                                                 struct mo_path_result *out) {
    int ancestors[MO_LIMIT_HOST_PATH_SIZE / 2 + 1];
    uint32_t depth = 0, links = 0, races = 0;
    ancestors[0] = root;
    char pending[MO_LIMIT_HOST_PATH_SIZE], component[MO_LIMIT_HOST_PATH_SIZE],
        target[MO_LIMIT_HOST_PATH_SIZE];
    memcpy(pending, path, strlen(path) + 1);
    size_t at = 0;
    bool trailing_slash = false;
    struct mo_file_error problem = {.status = FILE_OK};
    for (;;) {
        while (pending[at] == '/')
            at++;
        if (pending[at] == '\0') {
            int fd = fcntl(ancestors[depth], F_DUPFD_CLOEXEC, 0);
            if (fd < 0) {
                problem = mo_host_errno(errno);
                break;
            }
            int inspected = fstat(fd, &out->stat);
            problem =
                inspected == 0 ? kind(mode, &out->stat, trailing_slash) : mo_host_errno(errno);
            if (problem.status != FILE_OK) {
                mo_host_close(fd);
                break;
            }
            out->descriptor = fd;
            memcpy(out->name, ".", 2);
            break;
        }
        size_t end = at;
        while (pending[end] != '\0' && pending[end] != '/')
            end++;
        memcpy(component, pending + at, end - at);
        component[end - at] = '\0';
        size_t next = end;
        while (pending[next] == '/')
            next++;
        bool final = pending[next] == '\0';
        if (strcmp(component, ".") == 0) {
            at = next;
            continue;
        }
        if (strcmp(component, "..") == 0) {
            if (depth == 0) {
                problem.status = FILE_NOT_ALLOWED;
                break;
            }
            mo_host_close(ancestors[depth--]);
            at = next;
            continue;
        }
        struct stat st;
        int inspected = fstatat(ancestors[depth], component, &st, AT_SYMLINK_NOFOLLOW);
        bool missing = inspected < 0 && errno == ENOENT;
        if (missing && mode == PATH_MAKE_FOLDER) {
            if (mkdirat(ancestors[depth], component, 0777) != 0 && errno != EEXIST) {
                problem = mo_host_errno(errno);
                break;
            }
            inspected = fstatat(ancestors[depth], component, &st, AT_SYMLINK_NOFOLLOW);
            missing = false;
        }
        bool creating = missing && final && (mode == PATH_WRITE || mode == PATH_APPEND);
        if (inspected < 0 && !creating) {
            problem = mo_host_errno(errno);
            break;
        }
        if (!creating && S_ISLNK(st.st_mode) && !(final && mode == PATH_REMOVE)) {
            if (links == MO_LIMIT_PATH_LINKS) {
                problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "too many links"};
                break;
            }
            ssize_t n = readlinkat(ancestors[depth], component, target, sizeof target - 1);
            if (n < 0) {
                if (errno == EINVAL && races++ < MO_LIMIT_PATH_LINKS)
                    continue;
                problem = mo_host_errno(errno);
                break;
            }
            links++;
            size_t length = (size_t)n, rest = strlen(pending + next);
            if (length == 0 || target[0] == '/') {
                problem.status = FILE_NOT_ALLOWED;
                break;
            }
            if (length == sizeof target - 1 ||
                length + (rest != 0 ? 1 : 0) + rest >= sizeof pending) {
                problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
                break;
            }
            if (final && target[length - 1] == '/')
                trailing_slash = true;
            memmove(pending + length + (rest != 0 ? 1 : 0), pending + next, rest + 1);
            memcpy(pending, target, length);
            if (rest != 0)
                pending[length] = '/';
            at = 0;
            continue;
        }
        if (!final && !S_ISDIR(st.st_mode)) {
            problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"};
            break;
        }
        if (final && !creating) {
            problem = kind(mode, &st, trailing_slash);
            if (problem.status != FILE_OK)
                break;
            if (mode == PATH_REMOVE || mode == PATH_EXISTS) {
                int fd = fcntl(ancestors[depth], F_DUPFD_CLOEXEC, 0);
                if (fd < 0) {
                    problem = mo_host_errno(errno);
                    break;
                }
                out->descriptor = fd;
                out->stat = st;
                memcpy(out->name, component, strlen(component) + 1);
                break;
            }
        }
        int open_flags = final ? flags(mode) : O_RDONLY | O_DIRECTORY | O_CLOEXEC;
        if (final && (mode == PATH_FOLDER || mode == PATH_MAKE_FOLDER))
            open_flags |= O_DIRECTORY;
        int fd = openat(ancestors[depth], component, open_flags | O_NOFOLLOW, 0666);
        if (fd < 0) {
            if ((errno == ELOOP || errno == ENOTDIR) && races++ < MO_LIMIT_PATH_LINKS)
                continue;
            problem = mo_host_errno(errno);
            break;
        }
        if (fstat(fd, &st) != 0) {
            problem = mo_host_errno(errno);
            mo_host_close(fd);
            break;
        }
        if (final) {
            problem = kind(mode, &st, trailing_slash);
            if (problem.status != FILE_OK)
                mo_host_close(fd);
            else {
                out->descriptor = fd;
                out->stat = st;
            }
            break;
        }
        if (depth + 1 == MO_LIMIT_HOST_PATH_SIZE / 2 + 1) {
            mo_host_close(fd);
            problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
            break;
        }
        ancestors[++depth] = fd;
        at = next;
    }
    while (depth != 0)
        mo_host_close(ancestors[depth--]);
    return problem;
}

struct mo_file_error mo_host_resolve(struct mo_host *host, int root, const char *path,
                                     enum mo_path_mode mode, struct mo_path_result *out) {
    MO_ASSERT(root >= 0);
    if (strlen(path) >= MO_LIMIT_HOST_PATH_SIZE)
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
#if defined(__linux__)
    if (!host->force_fallback) {
        bool handled;
        struct mo_file_error problem = kernel(root, path, mode, out, &handled);
        if (handled)
            return problem;
    }
#else
    (void)host;
#endif
    return portable(root, path, mode, out);
}
