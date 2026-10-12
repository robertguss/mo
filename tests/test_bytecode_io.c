#include "bytecode/io.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>

/* The first eight types and four layouts are section 3's worked Chain encoding. Extra types
 * cover every kind, all seven capabilities, all choice shapes and a second nominal identity. */
#define STRING(text)                                                                               \
    { sizeof(text) - 1, (const uint8_t *)(text) }
static const struct mo_bytecode_string strings[] = {STRING("Chain"),
                                                    STRING("End"),
                                                    STRING("Link"),
                                                    STRING("value"),
                                                    STRING("next"),
                                                    STRING("Alias"),
                                                    STRING("Counts"),
                                                    STRING("lines"),
                                                    STRING("words"),
                                                    STRING("bytes"),
                                                    STRING("Some"),
                                                    STRING("Wc"),
                                                    STRING("Wc.add"),
                                                    STRING("Wc.fn.1"),
                                                    STRING("Wc.total.init"),
                                                    STRING("Wc.test.1"),
                                                    STRING("Wc.add.example.1"),
                                                    STRING("Wc.add.promise.1"),
                                                    STRING("Wc.add.promise.1.allows.1"),
                                                    STRING("wc.mo"),
                                                    STRING("a"),
                                                    STRING("a\0\xc3\xa9"),
                                                    STRING(""),
                                                    STRING("Chain")};
static const uint32_t chain[] = {3}, pair[] = {1, 4}, map_args[] = {2, 1}, element[] = {1};
static const struct mo_type_field link_fields[] = {{"value", 1}, {"next", 3}};
static const struct mo_type_field alias_fields[] = {{NULL, 3}};
static const struct mo_type_case chain_cases[] = {
    {.name = "End"},
    {.name = "Link", .fields = link_fields, .field_count = 2},
    {.name = "Alias", .fields = alias_fields, .field_count = 1}};
static const struct mo_type_field count_fields[] = {{"lines", 1}, {"words", 1}, {"bytes", 1}};
static const struct mo_type_case ordering_cases[] = {{.name = "End"}, {.name = "Link"}};
static const struct mo_type_field inline_field[] = {{NULL, 1}}, ref_field[] = {{NULL, 2}},
                                  decimal_field[] = {{NULL, 14}};
static const struct mo_type_case shape_cases[] = {
    {.name = "End"},
    {.name = "Some", .fields = inline_field, .field_count = 1},
    {.name = "Link", .fields = ref_field, .field_count = 1},
    {.name = "Alias", .fields = decimal_field, .field_count = 1}};
static const struct mo_type types[] = {
    {.kind = MO_TYPE_UNIT},
    {.kind = MO_TYPE_INT},
    {.kind = MO_TYPE_TEXT},
    {.kind = MO_TYPE_CHOICE,
     .name = "Chain",
     .cases = chain_cases,
     .case_count = 3,
     .defined = true},
    {.kind = MO_TYPE_LIST, .parameters = chain, .parameter_count = 1},
    {.kind = MO_TYPE_TUPLE, .parameters = pair, .parameter_count = 2},
    {.kind = MO_TYPE_FUNCTION, .parameters = chain, .parameter_count = 1, .result = 1},
    {.kind = MO_TYPE_ENV, .function = 0},
    {.kind = MO_TYPE_BOOL},
    {.kind = MO_TYPE_BYTE},
    {.kind = MO_TYPE_FLOAT},
    {.kind = MO_TYPE_CHAR},
    {.kind = MO_TYPE_TIME},
    {.kind = MO_TYPE_DURATION},
    {.kind = MO_TYPE_DECIMAL},
    {.kind = MO_TYPE_RANGE},
    {.kind = MO_TYPE_BYTES},
    {.kind = MO_TYPE_BIGINT},
    {.kind = MO_TYPE_SET, .parameters = element, .parameter_count = 1},
    {.kind = MO_TYPE_MAP, .parameters = map_args, .parameter_count = 2},
    {.kind = MO_TYPE_RECORD,
     .name = "Counts",
     .fields = count_fields,
     .field_count = 3,
     .defined = true},
    {.kind = MO_TYPE_CHOICE,
     .name = "Counts",
     .cases = ordering_cases,
     .case_count = 2,
     .defined = true},
    {.kind = MO_TYPE_CHOICE,
     .name = "Chain",
     .cases = shape_cases,
     .case_count = 4,
     .defined = true},
    {.kind = MO_TYPE_FUNCTION, .result = 0},
    {.kind = MO_TYPE_ENV, .function = 7},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_ARGS},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_ENV},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_CONSOLE},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_FILES},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_READ_FILES},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_CLOCK},
    {.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_RANDOM},
    {.kind = MO_TYPE_RECORD, .name = "Chain", .defined = true}};
static const uint32_t link_layout[] = {1, 3}, captures[] = {3, 2}, count_layout[] = {1, 1, 1};
static const struct mo_layout layouts[] = {
    {.kind = MO_LAYOUT_CASE, .type = 3, .case_index = 1, .fields = link_layout, .field_count = 2},
    {.kind = MO_LAYOUT_CASE, .type = 3, .case_index = 2, .fields = chain, .field_count = 1},
    {.kind = MO_LAYOUT_TUPLE, .type = 5, .fields = pair, .field_count = 2},
    {.kind = MO_LAYOUT_CLOSURE, .type = 6, .function = 0, .fields = captures, .field_count = 2},
    {.kind = MO_LAYOUT_RECORD, .type = 20, .fields = count_layout, .field_count = 3}};
static const uint8_t bytes_literal[] = {0xff, 0, 0x80}, magnitude[] = {0, 0x91};
static const struct mo_bytecode_constant constants[] = {
    {.type = 0},
    {.type = 8, .value.immediate = {1}},
    {.type = 9, .value.immediate = {255}},
    {.type = 1, .value.immediate = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
    {.type = 10, .value.immediate = {0x34, 0x12, 0, 0, 0, 0, 0xf8, 0x7f}}, /* distinct NaN bits */
    {.type = 11, .value.immediate = {0xff, 0xff, 0x10}},
    {.type = 12},
    {.type = 13, .value.immediate = {1, 2, 3, 4, 5, 6, 7, 8}},
    {.type = 14, .value.immediate = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}},
    {.type = 2, .value.string = 21},
    {.type = 16, .value.bytes = {3, bytes_literal}},
    {.type = 17, .value.bigint = {.sign = MO_BIGINT_POSITIVE, .magnitude = {2, magnitude}}},
    {.type = 23, .value.function = 7},
    {.type = 17, .value.bigint = {.sign = MO_BIGINT_ZERO}},
    {.type = 17, .value.bigint = {.sign = MO_BIGINT_NEGATIVE, .magnitude = {2, magnitude}}}};
static const struct mo_parameter closure_params[] = {{3, MO_PARAMETER_OWNED}},
                                 add_params[] = {{20, MO_PARAMETER_BORROWED},
                                                 {20, MO_PARAMETER_BORROWED}},
                                 promise_params[] = {{1, MO_PARAMETER_OWNED}};
