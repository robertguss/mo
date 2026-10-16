#include "lower/internal.h"

#include <stdlib.h>

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

static void body(struct lowering *p, mo_core_expr expr, struct mo_core_body_meta *meta,
                 struct frame *stack) {
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

struct data_order {
    struct data_instance *instance;
    uint32_t index;
};
static int data_order(const void *a, const void *b) {
    const struct data_order *left = a, *right = b;
    const struct data_instance *x = left->instance, *y = right->instance;
    if (x->owner != y->owner)
        return (x->owner > y->owner) - (x->owner < y->owner);
    uint32_t xs = x->source->declaration->declaration->span.start;
    uint32_t ys = y->source->declaration->declaration->span.start;
    return xs != ys ? (xs > ys) - (xs < ys)
                    : (left->index > right->index) - (left->index < right->index);
}

struct constant_order {
    const struct mo_symbol *symbol;
    uint32_t index;
};
static int constant_order(const void *a, const void *b) {
    const struct constant_order *x = a, *y = b;
    if (x->symbol->module != y->symbol->module)
        return x->symbol->module < y->symbol->module ? -1 : 1;
    uint32_t xs = x->symbol->declaration->span.start, ys = y->symbol->declaration->span.start;
    return (xs > ys) - (xs < ys);
}

static int function_order(const void *a, const void *b) {
    const struct instance *const *left = a, *const *right = b;
    const struct instance *x = *left, *y = *right;
    if (x->owner != y->owner)
        return (x->owner > y->owner) - (x->owner < y->owner);
    bool x_generated = x->source == NULL && x->spec == NULL;
    bool y_generated = y->source == NULL && y->spec == NULL;
    if (x_generated != y_generated)
        return x_generated ? 1 : -1;
    if (!x_generated) {
        uint32_t xm = x->source == NULL ? x->spec->module : x->source->signature->function->module;
        uint32_t ym = y->source == NULL ? y->spec->module : y->source->signature->function->module;
        if (xm != ym)
            return (xm > ym) - (xm < ym);
        uint32_t xs = x->meta.span.value.start, ys = y->meta.span.value.start;
        if (xs != ys)
            return (xs > ys) - (xs < ys);
    }
    return (x->index > y->index) - (x->index < y->index);
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
    struct frame *stack = LOWER_NEW(p, struct frame, MO_LIMIT_CORE_DEPTH);
    for (struct instance *f = p->functions; f != NULL; f = f->next) {
        body(p, f->fun.body, &f->meta.body, stack);
        if (f->meta.body.spans.count > MO_LIMIT_CORE_NODES - nodes)
            lower_refuse(p, MO_CODE_LOWER_WORK, "lowered document exceeds the Core node limit");
        nodes += f->meta.body.spans.count;
        m->funs.items[i++] = f->meta;
    }
    m->consts.count = core->consts.count;
    m->consts.items = LOWER_NEW(p, struct mo_core_const_meta, m->consts.count);
    for (i = 0; i < core->consts.count; i++) {
        struct instance *f = &p->constant_bodies[i];
        body(p, f->fun.body, &f->meta.body, stack);
        if (f->meta.body.spans.count > MO_LIMIT_CORE_NODES - nodes)
            lower_refuse(p, MO_CODE_LOWER_WORK, "lowered document exceeds the Core node limit");
        nodes += f->meta.body.spans.count;
        m->consts.items[i] = (struct mo_core_const_meta){f->meta.name, f->meta.span, f->meta.body};
    }
    m->modules.count = p->program->set->count;
    m->modules.items = LOWER_NEW(p, struct mo_core_module_meta, m->modules.count);
    struct cursor {
        size_t nf, ef, nd, ed, nc;
    };
    struct cursor *cursors = LOWER_NEW(p, struct cursor, m->modules.count);
    for (struct instance *f = p->functions; f != NULL; f = f->next) {
        lower_work(p, 1);
        struct mo_core_module_meta *out = &m->modules.items[f->owner];
        out->funs.count++;
        if (f->source != NULL && f->source->signature->function->module == f->owner &&
            f->source->signature->function->exported)
            out->exposed_funs.count++;
    }
    struct data_order *data = LOWER_NEW(p, struct data_order, p->data_count);
    uint32_t index = 0;
    for (struct data_instance *d = p->data; d != NULL; d = d->next) {
        lower_work(p, 1);
        data[index] = (struct data_order){d, index};
        index++;
        struct mo_core_module_meta *out = &m->modules.items[d->owner];
        out->data.count++;
        if (d->source->declaration->exported)
            out->exposed_types.count++;
    }
    for (uint32_t n = p->data_count; n != 0; n /= 2)
        lower_work(p, p->data_count);
    qsort(data, p->data_count, sizeof *data, data_order);
    struct constant_order *constants = LOWER_NEW(p, struct constant_order, p->constants.count);
    for (uint32_t j = 0; j < p->constants.count; j++) {
        lower_work(p, 1);
        const struct mo_symbol *symbol = p->program->constants[j].symbol;
        constants[j] = (struct constant_order){symbol, j};
        m->modules.items[symbol->module].consts.count++;
    }
    for (size_t n = p->constants.count; n != 0; n /= 2)
        lower_work(p, p->constants.count);
    qsort(constants, p->constants.count, sizeof *constants, constant_order);
    for (uint32_t module = 0; module < m->modules.count; module++) {
        struct mo_core_module_meta *out = &m->modules.items[module];
        out->name = lower_text(p, p->program->set->names[module]);
        out->funs.items = LOWER_NEW(p, uint32_t, out->funs.count);
        out->data.items = LOWER_NEW(p, uint32_t, out->data.count);
        out->consts.items = LOWER_NEW(p, uint32_t, out->consts.count);
        out->exposed_funs.items = LOWER_NEW(p, uint32_t, out->exposed_funs.count);
        out->exposed_types.items =
            LOWER_NEW(p, struct mo_core_exposed_type, out->exposed_types.count);
    }
    /* Specs can discover new instances after their anchors. Sort ownership
     * independently of the stable call/link indices, grouping each source
     * declaration's instances before the next declaration. Helpers come last. */
    struct instance **functions = LOWER_NEW(p, struct instance *, p->function_count);
    uint32_t function = 0;
    for (struct instance *f = p->functions; f != NULL; f = f->next) {
        lower_work(p, 1);
        functions[function++] = f;
    }
    for (uint32_t n = p->function_count; n != 0; n /= 2)
        lower_work(p, p->function_count);
    qsort(functions, p->function_count, sizeof *functions, function_order);
    for (uint32_t j = 0; j < p->function_count; j++) {
        lower_work(p, 1);
        struct instance *f = functions[j];
        struct mo_core_module_meta *out = &m->modules.items[f->owner];
        struct cursor *at = &cursors[f->owner];
        out->funs.items[at->nf++] = f->index;
        if (f->source != NULL && f->source->signature->function->module == f->owner &&
            f->source->signature->function->exported)
            out->exposed_funs.items[at->ef++] = f->index;
    }
    for (uint32_t j = 0; j < p->data_count; j++) {
        lower_work(p, 1);
        struct data_instance *d = data[j].instance;
        struct mo_core_module_meta *out = &m->modules.items[d->owner];
        struct cursor *at = &cursors[d->owner];
        out->data.items[at->nd++] = data[j].index;
        if (d->source->declaration->exported)
            out->exposed_types.items[at->ed++] =
                (struct mo_core_exposed_type){data[j].index, d->source->declaration->insides};
    }
    for (size_t j = 0; j < p->constants.count; j++) {
        lower_work(p, 1);
        uint32_t module = constants[j].symbol->module;
        m->modules.items[module].consts.items[cursors[module].nc++] = constants[j].index;
    }
}
