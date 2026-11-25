//! HTTP `api` block lowering (OpenAPI 3.1.0, per-route handlers, dispatch, CORS, static file
//! test dispatch, wire adapter, background jobs) and `ApiTest.send` / `ApiTest.run_job`
//! (`src/lower/apis.c` parity).

use mo_check::{
    ApiAuthMode, ApiJobReplyKind, ApiMethod, ApiParamKind, ApiReplyKind, CheckedApi, CheckedApiJob,
    CheckedApiRoute, ModuleId, ProofId, SourceType, SourceTypeKind, SymRef, TypeId, TypeOwner,
};
use mo_core::{
    Arm, CoreInteger, CoreNatural, Expr, ExprId, FnKind, IoOp, ListOp, Lit, Param, Prim, SimOp,
    TyId,
};
use mo_def::ErrorCode;
use mo_def::core_ops::{CoreIoNullaryOp, CoreListNullaryOp, CoreTextOp};
use mo_syntax::{NodeId, SyntaxKind};

use crate::abilities::{lower_json_decode_call, lower_json_schema, lower_to_json};
use crate::context::{LowerResult, Lowering};
use crate::scalar::lower_to_text;
use crate::types::{lower_ctors, lower_instance, lower_type};

fn lib_module(p: &mut Lowering<'_>, mod_name: &str) -> LowerResult<ModuleId> {
    for i in 0..p.program.interfaces.scopes.len() {
        p.work(1)?;
        let scope = &p.program.interfaces.scopes[i];
        if scope.library && scope.name.as_deref() == Some(mod_name) {
            return Ok(ModuleId(i as u32));
        }
    }
    p.refuse(
        ErrorCode::LowerUnfinished,
        "api lowering needs the standard library",
    )
}

fn lib_data_source(p: &mut Lowering<'_>, mod_name: &str, type_name: &str) -> LowerResult<TypeId> {
    let mid = lib_module(p, mod_name)?;
    let Some(sidx) =
        p.program.interfaces.scopes[mid.index()].find_symbol_index(type_name.as_bytes())
    else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "api lowering needs the standard library",
        );
    };
    let sym = SymRef::new(mid, sidx);
    for d in &p.program.signatures.data {
        p.work(1)?;
        if p.source_type(d.ty).owner == TypeOwner::Source(sym) {
            return Ok(d.ty);
        }
    }
    p.refuse(
        ErrorCode::LowerUnfinished,
        "api lowering needs the standard library",
    )
}

fn lib_call(
    p: &mut Lowering<'_>,
    mod_name: &str,
    fn_name: &str,
    args: Vec<ExprId>,
) -> LowerResult<ExprId> {
    let mid = lib_module(p, mod_name)?;
    let Some(sidx) = p.program.interfaces.scopes[mid.index()].find_symbol_index(fn_name.as_bytes())
    else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "api lowering needs the standard library",
        );
    };
    let sym = SymRef::new(mid, sidx);
    let inst = lower_instance(p, sym, &[], None, &[], sym.module, 0)?;
    let fname = p.instances[inst].fun.name.clone();
    p.expr_call(fname, args)
}

fn api_call(p: &mut Lowering<'_>, fn_name: &str, args: Vec<ExprId>) -> LowerResult<ExprId> {
    lib_call(p, "Api", fn_name, args)
}

fn builtin_ty(p: &mut Lowering<'_>, name: &str) -> LowerResult<TyId> {
    let src = p.types_overlay.alloc(SourceType {
        kind: SourceTypeKind::Named,
        name: name.to_string(),
        module: None,
        owner: TypeOwner::None,
        arguments: Vec::new(),
        height: 0,
        parameters: 0,
    });
    lower_type(p, src, None)
}

fn json_obj(
    p: &mut Lowering<'_>,
    json_ty: TyId,
    pairs: Vec<(&str, ExprId)>,
) -> LowerResult<ExprId> {
    let text_ty = builtin_ty(p, "Text")?;
    let mut flat = Vec::with_capacity(pairs.len() * 2);
    for (k, v) in pairs {
        flat.push(p.expr_text(k)?);
        flat.push(v);
    }
    let map_lit = p.expr_lit_map(text_ty, json_ty, flat)?;
    let ctor = p.copy_text("Object")?;
    p.expr_con(json_ty, ctor, vec![map_lit])
}

fn json_obj_owned(
    p: &mut Lowering<'_>,
    json_ty: TyId,
    pairs: Vec<(String, ExprId)>,
) -> LowerResult<ExprId> {
    let text_ty = builtin_ty(p, "Text")?;
    let mut flat = Vec::with_capacity(pairs.len() * 2);
    for (k, v) in pairs {
        flat.push(p.expr_text(&k)?);
        flat.push(v);
    }
    let map_lit = p.expr_lit_map(text_ty, json_ty, flat)?;
    let ctor = p.copy_text("Object")?;
    p.expr_con(json_ty, ctor, vec![map_lit])
}

fn json_arr(p: &mut Lowering<'_>, json_ty: TyId, items: Vec<ExprId>) -> LowerResult<ExprId> {
    let list_lit = p.expr_lit_list(json_ty, items)?;
    let ctor = p.copy_text("Array")?;
    p.expr_con(json_ty, ctor, vec![list_lit])
}

fn json_str(p: &mut Lowering<'_>, json_ty: TyId, s: &str) -> LowerResult<ExprId> {
    let t = p.expr_text(s)?;
    let ctor = p.copy_text("String")?;
    p.expr_con(json_ty, ctor, vec![t])
}

fn json_bool(p: &mut Lowering<'_>, json_ty: TyId, b: bool) -> LowerResult<ExprId> {
    let v = p.expr_bool(b)?;
    let ctor = p.copy_text("Boolean")?;
    p.expr_con(json_ty, ctor, vec![v])
}

const fn method_upper(m: ApiMethod) -> &'static str {
    match m {
        ApiMethod::Get => "GET",
        ApiMethod::Post => "POST",
        ApiMethod::Put => "PUT",
        ApiMethod::Patch => "PATCH",
        ApiMethod::Delete => "DELETE",
    }
}

const fn method_lower(m: ApiMethod) -> &'static str {
    match m {
        ApiMethod::Get => "get",
        ApiMethod::Post => "post",
        ApiMethod::Put => "put",
        ApiMethod::Patch => "patch",
        ApiMethod::Delete => "delete",
    }
}

fn openapi_path_template(p: &mut Lowering<'_>, path: &str) -> LowerResult<String> {
    let bytes = path.as_bytes();
    let mut out = String::with_capacity(bytes.len() * 2 + 2);
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] == b':' && (i == 0 || bytes[i - 1] == b'/') {
            out.push('{');
            i += 1;
            while i < bytes.len() && bytes[i] != b'/' {
                out.push(char::from(bytes[i]));
                i += 1;
            }
            out.push('}');
        } else {
            out.push(char::from(bytes[i]));
            i += 1;
        }
    }
    p.charge_alloc(out.len() + 1, 1, 1)?;
    Ok(out)
}

fn fn_doc_summary(p: &mut Lowering<'_>, module: ModuleId, fn_decl: NodeId) -> LowerResult<String> {
    let tree = p.module_tree(module);
    let leading = &tree.node(fn_decl).leading;
    let mut out = String::new();
    let mut first = true;
    for &cid in leading {
        let c = tree.comment(cid);
        if !c.documentation {
            continue;
        }
        let raw = tree.comment_text(cid);
        let mut start = 2usize.min(raw.len());
        if start < raw.len() && raw[start] == b' ' {
            start += 1;
        }
        if !first {
            out.push(' ');
        }
        out.push_str(&String::from_utf8_lossy(&raw[start..]));
        first = false;
    }
    if !out.is_empty() {
        p.charge_alloc(out.len() + 1, 1, 1)?;
    }
    Ok(out)
}

fn scalar_openapi_schema(p: &mut Lowering<'_>, ty_id: TypeId) -> LowerResult<ExprId> {
    let name = p.source_type(ty_id).name.clone();
    match name.as_str() {
        "Int" => {
            let s = p.expr_text("integer")?;
            lib_call(p, "Json", "schema_type", vec![s])
        }
        "Byte" => lib_call(p, "Json", "schema_byte", Vec::new()),
        "Float" => {
            let s = p.expr_text("number")?;
            lib_call(p, "Json", "schema_type", vec![s])
        }
        "Bool" => {
            let s = p.expr_text("boolean")?;
            lib_call(p, "Json", "schema_type", vec![s])
        }
        "Decimal" | "BigInt" => {
            let s1 = p.expr_text("string")?;
            let s2 = p.expr_text(if name == "Decimal" {
                "decimal"
            } else {
                "bigint"
            })?;
            lib_call(p, "Json", "schema_format", vec![s1, s2])
        }
        _ => {
            let s = p.expr_text("string")?;
            lib_call(p, "Json", "schema_type", vec![s])
        }
    }
}

