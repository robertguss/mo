#include "check/abilities.h"
#include "check/checked.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/limits.h"

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

static MO_MUST_USE bool resource_failed(const struct mo_diags *diags) {
    if (diags->dropped != 0)
        return true;
    for (uint32_t i = 0; i < diags->count; i++)
        if (diags->items[i].code >= 1000 && diags->items[i].code != MO_CODE_CHECK_UNFINISHED)
            return true;
    return false;
}

static MO_MUST_USE bool type_abilities(struct mo_ability_context *context,
                                       const struct mo_source_type *type, struct mo_span span,
                                       struct mo_diags *diags) {
    if (!mo_signature_type_available(context->signatures, type) ||
        mo_check_type_abilities(context, type))
        return true;
    bool kept = report(diags, context->error, span, context->message, context->reason);
    /* A failed recursive proof has provisional edges. Discard that context,
     * rather than allowing its unfinished cache into a later declaration. */
    *context = (struct mo_ability_context){.arena = context->arena,
                                           .interfaces = context->interfaces,
                                           .signatures = context->signatures,
                                           .work = context->work};
    return kept && !resource_failed(diags);
}

static MO_MUST_USE bool check_examples(struct mo_arena *arena, struct mo_program *program,
                                       struct mo_checked_spec *specs, uint32_t count,
                                       struct mo_diags *diags) {
    const struct mo_signatures *signatures = program->signatures;
    const struct mo_sources *sources = program->set->sources;
    uint32_t index = 0;
    for (uint32_t i = 0; i < program->function_count; i++) {
        const struct mo_function_signature *fn = program->functions[i].signature;
        const struct mo_syntax *written = fn->function->declaration->children[4];
        uint32_t number = 0;
        for (uint32_t j = 0; j < written->count; j++) {
            MO_ASSERT(written->children[j]->kind == MO_S_EXAMPLES);
            const struct mo_syntax *lines = written->children[j]->children[0];
            for (uint32_t k = 0; k < lines->count; k++) {
                MO_ASSERT(index < count && sources != NULL);
                const struct mo_syntax *line = lines->children[k];
                struct mo_span span = line->span;
                MO_ASSERT(span.file < sources->count && span.start <= span.end &&
                          span.end <= sources->files[span.file].length);
                size_t length = span.end - span.start;
                uint8_t *bytes = allocate(arena, diags, span, length, 1, 1);
                if (bytes == NULL)
                    return false;
                memcpy(bytes, sources->files[span.file].bytes + span.start, length);
                struct mo_checked_spec *spec = &specs[index++];
                *spec = (struct mo_checked_spec){.kind = MO_SPEC_EXAMPLE,
                                                 .syntax = line,
                                                 .module = fn->function->module,
                                                 .subject = i,
                                                 .number = ++number,
                                                 .promise = UINT32_MAX,
                                                 .text = {bytes, length, span}};
                bool halt;
                if (!mo_check_example(arena, program->interfaces, signatures,
                                      (uint32_t)(fn - signatures->items), line, diags, &spec->body,
                                      &halt)) {
                    if (halt)
                        return false;
                    continue;
                }
            }
        }
    }
    MO_ASSERT(index == count);
    return !mo_diags_any(diags);
}

