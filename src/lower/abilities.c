#include "lower/scalar.h"

#include <stdio.h>

/* Reserve a function before following its parts: nominal proof graphs may cycle.
 * Body construction drains this list instead of recursively walking the graph. */
struct derived {
    struct derived *next, *hash_next;
    const struct mo_ability_implementation *proof;
    const struct substitution *substitution;
    struct instance *function;
    mo_core_ty type;
    struct mo_span span;
};

static uint32_t bucket(struct lowering *p, mo_core_ty type, uint32_t owner,
                       enum mo_source_ability ability, uint32_t slots) {
    uint64_t key = lower_type_hash(p, type) ^ ((uint64_t)owner << 32) ^ ((uint64_t)ability << 48);
    key ^= key >> 30;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27;
    return (uint32_t)(key ^ (key >> 31)) & (slots - 1);
}

static mo_core_expr_list args(struct lowering *p, size_t count) {
    return (mo_core_expr_list){LOWER_NEW(p, mo_core_expr, count), count};
}
static mo_core_expr text(struct lowering *p, mo_core_string value) {
    mo_core_expr e = lower_unit(p);
    e->as.lit->tag = MO_CORE_LIT_TEXT;
    e->as.lit->as.text = value;
    return e;
}
static mo_core_expr concat(struct lowering *p, mo_core_expr left, mo_core_expr right) {
    mo_core_expr_list values = args(p, 2);
    values.items[0] = left;
    values.items[1] = right;
    return lower_prim(
        p, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_CONCAT},
        values);
}
static mo_core_expr projection(struct lowering *p, mo_core_expr value, size_t slot) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_PROJ);
    e->as.proj.expr = value;
    e->as.proj.index.digits = lower_number(p, slot);
    return e;
}
static mo_core_expr replace(struct lowering *p, mo_core_expr value, mo_core_string from,
                            mo_core_string to) {
    mo_core_expr_list values = args(p, 3);
    values.items[0] = value;
    values.items[1] = text(p, from);
    values.items[2] = text(p, to);
    return lower_prim(
        p, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_REPLACE},
        values);
}
static mo_core_expr quote(struct lowering *p, mo_core_expr value, bool character) {
    /* Escape existing slashes first, so escapes inserted afterward remain single.
     * All C0 controls and DEL have source spellings, including embedded NUL. */
    value = replace(p, value, lower_text(p, "\\"), lower_text(p, "\\\\"));
    const char *delimiter = character ? "'" : "\"";
    value = replace(p, value, lower_text(p, delimiter), lower_text(p, character ? "\\'" : "\\\""));
    if (!character)
        value = replace(p, value, lower_text(p, "#{"), lower_text(p, "\\#{"));
    for (unsigned i = 0; i <= 32; i++) {
        char control = (char)(i == 32 ? 127 : i);
        char escaped[9];
        int count = snprintf(escaped, sizeof escaped, "\\u{%x}", (unsigned char)control);
        MO_ASSERT(count > 0 && (size_t)count < sizeof escaped);
        mo_core_string spelling = control == '\n'   ? lower_text(p, "\\n")
                                  : control == '\r' ? lower_text(p, "\\r")
                                  : control == '\t' ? lower_text(p, "\\t")
                                                    : lower_copy(p, escaped, (size_t)count);
        value = replace(p, value, lower_copy(p, &control, 1), spelling);
    }
    return concat(p, text(p, lower_text(p, delimiter)),
                  concat(p, value, text(p, lower_text(p, delimiter))));
}
static mo_core_expr part_text(struct lowering *p, struct derived *d, uint32_t part,
                              mo_core_expr value) {
    MO_ASSERT(part < d->proof->part_count);
    const struct mo_ability_implementation *proof = d->proof->parts[part];
    const struct substitution *substitution = d->substitution;
    lower_proof(p, &proof, &substitution);
    mo_core_expr converted =
        lower_to_text(p, proof, substitution, d->function->owner, d->function->depth, value);
    mo_core_ty type = lower_type(p, proof->type, substitution);
    if (proof->kind == MO_ABILITY_NATIVE &&
        (type->tag == MO_CORE_TY_TEXT || type->tag == MO_CORE_TY_CHAR))
        converted = quote(p, converted, type->tag == MO_CORE_TY_CHAR);
    return converted;
}

