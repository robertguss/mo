#include "base/simops.h"
#include "bytecode/verify.h"
#include "bytecode_text.h"
#include "test.h"

#include <string.h>

#define N(a) ((uint32_t)(sizeof(a) / sizeof *(a)))
#define STR(s)                                                                                     \
    { sizeof(s) - 1, (const uint8_t *)(s) }
static const struct mo_bytecode_string strings[] = {
    STR("M"),       STR("Pair"),   STR("left"),     STR("right"),    STR("Maybe"),
    STR("None"),    STR("Some"),   STR("Ordering"), STR("Less"),     STR("Equal"),
    STR("Greater"), STR("Ok"),     STR("Error"),    STR("NotFound"), STR("NotAllowed"),
    STR("NotText"), STR("Failed"), STR("path"),     STR("reason"),   STR("File"),
    STR("Folder"),  STR("Link"),   STR("Other"),    STR("name"),     STR("kind")};
static const uint32_t pair[] = {3, 10}, list[] = {3}, int_arg[] = {3}, append_args[] = {16, 3};
static const struct mo_type_field fields[] = {{"left", 3}, {"right", 10}};
static const struct mo_type_case maybe[] = {{.name = "None"},
                                            {.name = "Some", .fields = fields, .field_count = 2}};
static const struct mo_type_case ordering[] = {
    {.name = "Less"}, {.name = "Equal"}, {.name = "Greater"}};
static const uint32_t default_code[] = {MO_OP_CONST, 0, 0, MO_OP_RET, 0};
static const uint32_t default_regs[] = {0, 3, 3, 1, 10, 10, 14, 14, 0, 15, 3, 13, 16, 17};
struct fixture {
    struct mo_bytecode file;
    struct mo_type types[32];
    struct mo_layout layouts[8];
    struct mo_bytecode_function functions[5];
    struct mo_parameter parameters[5][4];
    struct mo_bytecode_debug debug[5];
    struct mo_bytecode_constant constants[5];
    struct mo_bytecode_module module;
    struct mo_bytecode_global globals[2];
    uint32_t global_indices[2], init[2];
};
static const char *corpus;
static void seed(const struct mo_buffer *wire) {
    if (corpus == NULL) {
        return;
    }
    uint64_t hash = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = 0; i < wire->length; i++) {
        hash = (hash ^ wire->bytes[i]) * MO_BYTECODE_FNV_PRIME;
    }
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/fixture-%016llx.bc", corpus, (unsigned long long)hash);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    FILE *stream = fopen(path, "wb");
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(fwrite(wire->bytes, 1, wire->length, stream) == wire->length);
        CHECK(fclose(stream) == 0);
    }
}
static void init(struct fixture *t) {
    memset(t, 0, sizeof *t);
    for (uint32_t i = 0; i < 13; i++) {
        t->types[i].kind = (enum mo_type_kind)i;
    }
    t->types[13] =
        (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = pair, .parameter_count = 2};
    t->types[14] = (struct mo_type){.kind = MO_TYPE_RECORD,
                                    .name = "Pair",
                                    .fields = fields,
                                    .field_count = 2,
                                    .defined = true};
    t->types[15] = (struct mo_type){
        .kind = MO_TYPE_CHOICE, .name = "Maybe", .cases = maybe, .case_count = 2, .defined = true};
    t->types[16] = (struct mo_type){.kind = MO_TYPE_LIST, .parameters = list, .parameter_count = 1};
    t->types[17] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = int_arg, .parameter_count = 1, .result = 3};
    t->types[18] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = append_args, .parameter_count = 2, .result = 16};
    t->types[19] = (struct mo_type){.kind = MO_TYPE_ENV, .function = 1};
    t->types[20] = (struct mo_type){.kind = MO_TYPE_CHOICE,
                                    .name = "Ordering",
                                    .cases = ordering,
                                    .case_count = 3,
                                    .defined = true};
    t->layouts[0] =
        (struct mo_layout){.kind = MO_LAYOUT_TUPLE, .type = 13, .fields = pair, .field_count = 2};
    t->layouts[1] =
        (struct mo_layout){.kind = MO_LAYOUT_RECORD, .type = 14, .fields = pair, .field_count = 2};
    t->layouts[2] = (struct mo_layout){
        .kind = MO_LAYOUT_CASE, .type = 15, .case_index = 1, .fields = pair, .field_count = 2};
    t->constants[0] = (struct mo_bytecode_constant){.type = 0};
    t->constants[1] = (struct mo_bytecode_constant){.type = 3, .value.immediate = {7}};
    t->constants[2] = (struct mo_bytecode_constant){.type = 1, .value.immediate = {1}};
    t->constants[3] = (struct mo_bytecode_constant){.type = 10, .value.string = 0};
    for (uint32_t i = 0; i < N(t->functions); i++) {
        t->functions[i] = (struct mo_bytecode_function){.name = 0,
                                                        .module = 0,
                                                        .result_type = 0,
                                                        .code = default_code,
                                                        .code_count = N(default_code),
                                                        .register_types = default_regs,
                                                        .register_count = N(default_regs)};
        t->debug[i] = (struct mo_bytecode_debug){.text = MO_BYTECODE_NO_INDEX,
                                                 .file = MO_BYTECODE_NO_INDEX,
                                                 .subject = MO_BYTECODE_NO_INDEX};
    }
    t->module.name = 0;
    t->file = (struct mo_bytecode){.strings = strings,
                                   .string_count = N(strings),
                                   .types = t->types,
                                   .type_count = 19,
                                   .layouts = t->layouts,
                                   .layout_count = 3,
                                   .constants = t->constants,
                                   .constant_count = 4,
                                   .functions = t->functions,
                                   .function_count = 1,
                                   .modules = &t->module,
                                   .module_count = 1,
                                   .debug = t->debug,
                                   .debug_count = 1};
}
static void code(struct fixture *t, const uint32_t *words, uint32_t n) {
    t->functions[0].code = words;
    t->functions[0].code_count = n;
}
static void parameter(struct fixture *t, uint32_t function, uint32_t count,
                      enum mo_parameter_ownership ownership) {
    t->functions[function].parameters = t->parameters[function];
    t->functions[function].parameter_count = count;
    for (uint32_t i = 0; i < count; i++) {
        t->parameters[function][i] =
            (struct mo_parameter){t->functions[function].register_types[i], ownership};
    }
}
static MO_MUST_USE uint32_t position(const struct fixture *t, size_t length, uint32_t fn,
                                     uint32_t word) {
    size_t tail = 28 + (size_t)t->file.function_count * 36;
    for (uint32_t i = 0; i < t->file.file_count; i++)
        tail += 4 + t->file.files[i].length;
    tail += (size_t)t->module.global_count * 4 + (size_t)t->module.function_count * 4 +
            (size_t)t->module.type_count * 8;
    for (uint32_t i = fn + 1; i < t->file.function_count; i++) {
        const struct mo_bytecode_function *f = &t->functions[i];
        tail += 4 * (9 + (size_t)f->parameter_count * 2 + f->capture_count + f->register_count +
                     f->code_count);
    }
    tail += (size_t)t->functions[fn].code_count * 4;
    MO_ASSERT(tail <= length);
    return (uint32_t)(length - tail) + word * 4;
}
static void expect_kind(struct fixture *t, const char *reason, uint32_t fn, uint32_t word,
                        enum mo_bytecode_error_kind error_kind) {
    struct mo_buffer wire, output;
    struct mo_write_error we;
    struct mo_load_error le;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&output, MO_LIMIT_BYTECODE_SIZE);
    bool encoded = mo_bytecode_encode(&t->file, &wire, &we);
    CHECK(encoded);
    if (!encoded) {
        CHECK(fprintf(stderr, "structural fixture refused: %s\n", we.reason) >= 0);
        goto done;
    }
    seed(&wire);
    struct mo_image *image = mo_load(wire.bytes, wire.length, &le);
    CHECK((image == NULL) == (reason != NULL));
    if (image == NULL && reason == NULL) {
        CHECK(fprintf(stderr, "accepted fixture refused: %s at %u\n", le.reason, le.offset) >= 0);
    }
    if (reason != NULL && image == NULL) {
        if (strcmp(le.reason, reason) != 0) {
            CHECK(fprintf(stderr, "expected %s, got %s\n", reason, le.reason) >= 0);
        }
        CHECK(strcmp(le.reason, reason) == 0);
        CHECK(le.kind == error_kind);
        CHECK(le.offset == (fn == MO_NO_INDEX ? word : position(t, wire.length, fn, word)));
        CHECK(!mo_bytecode_write(&t->file, &output, &we));
        CHECK(output.length == 0 && strcmp(we.reason, reason) == 0);
        CHECK(we.kind == error_kind);
    } else if (image != NULL) {
        text_roundtrip(&wire);
        CHECK(mo_bytecode_write(mo_image_bytecode(image), &output, &we));
        CHECK(output.length == wire.length && memcmp(output.bytes, wire.bytes, wire.length) == 0);
        memset(wire.bytes, 0xa5, wire.length); /* image must not borrow any input */
        mo_buffer_clear(&wire);
        CHECK(mo_bytecode_write(mo_image_bytecode(image), &wire, &we));
        CHECK(output.length == wire.length && memcmp(output.bytes, wire.bytes, wire.length) == 0);
    }
    if (image != NULL) {
        mo_image_free(image);
    }
done:
    mo_buffer_free(&output);
    mo_buffer_free(&wire);
}
static void expect(struct fixture *t, const char *reason, uint32_t fn, uint32_t word) {
    expect_kind(t, reason, fn, word, MO_BYTECODE_INVALID);
}

