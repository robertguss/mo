#include "lower/scalar.h"

#include <stdlib.h>
#include <string.h>

/* CPS is a work list, not C calls. A continuation may be shared by branches;
 * mutable values cross that boundary as explicit join parameters. */
struct variable {
    const struct mo_checked_binding *binding;
    struct variable *next;
};
struct continuation;
struct loop {
    struct continuation *break_, *continue_;
};
struct state {
    struct variable *variables;
    struct loop *loop;
    struct continuation *return_;
};
enum continuation_kind {
    K_END,
    K_SEQUENCE,
    K_BIND,
    K_ARGUMENTS,
    K_JUMP,
    K_SELECT,
    K_PATTERN,
    K_MATCH,
    K_FOR,
    K_OPTION,
    K_TRY,
    K_TEXT,
    K_PROMISE,
    K_ANCHOR
};
struct anchor_value {
    uint32_t expression, slot;
};
struct continuation {
    enum continuation_kind kind;
    struct continuation *next;
    const struct mo_checked_expression *row;
    uint32_t at, skip;
    mo_core_string name;
    const struct mo_checked_binding *binding;
    mo_core_expr_list arguments;
    struct variable *variables;
    bool result, diagnostic;
    mo_core_expr extra, then_, else_;
    mo_core_string position;
    mo_core_ty position_type;
    const struct mo_checked_pattern *pattern;
    const struct anchor_value *values;
};
enum task_kind { TASK_ROW, TASK_FINISH, TASK_CONDITION, TASK_PATTERN, TASK_ARM, TASK_COPY };
struct task {
    enum task_kind kind;
    struct task *next;
    uint32_t row;
    struct state state;
    struct continuation *continuation;
    mo_core_expr *out, *success, value, then_, else_;
    const struct mo_checked_pattern *pattern;
    const struct mo_checked_arm *arm;
    bool diagnostic;
};
struct body {
    struct lowering *p;
    struct instance *instance;
    const struct mo_checked_body *source;
    mo_core_string *names;
    struct task *tasks;
};

static void push(struct body *b, struct task task) {
    struct task *t = LOWER_NEW(b->p, struct task, 1);
    *t = task;
    t->next = b->tasks;
    b->tasks = t;
}
static const struct mo_checked_expression *row(struct body *b, uint32_t index) {
    MO_ASSERT(index < b->source->count);
    return &b->source->items[index];
}
static bool returns(struct body *b, uint32_t root) {
    bool *seen = LOWER_NEW(b->p, bool, b->source->count);
    seen[root] = true;
    for (uint32_t i = root + 1; i != 0;) {
        i--;
        lower_work(b->p, 1);
        if (!seen[i])
            continue;
        const struct mo_checked_expression *r = row(b, i);
        if (r->syntax->kind == MO_S_RETURN || r->syntax->kind == MO_S_STOP || r->try_ != NULL)
            return true;
        if (r->lambda != NULL)
            continue;
        for (uint32_t j = 0; j < r->operand_count; j++) {
            lower_work(b->p, 1);
            MO_ASSERT(r->operands[j] < i);
            seen[r->operands[j]] = true;
        }
    }
    return false;
}
static mo_core_ty type(struct body *b, const struct mo_source_type *source) {
    return lower_type(b->p, source, &b->instance->substitution);
}
static mo_core_string binding_name(struct body *b, const struct mo_checked_binding *binding) {
    MO_ASSERT(binding >= b->source->bindings &&
              binding < b->source->bindings + b->source->binding_count);
    return b->names[(size_t)(binding - b->source->bindings)];
}
static void binder(struct body *b, mo_core_expr e, size_t slot,
                   const struct mo_checked_binding *binding) {
    struct lower_node *node = (struct lower_node *)e;
    MO_ASSERT(slot < node->binders.count);
    node->binders.items[slot] = (struct mo_core_name){true, lower_text(b->p, binding->name)};
}
static void binder_slots(struct body *b, mo_core_expr e, size_t count) {
    struct lower_node *node = (struct lower_node *)e;
    node->binders.count = count;
    node->binders.items = LOWER_NEW(b->p, struct mo_core_name, count);
}
static struct continuation *continuation(struct body *b, enum continuation_kind kind) {
    struct continuation *k = LOWER_NEW(b->p, struct continuation, 1);
    k->kind = kind;
    return k;
}
static void emit(struct body *b, uint32_t index, struct state state, struct continuation *k,
                 mo_core_expr *out) {
    push(b, (struct task){
                .kind = TASK_ROW, .row = index, .state = state, .continuation = k, .out = out});
}
static void finish(struct body *b, mo_core_expr value, struct state state, struct continuation *k,
                   mo_core_expr *out) {
    push(b,
         (struct task){
             .kind = TASK_FINISH, .state = state, .continuation = k, .out = out, .value = value});
}
static mo_core_expr let(struct body *b, mo_core_string name, mo_core_expr value) {
    mo_core_expr e = lower_expr(b->p, MO_CORE_EXPR_LET);
    e->as.let_.name = name;
    e->as.let_.bound = value;
    binder_slots(b, e, 1);
    return e;
}
static mo_core_expr_list arguments(struct body *b, size_t count) {
    return (mo_core_expr_list){.items = LOWER_NEW(b->p, mo_core_expr, count), .count = count};
}
static mo_core_expr projection(struct body *b, mo_core_expr value, size_t index) {
    mo_core_expr e = lower_expr(b->p, MO_CORE_EXPR_PROJ);
    e->as.proj.expr = value;
    e->as.proj.index.digits = lower_number(b->p, index);
    return e;
}
static mo_core_expr jump(struct body *b, struct continuation *k, mo_core_expr value) {
    struct mo_span previous = b->p->span;
    if (k->row != NULL)
        b->p->span = k->row->syntax->span;
    size_t count = k->result ? 1 : 0;
    if (k->extra != NULL)
        count++;
    for (struct variable *v = k->variables; v != NULL; v = v->next)
        count++;
    mo_core_expr e = lower_expr(b->p, MO_CORE_EXPR_JUMP);
    e->as.jump.name = k->name;
    e->as.jump.args = arguments(b, count);
    size_t at = 0;
    if (k->result)
        e->as.jump.args.items[at++] = value;
    if (k->extra != NULL)
        e->as.jump.args.items[at++] = k->extra;
    for (struct variable *v = k->variables; v != NULL; v = v->next)
        e->as.jump.args.items[at++] = lower_var(b->p, binding_name(b, v->binding));
    b->p->span = previous;
    return e;
}
static mo_core_expr join(struct body *b, struct continuation *k, mo_core_ty result) {
    mo_core_expr e = lower_expr(b->p, MO_CORE_EXPR_JOIN);
    e->as.join.name = k->name;
    size_t count = result == NULL ? 0 : 1;
    if (k->position_type != NULL)
        count++;
    for (struct variable *v = k->variables; v != NULL; v = v->next)
        count++;
    e->as.join.params.count = count;
    e->as.join.params.items = LOWER_NEW(b->p, struct mo_core_param, count);
    binder_slots(b, e, count);
    size_t at = 0;
    if (result != NULL)
        e->as.join.params.items[at++] = (struct mo_core_param){lower_fresh(b->p), result};
    if (k->position_type != NULL)
        e->as.join.params.items[at++] = (struct mo_core_param){k->position, k->position_type};
    for (struct variable *v = k->variables; v != NULL; v = v->next) {
        e->as.join.params.items[at] =
            (struct mo_core_param){binding_name(b, v->binding), type(b, v->binding->type)};
        binder(b, e, at++, v->binding);
    }
    return e;
}
static struct continuation *branch_join(struct body *b, struct task t, mo_core_ty result,
                                        mo_core_expr **out) {
    if (t.continuation->kind == K_END)
        return t.continuation;
    struct continuation *k = continuation(b, K_JUMP);
    k->name = lower_fresh(b->p);
    k->result = true;
    k->variables = t.state.variables;
    mo_core_expr e = join(b, k, result);
    *t.out = e;
    finish(b, lower_var(b->p, e->as.join.params.items[0].name), t.state, t.continuation,
           &e->as.join.body);
    *out = &e->as.join.rest;
    return k;
}
static mo_core_expr field_read(struct body *b, mo_core_ty base, mo_core_expr value,
                               uint32_t field) {
    if (base->tag == MO_CORE_TY_TUPLE)
        return projection(b, value, field);
    mo_core_ctors ctors = lower_ctors(b->p, base);
    MO_ASSERT(ctors.count == 1 && field < ctors.items[0].fields.count);
    mo_core_expr e = lower_expr(b->p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = value;
    e->as.case_.arms.count = 1;
    e->as.case_.arms.items = LOWER_NEW(b->p, struct mo_core_arm, 1);
    struct mo_core_arm *arm = e->as.case_.arms.items;
    arm->ctor = ctors.items[0].name;
    arm->names.count = ctors.items[0].fields.count;
    arm->names.items = LOWER_NEW(b->p, mo_core_string, arm->names.count);
    for (size_t i = 0; i < arm->names.count; i++)
        arm->names.items[i] = lower_fresh(b->p);
    arm->body = lower_var(b->p, arm->names.items[field]);
    return e;
}

static mo_core_expr field_update(struct body *b, const struct mo_checked_expression *assignment,
                                 mo_core_expr value) {
    uint32_t path[MO_LIMIT_NESTING_DEPTH];
    size_t count = 0;
    uint32_t at = assignment->operands[0];
    while (row(b, at)->field != UINT32_MAX && row(b, at)->operand_count == 1) {
        if (count == MO_LIMIT_NESTING_DEPTH)
            lower_refuse(b->p, MO_CODE_LOWER_DEPTH, "field update path is too deep");
        path[count++] = at;
        at = row(b, at)->operands[0];
    }
    MO_ASSERT(count != 0);
    mo_core_expr answer = NULL, *out = &answer;
    mo_core_expr old = lower_var(b->p, binding_name(b, assignment->binding));
    for (size_t i = count; i != 0; i--) {
        const struct mo_checked_expression *step = row(b, path[i - 1]);
        mo_core_ty base = type(b, row(b, step->operands[0])->type);
        mo_core_ctors ctors = lower_ctors(b->p, base);
        MO_ASSERT(ctors.count == 1 && step->field < ctors.items[0].fields.count);
        mo_core_expr e = lower_expr(b->p, MO_CORE_EXPR_CASE);
        e->as.case_.expr = old;
        e->as.case_.arms.count = 1;
        e->as.case_.arms.items = LOWER_NEW(b->p, struct mo_core_arm, 1);
        struct mo_core_arm *arm = e->as.case_.arms.items;
        arm->ctor = ctors.items[0].name;
        arm->names.count = ctors.items[0].fields.count;
        arm->names.items = LOWER_NEW(b->p, mo_core_string, arm->names.count);
        mo_core_expr rebuilt = lower_expr(b->p, MO_CORE_EXPR_CON);
        rebuilt->as.con.ty = base;
        rebuilt->as.con.ctor = arm->ctor;
        rebuilt->as.con.args = arguments(b, arm->names.count);
        for (size_t j = 0; j < arm->names.count; j++) {
            arm->names.items[j] = lower_fresh(b->p);
            rebuilt->as.con.args.items[j] = lower_var(b->p, arm->names.items[j]);
        }
        arm->body = rebuilt;
        *out = e;
        out = &rebuilt->as.con.args.items[step->field];
        old = lower_var(b->p, arm->names.items[step->field]);
    }
    *out = value;
    return answer;
}

static void pattern_parts(struct body *b, const struct mo_checked_pattern *pattern,
                          mo_core_expr_list values, mo_core_expr yes, mo_core_expr no,
                          mo_core_expr *out) {
    mo_core_expr *slots = LOWER_NEW(b->p, mo_core_expr, pattern->part_count + 1);
    slots[pattern->part_count] = yes;
    push(b, (struct task){.kind = TASK_COPY, .out = out, .success = slots});
    /* Dependencies run right to left, tests run left to right. */
    for (uint32_t i = 0; i < pattern->part_count; i++) {
        uint32_t index = pattern->parts[i].index;
        MO_ASSERT(index < values.count);
        push(b, (struct task){.kind = TASK_PATTERN,
                              .pattern = pattern->parts[i].pattern,
                              .value = values.items[index],
                              .success = &slots[i + 1],
                              .else_ = no,
                              .out = &slots[i]});
    }
}

static void pattern_task(struct body *b, struct task t) {
    struct lowering *p = b->p;
    const struct mo_checked_pattern *pattern = t.pattern;
    p->span = pattern->syntax->span;
    mo_core_expr yes = t.success == NULL ? t.then_ : *t.success;
    MO_ASSERT(yes != NULL && t.else_ != NULL);
    if (pattern->kind == MO_PATTERN_ANY) {
        *t.out = yes;
        return;
    }
    if (pattern->kind == MO_PATTERN_BIND) {
        mo_core_expr e = let(b, binding_name(b, pattern->binding), t.value);
        binder(b, e, 0, pattern->binding);
        e->as.let_.body = yes;
        *t.out = e;
        return;
    }
    mo_core_ty ty = type(b, pattern->type);
    if (pattern->kind == MO_PATTERN_LITERAL) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        mo_core_expr_list args = arguments(b, 2);
        args.items[0] = t.value;
        const struct mo_syntax *s = pattern->syntax;
        bool negative = s->kind == MO_S_NEGATE || s->kind == MO_S_PREFIX;
        if (negative)
            s = s->children[s->count - 1];
        args.items[1] = lower_literal(p, s, ty, negative);
        e->as.if_.cond =
            lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = ty}, args);
        e->as.if_.then_ = yes;
        e->as.if_.else_ = t.else_;
        *t.out = e;
        return;
    }
    if (pattern->kind == MO_PATTERN_TUPLE) {
        mo_core_expr_list values = arguments(b, ty->as.tuple.count);
        for (size_t i = 0; i < values.count; i++)
            values.items[i] = projection(b, t.value, i);
        pattern_parts(b, pattern, values, yes, t.else_, t.out);
        return;
    }
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    *t.out = e;
    if (pattern->kind == MO_PATTERN_LIST) {
        MO_ASSERT(ty->tag == MO_CORE_TY_LIST);
        mo_core_expr_list args = arguments(b, 1);
        args.items[0] = t.value;
        e->as.case_.expr = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                  .as.list = {.t = ty->as.list, .op.tag = MO_CORE_LIST_OP_UNCONS}},
            args);
        e->as.case_.arms.count = 2;
        e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        e->as.case_.arms.items[0] = (struct mo_core_arm){
            .ctor = lower_text(p, "None"), .body = pattern->part_count == 0 ? yes : t.else_};
        struct mo_core_arm *some = &e->as.case_.arms.items[1];
        some->ctor = lower_text(p, "Some");
        some->names.count = 1;
        some->names.items = LOWER_NEW(p, mo_core_string, 1);
        some->names.items[0] = lower_fresh(p);
        if (pattern->part_count == 0)
            some->body = t.else_;
        else {
            mo_core_expr_list values = arguments(b, 2);
            for (size_t i = 0; i < 2; i++)
                values.items[i] = projection(b, lower_var(p, some->names.items[0]), i);
            pattern_parts(b, pattern, values, yes, t.else_, &some->body);
        }
        return;
    }
    MO_ASSERT(pattern->kind == MO_PATTERN_CONSTRUCTOR);
    e->as.case_.expr = t.value;
    mo_core_ctors ctors = lower_ctors(p, ty);
    e->as.case_.arms.count = ctors.count;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, ctors.count);
    mo_core_string selected =
        lower_text(p, pattern->constructor.symbol == NULL ? pattern->constructor.builtin
                                                          : pattern->constructor.symbol->name);
    for (size_t i = 0; i < ctors.count; i++) {
        struct mo_core_arm *arm = &e->as.case_.arms.items[i];
        arm->ctor = ctors.items[i].name;
        arm->names.count = ctors.items[i].fields.count;
        arm->names.items = LOWER_NEW(p, mo_core_string, arm->names.count);
        mo_core_expr_list values = arguments(b, arm->names.count);
        for (size_t j = 0; j < arm->names.count; j++) {
            arm->names.items[j] = lower_fresh(p);
            values.items[j] = lower_var(p, arm->names.items[j]);
        }
        if (lower_same(arm->ctor, selected))
            pattern_parts(b, pattern, values, yes, t.else_, &arm->body);
        else
            arm->body = t.else_;
    }
}

