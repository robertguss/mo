#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "tools/api-server.h"
#include "tools/tools.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void write_file_str(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    MO_ASSERT(f != NULL);
    size_t len = strlen(text);
    MO_ASSERT(fwrite(text, 1, len, f) == len && fclose(f) == 0);
}

static bool send_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(fd, data + off, len - off, flags);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

static int connect_loopback(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    MO_ASSERT(fd >= 0);
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    MO_ASSERT(connect(fd, (const struct sockaddr *)&addr, sizeof addr) == 0);
    return fd;
}

static int http_exchange(uint16_t port, const char *raw_req, char *resp_buf, size_t cap) {
    int fd = connect_loopback(port);
    MO_ASSERT(send_all(fd, raw_req, strlen(raw_req)));

    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, resp_buf + total, cap - 1 - total, 0);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        total += (size_t)n;
    }
    resp_buf[total] = '\0';
    (void)close(fd);

    int status = 0;
    if (strncmp(resp_buf, "HTTP/1.1 ", 9) == 0)
        status = atoi(resp_buf + 9);
    return status;
}

/* Reads exactly one framed HTTP/1.1 response from a keep-alive socket, keeping leftover bytes. */
static int read_one_http_response(int fd, bool is_head, char *carry, size_t carry_cap,
                                  size_t *carry_len, char *out, size_t out_cap) {
    size_t header_end = SIZE_MAX;
    for (;;) {
        for (size_t i = 0; i + 3 < *carry_len; i++) {
            if (carry[i] == '\r' && carry[i + 1] == '\n' && carry[i + 2] == '\r' &&
                carry[i + 3] == '\n') {
                header_end = i;
                break;
            }
        }
        if (header_end != SIZE_MAX)
            break;
        MO_ASSERT(*carry_len + 1 < carry_cap);
        ssize_t n = recv(fd, carry + *carry_len, carry_cap - 1 - *carry_len, 0);
        if (n <= 0)
            return -1;
        *carry_len += (size_t)n;
    }
    int status = 0;
    if (header_end >= 12 && strncmp(carry, "HTTP/1.1 ", 9) == 0)
        status = atoi(carry + 9);

    size_t content_len = 0;
    if (!is_head && status != 204 && status != 304 && !(status >= 100 && status < 200)) {
        carry[header_end + 2] = '\0';
        const char *cl = strstr(carry, "\r\nContent-Length: ");
        if (cl != NULL)
            content_len = (size_t)strtoull(cl + 18, NULL, 10);
        carry[header_end + 2] = '\r';
    }

    size_t full_len = header_end + 4 + content_len;
    while (*carry_len < full_len) {
        MO_ASSERT(*carry_len + 1 < carry_cap);
        ssize_t n = recv(fd, carry + *carry_len, carry_cap - 1 - *carry_len, 0);
        if (n <= 0)
            return -1;
        *carry_len += (size_t)n;
    }
    MO_ASSERT(full_len + 1 <= out_cap);
    memcpy(out, carry, full_len);
    out[full_len] = '\0';

    size_t rem = *carry_len - full_len;
    if (rem > 0)
        memmove(carry, carry + full_len, rem);
    *carry_len = rem;
    return status;
}

struct client_job {
    uint16_t port;
    int worker_id;
    bool ok;
};

static void *concurrent_client(void *arg) {
    struct client_job *job = arg;
    char req[512], resp[8192], body[128], expected_field[128];
    int blen = snprintf(body, sizeof body, "{\"name\":\"item-%d\"}", job->worker_id);
    int flen =
        snprintf(expected_field, sizeof expected_field, "\"name\":\"item-%d\"", job->worker_id);
    MO_ASSERT(blen > 0 && flen > 0);
    int rlen = snprintf(req, sizeof req,
                        "POST /items HTTP/1.1\r\n"
                        "Host: 127.0.0.1:%u\r\n"
                        "Authorization: Bearer secret-key\r\n"
                        "Cookie: sid=super-secret-cookie\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n%s",
                        (unsigned)job->port, blen, body);
    MO_ASSERT(rlen > 0 && (size_t)rlen < sizeof req);
    int status = http_exchange(job->port, req, resp, sizeof resp);
    if (status != 200 || strstr(resp, expected_field) == NULL) {
        job->ok = false;
        return NULL;
    }

    rlen = snprintf(req, sizeof req,
                    "GET /items?limit=20&secret_query=never-log-me HTTP/1.1\r\n"
                    "Host: 127.0.0.1:%u\r\n"
                    "Connection: close\r\n\r\n",
                    (unsigned)job->port);
    MO_ASSERT(rlen > 0);
    status = http_exchange(job->port, req, resp, sizeof resp);
    job->ok = (status == 200 && strstr(resp, "\"name\":\"item-") != NULL);
    return NULL;
}

struct crash_job {
    uint16_t port;
    int worker_id;
    bool ok;
};

static void *concurrent_crash_client(void *arg) {
    struct crash_job *job = arg;
    char req[256], resp[4096];
    const char *path = (job->worker_id % 2 == 0) ? "/actor-crash" : "/crash";
    int rlen = snprintf(req, sizeof req,
                        "GET %s HTTP/1.1\r\n"
                        "Host: 127.0.0.1:%u\r\n"
                        "Connection: close\r\n\r\n",
                        path, (unsigned)job->port);
    MO_ASSERT(rlen > 0 && (size_t)rlen < sizeof req);
    job->ok = (http_exchange(job->port, req, resp, sizeof resp) == 500);
    return NULL;
}

struct slowloris_peer {
    int fd;
    int status;
    char body[512];
};

static void *slowloris_writer(void *arg) {
    struct slowloris_peer *peer = arg;
    const char *partial = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Slow: ";
    (void)send_all(peer->fd, partial, strlen(partial));
    for (int i = 0; i < 12; i++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 25000000};
        (void)nanosleep(&ts, NULL);
        if (!send_all(peer->fd, "a", 1))
            break;
    }
    size_t total = 0;
    while (total + 1 < sizeof peer->body) {
        ssize_t n = recv(peer->fd, peer->body + total, sizeof peer->body - 1 - total, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        total += (size_t)n;
    }
    peer->body[total] = '\0';
    if (strncmp(peer->body, "HTTP/1.1 ", 9) == 0)
        peer->status = atoi(peer->body + 9);
    (void)close(peer->fd);
    return NULL;
}

