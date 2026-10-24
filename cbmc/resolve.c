#define _GNU_SOURCE
#define MO_LIMIT_PATH_STEPS 8
#include "base/limits.h"
#undef MO_LIMIT_HOST_PATH_SIZE
#define MO_LIMIT_HOST_PATH_SIZE 8
#undef MO_LIMIT_PATH_LINKS
#define MO_LIMIT_PATH_LINKS 2
#ifndef MO_CBMC_KERNEL
#define MO_HOST_NO_OPENAT2
#endif
#include "host/path.c"

#include <stdarg.h>
#include <stdlib.h>

#pragma CPROVER check push
#pragma CPROVER check disable "bounds"
#pragma CPROVER check disable "pointer"
#pragma CPROVER check disable "pointer-overflow"
#pragma CPROVER check disable "pointer-primitive"
#pragma CPROVER check disable "signed-overflow"
#pragma CPROVER check disable "conversion"

#define DESCRIPTORS 4
static bool live[DESCRIPTORS];

bool nondet_bool(void);
uint32_t nondet_u32(void);
uint64_t nondet_u64(void);
int nondet_int(void);

_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

void reached_end(void) {
}

void *memcpy(void *destination, const void *source, size_t bytes) {
    __CPROVER_assert(__CPROVER_r_ok(source, bytes) && __CPROVER_w_ok(destination, bytes),
                     "resolver: copy stays inside its buffers");
    char *to = destination;
    const char *from = source;
    for (size_t i = 0; i < bytes; i++)
        to[i] = from[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t bytes) {
    __CPROVER_assert(bytes <= MO_LIMIT_HOST_PATH_SIZE, "resolver: bounded link expansion");
    __CPROVER_assert(__CPROVER_r_ok(source, bytes) && __CPROVER_w_ok(destination, bytes),
                     "resolver: move stays inside its buffers");
    char saved[MO_LIMIT_HOST_PATH_SIZE];
    const char *from = source;
    char *to = destination;
    for (size_t i = 0; i < bytes; i++)
        saved[i] = from[i];
    for (size_t i = 0; i < bytes; i++)
        to[i] = saved[i];
    return destination;
}

static void descriptor(int fd) {
    __CPROVER_assert(fd >= 0 && fd < DESCRIPTORS && live[fd],
                     "resolver: uses only retained, root-authorized descriptors");
}

static void component(const char *name) {
    size_t size = 0;
    for (;;) {
        __CPROVER_assert(__CPROVER_r_ok(name + size, 1), "resolver: component bytes are readable");
        if (name[size] == '\0')
            break;
        __CPROVER_assert(name[size] != '/', "resolver: a syscall component has no slash");
        size++;
    }
    __CPROVER_assert(size != 0, "resolver: a syscall component is nonempty");
    __CPROVER_assert(strcmp(name, "..") != 0, "resolver: never asks the OS for a parent");
}

static int failure(void) {
    int number = nondet_int();
    __CPROVER_assume(number == ENOENT || number == ELOOP || number == ENOTDIR || number == EEXIST ||
                     number == EINVAL || number == EACCES || number == EMFILE || number == ENOSYS ||
                     number == EPERM || number == EXDEV);
    errno = number;
    return -1;
}

static int opened(void) {
    if (nondet_bool())
        return failure();
    for (int fd = 1; fd < DESCRIPTORS; fd++) {
        if (!live[fd]) {
            live[fd] = true;
            return fd;
        }
    }
    return failure();
}

int openat(int dirfd, const char *name, int options, ...) {
    descriptor(dirfd);
    component(name);
    __CPROVER_assert((options & O_NOFOLLOW) != 0, "resolver: pins every component without links");
    return opened();
}

int close(int fd) {
    descriptor(fd);
    __CPROVER_assert(fd != 0, "resolver: never closes the retained root");
    live[fd] = false;
    return 0;
}

int fcntl(int fd, int command, ...) {
    descriptor(fd);
    __CPROVER_assert(command == F_DUPFD_CLOEXEC, "resolver: duplicates a retained descriptor");
    return opened();
}

static void status(struct stat *out) {
    __CPROVER_assert(__CPROVER_w_ok(out, sizeof *out), "resolver: stat output fits its buffer");
    uint32_t kind = nondet_u32();
    __CPROVER_assume(kind == S_IFDIR || kind == S_IFREG || kind == S_IFLNK || kind == S_IFIFO);
    *out = (struct stat){.st_mode = kind, .st_dev = nondet_u64(), .st_ino = nondet_u64()};
}

int fstat(int fd, struct stat *out) {
    descriptor(fd);
    if (nondet_bool())
        return failure();
    status(out);
    return 0;
}

int fstatat(int dirfd, const char *name, struct stat *out, int options) {
    descriptor(dirfd);
    component(name);
    __CPROVER_assert(options == AT_SYMLINK_NOFOLLOW, "resolver: inspects links without following");
    if (nondet_bool())
        return failure();
    status(out);
    return 0;
}

int mkdirat(int dirfd, const char *name, mode_t mode) {
    (void)mode;
    descriptor(dirfd);
    component(name);
    return nondet_bool() ? failure() : 0;
}

ssize_t readlinkat(int dirfd, const char *name, char *out, size_t capacity) {
    descriptor(dirfd);
    component(name);
    __CPROVER_assert(capacity <= MO_LIMIT_HOST_PATH_SIZE && __CPROVER_w_ok(out, capacity),
                     "resolver: link output fits its buffer");
    if (nondet_bool())
        return failure();
    uint32_t length = nondet_u32();
    __CPROVER_assume(length <= capacity);
    for (uint32_t i = 0; i < length; i++) {
        char byte = (char)nondet_int();
        __CPROVER_assume(byte == '.' || byte == '/' || byte == 'a');
        out[i] = byte;
    }
    return (ssize_t)length;
}

struct mo_file_error mo_host_errno(int number) {
    return (struct mo_file_error){.status = number == ENOENT ? FILE_NOT_FOUND : FILE_FAILED,
                                  .number = number};
}

static size_t any_path(char path[MO_LIMIT_HOST_PATH_SIZE + 1], bool canonical) {
    uint32_t length = nondet_u32();
    __CPROVER_assume(length <= MO_LIMIT_HOST_PATH_SIZE);
    __CPROVER_assume(canonical || length < MO_LIMIT_HOST_PATH_SIZE);
    for (uint32_t i = 0; i < length; i++) {
        char byte = (char)nondet_int();
        __CPROVER_assume(byte == '.' || byte == '/' || byte == 'a');
        path[i] = byte;
    }
    path[length] = '\0';
    __CPROVER_assume(path[0] != '/');
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (canonical && (path[i] == '/' || i == length) && length != 0) {
            __CPROVER_assume(i > start);
            __CPROVER_assume(!(i - start == 1 && path[start] == '.'));
            __CPROVER_assume(!(i - start == 2 && path[start] == '.' && path[start + 1] == '.'));
            start = i + 1;
        }
    }
    return length;
}

