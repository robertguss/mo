#include "check/infer.h"

#include "base/codes.h"
#include "base/limits.h"

#include <string.h>

static MO_MUST_USE struct mo_match_pattern *
match_pattern(struct inference *p, const struct mo_syntax *node, struct term *type, uintptr_t key,
              uint32_t signature, uint32_t count, const char *name) {
    if (!infer_work(p))
        return NULL;
    if (count > MO_LIMIT_SOURCE_CHECK_WORK - p->work) {
        bool reported =
            infer_problem(p, MO_CODE_CHECK_WORK, p->span,
                          "the function needs too much pattern-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    if (count > MO_LIMIT_PROGRAM_CHECK_WORK - *p->signatures->work) {
        bool reported =
            infer_problem(p, MO_CODE_CHECK_PROGRAM_WORK, p->span,
                          "the program needs too much pattern-checking work", NULL, NULL);
        (void)reported;
        return NULL;
    }
    p->work += count;
    *p->signatures->work += count;
    struct typed_pattern *out = infer_allocate(p, 1, sizeof(*out), _Alignof(struct typed_pattern));
    struct mo_match_pattern **items =
        infer_allocate(p, count, sizeof(*items), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return NULL;
    memset(items, 0, count * sizeof(*items));
    *out = (struct typed_pattern){
        .match = {.key = key, .signature = signature, .count = count, .name = name, .items = items},
        .type = type,
        .syntax = node,
        .next = p->patterns};
    p->patterns = out;
    return &out->match;
}

static MO_MUST_USE bool pattern_bind(struct inference *p, const struct mo_syntax *node,
                                     struct term *type, uint32_t group, uint32_t alternative) {
    struct name_slot *entry = node->kind == MO_S_NAME ? infer_slot(p, node, false) : NULL;
    struct local *local = entry == NULL ? NULL : entry->active;
    if (alternative > 1 && node->kind != MO_S_UNDERSCORE &&
        (local == NULL || local->pattern_group != group))
        return infer_problem(p, MO_CODE_PATTERN_ALTERNATIVES, node->span,
                             "the alternatives don't bind the same names",
                             "Arms with the same body share a line; their names must be visible "
                             "throughout that body (reference 7.2).",
                             NULL);
    if (alternative > 1 && local != NULL && local->pattern_group == group &&
        local->pattern_alternative != alternative) {
        if (!infer_unify(p, local->type, type, 0))
            return false;
        local->pattern_alternative = alternative;
        return infer_record(
            p, node, (struct value){.type = type, .binding = local->name, .bound = local->binding});
    } else {
        if (!infer_bind(p, node, type, false))
            return false;
        if (node->kind != MO_S_UNDERSCORE) {
            local = p->locals;
            local->pattern_group = group;
            local->pattern_alternative = alternative;
        }
    }
    return true;
}

static MO_MUST_USE bool pattern_node(struct inference *p, const struct mo_syntax *node,
                                     struct term *type, uint32_t group, uint32_t alternative,
                                     struct mo_match_pattern **out) {
    *out = NULL;
    if (node->kind == MO_S_GROUP)
        return infer_pattern(p, node->children[0], type, group, alternative, out);
    if (node->kind == MO_S_UNDERSCORE || node->kind == MO_S_NAME)
        return pattern_bind(p, node, type, group, alternative);
    if (node->kind == MO_S_TUPLE) {
        struct term *tuple = infer_make_type(p, (struct mo_source_type){.kind = MO_SOURCE_TUPLE,
                                                                        .name = "()",
                                                                        .module = UINT32_MAX,
                                                                        .count = node->count});
        if (tuple == NULL)
            return false;
        for (uint32_t i = 0; i < node->count; i++) {
            tuple->arguments[i] = infer_fresh(p, 0);
            if (tuple->arguments[i] == NULL)
                return false;
        }
        if (!infer_unify(p, type, tuple, 0))
            return false;
        *out = match_pattern(p, node, type, 5, 1, node->count, "tuple");
        if (*out == NULL)
            return false;
        for (uint32_t i = 0; i < node->count; i++)
            if (!infer_pattern(p, node->children[i], tuple->arguments[i], group, alternative,
                               &(*out)->items[i]))
                return false;
        return infer_record(p, node, (struct value){.type = type});
    }
    if (node->kind == MO_S_LIST) {
        struct term *list = infer_native(p, "List", 1);
        if (list == NULL)
            return false;
        list->arguments[0] = infer_fresh(p, 0);
        if (list->arguments[0] == NULL || !infer_unify(p, type, list, 0))
            return false;
        struct mo_match_pattern **parts =
            infer_allocate(p, node->count, sizeof(*parts), _Alignof(struct mo_match_pattern *));
        if (parts == NULL)
            return false;
        uint32_t length = node->count;
        bool rest = length != 0 && node->children[length - 1]->kind == MO_S_REST;
        if (rest)
            length--;
        for (uint32_t i = 0; i < length; i++)
            if (!infer_pattern(p, node->children[i], list->arguments[0], group, alternative,
                               &parts[i]))
                return false;
        struct mo_match_pattern *tail = NULL;
        if (rest) {
            if (!infer_pattern(p, node->children[length]->children[0], type, group, alternative,
                               &tail))
                return false;
        } else {
            tail = match_pattern(p, node, type, 3, 2, 0, "[]");
            if (tail == NULL)
                return false;
        }
        /* Flat list prefixes do not consume C stack. */
        for (uint32_t i = length; i != 0; i--) {
            struct mo_match_pattern *cons = match_pattern(p, node, type, 4, 2, 2, "list cell");
            if (cons == NULL)
                return false;
            cons->items[0] = parts[i - 1];
            cons->items[1] = tail;
            tail = cons;
        }
        *out = tail;
        return infer_record(p, node, (struct value){.type = type});
    }
    bool qualified_call =
        node->kind == MO_S_POSTFIX && node->count == 3 && node->children[0]->kind == MO_S_CAPNAME &&
        node->children[1]->kind == MO_S_QUALIFIED && node->children[2]->kind == MO_S_CALL;
    if (node->kind == MO_S_CAPNAME || node->kind == MO_S_QUALIFIED || node->kind == MO_S_CALL ||
        qualified_call) {
        const struct mo_syntax *name = node->kind == MO_S_CALL ? node->children[0] : node;
        const struct mo_syntax *args = node->kind == MO_S_CALL ? node->children[1] : NULL;
        const struct mo_syntax *prefix = NULL;
        if (qualified_call) {
            prefix = node->children[0];
            name = node->children[1]->children[0];
            args = node->children[2]->children[0];
        }
        const struct mo_builtin_case *builtin = mo_builtin_case_by_name(name);
        if (prefix != NULL)
            builtin = NULL;
        if (builtin != NULL) {
            struct term *nominal = infer_builtin_type(p, builtin, type);
            if (nominal == NULL)
                return false;
            bool unnamed = builtin->count != 0 && builtin->fields[0] == NULL;
            if ((unnamed &&
                 (args == NULL || args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG)) ||
                (!unnamed && builtin->count != 0 && args == NULL))
                return infer_problem(p, MO_CODE_PATTERN_FIELDS, node->span,
                                     "the pattern doesn't match the case's fields",
                                     "A case with named fields is matched by naming them; Some(p) "
                                     "matches one unnamed value (reference 7.3).",
                                     NULL);
            *out = match_pattern(p, node, type, builtin->key, builtin->cases, builtin->count,
                                 builtin->name);
            if (*out == NULL)
                return false;
            bool seen[2] = {false, false};
            for (uint32_t i = 0; args != NULL && i < args->count; i++) {
                const struct mo_syntax *arg = args->children[i];
                uint32_t index = infer_builtin_field(builtin, arg);
                if (index == UINT32_MAX || index >= builtin->count || seen[index])
                    return infer_problem(
                        p, MO_CODE_PATTERN_FIELDS, arg->span,
                        "a matched field is absent or repeated",
                        "A case is matched with its named fields; fields not named "
                        "are ignored (reference 7.3).",
                        NULL);
                seen[index] = true;
                struct term *field = infer_builtin_part(p, builtin, nominal, index);
                if (field == NULL || !infer_pattern(p, unnamed ? arg : arg->children[1], field,
                                                    group, alternative, &(*out)->items[index]))
                    return false;
            }
            return infer_record(p, node,
                                (struct value){.type = type, .builtin_case = builtin->name});
        }
        uint32_t count = 0;
        const struct mo_symbol *symbol = infer_definition(p, name, prefix, &count);
        if (p->failed)
            return false;
        const struct mo_data_signature *data = symbol == NULL ? NULL : infer_data_type(p, symbol);
        if (data == NULL)
            return infer_problem(p, MO_CODE_PATTERN_CONSTRUCTOR, name->span,
                                 "a pattern doesn't name a record or case",
                                 "A pattern names a case or record, a literal, a tuple or a list "
                                 "(reference 7.3).",
                                 NULL);
        if (!infer_visible_data(p, data->type->declaration))
            return false;
        struct term *parameters[26] = {NULL};
        struct term *nominal = infer_annotation(p, data->type, parameters, 0);
        if (nominal == NULL || !infer_unify(p, type, nominal, 0))
            return false;
        if ((data->unnamed &&
             (args == NULL || args->count != 1 || args->children[0]->kind == MO_S_NAMED_ARG)) ||
            (!data->unnamed && args == NULL && data->count != 0))
            return infer_problem(p, MO_CODE_PATTERN_FIELDS, node->span,
                                 "the pattern doesn't match the case's fields",
                                 "A case with named fields is matched by naming them; Circle(p) is "
                                 "only for one unnamed value (reference 7.3).",
                                 symbol->declaration->children[0]);
        const struct mo_symbol *owner = data->type->declaration;
        *out = match_pattern(p, node, type, (uintptr_t)symbol,
                             symbol->owner == UINT32_MAX ? 1u : owner->case_count, data->count,
                             symbol->name);
        if (*out == NULL)
            return false;
        (*out)->choice = symbol->owner == UINT32_MAX ? NULL : owner;
        (*out)->data = data;
        bool *seen = infer_allocate(p, data->count, sizeof(*seen), _Alignof(bool));
        if (seen == NULL)
            return false;
        memset(seen, 0, data->count * sizeof(*seen));
        for (uint32_t i = 0; args != NULL && i < args->count; i++) {
            const struct mo_syntax *arg = args->children[i];
            const struct mo_field_signature *field = data->unnamed ? &data->fields[0]
                                                     : arg->kind == MO_S_NAMED_ARG
                                                         ? infer_find_field(data, arg->children[0])
                                                         : NULL;
            if (!data->unnamed && data->count == 1 && args->count == 1 &&
                arg->kind != MO_S_NAMED_ARG) {
                const struct mo_syntax *field_name = data->fields[0].name;
                size_t length = field_name->span.end - field_name->span.start;
                char *replacement = infer_allocate(p, length + 3, 1, 1);
                if (replacement == NULL)
                    return false;
                memcpy(replacement, field_name->text, length);
                memcpy(replacement + length, ": ", 3);
                bool reported = infer_problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                                              "a matched field hasn't been named",
                                              "A case with named fields is matched by naming them "
                                              "(reference 7.3).",
                                              field_name);
                (void)reported;
                if (p->error.code == MO_CODE_PATTERN_FIELDS &&
                    p->error.primary.span.start == arg->span.start) {
                    p->edit = (struct mo_edit){{arg->span.file, arg->span.start, arg->span.start},
                                               replacement};
                    p->fix = (struct mo_fix){"name the matched field", &p->edit, 1};
                    p->error.fixes = &p->fix;
                    p->error.fix_count = 1;
                }
                return false;
            }
            if (field == NULL || seen[field->index])
                return infer_problem(p, MO_CODE_PATTERN_FIELDS, arg->span,
                                     "a matched field is absent or repeated",
                                     "A record or case is matched with its named fields; fields "
                                     "not named are ignored (reference 7.3).",
                                     symbol->declaration->children[0]);
            seen[field->index] = true;
            struct term *field_type = infer_annotation(p, field->type, parameters, 0);
            if (field_type == NULL ||
                !infer_pattern(p, data->unnamed ? arg : arg->children[1], field_type, group,
                               alternative, &(*out)->items[field->index]))
                return false;
            p->last->output.field = field;
        }
        return infer_record(p, node, (struct value){.type = type, .symbol = symbol});
    }
    bool literal =
        node->kind == MO_S_INT || node->kind == MO_S_DECIMAL || node->kind == MO_S_TEXT ||
        node->kind == MO_S_CHAR || node->kind == MO_S_TRUE || node->kind == MO_S_FALSE ||
        (node->kind == MO_S_NEGATE &&
         (node->children[0]->kind == MO_S_INT || node->children[0]->kind == MO_S_DECIMAL));
    if (!literal)
        return infer_problem(p, MO_CODE_PATTERN_CONSTRUCTOR, node->span,
                             "a pattern doesn't name a record or case",
                             "A pattern names a case or record, a literal, a tuple or a list "
                             "(reference 7.3).",
                             NULL);
    /* Retain inference notes for contextual range checking, but no executable
     * rows: the normalized LITERAL already carries this spelling and type. */
    bool checked = p->checked;
    p->checked = false;
    struct value literal_value = infer_expression(p, node, type);
    p->checked = checked;
    if (literal_value.type == NULL)
        return false;
    bool boolean = node->kind == MO_S_TRUE || node->kind == MO_S_FALSE;
    *out =
        match_pattern(p, node, type, node->kind == MO_S_TRUE ? 1u : 2u, boolean ? 2u : UINT32_MAX,
                      0, boolean ? (node->kind == MO_S_TRUE ? "true" : "false") : "literal");
    return *out != NULL;
}

MO_MUST_USE bool infer_pattern(struct inference *p, const struct mo_syntax *node, struct term *type,
                               uint32_t group, uint32_t alternative,
                               struct mo_match_pattern **out) {
    if (!infer_work(p))
        return false;
    MO_ASSERT(p->pattern_depth < MO_LIMIT_NESTING_DEPTH);
    p->span = node->span;
    p->pattern_depth++;
    bool ok = pattern_node(p, node, type, group, alternative, out);
    p->pattern_depth--;
    return ok;
}

MO_MUST_USE struct value infer_condition(struct inference *p, const struct mo_syntax *node,
                                         bool keep) {
    bool previous = p->keep_patterns;
    p->keep_patterns = keep;
    struct term *boolean = infer_native(p, "Bool", 0);
    struct value v =
        boolean == NULL ? (struct value){0} : infer_as(p, node, boolean, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    return v;
}

MO_MUST_USE struct value infer_match(struct inference *p, const struct mo_syntax *node,
                                     struct term *expected) {
    struct value input = infer_expression(p, node->children[0], NULL);
    struct term *result = expected == NULL ? infer_fresh(p, 0) : expected;
    if (input.type == NULL || result == NULL)
        return (struct value){0};
    const struct mo_syntax *arms = node->children[1];
    uint32_t count = 0;
    for (uint32_t i = 0; i < arms->count; i++)
        count += arms->children[i]->children[0]->count;
    struct match_check *check = infer_allocate(p, 1, sizeof(*check), _Alignof(struct match_check));
    struct mo_match_pattern **patterns =
        infer_allocate(p, count, sizeof(*patterns), _Alignof(struct mo_match_pattern *));
    if (p->failed)
        return (struct value){0};
    struct mo_span header = node->span;
    header.end = node->children[0]->span.end;
    *check = (struct match_check){.patterns = patterns,
                                  .span = header,
                                  .end = node->span.end,
                                  .scrutinee = node->children[0],
                                  .type = input.type,
                                  .next = p->matches};
    p->matches = check;
    struct value emitted = infer_graph(p, (struct value){.type = result}, 1 + 2 * arms->count);
    if (emitted.type == NULL)
        return emitted;
    uint32_t operand = 1;
    if (p->checked) {
        struct emitted_match *match =
            infer_allocate(p, 1, sizeof(*match), _Alignof(struct emitted_match));
        struct mo_checked_arm *rows =
            infer_allocate(p, arms->count, sizeof(*rows), _Alignof(struct mo_checked_arm));
        if (p->failed)
            return (struct value){0};
        *match = (struct emitted_match){.output = {.arm_count = arms->count, .arms = rows},
                                        .arms = rows};
        emitted.graph->match = match;
        emitted.graph->operands[0] = input.note;
    }
    bool never = true;
    for (uint32_t i = 0; i < arms->count; i++) {
        struct local *mark = p->locals;
        struct capture_path *old_path = p->capture_path;
        const struct mo_syntax *arm = arms->children[i], *alternatives = arm->children[0];
        uint32_t group = ++p->pattern_group;
        for (uint32_t j = 0; j < alternatives->count; j++) {
            struct mo_match_pattern *normalized = NULL;
            if (!infer_pattern(p, alternatives->children[j], input.type, group, j + 1, &normalized))
                return (struct value){0};
            if (normalized == NULL && check->catch_all == NULL)
                check->catch_all = alternatives->children[j];
            if (arm->children[1] == NULL)
                check->patterns[check->count++] = normalized;
            for (struct local *local = p->locals; local != mark; local = local->previous) {
                if (local->pattern_alternative != j + 1) {
                    bool reported = infer_problem(
                        p, MO_CODE_PATTERN_ALTERNATIVES, alternatives->children[j]->span,
                        "the alternatives don't bind the same names",
                        "Arms with the same body share a line; their names must be "
                        "visible throughout that body (reference 7.2).",
                        local->name);
                    (void)reported;
                    return (struct value){0};
                }
            }
        }
        if (p->checked)
            emitted.graph->match->arms[i] = (struct mo_checked_arm){
                .syntax = arm, .pattern_count = alternatives->count, .guard = UINT32_MAX};
        if (arm->children[1] != NULL) {
            struct value guard = infer_condition(p, arm->children[1], true);
            if (guard.type == NULL)
                return guard;
            if (p->checked) {
                emitted.graph->match->arms[i].guard = operand;
                emitted.graph->operands[operand++] = guard.note;
            }
        }
        if (!infer_capture_path(p, node, i))
            return (struct value){0};
        struct value body = infer_expression(p, arm->children[2], result);
        if (body.type == NULL || !infer_leave(p, mark))
            return (struct value){0};
        p->capture_path = old_path;
        if (p->checked) {
            emitted.graph->match->arms[i].body = operand;
            emitted.graph->operands[operand++] = body.note;
        }
        never = never && body.never;
    }
    if (p->checked)
        emitted.graph->output.operand_count = operand;
    emitted.never = never;
    return emitted;
}

MO_MUST_USE struct value infer_conditional(struct inference *p, const struct mo_syntax *node,
                                           struct term *expected) {
    const struct mo_syntax *parts = node->children[2];
    bool has_else = parts->count != 0 && parts->children[parts->count - 1]->kind == MO_S_ELSE;
    struct term *result =
        has_else ? (expected == NULL ? infer_fresh(p, 0) : expected) : infer_native(p, "Unit", 0);
    struct term *condition = infer_native(p, "Bool", 0);
    if (result == NULL || condition == NULL)
        return (struct value){0};
    struct value emitted =
        infer_graph(p, (struct value){.type = result}, 2 + 2 * parts->count - (has_else ? 1u : 0u));
    if (emitted.type == NULL)
        return emitted;
    uint32_t operand = 0;
    struct local *mark = p->locals;
    bool previous = p->keep_patterns;
    p->keep_patterns = true;
    struct value test = infer_as(p, node->children[0], condition, MO_CODE_CONDITION_TYPE);
    p->keep_patterns = previous;
    if (test.type == NULL)
        return (struct value){0};
    if (p->checked)
        emitted.graph->operands[operand++] = test.note;
    struct capture_path *old_path = p->capture_path;
    if (!infer_capture_path(p, node, 0))
        return (struct value){0};
    struct value branch = has_else ? infer_as(p, node->children[1], result, p->node_expectation)
                                   : infer_discard(p, node->children[1]);
    if (branch.type == NULL || !infer_leave(p, mark))
        return (struct value){0};
    p->capture_path = old_path;
    if (p->checked)
        emitted.graph->operands[operand++] = branch.note;
    bool never = has_else && branch.never;
    for (uint32_t i = 0; i < parts->count; i++) {
        const struct mo_syntax *part = parts->children[i];
        if (part->kind == MO_S_IF_PART) {
            p->keep_patterns = true;
            test = infer_as(p, part->children[0], condition, MO_CODE_CONDITION_TYPE);
            p->keep_patterns = previous;
            if (test.type == NULL)
                return (struct value){0};
            if (p->checked)
                emitted.graph->operands[operand++] = test.note;
        }
        const struct mo_syntax *body = part->children[part->kind == MO_S_ELSE ? 0 : 1];
        if (!infer_capture_path(p, node, i + 1))
            return (struct value){0};
        branch = has_else ? infer_as(p, body, result, p->node_expectation) : infer_discard(p, body);
        if (branch.type == NULL || !infer_leave(p, mark))
            return (struct value){0};
        p->capture_path = old_path;
        if (p->checked)
            emitted.graph->operands[operand++] = branch.note;
        never = never && branch.never;
    }
    if (never)
        result = expected == NULL ? infer_native(p, "Unit", 0) : expected;
    emitted.type = result;
    emitted.never = never;
    return emitted;
}

MO_MUST_USE bool infer_binder(struct inference *p, const struct mo_syntax *node,
                              struct term *type) {
    if (node->kind == MO_S_BINDER)
        return infer_binder(p, node->children[0], type);
    if (node->kind != MO_S_TUPLE)
        return infer_bind(p, node, type, false);
    struct term *tuple = infer_make_type(
        p, (struct mo_source_type){
               .kind = MO_SOURCE_TUPLE, .name = "()", .module = UINT32_MAX, .count = node->count});
    if (tuple == NULL)
        return false;
    for (uint32_t i = 0; i < node->count; i++) {
        tuple->arguments[i] = infer_fresh(p, 0);
        if (tuple->arguments[i] == NULL)
            return false;
    }
    if (!infer_unify(p, tuple, type, 0))
        return false;
    for (uint32_t i = 0; i < node->count; i++)
        if (!infer_binder(p, node->children[i], tuple->arguments[i]))
            return false;
    return true;
}

/* The part of tuple type `t` that `index` reads, or NULL after reporting. */
MO_MUST_USE struct term *infer_tuple_part(struct inference *p, struct term *t,
                                          const struct mo_syntax *index, uint32_t *part) {
    uint32_t at = 0;
    bool fits = true;
    for (uint32_t i = 0; i < index->span.end - index->span.start; i++) {
        uint8_t c = index->text[i];
        if (c == '_')
            continue;
        uint32_t digit = (uint32_t)(c - '0');
        if (digit > 9 || at > (UINT32_MAX - digit) / 10) {
            fits = false;
            break;
        }
        at = at * 10 + digit;
    }
    if (!fits || t->shape.kind != MO_SOURCE_TUPLE || at >= t->shape.count) {
        bool reported =
            infer_problem(p, MO_CODE_TUPLE_POSITION, index->span, "the tuple has no such part",
                          "Tuple parts are read with .0, .1 and so on (reference 3.6).", NULL);
        (void)reported;
        return NULL;
    }
    *part = at;
    return infer_argument(p, t, at);
}

MO_MUST_USE struct value infer_position(struct inference *p, struct value v,
                                        const struct mo_syntax *index) {
    if (v.type == NULL)
        return v;
    struct term *t = infer_root(v.type);
    if (t->variable) {
        /* A later use may settle the tuple's type (reference 5.1); solved with the dispatches. */
        struct value out = infer_deferred_function(p, index, NULL, 0);
        if (out.type == NULL)
            return out;
        out.dispatch->receiver = v;
        out.dispatch->position = true;
        out.never = v.never;
        return out;
    }
    uint32_t at;
    struct term *part = infer_tuple_part(p, t, index, &at);
    if (part == NULL)
        return (struct value){0};
    struct value out = infer_graph(p, (struct value){.type = part, .never = v.never}, 1);
    if (p->checked && out.type != NULL) {
        out.graph->operands[0] = v.note;
        out.graph->output.field = at;
    }
    return out;
}
