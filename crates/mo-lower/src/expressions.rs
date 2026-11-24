//! Iterative CPS body lowering, pattern decision trees, loops, field-split optimization,
//! and spec/promise/anchor bodies (`src/lower/expressions.c` parity).

use mo_check::{
    CheckedAbilityUse, CheckedBindingId, CheckedBody, CheckedExprId, CheckedExpression, CheckedOr,
    CheckedPatternId, CheckedPatternKind, CheckedSpecKind, ConstructorRef, ModuleId, SourceAbility,
    SourceType, SourceTypeKind, TypeOwner,
};
use mo_core::{Arm, Expr, ExprId, FnKind, ListOp, Lit, MapOp, Prim, Ty, TyId};
use mo_def::ErrorCode;
use mo_def::core_ops::{
    CoreBase, CoreCmpOp, CoreIntOp, CoreListNullaryOp, CoreMapNullaryOp, CoreRangeOp, CoreSetOp,
    CoreTextOp,
};
use mo_def::limits::{MO_LIMIT_CORE_DEPTH, MO_LIMIT_NESTING_DEPTH};
use mo_syntax::{SyntaxKind, TokenKind};

use crate::applications::{
    lower_cps_ability, lower_cps_ability_native, lower_cps_application, lower_cps_direct_function,
    lower_cps_kind,
};
use crate::constants::lower_global_name;
use crate::context::{ExprSlot, LowerResult, Lowering, SpecRef, UNSET_EXPR};
use crate::cps::{
    AnchorValue, BodyLowerer, ContId, ContinuationKind, LoopInfo, State, Task, TaskKind, VarNodeId,
};
use crate::scalar::{lower_literal, lower_text_part, lower_to_text};
use crate::specs::lower_subject_specs;
use crate::types::{lower_ctors, lower_proof, lower_type};

fn returns(b: &mut BodyLowerer<'_, '_>, root: CheckedExprId) -> LowerResult<bool> {
    let count = b.source.items.len();
    b.p.charge_alloc(count, 1, 1)?;
    let mut seen = vec![false; count];
    seen[root.index()] = true;
    for i in (0..=root.index()).rev() {
        b.p.work(1)?;
        if !seen[i] {
            continue;
        }
        let r = b.row(CheckedExprId(i as u32));
        let sk = b.syntax_kind(r.syntax);
        if sk == SyntaxKind::Return || sk == SyntaxKind::Stop || r.try_info.is_some() {
            return Ok(true);
        }
        if r.lambda.is_some() {
            continue;
        }
        for &op in &r.operands {
            b.p.work(1)?;
            debug_assert!(op.index() < i);
            seen[op.index()] = true;
        }
    }
    Ok(false)
}

fn branch_join(
    b: &mut BodyLowerer<'_, '_>,
    t: Task,
    result: TyId,
) -> LowerResult<(ContId, ExprSlot)> {
    let Some(cont_id) = t.continuation else {
        return b.p.refuse(
            ErrorCode::LowerUnfinished,
            "missing continuation in branch_join",
        );
    };
    if b.cont(cont_id).kind == ContinuationKind::End {
        return Ok((cont_id, t.out));
    }
    let k = b.alloc_cont(ContinuationKind::Jump)?;
    let kname = b.p.fresh()?;
    {
        let c = b.cont_mut(k);
        c.name = kname;
        c.result = true;
        c.variables = t.state.variables;
    }
    let (join_e, param_name) = b.cps_join(k, Some(result))?;
    b.write_slot(t.out, join_e);
    let pv = b.p.expr_var(param_name.unwrap_or_default())?;
    b.finish(Some(pv), t.state, cont_id, ExprSlot::JoinBody(join_e))?;
    Ok((k, ExprSlot::JoinRest(join_e)))
}

fn field_update(
    b: &mut BodyLowerer<'_, '_>,
    assignment: &CheckedExpression,
    value: ExprId,
) -> LowerResult<ExprId> {
    let mut path: Vec<CheckedExprId> = Vec::new();
    let mut at = assignment.operands[0];
    while b.row(at).field.is_some() && b.row(at).operands.len() == 1 {
        if path.len() as u64 == MO_LIMIT_NESTING_DEPTH {
            return b
                .p
                .refuse(ErrorCode::LowerDepth, "field update path is too deep");
        }
        path.push(at);
        at = b.row(at).operands[0];
    }
    debug_assert!(!path.is_empty());

    let Some(bind_id) = assignment.binding else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "field update missing binding");
    };
    let mut old = b.p.expr_var(b.binding_name(bind_id))?;

    // Build nested case/rebuild from outermost step down to innermost, linking through inner arg slot.
    let mut outer_case: Option<ExprId> = None;
    let mut prev_con: Option<(ExprId, usize)> = None;

    for &step_id in path.iter().rev() {
        let step = b.row(step_id);
        let base = b.lower_ty(b.row(step.operands[0]).ty)?;
        let ctors = lower_ctors(b.p, base)?;
        let field_idx = step.field.unwrap_or(0) as usize;
        debug_assert!(ctors.len() == 1 && field_idx < ctors[0].fields.len());
        let fcount = ctors[0].fields.len();
        let mut names = Vec::with_capacity(fcount);
        let mut con_args = Vec::with_capacity(fcount);
        for _ in 0..fcount {
            let vn = b.p.fresh()?;
            con_args.push(b.p.expr_var(vn.clone())?);
            names.push(vn);
        }
        let rebuilt = b.p.expr_con(base, ctors[0].name.clone(), con_args)?;
        let case_e = b.p.alloc_expr(Expr::Case {
            expr: old,
            arms: vec![Arm {
                ctor: ctors[0].name.clone(),
                names: names.clone(),
                body: rebuilt,
            }],
        })?;
        if let Some((pcon, pslot)) = prev_con {
            if let Expr::Con { args, .. } = b.p.arena.expr_mut(pcon) {
                args[pslot] = case_e;
            }
        } else {
            outer_case = Some(case_e);
        }
        prev_con = Some((rebuilt, field_idx));
        old = b.p.expr_var(names[field_idx].clone())?;
    }

    if let Some((pcon, pslot)) = prev_con
        && let Expr::Con { args, .. } = b.p.arena.expr_mut(pcon)
    {
        args[pslot] = value;
    }
    outer_case.ok_or_else(|| {
        let _ =
            b.p.refuse::<()>(ErrorCode::LowerUnfinished, "empty field update");
        crate::context::LowerAbort
    })
}

/// Schedules sub-pattern matching tasks and returns the slot index for the terminal `yes`
/// expression (`base + part_count`).
fn pattern_parts(
    b: &mut BodyLowerer<'_, '_>,
    pattern_id: CheckedPatternId,
    values: &[ExprId],
    yes: Option<ExprId>,
    no: ExprId,
    out: ExprSlot,
) -> LowerResult<usize> {
    let parts = b.pattern(pattern_id).parts.clone();
    let base = b.alloc_root_slots(parts.len() + 1)?;
    b.roots[base + parts.len()] = yes;
    b.push_task(Task {
        kind: TaskKind::Copy,
        row: CheckedExprId(0),
        state: State::default(),
        continuation: None,
        out,
        success_slot: Some(base),
        value: None,
        then_: None,
        else_: None,
        pattern: None,
        match_arm: None,
        diagnostic: false,
    })?;
    // LIFO: pushed 0..part_count so part_count-1 executes first (right-to-left dependencies,
    // left-to-right runtime evaluation).
    for (i, part) in parts.iter().enumerate() {
        let idx = part.index as usize;
        debug_assert!(idx < values.len());
        b.push_task(Task {
            kind: TaskKind::Pattern,
            row: CheckedExprId(0),
            state: State::default(),
            continuation: None,
            out: ExprSlot::Root(base + i),
            success_slot: Some(base + i + 1),
            value: Some(values[idx]),
            then_: None,
            else_: Some(no),
            pattern: Some(part.pattern),
            match_arm: None,
            diagnostic: false,
        })?;
    }
    Ok(base + parts.len())
}

fn pattern_task(b: &mut BodyLowerer<'_, '_>, t: Task) -> LowerResult<()> {
    let Some(pat_id) = t.pattern else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "pattern_task missing pattern");
    };
    let pat = b.pattern(pat_id).clone();
    b.p.span = b.syntax_span(pat.syntax);
    let yes = match t.success_slot {
        Some(slot) => b.roots[slot],
        None => t.then_,
    };
    let (Some(yes), Some(no), Some(scrut)) = (yes, t.else_, t.value) else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "pattern_task missing branches");
    };

    match pat.kind {
        CheckedPatternKind::Any => {
            b.write_slot(t.out, yes);
            Ok(())
        }
        CheckedPatternKind::Bind => {
            let Some(bind_id) = pat.binding else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "bind pattern missing binding");
            };
            let bname = b.binding_name(bind_id);
            let e = b.cps_let(bname, scrut)?;
            b.set_binder(e, 0, bind_id)?;
            if let Expr::Let { body, .. } = b.p.arena.expr_mut(e) {
                *body = yes;
            }
            b.write_slot(t.out, e);
            Ok(())
        }
        CheckedPatternKind::Literal => {
            let ty = b.lower_ty(pat.ty)?;
            let mut s_nid = pat.syntax;
            let sk = b.syntax_kind(s_nid);
            let negative = sk == SyntaxKind::Negate || sk == SyntaxKind::Prefix;
            if negative {
                let node = b.p.syntax_node(b.syntax_module, s_nid);
                if let Some(last) = node.children.last().copied().flatten() {
                    s_nid = last;
                }
            }
            let lit = lower_literal(b.p, b.syntax_module, s_nid, ty, negative)?;
            let cond = b.p.expr_prim(Prim::Eq(ty), vec![scrut, lit])?;
            let e = b.p.expr_if(cond, yes, no)?;
            b.write_slot(t.out, e);
            Ok(())
        }
        CheckedPatternKind::Tuple => {
            let ty = b.lower_ty(pat.ty)?;
            let arity = match b.p.core_ty(ty) {
                Ty::Tuple(parts) => parts.len(),
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "tuple pattern non-tuple type");
                }
            };
            let mut values = Vec::with_capacity(arity);
            for i in 0..arity {
                values.push(b.p.expr_proj(scrut, i as u64)?);
            }
            let _ = pattern_parts(b, pat_id, &values, Some(yes), no, t.out)?;
            Ok(())
        }
        CheckedPatternKind::List => {
            let ty = b.lower_ty(pat.ty)?;
            let elem_ty = match *b.p.core_ty(ty) {
                Ty::List(e) => e,
                _ => {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "list pattern non-list type");
                }
            };
            let uncons = b.p.expr_prim(
                Prim::List {
                    ty: elem_ty,
                    op: ListOp::Nullary(CoreListNullaryOp::Uncons),
                },
                vec![scrut],
            )?;
            let pair_var = b.p.fresh()?;
            let empty_pat = pat.parts.is_empty();
            let none_ctor = b.p.copy_text("None")?;
            let some_ctor = b.p.copy_text("Some")?;
            let case_e = b.p.alloc_expr(Expr::Case {
                expr: uncons,
                arms: vec![
                    Arm {
                        ctor: none_ctor,
                        names: Vec::new(),
                        body: if empty_pat { yes } else { no },
                    },
                    Arm {
                        ctor: some_ctor,
                        names: vec![pair_var.clone()],
                        body: if empty_pat { no } else { UNSET_EXPR },
                    },
                ],
            })?;
            b.write_slot(t.out, case_e);
            if !empty_pat {
                let pv0 = b.p.expr_var(pair_var.clone())?;
                let pv1 = b.p.expr_var(pair_var)?;
                let vals = [b.p.expr_proj(pv0, 0)?, b.p.expr_proj(pv1, 1)?];
                let _ = pattern_parts(
                    b,
                    pat_id,
                    &vals,
                    Some(yes),
                    no,
                    ExprSlot::CaseArmBody(case_e, 1),
                )?;
            }
            Ok(())
        }
        CheckedPatternKind::Constructor => {
            let ty = b.lower_ty(pat.ty)?;
            if matches!(b.p.core_ty(ty), Ty::Unit) {
                b.write_slot(t.out, yes);
                return Ok(());
            }
            let ctors = lower_ctors(b.p, ty)?;
            let selected = match pat.constructor {
                Some(ConstructorRef::Source(sym)) => {
                    b.p.program.interfaces.symbol(sym).name.clone()
                }
                Some(ConstructorRef::Builtin(name)) => name.to_string(),
                None => {
                    return b.p.refuse(
                        ErrorCode::LowerUnfinished,
                        "constructor pattern missing constructor",
                    );
                }
            };
            let mut arms = Vec::with_capacity(ctors.len());
            let mut matched_arm: Option<(usize, Vec<ExprId>)> = None;
            for (i, ctor) in ctors.iter().enumerate() {
                let mut names = Vec::with_capacity(ctor.fields.len());
                let mut vals = Vec::with_capacity(ctor.fields.len());
                for _ in 0..ctor.fields.len() {
                    let vn = b.p.fresh()?;
                    vals.push(b.p.expr_var(vn.clone())?);
                    names.push(vn);
                }
                let arm_body = if ctor.name == selected {
                    matched_arm = Some((i, vals));
                    UNSET_EXPR
                } else {
                    no
                };
                arms.push(Arm {
                    ctor: ctor.name.clone(),
                    names,
                    body: arm_body,
                });
            }
            let case_e = b.p.alloc_expr(Expr::Case { expr: scrut, arms })?;
            b.write_slot(t.out, case_e);
            if let Some((arm_idx, vals)) = matched_arm {
                let _ = pattern_parts(
                    b,
                    pat_id,
                    &vals,
                    Some(yes),
                    no,
                    ExprSlot::CaseArmBody(case_e, arm_idx),
                )?;
            }
            Ok(())
        }
    }
}

