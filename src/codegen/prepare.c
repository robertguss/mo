#include "codegen/internal.h"

#include <stdio.h>

#include "base/codes.h"

/* Capture lookup happens while all enclosing lexical scopes are still active. Intermediate
 * closures retain a grandparent's value even if only a nested closure uses it. */
static MO_MUST_USE uint32_t variable(struct compiler *c, struct function *f, mo_core_string key,
                                     unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    struct mo_codegen_index *entry = mo_codegen_find(f->locals, key);
    if (entry != NULL && entry->value != MO_NO_INDEX)
        return entry->value;
    MO_ASSERT(f->parent != NULL);
    uint32_t source = variable(c, f->parent, key, depth + 1);
    if (source == MO_NO_INDEX)
        return MO_NO_INDEX;
    mo_core_string capture_key = {(const char *)&source, sizeof source};
    struct mo_codegen_index *old = mo_codegen_find(f->capture_index, capture_key);
    uint32_t r;
    if (old != NULL) {
        r = old->value;
    } else {
        struct reg parent = ((struct reg *)f->parent->regs.items)[source];
        r = codegen_register_add(c, f, parent.type, parent.name);
        struct capture *item =
            codegen_append(c, &f->captures, 1, MO_LIMIT_FIELDS, MO_CODE_BYTECODE_TOO_BIG);
        if (r == MO_NO_INDEX || item == NULL ||
            codegen_index_add(c, &f->capture_index, capture_key, r, true) == NULL)
            return MO_NO_INDEX;
        *item = (struct capture){source, r};
    }
    entry = codegen_index_add(c, &f->locals, key, r, false);
    if (entry == NULL)
        return MO_NO_INDEX;
    entry->value = r;
    return r;
}
/* Only inference failures need a later context. Resolve them once, without retrying the Core
 * walk: an if/case may infer from a later arm and type an earlier stopping lambda. */
