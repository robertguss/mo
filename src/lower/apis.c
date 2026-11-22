#include "lower/scalar.h"

#include <stdio.h>
#include <string.h>

static mo_core_expr_list api_args(struct lowering *p, size_t count) {
    return (mo_core_expr_list){LOWER_NEW(p, mo_core_expr, count), count};
}

static mo_core_expr api_text(struct lowering *p, mo_core_string value) {
    mo_core_expr e = lower_unit(p);
    e->as.lit->tag = MO_CORE_LIT_TEXT;
    e->as.lit->as.text = value;
    return e;
}

static mo_core_expr api_cstr(struct lowering *p, const char *s) {
    return api_text(p, lower_text(p, s));
}

static mo_core_expr api_let(struct lowering *p, mo_core_string name, mo_core_expr bound,
                            mo_core_expr body) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_LET);
    e->as.let_.name = name;
    e->as.let_.bound = bound;
    e->as.let_.body = body;
    return e;
}

static mo_core_expr api_con(struct lowering *p, mo_core_ty ty, const char *ctor,
                            mo_core_expr_list values) {
    mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CON);
    e->as.con.ty = ty;
    e->as.con.ctor = lower_text(p, ctor);
    e->as.con.args = values;
    return e;
}

static mo_core_expr api_con1(struct lowering *p, mo_core_ty ty, const char *ctor, mo_core_expr a) {
    mo_core_expr_list values = api_args(p, 1);
    values.items[0] = a;
    return api_con(p, ty, ctor, values);
}

static const struct mo_module_scope *lib_scope(struct lowering *p, const char *mod_name) {
    if (p->program->interfaces != NULL)
        for (uint32_t i = 0; i < p->program->interfaces->count; i++) {
            lower_work(p, 1);
            const struct mo_module_scope *scope = &p->program->interfaces->scopes[i];
            if (scope->library && scope->name != NULL && strcmp(scope->name, mod_name) == 0)
                return scope;
        }
    lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "api lowering needs the standard library");
}

static const struct mo_source_type *lib_data_source(struct lowering *p, const char *mod_name,
                                                    const char *type_name) {
    const struct mo_module_scope *scope = lib_scope(p, mod_name);
    const struct mo_symbol *sym =
        mo_find_symbol(scope, (const uint8_t *)type_name, strlen(type_name));
    MO_ASSERT(sym != NULL);
    for (uint32_t i = 0; i < p->program->signatures->data_count; i++) {
        lower_work(p, 1);
        if (p->program->signatures->data[i].type->declaration == sym)
            return p->program->signatures->data[i].type;
    }
    MO_UNREACHABLE();
}

static mo_core_expr lib_call(struct lowering *p, const char *mod_name, const char *fn_name,
                             mo_core_expr_list values) {
    const struct mo_module_scope *scope = lib_scope(p, mod_name);
    const struct mo_symbol *sym = mo_find_symbol(scope, (const uint8_t *)fn_name, strlen(fn_name));
    MO_ASSERT(sym != NULL && sym->declaration->kind == MO_S_FN && sym->overload_count == 1);
    struct instance *f =
        lower_instance(p, sym, (struct mo_type_arguments){0}, NULL, 0, NULL, sym->module, 0);
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = f->fun.name;
    call->as.call.args = values;
    return call;
}

static mo_core_expr api_lib_call0(struct lowering *p, const char *fn_name) {
    return lib_call(p, "Api", fn_name, api_args(p, 0));
}

static mo_core_expr api_lib_call1(struct lowering *p, const char *fn_name, mo_core_expr a) {
    mo_core_expr_list v = api_args(p, 1);
    v.items[0] = a;
    return lib_call(p, "Api", fn_name, v);
}

static mo_core_expr api_lib_call2(struct lowering *p, const char *fn_name, mo_core_expr a,
                                  mo_core_expr b) {
    mo_core_expr_list v = api_args(p, 2);
    v.items[0] = a;
    v.items[1] = b;
    return lib_call(p, "Api", fn_name, v);
}

static mo_core_expr api_lib_call4(struct lowering *p, const char *fn_name, mo_core_expr a,
                                  mo_core_expr b, mo_core_expr c, mo_core_expr d) {
    mo_core_expr_list v = api_args(p, 4);
    v.items[0] = a;
    v.items[1] = b;
    v.items[2] = c;
    v.items[3] = d;
    return lib_call(p, "Api", fn_name, v);
}

static mo_core_ty builtin_ty(struct lowering *p, const char *name) {
    struct mo_source_type *src = LOWER_NEW(p, struct mo_source_type, 1);
    *src = (struct mo_source_type){.kind = MO_SOURCE_NAMED, .name = name, .module = UINT32_MAX};
    return lower_type(p, src, NULL);
}

static mo_core_expr json_obj(struct lowering *p, mo_core_ty json_ty, size_t count,
                             const mo_core_string *keys, const mo_core_expr *vals) {
    mo_core_ty text_ty = builtin_ty(p, "Text");
    mo_core_expr_list flat = api_args(p, 2 * count);
    for (size_t i = 0; i < count; i++) {
        flat.items[2 * i] = api_text(p, keys[i]);
        flat.items[2 * i + 1] = vals[i];
    }
    mo_core_expr map_lit =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                         .as.map = {.k = text_ty,
                                                    .v = json_ty,
                                                    .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                           .lit.digits = lower_number(p, count)}}},
                   flat);
    return api_con1(p, json_ty, "Object", map_lit);
}

static mo_core_expr json_arr(struct lowering *p, mo_core_ty json_ty, size_t count,
                             const mo_core_expr *items) {
    mo_core_expr_list list_args = api_args(p, count);
    for (size_t i = 0; i < count; i++)
        list_args.items[i] = items[i];
    mo_core_expr list_lit = lower_prim(
        p,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = json_ty,
                                          .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                 .as.lit.digits = lower_number(p, count)}}},
        list_args);
    return api_con1(p, json_ty, "Array", list_lit);
}

static const char *method_upper(enum mo_api_method m) {
    switch (m) {
    case MO_API_GET:
        return "GET";
    case MO_API_POST:
        return "POST";
    case MO_API_PUT:
        return "PUT";
    case MO_API_PATCH:
        return "PATCH";
    case MO_API_DELETE:
        return "DELETE";
    }
    MO_UNREACHABLE();
}

static const char *method_lower(enum mo_api_method m) {
    switch (m) {
    case MO_API_GET:
        return "get";
    case MO_API_POST:
        return "post";
    case MO_API_PUT:
        return "put";
    case MO_API_PATCH:
        return "patch";
    case MO_API_DELETE:
        return "delete";
    }
    MO_UNREACHABLE();
}

static mo_core_string openapi_path_template(struct lowering *p, const char *path) {
    size_t len = strlen(path);
    char *buf = LOWER_NEW(p, char, len * 2 + 2);
    size_t n = 0, i = 0;
    while (i < len) {
        if (path[i] == ':' && (i == 0 || path[i - 1] == '/')) {
            buf[n++] = '{';
            i++;
            while (i < len && path[i] != '/')
                buf[n++] = path[i++];
            buf[n++] = '}';
        } else {
            buf[n++] = path[i++];
        }
    }
    return (mo_core_string){buf, n};
}

static mo_core_string fn_doc_summary(struct lowering *p, const struct mo_syntax *fn_decl) {
    size_t size = 1;
    for (const struct mo_syntax_comment *c = fn_decl->leading; c != NULL; c = c->next)
        if (c->documentation)
            size += c->span.end - c->span.start + 1;
    if (size == 1)
        return (mo_core_string){NULL, 0};
    char *buf = LOWER_NEW(p, char, size);
    size_t length = 0;
    bool first = true;
    for (const struct mo_syntax_comment *c = fn_decl->leading; c != NULL; c = c->next) {
        if (!c->documentation)
            continue;
        size_t end = c->span.end - c->span.start, start = 2;
        if (start < end && c->text[start] == ' ')
            start++;
        if (!first)
            buf[length++] = ' ';
        memcpy(buf + length, c->text + start, end - start);
        length += end - start;
        first = false;
    }
    return (mo_core_string){buf, length};
}

static mo_core_expr scalar_openapi_schema(struct lowering *p, const struct mo_source_type *t) {
    if (strcmp(t->name, "Int") == 0) {
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = api_cstr(p, "integer");
        return lib_call(p, "Json", "schema_type", a);
    }
    if (strcmp(t->name, "Byte") == 0)
        return lib_call(p, "Json", "schema_byte", api_args(p, 0));
    if (strcmp(t->name, "Float") == 0) {
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = api_cstr(p, "number");
        return lib_call(p, "Json", "schema_type", a);
    }
    if (strcmp(t->name, "Bool") == 0) {
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = api_cstr(p, "boolean");
        return lib_call(p, "Json", "schema_type", a);
    }
    if (strcmp(t->name, "Decimal") == 0 || strcmp(t->name, "BigInt") == 0) {
        mo_core_expr_list a = api_args(p, 2);
        a.items[0] = api_cstr(p, "string");
        a.items[1] = api_cstr(p, strcmp(t->name, "Decimal") == 0 ? "decimal" : "bigint");
        return lib_call(p, "Json", "schema_format", a);
    }
    mo_core_expr_list a = api_args(p, 1);
    a.items[0] = api_cstr(p, "string");
    return lib_call(p, "Json", "schema_type", a);
}

static mo_core_expr empty_text_list(struct lowering *p) {
    mo_core_ty text_ty = builtin_ty(p, "Text");
    return lower_prim(
        p,
        (struct mo_core_prim){
            .tag = MO_CORE_PRIM_LIST,
            .as.list = {.t = text_ty,
                        .op = {.tag = MO_CORE_LIST_OP_LIT, .as.lit.digits = lower_text(p, "0")}}},
        api_args(p, 0));
}

static void collect_status_codes(const struct mo_checked_api *api,
                                 const struct mo_source_type *err_ty, uint16_t *codes,
                                 uint32_t *count) {
    if (err_ty == NULL || err_ty->declaration == NULL ||
        err_ty->declaration->declaration->kind != MO_S_ENUM)
        return;
    const struct mo_symbol *en = err_ty->declaration;
    for (uint32_t i = 0; i < api->status_count; i++) {
        if (api->statuses[i].error_enum != en)
            continue;
        uint16_t sc = api->statuses[i].status_code;
        bool exists = false;
        for (uint32_t k = 0; k < *count; k++)
            if (codes[k] == sc)
                exists = true;
        if (!exists)
            codes[(*count)++] = sc;
    }
}

