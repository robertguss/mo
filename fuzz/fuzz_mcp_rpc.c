#include "base/limits.h"
#include "base/utf8.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum { MAX_FUZZ_RPC_BYTES = 4096 };
static struct mo_image *image;
static uint32_t parse, method, render;

static uint32_t function_named(const char *wanted) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == strlen(wanted) && memcmp(name->bytes, wanted, name->length) == 0)
            return i;
    }
    MO_UNREACHABLE();
}

static struct mo_cell call(struct mo_process *process, uint32_t index,
                           const struct mo_cell *values) {
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[index];
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell arguments[3], result;
    struct mo_defect defect;
    MO_ASSERT(fn->parameter_count <= 3);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        arguments[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                           ? mo_value_dup(heap, fn->parameters[i].type, values[i])
                           : values[i];
    MO_ASSERT(mo_call(process, index, arguments, fn->parameter_count, &result, &defect) ==
              MO_RETURNED);
    return result;
}

static struct mo_cell payload(uint32_t type, struct mo_cell value) {
    struct mo_cell out = mo_cell_get_payload(value);
    return mo_case_shape(mo_image_types(image), type, mo_cell_get_tag(value)) == MO_CASE_BOXED
               ? mo_value_field(out, 0)
               : out;
}

static struct mo_cell classify(struct mo_process *process, struct mo_cell request) {
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[method];
    struct mo_heap *heap = mo_process_heap(process);
    uint32_t text_type = mo_image_bytecode(image)->functions[parse].parameters[0].type;
    struct mo_cell name, names, key;
    MO_ASSERT(mo_value_text(heap, "echo", 4, &name));
    MO_ASSERT(mo_value_list(heap, fn->parameters[1].type, &name, 1, &names));
    mo_value_drop(heap, text_type, name);
    MO_ASSERT(mo_value_text(heap, "0123456789abcdef", 16, &key));
    struct mo_cell args[] = {request, names, key};
    struct mo_cell result = call(process, method, args);
    mo_value_drop(heap, text_type, key);
    mo_value_drop(heap, fn->parameters[1].type, names);
    return result;
}

static void generated(struct mo_process *process, struct mo_cell text, struct mo_cell version,
                      size_t size) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    struct mo_heap *heap = mo_process_heap(process);
    uint32_t text_type = file->functions[parse].parameters[0].type;
    uint32_t json_type = file->functions[render].parameters[0].type;
    uint32_t envelope_type = file->functions[parse].result_type;
    struct mo_cell string;
    MO_ASSERT(mo_value_case(heap, json_type, 3, &text, 1, &string));
    struct mo_cell quoted = call(process, render, &string);
    mo_value_drop(heap, json_type, string);
    size_t quoted_size;
    const uint8_t *quoted_bytes = mo_value_data(quoted, &quoted_size);
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MAX_FUZZ_RPC_BYTES * 6 + 256);
    static const struct {
        const char *before, *after;
        uint32_t envelope, method;
    } cases[] = {
        {"{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"ping\",\"params\":{\"x\":", "}}", 4, 3},
        {"{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"tools/"
         "call\",\"params\":{\"name\":\"echo\",\"task\":true,\"arguments\":{\"x\":",
         "}}}", 4, 5},
        {"{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"initialize\",\"params\":{"
         "\"protocolVersion\":",
         ",\"capabilities\":{},\"clientInfo\":{\"name\":\"fuzz\",\"version\":\"0\"}}}", 4, 2},
        {"{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"ping\",\"params\":{\"_meta\":", "}}", 4, 0},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":", "}}", 3, 0},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"result\":", "}", 0, 0},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":", "}", 4, 3},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":{\"x\":null,\"\\u0078\":", "}}", 0,
         0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        mo_buffer_clear(&buffer);
        MO_ASSERT(mo_buffer_append_text(&buffer, cases[i].before));
        MO_ASSERT(mo_buffer_append(&buffer, quoted_bytes, quoted_size));
        MO_ASSERT(mo_buffer_append_text(&buffer, cases[i].after));
        struct mo_cell input;
        MO_ASSERT(mo_value_text(heap, (const char *)buffer.bytes, buffer.length, &input));
        struct mo_cell args[] = {input, version};
        struct mo_cell envelope = call(process, parse, args);
        uint32_t wanted = i == 6 && size > 128 ? 0 : cases[i].envelope;
        MO_ASSERT(mo_cell_get_tag(envelope) == wanted);
        if (wanted == 4) {
            struct mo_cell request = payload(envelope_type, envelope);
            struct mo_cell outcome = classify(process, request);
            MO_ASSERT(mo_cell_get_tag(outcome) == cases[i].method);
            if (cases[i].method == 5) {
                struct mo_cell fields = mo_cell_get_payload(outcome);
                MO_ASSERT(mo_cell_get_int(mo_value_field(fields, 0)) == 0);
            }
            mo_value_drop(heap, file->functions[method].result_type, outcome);
        } else if (wanted == 0) {
            int64_t code = i == 7 ? -32700 : -32600;
            MO_ASSERT(mo_cell_get_int(payload(envelope_type, envelope)) == code);
        }
        mo_value_drop(heap, envelope_type, envelope);
        mo_value_drop(heap, text_type, input);
    }
    mo_buffer_free(&buffer);
    mo_value_drop(heap, text_type, quoted);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > MAX_FUZZ_RPC_BYTES || !mo_utf8_valid(data, size))
        return 0;
    struct mo_limits limits = mo_limits_default();
    if (image == NULL) {
        image = mo_protocol_build();
        MO_ASSERT(image != NULL);
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        parse = function_named("McpRpc.parse");
        method = function_named("McpRpc.method");
        render = function_named("Json.to_text@Json");
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell text, version_text, version;
    MO_ASSERT(mo_value_text(heap, (const char *)data, size, &text));
    MO_ASSERT(mo_value_text(heap, "2025-11-25", 10, &version_text));
    MO_ASSERT(mo_value_case(heap, file->functions[parse].parameters[1].type, 1, &version_text, 1,
                            &version));
    mo_value_drop(heap, file->functions[parse].parameters[0].type, version_text);
    for (unsigned i = 0; i < 2; i++) {
        struct mo_cell args[] = {text, i == 0 ? version : mo_cell_case(0, mo_cell_unit())};
        struct mo_cell outcome = call(process, parse, args);
        MO_ASSERT(mo_cell_get_tag(outcome) <= 4);
        if (mo_cell_get_tag(outcome) == 4) {
            struct mo_cell request = payload(file->functions[parse].result_type, outcome);
            struct mo_cell classified = classify(process, request);
            MO_ASSERT(mo_cell_get_tag(classified) <= 5);
            mo_value_drop(heap, file->functions[method].result_type, classified);
        }
        mo_value_drop(heap, file->functions[parse].result_type, outcome);
    }
    generated(process, text, version, size);
    mo_value_drop(heap, file->functions[parse].parameters[1].type, version);
    mo_value_drop(heap, file->functions[parse].parameters[0].type, text);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(process);
    return 0;
}
