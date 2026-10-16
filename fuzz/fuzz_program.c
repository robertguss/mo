#include "base/buffer.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "check/native.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void binding(const struct mo_checked_body *body, const struct mo_checked_binding *value,
                    uint32_t parameters) {
    uintptr_t offset = (uintptr_t)value - (uintptr_t)body->bindings;
    MO_ASSERT(offset % sizeof(*value) == 0 && offset / sizeof(*value) < body->binding_count);
    MO_ASSERT(value->type != NULL && (value->type->parameters & ~parameters) == 0);
}

static void requirements(const struct mo_program *program,
                         const struct mo_type_arguments *arguments, uint32_t count,
                         const struct mo_checked_ability_use *uses) {
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_checked_ability_use *use = &uses[i];
        if (use->operand != UINT32_MAX)
            continue;
        MO_ASSERT(use->parameter != NULL && use->implementation != NULL);
        bool found = false;
        for (uint32_t j = 0; j < arguments->count; j++)
            found = found || arguments->items[j].parameter == use->parameter;
        MO_ASSERT(found);
        for (uint32_t j = 0; j < i; j++)
            MO_ASSERT(uses[j].operand != UINT32_MAX || use->parameter != uses[j].parameter ||
                      use->implementation->ability != uses[j].implementation->ability);
    }
    const struct mo_signatures *s = program->signatures;
    for (uint32_t i = 0; i < arguments->count; i++) {
        const struct mo_source_type *parameter = arguments->items[i].parameter;
        const struct mo_symbol *owner = parameter->declaration;
        unsigned letter = (unsigned)(parameter->name[0] - 'A'), mask = 0;
        MO_ASSERT(letter < 26);
        for (uint32_t j = 0; j < s->count; j++)
            if (s->items[j].function == owner)
                mask = s->items[j].requirements[letter];
        for (uint32_t j = 0; j < s->native_count; j++)
            if (&s->native[j].owner == owner)
                mask = s->native[j].requirements[letter];
        for (uint32_t j = 0; j < s->data_count; j++)
            if (s->data[j].type->declaration == owner &&
                (s->data[j].key_parameters & parameter->parameters) != 0)
                mask = (1u << MO_SOURCE_HASH) | (1u << MO_SOURCE_EQUAL);
        for (unsigned ability = 0; ability < 5; ability++) {
            uint32_t found = 0;
            for (uint32_t j = 0; j < count; j++)
                found += uses[j].operand == UINT32_MAX && uses[j].parameter == parameter &&
                                 uses[j].implementation->ability == (enum mo_source_ability)ability
                             ? 1u
                             : 0u;
            MO_ASSERT(found == ((mask & (1u << ability)) != 0 ? 1u : 0u));
        }
    }
}

static void pattern(const struct mo_program *program, const struct mo_checked_body *body,
                    const struct mo_checked_pattern *value, uint32_t parameters, uint32_t depth) {
    MO_ASSERT(depth < MO_LIMIT_SOURCE_TYPE_DEPTH);
    /* A flat source list becomes a cons spine; don't recurse down that spine. */
    while (value != NULL) {
        MO_ASSERT(value->type != NULL && (value->type->parameters & ~parameters) == 0);
        requirements(program, &value->arguments, value->ability_count, value->abilities);
        if (value->kind == MO_PATTERN_BIND) {
            MO_ASSERT(value->binding != NULL);
            binding(body, value->binding, parameters);
        }
        const struct mo_checked_pattern *tail = NULL;
        for (uint32_t i = 0; i < value->part_count; i++) {
            MO_ASSERT(value->parts[i].pattern != NULL);
            if (value->kind == MO_PATTERN_LIST && value->parts[i].index == 1)
                tail = value->parts[i].pattern;
            else
                pattern(program, body, value->parts[i].pattern, parameters, depth + 1);
        }
        value = tail;
    }
}

