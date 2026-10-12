/* Generated structural types must intern to the same index on repetition.
 * Mutated names/nominal definitions either fail without publication or finish
 * with case shapes that fit their fields. All walks stay off the C stack. */
#include "heap/types.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum { MOST_BYTES = 1024, MOST_FIELDS = 4 };

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > MOST_BYTES) {
        size = MOST_BYTES;
    }
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t index;
    for (unsigned kind = MO_TYPE_UNIT; kind <= MO_TYPE_BIGINT; kind++) {
        struct mo_type key = {.kind = (enum mo_type_kind)kind};
        MO_ASSERT(mo_types_intern(&types, &key, &index) == MO_TYPES_OK);
    }
    uint32_t choice;
    MO_ASSERT(mo_types_declare(&types, MO_TYPE_CHOICE, "Recursive", &choice) == MO_TYPES_OK);
    for (size_t at = 0; at + 3 < size; at += 4) {
        uint32_t parameters[MOST_FIELDS];
        for (unsigned i = 0; i < MOST_FIELDS; i++) {
            parameters[i] = data[at + i] % types.count;
        }
        enum mo_type_kind kind = (enum mo_type_kind)(data[at] % (MO_TYPE_CAPABILITY + 2));
        uint32_t count = data[at + 1] % (MOST_FIELDS + 1);
        if (kind == MO_TYPE_LIST || kind == MO_TYPE_SET) {
            count = 1;
        }
        if (kind == MO_TYPE_MAP || kind == MO_TYPE_TUPLE) {
            count = 2;
        }
        if (kind <= MO_TYPE_BIGINT || kind == MO_TYPE_ENV || kind == MO_TYPE_CAPABILITY) {
            count = 0;
        }
        struct mo_type key = {.kind = kind,
                              .parameters = parameters,
                              .parameter_count = count,
                              .result = parameters[0],
                              .function = data[at + 2],
                              .capability = (enum mo_capability)data[at + 3]};
        uint32_t before = types.count;
        index = MO_NO_INDEX;
        enum mo_type_problem problem = mo_types_intern(&types, &key, &index);
        if (problem == MO_TYPES_OK) {
            uint32_t repeated;
            MO_ASSERT(mo_types_intern(&types, &key, &repeated) == MO_TYPES_OK);
            MO_ASSERT(repeated == index && types.count <= before + 1);
        } else {
            MO_ASSERT(problem == MO_TYPES_INVALID && index == MO_NO_INDEX && types.count == before);
        }
    }
    char name[MOST_BYTES + 1];
    memcpy(name, data, size);
    name[size] = '\0';
    uint32_t named;
    enum mo_type_problem problem = mo_types_declare(&types, MO_TYPE_RECORD, name, &named);
    MO_ASSERT(problem == MO_TYPES_OK || problem == MO_TYPES_INVALID);
    if (problem == MO_TYPES_OK) {
        MO_ASSERT(mo_types_define_record(&types, named, NULL, 0) == MO_TYPES_OK);
    }
    struct mo_type_field fields[MOST_FIELDS];
    const char *names[] = {"a", "b", "c", "d"};
    uint32_t count = size == 0 ? 0 : data[0] % (MOST_FIELDS + 1);
    for (uint32_t i = 0; i < count; i++) {
        fields[i] = (struct mo_type_field){names[i], data[i % size] % types.count};
    }
    struct mo_type_case cases[] = {{.name = "Empty"},
                                   {.name = "Fields", .fields = fields, .field_count = count}};
    MO_ASSERT(mo_types_define_choice(&types, choice, cases, 2) == MO_TYPES_OK);
    MO_ASSERT(mo_types_finish(&types) == MO_TYPES_OK);
    MO_ASSERT(mo_case_shape(&types, choice, 0) == MO_CASE_EMPTY);
    enum mo_case_shape shape = mo_case_shape(&types, choice, 1);
    if (count == 0) {
        MO_ASSERT(shape == MO_CASE_EMPTY);
    }
    if (count > 1) {
        MO_ASSERT(shape == MO_CASE_BOXED);
    }
    if (count == 1) {
        const struct mo_type *field = &types.items[fields[0].type];
        if (field->kind == MO_TYPE_CHOICE || field->kind == MO_TYPE_DECIMAL ||
            field->kind == MO_TYPE_RANGE) {
            MO_ASSERT(shape == MO_CASE_BOXED);
        } else if (mo_type_is_reference(&types, fields[0].type)) {
            MO_ASSERT(shape == MO_CASE_REF);
        } else {
            MO_ASSERT(shape == MO_CASE_INLINE);
        }
    }
    MO_ASSERT(mo_type_is_reference(&types, choice) ==
              (shape == MO_CASE_REF || shape == MO_CASE_BOXED));
    mo_arena_free(&arena);
    return 0;
}
