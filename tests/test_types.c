#include "base/limits.h"
#include "heap/heap.h"
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static MO_MUST_USE uint32_t intern(struct mo_types *types, struct mo_type type) {
    uint32_t index = MO_NO_INDEX;
    MO_ASSERT(mo_types_intern(types, &type, &index) == MO_TYPES_OK);
    return index;
}

static MO_MUST_USE uint32_t declare(struct mo_types *types, enum mo_type_kind kind,
                                    const char *name) {
    uint32_t index = MO_NO_INDEX;
    MO_ASSERT(mo_types_declare(types, kind, name, &index) == MO_TYPES_OK);
    return index;
}

static MO_MUST_USE uint32_t option(struct mo_types *types, const char *name, uint32_t field) {
    uint32_t index = declare(types, MO_TYPE_CHOICE, name);
    struct mo_type_field value = {NULL, field};
    struct mo_type_case cases[] = {
        {.name = "None"},
        {.name = "Some", .fields = &value, .field_count = 1, .unnamed = true},
    };
    MO_ASSERT(mo_types_define_choice(types, index, cases, 2) == MO_TYPES_OK);
    return index;
}

static MO_MUST_USE uint32_t add_layout(struct mo_types *types, struct mo_layout layout) {
    uint32_t index = MO_NO_INDEX;
    MO_ASSERT(mo_types_layout(types, &layout, &index) == MO_TYPES_OK);
    return index;
}

static void test_interning_and_kinds(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t builtins[MO_TYPE_BIGINT + 1];
    for (unsigned kind = MO_TYPE_UNIT; kind <= MO_TYPE_BIGINT; kind++) {
        builtins[kind] = intern(&types, (struct mo_type){.kind = (enum mo_type_kind)kind});
        CHECK(intern(&types, (struct mo_type){.kind = (enum mo_type_kind)kind}) == builtins[kind]);
    }
    uint32_t integer = builtins[MO_TYPE_INT];
    uint32_t text = builtins[MO_TYPE_TEXT];
    uint32_t parameters[] = {integer, text};
    struct mo_type tuple_key = {
        .kind = MO_TYPE_TUPLE, .parameters = parameters, .parameter_count = 2};
    uint32_t tuple = intern(&types, tuple_key);
    CHECK(intern(&types, tuple_key) == tuple);
    parameters[0] = text;
    parameters[1] = integer;
    CHECK(intern(&types, tuple_key) != tuple);
    CHECK(types.items[tuple].parameters[0] == integer);
    uint32_t list = intern(
        &types,
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &integer, .parameter_count = 1});
    uint32_t set =
        intern(&types,
               (struct mo_type){.kind = MO_TYPE_SET, .parameters = &integer, .parameter_count = 1});
    uint32_t map = intern(
        &types,
        (struct mo_type){.kind = MO_TYPE_MAP, .parameters = parameters, .parameter_count = 2});
    uint32_t function = intern(&types, (struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                        .parameters = parameters,
                                                        .parameter_count = 2,
                                                        .result = integer});
    CHECK(function != intern(&types, (struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                      .parameters = parameters,
                                                      .parameter_count = 2,
                                                      .result = text}));
    CHECK(intern(&types, (struct mo_type){.kind = MO_TYPE_FUNCTION,
                                          .parameters = parameters,
                                          .parameter_count = 2,
                                          .result = integer}) == function);
    uint32_t environment = intern(&types, (struct mo_type){.kind = MO_TYPE_ENV, .function = 71});
    CHECK(environment == intern(&types, (struct mo_type){.kind = MO_TYPE_ENV, .function = 71}));
    CHECK(environment != intern(&types, (struct mo_type){.kind = MO_TYPE_ENV, .function = 72}));
    uint32_t capabilities[MO_CAP_RANDOM + 1];
    for (unsigned cap = MO_CAP_ARGS; cap <= MO_CAP_RANDOM; cap++) {
        struct mo_type key = {.kind = MO_TYPE_CAPABILITY, .capability = (enum mo_capability)cap};
        capabilities[cap] = intern(&types, key);
        CHECK(intern(&types, key) == capabilities[cap]);
    }
    /* Same printing name and fields, but different declarations: still nominal. */
    char name[] = "Page(Int)";
    uint32_t record = declare(&types, MO_TYPE_RECORD, name);
    name[0] = 'X';
    uint32_t other = declare(&types, MO_TYPE_RECORD, "Page(Int)");
    struct mo_type_field fields[] = {{"value", integer}, {"next", record}};
    CHECK(mo_types_define_record(&types, record, fields, 2) == MO_TYPES_OK);
    CHECK(mo_types_define_record(&types, other, fields, 2) == MO_TYPES_OK);
    fields[0].type = text;
    CHECK(types.items[record].fields[0].type == integer);
    CHECK(strcmp(types.items[record].name, "Page(Int)") == 0 && record != other);
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    for (unsigned kind = MO_TYPE_UNIT; kind <= MO_TYPE_BIGINT; kind++) {
        CHECK(mo_type_is_reference(&types, builtins[kind]) == (kind >= MO_TYPE_TEXT));
    }
    CHECK(mo_type_is_reference(&types, tuple));
    CHECK(mo_type_is_reference(&types, list));
    CHECK(mo_type_is_reference(&types, set));
    CHECK(mo_type_is_reference(&types, map));
    CHECK(mo_type_is_reference(&types, function));
    CHECK(mo_type_is_reference(&types, environment));
    CHECK(mo_type_is_reference(&types, record));
    for (unsigned cap = MO_CAP_ARGS; cap <= MO_CAP_RANDOM; cap++) {
        CHECK(mo_type_is_reference(&types, capabilities[cap]));
    }
    mo_arena_free(&arena);
}

