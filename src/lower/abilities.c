#include "lower/scalar.h"

#include <stdio.h>
#include <string.h>

enum derived_kind {
    DERIVED_TEXT,
    DERIVED_ORDER,
    DERIVED_TO_JSON,
    DERIVED_FROM_JSON,
    DERIVED_JSON_SCHEMA,
};

/* Reserve a function before following its parts: nominal proof graphs may cycle.
 * Body construction drains this list instead of recursively walking the graph. */
struct derived {
    struct derived *next, *hash_next;
    enum derived_kind kind;
    const struct mo_ability_implementation *proof;
    const struct substitution *substitution;
    struct instance *function;
    mo_core_ty type;
    struct mo_span span;
};

static uint32_t bucket(struct lowering *p, mo_core_ty type, uint32_t owner, enum derived_kind kind,
                       uint32_t slots) {
    uint64_t key = lower_type_hash(p, type) ^ ((uint64_t)owner << 32) ^ ((uint64_t)kind << 48);
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

static const struct mo_module_scope *json_scope(struct lowering *p) {
    if (p->program->interfaces != NULL)
        for (uint32_t i = 0; i < p->program->interfaces->count; i++) {
            lower_work(p, 1);
            const struct mo_module_scope *scope = &p->program->interfaces->scopes[i];
            if (scope->library && scope->name != NULL && strcmp(scope->name, "Json") == 0)
                return scope;
        }
    lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "Json ability needs the library");
}

static const struct mo_source_type *json_data_source(struct lowering *p, const char *name) {
    const struct mo_module_scope *scope = json_scope(p);
    const struct mo_symbol *sym = mo_find_symbol(scope, (const uint8_t *)name, strlen(name));
    MO_ASSERT(sym != NULL);
    for (uint32_t i = 0; i < p->program->signatures->data_count; i++) {
        lower_work(p, 1);
        if (p->program->signatures->data[i].type->declaration == sym)
            return p->program->signatures->data[i].type;
    }
    MO_UNREACHABLE();
}

static const struct mo_source_type *result_source_type(struct lowering *p,
                                                       const struct mo_source_type *ok_type) {
    const struct mo_source_type *err_type = json_data_source(p, "JsonDecodeError");
    const struct mo_source_type **pair = LOWER_NEW(p, const struct mo_source_type *, 2);
    pair[0] = ok_type;
    pair[1] = err_type;
    struct mo_source_type *res = LOWER_NEW(p, struct mo_source_type, 1);
    *res = (struct mo_source_type){.kind = MO_SOURCE_NAMED,
                                   .name = "Result",
                                   .module = UINT32_MAX,
                                   .count = 2,
                                   .arguments = pair,
                                   .height = ok_type->height + 1,
                                   .parameters = ok_type->parameters};
    return res;
}

static struct derived *reserve_kind(struct lowering *p,
                                    const struct mo_ability_implementation *proof,
                                    const struct substitution *substitution, uint32_t owner,
                                    uint32_t depth, enum derived_kind kind) {
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
            uint32_t at = bucket(p, item->type, item->function->owner, item->kind, slots);
            item->hash_next = table[at];
            table[at] = item;
        }
        p->derived_index = table;
        p->derived_slots = slots;
    }
    uint32_t at = bucket(p, type, owner, kind, p->derived_slots);
    struct derived *d;
    for (d = p->derived_index[at]; d != NULL; d = d->hash_next) {
        lower_work(p, 1);
        if (d->type == type && d->function->owner == owner && d->kind == kind)
            break;
    }
    if (d == NULL) {
        d = LOWER_NEW(p, struct derived, 1);
        d->kind = kind;
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
        const char *prefix = kind == DERIVED_TEXT        ? ".%to_text("
                             : kind == DERIVED_ORDER     ? ".%compare("
                             : kind == DERIVED_TO_JSON   ? ".%to_json("
                             : kind == DERIVED_FROM_JSON ? ".%from_json("
                                                         : ".%json_schema(";
        f->fun.name =
            lower_concat(p, f->fun.module, prefix,
                         lower_concat(p, lower_type_name(p, type), "", lower_text(p, ")")));
        f->fun.const_count.digits = lower_text(p, "0");
        static const struct mo_source_type text_type = {
            .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
        static const struct mo_source_type bool_type = {
            .kind = MO_SOURCE_NAMED, .name = "Bool", .module = UINT32_MAX};
        static const struct mo_source_type order_type = {
            .kind = MO_SOURCE_NAMED, .name = "Ordering", .module = UINT32_MAX};
        static const struct mo_source_type *const list_text_args[] = {&text_type};
        static const struct mo_source_type list_text_type = {.kind = MO_SOURCE_NAMED,
                                                             .name = "List",
                                                             .module = UINT32_MAX,
                                                             .count = 1,
                                                             .arguments = list_text_args};
        if (kind == DERIVED_TEXT || kind == DERIVED_ORDER) {
            f->fun.ret = lower_type(p, kind == DERIVED_TEXT ? &text_type : &order_type, NULL);
            f->fun.params.count = kind == DERIVED_TEXT ? 1 : 2;
            f->fun.params.items = LOWER_NEW(p, struct mo_core_param, f->fun.params.count);
            for (size_t i = 0; i < f->fun.params.count; i++)
                f->fun.params.items[i] = (struct mo_core_param){lower_fresh(p), type};
        } else if (kind == DERIVED_TO_JSON) {
            f->fun.ret = lower_type(p, json_data_source(p, "Json"), NULL);
            f->fun.params.count = 1;
            f->fun.params.items = LOWER_NEW(p, struct mo_core_param, 1);
            f->fun.params.items[0] = (struct mo_core_param){lower_fresh(p), type};
        } else if (kind == DERIVED_FROM_JSON) {
            f->fun.ret = lower_type(p, result_source_type(p, proof->type), substitution);
            f->fun.params.count = 3;
            f->fun.params.items = LOWER_NEW(p, struct mo_core_param, 3);
            f->fun.params.items[0] = (struct mo_core_param){
                lower_fresh(p), lower_type(p, json_data_source(p, "Json"), NULL)};
            f->fun.params.items[1] =
                (struct mo_core_param){lower_fresh(p), lower_type(p, &text_type, NULL)};
            f->fun.params.items[2] =
                (struct mo_core_param){lower_fresh(p), lower_type(p, &bool_type, NULL)};
        } else {
            f->fun.ret = lower_type(p, json_data_source(p, "Json"), NULL);
            f->fun.params.count = 1;
            f->fun.params.items = LOWER_NEW(p, struct mo_core_param, 1);
            f->fun.params.items[0] =
                (struct mo_core_param){lower_fresh(p), lower_type(p, &list_text_type, NULL)};
        }
        f->meta.body.params.count = f->fun.params.count;
        f->meta.body.params.items = LOWER_NEW(p, struct mo_core_name, f->fun.params.count);
        f->meta.name = f->fun.name;
        f->meta.span = lower_span(p, p->span);
    }
    return d;
}

static struct derived *reserve(struct lowering *p, const struct mo_ability_implementation *proof,
                               const struct substitution *substitution, uint32_t owner,
                               uint32_t depth) {
    return reserve_kind(p, proof, substitution, owner, depth,
                        proof->ability == MO_SOURCE_TEXT ? DERIVED_TEXT : DERIVED_ORDER);
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
        d->type->tag != MO_CORE_TY_ENTRY_KIND && d->type->tag != MO_CORE_TY_SIMULATION_ERROR &&
        d->type->tag != MO_CORE_TY_NETWORK_ERROR && d->type->tag != MO_CORE_TY_SQLITE_VALUE &&
        d->type->tag != MO_CORE_TY_SQLITE_ROWS && d->type->tag != MO_CORE_TY_SQLITE_ERROR)
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

