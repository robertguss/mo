//! Canonical Core type interning, monomorphized function & nominal data instance tables,
//! ability proof substitution, and built-in/user constructor descriptors (`src/lower/types.c` parity).

use mo_check::{
    AbilityImplementationKind, CheckedAbilityUse, CheckedFuncId, DataSignature, ModuleId, ProofId,
    SourceType, SourceTypeKind, SymRef, TypeArgument, TypeId,
};
use mo_core::{Cap, CtorDecl, FnKind, ServiceShape, Ty, TyId};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES, MO_LIMIT_INSTANTIATION_DEPTH,
    MO_LIMIT_SOURCE_ABILITY_DEPTH, MO_LIMIT_SOURCE_TYPE_DEPTH,
};
use mo_syntax::SyntaxKind;

use crate::constants::lower_global_name;
use crate::context::{
    AbilityBinding, CoreTypeEntry, DataInstance, LowerResult, Lowering, SubstId, Substitution,
};

fn text_hash(p: &mut Lowering<'_>, name: &str) -> LowerResult<u64> {
    p.work(name.len())?;
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for &b in name.as_bytes() {
        hash = (hash ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
    }
    Ok(hash)
}

pub(crate) fn lower_type_name(p: &mut Lowering<'_>, ty: TyId) -> LowerResult<String> {
    p.work(1)?;
    Ok(p.core_types[ty.0 as usize].name.clone())
}

pub(crate) fn lower_source_type(p: &Lowering<'_>, ty: TyId) -> Option<TypeId> {
    p.core_types[ty.0 as usize].closed_source
}

pub(crate) fn same_parameter(p: &Lowering<'_>, a: TypeId, b: TypeId) -> bool {
    if a == b {
        return true;
    }
    let ta = p.source_type(a);
    let tb = p.source_type(b);
    ta.kind == SourceTypeKind::Parameter
        && tb.kind == SourceTypeKind::Parameter
        && ta.owner == tb.owner
        && ta.module == tb.module
        && ta.name == tb.name
}

fn data_signature<'a>(p: &mut Lowering<'a>, symbol: SymRef) -> LowerResult<&'a DataSignature> {
    p.work(1)?;
    if let Some((_, sig)) = p.program.signatures.data_signature_by_symbol(symbol) {
        return Ok(sig);
    }
    p.refuse(
        ErrorCode::LowerUnfinished,
        "missing data signature during lowering",
    )
}

fn applied_name(p: &mut Lowering<'_>, mut name: String, types: &[TyId]) -> LowerResult<String> {
    if types.is_empty() {
        return Ok(name);
    }
    name = p.concat_text(&name, "(", "")?;
    for (i, &t) in types.iter().enumerate() {
        let tname = lower_type_name(p, t)?;
        name = p.concat_text(&name, if i == 0 { "" } else { ", " }, &tname)?;
    }
    p.concat_text(&name, ")", "")
}

pub(crate) fn lower_api_app_parts(p: &mut Lowering<'_>) -> LowerResult<[TyId; 4]> {
    let req_src = crate::apis::lib_data_source(p, "Api", "Request")?;
    let res_src = crate::apis::lib_data_source(p, "Api", "Response")?;
    let rs_src = crate::apis::lib_data_source(p, "Api", "RouteSpec")?;
    let json_src = crate::apis::lib_data_source(p, "Json", "Json")?;
    let rs_ty = lower_type(p, rs_src, None)?;
    let _ = lower_ctors(p, rs_ty)?;

    let text_src = p.types_overlay.builtin("Text");
    let unit_src = p.types_overlay.builtin("Unit");
    let res_unit_text_src = p
        .types_overlay
        .builtin_applied("Result", vec![unit_src, text_src]);
    let list_rs_src = p.types_overlay.builtin_applied("List", vec![rs_src]);

    let make_fn_src = |p: &mut Lowering<'_>, args: Vec<TypeId>| -> TypeId {
        let mut h = 0u32;
        for &a in &args {
            let ah = p.source_type(a).height;
            if h <= ah {
                h = ah + 1;
            }
        }
        p.types_overlay.alloc(SourceType {
            kind: SourceTypeKind::Function,
            name: "fn".to_string(),
            module: None,
            owner: mo_check::TypeOwner::None,
            arguments: args,
            height: h,
            parameters: 0,
        })
    };

    let send_src = make_fn_src(p, vec![req_src, res_src]);
    let job_src = make_fn_src(p, vec![text_src, json_src, res_unit_text_src]);
    let routes_src = make_fn_src(p, vec![list_rs_src]);
    let openapi_src = make_fn_src(p, vec![text_src]);

    Ok([
        lower_type(p, send_src, None)?,
        lower_type(p, job_src, None)?,
        lower_type(p, routes_src, None)?,
        lower_type(p, openapi_src, None)?,
    ])
}