static struct variable *pattern_variables(struct body *b, const struct mo_checked_pattern *root,
                                          struct variable *outer) {
    bool *seen = LOWER_NEW(b->p, bool, b->source->binding_count);
    struct scan {
        const struct mo_checked_pattern *pattern;
        struct scan *next;
    };
    struct scan *stack = LOWER_NEW(b->p, struct scan, 1);
    stack->pattern = root;
    while (stack != NULL) {
        lower_work(b->p, 1);
        const struct mo_checked_pattern *pattern = stack->pattern;
        stack = stack->next;
        if (pattern->kind == MO_PATTERN_BIND) {
            MO_ASSERT(pattern->binding >= b->source->bindings &&
                      pattern->binding < b->source->bindings + b->source->binding_count);
            seen[(size_t)(pattern->binding - b->source->bindings)] = true;
        }
        for (uint32_t i = 0; i < pattern->part_count; i++) {
            struct scan *next = LOWER_NEW(b->p, struct scan, 1);
            *next = (struct scan){pattern->parts[i].pattern, stack};
            stack = next;
        }
    }
    for (uint32_t i = b->source->binding_count; i != 0; i--)
        if (seen[i - 1]) {
            struct variable *v = LOWER_NEW(b->p, struct variable, 1);
            *v = (struct variable){&b->source->bindings[i - 1], outer};
            outer = v;
        }
    return outer;
}

/* Only condition-introduced names read by the target cross its join. Outer
 * immutable names remain lexical; live vars cross explicitly. Canonical
 * binders are unique, so a name introduced here cannot be local to the target. */
static struct variable *condition_variables(struct body *b, uint32_t index, mo_core_expr target,
                                            struct variable *outer) {
    const struct mo_checked_expression *root = row(b, index);
    if (root->pattern == NULL && root->operation != MO_T_AND && root->operation != MO_T_OR &&
        root->operation != MO_T_IMPLIES)
        return outer;
    struct variable *candidates = outer;
    uint32_t *rows = LOWER_NEW(b->p, uint32_t, b->source->count);
    size_t count = 1;
    rows[0] = index;
    while (count != 0) {
        lower_work(b->p, 1);
        const struct mo_checked_expression *r = row(b, rows[--count]);
        if (r->pattern != NULL)
            candidates = pattern_variables(b, r->pattern, candidates);
        if (r->operation == MO_T_AND || r->operation == MO_T_OR || r->operation == MO_T_IMPLIES) {
            MO_ASSERT(count + 2 <= b->source->count);
            rows[count++] = r->operands[0];
            rows[count++] = r->operands[1];
        }
    }
    if (candidates == outer)
        return outer;
    struct scan {
        mo_core_expr expr;
        size_t child;
    };
    struct scan *stack = LOWER_NEW(b->p, struct scan, MO_LIMIT_CORE_DEPTH);
    size_t depth = 1;
    stack[0].expr = target;
    bool *used = LOWER_NEW(b->p, bool, b->source->binding_count);
    while (depth != 0) {
        lower_work(b->p, 1);
        struct scan *f = &stack[depth - 1];
        if (f->child == 0 && f->expr->tag == MO_CORE_EXPR_VAR)
            for (struct variable *v = candidates; v != outer; v = v->next) {
                lower_work(b->p, 1);
                if (lower_same(f->expr->as.var, binding_name(b, v->binding)))
                    used[(size_t)(v->binding - b->source->bindings)] = true;
            }
        mo_core_expr next = lower_child(f->expr, f->child++);
        if (next == NULL)
            depth--;
        else {
            if (depth == MO_LIMIT_CORE_DEPTH)
                lower_refuse(b->p, MO_CODE_LOWER_DEPTH, "condition target is too deep");
            stack[depth++] = (struct scan){.expr = next};
        }
    }
    for (uint32_t i = b->source->binding_count; i != 0; i--)
        if (used[i - 1]) {
            struct variable *v = LOWER_NEW(b->p, struct variable, 1);
            *v = (struct variable){&b->source->bindings[i - 1], outer};
            outer = v;
        }
    return outer;
}

static bool atom(mo_core_expr e) {
    return e->tag == MO_CORE_EXPR_LIT || e->tag == MO_CORE_EXPR_VAR ||
           e->tag == MO_CORE_EXPR_GLOBAL;
}

static bool small_target(mo_core_expr e) {
    if (atom(e) || e->tag == MO_CORE_EXPR_STOP)
        return true;
    mo_core_expr_list args;
    if (e->tag == MO_CORE_EXPR_JUMP)
        args = e->as.jump.args;
    else if (e->tag == MO_CORE_EXPR_PRIM)
        args = e->as.prim.args;
    else
        return false;
    if (args.count > 4)
        return false;
    for (size_t i = 0; i < args.count; i++)
        if (!atom(args.items[i]))
            return false;
    return true;
}

static mo_core_expr share_target(struct body *b, struct task t, mo_core_expr target,
                                 mo_core_expr **out) {
    if (small_target(target))
        return target;
    struct continuation *k = continuation(b, K_JUMP);
    k->name = lower_fresh(b->p);
    k->variables = condition_variables(b, t.row, target, t.state.variables);
    mo_core_expr e = join(b, k, NULL);
    e->as.join.body = target;
    **out = e;
    *out = &e->as.join.rest;
    return jump(b, k, NULL);
}

static void arm_task(struct body *b, struct task t) {
    struct lowering *p = b->p;
    const struct mo_checked_arm *arm = t.arm;
    p->span = arm->syntax->span;
    struct continuation *selected = continuation(b, K_JUMP);
    selected->name = lower_fresh(p);
    selected->variables = pattern_variables(b, arm->patterns[0], t.state.variables);
    if (arm->guard != UINT32_MAX)
        selected->variables = condition_variables(b, arm->guard, t.value, selected->variables);
    mo_core_expr success = join(b, selected, NULL);
    success->as.join.body = t.value;
    *t.out = success;
    if (arm->guard != UINT32_MAX && arm->pattern_count > 1) {
        /* Re-enter the next alternative after a false guard. A single shared
         * guard/body preserves closure identity even when patterns overlap. */
        static const struct mo_source_type int_ = {
            .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
        struct continuation *choose = continuation(b, K_JUMP);
        choose->name = lower_fresh(p);
        choose->position = lower_fresh(p);
        choose->position_type = type(b, &int_);
        choose->variables = t.state.variables;
        mo_core_expr dispatch = join(b, choose, NULL);
        success->as.join.rest = dispatch;
        struct continuation initial = *choose;
        initial.extra = lower_int(p, 0);
        dispatch->as.join.rest = jump(b, &initial, NULL);
        struct continuation *guard = continuation(b, K_JUMP);
        guard->name = lower_fresh(p);
        guard->position = lower_fresh(p);
        guard->position_type = choose->position_type;
        guard->variables = pattern_variables(b, arm->patterns[0], t.state.variables);
        mo_core_expr tested = join(b, guard, NULL);
        dispatch->as.join.body = tested;
        mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
        select->as.if_.then_ = jump(b, selected, NULL);
        mo_core_expr_list args = arguments(b, 2);
        args.items[0] = lower_var(p, guard->position);
        args.items[1] = lower_int(p, 1);
        choose->extra = lower_prim(
            p, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_ADD}, args);
        select->as.if_.else_ = jump(b, choose, NULL);
        push(b, (struct task){.kind = TASK_CONDITION,
                              .row = arm->guard,
                              .state = t.state,
                              .value = select,
                              .out = &tested->as.join.body});
        mo_core_expr failure = t.else_;
        for (uint32_t j = arm->pattern_count; j != 0; j--) {
            mo_core_expr gate = lower_expr(p, MO_CORE_EXPR_IF);
            args = arguments(b, 2);
            args.items[0] = lower_var(p, choose->position);
            args.items[1] = lower_int(p, j - 1);
            gate->as.if_.cond =
                lower_prim(p,
                           (struct mo_core_prim){.tag = MO_CORE_PRIM_CMP,
                                                 .as.cmp = {MO_CORE_BASE_INT, MO_CORE_CMP_OP_LE}},
                           args);
            gate->as.if_.else_ = failure;
            struct continuation retry = *choose;
            retry.extra = lower_int(p, j);
            guard->extra = lower_int(p, j - 1);
            push(b, (struct task){.kind = TASK_PATTERN,
                                  .pattern = arm->patterns[j - 1],
                                  .value = t.then_,
                                  .then_ = jump(b, guard, NULL),
                                  .else_ = jump(b, &retry, NULL),
                                  .out = &gate->as.if_.then_});
            failure = gate;
        }
        tested->as.join.rest = failure;
        return;
    }
    mo_core_expr failure = t.else_;
    for (uint32_t j = arm->pattern_count; j != 0; j--) {
        struct continuation *next = continuation(b, K_JUMP);
        next->name = lower_fresh(p);
        next->variables = t.state.variables;
        mo_core_expr fallback = join(b, next, NULL);
        fallback->as.join.body = failure;
        mo_core_expr yes = jump(b, selected, NULL);
        mo_core_expr no = jump(b, next, NULL);
        if (arm->guard != UINT32_MAX) {
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
            select->as.if_.then_ = yes;
            select->as.if_.else_ = no;
            mo_core_expr *guarded = LOWER_NEW(p, mo_core_expr, 1);
            push(b, (struct task){.kind = TASK_PATTERN,
                                  .pattern = arm->patterns[j - 1],
                                  .value = t.then_,
                                  .success = guarded,
                                  .else_ = no,
                                  .out = &fallback->as.join.rest});
            push(b, (struct task){.kind = TASK_CONDITION,
                                  .row = arm->guard,
                                  .state = t.state,
                                  .value = select,
                                  .out = guarded});
        } else
            push(b, (struct task){.kind = TASK_PATTERN,
                                  .pattern = arm->patterns[j - 1],
                                  .value = t.then_,
                                  .then_ = yes,
                                  .else_ = no,
                                  .out = &fallback->as.join.rest});
        failure = fallback;
    }
    success->as.join.rest = failure;
}

static void match(struct body *b, struct task t, struct continuation *k) {
    struct lowering *p = b->p;
    p->span = k->row->syntax->span;
    mo_core_expr bound = let(b, lower_fresh(p), t.value);
    *t.out = bound;
    mo_core_expr *out = &bound->as.let_.body;
    struct task branch = t;
    branch.out = out;
    branch.continuation = k->next;
    struct continuation *done = branch_join(b, branch, type(b, k->row->type), &out);
    mo_core_expr failure = lower_expr(p, MO_CORE_EXPR_STOP);
    failure->as.stop = lower_text(p, "unreachable pattern failure");
    const struct mo_checked_match *m = k->row->match;
    for (uint32_t i = m->arm_count; i != 0; i--) {
        const struct mo_checked_arm *arm = &m->arms[i - 1];
        p->span = arm->syntax->span;
        struct continuation *arm_next = continuation(b, K_JUMP);
        arm_next->name = lower_fresh(p);
        arm_next->variables = t.state.variables;
        mo_core_expr arm_exit = join(b, arm_next, NULL);
        arm_exit->as.join.body = failure;
        failure = jump(b, arm_next, NULL);
        MO_ASSERT(arm->pattern_count != 0);
        mo_core_expr *body = LOWER_NEW(p, mo_core_expr, 1);
        push(b, (struct task){.kind = TASK_ARM,
                              .arm = arm,
                              .state = t.state,
                              .success = body,
                              .then_ = lower_var(p, bound->as.let_.name),
                              .else_ = failure,
                              .out = &arm_exit->as.join.rest});
        emit(b, arm->body, t.state, done, body);
        failure = arm_exit;
    }
    *out = failure;
}

static mo_core_expr unary(struct body *b, struct mo_core_prim prim, mo_core_expr value) {
    mo_core_expr_list args = arguments(b, 1);
    args.items[0] = value;
    return lower_prim(b->p, prim, args);
}

