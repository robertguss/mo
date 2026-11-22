#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "tools/api-server.h"

#include "base/codes.h"
#include "base/stack.h"
#include "base/utf8.h"
#include "bytecode/bytecode.h"
#include "check/check.h"
#include "codegen/codegen.h"
#include "host/host.h"
#include "lower/lower.h"
#include "tools/embedded.h"
#include "tools/run.h"
#include "tools/upgrade.h"
#include "vm/vm.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static pthread_mutex_t fallback_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool api_force_fallback = false;

void mo_api_set_force_fallback(bool force) {
    MO_ASSERT(pthread_mutex_lock(&fallback_mutex) == 0);
    api_force_fallback = force;
    MO_ASSERT(pthread_mutex_unlock(&fallback_mutex) == 0);
}

static bool get_force_fallback(void) {
    MO_ASSERT(pthread_mutex_lock(&fallback_mutex) == 0);
    bool v = api_force_fallback;
    MO_ASSERT(pthread_mutex_unlock(&fallback_mutex) == 0);
    return v;
}

struct api_static_mount {
    const char *prefix;
    size_t prefix_len;
    const char *dir;
};

struct api_job_meta {
    const char *name;
    bool has_schedule;
    uint64_t interval_us;
};

struct api_generation {
    uint32_t refs;
    struct mo_arena meta_arena;
    struct mo_image *image;
    uint32_t wire_fn;
    uint32_t job_fn;
    uint16_t default_port;
    const char *cors;
    uint32_t static_count;
    struct api_static_mount statics[MO_LIMIT_API_STATICS];
    uint32_t job_count;
    struct api_job_meta jobs[MO_LIMIT_API_JOBS];
    uint32_t sqlite_count;
    const char *sqlite_paths[MO_LIMIT_API_SQLITES];
    uint32_t actor_count;
    uint32_t actor_services[MO_LIMIT_API_ACTORS];
    struct mo_live_actor *actors[MO_LIMIT_API_ACTORS];
};

struct watched_file {
    const char *path;
    bool exists;
    uint64_t dev;
    uint64_t ino;
    uint64_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

struct watch_set {
    struct mo_arena arena;
    struct watched_file *items;
    uint32_t count;
};

static void stat_watched_file(const char *path, struct watched_file *out) {
    out->path = path;
    struct stat st;
    if (stat(path, &st) != 0) {
        out->exists = false;
        out->dev = 0;
        out->ino = 0;
        out->size = 0;
        out->mtime_sec = 0;
        out->mtime_nsec = 0;
        return;
    }
    out->exists = true;
    out->dev = (uint64_t)st.st_dev;
    out->ino = (uint64_t)st.st_ino;
    out->size = st.st_size >= 0 ? (uint64_t)st.st_size : 0;
#if defined(__APPLE__)
    out->mtime_sec = (int64_t)st.st_mtimespec.tv_sec;
    out->mtime_nsec = (int64_t)st.st_mtimespec.tv_nsec;
#else
    out->mtime_sec = (int64_t)st.st_mtim.tv_sec;
    out->mtime_nsec = (int64_t)st.st_mtim.tv_nsec;
#endif
}

static bool watch_set_has_changed(const struct watch_set *ws) {
    for (uint32_t i = 0; i < ws->count; i++) {
        struct watched_file cur;
        stat_watched_file(ws->items[i].path, &cur);
        if (cur.exists != ws->items[i].exists || cur.dev != ws->items[i].dev ||
            cur.ino != ws->items[i].ino || cur.size != ws->items[i].size ||
            cur.mtime_sec != ws->items[i].mtime_sec || cur.mtime_nsec != ws->items[i].mtime_nsec)
            return true;
    }
    return false;
}

static void watch_set_add_unique(struct watch_set *ws, uint32_t cap, const char *path) {
    if (path == NULL || path[0] == '\0' || ws->count >= cap)
        return;
    for (uint32_t i = 0; i < ws->count; i++)
        if (strcmp(ws->items[i].path, path) == 0)
            return;
    char *copy = mo_arena_copy_text(&ws->arena, path);
    if (copy == NULL)
        return;
    stat_watched_file(copy, &ws->items[ws->count++]);
}

struct probe_work {
    const char *path;
    bool has_api;
};

static void probe_entry_api(void *arg) {
    struct probe_work *w = arg;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    if (!mo_sources_init(&sources))
        return;
    if (!mo_diags_init(&diags)) {
        mo_sources_free(&sources);
        return;
    }
    mo_arena_init(&arena, 16u << 20);
    uint32_t file = 0;
    struct mo_source_error serr;
    if (mo_sources_add(&sources, w->path, &file, &serr)) {
        struct mo_module_syntax *mod = mo_parse(&arena, &sources, file, &diags);
        if (mod != NULL && mod->tree != NULL && mod->tree->count >= 2 &&
            mod->tree->children[1] != NULL) {
            const struct mo_syntax *items = mod->tree->children[1];
            for (uint32_t i = 0; i < items->count; i++)
                if (items->children[i]->kind == MO_S_API)
                    w->has_api = true;
        }
    }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

struct api_compile_work {
    const struct mo_cli *cli;
    FILE *output, *errors;
    pthread_mutex_t *diag_mutex;
    struct api_generation *gen;
    struct watch_set *watch_out;
    int status;
};

static void api_tool_diag(struct mo_diags *diags, uint16_t code, const char *msg,
                          const char *detail) {
    struct mo_diag d = {.code = code,
                        .message = msg,
                        .reason = code == MO_CODE_API_BLOCK
                                      ? "Serve an entry module that declares an api block "
                                        "(reference 11.6)."
                                      : NULL,
                        .primary = {.span = {.file = UINT32_MAX}, .text = detail}};
    bool added = mo_diags_add(diags, &d);
    (void)added;
}

static void destroy_gen_unlocked(struct api_generation *gen) {
    if (gen == NULL)
        return;
    for (uint32_t a = 0; a < gen->actor_count; a++)
        if (gen->actors[a] != NULL)
            mo_live_actor_free(gen->actors[a]);
    if (gen->image != NULL)
        mo_image_free(gen->image);
    struct mo_arena meta = gen->meta_arena;
    mo_arena_free(&meta);
}

static struct api_generation *alloc_generation_meta(const struct mo_checked_api *entry_api,
                                                    struct mo_image *img, uint32_t wire_fn,
                                                    uint32_t job_fn) {
    struct mo_arena meta;
    mo_arena_init(&meta, 128u << 10);
    struct api_generation *gen =
        mo_arena_allocate(&meta, sizeof *gen, _Alignof(struct api_generation));
    if (gen == NULL) {
        mo_arena_free(&meta);
        return NULL;
    }
    *gen = (struct api_generation){
        .refs = 1,
        .image = img,
        .wire_fn = wire_fn,
        .job_fn = job_fn,
        .default_port = entry_api != NULL ? entry_api->port : 0,
    };
    if (entry_api != NULL) {
        if (entry_api->cors != NULL) {
            char *c = mo_arena_copy_text(&meta, entry_api->cors);
            if (c == NULL) {
                mo_arena_free(&meta);
                return NULL;
            }
            gen->cors = c;
        }
        gen->static_count = entry_api->static_count;
        for (uint32_t s = 0; s < entry_api->static_count; s++) {
            char *pfx = mo_arena_copy_text(&meta, entry_api->statics[s].prefix);
            char *dir = mo_arena_copy_text(&meta, entry_api->statics[s].dir);
            if (pfx == NULL || dir == NULL) {
                mo_arena_free(&meta);
                return NULL;
            }
            gen->statics[s] =
                (struct api_static_mount){.prefix = pfx, .prefix_len = strlen(pfx), .dir = dir};
        }
        gen->job_count = entry_api->job_count;
        for (uint32_t j = 0; j < entry_api->job_count; j++) {
            char *jname = mo_arena_copy_text(&meta, entry_api->jobs[j].name);
            if (jname == NULL) {
                mo_arena_free(&meta);
                return NULL;
            }
            gen->jobs[j] = (struct api_job_meta){.name = jname,
                                                 .has_schedule = entry_api->jobs[j].has_schedule,
                                                 .interval_us = entry_api->jobs[j].interval_us};
        }
        gen->sqlite_count = entry_api->sqlite_count;
        for (uint32_t s = 0; s < entry_api->sqlite_count; s++) {
            char *spath = mo_arena_copy_text(&meta, entry_api->sqlites[s].path);
            if (spath == NULL) {
                mo_arena_free(&meta);
                return NULL;
            }
            gen->sqlite_paths[s] = spath;
        }
        gen->actor_count = entry_api->actor_count;
        for (uint32_t a = 0; a < entry_api->actor_count; a++)
            gen->actor_services[a] = entry_api->actors[a].service_index;
    }
    gen->meta_arena = meta;
    return gen;
}

static void collect_watch_files(struct watch_set *ws, const struct mo_cli *cli,
                                const struct mo_sources *sources, const struct mo_module_set *set) {
    mo_arena_init(&ws->arena, 4u << 20);
    uint32_t extra = set != NULL ? set->manifest_count : 0;
    uint32_t cap = sources->count + extra + 16;
    if (cap > MO_LIMIT_FILES + MO_LIMIT_DEPENDENCIES + 16)
        cap = MO_LIMIT_FILES + MO_LIMIT_DEPENDENCIES + 16;
    ws->items =
        mo_arena_allocate_array(&ws->arena, cap, sizeof *ws->items, _Alignof(struct watched_file));
    ws->count = 0;
    if (ws->items == NULL)
        return;
    if (cli->file_count > 0)
        watch_set_add_unique(ws, cap, mo_cli_file(cli, 0));
    if (set != NULL) {
        for (uint32_t m = 0; m < set->manifest_count; m++) {
            struct stat st;
            if (set->manifests[m] != NULL && stat(set->manifests[m], &st) == 0)
                watch_set_add_unique(ws, cap, set->manifests[m]);
        }
    }
    for (uint32_t i = 0; i < sources->count; i++) {
        bool is_embedded_lib =
            set != NULL && i < set->count && set->library[i] && cli->library == NULL;
        if (is_embedded_lib)
            continue;
        struct stat st;
        if (sources->files[i].path != NULL && stat(sources->files[i].path, &st) == 0)
            watch_set_add_unique(ws, cap, sources->files[i].path);
    }
}

static void compile_api_image(void *arg) {
    struct api_compile_work *w = arg;
    const struct mo_cli *cli = w->cli;
    struct mo_sources sources;
    struct mo_diags diags;
    if (!mo_sources_init(&sources)) {
        w->status = 1;
        return;
    }
    if (!mo_diags_init(&diags)) {
        mo_sources_free(&sources);
        w->status = 1;
        return;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);

    struct mo_module_set *set = mo_load_program_with_library(
        &arena, &sources, mo_cli_file(cli, 0), cli->library,
        cli->library == NULL ? mo_default_embedded_library() : NULL,
        cli->library == NULL ? mo_default_library_root() : NULL, &diags);
    if (w->watch_out != NULL)
        collect_watch_files(w->watch_out, cli, &sources, set);

    struct mo_program *program =
        set == NULL || mo_diags_any(&diags) ? NULL : mo_check(&arena, set, &diags);

    const struct mo_checked_api *entry_api = NULL;
    if (program != NULL && !mo_diags_any(&diags)) {
        for (uint32_t i = 0; i < program->api_count; i++)
            if (program->apis[i].module == 0)
                entry_api = &program->apis[i];
        if (entry_api == NULL)
            api_tool_diag(&diags, MO_CODE_API_BLOCK, "the entry module declares no api block",
                          NULL);
    }

    struct mo_arena lowered;
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);

    if (entry_api != NULL && !mo_diags_any(&diags)) {
        struct mo_services services;
        struct mo_core *core =
            mo_lower_services(&lowered, program, &sources, &diags, NULL, &services);
        if (core != NULL && !mo_diags_any(&diags) &&
            mo_core_check_services(core, &services, &diags) &&
            mo_compile_services(core, &services, &wire, &diags)) {
            struct mo_load_error load_err;
            struct mo_image *img = mo_load(wire.bytes, wire.length, &load_err);
            if (img == NULL) {
                api_tool_diag(&diags, MO_CODE_TOOL_PIPELINE, "compiled api bytecode failed to load",
                              load_err.reason);
            } else {
                struct mo_defect init_defect;
                if (mo_image_init(img, &cli->limits, &init_defect) != MO_RETURNED) {
                    if (w->errors != NULL) {
                        if (w->diag_mutex != NULL)
                            (void)pthread_mutex_lock(w->diag_mutex);
                        bool wrote = mo_write_defect(w->errors, NULL, &init_defect);
                        (void)wrote;
                        (void)fflush(w->errors);
                        if (w->diag_mutex != NULL)
                            (void)pthread_mutex_unlock(w->diag_mutex);
                    }
                    mo_image_free(img);
                    w->status = 70;
                    goto cleanup;
                }
                if (mo_program_has_anchors(program) &&
                    !mo_check_anchors(img, cli, &sources, program, &diags)) {
                    mo_image_free(img);
                } else {
                    const struct mo_bytecode *bc = mo_image_bytecode(img);
                    const char *mod_name = program->set->names[0];
                    char target_name[512];
                    int tn = snprintf(target_name, sizeof target_name, "%s.%%api.wire", mod_name);
                    MO_ASSERT(tn > 0 && (size_t)tn < sizeof target_name);
                    char job_target[512];
                    int jn = snprintf(job_target, sizeof job_target, "%s.%%api.job", mod_name);
                    MO_ASSERT(jn > 0 && (size_t)jn < sizeof job_target);
                    uint32_t found_fn = MO_NO_INDEX;
                    uint32_t found_job_fn = MO_NO_INDEX;
                    for (uint32_t i = 0; i < bc->function_count; i++) {
                        const struct mo_bytecode_string *s = &bc->strings[bc->functions[i].name];
                        if (s->length == (size_t)tn &&
                            memcmp(s->bytes, target_name, (size_t)tn) == 0)
                            found_fn = i;
                        if (s->length == (size_t)jn &&
                            memcmp(s->bytes, job_target, (size_t)jn) == 0)
                            found_job_fn = i;
                    }
                    MO_ASSERT(found_fn != MO_NO_INDEX && found_job_fn != MO_NO_INDEX);
                    struct api_generation *gen =
                        alloc_generation_meta(entry_api, img, found_fn, found_job_fn);
                    if (gen == NULL) {
                        mo_image_free(img);
                        w->status = 1;
                        goto cleanup;
                    }
                    w->gen = gen;
                    w->status = 0;
                }
            }
        }
    }

    if (w->gen == NULL) {
        if (w->output != NULL && w->errors != NULL) {
            if (w->diag_mutex != NULL)
                (void)pthread_mutex_lock(w->diag_mutex);
            w->status = mo_write_diagnostics(w->output, w->errors, false, MO_COMMAND_SERVE,
                                             &sources, &diags, NULL);
            (void)fflush(w->errors);
            if (w->diag_mutex != NULL)
                (void)pthread_mutex_unlock(w->diag_mutex);
        } else {
            w->status = 1;
        }
    }

cleanup:
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

static int compile_api_generation(const struct mo_cli *cli, FILE *output, FILE *errors,
                                  pthread_mutex_t *diag_mutex, struct api_generation **gen_out,
                                  struct watch_set *watch_out) {
    *gen_out = NULL;
    struct api_compile_work cw = {
        .cli = cli,
        .output = output,
        .errors = errors,
        .diag_mutex = diag_mutex,
        .watch_out = watch_out,
        .status = 1,
    };
    if (mo_with_stack(MO_SOURCE_STACK_SIZE, compile_api_image, &cw) != 0)
        return -1;
    if (cw.gen == NULL)
        return cw.status;
    *gen_out = cw.gen;
    return 0;
}

int mo_api_compile_file(const struct mo_cli *cli, FILE *output, FILE *errors,
                        struct mo_image **image_out, uint32_t *wire_fn_out,
                        uint16_t *default_port_out, uint32_t actor_services[MO_LIMIT_API_ACTORS],
                        uint32_t *actor_count_out) {
    *image_out = NULL;
    struct api_generation *gen = NULL;
    int rc = compile_api_generation(cli, output, errors, NULL, &gen, NULL);
    if (rc != 0 || gen == NULL)
        return rc;
    *image_out = gen->image;
    gen->image = NULL;
    *wire_fn_out = gen->wire_fn;
    if (default_port_out != NULL)
        *default_port_out = gen->default_port;
    if (actor_count_out != NULL)
        *actor_count_out = gen->actor_count;
    if (actor_services != NULL)
        for (uint32_t i = 0; i < gen->actor_count; i++)
            actor_services[i] = gen->actor_services[i];
    destroy_gen_unlocked(gen);
    return 0;
}

struct http_header_slice {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
};

struct parsed_http_request {
    const char *method;
    size_t method_len;
    const char *target;
    size_t target_len;
    bool http_1_1;
    uint32_t header_count;
    struct http_header_slice headers[MO_LIMIT_API_HEADERS];
    struct mo_buffer body;
};

static uint64_t api_monotonic_us(void) {
    struct timespec ts = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)(ts.tv_nsec / 1000);
}

static uint64_t add_deadline_us(uint64_t start_us, uint64_t delta_us) {
    return UINT64_MAX - start_us < delta_us ? UINT64_MAX : start_us + delta_us;
}

static bool send_all_deadline(int fd, const uint8_t *buf, size_t len, uint64_t deadline_us) {
    size_t sent = 0;
    while (sent < len) {
        uint64_t now = api_monotonic_us();
        if (now >= deadline_us)
            return false;
        uint64_t rem_us = deadline_us - now;
        int timeout_ms = rem_us > UINT64_C(60000000) ? 60000 : (int)((rem_us + 999) / 1000);
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0)
            return false;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(fd, buf + sent, len - sent, flags);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static ssize_t recv_deadline(int fd, uint8_t *buf, size_t len, uint64_t deadline_us,
                             bool *timed_out) {
    for (;;) {
        uint64_t now = api_monotonic_us();
        if (now >= deadline_us) {
            *timed_out = true;
            return -1;
        }
        uint64_t rem_us = deadline_us - now;
        int timeout_ms = rem_us > UINT64_C(60000000) ? 60000 : (int)((rem_us + 999) / 1000);
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) {
            *timed_out = true;
            return -1;
        }
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ssize_t got = recv(fd, buf, len, 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        return got;
    }
}

static const char *status_reason_phrase(int64_t code) {
    switch (code) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 303:
        return "See Other";
    case 304:
        return "Not Modified";
    case 307:
        return "Temporary Redirect";
    case 308:
        return "Permanent Redirect";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 409:
        return "Conflict";
    case 413:
        return "Content Too Large";
    case 422:
        return "Unprocessable Content";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    default:
        return "Response";
    }
}

static bool send_json_status_with_cors(int fd, int code, const char *err_code, const char *cors,
                                       bool keep_alive, bool is_head, uint64_t write_timeout) {
    uint64_t deadline = add_deadline_us(api_monotonic_us(), write_timeout);
    char payload[128];
    int plen = snprintf(payload, sizeof payload, "{\"error\":\"%s\"}", err_code);
    if (plen < 0 || (size_t)plen >= sizeof payload)
        return false;
    struct mo_buffer head;
    mo_buffer_init(&head, 512);
    char top[160];
    int tlen =
        snprintf(top, sizeof top,
                 "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n", code,
                 status_reason_phrase(code), plen);
    bool ok = tlen > 0 && (size_t)tlen < sizeof top && mo_buffer_append(&head, top, (size_t)tlen);
    if (ok && cors != NULL && cors[0] != '\0') {
        ok = mo_buffer_append_text(&head, "Access-Control-Allow-Origin: ") &&
             mo_buffer_append_text(&head, cors) && mo_buffer_append_text(&head, "\r\n");
    }
    ok = ok && mo_buffer_append_text(&head, keep_alive ? "Connection: keep-alive\r\n\r\n"
                                                       : "Connection: close\r\n\r\n");
    bool sent = false;
    if (ok) {
        sent = send_all_deadline(fd, head.bytes, head.length, deadline);
        if (sent && !is_head)
            sent = send_all_deadline(fd, (const uint8_t *)payload, (size_t)plen, deadline);
    }
    mo_buffer_free(&head);
    return sent;
}

static void send_fixed_json_error(int fd, int code, const char *err_code, uint64_t write_timeout) {
    (void)send_json_status_with_cors(fd, code, err_code, NULL, false, false, write_timeout);
}

static bool ascii_ieq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return false;
    }
    return true;
}

