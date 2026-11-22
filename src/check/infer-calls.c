#include "check/infer.h"
#include "check/signatures.h"

#include "base/codes.h"
#include "base/simops.h"

#include <string.h>

MO_MUST_USE struct term *infer_builtin_type(struct inference *p, const struct mo_builtin_case *data,
                                            struct term *expected) {
    struct term *type = infer_native(p, data->type, data->parameters);
    if (type == NULL)
        return NULL;
    for (uint32_t i = 0; i < data->parameters; i++) {
        type->arguments[i] = infer_fresh(p, 0);
        if (type->arguments[i] == NULL)
            return NULL;
    }
    if (expected != NULL && !infer_unify(p, type, expected, 0))
        return NULL;
    return type;
}

MO_MUST_USE uint32_t infer_builtin_field(const struct mo_builtin_case *data,
                                         const struct mo_syntax *arg) {
    if (data->fields[0] == NULL)
        return arg->kind == MO_S_NAMED_ARG ? UINT32_MAX : 0;
    if (arg->kind == MO_S_NAMED_ARG)
        for (uint32_t i = 0; i < data->count; i++)
            if (infer_compare_text(arg->children[0], data->fields[i]) == 0)
                return i;
    return UINT32_MAX;
}

MO_MUST_USE struct term *infer_builtin_part(struct inference *p, const struct mo_builtin_case *data,
                                            struct term *type, uint32_t index) {
    MO_ASSERT(index < data->count);
    if (strcmp(data->type, "Sqlite.Value") == 0) {
        const char *kind = strcmp(data->name, "Integer") == 0  ? "Int"
                           : strcmp(data->name, "Real") == 0   ? "Float"
                           : strcmp(data->name, "String") == 0 ? "Text"
                                                               : "Bytes";
        return infer_native(p, kind, 0);
    }
    if (strcmp(data->type, "Sqlite.Rows") == 0) {
        struct term *element = NULL;
        if (index == 0)
            element = infer_native(p, "Text", 0);
        else {
            element = infer_native(p, "List", 1);
            if (element == NULL)
                return NULL;
            element->arguments[0] = infer_native(p, "Sqlite.Value", 0);
            if (element->arguments[0] == NULL)
                return NULL;
        }
        if (element == NULL)
            return NULL;
        struct term *list = infer_native(p, "List", 1);
        if (list == NULL)
            return NULL;
        list->arguments[0] = element;
        return list;
    }
    if (data->fields[0] == NULL)
        return infer_argument(p, type, data->parameter);
    return infer_native(p,
                        index == 0                           ? "Text"
                        : strcmp(data->name, "Entry") == 0   ? "EntryKind"
                        : strcmp(data->name, "Refused") == 0 ? "Int"
                                                             : "Text",
                        0);
}

MO_MUST_USE struct value infer_builtin_construct(struct inference *p,
                                                 const struct mo_builtin_case *data,
                                                 const struct mo_syntax *args,
                                                 struct term *expected) {
    struct term *type = infer_builtin_type(p, data, expected);
    if (type == NULL)
        return (struct value){0};
    if ((args == NULL ? 0 : args->count) < data->count) {
        bool provided[2] = {false, false};
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            uint32_t index = infer_builtin_field(data, args->children[i]);
            if (index < data->count)
                provided[index] = true;
        }
        for (uint32_t i = 0; i < data->count; i++) {
            if (provided[i])
                continue;
            const char *name = data->fields[i];
            bool reported =
                infer_missing_field(p, args == NULL ? p->span : args->span, (const uint8_t *)name,
                                    name == NULL ? 0 : strlen(name), NULL);
            (void)reported;
            return (struct value){0};
        }
        MO_ASSERT(false);
    }
    if ((args == NULL ? 0 : args->count) != data->count) {
        bool reported =
            infer_problem(p, MO_CODE_DATA_ARGUMENTS, args == NULL ? p->span : args->span,
                          "the case hasn't been given every field",
                          "A case is built with every named field or its one unnamed "
                          "value (reference 4).",
                          NULL);
        (void)reported;
        return (struct value){0};
    }
    bool seen[2] = {false, false};
    struct value out =
        infer_graph(p, (struct value){.type = type, .builtin_case = data->name}, data->count);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->generic = mo_builtin_template(data);
        out.graph->instantiated = type;
    }
    for (uint32_t i = 0; args != NULL && i < args->count; i++) {
        const struct mo_syntax *arg = args->children[i];
        uint32_t index = infer_builtin_field(data, arg);
        if (index == UINT32_MAX || seen[index]) {
            bool reported = infer_problem(p, MO_CODE_DATA_ARGUMENTS, arg->span,
                                          "a supplied field doesn't match the case",
                                          "A case is built with every named field once or its "
                                          "one unnamed value (reference 4).",
                                          NULL);
            (void)reported;
            return (struct value){0};
        }
        seen[index] = true;
        struct term *field = infer_builtin_part(p, data, type, index);
        if (field == NULL)
            return (struct value){0};
        struct value v =
            infer_expression(p, arg->kind == MO_S_NAMED_ARG ? arg->children[1] : arg, field);
        if (v.type == NULL)
            return (struct value){0};
        if (p->checked) {
            if (arg->kind == MO_S_NAMED_ARG) {
                struct value supplied =
                    infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                if (supplied.type == NULL)
                    return supplied;
                supplied.graph->output.field = index;
                supplied.graph->operands[0] = v.note;
                if (!infer_record(p, arg, supplied))
                    return (struct value){0};
                v.note = p->last;
            }
            out.graph->operands[i] = v.note;
        }
        out.never = out.never || v.never;
    }
    return out;
}

MO_MUST_USE struct value infer_construct(struct inference *p, const struct mo_data_signature *data,
                                         const struct mo_syntax *args, struct term *expected) {
    if (!infer_visible_data(p, data->type->declaration))
        return (struct value){0};
    struct term *parameters[26] = {NULL};
    struct term *type = infer_annotation(p, data->type, parameters, 0);
    if (type == NULL || (expected != NULL && !infer_unify(p, type, expected, 0)))
        return (struct value){0};
    struct value out =
        infer_graph(p, (struct value){.type = type, .symbol = data->symbol}, args->count);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->generic = data->type;
        out.graph->instantiated = type;
    }
    bool *seen = infer_allocate(p, data->count, sizeof(*seen), _Alignof(bool));
    const struct mo_field_signature **selected = infer_allocate(
        p, args->count, sizeof(*selected), _Alignof(const struct mo_field_signature *));
    struct note_range *ranges =
        infer_allocate(p, args->count, sizeof(*ranges), _Alignof(struct note_range));
    if (p->failed)
        return (struct value){0};
    memset(seen, 0, data->count * sizeof(*seen));
    for (uint32_t i = 0; i < args->count; i++) {
        const struct mo_syntax *arg = args->children[i];
        const struct mo_field_signature *field =
            data->unnamed && arg->kind != MO_S_NAMED_ARG ? &data->fields[0]
            : arg->kind == MO_S_NAMED_ARG                ? infer_find_field(data, arg->children[0])
                                                         : NULL;
        if (field == NULL || seen[field->index]) {
            bool reported =
                infer_problem(p, MO_CODE_DATA_ARGUMENTS, arg->span,
                              "a supplied field doesn't match the record or case",
                              "A record or case is built by naming every field once, or "
                              "from its one unnamed value (reference 4).",
                              field == NULL ? data->symbol->declaration->children[0] : field->name);
            (void)reported;
            return (struct value){0};
        }
        seen[field->index] = true;
        selected[i] = field;
    }
    for (uint32_t i = 0; i < data->count; i++) {
        if (seen[i])
            continue;
        const struct mo_syntax *name = data->by_index[i]->name;
        bool reported =
            infer_missing_field(p, args->span, name == NULL ? NULL : name->text,
                                name == NULL ? 0 : name->span.end - name->span.start, name);
        (void)reported;
        return (struct value){0};
    }
    bool never = false;
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            const struct mo_syntax *value = data->unnamed ? arg : arg->children[1];
            if (infer_lambda_argument(value) != (pass != 0))
                continue;
            struct term *wanted = infer_annotation(p, selected[i]->type, parameters, 0);
            if (wanted == NULL)
                return (struct value){0};
            struct note *previous = p->last;
            struct value v = infer_as(p, value, wanted, MO_CODE_ARGUMENT_TYPE);
            if (v.type == NULL)
                return (struct value){0};
            p->last->output.field = selected[i];
            if (p->checked) {
                if (!data->unnamed) {
                    struct value supplied =
                        infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                    if (supplied.type == NULL)
                        return supplied;
                    supplied.graph->output.field = selected[i]->index;
                    supplied.graph->operands[0] = v.note;
                    if (!infer_record(p, arg, supplied))
                        return (struct value){0};
                    v.note = p->last;
                }
                out.graph->operands[i] = v.note;
            }
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            never = never || v.never;
        }
    }
    infer_order_notes(p, before, ranges, args->count);
    out.never = never;
    return out;
}

