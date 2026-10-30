#include "../tests/heap_fixture.h"
#include "base/limits.h"
#include "base/utf8.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *image;
static struct heap_fixture native;
static uint32_t parse, byte, bigint, decode, encode;

static uint32_t function_named(const char *wanted) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == strlen(wanted) && memcmp(name->bytes, wanted, name->length) == 0)
            return i;
    }
    MO_ASSERT(false);
    return 0;
}

static struct mo_cell call(struct mo_process *process, uint32_t index, struct mo_cell *arguments) {
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[index];
    struct mo_heap *heap = mo_process_heap(process);
    for (uint32_t i = 0; i < function->parameter_count; i++)
        if (function->parameters[i].ownership == MO_PARAMETER_OWNED)
            arguments[i] = mo_value_dup(heap, function->parameters[i].type, arguments[i]);
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

static void scalars(struct mo_process *process, struct mo_cell json) {
    static const enum mo_type_kind kinds[] = {
        MO_TYPE_UNIT,  MO_TYPE_BOOL,   MO_TYPE_BYTE,     MO_TYPE_INT,     MO_TYPE_FLOAT,
        MO_TYPE_CHAR,  MO_TYPE_TIME,   MO_TYPE_DURATION, MO_TYPE_DECIMAL, MO_TYPE_TEXT,
        MO_TYPE_BYTES, MO_TYPE_BIGINT, MO_TYPE_RANGE,
    };
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    uint32_t option = file->functions[decode].result_type;
    uint32_t scalar = file->functions[encode].parameters[0].type;
    uint32_t json_type = file->functions[encode].result_type;
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap *destination = mo_heap_new(native.image, &limits);
    MO_ASSERT(destination != NULL);
    for (uint32_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        struct mo_cell arguments[] = {mo_cell_case(i == 12 ? 16 : i, mo_cell_unit()), json,
                                      mo_cell_int(MO_LIMIT_MCP_BIGINT_DIGITS)};
        struct mo_cell decoded = call(process, decode, arguments);
        if (mo_cell_get_tag(decoded) == 1) {
            struct mo_cell value = payload(option, decoded), copied, packed;
            MO_ASSERT(mo_protocol_scalar_unpack(types, scalar, destination, kinds[i], value,
                                                MO_LIMIT_MCP_BIGINT_DIGITS + 2,
                                                &copied) == MO_PROTOCOL_VALUE_OK);
            MO_ASSERT(mo_protocol_scalar_pack(types, scalar, heap, kinds[i], copied,
                                              MO_LIMIT_MCP_BIGINT_DIGITS + 2,
                                              &packed) == MO_PROTOCOL_VALUE_OK);
            mo_value_drop(destination, native.builtin[kinds[i]], copied);
            struct mo_cell expected = call(process, encode, &value);
            struct mo_cell actual = call(process, encode, &packed);
            bool equal;
            MO_ASSERT(mo_value_equal(heap, json_type, expected, actual, &equal) && equal);
            arguments[0] = mo_cell_case(i == 12 ? 16 : i, mo_cell_unit());
            arguments[1] = actual;
            struct mo_cell again = call(process, decode, arguments);
            MO_ASSERT(mo_cell_get_tag(again) == 1);
            value = payload(option, again);
            struct mo_cell canonical = call(process, encode, &value);
            MO_ASSERT(mo_value_equal(heap, json_type, expected, canonical, &equal) && equal);
            mo_value_drop(heap, json_type, canonical);
            mo_value_drop(heap, option, again);
            mo_value_drop(heap, json_type, actual);
            mo_value_drop(heap, json_type, expected);
            mo_value_drop(heap, scalar, packed);
        }
        mo_value_drop(heap, option, decoded);
    }
    MO_ASSERT(mo_heap_objects(destination) == 0);
    mo_heap_free(destination);
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
        parse = function_named("Json.parse");
        byte = function_named("McpNumber.byte");
        bigint = function_named("McpNumber.bigint");
        decode = function_named("McpScalar.decode");
        encode = function_named("McpScalar.encode");
        heap_fixture_init(&native);
    }
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell text;
    MO_ASSERT(mo_value_text(heap, (const char *)data, size, &text));
    struct mo_cell arguments[2] = {text, mo_cell_int(MO_LIMIT_MCP_BIGINT_DIGITS)};
    struct mo_cell integer = call(process, bigint, arguments);
    size_t start = size != 0 && data[0] == '-' ? 1u : 0u;
    bool valid = size > start && size - start <= MO_LIMIT_MCP_BIGINT_DIGITS;
    for (size_t i = start; i < size; i++)
        valid = valid && data[i] >= '0' && data[i] <= '9';
    MO_ASSERT((mo_cell_get_tag(integer) == 1) == valid);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    if (valid) {
        uint32_t result_type = file->functions[bigint].result_type;
        uint32_t value_type = types->items[result_type].cases[1].fields[0].type;
        struct mo_buffer printed;
        mo_buffer_init(&printed, MO_LIMIT_MCP_BIGINT_DIGITS + 1);
        MO_ASSERT(mo_value_print(heap, value_type, payload(result_type, integer), &printed));
        while (start < size && data[start] == '0')
            start++;
        bool negative = start < size && data[0] == '-';
        size_t digits = size - start;
        if (digits == 0)
            MO_ASSERT(printed.length == 1 && printed.bytes[0] == '0');
        else {
            MO_ASSERT(printed.length == digits + (negative ? 1u : 0u));
            MO_ASSERT(!negative || printed.bytes[0] == '-');
            MO_ASSERT(memcmp(printed.bytes + (negative ? 1u : 0u), data + start, digits) == 0);
        }
        mo_buffer_free(&printed);
    }
    mo_value_drop(heap, file->functions[bigint].result_type, integer);
    arguments[0] = text;
    struct mo_cell parsed = call(process, parse, arguments);
    if (mo_cell_get_tag(parsed) == 0) {
        arguments[0] = payload(file->functions[parse].result_type, parsed);
        struct mo_cell converted = call(process, byte, arguments);
        MO_ASSERT(mo_cell_get_tag(converted) <= 1);
        mo_value_drop(heap, file->functions[byte].result_type, converted);
        scalars(process, payload(file->functions[parse].result_type, parsed));
    }
    mo_value_drop(heap, file->functions[parse].result_type, parsed);
    mo_value_drop(heap, file->functions[parse].parameters[0].type, text);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(process);
    return 0;
}