fn make_type(p: &mut Lowering<'_>, source_id: TypeId, args: &[TyId]) -> LowerResult<TyId> {
    p.charge_alloc(1, 96, 8)?;
    let source = p.source_type(source_id).clone();
    let (ty, name) = match source.kind {
        SourceTypeKind::Function => {
            debug_assert!(!args.is_empty());
            let param_count = args.len() - 1;
            let params = args[..param_count].to_vec();
            let ret = args[param_count];
            let fn_txt = p.copy_text("fn")?;
            let mut n = applied_name(p, fn_txt, &params)?;
            if param_count == 0 {
                n = p.concat_text(&n, "()", "")?;
            }
            let rname = lower_type_name(p, ret)?;
            n = p.concat_text(&n, " : ", &rname)?;
            (Ty::Fn { params, ret }, n)
        }
        SourceTypeKind::Tuple => {
            let n = applied_name(p, String::new(), args)?;
            (Ty::Tuple(args.to_vec()), n)
        }
        SourceTypeKind::Named => {
            let is_actor = source
                .owner
                .as_source()
                .is_some_and(|sym| p.program.interfaces.symbol(sym).decl_kind == SyntaxKind::Actor);
            if is_actor {
                let n = p.copy_text("Simulation")?;
                let full = applied_name(p, n, args)?;
                (Ty::Simulation, full)
            } else if let Some(mod_id) = source.module {
                let mod_name = p.module_name(mod_id).to_string();
                let qual = p.concat_text(&mod_name, ".", &source.name)?;
                let full = applied_name(p, qual, args)?;
                (Ty::Data(full.clone()), full)
            } else if source.name == "ApiTest.App" {
                let parts = lower_api_app_parts(p)?.to_vec();
                let full = applied_name(p, String::new(), &parts)?;
                (Ty::Tuple(parts), full)
            } else {
                let base_name = p.copy_text(&source.name)?;
                let full = applied_name(p, base_name, args)?;
                let ty = match source.name.as_str() {
                    "Int" => Ty::Int,
                    "BigInt" => Ty::BigInt,
                    "Byte" => Ty::Byte,
                    "Decimal" => Ty::Decimal,
                    "Float" => Ty::Float,
                    "Bool" => Ty::Bool,
                    "Char" => Ty::Char,
                    "Text" => Ty::Text,
                    "Bytes" => Ty::Bytes,
                    "Unit" => Ty::Unit,
                    "Range" => Ty::Range,
                    "Time" => Ty::Time,
                    "Duration" => Ty::Duration,
                    "List" => Ty::List(args[0]),
                    "Map" => Ty::Map {
                        key: args[0],
                        value: args[1],
                    },
                    "Set" => Ty::Set(args[0]),
                    "Option" => Ty::Option(args[0]),
                    "Result" => Ty::Result {
                        ok: args[0],
                        err: args[1],
                    },
                    "Ordering" => Ty::Ordering,
                    "FileError" => Ty::FileError,
                    "Entry" => Ty::Entry,
                    "EntryKind" => Ty::EntryKind,
                    "Simulation" => Ty::Simulation,
                    "SimulationError" => Ty::SimulationError,
                    "NetworkError" => Ty::NetworkError,
                    "Sqlite.Value" => Ty::SqliteValue,
                    "Sqlite.Rows" => Ty::SqliteRows,
                    "SqliteError" => Ty::SqliteError,
                    "Console" => Ty::Cap(Cap::Console),
                    "Files" => Ty::Cap(Cap::Files),
                    "ReadFiles" => Ty::Cap(Cap::ReadFiles),
                    "Clock" => Ty::Cap(Cap::Clock),
                    "Random" => Ty::Cap(Cap::Random),
                    "Args" => Ty::Cap(Cap::Args),
                    "Env" => Ty::Cap(Cap::Env),
                    "Network" => Ty::Cap(Cap::Network),
                    "Connection" => Ty::Cap(Cap::Connection),
                    "Sqlite" => Ty::Cap(Cap::Sqlite),
                    "Sqlite.Database" => Ty::Cap(Cap::Database),
                    "Command" => Ty::Cap(Cap::Command),
                    "SecureRandom" => Ty::Cap(Cap::SecureRandom),
                    "Files.File" => Ty::Cap(Cap::File),
                    _ => {
                        return p.refuse(
                            ErrorCode::LowerUnfinished,
                            "unknown built-in type during lowering",
                        );
                    }
                };
                (ty, full)
            }
        }
        SourceTypeKind::Parameter => {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "unsubstituted type parameter during lowering",
            );
        }
    };

    let _ = text_hash(p, &name)?;
    if let Some(&existing) = p.core_types_by_name.get(&name) {
        p.work(1 + name.len())?;
        return Ok(existing);
    }

    let is_data = matches!(ty, Ty::Data(_));
    let mut height = 0u32;
    if let Ty::Tuple(ref tuple_parts) = ty {
        for &part in tuple_parts {
            p.work(1)?;
            let part_h = p.core_types[part.0 as usize].height;
            if height <= part_h {
                height = part_h + 1;
            }
        }
    } else if !is_data {
        for &arg in args {
            p.work(1)?;
            let arg_h = p.core_types[arg.0 as usize].height;
            if height <= arg_h {
                height = arg_h + 1;
            }
        }
    }
    if u64::from(height) >= MO_LIMIT_CORE_DEPTH {
        return p.refuse(
            ErrorCode::LowerDepth,
            "substituted type exceeds the Core path limit",
        );
    }
    if p.core_types.len() as u64 >= MO_LIMIT_CORE_NODES {
        return p.refuse(
            ErrorCode::LowerWork,
            "lowering exceeds its Core type node limit",
        );
    }

    let closed_source = if p.options.collect_promises {
        let mut closed_args = Vec::with_capacity(args.len());
        let mut closed_height = 0u32;
        for &arg in args {
            p.work(1)?;
            let Some(cs) = p.core_types[arg.0 as usize].closed_source else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "missing closed source type for promise",
                );
            };
            let ch = p.source_type(cs).height;
            if closed_height <= ch {
                closed_height = ch + 1;
            }
            closed_args.push(cs);
        }
        let closed_ty = SourceType {
            kind: source.kind,
            name: source.name.clone(),
            module: source.module,
            owner: source.owner,
            arguments: closed_args,
            height: closed_height,
            parameters: 0,
        };
        Some(p.types_overlay.alloc(closed_ty))
    } else {
        None
    };

    let ty_id = p.arena.alloc_ty(ty);
    p.core_types.push(CoreTypeEntry {
        name: name.clone(),
        height,
        closed_source,
    });
    p.core_types_by_name.insert(name.clone(), ty_id);

    if is_data {
        let Some(owner) = source.module else {
            return p.refuse(ErrorCode::LowerUnfinished, "nominal data missing module");
        };
        let subst = if !args.is_empty() {
            let Some(mut sym) = source.owner.as_source() else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "nominal data missing owner symbol",
                );
            };
            if p.program.interfaces.symbol(sym).decl_kind == SyntaxKind::Enum {
                let first_case = p.program.interfaces.symbol(sym).cases[0];
                sym = SymRef::new(sym.module, first_case);
            }
            let abstract_ty = data_signature(p, sym)?.ty;
            let abstract_params = p.source_type(abstract_ty).arguments.clone();
            debug_assert_eq!(abstract_params.len(), args.len());
            Some(p.alloc_subst(Substitution {
                parameters: abstract_params,
                types: args.to_vec(),
                abilities: Vec::new(),
            })?)
        } else {
            None
        };
        p.charge_alloc(1, 128, 8)?;
        let data_idx = p.data_instances.len();
        p.data_instances.push(DataInstance {
            source_ty: source_id,
            owner,
            subst,
            name,
            ctors: Vec::new(),
            shape: ServiceShape {
                is_struct: false,
                unnamed: Vec::new(),
            },
        });
        p.data_by_ty.insert(ty_id, data_idx);
    }

    Ok(ty_id)
}