static struct derived *reserve(struct lowering *p, const struct mo_ability_implementation *proof,
                               const struct substitution *substitution, uint32_t owner,
                               uint32_t depth) {
    mo_core_ty type = lower_type(p, proof->type, substitution);
    if (proof->type->module != UINT32_MAX && proof->type->kind == MO_SOURCE_NAMED)
        owner = proof->type->module;
    if (p->derived_count >= p->derived_slots) {
        if (p->derived_count == MO_LIMIT_CORE_NODES)
            lower_refuse(p, MO_CODE_LOWER_WORK, "too many derived ability functions");
        uint32_t slots = p->derived_slots == 0 ? 64 : p->derived_slots * 2;
        struct derived **table = LOWER_NEW(p, struct derived *, slots);
        for (struct derived *item = p->derived; item != NULL; item = item->next) {
            lower_work(p, 1);
            uint32_t at = bucket(p, item->type, item->function->owner, item->proof->ability, slots);
            item->hash_next = table[at];
            table[at] = item;
        }
        p->derived_index = table;
        p->derived_slots = slots;
    }
    uint32_t at = bucket(p, type, owner, proof->ability, p->derived_slots);
    struct derived *d;
    for (d = p->derived_index[at]; d != NULL; d = d->hash_next) {
        lower_work(p, 1);
        if (d->type == type && d->function->owner == owner && d->proof->ability == proof->ability)
            break;
    }
    if (d == NULL) {
        d = LOWER_NEW(p, struct derived, 1);
        d->proof = proof;
        d->substitution = substitution;
        d->type = type;
        d->span = p->span;
        if (p->derived_tail == NULL)
            p->derived_tail = &p->derived;
        *p->derived_tail = d;
        p->derived_tail = &d->next;
        if (p->derived_pending == NULL)
            p->derived_pending = d;
        d->hash_next = p->derived_index[at];
        p->derived_index[at] = d;
        p->derived_count++;
        struct instance *f = LOWER_NEW(p, struct instance, 1);
        d->function = f;
        f->owner = owner;
        f->depth = depth;
        f->index = p->function_count++;
        *p->function_tail = f;
        p->function_tail = &f->next;
        f->fun.module = lower_text(p, p->program->set->names[owner]);
        f->fun.name = lower_concat(
            p, f->fun.module, proof->ability == MO_SOURCE_TEXT ? ".%to_text(" : ".%compare(",
            lower_concat(p, lower_type_name(p, type), "", lower_text(p, ")")));
        f->fun.const_count.digits = lower_text(p, "0");
        static const struct mo_source_type text_type = {
            .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
        static const struct mo_source_type order_type = {
            .kind = MO_SOURCE_NAMED, .name = "Ordering", .module = UINT32_MAX};
        f->fun.ret =
            lower_type(p, proof->ability == MO_SOURCE_TEXT ? &text_type : &order_type, NULL);
        f->fun.params.count = proof->ability == MO_SOURCE_TEXT ? 1 : 2;
        f->fun.params.items = LOWER_NEW(p, struct mo_core_param, f->fun.params.count);
        for (size_t i = 0; i < f->fun.params.count; i++)
            f->fun.params.items[i] = (struct mo_core_param){lower_fresh(p), type};
        f->meta.body.params.count = f->fun.params.count;
        f->meta.body.params.items = LOWER_NEW(p, struct mo_core_name, f->fun.params.count);
        f->meta.name = f->fun.name;
        f->meta.span = lower_span(p, p->span);
    }
    return d;
}

mo_core_expr lower_derived_text(struct lowering *p, const struct mo_ability_implementation *proof,
                                const struct substitution *substitution, uint32_t owner,
                                uint32_t depth, mo_core_expr value) {
    struct derived *d = reserve(p, proof, substitution, owner, depth);
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = d->function->fun.name;
    call->as.call.args = args(p, 1);
    call->as.call.args.items[0] = value;
    return call;
}

static mo_core_expr ordering(struct lowering *p, const char *name) {
    static const struct mo_source_type type = {
        .kind = MO_SOURCE_NAMED, .name = "Ordering", .module = UINT32_MAX};
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CON);
    e->as.con.ty = lower_type(p, &type, NULL);
    e->as.con.ctor = lower_text(p, name);
    return e;
}

