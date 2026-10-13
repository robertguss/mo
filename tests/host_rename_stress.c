#define _POSIX_C_SOURCE 200809L
#include "host_fixture.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { ROUNDS = 64, LOOKUPS = 64, RENAMES = 256, DESCRIPTORS = 64 };

/* Move only within the root. Reparenting an opened directory outside it is
 * expressly outside the directory-object authority guarantee. An absolute
 * link or a '..' link escaping that object must still never be followed. */
static void mover(int root, int commands, int replies) {
    for (unsigned round = 0; round < ROUNDS; round++) {
        char byte;
        MO_ASSERT(read(commands, &byte, 1) == 1 && byte == 'r');
        for (unsigned i = 0; i < RENAMES; i++) {
            MO_ASSERT(renameat(root, "gate", root, "park") == 0);
            MO_ASSERT(renameat(root, "attack", root, "gate") == 0);
            MO_ASSERT(renameat(root, "gate", root, "attack") == 0);
            MO_ASSERT(renameat(root, "park", root, "gate") == 0);
        }
        MO_ASSERT(write(replies, "d", 1) == 1);
    }
    MO_ASSERT(close(root) == 0 && close(commands) == 0 && close(replies) == 0);
    _exit(0);
}

static MO_MUST_USE unsigned descriptor_count(void) {
    unsigned count = 0;
    for (int fd = 0; fd < DESCRIPTORS; fd++) {
        int result = fcntl(fd, F_GETFD);
        MO_ASSERT(result >= 0 || errno == EBADF);
        count += result >= 0 ? 1u : 0u;
    }
    return count;
}

static void put(int root, const char *path, const char *text) {
    int fd = openat(root, path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    MO_ASSERT(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text) && close(fd) == 0);
}

static void holds(struct mo_cell value, const char *expected) {
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    CHECK(length == strlen(expected) && memcmp(bytes, expected, length) == 0);
}

struct totals {
    unsigned returned, refused;
};

static void lookup(struct host_fixture *f, struct mo_cell files, const char *path, bool writing,
                   bool escape, bool racing, struct totals *totals) {
    struct mo_cell args[] = {files, host_text(f, path), host_text(f, "inside")};
    struct mo_cell result;
    struct mo_host_error error;
    uint32_t operation = writing ? MO_HOST_FILES_WRITE_TEXT : MO_HOST_FILES_READ_TEXT;
    uint32_t type = host_result_type(f, operation);
    MO_ASSERT(mo_host_call(&f->host, f->heap, operation, type, args, &result, &error) ==
              MO_HOST_RETURNED);
    if (mo_cell_get_tag(result) == 0) {
        totals->returned++;
        CHECK(!escape);
        if (!writing)
            holds(mo_cell_get_payload(result), "inside");
    } else {
        totals->refused++;
        struct mo_cell reason = mo_value_field(mo_cell_get_payload(result), 0);
        uint32_t tag = mo_cell_get_tag(reason);
        /* A rename can make a component disappear/change kind or exhaust the
         * bounded retry loop; no Error may be mistaken for a successful read. */
        CHECK(tag == 0 || tag == 1 || tag == 3);
        CHECK(racing || escape);
        if (!racing && escape)
            CHECK(tag == 1);
    }
    mo_value_drop(f->heap, type, result);
    mo_value_drop(f->heap, f->types.text, args[1]);
    mo_value_drop(f->heap, f->types.text, args[2]);
}

