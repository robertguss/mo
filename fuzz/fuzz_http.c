#include "base/limits.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *image;
static uint32_t parse;
static uint32_t scan;
static uint32_t header_type;

static void prepare(void) {
    if (image != NULL)
        return;
    image = mo_protocol_build();
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    parse = scan = UINT32_MAX;
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 18 && memcmp(name->bytes, "Http.parse_headers", 18) == 0)
            parse = i;
        if (name->length == 17 && memcmp(name->bytes, "Http.scan_headers", 17) == 0)
            scan = i;
    }
    MO_ASSERT(parse != UINT32_MAX && file->functions[parse].parameter_count == 2);
    MO_ASSERT(scan != UINT32_MAX && file->functions[scan].parameter_count == 3);
    const struct mo_types *types = mo_image_types(image);
    header_type = file->functions[parse].parameters[1].type;
    MO_ASSERT(types->items[header_type].kind == MO_TYPE_CHOICE &&
              types->items[header_type].case_count == 1 &&
              types->items[header_type].cases[0].field_count == 3);
}

static void span(struct mo_cell value, size_t size) {
    struct mo_cell fields = mo_cell_get_payload(value);
    int64_t start = mo_cell_get_int(mo_value_field(fields, 0));
    int64_t length = mo_cell_get_int(mo_value_field(fields, 1));
    MO_ASSERT(start >= 0 && length >= 0 && (uint64_t)start <= size);
    MO_ASSERT((uint64_t)length <= size - (size_t)start);
}

static void scan_input(const uint8_t *data, size_t size, const struct mo_cell *cells) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_MCP_HEADER_HEAP;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_HEADER_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[scan];
    struct mo_cell args[3];
    MO_ASSERT(mo_value_list(heap, function->parameters[0].type, cells, (uint32_t)size, &args[0]));
    struct mo_cell state[] = {mo_cell_int(0), mo_cell_int(0), mo_cell_bool(false)};
    MO_ASSERT(mo_value_case(heap, function->parameters[1].type, 0, state, 3, &args[1]));
    args[2] = mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES);
    struct mo_cell result;
    struct mo_defect defect;
    MO_ASSERT(mo_call(process, scan, args, 3, &result, &defect) == MO_RETURNED);
    MO_ASSERT(mo_cell_get_tag(result) == 0);
    struct mo_cell record = mo_cell_get_payload(mo_value_field(mo_cell_get_payload(result), 0));
    size_t consumed = size;
    bool complete = false;
    for (size_t i = 0; i + 4 <= size; i++)
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) {
            consumed = i + 4;
            complete = true;
            break;
        }
    MO_ASSERT(mo_cell_get_int(mo_value_field(record, 1)) == (int64_t)consumed);
    MO_ASSERT(mo_cell_get_bool(mo_value_field(record, 2)) == complete);
    mo_process_free(process);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > MO_LIMIT_MCP_HEADER_BYTES)
        return 0;
    const struct mo_bearer bearer = {.bytes = "fuzz-credential", .length = 15};
    MO_ASSERT(mo_bearer_check(&bearer, data, size) ==
              (size == bearer.length && memcmp(data, bearer.bytes, size) == 0));
    prepare();
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_MCP_HEADER_HEAP;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_HEADER_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell cells[MO_LIMIT_MCP_HEADER_BYTES];
    for (size_t i = 0; i < size; i++)
        cells[i] = mo_cell_byte(data[i]);
    scan_input(data, size, cells);
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[parse];
    struct mo_cell arguments[2];
    MO_ASSERT(
        mo_value_list(heap, function->parameters[0].type, cells, (uint32_t)size, &arguments[0]));
    struct mo_cell bounds[] = {mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_LINE_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_HEADER_FIELDS)};
    MO_ASSERT(mo_value_case(heap, header_type, 0, bounds, 3, &arguments[1]));
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(process, parse, arguments, 2, &result, &defect);
    MO_ASSERT(outcome != MO_FAILED);
    if (outcome == MO_RETURNED && mo_cell_get_tag(result) == 0) {
        struct mo_cell value = mo_value_field(mo_cell_get_payload(result), 0);
        struct mo_cell head = mo_cell_get_payload(value);
        int64_t after = mo_cell_get_int(mo_value_field(head, 4));
        MO_ASSERT(after >= 4 && (uint64_t)after <= size);
        size_t consumed = (size_t)after;
        MO_ASSERT(memcmp(data + consumed - 4, "\r\n\r\n", 4) == 0);
        for (uint32_t i = 0; i < 3; i++)
            span(mo_value_field(head, i), consumed);
        struct mo_cell fields = mo_value_field(head, 3);
        uint32_t count = mo_list_size(fields);
        MO_ASSERT(count <= MO_LIMIT_MCP_HEADER_FIELDS);
        for (uint32_t i = 0; i < count; i++) {
            struct mo_cell field;
            MO_ASSERT(mo_list_get(fields, i, &field));
            struct mo_cell parts = mo_cell_get_payload(field);
            span(mo_value_field(parts, 0), consumed);
            span(mo_value_field(parts, 1), consumed);
        }
    }
    mo_process_free(process);
    return 0;
}