static bool ascii_ends_with_icase(const char *s, size_t slen, const char *suffix) {
    size_t suflen = strlen(suffix);
    if (slen < suflen)
        return false;
    return ascii_ieq(s + (slen - suflen), suflen, suffix);
}

static void connection_header_flags(const struct parsed_http_request *req, bool *has_close,
                                    bool *has_keepalive) {
    *has_close = false;
    *has_keepalive = false;
    for (uint32_t i = 0; i < req->header_count; i++) {
        if (!ascii_ieq(req->headers[i].name, req->headers[i].name_len, "connection"))
            continue;
        const char *v = req->headers[i].value;
        size_t len = req->headers[i].value_len;
        size_t pos = 0;
        while (pos < len) {
            size_t end = pos;
            while (end < len && v[end] != ',')
                end++;
            size_t ts = pos, te = end;
            while (ts < te && (v[ts] == ' ' || v[ts] == '\t'))
                ts++;
            while (te > ts && (v[te - 1] == ' ' || v[te - 1] == '\t'))
                te--;
            if (ascii_ieq(v + ts, te - ts, "close"))
                *has_close = true;
            else if (ascii_ieq(v + ts, te - ts, "keep-alive"))
                *has_keepalive = true;
            pos = end + 1;
        }
    }
}

static bool has_ctl_bytes(const char *s, size_t len, bool allow_tab) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x7f || (c < 0x20 && !(allow_tab && c == '\t')))
            return true;
    }
    return false;
}

static bool recv_more(int fd, uint8_t *buf, size_t cap, size_t *len, uint64_t deadline_us,
                      bool *timed_out) {
    if (*len >= cap)
        return false;
    ssize_t got = recv_deadline(fd, buf + *len, cap - *len, deadline_us, timed_out);
    if (got <= 0)
        return false;
    *len += (size_t)got;
    return true;
}

static int read_exact_body(int fd, const uint8_t *initial, size_t initial_len,
                           size_t content_length, size_t max_leftover, uint64_t deadline_us,
                           struct mo_buffer *out, struct mo_buffer *leftover) {
    size_t take = initial_len > content_length ? content_length : initial_len;
    if (take != 0 && !mo_buffer_append(out, initial, take))
        return 413;
    if (initial_len > content_length) {
        size_t extra = initial_len - content_length;
        if (extra > max_leftover)
            return 431;
        if (!mo_buffer_append(leftover, initial + content_length, extra))
            return 431;
        return 0;
    }
    uint8_t chunk[16384];
    while (out->length < content_length) {
        size_t want = content_length - out->length;
        if (want > sizeof chunk)
            want = sizeof chunk;
        bool timed_out = false;
        ssize_t got = recv_deadline(fd, chunk, want, deadline_us, &timed_out);
        if (got <= 0)
            return timed_out ? 408 : 400;
        if (!mo_buffer_append(out, chunk, (size_t)got))
            return 413;
    }
    return 0;
}

