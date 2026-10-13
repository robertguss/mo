#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host_fixture.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#if defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#endif
#endif
#if defined(SYS_openat2) && defined(RESOLVE_BENEATH)
#define MO_TEST_OPENAT2 1
static bool seccomp = true;
#endif

/* glibc's GNU declarations use an enum; POSIX and other libcs use int. */
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
typedef __rlimit_resource_t resource_type;
#else
typedef int resource_type;
#endif

static int root;
static bool replace_ancestor, replace_reopen, deny_raise;
MO_MUST_USE ssize_t mo_host_resolver_readlink(int fd, const char *path, char *out, size_t length);
MO_MUST_USE long mo_host_resolver_syscall(long number, ...);
MO_MUST_USE int mo_host_resolver_open(const char *path, int flags, ...);
MO_MUST_USE int mo_host_resolver_setrlimit(resource_type resource, const struct rlimit *limit);

int mo_host_resolver_open(const char *path, int flags, ...) {
    MO_ASSERT((flags & O_CREAT) == 0);
    if (replace_reopen && strncmp(path, "/proc/self/fd/", sizeof "/proc/self/fd/" - 1) == 0)
        return openat(root, "substitute", flags);
    return open(path, flags);
}

int mo_host_resolver_setrlimit(resource_type resource, const struct rlimit *limit) {
    MO_ASSERT(resource == RLIMIT_NOFILE);
    if (deny_raise) {
        errno = EPERM;
        return -1;
    }
    return setrlimit(resource, limit);
}

/* A real ancestor replacement at the link-read seam, not a forged stat result.
 * The old ancestor stays alive, so its inode cannot accidentally match the new. */
ssize_t mo_host_resolver_readlink(int fd, const char *path, char *out, size_t length) {
    ssize_t result = readlinkat(fd, path, out, length);
    if (result >= 0 && replace_ancestor) {
        replace_ancestor = false;
        MO_ASSERT(renameat(root, "a", root, "old") == 0 && mkdirat(root, "a", 0700) == 0);
    }
    return result;
}

long mo_host_resolver_syscall(long number, ...) {
#if defined(MO_TEST_OPENAT2)
    if (!seccomp) {
        va_list args;
        va_start(args, number);
        int fd = va_arg(args, int);
        const char *path = va_arg(args, const char *);
        struct open_how *how = va_arg(args, struct open_how *);
        size_t length = va_arg(args, size_t);
        va_end(args);
        return syscall(number, fd, path, how, length);
    }
#endif
    (void)number;
    errno = EPERM; /* A seccomp profile denying the feature probe. */
    return -1;
}

static MO_MUST_USE unsigned descriptors(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 128; fd++) {
        int status = fcntl(fd, F_GETFD);
        MO_ASSERT(status >= 0 || errno == EBADF);
        count += status >= 0 ? 1u : 0u;
    }
    return count;
}

static void expect(struct host_fixture *f, struct mo_cell files, const char *path,
                   const char *expected) {
    struct mo_cell args[] = {files, host_text(f, path)}, out;
    struct mo_host_error error;
    unsigned before = descriptors();
    MO_ASSERT(mo_host_call(&f->host, f->heap, MO_HOST_FILES_READ_TEXT, f->result_text, args, &out,
                           &error) == MO_HOST_RETURNED);
    struct mo_buffer printed;
    mo_buffer_init(&printed, 1024);
    MO_ASSERT(mo_value_print(f->heap, f->result_text, out, &printed));
    CHECK(printed.length == strlen(expected) &&
          memcmp(printed.bytes, expected, printed.length) == 0);
    mo_buffer_free(&printed);
    mo_value_drop(f->heap, f->result_text, out);
    mo_value_drop(f->heap, f->types.text, args[1]);
    CHECK(descriptors() == before);
}

