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
#include <string.h>
#include <time.h>
#include <unistd.h>

struct push_reply {
    bool failed, rate_limited;
    struct mo_buffer text, json;
    char slot[33];
};

static void reply_init(struct push_reply *r) {
    *r = (struct push_reply){0};
    mo_buffer_init(&r->text, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&r->json, MO_LIMIT_FETCH_RECEIVE_BYTES);
}

static void reply_free(struct push_reply *r) {
    mo_buffer_free(&r->text);
    mo_buffer_free(&r->json);
}

static bool reply_fields(struct mo_cell output, struct push_reply *out) {
    struct mo_cell item;
    size_t size;
    const uint8_t *bytes;
    MO_ASSERT(mo_list_get(output, 0, &item));
    bytes = mo_value_data(item, &size);
    out->failed = size == 4 && memcmp(bytes, "true", 4) == 0;
    mo_buffer_clear(&out->text);
    MO_ASSERT(mo_list_get(output, 1, &item));
    bytes = mo_value_data(item, &size);
    bool ok = mo_buffer_append(&out->text, bytes, size);
    mo_buffer_clear(&out->json);
    MO_ASSERT(mo_list_get(output, 2, &item));
    bytes = mo_value_data(item, &size);
    ok = ok && mo_buffer_append(&out->json, bytes, size);
    MO_ASSERT(mo_list_get(output, 3, &item));
    bytes = mo_value_data(item, &size);
    ok = ok && (size == 0 || size == 32) && memchr(bytes, 0, size) == NULL;
    if (ok) {
        memcpy(out->slot, bytes, size);
        out->slot[size] = 0;
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
            ok = (mo_list_size(output) == 4 && reply_fields(output, out)) || out->rate_limited;
            mo_value_drop(heap, fn->result_type, output);
        }
    }
    mo_process_free(process);
    return ok;
}

static bool overview_read(struct mo_image *image, uint32_t function, const struct mo_buffer *wire,
                          char base_out[65], bool *none, bool *rate_limited) {
    *none = false;
    *rate_limited = false;
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
            size_t size;
            const uint8_t *bytes = mo_value_data(output, &size);
            if (size == 12 && memcmp(bytes, "rate_limited", 12) == 0) {
                *rate_limited = true;
            } else if (size == 4 && memcmp(bytes, "none", 4) == 0) {
                *none = true;
            } else if (size == 64 && memchr(bytes, 0, 64) == NULL) {
                memcpy(base_out, bytes, 64);
                base_out[64] = 0;
                uint8_t check[32];
                ok = mo_fetch_unhex(base_out, check, 32);
            } else {
                ok = false;
            }
            mo_value_drop(heap, fn->result_type, output);
        }
    }
    mo_process_free(process);
    return ok;
}

static bool admin_rpc(struct fetch_endpoint *e, const char *tool, const char *arguments,
                      struct mo_buffer *wire) {
    uint64_t interval = UINT64_C(1000000) / MO_LIMIT_MCP_RATE + 5000;
    uint64_t elapsed = mo_fetch_now() - e->last_sent;
    if (e->last_sent != 0 && elapsed < interval) {
        struct timespec pause = {.tv_nsec = (long)((interval - elapsed) * 1000)};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return false;
    }
    if (mo_fetch_cancelled || mo_fetch_now() >= e->end)
        return false;
    struct mo_buffer request;
    mo_buffer_init(&request, MO_LIMIT_MCP_INGRESS_BYTES);
    size_t body = strlen("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
                         "\"name\":\"\",\"arguments\":}}") +
                  strlen(tool) + strlen(arguments);
    bool ok =
        mo_buffer_append_text(&request, "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                                        "application/json\r\nMCP-Protocol-Version: "
                                        "2025-11-25\r\nConnection: close\r\nContent-Length: ") &&
        mo_buffer_append_uint(&request, body) &&
        mo_buffer_append_text(&request, "\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                        "\"tools/call\",\"params\":{\"name\":\"") &&
        mo_buffer_append_text(&request, tool) &&
        mo_buffer_append_text(&request, "\",\"arguments\":") &&
        mo_buffer_append_text(&request, arguments) && mo_buffer_append_text(&request, "}}");
    int fd = ok ? mo_fetch_connect_unix(e->socket_path, e->end) : -1;
    ok = fd >= 0 && mo_fetch_send_request(fd, (const char *)request.bytes, request.length, e->end);
    mo_buffer_free(&request);
    if (ok) {
        e->last_sent = mo_fetch_now();
        mo_buffer_clear(wire);
        ok = mo_fetch_receive_response(fd, wire, e->end);
    }
    if (fd >= 0)
        (void)close(fd);
    return ok;
}

/* Only the explicit no-effect rate refusal is retried. Lost replies remain uncertain. */
static bool push_call(struct fetch_endpoint *e, const char *tool, const char *arguments,
                      struct mo_image *image, uint32_t function, struct push_reply *out) {
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_FETCH_RECEIVE_BYTES);
    wire.arena.confidential = true;
    bool ok;
    do {
        ok = admin_rpc(e, tool, arguments, &wire) && push_read(image, function, &wire, out);
    } while (ok && out->rate_limited);
    mo_buffer_free(&wire);
    return ok;
}

