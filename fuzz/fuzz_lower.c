#include "../tests/lower_fixture.h"
#include "base/codes.h"
#include "base/stack.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct source_input {
    const uint8_t *bytes;
    size_t size;
};

static void source_specs(void *argument) {
    const struct source_input *input = argument;
    struct mo_buffer text;
    mo_buffer_init(&text, 4096);
    MO_ASSERT(mo_buffer_append_text(&text, "module Main\ncomputed = source + 1\nsource = "));
    MO_ASSERT(mo_buffer_append_uint(&text, input->size == 0 ? 0 : input->bytes[0]));
    MO_ASSERT(mo_buffer_append_text(
        &text, "\nreader = fn() Some(try Some(1)) end\n"
               "fn pair(left: Z, right: A) : (Z, A) "
               "where Z: Number, Z: Equal, A: Equal\n"
               "promise \"scoped requirements\"\nresult.0 == left + left\nresult.1 == right\n"));
    size_t count = input->size < 4 ? input->size : 4;
    for (size_t i = 0; i <= count + 1; i++) {
        uint8_t byte = i < count ? input->bytes[i] : 0;
        bool decimal = (byte & 1) != 0, boolean = (byte & 2) != 0;
        const char *right = boolean ? "true" : "\"λ\"";
        MO_ASSERT(mo_buffer_append_text(&text, i == count + 1 ? "forbids result: ("
                                                              : "allows result: (") &&
                  mo_buffer_append_uint(&text, (uint32_t)byte * 2 + (i == count + 1)) &&
                  mo_buffer_append_text(&text, decimal ? ".5, " : ", ") &&
                  mo_buffer_append_text(&text, right) &&
                  mo_buffer_append_text(&text, "), right: ") &&
                  mo_buffer_append_text(&text, right) && mo_buffer_append_text(&text, ", left: ") &&
                  mo_buffer_append_uint(&text, byte) &&
                  mo_buffer_append_text(&text, decimal ? ".25\n" : "\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&text, "end\n(left + left, right)\nend\n"
                                           "test \"late concrete use\"\n"
                                           "assert pair(3, false) == (6, false)\n"
                                           "assert computed == source + 1\n"
                                           "assert (reader() or 9) == 1\nend\n"
                                           "fn main() : Int\n17\nend\n"));
    struct mo_arena frontend, output;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&frontend, 32 << 20);
    mo_arena_init(&output, 32 << 20);
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "main.mo", text.bytes, text.length, &file, &error));
    struct mo_module_syntax *module = mo_parse(&frontend, &sources, file, &diags);
    MO_ASSERT(module != NULL && !mo_diags_any(&diags));
    const char *name = "Main";
    struct mo_module_set set = {
        .count = 1, .modules = &module, .names = &name, .sources = &sources};
    struct mo_program *program = mo_check(&frontend, &set, &diags);
    MO_ASSERT(program != NULL && !mo_diags_any(&diags));
    struct mo_core *core = mo_lower(&output, program, &sources, &diags);
    MO_ASSERT(core != NULL && !mo_diags_any(&diags));
    mo_arena_free(&frontend);
    mo_sources_free(&sources);
    MO_ASSERT(mo_core_check(core, &diags)); /* Including spec metadata after frontend release. */
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_CORE_TEXT_SIZE);
    MO_ASSERT(mo_core_print(core, &wire));
    mo_buffer_free(&wire);
    mo_arena_free(&output);
    mo_diags_free(&diags);
    mo_buffer_free(&text);
}

