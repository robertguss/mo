#include "base/buffer.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void binding(const struct mo_checked_body *body, const struct mo_checked_binding *value) {
    uintptr_t offset = (uintptr_t)value - (uintptr_t)body->bindings;
    MO_ASSERT(offset % sizeof(*value) == 0 && offset / sizeof(*value) < body->binding_count);
    MO_ASSERT(value->type != NULL && value->type->parameters == 0);
}

static void pattern(const struct mo_checked_body *body, const struct mo_checked_pattern *value,
                    uint32_t depth) {
    MO_ASSERT(depth < MO_LIMIT_SOURCE_TYPE_DEPTH);
    /* A flat source list becomes a cons spine; don't recurse down that spine. */
    while (value != NULL) {
        MO_ASSERT(value->type != NULL && value->type->parameters == 0);
        if (value->kind == MO_PATTERN_BIND) {
            MO_ASSERT(value->binding != NULL);
            binding(body, value->binding);
        }
        const struct mo_checked_pattern *tail = NULL;
        for (uint32_t i = 0; i < value->part_count; i++) {
            MO_ASSERT(value->parts[i].pattern != NULL);
            if (value->kind == MO_PATTERN_LIST && value->parts[i].index == 1)
                tail = value->parts[i].pattern;
            else
                pattern(body, value->parts[i].pattern, depth + 1);
        }
        value = tail;
    }
}

static void graph(const struct mo_program *program) {
    MO_ASSERT(program->entry.function < program->function_count);
    for (uint32_t f = 0; f < program->function_count; f++) {
        const struct mo_checked_function *function = &program->functions[f];
        const struct mo_checked_body *body = &function->body;
        MO_ASSERT(body->root < body->count);
        for (uint32_t i = 0; i < function->parameter_count; i++)
            binding(body, function->parameters[i]);
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            MO_ASSERT(row->syntax != NULL && row->type != NULL && row->type->parameters == 0);
            for (uint32_t j = 0; j < row->operand_count; j++)
                MO_ASSERT(row->operands[j] < i);
            if (row->binding != NULL)
                binding(body, row->binding);
            if (row->assignment)
                MO_ASSERT(row->binding != NULL && row->binding->variable);
            if (row->receiver != UINT32_MAX)
                MO_ASSERT(row->receiver < i);
            for (uint32_t j = 0; j < row->ability_count; j++)
                MO_ASSERT(row->abilities[j].operand < i &&
                          row->abilities[j].implementation != NULL);
            for (uint32_t j = 0; j < row->arguments.count; j++)
                MO_ASSERT(row->arguments.items[j].parameter->kind == MO_SOURCE_PARAMETER &&
                          row->arguments.items[j].type->parameters == 0);
            if (row->pattern != NULL)
                pattern(body, row->pattern, 0);
            if (row->match != NULL) {
                MO_ASSERT(row->match->scrutinee < i);
                for (uint32_t j = 0; j < row->match->arm_count; j++) {
                    const struct mo_checked_arm *arm = &row->match->arms[j];
                    MO_ASSERT(arm->body < i && (arm->guard == UINT32_MAX || arm->guard < i));
                    for (uint32_t k = 0; k < arm->pattern_count; k++)
                        pattern(body, arm->patterns[k], 0);
                }
            }
            if (row->lambda != NULL) {
                MO_ASSERT(row->lambda->body < i && row->lambda->number != 0 &&
                          row->lambda->capture_count == 0);
                for (uint32_t j = 0; j < row->lambda->parameter_count; j++)
                    binding(body, row->lambda->parameters[j]);
            }
        }
    }
}

/* Only the generated arithmetic control uses this evaluator. Its expected
 * value is calculated from the bytes, not from the graph or its syntax. */