struct TypeFrame {
    source: TypeId,
    args: Vec<TyId>,
    at: usize,
}

/// Iteratively lowers a surface [`TypeId`] under optional [`SubstId`] to a canonical Core [`TyId`].
pub(crate) fn lower_type(
    p: &mut Lowering<'_>,
    ty: TypeId,
    subst: Option<SubstId>,
) -> LowerResult<TyId> {
    let eff_subst = if p.source_type(ty).parameters == 0 {
        None
    } else {
        subst
    };
    if let Some(&cached) = p.type_cache.get(&(ty, eff_subst)) {
        return Ok(cached);
    }

    let mut stack = vec![TypeFrame {
        source: ty,
        args: Vec::new(),
        at: 0,
    }];
    let mut last_result: Option<TyId> = None;

    loop {
        let stack_len = stack.len();
        let Some(frame) = stack.last_mut() else {
            break;
        };
        p.work(1)?;
        if let Some(child_res) = last_result.take() {
            frame.args.push(child_res);
        }

        let src_id = frame.source;
        let src_eff_subst = if p.source_type(src_id).parameters == 0 {
            None
        } else {
            subst
        };
        if let Some(&cached) = p.type_cache.get(&(src_id, src_eff_subst)) {
            stack.pop();
            last_result = Some(cached);
            continue;
        }

        let (kind, arg_count) = {
            let s = p.source_type(src_id);
            (s.kind, s.arguments.len())
        };

        if kind == SourceTypeKind::Parameter {
            let Some(sid) = subst else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "missing substitution for type parameter",
                );
            };
            let params = p.subst(sid).parameters.clone();
            let types = p.subst(sid).types.clone();
            let mut found = None;
            for (param, core_ty) in params.iter().zip(types.iter()) {
                if same_parameter(p, src_id, *param) {
                    found = Some(*core_ty);
                    break;
                }
            }
            let Some(res) = found else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "type parameter not found in substitution",
                );
            };
            remember_type(p, src_id, src_eff_subst, res)?;
            stack.pop();
            last_result = Some(res);
            continue;
        }

        if frame.at < arg_count {
            if stack_len as u64 > MO_LIMIT_SOURCE_TYPE_DEPTH {
                return p.refuse(
                    ErrorCode::LowerDepth,
                    "lowering exceeds its type path limit",
                );
            }
            let next_arg = p.source_type(src_id).arguments[frame.at];
            frame.at += 1;
            stack.push(TypeFrame {
                source: next_arg,
                args: Vec::new(),
                at: 0,
            });
            continue;
        }

        let finished_args = std::mem::take(&mut frame.args);
        let res = make_type(p, src_id, &finished_args)?;
        remember_type(p, src_id, src_eff_subst, res)?;
        stack.pop();
        last_result = Some(res);
    }

    last_result.ok_or_else(|| {
        let _ = p.refuse::<()>(ErrorCode::LowerUnfinished, "empty type lowering result");
        crate::context::LowerAbort
    })
}