fn empty_text_list(p: &mut Lowering<'_>) -> LowerResult<ExprId> {
    let text_ty = builtin_ty(p, "Text")?;
    p.expr_lit_list(text_ty, Vec::new())
}

fn collect_status_codes(
    p: &Lowering<'_>,
    api: &CheckedApi,
    err_ty: Option<TypeId>,
    codes: &mut Vec<u16>,
) {
    let Some(et) = err_ty else {
        return;
    };
    let Some(en) = p.source_type(et).owner.as_source() else {
        return;
    };
    if p.program.interfaces.symbol(en).decl_kind != SyntaxKind::Enum {
        return;
    }
    for st in &api.statuses {
        if st.error_enum == en && !codes.contains(&st.status_code) {
            codes.push(st.status_code);
        }
    }
}

fn build_route_operation_json(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    route: &CheckedApiRoute,
    json_ty: TyId,
) -> LowerResult<ExprId> {
    let mut fields: Vec<(String, ExprId)> = Vec::new();
    let fn_sym = p.program.function(route.function).symbol;
    let (fn_name, fn_mod, fn_decl) = {
        let s = p.program.interfaces.symbol(fn_sym);
        (s.name.clone(), s.module, s.declaration)
    };
    fields.push(("operationId".to_string(), json_str(p, json_ty, &fn_name)?));

    let summary = fn_doc_summary(p, fn_mod, fn_decl)?;
    if !summary.is_empty() {
        fields.push(("summary".to_string(), json_str(p, json_ty, &summary)?));
    }

    if route.auth_mode == ApiAuthMode::Required {
        let sv = json_arr(p, json_ty, Vec::new())?;
        let sec_item = json_obj(p, json_ty, vec![("bearerAuth", sv)])?;
        fields.push((
            "security".to_string(),
            json_arr(p, json_ty, vec![sec_item])?,
        ));
    }

    let mut oa_params = Vec::new();
    let mut body_param = None;
    for pm in &route.params {
        match pm.kind {
            ApiParamKind::Path | ApiParamKind::QueryRequired | ApiParamKind::QueryOptional => {
                let in_path = pm.kind == ApiParamKind::Path;
                let reqd = pm.kind != ApiParamKind::QueryOptional;
                let st = if pm.kind == ApiParamKind::QueryOptional {
                    p.source_type(pm.ty).arguments[0]
                } else {
                    pm.ty
                };
                let n_v = json_str(p, json_ty, &pm.name)?;
                let in_v = json_str(p, json_ty, if in_path { "path" } else { "query" })?;
                let req_v = json_bool(p, json_ty, reqd)?;
                let sch_v = scalar_openapi_schema(p, st)?;
                let p_obj = json_obj(
                    p,
                    json_ty,
                    vec![
                        ("name", n_v),
                        ("in", in_v),
                        ("required", req_v),
                        ("schema", sch_v),
                    ],
                )?;
                oa_params.push(p_obj);
            }
            ApiParamKind::Body => body_param = Some(pm),
            ApiParamKind::Auth
            | ApiParamKind::AuthOptional
            | ApiParamKind::Request
            | ApiParamKind::SqliteDb
            | ApiParamKind::Actor
            | ApiParamKind::Capability => {}
        }
    }
    let has_oa_params = !oa_params.is_empty();
    if has_oa_params {
        fields.push(("parameters".to_string(), json_arr(p, json_ty, oa_params)?));
    }

    if let Some(bp) = body_param
        && let Some(bjson) = bp.json
    {
        let seen = empty_text_list(p)?;
        let sv = lower_json_schema(p, bjson, None, api.module, 0, seen)?;
        let ctv = json_obj(p, json_ty, vec![("schema", sv)])?;
        let content = json_obj(p, json_ty, vec![("application/json", ctv)])?;
        let req_true = json_bool(p, json_ty, true)?;
        let req_body = json_obj(
            p,
            json_ty,
            vec![("required", req_true), ("content", content)],
        )?;
        fields.push(("requestBody".to_string(), req_body));
    }

    let mut err_codes = Vec::new();
    if route.auth_mode == ApiAuthMode::Required
        && let Some(ref auth) = api.auth
    {
        collect_status_codes(p, api, Some(auth.error_type), &mut err_codes);
    }
    collect_status_codes(p, api, route.error_type, &mut err_codes);

    let has_422 = (has_oa_params || body_param.is_some()) && !err_codes.contains(&422);
    let is_204 = matches!(route.reply, ApiReplyKind::Unit | ApiReplyKind::ResultUnit);

    let mut resp_pairs: Vec<(String, ExprId)> = Vec::new();
    let ok_code = if is_204 { "204" } else { "200" }.to_string();
    let ok_val = if let Some(ok_json) = route.ok_json {
        let seen = empty_text_list(p)?;
        let sv = lower_json_schema(p, ok_json, None, api.module, 0, seen)?;
        let ctv = json_obj(p, json_ty, vec![("schema", sv)])?;
        let content = json_obj(p, json_ty, vec![("application/json", ctv)])?;
        let ok_desc = json_str(p, json_ty, "OK")?;
        json_obj(
            p,
            json_ty,
            vec![("description", ok_desc), ("content", content)],
        )?
    } else {
        let desc = if is_204 { "No Content" } else { "OK" };
        let desc_v = json_str(p, json_ty, desc)?;
        json_obj(p, json_ty, vec![("description", desc_v)])?
    };
    resp_pairs.push((ok_code, ok_val));

    if has_422 {
        let val_desc = json_str(p, json_ty, "Validation Error")?;
        let v = json_obj(p, json_ty, vec![("description", val_desc)])?;
        resp_pairs.push(("422".to_string(), v));
    }

    for sc in err_codes {
        if (is_204 && sc == 204) || (!is_204 && sc == 200) {
            continue;
        }
        let err_desc = json_str(p, json_ty, "Application Error")?;
        let v = json_obj(p, json_ty, vec![("description", err_desc)])?;
        resp_pairs.push((sc.to_string(), v));
    }

    let responses = json_obj_owned(p, json_ty, resp_pairs)?;
    fields.push(("responses".to_string(), responses));

    json_obj_owned(p, json_ty, fields)
}

fn new_generated_fn(
    p: &mut Lowering<'_>,
    module: ModuleId,
    suffix: &str,
    params: Vec<Param>,
    ret_ty: TyId,
) -> LowerResult<usize> {
    let mod_name = p.module_name(module).to_string();
    let name = p.concat_text(&mod_name, suffix, "")?;
    let idx = p.push_instance(
        module,
        name.clone(),
        FnKind::Function,
        ret_ty,
        name,
        p.lower_span(p.span),
    )?;
    let const_count = p.program.constants.len() as u32;
    let digits = p.number_text(u64::from(const_count))?;
    let pcount = params.len();
    let inst = &mut p.instances[idx];
    inst.const_prefix = const_count;
    inst.fun.const_count = CoreNatural { digits };
    inst.fun.params = params;
    inst.meta.body.params = vec![None; pcount];
    Ok(idx)
}

fn emit_openapi_fn(p: &mut Lowering<'_>, api: &CheckedApi) -> LowerResult<usize> {
    let text_ty = builtin_ty(p, "Text")?;
    let json_src = lib_data_source(p, "Json", "Json")?;
    let json_ty = lower_type(p, json_src, None)?;
    let fn_idx = new_generated_fn(p, api.module, ".%api.openapi", Vec::new(), text_ty)?;

    let mut oa_paths = Vec::with_capacity(api.routes.len());
    for r in &api.routes {
        oa_paths.push(openapi_path_template(p, &r.path)?);
    }

    let mut done = vec![false; api.routes.len()];
    let mut path_pairs = Vec::new();
    for i in 0..api.routes.len() {
        if done[i] {
            continue;
        }
        let mut method_pairs = Vec::new();
        for j in i..api.routes.len() {
            if oa_paths[i] != oa_paths[j] {
                continue;
            }
            done[j] = true;
            let op = build_route_operation_json(p, api, &api.routes[j], json_ty)?;
            method_pairs.push((method_lower(api.routes[j].method).to_string(), op));
        }
        let obj = json_obj_owned(p, json_ty, method_pairs)?;
        path_pairs.push((oa_paths[i].clone(), obj));
    }

    let mod_name = p.module_name(api.module).to_string();
    let title_v = json_str(p, json_ty, &mod_name)?;
    let ver_v = json_str(p, json_ty, "0.1.0")?;
    let info = json_obj(p, json_ty, vec![("title", title_v), ("version", ver_v)])?;
    let paths_obj = json_obj_owned(p, json_ty, path_pairs)?;
    let mut top = vec![
        ("openapi".to_string(), json_str(p, json_ty, "3.1.0")?),
        ("info".to_string(), info),
        ("paths".to_string(), paths_obj),
    ];
    if api.auth.is_some() {
        let http_v = json_str(p, json_ty, "http")?;
        let bearer_v = json_str(p, json_ty, "bearer")?;
        let bearer = json_obj(p, json_ty, vec![("type", http_v), ("scheme", bearer_v)])?;
        let schemes = json_obj(p, json_ty, vec![("bearerAuth", bearer)])?;
        let comps = json_obj(p, json_ty, vec![("securitySchemes", schemes)])?;
        top.push(("components".to_string(), comps));
    }
    let doc = json_obj_owned(p, json_ty, top)?;
    let body = api_call(p, "render_json", vec![doc])?;
    p.instances[fn_idx].fun.body = body;
    p.instances[fn_idx].body_set = true;
    Ok(fn_idx)
}