static mo_core_expr build_route_operation_json(struct lowering *p, const struct mo_checked_api *api,
                                               const struct mo_checked_api_route *route,
                                               mo_core_ty json_ty) {
    mo_core_string keys[8];
    mo_core_expr vals[8];
    size_t n = 0;

    const struct mo_symbol *fn_sym = p->program->functions[route->function].signature->function;
    keys[n] = lower_text(p, "operationId");
    vals[n++] = api_con1(p, json_ty, "String", api_cstr(p, fn_sym->name));

    mo_core_string summary = fn_doc_summary(p, fn_sym->declaration);
    if (summary.length != 0) {
        keys[n] = lower_text(p, "summary");
        vals[n++] = api_con1(p, json_ty, "String", api_text(p, summary));
    }

    if (route->auth_mode == MO_API_AUTH_REQUIRED) {
        mo_core_string sk = lower_text(p, "bearerAuth");
        mo_core_expr sv = json_arr(p, json_ty, 0, NULL);
        mo_core_expr sec_item = json_obj(p, json_ty, 1, &sk, &sv);
        keys[n] = lower_text(p, "security");
        vals[n++] = json_arr(p, json_ty, 1, &sec_item);
    }

    uint32_t oa_param_count = 0;
    const struct mo_checked_api_param *body_param = NULL;
    for (uint32_t i = 0; i < route->param_count; i++) {
        enum mo_api_param_kind k = route->params[i].kind;
        if (k == MO_API_PARAM_PATH || k == MO_API_PARAM_QUERY_REQUIRED ||
            k == MO_API_PARAM_QUERY_OPTIONAL)
            oa_param_count++;
        else if (k == MO_API_PARAM_BODY)
            body_param = &route->params[i];
    }

    if (oa_param_count != 0) {
        mo_core_expr *pitems = LOWER_NEW(p, mo_core_expr, oa_param_count);
        uint32_t pat = 0;
        for (uint32_t i = 0; i < route->param_count; i++) {
            const struct mo_checked_api_param *pm = &route->params[i];
            if (pm->kind != MO_API_PARAM_PATH && pm->kind != MO_API_PARAM_QUERY_REQUIRED &&
                pm->kind != MO_API_PARAM_QUERY_OPTIONAL)
                continue;
            bool in_path = pm->kind == MO_API_PARAM_PATH;
            bool reqd = pm->kind != MO_API_PARAM_QUERY_OPTIONAL;
            const struct mo_source_type *st =
                pm->kind == MO_API_PARAM_QUERY_OPTIONAL ? pm->type->arguments[0] : pm->type;
            mo_core_string pkeys[4] = {lower_text(p, "name"), lower_text(p, "in"),
                                       lower_text(p, "required"), lower_text(p, "schema")};
            mo_core_expr pvals[4] = {
                api_con1(p, json_ty, "String", api_cstr(p, pm->name)),
                api_con1(p, json_ty, "String", api_cstr(p, in_path ? "path" : "query")),
                api_con1(p, json_ty, "Boolean", lower_bool(p, reqd)), scalar_openapi_schema(p, st)};
            pitems[pat++] = json_obj(p, json_ty, 4, pkeys, pvals);
        }
        keys[n] = lower_text(p, "parameters");
        vals[n++] = json_arr(p, json_ty, oa_param_count, pitems);
    }

    if (body_param != NULL) {
        mo_core_string sk = lower_text(p, "schema");
        mo_core_expr sv =
            lower_json_schema(p, body_param->json, NULL, api->module, 0, empty_text_list(p));
        mo_core_string ctk = lower_text(p, "application/json");
        mo_core_expr ctv = json_obj(p, json_ty, 1, &sk, &sv);
        mo_core_string bkeys[2] = {lower_text(p, "required"), lower_text(p, "content")};
        mo_core_expr bvals[2] = {api_con1(p, json_ty, "Boolean", lower_bool(p, true)),
                                 json_obj(p, json_ty, 1, &ctk, &ctv)};
        keys[n] = lower_text(p, "requestBody");
        vals[n++] = json_obj(p, json_ty, 2, bkeys, bvals);
    }

    uint16_t err_codes[MO_LIMIT_API_STATUSES + 4];
    uint32_t err_code_count = 0;
    if (route->auth_mode == MO_API_AUTH_REQUIRED && api->has_auth)
        collect_status_codes(api, api->auth.error_type, err_codes, &err_code_count);
    collect_status_codes(api, route->error_type, err_codes, &err_code_count);

    bool has_422 = oa_param_count != 0 || body_param != NULL;
    for (uint32_t c = 0; c < err_code_count; c++)
        if (err_codes[c] == 422)
            has_422 = false;

    size_t resp_cap = 2 + err_code_count;
    mo_core_string *rkeys = LOWER_NEW(p, mo_core_string, resp_cap);
    mo_core_expr *rvals = LOWER_NEW(p, mo_core_expr, resp_cap);
    size_t rn = 0;

    bool is_204 = route->reply == MO_API_REPLY_UNIT || route->reply == MO_API_REPLY_RESULT_UNIT;
    rkeys[rn] = lower_text(p, is_204 ? "204" : "200");
    if (route->ok_json != NULL) {
        mo_core_string sk = lower_text(p, "schema");
        mo_core_expr sv =
            lower_json_schema(p, route->ok_json, NULL, api->module, 0, empty_text_list(p));
        mo_core_string ctk = lower_text(p, "application/json");
        mo_core_expr ctv = json_obj(p, json_ty, 1, &sk, &sv);
        mo_core_string ok_keys[2] = {lower_text(p, "description"), lower_text(p, "content")};
        mo_core_expr ok_vals[2] = {api_con1(p, json_ty, "String", api_cstr(p, "OK")),
                                   json_obj(p, json_ty, 1, &ctk, &ctv)};
        rvals[rn++] = json_obj(p, json_ty, 2, ok_keys, ok_vals);
    } else {
        mo_core_string dk = lower_text(p, "description");
        mo_core_expr dv = api_con1(p, json_ty, "String", api_cstr(p, is_204 ? "No Content" : "OK"));
        rvals[rn++] = json_obj(p, json_ty, 1, &dk, &dv);
    }

    if (has_422) {
        mo_core_string dk = lower_text(p, "description");
        mo_core_expr dv = api_con1(p, json_ty, "String", api_cstr(p, "Validation Error"));
        rkeys[rn] = lower_text(p, "422");
        rvals[rn++] = json_obj(p, json_ty, 1, &dk, &dv);
    }

    for (uint32_t c = 0; c < err_code_count; c++) {
        uint16_t sc = err_codes[c];
        if ((is_204 && sc == 204) || (!is_204 && sc == 200))
            continue;
        mo_core_string dk = lower_text(p, "description");
        mo_core_expr dv = api_con1(p, json_ty, "String", api_cstr(p, "Application Error"));
        rkeys[rn] = lower_number(p, sc);
        rvals[rn++] = json_obj(p, json_ty, 1, &dk, &dv);
    }

    keys[n] = lower_text(p, "responses");
    vals[n++] = json_obj(p, json_ty, rn, rkeys, rvals);

    return json_obj(p, json_ty, n, keys, vals);
}

static struct instance *new_generated_fn(struct lowering *p, uint32_t module, const char *suffix,
                                         size_t param_count, const struct mo_core_param *params,
                                         mo_core_ty ret_ty) {
    struct instance *f = LOWER_NEW(p, struct instance, 1);
    f->owner = module;
    f->index = p->function_count++;
    *p->function_tail = f;
    p->function_tail = &f->next;
    f->const_prefix = p->program->constant_count;
    f->fun.module = lower_text(p, p->program->set->names[module]);
    f->fun.name = lower_concat(p, f->fun.module, suffix, lower_text(p, ""));
    f->fun.const_count.digits = lower_number(p, p->program->constant_count);
    f->fun.params.count = param_count;
    f->fun.params.items = LOWER_NEW(p, struct mo_core_param, param_count);
    f->meta.body.params.count = param_count;
    f->meta.body.params.items = LOWER_NEW(p, struct mo_core_name, param_count);
    for (size_t i = 0; i < param_count; i++)
        f->fun.params.items[i] = params[i];
    f->fun.ret = ret_ty;
    f->meta.name = f->fun.name;
    f->meta.span = lower_span(p, p->span);
    return f;
}

static struct instance *emit_openapi_fn(struct lowering *p, const struct mo_checked_api *api) {
    mo_core_ty text_ty = builtin_ty(p, "Text");
    mo_core_ty json_ty = lower_type(p, lib_data_source(p, "Json", "Json"), NULL);
    struct instance *f = new_generated_fn(p, api->module, ".%api.openapi", 0, NULL, text_ty);

    size_t rcap = api->route_count == 0 ? 1 : api->route_count;
    mo_core_string *oa_paths = LOWER_NEW(p, mo_core_string, rcap);
    for (uint32_t i = 0; i < api->route_count; i++)
        oa_paths[i] = openapi_path_template(p, api->routes[i].path);

    bool *done = LOWER_NEW(p, bool, rcap);
    mo_core_string *path_keys = LOWER_NEW(p, mo_core_string, rcap);
    mo_core_expr *path_vals = LOWER_NEW(p, mo_core_expr, rcap);
    size_t unique_paths = 0;

    for (uint32_t i = 0; i < api->route_count; i++) {
        if (done[i])
            continue;
        mo_core_string mkeys[5];
        mo_core_expr mvals[5];
        size_t mn = 0;
        for (uint32_t j = i; j < api->route_count; j++) {
            if (!lower_same(oa_paths[i], oa_paths[j]))
                continue;
            done[j] = true;
            mkeys[mn] = lower_text(p, method_lower(api->routes[j].method));
            mvals[mn++] = build_route_operation_json(p, api, &api->routes[j], json_ty);
        }
        path_keys[unique_paths] = oa_paths[i];
        path_vals[unique_paths++] = json_obj(p, json_ty, mn, mkeys, mvals);
    }

    mo_core_string info_keys[2] = {lower_text(p, "title"), lower_text(p, "version")};
    mo_core_expr info_vals[2] = {
        api_con1(p, json_ty, "String", api_cstr(p, p->program->set->names[api->module])),
        api_con1(p, json_ty, "String", api_cstr(p, "0.1.0"))};

    mo_core_string top_keys[4];
    mo_core_expr top_vals[4];
    size_t tn = 0;
    top_keys[tn] = lower_text(p, "openapi");
    top_vals[tn++] = api_con1(p, json_ty, "String", api_cstr(p, "3.1.0"));
    top_keys[tn] = lower_text(p, "info");
    top_vals[tn++] = json_obj(p, json_ty, 2, info_keys, info_vals);
    top_keys[tn] = lower_text(p, "paths");
    top_vals[tn++] = json_obj(p, json_ty, unique_paths, path_keys, path_vals);