static void loop(struct body *b, struct task t, const struct mo_checked_expression *r,
                 mo_core_expr iterable) {
    struct lowering *p = b->p;
    p->span = r->syntax->span;
    bool for_ = r->syntax->kind == MO_S_FOR;
    mo_core_expr *out = t.out;
    mo_core_ty collection = NULL;
    mo_core_string list = {0}, end = {0};
    if (for_) {
        collection = type(b, row(b, r->operands[0])->type);
        if (collection->tag == MO_CORE_TY_MAP || collection->tag == MO_CORE_TY_SET) {
            mo_core_ty converted = LOWER_NEW(p, struct mo_core_ty, 1);
            converted->tag = MO_CORE_TY_LIST;
            struct mo_core_prim prim = {0};
            if (collection->tag == MO_CORE_TY_SET) {
                converted->as.list = collection->as.set;
                prim.tag = MO_CORE_PRIM_SET;
                prim.as.set.t = collection->as.set;
                prim.as.set.op = MO_CORE_SET_OP_TO_LIST;
            } else {
                converted->as.list = LOWER_NEW(p, struct mo_core_ty, 1);
                converted->as.list->tag = MO_CORE_TY_TUPLE;
                converted->as.list->as.tuple.count = 2;
                converted->as.list->as.tuple.items = LOWER_NEW(p, mo_core_ty, 2);
                converted->as.list->as.tuple.items[0] = collection->as.map.k;
                converted->as.list->as.tuple.items[1] = collection->as.map.v;
                prim.tag = MO_CORE_PRIM_MAP;
                prim.as.map.k = collection->as.map.k;
                prim.as.map.v = collection->as.map.v;
                prim.as.map.op.tag = MO_CORE_MAP_OP_ENTRIES;
            }
            iterable = unary(b, prim, iterable);
            collection = converted;
        }
        mo_core_expr e = let(b, lower_fresh(p), iterable);
        *out = e;
        list = e->as.let_.name;
        out = &e->as.let_.body;
        if (collection->tag == MO_CORE_TY_RANGE) {
            mo_core_expr limit = let(b, lower_fresh(p),
                                     unary(b,
                                           (struct mo_core_prim){.tag = MO_CORE_PRIM_RANGE,
                                                                 .as.range = MO_CORE_RANGE_OP_END},
                                           lower_var(p, list)));
            *out = limit;
            end = limit->as.let_.name;
            out = &limit->as.let_.body;
        } else
            MO_ASSERT(collection->tag == MO_CORE_TY_LIST);
    }
    struct continuation *done = continuation(b, K_JUMP);
    done->name = lower_fresh(p);
    done->variables = t.state.variables;
    mo_core_expr exit = join(b, done, NULL);
    *out = exit;
    finish(b, lower_unit(p), t.state, t.continuation, &exit->as.join.body);
    struct continuation *again = continuation(b, K_JUMP);
    again->name = lower_fresh(p);
    again->row = r;
    again->variables = t.state.variables;
    if (for_) {
        static const struct mo_source_type int_type = {
            .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
        again->position = collection->tag == MO_CORE_TY_RANGE && r->binding != NULL
                              ? binding_name(b, r->binding)
                              : lower_fresh(p);
        again->position_type =
            collection->tag == MO_CORE_TY_RANGE ? type(b, &int_type) : collection;
    }
    mo_core_expr round = join(b, again, NULL);
    exit->as.join.rest = round;
    struct loop *scope = LOWER_NEW(p, struct loop, 1);
    *scope = (struct loop){done, again};
    struct state state = t.state;
    state.loop = scope;
    mo_core_expr *body_out;
    mo_core_expr element = NULL;
    struct continuation initial = *again;
    if (!for_) {
        mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
        select->as.if_.else_ = jump(b, done, NULL);
        push(b, (struct task){.kind = TASK_CONDITION,
                              .row = r->operands[0],
                              .state = state,
                              .value = select,
                              .out = &round->as.join.body});
        body_out = &select->as.if_.then_;
    } else if (collection->tag == MO_CORE_TY_RANGE) {
        if (r->binding != NULL)
            binder(b, round, 0, r->binding);
        mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
        round->as.join.body = select;
        mo_core_expr_list args = arguments(b, 2);
        args.items[0] = lower_var(p, again->position);
        args.items[1] = lower_var(p, end);
        select->as.if_.cond =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_CMP,
                                             .as.cmp = {MO_CORE_BASE_INT, MO_CORE_CMP_OP_LT}},
                       args);
        select->as.if_.else_ = jump(b, done, NULL);
        body_out = &select->as.if_.then_;
        element = lower_var(p, again->position);
        args = arguments(b, 2);
        args.items[0] = lower_var(p, again->position);
        args.items[1] = lower_int(p, 1);
        again->extra = lower_prim(
            p, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_ADD}, args);
        initial.extra = unary(
            b, (struct mo_core_prim){.tag = MO_CORE_PRIM_RANGE, .as.range = MO_CORE_RANGE_OP_START},
            lower_var(p, list));
    } else {
        mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
        round->as.join.body = select;
        select->as.case_.expr =
            unary(b,
                  (struct mo_core_prim){
                      .tag = MO_CORE_PRIM_LIST,
                      .as.list = {.t = collection->as.list, .op.tag = MO_CORE_LIST_OP_UNCONS}},
                  lower_var(p, again->position));
        select->as.case_.arms.count = 2;
        select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        select->as.case_.arms.items[0] =
            (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = jump(b, done, NULL)};
        struct mo_core_arm *some = &select->as.case_.arms.items[1];
        some->ctor = lower_text(p, "Some");
        some->names.count = 1;
        some->names.items = LOWER_NEW(p, mo_core_string, 1);
        some->names.items[0] = lower_fresh(p);
        mo_core_expr item =
            let(b, r->binding == NULL ? lower_fresh(p) : binding_name(b, r->binding),
                projection(b, lower_var(p, some->names.items[0]), 0));
        if (r->binding != NULL)
            binder(b, item, 0, r->binding);
        some->body = item;
        body_out = &item->as.let_.body;
        element = lower_var(p, item->as.let_.name);
        again->extra = projection(b, lower_var(p, some->names.items[0]), 1);
        initial.extra = lower_var(p, list);
    }
    round->as.join.rest = jump(b, &initial, NULL);
    if (for_ && r->binding == NULL && r->pattern != NULL) {
        mo_core_expr *success = LOWER_NEW(p, mo_core_expr, 1);
        mo_core_expr impossible = lower_expr(p, MO_CORE_EXPR_STOP);
        impossible->as.stop = lower_text(p, "unreachable loop binding failure");
        push(b, (struct task){.kind = TASK_PATTERN,
                              .pattern = r->pattern,
                              .value = element,
                              .success = success,
                              .else_ = impossible,
                              .out = body_out});
        body_out = success;
    }
    emit(b, r->operands[1], state, again, body_out);
}

static enum mo_syntax_kind kind(const struct mo_checked_expression *r) {
    const struct mo_syntax *s = r->syntax;
    if (r->operation != MO_T_EOF)
        return MO_S_BINARY;
    if (s->kind == MO_S_POSTFIX)
        return s->children[s->count - 1]->kind;
    if (s->kind == MO_S_BINARY_CHAIN)
        return MO_S_BINARY;
    if (s->kind == MO_S_PREFIX)
        return MO_S_NEGATE;
    return s->kind;
}

static const struct mo_ability_implementation *ability(struct body *b,
                                                       const struct mo_checked_expression *r,
                                                       enum mo_source_ability wanted,
                                                       uint32_t operand) {
    for (uint32_t i = 0; i < r->ability_count; i++) {
        lower_work(b->p, 1);
        const struct mo_checked_ability_use *use = &r->abilities[i];
        if (use->operand == operand && use->implementation->ability == wanted)
            return use->implementation;
    }
    lower_refuse(b->p, MO_CODE_LOWER_UNFINISHED, "this operation has no selected ability proof");
}

static const struct mo_ability_implementation *
parameter_ability(struct body *b, const struct mo_checked_expression *r, const char *parameter,
                  enum mo_source_ability wanted) {
    for (uint32_t i = 0; i < r->ability_count; i++) {
        lower_work(b->p, 1);
        const struct mo_checked_ability_use *use = &r->abilities[i];
        if (use->parameter != NULL && strcmp(use->parameter->name, parameter) == 0 &&
            use->implementation->ability == wanted)
            return use->implementation;
    }
    lower_refuse(b->p, MO_CODE_LOWER_UNFINISHED, "this operation has no selected ability proof");
}

static bool direct_function(struct body *b, const struct mo_checked_expression *r) {
    if (r->symbol == NULL || r->symbol->declaration->kind != MO_S_FN)
        return false;
    if (r->operand_count == 0)
        return true; /* A function-value adapter invokes its own declaration. */
    const struct mo_checked_expression *callee = row(b, r->operands[0]);
    return callee->operand_count == 0 && callee->symbol == r->symbol;
}

