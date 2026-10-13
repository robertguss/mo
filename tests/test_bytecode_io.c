#include "bytecode/io.h"
#include "heap/types.h"
#include "test.h"

#include <string.h>
#include <time.h>

/* These section-3 fixtures deliberately omit section-9 semantics. The private encoder keeps
 * their structural refusal and boundary checks independent of the public verified writer. */

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
static const struct mo_bytecode_bytes files[] = {STRING("wc.mo"), STRING("unused-\xff.mo"),
                                                 STRING("unused-\\xff.mo")};
static const struct mo_bytecode_span spans[] = {{0, 0, 13, 24}, {4, MO_BYTECODE_NO_INDEX, 0, 0}};
/* Unsorted, adjacent reuse of the same register; sorting must not change the wire order. */
static const struct mo_bytecode_register_name names[] = {{0, 20, 4, 8}, {0, 3, 0, 4}};
static const struct mo_bytecode_spec_line lines[] = {{0, 101, 110, 21},
                                                     {MO_BYTECODE_NO_INDEX, 0, 0, 22}};
static const struct mo_bytecode_debug debug[] = {
    {.text = MO_BYTECODE_NO_INDEX, .file = MO_BYTECODE_NO_INDEX, .subject = MO_BYTECODE_NO_INDEX},
    {.spans = spans,
     .span_count = 2,
     .names = names,
     .name_count = 2,
     .text = MO_BYTECODE_NO_INDEX,
     .file = 0,
     .start = 11,
     .end = 31,
     .subject = MO_BYTECODE_NO_INDEX},
    {.text = MO_BYTECODE_NO_INDEX, .file = MO_BYTECODE_NO_INDEX, .subject = MO_BYTECODE_NO_INDEX},
    {.text = 22, .file = MO_BYTECODE_NO_INDEX, .subject = MO_BYTECODE_NO_INDEX},
    {.text = 21, .file = 0, .start = 35, .end = 63, .subject = MO_BYTECODE_NO_INDEX},
    {.text = 21,
     .file = 0,
     .start = 100,
     .end = 140,
     .subject = 1,
     .lines = lines,
     .line_count = 2},
    {.text = 22,
     .file = 0,
     .start = 141,
     .end = 150,
     .subject = 5,
     .expectation = MO_BYTECODE_ALLOWS},
    {.text = MO_BYTECODE_NO_INDEX, .file = MO_BYTECODE_NO_INDEX, .subject = MO_BYTECODE_NO_INDEX}};
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
                                           .files = files,
                                           .file_count = ARRAY_COUNT(files),
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
          a->debug_count == b->debug_count && a->file_count == b->file_count);
    for (uint32_t i = 0; i < a->string_count; i++) {
        CHECK(a->strings[i].length == b->strings[i].length);
        CHECK(equal_bytes(a->strings[i].bytes, b->strings[i].bytes, a->strings[i].length));
    }
    for (uint32_t i = 0; i < a->file_count; i++) {
        CHECK(a->files[i].length == b->files[i].length);
        CHECK(equal_bytes(a->files[i].bytes, b->files[i].bytes, a->files[i].length));
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
        CHECK(a->debug[i].file == b->debug[i].file && a->debug[i].start == b->debug[i].start &&
              a->debug[i].end == b->debug[i].end && a->debug[i].subject == b->debug[i].subject &&
              a->debug[i].expectation == b->debug[i].expectation &&
              a->debug[i].line_count == b->debug[i].line_count);
        CHECK(equal_bytes(a->debug[i].lines, b->debug[i].lines,
                          (size_t)a->debug[i].line_count * sizeof *a->debug[i].lines));
    }
}

static void round_trip(const struct mo_bytecode *file) {
    struct mo_buffer out, again;
    struct mo_arena arena;
    struct mo_write_error write_error;
    struct mo_load_error load_error;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&again, MO_LIMIT_BYTECODE_SIZE);
    mo_arena_init(&arena, (size_t)4 << 30);
    bool written = mo_bytecode_encode(file, &out, &write_error);
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
                CHECK(mo_bytecode_encode(decoded, &again, &write_error));
                CHECK(out.length == again.length && equal_bytes(saved, again.bytes, out.length));
            }
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
    CHECK(mo_bytecode_encode(&file, &out, &error));
    CHECK(out.length == 60);
    CHECK(memcmp(out.bytes, "MOBC\0\0\2\0\0\0\0\0", 12) == 0);
    for (size_t i = 20; i < out.length; i++) {
        CHECK(out.bytes[i] == 0);
    }
    mo_buffer_free(&out);
    mo_buffer_init(&out, 59);
    CHECK(!mo_bytecode_encode(&file, &out, &error));
    CHECK(out.length == 0 && error.reason != NULL);
    CHECK(error.kind == MO_BYTECODE_LIMIT);
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

static void refuse_reason(const uint8_t *bytes, size_t n, const char *reason, uint32_t offset) {
    struct mo_arena arena;
    struct mo_load_error error = {0};
    mo_arena_init(&arena, (size_t)4 << 30);
    CHECK(mo_bytecode_decode(&arena, bytes, n, &error) == NULL);
    if (error.reason == NULL || strcmp(error.reason, reason) != 0 || error.offset != offset) {
        (void)fprintf(stderr, "wanted '%s' at %u; got '%s' at %u\n", reason, offset,
                      error.reason == NULL ? "accepted" : error.reason, error.offset);
    }
    CHECK(error.reason != NULL && strcmp(error.reason, reason) == 0);
    CHECK(error.offset == offset);
    mo_arena_free(&arena);
}

static void reject_word(struct mo_buffer *out, size_t offset, uint32_t value, const char *reason,
                        uint32_t error_offset) {
    uint32_t saved = 0;
    for (unsigned i = 0; i < 4; i++) {
        saved |= (uint32_t)out->bytes[offset + i] << (8 * i);
    }
    replace_word(out, offset, value);
    checksum(out);
    refuse_reason(out->bytes, out->length, reason, error_offset);
    replace_word(out, offset, saved);
    checksum(out);
}

static void expect_write_reason(const struct mo_bytecode *file, const char *reason) {
    struct mo_buffer out;
    struct mo_write_error error = {0};
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(!mo_bytecode_encode(file, &out, &error));
    if (error.reason != NULL && strcmp(error.reason, reason) != 0) {
        CHECK(fprintf(stderr, "writer expected %s, got %s\n", reason, error.reason) >= 0);
    }
    CHECK(out.length == 0 && error.reason != NULL && strcmp(error.reason, reason) == 0);
    mo_buffer_free(&out);
}

static void expect_write(const struct mo_bytecode *file) {
    struct mo_buffer out;
    struct mo_write_error error = {0};
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_encode(file, &out, &error));
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
#define BAD_TABLE(table, row, member, value, reason)                                               \
    do {                                                                                           \
        struct mo_bytecode changed_file = fixture;                                                 \
        TABLE_TYPE_##table copy[ARRAY_COUNT(table)];                                               \
        memcpy(copy, table, sizeof copy);                                                          \
        copy[row].member = (value);                                                                \
        changed_file.table = copy;                                                                 \
        expect_write_reason(&changed_file, reason);                                                \
    } while (0)

