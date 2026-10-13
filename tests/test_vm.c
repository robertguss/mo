#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "base/codes.h"
#include "base/stack.h"
#include "codegen/codegen.h"
#include "test.h"
#include "vm/vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vm_run.h"

static const char *corpus;
static unsigned seed;

static MO_MUST_USE struct mo_image *load(const struct mo_bytecode *file) {
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_write_error write_error;
    bool written = mo_bytecode_write(file, &wire, &write_error);
    CHECK(written);
    if (!written)
        (void)fprintf(stderr, "writer: %s\n", write_error.reason);
    if (written && corpus != NULL) {
        char path[1024];
        int n = snprintf(path, sizeof path, "%s/vm-%u.bc", corpus, seed++);
        MO_ASSERT(n >= 0 && (size_t)n < sizeof path);
        FILE *stream = fopen(path, "wb");
        MO_ASSERT(stream != NULL);
        CHECK(fwrite(wire.bytes, 1, wire.length, stream) == wire.length);
        CHECK(fclose(stream) == 0);
    }
    struct mo_load_error error;
    struct mo_image *image = written ? mo_load(wire.bytes, wire.length, &error) : NULL;
    CHECK(image != NULL);
    mo_buffer_free(&wire);
    return image;
}

static MO_MUST_USE enum mo_outcome number_report(uint32_t op, uint32_t left_type,
                                                 uint32_t right_type, uint32_t result_type,
                                                 struct mo_cell left, struct mo_cell right,
                                                 struct mo_cell *out, struct mo_buffer *text,
                                                 const char **reason) {
    struct mo_type types[14] = {0};
    for (unsigned i = 0; i < 13; i++)
        types[i].kind = (enum mo_type_kind)i;
    const struct mo_type_case ordering[] = {
        {.name = "Less"}, {.name = "Equal"}, {.name = "Greater"}};
    types[13] = (struct mo_type){.kind = MO_TYPE_CHOICE,
                                 .name = "Ordering",
                                 .defined = true,
                                 .cases = ordering,
                                 .case_count = 3};
    const struct mo_bytecode_string strings[] = {{1, (const uint8_t *)"M"},
                                                 {8, (const uint8_t *)"Ordering"},
                                                 {4, (const uint8_t *)"Less"},
                                                 {5, (const uint8_t *)"Equal"},
                                                 {7, (const uint8_t *)"Greater"}};
    const struct mo_parameter params[] = {{left_type, MO_PARAMETER_OWNED},
                                          {right_type, MO_PARAMETER_OWNED}};
    const uint32_t registers[] = {left_type, right_type, result_type};
    uint32_t code[] = {op, 2, 0, 1, MO_OP_RET, 2};
    const struct mo_opcode_info *info = mo_opcode_find(op);
    MO_ASSERT(info != NULL);
    uint32_t end = 1 + info->fixed_count;
    code[end] = MO_OP_RET;
    code[end + 1] = 2;
    const struct mo_bytecode_function fn = {.name = 0,
                                            .module = 0,
                                            .result_type = result_type,
                                            .parameters = params,
                                            .parameter_count = 2,
                                            .register_types = registers,
                                            .register_count = 3,
                                            .code = code,
                                            .code_count = end + 2};
    const struct mo_bytecode_debug debug = {
        .text = MO_NO_INDEX, .file = MO_NO_INDEX, .subject = MO_NO_INDEX};
    const struct mo_bytecode_module module = {.name = 0};
    const struct mo_bytecode file = {.strings = strings,
                                     .string_count = 5,
                                     .types = types,
                                     .type_count = 14,
                                     .functions = &fn,
                                     .function_count = 1,
                                     .modules = &module,
                                     .module_count = 1,
                                     .debug = &debug,
                                     .debug_count = 1};
    struct mo_image *image = load(&file);
    if (image == NULL)
        return MO_STOPPED;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell arguments[] = {left, right};
    enum mo_outcome outcome = mo_call(p, 0, arguments, 2, out, &defect);
    if (outcome != MO_RETURNED && reason != NULL)
        *reason = defect.reason;
    if (outcome == MO_RETURNED && text != NULL) {
        if (result_type == MO_TYPE_FLOAT) {
            uint64_t bits;
            memcpy(&bits, out->bytes, sizeof bits);
            MO_ASSERT(mo_buffer_append_text(text, "0x") && mo_buffer_append_hex(text, bits, 16));
        } else
            MO_ASSERT(mo_value_print(mo_process_heap(p), result_type, *out, text));
    }
    mo_process_free(p);
    mo_image_free(image);
    return outcome;
}

static MO_MUST_USE enum mo_outcome number(uint32_t op, uint32_t left_type, uint32_t right_type,
                                          uint32_t result_type, struct mo_cell left,
                                          struct mo_cell right, struct mo_cell *out) {
    return number_report(op, left_type, right_type, result_type, left, right, out, NULL, NULL);
}

static void test_numbers(void) {
    struct mo_cell out = mo_cell_int(99);
    CHECK(number(MO_OP_ADD_INT, MO_TYPE_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(13),
                 mo_cell_int(-20), &out) == MO_RETURNED);
    CHECK(mo_cell_get_int(out) == -7);
    const mo_i128 scale = INT64_C(1000000000000000000);
    const int64_t integers[] = {-8, -18, -65, -2, -3};
    const mo_i128 decimals[] = {-8 * scale, -18 * scale, -65 * scale, -26 * scale / 10, -3 * scale};
    const double floats[] = {-8.5, -18.5, -67.5, -2.7, -3.5};
    for (unsigned family = 0; family < 5; family++) {
        uint32_t op = MO_OP_ADD_INT + 3 * family;
        CHECK(number(op, MO_TYPE_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(-13), mo_cell_int(5),
                     &out) == MO_RETURNED);
        CHECK(mo_cell_get_int(out) == integers[family]);
        CHECK(number(op + 1, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL,
                     mo_cell_decimal(-13 * scale), mo_cell_decimal(5 * scale),
                     &out) == MO_RETURNED);
        CHECK(mo_cell_get_decimal(out) == decimals[family]);
        CHECK(number(op + 2, MO_TYPE_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, mo_cell_float(-13.5),
                     mo_cell_float(5), &out) == MO_RETURNED);
        CHECK(mo_cell_get_float(out) == floats[family]);
    }
    const struct {
        uint32_t type;
        struct mo_cell left, right;
    } comparisons[] = {
        {MO_TYPE_INT, mo_cell_int(-13), mo_cell_int(5)},
        {MO_TYPE_DECIMAL, mo_cell_decimal(-13), mo_cell_decimal(5)},
        {MO_TYPE_FLOAT, mo_cell_float(-13.5), mo_cell_float(5)},
        {MO_TYPE_BOOL, mo_cell_bool(false), mo_cell_bool(true)},
        {MO_TYPE_BYTE, mo_cell_byte(13), mo_cell_byte(20)},
        {MO_TYPE_CHAR, mo_cell_char(0xe9), mo_cell_char(0x1f600)},
        {MO_TYPE_TIME, mo_cell_time(-1), mo_cell_time(5)},
        {MO_TYPE_DURATION, mo_cell_duration(-13), mo_cell_duration(5)},
        {13, mo_cell_case(0, mo_cell_unit()), mo_cell_case(2, mo_cell_unit())},
    };
    for (unsigned kind = 0; kind < 9; kind++) {
        for (unsigned family = 0; family < 3; family++) {
            CHECK(number(MO_OP_EQ_INT + kind + 9 * family, comparisons[kind].type,
                         comparisons[kind].type, MO_TYPE_BOOL, comparisons[kind].left,
                         comparisons[kind].right, &out) == MO_RETURNED);
            CHECK(mo_cell_get_bool(out) == (family != 0));
        }
    }
    for (unsigned family = 0; family < 3; family++) {
        CHECK(number(MO_OP_EQ_FLOAT + 9 * family, MO_TYPE_FLOAT, MO_TYPE_FLOAT, MO_TYPE_BOOL,
                     mo_cell_float(NAN), mo_cell_float(5), &out) == MO_RETURNED);
        CHECK(!mo_cell_get_bool(out));
    }
    CHECK(number(MO_OP_CMP_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, 13, mo_cell_float(NAN),
                 mo_cell_float(5), &out) == MO_RETURNED);
    CHECK(mo_cell_get_tag(out) == 2);
    CHECK(number(MO_OP_CMP_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, 13, mo_cell_float(5),
                 mo_cell_float(NAN), &out) == MO_RETURNED);
    CHECK(mo_cell_get_tag(out) == 0);
    CHECK(number(MO_OP_CMP_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, 13, mo_cell_float(NAN),
                 mo_cell_float(NAN), &out) == MO_RETURNED);
    CHECK(mo_cell_get_tag(out) == 1);
    CHECK(number(MO_OP_NEG_INT, MO_TYPE_INT, MO_TYPE_UNIT, MO_TYPE_INT, mo_cell_int(-13),
                 mo_cell_unit(), &out) == MO_RETURNED &&
          mo_cell_get_int(out) == 13);
    CHECK(number(MO_OP_NEG_DURATION, MO_TYPE_DURATION, MO_TYPE_UNIT, MO_TYPE_DURATION,
                 mo_cell_duration(-13), mo_cell_unit(), &out) == MO_RETURNED &&
          mo_cell_get_duration(out) == 13);
    CHECK(number(MO_OP_NEG_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_UNIT, MO_TYPE_DECIMAL,
                 mo_cell_decimal(-13), mo_cell_unit(), &out) == MO_RETURNED &&
          mo_cell_get_decimal(out) == 13);
    CHECK(number(MO_OP_NEG_FLOAT, MO_TYPE_FLOAT, MO_TYPE_UNIT, MO_TYPE_FLOAT, mo_cell_float(0),
                 mo_cell_unit(), &out) == MO_RETURNED &&
          signbit(mo_cell_get_float(out)));
    CHECK(number(MO_OP_NOT, MO_TYPE_BOOL, MO_TYPE_UNIT, MO_TYPE_BOOL, mo_cell_bool(false),
                 mo_cell_unit(), &out) == MO_RETURNED &&
          mo_cell_get_bool(out));
    CHECK(number(MO_OP_MAKE_RANGE, MO_TYPE_INT, MO_TYPE_INT, MO_TYPE_RANGE, mo_cell_int(20),
                 mo_cell_int(-13), &out) == MO_RETURNED);
    CHECK(mo_cell_get_range(out).start == 20 && mo_cell_get_range(out).end == -13);
    CHECK(number(MO_OP_RANGE_START, MO_TYPE_RANGE, MO_TYPE_UNIT, MO_TYPE_INT,
                 mo_cell_range(20, -13), mo_cell_unit(), &out) == MO_RETURNED &&
          mo_cell_get_int(out) == 20);
    CHECK(number(MO_OP_RANGE_END, MO_TYPE_RANGE, MO_TYPE_UNIT, MO_TYPE_INT, mo_cell_range(20, -13),
                 mo_cell_unit(), &out) == MO_RETURNED &&
          mo_cell_get_int(out) == -13);
    const uint32_t duration_ops[] = {MO_OP_ADD_DURATION, MO_OP_SUB_DURATION, MO_OP_SCALE_DURATION};
    const int64_t duration_results[] = {-8, -18, -65};
    for (unsigned i = 0; i < 3; i++) {
        CHECK(number(duration_ops[i], MO_TYPE_DURATION, i == 2 ? MO_TYPE_INT : MO_TYPE_DURATION,
                     MO_TYPE_DURATION, mo_cell_duration(-13), mo_cell_int(5), &out) == MO_RETURNED);
        CHECK(mo_cell_get_duration(out) == duration_results[i]);
    }
    CHECK(number(MO_OP_ADD_TIME, MO_TYPE_TIME, MO_TYPE_DURATION, MO_TYPE_TIME, mo_cell_time(-13),
                 mo_cell_duration(5), &out) == MO_RETURNED &&
          mo_cell_get_time(out) == -8);
    CHECK(number(MO_OP_SUB_TIME, MO_TYPE_TIME, MO_TYPE_DURATION, MO_TYPE_TIME, mo_cell_time(-13),
                 mo_cell_duration(5), &out) == MO_RETURNED &&
          mo_cell_get_time(out) == -18);
    CHECK(number(MO_OP_DIFF_TIME, MO_TYPE_TIME, MO_TYPE_TIME, MO_TYPE_DURATION,
                 mo_cell_time(INT64_C(253402300799999999)),
                 mo_cell_time(INT64_C(-62135596800000000)), &out) == MO_RETURNED &&
          mo_cell_get_duration(out) == INT64_C(315537897599999999));
    const mo_i128 maximum = (mo_i128)(((mo_u128)1 << 127) - 1);
    CHECK(number(MO_OP_MUL_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL,
                 mo_cell_decimal(maximum), mo_cell_decimal(scale), &out) == MO_RETURNED);
    CHECK(mo_cell_get_decimal(out) == maximum);
    for (unsigned i = 0; i < 3; i++) {
        const int64_t counts[] = {1, 3, -3}, expected[] = {0, 2, -2};
        CHECK(number(MO_OP_MUL_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL,
                     mo_cell_decimal(counts[i]), mo_cell_decimal(scale / 2), &out) == MO_RETURNED);
        CHECK(mo_cell_get_decimal(out) == expected[i]);
        CHECK(number(MO_OP_DIV_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL,
                     mo_cell_decimal(counts[i]), mo_cell_decimal(2 * scale), &out) == MO_RETURNED);
        CHECK(mo_cell_get_decimal(out) == expected[i]);
    }
    const struct {
        uint32_t op, a_type, b_type;
        struct mo_cell a, b;
    } failures[] = {
        {MO_OP_ADD_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(INT64_MAX), mo_cell_int(1)},
        {MO_OP_SUB_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(INT64_MIN), mo_cell_int(1)},
        {MO_OP_MUL_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(INT64_MIN), mo_cell_int(-1)},
        {MO_OP_DIV_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(INT64_MIN), mo_cell_int(-1)},
        {MO_OP_DIV_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(1), mo_cell_int(0)},
        {MO_OP_REM_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, mo_cell_decimal(1),
         mo_cell_decimal(0)},
        {MO_OP_NEG_INT, MO_TYPE_INT, MO_TYPE_UNIT, mo_cell_int(INT64_MIN), mo_cell_unit()},
        {MO_OP_NEG_DURATION, MO_TYPE_DURATION, MO_TYPE_UNIT, mo_cell_duration(INT64_MIN),
         mo_cell_unit()},
        {MO_OP_ADD_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, mo_cell_decimal(maximum),
         mo_cell_decimal(1)},
        {MO_OP_SUB_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, mo_cell_decimal(-maximum),
         mo_cell_decimal(1)},
        {MO_OP_MUL_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, mo_cell_decimal(maximum),
         mo_cell_decimal(scale + 1)},
        {MO_OP_DIV_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, mo_cell_decimal(maximum),
         mo_cell_decimal(1)},
        {MO_OP_ADD_DURATION, MO_TYPE_DURATION, MO_TYPE_DURATION, mo_cell_duration(INT64_MAX),
         mo_cell_duration(1)},
        {MO_OP_ADD_TIME, MO_TYPE_TIME, MO_TYPE_DURATION, mo_cell_time(INT64_C(253402300799999999)),
         mo_cell_duration(1)},
    };
    for (unsigned i = 0; i < sizeof failures / sizeof failures[0]; i++) {
        out = mo_cell_int(99);
        CHECK(number(failures[i].op, failures[i].a_type, failures[i].b_type, failures[i].a_type,
                     failures[i].a, failures[i].b, &out) == MO_STOPPED);
        CHECK(mo_cell_get_int(out) == 99);
    }
    CHECK(number(MO_OP_DIV_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, mo_cell_float(1),
                 mo_cell_float(0), &out) == MO_RETURNED &&
          isinf(mo_cell_get_float(out)));
    CHECK(number(MO_OP_REM_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, MO_TYPE_FLOAT, mo_cell_float(1),
                 mo_cell_float(0), &out) == MO_RETURNED &&
          isnan(mo_cell_get_float(out)));
    CHECK(number(MO_OP_REM_INT, MO_TYPE_INT, MO_TYPE_INT, MO_TYPE_INT, mo_cell_int(INT64_MIN),
                 mo_cell_int(-1), &out) == MO_RETURNED &&
          mo_cell_get_int(out) == 0);
}

