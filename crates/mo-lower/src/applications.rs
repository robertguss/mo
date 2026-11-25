//! Expression application lowering: operators, native primitives, capability/IO operations,
//! Simulation/Actor operations, ApiTest calls, Json/Sqlite builtins, direct/indirect calls,
//! tuples/lists/maps/constructors (`src/lower/applications.c` parity).

use mo_check::{
    AbilityImplementationKind, CheckedExprId, CheckedExpression, ProofId, ServiceReply,
    SourceAbility,
};
use mo_core::{
    Arm, CoreInteger, CoreNatural, Expr, ExprId, IoOp, ListOp, Lit, MapOp, Prim, SimOp, Ty, TyId,
};
use mo_def::ErrorCode;
use mo_def::core_ops::{
    CoreBase, CoreBigOp, CoreByteOp, CoreBytesOp, CoreCharOp, CoreCmpOp, CoreCryptoOp, CoreDecOp,
    CoreFloatOp, CoreIntOp, CoreIoNullaryOp, CoreListNullaryOp, CoreMapNullaryOp, CoreRangeOp,
    CoreSetOp, CoreTextOp, CoreTimeOp, CoreUnicodeOp,
};
use mo_syntax::{SyntaxKind, TokenKind};

use crate::abilities::{
    lower_compare, lower_from_json, lower_json_decode_call, lower_json_schema,
    lower_sqlite_decode_call, lower_to_json,
};
use crate::apis::{lower_apitest_run_job, lower_apitest_send, lower_apitest_start};
use crate::context::{LowerResult, UNSET_EXPR};
use crate::cps::{BodyLowerer, ContinuationKind};
use crate::scalar::{lower_error_text, lower_operator, lower_to_text};
use crate::types::{lower_instance, lower_proof};

pub(crate) fn lower_cps_kind(b: &BodyLowerer<'_, '_>, r: &CheckedExpression) -> SyntaxKind {
    if r.operation != TokenKind::Eof {
        return SyntaxKind::Binary;
    }
    let s = b.p.syntax_node(b.syntax_module, r.syntax);
    match s.kind {
        SyntaxKind::Postfix => s
            .children
            .last()
            .copied()
            .flatten()
            .map_or(s.kind, |c| b.syntax_kind(c)),
        SyntaxKind::BinaryChain => SyntaxKind::Binary,
        SyntaxKind::Prefix => SyntaxKind::Negate,
        other => other,
    }
}

pub(crate) fn lower_cps_ability(
    b: &mut BodyLowerer<'_, '_>,
    r: &CheckedExpression,
    wanted: SourceAbility,
    operand: Option<CheckedExprId>,
) -> LowerResult<ProofId> {
    for ab in &r.abilities {
        b.p.work(1)?;
        if ab.operand == operand && b.p.proof(ab.implementation).ability == wanted {
            return Ok(ab.implementation);
        }
    }
    b.p.refuse(
        ErrorCode::LowerUnfinished,
        "this operation has no selected ability proof",
    )
}

pub(crate) fn lower_cps_ability_native(r: &CheckedExpression) -> bool {
    matches!(r.native.as_deref(), Some("to_text" | "compare" | "to_json"))
}

pub(crate) fn lower_cps_direct_function(b: &BodyLowerer<'_, '_>, r: &CheckedExpression) -> bool {
    let Some(sym) = r.symbol else {
        return false;
    };
    if b.p.program.interfaces.symbol(sym).decl_kind != SyntaxKind::Fn {
        return false;
    }
    if r.operands.is_empty() {
        return true;
    }
    let callee = b.row(r.operands[0]);
    callee.operands.is_empty() && callee.symbol == Some(sym)
}

fn list_sum(b: &mut BodyLowerer<'_, '_>, number: TyId, values: ExprId) -> LowerResult<ExprId> {
    let int_src = b.p.types_overlay.builtin("Int");
    let pos_ty = b.lower_ty(int_src)?;
    let mut zero = b.p.expr_int(0)?;
    if !matches!(b.p.core_ty(number), Ty::Int) {
        let convert = match b.p.core_ty(number) {
            Ty::BigInt => CoreIntOp::ToBig,
            Ty::Decimal => CoreIntOp::ToDecimal,
            Ty::Float => CoreIntOp::ToFloat,
            _ => {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "invalid List.sum element type");
            }
        };
        zero = b.cps_unary(Prim::Int(convert), zero)?;
    }

    let list_name = b.p.fresh()?;
    let end_name = b.p.fresh()?;
    let done_k = b.alloc_cont(ContinuationKind::Jump)?;
    {
        let dn = b.p.fresh()?;
        let c = b.cont_mut(done_k);
        c.name = dn;
        c.result = true;
    }
    let (exit_join, exit_param) = b.cps_join(done_k, Some(number))?;
    let exit_param_name = exit_param.unwrap_or_default();
    let exit_body = b.p.expr_var(exit_param_name)?;
    if let Expr::Join { body, .. } = b.p.arena.expr_mut(exit_join) {
        *body = exit_body;
    }

    let again_k = b.alloc_cont(ContinuationKind::Jump)?;
    let again_pos = b.p.fresh()?;
    {
        let an = b.p.fresh()?;
        let c = b.cont_mut(again_k);
        c.name = an;
        c.result = true;
        c.position = again_pos.clone();
        c.position_type = Some(pos_ty);
    }
    let (round_join, acc_param) = b.cps_join(again_k, Some(number))?;
    let acc_name = acc_param.unwrap_or_default();

    let pos_v0 = b.p.expr_var(again_pos.clone())?;
    let end_v = b.p.expr_var(end_name.clone())?;
    let cmp_cond = b.p.expr_prim(
        Prim::Cmp {
            base: CoreBase::Int,
            op: CoreCmpOp::Lt,
        },
        vec![pos_v0, end_v],
    )?;
    let acc_v0 = b.p.expr_var(acc_name.clone())?;
    let else_jump = b.cps_jump(done_k, Some(acc_v0))?;

    let list_v1 = b.p.expr_var(list_name.clone())?;
    let pos_v1 = b.p.expr_var(again_pos.clone())?;
    let get_expr = b.p.expr_prim(
        Prim::List {
            ty: number,
            op: ListOp::Nullary(CoreListNullaryOp::Get),
        },
        vec![list_v1, pos_v1],
    )?;
    let impossible = b.p.expr_stop("unreachable list index")?;
    let elem_var = b.p.fresh()?;
    let pos_v2 = b.p.expr_var(again_pos)?;
    let one = b.p.expr_int(1)?;
    let step =
        b.p.expr_prim(Prim::Int(CoreIntOp::Add), vec![pos_v2, one])?;
    b.cont_mut(again_k).extra = Some(step);

    let acc_v1 = b.p.expr_var(acc_name)?;
    let elem_v = b.p.expr_var(elem_var.clone())?;
    let plus_op = lower_operator(b.p, TokenKind::Plus, number, Some(number))?;
    let summed = b.p.expr_prim(plus_op, vec![acc_v1, elem_v])?;
    let some_body = b.cps_jump(again_k, Some(summed))?;

    let none_ctor = b.p.copy_text("None")?;
    let some_ctor = b.p.copy_text("Some")?;
    let select = b.p.alloc_expr(Expr::Case {
        expr: get_expr,
        arms: vec![
            Arm {
                ctor: none_ctor,
                names: Vec::new(),
                body: impossible,
            },
            Arm {
                ctor: some_ctor,
                names: vec![elem_var],
                body: some_body,
            },
        ],
    })?;
    let check_if = b.p.expr_if(cmp_cond, select, else_jump)?;
    let init_pos = b.p.expr_int(0)?;
    b.cont_mut(again_k).extra = Some(init_pos);
    let init_jump = b.cps_jump(again_k, Some(zero))?;
    if let Expr::Join { body, rest, .. } = b.p.arena.expr_mut(round_join) {
        *body = check_if;
        *rest = init_jump;
    }
    if let Expr::Join { rest, .. } = b.p.arena.expr_mut(exit_join) {
        *rest = round_join;
    }

    let list_v0 = b.p.expr_var(list_name.clone())?;
    let size_expr = b.cps_unary(
        Prim::List {
            ty: number,
            op: ListOp::Nullary(CoreListNullaryOp::Size),
        },
        list_v0,
    )?;
    let size_let = b.cps_let(end_name, size_expr)?;
    if let Expr::Let { body, .. } = b.p.arena.expr_mut(size_let) {
        *body = exit_join;
    }
    let held = b.cps_let(list_name, values)?;
    if let Expr::Let { body, .. } = b.p.arena.expr_mut(held) {
        *body = size_let;
    }
    Ok(held)
}