static void test_refusals(void) {
    BAD_TABLE(types, 0, kind, (enum mo_type_kind)99, "unknown type kind");
    BAD_TABLE(types, 3, defined, false, "incomplete nominal type");
    BAD_TABLE(types, 3, name, "missing", "name missing from strings or past limit");
    BAD_TABLE(types, 3, name, "", "invalid name");
    BAD_TABLE(types, 3, case_count, 0, "choice has no cases");
    BAD_TABLE(types, 3, case_count, MO_LIMIT_CHOICE_CASES + 1U,
              "invalid array or count past limit");
    BAD_TABLE(types, 4, parameter_count, 0, "invalid type parameter count");
    BAD_TABLE(types, 5, parameter_count, 1, "invalid type parameter count");
    BAD_TABLE(types, 5, parameter_count, MO_LIMIT_FIELDS + 1U, "invalid array or count past limit");
    BAD_TABLE(types, 6, parameter_count, MO_LIMIT_BYTECODE_PARAMETERS + 1U,
              "invalid array or count past limit");
    BAD_TABLE(types, 6, result, UINT32_MAX, "type result or env function out of range/kind");
    BAD_TABLE(types, 7, function, UINT32_MAX, "closure env mismatch");
    BAD_TABLE(types, 7, function, 1, "closure env mismatch");
    BAD_TABLE(types, 18, parameter_count, 0, "invalid type parameter count");
    BAD_TABLE(types, 19, parameter_count, 1, "invalid type parameter count");
    BAD_TABLE(types, 20, field_count, MO_LIMIT_FIELDS + 1U, "invalid array or count past limit");
    BAD_TABLE(types, 25, capability, (enum mo_capability)7, "unknown capability");
    BAD_TABLE(types, 31, kind, MO_TYPE_INT, "duplicate interned type");
    const uint32_t bad_index[] = {UINT32_MAX}, cycle[] = {4};
    BAD_TABLE(types, 4, parameters, bad_index, "type index out of range");
    BAD_TABLE(types, 4, parameters, cycle, "structural type cycle");
    struct mo_type nominal_copy[ARRAY_COUNT(types)];
    memcpy(nominal_copy, types, sizeof nominal_copy);
    struct mo_type_field bad_fields[] = {{"value", 1}, {"value", 3}};
    struct mo_type_case bad_cases[] = {{.name = "End"}, {.name = "End"}};
    struct mo_bytecode file = fixture;
    file.types = nominal_copy;
    nominal_copy[20].fields = bad_fields;
    nominal_copy[20].field_count = 2;
    expect_write_reason(&file, "duplicate field name");
    bad_fields[1].name = NULL;
    expect_write_reason(&file, "invalid name");
    bad_fields[1].name = "next";
    bad_fields[1].type = UINT32_MAX;
    expect_write_reason(&file, "field type out of range");
    nominal_copy[20] = types[20];
    nominal_copy[3].cases = bad_cases;
    nominal_copy[3].case_count = 2;
    expect_write_reason(&file, "duplicate case name");
    bad_cases[1].name = "Link";
    bad_cases[1].fields = bad_fields;
    bad_cases[1].field_count = 2;
    bad_fields[1].name = NULL;
    bad_fields[1].type = 3;
    expect_write_reason(&file, "invalid name");

    BAD_TABLE(layouts, 0, kind, (enum mo_layout_kind)4, "unknown layout kind");
    BAD_TABLE(layouts, 0, type, UINT32_MAX, "layout type out of range");
    BAD_TABLE(layouts, 0, type, 1, "invalid case layout");
    BAD_TABLE(layouts, 0, case_index, 0, "invalid case layout");
    BAD_TABLE(layouts, 0, case_index, UINT32_MAX, "invalid case layout");
    BAD_TABLE(layouts, 0, field_count, 1, "layout field count mismatch");
    BAD_TABLE(layouts, 1, fields, bad_index, "layout field type out of range");
    BAD_TABLE(layouts, 2, type, 20, "tuple layout fields mismatch");
    BAD_TABLE(layouts, 3, function, UINT32_MAX, "closure layout signature mismatch");
    BAD_TABLE(layouts, 3, function, 7, "closure layout signature mismatch");
    BAD_TABLE(layouts, 3, field_count, 1, "closure layout captures mismatch");
    BAD_TABLE(layouts, 3, fields, link_layout, "closure layout captures mismatch");
    BAD_TABLE(layouts, 4, field_count, MO_LIMIT_FIELDS + 1U, "invalid array or count past limit");

    BAD_TABLE(constants, 0, type, UINT32_MAX, "constant type out of range");
    BAD_TABLE(constants, 0, type, 15, "type has no literal encoding");
    BAD_TABLE(constants, 0, type, 21, "type has no literal encoding");
    BAD_TABLE(constants, 0, value.immediate[0], 1, "invalid scalar literal");
    BAD_TABLE(constants, 1, value.immediate[0], 2, "invalid scalar literal");
    BAD_TABLE(constants, 2, value.immediate[1], 1, "invalid scalar literal");
    BAD_TABLE(constants, 5, value.immediate[3], 1, "invalid scalar literal");
    BAD_TABLE(constants, 9, value.string, UINT32_MAX, "text string index out of range");
    BAD_TABLE(constants, 10, value.bytes.length, MO_LIMIT_STRING_SIZE + 1U,
              "invalid array or count past limit");
    BAD_TABLE(constants, 11, value.bigint.sign, (enum mo_bigint_sign)3, "invalid bigint sign");
    BAD_TABLE(constants, 11, value.bigint.sign, MO_BIGINT_ZERO, "noncanonical bigint literal");
    BAD_TABLE(constants, 13, value.bigint.sign, MO_BIGINT_POSITIVE, "noncanonical bigint literal");
    const uint8_t high_zero[] = {1, 0};
    BAD_TABLE(constants, 11, value.bigint.magnitude.bytes, high_zero,
              "noncanonical bigint literal");
    BAD_TABLE(constants, 12, value.function, 0, "invalid closure literal");
    BAD_TABLE(constants, 12, value.function, 1, "invalid closure literal");
    BAD_TABLE(constants, 12, value.function, UINT32_MAX, "invalid closure literal");

    BAD_TABLE(functions, 1, name, UINT32_MAX, "function name, module or result out of range");
    BAD_TABLE(functions, 1, name, 21, "function name, module or result out of range");
    BAD_TABLE(functions, 1, name, 22, "function name, module or result out of range");
    BAD_TABLE(functions, 1, module, UINT32_MAX, "function name, module or result out of range");
    BAD_TABLE(functions, 1, result_type, UINT32_MAX,
              "function name, module or result out of range");
    BAD_TABLE(functions, 1, kind, (enum mo_function_kind)7, "unknown function kind");
    BAD_TABLE(functions, 1, spec_code, true, "function spec flag disagrees with kind");
    BAD_TABLE(functions, 3, spec_code, false, "function spec flag disagrees with kind");
    BAD_TABLE(functions, 0, register_count, 1,
              "invalid function parameter/register/capture counts");
    BAD_TABLE(functions, 1, parameter_count, MO_LIMIT_BYTECODE_PARAMETERS + 1U,
              "invalid array or count past limit");
    BAD_TABLE(functions, 1, capture_count, 2, "invalid array or count past limit");
    BAD_TABLE(functions, 0, capture_count, MO_LIMIT_FIELDS + 1U,
              "invalid array or count past limit");
    BAD_TABLE(functions, 1, register_count, MO_LIMIT_FUNCTION_REGISTERS + 1U,
              "invalid array or count past limit");
    BAD_TABLE(functions, 1, code_count, MO_LIMIT_FUNCTION_CODE_WORDS + 1U,
              "invalid array or count past limit");
    BAD_TABLE(functions, 3, result_type, 1, "invalid function kind signature");
    BAD_TABLE(functions, 5, result_type, 0, "invalid function kind signature");
    BAD_TABLE(functions, 5, parameter_count, 0, "invalid function kind signature");
    BAD_TABLE(functions, 6, parameter_count, 1, "invalid array or count past limit");
    const uint32_t invalid_capture[] = {UINT32_MAX, 2};
    BAD_TABLE(functions, 0, capture_types, invalid_capture, "capture type out of range");
    const uint32_t wrong_env[] = {3, 24, 1}, invalid_reg[] = {3, 7, UINT32_MAX};
    BAD_TABLE(functions, 0, register_types, wrong_env, "closure env mismatch");
    BAD_TABLE(functions, 0, register_types, invalid_reg, "register type out of range");
    const struct mo_parameter wrong_param[] = {{1, MO_PARAMETER_OWNED}},
                              borrowed_closure[] = {{3, MO_PARAMETER_BORROWED}},
                              invalid_ownership[] = {{3, (enum mo_parameter_ownership)2}};
    BAD_TABLE(functions, 0, parameters, wrong_param, "invalid parameter type or borrowing");
    BAD_TABLE(functions, 0, parameters, borrowed_closure, "invalid parameter type or borrowing");
    BAD_TABLE(functions, 0, parameters, invalid_ownership, "invalid parameter ownership");

    BAD_TABLE(globals, 0, type, UINT32_MAX, "global type, name or module out of range");
    BAD_TABLE(globals, 0, name, UINT32_MAX, "global type, name or module out of range");
    BAD_TABLE(globals, 0, module, 1, "global type, name or module out of range");
    BAD_TABLE(modules, 0, name, UINT32_MAX, "invalid module name");
    BAD_TABLE(modules, 0, type_count, MO_LIMIT_MODULE_NAMES + 1U,
              "invalid array or count past limit");
    BAD_TABLE(modules, 0, function_count, MO_LIMIT_MODULE_NAMES + 1U,
              "invalid array or count past limit");
    BAD_TABLE(modules, 0, global_count, MO_LIMIT_MODULE_NAMES + 1U,
              "invalid array or count past limit");
    BAD_TABLE(modules, 0, functions, bad_index, "exposed function out of range/module");
    BAD_TABLE(modules, 0, globals, bad_index, "module global out of range/module or repeated");
    BAD_TABLE(modules, 0, global_count, 0, "global absent from module");
    const uint32_t repeated_globals[] = {0, 0};
    struct mo_bytecode_module module = modules[0];
    module.globals = repeated_globals;
    module.global_count = 2;
    file = fixture;
    file.modules = &module;
    expect_write_reason(&file, "module global out of range/module or repeated");
    const struct mo_bytecode_type_export invalid_export[] = {{UINT32_MAX, false}, {3, true}};
    BAD_TABLE(modules, 0, types, invalid_export, "exposed type out of range");
    file = fixture;
    file.init = bad_index;
    expect_write_reason(&file, "invalid or repeated init function");
    file.init_count = 0;
    expect_write_reason(&file, "init/global or debug/function count mismatch");
    file = fixture;
    file.init = element; /* non-init function */
    expect_write_reason(&file, "invalid or repeated init function");
    file = fixture;
    file.debug_count = 0;
    expect_write_reason(&file, "init/global or debug/function count mismatch");
    BAD_TABLE(debug, 1, text, 0, "invalid debug text index");
    BAD_TABLE(debug, 3, text, ARRAY_COUNT(strings), "invalid debug text index");
    BAD_TABLE(debug, 1, span_count, MO_LIMIT_DEBUG_ENTRIES + 1U,
              "invalid array or count past limit");
    BAD_TABLE(debug, 1, name_count, MO_LIMIT_DEBUG_ENTRIES + 1U,
              "invalid array or count past limit");
    struct mo_bytecode_span bad_spans[2];
    memcpy(bad_spans, spans, sizeof bad_spans);
    bad_spans[1].code_offset = 1; /* operand, not head */
    BAD_TABLE(debug, 1, spans, bad_spans, "invalid debug span");
    bad_spans[1].code_offset = 0;
    BAD_TABLE(debug, 1, spans, bad_spans, "invalid debug span");
    bad_spans[1] = spans[1];
    bad_spans[0].file = UINT32_MAX;
    BAD_TABLE(debug, 1, spans, bad_spans, "invalid debug span");
    bad_spans[0] = spans[0];
    bad_spans[0].start = 25;
    BAD_TABLE(debug, 1, spans, bad_spans, "invalid debug span");
    bad_spans[0] = spans[0];
    bad_spans[0].end = MO_LIMIT_FILE_SIZE + 1U;
    BAD_TABLE(debug, 1, spans, bad_spans, "invalid debug span");
    struct mo_bytecode_register_name bad_names[2];
    memcpy(bad_names, names, sizeof bad_names);
    bad_names[0].reg = UINT32_MAX;
    BAD_TABLE(debug, 1, names, bad_names, "invalid debug register range");
    bad_names[0] = names[0];
    bad_names[0].name = UINT32_MAX;
    BAD_TABLE(debug, 1, names, bad_names, "invalid debug register range");
    bad_names[0] = names[0];
    bad_names[0].code_end = 5;
    BAD_TABLE(debug, 1, names, bad_names, "invalid debug register range");
    bad_names[0].code_end = UINT32_MAX;
    BAD_TABLE(debug, 1, names, bad_names, "invalid debug register range");
    bad_names[0] = names[0];
    bad_names[0].code_start = bad_names[0].code_end;
    BAD_TABLE(debug, 1, names, bad_names, "invalid debug register range");
    bad_names[0] = names[0];
    bad_names[1].code_end = 8;
    BAD_TABLE(debug, 1, names, bad_names, "overlapping debug register ranges");

    const uint32_t broken_code[][5] = {
        {0, 2, 3, MO_OP_RET, 2},
        {MO_OP_CONST | (1U << 16), 2, 3, MO_OP_RET, 2},
        {MO_OP_CONST, UINT32_MAX, 3, MO_OP_RET, 2},
        {MO_OP_CONST, 2, UINT32_MAX, MO_OP_RET, 2},
        {MO_OP_JUMP, 1, MO_OP_RET, 2, MO_OP_RET}, /* jump to operand, then truncated ret */
        {MO_OP_JUMP, UINT32_MAX, MO_OP_RET, 2, MO_OP_RET}};
    for (size_t i = 0; i < sizeof broken_code / sizeof broken_code[0]; i++) {
        BAD_TABLE(functions, 0, code, broken_code[i],
                  i == 2 || i == 3 ? "instruction index out of range or target not a boundary"
                                   : "invalid instruction shape or instruction limit");
    }
    BAD_TABLE(functions, 0, code_count, 4, "invalid instruction shape or instruction limit");
    const uint32_t reg_limit[] = {MO_OP_RET, MO_LIMIT_FUNCTION_REGISTERS};
    struct mo_bytecode_function fn_copy[ARRAY_COUNT(functions)];
    memcpy(fn_copy, functions, sizeof fn_copy);
    fn_copy[0].code = reg_limit;
    fn_copy[0].code_count = 2;
    file = fixture;
    file.functions = fn_copy;
    expect_write_reason(&file, "instruction index out of range or target not a boundary");
    fn_copy[0] = functions[0];
    fn_copy[1].capture_types = link_layout;
    fn_copy[1].capture_count = 2;
    expect_write_reason(&file, "invalid function parameter/register/capture counts");
    fn_copy[1] = functions[1];
    fn_copy[6].parameters = wrong_param;
    fn_copy[6].parameter_count = 1;
    expect_write_reason(&file, "invalid function kind signature");
}

