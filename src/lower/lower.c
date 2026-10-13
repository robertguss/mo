#include "lower/scalar.h"

#include <stdio.h>
#include <string.h>

_Noreturn void lower_refuse(struct lowering *p, uint16_t code, const char *message) {
    struct mo_diag diag = {.code = code, .primary.span = p->span, .message = message};
    bool kept = mo_diags_add(p->diags, &diag);
    (void)kept;
    /* Only arena storage is live between this boundary and mo_lower. No
     * destructor, file, lock or independent scratch arena is skipped. */
    longjmp(p->escape, 1);
}
void lower_work(struct lowering *p, size_t amount) {
    if (amount > MO_LIMIT_LOWER_WORK - p->work)
        lower_refuse(p, MO_CODE_LOWER_WORK, "lowering exceeds its work limit");
    p->work += amount;
}
void *lower_allocate(struct lowering *p, size_t count, size_t size, size_t alignment) {
    MO_ASSERT(size != 0);
    if (count > SIZE_MAX / size)
        lower_refuse(p, MO_CODE_LOWER_NO_MEMORY, "the lowering allocation is too large");
    lower_work(p, count * size);
    void *result = mo_arena_allocate_array(p->arena, count, size, alignment);
    if (result == NULL)
        lower_refuse(p, MO_CODE_LOWER_NO_MEMORY, "the lowering arena has no room");
    memset(result, 0, count * size);
    return result;
}
mo_core_string lower_copy(struct lowering *p, const char *text, size_t length) {
    char *bytes = LOWER_NEW(p, char, length + 1);
    if (length != 0)
        memcpy(bytes, text, length);
    return (mo_core_string){bytes, length};
}
mo_core_string lower_text(struct lowering *p, const char *text) {
    return lower_copy(p, text, strlen(text));
}
mo_core_string lower_number(struct lowering *p, uint64_t number) {
    char text[21];
    int length = snprintf(text, sizeof text, "%llu", (unsigned long long)number);
    MO_ASSERT(length > 0 && (size_t)length < sizeof text);
    return lower_copy(p, text, (size_t)length);
}
mo_core_string lower_concat(struct lowering *p, mo_core_string a, const char *separator,
                            mo_core_string b) {
    size_t middle = strlen(separator);
    MO_ASSERT(a.length <= MO_LIMIT_LOWER_WORK && b.length <= MO_LIMIT_LOWER_WORK);
    size_t length = a.length + middle + b.length;
    char *bytes = LOWER_NEW(p, char, length + 1);
    memcpy(bytes, a.bytes, a.length);
    memcpy(bytes + a.length, separator, middle);
    memcpy(bytes + a.length + middle, b.bytes, b.length);
    return (mo_core_string){bytes, length};
}
mo_core_string lower_fresh(struct lowering *p) {
    MO_ASSERT(p->serial < MO_LIMIT_LOWER_WORK);
    return lower_concat(p, lower_text(p, "%"), "", lower_number(p, p->serial++));
}
bool lower_same(mo_core_string a, mo_core_string b) {
    return a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0;
}
struct mo_core_span lower_span(struct lowering *p, struct mo_span span) {
    MO_ASSERT(span.file < p->sources->count && span.start <= span.end &&
              span.end <= p->sources->files[span.file].length);
    return (struct mo_core_span){.some = true, .value = span};
}
mo_core_expr lower_expr(struct lowering *p, enum mo_core_expr_tag tag) {
    if (p->nodes == MO_LIMIT_CORE_NODES)
        lower_refuse(p, MO_CODE_LOWER_WORK, "lowering exceeds its Core node limit");
    p->nodes++;
    struct lower_node *node = LOWER_NEW(p, struct lower_node, 1);
    node->expr.tag = tag;
    node->span = lower_span(p, p->span);
    return &node->expr;
}
mo_core_expr lower_var(struct lowering *p, mo_core_string name) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_VAR);
    e->as.var = name;
    return e;
}
mo_core_expr lower_unit(struct lowering *p) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_LIT);
    e->as.lit = LOWER_NEW(p, struct mo_core_lit, 1);
    e->as.lit->tag = MO_CORE_LIT_UNIT;
    return e;
}
mo_core_expr lower_bool(struct lowering *p, bool value) {
    mo_core_expr e = lower_unit(p);
    e->as.lit->tag = MO_CORE_LIT_BOOL;
    e->as.lit->as.bool_ = value;
    return e;
}
mo_core_expr lower_int(struct lowering *p, uint64_t value) {
    mo_core_expr e = lower_unit(p);
    e->as.lit->tag = MO_CORE_LIT_INT;
    e->as.lit->as.int_.digits = lower_number(p, value);
    return e;
}
mo_core_expr lower_prim(struct lowering *p, struct mo_core_prim prim, mo_core_expr_list args) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_PRIM);
    e->as.prim.prim = prim;
    e->as.prim.args = args;
    return e;
}

