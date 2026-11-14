#include "check/infer.h"

#include "base/codes.h"

#include <string.h>

/* The receiver was walked in its live lexical scope. Insert the selected
 * function or callable-field projection after it, before the saved arguments. */
static MO_MUST_USE struct note *dispatch_callee(struct inference *p, struct dispatch *at,
                                                struct value selected, bool method) {
    struct note *last = p->last;
    if (!infer_record(p, method ? at->name : at->syntax, selected))
        return NULL;
    struct note *callee = p->last;
    last->next = NULL;
    p->last = last;
    callee->next = at->receiver.note->next;
    at->receiver.note->next = callee;
    return callee;
}

static MO_MUST_USE bool finish_dot(struct inference *p, struct dispatch *at) {
    bool method;
    struct value selected = infer_member(p, at->receiver, at->name, &method);
    if (selected.type == NULL)
        return false;
    if (method && at->arguments != NULL && at->count == 0 && infer_receiver_only(selected.type))
        return infer_empty_dot_call(p, at->call);
    if (!method && at->arguments == NULL) {
        if (!infer_unify(p, at->type, selected.type, 0))
            return false;
        at->note->output.field = selected.field;
        if (p->checked)
            *at->graph = *selected.graph;
        return true;
    }
    struct term *fn = infer_root(selected.type);
    if (fn->shape.kind != MO_SOURCE_FUNCTION) {
        struct term function = {.shape = {.kind = MO_SOURCE_FUNCTION, .name = "fn"}};
        return infer_unify_as(p, fn, &function, MO_CODE_NOT_FUNCTION);
    }
    uint32_t offset = method ? 1u : 0u;
    if (fn->shape.count != at->count + offset + 1)
        return infer_problem(p, MO_CODE_CALL_ARGUMENTS, p->span,
                             "a call's arguments don't match its parameters",
                             "Pass every argument by position (reference 5.2).", NULL);
    if (method &&
        !infer_unify_as(p, at->receiver.type, infer_argument(p, fn, 0), MO_CODE_ARGUMENT_TYPE))
        return false;
    for (uint32_t i = 0; i < at->count; i++) {
        p->span = at->arguments->children[i]->span;
        if (!at->inputs[i].never &&
            !infer_unify_as(p, at->inputs[i].type, infer_argument(p, fn, i + offset),
                            MO_CODE_ARGUMENT_TYPE))
            return false;
    }
    p->span = at->name->span;
    if (!infer_unify(p, at->type, infer_argument(p, fn, fn->shape.count - 1), 0))
        return false;
    if (!p->checked)
        return true;
    if (!method && at->note->output.syntax == at->syntax) {
        MO_ASSERT(at->call != NULL);
        at->note->output.syntax = at->call;
    }
    bool callee = !method || selected.symbol != NULL;
    struct note *function = callee ? dispatch_callee(p, at, selected, method) : NULL;
    if (callee && function == NULL)
        return false;
    uint32_t receiver = method && callee ? 1u : 0u;
    struct note **operands =
        infer_allocate(p, at->count + 1 + receiver, sizeof(*operands), _Alignof(struct note *));
    if (operands == NULL)
        return false;
    operands[0] = callee ? function : at->receiver.note;
    if (receiver != 0)
        operands[1] = at->receiver.note;
    for (uint32_t i = 0; i < at->count; i++)
        operands[i + 1 + receiver] = at->inputs[i].note;
    *at->graph =
        (struct emitted){.output = {.operand_count = at->count + 1 + receiver,
                                    .symbol = selected.symbol,
                                    .native = method ? selected.graph->output.native : NULL,
                                    .field = UINT32_MAX,
                                    .receiver = method ? receiver : UINT32_MAX},
                         .operands = operands,
                         .function_type = method ? selected.graph->function_type : NULL,
                         .requirements = method ? selected.graph->requirements : NULL,
                         .parameters = method ? selected.graph->parameters : NULL};
    at->note->callee = method ? function : NULL;
    at->note->output.symbol = selected.symbol;
    return true;
}

static MO_MUST_USE bool finish_position(struct inference *p, struct dispatch *at) {
    uint32_t part;
    struct term *type = infer_tuple_part(p, infer_root(at->receiver.type), at->name, &part);
    if (type == NULL || !infer_unify(p, at->type, type, 0))
        return false;
    if (!p->checked)
        return true;
    struct note **operands = infer_allocate(p, 1, sizeof(*operands), _Alignof(struct note *));
    if (operands == NULL)
        return false;
    operands[0] = at->receiver.note;
    *at->graph =
        (struct emitted){.output = {.operand_count = 1, .field = part, .receiver = UINT32_MAX},
                         .operands = operands};
    return true;
}

