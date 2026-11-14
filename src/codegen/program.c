#include "codegen/internal.h"

#include <string.h>

#include "base/codes.h"

/* Core established the first-match ordinary entry. Tools must find it by full name. */
MO_MUST_USE bool codegen_entry_valid(struct compiler *c) {
    const struct mo_core *p = c->core;
    struct mo_codegen_index *entry = mo_codegen_find(c->fun_index, p->entry);
    MO_ASSERT(entry != NULL);
    c->current = &p->funs.items[entry->value];
    c->declaration =
        p->metadata.source ? p->metadata.funs.items[entry->value].span : (struct mo_core_span){0};
    c->span = c->declaration;
    uint32_t full_name = c->funs[entry->value].name;
    for (uint32_t i = 0; i < c->function_count; i++) {
        if (i != entry->value && c->funs[i].name == full_name)
            return codegen_problem(c, MO_CODE_CODEGEN_ENTRY,
                                   "The entry's full runtime name is not unique in the image.");
    }
    return true;
}
/* One sizing walk keeps function pointers stable during recursive lifting. */
static MO_MUST_USE bool count_lambdas(struct compiler *c, mo_core_expr e, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    mo_core_expr small[3];
    mo_core_expr_list xs = {0};
    switch (e->tag) {
    case MO_CORE_EXPR_LAM:
        if (c->function_capacity == MO_LIMIT_FUNCTIONS)
            return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                   "Lifted closures exceed MO_LIMIT_FUNCTIONS.");
        c->function_capacity++;
        small[0] = e->as.lam.body;
        xs = (mo_core_expr_list){small, 1};
        break;
    case MO_CORE_EXPR_LET:
        small[0] = e->as.let_.bound;
        small[1] = e->as.let_.body;
        xs = (mo_core_expr_list){small, 2};
        break;
    case MO_CORE_EXPR_APP:
        if (!count_lambdas(c, e->as.app.f, depth + 1))
            return false;
        xs = e->as.app.args;
        break;
    case MO_CORE_EXPR_CALL:
        xs = e->as.call.args;
        break;
    case MO_CORE_EXPR_TUPLE:
        xs = e->as.tuple;
        break;
    case MO_CORE_EXPR_CON:
        xs = e->as.con.args;
        break;
    case MO_CORE_EXPR_PRIM:
        xs = e->as.prim.args;
        break;
    case MO_CORE_EXPR_JUMP:
        xs = e->as.jump.args;
        break;
    case MO_CORE_EXPR_CASE:
        if (!count_lambdas(c, e->as.case_.expr, depth + 1))
            return false;
        for (size_t i = 0; i < e->as.case_.arms.count; i++)
            if (!count_lambdas(c, e->as.case_.arms.items[i].body, depth + 1))
                return false;
        break;
    case MO_CORE_EXPR_IF:
        small[0] = e->as.if_.cond;
        small[1] = e->as.if_.then_;
        small[2] = e->as.if_.else_;
        xs = (mo_core_expr_list){small, 3};
        break;
    case MO_CORE_EXPR_PROJ:
        small[0] = e->as.proj.expr;
        xs = (mo_core_expr_list){small, 1};
        break;
    case MO_CORE_EXPR_FAIL:
        small[0] = e->as.fail;
        xs = (mo_core_expr_list){small, 1};
        break;
    case MO_CORE_EXPR_JOIN:
        small[0] = e->as.join.body;
        small[1] = e->as.join.rest;
        xs = (mo_core_expr_list){small, 2};
        break;
    case MO_CORE_EXPR_LIT:
    case MO_CORE_EXPR_VAR:
    case MO_CORE_EXPR_GLOBAL:
    case MO_CORE_EXPR_STOP:
        break;
    }
    for (size_t i = 0; i < xs.count; i++)
        if (!count_lambdas(c, xs.items[i], depth + 1))
            return false;
    return true;
}
MO_MUST_USE bool codegen_declarations(struct compiler *c) {
    const struct mo_core *p = c->core;
    if (c->services != NULL && c->services->count != 0 && c->services->data_count != p->data.count)
        return codegen_problem(c, MO_CODE_CODEGEN_INVALID,
                               "Service shapes differ from Core data table.");
    if (p->funs.count > MO_LIMIT_FUNCTIONS ||
        p->consts.count > (MO_LIMIT_FUNCTIONS - p->funs.count) / 2 ||
        p->consts.count > MO_LIMIT_GLOBALS || p->data.count > MO_LIMIT_TYPES)
        return codegen_problem(
            c, MO_CODE_BYTECODE_TOO_BIG,
            "Core and its initializer helpers exceed a bytecode declaration limit.");
    c->function_count = (uint32_t)(p->funs.count + 2 * p->consts.count);
    c->function_capacity = c->function_count;
    for (size_t i = 0; i < p->funs.count; i++)
        if (!count_lambdas(c, p->funs.items[i].body, 0))
            return false;
    for (size_t i = 0; i < p->consts.count; i++)
        if (!count_lambdas(c, p->consts.items[i].body, 0))
            return false;
    c->data = codegen_allocate(c, p->data.count, sizeof *c->data);
    c->funs = codegen_allocate(c, c->function_capacity, sizeof *c->funs);
    c->parameters = codegen_allocate(c, c->function_capacity, sizeof *c->parameters);
    c->bodies = codegen_allocate(c, c->function_capacity, sizeof *c->bodies);
    c->debug = codegen_allocate(c, c->function_capacity, sizeof *c->debug);
    size_t file_count = p->metadata.source ? p->metadata.files.count : 0;
    c->files = codegen_allocate(c, file_count, sizeof *c->files);
    if (c->data == NULL || c->funs == NULL || c->parameters == NULL || c->bodies == NULL ||
        c->debug == NULL || c->files == NULL)
        return false;
    memset(c->funs, 0, c->function_capacity * sizeof *c->funs);
    memset(c->parameters, 0, c->function_capacity * sizeof *c->parameters);
    memset(c->bodies, 0, c->function_capacity * sizeof *c->bodies);
    memset(c->debug, 0, c->function_capacity * sizeof *c->debug);
    for (size_t i = 0; i < file_count; i++) {
        mo_core_string path = p->metadata.files.items[i];
        if (path.length > MO_LIMIT_STRING_SIZE)
            return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                   "Filename exceeds the bytecode MO_LIMIT_STRING_SIZE bound.");
        c->files[i] =
            (struct mo_bytecode_bytes){(uint32_t)path.length, (const uint8_t *)path.bytes};
    }
    for (size_t i = 0; i < p->data.count; i++) {
        const char *n = codegen_name(c, p->data.items[i].name);
        if (n == NULL)
            return false;
        c->data[i] = MO_NO_INDEX;
        enum mo_type_problem status = mo_types_declare(&c->types, MO_TYPE_CHOICE, n, &c->data[i]);
        if (codegen_type_result(c, status, c->data[i]) == MO_NO_INDEX ||
            codegen_index_add(c, &c->data_index, p->data.items[i].name, c->data[i], false) == NULL)
            return false;
        if (c->services != NULL && c->services->count != 0 &&
            !codegen_service_shape(c, c->data[i], &c->services->data_shapes[i]))
            return false;
    }
    for (size_t i = 0; i < p->data.count; i++) {
        const struct mo_core_data *d = &p->data.items[i];
        if (d->ctors.count == 0)
            return codegen_problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                                   "Empty data declarations cannot come from lowering.");
        struct mo_type_case *cases = codegen_allocate(c, d->ctors.count, sizeof *cases);
        struct mo_codegen_index *seen = NULL;
        if (cases == NULL)
            return false;
        for (size_t j = 0; j < d->ctors.count; j++) {
            const struct mo_core_ctor *ctor = &d->ctors.items[j];
            if (mo_codegen_find(seen, ctor->name) != NULL)
                return codegen_problem(
                    c, MO_CODE_CODEGEN_REPRESENTATION,
                    "Duplicate Core constructor names cannot come from lowering.");
            if (codegen_index_add(c, &seen, ctor->name, 0, false) == NULL)
                return false;
            if (ctor->fields.count > MO_LIMIT_FIELDS)
                return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                       "Constructor exceeds MO_LIMIT_FIELDS.");
            const char *n = codegen_name(c, ctor->name);
            struct mo_type_field *field = codegen_allocate(c, ctor->fields.count, sizeof *field);
            if (n == NULL || field == NULL)
                return false;
            struct mo_codegen_index *field_names = NULL;
            for (size_t k = 0; k < ctor->fields.count; k++) {
                mo_core_string key = ctor->fields.items[k].name;
                if ((key.length == 0 && ctor->fields.count != 1) ||
                    mo_codegen_find(field_names, key) != NULL)
                    return codegen_problem(
                        c, MO_CODE_CODEGEN_REPRESENTATION,
                        "Empty or duplicate named fields cannot come from lowering.");
                if (codegen_index_add(c, &field_names, key, 0, false) == NULL)
                    return false;
                field[k].name = ctor->fields.items[k].name.length == 0
                                    ? NULL
                                    : codegen_name(c, ctor->fields.items[k].name);
                field[k].type = codegen_type(c, ctor->fields.items[k].ty, 0);
                if (c->error != 0)
                    return false;
            }
            cases[j] = (struct mo_type_case){n, field, (uint32_t)ctor->fields.count,
                                             ctor->fields.count == 1 && field[0].name == NULL};
        }
        enum mo_type_problem status =
            mo_types_define_choice(&c->types, c->data[i], cases, (uint32_t)d->ctors.count);
        if (codegen_type_result(c, status, c->data[i]) == MO_NO_INDEX)
            return false;
    }
    size_t module_count = p->metadata.source ? p->metadata.modules.count : 0;
    for (size_t i = 0; i < module_count; i++) {
        const struct mo_core_module_meta *m = &p->metadata.modules.items[i];
        uint32_t id = c->modules.count;
        struct mo_bytecode_module *b =
            codegen_append(c, &c->modules, 1, MO_LIMIT_MODULES, MO_CODE_BYTECODE_TOO_BIG);
        struct mo_bytecode_type_export *exports =
            codegen_allocate(c, m->exposed_types.count, sizeof *exports);
        if (b == NULL || exports == NULL)
            return false;
        for (size_t j = 0; j < m->exposed_types.count; j++)
            exports[j] =
                (struct mo_bytecode_type_export){c->data[m->exposed_types.items[j].index],
                                                 m->exposed_types.items[j].contents_exposed};
        *b = (struct mo_bytecode_module){.name = codegen_string(c, m->name),
                                         .type_count = (uint32_t)m->exposed_types.count,
                                         .types = exports,
                                         .function_count = (uint32_t)m->exposed_funs.count,
                                         .functions = m->exposed_funs.items,
                                         .global_count = (uint32_t)m->consts.count,
                                         .globals = m->consts.items};
        if (b->name == MO_NO_INDEX ||
            codegen_index_add(c, &c->module_index, m->name, id, false) == NULL)
            return false;
    }
    if (!p->metadata.source && (p->data.count != 0 || p->consts.count != 0)) {
        struct mo_bytecode_module *m =
            codegen_append(c, &c->modules, 1, MO_LIMIT_MODULES, MO_CODE_BYTECODE_TOO_BIG);
        if (m == NULL ||
            codegen_index_add(c, &c->module_index, codegen_text("%Core"), 0, false) == NULL)
            return false;
        *m = (struct mo_bytecode_module){.name = codegen_string(c, codegen_text("%Core"))};
        if (m->name == MO_NO_INDEX)
            return false;
    }
    static const enum mo_function_kind kinds[] = {MO_FUNCTION_FUNCTION, MO_FUNCTION_TEST,
                                                  MO_FUNCTION_EXAMPLE, MO_FUNCTION_PROMISE,
                                                  MO_FUNCTION_ANCHOR};
    for (size_t i = 0; i < p->funs.count; i++) {
        c->current = &p->funs.items[i];
        const struct mo_core_fun *fun = c->current;
        const struct mo_core_fun_meta *meta =
            p->metadata.source ? &p->metadata.funs.items[i] : NULL;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        if (!codegen_name_valid(c, fun->name) || !codegen_name_valid(c, fun->module))
            return false;
        if (fun->params.count > MO_LIMIT_BYTECODE_PARAMETERS)
            return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                   "Function exceeds MO_LIMIT_BYTECODE_PARAMETERS.");
        struct mo_codegen_index *module = mo_codegen_find(c->module_index, fun->module);
        if (module == NULL) {
            uint32_t id = c->modules.count;
            struct mo_bytecode_module *m =
                codegen_append(c, &c->modules, 1, MO_LIMIT_MODULES, MO_CODE_BYTECODE_TOO_BIG);
            if (m == NULL || codegen_name(c, fun->module) == NULL)
                return false;
            *m = (struct mo_bytecode_module){.name = codegen_string(c, fun->module)};
            module = codegen_index_add(c, &c->module_index, fun->module, id, false);
            if (module == NULL)
                return false;
        }
        uint32_t full_name = MO_NO_INDEX;
        if (meta != NULL)
            full_name = codegen_string(c, meta->name);
        else {
            struct mo_buffer b;
            mo_buffer_init(&b, MO_LIMIT_STRING_SIZE);
            bool ok = mo_buffer_append(&b, fun->module.bytes, fun->module.length) &&
                      mo_buffer_append_byte(&b, '.') &&
                      mo_buffer_append(&b, fun->name.bytes, fun->name.length);
            if (ok && codegen_name(c, (mo_core_string){(const char *)b.bytes, b.length}) != NULL)
                full_name = codegen_string(c, (mo_core_string){(const char *)b.bytes, b.length});
            mo_buffer_free(&b);
            if (full_name == MO_NO_INDEX &&
                !codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                 "Generated function name exceeds its limit."))
                return false;
        }
        struct mo_parameter *params = codegen_allocate(c, fun->params.count, sizeof *params);
        if (params == NULL)
            return false;
        c->parameters[i] = params;
        for (size_t j = 0; j < fun->params.count; j++) {
            if (!codegen_name_valid(c, fun->params.items[j].name))
                return false;
            params[j] = (struct mo_parameter){codegen_type(c, fun->params.items[j].ty, 0),
                                              MO_PARAMETER_OWNED};
            if (params[j].type == MO_NO_INDEX)
                return false;
        }
        c->funs[i] = (struct mo_bytecode_function){.name = full_name,
                                                   .module = module->value,
                                                   .kind = kinds[fun->kind],
                                                   .spec_code = fun->kind != MO_CORE_FUNCTION,
                                                   .parameter_count = (uint32_t)fun->params.count,
                                                   .parameters = params,
                                                   .result_type = codegen_type(c, fun->ret, 0)};
        c->bodies[i].meta = meta;
        c->bodies[i].source = fun;
        if (c->funs[i].result_type == MO_NO_INDEX ||
            codegen_index_add(c, &c->fun_index, fun->name, (uint32_t)i, false) == NULL)
            return false;
    }
    return c->error == 0;
}
MO_MUST_USE bool codegen_globals(struct compiler *c) {
    const struct mo_core *p = c->core;
    if (p->consts.count == 0)
        return true;
    c->globals = codegen_allocate(c, p->consts.count, sizeof *c->globals);
    c->init = codegen_allocate(c, p->consts.count, sizeof *c->init);
    if (c->globals == NULL || c->init == NULL)
        return false;
    if (p->metadata.source) {
        for (uint32_t i = 0; i < p->metadata.modules.count; i++) {
            const mo_core_indices owned = p->metadata.modules.items[i].consts;
            for (size_t j = 0; j < owned.count; j++)
                c->globals[owned.items[j]].module = i;
        }
    } else {
        if (p->consts.count > MO_LIMIT_MODULE_NAMES)
            return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                   "Source-less constants exceed MO_LIMIT_MODULE_NAMES in %Core.");
        uint32_t *ids = codegen_allocate(c, p->consts.count, sizeof *ids);
        if (ids == NULL)
            return false;
        struct mo_bytecode_module *module = c->modules.items;
        module[0].global_count = (uint32_t)p->consts.count;
        module[0].globals = ids;
        for (uint32_t i = 0; i < p->consts.count; i++) {
            ids[i] = i;
            c->globals[i].module = 0;
        }
    }
    uint32_t unit = codegen_scalar(c, MO_TYPE_UNIT);
    if (unit == MO_NO_INDEX)
        return false;
    c->unit_constant = codegen_constant(c, (struct mo_bytecode_constant){.type = unit});
    if (c->unit_constant == MO_NO_INDEX)
        return false;
    for (uint32_t i = 0; i < p->consts.count; i++) {
        const struct mo_core_const *k = &p->consts.items[i];
        struct mo_core_fun *source = codegen_allocate(c, 1, sizeof *source);
        if (source == NULL)
            return false;
        *source = (struct mo_core_fun){.name = k->name, .ret = k->ty, .body = k->body};
        c->current = source;
        const struct mo_core_const_meta *meta =
            p->metadata.source ? &p->metadata.consts.items[i] : NULL;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        if (!codegen_name_valid(c, k->name))
            return false;
        mo_core_string full_name = meta == NULL ? k->name : meta->name;
        c->globals[i].name = codegen_string(c, full_name);
        c->globals[i].type = codegen_type(c, k->ty, 0);
        uint32_t value = (uint32_t)p->funs.count + 2 * i, init = value + 1;
        c->init[i] = init;
        c->funs[value] = (struct mo_bytecode_function){
            .name = codegen_suffixed_name(c, full_name, ".init.%value"),
            .module = c->globals[i].module,
            .kind = MO_FUNCTION_FUNCTION,
            .result_type = c->globals[i].type};
        c->funs[init] =
            (struct mo_bytecode_function){.name = codegen_suffixed_name(c, full_name, ".init"),
                                          .module = c->globals[i].module,
                                          .kind = MO_FUNCTION_INIT,
                                          .result_type = unit};
        c->bodies[value].source = c->bodies[init].source = source;
        if (meta != NULL) {
            struct mo_core_fun_meta *body_meta = codegen_allocate(c, 1, sizeof *body_meta);
            if (body_meta == NULL)
                return false;
            *body_meta = (struct mo_core_fun_meta){
                .name = meta->name, .span = meta->span, .body = meta->body};
            c->bodies[value].meta = c->bodies[init].meta = body_meta;
        }
        if (c->error != 0 || codegen_index_add(c, &c->global_index, k->name, i, false) == NULL)
            return false;
    }
    return true;
}
MO_MUST_USE bool codegen_bodies(struct compiler *c) {
    uint32_t declared = c->function_count;
    for (uint32_t i = 0; i < declared; i++) {
        struct function *f = &c->bodies[i];
        c->current = f->source;
        const struct mo_core_fun_meta *meta = f->meta;
        c->declaration = meta == NULL ? (struct mo_core_span){0} : meta->span;
        c->span = c->declaration;
        f->declaration = c->declaration;
        f->root = f;
        codegen_function_init(f);
        if (c->funs[i].kind == MO_FUNCTION_INIT)
            continue;
        if (meta != NULL) {
            for (size_t j = 0; j < meta->body.bindings.count; j++) {
                const struct mo_core_binding_meta *binding = &meta->body.bindings.items[j];
                const uint32_t key[] = {binding->node, binding->slot};
                uint32_t n = codegen_string(c, binding->name);
                if (n == MO_NO_INDEX ||
                    codegen_index_add(c, &f->bindings,
                                      (mo_core_string){(const char *)key, sizeof key}, n,
                                      true) == NULL)
                    return false;
            }
            for (size_t j = 0; j < meta->body.closures.count; j++) {
                const struct mo_core_closure_meta *closure = &meta->body.closures.items[j];
                uint32_t id = codegen_string(c, closure->name);
                if (id == MO_NO_INDEX ||
                    codegen_index_add(
                        c, &f->closure_names,
                        (mo_core_string){(const char *)&closure->node, sizeof closure->node}, id,
                        true) == NULL)
                    return false;
            }
        }
        for (uint32_t j = 0; j < c->funs[i].parameter_count; j++) {
            uint32_t original = MO_NO_INDEX;
            if (meta != NULL && meta->body.params.items[j].some)
                original = codegen_string(c, meta->body.params.items[j].value);
            if (codegen_register_add(c, f, c->funs[i].parameters[j].type, original) == MO_NO_INDEX)
                return false;
        }
        for (uint32_t j = c->funs[i].parameter_count; j-- > 0;) {
            struct mo_codegen_index *entry = codegen_index_add(
                c, &f->locals, c->current->params.items[j].name, MO_NO_INDEX, false);
            if (entry == NULL)
                return false;
            entry->value = j;
        }
        f->body = codegen_prepare(c, f, c->current->body, c->funs[i].result_type, 0);
        if (f->body == NULL)
            return false;
    }
    uint32_t ignored = 0;
    if (codegen_type_result(c, mo_types_finish(&c->types), ignored) == MO_NO_INDEX)
        return false;
    for (uint32_t i = declared; i < c->function_count; i++) {
        const struct mo_bytecode_function *fun = &c->funs[i];
        enum mo_type_problem status =
            mo_types_layout(&c->types,
                            &(struct mo_layout){.kind = MO_LAYOUT_CLOSURE,
                                                .type = c->bodies[i].closure_type,
                                                .case_index = MO_NO_INDEX,
                                                .function = i,
                                                .fields = fun->capture_types,
                                                .field_count = fun->capture_count},
                            &ignored);
        if (codegen_type_result(c, status, ignored) == MO_NO_INDEX)
            return false;
    }
    c->layouts = codegen_allocate(c, c->types.count, sizeof *c->layouts);
    if (c->layouts == NULL)
        return false;
    for (uint32_t i = 0; i < c->types.count; i++) {
        const struct mo_type *t = &c->types.items[i];
        c->layouts[i] = MO_NO_INDEX;
        if (t->kind == MO_TYPE_TUPLE || t->kind == MO_TYPE_RECORD) {
            uint32_t count = t->kind == MO_TYPE_TUPLE ? t->parameter_count : t->field_count;
            const uint32_t *fields = t->parameters;
            if (t->kind == MO_TYPE_RECORD) {
                uint32_t *ids = codegen_allocate(c, count, sizeof *ids);
                if (ids == NULL)
                    return false;
                for (uint32_t j = 0; j < count; j++)
                    ids[j] = t->fields[j].type;
                fields = ids;
            }
            enum mo_type_problem status = mo_types_layout(
                &c->types,
                &(struct mo_layout){.kind = t->kind == MO_TYPE_TUPLE ? MO_LAYOUT_TUPLE
                                                                     : MO_LAYOUT_RECORD,
                                    .type = i,
                                    .case_index = MO_NO_INDEX,
                                    .function = MO_NO_INDEX,
                                    .fields = fields,
                                    .field_count = count},
                &c->layouts[i]);
            if (codegen_type_result(c, status, c->layouts[i]) == MO_NO_INDEX)
                return false;
        }
        if (t->kind == MO_TYPE_CHOICE) {
            for (uint32_t j = 0; j < t->case_count; j++) {
                if (mo_case_shape(&c->types, i, j) != MO_CASE_BOXED)
                    continue;
                uint32_t *fields = codegen_allocate(c, t->cases[j].field_count, sizeof *fields);
                if (fields == NULL)
                    return false;
                for (uint32_t k = 0; k < t->cases[j].field_count; k++)
                    fields[k] = t->cases[j].fields[k].type;
                enum mo_type_problem status =
                    mo_types_layout(&c->types,
                                    &(struct mo_layout){.kind = MO_LAYOUT_CASE,
                                                        .type = i,
                                                        .case_index = j,
                                                        .function = MO_NO_INDEX,
                                                        .fields = fields,
                                                        .field_count = t->cases[j].field_count},
                                    &ignored);
                if (codegen_type_result(c, status, ignored) == MO_NO_INDEX)
                    return false;
            }
        }
    }
    return true;
}
/* A normal call keeps the publication point reachable even for a body which always stops.
 * No code follows stop in the value function; its wrapper stores only a returned result. */