/* Generate the certified graph shapes, with fresh rows for every occurrence.
 * Every accepted document must check and print; a refusal there is a lower bug. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct lower_fixture f;
    lower_fixture_init(&f, "module Main\nfn main() : Int\nif true: 0 + 1 + 7 else: 0\nend\n");
    const char *spellings[] = {"0", "1", "7"};
    const struct mo_source_type *parts[] = {&f.int_, &f.int_};
    struct mo_source_type tuple = {.kind = MO_SOURCE_TUPLE, .count = 2, .arguments = parts};
    struct mo_source_type list = {.kind = MO_SOURCE_NAMED,
                                  .name = "List",
                                  .module = UINT32_MAX,
                                  .count = 1,
                                  .arguments = parts};
    struct mo_source_type option = list;
    option.name = "Option";
    struct mo_source_type function = {.kind = MO_SOURCE_FUNCTION, .count = 2, .arguments = parts};
    struct mo_checked_pattern patterns[16][3] = {0};
    struct mo_checked_pattern_part pattern_parts[16] = {0};
    const struct mo_checked_pattern *alternatives[16][2] = {{0}};
    struct mo_checked_arm arms[16][2] = {0};
    struct mo_checked_match matches[16] = {0};
    struct mo_checked_lambda lambdas[16] = {0};
    const struct mo_checked_binding *ignored = NULL;
    struct mo_syntax *guards[16][1] = {{0}};
    f.bindings[0] = (struct mo_checked_binding){.name = "n", .type = &f.int_, .variable = true};
    f.functions[0].body.binding_count = 1;
    uint32_t zero = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
    uint32_t start = lower_fixture_bind(&f, 0, zero, false);
    uint32_t last = lower_fixture_row(&f, MO_S_INT, &f.int_, "0", 0, NULL);
    size_t count = size < 16 ? size : 16;
    for (size_t i = 0; i < count; i++) {
        uint32_t right = lower_fixture_row(&f, MO_S_INT, &f.int_, spellings[data[i] % 3], 0, NULL);
        unsigned shape = (data[i] >> 3) % 7;
        if (shape == 0) {
            const enum mo_syntax_token ops[] = {MO_T_PLUS, MO_T_MINUS, MO_T_STAR, MO_T_SLASH};
            last = lower_fixture_op(&f, ops[data[i] & 3], &f.int_, last, right);
        } else if (shape == 1) {
            uint32_t left = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
            uint32_t condition =
                lower_fixture_op(&f, data[i] & 1 ? MO_T_LT : MO_T_EQ, &f.bool_, left, right);
            uint32_t flag = lower_fixture_row(&f, MO_S_TRUE, &f.bool_, "true", 0, NULL);
            const enum mo_syntax_token ops[] = {MO_T_AND, MO_T_OR, MO_T_IMPLIES};
            condition = lower_fixture_op(&f, ops[data[i] % 3], &f.bool_, condition, flag);
            uint32_t other = lower_fixture_row(&f, MO_S_INT, &f.int_, "7", 0, NULL);
            uint32_t branches[] = {condition, last, other};
            last = lower_fixture_row(&f, MO_S_IF, &f.int_, NULL, 3, branches);
        } else if (shape == 2) {
            uint32_t values[] = {last, right};
            uint32_t pair = lower_fixture_row(&f, MO_S_TUPLE, &tuple, NULL, 2, values);
            last = lower_fixture_row(&f, MO_S_FIELD, &f.int_, NULL, 1, &pair);
            f.rows[last].field = data[i] & 1;
        } else if (shape == 3) {
            uint32_t some = lower_fixture_row(&f, MO_S_CALL, &option, NULL, 1, &last);
            f.rows[some].native = "Some";
            uint32_t yes = right;
            uint32_t no = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
            last = lower_fixture_row(&f, MO_S_CASE, &f.int_, NULL, 0, NULL);
            patterns[i][0] = (struct mo_checked_pattern){
                .kind = MO_PATTERN_ANY, .syntax = &f.syntax[last], .type = &f.int_};
            pattern_parts[i] = (struct mo_checked_pattern_part){0, &patterns[i][0]};
            patterns[i][1] = (struct mo_checked_pattern){.kind = MO_PATTERN_CONSTRUCTOR,
                                                         .syntax = &f.syntax[last],
                                                         .type = &option,
                                                         .constructor.builtin = "Some",
                                                         .part_count = 1,
                                                         .parts = &pattern_parts[i]};
            patterns[i][2] = (struct mo_checked_pattern){.kind = MO_PATTERN_CONSTRUCTOR,
                                                         .syntax = &f.syntax[last],
                                                         .type = &option,
                                                         .constructor.builtin = "None"};
            for (size_t j = 0; j < 2; j++) {
                alternatives[i][j] = &patterns[i][j + 1];
                arms[i][j] = (struct mo_checked_arm){.syntax = &f.syntax[last],
                                                     .pattern_count = 1,
                                                     .patterns = &alternatives[i][j],
                                                     .guard = UINT32_MAX,
                                                     .body = j == 0 ? yes : no};
            }
            matches[i] = (struct mo_checked_match){some, 2, arms[i]};
            f.rows[last].match = &matches[i];
        } else if (shape == 4) {
            uint32_t values[] = {last, right};
            uint32_t iterable = lower_fixture_row(&f, MO_S_LIST, &list, NULL, 2, values);
            uint32_t read = lower_fixture_read(&f, 0);
            uint32_t one = lower_fixture_row(&f, MO_S_INT, &f.int_, "1", 0, NULL);
            uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, read, one);
            uint32_t body = lower_fixture_bind(&f, 0, sum, true);
            uint32_t operands[] = {iterable, body};
            uint32_t loop = lower_fixture_row(&f, MO_S_FOR, &f.unit_, NULL, 2, operands);
            patterns[i][0] = (struct mo_checked_pattern){
                .kind = MO_PATTERN_ANY, .syntax = &f.syntax[loop], .type = &f.int_};
            f.rows[loop].pattern = &patterns[i][0];
            uint32_t result = lower_fixture_read(&f, 0);
            uint32_t sequence[] = {loop, result};
            last = lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, sequence);
        } else if (shape == 5) {
            uint32_t lambda = lower_fixture_row(&f, MO_S_LAMBDA, &function, NULL, 1, &right);
            lambdas[i] = (struct mo_checked_lambda){.number = (uint32_t)i + 1,
                                                    .parameter_count = 1,
                                                    .parameters = &ignored,
                                                    .body = right};
            f.rows[lambda].lambda = &lambdas[i];
            uint32_t args[] = {lambda, last};
            last = lower_fixture_row(&f, MO_S_CALL, &f.int_, NULL, 2, args);
        } else {
            uint32_t read = lower_fixture_read(&f, 0);
            uint32_t sum = lower_fixture_op(&f, MO_T_PLUS, &f.int_, read, last);
            uint32_t write = lower_fixture_bind(&f, 0, sum, true);
            uint32_t guard = lower_fixture_row(&f, MO_S_TRUE, &f.bool_, "true", 0, NULL);
            uint32_t stop = lower_fixture_row(&f, data[i] & 1 ? MO_S_BREAK : MO_S_CONTINUE,
                                              &f.unit_, NULL, 1, &guard);
            guards[i][0] = &f.syntax[guard];
            f.syntax[stop].children = guards[i];
            uint32_t statements[] = {write, stop};
            uint32_t body = lower_fixture_row(&f, MO_S_SEQUENCE, &f.unit_, NULL, 2, statements);
            uint32_t left = lower_fixture_read(&f, 0);
            uint32_t test = lower_fixture_op(&f, MO_T_LT, &f.bool_, left, right);
            uint32_t operands[] = {test, body};
            uint32_t loop = lower_fixture_row(&f, MO_S_WHILE, &f.unit_, NULL, 2, operands);
            uint32_t result = lower_fixture_read(&f, 0);
            uint32_t sequence[] = {loop, result};
            last = lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, sequence);
        }
    }
    uint32_t statements[] = {start, last};
    (void)lower_fixture_row(&f, MO_S_SEQUENCE, &f.int_, NULL, 2, statements);
    uint32_t parents[256] = {0};
    for (uint32_t i = 0; i < f.count; i++) {
        const struct mo_checked_expression *row = &f.rows[i];
        for (uint32_t j = 0; j < row->operand_count; j++)
            MO_ASSERT(++parents[row->operands[j]] == 1);
        if (row->match != NULL) {
            MO_ASSERT(++parents[row->match->scrutinee] == 1);
            for (uint32_t j = 0; j < row->match->arm_count; j++) {
                const struct mo_checked_arm *arm = &row->match->arms[j];
                MO_ASSERT(++parents[arm->body] == 1);
                if (arm->guard != UINT32_MAX)
                    MO_ASSERT(++parents[arm->guard] == 1);
            }
        }
    }
    struct mo_core *core = mo_lower(&f.arena, &f.program, &f.sources, &f.diags);
    if (core != NULL) {
        MO_ASSERT(mo_core_check(core, &f.diags));
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
        bool printed = mo_core_print(core, &text); /* Codec bounds are independent of lowering. */
        (void)printed;
        mo_buffer_free(&text);
    } else {
        MO_ASSERT(f.diags.count != 0);
        uint16_t code = f.diags.items[0].code;
        MO_ASSERT(code == MO_CODE_LOWER_DEPTH || code == MO_CODE_LOWER_WORK ||
                  code == MO_CODE_LOWER_NO_MEMORY);
    }
    lower_fixture_free(&f);
    struct source_input input = {data, size};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, source_specs, &input) == 0);
    return 0;
}