static void collection_text(struct lowering *p, struct derived *d) {
    bool set = d->type->tag == MO_CORE_TY_SET;
    bool map = d->type->tag == MO_CORE_TY_MAP;
    MO_ASSERT(d->proof->part_count == (map ? 2u : 1u));
    MO_ASSERT(set || map || d->type->tag == MO_CORE_TY_LIST);
    mo_core_ty list_type = d->type;
    if (set || map) {
        struct mo_source_type *source = LOWER_NEW(p, struct mo_source_type, 1);
        *source = *d->proof->type;
        source->name = "List";
        if (map) {
            struct mo_source_type *pair = LOWER_NEW(p, struct mo_source_type, 1);
            *pair = *source;
            pair->kind = MO_SOURCE_TUPLE;
            pair->name = "Tuple";
            source->count = 1;
            const struct mo_source_type **element = LOWER_NEW(p, const struct mo_source_type *, 1);
            element[0] = pair;
            source->arguments = element;
        }
        list_type = lower_type(p, source, d->substitution);
    }
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
    join->as.join.params.items[0].ty = list_type;
    join->as.join.params.items[1] =
        (struct mo_core_param){lower_fresh(p), lower_type(p, &texts, NULL)};
    mo_core_expr start = lower_expr(p, MO_CORE_EXPR_JUMP);
    start->as.jump.name = join->as.join.name;
    start->as.jump.args = args(p, 2);
    start->as.jump.args.items[0] = lower_var(p, f->fun.params.items[0].name);
    if (set) {
        mo_core_expr_list values = args(p, 1);
        values.items[0] = start->as.jump.args.items[0];
        start->as.jump.args.items[0] = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_SET,
                                  .as.set = {.t = d->type->as.set, .op = MO_CORE_SET_OP_TO_LIST}},
            values);
    } else if (map) {
        mo_core_expr_list values = args(p, 1);
        values.items[0] = start->as.jump.args.items[0];
        start->as.jump.args.items[0] =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                             .as.map = {.k = d->type->as.map.k,
                                                        .v = d->type->as.map.v,
                                                        .op.tag = MO_CORE_MAP_OP_ENTRIES}},
                       values);
    }
    struct mo_core_prim list = {.tag = MO_CORE_PRIM_LIST, .as.list.t = f->fun.ret};
    list.as.list.op.tag = MO_CORE_LIST_OP_LIT;
    list.as.list.op.as.lit.digits = lower_text(p, "0");
    start->as.jump.args.items[1] = lower_prim(p, list, args(p, 0));
    join->as.join.rest = start;
    f->fun.body = join;
    mo_core_expr e =
        list_case(p, list_type->as.list, lower_var(p, join->as.join.params.items[0].name));
    join->as.join.body = e;
    mo_core_string accumulated = join->as.join.params.items[1].name;
    mo_core_expr_list values = args(p, 1);
    values.items[0] = lower_var(p, accumulated);
    list.as.list.op.tag = MO_CORE_LIST_OP_REVERSE;
    mo_core_expr_list pieces = args(p, 2);
    pieces.items[0] = lower_prim(p, list, values);
    pieces.items[1] = text(p, lower_text(p, ", "));
    e->as.case_.arms.items[0].body =
        concat(p, text(p, lower_text(p, map ? "{" : "[")),
               concat(p,
                      lower_prim(p,
                                 (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT,
                                                       .as.text = MO_CORE_TEXT_OP_JOIN},
                                 pieces),
                      text(p, lower_text(p, map   ? "}"
                                            : set ? "].to_set"
                                                  : "]"))));
    mo_core_string pair = e->as.case_.arms.items[1].names.items[0];
    mo_core_expr converted = lower_expr(p, MO_CORE_EXPR_LET);
    converted->as.let_.name = lower_fresh(p);
    mo_core_expr element = projection(p, lower_var(p, pair), 0);
    if (map) {
        mo_core_expr key = lower_expr(p, MO_CORE_EXPR_LET);
        key->as.let_.name = lower_fresh(p);
        key->as.let_.bound = part_text(p, d, 0, projection(p, element, 0));
        key->as.let_.body = concat(
            p, lower_var(p, key->as.let_.name),
            concat(p, text(p, lower_text(p, ": ")), part_text(p, d, 1, projection(p, element, 1))));
        converted->as.let_.bound = key;
    } else
        converted->as.let_.bound = part_text(p, d, 0, element);
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
    if (d->type->tag == MO_CORE_TY_LIST || d->type->tag == MO_CORE_TY_SET ||
        d->type->tag == MO_CORE_TY_MAP) {
        collection_text(p, d);
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
    /* FileError's text form is its own to_text, in lib/file-error.mo. */
    if (d->type->tag == MO_CORE_TY_FILE_ERROR)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "FileError's text form needs the library");
    if (d->type->tag != MO_CORE_TY_DATA && d->type->tag != MO_CORE_TY_OPTION &&
        d->type->tag != MO_CORE_TY_RESULT && d->type->tag != MO_CORE_TY_ORDERING &&
        d->type->tag != MO_CORE_TY_ENTRY && d->type->tag != MO_CORE_TY_ENTRY_KIND &&
        d->type->tag != MO_CORE_TY_SIMULATION_ERROR && d->type->tag != MO_CORE_TY_NETWORK_ERROR &&
        d->type->tag != MO_CORE_TY_SQLITE_VALUE && d->type->tag != MO_CORE_TY_SQLITE_ROWS &&
        d->type->tag != MO_CORE_TY_SQLITE_ERROR)
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
            else if (d->type->tag == MO_CORE_TY_ENTRY ||
                     d->type->tag == MO_CORE_TY_SIMULATION_ERROR ||
                     d->type->tag == MO_CORE_TY_NETWORK_ERROR ||
                     d->type->tag == MO_CORE_TY_SQLITE_ROWS ||
                     d->type->tag == MO_CORE_TY_SQLITE_ERROR) {
                static const char *const entry[] = {"name", "kind"};
                static const char *const sqlite_rows[] = {"columns", "rows"};
                static const char *const sqlite_errors[] = {"path", "reason", "message"};
                static const char *const sim_errors[] = {NULL, "reason"};
                static const char *const network[][2] = {{"host"}, {"host", "port"}, {NULL},
                                                         {NULL},   {"reason"},       {"reason"}};
                struct mo_core_param *named = LOWER_NEW(p, struct mo_core_param, count);
                for (size_t j = 0; j < count; j++) {
                    const char *label = d->type->tag == MO_CORE_TY_ENTRY          ? entry[j]
                                        : d->type->tag == MO_CORE_TY_SQLITE_ROWS  ? sqlite_rows[j]
                                        : d->type->tag == MO_CORE_TY_SQLITE_ERROR ? sqlite_errors[i]
                                        : d->type->tag == MO_CORE_TY_SIMULATION_ERROR
                                            ? sim_errors[i]
                                            : network[i][j];
                    named[j].name = lower_text(p, label);
                }
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

static const struct mo_symbol *json_fn_symbol(struct lowering *p, const char *name) {
    const struct mo_module_scope *scope = json_scope(p);
    const struct mo_symbol *sym = mo_find_symbol(scope, (const uint8_t *)name, strlen(name));
    MO_ASSERT(sym != NULL && sym->declaration->kind == MO_S_FN);
    return sym;
}

static const struct mo_source_type *find_type_parameter(const struct mo_source_type *t) {
    if (t->kind == MO_SOURCE_PARAMETER)
        return t;
    for (uint32_t i = 0; i < t->count; i++) {
        const struct mo_source_type *found = find_type_parameter(t->arguments[i]);
        if (found != NULL)
            return found;
    }
    return NULL;
}

static mo_core_expr json_call(struct lowering *p, const char *name, mo_core_expr_list values) {
    const struct mo_symbol *sym = json_fn_symbol(p, name);
    struct instance *f =
        lower_instance(p, sym, (struct mo_type_arguments){0}, NULL, 0, NULL, sym->module, 0);
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = f->fun.name;
    call->as.call.args = values;
    return call;
}

static mo_core_expr json_call1(struct lowering *p, const char *name, mo_core_expr a) {
    mo_core_expr_list values = args(p, 1);
    values.items[0] = a;
    return json_call(p, name, values);
}

static mo_core_expr json_call2(struct lowering *p, const char *name, mo_core_expr a,
                               mo_core_expr b) {
    mo_core_expr_list values = args(p, 2);
    values.items[0] = a;
    values.items[1] = b;
    return json_call(p, name, values);
}

static mo_core_expr json_generic_call(struct lowering *p, const char *name,
                                      const struct mo_source_type *arg_type,
                                      const struct substitution *substitution, uint32_t owner,
                                      uint32_t depth, mo_core_expr_list values) {
    const struct mo_symbol *sym = json_fn_symbol(p, name);
    const struct mo_function_signature *sig = NULL;
    for (uint32_t i = 0; i < p->program->signatures->count; i++) {
        lower_work(p, 1);
        if (p->program->signatures->items[i].function == sym) {
            sig = &p->program->signatures->items[i];
            break;
        }
    }
    MO_ASSERT(sig != NULL);
    const struct mo_source_type *param = find_type_parameter(sig->type);
    MO_ASSERT(param != NULL);
    struct mo_type_argument *item = LOWER_NEW(p, struct mo_type_argument, 1);
    *item = (struct mo_type_argument){param, arg_type};
    struct instance *f = lower_instance(p, sym, (struct mo_type_arguments){1, item}, substitution,
                                        0, NULL, owner, depth + 1);
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = f->fun.name;
    call->as.call.args = values;
    return call;
}

static mo_core_expr make_con(struct lowering *p, mo_core_ty ty, const char *ctor,
                             mo_core_expr_list values) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CON);
    e->as.con.ty = ty;
    e->as.con.ctor = lower_text(p, ctor);
    e->as.con.args = values;
    return e;
}