static bool read_chunked_body(int fd, const uint8_t *initial, size_t initial_len,
                              size_t max_leftover, uint64_t deadline_us, struct mo_buffer *out,
                              struct mo_buffer *leftover, int *err_status) {
    struct mo_buffer raw;
    mo_buffer_init(&raw, 65536);
    if (initial_len != 0 && !mo_buffer_append(&raw, initial, initial_len)) {
        mo_buffer_free(&raw);
        *err_status = 413;
        return false;
    }
    size_t pos = 0;
    size_t overhead_bytes = 0;
    uint8_t tmp[16384];
    for (;;) {
        if (pos >= 16384 && pos * 2 >= raw.length) {
            memmove(raw.bytes, raw.bytes + pos, raw.length - pos);
            raw.length -= pos;
            pos = 0;
        }
        size_t line_end = SIZE_MAX;
        for (size_t i = pos; i + 1 < raw.length; i++)
            if (raw.bytes[i] == '\r' && raw.bytes[i + 1] == '\n') {
                line_end = i;
                break;
            }
        if (line_end == SIZE_MAX) {
            if (raw.length - pos > 1024) {
                mo_buffer_free(&raw);
                *err_status = 400;
                return false;
            }
            bool timed_out = false;
            ssize_t got = recv_deadline(fd, tmp, sizeof tmp, deadline_us, &timed_out);
            if (got <= 0) {
                mo_buffer_free(&raw);
                *err_status = timed_out ? 408 : 400;
                return false;
            }
            if (!mo_buffer_append(&raw, tmp, (size_t)got)) {
                mo_buffer_free(&raw);
                *err_status = 413;
                return false;
            }
            continue;
        }
        size_t line_len = line_end - pos;
        if (line_len > 1024) {
            mo_buffer_free(&raw);
            *err_status = 400;
            return false;
        }
        if (overhead_bytes + line_len + 4 > MO_LIMIT_API_CHUNK_OVERHEAD_BYTES) {
            mo_buffer_free(&raw);
            *err_status = 413;
            return false;
        }
        overhead_bytes += line_len + 4;

        size_t chunk_sz = 0;
        bool any_hex = false;
        for (size_t i = pos; i < line_end; i++) {
            uint8_t c = raw.bytes[i];
            if (c == ';')
                break;
            unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10u
                         : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A') + 10u
                                                : 99u;
            if (d >= 16) {
                mo_buffer_free(&raw);
                *err_status = 400;
                return false;
            }
            any_hex = true;
            if (chunk_sz > (MO_LIMIT_API_BODY_BYTES - d) / 16) {
                mo_buffer_free(&raw);
                *err_status = 413;
                return false;
            }
            chunk_sz = chunk_sz * 16 + d;
        }
        if (!any_hex) {
            mo_buffer_free(&raw);
            *err_status = 400;
            return false;
        }
        pos = line_end + 2;
        if (chunk_sz == 0) {
            size_t trailer_bytes = 0;
            for (;;) {
                size_t teol = SIZE_MAX;
                for (size_t i = pos; i + 1 < raw.length; i++)
                    if (raw.bytes[i] == '\r' && raw.bytes[i + 1] == '\n') {
                        teol = i;
                        break;
                    }
                if (teol == SIZE_MAX) {
                    if (trailer_bytes + (raw.length - pos) > MO_LIMIT_API_HEADER_BYTES) {
                        mo_buffer_free(&raw);
                        *err_status = 431;
                        return false;
                    }
                    bool timed_out = false;
                    ssize_t got = recv_deadline(fd, tmp, sizeof tmp, deadline_us, &timed_out);
                    if (got <= 0) {
                        mo_buffer_free(&raw);
                        *err_status = timed_out ? 408 : 400;
                        return false;
                    }
                    if (!mo_buffer_append(&raw, tmp, (size_t)got)) {
                        mo_buffer_free(&raw);
                        *err_status = 413;
                        return false;
                    }
                    continue;
                }
                if (teol == pos) {
                    size_t done_pos = pos + 2;
                    if (raw.length > done_pos) {
                        size_t extra = raw.length - done_pos;
                        if (extra > max_leftover ||
                            !mo_buffer_append(leftover, raw.bytes + done_pos, extra)) {
                            mo_buffer_free(&raw);
                            *err_status = 431;
                            return false;
                        }
                    }
                    mo_buffer_free(&raw);
                    return true;
                }
                size_t tlen = teol - pos;
                if (trailer_bytes + tlen + 2 > MO_LIMIT_API_HEADER_BYTES) {
                    mo_buffer_free(&raw);
                    *err_status = 431;
                    return false;
                }
                const uint8_t *colon = memchr(raw.bytes + pos, ':', tlen);
                if (colon == NULL || colon == raw.bytes + pos ||
                    has_ctl_bytes((const char *)(raw.bytes + pos), tlen, true)) {
                    mo_buffer_free(&raw);
                    *err_status = 400;
                    return false;
                }
                trailer_bytes += tlen + 2;
                pos = teol + 2;
            }
        }
        if (out->length + chunk_sz > MO_LIMIT_API_BODY_BYTES) {
            mo_buffer_free(&raw);
            *err_status = 413;
            return false;
        }
        while (raw.length - pos < chunk_sz + 2) {
            bool timed_out = false;
            ssize_t got = recv_deadline(fd, tmp, sizeof tmp, deadline_us, &timed_out);
            if (got <= 0) {
                mo_buffer_free(&raw);
                *err_status = timed_out ? 408 : 400;
                return false;
            }
            if (!mo_buffer_append(&raw, tmp, (size_t)got)) {
                mo_buffer_free(&raw);
                *err_status = 413;
                return false;
            }
        }
        if (raw.bytes[pos + chunk_sz] != '\r' || raw.bytes[pos + chunk_sz + 1] != '\n' ||
            !mo_buffer_append(out, raw.bytes + pos, chunk_sz)) {
            mo_buffer_free(&raw);
            *err_status = 400;
            return false;
        }
        pos += chunk_sz + 2;
    }
}

/* Returns 0 on success, -1 on clean initial EOF, or HTTP status code (400/408/413/431). */
static int read_http_request(int fd, uint8_t *hbuf, size_t hcap, size_t initial_hlen,
                             uint64_t deadline_us, struct parsed_http_request *out,
                             struct mo_buffer *leftover) {
    size_t hlen = initial_hlen;
    size_t header_end = SIZE_MAX;
    for (;;) {
        for (size_t i = 0; i + 3 < hlen; i++)
            if (hbuf[i] == '\r' && hbuf[i + 1] == '\n' && hbuf[i + 2] == '\r' &&
                hbuf[i + 3] == '\n') {
                header_end = i;
                break;
            }
        if (header_end != SIZE_MAX)
            break;
        if (hlen >= MO_LIMIT_API_HEADER_BYTES)
            return 431;
        size_t prev = hlen;
        bool timed_out = false;
        if (!recv_more(fd, hbuf, hcap, &hlen, deadline_us, &timed_out))
            return timed_out ? 408 : (prev == 0 ? -1 : 400);
    }

    size_t line_end = 0;
    while (line_end + 1 <= header_end && !(hbuf[line_end] == '\r' && hbuf[line_end + 1] == '\n'))
        line_end++;
    const char *req_line = (const char *)hbuf;
    const char *sp1 = memchr(req_line, ' ', line_end);
    if (sp1 == NULL || sp1 == req_line)
        return 400;
    size_t rem_after_m = line_end - (size_t)(sp1 - req_line) - 1;
    const char *sp2 = memchr(sp1 + 1, ' ', rem_after_m);
    if (sp2 == NULL || sp2 == sp1 + 1)
        return 400;
    size_t ver_len = line_end - (size_t)(sp2 - req_line) - 1;
    bool is_11 = ver_len == 8 && memcmp(sp2 + 1, "HTTP/1.1", 8) == 0;
    bool is_10 = ver_len == 8 && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0;
    if (!is_11 && !is_10)
        return 400;

    out->method = req_line;
    out->method_len = (size_t)(sp1 - req_line);
    out->target = sp1 + 1;
    out->target_len = (size_t)(sp2 - (sp1 + 1));
    out->http_1_1 = is_11;
    for (size_t k = 0; k < out->method_len; k++)
        if ((unsigned char)out->method[k] <= 0x20 || (unsigned char)out->method[k] >= 0x7f)
            return 400;
    if (has_ctl_bytes(out->target, out->target_len, false) ||
        !mo_utf8_valid((const uint8_t *)out->method, out->method_len) ||
        !mo_utf8_valid((const uint8_t *)out->target, out->target_len))
        return 400;

    out->header_count = 0;
    bool has_cl = false, is_chunked = false;
    size_t content_length = 0;

    size_t pos = line_end + 2;
    while (pos < header_end) {
        size_t eol = pos;
        while (eol + 1 <= header_end && !(hbuf[eol] == '\r' && hbuf[eol + 1] == '\n'))
            eol++;
        const char *hline = (const char *)(hbuf + pos);
        size_t hline_len = eol - pos;
        const char *colon = memchr(hline, ':', hline_len);
        if (colon == NULL || colon == hline)
            return 400;
        size_t nlen = (size_t)(colon - hline);
        for (size_t k = 0; k < nlen; k++)
            if ((unsigned char)hline[k] <= 0x20 || (unsigned char)hline[k] >= 0x7f)
                return 400;
        const char *vstart = colon + 1;
        size_t vlen = hline_len - nlen - 1;
        while (vlen > 0 && (*vstart == ' ' || *vstart == '\t')) {
            vstart++;
            vlen--;
        }
        while (vlen > 0 && (vstart[vlen - 1] == ' ' || vstart[vlen - 1] == '\t'))
            vlen--;
        if (has_ctl_bytes(vstart, vlen, true) || !mo_utf8_valid((const uint8_t *)vstart, vlen))
            return 400;
        if (out->header_count >= MO_LIMIT_API_HEADERS)
            return 431;
        out->headers[out->header_count++] = (struct http_header_slice){hline, nlen, vstart, vlen};

        if (ascii_ieq(hline, nlen, "content-length")) {
            if (is_chunked || vlen == 0)
                return 400;
            size_t parsed_cl = 0;
            for (size_t k = 0; k < vlen; k++) {
                if (vstart[k] < '0' || vstart[k] > '9')
                    return 400;
                unsigned d = (unsigned)(vstart[k] - '0');
                if (parsed_cl > (MO_LIMIT_API_BODY_BYTES - d) / 10)
                    return 413;
                parsed_cl = parsed_cl * 10 + d;
            }
            if (has_cl && parsed_cl != content_length)
                return 400;
            has_cl = true;
            content_length = parsed_cl;
        } else if (ascii_ieq(hline, nlen, "transfer-encoding")) {
            if (has_cl || is_chunked || !ascii_ieq(vstart, vlen, "chunked"))
                return 400;
            is_chunked = true;
        }
        pos = eol + 2;
    }

    const uint8_t *after_headers = hbuf + header_end + 4;
    size_t after_len = hlen - (header_end + 4);
    if (is_chunked) {
        int chunk_err = 400;
        if (!read_chunked_body(fd, after_headers, after_len, hcap, deadline_us, &out->body,
                               leftover, &chunk_err))
            return chunk_err;
    } else if (content_length > 0) {
        int body_err = read_exact_body(fd, after_headers, after_len, content_length, hcap,
                                       deadline_us, &out->body, leftover);
        if (body_err != 0)
            return body_err;
    } else if (after_len > 0) {
        if (after_len > hcap || !mo_buffer_append(leftover, after_headers, after_len))
            return 431;
    }
    return 0;
}

static const char *static_mime_type(const char *path, size_t len) {
    if (ascii_ends_with_icase(path, len, ".webmanifest"))
        return "application/manifest+json";
    if (ascii_ends_with_icase(path, len, ".mjs") || ascii_ends_with_icase(path, len, ".js"))
        return "text/javascript; charset=utf-8";
    if (ascii_ends_with_icase(path, len, ".css"))
        return "text/css; charset=utf-8";
    if (ascii_ends_with_icase(path, len, ".html"))
        return "text/html; charset=utf-8";
    if (ascii_ends_with_icase(path, len, ".json") || ascii_ends_with_icase(path, len, ".map"))
        return "application/json";
    if (ascii_ends_with_icase(path, len, ".svg"))
        return "image/svg+xml";
    if (ascii_ends_with_icase(path, len, ".png"))
        return "image/png";
    if (ascii_ends_with_icase(path, len, ".jpeg") || ascii_ends_with_icase(path, len, ".jpg"))
        return "image/jpeg";
    if (ascii_ends_with_icase(path, len, ".webp"))
        return "image/webp";
    if (ascii_ends_with_icase(path, len, ".ico"))
        return "image/x-icon";
    if (ascii_ends_with_icase(path, len, ".woff2"))
        return "font/woff2";
    if (ascii_ends_with_icase(path, len, ".woff"))
        return "font/woff";
    if (ascii_ends_with_icase(path, len, ".txt"))
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static bool is_fingerprint_token(const char *s, size_t len) {
    if (len < 8 || len > 64)
        return false;
    bool has_digit = false, has_alpha = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')
            has_digit = true;
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            has_alpha = true;
        else if (c != '_')
            return false;
    }
    return has_digit && has_alpha;
}

