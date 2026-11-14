#include "check/abilities.h"
#include "check/check.h"
#include "check/checked.h"
#include "check/data.h"
#include "check/infer.h"
#include "check/patterns.h"

#include "base/codes.h"
#include "base/limits.h"

#include <stdlib.h>
#include <string.h>

static MO_MUST_USE struct mo_inferred_body *
infer_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
               const struct mo_signatures *signatures, struct mo_constant_context *constants,
               const struct mo_function_signature *signature, struct mo_diags *diags,
               struct mo_checked_function *checked, bool *halt, struct mo_checked_spec *spec,
               struct mo_checked_condition *conditions) {
    MO_ASSERT(arena != NULL && interfaces != NULL && signatures != NULL && diags != NULL);
    MO_ASSERT(halt != NULL && signature != NULL && signatures->work != NULL);
    MO_ASSERT(*signatures->work <= MO_LIMIT_PROGRAM_CHECK_WORK);
    *halt = false;
    const struct mo_syntax *syntax = signature->function->declaration;
    const struct mo_syntax *written = spec == NULL ? syntax : spec->syntax;
    bool promise = spec != NULL && spec->kind == MO_SPEC_PROMISE;
    bool anchor = spec != NULL && spec->kind == MO_SPEC_ANCHOR;
    bool test = spec != NULL && spec->kind == MO_SPEC_TEST;
    bool service_promise = (promise || anchor) && spec->subject == UINT32_MAX;
    bool constant = syntax->kind == MO_S_CONST;
    bool service = syntax->kind == MO_S_SERVICE_STATE || syntax->kind == MO_S_SERVICE_EVERY;
    struct inference p = {.arena = arena,
                          .diags = diags,
                          .interfaces = interfaces,
                          .signatures = signatures,
                          .function = signature,
                          .constants = constants,
                          .checked = checked != NULL,
                          .spec = spec != NULL && !service_promise,
                          .span = written->span};
    p.abilities = (struct mo_ability_context){
        .arena = arena, .interfaces = interfaces, .signatures = signatures, .work = &p.work};
    bool finalized = false;
    if (checked == NULL && syntax->children[4]->count != 0) {
        p.span = syntax->children[4]->children[0]->span;
        bool reported = infer_unfinished(&p);
        (void)reported;
        goto failed;
    }
    struct value instance =
        anchor ? infer_function_value(&p, signature->type, signature->requirements)
               : (struct value){.type = infer_annotation(&p, signature->type, NULL, 0)};
    struct term *fn = instance.type;
    if (fn == NULL)
        goto failed;
    p.result = constant       ? infer_fresh(&p, 0)
               : spec == NULL ? infer_argument(&p, fn, fn->shape.count - 1)
                              : infer_native(&p, test ? "Unit" : "Bool", 0);
    if (p.result == NULL)
        goto failed;
    const struct mo_syntax *parameters = test || constant || service ? NULL : syntax->children[1];
    uint32_t slots = service_promise ? fn->shape.count - 1 : fn->shape.count;
    uint32_t parameter_count = constant || service ? 0
                               : spec == NULL      ? parameters->count
                               : promise           ? slots
                                                   : 0;
    const struct mo_syntax **inputs = NULL;
    struct note **spec_notes = NULL;
    if (promise || anchor) {
        inputs = infer_allocate(&p, slots, sizeof(*inputs), _Alignof(const struct mo_syntax *));
        if (inputs == NULL ||
            !infer_anchor_inputs(&p, anchor ? written : written->children[2]->children[0],
                                 parameters, slots, inputs))
            goto failed;
        uint32_t count = anchor ? slots : written->children[1]->count;
        spec_notes = infer_allocate(&p, count, sizeof(*spec_notes), _Alignof(struct note *));
        if (spec_notes == NULL)
            goto failed;
    }
    struct note **parameter_notes =
        checked == NULL ? NULL
                        : infer_allocate(&p, parameter_count, sizeof(*parameter_notes),
                                         _Alignof(struct note *));
    if (checked != NULL && parameter_notes == NULL)
        goto failed;
    for (uint32_t i = 0; i < parameter_count; i++) {
        struct term *type = infer_argument(&p, fn, i);
        const struct mo_syntax *name =
            i < parameters->count ? parameters->children[i]->children[0] : inputs[i]->children[0];
        if (type == NULL || !infer_bind(&p, name, type, false))
            goto failed;
        if (promise && p.locals != NULL)
            p.locals->used = true; /* A promise need not mention every subject parameter. */
        if (checked != NULL)
            parameter_notes[i] = p.last;
    }
    struct value v = {0};
    if (anchor) {
        const struct mo_syntax *values = written->children[0];
        for (uint32_t i = 0; i < values->count; i++) {
            uint32_t slot = 0;
            while (inputs[slot] != values->children[i]) {
                if (!infer_work(&p))
                    goto failed;
                slot++;
                MO_ASSERT(slot < slots);
            }
            v = infer_as(&p, inputs[slot]->children[1], infer_argument(&p, fn, slot),
                         MO_CODE_ARGUMENT_TYPE);
            if (v.type == NULL)
                goto failed;
            spec_notes[slot] = v.note;
        }
    } else if (promise) {
        const struct mo_syntax *lines = written->children[1];
        for (uint32_t i = 0; i < lines->count; i++) {
            struct value line = infer_as(&p, lines->children[i], p.result, MO_CODE_CONDITION_TYPE);
            if (line.type == NULL)
                goto failed;
            spec_notes[i] = line.note;
            if (i == 0)
                v = line;
            else {
                struct value both =
                    infer_graph(&p, (struct value){.type = p.result, .never = v.never}, 2);
                if (both.type == NULL)
                    goto failed;
                both.graph->output.operation = MO_T_AND;
                both.graph->operands[0] = v.note;
                both.graph->operands[1] = line.note;
                if (!infer_record(&p, written, both))
                    goto failed;
                both.note = p.last;
                v = both;
            }
        }
    } else if (spec != NULL)
        v = test ? infer_discard(&p, written->children[1])
                 : infer_as(&p, written, p.result, MO_CODE_CONDITION_TYPE);
    else if (service)
        v = infer_as(&p, syntax->children[syntax->kind == MO_S_SERVICE_STATE ? 1 : 0], p.result,
                     MO_CODE_SERVICE_VALUE);
    else if (constant)
        v = infer_as(&p, syntax->children[1], p.result, MO_CODE_RESULT_TYPE);
    else
        v = infer_named(p.result, "Unit")
                ? infer_discard(&p, syntax->children[5])
                : infer_as(&p, syntax->children[5], p.result, MO_CODE_RESULT_TYPE);
    if (v.type == NULL || !infer_leave(&p, NULL))
        goto failed;
    if (!infer_finish_dispatches(&p) || !infer_finish_wrapping(&p))
        goto failed;
    struct mo_inferred_body *out =
        infer_allocate(&p, 1, sizeof(*out), _Alignof(struct mo_inferred_body));
    struct mo_inferred_expression *items =
        infer_allocate(&p, p.count, sizeof(*items), _Alignof(struct mo_inferred_expression));
    if (p.failed)
        goto failed;
    *out = (struct mo_inferred_body){.function = signature, .count = p.count, .items = items};
    finalized = true;
    bool valid = infer_finish_notes(&p, items);
    (void)valid;
    if (!p.fatal && !infer_finish_patterns(&p)) {
        *halt = true;
        mo_diags_sort(diags);
        return NULL;
    }
    if (!infer_finish_captures(&p))
        goto failed;
    if (p.error.code != 0)
        goto failed;
    if (checked != NULL) {
        struct mo_checked_expression *rows =
            infer_allocate(&p, p.count, sizeof(*rows), _Alignof(struct mo_checked_expression));
        struct mo_checked_binding *bindings = infer_allocate(&p, p.binding_count, sizeof(*bindings),
                                                             _Alignof(struct mo_checked_binding));
        const struct mo_checked_binding **parameter_bindings =
            infer_allocate(&p, parameter_count, sizeof(*parameter_bindings),
                           _Alignof(const struct mo_checked_binding *));
        struct note **lambdas =
            infer_allocate(&p, p.lambda_count, sizeof(*lambdas), _Alignof(struct note *));
        struct note **notes = infer_allocate(&p, p.count, sizeof(*notes), _Alignof(struct note *));
        if (p.failed)
            goto failed;
        uint32_t count = 0, binding_count = 0, lambda_count = 0, note_count = 0;
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            notes[note_count++] = n;
            if (n->graph != NULL) {
                n->index = count++;
                if (n->graph->lambda != NULL)
                    lambdas[lambda_count++] = n;
            }
            if (n->binding != n)
                continue;
            const struct mo_syntax *name = n->output.syntax;
            uint32_t length = name->span.end - name->span.start;
            char *text = infer_allocate(&p, (size_t)length + 1, 1, 1);
            if (text == NULL)
                goto failed;
            memcpy(text, name->text, length);
            text[length] = '\0';
            n->binding_index = binding_count++;
            bindings[n->binding_index] = (struct mo_checked_binding){.syntax = name,
                                                                     .span = name->span,
                                                                     .name = text,
                                                                     .type = n->output.type,
                                                                     .variable = n->variable};
        }
        MO_ASSERT(binding_count == p.binding_count);
        MO_ASSERT(lambda_count == p.lambda_count);
        MO_ASSERT(note_count == p.count);
        qsort(notes, note_count, sizeof(*notes), infer_syntax_note_order);
        qsort(lambdas, lambda_count, sizeof(*lambdas), infer_source_note_order);
        for (uint32_t i = 0; i < lambda_count; i++)
            lambdas[i]->graph->lambda->output.number = i + 1;
        for (uint32_t i = 0; i < parameter_count; i++) {
            MO_ASSERT(parameter_notes[i]->binding_index < binding_count);
            parameter_bindings[i] = &bindings[parameter_notes[i]->binding_index];
        }
        for (struct note *n = p.notes; n != NULL; n = n->next) {
            if (n->graph == NULL)
                continue;
            struct emitted *g = n->graph;
            uint32_t *operands =
                infer_allocate(&p, g->output.operand_count, sizeof(*operands), _Alignof(uint32_t));
            if (operands == NULL)
                goto failed;
            for (uint32_t i = 0; i < g->output.operand_count; i++) {
                MO_ASSERT(g->operands[i] != NULL && g->operands[i]->index < n->index);
                operands[i] = g->operands[i]->index;
            }
            p.span = n->output.syntax->span;
            if (!mo_check_type_abilities(&p.abilities, n->output.type)) {
                bool reported = infer_problem(&p, p.abilities.error, p.span, p.abilities.message,
                                              p.abilities.reason, NULL);
                (void)reported;
                goto failed;
            }
            if (!infer_finish_operator(&p, n) || !infer_finish_function_use(&p, g) ||
                !infer_finish_interpolation(&p, n))
                goto failed;
            if (g->try_result != NULL) {
                struct mo_checked_try *try_ =
                    infer_allocate(&p, 1, sizeof(*try_), _Alignof(struct mo_checked_try));
                if (try_ == NULL)
                    goto failed;
                const struct mo_source_type *result = infer_finish_type(&p, g->try_result, 0);
                if (result == NULL)
                    goto failed;
                *try_ = (struct mo_checked_try){
                    .result = result,
                    .failure = {.builtin = infer_named(g->try_result, "Option") ? "None" : "Error"},
                    .wrapper = g->failure->wrapper};
                if (g->failure->generic != NULL &&
                    (!infer_type_arguments(&p, g->failure->generic, result->arguments[1],
                                           &try_->arguments) ||
                     !infer_data_requirements(&p, &try_->arguments, &try_->ability_count,
                                              &try_->abilities)))
                    goto failed;
                g->output.try_ = try_;
            }
            if (g->generic != NULL && g->generic->count != 0) {
                const struct mo_source_type *actual = infer_finish_type(&p, g->instantiated, 0);
                if (actual == NULL ||
                    !infer_type_arguments(&p, g->generic, actual, &g->output.arguments) ||
                    !infer_data_requirements(&p, &g->output.arguments, &g->output.ability_count,
                                             &g->output.abilities))
                    goto failed;
            }
            if (g->match != NULL) {
                struct emitted_match *match = g->match;
                match->output.scrutinee = operands[0];
                for (uint32_t i = 0; i < match->output.arm_count; i++) {
                    struct mo_checked_arm *arm = &match->arms[i];
                    if (arm->guard != UINT32_MAX)
                        arm->guard = operands[arm->guard];
                    arm->body = operands[arm->body];
                    const struct mo_checked_pattern **patterns =
                        infer_allocate(&p, arm->pattern_count, sizeof(*patterns),
                                       _Alignof(const struct mo_checked_pattern *));
                    if (patterns == NULL)
                        goto failed;
                    arm->patterns = patterns;
                    for (uint32_t j = 0; j < arm->pattern_count; j++) {
                        patterns[j] = infer_checked_pattern(
                            &p, arm->syntax->children[0]->children[j], g->operands[0]->output.type,
                            notes, note_count, bindings, binding_count, 0);
                        if (patterns[j] == NULL)
                            goto failed;
                    }
                }
                g->output.match = &match->output;
            } else if (n->output.syntax->kind == MO_S_IS) {
                g->output.pattern = infer_checked_pattern(&p, n->output.syntax->children[1],
                                                          g->operands[0]->output.type, notes,
                                                          note_count, bindings, binding_count, 0);
                if (g->output.pattern == NULL)
                    goto failed;
            }
            if (g->pattern_type != NULL) {
                const struct mo_source_type *type = infer_finish_type(&p, g->pattern_type, 0);
                if (type == NULL)
                    goto failed;
                g->output.pattern =
                    infer_checked_pattern(&p, n->output.syntax->children[0], type, notes,
                                          note_count, bindings, binding_count, 0);
                if (g->output.pattern == NULL)
                    goto failed;
            }
            if (g->lambda != NULL) {
                struct emitted_lambda *lambda = g->lambda;
                const struct mo_checked_binding **bound =
                    infer_allocate(&p, lambda->output.parameter_count, sizeof(*bound),
                                   _Alignof(const struct mo_checked_binding *));
                if (bound == NULL)
                    goto failed;
                for (uint32_t i = 0; i < lambda->output.parameter_count; i++) {
                    const struct note *parameter = lambda->parameters[i];
                    MO_ASSERT(parameter == NULL || parameter->binding_index < binding_count);
                    bound[i] = parameter == NULL ? NULL : &bindings[parameter->binding_index];
                }
                lambda->output.parameters = bound;
                const struct mo_checked_binding **captures =
                    infer_allocate(&p, lambda->output.capture_count, sizeof(*captures),
                                   _Alignof(const struct mo_checked_binding *));
                if (captures == NULL)
                    goto failed;
                uint32_t at = 0;
                for (const struct capture *c = lambda->captures; c != NULL; c = c->next) {
                    MO_ASSERT(c->local->binding->binding_index < binding_count);
                    captures[at++] = &bindings[c->local->binding->binding_index];
                }
                MO_ASSERT(at == lambda->output.capture_count);
                lambda->output.captures = captures;
                lambda->output.body = operands[0];
                g->output.lambda = &lambda->output;
            }
            rows[n->index] = g->output;
            rows[n->index].syntax = n->output.syntax;
            rows[n->index].type = n->output.type;
            rows[n->index].operands = operands;
            rows[n->index].never = n->output.never;
            if (g->output.receiver != UINT32_MAX) {
                MO_ASSERT(g->output.receiver < g->output.operand_count);
                rows[n->index].receiver = operands[g->output.receiver];
            }
            if (n->binding != NULL) {
                MO_ASSERT(n->binding->binding_index < binding_count);
                rows[n->index].binding = &bindings[n->binding->binding_index];
            }
        }
        if (promise) {
            MO_ASSERT(conditions != NULL);
            for (uint32_t i = 0; i < spec->condition_count; i++)
                conditions[i].expression = spec_notes[i]->index;
        }
        if (anchor) {
            uint32_t *values = infer_allocate(&p, slots, sizeof(*values), _Alignof(uint32_t));
            if (values == NULL || infer_finish_type(&p, fn, 0) == NULL ||
                !infer_finish_function_use(&p, instance.graph))
                goto failed;
            for (uint32_t i = 0; i < slots; i++)
                values[i] = spec_notes[i]->index;
            spec->value_count = slots;
            spec->values = values;
            spec->arguments = instance.graph->output.arguments;
            spec->ability_count = instance.graph->output.ability_count;
            spec->abilities = instance.graph->output.abilities;
        }
        *checked =
            (struct mo_checked_function){.signature = signature,
                                         .parameter_count = parameter_count,
                                         .parameters = parameter_bindings,
                                         .body = {.count = count,
                                                  .items = rows,
                                                  .root = anchor ? UINT32_MAX : v.note->index,
                                                  .capture_free_parameters = p.capture_free,
                                                  .binding_count = binding_count,
                                                  .bindings = bindings}};
    }
    return out;
