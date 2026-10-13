#include "lower/internal.h"

struct frame {
    mo_core_expr expr;
    size_t child;
    bool entered;
};
mo_core_expr lower_child(mo_core_expr e, size_t at) {
    switch (e->tag) {
    case MO_CORE_EXPR_LIT:
    case MO_CORE_EXPR_VAR:
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_STOP:
        return NULL;
    case MO_CORE_EXPR_LET:
        return at == 0 ? e->as.let_.bound : at == 1 ? e->as.let_.body : NULL;
    case MO_CORE_EXPR_LAM:
        return at == 0 ? e->as.lam.body : NULL;
    case MO_CORE_EXPR_APP:
        return at == 0                      ? e->as.app.f
               : at <= e->as.app.args.count ? e->as.app.args.items[at - 1]
                                            : NULL;
    case MO_CORE_EXPR_CALL:
        return at < e->as.call.args.count ? e->as.call.args.items[at] : NULL;
    case MO_CORE_EXPR_TUPLE:
        return at < e->as.tuple.count ? e->as.tuple.items[at] : NULL;
    case MO_CORE_EXPR_PROJ:
        return at == 0 ? e->as.proj.expr : NULL;
    case MO_CORE_EXPR_CON:
        return at < e->as.con.args.count ? e->as.con.args.items[at] : NULL;
    case MO_CORE_EXPR_CASE:
        return at == 0                        ? e->as.case_.expr
               : at <= e->as.case_.arms.count ? e->as.case_.arms.items[at - 1].body
                                              : NULL;
    case MO_CORE_EXPR_IF:
        return at == 0   ? e->as.if_.cond
               : at == 1 ? e->as.if_.then_
               : at == 2 ? e->as.if_.else_
                         : NULL;
    case MO_CORE_EXPR_PRIM:
        return at < e->as.prim.args.count ? e->as.prim.args.items[at] : NULL;
    case MO_CORE_EXPR_FAIL:
        return at == 0 ? e->as.fail : NULL;
    case MO_CORE_EXPR_JOIN:
        return at == 0 ? e->as.join.body : at == 1 ? e->as.join.rest : NULL;
    case MO_CORE_EXPR_JUMP:
        return at < e->as.jump.args.count ? e->as.jump.args.items[at] : NULL;
    }
    MO_UNREACHABLE();
}

static void body(struct lowering *p, mo_core_expr expr, struct mo_core_body_meta *meta) {
    struct frame *stack = LOWER_NEW(p, struct frame, MO_LIMIT_CORE_DEPTH);
    size_t counts[3] = {0};
    /* Count unfolded occurrences, not allocation identities: shared failure
     * leaves still occupy separate wire/preorder positions. */
    for (unsigned pass = 0; pass < 2; pass++) {
        size_t depth = 1, node = 0, binding = 0, closure = 0;
        stack[0] = (struct frame){.expr = expr};
        while (depth != 0) {
            lower_work(p, 1);
            struct frame *f = &stack[depth - 1];
            MO_ASSERT(f->expr != NULL);
            struct lower_node *n = (struct lower_node *)f->expr;
            if (!f->entered) {
                p->span = n->span.value;
                if (node == MO_LIMIT_CORE_NODES)
                    lower_refuse(p, MO_CODE_LOWER_WORK, "lowered body exceeds the Core node limit");
                if (pass != 0)
                    meta->spans.items[node] = n->span;
                for (size_t i = 0; i < n->binders.count; i++) {
                    if (!n->binders.items[i].some)
                        continue;
                    if (pass != 0)
                        meta->bindings.items[binding] = (struct mo_core_binding_meta){
                            (uint32_t)node, (uint32_t)i, n->binders.items[i].value};
                    binding++;
                }
                if (f->expr->tag == MO_CORE_EXPR_LAM) {
                    MO_ASSERT(n->closure.length != 0);
                    if (pass != 0)
                        meta->closures.items[closure] =
                            (struct mo_core_closure_meta){(uint32_t)node, n->closure};
                    closure++;
                }
                node++;
                f->entered = true;
            }
            mo_core_expr next = lower_child(f->expr, f->child++);
            if (next == NULL) {
                depth--;
                continue;
            }
            if (depth == MO_LIMIT_CORE_DEPTH)
                lower_refuse(p, MO_CODE_LOWER_DEPTH, "lowered body exceeds the Core path limit");
            stack[depth++] = (struct frame){.expr = next};
        }
        if (pass == 0) {
            counts[0] = node;
            counts[1] = binding;
            counts[2] = closure;
            meta->spans = (mo_core_spans){LOWER_NEW(p, struct mo_core_span, node), node};
            meta->bindings = (mo_core_binding_metas){
                LOWER_NEW(p, struct mo_core_binding_meta, binding), binding};
            meta->closures = (mo_core_closure_metas){
                LOWER_NEW(p, struct mo_core_closure_meta, closure), closure};
        } else
            MO_ASSERT(node == counts[0] && binding == counts[1] && closure == counts[2]);
    }
}

