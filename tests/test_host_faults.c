#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "host_fixture.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Only this target's copies of host.c, random.c, console.c and real-files.c
 * rename these calls. Fixture setup and ordinary builds use the real system. */
static bool entropy_failure, clock_override, clock_failure;
static struct timespec clock_value;
struct io_plan {
    const int *steps; /* negative errno, zero result, or maximum transferred bytes */
    size_t count, at;
};
static struct io_plan reads, writes, polls;
static bool truncate_failure;

MO_MUST_USE int mo_host_fault_entropy(void *bytes, size_t length);
MO_MUST_USE int mo_host_fault_clock(clockid_t id, struct timespec *out);
MO_MUST_USE ssize_t mo_host_fault_read(int fd, void *bytes, size_t length);
MO_MUST_USE ssize_t mo_host_fault_write(int fd, const void *bytes, size_t length);
MO_MUST_USE ssize_t mo_host_fault_writev(int fd, const struct iovec *pieces, int count);
MO_MUST_USE int mo_host_fault_poll(struct pollfd *fds, nfds_t count, int timeout);
MO_MUST_USE int mo_host_fault_truncate(int fd, off_t length);

int mo_host_fault_entropy(void *bytes, size_t length) {
    if (entropy_failure) {
        errno = ENOSYS;
        return -1;
    }
    return getentropy(bytes, length);
}

int mo_host_fault_clock(clockid_t id, struct timespec *out) {
    MO_ASSERT(id == CLOCK_REALTIME);
    if (clock_failure) {
        errno = EIO;
        return -1;
    }
    if (clock_override) {
        *out = clock_value;
        return 0;
    }
    return clock_gettime(id, out);
}

static MO_MUST_USE int transfer(struct io_plan *plan, size_t *length) {
    if (plan->at == plan->count)
        return 1;
    int step = plan->steps[plan->at++];
    if (step < 0) {
        errno = -step;
        return -1;
    }
    if (step == 0)
        return 0;
    if (*length > (size_t)step)
        *length = (size_t)step;
    return 1;
}

ssize_t mo_host_fault_read(int fd, void *bytes, size_t length) {
    int result = transfer(&reads, &length);
    return result <= 0 ? result : read(fd, bytes, length);
}

ssize_t mo_host_fault_write(int fd, const void *bytes, size_t length) {
    int result = transfer(&writes, &length);
    return result <= 0 ? result : write(fd, bytes, length);
}

ssize_t mo_host_fault_writev(int fd, const struct iovec *pieces, int count) {
    MO_ASSERT(count > 0 && count <= 2);
    struct iovec limited[2];
    size_t length = 0;
    for (int i = 0; i < count; i++) {
        MO_ASSERT(pieces[i].iov_len <= SIZE_MAX - length);
        length += pieces[i].iov_len;
    }
    int result = transfer(&writes, &length);
    if (result <= 0)
        return result;
    int used = 0;
    for (int i = 0; i < count && length != 0; i++) {
        limited[used] = pieces[i];
        if (limited[used].iov_len > length)
            limited[used].iov_len = length;
        length -= limited[used].iov_len;
        used++;
    }
    return writev(fd, limited, used);
}

int mo_host_fault_poll(struct pollfd *fds, nfds_t count, int timeout) {
    size_t length = 1;
    int result = transfer(&polls, &length);
    return result <= 0 ? result : poll(fds, count, timeout);
}

int mo_host_fault_truncate(int fd, off_t length) {
    if (truncate_failure) {
        errno = EACCES;
        return -1;
    }
    return ftruncate(fd, length);
}

static MO_MUST_USE int temporary(void) {
    char path[] = "/tmp/mo-host-fault-XXXXXX";
    int fd = mkstemp(path);
    MO_ASSERT(fd >= 0 && unlink(path) == 0);
    return fd;
}

static void holds(struct mo_cell value, const char *expected) {
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    CHECK(length == strlen(expected) && (length == 0 || memcmp(bytes, expected, length) == 0));
}

static void file_holds(int fd, const char *expected) {
    char bytes[64];
    MO_ASSERT(lseek(fd, 0, SEEK_SET) == 0);
    ssize_t n = read(fd, bytes, sizeof bytes);
    CHECK(n >= 0 && (size_t)n == strlen(expected) &&
          (n == 0 || memcmp(bytes, expected, (size_t)n) == 0));
}

