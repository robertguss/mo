#include "codegen/internal.h"

#include <string.h>

#include "base/codes.h"

MO_MUST_USE struct mo_bytecode_span codegen_span(struct compiler *c, struct mo_core_span s) {
    if (s.some && s.value.end > MO_LIMIT_FILE_SIZE) {
        if (!codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                             "Source span exceeds the bytecode MO_LIMIT_FILE_SIZE bound."))
            return (struct mo_bytecode_span){.file = MO_NO_INDEX};
    }
    return s.some ? (struct mo_bytecode_span){0, s.value.file, s.value.start, s.value.end}
                  : (struct mo_bytecode_span){.file = MO_NO_INDEX};
}
MO_MUST_USE bool codegen_debug_add(struct compiler *c) {
    if (c->debug_entries == MO_LIMIT_DEBUG_ENTRIES)
        return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                               "Debug information exceeds MO_LIMIT_DEBUG_ENTRIES.");
    c->debug_entries++;
    return true;
}
/* Journal only reference-state changes. Branch restoration and normalization visit this log,
 * never the full register table, and nested checkpoints reuse its storage. */
static MO_MUST_USE bool state(struct compiler *c, struct function *f, uint32_t r,
                              enum ref_state value) {
    struct reg *reg = &((struct reg *)f->regs.items)[r];
    if (!codegen_reference(c, reg->type) || reg->state == value)
        return true;
    struct change *change =
        codegen_append(c, &f->changes, 1, 2 * MO_LIMIT_FUNCTION_CODE_WORDS, MO_CODE_CODEGEN_MEMORY);
    if (change == NULL)
        return false;
    *change = (struct change){r, reg->state};
    reg->state = value;
    return true;
}
static const char *const host_params[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) params,
#include "base/hostops.def"
};
static MO_MUST_USE bool consume(struct compiler *c, struct function *f, uint32_t r,
                                enum mo_operand_access access, const uint32_t *fixed,
                                enum mo_opcode op, uint32_t slot) {
    bool owned = access == MO_ACCESS_CONSUME;
    if (access == MO_ACCESS_MOVE)
        owned = ((struct reg *)f->regs.items)[r].state == REF_OWNED;
    else if (access == MO_ACCESS_CALL_PARAMETER) {
        uint32_t callee = fixed[op == MO_OP_CALL ? 1 : 0];
        owned = c->funs[callee].parameters[slot].ownership == MO_PARAMETER_OWNED;
    } else if (access == MO_ACCESS_PRIM_PARAMETER)
        owned = mo_codegen_primitive((enum mo_primitive_id)fixed[1]).params[slot] == 'o';
    else if (access == MO_ACCESS_HOST_PARAMETER)
        owned = host_params[fixed[1]][slot] == 'o';
    return !owned || state(c, f, r, REF_MOVED);
}
static MO_MUST_USE bool transfer(struct compiler *c, struct function *f, enum mo_opcode op,
                                 const uint32_t *fixed, const uint32_t *variable, uint32_t count) {
    const struct mo_opcode_info *info = mo_opcode_find((uint32_t)op);
    MO_ASSERT(info != NULL);
    enum ref_state result = op == MO_OP_CONST || op == MO_OP_GLOBAL ? REF_BORROWED : REF_OWNED;
    if (op == MO_OP_MOVE)
        result = ((struct reg *)f->regs.items)[fixed[1]].state;
    else if (op == MO_OP_CLEAR)
        result = REF_UNDEFINED;
    for (uint32_t i = 0; i < info->fixed_count; i++)
        if (info->fixed[i].kind == MO_OPERAND_REGISTER &&
            !consume(c, f, fixed[i], info->fixed[i].access, fixed, op, i))
            return false;
    if (info->variable.kind == MO_OPERAND_REGISTER)
        for (uint32_t i = 0; i < count; i++)
            if (!consume(c, f, variable[i], info->variable.access, fixed, op, i))
                return false;
    for (uint32_t i = 0; i < info->fixed_count; i++)
        if (info->fixed[i].kind == MO_OPERAND_REGISTER &&
            info->fixed[i].access == MO_ACCESS_WRITE && !state(c, f, fixed[i], result))
            return false;
    return true;
}
MO_MUST_USE uint32_t codegen_emit(struct compiler *c, struct function *f,
                                  struct mo_core_span source, enum mo_opcode op,
                                  const uint32_t *fixed, uint32_t n, const uint32_t *variable,
                                  uint32_t count) {
    c->span = source;
    if (f->instructions == MO_LIMIT_FUNCTION_INSTRUCTIONS) {
        if (!codegen_problem(c, MO_CODE_FUNCTION_TOO_LONG,
                             "Function exceeds MO_LIMIT_FUNCTION_INSTRUCTIONS."))
            return MO_NO_INDEX;
    }
    uint32_t offset = f->code.count;
    uint32_t *words = codegen_append(c, &f->code, 1 + n + count, MO_LIMIT_FUNCTION_CODE_WORDS,
                                     MO_CODE_FUNCTION_TOO_LONG);
    if (words == NULL)
        return MO_NO_INDEX;
    f->instructions++;
    words[0] = (uint32_t)op | (count << MO_OPCODE_VARIABLE_SHIFT);
    if (n != 0)
        memcpy(words + 1, fixed, (size_t)n * sizeof *fixed);
    if (count != 0)
        memcpy(words + 1 + n, variable, (size_t)count * sizeof *variable);
    if (c->core->metadata.source) {
        struct mo_bytecode_span current = codegen_span(c, source);
        if (c->error != 0)
            return MO_NO_INDEX;
        struct mo_bytecode_span *previous =
            f->spans.count == 0 ? NULL
                                : &((struct mo_bytecode_span *)f->spans.items)[f->spans.count - 1];
        if (previous == NULL || previous->file != current.file ||
            previous->start != current.start || previous->end != current.end) {
            struct mo_bytecode_span *s =
                codegen_append(c, &f->spans, 1, MO_LIMIT_DEBUG_ENTRIES, MO_CODE_BYTECODE_TOO_BIG);
            if (s == NULL || !codegen_debug_add(c))
                return MO_NO_INDEX;
            current.code_offset = offset;
            *s = current;
        }
    }
    if (!transfer(c, f, op, fixed, variable, count))
        return MO_NO_INDEX;
    return offset;
}
MO_MUST_USE bool codegen_named(struct compiler *c, struct function *f, uint32_t reg, uint32_t start,
                               uint32_t end) {
    uint32_t name_id = ((struct reg *)f->regs.items)[reg].name;
    if (name_id == MO_NO_INDEX || start == end)
        return true;
    struct mo_bytecode_register_name *n =
        codegen_append(c, &f->names, 1, MO_LIMIT_DEBUG_ENTRIES, MO_CODE_BYTECODE_TOO_BIG);
    if (n == NULL || !codegen_debug_add(c))
        return false;
    *n = (struct mo_bytecode_register_name){reg, name_id, start, end};
    return true;
}
/* Track only live owners, not every register at every return. */
void codegen_owner_add(struct function *f, uint32_t r) {
    struct reg *regs = f->regs.items;
    MO_ASSERT(!regs[r].owned);
    regs[r].owned = true;
    regs[r].previous = MO_NO_INDEX;
    regs[r].next = f->owned;
    if (f->owned != MO_NO_INDEX)
        regs[f->owned].previous = r;
    f->owned = r;
}
static void owner_remove(struct function *f, uint32_t r) {
    struct reg *regs = f->regs.items;
    if (!regs[r].owned)
        return;
    if (regs[r].previous == MO_NO_INDEX)
        f->owned = regs[r].next;
    else
        regs[regs[r].previous].next = regs[r].next;
    if (regs[r].next != MO_NO_INDEX)
        regs[regs[r].next].previous = regs[r].previous;
    regs[r].owned = false;
}
static MO_MUST_USE bool drop(struct compiler *c, struct function *f, uint32_t r,
                             struct mo_core_span s) {
    if (r == MO_NO_INDEX || !((struct reg *)f->regs.items)[r].owned)
        return true;
    if (codegen_emit(c, f, s, MO_OP_DROP, &r, 1, NULL, 0) == MO_NO_INDEX)
        return false;
    owner_remove(f, r);
    return true;
}
static MO_MUST_USE bool cleanup(struct compiler *c, struct function *f, struct mo_core_span s) {
    while (f->owned != MO_NO_INDEX)
        if (!drop(c, f, f->owned, s))
            return false;
    return true;
}
static void restore(struct function *f, uint32_t checkpoint) {
    struct reg *regs = f->regs.items;
    MO_ASSERT(checkpoint <= f->changes.count);
    while (f->changes.count != checkpoint) {
        struct change change = ((struct change *)f->changes.items)[--f->changes.count];
        struct reg *r = &regs[change.reg];
        if (r->owned && change.previous != REF_OWNED)
            owner_remove(f, change.reg);
        else if (!r->owned && change.previous == REF_OWNED)
            codegen_owner_add(f, change.reg);
        r->state = change.previous;
    }
}
/* Every live edge uses the same undefined state for dead references, including lexical
 * registers consumed within just one arm. Restoration publishes that state without code. */