fn pattern_variables(
    b: &mut BodyLowerer<'_, '_>,
    root: CheckedPatternId,
    mut outer: Option<VarNodeId>,
) -> LowerResult<Option<VarNodeId>> {
    let count = b.source.bindings.len();
    b.p.charge_alloc(count, 1, 1)?;
    let mut seen = vec![false; count];
    let mut stack = vec![root];
    while let Some(pid) = stack.pop() {
        b.p.work(1)?;
        let pat = b.pattern(pid);
        if pat.kind == CheckedPatternKind::Bind
            && let Some(bid) = pat.binding
        {
            seen[bid.index()] = true;
        }
        for part in &pat.parts {
            stack.push(part.pattern);
        }
    }
    for i in (0..count).rev() {
        if seen[i] {
            outer = Some(b.push_var_node(CheckedBindingId(i as u32), outer)?);
        }
    }
    Ok(outer)
}

fn condition_variables(
    b: &mut BodyLowerer<'_, '_>,
    index: CheckedExprId,
    target: ExprId,
    mut outer: Option<VarNodeId>,
) -> LowerResult<Option<VarNodeId>> {
    let root = b.row(index);
    if root.pattern.is_none()
        && !matches!(
            root.operation,
            TokenKind::And | TokenKind::Or | TokenKind::Implies
        )
    {
        return Ok(outer);
    }
    let mut candidates = outer;
    let mut rows = vec![index];
    while let Some(rid) = rows.pop() {
        b.p.work(1)?;
        let r = b.row(rid);
        if let Some(pid) = r.pattern {
            candidates = pattern_variables(b, pid, candidates)?;
        }
        if matches!(
            r.operation,
            TokenKind::And | TokenKind::Or | TokenKind::Implies
        ) {
            rows.push(r.operands[0]);
            rows.push(r.operands[1]);
        }
    }
    if candidates == outer {
        return Ok(outer);
    }

    let mut used = vec![false; b.source.bindings.len()];
    let mut stack: Vec<(ExprId, usize)> = vec![(target, 0)];
    while let Some((expr_id, child_idx)) = stack.last_mut() {
        b.p.work(1)?;
        let eid = *expr_id;
        let ci = *child_idx;
        *child_idx += 1;
        if ci == 0 {
            let vname_opt = if let Expr::Var(vname) = b.p.arena.expr(eid) {
                Some(vname.clone())
            } else {
                None
            };
            if let Some(vname_str) = vname_opt {
                let mut cur = candidates;
                while cur != outer {
                    let Some(vid) = cur else {
                        break;
                    };
                    b.p.work(1)?;
                    let vn = b.var_nodes[vid.0 as usize];
                    if b.names[vn.binding.index()] == vname_str {
                        used[vn.binding.index()] = true;
                    }
                    cur = vn.next;
                }
            }
        }
        let Some(next) = b.p.expr_child(eid, ci) else {
            stack.pop();
            continue;
        };
        if stack.len() as u64 == MO_LIMIT_CORE_DEPTH {
            return b
                .p
                .refuse(ErrorCode::LowerDepth, "condition target is too deep");
        }
        stack.push((next, 0));
    }

    for i in (0..used.len()).rev() {
        if used[i] {
            outer = Some(b.push_var_node(CheckedBindingId(i as u32), outer)?);
        }
    }
    Ok(outer)
}

fn is_atom(b: &BodyLowerer<'_, '_>, e: ExprId) -> bool {
    matches!(
        b.p.arena.expr(e),
        Expr::Lit(_) | Expr::Var(_) | Expr::Global(_)
    )
}

fn is_small_target(b: &BodyLowerer<'_, '_>, e: ExprId) -> bool {
    if is_atom(b, e) || matches!(b.p.arena.expr(e), Expr::Stop(_)) {
        return true;
    }
    let args = match b.p.arena.expr(e) {
        Expr::Jump { args, .. } | Expr::Prim { args, .. } => args,
        _ => return false,
    };
    args.len() <= 4 && args.iter().all(|&a| is_atom(b, a))
}

fn share_target(
    b: &mut BodyLowerer<'_, '_>,
    t: Task,
    target: ExprId,
    out: &mut ExprSlot,
) -> LowerResult<ExprId> {
    if is_small_target(b, target) {
        return Ok(target);
    }
    let k = b.alloc_cont(ContinuationKind::Jump)?;
    let kname = b.p.fresh()?;
    let vars = condition_variables(b, t.row, target, t.state.variables)?;
    {
        let c = b.cont_mut(k);
        c.name = kname;
        c.variables = vars;
    }
    let (join_e, _) = b.cps_join(k, None)?;
    if let Expr::Join { body, .. } = b.p.arena.expr_mut(join_e) {
        *body = target;
    }
    b.write_slot(*out, join_e);
    *out = ExprSlot::JoinRest(join_e);
    b.cps_jump(k, None)
}

fn arm_task(b: &mut BodyLowerer<'_, '_>, t: Task) -> LowerResult<()> {
    let Some((match_id, arm_idx)) = t.match_arm else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "arm_task missing arm");
    };
    let arm = b.arm(match_id, arm_idx).clone();
    let (Some(body_val), Some(scrut), Some(else_expr)) = (t.value, t.then_, t.else_) else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "arm_task missing values");
    };
    b.p.span = b.syntax_span(arm.syntax);

    let selected = b.alloc_cont(ContinuationKind::Jump)?;
    let sel_name = b.p.fresh()?;
    let mut sel_vars = pattern_variables(b, arm.patterns[0], t.state.variables)?;
    if let Some(guard_row) = arm.guard {
        sel_vars = condition_variables(b, guard_row, body_val, sel_vars)?;
    }
    {
        let c = b.cont_mut(selected);
        c.name = sel_name;
        c.variables = sel_vars;
    }
    let (success_join, _) = b.cps_join(selected, None)?;
    if let Expr::Join { body, .. } = b.p.arena.expr_mut(success_join) {
        *body = body_val;
    }
    b.write_slot(t.out, success_join);

    if let Some(guard_row) = arm.guard
        && arm.patterns.len() > 1
    {
        let int_src = b.p.types_overlay.builtin("Int");
        let int_ty = b.lower_ty(int_src)?;
        let choose = b.alloc_cont(ContinuationKind::Jump)?;
        let choose_name = b.p.fresh()?;
        let choose_pos = b.p.fresh()?;
        {
            let c = b.cont_mut(choose);
            c.name = choose_name;
            c.position = choose_pos.clone();
            c.position_type = Some(int_ty);
            c.variables = t.state.variables;
        }
        let (dispatch_join, _) = b.cps_join(choose, None)?;
        if let Expr::Join { rest, .. } = b.p.arena.expr_mut(success_join) {
            *rest = dispatch_join;
        }
        let initial = b.clone_cont(choose)?;
        let zero = b.p.expr_int(0)?;
        b.cont_mut(initial).extra = Some(zero);
        let init_jump = b.cps_jump(initial, None)?;
        if let Expr::Join { rest, .. } = b.p.arena.expr_mut(dispatch_join) {
            *rest = init_jump;
        }

        let guard_k = b.alloc_cont(ContinuationKind::Jump)?;
        let guard_name = b.p.fresh()?;
        let guard_pos = b.p.fresh()?;
        let guard_vars = pattern_variables(b, arm.patterns[0], t.state.variables)?;
        {
            let c = b.cont_mut(guard_k);
            c.name = guard_name;
            c.position = guard_pos.clone();
            c.position_type = Some(int_ty);
            c.variables = guard_vars;
        }
        let (tested_join, _) = b.cps_join(guard_k, None)?;
        if let Expr::Join { body, .. } = b.p.arena.expr_mut(dispatch_join) {
            *body = tested_join;
        }

        let sel_jump = b.cps_jump(selected, None)?;
        let gp_v = b.p.expr_var(guard_pos)?;
        let one = b.p.expr_int(1)?;
        let inc = b.p.expr_prim(Prim::Int(CoreIntOp::Add), vec![gp_v, one])?;
        b.cont_mut(choose).extra = Some(inc);
        let retry_jump = b.cps_jump(choose, None)?;
        let select_if = b.p.expr_if(UNSET_EXPR, sel_jump, retry_jump)?;
        b.push_task(Task {
            kind: TaskKind::Condition,
            row: guard_row,
            state: t.state,
            continuation: None,
            out: ExprSlot::JoinBody(tested_join),
            success_slot: None,
            value: Some(select_if),
            then_: None,
            else_: None,
            pattern: None,
            match_arm: None,
            diagnostic: false,
        })?;

        let mut failure = else_expr;
        for j in (1..=arm.patterns.len()).rev() {
            let cp_v = b.p.expr_var(choose_pos.clone())?;
            let jm1 = b.p.expr_int((j - 1) as u64)?;
            let cond = b.p.expr_prim(
                Prim::Cmp {
                    base: CoreBase::Int,
                    op: CoreCmpOp::Le,
                },
                vec![cp_v, jm1],
            )?;
            let gate = b.p.expr_if(cond, UNSET_EXPR, failure)?;
            let retry_k = b.clone_cont(choose)?;
            let j_lit = b.p.expr_int(j as u64)?;
            b.cont_mut(retry_k).extra = Some(j_lit);
            let jm1_lit = b.p.expr_int((j - 1) as u64)?;
            b.cont_mut(guard_k).extra = Some(jm1_lit);
            let then_j = b.cps_jump(guard_k, None)?;
            let else_j = b.cps_jump(retry_k, None)?;
            b.push_task(Task {
                kind: TaskKind::Pattern,
                row: CheckedExprId(0),
                state: State::default(),
                continuation: None,
                out: ExprSlot::IfThen(gate),
                success_slot: None,
                value: Some(scrut),
                then_: Some(then_j),
                else_: Some(else_j),
                pattern: Some(arm.patterns[j - 1]),
                match_arm: None,
                diagnostic: false,
            })?;
            failure = gate;
        }
        if let Expr::Join { rest, .. } = b.p.arena.expr_mut(tested_join) {
            *rest = failure;
        }
        return Ok(());
    }

    let mut failure = else_expr;
    for j in (1..=arm.patterns.len()).rev() {
        let next_k = b.alloc_cont(ContinuationKind::Jump)?;
        let next_name = b.p.fresh()?;
        {
            let c = b.cont_mut(next_k);
            c.name = next_name;
            c.variables = t.state.variables;
        }
        let (fallback, _) = b.cps_join(next_k, None)?;
        if let Expr::Join { body, .. } = b.p.arena.expr_mut(fallback) {
            *body = failure;
        }
        let yes_j = b.cps_jump(selected, None)?;
        let no_j = b.cps_jump(next_k, None)?;
        if let Some(guard_row) = arm.guard {
            let select = b.p.expr_if(UNSET_EXPR, yes_j, no_j)?;
            let guarded_slot = b.alloc_root_slot();
            b.push_task(Task {
                kind: TaskKind::Pattern,
                row: CheckedExprId(0),
                state: State::default(),
                continuation: None,
                out: ExprSlot::JoinRest(fallback),
                success_slot: Some(guarded_slot),
                value: Some(scrut),
                then_: None,
                else_: Some(no_j),
                pattern: Some(arm.patterns[j - 1]),
                match_arm: None,
                diagnostic: false,
            })?;
            b.push_task(Task {
                kind: TaskKind::Condition,
                row: guard_row,
                state: t.state,
                continuation: None,
                out: ExprSlot::Root(guarded_slot),
                success_slot: None,
                value: Some(select),
                then_: None,
                else_: None,
                pattern: None,
                match_arm: None,
                diagnostic: false,
            })?;
        } else {
            b.push_task(Task {
                kind: TaskKind::Pattern,
                row: CheckedExprId(0),
                state: State::default(),
                continuation: None,
                out: ExprSlot::JoinRest(fallback),
                success_slot: None,
                value: Some(scrut),
                then_: Some(yes_j),
                else_: Some(no_j),
                pattern: Some(arm.patterns[j - 1]),
                match_arm: None,
                diagnostic: false,
            })?;
        }
        failure = fallback;
    }
    if let Expr::Join { rest, .. } = b.p.arena.expr_mut(success_join) {
        *rest = failure;
    }
    Ok(())
}