static bool overview_call(struct fetch_endpoint *e, struct mo_image *image, uint32_t function,
                          char base_out[65], bool *none) {
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_FETCH_RECEIVE_BYTES);
    wire.arena.confidential = true;
    bool ok, rate_limited = false;
    do {
        ok = admin_rpc(e, "mo-overview", "{}", &wire) &&
             overview_read(image, function, &wire, base_out, none, &rate_limited);
    } while (ok && rate_limited);
    mo_buffer_free(&wire);
    return ok;
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
    return !ok || r->failed ? 1 : 0;
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

static bool readers(struct mo_image *image, uint32_t *upgrade_fn, uint32_t *overview_fn) {
    *upgrade_fn = MO_NO_INDEX;
    *overview_fn = MO_NO_INDEX;
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_defect initialization;
    if (image == NULL || mo_image_init(image, &limits, &initialization) != MO_RETURNED)
        return false;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 16 && memcmp(name->bytes, "McpFetch.upgrade", 16) == 0)
            *upgrade_fn = i;
        else if (name->length == 24 && memcmp(name->bytes, "McpFetch.overview_source", 24) == 0)
            *overview_fn = i;
    }
    MO_ASSERT(*upgrade_fn != MO_NO_INDEX && *overview_fn != MO_NO_INDEX);
    return *upgrade_fn != MO_NO_INDEX && *overview_fn != MO_NO_INDEX;
}

static int push_attempt(const struct mo_cli *cli, FILE *output, FILE *errors,
                        const char *socket_path, uint64_t end) {
    char active_base[65] = "";
    if (cli->push_base != NULL) {
        uint8_t base[32];
        if (!mo_fetch_unhex(cli->push_base, base, 32))
            return push_failure(cli, output, errors,
                                "--base needs the 64-digit lowercase source digest of the active "
                                "program",
                                2);
        memcpy(active_base, cli->push_base, 65);
    }
    struct mo_diags diags;
    struct mo_sources sources = {0};
    struct mo_buffer bundle;
    mo_buffer_init(&bundle, MO_LIMIT_BUNDLE_BYTES);
    if (!mo_diags_init(&diags))
        return 1;
    int status = 1;
    struct mo_image *image = NULL;
    if (!mo_sources_init(&sources) ||
        !mo_upgrade_bundle_with_library(mo_cli_file(cli, 0), cli->library, true, true, &bundle,
                                        &diags, &sources)) {
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
    uint32_t upgrade_fn = MO_NO_INDEX, overview_fn = MO_NO_INDEX;
    if (!readers(image, &upgrade_fn, &overview_fn)) {
        status = push_failure(cli, output, errors, "reader unavailable", 1);
        goto done;
    }
    struct fetch_endpoint endpoint = {.socket_path = socket_path, .end = end};
    if (cli->push_base == NULL) {
        bool none = false;
        if (!overview_call(&endpoint, image, overview_fn, active_base, &none)) {
            status = push_failure(cli, output, errors,
                                  "the service did not answer on the admin socket; nothing was "
                                  "changed",
                                  1);
            goto done;
        }
        if (none) {
            status = push_failure(cli, output, errors,
                                  "the active service has no recorded source bundle; restart with "
                                  "source first",
                                  1);
            goto done;
        }
    }
    struct push_reply reply;
    reply_init(&reply);
    struct mo_buffer arguments;
    mo_buffer_init(&arguments, MO_LIMIT_MCP_BODY_BYTES);
    bool ok = mo_buffer_append_text(&arguments, "{\"base\":\"") &&
              mo_buffer_append_text(&arguments, active_base) &&
              mo_buffer_append_text(&arguments, "\",\"candidate\":\"") &&
              mo_buffer_append_text(&arguments, candidate) &&
              mo_buffer_append_text(&arguments, "\",\"total\":") &&
              mo_buffer_append_uint(&arguments, bundle.length);
    if (ok && cli->migration != NULL)
        ok = mo_buffer_append_text(&arguments, ",\"migration\":") &&
             mo_render_json_text(&arguments, cli->migration, strlen(cli->migration));
    ok = ok && mo_buffer_append_byte(&arguments, '}') && mo_buffer_append_byte(&arguments, 0);
    bool answered = ok && push_call(&endpoint, "mo-upgrade-open", (const char *)arguments.bytes,
                                    image, upgrade_fn, &reply);
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
            push_call(&endpoint, "mo-upgrade-page", (const char *)arguments.bytes, image,
                      upgrade_fn, &reply);
        proceed = answered && !reply.failed;
    }
    bool committing = proceed;
    if (proceed) {
        mo_buffer_clear(&arguments);
        answered = mo_buffer_append_text(&arguments, "{\"slot\":\"") &&
                   mo_buffer_append_text(&arguments, slot) &&
                   mo_buffer_append_text(&arguments, "\",\"base\":\"") &&
                   mo_buffer_append_text(&arguments, active_base) &&
                   mo_buffer_append_text(&arguments, "\"}") &&
                   mo_buffer_append_byte(&arguments, 0) &&
                   push_call(&endpoint, "mo-upgrade-commit", (const char *)arguments.bytes, image,
                             upgrade_fn, &reply);
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

int mo_push(const struct mo_cli *cli, FILE *output, FILE *errors) {
    return mo_fetch_client(cli, output, errors, "push", push_attempt);
}