mo_core_expr lower_compare(struct lowering *p, const struct mo_ability_implementation *proof,
                           const struct substitution *substitution, uint32_t owner, uint32_t depth,
                           mo_core_expr left, mo_core_expr right) {
    MO_ASSERT(proof != NULL && proof->ability == MO_SOURCE_ORDER);
    lower_proof(p, &proof, &substitution);
    mo_core_expr_list values = args(p, 2);
    values.items[0] = left;
    values.items[1] = right;
    if (proof->kind == MO_ABILITY_DERIVED || proof->kind == MO_ABILITY_FUNCTION) {
        struct instance *f;
        if (proof->kind == MO_ABILITY_DERIVED)
            f = reserve(p, proof, substitution, owner, depth)->function;
        else
            f = lower_customizer(p, proof, substitution, owner, depth);
        mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
        call->as.call.name = f->fun.name;
        call->as.call.args = values;
        return call;
    }
    if (proof->kind != MO_ABILITY_NATIVE)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "abstract ordering is not substituted yet");
    mo_core_ty type = lower_type(p, proof->type, substitution);
    if (type->tag == MO_CORE_TY_UNIT)
        return ordering(p, "Equal");
    mo_core_expr less = lower_expr(p, MO_CORE_EXPR_IF);
    less->as.if_.cond = lower_prim(p, lower_operator(p, MO_T_LT, type, type), values);
    less->as.if_.then_ = ordering(p, "Less");
    mo_core_expr greater = lower_expr(p, MO_CORE_EXPR_IF);
    greater->as.if_.cond = lower_prim(p, lower_operator(p, MO_T_GT, type, type), values);
    greater->as.if_.then_ = ordering(p, "Greater");
    greater->as.if_.else_ = ordering(p, "Equal");
    less->as.if_.else_ = greater;
    if (type->tag == MO_CORE_TY_FLOAT) {
        struct mo_core_prim nan = {.tag = MO_CORE_PRIM_FLOAT, .as.float_ = MO_CORE_FLOAT_OP_IS_NAN};
        mo_core_expr_list one = args(p, 1);
        one.items[0] = right;
        mo_core_expr other = lower_expr(p, MO_CORE_EXPR_IF);
        other->as.if_.cond = lower_prim(p, nan, one);
        other->as.if_.then_ = ordering(p, "Less");
        other->as.if_.else_ = less;
        mo_core_expr both = lower_expr(p, MO_CORE_EXPR_IF);
        both->as.if_.cond = lower_prim(p, nan, one);
        both->as.if_.then_ = ordering(p, "Equal");
        both->as.if_.else_ = ordering(p, "Greater");
        one = args(p, 1);
        one.items[0] = left;
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_IF);
        e->as.if_.cond = lower_prim(p, nan, one);
        e->as.if_.then_ = both;
        e->as.if_.else_ = other;
        return e;
    }
    return less;
}

static mo_core_expr order_case(struct lowering *p, mo_core_expr value) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = value;
    e->as.case_.arms.count = 3;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 3);
    e->as.case_.arms.items[0] =
        (struct mo_core_arm){.ctor = lower_text(p, "Less"), .body = ordering(p, "Less")};
    e->as.case_.arms.items[1].ctor = lower_text(p, "Equal");
    e->as.case_.arms.items[2] =
        (struct mo_core_arm){.ctor = lower_text(p, "Greater"), .body = ordering(p, "Greater")};
    return e;
}