fn match_expr(b: &mut BodyLowerer<'_, '_>, t: Task, k: ContId) -> LowerResult<()> {
    let (Some(row_id), Some(k_next), Some(scrut_val)) = (b.cont(k).row, b.cont(k).next, t.value)
    else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "match_expr missing inputs");
    };
    let row_syntax = b.row(row_id).syntax;
    let row_ty = b.row(row_id).ty;
    let Some(match_id) = b.row(row_id).match_info else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "match_expr missing match_info");
    };
    b.p.span = b.syntax_span(row_syntax);
    let scrut_name = b.p.fresh()?;
    let bound = b.cps_let(scrut_name.clone(), scrut_val)?;
    b.write_slot(t.out, bound);

    let mut branch = t;
    branch.out = ExprSlot::LetBody(bound);
    branch.continuation = Some(k_next);
    let res_ty = b.lower_ty(row_ty)?;
    let (done_k, out) = branch_join(b, branch, res_ty)?;

    let mut failure = b.p.expr_stop("unreachable pattern failure")?;
    let arm_count = b.source.match_info(match_id).arms.len();
    for i in (0..arm_count).rev() {
        let arm_syntax = b.arm(match_id, i).syntax;
        let arm_body = b.arm(match_id, i).body;
        b.p.span = b.syntax_span(arm_syntax);

        let arm_next = b.alloc_cont(ContinuationKind::Jump)?;
        let aname = b.p.fresh()?;
        {
            let c = b.cont_mut(arm_next);
            c.name = aname;
            c.variables = t.state.variables;
        }
        let (arm_exit, _) = b.cps_join(arm_next, None)?;
        if let Expr::Join { body, .. } = b.p.arena.expr_mut(arm_exit) {
            *body = failure;
        }
        let fail_jump = b.cps_jump(arm_next, None)?;

        let body_slot = b.alloc_root_slot();
        let scrut_v = b.p.expr_var(scrut_name.clone())?;
        b.push_task(Task {
            kind: TaskKind::Arm,
            row: CheckedExprId(0),
            state: t.state,
            continuation: None,
            out: ExprSlot::JoinRest(arm_exit),
            success_slot: Some(body_slot),
            value: None,
            then_: Some(scrut_v),
            else_: Some(fail_jump),
            pattern: None,
            match_arm: Some((match_id, i)),
            diagnostic: false,
        })?;
        b.emit(arm_body, t.state, done_k, ExprSlot::Root(body_slot))?;
        failure = arm_exit;
    }
    b.write_slot(out, failure);
    Ok(())
}

enum ForCollection {
    None,
    Range,
    List(TyId),
    MapCountOnly,
    MapIndexed { key: TyId, value: TyId },
}

fn loop_expr(
    b: &mut BodyLowerer<'_, '_>,
    t: Task,
    row_id: CheckedExprId,
    mut iterable: Option<ExprId>,
) -> LowerResult<()> {
    let r = b.row(row_id).clone();
    b.p.span = b.syntax_span(r.syntax);
    let is_for = b.syntax_kind(r.syntax) == SyntaxKind::For;
    let int_src = b.p.types_overlay.builtin("Int");
    let mut out = t.out;

    let mut coll_kind = ForCollection::None;
    let mut list_var = String::new();
    let mut end_var = String::new();
    let mut map_keys_var = String::new();
    let mut map_vals_var = String::new();

    if is_for {
        let iter_val = iterable.take().unwrap_or(UNSET_EXPR);
        let coll_src = b.row(r.operands[0]).ty;
        let coll_ty = b.lower_ty(coll_src)?;
        let pat_kind = r.pattern.map(|pid| b.pattern(pid).kind);

        if let Ty::Map { key, value } = *b.p.core_ty(coll_ty)
            && r.binding.is_none()
            && matches!(
                pat_kind,
                None | Some(CheckedPatternKind::Any | CheckedPatternKind::Tuple)
            )
        {
            let count_only = matches!(pat_kind, None | Some(CheckedPatternKind::Any));
            list_var = b.p.fresh()?;
            let held = b.cps_let(list_var.clone(), iter_val)?;
            b.write_slot(out, held);
            out = ExprSlot::LetBody(held);

            if count_only {
                coll_kind = ForCollection::MapCountOnly;
                let lv = b.p.expr_var(list_var.clone())?;
                let sz = b.cps_unary(
                    Prim::Map {
                        key,
                        value,
                        op: MapOp::Nullary(CoreMapNullaryOp::Size),
                    },
                    lv,
                )?;
                end_var = b.p.fresh()?;
                let limit = b.cps_let(end_var.clone(), sz)?;
                b.write_slot(out, limit);
                out = ExprSlot::LetBody(limit);
            } else {
                coll_kind = ForCollection::MapIndexed { key, value };
                let lv1 = b.p.expr_var(list_var.clone())?;
                let ks_e = b.cps_unary(
                    Prim::Map {
                        key,
                        value,
                        op: MapOp::Nullary(CoreMapNullaryOp::Keys),
                    },
                    lv1,
                )?;
                map_keys_var = b.p.fresh()?;
                let ks_let = b.cps_let(map_keys_var.clone(), ks_e)?;
                b.write_slot(out, ks_let);
                out = ExprSlot::LetBody(ks_let);

                let lv2 = b.p.expr_var(list_var.clone())?;
                let vs_e = b.cps_unary(
                    Prim::Map {
                        key,
                        value,
                        op: MapOp::Nullary(CoreMapNullaryOp::Values),
                    },
                    lv2,
                )?;
                map_vals_var = b.p.fresh()?;
                let vs_let = b.cps_let(map_vals_var.clone(), vs_e)?;
                b.write_slot(out, vs_let);
                out = ExprSlot::LetBody(vs_let);

                let mkv = b.p.expr_var(map_keys_var.clone())?;
                let sz_e = b.cps_unary(
                    Prim::List {
                        ty: key,
                        op: ListOp::Nullary(CoreListNullaryOp::Size),
                    },
                    mkv,
                )?;
                end_var = b.p.fresh()?;
                let lim_let = b.cps_let(end_var.clone(), sz_e)?;
                b.write_slot(out, lim_let);
                out = ExprSlot::LetBody(lim_let);
            }
        } else {
            let (converted_iter, kind) = match *b.p.core_ty(coll_ty) {
                Ty::Range => (iter_val, ForCollection::Range),
                Ty::List(elem) => (iter_val, ForCollection::List(elem)),
                Ty::Set(elem) => {
                    let e = b.cps_unary(
                        Prim::Set {
                            ty: elem,
                            op: CoreSetOp::ToList,
                        },
                        iter_val,
                    )?;
                    (e, ForCollection::List(elem))
                }
                Ty::Map { key, value } => {
                    let map_args = b.p.source_type(coll_src).arguments.clone();
                    let tuple_src = b.p.types_overlay.alloc(SourceType {
                        kind: SourceTypeKind::Tuple,
                        name: "Tuple".to_string(),
                        module: None,
                        owner: TypeOwner::None,
                        arguments: map_args.clone(),
                        height: b.p.source_type(coll_src).height,
                        parameters: b.p.source_type(coll_src).parameters,
                    });
                    let pair_ty = b.lower_ty(tuple_src)?;
                    let e = b.cps_unary(
                        Prim::Map {
                            key,
                            value,
                            op: MapOp::Nullary(CoreMapNullaryOp::Entries),
                        },
                        iter_val,
                    )?;
                    (e, ForCollection::List(pair_ty))
                }
                _ => {
                    return b.p.refuse(
                        ErrorCode::LowerUnfinished,
                        "unsupported for-loop collection",
                    );
                }
            };
            coll_kind = kind;
            list_var = b.p.fresh()?;
            let held = b.cps_let(list_var.clone(), converted_iter)?;
            b.write_slot(out, held);
            out = ExprSlot::LetBody(held);

            let lv = b.p.expr_var(list_var.clone())?;
            let lim_e = match coll_kind {
                ForCollection::Range => b.cps_unary(Prim::Range(CoreRangeOp::End), lv)?,
                ForCollection::List(elem) => b.cps_unary(
                    Prim::List {
                        ty: elem,
                        op: ListOp::Nullary(CoreListNullaryOp::Size),
                    },
                    lv,
                )?,
                _ => UNSET_EXPR,
            };
            end_var = b.p.fresh()?;
            let lim_let = b.cps_let(end_var.clone(), lim_e)?;
            b.write_slot(out, lim_let);
            out = ExprSlot::LetBody(lim_let);
        }
    }

    let Some(t_cont) = t.continuation else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "loop missing continuation");
    };
    let done_k = b.alloc_cont(ContinuationKind::Jump)?;
    let done_name = b.p.fresh()?;
    {
        let c = b.cont_mut(done_k);
        c.name = done_name;
        c.variables = t.state.variables;
    }
    let (exit_join, _) = b.cps_join(done_k, None)?;
    b.write_slot(out, exit_join);
    let unit = b.p.expr_unit()?;
    b.finish(Some(unit), t.state, t_cont, ExprSlot::JoinBody(exit_join))?;

    let again_k = b.alloc_cont(ContinuationKind::Jump)?;
    let again_name = b.p.fresh()?;
    let again_pos = if is_for {
        if matches!(coll_kind, ForCollection::Range)
            && let Some(bid) = r.binding
        {
            b.binding_name(bid)
        } else {
            b.p.fresh()?
        }
    } else {
        String::new()
    };
    let int_ty = b.lower_ty(int_src)?;
    {
        let c = b.cont_mut(again_k);
        c.name = again_name;
        c.row = Some(row_id);
        c.variables = t.state.variables;
        if is_for {
            c.position = again_pos.clone();
            c.position_type = Some(int_ty);
        }
    }
    let (round_join, _) = b.cps_join(again_k, None)?;
    if let Expr::Join { rest, .. } = b.p.arena.expr_mut(exit_join) {
        *rest = round_join;
    }

    let scope = b.alloc_loop(LoopInfo {
        break_k: done_k,
        continue_k: again_k,
    })?;
    let mut inner_state = t.state;
    inner_state.loop_info = Some(scope);

    let initial_k = b.clone_cont(again_k)?;
    let mut body_out;
    let mut element: Option<ExprId> = None;

    if !is_for {
        let done_j = b.cps_jump(done_k, None)?;
        let select = b.p.expr_if(UNSET_EXPR, UNSET_EXPR, done_j)?;
        b.push_task(Task {
            kind: TaskKind::Condition,
            row: r.operands[0],
            state: inner_state,
            continuation: None,
            out: ExprSlot::JoinBody(round_join),
            success_slot: None,
            value: Some(select),
            then_: None,
            else_: None,
            pattern: None,
            match_arm: None,
            diagnostic: false,
        })?;
        body_out = ExprSlot::IfThen(select);
    } else {
        if matches!(coll_kind, ForCollection::Range)
            && let Some(bid) = r.binding
        {
            b.set_binder(round_join, 0, bid)?;
        }
        let pos_v0 = b.p.expr_var(again_pos.clone())?;
        let end_v = b.p.expr_var(end_var)?;
        let cond = b.p.expr_prim(
            Prim::Cmp {
                base: CoreBase::Int,
                op: CoreCmpOp::Lt,
            },
            vec![pos_v0, end_v],
        )?;
        let done_j = b.cps_jump(done_k, None)?;
        let gate = b.p.expr_if(cond, UNSET_EXPR, done_j)?;
        if let Expr::Join { body, .. } = b.p.arena.expr_mut(round_join) {
            *body = gate;
        }
        let pos_v1 = b.p.expr_var(again_pos.clone())?;
        let one = b.p.expr_int(1)?;
        let step =
            b.p.expr_prim(Prim::Int(CoreIntOp::Add), vec![pos_v1, one])?;
        b.cont_mut(again_k).extra = Some(step);

        match coll_kind {
            ForCollection::Range => {
                body_out = ExprSlot::IfThen(gate);
                element = Some(b.p.expr_var(again_pos)?);
                let lv = b.p.expr_var(list_var)?;
                let start = b.cps_unary(Prim::Range(CoreRangeOp::Start), lv)?;
                b.cont_mut(initial_k).extra = Some(start);
            }
            ForCollection::MapCountOnly => {
                let zero = b.p.expr_int(0)?;
                b.cont_mut(initial_k).extra = Some(zero);
                body_out = ExprSlot::IfThen(gate);
            }
            ForCollection::MapIndexed { key, value } => {
                let zero = b.p.expr_int(0)?;
                b.cont_mut(initial_k).extra = Some(zero);
                let imp1 = b.p.expr_stop("unreachable map index")?;
                let imp2 = b.p.expr_stop("unreachable map index")?;
                let pat_imp = b.p.expr_stop("unreachable loop binding failure")?;

                let k_some_var = b.p.fresh()?;
                let v_some_var = b.p.fresh()?;

                let mv_e = b.p.expr_var(map_vals_var)?;
                let pv_v = b.p.expr_var(again_pos.clone())?;
                let v_get = b.p.expr_prim(
                    Prim::List {
                        ty: value,
                        op: ListOp::Nullary(CoreListNullaryOp::Get),
                    },
                    vec![mv_e, pv_v],
                )?;
                let v_none_ctor = b.p.copy_text("None")?;
                let v_some_ctor = b.p.copy_text("Some")?;
                let v_case = b.p.alloc_expr(Expr::Case {
                    expr: v_get,
                    arms: vec![
                        Arm {
                            ctor: v_none_ctor,
                            names: Vec::new(),
                            body: imp2,
                        },
                        Arm {
                            ctor: v_some_ctor,
                            names: vec![v_some_var.clone()],
                            body: UNSET_EXPR,
                        },
                    ],
                })?;

                let mk_e = b.p.expr_var(map_keys_var)?;
                let pv_k = b.p.expr_var(again_pos)?;
                let k_get = b.p.expr_prim(
                    Prim::List {
                        ty: key,
                        op: ListOp::Nullary(CoreListNullaryOp::Get),
                    },
                    vec![mk_e, pv_k],
                )?;
                let k_none_ctor = b.p.copy_text("None")?;
                let k_some_ctor = b.p.copy_text("Some")?;
                let k_case = b.p.alloc_expr(Expr::Case {
                    expr: k_get,
                    arms: vec![
                        Arm {
                            ctor: k_none_ctor,
                            names: Vec::new(),
                            body: imp1,
                        },
                        Arm {
                            ctor: k_some_ctor,
                            names: vec![k_some_var.clone()],
                            body: v_case,
                        },
                    ],
                })?;
                if let Expr::If { then_, .. } = b.p.arena.expr_mut(gate) {
                    *then_ = k_case;
                }

                let pair_vals = [b.p.expr_var(k_some_var)?, b.p.expr_var(v_some_var)?];
                let Some(pat_id) = r.pattern else {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "map loop missing tuple pattern");
                };
                let yes_slot = pattern_parts(
                    b,
                    pat_id,
                    &pair_vals,
                    None,
                    pat_imp,
                    ExprSlot::CaseArmBody(v_case, 1),
                )?;
                body_out = ExprSlot::Root(yes_slot);
            }
            ForCollection::List(elem_ty) => {
                let zero = b.p.expr_int(0)?;
                b.cont_mut(initial_k).extra = Some(zero);
                let lv = b.p.expr_var(list_var)?;
                let pv = b.p.expr_var(again_pos)?;
                let get_e = b.p.expr_prim(
                    Prim::List {
                        ty: elem_ty,
                        op: ListOp::Nullary(CoreListNullaryOp::Get),
                    },
                    vec![lv, pv],
                )?;
                let imp = b.p.expr_stop("unreachable list index")?;
                let some_var = b.p.fresh()?;
                let none_ctor = b.p.copy_text("None")?;
                let some_ctor = b.p.copy_text("Some")?;
                let select = b.p.alloc_expr(Expr::Case {
                    expr: get_e,
                    arms: vec![
                        Arm {
                            ctor: none_ctor,
                            names: Vec::new(),
                            body: imp,
                        },
                        Arm {
                            ctor: some_ctor,
                            names: vec![some_var.clone()],
                            body: UNSET_EXPR,
                        },
                    ],
                })?;
                if let Expr::If { then_, .. } = b.p.arena.expr_mut(gate) {
                    *then_ = select;
                }
                if let Some(bid) = r.binding {
                    let bname = b.binding_name(bid);
                    let sv = b.p.expr_var(some_var)?;
                    let item = b.cps_let(bname.clone(), sv)?;
                    b.set_binder(item, 0, bid)?;
                    if let Expr::Case { arms, .. } = b.p.arena.expr_mut(select) {
                        arms[1].body = item;
                    }
                    body_out = ExprSlot::LetBody(item);
                    element = Some(b.p.expr_var(bname)?);
                } else {
                    body_out = ExprSlot::CaseArmBody(select, 1);
                    element = Some(b.p.expr_var(some_var)?);
                }
            }
            ForCollection::None => {
                body_out = ExprSlot::IfThen(gate);
            }
        }
    }

    let init_jump = b.cps_jump(initial_k, None)?;
    if let Expr::Join { rest, .. } = b.p.arena.expr_mut(round_join) {
        *rest = init_jump;
    }

    let is_map_indexed = matches!(
        coll_kind,
        ForCollection::MapCountOnly | ForCollection::MapIndexed { .. }
    );
    if is_for
        && !is_map_indexed
        && r.binding.is_none()
        && let Some(pat_id) = r.pattern
    {
        let success_slot = b.alloc_root_slot();
        let imp = b.p.expr_stop("unreachable loop binding failure")?;
        b.push_task(Task {
            kind: TaskKind::Pattern,
            row: CheckedExprId(0),
            state: State::default(),
            continuation: None,
            out: body_out,
            success_slot: Some(success_slot),
            value: element,
            then_: None,
            else_: Some(imp),
            pattern: Some(pat_id),
            match_arm: None,
            diagnostic: false,
        })?;
        body_out = ExprSlot::Root(success_slot);
    }

    b.emit(r.operands[1], inner_state, again_k, body_out)
}