static void graph(const struct mo_program *program) {
    MO_ASSERT(program->entry.function == UINT32_MAX ||
              program->entry.function < program->function_count);
    uint32_t nonconstants = program->function_count + program->spec_count;
    for (uint32_t f = 0; f < nonconstants + program->constant_count; f++) {
        const struct mo_checked_function *function =
            f < program->function_count ? &program->functions[f] : NULL;
        const struct mo_checked_spec *spec = function == NULL && f < nonconstants
                                                 ? &program->specs[f - program->function_count]
                                                 : NULL;
        const struct mo_checked_constant *constant =
            f >= nonconstants ? &program->constants[f - nonconstants] : NULL;
        const struct mo_checked_body *body = function != NULL ? &function->body
                                             : spec != NULL   ? &spec->body
                                                              : &constant->initializer;
        uint32_t parameters = function != NULL ? function->signature->type->parameters
                              : spec != NULL && spec->kind == MO_SPEC_PROMISE
                                  ? program->functions[spec->subject].signature->type->parameters
                                  : 0;
        MO_ASSERT(body->root < body->count ||
                  (spec != NULL && spec->kind == MO_SPEC_ANCHOR && body->root == UINT32_MAX));
        if (function != NULL)
            for (uint32_t i = 0; i < function->parameter_count; i++)
                binding(body, function->parameters[i], parameters);
        else if (spec != NULL) {
            MO_ASSERT(spec->module < program->set->count && spec->number != 0);
            MO_ASSERT(spec->subject < program->function_count ||
                      (spec->kind == MO_SPEC_TEST && spec->subject == UINT32_MAX));
            MO_ASSERT(spec->text.length == spec->text.span.end - spec->text.span.start);
            for (uint32_t i = 0; i < spec->parameter_count; i++)
                binding(body, spec->parameters[i], parameters);
            if (spec->kind == MO_SPEC_ANCHOR) {
                MO_ASSERT(spec->promise < program->spec_count &&
                          program->specs[spec->promise].kind == MO_SPEC_PROMISE &&
                          spec->value_count == program->specs[spec->promise].parameter_count);
                requirements(program, &spec->arguments, spec->ability_count, spec->abilities);
                for (uint32_t i = 0; i < spec->value_count; i++)
                    MO_ASSERT(spec->values[i] < body->count);
            } else {
                MO_ASSERT(strcmp(body->items[body->root].type->name,
                                 spec->kind == MO_SPEC_TEST ? "Unit" : "Bool") == 0);
                for (uint32_t i = 0; i < spec->condition_count; i++) {
                    const struct mo_checked_condition *condition = &spec->conditions[i];
                    MO_ASSERT(condition->expression < body->count &&
                              strcmp(body->items[condition->expression].type->name, "Bool") == 0 &&
                              condition->text.length ==
                                  condition->text.span.end - condition->text.span.start);
                }
            }
        } else {
            MO_ASSERT(constant->type == body->items[body->root].type &&
                      constant->type->parameters == 0 &&
                      constant->symbol->declaration->kind == MO_S_CONST);
            for (uint32_t i = 0; i < f - nonconstants; i++)
                MO_ASSERT(program->constants[i].symbol != constant->symbol);
        }
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            MO_ASSERT(row->syntax != NULL && row->type != NULL &&
                      (row->type->parameters & ~parameters) == 0);
            for (uint32_t j = 0; j < row->operand_count; j++)
                MO_ASSERT(row->operands[j] < i);
            if (row->binding != NULL)
                binding(body, row->binding, parameters);
            if (row->assignment)
                MO_ASSERT(row->binding != NULL && row->binding->variable);
            if (row->receiver != UINT32_MAX)
                MO_ASSERT(row->receiver < i);
            requirements(program, &row->arguments, row->ability_count, row->abilities);
            for (uint32_t j = 0; j < row->ability_count; j++) {
                const struct mo_checked_ability_use *use = &row->abilities[j];
                if (use->operand != UINT32_MAX)
                    MO_ASSERT(use->operand < i && use->parameter == NULL &&
                              use->implementation != NULL);
            }
            for (uint32_t j = 0; j < row->arguments.count; j++)
                MO_ASSERT(row->arguments.items[j].parameter->kind == MO_SOURCE_PARAMETER &&
                          (row->arguments.items[j].type->parameters & ~parameters) == 0);
            if (row->pattern != NULL)
                pattern(program, body, row->pattern, parameters, 0);
            if (row->match != NULL) {
                MO_ASSERT(row->match->scrutinee < i);
                for (uint32_t j = 0; j < row->match->arm_count; j++) {
                    const struct mo_checked_arm *arm = &row->match->arms[j];
                    MO_ASSERT(arm->body < i && (arm->guard == UINT32_MAX || arm->guard < i));
                    for (uint32_t k = 0; k < arm->pattern_count; k++)
                        pattern(program, body, arm->patterns[k], parameters, 0);
                }
            }
            if (row->operation == MO_T_OR) {
                MO_ASSERT(row->operand_count == 2 &&
                          row->never == body->items[row->operands[0]].never);
                const struct mo_source_type *left = body->items[row->operands[0]].type;
                MO_ASSERT(row->or_kind == MO_CHECK_OR_BOOL || row->or_kind == MO_CHECK_OR_OPTION);
                MO_ASSERT(
                    strcmp(left->name, row->or_kind == MO_CHECK_OR_BOOL ? "Bool" : "Option") == 0);
            } else
                MO_ASSERT(row->or_kind == MO_CHECK_OR_ABSENT);
            if (row->try_ != NULL) {
                MO_ASSERT(row->operand_count == 1);
                const struct mo_source_type *input = body->items[row->operands[0]].type;
                const struct mo_checked_try *try_ = row->try_;
                bool option = strcmp(input->name, "Option") == 0;
                MO_ASSERT(option || strcmp(input->name, "Result") == 0);
                MO_ASSERT(strcmp(try_->result->name, input->name) == 0);
                MO_ASSERT(strcmp(try_->failure.builtin, option ? "None" : "Error") == 0);
                MO_ASSERT(row->never == body->items[row->operands[0]].never);
                requirements(program, &try_->arguments, try_->ability_count, try_->abilities);
                for (uint32_t j = 0; j < try_->arguments.count; j++)
                    MO_ASSERT((try_->arguments.items[j].type->parameters & ~parameters) == 0);
            }
            if (row->lambda != NULL) {
                MO_ASSERT(row->lambda->body < i && row->lambda->number != 0 &&
                          row->lambda->capture_count == 0);
                for (uint32_t j = 0; j < row->lambda->parameter_count; j++)
                    if (row->lambda->parameters[j] != NULL)
                        binding(body, row->lambda->parameters[j], parameters);
            }
        }
    }
}