static void test_return(void) {
    const struct mo_type types[] = {{.kind = MO_TYPE_INT}};
    const struct mo_bytecode_string strings[] = {{1, (const uint8_t *)"M"}};
    const struct mo_bytecode_constant constant = {.type = 0, .value.immediate = {7}};
    const uint32_t registers[] = {0};
    const uint32_t code[] = {MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    const struct mo_bytecode_function function = {.name = 0,
                                                  .module = 0,
                                                  .result_type = 0,
                                                  .register_count = 1,
                                                  .register_types = registers,
                                                  .code_count = 5,
                                                  .code = code};
    const struct mo_bytecode_debug debug = {
        .text = MO_NO_INDEX, .file = MO_NO_INDEX, .subject = MO_NO_INDEX};
    const struct mo_bytecode_module module = {.name = 0};
    const struct mo_bytecode file = {.strings = strings,
                                     .string_count = 1,
                                     .types = types,
                                     .type_count = 1,
                                     .constants = &constant,
                                     .constant_count = 1,
                                     .functions = &function,
                                     .function_count = 1,
                                     .modules = &module,
                                     .module_count = 1,
                                     .debug = &debug,
                                     .debug_count = 1};
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_write_error write_error;
    CHECK(mo_bytecode_write(&file, &wire, &write_error));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    CHECK(image != NULL);
    mo_buffer_free(&wire);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *process = mo_process_new(image, &limits);
    CHECK(process != NULL);
    if (process != NULL) {
        struct mo_cell result = mo_cell_int(-1);
        CHECK(mo_call(process, 0, NULL, 0, &result, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_int(result) == 7);
        mo_value_drop(mo_process_heap(process), 0, result);
        mo_process_free(process);
    }
    mo_image_free(image);
}

/* Mutable writer input; all runtime observations go through the public VM API. */
struct fixture {
    struct mo_bytecode file;
    struct mo_type types[11];
    struct mo_bytecode_function functions[4];
    struct mo_bytecode_debug debug[4];
    uint32_t registers[4][16];
    struct mo_bytecode_global globals[2];
};
static void fixture_init(struct fixture *f) {
    static const struct mo_bytecode_string strings[] = {
        {1, (const uint8_t *)"M"}, {4, (const uint8_t *)"stop"},   {1, (const uint8_t *)"a"},
        {1, (const uint8_t *)"b"}, {3, (const uint8_t *)"Box"},    {1, (const uint8_t *)"A"},
        {1, (const uint8_t *)"B"}, {1, (const uint8_t *)"C"},      {1, (const uint8_t *)"D"},
        {1, (const uint8_t *)"x"}, {6, (const uint8_t *)"hidden"}, {4, (const uint8_t *)"M.mo"}};
    static const struct mo_bytecode_bytes files[] = {{4, (const uint8_t *)"M.mo"}};
    static const uint32_t pair[] = {0, 1}, integers[] = {0, 0}, text[] = {1}, integer[] = {0};
    static const struct mo_type_field fields[] = {{"a", 0}, {"b", 1}}, one_int[] = {{NULL, 0}},
                                      one_text[] = {{NULL, 1}};
    static const struct mo_type_case cases[] = {
        {.name = "A", .fields = one_int, .field_count = 1, .unnamed = true},
        {.name = "B", .fields = fields, .field_count = 2},
        {.name = "C", .fields = one_text, .field_count = 1, .unnamed = true},
        {.name = "D"}};
    static const struct mo_layout layouts[] = {
        {.kind = MO_LAYOUT_TUPLE, .type = 4, .fields = pair, .field_count = 2},
        {.kind = MO_LAYOUT_CASE, .type = 5, .case_index = 1, .fields = pair, .field_count = 2},
        {.kind = MO_LAYOUT_CLOSURE, .type = 6, .function = 3, .fields = text, .field_count = 1},
        {.kind = MO_LAYOUT_TUPLE, .type = 10, .fields = integers, .field_count = 2}};
    static const struct mo_bytecode_constant constants[] = {
        {.type = 0, .value.immediate = {7}},
        {.type = 1, .encoding = MO_CONSTANT_TEXT, .value.string = 1},
        {.type = 3},
        {.type = 0, .value.immediate = {1}},
        {.type = 0},
        {.type = 2, .value.immediate = {1}},
        {.type = 8}};
    static const struct mo_bytecode_module module = {.name = 0};
    static const uint32_t normal[] = {MO_OP_CONST, 0, 2, MO_OP_RET, 0},
                          closure[] = {MO_OP_DROP, 1, MO_OP_RET, 0};
    static const struct mo_parameter param[] = {{0, MO_PARAMETER_OWNED}};
    memset(f, 0, sizeof *f);
    f->types[0].kind = MO_TYPE_INT;
    f->types[1].kind = MO_TYPE_TEXT;
    f->types[2].kind = MO_TYPE_BOOL;
    f->types[3].kind = MO_TYPE_UNIT;
    f->types[4] = (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = pair, .parameter_count = 2};
    f->types[5] = (struct mo_type){
        .kind = MO_TYPE_CHOICE, .name = "Box", .defined = true, .cases = cases, .case_count = 4};
    f->types[6] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = integer, .parameter_count = 1, .result = 0};
    f->types[7] = (struct mo_type){.kind = MO_TYPE_ENV, .function = 3};
    f->types[8].kind = MO_TYPE_TIME;
    f->types[9] = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_CLOCK};
    f->types[10] =
        (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = integers, .parameter_count = 2};
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned j = 0; j < 16; j++)
            f->registers[i][j] = 3;
        f->functions[i] = (struct mo_bytecode_function){.name = 0,
                                                        .module = 0,
                                                        .result_type = 3,
                                                        .register_types = f->registers[i],
                                                        .register_count = 16,
                                                        .code = normal,
                                                        .code_count = 5};
        f->debug[i] = (struct mo_bytecode_debug){
            .text = MO_NO_INDEX, .file = MO_NO_INDEX, .subject = MO_NO_INDEX};
    }
    f->registers[3][0] = 0;
    f->registers[3][1] = 7;
    f->functions[3] = (struct mo_bytecode_function){.name = 0,
                                                    .module = 0,
                                                    .kind = MO_FUNCTION_CLOSURE,
                                                    .parameters = param,
                                                    .parameter_count = 1,
                                                    .result_type = 0,
                                                    .capture_types = text,
                                                    .capture_count = 1,
                                                    .register_types = f->registers[3],
                                                    .register_count = 2,
                                                    .code = closure,
                                                    .code_count = 4};
    f->file = (struct mo_bytecode){.strings = strings,
                                   .string_count = 12,
                                   .files = files,
                                   .file_count = 1,
                                   .types = f->types,
                                   .type_count = 11,
                                   .layouts = layouts,
                                   .layout_count = 4,
                                   .constants = constants,
                                   .constant_count = 7,
                                   .functions = f->functions,
                                   .function_count = 4,
                                   .debug = f->debug,
                                   .debug_count = 4,
                                   .modules = &module,
                                   .module_count = 1};
}
static void program(struct fixture *f, unsigned fn, const uint32_t *types, size_t count,
                    const uint32_t *code, size_t words, uint32_t result) {
    MO_ASSERT(count <= 16 && words <= UINT32_MAX);
    if (count != 0)
        memcpy(f->registers[fn], types, count * sizeof *types);
    f->functions[fn].register_count = (uint32_t)count;
    f->functions[fn].code = code;
    f->functions[fn].code_count = (uint32_t)words;
    f->functions[fn].result_type = result;
}
static MO_MUST_USE struct mo_image *initialized(struct fixture *f) {
    struct mo_image *image = load(&f->file);
    if (image != NULL) {
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    }
    return image;
}
static void test_primitives(void) {
    struct fixture f;
    fixture_init(&f);
    const uint32_t text_pair[] = {1, 1};
    f.types[10] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = text_pair, .parameter_count = 2, .result = 1};
    f.file.layout_count = 3;
    const struct mo_parameter param = {1, MO_PARAMETER_BORROWED};
    const uint32_t regs[] = {1, 1, 1, 1};
    const uint32_t code[] = {MO_OP_DUP,
                             1,
                             0,
                             MO_OP_DUP,
                             2,
                             0,
                             MO_OP_PRIM | (2u << 16),
                             3,
                             MO_PRIM_TEXT_CONCAT,
                             10,
                             1,
                             2,
                             MO_OP_DROP,
                             2,
                             MO_OP_RET,
                             3};
    program(&f, 0, regs, 4, code, 16, 1);
    f.functions[0].parameters = &param;
    f.functions[0].parameter_count = 1;
    const struct mo_bytecode_register_name names[] = {{1, 9, 0, 16}};
    f.debug[0].names = names;
    f.debug[0].name_count = 1;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    for (unsigned refused = 0; refused < 2; refused++) {
        struct mo_limits limits = mo_limits_default();
        if (refused != 0)
            limits.heap_size = 4096;
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_heap *heap = mo_process_heap(p);
        char bytes[2000];
        memset(bytes, 'x', sizeof bytes);
        struct mo_cell input, result = mo_cell_int(99);
        CHECK(mo_value_text(heap, bytes, sizeof bytes, &input));
        struct mo_defect defect;
        enum mo_outcome outcome = mo_call(p, 0, &input, 1, &result, &defect);
        CHECK(outcome == (refused != 0 ? MO_STOPPED : MO_RETURNED));
        if (outcome != (refused != 0 ? MO_STOPPED : MO_RETURNED) ||
            (refused != 0 && defect.limit_name == NULL)) {
            CHECK(false);
            mo_value_drop(heap, 1, input);
            mo_process_free(p);
            continue;
        }
        if (refused != 0) {
            CHECK(outcome == MO_STOPPED && mo_cell_get_int(result) == 99);
            CHECK(strcmp(defect.limit_name, "--max-heap") == 0 && defect.frames->pc == 6);
            CHECK(mo_cell_get_ref(input)->count == 3);
            struct mo_buffer report;
            mo_buffer_init(&report, 1024);
            CHECK(mo_render_defect(p, &defect, &report) && mo_buffer_append_byte(&report, 0));
            CHECK(strstr((const char *)report.bytes, "x = \"xxxx") != NULL);
            mo_buffer_free(&report);
        } else {
            CHECK(outcome == MO_RETURNED && mo_cell_get_ref(input)->count == 1);
            size_t length;
            const uint8_t *data = mo_value_data(result, &length);
            CHECK(length == 4000 && memcmp(data, bytes, sizeof bytes) == 0 &&
                  memcmp(data + sizeof bytes, bytes, sizeof bytes) == 0);
            mo_value_drop(heap, 1, result);
        }
        mo_value_drop(heap, 1, input);
        mo_process_free(p);
    }
    mo_image_free(image);
}
static void test_constants(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_type types[15];
    memcpy(types, f.types, sizeof f.types);
    types[11] = (struct mo_type){.kind = MO_TYPE_BIGINT};
    types[12] = (struct mo_type){.kind = MO_TYPE_BYTES};
    types[13] = (struct mo_type){.kind = MO_TYPE_DECIMAL};
    const uint32_t fields[] = {11, 12, 13, 0};
    types[14] = (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = fields, .parameter_count = 4};
    f.file.types = types;
    f.file.type_count = 15;
    struct mo_layout layouts[5];
    memcpy(layouts, f.file.layouts, 4 * sizeof *layouts);
    layouts[2].fields = NULL;
    layouts[2].field_count = 0;
    layouts[4] =
        (struct mo_layout){.kind = MO_LAYOUT_TUPLE, .type = 14, .fields = fields, .field_count = 4};
    f.file.layouts = layouts;
    f.file.layout_count = 5;
    f.functions[3].capture_types = NULL;
    f.functions[3].capture_count = 0;
    const uint8_t magnitude[] = {0, 1}, bytes[] = {0, 128, 255};
    const mo_i128 coefficient = ((mo_i128)1 << 100) + 17;
    struct mo_bytecode_constant constants[11];
    memcpy(constants, f.file.constants, 7 * sizeof *constants);
    constants[7] =
        (struct mo_bytecode_constant){.type = 11,
                                      .encoding = MO_CONSTANT_BIGINT,
                                      .value.bigint = {MO_BIGINT_NEGATIVE, {2, magnitude}}};
    constants[8] = (struct mo_bytecode_constant){
        .type = 12, .encoding = MO_CONSTANT_BYTES, .value.bytes = {3, bytes}};
    constants[9] = (struct mo_bytecode_constant){.type = 13, .encoding = MO_CONSTANT_DECIMAL128};
    for (unsigned i = 0; i < 16; i++)
        constants[9].value.immediate[i] = (uint8_t)((mo_u128)coefficient >> (8 * i));
    constants[10] = (struct mo_bytecode_constant){
        .type = 6, .encoding = MO_CONSTANT_CLOSURE, .value.function = 3};
    f.file.constants = constants;
    f.file.constant_count = 11;
    const uint32_t registers[] = {11, 12, 13, 6, 11, 12, 6, 0, 0, 14};
    const uint32_t code[] = {MO_OP_CONST,
                             0,
                             7,
                             MO_OP_CONST,
                             1,
                             8,
                             MO_OP_CONST,
                             2,
                             9,
                             MO_OP_CONST,
                             3,
                             10,
                             MO_OP_DUP,
                             4,
                             0,
                             MO_OP_DUP,
                             5,
                             1,
                             MO_OP_DUP,
                             6,
                             3,
                             MO_OP_CONST,
                             7,
                             0,
                             MO_OP_CALL_CLOSURE | (1u << 16),
                             8,
                             6,
                             7,
                             MO_OP_CONSTRUCT | (4u << 16),
                             9,
                             4,
                             4,
                             5,
                             2,
                             8,
                             MO_OP_RET,
                             9};
    program(&f, 0, registers, 10, code, sizeof code / sizeof code[0], 14);
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell result;
    struct mo_defect defect;
    CHECK(mo_call(p, 0, NULL, 0, &result, &defect) == MO_RETURNED);
    struct mo_cell big = mo_value_field(result, 0), data = mo_value_field(result, 1);
    CHECK(mo_value_bigint_sign(big) == MO_BIGINT_NEGATIVE);
    size_t length;
    CHECK(memcmp(mo_value_data(big, &length), magnitude, sizeof magnitude) == 0 && length == 2);
    CHECK(memcmp(mo_value_data(data, &length), bytes, sizeof bytes) == 0 && length == 3);
    CHECK(mo_cell_get_ref(big)->count == MO_IMMORTAL_COUNT &&
          mo_cell_get_ref(data)->count == MO_IMMORTAL_COUNT);
    CHECK(mo_cell_get_decimal(mo_value_field(result, 2)) == coefficient &&
          mo_cell_get_int(mo_value_field(result, 3)) == 7);
    mo_value_drop(mo_process_heap(p), 14, result);
    CHECK(mo_heap_objects(mo_process_heap(p)) == 0);
    mo_process_free(p);
    mo_image_free(image);
}
static void test_values(void) {
    struct fixture f;
    fixture_init(&f);
    const uint32_t types[] = {0, 1, 1, 4, 4, 2, 0, 0, 10};
    const uint32_t code[] = {MO_OP_CONST,
                             0,
                             0,
                             MO_OP_CONST,
                             1,
                             1,
                             MO_OP_DUP,
                             2,
                             1,
                             MO_OP_CONSTRUCT | (2u << 16),
                             3,
                             0,
                             0,
                             2,
                             MO_OP_DUP,
                             4,
                             3,
                             MO_OP_CONST,
                             0,
                             3,
                             MO_OP_SET_FIELD,
                             3,
                             3,
                             0,
                             0,
                             MO_OP_EQUAL,
                             5,
                             4,
                             3,
                             4,
                             MO_OP_FIELD,
                             6,
                             4,
                             0,
                             MO_OP_FIELD,
                             7,
                             3,
                             0,
                             MO_OP_DROP,
                             3,
                             MO_OP_DROP,
                             4,
                             MO_OP_BRANCH,
                             5,
                             46,
                             48,
                             MO_OP_STOP,
                             1,
                             MO_OP_CONSTRUCT | (2u << 16),
                             8,
                             3,
                             6,
                             7,
                             MO_OP_RET,
                             8};
    program(&f, 0, types, 9, code, sizeof code / sizeof code[0], 10);
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    for (unsigned i = 0; i < 2; i++) {
        struct mo_cell result;
        enum mo_outcome outcome = mo_call(p, 0, NULL, 0, &result, &defect);
        CHECK(outcome == MO_RETURNED);
        if (outcome != MO_RETURNED) {
            (void)fprintf(stderr, "values: %s at %u\n", defect.reason, defect.frames->pc);
            break;
        }
        CHECK(mo_cell_get_int(mo_value_field(result, 0)) == 7);
        CHECK(mo_cell_get_int(mo_value_field(result, 1)) == 1);
        mo_value_drop(mo_process_heap(p), 10, result);
        CHECK(mo_heap_objects(mo_process_heap(p)) == 0 && mo_process_stack_used(p) == 0);
    }
    mo_process_free(p);
    mo_image_free(image);

    /* All four choice shapes, switch arms, reset/reuse and token release. */
    for (uint32_t tag = 0; tag < 4; tag++) {
        fixture_init(&f);
        const uint32_t choice_types[] = {0, 1, 1, 5, 5, 0, 1, 4, 4, 4};
        uint32_t choice_code[128];
        uint32_t n = 0;
#define WORD(w)                                                                                    \
    do {                                                                                           \
        MO_ASSERT(n < 128);                                                                        \
        choice_code[n++] = (w);                                                                    \
    } while (0)
        WORD(MO_OP_CONST);
        WORD(0);
        WORD(0);
        WORD(MO_OP_CONST);
        WORD(1);
        WORD(1);
        WORD(MO_OP_DUP);
        WORD(2);
        WORD(1);
        uint32_t fields = tag == 3 ? 0 : tag == 1 ? 2 : 1;
        WORD(MO_OP_MAKE_CASE | (fields << 16));
        WORD(3);
        WORD(5);
        WORD(tag);
        if (fields != 0)
            WORD(tag == 2 ? 2 : 0);
        if (fields == 2)
            WORD(2);
        if (tag != 1 && tag != 2) {
            WORD(MO_OP_DROP);
            WORD(2);
        }
        WORD(MO_OP_MOVE);
        WORD(4);
        WORD(3);
        WORD(MO_OP_SWITCH | (4u << 16));
        WORD(4);
        uint32_t targets = n;
        n += 4;
        for (unsigned j = 0; j < 4; j++)
            choice_code[targets + j] = n;
        WORD(MO_OP_STOP);
        WORD(1);
        choice_code[targets + tag] = n;
        if (tag != 3) {
            WORD(MO_OP_CASE_FIELD);
            WORD(tag == 2 ? 6 : 5);
            WORD(4);
            WORD(tag);
            WORD(0);
        } else {
            WORD(MO_OP_CONST);
            WORD(5);
            WORD(0);
        }
        if (tag == 2) {
            WORD(MO_OP_DROP);
            WORD(6);
            WORD(MO_OP_CONST);
            WORD(5);
            WORD(0);
        }
        if (tag == 1) {
            WORD(MO_OP_RESET);
            WORD(3);
            WORD(4);
            WORD(MO_OP_DUP);
            WORD(2);
            WORD(1);
            WORD(MO_OP_REUSE | (2u << 16));
            WORD(4);
            WORD(3);
            WORD(1);
            WORD(5);
            WORD(2);
        }
        WORD(MO_OP_DROP);
        WORD(4);
        WORD(MO_OP_DUP);
        WORD(2);
        WORD(1);
        WORD(MO_OP_CONSTRUCT | (2u << 16));
        WORD(7);
        WORD(0);
        WORD(0);
        WORD(2);
        WORD(MO_OP_RESET);
        WORD(8);
        WORD(7);
        WORD(MO_OP_DUP);
        WORD(2);
        WORD(1);
        WORD(MO_OP_REUSE | (2u << 16));
        WORD(9);
        WORD(8);
        WORD(0);
        WORD(5);
        WORD(2);
        WORD(MO_OP_RESET);
        WORD(8);
        WORD(9);
        WORD(MO_OP_FREE_TOKEN);
        WORD(8);
        uint32_t done = n + 2;
        WORD(MO_OP_JUMP);
        WORD(done);
        WORD(MO_OP_RET);
        WORD(5);
#undef WORD
        program(&f, 0, choice_types, 10, choice_code, n, 0);
        image = initialized(&f);
        if (image == NULL)
            continue;
        p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell result;
        CHECK(mo_call(p, 0, NULL, 0, &result, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_int(result) == 7 && mo_heap_objects(mo_process_heap(p)) == 0);
        mo_process_free(p);
        mo_image_free(image);
    }
}
static void test_calls(void) {
    struct fixture f;
    fixture_init(&f);
    const struct mo_parameter params[] = {{0, MO_PARAMETER_OWNED}, {0, MO_PARAMETER_OWNED}};
    const uint32_t types[] = {0, 0, 0, 2, 0};
    const uint32_t loop[] = {MO_OP_CONST,
                             2,
                             4,
                             MO_OP_EQ_INT,
                             3,
                             0,
                             2,
                             MO_OP_BRANCH,
                             3,
                             11,
                             13,
                             MO_OP_RET,
                             1,
                             MO_OP_CONST,
                             2,
                             3,
                             MO_OP_SUB_INT,
                             0,
                             0,
                             2,
                             MO_OP_ADD_INT,
                             1,
                             1,
                             2,
                             MO_OP_TAIL_CALL | (2u << 16),
                             1,
                             0,
                             1};
    const uint32_t caller_types[] = {0, 0, 0};
    const uint32_t caller[] = {MO_OP_CALL | (2u << 16), 2, 1, 0, 1, MO_OP_RET, 2};
    program(&f, 0, caller_types, 3, caller, 7, 0);
    program(&f, 1, types, 5, loop, sizeof loop / sizeof loop[0], 0);
    f.functions[0].parameters = params;
    f.functions[0].parameter_count = 2;
    f.functions[1].parameters = params;
    f.functions[1].parameter_count = 2;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    const struct mo_cell args[] = {mo_cell_int(100000), mo_cell_int(7)};
    /* 72-byte frame header + 17 bytes/register, rounded to 8 bytes. */
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        limits.stack_size = scenario < 2 ? 160 : scenario == 2 ? 287 : 288;
        limits.call_depth = scenario == 0 || scenario == 4 ? 1 : 2;
        if (scenario == 1)
            limits.stack_size--;
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell out = mo_cell_int(99);
        uint32_t fn = scenario < 2 ? 1 : 0;
        enum mo_outcome outcome = mo_call(p, fn, args, 2, &out, &defect);
        CHECK(outcome == (scenario == 0 || scenario == 3 ? MO_RETURNED : MO_STOPPED));
        if (outcome == MO_RETURNED)
            CHECK(mo_cell_get_int(out) == 100007);
        else {
            CHECK(mo_cell_get_int(out) == 99);
            CHECK(strcmp(defect.limit_name, scenario == 4 ? "--max-depth" : "--max-stack") == 0);
        }
        mo_process_free(p);
    }
    mo_image_free(image);
    for (unsigned tail = 0; tail < 2; tail++) {
        fixture_init(&f);
        const uint32_t capture_types[] = {0, 1, 1, 6, 0};
        uint32_t code[] = {MO_OP_CONST,
                           0,
                           0,
                           MO_OP_CONST,
                           1,
                           1,
                           MO_OP_DUP,
                           2,
                           1,
                           MO_OP_MAKE_CLOSURE | (1u << 16),
                           3,
                           3,
                           2,
                           MO_OP_CALL_CLOSURE | (1u << 16),
                           4,
                           3,
                           0,
                           MO_OP_RET,
                           4};
        if (tail) {
            code[13] = MO_OP_TAIL_CALL_CLOSURE | (1u << 16);
            code[14] = 3;
            code[15] = 0;
        }
        program(&f, 0, capture_types, 5, code, tail ? 16 : 19, 0);
        /* Read a capture through env before dropping that owned environment. */
        const uint32_t env_types[] = {0, 7, 1};
        const uint32_t env_code[] = {MO_OP_FIELD, 2,          1, 0,         MO_OP_DROP,
                                     2,           MO_OP_DROP, 1, MO_OP_RET, 0};
        program(&f, 3, env_types, 3, env_code, 10, 0);
        image = initialized(&f);
        if (image == NULL)
            continue;
        limits = mo_limits_default();
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell out;
        CHECK(mo_call(p, 0, NULL, 0, &out, &defect) == MO_RETURNED && mo_cell_get_int(out) == 7);
        CHECK(mo_heap_objects(mo_process_heap(p)) == 0);
        mo_process_free(p);
        mo_image_free(image);
    }
}

