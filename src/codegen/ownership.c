#include "codegen/internal.h"

#include <string.h>

#include "base/codes.h"

MO_MUST_USE bool codegen_abilities_valid(struct compiler *c) {
    MO_ASSERT(c->types.finished);
    uint8_t *flags = mo_bytecode_abilities(&c->arena, c->types.items, c->types.count);
    if (flags == NULL)
        return codegen_problem(c, MO_CODE_CODEGEN_MEMORY,
                               "No scratch memory for runtime ability inference.");
    for (uint32_t i = 0; i < c->types.count; i++) {
        const struct mo_type *t = &c->types.items[i];
        if ((t->kind == MO_TYPE_MAP || t->kind == MO_TYPE_SET) &&
            (flags[t->parameters[0]] & MO_ABILITY_HASH) == 0)
            return codegen_problem(
                c, MO_CODE_CODEGEN_REPRESENTATION,
                "Map keys and Set elements without Hash cannot come from lowering.");
    }
    for (uint32_t i = 0; i < c->constraints.count; i++) {
        const struct constraint *item = &((struct constraint *)c->constraints.items)[i];
        if ((flags[item->type] & item->ability) == 0) {
            c->current = item->function;
            c->span = item->span;
            c->declaration = item->declaration;
            return codegen_problem(
                c, MO_CODE_CODEGEN_REPRESENTATION,
                item->ability == MO_ABILITY_EQUAL
                    ? "An operation on a type without Equal cannot come from lowering."
                    : "An operation on a type without Hash cannot come from lowering.");
        }
    }
    return true;
}
MO_MUST_USE bool codegen_operation_owned(const struct node *n, uint32_t slot) {
    if (n->operation.opcode == MO_OP_SIM)
        return false;
    return n->operation.literal != MO_CODEGEN_LITERAL_NONE || n->operation.params[slot] == 'o';
}
static void seed(struct compiler *c, uint32_t id) {
    if (id != MO_NO_INDEX) {
        struct ownership *o = &((struct ownership *)c->ownership.items)[id];
        if (codegen_reference(c, o->type))
            o->owned = true;
    }
}
static MO_MUST_USE bool depends(struct compiler *c, uint32_t from, uint32_t to) {
    if (from == MO_NO_INDEX || to == MO_NO_INDEX || from == to)
        return true;
    struct ownership *o = c->ownership.items;
    if (!codegen_reference(c, o[from].type) || !codegen_reference(c, o[to].type))
        return true;
    uint32_t id = c->edges.count;
    struct edge *e =
        codegen_append(c, &c->edges, 1, MO_LIMIT_CODEGEN_OWNERSHIP_EDGES, MO_CODE_CODEGEN_MEMORY);
    if (e == NULL)
        return false;
    *e = (struct edge){to, o[from].edges};
    o[from].edges = id;
    return true;
}
/* Dependencies propagate ownership once per slot. Tail calls add the reverse dependency:
 * an argument owned by the disappearing frame makes the callee's parameter owned too. */
