#define _POSIX_C_SOURCE 200809L
#include "host/tls.h"

#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *save_reason(struct mo_tls *tls, const char *message, const char *fallback) {
    const char *chosen = (message != NULL && message[0] != '\0') ? message : fallback;
    size_t length = strlen(chosen);
    if (length >= sizeof tls->reason)
        length = sizeof tls->reason - 1;
    memcpy(tls->reason, chosen, length);
    tls->reason[length] = '\0';
    return tls->reason;
}

static enum mo_tls_step classify(struct mo_tls *tls, SSL *ssl, int result, bool reading,
                                 size_t *read_count, struct mo_network_error *error) {
    int kind = SSL_get_error(ssl, result);
    if (kind == SSL_ERROR_WANT_READ)
        return MO_TLS_WANT_READ;
    if (kind == SSL_ERROR_WANT_WRITE)
        return MO_TLS_WANT_WRITE;
    if (kind == SSL_ERROR_ZERO_RETURN) {
        if (reading) {
            *read_count = 0;
            *error = (struct mo_network_error){.status = NETWORK_OK};
            return MO_TLS_DONE;
        }
        *error = (struct mo_network_error){.status = NETWORK_CLOSED};
        return MO_TLS_FAILED;
    }
    int saved_errno = errno;
    unsigned long queued = ERR_get_error();
    ERR_clear_error();
    long verify = SSL_get_verify_result(ssl);
    if (verify != X509_V_OK) {
        *error = (struct mo_network_error){
            .status = NETWORK_CERTIFICATE,
            .reason = save_reason(tls, X509_verify_cert_error_string(verify),
                                  "certificate verification failed")};
        return MO_TLS_FAILED;
    }
    if (queued != 0) {
        int lib = ERR_GET_LIB(queued);
        int reason = ERR_GET_REASON(queued);
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
        if (lib == ERR_LIB_SSL && reason == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
            *error = (struct mo_network_error){.status = NETWORK_CLOSED};
            return MO_TLS_FAILED;
        }
#endif
        if ((lib == ERR_LIB_SSL && reason == SSL_R_CERTIFICATE_VERIFY_FAILED) ||
            lib == ERR_LIB_X509) {
            *error = (struct mo_network_error){.status = NETWORK_CERTIFICATE,
                                               .reason =
                                                   save_reason(tls, ERR_reason_error_string(queued),
                                                               "certificate verification failed")};
            return MO_TLS_FAILED;
        }
    }
    if (kind == SSL_ERROR_SYSCALL && queued == 0) {
        if (saved_errno == 0 || saved_errno == ECONNRESET || saved_errno == EPIPE)
            *error = (struct mo_network_error){.status = NETWORK_CLOSED};
        else
            *error = (struct mo_network_error){.status = NETWORK_BROKEN, .number = saved_errno};
        return MO_TLS_FAILED;
    }
    *error = (struct mo_network_error){
        .status = NETWORK_BROKEN,
        .reason =
            save_reason(tls, queued != 0 ? ERR_reason_error_string(queued) : NULL, "TLS error")};
    return MO_TLS_FAILED;
}