    if (api->has_auth) {
        mo_core_string bkeys[2] = {lower_text(p, "type"), lower_text(p, "scheme")};
        mo_core_expr bvals[2] = {api_con1(p, json_ty, "String", api_cstr(p, "http")),
                                 api_con1(p, json_ty, "String", api_cstr(p, "bearer"))};
        mo_core_string sk = lower_text(p, "bearerAuth");
        mo_core_expr sv = json_obj(p, json_ty, 2, bkeys, bvals);
        mo_core_string ck = lower_text(p, "securitySchemes");
        mo_core_expr cv = json_obj(p, json_ty, 1, &sk, &sv);
        top_keys[tn] = lower_text(p, "components");
        top_vals[tn++] = json_obj(p, json_ty, 1, &ck, &cv);
    }

    mo_core_expr doc = json_obj(p, json_ty, tn, top_keys, top_vals);
    f->fun.body = api_lib_call1(p, "render_json", doc);
    return f;
}

struct dispatch_env {
    const struct mo_checked_api *api;
    const struct instance *openapi_fn;
    mo_core_ty req_ty;
    mo_core_ty res_ty;
    mo_core_ty text_ty;
    mo_core_ty bytes_ty;
    mo_core_string req_var;
    mo_core_string req_method;
    mo_core_string req_path;
    mo_core_string req_body;
    mo_core_string *cap_vars;
    mo_core_string *actor_vars;
};

static mo_core_expr encode_domain_error(struct lowering *p, const struct dispatch_env *env,
                                        const struct mo_source_type *err_src,
                                        const struct mo_ability_implementation *err_json,
                                        mo_core_expr err_expr) {
    mo_core_ty err_ty = lower_type(p, err_src, NULL);
    if (err_ty == env->res_ty)
        return err_expr;
    MO_ASSERT(err_json != NULL && err_src->declaration != NULL &&
              err_src->declaration->declaration->kind == MO_S_ENUM);
    const struct mo_symbol *en = err_src->declaration;
    mo_core_string ev = lower_fresh(p);
    mo_core_ctors ctors = lower_ctors(p, err_ty);
    mo_core_expr match = lower_expr(p, MO_CORE_EXPR_CASE);
    match->as.case_.expr = lower_var(p, ev);
    match->as.case_.arms.count = ctors.count;
    match->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, ctors.count);
    for (size_t i = 0; i < ctors.count; i++) {
        struct mo_core_ctor *ctor = &ctors.items[i];
        struct mo_core_arm *arm = &match->as.case_.arms.items[i];
        arm->ctor = ctor->name;
        arm->names.count = ctor->fields.count;
        arm->names.items = LOWER_NEW(p, mo_core_string, ctor->fields.count);
        for (size_t k = 0; k < ctor->fields.count; k++)
            arm->names.items[k] = lower_fresh(p);
        uint16_t status_code = 500;
        for (uint32_t s = 0; s < env->api->status_count; s++) {
            const struct mo_checked_api_status *st = &env->api->statuses[s];
            if (st->error_enum == en && st->variant->name_length == ctor->name.length &&
                memcmp(st->variant->name, ctor->name.bytes, ctor->name.length) == 0) {
                status_code = st->status_code;
                break;
            }
        }
        arm->body = lower_int(p, status_code);
    }
    mo_core_expr json_val = lower_to_json(p, err_json, NULL, env->api->module, 0, lower_var(p, ev));
    return api_let(p, ev, err_expr, api_lib_call2(p, "domain_error_response", match, json_val));
}

static const char *scalar_suffix(const struct mo_source_type *t) {
    if (strcmp(t->name, "Int") == 0)
        return "int";
    if (strcmp(t->name, "Byte") == 0)
        return "byte";
    if (strcmp(t->name, "BigInt") == 0)
        return "bigint";
    if (strcmp(t->name, "Decimal") == 0)
        return "decimal";
    if (strcmp(t->name, "Float") == 0)
        return "float";
    if (strcmp(t->name, "Bool") == 0)
        return "bool";
    return "text";
}

static mo_core_expr unwrap_or_return_response(struct lowering *p, mo_core_expr result_expr,
                                              mo_core_string ok_var, mo_core_expr cont) {
    mo_core_expr c = lower_expr(p, MO_CORE_EXPR_CASE);
    c->as.case_.expr = result_expr;
    c->as.case_.arms.count = 2;
    c->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    c->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
    c->as.case_.arms.items[0].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    c->as.case_.arms.items[0].names.items[0] = ok_var;
    c->as.case_.arms.items[0].body = cont;
    mo_core_string err_res = lower_fresh(p);
    c->as.case_.arms.items[1].ctor = lower_text(p, "Error");
    c->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    c->as.case_.arms.items[1].names.items[0] = err_res;
    c->as.case_.arms.items[1].body = lower_var(p, err_res);
    return c;
}

static mo_core_expr invoke_route_handler(struct lowering *p, const struct dispatch_env *env,
                                         const struct mo_checked_api_route *route,
                                         const mo_core_string *arg_vars) {
    const struct mo_symbol *fn_sym = p->program->functions[route->function].signature->function;
    struct instance *fn_inst =
        lower_instance(p, fn_sym, (struct mo_type_arguments){0}, NULL, 0, NULL, fn_sym->module, 0);
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = fn_inst->fun.name;
    call->as.call.args = api_args(p, route->param_count);
    for (uint32_t i = 0; i < route->param_count; i++)
        call->as.call.args.items[i] = lower_var(p, arg_vars[i]);

    switch (route->reply) {
    case MO_API_REPLY_RESPONSE:
        return call;
    case MO_API_REPLY_UNIT:
        return api_let(p, lower_fresh(p), call, api_lib_call0(p, "no_content"));
    case MO_API_REPLY_JSON: {
        mo_core_string v = lower_fresh(p);
        mo_core_expr j =
            lower_to_json(p, route->ok_json, NULL, env->api->module, 0, lower_var(p, v));
        return api_let(p, v, call, api_lib_call2(p, "encoded_json_response", lower_int(p, 200), j));
    }
    case MO_API_REPLY_RESULT_RESPONSE:
    case MO_API_REPLY_RESULT_UNIT:
    case MO_API_REPLY_RESULT_JSON: {
        mo_core_expr c = lower_expr(p, MO_CORE_EXPR_CASE);
        c->as.case_.expr = call;
        c->as.case_.arms.count = 2;
        c->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string ok_v = lower_fresh(p), err_v = lower_fresh(p);
        c->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        c->as.case_.arms.items[0].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        c->as.case_.arms.items[0].names.items[0] = ok_v;
        if (route->reply == MO_API_REPLY_RESULT_RESPONSE) {
            c->as.case_.arms.items[0].body = lower_var(p, ok_v);
        } else if (route->reply == MO_API_REPLY_RESULT_UNIT) {
            c->as.case_.arms.items[0].body = api_lib_call0(p, "no_content");
        } else {
            mo_core_expr j =
                lower_to_json(p, route->ok_json, NULL, env->api->module, 0, lower_var(p, ok_v));
            c->as.case_.arms.items[0].body =
                api_lib_call2(p, "encoded_json_response", lower_int(p, 200), j);
        }
        c->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        c->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        c->as.case_.arms.items[1].names.items[0] = err_v;
        c->as.case_.arms.items[1].body =
            encode_domain_error(p, env, route->error_type, route->error_json, lower_var(p, err_v));
        return c;
    }
    }
    MO_UNREACHABLE();
}

static mo_core_expr bind_route_params(struct lowering *p, const struct dispatch_env *env,
                                      const struct mo_checked_api_route *route,
                                      mo_core_string captured_var, mo_core_string auth_var,
                                      const mo_core_string *db_vars, uint32_t idx,
                                      mo_core_string *arg_vars) {
    if (idx == route->param_count)
        return invoke_route_handler(p, env, route, arg_vars);

    const struct mo_checked_api_param *pm = &route->params[idx];
    switch (pm->kind) {
    case MO_API_PARAM_AUTH:
    case MO_API_PARAM_AUTH_OPTIONAL:
        arg_vars[idx] = auth_var;
        return bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
    case MO_API_PARAM_REQUEST:
        arg_vars[idx] = env->req_var;
        return bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
    case MO_API_PARAM_SQLITE_DB:
        arg_vars[idx] = db_vars[pm->index];
        return bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
    case MO_API_PARAM_ACTOR:
        arg_vars[idx] = env->actor_vars[pm->index];
        return bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
    case MO_API_PARAM_CAPABILITY:
        arg_vars[idx] = env->cap_vars[pm->index];
        return bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
    case MO_API_PARAM_PATH: {
        mo_core_expr_list get_args = api_args(p, 2);
        get_args.items[0] = lower_var(p, captured_var);
        get_args.items[1] = lower_int(p, pm->index);
        mo_core_expr opt_seg = lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                  .as.list = {.t = env->text_ty, .op.tag = MO_CORE_LIST_OP_GET}},
            get_args);
        mo_core_expr seg_case = lower_expr(p, MO_CORE_EXPR_CASE);
        seg_case->as.case_.expr = opt_seg;
        seg_case->as.case_.arms.count = 2;
        seg_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_expr stop = lower_expr(p, MO_CORE_EXPR_STOP);
        stop->as.stop = lower_text(p, "unreachable matched path segment");
        seg_case->as.case_.arms.items[0] =
            (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = stop};
        mo_core_string raw_seg = lower_fresh(p);
        seg_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
        seg_case->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        seg_case->as.case_.arms.items[1].names.items[0] = raw_seg;

        char helper[48];
        (void)snprintf(helper, sizeof helper, "parse_path_%s", scalar_suffix(pm->type));
        mo_core_expr parsed =
            api_lib_call2(p, helper, lower_var(p, raw_seg), api_cstr(p, pm->name));
        mo_core_string val_var = lower_fresh(p);
        arg_vars[idx] = val_var;
        mo_core_expr next =
            bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
        seg_case->as.case_.arms.items[1].body = unwrap_or_return_response(p, parsed, val_var, next);
        return seg_case;
    }
    case MO_API_PARAM_QUERY_REQUIRED: {
        char helper[48];
        (void)snprintf(helper, sizeof helper, "require_query_%s", scalar_suffix(pm->type));
        mo_core_expr parsed =
            api_lib_call2(p, helper, lower_var(p, env->req_var), api_cstr(p, pm->name));
        mo_core_string val_var = lower_fresh(p);
        arg_vars[idx] = val_var;
        mo_core_expr next =
            bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
        return unwrap_or_return_response(p, parsed, val_var, next);
    }
    case MO_API_PARAM_QUERY_OPTIONAL: {
        char helper[48];
        (void)snprintf(helper, sizeof helper, "optional_query_%s",
                       scalar_suffix(pm->type->arguments[0]));
        mo_core_expr parsed =
            api_lib_call2(p, helper, lower_var(p, env->req_var), api_cstr(p, pm->name));
        mo_core_string val_var = lower_fresh(p);
        arg_vars[idx] = val_var;
        mo_core_expr next =
            bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
        return unwrap_or_return_response(p, parsed, val_var, next);
    }
    case MO_API_PARAM_BODY: {
        mo_core_expr decoded =
            lower_json_decode_call(p, pm->json, NULL, env->api->module, 0, env->bytes_ty,
                                   lower_var(p, env->req_body), true);
        mo_core_string val_var = lower_fresh(p), err_var = lower_fresh(p);
        arg_vars[idx] = val_var;
        mo_core_expr next =
            bind_route_params(p, env, route, captured_var, auth_var, db_vars, idx + 1, arg_vars);
        mo_core_expr c = lower_expr(p, MO_CORE_EXPR_CASE);
        c->as.case_.expr = decoded;
        c->as.case_.arms.count = 2;
        c->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        c->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        c->as.case_.arms.items[0].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        c->as.case_.arms.items[0].names.items[0] = val_var;
        c->as.case_.arms.items[0].body = next;
        c->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        c->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        c->as.case_.arms.items[1].names.items[0] = err_var;
        c->as.case_.arms.items[1].body =
            api_lib_call1(p, "body_decode_error", lower_var(p, err_var));
        return c;
    }
    }
    MO_UNREACHABLE();
}