static void test_initialization(void) {
    struct fixture f;
    fixture_init(&f);
    const uint32_t init[] = {1, 2};
    const uint32_t globals[] = {0, 1};
    struct mo_bytecode_module module = {.name = 0, .globals = globals, .global_count = 2};
    f.file.modules = &module;
    f.globals[0] = (struct mo_bytecode_global){.type = 4, .name = 2};
    f.globals[1] = (struct mo_bytecode_global){.type = 4, .name = 3};
    f.file.globals = f.globals;
    f.file.global_count = 2;
    f.file.init = init;
    f.file.init_count = 2;
    f.functions[1].kind = MO_FUNCTION_INIT;
    f.functions[2].kind = MO_FUNCTION_INIT;
    const uint32_t first_types[] = {0, 1, 1, 4, 3};
    const uint32_t first[] = {
        MO_OP_CONST, 0,         0,           MO_OP_CONST, 1,
        1,           MO_OP_DUP, 2,           1,           MO_OP_CONSTRUCT | (2u << 16),
        3,           0,         0,           2,           MO_OP_SET_GLOBAL,
        0,           3,         MO_OP_CONST, 4,           2,
        MO_OP_RET,   4};
    const uint32_t second_types[] = {4, 0, 1, 4, 3};
    const uint32_t second[] = {MO_OP_GLOBAL,
                               0,
                               0,
                               MO_OP_FIELD,
                               1,
                               0,
                               0,
                               MO_OP_FIELD,
                               2,
                               0,
                               1,
                               MO_OP_CONSTRUCT | (2u << 16),
                               3,
                               0,
                               1,
                               2,
                               MO_OP_SET_GLOBAL,
                               1,
                               3,
                               MO_OP_CONST,
                               4,
                               2,
                               MO_OP_RET,
                               4};
    const uint32_t result_types[] = {4, 4};
    const uint32_t result_code[] = {MO_OP_GLOBAL, 0, 1, MO_OP_DUP, 1, 0, MO_OP_RET, 1};
    program(&f, 1, first_types, 5, first, sizeof first / sizeof first[0], 3);
    program(&f, 2, second_types, 5, second, sizeof second / sizeof second[0], 3);
    program(&f, 0, result_types, 2, result_code, 8, 4);
    struct mo_image *image = load(&f.file);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_process_new(image, &limits) == NULL);
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_cell shared = mo_cell_unit();
    for (unsigned i = 0; i < 2; i++) {
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell out;
        CHECK(mo_call(p, 0, NULL, 0, &out, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_int(mo_value_field(out, 0)) == 7);
        CHECK(mo_cell_get_ref(out)->count == MO_IMMORTAL_COUNT);
        if (i != 0)
            CHECK(mo_cell_get_ref(out) == mo_cell_get_ref(shared));
        shared = out;
        mo_value_drop(mo_process_heap(p), 4, out);
        CHECK(mo_heap_objects(mo_process_heap(p)) == 0);
        mo_process_free(p);
    }
    mo_image_free(image);

    /* An indirect initializer call bypasses static dependency discovery; the
     * unset guard stops with both closure and initializer frames intact. */
    fixture_init(&f);
    module.global_count = 1;
    f.file.modules = &module;
    f.globals[0] = (struct mo_bytecode_global){.type = 0, .name = 2};
    f.file.globals = f.globals;
    f.file.global_count = 1;
    f.file.init = init;
    f.file.init_count = 1;
    f.functions[1].kind = MO_FUNCTION_INIT;
    const uint32_t indirect_types[] = {0, 1, 1, 6, 0, 3};
    const uint32_t indirect[] = {MO_OP_CONST,
                                 0,
                                 0,
                                 MO_OP_CONST,
                                 1,
                                 1,
                                 MO_OP_DUP,
                                 2,
                                 1,
                                 MO_OP_MAKE_CLOSURE | (1u << 16),
                                 3,
                                 3,
                                 2,
                                 MO_OP_CALL_CLOSURE | (1u << 16),
                                 4,
                                 3,
                                 0,
                                 MO_OP_SET_GLOBAL,
                                 0,
                                 4,
                                 MO_OP_CONST,
                                 5,
                                 2,
                                 MO_OP_RET,
                                 5};
    const uint32_t closure_types[] = {0, 7, 0};
    const uint32_t closure[] = {MO_OP_GLOBAL, 2, 0, MO_OP_DROP, 1, MO_OP_RET, 2};
    program(&f, 1, indirect_types, 6, indirect, sizeof indirect / sizeof indirect[0], 3);
    program(&f, 3, closure_types, 3, closure, 7, 0);
    const struct mo_bytecode_span caller_span = {13, 0, 20, 30}, callee_span = {0, 0, 40, 50};
    f.debug[1].spans = &caller_span;
    f.debug[1].span_count = 1;
    f.debug[3].spans = &callee_span;
    f.debug[3].span_count = 1;
    image = load(&f.file);
    if (image == NULL)
        return;
    CHECK(mo_image_init(image, &limits, &defect) == MO_STOPPED);
    CHECK(strcmp(defect.reason, "internal error: unset global") == 0);
    CHECK(defect.frames->function == 3 && defect.frames->previous->function == 1);
    CHECK(defect.span.file == 0 && defect.span.start == 40 && defect.span.end == 50);
    CHECK(defect.frames->previous->pc == 13 && defect.frames->previous->span.start == 20 &&
          defect.frames->previous->span.end == 30);
    struct mo_defect again;
    CHECK(mo_image_init(image, &limits, &again) == MO_STOPPED && again.frames == defect.frames);
    CHECK(mo_process_new(image, &limits) == NULL);
    struct mo_buffer text;
    mo_buffer_init(&text, 1024);
    CHECK(mo_render_defect(NULL, &defect, &text) && mo_buffer_append_byte(&text, 0));
    CHECK(strstr((const char *)text.bytes, "  at M (M.mo:40..50)\n  at M (M.mo:20..30)\n") != NULL);
    mo_buffer_free(&text);
    mo_image_free(image);
}
static void test_defects(void) {
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    for (unsigned kind = 0; kind < 4; kind++) {
        struct fixture f;
        fixture_init(&f);
        const uint32_t types[] = {1, 1, 1, 4, 4, 0, 9};
        uint32_t code[] = {MO_OP_CONST,
                           0,
                           1,
                           MO_OP_DUP,
                           1,
                           0,
                           MO_OP_MOVE,
                           2,
                           1,
                           MO_OP_CONST,
                           5,
                           0,
                           MO_OP_CONSTRUCT | (2u << 16),
                           3,
                           0,
                           5,
                           2,
                           MO_OP_RESET,
                           4,
                           3,
                           MO_OP_STOP,
                           0};
        if (kind == 1) {
            f.functions[0].kind = MO_FUNCTION_TEST;
            f.functions[0].spec_code = true;
            code[20] = MO_OP_FAIL;
        }
        if (kind >= 2) {
            code[20] = kind == 2 ? MO_OP_PRIM | (1u << 16) : MO_OP_HOST | (1u << 16);
            /* Separate typed hook programs below; no operand is consumed. */
            const uint32_t prim_types[] = {0, 0}, host_types[] = {8, 9, 3};
            const uint32_t prim[] = {
                MO_OP_CONST, 0, 0, MO_OP_PRIM | (1u << 16), 1, MO_PRIM_INT_ABS, 6, 0, MO_OP_RET, 1};
            const uint32_t host[] = {MO_OP_CONST, 0,
                                     6,           MO_OP_HOST | (1u << 16),
                                     1,           MO_HOST_CLOCK_FIXED,
                                     0,           MO_OP_DROP,
                                     1,           MO_OP_CONST,
                                     2,           2,
                                     MO_OP_RET,   2};
            struct mo_bytecode_constant constants[7];
            if (kind == 2) {
                memcpy(constants, f.file.constants, sizeof constants);
                memset(constants[0].value.immediate, 0, 16);
                constants[0].value.immediate[7] = 128;
                f.file.constants = constants;
                program(&f, 0, prim_types, 2, prim, 10, 0);
            } else {
                f.functions[0].kind = MO_FUNCTION_TEST;
                f.functions[0].spec_code = true;
                program(&f, 0, host_types, 3, host, 14, 3);
            }
            struct mo_image *image = initialized(&f);
            if (image == NULL)
                continue;
            struct mo_process *p = mo_process_new(image, &limits);
            MO_ASSERT(p != NULL);
            struct mo_cell out = mo_cell_int(99);
            enum mo_outcome outcome = mo_call(p, 0, NULL, 0, &out, &defect);
            if (kind == 2) {
                CHECK(outcome == MO_STOPPED);
                CHECK(strcmp(defect.reason, "overflow: Int") == 0 && mo_cell_get_int(out) == 99 &&
                      defect.frames->pc == 3);
            } else {
                CHECK(outcome == MO_RETURNED);
                mo_value_drop(mo_process_heap(p), 3, out);
                CHECK(mo_heap_objects(mo_process_heap(p)) == 0);
            }
            mo_process_free(p);
            mo_image_free(image);
            continue;
        }
        program(&f, 0, types, 7, code, 22, 3);
        const struct mo_bytecode_span spans[] = {{0, 0, 10, 20}, {20, MO_NO_INDEX, 0, 0}};
        const struct mo_bytecode_register_name names[] = {
            {0, 9, 0, 22}, {1, 10, 0, 22}, {3, 10, 0, 22}, {4, 10, 0, 22}, {6, 10, 0, 22}};
        f.debug[0].spans = spans;
        f.debug[0].span_count = 2;
        f.debug[0].names = names;
        f.debug[0].name_count = 5;
        struct mo_image *image = initialized(&f);
        if (image == NULL)
            continue;
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell out = mo_cell_int(99);
        CHECK(mo_call(p, 0, NULL, 0, &out, &defect) == (kind == 0 ? MO_STOPPED : MO_FAILED));
        CHECK(mo_cell_get_int(out) == 99 && defect.has_detail);
        CHECK(defect.span.file == MO_NO_INDEX && defect.frames->pc == 20);
        struct mo_buffer text;
        mo_buffer_init(&text, 1024);
        CHECK(mo_render_defect(p, &defect, &text));
        CHECK(mo_buffer_append_byte(&text, 0));
        CHECK(strstr((const char *)text.bytes, "x = \"stop\"\n") != NULL);
        CHECK(strstr((const char *)text.bytes, "hidden") == NULL);
        CHECK(strstr((const char *)text.bytes, "M.mo") == NULL);
        mo_buffer_free(&text);
        mo_buffer_init(&text, 4);
        CHECK(mo_buffer_append_text(&text, "kept"));
        CHECK(!mo_render_defect(p, &defect, &text) && text.length == 4 &&
              memcmp(text.bytes, "kept", 4) == 0);
        mo_buffer_free(&text);
        mo_process_free(p);
        mo_image_free(image);
    }
    /* Failure to allocate a constructor leaves its owned fields readable.
     * Fill the heap through the public builder, without internal accounting. */
    struct fixture f;
    fixture_init(&f);
    struct mo_parameter param = {1, MO_PARAMETER_BORROWED};
    const uint32_t types[] = {1, 1, 0, 4};
    const uint32_t code[] = {MO_OP_DUP, 1, 0, MO_OP_CONST, 2, 0, MO_OP_CONSTRUCT | (2u << 16), 3,
                             0,         2, 1, MO_OP_RET,   3};
    program(&f, 0, types, 4, code, 13, 4);
    f.functions[0].parameters = &param;
    f.functions[0].parameter_count = 1;
    const struct mo_bytecode_register_name names[] = {{1, 9, 0, 13}, {3, 10, 0, 13}};
    f.debug[0].names = names;
    f.debug[0].name_count = 2;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    limits.heap_size = 4096;
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    char bytes[4096];
    memset(bytes, 'a', sizeof bytes);
    size_t low = 0, high = sizeof bytes;
    struct mo_cell value;
    while (low < high) {
        size_t middle = low + (high - low + 1) / 2;
        if (mo_value_text(heap, bytes, middle, &value)) {
            mo_value_drop(heap, 1, value);
            low = middle;
        } else
            high = middle - 1;
    }
    CHECK(mo_value_text(heap, bytes, low, &value));
    struct mo_cell out = mo_cell_int(99);
    CHECK(mo_call(p, 0, &value, 1, &out, &defect) == MO_STOPPED);
    CHECK(strcmp(defect.reason, "running out of memory") == 0 &&
          strcmp(defect.limit_name, "--max-heap") == 0);
    CHECK(mo_cell_get_ref(value)->count == 2 && mo_cell_get_int(out) == 99 &&
          defect.frames->pc == 6);
    struct mo_buffer text;
    mo_buffer_init(&text, 1024);
    CHECK(mo_render_defect(p, &defect, &text) && mo_buffer_append_byte(&text, 0));
    CHECK(strstr((const char *)text.bytes, "x = \"aaaa") != NULL &&
          strstr((const char *)text.bytes, "<truncated>") != NULL);
    CHECK(strstr((const char *)text.bytes, "hidden") == NULL);
    mo_buffer_free(&text);
    mo_value_drop(heap, 1, value);
    mo_process_free(p);
    mo_image_free(image);
}

