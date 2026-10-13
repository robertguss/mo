#include "check/abilities.h"
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

static MO_MUST_USE bool native_type(const struct mo_source_type *type, const char *name) {
    return type != NULL && type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX &&
           strcmp(type->name, name) == 0;
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
    uint32_t work = 0;
    struct mo_ability_context abilities = {
        .arena = arena, .interfaces = interfaces, .signatures = signatures, .work = &work};
    for (uint32_t i = 0; i < signatures->count; i++) {
        work = 0;
        if (!mo_check_type_abilities(&abilities, signatures->items[i].type)) {
            bool kept =
                report(diags, abilities.error, signatures->items[i].function->declaration->span,
                       abilities.message, abilities.reason);
            (void)kept;
            return NULL;
        }
    }
    for (uint32_t i = 0; i < signatures->data_count; i++) {
        work = 0;
        if (!mo_check_type_abilities(&abilities, signatures->data[i].type)) {
            bool kept =
                report(diags, abilities.error, signatures->data[i].symbol->declaration->span,
                       abilities.message, abilities.reason);
            (void)kept;
            return NULL;
        }
        for (uint32_t j = 0; j < signatures->data[i].count; j++) {
            const struct mo_field_signature *field = signatures->data[i].by_index[j];
            if (!mo_check_type_abilities(&abilities, field->type)) {
                bool kept =
                    report(diags, abilities.error,
                           field->name == NULL ? signatures->data[i].symbol->declaration->span
                                               : field->name->span,
                           abilities.message, abilities.reason);
                (void)kept;
                return NULL;
            }
        }
    }
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
        if (fn->function->module == 0 && strcmp(fn->function->name, "main") == 0)
            main = i;
        uint32_t index = (uint32_t)(fn - signatures->items);
        if (!mo_check_function(arena, interfaces, signatures, index, diags, &functions[i]))
            return NULL;
    }
    if (main == UINT32_MAX) {
        bool kept =
            report(diags, MO_CODE_MAIN_CONTRACT, span, "the entry module has no main function",
                   "main declares the program's entry (reference 11.2).");
        (void)kept;
        return NULL;
    }
    const struct mo_checked_function *entry = &functions[main];
    struct mo_span entry_span = entry->signature->function->declaration->children[0]->span;
    unsigned capabilities = 0;
    static const char *const names[] = {"Args",      "Env",   "Console", "Files",
                                        "ReadFiles", "Clock", "Random"};
    for (uint32_t i = 0; i < entry->parameter_count; i++) {
        unsigned capability = 0;
        for (unsigned j = 0; j < sizeof(names) / sizeof(names[0]); j++)
            if (native_type(entry->parameters[i]->type, names[j]))
                capability = 1u << j;
        if (capability == 0 || (capabilities & capability) != 0) {
            bool kept =
                report(diags, MO_CODE_MAIN_CONTRACT, entry_span, "main has an invalid signature",
                       "main lists capabilities, at most one of each type "
                       "(reference 11.2).");
            (void)kept;
            return NULL;
        }
        capabilities |= capability;
    }
    const struct mo_source_type *type = entry->signature->type;
    const struct mo_source_type *result = type->arguments[type->count - 1];
    bool unit = native_type(result, "Unit"), status = native_type(result, "Int");
    bool wrapped = native_type(result, "Result") && native_type(result->arguments[0], "Unit");
    if (!unit && !status && !wrapped) {
        bool kept =
            report(diags, MO_CODE_MAIN_CONTRACT, entry_span, "main has an invalid signature",
                   "main returns nothing, an Int, or Result(Unit, E) (reference 11.2).");
        (void)kept;
        return NULL;
    }
    if (entry->parameter_count > 1 ||
        (entry->parameter_count == 1 && !native_type(entry->parameters[0]->type, "Console"))) {
        bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, entry_span,
                           "this main capability convention is not implemented", NULL);
        (void)kept;
        return NULL;
    }
    if (!unit && !status) {
        bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, entry_span,
                           "this main result convention is not implemented", NULL);
        (void)kept;
        return NULL;
    }
    *program = (struct mo_program){.set = set,
                                   .interfaces = interfaces,
                                   .signatures = signatures,
                                   .function_count = signatures->count,
                                   .functions = functions,
                                   .entry = {.function = main,
                                             .result = unit ? MO_ENTRY_UNIT : MO_ENTRY_STATUS,
                                             .capability_count = functions[main].parameter_count,
                                             .capabilities = functions[main].parameters}};
    return mo_check_recursion(arena, program, diags) ? program : NULL;
}
