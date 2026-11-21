#define _POSIX_C_SOURCE 200809L
#include "base/runtime-identity.h"
#include "tools/api-server.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *fuzz_image;
static uint32_t fuzz_wire_fn;
static uint32_t fuzz_actor_services[MO_LIMIT_API_ACTORS];
static uint32_t fuzz_actor_count;
static int fuzz_folder_fd = -1;
static pthread_mutex_t fuzz_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void init_fuzz_fixture(void) {
    (void)signal(SIGPIPE, SIG_IGN);
    mo_runtime_identity_init();
    char dir[] = "/tmp/mo-fuzz-api-XXXXXX";
    MO_ASSERT(mkdtemp(dir) != NULL);
    char app_path[256], data_dir[256];
    MO_ASSERT(snprintf(app_path, sizeof app_path, "%s/main.mo", dir) > 0);
    MO_ASSERT(snprintf(data_dir, sizeof data_dir, "%s/data", dir) > 0 &&
              mkdir(data_dir, 0700) == 0);

    FILE *f = fopen(app_path, "wb");
    MO_ASSERT(f != NULL);
    const char *src = "module Main\n"
                      "use Api\n"
                      "struct User\n  name: Text\nend\n"
                      "struct Payload\n  label: Text\n  count: Option(Int)\nend\n"
                      "enum Err\n  Denied\n  Missing\nend\n"
                      "actor Counter\n  state Int = 0\n  calls bump\nend\n"
                      "fn bump(s: Int, _t: Time, by: Int) : (Int, Int)\n  (s + by, s + by)\nend\n"
                      "api\n"
                      "  port 8080\n"
                      "  cors \"*\"\n"
                      "  actor counter: Counter\n"
                      "  auth User via check_auth\n"
                      "  get \"/health\" health\n"
                      "  get \"/items/:id\" get_item\n"
                      "  post \"/items\" post_item\n"
                      "  status Denied = 401\n"
                      "  status Missing = 404\n"
                      "end\n"
                      "fn check_auth(req: Api.Request) : Result(User, Err)\n"
                      "  return Error(Denied) if (req.header(\"Authorization\") or \"\") == \"\"\n"
                      "  Ok(User(name: \"u\"))\n"
                      "end\n"
                      "fn health() : Map(Text, Text)\n  {\"ok\": \"true\"}\nend\n"
                      "fn get_item(counter: Counter, id: Int, q: Option(Text)) : Result(Int, Err)\n"
                      "  return Error(Missing) if id < 0\n"
                      "  extra = case q\n    Some(s): s.byte_count\n    None: 0\n  end\n"
                      "  Ok(counter.bump(id + extra))\n"
                      "end\n"
                      "fn post_item(_u: User, body: Payload) : Api.Response\n"
                      "  Api.Response(status: 201, headers: [(\"X-Tag\", body.label)], body: "
                      "body.label.bytes)\n"
                      "end\n";
    size_t slen = strlen(src);
    MO_ASSERT(fwrite(src, 1, slen, f) == slen && fclose(f) == 0);

    const char *argv[] = {"mo", "serve", app_path, "--port", "0", "--data", data_dir};
    struct mo_cli cli;
    const char *perr = NULL;
    MO_ASSERT(mo_cli_parse(7, argv, &cli, &perr));
    uint16_t default_port = 0;
    MO_ASSERT(mo_api_compile_file(&cli, stderr, stderr, &fuzz_image, &fuzz_wire_fn, &default_port,
                                  fuzz_actor_services, &fuzz_actor_count) == 0 &&
              fuzz_image != NULL);
    fuzz_folder_fd = open(data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(fuzz_folder_fd >= 0);
    (void)unlink(app_path);
    (void)rmdir(data_dir);
    (void)rmdir(dir);
}

struct writer_ctx {
    int fd;
    const uint8_t *bytes;
    size_t size;
    size_t quantum;
    bool half_close;
};

static void *feed_request(void *arg) {
    const struct writer_ctx *w = arg;
    size_t off = 0;
    while (off < w->size) {
        size_t step = w->quantum == 0 || w->size - off < w->quantum ? w->size - off : w->quantum;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(w->fd, w->bytes + off, step, flags);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    if (w->half_close)
        (void)shutdown(w->fd, SHUT_WR);
    uint8_t sink[4096];
    for (;;) {
        ssize_t r = recv(w->fd, sink, sizeof sink, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
    }
    (void)close(w->fd);
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > MO_LIMIT_API_HEADER_BYTES + 8192)
        return 0;
    if (fuzz_image == NULL)
        init_fuzz_fixture();

    struct mo_live_actor *actors[MO_LIMIT_API_ACTORS] = {NULL};
    for (uint32_t a = 0; a < fuzz_actor_count; a++)
        MO_ASSERT(mo_live_actor_new(fuzz_image, fuzz_actor_services[a], NULL, &fuzz_log_mutex,
                                    &actors[a]) == MO_SERVICE_OK);

    int sv[2];
    MO_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    uint8_t control = data[0];
    struct writer_ctx w = {.fd = sv[0],
                           .bytes = data + 1,
                           .size = size - 1,
                           .quantum = (size_t)(control & 31U) * 8U,
                           .half_close = (control & 64U) == 0};
    uint64_t read_timeout_us = w.half_close ? 250000 : 1500;

    pthread_t tid;
    MO_ASSERT(pthread_create(&tid, NULL, feed_request, &w) == 0);

    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 16u << 20;
    mo_api_serve_fd(fuzz_image, fuzz_wire_fn, fuzz_folder_fd, -1, -1, NULL, &fuzz_log_mutex,
                    &limits, actors, fuzz_actor_count, read_timeout_us, 250000, sv[1]);

    MO_ASSERT(pthread_join(tid, NULL) == 0);
    for (uint32_t a = 0; a < fuzz_actor_count; a++)
        mo_live_actor_free(actors[a]);
    return 0;
}