#[derive(Clone)]
struct DispatchEnv {
    openapi_fn_name: String,
    req_ty: TyId,
    res_ty: TyId,
    text_ty: TyId,
    bytes_ty: TyId,
    req_var: String,
    req_method: String,
    req_path: String,
    req_body: String,
    cap_vars: Vec<String>,
    actor_vars: Vec<String>,
}

fn encode_domain_error(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    env: &DispatchEnv,
    err_src: TypeId,
    err_json: Option<ProofId>,
    err_expr: ExprId,
) -> LowerResult<ExprId> {
    let err_ty = lower_type(p, err_src, None)?;
    if err_ty == env.res_ty {
        return Ok(err_expr);
    }
    let Some(en) = p.source_type(err_src).owner.as_source() else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "domain error missing enum owner",
        );
    };
    let Some(ej) = err_json else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "domain error missing Json proof",
        );
    };
    let ev = p.fresh()?;
    let ctors = lower_ctors(p, err_ty)?;
    let mut arms = Vec::with_capacity(ctors.len());
    for ctor in &ctors {
        let mut names = Vec::with_capacity(ctor.fields.len());
        for _ in 0..ctor.fields.len() {
            names.push(p.fresh()?);
        }
        let mut status_code = 500u16;
        for st in &api.statuses {
            if st.error_enum == en && p.program.interfaces.symbol(st.variant).name == ctor.name {
                status_code = st.status_code;
                break;
            }
        }
        let code_expr = p.expr_int(u64::from(status_code))?;
        arms.push(Arm {
            ctor: ctor.name.clone(),
            names,
            body: code_expr,
        });
    }
    let ev1 = p.expr_var(ev.clone())?;
    let match_e = p.alloc_expr(Expr::Case { expr: ev1, arms })?;
    let ev2 = p.expr_var(ev.clone())?;
    let json_val = lower_to_json(p, ej, None, api.module, 0, ev2)?;
    let resp = api_call(p, "domain_error_response", vec![match_e, json_val])?;
    p.expr_let(ev, err_expr, resp)
}

fn scalar_suffix(p: &Lowering<'_>, ty_id: TypeId) -> &'static str {
    match p.source_type(ty_id).name.as_str() {
        "Int" => "int",
        "Byte" => "byte",
        "BigInt" => "bigint",
        "Decimal" => "decimal",
        "Float" => "float",
        "Bool" => "bool",
        _ => "text",
    }
}

fn unwrap_or_return_response(
    p: &mut Lowering<'_>,
    result_expr: ExprId,
    ok_var: String,
    cont: ExprId,
) -> LowerResult<ExprId> {
    let err_res = p.fresh()?;
    let err_v = p.expr_var(err_res.clone())?;
    let ok_ctor = p.copy_text("Ok")?;
    let err_ctor = p.copy_text("Error")?;
    p.alloc_expr(Expr::Case {
        expr: result_expr,
        arms: vec![
            Arm {
                ctor: ok_ctor,
                names: vec![ok_var],
                body: cont,
            },
            Arm {
                ctor: err_ctor,
                names: vec![err_res],
                body: err_v,
            },
        ],
    })
}

fn invoke_route_handler(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    env: &DispatchEnv,
    route: &CheckedApiRoute,
    arg_vars: &[String],
) -> LowerResult<ExprId> {
    let fn_sym = p.program.function(route.function).symbol;
    let fn_inst = lower_instance(p, fn_sym, &[], None, &[], fn_sym.module, 0)?;
    let fname = p.instances[fn_inst].fun.name.clone();
    let mut args = Vec::with_capacity(arg_vars.len());
    for av in arg_vars {
        args.push(p.expr_var(av.clone())?);
    }
    let call = p.expr_call(fname, args)?;

    match route.reply {
        ApiReplyKind::Response => Ok(call),
        ApiReplyKind::Unit => {
            let tmp = p.fresh()?;
            let nc = api_call(p, "no_content", Vec::new())?;
            p.expr_let(tmp, call, nc)
        }
        ApiReplyKind::Json => {
            let Some(ok_json) = route.ok_json else {
                return p.refuse(ErrorCode::LowerUnfinished, "Json route missing proof");
            };
            let v = p.fresh()?;
            let vv = p.expr_var(v.clone())?;
            let j = lower_to_json(p, ok_json, None, api.module, 0, vv)?;
            let code = p.expr_int(200)?;
            let resp = api_call(p, "encoded_json_response", vec![code, j])?;
            p.expr_let(v, call, resp)
        }
        ApiReplyKind::ResultResponse | ApiReplyKind::ResultUnit | ApiReplyKind::ResultJson => {
            let ok_v = p.fresh()?;
            let err_v = p.fresh()?;
            let ok_body = match route.reply {
                ApiReplyKind::ResultResponse => p.expr_var(ok_v.clone())?,
                ApiReplyKind::ResultUnit => api_call(p, "no_content", Vec::new())?,
                ApiReplyKind::ResultJson => {
                    let Some(ok_json) = route.ok_json else {
                        return p
                            .refuse(ErrorCode::LowerUnfinished, "ResultJson route missing proof");
                    };
                    let ov = p.expr_var(ok_v.clone())?;
                    let j = lower_to_json(p, ok_json, None, api.module, 0, ov)?;
                    let code = p.expr_int(200)?;
                    api_call(p, "encoded_json_response", vec![code, j])?
                }
                _ => p.expr_var(ok_v.clone())?,
            };
            let Some(err_ty) = route.error_type else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "Result route missing error_type",
                );
            };
            let ev = p.expr_var(err_v.clone())?;
            let err_body = encode_domain_error(p, api, env, err_ty, route.error_json, ev)?;
            let ok_ctor = p.copy_text("Ok")?;
            let err_ctor = p.copy_text("Error")?;
            p.alloc_expr(Expr::Case {
                expr: call,
                arms: vec![
                    Arm {
                        ctor: ok_ctor,
                        names: vec![ok_v],
                        body: ok_body,
                    },
                    Arm {
                        ctor: err_ctor,
                        names: vec![err_v],
                        body: err_body,
                    },
                ],
            })
        }
    }
}

/// Iteratively binds route parameters from innermost handler call outward (zero recursion).
fn bind_route_params(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    env: &DispatchEnv,
    route: &CheckedApiRoute,
    captured_var: &str,
    auth_var: &str,
    db_vars: &[String],
) -> LowerResult<ExprId> {
    let mut arg_vars = Vec::with_capacity(route.params.len());
    for pm in &route.params {
        let v = match pm.kind {
            ApiParamKind::Auth | ApiParamKind::AuthOptional => auth_var.to_string(),
            ApiParamKind::Request => env.req_var.clone(),
            ApiParamKind::SqliteDb => db_vars[pm.index as usize].clone(),
            ApiParamKind::Actor => env.actor_vars[pm.index as usize].clone(),
            ApiParamKind::Capability => env.cap_vars[pm.index as usize].clone(),
            ApiParamKind::Path
            | ApiParamKind::QueryRequired
            | ApiParamKind::QueryOptional
            | ApiParamKind::Body => p.fresh()?,
        };
        arg_vars.push(v);
    }

    let mut expr = invoke_route_handler(p, api, env, route, &arg_vars)?;

    for (idx, pm) in route.params.iter().enumerate().rev() {
        let val_var = arg_vars[idx].clone();
        match pm.kind {
            ApiParamKind::Auth
            | ApiParamKind::AuthOptional
            | ApiParamKind::Request
            | ApiParamKind::SqliteDb
            | ApiParamKind::Actor
            | ApiParamKind::Capability => {}
            ApiParamKind::Path => {
                let cv = p.expr_var(captured_var.to_string())?;
                let idx_e = p.expr_int(u64::from(pm.index))?;
                let opt_seg = p.expr_prim(
                    Prim::List {
                        ty: env.text_ty,
                        op: ListOp::Nullary(CoreListNullaryOp::Get),
                    },
                    vec![cv, idx_e],
                )?;
                let stop = p.expr_stop("unreachable matched path segment")?;
                let raw_seg = p.fresh()?;
                let helper = format!("parse_path_{}", scalar_suffix(p, pm.ty));
                let rsv = p.expr_var(raw_seg.clone())?;
                let pname = p.expr_text(&pm.name)?;
                let parsed = api_call(p, &helper, vec![rsv, pname])?;
                let some_body = unwrap_or_return_response(p, parsed, val_var, expr)?;
                let none_ctor = p.copy_text("None")?;
                let some_ctor = p.copy_text("Some")?;
                expr = p.alloc_expr(Expr::Case {
                    expr: opt_seg,
                    arms: vec![
                        Arm {
                            ctor: none_ctor,
                            names: Vec::new(),
                            body: stop,
                        },
                        Arm {
                            ctor: some_ctor,
                            names: vec![raw_seg],
                            body: some_body,
                        },
                    ],
                })?;
            }
            ApiParamKind::QueryRequired => {
                let helper = format!("require_query_{}", scalar_suffix(p, pm.ty));
                let rv = p.expr_var(env.req_var.clone())?;
                let pname = p.expr_text(&pm.name)?;
                let parsed = api_call(p, &helper, vec![rv, pname])?;
                expr = unwrap_or_return_response(p, parsed, val_var, expr)?;
            }
            ApiParamKind::QueryOptional => {
                let inner_ty = p.source_type(pm.ty).arguments[0];
                let helper = format!("optional_query_{}", scalar_suffix(p, inner_ty));
                let rv = p.expr_var(env.req_var.clone())?;
                let pname = p.expr_text(&pm.name)?;
                let parsed = api_call(p, &helper, vec![rv, pname])?;
                expr = unwrap_or_return_response(p, parsed, val_var, expr)?;
            }
            ApiParamKind::Body => {
                let Some(bjson) = pm.json else {
                    return p.refuse(ErrorCode::LowerUnfinished, "body param missing Json proof");
                };
                let rbv = p.expr_var(env.req_body.clone())?;
                let decoded =
                    lower_json_decode_call(p, bjson, None, api.module, 0, env.bytes_ty, rbv, true)?;
                let err_var = p.fresh()?;
                let ev = p.expr_var(err_var.clone())?;
                let err_body = api_call(p, "body_decode_error", vec![ev])?;
                let ok_ctor = p.copy_text("Ok")?;
                let err_ctor = p.copy_text("Error")?;
                expr = p.alloc_expr(Expr::Case {
                    expr: decoded,
                    arms: vec![
                        Arm {
                            ctor: ok_ctor,
                            names: vec![val_var],
                            body: expr,
                        },
                        Arm {
                            ctor: err_ctor,
                            names: vec![err_var],
                            body: err_body,
                        },
                    ],
                })?;
            }
        }
    }
    Ok(expr)
}