static void fields_order(struct lowering *p, struct derived *d, uint32_t first,
                         mo_core_expr_list left, mo_core_expr_list right, mo_core_expr *out) {
    MO_ASSERT(left.count == right.count);
    for (uint32_t i = 0; i < left.count; i++) {
        lower_work(p, 1);
        MO_ASSERT(first + i < d->proof->part_count);
        mo_core_expr e = order_case(p, lower_compare(p, d->proof->parts[first + i], d->substitution,
                                                     d->function->owner, d->function->depth,
                                                     left.items[i], right.items[i]));
        *out = e;
        out = &e->as.case_.arms.items[1].body;
    }
    *out = ordering(p, "Equal");
}

static mo_core_expr list_case(struct lowering *p, mo_core_ty element, mo_core_expr value) {
    mo_core_expr_list values = args(p, 1);
    values.items[0] = value;
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = lower_prim(
        p,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = element, .op.tag = MO_CORE_LIST_OP_UNCONS}},
        values);
    e->as.case_.arms.count = 2;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    e->as.case_.arms.items[0].ctor = lower_text(p, "None");
    e->as.case_.arms.items[1].ctor = lower_text(p, "Some");
    e->as.case_.arms.items[1].names.count = 1;
    e->as.case_.arms.items[1].names.items = LOWER_NEW(p, mo_core_string, 1);
    e->as.case_.arms.items[1].names.items[0] = lower_fresh(p);
    return e;
}

static void list_order(struct lowering *p, struct derived *d) {
    MO_ASSERT(d->proof->part_count == 1);
    struct instance *f = d->function;
    mo_core_expr join = lower_expr(p, MO_CORE_EXPR_JOIN);
    join->as.join.name = lower_fresh(p);
    join->as.join.params = f->fun.params;
    mo_core_expr start = lower_expr(p, MO_CORE_EXPR_JUMP);
    start->as.jump.name = join->as.join.name;
    start->as.jump.args = args(p, 2);
    for (uint32_t i = 0; i < 2; i++)
        start->as.jump.args.items[i] = lower_var(p, f->fun.params.items[i].name);
    join->as.join.rest = start;
    f->fun.body = join;
    mo_core_expr left = list_case(p, d->type->as.list, start->as.jump.args.items[0]);
    join->as.join.body = left;
    for (uint32_t i = 0; i < 2; i++) {
        mo_core_expr right = list_case(p, d->type->as.list, start->as.jump.args.items[1]);
        left->as.case_.arms.items[i].body = right;
        right->as.case_.arms.items[0].body = ordering(p, i == 0 ? "Equal" : "Greater");
        if (i == 0)
            right->as.case_.arms.items[1].body = ordering(p, "Less");
        else {
            mo_core_string a = left->as.case_.arms.items[1].names.items[0];
            mo_core_string b = right->as.case_.arms.items[1].names.items[0];
            mo_core_expr compare =
                order_case(p, lower_compare(p, d->proof->parts[0], d->substitution, f->owner,
                                            f->depth, projection(p, lower_var(p, a), 0),
                                            projection(p, lower_var(p, b), 0)));
            right->as.case_.arms.items[1].body = compare;
            mo_core_expr again = lower_expr(p, MO_CORE_EXPR_JUMP);
            again->as.jump.name = join->as.join.name;
            again->as.jump.args = args(p, 2);
            again->as.jump.args.items[0] = projection(p, lower_var(p, a), 1);
            again->as.jump.args.items[1] = projection(p, lower_var(p, b), 1);
            compare->as.case_.arms.items[1].body = again;
        }
    }
}