static void test_shapes_and_layouts(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t unit = intern(&types, (struct mo_type){.kind = MO_TYPE_UNIT});
    uint32_t integer = intern(&types, (struct mo_type){.kind = MO_TYPE_INT});
    uint32_t text = intern(&types, (struct mo_type){.kind = MO_TYPE_TEXT});
    uint32_t decimal = intern(&types, (struct mo_type){.kind = MO_TYPE_DECIMAL});
    uint32_t range = intern(&types, (struct mo_type){.kind = MO_TYPE_RANGE});
    uint32_t ordering = declare(&types, MO_TYPE_CHOICE, "Ordering");
    struct mo_type_case order_cases[] = {{.name = "Less"}, {.name = "Equal"}, {.name = "Greater"}};
    CHECK(mo_types_define_choice(&types, ordering, order_cases, 3) == MO_TYPES_OK);
    uint32_t options[] = {
        option(&types, "Option(Int)", integer),       option(&types, "Option(Text)", text),
        option(&types, "Option(Decimal)", decimal),   option(&types, "Option(Range)", range),
        option(&types, "Option(Ordering)", ordering),
    };
    uint32_t nested = option(&types, "Option(Option(Int))", options[0]);
    uint32_t nested_ref = option(&types, "Option(Option(Text))", options[1]);
    uint32_t error = declare(&types, MO_TYPE_CHOICE, "FileError");
    struct mo_type_field path = {"path", text};
    struct mo_type_field failure[] = {{"path", text}, {"reason", text}};
    struct mo_type_case errors[] = {
        {.name = "NotFound", .fields = &path, .field_count = 1},
        {.name = "NotAllowed", .fields = &path, .field_count = 1},
        {.name = "NotText", .fields = &path, .field_count = 1},
        {.name = "Failed", .fields = failure, .field_count = 2},
    };
    CHECK(mo_types_define_choice(&types, error, errors, 4) == MO_TYPES_OK);
    uint32_t result = declare(&types, MO_TYPE_CHOICE, "Result(Unit, FileError)");
    struct mo_type_field ok = {NULL, unit};
    struct mo_type_field bad = {NULL, error};
    struct mo_type_case results[] = {
        {.name = "Ok", .fields = &ok, .field_count = 1, .unnamed = true},
        {.name = "Error", .fields = &bad, .field_count = 1, .unnamed = true},
    };
    CHECK(mo_types_define_choice(&types, result, results, 2) == MO_TYPES_OK);
    uint32_t record = declare(&types, MO_TYPE_RECORD, "Entry");
    struct mo_type_field record_fields[] = {{"name", text}, {"value", result}};
    CHECK(mo_types_define_record(&types, record, record_fields, 2) == MO_TYPES_OK);
    uint32_t record_option = option(&types, "Option(Entry)", record);
    uint32_t json = declare(&types, MO_TYPE_CHOICE, "Json");
    uint32_t list = intern(
        &types, (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &json, .parameter_count = 1});
    uint32_t map_params[] = {text, json};
    uint32_t map = intern(
        &types,
        (struct mo_type){.kind = MO_TYPE_MAP, .parameters = map_params, .parameter_count = 2});
    struct mo_type_field json_fields[] = {
        {NULL, integer}, {NULL, text}, {NULL, list}, {NULL, map}, {NULL, json}};
    struct mo_type_case json_cases[] = {
        {.name = "Null"},
        {.name = "Number", .fields = &json_fields[0], .field_count = 1, .unnamed = true},
        {.name = "String", .fields = &json_fields[1], .field_count = 1, .unnamed = true},
        {.name = "Array", .fields = &json_fields[2], .field_count = 1, .unnamed = true},
        {.name = "Object", .fields = &json_fields[3], .field_count = 1, .unnamed = true},
        {.name = "Alias", .fields = &json_fields[4], .field_count = 1, .unnamed = true},
    };
    CHECK(mo_types_define_choice(&types, json, json_cases, 6) == MO_TYPES_OK);
    uint32_t tuple_params[] = {integer, text};
    uint32_t tuple = intern(
        &types,
        (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = tuple_params, .parameter_count = 2});
    uint32_t function = intern(&types, (struct mo_type){.kind = MO_TYPE_FUNCTION, .result = unit});
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    enum mo_case_shape expected[] = {MO_CASE_INLINE, MO_CASE_REF, MO_CASE_BOXED, MO_CASE_BOXED,
                                     MO_CASE_INLINE};
    for (unsigned i = 0; i < 5; i++) {
        CHECK(mo_case_shape(&types, options[i], 0) == MO_CASE_EMPTY);
        CHECK(mo_case_shape(&types, options[i], 1) == expected[i]);
        CHECK(mo_type_is_reference(&types, options[i]) == (i >= 1 && i <= 3));
        CHECK(types.items[options[i]].cases[1].unnamed);
        CHECK(types.items[options[i]].cases[1].fields[0].name == NULL);
    }
    CHECK(!mo_type_is_reference(&types, ordering));
    CHECK(mo_case_shape(&types, nested, 1) == MO_CASE_BOXED);
    CHECK(mo_case_shape(&types, nested_ref, 1) == MO_CASE_BOXED);
    CHECK(mo_case_shape(&types, result, 0) == MO_CASE_INLINE);
    CHECK(mo_case_shape(&types, result, 1) == MO_CASE_BOXED);
    CHECK(mo_type_is_reference(&types, result));
    CHECK(mo_case_shape(&types, error, 0) == MO_CASE_REF);
    CHECK(mo_case_shape(&types, error, 3) == MO_CASE_BOXED);
    CHECK(!types.items[error].cases[0].unnamed);
    CHECK(mo_case_shape(&types, record_option, 1) == MO_CASE_REF);
    enum mo_case_shape json_expected[] = {MO_CASE_EMPTY, MO_CASE_INLINE, MO_CASE_REF,
                                          MO_CASE_REF,   MO_CASE_REF,    MO_CASE_BOXED};
    for (uint32_t i = 0; i < 6; i++) {
        CHECK(mo_case_shape(&types, json, i) == json_expected[i]);
    }
    CHECK(mo_type_is_reference(&types, json));
    uint32_t record_types[] = {text, result};
    struct mo_layout layout = {.kind = MO_LAYOUT_RECORD,
                               .type = record,
                               .case_index = MO_NO_INDEX,
                               .function = MO_NO_INDEX,
                               .fields = record_types,
                               .field_count = 2};
    uint32_t record_layout = add_layout(&types, layout);
    CHECK(add_layout(&types, layout) == record_layout);
    record_types[1] = unit;
    uint32_t out = 123;
    CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_INVALID && out == 123);
    CHECK(types.layouts[record_layout].fields[1] == result);
    layout = (struct mo_layout){.kind = MO_LAYOUT_TUPLE,
                                .type = tuple,
                                .case_index = MO_NO_INDEX,
                                .function = MO_NO_INDEX,
                                .fields = tuple_params,
                                .field_count = 2};
    CHECK(add_layout(&types, layout) != record_layout);
    layout = (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                .type = result,
                                .case_index = 1,
                                .function = MO_NO_INDEX,
                                .fields = &error,
                                .field_count = 1};
    uint32_t case_layout = add_layout(&types, layout);
    CHECK(types.layouts[case_layout].case_index == 1 &&
          types.layouts[case_layout].fields[0] == error);
    layout.type = options[1];
    layout.fields = &text;
    CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_INVALID);
    layout = (struct mo_layout){.kind = MO_LAYOUT_CLOSURE,
                                .type = function,
                                .case_index = MO_NO_INDEX,
                                .function = 27,
                                .fields = tuple_params,
                                .field_count = 2};
    uint32_t closure_layout = add_layout(&types, layout);
    CHECK(add_layout(&types, layout) == closure_layout);
    CHECK(types.layouts[closure_layout].function == 27 &&
          types.layouts[closure_layout].fields[0] == integer);
    tuple_params[0] = types.count;
    CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_INVALID);
    CHECK(types.layouts[closure_layout].fields[0] == integer);
    layout.type = types.count;
    CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_INVALID);
    mo_arena_free(&arena);
}