static mo_core_expr application(struct body *b, const struct mo_checked_expression *r,
                                mo_core_expr_list args) {
    struct lowering *p = b->p;
    mo_core_ty result = type(b, r->type);
    if (r->operation != MO_T_EOF) {
        mo_core_ty left = type(b, row(b, r->operands[0])->type);
        mo_core_ty right = args.count == 2 ? type(b, row(b, r->operands[1])->type) : NULL;
        if (r->operation == MO_T_LT || r->operation == MO_T_LE || r->operation == MO_T_GT ||
            r->operation == MO_T_GE) {
            for (uint32_t i = 0; i < r->ability_count; i++) {
                lower_work(p, 1);
                const struct mo_checked_ability_use *use = &r->abilities[i];
                if (use->operand != r->operands[0] ||
                    use->implementation->ability != MO_SOURCE_ORDER)
                    continue;
                const struct mo_ability_implementation *proof = use->implementation;
                const struct substitution *substitution = &b->instance->substitution;
                lower_proof(p, &proof, &substitution);
                if (proof->kind == MO_ABILITY_NATIVE)
                    continue;
                static const struct mo_source_type ordering = {
                    .kind = MO_SOURCE_NAMED, .name = "Ordering", .module = UINT32_MAX};
                mo_core_ty order = type(b, &ordering);
                mo_core_expr_list values = arguments(b, 2);
                values.items[0] = lower_compare(p, proof, substitution, b->instance->owner,
                                                b->instance->depth, args.items[0], args.items[1]);
                values.items[1] = lower_expr(p, MO_CORE_EXPR_CON);
                values.items[1]->as.con.ty = order;
                values.items[1]->as.con.ctor = lower_text(
                    p, r->operation == MO_T_LT || r->operation == MO_T_GE ? "Less" : "Greater");
                mo_core_expr e = lower_prim(
                    p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = order}, values);
                if (r->operation == MO_T_LE || r->operation == MO_T_GE)
                    e = unary(b, (struct mo_core_prim){.tag = MO_CORE_PRIM_NOT}, e);
                return e;
            }
        }
        if (left->tag == MO_CORE_TY_UNIT && (r->operation == MO_T_LT || r->operation == MO_T_LE ||
                                             r->operation == MO_T_GT || r->operation == MO_T_GE))
            return lower_bool(p, r->operation == MO_T_LE || r->operation == MO_T_GE);
        mo_core_expr e = lower_prim(p, lower_operator(p, r->operation, left, right), args);
        if (r->operation == MO_T_NE) {
            mo_core_expr_list one = arguments(b, 1);
            one.items[0] = e;
            return lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_NOT}, one);
        }
        return e;
    }
    if (r->native != NULL) {
        static const struct {
            const char *name;
            enum mo_core_int_op operation;
        } natives[] = {{"Int.abs", MO_CORE_INT_OP_ABS},
                       {"Int.min", MO_CORE_INT_OP_MIN},
                       {"Int.max", MO_CORE_INT_OP_MAX},
                       {"Int.checked_add", MO_CORE_INT_OP_CHECKED_ADD},
                       {"Int.to_decimal", MO_CORE_INT_OP_TO_DECIMAL},
                       {"Int.seconds", MO_CORE_INT_OP_SECONDS},
                       {"Int.milliseconds", MO_CORE_INT_OP_MILLISECONDS},
                       {"Int.minutes", MO_CORE_INT_OP_MINUTES},
                       {"Int.hours", MO_CORE_INT_OP_HOURS},
                       {"Int.days", MO_CORE_INT_OP_DAYS}};
        for (size_t i = 0; i < sizeof natives / sizeof natives[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, natives[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = natives[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_range_op operation;
        } ranges[] = {{"Range.to_list", MO_CORE_RANGE_OP_TO_LIST},
                      {"Range.contains?", MO_CORE_RANGE_OP_CONTAINS}};
        for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, ranges[i].name) == 0)
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_RANGE,
                                                        .as.range = ranges[i].operation},
                                  args);
        }
        static const struct {
            const char *name;
            enum mo_core_dec_op operation;
        } decimals[] = {{"Decimal.abs", MO_CORE_DEC_OP_ABS},
                        {"Decimal.min", MO_CORE_DEC_OP_MIN},
                        {"Decimal.max", MO_CORE_DEC_OP_MAX},
                        {"Decimal.floor", MO_CORE_DEC_OP_FLOOR},
                        {"Decimal.ceil", MO_CORE_DEC_OP_CEIL},
                        {"Decimal.round", MO_CORE_DEC_OP_ROUND},
                        {"Decimal.to_int", MO_CORE_DEC_OP_TO_INT},
                        {"Decimal.to_fixed", MO_CORE_DEC_OP_TO_FIXED},
                        {"Decimal.to_float", MO_CORE_DEC_OP_TO_FLOAT}};
        for (size_t i = 0; i < sizeof decimals / sizeof decimals[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, decimals[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_DEC, .as.dec = decimals[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_time_op operation;
        } times[] = {{"Time.checked_add", MO_CORE_TIME_OP_CHECKED_ADD},
                     {"Time.parse", MO_CORE_TIME_OP_PARSE},
                     {"Duration.in_milliseconds", MO_CORE_TIME_OP_IN_MILLISECONDS},
                     {"Duration.in_seconds", MO_CORE_TIME_OP_IN_SECONDS}};
        for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, times[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_TIME, .as.time = times[i].operation},
                    args);
        }
    }
    if (r->native != NULL) {
        static const struct {
            const char *name;
            struct mo_core_io_op operation;
        } io[] = {{"Console.print", {MO_CORE_IO_OP_PRINT, false}},
                  {"Console.error", {MO_CORE_IO_OP_PRINT_ERROR, false}},
                  {"Console.read_line", {MO_CORE_IO_OP_READ_LINE, false}},
                  {"Args.list", {MO_CORE_IO_OP_ARGS_LIST, false}},
                  {"Args.of", {MO_CORE_IO_OP_ARGS_OF, false}},
                  {"Console.fake", {MO_CORE_IO_OP_CONSOLE_FAKE, false}},
                  {"Console.fake_input", {MO_CORE_IO_OP_CONSOLE_FAKE_INPUT, false}},
                  {"Console.printed", {MO_CORE_IO_OP_PRINTED, false}},
                  {"Console.printed_errors", {MO_CORE_IO_OP_PRINTED_ERRORS, false}},
                  {"Files.in_memory", {MO_CORE_IO_OP_FILES_IN_MEMORY, false}},
                  {"Files.read_only", {MO_CORE_IO_OP_READ_ONLY, false}},
                  {"Files.read_text", {MO_CORE_IO_OP_READ_TEXT, false}},
                  {"Files.list", {MO_CORE_IO_OP_LIST, false}},
                  {"ReadFiles.read_text", {MO_CORE_IO_OP_READ_TEXT, true}},
                  {"ReadFiles.list", {MO_CORE_IO_OP_LIST, true}},
                  {"Clock.now", {MO_CORE_IO_OP_NOW, false}},
                  {"Clock.fixed", {MO_CORE_IO_OP_CLOCK_FIXED, false}}};
        for (size_t i = 0; i < sizeof io / sizeof io[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, io[i].name) == 0)
                return lower_prim(
                    p, (struct mo_core_prim){.tag = MO_CORE_PRIM_IO, .as.io = io[i].operation},
                    args);
        }
        if (strcmp(r->native, "Option.or_fail") == 0) {
            /* Both arguments are already evaluated (reference 8.1): e is
             * computed even for Some, so only the case remains. */
            MO_ASSERT(args.count == 2);
            mo_core_ty out = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(out->tag == MO_CORE_TY_RESULT);
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
            select->as.case_.expr = args.items[0];
            select->as.case_.arms.count = 2;
            select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            mo_core_expr error = lower_expr(p, MO_CORE_EXPR_CON);
            error->as.con.ty = out;
            error->as.con.ctor = lower_text(p, "Error");
            error->as.con.args = arguments(b, 1);
            error->as.con.args.items[0] = args.items[1];
            select->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = error};
            struct mo_core_arm *some = &select->as.case_.arms.items[1];
            some->ctor = lower_text(p, "Some");
            some->names.count = 1;
            some->names.items = LOWER_NEW(p, mo_core_string, 1);
            some->names.items[0] = lower_fresh(p);
            mo_core_expr ok = lower_expr(p, MO_CORE_EXPR_CON);
            ok->as.con.ty = out;
            ok->as.con.ctor = lower_text(p, "Ok");
            ok->as.con.args = arguments(b, 1);
            ok->as.con.args.items[0] = lower_var(p, some->names.items[0]);
            some->body = ok;
            return select;
        }
        if (strcmp(r->native, "List.map") == 0) {
            MO_ASSERT(args.count == 2);
            mo_core_ty in_list =
                r->operand_count == 0
                    ? result->as.fn.params.items[0]
                    : type(b,
                           row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)->type);
            if (in_list->tag == MO_CORE_TY_FN)
                in_list = in_list->as.fn.params.items[0];
            mo_core_ty out_list = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(in_list->tag == MO_CORE_TY_LIST && out_list->tag == MO_CORE_TY_LIST);
            mo_core_expr loop_join = lower_expr(p, MO_CORE_EXPR_JOIN);
            loop_join->as.join.name = lower_fresh(p);
            loop_join->as.join.params.count = 2;
            loop_join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 2);
            mo_core_string rem = lower_fresh(p), acc = lower_fresh(p);
            loop_join->as.join.params.items[0] = (struct mo_core_param){rem, in_list};
            loop_join->as.join.params.items[1] = (struct mo_core_param){acc, out_list};
            mo_core_expr start = lower_expr(p, MO_CORE_EXPR_JUMP);
            start->as.jump.name = loop_join->as.join.name;
            start->as.jump.args = arguments(b, 2);
            start->as.jump.args.items[0] = args.items[0];
            struct mo_core_prim empty = {.tag = MO_CORE_PRIM_LIST, .as.list.t = out_list->as.list};
            empty.as.list.op.tag = MO_CORE_LIST_OP_LIT;
            empty.as.list.op.as.lit.digits = lower_text(p, "0");
            start->as.jump.args.items[1] = lower_prim(p, empty, arguments(b, 0));
            loop_join->as.join.rest = start;
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
            select->as.case_.expr =
                unary(b,
                      (struct mo_core_prim){
                          .tag = MO_CORE_PRIM_LIST,
                          .as.list = {.t = in_list->as.list, .op.tag = MO_CORE_LIST_OP_UNCONS}},
                      lower_var(p, rem));
            select->as.case_.arms.count = 2;
            select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            select->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = lower_var(p, acc)};
            struct mo_core_arm *some = &select->as.case_.arms.items[1];
            some->ctor = lower_text(p, "Some");
            some->names.count = 1;
            some->names.items = LOWER_NEW(p, mo_core_string, 1);
            mo_core_string pair = lower_fresh(p);
            some->names.items[0] = pair;
            mo_core_expr app = lower_expr(p, MO_CORE_EXPR_APP);
            app->as.app.f = args.items[1];
            app->as.app.args = arguments(b, 1);
            app->as.app.args.items[0] = projection(b, lower_var(p, pair), 0);
            mo_core_expr mapped = let(b, lower_fresh(p), app);
            some->body = mapped;
            mo_core_expr again = lower_expr(p, MO_CORE_EXPR_JUMP);
            again->as.jump.name = loop_join->as.join.name;
            again->as.jump.args = arguments(b, 2);
            again->as.jump.args.items[0] = projection(b, lower_var(p, pair), 1);
            mo_core_expr_list append_args = arguments(b, 2);
            append_args.items[0] = lower_var(p, acc);
            append_args.items[1] = lower_var(p, mapped->as.let_.name);
            again->as.jump.args.items[1] =
                lower_prim(p,
                           (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                                 .as.list = {.t = out_list->as.list,
                                                             .op.tag = MO_CORE_LIST_OP_APPEND}},
                           append_args);
            mapped->as.let_.body = again;
            loop_join->as.join.body = select;
            return loop_join;
        }
        if (strcmp(r->native, "List.sort_by_desc") == 0) {
            MO_ASSERT(args.count == 2);
            mo_core_ty list_ty = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(list_ty->tag == MO_CORE_TY_LIST);
            mo_core_ty elem_ty = list_ty->as.list;
            static const struct mo_source_type int_source = {
                .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
            static const struct mo_source_type ordering_source = {
                .kind = MO_SOURCE_NAMED, .name = "Ordering", .module = UINT32_MAX};
            mo_core_ty int_ty = type(b, &int_source);
            mo_core_ty order_ty = type(b, &ordering_source);
            const struct mo_ability_implementation *order_proof =
                parameter_ability(b, r, "K", MO_SOURCE_ORDER);
            mo_core_expr total =
                let(b, lower_fresh(p),
                    unary(b,
                          (struct mo_core_prim){
                              .tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_SIZE}},
                          args.items[0]));
            mo_core_expr pass_join = lower_expr(p, MO_CORE_EXPR_JOIN);
            pass_join->as.join.name = lower_fresh(p);
            pass_join->as.join.params.count = 2;
            pass_join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 2);
            mo_core_string curr = lower_fresh(p), width = lower_fresh(p);
            pass_join->as.join.params.items[0] = (struct mo_core_param){curr, list_ty};
            pass_join->as.join.params.items[1] = (struct mo_core_param){width, int_ty};
            mo_core_expr pass_start = lower_expr(p, MO_CORE_EXPR_JUMP);
            pass_start->as.jump.name = pass_join->as.join.name;
            pass_start->as.jump.args = arguments(b, 2);
            pass_start->as.jump.args.items[0] = args.items[0];
            pass_start->as.jump.args.items[1] = lower_int(p, 1);
            pass_join->as.join.rest = pass_start;
            total->as.let_.body = pass_join;

            mo_core_expr pass_if = lower_expr(p, MO_CORE_EXPR_IF);
            mo_core_expr_list cmp_args = arguments(b, 2);
            cmp_args.items[0] = lower_var(p, width);
            cmp_args.items[1] = lower_var(p, total->as.let_.name);
            pass_if->as.if_.cond =
                lower_prim(p,
                           (struct mo_core_prim){.tag = MO_CORE_PRIM_CMP,
                                                 .as.cmp = {MO_CORE_BASE_INT, MO_CORE_CMP_OP_GE}},
                           cmp_args);
            pass_if->as.if_.then_ = lower_var(p, curr);
            pass_join->as.join.body = pass_if;

            mo_core_expr sweep_join = lower_expr(p, MO_CORE_EXPR_JOIN);
            sweep_join->as.join.name = lower_fresh(p);
            sweep_join->as.join.params.count = 2;
            sweep_join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 2);
            mo_core_string rem = lower_fresh(p), out = lower_fresh(p);
            sweep_join->as.join.params.items[0] = (struct mo_core_param){rem, list_ty};
            sweep_join->as.join.params.items[1] = (struct mo_core_param){out, list_ty};
            mo_core_expr sweep_start = lower_expr(p, MO_CORE_EXPR_JUMP);
            sweep_start->as.jump.name = sweep_join->as.join.name;
            sweep_start->as.jump.args = arguments(b, 2);
            sweep_start->as.jump.args.items[0] = lower_var(p, curr);
            struct mo_core_prim empty = {.tag = MO_CORE_PRIM_LIST, .as.list.t = elem_ty};
            empty.as.list.op.tag = MO_CORE_LIST_OP_LIT;
            empty.as.list.op.as.lit.digits = lower_text(p, "0");
            sweep_start->as.jump.args.items[1] = lower_prim(p, empty, arguments(b, 0));
            sweep_join->as.join.rest = sweep_start;
            pass_if->as.if_.else_ = sweep_join;

            mo_core_expr sweep_if = lower_expr(p, MO_CORE_EXPR_IF);
            sweep_if->as.if_.cond =
                unary(b,
                      (struct mo_core_prim){
                          .tag = MO_CORE_PRIM_LIST,
                          .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_IS_EMPTY}},
                      lower_var(p, rem));
            mo_core_expr next_pass = lower_expr(p, MO_CORE_EXPR_JUMP);
            next_pass->as.jump.name = pass_join->as.join.name;
            next_pass->as.jump.args = arguments(b, 2);
            next_pass->as.jump.args.items[0] = lower_var(p, out);
            mo_core_expr_list dbl_args = arguments(b, 2);
            dbl_args.items[0] = lower_var(p, width);
            dbl_args.items[1] = lower_var(p, width);
            next_pass->as.jump.args.items[1] = lower_prim(
                p, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_ADD},
                dbl_args);
            sweep_if->as.if_.then_ = next_pass;
            sweep_join->as.join.body = sweep_if;

            struct mo_core_prim take_prim = {
                .tag = MO_CORE_PRIM_LIST,
                .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_TAKE}};
            struct mo_core_prim drop_prim = {
                .tag = MO_CORE_PRIM_LIST,
                .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_DROP}};
            struct mo_core_prim concat_prim = {
                .tag = MO_CORE_PRIM_LIST,
                .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_CONCAT}};
            struct mo_core_prim append_prim = {
                .tag = MO_CORE_PRIM_LIST,
                .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_APPEND}};
            struct mo_core_prim uncons_prim = {
                .tag = MO_CORE_PRIM_LIST,
                .as.list = {.t = elem_ty, .op.tag = MO_CORE_LIST_OP_UNCONS}};

            mo_core_expr_list slice_args = arguments(b, 2);
            slice_args.items[0] = lower_var(p, rem);
            slice_args.items[1] = lower_var(p, width);
            mo_core_expr left = let(b, lower_fresh(p), lower_prim(p, take_prim, slice_args));
            slice_args = arguments(b, 2);
            slice_args.items[0] = lower_var(p, rem);
            slice_args.items[1] = lower_var(p, width);
            mo_core_expr rest1 = let(b, lower_fresh(p), lower_prim(p, drop_prim, slice_args));
            slice_args = arguments(b, 2);
            slice_args.items[0] = lower_var(p, rest1->as.let_.name);
            slice_args.items[1] = lower_var(p, width);
            mo_core_expr right = let(b, lower_fresh(p), lower_prim(p, take_prim, slice_args));
            slice_args = arguments(b, 2);
            slice_args.items[0] = lower_var(p, rest1->as.let_.name);
            slice_args.items[1] = lower_var(p, width);
            mo_core_expr next_rem = let(b, lower_fresh(p), lower_prim(p, drop_prim, slice_args));
            left->as.let_.body = rest1;
            rest1->as.let_.body = right;
            right->as.let_.body = next_rem;
            sweep_if->as.if_.else_ = left;

            mo_core_expr merge_join = lower_expr(p, MO_CORE_EXPR_JOIN);
            merge_join->as.join.name = lower_fresh(p);
            merge_join->as.join.params.count = 3;
            merge_join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 3);
            mo_core_string l_var = lower_fresh(p), r_var = lower_fresh(p), acc_var = lower_fresh(p);
            merge_join->as.join.params.items[0] = (struct mo_core_param){l_var, list_ty};
            merge_join->as.join.params.items[1] = (struct mo_core_param){r_var, list_ty};
            merge_join->as.join.params.items[2] = (struct mo_core_param){acc_var, list_ty};
            mo_core_expr merge_start = lower_expr(p, MO_CORE_EXPR_JUMP);
            merge_start->as.jump.name = merge_join->as.join.name;
            merge_start->as.jump.args = arguments(b, 3);
            merge_start->as.jump.args.items[0] = lower_var(p, left->as.let_.name);
            merge_start->as.jump.args.items[1] = lower_var(p, right->as.let_.name);
            merge_start->as.jump.args.items[2] = lower_var(p, out);
            merge_join->as.join.rest = merge_start;
            next_rem->as.let_.body = merge_join;

            mo_core_expr l_case = lower_expr(p, MO_CORE_EXPR_CASE);
            l_case->as.case_.expr = unary(b, uncons_prim, lower_var(p, l_var));
            l_case->as.case_.arms.count = 2;
            l_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            mo_core_expr l_done = lower_expr(p, MO_CORE_EXPR_JUMP);
            l_done->as.jump.name = sweep_join->as.join.name;
            l_done->as.jump.args = arguments(b, 2);
            l_done->as.jump.args.items[0] = lower_var(p, next_rem->as.let_.name);
            mo_core_expr_list concat_r = arguments(b, 2);
            concat_r.items[0] = lower_var(p, acc_var);
            concat_r.items[1] = lower_var(p, r_var);
            l_done->as.jump.args.items[1] = lower_prim(p, concat_prim, concat_r);
            l_case->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = l_done};
            struct mo_core_arm *l_some = &l_case->as.case_.arms.items[1];
            l_some->ctor = lower_text(p, "Some");
            l_some->names.count = 1;
            l_some->names.items = LOWER_NEW(p, mo_core_string, 1);
            mo_core_string l_pair = lower_fresh(p);
            l_some->names.items[0] = l_pair;
            merge_join->as.join.body = l_case;

            mo_core_expr r_case = lower_expr(p, MO_CORE_EXPR_CASE);
            r_case->as.case_.expr = unary(b, uncons_prim, lower_var(p, r_var));
            r_case->as.case_.arms.count = 2;
            r_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            mo_core_expr r_done = lower_expr(p, MO_CORE_EXPR_JUMP);
            r_done->as.jump.name = sweep_join->as.join.name;
            r_done->as.jump.args = arguments(b, 2);
            r_done->as.jump.args.items[0] = lower_var(p, next_rem->as.let_.name);
            mo_core_expr_list concat_l = arguments(b, 2);
            concat_l.items[0] = lower_var(p, acc_var);
            concat_l.items[1] = lower_var(p, l_var);
            r_done->as.jump.args.items[1] = lower_prim(p, concat_prim, concat_l);
            r_case->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = r_done};
            struct mo_core_arm *r_some = &r_case->as.case_.arms.items[1];
            r_some->ctor = lower_text(p, "Some");
            r_some->names.count = 1;
            r_some->names.items = LOWER_NEW(p, mo_core_string, 1);
            mo_core_string r_pair = lower_fresh(p);
            r_some->names.items[0] = r_pair;
            l_some->body = r_case;

            mo_core_expr l_head = let(b, lower_fresh(p), projection(b, lower_var(p, l_pair), 0));
            mo_core_expr r_head = let(b, lower_fresh(p), projection(b, lower_var(p, r_pair), 0));
            mo_core_expr l_app = lower_expr(p, MO_CORE_EXPR_APP);
            l_app->as.app.f = args.items[1];
            l_app->as.app.args = arguments(b, 1);
            l_app->as.app.args.items[0] = lower_var(p, l_head->as.let_.name);
            mo_core_expr l_key = let(b, lower_fresh(p), l_app);
            mo_core_expr r_app = lower_expr(p, MO_CORE_EXPR_APP);
            r_app->as.app.f = args.items[1];
            r_app->as.app.args = arguments(b, 1);
            r_app->as.app.args.items[0] = lower_var(p, r_head->as.let_.name);
            mo_core_expr r_key = let(b, lower_fresh(p), r_app);
            l_head->as.let_.body = r_head;
            r_head->as.let_.body = l_key;
            l_key->as.let_.body = r_key;
            r_some->body = l_head;

            mo_core_expr pick = lower_expr(p, MO_CORE_EXPR_IF);
            mo_core_expr_list ord_args = arguments(b, 2);
            ord_args.items[0] = lower_compare(
                p, order_proof, &b->instance->substitution, b->instance->owner, b->instance->depth,
                lower_var(p, l_key->as.let_.name), lower_var(p, r_key->as.let_.name));
            ord_args.items[1] = lower_expr(p, MO_CORE_EXPR_CON);
            ord_args.items[1]->as.con.ty = order_ty;
            ord_args.items[1]->as.con.ctor = lower_text(p, "Less");
            pick->as.if_.cond = lower_prim(
                p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = order_ty}, ord_args);
            mo_core_expr take_r = lower_expr(p, MO_CORE_EXPR_JUMP);
            take_r->as.jump.name = merge_join->as.join.name;
            take_r->as.jump.args = arguments(b, 3);
            take_r->as.jump.args.items[0] = lower_var(p, l_var);
            take_r->as.jump.args.items[1] = projection(b, lower_var(p, r_pair), 1);
            mo_core_expr_list append_r = arguments(b, 2);
            append_r.items[0] = lower_var(p, acc_var);
            append_r.items[1] = lower_var(p, r_head->as.let_.name);
            take_r->as.jump.args.items[2] = lower_prim(p, append_prim, append_r);
            pick->as.if_.then_ = take_r;

            mo_core_expr take_l = lower_expr(p, MO_CORE_EXPR_JUMP);
            take_l->as.jump.name = merge_join->as.join.name;
            take_l->as.jump.args = arguments(b, 3);
            take_l->as.jump.args.items[0] = projection(b, lower_var(p, l_pair), 1);
            take_l->as.jump.args.items[1] = lower_var(p, r_var);
            mo_core_expr_list append_l = arguments(b, 2);
            append_l.items[0] = lower_var(p, acc_var);
            append_l.items[1] = lower_var(p, l_head->as.let_.name);
            take_l->as.jump.args.items[2] = lower_prim(p, append_prim, append_l);
            pick->as.if_.else_ = take_l;
            r_key->as.let_.body = pick;
            return total;
        }
        if (strcmp(r->native, "Map.update") == 0) {
            MO_ASSERT(args.count == 4);
            mo_core_ty map_ty = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(map_ty->tag == MO_CORE_TY_MAP);
            mo_core_expr_list get_args = arguments(b, 2);
            get_args.items[0] = args.items[0];
            get_args.items[1] = args.items[1];
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
            select->as.case_.expr =
                lower_prim(p,
                           (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                                 .as.map = {.k = map_ty->as.map.k,
                                                            .v = map_ty->as.map.v,
                                                            .op.tag = MO_CORE_MAP_OP_GET}},
                           get_args);
            select->as.case_.arms.count = 2;
            select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            select->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = args.items[2]};
            struct mo_core_arm *some = &select->as.case_.arms.items[1];
            some->ctor = lower_text(p, "Some");
            some->names.count = 1;
            some->names.items = LOWER_NEW(p, mo_core_string, 1);
            mo_core_string found = lower_fresh(p);
            some->names.items[0] = found;
            some->body = lower_var(p, found);
            mo_core_expr current = let(b, lower_fresh(p), select);
            mo_core_expr app = lower_expr(p, MO_CORE_EXPR_APP);
            app->as.app.f = args.items[3];
            app->as.app.args = arguments(b, 1);
            app->as.app.args.items[0] = lower_var(p, current->as.let_.name);
            mo_core_expr updated = let(b, lower_fresh(p), app);
            current->as.let_.body = updated;
            mo_core_expr_list set_args = arguments(b, 3);
            set_args.items[0] = args.items[0];
            set_args.items[1] = args.items[1];
            set_args.items[2] = lower_var(p, updated->as.let_.name);
            updated->as.let_.body =
                lower_prim(p,
                           (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                                 .as.map = {.k = map_ty->as.map.k,
                                                            .v = map_ty->as.map.v,
                                                            .op.tag = MO_CORE_MAP_OP_SET}},
                           set_args);
            return current;
        }
        if (strcmp(r->native, "List.all?") == 0) {
            MO_ASSERT(args.count == 2);
            mo_core_ty in_list =
                r->operand_count == 0
                    ? result->as.fn.params.items[0]
                    : type(b,
                           row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)->type);
            if (in_list->tag == MO_CORE_TY_FN)
                in_list = in_list->as.fn.params.items[0];
            mo_core_ty bool_ty = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(in_list->tag == MO_CORE_TY_LIST && bool_ty->tag == MO_CORE_TY_BOOL);
            mo_core_expr loop_join = lower_expr(p, MO_CORE_EXPR_JOIN);
            loop_join->as.join.name = lower_fresh(p);
            loop_join->as.join.params.count = 1;
            loop_join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 1);
            mo_core_string rem = lower_fresh(p);
            loop_join->as.join.params.items[0] = (struct mo_core_param){rem, in_list};
            mo_core_expr start = lower_expr(p, MO_CORE_EXPR_JUMP);
            start->as.jump.name = loop_join->as.join.name;
            start->as.jump.args = arguments(b, 1);
            start->as.jump.args.items[0] = args.items[0];
            loop_join->as.join.rest = start;
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
            select->as.case_.expr =
                unary(b,
                      (struct mo_core_prim){
                          .tag = MO_CORE_PRIM_LIST,
                          .as.list = {.t = in_list->as.list, .op.tag = MO_CORE_LIST_OP_UNCONS}},
                      lower_var(p, rem));
            select->as.case_.arms.count = 2;
            select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            select->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = lower_bool(p, true)};
            struct mo_core_arm *some = &select->as.case_.arms.items[1];
            some->ctor = lower_text(p, "Some");
            some->names.count = 1;
            some->names.items = LOWER_NEW(p, mo_core_string, 1);
            mo_core_string pair = lower_fresh(p);
            some->names.items[0] = pair;
            mo_core_expr app = lower_expr(p, MO_CORE_EXPR_APP);
            app->as.app.f = args.items[1];
            app->as.app.args = arguments(b, 1);
            app->as.app.args.items[0] = projection(b, lower_var(p, pair), 0);
            mo_core_expr branch = lower_expr(p, MO_CORE_EXPR_IF);
            branch->as.if_.cond = app;
            mo_core_expr hit = lower_expr(p, MO_CORE_EXPR_JUMP);
            hit->as.jump.name = loop_join->as.join.name;
            hit->as.jump.args = arguments(b, 1);
            hit->as.jump.args.items[0] = projection(b, lower_var(p, pair), 1);
            branch->as.if_.then_ = hit;
            branch->as.if_.else_ = lower_bool(p, false);
            some->body = branch;
            loop_join->as.join.body = select;
            return loop_join;
        }
        if (strcmp(r->native, "List.count") == 0) {
            MO_ASSERT(args.count == 2);
            mo_core_ty in_list =
                r->operand_count == 0
                    ? result->as.fn.params.items[0]
                    : type(b,
                           row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)->type);
            if (in_list->tag == MO_CORE_TY_FN)
                in_list = in_list->as.fn.params.items[0];
            mo_core_ty int_ty = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(in_list->tag == MO_CORE_TY_LIST && int_ty->tag == MO_CORE_TY_INT);
            mo_core_expr loop_join = lower_expr(p, MO_CORE_EXPR_JOIN);
            loop_join->as.join.name = lower_fresh(p);
            loop_join->as.join.params.count = 2;
            loop_join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 2);
            mo_core_string rem = lower_fresh(p), acc = lower_fresh(p);
            loop_join->as.join.params.items[0] = (struct mo_core_param){rem, in_list};
            loop_join->as.join.params.items[1] = (struct mo_core_param){acc, int_ty};
            mo_core_expr start = lower_expr(p, MO_CORE_EXPR_JUMP);
            start->as.jump.name = loop_join->as.join.name;
            start->as.jump.args = arguments(b, 2);
            start->as.jump.args.items[0] = args.items[0];
            start->as.jump.args.items[1] = lower_int(p, 0);
            loop_join->as.join.rest = start;
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
            select->as.case_.expr =
                unary(b,
                      (struct mo_core_prim){
                          .tag = MO_CORE_PRIM_LIST,
                          .as.list = {.t = in_list->as.list, .op.tag = MO_CORE_LIST_OP_UNCONS}},
                      lower_var(p, rem));
            select->as.case_.arms.count = 2;
            select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            select->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = lower_var(p, acc)};
            struct mo_core_arm *some = &select->as.case_.arms.items[1];
            some->ctor = lower_text(p, "Some");
            some->names.count = 1;
            some->names.items = LOWER_NEW(p, mo_core_string, 1);
            mo_core_string pair = lower_fresh(p);
            some->names.items[0] = pair;
            mo_core_expr app = lower_expr(p, MO_CORE_EXPR_APP);
            app->as.app.f = args.items[1];
            app->as.app.args = arguments(b, 1);
            app->as.app.args.items[0] = projection(b, lower_var(p, pair), 0);
            mo_core_expr branch = lower_expr(p, MO_CORE_EXPR_IF);
            branch->as.if_.cond = app;
            mo_core_expr hit = lower_expr(p, MO_CORE_EXPR_JUMP);
            hit->as.jump.name = loop_join->as.join.name;
            hit->as.jump.args = arguments(b, 2);
            hit->as.jump.args.items[0] = projection(b, lower_var(p, pair), 1);
            mo_core_expr_list inc_args = arguments(b, 2);
            inc_args.items[0] = lower_var(p, acc);
            inc_args.items[1] = lower_int(p, 1);
            hit->as.jump.args.items[1] = lower_prim(
                p, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_ADD},
                inc_args);
            mo_core_expr miss = lower_expr(p, MO_CORE_EXPR_JUMP);
            miss->as.jump.name = loop_join->as.join.name;
            miss->as.jump.args = arguments(b, 2);
            miss->as.jump.args.items[0] = projection(b, lower_var(p, pair), 1);
            miss->as.jump.args.items[1] = lower_var(p, acc);
            branch->as.if_.then_ = hit;
            branch->as.if_.else_ = miss;
            some->body = branch;
            loop_join->as.join.body = select;
            return loop_join;
        }
        if (strcmp(r->native, "List.join") == 0)
            return lower_prim(
                p, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_JOIN},
                args);
        static const struct {
            const char *name;
            enum mo_core_list_op_tag operation;
        } lists[] = {{"List.first", MO_CORE_LIST_OP_FIRST},
                     {"List.last", MO_CORE_LIST_OP_LAST},
                     {"List.get", MO_CORE_LIST_OP_GET},
                     {"List.size", MO_CORE_LIST_OP_SIZE},
                     {"List.empty?", MO_CORE_LIST_OP_IS_EMPTY},
                     {"List.append", MO_CORE_LIST_OP_APPEND},
                     {"List.prepend", MO_CORE_LIST_OP_PREPEND},
                     {"List.concat", MO_CORE_LIST_OP_CONCAT},
                     {"List.reverse", MO_CORE_LIST_OP_REVERSE},
                     {"List.take", MO_CORE_LIST_OP_TAKE},
                     {"List.drop", MO_CORE_LIST_OP_DROP},
                     {"List.set", MO_CORE_LIST_OP_SET},
                     {"List.remove_at", MO_CORE_LIST_OP_REMOVE_AT},
                     {"List.enumerate", MO_CORE_LIST_OP_ENUMERATE},
                     {"List.contains?", MO_CORE_LIST_OP_CONTAINS},
                     {"List.unique", MO_CORE_LIST_OP_UNIQUE}};
        for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, lists[i].name) == 0) {
                mo_core_ty receiver =
                    r->operand_count == 0
                        ? result->as.fn.params.items[0]
                        : type(b, row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)
                                      ->type);
                if (receiver->tag == MO_CORE_TY_FN)
                    receiver = receiver->as.fn.params.items[0];
                MO_ASSERT(receiver->tag == MO_CORE_TY_LIST);
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                                        .as.list = {.t = receiver->as.list,
                                                                    .op.tag = lists[i].operation}},
                                  args);
            }
        }
        static const struct {
            const char *name;
            enum mo_core_text_op operation;
        } texts[] = {{"Text.bytes", MO_CORE_TEXT_OP_BYTES},
                     {"Text.byte_count", MO_CORE_TEXT_OP_BYTE_COUNT},
                     {"Text.char_count", MO_CORE_TEXT_OP_CHAR_COUNT},
                     {"Text.empty?", MO_CORE_TEXT_OP_IS_EMPTY},
                     {"Text.chars", MO_CORE_TEXT_OP_CHARS},
                     {"Text.from_chars", MO_CORE_TEXT_OP_FROM_CHARS},
                     {"Text.words", MO_CORE_TEXT_OP_WORDS},
                     {"Text.split", MO_CORE_TEXT_OP_SPLIT},
                     {"Text.trim", MO_CORE_TEXT_OP_TRIM},
                     {"Text.trim_start", MO_CORE_TEXT_OP_TRIM_START},
                     {"Text.trim_end", MO_CORE_TEXT_OP_TRIM_END},
                     {"Text.pad_left", MO_CORE_TEXT_OP_PAD_LEFT},
                     {"Text.pad_right", MO_CORE_TEXT_OP_PAD_RIGHT},
                     {"Text.starts_with?", MO_CORE_TEXT_OP_STARTS_WITH},
                     {"Text.ends_with?", MO_CORE_TEXT_OP_ENDS_WITH},
                     {"Text.drop_prefix", MO_CORE_TEXT_OP_DROP_PREFIX},
                     {"Text.drop_suffix", MO_CORE_TEXT_OP_DROP_SUFFIX},
                     {"Text.replace", MO_CORE_TEXT_OP_REPLACE},
                     {"Text.repeat", MO_CORE_TEXT_OP_REPEAT},
                     {"Text.lines", MO_CORE_TEXT_OP_LINES},
                     {"Text.contains?", MO_CORE_TEXT_OP_CONTAINS},
                     {"Text.ascii_lowercase", MO_CORE_TEXT_OP_LOWER},
                     {"Text.ascii_uppercase", MO_CORE_TEXT_OP_UPPER},
                     {"Text.to_int", MO_CORE_TEXT_OP_TO_INT},
                     {"Text.to_decimal", MO_CORE_TEXT_OP_TO_DECIMAL},
                     {"Text.to_float", MO_CORE_TEXT_OP_TO_FLOAT}};
        for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, texts[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = texts[i].operation},
                    args);
        }
    }
    if (r->native != NULL &&
        (strcmp(r->native, "Map.get") == 0 || strcmp(r->native, "Map.set") == 0 ||
         strcmp(r->native, "Map.remove") == 0 || strcmp(r->native, "Map.size") == 0 ||
         strcmp(r->native, "Map.values") == 0)) {
        mo_core_ty receiver =
            r->operand_count == 0
                ? result->as.fn.params.items[0]
                : type(b, row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)->type);
        if (receiver->tag == MO_CORE_TY_FN)
            receiver = receiver->as.fn.params.items[0];
        MO_ASSERT(receiver->tag == MO_CORE_TY_MAP);
        struct mo_core_prim prim = {
            .tag = MO_CORE_PRIM_MAP,
            .as.map = {.k = receiver->as.map.k,
                       .v = receiver->as.map.v,
                       .op.tag = strcmp(r->native, "Map.get") == 0      ? MO_CORE_MAP_OP_GET
                                 : strcmp(r->native, "Map.set") == 0    ? MO_CORE_MAP_OP_SET
                                 : strcmp(r->native, "Map.remove") == 0 ? MO_CORE_MAP_OP_REMOVE
                                 : strcmp(r->native, "Map.size") == 0   ? MO_CORE_MAP_OP_SIZE
                                                                        : MO_CORE_MAP_OP_VALUES}};
        return lower_prim(p, prim, args);
    }
    if (r->native != NULL && strncmp(r->native, "Set.", 4) == 0) {
        static const struct {
            const char *name;
            enum mo_core_set_op operation;
        } sets[] = {{"Set.empty", MO_CORE_SET_OP_EMPTY},
                    {"Set.size", MO_CORE_SET_OP_SIZE},
                    {"Set.empty?", MO_CORE_SET_OP_IS_EMPTY},
                    {"Set.has?", MO_CORE_SET_OP_HAS},
                    {"Set.add", MO_CORE_SET_OP_ADD},
                    {"Set.remove", MO_CORE_SET_OP_REMOVE},
                    {"Set.union", MO_CORE_SET_OP_UNION},
                    {"Set.intersection", MO_CORE_SET_OP_INTERSECTION},
                    {"Set.difference", MO_CORE_SET_OP_DIFFERENCE},
                    {"Set.to_list", MO_CORE_SET_OP_TO_LIST}};
        for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, sets[i].name) != 0)
                continue;
            mo_core_ty receiver =
                r->operand_count == 0
                    ? (result->tag == MO_CORE_TY_FN ? result->as.fn.params.items[0] : result)
                    : type(b,
                           row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)->type);
            if (receiver->tag == MO_CORE_TY_FN)
                receiver = receiver->as.fn.params.items[0];
            MO_ASSERT(receiver->tag == MO_CORE_TY_SET);
            return lower_prim(
                p,
                (struct mo_core_prim){.tag = MO_CORE_PRIM_SET,
                                      .as.set = {.t = receiver->as.set, .op = sets[i].operation}},
                args);
        }
    }
    if (r->native != NULL) {
        const char *member = strrchr(r->native, '.');
        if (member != NULL && strcmp(member, ".to_text") == 0) {
            MO_ASSERT(args.count == 1);
            /* Qualified natives are already selected by the shared signature
             * table; an ordinary same-name helper has a source symbol instead. */
            mo_core_ty receiver = r->operand_count == 0
                                      ? type(b, r->type)->as.fn.params.items[0]
                                      : type(b, row(b, r->operands[r->operand_count - 1])->type);
            return lower_error_text(p, receiver, args.items[0]);
        }
    }
    if (r->native != NULL && strcmp(r->native, "to_text") == 0) {
        MO_ASSERT(args.count == 1);
        return lower_to_text(p, ability(b, r, MO_SOURCE_TEXT, r->operands[0]),
                             &b->instance->substitution, b->instance->owner, b->instance->depth,
                             args.items[0]);
    }
    if (r->native != NULL && strcmp(r->native, "compare") == 0) {
        MO_ASSERT(args.count == 2);
        return lower_compare(p, ability(b, r, MO_SOURCE_ORDER, r->operands[0]),
                             &b->instance->substitution, b->instance->owner, b->instance->depth,
                             args.items[0], args.items[1]);
    }
    if (r->native != NULL && strcmp(r->native, "None") != 0 && strcmp(r->native, "Some") != 0 &&
        strcmp(r->native, "Ok") != 0 && strcmp(r->native, "Error") != 0 &&
        strcmp(r->native, "Less") != 0 && strcmp(r->native, "Equal") != 0 &&
        strcmp(r->native, "Greater") != 0 && strcmp(r->native, "NotFound") != 0 &&
        strcmp(r->native, "NotAllowed") != 0 && strcmp(r->native, "NotText") != 0 &&
        strcmp(r->native, "Failed") != 0)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this native call is not lowered yet");
    if (kind(r) == MO_S_TUPLE || kind(r) == MO_S_MAP_ENTRY) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_TUPLE);
        e->as.tuple = args;
        return e;
    }
    if (r->field != UINT32_MAX && r->operand_count == 1 && kind(r) != MO_S_NAMED_ARG)
        return field_read(b, type(b, row(b, r->operands[0])->type), args.items[0], r->field);
    if (kind(r) == MO_S_LIST) {
        struct mo_core_prim prim = {
            .tag = MO_CORE_PRIM_LIST,
            .as.list = {
                .t = result->as.list,
                .op = {.tag = MO_CORE_LIST_OP_LIT, .as.lit.digits = lower_number(p, args.count)}}};
        return lower_prim(p, prim, args);
    }
    if (kind(r) == MO_S_MAP) {
        struct mo_core_prim prim = {
            .tag = MO_CORE_PRIM_MAP,
            .as.map = {
                .k = result->as.map.k,
                .v = result->as.map.v,
                .op = {.tag = MO_CORE_MAP_OP_LIT, .lit.digits = lower_number(p, args.count / 2)}}};
        return lower_prim(p, prim, args);
    }
    if (direct_function(b, r)) {
        struct instance *f = lower_instance(p, r->symbol, r->arguments, &b->instance->substitution,
                                            r->ability_count, r->abilities, b->instance->owner,
                                            b->instance->depth + (r->arguments.count != 0));
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CALL);
        e->as.call.name = f->fun.name;
        e->as.call.args = args;
        return e;
    }
    if ((r->symbol != NULL && r->symbol->declaration->kind != MO_S_CONST &&
         r->symbol->declaration->kind != MO_S_FN) ||
        (r->native != NULL && strchr(r->native, '.') == NULL)) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CON);
        e->as.con.ty = result;
        e->as.con.ctor = lower_text(p, r->symbol != NULL ? r->symbol->name : r->native);
        e->as.con.args = arguments(b, args.count);
        for (size_t i = 0; i < args.count; i++) {
            const struct mo_checked_expression *operand = row(b, r->operands[i]);
            size_t slot = kind(operand) == MO_S_NAMED_ARG ? operand->field : i;
            MO_ASSERT(slot < args.count);
            e->as.con.args.items[slot] = args.items[i];
        }
        return e;
    }
    if (kind(r) == MO_S_CALL) {
        MO_ASSERT(args.count != 0);
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_APP);
        e->as.app.f = args.items[0];
        e->as.app.args = (mo_core_expr_list){args.items + 1, args.count - 1};
        return e;
    }
    lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this checked operation is not lowered yet");
}