struct mo_network_error mo_tls_open(struct mo_tls *tls, int fd, const uint8_t *host,
                                    size_t length) {
    MO_ASSERT(length != 0 && length <= MO_LIMIT_HOST_NAME);
    tls->ctx = NULL;
    tls->ssl = NULL;
    tls->reason[0] = '\0';
    ERR_clear_error();
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL) != 1)
        return (struct mo_network_error){.status = NETWORK_BROKEN,
                                         .reason =
                                             save_reason(tls, NULL, "TLS initialization failed")};
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        unsigned long queued = ERR_get_error();
        ERR_clear_error();
        return (struct mo_network_error){
            .status = NETWORK_BROKEN,
            .reason = save_reason(tls, queued != 0 ? ERR_reason_error_string(queued) : NULL,
                                  "TLS context allocation failed")};
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    (void)SSL_CTX_set_mode(ctx,
                           SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    bool paths_ok = SSL_CTX_set_default_verify_paths(ctx) == 1;
#ifdef __APPLE__
    if (getenv("SSL_CERT_FILE") == NULL && getenv("SSL_CERT_DIR") == NULL &&
        access("/etc/ssl/cert.pem", R_OK) == 0) {
        if (SSL_CTX_load_verify_locations(ctx, "/etc/ssl/cert.pem", NULL) == 1)
            paths_ok = true;
        if (paths_ok)
            ERR_clear_error();
    }
#endif
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 || !paths_ok) {
        unsigned long queued = ERR_get_error();
        ERR_clear_error();
        SSL_CTX_free(ctx);
        return (struct mo_network_error){
            .status = NETWORK_CERTIFICATE,
            .reason = save_reason(tls, queued != 0 ? ERR_reason_error_string(queued) : NULL,
                                  "could not load root certificates")};
    }
    SSL *ssl = SSL_new(ctx);
    if (ssl == NULL) {
        unsigned long queued = ERR_get_error();
        ERR_clear_error();
        SSL_CTX_free(ctx);
        return (struct mo_network_error){
            .status = NETWORK_BROKEN,
            .reason = save_reason(tls, queued != 0 ? ERR_reason_error_string(queued) : NULL,
                                  "TLS session allocation failed")};
    }
    char host_text[MO_LIMIT_HOST_NAME + 1];
    memcpy(host_text, host, length);
    host_text[length] = '\0';
    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    bool configured = true;
    if (X509_VERIFY_PARAM_set1_ip_asc(param, host_text) != 1) {
        ERR_clear_error();
        configured = X509_VERIFY_PARAM_set1_host(param, host_text, length) == 1 &&
                     SSL_set_tlsext_host_name(ssl, host_text) == 1;
    }
    if (!configured || SSL_set_fd(ssl, fd) != 1) {
        unsigned long queued = ERR_get_error();
        ERR_clear_error();
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return (struct mo_network_error){
            .status = NETWORK_BROKEN,
            .reason = save_reason(tls, queued != 0 ? ERR_reason_error_string(queued) : NULL,
                                  "TLS host setup failed")};
    }
    tls->ctx = ctx;
    tls->ssl = ssl;
    return (struct mo_network_error){.status = NETWORK_OK};
}

enum mo_tls_step mo_tls_handshake(struct mo_tls *tls, struct mo_network_error *error) {
    SSL *ssl = tls->ssl;
    MO_ASSERT(ssl != NULL);
    ERR_clear_error();
    errno = 0;
    int result = SSL_connect(ssl);
    if (result == 1) {
        *error = (struct mo_network_error){.status = NETWORK_OK};
        return MO_TLS_DONE;
    }
    return classify(tls, ssl, result, false, NULL, error);
}

enum mo_tls_step mo_tls_write(struct mo_tls *tls, const uint8_t *bytes, size_t length,
                              size_t *written, struct mo_network_error *error) {
    SSL *ssl = tls->ssl;
    MO_ASSERT(ssl != NULL && length != 0);
    ERR_clear_error();
    errno = 0;
    int result = SSL_write_ex(ssl, bytes, length, written);
    if (result == 1) {
        *error = (struct mo_network_error){.status = NETWORK_OK};
        return MO_TLS_DONE;
    }
    return classify(tls, ssl, result, false, NULL, error);
}

enum mo_tls_step mo_tls_read(struct mo_tls *tls, uint8_t *bytes, size_t most, size_t *count,
                             struct mo_network_error *error) {
    SSL *ssl = tls->ssl;
    MO_ASSERT(ssl != NULL && most != 0);
    for (;;) {
        ERR_clear_error();
        errno = 0;
        size_t got = 0;
        int result = SSL_read_ex(ssl, bytes, most, &got);
        if (result == 1) {
            if (got == 0)
                continue;
            *count = got;
            *error = (struct mo_network_error){.status = NETWORK_OK};
            return MO_TLS_DONE;
        }
        return classify(tls, ssl, result, true, count, error);
    }
}

void mo_tls_close(struct mo_tls *tls) {
    if (tls->ssl != NULL) {
        SSL *ssl = tls->ssl;
        (void)SSL_shutdown(ssl);
        ERR_clear_error();
        SSL_free(ssl);
        tls->ssl = NULL;
    }
    if (tls->ctx != NULL) {
        SSL_CTX_free(tls->ctx);
        tls->ctx = NULL;
    }
}