static MO_MUST_USE struct mo_cell real(struct host_fixture *f, enum mo_capability cap, int fd) {
    struct mo_cell result;
    struct mo_host_error error;
    MO_ASSERT(mo_host_real(&f->host, f->heap, f->capabilities[cap], NULL, fd, &result, &error) ==
              MO_HOST_RETURNED);
    return result;
}

static void test_clock_entropy(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_host_error error;
    struct mo_cell clock = real(&f, MO_CAP_CLOCK, -1), out = mo_cell_int(99);
    clock_failure = true;
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_NOW, f.time, &clock, &out, &error) ==
          MO_HOST_INTERNAL);
    CHECK(mo_cell_get_int(out) == 99 &&
          strcmp(error.reason, "internal error: real-time clock unavailable") == 0);
    struct mo_cell fixed, time = mo_cell_time(37);
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_FIXED, f.capabilities[MO_CAP_CLOCK],
                           &time, &fixed, &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_NOW, f.time, &fixed, &out, &error) ==
              MO_HOST_RETURNED &&
          mo_cell_get_time(out) == 37);
    clock_failure = false;
    clock_override = true;
    const struct {
        int64_t seconds;
        long nanoseconds;
        int64_t expected;
        bool allowed;
    } cases[] = {{-62135596800, 0, INT64_C(-62135596800000000), true},
                 {-62135596801, 999999999, 0, false},
                 {253402300799, 999999999, INT64_C(253402300799999999), true},
                 {253402300800, 0, 0, false},
                 {123, 456789999, 123456789, true},
                 {-1, 999999999, -1, true}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        clock_value = (struct timespec){(time_t)cases[i].seconds, cases[i].nanoseconds};
        out = mo_cell_int(99);
        enum mo_host_outcome result =
            mo_host_call(&f.host, f.heap, MO_HOST_CLOCK_NOW, f.time, &clock, &out, &error);
        if (cases[i].allowed)
            CHECK(result == MO_HOST_RETURNED && mo_cell_get_time(out) == cases[i].expected);
        else
            CHECK(result == MO_HOST_BUG && mo_cell_get_int(out) == 99 &&
                  strcmp(error.reason, "time outside the years 1 to 9999") == 0);
    }
    clock_override = false;
    entropy_failure = true;
    out = mo_cell_int(99);
    CHECK(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_RANDOM], NULL, -1, &out, &error) ==
          MO_HOST_INTERNAL);
    CHECK(mo_cell_get_int(out) == 99 &&
          strcmp(error.reason, "internal error: system randomness unavailable") == 0);
    struct mo_cell seeded, seed = mo_cell_int(17);
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_RANDOM_SEEDED, f.capabilities[MO_CAP_RANDOM],
                           &seed, &seeded, &error) == MO_HOST_RETURNED);
    struct mo_cell args[] = {seeded, mo_cell_int(-37), mo_cell_int(71)};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_RANDOM_INT, f.types.integer, args, &out, &error) ==
              MO_HOST_RETURNED &&
          mo_cell_get_int(out) >= -37 && mo_cell_get_int(out) < 71);
    entropy_failure = false;
    struct mo_cell random = real(&f, MO_CAP_RANDOM, -1);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_RANDOM], random);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_RANDOM], seeded);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CLOCK], fixed);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CLOCK], clock);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static void test_console(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    int input = temporary(), output = temporary();
    f.host.input = input;
    f.host.output = output;
    f.host.errors = output;
    struct mo_cell console = real(&f, MO_CAP_CONSOLE, -1), text = host_text(&f, "abcd");
    struct mo_cell args[] = {console, text}, out;
    struct mo_host_error error;
    const int short_io[] = {-EINTR, -EAGAIN, 1, -EINTR, 2};
    const int interrupted_poll[] = {-EINTR};
    writes = (struct io_plan){short_io, 5, 0};
    polls = (struct io_plan){interrupted_poll, 1, 0};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_PRINT, f.unit, args, &out, &error) ==
          MO_HOST_RETURNED);
    CHECK(writes.at == writes.count && polls.at == polls.count);
    file_holds(output, "abcd\n");
    polls = (struct io_plan){0};
    const int failures[][2] = {{-EPIPE, 0}, {0, 0}, {2, -EIO}, {4, -EPIPE}};
    const char *partial[] = {"", "", "ab", "abcd"};
    for (unsigned i = 0; i < 4; i++) {
        MO_ASSERT(ftruncate(output, 0) == 0 && lseek(output, 0, SEEK_SET) == 0);
        writes = (struct io_plan){failures[i], i < 2 ? 1 : 2, 0};
        out = mo_cell_int(99);
        CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_ERROR, f.unit, args, &out, &error) ==
              MO_HOST_BUG);
        CHECK(mo_cell_get_int(out) == 99 && writes.at == writes.count &&
              strcmp(error.reason, "printing to an output that is gone") == 0);
        holds(text, "abcd");
        file_holds(output, partial[i]);
    }
    const int blocked[] = {-EAGAIN};
    const int bad_poll[] = {-EINTR, -EIO};
    MO_ASSERT(ftruncate(output, 0) == 0 && lseek(output, 0, SEEK_SET) == 0);
    writes = (struct io_plan){blocked, 1, 0};
    polls = (struct io_plan){bad_poll, 2, 0};
    out = mo_cell_int(99);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_PRINT, f.unit, args, &out, &error) ==
          MO_HOST_BUG);
    CHECK(mo_cell_get_int(out) == 99 && polls.at == polls.count &&
          strcmp(error.reason, "printing to an output that is gone") == 0);
    holds(text, "abcd");
    file_holds(output, "");
    writes = (struct io_plan){0};
    reads = (struct io_plan){blocked, 1, 0};
    polls = (struct io_plan){bad_poll, 2, 0};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out,
                       &error) == MO_HOST_INTERNAL);
    CHECK(mo_cell_get_int(out) == 99 && polls.at == polls.count &&
          strcmp(error.reason, "internal error: console input unavailable") == 0);
    polls = (struct io_plan){0};
    const int bad_read[] = {-EIO};
    reads = (struct io_plan){bad_read, 1, 0};
    out = mo_cell_int(99);
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out,
                       &error) == MO_HOST_INTERNAL);
    CHECK(mo_cell_get_int(out) == 99 && reads.at == reads.count &&
          strcmp(error.reason, "internal error: console input unavailable") == 0);
    MO_ASSERT(write(input, "line\r\nnext\n", 11) == 11 && lseek(input, 0, SEEK_SET) == 0);
    reads = (struct io_plan){short_io, 5, 0};
    polls = (struct io_plan){interrupted_poll, 1, 0};
    const char *lines[] = {"line", "next"};
    for (unsigned i = 0; i < 2; i++) {
        MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out,
                               &error) == MO_HOST_RETURNED);
        CHECK(mo_cell_get_tag(out) == 1);
        holds(mo_cell_get_payload(out), lines[i]);
        mo_value_drop(f.heap, f.option, out);
    }
    CHECK(reads.at == reads.count && polls.at == polls.count);
    polls = (struct io_plan){0};
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_CONSOLE_READ_LINE, f.option, &console, &out,
                           &error) == MO_HOST_RETURNED);
    CHECK(mo_cell_get_tag(out) == 0);
    mo_value_drop(f.heap, f.option, out);
    reads = (struct io_plan){0};
    mo_value_drop(f.heap, f.types.text, text);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_CONSOLE], console);
    CHECK(close(input) == 0 && close(output) == 0 && mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

static MO_MUST_USE unsigned descriptors(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 128; fd++) {
        int result = fcntl(fd, F_GETFD);
        MO_ASSERT(result >= 0 || errno == EBADF);
        count += result >= 0 ? 1u : 0u;
    }
    return count;
}