fn execute_matched_route(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    env: &DispatchEnv,
    route: &CheckedApiRoute,
    captured_var: &str,
) -> LowerResult<ExprId> {
    let mut need_db = vec![false; api.sqlites.len()];
    if route.auth_mode != ApiAuthMode::None
        && let Some(ref auth) = api.auth
    {
        for pm in &auth.params {
            if pm.kind == ApiParamKind::SqliteDb {
                need_db[pm.index as usize] = true;
            }
        }
    }
    for pm in &route.params {
        if pm.kind == ApiParamKind::SqliteDb {
            need_db[pm.index as usize] = true;
        }
    }

    let mut db_vars = vec![String::new(); api.sqlites.len()];
    for s in 0..api.sqlites.len() {
        if need_db[s] {
            db_vars[s] = p.fresh()?;
        }
    }
    let auth_var = if route.auth_mode != ApiAuthMode::None {
        p.fresh()?
    } else {
        String::new()
    };

    let mut inner = bind_route_params(p, api, env, route, captured_var, &auth_var, &db_vars)?;

    if route.auth_mode != ApiAuthMode::None
        && let Some(ref auth) = api.auth
    {
        let afn_sym = p.program.function(auth.function).symbol;
        let afn_inst = lower_instance(p, afn_sym, &[], None, &[], afn_sym.module, 0)?;
        let afname = p.instances[afn_inst].fun.name.clone();
        let mut aargs = Vec::with_capacity(auth.params.len());
        for pm in &auth.params {
            let vname = match pm.kind {
                ApiParamKind::Request => env.req_var.clone(),
                ApiParamKind::SqliteDb => db_vars[pm.index as usize].clone(),
                ApiParamKind::Actor => env.actor_vars[pm.index as usize].clone(),
                _ => env.cap_vars[pm.index as usize].clone(),
            };
            aargs.push(p.expr_var(vname)?);
        }
        let acall = p.expr_call(afname, aargs)?;
        let arms = if route.auth_mode == ApiAuthMode::Required {
            let aerr = p.fresh()?;
            let aev = p.expr_var(aerr.clone())?;
            let err_body = encode_domain_error(p, api, env, auth.error_type, auth.error_json, aev)?;
            vec![
                Arm {
                    ctor: p.copy_text("Ok")?,
                    names: vec![auth_var],
                    body: inner,
                },
                Arm {
                    ctor: p.copy_text("Error")?,
                    names: vec![aerr],
                    body: err_body,
                },
            ]
        } else {
            let opt_prin_src = p
                .types_overlay
                .builtin_applied("Option", vec![auth.principal]);
            let opt_prin_ty = lower_type(p, opt_prin_src, None)?;
            let ok_m = p.fresh()?;
            let aerr = p.fresh()?;
            let inner_err =
                bind_route_params(p, api, env, route, captured_var, &auth_var, &db_vars)?;
            let ok_mv = p.expr_var(ok_m.clone())?;
            let some_ctor = p.copy_text("Some")?;
            let some_p = p.expr_con(opt_prin_ty, some_ctor, vec![ok_mv])?;
            let ok_body = p.expr_let(auth_var.clone(), some_p, inner)?;
            let none_ctor = p.copy_text("None")?;
            let none_p = p.expr_con(opt_prin_ty, none_ctor, Vec::new())?;
            let err_body = p.expr_let(auth_var, none_p, inner_err)?;
            vec![
                Arm {
                    ctor: p.copy_text("Ok")?,
                    names: vec![ok_m],
                    body: ok_body,
                },
                Arm {
                    ctor: p.copy_text("Error")?,
                    names: vec![aerr],
                    body: err_body,
                },
            ]
        };
        inner = p.alloc_expr(Expr::Case { expr: acall, arms })?;
    }

    for slot in (0..api.sqlites.len()).rev() {
        if !need_db[slot] {
            continue;
        }
        let cap0 = p.expr_var(env.cap_vars[0].clone())?;
        let path_e = p.expr_text(&api.sqlites[slot].path)?;
        let opened = p.expr_prim(
            Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteOpen)),
            vec![cap0, path_e],
        )?;
        let derr = p.fresh()?;
        let dev = p.expr_var(derr.clone())?;
        let err_resp = api_call(p, "sqlite_open_error_response", vec![dev])?;
        let ok_ctor = p.copy_text("Ok")?;
        let err_ctor = p.copy_text("Error")?;
        inner = p.alloc_expr(Expr::Case {
            expr: opened,
            arms: vec![
                Arm {
                    ctor: ok_ctor,
                    names: vec![db_vars[slot].clone()],
                    body: inner,
                },
                Arm {
                    ctor: err_ctor,
                    names: vec![derr],
                    body: err_resp,
                },
            ],
        })?;
    }

    Ok(inner)
}

fn text_eq(p: &mut Lowering<'_>, text_ty: TyId, a: ExprId, b: ExprId) -> LowerResult<ExprId> {
    p.expr_prim(Prim::Eq(text_ty), vec![a, b])
}

