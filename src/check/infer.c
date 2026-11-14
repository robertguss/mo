#include "check/infer.h"

#include "base/codes.h"
#include "base/limits.h"

static MO_MUST_USE struct value infer_node(struct inference *p, const struct mo_syntax *node,
                                           struct term *expected) {
    switch (node->kind) {
    case MO_S_SEQUENCE:
        return infer_sequence(p, node, expected);
    case MO_S_EXPR_STMT:
    case MO_S_GROUP:
        return infer_as(p, node->children[0], expected, p->node_expectation);
    case MO_S_TEXT_INTERPOLATED: {
        uint32_t count = node->count / 2;
        struct value out =
            infer_graph(p, (struct value){.type = infer_native(p, "Text", 0)}, count);
        if (out.type == NULL)
            return out;
        for (uint32_t i = 0; i < count; i++) {
            struct value hole = infer_expression(p, node->children[2 * i + 1], NULL);
            if (hole.type == NULL)
                return (struct value){0};
            if (p->checked)
                out.graph->operands[i] = hole.note;
            out.never = out.never || hole.never;
        }
        return out;
    }
    case MO_S_NAME:
    case MO_S_CAPNAME:
    case MO_S_QUALIFIED:
        return infer_identifier(p, node, NULL);
    case MO_S_FIELD_ACCESS: {
        if (node->children[0]->kind == MO_S_CAPNAME &&
            (!infer_case_receiver(p, node->children[0]) ||
             infer_type_reading(node->children[0], node->children[1], NULL)))
            return infer_identifier(p, node->children[1], node->children[0]);
        bool called;
        return infer_field_value(p, infer_expression(p, node->children[0], NULL), node->children[1],
                                 node, NULL, &called);
    }
    case MO_S_INT:
        return (struct value){.type = infer_fresh(p, INT | BIG | BYTE | DECIMAL | FLOAT)};
    case MO_S_DECIMAL:
        return (struct value){.type = infer_fresh(p, DECIMAL | FLOAT)};
    case MO_S_CHAR:
        return (struct value){.type = infer_native(p, "Char", 0)};
    case MO_S_TEXT:
        return (struct value){.type = infer_native(p, "Text", 0)};
    case MO_S_TRUE:
    case MO_S_FALSE:
        return (struct value){.type = infer_native(p, "Bool", 0)};
    case MO_S_BIND:
    case MO_S_VAR:
    case MO_S_ADD_ASSIGN:
    case MO_S_SUB_ASSIGN: {
        const struct mo_syntax *name = node->children[0];
        if (name->kind == MO_S_FIELD_ACCESS || name->kind == MO_S_POSTFIX) {
            const struct mo_syntax *base = name;
            bool fields = true;
            while (base->kind == MO_S_FIELD_ACCESS || base->kind == MO_S_POSTFIX) {
                if (base->kind == MO_S_POSTFIX)
                    for (uint32_t i = 1; i < base->count; i++)
                        fields = fields && base->children[i]->kind == MO_S_FIELD_ACCESS;
                base = base->children[0];
            }
            struct name_slot *entry = base->kind == MO_S_NAME ? infer_slot(p, base, false) : NULL;
            struct local *variable = entry == NULL ? NULL : entry->active;
            if (!fields || variable == NULL || !variable->variable ||
                variable->function != p->lambda) {
                bool reported =
                    infer_problem(p, MO_CODE_NOT_VARIABLE, name->span,
                                  "a field is assigned without an assignable var",
                                  "A var or its fields can be assigned within the function "
                                  "that declares it (reference 6.2).",
                                  variable == NULL ? NULL : variable->name);
                (void)reported;
                return (struct value){0};
            }
            if (p->lambda != 0) {
                bool reported = infer_problem(
                    p, MO_CODE_LAMBDA_WRITE, name->span, "an anonymous function can't assign a var",
                    "An anonymous function can read names in scope, but can't "
                    "assign a var (reference 5.6).",
                    variable->name);
                (void)reported;
                return (struct value){0};
            }
            if (!infer_capture_write(p, variable, node, name))
                return (struct value){0};
            variable->version++;
            struct value target = infer_expression(p, name, NULL);
            if (target.type == NULL)
                return (struct value){0};
            p->last->output.binding = variable->name;
            struct value rhs =
                node->kind == MO_S_BIND
                    ? infer_expression(p, node->children[1], target.type)
                    : infer_binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS,
                                   target, node->children[1], target.type);
            if (rhs.type == NULL)
                return (struct value){0};
            if (p->checked && node->kind != MO_S_BIND) {
                if (!infer_record(p, node, rhs))
                    return (struct value){0};
                rhs.note = p->last;
            }
            struct value out = infer_graph(p,
                                           (struct value){.type = infer_native(p, "Unit", 0),
                                                          .binding = variable->name,
                                                          .bound = variable->binding,
                                                          .field = target.field,
                                                          .never = rhs.never},
                                           2);
            if (p->checked && out.type != NULL) {
                out.graph->output.assignment = true;
                out.graph->operands[0] = target.note;
                out.graph->operands[1] = rhs.note;
            }
            return out;
        }
        if (name->kind != MO_S_NAME && name->kind != MO_S_TUPLE && name->kind != MO_S_UNDERSCORE) {
            bool reported = infer_unfinished(p);
            (void)reported;
            return (struct value){0};
        }
        struct name_slot *s = name->kind == MO_S_NAME ? infer_slot(p, name, false) : NULL;
        struct local *l = s == NULL ? NULL : s->active;
        bool assignment = l != NULL && l->variable && node->kind != MO_S_VAR;
        if (l != NULL && !assignment) {
            /* Assigning a name first bound by `name = value` in this function needs only `var`
             * there, when the new value has the name's type (D290); a value of another type
             * needs a new name. Any other earlier meaning keeps the plain rule. */
            bool statement = l->statement && l->function == p->lambda && node->kind != MO_S_VAR;
            bool to_var = statement, differs = false;
            if (statement && node->kind == MO_S_BIND) {
                struct value rhs = infer_expression(p, node->children[1], NULL);
                if (p->fatal)
                    return (struct value){0};
                to_var = rhs.type != NULL && infer_same_type(p, rhs.type, l->type, 0);
                if (p->fatal)
                    return (struct value){0};
                differs = rhs.type != NULL && !to_var;
            }
            bool reported = infer_problem(
                p, MO_CODE_BIND_REPEATED, name->span, "a visible name is bound again",
                to_var    ? "While a name is visible it can't be bound again; to assign it again, "
                            "declare it with var where it is first bound (reference 6.2)."
                : differs ? "While a name is visible it can't be bound again; this value has "
                            "another type, so bind it to a new name instead (reference 6.1)."
                          : "While a local name or parameter is visible it can't be bound "
                            "again (reference 6.1).",
                l->name);
            (void)reported;
            if (to_var && p->error.code == MO_CODE_BIND_REPEATED &&
                p->error.primary.span.start == name->span.start) {
                struct mo_span at = l->name->span;
                p->edit = (struct mo_edit){{at.file, at.start, at.start}, "var "};
                p->fix = (struct mo_fix){"declare it with var", &p->edit, 1};
                p->error.fixes = &p->fix;
                p->error.fix_count = 1;
            }
            return (struct value){0};
        }
        if ((node->kind == MO_S_ADD_ASSIGN || node->kind == MO_S_SUB_ASSIGN) && !assignment) {
            bool reported = infer_problem(
                p, MO_CODE_NOT_VARIABLE, name->span, "a name that isn't var is assigned",
                "+= and -= assign a var or its fields (reference 6.2).", NULL);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && l->function != p->lambda) {
            bool reported =
                infer_problem(p, MO_CODE_NOT_VARIABLE, name->span,
                              "a var is assigned outside the function that declares it",
                              "A var can be assigned again within the function that declares "
                              "it (reference 6.2).",
                              l->name);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && p->lambda != 0) {
            bool reported = infer_problem(
                p, MO_CODE_LAMBDA_WRITE, name->span, "an anonymous function can't assign a var",
                "An anonymous function can read names in scope, but can't "
                "assign a var (reference 5.6).",
                l->name);
            (void)reported;
            return (struct value){0};
        }
        if (assignment && !infer_capture_write(p, l, node, name))
            return (struct value){0};
        if (assignment)
            l->version++;
        struct value target = {0};
        if (assignment) {
            target = (struct value){.type = l->type, .binding = l->name, .bound = l->binding};
            if (node->kind != MO_S_BIND)
                target = infer_graph(p, target, 0);
            if (target.type == NULL || !infer_record(p, name, target))
                return (struct value){0};
            target.note = p->last;
        }
        struct value rhs;
        if (assignment && node->kind != MO_S_BIND) {
            l->used = true;
            rhs = infer_binary(p, node->kind == MO_S_ADD_ASSIGN ? MO_T_PLUS : MO_T_MINUS, target,
                               node->children[1], l->type);
        } else
            rhs = infer_expression(p, node->children[1], assignment ? l->type : NULL);
        if (rhs.type == NULL)
            return (struct value){0};
        if (p->checked && assignment && node->kind != MO_S_BIND) {
            if (!infer_record(p, node, rhs))
                return (struct value){0};
            rhs.note = p->last;
        }
        if (!assignment && !(node->kind == MO_S_VAR ? infer_bind(p, name, rhs.type, true)
                                                    : infer_binder(p, name, rhs.type)))
            return (struct value){0};
        if (!assignment && node->kind == MO_S_BIND && name->kind == MO_S_NAME)
            p->locals->statement = true;
        struct value result =
            infer_graph(p,
                        (struct value){.type = infer_native(p, "Unit", 0),
                                       .binding = assignment ? l->name : NULL,
                                       .bound = assignment                 ? l->binding
                                                : name->kind == MO_S_TUPLE ? NULL
                                                                           : p->last->binding,
                                       .never = rhs.never},
                        1);
        if (p->checked && result.type != NULL) {
            result.graph->operands[0] = rhs.note;
            result.graph->output.assignment = assignment;
            if (name->kind == MO_S_TUPLE)
                result.graph->pattern_type = rhs.type;
        }
        return result;
    }
    case MO_S_BINARY:
    case MO_S_RANGE: {
        bool logical =
            node->token == MO_T_AND || node->token == MO_T_OR || node->token == MO_T_IMPLIES;
        struct local *mark = p->locals;
        struct value left = logical && node->token != MO_T_OR
                                ? infer_condition(p, node->children[0], true)
                                : infer_expression(p, node->children[0], NULL);
        struct value result = infer_binary(p, node->token, left, node->children[1], expected);
        if (result.type == NULL || (logical && node->token != MO_T_AND && !infer_leave(p, mark)))
            return (struct value){0};
        return result;
    }
    case MO_S_BINARY_CHAIN: {
        enum mo_syntax_token op = node->children[1]->token;
        struct value result = op == MO_T_AND ? infer_condition(p, node->children[0], true)
                                             : infer_expression(p, node->children[0], NULL);
        for (uint32_t i = 1; i < node->count && result.type != NULL; i += 2) {
            result = infer_binary(p, node->children[i]->token, result, node->children[i + 1],
                                  i + 2 == node->count ? expected : NULL);
            if (p->checked && result.type != NULL && i + 2 < node->count) {
                if (!infer_record(p, node->children[i], result))
                    return (struct value){0};
                result.note = p->last;
            }
        }
        return result;
    }
    case MO_S_CALL: {
        const struct mo_syntax *callee = node->children[0];
        const struct mo_builtin_case *builtin = mo_builtin_case_by_name(callee);
        if (builtin != NULL)
            return infer_builtin_construct(p, builtin, node->children[1], expected);
        if (callee->kind == MO_S_CAPNAME || callee->kind == MO_S_QUALIFIED) {
            uint32_t count = 0;
            const struct mo_symbol *symbol = infer_definition(p, callee, NULL, &count);
            if (p->failed)
                return (struct value){0};
            const struct mo_data_signature *data =
                symbol == NULL ? NULL : infer_data_type(p, symbol);
            if (data != NULL)
                return infer_construct(p, data, node->children[1], expected);
        }
        return infer_call(p, infer_expression(p, node->children[0], NULL), node->children[1], NULL);
    }
    case MO_S_POSITION:
        return infer_position(p, infer_expression(p, node->children[0], NULL), node->children[1]);
    case MO_S_POSTFIX: {
        bool qualified =
            node->children[0]->kind == MO_S_CAPNAME &&
            (node->children[1]->kind == MO_S_QUALIFIED ||
             (node->children[1]->kind == MO_S_FIELD_ACCESS &&
              (!infer_case_receiver(p, node->children[0]) ||
               infer_type_reading(node->children[0], node->children[1]->children[0],
                                  node->count > 2 && node->children[2]->kind == MO_S_CALL
                                      ? node->children[2]
                                      : NULL))));
        uint32_t start = qualified ? 2u : 1u;
        const struct mo_syntax *name =
            qualified ? node->children[1]->children[0] : node->children[0];
        bool simulation = qualified && infer_compare_text(node->children[0], "Simulation") == 0;
        const struct mo_data_signature *data = NULL;
        const struct mo_builtin_case *builtin =
            !qualified && start < node->count && node->children[start]->kind == MO_S_CALL
                ? mo_builtin_case_by_name(name)
                : NULL;
        bool ability = qualified && (infer_compare_text(name, "to_text") == 0 ||
                                     infer_compare_text(name, "compare") == 0);
        if (!simulation && (name->kind == MO_S_CAPNAME || qualified) && !ability &&
            start < node->count && node->children[start]->kind == MO_S_CALL) {
            uint32_t count = 0;
            const struct mo_symbol *symbol =
                infer_definition(p, name, qualified ? node->children[0] : NULL, &count);
            if (p->failed)
                return (struct value){0};
            data = symbol == NULL ? NULL : infer_data_type(p, symbol);
        }
        struct value v =
            simulation ? infer_simulation_call(p, name,
                                               start < node->count &&
                                                       node->children[start]->kind == MO_S_CALL
                                                   ? node->children[start]->children[0]
                                                   : NULL)
            : builtin != NULL
                ? infer_builtin_construct(p, builtin, node->children[start]->children[0],
                                          start + 1 == node->count ? expected : NULL)
            : data != NULL ? infer_construct(p, data, node->children[start]->children[0],
                                             start + 1 == node->count ? expected : NULL)
            : qualified    ? infer_identifier(p, node->children[1]->children[0], node->children[0])
                           : infer_expression(p, node->children[0], NULL);
        if (simulation || data != NULL || builtin != NULL)
            start++;
        if (p->checked && v.type != NULL && v.note == NULL && start < node->count) {
            if (v.graph == NULL)
                v = infer_graph(p, v, 0);
            if (v.type == NULL || !infer_record(p, node->children[start - 1], v))
                return (struct value){0};
            v.note = p->last;
        }
        for (uint32_t i = start; i < node->count && v.type != NULL; i++) {
            const struct mo_syntax *step = node->children[i];
            if (step->kind == MO_S_POSITION)
                v = infer_position(p, v, step->children[0]);
            else if (step->kind == MO_S_CALL)
                v = infer_call(p, v, step->children[0], NULL);
            else if (step->kind == MO_S_FIELD_ACCESS) {
                const struct mo_syntax *site = NULL;
                if (i + 1 < node->count && node->children[i + 1]->kind == MO_S_CALL)
                    site = node->children[i + 1];
                bool called;
                v = infer_field_value(p, v, step->children[0], step, site, &called);
                if (called && site != NULL)
                    i++;
            } else {
                bool reported = infer_unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            if (p->checked && v.type != NULL && i + 1 < node->count) {
                if (!infer_record(p, step, v))
                    return (struct value){0};
                v.note = p->last;
            }
        }
        return v;
    }
    case MO_S_LAMBDA: {
        const struct mo_syntax *parameters = node->children[0];
        struct term *fn =
            infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_FUNCTION,
                                                       .name = "fn",
                                                       .module = UINT32_MAX,
                                                       .count = parameters->count + 1});
        if (fn == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < fn->shape.count; i++) {
            fn->arguments[i] = infer_fresh(p, 0);
            if (fn->arguments[i] == NULL)
                return (struct value){0};
        }
        if (expected != NULL && !infer_unify_as(p, fn, expected, p->node_expectation))
            return (struct value){0};
        struct value emitted = infer_graph(p, (struct value){.type = fn}, 1);
        if (emitted.type == NULL)
            return emitted;
        if (p->checked) {
            struct emitted_lambda *lambda =
                infer_allocate(p, 1, sizeof(*lambda), _Alignof(struct emitted_lambda));
            struct note **notes =
                infer_allocate(p, parameters->count, sizeof(*notes), _Alignof(struct note *));
            if (p->failed)
                return (struct value){0};
            *lambda = (struct emitted_lambda){.output = {.parameter_count = parameters->count},
                                              .parameters = notes,
                                              .parent = p->lambda_frame,
                                              .next = p->lambdas,
                                              .syntax = node,
                                              .loop = p->capture_loop,
                                              .path = p->capture_path,
                                              .depth = p->lambda + 1};
            p->lambdas = lambda;
            emitted.graph->lambda = lambda;
            p->lambda_count++;
        }
        struct local *mark = p->locals;
        struct term *old_result = p->result;
        struct emitted_lambda *old_frame = p->lambda_frame;
        if (p->checked)
            p->lambda_frame = emitted.graph->lambda;
        struct capture_loop *old_loop = p->capture_loop;
        p->capture_loop = NULL;
        uint32_t old_loops = p->loops;
        p->loops = 0;
        p->lambda++;
        p->result = fn->arguments[parameters->count];
        for (uint32_t i = 0; i < parameters->count; i++) {
            if (!infer_bind(p, parameters->children[i], fn->arguments[i], false))
                return (struct value){0};
            if (p->checked)
                emitted.graph->lambda->parameters[i] = p->last->binding;
        }
        struct value body = infer_named(p->result, "Unit")
                                ? infer_discard(p, node->children[1])
                                : infer_as(p, node->children[1], p->result, MO_CODE_RESULT_TYPE);
        if (body.type == NULL || !infer_leave(p, mark))
            return (struct value){0};
        p->result = old_result;
        p->lambda_frame = old_frame;
        p->capture_loop = old_loop;
        p->loops = old_loops;
        p->lambda--;
        if (p->checked)
            emitted.graph->operands[0] = body.note;
        return emitted;
    }
    case MO_S_IF:
    case MO_S_LINE_IF:
        return infer_conditional(p, node, expected);
    case MO_S_CASE:
        return infer_match(p, node, expected);
    case MO_S_IS: {
        struct value input = infer_expression(p, node->children[0], NULL);
        struct mo_match_pattern *normalized = NULL;
        if (input.type == NULL ||
            !infer_pattern(p, node->children[1], input.type, 0, 1, &normalized))
            return (struct value){0};
        struct value out = infer_graph(
            p, (struct value){.type = infer_native(p, "Bool", 0), .never = input.never}, 1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = input.note;
        return out;
    }
    case MO_S_RETURN: {
        struct local *mark = p->locals;
        struct value guard = {0};
        if (node->children[1] != NULL) {
            guard = infer_condition(p, node->children[1], true);
            if (guard.type == NULL)
                return guard;
        }
        struct value value = node->children[0] == NULL
                                 ? (struct value){.type = infer_native(p, "Unit", 0)}
                                 : infer_as(p, node->children[0], p->result, MO_CODE_RESULT_TYPE);
        if (value.type == NULL || !infer_unify_as(p, value.type, p->result, MO_CODE_RESULT_TYPE) ||
            !infer_leave(p, mark))
            return (struct value){0};
        uint32_t count =
            (node->children[0] == NULL ? 0u : 1u) + (node->children[1] == NULL ? 0u : 1u);
        struct value out = infer_graph(
            p,
            (struct value){.type = infer_native(p, "Unit", 0), .never = node->children[1] == NULL},
            count);
        if (p->checked && out.type != NULL) {
            if (guard.note != NULL)
                out.graph->operands[0] = guard.note;
            if (value.note != NULL)
                out.graph->operands[count - 1] = value.note;
        }
        return out;
    }
    case MO_S_STOP: {
        struct term *text = infer_native(p, "Text", 0);
        struct value reason =
            text == NULL ? (struct value){0} : infer_expression(p, node->children[0], text);
        if (reason.type == NULL)
            return (struct value){0};
        struct value out = infer_graph(
            p,
            (struct value){.type = expected == NULL ? infer_native(p, "Unit", 0) : expected,
                           .never = true},
            1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = reason.note;
        return out;
    }
    case MO_S_ASSERT: {
        struct value test = infer_condition(p, node->children[0], true);
        if (test.type == NULL)
            return (struct value){0};
        struct value out = infer_graph(p, (struct value){.type = infer_native(p, "Unit", 0)}, 1);
        if (p->checked && out.type != NULL)
            out.graph->operands[0] = test.note;
        return out;
    }
    case MO_S_FOR:
    case MO_S_WHILE: {
        struct local *mark = p->locals;
        struct note *binding = NULL;
        struct term *element = NULL;
        uint32_t body_index = node->kind == MO_S_FOR ? 2 : 1;
        struct capture_loop *old_loop = p->capture_loop;
        struct capture_loop *loop = NULL;
        if (p->checked) {
            loop = infer_allocate(p, 1, sizeof(*loop), _Alignof(struct capture_loop));
            if (loop == NULL)
                return (struct value){0};
            *loop = (struct capture_loop){node, old_loop};
            if (node->kind == MO_S_WHILE)
                p->capture_loop = loop;
        }
        struct value test = node->kind == MO_S_WHILE ? infer_condition(p, node->children[0], true)
                                                     : infer_expression(p, node->children[1], NULL);
        if (test.type == NULL)
            return (struct value){0};
        if (node->kind == MO_S_WHILE) {
            struct term *boolean = infer_native(p, "Bool", 0);
            if (boolean == NULL || !infer_unify_as(p, test.type, boolean, MO_CODE_CONDITION_TYPE))
                return (struct value){0};
        } else {
            struct term *container = infer_root(test.type);
            if (infer_named(container, "Range"))
                element = infer_native(p, "Int", 0);
            else if (infer_named(container, "List") || infer_named(container, "Set"))
                element = infer_argument(p, container, 0);
            else if (infer_named(container, "Map")) {
                element = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                                     .name = "()",
                                                                     .module = UINT32_MAX,
                                                                     .count = 2});
                if (element != NULL) {
                    element->arguments[0] = infer_argument(p, container, 0);
                    element->arguments[1] = infer_argument(p, container, 1);
                    if (p->failed)
                        return (struct value){0};
                }
            } else {
                bool reported =
                    container->variable && container->numeric == 0
                        ? infer_unfinished(p)
                        : infer_problem(
                              p, MO_CODE_NOT_ITERABLE, node->children[body_index - 1]->span,
                              "for is given a value that can't be iterated",
                              "For walks a list, map, set or range (reference 7.5).", NULL);
                (void)reported;
            }
            if (element == NULL || !infer_binder(p, node->children[0], element))
                return (struct value){0};
            const struct mo_syntax *name = node->children[0];
            while (name->kind == MO_S_BINDER)
                name = name->children[0];
            if (name->kind != MO_S_TUPLE)
                binding = p->last->binding;
        }
        p->capture_loop = loop;
        p->loops++;
        struct term *unit = infer_native(p, "Unit", 0);
        struct value body =
            unit == NULL ? (struct value){0} : infer_discard(p, node->children[body_index]);
        p->loops--;
        p->capture_loop = old_loop;
        if (body.type == NULL || !infer_leave(p, mark))
            return (struct value){0};
        struct value out = infer_graph(p, (struct value){.type = unit, .bound = binding}, 2);
        if (p->checked && out.type != NULL) {
            out.graph->operands[0] = test.note;
            out.graph->operands[1] = body.note;
            out.graph->pattern_type = element;
        }
        return out;
    }
    case MO_S_BREAK:
    case MO_S_CONTINUE: {
        if (p->loops == 0) {
            bool reported = infer_problem(
                p, MO_CODE_EXIT_OUTSIDE_LOOP, node->span, "break or continue is outside a loop",
                "Break leaves the loop and continue starts its next round "
                "(reference 7.5).",
                NULL);
            (void)reported;
            return (struct value){0};
        }
        struct value guard = {0};
        if (node->children[0] != NULL) {
            struct term *condition = infer_native(p, "Bool", 0);
            guard = condition == NULL
                        ? (struct value){0}
                        : infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
            if (guard.type == NULL)
                return (struct value){0};
        }
        struct value out = infer_graph(
            p,
            (struct value){.type = infer_native(p, "Unit", 0), .never = node->children[0] == NULL},
            guard.type == NULL ? 0u : 1u);
        if (p->checked && guard.type != NULL && out.type != NULL)
            out.graph->operands[0] = guard.note;
        return out;
    }
    case MO_S_LIST:
    case MO_S_TUPLE:
    case MO_S_MAP: {
        uint32_t arity = node->kind == MO_S_TUPLE ? node->count : node->kind == MO_S_MAP ? 2u : 1u;
        struct term *t = infer_make_type(
            p, (struct mo_source_type){.kind = node->kind == MO_S_TUPLE ? MO_SOURCE_TUPLE
                                                                        : MO_SOURCE_NAMED,
                                       .name = node->kind == MO_S_TUPLE ? "()"
                                               : node->kind == MO_S_MAP ? "Map"
                                                                        : "List",
                                       .module = UINT32_MAX,
                                       .count = arity});
        if (t == NULL)
            return (struct value){0};
        for (uint32_t i = 0; i < arity; i++) {
            t->arguments[i] = infer_fresh(p, 0);
            if (t->arguments[i] == NULL)
                return (struct value){0};
        }
        if (expected != NULL && !infer_unify_as(p, t, expected, p->node_expectation))
            return (struct value){0};
        struct value out = infer_graph(p, (struct value){.type = t},
                                       node->kind == MO_S_MAP ? 2 * node->count : node->count);
        if (out.type == NULL)
            return out;
        uint32_t *versions = NULL;
        if (node->kind == MO_S_MAP && node->count > 1) {
            versions = infer_allocate(p, node->count, sizeof(*versions), _Alignof(uint32_t));
            if (versions == NULL)
                return (struct value){0};
        }
        for (uint32_t i = 0; i < node->count; i++) {
            const struct mo_syntax *part = node->children[i];
            if (node->kind == MO_S_MAP) {
                const struct mo_syntax *key = part->children[0];
                while (key->kind == MO_S_GROUP)
                    key = key->children[0];
                struct name_slot *name = key->kind == MO_S_NAME ? infer_slot(p, key, false) : NULL;
                if (versions != NULL)
                    versions[i] = name != NULL && name->active != NULL && name->active->variable
                                      ? name->active->version
                                      : 0;
                for (uint32_t j = 0; j < 2; j++) {
                    struct value v = infer_expression(p, part->children[j], t->arguments[j]);
                    if (v.type == NULL)
                        return (struct value){0};
                    if (p->checked)
                        out.graph->operands[2 * i + j] = v.note;
                    out.never = out.never || v.never;
                }
            } else {
                struct value v =
                    infer_expression(p, part, t->arguments[node->kind == MO_S_TUPLE ? i : 0]);
                if (v.type == NULL)
                    return (struct value){0};
                if (p->checked)
                    out.graph->operands[i] = v.note;
                out.never = out.never || v.never;
            }
        }
        if (node->kind == MO_S_MAP && !infer_map_keys(p, node, versions))
            return (struct value){0};
        return out;
    }
    case MO_S_TRY: {
        struct value input = infer_expression(p, node->children[0], NULL);
        p->span = (struct mo_span){node->span.file, node->span.start, node->span.start + 3};
        return infer_propagate(p, input, expected);
    }
    case MO_S_NEGATE:
    case MO_S_NOT:
    case MO_S_PREFIX: {
        const struct mo_syntax *operand = node->children[node->count - 1];
        struct value v = infer_expression(p, operand, NULL);
        if (v.type == NULL)
            return v;
        bool negative =
            node->kind == MO_S_NEGATE ||
            (node->kind == MO_S_PREFIX && node->children[node->count - 2]->token == MO_T_MINUS);
        if (negative && (operand->kind == MO_S_INT || operand->kind == MO_S_DECIMAL)) {
            p->last->negative = true;
        }
        uint32_t count = node->kind == MO_S_PREFIX ? node->count - 1 : 1;
        for (uint32_t i = count; i != 0; i--) {
            enum mo_syntax_token op = node->kind == MO_S_PREFIX ? node->children[i - 1]->token
                                      : node->kind == MO_S_NOT  ? MO_T_NOT
                                                                : MO_T_MINUS;
            if (op == MO_T_TRY) {
                p->span = node->children[i - 1]->span;
                v = infer_propagate(p, v, i == 1 ? expected : NULL);
                if (v.type == NULL)
                    return v;
                if (p->checked && i != 1) {
                    if (!infer_record(p, node->children[i - 1], v))
                        return (struct value){0};
                    v.note = p->last;
                }
                continue;
            }
            if (op == MO_T_MINUS && !p->checked && infer_named(v.type, "Duration")) {
                bool reported = infer_unfinished(p);
                (void)reported;
                return (struct value){0};
            }
            if (op == MO_T_MINUS && p->checked && infer_root(v.type)->variable &&
                infer_root(v.type)->numeric == 0 && infer_root(v.type)->deferred) {
                struct term *result = infer_fresh(p, 0);
                struct failure_check *pending =
                    infer_allocate(p, 1, sizeof(*pending), _Alignof(struct failure_check));
                if (result == NULL || pending == NULL)
                    return (struct value){0};
                result->deferred = true;
                *pending = (struct failure_check){.next = p->failures,
                                                  .input = v.type,
                                                  .value = result,
                                                  .op = MO_T_MINUS,
                                                  .code = i == 1 ? p->node_expectation : 0,
                                                  .span = node->span};
                p->failures = pending;
                v.type = result;
            } else {
                struct term *wanted =
                    op == MO_T_NOT ? infer_native(p, "Bool", 0)
                    : op == MO_T_MINUS
                        ? ((p->checked && infer_named(v.type, "Duration")) ||
                                   infer_root(v.type)->shape.kind == MO_SOURCE_PARAMETER
                               ? v.type
                               : infer_fresh(p, INT | BIG | DECIMAL | FLOAT))
                        : NULL;
                if (wanted == NULL) {
                    bool reported = infer_unfinished(p);
                    (void)reported;
                    return (struct value){0};
                }
                uint16_t code = op == MO_T_NOT                ? MO_CODE_CONDITION_TYPE
                                : infer_named(v.type, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                              : MO_CODE_NUMBER_ABILITY;
                if (!infer_unify_as(p, v.type, wanted, code))
                    return (struct value){0};
            }
            if (p->checked) {
                struct note *input = v.note;
                v = infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
                if (v.type == NULL)
                    return v;
                v.graph->operands[0] = input;
                v.graph->output.operation = op;
                if (i != 1) {
                    if (!infer_record(p, node->children[i - 1], v))
                        return (struct value){0};
                    v.note = p->last;
                }
            }
        }
        return v;
    }
    case MO_S_FILE:
    case MO_S_MODULE:
    case MO_S_EXPOSE:
    case MO_S_EXPOSE_ALL:
    case MO_S_USE:
    case MO_S_FN:
    case MO_S_PARAM:
    case MO_S_WHERE:
    case MO_S_STRUCT:
    case MO_S_FIELD:
    case MO_S_ENUM:
    case MO_S_CASE_DECL:
    case MO_S_TEST:
    case MO_S_CONST:
    case MO_S_SERVICE:
    case MO_S_SERVICE_STATE:
    case MO_S_SERVICE_TOOLS:
    case MO_S_SERVICE_EVERY:
    case MO_S_SERVICE_TOKEN:
    case MO_S_PROMISE:
    case MO_S_ANCHOR:
    case MO_S_EXAMPLES:
    case MO_S_TYPE:
    case MO_S_BINDER:
    case MO_S_ELSE:
    case MO_S_ARM:
    case MO_S_NAMED_ARG:
    case MO_S_UNDERSCORE:
    case MO_S_REST:
    case MO_S_MAP_ENTRY:
    case MO_S_TOKEN:
    case MO_S_IF_PART:
        break;
    }
    bool reported = infer_unfinished(p);
    (void)reported;
    return (struct value){0};
}

