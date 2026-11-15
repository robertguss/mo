#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/bundle.h"
#include "base/sha256.h"
#include "base/upload.h"
#include "tools/fetch.h"
#include "tools/protocol.h"
#include "tools/upgrade.h"
#include "vm/vm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct push_reply {
    bool failed, rate_limited, clipped;
    struct mo_buffer text, json, difference, bytes;
    char slot[33], next[21], review[65], base[65], candidate[65],
        migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1], version[21], total[21];
};

static void reply_init(struct push_reply *r) {
    *r = (struct push_reply){0};
    mo_buffer_init(&r->text, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&r->json, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&r->difference, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&r->bytes, MO_LIMIT_FETCH_RECEIVE_BYTES);
}

static void reply_free(struct push_reply *r) {
    mo_buffer_free(&r->text);
    mo_buffer_free(&r->json);
    mo_buffer_free(&r->difference);
    mo_buffer_free(&r->bytes);
}

static bool reply_fields(struct mo_cell output, struct push_reply *out) {
    struct mo_buffer *buffers[] = {&out->text, &out->json, &out->difference, &out->bytes};
    const uint32_t buffer_at[] = {1, 2, 10, 13};
    struct {
        uint32_t at;
        char *to;
        size_t size, exact;
    } texts[] = {{3, out->slot, sizeof out->slot, 32},
                 {4, out->next, sizeof out->next, 0},
                 {5, out->review, sizeof out->review, 64},
                 {6, out->base, sizeof out->base, 64},
                 {7, out->candidate, sizeof out->candidate, 64},
                 {8, out->migration, sizeof out->migration, 0},
                 {9, out->version, sizeof out->version, 0},
                 {12, out->total, sizeof out->total, 0}};
    struct mo_cell item;
    size_t size;
    const uint8_t *bytes;
    MO_ASSERT(mo_list_get(output, 0, &item));
    bytes = mo_value_data(item, &size);
    out->failed = size == 4 && memcmp(bytes, "true", 4) == 0;
    MO_ASSERT(mo_list_get(output, 11, &item));
    bytes = mo_value_data(item, &size);
    out->clipped = size == 4 && memcmp(bytes, "true", 4) == 0;
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof buffers / sizeof *buffers; i++) {
        mo_buffer_clear(buffers[i]);
        MO_ASSERT(mo_list_get(output, buffer_at[i], &item));
        bytes = mo_value_data(item, &size);
        ok = mo_buffer_append(buffers[i], bytes, size);
    }
    for (size_t i = 0; ok && i < sizeof texts / sizeof *texts; i++) {
        MO_ASSERT(mo_list_get(output, texts[i].at, &item));
        bytes = mo_value_data(item, &size);
        ok = size < texts[i].size && (texts[i].exact == 0 || size == 0 || size == texts[i].exact) &&
             memchr(bytes, 0, size) == NULL;
        if (ok) {
            memcpy(texts[i].to, bytes, size);
            texts[i].to[size] = 0;
        }
    }
    return ok;
}

static bool push_read(struct mo_image *image, uint32_t function, const struct mo_buffer *wire,
                      struct push_reply *out) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_process *process = mo_process_new(image, &limits);
    if (process == NULL)
        return false;
    mo_process_confidential(process);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell input, output;
    bool ok = mo_value_text(heap, (const char *)wire->bytes, wire->length, &input);
    if (ok) {
        struct mo_defect defect;
        mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
        ok = mo_call(process, function, &input, 1, &output, &defect) == MO_RETURNED;
        const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[function];
        if (fn->parameters[0].ownership != MO_PARAMETER_OWNED)
            mo_value_drop(heap, fn->parameters[0].type, input);
        if (ok) {
            out->rate_limited = false;
            if (mo_list_size(output) == 1) {
                struct mo_cell item;
                MO_ASSERT(mo_list_get(output, 0, &item));
                size_t length;
                const uint8_t *text = mo_value_data(item, &length);
                out->rate_limited = length == 12 && memcmp(text, "rate_limited", 12) == 0;
            }
            ok = (mo_list_size(output) == 14 && reply_fields(output, out)) || out->rate_limited;
            mo_value_drop(heap, fn->result_type, output);
        }
    }
    mo_process_free(process);
    return ok;
}

