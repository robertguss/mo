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
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern char **environ;

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
    struct mo_image *image;
    uint32_t wire_fn;
    uint16_t default_port;
    uint32_t actor_count;
    uint32_t actor_services[MO_LIMIT_API_ACTORS];
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
        w->default_port = entry_api->port;
        w->actor_count = entry_api->actor_count;
        for (uint32_t a = 0; a < entry_api->actor_count; a++)
            w->actor_services[a] = entry_api->actors[a].service_index;
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
                    bool wrote = mo_write_defect(w->errors, NULL, &init_defect);
                    (void)wrote;
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
                    uint32_t found_fn = MO_NO_INDEX;
                    for (uint32_t i = 0; i < bc->function_count; i++) {
                        const struct mo_bytecode_string *s = &bc->strings[bc->functions[i].name];
                        if (s->length == (size_t)tn &&
                            memcmp(s->bytes, target_name, (size_t)tn) == 0) {
                            found_fn = i;
                            break;
                        }
                    }
                    MO_ASSERT(found_fn != MO_NO_INDEX);
                    w->image = img;
                    w->wire_fn = found_fn;
                    w->status = 0;
                }
            }
        }
    }

    if (w->image == NULL)
        w->status = mo_write_diagnostics(w->output, w->errors, false, MO_COMMAND_SERVE, &sources,
                                         &diags, NULL);

cleanup:
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
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
    uint32_t header_count;
    struct http_header_slice headers[MO_LIMIT_API_HEADERS];
    struct mo_buffer body;
};

static bool send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(fd, buf + sent, len - sent, flags);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        sent += (size_t)n;
    }
    return true;
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
    case 304:
        return "Not Modified";
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

