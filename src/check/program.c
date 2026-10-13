#include "check/checked.h"

#include "base/codes.h"

#include <stdlib.h>
#include <string.h>

static MO_MUST_USE bool report(struct mo_diags *diags, uint16_t code, struct mo_span span,
                               const char *message, const char *reason) {
    return mo_diags_add(
        diags, &(struct mo_diag){
                   .code = code, .primary = {span, message}, .message = message, .reason = reason});
}

static MO_MUST_USE void *allocate(struct mo_arena *arena, struct mo_diags *diags,
                                  struct mo_span span, size_t count, size_t size,
                                  size_t alignment) {
    void *out = mo_arena_allocate_array(arena, count, size, alignment);
    if (out == NULL) {
        bool kept =
            report(diags, MO_CODE_CHECK_NO_MEMORY, span, "the checking arena has no room", NULL);
        (void)kept;
    }
    return out;
}

static MO_MUST_USE int source_order(const void *a, const void *b) {
    const struct mo_function_signature *x = *(const struct mo_function_signature *const *)a;
    const struct mo_function_signature *y = *(const struct mo_function_signature *const *)b;
    if (x->function->module != y->function->module)
        return x->function->module < y->function->module ? -1 : 1;
    uint32_t left = x->function->declaration->span.start,
             right = y->function->declaration->span.start;
    return left < right ? -1 : left > right ? 1 : 0;
}

struct mo_program *mo_check(struct mo_arena *arena, const struct mo_module_set *set,
                            struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && set != NULL && set->count != 0 && diags != NULL);
    if (mo_diags_any(diags))
        return NULL;
    struct mo_module_interfaces *interfaces = mo_resolve_modules(arena, set, diags);
    if (interfaces == NULL)
        return NULL;
    struct mo_signatures *signatures = mo_prepare_signatures(arena, set, interfaces, diags);
    if (signatures == NULL)
        return NULL;
    struct mo_span span = set->modules[0]->span;
    for (uint32_t i = 0; i < set->count; i++) {
        const struct mo_module_syntax *module = set->modules[i];
        const struct mo_syntax *items = module->tree->children[1];
        if (module->header.uses->count != 0) {
            bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, module->header.uses->span,
                               "unused import checking is not implemented", NULL);
            (void)kept;
            return NULL;
        }
        for (uint32_t j = 0; j < items->count; j++) {
            const struct mo_syntax *item = items->children[j];
            if (item->kind == MO_S_CONST || item->kind == MO_S_TEST) {
                bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, item->span,
                                   "constant or spec checking is not implemented", NULL);
                (void)kept;
                return NULL;
            }
        }
    }
    struct mo_program *program =
        allocate(arena, diags, span, 1, sizeof(*program), _Alignof(struct mo_program));
    struct mo_checked_function *functions =
        allocate(arena, diags, span, signatures->count, sizeof(*functions),
                 _Alignof(struct mo_checked_function));
    const struct mo_function_signature **order =
        allocate(arena, diags, span, signatures->count, sizeof(*order),
                 _Alignof(const struct mo_function_signature *));
    if (mo_diags_any(diags))
        return NULL;
    for (uint32_t i = 0; i < signatures->count; i++)
        order[i] = &signatures->items[i];
    qsort(order, signatures->count, sizeof(*order), source_order);
    uint32_t main = UINT32_MAX;
    for (uint32_t i = 0; i < signatures->count; i++) {
        const struct mo_function_signature *fn = order[i];
        if (fn->type->parameters != 0 || fn->type->count != 1) {
            bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, fn->function->declaration->span,
                               "parameter graph production is not implemented", NULL);
            (void)kept;
            return NULL;
        }
        if (fn->function->module == 0 && strcmp(fn->function->name, "main") == 0)
            main = i;
        uint32_t index = (uint32_t)(fn - signatures->items);
        if (!mo_check_function(arena, interfaces, signatures, index, diags, &functions[i]))
            return NULL;
    }
    const struct mo_source_type *result =
        main == UINT32_MAX ? NULL : functions[main].signature->type->arguments[0];
    bool unit = result != NULL && result->kind == MO_SOURCE_NAMED && result->module == UINT32_MAX &&
                strcmp(result->name, "Unit") == 0;
    bool status = result != NULL && result->kind == MO_SOURCE_NAMED &&
                  result->module == UINT32_MAX && strcmp(result->name, "Int") == 0;
    if (!unit && !status) {
        bool kept = report(
            diags, main == UINT32_MAX ? MO_CODE_MAIN_CONTRACT : MO_CODE_CHECK_UNFINISHED,
            main == UINT32_MAX ? span : functions[main].signature->function->declaration->span,
            main == UINT32_MAX ? "the entry module has no main function"
                               : "this main result convention is not implemented",
            main == UINT32_MAX ? "Main declares the program's entry (reference 11.2)." : NULL);
        (void)kept;
        return NULL;
    }
    *program = (struct mo_program){
        .set = set,
        .interfaces = interfaces,
        .signatures = signatures,
        .function_count = signatures->count,
        .functions = functions,
        .entry = {.function = main, .result = unit ? MO_ENTRY_UNIT : MO_ENTRY_STATUS}};
    return program;
}