fn text_concat(b: &mut BodyLowerer<'_, '_>, left: ExprId, right: ExprId) -> LowerResult<ExprId> {
    b.p.expr_prim(Prim::Text(CoreTextOp::Concat), vec![left, right])
}

fn detail_field(
    b: &mut BodyLowerer<'_, '_>,
    label: &str,
    ty: TyId,
    value: ExprId,
) -> LowerResult<ExprId> {
    let text_name = b.p.fresh()?;
    let structural = b.cps_unary(Prim::Structural(ty), value)?;
    let t1 = b.p.expr_var(text_name.clone())?;
    let t2 = b.p.expr_var(text_name.clone())?;
    let bc = b.cps_unary(Prim::Text(CoreTextOp::ByteCount), t1)?;
    let cnt = b.cps_unary(Prim::Int(CoreIntOp::ToText), bc)?;
    let lbl = b.p.expr_text(label)?;
    let colon = b.p.expr_text(":")?;
    let comma = b.p.expr_text(",")?;
    let lhs = text_concat(b, lbl, cnt)?;
    let t_comma = text_concat(b, t2, comma)?;
    let rhs = text_concat(b, colon, t_comma)?;
    let body = text_concat(b, lhs, rhs)?;
    let let_e = b.cps_let(text_name, structural)?;
    if let Expr::Let { body: lb, .. } = b.p.arena.expr_mut(let_e) {
        *lb = body;
    }
    Ok(let_e)
}

fn unwrap_condition(
    b: &mut BodyLowerer<'_, '_>,
    mut row: CheckedExprId,
) -> LowerResult<CheckedExprId> {
    loop {
        let r = b.row(row);
        let k = lower_cps_kind(b, r);
        if k == SyntaxKind::Group || k == SyntaxKind::ExprStmt {
            b.p.work(1)?;
            debug_assert!(r.operands.len() == 1 && r.operands[0].index() < row.index());
            row = r.operands[0];
        } else {
            return Ok(row);
        }
    }
}

fn current_spec_ref(b: &BodyLowerer<'_, '_>) -> Option<SpecRef> {
    b.instance_idx.and_then(|idx| b.p.instances[idx].spec)
}

fn start_promise_step(
    b: &mut BodyLowerer<'_, '_>,
    at: usize,
    mask: Option<String>,
    out: ExprSlot,
) -> LowerResult<()> {
    let Some(spec_ref) = current_spec_ref(b) else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "promise step missing spec");
    };
    let cond_count = b.p.spec_ref(spec_ref).conditions.len();
    if at == cond_count {
        let val = match mask {
            None => b.p.expr_int(0)?,
            Some(m) => b.p.expr_var(m)?,
        };
        b.write_slot(out, val);
        return Ok(());
    }
    let (cond_span, cond_expr) = {
        let c = &b.p.spec_ref(spec_ref).conditions[at];
        (c.text.span, c.expression)
    };
    b.p.span = cond_span;
    let unwrapped = unwrap_condition(b, cond_expr)?;
    let is_implies = b.row(unwrapped).operation == TokenKind::Implies;
    let mask_name = if at < 62 && is_implies {
        if let Some(m) = mask {
            m
        } else {
            let next = b.alloc_cont(ContinuationKind::Promise)?;
            b.cont_mut(next).at = at;
            return b.emit(cond_expr, State::default(), next, out);
        }
    } else {
        let next = b.alloc_cont(ContinuationKind::Promise)?;
        {
            let c = b.cont_mut(next);
            c.at = at;
            c.name = mask.unwrap_or_default();
        }
        return b.emit(cond_expr, State::default(), next, out);
    };

    let int_src = b.p.types_overlay.builtin("Int");
    let int_ty = b.lower_ty(int_src)?;
    let next_k = b.alloc_cont(ContinuationKind::Jump)?;
    let nk_name = b.p.fresh()?;
    {
        let c = b.cont_mut(next_k);
        c.name = nk_name;
        c.result = true;
    }
    let (join_e, next_mask_opt) = b.cps_join(next_k, Some(int_ty))?;
    let next_mask = next_mask_opt.unwrap_or_default();
    b.write_slot(out, join_e);

    let cont = b.alloc_cont(ContinuationKind::Promise)?;
    {
        let c = b.cont_mut(cont);
        c.at = at + 1;
        c.name = next_mask;
        c.result = true;
    }
    b.finish(None, State::default(), cont, ExprSlot::JoinBody(join_e))?;

    let mv0 = b.p.expr_var(mask_name.clone())?;
    let miss_branch = b.cps_jump(next_k, Some(mv0))?;

    let mv1 = b.p.expr_var(mask_name)?;
    let bit = b.p.expr_int(1u64 << at)?;
    let updated = b.p.expr_prim(Prim::Int(CoreIntOp::Sub), vec![mv1, bit])?;
    let hit_var = b.p.fresh()?;
    let hit_v = b.p.expr_var(hit_var.clone())?;
    let hit_jump = b.cps_jump(next_k, Some(hit_v))?;
    let hit_branch = b.cps_let(hit_var, updated)?;
    if let Expr::Let { body, .. } = b.p.arena.expr_mut(hit_branch) {
        *body = hit_jump;
    }

    let select = b.p.expr_if(UNSET_EXPR, UNSET_EXPR, miss_branch)?;
    let op0 = b.row(unwrapped).operands[0];
    let op1 = b.row(unwrapped).operands[1];
    b.push_task(Task {
        kind: TaskKind::Condition,
        row: op0,
        state: State::default(),
        continuation: None,
        out: ExprSlot::JoinRest(join_e),
        success_slot: None,
        value: Some(select),
        then_: None,
        else_: None,
        pattern: None,
        match_arm: None,
        diagnostic: false,
    })?;
    let fail_line = b.p.expr_int((at + 1) as u64)?;
    b.push_task(Task {
        kind: TaskKind::Condition,
        row: op1,
        state: State::default(),
        continuation: None,
        out: ExprSlot::IfThen(select),
        success_slot: None,
        value: None,
        then_: Some(hit_branch),
        else_: Some(fail_line),
        pattern: None,
        match_arm: None,
        diagnostic: false,
    })
}