static void derive_order(struct lowering *p, struct derived *d) {
    p->span = d->span;
    struct instance *f = d->function;
    if (d->type->tag == MO_CORE_TY_LIST) {
        list_order(p, d);
        return;
    }
    if (d->type->tag == MO_CORE_TY_TUPLE) {
        size_t count = d->type->as.tuple.count;
        MO_ASSERT(count == d->proof->part_count);
        mo_core_expr_list left = args(p, count), right = args(p, count);
        for (size_t i = 0; i < count; i++) {
            left.items[i] = projection(p, lower_var(p, f->fun.params.items[0].name), i);
            right.items[i] = projection(p, lower_var(p, f->fun.params.items[1].name), i);
        }
        fields_order(p, d, 0, left, right, &f->fun.body);
        return;
    }
    if (d->type->tag != MO_CORE_TY_DATA && d->type->tag != MO_CORE_TY_OPTION &&
        d->type->tag != MO_CORE_TY_RESULT && d->type->tag != MO_CORE_TY_ORDERING &&
        d->type->tag != MO_CORE_TY_FILE_ERROR && d->type->tag != MO_CORE_TY_ENTRY &&
        d->type->tag != MO_CORE_TY_ENTRY_KIND)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this derived order shape is not lowered yet");
    mo_core_ctors ctors = lower_ctors(p, d->type);
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = lower_var(p, f->fun.params.items[0].name);
    e->as.case_.arms.count = ctors.count;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, ctors.count);
    f->fun.body = e;
    uint32_t first = 0;
    for (size_t i = 0; i < ctors.count; i++) {
        lower_work(p, 1);
        size_t count = ctors.items[i].fields.count;
        struct mo_core_arm *a = &e->as.case_.arms.items[i];
        a->ctor = ctors.items[i].name;
        a->names.count = count;
        a->names.items = LOWER_NEW(p, mo_core_string, count);
        mo_core_expr_list left = args(p, count);
        for (size_t k = 0; k < count; k++) {
            a->names.items[k] = lower_fresh(p);
            left.items[k] = lower_var(p, a->names.items[k]);
        }
        mo_core_expr other = lower_expr(p, MO_CORE_EXPR_CASE);
        a->body = other;
        other->as.case_.expr = lower_var(p, f->fun.params.items[1].name);
        other->as.case_.arms.count = ctors.count;
        other->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, ctors.count);
        for (size_t j = 0; j < ctors.count; j++) {
            lower_work(p, 1);
            struct mo_core_arm *b = &other->as.case_.arms.items[j];
            b->ctor = ctors.items[j].name;
            b->names.count = ctors.items[j].fields.count;
            b->names.items = LOWER_NEW(p, mo_core_string, b->names.count);
            for (size_t k = 0; k < b->names.count; k++)
                b->names.items[k] = lower_fresh(p);
            if (i != j)
                b->body = ordering(p, i < j ? "Less" : "Greater");
            else {
                mo_core_expr_list right = args(p, count);
                for (size_t k = 0; k < count; k++)
                    right.items[k] = lower_var(p, b->names.items[k]);
                fields_order(p, d, first, left, right, &b->body);
            }
        }
        MO_ASSERT(count <= UINT32_MAX - first);
        first += (uint32_t)count;
    }
    MO_ASSERT(first == d->proof->part_count);
}