fn project_actor_reply(
    b: &mut BodyLowerer<'_, '_>,
    reply_kind: ServiceReply,
    projected_ty: TyId,
    raw_val: ExprId,
) -> LowerResult<ExprId> {
    match reply_kind {
        ServiceReply::ReadOnly => Ok(raw_val),
        ServiceReply::Update => b.p.expr_proj(raw_val, 1),
        ServiceReply::ResultUpdate => {
            let pair_v = b.p.fresh()?;
            let err_v = b.p.fresh()?;
            let pv = b.p.expr_var(pair_v.clone())?;
            let proj = b.p.expr_proj(pv, 1)?;
            let ok_ctor = b.p.copy_text("Ok")?;
            let err_ctor = b.p.copy_text("Error")?;
            let ok_con = b.p.expr_con(projected_ty, ok_ctor.clone(), vec![proj])?;
            let ev = b.p.expr_var(err_v.clone())?;
            let err_con = b.p.expr_con(projected_ty, err_ctor.clone(), vec![ev])?;
            b.p.alloc_expr(Expr::Case {
                expr: raw_val,
                arms: vec![
                    Arm {
                        ctor: ok_ctor,
                        names: vec![pair_v],
                        body: ok_con,
                    },
                    Arm {
                        ctor: err_ctor,
                        names: vec![err_v],
                        body: err_con,
                    },
                ],
            })
        }
    }
}

fn receiver_ty_for_collection(
    b: &mut BodyLowerer<'_, '_>,
    r: &CheckedExpression,
    result: TyId,
    allow_bare_result: bool,
) -> LowerResult<TyId> {
    let mut recv = if r.operands.is_empty() {
        match b.p.core_ty(result) {
            Ty::Fn { params, .. } => params[0],
            _ if allow_bare_result => result,
            _ => result,
        }
    } else {
        let src_row = r.receiver.unwrap_or(r.operands[0]);
        b.lower_ty(b.row(src_row).ty)?
    };
    if let Ty::Fn { params, .. } = b.p.core_ty(recv) {
        recv = params[0];
    }
    Ok(recv)
}