fn finish_task(b: &mut BodyLowerer<'_, '_>, mut t: Task) -> LowerResult<()> {
    let Some(k_id) = t.continuation else {
        return b.p.refuse(
            ErrorCode::LowerUnfinished,
            "finish_task missing continuation",
        );
    };
    let k = b.cont(k_id).clone();

    match k.kind {
        ContinuationKind::End => {
            if let Some(val) = t.value {
                b.write_slot(t.out, val);
            }
            Ok(())
        }
        ContinuationKind::Promise => {
            let mask = if k.name.is_empty() {
                None
            } else {
                Some(k.name)
            };
            if k.result {
                return start_promise_step(b, k.at, mask, t.out);
            }
            let Some(spec_ref) = current_spec_ref(b) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_PROMISE missing spec");
            };
            b.p.span = b.p.spec_ref(spec_ref).conditions[k.at].text.span;
            let else_line = b.p.expr_int((k.at + 1) as u64)?;
            let cond = t.value.unwrap_or(UNSET_EXPR);
            let e = b.p.expr_if(cond, UNSET_EXPR, else_line)?;
            b.write_slot(t.out, e);
            start_promise_step(b, k.at + 1, mask, ExprSlot::IfThen(e))
        }
        ContinuationKind::Anchor => {
            let Some(spec_ref) = current_spec_ref(b) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_ANCHOR missing spec");
            };
            let Some(arg_buf) = k.arguments else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_ANCHOR missing arg_buf");
            };
            let Some(vals_id) = k.values else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_ANCHOR missing anchor_vals");
            };
            let mut out = t.out;
            let mut val = t.value.unwrap_or(UNSET_EXPR);
            if !is_atom(b, val) {
                let vn = b.p.fresh()?;
                let bound = b.cps_let(vn.clone(), val)?;
                b.write_slot(out, bound);
                out = ExprSlot::LetBody(bound);
                val = b.p.expr_var(vn)?;
            }
            let slot = b.anchor_vals[vals_id.0 as usize][k.at].slot;
            b.arg_bufs[arg_buf.0 as usize][slot] = val;
            let val_count = b.p.spec_ref(spec_ref).values.len();
            if k.at + 1 < val_count {
                let next = b.clone_cont(k_id)?;
                b.cont_mut(next).at = k.at + 1;
                let next_expr = b.anchor_vals[vals_id.0 as usize][k.at + 1].expression;
                b.emit(next_expr, t.state, next, out)
            } else {
                let (mod_id, syn) = {
                    let s = b.p.spec_ref(spec_ref);
                    (s.module, s.syntax)
                };
                b.p.span = b.p.syntax_node(mod_id, syn).span;
                let subj_name = b
                    .instance_idx
                    .and_then(|idx| b.p.instances[idx].subject)
                    .map(|sidx| b.p.instances[sidx].fun.name.clone())
                    .unwrap_or_default();
                let args = b.arg_bufs[arg_buf.0 as usize].clone();
                let call = b.p.expr_call(subj_name, args)?;
                b.write_slot(out, call);
                Ok(())
            }
        }
        ContinuationKind::Jump => {
            let mut out = t.out;
            let val = t.value.unwrap_or(UNSET_EXPR);
            if !k.result && !is_atom(b, val) {
                let vn = b.p.fresh()?;
                let e = b.cps_let(vn, val)?;
                b.write_slot(out, e);
                out = ExprSlot::LetBody(e);
            }
            let j = b.cps_jump(k_id, Some(val))?;
            b.write_slot(out, j);
            Ok(())
        }
        ContinuationKind::Select => {
            let cond = t.value.unwrap_or(UNSET_EXPR);
            let e = b.p.expr_if(
                cond,
                k.then_.unwrap_or(UNSET_EXPR),
                k.else_.unwrap_or(UNSET_EXPR),
            )?;
            b.write_slot(t.out, e);
            Ok(())
        }
        ContinuationKind::Pattern => {
            if k.diagnostic
                && let Some(rid) = k.row
            {
                b.p.span = b.syntax_span(b.row(rid).syntax);
            }
            let vname = b.p.fresh()?;
            let e = b.cps_let(vname.clone(), t.value.unwrap_or(UNSET_EXPR))?;
            b.write_slot(t.out, e);
            let mut out = ExprSlot::LetBody(e);
            let mut else_expr = k.else_.unwrap_or(UNSET_EXPR);
            if k.diagnostic
                && let Some(rid) = k.row
            {
                let op0_ty = b.lower_ty(b.row(b.row(rid).operands[0]).ty)?;
                let vv = b.p.expr_var(vname.clone())?;
                let detail = detail_field(b, "input ", op0_ty, vv)?;
                let failure = b.p.expr_fail(detail)?;
                t.row = rid;
                else_expr = share_target(b, t, failure, &mut out)?;
            }
            let scrut = b.p.expr_var(vname)?;
            b.push_task(Task {
                kind: TaskKind::Pattern,
                row: CheckedExprId(0),
                state: State::default(),
                continuation: None,
                out,
                success_slot: None,
                value: Some(scrut),
                then_: k.then_,
                else_: Some(else_expr),
                pattern: k.pattern,
                match_arm: None,
                diagnostic: false,
            })
        }
        ContinuationKind::Match => match_expr(b, t, k_id),
        ContinuationKind::For => {
            let Some(row_id) = k.row else {
                return b.p.refuse(ErrorCode::LowerUnfinished, "K_FOR missing row");
            };
            t.continuation = k.next;
            loop_expr(b, t, row_id, t.value)
        }
        ContinuationKind::Text => {
            let (Some(row_id), Some(arg_buf), Some(k_next)) = (k.row, k.arguments, k.next) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_TEXT missing fields");
            };
            let r = b.row(row_id).clone();
            b.p.span = b.syntax_span(r.syntax);
            let proof = lower_cps_ability(b, &r, SourceAbility::Text, Some(r.operands[k.at]))?;
            let text_val = lower_to_text(
                b.p,
                proof,
                b.subst,
                b.owner,
                b.depth,
                t.value.unwrap_or(UNSET_EXPR),
            )?;
            let vn = b.p.fresh()?;
            let e = b.cps_let(vn.clone(), text_val)?;
            b.write_slot(t.out, e);
            let v_expr = b.p.expr_var(vn)?;
            b.arg_bufs[arg_buf.0 as usize][2 * k.at + 1] = v_expr;
            if k.at + 1 < r.operands.len() {
                let next = b.clone_cont(k_id)?;
                b.cont_mut(next).at = k.at + 1;
                b.emit(r.operands[k.at + 1], t.state, next, ExprSlot::LetBody(e))
            } else {
                b.p.span = b.syntax_span(r.syntax);
                let parts = b.arg_bufs[arg_buf.0 as usize].clone();
                let mut acc: Option<ExprId> = None;
                for &part in &parts {
                    if let Expr::Lit(Lit::Text(s)) = b.p.arena.expr(part)
                        && s.is_empty()
                    {
                        continue;
                    }
                    acc = Some(match acc {
                        None => part,
                        Some(prev) => text_concat(b, prev, part)?,
                    });
                }
                let final_text = acc.unwrap_or(parts[0]);
                b.finish(Some(final_text), t.state, k_next, ExprSlot::LetBody(e))
            }
        }
        ContinuationKind::Option | ContinuationKind::Try => {
            let (Some(row_id), Some(k_next)) = (k.row, k.next) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_OPTION/K_TRY missing fields");
            };
            let r = b.row(row_id).clone();
            b.p.span = b.syntax_span(r.syntax);
            let op0_ty = b.lower_ty(b.row(r.operands[0]).ty)?;
            let is_result = matches!(b.p.core_ty(op0_ty), Ty::Result { .. });
            let some_var = b.p.fresh()?;
            let err_var = if k.kind == ContinuationKind::Try && is_result {
                Some(b.p.fresh()?)
            } else {
                None
            };

            let (arms, some_arm_idx, none_arm_idx) = if is_result {
                (
                    vec![
                        Arm {
                            ctor: b.p.copy_text("Ok")?,
                            names: vec![some_var.clone()],
                            body: UNSET_EXPR,
                        },
                        Arm {
                            ctor: b.p.copy_text("Error")?,
                            names: err_var.iter().cloned().collect(),
                            body: UNSET_EXPR,
                        },
                    ],
                    0usize,
                    1usize,
                )
            } else {
                (
                    vec![
                        Arm {
                            ctor: b.p.copy_text("None")?,
                            names: Vec::new(),
                            body: UNSET_EXPR,
                        },
                        Arm {
                            ctor: b.p.copy_text("Some")?,
                            names: vec![some_var.clone()],
                            body: UNSET_EXPR,
                        },
                    ],
                    1usize,
                    0usize,
                )
            };
            let case_e = b.p.alloc_expr(Expr::Case {
                expr: t.value.unwrap_or(UNSET_EXPR),
                arms,
            })?;
            b.write_slot(t.out, case_e);
            let sv = b.p.expr_var(some_var)?;
            b.finish(
                Some(sv),
                t.state,
                k_next,
                ExprSlot::CaseArmBody(case_e, some_arm_idx),
            )?;
            if k.kind == ContinuationKind::Option {
                b.emit(
                    r.operands[1],
                    t.state,
                    k_next,
                    ExprSlot::CaseArmBody(case_e, none_arm_idx),
                )
            } else {
                let Some(checked_try) = r.try_info else {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "K_TRY missing try_info");
                };
                let enclosing = b.lower_ty(checked_try.result)?;
                let mut err_expr = match err_var {
                    Some(ev) => Some(b.p.expr_var(ev)?),
                    None => None,
                };
                if let Some(wrapper) = checked_try.wrapper {
                    let err_ty = match *b.p.core_ty(enclosing) {
                        Ty::Result { err, .. } => err,
                        _ => enclosing,
                    };
                    let wname = match wrapper {
                        ConstructorRef::Source(sym) => {
                            b.p.copy_text(&b.p.program.interfaces.symbol(sym).name)?
                        }
                        ConstructorRef::Builtin(s) => b.p.copy_text(s)?,
                    };
                    let wrapped =
                        b.p.expr_con(err_ty, wname, vec![err_expr.unwrap_or(UNSET_EXPR)])?;
                    err_expr = Some(wrapped);
                }
                let fname = match checked_try.failure {
                    Some(ConstructorRef::Source(sym)) => {
                        b.p.copy_text(&b.p.program.interfaces.symbol(sym).name)?
                    }
                    Some(ConstructorRef::Builtin(s)) => b.p.copy_text(s)?,
                    None => {
                        return b
                            .p
                            .refuse(ErrorCode::LowerUnfinished, "K_TRY missing failure ctor");
                    }
                };
                let fargs = match err_expr {
                    Some(e) => vec![e],
                    None => Vec::new(),
                };
                let failure = b.p.expr_con(enclosing, fname, fargs)?;
                let Some(ret_k) = t.state.return_k else {
                    return b
                        .p
                        .refuse(ErrorCode::LowerUnfinished, "K_TRY missing return_k");
                };
                let jmp = b.cps_jump(ret_k, Some(failure))?;
                b.write_slot(ExprSlot::CaseArmBody(case_e, none_arm_idx), jmp);
                Ok(())
            }
        }
        ContinuationKind::Sequence => {
            let (Some(row_id), Some(k_next)) = (k.row, k.next) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_SEQUENCE missing fields");
            };
            let mut out = t.out;
            let val = t.value.unwrap_or(UNSET_EXPR);
            if !is_atom(b, val) {
                let vn = b.p.fresh()?;
                let e = b.cps_let(vn, val)?;
                b.write_slot(out, e);
                out = ExprSlot::LetBody(e);
            }
            let op_count = b.row(row_id).operands.len();
            let next_cont = if k.at + 1 < op_count {
                let nc = b.clone_cont(k_id)?;
                b.cont_mut(nc).at = k.at + 1;
                nc
            } else {
                k_next
            };
            let op_at = b.row(row_id).operands[k.at];
            b.emit(op_at, t.state, next_cont, out)
        }
        ContinuationKind::FieldRebuild => {
            let Some(row_id) = k.row else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_FIELD_REBUILD missing row");
            };
            let r = b.row(row_id).clone();
            b.p.span = b.syntax_span(r.syntax);
            let base = b.lower_ty(b.row(r.operands[0]).ty)?;
            let ctors = lower_ctors(b.p, base)?;
            let field_idx = r.field.unwrap_or(0) as usize;
            let Some(sf_id) = t.state.split_fields else {
                return b.p.refuse(
                    ErrorCode::LowerUnfinished,
                    "K_FIELD_REBUILD missing split_fields",
                );
            };
            let sf = b.split_fields[sf_id.0 as usize].clone();
            let mut args = Vec::with_capacity(ctors[0].fields.len());
            for (j, name) in sf.iter().enumerate().take(ctors[0].fields.len()) {
                if j == field_idx {
                    args.push(t.value.unwrap_or(UNSET_EXPR));
                } else {
                    args.push(b.p.expr_var(name.clone())?);
                }
            }
            let rebuilt = b.p.expr_con(base, ctors[0].name.clone(), args)?;
            b.write_slot(t.out, rebuilt);
            Ok(())
        }
        ContinuationKind::Bind => {
            let (Some(row_id), Some(k_next)) = (k.row, k.next) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_BIND missing fields");
            };
            let r = b.row(row_id).clone();
            b.p.span = b.syntax_span(r.syntax);
            let mut val = t.value.unwrap_or(UNSET_EXPR);
            if r.assignment && r.operands.len() == 2 {
                val = field_update(b, &r, val)?;
            }
            let bind_name = match k.binding {
                Some(bid) => b.binding_name(bid),
                None => b.p.fresh()?,
            };
            let e = b.cps_let(bind_name.clone(), val)?;
            b.write_slot(t.out, e);
            if k.binding.is_none()
                && let Some(pat_id) = r.pattern
            {
                let success_slot = b.alloc_root_slot();
                let imp = b.p.expr_stop("unreachable binding failure")?;
                let bound_v = b.p.expr_var(bind_name)?;
                b.push_task(Task {
                    kind: TaskKind::Pattern,
                    row: CheckedExprId(0),
                    state: State::default(),
                    continuation: None,
                    out: ExprSlot::LetBody(e),
                    success_slot: Some(success_slot),
                    value: Some(bound_v),
                    then_: None,
                    else_: Some(imp),
                    pattern: Some(pat_id),
                    match_arm: None,
                    diagnostic: false,
                })?;
                let unit = b.p.expr_unit()?;
                return b.finish(Some(unit), t.state, k_next, ExprSlot::Root(success_slot));
            }
            if let Some(bid) = k.binding {
                b.set_binder(e, 0, bid)?;
                if b.binding(bid).variable && !r.assignment {
                    t.state.variables = Some(b.push_var_node(bid, t.state.variables)?);
                }
            }
            let unit = b.p.expr_unit()?;
            b.finish(Some(unit), t.state, k_next, ExprSlot::LetBody(e))
        }
        ContinuationKind::Arguments => {
            let (Some(row_id), Some(arg_buf)) = (k.row, k.arguments) else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "K_ARGUMENTS missing fields");
            };
            let skip = k.skip;
            let val = t.value.unwrap_or(UNSET_EXPR);
            let mut direct = !k.diagnostic && is_atom(b, val);
            if direct && let Expr::Var(vname) = b.p.arena.expr(val) {
                let vname_owned = vname.clone();
                let mut cur = t.state.variables;
                while let Some(vid) = cur {
                    b.p.work(1)?;
                    let vn = b.var_nodes[vid.0 as usize];
                    if b.names[vn.binding.index()] == vname_owned {
                        let op_count = b.row(row_id).operands.len();
                        for i in (k.at + 1 + skip)..op_count {
                            b.p.work(1)?;
                            let later = b.row(b.row(row_id).operands[i]);
                            if !later.operands.is_empty() || later.lambda.is_some() {
                                direct = false;
                            }
                        }
                    }
                    cur = vn.next;
                }
            }
            let mut out = t.out;
            if direct {
                b.arg_bufs[arg_buf.0 as usize][k.at] = val;
            } else {
                let e = b.cps_let(k.name.clone(), val)?;
                b.write_slot(out, e);
                out = ExprSlot::LetBody(e);
                b.arg_bufs[arg_buf.0 as usize][k.at] = b.p.expr_var(k.name.clone())?;
            }
            let total_args = b.arg_bufs[arg_buf.0 as usize].len();
            if k.at + 1 < total_args {
                let next = b.clone_cont(k_id)?;
                let fresh_name = b.p.fresh()?;
                {
                    let c = b.cont_mut(next);
                    c.at = k.at + 1;
                    c.name = fresh_name;
                }
                let next_op = b.row(row_id).operands[k.at + 1 + skip];
                b.emit(next_op, t.state, next, out)
            } else {
                let row_syntax = b.row(row_id).syntax;
                b.p.span = b.syntax_span(row_syntax);
                let args = b.arg_bufs[arg_buf.0 as usize].clone();
                let app_val = lower_cps_application(b, row_id, args.clone())?;
                if k.diagnostic {
                    let op0_ty = b.lower_ty(b.row(b.row(row_id).operands[0]).ty)?;
                    let left_d = detail_field(b, "left ", op0_ty, args[0])?;
                    let right_d = detail_field(b, "right ", op0_ty, args[1])?;
                    let msg = text_concat(b, left_d, right_d)?;
                    let fail = b.p.expr_fail(msg)?;
                    let e = b.p.expr_if(app_val, k.then_.unwrap_or(UNSET_EXPR), fail)?;
                    b.write_slot(out, e);
                    Ok(())
                } else {
                    let Some(k_next) = k.next else {
                        return b
                            .p
                            .refuse(ErrorCode::LowerUnfinished, "K_ARGUMENTS missing next");
                    };
                    b.finish(Some(app_val), t.state, k_next, out)
                }
            }
        }
    }
}