#ifdef MO_CBMC_KERNEL
long syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    int root = va_arg(args, int);
    const char *path = va_arg(args, const char *);
    const struct open_how *how = va_arg(args, const struct open_how *);
    size_t size = va_arg(args, size_t);
    va_end(args);
    descriptor(root);
    __CPROVER_assert(__CPROVER_r_ok(how, sizeof *how), "kernel: options fit their buffer");
    __CPROVER_assert(number == SYS_openat2 && root == 0 && strlen(path) < MO_LIMIT_HOST_PATH_SIZE &&
                         path[0] != '/',
                     "kernel: opens a relative path from the retained root");
    __CPROVER_assert(size == sizeof *how && how->flags == (O_PATH | O_CLOEXEC) && how->mode == 0,
                     "kernel: pins without opening a driver");
    __CPROVER_assert(how->resolve == (RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS) ||
                         how->resolve ==
                             (RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS),
                     "kernel: always requires beneath-root resolution without magic links");
    static bool interrupted;
    if (!interrupted && nondet_bool()) {
        interrupted = true;
        errno = EINTR;
        return -1;
    }
    return opened();
}

int snprintf(char *out, size_t capacity, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int fd = va_arg(args, int);
    va_end(args);
    descriptor(fd);
    __CPROVER_assert(strcmp(format, "/proc/self/fd/%d") == 0 && capacity >= 16,
                     "kernel: formats a retained descriptor path");
    __CPROVER_assert(__CPROVER_w_ok(out, capacity), "kernel: formatted descriptor fits its buffer");
    memcpy(out, "/proc/self/fd/", 14);
    out[14] = (char)('0' + fd);
    out[15] = '\0';
    return 15;
}