static bool is_fingerprinted_asset(const char *path, size_t len) {
    const char *leaf = path;
    size_t leaf_len = len;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            leaf = path + i + 1;
            leaf_len = len - (i + 1);
        }
    }
    const char *last_dot = NULL;
    const char *prev_dot = NULL;
    for (size_t i = 0; i < leaf_len; i++) {
        if (leaf[i] == '.') {
            prev_dot = last_dot;
            last_dot = leaf + i;
        }
    }
    if (last_dot == NULL || last_dot == leaf)
        return false;
    if (prev_dot != NULL && last_dot > prev_dot + 1 &&
        is_fingerprint_token(prev_dot + 1, (size_t)(last_dot - (prev_dot + 1))))
        return true;
    size_t stem_len = (size_t)(last_dot - leaf);
    const char *last_dash = NULL;
    for (size_t i = 0; i < stem_len; i++) {
        if (leaf[i] == '-')
            last_dash = leaf + i;
    }
    if (last_dash != NULL && leaf + stem_len > last_dash + 1 &&
        is_fingerprint_token(last_dash + 1, (size_t)((leaf + stem_len) - (last_dash + 1))))
        return true;
    return false;
}

static int hex_nibble_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool decode_and_validate_static_subpath(const char *raw, size_t raw_len, char *out,
                                               size_t out_cap, size_t *out_len) {
    if (raw_len == 0 || raw[0] == '/' || raw[raw_len - 1] == '/')
        return false;
    size_t dlen = 0;
    for (size_t i = 0; i < raw_len;) {
        if (dlen + 1 >= out_cap)
            return false;
        char c = raw[i];
        if (c == '%') {
            if (i + 2 >= raw_len)
                return false;
            int hi = hex_nibble_val(raw[i + 1]);
            int lo = hex_nibble_val(raw[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            uint8_t byte = (uint8_t)((hi << 4) | lo);
            if (byte == 0)
                return false;
            out[dlen++] = (char)byte;
            i += 3;
        } else {
            if (c == '\0')
                return false;
            out[dlen++] = c;
            i++;
        }
    }
    out[dlen] = '\0';
    if (!mo_utf8_valid((const uint8_t *)out, dlen))
        return false;
    if (dlen == 0 || out[0] == '/' || out[dlen - 1] == '/')
        return false;
    for (size_t i = 0; i < dlen; i++) {
        if (out[i] == '\\' || out[i] == '\0')
            return false;
    }
    size_t seg_start = 0;
    for (size_t i = 0; i <= dlen; i++) {
        if (i == dlen || out[i] == '/') {
            size_t slen = i - seg_start;
            if (slen == 0)
                return false;
            if ((slen == 1 && out[seg_start] == '.') ||
                (slen == 2 && out[seg_start] == '.' && out[seg_start + 1] == '.'))
                return false;
            seg_start = i + 1;
        }
    }
    *out_len = dlen;
    return true;
}

static bool etag_header_matches(const struct parsed_http_request *req, const char *etag) {
    size_t etag_len = strlen(etag);
    for (uint32_t i = 0; i < req->header_count; i++) {
        if (!ascii_ieq(req->headers[i].name, req->headers[i].name_len, "if-none-match"))
            continue;
        const char *v = req->headers[i].value;
        size_t len = req->headers[i].value_len;
        size_t pos = 0;
        while (pos < len) {
            size_t end = pos;
            while (end < len && v[end] != ',')
                end++;
            size_t ts = pos, te = end;
            while (ts < te && (v[ts] == ' ' || v[ts] == '\t'))
                ts++;
            while (te > ts && (v[te - 1] == ' ' || v[te - 1] == '\t'))
                te--;
            if (te - ts == 1 && v[ts] == '*')
                return true;
            if (te - ts >= 2 && v[ts] == 'W' && v[ts + 1] == '/')
                ts += 2;
            if (te - ts == etag_len && memcmp(v + ts, etag, etag_len) == 0)
                return true;
            pos = end + 1;
        }
    }
    return false;
}

/* Returns true if a static mount matched req_path and handled the response. */
static bool try_serve_static_asset(const struct api_generation *gen, int folder_fd,
                                   const struct parsed_http_request *req, const char *req_path,
                                   size_t req_path_len, bool keep_alive, uint64_t write_timeout_us,
                                   int client_fd, int *status_out, bool *io_ok_out) {
    if (gen == NULL || gen->static_count == 0 || folder_fd < 0)
        return false;
    if (gen->cors != NULL && ascii_ieq(req->method, req->method_len, "OPTIONS"))
        return false;

    const struct api_static_mount *matched = NULL;
    const char *raw_sub = NULL;
    size_t raw_sub_len = 0;
    for (uint32_t i = 0; i < gen->static_count; i++) {
        const struct api_static_mount *m = &gen->statics[i];
        if (req_path_len == m->prefix_len && memcmp(req_path, m->prefix, m->prefix_len) == 0) {
            matched = m;
            raw_sub = "";
            raw_sub_len = 0;
            break;
        }
        if (m->prefix_len == 1 && m->prefix[0] == '/' && req_path_len >= 1 && req_path[0] == '/') {
            matched = m;
            raw_sub = req_path + 1;
            raw_sub_len = req_path_len - 1;
            break;
        }
        if (req_path_len > m->prefix_len && memcmp(req_path, m->prefix, m->prefix_len) == 0 &&
            req_path[m->prefix_len] == '/') {
            matched = m;
            raw_sub = req_path + m->prefix_len + 1;
            raw_sub_len = req_path_len - m->prefix_len - 1;
            break;
        }
    }
    if (matched == NULL)
        return false;

    bool is_get = ascii_ieq(req->method, req->method_len, "GET");
    bool is_head = ascii_ieq(req->method, req->method_len, "HEAD");
    if (!is_get && !is_head) {
        *status_out = 405;
        *io_ok_out = send_json_status_with_cors(client_fd, 405, "method_not_allowed", gen->cors,
                                                keep_alive, false, write_timeout_us);
        return true;
    }

    char decoded[MO_LIMIT_HOST_PATH_SIZE];
    size_t decoded_len = 0;
    if (!decode_and_validate_static_subpath(raw_sub, raw_sub_len, decoded, sizeof decoded,
                                            &decoded_len)) {
        *status_out = 404;
        *io_ok_out = send_json_status_with_cors(client_fd, 404, "not_found", gen->cors, keep_alive,
                                                is_head, write_timeout_us);
        return true;
    }

    int file_fd = -1;
    uint64_t file_size = 0;
    int64_t mtime_sec = 0, mtime_nsec = 0;
    if (!mo_host_open_confined_file(folder_fd, matched->dir, decoded, get_force_fallback(),
                                    &file_fd, &file_size, &mtime_sec, &mtime_nsec) ||
        file_size > MO_LIMIT_STATIC_FILE_BYTES) {
        if (file_fd >= 0)
            (void)close(file_fd);
        *status_out = 404;
        *io_ok_out = send_json_status_with_cors(client_fd, 404, "not_found", gen->cors, keep_alive,
                                                is_head, write_timeout_us);
        return true;
    }

    char etag[96];
    int elen = snprintf(etag, sizeof etag, "\"%" PRIx64 "-%" PRIx64 "-%" PRIx64 "\"", file_size,
                        (uint64_t)mtime_sec, (uint64_t)mtime_nsec);
    MO_ASSERT(elen > 0 && (size_t)elen < sizeof etag);

    const char *cache_ctl = is_fingerprinted_asset(decoded, decoded_len)
                                ? "public, max-age=31536000, immutable"
                                : "public, max-age=0, must-revalidate";
    bool not_modified = etag_header_matches(req, etag);
    int code = not_modified ? 304 : 200;
    *status_out = code;

    struct mo_buffer head;
    mo_buffer_init(&head, 1024);
    char top[128];
    int tlen = snprintf(top, sizeof top, "HTTP/1.1 %d %s\r\n", code, status_reason_phrase(code));
    bool ok = tlen > 0 && (size_t)tlen < sizeof top && mo_buffer_append(&head, top, (size_t)tlen);
    if (ok && !not_modified) {
        const char *mime = static_mime_type(decoded, decoded_len);
        ok = mo_buffer_append_text(&head, "Content-Type: ") && mo_buffer_append_text(&head, mime) &&
             mo_buffer_append_text(&head, "\r\n");
    }
    ok = ok && mo_buffer_append_text(&head, "ETag: ") && mo_buffer_append_text(&head, etag) &&
         mo_buffer_append_text(&head, "\r\nCache-Control: ") &&
         mo_buffer_append_text(&head, cache_ctl) &&
         mo_buffer_append_text(&head, "\r\nX-Content-Type-Options: nosniff\r\n");
    if (ok && gen->cors != NULL && gen->cors[0] != '\0') {
        ok = mo_buffer_append_text(&head, "Access-Control-Allow-Origin: ") &&
             mo_buffer_append_text(&head, gen->cors) && mo_buffer_append_text(&head, "\r\n");
    }
    if (ok && !not_modified) {
        char cl_line[64];
        int clen = snprintf(cl_line, sizeof cl_line, "Content-Length: %" PRIu64 "\r\n", file_size);
        ok = clen > 0 && (size_t)clen < sizeof cl_line &&
             mo_buffer_append(&head, cl_line, (size_t)clen);
    }
    ok = ok && mo_buffer_append_text(&head, keep_alive ? "Connection: keep-alive\r\n\r\n"
                                                       : "Connection: close\r\n\r\n");

    uint64_t write_deadline = add_deadline_us(api_monotonic_us(), write_timeout_us);
    bool sent = ok && send_all_deadline(client_fd, head.bytes, head.length, write_deadline);
    mo_buffer_free(&head);

    if (sent && !not_modified && !is_head && file_size > 0) {
        uint8_t chunk[16384];
        uint64_t rem = file_size;
        while (rem > 0) {
            size_t want = rem > sizeof chunk ? sizeof chunk : (size_t)rem;
            ssize_t n = read(file_fd, chunk, want);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                sent = false;
                break;
            }
            if (n == 0) {
                sent = false;
                break;
            }
            if (!send_all_deadline(client_fd, chunk, (size_t)n, write_deadline)) {
                sent = false;
                break;
            }
            rem -= (uint64_t)n;
        }
    }

    (void)close(file_fd);
    *io_ok_out = sent;
    return true;
}

static bool build_env_map(struct mo_host *host, struct mo_heap *heap, struct mo_cell *out) {
    if (!mo_value_map(heap, host->types->files, NULL, NULL, 0, out))
        return false;
    for (char **item = environ; item != NULL && *item != NULL; item++) {
        const char *equals = strchr(*item, '=');
        if (equals == NULL)
            continue;
        size_t klen = (size_t)(equals - *item);
        size_t vlen = strlen(equals + 1);
        if (!mo_utf8_valid((const uint8_t *)*item, klen) ||
            !mo_utf8_valid((const uint8_t *)(equals + 1), vlen))
            continue;
        struct mo_cell key, value;
        bool ok = mo_value_text(heap, *item, klen, &key);
        if (ok) {
            ok = mo_value_text(heap, equals + 1, vlen, &value);
            if (ok) {
                ok = mo_table_set(heap, host->types->files, out, key, value);
                mo_value_drop(heap, host->types->text, value);
            }
            mo_value_drop(heap, host->types->text, key);
        }
        if (!ok) {
            mo_value_drop(heap, host->types->files, *out);
            return false;
        }
    }
    return true;
}

static bool append_sanitized_header_bytes(struct mo_buffer *out, const uint8_t *bytes, size_t len,
                                          bool is_name) {
    if (is_name && len == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' || c == '\n' || c == '\0')
            continue;
        if (is_name && (c <= 0x20 || c >= 0x7f || c == ':'))
            continue;
        if (!mo_buffer_append_byte(out, c))
            return false;
    }
    return true;
}

static void log_api_request(FILE *errors, pthread_mutex_t *log_mutex, const char *method,
                            size_t method_len, const char *path, size_t path_len, int status_code,
                            uint64_t duration_us) {
    if (errors == NULL)
        return;
    struct timespec wall = {0, 0};
    (void)clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm_utc;
    char ts_buf[96] = "1970-01-01T00:00:00Z";
    if (gmtime_r(&wall.tv_sec, &tm_utc) != NULL) {
        (void)snprintf(ts_buf, sizeof ts_buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                       tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour,
                       tm_utc.tm_min, tm_utc.tm_sec);
    }
    double dur_ms = (double)duration_us / 1000.0;
    if (log_mutex != NULL)
        (void)pthread_mutex_lock(log_mutex);
    (void)fprintf(errors, "mo serve: request: %s %.*s %.*s %d %.2fms\n", ts_buf, (int)method_len,
                  method, (int)path_len, path, status_code, dur_ms);
    (void)fflush(errors);
    if (log_mutex != NULL)
        (void)pthread_mutex_unlock(log_mutex);
}

static void log_api_job(FILE *errors, pthread_mutex_t *log_mutex, const char *job_name,
                        const char *outcome_str, uint64_t duration_us) {
    if (errors == NULL)
        return;
    struct timespec wall = {0, 0};
    (void)clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm_utc;
    char ts_buf[96] = "1970-01-01T00:00:00Z";
    if (gmtime_r(&wall.tv_sec, &tm_utc) != NULL) {
        (void)snprintf(ts_buf, sizeof ts_buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                       tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour,
                       tm_utc.tm_min, tm_utc.tm_sec);
    }
    double dur_ms = (double)duration_us / 1000.0;
    if (log_mutex != NULL)
        (void)pthread_mutex_lock(log_mutex);
    (void)fprintf(errors, "mo serve: job: %s %s %s %.2fms\n", ts_buf, job_name, outcome_str,
                  dur_ms);
    (void)fflush(errors);
    if (log_mutex != NULL)
        (void)pthread_mutex_unlock(log_mutex);
}

struct api_worker_pool {
    struct api_generation *active_gen;
    int folder_fd;
    int output_fd;
    int errors_fd;
    FILE *errors;
    struct mo_limits limits;

    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_cond_t job_cond;
    pthread_mutex_t log_mutex;
    bool stopping;
    bool job_thread_started;
    pthread_t job_thread;
    int queue[MO_LIMIT_API_QUEUE];
    uint32_t head, tail, count;
};

static bool pool_is_stopping(struct api_worker_pool *pool) {
    if (pool == NULL)
        return false;
    MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
    bool s = pool->stopping;
    MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
    return s || mo_serve_is_signalled();
}

static struct api_generation *retain_active_gen(struct api_worker_pool *pool) {
    MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
    struct api_generation *gen = pool->active_gen;
    MO_ASSERT(gen != NULL && gen->refs > 0);
    gen->refs++;
    MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
    return gen;
}

static void release_gen(struct api_worker_pool *pool, struct api_generation *gen) {
    if (pool == NULL || gen == NULL)
        return;
    MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
    MO_ASSERT(gen->refs > 0);
    gen->refs--;
    bool zero = gen->refs == 0;
    MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
    if (zero)
        destroy_gen_unlocked(gen);
}

/* Waits for at least 1 byte on an idle keep-alive socket, checking stopping every 50ms. */
static bool wait_keepalive_first_byte(int fd, struct api_worker_pool *pool, uint8_t *hbuf,
                                      size_t hcap, size_t *hlen_out, uint64_t idle_timeout_us) {
    uint64_t deadline = add_deadline_us(api_monotonic_us(), idle_timeout_us);
    for (;;) {
        if (pool_is_stopping(pool))
            return false;
        uint64_t now = api_monotonic_us();
        if (now >= deadline)
            return false;
        uint64_t rem_us = deadline - now;
        int wait_ms = rem_us > UINT64_C(50000) ? 50 : (int)((rem_us + 999) / 1000);
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, wait_ms);
        if (pr == 0)
            continue;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        ssize_t got = recv(fd, hbuf, hcap, 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        *hlen_out = (size_t)got;
        return true;
    }
}

static bool dispatch_vm_request(const struct mo_image *image, uint32_t wire_fn, int folder_fd,
                                int output_fd, int errors_fd, FILE *errors,
                                pthread_mutex_t *log_mutex, const struct mo_limits *limits,
                                struct mo_live_actor *const *actors, uint32_t actor_count,
                                struct mo_sqlite_pool *sql_pool,
                                const struct parsed_http_request *req, const char *req_path,
                                size_t req_path_len, bool keep_alive, uint64_t write_timeout_us,
                                int client_fd, int *status_out) {
    struct mo_process *proc = mo_process_new(image, limits);
    if (proc == NULL) {
        *status_out = 500;
        send_fixed_json_error(client_fd, 500, "internal_server_error", write_timeout_us);
        return false;
    }
    mo_process_instruction_limit(proc, MO_LIMIT_API_REQUEST_WORK);

    struct mo_heap *heap = mo_process_heap(proc);
    struct mo_host *host = mo_process_host(proc);
    host->sqlite_pool = sql_pool;
    if (output_fd >= 0)
        host->output = output_fd;
    if (errors_fd >= 0)
        host->errors = errors_fd;

    const struct mo_bytecode *bc = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    const struct mo_bytecode_function *fn = &bc->functions[wire_fn];
    uint32_t pcount = fn->parameter_count;
    MO_ASSERT(pcount >= 4 && pcount <= 4 + 12 + MO_LIMIT_API_ACTORS);

    struct mo_cell call_args[48] = {{{0}}};
    uint32_t built_args = 0;
    bool have_env = false;
    struct mo_cell env_map = {{0}};
    bool args_ok = true;

    args_ok = mo_value_text(heap, req->method, req->method_len, &call_args[0]);
    if (args_ok) {
        built_args = 1;
        args_ok = mo_value_text(heap, req->target, req->target_len, &call_args[1]);
    }
    if (args_ok) {
        built_args = 2;
        uint32_t list_ty = fn->parameters[2].type;
        uint32_t pair_ty = types->items[list_ty].parameters[0];
        uint32_t text_ty = types->items[pair_ty].parameters[0];
        args_ok = mo_value_list(heap, list_ty, NULL, 0, &call_args[2]);
        if (args_ok) {
            built_args = 3;
            for (uint32_t i = 0; args_ok && i < req->header_count; i++) {
                struct mo_cell kv[2];
                if (!mo_value_text(heap, req->headers[i].name, req->headers[i].name_len, &kv[0])) {
                    args_ok = false;
                    break;
                }
                if (!mo_value_text(heap, req->headers[i].value, req->headers[i].value_len,
                                   &kv[1])) {
                    mo_value_drop(heap, text_ty, kv[0]);
                    args_ok = false;
                    break;
                }
                struct mo_cell pair_cell;
                args_ok = mo_value_product(heap, pair_ty, kv, 2, &pair_cell);
                mo_value_drop(heap, text_ty, kv[0]);
                mo_value_drop(heap, text_ty, kv[1]);
                if (args_ok) {
                    args_ok = mo_list_append(heap, list_ty, &call_args[2], pair_cell);
                    mo_value_drop(heap, pair_ty, pair_cell);
                }
            }
        }
    }
    if (args_ok) {
        args_ok = mo_value_bytes(heap, req->body.bytes, req->body.length, &call_args[3]);
        if (args_ok)
            built_args = 4;
    }

    for (uint32_t i = 4; args_ok && i < pcount; i++) {
        uint32_t cap_ty = fn->parameters[i].type;
        enum mo_capability cap_kind = types->items[cap_ty].capability;
        if (cap_kind == MO_CAP_SIMULATION) {
            uint32_t actor_base = pcount - actor_count;
            MO_ASSERT(i >= actor_base && i - actor_base < actor_count);
            args_ok = mo_value_live_actor(heap, cap_ty, actors[i - actor_base], &call_args[i]);
            if (args_ok)
                built_args = i + 1;
            continue;
        }
        const struct mo_cell *contents = NULL;
        if (cap_kind == MO_CAP_ENV) {
            if (!have_env) {
                args_ok = have_env = build_env_map(host, heap, &env_map);
                if (!args_ok)
                    break;
            }
            contents = &env_map;
        }
        struct mo_host_error herr;
        if (mo_host_real(host, heap, cap_ty, contents, folder_fd, &call_args[i], &herr) !=
            MO_HOST_RETURNED) {
            args_ok = false;
            break;
        }
        built_args = i + 1;
    }

    if (!args_ok) {
        for (uint32_t i = 0; i < built_args; i++)
            mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
        if (have_env)
            mo_value_drop(heap, host->types->files, env_map);
        mo_process_free(proc);
        mo_sqlite_pool_cleanup(sql_pool);
        *status_out = 500;
        send_fixed_json_error(client_fd, 500, "internal_server_error", write_timeout_us);
        return false;
    }

    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(proc, wire_fn, call_args, pcount, &result, &defect);

    if (outcome != MO_RETURNED) {
        if (errors != NULL) {
            if (log_mutex != NULL)
                (void)pthread_mutex_lock(log_mutex);
            (void)fprintf(errors, "mo serve: failure: %.*s %.*s\n", (int)req->method_len,
                          req->method, (int)req_path_len, req_path);
            bool wrote = mo_write_defect(errors, proc, &defect);
            (void)wrote;
            (void)fflush(errors);
            if (log_mutex != NULL)
                (void)pthread_mutex_unlock(log_mutex);
        }
        for (uint32_t i = 0; i < pcount; i++)
            if (fn->parameters[i].ownership == MO_PARAMETER_BORROWED)
                mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
        if (have_env)
            mo_value_drop(heap, host->types->files, env_map);
        mo_process_free(proc);
        mo_sqlite_pool_cleanup(sql_pool);
        *status_out = 500;
        send_fixed_json_error(client_fd, 500, "internal_server_error", write_timeout_us);
        return false;
    }

    for (uint32_t i = 0; i < pcount; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_BORROWED)
            mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
    if (have_env)
        mo_value_drop(heap, host->types->files, env_map);

    int64_t status_code = mo_cell_get_int(mo_value_field(result, 0));
    if (status_code < 100 || status_code > 599)
        status_code = 500;
    *status_out = (int)status_code;
    struct mo_cell res_headers = mo_value_field(result, 1);
    struct mo_cell res_body = mo_value_field(result, 2);
    size_t body_len = 0;
    const uint8_t *body_ptr = mo_value_data(res_body, &body_len);
    bool is_head = ascii_ieq(req->method, req->method_len, "HEAD");
    bool no_body_status =
        (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 304;
    bool effective_keep_alive = keep_alive && status_code < 500;

    struct mo_buffer wire_out;
    mo_buffer_init(&wire_out, 4096);
    char status_line[96];
    int slen = snprintf(status_line, sizeof status_line, "HTTP/1.1 %d %s\r\n", (int)status_code,
                        status_reason_phrase(status_code));
    bool wok = slen > 0 && mo_buffer_append(&wire_out, status_line, (size_t)slen);

    uint32_t rh_count = mo_list_size(res_headers);
    for (uint32_t i = 0; wok && i < rh_count; i++) {
        struct mo_cell pair;
        if (!mo_list_get(res_headers, i, &pair))
            break;
        size_t nlen = 0, vlen = 0;
        const uint8_t *nbytes = mo_value_data(mo_value_field(pair, 0), &nlen);
        const uint8_t *vbytes = mo_value_data(mo_value_field(pair, 1), &vlen);
        if (ascii_ieq((const char *)nbytes, nlen, "content-length") ||
            ascii_ieq((const char *)nbytes, nlen, "transfer-encoding") ||
            ascii_ieq((const char *)nbytes, nlen, "connection"))
            continue;
        wok = append_sanitized_header_bytes(&wire_out, nbytes, nlen, true) &&
              mo_buffer_append_text(&wire_out, ": ") &&
              append_sanitized_header_bytes(&wire_out, vbytes, vlen, false) &&
              mo_buffer_append_text(&wire_out, "\r\n");
    }

    const char *conn_val = effective_keep_alive ? "keep-alive" : "close";
    char cl_hdr[128];
    int clen = no_body_status
                   ? snprintf(cl_hdr, sizeof cl_hdr, "Connection: %s\r\n\r\n", conn_val)
                   : snprintf(cl_hdr, sizeof cl_hdr,
                              "Content-Length: %zu\r\nConnection: %s\r\n\r\n", body_len, conn_val);
    wok = wok && clen > 0 && mo_buffer_append(&wire_out, cl_hdr, (size_t)clen);

    bool sent_ok = false;
    if (wok) {
        uint64_t write_deadline = add_deadline_us(api_monotonic_us(), write_timeout_us);
        sent_ok = send_all_deadline(client_fd, wire_out.bytes, wire_out.length, write_deadline);
        if (sent_ok && !no_body_status && !is_head && body_len > 0)
            sent_ok = send_all_deadline(client_fd, body_ptr, body_len, write_deadline);
    } else {
        *status_out = 500;
        send_fixed_json_error(client_fd, 500, "internal_server_error", write_timeout_us);
    }

    mo_buffer_free(&wire_out);
    mo_value_drop(heap, fn->result_type, result);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(proc);
    mo_sqlite_pool_cleanup(sql_pool);
    return sent_ok && effective_keep_alive;
}

static void serve_connection(struct api_worker_pool *pool, const struct api_generation *fixed_gen,
                             int folder_fd, int output_fd, int errors_fd, FILE *errors,
                             pthread_mutex_t *log_mutex, const struct mo_limits *limits,
                             struct mo_sqlite_pool *sql_pool, uint32_t max_requests,
                             uint64_t idle_timeout_us, uint64_t read_timeout_us,
                             uint64_t write_timeout_us, int client_fd) {
    int fl = fcntl(client_fd, F_GETFL);
    if (fl >= 0)
        (void)fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    size_t hcap = MO_LIMIT_API_HEADER_BYTES + 16384;
    struct mo_arena conn_arena;
    mo_arena_init(&conn_arena, hcap + 4096);
    uint8_t *conn_buf = mo_arena_allocate(&conn_arena, hcap, 1);
    if (conn_buf == NULL) {
        mo_arena_free(&conn_arena);
        send_fixed_json_error(client_fd, 500, "internal_server_error", write_timeout_us);
        (void)close(client_fd);
        return;
    }

    size_t conn_len = 0;
    for (uint32_t req_idx = 0; req_idx < max_requests; req_idx++) {
        if (req_idx > 0 && conn_len == 0) {
            if (!wait_keepalive_first_byte(client_fd, pool, conn_buf, hcap, &conn_len,
                                           idle_timeout_us))
                break;
        }
        if (pool_is_stopping(pool))
            break;

        uint64_t read_deadline = add_deadline_us(api_monotonic_us(), read_timeout_us);
        struct parsed_http_request req = {0};
        mo_buffer_init(&req.body, 4096);
        struct mo_buffer leftover;
        mo_buffer_init(&leftover, 4096);

        int parse_err =
            read_http_request(client_fd, conn_buf, hcap, conn_len, read_deadline, &req, &leftover);
        if (parse_err != 0) {
            if (parse_err > 0) {
                const char *code_str = parse_err == 408   ? "request_timeout"
                                       : parse_err == 413 ? "payload_too_large"
                                       : parse_err == 431 ? "headers_too_large"
                                                          : "bad_request";
                send_fixed_json_error(client_fd, parse_err, code_str, write_timeout_us);
            }
            mo_buffer_free(&leftover);
            mo_buffer_free(&req.body);
            break;
        }

        uint64_t req_start_us = api_monotonic_us();
        const char *qmark = memchr(req.target, '?', req.target_len);
        size_t req_path_len = qmark != NULL ? (size_t)(qmark - req.target) : req.target_len;
        const char *req_path = req.target;

        bool has_close = false, has_ka = false;
        connection_header_flags(&req, &has_close, &has_ka);
        bool keep_alive = !has_close && (req.http_1_1 || has_ka) && (req_idx + 1 < max_requests) &&
                          !pool_is_stopping(pool);

        struct api_generation *retained_gen = pool != NULL ? retain_active_gen(pool) : NULL;
        const struct api_generation *gen = retained_gen != NULL ? retained_gen : fixed_gen;
        MO_ASSERT(gen != NULL);

        int status_code = 500;
        bool continue_conn = false;
        bool static_io_ok = false;
        if (try_serve_static_asset(gen, folder_fd, &req, req_path, req_path_len, keep_alive,
                                   write_timeout_us, client_fd, &status_code, &static_io_ok)) {
            continue_conn = static_io_ok && keep_alive;
        } else {
            continue_conn = dispatch_vm_request(
                gen->image, gen->wire_fn, folder_fd, output_fd, errors_fd, errors, log_mutex,
                limits, gen->actors, gen->actor_count, sql_pool, &req, req_path, req_path_len,
                keep_alive, write_timeout_us, client_fd, &status_code);
        }

        uint64_t req_dur_us = api_monotonic_us() - req_start_us;
        log_api_request(errors, log_mutex, req.method, req.method_len, req_path, req_path_len,
                        status_code, req_dur_us);

        if (retained_gen != NULL)
            release_gen(pool, retained_gen);

        if (continue_conn && leftover.length <= hcap) {
            if (leftover.length > 0)
                memcpy(conn_buf, leftover.bytes, leftover.length);
            conn_len = leftover.length;
        } else {
            continue_conn = false;
        }

        mo_buffer_free(&leftover);
        mo_buffer_free(&req.body);

        if (!continue_conn)
            break;
    }

    mo_arena_free(&conn_arena);
    (void)close(client_fd);
}

void mo_api_serve_fd(const struct mo_image *image, uint32_t wire_fn, int folder_fd, int output_fd,
                     int errors_fd, FILE *errors, pthread_mutex_t *log_mutex,
                     const struct mo_limits *limits, struct mo_live_actor *const *actors,
                     uint32_t actor_count, uint64_t read_timeout_us, uint64_t write_timeout_us,
                     int client_fd) {
    struct api_generation fixed = {
        .refs = 1,
        .image = (struct mo_image *)(uintptr_t)image,
        .wire_fn = wire_fn,
        .actor_count = actor_count,
    };
    for (uint32_t a = 0; a < actor_count && a < MO_LIMIT_API_ACTORS; a++)
        fixed.actors[a] = actors[a];
    serve_connection(NULL, &fixed, folder_fd, output_fd, errors_fd, errors, log_mutex, limits, NULL,
                     1, read_timeout_us, read_timeout_us, write_timeout_us, client_fd);
}

static void *api_worker_main(void *arg) {
    struct api_worker_pool *pool = arg;
    struct mo_sqlite_pool *sql_pool = mo_sqlite_pool_new();
    for (;;) {
        MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
        while (pool->count == 0 && !pool->stopping)
            MO_ASSERT(pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex) == 0);
        if (pool->count == 0 && pool->stopping) {
            MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
            mo_sqlite_pool_free(sql_pool);
            return NULL;
        }
        int fd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % MO_LIMIT_API_QUEUE;
        pool->count--;
        MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);

        serve_connection(pool, NULL, pool->folder_fd, pool->output_fd, pool->errors_fd,
                         pool->errors, &pool->log_mutex, &pool->limits, sql_pool,
                         MO_LIMIT_API_KEEPALIVE_REQUESTS, MO_LIMIT_API_KEEPALIVE_IDLE_US,
                         MO_LIMIT_API_READ_US, MO_LIMIT_API_WRITE_US, fd);
    }
}