static void basics(void) {
    struct fixture t;
    init(&t);
    expect(&t, NULL, 0, 0);
    const uint32_t clear[] = {MO_OP_CLEAR, 0, MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    code(&t, clear, N(clear));
    expect(&t, NULL, 0, 0);
    const uint32_t undefined[] = {MO_OP_RET, 0};
    code(&t, undefined, N(undefined));
    expect(&t, "register is not readable", 0, 1);
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    expect(&t, NULL, 0, 0);
    parameter(&t, 0, 0, MO_PARAMETER_OWNED);
    const uint32_t fall[] = {MO_OP_CONST, 0, 0};
    code(&t, fall, N(fall));
    expect(&t, "function falls off code", 0, 0);
    const uint32_t dead[] = {MO_OP_CONST, 0, 0, MO_OP_RET, 0, MO_OP_RET, 0};
    code(&t, dead, N(dead));
    expect(&t, "unreachable instruction", 0, 5);
    code(&t, NULL, 0);
    expect(&t, "function has no entry instruction", 0, 0);
    const uint32_t wrong[] = {MO_OP_CONST, 0, 1, MO_OP_RET, 0};
    code(&t, wrong, N(wrong));
    expect(&t, "constant result type", 0, 0);
    const uint32_t loop[] = {MO_OP_JUMP, 0};
    code(&t, loop, N(loop));
    expect(&t, NULL, 0, 0);
}

static void memory(void) {
    struct fixture t;
    init(&t);
    const uint32_t borrowed[] = {MO_OP_CONST, 4, 3, MO_OP_MOVE, 5, 4,
                                 MO_OP_CONST, 0, 0, MO_OP_RET,  0};
    code(&t, borrowed, N(borrowed));
    expect(&t, NULL, 0, 0);
    const uint32_t consume_borrow[] = {MO_OP_CONST, 4, 3, MO_OP_DROP, 4,
                                       MO_OP_CONST, 0, 0, MO_OP_RET,  0};
    code(&t, consume_borrow, N(consume_borrow));
    expect(&t, "consume needs an owned reference", 0, 4);
    const uint32_t leak[] = {MO_OP_CONST, 4, 3, MO_OP_DUP, 5, 4, MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    code(&t, leak, N(leak));
    expect(&t, "return leaves ownership or token", 0, 9);
    const uint32_t overwrite[] = {MO_OP_CONST, 4, 3, MO_OP_DUP,  5, 4,
                                  MO_OP_DUP,   5, 4, MO_OP_STOP, 4};
    code(&t, overwrite, N(overwrite));
    expect(&t, "write overwrites ownership or token", 0, 7);
    const uint32_t moved[] = {MO_OP_CONST, 4, 3, MO_OP_DUP, 5, 4, MO_OP_DROP, 5, MO_OP_STOP, 5};
    code(&t, moved, N(moved));
    expect(&t, "register is not readable", 0, 9);
    const uint32_t stop[] = {MO_OP_CONST, 4, 3, MO_OP_DUP, 5, 4, MO_OP_STOP, 4};
    code(&t, stop, N(stop));
    expect(&t, NULL, 0, 0);
    const uint32_t replace[] = {MO_OP_CONST, 4,          3, MO_OP_DUP,   5, 4, MO_OP_MOVE, 5,
                                5,           MO_OP_DROP, 5, MO_OP_CONST, 0, 0, MO_OP_RET,  0};
    code(&t, replace, N(replace));
    expect(&t, NULL, 0, 0);
    struct mo_buffer wire;
    struct mo_write_error we;
    struct mo_load_error le;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_write(&t.file, &wire, &we));
    struct mo_image *image = mo_load(wire.bytes, wire.length, &le);
    CHECK(image != NULL);
    if (image != NULL) {
        bool readable[N(default_regs)];
        CHECK(mo_image_registers_readable(image, 0, 9, readable, &le));
        CHECK(readable[4] && readable[5] && !readable[0] && !readable[1]);
        CHECK(mo_image_registers_readable(image, 0, 11, readable, &le));
        CHECK(readable[4] && !readable[5] && !readable[0]);
        CHECK(!mo_image_registers_readable(image, 0, 10, readable, &le));
        CHECK(strcmp(le.reason, "invalid replay instruction") == 0 && le.offset == 0);
        CHECK(!mo_image_registers_readable(image, 1, 0, readable, &le));
        CHECK(strcmp(le.reason, "invalid replay function") == 0 && le.offset == 0);
        mo_image_free(image);
    }
    mo_buffer_free(&wire);
}

struct clear_replay {
    uint32_t visited;
};
static MO_MUST_USE bool clear_readable(uint32_t pc, const bool *readable, void *context) {
    struct clear_replay *replay = context;
    if (pc == 3 || pc == 5) {
        CHECK(readable[0] == (pc == 3));
        replay->visited++;
    }
    return true;
}

static void clears(void) {
    struct fixture t;
    init(&t);
    const uint32_t value[] = {MO_OP_CONST, 0, 0, MO_OP_CLEAR, 0, MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    code(&t, value, N(value));
    expect(&t, NULL, 0, 0);
    struct mo_buffer wire;
    struct mo_write_error we;
    struct mo_load_error le;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_write(&t.file, &wire, &we));
    struct mo_image *image = mo_load(wire.bytes, wire.length, &le);
    CHECK(image != NULL);
    if (image != NULL) {
        bool readable[N(default_regs)];
        CHECK(mo_image_registers_readable(image, 0, 3, readable, &le));
        CHECK(readable[0]);
        CHECK(mo_image_registers_readable(image, 0, 5, readable, &le));
        CHECK(!readable[0]);
        struct clear_replay replay = {0};
        CHECK(mo_image_visit_readable(image, 0, readable, clear_readable, &replay, &le));
        CHECK(replay.visited == 2);
        mo_image_free(image);
    }
    mo_buffer_free(&wire);
    const uint32_t read[] = {MO_OP_CONST, 0, 0, MO_OP_CLEAR, 0, MO_OP_RET, 0};
    code(&t, read, N(read));
    expect(&t, "register is not readable", 0, 6);
    const uint32_t owned[] = {MO_OP_CONST, 4, 3, MO_OP_DUP, 5, 4, MO_OP_CLEAR, 5, MO_OP_STOP, 4};
    code(&t, owned, N(owned));
    expect(&t, "write overwrites ownership or token", 0, 7);
    const uint32_t token[] = {MO_OP_CONST,
                              1,
                              1,
                              MO_OP_CONST,
                              4,
                              3,
                              MO_OP_DUP,
                              5,
                              4,
                              MO_OP_CONSTRUCT | (2U << 16),
                              6,
                              1,
                              1,
                              5,
                              MO_OP_RESET,
                              8,
                              6,
                              MO_OP_CLEAR,
                              8,
                              MO_OP_STOP,
                              4};
    code(&t, token, N(token));
    expect(&t, "write overwrites ownership or token", 0, 18);
    const uint32_t freed[] = {MO_OP_CONST, 1,
                              1,           MO_OP_CONST,
                              4,           3,
                              MO_OP_DUP,   5,
                              4,           MO_OP_CONSTRUCT | (2U << 16),
                              6,           1,
                              1,           5,
                              MO_OP_RESET, 8,
                              6,           MO_OP_FREE_TOKEN,
                              8,           MO_OP_CLEAR,
                              8,           MO_OP_CONST,
                              0,           0,
                              MO_OP_RET,   0};
    code(&t, freed, N(freed));
    expect(&t, NULL, 0, 0);
    const uint32_t joins[] = {MO_OP_CONST, 0,
                              0,           MO_OP_CONST,
                              3,           2,
                              MO_OP_CONST, 4,
                              3,           MO_OP_BRANCH,
                              3,           13,
                              17,          MO_OP_CLEAR,
                              4,           MO_OP_JUMP,
                              26,          MO_OP_DUP,
                              4,           4,
                              MO_OP_DROP,  4,
                              MO_OP_CLEAR, 4,
                              MO_OP_JUMP,  26,
                              MO_OP_RET,   0};
    code(&t, joins, N(joins));
    expect(&t, NULL, 0, 0);
    /* Leave the borrowed and moved states intact on either edge, without changing shape. */
    uint32_t unequal[N(joins)];
    memcpy(unequal, joins, sizeof unequal);
    unequal[14] = 2;
    code(&t, unequal, N(unequal));
    expect(&t, "register states disagree at join", 0, 26);
    memcpy(unequal, joins, sizeof unequal);
    unequal[23] = 2;
    expect(&t, "register states disagree at join", 0, 26);
    const uint32_t loop[] = {MO_OP_CONST,
                             0,
                             0,
                             MO_OP_CONST,
                             3,
                             2,
                             MO_OP_JUMP,
                             8,
                             MO_OP_BRANCH,
                             3,
                             12,
                             26,
                             MO_OP_CONST,
                             4,
                             3,
                             MO_OP_DUP,
                             5,
                             4,
                             MO_OP_DROP,
                             5,
                             MO_OP_CLEAR,
                             4,
                             MO_OP_CLEAR,
                             5,
                             MO_OP_JUMP,
                             8,
                             MO_OP_RET,
                             0};
    code(&t, loop, N(loop));
    expect(&t, NULL, 0, 0);
    uint32_t backedge[N(loop)];
    memcpy(backedge, loop, sizeof backedge);
    backedge[23] = 2;
    code(&t, backedge, N(backedge));
    expect(&t, "register states disagree at join", 0, 8);

    const uint32_t regs[] = {15, 10, 3};
    t.functions[0].register_types = regs;
    t.functions[0].register_count = N(regs);
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    const uint32_t known[] = {MO_OP_SWITCH | (2U << 16),
                              0,
                              13,
                              4,
                              MO_OP_CLEAR,
                              0,
                              MO_OP_CASE_FIELD,
                              2,
                              0,
                              1,
                              0,
                              MO_OP_STOP,
                              1,
                              MO_OP_STOP,
                              1};
    code(&t, known, N(known));
    expect(&t, "case is not known", 0, 6);
}

static void arithmetic(void) {
    const struct row {
        uint32_t op, left, right, result, count;
    } rows[] = {{MO_OP_ADD_INT, 3, 3, 3, 3},      {MO_OP_ADD_DECIMAL, 8, 8, 8, 3},
                {MO_OP_ADD_FLOAT, 4, 4, 4, 3},    {MO_OP_SUB_INT, 3, 3, 3, 3},
                {MO_OP_SUB_DECIMAL, 8, 8, 8, 3},  {MO_OP_SUB_FLOAT, 4, 4, 4, 3},
                {MO_OP_MUL_INT, 3, 3, 3, 3},      {MO_OP_MUL_DECIMAL, 8, 8, 8, 3},
                {MO_OP_MUL_FLOAT, 4, 4, 4, 3},    {MO_OP_DIV_INT, 3, 3, 3, 3},
                {MO_OP_DIV_DECIMAL, 8, 8, 8, 3},  {MO_OP_DIV_FLOAT, 4, 4, 4, 3},
                {MO_OP_REM_INT, 3, 3, 3, 3},      {MO_OP_REM_DECIMAL, 8, 8, 8, 3},
                {MO_OP_REM_FLOAT, 4, 4, 4, 3},    {MO_OP_NEG_INT, 3, 3, 3, 2},
                {MO_OP_NEG_DECIMAL, 8, 8, 8, 2},  {MO_OP_NEG_FLOAT, 4, 4, 4, 2},
                {MO_OP_NEG_DURATION, 7, 7, 7, 2}, {MO_OP_ADD_DURATION, 7, 7, 7, 3},
                {MO_OP_SUB_DURATION, 7, 7, 7, 3}, {MO_OP_SCALE_DURATION, 7, 3, 7, 3},
                {MO_OP_ADD_TIME, 6, 7, 6, 3},     {MO_OP_SUB_TIME, 6, 7, 6, 3},
                {MO_OP_DIFF_TIME, 6, 6, 7, 3},    {MO_OP_CMP_FLOAT, 4, 4, 19, 3},
                {MO_OP_NOT, 1, 1, 1, 2},          {MO_OP_MAKE_RANGE, 3, 3, 9, 3},
                {MO_OP_RANGE_START, 9, 9, 3, 2},  {MO_OP_RANGE_END, 9, 9, 3, 2}};
    for (uint32_t i = 0; i < N(rows) + 27; i++) {
        struct fixture t;
        init(&t);
        t.types[19] = t.types[20];
        t.file.type_count = 20;
        struct row row;
        if (i < N(rows)) {
            row = rows[i];
        } else {
            static const uint32_t kinds[] = {3, 8, 4, 1, 2, 5, 6, 7, 19};
            row = (struct row){MO_OP_EQ_INT + i - N(rows), kinds[(i - N(rows)) % 9],
                               kinds[(i - N(rows)) % 9], 1, 3};
        }
        uint32_t regs[] = {row.left, row.right, row.result};
        t.functions[0].register_types = regs;
        t.functions[0].register_count = N(regs);
        t.functions[0].result_type = row.result;
        parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
        uint32_t words[] = {row.op, 2, 0, 1, MO_OP_RET, 2};
        if (row.count == 2) {
            words[3] = MO_OP_RET;
            words[4] = 2;
        }
        code(&t, words, row.count + 3);
        expect(&t, NULL, 0, 0);
        regs[0] = row.left == 2 ? 3U : 2U;
        parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
        const char *reason = i < 15        ? "arithmetic argument type"
                             : i < 19      ? "negation argument type"
                             : i < 21      ? "duration arithmetic argument type"
                             : i == 21     ? "duration scale argument type"
                             : i < 24      ? "time arithmetic argument type"
                             : i == 24     ? "time difference argument type"
                             : i == 25     ? "float comparison argument type"
                             : i == 26     ? "not argument type"
                             : i == 27     ? "range argument type"
                             : i < N(rows) ? "range projection argument type"
                                           : "comparison argument type";
        expect(&t, reason, 0, 0);
        regs[0] = row.left;
        parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
        regs[2] = 2;
        t.functions[0].result_type = 2;
        reason = i < 15        ? "arithmetic result type"
                 : i < 19      ? "negation result type"
                 : i < 21      ? "duration arithmetic result type"
                 : i == 21     ? "duration scale result type"
                 : i < 24      ? "time arithmetic result type"
                 : i == 24     ? "time difference result type"
                 : i == 25     ? "float comparison needs Ordering"
                 : i == 26     ? "not result type"
                 : i == 27     ? "range result type"
                 : i < N(rows) ? "range projection result type"
                               : "comparison result type";
        expect(&t, reason, 0, 0);
    }
}

static void choices_and_joins(void) {
    struct fixture t;
    init(&t);
    const uint32_t regs[] = {15, 1, 0, 3, 10, 0, 15};
    t.functions[0].register_types = regs;
    t.functions[0].register_count = N(regs);
    parameter(&t, 0, 2, MO_PARAMETER_OWNED);
    const uint32_t words[] = {MO_OP_SWITCH | (2U << 16),
                              0,
                              4,
                              11,
                              MO_OP_DROP,
                              0,
                              MO_OP_CONST,
                              2,
                              0,
                              MO_OP_RET,
                              2,
                              MO_OP_CASE_FIELD,
                              3,
                              0,
                              1,
                              0,
                              MO_OP_CASE_FIELD,
                              4,
                              0,
                              1,
                              1,
                              MO_OP_DROP,
                              4,
                              MO_OP_DROP,
                              0,
                              MO_OP_CONST,
                              2,
                              0,
                              MO_OP_RET,
                              2};
    code(&t, words, N(words));
    expect(&t, NULL, 0, 0);
    uint32_t wrong[N(words)];
    memcpy(wrong, words, sizeof wrong);
    wrong[2] = 11;
    code(&t, wrong, N(wrong));
    expect(&t, "unreachable instruction", 0, 4);
    const uint32_t no_refinement[] = {MO_OP_CASE_FIELD, 3, 0, 1, 0, MO_OP_DROP, 0, MO_OP_RET, 3};
    t.functions[0].result_type = 3;
    code(&t, no_refinement, N(no_refinement));
    expect(&t, "case is not known", 0, 0);
    const uint32_t shared[] = {MO_OP_SWITCH | (2U << 16),
                               0,
                               4,
                               4,
                               MO_OP_CASE_FIELD,
                               3,
                               0,
                               1,
                               0,
                               MO_OP_DROP,
                               0,
                               MO_OP_RET,
                               3};
    code(&t, shared, N(shared));
    expect(&t, "case is not known", 0, 4);
    const uint32_t moved[] = {MO_OP_SWITCH | (2U << 16),
                              0,
                              4,
                              8,
                              MO_OP_DROP,
                              0,
                              MO_OP_RET,
                              3,
                              MO_OP_MOVE,
                              6,
                              0,
                              MO_OP_CASE_FIELD,
                              3,
                              6,
                              1,
                              0,
                              MO_OP_DROP,
                              6,
                              MO_OP_RET,
                              3};
    code(&t, moved, N(moved));
    expect(&t, "case is not known", 0, 11);
    init(&t);
    const uint32_t joins[] = {
        MO_OP_CONST, 0,  0,  MO_OP_CONST, 3, 2, MO_OP_CONST, 1,  1,          MO_OP_BRANCH,
        3,           13, 18, MO_OP_DUP,   2, 1, MO_OP_JUMP,  20, MO_OP_JUMP, 20,
        MO_OP_RET,   0};
    code(&t, joins, N(joins));
    expect(&t, "register states disagree at join", 0, 20);
    const uint32_t agrees[] = {MO_OP_CONST, 0,  0,  MO_OP_CONST, 3,  2,          MO_OP_BRANCH,
                               3,           10, 12, MO_OP_JUMP,  14, MO_OP_JUMP, 14,
                               MO_OP_RET,   0};
    code(&t, agrees, N(agrees));
    expect(&t, NULL, 0, 0);
}

static void data(void) {
    struct fixture t;
    init(&t);
    const uint32_t build[] = {MO_OP_CONST,
                              1,
                              1,
                              MO_OP_CONST,
                              4,
                              3,
                              MO_OP_DUP,
                              5,
                              4,
                              MO_OP_CONSTRUCT | (2U << 16),
                              6,
                              1,
                              1,
                              5,
                              MO_OP_FIELD,
                              2,
                              6,
                              0,
                              MO_OP_FIELD,
                              5,
                              6,
                              1,
                              MO_OP_SET_FIELD,
                              7,
                              6,
                              1,
                              5,
                              MO_OP_DROP,
                              7,
                              MO_OP_CONST,
                              0,
                              0,
                              MO_OP_RET,
                              0};
    code(&t, build, N(build));
    expect(&t, NULL, 0, 0);
    uint32_t wrong[N(build)];
    memcpy(wrong, build, sizeof wrong);
    wrong[17] = 2;
    code(&t, wrong, N(wrong));
    expect(&t, "field source or index", 0, 14);
    const uint32_t reuse[] = {MO_OP_CONST,
                              1,
                              1,
                              MO_OP_CONST,
                              4,
                              3,
                              MO_OP_DUP,
                              5,
                              4,
                              MO_OP_CONSTRUCT | (2U << 16),
                              6,
                              1,
                              1,
                              5,
                              MO_OP_RESET,
                              8,
                              6,
                              MO_OP_DUP,
                              5,
                              4,
                              MO_OP_REUSE | (2U << 16),
                              7,
                              8,
                              1,
                              1,
                              5,
                              MO_OP_DROP,
                              7,
                              MO_OP_CONST,
                              0,
                              0,
                              MO_OP_RET,
                              0};
    code(&t, reuse, N(reuse));
    expect(&t, NULL, 0, 0);
    uint32_t bad[N(reuse)];
    memcpy(bad, reuse, sizeof bad);
    bad[21] = 11;
    bad[23] = 0;
    code(&t, bad, N(bad));
    expect(&t, "reuse layout differs from token", 0, 20);
    const uint32_t token[] = {MO_OP_CONST, 1,
                              1,           MO_OP_CONST,
                              4,           3,
                              MO_OP_DUP,   5,
                              4,           MO_OP_CONSTRUCT | (2U << 16),
                              6,           1,
                              1,           5,
                              MO_OP_RESET, 8,
                              6,           MO_OP_FREE_TOKEN,
                              8,           MO_OP_CONST,
                              0,           0,
                              MO_OP_RET,   0};
    code(&t, token, N(token));
    expect(&t, NULL, 0, 0);
    const uint32_t not_token[] = {MO_OP_FREE_TOKEN, 8, MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    code(&t, not_token, N(not_token));
    expect(&t, "register is not a token", 0, 1);
    const uint32_t boxed[] = {
        MO_OP_CONST, 1,         1,           MO_OP_CONST, 4,
        3,           MO_OP_DUP, 5,           4,           MO_OP_MAKE_CASE | (2U << 16),
        9,           15,        1,           1,           5,
        MO_OP_DROP,  9,         MO_OP_CONST, 0,           0,
        MO_OP_RET,   0};
    code(&t, boxed, N(boxed));
    expect(&t, NULL, 0, 0);
    t.file.layout_count = 2;
    expect(&t, "boxed case layout is missing", 0, 9);

    init(&t);
    const uint32_t case_regs[] = {15, 0, 3};
    t.functions[0].register_types = case_regs;
    t.functions[0].register_count = N(case_regs);
    t.functions[0].result_type = 3;
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    const uint32_t reset_case[] = {
        MO_OP_CONST, 2, 1, MO_OP_SWITCH | (2U << 16), 0, 7,         11, MO_OP_DROP, 0, MO_OP_RET, 2,
        MO_OP_RESET, 1, 0, MO_OP_FREE_TOKEN,          1, MO_OP_RET, 2};
    code(&t, reset_case, N(reset_case));
    expect(&t, NULL, 0, 0);
    struct mo_buffer wire;
    struct mo_write_error we;
    struct mo_load_error le;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_write(&t.file, &wire, &we));
    struct mo_image *image = mo_load(wire.bytes, wire.length, &le);
    CHECK(image != NULL);
    if (image != NULL) {
        bool readable[N(case_regs)];
        CHECK(mo_image_registers_readable(image, 0, 14, readable, &le));
        CHECK(!readable[0] && !readable[1] && readable[2]);
        mo_image_free(image);
    }
    mo_buffer_free(&wire);
    uint32_t misuse[N(reset_case)];
    memcpy(misuse, reset_case, sizeof misuse);
    misuse[14] = MO_OP_DROP;
    code(&t, misuse, N(misuse));
    expect(&t, "register is not readable", 0, 15);
    const uint32_t token_exit[] = {MO_OP_CONST, 2,         1,         MO_OP_SWITCH | (2U << 16),
                                   0,           7,         11,        MO_OP_DROP,
                                   0,           MO_OP_RET, 2,         MO_OP_RESET,
                                   1,           0,         MO_OP_RET, 2};
    code(&t, token_exit, N(token_exit));
    expect(&t, "return leaves ownership or token", 0, 14);
    const uint32_t token_overwrite[] = {MO_OP_CONST,
                                        2,
                                        1,
                                        MO_OP_SWITCH | (2U << 16),
                                        0,
                                        7,
                                        11,
                                        MO_OP_DROP,
                                        0,
                                        MO_OP_RET,
                                        2,
                                        MO_OP_RESET,
                                        1,
                                        0,
                                        MO_OP_CONST,
                                        1,
                                        0,
                                        MO_OP_RET,
                                        2};
    code(&t, token_overwrite, N(token_overwrite));
    expect(&t, "write overwrites ownership or token", 0, 15);
    const uint32_t unknown_reset[] = {MO_OP_RESET, 1, 0, MO_OP_FREE_TOKEN, 1,
                                      MO_OP_CONST, 2, 1, MO_OP_RET,        2};
    code(&t, unknown_reset, N(unknown_reset));
    expect(&t, "reset needs a known fixed layout", 0, 0);
}

static void calls(void) {
    struct fixture t;
    init(&t);
    t.file.function_count = 2;
    t.file.debug_count = 2;
    const uint32_t target_regs[] = {10, 0}, target_code[] = {MO_OP_CONST, 1, 0, MO_OP_RET, 1};
    t.functions[1].register_types = target_regs;
    t.functions[1].register_count = N(target_regs);
    t.functions[1].code = target_code;
    t.functions[1].code_count = N(target_code);
    parameter(&t, 1, 1, MO_PARAMETER_BORROWED);
    const uint32_t borrowed[] = {MO_OP_CONST, 4, 3, MO_OP_CALL | (1U << 16), 0, 1, 4, MO_OP_RET, 0};
    code(&t, borrowed, N(borrowed));
    expect(&t, NULL, 0, 0);
    const uint32_t tail[] = {MO_OP_CONST, 4, 3, MO_OP_TAIL_CALL | (1U << 16), 1, 4};
    code(&t, tail, N(tail));
    expect(&t, NULL, 0, 0);
    const uint32_t owned_tail[] = {MO_OP_CONST, 4, 3, MO_OP_DUP, 5, 4, MO_OP_TAIL_CALL | (1U << 16),
                                   1,           5};
    code(&t, owned_tail, N(owned_tail));
    expect(&t, "tail borrow needs a borrowed reference", 0, 8);
    const uint32_t leak_tail[] = {MO_OP_CONST, 4, 3, MO_OP_DUP, 5, 4, MO_OP_TAIL_CALL | (1U << 16),
                                  1,           4};
    code(&t, leak_tail, N(leak_tail));
    expect(&t, "return leaves ownership or token", 0, 6);
    code(&t, owned_tail, N(owned_tail));
    const uint32_t drop_target[] = {MO_OP_DROP, 0, MO_OP_CONST, 1, 0, MO_OP_RET, 1};
    t.functions[1].code = drop_target;
    t.functions[1].code_count = N(drop_target);
    parameter(&t, 1, 1, MO_PARAMETER_OWNED);
    expect(&t, NULL, 0, 0);
    code(&t, borrowed, N(borrowed));
    expect(&t, "consume needs an owned reference", 0, 6);
    uint32_t wrong[N(borrowed)];
    memcpy(wrong, borrowed, sizeof wrong);
    wrong[6] = 1;
    code(&t, wrong, N(wrong));
    expect(&t, "direct call argument type", 0, 3);
    init(&t);
    t.file.function_count = 2;
    t.file.debug_count = 2;
    const uint32_t spec[] = {MO_OP_CONST, 4, 3, MO_OP_FAIL, 4};
    t.functions[1].kind = MO_FUNCTION_TEST;
    t.functions[1].spec_code = true;
    t.functions[1].code = spec;
    t.functions[1].code_count = N(spec);
    const uint32_t invoke[] = {MO_OP_CALL, 0, 1, MO_OP_RET, 0};
    code(&t, invoke, N(invoke));
    expect(&t, "direct call to spec code", 0, 0);
    t.functions[0].kind = MO_FUNCTION_TEST;
    t.functions[0].spec_code = true;
    expect(&t, NULL, 0, 0);
    t.file.function_count = 1;
    t.file.debug_count = 1;
    code(&t, spec, N(spec));
    expect(&t, NULL, 0, 0);
    t.functions[0].kind = MO_FUNCTION_FUNCTION;
    t.functions[0].spec_code = false;
    expect(&t, "fail outside spec code", 0, 3);
}

/* The one signature check behind make_closure and the validator's closure layouts and
 * literals: each case differs from the matching type in exactly one respect. */
static void closure_signatures(void) {
    const uint32_t int_param[] = {0}, two_ints[] = {0, 0}, bool_param[] = {1};
    const struct mo_type types[] = {
        {.kind = MO_TYPE_INT},
        {.kind = MO_TYPE_BOOL},
        {.kind = MO_TYPE_FUNCTION, .parameters = int_param, .parameter_count = 1, .result = 0},
        {.kind = MO_TYPE_FUNCTION, .parameters = int_param, .parameter_count = 1, .result = 1},
        {.kind = MO_TYPE_FUNCTION, .parameters = two_ints, .parameter_count = 2, .result = 0},
        {.kind = MO_TYPE_FUNCTION, .parameters = bool_param, .parameter_count = 1, .result = 0},
        {.kind = MO_TYPE_TUPLE, .parameters = int_param, .parameter_count = 1, .result = 0},
    };
    const struct mo_parameter param[] = {{0, MO_PARAMETER_OWNED}};
    const struct mo_bytecode_function functions[] = {
        {.kind = MO_FUNCTION_CLOSURE, .parameter_count = 1, .parameters = param, .result_type = 0},
        {.kind = MO_FUNCTION_FUNCTION, .parameter_count = 1, .parameters = param, .result_type = 0},
    };
    const struct mo_bytecode f = {.types = types,
                                  .type_count = N(types),
                                  .functions = functions,
                                  .function_count = N(functions)};
    CHECK(mo_verify_closure_signature(&f, 2, 0));
    CHECK(!mo_verify_closure_signature(&f, 3, 0)); /* result */
    CHECK(!mo_verify_closure_signature(&f, 4, 0)); /* parameter count */
    CHECK(!mo_verify_closure_signature(&f, 5, 0)); /* parameter type */
    CHECK(!mo_verify_closure_signature(&f, 6, 0)); /* not a function type */
    CHECK(!mo_verify_closure_signature(&f, 2, 1)); /* not closure code */
}

static void closures(void) {
    struct fixture t;
    init(&t);
    t.file.function_count = 2;
    t.file.debug_count = 2;
    t.file.type_count = 20;
    const uint32_t regs[] = {3, 19, 10}, body[] = {MO_OP_DROP, 1, MO_OP_RET, 0};
    t.functions[1].kind = MO_FUNCTION_CLOSURE;
    t.functions[1].result_type = 3;
    t.functions[1].register_types = regs;
    t.functions[1].register_count = N(regs);
    t.functions[1].code = body;
    t.functions[1].code_count = N(body);
    parameter(&t, 1, 1, MO_PARAMETER_OWNED);
    t.layouts[3] = (struct mo_layout){.kind = MO_LAYOUT_CLOSURE, .type = 17, .function = 1};
    t.file.layout_count = 4;
    t.constants[4] = (struct mo_bytecode_constant){.type = 17, .value.function = 1};
    t.file.constant_count = 5;
    const uint32_t call[] = {
        MO_OP_CONST, 13, 4, MO_OP_DUP,   13, 13, MO_OP_CONST, 1, 1, MO_OP_CALL_CLOSURE | (1U << 16),
        2,           13, 1, MO_OP_CONST, 0,  0,  MO_OP_RET,   0};
    code(&t, call, N(call));
    expect(&t, NULL, 0, 0);
    t.file.layout_count = 3;
    expect(&t, "closure constant layout is missing", MO_NO_INDEX, 20);
    t.file.layout_count = 4;
    const uint32_t direct[] = {
        MO_OP_CONST, 1, 1, MO_OP_CALL | (1U << 16), 2, 1, 1, MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    code(&t, direct, N(direct));
    expect(&t, "direct call target kind", 0, 3);
    const uint32_t construct[] = {MO_OP_CONSTRUCT, 13, 3, MO_OP_DROP, 13,
                                  MO_OP_CONST,     0,  0, MO_OP_RET,  0};
    code(&t, construct, N(construct));
    expect(&t, "construct/reuse closure layout", 0, 0);
    const uint32_t tail[] = {MO_OP_CONST, 13,
                             4,           MO_OP_DUP,
                             13,          13,
                             MO_OP_CONST, 1,
                             1,           MO_OP_TAIL_CALL_CLOSURE | (1U << 16),
                             13,          1};
    t.functions[0].result_type = 3;
    code(&t, tail, N(tail));
    expect(&t, NULL, 0, 0);
    t.functions[1].spec_code = true;
    expect(&t, "spec closure constant in non-spec code", 0, 0);
    t.functions[1].spec_code = false;
    t.functions[0].result_type = 0;
    const uint32_t capture[] = {10};
    const uint32_t captured_body[] = {MO_OP_FIELD, 2,          1, 0,         MO_OP_DROP,
                                      2,           MO_OP_DROP, 1, MO_OP_RET, 0};
    t.functions[1].capture_count = 1;
    t.functions[1].capture_types = capture;
    t.functions[1].code = captured_body;
    t.functions[1].code_count = N(captured_body);
    t.layouts[3].fields = capture;
    t.layouts[3].field_count = 1;
    t.file.constant_count = 4;
    const uint32_t build[] = {MO_OP_CONST,
                              4,
                              3,
                              MO_OP_DUP,
                              5,
                              4,
                              MO_OP_MAKE_CLOSURE | (1U << 16),
                              13,
                              1,
                              5,
                              MO_OP_CONST,
                              1,
                              1,
                              MO_OP_CALL_CLOSURE | (1U << 16),
                              2,
                              13,
                              1,
                              MO_OP_CONST,
                              0,
                              0,
                              MO_OP_RET,
                              0};
    code(&t, build, N(build));
    expect(&t, NULL, 0, 0);
    uint32_t bad_closure[N(build)];
    memcpy(bad_closure, build, sizeof bad_closure);
    bad_closure[8] = 0;
    code(&t, bad_closure, N(bad_closure));
    expect(&t, "closure signature", 0, 6);
    bad_closure[8] = 1;
    bad_closure[9] = 1;
    expect(&t, "closure capture types or count", 0, 6);
    code(&t, build, N(build));
    const uint32_t set_env[] = {MO_OP_SET_FIELD, 1, 1, 0, 2, MO_OP_STOP, 2};
    t.functions[1].code = set_env;
    t.functions[1].code_count = N(set_env);
    expect(&t, "set_field needs record or tuple", 1, 0);
    t.functions[1].code = captured_body;
    t.functions[1].code_count = N(captured_body);
    t.file.layout_count = 3;
    expect(&t, "closure layout is missing", 0, 6);
    t.file.layout_count = 4;
    t.functions[1].spec_code = true;
    expect(&t, "spec closure in non-spec code", 0, 6);
}

static void primitives(void) {
    struct fixture t;
    init(&t);
    const uint32_t regs[] = {16, 3};
    t.functions[0].register_types = regs;
    t.functions[0].register_count = N(regs);
    t.functions[0].result_type = 16;
    parameter(&t, 0, 2, MO_PARAMETER_OWNED);
    uint32_t append[] = {MO_OP_PRIM | (2U << 16), 0, MO_PRIM_LIST_APPEND, 18, 0, 1, MO_OP_RET, 0};
    code(&t, append, N(append));
    expect(&t, NULL, 0, 0);
    append[1] = 1;
    expect(&t, "primitive result type", 0, 0);
    append[1] = 0;
    append[4] = 1;
    expect(&t, "primitive argument types or count", 0, 0);
    append[4] = 0;
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    expect(&t, "consume needs an owned reference", 0, 4);
    parameter(&t, 0, 2, MO_PARAMETER_OWNED);
    append[2] = MO_PRIM_LIST_SET;
    expect(&t, "primitive arity", 0, 0);
    init(&t);
    const uint32_t text_args[] = {10, 10};
    t.types[19] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = text_args, .parameter_count = 2, .result = 10};
    t.file.type_count = 20;
    const uint32_t text_regs[] = {10, 10};
    t.functions[0].register_types = text_regs;
    t.functions[0].register_count = N(text_regs);
    t.functions[0].result_type = 10;
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    const uint32_t repeat[] = {
        MO_OP_PRIM | (2U << 16), 0, MO_PRIM_TEXT_CONCAT, 19, 0, 0, MO_OP_RET, 0};
    code(&t, repeat, N(repeat));
    expect(&t, "consumed register is repeated", 0, 5);
    t.file.function_count = 2;
    t.file.debug_count = 2;
    const uint32_t alias_regs[] = {10, 10, 10}, alias_body[] = {MO_OP_RET, 1};
    t.functions[1].register_types = alias_regs;
    t.functions[1].register_count = N(alias_regs);
    t.functions[1].result_type = 10;
    t.functions[1].code = alias_body;
    t.functions[1].code_count = N(alias_body);
    parameter(&t, 1, 2, MO_PARAMETER_OWNED);
    t.parameters[1][0].ownership = MO_PARAMETER_BORROWED;
    t.functions[0].register_types = alias_regs;
    t.functions[0].register_count = N(alias_regs);
    parameter(&t, 0, 2, MO_PARAMETER_OWNED);
    uint32_t distinct[] = {MO_OP_CALL | (2U << 16), 2, 1, 0, 1, MO_OP_DROP, 0, MO_OP_RET, 2};
    code(&t, distinct, N(distinct));
    expect(&t, NULL, 0, 0);
    distinct[4] = 0;
    expect(&t, "consumed register is repeated", 0, 4);
    t.functions[0].register_types = text_regs;
    t.functions[0].register_count = N(text_regs);
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    const uint32_t read_then_consume[] = {MO_OP_CALL | (2U << 16), 0, 1, 0, 0, MO_OP_RET, 0};
    code(&t, read_then_consume, N(read_then_consume));
    expect(&t, "consumed register is repeated", 0, 4);
    t.file.function_count = 1;
    t.file.debug_count = 1;
    const uint32_t concat[] = {
        MO_OP_CONST, 1, 3, MO_OP_PRIM | (2U << 16), 0, MO_PRIM_TEXT_CONCAT, 19, 0, 1, MO_OP_RET, 0};
    code(&t, concat, N(concat));
    expect(&t, NULL, 0, 0);
    init(&t);
    t.types[19] = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_CONSOLE};
    t.file.type_count = 20;
    const uint32_t host_regs[] = {19, 10, 0};
    t.functions[0].register_types = host_regs;
    t.functions[0].register_count = N(host_regs);
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    const uint32_t host[] = {MO_OP_HOST | (2U << 16), 2, MO_HOST_CONSOLE_PRINT, 0, 1, MO_OP_RET, 2};
    code(&t, host, N(host));
    expect(&t, NULL, 0, 0);
    t.types[19].capability = MO_CAP_FILES;
    expect(&t, "host signature or capability", 0, 0);
    t.types[19].capability = MO_CAP_CONSOLE;
    const uint32_t fake[] = {MO_OP_HOST, 0, MO_HOST_CONSOLE_FAKE, MO_OP_DROP, 0, MO_OP_CONST, 2, 0,
                             MO_OP_RET,  2};
    parameter(&t, 0, 0, MO_PARAMETER_OWNED);
    code(&t, fake, N(fake));
    expect(&t, "spec host in non-spec code", 0, 0);
    t.functions[0].kind = MO_FUNCTION_TEST;
    t.functions[0].spec_code = true;
    expect(&t, NULL, 0, 0);
}

