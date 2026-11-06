#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "tools/tools.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned sync_calls, lookup_calls, mode;
int mo_fetch_deadline_lookup(const char *path, struct stat *out);
int mo_fetch_deadline_socket(int domain, int type, int protocol);
int mo_fetch_deadline_connect(int fd, const struct sockaddr *address, socklen_t size);
int mo_fetch_deadline_peer(int fd, struct sockaddr *address, socklen_t *size);
ssize_t mo_fetch_deadline_send(int fd, const void *bytes, size_t size, int flags);
int mo_fetch_deadline_sync(int fd);

int mo_fetch_deadline_lookup(const char *path, struct stat *out) {
    if (mode == 0)
        return lstat(path, out);
    lookup_calls++;
    struct timespec stall = {.tv_sec = 5};
    if (nanosleep(&stall, NULL) != 0)
        return -1;
    return lstat(path, out);
}
int mo_fetch_deadline_socket(int domain, int type, int protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return -1;
    FILE *fixture = fopen("fuzz/corpus/fuzz_fetch/page", "rb");
    MO_ASSERT(fixture != NULL);
    uint8_t bytes[4096];
    size_t length = fread(bytes, 1, sizeof bytes, fixture);
    MO_ASSERT(!ferror(fixture) && feof(fixture) && length > 0);
    MO_ASSERT(fclose(fixture) == 0);
    MO_ASSERT(write(pair[1], bytes, length) == (ssize_t)length);
    MO_ASSERT(close(pair[1]) == 0);
    return pair[0];
}
int mo_fetch_deadline_connect(int fd, const struct sockaddr *address, socklen_t size) {
    (void)fd;
    (void)address;
    (void)size;
    return 0;
}
int mo_fetch_deadline_peer(int fd, struct sockaddr *address, socklen_t *size) {
    (void)fd;
    struct sockaddr_in peer = {.sin_family = AF_INET,
                               .sin_addr = {.s_addr = htonl(UINT32_C(0x7f000001))}};
    MO_ASSERT(*size >= sizeof peer);
    memcpy(address, &peer, sizeof peer);
    *size = sizeof peer;
    return 0;
}
ssize_t mo_fetch_deadline_send(int fd, const void *bytes, size_t size, int flags) {
    (void)fd;
    (void)bytes;
    (void)flags;
    return (ssize_t)size;
}
int mo_fetch_deadline_sync(int fd) {
    (void)fd;
    sync_calls++;
    struct timespec stall = {.tv_sec = 5};
    return nanosleep(&stall, NULL);
}
int main(void) {
    char directory[] = "/tmp/mo-fetch-deadline-XXXXXX";
    MO_ASSERT(mkdtemp(directory) != NULL);
    char path[128];
    int n = snprintf(path, sizeof path, "%s/artifact", directory);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    MO_ASSERT(setenv("MO_FETCH_DEADLINE_TOKEN", "FetchDeadlineCanary", 1) == 0);
    FILE *errors = tmpfile();
    MO_ASSERT(errors != NULL);
    struct mo_cli cli = {.fetch_url = "http://127.0.0.1:1234/mcp",
                         .output = path,
                         .fetch_token = "MO_FETCH_DEADLINE_TOKEN"};
    for (mode = 0; mode < 2; mode++) {
        struct timespec before, after;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
        CHECK(mo_fetch(&cli, errors) == 1);
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
        double elapsed =
            (double)(after.tv_sec - before.tv_sec) + (double)(after.tv_nsec - before.tv_nsec) / 1e9;
        CHECK(sync_calls == 1 && lookup_calls == mode && elapsed < 3.5);
        CHECK(access(path, F_OK) != 0);
    }
    CHECK(fclose(errors) == 0);
    CHECK(rmdir(directory) == 0);
    return mo_test_report("fetch deadline");
}