static mo_core_expr execute_matched_route(struct lowering *p, const struct dispatch_env *env,
                                          const struct mo_checked_api_route *route,
                                          mo_core_string captured_var) {
    bool need_db[MO_LIMIT_API_SQLITES] = {false};
    if (route->auth_mode != MO_API_AUTH_NONE && env->api->has_auth) {
        for (uint32_t i = 0; i < env->api->auth.param_count; i++)
            if (env->api->auth.params[i].kind == MO_API_PARAM_SQLITE_DB)
                need_db[env->api->auth.params[i].index] = true;
    }
    for (uint32_t i = 0; i < route->param_count; i++)
        if (route->params[i].kind == MO_API_PARAM_SQLITE_DB)
            need_db[route->params[i].index] = true;

    mo_core_string db_vars[MO_LIMIT_API_SQLITES] = {{0}};
    for (uint32_t s = 0; s < env->api->sqlite_count; s++)
        if (need_db[s])
            db_vars[s] = lower_fresh(p);

    mo_core_string auth_var = {0};
    mo_core_string *arg_vars =
        LOWER_NEW(p, mo_core_string, route->param_count == 0 ? 1 : route->param_count);
    if (route->auth_mode != MO_API_AUTH_NONE)
        auth_var = lower_fresh(p);

    mo_core_expr inner =
        bind_route_params(p, env, route, captured_var, auth_var, db_vars, 0, arg_vars);

    if (route->auth_mode != MO_API_AUTH_NONE && env->api->has_auth) {
        const struct mo_checked_api_auth *auth = &env->api->auth;
        const struct mo_symbol *afn_sym = p->program->functions[auth->function].signature->function;
        struct instance *afn_inst = lower_instance(p, afn_sym, (struct mo_type_arguments){0}, NULL,
                                                   0, NULL, afn_sym->module, 0);
        mo_core_expr acall = lower_expr(p, MO_CORE_EXPR_CALL);
        acall->as.call.name = afn_inst->fun.name;
        acall->as.call.args = api_args(p, auth->param_count);
        for (uint32_t i = 0; i < auth->param_count; i++) {
            const struct mo_checked_api_param *pm = &auth->params[i];
            if (pm->kind == MO_API_PARAM_REQUEST)
                acall->as.call.args.items[i] = lower_var(p, env->req_var);
            else if (pm->kind == MO_API_PARAM_SQLITE_DB)
                acall->as.call.args.items[i] = lower_var(p, db_vars[pm->index]);
            else if (pm->kind == MO_API_PARAM_ACTOR)
                acall->as.call.args.items[i] = lower_var(p, env->actor_vars[pm->index]);
            else
                acall->as.call.args.items[i] = lower_var(p, env->cap_vars[pm->index]);
        }
        mo_core_expr acase = lower_expr(p, MO_CORE_EXPR_CASE);
        acase->as.case_.expr = acall;
        acase->as.case_.arms.count = 2;
        acase->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        if (route->auth_mode == MO_API_AUTH_REQUIRED) {
            mo_core_string aerr = lower_fresh(p);
            acase->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
            acase->as.case_.arms.items[0].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            acase->as.case_.arms.items[0].names.items[0] = auth_var;
            acase->as.case_.arms.items[0].body = inner;
            acase->as.case_.arms.items[1].ctor = lower_text(p, "Error");
            acase->as.case_.arms.items[1].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            acase->as.case_.arms.items[1].names.items[0] = aerr;
            acase->as.case_.arms.items[1].body =
                encode_domain_error(p, env, auth->error_type, auth->error_json, lower_var(p, aerr));
        } else {
            const struct mo_source_type **prin_arg = LOWER_NEW(p, const struct mo_source_type *, 1);
            *prin_arg = auth->principal;
            struct mo_source_type *opt_prin_src = LOWER_NEW(p, struct mo_source_type, 1);
            *opt_prin_src = (struct mo_source_type){.kind = MO_SOURCE_NAMED,
                                                    .name = "Option",
                                                    .module = UINT32_MAX,
                                                    .count = 1,
                                                    .arguments = prin_arg};
            mo_core_ty opt_prin_ty = lower_type(p, opt_prin_src, NULL);
            mo_core_string ok_m = lower_fresh(p), aerr = lower_fresh(p);
            mo_core_expr inner_err =
                bind_route_params(p, env, route, captured_var, auth_var, db_vars, 0, arg_vars);
            acase->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
            acase->as.case_.arms.items[0].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            acase->as.case_.arms.items[0].names.items[0] = ok_m;
            acase->as.case_.arms.items[0].body =
                api_let(p, auth_var, api_con1(p, opt_prin_ty, "Some", lower_var(p, ok_m)), inner);
            acase->as.case_.arms.items[1].ctor = lower_text(p, "Error");
            acase->as.case_.arms.items[1].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            acase->as.case_.arms.items[1].names.items[0] = aerr;
            acase->as.case_.arms.items[1].body =
                api_let(p, auth_var, api_con(p, opt_prin_ty, "None", api_args(p, 0)), inner_err);
        }
        inner = acase;
    }

    for (uint32_t s = env->api->sqlite_count; s != 0; s--) {
        uint32_t slot = s - 1;
        if (!need_db[slot])
            continue;
        MO_ASSERT(env->api->capability_count > 0 &&
                  strcmp(env->api->capabilities[0], "Sqlite") == 0);
        mo_core_expr_list open_args = api_args(p, 2);
        open_args.items[0] = lower_var(p, env->cap_vars[0]);
        open_args.items[1] = api_cstr(p, env->api->sqlites[slot].path);
        mo_core_expr opened =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                             .as.io = {MO_CORE_IO_OP_SQLITE_OPEN, false}},
                       open_args);
        mo_core_expr dcase = lower_expr(p, MO_CORE_EXPR_CASE);
        dcase->as.case_.expr = opened;
        dcase->as.case_.arms.count = 2;
        dcase->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string derr = lower_fresh(p);
        dcase->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        dcase->as.case_.arms.items[0].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        dcase->as.case_.arms.items[0].names.items[0] = db_vars[slot];
        dcase->as.case_.arms.items[0].body = inner;
        dcase->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        dcase->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        dcase->as.case_.arms.items[1].names.items[0] = derr;
        dcase->as.case_.arms.items[1].body =
            api_lib_call1(p, "sqlite_open_error_response", lower_var(p, derr));
        inner = dcase;
    }

    return inner;
}

static mo_core_expr text_eq(struct lowering *p, mo_core_ty text_ty, mo_core_expr a,
                            mo_core_expr b) {
    mo_core_expr_list eq_args = api_args(p, 2);
    eq_args.items[0] = a;
    eq_args.items[1] = b;
    return lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = text_ty}, eq_args);
}

static mo_core_expr builtin_openapi_fallback(struct lowering *p, const struct dispatch_env *env,
                                             mo_core_string path_matched_var) {
    mo_core_expr not_found_or_405 = lower_expr(p, MO_CORE_EXPR_IF);
    not_found_or_405->as.if_.cond = lower_var(p, path_matched_var);
    not_found_or_405->as.if_.then_ = api_lib_call0(p, "method_not_allowed_response");
    not_found_or_405->as.if_.else_ = api_lib_call0(p, "not_found_response");

    /* Check GET /docs */
    mo_core_expr docs_if = lower_expr(p, MO_CORE_EXPR_IF);
    docs_if->as.if_.cond =
        text_eq(p, env->text_ty, lower_var(p, env->req_method), api_cstr(p, "GET"));
    docs_if->as.if_.then_ = api_lib_call1(p, "openapi_docs_html",
                                          api_cstr(p, p->program->set->names[env->api->module]));
    docs_if->as.if_.else_ = api_lib_call0(p, "method_not_allowed_response");

    mo_core_expr docs_case = lower_expr(p, MO_CORE_EXPR_CASE);
    docs_case->as.case_.expr =
        api_lib_call2(p, "match_path", api_cstr(p, "/docs"), lower_var(p, env->req_path));
    docs_case->as.case_.arms.count = 2;
    docs_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    docs_case->as.case_.arms.items[0] =
        (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = not_found_or_405};
    docs_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
    docs_case->as.case_.arms.items[1].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    docs_case->as.case_.arms.items[1].names.items[0] = lower_fresh(p);
    docs_case->as.case_.arms.items[1].body = docs_if;

    /* Check GET /openapi.json */
    mo_core_expr call_oa = lower_expr(p, MO_CORE_EXPR_CALL);
    call_oa->as.call.name = env->openapi_fn->fun.name;
    call_oa->as.call.args = api_args(p, 0);
    mo_core_expr_list bytes_args = api_args(p, 1);
    bytes_args.items[0] = call_oa;
    mo_core_expr oa_bytes = lower_prim(
        p, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_BYTES},
        bytes_args);
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type *const pair_args[2] = {&text_src, &text_src};
    static const struct mo_source_type pair_src = {.kind = MO_SOURCE_TUPLE,
                                                   .name = "()",
                                                   .module = UINT32_MAX,
                                                   .count = 2,
                                                   .arguments = pair_args};
    mo_core_ty hdr_pair_ty = lower_type(p, &pair_src, NULL);
    mo_core_expr hdr_tuple = lower_expr(p, MO_CORE_EXPR_TUPLE);
    hdr_tuple->as.tuple = api_args(p, 2);
    hdr_tuple->as.tuple.items[0] = api_cstr(p, "Content-Type");
    hdr_tuple->as.tuple.items[1] = api_cstr(p, "application/json");
    mo_core_expr_list hdr_list_args = api_args(p, 1);
    hdr_list_args.items[0] = hdr_tuple;
    mo_core_expr hdrs =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                         .as.list = {.t = hdr_pair_ty,
                                                     .op = {.tag = MO_CORE_LIST_OP_LIT,
                                                            .as.lit.digits = lower_text(p, "1")}}},
                   hdr_list_args);
    mo_core_expr_list res_args = api_args(p, 3);
    res_args.items[0] = lower_int(p, 200);
    res_args.items[1] = hdrs;
    res_args.items[2] = oa_bytes;
    mo_core_expr oa_res = lib_call(p, "Api", "response", res_args);

    mo_core_expr oa_if = lower_expr(p, MO_CORE_EXPR_IF);
    oa_if->as.if_.cond =
        text_eq(p, env->text_ty, lower_var(p, env->req_method), api_cstr(p, "GET"));
    oa_if->as.if_.then_ = oa_res;
    oa_if->as.if_.else_ = api_lib_call0(p, "method_not_allowed_response");

    mo_core_expr oa_case = lower_expr(p, MO_CORE_EXPR_CASE);
    oa_case->as.case_.expr =
        api_lib_call2(p, "match_path", api_cstr(p, "/openapi.json"), lower_var(p, env->req_path));
    oa_case->as.case_.arms.count = 2;
    oa_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    oa_case->as.case_.arms.items[0] =
        (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = docs_case};
    oa_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
    oa_case->as.case_.arms.items[1].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    oa_case->as.case_.arms.items[1].names.items[0] = lower_fresh(p);
    oa_case->as.case_.arms.items[1].body = oa_if;

    return oa_case;
}