static MO_MUST_USE bool forget_dead(struct compiler *c, struct function *f,
                                    const struct live_set *live, uint32_t keep,
                                    struct mo_core_span s, bool instructions) {
    if (!codegen_live_charge(c, f, (uint64_t)f->reference_count + live->count))
        return false;
    struct reg *regs = f->regs.items;
    uint32_t at = 0;
    for (uint32_t j = 0; j < f->reference_count; j++) {
        uint32_t i = f->references[j];
        while (at < live->count && live->items[at] < i)
            at++;
        if (i == keep || (at < live->count && live->items[at] == i) || regs[i].protected ||
            regs[i].state == REF_UNDEFINED)
            continue;
        if (instructions) {
            if (!drop(c, f, i, s) ||
                codegen_emit(c, f, s, MO_OP_CLEAR, &i, 1, NULL, 0) == MO_NO_INDEX)
                return false;
        } else {
            owner_remove(f, i);
            if (!state(c, f, i, REF_UNDEFINED))
                return false;
        }
    }
    return true;
}
static MO_MUST_USE bool normalize(struct compiler *c, struct function *f, uint32_t checkpoint,
                                  uint32_t keep, struct mo_core_span s,
                                  const struct live_set *live) {
    MO_ASSERT(checkpoint <= f->changes.count && f->mark != UINT32_MAX);
    if (live != NULL && !forget_dead(c, f, live, keep, s, true))
        return false;
    uint32_t mark = ++f->mark, end = f->changes.count;
    struct reg *regs = f->regs.items;
    for (uint32_t i = checkpoint; i < end; i++) {
        struct change change = ((struct change *)f->changes.items)[i];
        struct reg *r = &regs[change.reg];
        if (r->mark != mark) {
            r->mark = mark;
            r->goal = change.previous;
        }
    }
    for (uint32_t i = end; i != checkpoint;) {
        uint32_t id = ((struct change *)f->changes.items)[--i].reg;
        struct reg *r = &regs[id];
        if (r->mark != mark)
            continue;
        r->mark = 0;
        if (live != NULL && !codegen_live_charge(c, f, 33))
            return false;
        if (id == keep || r->protected || r->state == r->goal ||
            (live != NULL && id < f->live_count && !codegen_live_has(live, id)))
            continue;
        /* Only path-local slots change on a live edge; lexical owners outside it survive.
         * Jump argument copies are protected until simultaneous parameter transfer. */
        MO_ASSERT(r->goal == REF_UNDEFINED);
        if (r->state == REF_OWNED) {
            MO_ASSERT(r->owned);
            if (!drop(c, f, id, s))
                return false;
        }
        if (codegen_emit(c, f, s, MO_OP_CLEAR, &id, 1, NULL, 0) == MO_NO_INDEX)
            return false;
    }
    return true;
}
static MO_MUST_USE bool merged(struct compiler *c, struct function *f, uint32_t r) {
    if (!codegen_reference(c, ((struct reg *)f->regs.items)[r].type))
        return true;
    bool owned = ((struct ownership *)c->ownership.items)[codegen_flow_id(f, r)].owned;
    if (!state(c, f, r, owned ? REF_OWNED : REF_BORROWED))
        return false;
    if (owned)
        codegen_owner_add(f, r);
    return true;
}
static MO_MUST_USE bool copy(struct compiler *c, struct function *f, uint32_t to, uint32_t from,
                             bool owned, struct mo_core_span s, bool last) {
    bool ref = codegen_reference(c, ((struct reg *)f->regs.items)[from].type);
    bool move = ref && owned && last && ((struct reg *)f->regs.items)[from].owned;
    enum mo_opcode op = ref && owned && !move ? MO_OP_DUP : MO_OP_MOVE;
    MO_ASSERT(!ref || owned || !((struct reg *)f->regs.items)[from].owned);
    const uint32_t args[] = {to, from};
    if (codegen_emit(c, f, s, op, args, 2, NULL, 0) == MO_NO_INDEX)
        return false;
    if (move)
        owner_remove(f, from);
    if (ref && owned)
        codegen_owner_add(f, to);
    return true;
}
static MO_MUST_USE uint32_t argument(struct compiler *c, struct function *f, uint32_t r, bool owned,
                                     struct mo_core_span s, bool last) {
    uint32_t ty = ((struct reg *)f->regs.items)[r].type;
    if (!codegen_reference(c, ty) || !owned || (last && ((struct reg *)f->regs.items)[r].owned))
        return r;
    uint32_t tmp = codegen_register_add(c, f, ty, MO_NO_INDEX);
    if (tmp == MO_NO_INDEX || !copy(c, f, tmp, r, true, s, false))
        return MO_NO_INDEX;
    return tmp;
}
/* Immediate slots have a stable value state at every edge. Reference slots start undefined
 * and are cleared back to that state when their path-local value dies. */