static mo_core_expr text_value(struct body *b, const char *value) {
    mo_core_expr e = lower_unit(b->p);
    e->as.lit->tag = MO_CORE_LIT_TEXT;
    e->as.lit->as.text = lower_text(b->p, value);
    return e;
}

static mo_core_expr text_concat(struct body *b, mo_core_expr left, mo_core_expr right) {
    mo_core_expr_list values = arguments(b, 2);
    values.items[0] = left;
    values.items[1] = right;
    return lower_prim(
        b->p, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_CONCAT},
        values);
}

/* Named netstrings retain raw structural text, including newline/NUL. Bind the
 * observation once: its byte length and payload must describe the same value. */
static mo_core_expr detail_field(struct body *b, const char *label, mo_core_ty type_,
                                 mo_core_expr value) {
    mo_core_expr e =
        let(b, lower_fresh(b->p),
            unary(b, (struct mo_core_prim){.tag = MO_CORE_PRIM_STRUCTURAL, .as.structural = type_},
                  value));
    mo_core_expr text = lower_var(b->p, e->as.let_.name);
    mo_core_expr count = unary(
        b, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_BYTE_COUNT},
        text);
    count =
        unary(b, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_TO_TEXT},
              count);
    e->as.let_.body =
        text_concat(b, text_concat(b, text_value(b, label), count),
                    text_concat(b, text_value(b, ":"), text_concat(b, text, text_value(b, ","))));
    return e;
}