static void test_monotonic_read_deadline(const char *app_path, const char *data_dir) {
    const char *argv[] = {"mo", "serve", app_path, "--port", "0", "--data", data_dir};
    struct mo_cli cli;
    const char *perr = NULL;
    MO_ASSERT(mo_cli_parse(7, argv, &cli, &perr));

    struct mo_image *image = NULL;
    uint32_t wire_fn = MO_NO_INDEX;
    uint16_t default_port = 0;
    uint32_t actor_services[MO_LIMIT_API_ACTORS] = {0};
    uint32_t actor_count = 0;
    MO_ASSERT(mo_api_compile_file(&cli, NULL, NULL, &image, &wire_fn, &default_port, actor_services,
                                  &actor_count) == 0 &&
              image != NULL);

    int folder_fd = open(data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(folder_fd >= 0);
    pthread_mutex_t log_mutex;
    MO_ASSERT(pthread_mutex_init(&log_mutex, NULL) == 0);
    struct mo_live_actor *actors[MO_LIMIT_API_ACTORS] = {NULL};
    for (uint32_t a = 0; a < actor_count; a++)
        MO_ASSERT(mo_live_actor_new(image, actor_services[a], NULL, &log_mutex, &actors[a]) ==
                  MO_SERVICE_OK);

    struct mo_limits req_limits = cli.limits;
    req_limits.heap_size = MO_LIMIT_API_REQUEST_HEAP;

    int fast_sv[2];
    MO_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fast_sv) == 0);
    const char *fast_req = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    MO_ASSERT(send_all(fast_sv[0], fast_req, strlen(fast_req)));
    (void)shutdown(fast_sv[0], SHUT_WR);
    mo_api_serve_fd(image, wire_fn, folder_fd, -1, -1, NULL, &log_mutex, &req_limits, actors,
                    actor_count, 120000, 1000000, fast_sv[1]);
    char fast_resp[512];
    ssize_t got = recv(fast_sv[0], fast_resp, sizeof fast_resp - 1, 0);
    MO_ASSERT(got > 0);
    fast_resp[got] = '\0';
    (void)close(fast_sv[0]);
    CHECK(strncmp(fast_resp, "HTTP/1.1 200 OK", 15) == 0);

    int slow_sv[2];
    MO_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, slow_sv) == 0);
    struct slowloris_peer peer = {.fd = slow_sv[0], .status = 0};
    pthread_t writer_tid;
    MO_ASSERT(pthread_create(&writer_tid, NULL, slowloris_writer, &peer) == 0);
    mo_api_serve_fd(image, wire_fn, folder_fd, -1, -1, NULL, &log_mutex, &req_limits, actors,
                    actor_count, 120000, 1000000, slow_sv[1]);
    MO_ASSERT(pthread_join(writer_tid, NULL) == 0);
    CHECK(peer.status == 408);
    CHECK(strstr(peer.body, "\"request_timeout\"") != NULL);

    for (uint32_t a = 0; a < actor_count; a++)
        mo_live_actor_free(actors[a]);
    (void)pthread_mutex_destroy(&log_mutex);
    (void)close(folder_fd);
    mo_image_free(image);
}

static void test_cli_serve_flags(const char *dir) {
    struct mo_cli cli;
    const char *err = NULL;

    const char *ok_argv[] = {"mo",      "serve",     "app.mo", "--bind",
                             "0.0.0.0", "--workers", "64",     "--watch"};
    CHECK(mo_cli_parse(8, ok_argv, &cli, &err));
    CHECK(cli.bind_host != NULL && strcmp(cli.bind_host, "0.0.0.0") == 0);
    CHECK(cli.has_workers && cli.workers == 64 && cli.watch);

    static const char *const bad_binds[] = {"localhost", "999.0.0.1", "127.0.0", "01.0.0.1", "::1"};
    for (size_t i = 0; i < sizeof bad_binds / sizeof bad_binds[0]; i++) {
        const char *argv[] = {"mo", "serve", "app.mo", "--bind", bad_binds[i]};
        CHECK(!mo_cli_parse(5, argv, &cli, &err));
        CHECK(err != NULL && strstr(err, "invalid bind address") != NULL);
    }

    static const char *const bad_workers[] = {"0", "65", "-1", "abc"};
    for (size_t i = 0; i < sizeof bad_workers / sizeof bad_workers[0]; i++) {
        const char *argv[] = {"mo", "serve", "app.mo", "--workers", bad_workers[i]};
        CHECK(!mo_cli_parse(5, argv, &cli, &err));
        CHECK(err != NULL && strstr(err, "invalid worker count") != NULL);
    }

    const char *no_file_watch[] = {"mo", "serve", "--port", "8080", "--data", "d", "--watch"};
    CHECK(!mo_cli_parse(7, no_file_watch, &cli, &err));
    CHECK(err != NULL && strstr(err, "apply only to api servers") != NULL);

    /* Non-API MCP service rejects --watch with status 2 */
    char svc_path[300];
    MO_ASSERT(snprintf(svc_path, sizeof svc_path, "%s/svc.mo", dir) > 0);
    write_file_str(svc_path, "module Svc\nservice\n  state Int = 0\n  tools read\n"
                             "  token from env \"SVC_TOK\"\nend\n"
                             "fn read(s: Int, _t: Time) : Int\ns\nend\n");
    const char *svc_argv[] = {"mo", "serve", svc_path, "--port", "0", "--data", dir, "--watch"};
    MO_ASSERT(mo_cli_parse(8, svc_argv, &cli, &err));
    FILE *ferr = tmpfile();
    MO_ASSERT(ferr != NULL);
    int st = 0;
    CHECK(mo_serve_maybe_api(&cli, ferr, ferr, &st) && st == 2);
    (void)fclose(ferr);
}

static uint16_t spawn_server(int argc, const char *const argv[], pid_t *pid_out, int *out_rfd,
                             int *err_rfd) {
    int out_pipe[2], err_pipe[2];
    MO_ASSERT(pipe(out_pipe) == 0 && pipe(err_pipe) == 0);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        (void)close(out_pipe[0]);
        (void)close(err_pipe[0]);
        FILE *fout = fdopen(out_pipe[1], "w");
        FILE *ferr = fdopen(err_pipe[1], "w");
        MO_ASSERT(fout != NULL && ferr != NULL);
        struct mo_cli cli;
        const char *perr = NULL;
        MO_ASSERT(mo_cli_parse(argc, argv, &cli, &perr));
        int rc = mo_cli_execute(&cli, fout, ferr);
        (void)fflush(fout);
        (void)fflush(ferr);
        (void)fclose(fout);
        (void)fclose(ferr);
        _Exit(rc);
    }
    (void)close(out_pipe[1]);
    (void)close(err_pipe[1]);

    char line[256];
    size_t pos = 0;
    while (pos + 1 < sizeof line) {
        ssize_t n = read(out_pipe[0], line + pos, 1);
        if (n <= 0)
            break;
        if (line[pos++] == '\n')
            break;
    }
    line[pos] = '\0';
    if (pos == 0) {
        char diag_buf[4096];
        ssize_t dn = read(err_pipe[0], diag_buf, sizeof diag_buf - 1);
        if (dn > 0) {
            diag_buf[dn] = '\0';
            (void)fprintf(stderr, "spawn_server stderr:\n%s\n", diag_buf);
        }
    }
    char host_buf[64] = "";
    unsigned port_num = 0;
    CHECK(sscanf(line, "mo serve: listening on http://%63[^:]:%u", host_buf, &port_num) == 2);
    CHECK(port_num > 0 && port_num <= 65535);
    *pid_out = pid;
    *out_rfd = out_pipe[0];
    *err_rfd = err_pipe[0];
    return (uint16_t)port_num;
}

static void stop_server(pid_t pid, int out_rfd, int err_rfd, char *err_buf, size_t err_cap) {
    MO_ASSERT(kill(pid, SIGTERM) == 0);
    size_t elen = 0;
    while (elen + 1 < err_cap) {
        ssize_t n = read(err_rfd, err_buf + elen, err_cap - 1 - elen);
        if (n <= 0)
            break;
        elen += (size_t)n;
    }
    err_buf[elen] = '\0';
    (void)close(out_rfd);
    (void)close(err_rfd);
    int wstatus = 0;
    MO_ASSERT(waitpid(pid, &wstatus, 0) == pid);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
}

