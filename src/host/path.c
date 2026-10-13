#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host/files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__) && !defined(MO_HOST_NO_OPENAT2)
#include <sys/syscall.h>
#if defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#endif
#endif

#if defined(SYS_openat2) && defined(RESOLVE_BENEATH)
#define MO_HAVE_OPENAT2 1
#endif

void mo_host_close(int descriptor) {
    MO_ASSERT(descriptor >= 0);
    /* Do not retry close: the descriptor can already have been released. */
    int result = close(descriptor);
    MO_ASSERT(result == 0 || errno != EBADF);
}

static MO_MUST_USE int directory_flags(void) {
#if defined(O_SEARCH)
    return O_SEARCH | O_DIRECTORY | O_CLOEXEC;
#elif defined(O_EXEC)
    return O_EXEC | O_DIRECTORY | O_CLOEXEC;
#elif defined(O_PATH)
    return O_PATH | O_DIRECTORY | O_CLOEXEC;
#else
    return O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#endif
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
    int common = O_CLOEXEC | O_NONBLOCK | O_NOCTTY;
    if (mode == PATH_WRITE || mode == PATH_APPEND)
        return O_WRONLY | O_CREAT | common | (mode == PATH_APPEND ? O_APPEND : 0);
    return O_RDONLY | common;
}

static MO_MUST_USE struct mo_file_error portable(int root, const char *path, enum mo_path_mode mode,
                                                 uint32_t *steps, struct mo_path_result *out);

#if defined(MO_HAVE_OPENAT2)
static MO_MUST_USE long beneath(int root, const char *path, bool links) {
    struct open_how how = {.flags = O_PATH | O_CLOEXEC,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS |
                                      (links ? 0 : RESOLVE_NO_SYMLINKS)};
    long opened;
    do {
        opened = syscall(SYS_openat2, root, path, &how, sizeof how);
    } while (opened < 0 && errno == EINTR);
    return opened;
}