static void finish_task(struct body *b, struct task t) {
    struct lowering *p = b->p;
    struct continuation *k = t.continuation;
    if (k->kind == K_END) {
        *t.out = t.value;
        return;
    }
    if (k->kind == K_PROMISE) {
        const struct mo_checked_spec *spec = b->instance->spec;
        p->span = spec->conditions[k->at].text.span;
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        e->as.if_.cond = t.value;
        e->as.if_.else_ = lower_int(p, (uint64_t)k->at + 1);
        *t.out = e;
        if (k->at + 1 == spec->condition_count)
            e->as.if_.then_ = lower_int(p, 0);
        else {
            struct continuation *next = continuation(b, K_PROMISE);
            next->at = k->at + 1;
            emit(b, spec->conditions[next->at].expression, t.state, next, &e->as.if_.then_);
        }
        return;
    }
    if (k->kind == K_ANCHOR) {
        const struct mo_checked_spec *spec = b->instance->spec;
        mo_core_expr *out = t.out;
        mo_core_expr value = t.value;
        if (!atom(value)) {
            mo_core_expr bound = let(b, lower_fresh(p), value);
            *out = bound;
            out = &bound->as.let_.body;
            value = lower_var(p, bound->as.let_.name);
        }
        k->arguments.items[k->values[k->at].slot] = value;
        if (k->at + 1 < spec->value_count) {
            struct continuation *next = continuation(b, K_ANCHOR);
            *next = *k;
            next->at++;
            emit(b, next->values[next->at].expression, t.state, next, out);
        } else {
            p->span = spec->syntax->span;
            mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
            call->as.call.name = b->instance->subject->fun.name;
            call->as.call.args = k->arguments;
            *out = call;
        }
        return;
    }
    if (k->kind == K_JUMP) {
        mo_core_expr *out = t.out;
        if (!k->result && !atom(t.value)) {
            mo_core_expr e = let(b, lower_fresh(p), t.value);
            *out = e;
            out = &e->as.let_.body;
        }
        *out = jump(b, k, t.value);
        return;
    }
    if (k->kind == K_SELECT) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        e->as.if_.cond = t.value;
        e->as.if_.then_ = k->then_;
        e->as.if_.else_ = k->else_;
        *t.out = e;
        return;
    }
    if (k->kind == K_PATTERN) {
        if (k->diagnostic)
            p->span = k->row->syntax->span;
        mo_core_expr e = let(b, lower_fresh(p), t.value);
        *t.out = e;
        mo_core_expr *out = &e->as.let_.body;
        if (k->diagnostic) {
            mo_core_expr failure = lower_expr(p, MO_CORE_EXPR_FAIL);
            failure->as.fail = detail_field(b, "input ", type(b, row(b, k->row->operands[0])->type),
                                            lower_var(p, e->as.let_.name));
            /* Share failures only inside the saved input's lexical scope. */
            t.row = (uint32_t)(k->row - b->source->items);
            k->else_ = share_target(b, t, failure, &out);
        }
        push(b, (struct task){.kind = TASK_PATTERN,
                              .pattern = k->pattern,
                              .value = lower_var(p, e->as.let_.name),
                              .then_ = k->then_,
                              .else_ = k->else_,
                              .out = out});
        return;
    }
    if (k->kind == K_MATCH) {
        match(b, t, k);
        return;
    }
    if (k->kind == K_FOR) {
        t.continuation = k->next;
        loop(b, t, k->row, t.value);
        return;
    }
    if (k->kind == K_TEXT) {
        p->span = k->row->syntax->span;
        const struct mo_ability_implementation *proof =
            ability(b, k->row, MO_SOURCE_TEXT, k->row->operands[k->at]);
        mo_core_expr e = let(b, lower_fresh(p),
                             lower_to_text(p, proof, &b->instance->substitution, b->instance->owner,
                                           b->instance->depth, t.value));
        *t.out = e;
        k->arguments.items[2 * k->at + 1] = lower_var(p, e->as.let_.name);
        if (k->at + 1 < k->row->operand_count) {
            struct continuation *next = continuation(b, K_TEXT);
            *next = *k;
            next->at++;
            emit(b, k->row->operands[next->at], t.state, next, &e->as.let_.body);
        } else {
            p->span = k->row->syntax->span;
            mo_core_expr text = k->arguments.items[k->arguments.count - 1];
            for (size_t i = k->arguments.count - 1; i != 0; i--) {
                mo_core_expr_list args = arguments(b, 2);
                args.items[0] = k->arguments.items[i - 1];
                args.items[1] = text;
                text = lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT,
                                                        .as.text = MO_CORE_TEXT_OP_CONCAT},
                                  args);
            }
            finish(b, text, t.state, k->next, &e->as.let_.body);
        }
        return;
    }
    if (k->kind == K_OPTION || k->kind == K_TRY) {
        p->span = k->row->syntax->span;
        bool result = type(b, row(b, k->row->operands[0])->type)->tag == MO_CORE_TY_RESULT;
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
        e->as.case_.expr = t.value;
        e->as.case_.arms.count = 2;
        e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        struct mo_core_arm *none = &e->as.case_.arms.items[result ? 1 : 0];
        struct mo_core_arm *some = &e->as.case_.arms.items[result ? 0 : 1];
        none->ctor = lower_text(p, result ? "Error" : "None");
        some->ctor = lower_text(p, result ? "Ok" : "Some");
        some->names.count = 1;
        some->names.items = LOWER_NEW(p, mo_core_string, 1);
        some->names.items[0] = lower_fresh(p);
        *t.out = e;
        finish(b, lower_var(p, some->names.items[0]), t.state, k->next, &some->body);
        if (k->kind == K_OPTION)
            emit(b, k->row->operands[1], t.state, k->next, &none->body);
        else {
            const struct mo_checked_try *checked = k->row->try_;
            mo_core_ty enclosing = type(b, checked->result);
            mo_core_expr error = NULL;
            if (result) {
                none->names.count = 1;
                none->names.items = LOWER_NEW(p, mo_core_string, 1);
                none->names.items[0] = lower_fresh(p);
                error = lower_var(p, none->names.items[0]);
            }
            if (checked->wrapper.symbol != NULL || checked->wrapper.builtin != NULL) {
                MO_ASSERT(result);
                mo_core_expr wrapped = lower_expr(p, MO_CORE_EXPR_CON);
                wrapped->as.con.ty = enclosing->as.result.e;
                wrapped->as.con.ctor =
                    lower_text(p, checked->wrapper.symbol == NULL ? checked->wrapper.builtin
                                                                  : checked->wrapper.symbol->name);
                wrapped->as.con.args = arguments(b, 1);
                wrapped->as.con.args.items[0] = error;
                error = wrapped;
            }
            mo_core_expr failure = lower_expr(p, MO_CORE_EXPR_CON);
            failure->as.con.ty = enclosing;
            failure->as.con.ctor =
                lower_text(p, checked->failure.symbol == NULL ? checked->failure.builtin
                                                              : checked->failure.symbol->name);
            failure->as.con.args = arguments(b, result ? 1 : 0);
            if (result)
                failure->as.con.args.items[0] = error;
            MO_ASSERT(t.state.return_ != NULL);
            none->body = jump(b, t.state.return_, failure);
        }
        return;
    }
    if (k->kind == K_SEQUENCE) {
        mo_core_expr *out = t.out;
        if (!atom(t.value)) {
            mo_core_expr e = let(b, lower_fresh(p), t.value);
            *out = e;
            out = &e->as.let_.body;
        }
        struct continuation *next = k->next;
        if (k->at + 1 < k->row->operand_count) {
            next = continuation(b, K_SEQUENCE);
            *next = *k;
            next->at++;
        }
        emit(b, k->row->operands[k->at], t.state, next, out);
        return;
    }
    if (k->kind == K_BIND) {
        p->span = k->row->syntax->span;
        mo_core_expr value = t.value;
        if (k->row->assignment && k->row->operand_count == 2)
            value = field_update(b, k->row, value);
        mo_core_expr e =
            let(b, k->binding == NULL ? lower_fresh(p) : binding_name(b, k->binding), value);
        *t.out = e;
        if (k->binding == NULL && k->row->pattern != NULL) {
            mo_core_expr *success = LOWER_NEW(p, mo_core_expr, 1);
            mo_core_expr impossible = lower_expr(p, MO_CORE_EXPR_STOP);
            impossible->as.stop = lower_text(p, "unreachable binding failure");
            push(b, (struct task){.kind = TASK_PATTERN,
                                  .pattern = k->row->pattern,
                                  .value = lower_var(p, e->as.let_.name),
                                  .success = success,
                                  .else_ = impossible,
                                  .out = &e->as.let_.body});
            finish(b, lower_unit(p), t.state, k->next, success);
            return;
        }
        if (k->binding != NULL) {
            binder(b, e, 0, k->binding);
            if (k->binding->variable && !k->row->assignment) {
                struct variable *v = LOWER_NEW(p, struct variable, 1);
                *v = (struct variable){k->binding, t.state.variables};
                t.state.variables = v;
            }
        }
        finish(b, lower_unit(p), t.state, k->next, &e->as.let_.body);
        return;
    }
    MO_ASSERT(k->kind == K_ARGUMENTS);
    uint32_t skip = k->skip;
    bool direct = !k->diagnostic && atom(t.value);
    /* A mutable read must be snapped before a later inline assignment. Plain
     * names/literals cannot assign, so x += 1 needs only its actual SSA let. */
    if (direct && t.value->tag == MO_CORE_EXPR_VAR)
        for (struct variable *v = t.state.variables; v != NULL; v = v->next) {
            lower_work(p, 1);
            if (!lower_same(t.value->as.var, binding_name(b, v->binding)))
                continue;
            for (uint32_t i = k->at + 1 + skip; i < k->row->operand_count; i++) {
                lower_work(p, 1);
                const struct mo_checked_expression *later = row(b, k->row->operands[i]);
                if (later->operand_count != 0 || later->lambda != NULL)
                    direct = false;
            }
        }
    mo_core_expr *out = t.out;
    if (direct)
        k->arguments.items[k->at] = t.value;
    else {
        mo_core_expr e = let(b, k->name, t.value);
        *out = e;
        out = &e->as.let_.body;
        k->arguments.items[k->at] = lower_var(p, k->name);
    }
    if ((size_t)k->at + 1 < k->arguments.count) {
        struct continuation *next = continuation(b, K_ARGUMENTS);
        *next = *k;
        next->at++;
        next->name = lower_fresh(p);
        emit(b, k->row->operands[next->at + skip], t.state, next, out);
    } else {
        p->span = k->row->syntax->span;
        mo_core_expr value = application(b, k->row, k->arguments);
        if (k->diagnostic) {
            MO_ASSERT(k->row->operation == MO_T_EQ && k->arguments.count == 2);
            mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
            e->as.if_.cond = value;
            e->as.if_.then_ = k->then_;
            e->as.if_.else_ = lower_expr(p, MO_CORE_EXPR_FAIL);
            mo_core_ty operand = type(b, row(b, k->row->operands[0])->type);
            mo_core_expr left = detail_field(b, "left ", operand, k->arguments.items[0]);
            mo_core_expr right = detail_field(b, "right ", operand, k->arguments.items[1]);
            e->as.if_.else_->as.fail = text_concat(b, left, right);
            *out = e;
        } else
            finish(b, value, t.state, k->next, out);
    }
}

