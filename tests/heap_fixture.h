#ifndef MO_HEAP_FIXTURE_H
#define MO_HEAP_FIXTURE_H

#include "heap/heap.h"

struct heap_fixture {
    struct mo_arena arena;
    struct mo_types types;
    uint32_t builtin[MO_TYPE_BIGINT + 1];
    uint32_t list;
    uint32_t map;
    uint32_t set;
    uint32_t map_keys;
    uint32_t node;
    uint32_t option;
    uint32_t tuple;
    uint32_t function;
    uint32_t node_layout;
    uint32_t tuple_layout;
    uint32_t case_layout;
};

static MO_MUST_USE uint32_t heap_intern(struct mo_types *types, struct mo_type type) {
    uint32_t out;
    MO_ASSERT(mo_types_intern(types, &type, &out) == MO_TYPES_OK);
    return out;
}

static MO_MUST_USE uint32_t heap_layout(struct mo_types *types, struct mo_layout layout) {
    uint32_t out;
    MO_ASSERT(mo_types_layout(types, &layout, &out) == MO_TYPES_OK);
    return out;
}

static void heap_fixture_init(struct heap_fixture *fixture) {
    mo_arena_init(&fixture->arena, 1 << 20);
    mo_types_init(&fixture->types, &fixture->arena);
    struct mo_types *types = &fixture->types;
    for (unsigned i = 0; i <= MO_TYPE_BIGINT; i++) {
        fixture->builtin[i] = heap_intern(types, (struct mo_type){.kind = (enum mo_type_kind)i});
    }
    uint32_t text = fixture->builtin[MO_TYPE_TEXT];
    fixture->list = heap_intern(
        types, (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &text, .parameter_count = 1});
    MO_ASSERT(mo_types_declare(types, MO_TYPE_CHOICE, "Node", &fixture->node) == MO_TYPES_OK);
    struct mo_type_field tail = {NULL, fixture->node};
    struct mo_type_case nodes[] = {
        {.name = "End"}, {.name = "Next", .fields = &tail, .field_count = 1, .unnamed = true}};
    MO_ASSERT(mo_types_define_choice(types, fixture->node, nodes, 2) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(types, MO_TYPE_CHOICE, "Option", &fixture->option) == MO_TYPES_OK);
    struct mo_type_field fields[] = {{NULL, fixture->builtin[MO_TYPE_INT]}, {NULL, text}};
    struct mo_type_case options[] = {
        {.name = "None"},
        {.name = "Number", .fields = fields, .field_count = 1, .unnamed = true},
        {.name = "Text", .fields = fields + 1, .field_count = 1, .unnamed = true},
        {.name = "Box", .fields = &tail, .field_count = 1, .unnamed = true},
    };
    MO_ASSERT(mo_types_define_choice(types, fixture->option, options, 4) == MO_TYPES_OK);
    uint32_t pair[] = {text, fixture->list};
    fixture->map = heap_intern(
        types, (struct mo_type){.kind = MO_TYPE_MAP, .parameters = pair, .parameter_count = 2});
    fixture->set = heap_intern(
        types, (struct mo_type){.kind = MO_TYPE_SET, .parameters = &text, .parameter_count = 1});
    uint32_t map_pair[] = {fixture->map, text};
    fixture->map_keys = heap_intern(
        types, (struct mo_type){.kind = MO_TYPE_MAP, .parameters = map_pair, .parameter_count = 2});
    fixture->tuple = heap_intern(
        types, (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = pair, .parameter_count = 2});
    fixture->function =
        heap_intern(types, (struct mo_type){.kind = MO_TYPE_FUNCTION,
                                            .result = fixture->builtin[MO_TYPE_UNIT]});
    MO_ASSERT(mo_types_finish(types) == MO_TYPES_OK);
    fixture->node_layout = heap_layout(types, (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                                                 .type = fixture->node,
                                                                 .case_index = 1,
                                                                 .function = MO_NO_INDEX,
                                                                 .fields = &fixture->node,
                                                                 .field_count = 1});
    fixture->case_layout = heap_layout(types, (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                                                 .type = fixture->option,
                                                                 .case_index = 3,
                                                                 .function = MO_NO_INDEX,
                                                                 .fields = &fixture->node,
                                                                 .field_count = 1});
    fixture->tuple_layout = heap_layout(types, (struct mo_layout){.kind = MO_LAYOUT_TUPLE,
                                                                  .type = fixture->tuple,
                                                                  .case_index = MO_NO_INDEX,
                                                                  .function = MO_NO_INDEX,
                                                                  .fields = pair,
                                                                  .field_count = 2});
    uint32_t layout = heap_layout(types, (struct mo_layout){.kind = MO_LAYOUT_CLOSURE,
                                                            .type = fixture->function,
                                                            .case_index = MO_NO_INDEX,
                                                            .function = 7,
                                                            .fields = pair,
                                                            .field_count = 2});
    MO_ASSERT(layout != MO_NO_INDEX);
    layout = heap_layout(types, (struct mo_layout){.kind = MO_LAYOUT_CLOSURE,
                                                   .type = fixture->function,
                                                   .case_index = MO_NO_INDEX,
                                                   .function = 8});
    MO_ASSERT(layout != MO_NO_INDEX);
}

#endif