static void abi_shapes(void) {
    struct fixture t;
    init(&t);
    struct mo_type_field payload = {NULL, 3};
    struct mo_type_case option[] = {{.name = "None"},
                                    {.name = "Some", .fields = &payload, .field_count = 1}};
    const uint32_t get_args[] = {16, 3}, get_regs[] = {16, 3, 19};
    t.types[19] = (struct mo_type){
        .kind = MO_TYPE_CHOICE, .name = "Maybe", .cases = option, .case_count = 2, .defined = true};
    t.types[20] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = get_args, .parameter_count = 2, .result = 19};
    t.file.type_count = 21;
    t.functions[0].register_types = get_regs;
    t.functions[0].register_count = N(get_regs);
    t.functions[0].result_type = 19;
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    const uint32_t get[] = {MO_OP_PRIM | (2U << 16), 2, MO_PRIM_LIST_GET, 20, 0, 1, MO_OP_RET, 2};
    code(&t, get, N(get));
    expect(&t, NULL, 0, 0);
    payload.type = 4;
    expect(&t, "primitive signature or constraint", 0, 0);
    payload.type = 3;
    option[0].name = "Error";
    expect(&t, "primitive signature or constraint", 0, 0);
    option[0].name = "None";
    payload.name = "left";
    expect(&t, "primitive signature or constraint", 0, 0);
    payload.name = NULL;
    option[0].fields = &payload;
    option[0].field_count = 1;
    expect(&t, "primitive signature or constraint", 0, 0);
    option[0].fields = NULL;
    option[0].field_count = 0;
    payload.type = 8;
    const uint32_t text_arg[] = {10}, decimal_regs[] = {10, 19}, decimal_fields[] = {8};
    t.types[20].parameters = text_arg;
    t.types[20].parameter_count = 1;
    t.functions[0].register_types = decimal_regs;
    t.functions[0].register_count = N(decimal_regs);
    parameter(&t, 0, 1, MO_PARAMETER_BORROWED);
    const uint32_t decimal[] = {
        MO_OP_PRIM | (1U << 16), 1, MO_PRIM_TEXT_TO_DECIMAL, 20, 0, MO_OP_RET, 1};
    code(&t, decimal, N(decimal));
    expect(&t, "primitive signature or constraint", 0, 0);
    t.layouts[3] = (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                      .type = 19,
                                      .case_index = 1,
                                      .fields = decimal_fields,
                                      .field_count = 1};
    t.file.layout_count = 4;
    expect(&t, NULL, 0, 0);

    init(&t);
    const uint32_t texts[] = {10}, tuples[] = {13}, input[] = {19}, enumerate_regs[] = {19, 20};
    t.types[19] = (struct mo_type){.kind = MO_TYPE_LIST, .parameters = texts, .parameter_count = 1};
    t.types[20] =
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = tuples, .parameter_count = 1};
    t.types[21] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = input, .parameter_count = 1, .result = 20};
    t.file.type_count = 22;
    t.functions[0].register_types = enumerate_regs;
    t.functions[0].register_count = N(enumerate_regs);
    t.functions[0].result_type = 20;
    parameter(&t, 0, 1, MO_PARAMETER_BORROWED);
    const uint32_t enumerate[] = {
        MO_OP_PRIM | (1U << 16), 1, MO_PRIM_LIST_ENUMERATE, 21, 0, MO_OP_RET, 1};
    code(&t, enumerate, N(enumerate));
    expect(&t, NULL, 0, 0);
    t.file.layout_count = 0;
    expect(&t, "primitive signature or constraint", 0, 0);

    init(&t);
    struct mo_type_field error_fields[] = {{"path", 10}, {"reason", 10}};
    struct mo_type_case errors[] = {
        {.name = "NotFound", .fields = error_fields, .field_count = 1},
        {.name = "NotAllowed", .fields = error_fields, .field_count = 1},
        {.name = "NotText", .fields = error_fields, .field_count = 1},
        {.name = "Failed", .fields = error_fields, .field_count = 2}};
    struct mo_type_field result_fields[] = {{NULL, 10}, {NULL, 20}};
    struct mo_type_case result[] = {
        {.name = "Ok", .fields = result_fields, .field_count = 1},
        {.name = "Error", .fields = result_fields + 1, .field_count = 1}};
    t.types[19] = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_FILES};
    t.types[20] = (struct mo_type){
        .kind = MO_TYPE_CHOICE, .name = "Maybe", .cases = errors, .case_count = 4, .defined = true};
    t.types[21] = (struct mo_type){
        .kind = MO_TYPE_CHOICE, .name = "Pair", .cases = result, .case_count = 2, .defined = true};
    t.file.type_count = 22;
    uint32_t failed_types[] = {10, 10};
    const uint32_t result_error[] = {20}, host_regs[] = {19, 10, 21};
    t.layouts[3] = (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                      .type = 20,
                                      .case_index = 3,
                                      .fields = failed_types,
                                      .field_count = 2};
    t.layouts[4] = (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                      .type = 21,
                                      .case_index = 1,
                                      .fields = result_error,
                                      .field_count = 1};
    t.file.layout_count = 5;
    t.functions[0].register_types = host_regs;
    t.functions[0].register_count = N(host_regs);
    t.functions[0].result_type = 21;
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    uint32_t host[] = {MO_OP_HOST | (2U << 16), 2, MO_HOST_FILES_READ_TEXT, 0, 1, MO_OP_RET, 2};
    code(&t, host, N(host));
    expect(&t, NULL, 0, 0);
    t.file.layout_count = 4;
    expect(&t, "host signature or capability", 0, 0);
    t.file.layout_count = 5;
    errors[3].name = "Some";
    expect(&t, "host signature or capability", 0, 0);
    errors[3].name = "Failed";
    error_fields[0].name = "name";
    expect(&t, "host signature or capability", 0, 0);
    error_fields[0].name = "path";
    error_fields[1].name = "left";
    expect(&t, "host signature or capability", 0, 0);
    error_fields[1].name = "reason";
    errors[0].field_count = 2;
    t.layouts[5] = t.layouts[3];
    t.layouts[5].case_index = 0;
    t.file.layout_count = 6;
    expect(&t, "host signature or capability", 0, 0);
    errors[0].field_count = 1;
    t.file.layout_count = 5;
    for (uint32_t i = 0; i < 2; i++) {
        error_fields[i].type = 3;
        failed_types[i] = 3;
        expect(&t, "host signature or capability", 0, 0);
        error_fields[i].type = 10;
        failed_types[i] = 10;
    }
    struct mo_type_case entry_kinds[] = {
        {.name = "File"}, {.name = "Folder"}, {.name = "Link"}, {.name = "Other"}};
    struct mo_type_field entry_fields[] = {{"name", 10}, {"kind", 22}};
    uint32_t entry_layout[] = {10, 22};
    const uint32_t entries[] = {23};
    t.types[22] = (struct mo_type){.kind = MO_TYPE_CHOICE,
                                   .name = "Ordering",
                                   .cases = entry_kinds,
                                   .case_count = 4,
                                   .defined = true};
    t.types[23] = (struct mo_type){.kind = MO_TYPE_RECORD,
                                   .name = "Maybe",
                                   .fields = entry_fields,
                                   .field_count = 2,
                                   .defined = true};
    t.types[24] =
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = entries, .parameter_count = 1};
    t.layouts[5] = (struct mo_layout){
        .kind = MO_LAYOUT_RECORD, .type = 23, .fields = entry_layout, .field_count = 2};
    t.file.type_count = 25;
    t.file.layout_count = 6;
    result_fields[0].type = 24;
    host[2] = MO_HOST_FILES_ENTRIES;
    expect(&t, NULL, 0, 0);
    entry_fields[0].name = "path";
    expect(&t, "host signature or capability", 0, 0);
    entry_fields[0].name = "name";
    entry_fields[1].name = "right";
    expect(&t, "host signature or capability", 0, 0);
    entry_fields[1].name = "kind";
    entry_fields[0].type = 3;
    entry_layout[0] = 3;
    expect(&t, "host signature or capability", 0, 0);
    entry_fields[0].type = 10;
    entry_layout[0] = 10;
    entry_fields[1].type = 3;
    entry_layout[1] = 3;
    expect(&t, "host signature or capability", 0, 0);
    entry_fields[1].type = 22;
    entry_layout[1] = 22;
    t.types[22].case_count = 3;
    expect(&t, "host signature or capability", 0, 0);
    t.types[22].case_count = 4;
    entry_kinds[0].name = "Less";
    expect(&t, "host signature or capability", 0, 0);
    entry_kinds[0].name = "File";
    entry_kinds[0].fields = &payload;
    entry_kinds[0].field_count = 1;
    payload.type = 3;
    expect(&t, "host signature or capability", 0, 0);
    entry_kinds[0].fields = NULL;
    entry_kinds[0].field_count = 0;
    t.file.layout_count = 5;
    expect(&t, "host signature or capability", 0, 0);
}

