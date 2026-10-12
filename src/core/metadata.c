#include "core/core.h"

#include <string.h>

#include "base/limits.h"

static MO_MUST_USE bool name_valid(mo_core_string s) {
    return s.length != 0 && s.bytes != NULL && memchr(s.bytes, 0, s.length) == NULL;
}
static MO_MUST_USE bool same_name(mo_core_string a, mo_core_string b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.bytes, b.bytes, a.length) == 0);
}
static MO_MUST_USE int name_order(mo_core_string a, mo_core_string b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int order = n == 0 ? 0 : memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}
/* Bottom-up merge sorting gives a worst-case bound even for hostile common-prefix names. */
static MO_MUST_USE bool module_names_valid(mo_core_module_metas modules, struct mo_arena *scratch) {
    mo_core_string *names =
        mo_arena_allocate_array(scratch, modules.count, sizeof *names, _Alignof(mo_core_string));
    mo_core_string *temp =
        mo_arena_allocate_array(scratch, modules.count, sizeof *temp, _Alignof(mo_core_string));
    if (names == NULL || temp == NULL)
        return false;
    for (size_t i = 0; i < modules.count; i++) {
        names[i] = modules.items[i].name;
        if (!name_valid(names[i]))
            return false;
    }
    for (size_t width = 1; width < modules.count; width *= 2) {
        for (size_t start = 0; start < modules.count; start += 2 * width) {
            size_t middle = start + width < modules.count ? start + width : modules.count;
            size_t end = start + 2 * width < modules.count ? start + 2 * width : modules.count;
            size_t a = start, b = middle;
            for (size_t i = start; i < end; i++)
                temp[i] = a < middle && (b == end || name_order(names[a], names[b]) <= 0)
                              ? names[a++]
                              : names[b++];
        }
        mo_core_string *swap = names;
        names = temp;
        temp = swap;
    }
    for (size_t i = 1; i < modules.count; i++)
        if (same_name(names[i - 1], names[i]))
            return false;
    return true;
}
static MO_MUST_USE bool span_valid(struct mo_core_span s, size_t files) {
    return !s.some || (s.value.file < files && s.value.start <= s.value.end);
}