MO_MUST_USE struct value infer_expression(struct inference *p, const struct mo_syntax *node,
                                          struct term *expected) {
    if (!infer_work(p))
        return (struct value){0};
    MO_ASSERT(node != NULL && !node->incomplete);
    struct local *mark = p->locals;
    bool keep = p->keep_patterns;
    bool propagates = node->kind == MO_S_IS || node->kind == MO_S_GROUP ||
                      (node->kind == MO_S_BINARY && node->token == MO_T_AND) ||
                      (node->kind == MO_S_BINARY_CHAIN && node->children[1]->token == MO_T_AND);
    p->keep_patterns = node->kind == MO_S_GROUP && keep;
    p->span = node->span;
    if (p->depth == MO_LIMIT_SOURCE_INFERENCE_DEPTH) {
        bool reported = infer_problem(p, MO_CODE_CHECK_SYNTAX_DEPTH, node->span,
                                      "the body needs too many inference frames", NULL, NULL);
        (void)reported;
        return (struct value){0};
    }
    uint16_t code = p->expectation, saved = p->node_expectation;
    p->node_expectation = code;
    p->expectation = 0;
    p->depth++;
    if (p->checked && node->kind != MO_S_SEQUENCE && node->kind != MO_S_EXPR_STMT &&
        node->kind != MO_S_GROUP && node->kind != MO_S_INT && node->kind != MO_S_DECIMAL &&
        node->kind != MO_S_CHAR && node->kind != MO_S_TEXT && node->kind != MO_S_TRUE &&
        node->kind != MO_S_FALSE && node->kind != MO_S_NAME && node->kind != MO_S_VAR &&
        node->kind != MO_S_BIND && node->kind != MO_S_BINARY && node->kind != MO_S_BINARY_CHAIN &&
        node->kind != MO_S_RANGE && node->kind != MO_S_NEGATE && node->kind != MO_S_NOT &&
        node->kind != MO_S_PREFIX && node->kind != MO_S_CALL && node->kind != MO_S_POSTFIX &&
        node->kind != MO_S_LAMBDA && node->kind != MO_S_IF && node->kind != MO_S_LINE_IF &&
        node->kind != MO_S_CAPNAME && node->kind != MO_S_QUALIFIED &&
        node->kind != MO_S_FIELD_ACCESS && node->kind != MO_S_RETURN && node->kind != MO_S_STOP &&
        node->kind != MO_S_ASSERT && node->kind != MO_S_CASE && node->kind != MO_S_IS &&
        node->kind != MO_S_FOR && node->kind != MO_S_WHILE && node->kind != MO_S_BREAK &&
        node->kind != MO_S_CONTINUE && node->kind != MO_S_LIST && node->kind != MO_S_MAP &&
        node->kind != MO_S_TUPLE && node->kind != MO_S_POSITION && node->kind != MO_S_ADD_ASSIGN &&
        node->kind != MO_S_SUB_ASSIGN && node->kind != MO_S_TEXT_INTERPOLATED &&
        node->kind != MO_S_TRY) {
        bool reported = infer_unfinished(p);
        (void)reported;
        p->depth--;
        return (struct value){0};
    }
    struct value v = infer_node(p, node, expected);
    p->depth--;
    p->expectation = code;
    p->node_expectation = saved;
    p->keep_patterns = keep;
    p->span = node->span;
    if (v.type == NULL || (propagates && !keep && !infer_leave(p, mark)) ||
        (expected != NULL && !v.never && !infer_unify(p, v.type, expected, 0)))
        return (struct value){0};
    if (p->checked && (node->kind == MO_S_EXPR_STMT || node->kind == MO_S_GROUP)) {
        struct note *operand = v.note;
        v = infer_graph(p, (struct value){.type = v.type, .never = v.never}, 1);
        if (v.type == NULL)
            return v;
        v.graph->operands[0] = operand;
    } else if (p->checked && v.graph == NULL)
        v = infer_graph(p, v, 0);
    if (v.type == NULL)
        return v;
    if (!infer_record(p, node, v))
        return (struct value){0};
    v.note = p->last;
    return v;
}
