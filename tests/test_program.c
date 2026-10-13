#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"

#include <string.h>

struct fixture {
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_module_syntax *module;
    uint32_t order, *uses;
    struct mo_module_set set;
    struct mo_program *program;
};

static void init(struct fixture *f, const char *text) {
    *f = (struct fixture){0};
    mo_arena_init(&f->syntax, 1 << 20);
    mo_arena_init(&f->arena, 4 << 20);
    MO_ASSERT(mo_sources_init(&f->sources) && mo_diags_init(&f->diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, "main.mo", (const uint8_t *)text, strlen(text),
                                   &file, &error));
    f->module = mo_parse(&f->syntax, &f->sources, file, &f->diags);
    MO_ASSERT(f->module != NULL && !mo_diags_any(&f->diags));
    f->set = (struct mo_module_set){
        .count = 1, .modules = &f->module, .order = &f->order, .uses = &f->uses};
    f->program = mo_check(&f->arena, &f->set, &f->diags);
}

static void free_fixture(struct fixture *f) {
    mo_arena_free(&f->arena);
    mo_arena_free(&f->syntax);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
}

static void entry_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn identity(value: Int) : Int\n"
             "var copy = value\ncopy = 42\ncopy\nend\n"
             "fn main() : Int\nidentity(7 + 12 * 3 - 1)\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        CHECK(program->function_count == 2);
        CHECK(program->entry.function == 1 && program->entry.result == MO_ENTRY_STATUS);
        const struct mo_checked_function *identity = &program->functions[0];
        CHECK(identity->parameter_count == 1 && identity->body.binding_count == 2);
        if (identity->parameter_count == 1 && identity->body.binding_count == 2) {
            const struct mo_checked_binding *parameter = identity->parameters[0];
            const struct mo_checked_binding *copy = &identity->body.bindings[1];
            CHECK(strcmp(parameter->name, "value") == 0);
            CHECK(strcmp(parameter->type->name, "Int") == 0 && !parameter->variable);
            CHECK(strcmp(copy->name, "copy") == 0 && copy->variable);
            CHECK(strcmp(copy->type->name, "Int") == 0);
            bool read = false, initialized = false, assigned = false;
            for (uint32_t i = 0; i < identity->body.count; i++) {
                const struct mo_checked_expression *row = &identity->body.items[i];
                if (row->syntax->kind == MO_S_NAME && row->binding == parameter)
                    read = true;
                if (row->syntax->kind == MO_S_VAR || row->syntax->kind == MO_S_BIND) {
                    CHECK(row->binding == copy);
                    CHECK(row->operand_count == 1 && row->operands[0] < i);
                    CHECK(row->assignment == (row->syntax->kind == MO_S_BIND));
                    if (row->syntax->kind == MO_S_VAR)
                        initialized = true;
                    else
                        assigned = true;
                }
            }
            CHECK(read && initialized && assigned);
        }
        const struct mo_checked_body *body = &program->functions[program->entry.function].body;
        CHECK(body->root < body->count);
        CHECK(strcmp(body->items[body->root].type->name, "Int") == 0);
        MO_ASSERT(body->count <= 64);
        int64_t values[64] = {0};
        uint32_t operators = 0;
        bool found = false, called = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->operand_count; j++)
                CHECK(row->operands[j] < i);
            if (row->syntax->kind == MO_S_INT) {
                for (uint32_t j = 0; j < row->syntax->span.end - row->syntax->span.start; j++)
                    values[i] = values[i] * 10 + (int64_t)(row->syntax->text[j] - '0');
                found = found || values[i] == 7;
            } else if (row->operation != MO_T_EOF) {
                operators++;
                MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
                int64_t left = values[row->operands[0]], right = values[row->operands[1]];
                CHECK(row->ability_count == 1);
                if (row->ability_count == 1) {
                    CHECK(row->abilities[0].operand == row->operands[0]);
                    CHECK(row->abilities[0].implementation->kind == MO_ABILITY_NATIVE);
                    CHECK(row->abilities[0].implementation->ability == MO_SOURCE_NUMBER);
                    CHECK(strcmp(row->abilities[0].implementation->type->name, "Int") == 0);
                }
                if (row->operation == MO_T_PLUS)
                    values[i] = left + right;
                else if (row->operation == MO_T_MINUS)
                    values[i] = left - right;
                else {
                    CHECK(row->operation == MO_T_STAR);
                    values[i] = left * right;
                }
            } else if (row->syntax->kind == MO_S_CALL) {
                CHECK(row->symbol == identity->signature->function);
                MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
                CHECK(body->items[row->operands[0]].symbol == row->symbol);
                CHECK(body->items[row->operands[0]].type->kind == MO_SOURCE_FUNCTION);
                CHECK(strcmp(body->items[row->operands[1]].type->name, "Int") == 0);
                CHECK(row->arguments.count == 0);
                CHECK(values[row->operands[1]] == 42);
                called = true;
            } else if (row->operand_count != 0) {
                values[i] = values[row->operands[row->operand_count - 1]];
            }
        }
        CHECK(found && called && operators == 3);
    }
    free_fixture(&f);
}