static void test_debug_metadata(void) {
    BAD_TABLE(debug, 1, file, ARRAY_COUNT(files), "invalid declaration span");
    BAD_TABLE(debug, 1, file, MO_BYTECODE_NO_INDEX, "invalid declaration span");
    BAD_TABLE(debug, 0, start, 1, "invalid declaration span");
    BAD_TABLE(debug, 0, end, 1, "invalid declaration span");
    BAD_TABLE(debug, 1, start, 32, "invalid declaration span");
    BAD_TABLE(debug, 1, end, MO_LIMIT_FILE_SIZE + 1U, "invalid declaration span");
    BAD_TABLE(debug, 1, subject, 1, "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 5, subject, ARRAY_COUNT(functions),
              "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 5, subject, 0, "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 5, subject, 5, "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 6, subject, 1, "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 5, line_count, 0, "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 5, line_count, MO_LIMIT_DEBUG_ENTRIES + 1U,
              "invalid array or count past limit");
    BAD_TABLE(debug, 5, lines, NULL, "invalid array or count past limit");
    BAD_TABLE(debug, 5, expectation, MO_BYTECODE_ALLOWS,
              "condition lines or expectation on wrong function kind");
    BAD_TABLE(debug, 6, expectation, MO_BYTECODE_NOT_ANCHOR,
              "invalid spec subject or missing linked metadata");
    BAD_TABLE(debug, 6, expectation, (enum mo_bytecode_anchor_expectation)3,
              "invalid anchor expectation");

    struct mo_bytecode_spec_line bad_lines[ARRAY_COUNT(lines)];
    memcpy(bad_lines, lines, sizeof bad_lines);
    bad_lines[0].file = ARRAY_COUNT(files);
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");
    bad_lines[0].file = MO_BYTECODE_NO_INDEX;
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");
    bad_lines[0] = lines[0];
    bad_lines[0].start = 111;
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");
    bad_lines[0] = lines[0];
    bad_lines[0].end = MO_LIMIT_FILE_SIZE + 1U;
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");
    bad_lines[0] = lines[0];
    bad_lines[0].text = MO_BYTECODE_NO_INDEX;
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");
    bad_lines[0] = lines[0];
    bad_lines[1].start = 1;
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");
    bad_lines[1] = lines[1];
    bad_lines[1].end = 1;
    BAD_TABLE(debug, 5, lines, bad_lines, "invalid condition line");

    struct mo_bytecode f = fixture;
    struct mo_bytecode_debug ds[ARRAY_COUNT(debug)];
    memcpy(ds, debug, sizeof ds);
    f.debug = ds;
    ds[6].line_count = 1;
    ds[6].lines = lines;
    expect_write_reason(&f, "condition lines or expectation on wrong function kind");
    ds[6] = debug[6];
    ds[6].expectation = MO_BYTECODE_FORBIDS;
    ds[1].start = MO_LIMIT_FILE_SIZE;
    ds[1].end = MO_LIMIT_FILE_SIZE; /* a zero-length declaration at the limit */
    round_trip(&f);
    ds[1] = debug[1];
    const struct mo_bytecode_span cleared[] = {{0, MO_BYTECODE_NO_INDEX, 0, 0}, {4, 0, 25, 30}};
    ds[1].spans = cleared;
    round_trip(&f); /* attribution clears, then resumes at a later instruction */
    for (uint32_t i = 0; i < ARRAY_COUNT(ds); i++) {
        ds[i] = (struct mo_bytecode_debug){.text = MO_BYTECODE_NO_INDEX,
                                           .file = MO_BYTECODE_NO_INDEX,
                                           .subject = MO_BYTECODE_NO_INDEX};
    }
    round_trip(&f); /* noSource has no spec text, links, conditions or expectations */

    /* Independent wire words for the final promise, anchor and source-less closure entries. */
    const uint32_t tail[] = {0, 0,          21,         0,   100,        140,        1, 0,  2,
                             0, 101,        110,        21,  UINT32_MAX, 0,          0, 22, 0,
                             0, 22,         0,          141, 150,        5,          1, 0,  0,
                             0, UINT32_MAX, UINT32_MAX, 0,   0,          UINT32_MAX, 0, 0};
    struct mo_buffer expected, out;
    struct mo_write_error error;
    mo_buffer_init(&expected, 1024);
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    for (size_t i = 0; i < sizeof tail / sizeof tail[0]; i++) {
        CHECK(mo_buffer_append_le32(&expected, tail[i]));
    }
    CHECK(mo_bytecode_encode(&fixture, &out, &error));
    CHECK(out.length >= expected.length);
    if (out.length >= expected.length) {
        size_t offset = out.length - expected.length;
        CHECK(equal_bytes(out.bytes + offset, expected.bytes, expected.length));
        const size_t slots[] = {3, 4, 6, 7, 8, 9, 10, 12, 24, 34};
        const uint32_t values[] = {
            ARRAY_COUNT(files),   141, 0, 1, MO_LIMIT_DEBUG_ENTRIES + 1U, ARRAY_COUNT(files), 111,
            MO_BYTECODE_NO_INDEX, 3,   1};
        const char *reasons[] = {"invalid declaration span",
                                 "invalid declaration span",
                                 "invalid spec subject or missing linked metadata",
                                 "condition lines or expectation on wrong function kind",
                                 "count past limit or remaining bytes",
                                 "invalid condition line",
                                 "invalid condition line",
                                 "invalid condition line",
                                 "invalid anchor expectation",
                                 "count past limit or remaining bytes"};
        const uint32_t reported[] = {3, 3, 6, 7, 8, 9, 9, 9, 24, 34};
        for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) {
            replace_word(&out, offset + slots[i] * 4, values[i]);
            checksum(&out);
            refuse_reason(out.bytes, out.length, reasons[i], (uint32_t)offset + reported[i] * 4);
            replace_word(&out, offset + slots[i] * 4, tail[slots[i]]);
        }
        /* Every truncation within the extension refuses with a valid checksum. */
        size_t length = out.length;
        for (size_t end = offset; end < length; end++) {
            out.length = end;
            checksum(&out);
            refuse_bytes(out.bytes, end);
        }
    }
    mo_buffer_free(&out);
    mo_buffer_free(&expected);
}