MO_MUST_USE struct value infer_ability_function(struct inference *p, struct value receiver,
                                                const struct mo_syntax *name) {
    bool text = infer_compare_text(name, "to_text") == 0;
    bool json = infer_compare_text(name, "to_json") == 0;
    struct term *result_term = NULL;
    if (json) {
        const struct mo_source_type *json_ty = mo_json_type(p->interfaces, p->signatures, "Json");
        if (json_ty == NULL) {
            bool reported = infer_problem(
                p, MO_CODE_IMPORT_MISSING, name->span, "to_json needs use Json",
                "Bring the Json module into scope with use Json (reference 9.2, 10.3).", NULL);
            (void)reported;
            return (struct value){0};
        }
        result_term = infer_annotation(p, json_ty, NULL, 0);
    } else
        result_term = infer_native(p, text ? "Text" : "Ordering", 0);
    struct term *type =
        infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                   .name = "fn",
                                                   .module = UINT32_MAX,
                                                   .count = text || json ? 2u : 3u});
    if (type == NULL || result_term == NULL)
        return (struct value){0};
    type->arguments[0] = receiver.type;
    if (!text && !json)
        type->arguments[1] = receiver.type;
    type->arguments[type->shape.count - 1] = result_term;
    if (p->failed)
        return (struct value){0};
    struct value out = infer_graph(p, (struct value){.type = type}, 0);
    if (p->checked && out.type != NULL)
        out.graph->output.native = json ? "to_json" : text ? "to_text" : "compare";
    return out;
}

/* Resolve a member without walking its explicit arguments. A method returns
 * its function type; a visible field returns its projection. */
MO_MUST_USE struct value infer_member(struct inference *p, struct value v,
                                      const struct mo_syntax *name, bool *called) {
    *called = true;
    if (v.type == NULL)
        return v;
    p->span = name->span;
    struct term *type = infer_root(v.type);
    MO_ASSERT(!type->variable);
    if (!type->variable && type->shape.kind == MO_SOURCE_NAMED &&
        type->shape.module == UINT32_MAX) {
        if (infer_named(type, "Entry") &&
            (infer_compare_text(name, "name") == 0 || infer_compare_text(name, "kind") == 0)) {
            *called = false;
            uint32_t index = infer_compare_text(name, "name") == 0 ? 0u : 1u;
            const struct mo_builtin_case *data = mo_builtin_case_by_type("Entry");
            struct value out = infer_graph(
                p,
                (struct value){.type = infer_builtin_part(p, data, type, index), .never = v.never},
                1);
            if (p->checked && out.type != NULL) {
                out.graph->output.field = index;
                out.graph->output.native = index == 0 ? "Entry.name" : "Entry.kind";
                out.graph->operands[0] = v.note;
            }
            return out;
        }
        if (infer_named(type, "Sqlite.Rows") &&
            (infer_compare_text(name, "columns") == 0 || infer_compare_text(name, "rows") == 0)) {
            *called = false;
            uint32_t index = infer_compare_text(name, "columns") == 0 ? 0u : 1u;
            const struct mo_builtin_case *data = mo_builtin_case_by_type("Sqlite.Rows");
            struct value out = infer_graph(
                p,
                (struct value){.type = infer_builtin_part(p, data, type, index), .never = v.never},
                1);
            if (p->checked && out.type != NULL) {
                out.graph->output.field = index;
                out.graph->output.native = index == 0 ? "Sqlite.Rows.columns" : "Sqlite.Rows.rows";
                out.graph->operands[0] = v.note;
            }
            return out;
        }
        const char *module =
            strcmp(type->shape.name, "Sqlite.Database") == 0 ? "Sqlite" : type->shape.name;
        const struct mo_native_signature *signature = infer_native_definition(p, module, name);
        if (signature != NULL)
            return infer_native_identifier(p, signature);
    }
    if (!type->variable && type->shape.kind == MO_SOURCE_NAMED && type->shape.declaration != NULL &&
        type->shape.declaration->declaration->kind == MO_S_ACTOR) {
        const struct mo_program *program = p->signatures->service_program;
        const struct mo_checked_service *service = NULL;
        uint32_t service_index = 0;
        if (program != NULL) {
            for (uint32_t i = 0; i < program->service_count; i++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (program->services[i].is_actor &&
                    program->services[i].actor_symbol == type->shape.declaration) {
                    service = &program->services[i];
                    service_index = i;
                    break;
                }
            }
        }
        if (service != NULL) {
            for (uint32_t ti = 0; ti < service->tool_count; ti++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (infer_compare_name(name, service->tools[ti].name) != 0)
                    continue;
                const struct mo_source_type *sig =
                    program->functions[service->tools[ti].function].signature->type;
                MO_ASSERT(sig->count >= 3);
                uint32_t fn_arity = sig->count - 1;
                struct term *fn_term =
                    infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                               .name = "fn",
                                                               .module = UINT32_MAX,
                                                               .count = fn_arity});
                if (fn_term == NULL)
                    return (struct value){0};
                fn_term->arguments[0] = type;
                for (uint32_t k = 2; k < sig->count - 1; k++) {
                    fn_term->arguments[k - 1] = infer_annotation(p, sig->arguments[k], NULL, 0);
                    if (fn_term->arguments[k - 1] == NULL)
                        return (struct value){0};
                }
                const struct mo_source_type *raw_ret = sig->arguments[sig->count - 1];
                struct term *reply_term = NULL;
                if (service->tools[ti].reply == MO_SERVICE_READ_ONLY) {
                    reply_term = infer_annotation(p, raw_ret, NULL, 0);
                } else if (service->tools[ti].reply == MO_SERVICE_UPDATE) {
                    reply_term = infer_annotation(p, raw_ret->arguments[1], NULL, 0);
                } else {
                    reply_term = infer_native(p, "Result", 2);
                    if (reply_term != NULL) {
                        reply_term->arguments[0] =
                            infer_annotation(p, raw_ret->arguments[0]->arguments[1], NULL, 0);
                        reply_term->arguments[1] =
                            infer_annotation(p, raw_ret->arguments[1], NULL, 0);
                        if (reply_term->arguments[0] == NULL || reply_term->arguments[1] == NULL)
                            return (struct value){0};
                    }
                }
                if (reply_term == NULL)
                    return (struct value){0};
                fn_term->arguments[fn_arity - 1] = reply_term;
                struct value out = infer_graph(p, (struct value){.type = fn_term}, 0);
                if (p->checked && out.type != NULL) {
                    out.graph->output.native = "Actor.call";
                    out.graph->output.service = service_index;
                    out.graph->output.tool = ti;
                }
                return out;
            }
        }
    }
    const struct mo_data_signature *data = type->variable || type->shape.declaration == NULL
                                               ? NULL
                                               : infer_data_type(p, type->shape.declaration);
    const struct mo_field_signature *field = data == NULL ? NULL : infer_find_field(data, name);
    bool visible = data != NULL && (type->shape.module == p->function->function->module ||
                                    type->shape.declaration->insides);
    if ((field == NULL || !visible) &&
        (infer_compare_text(name, "to_text") == 0 || infer_compare_text(name, "compare") == 0 ||
         infer_compare_text(name, "to_json") == 0))
        return infer_ability_function(p, v, name);
    if (field == NULL || !visible) {
        uint32_t count = 0;
        const struct mo_module_scope *owner = infer_type_scope(p, type);
        if (p->failed)
            return (struct value){0};
        const struct mo_symbol *method =
            owner == NULL ? NULL : infer_find_symbol(owner, name, &count);
        bool plausible = method != NULL && method->declaration->kind == MO_S_FN &&
                         (method->module == p->function->function->module || method->exported);
        if (!plausible) {
            method = infer_definition(p, name, NULL, &count);
            plausible = method != NULL && method->declaration->kind == MO_S_FN;
        }
        if (plausible) {
            method = infer_select_overload(p, method, count, type);
            if (method == NULL)
                return (struct value){0};
            return infer_declared_function(p, method);
        }
    }
    if (type->shape.declaration == NULL) {
        const char *habit = infer_length_habit(name);
        if (infer_named(type, "Simulation"))
            habit = "Simulation operations are called through the module, as in "
                    "Simulation.crash(sim), not as dot calls (reference 12.4).";
        bool reported = infer_problem(p, MO_CODE_NAME_UNKNOWN, name->span,
                                      "no visible function has this dot-call name",
                                      habit != NULL ? habit
                                                    : "A dot call looks in the type's module, then "
                                                      "among the names in scope (reference 5.4).",
                                      NULL);
        (void)reported;
        return (struct value){0};
    }
    if (type->shape.declaration != NULL &&
        type->shape.declaration->declaration->kind == MO_S_ACTOR) {
        bool reported = infer_problem(
            p, MO_CODE_NAME_UNKNOWN, name->span, "the actor declares no such call",
            "Actor dot calls invoke handlers listed in the actor's calls clause, or visible "
            "functions taking the actor handle first (reference 11.7).",
            type->shape.declaration->declaration->children[0]);
        (void)reported;
        return (struct value){0};
    }
    if (!infer_visible_data(p, type->shape.declaration))
        return (struct value){0};
    if (field == NULL) {
        bool reported =
            infer_problem(p, MO_CODE_FIELD_MISSING, name->span, "the record has no such field",
                          "A struct declares named fields, each with a type (reference 4.1).",
                          type->shape.declaration->declaration->children[0]);
        (void)reported;
        return (struct value){0};
    }
    *called = false;
    struct term *parameters[26] = {NULL};
    for (uint32_t i = 0; i < type->shape.count; i++) {
        MO_ASSERT(data->parameters != NULL && i < data->parameters->count);
        parameters[data->parameters->children[i]->text[0] - 'A'] = infer_argument(p, type, i);
        if (p->failed)
            return (struct value){0};
    }
    struct value out =
        infer_graph(p,
                    (struct value){.type = infer_annotation(p, field->type, parameters, 0),
                                   .field = field,
                                   .never = v.never},
                    1);
    if (p->checked && out.type != NULL) {
        out.graph->output.field = field->index;
        out.graph->operands[0] = v.note;
        out.graph->generic = data->type;
        out.graph->instantiated = type;
    }
    return out;
}