fn require_concrete_proofs(
    b: &mut BodyLowerer<'_, '_>,
    uses: &[CheckedAbilityUse],
) -> LowerResult<()> {
    for u in uses {
        b.p.work(1)?;
        let mut proof = u.implementation;
        let mut subst = b.subst;
        lower_proof(b.p, &mut proof, &mut subst)?;
    }
    Ok(())
}

fn is_split_field_read(
    b: &BodyLowerer<'_, '_>,
    r: &CheckedExpression,
    target: Option<CheckedBindingId>,
) -> bool {
    let Some(target_id) = target else {
        return false;
    };
    if r.field.is_none() || r.operands.len() != 1 || lower_cps_kind(b, r) == SyntaxKind::NamedArg {
        return false;
    }
    let base = b.row(r.operands[0]);
    base.binding == Some(target_id)
        && base.operands.is_empty()
        && !base.assignment
        && base.lambda.is_none()
}

fn can_split_field_rhs(
    b: &mut BodyLowerer<'_, '_>,
    r: &CheckedExpression,
    state: State,
) -> LowerResult<bool> {
    if state.split_binding.is_some()
        || !r.assignment
        || r.operands.len() != 2
        || r.binding.is_none()
    {
        return Ok(false);
    }
    let proj = b.row(r.operands[0]);
    if !is_split_field_read(b, proj, r.binding) {
        return Ok(false);
    }
    let base_ty = b.lower_ty(b.row(proj.operands[0]).ty)?;
    if !matches!(b.p.core_ty(base_ty), Ty::Data(_)) {
        return Ok(false);
    }
    let count = b.source.items.len();
    b.p.charge_alloc(count, 1, 1)?;
    let mut seen = vec![false; count];
    let rhs = r.operands[1];
    seen[rhs.index()] = true;
    let mut reads_field = false;
    for i in (0..=rhs.index()).rev() {
        b.p.work(1)?;
        if !seen[i] {
            continue;
        }
        let node = b.row(CheckedExprId(i as u32));
        let kind = lower_cps_kind(b, node);
        if node.assignment
            || node.try_info.is_some()
            || node.match_info.is_some()
            || node.or_kind == CheckedOr::Option
            || matches!(
                node.operation,
                TokenKind::And | TokenKind::Or | TokenKind::Implies
            )
            || matches!(
                kind,
                SyntaxKind::Return
                    | SyntaxKind::Stop
                    | SyntaxKind::Break
                    | SyntaxKind::Continue
                    | SyntaxKind::If
                    | SyntaxKind::LineIf
                    | SyntaxKind::While
                    | SyntaxKind::For
            )
        {
            return Ok(false);
        }
        if let Some(lam_id) = node.lambda {
            let lam = b.source.lambda(lam_id);
            if r.binding.is_some_and(|rb| lam.captures.contains(&rb)) {
                return Ok(false);
            }
            continue;
        }
        if is_split_field_read(b, node, r.binding) {
            reads_field = true;
            continue;
        }
        if node.binding == r.binding {
            return Ok(false);
        }
        for &op in &node.operands {
            b.p.work(1)?;
            debug_assert!(op.index() < i);
            seen[op.index()] = true;
        }
    }
    Ok(reads_field)
}

