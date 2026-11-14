#include "check/infer.h"

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
    if (data->fields[0] == NULL)
        return infer_argument(p, type, data->parameter);
    return infer_native(p, strcmp(data->name, "Entry") == 0 && index == 1 ? "EntryKind" : "Text",
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
    struct term *type = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                                   .name = "fn",
                                                                   .module = UINT32_MAX,
                                                                   .count = text ? 2u : 3u});
    if (type == NULL)
        return (struct value){0};
    type->arguments[0] = receiver.type;
    if (!text)
        type->arguments[1] = receiver.type;
    type->arguments[type->shape.count - 1] = infer_native(p, text ? "Text" : "Ordering", 0);
    if (p->failed)
        return (struct value){0};
    struct value out = infer_graph(p, (struct value){.type = type}, 0);
    if (p->checked && out.type != NULL)
        out.graph->output.native = text ? "to_text" : "compare";
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
        const struct mo_native_signature *signature =
            infer_native_definition(p, type->shape.name, name);
        if (signature != NULL)
            return infer_native_identifier(p, signature);
    }
    const struct mo_data_signature *data = type->variable || type->shape.declaration == NULL
                                               ? NULL
                                               : infer_data_type(p, type->shape.declaration);
    const struct mo_field_signature *field = data == NULL ? NULL : infer_find_field(data, name);
    bool visible = data != NULL && (type->shape.module == p->function->function->module ||
                                    type->shape.declaration->insides);
    if ((field == NULL || !visible) &&
        (infer_compare_text(name, "to_text") == 0 || infer_compare_text(name, "compare") == 0))
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
        if (scope != NULL && program != NULL) {
            for (uint32_t i = 0; i < program->service_count; i++) {
                if (!infer_work(p))
                    return (struct value){0};
                if (&p->interfaces->scopes[program->services[i].module] == scope) {
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
                start ? "Name the current module or one it uses directly, as in "
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
            result.graph->function_type = callee.graph->function_type;
            result.graph->requirements = callee.graph->requirements;
            result.graph->parameters = callee.graph->parameters;
        }
        if (receiver != NULL)
            result.graph->output.receiver = extra;
    }
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