static mo_core_expr make_con1(struct lowering *p, mo_core_ty ty, const char *ctor, mo_core_expr a) {
    mo_core_expr_list values = args(p, 1);
    values.items[0] = a;
    return make_con(p, ty, ctor, values);
}

static mo_core_expr make_let(struct lowering *p, mo_core_string name, mo_core_expr bound,
                             mo_core_expr body) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_LET);
    e->as.let_.name = name;
    e->as.let_.bound = bound;
    e->as.let_.body = body;
    return e;
}

static mo_core_expr empty_json_object(struct lowering *p) {
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_ty json_ty = lower_type(p, json_data_source(p, "Json"), NULL);
    mo_core_expr map_lit =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                         .as.map = {.k = text_ty,
                                                    .v = json_ty,
                                                    .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                           .lit.digits = lower_text(p, "0")}}},
                   args(p, 0));
    return make_con1(p, json_ty, "Object", map_lit);
}

static mo_core_string snake_string(struct lowering *p, mo_core_string raw) {
    size_t cap = raw.length * 2 + 2;
    char *buf = LOWER_NEW(p, char, cap);
    size_t n = 0;
    for (size_t i = 0; i < raw.length; i++) {
        unsigned char c = (unsigned char)raw.bytes[i];
        bool cap_c = c >= 'A' && c <= 'Z';
        bool word = i != 0 && cap_c &&
                    ((raw.bytes[i - 1] >= 'a' && raw.bytes[i - 1] <= 'z') ||
                     (raw.bytes[i - 1] >= '0' && raw.bytes[i - 1] <= '9') ||
                     (i + 1 < raw.length && raw.bytes[i + 1] >= 'a' && raw.bytes[i + 1] <= 'z'));
        if (word)
            buf[n++] = '_';
        buf[n++] = (char)(cap_c ? c + ('a' - 'A') : c);
    }
    return (mo_core_string){buf, n};
}

static bool is_json_struct(const struct derived *d) {
    if (d->type->tag == MO_CORE_TY_DATA)
        return d->proof->type->declaration->declaration->kind == MO_S_STRUCT;
    return d->type->tag == MO_CORE_TY_ENTRY || d->type->tag == MO_CORE_TY_SQLITE_ROWS;
}

static mo_core_string *ctor_labels(struct lowering *p, mo_core_ty type, size_t ctor_idx,
                                   const struct mo_core_ctor *ctor, bool *unnamed) {
    size_t count = ctor->fields.count;
    *unnamed = false;
    if (count == 0)
        return NULL;
    if (type->tag == MO_CORE_TY_DATA) {
        if (count == 1 && lower_same(ctor->fields.items[0].name, lower_text(p, "0"))) {
            *unnamed = true;
            return NULL;
        }
        mo_core_string *out = LOWER_NEW(p, mo_core_string, count);
        for (size_t j = 0; j < count; j++)
            out[j] = ctor->fields.items[j].name;
        return out;
    }
    if (type->tag == MO_CORE_TY_RESULT) {
        *unnamed = true;
        return NULL;
    }
    static const char *const entry[] = {"name", "kind"};
    static const char *const sqlite_rows[] = {"columns", "rows"};
    static const char *const sqlite_errors[] = {"path", "reason", "message"};
    static const char *const file_errors[] = {"path", "reason"};
    static const char *const sim_errors[] = {NULL, "reason"};
    static const char *const network[][2] = {{"host"}, {"host", "port"}, {NULL},
                                             {NULL},   {"reason"},       {"reason"}};
    mo_core_string *out = LOWER_NEW(p, mo_core_string, count);
    for (size_t j = 0; j < count; j++) {
        const char *label = type->tag == MO_CORE_TY_ENTRY              ? entry[j]
                            : type->tag == MO_CORE_TY_SQLITE_ROWS      ? sqlite_rows[j]
                            : type->tag == MO_CORE_TY_SQLITE_ERROR     ? sqlite_errors[ctor_idx]
                            : type->tag == MO_CORE_TY_FILE_ERROR       ? file_errors[j]
                            : type->tag == MO_CORE_TY_SIMULATION_ERROR ? sim_errors[ctor_idx]
                                                                       : network[ctor_idx][j];
        out[j] = lower_text(p, label);
    }
    return out;
}

mo_core_expr lower_to_json(struct lowering *p, const struct mo_ability_implementation *proof,
                           const struct substitution *substitution, uint32_t owner, uint32_t depth,
                           mo_core_expr value) {
    MO_ASSERT(proof != NULL && proof->ability == MO_SOURCE_JSON);
    lower_proof(p, &proof, &substitution);
    if (proof->encode_function != NULL) {
        struct instance *f = lower_customizer_fn(
            p, proof->encode_function, proof->encode_arguments, proof->encode_part_count,
            proof->encode_parts, proof->encode_part_parameters, substitution, owner, depth);
        mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
        call->as.call.name = f->fun.name;
        call->as.call.args = args(p, 1);
        call->as.call.args.items[0] = value;
        return call;
    }
    if (proof->kind == MO_ABILITY_DERIVED) {
        struct derived *d = reserve_kind(p, proof, substitution, owner, depth, DERIVED_TO_JSON);
        mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
        call->as.call.name = d->function->fun.name;
        call->as.call.args = args(p, 1);
        call->as.call.args.items[0] = value;
        return call;
    }
    if (proof->kind != MO_ABILITY_NATIVE)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "abstract Json ability is not substituted yet");
    mo_core_ty type = lower_type(p, proof->type, substitution);
    mo_core_ty json_ty = lower_type(p, json_data_source(p, "Json"), NULL);
    if (type->tag == MO_CORE_TY_UNIT)
        return make_let(p, lower_fresh(p), value, make_con(p, json_ty, "Null", args(p, 0)));
    if (type->tag == MO_CORE_TY_BOOL)
        return make_con1(p, json_ty, "Boolean", value);
    if (type->tag == MO_CORE_TY_INT)
        return json_call1(p, "int", value);
    if (type->tag == MO_CORE_TY_BYTE)
        return json_call1(p, "encode_byte", value);
    if (type->tag == MO_CORE_TY_FLOAT)
        return json_call1(p, "encode_float", value);
    if (type->tag == MO_CORE_TY_DECIMAL)
        return json_call1(p, "encode_decimal_str", value);
    if (type->tag == MO_CORE_TY_BIG_INT)
        return json_call1(p, "encode_big_int", value);
    if (type->tag == MO_CORE_TY_TEXT)
        return make_con1(p, json_ty, "String", value);
    if (type->tag == MO_CORE_TY_CHAR)
        return json_call1(p, "encode_char", value);
    if (type->tag == MO_CORE_TY_TIME)
        return json_call1(p, "encode_time", value);
    MO_ASSERT(type->tag == MO_CORE_TY_DATA);
    if (strcmp(proof->type->name, "Json") == 0)
        return value;
    MO_ASSERT(strcmp(proof->type->name, "JsonNumber") == 0);
    return make_con1(p, json_ty, "Number", value);
}

mo_core_expr lower_from_json(struct lowering *p, const struct mo_ability_implementation *proof,
                             const struct substitution *substitution, uint32_t owner,
                             uint32_t depth, mo_core_expr value, mo_core_expr path,
                             mo_core_expr strict) {
    MO_ASSERT(proof != NULL && proof->ability == MO_SOURCE_JSON);
    lower_proof(p, &proof, &substitution);
    if (proof->decode_function != NULL) {
        struct instance *f = lower_customizer_fn(
            p, proof->decode_function, proof->decode_arguments, proof->decode_part_count,
            proof->decode_parts, proof->decode_part_parameters, substitution, owner, depth);
        mo_core_expr raw = lower_expr(p, MO_CORE_EXPR_CALL);
        raw->as.call.name = f->fun.name;
        raw->as.call.args = args(p, 1);
        raw->as.call.args.items[0] = value;
        mo_core_expr_list wrap_args = args(p, 2);
        wrap_args.items[0] = raw;
        wrap_args.items[1] = path;
        return make_let(p, lower_fresh(p), strict,
                        json_generic_call(p, "wrap_custom_error", proof->type, substitution, owner,
                                          depth, wrap_args));
    }
    if (proof->kind == MO_ABILITY_DERIVED) {
        struct derived *d = reserve_kind(p, proof, substitution, owner, depth, DERIVED_FROM_JSON);
        mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
        call->as.call.name = d->function->fun.name;
        call->as.call.args = args(p, 3);
        call->as.call.args.items[0] = value;
        call->as.call.args.items[1] = path;
        call->as.call.args.items[2] = strict;
        return call;
    }
    if (proof->kind != MO_ABILITY_NATIVE)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "abstract Json ability is not substituted yet");
    mo_core_ty type = lower_type(p, proof->type, substitution);
    if (type->tag == MO_CORE_TY_DATA && strcmp(proof->type->name, "Json") == 0) {
        mo_core_ty ret_ty = lower_type(p, result_source_type(p, proof->type), substitution);
        return make_let(p, lower_fresh(p), path,
                        make_let(p, lower_fresh(p), strict, make_con1(p, ret_ty, "Ok", value)));
    }
    const char *helper = type->tag == MO_CORE_TY_UNIT      ? "decode_unit"
                         : type->tag == MO_CORE_TY_BOOL    ? "decode_bool"
                         : type->tag == MO_CORE_TY_INT     ? "decode_int"
                         : type->tag == MO_CORE_TY_BYTE    ? "decode_byte"
                         : type->tag == MO_CORE_TY_FLOAT   ? "decode_float"
                         : type->tag == MO_CORE_TY_DECIMAL ? "decode_decimal"
                         : type->tag == MO_CORE_TY_BIG_INT ? "decode_big_int"
                         : type->tag == MO_CORE_TY_TEXT    ? "decode_text"
                         : type->tag == MO_CORE_TY_CHAR    ? "decode_char"
                         : type->tag == MO_CORE_TY_TIME    ? "decode_time"
                                                           : "decode_json_number";
    return make_let(p, lower_fresh(p), strict, json_call2(p, helper, value, path));
}