static const uint32_t closure_regs[] = {3, 7, 1},
                      add_regs[] = {20, 20, 1, 1, 1, 1, 1, 1, 1, 1, 1, 20}, unit_regs[] = {0},
                      int_regs[] = {1}, empty_closure_regs[] = {24, 0};
static const uint32_t closure_code[] = {MO_OP_CONST, 2, 3, MO_OP_RET, 2},
                      unit_code[] = {MO_OP_CONST, 0, 0, MO_OP_RET, 0}, int_code[] = {MO_OP_RET, 0},
                      empty_closure_code[] = {MO_OP_CONST, 1, 0, MO_OP_RET, 1};
/* Section 10's Wc.add example, transcribed as words without using its assembler. */
static const uint32_t add_code[] = {MO_OP_FIELD,
                                    2,
                                    0,
                                    0,
                                    MO_OP_FIELD,
                                    3,
                                    1,
                                    0,
                                    MO_OP_ADD_INT,
                                    4,
                                    2,
                                    3,
                                    MO_OP_FIELD,
                                    5,
                                    0,
                                    1,
                                    MO_OP_FIELD,
                                    6,
                                    1,
                                    1,
                                    MO_OP_ADD_INT,
                                    7,
                                    5,
                                    6,
                                    MO_OP_FIELD,
                                    8,
                                    0,
                                    2,
                                    MO_OP_FIELD,
                                    9,
                                    1,
                                    2,
                                    MO_OP_ADD_INT,
                                    10,
                                    8,
                                    9,
                                    MO_OP_CONSTRUCT | (3U << 16),
                                    11,
                                    4,
                                    4,
                                    7,
                                    10,
                                    MO_OP_RET,
                                    11};
#define ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define CODE(regs, words)                                                                          \
    .register_types = regs, .register_count = ARRAY_COUNT(regs), .code = words,                    \
    .code_count = ARRAY_COUNT(words)
static const struct mo_bytecode_function functions[] = {
    {.name = 13,
     .kind = MO_FUNCTION_CLOSURE,
     .parameter_count = 1,
     .parameters = closure_params,
     .result_type = 1,
     .capture_types = captures,
     .capture_count = 2,
     CODE(closure_regs, closure_code)},
    {.name = 12,
     .kind = MO_FUNCTION_FUNCTION,
     .parameter_count = 2,
     .parameters = add_params,
     .result_type = 20,
     CODE(add_regs, add_code)},
    {.name = 14, .kind = MO_FUNCTION_INIT, CODE(unit_regs, unit_code)},
    {.name = 15, .kind = MO_FUNCTION_TEST, .spec_code = true, CODE(unit_regs, unit_code)},
    {.name = 16, .kind = MO_FUNCTION_EXAMPLE, .spec_code = true, CODE(unit_regs, unit_code)},
    {.name = 17,
     .kind = MO_FUNCTION_PROMISE,
     .spec_code = true,
     .parameter_count = 1,
     .parameters = promise_params,
     .result_type = 1,
     CODE(int_regs, int_code)},
    {.name = 18,
     .kind = MO_FUNCTION_ANCHOR,
     .spec_code = true,
     .result_type = 1,
     CODE(int_regs, int_code)},
    {.name = 13, .kind = MO_FUNCTION_CLOSURE, CODE(empty_closure_regs, empty_closure_code)}};
static const struct mo_bytecode_global globals[] = {{1, 3, 0}};
static const uint32_t init[] = {2}, exported_functions[] = {1}, module_globals[] = {0};
static const struct mo_bytecode_type_export exports[] = {{20, true}, {3, false}};
static const struct mo_bytecode_module modules[] = {{.name = 11,
                                                     .types = exports,
                                                     .type_count = 2,
                                                     .functions = exported_functions,
                                                     .function_count = 1,
                                                     .globals = module_globals,
                                                     .global_count = 1}};
static const struct mo_bytecode_span spans[] = {{0, 19, 13, 24}, {4, 19, 25, 30}};
/* Unsorted, adjacent reuse of the same register; sorting must not change the wire order. */
static const struct mo_bytecode_register_name names[] = {{0, 20, 4, 8}, {0, 3, 0, 4}};
static const struct mo_bytecode_debug debug[] = {{.text = MO_BYTECODE_NO_INDEX},
                                                 {.spans = spans,
                                                  .span_count = 2,
                                                  .names = names,
                                                  .name_count = 2,
                                                  .text = MO_BYTECODE_NO_INDEX},
                                                 {.text = MO_BYTECODE_NO_INDEX},
                                                 {.text = 22},
                                                 {.text = 21},
                                                 {.text = 21},
                                                 {.text = 22},
                                                 {.text = MO_BYTECODE_NO_INDEX}};
/* Table names are plural but their count fields are singular. */
static const struct mo_bytecode fixture = {.strings = strings,
                                           .string_count = ARRAY_COUNT(strings),
                                           .types = types,
                                           .type_count = ARRAY_COUNT(types),
                                           .layouts = layouts,
                                           .layout_count = ARRAY_COUNT(layouts),
                                           .constants = constants,
                                           .constant_count = ARRAY_COUNT(constants),
                                           .globals = globals,
                                           .global_count = ARRAY_COUNT(globals),
                                           .init = init,
                                           .init_count = ARRAY_COUNT(init),
                                           .functions = functions,
                                           .function_count = ARRAY_COUNT(functions),
                                           .modules = modules,
                                           .module_count = ARRAY_COUNT(modules),
                                           .debug = debug,
                                           .debug_count = ARRAY_COUNT(debug)};

static MO_MUST_USE bool equal_bytes(const void *a, const void *b, size_t n) {
    return n == 0 || memcmp(a, b, n) == 0;
}