static void test_borrowing(void) {
    struct fixture f;
    fixture_init(&f);
    const struct mo_parameter owned = {1, MO_PARAMETER_OWNED},
                              borrowed = {1, MO_PARAMETER_BORROWED};
    const uint32_t types[] = {1, 1, 1};
    const uint32_t callee[] = {MO_OP_MOVE, 1, 0, MO_OP_DUP, 2, 1, MO_OP_RET, 2};
    const uint32_t caller[] = {MO_OP_CALL | (1u << 16), 1, 1, 0, MO_OP_DROP, 0, MO_OP_RET, 1};
    program(&f, 0, types, 2, caller, 8, 1);
    program(&f, 1, types, 3, callee, 8, 1);
    f.functions[0].parameters = &owned;
    f.functions[0].parameter_count = 1;
    f.functions[1].parameters = &borrowed;
    f.functions[1].parameter_count = 1;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        if (scenario == 2)
            limits.call_depth = 0;
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_heap *heap = mo_process_heap(p);
        struct mo_cell text, out;
        CHECK(mo_value_text(heap, "borrowed", 8, &text));
        struct mo_cell argument = scenario == 2 ? mo_value_dup(heap, 1, text) : text;
        uint32_t fn = scenario == 1 ? 1 : 0;
        enum mo_outcome outcome = mo_call(p, fn, &argument, 1, &out, &defect);
        CHECK(outcome == (scenario == 2 ? MO_STOPPED : MO_RETURNED));
        if (outcome == MO_RETURNED) {
            CHECK(mo_cell_get_ref(out)->count == (scenario == 1 ? 2u : 1u));
            size_t length;
            CHECK(memcmp(mo_value_data(out, &length), "borrowed", 8) == 0 && length == 8);
            mo_value_drop(heap, 1, out);
        } else
            CHECK(defect.frames == NULL && mo_cell_get_ref(text)->count == 1);
        if (scenario != 0)
            mo_value_drop(heap, 1, text);
        CHECK(mo_heap_objects(heap) == 0);
        mo_process_free(p);
    }
    mo_image_free(image);
}