mo_core_expr lower_json_decode_call(struct lowering *p,
                                    const struct mo_ability_implementation *proof,
                                    const struct substitution *substitution, uint32_t owner,
                                    uint32_t depth, mo_core_ty input_ty, mo_core_expr input,
                                    bool strict) {
    MO_ASSERT(proof != NULL && proof->ability == MO_SOURCE_JSON);
    lower_proof(p, &proof, &substitution);
    mo_core_ty ret_ty = lower_type(p, result_source_type(p, proof->type), substitution);
    const char *parse_fn =
        input_ty->tag == MO_CORE_TY_BYTES ? "parse_bytes_input" : "parse_text_input";
    mo_core_expr parsed = json_call1(p, parse_fn, input);
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = parsed;
    e->as.case_.arms.count = 2;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    mo_core_string ok_name = lower_fresh(p), err_name = lower_fresh(p);
    e->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
    e->as.case_.arms.items[0].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    e->as.case_.arms.items[0].names.items[0] = ok_name;
    e->as.case_.arms.items[0].body =
        lower_from_json(p, proof, substitution, owner, depth, lower_var(p, ok_name),
                        text(p, lower_text(p, "")), lower_bool(p, strict));
    e->as.case_.arms.items[1].ctor = lower_text(p, "Error");
    e->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    e->as.case_.arms.items[1].names.items[0] = err_name;
    e->as.case_.arms.items[1].body = make_con1(p, ret_ty, "Error", lower_var(p, err_name));
    return e;
}

mo_core_expr lower_json_schema(struct lowering *p, const struct mo_ability_implementation *proof,
                               const struct substitution *substitution, uint32_t owner,
                               uint32_t depth, mo_core_expr seen) {
    MO_ASSERT(proof != NULL && proof->ability == MO_SOURCE_JSON);
    lower_proof(p, &proof, &substitution);
    if (proof->encode_function != NULL || proof->decode_function != NULL)
        return make_let(p, lower_fresh(p), seen, empty_json_object(p));
    if (proof->kind == MO_ABILITY_DERIVED) {
        struct derived *d = reserve_kind(p, proof, substitution, owner, depth, DERIVED_JSON_SCHEMA);
        mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
        call->as.call.name = d->function->fun.name;
        call->as.call.args = args(p, 1);
        call->as.call.args.items[0] = seen;
        return call;
    }
    if (proof->kind != MO_ABILITY_NATIVE)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "abstract Json ability is not substituted yet");
    mo_core_ty type = lower_type(p, proof->type, substitution);
    mo_core_expr res = NULL;
    if (type->tag == MO_CORE_TY_UNIT)
        res = json_call1(p, "schema_type", text(p, lower_text(p, "null")));
    else if (type->tag == MO_CORE_TY_BOOL)
        res = json_call1(p, "schema_type", text(p, lower_text(p, "boolean")));
    else if (type->tag == MO_CORE_TY_INT)
        res = json_call1(p, "schema_type", text(p, lower_text(p, "integer")));
    else if (type->tag == MO_CORE_TY_BYTE)
        res = json_call(p, "schema_byte", args(p, 0));
    else if (type->tag == MO_CORE_TY_FLOAT)
        res = json_call1(p, "schema_type", text(p, lower_text(p, "number")));
    else if (type->tag == MO_CORE_TY_DECIMAL)
        res = json_call2(p, "schema_format", text(p, lower_text(p, "string")),
                         text(p, lower_text(p, "decimal")));
    else if (type->tag == MO_CORE_TY_BIG_INT)
        res = json_call2(p, "schema_format", text(p, lower_text(p, "string")),
                         text(p, lower_text(p, "bigint")));
    else if (type->tag == MO_CORE_TY_TEXT)
        res = json_call1(p, "schema_type", text(p, lower_text(p, "string")));
    else if (type->tag == MO_CORE_TY_CHAR)
        res = json_call(p, "schema_char", args(p, 0));
    else if (type->tag == MO_CORE_TY_TIME)
        res = json_call2(p, "schema_format", text(p, lower_text(p, "string")),
                         text(p, lower_text(p, "date-time")));
    else if (strcmp(proof->type->name, "Json") == 0)
        res = empty_json_object(p);
    else
        res = json_call1(p, "schema_type", text(p, lower_text(p, "number")));
    return make_let(p, lower_fresh(p), seen, res);
}

static mo_core_expr part_to_json(struct lowering *p, struct derived *d, uint32_t part,
                                 mo_core_expr value) {
    MO_ASSERT(part < d->proof->part_count);
    return lower_to_json(p, d->proof->parts[part], d->substitution, d->function->owner,
                         d->function->depth, value);
}

static mo_core_expr part_from_json(struct lowering *p, struct derived *d, uint32_t part,
                                   mo_core_expr value, mo_core_expr path, mo_core_expr strict) {
    MO_ASSERT(part < d->proof->part_count);
    return lower_from_json(p, d->proof->parts[part], d->substitution, d->function->owner,
                           d->function->depth, value, path, strict);
}

static mo_core_expr part_json_schema(struct lowering *p, struct derived *d, uint32_t part,
                                     mo_core_expr seen) {
    MO_ASSERT(part < d->proof->part_count);
    return lower_json_schema(p, d->proof->parts[part], d->substitution, d->function->owner,
                             d->function->depth, seen);
}

static mo_core_expr encode_lambda(struct lowering *p, struct derived *d, uint32_t part,
                                  mo_core_ty item_ty) {
    mo_core_expr lam = lower_expr(p, MO_CORE_EXPR_LAM);
    mo_core_string param = lower_fresh(p);
    lam->as.lam.params.count = 1;
    lam->as.lam.params.items = LOWER_NEW(p, struct mo_core_param, 1);
    lam->as.lam.params.items[0] = (struct mo_core_param){param, item_ty};
    lam->as.lam.body = part_to_json(p, d, part, lower_var(p, param));
    ((struct lower_node *)lam)->closure =
        lower_concat(p, d->function->meta.name, ".fn.", lower_number(p, p->serial++));
    return lam;
}

static mo_core_expr decode_lambda(struct lowering *p, struct derived *d, uint32_t part) {
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type bool_src = {
        .kind = MO_SOURCE_NAMED, .name = "Bool", .module = UINT32_MAX};
    mo_core_ty json_ty = lower_type(p, json_data_source(p, "Json"), NULL);
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_ty bool_ty = lower_type(p, &bool_src, NULL);
    mo_core_expr lam = lower_expr(p, MO_CORE_EXPR_LAM);
    mo_core_string v_name = lower_fresh(p), p_name = lower_fresh(p), s_name = lower_fresh(p);
    lam->as.lam.params.count = 3;
    lam->as.lam.params.items = LOWER_NEW(p, struct mo_core_param, 3);
    lam->as.lam.params.items[0] = (struct mo_core_param){v_name, json_ty};
    lam->as.lam.params.items[1] = (struct mo_core_param){p_name, text_ty};
    lam->as.lam.params.items[2] = (struct mo_core_param){s_name, bool_ty};
    lam->as.lam.body = part_from_json(p, d, part, lower_var(p, v_name), lower_var(p, p_name),
                                      lower_var(p, s_name));
    ((struct lower_node *)lam)->closure =
        lower_concat(p, d->function->meta.name, ".fn.", lower_number(p, p->serial++));
    return lam;
}