static void test_invalid_and_recursive_definitions(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t out = 123;
    struct mo_type key = {.kind = MO_TYPE_LIST, .parameters = &out, .parameter_count = 1};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_INVALID && types.count == 0 &&
          out == 123);
    key = (struct mo_type){.kind = (enum mo_type_kind)999};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_INVALID);
    key = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = (enum mo_capability)999};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_INVALID);
    CHECK(mo_types_declare(&types, MO_TYPE_CHOICE, "\xed\xa0\x80", &out) == MO_TYPES_INVALID);
    CHECK(mo_types_declare(&types, MO_TYPE_CHOICE, "\xc0\xaf", &out) == MO_TYPES_INVALID);
    CHECK(mo_types_declare(&types, MO_TYPE_CHOICE, "\xf4\x90\x80\x80", &out) == MO_TYPES_INVALID);
    CHECK(mo_types_declare(&types, MO_TYPE_CHOICE, "\xe2\x82", &out) == MO_TYPES_INVALID);
    CHECK(mo_types_declare(&types, MO_TYPE_CHOICE, "", &out) == MO_TYPES_INVALID);
    uint32_t a = declare(&types, MO_TYPE_CHOICE, "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    uint32_t b = declare(&types, MO_TYPE_CHOICE, "B");
    CHECK(mo_types_finish(&types) == MO_TYPES_INVALID);
    CHECK(mo_types_define_choice(&types, a, NULL, 0) == MO_TYPES_INVALID);
    struct mo_type_field field = {NULL, b};
    struct mo_type_case cases[] = {
        {.name = "Wrap", .fields = &field, .field_count = 1, .unnamed = true}, {.name = "Wrap"}};
    CHECK(mo_types_define_choice(&types, a, cases, 2) == MO_TYPES_INVALID);
    cases[0].unnamed = false;
    CHECK(mo_types_define_choice(&types, a, cases, 1) == MO_TYPES_INVALID);
    cases[0].unnamed = true;
    field.name = "value";
    CHECK(mo_types_define_choice(&types, a, cases, 1) == MO_TYPES_INVALID);
    field.name = NULL;
    cases[0].field_count = 0;
    CHECK(mo_types_define_choice(&types, a, cases, 1) == MO_TYPES_INVALID);
    cases[0].field_count = 1;
    CHECK(mo_types_define_choice(&types, a, cases, 1) == MO_TYPES_OK);
    field.type = a;
    CHECK(mo_types_define_choice(&types, b, cases, 1) == MO_TYPES_OK);
    CHECK(mo_types_define_choice(&types, b, cases, 1) == MO_TYPES_INVALID);
    uint32_t record = declare(&types, MO_TYPE_RECORD, "Record");
    struct mo_type_field fields[] = {{"x", a}, {"x", b}};
    CHECK(mo_types_define_record(&types, record, fields, 2) == MO_TYPES_INVALID);
    fields[1].name = "y";
    fields[1].type = types.count;
    CHECK(mo_types_define_record(&types, record, fields, 2) == MO_TYPES_INVALID);
    fields[1].type = b;
    CHECK(mo_types_define_record(&types, record, fields, 2) == MO_TYPES_OK);
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    CHECK(mo_case_shape(&types, a, 0) == MO_CASE_BOXED &&
          mo_case_shape(&types, b, 0) == MO_CASE_BOXED);
    CHECK(mo_type_is_reference(&types, a) && mo_type_is_reference(&types, b));
    key = (struct mo_type){.kind = MO_TYPE_UNIT};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_INVALID);
    CHECK(mo_types_declare(&types, MO_TYPE_RECORD, "More", &out) == MO_TYPES_INVALID);
    mo_arena_free(&arena);
}