static void test_wire_refusals(void) {
    struct mo_buffer out;
    struct mo_write_error error;
    const struct mo_bytecode empty = {0};
    mo_buffer_init(&out, 1024);
    CHECK(mo_bytecode_encode(&empty, &out, &error));
    for (size_t i = 0; i < 20; i++) {
        out.bytes[i] ^= 0x80;
        uint32_t offset = i < 4 ? 0U : i < 6 ? 4U : i < 8 ? 6U : i < 12 ? 8U : 12U;
        const char *reason = i < 4    ? "invalid header or bytecode size"
                             : i < 12 ? "unsupported version or flags"
                                      : "checksum mismatch";
        refuse_reason(out.bytes, out.length, reason, offset);
        out.bytes[i] ^= 0x80;
    }
    for (size_t n = 0; n < out.length; n++) {
        refuse_bytes(out.bytes, n);
    }
    const uint32_t limits[] = {MO_LIMIT_STRINGS,   MO_LIMIT_TYPES,   MO_LIMIT_LAYOUTS,
                               MO_LIMIT_CONSTANTS, MO_LIMIT_GLOBALS, MO_LIMIT_INITS,
                               MO_LIMIT_FUNCTIONS, MO_LIMIT_MODULES, MO_LIMIT_FILES,
                               MO_LIMIT_FUNCTIONS};
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        replace_word(&out, 20 + i * 4, limits[i] + 1U);
        checksum(&out);
        refuse_reason(out.bytes, out.length, "count past limit or remaining bytes",
                      (uint32_t)(20 + i * 4));
        replace_word(&out, 20 + i * 4, limits[i]); /* within limit, not within remaining bytes */
        checksum(&out);
        refuse_reason(out.bytes, out.length, "count past limit or remaining bytes",
                      (uint32_t)(20 + i * 4));
        replace_word(&out, 20 + i * 4, 0);
    }
    checksum(&out);
    CHECK(mo_buffer_append_byte(&out, 0));
    checksum(&out);
    refuse_reason(out.bytes, out.length, "trailing bytes", 60);
    out.length--;
    checksum(&out);
    struct mo_arena no_memory;
    struct mo_load_error load_error;
    mo_arena_init(&no_memory, 0);
    CHECK(mo_bytecode_decode(&no_memory, out.bytes, out.length, &load_error) == NULL);
    CHECK(load_error.offset == 0 && strcmp(load_error.reason, "no memory") == 0);
    CHECK(load_error.kind == MO_BYTECODE_NO_MEMORY);
    mo_arena_free(&no_memory);
    refuse_bytes(out.bytes, (size_t)MO_LIMIT_BYTECODE_SIZE + 1); /* refused before reading */
    mo_buffer_free(&out);

    /* A record naming the second of two identical strings is structurally noncanonical. */
    static const uint8_t duplicate_name[] = {
        'M', 'O', 'B', 'C', 0, 0, 2, 0,   0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 0,
        0,   0,   'x', 1,   0, 0, 0, 'x', 1, 0, 0, 0, 17, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,   0,   0,   0,   0, 0, 0, 0,   0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    mo_buffer_init(&out, 1024);
    CHECK(mo_buffer_append(&out, duplicate_name, sizeof duplicate_name));
    CHECK(mo_buffer_append_le32(&out, 0)); /* files */
    checksum(&out);
    refuse_reason(out.bytes, out.length, "name does not use lowest string index", 42);
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
    const uint32_t suffix[] = {
        0,          0,  0,                /* constants, globals, init */
        1,                                /* functions */
        0,          0,  1, 0,             /* name, module, closure, not spec */
        1,          3,  0,                /* one owned Chain parameter */
        1,          2,  3, 2,             /* result Int, captures Chain, Text */
        2,          3,  7,                /* registers Chain, env */
        2,          71, 0,                /* ret r0: deliberately not type-verified in this task */
        1,          0,  0, 0,          0, /* module Chain with no exports or globals */
        0,                                /* files */
        1,          0,  0, UINT32_MAX,    /* debug: empty span/name arrays, no text */
        UINT32_MAX, 0,  0, UINT32_MAX, 0, 0}; /* no source or links/lines */
    const uint8_t header[20] = {'M', 'O', 'B', 'C', 0, 0, 2, 0};
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
    CHECK(mo_bytecode_encode(&f, &actual, &write_error));
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
        CHECK(mo_bytecode_encode(decoded, &actual, &write_error));
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
        if (i == 1 || i == 2) {
            expect_write(&f);
        } else {
            expect_write_reason(&f, "time literal out of range");
        }
    }
    copy[6] = constants[6];
    const uint64_t chars[] = {0xd7ff, 0xd800, 0xdfff, 0xe000, 0x10ffff, 0x110000};
    for (size_t i = 0; i < sizeof chars / sizeof chars[0]; i++) {
        literal_bits(&copy[5], chars[i]);
        if (i == 0 || i == 3 || i == 4) {
            expect_write(&f);
        } else {
            expect_write_reason(&f, "invalid scalar literal");
        }
    }
    copy[5] = constants[5];
    memset(copy[8].value.immediate, 0, 16);
    copy[8].value.immediate[15] = 0x80;
    expect_write_reason(&f, "decimal literal out of range"); /* -2^127 */
    copy[8].value.immediate[0] = 1;
    expect_write(&f); /* -(2^127 - 1) */
    memset(copy[8].value.immediate, 0xff, 16);
    copy[8].value.immediate[15] = 0x7f;
    expect_write(&f); /* 2^127 - 1 */
    const uint64_t floats[] = {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                               UINT64_C(0x7ff0000000000001), UINT64_C(0x8000000000000000)};
    for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++) {
        literal_bits(&copy[4], floats[i]);
        expect_write(&f);
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
        if (i < 7) {
            expect_write(&f);
        } else {
            expect_write_reason(&f, "invalid UTF-8 string");
        }
    }
    copy[21] = strings[21];
    copy[21].length = MO_LIMIT_STRING_SIZE + 1U;
    expect_write_reason(&f, "invalid array or count past limit");
}