fn builtin_openapi_fallback(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    env: &DispatchEnv,
    path_matched_var: &str,
) -> LowerResult<ExprId> {
    let pmv = p.expr_var(path_matched_var.to_string())?;
    let m405 = api_call(p, "method_not_allowed_response", Vec::new())?;
    let m404 = api_call(p, "not_found_response", Vec::new())?;
    let not_found_or_405 = p.expr_if(pmv, m405, m404)?;

    // GET /docs
    let rm1 = p.expr_var(env.req_method.clone())?;
    let get1 = p.expr_text("GET")?;
    let docs_cond = text_eq(p, env.text_ty, rm1, get1)?;
    let mod_name = p.module_name(api.module).to_string();
    let mod_e = p.expr_text(&mod_name)?;
    let docs_html = api_call(p, "openapi_docs_html", vec![mod_e])?;
    let docs_405 = api_call(p, "method_not_allowed_response", Vec::new())?;
    let docs_if = p.expr_if(docs_cond, docs_html, docs_405)?;

    let docs_pat = p.expr_text("/docs")?;
    let rp1 = p.expr_var(env.req_path.clone())?;
    let docs_match = api_call(p, "match_path", vec![docs_pat, rp1])?;
    let docs_ign = p.fresh()?;
    let none_ctor = p.copy_text("None")?;
    let some_ctor = p.copy_text("Some")?;
    let docs_case = p.alloc_expr(Expr::Case {
        expr: docs_match,
        arms: vec![
            Arm {
                ctor: none_ctor,
                names: Vec::new(),
                body: not_found_or_405,
            },
            Arm {
                ctor: some_ctor,
                names: vec![docs_ign],
                body: docs_if,
            },
        ],
    })?;

    // GET /openapi.json
    let call_oa = p.expr_call(env.openapi_fn_name.clone(), Vec::new())?;
    let oa_bytes = p.expr_prim(Prim::Text(CoreTextOp::Bytes), vec![call_oa])?;
    let text_src = p.types_overlay.builtin("Text");
    let pair_src = p.types_overlay.alloc(SourceType {
        kind: SourceTypeKind::Tuple,
        name: "()".to_string(),
        module: None,
        owner: TypeOwner::None,
        arguments: vec![text_src, text_src],
        height: 1,
        parameters: 0,
    });
    let hdr_pair_ty = lower_type(p, pair_src, None)?;
    let ct_k = p.expr_text("Content-Type")?;
    let ct_v = p.expr_text("application/json")?;
    let hdr_tup = p.expr_tuple(vec![ct_k, ct_v])?;
    let hdrs = p.expr_lit_list(hdr_pair_ty, vec![hdr_tup])?;
    let code200 = p.expr_int(200)?;
    let oa_res = api_call(p, "response", vec![code200, hdrs, oa_bytes])?;

    let rm2 = p.expr_var(env.req_method.clone())?;
    let get2 = p.expr_text("GET")?;
    let oa_cond = text_eq(p, env.text_ty, rm2, get2)?;
    let oa_405 = api_call(p, "method_not_allowed_response", Vec::new())?;
    let oa_if = p.expr_if(oa_cond, oa_res, oa_405)?;

    let oa_pat = p.expr_text("/openapi.json")?;
    let rp2 = p.expr_var(env.req_path.clone())?;
    let oa_match = api_call(p, "match_path", vec![oa_pat, rp2])?;
    let oa_ign = p.fresh()?;
    let oa_none = p.copy_text("None")?;
    let oa_some = p.copy_text("Some")?;
    p.alloc_expr(Expr::Case {
        expr: oa_match,
        arms: vec![
            Arm {
                ctor: oa_none,
                names: Vec::new(),
                body: docs_case,
            },
            Arm {
                ctor: oa_some,
                names: vec![oa_ign],
                body: oa_if,
            },
        ],
    })
}

fn emit_single_route_fn(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    base_env: &DispatchEnv,
    route_idx: usize,
) -> LowerResult<String> {
    let route = &api.routes[route_idx];
    p.span = p.syntax_node(api.module, route.syntax).span;
    let text_src = p.types_overlay.builtin("Text");
    let list_text_src = p.types_overlay.builtin_applied("List", vec![text_src]);
    let list_text_ty = lower_type(p, list_text_src, None)?;

    let req_p = p.fresh()?;
    let body_p = p.fresh()?;
    let cap_p = p.fresh()?;
    let mut params = vec![
        p.make_param(req_p.clone(), base_env.req_ty)?,
        p.make_param(body_p.clone(), base_env.bytes_ty)?,
        p.make_param(cap_p.clone(), list_text_ty)?,
    ];
    let mut rcaps = Vec::with_capacity(api.capabilities.len());
    for cname in &api.capabilities {
        let cv = p.fresh()?;
        let cty = builtin_ty(p, cname)?;
        params.push(p.make_param(cv.clone(), cty)?);
        rcaps.push(cv);
    }
    let sim_ty = builtin_ty(p, "Simulation")?;
    let mut ractors = Vec::with_capacity(api.actors.len());
    for _ in 0..api.actors.len() {
        let av = p.fresh()?;
        params.push(p.make_param(av.clone(), sim_ty)?);
        ractors.push(av);
    }

    let suffix = format!(".%api.route.{route_idx}");
    let fn_idx = new_generated_fn(p, api.module, &suffix, params, base_env.res_ty)?;

    let mut renv = base_env.clone();
    renv.req_var = req_p;
    renv.req_body = body_p;
    renv.cap_vars = rcaps;
    renv.actor_vars = ractors;
    let body = execute_matched_route(p, api, &renv, route, &cap_p)?;
    p.instances[fn_idx].fun.body = body;
    p.instances[fn_idx].body_set = true;
    Ok(p.instances[fn_idx].fun.name.clone())
}

fn make_jump1(p: &mut Lowering<'_>, label: String, arg: ExprId) -> LowerResult<ExprId> {
    p.alloc_expr(Expr::Jump {
        name: label,
        args: vec![arg],
    })
}

/// Iteratively constructs the route-matching join chain (zero recursion).
fn build_route_chain(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    env: &DispatchEnv,
    route_fn_names: &[String],
    init_matched: &str,
) -> LowerResult<ExprId> {
    let count = api.routes.len();
    let mut matched_vars = Vec::with_capacity(count + 1);
    matched_vars.push(init_matched.to_string());
    let mut join_labels = Vec::with_capacity(count);
    for _ in 0..count {
        join_labels.push(p.fresh()?);
        matched_vars.push(p.fresh()?);
    }

    let mut chain = builtin_openapi_fallback(p, api, env, &matched_vars[count])?;
    let bool_ty = builtin_ty(p, "Bool")?;

    for idx in (0..count).rev() {
        let route = &api.routes[idx];
        p.span = p.syntax_node(api.module, route.syntax).span;
        let join_label = join_labels[idx].clone();
        let joined_matched = matched_vars[idx + 1].clone();
        let prev_matched = &matched_vars[idx];

        let pat_e = p.expr_text(&route.path)?;
        let rp = p.expr_var(env.req_path.clone())?;
        let match_call = api_call(p, "match_path", vec![pat_e, rp])?;
        let captured = p.fresh()?;

        let mut call_args = Vec::with_capacity(3 + api.capabilities.len() + api.actors.len());
        call_args.push(p.expr_var(env.req_var.clone())?);
        call_args.push(p.expr_var(env.req_body.clone())?);
        call_args.push(p.expr_var(captured.clone())?);
        for cv in &env.cap_vars {
            call_args.push(p.expr_var(cv.clone())?);
        }
        for av in &env.actor_vars {
            call_args.push(p.expr_var(av.clone())?);
        }
        let rcall = p.expr_call(route_fn_names[idx].clone(), call_args)?;

        let rm = p.expr_var(env.req_method.clone())?;
        let mstr = p.expr_text(method_upper(route.method))?;
        let mcond = text_eq(p, env.text_ty, rm, mstr)?;
        let t_lit = p.expr_bool(true)?;
        let m_else = make_jump1(p, join_label.clone(), t_lit)?;
        let method_if = p.expr_if(mcond, rcall, m_else)?;

        let pmv = p.expr_var(prev_matched.clone())?;
        let none_j = make_jump1(p, join_label.clone(), pmv)?;
        let none_ctor = p.copy_text("None")?;
        let some_ctor = p.copy_text("Some")?;
        let mcase = p.alloc_expr(Expr::Case {
            expr: match_call,
            arms: vec![
                Arm {
                    ctor: none_ctor,
                    names: Vec::new(),
                    body: none_j,
                },
                Arm {
                    ctor: some_ctor,
                    names: vec![captured],
                    body: method_if,
                },
            ],
        })?;

        let jp0 = p.make_param(joined_matched, bool_ty)?;
        chain = p.alloc_expr(Expr::Join {
            name: join_label,
            params: vec![jp0],
            body: chain,
            rest: mcase,
        })?;
    }

    Ok(chain)
}