static void globals(void) {
    struct fixture t;
    init(&t);
    t.file.function_count = 4;
    t.file.debug_count = 4;
    t.file.globals = t.globals;
    t.file.global_count = 2;
    t.file.init = t.init;
    t.file.init_count = 2;
    t.init[0] = 0;
    t.init[1] = 1;
    t.global_indices[0] = 0;
    t.global_indices[1] = 1;
    t.module.globals = t.global_indices;
    t.module.global_count = 2;
    t.globals[0] = (struct mo_bytecode_global){.type = 3};
    t.globals[1] = t.globals[0];
    t.functions[0].kind = MO_FUNCTION_INIT;
    t.functions[1].kind = MO_FUNCTION_INIT;
    t.functions[2].result_type = 3;
    t.functions[3].result_type = 3;
    const uint32_t first[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1,
                              MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    const uint32_t second[] = {MO_OP_CALL,  1, 2, MO_OP_SET_GLOBAL, 1, 1,
                               MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    const uint32_t helper[] = {MO_OP_TAIL_CALL, 3};
    const uint32_t recursive[] = {
        MO_OP_CONST, 3,         2, MO_OP_BRANCH,    3, 7, 12, MO_OP_GLOBAL, 1,
        0,           MO_OP_RET, 1, MO_OP_TAIL_CALL, 2};
    code(&t, first, N(first));
    t.functions[1].code = second;
    t.functions[1].code_count = N(second);
    t.functions[2].code = helper;
    t.functions[2].code_count = N(helper);
    t.functions[3].code = recursive;
    t.functions[3].code_count = N(recursive);
    expect(&t, NULL, 0, 0);
    uint32_t bad_store[N(first)], bad_read[N(recursive)];
    memcpy(bad_store, first, sizeof bad_store);
    bad_store[5] = 3;
    code(&t, bad_store, N(bad_store));
    expect(&t, "global store type", 0, 3);
    code(&t, first, N(first));
    memcpy(bad_read, recursive, sizeof bad_read);
    bad_read[8] = 3;
    t.functions[3].code = bad_read;
    expect(&t, "global result type", 3, 7);
    t.functions[3].code = recursive;
    t.init[0] = 1;
    t.init[1] = 0;
    expect(&t, "init reads an unset global", 1, 0);
    t.init[0] = 0;
    t.init[1] = 1;
    const uint32_t twice[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1, MO_OP_SET_GLOBAL, 0, 1,
                              MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    code(&t, twice, N(twice));
    expect(&t, "init sets its global twice", 0, 6);
    const uint32_t different[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1, MO_OP_SET_GLOBAL, 1, 1,
                                  MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    code(&t, different, N(different));
    expect(&t, "init sets different globals", 0, 6);
    code(&t, default_code, N(default_code));
    expect(&t, "init has no global store", 0, 0);
    const uint32_t duplicate[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 1, 1,
                                  MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    code(&t, duplicate, N(duplicate));
    expect(&t, "global has multiple initializers", MO_NO_INDEX, 20);
    const uint32_t self[] = {MO_OP_GLOBAL, 1, 0, MO_OP_SET_GLOBAL, 0, 1,
                             MO_OP_CONST,  0, 0, MO_OP_RET,        0};
    code(&t, self, N(self));
    expect(&t, "init reads an unset global", 0, 0);
    const uint32_t after[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1, MO_OP_GLOBAL, 1, 0,
                              MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    code(&t, after, N(after));
    expect(&t, NULL, 0, 0);
    const uint32_t missing_store[] = {
        MO_OP_CONST, 0, 0, MO_OP_CONST,      3, 2, MO_OP_BRANCH, 3, 10, 12, MO_OP_RET, 0,
        MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1, MO_OP_RET,    0};
    code(&t, missing_store, N(missing_store));
    expect(&t, "init returns without setting its global", 0, 10);
    const uint32_t call_init[] = {MO_OP_CALL, 0, 0, MO_OP_CONST, 1, 1, MO_OP_RET, 1};
    code(&t, first, N(first));
    t.functions[2].code = call_init;
    t.functions[2].code_count = N(call_init);
    expect(&t, "direct call target kind", 2, 0);
    const uint32_t store_outside[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1, MO_OP_RET, 1};
    t.functions[2].code = store_outside;
    t.functions[2].code_count = N(store_outside);
    expect(&t, "global store outside init", 2, 3);
}

static void closure_initialization(void) {
    struct fixture t;
    init(&t);
    t.file.function_count = 4;
    t.file.debug_count = 4;
    t.file.globals = t.globals;
    t.file.global_count = 2;
    t.file.init = t.init;
    t.file.init_count = 2;
    t.init[0] = 0;
    t.init[1] = 1;
    t.global_indices[0] = 0;
    t.global_indices[1] = 1;
    t.module.globals = t.global_indices;
    t.module.global_count = 2;
    t.globals[0] = (struct mo_bytecode_global){.type = 3};
    t.globals[1] = t.globals[0];
    t.functions[0].kind = MO_FUNCTION_INIT;
    t.functions[1].kind = MO_FUNCTION_INIT;
    t.functions[2].kind = MO_FUNCTION_CLOSURE;
    t.types[19] = (struct mo_type){.kind = MO_TYPE_FUNCTION, .result = 3};
    t.types[20] = (struct mo_type){.kind = MO_TYPE_ENV, .function = 2};
    t.file.type_count = 21;
    t.layouts[3] = (struct mo_layout){.kind = MO_LAYOUT_CLOSURE, .type = 19, .function = 2};
    t.file.layout_count = 4;
    t.constants[4] = (struct mo_bytecode_constant){.type = 19, .value.function = 2};
    t.file.constant_count = 5;
    const uint32_t first[] = {MO_OP_CONST, 1, 1, MO_OP_SET_GLOBAL, 0, 1,
                              MO_OP_CONST, 0, 0, MO_OP_RET,        0};
    code(&t, first, N(first));
    const uint32_t init_regs[] = {0, 3, 19}, second[] = {MO_OP_CONST,
                                                         2,
                                                         4,
                                                         MO_OP_CALL | (1U << 16),
                                                         1,
                                                         3,
                                                         2,
                                                         MO_OP_SET_GLOBAL,
                                                         1,
                                                         1,
                                                         MO_OP_CONST,
                                                         0,
                                                         0,
                                                         MO_OP_RET,
                                                         0};
    const uint32_t closure_regs[] = {20, 3},
                   closure_body[] = {MO_OP_CONST, 1, 1, MO_OP_DROP, 0, MO_OP_RET, 1};
    const uint32_t helper_regs[] = {19, 19, 3},
                   helper[] = {MO_OP_DUP, 1, 0, MO_OP_CALL_CLOSURE, 2, 1, MO_OP_RET, 2};
    t.functions[1].register_types = init_regs;
    t.functions[1].register_count = N(init_regs);
    t.functions[1].code = second;
    t.functions[1].code_count = N(second);
    t.functions[2].register_types = closure_regs;
    t.functions[2].register_count = N(closure_regs);
    t.functions[2].result_type = 3;
    t.functions[2].code = closure_body;
    t.functions[2].code_count = N(closure_body);
    t.functions[3].register_types = helper_regs;
    t.functions[3].register_count = N(helper_regs);
    t.functions[3].result_type = 3;
    t.functions[3].code = helper;
    t.functions[3].code_count = N(helper);
    parameter(&t, 3, 1, MO_PARAMETER_BORROWED);
    expect(&t, NULL, 0, 0);
    t.init[0] = 1;
    t.init[1] = 0;
    expect(&t, NULL, 0, 0);
    t.init[0] = 0;
    t.init[1] = 1;
    /* An unrelated same-signature closure must not create an initializer dependency. */
    t.file.function_count = 5;
    t.file.debug_count = 5;
    t.types[21] = (struct mo_type){.kind = MO_TYPE_ENV, .function = 4};
    t.file.type_count = 22;
    t.layouts[4] = (struct mo_layout){.kind = MO_LAYOUT_CLOSURE, .type = 19, .function = 4};
    t.file.layout_count = 5;
    const uint32_t other_regs[] = {21, 3},
                   other[] = {MO_OP_GLOBAL, 1, 1, MO_OP_DROP, 0, MO_OP_RET, 1};
    t.functions[4].kind = MO_FUNCTION_CLOSURE;
    t.functions[4].register_types = other_regs;
    t.functions[4].register_count = N(other_regs);
    t.functions[4].result_type = 3;
    t.functions[4].code = other;
    t.functions[4].code_count = N(other);
    expect(&t, NULL, 0, 0);
    t.init[0] = 1;
    t.init[1] = 0;
    expect(&t, NULL, 0, 0);
}

static void abilities(void) {
    struct fixture t;
    init(&t);
    CHECK(MO_ABILITY_EQUAL == 1 && MO_ABILITY_HASH == 2 && MO_ABILITY_ORDER == 4);
    {
        struct mo_arena arena;
        mo_arena_init(&arena, 1 << 16);
        uint8_t *flags = mo_bytecode_abilities(&arena, t.types, t.file.type_count);
        CHECK(flags != NULL);
        if (flags != NULL) {
            CHECK(flags[MO_TYPE_INT] == (MO_ABILITY_EQUAL | MO_ABILITY_HASH));
            CHECK(flags[MO_TYPE_FLOAT] == MO_ABILITY_EQUAL);
            CHECK(flags[17] == 0);
        }
        mo_arena_free(&arena);
    }
    const uint32_t eq[] = {MO_OP_CONST, 1,           1, MO_OP_EQUAL, 3,         3, 1,
                           1,           MO_OP_CONST, 0, 0,           MO_OP_RET, 0};
    code(&t, eq, N(eq));
    expect(&t, NULL, 0, 0);
    const uint32_t fn_regs[] = {17, 1, 0};
    t.functions[0].register_types = fn_regs;
    t.functions[0].register_count = N(fn_regs);
    parameter(&t, 0, 1, MO_PARAMETER_BORROWED);
    const uint32_t bad[] = {MO_OP_EQUAL, 1, 17, 0, 0, MO_OP_CONST, 2, 0, MO_OP_RET, 2};
    code(&t, bad, N(bad));
    expect(&t, "equal lacks Equal", 0, 0);
    init(&t);
    const uint32_t float_list[] = {4};
    t.types[19] =
        (struct mo_type){.kind = MO_TYPE_SET, .parameters = float_list, .parameter_count = 1};
    t.file.type_count = 20;
    expect(&t, "map/set key lacks Hash", MO_NO_INDEX, 20);
    t.types[19].parameters = list;
    expect(&t, NULL, 0, 0);
    const uint32_t function_list[] = {17};
    t.types[19] =
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = function_list, .parameter_count = 1};
    const uint32_t contains_args[] = {19, 17};
    t.types[20] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = contains_args, .parameter_count = 2, .result = 1};
    t.file.type_count = 21;
    const uint32_t contains_regs[] = {19, 17, 1};
    t.functions[0].register_types = contains_regs;
    t.functions[0].register_count = N(contains_regs);
    t.functions[0].result_type = 1;
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    const uint32_t contains[] = {
        MO_OP_PRIM | (2U << 16), 2, MO_PRIM_LIST_CONTAINS, 20, 0, 1, MO_OP_RET, 2};
    code(&t, contains, N(contains));
    expect(&t, "library Equal constraint", 0, 0);
    t.types[19].parameters = float_list;
    const uint32_t float_contains_args[] = {19, 4}, float_contains_regs[] = {19, 4, 1};
    t.types[20].parameters = float_contains_args;
    t.functions[0].register_types = float_contains_regs;
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    expect(&t, NULL, 0, 0);
    t.types[20].parameter_count = 1;
    t.types[20].result = 4;
    const uint32_t sum_regs[] = {19, 4};
    t.functions[0].register_types = sum_regs;
    t.functions[0].register_count = N(sum_regs);
    t.functions[0].result_type = 4;
    parameter(&t, 0, 1, MO_PARAMETER_BORROWED);
    const uint32_t sum[] = {MO_OP_PRIM | (1U << 16), 1, MO_PRIM_LIST_SUM, 20, 0, MO_OP_RET, 1};
    code(&t, sum, N(sum));
    expect(&t, NULL, 0, 0);
    const uint32_t bool_list[] = {1}, bool_sum_regs[] = {19, 1};
    t.types[19].parameters = bool_list;
    t.types[20].result = 1;
    t.functions[0].register_types = bool_sum_regs;
    t.functions[0].result_type = 1;
    parameter(&t, 0, 1, MO_PARAMETER_BORROWED);
    expect(&t, "library Number constraint", 0, 0);
}

static void linked_promise(void) {
    struct fixture t;
    init(&t);
    t.file.function_count = 2;
    t.file.debug_count = 2;
    const uint32_t registers[] = {0, 3}, body[] = {MO_OP_CONST, 1, 1, MO_OP_RET, 1};
    const struct mo_bytecode_spec_line line = {.file = MO_NO_INDEX, .text = 0};
    t.functions[1].kind = MO_FUNCTION_PROMISE;
    t.functions[1].spec_code = true;
    t.functions[1].register_types = registers;
    t.functions[1].register_count = N(registers);
    t.functions[1].result_type = 3;
    t.functions[1].code = body;
    t.functions[1].code_count = N(body);
    parameter(&t, 1, 1, MO_PARAMETER_OWNED);
    t.debug[1].subject = 0;
    t.debug[1].lines = &line;
    t.debug[1].line_count = 1;
    expect(&t, NULL, 0, 0);
    const uint32_t mismatch[] = {1, 3};
    t.functions[1].register_types = mismatch;
    parameter(&t, 1, 1, MO_PARAMETER_OWNED);
    expect(&t, "promise parameters differ from subject", MO_NO_INDEX, 20);
    t.debug[1].subject = MO_NO_INDEX;
    expect(&t, NULL, 0, 0);
}

static void worked_text(const struct mo_bytecode *file) {
    struct mo_buffer texts[2], expected, printed, assembled;
    mo_buffer_init(&texts[0], MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&texts[1], MO_LIMIT_BYTECODE_TEXT_SIZE);
    mo_buffer_init(&expected, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&assembled, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&printed, MO_LIMIT_BYTECODE_TEXT_SIZE);
    FILE *doc = fopen("docs/design/bytecode.md", "rb");
    CHECK(doc != NULL);
    bool section = false, fence = false;
    unsigned count = 0;
    if (doc != NULL) {
        char line[4096];
        while (fgets(line, sizeof line, doc) != NULL) {
            if (strncmp(line, "### 10.4", 8) == 0)
                break;
            if (strncmp(line, "### 10.3", 8) == 0)
                section = true;
            if (!section)
                continue;
            if (strcmp(line, "```text\n") == 0) {
                fence = true;
                continue;
            }
            if (fence && strcmp(line, "```\n") == 0) {
                fence = false;
                count++;
                continue;
            }
            if (fence && count < 2)
                CHECK(mo_buffer_append_text(&texts[count], line));
        }
        CHECK(ferror(doc) == 0);
        CHECK(fclose(doc) == 0);
    }
    CHECK(count == 2);
    struct mo_write_error we;
    struct mo_load_error le;
    struct mo_assemble_error ae;
    CHECK(mo_bytecode_write(file, &expected, &we));
    CHECK(mo_bytecode_disassemble(expected.bytes, expected.length, &printed, &le));
    CHECK(printed.length == texts[0].length &&
          memcmp(printed.bytes, texts[0].bytes, printed.length) == 0);
    for (unsigned i = 0; i < 2; i++) {
        bool ok =
            mo_bytecode_assemble((const char *)texts[i].bytes, texts[i].length, &assembled, &ae);
        CHECK(ok);
        if (!ok)
            CHECK(fprintf(stderr, "worked text %u refused at %u: %s\n", i, ae.offset, ae.reason) >=
                  0);
        if (ok)
            CHECK(assembled.length == expected.length &&
                  memcmp(assembled.bytes, expected.bytes, expected.length) == 0);
        mo_buffer_clear(&assembled);
    }
    mo_buffer_free(&printed);
    mo_buffer_free(&assembled);
    mo_buffer_free(&expected);
    mo_buffer_free(&texts[1]);
    mo_buffer_free(&texts[0]);
}

static void worked_examples(void) {
    /* Section 3's exact recursive Chain tables, completed with an owned closure body. */
    struct fixture t;
    init(&t);
    const struct mo_bytecode_string chain_names[] = {STR("Chain"), STR("End"),  STR("Link"),
                                                     STR("value"), STR("next"), STR("Alias")};
    const struct mo_type_field link[] = {{"value", 1}, {"next", 3}}, alias[] = {{NULL, 3}};
    const struct mo_type_case chain[] = {{.name = "End"},
                                         {.name = "Link", .fields = link, .field_count = 2},
                                         {.name = "Alias", .fields = alias, .field_count = 1}};
    const uint32_t chain_arg[] = {3}, tuple[] = {1, 4}, captures[] = {3, 2}, links[] = {1, 3};
    const struct mo_type chain_types[] = {
        {.kind = MO_TYPE_UNIT},
        {.kind = MO_TYPE_INT},
        {.kind = MO_TYPE_TEXT},
        {.kind = MO_TYPE_CHOICE, .name = "Chain", .cases = chain, .case_count = 3, .defined = true},
        {.kind = MO_TYPE_LIST, .parameters = chain_arg, .parameter_count = 1},
        {.kind = MO_TYPE_TUPLE, .parameters = tuple, .parameter_count = 2},
        {.kind = MO_TYPE_FUNCTION, .parameters = chain_arg, .parameter_count = 1, .result = 1},
        {.kind = MO_TYPE_ENV, .function = 0}};
    const struct mo_layout chain_layouts[] = {
        {.kind = MO_LAYOUT_CASE, .type = 3, .case_index = 1, .fields = links, .field_count = 2},
        {.kind = MO_LAYOUT_CASE, .type = 3, .case_index = 2, .fields = chain_arg, .field_count = 1},
        {.kind = MO_LAYOUT_TUPLE, .type = 5, .fields = tuple, .field_count = 2},
        {.kind = MO_LAYOUT_CLOSURE,
         .type = 6,
         .function = 0,
         .fields = captures,
         .field_count = 2}};
    const uint32_t chain_regs[] = {3, 7, 1},
                   chain_body[] = {MO_OP_CONST, 2, 0, MO_OP_DROP, 0, MO_OP_DROP, 1, MO_OP_RET, 2};
    t.file.strings = chain_names;
    t.file.string_count = N(chain_names);
    t.file.types = chain_types;
    t.file.type_count = N(chain_types);
    t.file.layouts = chain_layouts;
    t.file.layout_count = N(chain_layouts);
    t.constants[0] = (struct mo_bytecode_constant){.type = 1, .value.immediate = {7}};
    t.file.constant_count = 1;
    t.functions[0].kind = MO_FUNCTION_CLOSURE;
    t.functions[0].register_types = chain_regs;
    t.functions[0].register_count = N(chain_regs);
    t.functions[0].capture_types = captures;
    t.functions[0].capture_count = N(captures);
    t.functions[0].result_type = 1;
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    code(&t, chain_body, N(chain_body));
    expect(&t, NULL, 0, 0);

    /* Section 10.3's indexed Demo, including its unused Hidden type and all spec metadata. */
    init(&t);
    const struct mo_bytecode_string demo_names[] = {STR("unused"),
                                                    STR("Hidden"),
                                                    STR("Visible"),
                                                    STR("value"),
                                                    STR("Demo"),
                                                    STR("Demo.test.1"),
                                                    STR("answer"),
                                                    STR("keeps the answer"),
                                                    STR("demo.mo"),
                                                    STR("Demo.identity"),
                                                    STR("n"),
                                                    STR("Demo.identity.promise.1"),
                                                    STR("result"),
                                                    STR("promise \"identity\""),
                                                    STR("result == n"),
                                                    STR("n >= 0"),
                                                    STR("Demo.identity.promise.1.allows.1"),
                                                    STR("allows n: 0, result: 0"),
                                                    STR("Demo.identity.promise.1.forbids.1"),
                                                    STR("forbids n: 1, result: 0")};
    const uint32_t int_type[] = {1}, int_pair[] = {1, 1};
    const struct mo_type_field value[] = {{"value", 1}};
    const struct mo_type demo_types[] = {
        {.kind = MO_TYPE_UNIT},
        {.kind = MO_TYPE_INT},
        {.kind = MO_TYPE_LIST, .parameters = int_type, .parameter_count = 1},
        {.kind = MO_TYPE_TUPLE, .parameters = int_pair, .parameter_count = 2},
        {.kind = MO_TYPE_RECORD,
         .name = "Hidden",
         .fields = value,
         .field_count = 1,
         .defined = true},
        {.kind = MO_TYPE_RECORD,
         .name = "Visible",
         .fields = value,
         .field_count = 1,
         .defined = true},
        {.kind = MO_TYPE_BOOL}};
    const struct mo_layout demo_layouts[] = {
        {.kind = MO_LAYOUT_RECORD, .type = 5, .fields = int_type, .field_count = 1},
        {.kind = MO_LAYOUT_TUPLE, .type = 3, .fields = int_pair, .field_count = 2}};
    t.file.strings = demo_names;
    t.file.string_count = N(demo_names);
    const struct mo_bytecode_bytes demo_files[] = {STR("demo.mo")};
    t.file.files = demo_files;
    t.file.file_count = N(demo_files);
    t.file.types = demo_types;
    t.file.type_count = N(demo_types);
    t.file.layouts = demo_layouts;
    t.file.layout_count = N(demo_layouts);
    t.file.constant_count = 5;
    t.constants[0] = (struct mo_bytecode_constant){.type = 1, .value.immediate = {7}};
    t.constants[1] = (struct mo_bytecode_constant){.type = 0};
    for (uint32_t i = 2; i < 5; i++) {
        t.constants[i] =
            (struct mo_bytecode_constant){.type = 1, .value.immediate = {(uint8_t)(i - 2)}};
    }
    const uint32_t test_regs[] = {1, 3, 2, 5, 0}, promise_regs[] = {1, 1, 6, 1};
    const uint32_t test[] = {MO_OP_CONST,
                             0,
                             0,
                             MO_OP_CONSTRUCT | (2U << 16),
                             1,
                             1,
                             0,
                             0,
                             MO_OP_DROP,
                             1,
                             MO_OP_CONSTRUCT | (1U << 16),
                             3,
                             0,
                             0,
                             MO_OP_DROP,
                             3,
                             MO_OP_CONST,
                             4,
                             1,
                             MO_OP_RET,
                             4};
    const uint32_t identity[] = {MO_OP_RET, 0};
    const uint32_t promise[] = {MO_OP_EQ_INT,
                                2,
                                1,
                                0,
                                MO_OP_BRANCH,
                                2,
                                8,
                                24,
                                MO_OP_CONST,
                                3,
                                2,
                                MO_OP_LE_INT,
                                2,
                                3,
                                0,
                                MO_OP_BRANCH,
                                2,
                                19,
                                29,
                                MO_OP_CONST,
                                3,
                                2,
                                MO_OP_RET,
                                3,
                                MO_OP_CONST,
                                3,
                                3,
                                MO_OP_RET,
                                3,
                                MO_OP_CONST,
                                3,
                                4,
                                MO_OP_RET,
                                3};
    const uint32_t allows[] = {MO_OP_CONST, 0,         2, MO_OP_CALL | (2U << 16), 1, 2, 0,
                               0,           MO_OP_RET, 1};
    const uint32_t forbids_regs[] = {1, 1, 1},
                   forbids[] = {MO_OP_CONST, 0, 3, MO_OP_CONST, 1, 2, MO_OP_CALL | (2U << 16), 2,
                                2,           0, 1, MO_OP_RET,   2};
    const uint32_t *bodies[] = {test, identity, promise, allows, forbids};
    const uint32_t sizes[] = {N(test), N(identity), N(promise), N(allows), N(forbids)};
    const uint32_t names[] = {5, 9, 11, 16, 18};
    for (uint32_t i = 0; i < 5; i++) {
        t.functions[i].name = names[i];
        t.functions[i].code = bodies[i];
        t.functions[i].code_count = sizes[i];
        t.functions[i].register_types = i == 0 ? test_regs : i == 2 ? promise_regs : forbids_regs;
        t.functions[i].register_count = i == 0 ? 5U : i == 1 ? 1U : i == 2 ? 4U : i == 3 ? 2U : 3U;
        t.functions[i].result_type = i == 0 ? 0U : 1U;
        t.functions[i].kind = i == 0   ? MO_FUNCTION_TEST
                              : i == 1 ? MO_FUNCTION_FUNCTION
                              : i == 2 ? MO_FUNCTION_PROMISE
                                       : MO_FUNCTION_ANCHOR;
        t.functions[i].spec_code = i != 1;
    }
    parameter(&t, 1, 1, MO_PARAMETER_OWNED);
    parameter(&t, 2, 2, MO_PARAMETER_OWNED);
    t.module.name = 4;
    const struct mo_bytecode_type_export exposed[] = {{4, false}, {5, true}};
    const uint32_t exports[] = {1};
    t.module.types = exposed;
    t.module.type_count = N(exposed);
    t.module.functions = exports;
    t.module.function_count = N(exports);
    t.file.function_count = 5;
    t.file.debug_count = 5;
    const struct mo_bytecode_span spans[] = {{0, 0, 10, 20}, {3, MO_NO_INDEX, 0, 0}};
    const struct mo_bytecode_register_name answer[] = {{0, 6, 3, 21}};
    const struct mo_bytecode_spec_line lines[] = {{0, 50, 60, 14}, {MO_NO_INDEX, 0, 0, 15}};
    t.debug[0] = (struct mo_bytecode_debug){.spans = spans,
                                            .span_count = 2,
                                            .names = answer,
                                            .name_count = 1,
                                            .text = 7,
                                            .file = 0,
                                            .end = 21,
                                            .subject = MO_NO_INDEX};
    t.debug[1].file = 0;
    t.debug[1].start = 22;
    t.debug[1].end = 39;
    t.debug[2] = (struct mo_bytecode_debug){.text = 13,
                                            .file = 0,
                                            .start = 40,
                                            .end = 90,
                                            .subject = 1,
                                            .lines = lines,
                                            .line_count = 2};
    t.debug[3] = (struct mo_bytecode_debug){.text = 17,
                                            .file = 0,
                                            .start = 91,
                                            .end = 110,
                                            .subject = 2,
                                            .expectation = MO_BYTECODE_ALLOWS};
    t.debug[4] = (struct mo_bytecode_debug){
        .text = 19, .file = MO_NO_INDEX, .subject = 2, .expectation = MO_BYTECODE_FORBIDS};
    expect(&t, NULL, 0, 0);
    worked_text(&t.file);
}

static void work_bound(void) {
    /* Exponentially many paths reconverge after each switch. A path-enumerating verifier or
     * a worklist that replays full vectors for each weakened case fact breaks this bound. */
    for (uint32_t n = 64; n <= 256; n *= 2) {
        struct fixture t;
        init(&t);
        uint32_t regs[258], words[1032];
        struct mo_parameter params[256];
        bool readable[258];
        for (uint32_t i = 0; i < n; i++) {
            regs[i] = 15;
            params[i] = (struct mo_parameter){15, MO_PARAMETER_BORROWED};
        }
        regs[n] = 0;
        words[0] = MO_OP_CONST;
        words[1] = n;
        words[2] = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = 3 + i * 4;
            words[at] = MO_OP_SWITCH | (2U << 16);
            words[at + 1] = i;
            words[at + 2] = at + 4;
            words[at + 3] = at + 4;
        }
        words[3 + n * 4] = MO_OP_RET;
        words[4 + n * 4] = n;
        t.functions[0].register_types = regs;
        t.functions[0].register_count = n + 1;
        t.functions[0].parameters = params;
        t.functions[0].parameter_count = n;
        code(&t, words, 5 + n * 4);
        expect(&t, NULL, 0, 0);
        struct mo_arena arena;
        struct mo_types view;
        const uint32_t *offsets;
        struct mo_buffer wire;
        struct mo_write_error we;
        struct mo_load_error le;
        mo_arena_init(&arena, (size_t)64 << 20);
        mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
        CHECK(mo_bytecode_write(&t.file, &wire, &we));
        const struct mo_bytecode *f =
            mo_bytecode_decode_info(&arena, wire.bytes, wire.length, &offsets, &view, NULL, &le);
        CHECK(f != NULL);
        if (f != NULL) {
            struct mo_verification verified;
            CHECK(mo_bytecode_verify(f, &view, offsets, wire.length, &arena, &verified, NULL, &le));
            uint64_t work = 0;
            CHECK(mo_bytecode_readable(f, &view, &verified, 0, offsets[0], 3 + n * 4, readable,
                                       &work, &le));
            for (uint32_t i = 0; i <= n; i++) {
                CHECK(readable[i]);
            }
            CHECK(work <= UINT64_C(8) * t.functions[0].code_count * (n + 1));
            CHECK(printf("verifier work: %u registers, %u words, %llu steps\n", n + 1,
                         t.functions[0].code_count, (unsigned long long)work) >= 0);
        }
        mo_arena_free(&arena);
        mo_buffer_free(&wire);
    }
}

static void instruction_refusals(void) {
    /* Each row starts from an accepted, structurally valid file and changes exactly one
     * operand. The stop deliberately avoids unrelated exit/leak checks masking a type rule. */
    const struct {
        uint32_t regs[6], words[13], count, edit, value, pc;
        const char *reason;
    } rows[] = {
        {{3, 10, 15, 10, 14, 13},
         {MO_OP_CONSTRUCT | (2U << 16), 4, 1, 0, 1, MO_OP_STOP, 3},
         7,
         1,
         5,
         0,
         "construct/reuse result type"},
        {{3, 10, 15, 10, 14, 13},
         {MO_OP_CONSTRUCT | (2U << 16), 4, 1, 0, 1, MO_OP_STOP, 3},
         7,
         3,
         2,
         0,
         "construct/reuse field type"},
        {{3, 10, 15, 10, 15, 14},
         {MO_OP_MAKE_CASE | (2U << 16), 4, 15, 1, 0, 1, MO_OP_STOP, 3},
         8,
         1,
         5,
         0,
         "make_case result type"},
        {{3, 10, 15, 10, 15, 14},
         {MO_OP_MAKE_CASE | (2U << 16), 4, 15, 1, 0, 1, MO_OP_STOP, 3},
         8,
         4,
         2,
         0,
         "make_case field type"},
        {{3, 10, 15, 10, 15, 14},
         {MO_OP_MAKE_CASE | (2U << 16), 4, 15, 1, 0, 1, MO_OP_STOP, 3},
         8,
         3,
         2,
         0,
         "make_case case index"},
        {{3, 10, 15, 10, 15, 14},
         {MO_OP_MAKE_CASE | (2U << 16), 4, 15, 1, 0, 1, MO_OP_STOP, 3},
         8,
         2,
         14,
         0,
         "make_case needs choice"},
        {{14, 10, 3, 10, 3, 10},
         {MO_OP_FIELD, 4, 0, 0, MO_OP_STOP, 3},
         6,
         1,
         5,
         0,
         "field result type"},
        {{14, 10, 3, 10, 3, 10},
         {MO_OP_FIELD, 4, 0, 0, MO_OP_STOP, 3},
         6,
         3,
         2,
         0,
         "field source or index"},
        {{15, 10, 3, 10, 3, 10},
         {MO_OP_SWITCH | (2U << 16), 0, 11, 4, MO_OP_CASE_FIELD, 4, 0, 1, 0, MO_OP_STOP, 3,
          MO_OP_STOP, 3},
         13,
         5,
         5,
         4,
         "case_field result type"},
        {{15, 10, 3, 10, 3, 10},
         {MO_OP_SWITCH | (2U << 16), 0, 11, 4, MO_OP_CASE_FIELD, 4, 0, 1, 0, MO_OP_STOP, 3,
          MO_OP_STOP, 3},
         13,
         8,
         2,
         4,
         "case_field field index"},
        {{15, 10, 3, 10, 3, 10},
         {MO_OP_SWITCH | (2U << 16), 0, 11, 4, MO_OP_CASE_FIELD, 4, 0, 1, 0, MO_OP_STOP, 3,
          MO_OP_STOP, 3},
         13,
         7,
         2,
         4,
         "case_field case index"},
        {{15, 10, 3, 10, 3, 10},
         {MO_OP_SWITCH | (2U << 16), 0, 11, 4, MO_OP_CASE_FIELD, 4, 0, 1, 0, MO_OP_STOP, 3,
          MO_OP_STOP, 3},
         13,
         6,
         2,
         4,
         "case_field needs choice"},
        {{14, 3, 15, 10, 14, 13},
         {MO_OP_SET_FIELD, 4, 0, 0, 1, MO_OP_STOP, 3},
         7,
         1,
         5,
         0,
         "set_field result type"},
        {{14, 3, 15, 10, 14, 13},
         {MO_OP_SET_FIELD, 4, 0, 0, 1, MO_OP_STOP, 3},
         7,
         4,
         2,
         0,
         "set_field value type"},
        {{14, 3, 15, 10, 14, 13},
         {MO_OP_SET_FIELD, 4, 0, 0, 1, MO_OP_STOP, 3},
         7,
         3,
         2,
         0,
         "set_field field index"},
        {{14, 3, 15, 10, 14, 13},
         {MO_OP_SET_FIELD, 4, 0, 0, 1, MO_OP_STOP, 3},
         7,
         2,
         2,
         0,
         "set_field needs record or tuple"},
        {{14, 3, 15, 10, 0, 13},
         {MO_OP_RESET, 4, 0, MO_OP_FREE_TOKEN, 4, MO_OP_STOP, 3},
         7,
         2,
         1,
         0,
         "reset source type"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_CALL | (1U << 16), 4, 1, 0, MO_OP_STOP, 3},
         6,
         1,
         5,
         0,
         "direct call result type"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_CALL | (1U << 16), 4, 1, 0, MO_OP_STOP, 3},
         6,
         3,
         2,
         0,
         "direct call argument type"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_CALL_CLOSURE | (1U << 16), 4, 0, 1, MO_OP_STOP, 3},
         6,
         1,
         5,
         0,
         "closure call result type"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_CALL_CLOSURE | (1U << 16), 4, 0, 1, MO_OP_STOP, 3},
         6,
         3,
         2,
         0,
         "closure call argument type"},
        {{3, 3, 1, 10, 3, 10}, {MO_OP_RET, 0}, 2, 1, 2, 0, "return type"},
        {{4, 4, 1, 10, 20, 3},
         {MO_OP_CMP_FLOAT, 4, 0, 1, MO_OP_STOP, 3},
         6,
         1,
         5,
         0,
         "float comparison needs Ordering"},
        {{20, 20, 1, 10, 1, 15},
         {MO_OP_EQ_CHOICE, 4, 0, 1, MO_OP_STOP, 3},
         6,
         3,
         2,
         0,
         "comparison argument type"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_MOVE, 4, 0, MO_OP_STOP, 3},
         5,
         1,
         5,
         0,
         "move/dup type mismatch"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_DUP, 4, 0, MO_OP_STOP, 3},
         5,
         1,
         5,
         0,
         "move/dup type mismatch"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_BRANCH, 2, 4, 4, MO_OP_STOP, 3},
         6,
         1,
         0,
         0,
         "branch needs Bool"},
        {{15, 3, 1, 10, 3, 10},
         {MO_OP_SWITCH | (2U << 16), 0, 4, 4, MO_OP_STOP, 3},
         6,
         1,
         1,
         0,
         "switch needs choice"},
        {{3, 3, 1, 10, 3, 10}, {MO_OP_STOP, 3}, 2, 1, 0, 0, "stop needs Text"},
        {{3, 3, 1, 10, 3, 10}, {MO_OP_CONST, 3, 3, MO_OP_FAIL, 3}, 5, 4, 0, 3, "fail needs Text"},
        {{3, 3, 1, 10, 1, 10},
         {MO_OP_EQUAL, 4, 3, 0, 1, MO_OP_STOP, 3},
         7,
         1,
         5,
         0,
         "equal result type"},
        {{3, 3, 1, 10, 1, 10},
         {MO_OP_EQUAL, 4, 3, 0, 1, MO_OP_STOP, 3},
         7,
         3,
         2,
         0,
         "equal argument type"},
        {{16, 3, 1, 10, 16, 10},
         {MO_OP_PRIM | (2U << 16), 4, MO_PRIM_LIST_APPEND, 18, 0, 1, MO_OP_STOP, 3},
         8,
         3,
         13,
         0,
         "primitive needs function type"},
        {{19, 10, 1, 10, 0, 10},
         {MO_OP_HOST | (2U << 16), 4, MO_HOST_CONSOLE_PRINT, 0, 1, MO_OP_STOP, 3},
         7,
         2,
         MO_HOST_CONSOLE_FAKE,
         0,
         "host arity"},
        {{3, 3, 1, 10, 3, 10},
         {MO_OP_CALL_CLOSURE | (1U << 16), 4, 0, 1, MO_OP_STOP, 3},
         6,
         2,
         1,
         0,
         "closure call needs function"},
    };
    for (uint32_t i = 0; i < N(rows); i++) {
        struct fixture t;
        init(&t);
        uint32_t regs[6], words[13];
        memcpy(regs, rows[i].regs, sizeof regs);
        memcpy(words, rows[i].words, sizeof words);
        t.functions[0].register_types = regs;
        t.functions[0].register_count = N(regs);
        parameter(&t, 0, 4, MO_PARAMETER_OWNED);
        uint32_t op = words[0] & MO_OPCODE_MASK;
        if (op == MO_OP_RET) {
            t.functions[0].result_type = 3;
            parameter(&t, 0, 4, MO_PARAMETER_BORROWED);
        }
        if (op == MO_OP_CMP_FLOAT || op == MO_OP_EQ_CHOICE) {
            t.file.type_count = 21;
            t.types[19] = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_FILES};
        }
        if (op == MO_OP_CALL) {
            t.file.function_count = 2;
            t.file.debug_count = 2;
            t.functions[1].register_types = int_arg;
            t.functions[1].register_count = 1;
            t.functions[1].result_type = 3;
            parameter(&t, 1, 1, MO_PARAMETER_OWNED);
            static const uint32_t ret[] = {MO_OP_RET, 0};
            t.functions[1].code = ret;
            t.functions[1].code_count = N(ret);
        }
        if (op == MO_OP_CALL_CLOSURE) {
            regs[0] = 17;
            parameter(&t, 0, 4, MO_PARAMETER_OWNED);
        }
        if (op == MO_OP_HOST) {
            t.file.type_count = 20;
            t.types[19] =
                (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_CONSOLE};
        }
        if (op == MO_OP_CONST) {
            t.functions[0].kind = MO_FUNCTION_TEST;
            t.functions[0].spec_code = true;
            parameter(&t, 0, 0, MO_PARAMETER_OWNED);
        }
        code(&t, words, rows[i].count);
        expect(&t, NULL, 0, 0);
        words[rows[i].edit] = rows[i].value;
        expect(&t, rows[i].reason, 0, rows[i].pc);
        /* Shorten exactly one argument list without breaking the following instruction. */
        bool first = true;
        for (uint32_t j = 0; j < i; j++) {
            first &= rows[j].words[0] != rows[i].words[0];
        }
        if (first && (op == MO_OP_CALL || op == MO_OP_CALL_CLOSURE || op == MO_OP_CONSTRUCT ||
                      op == MO_OP_MAKE_CASE)) {
            memcpy(words, rows[i].words, sizeof words);
            const struct mo_opcode_info *info = mo_opcode_find(op);
            MO_ASSERT(info != NULL);
            uint32_t end = 1U + info->fixed_count + (words[0] >> 16);
            memmove(words + end - 1, words + end, (rows[i].count - end) * sizeof *words);
            words[0] -= 1U << 16;
            code(&t, words, rows[i].count - 1);
            const char *reason = op == MO_OP_CALL           ? "direct call argument count"
                                 : op == MO_OP_CALL_CLOSURE ? "closure call argument count"
                                 : op == MO_OP_CONSTRUCT    ? "construct/reuse field count"
                                                            : "make_case field count";
            expect(&t, reason, 0, 0);
        }
    }
}