static MO_MUST_USE struct mo_cell file_call(struct host_fixture *f, uint32_t operation,
                                            const struct mo_cell *args) {
    struct mo_cell result;
    struct mo_host_error error;
    unsigned before = descriptors();
    MO_ASSERT(mo_host_call(&f->host, f->heap, operation, host_result_type(f, operation), args,
                           &result, &error) == MO_HOST_RETURNED);
    CHECK(descriptors() == before);
    return result;
}

static void file_error(struct mo_cell result, uint32_t tag) {
    CHECK(mo_cell_get_tag(result) == 1);
    struct mo_cell error = mo_value_field(mo_cell_get_payload(result), 0);
    CHECK(mo_cell_get_tag(error) == tag);
    struct mo_cell payload = mo_cell_get_payload(error);
    holds(tag == 3 ? mo_value_field(payload, 0) : payload, "data");
    if (tag == 3)
        holds(mo_value_field(payload, 1), strerror(EIO));
}

static void test_files(bool portable) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    f.host.force_fallback = portable;
    char path[] = "/tmp/mo-host-fault-files-XXXXXX";
    MO_ASSERT(mkdtemp(path) != NULL);
    int folder = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(folder >= 0);
    struct mo_cell files = real(&f, MO_CAP_FILES, folder);
    struct mo_cell args[] = {files, host_text(&f, "data"), host_text(&f, "abcdef")};
    struct mo_cell result = file_call(&f, MO_HOST_FILES_WRITE_TEXT, args);
    CHECK(mo_cell_get_tag(result) == 0);
    mo_value_drop(f.heap, f.result_unit, result);
    int fd = openat(folder, "data", O_RDONLY | O_CLOEXEC);
    MO_ASSERT(fd >= 0);
    truncate_failure = true;
    result = file_call(&f, MO_HOST_FILES_WRITE_TEXT, args);
    file_error(result, 1); /* NotAllowed, with the original path. */
    file_holds(fd, "abcdef");
    mo_value_drop(f.heap, f.result_unit, result);
    truncate_failure = false;
    const int bad_write[] = {2, -EIO};
    writes = (struct io_plan){bad_write, 2, 0};
    result = file_call(&f, MO_HOST_FILES_WRITE_TEXT, args);
    file_error(result, 3); /* Failed; an external partial write cannot roll back. */
    CHECK(writes.at == writes.count);
    file_holds(fd, "ab");
    holds(args[1], "data");
    holds(args[2], "abcdef");
    mo_value_drop(f.heap, f.result_unit, result);
    const int zero_write[] = {0};
    writes = (struct io_plan){zero_write, 1, 0};
    result = file_call(&f, MO_HOST_FILES_APPEND_TEXT, args);
    file_error(result, 3);
    CHECK(writes.at == writes.count);
    file_holds(fd, "ab");
    mo_value_drop(f.heap, f.result_unit, result);
    const int short_io[] = {-EINTR, 1, -EINTR, 2};
    writes = (struct io_plan){short_io, 4, 0};
    result = file_call(&f, MO_HOST_FILES_WRITE_TEXT, args);
    CHECK(mo_cell_get_tag(result) == 0 && writes.at == writes.count);
    file_holds(fd, "abcdef");
    mo_value_drop(f.heap, f.result_unit, result);
    writes = (struct io_plan){0};
    const int bad_read[] = {2, -EIO};
    reads = (struct io_plan){bad_read, 2, 0};
    result = file_call(&f, MO_HOST_FILES_READ_TEXT, args);
    file_error(result, 3);
    CHECK(reads.at == reads.count);
    mo_value_drop(f.heap, f.result_text, result);
    reads = (struct io_plan){short_io, 4, 0};
    result = file_call(&f, MO_HOST_FILES_READ_TEXT, args);
    CHECK(mo_cell_get_tag(result) == 0 && reads.at == reads.count);
    holds(mo_cell_get_payload(result), "abcdef");
    mo_value_drop(f.heap, f.result_text, result);
    reads = (struct io_plan){0};
    CHECK(close(fd) == 0 && unlinkat(folder, "data", 0) == 0 && close(folder) == 0 &&
          rmdir(path) == 0);
    mo_value_drop(f.heap, f.types.text, args[1]);
    mo_value_drop(f.heap, f.types.text, args[2]);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_FILES], files);
    CHECK(mo_heap_objects(f.heap) == 0 && f.host.open_folders == 0);
    host_fixture_free(&f);
}

int main(void) {
    test_clock_entropy();
    test_console();
    test_files(false);
    test_files(true);
    return mo_test_report("host faults");
}
