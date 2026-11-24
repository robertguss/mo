#![forbid(unsafe_code)]

//! Pure-Rust Mo lowering pass (`src/lower/` full parity).
//!
//! Translates a certified [`mo_check::CheckedProgram`] into a monomorphized, CPS-normalized
//! [`mo_core::Program`] with compiler-only [`mo_core::SourceMetadata`], [`mo_core::Services`]
//! descriptors, and optional [`LoweredPromise`] parameter-type summaries.
//!
//! Depends only on `mo-def`, `mo-base`, `mo-syntax`, `mo-check`, and `mo-core`.

mod abilities;
mod apis;
mod applications;
mod constants;
mod context;
mod cps;
mod expressions;
mod metadata;
mod scalar;
mod specs;
mod types;

#[cfg(test)]
mod tests;

use mo_base::{DiagBag, SourceMap};
use mo_check::{
    CheckedBody, CheckedEntryResult, CheckedFuncId, CheckedProgram, CheckedSpecId, CheckedSpecKind,
    ModuleId, ServiceReply, SourceType, TypeId,
};
use mo_core::{
    Arm, Cap, CoreArena, CoreNatural, DataDecl, Expr, ExprId, FnKind, IoOp, Lit, Prim, Program,
    Service, ServiceShape, ServiceTimer, ServiceTool, Services, Ty,
};
use mo_def::ErrorCode;
use mo_def::core_ops::{CoreBase, CoreCmpOp, CoreIoNullaryOp};
use mo_syntax::{NodeId, SyntaxKind};

pub use crate::context::LowerOptions;

use crate::abilities::lower_derived_functions;
use crate::apis::lower_apis;
use crate::constants::{lower_constant_prefixes, lower_constants};
use crate::context::{LowerResult, Lowering, SpecRef};
use crate::expressions::{lower_body_ref, lower_function};
use crate::metadata::lower_metadata;
use crate::scalar::{lower_literal, lower_to_text};
use crate::specs::lower_specs;
use crate::types::{lower_data, lower_indices, lower_instance, lower_source_type, lower_type};

/// One monomorphized function promise and its closed concrete source parameter types
/// (`struct mo_lowered_promise` in `src/lower/lower.h`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LoweredPromise {
    pub spec: CheckedSpecId,
    pub name: String,
    pub parameters: Vec<SourceType>,
    /// Closed source type IDs resolved against `CheckedProgram::signatures.types` +
    /// [`LoweredProgram::extra_types`].
    pub parameter_ids: Vec<TypeId>,
}

/// Output of lowering a [`CheckedProgram`] into Mo Core.
#[derive(Debug, Clone)]
pub struct LoweredProgram {
    pub arena: CoreArena,
    pub program: Program,
    pub services: Services,
    pub promises: Vec<LoweredPromise>,
    /// Synthesized closed [`SourceType`] nodes appended past `CheckedProgram::signatures.types.len()`.
    pub extra_types: Vec<SourceType>,
    pub query_index: Option<u32>,
}

fn status_check(p: &mut Lowering<'_>, value: ExprId) -> LowerResult<ExprId> {
    let name = p.fresh()?;
    let stop = p.expr_stop("exit status outside 0 to 255")?;
    let v0 = p.expr_var(name.clone())?;
    let zero = p.expr_int(0)?;
    let lt = p.expr_prim(
        Prim::Cmp {
            base: CoreBase::Int,
            op: CoreCmpOp::Lt,
        },
        vec![v0, zero],
    )?;
    let v1 = p.expr_var(name.clone())?;
    let hi = p.expr_int(255)?;
    let gt = p.expr_prim(
        Prim::Cmp {
            base: CoreBase::Int,
            op: CoreCmpOp::Gt,
        },
        vec![v1, hi],
    )?;
    let v2 = p.expr_var(name.clone())?;
    let upper = p.expr_if(gt, stop, v2)?;
    let test = p.expr_if(lt, stop, upper)?;
    p.expr_let(name, value, test)
}

