#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/sha256.h"
#include "tools/fetch.h"
#include "tools/protocol.h"
#include "tools/upgrade.h"
#include "vm/vm.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct push_reply {
    bool failed, rate_limited;
    struct mo_buffer text, json;
    char slot[33], next[21];
};

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
            ok = mo_list_size(output) == 5;
            struct mo_cell item;
            size_t size;
            const uint8_t *bytes;
            if (ok) {
                MO_ASSERT(mo_list_get(output, 0, &item));
                bytes = mo_value_data(item, &size);
                out->failed = size == 4 && memcmp(bytes, "true", 4) == 0;
                MO_ASSERT(mo_list_get(output, 1, &item));
                bytes = mo_value_data(item, &size);
                ok = mo_buffer_append(&out->text, bytes, size);
                MO_ASSERT(mo_list_get(output, 2, &item));
                bytes = mo_value_data(item, &size);
                ok = ok && mo_buffer_append(&out->json, bytes, size);
                MO_ASSERT(mo_list_get(output, 3, &item));
                bytes = mo_value_data(item, &size);
                ok = ok && (size == 0 || size == 32);
                if (ok) {
                    memcpy(out->slot, bytes, size);
                    out->slot[size] = 0;
                }
                MO_ASSERT(mo_list_get(output, 4, &item));
                bytes = mo_value_data(item, &size);
                ok = ok && size < sizeof out->next;
                if (ok) {
                    memcpy(out->next, bytes, size);
                    out->next[size] = 0;
                }
            }
            ok = ok || out->rate_limited;
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

static void base64_text(const uint8_t *in, size_t length, char *out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t a = in[i], b = i + 1 < length ? in[i + 1] : 0, c = i + 2 < length ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[at++] = alphabet[(triple >> 18) & 63];
        out[at++] = alphabet[(triple >> 12) & 63];
        out[at++] = i + 1 < length ? alphabet[(triple >> 6) & 63] : '=';
        out[at++] = i + 2 < length ? alphabet[triple & 63] : '=';
    }
    out[at] = 0;
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
    return ok && !r->failed ? 0 : 1;
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
    struct push_reply reply = {0};
    mo_buffer_init(&reply.text, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&reply.json, MO_LIMIT_FETCH_RECEIVE_BYTES);
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
        base64_text(bundle.bytes + offset, length, text);
        mo_buffer_clear(&arguments);
        mo_buffer_clear(&reply.text);
        mo_buffer_clear(&reply.json);
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
        mo_buffer_clear(&reply.text);
        mo_buffer_clear(&reply.json);
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
    mo_buffer_free(&reply.text);
    mo_buffer_free(&reply.json);
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
    struct push_reply reply = {0};
    mo_buffer_init(&reply.text, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&reply.json, MO_LIMIT_FETCH_RECEIVE_BYTES);
    const char *tool = cli->items             ? "mo-intent-items"
                       : cli->setting != NULL ? "mo-review-setting"
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
                : snprintf(arguments, sizeof arguments, "{\"program\":\"%s\",\"digest\":\"%s\"}",
                           cli->push_base, cli->approve[i]);
        mo_buffer_clear(&reply.text);
        mo_buffer_clear(&reply.json);
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
    mo_buffer_free(&reply.text);
    mo_buffer_free(&reply.json);
    mo_image_free(image);
    return status;
}

int mo_push(const struct mo_cli *cli, FILE *output, FILE *errors) {
    return mo_fetch_client(cli, output, errors, "push", push_attempt);
}

int mo_review(const struct mo_cli *cli, FILE *output, FILE *errors) {
    return mo_fetch_client(cli, output, errors, "review", review_attempt);
}