static bool push_call_once(struct fetch_endpoint *e, const char *upgrade, const char *tool,
                           const char *arguments, struct mo_image *image, uint32_t function,
                           struct push_reply *out) {
    uint64_t interval = UINT64_C(1000000) / MO_LIMIT_MCP_RATE + 5000;
    uint64_t elapsed = mo_fetch_now() - e->last_sent;
    if (e->last_sent != 0 && elapsed < interval) {
        struct timespec pause = {.tv_nsec = (long)((interval - elapsed) * 1000)};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return false;
    }
    if (mo_fetch_cancelled || mo_fetch_now() >= e->end)
        return false;
    struct mo_buffer request, wire;
    mo_buffer_init(&request, MO_LIMIT_MCP_INGRESS_BYTES);
    request.arena.confidential = true;
    mo_buffer_init(&wire, MO_LIMIT_FETCH_RECEIVE_BYTES);
    wire.arena.confidential = true;
    char head[160];
    size_t body = strlen("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
                         "\"name\":\"\",\"arguments\":}}") +
                  strlen(tool) + strlen(arguments);
    int n = snprintf(head, sizeof head, "POST /mcp HTTP/1.1\r\nHost: %s:%s\r\n",
                     strcmp(e->host, "::1") == 0 ? "[::1]" : e->host, e->port);
    bool ok =
        n > 0 && (size_t)n < sizeof head && mo_buffer_append(&request, head, (size_t)n) &&
        mo_buffer_append_text(&request, "Authorization: Bearer ") &&
        mo_buffer_append_text(&request, e->token) && mo_buffer_append_text(&request, "\r\n") &&
        mo_buffer_append_text(&request, e->header) &&
        mo_buffer_append_text(&request, ": Bearer ") && mo_buffer_append_text(&request, upgrade) &&
        mo_buffer_append_text(&request,
                              "\r\nContent-Type: application/json\r\nMCP-Protocol-Version: "
                              "2025-11-25\r\nConnection: close\r\nContent-Length: ") &&
        mo_buffer_append_uint(&request, body) &&
        mo_buffer_append_text(&request, "\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                        "\"tools/call\",\"params\":{\"name\":\"") &&
        mo_buffer_append_text(&request, tool) &&
        mo_buffer_append_text(&request, "\",\"arguments\":") &&
        mo_buffer_append_text(&request, arguments) && mo_buffer_append_text(&request, "}}");
    int fd = ok ? mo_fetch_connect_local(e->host, e->port, e->end) : -1;
    ok = fd >= 0 && mo_fetch_send_request(fd, (const char *)request.bytes, request.length, e->end);
    mo_host_wipe(request.bytes, request.length);
    mo_buffer_free(&request);
    if (ok) {
        e->last_sent = mo_fetch_now();
        ok = mo_fetch_receive_response(fd, &wire, e->end) && push_read(image, function, &wire, out);
    }
    if (fd >= 0)
        (void)close(fd);
    mo_buffer_free(&wire);
    return ok;
}

/* Only the explicit no-effect rate refusal is retried. Lost replies remain uncertain.
 * The existing whole-command deadline bounds retries and credential exposure. */
static bool push_call(struct fetch_endpoint *e, const char *upgrade, const char *tool,
                      const char *arguments, struct mo_image *image, uint32_t function,
                      struct push_reply *out) {
    do {
        if (!push_call_once(e, upgrade, tool, arguments, image, function, out))
            return false;
    } while (out->rate_limited);
    return true;
}

static bool credential(const char *name, const char **out) {
    const char *value = getenv(name);
    size_t size = value == NULL ? 0 : strnlen(value, MO_LIMIT_MCP_TOKEN_BYTES + 1);
    if (size == 0 || size > MO_LIMIT_MCP_TOKEN_BYTES)
        return false;
    for (size_t i = 0; i < size; i++)
        if ((unsigned char)value[i] <= 32 || (unsigned char)value[i] >= 127)
            return false;
    *out = value;
    return true;
}