static void console_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn main(console: Console)\nconsole.print(\"hello\")\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        CHECK(program->entry.result == MO_ENTRY_UNIT);
        CHECK(program->entry.capability_count == 1);
        const struct mo_checked_function *main = &program->functions[program->entry.function];
        MO_ASSERT(main->parameter_count == 1 && program->entry.capability_count == 1);
        CHECK(program->entry.capabilities[0] == main->parameters[0]);
        CHECK(strcmp(main->parameters[0]->type->name, "Console") == 0);
        bool printed = false;
        for (uint32_t i = 0; i < main->body.count; i++) {
            const struct mo_checked_expression *row = &main->body.items[i];
            if (row->native == NULL)
                continue;
            CHECK(strcmp(row->native, "Console.print") == 0);
            MO_ASSERT(row->operand_count == 2 && row->operands[0] < i && row->operands[1] < i);
            CHECK(row->receiver == row->operands[0]);
            CHECK(main->body.items[row->receiver].binding == main->parameters[0]);
            const struct mo_checked_expression *text = &main->body.items[row->operands[1]];
            CHECK(text->syntax->kind == MO_S_TEXT);
            CHECK(text->syntax->span.end - text->syntax->span.start == 7);
            CHECK(memcmp(text->syntax->text, "\"hello\"", 7) == 0);
            CHECK(strcmp(row->type->name, "Unit") == 0);
            printed = true;
        }
        CHECK(printed);
    }
    free_fixture(&f);
}

static void lambda_graph(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "fn compose(f: fn(Int) : Int, g: fn(Int) : Int, value: Int) : Int\n"
             "f(g(value))\nend\n"
             "fn main() : Int\ncompose(fn(x) x + 1 end, fn(y) y * 2 end, 20)\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_checked_body *body = &program->functions[program->entry.function].body;
        uint32_t count = 0;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            if (row->lambda == NULL)
                continue;
            const struct mo_checked_lambda *lambda = row->lambda;
            count++;
            CHECK(lambda->number == count && lambda->capture_count == 0);
            MO_ASSERT(lambda->parameter_count == 1 && lambda->body < i);
            CHECK(row->operand_count == 1 && row->operands[0] == lambda->body);
            CHECK(strcmp(lambda->parameters[0]->name, count == 1 ? "x" : "y") == 0);
            CHECK(strcmp(lambda->parameters[0]->type->name, "Int") == 0);
            CHECK(row->type->kind == MO_SOURCE_FUNCTION && row->type->count == 2);
            CHECK(strcmp(row->type->arguments[1]->name, "Int") == 0);
            bool read = false;
            for (uint32_t j = 0; j < lambda->body; j++)
                read = read || body->items[j].binding == lambda->parameters[0];
            CHECK(read);
        }
        CHECK(count == 2);
    }
    free_fixture(&f);
}

static void branch_graph(void) {
    struct fixture f;
    init(&f, "module Main\nfn choose(flag: Bool) : Int\n"
             "if flag\nreturn 7\nelse if not flag\nreturn 9\nelse\nstop \"unreachable\"\nend\n"
             "end\nfn main() : Int\nchoose(false)\nend\n");
    const struct mo_program *program = f.program;
    CHECK(program != NULL && !mo_diags_any(&f.diags));
    if (program != NULL) {
        const struct mo_checked_body *body = &program->functions[0].body;
        bool branched = false;
        uint32_t returns = 0, stops = 0;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->operand_count; j++)
                CHECK(row->operands[j] < i);
            if (row->syntax->kind == MO_S_RETURN) {
                MO_ASSERT(row->operand_count == 1);
                CHECK(row->never);
                const struct mo_syntax *value = body->items[row->operands[0]].syntax;
                CHECK(value->kind == MO_S_INT && value->text[0] == (returns == 0 ? '7' : '9'));
                returns++;
            } else if (row->syntax->kind == MO_S_STOP) {
                MO_ASSERT(row->operand_count == 1);
                CHECK(row->never && body->items[row->operands[0]].syntax->kind == MO_S_TEXT);
                stops++;
            } else if (row->syntax->kind == MO_S_IF) {
                MO_ASSERT(row->operand_count == 5);
                CHECK(row->never && strcmp(row->type->name, "Int") == 0);
                CHECK(strcmp(body->items[row->operands[0]].type->name, "Bool") == 0);
                CHECK(strcmp(body->items[row->operands[2]].type->name, "Bool") == 0);
                CHECK(body->items[row->operands[4]].never);
                branched = true;
            }
        }
        CHECK(branched && returns == 2 && stops == 1);
    }
    free_fixture(&f);
}

static void run(void *argument) {
    (void)argument;
    entry_graph();
    console_graph();
    lambda_graph();
    branch_graph();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("program");
}