/* Actual boundaries, not altered table counters: interning must still find an
 * existing entry when the table is full, and reject only the next distinct one. */
static void test_table_limits(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, (size_t)512 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    for (uint32_t i = 0; i < MO_LIMIT_TYPES; i++) {
        CHECK(intern(&types, (struct mo_type){.kind = MO_TYPE_ENV, .function = i}) == i);
    }
    CHECK(types.count == MO_LIMIT_TYPES);
    uint32_t out = 123;
    struct mo_type key = {.kind = MO_TYPE_UNIT};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_TYPE_LIMIT && out == 123);
    CHECK(mo_types_declare(&types, MO_TYPE_RECORD, "Overflow", &out) == MO_TYPES_TYPE_LIMIT);
    CHECK(intern(&types, (struct mo_type){.kind = MO_TYPE_ENV, .function = 971}) == 971);
    key = (struct mo_type){.kind = MO_TYPE_ENV, .function = MO_LIMIT_FUNCTIONS};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_INVALID);
    mo_arena_free(&arena);
    mo_types_init(&types, &arena);
    uint32_t unit = intern(&types, (struct mo_type){.kind = MO_TYPE_UNIT});
    uint32_t function = intern(&types, (struct mo_type){.kind = MO_TYPE_FUNCTION, .result = unit});
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    struct mo_layout layout = {
        .kind = MO_LAYOUT_CLOSURE, .type = function, .case_index = MO_NO_INDEX};
    for (uint32_t i = 0; i < MO_LIMIT_LAYOUTS; i++) {
        layout.function = i;
        CHECK(add_layout(&types, layout) == i);
    }
    /* A different capture list makes another valid layout within index limits. */
    layout.fields = &unit;
    layout.field_count = 1;
    CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_LAYOUT_LIMIT);
    layout.fields = NULL;
    layout.field_count = 0;
    CHECK(add_layout(&types, layout) == MO_LIMIT_LAYOUTS - 1);
    layout.function = MO_LIMIT_FUNCTIONS;
    CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_INVALID);
    mo_arena_free(&arena);
}