static void test_filename_table(void) {
    struct mo_buffer wire;
    struct mo_write_error we;
    struct mo_load_error le;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode_bytes rows[MO_LIMIT_FILES];
    for (uint32_t i = 0; i < MO_LIMIT_FILES; i++)
        rows[i] = (struct mo_bytecode_bytes)STRING("\xff");
    struct mo_bytecode file = {.files = rows, .file_count = MO_LIMIT_FILES};
    CHECK(mo_bytecode_write(&file, &wire, &we));
    struct mo_image *image = mo_load(wire.bytes, wire.length, &le);
    CHECK(image != NULL);
    if (image != NULL) {
        memset(wire.bytes, 0, wire.length);
        const struct mo_bytecode *b = mo_image_bytecode(image);
        CHECK(b->file_count == MO_LIMIT_FILES);
        for (uint32_t i = 0; i < b->file_count; i++)
            CHECK(b->files[i].length == 1 && b->files[i].bytes[0] == 0xff);
        mo_image_free(image);
    }
    mo_buffer_clear(&wire);
    file.file_count++;
    CHECK(!mo_bytecode_write(&file, &wire, &we) && wire.length == 0 &&
          we.kind == MO_BYTECODE_LIMIT);
    file.file_count = 1;
    const struct mo_bytecode_bytes bad[] = {
        STRING(""), STRING("a\0b"), {MO_LIMIT_STRING_SIZE + 1U, (const uint8_t *)"x"}};
    for (uint32_t i = 0; i < ARRAY_COUNT(bad); i++) {
        rows[0] = bad[i];
        CHECK(!mo_bytecode_write(&file, &wire, &we) && wire.length == 0);
        CHECK(we.kind == (i == 2 ? MO_BYTECODE_LIMIT : MO_BYTECODE_INVALID));
    }
    struct mo_arena arena;
    mo_arena_init(&arena, 2 * (size_t)MO_LIMIT_STRING_SIZE);
    uint8_t *long_path = mo_arena_allocate(&arena, MO_LIMIT_STRING_SIZE, 1);
    CHECK(long_path != NULL);
    if (long_path != NULL) {
        memset(long_path, 0xff, MO_LIMIT_STRING_SIZE);
        rows[0] = (struct mo_bytecode_bytes){MO_LIMIT_STRING_SIZE, long_path};
        CHECK(mo_bytecode_write(&file, &wire, &we));
        image = mo_load(wire.bytes, wire.length, &le);
        CHECK(image != NULL);
        if (image != NULL) {
            const struct mo_bytecode_bytes *path = &mo_image_bytecode(image)->files[0];
            CHECK(path->length == MO_LIMIT_STRING_SIZE &&
                  memcmp(path->bytes, long_path, MO_LIMIT_STRING_SIZE) == 0);
            mo_image_free(image);
        }
        mo_buffer_clear(&wire);
    }
    mo_arena_free(&arena);
    rows[0] = (struct mo_bytecode_bytes)STRING("\xc3");
    CHECK(mo_bytecode_write(&file, &wire, &we));
    /* Old binary versions remain refused, even with the new section layout. */
    wire.bytes[6] = 1;
    CHECK(mo_load(wire.bytes, wire.length, &le) == NULL && le.offset == 6 &&
          le.kind == MO_BYTECODE_INVALID);
    mo_buffer_free(&wire);
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
    expect_write(&f); /* largest tuple and matching layout */
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
    expect_write(&f); /* generated promise's 256th argument */
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
    expect_write(&f); /* 255 explicit arguments plus env */
    fs[0].parameter_count++;
    registers[MO_LIMIT_BYTECODE_PARAMETERS - 1] = 3;
    registers[MO_LIMIT_BYTECODE_PARAMETERS] = 7;
    fs[0].register_count++;
    ts[6].parameter_count++;
    expect_write_reason(&f, "invalid function parameter/register/capture counts");
    fs[0] = functions[0];
    ts[6] = types[6];
    for (uint32_t i = 0; i < MO_LIMIT_FUNCTION_REGISTERS; i++) {
        many[i] = i < 2 ? 20U : 1U;
    }
    fs[1].register_types = many;
    fs[1].register_count = MO_LIMIT_FUNCTION_REGISTERS;
    expect_write(&f);
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
        expect_write(&f);
        fs[7].code_count += 2;
        expect_write_reason(&f, "invalid instruction shape or instruction limit");
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
        expect_write(&f); /* maximal variable tail; switch type compatibility is later */
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
    CHECK(mo_bytecode_encode(&fixture, &expected, &error));
    CHECK(mo_bytecode_encode(&f, &out, &error));
    CHECK(out.length == expected.length && equal_bytes(out.bytes, expected.bytes, out.length));
    mo_buffer_free(&out);
    mo_buffer_init(&out, expected.length - 1);
    CHECK(!mo_bytecode_encode(&fixture, &out, &error));
    CHECK(out.length == 0);
    mo_buffer_free(&out);
    mo_buffer_init(&out, expected.length);
    CHECK(mo_bytecode_encode(&fixture, &out, &error));
    CHECK(out.length == expected.length);
    struct mo_load_error load_error;
    const struct mo_bytecode *decoded =
        mo_bytecode_decode(&arena, out.bytes, out.length, &load_error);
    CHECK(decoded != NULL);
    if (decoded != NULL) {
        CHECK(decoded->types[3].reference && decoded->types[3].has_fields);
        CHECK(!decoded->types[21].reference && !decoded->types[21].has_fields);
        CHECK(decoded->types[22].reference && decoded->types[22].has_fields);
        CHECK(decoded->types[3].cases[2].unnamed);
        CHECK(decoded->constants[4].encoding == MO_CONSTANT_IMMEDIATE64);
        CHECK(decoded->constants[8].encoding == MO_CONSTANT_DECIMAL128);
        CHECK(decoded->constants[12].encoding == MO_CONSTANT_CLOSURE);
    }
    /* The exposed-type boolean is u32, not a C bool copied from memory. */
    const size_t module_start = out.length - (4 + 8 * 36 + 6 * 16) - 44;
    replace_word(&out, module_start + 16, 2);
    checksum(&out);
    refuse_bytes(out.bytes, out.length);
    mo_buffer_free(&out);
    mo_buffer_free(&expected);
    mo_arena_free(&arena);
    const char shared[] = "Counts";
    const struct mo_bytecode_string prefixes[] = {{1, (const uint8_t *)shared},
                                                  {6, (const uint8_t *)shared}};
    const struct mo_type record = {.kind = MO_TYPE_RECORD, .name = shared, .defined = true};
    const struct mo_bytecode prefixed = {
        .strings = prefixes, .string_count = 2, .types = &record, .type_count = 1};
    round_trip(&prefixed); /* pointer equality must not turn a complete name into a prefix */
}

