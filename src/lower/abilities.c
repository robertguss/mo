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

static uint32_t bucket(mo_core_ty type, uint32_t owner, uint32_t slots) {
    uint64_t key = (uintptr_t)type ^ ((uint64_t)owner << 32);
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
    mo_core_expr converted =
        lower_to_text(p, proof, d->substitution, d->function->owner, d->function->depth, value);
    mo_core_ty type = lower_type(p, proof->type, d->substitution);
    if (proof->kind == MO_ABILITY_NATIVE &&
        (type->tag == MO_CORE_TY_TEXT || type->tag == MO_CORE_TY_CHAR))
        converted = quote(p, converted, type->tag == MO_CORE_TY_CHAR);
    return converted;
}

mo_core_expr lower_derived_text(struct lowering *p, const struct mo_ability_implementation *proof,
                                const struct substitution *substitution, uint32_t owner,
                                uint32_t depth, mo_core_expr value) {
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
            uint32_t at = bucket(item->type, item->function->owner, slots);
            item->hash_next = table[at];
            table[at] = item;
        }
        p->derived_index = table;
        p->derived_slots = slots;
    }
    uint32_t at = bucket(type, owner, p->derived_slots);
    struct derived *d;
    for (d = p->derived_index[at]; d != NULL; d = d->hash_next) {
        lower_work(p, 1);
        if (d->type == type && d->function->owner == owner)
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
        f->fun.name =
            lower_concat(p, f->fun.module, ".%to_text(",
                         lower_concat(p, lower_type_name(p, type), "", lower_text(p, ")")));
        f->fun.const_count.digits = lower_text(p, "0");
        static const struct mo_source_type text_type = {
            .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
        f->fun.ret = lower_type(p, &text_type, NULL);
        f->fun.params.count = 1;
        f->fun.params.items = LOWER_NEW(p, struct mo_core_param, 1);
        f->fun.params.items[0] = (struct mo_core_param){lower_fresh(p), type};
        f->meta.body.params.count = 1;
        f->meta.body.params.items = LOWER_NEW(p, struct mo_core_name, 1);
        f->meta.name = f->fun.name;
        f->meta.span = lower_span(p, p->span);
    }
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = d->function->fun.name;
    call->as.call.args = args(p, 1);
    call->as.call.args.items[0] = value;
    return call;
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

static void derive_text(struct lowering *p, struct derived *d) {
    p->span = d->span;
    struct instance *f = d->function;
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
        derive_text(p, d);
        p->derived_pending = d->next;
    }
}