static void stress(bool portable) {
    char path[] = "/tmp/mo-host-race-XXXXXX";
    MO_ASSERT(mkdtemp(path) != NULL);
    int parent = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(parent >= 0 && mkdirat(parent, "root", 0700) == 0 &&
              mkdirat(parent, "outside", 0700) == 0);
    int root = openat(parent, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(root >= 0 && mkdirat(root, "gate", 0700) == 0 && mkdirat(root, "safe", 0700) == 0);
    put(parent, "outside/data", "outside sentinel");
    put(root, "gate/data", "inside");
    put(root, "safe/data", "inside");
    MO_ASSERT(symlinkat("../safe", root, "gate/back") == 0 &&
              symlinkat("../../outside", root, "gate/escape") == 0 &&
              symlinkat("../outside", root, "attack") == 0);
    char absolute[256];
    int n = snprintf(absolute, sizeof absolute, "%s/outside", path);
    MO_ASSERT(n > 0 && (size_t)n < sizeof absolute &&
              symlinkat(absolute, root, "gate/absolute") == 0);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    f.host.force_fallback = portable;
    struct mo_cell files;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_FILES], NULL, root, &files,
                           &error) == MO_HOST_RETURNED);
    struct totals totals = {0};
    /* Stable controls ensure the assertions catch escapes even when scheduling
     * happens to let every racy lookup run between rename batches. */
    lookup(&f, files, "gate/back/data", false, false, false, &totals);
    lookup(&f, files, "gate/escape/data", false, true, false, &totals);
    lookup(&f, files, "gate/absolute/data", true, true, false, &totals);
    int commands[2], replies[2];
    MO_ASSERT(pipe(commands) == 0 && pipe(replies) == 0);
    pid_t child = fork();
    MO_ASSERT(child >= 0);
    if (child == 0) {
        MO_ASSERT(close(commands[1]) == 0 && close(replies[0]) == 0 && close(parent) == 0);
        mover(root, commands[0], replies[1]);
    }
    MO_ASSERT(close(commands[0]) == 0 && close(replies[1]) == 0);
    unsigned before = descriptor_count();
    for (unsigned round = 0; round < ROUNDS; round++) {
        MO_ASSERT(write(commands[1], "r", 1) == 1);
        for (unsigned i = 0; i < LOOKUPS; i++) {
            lookup(&f, files, "gate/data", false, false, true, &totals);
            lookup(&f, files, "gate/back/data", true, false, true, &totals);
            lookup(&f, files, "gate/escape/data", false, true, true, &totals);
            lookup(&f, files, "gate/absolute/data", true, true, true, &totals);
        }
        char reply;
        MO_ASSERT(read(replies[0], &reply, 1) == 1 && reply == 'd');
        CHECK(descriptor_count() == before && f.host.open_folders == 1);
    }
    int status;
    MO_ASSERT(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(commands[1]) == 0 && close(replies[0]) == 0);
    int sentinel = openat(parent, "outside/data", O_RDONLY | O_CLOEXEC);
    MO_ASSERT(sentinel >= 0);
    char bytes[32];
    ssize_t length = read(sentinel, bytes, sizeof bytes);
    CHECK(length == 16 && memcmp(bytes, "outside sentinel", 16) == 0);
    CHECK(close(sentinel) == 0);
    lookup(&f, files, "gate/back/data", false, false, false, &totals);
    CHECK(printf("host rename stress (%s): %u operations, %u successes, %u refusals, "
                 "%u concurrent renames\n",
                 portable ? "portable" : "default", totals.returned + totals.refused,
                 totals.returned, totals.refused, ROUNDS * RENAMES * 4u) > 0);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    CHECK(unlinkat(root, "gate/data", 0) == 0 && unlinkat(root, "gate/back", 0) == 0 &&
          unlinkat(root, "gate/escape", 0) == 0 && unlinkat(root, "gate/absolute", 0) == 0 &&
          unlinkat(root, "gate", AT_REMOVEDIR) == 0 && unlinkat(root, "safe/data", 0) == 0 &&
          unlinkat(root, "safe", AT_REMOVEDIR) == 0 && unlinkat(root, "attack", 0) == 0);
    CHECK(close(root) == 0 && unlinkat(parent, "root", AT_REMOVEDIR) == 0 &&
          unlinkat(parent, "outside/data", 0) == 0 &&
          unlinkat(parent, "outside", AT_REMOVEDIR) == 0 && close(parent) == 0 && rmdir(path) == 0);
}

int main(void) {
    struct rlimit original;
    MO_ASSERT(getrlimit(RLIMIT_NOFILE, &original) == 0 && original.rlim_cur >= DESCRIPTORS);
    struct rlimit limited = original;
    limited.rlim_cur = DESCRIPTORS;
    MO_ASSERT(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    stress(false);
    stress(true);
    CHECK(setrlimit(RLIMIT_NOFILE, &original) == 0);
    return mo_test_report("host rename stress");
}