static void fields_text(struct lowering *p, struct derived *d, uint32_t first,
                        mo_core_expr_list values, const struct mo_core_param *fields,
                        mo_core_string opening, mo_core_string closing, mo_core_expr *out) {
    size_t count = values.count;
    if (count == 0) {
        *out = text(p, lower_concat(p, opening, "", closing));
        return;
    }
    mo_core_expr_list pieces = args(p, 2 * count + 1);
    pieces.items[0] = text(p, opening);
    for (uint32_t i = 0; i < count; i++) {
        lower_work(p, 1);
        mo_core_expr bound = lower_expr(p, MO_CORE_EXPR_LET);
        bound->as.let_.name = lower_fresh(p);
        bound->as.let_.bound = part_text(p, d, first + i, values.items[i]);
        *out = bound;
        out = &bound->as.let_.body;
        pieces.items[2 * i + 1] = lower_var(p, bound->as.let_.name);
        if (fields != NULL && !lower_same(fields[i].name, lower_text(p, "0")))
            pieces.items[2 * i + 1] =
                concat(p, text(p, lower_concat(p, fields[i].name, ": ", lower_text(p, ""))),
                       pieces.items[2 * i + 1]);
        pieces.items[2 * i + 2] = text(p, i + 1 == count ? closing : lower_text(p, ", "));
    }
    mo_core_expr result = pieces.items[pieces.count - 1];
    for (size_t i = pieces.count - 1; i != 0; i--)
        result = concat(p, pieces.items[i - 1], result);
    *out = result;
}

static void list_text(struct lowering *p, struct derived *d) {
    MO_ASSERT(d->proof->part_count == 1);
    struct instance *f = d->function;
    static const struct mo_source_type text_type = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type *const parts[] = {&text_type};
    static const struct mo_source_type texts = {.kind = MO_SOURCE_NAMED,
                                                .name = "List",
                                                .module = UINT32_MAX,
                                                .count = 1,
                                                .arguments = parts};
    mo_core_expr join = lower_expr(p, MO_CORE_EXPR_JOIN);
    join->as.join.name = lower_fresh(p);
    join->as.join.params.count = 2;
    join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 2);
    join->as.join.params.items[0] = f->fun.params.items[0];
    join->as.join.params.items[1] =
        (struct mo_core_param){lower_fresh(p), lower_type(p, &texts, NULL)};
    mo_core_expr start = lower_expr(p, MO_CORE_EXPR_JUMP);
    start->as.jump.name = join->as.join.name;
    start->as.jump.args = args(p, 2);
    start->as.jump.args.items[0] = lower_var(p, f->fun.params.items[0].name);
    struct mo_core_prim list = {.tag = MO_CORE_PRIM_LIST, .as.list.t = f->fun.ret};
    list.as.list.op.tag = MO_CORE_LIST_OP_LIT;
    list.as.list.op.as.lit.digits = lower_text(p, "0");
    start->as.jump.args.items[1] = lower_prim(p, list, args(p, 0));
    join->as.join.rest = start;
    f->fun.body = join;
    mo_core_expr e = list_case(p, d->type->as.list, start->as.jump.args.items[0]);
    join->as.join.body = e;
    mo_core_string accumulated = join->as.join.params.items[1].name;
    mo_core_expr_list values = args(p, 1);
    values.items[0] = lower_var(p, accumulated);
    list.as.list.op.tag = MO_CORE_LIST_OP_REVERSE;
    mo_core_expr_list pieces = args(p, 2);
    pieces.items[0] = lower_prim(p, list, values);
    pieces.items[1] = text(p, lower_text(p, ", "));
    e->as.case_.arms.items[0].body =
        concat(p, text(p, lower_text(p, "[")),
               concat(p,
                      lower_prim(p,
                                 (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT,
                                                       .as.text = MO_CORE_TEXT_OP_JOIN},
                                 pieces),
                      text(p, lower_text(p, "]"))));
    mo_core_string pair = e->as.case_.arms.items[1].names.items[0];
    mo_core_expr converted = lower_expr(p, MO_CORE_EXPR_LET);
    converted->as.let_.name = lower_fresh(p);
    converted->as.let_.bound = part_text(p, d, 0, projection(p, lower_var(p, pair), 0));
    e->as.case_.arms.items[1].body = converted;
    mo_core_expr again = lower_expr(p, MO_CORE_EXPR_JUMP);
    again->as.jump.name = join->as.join.name;
    again->as.jump.args = args(p, 2);
    again->as.jump.args.items[0] = projection(p, lower_var(p, pair), 1);
    values = args(p, 2);
    values.items[0] = lower_var(p, accumulated);
    values.items[1] = lower_var(p, converted->as.let_.name);
    list.as.list.op.tag = MO_CORE_LIST_OP_PREPEND;
    again->as.jump.args.items[1] = lower_prim(p, list, values);
    converted->as.let_.body = again;
}