static void marker(int fd) {
    int file = openat(fd, "marker", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(file >= 0 && write(file, "kept", 4) == 4 && close(file) == 0);
}

int main(void) {
    char path[] = "/tmp/mo-host-resolver-XXXXXX";
    MO_ASSERT(mkdtemp(path) != NULL);
    root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(root >= 0 && mkdirat(root, "a", 0700) == 0 && mkdirat(root, "a/b", 0700) == 0 &&
              symlinkat("../marker", root, "a/b/up") == 0);
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell files;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL, root, &files,
                           &error) == MO_HOST_RETURNED);
    marker(root);
    /* EPERM at the feature probe must not refuse ordinary reads or exists. */
    expect(&f, files, "marker", "Ok(\"kept\")");
    struct rlimit original;
    struct sigaction previous;
    MO_ASSERT(getrlimit(RLIMIT_NOFILE, &original) == 0 && original.rlim_cur >= 64 &&
              sigaction(SIGPIPE, NULL, &previous) == 0);
    struct rlimit lowered = original;
    lowered.rlim_cur = 64;
    MO_ASSERT(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
    deny_raise = true;
    CHECK(mo_host_start(&f.host) == NULL);
    deny_raise = false;
    struct rlimit remaining;
    CHECK(getrlimit(RLIMIT_NOFILE, &remaining) == 0 && remaining.rlim_cur == 64);
    expect(&f, files, "marker", "Ok(\"kept\")");
    MO_ASSERT(setrlimit(RLIMIT_NOFILE, &original) == 0 && sigaction(SIGPIPE, &previous, NULL) == 0);
    struct mo_cell args[] = {files, host_text(&f, "marker")}, out;
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_FILES_EXISTS, f.boolean, args, &out, &error) ==
              MO_HOST_RETURNED &&
          mo_cell_get_bool(out));
    mo_value_drop(f.heap, f.types.text, args[1]);
    replace_ancestor = true;
    expect(&f, files, "a/b/up",
           "Error(Failed(path: \"a/b/up\", reason: \"changed while being used\"))");
    CHECK(unlinkat(root, "old/b/up", 0) == 0 && unlinkat(root, "old/b", AT_REMOVEDIR) == 0 &&
          unlinkat(root, "old", AT_REMOVEDIR) == 0 && unlinkat(root, "a", AT_REMOVEDIR) == 0);
    int current = root;
    char deep[136];
    for (unsigned i = 1; i <= 64; i++) {
        MO_ASSERT(mkdirat(current, "d", 0700) == 0);
        int next = openat(current, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(next >= 0);
        if (current != root)
            MO_ASSERT(close(current) == 0);
        current = next;
        memcpy(deep + 2 * (i - 1), "d/", 2);
        if (i == 63 || i == 64)
            marker(current);
    }
    memcpy(deep + 126, "marker", 7);
    expect(&f, files, deep, "Ok(\"kept\")"); /* 63 directories + the leaf = 64. */
    memcpy(deep + 126, "d/marker", 9);
    char expected[256];
    int n = snprintf(expected, sizeof expected,
                     "Error(Failed(path: \"%s\", reason: \"MO_LIMIT_PATH_STEPS\"))", deep);
    MO_ASSERT(n > 0 && (size_t)n < sizeof expected);
    expect(&f, files, deep, expected);
    /* 33 descents, then '..' replays 32 ancestors: the shared 64-step
     * budget runs out in replay, even though the original path is short. */
    char replay_path[80];
    for (unsigned i = 0; i < 33; i++)
        memcpy(replay_path + 2 * i, "d/", 2);
    memcpy(replay_path + 66, "../marker", 10);
    /* '..' must arise during link expansion, after lexical normalization. */
    int link_parent = openat(root, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(link_parent >= 0 && symlinkat(replay_path + 2, link_parent, "replay") == 0 &&
              close(link_parent) == 0);
    expect(&f, files, "d/replay",
           "Error(Failed(path: \"d/replay\", reason: \"MO_LIMIT_PATH_STEPS\"))");
    MO_ASSERT(unlinkat(root, "d/replay", 0) == 0);
#if defined(MO_TEST_OPENAT2)
    seccomp = false;
    f.host.kernel_probed = false;
    expect(&f, files, "marker", "Ok(\"kept\")");
    if (f.host.kernel_available) {
        int substitute = openat(root, "substitute", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        MO_ASSERT(substitute >= 0 && write(substitute, "outside sentinel", 16) == 16);
        replace_reopen = true;
        expect(&f, files, "marker",
               "Error(Failed(path: \"marker\", reason: \"changed while being used\"))");
        struct mo_cell refused[] = {files, host_text(&f, "marker"), host_text(&f, "changed")};
        unsigned before_reopen = descriptors();
        MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_WRITE_TEXT, f.result_unit, refused,
                               &out, &error) == MO_HOST_RETURNED);
        CHECK(mo_cell_get_tag(out) == 1);
        if (mo_cell_get_tag(out) == 1) {
            struct mo_cell failure = mo_value_field(mo_cell_get_payload(out), 0);
            CHECK(mo_cell_get_tag(failure) == 3);
        }
        mo_value_drop(f.heap, f.result_unit, out);
        mo_value_drop(f.heap, f.types.text, refused[1]);
        mo_value_drop(f.heap, f.types.text, refused[2]);
        CHECK(descriptors() == before_reopen);
        replace_reopen = false;
        char sentinel[16];
        CHECK(pread(substitute, sentinel, sizeof sentinel, 0) == 16 &&
              memcmp(sentinel, "outside sentinel", 16) == 0);
        expect(&f, files, "marker", "Ok(\"kept\")");
        CHECK(close(substitute) == 0 && unlinkat(root, "substitute", 0) == 0);
        /* Each portable pass takes 33 steps, within 64 separately, but
         * creation must share the budget with the Linux expansion check. */
        memcpy(replay_path + 62, "fresh", 6);
        MO_ASSERT(symlinkat(replay_path, root, "create-budget") == 0);
        struct mo_cell write_args[] = {files, host_text(&f, "create-budget"), host_text(&f, "new")};
        unsigned before = descriptors();
        MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_WRITE_TEXT, f.result_unit, write_args,
                               &out, &error) == MO_HOST_RETURNED);
        struct mo_buffer printed;
        mo_buffer_init(&printed, 1024);
        MO_ASSERT(mo_value_print(f.heap, f.result_unit, out, &printed));
        const char *wanted =
            "Error(Failed(path: \"create-budget\", reason: \"MO_LIMIT_PATH_STEPS\"))";
        CHECK(printed.length == strlen(wanted) &&
              memcmp(printed.bytes, wanted, printed.length) == 0);
        mo_buffer_free(&printed);
        mo_value_drop(f.heap, f.result_unit, out);
        mo_value_drop(f.heap, f.types.text, write_args[1]);
        mo_value_drop(f.heap, f.types.text, write_args[2]);
        struct stat absent;
        CHECK(fstatat(root, replay_path, &absent, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
        CHECK(descriptors() == before && unlinkat(root, "create-budget", 0) == 0);
    }
#endif
    for (unsigned i = 64; i != 0; i--) {
        if (i == 63 || i == 64)
            MO_ASSERT(unlinkat(current, "marker", 0) == 0);
        int parent = openat(current, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        MO_ASSERT(parent >= 0 && close(current) == 0 && unlinkat(parent, "d", AT_REMOVEDIR) == 0);
        current = parent;
    }
    MO_ASSERT(close(current) == 0);
    int large = openat(root, "large", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(large >= 0);
    char bytes[65]; /* One byte past the native copies' representation limit. */
    memset(bytes, 'x', sizeof bytes);
    MO_ASSERT(write(large, bytes, sizeof bytes) == (ssize_t)sizeof bytes &&
              lseek(large, 0, SEEK_SET) == 0);
    args[1] = host_text(&f, "large");
    out = mo_cell_int(99);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_FILES_READ_TEXT, f.result_text, args, &out,
                       &error) == MO_HOST_OUT_OF_MEMORY &&
          errno == EOVERFLOW && mo_cell_get_int(out) == 99);
    mo_value_drop(f.heap, f.types.text, args[1]);
    struct mo_cell console;
    f.host.input = large;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                           &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out,
                       &error) == MO_HOST_OUT_OF_MEMORY &&
          errno == EOVERFLOW && mo_cell_get_int(out) == 99);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    /* CRLF is a terminator, not part of the Text's representation length. */
    MO_ASSERT(ftruncate(large, 64) == 0 && pwrite(large, "\r\n", 2, 64) == 2 &&
              lseek(large, 0, SEEK_SET) == 0 &&
              mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_CONSOLE], NULL, -1, &console,
                           &error) == MO_HOST_RETURNED);
    enum mo_host_outcome outcome =
        mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out, &error);
    CHECK(outcome == MO_HOST_RETURNED);
    if (outcome == MO_HOST_RETURNED) {
        CHECK(mo_cell_get_tag(out) == 1);
        size_t length;
        const uint8_t *text = mo_value_data(mo_cell_get_payload(out), &length);
        CHECK(length == 64 && memcmp(text, bytes, 64) == 0);
        mo_value_drop(f.heap, f.option, out);
    }
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    CHECK(close(large) == 0 && unlinkat(root, "large", 0) == 0 &&
          unlinkat(root, "marker", 0) == 0 && close(root) == 0 && rmdir(path) == 0);
    return mo_test_report("host resolver limits and races");
}
