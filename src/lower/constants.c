#include "lower/internal.h"

#include <stdlib.h>
#include <string.h>

mo_core_string lower_global_name(struct lowering *p, const struct mo_symbol *symbol) {
    return lower_concat(p, lower_text(p, p->program->set->names[symbol->module]), ".",
                        lower_text(p, symbol->name));
}

void lower_constants(struct lowering *p) {
    uint32_t count = p->program->constant_count;
    p->constants.count = count;
    p->constants.items = LOWER_NEW(p, struct mo_core_const, count);
    p->constant_bodies = LOWER_NEW(p, struct instance, count);
    for (uint32_t i = 0; i < count; i++) {
        lower_work(p, 1);
        const struct mo_checked_constant *source = &p->program->constants[i];
        p->span = source->symbol->declaration->span;
        struct instance *f = &p->constant_bodies[i];
        f->owner = source->symbol->module;
        f->meta.name = lower_global_name(p, source->symbol);
        f->meta.span = lower_span(p, p->span);
        f->fun.ret = lower_type(p, source->type, NULL);
        lower_body(p, f, &source->initializer, 0, NULL);
        p->constants.items[i] = (struct mo_core_const){f->meta.name, f->fun.ret, f->fun.body};
    }
}

static int name_order(const void *a, const void *b) {
    const struct instance *const *left = a, *const *right = b;
    mo_core_string x = (*left)->fun.name, y = (*right)->fun.name;
    size_t length = x.length < y.length ? x.length : y.length;
    int order = memcmp(x.bytes, y.bytes, length);
    return order != 0 ? order : (x.length > y.length) - (x.length < y.length);
}

struct prefix_walk {
    struct instance **index, **queue;
    bool *queued;
    uint32_t head, tail, count;
    struct frame {
        mo_core_expr expr;
        size_t child;
    } * stack;
};

static void restrict_calls(struct lowering *p, struct prefix_walk *w, mo_core_expr body,
                           uint32_t prefix) {
    size_t depth = 1;
    w->stack[0] = (struct frame){body, 0};
    while (depth != 0) {
        lower_work(p, 1);
        struct frame *frame = &w->stack[depth - 1];
        mo_core_expr e = frame->expr;
        if (frame->child == 0 && e->tag == MO_CORE_EXPR_CALL) {
            uint32_t lo = 0, hi = p->function_count;
            struct instance key = {.fun.name = e->as.call.name}, *pointer = &key;
            while (lo < hi) {
                lower_work(p, 1 + e->as.call.name.length);
                uint32_t mid = lo + (hi - lo) / 2;
                if (name_order(&w->index[mid], &pointer) < 0)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            MO_ASSERT(lo < p->function_count &&
                      lower_same(w->index[lo]->fun.name, e->as.call.name));
            struct instance *callee = w->index[lo];
            if (prefix < callee->const_prefix) {
                callee->const_prefix = prefix;
                if (!w->queued[callee->index]) {
                    MO_ASSERT(w->count < p->function_count);
                    w->queue[w->tail] = callee;
                    w->tail = (w->tail + 1) % p->function_count;
                    w->count++;
                    w->queued[callee->index] = true;
                }
            }
        }
        mo_core_expr next = lower_child(e, frame->child++);
        if (next == NULL) {
            depth--;
            continue;
        }
        if (depth == MO_LIMIT_CORE_DEPTH)
            lower_refuse(p, MO_CODE_LOWER_DEPTH, "lowered body exceeds the Core path limit");
        w->stack[depth++] = (struct frame){next, 0};
    }
}

void lower_constant_prefixes(struct lowering *p) {
    uint32_t count = p->program->constant_count;
    if (count == 0)
        return;
    struct prefix_walk w = {.index = LOWER_NEW(p, struct instance *, p->function_count),
                            .queue = LOWER_NEW(p, struct instance *, p->function_count),
                            .queued = LOWER_NEW(p, bool, p->function_count),
                            .stack = LOWER_NEW(p, struct frame, MO_LIMIT_CORE_DEPTH)};
    for (struct instance *f = p->functions; f != NULL; f = f->next) {
        lower_work(p, 1);
        f->const_prefix = count;
        w.index[f->index] = f;
        for (uint32_t n = p->function_count; n != 0; n /= 2)
            lower_work(p, 1 + f->fun.name.length);
    }
    qsort(w.index, p->function_count, sizeof *w.index, name_order);
    /* Ordinary invocations have the full prefix. Any function reachable while
     * initializing constant i must also work at prefix i. Tighten to the
     * earliest such caller and propagate through recursive calls/closures. */
    for (uint32_t i = 0; i < count; i++)
        restrict_calls(p, &w, p->constants.items[i].body, i);
    while (w.count != 0) {
        struct instance *f = w.queue[w.head];
        w.head = (w.head + 1) % p->function_count;
        w.count--;
        w.queued[f->index] = false;
        restrict_calls(p, &w, f->fun.body, f->const_prefix);
    }
    for (struct instance *f = p->functions; f != NULL; f = f->next)
        f->fun.const_count.digits = lower_number(p, f->const_prefix);
}