static struct instance *
emit_single_route_fn(struct lowering *p, const struct dispatch_env *base_env, uint32_t route_idx) {
    const struct mo_checked_api *api = base_env->api;
    const struct mo_checked_api_route *route = &api->routes[route_idx];
    p->span = route->syntax->span;

    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type *const t_args[1] = {&text_src};
    static const struct mo_source_type list_text_src = {.kind = MO_SOURCE_NAMED,
                                                        .name = "List",
                                                        .module = UINT32_MAX,
                                                        .count = 1,
                                                        .arguments = t_args};
    mo_core_ty list_text_ty = lower_type(p, &list_text_src, NULL);

    size_t pcount = 3 + api->capability_count + api->actor_count;
    struct mo_core_param *params = LOWER_NEW(p, struct mo_core_param, pcount);
    mo_core_string *rcaps =
        LOWER_NEW(p, mo_core_string, api->capability_count == 0 ? 1 : api->capability_count);
    mo_core_string *ractors =
        LOWER_NEW(p, mo_core_string, api->actor_count == 0 ? 1 : api->actor_count);
    params[0] = (struct mo_core_param){lower_fresh(p), base_env->req_ty};
    params[1] = (struct mo_core_param){lower_fresh(p), base_env->bytes_ty};
    params[2] = (struct mo_core_param){lower_fresh(p), list_text_ty};
    for (uint32_t c = 0; c < api->capability_count; c++) {
        rcaps[c] = lower_fresh(p);
        params[3 + c] = (struct mo_core_param){rcaps[c], builtin_ty(p, api->capabilities[c])};
    }
    mo_core_ty sim_ty = builtin_ty(p, "Simulation");
    for (uint32_t a = 0; a < api->actor_count; a++) {
        ractors[a] = lower_fresh(p);
        params[3 + api->capability_count + a] = (struct mo_core_param){ractors[a], sim_ty};
    }

    char suffix[48];
    (void)snprintf(suffix, sizeof suffix, ".%%api.route.%u", (unsigned)route_idx);
    struct instance *fn =
        new_generated_fn(p, api->module, suffix, pcount, params, base_env->res_ty);

    struct dispatch_env renv = *base_env;
    renv.req_var = params[0].name;
    renv.req_body = params[1].name;
    renv.cap_vars = rcaps;
    renv.actor_vars = ractors;
    fn->fun.body = execute_matched_route(p, &renv, route, params[2].name);
    return fn;
}

static mo_core_expr make_jump1(struct lowering *p, mo_core_string label, mo_core_expr arg) {
    mo_core_expr j = lower_expr(p, MO_CORE_EXPR_JUMP);
    j->as.jump.name = label;
    j->as.jump.args = api_args(p, 1);
    j->as.jump.args.items[0] = arg;
    return j;
}

static mo_core_expr build_route_chain(struct lowering *p, const struct dispatch_env *env,
                                      struct instance *const *route_fns, uint32_t route_idx,
                                      mo_core_string path_matched_var) {
    if (route_idx == env->api->route_count)
        return builtin_openapi_fallback(p, env, path_matched_var);

    const struct mo_checked_api_route *route = &env->api->routes[route_idx];
    p->span = route->syntax->span;

    mo_core_ty bool_ty = builtin_ty(p, "Bool");
    mo_core_string join_label = lower_fresh(p);
    mo_core_string joined_matched = lower_fresh(p);

    mo_core_expr join = lower_expr(p, MO_CORE_EXPR_JOIN);
    join->as.join.name = join_label;
    join->as.join.params.count = 1;
    join->as.join.params.items = LOWER_NEW(p, struct mo_core_param, 1);
    join->as.join.params.items[0] = (struct mo_core_param){joined_matched, bool_ty};
    join->as.join.body = build_route_chain(p, env, route_fns, route_idx + 1, joined_matched);

    mo_core_expr match_call =
        api_lib_call2(p, "match_path", api_cstr(p, route->path), lower_var(p, env->req_path));
    mo_core_string captured = lower_fresh(p);

    size_t call_argc = 3 + env->api->capability_count + env->api->actor_count;
    mo_core_expr rcall = lower_expr(p, MO_CORE_EXPR_CALL);
    rcall->as.call.name = route_fns[route_idx]->fun.name;
    rcall->as.call.args = api_args(p, call_argc);
    rcall->as.call.args.items[0] = lower_var(p, env->req_var);
    rcall->as.call.args.items[1] = lower_var(p, env->req_body);
    rcall->as.call.args.items[2] = lower_var(p, captured);
    for (uint32_t c = 0; c < env->api->capability_count; c++)
        rcall->as.call.args.items[3 + c] = lower_var(p, env->cap_vars[c]);
    for (uint32_t a = 0; a < env->api->actor_count; a++)
        rcall->as.call.args.items[3 + env->api->capability_count + a] =
            lower_var(p, env->actor_vars[a]);

    mo_core_expr method_if = lower_expr(p, MO_CORE_EXPR_IF);
    method_if->as.if_.cond = text_eq(p, env->text_ty, lower_var(p, env->req_method),
                                     api_cstr(p, method_upper(route->method)));
    method_if->as.if_.then_ = rcall;
    method_if->as.if_.else_ = make_jump1(p, join_label, lower_bool(p, true));

    mo_core_expr mcase = lower_expr(p, MO_CORE_EXPR_CASE);
    mcase->as.case_.expr = match_call;
    mcase->as.case_.arms.count = 2;
    mcase->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    mcase->as.case_.arms.items[0] =
        (struct mo_core_arm){.ctor = lower_text(p, "None"),
                             .body = make_jump1(p, join_label, lower_var(p, path_matched_var))};
    mcase->as.case_.arms.items[1].ctor = lower_text(p, "Some");
    mcase->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    mcase->as.case_.arms.items[1].names.items[0] = captured;
    mcase->as.case_.arms.items[1].body = method_if;

    join->as.join.rest = mcase;
    return join;
}

static void emit_wire_fn(struct lowering *p, const struct mo_checked_api *api,
                         const struct instance *disp_fn);