bool infer_receiver_only(struct term *type) {
    type = infer_root(type);
    return !type->variable && type->shape.kind == MO_SOURCE_FUNCTION && type->shape.count == 2;
}

/* `x.f()` where `f` takes only the receiver: reference 5.4 spells it `x.f`. A
 * field holding a function is called with `()` and never reaches here. */
MO_MUST_USE bool infer_empty_dot_call(struct inference *p, const struct mo_syntax *site) {
    struct mo_span span = site->span;
    bool reported =
        infer_problem(p, MO_CODE_DOT_CALL_PARENTHESES, span, "write x.f, not x.f()",
                      "x.f, with no other arguments, calls f(x) (reference 5.4).", NULL);
    (void)reported;
    if (p->error.code == MO_CODE_DOT_CALL_PARENTHESES &&
        p->error.primary.span.start == span.start) {
        p->edit = (struct mo_edit){span, ""};
        p->fix = (struct mo_fix){"remove the parentheses", &p->edit, 1};
        p->error.fixes = &p->fix;
        p->error.fix_count = 1;
    }
    return false;
}

MO_MUST_USE struct value infer_field_value(struct inference *p, struct value receiver,
                                           const struct mo_syntax *name,
                                           const struct mo_syntax *syntax,
                                           const struct mo_syntax *site, bool *called) {
    if (receiver.type == NULL)
        return receiver;
    const struct mo_syntax *args = site == NULL ? NULL : site->children[0];
    if (infer_root(receiver.type)->variable) {
        *called = true;
        uint32_t count = args == NULL ? 0u : args->count;
        struct value out = infer_deferred_function(p, name, NULL, count);
        if (out.type == NULL)
            return out;
        struct dispatch *pending = out.dispatch;
        pending->syntax = syntax;
        pending->call = site;
        pending->arguments = args;
        pending->receiver = receiver;
        pending->inputs =
            infer_allocate(p, count, sizeof(*pending->inputs), _Alignof(struct value));
        if (pending->inputs == NULL)
            return (struct value){0};
        out.never = receiver.never;
        for (uint32_t i = 0; i < count; i++) {
            if (args->children[i]->kind == MO_S_NAMED_ARG) {
                bool reported =
                    infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                  "a function is called with named fields",
                                  "Arguments are passed by position (reference 5.2).", NULL);
                (void)reported;
                return (struct value){0};
            }
            pending->inputs[i] = infer_expression(p, args->children[i], NULL);
            if (pending->inputs[i].type == NULL)
                return (struct value){0};
            out.never = out.never || pending->inputs[i].never;
        }
        return out;
    }
    struct value resolved = infer_member(p, receiver, name, called);
    if (resolved.type == NULL || !*called)
        return resolved;
    if (args != NULL && args->count == 0 && infer_receiver_only(resolved.type) &&
        !infer_empty_dot_call(p, site))
        return (struct value){0};
    if (p->checked && resolved.symbol != NULL) {
        if (!infer_record(p, name, resolved))
            return (struct value){0};
        resolved.note = p->last;
    }
    return infer_call(p, resolved, args, &receiver);
}

MO_MUST_USE struct value infer_simulation_call(struct inference *p, const struct mo_syntax *name,
                                               const struct mo_syntax *args) {
    static const struct {
        const char *name, *qualified;
        enum mo_sim_operation operation;
    } operations[] = {
#define HOSTOP(id, module, member, signature, params, bugs, spec_only)
#define SIMOP(id, module, member, spec_only) {member, module "." member, MO_SIM_##id},
#include "base/hostops.def"
    };
    uint32_t op = 0;
    while (op < MO_SIM_COUNT && infer_compare_text(name, operations[op].name) != 0)
        op++;
    if (op == MO_SIM_COUNT) {
        bool kept = infer_problem(p, MO_CODE_NAME_UNKNOWN, name->span, infer_unknown_member,
                                  "Simulation's operations are start, send, advance, crash and "
                                  "restart (reference 12.4).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    if (args == NULL) {
        bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, name->span,
                                  "a simulation operation needs a direct call",
                                  "Simulation operations are not values; call one directly, as in "
                                  "Simulation.crash(sim) (reference 12.4).",
                                  NULL);
        (void)kept;
        if (p->error.code == MO_CODE_CALL_ARGUMENTS &&
            p->error.primary.span.start == name->span.start)
            p->error.primary.text = "call this operation directly";
        return (struct value){0};
    }
    enum mo_sim_operation operation = operations[op].operation;
    if (operation == MO_SIM_START && !p->spec) {
        bool kept = infer_problem(p, MO_CODE_FAKE_OUTSIDE_SPEC, name->span,
                                  "a fake capability is made only in a test, example or promise",
                                  "Fakes are made only in tests, examples and promises, never in a "
                                  "module's constants or its functions (reference 12.4).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    uint32_t selector = operation == MO_SIM_START ? 0 : operation == MO_SIM_SEND ? 1 : UINT32_MAX;
    uint32_t count = operation == MO_SIM_START || operation == MO_SIM_ADVANCE ? 2 : 1;
    uint32_t service_index = 0, tool_index = 0;
    const struct mo_source_type *signature = NULL;
    if ((operation == MO_SIM_SEND && args->count < 2) ||
        (operation != MO_SIM_SEND && args->count != count))
        goto bad_arguments;
    if (selector != UINT32_MAX) {
        const struct mo_syntax *prefix = args->count > selector ? args->children[selector] : NULL;
        const struct mo_syntax *tool = NULL;
        if (operation == MO_SIM_SEND) {
            if (prefix != NULL && prefix->count == 2 &&
                (prefix->kind == MO_S_FIELD_ACCESS || prefix->kind == MO_S_QUALIFIED)) {
                tool = prefix->children[1];
                prefix = prefix->children[0];
            } else if (prefix != NULL && prefix->kind == MO_S_POSTFIX && prefix->count == 2 &&
                       (prefix->children[1]->kind == MO_S_FIELD_ACCESS ||
                        prefix->children[1]->kind == MO_S_QUALIFIED)) {
                tool = prefix->children[1]->children[0];
                prefix = prefix->children[0];
            } else
                prefix = NULL;
        }
        const struct mo_module_scope *scope = NULL;
        uint32_t alias = UINT32_MAX;
        const struct mo_program *program = p->signatures->service_program;
        if (prefix != NULL && prefix->kind == MO_S_CAPNAME) {
            const struct mo_module_scope *own =
                &p->interfaces->scopes[p->function->function->module];
            const struct mo_syntax *path =
                program == NULL ? NULL
                                : program->set->modules[p->function->function->module]->header.path;
            bool current = own->name != NULL
                               ? infer_compare_text(prefix, own->name) == 0
                               : path != NULL && path->count == 1 &&
                                     infer_compare_name(prefix, path->children[0]) == 0;
            scope = current ? own : infer_prefix_module(p, prefix, &alias);
        }
        const struct mo_checked_service *service = NULL;
        bool named_actor = false;
        if (scope != NULL && program != NULL) {
            for (uint32_t i = 0; i < program->service_count; i++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (&p->interfaces->scopes[program->services[i].module] == scope) {
                    if (program->services[i].is_actor) {
                        named_actor = true;
                        break;
                    }
                    service = &program->services[i];
                    service_index = i;
                    break;
                }
            }
        }
        if (service != NULL && operation == MO_SIM_SEND) {
            for (tool_index = 0; tool_index < service->tool_count; tool_index++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (infer_compare_name(tool, service->tools[tool_index].name) == 0) {
                    signature =
                        program->functions[service->tools[tool_index].function].signature->type;
                    count = signature->count - 1;
                    break;
                }
            }
        }
        if (service == NULL || (operation == MO_SIM_SEND && signature == NULL)) {
            bool start = operation == MO_SIM_START;
            bool kept = infer_problem(
                p, MO_CODE_SERVICE_HANDLER, prefix == NULL ? args->span : prefix->span,
                start ? "Simulation.start must name a service module"
                      : "Simulation.send must name a registered tool",
                named_actor
                    ? "That module declares an actor, not a service; use Actor.start and actor "
                      "dot calls or Actor.send instead (reference 11.7)."
                : start ? "Name the current module or one it uses directly, as in "
                          "Simulation.start(TaskQueue, at); that module must declare a "
                          "service (reference 12.4)."
                        : "Write the service module and one of its tools directly, as in "
                          "TaskQueue.submit; timers and other functions are not tools "
                          "(reference 12.4).",
                NULL);
            (void)kept;
            return (struct value){0};
        }
        if (!infer_use_alias(p, alias, prefix))
            return (struct value){0};
    }
    if (args->count != count)
        goto bad_arguments;
    struct term *result = infer_native(p,
                                       operation == MO_SIM_START  ? "Simulation"
                                       : operation == MO_SIM_SEND ? "Result"
                                                                  : "Unit",
                                       operation == MO_SIM_SEND ? 2 : 0);
    if (result == NULL)
        return (struct value){0};
    if (operation == MO_SIM_SEND) {
        result->arguments[0] =
            infer_annotation(p, signature->arguments[signature->count - 1], NULL, 0);
        result->arguments[1] = infer_native(p, "SimulationError", 0);
        if (result->arguments[0] == NULL || result->arguments[1] == NULL)
            return (struct value){0};
    }
    struct value out =
        infer_graph(p, (struct value){.type = result}, count - (selector != UINT32_MAX));
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->output.native = operations[op].qualified;
        out.graph->output.service = service_index;
        out.graph->output.tool = tool_index;
    }
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (i == selector)
            continue;
        struct term *wanted = operation == MO_SIM_START ? infer_native(p, "Time", 0)
                              : i == 0                  ? infer_native(p, "Simulation", 0)
                              : operation == MO_SIM_ADVANCE
                                  ? infer_native(p, "Duration", 0)
                                  : infer_annotation(p, signature->arguments[i], NULL, 0);
        if (wanted == NULL)
            return (struct value){0};
        struct value input = infer_as(p, args->children[i], wanted, MO_CODE_ARGUMENT_TYPE);
        if (input.type == NULL)
            return (struct value){0};
        if (p->checked)
            out.graph->operands[emitted] = input.note;
        emitted++;
        out.never = out.never || input.never;
    }
    return out;

bad_arguments : {
    const char *reasons[MO_SIM_COUNT] = {
        [MO_SIM_START] = "Simulation.start takes a service module and a start Time.",
        [MO_SIM_SEND] = ("Simulation.send takes the handle, Module.tool, then the tool's arguments "
                         "after State and Time, which send supplies."),
        [MO_SIM_ADVANCE] = "Simulation.advance takes the handle and a Duration.",
        [MO_SIM_CRASH] = "Simulation.crash takes only the handle.",
        [MO_SIM_RESTART] = "Simulation.restart takes only the handle."};
    bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->span,
                              "a simulation call's arguments don't match its parameters",
                              reasons[operation], NULL);
    (void)kept;
    return (struct value){0};
}
}