failed:
    if (constants != NULL && constants->pending != NULL)
        return NULL; /* The explicit constant driver resumes after its dependency. */
    if (!p.fatal && !finalized) {
        p.failed = false;
        p.partial = true;
        bool partial_valid = infer_finish_notes(&p, NULL);
        (void)partial_valid;
        if (!p.fatal && !infer_finish_patterns(&p)) {
            *halt = true;
            mo_diags_sort(diags);
            return NULL;
        }
        if (!p.fatal) {
            bool captures_valid = infer_finish_captures(&p);
            (void)captures_valid;
        }
    }
    infer_emit_problem(&p);
    *halt = p.error.code >= 1000 || (diags->dropped != 0 && diags->count < MO_LIMIT_DIAGNOSTICS);
    mo_diags_sort(diags);
    return NULL;
}

struct mo_inferred_body *mo_infer_function(struct mo_arena *arena,
                                           const struct mo_module_interfaces *interfaces,
                                           const struct mo_signatures *signatures,
                                           uint32_t function, struct mo_diags *diags) {
    bool halt;
    MO_ASSERT(function < signatures->count);
    return infer_function(arena, interfaces, signatures, NULL, &signatures->items[function], diags,
                          NULL, &halt, NULL, NULL);
}

bool mo_check_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures,
                       struct mo_constant_context *constants, uint32_t function,
                       struct mo_diags *diags, struct mo_checked_function *out, bool *halt) {
    MO_ASSERT(out != NULL && function < signatures->count);
    return infer_function(arena, interfaces, signatures, constants, &signatures->items[function],
                          diags, out, halt, NULL, NULL) != NULL;
}

