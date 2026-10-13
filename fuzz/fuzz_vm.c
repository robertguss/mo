#include "vm/vm.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static MO_MUST_USE bool argument(struct mo_heap *heap, const struct mo_types *types, uint32_t type,
                                 uint64_t bits, struct mo_cell *out) {
    enum mo_type_kind kind = types->items[type].kind;
    if (kind <= MO_TYPE_RANGE) {
        switch (kind) {
        case MO_TYPE_UNIT:
            *out = mo_cell_unit();
            break;
        case MO_TYPE_BOOL:
            *out = mo_cell_bool((bits & 1) != 0);
            break;
        case MO_TYPE_BYTE:
            *out = mo_cell_byte((uint8_t)bits);
            break;
        case MO_TYPE_CHAR:
            *out = mo_cell_char((uint32_t)(bits % 128));
            break;
        case MO_TYPE_INT:
            *out = mo_cell_int((int64_t)(bits % 100) - 50);
            break;
        case MO_TYPE_FLOAT: {
            double number;
            memcpy(&number, &bits, 8);
            *out = mo_cell_float(number);
            break;
        }
        case MO_TYPE_TIME:
            *out = mo_cell_time((int64_t)(bits % 100));
            break;
        case MO_TYPE_DURATION:
            *out = mo_cell_duration((int64_t)bits);
            break;
        case MO_TYPE_DECIMAL:
            *out = mo_cell_decimal((mo_i128)bits * INT64_C(1000000000000000000));
            break;
        case MO_TYPE_RANGE:
            *out = mo_cell_range((int64_t)(bits % 50), (int64_t)(bits % 100));
            break;
        case MO_TYPE_TEXT:
        case MO_TYPE_BYTES:
        case MO_TYPE_BIGINT:
        case MO_TYPE_TUPLE:
        case MO_TYPE_LIST:
        case MO_TYPE_SET:
        case MO_TYPE_MAP:
        case MO_TYPE_RECORD:
        case MO_TYPE_CHOICE:
        case MO_TYPE_FUNCTION:
        case MO_TYPE_ENV:
        case MO_TYPE_CAPABILITY:
            MO_UNREACHABLE();
        }
        return true;
    }
    if (kind == MO_TYPE_TEXT)
        return mo_value_text(heap, "argument", 8, out);
    if (kind == MO_TYPE_BYTES)
        return mo_value_bytes(heap, (const uint8_t *)&bits, 8, out);
    if (kind == MO_TYPE_BIGINT)
        return mo_value_bigint(heap, MO_BIGINT_ZERO, NULL, 0, out);
    if (kind == MO_TYPE_LIST)
        return mo_value_list(heap, type, NULL, 0, out);
    if (kind == MO_TYPE_SET)
        return mo_value_set(heap, type, NULL, 0, out);
    if (kind == MO_TYPE_MAP)
        return mo_value_map(heap, type, NULL, NULL, 0, out);
    if (kind == MO_TYPE_CHOICE) {
        for (uint32_t i = 0; i < types->items[type].case_count; i++)
            if (types->items[type].cases[i].field_count == 0)
                return mo_value_case(heap, type, i, NULL, 0, out);
    }
    return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < MO_BYTECODE_HEADER_SIZE || size > (64 << 10))
        return 0;
    uint8_t *wire = malloc(size);
    if (wire == NULL)
        return 0;
    memcpy(wire, data, size);
    /* Mutations of instructions/tables reach the verifier, not just checksum
     * rejection. Execute only the image it publishes. */
    uint64_t checksum = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = MO_BYTECODE_HEADER_SIZE; i < size; i++)
        checksum = (checksum ^ wire[i]) * MO_BYTECODE_FNV_PRIME;
    for (unsigned i = 0; i < 8; i++)
        wire[12 + i] = (uint8_t)(checksum >> (8 * i));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire, size, &error);
    free(wire);
    if (image == NULL)
        return 0;
    uint64_t bits = 0;
    memcpy(&bits, data + 12, 8);
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 4096 + (size_t)(bits % (60 << 10));
    limits.stack_size = 64 + (size_t)((bits >> 16) % 8192);
    limits.call_depth = 1 + (uint32_t)((bits >> 32) % 8);
    struct mo_defect defect;
    if (mo_image_init_bounded(image, &limits, 512, &defect) != MO_RETURNED) {
        struct mo_buffer text;
        mo_buffer_init(&text, 1024);
        bool rendered = mo_render_defect(NULL, &defect, &text);
        (void)rendered;
        mo_buffer_free(&text);
        mo_image_free(image);
        return 0;
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    uint32_t functions = file->function_count < 4 ? file->function_count : 4;
    for (uint32_t f = 0; f < functions; f++) {
        const struct mo_bytecode_function *fn = &file->functions[f];
        if (fn->kind == MO_FUNCTION_CLOSURE || fn->kind == MO_FUNCTION_INIT)
            continue;
        struct mo_process *p = mo_process_new(image, &limits);
        if (p == NULL)
            break;
        struct mo_heap *heap = mo_process_heap(p);
        size_t baseline = mo_heap_used(heap);
        struct mo_cell args[MO_LIMIT_BYTECODE_PARAMETERS];
        uint32_t n = 0;
        while (n < fn->parameter_count &&
               argument(heap, types, fn->parameters[n].type, bits, &args[n]))
            n++;
        if (n != fn->parameter_count) {
            for (uint32_t i = 0; i < n; i++)
                mo_value_drop(heap, fn->parameters[i].type, args[i]);
            mo_process_free(p);
            continue;
        }
        mo_process_instruction_limit(p, 512);
        struct mo_cell result = mo_cell_int(99), unchanged = result;
        enum mo_outcome outcome = mo_call(p, f, args, n, &result, &defect);
        if (outcome == MO_RETURNED) {
            mo_value_drop(heap, fn->result_type, result);
        } else {
            MO_ASSERT(memcmp(&result, &unchanged, sizeof result) == 0);
            struct mo_buffer text;
            mo_buffer_init(&text, 1024);
            bool rendered = mo_render_defect(p, &defect, &text);
            (void)rendered;
            mo_buffer_free(&text);
        }
        for (uint32_t i = 0; i < n; i++)
            if (fn->parameters[i].ownership == MO_PARAMETER_BORROWED)
                mo_value_drop(heap, fn->parameters[i].type, args[i]);
        if (outcome == MO_RETURNED)
            MO_ASSERT(mo_process_stack_used(p) == 0 && mo_heap_objects(heap) == 0 &&
                      mo_heap_used(heap) == baseline);
        mo_process_free(p);
    }
    mo_image_free(image);
    return 0;
}