static MO_MUST_USE const struct mo_symbol *resolve_actor_type_arg(struct inference *p,
                                                                  const struct mo_syntax *arg) {
    uint32_t count = 0;
    if (arg->kind == MO_S_CAPNAME)
        return infer_definition(p, arg, NULL, &count);
    if (arg->kind == MO_S_QUALIFIED)
        return infer_definition(p, arg->children[1], arg->children[0], &count);
    if (arg->kind == MO_S_POSTFIX && arg->count == 2 && arg->children[0]->kind == MO_S_CAPNAME &&
        arg->children[1]->kind == MO_S_QUALIFIED)
        return infer_definition(p, arg->children[1]->children[0], arg->children[0], &count);
    return NULL;
}

MO_MUST_USE struct value infer_actor_call(struct inference *p, const struct mo_syntax *name,
                                          const struct mo_syntax *args) {
    enum actor_op {
        ACTOR_OP_START,
        ACTOR_OP_START_AT,
        ACTOR_OP_SEND,
        ACTOR_OP_ADVANCE,
        ACTOR_OP_CRASH,
        ACTOR_OP_RESTART,
        ACTOR_OP_COUNT
    };
    static const struct {
        const char *member, *qualified;
    } ops[ACTOR_OP_COUNT] = {
        [ACTOR_OP_START] = {"start", "Actor.start"},
        [ACTOR_OP_START_AT] = {"start_at", "Actor.start_at"},
        [ACTOR_OP_SEND] = {"send", "Actor.send"},
        [ACTOR_OP_ADVANCE] = {"advance", "Actor.advance"},
        [ACTOR_OP_CRASH] = {"crash", "Actor.crash"},
        [ACTOR_OP_RESTART] = {"restart", "Actor.restart"},
    };
    uint32_t op = 0;
    while (op < ACTOR_OP_COUNT && infer_compare_text(name, ops[op].member) != 0)
        op++;
    if (op == ACTOR_OP_COUNT) {
        bool kept = infer_problem(p, MO_CODE_NAME_UNKNOWN, name->span, infer_unknown_member,
                                  "Actor's operations are start, start_at, send, advance, crash "
                                  "and restart (reference 11.7).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    if (args == NULL) {
        bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, name->span,
                                  "an Actor operation needs a direct call",
                                  "Actor operations are not values; call one directly, as in "
                                  "Actor.start(RateLimiter) (reference 11.7).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    if (op != ACTOR_OP_SEND && !p->spec) {
        bool kept = infer_problem(p, MO_CODE_FAKE_OUTSIDE_SPEC, name->span,
                                  "a fake capability is made only in a test, example or promise",
                                  "Actor test controls (start, start_at, advance, crash, restart) "
                                  "are allowed only in tests, examples and promises "
                                  "(reference 11.7, 12.4).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }

    const struct mo_program *program = p->signatures->service_program;
    if (op == ACTOR_OP_START || op == ACTOR_OP_START_AT) {
        uint32_t expected_argc = op == ACTOR_OP_START ? 1u : 2u;
        if (args->count != expected_argc) {
            bool kept = infer_problem(
                p, MO_CODE_CALL_ARGUMENTS, args->span,
                "an Actor call's arguments don't match its parameters",
                op == ACTOR_OP_START ? "Actor.start takes an actor type name."
                                     : "Actor.start_at takes an actor type name and a start Time.",
                NULL);
            (void)kept;
            return (struct value){0};
        }
        const struct mo_symbol *actor_sym = resolve_actor_type_arg(p, args->children[0]);
        if (p->failed)
            return (struct value){0};
        uint32_t service_index = UINT32_MAX;
        if (actor_sym != NULL && actor_sym->declaration->kind == MO_S_ACTOR && program != NULL) {
            for (uint32_t i = 0; i < program->service_count; i++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (program->services[i].is_actor &&
                    program->services[i].actor_symbol == actor_sym) {
                    service_index = i;
                    break;
                }
            }
        }
        if (service_index == UINT32_MAX) {
            bool kept = infer_problem(
                p, MO_CODE_ACTOR_BLOCK, args->children[0]->span,
                "Actor.start must name a visible actor type",
                "Write Actor.start(RateLimiter) or Actor.start(Mod.RateLimiter) naming an "
                "actor declared in scope (reference 11.7).",
                NULL);
            (void)kept;
            return (struct value){0};
        }
        const struct mo_nominal_signature *nom =
            mo_nominal_signature_by_symbol(p->signatures, actor_sym);
        MO_ASSERT(nom != NULL);
        struct term *actor_term = infer_annotation(p, nom->type, NULL, 0);
        if (actor_term == NULL)
            return (struct value){0};
        uint32_t emit_argc = op == ACTOR_OP_START ? 0u : 1u;
        struct value out = infer_graph(p, (struct value){.type = actor_term}, emit_argc);
        if (out.type == NULL)
            return out;
        if (p->checked) {
            out.graph->output.native = ops[op].qualified;
            out.graph->output.service = service_index;
        }
        if (op == ACTOR_OP_START_AT) {
            struct term *time_ty = infer_native(p, "Time", 0);
            if (time_ty == NULL)
                return (struct value){0};
            struct value tval = infer_as(p, args->children[1], time_ty, MO_CODE_ARGUMENT_TYPE);
            if (tval.type == NULL)
                return (struct value){0};
            if (p->checked)
                out.graph->operands[0] = tval.note;
            out.never = tval.never;
        }
        return out;
    }

    if ((op == ACTOR_OP_SEND && args->count < 2) || (op == ACTOR_OP_ADVANCE && args->count != 2) ||
        ((op == ACTOR_OP_CRASH || op == ACTOR_OP_RESTART) && args->count != 1)) {
        const char *reason =
            op == ACTOR_OP_SEND      ? "Actor.send takes the actor handle, ActorType.call, and the "
                                       "call's arguments."
            : op == ACTOR_OP_ADVANCE ? "Actor.advance takes the actor handle and a Duration."
                                     : "Actor.crash and Actor.restart take only the actor handle.";
        bool kept =
            infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->span,
                          "an Actor call's arguments don't match its parameters", reason, NULL);
        (void)kept;
        return (struct value){0};
    }

    struct value handle = infer_expression(p, args->children[0], NULL);
    if (handle.type == NULL)
        return (struct value){0};
    struct term *hroot = infer_root(handle.type);
    const struct mo_checked_service *service = NULL;
    uint32_t service_index = 0;
    if (!hroot->variable && hroot->shape.kind == MO_SOURCE_NAMED &&
        hroot->shape.declaration != NULL &&
        hroot->shape.declaration->declaration->kind == MO_S_ACTOR && program != NULL) {
        for (uint32_t i = 0; i < program->service_count; i++) {
            if (!infer_work(p))
                return (struct value){0};
            if (program->services[i].is_actor &&
                program->services[i].actor_symbol == hroot->shape.declaration) {
                service = &program->services[i];
                service_index = i;
                break;
            }
        }
    }
    if (service == NULL) {
        bool kept =
            infer_problem(p, MO_CODE_ARGUMENT_TYPE, args->children[0]->span,
                          "Actor operations require an actor handle as the first argument",
                          "Pass a value whose type is a declared actor (reference 11.7).", NULL);
        (void)kept;
        return (struct value){0};
    }

    if (op != ACTOR_OP_SEND) {
        struct term *unit_ty = infer_native(p, "Unit", 0);
        uint32_t emit_argc = op == ACTOR_OP_ADVANCE ? 2u : 1u;
        struct value out =
            infer_graph(p, (struct value){.type = unit_ty, .never = handle.never}, emit_argc);
        if (out.type == NULL)
            return out;
        if (p->checked) {
            out.graph->output.native = ops[op].qualified;
            out.graph->output.service = service_index;
            out.graph->operands[0] = handle.note;
        }
        if (op == ACTOR_OP_ADVANCE) {
            struct term *dur_ty = infer_native(p, "Duration", 0);
            if (dur_ty == NULL)
                return (struct value){0};
            struct value dval = infer_as(p, args->children[1], dur_ty, MO_CODE_ARGUMENT_TYPE);
            if (dval.type == NULL)
                return (struct value){0};
            if (p->checked)
                out.graph->operands[1] = dval.note;
            out.never = out.never || dval.never;
        }
        return out;
    }

    /* Actor.send(handle, Prefix.call_name, ...args) */
    const struct mo_syntax *sel = args->children[1];
    const struct mo_syntax *prefix = NULL;
    const struct mo_syntax *call_name = NULL;
    if (sel->count == 2 && (sel->kind == MO_S_FIELD_ACCESS || sel->kind == MO_S_QUALIFIED)) {
        prefix = sel->children[0];
        call_name = sel->children[1];
    } else if (sel->kind == MO_S_POSTFIX && sel->count == 2 &&
               (sel->children[1]->kind == MO_S_FIELD_ACCESS ||
                sel->children[1]->kind == MO_S_QUALIFIED)) {
        prefix = sel->children[0];
        call_name = sel->children[1]->children[0];
    }
    bool prefix_matches = false;
    if (prefix != NULL && prefix->kind == MO_S_CAPNAME) {
        uint32_t alias = UINT32_MAX;
        const struct mo_module_scope *own = &p->interfaces->scopes[p->function->function->module];
        const struct mo_module_scope *svc_scope = &p->interfaces->scopes[service->module];
        const struct mo_syntax *path =
            program->set->modules[p->function->function->module]->header.path;
        bool current_mod = own == svc_scope &&
                           ((own->name != NULL && infer_compare_text(prefix, own->name) == 0) ||
                            (path != NULL && path->count == 1 &&
                             infer_compare_name(prefix, path->children[0]) == 0));
        if (current_mod || (service->module == p->function->function->module &&
                            infer_compare_text(prefix, service->actor_symbol->name) == 0)) {
            prefix_matches = true;
        } else {
            const struct mo_module_scope *mod_target = infer_prefix_module(p, prefix, &alias);
            if (mod_target == svc_scope) {
                if (!infer_use_alias(p, alias, prefix))
                    return (struct value){0};
                prefix_matches = true;
            } else {
                uint32_t def_cnt = 0;
                const struct mo_symbol *sym = infer_definition(p, prefix, NULL, &def_cnt);
                if (p->failed)
                    return (struct value){0};
                if (sym == service->actor_symbol)
                    prefix_matches = true;
            }
        }
    } else if (prefix != NULL && (prefix->kind == MO_S_QUALIFIED ||
                                  (prefix->kind == MO_S_POSTFIX && prefix->count == 2 &&
                                   prefix->children[0]->kind == MO_S_CAPNAME &&
                                   prefix->children[1]->kind == MO_S_QUALIFIED))) {
        const struct mo_symbol *sym = resolve_actor_type_arg(p, prefix);
        if (p->failed)
            return (struct value){0};
        if (sym == service->actor_symbol)
            prefix_matches = true;
    }

    uint32_t tool_index = UINT32_MAX;
    const struct mo_source_type *signature = NULL;
    if (prefix_matches && call_name != NULL) {
        for (uint32_t ti = 0; ti < service->tool_count; ti++) {
            if (!infer_work(p))
                return (struct value){0};
            if (infer_compare_name(call_name, service->tools[ti].name) == 0) {
                tool_index = ti;
                signature = program->functions[service->tools[ti].function].signature->type;
                break;
            }
        }
    }
    if (signature == NULL) {
        bool kept = infer_problem(
            p, MO_CODE_ACTOR_BLOCK, sel->span, "Actor.send must name one of the actor's calls",
            "Write ActorType.call_name or Module.call_name matching the handle's actor type "
            "(reference 11.7).",
            NULL);
        (void)kept;
        return (struct value){0};
    }
    uint32_t expected_total = signature->count - 1;
    if (args->count != expected_total) {
        bool kept = infer_problem(
            p, MO_CODE_CALL_ARGUMENTS, args->span,
            "an Actor.send call's arguments don't match its parameters",
            "Actor.send takes the handle, ActorType.call, then the call's arguments after State "
            "and Time.",
            NULL);
        (void)kept;
        return (struct value){0};
    }

    const struct mo_source_type *raw_ret = signature->arguments[signature->count - 1];
    struct term *proj_reply = NULL;
    if (service->tools[tool_index].reply == MO_SERVICE_READ_ONLY) {
        proj_reply = infer_annotation(p, raw_ret, NULL, 0);
    } else if (service->tools[tool_index].reply == MO_SERVICE_UPDATE) {
        proj_reply = infer_annotation(p, raw_ret->arguments[1], NULL, 0);
    } else {
        proj_reply = infer_native(p, "Result", 2);
        if (proj_reply != NULL) {
            proj_reply->arguments[0] =
                infer_annotation(p, raw_ret->arguments[0]->arguments[1], NULL, 0);
            proj_reply->arguments[1] = infer_annotation(p, raw_ret->arguments[1], NULL, 0);
            if (proj_reply->arguments[0] == NULL || proj_reply->arguments[1] == NULL)
                return (struct value){0};
        }
    }
    struct term *res_term = infer_native(p, "Result", 2);
    if (proj_reply == NULL || res_term == NULL)
        return (struct value){0};
    res_term->arguments[0] = proj_reply;
    res_term->arguments[1] = infer_native(p, "SimulationError", 0);
    if (res_term->arguments[1] == NULL)
        return (struct value){0};

    uint32_t emit_argc = expected_total - 1;
    struct value out =
        infer_graph(p, (struct value){.type = res_term, .never = handle.never}, emit_argc);
    if (out.type == NULL)
        return out;
    if (p->checked) {
        out.graph->output.native = "Actor.send";
        out.graph->output.service = service_index;
        out.graph->output.tool = tool_index;
        out.graph->operands[0] = handle.note;
    }
    for (uint32_t i = 2; i < expected_total; i++) {
        struct term *wanted = infer_annotation(p, signature->arguments[i], NULL, 0);
        if (wanted == NULL)
            return (struct value){0};
        struct value aval = infer_as(p, args->children[i], wanted, MO_CODE_ARGUMENT_TYPE);
        if (aval.type == NULL)
            return (struct value){0};
        if (p->checked)
            out.graph->operands[i - 1] = aval.note;
        out.never = out.never || aval.never;
    }
    return out;
}

static MO_MUST_USE const struct mo_source_type *
find_lib_type(const struct mo_module_interfaces *interfaces, const struct mo_signatures *signatures,
              const char *mod_name, const char *name) {
    for (uint32_t i = 0; i < interfaces->count; i++) {
        const struct mo_module_scope *scope = &interfaces->scopes[i];
        if (!scope->library || scope->name == NULL || strcmp(scope->name, mod_name) != 0)
            continue;
        const struct mo_symbol *sym = mo_find_symbol(scope, (const uint8_t *)name, strlen(name));
        if (sym == NULL)
            return NULL;
        const struct mo_nominal_signature *nom = mo_nominal_signature_by_symbol(signatures, sym);
        return nom == NULL ? NULL : nom->type;
    }
    return NULL;
}

MO_MUST_USE struct value infer_apitest_call(struct inference *p, const struct mo_syntax *name,
                                            const struct mo_syntax *args) {
    bool is_send = infer_compare_text(name, "send") == 0;
    bool is_job = infer_compare_text(name, "run_job") == 0;
    if (!is_send && !is_job) {
        bool kept = infer_problem(p, MO_CODE_NAME_UNKNOWN, name->span, infer_unknown_member,
                                  "ApiTest's operations are send(Module, req, ...capabilities) and "
                                  "run_job(Module, name, payload, ...capabilities).",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    if (args == NULL) {
        bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, name->span,
                                  is_send ? "ApiTest.send needs a direct call"
                                          : "ApiTest.run_job needs a direct call",
                                  is_send ? "Call ApiTest.send(Module, req) directly."
                                          : "Call ApiTest.run_job(Module, name, payload) directly.",
                                  NULL);
        (void)kept;
        return (struct value){0};
    }
    if (!p->spec) {
        bool kept = infer_problem(
            p, MO_CODE_FAKE_OUTSIDE_SPEC, name->span,
            "a fake capability is made only in a test, example or promise",
            "ApiTest runs only in tests, examples and promises, never in a module's constants or "
            "its functions (reference 12.4).",
            NULL);
        (void)kept;
        return (struct value){0};
    }
    uint32_t min_args = is_job ? 3u : 2u;
    if (args->count < min_args) {
        bool kept = infer_problem(
            p, MO_CODE_CALL_ARGUMENTS, args->span,
            is_send ? "ApiTest.send needs a module and a Request"
                    : "ApiTest.run_job needs a module, job name and Json payload",
            is_send ? "Write ApiTest.send(Module, req) with optional fake capabilities."
                    : "Write ApiTest.run_job(Module, \"job.name\", payload) with optional fake "
                      "capabilities.",
            NULL);
        (void)kept;
        return (struct value){0};
    }
    const struct mo_syntax *prefix = args->children[0];
    const struct mo_module_scope *scope = NULL;
    uint32_t alias = UINT32_MAX;
    const struct mo_program *program = p->signatures->service_program;
    if (prefix->kind == MO_S_CAPNAME) {
        const struct mo_module_scope *own = &p->interfaces->scopes[p->function->function->module];
        const struct mo_syntax *path =
            program == NULL ? NULL
                            : program->set->modules[p->function->function->module]->header.path;
        bool current = own->name != NULL ? infer_compare_text(prefix, own->name) == 0
                                         : path != NULL && path->count == 1 &&
                                               infer_compare_name(prefix, path->children[0]) == 0;
        scope = current ? own : infer_prefix_module(p, prefix, &alias);
    }
    const struct mo_checked_api *api = NULL;
    uint32_t api_index = 0;
    if (scope != NULL && program != NULL) {
        for (uint32_t i = 0; i < program->api_count; i++) {
            if (!infer_work(p))
                return (struct value){0};
            if (&p->interfaces->scopes[program->apis[i].module] == scope) {
                api = &program->apis[i];
                api_index = i;
                break;
            }
        }
    }
    if (api == NULL) {
        bool kept = infer_problem(
            p, MO_CODE_API_BLOCK, prefix->span,
            is_send ? "ApiTest.send must name an api module"
                    : "ApiTest.run_job must name an api module",
            "Name the current module or one it uses directly, and that module must declare an "
            "api block.",
            NULL);
        (void)kept;
        return (struct value){0};
    }
    if (!infer_use_alias(p, alias, prefix))
        return (struct value){0};

    struct value out = {0};
    if (is_send) {
        const struct mo_source_type *req_src =
            find_lib_type(p->interfaces, p->signatures, "Api", "Request");
        const struct mo_source_type *res_src =
            find_lib_type(p->interfaces, p->signatures, "Api", "Response");
        if (req_src == NULL || res_src == NULL) {
            bool kept = infer_problem(p, MO_CODE_IMPORT_MISSING, name->span,
                                      "ApiTest.send needs the Api library module",
                                      "Bring Api into scope with use Api.", NULL);
            (void)kept;
            return (struct value){0};
        }
        struct term *req_term = infer_annotation(p, req_src, NULL, 0);
        struct term *res_term = infer_annotation(p, res_src, NULL, 0);
        if (req_term == NULL || res_term == NULL)
            return (struct value){0};

        out = infer_graph(p, (struct value){.type = res_term}, args->count - 1);
        if (out.type == NULL)
            return out;
        if (p->checked) {
            out.graph->output.native = "ApiTest.send";
            out.graph->output.service = api_index;
        }
        struct value req_val = infer_as(p, args->children[1], req_term, MO_CODE_ARGUMENT_TYPE);
        if (req_val.type == NULL)
            return (struct value){0};
        if (p->checked)
            out.graph->operands[0] = req_val.note;
        out.never = req_val.never;
    } else {
        const struct mo_source_type *json_src =
            find_lib_type(p->interfaces, p->signatures, "Json", "Json");
        if (json_src == NULL) {
            bool kept = infer_problem(p, MO_CODE_IMPORT_MISSING, name->span,
                                      "ApiTest.run_job needs the Json library module",
                                      "Bring Json into scope with use Json.", NULL);
            (void)kept;
            return (struct value){0};
        }
        struct term *text_term = infer_native(p, "Text", 0);
        struct term *unit_term = infer_native(p, "Unit", 0);
        struct term *json_term = infer_annotation(p, json_src, NULL, 0);
        struct term *res_term = infer_native(p, "Result", 2);
        if (text_term == NULL || unit_term == NULL || json_term == NULL || res_term == NULL)
            return (struct value){0};
        res_term->arguments[0] = unit_term;
        res_term->arguments[1] = text_term;

        out = infer_graph(p, (struct value){.type = res_term}, args->count - 1);
        if (out.type == NULL)
            return out;
        if (p->checked) {
            out.graph->output.native = "ApiTest.run_job";
            out.graph->output.service = api_index;
        }
        struct value name_val = infer_as(p, args->children[1], text_term, MO_CODE_ARGUMENT_TYPE);
        if (name_val.type == NULL)
            return (struct value){0};
        struct value payload_val = infer_as(p, args->children[2], json_term, MO_CODE_ARGUMENT_TYPE);
        if (payload_val.type == NULL)
            return (struct value){0};
        if (p->checked) {
            out.graph->operands[0] = name_val.note;
            out.graph->operands[1] = payload_val.note;
        }
        out.never = name_val.never || payload_val.never;
    }

    static const char *const allowed_caps[] = {"Sqlite",  "Network",     "Clock", "Random",
                                               "Files",   "ReadFiles",   "Env",   "Console",
                                               "Command", "SecureRandom"};
    uint32_t seen_caps = 0;
    uint32_t seen_actors = 0;
    for (uint32_t i = min_args; i < args->count; i++) {
        struct value cap_val = infer_expression(p, args->children[i], NULL);
        if (cap_val.type == NULL)
            return (struct value){0};
        struct term *cap_root = infer_root(cap_val.type);
        if (!cap_root->variable && cap_root->shape.kind == MO_SOURCE_NAMED &&
            cap_root->shape.declaration != NULL &&
            cap_root->shape.declaration->declaration->kind == MO_S_ACTOR) {
            uint32_t actor_slot = UINT32_MAX;
            for (uint32_t a = 0; a < api->actor_count; a++)
                if (api->actors[a].type->declaration == cap_root->shape.declaration) {
                    actor_slot = a;
                    break;
                }
            if (actor_slot == UINT32_MAX) {
                bool kept = infer_problem(
                    p, MO_CODE_ARGUMENT_TYPE, args->children[i]->span,
                    "ApiTest.send passes an actor not declared by this api",
                    "Declare actor <name>: <ActorType> in the api block to pass that actor handle.",
                    NULL);
                (void)kept;
                return (struct value){0};
            }
            if ((seen_actors & (UINT32_C(1) << actor_slot)) != 0) {
                bool kept =
                    infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                  "ApiTest.send passes the same actor type twice",
                                  "Pass at most one handle of each declared actor type.", NULL);
                (void)kept;
                return (struct value){0};
            }
            seen_actors |= UINT32_C(1) << actor_slot;
            if (p->checked)
                out.graph->operands[i - 1] = cap_val.note;
            out.never = out.never || cap_val.never;
            continue;
        }
        uint32_t slot = UINT32_MAX;
        if (!cap_root->variable && cap_root->shape.kind == MO_SOURCE_NAMED &&
            cap_root->shape.module == UINT32_MAX) {
            for (uint32_t c = 0; c < sizeof allowed_caps / sizeof allowed_caps[0]; c++)
                if (strcmp(cap_root->shape.name, allowed_caps[c]) == 0) {
                    slot = c;
                    break;
                }
        }
        if (slot == UINT32_MAX) {
            bool kept = infer_problem(
                p, MO_CODE_ARGUMENT_TYPE, args->children[i]->span,
                "ApiTest.send optional arguments must be capabilities or declared actors",
                "Pass fake capabilities such as Sqlite.in_memory() or actor handles from "
                "Actor.start(...).",
                NULL);
            (void)kept;
            return (struct value){0};
        }
        if ((seen_caps & (UINT32_C(1) << slot)) != 0) {
            bool kept = infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                                      "ApiTest.send passes the same capability type twice",
                                      "Pass at most one capability of each type.", NULL);
            (void)kept;
            return (struct value){0};
        }
        seen_caps |= UINT32_C(1) << slot;
        if (p->checked)
            out.graph->operands[i - 1] = cap_val.note;
        out.never = out.never || cap_val.never;
    }
    return out;
}

MO_MUST_USE struct value infer_sequence(struct inference *p, const struct mo_syntax *node,
                                        struct term *expected) {
    struct value last = {.type = infer_native(p, "Unit", 0)};
    struct value emitted = infer_graph(p, last, node->count);
    if (emitted.type == NULL)
        return (struct value){0};
    bool never = false;
    for (uint32_t i = 0; i < node->count; i++) {
        last = infer_as(p, node->children[i], i + 1 == node->count && !never ? expected : NULL,
                        p->node_expectation);
        if (last.type == NULL)
            return (struct value){0};
        if (p->checked)
            emitted.graph->operands[i] = last.note;
        never = never || last.never;
        /* The last note belongs to this statement, not its nested expressions. */
        if (i + 1 != node->count)
            p->last->discarded = !last.never;
    }
    last.never = never;
    last.graph = emitted.graph;
    return last;
}

MO_MUST_USE struct value infer_call(struct inference *p, struct value callee,
                                    const struct mo_syntax *args, const struct value *receiver) {
    if (callee.type == NULL)
        return (struct value){0};
    struct term *t = infer_root(callee.type);
    if (t->variable) {
        struct term *fn = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                                     .name = "fn",
                                                                     .module = UINT32_MAX,
                                                                     .count = args->count + 1});
        if (fn == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < fn->shape.count; i++) {
            fn->arguments[i] = infer_fresh(p, 0);
            if (fn->arguments[i] == NULL)
                return (struct value){0};
        }
        if (!infer_unify(p, t, fn, 0))
            return (struct value){0};
        t = infer_root(fn);
    }
    if (t->shape.kind != MO_SOURCE_FUNCTION) {
        if (callee.binding != NULL) {
            uint32_t count = 0;
            const struct mo_symbol *symbol = infer_definition(p, callee.binding, NULL, &count);
            if (symbol != NULL && symbol->declaration->kind == MO_S_FN) {
                bool reported =
                    infer_problem(p, MO_CODE_BIND_REPEATED, p->span,
                                  "a name means both a local and a module function",
                                  "A function can't refer to a module function by a name it "
                                  "uses for a local (reference 6.1).",
                                  callee.binding);
                (void)reported;
                return (struct value){0};
            }
        }
        struct term function = {.shape = {.kind = MO_SOURCE_FUNCTION, .name = "fn"}};
        bool valid = infer_unify_as(p, t, &function, MO_CODE_NOT_FUNCTION);
        (void)valid;
        return (struct value){0};
    }
    uint32_t offset = receiver == NULL ? 0u : 1u;
    uint32_t count = args == NULL ? 0u : args->count;
    if (t->shape.count != count + offset + 1) {
        bool reported =
            infer_problem(p, MO_CODE_CALL_ARGUMENTS,
                          args == NULL || args->span.start == args->span.end ? p->span : args->span,
                          "a call's arguments don't match its parameters",
                          "A function has no default or optional parameters; pass every "
                          "argument by position (reference 5.2).",
                          NULL);
        (void)reported;
        return (struct value){0};
    }
    bool never = callee.never;
    if (receiver != NULL) {
        if (!infer_unify_as(p, receiver->type, infer_argument(p, t, 0), MO_CODE_ARGUMENT_TYPE))
            return (struct value){0};
        never = never || receiver->never;
    }
    for (uint32_t i = 0; i < count; i++)
        if (args->children[i]->kind == MO_S_NAMED_ARG) {
            bool reported =
                infer_problem(p, MO_CODE_CALL_ARGUMENTS, args->children[i]->span,
                              "a function is called with named fields",
                              "Arguments are passed by position; only records and cases "
                              "are built with named fields (reference 5.2).",
                              NULL);
            (void)reported;
            return (struct value){0};
        }
    struct note_range *ranges =
        infer_allocate(p, count, sizeof(*ranges), _Alignof(struct note_range));
    if (ranges == NULL)
        return (struct value){0};
    struct value result = {0};
    if (p->checked) {
        bool declared_dot = receiver != NULL && callee.note != NULL;
        uint32_t extra = declared_dot ? 1u : 0u;
        result = infer_graph(p, (struct value){.type = infer_argument(p, t, t->shape.count - 1)},
                             count + 1 + extra);
        if (result.type == NULL)
            return result;
        result.graph->operands[0] = receiver == NULL || declared_dot ? callee.note : receiver->note;
        if (declared_dot)
            result.graph->operands[1] = receiver->note;
        result.graph->output.symbol = callee.symbol;
        if (callee.graph != NULL) {
            result.graph->output.native = callee.graph->output.native;
            result.graph->output.service = callee.graph->output.service;
            result.graph->output.tool = callee.graph->output.tool;
            result.graph->function_type = callee.graph->function_type;
            result.graph->requirements = callee.graph->requirements;
            result.graph->parameters = callee.graph->parameters;
            result.graph->json_decode = callee.graph->json_decode;
        }
        if (receiver != NULL)
            result.graph->output.receiver = extra;
    }
    result.json_decode = callee.json_decode;
    struct note *before = p->last;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < count; i++) {
            if (infer_lambda_argument(args->children[i]) != (pass != 0))
                continue;
            struct note *previous = p->last;
            struct value arg = infer_as(p, args->children[i], infer_argument(p, t, i + offset),
                                        MO_CODE_ARGUMENT_TYPE);
            if (arg.type == NULL)
                return (struct value){0};
            ranges[i] = (struct note_range){previous == NULL ? p->notes : previous->next, p->last};
            if (p->checked)
                result.graph->operands[i + (receiver != NULL && callee.note != NULL ? 2u : 1u)] =
                    arg.note;
            never = never || arg.never;
        }
    }
    /* Constraint solving may visit lambdas last; metadata retains written order
     * so this never suggests reordering evaluation to downstream consumers. */
    infer_order_notes(p, before, ranges, count);
    if (!p->checked)
        result.type = infer_argument(p, t, t->shape.count - 1);
    result.callee = callee.note;
    result.never = never;
    return result;
}