static MO_MUST_USE bool borrow_walk(struct compiler *c, struct function *f, struct node *n,
                                    bool tail, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    uint32_t own = codegen_flow_id(f, n->reg);
    if (n->core->tag == MO_CORE_EXPR_CASE) {
        if (!borrow_walk(c, f, n->children[0], false, depth + 1))
            return false;
        for (uint32_t i = 1; i < n->count; i++) {
            struct node *arm = n->children[i], *body = arm->children[0];
            for (uint32_t j = 0; j < arm->bind_count; j++) {
                uint32_t field = codegen_flow_id(f, arm->binds[j]);
                seed(c, field);
            }
            if (!borrow_walk(c, f, body, tail, depth + 1) ||
                !depends(c, own, codegen_flow_id(f, body->reg)) ||
                !depends(c, codegen_flow_id(f, body->reg), own))
                return false;
        }
    } else {
        for (uint32_t i = 0; i < n->count; i++) {
            bool child_tail = tail && ((n->core->tag == MO_CORE_EXPR_LET && i == 1) ||
                                       (n->core->tag == MO_CORE_EXPR_IF && i != 0) ||
                                       n->core->tag == MO_CORE_EXPR_JOIN);
            if (!borrow_walk(c, f, n->children[i], child_tail, depth + 1))
                return false;
        }
    }
    if (n->core->tag == MO_CORE_EXPR_LET) {
        uint32_t bound = codegen_flow_id(f, n->children[0]->reg),
                 binding = codegen_flow_id(f, n->binds[0]);
        if (!depends(c, binding, bound) || !depends(c, bound, binding))
            return false;
    } else if (n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_JOIN) {
        for (uint32_t i = n->core->tag == MO_CORE_EXPR_JOIN ? 0u : 1u; i < n->count; i++)
            if (!depends(c, own, codegen_flow_id(f, n->children[i]->reg)) ||
                !depends(c, codegen_flow_id(f, n->children[i]->reg), own))
                return false;
        if (n->core->tag == MO_CORE_EXPR_JOIN)
            for (uint32_t i = 0; i < n->bind_count; i++)
                seed(c, codegen_flow_id(f, n->binds[i]));
    } else if (n->core->tag == MO_CORE_EXPR_JUMP) {
        for (uint32_t i = 0; i < n->count; i++)
            seed(c, codegen_flow_id(f, n->children[i]->reg));
    } else if (n->core->tag == MO_CORE_EXPR_CALL) {
        struct function *callee = &c->bodies[n->index];
        for (uint32_t i = 0; i < n->count; i++) {
            uint32_t arg = codegen_flow_id(f, n->children[i]->reg),
                     param = codegen_flow_id(callee, i);
            if (!depends(c, param, arg) || (tail && !depends(c, arg, param)))
                return false;
        }
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_PRIM) {
        for (uint32_t i = 0; i < n->count; i++)
            if (codegen_operation_owned(n, i))
                seed(c, codegen_flow_id(f, n->children[i]->reg));
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_LAM) {
        for (uint32_t i = 0; i < n->bind_count; i++)
            seed(c, codegen_flow_id(f, n->binds[i]));
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_APP || n->core->tag == MO_CORE_EXPR_CON ||
               n->core->tag == MO_CORE_EXPR_TUPLE) {
        for (uint32_t i = 0; i < n->count; i++)
            seed(c, codegen_flow_id(f, n->children[i]->reg));
        seed(c, own);
    } else if (n->core->tag == MO_CORE_EXPR_PROJ) {
        seed(c, own);
    }
    if (tail && n->core->tag != MO_CORE_EXPR_STOP)
        seed(c, own);
    return true;
}
MO_MUST_USE bool codegen_borrow_infer(struct compiler *c) {
    for (uint32_t i = 0; i < c->function_count; i++) {
        struct function *f = &c->bodies[i];
        if (c->funs[i].kind == MO_FUNCTION_INIT)
            continue;
        if (c->funs[i].kind != MO_FUNCTION_FUNCTION)
            for (uint32_t j = 0; j < c->funs[i].parameter_count; j++)
                seed(c, codegen_flow_id(f, j));
        for (uint32_t j = 0; j < f->captures.count; j++)
            seed(c, codegen_flow_id(f, ((struct capture *)f->captures.items)[j].reg));
        if (!borrow_walk(c, f, f->body, true, 0))
            return false;
    }
    uint32_t *queue = codegen_allocate(c, c->ownership.count, sizeof *queue);
    if (queue == NULL)
        return false;
    struct ownership *slots = c->ownership.items;
    const struct edge *edges = c->edges.items;
    uint32_t end = 0;
    for (uint32_t i = 0; i < c->ownership.count; i++)
        if (slots[i].owned)
            queue[end++] = i;
    for (uint32_t at = 0; at < end; at++) {
        for (uint32_t e = slots[queue[at]].edges; e != MO_NO_INDEX; e = edges[e].next) {
            uint32_t to = edges[e].to;
            if (!slots[to].owned) {
                slots[to].owned = true;
                queue[end++] = to;
            }
        }
    }
    for (uint32_t i = 0; i < c->function_count; i++)
        for (uint32_t j = 0; j < c->funs[i].parameter_count; j++)
            if (codegen_reference(c, c->parameters[i][j].type) &&
                !slots[codegen_flow_id(&c->bodies[i], j)].owned)
                c->parameters[i][j].ownership = MO_PARAMETER_BORROWED;
    return true;
}