static void refinement_and_constraints(void) {
    struct fixture t;
    init(&t);
    const uint32_t regs[] = {15, 10, 3, 15};
    t.functions[0].register_types = regs;
    t.functions[0].register_count = N(regs);
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    const uint32_t good[] = {MO_OP_SWITCH | (2U << 16),
                             0,
                             11,
                             4,
                             MO_OP_CASE_FIELD,
                             2,
                             0,
                             1,
                             0,
                             MO_OP_STOP,
                             1,
                             MO_OP_STOP,
                             1};
    code(&t, good, N(good));
    expect(&t, NULL, 0, 0);
    const uint32_t same_block[] = {MO_OP_SWITCH | (2U << 16),
                                   0,
                                   14,
                                   4,
                                   MO_OP_MOVE,
                                   0,
                                   0,
                                   MO_OP_CASE_FIELD,
                                   2,
                                   0,
                                   1,
                                   0,
                                   MO_OP_STOP,
                                   1,
                                   MO_OP_STOP,
                                   1};
    code(&t, same_block, N(same_block));
    expect(&t, "case is not known", 0, 7);
    const uint32_t predecessor[] = {
        MO_OP_SWITCH | (2U << 16), 0, 16, 4, MO_OP_MOVE, 0,          0, MO_OP_JUMP, 9,
        MO_OP_CASE_FIELD,          2, 0,  1, 0,          MO_OP_STOP, 1, MO_OP_STOP, 1};
    code(&t, predecessor, N(predecessor));
    expect(&t, "case is not known", 0, 9);
    const uint32_t switches[] = {MO_OP_SWITCH | (2U << 16), 0, 4, 4, MO_OP_STOP, 1};
    code(&t, switches, N(switches));
    expect(&t, NULL, 0, 0);
    const uint32_t missing[] = {MO_OP_SWITCH | (1U << 16), 0, 3, MO_OP_STOP, 1};
    code(&t, missing, N(missing));
    expect(&t, "switch target count", 0, 0);

    init(&t);
    t.types[19] = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_FILES};
    t.file.type_count = 21;
    const uint32_t cmp_regs[] = {20, 1}, compare[] = {MO_OP_EQ_CHOICE, 1, 0, 0, MO_OP_RET, 1};
    t.functions[0].register_types = cmp_regs;
    t.functions[0].register_count = N(cmp_regs);
    t.functions[0].result_type = 1;
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    code(&t, compare, N(compare));
    expect(&t, NULL, 0, 0);
    struct mo_type_case cases[N(ordering)];
    memcpy(cases, ordering, sizeof cases);
    struct mo_type_field payload = {NULL, 3};
    cases[0].fields = &payload;
    cases[0].field_count = 1;
    t.types[20].cases = cases;
    expect(&t, "choice comparison has fields", 0, 0);

    init(&t);
    uint32_t element[] = {3};
    const uint32_t parameters[] = {16}, unique_regs[] = {16};
    t.types[16].parameters = element;
    t.types[19] = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = parameters, .parameter_count = 1, .result = 16};
    t.file.type_count = 20;
    t.functions[0].register_types = unique_regs;
    t.functions[0].register_count = N(unique_regs);
    t.functions[0].result_type = 16;
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    const uint32_t unique[] = {
        MO_OP_PRIM | (1U << 16), 0, MO_PRIM_LIST_UNIQUE, 19, 0, MO_OP_RET, 0};
    code(&t, unique, N(unique));
    expect(&t, NULL, 0, 0);
    element[0] = 4;
    expect(&t, "library Hash constraint", 0, 0);
}