static void test_subject_and_promise_parameter_boundary(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t integer = intern(&types, (struct mo_type){.kind = MO_TYPE_INT});
    uint32_t text = intern(&types, (struct mo_type){.kind = MO_TYPE_TEXT});
    uint32_t boolean = intern(&types, (struct mo_type){.kind = MO_TYPE_BOOL});
    uint32_t parameters[257];
    for (uint32_t i = 0; i < 255; i++) {
        parameters[i] = i % 2 == 0 ? text : integer;
    }
    struct mo_type signature = {.kind = MO_TYPE_FUNCTION,
                                .parameters = parameters,
                                .parameter_count = 255,
                                .result = boolean};
    uint32_t subject = intern(&types, signature);
    parameters[255] = boolean; /* subject's result, not another copy of its last input */
    signature.parameter_count = 256;
    signature.result = integer; /* promise's failing-line number */
    uint32_t promise = intern(&types, signature);
    CHECK(subject != promise);
    CHECK(intern(&types, signature) == promise);
    parameters[256] = integer;
    signature.parameter_count = 257;
    uint32_t count = types.count;
    uint32_t out = 123;
    CHECK(mo_types_intern(&types, &signature, &out) == MO_TYPES_PARAMETER_LIMIT);
    CHECK(types.count == count && out == 123);
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    CHECK(types.items[subject].parameter_count == 255);
    CHECK(types.items[subject].result == boolean);
    CHECK(types.items[promise].parameter_count == 256);
    CHECK(types.items[promise].result == integer);
    for (uint32_t i = 0; i < 255; i++) {
        uint32_t expected = i % 2 == 0 ? text : integer;
        CHECK(types.items[subject].parameters[i] == expected);
        CHECK(types.items[promise].parameters[i] == expected);
    }
    CHECK(types.items[promise].parameters[255] == boolean);
    mo_arena_free(&arena);
}