fn emit_entry(p: &mut Lowering<'_>, main_idx: Option<usize>) -> LowerResult<()> {
    let checked = p.program.entry.clone();
    p.span = match main_idx {
        Some(midx) => {
            let fid = p.instances[midx].source;
            match fid {
                Some(id) => {
                    p.program
                        .interfaces
                        .symbol(p.program.function(id).symbol)
                        .declaration_span
                }
                None => p.program.modules.modules[0].syntax.span,
            }
        }
        None => p.program.modules.modules[0].syntax.span,
    };

    let int_src = p.types_overlay.builtin("Int");
    let int_ty = lower_type(p, int_src, None)?;
    let mod0_name = p.module_name(ModuleId(0)).to_string();
    let meta_name = match main_idx {
        Some(midx) => {
            let mname = p.instances[midx].meta.name.clone();
            p.concat_text(&mname, ".", "entry")?
        }
        None => p.concat_text(&mod0_name, ".", "%entry")?,
    };
    let entry_name = p.copy_text("%entry")?;
    let wrapper_idx = p.push_instance(
        ModuleId(0),
        entry_name,
        FnKind::Function,
        int_ty,
        meta_name,
        p.lower_span(p.span),
    )?;

    let Some(midx) = main_idx else {
        let zero = p.expr_int(0)?;
        p.instances[wrapper_idx].fun.body = zero;
        p.instances[wrapper_idx].body_set = true;
        return Ok(());
    };

    let main_params = p.instances[midx].fun.params.clone();
    let main_meta_params = p.instances[midx].meta.body.params.clone();
    let main_name = p.instances[midx].fun.name.clone();
    let main_func_id = checked.function;

    let mut console_var: Option<String> = None;
    let mut has_actor_param = false;
    for (i, pm) in main_params.iter().enumerate() {
        if matches!(p.core_ty(pm.ty), Ty::Cap(Cap::Console)) {
            console_var = Some(pm.name.clone());
        }
        if let Some(fid) = main_func_id
            && i < checked.capabilities.len()
        {
            let b_ty = p
                .program
                .function(fid)
                .body
                .binding(checked.capabilities[i])
                .ty;
            if p.source_type(b_ty)
                .owner
                .as_source()
                .is_some_and(|s| p.program.interfaces.symbol(s).decl_kind == SyntaxKind::Actor)
            {
                has_actor_param = true;
            }
        }
    }

    let add_console = checked.result == CheckedEntryResult::Result && console_var.is_none();
    let mut w_params = main_params.clone();
    let mut w_meta_params = main_meta_params;
    if add_console || has_actor_param {
        if let Some(fid) = main_func_id {
            for (i, &cap_bid) in checked.capabilities.iter().enumerate() {
                let b_ty = p.program.function(fid).body.binding(cap_bid).ty;
                let st = p.source_type(b_ty);
                if st
                    .owner
                    .as_source()
                    .is_some_and(|s| p.program.interfaces.symbol(s).decl_kind == SyntaxKind::Actor)
                    && let Some(amod) = st.module
                {
                    let amod_name = p.module_name(amod).to_string();
                    let label = p.concat_text("%actor.", &amod_name, "")?;
                    w_meta_params[i] = Some(label);
                }
            }
        }
        if add_console {
            let cname = p.fresh()?;
            let con_src = p.types_overlay.builtin("Console");
            let con_ty = lower_type(p, con_src, None)?;
            w_params.push(p.make_param(cname.clone(), con_ty)?);
            w_meta_params.push(None);
            console_var = Some(cname);
        }
    }
    p.instances[wrapper_idx].fun.params = w_params;
    p.instances[wrapper_idx].meta.body.params = w_meta_params;

    let mut call_args = Vec::with_capacity(main_params.len());
    for pm in &main_params {
        call_args.push(p.expr_var(pm.name.clone())?);
    }
    let call = p.expr_call(main_name, call_args)?;

    let body = match checked.result {
        CheckedEntryResult::Status => status_check(p, call)?,
        CheckedEntryResult::Unit => {
            let tmp = p.fresh()?;
            let zero = p.expr_int(0)?;
            p.expr_let(tmp, call, zero)?
        }
        CheckedEntryResult::Result => {
            let Some(err_proof) = checked.error_text else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "Result main missing error_text proof",
                );
            };
            let ok_var = p.fresh()?;
            let err_var = p.fresh()?;
            let ok_body = p.expr_int(0)?;
            let ev = p.expr_var(err_var.clone())?;
            let text_e = lower_to_text(p, err_proof, None, ModuleId(0), 0, ev)?;
            let cv = p.expr_var(console_var.unwrap_or_default())?;
            let printed = p.expr_prim(
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::PrintError)),
                vec![cv, text_e],
            )?;
            let tmp = p.fresh()?;
            let one = p.expr_int(1)?;
            let err_body = p.expr_let(tmp, printed, one)?;
            let ok_ctor = p.copy_text("Ok")?;
            let err_ctor = p.copy_text("Error")?;
            p.alloc_expr(Expr::Case {
                expr: call,
                arms: vec![
                    Arm {
                        ctor: ok_ctor,
                        names: vec![ok_var],
                        body: ok_body,
                    },
                    Arm {
                        ctor: err_ctor,
                        names: vec![err_var],
                        body: err_body,
                    },
                ],
            })?
        }
    };
    p.instances[wrapper_idx].fun.body = body;
    p.instances[wrapper_idx].body_set = true;
    Ok(())
}