/// Lowers an evaluated expression application row (`lower_cps_application`).
pub(crate) fn lower_cps_application(
    b: &mut BodyLowerer<'_, '_>,
    row_id: CheckedExprId,
    args: Vec<ExprId>,
) -> LowerResult<ExprId> {
    let r = b.row(row_id);
    let result = b.lower_ty(r.ty)?;

    // 1. Unary / binary operators
    if r.operation != TokenKind::Eof {
        let left = b.lower_ty(b.row(r.operands[0]).ty)?;
        let right = if args.len() == 2 {
            Some(b.lower_ty(b.row(r.operands[1]).ty)?)
        } else {
            None
        };
        if matches!(
            r.operation,
            TokenKind::Lt | TokenKind::Le | TokenKind::Gt | TokenKind::Ge
        ) {
            for ab in &r.abilities {
                b.p.work(1)?;
                if ab.operand != Some(r.operands[0])
                    || b.p.proof(ab.implementation).ability != SourceAbility::Order
                {
                    continue;
                }
                let mut proof = ab.implementation;
                let mut subst = b.subst;
                lower_proof(b.p, &mut proof, &mut subst)?;
                if b.p.proof(proof).kind == AbilityImplementationKind::Native {
                    continue;
                }
                let order_src = b.p.types_overlay.builtin("Ordering");
                let order_ty = b.lower_ty(order_src)?;
                let cmp = lower_compare(b.p, proof, subst, b.owner, b.depth, args[0], args[1])?;
                let target_ctor = if matches!(r.operation, TokenKind::Lt | TokenKind::Ge) {
                    "Less"
                } else {
                    "Greater"
                };
                let target_s = b.p.copy_text(target_ctor)?;
                let target = b.p.expr_con(order_ty, target_s, Vec::new())?;
                let mut eq = b.p.expr_prim(Prim::Eq(order_ty), vec![cmp, target])?;
                if matches!(r.operation, TokenKind::Le | TokenKind::Ge) {
                    eq = b.cps_unary(Prim::Not, eq)?;
                }
                return Ok(eq);
            }
        }
        if matches!(b.p.core_ty(left), Ty::Unit)
            && matches!(
                r.operation,
                TokenKind::Lt | TokenKind::Le | TokenKind::Gt | TokenKind::Ge
            )
        {
            return b
                .p
                .expr_bool(matches!(r.operation, TokenKind::Le | TokenKind::Ge));
        }
        let op = lower_operator(b.p, r.operation, left, right)?;
        let e = b.p.expr_prim(op, args)?;
        if r.operation == TokenKind::Ne {
            return b.p.expr_prim(Prim::Not, vec![e]);
        }
        return Ok(e);
    }

    // 2. Native / built-in operations
    if let Some(native) = r.native.as_deref() {
        // ApiTest.start / ApiTest.send / ApiTest.run_job / ApiTest.App.*
        if native == "ApiTest.start" || native == "ApiTest.send" || native == "ApiTest.run_job" {
            let arg_types: Vec<mo_check::TypeId> =
                r.operands.iter().map(|&op| b.row(op).ty).collect();
            let api_idx = r.service.map_or(0, |s| s.0 as usize);
            return if native == "ApiTest.start" {
                let caller_name = b.meta_name.clone();
                lower_apitest_start(b.p, api_idx, args, &arg_types, &caller_name)
            } else if native == "ApiTest.send" {
                lower_apitest_send(b.p, api_idx, args, &arg_types)
            } else {
                lower_apitest_run_job(b.p, api_idx, args, &arg_types)
            };
        }
        if let Some(app_op) = native.strip_prefix("ApiTest.App.") {
            let slot = match app_op {
                "send" => 0,
                "run_job" => 1,
                "routes" => 2,
                "openapi_json" => 3,
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "unknown ApiTest.App operation");
                }
            };
            let func = b.p.expr_proj(args[0], slot)?;
            let call_args = args[1..].to_vec();
            b.p.charge_alloc(call_args.len(), 8, 8)?;
            return b.p.alloc_expr(Expr::App {
                func,
                args: call_args,
            });
        }

        // Simulation.*
        if let Some(member) = native.strip_prefix("Simulation.") {
            let desc = b.p.number_text(u64::from(r.service.map_or(0, |s| s.0)))?;
            let tool = b.p.number_text(u64::from(r.tool.unwrap_or(0)))?;
            let sim_op = match member {
                "start" => SimOp::Start {
                    descriptor: CoreNatural { digits: desc },
                },
                "send" => {
                    let mut arg_tys = Vec::with_capacity(r.operands.len().saturating_sub(1));
                    for &op in r.operands.iter().skip(1) {
                        arg_tys.push(b.lower_ty(b.row(op).ty)?);
                    }
                    let ok_src = b.p.source_type(r.ty).arguments[0];
                    let res_ty = b.lower_ty(ok_src)?;
                    SimOp::Send {
                        descriptor: CoreNatural { digits: desc },
                        tool: CoreNatural { digits: tool },
                        arguments: arg_tys,
                        result: res_ty,
                    }
                }
                "advance" => SimOp::Advance,
                "crash" => SimOp::Crash,
                "restart" => SimOp::Restart,
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "unknown Simulation operation");
                }
            };
            return b.p.expr_prim(Prim::Sim(sim_op), args);
        }

        // Actor.*
        if let Some(member) = native.strip_prefix("Actor.") {
            match member {
                "start" | "start_at" => {
                    let desc = b.p.number_text(u64::from(r.service.map_or(0, |s| s.0)))?;
                    let sim_op = SimOp::Start {
                        descriptor: CoreNatural { digits: desc },
                    };
                    let sargs = if member == "start" {
                        b.p.charge_alloc(1, 40, 8)?;
                        let digits = b.p.copy_text("1767225600000000")?;
                        let tlit = b.p.alloc_expr(Expr::Lit(Lit::Time(CoreInteger {
                            digits,
                            negative: false,
                        })))?;
                        vec![tlit]
                    } else {
                        args
                    };
                    return b.p.expr_prim(Prim::Sim(sim_op), sargs);
                }
                "advance" | "crash" | "restart" => {
                    let sim_op = match member {
                        "advance" => SimOp::Advance,
                        "crash" => SimOp::Crash,
                        _ => SimOp::Restart,
                    };
                    return b.p.expr_prim(Prim::Sim(sim_op), args);
                }
                "call" | "send" => {
                    let is_send = member == "send";
                    let svc_idx = r.service.map_or(0, |s| s.0 as usize);
                    let tool_idx = r.tool.unwrap_or(0) as usize;
                    let (tool_reply, fn_sig_args) = {
                        let tool = &b.p.program.services[svc_idx].tools[tool_idx];
                        let fn_id = tool.function;
                        let sig_id = b.p.program.function(fn_id).signature;
                        let fn_ty = b.p.program.signatures.functions[sig_id.index()].ty;
                        let Some(ft) = fn_ty else {
                            return b.p.refuse(
                                ErrorCode::LowerUnfinished,
                                "actor tool missing signature",
                            );
                        };
                        (tool.reply, b.p.source_type(ft).arguments.clone())
                    };
                    let raw_ret_src = fn_sig_args[fn_sig_args.len() - 1];
                    let desc = b.p.number_text(svc_idx as u64)?;
                    let tool_num = b.p.number_text(tool_idx as u64)?;
                    let mut sim_arg_tys = Vec::with_capacity(args.len().saturating_sub(1));
                    for i in 0..args.len().saturating_sub(1) {
                        sim_arg_tys.push(b.lower_ty(fn_sig_args[i + 2])?);
                    }
                    let raw_ret_ty = b.lower_ty(raw_ret_src)?;
                    let sent = b.p.expr_prim(
                        Prim::Sim(SimOp::Send {
                            descriptor: CoreNatural { digits: desc },
                            tool: CoreNatural { digits: tool_num },
                            arguments: sim_arg_tys,
                            result: raw_ret_ty,
                        }),
                        args,
                    )?;
                    let proj_ty = if is_send {
                        match *b.p.core_ty(result) {
                            Ty::Result { ok, .. } => ok,
                            _ => result,
                        }
                    } else {
                        result
                    };
                    let raw_v = b.p.fresh()?;
                    let sim_err_v = b.p.fresh()?;
                    let rv_e = b.p.expr_var(raw_v.clone())?;
                    let projected = project_actor_reply(b, tool_reply, proj_ty, rv_e)?;
                    let ok_ctor = b.p.copy_text("Ok")?;
                    let err_ctor = b.p.copy_text("Error")?;
                    let ok_body = if is_send {
                        b.p.expr_con(result, ok_ctor.clone(), vec![projected])?
                    } else {
                        projected
                    };
                    let err_body = if is_send {
                        let sev = b.p.expr_var(sim_err_v.clone())?;
                        b.p.expr_con(result, err_ctor.clone(), vec![sev])?
                    } else {
                        let sev = b.p.expr_var(sim_err_v.clone())?;
                        let down_stop = b.p.expr_stop("actor is stopped")?;
                        let fail_stop = b.p.expr_stop("actor handler failed")?;
                        let down_ctor = b.p.copy_text("ServiceDown")?;
                        let fail_ctor = b.p.copy_text("ToolFailed")?;
                        b.p.alloc_expr(Expr::Case {
                            expr: sev,
                            arms: vec![
                                Arm {
                                    ctor: down_ctor,
                                    names: Vec::new(),
                                    body: down_stop,
                                },
                                Arm {
                                    ctor: fail_ctor,
                                    names: Vec::new(),
                                    body: fail_stop,
                                },
                            ],
                        })?
                    };
                    return b.p.alloc_expr(Expr::Case {
                        expr: sent,
                        arms: vec![
                            Arm {
                                ctor: ok_ctor,
                                names: vec![raw_v],
                                body: ok_body,
                            },
                            Arm {
                                ctor: err_ctor,
                                names: vec![sim_err_v],
                                body: err_body,
                            },
                        ],
                    });
                }
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "unknown Actor operation");
                }
            }
        }

        // Direct primitive tables
        if let Some(prim) = match_static_prim(native) {
            return b.p.expr_prim(prim, args);
        }

        if native == "List.sum" {
            let elem = match *b.p.core_ty(result) {
                Ty::Fn { ret, .. } => ret,
                _ => result,
            };
            return list_sum(b, elem, args[0]);
        }
        if native == "List.zip" {
            let zipped = match *b.p.core_ty(result) {
                Ty::Fn { ret, .. } => ret,
                _ => result,
            };
            let (t0, t1) = match b.p.core_ty(zipped) {
                Ty::List(inner) => match b.p.core_ty(*inner) {
                    Ty::Tuple(parts) if parts.len() == 2 => (parts[0], parts[1]),
                    _ => {
                        return b
                            .p
                            .refuse(ErrorCode::LowerUnfinished, "invalid List.zip result");
                    }
                },
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "invalid List.zip result");
                }
            };
            return b.p.expr_prim(
                Prim::List {
                    ty: t0,
                    op: ListOp::Zip(t1),
                },
                args,
            );
        }
        if let Some(lop) = match_list_op(native) {
            let recv = receiver_ty_for_collection(b, r, result, false)?;
            let elem = match *b.p.core_ty(recv) {
                Ty::List(e) => e,
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "expected List receiver");
                }
            };
            return b.p.expr_prim(
                Prim::List {
                    ty: elem,
                    op: ListOp::Nullary(lop),
                },
                args,
            );
        }
        if native == "List.to_map" {
            let map_ty = match *b.p.core_ty(result) {
                Ty::Fn { ret, .. } => ret,
                _ => result,
            };
            let (k, v) = match *b.p.core_ty(map_ty) {
                Ty::Map { key, value } => (key, value),
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "expected Map result");
                }
            };
            return b.p.expr_prim(
                Prim::Map {
                    key: k,
                    value: v,
                    op: MapOp::Nullary(CoreMapNullaryOp::OfList),
                },
                args,
            );
        }
        if native == "List.to_set" {
            let set_ty = match *b.p.core_ty(result) {
                Ty::Fn { ret, .. } => ret,
                _ => result,
            };
            let elem = match *b.p.core_ty(set_ty) {
                Ty::Set(e) => e,
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "expected Set result");
                }
            };
            return b.p.expr_prim(
                Prim::Set {
                    ty: elem,
                    op: CoreSetOp::OfList,
                },
                args,
            );
        }
        if let Some(mop) = match_map_op(native) {
            let recv = receiver_ty_for_collection(b, r, result, false)?;
            let (k, v) = match *b.p.core_ty(recv) {
                Ty::Map { key, value } => (key, value),
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "expected Map receiver");
                }
            };
            return b.p.expr_prim(
                Prim::Map {
                    key: k,
                    value: v,
                    op: MapOp::Nullary(mop),
                },
                args,
            );
        }
        if let Some(sop) = match_set_op(native) {
            let recv = receiver_ty_for_collection(b, r, result, true)?;
            let elem = match *b.p.core_ty(recv) {
                Ty::Set(e) => e,
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "expected Set receiver");
                }
            };
            return b.p.expr_prim(Prim::Set { ty: elem, op: sop }, args);
        }
        if native.ends_with(".to_text") {
            let recv = if r.operands.is_empty() {
                match b.p.core_ty(result) {
                    Ty::Fn { params, .. } => params[0],
                    _ => result,
                }
            } else {
                b.lower_ty(b.row(r.operands[r.operands.len() - 1]).ty)?
            };
            return lower_error_text(b.p, recv, args[0]);
        }

        let proved = if r.operands.is_empty() {
            Some(row_id)
        } else {
            Some(r.operands[0])
        };
        if native == "to_text" {
            let pr = lower_cps_ability(b, r, SourceAbility::Text, proved)?;
            return lower_to_text(b.p, pr, b.subst, b.owner, b.depth, args[0]);
        }
        if native == "compare" {
            let pr = lower_cps_ability(b, r, SourceAbility::Order, proved)?;
            return lower_compare(b.p, pr, b.subst, b.owner, b.depth, args[0], args[1]);
        }
        if native == "to_json" {
            let pr = lower_cps_ability(b, r, SourceAbility::Json, proved)?;
            return lower_to_json(b.p, pr, b.subst, b.owner, b.depth, args[0]);
        }
        if matches!(
            native,
            "Entry.name" | "Entry.kind" | "Sqlite.Rows.columns" | "Sqlite.Rows.rows"
        ) {
            let field = r.field.unwrap_or(0);
            let base = b.lower_ty(b.row(r.operands[0]).ty)?;
            return b.cps_field_read(base, args[0], field);
        }
        if !is_builtin_ctor_native(native) {
            return b.p.refuse(
                ErrorCode::LowerUnfinished,
                "this native call is not lowered yet",
            );
        }
    }

    // 3. Tuples, field reads, lists, maps
    let kind = lower_cps_kind(b, r);
    if matches!(kind, SyntaxKind::Tuple | SyntaxKind::MapEntry) {
        return b.p.expr_tuple(args);
    }
    if let Some(field) = r.field
        && r.operands.len() == 1
        && kind != SyntaxKind::NamedArg
    {
        let base = b.lower_ty(b.row(r.operands[0]).ty)?;
        return b.cps_field_read(base, args[0], field);
    }
    if kind == SyntaxKind::List {
        let elem = match *b.p.core_ty(result) {
            Ty::List(e) => e,
            _ => {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "expected List literal type");
            }
        };
        return b.p.expr_lit_list(elem, args);
    }
    if kind == SyntaxKind::Map {
        let (k, v) = match *b.p.core_ty(result) {
            Ty::Map { key, value } => (key, value),
            _ => {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "expected Map literal type");
            }
        };
        return b.p.expr_lit_map(k, v, args);
    }

    // 4. Direct function calls (including Json.* and Sqlite.decode* compiler hooks)
    if lower_cps_direct_function(b, r) {
        let Some(sym) = r.symbol else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "direct function missing symbol");
        };
        let (is_lib, mod_name, fn_name, recv_name) = {
            let scope = &b.p.program.interfaces.scopes[sym.module.index()];
            let s = &scope.symbols[sym.symbol.index()];
            let rn = s.receiver.map(|ri| scope.symbols[ri.index()].name.clone());
            (scope.library, scope.name.clone(), s.name.clone(), rn)
        };
        if is_lib && mod_name.as_deref() == Some("Json") && r.arguments.len() == 1 {
            if recv_name.as_deref() == Some("Json")
                && matches!(fn_name.as_str(), "from_json" | "from_json_strict")
            {
                let empty_path = b.p.expr_text("")?;
                let strict = b.p.expr_bool(fn_name == "from_json_strict")?;
                let pr = lower_cps_ability(b, r, SourceAbility::Json, None)?;
                return lower_from_json(
                    b.p, pr, b.subst, b.owner, b.depth, args[0], empty_path, strict,
                );
            }
            if recv_name.is_none() && matches!(fn_name.as_str(), "decode" | "decode_strict") {
                let input_ty = if r.operands.is_empty() {
                    match b.p.core_ty(result) {
                        Ty::Fn { params, .. } => params[0],
                        _ => result,
                    }
                } else {
                    b.lower_ty(b.row(r.operands[r.operands.len() - 1]).ty)?
                };
                let pr = lower_cps_ability(b, r, SourceAbility::Json, None)?;
                return lower_json_decode_call(
                    b.p,
                    pr,
                    b.subst,
                    b.owner,
                    b.depth,
                    input_ty,
                    args[0],
                    fn_name == "decode_strict",
                );
            }
            if recv_name.is_none() && fn_name == "schema" {
                let text_src = b.p.types_overlay.builtin("Text");
                let text_ty = b.lower_ty(text_src)?;
                let seen = b.p.expr_lit_list(text_ty, Vec::new())?;
                let pr = lower_cps_ability(b, r, SourceAbility::Json, None)?;
                return lower_json_schema(b.p, pr, b.subst, b.owner, b.depth, seen);
            }
        }
        if is_lib
            && mod_name.as_deref() == Some("Sqlite")
            && r.arguments.len() == 1
            && recv_name.is_none()
            && matches!(
                fn_name.as_str(),
                "decode" | "decode_one" | "decode_optional"
            )
        {
            let target_src = r.arguments[0].ty;
            let caller_name = b.meta_name.clone();
            return lower_sqlite_decode_call(
                b.p,
                target_src,
                &r.abilities,
                b.subst,
                b.owner,
                b.depth,
                &caller_name,
                &fn_name,
                args[0],
            );
        }

        if is_lib
            && mod_name.as_deref() == Some("Network")
            && matches!(fn_name.as_str(), "fake_scripted" | "recorded_calls")
        {
            let helper_name = if fn_name == "fake_scripted" {
                "encode_scripts"
            } else {
                "decode_recorded_calls"
            };
            let Some(helper_idx) = b.p.program.interfaces.scopes[sym.module.index()]
                .find_symbol_index(helper_name.as_bytes())
            else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "missing Network helper symbol");
            };
            let helper_sym = mo_check::SymRef::new(sym.module, helper_idx);
            let helper_inst = lower_instance(b.p, helper_sym, &[], None, &[], b.owner, b.depth)?;
            let helper_fn = b.p.instances[helper_inst].fun.name.clone();
            let io_prim = if fn_name == "fake_scripted" {
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::NetworkFakeScripted))
            } else {
                Prim::Io(IoOp::Nullary(CoreIoNullaryOp::NetworkRecordedCalls))
            };
            if r.operands.is_empty() {
                let param_ty = match b.p.core_ty(result) {
                    Ty::Fn { params, .. } if params.len() == 1 => params[0],
                    _ => {
                        return b.p.refuse(
                            ErrorCode::LowerUnfinished,
                            "expected unary function type for Network spec operation",
                        );
                    }
                };
                let p_name = b.p.fresh()?;
                let pv = b.p.expr_var(p_name.clone())?;
                let body = if fn_name == "fake_scripted" {
                    let encoded = b.p.expr_call(helper_fn, vec![pv])?;
                    b.p.expr_prim(io_prim, vec![encoded])?
                } else {
                    let raw = b.p.expr_prim(io_prim, vec![pv])?;
                    b.p.expr_call(helper_fn, vec![raw])?
                };
                return b.p.alloc_expr(Expr::Lam {
                    params: vec![mo_core::Param {
                        name: p_name,
                        ty: param_ty,
                    }],
                    body,
                });
            }
            let arg_expr = args[args.len() - 1];
            return if fn_name == "fake_scripted" {
                let encoded = b.p.expr_call(helper_fn, vec![arg_expr])?;
                b.p.expr_prim(io_prim, vec![encoded])
            } else {
                let raw = b.p.expr_prim(io_prim, vec![arg_expr])?;
                b.p.expr_call(helper_fn, vec![raw])
            };
        }

        let next_depth = b.depth + u32::from(!r.arguments.is_empty());
        let inst = lower_instance(
            b.p,
            sym,
            &r.arguments,
            b.subst,
            &r.abilities,
            b.owner,
            next_depth,
        )?;
        let fname = b.p.instances[inst].fun.name.clone();
        return b.p.expr_call(fname, args);
    }

    // 5. Constructors (user struct/enum case or built-in constructor)
    let is_user_ctor = r.symbol.is_some_and(|s| {
        let dk = b.p.program.interfaces.symbol(s).decl_kind;
        dk != SyntaxKind::Const && dk != SyntaxKind::Fn
    });
    let is_builtin_ctor = r.native.as_deref().is_some_and(|n| !n.contains('.'));
    if is_user_ctor || is_builtin_ctor {
        let ctor_name = match r.symbol {
            Some(s) => b.p.copy_text(&b.p.program.interfaces.symbol(s).name)?,
            None => b.p.copy_text(r.native.as_deref().unwrap_or(""))?,
        };
        let mut ordered = vec![UNSET_EXPR; args.len()];
        for (i, &arg_e) in args.iter().enumerate() {
            let op_row = b.row(r.operands[i]);
            let slot = if lower_cps_kind(b, op_row) == SyntaxKind::NamedArg {
                op_row.field.unwrap_or(i as u32) as usize
            } else {
                i
            };
            ordered[slot] = arg_e;
        }
        return b.p.expr_con(result, ctor_name, ordered);
    }

    // 6. Indirect call (closure application)
    if kind == SyntaxKind::Call {
        let func = args[0];
        let call_args = args[1..].to_vec();
        b.p.charge_alloc(call_args.len(), 8, 8)?;
        return b.p.alloc_expr(Expr::App {
            func,
            args: call_args,
        });
    }

    b.p.refuse(
        ErrorCode::LowerUnfinished,
        "this checked operation is not lowered yet",
    )
}