static mo_core_expr json_object_from_pairs(struct lowering *p, mo_core_ty json_ty, size_t count,
                                           const mo_core_string *keys, mo_core_expr_list vals) {
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_expr_list flat = args(p, 2 * count);
    for (size_t i = 0; i < count; i++) {
        flat.items[2 * i] = text(p, keys[i]);
        flat.items[2 * i + 1] = vals.items[i];
    }
    mo_core_expr map_lit =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                         .as.map = {.k = text_ty,
                                                    .v = json_ty,
                                                    .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                           .lit.digits = lower_number(p, count)}}},
                   flat);
    return make_con1(p, json_ty, "Object", map_lit);
}

static void derive_to_json(struct lowering *p, struct derived *d) {
    p->span = d->span;
    struct instance *f = d->function;
    mo_core_ty json_ty = f->fun.ret;
    mo_core_expr input = lower_var(p, f->fun.params.items[0].name);
    if (d->type->tag == MO_CORE_TY_LIST || d->type->tag == MO_CORE_TY_SET) {
        mo_core_ty elem_ty = d->type->tag == MO_CORE_TY_SET ? d->type->as.set : d->type->as.list;
        if (d->type->tag == MO_CORE_TY_SET) {
            mo_core_expr_list one = args(p, 1);
            one.items[0] = input;
            input = lower_prim(
                p,
                (struct mo_core_prim){.tag = MO_CORE_PRIM_SET,
                                      .as.set = {.t = elem_ty, .op = MO_CORE_SET_OP_TO_LIST}},
                one);
        }
        mo_core_expr_list call_args = args(p, 2);
        call_args.items[0] = input;
        call_args.items[1] = encode_lambda(p, d, 0, elem_ty);
        f->fun.body = json_generic_call(p, "encode_list", d->proof->type->arguments[0],
                                        d->substitution, f->owner, f->depth, call_args);
        return;
    }
    if (d->type->tag == MO_CORE_TY_MAP) {
        mo_core_expr_list call_args = args(p, 2);
        call_args.items[0] = input;
        call_args.items[1] = encode_lambda(p, d, 1, d->type->as.map.v);
        f->fun.body = json_generic_call(p, "encode_map", d->proof->type->arguments[1],
                                        d->substitution, f->owner, f->depth, call_args);
        return;
    }
    if (d->type->tag == MO_CORE_TY_OPTION) {
        mo_core_expr_list call_args = args(p, 2);
        call_args.items[0] = input;
        call_args.items[1] = encode_lambda(p, d, 0, d->type->as.option);
        f->fun.body = json_generic_call(p, "encode_option", d->proof->type->arguments[0],
                                        d->substitution, f->owner, f->depth, call_args);
        return;
    }
    if (d->type->tag == MO_CORE_TY_TUPLE) {
        size_t count = d->type->as.tuple.count;
        mo_core_expr_list items = args(p, count);
        for (size_t i = 0; i < count; i++)
            items.items[i] = part_to_json(p, d, (uint32_t)i, projection(p, input, i));
        mo_core_expr list_lit = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                  .as.list = {.t = json_ty,
                                              .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                     .as.lit.digits = lower_number(p, count)}}},
            items);
        f->fun.body = make_con1(p, json_ty, "Array", list_lit);
        return;
    }
    mo_core_ctors ctors = lower_ctors(p, d->type);
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
    e->as.case_.expr = input;
    e->as.case_.arms.count = ctors.count;
    e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, ctors.count);
    f->fun.body = e;
    bool record = is_json_struct(d);
    uint32_t first = 0;
    for (size_t i = 0; i < ctors.count; i++) {
        lower_work(p, 1);
        struct mo_core_ctor *ctor = &ctors.items[i];
        struct mo_core_arm *arm = &e->as.case_.arms.items[i];
        arm->ctor = ctor->name;
        size_t count = ctor->fields.count;
        arm->names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, count), count};
        mo_core_expr_list encoded = args(p, count);
        for (size_t j = 0; j < count; j++) {
            arm->names.items[j] = lower_fresh(p);
            encoded.items[j] =
                part_to_json(p, d, first + (uint32_t)j, lower_var(p, arm->names.items[j]));
        }
        bool unnamed = false;
        mo_core_string *labels = ctor_labels(p, d->type, i, ctor, &unnamed);
        if (record)
            arm->body = json_object_from_pairs(p, json_ty, count, labels, encoded);
        else if (count == 0)
            arm->body = make_con1(p, json_ty, "String", text(p, snake_string(p, ctor->name)));
        else {
            mo_core_string tag = snake_string(p, ctor->name);
            mo_core_expr payload = unnamed
                                       ? encoded.items[0]
                                       : json_object_from_pairs(p, json_ty, count, labels, encoded);
            mo_core_expr_list one_val = args(p, 1);
            one_val.items[0] = payload;
            arm->body = json_object_from_pairs(p, json_ty, 1, &tag, one_val);
        }
        first += (uint32_t)count;
    }
    MO_ASSERT(first == d->proof->part_count);
}

