#include "../tests/input_values.h"
#include "tools/inputs.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct input_bytes {
    const uint8_t *data;
    size_t size, at;
};

static MO_MUST_USE uint8_t next_byte(struct input_bytes *in) {
    return in->at < in->size ? in->data[in->at++] : 0;
}

static MO_MUST_USE uint32_t intern(struct mo_types *types, struct mo_type type) {
    uint32_t out;
    MO_ASSERT(mo_types_intern(types, &type, &out) == MO_TYPES_OK);
    return out;
}

static void add_layout(struct mo_types *types, uint32_t type, uint32_t tag, const uint32_t *fields,
                       uint32_t count) {
    enum mo_layout_kind kind = types->items[type].kind == MO_TYPE_RECORD  ? MO_LAYOUT_RECORD
                               : types->items[type].kind == MO_TYPE_TUPLE ? MO_LAYOUT_TUPLE
                                                                          : MO_LAYOUT_CASE;
    struct mo_layout layout = {.kind = kind,
                               .type = type,
                               .case_index = tag,
                               .function = MO_NO_INDEX,
                               .fields = fields,
                               .field_count = count};
    uint32_t out;
    MO_ASSERT(mo_types_layout(types, &layout, &out) == MO_TYPES_OK);
}

static void random_types(struct mo_types *types, struct input_bytes *in) {
    for (unsigned i = 0; i <= MO_TYPE_BIGINT; i++) {
        uint32_t ignored = intern(types, (struct mo_type){.kind = (enum mo_type_kind)i});
        (void)ignored;
    }
    static const char *const names[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
    uint32_t nominal[8];
    uint32_t count = 1U + next_byte(in) % 8U;
    for (uint32_t i = 0; i < count; i++) {
        enum mo_type_kind kind = next_byte(in) % 2 == 0 ? MO_TYPE_RECORD : MO_TYPE_CHOICE;
        MO_ASSERT(mo_types_declare(types, kind, names[i], &nominal[i]) == MO_TYPES_OK);
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t field_types[2];
        for (unsigned f = 0; f < 2; f++) {
            uint32_t child = next_byte(in) % types->count;
            unsigned wrapper = next_byte(in) % 5;
            if (wrapper == 0)
                field_types[f] = child;
            else {
                enum mo_type_kind kind = wrapper == 1   ? MO_TYPE_LIST
                                         : wrapper == 2 ? MO_TYPE_SET
                                         : wrapper == 3 ? MO_TYPE_MAP
                                                        : MO_TYPE_TUPLE;
                uint32_t parameters[] = {child, next_byte(in) % types->count};
                field_types[f] =
                    intern(types, (struct mo_type){.kind = kind,
                                                   .parameters = parameters,
                                                   .parameter_count = wrapper < 3 ? 1U : 2U});
            }
        }
        struct mo_type_field fields[] = {{"first", field_types[0]}, {"second", field_types[1]}};
        uint32_t fields_count = next_byte(in) % 3U;
        if (types->items[nominal[i]].kind == MO_TYPE_RECORD)
            MO_ASSERT(mo_types_define_record(types, nominal[i], fields, fields_count) ==
                      MO_TYPES_OK);
        else {
            struct mo_type_case cases[] = {
                {.name = "Some", .fields = fields, .field_count = fields_count}, {.name = "None"}};
            uint32_t cases_count = 1U + next_byte(in) % 2U;
            MO_ASSERT(mo_types_define_choice(types, nominal[i], cases, cases_count) == MO_TYPES_OK);
        }
    }
    MO_ASSERT(mo_types_finish(types) == MO_TYPES_OK);
    for (uint32_t i = 0; i < types->count; i++) {
        const struct mo_type *type = &types->items[i];
        uint32_t fields[2];
        if (type->kind == MO_TYPE_RECORD) {
            for (uint32_t f = 0; f < type->field_count; f++)
                fields[f] = type->fields[f].type;
            add_layout(types, i, MO_NO_INDEX, fields, type->field_count);
        } else if (type->kind == MO_TYPE_TUPLE)
            add_layout(types, i, MO_NO_INDEX, type->parameters, type->parameter_count);
        else if (type->kind == MO_TYPE_CHOICE) {
            for (uint32_t c = 0; c < type->case_count; c++) {
                if (mo_case_shape(types, i, c) != MO_CASE_BOXED)
                    continue;
                for (uint32_t f = 0; f < type->cases[c].field_count; f++)
                    fields[f] = type->cases[c].fields[f].type;
                add_layout(types, i, c, fields, type->cases[c].field_count);
            }
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct input_bytes bytes = {data, size, 0};
    uint8_t selection[] = {next_byte(&bytes), next_byte(&bytes)};
    uint32_t count = 1U + next_byte(&bytes) % 2U;
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    random_types(&types, &bytes);
    uint32_t parameters[] = {types.count - 1U - selection[0] % types.count,
                             types.count - 1U - selection[1] % types.count};
    struct mo_input_limits limits = mo_input_limits_default();
    limits.combinations = 16;
    limits.depth = 16;
    limits.nodes = 128;
    limits.elements = 8;
    limits.bytes = 4096;
    struct mo_inputs first, second;
    enum mo_input_result result = mo_inputs_init(&first, &types, parameters, count,
                                                 (const char *)data, size, &limits, NULL, 0);
    enum mo_input_result other = mo_inputs_init(&second, &types, parameters, count,
                                                (const char *)data, size, &limits, NULL, 0);
    MO_ASSERT(result == other);
    if (result == MO_INPUT_OK) {
        struct mo_limits heap_limits = mo_limits_default();
        heap_limits.heap_size = 4 << 20;
        struct mo_heap_image *images[] = {mo_heap_image_new(&types, &heap_limits),
                                          mo_heap_image_new(&types, &heap_limits)};
        MO_ASSERT(images[0] != NULL && images[1] != NULL);
        struct mo_heap *heaps[] = {mo_heap_new(images[0], &heap_limits),
                                   mo_heap_new(images[1], &heap_limits)};
        MO_ASSERT(heaps[0] != NULL && heaps[1] != NULL);
        for (unsigned trial = 0; trial < 48; trial++) {
            struct mo_cell a[2], b[2];
            enum mo_input_kind ka, kb;
            result = mo_inputs_next(&first, heaps[0], a, &ka);
            other = mo_inputs_next(&second, heaps[1], b, &kb);
            MO_ASSERT(result == other);
            if (result != MO_INPUT_OK) {
                MO_ASSERT(result == MO_INPUT_LIMIT || result == MO_INPUT_NO_MEMORY);
                MO_ASSERT(mo_heap_objects(heaps[0]) == 0 && mo_heap_objects(heaps[1]) == 0);
                mo_inputs_skip(&first);
                mo_inputs_skip(&second);
                continue;
            }
            MO_ASSERT(ka == kb);
            for (uint32_t p = 0; p < count; p++) {
                MO_ASSERT(input_value_typed(heaps[0], &types, parameters[p], a[p]));
                MO_ASSERT(input_value_typed(heaps[1], &types, parameters[p], b[p]));
                struct mo_buffer left, right;
                mo_buffer_init(&left, 1 << 20);
                mo_buffer_init(&right, 1 << 20);
                MO_ASSERT(mo_value_print(heaps[0], parameters[p], a[p], &left));
                MO_ASSERT(mo_value_print(heaps[1], parameters[p], b[p], &right));
                MO_ASSERT(left.length == right.length &&
                          (left.length == 0 || memcmp(left.bytes, right.bytes, left.length) == 0));
                mo_buffer_free(&right);
                mo_buffer_free(&left);
            }
            struct mo_input_shrinker shrinker;
            mo_input_shrink_init(&shrinker, &first, 2);
            struct mo_cell candidate[2];
            result = mo_input_shrink_next(&shrinker, a, heaps[1], candidate);
            if (result == MO_INPUT_OK) {
                for (uint32_t p = 0; p < count; p++) {
                    MO_ASSERT(input_value_typed(heaps[1], &types, parameters[p], candidate[p]));
                    mo_value_drop(heaps[1], parameters[p], candidate[p]);
                }
                mo_input_shrink_feedback(&shrinker, false);
            } else
                MO_ASSERT(result == MO_INPUT_DONE || result == MO_INPUT_LIMIT ||
                          result == MO_INPUT_NO_MEMORY);
            for (uint32_t p = 0; p < count; p++) {
                mo_value_drop(heaps[0], parameters[p], a[p]);
                mo_value_drop(heaps[1], parameters[p], b[p]);
            }
            MO_ASSERT(mo_heap_objects(heaps[0]) == 0 && mo_heap_objects(heaps[1]) == 0);
        }
        for (unsigned i = 0; i < 2; i++) {
            mo_heap_free(heaps[i]);
            mo_heap_image_free(images[i]);
        }
    } else
        MO_ASSERT(result == MO_INPUT_UNSUPPORTED || result == MO_INPUT_UNINHABITED ||
                  result == MO_INPUT_LIMIT || result == MO_INPUT_NO_MEMORY);
    mo_inputs_free(&second);
    mo_inputs_free(&first);
    mo_arena_free(&arena);
    return 0;
}