fn match_static_prim(native: &str) -> Option<Prim> {
    match native {
        // Int
        "Int.abs" => Some(Prim::Int(CoreIntOp::Abs)),
        "Int.min" => Some(Prim::Int(CoreIntOp::Min)),
        "Int.max" => Some(Prim::Int(CoreIntOp::Max)),
        "Int.pow" => Some(Prim::Int(CoreIntOp::Pow)),
        "Int.floor_div" => Some(Prim::Int(CoreIntOp::FloorDiv)),
        "Int.floor_mod" => Some(Prim::Int(CoreIntOp::FloorMod)),
        "Int.wrapping_add" => Some(Prim::Int(CoreIntOp::WrappingAdd)),
        "Int.wrapping_sub" => Some(Prim::Int(CoreIntOp::WrappingSub)),
        "Int.wrapping_mul" => Some(Prim::Int(CoreIntOp::WrappingMul)),
        "Int.saturating_add" => Some(Prim::Int(CoreIntOp::SaturatingAdd)),
        "Int.saturating_sub" => Some(Prim::Int(CoreIntOp::SaturatingSub)),
        "Int.saturating_mul" => Some(Prim::Int(CoreIntOp::SaturatingMul)),
        "Int.checked_add" => Some(Prim::Int(CoreIntOp::CheckedAdd)),
        "Int.checked_sub" => Some(Prim::Int(CoreIntOp::CheckedSub)),
        "Int.checked_mul" => Some(Prim::Int(CoreIntOp::CheckedMul)),
        "Int.to_decimal" => Some(Prim::Int(CoreIntOp::ToDecimal)),
        "Int.to_float" => Some(Prim::Int(CoreIntOp::ToFloat)),
        "Int.to_big" => Some(Prim::Int(CoreIntOp::ToBig)),
        "Int.to_byte" => Some(Prim::Int(CoreIntOp::ToByte)),
        "Int.to_char" => Some(Prim::Int(CoreIntOp::ToChar)),
        "Int.seconds" => Some(Prim::Int(CoreIntOp::Seconds)),
        "Int.milliseconds" => Some(Prim::Int(CoreIntOp::Milliseconds)),
        "Int.minutes" => Some(Prim::Int(CoreIntOp::Minutes)),
        "Int.hours" => Some(Prim::Int(CoreIntOp::Hours)),
        "Int.days" => Some(Prim::Int(CoreIntOp::Days)),
        // BigInt
        "BigInt.abs" => Some(Prim::Big(CoreBigOp::Abs)),
        "BigInt.min" => Some(Prim::Big(CoreBigOp::Min)),
        "BigInt.max" => Some(Prim::Big(CoreBigOp::Max)),
        "BigInt.pow" => Some(Prim::Big(CoreBigOp::Pow)),
        "BigInt.floor_div" => Some(Prim::Big(CoreBigOp::FloorDiv)),
        "BigInt.floor_mod" => Some(Prim::Big(CoreBigOp::FloorMod)),
        "BigInt.to_int" => Some(Prim::Big(CoreBigOp::ToInt)),
        // Float
        "Float.abs" => Some(Prim::Float(CoreFloatOp::Abs)),
        "Float.floor" => Some(Prim::Float(CoreFloatOp::Floor)),
        "Float.ceil" => Some(Prim::Float(CoreFloatOp::Ceil)),
        "Float.round" => Some(Prim::Float(CoreFloatOp::Round)),
        "Float.sqrt" => Some(Prim::Float(CoreFloatOp::Sqrt)),
        "Float.nan?" => Some(Prim::Float(CoreFloatOp::IsNan)),
        "Float.infinite?" => Some(Prim::Float(CoreFloatOp::IsInfinite)),
        "Float.to_decimal" => Some(Prim::Float(CoreFloatOp::ToDecimal)),
        // Byte
        "Byte.to_int" => Some(Prim::Byte(CoreByteOp::ToInt)),
        // Bytes
        "Bytes.size" => Some(Prim::Bytes(CoreBytesOp::Size)),
        "Bytes.get" => Some(Prim::Bytes(CoreBytesOp::Get)),
        "Bytes.decode_utf8" => Some(Prim::Bytes(CoreBytesOp::DecodeUtf8)),
        "Bytes.slice" => Some(Prim::Bytes(CoreBytesOp::Slice)),
        "Bytes.join" => Some(Prim::Bytes(CoreBytesOp::Join)),
        "Bytes.to_hex" => Some(Prim::Bytes(CoreBytesOp::ToHex)),
        "Bytes.from_hex" => Some(Prim::Bytes(CoreBytesOp::FromHex)),
        "Bytes.to_base64url" => Some(Prim::Bytes(CoreBytesOp::ToBase64url)),
        "Bytes.from_base64url" => Some(Prim::Bytes(CoreBytesOp::FromBase64url)),
        "Bytes.empty" => Some(Prim::Bytes(CoreBytesOp::Empty)),
        "Bytes.zeroed" => Some(Prim::Bytes(CoreBytesOp::Zeroed)),
        "Bytes.from_list" => Some(Prim::Bytes(CoreBytesOp::FromList)),
        "Bytes.to_list" => Some(Prim::Bytes(CoreBytesOp::ToList)),
        "Bytes.from_u16_be" => Some(Prim::Bytes(CoreBytesOp::FromU16Be)),
        "Bytes.from_u32_be" => Some(Prim::Bytes(CoreBytesOp::FromU32Be)),
        "Bytes.from_i64_be" => Some(Prim::Bytes(CoreBytesOp::FromI64Be)),
        "Bytes.u16_be_at" => Some(Prim::Bytes(CoreBytesOp::U16BeAt)),
        "Bytes.u32_be_at" => Some(Prim::Bytes(CoreBytesOp::U32BeAt)),
        "Bytes.i64_be_at" => Some(Prim::Bytes(CoreBytesOp::I64BeAt)),
        "Bytes.empty?" => Some(Prim::Bytes(CoreBytesOp::IsEmpty)),
        "Bytes.compare_slice_at" => Some(Prim::Bytes(CoreBytesOp::CompareSliceAt)),
        // Range
        "Range.to_list" => Some(Prim::Range(CoreRangeOp::ToList)),
        "Range.contains?" => Some(Prim::Range(CoreRangeOp::Contains)),
        // Decimal
        "Decimal.abs" => Some(Prim::Dec(CoreDecOp::Abs)),
        "Decimal.min" => Some(Prim::Dec(CoreDecOp::Min)),
        "Decimal.max" => Some(Prim::Dec(CoreDecOp::Max)),
        "Decimal.floor" => Some(Prim::Dec(CoreDecOp::Floor)),
        "Decimal.ceil" => Some(Prim::Dec(CoreDecOp::Ceil)),
        "Decimal.round" => Some(Prim::Dec(CoreDecOp::Round)),
        "Decimal.to_int" => Some(Prim::Dec(CoreDecOp::ToInt)),
        "Decimal.to_fixed" => Some(Prim::Dec(CoreDecOp::ToFixed)),
        "Decimal.to_float" => Some(Prim::Dec(CoreDecOp::ToFloat)),
        // Time / Duration
        "Time.checked_add" => Some(Prim::Time(CoreTimeOp::CheckedAdd)),
        "Time.parse" => Some(Prim::Time(CoreTimeOp::Parse)),
        "Duration.in_milliseconds" => Some(Prim::Time(CoreTimeOp::InMilliseconds)),
        "Duration.in_seconds" => Some(Prim::Time(CoreTimeOp::InSeconds)),
        // Unicode16
        "Unicode16.graphemes" => Some(Prim::Unicode16(CoreUnicodeOp::Graphemes)),
        "Unicode16.display_width" => Some(Prim::Unicode16(CoreUnicodeOp::DisplayWidth)),
        "Unicode16.normalize_nfc" => Some(Prim::Unicode16(CoreUnicodeOp::NormalizeNfc)),
        "Unicode16.lowercase" => Some(Prim::Unicode16(CoreUnicodeOp::Lowercase)),
        "Unicode16.uppercase" => Some(Prim::Unicode16(CoreUnicodeOp::Uppercase)),
        "Unicode16.letter?" => Some(Prim::Unicode16(CoreUnicodeOp::Letter)),
        "Unicode16.digit?" => Some(Prim::Unicode16(CoreUnicodeOp::Digit)),
        // Crypto
        "Crypto.sha256" => Some(Prim::Crypto(CoreCryptoOp::Sha256)),
        "Crypto.hmac_sha256" => Some(Prim::Crypto(CoreCryptoOp::HmacSha256)),
        "Crypto.constant_time_eq?" => Some(Prim::Crypto(CoreCryptoOp::ConstantTimeEq)),
        "Crypto.seal_raw" => Some(Prim::Crypto(CoreCryptoOp::SealRaw)),
        "Crypto.open_raw" => Some(Prim::Crypto(CoreCryptoOp::OpenRaw)),
        "Crypto.hash_password_raw" => Some(Prim::Crypto(CoreCryptoOp::HashPasswordRaw)),
        "Crypto.verify_password" => Some(Prim::Crypto(CoreCryptoOp::VerifyPassword)),
        // Char
        "Char.code" => Some(Prim::Char(CoreCharOp::Code)),
        "Char.ascii_digit?" => Some(Prim::Char(CoreCharOp::IsDigit)),
        "Char.ascii_letter?" => Some(Prim::Char(CoreCharOp::IsLetter)),
        "Char.ascii_white_space?" => Some(Prim::Char(CoreCharOp::IsWhiteSpace)),
        "Char.ascii_lowercase" => Some(Prim::Char(CoreCharOp::Lower)),
        "Char.ascii_uppercase" => Some(Prim::Char(CoreCharOp::Upper)),
        "Char.to_text" => Some(Prim::Char(CoreCharOp::ToText)),
        // Text + List.join
        "List.join" => Some(Prim::Text(CoreTextOp::Join)),
        "Text.bytes" => Some(Prim::Text(CoreTextOp::Bytes)),
        "Text.byte_count" => Some(Prim::Text(CoreTextOp::ByteCount)),
        "Text.char_count" => Some(Prim::Text(CoreTextOp::CharCount)),
        "Text.empty?" => Some(Prim::Text(CoreTextOp::IsEmpty)),
        "Text.chars" => Some(Prim::Text(CoreTextOp::Chars)),
        "Text.from_chars" => Some(Prim::Text(CoreTextOp::FromChars)),
        "Text.words" => Some(Prim::Text(CoreTextOp::Words)),
        "Text.split" => Some(Prim::Text(CoreTextOp::Split)),
        "Text.trim" => Some(Prim::Text(CoreTextOp::Trim)),
        "Text.trim_start" => Some(Prim::Text(CoreTextOp::TrimStart)),
        "Text.trim_end" => Some(Prim::Text(CoreTextOp::TrimEnd)),
        "Text.pad_left" => Some(Prim::Text(CoreTextOp::PadLeft)),
        "Text.pad_right" => Some(Prim::Text(CoreTextOp::PadRight)),
        "Text.starts_with?" => Some(Prim::Text(CoreTextOp::StartsWith)),
        "Text.ends_with?" => Some(Prim::Text(CoreTextOp::EndsWith)),
        "Text.drop_prefix" => Some(Prim::Text(CoreTextOp::DropPrefix)),
        "Text.drop_suffix" => Some(Prim::Text(CoreTextOp::DropSuffix)),
        "Text.replace" => Some(Prim::Text(CoreTextOp::Replace)),
        "Text.repeat" => Some(Prim::Text(CoreTextOp::Repeat)),
        "Text.lines" => Some(Prim::Text(CoreTextOp::Lines)),
        "Text.contains?" => Some(Prim::Text(CoreTextOp::Contains)),
        "Text.ascii_lowercase" => Some(Prim::Text(CoreTextOp::Lower)),
        "Text.ascii_uppercase" => Some(Prim::Text(CoreTextOp::Upper)),
        "Text.to_int" => Some(Prim::Text(CoreTextOp::ToInt)),
        "Text.to_decimal" => Some(Prim::Text(CoreTextOp::ToDecimal)),
        "Text.to_float" => Some(Prim::Text(CoreTextOp::ToFloat)),
        // IO capability operations
        "Console.print" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Print))),
        "Console.error" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::PrintError))),
        "Console.read_line" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadLine))),
        "Args.list" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ArgsList))),
        "Args.of" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ArgsOf))),
        "Env.get" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::EnvGet))),
        "Env.of" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::EnvOf))),
        "Console.fake" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ConsoleFake))),
        "Console.fake_input" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ConsoleFakeInput))),
        "Console.printed" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Printed))),
        "Console.printed_errors" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::PrintedErrors))),
        "Files.in_memory" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FilesInMemory))),
        "Files.read_only" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadOnly))),
        "Files.read_text" => Some(Prim::Io(IoOp::ReadText { ro: false })),
        "Files.write_text" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::WriteText))),
        "Files.read_bytes" => Some(Prim::Io(IoOp::ReadBytes { ro: false })),
        "Files.append_text" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::AppendText))),
        "Files.remove" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Remove))),
        "Files.make_folder" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::MakeFolder))),
        "Files.list" => Some(Prim::Io(IoOp::List { ro: false })),
        "Files.entries" => Some(Prim::Io(IoOp::Entries { ro: false })),
        "Files.exists?" => Some(Prim::Io(IoOp::Exists { ro: false })),
        "Files.within" => Some(Prim::Io(IoOp::Within { ro: false })),
        "ReadFiles.read_text" => Some(Prim::Io(IoOp::ReadText { ro: true })),
        "ReadFiles.read_bytes" => Some(Prim::Io(IoOp::ReadBytes { ro: true })),
        "ReadFiles.list" => Some(Prim::Io(IoOp::List { ro: true })),
        "ReadFiles.entries" => Some(Prim::Io(IoOp::Entries { ro: true })),
        "ReadFiles.exists?" => Some(Prim::Io(IoOp::Exists { ro: true })),
        "ReadFiles.within" => Some(Prim::Io(IoOp::Within { ro: true })),
        "Clock.now" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Now))),
        "Clock.sleep" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Sleep))),
        "Clock.fixed" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ClockFixed))),
        "Random.int" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::RandomInt))),
        "Random.seeded" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::RandomSeeded))),
        "Network.connect" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Connect))),
        "Network.connect_secure" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ConnectSecure))),
        "Network.allow_hosts" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::AllowHosts))),
        "Network.fake" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::NetworkFake))),
        "Network.fake_scripted_raw" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::NetworkFakeScripted,
        ))),
        "Network.recorded_calls_raw" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::NetworkRecordedCalls,
        ))),
        "Connection.send" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Send))),
        "Connection.receive" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Receive))),
        "Sqlite.open" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteOpen))),
        "Sqlite.fake" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteFake))),
        "Sqlite.run" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteRun))),
        "Sqlite.query" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteQuery))),
        "Sqlite.execute_raw" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteExecuteRaw))),
        "Sqlite.stats_raw" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SqliteStatsRaw))),
        "Sqlite.reset_stats_raw" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::SqliteResetStatsRaw,
        ))),
        "Command.run_raw" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::CommandRunRaw))),
        "Command.within" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::CommandWithin))),
        "Command.allow_programs" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::CommandAllowPrograms,
        ))),
        "Command.fake" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::CommandFake))),
        "Command.allow_loopback" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::CommandAllowLoopback,
        ))),
        "Command.spawn_raw" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::CommandSpawnRaw))),
        "Command.pid" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::CommandProcessPid))),
        "Command.alive?" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::CommandProcessAlive,
        ))),
        "Command.kill_raw" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::CommandProcessKillRaw,
        ))),
        "Command.wait_raw" => Some(Prim::Io(IoOp::Nullary(
            CoreIoNullaryOp::CommandProcessWaitRaw,
        ))),
        "SecureRandom.bytes" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SecureRandomBytes))),
        "SecureRandom.seeded" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::SecureRandomSeeded))),
        "Files.write_bytes" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::WriteBytes))),
        "Files.rename" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::Rename))),
        "Files.open" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileOpen))),
        "Files.read_at" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileReadAt))),
        "Files.write_at" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileWriteAt))),
        "Files.size" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileSize))),
        "Files.truncate" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileTruncate))),
        "Files.sync" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileSync))),
        "Files.lock_shared" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileLockShared))),
        "Files.lock_exclusive" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileLockExclusive))),
        "Files.unlock" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::FileUnlock))),
        "ReadFiles.open" => Some(Prim::Io(IoOp::Nullary(CoreIoNullaryOp::ReadFileOpen))),
        _ => None,
    }
}