static void equal_structure(const struct mo_bytecode *a, const struct mo_bytecode *b) {
    CHECK(a->string_count == b->string_count && a->type_count == b->type_count &&
          a->layout_count == b->layout_count && a->constant_count == b->constant_count &&
          a->global_count == b->global_count && a->init_count == b->init_count &&
          a->function_count == b->function_count && a->module_count == b->module_count &&
          a->debug_count == b->debug_count);
    for (uint32_t i = 0; i < a->string_count; i++) {
        CHECK(a->strings[i].length == b->strings[i].length);
        CHECK(equal_bytes(a->strings[i].bytes, b->strings[i].bytes, a->strings[i].length));
    }
    for (uint32_t i = 0; i < a->type_count; i++) {
        const struct mo_type *x = &a->types[i], *y = &b->types[i];
        CHECK(x->kind == y->kind && x->parameter_count == y->parameter_count);
        CHECK(equal_bytes(x->parameters, y->parameters, (size_t)x->parameter_count * 4));
        CHECK(x->result == y->result && x->function == y->function &&
              x->capability == y->capability);
        CHECK(x->field_count == y->field_count && x->case_count == y->case_count);
        if (x->name != NULL) {
            CHECK(strcmp(x->name, y->name) == 0);
        }
        for (uint32_t j = 0; j < x->field_count; j++) {
            CHECK(x->fields[j].type == y->fields[j].type);
            CHECK(strcmp(x->fields[j].name, y->fields[j].name) == 0);
        }
        for (uint32_t j = 0; j < x->case_count; j++) {
            CHECK(strcmp(x->cases[j].name, y->cases[j].name) == 0);
            CHECK(x->cases[j].field_count == y->cases[j].field_count);
            for (uint32_t k = 0; k < x->cases[j].field_count; k++) {
                const struct mo_type_field *xf = &x->cases[j].fields[k],
                                           *yf = &y->cases[j].fields[k];
                CHECK(xf->type == yf->type);
                CHECK(xf->name == NULL ? yf->name == NULL : strcmp(xf->name, yf->name) == 0);
            }
        }
    }
    for (uint32_t i = 0; i < a->layout_count; i++) {
        const struct mo_layout *x = &a->layouts[i], *y = &b->layouts[i];
        CHECK(x->kind == y->kind && x->type == y->type && x->field_count == y->field_count);
        CHECK(x->kind != MO_LAYOUT_CASE || x->case_index == y->case_index);
        CHECK(x->kind != MO_LAYOUT_CLOSURE || x->function == y->function);
        CHECK(equal_bytes(x->fields, y->fields, (size_t)x->field_count * 4));
    }
    for (uint32_t i = 0; i < a->constant_count; i++) {
        const struct mo_bytecode_constant *x = &a->constants[i], *y = &b->constants[i];
        CHECK(x->type == y->type);
        enum mo_type_kind kind = a->types[x->type].kind;
        if (kind <= MO_TYPE_DECIMAL) {
            CHECK(equal_bytes(x->value.immediate, y->value.immediate,
                              kind == MO_TYPE_DECIMAL ? 16 : 8));
        } else if (kind == MO_TYPE_TEXT) {
            CHECK(x->value.string == y->value.string);
        } else if (kind == MO_TYPE_FUNCTION) {
            CHECK(x->value.function == y->value.function);
        } else {
            const struct mo_bytecode_bytes *xb =
                kind == MO_TYPE_BYTES ? &x->value.bytes : &x->value.bigint.magnitude;
            const struct mo_bytecode_bytes *yb =
                kind == MO_TYPE_BYTES ? &y->value.bytes : &y->value.bigint.magnitude;
            CHECK(xb->length == yb->length && equal_bytes(xb->bytes, yb->bytes, xb->length));
            CHECK(kind != MO_TYPE_BIGINT || x->value.bigint.sign == y->value.bigint.sign);
        }
    }
    CHECK(equal_bytes(a->globals, b->globals, (size_t)a->global_count * sizeof *a->globals));
    CHECK(equal_bytes(a->init, b->init, (size_t)a->init_count * 4));
    for (uint32_t i = 0; i < a->function_count; i++) {
        const struct mo_bytecode_function *x = &a->functions[i], *y = &b->functions[i];
        CHECK(x->name == y->name && x->module == y->module && x->kind == y->kind &&
              x->spec_code == y->spec_code);
        CHECK(x->parameter_count == y->parameter_count && x->result_type == y->result_type);
        CHECK(equal_bytes(x->parameters, y->parameters,
                          (size_t)x->parameter_count * sizeof *x->parameters));
        CHECK(x->capture_count == y->capture_count && x->register_count == y->register_count &&
              x->code_count == y->code_count);
        CHECK(equal_bytes(x->capture_types, y->capture_types, (size_t)x->capture_count * 4));
        CHECK(equal_bytes(x->register_types, y->register_types, (size_t)x->register_count * 4));
        CHECK(equal_bytes(x->code, y->code, (size_t)x->code_count * 4));
    }
    for (uint32_t i = 0; i < a->module_count; i++) {
        const struct mo_bytecode_module *x = &a->modules[i], *y = &b->modules[i];
        CHECK(x->name == y->name && x->type_count == y->type_count &&
              x->function_count == y->function_count && x->global_count == y->global_count);
        for (uint32_t j = 0; j < x->type_count; j++) {
            CHECK(x->types[j].type == y->types[j].type &&
                  x->types[j].contents_exposed == y->types[j].contents_exposed);
        }
        CHECK(equal_bytes(x->functions, y->functions, (size_t)x->function_count * 4));
        CHECK(equal_bytes(x->globals, y->globals, (size_t)x->global_count * 4));
    }
    for (uint32_t i = 0; i < a->debug_count; i++) {
        CHECK(a->debug[i].span_count == b->debug[i].span_count &&
              a->debug[i].name_count == b->debug[i].name_count &&
              a->debug[i].text == b->debug[i].text);
        CHECK(equal_bytes(a->debug[i].spans, b->debug[i].spans,
                          (size_t)a->debug[i].span_count * sizeof *a->debug[i].spans));
        CHECK(equal_bytes(a->debug[i].names, b->debug[i].names,
                          (size_t)a->debug[i].name_count * sizeof *a->debug[i].names));
    }
}

static void round_trip(const struct mo_bytecode *file) {
    struct mo_buffer out, again;
    struct mo_arena arena;
    struct mo_write_error write_error;
    struct mo_load_error load_error;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&again, MO_LIMIT_BYTECODE_SIZE);
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    bool written = mo_bytecode_write(file, &out, &write_error);
    CHECK(written);
    if (written) {
        const struct mo_bytecode *decoded =
            mo_bytecode_decode(&arena, out.bytes, out.length, &load_error);
        CHECK(decoded != NULL);
        if (decoded != NULL) {
            equal_structure(file, decoded);
            /* Destroy the input before writing, to check that the decoder owns every pointer. */
            uint8_t *saved = mo_arena_copy(&arena, out.bytes, out.length, 1);
            CHECK(saved != NULL);
            if (saved != NULL) {
                memset(out.bytes, 0xa5, out.length);
                CHECK(mo_bytecode_write(decoded, &again, &write_error));
                CHECK(out.length == again.length && equal_bytes(saved, again.bytes, out.length));
            }
            CHECK(decoded->types[3].reference && decoded->types[3].has_fields);
            CHECK(!decoded->types[21].reference && !decoded->types[21].has_fields);
            CHECK(decoded->types[22].reference && decoded->types[22].has_fields);
            CHECK(decoded->types[3].cases[2].unnamed);
            CHECK(decoded->constants[4].encoding == MO_CONSTANT_IMMEDIATE64);
            CHECK(decoded->constants[8].encoding == MO_CONSTANT_DECIMAL128);
            CHECK(decoded->constants[12].encoding == MO_CONSTANT_CLOSURE);
        }
    } else {
        (void)fprintf(stderr, "round trip refused: %s\n", write_error.reason);
    }
    mo_arena_free(&arena);
    mo_buffer_free(&again);
    mo_buffer_free(&out);
}