static mo_core_expr status(struct lowering *p, mo_core_expr value) {
    mo_core_string name = lower_fresh(p);
    mo_core_expr bound = lower_expr(p, MO_CORE_EXPR_LET);
    bound->as.let_.name = name;
    bound->as.let_.bound = value;
    mo_core_expr test = lower_expr(p, MO_CORE_EXPR_IF);
    mo_core_expr_list args = {.count = 2, .items = LOWER_NEW(p, mo_core_expr, 2)};
    args.items[0] = lower_var(p, name);
    args.items[1] = lower_int(p, 0);
    test->as.if_.cond =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_CMP,
                                         .as.cmp = {MO_CORE_BASE_INT, MO_CORE_CMP_OP_LT}},
                   args);
    mo_core_expr stop = lower_expr(p, MO_CORE_EXPR_STOP);
    stop->as.stop = lower_text(p, "exit status outside 0 to 255");
    test->as.if_.then_ = stop;
    mo_core_expr upper = lower_expr(p, MO_CORE_EXPR_IF);
    args.items = LOWER_NEW(p, mo_core_expr, 2);
    args.items[0] = lower_var(p, name);
    args.items[1] = lower_int(p, 255);
    upper->as.if_.cond =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_CMP,
                                         .as.cmp = {MO_CORE_BASE_INT, MO_CORE_CMP_OP_GT}},
                   args);
    upper->as.if_.then_ = stop;
    upper->as.if_.else_ = lower_var(p, name);
    test->as.if_.else_ = upper;
    bound->as.let_.body = test;
    return bound;
}

static void entry(struct lowering *p, struct instance *main) {
    const struct mo_checked_entry *checked = &p->program->entry;
    const struct mo_symbol *symbol = main->source->signature->function;
    p->span = symbol->declaration->span;
    struct instance *wrapper = LOWER_NEW(p, struct instance, 1);
    wrapper->owner = 0;
    wrapper->index = p->function_count++;
    *p->function_tail = wrapper;
    p->function_tail = &wrapper->next;
    wrapper->fun.name = lower_text(p, "%entry");
    wrapper->fun.module = lower_text(p, p->program->set->names[0]);
    wrapper->fun.const_count.digits = lower_text(p, "0");
    static const struct mo_source_type int_type = {
        .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    wrapper->fun.ret = lower_type(p, &int_type, NULL);
    wrapper->meta.name = lower_concat(p, main->meta.name, ".", lower_text(p, "entry"));
    wrapper->meta.span = lower_span(p, p->span);
    wrapper->fun.params = main->fun.params;
    wrapper->meta.body.params = main->meta.body.params;
    mo_core_string console = {0};
    for (size_t i = 0; i < main->fun.params.count; i++) {
        mo_core_ty t = main->fun.params.items[i].ty;
        if (t->tag == MO_CORE_TY_CAP && t->as.cap->tag == MO_CORE_CAP_CONSOLE)
            console = main->fun.params.items[i].name;
    }
    if (checked->result == MO_ENTRY_RESULT && console.length == 0) {
        static const struct mo_source_type console_type = {
            .kind = MO_SOURCE_NAMED, .name = "Console", .module = UINT32_MAX};
        size_t count = main->fun.params.count;
        wrapper->fun.params.items = LOWER_NEW(p, struct mo_core_param, count + 1);
        wrapper->meta.body.params.items = LOWER_NEW(p, struct mo_core_name, count + 1);
        for (size_t i = 0; i < count; i++) {
            wrapper->fun.params.items[i] = main->fun.params.items[i];
            wrapper->meta.body.params.items[i] = main->meta.body.params.items[i];
        }
        console = lower_fresh(p);
        wrapper->fun.params.items[count] =
            (struct mo_core_param){console, lower_type(p, &console_type, NULL)};
        wrapper->fun.params.count = wrapper->meta.body.params.count = count + 1;
    }
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = main->fun.name;
    call->as.call.args.count = main->fun.params.count;
    call->as.call.args.items = LOWER_NEW(p, mo_core_expr, main->fun.params.count);
    for (size_t i = 0; i < main->fun.params.count; i++)
        call->as.call.args.items[i] = lower_var(p, main->fun.params.items[i].name);
    if (checked->result == MO_ENTRY_STATUS)
        wrapper->fun.body = status(p, call);
    else if (checked->result == MO_ENTRY_UNIT) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_LET);
        e->as.let_.name = lower_fresh(p);
        e->as.let_.bound = call;
        e->as.let_.body = lower_int(p, 0);
        wrapper->fun.body = e;
    } else {
        MO_ASSERT(main->fun.ret->tag == MO_CORE_TY_RESULT && checked->error_text != NULL);
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CASE);
        e->as.case_.expr = call;
        e->as.case_.arms.count = 2;
        e->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        for (size_t i = 0; i < 2; i++) {
            struct mo_core_arm *arm = &e->as.case_.arms.items[i];
            arm->ctor = lower_text(p, i == 0 ? "Ok" : "Error");
            arm->names.count = 1;
            arm->names.items = LOWER_NEW(p, mo_core_string, 1);
            arm->names.items[0] = lower_fresh(p);
        }
        e->as.case_.arms.items[0].body = lower_int(p, 0);
        mo_core_expr error = lower_var(p, e->as.case_.arms.items[1].names.items[0]);
        mo_core_expr text;
        if (checked->error_text->kind == MO_ABILITY_NATIVE)
            text = lower_error_text(p, main->fun.ret->as.result.e, error);
        else if (checked->error_text->kind == MO_ABILITY_FUNCTION) {
            struct instance *f = lower_instance(p, checked->error_text->function,
                                                checked->error_text->arguments, NULL, 0, 0);
            text = lower_expr(p, MO_CORE_EXPR_CALL);
            text->as.call.name = f->fun.name;
            text->as.call.args.count = 1;
            text->as.call.args.items = LOWER_NEW(p, mo_core_expr, 1);
            text->as.call.args.items[0] = error;
        } else
            lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "derived error text is not lowered yet");
        mo_core_expr_list args = {.count = 2, .items = LOWER_NEW(p, mo_core_expr, 2)};
        args.items[0] = lower_var(p, console);
        args.items[1] = text;
        mo_core_expr printed = lower_expr(p, MO_CORE_EXPR_LET);
        printed->as.let_.name = lower_fresh(p);
        printed->as.let_.bound = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_IO, .as.io.tag = MO_CORE_IO_OP_PRINT_ERROR},
            args);
        printed->as.let_.body = lower_int(p, 1);
        e->as.case_.arms.items[1].body = printed;
        wrapper->fun.body = e;
    }
}