static MO_MUST_USE int64_t arithmetic(const struct mo_checked_body *body) {
    int64_t values[64] = {0};
    MO_ASSERT(body->count <= 64);
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_checked_expression *row = &body->items[i];
        if (row->syntax->kind == MO_S_INT) {
            for (uint32_t j = 0; j < row->syntax->span.end - row->syntax->span.start; j++)
                values[i] = values[i] * 10 + (int64_t)(row->syntax->text[j] - '0');
        } else if (row->operation != MO_T_EOF) {
            MO_ASSERT(row->operand_count == 2);
            int64_t a = values[row->operands[0]], b = values[row->operands[1]];
            if (row->operation == MO_T_PLUS)
                values[i] = a + b;
            else if (row->operation == MO_T_MINUS)
                values[i] = a - b;
            else {
                MO_ASSERT(row->operation == MO_T_STAR);
                values[i] = a * b;
            }
        } else {
            MO_ASSERT(row->operand_count != 0);
            values[i] = values[row->operands[row->operand_count - 1]];
        }
    }
    return values[body->root];
}

static void check(const uint8_t *bytes, size_t size, size_t limit, int expected, int64_t result) {
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena syntax, arena;
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    mo_arena_init(&syntax, 64 << 20);
    mo_arena_init(&arena, limit);
    bool reached = false;
    uint32_t file;
    struct mo_source_error error;
    if (!mo_sources_add_bytes(&sources, "main.mo", bytes, size, &file, &error))
        goto done;
    struct mo_module_syntax *module = mo_parse(&syntax, &sources, file, &diags);
    if (module == NULL || mo_diags_any(&diags) || module->header.uses->count != 0)
        goto done;
    reached = true;
    uint32_t order = 0, *uses = NULL;
    struct mo_module_set set = {.count = 1, .modules = &module, .order = &order, .uses = &uses};
    struct mo_program *program = mo_check(&arena, &set, &diags);
    MO_ASSERT((program == NULL) == mo_diags_any(&diags));
    if (expected >= 0)
        MO_ASSERT((program != NULL) == (expected != 0));
    if (program != NULL) {
        graph(program);
        if (expected == 1)
            MO_ASSERT(arithmetic(&program->functions[program->entry.function].body) == result);
    }
done:
    MO_ASSERT(expected < 0 || reached);
    mo_arena_free(&arena);
    mo_arena_free(&syntax);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

struct input {
    const uint8_t *bytes;
    size_t size;
};

static void run(void *argument) {
    const struct input *input = argument;
    uint8_t values[4] = {0};
    for (size_t i = 0; i < 4 && i < input->size; i++)
        values[i] = input->bytes[i];
    check(input->bytes, input->size, 32 << 20, -1, 0);
    if ((values[0] & 1) != 0)
        check(input->bytes, input->size, (size_t)values[0] * input->size, -1, 0);
    struct mo_buffer source;
    mo_buffer_init(&source, 4096);
    MO_ASSERT(mo_buffer_append_text(&source, "module Main\nfn main() : "));
    bool valid = (values[0] & 2) == 0;
    MO_ASSERT(mo_buffer_append_text(&source, valid ? "Int\n(" : "Text\n("));
    MO_ASSERT(mo_buffer_append_uint(&source, values[0]));
    MO_ASSERT(mo_buffer_append_text(&source, " + "));
    MO_ASSERT(mo_buffer_append_uint(&source, values[1]));
    MO_ASSERT(mo_buffer_append_text(&source, ") * "));
    MO_ASSERT(mo_buffer_append_uint(&source, values[2]));
    MO_ASSERT(mo_buffer_append_text(&source, " - "));
    MO_ASSERT(mo_buffer_append_uint(&source, values[3]));
    MO_ASSERT(mo_buffer_append_text(&source, "\nend\n"));
    int64_t result = ((int64_t)values[0] + values[1]) * values[2] - values[3];
    check(source.bytes, source.length, 32 << 20, valid ? 1 : 0, result);
    mo_buffer_free(&source);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1 << 20))
        return 0;
    struct input input = {data, size};
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, &input) == 0);
    return 0;
}