static void test_empty(void) {
    const struct mo_bytecode file = {0};
    struct mo_buffer out;
    struct mo_write_error error;
    mo_buffer_init(&out, 1024);
    CHECK(mo_bytecode_write(&file, &out, &error));
    CHECK(out.length == 56);
    CHECK(memcmp(out.bytes, "MOBC\0\0\1\0\0\0\0\0", 12) == 0);
    for (size_t i = 20; i < out.length; i++) {
        CHECK(out.bytes[i] == 0);
    }
    mo_buffer_free(&out);
    mo_buffer_init(&out, 55);
    CHECK(!mo_bytecode_write(&file, &out, &error));
    CHECK(out.length == 0 && error.reason != NULL);
    mo_buffer_free(&out);
}

static void checksum(struct mo_buffer *buffer) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 20; i < buffer->length; i++) {
        hash ^= buffer->bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    for (unsigned i = 0; i < 8; i++) {
        buffer->bytes[12 + i] = (uint8_t)(hash >> (8 * i));
    }
}

static void replace_word(struct mo_buffer *out, size_t offset, uint32_t value) {
    MO_ASSERT(offset + 4 <= out->length);
    for (unsigned i = 0; i < 4; i++) {
        out->bytes[offset + i] = (uint8_t)(value >> (8 * i));
    }
}

static void refuse_bytes(const uint8_t *bytes, size_t n) {
    struct mo_arena arena;
    struct mo_load_error error;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_decode(&arena, bytes, n, &error) == NULL);
    CHECK(error.reason != NULL && error.offset <= n);
    mo_arena_free(&arena);
}

static void expect_write(const struct mo_bytecode *file, bool success) {
    struct mo_buffer out;
    struct mo_write_error error = {0};
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    bool actual = mo_bytecode_write(file, &out, &error);
    CHECK(actual == success);
    if (!actual) {
        CHECK(out.length == 0 && error.reason != NULL);
    }
    mo_buffer_free(&out);
}

/* Deliberately change one contract at a time, leaving unrelated tables well-formed. */
#define TABLE_TYPE_types struct mo_type
#define TABLE_TYPE_layouts struct mo_layout
#define TABLE_TYPE_constants struct mo_bytecode_constant
#define TABLE_TYPE_functions struct mo_bytecode_function
#define TABLE_TYPE_globals struct mo_bytecode_global
#define TABLE_TYPE_modules struct mo_bytecode_module
#define TABLE_TYPE_debug struct mo_bytecode_debug
#define BAD_TABLE(table, row, member, value)                                                       \
    do {                                                                                           \
        struct mo_bytecode changed_file = fixture;                                                 \
        TABLE_TYPE_##table copy[ARRAY_COUNT(table)];                                               \
        memcpy(copy, table, sizeof copy);                                                          \
        copy[row].member = (value);                                                                \
        changed_file.table = copy;                                                                 \
        expect_write(&changed_file, false);                                                        \
    } while (0)