fn remember_type(
    p: &mut Lowering<'_>,
    source: TypeId,
    subst: Option<SubstId>,
    ty: TyId,
) -> LowerResult<()> {
    if p.cache_count as u64 >= MO_LIMIT_CORE_NODES {
        return p.refuse(
            ErrorCode::LowerWork,
            "lowering exceeds its type cache limit",
        );
    }
    p.charge_alloc(1, 48, 8)?;
    p.type_cache.insert((source, subst), ty);
    p.cache_count += 1;
    Ok(())
}

/// Builds fast lookup tables for checked functions and constants (`lower_indices`).
pub(crate) fn lower_indices(p: &mut Lowering<'_>) -> LowerResult<()> {
    let count = p.program.functions.len();
    p.charge_alloc(count, 8, 8)?;
    for (i, func) in p.program.functions.iter().enumerate() {
        p.work(1)?;
        p.func_by_sym.insert(func.symbol, CheckedFuncId(i as u32));
    }
    for (i, c) in p.program.constants.iter().enumerate() {
        p.work(1)?;
        p.const_by_sym
            .insert(c.symbol, mo_check::CheckedConstId(i as u32));
    }
    Ok(())
}

/// Unwraps `Parameter` ability proofs through the substitution chain until a concrete proof
/// (`Native`, `Derived`, or `Function`) is reached.
pub(crate) fn lower_proof(
    p: &mut Lowering<'_>,
    proof: &mut ProofId,
    subst: &mut Option<SubstId>,
) -> LowerResult<()> {
    let mut depth = 0u32;
    while p.proof(*proof).kind == AbilityImplementationKind::Parameter {
        p.work(1)?;
        if u64::from(depth) == MO_LIMIT_SOURCE_ABILITY_DEPTH {
            return p.refuse(
                ErrorCode::LowerDepth,
                "ability substitution is nested too deeply",
            );
        }
        depth += 1;
        let want_ability = p.proof(*proof).ability;
        let want_ty = p.proof(*proof).ty;
        let mut found: Option<AbilityBinding> = None;
        if let Some(sid) = *subst {
            let abilities = p.subst(sid).abilities.clone();
            for cand in abilities {
                p.work(1)?;
                if p.proof(cand.proof).ability == want_ability
                    && same_parameter(p, cand.parameter, want_ty)
                {
                    found = Some(cand);
                    break;
                }
            }
        }
        let Some(binding) = found else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "this generic requirement has no keyed implementation proof",
            );
        };
        *proof = binding.proof;
        *subst = binding.substitution;
    }
    Ok(())
}

