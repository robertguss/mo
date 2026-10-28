#include "base/limits.h"
#include "base/utf8.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *image;
static uint32_t parse, byte, bigint;

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
    }
    mo_value_drop(heap, file->functions[parse].result_type, parsed);
    mo_value_drop(heap, file->functions[parse].parameters[0].type, text);
    MO_ASSERT(mo_heap_objects(heap) == 0);
    mo_process_free(process);
    return 0;
}