/* Only the generated arithmetic control uses this evaluator. Its expected
 * value is calculated from the bytes, not from the graph or its syntax. */
static MO_MUST_USE int64_t arithmetic(const struct mo_program *program,
                                      const struct mo_checked_body *body, uint32_t initialized,
                                      const int64_t *constants) {
    int64_t values[64] = {0};
    MO_ASSERT(body->count <= 64);
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_checked_expression *row = &body->items[i];
        if (row->syntax->kind == MO_S_INT) {
            for (uint32_t j = 0; j < row->syntax->span.end - row->syntax->span.start; j++)
                values[i] = values[i] * 10 + (int64_t)(row->syntax->text[j] - '0');
        } else if (row->symbol != NULL && row->symbol->declaration->kind == MO_S_CONST) {
            uint32_t at = 0;
            while (at < initialized && program->constants[at].symbol != row->symbol)
                at++;
            MO_ASSERT(at < initialized);
            values[i] = constants[at];
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
    struct mo_module_set set = {
        .count = 1, .modules = &module, .order = &order, .uses = &uses, .sources = &sources};
    struct mo_program *program = mo_check(&arena, &set, &diags);
    MO_ASSERT((program == NULL) == mo_diags_any(&diags));
    if (expected >= 0)
        MO_ASSERT((program != NULL) == (expected != 0));
    if (program != NULL) {
        graph(program);
        if (expected == 1) {
            int64_t constants[8] = {0};
            MO_ASSERT(program->constant_count <= 8);
            for (uint32_t i = 0; i < program->constant_count; i++)
                constants[i] =
                    arithmetic(program, &program->constants[i].initializer, i, constants);
            MO_ASSERT(arithmetic(program, &program->functions[program->entry.function].body,
                                 program->constant_count, constants) == result);
        }
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
    mo_buffer_clear(&source);
    MO_ASSERT(
        mo_buffer_append_text(&source, "module Main\nresult = (left + right) * factor - offset\n"));
    static const char *const names[] = {"left", "right", "factor", "offset"};
    for (uint32_t i = 4; i != 0; i--) {
        MO_ASSERT(
            mo_buffer_append_text(&source, names[i - 1]) && mo_buffer_append_text(&source, " = ") &&
            mo_buffer_append_uint(&source, values[i - 1]) && mo_buffer_append_text(&source, "\n"));
    }
    MO_ASSERT(mo_buffer_append_text(&source, valid ? "fn main() : Int\nresult\nend\n"
                                                   : "fn main() : Text\nresult\nend\n"));
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