/* Prints the service's report: the text form, or the structured JSON with --json. Refusals go
 * to the error stream unless JSON was asked for. */
static int push_report(const struct mo_cli *cli, FILE *output, FILE *errors,
                       const struct push_reply *r) {
    const struct mo_buffer *shown = cli->json ? &r->json : &r->text;
    FILE *stream = cli->json || !r->failed ? output : errors;
    bool ok = fwrite(shown->bytes, 1, shown->length, stream) == shown->length &&
              (shown->length != 0 && shown->bytes[shown->length - 1] == '\n'
                   ? true
                   : fputc('\n', stream) != EOF) &&
              fflush(stream) == 0;
    return !ok || r->failed ? 1 : r->review[0] != 0 ? 3 : 0;
}

/* A failure on the client's side: the message on the error stream, or with --json a report on
 * standard output so it stays parseable. */
static int push_failure(const struct mo_cli *cli, FILE *output, FILE *errors, const char *message,
                        int status) {
    if (!cli->json) {
        (void)fprintf(errors, "mo push: %s\n", message);
        return status;
    }
    struct mo_buffer json;
    mo_buffer_init(&json, 1024);
    bool ok = mo_buffer_append_text(&json, "{\"version\":1,\"status\":\"refused\",\"code\":null,"
                                           "\"class\":\"client\",\"message\":") &&
              mo_render_json_text(&json, message, strlen(message)) &&
              mo_buffer_append_text(&json, "}\n") &&
              fwrite(json.bytes, 1, json.length, output) == json.length && fflush(output) == 0;
    mo_buffer_free(&json);
    return ok ? status : 1;
}

static uint32_t reader(struct mo_image *image) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_defect initialization;
    if (image == NULL || mo_image_init(image, &limits, &initialization) != MO_RETURNED)
        return MO_NO_INDEX;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    uint32_t function = MO_NO_INDEX;
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 16 && memcmp(name->bytes, "McpFetch.upgrade", 16) == 0)
            function = i;
    }
    MO_ASSERT(function != MO_NO_INDEX);
    return function;
}