struct body_walk {
    const struct mo_core_body_meta *meta;
    size_t node, binding, closure, files;
};
static MO_MUST_USE bool walk_expr(struct body_walk *w, mo_core_expr e, size_t depth);
static MO_MUST_USE bool walk_args(struct body_walk *w, mo_core_expr_list xs, size_t depth) {
    for (size_t i = 0; i < xs.count; i++)
        if (!walk_expr(w, xs.items[i], depth))
            return false;
    return true;
}
static MO_MUST_USE bool walk_expr(struct body_walk *w, mo_core_expr e, size_t depth) {
    if (e == NULL || depth >= MO_LIMIT_CORE_DEPTH || w->node >= w->meta->spans.count ||
        !span_valid(w->meta->spans.items[w->node], w->files))
        return false;
    size_t node = w->node++;
    size_t slots = 0;
    if (e->tag == MO_CORE_EXPR_LET)
        slots = 1;
    else if (e->tag == MO_CORE_EXPR_LAM)
        slots = e->as.lam.params.count;
    else if (e->tag == MO_CORE_EXPR_JOIN)
        slots = e->as.join.params.count;
    else if (e->tag == MO_CORE_EXPR_CASE)
        for (size_t i = 0; i < e->as.case_.arms.count; i++)
            slots += e->as.case_.arms.items[i].names.count;
    while (w->binding < w->meta->bindings.count &&
           w->meta->bindings.items[w->binding].node == node) {
        const struct mo_core_binding_meta *b = &w->meta->bindings.items[w->binding++];
        if (b->slot >= slots || !name_valid(b->name))
            return false;
    }
    if (e->tag == MO_CORE_EXPR_LAM) {
        if (w->closure == w->meta->closures.count)
            return false;
        const struct mo_core_closure_meta *c = &w->meta->closures.items[w->closure++];
        if (c->node != node || !name_valid(c->name))
            return false;
    }
    switch (e->tag) {
    case MO_CORE_EXPR_LIT:
    case MO_CORE_EXPR_VAR:
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_STOP:
        return true;
    case MO_CORE_EXPR_LET:
        return walk_expr(w, e->as.let_.bound, depth + 1) &&
               walk_expr(w, e->as.let_.body, depth + 1);
    case MO_CORE_EXPR_LAM:
        return walk_expr(w, e->as.lam.body, depth + 1);
    case MO_CORE_EXPR_APP:
        return walk_expr(w, e->as.app.f, depth + 1) && walk_args(w, e->as.app.args, depth + 1);
    case MO_CORE_EXPR_CALL:
        return walk_args(w, e->as.call.args, depth + 1);
    case MO_CORE_EXPR_TUPLE:
        return walk_args(w, e->as.tuple, depth + 1);
    case MO_CORE_EXPR_PROJ:
        return walk_expr(w, e->as.proj.expr, depth + 1);
    case MO_CORE_EXPR_CON:
        return walk_args(w, e->as.con.args, depth + 1);
    case MO_CORE_EXPR_CASE:
        if (!walk_expr(w, e->as.case_.expr, depth + 1))
            return false;
        for (size_t i = 0; i < e->as.case_.arms.count; i++)
            if (!walk_expr(w, e->as.case_.arms.items[i].body, depth + 1))
                return false;
        return true;
    case MO_CORE_EXPR_IF:
        return walk_expr(w, e->as.if_.cond, depth + 1) &&
               walk_expr(w, e->as.if_.then_, depth + 1) && walk_expr(w, e->as.if_.else_, depth + 1);
    case MO_CORE_EXPR_PRIM:
        return walk_args(w, e->as.prim.args, depth + 1);
    case MO_CORE_EXPR_FAIL:
        return walk_expr(w, e->as.fail, depth + 1);
    case MO_CORE_EXPR_JOIN:
        return walk_expr(w, e->as.join.body, depth + 1) && walk_expr(w, e->as.join.rest, depth + 1);
    case MO_CORE_EXPR_JUMP:
        return walk_args(w, e->as.jump.args, depth + 1);
    }
    return false;
}
static MO_MUST_USE bool body_valid(const struct mo_core_body_meta *b, mo_core_expr e, size_t params,
                                   size_t files) {
    if (b->params.count != params)
        return false;
    for (size_t i = 0; i < params; i++)
        if (b->params.items[i].some && !name_valid(b->params.items[i].value))
            return false;
    for (size_t i = 1; i < b->bindings.count; i++) {
        const struct mo_core_binding_meta *a = &b->bindings.items[i - 1],
                                          *c = &b->bindings.items[i];
        if (a->node > c->node || (a->node == c->node && a->slot >= c->slot))
            return false;
    }
    struct body_walk w = {.meta = b, .files = files};
    return walk_expr(&w, e, 0) && w.node == b->spans.count && w.binding == b->bindings.count &&
           w.closure == b->closures.count;
}