static void test_refusals(void) {
    BAD_TABLE(types, 0, kind, (enum mo_type_kind)99);
    BAD_TABLE(types, 3, defined, false);
    BAD_TABLE(types, 3, name, "missing");
    BAD_TABLE(types, 3, name, "");
    BAD_TABLE(types, 3, case_count, 0);
    BAD_TABLE(types, 3, case_count, MO_LIMIT_CHOICE_CASES + 1U);
    BAD_TABLE(types, 4, parameter_count, 0);
    BAD_TABLE(types, 5, parameter_count, 1);
    BAD_TABLE(types, 5, parameter_count, MO_LIMIT_FIELDS + 1U);
    BAD_TABLE(types, 6, parameter_count, MO_LIMIT_BYTECODE_PARAMETERS + 1U);
    BAD_TABLE(types, 6, result, UINT32_MAX);
    BAD_TABLE(types, 7, function, UINT32_MAX);
    BAD_TABLE(types, 7, function, 1);
    BAD_TABLE(types, 18, parameter_count, 0);
    BAD_TABLE(types, 19, parameter_count, 1);
    BAD_TABLE(types, 20, field_count, MO_LIMIT_FIELDS + 1U);
    BAD_TABLE(types, 25, capability, (enum mo_capability)7);
    BAD_TABLE(types, 31, kind, MO_TYPE_INT); /* duplicate interned built-in */
    const uint32_t bad_index[] = {UINT32_MAX}, cycle[] = {4};
    BAD_TABLE(types, 4, parameters, bad_index);
    BAD_TABLE(types, 4, parameters, cycle);
    struct mo_type nominal_copy[ARRAY_COUNT(types)];
    memcpy(nominal_copy, types, sizeof nominal_copy);
    struct mo_type_field bad_fields[] = {{"value", 1}, {"value", 3}};
    struct mo_type_case bad_cases[] = {{.name = "End"}, {.name = "End"}};
    struct mo_bytecode file = fixture;
    file.types = nominal_copy;
    nominal_copy[20].fields = bad_fields;
    nominal_copy[20].field_count = 2;
    expect_write(&file, false); /* duplicate field names */
    bad_fields[1].name = NULL;
    expect_write(&file, false); /* unnamed record field */
    bad_fields[1].name = "next";
    bad_fields[1].type = UINT32_MAX;
    expect_write(&file, false);
    nominal_copy[20] = types[20];
    nominal_copy[3].cases = bad_cases;
    nominal_copy[3].case_count = 2;
    expect_write(&file, false); /* duplicate case names */
    bad_cases[1].name = "Link";
    bad_cases[1].fields = bad_fields;
    bad_cases[1].field_count = 2;
    bad_fields[1].name = NULL;
    bad_fields[1].type = 3;
    expect_write(&file, false); /* unnamed field in a multi-field case */

    BAD_TABLE(layouts, 0, kind, (enum mo_layout_kind)4);
    BAD_TABLE(layouts, 0, type, UINT32_MAX);
    BAD_TABLE(layouts, 0, type, 1);
    BAD_TABLE(layouts, 0, case_index, 0); /* not boxed */
    BAD_TABLE(layouts, 0, case_index, UINT32_MAX);
    BAD_TABLE(layouts, 0, field_count, 1);
    BAD_TABLE(layouts, 1, fields, bad_index);
    BAD_TABLE(layouts, 2, type, 20);
    BAD_TABLE(layouts, 3, function, UINT32_MAX);
    BAD_TABLE(layouts, 3, function, 7); /* signature mismatch */
    BAD_TABLE(layouts, 3, field_count, 1);
    BAD_TABLE(layouts, 3, fields, link_layout); /* captures mismatch */
    BAD_TABLE(layouts, 4, field_count, MO_LIMIT_FIELDS + 1U);

    BAD_TABLE(constants, 0, type, UINT32_MAX);
    BAD_TABLE(constants, 0, type, 15); /* Range has no literal encoding */
    BAD_TABLE(constants, 0, type, 21); /* nor Choice */
    BAD_TABLE(constants, 0, value.immediate[0], 1);
    BAD_TABLE(constants, 1, value.immediate[0], 2);
    BAD_TABLE(constants, 2, value.immediate[1], 1);
    BAD_TABLE(constants, 5, value.immediate[3], 1);
    BAD_TABLE(constants, 9, value.string, UINT32_MAX);
    BAD_TABLE(constants, 10, value.bytes.length, MO_LIMIT_STRING_SIZE + 1U);
    BAD_TABLE(constants, 11, value.bigint.sign, (enum mo_bigint_sign)3);
    BAD_TABLE(constants, 11, value.bigint.sign, MO_BIGINT_ZERO);
    BAD_TABLE(constants, 13, value.bigint.sign, MO_BIGINT_POSITIVE);
    const uint8_t high_zero[] = {1, 0};
    BAD_TABLE(constants, 11, value.bigint.magnitude.bytes, high_zero);
    BAD_TABLE(constants, 12, value.function, 0); /* captured closure */
    BAD_TABLE(constants, 12, value.function, 1); /* wrong kind */
    BAD_TABLE(constants, 12, value.function, UINT32_MAX);

    BAD_TABLE(functions, 1, name, UINT32_MAX);
    BAD_TABLE(functions, 1, name, 21); /* embedded NUL */
    BAD_TABLE(functions, 1, name, 22); /* empty */
    BAD_TABLE(functions, 1, module, UINT32_MAX);
    BAD_TABLE(functions, 1, result_type, UINT32_MAX);
    BAD_TABLE(functions, 1, kind, (enum mo_function_kind)7);
    BAD_TABLE(functions, 1, spec_code, true);
    BAD_TABLE(functions, 3, spec_code, false);
    BAD_TABLE(functions, 0, register_count, 1);
    BAD_TABLE(functions, 1, parameter_count, MO_LIMIT_BYTECODE_PARAMETERS + 1U);
    BAD_TABLE(functions, 1, capture_count, 2);
    BAD_TABLE(functions, 0, capture_count, MO_LIMIT_FIELDS + 1U);
    BAD_TABLE(functions, 1, register_count, MO_LIMIT_FUNCTION_REGISTERS + 1U);
    BAD_TABLE(functions, 1, code_count, MO_LIMIT_FUNCTION_CODE_WORDS + 1U);
    BAD_TABLE(functions, 3, result_type, 1);
    BAD_TABLE(functions, 5, result_type, 0);
    BAD_TABLE(functions, 5, parameter_count, 0);
    BAD_TABLE(functions, 6, parameter_count, 1);
    const uint32_t invalid_capture[] = {UINT32_MAX, 2};
    BAD_TABLE(functions, 0, capture_types, invalid_capture);
    const uint32_t wrong_env[] = {3, 24, 1}, invalid_reg[] = {3, 7, UINT32_MAX};
    BAD_TABLE(functions, 0, register_types, wrong_env);
    BAD_TABLE(functions, 0, register_types, invalid_reg);
    const struct mo_parameter wrong_param[] = {{1, MO_PARAMETER_OWNED}},
                              borrowed_closure[] = {{3, MO_PARAMETER_BORROWED}},
                              invalid_ownership[] = {{3, (enum mo_parameter_ownership)2}};
    BAD_TABLE(functions, 0, parameters, wrong_param);
    BAD_TABLE(functions, 0, parameters, borrowed_closure);
    BAD_TABLE(functions, 0, parameters, invalid_ownership);

    BAD_TABLE(globals, 0, type, UINT32_MAX);
    BAD_TABLE(globals, 0, name, UINT32_MAX);
    BAD_TABLE(globals, 0, module, 1);
    BAD_TABLE(modules, 0, name, UINT32_MAX);
    BAD_TABLE(modules, 0, type_count, MO_LIMIT_MODULE_NAMES + 1U);
    BAD_TABLE(modules, 0, function_count, MO_LIMIT_MODULE_NAMES + 1U);
    BAD_TABLE(modules, 0, global_count, MO_LIMIT_MODULE_NAMES + 1U);
    BAD_TABLE(modules, 0, functions, bad_index);
    BAD_TABLE(modules, 0, globals, bad_index);
    BAD_TABLE(modules, 0, global_count, 0);
    const uint32_t repeated_globals[] = {0, 0};
    struct mo_bytecode_module module = modules[0];
    module.globals = repeated_globals;
    module.global_count = 2;
    file = fixture;
    file.modules = &module;
    expect_write(&file, false);
    const struct mo_bytecode_type_export invalid_export[] = {{UINT32_MAX, false}, {3, true}};
    BAD_TABLE(modules, 0, types, invalid_export);
    file = fixture;
    file.init = bad_index;
    expect_write(&file, false);
    file.init_count = 0;
    expect_write(&file, false);
    file = fixture;
    file.init = element; /* non-init function */
    expect_write(&file, false);
    file = fixture;
    file.debug_count = 0;
    expect_write(&file, false);
    BAD_TABLE(debug, 1, text, 0);
    BAD_TABLE(debug, 3, text, MO_BYTECODE_NO_INDEX);
    BAD_TABLE(debug, 3, text, ARRAY_COUNT(strings));
    BAD_TABLE(debug, 1, span_count, MO_LIMIT_DEBUG_ENTRIES + 1U);
    BAD_TABLE(debug, 1, name_count, MO_LIMIT_DEBUG_ENTRIES + 1U);
    struct mo_bytecode_span bad_spans[2];
    memcpy(bad_spans, spans, sizeof bad_spans);
    bad_spans[1].code_offset = 1; /* operand, not head */
    BAD_TABLE(debug, 1, spans, bad_spans);
    bad_spans[1].code_offset = 0;
    BAD_TABLE(debug, 1, spans, bad_spans);
    bad_spans[1] = spans[1];
    bad_spans[0].file = UINT32_MAX;
    BAD_TABLE(debug, 1, spans, bad_spans);
    bad_spans[0] = spans[0];
    bad_spans[0].start = 25;
    BAD_TABLE(debug, 1, spans, bad_spans);
    bad_spans[0] = spans[0];
    bad_spans[0].end = MO_LIMIT_FILE_SIZE + 1U;
    BAD_TABLE(debug, 1, spans, bad_spans);
    struct mo_bytecode_register_name bad_names[2];
    memcpy(bad_names, names, sizeof bad_names);
    bad_names[0].reg = UINT32_MAX;
    BAD_TABLE(debug, 1, names, bad_names);
    bad_names[0] = names[0];
    bad_names[0].name = UINT32_MAX;
    BAD_TABLE(debug, 1, names, bad_names);
    bad_names[0] = names[0];
    bad_names[0].code_end = 5;
    BAD_TABLE(debug, 1, names, bad_names);
    bad_names[0].code_end = UINT32_MAX;
    BAD_TABLE(debug, 1, names, bad_names);
    bad_names[0] = names[0];
    bad_names[0].code_start = bad_names[0].code_end;
    BAD_TABLE(debug, 1, names, bad_names);
    bad_names[0] = names[0];
    bad_names[1].code_end = 8;
    BAD_TABLE(debug, 1, names, bad_names); /* overlap */

    const uint32_t broken_code[][5] = {
        {0, 2, 3, MO_OP_RET, 2},
        {MO_OP_CONST | (1U << 16), 2, 3, MO_OP_RET, 2},
        {MO_OP_CONST, UINT32_MAX, 3, MO_OP_RET, 2},
        {MO_OP_CONST, 2, UINT32_MAX, MO_OP_RET, 2},
        {MO_OP_JUMP, 1, MO_OP_RET, 2, MO_OP_RET}, /* jump to operand, then truncated ret */
        {MO_OP_JUMP, UINT32_MAX, MO_OP_RET, 2, MO_OP_RET}};
    for (size_t i = 0; i < sizeof broken_code / sizeof broken_code[0]; i++) {
        BAD_TABLE(functions, 0, code, broken_code[i]);
    }
    BAD_TABLE(functions, 0, code_count, 4); /* truncated ret */
    const uint32_t reg_limit[] = {MO_OP_RET, MO_LIMIT_FUNCTION_REGISTERS};
    struct mo_bytecode_function fn_copy[ARRAY_COUNT(functions)];
    memcpy(fn_copy, functions, sizeof fn_copy);
    fn_copy[0].code = reg_limit;
    fn_copy[0].code_count = 2;
    file = fixture;
    file.functions = fn_copy;
    expect_write(&file, false);
}