static MO_MUST_USE struct term *failure_container(struct inference *p, bool option) {
    struct term *out = infer_native(p, option ? "Option" : "Result", option ? 1u : 2u);
    if (out == NULL)
        return NULL;
    for (uint32_t i = 0; i < out->shape.count; i++) {
        out->arguments[i] = infer_fresh(p, 0);
        if (out->arguments[i] == NULL)
            return NULL;
    }
    return out;
}

MO_MUST_USE bool infer_solve_failure(struct inference *p, struct failure_check *at, bool defaults) {
    p->span = at->span;
    struct term *input = infer_root(at->input);
    if (at->op != MO_T_EOF) {
        if (input->variable && defaults && input->numeric != 0 &&
            infer_finish_type(p, input, 0) == NULL)
            return false;
        input = infer_root(at->input);
        struct term *right = at->fallback == NULL ? NULL : infer_root(at->fallback);
        if (at->fallback == NULL) {
            if (input->variable)
                return true;
            if (!infer_named(input, "Duration")) {
                struct term *numeric = input->shape.kind == MO_SOURCE_PARAMETER
                                           ? input
                                           : infer_fresh(p, INT | BIG | DECIMAL | FLOAT);
                if (numeric == NULL ||
                    !infer_unify_as(p, input, numeric,
                                    infer_named(input, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                               : MO_CODE_NUMBER_ABILITY))
                    return false;
            }
            if (!infer_unify_as(p, input, at->value, at->code))
                return false;
            at->solved = true;
            return true;
        }
        if (infer_named(input, "Time") && (at->op == MO_T_PLUS || at->op == MO_T_MINUS)) {
            struct term *duration = infer_native(p, "Duration", 0);
            if (duration == NULL)
                return false;
            if (at->op == MO_T_PLUS) {
                if (!infer_unify(p, at->fallback, duration, 0) ||
                    !infer_unify_as(p, input, at->value, at->code))
                    return false;
                at->solved = true;
                return true;
            }
            if (right->variable && right->numeric == 0)
                return true;
            bool diff = infer_named(right, "Time");
            if (!infer_unify(p, at->fallback, diff ? input : duration, 0) ||
                !infer_unify_as(p, diff ? duration : input, at->value, at->code))
                return false;
            at->solved = true;
            return true;
        }
        if (infer_named(input, "Duration") &&
            (at->op == MO_T_PLUS || at->op == MO_T_MINUS || at->op == MO_T_STAR)) {
            struct term *wanted = at->op == MO_T_STAR ? infer_native(p, "Int", 0) : input;
            if (wanted == NULL || !infer_unify(p, at->fallback, wanted, 0) ||
                !infer_unify_as(p, input, at->value, at->code))
                return false;
            at->solved = true;
            return true;
        }
        if (input->variable)
            return true;
        struct term *numeric = input->shape.kind == MO_SOURCE_PARAMETER
                                   ? input
                                   : infer_fresh(p, INT | BIG | DECIMAL | FLOAT);
        if (numeric == NULL ||
            !infer_unify_as(p, input, numeric,
                            infer_named(input, "Byte") ? MO_CODE_BYTE_ARITHMETIC
                                                       : MO_CODE_NUMBER_ABILITY) ||
            !infer_unify(p, at->fallback, input, 0) ||
            !infer_unify_as(p, input, at->value, at->code))
            return false;
        at->solved = true;
        return true;
    }
    if (at->fallback != NULL) {
        if (input->variable && defaults && input->numeric != 0 &&
            infer_finish_type(p, input, 0) == NULL)
            return false;
        if (infer_root(input)->variable)
            return true;
        bool option = infer_named(input, "Option");
        struct term *value = option ? infer_argument(p, input, 0) : infer_native(p, "Bool", 0);
        if (value == NULL ||
            (!option && !infer_unify_as(p, input, value, MO_CODE_CONDITION_TYPE)) ||
            !infer_unify(p, at->value, value, 0) ||
            (!at->never && !infer_unify(p, at->fallback, value, 0)))
            return false;
        if (at->graph != NULL)
            at->graph->output.or_kind = option ? MO_CHECK_OR_OPTION : MO_CHECK_OR_BOOL;
        at->solved = true;
        return true;
    }
    if (input->variable && input->numeric != 0) {
        if (!defaults)
            return true;
        if (infer_finish_type(p, input, 0) == NULL)
            return false;
    }
    struct term *context = infer_root(at->context);
    bool option = infer_named(input, "Option"), result = infer_named(input, "Result");
    if (at->never && (infer_named(context, "Option") || infer_named(context, "Result")) &&
        !option && !result) {
        input = failure_container(p, infer_named(context, "Option"));
        if (input == NULL)
            return false;
        at->input = input;
        if (at->input_note != NULL)
            at->input_note->type = input;
        option = infer_named(input, "Option");
        result = !option;
    }
    if (context->variable && (option || result)) {
        struct term *wanted = failure_container(p, option);
        if (wanted == NULL || !infer_unify(p, context, wanted, 0))
            return false;
        context = infer_root(context);
    }
    if (input->variable && (infer_named(context, "Option") || infer_named(context, "Result"))) {
        struct term *wanted = failure_container(p, infer_named(context, "Option"));
        if (wanted == NULL || !infer_unify(p, input, wanted, 0))
            return false;
        input = infer_root(input);
        option = infer_named(input, "Option");
        result = !option;
    }
    if (input->variable || context->variable)
        return true;
    if (!option && !result)
        return infer_problem(p, MO_CODE_TRY_INPUT, p->span, "try needs a Result or an Option",
                             "try takes the value out of an Ok or Some (reference 8.3).", NULL);
    if (!infer_named(context, option ? "Option" : "Result"))
        return infer_problem(p, MO_CODE_TRY_RESULT, p->span,
                             "try needs a function returning the same failure kind",
                             "try works on a Result in a function returning Result, or on an "
                             "Option in a function returning Option; otherwise handle the "
                             "failure with case (reference 8.3).",
                             NULL);
    struct term *value = infer_argument(p, input, 0);
    if (value == NULL || !infer_unify(p, at->value, value, 0))
        return false;
    at->solved = true;
    return true;
}

MO_MUST_USE bool infer_finish_dispatches(struct inference *p) {
    bool progress;
    bool defaults = false;
    do {
        progress = false;
        for (struct dispatch *at = p->dispatches; at != NULL; at = at->next) {
            if (!infer_work(p))
                return false;
            if (at->solved)
                continue;
            p->span = at->name->span;
            if (at->group == NULL) {
                struct term *receiver = infer_root(at->receiver.type);
                if (receiver->variable && defaults && receiver->numeric != 0 &&
                    infer_finish_type(p, receiver, 0) == NULL)
                    return false;
                if (receiver->variable)
                    continue;
                if (at->position ? !finish_position(p, at) : !finish_dot(p, at))
                    return false;
                at->solved = true;
                progress = true;
                continue;
            }
            struct term *type = infer_root(at->type);
            if (type->variable)
                continue;
            if (type->shape.kind != MO_SOURCE_FUNCTION || type->shape.count < 2)
                return infer_problem(
                    p, MO_CODE_CALL_ARGUMENTS, p->span,
                    "an overloaded function needs a first argument",
                    "Overloads differ in their first parameter's type (reference 5.1).", NULL);
            struct term *receiver = infer_argument(p, type, 0);
            if (receiver == NULL)
                return false;
            if (infer_root(receiver)->variable && infer_root(receiver)->numeric == 0)
                continue;
            const struct mo_symbol *symbol =
                infer_select_overload(p, at->group, at->count, receiver);
            if (symbol == NULL) {
                if (!p->failed)
                    return infer_problem(
                        p, MO_CODE_ARGUMENT_TYPE, p->span,
                        "no overload takes this first argument's type",
                        "Overloads take local record or choice types (reference 5.1).", NULL);
                return false;
            }
            struct value selected = infer_declared_function(p, symbol);
            if (selected.type == NULL ||
                !infer_unify_as(p, selected.type, type, MO_CODE_ARGUMENT_TYPE))
                return false;
            at->note->output.symbol = symbol;
            if (p->checked) {
                *at->graph = *selected.graph;
                at->graph->output.symbol = symbol;
            }
            at->solved = true;
            progress = true;
        }
        for (struct failure_check *at = p->failures; at != NULL; at = at->next) {
            if (!infer_work(p))
                return false;
            if (!at->solved) {
                if (!infer_solve_failure(p, at, defaults))
                    return false;
                progress = progress || at->solved;
            }
        }
        if (!progress && !defaults) {
            defaults = true;
            progress = true;
        } else if (!progress) {
            for (struct failure_check *at = p->failures; at != NULL; at = at->next) {
                if (at->solved || at->op == MO_T_EOF)
                    continue;
                p->span = at->span;
                if (at->op == MO_T_MINUS && infer_named(at->input, "Time")) {
                    struct term *value = infer_root(at->value);
                    if (!infer_named(value, "Duration") && !infer_named(value, "Time"))
                        continue;
                    struct term *duration = infer_native(p, "Duration", 0);
                    if (duration == NULL)
                        return false;
                    bool diff = infer_named(value, "Duration");
                    if (!infer_unify(p, at->fallback, diff ? at->input : duration, 0) ||
                        !infer_unify_as(p, diff ? duration : at->input, at->value, at->code))
                        return false;
                    at->solved = true;
                    progress = true;
                    continue;
                }
                if (!infer_root(at->input)->variable)
                    continue;
                struct term *numeric = infer_fresh(p, INT | BIG | DECIMAL | FLOAT);
                if (numeric == NULL ||
                    !infer_unify_as(p, at->input, numeric, MO_CODE_NUMBER_ABILITY) ||
                    (at->fallback != NULL && !infer_unify(p, at->fallback, at->input, 0)) ||
                    !infer_unify_as(p, at->input, at->value, at->code))
                    return false;
                at->solved = true;
                progress = true;
            }
        }
    } while (progress);
    for (struct dispatch *at = p->dispatches; at != NULL; at = at->next)
        if (!at->solved)
            return at->position
                       ? infer_problem(
                             p, MO_CODE_CANNOT_INFER, at->name->span,
                             "no use decides this expression's type",
                             "A function's signature gives its types; inside a body types "
                             "are inferred from all their uses (reference 5.1).",
                             NULL)
                       : infer_problem(p, MO_CODE_CANNOT_INFER, at->name->span,
                                       "no use decides this dispatch's receiver type",
                                       "Give the value a type, or write the call another way "
                                       "(reference 5.4).",
                                       NULL);
    for (struct failure_check *at = p->failures; at != NULL; at = at->next)
        if (!at->solved)
            return infer_problem(
                p, MO_CODE_CANNOT_INFER, at->span,
                at->op != MO_T_EOF ? "no use decides this expression's type"
                                   : "no use decides this failure expression's type",
                at->op != MO_T_EOF
                    ? "A function's signature gives its types; inside a body types "
                      "are inferred from all their uses (reference 5.1)."
                    : "or needs Bool or Option, and try needs Option or Result in the "
                      "enclosing function (reference 8).",
                NULL);
    return true;
}

MO_MUST_USE bool infer_anchor_inputs(struct inference *p, const struct mo_syntax *anchor,
                                     const struct mo_syntax *parameters, uint32_t count,
                                     const struct mo_syntax **inputs) {
    const struct mo_syntax *values = anchor->children[0];
    memset(inputs, 0, (size_t)count * sizeof(*inputs));
    for (uint32_t i = 0; i < values->count; i++) {
        const struct mo_syntax *item = values->children[i];
        const struct mo_syntax *name = item->children[0];
        uint32_t at = parameters->count;
        if (count == parameters->count || infer_compare_text(name, "result") != 0) {
            for (at = 0; at < parameters->count; at++) {
                if (!infer_work(p))
                    return false;
                if (infer_compare_name(name, parameters->children[at]->children[0]) == 0)
                    break;
            }
            if (at == parameters->count)
                return infer_problem(
                    p, MO_CODE_ANCHOR_ARGUMENTS, name->span,
                    "an anchor names a parameter or result",
                    "Anchors give every parameter and result a value (reference 12.2).", NULL);
        }
        if (inputs[at] != NULL)
            return infer_problem(
                p, MO_CODE_ANCHOR_ARGUMENTS, name->span,
                "an anchor gives a name more than one value",
                "Anchors give every parameter and result one value (reference 12.2).",
                inputs[at]->children[0]);
        inputs[at] = item;
    }
    for (uint32_t i = 0; i < count; i++)
        if (inputs[i] == NULL)
            return infer_problem(p, MO_CODE_ANCHOR_ARGUMENTS, anchor->span,
                                 "an anchor gives every parameter and result a value",
                                 "No parameter or result can be omitted (reference 12.2).", NULL);
    return true;
}