int open(const char *path, int options, ...) {
    __CPROVER_assert(strncmp(path, "/proc/self/fd/", 14) == 0 && path[15] == '\0',
                     "kernel: reopens only its own descriptor");
    descriptor(path[14] - '0');
    __CPROVER_assert((options & O_CREAT) == 0, "kernel: reopening cannot create a pathname");
    return opened();
}

struct mo_file_error model_portable(int root, const char *path, enum mo_path_mode mode,
                                    uint32_t *steps, struct mo_path_result *out) {
    descriptor(root);
    __CPROVER_assert(__CPROVER_w_ok(steps, sizeof *steps) && __CPROVER_w_ok(out, sizeof *out),
                     "kernel: portable outputs fit their buffers");
    __CPROVER_assert(root == 0 && path[0] != '/' && strlen(path) < MO_LIMIT_HOST_PATH_SIZE &&
                         mode <= PATH_EXISTS && *steps <= MO_LIMIT_PATH_STEPS,
                     "kernel: checks link expansion under the portable proof's preconditions");
    uint32_t used = nondet_u32();
    __CPROVER_assume(used >= *steps && used <= MO_LIMIT_PATH_STEPS);
    *steps = used;
    if (nondet_bool()) {
        int fd = opened();
        if (fd >= 0) {
            out->descriptor = fd;
            return (struct mo_file_error){.status = FILE_OK};
        }
    }
    uint32_t status = nondet_u32();
    __CPROVER_assume(status > FILE_OK && status <= FILE_FAILED);
    return (struct mo_file_error){.status = (enum mo_file_status)status};
}

void harness_resolve_kernel(void) {
    live[0] = true;
    char path[MO_LIMIT_HOST_PATH_SIZE + 1];
    (void)any_path(path, true);
    uint32_t mode = nondet_u32();
    __CPROVER_assume(mode <= PATH_EXISTS);
    struct mo_host host = {.kernel_probed = nondet_bool(),
                           .kernel_available = nondet_bool(),
                           .force_fallback = nondet_bool()};
    struct mo_path_result out = {.descriptor = -1};
    struct mo_file_error error = mo_host_resolve(&host, 0, path, (enum mo_path_mode)mode, &out);
    __CPROVER_assert(live[0], "kernel: retains root authority");
    for (int fd = 1; fd < DESCRIPTORS; fd++)
        __CPROVER_assert(live[fd] == (error.status == FILE_OK && out.descriptor == fd),
                         "kernel: returns one retained descriptor or closes every temporary");
    if (error.status == FILE_OK)
        descriptor(out.descriptor);
    reached_end();
}
#else
void harness_resolve_portable(void) {
    live[0] = true;
    char path[MO_LIMIT_HOST_PATH_SIZE + 1];
    size_t length = any_path(path, false);
    __CPROVER_assume(length < MO_LIMIT_HOST_PATH_SIZE);
    uint32_t mode = nondet_u32();
    __CPROVER_assume(mode <= PATH_EXISTS);
    struct mo_path_result out = {.descriptor = -1};
    uint32_t steps = nondet_u32();
    __CPROVER_assume(steps <= MO_LIMIT_PATH_STEPS);
    uint32_t before = steps;
    struct mo_file_error error = portable(0, path, (enum mo_path_mode)mode, &steps, &out);
    __CPROVER_assert(steps >= before && steps <= MO_LIMIT_PATH_STEPS,
                     "resolver: shared work budget never decreases or exceeds its limit");
    __CPROVER_assert(live[0], "resolver: retains root authority");
    for (int fd = 1; fd < DESCRIPTORS; fd++)
        __CPROVER_assert(live[fd] == (error.status == FILE_OK && out.descriptor == fd),
                         "resolver: returns one retained descriptor or closes every temporary");
    if (error.status == FILE_OK)
        descriptor(out.descriptor);
    reached_end();
}
#endif

#pragma CPROVER check pop