bool mo_check_spec_body(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                        const struct mo_signatures *signatures,
                        struct mo_constant_context *constants,
                        const struct mo_function_signature *signature, struct mo_checked_spec *spec,
                        struct mo_checked_condition *conditions, struct mo_diags *diags,
                        bool *halt) {
    MO_ASSERT(spec != NULL && spec->syntax != NULL);
    struct mo_checked_function checked;
    if (infer_function(arena, interfaces, signatures, constants, signature, diags, &checked, halt,
                       spec, conditions) == NULL)
        return false;
    spec->body = checked.body;
    spec->parameter_count = checked.parameter_count;
    spec->parameters = checked.parameters;
    return true;
}

bool mo_check_constant(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                       const struct mo_signatures *signatures,
                       struct mo_constant_context *constants, struct mo_checked_constant *constant,
                       struct mo_diags *diags, bool *halt) {
    struct mo_source_type unit = {.kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
    const struct mo_source_type *result = &unit;
    struct mo_source_type type = {.kind = MO_SOURCE_FUNCTION,
                                  .name = "fn",
                                  .count = 1,
                                  .module = UINT32_MAX,
                                  .arguments = &result,
                                  .height = 1};
    struct mo_function_signature context = {.function = constant->symbol, .type = &type};
    struct mo_checked_function checked;
    if (infer_function(arena, interfaces, signatures, constants, &context, diags, &checked, halt,
                       NULL, NULL) == NULL)
        return false;
    const struct mo_source_type *value = checked.body.items[checked.body.root].type;
    if (constant->symbol->name[constant->symbol->name_length - 1] == '?') {
        const struct mo_source_type *answer =
            value->kind == MO_SOURCE_FUNCTION ? value->arguments[value->count - 1] : value;
        if (answer->kind != MO_SOURCE_NAMED || answer->module != UINT32_MAX ||
            strcmp(answer->name, "Bool") != 0) {
            bool kept = mo_diags_add(
                diags, &(struct mo_diag){
                           .code = MO_CODE_BOOL_NAME,
                           .primary = {constant->symbol->declaration->children[0]->span,
                                       "must be Bool or a function returning Bool"},
                           .message = "a name ending in ? isn't a yes-or-no answer",
                           .reason = "A name ending in ? is a Bool, or a function that returns "
                                     "one (reference 2.3)."});
            (void)kept;
            *halt = diags->dropped != 0 && diags->count < MO_LIMIT_DIAGNOSTICS;
            return false;
        }
    }
    constant->initializer = checked.body;
    constant->type = value;
    return true;
}

bool mo_check_service_value(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                            const struct mo_signatures *signatures,
                            struct mo_constant_context *constants, uint32_t module,
                            const struct mo_syntax *clause, const struct mo_source_type *expected,
                            struct mo_diags *diags, struct mo_checked_body *out) {
    MO_ASSERT(clause->kind == MO_S_SERVICE_STATE || clause->kind == MO_S_SERVICE_EVERY);
    struct mo_symbol owner = {.name = "service", .declaration = clause, .module = module};
    struct mo_source_type type = {.kind = MO_SOURCE_FUNCTION,
                                  .name = "fn",
                                  .module = UINT32_MAX,
                                  .count = 1,
                                  .arguments = &expected,
                                  .height = expected->height + 1};
    struct mo_function_signature signature = {.function = &owner, .type = &type};
    struct mo_checked_function checked;
    bool halt;
    if (infer_function(arena, interfaces, signatures, constants, &signature, diags, &checked, &halt,
                       NULL, NULL) == NULL)
        return false;
    *out = checked.body;
    return true;
}