static void test_wire_refusals(void) {
    struct mo_buffer out;
    struct mo_write_error error;
    const struct mo_bytecode empty = {0};
    mo_buffer_init(&out, 1024);
    CHECK(mo_bytecode_write(&empty, &out, &error));
    for (size_t i = 0; i < 20; i++) {
        out.bytes[i] ^= 0x80;
        refuse_bytes(out.bytes, out.length);
        out.bytes[i] ^= 0x80;
    }
    for (size_t n = 0; n < out.length; n++) {
        refuse_bytes(out.bytes, n);
    }
    const uint32_t limits[] = {MO_LIMIT_STRINGS,   MO_LIMIT_TYPES,   MO_LIMIT_LAYOUTS,
                               MO_LIMIT_CONSTANTS, MO_LIMIT_GLOBALS, MO_LIMIT_INITS,
                               MO_LIMIT_FUNCTIONS, MO_LIMIT_MODULES, MO_LIMIT_FUNCTIONS};
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        replace_word(&out, 20 + i * 4, limits[i] + 1U);
        checksum(&out);
        refuse_bytes(out.bytes, out.length);
        replace_word(&out, 20 + i * 4, limits[i]); /* within limit, not within remaining bytes */
        checksum(&out);
        refuse_bytes(out.bytes, out.length);
        replace_word(&out, 20 + i * 4, 0);
    }
    checksum(&out);
    CHECK(mo_buffer_append_byte(&out, 0));
    checksum(&out);
    refuse_bytes(out.bytes, out.length);
    out.length--;
    checksum(&out);
    struct mo_arena no_memory;
    struct mo_load_error load_error;
    mo_arena_init(&no_memory, 0);
    CHECK(mo_bytecode_decode(&no_memory, out.bytes, out.length, &load_error) == NULL);
    CHECK(load_error.offset == 0 && strcmp(load_error.reason, "no memory") == 0);
    mo_arena_free(&no_memory);
    refuse_bytes(out.bytes, (size_t)MO_LIMIT_BYTECODE_SIZE + 1); /* refused before reading */
    mo_buffer_free(&out);

    /* A record naming the second of two identical strings is structurally noncanonical. */
    static const uint8_t duplicate_name[] = {
        'M', 'O', 'B', 'C', 0, 0, 1, 0,   0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 0,
        0,   0,   'x', 1,   0, 0, 0, 'x', 1, 0, 0, 0, 17, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,   0,   0,   0,   0, 0, 0, 0,   0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    mo_buffer_init(&out, 1024);
    CHECK(mo_buffer_append(&out, duplicate_name, sizeof duplicate_name));
    checksum(&out);
    refuse_bytes(out.bytes, out.length);
    replace_word(&out, 42, 0);
    checksum(&out);
    struct mo_arena arena;
    mo_arena_init(&arena, 4096);
    CHECK(mo_bytecode_decode(&arena, out.bytes, out.length, &load_error) != NULL);
    mo_arena_free(&arena);
    mo_buffer_free(&out);
}

static void put_hex(struct mo_buffer *out, const char *hex) {
    for (size_t i = 0; hex[i] != '\0'; i += 2) {
        const char *digits = "0123456789abcdef";
        const char *hi = strchr(digits, hex[i]), *lo = strchr(digits, hex[i + 1]);
        MO_ASSERT(hi != NULL && lo != NULL);
        CHECK(mo_buffer_append_byte(out, (uint8_t)((hi - digits) * 16 + (lo - digits))));
    }
}

