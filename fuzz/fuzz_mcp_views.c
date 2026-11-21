#include "base/limits.h"
#include "base/utf8.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* One part: a byte below 128 is that list index; 128 + k is a field name of the next k % 32
 * bytes. */
static bool part(struct mo_heap *heap, const struct mo_types *types, uint32_t json,
                 const uint8_t **data, size_t *size, struct mo_cell *out) {
    if (*size == 0)
        return false;
    uint8_t first = **data;
    (*data)++;
    (*size)--;
    struct mo_cell text, payload;
    uint32_t tag = 3;
    if (first < 128) {
        char digits[4];
        int written = snprintf(digits, sizeof digits, "%u", first);
        MO_ASSERT(written > 0 && (size_t)written < sizeof digits);
        MO_ASSERT(mo_value_text(heap, digits, (size_t)written, &text));
        uint32_t number = types->items[json].cases[2].fields[0].type;
        MO_ASSERT(mo_value_case(heap, number, 0, &text, 1, &payload));
        mo_value_drop(heap, types->items[number].cases[0].fields[0].type, text);
        tag = 2;
    } else {
        size_t length = (size_t)(first - 128) % 32;
        if (length > *size)
            return false;
        if (!mo_value_text(heap, (const char *)*data, length, &payload))
            return false;
        *data += length;
        *size -= length;
    }
    MO_ASSERT(mo_value_case(heap, json, tag, &payload, 1, out));
    mo_value_drop(heap, types->items[json].cases[tag].fields[0].type, payload);
    return true;
}

static uint32_t function_named(struct mo_image *image, const char *wanted) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == strlen(wanted) && memcmp(name->bytes, wanted, name->length) == 0)
            return i;
    }
    MO_UNREACHABLE();
}

static bool invoke(struct mo_process *process, struct mo_image *image, uint32_t entry,
                   struct mo_cell *args, struct mo_cell *result) {
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[entry];
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell owned[2];
    MO_ASSERT(fn->parameter_count <= 2);
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        owned[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                       ? mo_value_dup(heap, fn->parameters[i].type, args[i])
                       : args[i];
    struct mo_defect defect;
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    enum mo_outcome outcome = mo_call(process, entry, owned, fn->parameter_count, result, &defect);
    MO_ASSERT(outcome == MO_RETURNED || (outcome == MO_FAILED && defect.limit_name != NULL));
    return outcome == MO_RETURNED;
}

/* JSON seeds exercise all upload schemas and extraction; raw HTTP seeds exercise the
 * push reader, including the no-effect rate refusal used for safe retries. */
static void upgrades(struct mo_process *process, struct mo_image *image, const uint8_t *data,
                     size_t size) {
    if (!mo_utf8_valid(data, size))
        return;
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    uint32_t reader = function_named(image, "McpFetch.upgrade");
    uint32_t parse = function_named(image, "Json.parse");
    uint32_t valid = function_named(image, "McpViews.valid");
    uint32_t extract = function_named(image, "McpViews.upgrade_arguments");
    struct mo_cell text, result;
    if (!mo_value_text(heap, (const char *)data, size, &text))
        return;
    if (invoke(process, image, reader, &text, &result)) {
        uint32_t count = mo_list_size(result);
        MO_ASSERT(count == 0 || count == 1 || count == 4);
        mo_value_drop(heap, file->functions[reader].result_type, result);
    }
    if (invoke(process, image, parse, &text, &result)) {
        if (mo_cell_get_tag(result) == 0) {
            uint32_t type = file->functions[parse].result_type;
            struct mo_cell json = mo_cell_get_payload(result);
            if (mo_case_shape(mo_image_types(image), type, 0) == MO_CASE_BOXED)
                json = mo_value_field(json, 0);
            for (int64_t index = 9; index <= 11; index++) {
                struct mo_cell args[] = {mo_cell_int(index), json}, checked;
                if (invoke(process, image, valid, args, &checked) && mo_cell_get_bool(checked)) {
                    struct mo_cell fields;
                    if (invoke(process, image, extract, &json, &fields)) {
                        MO_ASSERT(mo_list_size(fields) == 8);
                        mo_value_drop(heap, file->functions[extract].result_type, fields);
                    }
                }
            }
        }
        mo_value_drop(heap, file->functions[parse].result_type, result);
    }
    mo_value_drop(heap, file->functions[reader].parameters[0].type, text);
}

/* Input: a byte whose low two bits count the path parts, those parts, then the saved bytes. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > MO_LIMIT_MCP_BODY_BYTES)
        return 0;
    static struct mo_image *image;
    static uint32_t entry;
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_MCP_REQUEST_MEMORY;
    struct mo_defect defect;
    if (image == NULL) {
        image = mo_protocol_build();
        MO_ASSERT(image != NULL && mo_image_init(image, &limits, &defect) == MO_RETURNED);
        const struct mo_bytecode *file = mo_image_bytecode(image);
        entry = MO_NO_INDEX;
        const char wanted[] = "McpViews.saved_state";
        for (uint32_t i = 0; i < file->function_count; i++) {
            const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
            if (name->length == sizeof wanted - 1 &&
                memcmp(name->bytes, wanted, sizeof wanted - 1) == 0)
                entry = i;
        }
        MO_ASSERT(entry != MO_NO_INDEX);
    }
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    upgrades(process, image, data, size);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[entry];
    const struct mo_types *types = mo_image_types(image);
    uint32_t json = fn->parameters[1].type;
    uint32_t array = types->items[json].cases[4].fields[0].type;
    struct mo_cell args[3], list, result;
    MO_ASSERT(mo_value_list(heap, array, NULL, 0, &list));
    unsigned parts = data[0] & 3u;
    data++;
    size--;
    bool ok = true;
    for (unsigned i = 0; ok && i < parts; i++) {
        struct mo_cell item;
        ok = part(heap, types, json, &data, &size, &item);
        if (ok) {
            MO_ASSERT(mo_list_append(heap, array, &list, item));
            mo_value_drop(heap, json, item);
        }
    }
    if (!ok) {
        mo_value_drop(heap, array, list);
        mo_process_free(process);
        return 0;
    }
    MO_ASSERT(mo_value_bytes(heap, data, size, &args[0]));
    MO_ASSERT(mo_value_case(heap, json, 4, &list, 1, &args[1]));
    mo_value_drop(heap, array, list);
    args[2] = mo_cell_int(MO_LIMIT_MCP_VIEW_BYTES);
    struct mo_cell owned[3];
    for (uint32_t i = 0; i < 3; i++)
        owned[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                       ? mo_value_dup(heap, fn->parameters[i].type, args[i])
                       : args[i];
    enum mo_outcome outcome = mo_call(process, entry, owned, 3, &result, &defect);
    MO_ASSERT(outcome == MO_RETURNED || (outcome == MO_FAILED && defect.limit_name != NULL));
    if (outcome == MO_RETURNED)
        mo_value_drop(heap, fn->result_type, result);
    for (uint32_t i = 0; i < 3; i++)
        mo_value_drop(heap, fn->parameters[i].type, args[i]);
    mo_process_free(process);
    return 0;
}