fn row_task(b: &mut BodyLowerer<'_, '_>, t: Task) -> LowerResult<()> {
    let r = b.row(t.row).clone();
    let tag = lower_cps_kind(b, &r);
    require_concrete_proofs(b, &r.abilities)?;

    let Some(t_cont) = t.continuation else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "row_task missing continuation");
    };

    if is_split_field_read(b, &r, t.state.split_binding)
        && let Some(sf_id) = t.state.split_fields
    {
        let fname = b.split_fields[sf_id.0 as usize][r.field.unwrap_or(0) as usize].clone();
        let v = b.p.expr_var(fname)?;
        return b.finish(Some(v), t.state, t_cont, t.out);
    }

    if tag == SyntaxKind::TextInterpolated {
        let syn_count = b.p.syntax_node(b.syntax_module, r.syntax).children.len();
        let arg_buf = b.alloc_arg_buf(syn_count)?;
        for i in (0..syn_count).step_by(2) {
            let part = lower_text_part(b.p, b.syntax_module, r.syntax, i)?;
            b.arg_bufs[arg_buf.0 as usize][i] = part;
        }
        let k = b.alloc_cont(ContinuationKind::Text)?;
        {
            let c = b.cont_mut(k);
            c.row = Some(t.row);
            c.next = Some(t_cont);
            c.arguments = Some(arg_buf);
        }
        return b.emit(r.operands[0], t.state, k, t.out);
    }

    if let Some(ref try_info) = r.try_info {
        require_concrete_proofs(b, &try_info.abilities)?;
        let k = b.alloc_cont(ContinuationKind::Try)?;
        {
            let c = b.cont_mut(k);
            c.row = Some(t.row);
            c.next = Some(t_cont);
        }
        return b.emit(r.operands[0], t.state, k, t.out);
    }

    if r.or_kind == CheckedOr::Option {
        let rty = b.lower_ty(r.ty)?;
        let (done, out) = branch_join(b, t, rty)?;
        let k = b.alloc_cont(ContinuationKind::Option)?;
        {
            let c = b.cont_mut(k);
            c.row = Some(t.row);
            c.next = Some(done);
        }
        return b.emit(r.operands[0], t.state, k, out);
    }

    if tag == SyntaxKind::Sequence {
        if r.operands.is_empty() {
            let u = b.p.expr_unit()?;
            return b.finish(Some(u), t.state, t_cont, t.out);
        }
        let k = if r.operands.len() > 1 {
            let seq = b.alloc_cont(ContinuationKind::Sequence)?;
            {
                let c = b.cont_mut(seq);
                c.row = Some(t.row);
                c.at = 1;
                c.next = Some(t_cont);
            }
            seq
        } else {
            t_cont
        };
        return b.emit(r.operands[0], t.state, k, t.out);
    }

    if matches!(
        tag,
        SyntaxKind::Group | SyntaxKind::ExprStmt | SyntaxKind::NamedArg
    ) {
        return b.emit(r.operands[0], t.state, t_cont, t.out);
    }

    if matches!(
        tag,
        SyntaxKind::Bind | SyntaxKind::Var | SyntaxKind::AddAssign | SyntaxKind::SubAssign
    ) {
        if can_split_field_rhs(b, &r, t.state)? {
            let proj_id = r.operands[0];
            let proj = b.row(proj_id).clone();
            let base = b.lower_ty(b.row(proj.operands[0]).ty)?;
            let ctors = lower_ctors(b.p, base)?;
            let Some(bid) = r.binding else {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "split field missing binding");
            };
            let mut names = Vec::with_capacity(ctors[0].fields.len());
            for _ in 0..ctors[0].fields.len() {
                names.push(b.p.fresh()?);
            }
            let bind_name = b.binding_name(bid);
            let old_v = b.p.expr_var(bind_name.clone())?;
            let case_e = b.p.alloc_expr(Expr::Case {
                expr: old_v,
                arms: vec![Arm {
                    ctor: ctors[0].name.clone(),
                    names: names.clone(),
                    body: UNSET_EXPR,
                }],
            })?;
            let let_e = b.cps_let(bind_name, case_e)?;
            b.set_binder(let_e, 0, bid)?;
            b.write_slot(t.out, let_e);
            let unit = b.p.expr_unit()?;
            b.finish(Some(unit), t.state, t_cont, ExprSlot::LetBody(let_e))?;

            let sf_id = b.alloc_split_fields(names);
            let mut inner = t.state;
            inner.split_binding = Some(bid);
            inner.split_fields = Some(sf_id);
            let reb = b.alloc_cont(ContinuationKind::FieldRebuild)?;
            b.cont_mut(reb).row = Some(proj_id);
            return b.emit(r.operands[1], inner, reb, ExprSlot::CaseArmBody(case_e, 0));
        }
        let k = b.alloc_cont(ContinuationKind::Bind)?;
        {
            let c = b.cont_mut(k);
            c.binding = r.binding;
            c.row = Some(t.row);
            c.next = Some(t_cont);
        }
        return b.emit(r.operands[r.operands.len() - 1], t.state, k, t.out);
    }

    if let Some(match_id) = r.match_info {
        let scrut = b.source.match_info(match_id).scrutinee;
        let k = b.alloc_cont(ContinuationKind::Match)?;
        {
            let c = b.cont_mut(k);
            c.row = Some(t.row);
            c.next = Some(t_cont);
        }
        return b.emit(scrut, t.state, k, t.out);
    }

    if tag == SyntaxKind::While {
        return loop_expr(b, t, t.row, None);
    }

    if tag == SyntaxKind::For {
        let k = b.alloc_cont(ContinuationKind::For)?;
        {
            let c = b.cont_mut(k);
            c.row = Some(t.row);
            c.next = Some(t_cont);
        }
        return b.emit(r.operands[0], t.state, k, t.out);
    }

    if let Some(pat_id) = r.pattern {
        let rty = b.lower_ty(r.ty)?;
        let (done, out) = branch_join(b, t, rty)?;
        let k = b.alloc_cont(ContinuationKind::Pattern)?;
        let t_lit = b.p.expr_bool(true)?;
        let f_lit = b.p.expr_bool(false)?;
        let (then_e, else_e) = if b.cont(done).kind == ContinuationKind::End {
            (t_lit, f_lit)
        } else {
            (
                b.cps_jump(done, Some(t_lit))?,
                b.cps_jump(done, Some(f_lit))?,
            )
        };
        {
            let c = b.cont_mut(k);
            c.pattern = Some(pat_id);
            c.then_ = Some(then_e);
            c.else_ = Some(else_e);
        }
        return b.emit(r.operands[0], t.state, k, out);
    }

    if tag == SyntaxKind::Assert {
        let is_spec_fn = b
            .instance_idx
            .is_some_and(|idx| b.p.instances[idx].fun.kind != FnKind::Function);
        let else_e = if is_spec_fn {
            let msg = b.p.expr_text("assertion failed")?;
            b.p.expr_fail(msg)?
        } else {
            b.p.expr_stop("assertion failed")?
        };
        let e = b.p.expr_if(UNSET_EXPR, UNSET_EXPR, else_e)?;
        b.push_task(Task {
            kind: TaskKind::Condition,
            row: r.operands[0],
            state: t.state,
            continuation: None,
            out: t.out,
            success_slot: None,
            value: Some(e),
            then_: None,
            else_: None,
            pattern: None,
            match_arm: None,
            diagnostic: is_spec_fn,
        })?;
        let unit = b.p.expr_unit()?;
        return b.finish(Some(unit), t.state, t_cont, ExprSlot::IfThen(e));
    }

    if matches!(
        tag,
        SyntaxKind::Return | SyntaxKind::Break | SyntaxKind::Continue
    ) {
        let target_k = match tag {
            SyntaxKind::Return => t.state.return_k,
            SyntaxKind::Break => t.state.loop_info.map(|l| b.loops[l.0 as usize].break_k),
            _ => t.state.loop_info.map(|l| b.loops[l.0 as usize].continue_k),
        };
        let Some(orig_k) = target_k else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "control jump missing target");
        };
        let exit_k = b.clone_cont(orig_k)?;
        b.cont_mut(exit_k).row = Some(t.row);

        let children = b.p.syntax_node(b.syntax_module, r.syntax).children.clone();
        let guarded = if tag == SyntaxKind::Return {
            children.get(1).copied().flatten().is_some()
        } else {
            children.first().copied().flatten().is_some()
        };
        let has_value = tag == SyntaxKind::Return && children.first().copied().flatten().is_some();

        if guarded {
            let then_init = if has_value {
                UNSET_EXPR
            } else {
                let u = b.p.expr_unit()?;
                b.cps_jump(exit_k, Some(u))?
            };
            let select = b.p.expr_if(UNSET_EXPR, then_init, UNSET_EXPR)?;
            b.push_task(Task {
                kind: TaskKind::Condition,
                row: r.operands[0],
                state: t.state,
                continuation: None,
                out: t.out,
                success_slot: None,
                value: Some(select),
                then_: None,
                else_: None,
                pattern: None,
                match_arm: None,
                diagnostic: false,
            })?;
            let unit = b.p.expr_unit()?;
            b.finish(Some(unit), t.state, t_cont, ExprSlot::IfElse(select))?;
            if has_value {
                b.emit(r.operands[1], t.state, exit_k, ExprSlot::IfThen(select))?;
            }
        } else if has_value {
            b.emit(r.operands[0], t.state, exit_k, t.out)?;
        } else {
            let u = b.p.expr_unit()?;
            let jmp = b.cps_jump(exit_k, Some(u))?;
            b.write_slot(t.out, jmp);
        }
        return Ok(());
    }

    if matches!(tag, SyntaxKind::If | SyntaxKind::LineIf)
        || matches!(
            r.operation,
            TokenKind::And | TokenKind::Or | TokenKind::Implies
        )
    {
        let rty = b.lower_ty(r.ty)?;
        let (k, out) = branch_join(b, t, rty)?;
        if matches!(tag, SyntaxKind::If | SyntaxKind::LineIf) {
            let pairs = r.operands.len() / 2;
            let mut pos = out;
            for i in 0..pairs {
                let select = b.p.expr_if(UNSET_EXPR, UNSET_EXPR, UNSET_EXPR)?;
                b.write_slot(pos, select);
                b.push_task(Task {
                    kind: TaskKind::Condition,
                    row: r.operands[2 * i],
                    state: t.state,
                    continuation: None,
                    out: pos,
                    success_slot: None,
                    value: Some(select),
                    then_: None,
                    else_: None,
                    pattern: None,
                    match_arm: None,
                    diagnostic: false,
                })?;
                b.emit(r.operands[2 * i + 1], t.state, k, ExprSlot::IfThen(select))?;
                pos = ExprSlot::IfElse(select);
            }
            if !r.operands.len().is_multiple_of(2) {
                b.emit(r.operands[r.operands.len() - 1], t.state, k, pos)?;
            } else {
                let u = b.p.expr_unit()?;
                b.finish(Some(u), t.state, k, pos)?;
            }
        } else {
            let select = b.p.expr_if(UNSET_EXPR, UNSET_EXPR, UNSET_EXPR)?;
            let is_or = r.operation == TokenKind::Or;
            b.push_task(Task {
                kind: TaskKind::Condition,
                row: r.operands[0],
                state: t.state,
                continuation: None,
                out,
                success_slot: None,
                value: Some(select),
                then_: None,
                else_: None,
                pattern: None,
                match_arm: None,
                diagnostic: false,
            })?;
            let rhs_slot = if is_or {
                ExprSlot::IfElse(select)
            } else {
                ExprSlot::IfThen(select)
            };
            let short_slot = if is_or {
                ExprSlot::IfThen(select)
            } else {
                ExprSlot::IfElse(select)
            };
            b.emit(r.operands[1], t.state, k, rhs_slot)?;
            let short_val = b.p.expr_bool(r.operation != TokenKind::And)?;
            b.finish(Some(short_val), t.state, k, short_slot)?;
        }
        return Ok(());
    }

    if tag == SyntaxKind::Stop {
        let Some(msg_nid) =
            b.p.syntax_node(b.syntax_module, r.syntax)
                .children
                .first()
                .copied()
                .flatten()
        else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "stop missing message literal");
        };
        let text_src = b.p.types_overlay.builtin("Text");
        let text_ty = b.lower_ty(text_src)?;
        let lit = lower_literal(b.p, b.syntax_module, msg_nid, text_ty, false)?;
        let msg = match b.p.arena.expr(lit) {
            Expr::Lit(Lit::Text(s)) => s.clone(),
            _ => String::new(),
        };
        let stop_e = b.p.expr_stop(&msg)?;
        b.write_slot(t.out, stop_e);
        return Ok(());
    }

    if matches!(
        tag,
        SyntaxKind::Int
            | SyntaxKind::Decimal
            | SyntaxKind::Char
            | SyntaxKind::Text
            | SyntaxKind::True
            | SyntaxKind::False
    ) || r.native.as_deref() == Some("Unit")
    {
        let rty = b.lower_ty(r.ty)?;
        let lit = lower_literal(b.p, b.syntax_module, r.syntax, rty, false)?;
        return b.finish(Some(lit), t.state, t_cont, t.out);
    }

    if r.operation == TokenKind::Minus && r.operands.len() == 1 {
        let op0 = b.row(r.operands[0]);
        let op0_kind = lower_cps_kind(b, op0);
        if matches!(op0_kind, SyntaxKind::Int | SyntaxKind::Decimal) {
            let op0_syn = op0.syntax;
            let rty = b.lower_ty(r.ty)?;
            let lit = lower_literal(b.p, b.syntax_module, op0_syn, rty, true)?;
            return b.finish(Some(lit), t.state, t_cont, t.out);
        }
    }

    if let Some(bid) = r.binding
        && r.operands.is_empty()
    {
        let v = b.p.expr_var(b.binding_name(bid))?;
        return b.finish(Some(v), t.state, t_cont, t.out);
    }

    if let Some(lam_id) = r.lambda {
        let lam = b.source.lambda(lam_id).clone();
        let fn_ty = b.lower_ty(r.ty)?;
        let (param_tys, ret_ty) = match b.p.core_ty(fn_ty).clone() {
            Ty::Fn { params, ret } => (params, ret),
            _ => {
                return b.p.refuse(ErrorCode::LowerUnfinished, "lambda non-fn type");
            }
        };
        let mut core_params = Vec::with_capacity(lam.parameters.len());
        for (i, &p_opt) in lam.parameters.iter().enumerate() {
            let pname = match p_opt {
                Some(bid) => b.binding_name(bid),
                None => b.p.fresh()?,
            };
            core_params.push(b.p.make_param(pname, param_tys[i])?);
        }
        let lam_e = b.p.alloc_expr(Expr::Lam {
            params: core_params,
            body: UNSET_EXPR,
        })?;
        b.binder_slots(lam_e, lam.parameters.len())?;
        for (i, &p_opt) in lam.parameters.iter().enumerate() {
            if let Some(bid) = p_opt {
                b.set_binder(lam_e, i, bid)?;
            }
        }
        let num = b.p.number_text(u64::from(lam.number))?;
        let meta_name = b.meta_name.clone();
        let cname = b.p.concat_text(&meta_name, ".fn.", &num)?;
        b.p.set_closure(lam_e, cname);

        let mut body_slot = ExprSlot::LamBody(lam_e);
        for &cap_bid in &lam.captures {
            let cname = b.binding_name(cap_bid);
            let cv = b.p.expr_var(cname.clone())?;
            let snap = b.cps_let(cname, cv)?;
            b.set_binder(snap, 0, cap_bid)?;
            b.write_slot(body_slot, snap);
            body_slot = ExprSlot::LetBody(snap);
        }

        let end_k = b.alloc_cont(ContinuationKind::End)?;
        if returns(b, lam.body)? {
            let ret_k = b.alloc_cont(ContinuationKind::Jump)?;
            let rname = b.p.fresh()?;
            {
                let c = b.cont_mut(ret_k);
                c.result = true;
                c.name = rname;
            }
            let (ret_join, param_name) = b.cps_join(ret_k, Some(ret_ty))?;
            let rpv = b.p.expr_var(param_name.unwrap_or_default())?;
            if let Expr::Join { body, .. } = b.p.arena.expr_mut(ret_join) {
                *body = rpv;
            }
            b.write_slot(body_slot, ret_join);
            b.emit(
                lam.body,
                State {
                    return_k: Some(ret_k),
                    ..State::default()
                },
                end_k,
                ExprSlot::JoinRest(ret_join),
            )?;
        } else {
            b.emit(lam.body, State::default(), end_k, body_slot)?;
        }
        return b.finish(Some(lam_e), t.state, t_cont, t.out);
    }

    if r.operands.is_empty()
        && let Some(sym) = r.symbol
        && b.p.program.interfaces.symbol(sym).decl_kind == SyntaxKind::Const
    {
        let gname = lower_global_name(b.p, sym)?;
        let e = b.p.expr_global(gname)?;
        return b.finish(Some(e), t.state, t_cont, t.out);
    }

    let qualified = r
        .native
        .as_deref()
        .is_some_and(|n| r.receiver.is_none() && (n.contains('.') || lower_cps_ability_native(&r)));
    if r.operands.is_empty()
        && (r
            .symbol
            .is_some_and(|s| b.p.program.interfaces.symbol(s).decl_kind == SyntaxKind::Fn)
            || (qualified && b.p.source_type(r.ty).kind == SourceTypeKind::Function))
    {
        let sig = b.lower_ty(r.ty)?;
        let param_tys = match b.p.core_ty(sig).clone() {
            Ty::Fn { params, .. } => params,
            _ => {
                return b
                    .p
                    .refuse(ErrorCode::LowerUnfinished, "function ref non-fn type");
            }
        };
        let mut core_params = Vec::with_capacity(param_tys.len());
        let mut args = Vec::with_capacity(param_tys.len());
        for pty in param_tys {
            let pname = b.p.fresh()?;
            args.push(b.p.expr_var(pname.clone())?);
            core_params.push(b.p.make_param(pname, pty)?);
        }
        let body = lower_cps_application(b, t.row, args)?;
        let lam_e = b.p.alloc_expr(Expr::Lam {
            params: core_params,
            body,
        })?;
        let row_num = b.p.number_text(u64::from(t.row.0))?;
        let meta_name = b.meta_name.clone();
        let cname = b.p.concat_text(&meta_name, ".ref.", &row_num)?;
        b.p.set_closure(lam_e, cname);
        return b.finish(Some(lam_e), t.state, t_cont, t.out);
    }

    let mut skip = usize::from(lower_cps_direct_function(b, &r));
    if qualified
        && !r.operands.is_empty()
        && b.p.source_type(b.row(r.operands[0]).ty).kind == SourceTypeKind::Function
    {
        skip = 1;
    }
    if r.operands.len() == skip {
        let app = lower_cps_application(b, t.row, Vec::new())?;
        return b.finish(Some(app), t.state, t_cont, t.out);
    }

    let arg_buf = b.alloc_arg_buf(r.operands.len() - skip)?;
    let k = b.alloc_cont(ContinuationKind::Arguments)?;
    let kname = b.p.fresh()?;
    {
        let c = b.cont_mut(k);
        c.row = Some(t.row);
        c.skip = skip;
        c.next = Some(t_cont);
        c.name = kname;
        c.arguments = Some(arg_buf);
    }
    b.emit(r.operands[skip], t.state, k, t.out)
}