static const char *copy_diag_text(struct inference *p, const char *text) {
    if (text == NULL)
        return NULL;
    size_t n = strlen(text) + 1;
    char *copy = infer_allocate(p, n, 1, 1);
    if (copy != NULL)
        memcpy(copy, text, n);
    return copy;
}

MO_MUST_USE struct value infer_type_application(struct inference *p, struct value callee,
                                                const struct mo_syntax *args) {
    if (callee.type == NULL)
        return (struct value){0};
    if (callee.function_type == NULL || callee.parameters == NULL) {
        bool reported = infer_problem(
            p, MO_CODE_TYPE_ARGUMENTS, args->span, "this function doesn't take type arguments",
            "Bracketed type arguments instantiate a generic function's type "
            "parameters, as in Json.decode[User](text) (reference 5.5).",
            NULL);
        (void)reported;
        return (struct value){0};
    }
    uint32_t expected_count = 0;
    for (unsigned i = 0; i < 26; i++)
        expected_count += (callee.function_type->parameters & (UINT32_C(1) << i)) != 0 ? 1u : 0u;
    if (expected_count != args->count) {
        bool reported =
            infer_problem(p, MO_CODE_TYPE_ARGUMENTS, args->span,
                          "a function's type arguments don't match its type parameters",
                          "Pass one type argument for each type parameter of the generic function "
                          "(reference 5.5).",
                          NULL);
        (void)reported;
        return (struct value){0};
    }
    for (uint32_t i = 0; i < args->count; i++) {
        struct mo_diags local_diags = {0};
        if (!mo_diags_init(&local_diags)) {
            bool reported = infer_problem(p, MO_CODE_CHECK_NO_MEMORY, args->children[i]->span,
                                          "the checking arena has no room", NULL, NULL);
            (void)reported;
            p->fatal = true;
            return (struct value){0};
        }
        const struct mo_source_type *resolved = mo_resolve_expression_type(
            p->arena, p->interfaces, p->signatures, p->function, args->children[i], &local_diags);
        if (resolved == NULL || mo_diags_any(&local_diags)) {
            if (local_diags.count != 0) {
                const struct mo_diag *d = &local_diags.items[0];
                const char *msg = copy_diag_text(p, d->message);
                const char *rsn = copy_diag_text(p, d->reason);
                uint16_t code = d->code;
                struct mo_span span = d->primary.span;
                mo_diags_free(&local_diags);
                if (code >= 1000)
                    p->fatal = true;
                bool reported = infer_problem(p, code, span, msg, rsn, NULL);
                (void)reported;
            } else {
                mo_diags_free(&local_diags);
                bool reported = infer_problem(p, MO_CODE_CHECK_NO_MEMORY, args->children[i]->span,
                                              "the checking arena has no room", NULL, NULL);
                (void)reported;
                p->fatal = true;
            }
            return (struct value){0};
        }
        mo_diags_free(&local_diags);
        struct term *term = infer_annotation(p, resolved, NULL, 0);
        if (term == NULL)
            return (struct value){0};
        if (!mo_check_type_abilities(&p->abilities, resolved)) {
            bool reported = infer_problem(p, p->abilities.error, args->children[i]->span,
                                          p->abilities.message, p->abilities.reason, NULL);
            (void)reported;
            return (struct value){0};
        }
        if (!infer_unify(p, callee.parameters[i], term, 0))
            return (struct value){0};
    }
    return callee;
}