static MO_MUST_USE struct mo_file_error kernel(struct mo_host *host, int root, const char *path,
                                               enum mo_path_mode mode, uint32_t *steps,
                                               struct mo_path_result *out, bool *handled) {
    *handled = false;
    if (!host->kernel_probed) {
        long probe = beneath(root, ".", false);
        host->kernel_available =
            probe >= 0 || (errno != ENOSYS && errno != EINVAL && errno != EPERM);
        host->kernel_probed = true;
        if (probe >= 0) {
            MO_ASSERT(probe <= INT_MAX);
            mo_host_close((int)probe);
        }
    }
    if (!host->kernel_available || mode == PATH_MAKE_FOLDER)
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
    /* O_PATH cannot run a device driver. Classify the pinned object first;
     * reopening our own retained descriptor cannot race a pathname replacement. */
    long opened = beneath(root, lookup, false);
    if (opened < 0 && errno == ELOOP) {
        /* The kernel need not bound an expanded target plus its remaining
         * path by Mo's buffer size. Apply Mo's link/length limits without
         * opening the leaf for I/O, then pin beneath root with openat2.
         * This checks path semantics, not whole-lookup ancestry or a snapshot. */
        struct mo_path_result checked;
        struct mo_file_error problem = portable(root, lookup, PATH_EXISTS, steps, &checked);
        if (problem.status == FILE_NOT_FOUND)
            return (struct mo_file_error){.status = FILE_OK}; /* Creation uses the full walk. */
        if (problem.status != FILE_OK) {
            *handled = true;
            return problem;
        }
        mo_host_close(checked.descriptor);
        opened = beneath(root, lookup, true);
    }
    if (opened < 0) {
        if (errno == EXDEV) {
            *handled = true;
            return (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
        }
        /* Portable expansion supplies stable kind/length/link errors, including
         * ENOTDIR after a trailing-slash link and overlong expanded missing paths. */
        return (struct mo_file_error){.status = FILE_OK};
    }
    MO_ASSERT(opened <= INT_MAX);
    int fd = (int)opened;
    int inspected = mode == PATH_REMOVE ? fstatat(fd, out->name, &out->stat, AT_SYMLINK_NOFOLLOW)
                                        : fstat(fd, &out->stat);
    struct mo_file_error problem =
        inspected == 0 ? kind(mode, &out->stat, false) : mo_host_errno(errno);
    if (problem.status == FILE_OK &&
        (mode == PATH_READ || mode == PATH_WRITE || mode == PATH_APPEND)) {
        char retained[64];
        int n = snprintf(retained, sizeof retained, "/proc/self/fd/%d", fd);
        MO_ASSERT(n > 0 && (size_t)n < sizeof retained);
        int actual = open(retained, flags(mode) & ~O_CREAT);
        if (actual < 0) {
            /* A Linux system without procfs still has the portable resolver. */
            mo_host_close(fd);
            return (struct mo_file_error){.status = FILE_OK};
        }
        struct stat reopened;
        if (fstat(actual, &reopened) != 0)
            problem = mo_host_errno(errno);
        else if (reopened.st_dev != out->stat.st_dev || reopened.st_ino != out->stat.st_ino)
            problem =
                (struct mo_file_error){.status = FILE_FAILED, .reason = "changed while being used"};
        mo_host_close(fd);
        fd = actual;
    }
    *handled = true;
    if (problem.status != FILE_OK) {
        mo_host_close(fd);
        return problem;
    }
    out->descriptor = fd;
    return problem;
}
#endif

struct ancestor {
    dev_t device;
    ino_t inode;
    size_t name;
};

static MO_MUST_USE bool step(uint32_t *steps) {
    if (*steps == MO_LIMIT_PATH_STEPS)
        return false;
    (*steps)++;
    return true;
}

static MO_MUST_USE struct mo_file_error replay(int root, const struct ancestor *ancestors,
                                               const char *names, uint32_t depth, uint32_t *steps,
                                               int *out) {
    int current = root;
    struct mo_file_error problem = {.status = FILE_OK};
    for (uint32_t i = 0; i < depth; i++) {
        if (!step(steps)) {
            problem =
                (struct mo_file_error){.status = FILE_FAILED, .reason = "MO_LIMIT_PATH_STEPS"};
            break;
        }
        int next = openat(current, names + ancestors[i].name, directory_flags() | O_NOFOLLOW);
        if (next < 0) {
            problem =
                (struct mo_file_error){.status = FILE_FAILED, .reason = "changed while being used"};
            break;
        }
        struct stat st;
        int inspected = fstat(next, &st);
        if (inspected < 0 || st.st_dev != ancestors[i].device || st.st_ino != ancestors[i].inode) {
            mo_host_close(next);
            problem =
                (struct mo_file_error){.status = FILE_FAILED, .reason = "changed while being used"};
            break;
        }
        if (current != root)
            mo_host_close(current);
        current = next;
    }
    if (problem.status == FILE_OK)
        *out = current;
    else if (current != root)
        mo_host_close(current);
    return problem;
}

/* Directory-object authority, not an atomic namespace snapshot. Another process
 * can reparent an opened object. For '..' we reconstruct from the retained root,
 * never from an OS parent, with fresh authorization and saved identity checks.
 * Concurrent inode reuse may authorize a replacement still reached from root.
 * This is not a whole-lookup pre/post ancestry check. At most three temporary
 * descriptors are live, independent of depth; replay shares the work bound. */
static MO_MUST_USE struct mo_file_error portable(int root, const char *path, enum mo_path_mode mode,
                                                 uint32_t *steps, struct mo_path_result *out) {
    struct ancestor ancestors[MO_LIMIT_HOST_PATH_SIZE / 2 + 1];
    uint32_t depth = 0, links = 0, races = 0;
    int current = root;
    char names[MO_LIMIT_HOST_PATH_SIZE], pending[MO_LIMIT_HOST_PATH_SIZE],
        component[MO_LIMIT_HOST_PATH_SIZE], target[MO_LIMIT_HOST_PATH_SIZE];
    size_t name_size = 0, at = 0;
    memcpy(pending, path, strlen(path) + 1);
    bool trailing_slash = false;
    struct mo_file_error problem = {.status = FILE_OK};
    for (;;) {
        if (!step(steps)) {
            problem =
                (struct mo_file_error){.status = FILE_FAILED, .reason = "MO_LIMIT_PATH_STEPS"};
            break;
        }
        while (pending[at] == '/')
            at++;
        if (pending[at] == '\0') {
            int fd = fcntl(current, F_DUPFD_CLOEXEC, 0);
            if (fd < 0) {
                problem = mo_host_errno(errno);
                break;
            }
            int inspected = fstat(fd, &out->stat);
            problem =
                inspected == 0 ? kind(mode, &out->stat, trailing_slash) : mo_host_errno(errno);
            if (problem.status != FILE_OK)
                mo_host_close(fd);
            else {
                out->descriptor = fd;
                memcpy(out->name, ".", 2);
            }
            break;
        }
        size_t end = at;
        while (pending[end] != '\0' && pending[end] != '/')
            end++;
        size_t length = end - at;
        memcpy(component, pending + at, length);
        component[length] = '\0';
        size_t next = end;
        while (pending[next] == '/')
            next++;
        bool final = pending[next] == '\0';
        if (final && pending[end] == '/')
            trailing_slash = true;
        if (strcmp(component, ".") == 0) {
            at = next;
            continue;
        }
        if (strcmp(component, "..") == 0) {
            if (depth == 0) {
                problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
                break;
            }
            name_size = ancestors[--depth].name;
            /* Collapse a run of parent components before replay, so a deep
             * descent followed by deep '..' is linear rather than quadratic. */
            while (pending[next] == '.' && pending[next + 1] == '.' &&
                   (pending[next + 2] == '/' || pending[next + 2] == '\0')) {
                if (depth == 0) {
                    problem =
                        (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
                    break;
                }
                if (!step(steps)) {
                    problem = (struct mo_file_error){.status = FILE_FAILED,
                                                     .reason = "MO_LIMIT_PATH_STEPS"};
                    break;
                }
                name_size = ancestors[--depth].name;
                next += 2;
                while (pending[next] == '/')
                    next++;
            }
            if (problem.status != FILE_OK)
                break;
            if (current != root)
                mo_host_close(current);
            current = root;
            problem = replay(root, ancestors, names, depth, steps, &current);
            if (problem.status != FILE_OK)
                break;
            at = next;
            continue;
        }
        struct stat st;
        int inspected = fstatat(current, component, &st, AT_SYMLINK_NOFOLLOW);
        bool missing = inspected < 0 && errno == ENOENT;
        if (missing && mode == PATH_MAKE_FOLDER) {
            if (mkdirat(current, component, 0777) != 0 && errno != EEXIST) {
                problem = mo_host_errno(errno);
                break;
            }
            inspected = fstatat(current, component, &st, AT_SYMLINK_NOFOLLOW);
            missing = false;
        }
        bool creating = missing && final && (mode == PATH_WRITE || mode == PATH_APPEND);
        if (creating && trailing_slash) {
            problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "not a folder"};
            break;
        }
        if (inspected < 0 && !creating) {
            problem = mo_host_errno(errno);
            break;
        }
        if (!creating && S_ISLNK(st.st_mode) && !(final && mode == PATH_REMOVE)) {
            if (links == MO_LIMIT_PATH_LINKS) {
                problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "too many links"};
                break;
            }
            ssize_t n = readlinkat(current, component, target, sizeof target);
            if (n < 0) {
                if (errno == EINVAL && races++ < MO_LIMIT_PATH_LINKS)
                    continue;
                problem = mo_host_errno(errno);
                break;
            }
            links++;
            size_t size = (size_t)n, rest = strlen(pending + next);
            if (size == 0 || target[0] == '/') {
                problem = (struct mo_file_error){.status = FILE_NOT_ALLOWED, .containment = true};
                break;
            }
            if (size + (rest != 0 ? 1 : 0) + rest >= sizeof pending) {
                problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
                break;
            }
            memmove(pending + size + (rest != 0 ? 1 : 0), pending + next, rest + 1);
            memcpy(pending, target, size);
            if (rest != 0)
                pending[size] = '/';
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
                int fd = fcntl(current, F_DUPFD_CLOEXEC, 0);
                if (fd < 0) {
                    problem = mo_host_errno(errno);
                    break;
                }
                out->descriptor = fd;
                out->stat = st;
                memcpy(out->name, component, length + 1);
                break;
            }
        }
        int open_flags = final ? flags(mode) : directory_flags();
        if (final && (mode == PATH_FOLDER || mode == PATH_MAKE_FOLDER))
            open_flags = directory_flags();
        if (creating)
            open_flags |= O_EXCL;
        int fd = openat(current, component, open_flags | O_NOFOLLOW, 0666);
        if (fd < 0) {
            if ((errno == ELOOP || errno == ENOTDIR || (creating && errno == EEXIST)) &&
                races++ < MO_LIMIT_PATH_LINKS)
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
        if (depth == MO_LIMIT_HOST_PATH_SIZE / 2 || length + 1 > sizeof names - name_size) {
            mo_host_close(fd);
            problem = (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
            break;
        }
        ancestors[depth++] = (struct ancestor){st.st_dev, st.st_ino, name_size};
        memcpy(names + name_size, component, length + 1);
        name_size += length + 1;
        if (current != root)
            mo_host_close(current);
        current = fd;
        at = next;
    }
    if (current != root)
        mo_host_close(current);
    return problem;
}

struct mo_file_error mo_host_resolve(struct mo_host *host, int root, const char *path,
                                     enum mo_path_mode mode, struct mo_path_result *out) {
    MO_ASSERT(root >= 0);
    if (strlen(path) >= MO_LIMIT_HOST_PATH_SIZE)
        return (struct mo_file_error){.status = FILE_FAILED, .reason = "path too long"};
    uint32_t steps = 0; /* Also shared by the Linux link check and its fallback. */
#if defined(MO_HAVE_OPENAT2)
    if (!host->force_fallback) {
        bool handled;
        struct mo_file_error problem = kernel(host, root, path, mode, &steps, out, &handled);
        if (handled)
            return problem;
    }
#else
    (void)host;
#endif
    return portable(root, path, mode, &steps, out);
}