static void test_worked_encoding(void) {
    /* Exact bytes from section 3, including its six strings. Neither enums nor the writer
     * supply the expected bytes. Complete the omitted sections with one structural closure. */
    const char hex[] = "06000000"
                       "05000000436861696e03000000456e64040000004c696e6b"
                       "0500000076616c7565040000006e65787405000000416c696173"
                       "0800000000000000030000000a000000"
                       "1200000000000000030000000100000000000000"
                       "020000000200000003000000010000000400000003000000"
                       "0500000001000000ffffffff03000000"
                       "0e0000000100000003000000"
                       "0d000000020000000100000004000000"
                       "13000000010000000300000001000000"
                       "1400000000000000"
                       "04000000"
                       "020000000300000001000000020000000100000003000000"
                       "0200000003000000020000000100000003000000"
                       "0100000005000000020000000100000004000000"
                       "030000000600000000000000020000000300000002000000";
    const uint32_t suffix[] = {0, 0,  0,    /* constants, globals, init */
                               1,           /* functions */
                               0, 0,  1, 0, /* name, module, closure, not spec */
                               1, 3,  0,    /* one owned Chain parameter */
                               1, 2,  3, 2, /* result Int, captures Chain, Text */
                               2, 3,  7,    /* registers Chain, env */
                               2, 71, 0, /* ret r0: deliberately not type-verified in this task */
                               1, 0,  0, 0,         0, /* module Chain with no exports or globals */
                               1, 0,  0, UINT32_MAX};  /* debug: empty entry */
    const uint8_t header[20] = {'M', 'O', 'B', 'C', 0, 0, 1, 0};
    struct mo_buffer expected, actual;
    mo_buffer_init(&expected, 4096);
    mo_buffer_init(&actual, 4096);
    CHECK(mo_buffer_append(&expected, header, sizeof header));
    put_hex(&expected, hex);
    for (size_t i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
        CHECK(mo_buffer_append_le32(&expected, suffix[i]));
    }
    checksum(&expected);
    struct mo_bytecode f = {.strings = strings,
                            .string_count = 6,
                            .types = types,
                            .type_count = 8,
                            .layouts = layouts,
                            .layout_count = 4};
    struct mo_bytecode_function fn = functions[0];
    const uint32_t code[] = {71, 0};
    fn.name = 0;
    fn.code = code;
    fn.code_count = 2;
    fn.register_count = 2;
    f.functions = &fn;
    f.function_count = 1;
    const struct mo_bytecode_module module = {.name = 0};
    f.modules = &module;
    f.module_count = 1;
    f.debug = debug;
    f.debug_count = 1;
    struct mo_write_error write_error;
    CHECK(mo_bytecode_write(&f, &actual, &write_error));
    CHECK(expected.length == actual.length &&
          equal_bytes(expected.bytes, actual.bytes, expected.length));
    struct mo_arena arena;
    struct mo_load_error error;
    mo_arena_init(&arena, 65536);
    const struct mo_bytecode *decoded =
        mo_bytecode_decode(&arena, expected.bytes, expected.length, &error);
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        equal_structure(&f, decoded);
        mo_buffer_clear(&actual);
        CHECK(mo_bytecode_write(decoded, &actual, &write_error));
        CHECK(expected.length == actual.length &&
              equal_bytes(expected.bytes, actual.bytes, expected.length));
    }
    const size_t function_start = 20 + strlen(hex) / 2 + 16;
    const size_t mutations[] = {24,
                                98,
                                114,
                                138,
                                154,
                                166,
                                182,
                                218,
                                function_start + 12,
                                function_start + 16,
                                function_start + 24,
                                function_start + 32,
                                function_start + 44,
                                function_start + 56};
    const uint32_t values[] = {MO_LIMIT_STRING_SIZE + 1U,
                               MO_LIMIT_CHOICE_CASES + 1U,
                               MO_LIMIT_FIELDS + 1U,
                               MO_LIMIT_FIELDS + 1U,
                               0,
                               1,
                               MO_LIMIT_BYTECODE_PARAMETERS + 1U,
                               MO_LIMIT_FIELDS + 1U,
                               2,
                               MO_LIMIT_BYTECODE_PARAMETERS + 1U,
                               2,
                               MO_LIMIT_FIELDS + 1U,
                               MO_LIMIT_FUNCTION_REGISTERS + 1U,
                               MO_LIMIT_FUNCTION_CODE_WORDS + 1U};
    for (size_t i = 0; i < sizeof mutations / sizeof mutations[0]; i++) {
        memcpy(expected.bytes, actual.bytes, expected.length);
        replace_word(&expected, mutations[i], values[i]);
        checksum(&expected);
        refuse_bytes(expected.bytes, expected.length);
    }
    memcpy(expected.bytes, actual.bytes, expected.length);
    /* Check every truncation again with a valid checksum: each stops inside a real section. */
    size_t length = expected.length;
    for (size_t n = 20; n < length; n++) {
        expected.length = n;
        checksum(&expected);
        refuse_bytes(expected.bytes, n);
    }
    expected.length = length;
    mo_arena_free(&arena);
    mo_buffer_free(&expected);
    mo_buffer_free(&actual);
}

static void literal_bits(struct mo_bytecode_constant *constant, uint64_t value) {
    memset(constant->value.immediate, 0, 16);
    for (unsigned i = 0; i < 8; i++) {
        constant->value.immediate[i] = (uint8_t)(value >> (8 * i));
    }
}

static void test_literal_boundaries(void) {
    struct mo_bytecode f = fixture;
    struct mo_bytecode_constant copy[ARRAY_COUNT(constants)];
    memcpy(copy, constants, sizeof copy);
    f.constants = copy;
    const uint64_t times[] = {UINT64_MAX - UINT64_C(62135596800000000),
                              UINT64_MAX - UINT64_C(62135596800000000) + 1,
                              UINT64_C(253402300799999999), UINT64_C(253402300800000000)};
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        literal_bits(&copy[6], times[i]);
        expect_write(&f, i == 1 || i == 2);
    }
    copy[6] = constants[6];
    const uint64_t chars[] = {0xd7ff, 0xd800, 0xdfff, 0xe000, 0x10ffff, 0x110000};
    for (size_t i = 0; i < sizeof chars / sizeof chars[0]; i++) {
        literal_bits(&copy[5], chars[i]);
        expect_write(&f, i == 0 || i == 3 || i == 4);
    }
    copy[5] = constants[5];
    memset(copy[8].value.immediate, 0, 16);
    copy[8].value.immediate[15] = 0x80;
    expect_write(&f, false); /* -2^127 */
    copy[8].value.immediate[0] = 1;
    expect_write(&f, true); /* -(2^127 - 1) */
    memset(copy[8].value.immediate, 0xff, 16);
    copy[8].value.immediate[15] = 0x7f;
    expect_write(&f, true); /* 2^127 - 1 */
    const uint64_t floats[] = {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                               UINT64_C(0x7ff0000000000001), UINT64_C(0x8000000000000000)};
    for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++) {
        literal_bits(&copy[4], floats[i]);
        expect_write(&f, true);
    }
}

static void test_utf8(void) {
    const uint8_t sequences[][4] = {{0},
                                    {0x7f},
                                    {0xc2, 0x80},
                                    {0xe0, 0xa0, 0x80},
                                    {0xed, 0x9f, 0xbf},
                                    {0xf0, 0x90, 0x80, 0x80},
                                    {0xf4, 0x8f, 0xbf, 0xbf},
                                    {0x80},
                                    {0xc0, 0x80},
                                    {0xc1, 0xbf},
                                    {0xc2},
                                    {0xe0, 0x9f, 0xbf},
                                    {0xed, 0xa0, 0x80},
                                    {0xf0, 0x8f, 0xbf, 0xbf},
                                    {0xf4, 0x90, 0x80, 0x80},
                                    {0xf5, 0x80, 0x80, 0x80},
                                    {0xff},
                                    {0xe1, 0x80},
                                    {0xf1, 0x80, 0x80},
                                    {0xe1, 0x41, 0x80}};
    const uint32_t lengths[] = {1, 1, 2, 3, 3, 4, 4, 1, 2, 2, 1, 3, 3, 4, 4, 4, 1, 2, 3, 3};
    struct mo_bytecode f = fixture;
    struct mo_bytecode_string copy[ARRAY_COUNT(strings)];
    memcpy(copy, strings, sizeof copy);
    f.strings = copy;
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        copy[21] = (struct mo_bytecode_string){.length = lengths[i], .bytes = sequences[i]};
        expect_write(&f, i < 7);
    }
    copy[21] = strings[21];
    copy[21].length = MO_LIMIT_STRING_SIZE + 1U;
    expect_write(&f, false);
}