static mo_core_expr decode_fields_object(struct lowering *p, struct derived *d, uint32_t first,
                                         const struct mo_core_ctor *ctor,
                                         const mo_core_string *labels, mo_core_expr obj_expr,
                                         mo_core_string path_var, mo_core_string strict_var) {
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type int_src = {
        .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_ty int_ty = lower_type(p, &int_src, NULL);
    mo_core_ty json_ty = lower_type(p, json_data_source(p, "Json"), NULL);
    mo_core_ty ret_ty = d->function->fun.ret;
    size_t count = ctor->fields.count;

    mo_core_expr expect_case = lower_expr(p, MO_CORE_EXPR_CASE);
    expect_case->as.case_.expr = json_call2(p, "expect_object", obj_expr, lower_var(p, path_var));
    expect_case->as.case_.arms.count = 2;
    expect_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    mo_core_string map_var = lower_fresh(p), err_var = lower_fresh(p);
    expect_case->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
    expect_case->as.case_.arms.items[0].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    expect_case->as.case_.arms.items[0].names.items[0] = map_var;
    expect_case->as.case_.arms.items[1].ctor = lower_text(p, "Error");
    expect_case->as.case_.arms.items[1].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    expect_case->as.case_.arms.items[1].names.items[0] = err_var;
    expect_case->as.case_.arms.items[1].body = make_con1(p, ret_ty, "Error", lower_var(p, err_var));

    mo_core_string present_var = lower_fresh(p);
    mo_core_expr init_present = make_let(p, present_var, lower_int(p, 0), NULL);
    expect_case->as.case_.arms.items[0].body = init_present;
    mo_core_expr *cursor = &init_present->as.let_.body;
    mo_core_string *field_vars = LOWER_NEW(p, mo_core_string, count);

    for (size_t j = 0; j < count; j++) {
        lower_work(p, 1);
        mo_core_ty field_ty = ctor->fields.items[j].ty;
        mo_core_string next_val = lower_fresh(p), next_present = lower_fresh(p);
        field_vars[j] = next_val;

        mo_core_expr join = lower_expr(p, MO_CORE_EXPR_JOIN);
        join->as.join.name = lower_fresh(p);
        join->as.join.params.count = 2;
        join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 2);
        join->as.join.params.items[0] = (struct mo_core_param){next_val, field_ty};
        join->as.join.params.items[1] = (struct mo_core_param){next_present, int_ty};

        mo_core_expr_list get_args = args(p, 2);
        get_args.items[0] = lower_var(p, map_var);
        get_args.items[1] = text(p, labels[j]);
        mo_core_expr get_case = lower_expr(p, MO_CORE_EXPR_CASE);
        get_case->as.case_.expr =
            lower_prim(p,
                       (struct mo_core_prim){
                           .tag = MO_CORE_PRIM_MAP,
                           .as.map = {.k = text_ty, .v = json_ty, .op.tag = MO_CORE_MAP_OP_GET}},
                       get_args);
        get_case->as.case_.arms.count = 2;
        get_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        get_case->as.case_.arms.items[0].ctor = lower_text(p, "None");
        if (field_ty->tag == MO_CORE_TY_OPTION) {
            mo_core_expr jmp = lower_expr(p, MO_CORE_EXPR_JUMP);
            jmp->as.jump.name = join->as.join.name;
            jmp->as.jump.args = args(p, 2);
            jmp->as.jump.args.items[0] = make_con(p, field_ty, "None", args(p, 0));
            jmp->as.jump.args.items[1] = lower_var(p, present_var);
            get_case->as.case_.arms.items[0].body = jmp;
        } else {
            get_case->as.case_.arms.items[0].body = make_con1(
                p, ret_ty, "Error",
                json_call2(p, "missing_field_error", lower_var(p, path_var), text(p, labels[j])));
        }

        mo_core_string raw_fv = lower_fresh(p);
        get_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
        get_case->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        get_case->as.case_.arms.items[1].names.items[0] = raw_fv;

        mo_core_expr f_path =
            json_call2(p, "join_path", lower_var(p, path_var), text(p, labels[j]));
        mo_core_expr dec_case = lower_expr(p, MO_CORE_EXPR_CASE);
        dec_case->as.case_.expr = part_from_json(p, d, first + (uint32_t)j, lower_var(p, raw_fv),
                                                 f_path, lower_var(p, strict_var));
        dec_case->as.case_.arms.count = 2;
        dec_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string dec_ok = lower_fresh(p), dec_err = lower_fresh(p);
        dec_case->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        dec_case->as.case_.arms.items[0].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        dec_case->as.case_.arms.items[0].names.items[0] = dec_ok;
        mo_core_expr_list inc_args = args(p, 2);
        inc_args.items[0] = lower_var(p, present_var);
        inc_args.items[1] = lower_int(p, 1);
        mo_core_expr jmp_ok = lower_expr(p, MO_CORE_EXPR_JUMP);
        jmp_ok->as.jump.name = join->as.join.name;
        jmp_ok->as.jump.args = args(p, 2);
        jmp_ok->as.jump.args.items[0] = lower_var(p, dec_ok);
        jmp_ok->as.jump.args.items[1] = lower_prim(
            p, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_ADD},
            inc_args);
        dec_case->as.case_.arms.items[0].body = jmp_ok;

        dec_case->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        dec_case->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        dec_case->as.case_.arms.items[1].names.items[0] = dec_err;
        dec_case->as.case_.arms.items[1].body =
            make_con1(p, ret_ty, "Error", lower_var(p, dec_err));

        get_case->as.case_.arms.items[1].body = dec_case;
        join->as.join.rest = get_case;
        *cursor = join;
        cursor = &join->as.join.body;
        present_var = next_present;
    }

    mo_core_expr_list built_fields = args(p, count);
    mo_core_expr_list known_items = args(p, count);
    for (size_t j = 0; j < count; j++) {
        built_fields.items[j] = lower_var(p, field_vars[j]);
        known_items.items[j] = text(p, labels[j]);
    }
    mo_core_expr known_list = lower_prim(
        p,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = text_ty,
                                          .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                 .as.lit.digits = lower_number(p, count)}}},
        known_items);
    mo_core_expr ok_val =
        make_con1(p, ret_ty, "Ok", make_con(p, d->type, ctor->name.bytes, built_fields));
    mo_core_expr ok_else =
        make_con1(p, ret_ty, "Ok", make_con(p, d->type, ctor->name.bytes, built_fields));

    mo_core_expr_list strict_args = args(p, 4);
    strict_args.items[0] = lower_var(p, map_var);
    strict_args.items[1] = lower_var(p, present_var);
    strict_args.items[2] = known_list;
    strict_args.items[3] = lower_var(p, path_var);
    mo_core_expr strict_case = lower_expr(p, MO_CORE_EXPR_CASE);
    strict_case->as.case_.expr = json_call(p, "check_strict_keys", strict_args);
    strict_case->as.case_.arms.count = 2;
    strict_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    mo_core_string strict_ok = lower_fresh(p), strict_err = lower_fresh(p);
    strict_case->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
    strict_case->as.case_.arms.items[0].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    strict_case->as.case_.arms.items[0].names.items[0] = strict_ok;
    strict_case->as.case_.arms.items[0].body = ok_val;
    strict_case->as.case_.arms.items[1].ctor = lower_text(p, "Error");
    strict_case->as.case_.arms.items[1].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    strict_case->as.case_.arms.items[1].names.items[0] = strict_err;
    strict_case->as.case_.arms.items[1].body =
        make_con1(p, ret_ty, "Error", lower_var(p, strict_err));

    mo_core_expr gate = lower_expr(p, MO_CORE_EXPR_IF);
    gate->as.if_.cond = lower_var(p, strict_var);
    gate->as.if_.then_ = strict_case;
    gate->as.if_.else_ = ok_else;
    *cursor = gate;
    return expect_case;
}

static mo_core_expr bad_enum_case(struct lowering *p, mo_core_ty ret_ty, mo_core_string val_var,
                                  mo_core_string path_var, mo_core_string type_label) {
    mo_core_expr_list call_args = args(p, 3);
    call_args.items[0] = lower_var(p, val_var);
    call_args.items[1] = lower_var(p, path_var);
    call_args.items[2] = text(p, type_label);
    return make_con1(p, ret_ty, "Error", json_call(p, "unknown_case_error", call_args));
}