static void hostile_work(void) {
    struct fixture t;
    init(&t);
    struct mo_arena arena;
    mo_arena_init(&arena, (size_t)64 << 20);
    const uint32_t count = MO_LIMIT_FUNCTION_REGISTERS;
    uint32_t *regs = mo_arena_allocate_array(&arena, count, sizeof *regs, _Alignof(uint32_t));
    uint32_t *words = mo_arena_allocate_array(&arena, MO_LIMIT_FUNCTION_CODE_WORDS, sizeof *words,
                                              _Alignof(uint32_t));
    bool *readable = mo_arena_allocate_array(&arena, count, sizeof *readable, _Alignof(bool));
    MO_ASSERT(regs != NULL && words != NULL && readable != NULL);
    for (uint32_t r = 0; r < count; r++) {
        regs[r] = r == 0 ? 0U : 15U;
    }
    words[0] = MO_OP_CONST;
    words[1] = 0;
    words[2] = 0;
    uint32_t pc = 3;
    for (uint32_t i = 0; i < 262000; i++) {
        words[pc++] = MO_OP_DROP;
        words[pc++] = 0;
    }
    words[pc++] = MO_OP_RET;
    words[pc++] = 0;
    t.functions[0].register_types = regs;
    t.functions[0].register_count = count;
    code(&t, words, pc);
    expect(&t, NULL, 0, 0);
    struct mo_buffer wire;
    struct mo_write_error we;
    struct mo_load_error le;
    struct mo_types view;
    struct mo_verification verified;
    const uint32_t *offsets;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_encode(&t.file, &wire, &we));
    const struct mo_bytecode *f =
        mo_bytecode_decode_info(&arena, wire.bytes, wire.length, &offsets, &view, NULL, &le);
    MO_ASSERT(f != NULL);
    CHECK(mo_bytecode_verify(f, &view, offsets, wire.length, &arena, &verified, NULL, &le));
    uint64_t work = 0;
    CHECK(mo_bytecode_readable(f, &view, &verified, 0, offsets[0], pc - 2, readable, &work, &le));
    CHECK(work < UINT64_C(4) * (pc + count));
    CHECK(readable[0] && !readable[count - 1]);
    CHECK(printf("unused-choice regression: %u registers, %u words, %llu steps\n", count, pc,
                 (unsigned long long)work) >= 0);
    mo_buffer_free(&wire);
    /* A jump chain needs no state snapshots. Test the exact product bound, then its next
     * representable value, using only unused registers and otherwise identical code. */
    for (uint32_t blocks = 2048; blocks <= 2049; blocks++) {
        t.functions[0].register_count = 2048;
        pc = 0;
        for (uint32_t b = 1; b < blocks; b++) {
            words[pc++] = MO_OP_JUMP;
            words[pc] = pc + 1;
            pc++;
        }
        words[pc++] = MO_OP_CONST;
        words[pc++] = 0;
        words[pc++] = 0;
        words[pc++] = MO_OP_RET;
        words[pc++] = 0;
        code(&t, words, pc);
        expect_kind(&t, blocks == 2048 ? NULL : "register-block limit", 0, 0, MO_BYTECODE_LIMIT);
    }
    /* Repeated two-edge joins fit the product ceiling but exhaust the wire-sized work budget.
     * The forward walk costs 264,710 steps; two 1,025-slot comparisons fit the 266,880-step
     * budget. The third, at word 1019, must fail before comparing or allocating more. */
    for (uint32_t r = 0; r < 1024; r++) {
        regs[r] = r == 0 ? 15U : 0U;
    }
    t.functions[0].register_count = 1024;
    parameter(&t, 0, 1, MO_PARAMETER_BORROWED);
    pc = 0;
    words[pc++] = MO_OP_CONST;
    words[pc++] = 1;
    words[pc++] = 0;
    for (uint32_t b = 0; b < 256; b++) {
        words[pc++] = MO_OP_SWITCH | (2U << 16);
        words[pc++] = 0;
        words[pc] = pc + 2;
        words[pc + 1] = pc + 2;
        pc += 2;
    }
    words[pc++] = MO_OP_RET;
    words[pc++] = 1;
    code(&t, words, pc);
    expect_kind(&t, "verification work limit", 0, 1019, MO_BYTECODE_LIMIT);
    mo_arena_free(&arena);
}

