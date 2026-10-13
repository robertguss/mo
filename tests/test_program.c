#include "base/codes.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"

#include <string.h>

static void entry_graph(void) {
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    mo_arena_init(&syntax, 1 << 20);
    mo_arena_init(&arena, 4 << 20);
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    const char *text = "module Main\nfn main() : Int\n42\nend\n";
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "main.mo", (const uint8_t *)text, strlen(text), &file,
                                   &error));
    struct mo_module_syntax *module = mo_parse(&syntax, &sources, file, &diags);
    MO_ASSERT(module != NULL && !mo_diags_any(&diags));
    uint32_t order[] = {0}, *uses[] = {NULL};
    struct mo_module_set set = {.count = 1, .modules = &module, .order = order, .uses = uses};
    struct mo_program *program = mo_check(&arena, &set, &diags);
    CHECK(program != NULL && !mo_diags_any(&diags));
    if (program != NULL) {
        CHECK(program->function_count == 1);
        CHECK(program->entry.function == 0 && program->entry.result == MO_ENTRY_STATUS);
        const struct mo_checked_body *body = &program->functions[0].body;
        CHECK(body->root < body->count);
        CHECK(strcmp(body->items[body->root].type->name, "Int") == 0);
        bool found = false;
        for (uint32_t i = 0; i < body->count; i++) {
            const struct mo_checked_expression *row = &body->items[i];
            for (uint32_t j = 0; j < row->operand_count; j++)
                CHECK(row->operands[j] < i);
            if (row->syntax->kind == MO_S_INT) {
                CHECK(row->syntax->span.end - row->syntax->span.start == 2);
                CHECK(memcmp(row->syntax->text, "42", 2) == 0);
                found = true;
            }
        }
        CHECK(found);
    }
    mo_arena_free(&arena);
    mo_arena_free(&syntax);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
}

static void run(void *argument) {
    (void)argument;
    entry_graph();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("program");
}