MO_MUST_USE bool codegen_initialize(struct compiler *c, struct function *f, uint32_t reg,
                                    unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    uint32_t ty = ((struct reg *)f->regs.items)[reg].type;
    const struct mo_type *t = &c->types.items[ty];
    if (codegen_reference(c, ty))
        return true;
    if (t->kind == MO_TYPE_RANGE) {
        uint32_t field = codegen_register_add(c, f, c->range_int, MO_NO_INDEX);
        if (field == MO_NO_INDEX || !codegen_initialize(c, f, field, depth + 1))
            return false;
        const uint32_t args[] = {reg, field, field};
        return codegen_emit(c, f, c->declaration, MO_OP_MAKE_RANGE, args, 3, NULL, 0) !=
               MO_NO_INDEX;
    }
    if (t->kind == MO_TYPE_CHOICE) {
        const struct mo_type_case *first = &t->cases[0];
        uint32_t field = MO_NO_INDEX;
        uint32_t count = first->field_count;
        if (count != 0) {
            uint32_t field_type = first->fields[0].type;
            mo_core_string key = {(const char *)&field_type, sizeof field_type};
            struct mo_codegen_index *old = mo_codegen_find(f->zeros, key);
            if (old != NULL) {
                field = old->value;
            } else {
                field = codegen_register_add(c, f, field_type, MO_NO_INDEX);
                if (field == MO_NO_INDEX || !codegen_initialize(c, f, field, depth + 1) ||
                    codegen_index_add(c, &f->zeros, key, field, true) == NULL)
                    return false;
            }
        }
        const uint32_t fixed[] = {reg, ty, 0};
        return codegen_emit(c, f, c->declaration, MO_OP_MAKE_CASE, fixed, 3, &field, count) !=
               MO_NO_INDEX;
    }
    struct mo_bytecode_constant k = {.type = ty};
    uint32_t id = codegen_constant(c, k);
    const uint32_t fixed[] = {reg, id};
    return id != MO_NO_INDEX &&
           codegen_emit(c, f, c->declaration, MO_OP_CONST, fixed, 2, NULL, 0) != MO_NO_INDEX;
}
/* Expansion temporaries are private owners; the trusted row specifies which ones are consumed. */
static MO_MUST_USE bool library(struct compiler *c, struct function *f, enum mo_primitive_id row,
                                uint32_t signature, uint32_t result, const uint32_t *args,
                                uint32_t count, struct mo_core_span source) {
    struct mo_codegen_operation op = mo_codegen_primitive(row);
    MO_ASSERT(count == c->types.items[signature].parameter_count);
    for (uint32_t i = 0; i < count; i++)
        if (op.params[i] == 'o' &&
            codegen_reference(c, ((struct reg *)f->regs.items)[args[i]].type)) {
            MO_ASSERT(((struct reg *)f->regs.items)[args[i]].owned);
            owner_remove(f, args[i]);
        }
    const uint32_t fixed[] = {result, (uint32_t)row, signature};
    if (codegen_emit(c, f, source, MO_OP_PRIM, fixed, 3, args, count) == MO_NO_INDEX)
        return false;
    if (codegen_reference(c, ((struct reg *)f->regs.items)[result].type))
        codegen_owner_add(f, result);
    return true;
}
static MO_MUST_USE bool range_text(struct compiler *c, struct function *f, struct node *n,
                                   uint32_t value) {
    const uint32_t start[] = {n->binds[0], value}, end[] = {n->binds[1], value},
                   separator[] = {n->binds[4], n->index}, prefix[] = {n->binds[2], n->binds[4]},
                   suffix[] = {n->binds[5], n->binds[3]};
    return codegen_emit(c, f, n->span, MO_OP_RANGE_START, start, 2, NULL, 0) != MO_NO_INDEX &&
           codegen_emit(c, f, n->span, MO_OP_RANGE_END, end, 2, NULL, 0) != MO_NO_INDEX &&
           codegen_emit(c, f, n->span, MO_OP_CONST, separator, 2, NULL, 0) != MO_NO_INDEX &&
           library(c, f, MO_PRIM_INT_TO_TEXT, n->cases[0], n->binds[2], n->binds, 1, n->span) &&
           library(c, f, MO_PRIM_INT_TO_TEXT, n->cases[0], n->binds[3], n->binds + 1, 1, n->span) &&
           library(c, f, MO_PRIM_TEXT_CONCAT, n->cases[1], n->binds[5], prefix, 2, n->span) &&
           library(c, f, MO_PRIM_TEXT_CONCAT, n->cases[1], n->reg, suffix, 2, n->span) &&
           drop(c, f, n->binds[3], n->span);
}
static MO_MUST_USE bool uncons(struct compiler *c, struct function *f, struct node *n,
                               uint32_t list) {
    uint32_t first = n->binds[0], head = n->binds[1], rest = n->binds[2], one = n->binds[3],
             pair = n->binds[4];
    if (!library(c, f, MO_PRIM_LIST_FIRST, n->cases[0], first, &list, 1, n->span))
        return false;
    /* Both arms consume this argument. Keep its moved state across normalization,
     * then publish that common state after restoring the branch checkpoint. */
    MO_ASSERT(((struct reg *)f->regs.items)[list].owned &&
              !((struct reg *)f->regs.items)[list].protected);
    ((struct reg *)f->regs.items)[list].protected = true;
    uint32_t checkpoint = f->changes.count;
    uint32_t branch = codegen_emit(c, f, n->span, MO_OP_SWITCH, &first, 1, (uint32_t[]){0, 0}, 2);
    if (branch == MO_NO_INDEX)
        return false;
    ((uint32_t *)f->code.items)[branch + 2] = f->code.count;
    const uint32_t none[] = {n->reg, n->type, 0};
    if (!drop(c, f, list, n->span) ||
        codegen_emit(c, f, n->span, MO_OP_MAKE_CASE, none, 3, NULL, 0) == MO_NO_INDEX)
        return false;
    codegen_owner_add(f, n->reg);
    uint32_t skip = codegen_emit(c, f, n->span, MO_OP_JUMP, (uint32_t[]){0}, 1, NULL, 0);
    if (skip == MO_NO_INDEX)
        return false;
    restore(f, checkpoint);
    ((uint32_t *)f->code.items)[branch + 3] = f->code.count;
    const uint32_t field[] = {head, first, 1, 0}, count[] = {one, n->index};
    if (codegen_emit(c, f, n->span, MO_OP_CASE_FIELD, field, 4, NULL, 0) == MO_NO_INDEX ||
        codegen_emit(c, f, n->span, MO_OP_CONST, count, 2, NULL, 0) == MO_NO_INDEX)
        return false;
    if (codegen_reference(c, ((struct reg *)f->regs.items)[head].type))
        codegen_owner_add(f, head);
    if (!library(c, f, MO_PRIM_LIST_DROP, n->cases[1], rest, (uint32_t[]){list, one}, 2, n->span))
        return false;
    const uint32_t construct[] = {pair, c->layouts[((struct reg *)f->regs.items)[pair].type]},
                   some[] = {n->reg, n->type, 1}, fields[] = {head, rest};
    owner_remove(f, head);
    owner_remove(f, rest);
    if (codegen_emit(c, f, n->span, MO_OP_CONSTRUCT, construct, 2, fields, 2) == MO_NO_INDEX)
        return false;
    /* The private tuple moves straight into Some; it never joins the live-owner set. */
    if (codegen_emit(c, f, n->span, MO_OP_MAKE_CASE, some, 3, &pair, 1) == MO_NO_INDEX)
        return false;
    codegen_owner_add(f, n->reg);
    if (!normalize(c, f, checkpoint, n->reg, n->span, NULL))
        return false;
    ((uint32_t *)f->code.items)[skip + 1] = f->code.count;
    restore(f, checkpoint);
    ((struct reg *)f->regs.items)[list].protected = false;
    owner_remove(f, list);
    return state(c, f, list, REF_MOVED) && merged(c, f, n->reg) && drop(c, f, first, n->span);
}
/* Jumps only target enclosing joins. Visit the rest before the continuation body so
 * jumps in unreachable bodies cannot make an enclosing continuation reachable.
 * Return whether evaluation falls through to the next expression. Tail returns
 * need no special handling: their enclosing expression has no next operand. */