static void failure_kinds(void) {
    struct fixture t;
    init(&t);
    struct mo_buffer wire;
    struct mo_write_error we;
    struct mo_load_error le;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_write(&t.file, &wire, &we));
    const struct {
        uint32_t at, value;
        const char *reason;
    } ceilings[] = {{20, MO_LIMIT_STRINGS + 1U, "count past limit or remaining bytes"},
                    {24, MO_LIMIT_STRING_SIZE + 1U, "byte length past limit or remaining bytes"},
                    {position(&t, wire.length, 0, 0),
                     MO_OP_CALL | ((MO_LIMIT_BYTECODE_PARAMETERS + 1U) << 16),
                     "invalid instruction shape or instruction limit"}};
    for (uint32_t i = 0; i < N(ceilings); i++) {
        mo_buffer_clear(&wire);
        CHECK(mo_bytecode_write(&t.file, &wire, &we));
        for (unsigned j = 0; j < 4; j++) {
            wire.bytes[ceilings[i].at + j] = (uint8_t)(ceilings[i].value >> (j * 8));
        }
        uint64_t checksum = MO_BYTECODE_FNV_OFFSET;
        for (size_t j = MO_BYTECODE_HEADER_SIZE; j < wire.length; j++) {
            checksum = (checksum ^ wire.bytes[j]) * MO_BYTECODE_FNV_PRIME;
        }
        for (unsigned j = 0; j < 8; j++) {
            wire.bytes[12 + j] = (uint8_t)(checksum >> (j * 8));
        }
        CHECK(mo_load(wire.bytes, wire.length, &le) == NULL);
        CHECK(le.kind == MO_BYTECODE_LIMIT && le.offset == ceilings[i].at);
        CHECK(strcmp(le.reason, ceilings[i].reason) == 0);
    }
    wire.bytes[0] = 0;
    CHECK(mo_load(wire.bytes, wire.length, &le) == NULL);
    CHECK(le.kind == MO_BYTECODE_INVALID && le.offset == 0);
    CHECK(mo_load(wire.bytes, (size_t)MO_LIMIT_BYTECODE_SIZE + 1, &le) == NULL);
    CHECK(le.kind == MO_BYTECODE_LIMIT && le.offset == 0);
    mo_buffer_free(&wire);
    mo_buffer_init(&wire, 0);
    CHECK(!mo_bytecode_write(&t.file, &wire, &we));
    CHECK(we.kind == MO_BYTECODE_LIMIT && wire.length == 0);
    mo_buffer_free(&wire);
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    t.file.function_count = MO_LIMIT_FUNCTIONS + 1;
    CHECK(!mo_bytecode_write(&t.file, &wire, &we));
    CHECK(we.kind == MO_BYTECODE_LIMIT && wire.length == 0);
    t.file.function_count = 1;
    /* Inject allocation failure through the buffer's arena, not the output byte ceiling. */
    mo_buffer_free(&wire);
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    mo_arena_init(&wire.arena, 0);
    CHECK(!mo_bytecode_write(&t.file, &wire, &we));
    CHECK(we.kind == MO_BYTECODE_NO_MEMORY && wire.length == 0);
    mo_buffer_free(&wire);
}