fn match_list_op(native: &str) -> Option<CoreListNullaryOp> {
    match native {
        "List.first" => Some(CoreListNullaryOp::First),
        "List.last" => Some(CoreListNullaryOp::Last),
        "List.get" => Some(CoreListNullaryOp::Get),
        "List.size" => Some(CoreListNullaryOp::Size),
        "List.empty?" => Some(CoreListNullaryOp::IsEmpty),
        "List.append" => Some(CoreListNullaryOp::Append),
        "List.prepend" => Some(CoreListNullaryOp::Prepend),
        "List.concat" => Some(CoreListNullaryOp::Concat),
        "List.reverse" => Some(CoreListNullaryOp::Reverse),
        "List.take" => Some(CoreListNullaryOp::Take),
        "List.drop" => Some(CoreListNullaryOp::Drop),
        "List.set" => Some(CoreListNullaryOp::Set),
        "List.remove_at" => Some(CoreListNullaryOp::RemoveAt),
        "List.enumerate" => Some(CoreListNullaryOp::Enumerate),
        "List.contains?" => Some(CoreListNullaryOp::Contains),
        "List.unique" => Some(CoreListNullaryOp::Unique),
        _ => None,
    }
}

fn match_map_op(native: &str) -> Option<CoreMapNullaryOp> {
    match native {
        "Map.get" => Some(CoreMapNullaryOp::Get),
        "Map.set" => Some(CoreMapNullaryOp::Set),
        "Map.remove" => Some(CoreMapNullaryOp::Remove),
        "Map.size" => Some(CoreMapNullaryOp::Size),
        "Map.empty?" => Some(CoreMapNullaryOp::IsEmpty),
        "Map.has?" => Some(CoreMapNullaryOp::Has),
        "Map.keys" => Some(CoreMapNullaryOp::Keys),
        "Map.values" => Some(CoreMapNullaryOp::Values),
        "Map.entries" => Some(CoreMapNullaryOp::Entries),
        _ => None,
    }
}