#[expect(
    clippy::too_many_arguments,
    reason = "mirrors C lower_customizer_fn parameter list directly"
)]
pub(crate) fn lower_customizer_fn(
    p: &mut Lowering<'_>,
    function: SymRef,
    arguments: &[TypeArgument],
    parts: &[ProofId],
    part_parameters: &[TypeId],
    subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
) -> LowerResult<usize> {
    let mut uses = Vec::with_capacity(parts.len());
    for (&impl_id, &param_ty) in parts.iter().zip(part_parameters.iter()) {
        p.work(1)?;
        uses.push(CheckedAbilityUse {
            operand: None,
            implementation: impl_id,
            parameter: Some(param_ty),
        });
    }
    let next_depth = depth + u32::from(!arguments.is_empty());
    lower_instance(p, function, arguments, subst, &uses, owner, next_depth)
}

pub(crate) fn lower_customizer(
    p: &mut Lowering<'_>,
    proof: ProofId,
    subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
) -> LowerResult<usize> {
    let imp = p.proof(proof).clone();
    debug_assert_eq!(imp.kind, AbilityImplementationKind::Function);
    let Some(func_sym) = imp.function else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "customizer proof missing function",
        );
    };
    lower_customizer_fn(
        p,
        func_sym,
        &imp.arguments,
        &imp.parts,
        &imp.part_parameters,
        subst,
        owner,
        depth,
    )
}

