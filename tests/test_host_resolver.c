#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host_fixture.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int root;
static bool replace_ancestor;
MO_MUST_USE ssize_t mo_host_resolver_readlink(int fd, const char *path, char *out, size_t length);
MO_MUST_USE long mo_host_resolver_syscall(long number, ...);

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
    char bytes[128];
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
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    CHECK(close(large) == 0 && unlinkat(root, "large", 0) == 0 &&
          unlinkat(root, "marker", 0) == 0 && close(root) == 0 && rmdir(path) == 0);
    return mo_test_report("host resolver limits and races");
}
