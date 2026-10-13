#ifndef MO_PRIM_FIXTURE_H
#define MO_PRIM_FIXTURE_H

#include "prim/prim.h"

#include <string.h>

struct prim_fixture {
    struct mo_arena arena;
    struct mo_types types;
    struct mo_heap_image *image;
    struct mo_heap *heap;
    uint32_t builtin[MO_TYPE_BIGINT + 1];
    uint32_t signature[MO_PRIM_COUNT];
    uint32_t options[MO_TYPE_BIGINT + 1];
};

static MO_MUST_USE uint32_t prim_type(struct prim_fixture *f, const char **cursor) {
    const char *p = *cursor;
    while (*p == ' ')
        p++;
    uint32_t parameters[4], count = 0, result = MO_NO_INDEX;
    enum mo_type_kind kind = MO_TYPE_TUPLE;
    if (*p != '(') {
        const char *start = p;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
            p++;
        size_t length = (size_t)(p - start);
        static const char *const names[] = {"Unit", "Bool",  "Byte",     "Int",     "Float",
                                            "Char", "Time",  "Duration", "Decimal", "Range",
                                            "Text", "Bytes", "BigInt"};
        for (unsigned i = 0; i <= MO_TYPE_BIGINT; i++) {
            if (strlen(names[i]) == length && memcmp(start, names[i], length) == 0) {
                *cursor = p;
                return f->builtin[i];
            }
        }
        if (length == 1) {
            MO_ASSERT(*start == 'T' || *start == 'U' || *start == 'K' || *start == 'V');
            *cursor = p;
            return f->builtin[*start == 'U' || *start == 'V' ? MO_TYPE_TEXT : MO_TYPE_INT];
        }
        if (length == 2 && memcmp(start, "fn", 2) == 0)
            kind = MO_TYPE_FUNCTION;
        else if (length == 4 && memcmp(start, "List", 4) == 0)
            kind = MO_TYPE_LIST;
        else if (length == 3 && memcmp(start, "Map", 3) == 0)
            kind = MO_TYPE_MAP;
        else if (length == 3 && memcmp(start, "Set", 3) == 0)
            kind = MO_TYPE_SET;
        else if (length == 6 && memcmp(start, "Option", 6) == 0) {
            MO_ASSERT(*p++ == '(');
            uint32_t element = prim_type(f, &p);
            MO_ASSERT(*p++ == ')' && element <= MO_TYPE_BIGINT);
            if (f->options[element] == MO_NO_INDEX) {
                uint32_t index;
                MO_ASSERT(mo_types_declare(&f->types, MO_TYPE_CHOICE, "Option", &index) ==
                          MO_TYPES_OK);
                struct mo_type_field field = {.type = element};
                struct mo_type_case cases[] = {
                    {.name = "None"},
                    {.name = "Some", .fields = &field, .field_count = 1, .unnamed = true}};
                MO_ASSERT(mo_types_define_choice(&f->types, index, cases, 2) == MO_TYPES_OK);
                f->options[element] = index;
            }
            *cursor = p;
            return f->options[element];
        } else if (length == 8 && memcmp(start, "Ordering", 8) == 0) {
            uint32_t index;
            MO_ASSERT(mo_types_declare(&f->types, MO_TYPE_CHOICE, "Ordering", &index) ==
                      MO_TYPES_OK);
            struct mo_type_case cases[] = {{.name = "Less"}, {.name = "Same"}, {.name = "More"}};
            MO_ASSERT(mo_types_define_choice(&f->types, index, cases, 3) == MO_TYPES_OK);
            *cursor = p;
            return index;
        } else {
            MO_UNREACHABLE();
        }
    }
    MO_ASSERT(*p++ == '(');
    while (*p != ')') {
        MO_ASSERT(count < 4);
        parameters[count++] = prim_type(f, &p);
        if (*p != ')') {
            MO_ASSERT(*p++ == ',');
            while (*p == ' ')
                p++;
        }
    }
    p++;
    if (kind == MO_TYPE_FUNCTION) {
        while (*p == ' ')
            p++;
        MO_ASSERT(*p++ == ':');
        result = prim_type(f, &p);
    }
    struct mo_type type = {
        .kind = kind, .parameters = parameters, .parameter_count = count, .result = result};
    uint32_t index;
    MO_ASSERT(mo_types_intern(&f->types, &type, &index) == MO_TYPES_OK);
    *cursor = p;
    return index;
}

static void prim_fixture_init(struct prim_fixture *f, size_t budget) {
    memset(f, 0, sizeof *f);
    mo_arena_init(&f->arena, 4 << 20);
    mo_types_init(&f->types, &f->arena);
    for (unsigned i = 0; i <= MO_TYPE_BIGINT; i++) {
        struct mo_type t = {.kind = (enum mo_type_kind)i};
        MO_ASSERT(mo_types_intern(&f->types, &t, &f->builtin[i]) == MO_TYPES_OK);
        f->options[i] = MO_NO_INDEX;
    }
    /* The parser below needs only the numeric signatures until generic options are added. */
#define PRIM(id, module, name, spelling, params, bugs)                                             \
    if (MO_PRIM_##id <= MO_PRIM_FLOAT_TO_TEXT &&                                                   \
        !(MO_PRIM_##id >= MO_PRIM_BIGINT_ADD && MO_PRIM_##id <= MO_PRIM_BIGINT_TO_TEXT)) {         \
        const char *cursor = spelling;                                                             \
        f->signature[MO_PRIM_##id] = prim_type(f, &cursor);                                        \
    }
#include "base/prims.def"
    MO_ASSERT(mo_types_finish(&f->types) == MO_TYPES_OK);
    for (uint32_t i = 0; i < f->types.count; i++) {
        const struct mo_type *t = &f->types.items[i];
        if (t->kind == MO_TYPE_CHOICE) {
            for (uint32_t k = 0; k < t->case_count; k++) {
                if (mo_case_shape(&f->types, i, k) == MO_CASE_BOXED) {
                    uint32_t field = t->cases[k].fields[0].type, index;
                    struct mo_layout layout = {.kind = MO_LAYOUT_CASE,
                                               .type = i,
                                               .case_index = k,
                                               .function = MO_NO_INDEX,
                                               .fields = &field,
                                               .field_count = 1};
                    MO_ASSERT(mo_types_layout(&f->types, &layout, &index) == MO_TYPES_OK);
                }
            }
        }
    }
    struct mo_limits limits = mo_limits_default();
    f->image = mo_heap_image_new(&f->types, &limits);
    MO_ASSERT(f->image != NULL);
    limits.heap_size = budget;
    f->heap = mo_heap_new(f->image, &limits);
    MO_ASSERT(f->heap != NULL);
}

static void prim_fixture_free(struct prim_fixture *f) {
    mo_heap_free(f->heap);
    mo_heap_image_free(f->image);
    mo_arena_free(&f->arena);
}

#endif