static void test_large_tables(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, (size_t)1 << 30);
    uint32_t n = MO_LIMIT_TYPES;
    struct mo_type *ts = mo_arena_allocate_array(&arena, n, sizeof *ts, _Alignof(struct mo_type));
    struct mo_layout *ls =
        mo_arena_allocate_array(&arena, n, sizeof *ls, _Alignof(struct mo_layout));
    uint32_t *parameters = mo_arena_allocate_array(&arena, (size_t)(n - 2) * 3, sizeof *parameters,
                                                   _Alignof(uint32_t));
    struct mo_bytecode_type_export *es = mo_arena_allocate_array(
        &arena, MO_LIMIT_MODULE_NAMES, sizeof *es, _Alignof(struct mo_bytecode_type_export));
    CHECK(ts != NULL && ls != NULL && parameters != NULL && es != NULL);
    if (ts == NULL || ls == NULL || parameters == NULL || es == NULL) {
        mo_arena_free(&arena);
        return;
    }
    const struct mo_bytecode_string ss[] = {STRING("C"), STRING("x"), STRING("y")};
    const struct mo_type_field fields[] = {{"x", 0}, {"y", 0}};
    const struct mo_type_case cases[] = {{.name = "x", .fields = fields, .field_count = 2},
                                         {.name = "y", .fields = fields, .field_count = 2}};
    const uint32_t two_ints[] = {0, 0};
    ts[0] = (struct mo_type){.kind = MO_TYPE_INT, .defined = true};
    ts[1] = (struct mo_type){
        .kind = MO_TYPE_CHOICE, .name = "C", .defined = true, .cases = cases, .case_count = 2};
    ls[0] = (struct mo_layout){
        .kind = MO_LAYOUT_CASE, .type = 1, .case_index = 0, .fields = two_ints, .field_count = 2};
    ls[1] = ls[0];
    ls[1].case_index = 1;
    for (uint32_t i = 2; i < n; i++) {
        uint32_t *p = parameters + (size_t)(i - 2) * 3;
        p[0] = 0;
        p[1] = 0;
        p[2] = i + 1 < n ? i + 1 : 0;
        ts[i] = (struct mo_type){
            .kind = MO_TYPE_TUPLE, .parameters = p, .parameter_count = 3, .defined = true};
        ls[i] =
            (struct mo_layout){.kind = MO_LAYOUT_TUPLE, .type = i, .fields = p, .field_count = 3};
    }
    for (uint32_t i = 0; i < MO_LIMIT_MODULE_NAMES; i++) {
        es[i] = (struct mo_bytecode_type_export){i, (i & 1U) != 0};
    }
    struct mo_bytecode_module module = {
        .name = 0, .types = es, .type_count = MO_LIMIT_MODULE_NAMES};
    struct mo_bytecode f = {.strings = ss,
                            .string_count = ARRAY_COUNT(ss),
                            .types = ts,
                            .type_count = n,
                            .layouts = ls,
                            .layout_count = n,
                            .modules = &module,
                            .module_count = 1};
    /* A million forward dependencies exercise the explicit DFS stack. Two boxed cases let
     * both table counts reach their exact limits without duplicate layouts. */
    round_trip(&f);
    struct mo_buffer out;
    struct mo_write_error error;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_encode(&f, &out, &error));
    reject_word(&out, 39, n + 1, "count past limit or remaining bytes", 39);
    uint32_t layout_count = 43 + 4 + 60 + (n - 2) * 20;
    reject_word(&out, layout_count, MO_LIMIT_LAYOUTS + 1U, "count past limit or remaining bytes",
                layout_count);
    uint32_t module_count = (uint32_t)out.length - 8 - (16 + MO_LIMIT_MODULE_NAMES * 8);
    reject_word(&out, module_count + 4, MO_LIMIT_MODULE_NAMES + 1U,
                "count past limit or remaining bytes", module_count + 4);
    /* Equal type exports are refused even when they disagree on visibility. */
    reject_word(&out, module_count + 16, 0, "repeated exposed type", 20);
    f.type_count++;
    expect_write_reason(&f, "invalid array or count past limit");
    f.type_count--;
    f.layout_count++;
    expect_write_reason(&f, "invalid array or count past limit");
    mo_buffer_free(&out);
    mo_arena_free(&arena);
}

static void test_large_names(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 16 << 20);
    uint8_t *text = mo_arena_allocate(&arena, (size_t)MO_LIMIT_STRING_SIZE + 1, 1);
    CHECK(text != NULL);
    if (text == NULL) {
        mo_arena_free(&arena);
        return;
    }
    memset(text, 'a', MO_LIMIT_STRING_SIZE);
    text[MO_LIMIT_STRING_SIZE] = 0;
    const struct mo_bytecode_string s = {MO_LIMIT_STRING_SIZE, text};
    struct mo_type ts[300];
    for (uint32_t i = 0; i < ARRAY_COUNT(ts); i++) {
        ts[i] =
            (struct mo_type){.kind = MO_TYPE_RECORD, .name = (const char *)text, .defined = true};
    }
    const struct mo_bytecode f = {
        .strings = &s, .string_count = 1, .types = ts, .type_count = ARRAY_COUNT(ts)};
    round_trip(&f); /* names are shared, not copied once per nominal identity */
    struct mo_buffer out;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    const uint8_t header[20] = {'M', 'O', 'B', 'C', 0, 0, 2, 0};
    CHECK(mo_buffer_append(&out, header, sizeof header));
    CHECK(mo_buffer_append_le32(&out, 1));
    CHECK(mo_buffer_append_le32(&out, MO_LIMIT_STRING_SIZE));
    CHECK(mo_buffer_append(&out, text, MO_LIMIT_STRING_SIZE));
    const uint32_t prefix[] = {2, 3, 17, 0, MO_LIMIT_FIELDS};
    for (uint32_t i = 0; i < ARRAY_COUNT(prefix); i++) {
        CHECK(mo_buffer_append_le32(&out, prefix[i]));
    }
    for (uint32_t i = 0; i < MO_LIMIT_FIELDS; i++) {
        CHECK(mo_buffer_append_le32(&out, 0));
        CHECK(mo_buffer_append_le32(&out, 0));
    }
    for (unsigned i = 0; i < 8; i++) {
        CHECK(mo_buffer_append_le32(&out, 0));
    }
    checksum(&out);
    clock_t start = clock();
    refuse_reason(out.bytes, out.length, "duplicate field name", 20);
    clock_t end = clock();
    CHECK(start != (clock_t)-1 && end != (clock_t)-1);
    /* The old repeated scans take tens of CPU seconds; allow ample sanitizer/CI margin. */
    double seconds = (double)(end - start) / CLOCKS_PER_SEC;
    CHECK(seconds < 10);
    CHECK(printf("bytecode repeated-name refusal: %.3f CPU seconds\n", seconds) >= 0);
    reject_word(&out, 24, MO_LIMIT_STRING_SIZE + 1U, "byte length past limit or remaining bytes",
                24);
    mo_buffer_free(&out);
    mo_arena_free(&arena);
}

/* Reproduce collisions in the old heap interner without depending on its current hash.
 * Distinct tuples all land in a 1024-slot window of a 262144-slot table. */
static MO_MUST_USE uint64_t collision_mix(uint64_t hash, uint32_t value) {
    hash ^= value;
    hash *= UINT64_C(0x9e3779b185ebca87);
    return hash ^ (hash >> 32);
}

static MO_MUST_USE struct mo_bytecode collision_fixture(struct mo_arena *arena) {
    enum { NOMINALS = 257, TUPLES = 65536 };
    static const struct mo_bytecode_string s = STRING("C");
    struct mo_type *ts =
        mo_arena_allocate_array(arena, NOMINALS + TUPLES, sizeof *ts, _Alignof(struct mo_type));
    uint32_t *parameters =
        mo_arena_allocate_array(arena, TUPLES * 3, sizeof *parameters, _Alignof(uint32_t));
    CHECK(ts != NULL && parameters != NULL);
    if (ts == NULL || parameters == NULL) {
        return (struct mo_bytecode){0};
    }
    for (uint32_t i = 0; i < NOMINALS; i++) {
        ts[i] = (struct mo_type){.kind = MO_TYPE_RECORD, .name = "C", .defined = true};
    }
    uint64_t base = collision_mix(0, 13);
    for (unsigned i = 0; i < 3; i++) {
        base = collision_mix(base, 0);
    }
    base = collision_mix(base, 3);
    uint32_t count = 0;
    for (uint32_t a = 0; a < NOMINALS && count < TUPLES; a++) {
        uint64_t first = collision_mix(base, a);
        for (uint32_t b = 0; b < NOMINALS && count < TUPLES; b++) {
            uint64_t second = collision_mix(first, b);
            for (uint32_t c = 0; c < NOMINALS && count < TUPLES; c++) {
                if ((collision_mix(second, c) & UINT64_C(262143)) >= 1024) {
                    continue;
                }
                uint32_t *p = parameters + (size_t)count * 3;
                p[0] = a;
                p[1] = b;
                p[2] = c;
                ts[NOMINALS + count++] = (struct mo_type){
                    .kind = MO_TYPE_TUPLE, .parameters = p, .parameter_count = 3, .defined = true};
            }
        }
    }
    CHECK(count == TUPLES);
    return (struct mo_bytecode){
        .strings = &s, .string_count = 1, .types = ts, .type_count = NOMINALS + count};
}