static void simulation_without_services(void) {
    struct fixture t;
    init(&t);
    t.types[19] = (struct mo_type){.kind = MO_TYPE_CAPABILITY, .capability = MO_CAP_SIMULATION};
    t.file.type_count = 20;
    const uint32_t registers[] = {19, 7, 0};
    t.functions[0].register_types = registers;
    t.functions[0].register_count = N(registers);
    parameter(&t, 0, 2, MO_PARAMETER_BORROWED);
    for (uint32_t op = MO_SIM_ADVANCE; op <= MO_SIM_RESTART; op++) {
        uint32_t controls[] = {MO_OP_SIM | (1U << 16), 2, op, 0, 0, 0, MO_OP_RET, 2};
        const uint32_t advance[] = {
            MO_OP_SIM | (2U << 16), 2, MO_SIM_ADVANCE, 0, 0, 0, 1, MO_OP_RET, 2};
        if (op == MO_SIM_ADVANCE)
            code(&t, advance, N(advance));
        else
            code(&t, controls, N(controls));
        expect(&t, NULL, 0, 0);
    }
    const uint32_t start_regs[] = {6, 19, 0};
    const uint32_t start[] = {MO_OP_CONST, 0,
                              4,           MO_OP_SIM | (1U << 16),
                              1,           MO_SIM_START,
                              0,           0,
                              0,           MO_OP_DROP,
                              1,           MO_OP_CONST,
                              2,           0,
                              MO_OP_RET,   2};
    t.functions[0].register_types = start_regs;
    t.functions[0].kind = MO_FUNCTION_TEST;
    t.functions[0].spec_code = true;
    t.constants[4] = (struct mo_bytecode_constant){.type = 6};
    t.file.constant_count = 5;
    parameter(&t, 0, 0, MO_PARAMETER_OWNED);
    code(&t, start, N(start));
    expect(&t, "simulation start descriptor", 0, 3);

    t.types[20] = (struct mo_type){.kind = MO_TYPE_ENV, .function = 0};
    t.file.type_count = 21;
    const uint32_t send_regs[] = {19, 20, 0};
    const uint32_t send[] = {MO_OP_SIM | (1U << 16),
                             2,
                             MO_SIM_SEND,
                             0,
                             0,
                             0,
                             MO_OP_DROP,
                             0,
                             MO_OP_DROP,
                             1,
                             MO_OP_RET,
                             2};
    t.functions[0].register_types = send_regs;
    t.functions[0].kind = MO_FUNCTION_CLOSURE;
    parameter(&t, 0, 1, MO_PARAMETER_OWNED);
    code(&t, send, N(send));
    expect(&t, "simulation send descriptor", 0, 0);
}

static void service_refusal(void) {
    struct fixture t;
    init(&t);
    const struct mo_bytecode_service service = {.start = 99};
    t.file.service_count = 1;
    t.file.services = &service;
    struct mo_buffer wire;
    struct mo_write_error error;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(!mo_bytecode_write(&t.file, &wire, &error));
    CHECK(wire.length == 0);
    mo_buffer_free(&wire);
}

int main(int argc, char **argv) {
    MO_ASSERT(argc == 1 || (argc == 3 && strcmp(argv[1], "--corpus") == 0));
    if (argc == 3) {
        corpus = argv[2];
    }
    basics();
    memory();
    clears();
    arithmetic();
    choices_and_joins();
    data();
    calls();
    closure_signatures();
    closures();
    primitives();
    abi_shapes();
    globals();
    closure_initialization();
    abilities();
    linked_promise();
    worked_examples();
    instruction_refusals();
    refinement_and_constraints();
    work_bound();
    hostile_work();
    failure_kinds();
    simulation_without_services();
    service_refusal();
    return mo_test_report("bytecode verifier");
}
