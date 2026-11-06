#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "tools/tools.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned mode, connections, peers, sends;
int mo_fetch_fault_open(const char *path, int flags, ...);
int mo_fetch_fault_resolve(const char *node, const char *service, const struct addrinfo *hints,
                           struct addrinfo **result);
void mo_fetch_fault_free(struct addrinfo *addresses);
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
int mo_fetch_fault_resolve(const char *node, const char *service, const struct addrinfo *hints,
                           struct addrinfo **result) {
    (void)node;
    (void)service;
    CHECK((hints->ai_flags & AI_NUMERICHOST) != 0);
    static struct sockaddr_in address;
    static struct addrinfo resolved;
    address = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(mode == 0 ? UINT32_C(0xc0000201) : UINT32_C(0x7f000001))}};
    resolved = (struct addrinfo){.ai_family = AF_INET,
                                 .ai_socktype = SOCK_STREAM,
                                 .ai_addrlen = sizeof address,
                                 .ai_addr = (struct sockaddr *)&address};
    *result = &resolved;
    return 0;
}
void mo_fetch_fault_free(struct addrinfo *addresses) {
    (void)addresses;
}
int mo_fetch_fault_connect(int fd, const struct sockaddr *address, socklen_t size) {
    (void)fd;
    (void)address;
    (void)size;
    connections++;
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
    char path[128];
    int n = snprintf(path, sizeof path, "%s/artifact", directory);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    MO_ASSERT(setenv("MO_FETCH_PEER_TOKEN", "FetchPeerCredentialCanary", 1) == 0);
    for (mode = 0; mode < 3; mode++) {
        FILE *errors = tmpfile();
        MO_ASSERT(errors != NULL);
        struct mo_cli cli = {.fetch_url = "http://127.0.0.1:1234/mcp",
                             .output = path,
                             .fetch_token = "MO_FETCH_PEER_TOKEN"};
        CHECK(mo_fetch(&cli, errors) == 1);
        CHECK(access(path, F_OK) != 0 && sends == 0);
        CHECK(connections == (mode == 0 ? 0U : 1U) && peers == (mode == 0 ? 0U : 1U));
        CHECK(fflush(errors) == 0);
        rewind(errors);
        char text[1024] = {0};
        size_t size = fread(text, 1, sizeof text - 1, errors);
        CHECK(!ferror(errors) && size > 0 && strstr(text, "FetchPeerCredentialCanary") == NULL);
        CHECK(fclose(errors) == 0);
    }
    CHECK(rmdir(directory) == 0);
    return mo_test_report("fetch peer");
}