static MO_MUST_USE bool context(struct compiler *c, struct function *f, struct node *n,
                                uint32_t expected, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    if (expected == MO_NO_INDEX || n->type != MO_NO_INDEX)
        return true;
    if (n->core->tag == MO_CORE_EXPR_STOP || n->core->tag == MO_CORE_EXPR_FAIL ||
        n->core->tag == MO_CORE_EXPR_JUMP)
        return true;
    if (n->core->tag == MO_CORE_EXPR_LAM) {
        struct function *lifted = &c->bodies[n->index];
        uint32_t result = c->types.items[expected].result;
        c->funs[n->index].result_type = result;
        lifted->closure_type = expected;
        if (!context(c, lifted, lifted->body, result, depth + 1))
            return false;
    } else if (n->core->tag == MO_CORE_EXPR_LET) {
        if (!context(c, f, n->children[1], expected, depth + 1))
            return false;
        n->type = n->children[1]->type;
        n->reg = n->children[1]->reg;
        n->temporary = n->children[1]->temporary || n->reg == n->binds[0];
        return true;
    } else if (n->core->tag == MO_CORE_EXPR_JOIN) {
        for (uint32_t i = 0; i < n->count; i++)
            if (!context(c, f, n->children[i], expected, depth + 1))
                return false;
    } else {
        MO_ASSERT(n->core->tag == MO_CORE_EXPR_IF || n->core->tag == MO_CORE_EXPR_CASE);
        for (uint32_t i = 1; i < n->count; i++) {
            struct node *body =
                n->core->tag == MO_CORE_EXPR_CASE ? n->children[i]->children[0] : n->children[i];
            if (!context(c, f, body, expected, depth + 1))
                return false;
        }
    }
    n->type = expected;
    n->reg = codegen_register_add(c, f, expected, MO_NO_INDEX);
    return n->reg != MO_NO_INDEX;
}
static MO_MUST_USE bool lambda(struct compiler *c, struct function *parent, struct node *n,
                               uint32_t preorder, uint32_t expected, unsigned depth) {
    MO_ASSERT(c->function_count < c->function_capacity);
    uint32_t index = c->function_count++;
    n->index = index;
    struct function *f = &c->bodies[index];
    *f = (struct function){.parent = parent,
                           .root = parent->root,
                           .meta = parent->meta,
                           .source = parent->source,
                           .preorder = parent->preorder,
                           .bindings = parent->bindings,
                           .declaration = n->span};
    codegen_function_init(f);
    uint32_t number = ++f->root->lambda_number;
    struct mo_codegen_index *named_closure = mo_codegen_find(
        f->root->closure_names, (mo_core_string){(const char *)&preorder, sizeof preorder});
    char suffix[32];
    int length = snprintf(suffix, sizeof suffix, ".fn.%u", number);
    MO_ASSERT(length > 0 && (size_t)length < sizeof suffix);
    uint32_t name_id = named_closure == NULL ? codegen_suffixed_name(c, f->source->name, suffix)
                                             : named_closure->value;
    uint32_t parent_index = (uint32_t)(parent - c->bodies);
    mo_core_params ps = n->core->as.lam.params;
    if (ps.count > MO_LIMIT_PARAMETERS)
        return codegen_problem(c, MO_CODE_BYTECODE_TOO_BIG, "Closure exceeds MO_LIMIT_PARAMETERS.");
    struct mo_parameter *parameters = codegen_allocate(c, ps.count, sizeof *parameters);
    uint32_t *types = codegen_allocate(c, ps.count, sizeof *types);
    if (name_id == MO_NO_INDEX || parameters == NULL || types == NULL)
        return false;
    c->parameters[index] = parameters;
    c->funs[index] = (struct mo_bytecode_function){.name = name_id,
                                                   .module = c->funs[parent_index].module,
                                                   .kind = MO_FUNCTION_CLOSURE,
                                                   .spec_code = c->funs[parent_index].spec_code,
                                                   .parameter_count = (uint32_t)ps.count,
                                                   .parameters = parameters};
    for (uint32_t i = 0; i < ps.count; i++) {
        if (!codegen_name_valid(c, ps.items[i].name))
            return false;
        types[i] = codegen_type(c, ps.items[i].ty, 0);
        parameters[i] = (struct mo_parameter){types[i], MO_PARAMETER_OWNED};
        if (codegen_register_add(c, f, types[i], codegen_binding_name(parent, preorder, i)) ==
            MO_NO_INDEX)
            return false;
    }
    uint32_t env = MO_NO_INDEX;
    enum mo_type_problem status =
        mo_types_intern(&c->types, &(struct mo_type){.kind = MO_TYPE_ENV, .function = index}, &env);
    if (codegen_type_result(c, status, env) == MO_NO_INDEX ||
        codegen_register_add(c, f, env, MO_NO_INDEX) == MO_NO_INDEX)
        return false;
    for (uint32_t i = (uint32_t)ps.count; i-- > 0;) {
        struct mo_codegen_index *entry =
            codegen_index_add(c, &f->locals, ps.items[i].name, i, false);
        if (entry == NULL)
            return false;
        entry->value = i;
    }
    uint32_t result = expected == MO_NO_INDEX ? MO_NO_INDEX : c->types.items[expected].result;
    f->body = codegen_prepare(c, f, n->core->as.lam.body, result, depth + 1);
    parent->preorder = f->preorder;
    if (f->body == NULL)
        return false;
    if (result == MO_NO_INDEX)
        result = f->body->type;
    c->funs[index].result_type = result;
    if (result != MO_NO_INDEX) {
        status = mo_types_intern(&c->types,
                                 &(struct mo_type){.kind = MO_TYPE_FUNCTION,
                                                   .parameters = types,
                                                   .parameter_count = (uint32_t)ps.count,
                                                   .result = result},
                                 &n->type);
        if (codegen_type_result(c, status, n->type) == MO_NO_INDEX)
            return false;
    }
    uint32_t *captures = codegen_allocate(c, f->captures.count, sizeof *captures);
    n->binds = codegen_allocate(c, f->captures.count, sizeof *n->binds);
    if (captures == NULL || n->binds == NULL)
        return false;
    n->bind_count = f->captures.count;
    for (uint32_t i = 0; i < f->captures.count; i++) {
        const struct capture *item = &((struct capture *)f->captures.items)[i];
        n->binds[i] = item->source;
        captures[i] = ((struct reg *)f->regs.items)[item->reg].type;
    }
    c->funs[index].capture_count = f->captures.count;
    c->funs[index].capture_types = captures;
    f->closure_type = n->type;
    return true;
}
static MO_MUST_USE bool children(struct compiler *c, struct function *f, struct node *n,
                                 mo_core_expr_list xs, uint32_t expected, unsigned depth) {
    n->count = (uint32_t)xs.count;
    n->children = codegen_allocate(c, xs.count, sizeof *n->children);
    if (n->children == NULL)
        return false;
    for (uint32_t i = 0; i < n->count; i++) {
        uint32_t child_type = MO_NO_INDEX;
        if (n->core->tag == MO_CORE_EXPR_CALL)
            child_type = c->funs[n->index].parameters[i].type;
        else if (n->core->tag == MO_CORE_EXPR_JUMP) {
            const struct node *target = ((struct join *)f->joins.items)[n->index].node;
            child_type = ((struct reg *)f->regs.items)[target->binds[i]].type;
        } else if (n->core->tag == MO_CORE_EXPR_CON) {
            const struct mo_type *t = &c->types.items[n->type];
            child_type =
                t->kind == MO_TYPE_RECORD ? t->fields[i].type : t->cases[n->index].fields[i].type;
        } else if (n->core->tag == MO_CORE_EXPR_IF)
            child_type = i == 0 ? codegen_scalar(c, MO_TYPE_BOOL) : expected;
        else if (n->core->tag == MO_CORE_EXPR_PRIM) {
            uint32_t slot = n->operation.literal == MO_CODEGEN_LITERAL_LIST  ? 1
                            : n->operation.literal == MO_CODEGEN_LITERAL_MAP ? 1 + i % 2
                                                                             : i;
            child_type = c->types.items[n->signature].parameters[slot];
        }
        n->children[i] = codegen_prepare(c, f, xs.items[i], child_type, depth + 1);
        if (n->children[i] == NULL)
            return false;
    }
    return true;
}
MO_MUST_USE struct node *codegen_prepare(struct compiler *c, struct function *f, mo_core_expr expr,
                                         uint32_t expected, unsigned depth) {
    if (depth >= MO_LIMIT_CORE_DEPTH) {
        if (!codegen_problem(c, MO_CODE_NESTED_TOO_DEEPLY,
                             "Core expression nesting exceeds MO_LIMIT_CORE_DEPTH."))
            return NULL;
    }
    uint32_t preorder = f->preorder++;
    struct node *n = codegen_allocate(c, 1, sizeof *n);
    if (n == NULL)
        return NULL;
    *n = (struct node){.core = expr,
                       .type = MO_NO_INDEX,
                       .reg = MO_NO_INDEX,
                       .index = MO_NO_INDEX,
                       .temporary = true};
    if (f->meta != NULL)
        n->span = f->meta->body.spans.items[preorder];
    c->span = n->span;
    mo_core_expr small[3];
    switch (expr->tag) {
    case MO_CORE_EXPR_LIT:
        n->index = codegen_literal(c, expr->as.lit, &n->type);
        break;
    case MO_CORE_EXPR_VAR: {
        n->reg = variable(c, f, expr->as.var, 0);
        if (n->reg == MO_NO_INDEX)
            return NULL;
        n->type = ((struct reg *)f->regs.items)[n->reg].type;
        n->temporary = false;
        return n;
    }
    case MO_CORE_EXPR_GLOBAL: {
        struct mo_codegen_index *entry = mo_codegen_find(c->global_index, expr->as.global);
        MO_ASSERT(entry != NULL);
        n->index = entry->value;
        n->type = c->globals[n->index].type;
        break;
    }
    case MO_CORE_EXPR_LET: {
        if (!codegen_name_valid(c, expr->as.let_.name))
            return NULL;
        n->children = codegen_allocate(c, 2, sizeof *n->children);
        n->binds = codegen_allocate(c, 1, sizeof *n->binds);
        if (n->children == NULL || n->binds == NULL)
            return NULL;
        n->count = 2;
        n->bind_count = 1;
        n->children[0] = codegen_prepare(c, f, expr->as.let_.bound, MO_NO_INDEX, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        n->binds[0] =
            codegen_register_add(c, f, n->children[0]->type, codegen_binding_name(f, preorder, 0));
        struct mo_codegen_index *entry =
            codegen_index_add(c, &f->locals, expr->as.let_.name, MO_NO_INDEX, false);
        if (entry == NULL || n->binds[0] == MO_NO_INDEX)
            return NULL;
        uint32_t saved = entry->value;
        entry->value = n->binds[0];
        n->children[1] = codegen_prepare(c, f, expr->as.let_.body, expected, depth + 1);
        entry->value = saved;
        if (n->children[1] == NULL)
            return NULL;
        n->type = n->children[1]->type;
        n->reg = n->children[1]->reg;
        n->temporary = n->children[1]->temporary || n->reg == n->binds[0];
        return n;
    }
    case MO_CORE_EXPR_CALL: {
        struct mo_codegen_index *entry = mo_codegen_find(c->fun_index, expr->as.call.name);
        MO_ASSERT(entry != NULL);
        n->index = entry->value;
        n->type = c->funs[n->index].result_type;
        if (!children(c, f, n, expr->as.call.args, MO_NO_INDEX, depth))
            return NULL;
        break;
    }
    case MO_CORE_EXPR_IF:
        small[0] = expr->as.if_.cond;
        small[1] = expr->as.if_.then_;
        small[2] = expr->as.if_.else_;
        if (!children(c, f, n, (mo_core_expr_list){small, 3}, expected, depth))
            return NULL;
        n->type = n->children[1]->type != MO_NO_INDEX ? n->children[1]->type : n->children[2]->type;
        if (n->type == MO_NO_INDEX)
            n->type = expected;
        for (uint32_t i = 1; i < n->count; i++)
            if (!context(c, f, n->children[i], n->type, depth + 1))
                return NULL;
        break;
    case MO_CORE_EXPR_CON:
        n->type = codegen_type(c, expr->as.con.ty, 0);
        if (n->type == MO_NO_INDEX)
            return NULL;
        n->index = codegen_constructor(c, n->type, expr->as.con.ctor);
        if (n->index == MO_NO_INDEX || !children(c, f, n, expr->as.con.args, MO_NO_INDEX, depth))
            return NULL;
        break;
    case MO_CORE_EXPR_CASE: {
        n->count = 1 + (uint32_t)expr->as.case_.arms.count;
        n->children = codegen_allocate(c, n->count, sizeof *n->children);
        n->cases = codegen_allocate(c, n->count - 1, sizeof *n->cases);
        if (n->children == NULL || n->cases == NULL)
            return NULL;
        n->children[0] = codegen_prepare(c, f, expr->as.case_.expr, MO_NO_INDEX, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        uint32_t ty = n->children[0]->type;
        MO_ASSERT(ty != MO_NO_INDEX);
        uint32_t slot = 0;
        for (uint32_t i = 1; i < n->count; i++) {
            const struct mo_core_arm *arm = &expr->as.case_.arms.items[i - 1];
            uint32_t case_id = codegen_constructor(c, ty, arm->ctor);
            if (case_id == MO_NO_INDEX)
                return NULL;
            n->cases[i - 1] = case_id;
            const struct mo_type *t = &c->types.items[ty];
            struct mo_type_case record = {.fields = t->fields, .field_count = t->field_count};
            const struct mo_type_case *ctor =
                t->kind == MO_TYPE_RECORD ? &record : &t->cases[case_id];
            struct mo_codegen_index **entries =
                codegen_allocate(c, arm->names.count, sizeof *entries);
            uint32_t *saved = codegen_allocate(c, arm->names.count, sizeof *saved);
            uint32_t *regs = codegen_allocate(c, arm->names.count, sizeof *regs);
            if (entries == NULL || saved == NULL || regs == NULL)
                return NULL;
            for (uint32_t j = 0; j < ctor->field_count; j++) {
                c->span = n->span;
                if (!codegen_name_valid(c, arm->names.items[j]))
                    return NULL;
                regs[j] = codegen_register_add(c, f, ctor->fields[j].type,
                                               codegen_binding_name(f, preorder, slot++));
                entries[j] =
                    codegen_index_add(c, &f->locals, arm->names.items[j], MO_NO_INDEX, false);
                if (regs[j] == MO_NO_INDEX || entries[j] == NULL)
                    return NULL;
            }
            for (uint32_t j = ctor->field_count; j-- > 0;) {
                saved[j] = entries[j]->value;
                entries[j]->value = regs[j];
            }
            struct node *body = codegen_prepare(c, f, arm->body, expected, depth + 1);
            for (uint32_t j = 0; j < ctor->field_count; j++)
                entries[j]->value = saved[j];
            if (body == NULL)
                return NULL;
            /* Arm wrapper holds pattern registers independently of its body's binders. */
            struct node *wrapper = codegen_allocate(c, 1, sizeof *wrapper);
            if (wrapper == NULL)
                return NULL;
            *wrapper = *body;
            wrapper->children = codegen_allocate(c, 1, sizeof *wrapper->children);
            if (wrapper->children == NULL)
                return NULL;
            wrapper->children[0] = body;
            wrapper->binds = regs;
            wrapper->bind_count = ctor->field_count;
            n->children[i] = wrapper;
            if (n->type == MO_NO_INDEX)
                n->type = body->type;
        }
        if (n->type == MO_NO_INDEX)
            n->type = expected;
        for (uint32_t i = 1; i < n->count; i++)
            if (!context(c, f, n->children[i]->children[0], n->type, depth + 1))
                return NULL;
        break;
    }
    case MO_CORE_EXPR_STOP: {
        uint32_t ty = codegen_scalar(c, MO_TYPE_TEXT), s = codegen_string(c, expr->as.stop);
        if (ty == MO_NO_INDEX || s == MO_NO_INDEX)
            return NULL;
        struct mo_bytecode_constant k = {.type = ty};
        k.value.string = s;
        n->index = codegen_constant(c, k);
        n->reg = codegen_register_add(c, f, ty, MO_NO_INDEX);
        return c->error == 0 ? n : NULL;
    }
    case MO_CORE_EXPR_TUPLE: {
        if (expr->as.tuple.count < 2) {
            if (!codegen_problem(c, MO_CODE_CODEGEN_REPRESENTATION,
                                 "Zero/one-element tuples cannot come from lowering."))
                return NULL;
        }
        if (!children(c, f, n, expr->as.tuple, MO_NO_INDEX, depth))
            return NULL;
        uint32_t *params = codegen_allocate(c, n->count, sizeof *params);
        if (params == NULL)
            return NULL;
        for (uint32_t i = 0; i < n->count; i++)
            params[i] = n->children[i]->type;
        enum mo_type_problem status = mo_types_intern(
            &c->types,
            &(struct mo_type){
                .kind = MO_TYPE_TUPLE, .parameters = params, .parameter_count = n->count},
            &n->type);
        if (codegen_type_result(c, status, n->type) == MO_NO_INDEX)
            return NULL;
        break;
    }
    case MO_CORE_EXPR_PROJ:
        small[0] = expr->as.proj.expr;
        if (!children(c, f, n, (mo_core_expr_list){small, 1}, MO_NO_INDEX, depth))
            return NULL;
        n->index = 0;
        for (size_t i = 0; i < expr->as.proj.index.digits.length; i++)
            n->index = n->index * 10u + (unsigned)(expr->as.proj.index.digits.bytes[i] - '0');
        n->type = c->types.items[n->children[0]->type].parameters[n->index];
        break;
    case MO_CORE_EXPR_FAIL:
        small[0] = expr->as.fail;
        if (!children(c, f, n, (mo_core_expr_list){small, 1}, MO_NO_INDEX, depth))
            return NULL;
        return n;
    case MO_CORE_EXPR_PRIM: {
        const struct mo_core_prim *p = &expr->as.prim.prim;
        if (p->tag == MO_CORE_PRIM_SIM) {
            n->operation.opcode = MO_OP_SIM;
            n->signature = codegen_simulation_signature(c, &p->as.sim);
            if (n->signature == MO_NO_INDEX)
                return NULL;
            n->type = c->types.items[n->signature].result;
            if (!children(c, f, n, expr->as.prim.args, MO_NO_INDEX, depth))
                return NULL;
            break;
        }
        if (!mo_codegen_operation(p, &n->operation)) {
            if (!codegen_unsupported(c, "The Core primitive has no code generation route."))
                return NULL;
        }
        mo_core_ty variables[4] = {NULL};
        if (p->tag == MO_CORE_PRIM_LIST) {
            variables[0] = p->as.list.t;
            if (p->as.list.op.tag == MO_CORE_LIST_OP_ZIP)
                variables[1] = p->as.list.op.as.zip;
        } else if (p->tag == MO_CORE_PRIM_MAP) {
            variables[2] = p->as.map.k;
            variables[3] = p->as.map.v;
        } else if (p->tag == MO_CORE_PRIM_SET)
            variables[0] = p->as.set.t;
        else if (p->tag == MO_CORE_PRIM_EQ)
            variables[0] = p->as.eq;
        else if (p->tag == MO_CORE_PRIM_STRUCTURAL)
            variables[0] = p->as.structural;
        n->signature = codegen_operation_signature(c, &n->operation, variables);
        if (n->signature == MO_NO_INDEX)
            return NULL;
        n->type = n->operation.compare ? codegen_scalar(c, MO_TYPE_BOOL)
                                       : c->types.items[n->signature].result;
        if (!children(c, f, n, expr->as.prim.args, MO_NO_INDEX, depth))
            return NULL;
        if (n->operation.expansion == MO_CODEGEN_EXPAND_RANGE) {
            n->bind_count = 6;
            n->binds = codegen_allocate(c, n->bind_count, sizeof *n->binds);
            n->cases = codegen_allocate(c, 2, sizeof *n->cases);
            struct mo_codegen_operation convert = mo_codegen_primitive(MO_PRIM_INT_TO_TEXT),
                                        concat = mo_codegen_primitive(MO_PRIM_TEXT_CONCAT);
            if (n->binds == NULL || n->cases == NULL)
                return NULL;
            n->cases[0] = codegen_operation_signature(c, &convert, variables);
            n->cases[1] = codegen_operation_signature(c, &concat, variables);
            uint32_t integer = codegen_scalar(c, MO_TYPE_INT),
                     separator = codegen_string(c, codegen_text(".."));
            if (n->cases[0] == MO_NO_INDEX || n->cases[1] == MO_NO_INDEX ||
                integer == MO_NO_INDEX || separator == MO_NO_INDEX)
                return NULL;
            n->index = codegen_constant(
                c, (struct mo_bytecode_constant){.type = n->type, .value.string = separator});
            if (n->index == MO_NO_INDEX)
                return NULL;
            for (uint32_t i = 0; i < n->bind_count; i++)
                if ((n->binds[i] = codegen_register_add(c, f, i < 2 ? integer : n->type,
                                                        MO_NO_INDEX)) == MO_NO_INDEX)
                    return NULL;
        } else if (n->operation.expansion == MO_CODEGEN_EXPAND_UNCONS) {
            n->bind_count = 5;
            n->binds = codegen_allocate(c, n->bind_count, sizeof *n->binds);
            n->cases = codegen_allocate(c, 2, sizeof *n->cases);
            struct mo_codegen_operation first = mo_codegen_primitive(MO_PRIM_LIST_FIRST),
                                        rest = mo_codegen_primitive(MO_PRIM_LIST_DROP);
            if (n->binds == NULL || n->cases == NULL)
                return NULL;
            n->cases[0] = codegen_operation_signature(c, &first, variables);
            n->cases[1] = codegen_operation_signature(c, &rest, variables);
            if (n->cases[0] == MO_NO_INDEX || n->cases[1] == MO_NO_INDEX)
                return NULL;
            uint32_t option = c->types.items[n->cases[0]].result,
                     pair = c->types.items[n->type].cases[1].fields[0].type;
            const uint32_t types[] = {option, c->types.items[option].cases[1].fields[0].type,
                                      c->types.items[n->signature].parameters[0],
                                      c->types.items[n->cases[1]].parameters[1], pair};
            n->index = codegen_constant(
                c, (struct mo_bytecode_constant){.type = types[3], .value.immediate = {1}});
            if (n->index == MO_NO_INDEX)
                return NULL;
            for (uint32_t i = 0; i < n->bind_count; i++)
                if ((n->binds[i] = codegen_register_add(c, f, types[i], MO_NO_INDEX)) ==
                    MO_NO_INDEX)
                    return NULL;
        }
        if (n->operation.compare) {
            n->binds = codegen_allocate(c, 2, sizeof *n->binds);
            if (n->binds == NULL)
                return NULL;
            uint32_t ordering = c->types.items[n->signature].result;
            for (uint32_t i = 0; i < 2; i++)
                if ((n->binds[i] = codegen_register_add(c, f, ordering, MO_NO_INDEX)) ==
                    MO_NO_INDEX)
                    return NULL;
        }
        if (n->operation.literal != MO_CODEGEN_LITERAL_NONE) {
            bool map = n->operation.literal == MO_CODEGEN_LITERAL_MAP;
            struct mo_codegen_operation empty =
                mo_codegen_primitive(map ? MO_PRIM_MAP_EMPTY : MO_PRIM_LIST_EMPTY);
            n->index = codegen_operation_signature(c, &empty, variables);
            n->bind_count = n->count / (map ? 2u : 1u);
            n->binds = codegen_allocate(c, n->bind_count, sizeof *n->binds);
            if (n->index == MO_NO_INDEX || n->binds == NULL)
                return NULL;
            for (uint32_t i = 0; i < n->bind_count; i++)
                if ((n->binds[i] = codegen_register_add(c, f, n->type, MO_NO_INDEX)) == MO_NO_INDEX)
                    return NULL;
        }
        break;
    }
    case MO_CORE_EXPR_LAM:
        if (!lambda(c, f, n, preorder, expected, depth))
            return NULL;
        break;
    case MO_CORE_EXPR_APP: {
        n->count = 1 + (uint32_t)expr->as.app.args.count;
        n->children = codegen_allocate(c, n->count, sizeof *n->children);
        if (n->children == NULL)
            return NULL;
        n->children[0] = codegen_prepare(c, f, expr->as.app.f, MO_NO_INDEX, depth + 1);
        if (n->children[0] == NULL)
            return NULL;
        n->type = c->types.items[n->children[0]->type].result;
        for (uint32_t i = 1; i < n->count; i++) {
            uint32_t arg_type = c->types.items[n->children[0]->type].parameters[i - 1];
            n->children[i] =
                codegen_prepare(c, f, expr->as.app.args.items[i - 1], arg_type, depth + 1);
            if (n->children[i] == NULL)
                return NULL;
        }
        break;
    }
    case MO_CORE_EXPR_JOIN: {
        mo_core_params ps = expr->as.join.params;
        if (!codegen_name_valid(c, expr->as.join.name))
            return NULL;
        n->index = f->joins.count;
        struct join *item =
            codegen_append(c, &f->joins, 1, MO_LIMIT_CORE_NODES, MO_CODE_CODEGEN_MEMORY);
        n->count = 2;
        n->bind_count = (uint32_t)ps.count;
        n->binds = codegen_allocate(c, ps.count, sizeof *n->binds);
        n->children = codegen_allocate(c, 2, sizeof *n->children);
        struct mo_codegen_index **entries = codegen_allocate(c, ps.count, sizeof *entries);
        uint32_t *saved = codegen_allocate(c, ps.count, sizeof *saved);
        struct mo_codegen_index *entry =
            codegen_index_add(c, &f->join_names, expr->as.join.name, MO_NO_INDEX, false);
        if (item == NULL || n->binds == NULL || n->children == NULL || entries == NULL ||
            saved == NULL || entry == NULL)
            return NULL;
        *item = (struct join){.node = n, .offset = MO_NO_INDEX, .checkpoint = MO_NO_INDEX};
        for (uint32_t i = 0; i < ps.count; i++) {
            if (!codegen_name_valid(c, ps.items[i].name))
                return NULL;
            uint32_t ty = codegen_type(c, ps.items[i].ty, 0);
            n->binds[i] = codegen_register_add(c, f, ty, codegen_binding_name(f, preorder, i));
            entries[i] = codegen_index_add(c, &f->locals, ps.items[i].name, MO_NO_INDEX, false);
            if (n->binds[i] == MO_NO_INDEX || entries[i] == NULL)
                return NULL;
        }
        uint32_t prior = entry->value;
        entry->value = n->index;
        for (uint32_t i = (uint32_t)ps.count; i-- > 0;) {
            saved[i] = entries[i]->value;
            entries[i]->value = n->binds[i];
        }
        n->children[0] = codegen_prepare(c, f, expr->as.join.body, expected, depth + 1);
        for (uint32_t i = 0; i < ps.count; i++)
            entries[i]->value = saved[i];
        n->children[1] = n->children[0] == NULL
                             ? NULL
                             : codegen_prepare(c, f, expr->as.join.rest, expected, depth + 1);
        entry->value = prior;
        if (n->children[0] == NULL || n->children[1] == NULL)
            return NULL;
        n->type = expected != MO_NO_INDEX               ? expected
                  : n->children[1]->type != MO_NO_INDEX ? n->children[1]->type
                                                        : n->children[0]->type;
        for (uint32_t i = 0; i < n->count; i++)
            if (!context(c, f, n->children[i], n->type, depth + 1))
                return NULL;
        break;
    }
    case MO_CORE_EXPR_JUMP: {
        struct mo_codegen_index *entry = mo_codegen_find(f->join_names, expr->as.jump.name);
        MO_ASSERT(entry != NULL && entry->value != MO_NO_INDEX);
        n->index = entry->value;
        if (!children(c, f, n, expr->as.jump.args, MO_NO_INDEX, depth))
            return NULL;
        n->bind_count = n->count;
        n->binds = codegen_allocate(c, n->count, sizeof *n->binds);
        if (n->binds == NULL)
            return NULL;
        const struct node *target = ((struct join *)f->joins.items)[n->index].node;
        for (uint32_t i = 0; i < n->count; i++) {
            uint32_t ty = ((struct reg *)f->regs.items)[target->binds[i]].type;
            n->binds[i] = codegen_register_add(c, f, ty, MO_NO_INDEX);
            if (n->binds[i] == MO_NO_INDEX)
                return NULL;
        }
        return n;
    }
    }
    if (c->error != 0)
        return NULL;
    n->reg = codegen_register_add(c, f, n->type, MO_NO_INDEX);
    return c->error == 0 ? n : NULL;
}
