#include "lower/internal.h"

static struct instance *reserve(struct lowering *p, uint32_t index, struct instance *subject) {
    const struct mo_checked_spec *source = &p->program->specs[index];
    p->span = source->syntax->span;
    for (struct instance *f = p->specs[index]; f != NULL; f = f->source_next) {
        lower_work(p, 1);
        if (f->subject == subject)
            return f;
    }
    if (p->function_count == MO_LIMIT_CORE_NODES)
        lower_refuse(p, MO_CODE_LOWER_WORK, "lowering exceeds its spec function limit");
    struct instance *f = LOWER_NEW(p, struct instance, 1);
    f->spec = source;
    f->subject = subject;
    f->owner = source->kind == MO_SPEC_PROMISE ? subject->owner : source->module;
    f->depth = subject == NULL ? 0 : subject->depth;
    if (source->kind == MO_SPEC_PROMISE)
        f->substitution = subject->substitution; /* Preserve the subject's scoped keys/proofs. */
    f->index = p->function_count++;
    *p->function_tail = f;
    p->function_tail = &f->next;
    f->source_next = p->specs[index];
    p->specs[index] = f;
    f->fun.module = lower_text(p, p->program->set->names[f->owner]);
    switch (source->kind) {
    case MO_SPEC_TEST:
        f->fun.kind = MO_CORE_TEST;
        break;
    case MO_SPEC_EXAMPLE:
        f->fun.kind = MO_CORE_EXAMPLE;
        break;
    case MO_SPEC_PROMISE:
        f->fun.kind = MO_CORE_PROMISE;
        break;
    case MO_SPEC_ANCHOR:
        f->fun.kind = MO_CORE_ANCHOR;
        break;
    }
    f->fun.const_count.digits = lower_text(p, "0");
    f->meta.span = lower_span(p, p->span);
    f->meta.text.some = true;
    bool wording = source->kind == MO_SPEC_TEST;
    f->meta.text.value = wording
                             ? lower_copy(p, (const char *)source->wording, source->wording_length)
                             : lower_copy(p, (const char *)source->text.bytes, source->text.length);
    static const struct mo_source_type unit = {
        .kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
    static const struct mo_source_type int_ = {
        .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    f->fun.ret = lower_type(
        p, source->kind == MO_SPEC_PROMISE || source->kind == MO_SPEC_ANCHOR ? &int_ : &unit, NULL);
    mo_core_string name = f->fun.module;
    const char *suffix = ".test.";
    if (source->kind == MO_SPEC_EXAMPLE) {
        MO_ASSERT(source->subject < p->program->function_count);
        /* Examples have no subject parameters. They run once, even on a
         * generic subject; concrete uses inside their own body instantiate it. */
        name = lower_function_name(p, p->program->functions[source->subject].signature->function);
        suffix = ".example.";
    } else if (source->kind == MO_SPEC_PROMISE) {
        name = subject->fun.name;
        suffix = ".promise.";
        f->meta.subject = (struct mo_core_index){true, subject->index};
        f->meta.lines.count = source->condition_count;
        f->meta.lines.items = LOWER_NEW(p, struct mo_core_spec_line, source->condition_count);
        for (uint32_t j = 0; j < source->condition_count; j++) {
            const struct mo_spec_text *line = &source->conditions[j].text;
            f->meta.lines.items[j] = (struct mo_core_spec_line){
                lower_span(p, line->span), lower_copy(p, (const char *)line->bytes, line->length)};
        }
    } else if (source->kind == MO_SPEC_ANCHOR) {
        name = subject->fun.name;
        suffix = source->allows ? ".allows." : ".forbids.";
        f->meta.subject = (struct mo_core_index){true, subject->index};
        f->meta.allows = (struct mo_core_expectation){true, source->allows};
    }
    f->fun.name = lower_concat(p, name, suffix, lower_number(p, source->number));
    f->meta.name = f->fun.name;
    return f;
}

/* A library module's examples, promises and tests run only when `mo test` is given its file, and
 * then that file is the program's own module, not a library one. So no program lowers them. */
static bool library_spec(const struct lowering *p, const struct mo_checked_spec *source) {
    const struct mo_module_set *set = p->program->set;
    return source->module != 0 && set != NULL && set->library != NULL &&
           set->library[source->module];
}

void lower_subject_specs(struct lowering *p, struct instance *subject) {
    if (p->promise_heads == NULL)
        return; /* Initial ordinary-body walk precedes spec reservation. */
    uint32_t index = (uint32_t)(subject->source - p->program->functions);
    for (uint32_t i = p->promise_heads[index]; i != UINT32_MAX; i = p->promise_next[i]) {
        lower_work(p, 1);
        (void)reserve(p, i, subject);
    }
}

void lower_specs(struct lowering *p) {
    uint32_t count = p->program->spec_count;
    p->specs = LOWER_NEW(p, struct instance *, count);
    p->promise_heads = LOWER_NEW(p, uint32_t, p->program->function_count);
    p->promise_next = LOWER_NEW(p, uint32_t, count);
    for (uint32_t i = 0; i < p->program->function_count; i++)
        p->promise_heads[i] = UINT32_MAX;
    for (uint32_t i = count; i != 0; i--) {
        lower_work(p, 1);
        const struct mo_checked_spec *source = &p->program->specs[i - 1];
        if (source->kind != MO_SPEC_PROMISE || library_spec(p, source))
            continue;
        MO_ASSERT(source->subject < p->program->function_count);
        p->promise_next[i - 1] = p->promise_heads[source->subject];
        p->promise_heads[source->subject] = i - 1;
    }
    /* Reserve links before draining any spec body. Generic promise templates
     * emit only at concrete subject uses, including the anchors themselves. */
    for (uint32_t i = 0; i < count; i++) {
        lower_work(p, 1);
        const struct mo_checked_spec *source = &p->program->specs[i];
        if (library_spec(p, source))
            continue;
        p->span = source->syntax->span;
        if (source->kind == MO_SPEC_EXAMPLE || source->kind == MO_SPEC_TEST)
            (void)reserve(p, i, NULL);
        else if (source->kind == MO_SPEC_ANCHOR) {
            MO_ASSERT(source->subject < p->program->function_count && source->promise < count &&
                      p->program->specs[source->promise].kind == MO_SPEC_PROMISE);
            const struct mo_checked_function *function = &p->program->functions[source->subject];
            struct instance *subject = lower_instance(
                p, function->signature->function, source->arguments, NULL, source->ability_count,
                source->abilities, source->module, source->arguments.count != 0);
            struct instance *promise = reserve(p, source->promise, subject);
            (void)reserve(p, i, promise);
        }
    }
    for (struct instance *f = p->functions; f != NULL; f = f->next)
        if (f->source != NULL)
            lower_subject_specs(p, f);
}