enum job_exec_outcome {
    JOB_EXEC_OK,
    JOB_EXEC_ERROR,
    JOB_EXEC_DEFECT,
};

static size_t utf8_safe_prefix(const uint8_t *bytes, size_t len, size_t max_bytes) {
    if (len <= max_bytes)
        return len;
    size_t cut = max_bytes;
    while (cut > 0 && (bytes[cut] & 0xc0) == 0x80)
        cut--;
    return mo_utf8_valid(bytes, cut) ? cut : 0;
}

static enum job_exec_outcome
execute_vm_job(const struct api_generation *gen, int folder_fd, int output_fd, int errors_fd,
               FILE *errors, pthread_mutex_t *log_mutex, const struct mo_limits *limits,
               struct mo_sqlite_pool *sql_pool, const char *job_name, const char *payload_json,
               char *err_buf, size_t err_cap) {
    if (err_cap > 0)
        err_buf[0] = '\0';
    uint64_t start_us = api_monotonic_us();
    struct mo_process *proc = mo_process_new(gen->image, limits);
    if (proc == NULL) {
        if (err_cap > 0)
            (void)snprintf(err_buf, err_cap, "out of memory starting job process");
        log_api_job(errors, log_mutex, job_name, "defect", api_monotonic_us() - start_us);
        return JOB_EXEC_DEFECT;
    }
    mo_process_instruction_limit(proc, MO_LIMIT_API_JOB_WORK);

    struct mo_heap *heap = mo_process_heap(proc);
    struct mo_host *host = mo_process_host(proc);
    host->sqlite_pool = sql_pool;
    host->force_fallback = get_force_fallback();
    if (output_fd >= 0)
        host->output = output_fd;
    if (errors_fd >= 0)
        host->errors = errors_fd;

    const struct mo_bytecode *bc = mo_image_bytecode(gen->image);
    const struct mo_types *types = mo_image_types(gen->image);
    const struct mo_bytecode_function *fn = &bc->functions[gen->job_fn];
    uint32_t pcount = fn->parameter_count;
    MO_ASSERT(pcount >= 2 && pcount <= 2 + 12 + MO_LIMIT_API_ACTORS);

    struct mo_cell call_args[48] = {{{0}}};
    uint32_t built_args = 0;
    bool have_env = false;
    struct mo_cell env_map = {{0}};
    bool args_ok = mo_value_text(heap, job_name, strlen(job_name), &call_args[0]);
    if (args_ok) {
        built_args = 1;
        const char *pj = payload_json != NULL ? payload_json : "";
        args_ok = mo_value_text(heap, pj, strlen(pj), &call_args[1]);
        if (args_ok)
            built_args = 2;
    }

    for (uint32_t i = 2; args_ok && i < pcount; i++) {
        uint32_t cap_ty = fn->parameters[i].type;
        enum mo_capability cap_kind = types->items[cap_ty].capability;
        if (cap_kind == MO_CAP_SIMULATION) {
            uint32_t actor_base = pcount - gen->actor_count;
            MO_ASSERT(i >= actor_base && i - actor_base < gen->actor_count);
            args_ok = mo_value_live_actor(heap, cap_ty, gen->actors[i - actor_base], &call_args[i]);
            if (args_ok)
                built_args = i + 1;
            continue;
        }
        const struct mo_cell *contents = NULL;
        if (cap_kind == MO_CAP_ENV) {
            if (!have_env) {
                args_ok = have_env = build_env_map(host, heap, &env_map);
                if (!args_ok)
                    break;
            }
            contents = &env_map;
        }
        struct mo_host_error herr;
        if (mo_host_real(host, heap, cap_ty, contents, folder_fd, &call_args[i], &herr) !=
            MO_HOST_RETURNED) {
            args_ok = false;
            break;
        }
        built_args = i + 1;
    }

    if (!args_ok) {
        for (uint32_t i = 0; i < built_args; i++)
            mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
        if (have_env)
            mo_value_drop(heap, host->types->files, env_map);
        mo_process_free(proc);
        mo_sqlite_pool_cleanup(sql_pool);
        if (err_cap > 0)
            (void)snprintf(err_buf, err_cap, "unable to initialize job capabilities");
        log_api_job(errors, log_mutex, job_name, "defect", api_monotonic_us() - start_us);
        return JOB_EXEC_DEFECT;
    }

    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(proc, gen->job_fn, call_args, pcount, &result, &defect);

    for (uint32_t i = 0; i < pcount; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_BORROWED)
            mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
    if (have_env)
        mo_value_drop(heap, host->types->files, env_map);

    if (outcome != MO_RETURNED) {
        if (err_cap > 0) {
            const char *reason = defect.reason != NULL ? defect.reason : "job stopped";
            if (defect.limit_name != NULL)
                (void)snprintf(err_buf, err_cap, "defect: %s (%s)", reason, defect.limit_name);
            else
                (void)snprintf(err_buf, err_cap, "defect: %s", reason);
        }
        mo_process_free(proc);
        mo_sqlite_pool_cleanup(sql_pool);
        log_api_job(errors, log_mutex, job_name, "defect", api_monotonic_us() - start_us);
        return JOB_EXEC_DEFECT;
    }

    uint32_t tag = mo_cell_get_tag(result);
    enum job_exec_outcome jres = tag == 0 ? JOB_EXEC_OK : JOB_EXEC_ERROR;
    if (tag != 0 && err_cap > 1) {
        struct mo_cell payload = mo_cell_get_payload(result);
        struct mo_cell err_cell = mo_case_shape(types, fn->result_type, tag) == MO_CASE_BOXED
                                      ? mo_value_field(payload, 0)
                                      : payload;
        size_t elen = 0;
        const uint8_t *ebytes = mo_value_data(err_cell, &elen);
        size_t copy_len = utf8_safe_prefix(ebytes, elen, err_cap - 1);
        if (copy_len > 0)
            memcpy(err_buf, ebytes, copy_len);
        err_buf[copy_len] = '\0';
    }
    mo_value_drop(heap, fn->result_type, result);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(proc);
    mo_sqlite_pool_cleanup(sql_pool);
    log_api_job(errors, log_mutex, job_name, jres == JOB_EXEC_OK ? "ok" : "error",
                api_monotonic_us() - start_us);
    return jres;
}

