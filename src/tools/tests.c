#include "tools/tests.h"

#include "base/codes.h"

#include <string.h>

const char *mo_observation_kind(enum mo_checked_spec_kind kind) {
    switch (kind) {
    case MO_SPEC_TEST:
        return "test";
    case MO_SPEC_EXAMPLE:
        return "example";
    case MO_SPEC_PROMISE:
        return "promise";
    case MO_SPEC_ANCHOR:
        break;
    }
    MO_UNREACHABLE();
}

static char *name(struct mo_arena *arena, const struct mo_program *program,
                  const struct mo_checked_spec *spec) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_buffer_append_text(&buffer, program->set->names[spec->module]);
    if (spec->subject != UINT32_MAX) {
        const struct mo_symbol *symbol = program->functions[spec->subject].signature->function;
        ok = ok && mo_buffer_append_text(&buffer, ".") &&
             mo_buffer_append_text(&buffer, symbol->name);
        const struct mo_module_scope *scope = &program->interfaces->scopes[symbol->module];
        const struct mo_symbol *group =
            mo_find_symbol(scope, (const uint8_t *)symbol->name, symbol->name_length);
        MO_ASSERT(group != NULL);
        if (symbol->receiver != UINT32_MAX && group->overload_count > 1)
            ok = ok && mo_buffer_append_text(&buffer, "@") &&
                 mo_buffer_append_text(&buffer, scope->symbols[symbol->receiver].name);
    }
    ok = ok && mo_buffer_append_text(&buffer, ".") &&
         mo_buffer_append_text(&buffer, mo_observation_kind(spec->kind)) &&
         mo_buffer_append_text(&buffer, ".") && mo_buffer_append_uint(&buffer, spec->number) &&
         mo_buffer_append_byte(&buffer, 0);
    char *result = ok ? mo_arena_copy(arena, buffer.bytes, buffer.length, 1) : NULL;
    mo_buffer_free(&buffer);
    return result;
}

struct mo_test_selection *mo_select_tests(struct mo_arena *arena, const struct mo_program *program,
                                          const struct mo_cli *cli, struct mo_diags *diags) {
    struct mo_test_selection *selection =
        mo_arena_allocate(arena, sizeof(*selection), _Alignof(struct mo_test_selection));
    if (selection == NULL)
        goto exhausted;
    *selection = (struct mo_test_selection){0};
    selection->items = mo_arena_allocate_array(
        arena, program->spec_count, sizeof(*selection->items), _Alignof(struct mo_observation));
    if (selection->items == NULL)
        goto exhausted;
    bool found[MO_CLI_ARGUMENTS] = {false};
    for (uint32_t i = 0; i < program->spec_count; i++) {
        const struct mo_checked_spec *spec = &program->specs[i];
        if (spec->kind == MO_SPEC_ANCHOR)
            continue;
        char *full = name(arena, program, spec);
        if (full == NULL)
            goto exhausted;
        bool selected = cli->only_count == 0;
        for (uint32_t j = 0; j < cli->only_count; j++)
            if (strcmp(full, cli->only[j]) == 0)
                selected = found[j] = true;
        selected = selected && (cli->module == NULL ||
                                strcmp(cli->module, program->set->names[spec->module]) == 0);
        if (selected)
            selection->items[selection->count++] = (struct mo_observation){full, spec};
    }
    for (uint32_t i = 0; i < cli->only_count; i++)
        if (!found[i])
            selection->mistake = "--only does not name an example, promise or test";
    if (selection->mistake == NULL && selection->count == 0)
        selection->mistake = "test selection matches no examples, promises or tests";
    return selection;
exhausted:;
    struct mo_diag diag = {.code = MO_CODE_TOOL_PIPELINE,
                           .message = "test inventory exceeds tool resources",
                           .primary.span.file = UINT32_MAX};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
    return NULL;
}