pub(crate) fn lower_function_name(p: &mut Lowering<'_>, symbol: SymRef) -> LowerResult<String> {
    let mut name = lower_global_name(p, symbol)?;
    let sym = p.program.interfaces.symbol(symbol);
    if let Some(recv_idx) = sym.receiver {
        let scope = &p.program.interfaces.scopes[sym.module.index()];
        if let Some(group) = scope.find_symbol(sym.name.as_bytes())
            && group.overload_count > 1
        {
            let recv_name = scope.symbols[recv_idx.index()].name.clone();
            name = p.concat_text(&name, "@", &recv_name)?;
        }
    }
    Ok(name)
}

/// Reserves (or retrieves memoized) Core function instance for `symbol` with concrete `arguments`.
pub(crate) fn lower_instance(
    p: &mut Lowering<'_>,
    symbol: SymRef,
    arguments: &[TypeArgument],
    subst: Option<SubstId>,
    abilities: &[CheckedAbilityUse],
    owner: ModuleId,
    depth: u32,
) -> LowerResult<usize> {
    let Some(&func_id) = p.func_by_sym.get(&symbol) else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "checked function missing during lowering",
        );
    };
    let sym_module = p.program.interfaces.symbol(symbol).module;
    let instance_owner = if arguments.is_empty() {
        sym_module
    } else {
        owner
    };
    let mut types = Vec::with_capacity(arguments.len());
    for arg in arguments {
        types.push(lower_type(p, arg.ty, subst)?);
    }

    let key = (func_id, instance_owner, types.clone());
    if let Some(&existing) = p.instance_index.get(&key) {
        p.work(1 + arguments.len())?;
        return Ok(existing);
    }
    if u64::from(depth) > MO_LIMIT_INSTANTIATION_DEPTH {
        return p.refuse(
            ErrorCode::LowerDepth,
            "generic instantiation is nested too deeply",
        );
    }

    let mut subst_params = Vec::with_capacity(arguments.len());
    for arg in arguments {
        subst_params.push(arg.parameter);
    }
    let mut ability_bindings = Vec::new();
    for ab in abilities {
        p.work(1)?;
        if ab.operand.is_some() {
            continue;
        }
        let Some(param) = ab.parameter else {
            continue;
        };
        let mut proof = ab.implementation;
        let mut env = subst;
        lower_proof(p, &mut proof, &mut env)?;
        ability_bindings.push(AbilityBinding {
            parameter: param,
            proof,
            substitution: env,
        });
    }
    let inst_subst = if subst_params.is_empty() && ability_bindings.is_empty() {
        None
    } else {
        Some(p.alloc_subst(Substitution {
            parameters: subst_params,
            types: types.clone(),
            abilities: ability_bindings,
        })?)
    };

    let base_name = lower_function_name(p, symbol)?;
    let mut name = applied_name(p, base_name, &types)?;
    if !arguments.is_empty() {
        let owner_name = p.module_name(owner).to_string();
        name = p.concat_text(&owner_name, "::", &name)?;
    }
    let decl_span = p.program.interfaces.symbol(symbol).declaration_span;
    let unit_src = p.types_overlay.builtin("Unit");
    let placeholder_ret = lower_type(p, unit_src, None)?;
    let idx = p.push_instance(
        instance_owner,
        name.clone(),
        FnKind::Function,
        placeholder_ret,
        name,
        p.lower_span(decl_span),
    )?;
    p.instances[idx].source = Some(func_id);
    p.instances[idx].subst = inst_subst;
    p.instances[idx].depth = depth;
    p.instance_index.insert(key, idx);
    Ok(idx)
}