fn emit_wire_fn(p: &mut Lowering<'_>, api: &CheckedApi, disp_fn_name: &str) -> LowerResult<()> {
    let req_src = lib_data_source(p, "Api", "Request")?;
    let req_ty = lower_type(p, req_src, None)?;
    let text_ty = builtin_ty(p, "Text")?;
    let bytes_ty = builtin_ty(p, "Bytes")?;

    let text_src = p.types_overlay.builtin("Text");
    let bytes_src = p.types_overlay.builtin("Bytes");
    let int_src = p.types_overlay.builtin("Int");
    let pair_src = p.types_overlay.alloc(SourceType {
        kind: SourceTypeKind::Tuple,
        name: "()".to_string(),
        module: None,
        owner: TypeOwner::None,
        arguments: vec![text_src, text_src],
        height: 1,
        parameters: 0,
    });
    let hdrs_src = p.types_overlay.builtin_applied("List", vec![pair_src]);
    let ret_src = p.types_overlay.alloc(SourceType {
        kind: SourceTypeKind::Tuple,
        name: "()".to_string(),
        module: None,
        owner: TypeOwner::None,
        arguments: vec![int_src, hdrs_src, bytes_src],
        height: 3,
        parameters: 0,
    });
    let hdrs_ty = lower_type(p, hdrs_src, None)?;
    let ret_ty = lower_type(p, ret_src, None)?;

    let p_method = p.fresh()?;
    let p_target = p.fresh()?;
    let p_headers = p.fresh()?;
    let p_body = p.fresh()?;
    let mut params = vec![
        p.make_param(p_method.clone(), text_ty)?,
        p.make_param(p_target.clone(), text_ty)?,
        p.make_param(p_headers.clone(), hdrs_ty)?,
        p.make_param(p_body.clone(), bytes_ty)?,
    ];
    let mut cap_names = Vec::with_capacity(api.capabilities.len());
    for cname in &api.capabilities {
        let v = p.fresh()?;
        let ty = builtin_ty(p, cname)?;
        params.push(p.make_param(v.clone(), ty)?);
        cap_names.push(v);
    }
    let sim_ty = builtin_ty(p, "Simulation")?;
    let mut actor_names = Vec::with_capacity(api.actors.len());
    for _ in 0..api.actors.len() {
        let v = p.fresh()?;
        params.push(p.make_param(v.clone(), sim_ty)?);
        actor_names.push(v);
    }

    let wire_idx = new_generated_fn(p, api.module, ".%api.wire", params, ret_ty)?;

    let pm_e = p.expr_var(p_method)?;
    let pt_e = p.expr_var(p_target)?;
    let base_req = api_call(p, "request", vec![pm_e, pt_e])?;
    let rm = p.fresh()?;
    let rp = p.fresh()?;
    let rq = p.fresh()?;
    let ign1 = p.fresh()?;
    let ign2 = p.fresh()?;

    let req_ctor = p.copy_text("Request")?;
    let f0 = p.expr_var(rm.clone())?;
    let f1 = p.expr_var(rp.clone())?;
    let f2 = p.expr_var(rq.clone())?;
    let f3 = p.expr_var(p_headers)?;
    let f4 = p.expr_var(p_body)?;
    let full_req = p.expr_con(req_ty, req_ctor, vec![f0, f1, f2, f3, f4])?;

    let req_v = p.fresh()?;
    let mut dargs = Vec::with_capacity(1 + cap_names.len() + actor_names.len());
    dargs.push(p.expr_var(req_v.clone())?);
    for cn in &cap_names {
        dargs.push(p.expr_var(cn.clone())?);
    }
    for an in &actor_names {
        dargs.push(p.expr_var(an.clone())?);
    }
    let call_disp = p.expr_call(disp_fn_name.to_string(), dargs)?;

    let st_v = p.fresh()?;
    let rh_v = p.fresh()?;
    let rb_v = p.fresh()?;
    let t0 = p.expr_var(st_v.clone())?;
    let t1 = p.expr_var(rh_v.clone())?;
    let t2 = p.expr_var(rb_v.clone())?;
    let tup = p.expr_tuple(vec![t0, t1, t2])?;
    let res_ctor = p.copy_text("Response")?;
    let unpack_res = p.alloc_expr(Expr::Case {
        expr: call_disp,
        arms: vec![Arm {
            ctor: res_ctor,
            names: vec![st_v, rh_v, rb_v],
            body: tup,
        }],
    })?;
    let req_let = p.expr_let(req_v, full_req, unpack_res)?;
    let req_ctor2 = p.copy_text("Request")?;
    let unpack_req = p.alloc_expr(Expr::Case {
        expr: base_req,
        arms: vec![Arm {
            ctor: req_ctor2,
            names: vec![rm, rp, rq, ign1, ign2],
            body: req_let,
        }],
    })?;

    p.instances[wire_idx].fun.body = unpack_req;
    p.instances[wire_idx].body_set = true;
    Ok(())
}

fn emit_dispatch_fns(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    openapi_fn_idx: usize,
) -> LowerResult<()> {
    let req_src = lib_data_source(p, "Api", "Request")?;
    let res_src = lib_data_source(p, "Api", "Response")?;
    let req_ty = lower_type(p, req_src, None)?;
    let res_ty = lower_type(p, res_src, None)?;
    let text_ty = builtin_ty(p, "Text")?;
    let bytes_ty = builtin_ty(p, "Bytes")?;
    let openapi_fn_name = p.instances[openapi_fn_idx].fun.name.clone();

    let req_p = p.fresh()?;
    let mut route_params = vec![p.make_param(req_p.clone(), req_ty)?];
    let mut cap_vars = Vec::with_capacity(api.capabilities.len());
    for cname in &api.capabilities {
        let cv = p.fresh()?;
        let cty = builtin_ty(p, cname)?;
        route_params.push(p.make_param(cv.clone(), cty)?);
        cap_vars.push(cv);
    }
    let sim_ty = builtin_ty(p, "Simulation")?;
    let mut actor_vars = Vec::with_capacity(api.actors.len());
    for _ in 0..api.actors.len() {
        let av = p.fresh()?;
        route_params.push(p.make_param(av.clone(), sim_ty)?);
        actor_vars.push(av);
    }
    let param_tys: Vec<TyId> = route_params.iter().map(|pm| pm.ty).collect();
    let route_fn_idx = new_generated_fn(p, api.module, ".%api.route", route_params, res_ty)?;

    let env = DispatchEnv {
        openapi_fn_name,
        req_ty,
        res_ty,
        text_ty,
        bytes_ty,
        req_var: req_p,
        req_method: p.fresh()?,
        req_path: p.fresh()?,
        req_body: p.fresh()?,
        cap_vars,
        actor_vars,
    };

    let mut route_fn_names = Vec::with_capacity(api.routes.len());
    for r in 0..api.routes.len() {
        route_fn_names.push(emit_single_route_fn(p, api, &env, r)?);
    }

    let init_matched = p.fresh()?;
    let chain = build_route_chain(p, api, &env, &route_fn_names, &init_matched)?;
    let f_lit = p.expr_bool(false)?;
    let chain_let = p.expr_let(init_matched, f_lit, chain)?;
    let req_v = p.expr_var(env.req_var.clone())?;
    let ign2 = p.fresh()?;
    let ign3 = p.fresh()?;
    let req_ctor = p.copy_text("Request")?;
    let unpack = p.alloc_expr(Expr::Case {
        expr: req_v,
        arms: vec![Arm {
            ctor: req_ctor,
            names: vec![env.req_method, env.req_path, ign2, ign3, env.req_body],
            body: chain_let,
        }],
    })?;
    p.instances[route_fn_idx].fun.body = unpack;
    p.instances[route_fn_idx].body_set = true;

    let mut disp_params = Vec::with_capacity(param_tys.len());
    for &pty in &param_tys {
        let pn = p.fresh()?;
        disp_params.push(p.make_param(pn, pty)?);
    }
    let disp_param_names: Vec<String> = disp_params.iter().map(|pm| pm.name.clone()).collect();
    let disp_fn_idx = new_generated_fn(p, api.module, ".%api.dispatch", disp_params, res_ty)?;
    let route_fn_name = p.instances[route_fn_idx].fun.name.clone();

    let mut rcall_args = Vec::with_capacity(disp_param_names.len());
    for dn in &disp_param_names {
        rcall_args.push(p.expr_var(dn.clone())?);
    }
    let call_route = p.expr_call(route_fn_name, rcall_args)?;

    let disp_body = if let Some(ref cors) = api.cors {
        let dm = p.fresh()?;
        let dmv = p.expr_var(dm.clone())?;
        let opts = p.expr_text("OPTIONS")?;
        let cond = text_eq(p, text_ty, dmv, opts)?;
        let c1 = p.expr_text(cors)?;
        let pre = api_call(p, "cors_preflight", vec![c1])?;
        let c2 = p.expr_text(cors)?;
        let app = api_call(p, "apply_cors", vec![call_route, c2])?;
        let cors_if = p.expr_if(cond, pre, app)?;
        let req_v2 = p.expr_var(disp_param_names[0].clone())?;
        let mut names = vec![dm];
        for _ in 1..5 {
            names.push(p.fresh()?);
        }
        let req_ctor = p.copy_text("Request")?;
        p.alloc_expr(Expr::Case {
            expr: req_v2,
            arms: vec![Arm {
                ctor: req_ctor,
                names,
                body: cors_if,
            }],
        })?
    } else {
        call_route
    };
    p.instances[disp_fn_idx].fun.body = disp_body;
    p.instances[disp_fn_idx].body_set = true;
    let disp_fn_name = p.instances[disp_fn_idx].fun.name.clone();

    if !api.statics.is_empty() {
        let rf_ty = builtin_ty(p, "ReadFiles")?;
        let mut tparams = Vec::with_capacity(param_tys.len() + 1);
        let tp0 = p.fresh()?;
        let tp1 = p.fresh()?;
        tparams.push(p.make_param(tp0, param_tys[0])?);
        tparams.push(p.make_param(tp1, rf_ty)?);
        for &pty in param_tys.iter().skip(1) {
            let pn = p.fresh()?;
            tparams.push(p.make_param(pn, pty)?);
        }
        let tp_names: Vec<String> = tparams.iter().map(|pm| pm.name.clone()).collect();
        let test_fn_idx = new_generated_fn(p, api.module, ".%api.test_dispatch", tparams, res_ty)?;

        let mut fb_args = Vec::with_capacity(param_tys.len());
        fb_args.push(p.expr_var(tp_names[0].clone())?);
        for tn in tp_names.iter().skip(2) {
            fb_args.push(p.expr_var(tn.clone())?);
        }
        let mut static_chain = p.expr_call(disp_fn_name.clone(), fb_args)?;
        for st in api.statics.iter().rev() {
            let req_e = p.expr_var(tp_names[0].clone())?;
            let pref_e = p.expr_text(&st.prefix)?;
            let dir_e = p.expr_text(&st.dir)?;
            let rf_e = p.expr_var(tp_names[1].clone())?;
            let st_call = api_call(p, "serve_static_fake", vec![req_e, pref_e, dir_e, rf_e])?;
            let sres = p.fresh()?;
            let sres_v = p.expr_var(sres.clone())?;
            let some_body = if let Some(ref cors) = api.cors {
                let ce = p.expr_text(cors)?;
                api_call(p, "apply_cors", vec![sres_v, ce])?
            } else {
                sres_v
            };
            let none_ctor = p.copy_text("None")?;
            let some_ctor = p.copy_text("Some")?;
            static_chain = p.alloc_expr(Expr::Case {
                expr: st_call,
                arms: vec![
                    Arm {
                        ctor: none_ctor,
                        names: Vec::new(),
                        body: static_chain,
                    },
                    Arm {
                        ctor: some_ctor,
                        names: vec![sres],
                        body: some_body,
                    },
                ],
            })?;
        }

        let test_body = if let Some(ref cors) = api.cors {
            let tm = p.fresh()?;
            let tmv = p.expr_var(tm.clone())?;
            let opts = p.expr_text("OPTIONS")?;
            let cond = text_eq(p, text_ty, tmv, opts)?;
            let ce = p.expr_text(cors)?;
            let pre = api_call(p, "cors_preflight", vec![ce])?;
            let tcors_if = p.expr_if(cond, pre, static_chain)?;
            let req_e = p.expr_var(tp_names[0].clone())?;
            let mut names = vec![tm];
            for _ in 1..5 {
                names.push(p.fresh()?);
            }
            let req_ctor = p.copy_text("Request")?;
            p.alloc_expr(Expr::Case {
                expr: req_e,
                arms: vec![Arm {
                    ctor: req_ctor,
                    names,
                    body: tcors_if,
                }],
            })?
        } else {
            static_chain
        };
        p.instances[test_fn_idx].fun.body = test_body;
        p.instances[test_fn_idx].body_set = true;
    }

    emit_wire_fn(p, api, &disp_fn_name)
}

