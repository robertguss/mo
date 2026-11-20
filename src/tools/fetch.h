#ifndef MO_TOOLS_FETCH_H
#define MO_TOOLS_FETCH_H
#include "tools/tools.h"
#include <signal.h>
extern volatile sig_atomic_t mo_fetch_cancelled;

struct fetch_endpoint {
    uint64_t failure;
    const char *socket_path;
    uint64_t end, last_sent;
};

enum mo_fetch_live_status {
    MO_FETCH_LIVE_OK,
    MO_FETCH_LIVE_SOCKET,
    MO_FETCH_LIVE_CONNECT,
    MO_FETCH_LIVE_REPLACED,
    MO_FETCH_LIVE_RELEASED,
    MO_FETCH_LIVE_REFUSED,
};

uint64_t mo_fetch_now(void);
bool mo_fetch_socket_valid(const char *socket_path);
int mo_fetch_connect_unix(const char *socket_path, uint64_t end);
bool mo_fetch_send_request(int fd, const char *request, size_t length, uint64_t end);
bool mo_fetch_receive_response(int fd, struct mo_buffer *wire, uint64_t end);
bool mo_fetch_unhex(const char *s, uint8_t *out, size_t n);
MO_MUST_USE enum mo_fetch_live_status mo_fetch_live_export(const char *admin_socket,
                                                           uint64_t failure, struct mo_arena *arena,
                                                           const uint8_t **bytes, size_t *length);
int mo_fetch_client(const struct mo_cli *cli, FILE *output, FILE *errors, const char *command,
                    int (*attempt)(const struct mo_cli *, FILE *, FILE *, const char *, uint64_t));

#endif