static MO_MUST_USE struct mo_program *
check_program(struct mo_arena *arena, const struct mo_module_set *set, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && set != NULL && set->count != 0 && diags != NULL);
    if (mo_diags_any(diags))
        return NULL;
    struct mo_module_interfaces *interfaces = mo_resolve_modules(arena, set, diags);
    if (interfaces == NULL)
        return NULL;
    struct mo_signatures *signatures =
        mo_prepare_available_signatures(arena, set, interfaces, diags);
    if (signatures == NULL)
        return NULL;
    uint32_t work = 0;
    struct mo_ability_context abilities = {
        .arena = arena, .interfaces = interfaces, .signatures = signatures, .work = &work};
    for (uint32_t i = 0; i < signatures->count; i++) {
        work = 0;
        struct mo_function_signature *fn = &signatures->items[i];
        if (!mo_signature_type_available(signatures, fn->type)) {
            fn->type = NULL;
            continue;
        }
        const struct mo_syntax *syntax = fn->function->declaration;
        const struct mo_syntax *parameters = syntax->children[1];
        for (uint32_t j = 0; j < fn->type->count; j++) {
            const struct mo_syntax *annotation =
                j < parameters->count ? parameters->children[j]->children[1] : syntax->children[2];
            uint32_t before = diags->count;
            if (!type_abilities(&abilities, fn->type->arguments[j],
                                annotation == NULL ? syntax->children[0]->span : annotation->span,
                                diags))
                return NULL;
            if (diags->count != before) {
                fn->type = NULL;
                signatures->recovering = true;
                break;
            }
        }
    }
    uint32_t customizers = 0;
    for (uint32_t i = 0; i < signatures->customizer_count; i++)
        if (signatures->customizers[i]->type != NULL)
            signatures->customizers[customizers++] = signatures->customizers[i];
    signatures->customizer_count = customizers;
    for (uint32_t i = 0; i < signatures->data_count; i++) {
        work = 0;
        if (!type_abilities(&abilities, signatures->data[i].type,
                            signatures->data[i].symbol->declaration->children[0]->span, diags))
            return NULL;
        for (uint32_t j = 0; j < signatures->data[i].count; j++) {
            const struct mo_field_signature *field = signatures->data[i].by_index[j];
            if (!type_abilities(&abilities, field->type, field->annotation->span, diags))
                return NULL;
        }
    }
    struct mo_span span = set->modules[0]->span;
    uint32_t spec_count = 0;
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
            if (item->kind != MO_S_FN)
                continue;
            const struct mo_syntax *specs = item->children[4];
            for (uint32_t k = 0; k < specs->count; k++) {
                const struct mo_syntax *spec = specs->children[k];
                if (spec->kind != MO_S_EXAMPLES || set->sources == NULL) {
                    bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, spec->span,
                                       "this specification rule is not implemented", NULL);
                    (void)kept;
                    return NULL;
                }
                uint32_t lines = spec->children[0]->count;
                if (lines > MO_LIMIT_PROGRAM_CHECK_WORK - *signatures->work) {
                    bool kept = report(diags, MO_CODE_CHECK_PROGRAM_WORK, spec->span,
                                       "the program needs too much spec-checking work", NULL);
                    (void)kept;
                    return NULL;
                }
                *signatures->work += lines;
                spec_count += lines;
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
    if (program == NULL || functions == NULL || order == NULL)
        return NULL;
    for (uint32_t i = 0; i < signatures->count; i++)
        order[i] = &signatures->items[i];
    qsort(order, signatures->count, sizeof(*order), source_order);
    uint32_t main = UINT32_MAX;
    for (uint32_t i = 0; i < signatures->count; i++) {
        const struct mo_function_signature *fn = order[i];
        if (fn->type == NULL)
            continue;
        if (fn->function->module == 0 && strcmp(fn->function->name, "main") == 0)
            main = i;
        uint32_t index = (uint32_t)(fn - signatures->items);
        bool halt;
        if (!mo_check_function(arena, interfaces, signatures, index, diags, &functions[i], &halt) &&
            halt)
            return NULL;
    }
    mo_diags_sort(diags);
    if (mo_diags_any(diags))
        return NULL;
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
    struct mo_checked_spec *specs =
        allocate(arena, diags, span, spec_count, sizeof(*specs), _Alignof(struct mo_checked_spec));
    if (specs == NULL || !check_examples(arena, program, specs, spec_count, diags))
        return NULL;
    program->spec_count = spec_count;
    program->specs = specs;
    return mo_check_recursion(arena, program, diags) ? program : NULL;
}

struct mo_program *mo_check(struct mo_arena *arena, const struct mo_module_set *set,
                            struct mo_diags *diags) {
    struct mo_program *out = check_program(arena, set, diags);
    mo_diags_sort(diags);
    return out;
}