struct job_schedule_slot {
    char name[129];
    bool has_schedule;
    uint64_t interval_us;
    uint64_t next_due_us;
};

static void sleep_job_worker_us(struct api_worker_pool *pool, uint64_t wait_us) {
    if (wait_us == 0)
        return;
    if (wait_us > MO_LIMIT_API_JOB_POLL_US)
        wait_us = MO_LIMIT_API_JOB_POLL_US;
    struct timespec abstime = {0, 0};
    (void)clock_gettime(CLOCK_REALTIME, &abstime);
    uint64_t add_sec = wait_us / UINT64_C(1000000);
    long add_nsec = (long)((wait_us % UINT64_C(1000000)) * 1000);
    abstime.tv_sec += (time_t)add_sec;
    abstime.tv_nsec += add_nsec;
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec += 1;
        abstime.tv_nsec -= 1000000000L;
    }
    MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
    if (!pool->stopping && !mo_serve_is_signalled())
        (void)pthread_cond_timedwait(&pool->job_cond, &pool->queue_mutex, &abstime);
    MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
}

static void *job_worker_main(void *arg) {
    struct api_worker_pool *pool = arg;
    struct mo_sqlite_pool *sql_pool = mo_sqlite_pool_new();
    const struct api_generation *seen_gen = NULL;
    uint32_t sched_count = 0;
    struct job_schedule_slot sched[MO_LIMIT_API_JOBS];
    memset(sched, 0, sizeof sched);
    bool recovered_db[MO_LIMIT_API_SQLITES] = {false};

    for (;;) {
        if (pool_is_stopping(pool))
            break;

        struct api_generation *gen = retain_active_gen(pool);
        if (gen != seen_gen) {
            struct job_schedule_slot prev_sched[MO_LIMIT_API_JOBS];
            uint32_t prev_count = sched_count;
            memcpy(prev_sched, sched, sizeof sched);

            sched_count = gen->job_count;
            uint64_t now_us = api_monotonic_us();
            for (uint32_t j = 0; j < gen->job_count; j++) {
                const struct api_job_meta *jm = &gen->jobs[j];
                (void)snprintf(sched[j].name, sizeof sched[j].name, "%s", jm->name);
                sched[j].has_schedule = jm->has_schedule;
                sched[j].interval_us = jm->interval_us;
                sched[j].next_due_us = UINT64_MAX;
                if (jm->has_schedule) {
                    uint64_t kept_due = 0;
                    for (uint32_t p = 0; p < prev_count; p++) {
                        if (prev_sched[p].has_schedule &&
                            prev_sched[p].interval_us == jm->interval_us &&
                            strcmp(prev_sched[p].name, jm->name) == 0) {
                            kept_due = prev_sched[p].next_due_us;
                            break;
                        }
                    }
                    sched[j].next_due_us =
                        kept_due != 0 ? kept_due : add_deadline_us(now_us, jm->interval_us);
                }
            }
            memset(recovered_db, 0, sizeof recovered_db);
            seen_gen = gen;
        }

        if (gen->job_count > 0) {
            for (uint32_t j = 0; j < gen->job_count && !pool_is_stopping(pool); j++) {
                if (!sched[j].has_schedule)
                    continue;
                uint64_t now_us = api_monotonic_us();
                if (now_us >= sched[j].next_due_us) {
                    char err_buf[512];
                    (void)execute_vm_job(gen, pool->folder_fd, pool->output_fd, pool->errors_fd,
                                         pool->errors, &pool->log_mutex, &pool->limits, sql_pool,
                                         gen->jobs[j].name, "null", err_buf, sizeof err_buf);
                    sched[j].next_due_us =
                        add_deadline_us(api_monotonic_us(), sched[j].interval_us);
                }
            }

            if (gen->sqlite_count > 0 && sql_pool != NULL && !pool_is_stopping(pool)) {
                const char *job_names[MO_LIMIT_API_JOBS];
                for (uint32_t j = 0; j < gen->job_count; j++)
                    job_names[j] = gen->jobs[j].name;
                bool force_fb = get_force_fallback();
                for (uint32_t s = 0; s < gen->sqlite_count && !pool_is_stopping(pool); s++) {
                    const char *db_path = gen->sqlite_paths[s];
                    if (!mo_sqlite_jobs_has_table(sql_pool, pool->folder_fd, db_path, force_fb))
                        continue;
                    if (!recovered_db[s]) {
                        mo_sqlite_jobs_requeue_stale(sql_pool, pool->folder_fd, db_path, force_fb,
                                                     MO_LIMIT_API_JOB_STALE_SECONDS);
                        recovered_db[s] = true;
                    }
                    while (!pool_is_stopping(pool)) {
                        struct mo_arvo_job_claim claim = {0};
                        if (!mo_sqlite_jobs_claim_next(sql_pool, pool->folder_fd, db_path, force_fb,
                                                       job_names, gen->job_count, &claim))
                            break;
                        char err_buf[512];
                        enum job_exec_outcome out = execute_vm_job(
                            gen, pool->folder_fd, pool->output_fd, pool->errors_fd, pool->errors,
                            &pool->log_mutex, &pool->limits, sql_pool, claim.job_name,
                            claim.payload_json, err_buf, sizeof err_buf);
                        if (out == JOB_EXEC_OK) {
                            mo_sqlite_jobs_complete(sql_pool, pool->folder_fd, db_path, force_fb,
                                                    claim.id);
                        } else {
                            mo_sqlite_jobs_fail_or_retry(sql_pool, pool->folder_fd, db_path,
                                                         force_fb, claim.id, claim.attempts,
                                                         claim.max_attempts, err_buf);
                        }
                        mo_arvo_job_claim_free(&claim);
                    }
                }
            }
        }

        uint64_t next_wake_us = MO_LIMIT_API_JOB_POLL_US;
        uint64_t now_after = api_monotonic_us();
        for (uint32_t j = 0; j < sched_count; j++) {
            if (!sched[j].has_schedule)
                continue;
            uint64_t rem = sched[j].next_due_us > now_after ? sched[j].next_due_us - now_after : 0;
            if (rem < next_wake_us)
                next_wake_us = rem;
        }
        release_gen(pool, gen);

        if (pool_is_stopping(pool))
            break;
        sleep_job_worker_us(pool, next_wake_us);
    }

    mo_sqlite_pool_free(sql_pool);
    return NULL;
}

