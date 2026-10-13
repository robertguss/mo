#include "lower/internal.h"

void lower_specs(struct lowering *p) {
    uint32_t count = p->program->spec_count;
    p->specs = LOWER_NEW(p, struct instance *, count);
    for (uint32_t i = 0; i < count; i++) {
        lower_work(p, 1);
        const struct mo_checked_spec *source = &p->program->specs[i];
        p->span = source->syntax->span;
        if (source->kind == MO_SPEC_PROMISE || source->kind == MO_SPEC_ANCHOR)
            lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "promise and anchor code is not lowered yet");
        struct instance *f = LOWER_NEW(p, struct instance, 1);
        f->spec = source;
        f->owner = source->module;
        f->index = p->function_count++;
        *p->function_tail = f;
        p->function_tail = &f->next;
        p->specs[i] = f;
        f->fun.module = lower_text(p, p->program->set->names[source->module]);
        f->fun.kind = source->kind == MO_SPEC_TEST ? MO_CORE_TEST : MO_CORE_EXAMPLE;
        mo_core_string name = f->fun.module;
        if (source->kind == MO_SPEC_EXAMPLE) {
            MO_ASSERT(source->subject < p->program->function_count);
            const struct mo_checked_function *subject = &p->program->functions[source->subject];
            if (subject->signature->type->parameters != 0)
                lower_refuse(p, MO_CODE_LOWER_UNFINISHED,
                             "generic spec instances are not lowered yet");
            name = lower_instance(p, subject->signature->function, (struct mo_type_arguments){0},
                                  NULL, f->owner, 0)
                       ->meta.name;
        }
        f->fun.name = lower_concat(p, name, source->kind == MO_SPEC_TEST ? ".test." : ".example.",
                                   lower_number(p, source->number));
        f->fun.const_count.digits = lower_text(p, "0");
        f->meta.name = f->fun.name;
        f->meta.span = lower_span(p, p->span);
        f->meta.text.some = true;
        f->meta.text.value =
            source->kind == MO_SPEC_TEST
                ? lower_copy(p, (const char *)source->wording, source->wording_length)
                : lower_copy(p, (const char *)source->text.bytes, source->text.length);
        static const struct mo_source_type unit = {
            .kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
        f->fun.ret = lower_type(p, &unit, NULL);
        lower_body(p, f, &source->body, source->parameter_count, source->parameters);
    }
}