static void test_scratch_and_fuel(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_type types[12];
    memcpy(types, f.types, sizeof f.types);
    uint32_t fields[17] = {0};
    types[11] =
        (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = fields, .parameter_count = 17};
    struct mo_layout layouts[5];
    memcpy(layouts, f.file.layouts, 4 * sizeof *layouts);
    layouts[4] = (struct mo_layout){
        .kind = MO_LAYOUT_TUPLE, .type = 11, .fields = fields, .field_count = 17};
    f.file.types = types;
    f.file.type_count = 12;
    f.file.layouts = layouts;
    f.file.layout_count = 5;
    const uint32_t registers[] = {0, 11};
    uint32_t code[25] = {MO_OP_CONST, 0, 0, MO_OP_CONSTRUCT | (17u << 16), 1, 4};
    code[23] = MO_OP_RET;
    code[24] = 1;
    program(&f, 0, registers, 2, code, 25, 11);
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    /* Refuse the constructor with scratch available, then refuse its gather
     * scratch itself. Both leave the register and destination untouched.
     * Find the process minimum through its public API: host metadata is also
     * charged, and this test bounds execution space, not a private struct. */
    size_t minimum = 0;
    struct mo_limits probe_limits = limits;
    struct mo_process *probe;
    do {
        probe_limits.heap_size = minimum++;
        probe = mo_process_new(image, &probe_limits);
    } while (probe == NULL && minimum < 4096);
    MO_ASSERT(probe != NULL);
    mo_process_free(probe);
    minimum--;
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        if (scenario != 0)
            limits.heap_size = minimum + (scenario == 1 ? 400u : 0u);
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_heap *heap = mo_process_heap(p);
        size_t baseline = mo_heap_used(heap);
        struct mo_cell out = mo_cell_int(99);
        enum mo_outcome outcome = mo_call(p, 0, NULL, 0, &out, &defect);
        CHECK(outcome == (scenario == 0 ? MO_RETURNED : MO_STOPPED));
        if (outcome == MO_RETURNED) {
            for (uint32_t i = 0; i < 17; i++)
                CHECK(mo_cell_get_int(mo_value_field(out, i)) == 7);
            mo_value_drop(heap, 11, out);
        } else
            CHECK(defect.frames->pc == 3 && mo_cell_get_int(out) == 99);
        CHECK(mo_heap_objects(heap) == 0 && mo_heap_used(heap) == baseline);
        mo_process_free(p);
    }
    mo_image_free(image);
    fixture_init(&f);
    const uint32_t loop[] = {MO_OP_JUMP, 0};
    program(&f, 0, NULL, 0, loop, 2, 3);
    image = initialized(&f);
    if (image == NULL)
        return;
    limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    mo_process_instruction_limit(p, 10);
    struct mo_cell out = mo_cell_int(99);
    CHECK(mo_call(p, 0, NULL, 0, &out, &defect) == MO_STOPPED);
    CHECK(strcmp(defect.reason, "internal error: instruction budget exhausted") == 0 &&
          mo_cell_get_int(out) == 99);
    struct mo_buffer text;
    mo_buffer_init(&text, 1024);
    CHECK(mo_render_defect(p, &defect, &text));
    mo_buffer_free(&text);
    mo_process_free(p);
    mo_image_free(image);

    const uint32_t init = 0;
    const uint32_t init_types[] = {2, 3};
    const uint32_t init_code[] = {MO_OP_CONST,      0, 5, MO_OP_BRANCH, 0, 3, 7, MO_OP_CONST, 1, 2,
                                  MO_OP_SET_GLOBAL, 0, 1, MO_OP_RET,    1};
    program(&f, 0, init_types, 2, init_code, 15, 3);
    f.functions[0].kind = MO_FUNCTION_INIT;
    f.file.init = &init;
    f.file.init_count = 1;
    f.globals[0] = (struct mo_bytecode_global){.type = 3, .name = 2};
    f.file.globals = f.globals;
    f.file.global_count = 1;
    const struct mo_bytecode_module module = {.name = 0, .globals = &init, .global_count = 1};
    f.file.modules = &module;
    image = load(&f.file);
    if (image == NULL)
        return;
    CHECK(mo_image_init_bounded(image, &limits, 10, &defect) == MO_STOPPED);
    CHECK(strcmp(defect.reason, "internal error: instruction budget exhausted") == 0 &&
          defect.frames->function == 0 && defect.frames->pc == 3);
    CHECK(mo_process_new(image, &limits) == NULL);
    mo_image_free(image);
}

static MO_MUST_USE struct mo_image *assemble(const char *source) {
    struct mo_buffer wire;
    struct mo_assemble_error error;
    struct mo_load_error load_error;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    bool ok = mo_bytecode_assemble(source, strlen(source), &wire, &error);
    CHECK(ok);
    if (!ok)
        CHECK(fprintf(stderr, "assembly: %s at %u\n", error.reason, error.offset) > 0);
    struct mo_image *image = ok ? mo_load(wire.bytes, wire.length, &load_error) : NULL;
    CHECK(image != NULL);
    mo_buffer_free(&wire);
    if (image != NULL && corpus != NULL) {
        struct mo_image *copy = load(mo_image_bytecode(image));
        mo_image_free(image);
        image = copy;
    }
    return image;
}