struct mo_core *mo_lower(struct mo_arena *arena, const struct mo_program *program,
                         const struct mo_sources *sources, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && program != NULL && sources != NULL && diags != NULL);
    struct lowering *p = mo_arena_allocate(arena, sizeof *p, _Alignof(struct lowering));
    if (p == NULL) {
        struct mo_diag diag = {.code = MO_CODE_LOWER_NO_MEMORY,
                               .primary.span.file = UINT32_MAX,
                               .message = "the lowering arena has no room"};
        bool kept = mo_diags_add(diags, &diag);
        (void)kept;
        return NULL;
    }
    *p = (struct lowering){.arena = arena,
                           .program = program,
                           .sources = sources,
                           .diags = diags,
                           .span.file = UINT32_MAX};
    if (setjmp(p->escape) != 0)
        return NULL;
    if (program->function_count == 0 || program->constant_count != 0 || program->spec_count != 0)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED,
                     "constants and specification code are not lowered yet");
    p->function_tail = &p->functions;
    p->data_tail = &p->data;
    lower_indices(p);
    if (program->signatures != NULL)
        for (uint32_t i = 0; i < program->signatures->data_count; i++) {
            const struct mo_source_type *type = program->signatures->data[i].type;
            if (type->parameters == 0)
                (void)lower_type(p, type, NULL);
        }
    MO_ASSERT(program->entry.function < program->function_count);
    struct instance *main =
        lower_instance(p, program->functions[program->entry.function].signature->function,
                       (struct mo_type_arguments){0}, NULL, 0, 0);
    for (uint32_t i = 0; i < program->function_count; i++) {
        const struct mo_checked_function *f = &program->functions[i];
        if (f->signature->type->parameters == 0)
            (void)lower_instance(p, f->signature->function, (struct mo_type_arguments){0}, NULL,
                                 f->signature->function->module, 0);
    }
    for (struct instance *f = p->functions; f != NULL; f = f->next)
        lower_function(p, f);
    entry(p, main);
    for (struct instance *f = p->functions; f != NULL; f = f->next)
        if (f->source != NULL && f->fun.body == NULL)
            lower_function(p, f);
    for (struct data_instance *d = p->data; d != NULL; d = d->next)
        lower_data(p, d);
    struct mo_core *result = LOWER_NEW(p, struct mo_core, 1);
    result->entry = lower_text(p, "%entry");
    result->funs.count = p->function_count;
    result->funs.items = LOWER_NEW(p, struct mo_core_fun, result->funs.count);
    result->data.count = p->data_count;
    result->data.items = LOWER_NEW(p, struct mo_core_data, result->data.count);
    size_t i = 0;
    for (struct instance *f = p->functions; f != NULL; f = f->next)
        result->funs.items[i++] = f->fun;
    i = 0;
    for (struct data_instance *d = p->data; d != NULL; d = d->next)
        result->data.items[i++] = d->data;
    lower_metadata(p, result);
    /* The wire form includes list/schema wrappers absent from Expr height,
     * and metadata contributes bytes/nodes. Check the actual public codec;
     * release its independent storage before a refusal can longjmp. */
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
    bool printable = mo_core_print(result, &text);
    size_t bytes = text.length;
    mo_buffer_free(&text);
    if (!printable)
        lower_refuse(p, MO_CODE_LOWER_TEXT, "lowered Core cannot fit the text codec's bounds");
    lower_work(p, bytes);
    return result;
}