/// Populates Core constructor fields and [`ServiceShape`] for one monomorphized `DataInstance`.
pub(crate) fn lower_data(p: &mut Lowering<'_>, data_idx: usize) -> LowerResult<()> {
    if !p.data_instances[data_idx].ctors.is_empty() {
        return Ok(());
    }
    let source_ty = p.data_instances[data_idx].source_ty;
    let subst = p.data_instances[data_idx].subst;
    let Some(type_sym) = p.source_type(source_ty).owner.as_source() else {
        return p.refuse(ErrorCode::LowerUnfinished, "data instance missing owner");
    };
    let (choice, case_syms) = {
        let sym = p.program.interfaces.symbol(type_sym);
        let is_enum = sym.decl_kind == SyntaxKind::Enum;
        let cases: Vec<SymRef> = if is_enum {
            sym.cases
                .iter()
                .map(|&ci| SymRef::new(sym.module, ci))
                .collect()
        } else {
            vec![type_sym]
        };
        (is_enum, cases)
    };
    let record_shape = choice && !p.program.services.is_empty();
    let mut unnamed = if record_shape {
        Vec::with_capacity(case_syms.len())
    } else {
        Vec::new()
    };
    let mut ctors = Vec::with_capacity(case_syms.len());
    p.charge_alloc(case_syms.len(), 32, 8)?;

    for sym in case_syms {
        let sym_name = p.program.interfaces.symbol(sym).name.clone();
        let (field_specs, is_unnamed) = {
            let Some((_, sig)) = p.program.signatures.data_signature_by_symbol(sym) else {
                return p.refuse(ErrorCode::LowerUnfinished, "missing constructor signature");
            };
            let mut list = Vec::with_capacity(sig.fields.len());
            for j in 0..sig.fields.len() {
                let f = sig.field_by_index(j);
                list.push((f.name.clone(), f.ty));
            }
            let un = sig.fields.len() == 1 && sig.field_by_index(0).name.is_none();
            (list, un)
        };
        if record_shape {
            unnamed.push(is_unnamed);
        }
        let mut fields = Vec::with_capacity(field_specs.len());
        for (fname, fty) in field_specs {
            let core_name = match fname {
                Some(n) => p.copy_text(&n)?,
                None => p.copy_text("0")?,
            };
            let core_ty = lower_type(p, fty, subst)?;
            fields.push(p.make_param(core_name, core_ty)?);
        }
        ctors.push(CtorDecl {
            name: p.copy_text(&sym_name)?,
            fields,
        });
    }

    p.data_instances[data_idx].ctors = ctors;
    p.data_instances[data_idx].shape = ServiceShape {
        is_struct: !choice,
        unnamed,
    };
    Ok(())
}