static void run_static_checks_for_port(uint16_t port) {
    char resp[16384], req[1024];

    static const struct {
        const char *file;
        const char *mime;
        bool immutable;
    } cases[] = {
        {"app-a1b2c3d4.js", "text/javascript; charset=utf-8", true},
        {"mod.mjs", "text/javascript; charset=utf-8", false},
        {"style.9f8e7d6c.css", "text/css; charset=utf-8", true},
        {"index.html", "text/html; charset=utf-8", false},
        {"manifest.json", "application/json", false},
        {"app.js.map", "application/json", false},
        {"icon.svg", "image/svg+xml", false},
        {"logo.png", "image/png", false},
        {"photo.jpg", "image/jpeg", false},
        {"pic.jpeg", "image/jpeg", false},
        {"hero.webp", "image/webp", false},
        {"favicon.ico", "image/x-icon", false},
        {"font.woff", "font/woff", false},
        {"font.woff2", "font/woff2", false},
        {"site.webmanifest", "application/manifest+json", false},
        {"readme.txt", "text/plain; charset=utf-8", false},
        {"raw.bin", "application/octet-stream", false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        (void)snprintf(req, sizeof req,
                       "GET /assets/%s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                       cases[i].file);
        int st = http_exchange(port, req, resp, sizeof resp);
        CHECK(st == 200);
        char expected_ct[128];
        (void)snprintf(expected_ct, sizeof expected_ct, "Content-Type: %s\r\n", cases[i].mime);
        CHECK(strstr(resp, expected_ct) != NULL);
        CHECK(strstr(resp, "X-Content-Type-Options: nosniff\r\n") != NULL);
        if (cases[i].immutable)
            CHECK(strstr(resp, "Cache-Control: public, max-age=31536000, immutable\r\n") != NULL);
        else
            CHECK(strstr(resp, "Cache-Control: public, max-age=0, must-revalidate\r\n") != NULL);
    }

    /* ETag + If-None-Match -> 304 Not Modified, and HEAD -> 200 with empty body */
    CHECK(http_exchange(port,
                        "GET /assets/app-a1b2c3d4.js HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
                        "close\r\n\r\n",
                        resp, sizeof resp) == 200);
    const char *etag_hdr = strstr(resp, "ETag: ");
    MO_ASSERT(etag_hdr != NULL);
    const char *etag_end = strstr(etag_hdr + 6, "\r\n");
    MO_ASSERT(etag_end != NULL);
    char etag[96];
    size_t elen = (size_t)(etag_end - (etag_hdr + 6));
    MO_ASSERT(elen < sizeof etag);
    memcpy(etag, etag_hdr + 6, elen);
    etag[elen] = '\0';

    (void)snprintf(req, sizeof req,
                   "GET /assets/app-a1b2c3d4.js HTTP/1.1\r\nHost: 127.0.0.1\r\nIf-None-Match: "
                   "%s\r\nConnection: close\r\n\r\n",
                   etag);
    CHECK(http_exchange(port, req, resp, sizeof resp) == 304);
    const char *body_304 = strstr(resp, "\r\n\r\n");
    CHECK(body_304 != NULL && body_304[4] == '\0');

    CHECK(http_exchange(port,
                        "HEAD /assets/app-a1b2c3d4.js HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
                        "close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "Content-Length: 19\r\n") != NULL);
    const char *body_head = strstr(resp, "\r\n\r\n");
    CHECK(body_head != NULL && body_head[4] == '\0');

    /* Static mount takes precedence over dynamic route GET /assets/:slug */
    CHECK(http_exchange(
              port,
              "GET /assets/index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);
    CHECK(strstr(resp, "<h1>Hello</h1>") != NULL);

    /* Internal relative symlink within public/ succeeds */
    CHECK(http_exchange(
              port,
              "GET /assets/alias.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);
    CHECK(strstr(resp, "<h1>Hello</h1>") != NULL);

    /* POST on static mount returns 405 */
    CHECK(http_exchange(
              port,
              "POST /assets/index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 405);

    /* Traversal, backslash, NUL, directory, and symlink escape attempts all return 404 */
    static const char *const blocked[] = {
        "/assets",
        "/assets/",
        "/assets/sub",
        "/assets/sub/",
        "/assets/../secret.txt",
        "/assets/%2e%2e/secret.txt",
        "/assets/sub/..%2f..%2fsecret.txt",
        "/assets/a%5cb.txt",
        "/assets/a%00b.txt",
        "/assets/escape_rel.txt",
        "/assets/escape_abs.txt",
    };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
        (void)snprintf(req, sizeof req,
                       "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                       blocked[i]);
        CHECK(http_exchange(port, req, resp, sizeof resp) == 404);
        CHECK(strstr(resp, "TOP_SECRET_DATA") == NULL);
    }
}

static void test_watch_live_reload(const char *base_dir) {
    char wdir[300], main_mo[340], routes_mo[340];
    MO_ASSERT(snprintf(wdir, sizeof wdir, "%s/watch", base_dir) > 0 && mkdir(wdir, 0700) == 0);
    MO_ASSERT(snprintf(main_mo, sizeof main_mo, "%s/main.mo", wdir) > 0);
    MO_ASSERT(snprintf(routes_mo, sizeof routes_mo, "%s/routes.mo", wdir) > 0);

    write_file_str(routes_mo, "module Routes\nexpose version\nfn version() : Map(Text, Text)\n"
                              "  {\"v\": \"v1\"}\nend\n");
    write_file_str(main_mo, "module Main\nuse Routes\napi\n  scope \"/api\"\n"
                            "    get \"/version\" Routes.version\n  end\nend\n");

    const char *argv[] = {"mo",        "serve",     main_mo, "--port", "0",  "--bind",
                          "127.0.0.1", "--workers", "2",     "--data", wdir, "--watch"};
    pid_t pid = -1;
    int out_rfd = -1, err_rfd = -1;
    uint16_t port = spawn_server(12, argv, &pid, &out_rfd, &err_rfd);

    char resp[4096];
    CHECK(http_exchange(port,
                        "GET /api/version HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"v\":\"v1\"") != NULL);

    /* 1. Valid edit to imported module reloads into a fresh image without dropping socket */
    write_file_str(routes_mo, "module Routes\nexpose version\nfn version() : Map(Text, Text)\n"
                              "  {\"v\": \"v2-reloaded\"}\nend\n");
    bool saw_v2 = false;
    for (int attempt = 0; attempt < 25 && !saw_v2; attempt++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        (void)nanosleep(&ts, NULL);
        if (http_exchange(
                port, "GET /api/version HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                resp, sizeof resp) == 200 &&
            strstr(resp, "\"v\":\"v2-reloaded\"") != NULL) {
            saw_v2 = true;
        }
    }
    CHECK(saw_v2);

    /* 2. Compile error prints diagnostics and keeps serving last good image (v2-reloaded) */
    write_file_str(routes_mo, "module Routes\nexpose version\nfn version() : Int\n"
                              "  \"type-error\"\nend\n");
    struct timespec wait_bad = {.tv_sec = 0, .tv_nsec = 400000000};
    (void)nanosleep(&wait_bad, NULL);
    CHECK(http_exchange(port,
                        "GET /api/version HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"v\":\"v2-reloaded\"") != NULL);

    /* 3. Fixing the error reloads cleanly to v3 */
    write_file_str(routes_mo, "module Routes\nexpose version\nfn version() : Map(Text, Text)\n"
                              "  {\"v\": \"v3-fixed\"}\nend\n");
    bool saw_v3 = false;
    for (int attempt = 0; attempt < 25 && !saw_v3; attempt++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        (void)nanosleep(&ts, NULL);
        if (http_exchange(
                port, "GET /api/version HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                resp, sizeof resp) == 200 &&
            strstr(resp, "\"v\":\"v3-fixed\"") != NULL) {
            saw_v3 = true;
        }
    }
    CHECK(saw_v3);

    char err_log[32768];
    stop_server(pid, out_rfd, err_rfd, err_log, sizeof err_log);
    CHECK(strstr(err_log, "mo serve: reload:") != NULL);
    CHECK(strstr(err_log, "error E") != NULL);
}

static void test_watch_manifest_dep_reload(const char *base_dir) {
    char dep_dir[300], dep_src[340], dep_mo[380], app_dir[300], toml_path[340], main_mo[340];
    MO_ASSERT(snprintf(dep_dir, sizeof dep_dir, "%s/dep_pkg", base_dir) > 0 &&
              mkdir(dep_dir, 0700) == 0);
    MO_ASSERT(snprintf(dep_src, sizeof dep_src, "%s/src", dep_dir) > 0 &&
              mkdir(dep_src, 0700) == 0);
    MO_ASSERT(snprintf(dep_mo, sizeof dep_mo, "%s/greeter.mo", dep_src) > 0);
    MO_ASSERT(snprintf(app_dir, sizeof app_dir, "%s/watch_pkg", base_dir) > 0 &&
              mkdir(app_dir, 0700) == 0);
    MO_ASSERT(snprintf(toml_path, sizeof toml_path, "%s/mo.toml", app_dir) > 0);
    MO_ASSERT(snprintf(main_mo, sizeof main_mo, "%s/main.mo", app_dir) > 0);

    write_file_str(dep_mo, "module Greeter\nexpose greet\nfn greet() : Map(Text, Text)\n"
                           "  {\"msg\": \"dep-v1\"}\nend\n");
    write_file_str(toml_path, "[package]\nname = \"watch-pkg\"\n\n[dependencies]\n"
                              "greeter = { path = \"../dep_pkg\" }\n");
    write_file_str(main_mo, "module Main\nuse Greeter\napi\n  get \"/greet\" Greeter.greet\nend\n");

    const char *argv[] = {"mo",        "serve",     main_mo, "--port", "0",     "--bind",
                          "127.0.0.1", "--workers", "2",     "--data", app_dir, "--watch"};
    pid_t pid = -1;
    int out_rfd = -1, err_rfd = -1;
    uint16_t port = spawn_server(12, argv, &pid, &out_rfd, &err_rfd);

    char resp[4096];
    CHECK(http_exchange(port, "GET /greet HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"msg\":\"dep-v1\"") != NULL);

    /* Editing a .mo file inside a mo.toml local path dependency triggers live reload */
    write_file_str(dep_mo, "module Greeter\nexpose greet\nfn greet() : Map(Text, Text)\n"
                           "  {\"msg\": \"dep-v2\"}\nend\n");
    bool saw_v2 = false;
    for (int attempt = 0; attempt < 25 && !saw_v2; attempt++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        (void)nanosleep(&ts, NULL);
        if (http_exchange(port,
                          "GET /greet HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                          resp, sizeof resp) == 200 &&
            strstr(resp, "\"msg\":\"dep-v2\"") != NULL) {
            saw_v2 = true;
        }
    }
    CHECK(saw_v2);

    char err_log[32768];
    stop_server(pid, out_rfd, err_rfd, err_log, sizeof err_log);
    CHECK(strstr(err_log, "mo serve: reload:") != NULL);
}

static void test_background_jobs(const char *base_dir) {
    char japp_dir[300], jdata_dir[300], jobs_mod[340], main_mo[340];
    MO_ASSERT(snprintf(japp_dir, sizeof japp_dir, "%s/jobs_app", base_dir) > 0 &&
              mkdir(japp_dir, 0700) == 0);
    MO_ASSERT(snprintf(jdata_dir, sizeof jdata_dir, "%s/jobs_data", base_dir) > 0 &&
              mkdir(jdata_dir, 0700) == 0);
    MO_ASSERT(snprintf(jobs_mod, sizeof jobs_mod, "%s/jobs-mod.mo", japp_dir) > 0);
    MO_ASSERT(snprintf(main_mo, sizeof main_mo, "%s/main.mo", japp_dir) > 0);

    write_file_str(jobs_mod, "module JobsMod\n"
                             "expose heartbeat\n"
                             "use Json\n"
                             "fn heartbeat(_payload: Json, db: Sqlite.Database) : Unit\n"
                             "  _i = db.run(\"INSERT INTO ticks(n) VALUES (1)\", [])\n"
                             "  Unit\n"
                             "end\n");

    write_file_str(
        main_mo,
        "module Main\n"
        "use Json\n"
        "use Arvo.Jobs\n"
        "use JobsMod\n"
        "struct EnqInput\n"
        "  mode: Text\n"
        "  tag: Text\n"
        "  max_attempts: Int\n"
        "end\n"
        "struct ImportedRow\n"
        "  tag: Text\n"
        "end\n"
        "struct Summary\n"
        "  ticks: Int\n"
        "  imported: List(Text)\n"
        "  jobs: List(Jobs.JobRecord)\n"
        "end\n"
        "enum AppErr\n"
        "  SqlErr(Text)\n"
        "end\n"
        "enum JobFail\n"
        "  UpstreamDown(Text)\n"
        "end\n"
        "api\n"
        "  sqlite db from \"jobs.db\"\n"
        "  job \"heartbeat\" JobsMod.heartbeat every 1.second\n"
        "  job \"import.batch\" run_import\n"
        "  post \"/enqueue\" enqueue_job\n"
        "  get \"/summary\" get_summary\n"
        "  status SqlErr(_) = 500\n"
        "end\n"
        "fn enqueue_in_tx(\n"
        "  tx_db: Sqlite.Database,\n"
        "  ctx: (SecureRandom, Clock, EnqInput)\n"
        ") : Result(Text, Text)\n"
        "  _d = try tx_db.run(\"INSERT INTO imported(tag) VALUES (?)\", "
        "[Sqlite.String(\"enq-#{ctx.2.tag}\")]).map_error(fn(e) \"#{e}\" "
        "end)\n"
        "  payload = Json.Object({\n"
        "    \"mode\": Json.String(ctx.2.mode),\n"
        "    \"tag\": Json.String(ctx.2.tag),\n"
        "    \"secret\": Json.String(\"top-secret-job-payload\")\n"
        "  })\n"
        "  Jobs.enqueue_at(\n"
        "    tx_db,\n"
        "    ctx.0,\n"
        "    ctx.1,\n"
        "    \"default\",\n"
        "    \"import.batch\",\n"
        "    payload,\n"
        "    ctx.1.now,\n"
        "    ctx.2.max_attempts\n"
        "  ).map_error(fn(e) \"#{e}\" end)\n"
        "end\n"
        "fn enqueue_job(\n"
        "  db: Sqlite.Database,\n"
        "  rng: SecureRandom,\n"
        "  clock: Clock,\n"
        "  input: EnqInput\n"
        ") : Result(Map(Text, Text), AppErr)\n"
        "  jid = try Sqlite.transaction(db, (rng, clock, input), enqueue_in_tx, fn(e) \"#{e}\" "
        "end).map_error(fn(m) SqlErr(m) end)\n"
        "  Ok({\"job_id\": jid})\n"
        "end\n"
        "fn run_import(payload: Json, db: Sqlite.Database) : Result(Unit, JobFail)\n"
        "  mode = payload.text_at(\"mode\") or \"ok\"\n"
        "  tag = payload.text_at(\"tag\") or \"none\"\n"
        "  return Error(UpstreamDown(\"502 gateway\")) if mode == \"fail\"\n"
        "  if mode == \"crash\"\n"
        "    _b = db.run(\"BEGIN IMMEDIATE\", [])\n"
        "    _p = db.run(\"INSERT INTO imported(tag) VALUES ('phantom-job-tx')\", [])\n"
        "    stop \"job worker crashed\"\n"
        "  end\n"
        "  _ins = db.run(\"INSERT INTO imported(tag) VALUES (?)\", [Sqlite.String(tag)])\n"
        "  Ok(Unit)\n"
        "end\n"
        "fn get_summary(db: Sqlite.Database) : Result(Summary, AppErr)\n"
        "  tick_rows = try db.query(\"SELECT COUNT(*) FROM ticks\", []).map_error(fn(e) "
        "SqlErr(\"#{e}\") end)\n"
        "  ticks = case tick_rows.rows.first\n"
        "    Some([Sqlite.Integer(n)]): n\n"
        "    Some(_): 0\n"
        "    None: 0\n"
        "  end\n"
        "  imp_rows = try db.query(\"SELECT tag FROM imported ORDER BY tag ASC\", "
        "[]).map_error(fn(e) SqlErr(\"#{e}\") end)\n"
        "  imp = try Sqlite.decode[ImportedRow](imp_rows).map_error(fn(e) SqlErr(e.to_text) end)\n"
        "  jobs = try Jobs.list_recent(db, 20).map_error(fn(e) SqlErr(\"#{e}\") end)\n"
        "  Ok(Summary(ticks: ticks, imported: imp.map(fn(r) r.tag end), jobs: jobs))\n"
        "end\n"
        "fn main(sqlite: Sqlite)\n"
        "  db = case sqlite.open(\"jobs.db\")\n"
        "    Ok(d): d\n"
        "    Error(_): stop \"open jobs.db\"\n"
        "  end\n"
        "  assert Jobs.init_schema(db) == Ok(Unit)\n"
        "  assert db.run(\"CREATE TABLE IF NOT EXISTS ticks(n INT NOT NULL) STRICT\", []) == "
        "Ok(Unit)\n"
        "  assert db.run(\"CREATE TABLE IF NOT EXISTS imported(tag TEXT NOT NULL) STRICT\", []) == "
        "Ok(Unit)\n"
        "  stale = \"2020-01-01T00:00:00.000000Z\"\n"
        "  assert db.run(\n"
        "    \"INSERT INTO arvo_jobs (id, queue, job_name, payload_json, state, attempts, "
        "max_attempts, run_at, locked_at, last_error, created_at, updated_at) VALUES "
        "('000000000000000000000STALE', 'default', 'import.batch', "
        "'{\\\"mode\\\":\\\"ok\\\",\\\"tag\\\":\\\"recovered-stale\\\",\\\"secret\\\":\\\"top-"
        "secret-job-payload\\\"}', 'running', 1, 3, ?, ?, NULL, ?, ?)\",\n"
        "    [Sqlite.String(stale), Sqlite.String(stale), Sqlite.String(stale), "
        "Sqlite.String(stale)]\n"
        "  ) == Ok(Unit)\n"
        "end\n");

    /* Seed schema + orphaned 'running' job before spawning mo serve */
    {
        const char *seed_argv[] = {"mo", "run", "--state-dir", jdata_dir, main_mo};
        struct mo_cli seed_cli;
        const char *serr = NULL;
        MO_ASSERT(mo_cli_parse(5, seed_argv, &seed_cli, &serr));
        FILE *fnull = tmpfile();
        MO_ASSERT(fnull != NULL);
        CHECK(mo_cli_execute(&seed_cli, fnull, fnull) == 0);
        (void)fclose(fnull);
    }

    /* Verify mo format accepts job clauses cleanly */
    {
        const char *fmt_argv[] = {"mo", "format", main_mo, jobs_mod};
        struct mo_cli fmt_cli;
        const char *ferr = NULL;
        MO_ASSERT(mo_cli_parse(4, fmt_argv, &fmt_cli, &ferr));
        FILE *fnull = tmpfile();
        MO_ASSERT(fnull != NULL);
        CHECK(mo_cli_execute(&fmt_cli, fnull, fnull) == 0);
        (void)fclose(fnull);
    }

    const char *argv[] = {"mo",        "serve",     main_mo, "--port", "0",      "--bind",
                          "127.0.0.1", "--workers", "2",     "--data", jdata_dir};
    pid_t pid = -1;
    int out_rfd = -1, err_rfd = -1;
    uint16_t port = spawn_server(11, argv, &pid, &out_rfd, &err_rfd);

    char resp[32768];
    const char *enq_ok = "{\"mode\":\"ok\",\"tag\":\"done-one\",\"max_attempts\":3}";
    const char *enq_fail = "{\"mode\":\"fail\",\"tag\":\"retry-me\",\"max_attempts\":3}";
    const char *enq_crash = "{\"mode\":\"crash\",\"tag\":\"boom-one\",\"max_attempts\":1}";
    char req[1024];

    (void)snprintf(req, sizeof req,
                   "POST /enqueue HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
                   "application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                   strlen(enq_ok), enq_ok);
    CHECK(http_exchange(port, req, resp, sizeof resp) == 200);

    (void)snprintf(req, sizeof req,
                   "POST /enqueue HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
                   "application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                   strlen(enq_fail), enq_fail);
    CHECK(http_exchange(port, req, resp, sizeof resp) == 200);

    (void)snprintf(req, sizeof req,
                   "POST /enqueue HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
                   "application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                   strlen(enq_crash), enq_crash);
    CHECK(http_exchange(port, req, resp, sizeof resp) == 200);

    /* Wait up to 6 seconds for periodic heartbeat (>= 1s) + all 4 queued jobs to settle */
    bool settled = false;
    for (int attempt = 0; attempt < 60 && !settled; attempt++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        (void)nanosleep(&ts, NULL);
        if (http_exchange(port,
                          "GET /summary HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                          resp, sizeof resp) == 200) {
            bool has_tick =
                strstr(resp, "\"ticks\":0") == NULL && strstr(resp, "\"ticks\":") != NULL;
            bool has_stale = strstr(resp, "\"recovered-stale\"") != NULL;
            bool has_done = strstr(resp, "\"done-one\"") != NULL;
            bool has_retry_err = strstr(resp, "502 gateway") != NULL;
            bool has_crash_failed = strstr(resp, "\"state\":\"failed\"") != NULL &&
                                    strstr(resp, "\"last_error\":\"defect: stop\"") != NULL;
            if (has_tick && has_stale && has_done && has_retry_err && has_crash_failed)
                settled = true;
        }
    }
    CHECK(settled);
    /* Mid-transaction defect inside crashing job rolled back 'phantom-job-tx' cleanly */
    CHECK(strstr(resp, "phantom-job-tx") == NULL);

    char err_log[65536];
    stop_server(pid, out_rfd, err_rfd, err_log, sizeof err_log);
    CHECK(strstr(err_log, "mo serve: job: 20") != NULL);
    CHECK(strstr(err_log, " heartbeat ok ") != NULL);
    CHECK(strstr(err_log, " import.batch ok ") != NULL);
    CHECK(strstr(err_log, " import.batch error ") != NULL);
    CHECK(strstr(err_log, " import.batch defect ") != NULL);
    /* Job payload secrets never appear in server stderr logs */
    CHECK(strstr(err_log, "top-secret-job-payload") == NULL);
}

int main(void) {
    (void)signal(SIGPIPE, SIG_IGN);
    char dir[] = "/tmp/mo-test-api-srv-XXXXXX";
    MO_ASSERT(mkdtemp(dir) != NULL);

    test_cli_serve_flags(dir);

    char data_dir[256], pub_dir[300], sub_dir[340], app_path[300], auth_mod_path[300];
    MO_ASSERT(snprintf(data_dir, sizeof data_dir, "%s/data", dir) > 0 &&
              mkdir(data_dir, 0700) == 0);
    MO_ASSERT(snprintf(pub_dir, sizeof pub_dir, "%s/public", data_dir) > 0 &&
              mkdir(pub_dir, 0700) == 0);
    MO_ASSERT(snprintf(sub_dir, sizeof sub_dir, "%s/sub", pub_dir) > 0 &&
              mkdir(sub_dir, 0700) == 0);
    MO_ASSERT(snprintf(app_path, sizeof app_path, "%s/main.mo", dir) > 0);
    MO_ASSERT(snprintf(auth_mod_path, sizeof auth_mod_path, "%s/auth-mod.mo", dir) > 0);

    /* Populate static files + symlink confinement fixtures */
    char pbuf[380];
    (void)snprintf(pbuf, sizeof pbuf, "%s/secret.txt", data_dir);
    write_file_str(pbuf, "TOP_SECRET_DATA");
    (void)snprintf(pbuf, sizeof pbuf, "%s/app-a1b2c3d4.js", pub_dir);
    write_file_str(pbuf, "console.log('ok');\n");
    (void)snprintf(pbuf, sizeof pbuf, "%s/mod.mjs", pub_dir);
    write_file_str(pbuf, "export const x = 1;");
    (void)snprintf(pbuf, sizeof pbuf, "%s/style.9f8e7d6c.css", pub_dir);
    write_file_str(pbuf, "body{margin:0}");
    (void)snprintf(pbuf, sizeof pbuf, "%s/index.html", pub_dir);
    write_file_str(pbuf, "<h1>Hello</h1>");
    (void)snprintf(pbuf, sizeof pbuf, "%s/manifest.json", pub_dir);
    write_file_str(pbuf, "{}");
    (void)snprintf(pbuf, sizeof pbuf, "%s/app.js.map", pub_dir);
    write_file_str(pbuf, "{}");
    (void)snprintf(pbuf, sizeof pbuf, "%s/icon.svg", pub_dir);
    write_file_str(pbuf, "<svg></svg>");
    (void)snprintf(pbuf, sizeof pbuf, "%s/logo.png", pub_dir);
    write_file_str(pbuf, "png");
    (void)snprintf(pbuf, sizeof pbuf, "%s/photo.jpg", pub_dir);
    write_file_str(pbuf, "jpg");
    (void)snprintf(pbuf, sizeof pbuf, "%s/pic.jpeg", pub_dir);
    write_file_str(pbuf, "jpeg");
    (void)snprintf(pbuf, sizeof pbuf, "%s/hero.webp", pub_dir);
    write_file_str(pbuf, "webp");
    (void)snprintf(pbuf, sizeof pbuf, "%s/favicon.ico", pub_dir);
    write_file_str(pbuf, "ico");
    (void)snprintf(pbuf, sizeof pbuf, "%s/font.woff", pub_dir);
    write_file_str(pbuf, "woff");
    (void)snprintf(pbuf, sizeof pbuf, "%s/font.woff2", pub_dir);
    write_file_str(pbuf, "woff2");
    (void)snprintf(pbuf, sizeof pbuf, "%s/site.webmanifest", pub_dir);
    write_file_str(pbuf, "{}");
    (void)snprintf(pbuf, sizeof pbuf, "%s/readme.txt", pub_dir);
    write_file_str(pbuf, "hi");
    (void)snprintf(pbuf, sizeof pbuf, "%s/raw.bin", pub_dir);
    write_file_str(pbuf, "bin");
    (void)snprintf(pbuf, sizeof pbuf, "%s/nested.txt", sub_dir);
    write_file_str(pbuf, "nested");
    (void)snprintf(pbuf, sizeof pbuf, "%s/alias.html", pub_dir);
    MO_ASSERT(symlink("index.html", pbuf) == 0);
    (void)snprintf(pbuf, sizeof pbuf, "%s/escape_rel.txt", pub_dir);
    MO_ASSERT(symlink("../secret.txt", pbuf) == 0);
    (void)snprintf(pbuf, sizeof pbuf, "%s/escape_abs.txt", pub_dir);
    MO_ASSERT(symlink("/etc/hosts", pbuf) == 0);

    write_file_str(auth_mod_path,
                   "module AuthMod\n"
                   "expose Principal(..), AppError(..), authenticate\n"
                   "use Api\n"
                   "struct Principal\n  login: Text\nend\n"
                   "enum AppError\n"
                   "  Unauthorized\n  ItemMissing(Int)\n  SqlProblem(Text)\n  RateLimited\n"
                   "end\n"
                   "fn authenticate(req: Api.Request, env: Env) : Result(Principal, AppError)\n"
                   "  expected = env.get(\"API_TOKEN\") or \"secret-key\"\n"
                   "  raw = req.header(\"Authorization\") or \"\"\n"
                   "  tok = raw.drop_prefix(\"Bearer \") or \"\"\n"
                   "  return Error(Unauthorized) if tok != expected\n"
                   "  Ok(Principal(login: \"ada\"))\n"
                   "end\n");

    write_file_str(
        app_path,
        "module Main\n"
        "use Api\n"
        "use AuthMod\n"
        "struct NewItem\n  name: Text\nend\n"
        "struct Item\n  id: Int\n  name: Text\n  owner: Text\nend\n"
        "actor RateLimiter\n"
        "  state Map(Text, Int) = {}\n"
        "  calls allow_hit, crash_actor, hit_count\n"
        "end\n"
        "fn allow_hit(s: Map(Text, Int), _t: Time, key: Text) : (Map(Text, Int), Bool)\n"
        "  n = s.get(key) or 0\n"
        "  if n >= 2: (s, false) else: (s.set(key, n + 1), true)\n"
        "end\n"
        "fn crash_actor(_s: Map(Text, Int), _t: Time) : (Map(Text, Int), Int)\n"
        "  stop \"actor boom\"\n"
        "end\n"
        "fn hit_count(s: Map(Text, Int), _t: Time, key: Text) : Int\n"
        "  s.get(key) or 0\n"
        "end\n"
        "api\n"
        "  port 8080\n"
        "  cors \"*\"\n"
        "  sqlite db from \"items.db\"\n"
        "  actor limiter: RateLimiter\n"
        "  static \"/assets\" from \"public\"\n"
        "  auth AuthMod.Principal via AuthMod.authenticate\n"
        "  get \"/health\" health\n"
        "  get \"/whoami\" whoami\n"
        "  get \"/cookies\" set_two_cookies\n"
        "  get \"/redir-other\" redir_other\n"
        "  get \"/redir-307\" redir_307\n"
        "  get \"/assets/:slug\" dynamic_asset_fallback\n"
        "  scope \"/items\"\n"
        "    post \"\" create_item\n"
        "    get \"\" list_items\n"
        "    get \"/:id\" get_item\n"
        "  end\n"
        "  post \"/limited/:key\" limited_hit\n"
        "  get \"/actor-crash\" route_actor_crash\n"
        "  get \"/crash\" crash_route\n"
        "  post \"/tx-defect\" tx_defect_route\n"
        "  status AuthMod.Unauthorized = 401\n"
        "  status AuthMod.ItemMissing(_) = 404\n"
        "  status AuthMod.SqlProblem(_) = 500\n"
        "  status AuthMod.RateLimited = 429\n"
        "end\n"
        "fn sql_ok(r: Result(T, SqliteError)) : Result(T, AuthMod.AppError)\n"
        "  r.map_error(fn(e) AuthMod.SqlProblem(\"#{e}\") end)\n"
        "end\n"
        "fn ensure_table(db: Sqlite.Database) : Result(Unit, AuthMod.AppError)\n"
        "  sql_ok(db.run(\"CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT NOT NULL, owner TEXT NOT NULL)\", []))\n"
        "end\n"
        "fn health() : Map(Text, Text)\n  {\"status\": \"ok\"}\nend\n"
        "fn whoami(viewer: Option(AuthMod.Principal)) : Map(Text, Text)\n"
        "  case viewer\n"
        "    Some(u): {\"viewer\": u.login}\n"
        "    None: {\"viewer\": \"guest\"}\n"
        "  end\n"
        "end\n"
        "fn set_two_cookies(req: Api.Request) : Api.Response\n"
        "  tags = req.headers_named(\"X-Tag\").join(\",\")\n"
        "  r0 = Api.json_response(200, {\"tags\": tags})\n"
        "  r1 = r0.add_header(\"Set-Cookie\", \"sid=one; Path=/; HttpOnly\")\n"
        "  r1.add_header(\"Set-Cookie\", \"xsrf=two; Path=/\")\n"
        "end\n"
        "fn redir_other() : Api.Response\n  Api.redirect_see_other(\"/items\")\nend\n"
        "fn redir_307() : Api.Response\n  Api.redirect_with(307, \"/health\")\nend\n"
        "fn dynamic_asset_fallback(slug: Text) : Map(Text, Text)\n  {\"dyn\": slug}\nend\n"
        "fn create_item(u: AuthMod.Principal, db: Sqlite.Database, input: NewItem) : Result(Item, "
        "AuthMod.AppError)\n"
        "  _t = try ensure_table(db)\n"
        "  ins = try sql_ok(db.execute(\"INSERT INTO items(name, owner) VALUES (?, ?)\", "
        "[Sqlite.String(input.name), Sqlite.String(u.login)]))\n"
        "  Ok(Item(id: ins.last_insert_rowid, name: input.name, owner: u.login))\n"
        "end\n"
        "fn get_item(db: Sqlite.Database, id: Int) : Result(Item, AuthMod.AppError)\n"
        "  _t = try ensure_table(db)\n"
        "  rows = try sql_ok(db.query(\"SELECT id, name, owner FROM items WHERE id = ?\", "
        "[Sqlite.Integer(id)]))\n"
        "  opt = try Sqlite.decode_optional[Item](rows).map_error(fn(e) "
        "AuthMod.SqlProblem(e.to_text) end)\n"
        "  case opt\n"
        "    Some(item): Ok(item)\n"
        "    None: Error(AuthMod.ItemMissing(id))\n"
        "  end\n"
        "end\n"
        "fn list_items(db: Sqlite.Database, limit: Option(Int)) : Result(List(Item), "
        "AuthMod.AppError)\n"
        "  _t = try ensure_table(db)\n"
        "  rows = try sql_ok(db.query(\"SELECT id, name, owner FROM items ORDER BY id ASC LIMIT "
        "?\", [Sqlite.Integer(limit or 50)]))\n"
        "  Sqlite.decode[Item](rows).map_error(fn(e) AuthMod.SqlProblem(e.to_text) end)\n"
        "end\n"
        "fn limited_hit(limiter: RateLimiter, key: Text) : Result(Int, AuthMod.AppError)\n"
        "  return Error(AuthMod.RateLimited) if not limiter.allow_hit(key)\n"
        "  Ok(limiter.hit_count(key))\n"
        "end\n"
        "fn route_actor_crash(limiter: RateLimiter) : Int\n"
        "  limiter.crash_actor\n"
        "end\n"
        "fn crash_route() : Int\n"
        "  stop \"handler crashed\"\n"
        "end\n"
        "fn tx_defect_route(db: Sqlite.Database) : Result(Int, AuthMod.AppError)\n"
        "  _t = try ensure_table(db)\n"
        "  _b = try sql_ok(db.run(\"BEGIN IMMEDIATE\", []))\n"
        "  _x = try sql_ok(db.execute(\"INSERT INTO items(name, owner) VALUES ('phantom-tx', "
        "'nobody')\", []))\n"
        "  stop \"mid-transaction defect\"\n"
        "end\n");

    /* Verify mo format round-trip preserves static, scope, and qualified handlers */
    {
        const char *fmt_argv[] = {"mo", "format", app_path, auth_mod_path};
        struct mo_cli fmt_cli;
        const char *ferr_msg = NULL;
        MO_ASSERT(mo_cli_parse(4, fmt_argv, &fmt_cli, &ferr_msg));
        FILE *fnull = tmpfile();
        MO_ASSERT(fnull != NULL);
        CHECK(mo_cli_execute(&fmt_cli, fnull, fnull) == 0);
        (void)fclose(fnull);
    }

    const char *srv_argv[] = {"mo",        "serve",     app_path, "--port", "0",     "--bind",
                              "127.0.0.1", "--workers", "4",      "--data", data_dir};
    pid_t pid = -1;
    int out_rfd = -1, err_rfd = -1;
    uint16_t port = spawn_server(11, srv_argv, &pid, &out_rfd, &err_rfd);

    /* 1. Parallel concurrent SQLite writes and reads across 8 threads */
    enum { NCLIENTS = 8 };
    pthread_t tids[NCLIENTS];
    struct client_job jobs[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++) {
        jobs[i] = (struct client_job){.port = port, .worker_id = i + 1, .ok = false};
        MO_ASSERT(pthread_create(&tids[i], NULL, concurrent_client, &jobs[i]) == 0);
    }
    for (int i = 0; i < NCLIENTS; i++) {
        MO_ASSERT(pthread_join(tids[i], NULL) == 0);
        CHECK(jobs[i].ok);
    }

    char resp[16384];
    /* 2. Static assets + security confinement (Linux openat2 path) */
    run_static_checks_for_port(port);

    /* 3. Optional auth (Option(Principal)), multiple Set-Cookie, headers_named, 303/307 redirects
     */
    CHECK(http_exchange(port,
                        "GET /whoami HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"viewer\":\"guest\"") != NULL);
    CHECK(http_exchange(port,
                        "GET /whoami HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "wrong\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"viewer\":\"guest\"") != NULL);
    CHECK(http_exchange(port,
                        "GET /whoami HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"viewer\":\"ada\"") != NULL);

    CHECK(
        http_exchange(port,
                      "GET /cookies?secret_q=never-log-me HTTP/1.1\r\nHost: "
                      "127.0.0.1\r\nCookie: session=super-secret-cookie\r\nX-Tag: alpha\r\nx-tag: "
                      "beta\r\nConnection: close\r\n\r\n",
                      resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"tags\":\"alpha,beta\"") != NULL);
    CHECK(strstr(resp, "Set-Cookie: sid=one; Path=/; HttpOnly\r\n") != NULL);
    CHECK(strstr(resp, "Set-Cookie: xsrf=two; Path=/\r\n") != NULL);

    CHECK(http_exchange(port,
                        "GET /redir-other HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 303);
    CHECK(strstr(resp, "HTTP/1.1 303 See Other\r\n") != NULL);
    CHECK(strstr(resp, "Location: /items\r\n") != NULL);

    CHECK(http_exchange(port,
                        "GET /redir-307 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 307);
    CHECK(strstr(resp, "HTTP/1.1 307 Temporary Redirect\r\n") != NULL);
    CHECK(strstr(resp, "Location: /health\r\n") != NULL);

    /* 4. HTTP/1.1 keep-alive + pipelined requests on a single TCP socket */
    {
        int kfd = connect_loopback(port);
        char carry[16384], kresp[8192];
        size_t carry_len = 0;

        const char *r1 = "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                         "secret-key\r\nContent-Type: application/json\r\nContent-Length: "
                         "17\r\n\r\n{\"name\":\"ka-one\"}";
        MO_ASSERT(send_all(kfd, r1, strlen(r1)));
        CHECK(read_one_http_response(kfd, false, carry, sizeof carry, &carry_len, kresp,
                                     sizeof kresp) == 200);
        CHECK(strstr(kresp, "Connection: keep-alive\r\n") != NULL);
        CHECK(strstr(kresp, "\"name\":\"ka-one\"") != NULL);

        const char *r2 = "GET /assets/index.html HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        MO_ASSERT(send_all(kfd, r2, strlen(r2)));
        CHECK(read_one_http_response(kfd, false, carry, sizeof carry, &carry_len, kresp,
                                     sizeof kresp) == 200);
        CHECK(strstr(kresp, "Connection: keep-alive\r\n") != NULL);
        CHECK(strstr(kresp, "<h1>Hello</h1>") != NULL);

        /* Pipelined POST + final GET with Connection: close sent in one write */
        const char *pipelined =
            "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
            "secret-key\r\nContent-Type: application/json\r\nContent-Length: "
            "17\r\n\r\n{\"name\":\"ka-two\"}"
            "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        MO_ASSERT(send_all(kfd, pipelined, strlen(pipelined)));
        CHECK(read_one_http_response(kfd, false, carry, sizeof carry, &carry_len, kresp,
                                     sizeof kresp) == 200);
        CHECK(strstr(kresp, "\"name\":\"ka-two\"") != NULL);
        CHECK(read_one_http_response(kfd, false, carry, sizeof carry, &carry_len, kresp,
                                     sizeof kresp) == 200);
        CHECK(strstr(kresp, "Connection: close\r\n") != NULL);
        char eof_ch = 0;
        CHECK(recv(kfd, &eof_ch, 1, 0) == 0);
        (void)close(kfd);
    }

    /* 5. CORS preflight, OpenAPI 3.1, /docs, 401 auth, 422 strict JSON, 404 status mapping */
    CHECK(http_exchange(port,
                        "OPTIONS /items HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 204);
    CHECK(strstr(resp, "Access-Control-Allow-Origin: *") != NULL);

    CHECK(http_exchange(
              port, "GET /openapi.json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"openapi\":\"3.1.0\"") != NULL);
    CHECK(strstr(resp, "\"/items/{id}\"") != NULL);

    CHECK(http_exchange(port, "GET /docs HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "/openapi.json") != NULL);

    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: "
                        "12\r\nConnection: close\r\n\r\n{\"name\":\"x\"}",
                        resp, sizeof resp) == 401);

    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nContent-Length: 22\r\nConnection: "
                        "close\r\n\r\n{\"name\":\"x\",\"extra\":1}",
                        resp, sizeof resp) == 422);

    CHECK(http_exchange(port,
                        "GET /items/999 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 404);

    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                        "12\r\n{\"name\":\"chunked\"}\r\n0\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"name\":\"chunked\"") != NULL);

    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                        "12\r\n{\"name\":\"badtail\"}\r\n0\r\nXX\r\n\r\n",
                        resp, sizeof resp) == 400);
    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
                        "chunked\r\nConnection: close\r\n\r\n"
                        "12\r\n{\"name\":\"dup-te!\"}\r\n0\r\n\r\n",
                        resp, sizeof resp) == 400);
    CHECK(http_exchange(port,
                        "GET /items\x01 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 400);

    /* 6. Live in-process actor state + fault isolation across requests */
    CHECK(http_exchange(
              port, "POST /limited/ada HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);
    CHECK(http_exchange(
              port, "POST /limited/ada HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);
    CHECK(http_exchange(
              port, "POST /limited/ada HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 429);
    CHECK(http_exchange(port,
                        "GET /actor-crash HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 500);
    CHECK(http_exchange(
              port, "POST /limited/ada HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 429);
    CHECK(http_exchange(
              port, "POST /limited/grace HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);

    CHECK(http_exchange(port, "GET /crash HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 500);
    CHECK(strstr(resp, "{\"error\":\"internal_server_error\"}") != NULL);

    CHECK(http_exchange(port,
                        "GET /items/1 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);

    pthread_t ctids[4];
    struct crash_job cjobs[4];
    for (int i = 0; i < 4; i++) {
        cjobs[i] = (struct crash_job){.port = port, .worker_id = i, .ok = false};
        MO_ASSERT(pthread_create(&ctids[i], NULL, concurrent_crash_client, &cjobs[i]) == 0);
    }
    for (int i = 0; i < 4; i++) {
        MO_ASSERT(pthread_join(ctids[i], NULL) == 0);
        CHECK(cjobs[i].ok);
    }

    test_monotonic_read_deadline(app_path, data_dir);

    /* Open an idle keep-alive connection right before SIGTERM to verify shutdown never hangs */
    int idle_ka_fd = connect_loopback(port);
    {
        char carry[4096], kresp[2048];
        size_t carry_len = 0;
        const char *ping = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        MO_ASSERT(send_all(idle_ka_fd, ping, strlen(ping)));
        CHECK(read_one_http_response(idle_ka_fd, false, carry, sizeof carry, &carry_len, kresp,
                                     sizeof kresp) == 200);
    }

    char err_log[65536];
    stop_server(pid, out_rfd, err_rfd, err_log, sizeof err_log);
    (void)close(idle_ka_fd);

    CHECK(strstr(err_log, "mo: actor failure: crash_actor:") != NULL);
    CHECK(strstr(err_log, "mo serve: failure:") != NULL);
    CHECK(strstr(err_log, "handler crashed") != NULL);
    CHECK(strstr(err_log, "mo serve: request: 20") != NULL);
    CHECK(strstr(err_log, "POST /items 200") != NULL);
    /* Secrets, cookies, query tokens, and request bodies never appear in logs */
    CHECK(strstr(err_log, "secret-key") == NULL);
    CHECK(strstr(err_log, "super-secret-cookie") == NULL);
    CHECK(strstr(err_log, "never-log-me") == NULL);
    CHECK(strstr(err_log, "ka-one") == NULL);
    CHECK(strstr(err_log, "mo serve: stop: a signal asked the server to stop") != NULL);

    /* 7. Portable O_NOFOLLOW static confinement fallback + 1-worker pooled SQLite auto-rollback */
    mo_api_set_force_fallback(true);
    {
        const char *w1_argv[] = {"mo",        "serve", app_path, "--port", "0",
                                 "--workers", "1",     "--data", data_dir};
        pid_t w1_pid = -1;
        int w1_out = -1, w1_err = -1;
        uint16_t w1_port = spawn_server(9, w1_argv, &w1_pid, &w1_out, &w1_err);

        run_static_checks_for_port(w1_port);

        /* Trigger mid-transaction stop on the single worker's pooled sqlite3* connection */
        CHECK(http_exchange(
                  w1_port,
                  "POST /tx-defect HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", resp,
                  sizeof resp) == 500);

        /* Next write + read on the same single worker must succeed with phantom-tx rolled back */
        CHECK(http_exchange(w1_port,
                            "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                            "secret-key\r\nContent-Type: application/json\r\nContent-Length: "
                            "20\r\nConnection: close\r\n\r\n{\"name\":\"after-tx!\"}",
                            resp, sizeof resp) == 200);
        CHECK(strstr(resp, "\"name\":\"after-tx!\"") != NULL);

        CHECK(http_exchange(
                  w1_port,
                  "GET /items?limit=100 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                  resp, sizeof resp) == 200);
        CHECK(strstr(resp, "phantom-tx") == NULL);
        CHECK(strstr(resp, "\"name\":\"after-tx!\"") != NULL);

        char w1_log[65536];
        stop_server(w1_pid, w1_out, w1_err, w1_log, sizeof w1_log);
        CHECK(strstr(w1_log, "mid-transaction defect") != NULL);
    }
    mo_api_set_force_fallback(false);

    /* 8. --watch live reload across multi-module dependency edits, compile errors, and mo.toml */
    test_watch_live_reload(dir);
    test_watch_manifest_dep_reload(dir);

    /* 9. Periodic and durable SQLite background jobs (retry, defect rollback, stale recovery) */
    test_background_jobs(dir);

    char cmd[512];
    MO_ASSERT(snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir) > 0 && system(cmd) == 0);
    return mo_test_report("api_server");
}