static void derive_text(struct lowering *p, struct derived *d) {
    p->span = d->span;
    struct instance *f = d->function;
    if (d->type->tag == MO_CORE_TY_LIST) {
        list_text(p, d);
        return;
    }
    if (d->type->tag == MO_CORE_TY_TUPLE) {
        size_t count = d->type->as.tuple.count;
        MO_ASSERT(count == d->proof->part_count);
        mo_core_expr_list values = args(p, count);
        for (size_t i = 0; i < count; i++)
            values.items[i] = projection(p, lower_var(p, f->fun.params.items[0].name), i);
        fields_text(p, d, 0, values, NULL, lower_text(p, "("), lower_text(p, ")"), &f->fun.body);
        return;
    }
    if (d->type->tag != MO_CORE_TY_DATA && d->type->tag != MO_CORE_TY_OPTION &&
        d->type->tag != MO_CORE_TY_RESULT && d->type->tag != MO_CORE_TY_ORDERING &&
        d->type->tag != MO_CORE_TY_FILE_ERROR && d->type->tag != MO_CORE_TY_ENTRY &&
        d->type->tag != MO_CORE_TY_ENTRY_KIND)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this derived text shape is not lowered yet");
    mo_core_ctors ctors = lower_ctors(p, d->type);
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = lower_var(p, f->fun.params.items[0].name);
    e->as.case_.arms.count = ctors.count;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, ctors.count);
    f->fun.body = e;
    uint32_t first = 0;
    for (size_t i = 0; i < ctors.count; i++) {
        lower_work(p, 1);
        struct mo_core_ctor *ctor = &ctors.items[i];
        struct mo_core_arm *arm = &e->as.case_.arms.items[i];
        arm->ctor = ctor->name;
        size_t count = ctor->fields.count;
        arm->names.count = count;
        arm->names.items = LOWER_NEW(p, mo_core_string, count);
        mo_core_expr_list values = args(p, count);
        for (size_t j = 0; j < count; j++) {
            arm->names.items[j] = lower_fresh(p);
            values.items[j] = lower_var(p, arm->names.items[j]);
        }
        if (count == 0)
            arm->body = text(p, ctor->name);
        else {
            const struct mo_core_param *fields = NULL;
            if (d->type->tag == MO_CORE_TY_DATA)
                fields = ctor->fields.items;
            else if (d->type->tag == MO_CORE_TY_FILE_ERROR || d->type->tag == MO_CORE_TY_ENTRY) {
                struct mo_core_param *named = LOWER_NEW(p, struct mo_core_param, count);
                for (size_t j = 0; j < count; j++)
                    named[j].name = lower_text(p, j == 0                             ? "path"
                                                  : d->type->tag == MO_CORE_TY_ENTRY ? "kind"
                                                                                     : "reason");
                fields = named;
            }
            fields_text(p, d, first, values, fields,
                        lower_concat(p, ctor->name, "(", lower_text(p, "")), lower_text(p, ")"),
                        &arm->body);
        }
        MO_ASSERT(count <= UINT32_MAX - first);
        first += (uint32_t)count;
    }
    MO_ASSERT(first == d->proof->part_count);
}

void lower_derived_functions(struct lowering *p) {
    /* Building a body can append children, including recursive self-edges
     * resolved by the reserved function. Each pending body runs once. */
    while (p->derived_pending != NULL) {
        lower_work(p, 1);
        struct derived *d = p->derived_pending;
        if (d->proof->ability == MO_SOURCE_TEXT)
            derive_text(p, d);
        else {
            MO_ASSERT(d->proof->ability == MO_SOURCE_ORDER);
            derive_order(p, d);
        }
        p->derived_pending = d->next;
    }
}