static void require_concrete_proofs(struct body *b, uint32_t count,
                                    const struct mo_checked_ability_use *uses) {
    for (uint32_t i = 0; i < count; i++) {
        lower_work(b->p, 1);
        const struct mo_ability_implementation *proof = uses[i].implementation;
        const struct substitution *substitution = &b->instance->substitution;
        lower_proof(b->p, &proof, &substitution);
    }
}

static void row_task(struct body *b, struct task t) {
    struct lowering *p = b->p;
    const struct mo_checked_expression *r = row(b, t.row);
    enum mo_syntax_kind tag = kind(r);
    require_concrete_proofs(b, r->ability_count, r->abilities);
    if (tag == MO_S_TEXT_INTERPOLATED) {
        MO_ASSERT(r->operand_count != 0 && r->syntax->count == 2 * r->operand_count + 1);
        struct continuation *k = continuation(b, K_TEXT);
        k->row = r;
        k->next = t.continuation;
        k->arguments = arguments(b, r->syntax->count);
        for (uint32_t i = 0; i < r->syntax->count; i += 2)
            k->arguments.items[i] = lower_text_part(p, r->syntax, i);
        emit(b, r->operands[0], t.state, k, t.out);
        return;
    }
    if (r->try_ != NULL) {
        require_concrete_proofs(b, r->try_->ability_count, r->try_->abilities);
        struct continuation *k = continuation(b, K_TRY);
        k->row = r;
        k->next = t.continuation;
        emit(b, r->operands[0], t.state, k, t.out);
        return;
    }
    if (r->or_kind == MO_CHECK_OR_OPTION) {
        mo_core_expr *out = t.out;
        struct continuation *done = branch_join(b, t, type(b, r->type), &out);
        struct continuation *k = continuation(b, K_OPTION);
        k->row = r;
        k->next = done;
        emit(b, r->operands[0], t.state, k, out);
        return;
    }
    if (tag == MO_S_SEQUENCE) {
        if (r->operand_count == 0)
            finish(b, lower_unit(p), t.state, t.continuation, t.out);
        else {
            struct continuation *k = t.continuation;
            if (r->operand_count > 1) {
                k = continuation(b, K_SEQUENCE);
                k->row = r;
                k->at = 1;
                k->next = t.continuation;
            }
            emit(b, r->operands[0], t.state, k, t.out);
        }
        return;
    }
    if (tag == MO_S_GROUP || tag == MO_S_EXPR_STMT || tag == MO_S_NAMED_ARG) {
        MO_ASSERT(r->operand_count == 1);
        emit(b, r->operands[0], t.state, t.continuation, t.out);
        return;
    }
    if (tag == MO_S_BIND || tag == MO_S_VAR || tag == MO_S_ADD_ASSIGN || tag == MO_S_SUB_ASSIGN) {
        struct continuation *k = continuation(b, K_BIND);
        k->binding = r->binding;
        k->row = r;
        k->next = t.continuation;
        emit(b, r->operands[r->operand_count - 1], t.state, k, t.out);
        return;
    }
    if (r->match != NULL) {
        struct continuation *k = continuation(b, K_MATCH);
        k->row = r;
        k->next = t.continuation;
        emit(b, r->match->scrutinee, t.state, k, t.out);
        return;
    }
    if (tag == MO_S_WHILE) {
        loop(b, t, r, NULL);
        return;
    }
    if (tag == MO_S_FOR) {
        struct continuation *k = continuation(b, K_FOR);
        k->row = r;
        k->next = t.continuation;
        emit(b, r->operands[0], t.state, k, t.out);
        return;
    }
    if (r->pattern != NULL) {
        mo_core_expr *out = t.out;
        struct continuation *done = branch_join(b, t, type(b, r->type), &out);
        struct continuation *k = continuation(b, K_PATTERN);
        k->pattern = r->pattern;
        emit(b, r->operands[0], t.state, k, out);
        k->then_ = done->kind == K_END ? lower_bool(p, true) : jump(b, done, lower_bool(p, true));
        k->else_ = done->kind == K_END ? lower_bool(p, false) : jump(b, done, lower_bool(p, false));
        return;
    }
    if (tag == MO_S_ASSERT) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        if (b->instance->fun.kind == MO_CORE_FUNCTION) {
            e->as.if_.else_ = lower_expr(p, MO_CORE_EXPR_STOP);
            e->as.if_.else_->as.stop = lower_text(p, "assertion failed");
        } else {
            e->as.if_.else_ = lower_expr(p, MO_CORE_EXPR_FAIL);
            e->as.if_.else_->as.fail = text_value(b, "assertion failed");
        }
        push(b, (struct task){.kind = TASK_CONDITION,
                              .row = r->operands[0],
                              .state = t.state,
                              .out = t.out,
                              .value = e,
                              .diagnostic = b->instance->fun.kind != MO_CORE_FUNCTION});
        finish(b, lower_unit(p), t.state, t.continuation, &e->as.if_.then_);
        return;
    }
    if (tag == MO_S_RETURN || tag == MO_S_BREAK || tag == MO_S_CONTINUE) {
        struct continuation *k = tag == MO_S_RETURN  ? t.state.return_
                                 : tag == MO_S_BREAK ? t.state.loop->break_
                                                     : t.state.loop->continue_;
        MO_ASSERT(k != NULL);
        struct continuation *exit = continuation(b, K_JUMP);
        *exit = *k;
        exit->row = r;
        k = exit;
        bool guarded =
            tag == MO_S_RETURN ? r->syntax->children[1] != NULL : r->syntax->children[0] != NULL;
        bool value = tag == MO_S_RETURN && r->syntax->children[0] != NULL;
        if (guarded) {
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
            /* LIFO: complete both branch trees before the condition uses them. */
            push(b, (struct task){.kind = TASK_CONDITION,
                                  .row = r->operands[0],
                                  .state = t.state,
                                  .out = t.out,
                                  .value = select});
            finish(b, lower_unit(p), t.state, t.continuation, &select->as.if_.else_);
            if (value)
                emit(b, r->operands[1], t.state, k, &select->as.if_.then_);
            else
                select->as.if_.then_ = jump(b, k, lower_unit(p));
        } else if (value)
            emit(b, r->operands[0], t.state, k, t.out);
        else
            *t.out = jump(b, k, lower_unit(p));
        return;
    }
    if (tag == MO_S_IF || tag == MO_S_LINE_IF || r->operation == MO_T_AND ||
        r->operation == MO_T_OR || r->operation == MO_T_IMPLIES) {
        mo_core_expr *out = t.out;
        struct continuation *k = branch_join(b, t, type(b, r->type), &out);
        if (tag == MO_S_IF || tag == MO_S_LINE_IF) {
            /* An iterative else-if chain, with all branch outputs arena-held. */
            size_t pairs = r->operand_count / 2;
            mo_core_expr *position = out;
            for (size_t i = 0; i < pairs; i++) {
                mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
                *position = select;
                push(b, (struct task){.kind = TASK_CONDITION,
                                      .row = r->operands[2 * i],
                                      .state = t.state,
                                      .out = position,
                                      .value = select});
                emit(b, r->operands[2 * i + 1], t.state, k, &select->as.if_.then_);
                position = &select->as.if_.else_;
            }
            if (r->operand_count % 2 != 0)
                emit(b, r->operands[r->operand_count - 1], t.state, k, position);
            else
                finish(b, lower_unit(p), t.state, k, position);
        } else {
            mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
            bool or_ = r->operation == MO_T_OR;
            push(b, (struct task){.kind = TASK_CONDITION,
                                  .row = r->operands[0],
                                  .state = t.state,
                                  .out = out,
                                  .value = select});
            emit(b, r->operands[1], t.state, k,
                 or_ ? &select->as.if_.else_ : &select->as.if_.then_);
            finish(b, lower_bool(p, r->operation != MO_T_AND), t.state, k,
                   or_ ? &select->as.if_.then_ : &select->as.if_.else_);
        }
        return;
    }
    if (tag == MO_S_STOP) {
        const struct mo_syntax *s = r->syntax->children[0];
        static const struct mo_source_type text = {
            .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_STOP);
        e->as.stop = lower_literal(p, s, type(b, &text), false)->as.lit->as.text;
        *t.out = e;
        return;
    }
    if (tag == MO_S_INT || tag == MO_S_DECIMAL || tag == MO_S_CHAR || tag == MO_S_TEXT ||
        tag == MO_S_TRUE || tag == MO_S_FALSE ||
        (r->native != NULL && strcmp(r->native, "Unit") == 0)) {
        finish(b, lower_literal(p, r->syntax, type(b, r->type), false), t.state, t.continuation,
               t.out);
        return;
    }
    if (r->operation == MO_T_MINUS && r->operand_count == 1 &&
        (kind(row(b, r->operands[0])) == MO_S_INT ||
         kind(row(b, r->operands[0])) == MO_S_DECIMAL)) {
        finish(b, lower_literal(p, row(b, r->operands[0])->syntax, type(b, r->type), true), t.state,
               t.continuation, t.out);
        return;
    }
    if (r->binding != NULL && r->operand_count == 0) {
        finish(b, lower_var(p, binding_name(b, r->binding)), t.state, t.continuation, t.out);
        return;
    }
    if (r->lambda != NULL) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_LAM);
        e->as.lam.params.count = r->lambda->parameter_count;
        e->as.lam.params.items = LOWER_NEW(p, struct mo_core_param, r->lambda->parameter_count);
        binder_slots(b, e, r->lambda->parameter_count);
        for (uint32_t i = 0; i < r->lambda->parameter_count; i++) {
            const struct mo_checked_binding *parameter = r->lambda->parameters[i];
            e->as.lam.params.items[i] = (struct mo_core_param){
                parameter == NULL ? lower_fresh(p) : binding_name(b, parameter),
                type(b, r->type)->as.fn.params.items[i]};
            if (parameter != NULL)
                binder(b, e, i, parameter);
        }
        ((struct lower_node *)e)->closure =
            lower_concat(p, b->instance->meta.name, ".fn.", lower_number(p, r->lambda->number));
        mo_core_expr *body = &e->as.lam.body;
        for (uint32_t i = 0; i < r->lambda->capture_count; i++) {
            const struct mo_checked_binding *capture = r->lambda->captures[i];
            mo_core_string name = binding_name(b, capture);
            mo_core_expr snapshot = let(b, name, lower_var(p, name));
            binder(b, snapshot, 0, capture);
            *body = snapshot;
            body = &snapshot->as.let_.body;
        }
        if (returns(b, r->lambda->body)) {
            struct continuation *ret = continuation(b, K_JUMP);
            ret->result = true;
            ret->name = lower_fresh(p);
            mo_core_expr return_join = join(b, ret, type(b, r->type)->as.fn.ret);
            return_join->as.join.body = lower_var(p, return_join->as.join.params.items[0].name);
            *body = return_join;
            emit(b, r->lambda->body, (struct state){.return_ = ret}, continuation(b, K_END),
                 &return_join->as.join.rest);
        } else
            emit(b, r->lambda->body, (struct state){0}, continuation(b, K_END), body);
        finish(b, e, t.state, t.continuation, t.out);
        return;
    }
    if (r->operand_count == 0 && r->symbol != NULL && r->symbol->declaration->kind == MO_S_CONST) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_GLOBAL);
        e->as.global = lower_global_name(p, r->symbol);
        finish(b, e, t.state, t.continuation, t.out);
        return;
    }
    if (r->operand_count == 0 && ((r->symbol != NULL && r->symbol->declaration->kind == MO_S_FN) ||
                                  (r->native != NULL && strchr(r->native, '.') != NULL &&
                                   r->type->kind == MO_SOURCE_FUNCTION))) {
        mo_core_ty signature = type(b, r->type);
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_LAM);
        e->as.lam.params.count = signature->as.fn.params.count;
        e->as.lam.params.items = LOWER_NEW(p, struct mo_core_param, e->as.lam.params.count);
        mo_core_expr_list args = arguments(b, e->as.lam.params.count);
        for (size_t i = 0; i < e->as.lam.params.count; i++) {
            mo_core_string name = lower_fresh(p);
            e->as.lam.params.items[i] =
                (struct mo_core_param){name, signature->as.fn.params.items[i]};
            args.items[i] = lower_var(p, name);
        }
        e->as.lam.body = application(b, r, args);
        ((struct lower_node *)e)->closure =
            lower_concat(p, b->instance->meta.name, ".ref.", lower_number(p, t.row));
        finish(b, e, t.state, t.continuation, t.out);
        return;
    }
    uint32_t skip = direct_function(b, r) ? 1 : 0;
    /* An ordinary namespace call retains its native function-value callee;
     * a native receiver dot call has only the receiver and explicit arguments. */
    if (r->native != NULL && strchr(r->native, '.') != NULL && r->receiver == UINT32_MAX &&
        r->operand_count != 0 && row(b, r->operands[0])->type->kind == MO_SOURCE_FUNCTION)
        skip = 1;
    if (r->operand_count == skip) {
        finish(b, application(b, r, arguments(b, 0)), t.state, t.continuation, t.out);
        return;
    }
    struct continuation *k = continuation(b, K_ARGUMENTS);
    k->row = r;
    k->skip = skip;
    k->next = t.continuation;
    k->name = lower_fresh(p);
    k->arguments = arguments(b, r->operand_count - skip);
    emit(b, r->operands[skip], t.state, k, t.out);
}