/* Backward liveness is independent of ownership inference. Sorted sets contain only
 * references. Each lexical join converges its body (including nested joins) locally;
 * enclosing joins retry that body only when their own free-variable set grows. */
MO_MUST_USE bool codegen_live_charge(struct compiler *c, struct function *f, uint64_t count) {
    if (count > MO_LIMIT_CODEGEN_LIVE_WORK - f->live_work)
        return codegen_problem(c, MO_CODE_CODEGEN_LIVE_WORK,
                               "Reference liveness exceeds MO_LIMIT_CODEGEN_LIVE_WORK.");
    f->live_work += count;
    return true;
}
static MO_MUST_USE bool live_reserve(struct compiler *c, struct function *f, struct live_set *live,
                                     uint32_t count) {
    MO_ASSERT(count <= f->reference_count);
    if (count <= live->capacity)
        return true;
    uint32_t capacity = live->capacity == 0 ? 4 : live->capacity;
    if (capacity > f->reference_count)
        capacity = f->reference_count;
    while (capacity < count)
        capacity = capacity > f->reference_count / 2 ? f->reference_count : capacity * 2;
    if (capacity > MO_LIMIT_CODEGEN_LIVE_CELLS - f->live_cells)
        return codegen_problem(c, MO_CODE_CODEGEN_LIVE_WORK,
                               "Reference liveness exceeds MO_LIMIT_CODEGEN_LIVE_CELLS.");
    if (!codegen_live_charge(c, f, live->count))
        return false;
    uint32_t *items = codegen_allocate(c, capacity, sizeof *items);
    if (items == NULL)
        return false;
    if (live->count != 0)
        memcpy(items, live->items, (size_t)live->count * sizeof *items);
    f->live_cells += capacity; /* Include old blocks retained by the arena. */
    live->items = items;
    live->capacity = capacity;
    return true;
}
static MO_MUST_USE bool live_use(struct compiler *c, struct function *f, struct live_set *live,
                                 uint32_t r) {
    if (r == MO_NO_INDEX || !codegen_reference(c, ((struct reg *)f->regs.items)[r].type))
        return true;
    MO_ASSERT(r < f->live_count);
    if (!codegen_live_charge(c, f, 33)) /* A uint32_t binary search visits at most 32 cells. */
        return false;
    uint32_t at = codegen_live_position(live, r);
    if (at < live->count && live->items[at] == r)
        return true;
    if (!codegen_live_charge(c, f, 1u + live->count - at) ||
        !live_reserve(c, f, live, live->count + 1))
        return false;
    memmove(live->items + at + 1, live->items + at,
            (size_t)(live->count - at) * sizeof *live->items);
    live->items[at] = r;
    live->count++;
    return true;
}
static MO_MUST_USE bool live_kill(struct compiler *c, struct function *f, struct live_set *live,
                                  uint32_t r) {
    if (!codegen_live_charge(c, f, 33))
        return false;
    uint32_t at = codegen_live_position(live, r);
    if (at == live->count || live->items[at] != r)
        return true;
    if (!codegen_live_charge(c, f, live->count - at))
        return false;
    live->count--;
    memmove(live->items + at, live->items + at + 1,
            (size_t)(live->count - at) * sizeof *live->items);
    return true;
}
MO_MUST_USE bool codegen_operand_owned(struct compiler *c, const struct node *n, uint32_t i) {
    if (n->core->tag == MO_CORE_EXPR_CALL)
        return c->funs[n->index].parameters[i].ownership == MO_PARAMETER_OWNED;
    if (n->core->tag == MO_CORE_EXPR_PRIM)
        return codegen_operation_owned(n, i);
    return true;
}
static MO_MUST_USE bool live_copy(struct compiler *c, struct function *f, struct live_set *to,
                                  const struct live_set *from) {
    MO_ASSERT(to != from);
    if (!codegen_live_charge(c, f, 1u + from->count) || !live_reserve(c, f, to, from->count))
        return false;
    if (from->count != 0)
        memcpy(to->items, from->items, (size_t)from->count * sizeof *to->items);
    to->count = from->count;
    return true;
}
static MO_MUST_USE bool live_clear(struct compiler *c, struct function *f, struct live_set *live) {
    if (!codegen_live_charge(c, f, 1))
        return false;
    live->count = 0;
    return true;
}
static MO_MUST_USE bool live_union(struct compiler *c, struct function *f, struct live_set *to,
                                   const struct live_set *from, bool *changed) {
    MO_ASSERT(to != from);
    if (!codegen_live_charge(c, f, 1u + UINT64_C(2) * (to->count + from->count)))
        return false;
    uint32_t a = 0, added = 0;
    for (uint32_t b = 0; b < from->count; b++) {
        while (a < to->count && to->items[a] < from->items[b])
            a++;
        if (a == to->count || to->items[a] != from->items[b])
            added++;
    }
    if (added == 0)
        return true;
    uint32_t count = to->count + added;
    if (!live_reserve(c, f, to, count))
        return false;
    a = to->count;
    uint32_t b = from->count, end = count;
    while (b != 0) {
        if (a != 0 && to->items[a - 1] > from->items[b - 1])
            to->items[--end] = to->items[--a];
        else {
            if (a != 0 && to->items[a - 1] == from->items[b - 1])
                a--;
            to->items[--end] = from->items[--b];
        }
    }
    MO_ASSERT(end == a);
    to->count = count;
    *changed = true;
    return true;
}
static MO_MUST_USE bool live_walk(struct compiler *c, struct function *f, struct node *n,
                                  struct live_set *live, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    c->span = n->span;
    /* Cover direct membership searches as well as the node/operand visits. */
    if (!codegen_live_charge(c, f,
                             33u * (2u + UINT64_C(3) * n->count + UINT64_C(2) * n->bind_count)))
        return false;
    if (n->last == NULL) {
        uint32_t count = n->core->tag == MO_CORE_EXPR_LAM ? n->bind_count : n->count;
        n->last = codegen_allocate(c, count == 0 ? 1 : count, sizeof *n->last);
        if (n->last == NULL)
            return false;
    }
    if (n->core->tag == MO_CORE_EXPR_VAR)
        return true; /* The parent instruction already marked this lexical register. */
    if (n->core->tag == MO_CORE_EXPR_LET) {
        if (!live_walk(c, f, n->children[1], live, depth + 1) ||
            !live_kill(c, f, live, n->binds[0]))
            return false;
        uint32_t r = n->children[0]->reg;
        n->last[0] = !codegen_live_has(live, r);
        return live_use(c, f, live, r) && live_walk(c, f, n->children[0], live, depth + 1);
    }
    if (!live_kill(c, f, live, n->reg))
        return false;
    bool branch = n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_CASE;
    bool join = n->core->tag == MO_CORE_EXPR_JOIN;
    if (branch || join) {
        if (!live_copy(c, f, &n->after, live))
            return false;
        if (branch) {
            bool choice = n->core->tag == MO_CORE_EXPR_CASE;
            if (!live_clear(c, f, live))
                return false;
            for (uint32_t i = 1; i < n->count; i++) {
                struct node *arm = n->children[i], *body = choice ? arm->children[0] : arm;
                if (!live_copy(c, f, &n->work, &n->after))
                    return false;
                n->last[i] = !codegen_live_has(&n->work, body->reg);
                if (!live_use(c, f, &n->work, body->reg) ||
                    !live_walk(c, f, body, &n->work, depth + 1))
                    return false;
                if (choice) {
                    if (!codegen_live_charge(c, f, arm->bind_count))
                        return false;
                    for (uint32_t j = 0; j < arm->bind_count; j++)
                        if (!live_kill(c, f, &n->work, arm->binds[j]))
                            return false;
                }
                bool ignored = false;
                if (!live_union(c, f, live, &n->work, &ignored))
                    return false;
            }
            return live_use(c, f, live, n->children[0]->reg) &&
                   live_walk(c, f, n->children[0], live, depth + 1);
        }
        struct join *target = &((struct join *)f->joins.items)[n->index];
        if (target->used == NULL) {
            target->used = codegen_allocate(c, n->bind_count, sizeof *target->used);
            if (target->used == NULL)
                return false;
        }
        bool changed;
        do {
            if (!live_copy(c, f, &n->work, &n->after))
                return false;
            n->last[0] = !codegen_live_has(&n->work, n->children[0]->reg);
            if (!live_use(c, f, &n->work, n->children[0]->reg) ||
                !live_walk(c, f, n->children[0], &n->work, depth + 1))
                return false;
            for (uint32_t i = 0; i < n->bind_count; i++) {
                target->used[i] = codegen_live_has(&n->work, n->binds[i]);
                if (!live_kill(c, f, &n->work, n->binds[i]))
                    return false;
            }
            changed = false;
            if (!live_union(c, f, &target->live, &n->work, &changed))
                return false;
        } while (changed);
        if (!live_copy(c, f, live, &n->after))
            return false;
        n->last[1] = !codegen_live_has(live, n->children[1]->reg);
        return live_use(c, f, live, n->children[1]->reg) &&
               live_walk(c, f, n->children[1], live, depth + 1);
    }
    if (n->core->tag == MO_CORE_EXPR_JUMP) {
        const struct join *target = &((struct join *)f->joins.items)[n->index];
        if (!live_copy(c, f, live, &target->live))
            return false;
    } else if ((n->core->tag == MO_CORE_EXPR_STOP || n->core->tag == MO_CORE_EXPR_FAIL) &&
               !live_clear(c, f, live))
        return false;
    if (n->core->tag == MO_CORE_EXPR_LAM) {
        for (uint32_t i = n->bind_count; i-- > 0;) {
            uint32_t r = n->binds[i];
            n->last[i] = !codegen_live_has(live, r);
            if (!live_use(c, f, live, r))
                return false;
        }
        return true;
    }
    /* Borrowed operands sharing an owned operand need their original register throughout
     * the instruction. Repeated owned operands duplicate all but the final occurrence. */
    for (uint32_t i = 0; i < n->count; i++)
        if (!codegen_operand_owned(c, n, i) && !live_use(c, f, live, n->children[i]->reg))
            return false;
    for (uint32_t i = n->count; i-- > 0;) {
        uint32_t r = n->children[i]->reg;
        n->last[i] = !codegen_live_has(live, r);
        if (!live_use(c, f, live, r))
            return false;
    }
    for (uint32_t i = n->count; i-- > 0;)
        if (!live_walk(c, f, n->children[i], live, depth + 1))
            return false;
    return true;
}
MO_MUST_USE bool codegen_live_infer(struct compiler *c, struct function *f) {
    f->live_count = f->regs.count;
    if (!codegen_live_charge(c, f, UINT64_C(2) * f->live_count))
        return false;
    const struct reg *regs = f->regs.items;
    for (uint32_t i = 0; i < f->live_count; i++)
        if (codegen_reference(c, regs[i].type))
            f->reference_count++;
    f->references = codegen_allocate(c, f->reference_count, sizeof *f->references);
    if (f->references == NULL)
        return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < f->live_count; i++)
        if (codegen_reference(c, regs[i].type))
            f->references[count++] = i;
    MO_ASSERT(count == f->reference_count);
    struct live_set live = {0};
    return live_use(c, f, &live, f->body->reg) && live_walk(c, f, f->body, &live, 0);
}
