#include "lower/internal.h"

void lower_specs(struct lowering *p) {
    uint32_t count = p->program->spec_count;
    p->specs = LOWER_NEW(p, struct instance *, count);
    for (uint32_t i = 0; i < count; i++) {
        lower_work(p, 1);
        const struct mo_checked_spec *source = &p->program->specs[i];
        p->span = source->syntax->span;
        struct instance *f = LOWER_NEW(p, struct instance, 1);
        f->spec = source;
        f->owner = source->module;
        if (p->function_count == MO_LIMIT_CORE_NODES)
            lower_refuse(p, MO_CODE_LOWER_WORK, "lowering exceeds its spec function limit");
        f->index = p->function_count++;
        *p->function_tail = f;
        p->function_tail = &f->next;
        p->specs[i] = f;
        f->fun.module = lower_text(p, p->program->set->names[source->module]);
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
        bool wording = source->kind == MO_SPEC_TEST || source->kind == MO_SPEC_PROMISE;
        f->meta.text.value =
            wording ? lower_copy(p, (const char *)source->wording, source->wording_length)
                    : lower_copy(p, (const char *)source->text.bytes, source->text.length);
        static const struct mo_source_type unit = {
            .kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
        static const struct mo_source_type int_ = {
            .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
        f->fun.ret = lower_type(
            p, source->kind == MO_SPEC_PROMISE || source->kind == MO_SPEC_ANCHOR ? &int_ : &unit,
            NULL);
        if (source->kind == MO_SPEC_ANCHOR)
            continue;
        mo_core_string name = f->fun.module;
        const char *suffix = ".test.";
        if (source->kind != MO_SPEC_TEST) {
            MO_ASSERT(source->subject < p->program->function_count);
            const struct mo_checked_function *subject = &p->program->functions[source->subject];
            if (subject->signature->type->parameters != 0)
                lower_refuse(p, MO_CODE_LOWER_UNFINISHED,
                             "generic spec instances are not lowered yet");
            struct instance *ordinary =
                lower_instance(p, subject->signature->function, (struct mo_type_arguments){0}, NULL,
                               0, NULL, f->owner, 0);
            name = ordinary->meta.name;
            suffix = source->kind == MO_SPEC_PROMISE ? ".promise." : ".example.";
            if (source->kind == MO_SPEC_PROMISE) {
                f->meta.subject = (struct mo_core_index){true, ordinary->index};
                f->meta.lines.count = source->condition_count;
                f->meta.lines.items =
                    LOWER_NEW(p, struct mo_core_spec_line, source->condition_count);
                for (uint32_t j = 0; j < source->condition_count; j++) {
                    const struct mo_spec_text *line = &source->conditions[j].text;
                    f->meta.lines.items[j] = (struct mo_core_spec_line){
                        lower_span(p, line->span),
                        lower_copy(p, (const char *)line->bytes, line->length)};
                }
            }
        }
        f->fun.name = lower_concat(p, name, suffix, lower_number(p, source->number));
        f->meta.name = f->fun.name;
    }
    /* Reserve every link before bodies; an anchor may precede its promise in
     * the graph's array without acquiring a discovery-dependent name. */
    for (uint32_t i = 0; i < count; i++) {
        lower_work(p, 1);
        const struct mo_checked_spec *source = &p->program->specs[i];
        struct instance *f = p->specs[i];
        p->span = source->syntax->span;
        if (source->kind == MO_SPEC_ANCHOR) {
            if (source->arguments.count != 0)
                lower_refuse(p, MO_CODE_LOWER_UNFINISHED,
                             "generic anchor instances are not lowered yet");
            MO_ASSERT(source->promise < count &&
                      p->program->specs[source->promise].kind == MO_SPEC_PROMISE);
            struct instance *promise = p->specs[source->promise];
            f->fun.name =
                lower_concat(p, promise->fun.name, source->allows ? ".allows." : ".forbids.",
                             lower_number(p, source->number));
            f->meta.name = f->fun.name;
            f->meta.subject = (struct mo_core_index){true, promise->index};
            f->meta.allows = (struct mo_core_expectation){true, source->allows};
        }
        lower_body(p, f, &source->body, source->parameter_count, source->parameters);
    }
}