static void test_hash_collisions(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 16 << 20);
    struct mo_bytecode f = collision_fixture(&arena);
    clock_t start = clock();
    round_trip(&f);
    clock_t end = clock();
    CHECK(start != (clock_t)-1 && end != (clock_t)-1);
    double seconds = (double)(end - start) / CLOCKS_PER_SEC;
    CHECK(seconds < 10);
    CHECK(printf("bytecode collision round trip: %.3f CPU seconds\n", seconds) >= 0);
    mo_arena_free(&arena);
}

static void test_large_choice_and_code(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 16 << 20);
    uint32_t n = MO_LIMIT_CHOICE_CASES;
    struct mo_bytecode_string *ss = mo_arena_allocate_array(&arena, (size_t)n + 1, sizeof *ss,
                                                            _Alignof(struct mo_bytecode_string));
    struct mo_type_case *cases =
        mo_arena_allocate_array(&arena, n, sizeof *cases, _Alignof(struct mo_type_case));
    char *text = mo_arena_allocate(&arena, (size_t)n * 6, 1);
    CHECK(ss != NULL && cases != NULL && text != NULL);
    if (ss != NULL && cases != NULL && text != NULL) {
        ss[0] = (struct mo_bytecode_string)STRING("Choice");
        for (uint32_t i = 0; i < n; i++) {
            char *name = text + (size_t)i * 6;
            CHECK(snprintf(name, 6, "c%04x", i) == 5);
            ss[i + 1] = (struct mo_bytecode_string){5, (const uint8_t *)name};
            cases[i] = (struct mo_type_case){.name = name};
        }
        struct mo_type t = {.kind = MO_TYPE_CHOICE,
                            .name = "Choice",
                            .defined = true,
                            .cases = cases,
                            .case_count = n};
        struct mo_bytecode f = {.strings = ss, .string_count = n + 1, .types = &t, .type_count = 1};
        round_trip(&f);
        struct mo_buffer out;
        struct mo_write_error error;
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        CHECK(mo_bytecode_encode(&f, &out, &error));
        uint32_t count = 34 + 9 * n + 12;
        reject_word(&out, count, n + 1, "count past limit or remaining bytes", count);
        t.case_count++;
        expect_write_reason(&f, "invalid array or count past limit");
        mo_buffer_free(&out);
    }
    uint32_t *code = mo_arena_allocate_array(&arena, MO_LIMIT_FUNCTION_CODE_WORDS + 1U,
                                             sizeof *code, _Alignof(uint32_t));
    CHECK(code != NULL);
    if (code != NULL) {
        for (uint32_t i = 0; i < MO_LIMIT_FUNCTION_CODE_WORDS; i += 4) {
            code[i] = MO_OP_FIELD;
            code[i + 1] = 0;
            code[i + 2] = 0;
            code[i + 3] = 0;
        }
        struct mo_bytecode f = fixture;
        struct mo_bytecode_function fs[ARRAY_COUNT(functions)];
        memcpy(fs, functions, sizeof fs);
        f.functions = fs;
        fs[7].code = code;
        fs[7].code_count = MO_LIMIT_FUNCTION_CODE_WORDS;
        round_trip(&f); /* also exactly MO_LIMIT_FUNCTION_INSTRUCTIONS */
        fs[7].code_count++;
        expect_write_reason(&f, "invalid array or count past limit");
    }
    mo_arena_free(&arena);
}

struct fixture_positions {
    uint32_t types[ARRAY_COUNT(types)];
    uint32_t layouts[ARRAY_COUNT(layouts)];
    uint32_t constants[ARRAY_COUNT(constants)];
    uint32_t functions[ARRAY_COUNT(functions)];
    uint32_t code[ARRAY_COUNT(functions)];
    uint32_t debug[ARRAY_COUNT(debug)];
    uint32_t globals;
    uint32_t init;
    uint32_t module;
};

/* Offsets from the documented wire sizes, not native sizeofs or decoder bookkeeping. */
static MO_MUST_USE struct fixture_positions fixture_positions(void) {
    struct fixture_positions p = {0};
    uint32_t next = 24;
    for (uint32_t i = 0; i < ARRAY_COUNT(strings); i++) {
        next += 4 + strings[i].length;
    }
    next += 4;
    for (uint32_t i = 0; i < ARRAY_COUNT(types); i++) {
        p.types[i] = next;
        const struct mo_type *t = &types[i];
        next += 4;
        if (t->kind >= MO_TYPE_TUPLE && t->kind <= MO_TYPE_MAP) {
            next += 4 + t->parameter_count * 4;
        } else if (t->kind == MO_TYPE_FUNCTION) {
            next += 8 + t->parameter_count * 4;
        } else if (t->kind == MO_TYPE_RECORD) {
            next += 8 + t->field_count * 8;
        } else if (t->kind == MO_TYPE_CHOICE) {
            next += 8;
            for (uint32_t j = 0; j < t->case_count; j++) {
                next += 8 + t->cases[j].field_count * 8;
            }
        } else if (t->kind == MO_TYPE_ENV || t->kind == MO_TYPE_CAPABILITY) {
            next += 4;
        }
    }
    next += 4;
    for (uint32_t i = 0; i < ARRAY_COUNT(layouts); i++) {
        p.layouts[i] = next;
        const struct mo_layout *l = &layouts[i];
        next += 12 + l->field_count * 4;
        if (l->kind == MO_LAYOUT_CASE || l->kind == MO_LAYOUT_CLOSURE) {
            next += 4;
        }
    }
    next += 4;
    for (uint32_t i = 0; i < ARRAY_COUNT(constants); i++) {
        p.constants[i] = next;
        const struct mo_bytecode_constant *k = &constants[i];
        enum mo_type_kind kind = types[k->type].kind;
        next += 4;
        next += kind == MO_TYPE_DECIMAL    ? 16U
                : kind <= MO_TYPE_DURATION ? 8U
                : kind == MO_TYPE_BYTES    ? 4 + k->value.bytes.length
                : kind == MO_TYPE_BIGINT   ? 8 + k->value.bigint.magnitude.length
                                           : 4U;
    }
    p.globals = next + 4;
    next += 4 + ARRAY_COUNT(globals) * 12;
    p.init = next;
    next += 4 + ARRAY_COUNT(init) * 4;
    next += 4;
    for (uint32_t i = 0; i < ARRAY_COUNT(functions); i++) {
        p.functions[i] = next;
        const struct mo_bytecode_function *fn = &functions[i];
        next += 36 + fn->parameter_count * 8 + fn->capture_count * 4 + fn->register_count * 4;
        p.code[i] = next;
        next += fn->code_count * 4;
    }
    p.module = next + 4;
    next += 4 + 16 + modules[0].type_count * 8 +
            (modules[0].function_count + modules[0].global_count) * 4;
    next += 4;
    for (uint32_t i = 0; i < ARRAY_COUNT(files); i++)
        next += 4 + files[i].length;
    next += 4;
    for (uint32_t i = 0; i < ARRAY_COUNT(debug); i++) {
        p.debug[i] = next;
        next += 36 + (debug[i].span_count + debug[i].name_count + debug[i].line_count) * 16;
    }
    return p;
}

static void test_refusal_details(void) {
    struct mo_buffer out;
    struct mo_write_error error;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_encode(&fixture, &out, &error));
    struct fixture_positions p = fixture_positions();
    struct mutation {
        uint32_t offset;
        uint32_t value;
        const char *reason;
        uint32_t reported;
    };
#define LOCAL(at, value, reason)                                                                   \
    { at, value, reason, at }