static void test_counts_and_name_limits(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, (size_t)128 << 20);
    struct mo_types types;
    mo_types_init(&types, &arena);
    uint32_t unit = intern(&types, (struct mo_type){.kind = MO_TYPE_UNIT});
    uint32_t *indices =
        mo_arena_allocate_array(&arena, MO_LIMIT_FIELDS + 1, sizeof *indices, _Alignof(uint32_t));
    struct mo_type_field *fields = mo_arena_allocate_array(&arena, MO_LIMIT_FIELDS, sizeof *fields,
                                                           _Alignof(struct mo_type_field));
    struct mo_type_case *cases = mo_arena_allocate_array(
        &arena, MO_LIMIT_CHOICE_CASES, sizeof *cases, _Alignof(struct mo_type_case));
    char(*names)[16] = mo_arena_allocate_array(&arena, MO_LIMIT_FIELDS, sizeof *names, 1);
    MO_ASSERT(indices != NULL && fields != NULL && cases != NULL && names != NULL);
    for (uint32_t i = 0; i < MO_LIMIT_FIELDS; i++) {
        indices[i] = unit;
        int written = snprintf(names[i], sizeof names[i], "f%u", i);
        MO_ASSERT(written > 0 && (size_t)written < sizeof names[i]);
        fields[i] = (struct mo_type_field){names[i], unit};
        cases[i] = (struct mo_type_case){.name = names[i]};
    }
    uint32_t out = 123;
    struct mo_type key = {
        .kind = MO_TYPE_TUPLE, .parameters = indices, .parameter_count = MO_LIMIT_FIELDS + 1};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_FIELD_LIMIT);
    key.parameter_count = MO_LIMIT_FIELDS;
    uint32_t tuple = intern(&types, key);
    key = (struct mo_type){.kind = MO_TYPE_FUNCTION,
                           .parameters = indices,
                           .parameter_count = MO_LIMIT_BYTECODE_PARAMETERS + 1,
                           .result = unit};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_PARAMETER_LIMIT);
    key.parameter_count = MO_LIMIT_BYTECODE_PARAMETERS;
    uint32_t function = intern(&types, key);
    uint32_t record = declare(&types, MO_TYPE_RECORD, "LargeRecord");
    CHECK(mo_types_define_record(&types, record, NULL, MO_LIMIT_FIELDS + 1) ==
          MO_TYPES_FIELD_LIMIT);
    CHECK(mo_types_define_record(&types, record, fields, MO_LIMIT_FIELDS) == MO_TYPES_OK);
    uint32_t choice = declare(&types, MO_TYPE_CHOICE, "LargeChoice");
    CHECK(mo_types_define_choice(&types, choice, NULL, MO_LIMIT_CHOICE_CASES + 1) ==
          MO_TYPES_CASE_LIMIT);
    CHECK(mo_types_define_choice(&types, choice, cases, MO_LIMIT_CHOICE_CASES) == MO_TYPES_OK);
    uint32_t wide = declare(&types, MO_TYPE_CHOICE, "WideCase");
    struct mo_type_case wide_case = {
        .name = "Wide", .fields = fields, .field_count = MO_LIMIT_FIELDS + 1};
    CHECK(mo_types_define_choice(&types, wide, &wide_case, 1) == MO_TYPES_FIELD_LIMIT);
    wide_case.field_count = MO_LIMIT_FIELDS;
    CHECK(mo_types_define_choice(&types, wide, &wide_case, 1) == MO_TYPES_OK);
    char *long_name = mo_arena_allocate(&arena, MO_LIMIT_STRING_SIZE + 2, 1);
    MO_ASSERT(long_name != NULL);
    memset(long_name, 'n', MO_LIMIT_STRING_SIZE + 1);
    long_name[MO_LIMIT_STRING_SIZE + 1] = '\0';
    CHECK(mo_types_declare(&types, MO_TYPE_RECORD, long_name, &out) == MO_TYPES_STRING_LIMIT);
    uint32_t named = declare(&types, MO_TYPE_RECORD, "Named");
    struct mo_type_field named_field = {long_name, unit};
    CHECK(mo_types_define_record(&types, named, &named_field, 1) == MO_TYPES_STRING_LIMIT);
    uint32_t named_choice = declare(&types, MO_TYPE_CHOICE, "NamedChoice");
    struct mo_type_case named_case = {.name = long_name};
    CHECK(mo_types_define_choice(&types, named_choice, &named_case, 1) == MO_TYPES_STRING_LIMIT);
    named_case.name = "Case";
    named_case.fields = &named_field;
    named_case.field_count = 1;
    CHECK(mo_types_define_choice(&types, named_choice, &named_case, 1) == MO_TYPES_STRING_LIMIT);
    long_name[MO_LIMIT_STRING_SIZE] = '\0';
    uint32_t long_type = declare(&types, MO_TYPE_RECORD, long_name);
    CHECK(mo_types_define_record(&types, long_type, NULL, 0) == MO_TYPES_OK);
    CHECK(mo_types_define_record(&types, named, &named_field, 1) == MO_TYPES_OK);
    named_case.name = long_name;
    CHECK(mo_types_define_choice(&types, named_choice, &named_case, 1) == MO_TYPES_OK);
    CHECK(mo_types_finish(&types) == MO_TYPES_OK);
    CHECK(!mo_type_is_reference(&types, choice));
    CHECK(mo_case_shape(&types, wide, 0) == MO_CASE_BOXED);
    enum mo_layout_kind kinds[] = {MO_LAYOUT_TUPLE, MO_LAYOUT_RECORD, MO_LAYOUT_CASE,
                                   MO_LAYOUT_CLOSURE};
    uint32_t layout_types[] = {tuple, record, wide, function};
    for (unsigned i = 0; i < 4; i++) {
        struct mo_layout layout = {.kind = kinds[i],
                                   .type = layout_types[i],
                                   .case_index = i == 2 ? 0 : MO_NO_INDEX,
                                   .function = i == 3 ? 0 : MO_NO_INDEX,
                                   .fields = indices,
                                   .field_count = MO_LIMIT_FIELDS + 1};
        CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_FIELD_LIMIT);
        layout.field_count = MO_LIMIT_FIELDS;
        CHECK(mo_types_layout(&types, &layout, &out) == MO_TYPES_OK);
    }
    mo_arena_free(&arena);
}