static void derive_from_json(struct lowering *p, struct derived *d) {
    p->span = d->span;
    struct instance *f = d->function;
    mo_core_ty ret_ty = f->fun.ret;
    mo_core_string val_var = f->fun.params.items[0].name;
    mo_core_string path_var = f->fun.params.items[1].name;
    mo_core_string strict_var = f->fun.params.items[2].name;

    if (d->type->tag == MO_CORE_TY_LIST || d->type->tag == MO_CORE_TY_SET) {
        mo_core_expr_list call_args = args(p, 4);
        call_args.items[0] = lower_var(p, val_var);
        call_args.items[1] = lower_var(p, path_var);
        call_args.items[2] = lower_var(p, strict_var);
        call_args.items[3] = decode_lambda(p, d, 0);
        mo_core_expr list_res = json_generic_call(p, "decode_list", d->proof->type->arguments[0],
                                                  d->substitution, f->owner, f->depth, call_args);
        if (d->type->tag == MO_CORE_TY_LIST) {
            f->fun.body = list_res;
            return;
        }
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
        e->as.case_.expr = list_res;
        e->as.case_.arms.count = 2;
        e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string lst_var = lower_fresh(p), err_var = lower_fresh(p);
        e->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        e->as.case_.arms.items[0].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        e->as.case_.arms.items[0].names.items[0] = lst_var;
        mo_core_expr_list one = args(p, 1);
        one.items[0] = lower_var(p, lst_var);
        e->as.case_.arms.items[0].body =
            make_con1(p, ret_ty, "Ok",
                      lower_prim(p,
                                 (struct mo_core_prim){.tag = MO_CORE_PRIM_SET,
                                                       .as.set = {.t = d->type->as.set,
                                                                  .op = MO_CORE_SET_OP_OF_LIST}},
                                 one));
        e->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        e->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        e->as.case_.arms.items[1].names.items[0] = err_var;
        e->as.case_.arms.items[1].body = make_con1(p, ret_ty, "Error", lower_var(p, err_var));
        f->fun.body = e;
        return;
    }
    if (d->type->tag == MO_CORE_TY_MAP) {
        mo_core_expr_list call_args = args(p, 4);
        call_args.items[0] = lower_var(p, val_var);
        call_args.items[1] = lower_var(p, path_var);
        call_args.items[2] = lower_var(p, strict_var);
        call_args.items[3] = decode_lambda(p, d, 1);
        f->fun.body = json_generic_call(p, "decode_map", d->proof->type->arguments[1],
                                        d->substitution, f->owner, f->depth, call_args);
        return;
    }
    if (d->type->tag == MO_CORE_TY_OPTION) {
        mo_core_expr_list call_args = args(p, 4);
        call_args.items[0] = lower_var(p, val_var);
        call_args.items[1] = lower_var(p, path_var);
        call_args.items[2] = lower_var(p, strict_var);
        call_args.items[3] = decode_lambda(p, d, 0);
        f->fun.body = json_generic_call(p, "decode_option", d->proof->type->arguments[0],
                                        d->substitution, f->owner, f->depth, call_args);
        return;
    }
    if (d->type->tag == MO_CORE_TY_TUPLE) {
        size_t count = d->type->as.tuple.count;
        mo_core_ty json_ty = lower_type(p, json_data_source(p, "Json"), NULL);
        mo_core_expr_list exp_args = args(p, 3);
        exp_args.items[0] = lower_var(p, val_var);
        exp_args.items[1] = lower_int(p, count);
        exp_args.items[2] = lower_var(p, path_var);
        mo_core_expr arr_case = lower_expr(p, MO_CORE_EXPR_CASE);
        arr_case->as.case_.expr = json_call(p, "expect_tuple_array", exp_args);
        arr_case->as.case_.arms.count = 2;
        arr_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string arr_var = lower_fresh(p), err_var = lower_fresh(p);
        arr_case->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        arr_case->as.case_.arms.items[0].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        arr_case->as.case_.arms.items[0].names.items[0] = arr_var;
        arr_case->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        arr_case->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        arr_case->as.case_.arms.items[1].names.items[0] = err_var;
        arr_case->as.case_.arms.items[1].body =
            make_con1(p, ret_ty, "Error", lower_var(p, err_var));

        mo_core_expr *cursor = &arr_case->as.case_.arms.items[0].body;
        mo_core_string *part_vars = LOWER_NEW(p, mo_core_string, count);
        for (size_t i = 0; i < count; i++) {
            mo_core_expr_list get_args = args(p, 2);
            get_args.items[0] = lower_var(p, arr_var);
            get_args.items[1] = lower_int(p, i);
            mo_core_expr get_case = lower_expr(p, MO_CORE_EXPR_CASE);
            get_case->as.case_.expr = lower_prim(
                p,
                (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                      .as.list = {.t = json_ty, .op.tag = MO_CORE_LIST_OP_GET}},
                get_args);
            get_case->as.case_.arms.count = 2;
            get_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            mo_core_expr stop = lower_expr(p, MO_CORE_EXPR_STOP);
            stop->as.stop = lower_text(p, "unreachable tuple index");
            get_case->as.case_.arms.items[0].ctor = lower_text(p, "None");
            get_case->as.case_.arms.items[0].body = stop;
            mo_core_string raw_item = lower_fresh(p);
            get_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
            get_case->as.case_.arms.items[1].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            get_case->as.case_.arms.items[1].names.items[0] = raw_item;

            mo_core_expr idx_path =
                json_call2(p, "index_path", lower_var(p, path_var), lower_int(p, i));
            mo_core_expr dec_case = lower_expr(p, MO_CORE_EXPR_CASE);
            dec_case->as.case_.expr = part_from_json(p, d, (uint32_t)i, lower_var(p, raw_item),
                                                     idx_path, lower_var(p, strict_var));
            dec_case->as.case_.arms.count = 2;
            dec_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            mo_core_string ok_item = lower_fresh(p), err_item = lower_fresh(p);
            part_vars[i] = ok_item;
            dec_case->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
            dec_case->as.case_.arms.items[0].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            dec_case->as.case_.arms.items[0].names.items[0] = ok_item;
            dec_case->as.case_.arms.items[1].ctor = lower_text(p, "Error");
            dec_case->as.case_.arms.items[1].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            dec_case->as.case_.arms.items[1].names.items[0] = err_item;
            dec_case->as.case_.arms.items[1].body =
                make_con1(p, ret_ty, "Error", lower_var(p, err_item));

            get_case->as.case_.arms.items[1].body = dec_case;
            *cursor = get_case;
            cursor = &dec_case->as.case_.arms.items[0].body;
        }
        mo_core_expr tup = lower_expr(p, MO_CORE_EXPR_TUPLE);
        tup->as.tuple = args(p, count);
        for (size_t i = 0; i < count; i++)
            tup->as.tuple.items[i] = lower_var(p, part_vars[i]);
        *cursor = make_con1(p, ret_ty, "Ok", tup);
        f->fun.body = arr_case;
        return;
    }

    mo_core_ctors ctors = lower_ctors(p, d->type);
    if (is_json_struct(d)) {
        bool unnamed = false;
        mo_core_string *labels = ctor_labels(p, d->type, 0, &ctors.items[0], &unnamed);
        MO_ASSERT(!unnamed);
        f->fun.body = decode_fields_object(p, d, 0, &ctors.items[0], labels, lower_var(p, val_var),
                                           path_var, strict_var);
        return;
    }

    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type int_src = {
        .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_ty int_ty = lower_type(p, &int_src, NULL);
    mo_core_ty json_ty = lower_type(p, json_data_source(p, "Json"), NULL);
    mo_core_ctors json_ctors = lower_ctors(p, json_ty);
    mo_core_string type_label = lower_text(p, d->proof->type->name);

    uint32_t *offsets = LOWER_NEW(p, uint32_t, ctors.count);
    uint32_t running = 0;
    for (size_t i = 0; i < ctors.count; i++) {
        offsets[i] = running;
        running += (uint32_t)ctors.items[i].fields.count;
    }
    MO_ASSERT(running == d->proof->part_count);

    mo_core_expr top = lower_expr(p, MO_CORE_EXPR_CASE);
    top->as.case_.expr = lower_var(p, val_var);
    top->as.case_.arms.count = json_ctors.count;
    top->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, json_ctors.count);
    for (size_t k = 0; k < json_ctors.count; k++) {
        struct mo_core_arm *jarm = &top->as.case_.arms.items[k];
        jarm->ctor = json_ctors.items[k].name;
        size_t jcount = json_ctors.items[k].fields.count;
        jarm->names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, jcount), jcount};
        for (size_t m = 0; m < jcount; m++)
            jarm->names.items[m] = lower_fresh(p);
        if (lower_same(jarm->ctor, lower_text(p, "String"))) {
            mo_core_string str_var = jarm->names.items[0];
            mo_core_expr branch = bad_enum_case(p, ret_ty, val_var, path_var, type_label);
            for (size_t i = ctors.count; i != 0; i--) {
                if (ctors.items[i - 1].fields.count != 0)
                    continue;
                mo_core_string tag = snake_string(p, ctors.items[i - 1].name);
                mo_core_expr_list eq_args = args(p, 2);
                eq_args.items[0] = lower_var(p, str_var);
                eq_args.items[1] = text(p, tag);
                mo_core_expr sel = lower_expr(p, MO_CORE_EXPR_IF);
                sel->as.if_.cond = lower_prim(
                    p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = text_ty}, eq_args);
                sel->as.if_.then_ =
                    make_con1(p, ret_ty, "Ok",
                              make_con(p, d->type, ctors.items[i - 1].name.bytes, args(p, 0)));
                sel->as.if_.else_ = branch;
                branch = sel;
            }
            jarm->body = branch;
        } else if (lower_same(jarm->ctor, lower_text(p, "Object"))) {
            mo_core_string map_var = jarm->names.items[0];
            mo_core_expr branch = bad_enum_case(p, ret_ty, val_var, path_var, type_label);
            for (size_t i = ctors.count; i != 0; i--) {
                struct mo_core_ctor *ctor = &ctors.items[i - 1];
                if (ctor->fields.count == 0)
                    continue;
                mo_core_string tag = snake_string(p, ctor->name);
                bool unnamed = false;
                mo_core_string *labels = ctor_labels(p, d->type, i - 1, ctor, &unnamed);

                mo_core_expr_list get_args = args(p, 2);
                get_args.items[0] = lower_var(p, map_var);
                get_args.items[1] = text(p, tag);
                mo_core_expr get_case = lower_expr(p, MO_CORE_EXPR_CASE);
                get_case->as.case_.expr = lower_prim(
                    p,
                    (struct mo_core_prim){
                        .tag = MO_CORE_PRIM_MAP,
                        .as.map = {.k = text_ty, .v = json_ty, .op.tag = MO_CORE_MAP_OP_GET}},
                    get_args);
                get_case->as.case_.arms.count = 2;
                get_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
                get_case->as.case_.arms.items[0].ctor = lower_text(p, "None");
                get_case->as.case_.arms.items[0].body = branch;

                mo_core_string inner_var = lower_fresh(p), inner_path = lower_fresh(p);
                get_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
                get_case->as.case_.arms.items[1].names =
                    (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
                get_case->as.case_.arms.items[1].names.items[0] = inner_var;

                mo_core_expr payload_body = NULL;
                if (unnamed) {
                    mo_core_expr dec_case = lower_expr(p, MO_CORE_EXPR_CASE);
                    dec_case->as.case_.expr =
                        part_from_json(p, d, offsets[i - 1], lower_var(p, inner_var),
                                       lower_var(p, inner_path), lower_var(p, strict_var));
                    dec_case->as.case_.arms.count = 2;
                    dec_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
                    mo_core_string ok_v = lower_fresh(p), err_v = lower_fresh(p);
                    dec_case->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
                    dec_case->as.case_.arms.items[0].names =
                        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
                    dec_case->as.case_.arms.items[0].names.items[0] = ok_v;
                    dec_case->as.case_.arms.items[0].body =
                        make_con1(p, ret_ty, "Ok",
                                  make_con1(p, d->type, ctor->name.bytes, lower_var(p, ok_v)));
                    dec_case->as.case_.arms.items[1].ctor = lower_text(p, "Error");
                    dec_case->as.case_.arms.items[1].names =
                        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
                    dec_case->as.case_.arms.items[1].names.items[0] = err_v;
                    dec_case->as.case_.arms.items[1].body =
                        make_con1(p, ret_ty, "Error", lower_var(p, err_v));
                    payload_body = dec_case;
                } else {
                    payload_body =
                        decode_fields_object(p, d, offsets[i - 1], ctor, labels,
                                             lower_var(p, inner_var), inner_path, strict_var);
                }
                get_case->as.case_.arms.items[1].body = make_let(
                    p, inner_path, json_call2(p, "join_path", lower_var(p, path_var), text(p, tag)),
                    payload_body);
                branch = get_case;
            }
            mo_core_expr_list sz_arg = args(p, 1);
            sz_arg.items[0] = lower_var(p, map_var);
            mo_core_expr_list eq_args = args(p, 2);
            eq_args.items[0] = lower_prim(
                p,
                (struct mo_core_prim){
                    .tag = MO_CORE_PRIM_MAP,
                    .as.map = {.k = text_ty, .v = json_ty, .op.tag = MO_CORE_MAP_OP_SIZE}},
                sz_arg);
            eq_args.items[1] = lower_int(p, 1);
            mo_core_expr size_gate = lower_expr(p, MO_CORE_EXPR_IF);
            size_gate->as.if_.cond = lower_prim(
                p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = int_ty}, eq_args);
            size_gate->as.if_.then_ = branch;
            size_gate->as.if_.else_ = bad_enum_case(p, ret_ty, val_var, path_var, type_label);
            jarm->body = size_gate;
        } else {
            jarm->body = bad_enum_case(p, ret_ty, val_var, path_var, type_label);
        }
    }
    f->fun.body = top;
}