#define RELATION(at, value, reason)                                                                \
    { at, value, reason, 20 }
    const struct mutation cases[] = {
        LOCAL(p.types[0], 22, "unknown type kind"),
        LOCAL(p.types[3] + 4, ARRAY_COUNT(strings), "name string index out of range"),
        LOCAL(p.types[3] + 4, 22, "name is empty or contains NUL"),
        LOCAL(p.types[3] + 4, 21, "name is empty or contains NUL"),
        LOCAL(p.types[25] + 4, 7, "unknown capability"),
        LOCAL(p.types[3] + 8, 0, "choice has no cases"),
        LOCAL(p.types[4] + 4, 0, "invalid type parameter count"),
        LOCAL(p.types[5] + 4, 1, "invalid type parameter count"),
        LOCAL(p.types[19] + 4, 1, "invalid type parameter count"),
        RELATION(p.types[4] + 8, ARRAY_COUNT(types), "type index out of range"),
        RELATION(p.types[4] + 8, 4, "structural type cycle"),
        RELATION(p.types[6] + 12, ARRAY_COUNT(types),
                 "type result or env function out of range/kind"),
        RELATION(p.types[7] + 4, 1, "closure env mismatch"),
        RELATION(p.types[8], 3, "duplicate interned type"),
        RELATION(p.types[20] + 20, 7, "duplicate field name"),
        RELATION(p.types[21] + 20, 1, "duplicate case name"),
        LOCAL(p.layouts[0], 4, "unknown layout kind"),
        RELATION(p.layouts[0] + 4, ARRAY_COUNT(types), "layout type out of range"),
        RELATION(p.layouts[0] + 8, 0, "invalid case layout"),
        RELATION(p.layouts[0] + 16, ARRAY_COUNT(types), "layout field type out of range"),
        RELATION(p.layouts[0] + 16, 0, "layout field type mismatch"),
        LOCAL(p.constants[0], ARRAY_COUNT(types), "constant type out of range"),
        LOCAL(p.constants[0], 15, "type has no literal encoding"),
        RELATION(p.constants[0] + 4, 1, "invalid scalar literal"),
        LOCAL(p.constants[11] + 4, 3, "invalid bigint sign"),
        RELATION(p.constants[9] + 4, ARRAY_COUNT(strings), "text string index out of range"),
        RELATION(p.constants[12] + 4, 0, "invalid closure literal"),
        LOCAL(p.functions[0] + 8, 7, "unknown function kind"),
        LOCAL(p.functions[0] + 12, 2, "invalid boolean"),
        LOCAL(p.functions[0] + 24, 2, "invalid parameter ownership"),
        LOCAL(p.functions[0] + 16, MO_LIMIT_BYTECODE_PARAMETERS + 1U,
              "count past limit or remaining bytes"),
        LOCAL(p.code[0] - 4, MO_LIMIT_FUNCTION_CODE_WORDS + 1U,
              "count past limit or remaining bytes"),
        RELATION(p.globals, ARRAY_COUNT(types), "global type, name or module out of range"),
        RELATION(p.init + 4, 1, "invalid or repeated init function"),
        LOCAL(p.module + 12, 2, "invalid boolean"),
        RELATION(p.module + 16, 20, "repeated exposed type"),
        LOCAL(p.code[0], 0, "invalid instruction shape or instruction limit"),
        LOCAL(p.code[0] + 4, 3, "instruction index out of range or target not a boundary"),
        LOCAL(p.debug[1] + 20, 0, "invalid debug span"),
        LOCAL(p.debug[1] + 36 + 4, ARRAY_COUNT(add_regs), "invalid debug register range"),
        LOCAL(p.debug[1] + 72 + 4, 22, "invalid declaration span"),
        LOCAL(p.debug[5] + 24, 6, "invalid spec subject or missing linked metadata"),
        LOCAL(p.debug[6] + 28, 3, "invalid anchor expectation")};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reject_word(&out, cases[i].offset, cases[i].value, cases[i].reason, cases[i].reported);
    }
#undef RELATION
#undef LOCAL
    /* An unused env entry reaches the type-table check without a closure register masking it. */
    replace_word(&out, p.types[25], MO_TYPE_ENV);
    replace_word(&out, p.types[25] + 4, ARRAY_COUNT(functions));
    checksum(&out);
    refuse_reason(out.bytes, out.length, "type result or env function out of range/kind", 20);
    mo_buffer_clear(&out);
    CHECK(mo_bytecode_encode(&fixture, &out, &error));
    /* A closure entry and a two-field case have equal wire widths. Replace the former with
     * another copy of case layout 0, retaining all following section boundaries. */
    const uint32_t duplicate[] = {2, 3, 1, 2, 1, 3};
    for (uint32_t i = 0; i < ARRAY_COUNT(duplicate); i++) {
        replace_word(&out, p.layouts[3] + i * 4, duplicate[i]);
    }
    checksum(&out);
    refuse_reason(out.bytes, out.length, "duplicate layout", 20);
    mo_buffer_clear(&out);
    CHECK(mo_bytecode_encode(&fixture, &out, &error));
    /* A newly declared init is absent from the order, not merely an invalid init index. */
    replace_word(&out, p.functions[3] + 8, MO_FUNCTION_INIT);
    replace_word(&out, p.functions[3] + 12, 0);
    checksum(&out);
    refuse_reason(out.bytes, out.length, "init function absent from init section", 20);
    mo_buffer_free(&out);

    /* The last scalar payload is short, but the advertised count's minimum still fits. */
    mo_buffer_init(&out, 1024);
    const uint8_t header[20] = {'M', 'O', 'B', 'C', 0, 0, 2, 0};
    CHECK(mo_buffer_append(&out, header, sizeof header));
    const uint32_t prefix[] = {0, 1, 3, 0, 1, 0};
    for (uint32_t i = 0; i < ARRAY_COUNT(prefix); i++) {
        CHECK(mo_buffer_append_le32(&out, prefix[i]));
    }
    for (unsigned i = 0; i < 7; i++) {
        CHECK(mo_buffer_append_byte(&out, 0));
    }
    checksum(&out);
    refuse_reason(out.bytes, out.length, "truncated literal", 44);
    mo_buffer_free(&out);
}

static void test_operand_refusals(void) {
    const char *reason = "instruction index out of range or target not a boundary";
    struct fixture_positions p = fixture_positions();
    for (uint32_t i = 0; i < MO_OPCODE_COUNT; i++) {
        const struct mo_opcode_info *op = &mo_opcodes[i];
        uint32_t code[1 + MO_OPCODE_FIXED_OPERANDS + 2] = {op->number};
        uint32_t tail = op->variable_limit == 0 ? 0U : 2U;
        code[0] |= tail << 16;
        struct mo_bytecode f = fixture;
        struct mo_bytecode_function fs[ARRAY_COUNT(functions)];
        memcpy(fs, functions, sizeof fs);
        f.functions = fs;
        fs[7].code = code;
        fs[7].code_count = 1U + op->fixed_count + tail;
        struct mo_buffer out;
        struct mo_write_error error;
        mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
        CHECK(mo_bytecode_encode(&f, &out, &error));
        for (uint32_t j = 0; j < op->fixed_count + tail; j++) {
            enum mo_operand_kind kind = j < op->fixed_count ? op->fixed[j].kind : op->variable.kind;
            uint32_t limit = 0;
            switch (kind) {
            case MO_OPERAND_REGISTER:
                limit = fs[7].register_count;
                break;
            case MO_OPERAND_TYPE:
                limit = f.type_count;
                break;
            case MO_OPERAND_LAYOUT:
                limit = f.layout_count;
                break;
            case MO_OPERAND_CONSTANT:
                limit = f.constant_count;
                break;
            case MO_OPERAND_GLOBAL:
                limit = f.global_count;
                break;
            case MO_OPERAND_FUNCTION:
                limit = f.function_count;
                break;
            case MO_OPERAND_PRIMITIVE:
                limit = MO_PRIM_COUNT;
                break;
            case MO_OPERAND_HOST:
                limit = MO_HOST_COUNT;
                break;
            case MO_OPERAND_TARGET:
                limit = fs[7].code_count;
                break;
            case MO_OPERAND_FIELD:
                limit = MO_LIMIT_FIELDS;
                break;
            case MO_OPERAND_CASE:
                limit = MO_LIMIT_CHOICE_CASES;
                break;
            case MO_OPERAND_NONE:
                MO_UNREACHABLE();
            }
            uint32_t offset = p.code[7] + 4 * (1 + j);
            reject_word(&out, offset, limit, reason, offset);
        }
        mo_buffer_free(&out);
    }
}

int main(void) {
    test_empty();
    round_trip(&fixture);
    test_refusals();
    test_debug_metadata();
    test_wire_refusals();
    test_worked_encoding();
    test_literal_boundaries();
    test_utf8();
    test_filename_table();
    test_limits_and_caches();
    test_large_tables();
    test_large_names();
    test_hash_collisions();
    test_large_choice_and_code();
    test_refusal_details();
    test_operand_refusals();
    return mo_test_report("test_bytecode_io");
}