/* A declaration belongs to exactly one module. Bit 0 records ownership, bit 1 exposure. */
static MO_MUST_USE bool mark_members(mo_core_indices xs, uint8_t *seen, size_t count) {
    for (size_t i = 0; i < xs.count; i++) {
        uint32_t n = xs.items[i];
        if (n >= count || seen[n] != 0)
            return false;
        seen[n] = 1;
    }
    return true;
}
static MO_MUST_USE bool modules_valid(const struct mo_core *p, struct mo_arena *scratch) {
    const struct mo_core_metadata *m = &p->metadata;
    if (!module_names_valid(m->modules, scratch))
        return false;
    size_t counts[] = {p->data.count, p->funs.count, p->consts.count};
    uint8_t *seen[3];
    for (size_t i = 0; i < 3; i++) {
        if (counts[i] > MO_LIMIT_CORE_NODES)
            return false;
        seen[i] = mo_arena_allocate(scratch, counts[i] + 1, 1);
        if (seen[i] == NULL)
            return false;
        memset(seen[i], 0, counts[i] + 1);
    }
    uint32_t *data_owner =
        mo_arena_allocate_array(scratch, counts[0] + 1, sizeof *data_owner, _Alignof(uint32_t));
    if (data_owner == NULL)
        return false;
    for (size_t i = 0; i < m->modules.count; i++) {
        const struct mo_core_module_meta *mod = &m->modules.items[i];
        if (!name_valid(mod->name) || !mark_members(mod->data, seen[0], counts[0]) ||
            !mark_members(mod->funs, seen[1], counts[1]) ||
            !mark_members(mod->consts, seen[2], counts[2]))
            return false;
        for (size_t j = 0; j < mod->data.count; j++)
            data_owner[mod->data.items[j]] = (uint32_t)i;
        for (size_t j = 0; j < mod->funs.count; j++)
            if (!same_name(p->funs.items[mod->funs.items[j]].module, mod->name))
                return false;
        for (size_t j = 0; j < mod->exposed_types.count; j++) {
            uint32_t n = mod->exposed_types.items[j].index;
            if (n >= counts[0] || seen[0][n] != 1 || data_owner[n] != i)
                return false;
            seen[0][n] = 3;
        }
        for (size_t j = 0; j < mod->exposed_funs.count; j++) {
            uint32_t n = mod->exposed_funs.items[j];
            if (n >= counts[1] || seen[1][n] != 1 || p->funs.items[n].kind != MO_CORE_FUNCTION ||
                !same_name(p->funs.items[n].module, mod->name))
                return false;
            seen[1][n] = 3;
        }
    }
    for (size_t i = 0; i < 3; i++)
        for (size_t j = 0; j < counts[i]; j++)
            if (seen[i][j] == 0)
                return false;
    return true;
}
bool mo_core_metadata_valid(const struct mo_core *p) {
    MO_ASSERT(p != NULL);
    const struct mo_core_metadata *m = &p->metadata;
    if (!m->source)
        return true;
    if (m->files.count > MO_LIMIT_FILES || m->modules.count > MO_LIMIT_MODULES ||
        m->funs.count != p->funs.count || m->consts.count != p->consts.count)
        return false;
    for (size_t i = 0; i < m->files.count; i++)
        if (!name_valid(m->files.items[i]))
            return false;
    struct mo_arena scratch;
    mo_arena_init(&scratch, MO_LIMIT_CORE_SCRATCH_SIZE);
    bool ok = modules_valid(p, &scratch);
    mo_arena_free(&scratch);
    if (!ok)
        return false;
    for (size_t i = 0; i < m->consts.count; i++) {
        const struct mo_core_const_meta *c = &m->consts.items[i];
        if (!name_valid(c->name) || !span_valid(c->span, m->files.count) ||
            !body_valid(&c->body, p->consts.items[i].body, 0, m->files.count))
            return false;
    }
    for (size_t i = 0; i < m->funs.count; i++) {
        const struct mo_core_fun_meta *f = &m->funs.items[i];
        const struct mo_core_fun *decl = &p->funs.items[i];
        bool spec = decl->kind != MO_CORE_FUNCTION;
        if (!name_valid(f->name) || !span_valid(f->span, m->files.count) || f->text.some != spec ||
            !body_valid(&f->body, decl->body, decl->params.count, m->files.count) ||
            f->allows.some != (decl->kind == MO_CORE_ANCHOR) ||
            (f->lines.count != 0) != (decl->kind == MO_CORE_PROMISE))
            return false;
        for (size_t j = 0; j < f->lines.count; j++)
            if (!span_valid(f->lines.items[j].span, m->files.count))
                return false;
        bool linked = decl->kind == MO_CORE_PROMISE || decl->kind == MO_CORE_ANCHOR;
        if (f->subject.some != linked)
            return false;
        if (linked && (f->subject.value >= p->funs.count ||
                       p->funs.items[f->subject.value].kind !=
                           (decl->kind == MO_CORE_PROMISE ? MO_CORE_FUNCTION : MO_CORE_PROMISE)))
            return false;
    }
    return true;
}
