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

static MO_MUST_USE bool check_imports(const struct mo_signatures *signatures,
                                      struct mo_diags *diags) {
    bool valid = true;
    for (uint32_t i = 0; i < signatures->import_count; i++) {
        const struct mo_import_usage *use = &signatures->imports[i];
        if (*signatures->work == MO_LIMIT_PROGRAM_CHECK_WORK) {
            bool kept = report(diags, MO_CODE_CHECK_PROGRAM_WORK, use->origin->span,
                               "the program needs too much import-checking work", NULL);
            (void)kept;
            return false;
        }
        (*signatures->work)++;
        if (use->used || (use->parent == i && use->origin->children[1] != NULL) ||
            (use->parent != i && use->origin->text[0] == '_'))
            continue;
        struct mo_span span = use->parent == i ? use->origin->children[0]->span : use->origin->span;
        if (!report(diags, MO_CODE_UNUSED_IMPORT, span,
                    use->parent == i ? "an imported module is unused"
                                     : "an imported name is unused",
                    "Unused imports are errors (reference 6.1).") &&
            diags->count < MO_LIMIT_DIAGNOSTICS)
            return false;
        valid = false;
    }
    return valid;
}

static MO_MUST_USE int spec_source_order(const void *a, const void *b) {
    const struct mo_checked_spec *x = *(const struct mo_checked_spec *const *)a;
    const struct mo_checked_spec *y = *(const struct mo_checked_spec *const *)b;
    if (x->module != y->module)
        return x->module < y->module ? -1 : 1;
    return x->syntax->span.start < y->syntax->span.start   ? -1
           : x->syntax->span.start > y->syntax->span.start ? 1
                                                           : 0;
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

static MO_MUST_USE bool copy_text(struct mo_arena *arena, struct mo_diags *diags,
                                  const struct mo_sources *sources, struct mo_span span,
                                  struct mo_spec_text *out) {
    MO_ASSERT(sources != NULL && span.file < sources->count && span.start <= span.end &&
              span.end <= sources->files[span.file].length);
    size_t length = span.end - span.start;
    uint8_t *bytes = allocate(arena, diags, span, length, 1, 1);
    if (bytes == NULL)
        return false;
    memcpy(bytes, sources->files[span.file].bytes + span.start, length);
    *out = (struct mo_spec_text){bytes, length, span};
    return true;
}

static MO_MUST_USE unsigned hex_digit(uint8_t c) {
    return c >= 'a'   ? (unsigned)(c - 'a') + 10
           : c >= 'A' ? (unsigned)(c - 'A') + 10
                      : (unsigned)(c - '0');
}

static MO_MUST_USE bool wording(struct mo_arena *arena, struct mo_diags *diags,
                                const struct mo_syntax *text, struct mo_checked_spec *spec) {
    size_t length = text->span.end - text->span.start;
    size_t quote = length >= 6 && text->text[1] == '"' && text->text[2] == '"' ? 3 : 1;
    uint8_t *out = allocate(arena, diags, text->span, length, 1, 1);
    if (out == NULL)
        return false;
    size_t begin = quote, end = length - quote, indent = 0;
    if (quote == 3) {
        while (begin < end && text->text[begin] != '\n')
            begin++;
        MO_ASSERT(begin < end);
        begin++;
        size_t closing = end;
        while (closing > begin && text->text[closing - 1] == ' ')
            closing--;
        indent = end - closing;
        end = closing > begin ? closing - 1 : begin;
        if (end > begin && text->text[end - 1] == '\r')
            end--;
    }
    size_t at = 0;
    bool line_start = quote == 3;
    for (size_t i = begin; i < end; i++) {
        if (line_start) {
            size_t spaces = 0;
            while (i < end && spaces < indent && text->text[i] == ' ') {
                i++;
                spaces++;
            }
            line_start = false;
            if (i == end)
                break;
        }
        uint8_t c = text->text[i];
        if (quote == 3 && c == '\r') {
            MO_ASSERT(i + 1 < end && text->text[i + 1] == '\n');
            i++;
            c = '\n';
        }
        if (quote == 3 && c == '\n')
            line_start = true;
        if (c != '\\') {
            out[at++] = c;
            continue;
        }
        MO_ASSERT(++i < end);
        c = text->text[i];
        if (c != 'u') {
            out[at++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
            continue;
        }
        MO_ASSERT(text->text[++i] == '{');
        uint32_t scalar = 0;
        while (text->text[++i] != '}')
            scalar = scalar * 16 + hex_digit(text->text[i]);
        if (scalar < 0x80)
            out[at++] = (uint8_t)scalar;
        else {
            unsigned extra = scalar < 0x800 ? 1 : scalar < 0x10000 ? 2 : 3;
            out[at++] = (uint8_t)((extra == 1   ? 0xc0u
                                   : extra == 2 ? 0xe0u
                                                : 0xf0u) |
                                  (scalar >> (6 * extra)));
            for (unsigned j = extra; j != 0; j--)
                out[at++] = (uint8_t)(0x80u | ((scalar >> (6 * (j - 1))) & 63u));
        }
    }
    MO_ASSERT(at <= length);
    spec->wording = out;
    spec->wording_length = at;
    return true;
}

static MO_MUST_USE bool check_specs(struct mo_arena *arena, struct mo_program *program,
                                    struct mo_constant_context *constants,
                                    struct mo_checked_spec *specs, uint32_t count,
                                    struct mo_diags *diags) {
    const struct mo_signatures *signatures = program->signatures;
    const struct mo_sources *sources = program->set->sources;
    uint32_t index = 0;
    for (uint32_t i = 0; i < program->function_count; i++) {
        const struct mo_function_signature *fn = program->functions[i].signature;
        const struct mo_syntax *written = fn->function->declaration->children[4];
        uint32_t example_number = 0, promise_number = 0;
        for (uint32_t j = 0; j < written->count; j++) {
            const struct mo_syntax *item = written->children[j];
            if (item->kind == MO_S_EXAMPLES) {
                const struct mo_syntax *lines = item->children[0];
                for (uint32_t k = 0; k < lines->count; k++) {
                    MO_ASSERT(index < count);
                    const struct mo_syntax *line = lines->children[k];
                    struct mo_checked_spec *spec = &specs[index++];
                    *spec = (struct mo_checked_spec){.kind = MO_SPEC_EXAMPLE,
                                                     .syntax = line,
                                                     .module = fn->function->module,
                                                     .subject = i,
                                                     .number = ++example_number,
                                                     .promise = UINT32_MAX};
                    if (!copy_text(arena, diags, sources, line->span, &spec->text))
                        return false;
                    bool halt;
                    if (!mo_check_spec_body(arena, program->interfaces, signatures, constants, fn,
                                            spec, NULL, diags, &halt) &&
                        halt)
                        return false;
                }
                continue;
            }
            MO_ASSERT(item->kind == MO_S_PROMISE && index < count);
            uint32_t promise_index = index;
            struct mo_checked_spec *promise = &specs[index++];
            *promise = (struct mo_checked_spec){.kind = MO_SPEC_PROMISE,
                                                .syntax = item,
                                                .module = fn->function->module,
                                                .subject = i,
                                                .number = ++promise_number,
                                                .promise = UINT32_MAX};
            struct mo_span header = item->span;
            header.end = item->children[0]->span.end;
            if (!copy_text(arena, diags, sources, header, &promise->text) ||
                !wording(arena, diags, item->children[0], promise))
                return false;
            const struct mo_syntax *lines = item->children[1];
            struct mo_checked_condition *conditions =
                allocate(arena, diags, header, lines->count, sizeof(*conditions),
                         _Alignof(struct mo_checked_condition));
            if (conditions == NULL)
                return false;
            for (uint32_t k = 0; k < lines->count; k++) {
                conditions[k] = (struct mo_checked_condition){.expression = UINT32_MAX};
                if (!copy_text(arena, diags, sources, lines->children[k]->span,
                               &conditions[k].text))
                    return false;
            }
            promise->condition_count = lines->count;
            promise->conditions = conditions;
            bool halt;
            const struct mo_syntax *anchors = item->children[2];
            if (!mo_check_spec_body(arena, program->interfaces, signatures, constants, fn, promise,
                                    conditions, diags, &halt)) {
                if (halt)
                    return false;
                index += anchors->count; /* Anchors depend on a valid promise context. */
                continue;
            }
            uint32_t allows = 0, forbids = 0;
            for (uint32_t k = 0; k < anchors->count; k++) {
                MO_ASSERT(index < count);
                const struct mo_syntax *line = anchors->children[k];
                struct mo_checked_spec *spec = &specs[index++];
                bool allow = line->token == MO_T_ALLOWS;
                *spec = (struct mo_checked_spec){.kind = MO_SPEC_ANCHOR,
                                                 .syntax = line,
                                                 .module = fn->function->module,
                                                 .subject = i,
                                                 .number = allow ? ++allows : ++forbids,
                                                 .promise = promise_index,
                                                 .allows = allow};
                if (!copy_text(arena, diags, sources, line->span, &spec->text))
                    return false;
                if (!mo_check_spec_body(arena, program->interfaces, signatures, constants, fn, spec,
                                        NULL, diags, &halt) &&
                    halt)
                    return false;
            }
        }
    }
    for (uint32_t module = 0; module < program->set->count; module++) {
        const struct mo_syntax *items = program->set->modules[module]->tree->children[1];
        uint32_t number = 0;
        for (uint32_t i = 0; i < items->count; i++) {
            const struct mo_syntax *item = items->children[i];
            if (item->kind != MO_S_TEST)
                continue;
            MO_ASSERT(index < count);
            struct mo_checked_spec *spec = &specs[index++];
            *spec = (struct mo_checked_spec){.kind = MO_SPEC_TEST,
                                             .syntax = item,
                                             .module = module,
                                             .number = ++number,
                                             .subject = UINT32_MAX,
                                             .promise = UINT32_MAX};
            struct mo_span header = item->span;
            header.end = item->children[0]->span.end;
            if (!copy_text(arena, diags, sources, header, &spec->text) ||
                !wording(arena, diags, item->children[0], spec))
                return false;
            struct mo_source_type unit = {
                .kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
            const struct mo_source_type *result = &unit;
            struct mo_source_type type = {.kind = MO_SOURCE_FUNCTION,
                                          .name = "fn",
                                          .count = 1,
                                          .module = UINT32_MAX,
                                          .arguments = &result,
                                          .height = 1};
            struct mo_symbol owner = {.name = "test", .module = module, .declaration = item};
            struct mo_function_signature context = {.function = &owner, .type = &type};
            bool halt;
            if (!mo_check_spec_body(arena, program->interfaces, signatures, constants, &context,
                                    spec, NULL, diags, &halt) &&
                halt)
                return false;
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
        mo_prepare_available_signatures(arena, set, interfaces, diags, true);
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
        for (uint32_t j = 0; j < items->count; j++) {
            const struct mo_syntax *item = items->children[j];
            if (item->kind == MO_S_TEST) {
                spec_count++;
                if (set->sources == NULL) {
                    bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, item->span,
                                       "specifications need their original sources", NULL);
                    (void)kept;
                    return NULL;
                }
            }
            if (item->kind != MO_S_FN)
                continue;
            const struct mo_syntax *specs = item->children[4];
            for (uint32_t k = 0; k < specs->count; k++) {
                const struct mo_syntax *spec = specs->children[k];
                if (set->sources == NULL) {
                    bool kept = report(diags, MO_CODE_CHECK_UNFINISHED, spec->span,
                                       "this specification rule is not implemented", NULL);
                    (void)kept;
                    return NULL;
                }
                uint32_t lines = spec->kind == MO_S_EXAMPLES ? spec->children[0]->count
                                                             : 1 + spec->children[2]->count;
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
    struct mo_constant_context constants;
    if (!mo_check_constants(arena, interfaces, signatures, &constants, diags))
        return NULL;
    uint32_t main = UINT32_MAX;
    for (uint32_t i = 0; i < signatures->count; i++) {
        const struct mo_function_signature *fn = order[i];
        if (fn->type == NULL)
            continue;
        if (fn->function->module == 0 && strcmp(fn->function->name, "main") == 0)
            main = i;
        uint32_t index = (uint32_t)(fn - signatures->items);
        bool halt;
        if (!mo_check_function(arena, interfaces, signatures, &constants, index, diags,
                               &functions[i], &halt) &&
            halt)
            return NULL;
    }
    mo_diags_sort(diags);
    if (mo_diags_any(diags))
        return NULL;
    struct mo_checked_entry checked_entry = {.function = UINT32_MAX};
    if (main != UINT32_MAX) {
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
                bool kept = report(diags, MO_CODE_MAIN_CONTRACT, entry_span,
                                   "main has an invalid signature",
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
        bool wrapped = native_type(result, "Result") && native_type(result->arguments[0], "Unit") &&
                       result->arguments[1]->parameters == 0;
        if (!unit && !status && !wrapped) {
            bool kept =
                report(diags, MO_CODE_MAIN_CONTRACT, entry_span, "main has an invalid signature",
                       "main returns nothing, an Int, or Result(Unit, E) (reference 11.2).");
            (void)kept;
            return NULL;
        }
        const struct mo_ability_implementation *error_text = NULL;
        if (wrapped) {
            work = 0;
            error_text = mo_prove_ability(&abilities, result->arguments[1], MO_SOURCE_TEXT);
            if (error_text == NULL) {
                const struct mo_syntax *annotation =
                    entry->signature->function->declaration->children[2];
                bool kept = report(diags, abilities.error,
                                   annotation == NULL ? entry_span : annotation->span,
                                   abilities.message, abilities.reason);
                (void)kept;
                return NULL;
            }
        }
        checked_entry = (struct mo_checked_entry){.function = main,
                                                  .result = unit     ? MO_ENTRY_UNIT
                                                            : status ? MO_ENTRY_STATUS
                                                                     : MO_ENTRY_RESULT,
                                                  .capability_count = entry->parameter_count,
                                                  .capabilities = entry->parameters,
                                                  .error_text = error_text};
    }
    *program = (struct mo_program){.set = set,
                                   .interfaces = interfaces,
                                   .signatures = signatures,
                                   .function_count = signatures->count,
                                   .functions = functions,
                                   .constant_count = constants.count,
                                   .constants = constants.items,
                                   .entry = checked_entry};
    struct mo_checked_spec *specs =
        allocate(arena, diags, span, spec_count, sizeof(*specs), _Alignof(struct mo_checked_spec));
    if (specs == NULL || !check_specs(arena, program, &constants, specs, spec_count, diags))
        return NULL;
    const struct mo_checked_spec **spec_order =
        allocate(arena, diags, span, spec_count, sizeof(*spec_order),
                 _Alignof(const struct mo_checked_spec *));
    uint32_t *spec_indices =
        allocate(arena, diags, span, spec_count, sizeof(*spec_indices), _Alignof(uint32_t));
    struct mo_checked_spec *ordered_specs = allocate(
        arena, diags, span, spec_count, sizeof(*ordered_specs), _Alignof(struct mo_checked_spec));
    if (spec_order == NULL || spec_indices == NULL || ordered_specs == NULL)
        return NULL;
    for (uint32_t i = 0; i < spec_count; i++)
        spec_order[i] = &specs[i];
    qsort(spec_order, spec_count, sizeof(*spec_order), spec_source_order);
    for (uint32_t i = 0; i < spec_count; i++)
        spec_indices[(uint32_t)(spec_order[i] - specs)] = i;
    for (uint32_t i = 0; i < spec_count; i++) {
        ordered_specs[i] = *spec_order[i];
        if (ordered_specs[i].promise != UINT32_MAX)
            ordered_specs[i].promise = spec_indices[ordered_specs[i].promise];
    }
    program->spec_count = spec_count;
    program->specs = ordered_specs;
    if (!mo_check_recursion(arena, program, diags) ||
        !mo_check_capture_arguments(arena, program, diags))
        return NULL;
    if (!check_imports(signatures, diags))
        return NULL;
    return mo_order_constants(arena, program, &constants, diags) ? program : NULL;
}

struct mo_program *mo_check(struct mo_arena *arena, const struct mo_module_set *set,
                            struct mo_diags *diags) {
    struct mo_program *out = check_program(arena, set, diags);
    mo_diags_sort(diags);
    return out;
}
