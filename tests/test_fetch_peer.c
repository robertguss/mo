#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "tools/tools.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static unsigned mode, connections, peers, sends;
int mo_fetch_fault_open(const char *path, int flags, ...);
int mo_fetch_fault_connect(int fd, const struct sockaddr *address, socklen_t size);
int mo_fetch_fault_peer(int fd, struct sockaddr *address, socklen_t *size);
ssize_t mo_fetch_fault_send(int fd, const void *bytes, size_t size, int flags);

int mo_fetch_fault_open(const char *path, int flags, ...) {
    va_list arguments;
    va_start(arguments, flags);
    int permissions = va_arg(arguments, int);
    va_end(arguments);
    int fd = open(path, flags, (mode_t)permissions);
    if (fd >= 0 && mode == 2)
        MO_ASSERT(raise(SIGTERM) == 0);
    return fd;
}
int mo_fetch_fault_connect(int fd, const struct sockaddr *address, socklen_t size) {
    (void)fd;
    CHECK(address->sa_family == AF_UNIX && size == sizeof(struct sockaddr_un));
    connections++;
    if (mode == 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    return 0;
}
int mo_fetch_fault_peer(int fd, struct sockaddr *address, socklen_t *size) {
    (void)fd;
    peers++;
    MO_ASSERT(*size >= sizeof(struct sockaddr_in));
    struct sockaddr_in remote = {.sin_family = AF_INET,
                                 .sin_addr = {.s_addr = htonl(UINT32_C(0xc0000201))}};
    memcpy(address, &remote, sizeof remote);
    *size = sizeof remote;
    return 0;
}
ssize_t mo_fetch_fault_send(int fd, const void *bytes, size_t size, int flags) {
    (void)fd;
    (void)bytes;
    (void)size;
    (void)flags;
    sends++;
    errno = ECONNREFUSED;
    return -1;
}
int main(void) {
    char directory[] = "/tmp/mo-fetch-peer-XXXXXX";
    MO_ASSERT(mkdtemp(directory) != NULL);
    char path[128], sock[128];
    int n = snprintf(path, sizeof path, "%s/artifact", directory);
    int m = snprintf(sock, sizeof sock, "%s/admin.sock", directory);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path && m > 0 && (size_t)m < sizeof sock);
    for (mode = 0; mode < 3; mode++) {
        connections = peers = sends = 0;
        FILE *errors = tmpfile();
        MO_ASSERT(errors != NULL);
        struct mo_cli cli = {.admin_socket = sock, .output = path};
        CHECK(mo_fetch(&cli, errors) == 1);
        CHECK(access(path, F_OK) != 0 && sends == 0);
        CHECK(connections == (mode == 2 ? 0U : 1U) && peers == (mode == 1 ? 1U : 0U));
        CHECK(fclose(errors) == 0);
    }
    CHECK(rmdir(directory) == 0);
    return mo_test_report("fetch peer");
}