fn execute_matched_job(
    p: &mut Lowering<'_>,
    api: &CheckedApi,
    job: &CheckedApiJob,
    job_ret_ty: TyId,
    payload_json_var: &str,
    cap_vars: &[String],
    actor_vars: &[String],
) -> LowerResult<ExprId> {
    p.span = p.syntax_node(api.module, job.syntax).span;
    let mut need_db = vec![false; api.sqlites.len()];
    for pm in job.params.iter().skip(1) {
        if pm.kind == ApiParamKind::SqliteDb {
            need_db[pm.index as usize] = true;
        }
    }
    let mut db_vars = vec![String::new(); api.sqlites.len()];
    for s in 0..api.sqlites.len() {
        if need_db[s] {
            db_vars[s] = p.fresh()?;
        }
    }
    let payload_var = p.fresh()?;
    let fn_sym = p.program.function(job.function).symbol;
    let fn_inst = lower_instance(p, fn_sym, &[], None, &[], fn_sym.module, 0)?;
    let fname = p.instances[fn_inst].fun.name.clone();
    let mut cargs = Vec::with_capacity(job.params.len());
    cargs.push(p.expr_var(payload_var.clone())?);
    for pm in job.params.iter().skip(1) {
        let vn = match pm.kind {
            ApiParamKind::SqliteDb => db_vars[pm.index as usize].clone(),
            ApiParamKind::Actor => actor_vars[pm.index as usize].clone(),
            _ => cap_vars[pm.index as usize].clone(),
        };
        cargs.push(p.expr_var(vn)?);
    }
    let call = p.expr_call(fname, cargs)?;

    let ok_ctor = p.copy_text("Ok")?;
    let err_ctor = p.copy_text("Error")?;
    let mut inner = match job.reply {
        ApiJobReplyKind::Unit => {
            let tmp = p.fresh()?;
            let u = p.expr_unit()?;
            let ok = p.expr_con(job_ret_ty, ok_ctor.clone(), vec![u])?;
            p.expr_let(tmp, call, ok)?
        }
        ApiJobReplyKind::ResultUnit => {
            let uvar = p.fresh()?;
            let evar = p.fresh()?;
            let uv = p.expr_var(uvar.clone())?;
            let ok_b = p.expr_con(job_ret_ty, ok_ctor.clone(), vec![uv])?;
            let Some(et) = job.error_text else {
                return p.refuse(ErrorCode::LowerUnfinished, "job missing error_text proof");
            };
            let ev = p.expr_var(evar.clone())?;
            let err_txt = lower_to_text(p, et, None, api.module, 0, ev)?;
            let err_b = p.expr_con(job_ret_ty, err_ctor.clone(), vec![err_txt])?;
            p.alloc_expr(Expr::Case {
                expr: call,
                arms: vec![
                    Arm {
                        ctor: ok_ctor.clone(),
                        names: vec![uvar],
                        body: ok_b,
                    },
                    Arm {
                        ctor: err_ctor.clone(),
                        names: vec![evar],
                        body: err_b,
                    },
                ],
            })?
        }
    };

    for slot in (0..api.sqlites.len()).rev() {
        if !need_db[slot] {
            continue;
        }
        let cap0 = p.expr_var(cap_vars[0].clone())?;
        let path_e = p.expr_text(&api.sqlites[slot].path)?;
        let opened = p.expr_prim(
            Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteOpen)),
            vec![cap0, path_e],
        )?;
        let derr = p.fresh()?;
        let dev = p.expr_var(derr.clone())?;
        let err_b = api_call(p, "sqlite_job_error", vec![dev])?;
        inner = p.alloc_expr(Expr::Case {
            expr: opened,
            arms: vec![
                Arm {
                    ctor: ok_ctor.clone(),
                    names: vec![db_vars[slot].clone()],
                    body: inner,
                },
                Arm {
                    ctor: err_ctor.clone(),
                    names: vec![derr],
                    body: err_b,
                },
            ],
        })?;
    }

    let pjv = p.expr_var(payload_json_var.to_string())?;
    let parsed = api_call(p, "parse_job_payload", vec![pjv])?;
    let perr = p.fresh()?;
    let pev = p.expr_var(perr.clone())?;
    let perr_b = p.expr_con(job_ret_ty, err_ctor.clone(), vec![pev])?;
    p.alloc_expr(Expr::Case {
        expr: parsed,
        arms: vec![
            Arm {
                ctor: ok_ctor,
                names: vec![payload_var],
                body: inner,
            },
            Arm {
                ctor: err_ctor,
                names: vec![perr],
                body: perr_b,
            },
        ],
    })
}

fn emit_job_fn(p: &mut Lowering<'_>, api: &CheckedApi) -> LowerResult<()> {
    let unit_src = p.types_overlay.builtin("Unit");
    let text_src = p.types_overlay.builtin("Text");
    let res_src = p
        .types_overlay
        .builtin_applied("Result", vec![unit_src, text_src]);
    let text_ty = lower_type(p, text_src, None)?;
    let job_ret_ty = lower_type(p, res_src, None)?;

    let p_name = p.fresh()?;
    let p_payload = p.fresh()?;
    let mut params = vec![
        p.make_param(p_name.clone(), text_ty)?,
        p.make_param(p_payload.clone(), text_ty)?,
    ];
    let mut cap_vars = Vec::with_capacity(api.capabilities.len());
    for cname in &api.capabilities {
        let v = p.fresh()?;
        let ty = builtin_ty(p, cname)?;
        params.push(p.make_param(v.clone(), ty)?);
        cap_vars.push(v);
    }
    let sim_ty = builtin_ty(p, "Simulation")?;
    let mut actor_vars = Vec::with_capacity(api.actors.len());
    for _ in 0..api.actors.len() {
        let v = p.fresh()?;
        params.push(p.make_param(v.clone(), sim_ty)?);
        actor_vars.push(v);
    }

    let job_idx = new_generated_fn(p, api.module, ".%api.job", params, job_ret_ty)?;
    let pn0 = p.expr_var(p_name.clone())?;
    let mut chain = api_call(p, "unknown_job_error", vec![pn0])?;
    for job in api.jobs.iter().rev() {
        let pnv = p.expr_var(p_name.clone())?;
        let jname = p.expr_text(&job.name)?;
        let cond = text_eq(p, text_ty, pnv, jname)?;
        let then_ =
            execute_matched_job(p, api, job, job_ret_ty, &p_payload, &cap_vars, &actor_vars)?;
        chain = p.expr_if(cond, then_, chain)?;
    }
    p.instances[job_idx].fun.body = chain;
    p.instances[job_idx].body_set = true;
    Ok(())
}