static void test_clear(void) {
    struct mo_image *image =
        assemble("bytecode 0.2 shorthand\nmodule M exposes [M.clear]\n"
                 "function M.clear(x: Text borrowed, n: Int owned) : Unit function in M\n"
                 " registers [r2: Text, r3: Int, r4: Text, r5: Text]\n"
                 " variables [(r0, \"cleared\", @start, @end),"
                 " (r1, \"cleared\", @start, @end), (r2, \"cleared\", @start, @end),"
                 " (r3, \"cleared\", @start, @end), (r4, \"cleared\", @start, @end),"
                 " (r5, \"reason\", @start, @end)]\n"
                 " @start:\n move r2, r0\n clear r2\n clear r1\n dup r4, r0\n"
                 " drop r4\n clear r4\n clear r3\n const r3, 17\n clear r3\n"
                 " clear r0\n const r5, \"clear stop\"\n stop r5\n @end:\nend\n");
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    struct mo_cell args[] = {mo_cell_unit(), mo_cell_int(-19)}, result;
    MO_ASSERT(mo_value_text(heap, "borrowed", 8, &args[0]));
    size_t used = mo_heap_used(heap);
    CHECK(mo_call(p, 0, args, 2, &result, &defect) == MO_STOPPED);
    CHECK(strcmp(defect.reason, "stop") == 0);
    CHECK(mo_heap_used(heap) == used && mo_cell_get_ref(args[0])->count == 1);
    bool readable[6];
    struct mo_load_error error;
    CHECK(mo_image_registers_readable(image, 0, defect.frames->pc, readable, &error));
    for (unsigned i = 0; i < 6; i++)
        CHECK(readable[i] == (i == 5));
    struct mo_buffer report;
    mo_buffer_init(&report, 1024);
    CHECK(mo_render_defect(p, &defect, &report) && mo_buffer_append_byte(&report, 0));
    CHECK(strstr((const char *)report.bytes, "cleared") == NULL);
    CHECK(strstr((const char *)report.bytes, "  reason = \"clear stop\"\n") != NULL);
    mo_buffer_free(&report);
    mo_value_drop(heap, mo_image_bytecode(image)->functions[0].register_types[0], args[0]);
    mo_process_free(p);
    mo_image_free(image);
}

static void test_bounded_report(void) {
    struct mo_image *image =
        assemble("bytecode 0.2 shorthand\nmodule M exposes [M.loop]\n"
                 "function M.loop() : Unit function in M\n registers [r0: Unit]\n"
                 " call r0, M.loop\n ret r0\nend\n");
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell out;
    CHECK(mo_call(p, 0, NULL, 0, &out, &defect) == MO_STOPPED);
    for (unsigned size = 0; size < 2; size++) {
        struct mo_buffer text;
        mo_buffer_init(&text, size == 0 ? 65536 : 256);
        CHECK(mo_render_defect(p, &defect, &text));
        CHECK(mo_buffer_append_byte(&text, 0));
        CHECK(strstr((const char *)text.bytes,
                     "out of stack (--max-depth = 10000)\n  at M.loop\n") != NULL);
        CHECK(strstr((const char *)text.bytes, size == 0 ? "9992 frames" : "<truncated>") != NULL);
        mo_buffer_free(&text);
    }
    mo_process_free(p);
    mo_image_free(image);

    struct fixture f;
    fixture_init(&f);
    struct mo_parameter parameter = {1, MO_PARAMETER_BORROWED};
    const uint32_t type = 1, stop[] = {MO_OP_STOP, 0};
    program(&f, 0, &type, 1, stop, 2, 3);
    f.functions[0].parameters = &parameter;
    f.functions[0].parameter_count = 1;
    const struct mo_bytecode_register_name name = {0, 9, 0, 2};
    f.debug[0].names = &name;
    f.debug[0].name_count = 1;
    struct mo_bytecode_string strings[12];
    memcpy(strings, f.file.strings, sizeof strings);
    uint8_t function_name[1000], file_name[1000];
    memset(function_name, 'N', sizeof function_name);
    memset(file_name, 'F', sizeof file_name);
    strings[0] = (struct mo_bytecode_string){sizeof function_name, function_name};
    const struct mo_bytecode_bytes path = {sizeof file_name, file_name};
    f.file.files = &path;
    f.file.file_count = 1;
    f.file.strings = strings;
    const struct mo_bytecode_span span = {.file = 0, .start = 7, .end = 19};
    f.debug[0].spans = &span;
    f.debug[0].span_count = 1;
    image = initialized(&f);
    if (image == NULL)
        return;
    p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    char message[100000];
    memset(message, 'a', sizeof message);
    memcpy(message, "\n  at forged\033\0", 15);
    struct mo_cell detail;
    CHECK(mo_value_text(mo_process_heap(p), message, sizeof message, &detail));
    CHECK(mo_call(p, 0, &detail, 1, &out, &defect) == MO_STOPPED);
    struct mo_buffer text;
    mo_buffer_init(&text, SIZE_MAX);
    CHECK(mo_render_defect(p, &defect, &text));
    CHECK(text.length < MO_LIMIT_DEFECT_TEXT_SIZE && mo_buffer_append_byte(&text, 0));
    CHECK(strstr((const char *)text.bytes, "\n  at forged") == NULL);
    CHECK(strstr((const char *)text.bytes, "\\n  at forged\\u{1b}\\u{0}") != NULL);
    CHECK(strstr((const char *)text.bytes, "x = \"\\n") != NULL);
    CHECK(strstr((const char *)text.bytes, "<truncated>") != NULL);
    mo_buffer_free(&text);
    mo_buffer_init(&text, 256);
    CHECK(mo_render_defect(p, &defect, &text));
    char small[257];
    MO_ASSERT(text.length < sizeof small);
    memcpy(small, text.bytes, text.length);
    small[text.length] = '\0';
    CHECK(strstr(small, "stop\n  at N") != NULL && strstr(small, " (F") != NULL &&
          strstr(small, ":7..19)\n") != NULL && strstr(small, "<truncated>") != NULL);
    mo_buffer_free(&text);
    mo_value_drop(mo_process_heap(p), 1, detail);
    mo_process_free(p);
    mo_image_free(image);
}

static void test_report_buffers(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_parameter parameter = {1, MO_PARAMETER_BORROWED};
    const uint32_t type = 1, stop[] = {MO_OP_STOP, 0};
    program(&f, 0, &type, 1, stop, 2, 3);
    f.functions[0].parameters = &parameter;
    f.functions[0].parameter_count = 1;
    const struct mo_bytecode_register_name name = {0, 9, 0, 2};
    f.debug[0].names = &name;
    f.debug[0].name_count = 1;
    struct mo_bytecode_string strings[12];
    memcpy(strings, f.file.strings, sizeof strings);
    strings[0] = (struct mo_bytecode_string){3, (const uint8_t *)"M.f"};
    const struct mo_bytecode_bytes path = {4, (const uint8_t *)"m.mo"};
    f.file.files = &path;
    f.file.file_count = 1;
    f.file.strings = strings;
    const struct mo_bytecode_span span = {.file = 0, .start = 1, .end = 2};
    f.debug[0].spans = &span;
    f.debug[0].span_count = 1;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    char bytes[5000];
    memset(bytes, 'a', sizeof bytes);
    memcpy(bytes, "detail! ", 8);
    struct mo_cell detail, result;
    MO_ASSERT(mo_value_text(mo_process_heap(p), bytes, sizeof bytes, &detail));
    struct mo_defect defect;
    CHECK(mo_call(p, 0, &detail, 1, &result, &defect) == MO_STOPPED);
    const char *head = "stop\n  at M.f (m.mo:1..2)\n";
    for (size_t size = 0; size <= 6144; size++) {
        struct mo_buffer text;
        mo_buffer_init(&text, size);
        bool rendered = mo_render_defect(p, &defect, &text);
        CHECK(text.length <= size);
        CHECK(rendered == (size >= strlen(head)));
        char report[6145];
        if (text.length != 0)
            memcpy(report, text.bytes, text.length);
        report[text.length] = '\0';
        if (size >= 5)
            CHECK(strncmp(report, "stop\n", 5) == 0);
        if (size >= strlen(head))
            CHECK(strncmp(report, head, strlen(head)) == 0);
        if (size >= 80) {
            CHECK(strstr(report, "\n  detail: detail! ") != NULL);
            CHECK(strstr(report, "<truncated>") != NULL);
        }
        if (size >= 5000)
            CHECK(strstr(report, "\n  x = \"") != NULL);
        mo_buffer_free(&text);
    }
    mo_value_drop(mo_process_heap(p), 1, detail);
    mo_process_free(p);
    mo_image_free(image);
}

static void test_report_chars(void) {
    struct fixture f;
    fixture_init(&f);
    struct mo_type types[12];
    memcpy(types, f.types, sizeof f.types);
    types[11] = (struct mo_type){.kind = MO_TYPE_CHAR};
    f.file.types = types;
    f.file.type_count = 12;
    struct mo_parameter parameters[] = {{11, MO_PARAMETER_OWNED}, {1, MO_PARAMETER_BORROWED}};
    const uint32_t registers[] = {11, 1}, stop[] = {MO_OP_STOP, 1};
    program(&f, 0, registers, 2, stop, 2, 3);
    f.functions[0].parameters = parameters;
    f.functions[0].parameter_count = 2;
    const struct mo_bytecode_register_name name = {0, 9, 0, 2};
    f.debug[0].names = &name;
    f.debug[0].name_count = 1;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    const uint32_t scalars[] = {0x1b, '\n', 0x9b, '\''};
    const char *expected[] = {"  x = '\\u{1b}'\n", "  x = '\\n'\n", "  x = '\\u{9b}'\n",
                              "  x = '\\\''\n"};
    for (unsigned i = 0; i < 4; i++) {
        struct mo_limits limits = mo_limits_default();
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell args[] = {mo_cell_char(scalars[i]), mo_cell_unit()}, result;
        MO_ASSERT(mo_value_text(mo_process_heap(p), "reason", 6, &args[1]));
        struct mo_defect defect;
        CHECK(mo_call(p, 0, args, 2, &result, &defect) == MO_STOPPED);
        struct mo_buffer report;
        mo_buffer_init(&report, MO_LIMIT_DEFECT_TEXT_SIZE);
        CHECK(mo_render_defect(p, &defect, &report));
        MO_ASSERT(mo_buffer_append_byte(&report, 0));
        CHECK(strstr((const char *)report.bytes, expected[i]) != NULL);
        CHECK(memchr(report.bytes, 0x1b, report.length) == NULL);
        mo_buffer_free(&report);
        mo_value_drop(mo_process_heap(p), 1, args[1]);
        mo_process_free(p);
    }
    mo_image_free(image);
}