static void test_limits_and_caches(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 8 << 20);
    uint32_t *many =
        mo_arena_allocate_array(&arena, MO_LIMIT_FIELDS, sizeof *many, _Alignof(uint32_t));
    CHECK(many != NULL);
    if (many == NULL) {
        mo_arena_free(&arena);
        return;
    }
    for (uint32_t i = 0; i < MO_LIMIT_FIELDS; i++) {
        many[i] = 1;
    }
    struct mo_bytecode f = fixture;
    struct mo_type ts[ARRAY_COUNT(types)];
    struct mo_layout ls[ARRAY_COUNT(layouts)];
    struct mo_bytecode_function fs[ARRAY_COUNT(functions)];
    memcpy(ts, types, sizeof ts);
    memcpy(ls, layouts, sizeof ls);
    memcpy(fs, functions, sizeof fs);
    f.types = ts;
    f.layouts = ls;
    f.functions = fs;
    ts[5].parameters = many;
    ts[5].parameter_count = MO_LIMIT_FIELDS;
    ls[2].fields = many;
    ls[2].field_count = MO_LIMIT_FIELDS;
    expect_write(&f, true); /* largest tuple and matching layout */
    ts[5] = types[5];
    ls[2] = layouts[2];
    struct mo_parameter parameters[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t registers[MO_LIMIT_BYTECODE_PARAMETERS + 1];
    for (uint32_t i = 0; i < MO_LIMIT_BYTECODE_PARAMETERS; i++) {
        parameters[i] = (struct mo_parameter){1, MO_PARAMETER_OWNED};
        registers[i] = 1;
    }
    fs[5].parameters = parameters;
    fs[5].parameter_count = MO_LIMIT_BYTECODE_PARAMETERS;
    fs[5].register_types = registers;
    fs[5].register_count = MO_LIMIT_BYTECODE_PARAMETERS;
    expect_write(&f, true); /* generated promise's 256th argument */
    fs[5] = functions[5];
    for (uint32_t i = 0; i < MO_LIMIT_BYTECODE_PARAMETERS; i++) {
        parameters[i].type = 3;
        registers[i] = 3;
        many[i] = 3;
    }
    registers[MO_LIMIT_BYTECODE_PARAMETERS - 1] = 7;
    fs[0].parameters = parameters;
    fs[0].parameter_count = MO_LIMIT_BYTECODE_PARAMETERS - 1U;
    fs[0].register_types = registers;
    fs[0].register_count = MO_LIMIT_BYTECODE_PARAMETERS;
    ts[6].parameters = many;
    ts[6].parameter_count = MO_LIMIT_BYTECODE_PARAMETERS - 1U;
    expect_write(&f, true); /* 255 explicit arguments plus env */
    fs[0].parameter_count++;
    registers[MO_LIMIT_BYTECODE_PARAMETERS - 1] = 3;
    registers[MO_LIMIT_BYTECODE_PARAMETERS] = 7;
    fs[0].register_count++;
    ts[6].parameter_count++;
    expect_write(&f, false); /* 256 explicit arguments plus env */
    fs[0] = functions[0];
    ts[6] = types[6];
    for (uint32_t i = 0; i < MO_LIMIT_FUNCTION_REGISTERS; i++) {
        many[i] = i < 2 ? 20U : 1U;
    }
    fs[1].register_types = many;
    fs[1].register_count = MO_LIMIT_FUNCTION_REGISTERS;
    expect_write(&f, true);
    fs[1] = functions[1];
    uint32_t *code = mo_arena_allocate_array(&arena, 2U * (MO_LIMIT_FUNCTION_INSTRUCTIONS + 1U),
                                             sizeof *code, _Alignof(uint32_t));
    CHECK(code != NULL);
    if (code != NULL) {
        for (uint32_t i = 0; i < MO_LIMIT_FUNCTION_INSTRUCTIONS + 1U; i++) {
            code[2 * i] = MO_OP_RET;
            code[2 * i + 1] = 1;
        }
        fs[7].code = code;
        fs[7].code_count = 2U * MO_LIMIT_FUNCTION_INSTRUCTIONS;
        expect_write(&f, true);
        fs[7].code_count += 2;
        expect_write(&f, false);
    }
    fs[7] = functions[7];
    for (uint32_t i = 0; i < MO_LIMIT_CHOICE_CASES; i++) {
        many[i] = 0;
    }
    uint32_t *switch_code = mo_arena_allocate_array(&arena, 2U + MO_LIMIT_CHOICE_CASES,
                                                    sizeof *switch_code, _Alignof(uint32_t));
    CHECK(switch_code != NULL);
    if (switch_code != NULL) {
        switch_code[0] = MO_OP_SWITCH | ((uint32_t)MO_LIMIT_CHOICE_CASES << 16);
        switch_code[1] = 0;
        memcpy(switch_code + 2, many, (size_t)MO_LIMIT_CHOICE_CASES * 4);
        fs[0].code = switch_code;
        fs[0].code_count = 2U + MO_LIMIT_CHOICE_CASES;
        expect_write(&f, true); /* maximal variable tail; switch type compatibility is later */
    }
    fs[0] = functions[0];
    /* Garbage in decoded-only and inactive fields must not enter the wire or change acceptance. */
    struct mo_bytecode_constant ks[ARRAY_COUNT(constants)];
    memcpy(ks, constants, sizeof ks);
    f.constants = ks;
    f.header.major = UINT16_MAX;
    f.header.flags = UINT32_MAX;
    for (uint32_t i = 0; i < f.type_count; i++) {
        ts[i].reference = !ts[i].reference;
        ts[i].has_fields = !ts[i].has_fields;
    }
    for (uint32_t i = 0; i < f.constant_count; i++) {
        ks[i].encoding = (enum mo_constant_encoding)99;
    }
    struct mo_buffer out, expected;
    struct mo_write_error error;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&expected, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_write(&fixture, &expected, &error));
    CHECK(mo_bytecode_write(&f, &out, &error));
    CHECK(out.length == expected.length && equal_bytes(out.bytes, expected.bytes, out.length));
    mo_buffer_free(&out);
    mo_buffer_init(&out, expected.length - 1);
    CHECK(!mo_bytecode_write(&fixture, &out, &error));
    CHECK(out.length == 0);
    mo_buffer_free(&out);
    mo_buffer_init(&out, expected.length);
    CHECK(mo_bytecode_write(&fixture, &out, &error));
    CHECK(out.length == expected.length);
    /* The exposed-type boolean is u32, not a C bool copied from memory. */
    const size_t module_start = out.length - (4 + 8 * 12 + 4 * 16) - 44;
    replace_word(&out, module_start + 16, 2);
    checksum(&out);
    refuse_bytes(out.bytes, out.length);
    mo_buffer_free(&out);
    mo_buffer_free(&expected);
    mo_arena_free(&arena);
}

int main(void) {
    test_empty();
    round_trip(&fixture);
    test_refusals();
    test_wire_refusals();
    test_worked_encoding();
    test_literal_boundaries();
    test_utf8();
    test_limits_and_caches();
    return mo_test_report("test_bytecode_io");
}