static void emit_dispatch_fns(struct lowering *p, const struct mo_checked_api *api,
                              const struct instance *openapi_fn) {
    mo_core_ty req_ty = lower_type(p, lib_data_source(p, "Api", "Request"), NULL);
    mo_core_ty res_ty = lower_type(p, lib_data_source(p, "Api", "Response"), NULL);
    mo_core_ty text_ty = builtin_ty(p, "Text");
    mo_core_ty bytes_ty = builtin_ty(p, "Bytes");

    size_t param_count = 1 + api->capability_count + api->actor_count;
    struct mo_core_param *route_params = LOWER_NEW(p, struct mo_core_param, param_count);
    mo_core_string *cap_vars =
        LOWER_NEW(p, mo_core_string, api->capability_count == 0 ? 1 : api->capability_count);
    mo_core_string *actor_vars =
        LOWER_NEW(p, mo_core_string, api->actor_count == 0 ? 1 : api->actor_count);
    route_params[0] = (struct mo_core_param){lower_fresh(p), req_ty};
    for (uint32_t c = 0; c < api->capability_count; c++) {
        cap_vars[c] = lower_fresh(p);
        route_params[1 + c] =
            (struct mo_core_param){cap_vars[c], builtin_ty(p, api->capabilities[c])};
    }
    mo_core_ty sim_ty = builtin_ty(p, "Simulation");
    for (uint32_t a = 0; a < api->actor_count; a++) {
        actor_vars[a] = lower_fresh(p);
        route_params[1 + api->capability_count + a] = (struct mo_core_param){actor_vars[a], sim_ty};
    }

    struct instance *route_fn =
        new_generated_fn(p, api->module, ".%api.route", param_count, route_params, res_ty);

    struct dispatch_env env = {.api = api,
                               .openapi_fn = openapi_fn,
                               .req_ty = req_ty,
                               .res_ty = res_ty,
                               .text_ty = text_ty,
                               .bytes_ty = bytes_ty,
                               .req_var = route_params[0].name,
                               .req_method = lower_fresh(p),
                               .req_path = lower_fresh(p),
                               .req_body = lower_fresh(p),
                               .cap_vars = cap_vars,
                               .actor_vars = actor_vars};

    struct instance **route_fns =
        LOWER_NEW(p, struct instance *, api->route_count == 0 ? 1 : api->route_count);
    for (uint32_t r = 0; r < api->route_count; r++)
        route_fns[r] = emit_single_route_fn(p, &env, r);

    mo_core_string init_matched = lower_fresh(p);
    mo_core_expr chain = api_let(p, init_matched, lower_bool(p, false),
                                 build_route_chain(p, &env, route_fns, 0, init_matched));

    mo_core_expr unpack = lower_expr(p, MO_CORE_EXPR_CASE);
    unpack->as.case_.expr = lower_var(p, env.req_var);
    unpack->as.case_.arms.count = 1;
    unpack->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 1);
    struct mo_core_arm *uarm = &unpack->as.case_.arms.items[0];
    uarm->ctor = lower_text(p, "Request");
    uarm->names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 5), 5};
    uarm->names.items[0] = env.req_method;
    uarm->names.items[1] = env.req_path;
    uarm->names.items[2] = lower_fresh(p);
    uarm->names.items[3] = lower_fresh(p);
    uarm->names.items[4] = env.req_body;
    uarm->body = chain;
    route_fn->fun.body = unpack;

    struct mo_core_param *disp_params = LOWER_NEW(p, struct mo_core_param, param_count);
    for (size_t i = 0; i < param_count; i++)
        disp_params[i] = (struct mo_core_param){lower_fresh(p), route_params[i].ty};
    struct instance *disp_fn =
        new_generated_fn(p, api->module, ".%api.dispatch", param_count, disp_params, res_ty);

    mo_core_expr call_route = lower_expr(p, MO_CORE_EXPR_CALL);
    call_route->as.call.name = route_fn->fun.name;
    call_route->as.call.args = api_args(p, param_count);
    for (size_t i = 0; i < param_count; i++)
        call_route->as.call.args.items[i] = lower_var(p, disp_params[i].name);

    if (api->cors == NULL) {
        disp_fn->fun.body = call_route;
    } else {
        mo_core_string dm = lower_fresh(p);
        mo_core_expr cors_if = lower_expr(p, MO_CORE_EXPR_IF);
        cors_if->as.if_.cond = text_eq(p, text_ty, lower_var(p, dm), api_cstr(p, "OPTIONS"));
        cors_if->as.if_.then_ = api_lib_call1(p, "cors_preflight", api_cstr(p, api->cors));
        cors_if->as.if_.else_ = api_lib_call2(p, "apply_cors", call_route, api_cstr(p, api->cors));

        mo_core_expr dunpack = lower_expr(p, MO_CORE_EXPR_CASE);
        dunpack->as.case_.expr = lower_var(p, disp_params[0].name);
        dunpack->as.case_.arms.count = 1;
        dunpack->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 1);
        struct mo_core_arm *darm = &dunpack->as.case_.arms.items[0];
        darm->ctor = lower_text(p, "Request");
        darm->names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 5), 5};
        darm->names.items[0] = dm;
        for (size_t k = 1; k < 5; k++)
            darm->names.items[k] = lower_fresh(p);
        darm->body = cors_if;
        disp_fn->fun.body = dunpack;
    }

    if (api->static_count > 0) {
        size_t tparam_count = 2 + api->capability_count + api->actor_count;
        struct mo_core_param *tparams = LOWER_NEW(p, struct mo_core_param, tparam_count);
        tparams[0] = (struct mo_core_param){lower_fresh(p), route_params[0].ty};
        tparams[1] = (struct mo_core_param){lower_fresh(p), builtin_ty(p, "ReadFiles")};
        for (size_t i = 1; i < param_count; i++)
            tparams[1 + i] = (struct mo_core_param){lower_fresh(p), route_params[i].ty};
        struct instance *test_fn =
            new_generated_fn(p, api->module, ".%api.test_dispatch", tparam_count, tparams, res_ty);

        mo_core_expr fallback = lower_expr(p, MO_CORE_EXPR_CALL);
        fallback->as.call.name = disp_fn->fun.name;
        fallback->as.call.args = api_args(p, param_count);
        fallback->as.call.args.items[0] = lower_var(p, tparams[0].name);
        for (size_t i = 1; i < param_count; i++)
            fallback->as.call.args.items[i] = lower_var(p, tparams[1 + i].name);

        mo_core_expr static_chain = fallback;
        for (uint32_t s = api->static_count; s != 0; s--) {
            const struct mo_checked_api_static *st = &api->statics[s - 1];
            mo_core_expr st_call = api_lib_call4(
                p, "serve_static_fake", lower_var(p, tparams[0].name), api_cstr(p, st->prefix),
                api_cstr(p, st->dir), lower_var(p, tparams[1].name));
            mo_core_expr st_case = lower_expr(p, MO_CORE_EXPR_CASE);
            st_case->as.case_.expr = st_call;
            st_case->as.case_.arms.count = 2;
            st_case->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
            st_case->as.case_.arms.items[0] =
                (struct mo_core_arm){.ctor = lower_text(p, "None"), .body = static_chain};
            mo_core_string sres = lower_fresh(p);
            st_case->as.case_.arms.items[1].ctor = lower_text(p, "Some");
            st_case->as.case_.arms.items[1].names =
                (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
            st_case->as.case_.arms.items[1].names.items[0] = sres;
            st_case->as.case_.arms.items[1].body =
                api->cors == NULL
                    ? lower_var(p, sres)
                    : api_lib_call2(p, "apply_cors", lower_var(p, sres), api_cstr(p, api->cors));
            static_chain = st_case;
        }

        if (api->cors != NULL) {
            mo_core_string tm = lower_fresh(p);
            mo_core_expr tcors_if = lower_expr(p, MO_CORE_EXPR_IF);
            tcors_if->as.if_.cond = text_eq(p, text_ty, lower_var(p, tm), api_cstr(p, "OPTIONS"));
            tcors_if->as.if_.then_ = api_lib_call1(p, "cors_preflight", api_cstr(p, api->cors));
            tcors_if->as.if_.else_ = static_chain;

            mo_core_expr tunpack = lower_expr(p, MO_CORE_EXPR_CASE);
            tunpack->as.case_.expr = lower_var(p, tparams[0].name);
            tunpack->as.case_.arms.count = 1;
            tunpack->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 1);
            struct mo_core_arm *tarm = &tunpack->as.case_.arms.items[0];
            tarm->ctor = lower_text(p, "Request");
            tarm->names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 5), 5};
            tarm->names.items[0] = tm;
            for (size_t k = 1; k < 5; k++)
                tarm->names.items[k] = lower_fresh(p);
            tarm->body = tcors_if;
            test_fn->fun.body = tunpack;
        } else {
            test_fn->fun.body = static_chain;
        }
    }

    emit_wire_fn(p, api, disp_fn);
}

static void emit_wire_fn(struct lowering *p, const struct mo_checked_api *api,
                         const struct instance *disp_fn) {
    mo_core_ty req_ty = lower_type(p, lib_data_source(p, "Api", "Request"), NULL);
    mo_core_ty text_ty = builtin_ty(p, "Text");
    mo_core_ty bytes_ty = builtin_ty(p, "Bytes");
    mo_core_ty int_ty = builtin_ty(p, "Int");

    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type bytes_src = {
        .kind = MO_SOURCE_NAMED, .name = "Bytes", .module = UINT32_MAX};
    static const struct mo_source_type int_src = {
        .kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    static const struct mo_source_type *const pair_args[2] = {&text_src, &text_src};
    static const struct mo_source_type pair_src = {.kind = MO_SOURCE_TUPLE,
                                                   .name = "()",
                                                   .module = UINT32_MAX,
                                                   .count = 2,
                                                   .arguments = pair_args};
    static const struct mo_source_type *const hdrs_arg[1] = {&pair_src};
    static const struct mo_source_type hdrs_src = {.kind = MO_SOURCE_NAMED,
                                                   .name = "List",
                                                   .module = UINT32_MAX,
                                                   .count = 1,
                                                   .arguments = hdrs_arg};
    static const struct mo_source_type *const ret_args[3] = {&int_src, &hdrs_src, &bytes_src};
    static const struct mo_source_type ret_src = {.kind = MO_SOURCE_TUPLE,
                                                  .name = "()",
                                                  .module = UINT32_MAX,
                                                  .count = 3,
                                                  .arguments = ret_args};

    mo_core_ty hdrs_ty = lower_type(p, &hdrs_src, NULL);
    mo_core_ty ret_ty = lower_type(p, &ret_src, NULL);
    (void)int_ty;

    size_t param_count = 4 + api->capability_count + api->actor_count;
    struct mo_core_param *params = LOWER_NEW(p, struct mo_core_param, param_count);
    params[0] = (struct mo_core_param){lower_fresh(p), text_ty};  /* method */
    params[1] = (struct mo_core_param){lower_fresh(p), text_ty};  /* target */
    params[2] = (struct mo_core_param){lower_fresh(p), hdrs_ty};  /* headers */
    params[3] = (struct mo_core_param){lower_fresh(p), bytes_ty}; /* body */
    for (uint32_t c = 0; c < api->capability_count; c++)
        params[4 + c] = (struct mo_core_param){lower_fresh(p), builtin_ty(p, api->capabilities[c])};
    mo_core_ty sim_ty = builtin_ty(p, "Simulation");
    for (uint32_t a = 0; a < api->actor_count; a++)
        params[4 + api->capability_count + a] = (struct mo_core_param){lower_fresh(p), sim_ty};

    struct instance *wire_fn =
        new_generated_fn(p, api->module, ".%api.wire", param_count, params, ret_ty);

    mo_core_expr base_req =
        api_lib_call2(p, "request", lower_var(p, params[0].name), lower_var(p, params[1].name));
    mo_core_string rm = lower_fresh(p), rp = lower_fresh(p), rq = lower_fresh(p);
    mo_core_expr_list full_fields = api_args(p, 5);
    full_fields.items[0] = lower_var(p, rm);
    full_fields.items[1] = lower_var(p, rp);
    full_fields.items[2] = lower_var(p, rq);
    full_fields.items[3] = lower_var(p, params[2].name);
    full_fields.items[4] = lower_var(p, params[3].name);
    mo_core_expr full_req = api_con(p, req_ty, "Request", full_fields);

    mo_core_string req_v = lower_fresh(p);
    size_t disp_argc = 1 + api->capability_count + api->actor_count;
    mo_core_expr call_disp = lower_expr(p, MO_CORE_EXPR_CALL);
    call_disp->as.call.name = disp_fn->fun.name;
    call_disp->as.call.args = api_args(p, disp_argc);
    call_disp->as.call.args.items[0] = lower_var(p, req_v);
    for (uint32_t c = 0; c < api->capability_count; c++)
        call_disp->as.call.args.items[1 + c] = lower_var(p, params[4 + c].name);
    for (uint32_t a = 0; a < api->actor_count; a++)
        call_disp->as.call.args.items[1 + api->capability_count + a] =
            lower_var(p, params[4 + api->capability_count + a].name);

    mo_core_string st_v = lower_fresh(p), rh_v = lower_fresh(p), rb_v = lower_fresh(p);
    mo_core_expr tup = lower_expr(p, MO_CORE_EXPR_TUPLE);
    tup->as.tuple = api_args(p, 3);
    tup->as.tuple.items[0] = lower_var(p, st_v);
    tup->as.tuple.items[1] = lower_var(p, rh_v);
    tup->as.tuple.items[2] = lower_var(p, rb_v);

    mo_core_expr unpack_res = lower_expr(p, MO_CORE_EXPR_CASE);
    unpack_res->as.case_.expr = call_disp;
    unpack_res->as.case_.arms.count = 1;
    unpack_res->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 1);
    unpack_res->as.case_.arms.items[0].ctor = lower_text(p, "Response");
    unpack_res->as.case_.arms.items[0].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 3), 3};
    unpack_res->as.case_.arms.items[0].names.items[0] = st_v;
    unpack_res->as.case_.arms.items[0].names.items[1] = rh_v;
    unpack_res->as.case_.arms.items[0].names.items[2] = rb_v;
    unpack_res->as.case_.arms.items[0].body = tup;

    mo_core_expr unpack_req = lower_expr(p, MO_CORE_EXPR_CASE);
    unpack_req->as.case_.expr = base_req;
    unpack_req->as.case_.arms.count = 1;
    unpack_req->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 1);
    unpack_req->as.case_.arms.items[0].ctor = lower_text(p, "Request");
    unpack_req->as.case_.arms.items[0].names =
        (mo_core_string_list){LOWER_NEW(p, mo_core_string, 5), 5};
    unpack_req->as.case_.arms.items[0].names.items[0] = rm;
    unpack_req->as.case_.arms.items[0].names.items[1] = rp;
    unpack_req->as.case_.arms.items[0].names.items[2] = rq;
    unpack_req->as.case_.arms.items[0].names.items[3] = lower_fresh(p);
    unpack_req->as.case_.arms.items[0].names.items[4] = lower_fresh(p);
    unpack_req->as.case_.arms.items[0].body = api_let(p, req_v, full_req, unpack_res);

    wire_fn->fun.body = unpack_req;
}