fn service_value<'b>(
    p: &mut Lowering<'b>,
    module: ModuleId,
    suffix: &str,
    index: u32,
    ty: TypeId,
    body: &'b CheckedBody,
) -> LowerResult<u32> {
    let mod_name = p.module_name(module).to_string();
    let num = if index == 0 {
        String::new()
    } else {
        p.number_text(u64::from(index))?
    };
    let name = p.concat_text(&mod_name, suffix, &num)?;
    let ret = lower_type(p, ty, None)?;
    let idx = p.push_instance(
        module,
        name.clone(),
        FnKind::Function,
        ret,
        name,
        p.lower_span(p.span),
    )?;
    let ccount = p.program.constants.len() as u64;
    let digits = p.number_text(ccount)?;
    p.instances[idx].fun.const_count = CoreNatural { digits };
    lower_body_ref(p, idx, module, body, &[])?;
    Ok(p.instances[idx].index)
}

fn core_function(p: &mut Lowering<'_>, func_id: CheckedFuncId) -> LowerResult<u32> {
    let sym = p.program.function(func_id).symbol;
    let inst = lower_instance(p, sym, &[], None, &[], sym.module, 0)?;
    Ok(p.instances[inst].index)
}

fn service_description(
    p: &mut Lowering<'_>,
    module: ModuleId,
    fn_decl: NodeId,
) -> LowerResult<String> {
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
            out.push('\n');
        }
        out.push_str(&String::from_utf8_lossy(&raw[start..]));
        first = false;
    }
    p.charge_alloc(out.len() + 1, 1, 1)?;
    Ok(out)
}

fn lower_services_block<'b>(p: &mut Lowering<'b>) -> LowerResult<Services> {
    let count = p.program.services.len();
    let mut items = Vec::with_capacity(count);
    let duration_src = p.types_overlay.builtin("Duration");
    let text_src = p.types_overlay.builtin("Text");
    let text_ty = lower_type(p, text_src, None)?;

    for i in 0..count {
        let (module, syn, state, is_actor, token_source, tool_count, timer_count) = {
            let s = &p.program.services[i];
            (
                s.module,
                s.syntax,
                s.state,
                s.is_actor,
                s.token_source,
                s.tools.len(),
                s.timers.len(),
            )
        };
        p.span = p.syntax_node(module, syn).span;
        let init_body = &p.program.services[i].initializer;
        let start = service_value(p, module, ".%service.start", 0, state, init_body)?;
        let token_env = if is_actor {
            p.copy_text("_MO_ACTOR")?
        } else if let Some(ts) = token_source {
            let lit = lower_literal(p, module, ts, text_ty, false)?;
            match p.arena.expr(lit) {
                Expr::Lit(Lit::Text(s)) => s.clone(),
                _ => String::new(),
            }
        } else {
            String::new()
        };

        let mut tools = Vec::with_capacity(tool_count);
        for j in 0..tool_count {
            let (t_name_nid, t_func, t_reply) = {
                let t = &p.program.services[i].tools[j];
                (t.name, t.function, t.reply)
            };
            let tname = p.syntax_str(module, t_name_nid);
            let tname = p.copy_text(&tname)?;
            let fn_idx = core_function(p, t_func)?;
            let (fn_mod, fn_decl, arg_names) = {
                let f = p.program.function(t_func);
                let sym = p.program.interfaces.symbol(f.symbol);
                let mut names = Vec::with_capacity(f.parameters.len().saturating_sub(2));
                for &bid in f.parameters.iter().skip(2) {
                    names.push(f.body.binding(bid).name.clone());
                }
                (sym.module, sym.declaration, names)
            };
            let description = service_description(p, fn_mod, fn_decl)?;
            tools.push(ServiceTool {
                name: tname,
                function: fn_idx,
                read_only: t_reply == ServiceReply::ReadOnly,
                description,
                argument_names: arg_names,
            });
        }

        let mut timers = Vec::with_capacity(timer_count);
        for j in 0..timer_count {
            let (tm_syn, tm_func) = {
                let tm = &p.program.services[i].timers[j];
                (tm.syntax, tm.function)
            };
            p.span = p.syntax_node(module, tm_syn).span;
            let fn_idx = core_function(p, tm_func)?;
            let interval_body = &p.program.services[i].timers[j].interval;
            let interval = service_value(
                p,
                module,
                ".%service.interval.",
                (j + 1) as u32,
                duration_src,
                interval_body,
            )?;
            timers.push(ServiceTimer {
                function: fn_idx,
                interval,
            });
        }

        items.push(Service {
            start,
            token_env,
            tools,
            timers,
        });
    }

    Ok(Services {
        items,
        data_shapes: Vec::new(),
    })
}