static void send_fixed_json_error(int fd, int code, const char *err_code) {
    char payload[128];
    int plen = snprintf(payload, sizeof payload, "{\"error\":\"%s\"}", err_code);
    if (plen < 0 || (size_t)plen >= sizeof payload)
        return;
    char head[256];
    int hlen = snprintf(head, sizeof head,
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
                        "%d\r\nConnection: close\r\n\r\n",
                        code, status_reason_phrase(code), plen);
    if (hlen > 0 && (size_t)hlen < sizeof head) {
        (void)send_all(fd, (const uint8_t *)head, (size_t)hlen);
        (void)send_all(fd, (const uint8_t *)payload, (size_t)plen);
    }
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

static bool recv_more(int fd, uint8_t *buf, size_t cap, size_t *len) {
    if (*len >= cap)
        return false;
    for (;;) {
        ssize_t got = recv(fd, buf + *len, cap - *len, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        *len += (size_t)got;
        return true;
    }
}

static bool read_exact_body(int fd, const uint8_t *initial, size_t initial_len,
                            size_t content_length, struct mo_buffer *out) {
    if (initial_len > content_length)
        initial_len = content_length;
    if (initial_len != 0 && !mo_buffer_append(out, initial, initial_len))
        return false;
    uint8_t chunk[16384];
    while (out->length < content_length) {
        size_t want = content_length - out->length;
        if (want > sizeof chunk)
            want = sizeof chunk;
        ssize_t got = recv(fd, chunk, want, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        if (!mo_buffer_append(out, chunk, (size_t)got))
            return false;
    }
    return true;
}

static bool read_chunked_body(int fd, const uint8_t *initial, size_t initial_len,
                              struct mo_buffer *out, int *err_status) {
    struct mo_buffer raw;
    mo_buffer_init(&raw, MO_LIMIT_API_BODY_BYTES + 65536);
    if (initial_len != 0 && !mo_buffer_append(&raw, initial, initial_len)) {
        mo_buffer_free(&raw);
        *err_status = 413;
        return false;
    }
    size_t pos = 0;
    uint8_t tmp[16384];
    for (;;) {
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
            ssize_t got = recv(fd, tmp, sizeof tmp, 0);
            if (got <= 0) {
                if (got < 0 && errno == EINTR)
                    continue;
                mo_buffer_free(&raw);
                *err_status = 400;
                return false;
            }
            if (!mo_buffer_append(&raw, tmp, (size_t)got)) {
                mo_buffer_free(&raw);
                *err_status = 413;
                return false;
            }
            continue;
        }
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
            while (raw.length - pos < 2) {
                ssize_t got = recv(fd, tmp, sizeof tmp, 0);
                if (got <= 0) {
                    if (got < 0 && errno == EINTR)
                        continue;
                    mo_buffer_free(&raw);
                    *err_status = 400;
                    return false;
                }
                if (!mo_buffer_append(&raw, tmp, (size_t)got)) {
                    mo_buffer_free(&raw);
                    *err_status = 413;
                    return false;
                }
            }
            mo_buffer_free(&raw);
            return true;
        }
        if (out->length + chunk_sz > MO_LIMIT_API_BODY_BYTES) {
            mo_buffer_free(&raw);
            *err_status = 413;
            return false;
        }
        while (raw.length - pos < chunk_sz + 2) {
            ssize_t got = recv(fd, tmp, sizeof tmp, 0);
            if (got <= 0) {
                if (got < 0 && errno == EINTR)
                    continue;
                mo_buffer_free(&raw);
                *err_status = 400;
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

/* Returns 0 on success, -1 on clean initial EOF, or HTTP status code (400/413/431) on error. */
static int read_http_request(int fd, uint8_t *hbuf, size_t hcap, struct parsed_http_request *out) {
    size_t hlen = 0;
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
        if (!recv_more(fd, hbuf, hcap, &hlen))
            return prev == 0 ? -1 : 400;
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
    if (ver_len != 8 ||
        (memcmp(sp2 + 1, "HTTP/1.1", 8) != 0 && memcmp(sp2 + 1, "HTTP/1.0", 8) != 0))
        return 400;

    out->method = req_line;
    out->method_len = (size_t)(sp1 - req_line);
    out->target = sp1 + 1;
    out->target_len = (size_t)(sp2 - (sp1 + 1));
    if (!mo_utf8_valid((const uint8_t *)out->method, out->method_len) ||
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
        if (!mo_utf8_valid((const uint8_t *)vstart, vlen))
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
            if (has_cl || !ascii_ieq(vstart, vlen, "chunked"))
                return 400;
            is_chunked = true;
        }
        pos = eol + 2;
    }

    const uint8_t *after_headers = hbuf + header_end + 4;
    size_t after_len = hlen - (header_end + 4);
    if (is_chunked) {
        int chunk_err = 400;
        if (!read_chunked_body(fd, after_headers, after_len, &out->body, &chunk_err))
            return chunk_err;
    } else if (content_length > 0) {
        if (!read_exact_body(fd, after_headers, after_len, content_length, &out->body))
            return 400;
    }
    return 0;
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

struct api_worker_pool {
    const struct mo_image *image;
    uint32_t wire_fn;
    int folder_fd;
    FILE *output;
    FILE *errors;
    struct mo_limits limits;
    uint32_t actor_count;
    struct mo_live_actor *actors[MO_LIMIT_API_ACTORS];

    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_mutex_t log_mutex;
    bool stopping;
    int queue[MO_LIMIT_API_QUEUE];
    uint32_t head, tail, count;
};

static void dispatch_client_fd(struct api_worker_pool *pool, int client_fd) {
    struct timeval tv = {.tv_sec = 15, .tv_usec = 0};
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    size_t hcap = MO_LIMIT_API_HEADER_BYTES + 16384;
    struct mo_arena req_arena;
    mo_arena_init(&req_arena, hcap + 4096);
    uint8_t *hbuf = mo_arena_allocate(&req_arena, hcap, 1);
    if (hbuf == NULL) {
        mo_arena_free(&req_arena);
        send_fixed_json_error(client_fd, 500, "internal_server_error");
        (void)close(client_fd);
        return;
    }

    struct parsed_http_request req = {0};
    mo_buffer_init(&req.body, MO_LIMIT_API_BODY_BYTES);
    int parse_err = read_http_request(client_fd, hbuf, hcap, &req);
    if (parse_err != 0) {
        if (parse_err > 0) {
            const char *code_str = parse_err == 413   ? "payload_too_large"
                                   : parse_err == 431 ? "headers_too_large"
                                                      : "bad_request";
            send_fixed_json_error(client_fd, parse_err, code_str);
        }
        mo_buffer_free(&req.body);
        mo_arena_free(&req_arena);
        (void)close(client_fd);
        return;
    }

    struct mo_process *proc = mo_process_new(pool->image, &pool->limits);
    if (proc == NULL) {
        send_fixed_json_error(client_fd, 500, "internal_server_error");
        mo_buffer_free(&req.body);
        mo_arena_free(&req_arena);
        (void)close(client_fd);
        return;
    }
    mo_process_instruction_limit(proc, MO_LIMIT_API_REQUEST_WORK);

    struct mo_heap *heap = mo_process_heap(proc);
    struct mo_host *host = mo_process_host(proc);
    if (fileno(pool->output) >= 0)
        host->output = fileno(pool->output);
    if (fileno(pool->errors) >= 0)
        host->errors = fileno(pool->errors);

    const struct mo_bytecode *bc = mo_image_bytecode(pool->image);
    const struct mo_types *types = mo_image_types(pool->image);
    const struct mo_bytecode_function *fn = &bc->functions[pool->wire_fn];
    uint32_t pcount = fn->parameter_count;
    MO_ASSERT(pcount >= 4 && pcount <= 4 + 9 + MO_LIMIT_API_ACTORS);

    struct mo_cell call_args[48] = {{{0}}};
    uint32_t built_args = 0;
    bool have_env = false;
    struct mo_cell env_map = {{0}};
    bool args_ok = true;

    args_ok = mo_value_text(heap, req.method, req.method_len, &call_args[0]);
    if (args_ok) {
        built_args = 1;
        args_ok = mo_value_text(heap, req.target, req.target_len, &call_args[1]);
    }
    if (args_ok) {
        built_args = 2;
        uint32_t list_ty = fn->parameters[2].type;
        uint32_t pair_ty = types->items[list_ty].parameters[0];
        uint32_t text_ty = types->items[pair_ty].parameters[0];
        args_ok = mo_value_list(heap, list_ty, NULL, 0, &call_args[2]);
        if (args_ok) {
            built_args = 3;
            for (uint32_t i = 0; args_ok && i < req.header_count; i++) {
                struct mo_cell kv[2];
                if (!mo_value_text(heap, req.headers[i].name, req.headers[i].name_len, &kv[0])) {
                    args_ok = false;
                    break;
                }
                if (!mo_value_text(heap, req.headers[i].value, req.headers[i].value_len, &kv[1])) {
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
        args_ok = mo_value_bytes(heap, req.body.bytes, req.body.length, &call_args[3]);
        if (args_ok)
            built_args = 4;
    }

    for (uint32_t i = 4; args_ok && i < pcount; i++) {
        uint32_t cap_ty = fn->parameters[i].type;
        enum mo_capability cap_kind = types->items[cap_ty].capability;
        if (cap_kind == MO_CAP_SIMULATION) {
            uint32_t actor_base = pcount - pool->actor_count;
            MO_ASSERT(i >= actor_base && i - actor_base < pool->actor_count);
            args_ok =
                mo_value_live_actor(heap, cap_ty, pool->actors[i - actor_base], &call_args[i]);
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
        if (mo_host_real(host, heap, cap_ty, contents, pool->folder_fd, &call_args[i], &herr) !=
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
        send_fixed_json_error(client_fd, 500, "internal_server_error");
        mo_buffer_free(&req.body);
        mo_arena_free(&req_arena);
        (void)close(client_fd);
        return;
    }

    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(proc, pool->wire_fn, call_args, pcount, &result, &defect);

    if (outcome != MO_RETURNED) {
        (void)pthread_mutex_lock(&pool->log_mutex);
        (void)fprintf(pool->errors, "mo serve: failure: %.*s %.*s\n", (int)req.method_len,
                      req.method, (int)req.target_len, req.target);
        bool wrote = mo_write_defect(pool->errors, proc, &defect);
        (void)wrote;
        (void)fflush(pool->errors);
        (void)pthread_mutex_unlock(&pool->log_mutex);
        for (uint32_t i = 0; i < pcount; i++)
            if (fn->parameters[i].ownership == MO_PARAMETER_BORROWED)
                mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
        if (have_env)
            mo_value_drop(heap, host->types->files, env_map);
        mo_process_free(proc);
        send_fixed_json_error(client_fd, 500, "internal_server_error");
        mo_buffer_free(&req.body);
        mo_arena_free(&req_arena);
        (void)close(client_fd);
        return;
    }

    for (uint32_t i = 0; i < pcount; i++)
        if (fn->parameters[i].ownership == MO_PARAMETER_BORROWED)
            mo_value_drop(heap, fn->parameters[i].type, call_args[i]);
    if (have_env)
        mo_value_drop(heap, host->types->files, env_map);

    int64_t status_code = mo_cell_get_int(mo_value_field(result, 0));
    if (status_code < 100 || status_code > 599)
        status_code = 500;
    struct mo_cell res_headers = mo_value_field(result, 1);
    struct mo_cell res_body = mo_value_field(result, 2);
    size_t body_len = 0;
    const uint8_t *body_ptr = mo_value_data(res_body, &body_len);
    bool is_head = ascii_ieq(req.method, req.method_len, "HEAD");
    bool no_body_status =
        (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 304;

    struct mo_buffer wire_out;
    mo_buffer_init(&wire_out, MO_LIMIT_API_HEADER_BYTES + 4096);
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

    char cl_hdr[96];
    int clen = no_body_status
                   ? snprintf(cl_hdr, sizeof cl_hdr, "Connection: close\r\n\r\n")
                   : snprintf(cl_hdr, sizeof cl_hdr,
                              "Content-Length: %zu\r\nConnection: close\r\n\r\n", body_len);
    wok = wok && clen > 0 && mo_buffer_append(&wire_out, cl_hdr, (size_t)clen);

    if (wok) {
        bool sent = send_all(client_fd, wire_out.bytes, wire_out.length);
        if (sent && !no_body_status && !is_head && body_len > 0)
            (void)send_all(client_fd, body_ptr, body_len);
    } else {
        send_fixed_json_error(client_fd, 500, "internal_server_error");
    }

    mo_buffer_free(&wire_out);
    mo_value_drop(heap, fn->result_type, result);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(proc);
    mo_buffer_free(&req.body);
    mo_arena_free(&req_arena);
    (void)close(client_fd);
}

static void *api_worker_main(void *arg) {
    struct api_worker_pool *pool = arg;
    for (;;) {
        MO_ASSERT(pthread_mutex_lock(&pool->queue_mutex) == 0);
        while (pool->count == 0 && !pool->stopping)
            MO_ASSERT(pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex) == 0);
        if (pool->count == 0 && pool->stopping) {
            MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
            return NULL;
        }
        int fd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % MO_LIMIT_API_QUEUE;
        pool->count--;
        MO_ASSERT(pthread_mutex_unlock(&pool->queue_mutex) == 0);
        dispatch_client_fd(pool, fd);
    }
}

static int open_api_listen_socket(uint16_t port, uint16_t *actual_port, int *err_out) {
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
    MO_ASSERT(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
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

bool mo_serve_maybe_api(const struct mo_cli *cli, FILE *output, FILE *errors, int *status_out) {
    bool is_api = false;
    if (cli->file_count == 1 && !cli->upgrade && !cli->adopt && cli->migration == NULL &&
        cli->admin_socket == NULL) {
        struct probe_work pw = {.path = mo_cli_file(cli, 0)};
        if (mo_with_stack(MO_SOURCE_STACK_SIZE, probe_entry_api, &pw) == 0)
            is_api = pw.has_api;
    }

    if (!is_api) {
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

    struct api_compile_work cw = {
        .cli = cli, .output = output, .errors = errors, .wire_fn = MO_NO_INDEX, .status = 1};
    int stack_err = mo_with_stack(MO_SOURCE_STACK_SIZE, compile_api_image, &cw);
    if (stack_err != 0) {
        *status_out =
            report_serve_start_error(output, errors, "could not prepare the source pipeline stack");
        return true;
    }
    if (cw.image == NULL) {
        *status_out = cw.status;
        return true;
    }

    struct mo_process *probe = mo_process_new(cw.image, &cli->limits);
    const char *host_err = probe == NULL ? "out of memory" : mo_host_start(mo_process_host(probe));
    mo_process_free(probe);
    if (host_err != NULL) {
        mo_image_free(cw.image);
        *status_out = report_serve_start_error(output, errors, host_err);
        return true;
    }

    int folder_fd = open(data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder_fd < 0) {
        char buf[MO_CLI_ARGUMENT_SIZE + 128];
        (void)snprintf(buf, sizeof buf, "mo serve can't open the data folder %s: %s", data_dir,
                       strerror(errno));
        mo_image_free(cw.image);
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    uint16_t requested_port = cli->has_port ? cli->port : cw.default_port;
    uint16_t actual_port = 0;
    int listen_err = 0;
    int listen_fd = open_api_listen_socket(requested_port, &actual_port, &listen_err);
    if (listen_fd < 0) {
        char buf[192];
        (void)snprintf(buf, sizeof buf, "mo serve could not listen on port %u: %s",
                       (unsigned)requested_port, strerror(listen_err));
        (void)close(folder_fd);
        mo_image_free(cw.image);
        *status_out = report_serve_start_error(output, errors, buf);
        return true;
    }

    struct mo_limits req_limits = cli->limits;
    req_limits.heap_size = MO_LIMIT_API_REQUEST_HEAP;

    struct api_worker_pool pool = {.image = cw.image,
                                   .wire_fn = cw.wire_fn,
                                   .folder_fd = folder_fd,
                                   .output = output,
                                   .errors = errors,
                                   .limits = req_limits,
                                   .actor_count = cw.actor_count};
    for (uint32_t a = 0; a < cw.actor_count; a++) {
        enum mo_service_status ast =
            mo_live_actor_new(cw.image, cw.actor_services[a], errors, &pool.actors[a]);
        if (ast != MO_SERVICE_OK) {
            char buf[192];
            (void)snprintf(buf, sizeof buf, "mo serve could not start actor: %s",
                           mo_service_status_text(ast));
            for (uint32_t prev = 0; prev < a; prev++)
                mo_live_actor_free(pool.actors[prev]);
            (void)close(listen_fd);
            (void)close(folder_fd);
            mo_image_free(cw.image);
            *status_out = report_serve_start_error(output, errors, buf);
            return true;
        }
    }
    MO_ASSERT(pthread_mutex_init(&pool.queue_mutex, NULL) == 0);
    MO_ASSERT(pthread_cond_init(&pool.queue_cond, NULL) == 0);
    MO_ASSERT(pthread_mutex_init(&pool.log_mutex, NULL) == 0);

    pthread_attr_t attr;
    MO_ASSERT(pthread_attr_init(&attr) == 0);
    (void)pthread_attr_setstacksize(&attr, 2u << 20);

    pthread_t workers[MO_LIMIT_API_WORKERS];
    uint32_t started_workers = 0;
    for (uint32_t i = 0; i < MO_LIMIT_API_WORKERS; i++) {
        if (pthread_create(&workers[i], &attr, api_worker_main, &pool) != 0)
            break;
        started_workers++;
    }
    (void)pthread_attr_destroy(&attr);

    if (started_workers == 0) {
        for (uint32_t a = 0; a < pool.actor_count; a++)
            mo_live_actor_free(pool.actors[a]);
        (void)pthread_mutex_destroy(&pool.log_mutex);
        (void)pthread_cond_destroy(&pool.queue_cond);
        (void)pthread_mutex_destroy(&pool.queue_mutex);
        (void)close(listen_fd);
        (void)close(folder_fd);
        mo_image_free(cw.image);
        *status_out = report_serve_start_error(output, errors, "mo serve could not start workers");
        return true;
    }

    if (fprintf(output, "mo serve: listening on http://127.0.0.1:%u\n", (unsigned)actual_port) <
            0 ||
        fflush(output) != 0) {
        MO_ASSERT(pthread_mutex_lock(&pool.queue_mutex) == 0);
        pool.stopping = true;
        MO_ASSERT(pthread_cond_broadcast(&pool.queue_cond) == 0);
        MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);
        for (uint32_t i = 0; i < started_workers; i++)
            (void)pthread_join(workers[i], NULL);
        for (uint32_t a = 0; a < pool.actor_count; a++)
            mo_live_actor_free(pool.actors[a]);
        (void)pthread_mutex_destroy(&pool.log_mutex);
        (void)pthread_cond_destroy(&pool.queue_cond);
        (void)pthread_mutex_destroy(&pool.queue_mutex);
        (void)close(listen_fd);
        (void)close(folder_fd);
        mo_image_free(cw.image);
        *status_out =
            report_serve_start_error(output, errors, "mo serve could not write its listening line");
        return true;
    }

    while (!mo_serve_is_signalled()) {
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
                send_fixed_json_error(cfd, 503, "service_unavailable");
                (void)close(cfd);
            }
        }
    }

    (void)close(listen_fd);
    MO_ASSERT(pthread_mutex_lock(&pool.queue_mutex) == 0);
    pool.stopping = true;
    MO_ASSERT(pthread_cond_broadcast(&pool.queue_cond) == 0);
    MO_ASSERT(pthread_mutex_unlock(&pool.queue_mutex) == 0);

    for (uint32_t i = 0; i < started_workers; i++)
        (void)pthread_join(workers[i], NULL);

    for (uint32_t a = 0; a < pool.actor_count; a++)
        mo_live_actor_free(pool.actors[a]);

    (void)fprintf(errors, "mo serve: stop: a signal asked the server to stop\n");
    (void)fflush(errors);

    (void)pthread_mutex_destroy(&pool.log_mutex);
    (void)pthread_cond_destroy(&pool.queue_cond);
    (void)pthread_mutex_destroy(&pool.queue_mutex);
    (void)close(folder_fd);
    mo_image_free(cw.image);
    *status_out = 0;
    return true;
}