static int push_attempt(const struct mo_cli *cli, FILE *output, FILE *errors, const char *host,
                        const char *port, uint64_t end) {
    uint8_t base[32];
    if (!mo_fetch_unhex(cli->push_base, base, 32))
        return push_failure(cli, output, errors,
                            "--base needs the 64-digit lowercase source digest of the active "
                            "program",
                            2);
    const char *token = NULL, *upgrade = NULL;
    if (!credential(cli->fetch_token, &token) || !credential(cli->upgrade_token, &upgrade))
        return push_failure(cli, output, errors, "token unavailable or invalid", 1);
    struct mo_diags diags;
    struct mo_sources sources = {0};
    struct mo_buffer bundle;
    mo_buffer_init(&bundle, MO_LIMIT_BUNDLE_BYTES);
    if (!mo_diags_init(&diags))
        return 1;
    int status = 1;
    struct mo_image *image = NULL;
    if (!mo_sources_init(&sources) ||
        !mo_upgrade_bundle(mo_cli_file(cli, 0), true, true, &bundle, &diags, &sources)) {
        status = mo_write_diagnostics(output, errors, cli->json, MO_COMMAND_PUSH, &sources, &diags,
                                      NULL);
        status = status == 0 ? 1 : status;
        goto done;
    }
    uint8_t digest[32];
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bundle.bytes, bundle.length);
    mo_sha256_finish(&hash, digest);
    char candidate[65];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        candidate[2 * i] = digits[digest[i] >> 4];
        candidate[2 * i + 1] = digits[digest[i] & 15];
    }
    candidate[64] = 0;
    image = mo_protocol_build();
    uint32_t function = reader(image);
    if (function == MO_NO_INDEX) {
        status = push_failure(cli, output, errors, "reader unavailable", 1);
        goto done;
    }
    struct fetch_endpoint endpoint = {.host = host,
                                      .port = port,
                                      .token = token,
                                      .header = "Mo-Upgrade-Authorization",
                                      .end = end};
    struct push_reply reply;
    reply_init(&reply);
    struct mo_buffer arguments;
    mo_buffer_init(&arguments, MO_LIMIT_MCP_BODY_BYTES);
    bool ok = mo_buffer_append_text(&arguments, "{\"base\":\"") &&
              mo_buffer_append_text(&arguments, cli->push_base) &&
              mo_buffer_append_text(&arguments, "\",\"candidate\":\"") &&
              mo_buffer_append_text(&arguments, candidate) &&
              mo_buffer_append_text(&arguments, "\",\"total\":") &&
              mo_buffer_append_uint(&arguments, bundle.length);
    if (ok && cli->migration != NULL)
        ok = mo_buffer_append_text(&arguments, ",\"migration\":") &&
             mo_render_json_text(&arguments, cli->migration, strlen(cli->migration));
    ok = ok && mo_buffer_append_byte(&arguments, '}') && mo_buffer_append_byte(&arguments, 0);
    bool answered = ok && push_call(&endpoint, upgrade, "mo-upgrade-open",
                                    (const char *)arguments.bytes, image, function, &reply);
    char slot[33] = "";
    bool proceed = answered && !reply.failed && reply.slot[0] != 0;
    if (proceed)
        memcpy(slot, reply.slot, sizeof slot);
    size_t pages = (bundle.length + MO_LIMIT_UPGRADE_PAGE_BYTES - 1) / MO_LIMIT_UPGRADE_PAGE_BYTES;
    char text[MO_LIMIT_UPGRADE_PAGE_TEXT + 1];
    for (size_t i = 0; proceed && i < pages; i++) {
        size_t offset = i * MO_LIMIT_UPGRADE_PAGE_BYTES;
        size_t length = bundle.length - offset < MO_LIMIT_UPGRADE_PAGE_BYTES
                            ? bundle.length - offset
                            : MO_LIMIT_UPGRADE_PAGE_BYTES;
        mo_upload_text(bundle.bytes + offset, length, text);
        mo_buffer_clear(&arguments);
        char numbers[96];
        int n = snprintf(numbers, sizeof numbers, "\",\"index\":%zu,\"offset\":%zu,\"total\":%zu",
                         i, offset, bundle.length);
        answered =
            n > 0 && (size_t)n < sizeof numbers &&
            mo_buffer_append_text(&arguments, "{\"slot\":\"") &&
            mo_buffer_append_text(&arguments, slot) && mo_buffer_append_text(&arguments, numbers) &&
            mo_buffer_append_text(&arguments, ",\"bytes\":\"") &&
            mo_buffer_append_text(&arguments, text) && mo_buffer_append_text(&arguments, "\"}") &&
            mo_buffer_append_byte(&arguments, 0) &&
            push_call(&endpoint, upgrade, "mo-upgrade-page", (const char *)arguments.bytes, image,
                      function, &reply);
        proceed = answered && !reply.failed;
    }
    bool committing = proceed;
    if (proceed) {
        mo_buffer_clear(&arguments);
        answered = mo_buffer_append_text(&arguments, "{\"slot\":\"") &&
                   mo_buffer_append_text(&arguments, slot) &&
                   mo_buffer_append_text(&arguments, "\",\"base\":\"") &&
                   mo_buffer_append_text(&arguments, cli->push_base) &&
                   mo_buffer_append_text(&arguments, "\"}") &&
                   mo_buffer_append_byte(&arguments, 0) &&
                   push_call(&endpoint, upgrade, "mo-upgrade-commit", (const char *)arguments.bytes,
                             image, function, &reply);
    }
    if (answered)
        status = push_report(cli, output, errors, &reply);
    else
        /* Only the commit can change the service; an earlier failure changed nothing. */
        status = push_failure(cli, output, errors,
                              committing ? "the service did not answer the commit; the upgrade "
                                           "may or may not have been applied; push again to "
                                           "learn which program is active"
                                         : "the service did not answer or refused the request "
                                           "before the commit; nothing was changed",
                              1);
    mo_buffer_free(&arguments);
    reply_free(&reply);