MO_MUST_USE bool codegen_reachable_walk(struct function *f, const struct node *n, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    if (n->core->tag == MO_CORE_EXPR_JOIN) {
        bool live = codegen_reachable_walk(f, n->children[1], depth + 1);
        if (((struct join *)f->joins.items)[n->index].reachable)
            live |= codegen_reachable_walk(f, n->children[0], depth + 1);
        return live;
    }
    if (n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_CASE) {
        if (!codegen_reachable_walk(f, n->children[0], depth + 1))
            return false;
        bool live = false;
        /* CASE arm wrappers copy the body's tag, but hold only the body as a child. */
        for (uint32_t i = 1; i < n->count; i++) {
            const struct node *arm = n->children[i];
            live |= codegen_reachable_walk(
                f, n->core->tag == MO_CORE_EXPR_CASE ? arm->children[0] : arm, depth + 1);
        }
        return live;
    }
    for (uint32_t i = 0; i < n->count; i++)
        if (!codegen_reachable_walk(f, n->children[i], depth + 1))
            return false;
    if (n->core->tag == MO_CORE_EXPR_JUMP) {
        ((struct join *)f->joins.items)[n->index].reachable = true;
        return false;
    }
    return n->core->tag != MO_CORE_EXPR_FAIL && n->core->tag != MO_CORE_EXPR_STOP;
}
/* false through *live means a terminal path, not a compiler refusal. */
MO_MUST_USE bool codegen_generate(struct compiler *c, struct function *f, struct node *n, bool tail,
                                  unsigned depth, bool *live) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    c->span = n->span;
    *live = true;
    switch (n->core->tag) {
    case MO_CORE_EXPR_LIT: {
        const uint32_t args[] = {n->reg, n->index};
        if (codegen_emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX)
            return false;
        break;
    }
    case MO_CORE_EXPR_VAR:
        break;
    case MO_CORE_EXPR_GLOBAL: {
        const uint32_t args[] = {n->reg, n->index};
        if (codegen_emit(c, f, n->span, MO_OP_GLOBAL, args, 2, NULL, 0) == MO_NO_INDEX)
            return false;
        break;
    }
    case MO_CORE_EXPR_LET: {
        if (!codegen_generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        uint32_t start = f->code.count;
        bool owned =
            ((struct ownership *)c->ownership.items)[codegen_flow_id(f, n->binds[0])].owned;
        if (!copy(c, f, n->binds[0], n->children[0]->reg, owned, n->span, n->last[0]) ||
            (n->children[0]->temporary && !drop(c, f, n->children[0]->reg, n->span)) ||
            !codegen_generate(c, f, n->children[1], tail, depth + 1, live))
            return false;
        if (*live && n->binds[0] != n->reg && !drop(c, f, n->binds[0], n->span))
            return false;
        return codegen_named(c, f, n->binds[0], start, f->code.count);
    }
    case MO_CORE_EXPR_LAM: {
        if (n->bind_count == 0) {
            struct mo_bytecode_constant k = {.type = n->type};
            k.value.function = n->index;
            uint32_t id = codegen_constant(c, k);
            const uint32_t args[] = {n->reg, id};
            if (id == MO_NO_INDEX ||
                codegen_emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX)
                return false;
        } else {
            uint32_t *regs = codegen_allocate(c, n->bind_count, sizeof *regs);
            if (regs == NULL)
                return false;
            for (uint32_t i = 0; i < n->bind_count; i++) {
                regs[i] = argument(c, f, n->binds[i], true, n->span, n->last[i]);
                if (regs[i] == MO_NO_INDEX)
                    return false;
                owner_remove(f, regs[i]);
            }
            const uint32_t args[] = {n->reg, n->index};
            if (codegen_emit(c, f, n->span, MO_OP_MAKE_CLOSURE, args, 2, regs, n->bind_count) ==
                MO_NO_INDEX)
                return false;
            codegen_owner_add(f, n->reg);
        }
        break;
    }
    case MO_CORE_EXPR_APP: {
        uint32_t *regs = codegen_allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            if (!codegen_generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            regs[i] = argument(c, f, n->children[i]->reg, true, n->span, n->last[i]);
            if (regs[i] == MO_NO_INDEX)
                return false;
        }
        for (uint32_t i = 0; i < n->count; i++)
            owner_remove(f, regs[i]);
        if (tail && !cleanup(c, f, n->span))
            return false;
        const uint32_t args[] = {n->reg, regs[0]};
        if (codegen_emit(c, f, n->span, tail ? MO_OP_TAIL_CALL_CLOSURE : MO_OP_CALL_CLOSURE,
                         tail ? args + 1 : args, tail ? 1 : 2, regs + 1,
                         n->count - 1) == MO_NO_INDEX)
            return false;
        *live = !tail;
        if (*live) {
            if (codegen_reference(c, n->type))
                codegen_owner_add(f, n->reg);
            for (uint32_t i = 0; i < n->count; i++)
                if (n->children[i]->temporary && !drop(c, f, n->children[i]->reg, n->span))
                    return false;
        }
        break;
    }
    case MO_CORE_EXPR_CALL:
    case MO_CORE_EXPR_CON:
    case MO_CORE_EXPR_TUPLE: {
        uint32_t *regs = codegen_allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            if (!codegen_generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            regs[i] = argument(c, f, n->children[i]->reg, codegen_operand_owned(c, n, i), n->span,
                               n->last[i]);
            if (regs[i] == MO_NO_INDEX)
                return false;
        }
        uint32_t args[] = {n->reg, n->index, 0};
        enum mo_opcode op = MO_OP_CALL;
        uint32_t count = 2;
        if (n->core->tag == MO_CORE_EXPR_CON) {
            if (c->types.items[n->type].kind == MO_TYPE_RECORD) {
                op = MO_OP_CONSTRUCT;
                args[1] = c->layouts[n->type];
            } else {
                op = MO_OP_MAKE_CASE;
                args[1] = n->type;
                args[2] = n->index;
                count = 3;
            }
        } else if (n->core->tag == MO_CORE_EXPR_TUPLE) {
            op = MO_OP_CONSTRUCT;
            args[1] = c->layouts[n->type];
        } else if (tail) {
            op = MO_OP_TAIL_CALL;
            args[0] = n->index;
            count = 1;
            *live = false;
        }
        /* Reserve consumed operands while dropping the rest of a tail frame. */
        for (uint32_t i = 0; i < n->count; i++)
            if (codegen_operand_owned(c, n, i))
                owner_remove(f, regs[i]);
        if (op == MO_OP_TAIL_CALL && !cleanup(c, f, n->span))
            return false;
        if (codegen_emit(c, f, n->span, op, args, count, regs, n->count) == MO_NO_INDEX)
            return false;
        if (*live) {
            if (codegen_reference(c, n->type))
                codegen_owner_add(f, n->reg);
            for (uint32_t i = 0; i < n->count; i++)
                if (n->children[i]->temporary && !drop(c, f, n->children[i]->reg, n->span))
                    return false;
        }
        break;
    }
    case MO_CORE_EXPR_IF:
    case MO_CORE_EXPR_CASE: {
        if (!codegen_generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        bool choice = n->core->tag == MO_CORE_EXPR_CASE;
        if (choice && c->types.items[n->children[0]->type].kind == MO_TYPE_RECORD) {
            MO_ASSERT(n->count == 2);
            struct node *arm = n->children[1], *body = arm->children[0];
            for (uint32_t j = 0; j < arm->bind_count; j++) {
                const uint32_t field[] = {arm->binds[j], n->children[0]->reg, j};
                if (codegen_emit(c, f, n->span, MO_OP_FIELD, field, 3, NULL, 0) == MO_NO_INDEX)
                    return false;
                if (codegen_reference(c, ((struct reg *)f->regs.items)[arm->binds[j]].type))
                    codegen_owner_add(f, arm->binds[j]);
            }
            uint32_t start = f->code.count;
            if (!codegen_generate(c, f, body, tail, depth + 1, live))
                return false;
            if (*live) {
                bool owned =
                    ((struct ownership *)c->ownership.items)[codegen_flow_id(f, n->reg)].owned;
                if (!copy(c, f, n->reg, body->reg, owned, n->span, n->last[1]) ||
                    (body->temporary && !drop(c, f, body->reg, n->span)) ||
                    (n->children[0]->temporary && !drop(c, f, n->children[0]->reg, n->span)))
                    return false;
                for (uint32_t j = 0; j < arm->bind_count; j++)
                    if (!drop(c, f, arm->binds[j], n->span))
                        return false;
            }
            for (uint32_t j = 0; j < arm->bind_count; j++)
                if (!codegen_named(c, f, arm->binds[j], start, f->code.count))
                    return false;
            break;
        }
        uint32_t arms = n->count - 1;
        uint32_t *targets = codegen_allocate(c, arms, sizeof *targets),
                 *jumps = codegen_allocate(c, arms, sizeof *jumps);
        if (targets == NULL || jumps == NULL)
            return false;
        memset(targets, 0, (size_t)arms * sizeof *targets);
        for (uint32_t i = 0; i < arms; i++)
            jumps[i] = MO_NO_INDEX;
        uint32_t fixed[] = {n->children[0]->reg, 0, 0};
        uint32_t at = codegen_emit(c, f, n->span, choice ? MO_OP_SWITCH : MO_OP_BRANCH, fixed,
                                   choice ? 1 : 3, choice ? targets : NULL, choice ? arms : 0);
        if (at == MO_NO_INDEX)
            return false;
        uint32_t checkpoint = f->changes.count;
        bool any = false;
        for (uint32_t i = 0; i < arms; i++) {
            restore(f, checkpoint);
            uint32_t case_id = choice ? n->cases[i] : i;
            targets[case_id] = f->code.count;
            uint32_t start = f->code.count;
            struct node *arm = n->children[i + 1], *body = choice ? arm->children[0] : arm;
            if (choice) {
                for (uint32_t j = 0; j < arm->bind_count; j++) {
                    const uint32_t field[] = {arm->binds[j], n->children[0]->reg, case_id, j};
                    if (codegen_emit(c, f, n->span, MO_OP_CASE_FIELD, field, 4, NULL, 0) ==
                        MO_NO_INDEX)
                        return false;
                    if (codegen_reference(c, ((struct reg *)f->regs.items)[arm->binds[j]].type))
                        codegen_owner_add(f, arm->binds[j]);
                }
            }
            if (!codegen_generate(c, f, body, tail, depth + 1, live))
                return false;
            if (choice) {
                for (uint32_t j = 0; j < arm->bind_count; j++)
                    if (!codegen_named(c, f, arm->binds[j], start, f->code.count))
                        return false;
            }
            if (*live) {
                any = true;
                const uint32_t jump[] = {0};
                bool owned =
                    n->reg != MO_NO_INDEX &&
                    ((struct ownership *)c->ownership.items)[codegen_flow_id(f, n->reg)].owned;
                if (!copy(c, f, n->reg, body->reg, owned, n->span, n->last[i + 1]) ||
                    !normalize(c, f, checkpoint, n->reg, n->span, &n->after))
                    return false;
                jumps[i] = codegen_emit(c, f, n->span, MO_OP_JUMP, jump, 1, NULL, 0);
                if (jumps[i] == MO_NO_INDEX)
                    return false;
            }
        }
        uint32_t *words = f->code.items;
        memcpy(words + at + 2, targets, (size_t)arms * sizeof *targets);
        for (uint32_t i = 0; i < arms; i++)
            if (jumps[i] != MO_NO_INDEX)
                words[jumps[i] + 1] = f->code.count;
        restore(f, checkpoint);
        if (any && (!forget_dead(c, f, &n->after, n->reg, n->span, false) || !merged(c, f, n->reg)))
            return false;
        *live = any;
        break;
    }
    case MO_CORE_EXPR_PROJ: {
        if (!codegen_generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        const uint32_t args[] = {n->reg, n->children[0]->reg, n->index};
        if (codegen_emit(c, f, n->span, MO_OP_FIELD, args, 3, NULL, 0) == MO_NO_INDEX)
            return false;
        if (codegen_reference(c, n->type))
            codegen_owner_add(f, n->reg);
        if (n->children[0]->temporary && !drop(c, f, n->children[0]->reg, n->span))
            return false;
        break;
    }
    case MO_CORE_EXPR_PRIM: {
        uint32_t *regs = codegen_allocate(c, n->count, sizeof *regs);
        if (regs == NULL)
            return false;
        /* Evaluate every operand before calling or inserting, including later literal pairs.
         * A duplicate map key must not suppress the effects of a later Core argument. */
        for (uint32_t i = 0; i < n->count; i++) {
            if (!codegen_generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            regs[i] = argument(c, f, n->children[i]->reg, codegen_operation_owned(n, i), n->span,
                               n->last[i]);
            if (regs[i] == MO_NO_INDEX)
                return false;
        }
        if (n->operation.expansion == MO_CODEGEN_EXPAND_RANGE) {
            if (!range_text(c, f, n, regs[0]))
                return false;
        } else if (n->operation.expansion == MO_CODEGEN_EXPAND_UNCONS) {
            if (!uncons(c, f, n, regs[0]))
                return false;
        } else if (n->operation.literal != MO_CODEGEN_LITERAL_NONE) {
            bool map = n->operation.literal == MO_CODEGEN_LITERAL_MAP;
            uint32_t width = map ? 2 : 1;
            uint32_t builder = n->bind_count == 0 ? n->reg : n->binds[0];
            const uint32_t empty[] = {builder, map ? MO_PRIM_MAP_EMPTY : MO_PRIM_LIST_EMPTY,
                                      n->index};
            if (codegen_emit(c, f, n->span, MO_OP_PRIM, empty, 3, NULL, 0) == MO_NO_INDEX)
                return false;
            codegen_owner_add(f, builder);
            for (uint32_t i = 0; i < n->bind_count; i++) {
                uint32_t dest = i + 1 == n->bind_count ? n->reg : n->binds[i + 1];
                uint32_t args[3] = {builder, regs[i * width], map ? regs[i * width + 1] : 0};
                owner_remove(f, builder);
                for (uint32_t j = 0; j < width; j++)
                    owner_remove(f, regs[i * width + j]);
                const uint32_t insert[] = {dest, n->operation.row, n->signature};
                if (codegen_emit(c, f, n->span, MO_OP_PRIM, insert, 3, args, width + 1) ==
                    MO_NO_INDEX)
                    return false;
                codegen_owner_add(f, dest);
                builder = dest;
            }
        } else {
            for (uint32_t i = 0; i < n->count; i++)
                if (codegen_operation_owned(n, i))
                    owner_remove(f, regs[i]);
            if (n->operation.reverse) {
                MO_ASSERT(n->count == 2);
                uint32_t swap = regs[0];
                regs[0] = regs[1];
                regs[1] = swap;
            }
            if (n->operation.opcode == MO_OP_SIM) {
                const struct mo_core_sim_op *sim = &n->core->as.prim.prim.as.sim;
                const uint32_t fixed[] = {n->reg, (uint32_t)sim->tag,
                                          sim->tag == MO_SIM_START || sim->tag == MO_SIM_SEND
                                              ? codegen_selector(sim->descriptor)
                                              : 0,
                                          sim->tag == MO_SIM_SEND ? codegen_selector(sim->tool)
                                                                  : 0};
                if (codegen_emit(c, f, n->span, MO_OP_SIM, fixed, 4, regs, n->count) == MO_NO_INDEX)
                    return false;
            } else if (n->operation.opcode == MO_OP_PRIM || n->operation.opcode == MO_OP_HOST) {
                const uint32_t fixed[] = {n->operation.compare ? n->binds[0] : n->reg,
                                          n->operation.row, n->signature};
                if (codegen_emit(c, f, n->span, n->operation.opcode, fixed,
                                 n->operation.opcode == MO_OP_HOST ? 2 : 3, regs,
                                 n->count) == MO_NO_INDEX)
                    return false;
            } else if (n->operation.opcode == MO_OP_EQUAL) {
                const uint32_t fixed[] = {n->reg, c->types.items[n->signature].parameters[0],
                                          regs[0], regs[1]};
                if (codegen_emit(c, f, n->span, MO_OP_EQUAL, fixed, 4, NULL, 0) == MO_NO_INDEX)
                    return false;
            } else {
                uint32_t fixed[4] = {n->reg, 0, 0, 0};
                MO_ASSERT(n->count <= 2);
                memcpy(fixed + 1, regs, (size_t)n->count * sizeof *regs);
                if (codegen_emit(c, f, n->span, n->operation.opcode, fixed, 1 + n->count, NULL,
                                 0) == MO_NO_INDEX)
                    return false;
            }
            if (n->operation.compare) {
                const uint32_t equal[] = {n->binds[1], c->types.items[n->signature].result, 1};
                const uint32_t test[] = {n->reg, n->binds[0], n->binds[1]};
                enum mo_core_cmp_op cmp = n->core->as.prim.prim.as.cmp.op;
                enum mo_opcode op = cmp == MO_CORE_CMP_OP_LE || cmp == MO_CORE_CMP_OP_GE
                                        ? MO_OP_LE_CHOICE
                                        : MO_OP_LT_CHOICE;
                if (codegen_emit(c, f, n->span, MO_OP_MAKE_CASE, equal, 3, NULL, 0) ==
                        MO_NO_INDEX ||
                    codegen_emit(c, f, n->span, op, test, 3, NULL, 0) == MO_NO_INDEX)
                    return false;
            }
            if (codegen_reference(c, n->type))
                codegen_owner_add(f, n->reg);
        }
        for (uint32_t i = 0; i < n->count; i++)
            if (n->children[i]->temporary && !drop(c, f, n->children[i]->reg, n->span))
                return false;
        break;
    }
    case MO_CORE_EXPR_FAIL:
        if (!codegen_generate(c, f, n->children[0], false, depth + 1, live) || !*live)
            return c->error == 0;
        if (codegen_emit(c, f, n->span, MO_OP_FAIL, &n->children[0]->reg, 1, NULL, 0) ==
            MO_NO_INDEX)
            return false;
        *live = false;
        break;
    case MO_CORE_EXPR_STOP: {
        const uint32_t args[] = {n->reg, n->index}, stop[] = {n->reg};
        if (codegen_emit(c, f, n->span, MO_OP_CONST, args, 2, NULL, 0) == MO_NO_INDEX ||
            codegen_emit(c, f, n->span, MO_OP_STOP, stop, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
        break;
    }
    case MO_CORE_EXPR_JOIN: {
        uint32_t skip = codegen_emit(c, f, n->span, MO_OP_JUMP, (uint32_t[]){0}, 1, NULL, 0);
        if (skip == MO_NO_INDEX)
            return false;
        uint32_t checkpoint = f->changes.count;
        struct join *joins = f->joins.items;
        joins[n->index].offset = f->code.count;
        joins[n->index].checkpoint = checkpoint;
        if (!forget_dead(c, f, &joins[n->index].live, MO_NO_INDEX, n->span, false))
            return false;
        /* Every incoming jump writes owned reference parameters. The first entry
         * and loop backedges share the declaration's lexical state outside those parameters. */
        for (uint32_t i = 0; i < n->bind_count; i++)
            if (codegen_reference(c, ((struct reg *)f->regs.items)[n->binds[i]].type)) {
                if (!state(c, f, n->binds[i], REF_OWNED))
                    return false;
                codegen_owner_add(f, n->binds[i]);
                if (!joins[n->index].used[i] && !drop(c, f, n->binds[i], n->span))
                    return false;
            }
        uint32_t start = f->code.count;
        /* An exhaustive terminal rest can leave a continuation with no incoming jump.
         * Its body has no valid reference state and must not be generated. */
        bool reachable = joins[n->index].reachable;
        if (reachable && !codegen_generate(c, f, n->children[0], tail, depth + 1, live))
            return false;
        uint32_t exit = MO_NO_INDEX;
        bool any = reachable && *live;
        if (any) {
            bool owned = ((struct ownership *)c->ownership.items)[codegen_flow_id(f, n->reg)].owned;
            if (!copy(c, f, n->reg, n->children[0]->reg, owned, n->span, n->last[0]) ||
                !normalize(c, f, checkpoint, n->reg, n->span, &n->after))
                return false;
            exit = codegen_emit(c, f, n->span, MO_OP_JUMP, (uint32_t[]){0}, 1, NULL, 0);
            if (exit == MO_NO_INDEX)
                return false;
        }
        for (uint32_t i = 0; i < n->bind_count; i++)
            if (!codegen_named(c, f, n->binds[i], start, f->code.count))
                return false;
        restore(f, checkpoint);
        ((uint32_t *)f->code.items)[skip + 1] = f->code.count;
        if (!codegen_generate(c, f, n->children[1], tail, depth + 1, live))
            return false;
        if (*live) {
            bool owned = ((struct ownership *)c->ownership.items)[codegen_flow_id(f, n->reg)].owned;
            if (!copy(c, f, n->reg, n->children[1]->reg, owned, n->span, n->last[1]) ||
                !normalize(c, f, checkpoint, n->reg, n->span, &n->after))
                return false;
        }
        if (exit != MO_NO_INDEX)
            ((uint32_t *)f->code.items)[exit + 1] = f->code.count;
        *live |= any;
        restore(f, checkpoint);
        if (*live &&
            (!forget_dead(c, f, &n->after, n->reg, n->span, false) || !merged(c, f, n->reg)))
            return false;
        break;
    }
    case MO_CORE_EXPR_JUMP: {
        const struct join *target = &((struct join *)f->joins.items)[n->index];
        MO_ASSERT(target->offset != MO_NO_INDEX);
        for (uint32_t i = 0; i < n->count; i++) {
            if (!codegen_generate(c, f, n->children[i], false, depth + 1, live) || !*live)
                return c->error == 0;
        }
        for (uint32_t i = 0; i < n->count; i++) {
            if (!copy(c, f, n->binds[i], n->children[i]->reg, true, n->span, n->last[i]))
                return false;
        }
        for (uint32_t i = 0; i < n->count; i++)
            ((struct reg *)f->regs.items)[n->binds[i]].protected = true;
        if (!normalize(c, f, target->checkpoint, MO_NO_INDEX, n->span, &target->live))
            return false;
        for (uint32_t i = 0; i < n->count; i++) {
            uint32_t to = target->node->binds[i], from = n->binds[i];
            const uint32_t args[] = {to, from};
            if (codegen_emit(c, f, n->span, MO_OP_MOVE, args, 2, NULL, 0) == MO_NO_INDEX)
                return false;
            ((struct reg *)f->regs.items)[from].protected = false;
            if (codegen_reference(c, ((struct reg *)f->regs.items)[from].type)) {
                owner_remove(f, from);
                codegen_owner_add(f, to);
                if (codegen_emit(c, f, n->span, MO_OP_CLEAR, &from, 1, NULL, 0) == MO_NO_INDEX)
                    return false;
            }
        }
        MO_ASSERT(target->reachable);
        if (codegen_emit(c, f, n->span, MO_OP_JUMP, &target->offset, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
        break;
    }
    }
    if (*live && tail) {
        uint32_t result = argument(c, f, n->reg, true, n->span, true);
        if (result == MO_NO_INDEX)
            return false;
        owner_remove(f, result);
        if (!cleanup(c, f, n->span) ||
            codegen_emit(c, f, n->span, MO_OP_RET, &result, 1, NULL, 0) == MO_NO_INDEX)
            return false;
        *live = false;
    }
    return true;
}