/// Lowers every HTTP `api` block in the checked program.
pub(crate) fn lower_apis(p: &mut Lowering<'_>) -> LowerResult<()> {
    let apis = p.program.apis.clone();
    for api in &apis {
        p.span = p.syntax_node(api.module, api.syntax).span;
        let oa = emit_openapi_fn(p, api)?;
        emit_dispatch_fns(p, api, oa)?;
        emit_job_fn(p, api)?;
    }
    Ok(())
}

fn default_fake_cap(p: &mut Lowering<'_>, cap_name: &str) -> LowerResult<ExprId> {
    let text_ty = builtin_ty(p, "Text")?;
    match cap_name {
        "Sqlite" => p.expr_prim(
            Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteFake)),
            Vec::new(),
        ),
        "Console" => p.expr_prim(
            Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ConsoleFake)),
            Vec::new(),
        ),
        "Random" => {
            let z = p.expr_int(0)?;
            p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::RandomSeeded)),
                vec![z],
            )
        }
        "SecureRandom" => {
            let z = p.expr_int(0)?;
            p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SecureRandomSeeded)),
                vec![z],
            )
        }
        "Clock" => {
            p.charge_alloc(1, 40, 8)?;
            let digits = p.copy_text("1767225600000000")?;
            let tlit = p.alloc_expr(Expr::Lit(Lit::Time(CoreInteger {
                digits,
                negative: false,
            })))?;
            p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ClockFixed)),
                vec![tlit],
            )
        }
        "Env" => {
            let emap = p.expr_lit_map(text_ty, text_ty, Vec::new())?;
            p.expr_prim(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::EnvOf)), vec![emap])
        }
        "Files" | "ReadFiles" => {
            let emap = p.expr_lit_map(text_ty, text_ty, Vec::new())?;
            let files = p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FilesInMemory)),
                vec![emap],
            )?;
            if cap_name == "Files" {
                Ok(files)
            } else {
                p.expr_prim(
                    Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadOnly)),
                    vec![files],
                )
            }
        }
        "Command" => {
            let bytes_ty = builtin_ty(p, "Bytes")?;
            let emap = p.expr_lit_map(bytes_ty, bytes_ty, Vec::new())?;
            p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::CommandFake)),
                vec![emap],
            )
        }
        _ => {
            let bytes_src = p.types_overlay.builtin("Bytes");
            let inner_src = p
                .types_overlay
                .builtin_applied("Map", vec![bytes_src, bytes_src]);
            let inner_ty = lower_type(p, inner_src, None)?;
            let enet = p.expr_lit_map(text_ty, inner_ty, Vec::new())?;
            p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::NetworkFake)),
                vec![enet],
            )
        }
    }
}

fn default_fake_actor(p: &mut Lowering<'_>, service_index: u32) -> LowerResult<ExprId> {
    p.charge_alloc(1, 40, 8)?;
    let digits = p.copy_text("1767225600000000")?;
    let tlit = p.alloc_expr(Expr::Lit(Lit::Time(CoreInteger {
        digits,
        negative: false,
    })))?;
    let desc = p.number_text(u64::from(service_index))?;
    p.expr_prim(
        Prim::Sim(SimOp::Start {
            descriptor: CoreNatural { digits: desc },
        }),
        vec![tlit],
    )
}

pub(crate) fn lower_apitest_send(
    p: &mut Lowering<'_>,
    api_index: usize,
    args: Vec<ExprId>,
    arg_types: &[TypeId],
) -> LowerResult<ExprId> {
    let api = p.program.apis[api_index].clone();
    let supplied = args.len() - 1;
    let mut sup_vars = Vec::with_capacity(supplied);
    for _ in 0..supplied {
        sup_vars.push(p.fresh()?);
    }
    let req_var = p.fresh()?;
    let has_static = !api.statics.is_empty();
    let mod_name = p.module_name(api.module).to_string();
    let suffix = if has_static {
        ".%api.test_dispatch"
    } else {
        ".%api.dispatch"
    };
    let callee = p.concat_text(&mod_name, suffix, "")?;

    let mut call_args =
        Vec::with_capacity(1 + usize::from(has_static) + api.capabilities.len() + api.actors.len());
    call_args.push(p.expr_var(req_var.clone())?);

    if has_static {
        let mut st_files = None;
        for i in 0..supplied {
            let st = p.source_type(arg_types[1 + i]);
            if st.owner == TypeOwner::None && st.name == "ReadFiles" {
                st_files = Some(p.expr_var(sup_vars[i].clone())?);
                break;
            }
        }
        if st_files.is_none() {
            for i in 0..supplied {
                let st = p.source_type(arg_types[1 + i]);
                if st.owner == TypeOwner::None && st.name == "Files" {
                    let fv = p.expr_var(sup_vars[i].clone())?;
                    st_files = Some(
                        p.expr_prim(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadOnly)), vec![fv])?,
                    );
                    break;
                }
            }
        }
        let chosen = match st_files {
            Some(e) => e,
            None => default_fake_cap(p, "ReadFiles")?,
        };
        call_args.push(chosen);
    }

    for wanted in &api.capabilities {
        let mut chosen = None;
        for i in 0..supplied {
            let st = p.source_type(arg_types[1 + i]);
            if st.owner == TypeOwner::None && st.name == *wanted {
                chosen = Some(p.expr_var(sup_vars[i].clone())?);
                break;
            }
        }
        if chosen.is_none() && wanted == "ReadFiles" {
            for i in 0..supplied {
                let st = p.source_type(arg_types[1 + i]);
                if st.owner == TypeOwner::None && st.name == "Files" {
                    let fv = p.expr_var(sup_vars[i].clone())?;
                    chosen = Some(
                        p.expr_prim(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadOnly)), vec![fv])?,
                    );
                    break;
                }
            }
        }
        let e = match chosen {
            Some(e) => e,
            None => default_fake_cap(p, wanted)?,
        };
        call_args.push(e);
    }

    for act in &api.actors {
        let wanted_owner = p.source_type(act.ty).owner;
        let mut chosen = None;
        for i in 0..supplied {
            if p.source_type(arg_types[1 + i]).owner == wanted_owner {
                chosen = Some(p.expr_var(sup_vars[i].clone())?);
                break;
            }
        }
        let e = match chosen {
            Some(e) => e,
            None => default_fake_actor(p, act.service_index.0)?,
        };
        call_args.push(e);
    }

    let mut body = p.expr_call(callee, call_args)?;
    for i in (0..supplied).rev() {
        body = p.expr_let(sup_vars[i].clone(), args[1 + i], body)?;
    }
    p.expr_let(req_var, args[0], body)
}

pub(crate) fn lower_apitest_run_job(
    p: &mut Lowering<'_>,
    api_index: usize,
    args: Vec<ExprId>,
    arg_types: &[TypeId],
) -> LowerResult<ExprId> {
    let api = p.program.apis[api_index].clone();
    let supplied = args.len() - 2;
    let mut sup_vars = Vec::with_capacity(supplied);
    for _ in 0..supplied {
        sup_vars.push(p.fresh()?);
    }
    let name_var = p.fresh()?;
    let payload_var = p.fresh()?;
    let pv = p.expr_var(payload_var.clone())?;
    let payload_text = api_call(p, "encode_job_payload", vec![pv])?;

    let mod_name = p.module_name(api.module).to_string();
    let callee = p.concat_text(&mod_name, ".%api.job", "")?;
    let mut call_args = Vec::with_capacity(2 + api.capabilities.len() + api.actors.len());
    call_args.push(p.expr_var(name_var.clone())?);
    call_args.push(payload_text);

    for wanted in &api.capabilities {
        let mut chosen = None;
        for i in 0..supplied {
            let st = p.source_type(arg_types[2 + i]);
            if st.owner == TypeOwner::None && st.name == *wanted {
                chosen = Some(p.expr_var(sup_vars[i].clone())?);
                break;
            }
        }
        if chosen.is_none() && wanted == "ReadFiles" {
            for i in 0..supplied {
                let st = p.source_type(arg_types[2 + i]);
                if st.owner == TypeOwner::None && st.name == "Files" {
                    let fv = p.expr_var(sup_vars[i].clone())?;
                    chosen = Some(
                        p.expr_prim(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadOnly)), vec![fv])?,
                    );
                    break;
                }
            }
        }
        let e = match chosen {
            Some(e) => e,
            None => default_fake_cap(p, wanted)?,
        };
        call_args.push(e);
    }

    for act in &api.actors {
        let wanted_owner = p.source_type(act.ty).owner;
        let mut chosen = None;
        for i in 0..supplied {
            if p.source_type(arg_types[2 + i]).owner == wanted_owner {
                chosen = Some(p.expr_var(sup_vars[i].clone())?);
                break;
            }
        }
        let e = match chosen {
            Some(e) => e,
            None => default_fake_actor(p, act.service_index.0)?,
        };
        call_args.push(e);
    }

    let mut body = p.expr_call(callee, call_args)?;
    for i in (0..supplied).rev() {
        body = p.expr_let(sup_vars[i].clone(), args[2 + i], body)?;
    }
    body = p.expr_let(payload_var, args[1], body)?;
    p.expr_let(name_var, args[0], body)
}
