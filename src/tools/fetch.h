#ifndef MO_TOOLS_FETCH_H
#define MO_TOOLS_FETCH_H
#include "tools/tools.h"
#include <signal.h>
extern volatile sig_atomic_t mo_fetch_cancelled;

struct fetch_endpoint {
    uint64_t failure;
    const char *host, *port, *token, *header;
    uint64_t end, last_sent;
};

uint64_t mo_fetch_now(void);
int mo_fetch_connect_local(const char *host, const char *port, uint64_t end);
bool mo_fetch_send_request(int fd, const char *request, size_t length, uint64_t end);
bool mo_fetch_receive_response(int fd, struct mo_buffer *wire, uint64_t end);
bool mo_fetch_unhex(const char *s, uint8_t *out, size_t n);
int mo_fetch_client(const struct mo_cli *cli, FILE *output, FILE *errors, const char *command,
                    int (*attempt)(const struct mo_cli *, FILE *, FILE *, const char *,
                                   const char *, uint64_t));

#endif