fn is_function_promise(p: &Lowering<'_>, inst_idx: usize) -> Option<CheckedSpecId> {
    match p.instances[inst_idx].spec {
        Some(SpecRef::Program(spec_id)) => {
            let s = p.program.spec(spec_id);
            if s.kind == CheckedSpecKind::Promise && s.subject.is_some() {
                Some(spec_id)
            } else {
                None
            }
        }
        _ => None,
    }
}

fn run_lowering(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    options: &LowerOptions,
    diags: &mut DiagBag,
) -> LowerResult<LoweredProgram> {
    let mut p = Lowering::new(program, options, diags)?;

    lower_indices(&mut p)?;

    // 1. Pre-lower all non-generic nominal data declarations.
    let data_sig_tys: Vec<TypeId> = program.signatures.data.iter().map(|d| d.ty).collect();
    for ty_id in data_sig_tys {
        if p.source_type(ty_id).parameters == 0 {
            let _ = lower_type(&mut p, ty_id, None)?;
        }
    }

    // 2. Reserve `main` (owner 0) if present, plus all non-generic top-level functions.
    let main_inst = match program.entry.function {
        Some(fid) => {
            let sym = program.function(fid).symbol;
            Some(lower_instance(&mut p, sym, &[], None, &[], ModuleId(0), 0)?)
        }
        None => None,
    };
    for f in &program.functions {
        let sig_ty = program.signatures.functions[f.signature.index()].ty;
        if let Some(tid) = sig_ty
            && p.source_type(tid).parameters == 0
        {
            let sym = f.symbol;
            let _ = lower_instance(&mut p, sym, &[], None, &[], sym.module, 0)?;
        }
    }

    // 3. Reserve optional query function.
    let query_index = match options.query {
        Some(q) => Some(core_function(&mut p, q)?),
        None => None,
    };

    // 4. Lower all currently queued (and newly discovered) ordinary functions.
    let mut idx = 0usize;
    while idx < p.instances.len() {
        if p.instances[idx].source.is_some() && !p.instances[idx].body_set {
            lower_function(&mut p, idx)?;
        }
        idx += 1;
    }

    // 5. Constants, specs, services, APIs, %entry.
    lower_constants(&mut p)?;
    lower_specs(&mut p)?;
    let mut service_data = lower_services_block(&mut p)?;
    lower_apis(&mut p)?;
    emit_entry(&mut p, main_inst)?;

    // 6. Fixed-point drain of derived ability functions and newly discovered source/spec instances.
    loop {
        lower_derived_functions(&mut p)?;
        let mut pending = false;
        let len = p.instances.len();
        for i in 0..len {
            p.work(1)?;
            if p.instances[i].body_set {
                continue;
            }
            if p.instances[i].source.is_some() {
                pending = true;
                lower_function(&mut p, i)?;
            } else if let Some(spec_ref) = p.instances[i].spec {
                pending = true;
                let (smod, params) = {
                    let s = p.spec_ref(spec_ref);
                    (s.module, s.parameters.clone())
                };
                let body = match spec_ref {
                    SpecRef::Program(id) => &p.program.spec(id).body,
                    SpecRef::ServicePromise { service, promise } => {
                        &p.program.services[service].promises[promise].spec.body
                    }
                    SpecRef::ServiceAnchor {
                        service,
                        promise,
                        anchor,
                    } => &p.program.services[service].promises[promise].anchors[anchor].body,
                };
                lower_body_ref(&mut p, i, smod, body, &params)?;
            }
        }
        if !pending && p.derived_pending_head >= p.derived.len() {
            break;
        }
    }

    // 7. Constant prefix tightening & E1150 check.
    lower_constant_prefixes(&mut p)?;

    // 8. Finalize all monomorphized nominal data constructors & service shapes.
    let data_count = p.data_instances.len();
    for i in 0..data_count {
        lower_data(&mut p, i)?;
    }

    // 9. Assemble Core Program + Services + Metadata + LoweredPromises.
    let mut data_decls = Vec::with_capacity(data_count);
    let mut shapes: Vec<ServiceShape> = if !service_data.items.is_empty() {
        Vec::with_capacity(data_count)
    } else {
        Vec::new()
    };
    for d in &p.data_instances {
        data_decls.push(DataDecl {
            name: d.name.clone(),
            ctors: d.ctors.clone(),
        });
        if !service_data.items.is_empty() {
            shapes.push(d.shape.clone());
        }
    }
    service_data.data_shapes = shapes;

    let fun_decls = p.instances.iter().map(|inst| inst.fun.clone()).collect();
    let const_decls = p.consts.iter().map(|c| c.decl.clone()).collect();

    let mut core_program = Program {
        data: data_decls,
        funs: fun_decls,
        consts: const_decls,
        entry: "%entry".to_string(),
        metadata: mo_core::Metadata::NoSource,
    };

    lower_metadata(&mut p, sources, &mut core_program)?;

    let mut promises = Vec::new();
    if options.collect_promises {
        for i in 0..p.instances.len() {
            p.work(1)?;
            let Some(spec_id) = is_function_promise(&p, i) else {
                continue;
            };
            let Some(subj_idx) = p.instances[i].subject else {
                continue;
            };
            let subj_param_tys: Vec<mo_core::TyId> = p.instances[subj_idx]
                .fun
                .params
                .iter()
                .map(|pm| pm.ty)
                .collect();
            let mut param_sources = Vec::with_capacity(subj_param_tys.len());
            let mut param_ids = Vec::with_capacity(subj_param_tys.len());
            for ty_id in subj_param_tys {
                p.work(1)?;
                let Some(src_id) = lower_source_type(&p, ty_id) else {
                    return p.refuse(
                        ErrorCode::LowerUnfinished,
                        "missing closed source type for promise parameter",
                    );
                };
                param_sources.push(p.source_type(src_id).clone());
                param_ids.push(src_id);
            }
            promises.push(LoweredPromise {
                spec: spec_id,
                name: p.instances[i].fun.name.clone(),
                parameters: param_sources,
                parameter_ids: param_ids,
            });
        }
    }

    let services = if options.include_services {
        service_data
    } else {
        Services::default()
    };

    Ok(LoweredProgram {
        arena: p.arena,
        program: core_program,
        services,
        promises,
        extra_types: p.types_overlay.into_extra(),
        query_index,
    })
}