static mo_core_expr schema_fields_object(struct lowering *p, struct derived *d, uint32_t first,
                                         const struct mo_core_ctor *ctor,
                                         const mo_core_string *labels, mo_core_string seen_var) {
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_ty json_ty = d->function->fun.ret;
    size_t count = ctor->fields.count, req_count = 0;
    mo_core_expr_list map_items = args(p, 2 * count);
    for (size_t j = 0; j < count; j++) {
        map_items.items[2 * j] = text(p, labels[j]);
        map_items.items[2 * j + 1] =
            part_json_schema(p, d, first + (uint32_t)j, lower_var(p, seen_var));
        if (ctor->fields.items[j].ty->tag != MO_CORE_TY_OPTION)
            req_count++;
    }
    mo_core_expr props_map =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                         .as.map = {.k = text_ty,
                                                    .v = json_ty,
                                                    .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                           .lit.digits = lower_number(p, count)}}},
                   map_items);
    mo_core_expr_list req_items = args(p, req_count);
    size_t at = 0;
    for (size_t j = 0; j < count; j++)
        if (ctor->fields.items[j].ty->tag != MO_CORE_TY_OPTION)
            req_items.items[at++] = text(p, labels[j]);
    mo_core_expr req_list = lower_prim(
        p,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = text_ty,
                                          .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                 .as.lit.digits = lower_number(p, req_count)}}},
        req_items);
    return json_call2(p, "schema_object", props_map, req_list);
}

static void derive_json_schema(struct lowering *p, struct derived *d) {
    p->span = d->span;
    struct instance *f = d->function;
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    mo_core_ty text_ty = lower_type(p, &text_src, NULL);
    mo_core_ty json_ty = f->fun.ret;
    mo_core_string seen_var = f->fun.params.items[0].name;
    mo_core_expr *body_out = &f->fun.body;

    if (d->type->tag == MO_CORE_TY_DATA) {
        mo_core_string full_key = lower_type_name(p, d->type);
        mo_core_string short_name = lower_text(p, d->proof->type->name);
        mo_core_expr_list has_args = args(p, 2);
        has_args.items[0] = lower_var(p, seen_var);
        has_args.items[1] = text(p, full_key);
        mo_core_expr guard = lower_expr(p, MO_CORE_EXPR_IF);
        guard->as.if_.cond = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                  .as.list = {.t = text_ty, .op.tag = MO_CORE_LIST_OP_CONTAINS}},
            has_args);
        guard->as.if_.then_ = json_call1(p, "schema_ref", text(p, short_name));
        mo_core_string next_seen = lower_fresh(p);
        mo_core_expr_list app_args = args(p, 2);
        app_args.items[0] = lower_var(p, seen_var);
        app_args.items[1] = text(p, full_key);
        mo_core_expr push_seen =
            make_let(p, next_seen,
                     lower_prim(p,
                                (struct mo_core_prim){
                                    .tag = MO_CORE_PRIM_LIST,
                                    .as.list = {.t = text_ty, .op.tag = MO_CORE_LIST_OP_APPEND}},
                                app_args),
                     NULL);
        guard->as.if_.else_ = push_seen;
        *body_out = guard;
        body_out = &push_seen->as.let_.body;
        seen_var = next_seen;
    }

    if (d->type->tag == MO_CORE_TY_LIST || d->type->tag == MO_CORE_TY_SET) {
        *body_out =
            json_call1(p, "schema_array", part_json_schema(p, d, 0, lower_var(p, seen_var)));
        return;
    }
    if (d->type->tag == MO_CORE_TY_MAP) {
        *body_out = json_call1(p, "schema_map", part_json_schema(p, d, 1, lower_var(p, seen_var)));
        return;
    }
    if (d->type->tag == MO_CORE_TY_OPTION) {
        *body_out =
            json_call1(p, "schema_nullable", part_json_schema(p, d, 0, lower_var(p, seen_var)));
        return;
    }
    if (d->type->tag == MO_CORE_TY_TUPLE) {
        size_t count = d->type->as.tuple.count;
        mo_core_expr_list items = args(p, count);
        for (size_t i = 0; i < count; i++)
            items.items[i] = part_json_schema(p, d, (uint32_t)i, lower_var(p, seen_var));
        mo_core_expr lst = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                  .as.list = {.t = json_ty,
                                              .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                     .as.lit.digits = lower_number(p, count)}}},
            items);
        *body_out = json_call1(p, "schema_tuple", lst);
        return;
    }

    mo_core_ctors ctors = lower_ctors(p, d->type);
    if (is_json_struct(d)) {
        bool unnamed = false;
        mo_core_string *labels = ctor_labels(p, d->type, 0, &ctors.items[0], &unnamed);
        MO_ASSERT(!unnamed);
        *body_out = schema_fields_object(p, d, 0, &ctors.items[0], labels, seen_var);
        return;
    }

    bool all_unit = true;
    for (size_t i = 0; i < ctors.count; i++)
        if (ctors.items[i].fields.count != 0)
            all_unit = false;
    if (all_unit) {
        mo_core_expr_list tags = args(p, ctors.count);
        for (size_t i = 0; i < ctors.count; i++)
            tags.items[i] = text(p, snake_string(p, ctors.items[i].name));
        mo_core_expr tag_list =
            lower_prim(p,
                       (struct mo_core_prim){
                           .tag = MO_CORE_PRIM_LIST,
                           .as.list = {.t = text_ty,
                                       .op = {.tag = MO_CORE_LIST_OP_LIT,
                                              .as.lit.digits = lower_number(p, ctors.count)}}},
                       tags);
        *body_out = json_call1(p, "schema_enum_strings", tag_list);
        return;
    }

    mo_core_expr_list variants = args(p, ctors.count);
    uint32_t first = 0;
    for (size_t i = 0; i < ctors.count; i++) {
        struct mo_core_ctor *ctor = &ctors.items[i];
        mo_core_string tag = snake_string(p, ctor->name);
        if (ctor->fields.count == 0) {
            mo_core_expr_list one = args(p, 1);
            one.items[0] = text(p, tag);
            mo_core_expr one_list = lower_prim(
                p,
                (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                      .as.list = {.t = text_ty,
                                                  .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                         .as.lit.digits = lower_text(p, "1")}}},
                one);
            variants.items[i] = json_call1(p, "schema_enum_strings", one_list);
        } else {
            bool unnamed = false;
            mo_core_string *labels = ctor_labels(p, d->type, i, ctor, &unnamed);
            mo_core_expr inner = unnamed
                                     ? part_json_schema(p, d, first, lower_var(p, seen_var))
                                     : schema_fields_object(p, d, first, ctor, labels, seen_var);
            variants.items[i] = json_call2(p, "schema_tagged_case", text(p, tag), inner);
        }
        first += (uint32_t)ctor->fields.count;
    }
    MO_ASSERT(first == d->proof->part_count);
    mo_core_expr var_list = lower_prim(
        p,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = json_ty,
                                          .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                 .as.lit.digits = lower_number(p, ctors.count)}}},
        variants);
    *body_out = json_call1(p, "schema_any_of", var_list);
}

void lower_derived_functions(struct lowering *p) {
    /* Building a body can append children, including recursive self-edges
     * resolved by the reserved function. Each pending body runs once. */
    while (p->derived_pending != NULL) {
        lower_work(p, 1);
        struct derived *d = p->derived_pending;
        switch (d->kind) {
        case DERIVED_TEXT:
            derive_text(p, d);
            break;
        case DERIVED_ORDER:
            derive_order(p, d);
            break;
        case DERIVED_TO_JSON:
            derive_to_json(p, d);
            break;
        case DERIVED_FROM_JSON:
            derive_from_json(p, d);
            break;
        case DERIVED_JSON_SCHEMA:
            derive_json_schema(p, d);
            break;
        }
        p->derived_pending = d->next;
    }
}