static int anchor_order(const void *a, const void *b) {
    const struct anchor_value *x = a, *y = b;
    return (x->expression > y->expression) - (x->expression < y->expression);
}

void lower_body(struct lowering *p, struct instance *instance, const struct mo_checked_body *source,
                uint32_t parameter_count, const struct mo_checked_binding *const *parameters) {
    struct body b = {.p = p, .instance = instance, .source = source};
    b.names = LOWER_NEW(p, mo_core_string, b.source->binding_count);
    for (uint32_t i = 0; i < b.source->binding_count; i++)
        b.names[i] = lower_fresh(p);
    instance->fun.params.count = parameter_count;
    instance->fun.params.items = LOWER_NEW(p, struct mo_core_param, instance->fun.params.count);
    instance->meta.body.params.count = instance->fun.params.count;
    instance->meta.body.params.items =
        LOWER_NEW(p, struct mo_core_name, instance->fun.params.count);
    for (size_t i = 0; i < instance->fun.params.count; i++) {
        const struct mo_checked_binding *parameter = parameters[i];
        instance->fun.params.items[i] =
            (struct mo_core_param){binding_name(&b, parameter), type(&b, parameter->type)};
        instance->meta.body.params.items[i] =
            (struct mo_core_name){true, lower_text(p, parameter->name)};
    }
    if (instance->fun.kind == MO_CORE_ANCHOR) {
        const struct mo_checked_spec *spec = instance->spec;
        MO_ASSERT(spec->value_count != 0);
        struct continuation *k = continuation(&b, K_ANCHOR);
        struct anchor_value *values = LOWER_NEW(p, struct anchor_value, spec->value_count);
        for (uint32_t i = 0; i < spec->value_count; i++) {
            values[i] = (struct anchor_value){spec->values[i], i};
            for (uint32_t n = spec->value_count; n != 0; n /= 2)
                lower_work(p, 1);
        }
        qsort(values, spec->value_count, sizeof *values, anchor_order);
        k->values = values;
        k->arguments = arguments(&b, spec->value_count);
        emit(&b, values[0].expression, (struct state){0}, k, &instance->fun.body);
    } else if (instance->fun.kind == MO_CORE_PROMISE) {
        MO_ASSERT(instance->spec->condition_count != 0);
        emit(&b, instance->spec->conditions[0].expression, (struct state){0},
             continuation(&b, K_PROMISE), &instance->fun.body);
    } else if (instance->fun.kind == MO_CORE_EXAMPLE) {
        p->span = instance->spec->syntax->span;
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        e->as.if_.then_ = lower_unit(p);
        e->as.if_.else_ = lower_expr(p, MO_CORE_EXPR_FAIL);
        e->as.if_.else_->as.fail = text_value(&b, "example failed");
        push(&b, (struct task){.kind = TASK_CONDITION,
                               .row = b.source->root,
                               .out = &instance->fun.body,
                               .value = e,
                               .diagnostic = true});
    } else if (returns(&b, b.source->root)) {
        struct continuation *ret = continuation(&b, K_JUMP);
        ret->result = true;
        ret->name = lower_fresh(p);
        mo_core_expr e = join(&b, ret, instance->fun.ret);
        e->as.join.body = lower_var(p, e->as.join.params.items[0].name);
        instance->fun.body = e;
        emit(&b, b.source->root, (struct state){.return_ = ret}, continuation(&b, K_END),
             &e->as.join.rest);
    } else
        emit(&b, b.source->root, (struct state){0}, continuation(&b, K_END), &instance->fun.body);
    while (b.tasks != NULL) {
        lower_work(p, 1);
        struct task t = *b.tasks;
        b.tasks = t.next;
        if (t.kind == TASK_ROW || t.kind == TASK_CONDITION)
            p->span = row(&b, t.row)->syntax->span;
        else if (t.kind == TASK_FINISH && t.value != NULL)
            p->span = ((struct lower_node *)t.value)->span.value;
        if (t.kind == TASK_ROW)
            row_task(&b, t);
        else if (t.kind == TASK_FINISH)
            finish_task(&b, t);
        else if (t.kind == TASK_CONDITION) {
            if (t.value != NULL) {
                t.then_ = t.value->as.if_.then_;
                t.else_ = t.value->as.if_.else_;
            }
            const struct mo_checked_expression *r = row(&b, t.row);
            while (kind(r) == MO_S_GROUP || kind(r) == MO_S_EXPR_STMT) {
                lower_work(p, 1);
                MO_ASSERT(r->operand_count == 1 && r->operands[0] < t.row);
                t.row = r->operands[0];
                r = row(&b, t.row);
            }
            p->span = r->syntax->span;
            require_concrete_proofs(&b, r->ability_count, r->abilities);
            if (r->operation == MO_T_OR || r->operation == MO_T_IMPLIES)
                t.then_ = share_target(&b, t, t.then_, &t.out);
            if (r->operation == MO_T_AND || r->operation == MO_T_OR || r->pattern != NULL)
                t.else_ = share_target(&b, t, t.else_, &t.out);
            if (r->operation == MO_T_AND ||
                (r->operation == MO_T_OR && r->or_kind != MO_CHECK_OR_OPTION) ||
                r->operation == MO_T_IMPLIES) {
                mo_core_expr select = lower_expr(p, MO_CORE_EXPR_IF);
                bool or_ = r->operation == MO_T_OR;
                select->as.if_.then_ = or_ ? t.then_ : NULL;
                select->as.if_.else_ = or_                            ? NULL
                                       : r->operation == MO_T_IMPLIES ? t.then_
                                                                      : t.else_;
                push(&b, (struct task){.kind = TASK_CONDITION,
                                       .row = r->operands[0],
                                       .state = t.state,
                                       .out = t.out,
                                       .value = select,
                                       .diagnostic = t.diagnostic && r->operation == MO_T_AND});
                push(&b, (struct task){.kind = TASK_CONDITION,
                                       .row = r->operands[1],
                                       .state = t.state,
                                       .out = or_ ? &select->as.if_.else_ : &select->as.if_.then_,
                                       .then_ = t.then_,
                                       .else_ = t.else_,
                                       .diagnostic = t.diagnostic});
            } else if (t.diagnostic && r->operation == MO_T_EQ) {
                struct continuation *k = continuation(&b, K_ARGUMENTS);
                k->row = r;
                k->arguments = arguments(&b, 2);
                k->name = lower_fresh(p);
                k->then_ = t.then_;
                k->diagnostic = true;
                emit(&b, r->operands[0], t.state, k, t.out);
            } else {
                struct continuation *k =
                    continuation(&b, r->pattern == NULL ? K_SELECT : K_PATTERN);
                k->then_ = t.then_;
                k->else_ = t.else_;
                k->pattern = r->pattern;
                k->row = r;
                k->diagnostic = t.diagnostic && r->pattern != NULL;
                emit(&b, r->pattern == NULL ? t.row : r->operands[0], t.state, k, t.out);
            }
        } else if (t.kind == TASK_PATTERN)
            pattern_task(&b, t);
        else if (t.kind == TASK_ARM) {
            t.value = *t.success;
            arm_task(&b, t);
        } else {
            MO_ASSERT(t.kind == TASK_COPY && *t.success != NULL);
            *t.out = *t.success;
        }
    }
}

void lower_function(struct lowering *p, struct instance *instance) {
    const struct mo_checked_function *source = instance->source;
    p->span = source->signature->function->declaration->span;
    instance->fun.ret = lower_type(p, source->signature->type, &instance->substitution)->as.fn.ret;
    lower_body(p, instance, &source->body, source->parameter_count, source->parameters);
    lower_subject_specs(p, instance);
}