/// Returns constructor descriptors for any algebraic Core type (nominal `Data` or built-in choice/record).
pub(crate) fn lower_ctors(p: &mut Lowering<'_>, ty: TyId) -> LowerResult<Vec<CtorDecl>> {
    if matches!(p.core_ty(ty), Ty::Data(_)) {
        let Some(&idx) = p.data_by_ty.get(&ty) else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "unknown nominal data type in lower_ctors",
            );
        };
        lower_data(p, idx)?;
        return Ok(p.data_instances[idx].ctors.clone());
    }

    let core_ty = p.core_ty(ty).clone();
    let specs: Vec<(&'static str, Vec<TyId>)> = match core_ty {
        Ty::Option(inner) => vec![("None", vec![]), ("Some", vec![inner])],
        Ty::Result { ok, err } => vec![("Ok", vec![ok]), ("Error", vec![err])],
        Ty::Ordering => vec![("Less", vec![]), ("Equal", vec![]), ("Greater", vec![])],
        Ty::EntryKind => vec![
            ("File", vec![]),
            ("Folder", vec![]),
            ("Link", vec![]),
            ("Other", vec![]),
        ],
        Ty::FileError => {
            let text_src = p.types_overlay.builtin("Text");
            let text = lower_type(p, text_src, None)?;
            vec![
                ("NotFound", vec![text]),
                ("NotAllowed", vec![text]),
                ("NotText", vec![text]),
                ("Failed", vec![text, text]),
            ]
        }
        Ty::Entry => {
            let text_src = p.types_overlay.builtin("Text");
            let ek_src = p.types_overlay.builtin("EntryKind");
            let text = lower_type(p, text_src, None)?;
            let ek = lower_type(p, ek_src, None)?;
            vec![("Entry", vec![text, ek])]
        }
        Ty::SimulationError => {
            let text_src = p.types_overlay.builtin("Text");
            let text = lower_type(p, text_src, None)?;
            vec![("ServiceDown", vec![]), ("ToolFailed", vec![text])]
        }
        Ty::NetworkError => {
            let text_src = p.types_overlay.builtin("Text");
            let int_src = p.types_overlay.builtin("Int");
            let text = lower_type(p, text_src, None)?;
            let int = lower_type(p, int_src, None)?;
            vec![
                ("UnknownHost", vec![text]),
                ("Refused", vec![text, int]),
                ("TimedOut", vec![]),
                ("Closed", vec![]),
                ("Broken", vec![text]),
                ("Certificate", vec![text]),
            ]
        }
        Ty::SqliteError => {
            let text_src = p.types_overlay.builtin("Text");
            let text = lower_type(p, text_src, None)?;
            vec![
                ("OutsideFolder", vec![text]),
                ("NotPermitted", vec![text]),
                ("SqlFailed", vec![text]),
            ]
        }
        Ty::SqliteValue => {
            let int_src = p.types_overlay.builtin("Int");
            let float_src = p.types_overlay.builtin("Float");
            let text_src = p.types_overlay.builtin("Text");
            let bytes_src = p.types_overlay.builtin("Bytes");
            let int = lower_type(p, int_src, None)?;
            let float = lower_type(p, float_src, None)?;
            let text = lower_type(p, text_src, None)?;
            let bytes = lower_type(p, bytes_src, None)?;
            vec![
                ("Null", vec![]),
                ("Integer", vec![int]),
                ("Real", vec![float]),
                ("String", vec![text]),
                ("Blob", vec![bytes]),
            ]
        }
        Ty::SqliteRows => {
            let text_src = p.types_overlay.builtin("Text");
            let val_src = p.types_overlay.builtin("Sqlite.Value");
            let list_text_src = p.types_overlay.builtin_applied("List", vec![text_src]);
            let list_val_src = p.types_overlay.builtin_applied("List", vec![val_src]);
            let list_rows_src = p.types_overlay.builtin_applied("List", vec![list_val_src]);
            let cols = lower_type(p, list_text_src, None)?;
            let rows = lower_type(p, list_rows_src, None)?;
            vec![("Rows", vec![cols, rows])]
        }
        Ty::Int
        | Ty::BigInt
        | Ty::Byte
        | Ty::Decimal
        | Ty::Float
        | Ty::Bool
        | Ty::Char
        | Ty::Text
        | Ty::Bytes
        | Ty::Unit
        | Ty::Range
        | Ty::Time
        | Ty::Duration
        | Ty::Tuple(_)
        | Ty::List(_)
        | Ty::Map { .. }
        | Ty::Set(_)
        | Ty::Data(_)
        | Ty::Simulation
        | Ty::Fn { .. }
        | Ty::Cap(_) => {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "type has no constructors in lower_ctors",
            );
        }
    };

    let mut out = Vec::with_capacity(specs.len());
    for (cname, field_tys) in specs {
        let mut fields = Vec::with_capacity(field_tys.len());
        for (j, fty) in field_tys.into_iter().enumerate() {
            let fname = p.number_text(j as u64)?;
            fields.push(p.make_param(fname, fty)?);
        }
        out.push(CtorDecl {
            name: p.copy_text(cname)?,
            fields,
        });
    }
    Ok(out)
}