fn run_cps_loop(b: &mut BodyLowerer<'_, '_>) -> LowerResult<()> {
    while let Some(mut t) = b.tasks.pop() {
        b.p.work(1)?;
        match t.kind {
            TaskKind::Row | TaskKind::Condition => {
                let syn = b.row(t.row).syntax;
                b.p.span = b.syntax_span(syn);
            }
            TaskKind::Finish => {
                if let Some(val) = t.value
                    && let Some(sp) = b.p.provenance[val.0 as usize].span
                {
                    b.p.span = sp;
                }
            }
            TaskKind::Pattern | TaskKind::Arm | TaskKind::Copy => {}
        }

        match t.kind {
            TaskKind::Row => row_task(b, t)?,
            TaskKind::Finish => finish_task(b, t)?,
            TaskKind::Condition => {
                if let Some(val) = t.value
                    && let Expr::If { then_, else_, .. } = *b.p.arena.expr(val)
                {
                    t.then_ = Some(then_);
                    t.else_ = Some(else_);
                }
                t.row = unwrap_condition(b, t.row)?;
                let r = b.row(t.row).clone();
                b.p.span = b.syntax_span(r.syntax);
                require_concrete_proofs(b, &r.abilities)?;

                if matches!(r.operation, TokenKind::Or | TokenKind::Implies)
                    && let Some(th) = t.then_
                {
                    t.then_ = Some(share_target(b, t, th, &mut t.out)?);
                }
                if (matches!(r.operation, TokenKind::And | TokenKind::Or) || r.pattern.is_some())
                    && let Some(el) = t.else_
                {
                    t.else_ = Some(share_target(b, t, el, &mut t.out)?);
                }

                if r.operation == TokenKind::And
                    || (r.operation == TokenKind::Or && r.or_kind != CheckedOr::Option)
                    || r.operation == TokenKind::Implies
                {
                    let is_or = r.operation == TokenKind::Or;
                    let then_init = if is_or {
                        t.then_.unwrap_or(UNSET_EXPR)
                    } else {
                        UNSET_EXPR
                    };
                    let else_init = if is_or {
                        UNSET_EXPR
                    } else if r.operation == TokenKind::Implies {
                        t.then_.unwrap_or(UNSET_EXPR)
                    } else {
                        t.else_.unwrap_or(UNSET_EXPR)
                    };
                    let select = b.p.expr_if(UNSET_EXPR, then_init, else_init)?;
                    b.push_task(Task {
                        kind: TaskKind::Condition,
                        row: r.operands[0],
                        state: t.state,
                        continuation: None,
                        out: t.out,
                        success_slot: None,
                        value: Some(select),
                        then_: None,
                        else_: None,
                        pattern: None,
                        match_arm: None,
                        diagnostic: t.diagnostic && r.operation == TokenKind::And,
                    })?;
                    b.push_task(Task {
                        kind: TaskKind::Condition,
                        row: r.operands[1],
                        state: t.state,
                        continuation: None,
                        out: if is_or {
                            ExprSlot::IfElse(select)
                        } else {
                            ExprSlot::IfThen(select)
                        },
                        success_slot: None,
                        value: None,
                        then_: t.then_,
                        else_: t.else_,
                        pattern: None,
                        match_arm: None,
                        diagnostic: t.diagnostic,
                    })?;
                } else if t.diagnostic && r.operation == TokenKind::Eq {
                    let arg_buf = b.alloc_arg_buf(2)?;
                    let k = b.alloc_cont(ContinuationKind::Arguments)?;
                    let kname = b.p.fresh()?;
                    {
                        let c = b.cont_mut(k);
                        c.row = Some(t.row);
                        c.arguments = Some(arg_buf);
                        c.name = kname;
                        c.then_ = t.then_;
                        c.diagnostic = true;
                    }
                    b.emit(r.operands[0], t.state, k, t.out)?;
                } else {
                    let kind = if r.pattern.is_none() {
                        ContinuationKind::Select
                    } else {
                        ContinuationKind::Pattern
                    };
                    let k = b.alloc_cont(kind)?;
                    {
                        let c = b.cont_mut(k);
                        c.then_ = t.then_;
                        c.else_ = t.else_;
                        c.pattern = r.pattern;
                        c.row = Some(t.row);
                        c.diagnostic = t.diagnostic && r.pattern.is_some();
                    }
                    let target_row = if r.pattern.is_none() {
                        t.row
                    } else {
                        r.operands[0]
                    };
                    b.emit(target_row, t.state, k, t.out)?;
                }
            }
            TaskKind::Pattern => pattern_task(b, t)?,
            TaskKind::Arm => {
                t.value = t.success_slot.and_then(|s| b.roots[s]);
                arm_task(b, t)?;
            }
            TaskKind::Copy => {
                if let Some(val) = t.success_slot.and_then(|s| b.roots[s]) {
                    b.write_slot(t.out, val);
                }
            }
        }
    }
    Ok(())
}

/// Lowers one function, spec, or service body where `source` borrows from `p.program` (`'b`).
pub(crate) fn lower_body_ref<'b>(
    p: &mut Lowering<'b>,
    inst_idx: usize,
    syntax_module: ModuleId,
    source: &'b CheckedBody,
    parameters: &[CheckedBindingId],
) -> LowerResult<()> {
    let (owner, subst, depth, meta_name, fn_kind, ret_ty, spec_opt) = {
        let inst = &p.instances[inst_idx];
        (
            inst.owner,
            inst.subst,
            inst.depth,
            inst.meta.name.clone(),
            inst.fun.kind,
            inst.fun.ret,
            inst.spec,
        )
    };
    p.charge_alloc(source.bindings.len(), 16, 8)?;
    let mut names = Vec::with_capacity(source.bindings.len());
    for _ in 0..source.bindings.len() {
        names.push(p.fresh()?);
    }

    let mut core_params = Vec::with_capacity(parameters.len());
    let mut meta_params = Vec::with_capacity(parameters.len());
    for &bid in parameters {
        let binding = source.binding(bid);
        let pty = lower_type(p, binding.ty, subst)?;
        core_params.push(p.make_param(names[bid.index()].clone(), pty)?);
        meta_params.push(Some(p.copy_text(&binding.name)?));
    }
    p.instances[inst_idx].fun.params = core_params;
    p.instances[inst_idx].meta.body.params = meta_params;

    let mut b = BodyLowerer {
        p,
        instance_idx: Some(inst_idx),
        owner,
        syntax_module,
        subst,
        depth,
        meta_name,
        source,
        names,
        var_nodes: Vec::new(),
        loops: Vec::new(),
        conts: Vec::new(),
        arg_bufs: Vec::new(),
        split_fields: Vec::new(),
        anchor_vals: Vec::new(),
        roots: vec![None],
        tasks: Vec::new(),
    };

    let root_slot = ExprSlot::Root(0);
    let spec_kind = spec_opt.map(|sr| b.p.spec_ref(sr).kind);

    if spec_kind == Some(CheckedSpecKind::Anchor) {
        let Some(sr) = spec_opt else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "anchor missing spec ref");
        };
        let vals_raw = b.p.spec_ref(sr).values.clone();
        let mut values: Vec<AnchorValue> = vals_raw
            .into_iter()
            .enumerate()
            .map(|(slot, expression)| AnchorValue { expression, slot })
            .collect();
        values.sort_by_key(|v| v.expression);
        let first_expr = values[0].expression;
        let vcount = values.len();
        let av_id = b.alloc_anchor_vals(values);
        let arg_buf = b.alloc_arg_buf(vcount)?;
        let k = b.alloc_cont(ContinuationKind::Anchor)?;
        {
            let c = b.cont_mut(k);
            c.values = Some(av_id);
            c.arguments = Some(arg_buf);
        }
        b.emit(first_expr, State::default(), k, root_slot)?;
    } else if spec_kind == Some(CheckedSpecKind::Promise) {
        let Some(sr) = spec_opt else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "promise missing spec ref");
        };
        let conds = b.p.spec_ref(sr).conditions.clone();
        let mut track_implies = false;
        if b.p.options.promise_vacuity {
            for cond in conds.iter().take(62) {
                let unwrapped = unwrap_condition(&mut b, cond.expression)?;
                if b.row(unwrapped).operation == TokenKind::Implies {
                    track_implies = true;
                }
            }
        }
        if track_implies {
            b.p.span = conds[0].text.span;
            let mask_name = b.p.fresh()?;
            let zero = b.p.expr_int(0)?;
            let init = b.cps_let(mask_name.clone(), zero)?;
            b.write_slot(root_slot, init);
            start_promise_step(&mut b, 0, Some(mask_name), ExprSlot::LetBody(init))?;
        } else {
            start_promise_step(&mut b, 0, None, root_slot)?;
        }
    } else if fn_kind == FnKind::Example {
        let Some(sr) = spec_opt else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "example missing spec ref");
        };
        let syn = b.p.spec_ref(sr).syntax;
        b.p.span = b.syntax_span(syn);
        let unit = b.p.expr_unit()?;
        let msg = b.p.expr_text("example failed")?;
        let fail = b.p.expr_fail(msg)?;
        let e = b.p.expr_if(UNSET_EXPR, unit, fail)?;
        let Some(root_expr) = source.root else {
            return b
                .p
                .refuse(ErrorCode::LowerUnfinished, "example body missing root");
        };
        b.push_task(Task {
            kind: TaskKind::Condition,
            row: root_expr,
            state: State::default(),
            continuation: None,
            out: root_slot,
            success_slot: None,
            value: Some(e),
            then_: None,
            else_: None,
            pattern: None,
            match_arm: None,
            diagnostic: true,
        })?;
    } else {
        let Some(root_expr) = source.root else {
            return b.p.refuse(ErrorCode::LowerUnfinished, "body missing root");
        };
        let end_k = b.alloc_cont(ContinuationKind::End)?;
        if returns(&mut b, root_expr)? {
            let ret_k = b.alloc_cont(ContinuationKind::Jump)?;
            let rname = b.p.fresh()?;
            {
                let c = b.cont_mut(ret_k);
                c.result = true;
                c.name = rname;
            }
            let (ret_join, param_name) = b.cps_join(ret_k, Some(ret_ty))?;
            let rpv = b.p.expr_var(param_name.unwrap_or_default())?;
            if let Expr::Join { body, .. } = b.p.arena.expr_mut(ret_join) {
                *body = rpv;
            }
            b.write_slot(root_slot, ret_join);
            b.emit(
                root_expr,
                State {
                    return_k: Some(ret_k),
                    ..State::default()
                },
                end_k,
                ExprSlot::JoinRest(ret_join),
            )?;
        } else {
            b.emit(root_expr, State::default(), end_k, root_slot)?;
        }
    }

    run_cps_loop(&mut b)?;
    let body_expr = b.roots[0].unwrap_or(UNSET_EXPR);
    p.instances[inst_idx].fun.body = body_expr;
    p.instances[inst_idx].body_set = true;
    Ok(())
}

/// Lowers one constant initializer body and returns its root Core [`ExprId`].
pub(crate) fn lower_constant_body<'b>(
    p: &mut Lowering<'b>,
    const_idx: usize,
    owner: ModuleId,
    meta_name: &str,
    ret_ty: TyId,
) -> LowerResult<ExprId> {
    let source = &p.program.constants[const_idx].initializer;
    p.charge_alloc(source.bindings.len(), 16, 8)?;
    let mut names = Vec::with_capacity(source.bindings.len());
    for _ in 0..source.bindings.len() {
        names.push(p.fresh()?);
    }

    let mut b = BodyLowerer {
        p,
        instance_idx: None,
        owner,
        syntax_module: owner,
        subst: None,
        depth: 0,
        meta_name: meta_name.to_string(),
        source,
        names,
        var_nodes: Vec::new(),
        loops: Vec::new(),
        conts: Vec::new(),
        arg_bufs: Vec::new(),
        split_fields: Vec::new(),
        anchor_vals: Vec::new(),
        roots: vec![None],
        tasks: Vec::new(),
    };
    let root_slot = ExprSlot::Root(0);
    let Some(root_expr) = source.root else {
        return b
            .p
            .refuse(ErrorCode::LowerUnfinished, "constant body missing root");
    };
    let end_k = b.alloc_cont(ContinuationKind::End)?;
    if returns(&mut b, root_expr)? {
        let ret_k = b.alloc_cont(ContinuationKind::Jump)?;
        let rname = b.p.fresh()?;
        {
            let c = b.cont_mut(ret_k);
            c.result = true;
            c.name = rname;
        }
        let (ret_join, param_name) = b.cps_join(ret_k, Some(ret_ty))?;
        let rpv = b.p.expr_var(param_name.unwrap_or_default())?;
        if let Expr::Join { body, .. } = b.p.arena.expr_mut(ret_join) {
            *body = rpv;
        }
        b.write_slot(root_slot, ret_join);
        b.emit(
            root_expr,
            State {
                return_k: Some(ret_k),
                ..State::default()
            },
            end_k,
            ExprSlot::JoinRest(ret_join),
        )?;
    } else {
        b.emit(root_expr, State::default(), end_k, root_slot)?;
    }
    run_cps_loop(&mut b)?;
    Ok(b.roots[0].unwrap_or(UNSET_EXPR))
}

/// Lowers one source function instance and reserves any attached promises.
pub(crate) fn lower_function(p: &mut Lowering<'_>, inst_idx: usize) -> LowerResult<()> {
    let Some(func_id) = p.instances[inst_idx].source else {
        return Ok(());
    };
    let (sym, sig_ty_opt, params, subst) = {
        let f = p.program.function(func_id);
        let sig_ty = p.program.signatures.functions[f.signature.index()].ty;
        (
            f.symbol,
            sig_ty,
            f.parameters.clone(),
            p.instances[inst_idx].subst,
        )
    };
    let decl_span = p.program.interfaces.symbol(sym).declaration_span;
    let syntax_module = p.program.interfaces.symbol(sym).module;
    p.span = decl_span;
    let Some(sig_ty) = sig_ty_opt else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "function missing signature type",
        );
    };
    let fn_core_ty = lower_type(p, sig_ty, subst)?;
    let ret = match *p.core_ty(fn_core_ty) {
        Ty::Fn { ret, .. } => ret,
        _ => {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "function signature is not a function type",
            );
        }
    };
    p.instances[inst_idx].fun.ret = ret;
    let body = &p.program.function(func_id).body;
    lower_body_ref(p, inst_idx, syntax_module, body, &params)?;
    lower_subject_specs(p, inst_idx)?;
    Ok(())
}