done:
    if (image != NULL)
        mo_image_free(image);
    mo_buffer_free(&bundle);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    return status;
}

static int review_failure(FILE *errors, const char *message, int status) {
    (void)fprintf(errors, "mo review: %s\n", message);
    return status;
}

static bool review_tree(struct fetch_endpoint *e, const char *approval, struct mo_image *image,
                        uint32_t function, struct push_reply *reply, const char *review,
                        const char *source, uint8_t *tree, size_t *length) {
    struct mo_upload upload;
    for (uint64_t index = 0; index <= MO_LIMIT_BUNDLE_BYTES / MO_LIMIT_UPGRADE_PAGE_BYTES;
         index++) {
        uint64_t offset = index * MO_LIMIT_UPGRADE_PAGE_BYTES;
        char arguments[160], *end = NULL;
        int n = snprintf(arguments, sizeof arguments,
                         "{\"review\":\"%s\",\"source\":\"%s\",\"offset\":%llu}", review, source,
                         (unsigned long long)offset);
        if (n <= 0 || (size_t)n >= sizeof arguments ||
            !push_call(e, approval, "mo-review-source", arguments, image, function, reply) ||
            reply->failed || reply->total[0] < '1' || reply->total[0] > '9')
            return false;
        errno = 0;
        unsigned long long total = strtoull(reply->total, &end, 10);
        if (errno != 0 || *end != 0 || total > MO_LIMIT_BUNDLE_BYTES)
            return false;
        if (index == 0)
            mo_upload_init(&upload, tree, (size_t)total);
        enum mo_upload_status status =
            mo_upload_page(&upload, index, offset, total, reply->bytes.bytes, reply->bytes.length);
        if (status == MO_UPLOAD_COMPLETE) {
            *length = (size_t)total;
            return true;
        }
        if (status != MO_UPLOAD_MORE)
            return false;
    }
    return false;
}

static bool write_tree(const char *root, const uint8_t *bytes, size_t length) {
    struct mo_bundle bundle;
    if (!mo_bundle_read(bytes, length, &bundle) || mkdir(root, 0700) != 0)
        return false;
    bool ok = true;
    for (uint32_t i = 0; ok && i < bundle.count; i++) {
        const struct mo_bundle_file *file = &bundle.files[i];
        char path[MO_LIMIT_PATH_SIZE + MO_LIMIT_BUNDLE_PATH_BYTES + 16];
        int n = snprintf(path, sizeof path, "%s/%.*s", root, (int)file->path.length,
                         (const char *)file->path.bytes);
        ok = n > 0 && (size_t)n < sizeof path;
        for (size_t at = strlen(root) + 1; ok && path[at] != 0; at++)
            if (path[at] == '/') {
                path[at] = 0;
                ok = mkdir(path, 0700) == 0 || errno == EEXIST;
                path[at] = '/';
            }
        int fd = ok ? open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
        size_t written = 0;
        while (fd >= 0 && written < file->source.length) {
            ssize_t step = write(fd, file->source.bytes + written, file->source.length - written);
            if (step <= 0)
                break;
            written += (size_t)step;
        }
        ok = fd >= 0 && written == file->source.length && close(fd) == 0;
    }
    return ok;
}

struct review_shown {
    char review[65], base[65], candidate[65], migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1],
        version[21];
    struct mo_buffer difference, text;
    struct mo_arena arena;
    uint8_t *pending, *active;
    size_t pending_size, active_size;
};

static bool review_verified(const struct review_shown *r) {
    uint8_t id[32], base[32], candidate[32], digest[32], computed[32];
    char *end = NULL;
    errno = 0;
    unsigned long long version = strtoull(r->version, &end, 10);
    if (r->version[0] < '0' || r->version[0] > '9' || *end != 0 || errno != 0 ||
        !mo_fetch_unhex(r->review, id, 32) || !mo_fetch_unhex(r->base, base, 32) ||
        !mo_fetch_unhex(r->candidate, candidate, 32))
        return false;
    mo_door_review_id(base, candidate, r->migration, version, r->difference.bytes,
                      r->difference.length, computed);
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, r->pending, r->pending_size);
    mo_sha256_finish(&hash, digest);
    bool ok = memcmp(digest, candidate, 32) == 0 && memcmp(computed, id, 32) == 0;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, r->active, r->active_size);
    mo_sha256_finish(&hash, digest);
    return ok && memcmp(digest, base, 32) == 0;
}