static void test_allocation_failures(void) {
    struct mo_arena arena;
    struct mo_types types;
    /* Failure can happen after copying a name, growing items, or copying fields.
     * Retry only after making room, and never observe a half-defined entry. */
    for (size_t limit = 0; limit < 4096; limit += 37) {
        mo_arena_init(&arena, limit);
        mo_types_init(&types, &arena);
        uint32_t out = 123;
        enum mo_type_problem problem = mo_types_declare(&types, MO_TYPE_RECORD, "R", &out);
        CHECK(problem == MO_TYPES_OK || problem == MO_TYPES_NO_MEMORY);
        if (problem != MO_TYPES_OK) {
            CHECK(types.count == 0 && out == 123);
        } else {
            uint32_t record = out;
            struct mo_type_field field = {"self", record};
            problem = mo_types_define_record(&types, record, &field, 1);
            CHECK(problem == MO_TYPES_OK || problem == MO_TYPES_NO_MEMORY);
            CHECK(types.items[record].defined == (problem == MO_TYPES_OK));
        }
        mo_arena_free(&arena);
    }
    mo_arena_init(&arena, 0);
    mo_types_init(&types, &arena);
    uint32_t out = 123;
    struct mo_type key = {.kind = MO_TYPE_UNIT};
    CHECK(mo_types_intern(&types, &key, &out) == MO_TYPES_NO_MEMORY && out == 123);
    CHECK(types.count == 0);
    mo_arena_free(&arena);
}