void lower_metadata(struct lowering *p, struct mo_core *core) {
    struct mo_core_metadata *m = &core->metadata;
    m->source = true;
    m->files.count = p->sources->count;
    m->files.items = LOWER_NEW(p, mo_core_string, m->files.count);
    for (size_t i = 0; i < m->files.count; i++)
        m->files.items[i] = lower_text(p, p->sources->files[i].path);
    m->funs.count = core->funs.count;
    m->funs.items = LOWER_NEW(p, struct mo_core_fun_meta, m->funs.count);
    size_t i = 0;
    size_t nodes = p->type_count;
    for (struct instance *f = p->functions; f != NULL; f = f->next) {
        body(p, f->fun.body, &f->meta.body);
        if (f->meta.body.spans.count > MO_LIMIT_CORE_NODES - nodes)
            lower_refuse(p, MO_CODE_LOWER_WORK, "lowered document exceeds the Core node limit");
        nodes += f->meta.body.spans.count;
        m->funs.items[i++] = f->meta;
    }
    m->modules.count = p->program->set->count;
    m->modules.items = LOWER_NEW(p, struct mo_core_module_meta, m->modules.count);
    for (uint32_t module = 0; module < m->modules.count; module++) {
        struct mo_core_module_meta *out = &m->modules.items[module];
        out->name = lower_text(p, p->program->set->names[module]);
        size_t nf = 0, nd = 0, ef = 0, ed = 0;
        for (struct instance *f = p->functions; f != NULL; f = f->next) {
            lower_work(p, 1);
            if (f->owner != module)
                continue;
            nf++;
            if (f->source != NULL && f->source->signature->function->module == module &&
                f->source->signature->function->exported)
                ef++;
        }
        for (struct data_instance *d = p->data; d != NULL; d = d->next) {
            lower_work(p, 1);
            if (d->owner != module)
                continue;
            nd++;
            if (d->source->module == module && d->source->declaration->exported)
                ed++;
        }
        out->funs.items = LOWER_NEW(p, uint32_t, nf);
        out->funs.count = nf;
        out->data.items = LOWER_NEW(p, uint32_t, nd);
        out->data.count = nd;
        out->exposed_funs.items = LOWER_NEW(p, uint32_t, ef);
        out->exposed_funs.count = ef;
        out->exposed_types.items = LOWER_NEW(p, struct mo_core_exposed_type, ed);
        out->exposed_types.count = ed;
        nf = ef = nd = ed = 0;
        /* Declaration/source order, not the work-list's discovery order. */
        for (uint32_t source = 0; source < p->program->function_count; source++) {
            for (struct instance *f = p->functions; f != NULL; f = f->next) {
                lower_work(p, 1);
                if (f->owner != module || f->source != &p->program->functions[source])
                    continue;
                out->funs.items[nf++] = f->index;
                if (f->source->signature->function->module == module &&
                    f->source->signature->function->exported)
                    out->exposed_funs.items[ef++] = f->index;
            }
        }
        for (struct instance *f = p->functions; f != NULL; f = f->next)
            if (f->owner == module && f->source == NULL)
                out->funs.items[nf++] = f->index;
        const struct mo_syntax *items = p->program->set->modules[module]->tree->children[1];
        for (uint32_t declaration = 0; declaration < items->count; declaration++) {
            uint32_t index = 0;
            for (struct data_instance *d = p->data; d != NULL; d = d->next, index++) {
                lower_work(p, 1);
                if (d->owner != module ||
                    d->source->declaration->declaration != items->children[declaration])
                    continue;
                out->data.items[nd++] = index;
                if (d->source->module == module && d->source->declaration->exported)
                    out->exposed_types.items[ed++] =
                        (struct mo_core_exposed_type){index, d->source->declaration->insides};
            }
        }
        MO_ASSERT(nf == out->funs.count && ef == out->exposed_funs.count && nd == out->data.count &&
                  ed == out->exposed_types.count);
    }
}