static bool ensure_job_worker_started(struct api_worker_pool *pool) {
    if (pool->job_thread_started)
        return true;
    pthread_attr_t attr;
    MO_ASSERT(pthread_attr_init(&attr) == 0);
    (void)pthread_attr_setstacksize(&attr, 2u << 20);
    int rc = pthread_create(&pool->job_thread, &attr, job_worker_main, pool);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0)
        return false;
    pool->job_thread_started = true;
    return true;
}

static int open_api_listen_socket(const char *bind_host, uint16_t port, uint16_t *actual_port,
                                  int *err_out) {
    int type = SOCK_STREAM;
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
#endif
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        *err_out = errno;
        return -1;
    }
    int fl = fcntl(fd, F_GETFL);
    int fdfl = fcntl(fd, F_GETFD);
    int reuse = 1;
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fdfl < 0 ||
        fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) != 0) {
        *err_out = errno;
        (void)close(fd);
        return -1;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, bind_host, &addr.sin_addr) != 1) {
        *err_out = EINVAL;
        (void)close(fd);
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, MO_LIMIT_API_QUEUE) != 0) {
        *err_out = errno;
        (void)close(fd);
        return -1;
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof bound;
    if (getsockname(fd, (struct sockaddr *)&bound, &blen) != 0) {
        *err_out = errno;
        (void)close(fd);
        return -1;
    }
    *actual_port = ntohs(bound.sin_port);
    return fd;
}

static int report_serve_start_error(FILE *output, FILE *errors, const char *msg) {
    struct mo_sources sources;
    struct mo_diags diags;
    if (!mo_sources_init(&sources))
        return 1;
    if (!mo_diags_init(&diags)) {
        mo_sources_free(&sources);
        return 1;
    }
    api_tool_diag(&diags, MO_CODE_TOOL_SERVICE_START, msg, NULL);
    int st = mo_write_diagnostics(output, errors, false, MO_COMMAND_SERVE, &sources, &diags, NULL);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return st;
}

static bool start_generation_actors(struct api_generation *gen, FILE *errors,
                                    pthread_mutex_t *log_mutex, enum mo_service_status *err_out) {
    for (uint32_t a = 0; a < gen->actor_count; a++) {
        enum mo_service_status ast = mo_live_actor_new(gen->image, gen->actor_services[a], errors,
                                                       log_mutex, &gen->actors[a]);
        if (ast != MO_SERVICE_OK) {
            for (uint32_t prev = 0; prev < a; prev++) {
                mo_live_actor_free(gen->actors[prev]);
                gen->actors[prev] = NULL;
            }
            if (err_out != NULL)
                *err_out = ast;
            return false;
        }
    }
    return true;
}

static void maybe_reload_watched_api(const struct mo_cli *cli, FILE *output, FILE *errors,
                                     struct api_worker_pool *pool, struct watch_set *watched) {
    if (!watch_set_has_changed(watched))
        return;

    struct api_generation *next_gen = NULL;
    struct watch_set next_watch = {0};
    int rc = compile_api_generation(cli, output, errors, &pool->log_mutex, &next_gen, &next_watch);
    if (rc == 0 && next_gen != NULL) {
        enum mo_service_status ast = MO_SERVICE_OK;
        if (!start_generation_actors(next_gen, errors, &pool->log_mutex, &ast)) {
            if (errors != NULL) {
                (void)pthread_mutex_lock(&pool->log_mutex);
                (void)fprintf(errors, "mo serve: reload: could not start actor: %s\n",
                              mo_service_status_text(ast));
                (void)fflush(errors);
                (void)pthread_mutex_unlock(&pool->log_mutex);
            }
            destroy_gen_unlocked(next_gen);
            for (uint32_t i = 0; i < watched->count; i++)
                stat_watched_file(watched->items[i].path, &watched->items[i]);
            mo_arena_free(&next_watch.arena);
            return;
        }
        MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
        struct api_generation *old_gen = pool->active_gen;
        pool->active_gen = next_gen;
        MO_ASSERT(old_gen != NULL && old_gen->refs > 0);
        old_gen->refs--;
        bool free_old = old_gen->refs == 0;
        if (pool->job_thread_started)
            MO_ASSERT(pthread_cond_broadcast(&pool->queue_cond) == 0);
        MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
        if (free_old)
            destroy_gen_unlocked(old_gen);
        if (next_gen->job_count > 0)
            (void)ensure_job_worker_started(pool);

        mo_arena_free(&watched->arena);
        *watched = next_watch;

        if (errors != NULL) {
            (void)pthread_mutex_lock(&pool->log_mutex);
            (void)fprintf(errors, "mo serve: reload: %s\n", mo_cli_file(cli, 0));
            (void)fflush(errors);
            (void)pthread_mutex_unlock(&pool->log_mutex);
        }
    } else {
        /* Keep any previously tracked module paths alongside newly seen files,
         * and refresh all stat snapshots so diagnostics print once per save. */
        if (next_watch.items != NULL) {
            uint32_t merged_cap = watched->count + next_watch.count + 8;
            struct mo_arena merged_arena;
            mo_arena_init(&merged_arena, 4u << 20);
            struct watched_file *merged_items = mo_arena_allocate_array(
                &merged_arena, merged_cap, sizeof *merged_items, _Alignof(struct watched_file));
            if (merged_items != NULL) {
                struct watch_set merged = {
                    .arena = merged_arena, .items = merged_items, .count = 0};
                for (uint32_t i = 0; i < next_watch.count; i++)
                    watch_set_add_unique(&merged, merged_cap, next_watch.items[i].path);
                for (uint32_t i = 0; i < watched->count; i++)
                    watch_set_add_unique(&merged, merged_cap, watched->items[i].path);
                mo_arena_free(&watched->arena);
                *watched = merged;
            } else {
                mo_arena_free(&merged_arena);
                for (uint32_t i = 0; i < watched->count; i++)
                    stat_watched_file(watched->items[i].path, &watched->items[i]);
            }
            mo_arena_free(&next_watch.arena);
        } else {
            for (uint32_t i = 0; i < watched->count; i++)
                stat_watched_file(watched->items[i].path, &watched->items[i]);
        }
    }
}