static void test_cells(void) {
    struct mo_cell zero = mo_cell_unit();
    for (unsigned i = 0; i < 16; i++) {
        CHECK(zero.bytes[i] == 0);
    }
    CHECK(mo_cell_get_bool(mo_cell_bool(true)));
    CHECK(!mo_cell_get_bool(mo_cell_bool(false)));
    CHECK(mo_cell_get_byte(mo_cell_byte(253)) == 253);
    CHECK(mo_cell_get_int(mo_cell_int(INT64_MIN)) == INT64_MIN);
    CHECK(mo_cell_get_int(mo_cell_int(INT64_MAX)) == INT64_MAX);
    CHECK(mo_cell_get_float(mo_cell_float(-3.125)) == -3.125);
    CHECK(signbit(mo_cell_get_float(mo_cell_float(-0.0))));
    CHECK(isnan(mo_cell_get_float(mo_cell_float(NAN))));
    CHECK(mo_cell_get_char(mo_cell_char(0x10ffff)) == 0x10ffff);
    CHECK(mo_cell_get_char(mo_cell_char(0xd7ff)) == 0xd7ff);
    CHECK(mo_cell_get_char(mo_cell_char(0xe000)) == 0xe000);
    CHECK(mo_cell_get_time(mo_cell_time(INT64_C(-62135596800000000))) ==
          INT64_C(-62135596800000000));
    CHECK(mo_cell_get_time(mo_cell_time(INT64_C(253402300799999999))) ==
          INT64_C(253402300799999999));
    CHECK(mo_cell_get_duration(mo_cell_duration(INT64_MIN)) == INT64_MIN);
    mo_i128 max = (mo_i128)(((mo_u128)1 << 127) - 1);
    CHECK(mo_cell_get_decimal(mo_cell_decimal(max)) == max);
    CHECK(mo_cell_get_decimal(mo_cell_decimal(-max)) == -max);
    struct {
        struct mo_object header;
        struct mo_cell fields[2];
    } block = {.header = {1, 7}, .fields = {mo_cell_decimal(-max), mo_cell_range(-17, 43)}};
    CHECK((uint8_t *)&block.fields[0] - (uint8_t *)&block == 8);
    CHECK(mo_cell_get_decimal(block.fields[0]) == -max);
    struct mo_range range = mo_cell_get_range(mo_cell_range(-17, 43));
    CHECK(range.start == -17 && range.end == 43);
    struct mo_cell small[] = {mo_cell_bool(true),   mo_cell_byte(253),      mo_cell_int(-71),
                              mo_cell_float(1.25),  mo_cell_char(0x10ffff), mo_cell_time(0),
                              mo_cell_duration(-42)};
    unsigned sizes[] = {1, 1, 8, 8, 4, 8, 8};
    for (unsigned i = 0; i < sizeof small / sizeof small[0]; i++) {
        for (unsigned j = sizes[i]; j < 16; j++) {
            CHECK(small[i].bytes[j] == 0);
        }
    }
    struct mo_object object = {.count = MO_IMMORTAL_COUNT, .layout = 19};
    struct mo_cell ref = mo_cell_ref(&object);
    CHECK(mo_cell_get_ref(ref) == &object);
    for (unsigned i = 8; i < 16; i++) {
        CHECK(ref.bytes[i] == 0);
    }
    struct mo_cell choice = mo_cell_case(71, ref);
    CHECK(mo_cell_get_tag(choice) == 71);
    CHECK(mo_cell_get_ref(mo_cell_get_payload(choice)) == &object);
    for (unsigned i = 4; i < 8; i++) {
        CHECK(choice.bytes[i] == 0);
    }
    choice = mo_cell_case(2, mo_cell_int(-917));
    CHECK(mo_cell_get_int(mo_cell_get_payload(choice)) == -917);
    choice = mo_cell_case(65534, zero);
    CHECK(mo_cell_get_tag(choice) == 65534);
    for (unsigned i = 4; i < 16; i++) {
        CHECK(choice.bytes[i] == 0);
    }
}

int main(void) {
    test_interning_and_kinds();
    test_shapes_and_layouts();
    test_invalid_and_recursive_definitions();
    test_table_limits();
    test_subject_and_promise_parameter_boundary();
    test_counts_and_name_limits();
    test_allocation_failures();
    test_cells();
    return mo_test_report("test_types");
}