MO_MUST_USE bool codegen_initializer(struct compiler *c, struct function *f, uint32_t index) {
    uint32_t global = (index - (uint32_t)c->core->funs.count) / 2;
    uint32_t value = codegen_register_add(c, f, c->globals[global].type, MO_NO_INDEX),
             unit = codegen_register_add(c, f, c->funs[index].result_type, MO_NO_INDEX);
    if (value == MO_NO_INDEX || unit == MO_NO_INDEX)
        return false;
    const uint32_t call[] = {value, index - 1}, store[] = {global, value},
                   result[] = {unit, c->unit_constant};
    struct mo_core_span source =
        f->meta == NULL ? (struct mo_core_span){0} : f->meta->body.spans.items[0];
    return codegen_emit(c, f, source, MO_OP_CALL, call, 2, NULL, 0) != MO_NO_INDEX &&
           codegen_emit(c, f, c->declaration, MO_OP_SET_GLOBAL, store, 2, NULL, 0) != MO_NO_INDEX &&
           codegen_emit(c, f, c->declaration, MO_OP_CONST, result, 2, NULL, 0) != MO_NO_INDEX &&
           codegen_emit(c, f, c->declaration, MO_OP_RET, &unit, 1, NULL, 0) != MO_NO_INDEX;
}
static MO_MUST_USE bool service_shapes(struct compiler *c, struct mo_bytecode_service *services,
                                       const struct mo_bytecode *file) {
    const struct mo_bytecode_service_shape **by_type =
        codegen_allocate(c, c->types.count, sizeof *by_type);
    if (by_type == NULL)
        return false;
    memset(by_type, 0, c->types.count * sizeof *by_type);
    const struct mo_bytecode_service_shape *known = c->shapes.items;
    for (uint32_t i = 0; i < c->shapes.count; i++) {
        MO_ASSERT(known[i].type < c->types.count && by_type[known[i].type] == NULL);
        by_type[known[i].type] = &known[i];
    }
    struct mo_bytecode_service_reach *reach;
    if (!mo_bytecode_service_reach(&c->arena, file, &reach))
        return codegen_problem(c, MO_CODE_CODEGEN_MEMORY,
                               "Code generation exhausted its scratch limit or memory.");
    for (uint32_t i = 0; i < file->service_count; i++) {
        struct mo_bytecode_service *s = &services[i];
        s->shape_count = reach[i].count;
        struct mo_bytecode_service_shape *shapes =
            codegen_allocate(c, s->shape_count, sizeof *shapes);
        if (shapes == NULL)
            return false;
        s->shapes = shapes;
        for (uint32_t j = 0; j < s->shape_count; j++) {
            uint32_t type = reach[i].types[j];
            MO_ASSERT(by_type[type] != NULL);
            shapes[j] = *by_type[type];
        }
    }
    return true;
}
MO_MUST_USE bool codegen_service_descriptors(struct compiler *c, struct mo_bytecode *file) {
    if (c->services == NULL || c->services->count == 0)
        return true;
    if (c->services->count > MO_LIMIT_SERVICES)
        return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                               "Service count exceeds MO_LIMIT_SERVICES.");
    file->service_count = c->services->count;
    struct mo_bytecode_service *items = codegen_allocate(c, file->service_count, sizeof *items);
    if (items == NULL)
        return false;
    file->services = items;
    for (uint32_t i = 0; i < file->service_count; i++) {
        const struct mo_service *s = &c->services->items[i];
        if (s->start >= c->core->funs.count)
            return codegen_problem(c, MO_CODE_CODEGEN_INVALID,
                                   "Service function index is out of range.");
        if (s->tool_count > MO_LIMIT_SERVICE_HANDLERS ||
            s->timer_count > MO_LIMIT_SERVICE_HANDLERS - s->tool_count)
            return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG,
                                   "Service handler count exceeds MO_LIMIT_SERVICE_HANDLERS.");
        struct mo_bytecode_service_tool *tools = codegen_allocate(c, s->tool_count, sizeof *tools);
        struct mo_bytecode_service_timer *timers =
            codegen_allocate(c, s->timer_count, sizeof *timers);
        if (tools == NULL || timers == NULL)
            return false;
        items[i] =
            (struct mo_bytecode_service){.module = c->funs[s->start].module,
                                         .state_type = c->funs[s->start].result_type,
                                         .start = s->start,
                                         .token_env = codegen_string(c, codegen_text(s->token_env)),
                                         .tool_count = s->tool_count,
                                         .tools = tools,
                                         .timer_count = s->timer_count,
                                         .timers = timers};
        for (uint32_t j = 0; j < s->tool_count; j++) {
            const struct mo_service_tool *t = &s->tools[j];
            if (t->function >= c->core->funs.count || c->funs[t->function].parameter_count < 2)
                return codegen_problem(c, MO_CODE_CODEGEN_INVALID,
                                       "Invalid service handler signature.");
            const struct mo_bytecode_function *fn = &c->funs[t->function];
            uint32_t count = fn->parameter_count - 2;
            if (count != t->argument_count)
                return codegen_problem(c, MO_CODE_CODEGEN_INVALID,
                                       "Invalid service handler signature.");
            uint32_t *arguments = codegen_allocate(c, count, sizeof *arguments);
            uint32_t *names = codegen_allocate(c, count, sizeof *names);
            if (arguments == NULL || names == NULL)
                return false;
            for (uint32_t k = 0; k < count; k++) {
                arguments[k] = fn->parameters[k + 2].type;
                names[k] = codegen_string(c, codegen_text(t->argument_names[k]));
            }
            tools[j] = (struct mo_bytecode_service_tool){
                .name = codegen_string(c, codegen_text(t->name)),
                .function = t->function,
                .read_only = t->read_only,
                .argument_count = count,
                .argument_types = arguments,
                .result_type = fn->result_type,
                .description = codegen_string(c, codegen_text(t->description)),
                .argument_name_count = count,
                .argument_names = names};
        }
        for (uint32_t j = 0; j < s->timer_count; j++) {
            if (s->timers[j].function >= c->core->funs.count ||
                s->timers[j].interval >= c->core->funs.count)
                return codegen_problem(c, MO_CODE_CODEGEN_INVALID,
                                       "Service function index is out of range.");
            timers[j] =
                (struct mo_bytecode_service_timer){s->timers[j].function, s->timers[j].interval};
        }
    }
    file->string_count = c->strings.count;
    file->strings = c->strings.items;
    return c->error == 0 && service_shapes(c, items, file);
}