bool mo_serve_maybe_api(const struct mo_cli *cli, FILE *output, FILE *errors, int *status_out) {
    bool is_api = false;
    if (cli->file_count == 1 && !cli->upgrade && !cli->adopt && cli->migration == NULL &&
        cli->admin_socket == NULL) {
        struct probe_work pw = {.path = mo_cli_file(cli, 0)};
        if (mo_with_stack(MO_SOURCE_STACK_SIZE, probe_entry_api, &pw) == 0)
            is_api = pw.has_api;
    }

    if (!is_api) {
        if (cli->bind_host != NULL || cli->has_workers || cli->watch) {
            (void)fprintf(errors,
                          "--bind, --workers and --watch apply only to api servers: \"serve\"\n");
            *status_out = 2;
            return true;
        }
        if (!cli->has_port) {
            (void)fprintf(errors, "serve needs --port N: \"serve\"\n");
            *status_out = 2;
            return true;
        }
        if (cli->data == NULL) {
            (void)fprintf(errors, "serve needs --data DIR: \"serve\"\n");
            *status_out = 2;
            return true;
        }
        return false;
    }

    if (!mo_serve_watch_signals()) {
        char buf[256];
        (void)snprintf(buf, sizeof buf, "mo serve could not watch for stop signals: %s",
                       strerror(errno));
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    const char *data_dir = cli->data != NULL ? cli->data : ".";
    struct stat dir_st;
    if (stat(data_dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
        char buf[MO_CLI_ARGUMENT_SIZE + 128];
        (void)snprintf(buf, sizeof buf, "mo serve can't use the data folder %s: %s", data_dir,
                       strerror(errno != 0 ? errno : ENOTDIR));
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    struct api_generation *gen = NULL;
    struct watch_set watched = {0};
    int comp_rc =
        compile_api_generation(cli, output, errors, NULL, &gen, cli->watch ? &watched : NULL);
    if (comp_rc < 0) {
        if (cli->watch)
            mo_arena_free(&watched.arena);
        *status_out =
            report_serve_start_error(output, errors, "could not prepare the source pipeline stack");
        return true;
    }
    if (gen == NULL) {
        if (cli->watch)
            mo_arena_free(&watched.arena);
        *status_out = comp_rc;
        return true;
    }

    struct mo_process *probe = mo_process_new(gen->image, &cli->limits);
    const char *host_err = probe == NULL ? "out of memory" : mo_host_start(mo_process_host(probe));
    mo_process_free(probe);
    if (host_err != NULL) {
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(gen);
        *status_out = report_serve_start_error(output, errors, host_err);
        return true;
    }

    int folder_fd = open(data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder_fd < 0) {
        char buf[MO_CLI_ARGUMENT_SIZE + 128];
        (void)snprintf(buf, sizeof buf, "mo serve can't open the data folder %s: %s", data_dir,
                       strerror(errno));
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(gen);
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    const char *bind_host = cli->bind_host != NULL ? cli->bind_host : "127.0.0.1";
    uint16_t requested_port = cli->has_port ? cli->port : gen->default_port;
    uint16_t actual_port = 0;
    int listen_err = 0;
    int listen_fd = open_api_listen_socket(bind_host, requested_port, &actual_port, &listen_err);
    if (listen_fd < 0) {
        char buf[256];
        (void)snprintf(buf, sizeof buf, "mo serve could not listen on %s:%u: %s", bind_host,
                       (unsigned)requested_port, strerror(listen_err));
        (void)close(folder_fd);
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(gen);
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    struct mo_limits req_limits = cli->limits;
    req_limits.heap_size = MO_LIMIT_API_REQUEST_HEAP;

    struct api_worker_pool pool = {.active_gen = gen,
                                   .folder_fd = folder_fd,
                                   .output_fd = output != NULL ? fileno(output) : -1,
                                   .errors_fd = errors != NULL ? fileno(errors) : -1,
                                   .errors = errors,
                                   .limits = req_limits};
    MO_ASSERT(pthread_mutex_init(&pool.queue_mutex, NULL) == 0);
    MO_ASSERT(pthread_cond_init(&pool.queue_cond, NULL) == 0);
    MO_ASSERT(pthread_cond_init(&pool.job_cond, NULL) == 0);
    MO_ASSERT(pthread_mutex_init(&pool.log_mutex, NULL) == 0);

    enum mo_service_status actor_err = MO_SERVICE_OK;
    if (!start_generation_actors(gen, errors, &pool.log_mutex, &actor_err)) {
        char buf[192];
        (void)snprintf(buf, sizeof buf, "mo serve could not start actor: %s",
                       mo_service_status_text(actor_err));
        (void)pthread_mutex_destroy(&pool.log_mutex);
        (void)pthread_cond_destroy(&pool.job_cond);
        (void)pthread_cond_destroy(&pool.queue_cond);
        (void)pthread_mutex_destroy(&pool.queue_mutex);
        (void)close(listen_fd);
        (void)close(folder_fd);
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(gen);
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    pthread_attr_t attr;
    MO_ASSERT(pthread_attr_init(&attr) == 0);
    (void)pthread_attr_setstacksize(&attr, 2u << 20);

    uint32_t target_workers = cli->workers > 0 ? cli->workers : MO_LIMIT_API_WORKERS;
    if (target_workers > MO_LIMIT_API_WORKERS_MAX)
        target_workers = MO_LIMIT_API_WORKERS_MAX;
    pthread_t workers[MO_LIMIT_API_WORKERS_MAX];
    uint32_t started_workers = 0;
    for (uint32_t i = 0; i < target_workers; i++) {
        if (pthread_create(&workers[i], &attr, api_worker_main, &pool) != 0)
            break;
        started_workers++;
    }
    (void)pthread_attr_destroy(&attr);

    if (started_workers == 0) {
        (void)pthread_mutex_destroy(&pool.log_mutex);
        (void)pthread_cond_destroy(&pool.job_cond);
        (void)pthread_cond_destroy(&pool.queue_cond);
        (void)pthread_mutex_destroy(&pool.queue_mutex);
        (void)close(listen_fd);
        (void)close(folder_fd);
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(gen);
        *status_out = report_serve_start_error(output, errors, "mo serve could not start workers");
        return true;
    }

    if (gen->job_count > 0 && !ensure_job_worker_started(&pool)) {
        MO_ASSERT(pthread_mutex_lock(&pool.queue_mutex) == 0);
        pool.stopping = true;
        MO_ASSERT(pthread_cond_broadcast(&pool.queue_cond) == 0);
        MO_ASSERT(pthread_cond_broadcast(&pool.job_cond) == 0);
        MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);
        for (uint32_t i = 0; i < started_workers; i++)
            (void)pthread_join(workers[i], NULL);
        (void)pthread_mutex_destroy(&pool.log_mutex);
        (void)pthread_cond_destroy(&pool.job_cond);
        (void)pthread_cond_destroy(&pool.queue_cond);
        (void)pthread_mutex_destroy(&pool.queue_mutex);
        (void)close(listen_fd);
        (void)close(folder_fd);
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(pool.active_gen);
        *status_out =
            report_serve_start_error(output, errors, "mo serve could not start job worker");
        return true;
    }

    if (fprintf(output, "mo serve: listening on http://%s:%u\n", bind_host, (unsigned)actual_port) <
            0 ||
        fflush(output) != 0) {
        MO_ASSERT(pthread_mutex_lock(&pool.queue_mutex) == 0);
        pool.stopping = true;
        MO_ASSERT(pthread_cond_broadcast(&pool.queue_cond) == 0);
        MO_ASSERT(pthread_cond_broadcast(&pool.job_cond) == 0);
        MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);
        for (uint32_t i = 0; i < started_workers; i++)
            (void)pthread_join(workers[i], NULL);
        if (pool.job_thread_started)
            (void)pthread_join(pool.job_thread, NULL);
        (void)pthread_mutex_destroy(&pool.log_mutex);
        (void)pthread_cond_destroy(&pool.job_cond);
        (void)pthread_cond_destroy(&pool.queue_cond);
        (void)pthread_mutex_destroy(&pool.queue_mutex);
        (void)close(listen_fd);
        (void)close(folder_fd);
        if (cli->watch)
            mo_arena_free(&watched.arena);
        destroy_gen_unlocked(pool.active_gen);
        *status_out =
            report_serve_start_error(output, errors, "mo serve could not write its listening line");
        return true;
    }

    uint64_t next_watch_us =
        cli->watch ? add_deadline_us(api_monotonic_us(), MO_LIMIT_API_WATCH_POLL_US) : UINT64_MAX;

    while (!mo_serve_is_signalled()) {
        if (cli->watch) {
            uint64_t now_us = api_monotonic_us();
            if (now_us >= next_watch_us) {
                maybe_reload_watched_api(cli, output, errors, &pool, &watched);
                next_watch_us = add_deadline_us(api_monotonic_us(), MO_LIMIT_API_WATCH_POLL_US);
            }
        }
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 50);
        if (ready <= 0)
            continue;
        for (;;) {
            int cfd = -1;
#if defined(SOCK_CLOEXEC) && defined(__linux__)
            cfd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
#else
            cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                int cfl = fcntl(cfd, F_GETFD);
                if (cfl >= 0)
                    (void)fcntl(cfd, F_SETFD, cfl | FD_CLOEXEC);
            }
#endif
            if (cfd < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            MO_ASSERT(pthread_mutex_lock(&pool.queue_mutex) == 0);
            if (pool.count < MO_LIMIT_API_QUEUE) {
                pool.queue[pool.tail] = cfd;
                pool.tail = (pool.tail + 1) % MO_LIMIT_API_QUEUE;
                pool.count++;
                MO_ASSERT(pthread_cond_signal(&pool.queue_cond) == 0);
                MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);
            } else {
                MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);
                send_fixed_json_error(cfd, 503, "service_unavailable", MO_LIMIT_API_WRITE_US);
                (void)close(cfd);
            }
        }
    }

    (void)close(listen_fd);
    MO_ASSERT(pthread_mutex_lock(&pool.queue_mutex) == 0);
    pool.stopping = true;
    MO_ASSERT(pthread_cond_broadcast(&pool.queue_cond) == 0);
    MO_ASSERT(pthread_cond_broadcast(&pool.job_cond) == 0);
    MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);

    for (uint32_t i = 0; i < started_workers; i++)
        (void)pthread_join(workers[i], NULL);
    if (pool.job_thread_started)
        (void)pthread_join(pool.job_thread, NULL);

    if (cli->watch)
        mo_arena_free(&watched.arena);

    destroy_gen_unlocked(pool.active_gen);

    (void)fprintf(errors, "mo serve: stop: a signal asked the server to stop\n");
    (void)fflush(errors);

    (void)pthread_mutex_destroy(&pool.log_mutex);
    (void)pthread_cond_destroy(&pool.job_cond);
    (void)pthread_cond_destroy(&pool.queue_cond);
    (void)pthread_mutex_destroy(&pool.queue_mutex);
    (void)close(folder_fd);
    *status_out = 0;
    return true;
}