static mo_core_expr execute_matched_job(struct lowering *p, const struct mo_checked_api *api,
                                        const struct mo_checked_api_job *job, mo_core_ty job_ret_ty,
                                        mo_core_string payload_json_var,
                                        const mo_core_string *cap_vars,
                                        const mo_core_string *actor_vars) {
    p->span = job->syntax->span;
    bool need_db[MO_LIMIT_API_SQLITES] = {false};
    for (uint32_t i = 1; i < job->param_count; i++)
        if (job->params[i].kind == MO_API_PARAM_SQLITE_DB)
            need_db[job->params[i].index] = true;

    mo_core_string db_vars[MO_LIMIT_API_SQLITES] = {{0}};
    for (uint32_t s = 0; s < api->sqlite_count; s++)
        if (need_db[s])
            db_vars[s] = lower_fresh(p);

    mo_core_string payload_var = lower_fresh(p);
    const struct mo_symbol *fn_sym = p->program->functions[job->function].signature->function;
    struct instance *fn_inst =
        lower_instance(p, fn_sym, (struct mo_type_arguments){0}, NULL, 0, NULL, fn_sym->module, 0);
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = fn_inst->fun.name;
    call->as.call.args = api_args(p, job->param_count);
    call->as.call.args.items[0] = lower_var(p, payload_var);
    for (uint32_t i = 1; i < job->param_count; i++) {
        const struct mo_checked_api_param *pm = &job->params[i];
        if (pm->kind == MO_API_PARAM_SQLITE_DB)
            call->as.call.args.items[i] = lower_var(p, db_vars[pm->index]);
        else if (pm->kind == MO_API_PARAM_ACTOR)
            call->as.call.args.items[i] = lower_var(p, actor_vars[pm->index]);
        else
            call->as.call.args.items[i] = lower_var(p, cap_vars[pm->index]);
    }

    mo_core_expr inner = NULL;
    if (job->reply == MO_API_JOB_UNIT) {
        inner = api_let(p, lower_fresh(p), call, api_con1(p, job_ret_ty, "Ok", lower_unit(p)));
    } else {
        mo_core_expr rcase = lower_expr(p, MO_CORE_EXPR_CASE);
        rcase->as.case_.expr = call;
        rcase->as.case_.arms.count = 2;
        rcase->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string uvar = lower_fresh(p), evar = lower_fresh(p);
        rcase->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        rcase->as.case_.arms.items[0].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        rcase->as.case_.arms.items[0].names.items[0] = uvar;
        rcase->as.case_.arms.items[0].body = api_con1(p, job_ret_ty, "Ok", lower_var(p, uvar));
        rcase->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        rcase->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        rcase->as.case_.arms.items[1].names.items[0] = evar;
        mo_core_expr err_txt =
            lower_to_text(p, job->error_text, NULL, api->module, 0, lower_var(p, evar));
        rcase->as.case_.arms.items[1].body = api_con1(p, job_ret_ty, "Error", err_txt);
        inner = rcase;
    }

    for (uint32_t s = api->sqlite_count; s != 0; s--) {
        uint32_t slot = s - 1;
        if (!need_db[slot])
            continue;
        MO_ASSERT(api->capability_count > 0 && strcmp(api->capabilities[0], "Sqlite") == 0);
        mo_core_expr_list open_args = api_args(p, 2);
        open_args.items[0] = lower_var(p, cap_vars[0]);
        open_args.items[1] = api_cstr(p, api->sqlites[slot].path);
        mo_core_expr opened =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                             .as.io = {MO_CORE_IO_OP_SQLITE_OPEN, false}},
                       open_args);
        mo_core_expr dcase = lower_expr(p, MO_CORE_EXPR_CASE);
        dcase->as.case_.expr = opened;
        dcase->as.case_.arms.count = 2;
        dcase->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
        mo_core_string derr = lower_fresh(p);
        dcase->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
        dcase->as.case_.arms.items[0].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        dcase->as.case_.arms.items[0].names.items[0] = db_vars[slot];
        dcase->as.case_.arms.items[0].body = inner;
        dcase->as.case_.arms.items[1].ctor = lower_text(p, "Error");
        dcase->as.case_.arms.items[1].names =
            (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
        dcase->as.case_.arms.items[1].names.items[0] = derr;
        dcase->as.case_.arms.items[1].body =
            api_lib_call1(p, "sqlite_job_error", lower_var(p, derr));
        inner = dcase;
    }

    mo_core_expr parsed = api_lib_call1(p, "parse_job_payload", lower_var(p, payload_json_var));
    mo_core_expr pcase = lower_expr(p, MO_CORE_EXPR_CASE);
    pcase->as.case_.expr = parsed;
    pcase->as.case_.arms.count = 2;
    pcase->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    mo_core_string perr = lower_fresh(p);
    pcase->as.case_.arms.items[0].ctor = lower_text(p, "Ok");
    pcase->as.case_.arms.items[0].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    pcase->as.case_.arms.items[0].names.items[0] = payload_var;
    pcase->as.case_.arms.items[0].body = inner;
    pcase->as.case_.arms.items[1].ctor = lower_text(p, "Error");
    pcase->as.case_.arms.items[1].names = (mo_core_string_list){LOWER_NEW(p, mo_core_string, 1), 1};
    pcase->as.case_.arms.items[1].names.items[0] = perr;
    pcase->as.case_.arms.items[1].body = api_con1(p, job_ret_ty, "Error", lower_var(p, perr));
    return pcase;
}

static void emit_job_fn(struct lowering *p, const struct mo_checked_api *api) {
    static const struct mo_source_type unit_src = {
        .kind = MO_SOURCE_NAMED, .name = "Unit", .module = UINT32_MAX};
    static const struct mo_source_type text_src = {
        .kind = MO_SOURCE_NAMED, .name = "Text", .module = UINT32_MAX};
    static const struct mo_source_type *const res_args[2] = {&unit_src, &text_src};
    static const struct mo_source_type res_src = {.kind = MO_SOURCE_NAMED,
                                                  .name = "Result",
                                                  .module = UINT32_MAX,
                                                  .count = 2,
                                                  .arguments = res_args};
    mo_core_ty text_ty = builtin_ty(p, "Text");
    mo_core_ty job_ret_ty = lower_type(p, &res_src, NULL);

    size_t param_count = 2 + api->capability_count + api->actor_count;
    struct mo_core_param *params = LOWER_NEW(p, struct mo_core_param, param_count);
    mo_core_string *cap_vars =
        LOWER_NEW(p, mo_core_string, api->capability_count == 0 ? 1 : api->capability_count);
    mo_core_string *actor_vars =
        LOWER_NEW(p, mo_core_string, api->actor_count == 0 ? 1 : api->actor_count);
    params[0] = (struct mo_core_param){lower_fresh(p), text_ty}; /* job_name */
    params[1] = (struct mo_core_param){lower_fresh(p), text_ty}; /* payload_json */
    for (uint32_t c = 0; c < api->capability_count; c++) {
        cap_vars[c] = lower_fresh(p);
        params[2 + c] = (struct mo_core_param){cap_vars[c], builtin_ty(p, api->capabilities[c])};
    }
    mo_core_ty sim_ty = builtin_ty(p, "Simulation");
    for (uint32_t a = 0; a < api->actor_count; a++) {
        actor_vars[a] = lower_fresh(p);
        params[2 + api->capability_count + a] = (struct mo_core_param){actor_vars[a], sim_ty};
    }

    struct instance *job_fn =
        new_generated_fn(p, api->module, ".%api.job", param_count, params, job_ret_ty);

    mo_core_expr chain = api_lib_call1(p, "unknown_job_error", lower_var(p, params[0].name));
    for (uint32_t j = api->job_count; j != 0; j--) {
        const struct mo_checked_api_job *job = &api->jobs[j - 1];
        mo_core_expr branch = lower_expr(p, MO_CORE_EXPR_IF);
        branch->as.if_.cond =
            text_eq(p, text_ty, lower_var(p, params[0].name), api_cstr(p, job->name));
        branch->as.if_.then_ =
            execute_matched_job(p, api, job, job_ret_ty, params[1].name, cap_vars, actor_vars);
        branch->as.if_.else_ = chain;
        chain = branch;
    }
    job_fn->fun.body = chain;
}

void lower_apis(struct lowering *p) {
    for (uint32_t i = 0; i < p->program->api_count; i++) {
        const struct mo_checked_api *api = &p->program->apis[i];
        p->span = api->syntax->span;
        struct instance *oa = emit_openapi_fn(p, api);
        emit_dispatch_fns(p, api, oa);
        emit_job_fn(p, api);
    }
}