static void test_report_names(void) {
    struct fixture f;
    fixture_init(&f);
    const char *forged = "\n  at forged (x.mo:1..2)\033";
    struct mo_bytecode_string strings[15];
    memcpy(strings, f.file.strings, 12 * sizeof *strings);
    strings[12] = (struct mo_bytecode_string){(uint32_t)strlen(forged), (const uint8_t *)forged};
    strings[13] = (struct mo_bytecode_string){8, (const uint8_t *)"a\nforged"};
    strings[14] = (struct mo_bytecode_string){2, (const uint8_t *)"\xc2\x9b"};
    f.file.strings = strings;
    f.file.string_count = 15;
    struct mo_type types[12];
    memcpy(types, f.types, sizeof f.types);
    struct mo_type_case cases[4];
    memcpy(cases, types[5].cases, sizeof cases);
    struct mo_type_field case_fields[] = {{"a\nforged", 0}, {"b", 1}};
    cases[1].name = forged;
    cases[1].fields = case_fields;
    types[5].cases = cases;
    struct mo_type_field field = {"\xc2\x9b", 1};
    types[11] = (struct mo_type){.kind = MO_TYPE_RECORD,
                                 .name = forged,
                                 .defined = true,
                                 .fields = &field,
                                 .field_count = 1};
    f.file.types = types;
    f.file.type_count = 12;
    struct mo_layout layouts[5];
    memcpy(layouts, f.file.layouts, 4 * sizeof *layouts);
    uint32_t text_type = 1;
    layouts[4] = (struct mo_layout){
        .kind = MO_LAYOUT_RECORD, .type = 11, .fields = &text_type, .field_count = 1};
    f.file.layouts = layouts;
    f.file.layout_count = 5;
    struct mo_parameter parameters[] = {
        {5, MO_PARAMETER_BORROWED}, {11, MO_PARAMETER_BORROWED}, {1, MO_PARAMETER_BORROWED}};
    const uint32_t registers[] = {5, 11, 1}, stop[] = {MO_OP_STOP, 2};
    program(&f, 0, registers, 3, stop, 2, 3);
    f.functions[0].parameters = parameters;
    f.functions[0].parameter_count = 3;
    const struct mo_bytecode_register_name names[] = {{0, 9, 0, 2}, {1, 10, 0, 2}};
    f.debug[0].names = names;
    f.debug[0].name_count = 2;
    struct mo_image *image = initialized(&f);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_heap *heap = mo_process_heap(p);
    struct mo_cell text, args[3], result;
    MO_ASSERT(mo_value_text(heap, "\xc2\x9b", 2, &text));
    struct mo_cell fields[] = {mo_cell_int(7), text};
    MO_ASSERT(mo_value_case(heap, 5, 1, fields, 2, &args[0]));
    MO_ASSERT(mo_value_construct(heap, 4, &text, 1, &args[1]));
    MO_ASSERT(mo_value_text(heap, "reason", 6, &args[2]));
    struct mo_defect defect;
    CHECK(mo_call(p, 0, args, 3, &result, &defect) == MO_STOPPED);
    struct mo_buffer report;
    mo_buffer_init(&report, MO_LIMIT_DEFECT_TEXT_SIZE);
    CHECK(mo_render_defect(p, &defect, &report));
    MO_ASSERT(mo_buffer_append_byte(&report, 0));
    CHECK(strstr((const char *)report.bytes, "\n  at forged") == NULL);
    CHECK(memchr(report.bytes, 0x1b, report.length) == NULL);
    CHECK(strstr((const char *)report.bytes, "\xc2\x9b") == NULL);
    CHECK(strstr((const char *)report.bytes, "\\n  at forged (x.mo:1..2)\\u{1b}") != NULL);
    CHECK(strstr((const char *)report.bytes, "a\\nforged: 7, b: \"\\u{9b}\"") != NULL);
    CHECK(strstr((const char *)report.bytes, "\\u{9b}: \"\\u{9b}\"") != NULL);
    mo_buffer_free(&report);
    mo_value_drop(heap, 1, text);
    for (unsigned i = 0; i < 3; i++)
        mo_value_drop(heap, registers[i], args[i]);
    mo_process_free(p);
    mo_image_free(image);
}

static void test_tail_growth_and_later_stop(void) {
    struct mo_image *image =
        assemble("bytecode 0.2 shorthand\nmodule M exposes [M.start]\n"
                 "function M.start(x: Int owned) : Int function in M\n registers []\n"
                 " tail_call M.larger, r0\nend\n"
                 "function M.larger(x: Int owned) : Int function in M\n"
                 " registers [r1: Int, r2: Int, r3: Int, r4: Int, r5: Int, r6: Int, r7: Int]\n"
                 " ret r0\nend\n");
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    limits.call_depth = 1;
    for (unsigned boundary = 0; boundary < 2; boundary++) {
        limits.stack_size = boundary == 0 ? 207 : 208;
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell input = mo_cell_int(37), out = mo_cell_int(99);
        enum mo_outcome outcome = mo_call(p, 0, &input, 1, &out, &defect);
        CHECK(outcome == (boundary == 0 ? MO_STOPPED : MO_RETURNED));
        if (boundary == 0)
            CHECK(strcmp(defect.limit_name, "--max-stack") == 0 && defect.frames->function == 0 &&
                  defect.frames->pc == 0 && defect.frames->previous == NULL);
        else
            CHECK(mo_cell_get_int(out) == 37 && mo_process_stack_used(p) == 0);
        mo_process_free(p);
    }
    mo_image_free(image);
    image = assemble(
        "bytecode 0.2 shorthand\nmodule M exposes [M.later]\n"
        "function M.later(ok: Bool owned) : Unit function in M\n registers [r1: Unit, r2: Text]\n"
        " branch r0, @ok, @bad\n@ok:\n const r1, ()\n ret r1\n"
        "@bad:\n const r2, \"later\"\n stop r2\nend\n");
    if (image == NULL)
        return;
    limits = mo_limits_default();
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    struct mo_cell input = mo_cell_bool(true), out;
    CHECK(mo_call(p, 0, &input, 1, &out, &defect) == MO_RETURNED);
    input = mo_cell_bool(false);
    CHECK(mo_call(p, 0, &input, 1, &out, &defect) == MO_STOPPED);
    size_t length;
    CHECK(memcmp(mo_value_data(defect.detail, &length), "later", 5) == 0 && length == 5);
    CHECK(defect.frames->previous == NULL);
    mo_process_free(p);
    mo_image_free(image);
}

/* Fill the remaining heap exactly, rather than pinning private object sizes. */
static MO_MUST_USE struct mo_cell fill(struct mo_heap *heap) {
    char bytes[8192];
    memset(bytes, 'f', sizeof bytes);
    size_t low = 0, high = sizeof bytes;
    struct mo_cell value;
    while (low < high) {
        size_t middle = low + (high - low + 1) / 2;
        if (mo_value_text(heap, bytes, middle, &value)) {
            mo_value_drop(heap, 1, value);
            low = middle;
        } else
            high = middle - 1;
    }
    MO_ASSERT(mo_value_text(heap, bytes, low, &value));
    return value;
}

static void test_allocation_stops(void) {
    /* Boxed case, captured closure, shared reset's empty token, COW field
     * update and nonidentical compound equality. Every stop is transactional. */
    for (unsigned operation = 0; operation < 5; operation++) {
        struct fixture f;
        fixture_init(&f);
        const struct mo_parameter parameter = {4, MO_PARAMETER_BORROWED};
        const struct mo_parameter params[] = {{0, MO_PARAMETER_OWNED}, {1, MO_PARAMETER_BORROWED}};
        const uint32_t constructors[] = {0, 1, 1, 5, 6};
        const uint32_t boxed[] = {MO_OP_DUP, 2,         1, MO_OP_MAKE_CASE | (2u << 16), 3, 5, 1, 0,
                                  2,         MO_OP_RET, 3};
        const uint32_t closure[] = {MO_OP_DUP, 2,         1, MO_OP_MAKE_CLOSURE | (1u << 16), 4, 3,
                                    2,         MO_OP_RET, 4};
        const uint32_t registers[] = {4, 4, 4, 0, 1, 2};
        const uint32_t reuse[] = {MO_OP_DUP,
                                  1,
                                  0,
                                  MO_OP_RESET,
                                  2,
                                  1,
                                  MO_OP_FIELD,
                                  3,
                                  0,
                                  0,
                                  MO_OP_FIELD,
                                  4,
                                  0,
                                  1,
                                  MO_OP_REUSE | (2u << 16),
                                  1,
                                  2,
                                  0,
                                  3,
                                  4,
                                  MO_OP_RET,
                                  1};
        const uint32_t update[] = {MO_OP_DUP,       1, 0, MO_OP_FIELD, 3, 0,         0,
                                   MO_OP_SET_FIELD, 1, 1, 0,           3, MO_OP_RET, 1};
        const uint32_t equals[] = {MO_OP_EQUAL, 5, 4, 0, 1, MO_OP_RET, 5};
        const struct mo_parameter pairs[] = {{4, MO_PARAMETER_BORROWED},
                                             {4, MO_PARAMETER_BORROWED}};
        const uint32_t *code = operation == 0   ? boxed
                               : operation == 1 ? closure
                               : operation == 2 ? reuse
                               : operation == 3 ? update
                                                : equals;
        uint32_t words = operation == 0   ? 11
                         : operation == 1 ? 9
                         : operation == 2 ? 22
                         : operation == 3 ? 14
                                          : 7;
        program(&f, 0, operation < 2 ? constructors : registers, operation < 2 ? 5 : 6, code, words,
                operation == 0   ? 5
                : operation == 1 ? 6
                : operation == 4 ? 2
                                 : 4);
        f.functions[0].parameters = operation < 2 ? params : operation == 4 ? pairs : &parameter;
        f.functions[0].parameter_count = operation < 2 || operation == 4 ? 2 : 1;
        struct mo_image *image = initialized(&f);
        if (image == NULL)
            continue;
        struct mo_limits limits = mo_limits_default();
        limits.heap_size = 8192;
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_heap *heap = mo_process_heap(p);
        struct mo_cell text, args[2];
        CHECK(mo_value_text(heap, "original", 8, &text));
        if (operation < 2) {
            args[0] = mo_cell_int(17);
            args[1] = text;
        } else {
            const struct mo_cell fields[] = {mo_cell_int(17), text};
            CHECK(mo_value_construct(heap, 0, fields, 2, &args[0]));
            if (operation == 4)
                CHECK(mo_value_construct(heap, 0, fields, 2, &args[1]));
        }
        struct mo_cell filler = fill(heap), out = mo_cell_int(99);
        struct mo_defect defect;
        CHECK(mo_call(p, 0, args, f.functions[0].parameter_count, &out, &defect) == MO_STOPPED);
        CHECK(strcmp(defect.reason, "running out of memory") == 0 &&
              strcmp(defect.limit_name, "--max-heap") == 0);
        CHECK(defect.frames->pc == (operation < 2    ? 3u
                                    : operation == 2 ? 14u
                                    : operation == 3 ? 7u
                                                     : 0u));
        CHECK(mo_cell_get_int(out) == 99);
        if (operation >= 2) {
            CHECK(mo_cell_get_int(mo_value_field(args[0], 0)) == 17);
            mo_value_drop(heap, 4, args[0]);
            if (operation == 4)
                mo_value_drop(heap, 4, args[1]);
        }
        mo_value_drop(heap, 1, text);
        mo_value_drop(heap, 1, filler);
        mo_process_free(p);
        mo_image_free(image);
    }
}

static void test_initialization_memory(void) {
    struct fixture f;
    fixture_init(&f);
    const uint32_t init = 0, registers[] = {0, 0, 10, 3};
    const uint32_t code[] = {MO_OP_CONST,
                             0,
                             0,
                             MO_OP_CONST,
                             1,
                             0,
                             MO_OP_CONSTRUCT | (2u << 16),
                             2,
                             3,
                             0,
                             1,
                             MO_OP_SET_GLOBAL,
                             0,
                             2,
                             MO_OP_CONST,
                             3,
                             2,
                             MO_OP_RET,
                             3};
    program(&f, 0, registers, 4, code, 19, 3);
    f.functions[0].kind = MO_FUNCTION_INIT;
    f.file.init = &init;
    f.file.init_count = 1;
    f.globals[0] = (struct mo_bytecode_global){.type = 10, .name = 2};
    f.file.globals = f.globals;
    f.file.global_count = 1;
    const struct mo_bytecode_module module = {.name = 0, .globals = &init, .global_count = 1};
    f.file.modules = &module;
    bool retry = false, permanent = false, freeze = false, success = false;
    for (size_t size = 1; size <= 8192; size += 16) {
        struct mo_image *image = load(&f.file);
        if (image == NULL)
            return;
        struct mo_limits small = mo_limits_default(), large = small;
        small.heap_size = size;
        struct mo_defect defect, again;
        enum mo_outcome outcome = mo_image_init(image, &small, &defect);
        if (outcome == MO_RETURNED) {
            success = true;
            mo_image_free(image);
            break;
        }
        CHECK(strcmp(defect.reason, "running out of memory") == 0 &&
              strcmp(defect.limit_name, "--max-heap") == 0);
        if (mo_image_runtime(image) == NULL) {
            retry = true;
            CHECK(mo_image_init(image, &large, &again) == MO_RETURNED);
        } else {
            permanent = true;
            if (defect.frames != NULL && defect.frames->pc == 11)
                freeze = true;
            CHECK(mo_image_init(image, &large, &again) == MO_STOPPED &&
                  again.frames == defect.frames && again.limit == size);
        }
        mo_image_free(image);
    }
    CHECK(retry && permanent && freeze && success);
}

/* Differential driver: execute compiler output, with no interpreter internals.
 * Run wrappers keep capability parameters and reconstruct ordinary arguments.
 * A staged compiler refusal is distinct from an outcome. */