fn match_set_op(native: &str) -> Option<CoreSetOp> {
    match native {
        "Set.empty" => Some(CoreSetOp::Empty),
        "Set.size" => Some(CoreSetOp::Size),
        "Set.empty?" => Some(CoreSetOp::IsEmpty),
        "Set.has?" => Some(CoreSetOp::Has),
        "Set.add" => Some(CoreSetOp::Add),
        "Set.remove" => Some(CoreSetOp::Remove),
        "Set.union" => Some(CoreSetOp::Union),
        "Set.intersection" => Some(CoreSetOp::Intersection),
        "Set.difference" => Some(CoreSetOp::Difference),
        "Set.to_list" => Some(CoreSetOp::ToList),
        _ => None,
    }
}

fn is_builtin_ctor_native(native: &str) -> bool {
    matches!(
        native,
        "None"
            | "Some"
            | "Ok"
            | "Error"
            | "Less"
            | "Equal"
            | "Greater"
            | "NotFound"
            | "NotAllowed"
            | "NotText"
            | "Failed"
            | "Entry"
            | "File"
            | "Folder"
            | "Link"
            | "Other"
            | "ServiceDown"
            | "ToolFailed"
            | "UnknownHost"
            | "Refused"
            | "TimedOut"
            | "Closed"
            | "Broken"
            | "Certificate"
            | "OutsideFolder"
            | "NotPermitted"
            | "SqlFailed"
            | "Null"
            | "Integer"
            | "Real"
            | "String"
            | "Blob"
            | "Rows"
    )
}