static mo_core_expr default_fake_cap(struct lowering *p, const char *cap_name) {
    mo_core_ty text_ty = builtin_ty(p, "Text");
    if (strcmp(cap_name, "Sqlite") == 0) {
        return lower_prim(p,
                          (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                .as.io = {MO_CORE_IO_OP_SQLITE_FAKE, false}},
                          api_args(p, 0));
    }
    if (strcmp(cap_name, "Console") == 0) {
        return lower_prim(p,
                          (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                .as.io = {MO_CORE_IO_OP_CONSOLE_FAKE, false}},
                          api_args(p, 0));
    }
    if (strcmp(cap_name, "Random") == 0) {
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = lower_int(p, 0);
        return lower_prim(p,
                          (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                .as.io = {MO_CORE_IO_OP_RANDOM_SEEDED, false}},
                          a);
    }
    if (strcmp(cap_name, "SecureRandom") == 0) {
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = lower_int(p, 0);
        return lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                  .as.io = {MO_CORE_IO_OP_SECURE_RANDOM_SEEDED, false}},
            a);
    }
    if (strcmp(cap_name, "Clock") == 0) {
        mo_core_expr tlit = lower_unit(p);
        tlit->as.lit->tag = MO_CORE_LIT_TIME;
        tlit->as.lit->as.time.digits = lower_text(p, "1767225600000000"); /* 2026-01-01T00:00:00Z */
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = tlit;
        return lower_prim(p,
                          (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                .as.io = {MO_CORE_IO_OP_CLOCK_FIXED, false}},
                          a);
    }
    if (strcmp(cap_name, "Env") == 0) {
        mo_core_expr empty_map =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                             .as.map = {.k = text_ty,
                                                        .v = text_ty,
                                                        .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                               .lit.digits = lower_text(p, "0")}}},
                       api_args(p, 0));
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = empty_map;
        return lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_IO, .as.io = {MO_CORE_IO_OP_ENV_OF, false}},
            a);
    }
    if (strcmp(cap_name, "Files") == 0 || strcmp(cap_name, "ReadFiles") == 0) {
        mo_core_expr empty_map =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                             .as.map = {.k = text_ty,
                                                        .v = text_ty,
                                                        .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                               .lit.digits = lower_text(p, "0")}}},
                       api_args(p, 0));
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = empty_map;
        mo_core_expr files =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                             .as.io = {MO_CORE_IO_OP_FILES_IN_MEMORY, false}},
                       a);
        if (strcmp(cap_name, "Files") == 0)
            return files;
        mo_core_expr_list ro = api_args(p, 1);
        ro.items[0] = files;
        return lower_prim(p,
                          (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                .as.io = {MO_CORE_IO_OP_READ_ONLY, false}},
                          ro);
    }
    if (strcmp(cap_name, "Command") == 0) {
        mo_core_ty bytes_ty = builtin_ty(p, "Bytes");
        mo_core_expr empty_cmd =
            lower_prim(p,
                       (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                             .as.map = {.k = bytes_ty,
                                                        .v = bytes_ty,
                                                        .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                               .lit.digits = lower_text(p, "0")}}},
                       api_args(p, 0));
        mo_core_expr_list a = api_args(p, 1);
        a.items[0] = empty_cmd;
        return lower_prim(p,
                          (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                .as.io = {MO_CORE_IO_OP_COMMAND_FAKE, false}},
                          a);
    }
    MO_ASSERT(strcmp(cap_name, "Network") == 0);
    static const struct mo_source_type bytes_src = {
        .kind = MO_SOURCE_NAMED, .name = "Bytes", .module = UINT32_MAX};
    static const struct mo_source_type *const inner_args[2] = {&bytes_src, &bytes_src};
    static const struct mo_source_type inner_src = {.kind = MO_SOURCE_NAMED,
                                                    .name = "Map",
                                                    .module = UINT32_MAX,
                                                    .count = 2,
                                                    .arguments = inner_args};
    mo_core_ty inner_map_ty = lower_type(p, &inner_src, NULL);
    mo_core_expr empty_net =
        lower_prim(p,
                   (struct mo_core_prim){.tag = MO_CORE_PRIM_MAP,
                                         .as.map = {.k = text_ty,
                                                    .v = inner_map_ty,
                                                    .op = {.tag = MO_CORE_MAP_OP_LIT,
                                                           .lit.digits = lower_text(p, "0")}}},
                   api_args(p, 0));
    mo_core_expr_list a = api_args(p, 1);
    a.items[0] = empty_net;
    return lower_prim(
        p,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_IO, .as.io = {MO_CORE_IO_OP_NETWORK_FAKE, false}},
        a);
}

mo_core_expr lower_apitest_send(struct lowering *p, uint32_t api_index, mo_core_expr_list args,
                                const struct mo_source_type *const *arg_types) {
    MO_ASSERT(api_index < p->program->api_count && args.count >= 1);
    const struct mo_checked_api *api = &p->program->apis[api_index];

    size_t supplied = args.count - 1;
    mo_core_string *sup_vars = LOWER_NEW(p, mo_core_string, supplied == 0 ? 1 : supplied);
    for (size_t i = 0; i < supplied; i++)
        sup_vars[i] = lower_fresh(p);

    mo_core_string req_var = lower_fresh(p);
    bool has_static = api->static_count > 0;
    size_t extra_static = has_static ? 1u : 0u;
    size_t call_arity = 1 + extra_static + api->capability_count + api->actor_count;
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name =
        lower_concat(p, lower_text(p, p->program->set->names[api->module]),
                     has_static ? ".%api.test_dispatch" : ".%api.dispatch", lower_text(p, ""));
    call->as.call.args = api_args(p, call_arity);
    call->as.call.args.items[0] = lower_var(p, req_var);

    if (has_static) {
        mo_core_expr st_files = NULL;
        for (size_t i = 0; i < supplied; i++) {
            if (arg_types[1 + i]->declaration == NULL &&
                strcmp(arg_types[1 + i]->name, "ReadFiles") == 0) {
                st_files = lower_var(p, sup_vars[i]);
                break;
            }
        }
        if (st_files == NULL) {
            for (size_t i = 0; i < supplied; i++) {
                if (arg_types[1 + i]->declaration == NULL &&
                    strcmp(arg_types[1 + i]->name, "Files") == 0) {
                    mo_core_expr_list ro = api_args(p, 1);
                    ro.items[0] = lower_var(p, sup_vars[i]);
                    st_files =
                        lower_prim(p,
                                   (struct mo_core_prim){.tag = MO_CORE_PRIM_IO,
                                                         .as.io = {MO_CORE_IO_OP_READ_ONLY, false}},
                                   ro);
                    break;
                }
            }
        }
        call->as.call.args.items[1] =
            st_files != NULL ? st_files : default_fake_cap(p, "ReadFiles");
    }

    for (uint32_t c = 0; c < api->capability_count; c++) {
        const char *wanted = api->capabilities[c];
        mo_core_expr chosen = NULL;
        for (size_t i = 0; i < supplied; i++)
            if (arg_types[1 + i]->declaration == NULL &&
                strcmp(arg_types[1 + i]->name, wanted) == 0) {
                chosen = lower_var(p, sup_vars[i]);
                break;
            }
        call->as.call.args.items[1 + extra_static + c] =
            chosen != NULL ? chosen : default_fake_cap(p, wanted);
    }

    for (uint32_t a = 0; a < api->actor_count; a++) {
        const struct mo_symbol *wanted_actor = api->actors[a].type->declaration;
        mo_core_expr chosen = NULL;
        for (size_t i = 0; i < supplied; i++)
            if (arg_types[1 + i]->declaration == wanted_actor) {
                chosen = lower_var(p, sup_vars[i]);
                break;
            }
        if (chosen == NULL) {
            mo_core_expr tlit = lower_unit(p);
            tlit->as.lit->tag = MO_CORE_LIT_TIME;
            tlit->as.lit->as.time.digits =
                lower_text(p, "1767225600000000"); /* 2026-01-01T00:00:00Z */
            mo_core_expr_list sargs = api_args(p, 1);
            sargs.items[0] = tlit;
            struct mo_core_sim_op op = {
                .tag = MO_SIM_START,
                .descriptor = {.digits = lower_number(p, api->actors[a].service_index)},
                .tool = {.digits = lower_text(p, "0")}};
            chosen =
                lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_SIM, .as.sim = op}, sargs);
        }
        call->as.call.args.items[1 + extra_static + api->capability_count + a] = chosen;
    }

    mo_core_expr body = call;
    for (size_t i = supplied; i != 0; i--)
        body = api_let(p, sup_vars[i - 1], args.items[i], body);
    return api_let(p, req_var, args.items[0], body);
}

mo_core_expr lower_apitest_run_job(struct lowering *p, uint32_t api_index, mo_core_expr_list args,
                                   const struct mo_source_type *const *arg_types) {
    MO_ASSERT(api_index < p->program->api_count && args.count >= 2);
    const struct mo_checked_api *api = &p->program->apis[api_index];

    size_t supplied = args.count - 2;
    mo_core_string *sup_vars = LOWER_NEW(p, mo_core_string, supplied == 0 ? 1 : supplied);
    for (size_t i = 0; i < supplied; i++)
        sup_vars[i] = lower_fresh(p);

    mo_core_string name_var = lower_fresh(p);
    mo_core_string payload_var = lower_fresh(p);
    mo_core_expr payload_text = api_lib_call1(p, "encode_job_payload", lower_var(p, payload_var));

    size_t call_arity = 2 + api->capability_count + api->actor_count;
    mo_core_expr call = lower_expr(p, MO_CORE_EXPR_CALL);
    call->as.call.name = lower_concat(p, lower_text(p, p->program->set->names[api->module]),
                                      ".%api.job", lower_text(p, ""));
    call->as.call.args = api_args(p, call_arity);
    call->as.call.args.items[0] = lower_var(p, name_var);
    call->as.call.args.items[1] = payload_text;

    for (uint32_t c = 0; c < api->capability_count; c++) {
        const char *wanted = api->capabilities[c];
        mo_core_expr chosen = NULL;
        for (size_t i = 0; i < supplied; i++)
            if (arg_types[2 + i]->declaration == NULL &&
                strcmp(arg_types[2 + i]->name, wanted) == 0) {
                chosen = lower_var(p, sup_vars[i]);
                break;
            }
        call->as.call.args.items[2 + c] = chosen != NULL ? chosen : default_fake_cap(p, wanted);
    }

    for (uint32_t a = 0; a < api->actor_count; a++) {
        const struct mo_symbol *wanted_actor = api->actors[a].type->declaration;
        mo_core_expr chosen = NULL;
        for (size_t i = 0; i < supplied; i++)
            if (arg_types[2 + i]->declaration == wanted_actor) {
                chosen = lower_var(p, sup_vars[i]);
                break;
            }
        if (chosen == NULL) {
            mo_core_expr tlit = lower_unit(p);
            tlit->as.lit->tag = MO_CORE_LIT_TIME;
            tlit->as.lit->as.time.digits =
                lower_text(p, "1767225600000000"); /* 2026-01-01T00:00:00Z */
            mo_core_expr_list sargs = api_args(p, 1);
            sargs.items[0] = tlit;
            struct mo_core_sim_op op = {
                .tag = MO_SIM_START,
                .descriptor = {.digits = lower_number(p, api->actors[a].service_index)},
                .tool = {.digits = lower_text(p, "0")}};
            chosen =
                lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_SIM, .as.sim = op}, sargs);
        }
        call->as.call.args.items[2 + api->capability_count + a] = chosen;
    }

    mo_core_expr body = call;
    for (size_t i = supplied; i != 0; i--)
        body = api_let(p, sup_vars[i - 1], args.items[2 + i - 1], body);
    body = api_let(p, payload_var, args.items[1], body);
    return api_let(p, name_var, args.items[0], body);
}
