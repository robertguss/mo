#include "../tests/service_compile.h"
#include "base/utf8.h"
#include "tools/protocol.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *image, *service;
static uint32_t parse, decode, encode;

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
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[index];
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell arguments[5];
    MO_ASSERT(function->parameter_count <= 5);
    for (uint32_t i = 0; i < function->parameter_count; i++)
        arguments[i] = function->parameters[i].ownership == MO_PARAMETER_OWNED
                           ? mo_value_dup(heap, function->parameters[i].type, values[i])
                           : values[i];
    struct mo_cell result;
    struct mo_defect defect;
    MO_ASSERT(mo_call(process, index, arguments, function->parameter_count, &result, &defect) ==
              MO_RETURNED);
    return result;
}

static struct mo_cell payload(uint32_t type, struct mo_cell value) {
    enum mo_case_shape shape = mo_case_shape(mo_image_types(image), type, mo_cell_get_tag(value));
    struct mo_cell result = mo_cell_get_payload(value);
    return shape == MO_CASE_BOXED ? mo_value_field(result, 0) : result;
}

static void roundtrip(struct mo_process *process, struct mo_cell json) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    struct mo_protocol_descriptor descriptor;
    struct mo_heap *heap = mo_process_heap(process);
    MO_ASSERT(mo_protocol_descriptor(image, heap, service, 0, &descriptor));
    struct mo_cell root = mo_cell_get_payload(descriptor.value), tool;
    struct mo_cell graph = mo_value_field(root, 0);
    MO_ASSERT(mo_list_get(mo_value_field(root, 1), 0, &tool));
    struct mo_cell fields = mo_value_field(mo_cell_get_payload(tool), 3);
    struct mo_protocol_values types = {mo_image_types(image), mo_image_types(service),
                                       &mo_image_bytecode(service)->services[0],
                                       file->functions[encode].parameters[2].type};
    struct mo_limits limits = mo_limits_default();
    struct mo_process *destination = mo_process_new(service, &limits);
    MO_ASSERT(destination != NULL);
    struct mo_heap *native = mo_process_heap(destination);
    uint32_t result_type = file->functions[decode].result_type;
    uint32_t json_result = file->functions[encode].result_type;
    for (uint32_t i = 0; i < mo_list_size(fields); i++) {
        struct mo_cell field;
        MO_ASSERT(mo_list_get(fields, i, &field));
        struct mo_cell index = mo_value_field(mo_cell_get_payload(field), 1);
        struct mo_cell args[] = {graph, index, json, mo_cell_int(60), mo_cell_int(4096)};
        struct mo_cell decoded = call(process, decode, args);
        if (mo_cell_get_tag(decoded) == 0) {
            uint32_t native_type = types.descriptor->tools[0].argument_types[i];
            struct mo_cell value = payload(result_type, decoded), copied = mo_cell_int(73);
            enum mo_protocol_value_status status = mo_protocol_value_unpack(
                &types, native, native_type, value, MO_LIMIT_MCP_BIGINT_DIGITS + 2, &copied);
            MO_ASSERT(status == MO_PROTOCOL_VALUE_OK || status == MO_PROTOCOL_VALUE_INVALID);
            if (status == MO_PROTOCOL_VALUE_OK) {
                struct mo_cell packed;
                MO_ASSERT(mo_protocol_value_pack(&types, heap, native_type, copied,
                                                 MO_LIMIT_MCP_BIGINT_DIGITS + 2,
                                                 &packed) == MO_PROTOCOL_VALUE_OK);
                mo_value_drop(native, native_type, copied);
                args[2] = value;
                struct mo_cell expected = call(process, encode, args);
                args[2] = packed;
                struct mo_cell actual = call(process, encode, args);
                bool equal;
                MO_ASSERT(mo_value_equal(heap, json_result, expected, actual, &equal) && equal);
                mo_value_drop(heap, json_result, expected);
                mo_value_drop(heap, json_result, actual);
                mo_value_drop(heap, types.value, packed);
            } else
                MO_ASSERT(mo_cell_get_int(copied) == 73);
        }
        mo_value_drop(heap, result_type, decoded);
        MO_ASSERT(mo_heap_objects(native) == 0);
    }
    mo_process_free(destination);
    mo_value_drop(heap, descriptor.type, descriptor.value);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > MO_LIMIT_MCP_BIGINT_DIGITS + 2 || !mo_utf8_valid(data, size))
        return 0;
    struct mo_limits limits = mo_limits_default();
    if (image == NULL) {
        image = mo_protocol_build();
        MO_ASSERT(image != NULL);
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        service = compile(
            "module Main\nstruct Record\nname: Text\nkind: EntryKind\nend\n"
            "enum Tree\nEnd\nNext(Tree)\nNamed(value: Text)\nPair(left: Int, "
            "right: Bool)\nend\n"
            "service\nstate Int = 0\ntools accept\ntoken from env \"TOKEN\"\nend\n"
            "fn accept(_state: Int, _now: Time, _list: List(Record), _set: Set(Int), "
            "_map: Map(Int, List(Text)), _pair: (Text, Int), _entry: Entry, _record: Record, "
            "_tree: Tree, _nested: Set(Set(Int)))\nUnit\nend\n");
        parse = function_named("Json.parse");
        decode = function_named("McpValue.decode");
        encode = function_named("McpValue.encode");
    }
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell text;
    MO_ASSERT(mo_value_text(heap, (const char *)data, size, &text));
    struct mo_cell parsed = call(process, parse, &text);
    uint32_t result_type = mo_image_bytecode(image)->functions[parse].result_type;
    if (mo_cell_get_tag(parsed) == 0)
        roundtrip(process, payload(result_type, parsed));
    mo_value_drop(heap, result_type, parsed);
    mo_value_drop(heap, mo_image_bytecode(image)->functions[parse].parameters[0].type, text);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(process);
    return 0;
}