static MO_MUST_USE int core_run_on_stack(const char *path, const char *test,
                                         const char *directory) {
    struct mo_sources sources;
    struct mo_source_error source_error;
    uint32_t file;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_sources_add(&sources, path, &file, &source_error));
    const struct mo_source *source = &sources.files[file];
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    struct mo_diags diags;
    MO_ASSERT(mo_diags_init(&diags));
    struct mo_core_run *run =
        directory == NULL ? NULL
                          : mo_core_text_parse(&arena, MO_CORE_TEXT_RUN,
                                               (const char *)source->bytes, source->length, &diags);
    MO_ASSERT(directory == NULL || run != NULL);
    struct mo_core *core =
        run == NULL ? mo_core_parse(&arena, (const char *)source->bytes, source->length, &diags)
                    : &run->program;
    MO_ASSERT(core != NULL && mo_core_check(core, &diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    int status = 0;
    if (!mo_compile(core, &wire, &diags)) {
        MO_ASSERT(diags.count == 1 && diags.items[0].code == MO_CODE_CODEGEN_UNSUPPORTED);
        (void)puts("unsupported");
        status = 77;
    } else {
        struct mo_load_error error;
        struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
        MO_ASSERT(image != NULL);
        uint32_t function = MO_NO_INDEX;
        for (size_t i = 0; i < core->funs.count; i++) {
            mo_core_string name = core->funs.items[i].name;
            mo_core_string target =
                test == NULL ? core->entry : (mo_core_string){test, strlen(test)};
            if (name.length == target.length &&
                memcmp(name.bytes, target.bytes, name.length) == 0) {
                function = (uint32_t)i;
                break;
            }
        }
        MO_ASSERT(function != MO_NO_INDEX);
        const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[function];
        MO_ASSERT(run != NULL || fn->parameter_count == 0);
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        enum mo_outcome outcome = mo_image_init_bounded(image, &limits, 10000, &defect);
        struct mo_process *p = outcome == MO_RETURNED ? mo_process_new(image, &limits) : NULL;
        struct mo_cell result;
        struct vm_run_context context;
        bool adapted = false;
        if (outcome == MO_RETURNED) {
            MO_ASSERT(p != NULL);
            mo_process_instruction_limit(p, 10000);
            if (run == NULL)
                outcome = mo_call(p, function, NULL, 0, &result, &defect);
            else {
                vm_run_setup(&context, &arena, p, run, fn, directory);
                adapted = true;
                struct mo_main_capabilities supplied = {context.arguments, fn->parameter_count};
                outcome = mo_call_main(p, function, NULL, NULL, context.folder, &supplied, &result,
                                       &defect);
            }
        }
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_STRING_SIZE);
        if (outcome == MO_RETURNED) {
            MO_ASSERT(puts("returned") >= 0);
            uint32_t type = mo_image_bytecode(image)->functions[function].result_type;
            if (mo_image_types(image)->items[type].kind == MO_TYPE_FLOAT) {
                char bits[32];
                uint64_t value;
                memcpy(&value, result.bytes, 8);
                int n = snprintf(bits, sizeof bits, "0x%016llx", (unsigned long long)value);
                MO_ASSERT(n == 18 && mo_buffer_append(&text, bits, (size_t)n));
            } else
                MO_ASSERT(mo_value_print(mo_process_heap(p), type, result, &text));
            mo_value_drop(mo_process_heap(p), type, result);
        } else {
            const char *kind = outcome == MO_FAILED ? "failed" : "stopped";
            if (outcome == MO_STOPPED && !defect.has_detail) {
                if (strcmp(defect.reason, "internal error: primitive dispatch unavailable") == 0)
                    kind = "needs primitive dispatch";
                else if (strcmp(defect.reason, "internal error: host dispatch unavailable") == 0)
                    kind = "needs host dispatch";
            }
            MO_ASSERT(puts(kind) >= 0);
            if (defect.has_detail) {
                size_t n;
                const uint8_t *bytes = mo_value_data(defect.detail, &n);
                MO_ASSERT(mo_buffer_append(&text, bytes, n));
            } else
                MO_ASSERT(mo_buffer_append_text(&text, defect.reason));
        }
        MO_ASSERT((text.length == 0 || fwrite(text.bytes, 1, text.length, stdout) == text.length) &&
                  putchar('\n') != EOF);
        mo_buffer_free(&text);
        if (adapted) {
            vm_run_effects(&context);
            vm_run_dispose(&context, fn);
        }
        mo_process_free(p);
        mo_image_free(image);
    }
    mo_buffer_free(&wire);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
    mo_sources_free(&sources);
    return status;
}
struct core_arguments {
    const char *path, *test, *directory;
    int status;
};
static void core_run_callback(void *argument) {
    struct core_arguments *a = argument;
    a->status = core_run_on_stack(a->path, a->test, a->directory);
}
static MO_MUST_USE int core_run(const char *path, const char *test, const char *directory) {
    struct core_arguments a = {.path = path, .test = test, .directory = directory};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, core_run_callback, &a) == 0);
    return a.status;
}

static MO_MUST_USE mo_i128 decimal_input(const char *text) {
    bool negative = text[0] == '-';
    if (negative)
        text++;
    mo_u128 value = 0, maximum = ((mo_u128)1 << 127) - 1;
    while (*text != 0) {
        MO_ASSERT(*text >= '0' && *text <= '9');
        unsigned digit = (unsigned)(*text++ - '0');
        MO_ASSERT(value <= (maximum - digit) / 10);
        value = value * 10 + digit;
    }
    return negative ? -(mo_i128)value : (mo_i128)value;
}
static MO_MUST_USE int decimal_driver(void) {
    char line[160], left[64], right[64];
    unsigned family;
    while (fgets(line, sizeof line, stdin) != NULL) {
        MO_ASSERT(sscanf(line, "%u %63s %63s", &family, left, right) == 3 && family < 5);
        struct mo_cell out;
        enum mo_outcome outcome = number(
            MO_OP_ADD_DECIMAL + 3 * family, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL, MO_TYPE_DECIMAL,
            mo_cell_decimal(decimal_input(left)), mo_cell_decimal(decimal_input(right)), &out);
        if (outcome != MO_RETURNED)
            MO_ASSERT(puts("stopped") >= 0);
        else {
            mo_u128 bits = (mo_u128)mo_cell_get_decimal(out);
            MO_ASSERT(printf("%016llx%016llx\n", (unsigned long long)(bits >> 64),
                             (unsigned long long)bits) == 33);
        }
    }
    MO_ASSERT(!ferror(stdin));
    return mo_test_failures != 0;
}

static MO_MUST_USE struct mo_cell cell_input(const char *text) {
    struct mo_cell value;
    MO_ASSERT(strlen(text) == 32);
    for (unsigned i = 0; i < 16; i++) {
        unsigned byte;
        MO_ASSERT(sscanf(text + 2 * i, "%2x", &byte) == 1);
        value.bytes[i] = (uint8_t)byte;
    }
    return value;
}

static MO_MUST_USE int operation_driver(void) {
    char line[256], left[33], right[33];
    unsigned op, a, b, result;
    while (fgets(line, sizeof line, stdin) != NULL) {
        MO_ASSERT(sscanf(line, "%u %u %u %u %32s %32s", &op, &a, &b, &result, left, right) == 6);
        struct mo_cell value;
        struct mo_buffer text;
        mo_buffer_init(&text, 256);
        const char *reason = NULL;
        enum mo_outcome outcome = number_report(op, a, b, result, cell_input(left),
                                                cell_input(right), &value, &text, &reason);
        if (outcome == MO_RETURNED) {
            MO_ASSERT(printf("returned %.*s\n", (int)text.length, (const char *)text.bytes) > 0);
        } else
            MO_ASSERT(printf("stopped %s\n", reason) > 0);
        mo_buffer_free(&text);
    }
    MO_ASSERT(!ferror(stdin));
    return mo_test_failures != 0;
}

static MO_MUST_USE int structure_driver(void) {
    char line[160];
    unsigned kind;
    long long a, b, c;
    while (fgets(line, sizeof line, stdin) != NULL) {
        MO_ASSERT(sscanf(line, "%u %lld %lld %lld", &kind, &a, &b, &c) == 4 && kind < 4);
        struct fixture f;
        fixture_init(&f);
        struct mo_type types[12];
        memcpy(types, f.types, sizeof f.types);
        const uint32_t fields[] = {10, 10, 2};
        types[11] =
            (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = fields, .parameter_count = 3};
        struct mo_layout layouts[5];
        memcpy(layouts, f.file.layouts, 4 * sizeof *layouts);
        layouts[4] = (struct mo_layout){
            .kind = MO_LAYOUT_TUPLE, .type = 11, .fields = fields, .field_count = 3};
        f.file.types = types;
        f.file.type_count = 12;
        f.file.layouts = layouts;
        f.file.layout_count = 5;
        const struct mo_parameter params[] = {
            {10, MO_PARAMETER_OWNED}, {10, MO_PARAMETER_BORROWED}, {0, MO_PARAMETER_OWNED}};
        const uint32_t registers[] = {10, 10, 0, 0, 10, 2, 11, 10};
        uint32_t code[32] = {MO_OP_DUP, 7, kind % 2 == 0 ? 1u : 0u};
        uint32_t n = 3;
        if (kind < 2) {
            const uint32_t update[] = {MO_OP_SET_FIELD, 0, 0, 0, 2};
            memcpy(code + n, update, sizeof update);
            n += 5;
        } else {
            const uint32_t rebuild[] = {
                MO_OP_FIELD, 3, 0, 1, MO_OP_RESET, 4, 0, MO_OP_REUSE | (2u << 16), 0, 4, 3, 2, 3};
            memcpy(code + n, rebuild, sizeof rebuild);
            n += 13;
        }
        const uint32_t finish[] = {
            MO_OP_EQUAL, 5, 10, 0, 1, MO_OP_CONSTRUCT | (3u << 16), 6, 4, 7, 0, 5, MO_OP_RET, 6};
        memcpy(code + n, finish, sizeof finish);
        n += 13;
        program(&f, 0, registers, 8, code, n, 11);
        f.functions[0].parameters = params;
        f.functions[0].parameter_count = 3;
        struct mo_image *image = initialized(&f);
        MO_ASSERT(image != NULL);
        struct mo_limits limits = mo_limits_default();
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_heap *heap = mo_process_heap(p);
        const struct mo_cell pair[] = {mo_cell_int((int64_t)a), mo_cell_int((int64_t)b)};
        struct mo_cell args[3], result;
        MO_ASSERT(mo_value_construct(heap, 3, pair, 2, &args[0]) &&
                  mo_value_construct(heap, 3, pair, 2, &args[1]));
        args[2] = mo_cell_int((int64_t)c);
        uint64_t hash;
        MO_ASSERT(mo_value_hash(heap, 10, args[0], &hash));
        struct mo_defect defect;
        MO_ASSERT(mo_call(p, 0, args, 3, &result, &defect) == MO_RETURNED);
        struct mo_buffer text;
        mo_buffer_init(&text, 512);
        MO_ASSERT(mo_value_print(heap, 11, result, &text));
        MO_ASSERT(printf("returned %.*s\n", (int)text.length, (const char *)text.bytes) > 0);
        mo_buffer_free(&text);
        mo_value_drop(heap, 11, result);
        mo_value_drop(heap, 10, args[1]);
        MO_ASSERT(mo_heap_objects(heap) == 0);
        mo_process_free(p);
        mo_image_free(image);
    }
    MO_ASSERT(!ferror(stdin));
    return mo_test_failures != 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--core") == 0)
        return core_run(argv[2], NULL, NULL);
    if (argc == 4 && strcmp(argv[1], "--core-test") == 0)
        return core_run(argv[2], argv[3], NULL);
    if (argc == 4 && strcmp(argv[1], "--run") == 0)
        return core_run(argv[2], NULL, argv[3]);
    if (argc == 2 && strcmp(argv[1], "--decimal") == 0)
        return decimal_driver();
    if (argc == 2 && strcmp(argv[1], "--operations") == 0)
        return operation_driver();
    if (argc == 2 && strcmp(argv[1], "--structures") == 0)
        return structure_driver();
    if (argc == 3 && strcmp(argv[1], "--corpus") == 0)
        corpus = argv[2];
    test_return();
    test_numbers();
    test_constants();
    test_primitives();
    test_values();
    test_calls();
    test_initialization();
    test_defects();
    test_borrowing();
    test_scratch_and_fuel();
    test_clear();
    test_bounded_report();
    test_report_buffers();
    test_report_chars();
    test_report_names();
    test_tail_growth_and_later_stop();
    test_allocation_stops();
    test_initialization_memory();
    return mo_test_report("vm");
}
