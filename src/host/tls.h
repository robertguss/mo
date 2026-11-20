#ifndef MO_HOST_TLS_H
#define MO_HOST_TLS_H

#include "host/network.h"

enum mo_tls_step { MO_TLS_DONE, MO_TLS_WANT_READ, MO_TLS_WANT_WRITE, MO_TLS_FAILED };

struct mo_tls {
    void *ctx;
    void *ssl;
    char reason[256];
};

MO_MUST_USE struct mo_network_error mo_tls_open(struct mo_tls *tls, int fd, const uint8_t *host,
                                                size_t length);
MO_MUST_USE enum mo_tls_step mo_tls_handshake(struct mo_tls *tls, struct mo_network_error *error);
MO_MUST_USE enum mo_tls_step mo_tls_write(struct mo_tls *tls, const uint8_t *bytes, size_t length,
                                          size_t *written, struct mo_network_error *error);
MO_MUST_USE enum mo_tls_step mo_tls_read(struct mo_tls *tls, uint8_t *bytes, size_t most,
                                         size_t *count, struct mo_network_error *error);
void mo_tls_close(struct mo_tls *tls);

#endif