static int review_show(const struct mo_cli *cli, FILE *output, FILE *errors,
                       struct fetch_endpoint *e, const char *approval, struct mo_image *image,
                       uint32_t function, struct push_reply *reply) {
    if (!push_call(e, approval, "mo-review-pending", "{}", image, function, reply))
        return review_failure(errors, "the service did not answer", 1);
    if (reply->failed || reply->review[0] == 0)
        return push_report(cli, output, errors, reply);
    struct review_shown r;
    memcpy(r.review, reply->review, sizeof r.review);
    memcpy(r.base, reply->base, sizeof r.base);
    memcpy(r.candidate, reply->candidate, sizeof r.candidate);
    memcpy(r.migration, reply->migration, sizeof r.migration);
    memcpy(r.version, reply->version, sizeof r.version);
    mo_buffer_init(&r.difference, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&r.text, 2 * MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_arena_init(&r.arena, 2 * (size_t)MO_LIMIT_BUNDLE_BYTES + 2 * MO_ARENA_CHUNK_HEADER);
    r.pending = mo_arena_allocate(&r.arena, MO_LIMIT_BUNDLE_BYTES, 1);
    r.active = mo_arena_allocate(&r.arena, MO_LIMIT_BUNDLE_BYTES, 1);
    bool clipped = reply->clipped;
    bool ok = r.pending != NULL && r.active != NULL &&
              mo_buffer_append(&r.difference, reply->difference.bytes, reply->difference.length) &&
              mo_buffer_append_text(&r.text, "base ") && mo_buffer_append_text(&r.text, r.base) &&
              mo_buffer_append_text(&r.text, "\ncandidate ") &&
              mo_buffer_append_text(&r.text, r.candidate) &&
              mo_buffer_append_text(&r.text, "\nmigration ") &&
              (r.migration[0] == 0
                   ? mo_buffer_append_text(&r.text, "none")
                   : mo_render_terminal_text(&r.text, r.migration, strlen(r.migration))) &&
              mo_buffer_append_text(&r.text, "\napproval version ") &&
              mo_buffer_append_text(&r.text, r.version) && mo_buffer_append_byte(&r.text, '\n') &&
              mo_door_lines(&r.text, r.difference.bytes, r.difference.length) &&
              fwrite(r.text.bytes, 1, r.text.length, output) == r.text.length &&
              fflush(output) == 0;
    int status = ok ? 0 : review_failure(errors, "could not show the intent difference", 1);
    if (status == 0 && clipped)
        status = review_failure(errors,
                                "the intent difference is too large for one page, so no review "
                                "id can be checked or offered",
                                1);
    if (status == 0 && (!review_tree(e, approval, image, function, reply, r.review, "pending",
                                     r.pending, &r.pending_size) ||
                        !review_tree(e, approval, image, function, reply, r.review, "active",
                                     r.active, &r.active_size))) {
        if (reply->failed)
            (void)push_report(cli, output, errors, reply);
        status = review_failure(errors,
                                "the held upgrade changed or did not arrive whole while it was "
                                "read; no review id is offered",
                                1);
    }
    if (status == 0 && !review_verified(&r))
        status = review_failure(errors,
                                "the source or the intent difference does not match the review "
                                "id; no review id is offered",
                                1);
    char pending[MO_LIMIT_PATH_SIZE + 16], active[MO_LIMIT_PATH_SIZE + 16];
    int n = snprintf(pending, sizeof pending, "%s/pending", cli->output);
    int m = snprintf(active, sizeof active, "%s/active", cli->output);
    if (status == 0 &&
        (n <= 0 || (size_t)n >= sizeof pending || m <= 0 || (size_t)m >= sizeof active ||
         (mkdir(cli->output, 0700) != 0 && errno != EEXIST) ||
         !write_tree(pending, r.pending, r.pending_size) ||
         !write_tree(active, r.active, r.active_size)))
        status = review_failure(errors,
                                "could not write the source trees; DIR/pending and DIR/active must "
                                "not exist yet",
                                1);
    if (status == 0 && (fprintf(output, "pending source in %s\nactive source in %s\nreview %s\n",
                                pending, active, r.review) < 0 ||
                        fflush(output) != 0))
        status = 1;
    mo_buffer_free(&r.difference);
    mo_buffer_free(&r.text);
    mo_arena_free(&r.arena);
    return status;
}

static int review_attempt(const struct mo_cli *cli, FILE *output, FILE *errors, const char *host,
                          const char *port, uint64_t end) {
    uint8_t digest[32];
    if (cli->approve_count != 0 &&
        (cli->push_base == NULL || !mo_fetch_unhex(cli->push_base, digest, 32)))
        return review_failure(errors,
                              "--approve needs --base with the 64-digit lowercase source digest "
                              "of the active program",
                              2);
    for (uint32_t i = 0; i < cli->approve_count; i++)
        if (!mo_fetch_unhex(cli->approve[i], digest, 32))
            return review_failure(errors, "--approve needs 64-digit lowercase item digests", 2);
    if (cli->answer != NULL && !mo_fetch_unhex(cli->review, digest, 32))
        return review_failure(errors, "--yes and --no need the 64-digit lowercase review id", 2);
    const char *token = NULL, *approval = NULL;
    if (!credential(cli->fetch_token, &token) || !credential(cli->approval_token, &approval))
        return review_failure(errors, "token unavailable or invalid", 1);
    struct mo_image *image = mo_protocol_build();
    uint32_t function = reader(image);
    if (function == MO_NO_INDEX) {
        if (image != NULL)
            mo_image_free(image);
        return review_failure(errors, "reader unavailable", 1);
    }
    struct fetch_endpoint endpoint = {.host = host,
                                      .port = port,
                                      .token = token,
                                      .header = "Mo-Approval-Authorization",
                                      .end = end};
    struct push_reply reply;
    reply_init(&reply);
    if (cli->output != NULL) {
        int shown = review_show(cli, output, errors, &endpoint, approval, image, function, &reply);
        reply_free(&reply);
        mo_image_free(image);
        return shown;
    }
    const char *tool = cli->items             ? "mo-intent-items"
                       : cli->setting != NULL ? "mo-review-setting"
                       : cli->answer != NULL  ? "mo-review-answer"
                                              : "mo-intent-approve";
    char arguments[256];
    int status = 0;
    bool more = true;
    for (uint32_t i = 0; status == 0 && more; i++) {
        int n =
            cli->items ? snprintf(arguments, sizeof arguments, "{\"offset\":%s}",
                                  i == 0 ? "0" : reply.next)
            : cli->setting != NULL
                ? snprintf(arguments, sizeof arguments, "{\"setting\":\"%s\"}", cli->setting)
            : cli->answer != NULL
                ? snprintf(arguments, sizeof arguments, "{\"review\":\"%s\",\"answer\":\"%s\"}",
                           cli->review, cli->answer)
                : snprintf(arguments, sizeof arguments, "{\"program\":\"%s\",\"digest\":\"%s\"}",
                           cli->push_base, cli->approve[i]);
        if (n <= 0 || (size_t)n >= sizeof arguments ||
            !push_call(&endpoint, approval, tool, arguments, image, function, &reply))
            status = review_failure(errors,
                                    "the service did not answer; run mo review --items to see "
                                    "what it recorded",
                                    1);
        else
            status = push_report(cli, output, errors, &reply);
        more = cli->items ? reply.next[0] != 0 : i + 1 < cli->approve_count;
    }
    reply_free(&reply);
    mo_image_free(image);
    return status;
}

int mo_push(const struct mo_cli *cli, FILE *output, FILE *errors) {
    return mo_fetch_client(cli, output, errors, "push", push_attempt);
}

int mo_review(const struct mo_cli *cli, FILE *output, FILE *errors) {
    return mo_fetch_client(cli, output, errors, "review", review_attempt);
}