/// Lowers a checked program with explicit [`LowerOptions`].
#[must_use]
pub fn lower_with_options(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    options: &LowerOptions,
    diags: &mut DiagBag,
) -> Option<LoweredProgram> {
    run_lowering(program, sources, options, diags).ok()
}

/// Lowers a checked program (`mo_lower` equivalent).
#[must_use]
pub fn lower_program(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    diags: &mut DiagBag,
) -> Option<LoweredProgram> {
    lower_with_options(program, sources, &LowerOptions::default(), diags)
}

/// Lowers a checked program and retains closed source parameter types for promises
/// (`mo_lower_for_tests` equivalent).
#[must_use]
pub fn lower_for_tests(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    diags: &mut DiagBag,
) -> Option<LoweredProgram> {
    let opts = LowerOptions {
        collect_promises: true,
        ..LowerOptions::default()
    };
    lower_with_options(program, sources, &opts, diags)
}

/// Lowers a checked program with service descriptors and optional promise metadata
/// (`mo_lower_services` equivalent).
#[must_use]
pub fn lower_services(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    diags: &mut DiagBag,
) -> Option<LoweredProgram> {
    let opts = LowerOptions {
        include_services: true,
        ..LowerOptions::default()
    };
    lower_with_options(program, sources, &opts, diags)
}

/// Lowers a checked program in `mo test` mode with promise vacuity tracking, promise metadata,
/// and service descriptors (`mo_lower_test_services` equivalent).
#[must_use]
pub fn lower_test_services(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    diags: &mut DiagBag,
) -> Option<LoweredProgram> {
    let opts = LowerOptions {
        promise_vacuity: true,
        collect_promises: true,
        include_services: true,
        ..LowerOptions::default()
    };
    lower_with_options(program, sources, &opts, diags)
}

/// Lowers a checked program targeting a specific zero-generic query function
/// (`mo_lower_query` equivalent).
#[must_use]
pub fn lower_query(
    program: &CheckedProgram,
    sources: Option<&SourceMap>,
    query: CheckedFuncId,
    diags: &mut DiagBag,
) -> Option<LoweredProgram> {
    let opts = LowerOptions {
        include_services: true,
        query: Some(query),
        ..LowerOptions::default()
    };
    lower_with_options(program, sources, &opts, diags)
}