MO_MUST_USE struct value infer_binary(struct inference *p, enum mo_syntax_token op, struct value a,
                                      const struct mo_syntax *right, struct term *expected) {
    if (a.type == NULL)
        return (struct value){0};
    bool logical = op == MO_T_AND || op == MO_T_OR || op == MO_T_IMPLIES;
    bool comparison = op == MO_T_EQ || op == MO_T_NE || op == MO_T_LT || op == MO_T_LE ||
                      op == MO_T_GT || op == MO_T_GE;
    if (op == MO_T_OR && infer_root(a.type)->variable) {
        struct mo_span span = p->span;
        struct value fallback = infer_expression(p, right, expected);
        struct value out =
            infer_graph(p, (struct value){.type = infer_fresh(p, 0), .never = a.never}, 2);
        struct failure_check *pending =
            infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
        if (fallback.type == NULL || out.type == NULL || pending == NULL)
            return (struct value){0};
        *pending = (struct failure_check){.next = p->failures,
                                          .input = a.type,
                                          .value = out.type,
                                          .fallback = fallback.type,
                                          .graph = out.graph,
                                          .span = span,
                                          .never = fallback.never};
        p->failures = pending;
        if (p->checked) {
            out.graph->operands[0] = a.note;
            out.graph->operands[1] = fallback.note;
            out.graph->output.operation = MO_T_OR;
        }
        return out;
    }
    if (logical && op == MO_T_OR && infer_named(a.type, "Option")) {
        struct term *payload = infer_argument(p, a.type, 0);
        if (payload == NULL ||
            (expected != NULL && !infer_unify_as(p, payload, expected, p->node_expectation)))
            return (struct value){0};
        struct value fallback = infer_expression(p, right, payload);
        if (fallback.type == NULL)
            return fallback;
        struct value out = infer_graph(p, (struct value){.type = payload, .never = a.never}, 2);
        if (p->checked && out.type != NULL) {
            out.graph->operands[0] = a.note;
            out.graph->operands[1] = fallback.note;
            out.graph->output.operation = MO_T_OR;
            out.graph->output.or_kind = MO_CHECK_OR_OPTION;
        }
        return out;
    }
    struct term *operand = logical             ? infer_native(p, "Bool", 0)
                           : op == MO_T_DOTDOT ? infer_native(p, "Int", 0)
                                               : a.type;
    if (operand == NULL ||
        !infer_unify_as(p, a.type, operand, logical ? MO_CODE_CONDITION_TYPE : 0))
        return (struct value){0};
    struct term *right_type = operand;
    struct term *result = logical || comparison ? infer_native(p, "Bool", 0)
                          : op == MO_T_DOTDOT   ? infer_native(p, "Range", 0)
                                                : operand;
    struct value b = {0};
    if (!logical && !comparison && op != MO_T_DOTDOT) {
        if (!p->checked && (infer_named(operand, "Time") || infer_named(operand, "Duration") ||
                            infer_root(operand)->shape.kind == MO_SOURCE_PARAMETER)) {
            bool reported = infer_unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        if (infer_named(operand, "Time") && (op == MO_T_PLUS || op == MO_T_MINUS)) {
            struct term *duration = infer_native(p, "Duration", 0);
            if (duration == NULL)
                return (struct value){0};
            if (op == MO_T_PLUS) {
                if (expected != NULL && !infer_unify_as(p, operand, expected, p->node_expectation))
                    return (struct value){0};
                right_type = duration;
            } else {
                struct mo_span span = p->span;
                b = infer_expression(p, right, NULL);
                if (b.type == NULL)
                    return (struct value){0};
                if (!infer_root(b.type)->variable || infer_root(b.type)->numeric != 0) {
                    if (infer_named(b.type, "Time")) {
                        result = duration;
                    } else {
                        if (!infer_unify(p, b.type, duration, 0))
                            return (struct value){0};
                        result = operand;
                    }
                    if (expected != NULL &&
                        !infer_unify_as(p, result, expected, p->node_expectation))
                        return (struct value){0};
                } else {
                    result = infer_fresh(p, 0);
                    struct failure_check *pending =
                        infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
                    if (result == NULL || pending == NULL ||
                        (expected != NULL && !infer_unify(p, result, expected, 0)))
                        return (struct value){0};
                    infer_root(result)->deferred = true;
                    *pending = (struct failure_check){.next = p->failures,
                                                      .input = operand,
                                                      .fallback = b.type,
                                                      .value = result,
                                                      .op = MO_T_MINUS,
                                                      .code = p->node_expectation,
                                                      .span = span};
                    p->failures = pending;
                }
            }
        } else if (infer_named(operand, "Duration") &&
                   (op == MO_T_PLUS || op == MO_T_MINUS || op == MO_T_STAR)) {
            if (expected != NULL && !infer_unify_as(p, operand, expected, p->node_expectation))
                return (struct value){0};
            if (op == MO_T_STAR) {
                right_type = infer_native(p, "Int", 0);
                if (right_type == NULL)
                    return (struct value){0};
            }
        } else if (p->checked && (op == MO_T_PLUS || op == MO_T_MINUS || op == MO_T_STAR) &&
                   infer_root(operand)->variable && infer_root(operand)->numeric == 0 &&
                   infer_root(operand)->deferred) {
            struct mo_span span = p->span;
            b = infer_expression(p, right, NULL);
            result = infer_fresh(p, 0);
            struct failure_check *pending =
                infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
            if (b.type == NULL || result == NULL || pending == NULL ||
                (expected != NULL && !infer_unify(p, result, expected, 0)))
                return (struct value){0};
            infer_root(result)->deferred = true;
            *pending = (struct failure_check){.next = p->failures,
                                              .input = operand,
                                              .fallback = b.type,
                                              .value = result,
                                              .op = op,
                                              .code = p->node_expectation,
                                              .span = span};
            p->failures = pending;
        } else {
            struct term *numeric = infer_root(operand)->shape.kind == MO_SOURCE_PARAMETER
                                       ? operand
                                       : infer_fresh(p, INT | BIG | DECIMAL | FLOAT);
            if (numeric == NULL ||
                !infer_unify_as(p, operand, numeric,
                                infer_named(operand, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                             : MO_CODE_NUMBER_ABILITY))
                return (struct value){0};
            if (expected != NULL && !infer_unify_as(p, operand, expected, p->node_expectation))
                return (struct value){0};
        }
    }
    if (b.type == NULL)
        b = logical ? infer_condition(p, right, op == MO_T_AND)
                    : infer_expression(p, right, right_type);
    if (b.type == NULL)
        return (struct value){0};
    struct value out = infer_graph(
        p, (struct value){.type = result, .never = a.never || (!logical && b.never)}, 2);
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = a.note;
        out.graph->operands[1] = b.note;
        out.graph->output.operation = op;
        out.graph->output.or_kind = op == MO_T_OR ? MO_CHECK_OR_BOOL : MO_CHECK_OR_ABSENT;
    }
    return out;
}

MO_MUST_USE struct value infer_propagate(struct inference *p, struct value input,
                                         struct term *expected) {
    if (input.type == NULL)
        return input;
    enum mo_syntax_kind owner = p->function->function->declaration->kind;
    if ((owner == MO_S_CONST || owner == MO_S_SERVICE_STATE || owner == MO_S_SERVICE_EVERY) &&
        p->lambda == 0) {
        bool reported = infer_problem(p, MO_CODE_TRY_RESULT, p->span,
                                      "try has no enclosing function returning Option or Result",
                                      "try passes a failure upwards from the nearest function or "
                                      "anonymous function (reference 8.3).",
                                      NULL);
        (void)reported;
        return (struct value){0};
    }
    struct value out =
        infer_graph(p,
                    (struct value){.type = expected == NULL ? infer_fresh(p, 0) : expected,
                                   .never = input.never},
                    1);
    struct failure_check *pending =
        infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
    if (out.type == NULL || pending == NULL)
        return (struct value){0};
    *pending = (struct failure_check){.next = p->failures,
                                      .input = input.type,
                                      .value = out.type,
                                      .context = p->result,
                                      .input_note = input.note,
                                      .graph = out.graph,
                                      .span = p->span,
                                      .never = input.never};
    p->failures = pending;
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = input.note;
        out.graph->try_result = p->result;
        out.graph->failure = pending;
    }
    if (!infer_root(p->result)->variable &&
        (infer_named(input.type, "Option") || infer_named(input.type, "Result")) &&
        !infer_solve_failure(p, pending, false))
        return (struct value){0};
    return out;
}
