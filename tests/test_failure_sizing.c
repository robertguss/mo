#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/mo-capture-sizing-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0)
        return mo_test_report("failure sizing");
    CHECK(close(fd) == 0 && unlink(path) == 0);
    struct mo_store *store = NULL;
    CHECK(mo_store_open(path, &store) == MO_STORE_OK);
    struct mo_arena arena;
    size_t message_size = MO_LIMIT_FAILURE_MESSAGE_BYTES - MO_LIMIT_FAILURE_ENVELOPE_BYTES;
    mo_arena_init(&arena, MO_LIMIT_FAILURE_RECORD_BYTES + 4096);
    uint8_t *state = mo_arena_allocate(&arena, MO_LIMIT_SAVED_BYTES, 1);
    uint8_t *message = mo_arena_allocate(&arena, message_size, 1);
    MO_ASSERT(state != NULL && message != NULL);
    memset(state, 42, MO_LIMIT_SAVED_BYTES);
    memset(message, 43, message_size);
    struct mo_store_record before = {MO_STORE_FAILURE_STATE, 0, 1, state, MO_LIMIT_SAVED_BYTES};
    struct mo_store_record call = {MO_STORE_FAILURE_MESSAGE, 0, 1, message, message_size};
    struct timespec start, end;
    struct rusage baseline, peak;
    CHECK(getrusage(RUSAGE_SELF, &baseline) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    uint64_t id = 0;
    for (unsigned i = 0; i < 3; i++)
        CHECK(mo_store_capture(store, "maximum-record", &before, &call, &id) ==
              MO_STORE_CAPTURE_OK);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK(getrusage(RUSAGE_SELF, &peak) == 0);
    CHECK(id == 3);
    mo_store_close(store);
    struct stat info;
    CHECK(stat(path, &info) == 0);
    double seconds =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    CHECK(printf("{\"state_bytes\":%d,\"message_bytes\":%zu,\"pair_frame_bytes\":%zu,"
                 "\"three_transactions_seconds\":%.6f,\"baseline_rss_kib\":%ld,"
                 "\"peak_rss_kib\":%ld,\"sqlite_file_bytes\":%lld}\n",
                 MO_LIMIT_SAVED_BYTES, message_size,
                 (size_t)MO_LIMIT_SAVED_BYTES + message_size + 2 * MO_STORE_FRAME_BYTES, seconds,
                 baseline.ru_maxrss, peak.ru_maxrss, (long long)info.st_size) > 0);
    mo_arena_free(&arena);
    CHECK(unlink(path) == 0);
    return mo_test_report("failure sizing");
}
