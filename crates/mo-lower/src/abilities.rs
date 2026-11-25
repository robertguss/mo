//! Derived `Text`, `Order`, `Json` (`to_json` / `from_json` / `schema`) ability synthesis
//! and `Sqlite.decode*` lowering (`src/lower/abilities.c` parity).

use mo_check::{
    AbilityImplementationKind, CheckedAbilityUse, ModuleId, ProofId, SourceAbility, SourceType,
    SourceTypeKind, SymRef, TypeArgument, TypeId, TypeOwner,
};
use mo_core::{Arm, CtorDecl, Expr, ExprId, FnKind, ListOp, MapOp, Prim, Ty, TyId};
use mo_def::ErrorCode;
use mo_def::core_ops::{
    CoreCharOp, CoreFloatOp, CoreIntOp, CoreListNullaryOp, CoreMapNullaryOp, CoreSetOp, CoreTextOp,
};
use mo_def::limits::MO_LIMIT_CORE_NODES;
use mo_syntax::{SyntaxKind, TokenKind};

use crate::context::{DerivedEntry, DerivedKind, LowerResult, Lowering, SubstId};
use crate::scalar::{lower_operator, lower_to_text};
use crate::types::{
    lower_ctors, lower_customizer, lower_customizer_fn, lower_instance, lower_proof, lower_type,
    lower_type_name,
};

fn concat(p: &mut Lowering<'_>, left: ExprId, right: ExprId) -> LowerResult<ExprId> {
    p.expr_prim(Prim::Text(CoreTextOp::Concat), vec![left, right])
}

fn part_text(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    part: usize,
    value: ExprId,
) -> LowerResult<ExprId> {
    let (mut proof, mut subst, owner, depth) = {
        let d = &p.derived[derived_idx];
        let pr = p.proof(d.proof).parts[part];
        (pr, d.subst, d.owner, d.depth)
    };
    lower_proof(p, &mut proof, &mut subst)?;
    let (kind, proof_ty) = {
        let imp = p.proof(proof);
        (imp.kind, imp.ty)
    };
    if kind == AbilityImplementationKind::Native {
        let core_ty = lower_type(p, proof_ty, subst)?;
        match p.core_ty(core_ty) {
            Ty::Text => return p.expr_prim(Prim::Text(CoreTextOp::Quoted), vec![value]),
            Ty::Char => return p.expr_prim(Prim::Char(CoreCharOp::Quoted), vec![value]),
            _ => {}
        }
    }
    lower_to_text(p, proof, subst, owner, depth, value)
}

fn find_library_module(
    p: &mut Lowering<'_>,
    mod_name: &str,
    err_msg: &'static str,
) -> LowerResult<ModuleId> {
    let count = p.program.interfaces.scopes.len();
    for i in 0..count {
        p.work(1)?;
        let scope = &p.program.interfaces.scopes[i];
        if scope.library && scope.name.as_deref() == Some(mod_name) {
            return Ok(ModuleId(i as u32));
        }
    }
    p.refuse(ErrorCode::LowerUnfinished, err_msg)
}

fn library_data_source(
    p: &mut Lowering<'_>,
    mod_name: &str,
    type_name: &str,
    err_msg: &'static str,
) -> LowerResult<TypeId> {
    let mod_id = find_library_module(p, mod_name, err_msg)?;
    let sym_idx =
        p.program.interfaces.scopes[mod_id.index()].find_symbol_index(type_name.as_bytes());
    let Some(sidx) = sym_idx else {
        return p.refuse(ErrorCode::LowerUnfinished, err_msg);
    };
    let sym = SymRef::new(mod_id, sidx);
    for d in &p.program.signatures.data {
        p.work(1)?;
        if p.source_type(d.ty).owner == TypeOwner::Source(sym) {
            return Ok(d.ty);
        }
    }
    p.refuse(ErrorCode::LowerUnfinished, err_msg)
}

fn json_data_source(p: &mut Lowering<'_>, name: &str) -> LowerResult<TypeId> {
    library_data_source(p, "Json", name, "Json ability needs the library")
}

fn sqlite_data_source(p: &mut Lowering<'_>, name: &str) -> LowerResult<TypeId> {
    library_data_source(p, "Sqlite", name, "Sqlite decode needs the library")
}

fn result_source_type(p: &mut Lowering<'_>, ok_type: TypeId) -> LowerResult<TypeId> {
    let err_type = json_data_source(p, "JsonDecodeError")?;
    let (ok_height, ok_params) = {
        let s = p.source_type(ok_type);
        (s.height, s.parameters)
    };
    Ok(p.types_overlay.alloc(SourceType {
        kind: SourceTypeKind::Named,
        name: "Result".to_string(),
        module: None,
        owner: TypeOwner::None,
        arguments: vec![ok_type, err_type],
        height: ok_height.saturating_add(1),
        parameters: ok_params,
    }))
}

fn reserve_kind(
    p: &mut Lowering<'_>,
    proof: ProofId,
    subst: Option<SubstId>,
    mut owner: ModuleId,
    depth: u32,
    kind: DerivedKind,
) -> LowerResult<usize> {
    let proof_ty = p.proof(proof).ty;
    let ty = lower_type(p, proof_ty, subst)?;
    {
        let st = p.source_type(proof_ty);
        if st.kind == SourceTypeKind::Named
            && let Some(m) = st.module
        {
            owner = m;
        }
    }

    let key = (kind, ty, owner);
    if let Some(&existing) = p.derived_index.get(&key) {
        p.work(1)?;
        return Ok(existing);
    }
    if p.derived.len() as u64 >= MO_LIMIT_CORE_NODES {
        return p.refuse(ErrorCode::LowerWork, "too many derived ability functions");
    }

    let prefix = match kind {
        DerivedKind::Text => ".%to_text(",
        DerivedKind::Order => ".%compare(",
        DerivedKind::ToJson => ".%to_json(",
        DerivedKind::FromJson => ".%from_json(",
        DerivedKind::JsonSchema => ".%json_schema(",
    };
    let mod_name = p.module_name(owner).to_string();
    let ty_name = lower_type_name(p, ty)?;
    let closed_ty = p.concat_text(&ty_name, "", ")")?;
    let fn_name = p.concat_text(&mod_name, prefix, &closed_ty)?;

    let text_src = p.types_overlay.builtin("Text");
    let bool_src = p.types_overlay.builtin("Bool");
    let order_src = p.types_overlay.builtin("Ordering");
    let list_text_src = p.types_overlay.builtin_applied("List", vec![text_src]);

    let (ret_ty, param_tys) = match kind {
        DerivedKind::Text => (lower_type(p, text_src, None)?, vec![ty]),
        DerivedKind::Order => (lower_type(p, order_src, None)?, vec![ty, ty]),
        DerivedKind::ToJson => {
            let j_src = json_data_source(p, "Json")?;
            (lower_type(p, j_src, None)?, vec![ty])
        }
        DerivedKind::FromJson => {
            let r_src = result_source_type(p, proof_ty)?;
            let j_src = json_data_source(p, "Json")?;
            (
                lower_type(p, r_src, subst)?,
                vec![
                    lower_type(p, j_src, None)?,
                    lower_type(p, text_src, None)?,
                    lower_type(p, bool_src, None)?,
                ],
            )
        }
        DerivedKind::JsonSchema => {
            let j_src = json_data_source(p, "Json")?;
            (
                lower_type(p, j_src, None)?,
                vec![lower_type(p, list_text_src, None)?],
            )
        }
    };

    let inst_idx = p.push_instance(
        owner,
        fn_name.clone(),
        FnKind::Function,
        ret_ty,
        fn_name,
        p.lower_span(p.span),
    )?;
    p.instances[inst_idx].depth = depth;
    let mut params = Vec::with_capacity(param_tys.len());
    for pty in param_tys {
        let pname = p.fresh()?;
        params.push(p.make_param(pname, pty)?);
    }
    let meta_params = vec![None; params.len()];
    p.instances[inst_idx].fun.params = params;
    p.instances[inst_idx].meta.body.params = meta_params;

    let d_idx = p.derived.len();
    p.charge_alloc(1, 64, 8)?;
    p.derived.push(DerivedEntry {
        kind,
        ty,
        proof,
        subst,
        owner,
        depth,
        span: p.span,
        instance: inst_idx,
    });
    p.derived_index.insert(key, d_idx);
    Ok(d_idx)
}

pub(crate) fn lower_derived_text(
    p: &mut Lowering<'_>,
    proof: ProofId,
    subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    value: ExprId,
) -> LowerResult<ExprId> {
    let d_idx = reserve_kind(p, proof, subst, owner, depth, DerivedKind::Text)?;
    let fn_name = p.instances[p.derived[d_idx].instance].fun.name.clone();
    p.expr_call(fn_name, vec![value])
}

fn ordering_con(p: &mut Lowering<'_>, name: &str) -> LowerResult<ExprId> {
    let order_src = p.types_overlay.builtin("Ordering");
    let order_ty = lower_type(p, order_src, None)?;
    let ctor = p.copy_text(name)?;
    p.expr_con(order_ty, ctor, Vec::new())
}

pub(crate) fn lower_compare(
    p: &mut Lowering<'_>,
    mut proof: ProofId,
    mut subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    left: ExprId,
    right: ExprId,
) -> LowerResult<ExprId> {
    debug_assert_eq!(p.proof(proof).ability, SourceAbility::Order);
    lower_proof(p, &mut proof, &mut subst)?;
    let kind = p.proof(proof).kind;
    let proof_ty = p.proof(proof).ty;
    match kind {
        AbilityImplementationKind::Derived => {
            let d_idx = reserve_kind(p, proof, subst, owner, depth, DerivedKind::Order)?;
            let name = p.instances[p.derived[d_idx].instance].fun.name.clone();
            p.expr_call(name, vec![left, right])
        }
        AbilityImplementationKind::Function => {
            let inst = lower_customizer(p, proof, subst, owner, depth)?;
            let name = p.instances[inst].fun.name.clone();
            p.expr_call(name, vec![left, right])
        }
        AbilityImplementationKind::Parameter => p.refuse(
            ErrorCode::LowerUnfinished,
            "abstract ordering is not substituted yet",
        ),
        AbilityImplementationKind::Native => {
            let ty = lower_type(p, proof_ty, subst)?;
            if matches!(p.core_ty(ty), Ty::Unit) {
                return ordering_con(p, "Equal");
            }
            let lt_op = lower_operator(p, TokenKind::Lt, ty, Some(ty))?;
            let gt_op = lower_operator(p, TokenKind::Gt, ty, Some(ty))?;
            let gt_cond = p.expr_prim(gt_op, vec![left, right])?;
            let gt_then = ordering_con(p, "Greater")?;
            let gt_else = ordering_con(p, "Equal")?;
            let greater = p.expr_if(gt_cond, gt_then, gt_else)?;
            let lt_cond = p.expr_prim(lt_op, vec![left, right])?;
            let lt_then = ordering_con(p, "Less")?;
            let less = p.expr_if(lt_cond, lt_then, greater)?;
            if matches!(p.core_ty(ty), Ty::Float) {
                let r_nan = p.expr_prim(Prim::Float(CoreFloatOp::IsNan), vec![right])?;
                let o_then = ordering_con(p, "Less")?;
                let other = p.expr_if(r_nan, o_then, less)?;

                let r_nan2 = p.expr_prim(Prim::Float(CoreFloatOp::IsNan), vec![right])?;
                let b_then = ordering_con(p, "Equal")?;
                let b_else = ordering_con(p, "Greater")?;
                let both = p.expr_if(r_nan2, b_then, b_else)?;

                let l_nan = p.expr_prim(Prim::Float(CoreFloatOp::IsNan), vec![left])?;
                p.expr_if(l_nan, both, other)
            } else {
                Ok(less)
            }
        }
    }
}

fn order_case(p: &mut Lowering<'_>, value: ExprId, equal_body: ExprId) -> LowerResult<ExprId> {
    let less_val = ordering_con(p, "Less")?;
    let greater_val = ordering_con(p, "Greater")?;
    let less_ctor = p.copy_text("Less")?;
    let equal_ctor = p.copy_text("Equal")?;
    let greater_ctor = p.copy_text("Greater")?;
    p.alloc_expr(Expr::Case {
        expr: value,
        arms: vec![
            Arm {
                ctor: less_ctor,
                names: Vec::new(),
                body: less_val,
            },
            Arm {
                ctor: equal_ctor,
                names: Vec::new(),
                body: equal_body,
            },
            Arm {
                ctor: greater_ctor,
                names: Vec::new(),
                body: greater_val,
            },
        ],
    })
}

fn fields_order(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    first: usize,
    left: &[ExprId],
    right: &[ExprId],
) -> LowerResult<ExprId> {
    debug_assert_eq!(left.len(), right.len());
    let (proof_id, subst, owner, depth) = {
        let d = &p.derived[derived_idx];
        (d.proof, d.subst, d.owner, d.depth)
    };
    let mut cur = ordering_con(p, "Equal")?;
    for i in (0..left.len()).rev() {
        p.work(1)?;
        let part_proof = p.proof(proof_id).parts[first + i];
        let cmp = lower_compare(p, part_proof, subst, owner, depth, left[i], right[i])?;
        cur = order_case(p, cmp, cur)?;
    }
    Ok(cur)
}

fn list_case(
    p: &mut Lowering<'_>,
    element: TyId,
    value: ExprId,
    none_body: ExprId,
    some_var: String,
    some_body: ExprId,
) -> LowerResult<ExprId> {
    let scrut = p.expr_prim(
        Prim::List {
            ty: element,
            op: ListOp::Nullary(CoreListNullaryOp::Uncons),
        },
        vec![value],
    )?;
    let none_ctor = p.copy_text("None")?;
    let some_ctor = p.copy_text("Some")?;
    p.alloc_expr(Expr::Case {
        expr: scrut,
        arms: vec![
            Arm {
                ctor: none_ctor,
                names: Vec::new(),
                body: none_body,
            },
            Arm {
                ctor: some_ctor,
                names: vec![some_var],
                body: some_body,
            },
        ],
    })
}

fn derive_list_order(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<ExprId> {
    let (inst_idx, elem_ty, part_proof, subst, owner, depth) = {
        let d = &p.derived[derived_idx];
        let elem = match *p.core_ty(d.ty) {
            Ty::List(e) => e,
            _ => return p.refuse(ErrorCode::LowerUnfinished, "expected list type"),
        };
        (
            d.instance,
            elem,
            p.proof(d.proof).parts[0],
            d.subst,
            d.owner,
            d.depth,
        )
    };
    let params = p.instances[inst_idx].fun.params.clone();
    let join_name = p.fresh()?;
    let p0 = params[0].name.clone();
    let p1 = params[1].name.clone();

    // Build inner branches
    let a_var = p.fresh()?;
    let b_var_empty = p.fresh()?;
    let b_var_cons = p.fresh()?;

    // Case left == None:
    let none_eq = ordering_con(p, "Equal")?;
    let none_less = ordering_con(p, "Less")?;
    let p1_v0 = p.expr_var(p1.clone())?;
    let left_none_body = list_case(p, elem_ty, p1_v0, none_eq, b_var_empty, none_less)?;

    // Case left == Some(a):
    let a_head_src = p.expr_var(a_var.clone())?;
    let a_head = p.expr_proj(a_head_src, 0)?;
    let b_head_src = p.expr_var(b_var_cons.clone())?;
    let b_head = p.expr_proj(b_head_src, 0)?;
    let cmp_head = lower_compare(p, part_proof, subst, owner, depth, a_head, b_head)?;
    let a_tail_src = p.expr_var(a_var.clone())?;
    let a_tail = p.expr_proj(a_tail_src, 1)?;
    let b_tail_src = p.expr_var(b_var_cons.clone())?;
    let b_tail = p.expr_proj(b_tail_src, 1)?;
    let again = p.alloc_expr(Expr::Jump {
        name: join_name.clone(),
        args: vec![a_tail, b_tail],
    })?;
    let cmp_case = order_case(p, cmp_head, again)?;
    let some_greater = ordering_con(p, "Greater")?;
    let p1_v1 = p.expr_var(p1.clone())?;
    let left_some_body = list_case(p, elem_ty, p1_v1, some_greater, b_var_cons, cmp_case)?;

    let p0_v = p.expr_var(p0.clone())?;
    let body = list_case(p, elem_ty, p0_v, left_none_body, a_var, left_some_body)?;

    let s0 = p.expr_var(p0)?;
    let s1 = p.expr_var(p1)?;
    let rest = p.alloc_expr(Expr::Jump {
        name: join_name.clone(),
        args: vec![s0, s1],
    })?;

    p.alloc_expr(Expr::Join {
        name: join_name,
        params,
        body,
        rest,
    })
}

fn derive_order(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<()> {
    let (span, ty, inst_idx) = {
        let d = &p.derived[derived_idx];
        (d.span, d.ty, d.instance)
    };
    p.span = span;
    let body = match p.core_ty(ty).clone() {
        Ty::List(_) => derive_list_order(p, derived_idx)?,
        Ty::Tuple(ref parts) => {
            let count = parts.len();
            let p0 = p.instances[inst_idx].fun.params[0].name.clone();
            let p1 = p.instances[inst_idx].fun.params[1].name.clone();
            let mut left = Vec::with_capacity(count);
            let mut right = Vec::with_capacity(count);
            for i in 0..count {
                let v0 = p.expr_var(p0.clone())?;
                let v1 = p.expr_var(p1.clone())?;
                left.push(p.expr_proj(v0, i as u64)?);
                right.push(p.expr_proj(v1, i as u64)?);
            }
            fields_order(p, derived_idx, 0, &left, &right)?
        }
        Ty::Data(_)
        | Ty::Option(_)
        | Ty::Result { .. }
        | Ty::Ordering
        | Ty::FileError
        | Ty::Entry
        | Ty::EntryKind
        | Ty::SimulationError
        | Ty::NetworkError
        | Ty::SqliteValue
        | Ty::SqliteRows
        | Ty::SqliteError => {
            let ctors = lower_ctors(p, ty)?;
            let p0 = p.instances[inst_idx].fun.params[0].name.clone();
            let p1 = p.instances[inst_idx].fun.params[1].name.clone();
            let mut outer_arms = Vec::with_capacity(ctors.len());
            let mut first = 0usize;
            for (i, ctor) in ctors.iter().enumerate() {
                p.work(1)?;
                let fcount = ctor.fields.len();
                let mut left_names = Vec::with_capacity(fcount);
                let mut left_exprs = Vec::with_capacity(fcount);
                for _ in 0..fcount {
                    let vn = p.fresh()?;
                    left_exprs.push(p.expr_var(vn.clone())?);
                    left_names.push(vn);
                }
                let mut inner_arms = Vec::with_capacity(ctors.len());
                for (j, other_ctor) in ctors.iter().enumerate() {
                    p.work(1)?;
                    let mut right_names = Vec::with_capacity(other_ctor.fields.len());
                    for _ in 0..other_ctor.fields.len() {
                        right_names.push(p.fresh()?);
                    }
                    let arm_body = if i != j {
                        ordering_con(p, if i < j { "Less" } else { "Greater" })?
                    } else {
                        let mut right_exprs = Vec::with_capacity(fcount);
                        for rn in &right_names {
                            right_exprs.push(p.expr_var(rn.clone())?);
                        }
                        fields_order(p, derived_idx, first, &left_exprs, &right_exprs)?
                    };
                    inner_arms.push(Arm {
                        ctor: other_ctor.name.clone(),
                        names: right_names,
                        body: arm_body,
                    });
                }
                let p1_v = p.expr_var(p1.clone())?;
                let inner_case = p.alloc_expr(Expr::Case {
                    expr: p1_v,
                    arms: inner_arms,
                })?;
                outer_arms.push(Arm {
                    ctor: ctor.name.clone(),
                    names: left_names,
                    body: inner_case,
                });
                first += fcount;
            }
            let p0_v = p.expr_var(p0)?;
            p.alloc_expr(Expr::Case {
                expr: p0_v,
                arms: outer_arms,
            })?
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
        | Ty::Map { .. }
        | Ty::Set(_)
        | Ty::Simulation
        | Ty::Fn { .. }
        | Ty::Cap(_) => {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "this derived order shape is not lowered yet",
            );
        }
    };
    p.instances[inst_idx].fun.body = body;
    p.instances[inst_idx].body_set = true;
    Ok(())
}

fn fields_text(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    first: usize,
    values: &[ExprId],
    field_labels: Option<&[String]>,
    opening: &str,
    closing: &str,
) -> LowerResult<ExprId> {
    if values.is_empty() {
        let combined = p.concat_text(opening, "", closing)?;
        return p.expr_text(&combined);
    }
    let count = values.len();
    let mut let_bindings: Vec<(String, ExprId)> = Vec::with_capacity(count);
    let mut pieces: Vec<ExprId> = Vec::with_capacity(2 * count + 1);
    pieces.push(p.expr_text(opening)?);

    for (i, &val) in values.iter().enumerate() {
        p.work(1)?;
        let vname = p.fresh()?;
        let bound = part_text(p, derived_idx, first + i, val)?;
        let_bindings.push((vname.clone(), bound));
        let mut piece = p.expr_var(vname)?;
        if let Some(labels) = field_labels
            && labels[i] != "0"
        {
            let prefix = p.concat_text(&labels[i], ": ", "")?;
            let prefix_expr = p.expr_text(&prefix)?;
            piece = concat(p, prefix_expr, piece)?;
        }
        pieces.push(piece);
        let sep = if i + 1 == count { closing } else { ", " };
        pieces.push(p.expr_text(sep)?);
    }

    let mut result = pieces[pieces.len() - 1];
    for i in (0..pieces.len() - 1).rev() {
        result = concat(p, pieces[i], result)?;
    }
    for (vname, bound) in let_bindings.into_iter().rev() {
        result = p.expr_let(vname, bound, result)?;
    }
    Ok(result)
}

fn derive_collection_text(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<ExprId> {
    let (ty, proof_id, subst, inst_idx) = {
        let d = &p.derived[derived_idx];
        (d.ty, d.proof, d.subst, d.instance)
    };
    let proof_ty = p.proof(proof_id).ty;
    let is_set = matches!(p.core_ty(ty), Ty::Set(_));
    let is_map = matches!(p.core_ty(ty), Ty::Map { .. });

    let list_type = if is_set || is_map {
        let orig = p.source_type(proof_ty).clone();
        let src_list = if is_map {
            let pair = p.types_overlay.alloc(SourceType {
                kind: SourceTypeKind::Tuple,
                name: "Tuple".to_string(),
                module: None,
                owner: TypeOwner::None,
                arguments: orig.arguments.clone(),
                height: orig.height,
                parameters: orig.parameters,
            });
            p.types_overlay.alloc(SourceType {
                kind: SourceTypeKind::Named,
                name: "List".to_string(),
                module: None,
                owner: TypeOwner::None,
                arguments: vec![pair],
                height: orig.height.saturating_add(1),
                parameters: orig.parameters,
            })
        } else {
            p.types_overlay.alloc(SourceType {
                kind: SourceTypeKind::Named,
                name: "List".to_string(),
                module: None,
                owner: TypeOwner::None,
                arguments: orig.arguments,
                height: orig.height,
                parameters: orig.parameters,
            })
        };
        lower_type(p, src_list, subst)?
    } else {
        ty
    };
    let list_elem_ty = match *p.core_ty(list_type) {
        Ty::List(e) => e,
        _ => return p.refuse(ErrorCode::LowerUnfinished, "expected list type"),
    };

    let text_src = p.types_overlay.builtin("Text");
    let list_text_src = p.types_overlay.builtin_applied("List", vec![text_src]);
    let list_text_ty = lower_type(p, list_text_src, None)?;
    let ret_ty = p.instances[inst_idx].fun.ret;
    let param0_name = p.instances[inst_idx].fun.params[0].name.clone();

    let join_name = p.fresh()?;
    let acc_name = p.fresh()?;
    let join_params = vec![
        p.make_param(param0_name.clone(), list_type)?,
        p.make_param(acc_name.clone(), list_text_ty)?,
    ];

    // Initial jump
    let mut init_list = p.expr_var(param0_name.clone())?;
    if let Ty::Set(elem) = *p.core_ty(ty) {
        init_list = p.expr_prim(
            Prim::Set {
                ty: elem,
                op: CoreSetOp::ToList,
            },
            vec![init_list],
        )?;
    } else if let Ty::Map { key, value } = *p.core_ty(ty) {
        init_list = p.expr_prim(
            Prim::Map {
                key,
                value,
                op: MapOp::Nullary(CoreMapNullaryOp::Entries),
            },
            vec![init_list],
        )?;
    }
    let empty_acc = p.expr_lit_list(ret_ty, Vec::new())?;
    let start_jump = p.alloc_expr(Expr::Jump {
        name: join_name.clone(),
        args: vec![init_list, empty_acc],
    })?;

    // None arm: open + join(reverse(acc), ", ") + close
    let acc_v = p.expr_var(acc_name.clone())?;
    let rev = p.expr_prim(
        Prim::List {
            ty: ret_ty,
            op: ListOp::Nullary(CoreListNullaryOp::Reverse),
        },
        vec![acc_v],
    )?;
    let comma = p.expr_text(", ")?;
    let joined = p.expr_prim(Prim::Text(CoreTextOp::Join), vec![rev, comma])?;
    let open = p.expr_text(if is_map { "{" } else { "[" })?;
    let close = p.expr_text(if is_map {
        "}"
    } else if is_set {
        "].to_set"
    } else {
        "]"
    })?;
    let tail_cat = concat(p, joined, close)?;
    let none_body = concat(p, open, tail_cat)?;

    // Some(pair) arm
    let pair_name = p.fresh()?;
    let conv_name = p.fresh()?;
    let pair_v0 = p.expr_var(pair_name.clone())?;
    let elem_expr = p.expr_proj(pair_v0, 0)?;
    let conv_bound = if is_map {
        let key_name = p.fresh()?;
        let k_proj = p.expr_proj(elem_expr, 0)?;
        let k_bound = part_text(p, derived_idx, 0, k_proj)?;
        let pair_v_for_val = p.expr_var(pair_name.clone())?;
        let elem_expr2 = p.expr_proj(pair_v_for_val, 0)?;
        let v_proj = p.expr_proj(elem_expr2, 1)?;
        let v_text = part_text(p, derived_idx, 1, v_proj)?;
        let colon = p.expr_text(": ")?;
        let rhs = concat(p, colon, v_text)?;
        let kv = p.expr_var(key_name.clone())?;
        let k_body = concat(p, kv, rhs)?;
        p.expr_let(key_name, k_bound, k_body)?
    } else {
        part_text(p, derived_idx, 0, elem_expr)?
    };
    let pair_v1 = p.expr_var(pair_name.clone())?;
    let tail_list = p.expr_proj(pair_v1, 1)?;
    let acc_v2 = p.expr_var(acc_name)?;
    let conv_v = p.expr_var(conv_name.clone())?;
    let prepended = p.expr_prim(
        Prim::List {
            ty: ret_ty,
            op: ListOp::Nullary(CoreListNullaryOp::Prepend),
        },
        vec![acc_v2, conv_v],
    )?;
    let again = p.alloc_expr(Expr::Jump {
        name: join_name.clone(),
        args: vec![tail_list, prepended],
    })?;
    let some_body = p.expr_let(conv_name, conv_bound, again)?;

    let cur_list = p.expr_var(param0_name)?;
    let body = list_case(p, list_elem_ty, cur_list, none_body, pair_name, some_body)?;

    p.alloc_expr(Expr::Join {
        name: join_name,
        params: join_params,
        body,
        rest: start_jump,
    })
}

fn derive_text(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<()> {
    let (span, ty, inst_idx) = {
        let d = &p.derived[derived_idx];
        (d.span, d.ty, d.instance)
    };
    p.span = span;
    let body = match p.core_ty(ty).clone() {
        Ty::List(_) | Ty::Set(_) | Ty::Map { .. } => derive_collection_text(p, derived_idx)?,
        Ty::Tuple(ref parts) => {
            let count = parts.len();
            let p0 = p.instances[inst_idx].fun.params[0].name.clone();
            let mut vals = Vec::with_capacity(count);
            for i in 0..count {
                let v = p.expr_var(p0.clone())?;
                vals.push(p.expr_proj(v, i as u64)?);
            }
            fields_text(p, derived_idx, 0, &vals, None, "(", ")")?
        }
        Ty::FileError => {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "FileError's text form needs the library",
            );
        }
        Ty::Data(_)
        | Ty::Option(_)
        | Ty::Result { .. }
        | Ty::Ordering
        | Ty::Entry
        | Ty::EntryKind
        | Ty::SimulationError
        | Ty::NetworkError
        | Ty::SqliteValue
        | Ty::SqliteRows
        | Ty::SqliteError => {
            let ctors = lower_ctors(p, ty)?;
            let p0 = p.instances[inst_idx].fun.params[0].name.clone();
            let mut arms = Vec::with_capacity(ctors.len());
            let mut first = 0usize;
            for (i, ctor) in ctors.iter().enumerate() {
                p.work(1)?;
                let fcount = ctor.fields.len();
                let mut names = Vec::with_capacity(fcount);
                let mut vals = Vec::with_capacity(fcount);
                for _ in 0..fcount {
                    let vn = p.fresh()?;
                    vals.push(p.expr_var(vn.clone())?);
                    names.push(vn);
                }
                let arm_body = if fcount == 0 {
                    p.expr_text(&ctor.name)?
                } else {
                    let labels: Option<Vec<String>> = match p.core_ty(ty) {
                        Ty::Data(_) => Some(ctor.fields.iter().map(|f| f.name.clone()).collect()),
                        Ty::Entry => Some(vec!["name".to_string(), "kind".to_string()]),
                        Ty::SqliteRows => Some(vec!["columns".to_string(), "rows".to_string()]),
                        Ty::SqliteError => {
                            let label = match i {
                                0 => "path",
                                1 => "reason",
                                _ => "message",
                            };
                            Some(vec![label.to_string()])
                        }
                        Ty::SimulationError => Some(vec!["reason".to_string()]),
                        Ty::NetworkError => {
                            let ls: Vec<String> = match i {
                                0 => vec!["host".to_string()],
                                1 => vec!["host".to_string(), "port".to_string()],
                                4 | 5 => vec!["reason".to_string()],
                                _ => Vec::new(),
                            };
                            Some(ls)
                        }
                        _ => None,
                    };
                    let open = p.concat_text(&ctor.name, "(", "")?;
                    fields_text(p, derived_idx, first, &vals, labels.as_deref(), &open, ")")?
                };
                arms.push(Arm {
                    ctor: ctor.name.clone(),
                    names,
                    body: arm_body,
                });
                first += fcount;
            }
            let p0_v = p.expr_var(p0)?;
            p.alloc_expr(Expr::Case { expr: p0_v, arms })?
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
        | Ty::Simulation
        | Ty::Fn { .. }
        | Ty::Cap(_) => {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "this derived text shape is not lowered yet",
            );
        }
    };
    p.instances[inst_idx].fun.body = body;
    p.instances[inst_idx].body_set = true;
    Ok(())
}

// ---------------------------------------------------------------------------
// Json & Sqlite helpers
// ---------------------------------------------------------------------------

fn find_type_parameter_iter(p: &Lowering<'_>, root: TypeId) -> Option<TypeId> {
    let mut stack = vec![root];
    while let Some(tid) = stack.pop() {
        let st = p.source_type(tid);
        if st.kind == SourceTypeKind::Parameter {
            return Some(tid);
        }
        for &arg in st.arguments.iter().rev() {
            stack.push(arg);
        }
    }
    None
}

fn lib_fn_symbol(
    p: &mut Lowering<'_>,
    mod_name: &str,
    fn_name: &str,
    err_msg: &'static str,
) -> LowerResult<SymRef> {
    let mod_id = find_library_module(p, mod_name, err_msg)?;
    let Some(sidx) =
        p.program.interfaces.scopes[mod_id.index()].find_symbol_index(fn_name.as_bytes())
    else {
        return p.refuse(ErrorCode::LowerUnfinished, err_msg);
    };
    Ok(SymRef::new(mod_id, sidx))
}

fn json_call(p: &mut Lowering<'_>, name: &str, args: Vec<ExprId>) -> LowerResult<ExprId> {
    let sym = lib_fn_symbol(p, "Json", name, "Json ability needs the library")?;
    let inst = lower_instance(p, sym, &[], None, &[], sym.module, 0)?;
    let fname = p.instances[inst].fun.name.clone();
    p.expr_call(fname, args)
}

fn json_generic_call(
    p: &mut Lowering<'_>,
    name: &str,
    arg_type: TypeId,
    subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    args: Vec<ExprId>,
) -> LowerResult<ExprId> {
    let sym = lib_fn_symbol(p, "Json", name, "Json ability needs the library")?;
    let sig_ty = p
        .program
        .signatures
        .functions
        .iter()
        .find(|f| f.function == sym)
        .and_then(|f| f.ty);
    let Some(fn_ty) = sig_ty else {
        return p.refuse(ErrorCode::LowerUnfinished, "Json helper missing signature");
    };
    let Some(param) = find_type_parameter_iter(p, fn_ty) else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "Json generic helper missing type parameter",
        );
    };
    let inst = lower_instance(
        p,
        sym,
        &[TypeArgument {
            parameter: param,
            ty: arg_type,
        }],
        subst,
        &[],
        owner,
        depth + 1,
    )?;
    let fname = p.instances[inst].fun.name.clone();
    p.expr_call(fname, args)
}

fn empty_json_object(p: &mut Lowering<'_>) -> LowerResult<ExprId> {
    let text_src = p.types_overlay.builtin("Text");
    let text_ty = lower_type(p, text_src, None)?;
    let json_src = json_data_source(p, "Json")?;
    let json_ty = lower_type(p, json_src, None)?;
    let map_lit = p.expr_lit_map(text_ty, json_ty, Vec::new())?;
    let ctor = p.copy_text("Object")?;
    p.expr_con(json_ty, ctor, vec![map_lit])
}

fn snake_string(p: &mut Lowering<'_>, raw: &str) -> LowerResult<String> {
    let bytes = raw.as_bytes();
    let mut out = String::with_capacity(bytes.len() * 2 + 2);
    for (i, &c) in bytes.iter().enumerate() {
        let cap_c = c.is_ascii_uppercase();
        let word = i != 0
            && cap_c
            && (bytes[i - 1].is_ascii_lowercase()
                || bytes[i - 1].is_ascii_digit()
                || (i + 1 < bytes.len() && bytes[i + 1].is_ascii_lowercase()));
        if word {
            out.push('_');
        }
        out.push(char::from(if cap_c { c + (b'a' - b'A') } else { c }));
    }
    p.charge_alloc(out.len() + 1, 1, 1)?;
    Ok(out)
}

fn is_json_struct(p: &Lowering<'_>, d: &DerivedEntry) -> bool {
    if matches!(p.core_ty(d.ty), Ty::Data(_)) {
        let proof_ty = p.proof(d.proof).ty;
        return p
            .source_type(proof_ty)
            .owner
            .as_source()
            .is_some_and(|s| p.program.interfaces.symbol(s).decl_kind == SyntaxKind::Struct);
    }
    matches!(p.core_ty(d.ty), Ty::Entry | Ty::SqliteRows)
}

fn ctor_labels(
    p: &mut Lowering<'_>,
    ty: TyId,
    ctor_idx: usize,
    ctor: &CtorDecl,
) -> LowerResult<(Vec<String>, bool)> {
    let count = ctor.fields.len();
    if count == 0 {
        return Ok((Vec::new(), false));
    }
    if matches!(p.core_ty(ty), Ty::Data(_)) {
        if count == 1 && ctor.fields[0].name == "0" {
            return Ok((Vec::new(), true));
        }
        return Ok((ctor.fields.iter().map(|f| f.name.clone()).collect(), false));
    }
    if matches!(p.core_ty(ty), Ty::Result { .. }) {
        return Ok((Vec::new(), true));
    }
    let mut out = Vec::with_capacity(count);
    for j in 0..count {
        let label = match p.core_ty(ty) {
            Ty::Entry => {
                if j == 0 {
                    "name"
                } else {
                    "kind"
                }
            }
            Ty::SqliteRows => {
                if j == 0 {
                    "columns"
                } else {
                    "rows"
                }
            }
            Ty::SqliteError => match ctor_idx {
                0 => "path",
                1 => "reason",
                _ => "message",
            },
            Ty::FileError => {
                if j == 0 {
                    "path"
                } else {
                    "reason"
                }
            }
            Ty::SimulationError => "reason",
            Ty::NetworkError => match (ctor_idx, j) {
                (0, 0) | (1, 0) => "host",
                (1, 1) => "port",
                _ => "reason",
            },
            _ => "value",
        };
        out.push(p.copy_text(label)?);
    }
    Ok((out, false))
}

pub(crate) fn lower_to_json(
    p: &mut Lowering<'_>,
    mut proof: ProofId,
    mut subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    value: ExprId,
) -> LowerResult<ExprId> {
    debug_assert_eq!(p.proof(proof).ability, SourceAbility::Json);
    lower_proof(p, &mut proof, &mut subst)?;
    let imp = p.proof(proof).clone();
    if let Some(enc_fn) = imp.encode_function {
        let inst = lower_customizer_fn(
            p,
            enc_fn,
            &imp.encode_arguments,
            &imp.encode_parts,
            &imp.encode_part_parameters,
            subst,
            owner,
            depth,
        )?;
        let fname = p.instances[inst].fun.name.clone();
        return p.expr_call(fname, vec![value]);
    }
    if imp.kind == AbilityImplementationKind::Derived {
        let d_idx = reserve_kind(p, proof, subst, owner, depth, DerivedKind::ToJson)?;
        let fname = p.instances[p.derived[d_idx].instance].fun.name.clone();
        return p.expr_call(fname, vec![value]);
    }
    if imp.kind != AbilityImplementationKind::Native {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "abstract Json ability is not substituted yet",
        );
    }
    let ty = lower_type(p, imp.ty, subst)?;
    let json_src = json_data_source(p, "Json")?;
    let json_ty = lower_type(p, json_src, None)?;
    match p.core_ty(ty).clone() {
        Ty::Unit => {
            let tmp = p.fresh()?;
            let ctor = p.copy_text("Null")?;
            let null = p.expr_con(json_ty, ctor, Vec::new())?;
            p.expr_let(tmp, value, null)
        }
        Ty::Bool => {
            let ctor = p.copy_text("Boolean")?;
            p.expr_con(json_ty, ctor, vec![value])
        }
        Ty::Int => json_call(p, "int", vec![value]),
        Ty::Byte => json_call(p, "encode_byte", vec![value]),
        Ty::Float => json_call(p, "encode_float", vec![value]),
        Ty::Decimal => json_call(p, "encode_decimal_str", vec![value]),
        Ty::BigInt => json_call(p, "encode_big_int", vec![value]),
        Ty::Text => {
            let ctor = p.copy_text("String")?;
            p.expr_con(json_ty, ctor, vec![value])
        }
        Ty::Char => json_call(p, "encode_char", vec![value]),
        Ty::Time => json_call(p, "encode_time", vec![value]),
        Ty::Data(_) => {
            if p.source_type(imp.ty).name == "Json" {
                Ok(value)
            } else {
                let ctor = p.copy_text("Number")?;
                p.expr_con(json_ty, ctor, vec![value])
            }
        }
        _ => p.refuse(
            ErrorCode::LowerUnfinished,
            "unsupported native Json encode type",
        ),
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "mirrors C lower_from_json signature directly"
)]
pub(crate) fn lower_from_json(
    p: &mut Lowering<'_>,
    mut proof: ProofId,
    mut subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    value: ExprId,
    path: ExprId,
    strict: ExprId,
) -> LowerResult<ExprId> {
    debug_assert_eq!(p.proof(proof).ability, SourceAbility::Json);
    lower_proof(p, &mut proof, &mut subst)?;
    let imp = p.proof(proof).clone();
    if let Some(dec_fn) = imp.decode_function {
        let inst = lower_customizer_fn(
            p,
            dec_fn,
            &imp.decode_arguments,
            &imp.decode_parts,
            &imp.decode_part_parameters,
            subst,
            owner,
            depth,
        )?;
        let fname = p.instances[inst].fun.name.clone();
        let raw = p.expr_call(fname, vec![value])?;
        let wrapped = json_generic_call(
            p,
            "wrap_custom_error",
            imp.ty,
            subst,
            owner,
            depth,
            vec![raw, path],
        )?;
        let tmp = p.fresh()?;
        return p.expr_let(tmp, strict, wrapped);
    }
    if imp.kind == AbilityImplementationKind::Derived {
        let d_idx = reserve_kind(p, proof, subst, owner, depth, DerivedKind::FromJson)?;
        let fname = p.instances[p.derived[d_idx].instance].fun.name.clone();
        return p.expr_call(fname, vec![value, path, strict]);
    }
    if imp.kind != AbilityImplementationKind::Native {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "abstract Json ability is not substituted yet",
        );
    }
    let ty = lower_type(p, imp.ty, subst)?;
    if matches!(p.core_ty(ty), Ty::Data(_)) && p.source_type(imp.ty).name == "Json" {
        let r_src = result_source_type(p, imp.ty)?;
        let ret_ty = lower_type(p, r_src, subst)?;
        let ctor = p.copy_text("Ok")?;
        let ok = p.expr_con(ret_ty, ctor, vec![value])?;
        let t1 = p.fresh()?;
        let t2 = p.fresh()?;
        let inner = p.expr_let(t2, strict, ok)?;
        return p.expr_let(t1, path, inner);
    }
    let helper = match p.core_ty(ty) {
        Ty::Unit => "decode_unit",
        Ty::Bool => "decode_bool",
        Ty::Int => "decode_int",
        Ty::Byte => "decode_byte",
        Ty::Float => "decode_float",
        Ty::Decimal => "decode_decimal",
        Ty::BigInt => "decode_big_int",
        Ty::Text => "decode_text",
        Ty::Char => "decode_char",
        Ty::Time => "decode_time",
        _ => "decode_json_number",
    };
    let call = json_call(p, helper, vec![value, path])?;
    let tmp = p.fresh()?;
    p.expr_let(tmp, strict, call)
}

#[expect(
    clippy::too_many_arguments,
    reason = "mirrors C lower_json_decode_call signature directly"
)]
pub(crate) fn lower_json_decode_call(
    p: &mut Lowering<'_>,
    mut proof: ProofId,
    mut subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    input_ty: TyId,
    input: ExprId,
    strict: bool,
) -> LowerResult<ExprId> {
    debug_assert_eq!(p.proof(proof).ability, SourceAbility::Json);
    lower_proof(p, &mut proof, &mut subst)?;
    let proof_ty = p.proof(proof).ty;
    let r_src = result_source_type(p, proof_ty)?;
    let ret_ty = lower_type(p, r_src, subst)?;
    let parse_fn = if matches!(p.core_ty(input_ty), Ty::Bytes) {
        "parse_bytes_input"
    } else {
        "parse_text_input"
    };
    let parsed = json_call(p, parse_fn, vec![input])?;
    let ok_name = p.fresh()?;
    let err_name = p.fresh()?;
    let ok_v = p.expr_var(ok_name.clone())?;
    let empty_path = p.expr_text("")?;
    let strict_b = p.expr_bool(strict)?;
    let ok_body = lower_from_json(p, proof, subst, owner, depth, ok_v, empty_path, strict_b)?;
    let err_v = p.expr_var(err_name.clone())?;
    let err_ctor = p.copy_text("Error")?;
    let err_body = p.expr_con(ret_ty, err_ctor.clone(), vec![err_v])?;
    let ok_ctor = p.copy_text("Ok")?;
    p.alloc_expr(Expr::Case {
        expr: parsed,
        arms: vec![
            Arm {
                ctor: ok_ctor,
                names: vec![ok_name],
                body: ok_body,
            },
            Arm {
                ctor: err_ctor,
                names: vec![err_name],
                body: err_body,
            },
        ],
    })
}

pub(crate) fn lower_json_schema(
    p: &mut Lowering<'_>,
    mut proof: ProofId,
    mut subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    seen: ExprId,
) -> LowerResult<ExprId> {
    debug_assert_eq!(p.proof(proof).ability, SourceAbility::Json);
    lower_proof(p, &mut proof, &mut subst)?;
    let imp = p.proof(proof).clone();
    if imp.encode_function.is_some() || imp.decode_function.is_some() {
        let tmp = p.fresh()?;
        let empty = empty_json_object(p)?;
        return p.expr_let(tmp, seen, empty);
    }
    if imp.kind == AbilityImplementationKind::Derived {
        let d_idx = reserve_kind(p, proof, subst, owner, depth, DerivedKind::JsonSchema)?;
        let fname = p.instances[p.derived[d_idx].instance].fun.name.clone();
        return p.expr_call(fname, vec![seen]);
    }
    if imp.kind != AbilityImplementationKind::Native {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "abstract Json ability is not substituted yet",
        );
    }
    let ty = lower_type(p, imp.ty, subst)?;
    let res = match p.core_ty(ty) {
        Ty::Unit => {
            let s = p.expr_text("null")?;
            json_call(p, "schema_type", vec![s])?
        }
        Ty::Bool => {
            let s = p.expr_text("boolean")?;
            json_call(p, "schema_type", vec![s])?
        }
        Ty::Int => {
            let s = p.expr_text("integer")?;
            json_call(p, "schema_type", vec![s])?
        }
        Ty::Byte => json_call(p, "schema_byte", Vec::new())?,
        Ty::Float => {
            let s = p.expr_text("number")?;
            json_call(p, "schema_type", vec![s])?
        }
        Ty::Decimal => {
            let s1 = p.expr_text("string")?;
            let s2 = p.expr_text("decimal")?;
            json_call(p, "schema_format", vec![s1, s2])?
        }
        Ty::BigInt => {
            let s1 = p.expr_text("string")?;
            let s2 = p.expr_text("bigint")?;
            json_call(p, "schema_format", vec![s1, s2])?
        }
        Ty::Text => {
            let s = p.expr_text("string")?;
            json_call(p, "schema_type", vec![s])?
        }
        Ty::Char => json_call(p, "schema_char", Vec::new())?,
        Ty::Time => {
            let s1 = p.expr_text("string")?;
            let s2 = p.expr_text("date-time")?;
            json_call(p, "schema_format", vec![s1, s2])?
        }
        _ if p.source_type(imp.ty).name == "Json" => empty_json_object(p)?,
        _ => {
            let s = p.expr_text("number")?;
            json_call(p, "schema_type", vec![s])?
        }
    };
    let tmp = p.fresh()?;
    p.expr_let(tmp, seen, res)
}

// ---------------------------------------------------------------------------
// Sqlite.decode / decode_one / decode_optional
// ---------------------------------------------------------------------------

fn sqlite_call(p: &mut Lowering<'_>, name: &str, args: Vec<ExprId>) -> LowerResult<ExprId> {
    let sym = lib_fn_symbol(p, "Sqlite", name, "Sqlite decode needs the library")?;
    let inst = lower_instance(p, sym, &[], None, &[], sym.module, 0)?;
    let fname = p.instances[inst].fun.name.clone();
    p.expr_call(fname, args)
}

fn sqlite_generic_call(
    p: &mut Lowering<'_>,
    name: &str,
    arg_type: TypeId,
    subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    args: Vec<ExprId>,
) -> LowerResult<ExprId> {
    let sym = lib_fn_symbol(p, "Sqlite", name, "Sqlite decode needs the library")?;
    let sig_ty = p
        .program
        .signatures
        .functions
        .iter()
        .find(|f| f.function == sym)
        .and_then(|f| f.ty);
    let Some(fn_ty) = sig_ty else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "Sqlite helper missing signature",
        );
    };
    let Some(param) = find_type_parameter_iter(p, fn_ty) else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "Sqlite generic helper missing type parameter",
        );
    };
    let inst = lower_instance(
        p,
        sym,
        &[TypeArgument {
            parameter: param,
            ty: arg_type,
        }],
        subst,
        &[],
        owner,
        depth + 1,
    )?;
    let fname = p.instances[inst].fun.name.clone();
    p.expr_call(fname, args)
}

fn subst_struct_field_source(
    p: &mut Lowering<'_>,
    t: TypeId,
    generic: TypeId,
    actual: TypeId,
) -> LowerResult<TypeId> {
    let g_args = p.source_type(generic).arguments.clone();
    if g_args.is_empty() || p.source_type(t).parameters == 0 {
        return Ok(t);
    }
    let a_args = p.source_type(actual).arguments.clone();
    // Explicit stack substitution (no recursion).
    struct Frame {
        orig: TypeId,
        args: Vec<TypeId>,
        at: usize,
    }
    let mut stack = vec![Frame {
        orig: t,
        args: Vec::new(),
        at: 0,
    }];
    let mut last: Option<TypeId> = None;
    while let Some(frame) = stack.last_mut() {
        p.work(1)?;
        if let Some(res) = last.take() {
            frame.args.push(res);
        }
        let orig = frame.orig;
        let st = p.source_type(orig).clone();
        if st.parameters == 0 {
            stack.pop();
            last = Some(orig);
            continue;
        }
        if st.kind == SourceTypeKind::Parameter {
            let mut mapped = orig;
            for (i, &gp) in g_args.iter().enumerate() {
                if p.source_type(gp).name.as_bytes().first() == st.name.as_bytes().first() {
                    mapped = a_args[i];
                    break;
                }
            }
            stack.pop();
            last = Some(mapped);
            continue;
        }
        if frame.at < st.arguments.len() {
            let child = st.arguments[frame.at];
            frame.at += 1;
            stack.push(Frame {
                orig: child,
                args: Vec::new(),
                at: 0,
            });
            continue;
        }
        let new_args = std::mem::take(&mut frame.args);
        let mut mask = 0u32;
        let mut height = 0u32;
        for &na in &new_args {
            let child = p.source_type(na);
            mask |= child.parameters;
            height = height.max(child.height.saturating_add(1));
        }
        let made = p.types_overlay.alloc(SourceType {
            kind: st.kind,
            name: st.name,
            module: st.module,
            owner: st.owner,
            arguments: new_args,
            height,
            parameters: mask,
        });
        stack.pop();
        last = Some(made);
    }
    Ok(last.unwrap_or(t))
}

fn sqlite_result_ty(
    p: &mut Lowering<'_>,
    ok_src: TypeId,
    err_src: TypeId,
    subst: Option<SubstId>,
) -> LowerResult<TyId> {
    let ok_params = p.source_type(ok_src).parameters;
    let res_src = p.types_overlay.alloc(SourceType {
        kind: SourceTypeKind::Named,
        name: "Result".to_string(),
        module: None,
        owner: TypeOwner::None,
        arguments: vec![ok_src, err_src],
        height: p.source_type(ok_src).height.saturating_add(1),
        parameters: ok_params,
    });
    lower_type(p, res_src, subst)
}

#[expect(
    clippy::too_many_arguments,
    reason = "mirrors C lower_sqlite_decode_call signature directly"
)]
pub(crate) fn lower_sqlite_decode_call(
    p: &mut Lowering<'_>,
    target_src: TypeId,
    abilities: &[CheckedAbilityUse],
    subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    caller_name: &str,
    fn_name: &str,
    rows_expr: ExprId,
) -> LowerResult<ExprId> {
    let text_src = p.types_overlay.builtin("Text");
    let int_src = p.types_overlay.builtin("Int");
    let sval_src = p.types_overlay.builtin("Sqlite.Value");
    let int_list_src = p.types_overlay.builtin_applied("List", vec![int_src]);
    let sval_list_src = p.types_overlay.builtin_applied("List", vec![sval_src]);

    let err_src = sqlite_data_source(p, "SqliteDecodeError")?;
    let text_ty = lower_type(p, text_src, None)?;
    let int_list_ty = lower_type(p, int_list_src, None)?;
    let sval_list_ty = lower_type(p, sval_list_src, None)?;
    let target_ty = lower_type(p, target_src, subst)?;
    let row_res_ty = sqlite_result_ty(p, target_src, err_src, subst)?;

    let Some(target_sym) = p.source_type(target_src).owner.as_source() else {
        return p.refuse(
            ErrorCode::LowerUnfinished,
            "Sqlite.decode target missing struct owner",
        );
    };
    let (data_ty, field_orig_tys) = {
        let Some((_, data)) = p.program.signatures.data_signature_by_symbol(target_sym) else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "Sqlite.decode target missing data signature",
            );
        };
        let mut tys = Vec::with_capacity(data.fields.len());
        for j in 0..data.fields.len() {
            tys.push(data.field_by_index(j).ty);
        }
        (data.ty, tys)
    };

    let ctors = lower_ctors(p, target_ty)?;
    let ctor = &ctors[0];
    let fcount = ctor.fields.len();

    let mut col_items = Vec::with_capacity(fcount);
    for f in &ctor.fields {
        col_items.push(p.expr_text(&f.name)?);
    }
    let required_list = p.expr_lit_list(text_ty, col_items)?;

    let indices_var = p.fresh()?;
    let row_var = p.fresh()?;
    let mut field_vars = Vec::with_capacity(fcount);
    for _ in 0..fcount {
        field_vars.push(p.fresh()?);
    }

    // Build nested decode chain from inside out (or record slots). Building inside-out is clean!
    let mut con_args = Vec::with_capacity(fcount);
    for fv in &field_vars {
        con_args.push(p.expr_var(fv.clone())?);
    }
    let built_struct = p.expr_con(target_ty, ctor.name.clone(), con_args)?;
    let ok_ctor = p.copy_text("Ok")?;
    let err_ctor = p.copy_text("Error")?;
    let mut body = p.expr_con(row_res_ty, ok_ctor.clone(), vec![built_struct])?;

    for j in (0..fcount).rev() {
        p.work(1)?;
        let field_src = subst_struct_field_source(p, field_orig_tys[j], data_ty, target_src)?;
        let field_ty = ctor.fields[j].ty;
        let is_opt = matches!(p.core_ty(field_ty), Ty::Option(_));
        let u_src = if is_opt {
            p.source_type(field_src).arguments[0]
        } else {
            field_src
        };
        let u_ty = match *p.core_ty(field_ty) {
            Ty::Option(inner) => inner,
            _ => field_ty,
        };
        let u_res_ty = sqlite_result_ty(p, u_src, err_src, subst)?;
        let field_res_ty = if is_opt {
            sqlite_result_ty(p, field_src, err_src, subst)?
        } else {
            u_res_ty
        };

        let json_proof = abilities
            .iter()
            .find(|ab| {
                ab.operand.is_some_and(|op| op.0 as usize == j)
                    && p.proof(ab.implementation).ability == SourceAbility::Json
            })
            .map(|ab| ab.implementation);

        let col_name = ctor.fields[j].name.clone();
        let cell_var = p.fresh()?;
        let row_v = p.expr_var(row_var.clone())?;
        let idx_v = p.expr_var(indices_var.clone())?;
        let j_lit = p.expr_int(j as u64)?;
        let cell_bound = sqlite_call(p, "row_cell", vec![row_v, idx_v, j_lit])?;

        let decode_u = if let Some(jp) = json_proof {
            let cv = p.expr_var(cell_var.clone())?;
            let cn = p.expr_text(&col_name)?;
            let text_res = sqlite_call(p, "decode_cell_text", vec![cv, cn])?;
            let raw_json_var = p.fresh()?;
            let terr_var = p.fresh()?;
            let rjv = p.expr_var(raw_json_var.clone())?;
            let json_decoded =
                lower_json_decode_call(p, jp, subst, owner, depth, text_ty, rjv, false)?;
            let cn2 = p.expr_text(&col_name)?;
            let ok_body = sqlite_generic_call(
                p,
                "map_json_decode_result",
                u_src,
                subst,
                owner,
                depth,
                vec![json_decoded, cn2],
            )?;
            let tev = p.expr_var(terr_var.clone())?;
            let err_body = p.expr_con(u_res_ty, err_ctor.clone(), vec![tev])?;
            p.alloc_expr(Expr::Case {
                expr: text_res,
                arms: vec![
                    Arm {
                        ctor: ok_ctor.clone(),
                        names: vec![raw_json_var],
                        body: ok_body,
                    },
                    Arm {
                        ctor: err_ctor.clone(),
                        names: vec![terr_var],
                        body: err_body,
                    },
                ],
            })?
        } else {
            let helper = match p.core_ty(u_ty) {
                Ty::Int => "decode_cell_int",
                Ty::Bool => "decode_cell_bool",
                Ty::Float => "decode_cell_float",
                Ty::Text => "decode_cell_text",
                Ty::Bytes => "decode_cell_bytes",
                Ty::Decimal => "decode_cell_decimal",
                Ty::Time => "decode_cell_time",
                _ => "decode_cell_date",
            };
            let cv = p.expr_var(cell_var.clone())?;
            let cn = p.expr_text(&col_name)?;
            sqlite_call(p, helper, vec![cv, cn])?
        };

        let field_res_expr = if is_opt {
            let uv = p.fresh()?;
            let ue = p.fresh()?;
            let uv_e = p.expr_var(uv.clone())?;
            let some_ctor = p.copy_text("Some")?;
            let none_ctor = p.copy_text("None")?;
            let some_e = p.expr_con(field_ty, some_ctor, vec![uv_e])?;
            let ok_some = p.expr_con(field_res_ty, ok_ctor.clone(), vec![some_e])?;
            let ue_e = p.expr_var(ue.clone())?;
            let err_e = p.expr_con(field_res_ty, err_ctor.clone(), vec![ue_e])?;
            let ocase = p.alloc_expr(Expr::Case {
                expr: decode_u,
                arms: vec![
                    Arm {
                        ctor: ok_ctor.clone(),
                        names: vec![uv],
                        body: ok_some,
                    },
                    Arm {
                        ctor: err_ctor.clone(),
                        names: vec![ue],
                        body: err_e,
                    },
                ],
            })?;
            let cv = p.expr_var(cell_var.clone())?;
            let is_null = sqlite_call(p, "cell_is_null?", vec![cv])?;
            let none_e = p.expr_con(field_ty, none_ctor, Vec::new())?;
            let ok_none = p.expr_con(field_res_ty, ok_ctor.clone(), vec![none_e])?;
            p.expr_if(is_null, ok_none, ocase)?
        } else {
            decode_u
        };

        let fe = p.fresh()?;
        let fe_v = p.expr_var(fe.clone())?;
        let err_arm_body = p.expr_con(row_res_ty, err_ctor.clone(), vec![fe_v])?;
        let fcase = p.alloc_expr(Expr::Case {
            expr: field_res_expr,
            arms: vec![
                Arm {
                    ctor: ok_ctor.clone(),
                    names: vec![field_vars[j].clone()],
                    body,
                },
                Arm {
                    ctor: err_ctor.clone(),
                    names: vec![fe],
                    body: err_arm_body,
                },
            ],
        })?;
        body = p.expr_let(cell_var, cell_bound, fcase)?;
    }

    let p0 = p.make_param(indices_var, int_list_ty)?;
    let p1 = p.make_param(row_var, sval_list_ty)?;
    let lam = p.alloc_expr(Expr::Lam {
        params: vec![p0, p1],
        body,
    })?;
    let serial = p.serial;
    p.serial += 1;
    let num = p.number_text(serial)?;
    let closure_name = p.concat_text(caller_name, ".fn.", &num)?;
    p.set_closure(lam, closure_name);

    let driver = match fn_name {
        "decode_one" => "decode_one_driver",
        "decode_optional" => "decode_optional_driver",
        _ => "decode_rows_driver",
    };
    sqlite_generic_call(
        p,
        driver,
        target_src,
        subst,
        owner,
        depth,
        vec![rows_expr, required_list, lam],
    )
}

// ---------------------------------------------------------------------------
// Derived to_json / from_json / json_schema bodies
// ---------------------------------------------------------------------------

fn part_to_json(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    part: usize,
    value: ExprId,
) -> LowerResult<ExprId> {
    let (pr, subst, owner, depth) = {
        let d = &p.derived[derived_idx];
        (p.proof(d.proof).parts[part], d.subst, d.owner, d.depth)
    };
    lower_to_json(p, pr, subst, owner, depth, value)
}

fn part_from_json(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    part: usize,
    value: ExprId,
    path: ExprId,
    strict: ExprId,
) -> LowerResult<ExprId> {
    let (pr, subst, owner, depth) = {
        let d = &p.derived[derived_idx];
        (p.proof(d.proof).parts[part], d.subst, d.owner, d.depth)
    };
    lower_from_json(p, pr, subst, owner, depth, value, path, strict)
}

fn part_json_schema(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    part: usize,
    seen: ExprId,
) -> LowerResult<ExprId> {
    let (pr, subst, owner, depth) = {
        let d = &p.derived[derived_idx];
        (p.proof(d.proof).parts[part], d.subst, d.owner, d.depth)
    };
    lower_json_schema(p, pr, subst, owner, depth, seen)
}

fn encode_lambda(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    part: usize,
    item_ty: TyId,
) -> LowerResult<ExprId> {
    let param = p.fresh()?;
    let pv = p.expr_var(param.clone())?;
    let body = part_to_json(p, derived_idx, part, pv)?;
    let p0 = p.make_param(param, item_ty)?;
    let lam = p.alloc_expr(Expr::Lam {
        params: vec![p0],
        body,
    })?;
    let serial = p.serial;
    p.serial += 1;
    let num = p.number_text(serial)?;
    let meta_name = p.instances[p.derived[derived_idx].instance]
        .meta
        .name
        .clone();
    let cname = p.concat_text(&meta_name, ".fn.", &num)?;
    p.set_closure(lam, cname);
    Ok(lam)
}

fn decode_lambda(p: &mut Lowering<'_>, derived_idx: usize, part: usize) -> LowerResult<ExprId> {
    let text_src = p.types_overlay.builtin("Text");
    let bool_src = p.types_overlay.builtin("Bool");
    let json_src = json_data_source(p, "Json")?;
    let json_ty = lower_type(p, json_src, None)?;
    let text_ty = lower_type(p, text_src, None)?;
    let bool_ty = lower_type(p, bool_src, None)?;

    let v_name = p.fresh()?;
    let p_name = p.fresh()?;
    let s_name = p.fresh()?;
    let vv = p.expr_var(v_name.clone())?;
    let pv = p.expr_var(p_name.clone())?;
    let sv = p.expr_var(s_name.clone())?;
    let body = part_from_json(p, derived_idx, part, vv, pv, sv)?;
    let p0 = p.make_param(v_name, json_ty)?;
    let p1 = p.make_param(p_name, text_ty)?;
    let p2 = p.make_param(s_name, bool_ty)?;
    let lam = p.alloc_expr(Expr::Lam {
        params: vec![p0, p1, p2],
        body,
    })?;
    let serial = p.serial;
    p.serial += 1;
    let num = p.number_text(serial)?;
    let meta_name = p.instances[p.derived[derived_idx].instance]
        .meta
        .name
        .clone();
    let cname = p.concat_text(&meta_name, ".fn.", &num)?;
    p.set_closure(lam, cname);
    Ok(lam)
}

fn json_object_from_pairs(
    p: &mut Lowering<'_>,
    json_ty: TyId,
    keys: &[String],
    vals: &[ExprId],
) -> LowerResult<ExprId> {
    let text_src = p.types_overlay.builtin("Text");
    let text_ty = lower_type(p, text_src, None)?;
    let mut flat = Vec::with_capacity(keys.len() * 2);
    for (k, &v) in keys.iter().zip(vals.iter()) {
        flat.push(p.expr_text(k)?);
        flat.push(v);
    }
    let map_lit = p.expr_lit_map(text_ty, json_ty, flat)?;
    let ctor = p.copy_text("Object")?;
    p.expr_con(json_ty, ctor, vec![map_lit])
}

fn derive_to_json(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<()> {
    let (span, ty, proof_id, subst, owner, depth, inst_idx) = {
        let d = &p.derived[derived_idx];
        (d.span, d.ty, d.proof, d.subst, d.owner, d.depth, d.instance)
    };
    p.span = span;
    let json_ty = p.instances[inst_idx].fun.ret;
    let p0 = p.instances[inst_idx].fun.params[0].name.clone();
    let input = p.expr_var(p0)?;
    let proof_ty_args = p.source_type(p.proof(proof_id).ty).arguments.clone();

    let body = match p.core_ty(ty).clone() {
        Ty::List(elem) => {
            let lam = encode_lambda(p, derived_idx, 0, elem)?;
            json_generic_call(
                p,
                "encode_list",
                proof_ty_args[0],
                subst,
                owner,
                depth,
                vec![input, lam],
            )?
        }
        Ty::Set(elem) => {
            let as_list = p.expr_prim(
                Prim::Set {
                    ty: elem,
                    op: CoreSetOp::ToList,
                },
                vec![input],
            )?;
            let lam = encode_lambda(p, derived_idx, 0, elem)?;
            json_generic_call(
                p,
                "encode_list",
                proof_ty_args[0],
                subst,
                owner,
                depth,
                vec![as_list, lam],
            )?
        }
        Ty::Map { value, .. } => {
            let lam = encode_lambda(p, derived_idx, 1, value)?;
            json_generic_call(
                p,
                "encode_map",
                proof_ty_args[1],
                subst,
                owner,
                depth,
                vec![input, lam],
            )?
        }
        Ty::Option(inner) => {
            let lam = encode_lambda(p, derived_idx, 0, inner)?;
            json_generic_call(
                p,
                "encode_option",
                proof_ty_args[0],
                subst,
                owner,
                depth,
                vec![input, lam],
            )?
        }
        Ty::Tuple(ref parts) => {
            let mut items = Vec::with_capacity(parts.len());
            for i in 0..parts.len() {
                let p0_again = p.expr_var(p.instances[inst_idx].fun.params[0].name.clone())?;
                let proj = p.expr_proj(p0_again, i as u64)?;
                items.push(part_to_json(p, derived_idx, i, proj)?);
            }
            let list_lit = p.expr_lit_list(json_ty, items)?;
            let ctor = p.copy_text("Array")?;
            p.expr_con(json_ty, ctor, vec![list_lit])?
        }
        _ => {
            let ctors = lower_ctors(p, ty)?;
            let record = is_json_struct(p, &p.derived[derived_idx]);
            let mut arms = Vec::with_capacity(ctors.len());
            let mut first = 0usize;
            for (i, ctor) in ctors.iter().enumerate() {
                p.work(1)?;
                let fcount = ctor.fields.len();
                let mut names = Vec::with_capacity(fcount);
                let mut encoded = Vec::with_capacity(fcount);
                for j in 0..fcount {
                    let vn = p.fresh()?;
                    let ve = p.expr_var(vn.clone())?;
                    encoded.push(part_to_json(p, derived_idx, first + j, ve)?);
                    names.push(vn);
                }
                let (labels, unnamed) = ctor_labels(p, ty, i, ctor)?;
                let arm_body = if record {
                    json_object_from_pairs(p, json_ty, &labels, &encoded)?
                } else if fcount == 0 {
                    let tag = snake_string(p, &ctor.name)?;
                    let t_e = p.expr_text(&tag)?;
                    let sc = p.copy_text("String")?;
                    p.expr_con(json_ty, sc, vec![t_e])?
                } else {
                    let tag = snake_string(p, &ctor.name)?;
                    let payload = if unnamed {
                        encoded[0]
                    } else {
                        json_object_from_pairs(p, json_ty, &labels, &encoded)?
                    };
                    json_object_from_pairs(p, json_ty, &[tag], &[payload])?
                };
                arms.push(Arm {
                    ctor: ctor.name.clone(),
                    names,
                    body: arm_body,
                });
                first += fcount;
            }
            p.alloc_expr(Expr::Case { expr: input, arms })?
        }
    };
    p.instances[inst_idx].fun.body = body;
    p.instances[inst_idx].body_set = true;
    Ok(())
}

#[expect(
    clippy::too_many_arguments,
    reason = "mirrors C decode_fields_object signature directly"
)]
fn decode_fields_object(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    first: usize,
    ctor: &CtorDecl,
    labels: &[String],
    obj_expr: ExprId,
    path_var: &str,
    strict_var: &str,
) -> LowerResult<ExprId> {
    let text_src = p.types_overlay.builtin("Text");
    let int_src = p.types_overlay.builtin("Int");
    let text_ty = lower_type(p, text_src, None)?;
    let int_ty = lower_type(p, int_src, None)?;
    let json_src = json_data_source(p, "Json")?;
    let json_ty = lower_type(p, json_src, None)?;
    let (ty, ret_ty) = {
        let d = &p.derived[derived_idx];
        (d.ty, p.instances[d.instance].fun.ret)
    };
    let count = ctor.fields.len();
    let ok_ctor = p.copy_text("Ok")?;
    let err_ctor = p.copy_text("Error")?;

    let map_var = p.fresh()?;
    let err_var = p.fresh()?;
    let init_present_var = p.fresh()?;

    let mut present_vars = Vec::with_capacity(count + 1);
    present_vars.push(init_present_var.clone());
    let mut field_vars = Vec::with_capacity(count);
    for _ in 0..count {
        field_vars.push(p.fresh()?);
        present_vars.push(p.fresh()?);
    }

    // Innermost: strict check gate
    let mut built_fields = Vec::with_capacity(count);
    let mut known_items = Vec::with_capacity(count);
    for j in 0..count {
        built_fields.push(p.expr_var(field_vars[j].clone())?);
        known_items.push(p.expr_text(&labels[j])?);
    }
    let known_list = p.expr_lit_list(text_ty, known_items)?;
    let built1 = p.expr_con(ty, ctor.name.clone(), built_fields)?;
    let ok_val = p.expr_con(ret_ty, ok_ctor.clone(), vec![built1])?;

    let mut built_fields2 = Vec::with_capacity(count);
    for fv in &field_vars {
        built_fields2.push(p.expr_var(fv.clone())?);
    }
    let built2 = p.expr_con(ty, ctor.name.clone(), built_fields2)?;
    let ok_else = p.expr_con(ret_ty, ok_ctor.clone(), vec![built2])?;

    let mv_s = p.expr_var(map_var.clone())?;
    let pv_s = p.expr_var(present_vars[count].clone())?;
    let path_s = p.expr_var(path_var.to_string())?;
    let strict_call = json_call(p, "check_strict_keys", vec![mv_s, pv_s, known_list, path_s])?;
    let strict_ok = p.fresh()?;
    let strict_err = p.fresh()?;
    let se_v = p.expr_var(strict_err.clone())?;
    let se_body = p.expr_con(ret_ty, err_ctor.clone(), vec![se_v])?;
    let strict_case = p.alloc_expr(Expr::Case {
        expr: strict_call,
        arms: vec![
            Arm {
                ctor: ok_ctor.clone(),
                names: vec![strict_ok],
                body: ok_val,
            },
            Arm {
                ctor: err_ctor.clone(),
                names: vec![strict_err],
                body: se_body,
            },
        ],
    })?;
    let sv_cond = p.expr_var(strict_var.to_string())?;
    let mut inner = p.expr_if(sv_cond, strict_case, ok_else)?;

    // Wrap each field join from count-1 down to 0
    for j in (0..count).rev() {
        p.work(1)?;
        let field_ty = ctor.fields[j].ty;
        let next_val = field_vars[j].clone();
        let next_present = present_vars[j + 1].clone();
        let cur_present = present_vars[j].clone();
        let join_label = p.fresh()?;

        let mv = p.expr_var(map_var.clone())?;
        let lbl = p.expr_text(&labels[j])?;
        let get_expr = p.expr_prim(
            Prim::Map {
                key: text_ty,
                value: json_ty,
                op: MapOp::Nullary(CoreMapNullaryOp::Get),
            },
            vec![mv, lbl],
        )?;

        let none_body = if matches!(p.core_ty(field_ty), Ty::Option(_)) {
            let none_ctor = p.copy_text("None")?;
            let none_con = p.expr_con(field_ty, none_ctor, Vec::new())?;
            let cp = p.expr_var(cur_present.clone())?;
            p.alloc_expr(Expr::Jump {
                name: join_label.clone(),
                args: vec![none_con, cp],
            })?
        } else {
            let pv = p.expr_var(path_var.to_string())?;
            let lbl2 = p.expr_text(&labels[j])?;
            let miss = json_call(p, "missing_field_error", vec![pv, lbl2])?;
            p.expr_con(ret_ty, err_ctor.clone(), vec![miss])?
        };

        let raw_fv = p.fresh()?;
        let pv2 = p.expr_var(path_var.to_string())?;
        let lbl3 = p.expr_text(&labels[j])?;
        let f_path = json_call(p, "join_path", vec![pv2, lbl3])?;
        let rfv_e = p.expr_var(raw_fv.clone())?;
        let sv_e = p.expr_var(strict_var.to_string())?;
        let dec_expr = part_from_json(p, derived_idx, first + j, rfv_e, f_path, sv_e)?;

        let dec_ok = p.fresh()?;
        let dec_err = p.fresh()?;
        let dok_v = p.expr_var(dec_ok.clone())?;
        let cp2 = p.expr_var(cur_present)?;
        let one = p.expr_int(1)?;
        let inc = p.expr_prim(Prim::Int(CoreIntOp::Add), vec![cp2, one])?;
        let jmp_ok = p.alloc_expr(Expr::Jump {
            name: join_label.clone(),
            args: vec![dok_v, inc],
        })?;
        let derr_v = p.expr_var(dec_err.clone())?;
        let dec_err_body = p.expr_con(ret_ty, err_ctor.clone(), vec![derr_v])?;
        let dec_case = p.alloc_expr(Expr::Case {
            expr: dec_expr,
            arms: vec![
                Arm {
                    ctor: ok_ctor.clone(),
                    names: vec![dec_ok],
                    body: jmp_ok,
                },
                Arm {
                    ctor: err_ctor.clone(),
                    names: vec![dec_err],
                    body: dec_err_body,
                },
            ],
        })?;

        let none_ctor = p.copy_text("None")?;
        let some_ctor = p.copy_text("Some")?;
        let get_case = p.alloc_expr(Expr::Case {
            expr: get_expr,
            arms: vec![
                Arm {
                    ctor: none_ctor,
                    names: Vec::new(),
                    body: none_body,
                },
                Arm {
                    ctor: some_ctor,
                    names: vec![raw_fv],
                    body: dec_case,
                },
            ],
        })?;

        let jp0 = p.make_param(next_val, field_ty)?;
        let jp1 = p.make_param(next_present, int_ty)?;
        inner = p.alloc_expr(Expr::Join {
            name: join_label,
            params: vec![jp0, jp1],
            body: inner,
            rest: get_case,
        })?;
    }

    let zero = p.expr_int(0)?;
    let ok_body = p.expr_let(init_present_var, zero, inner)?;
    let pv_top = p.expr_var(path_var.to_string())?;
    let expect_obj = json_call(p, "expect_object", vec![obj_expr, pv_top])?;
    let ev_top = p.expr_var(err_var.clone())?;
    let err_body = p.expr_con(ret_ty, err_ctor.clone(), vec![ev_top])?;

    p.alloc_expr(Expr::Case {
        expr: expect_obj,
        arms: vec![
            Arm {
                ctor: ok_ctor,
                names: vec![map_var],
                body: ok_body,
            },
            Arm {
                ctor: err_ctor,
                names: vec![err_var],
                body: err_body,
            },
        ],
    })
}

fn bad_enum_case(
    p: &mut Lowering<'_>,
    ret_ty: TyId,
    val_var: &str,
    path_var: &str,
    type_label: &str,
) -> LowerResult<ExprId> {
    let vv = p.expr_var(val_var.to_string())?;
    let pv = p.expr_var(path_var.to_string())?;
    let tl = p.expr_text(type_label)?;
    let err_val = json_call(p, "unknown_case_error", vec![vv, pv, tl])?;
    let ctor = p.copy_text("Error")?;
    p.expr_con(ret_ty, ctor, vec![err_val])
}

fn derive_from_json(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<()> {
    let (span, ty, proof_id, subst, owner, depth, inst_idx) = {
        let d = &p.derived[derived_idx];
        (d.span, d.ty, d.proof, d.subst, d.owner, d.depth, d.instance)
    };
    p.span = span;
    let ret_ty = p.instances[inst_idx].fun.ret;
    let val_var = p.instances[inst_idx].fun.params[0].name.clone();
    let path_var = p.instances[inst_idx].fun.params[1].name.clone();
    let strict_var = p.instances[inst_idx].fun.params[2].name.clone();
    let proof_ty_id = p.proof(proof_id).ty;
    let proof_ty_args = p.source_type(proof_ty_id).arguments.clone();
    let ok_ctor = p.copy_text("Ok")?;
    let err_ctor = p.copy_text("Error")?;

    let body = match p.core_ty(ty).clone() {
        Ty::List(_) => {
            let vv = p.expr_var(val_var)?;
            let pv = p.expr_var(path_var)?;
            let sv = p.expr_var(strict_var)?;
            let lam = decode_lambda(p, derived_idx, 0)?;
            json_generic_call(
                p,
                "decode_list",
                proof_ty_args[0],
                subst,
                owner,
                depth,
                vec![vv, pv, sv, lam],
            )?
        }
        Ty::Set(elem) => {
            let vv = p.expr_var(val_var)?;
            let pv = p.expr_var(path_var)?;
            let sv = p.expr_var(strict_var)?;
            let lam = decode_lambda(p, derived_idx, 0)?;
            let list_res = json_generic_call(
                p,
                "decode_list",
                proof_ty_args[0],
                subst,
                owner,
                depth,
                vec![vv, pv, sv, lam],
            )?;
            let lst_var = p.fresh()?;
            let err_var = p.fresh()?;
            let lv = p.expr_var(lst_var.clone())?;
            let set_val = p.expr_prim(
                Prim::Set {
                    ty: elem,
                    op: CoreSetOp::OfList,
                },
                vec![lv],
            )?;
            let ok_body = p.expr_con(ret_ty, ok_ctor.clone(), vec![set_val])?;
            let ev = p.expr_var(err_var.clone())?;
            let err_body = p.expr_con(ret_ty, err_ctor.clone(), vec![ev])?;
            p.alloc_expr(Expr::Case {
                expr: list_res,
                arms: vec![
                    Arm {
                        ctor: ok_ctor,
                        names: vec![lst_var],
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
        Ty::Map { .. } => {
            let vv = p.expr_var(val_var)?;
            let pv = p.expr_var(path_var)?;
            let sv = p.expr_var(strict_var)?;
            let lam = decode_lambda(p, derived_idx, 1)?;
            json_generic_call(
                p,
                "decode_map",
                proof_ty_args[1],
                subst,
                owner,
                depth,
                vec![vv, pv, sv, lam],
            )?
        }
        Ty::Option(_) => {
            let vv = p.expr_var(val_var)?;
            let pv = p.expr_var(path_var)?;
            let sv = p.expr_var(strict_var)?;
            let lam = decode_lambda(p, derived_idx, 0)?;
            json_generic_call(
                p,
                "decode_option",
                proof_ty_args[0],
                subst,
                owner,
                depth,
                vec![vv, pv, sv, lam],
            )?
        }
        Ty::Tuple(ref parts) => {
            let count = parts.len();
            let json_src = json_data_source(p, "Json")?;
            let json_ty = lower_type(p, json_src, None)?;
            let arr_var = p.fresh()?;
            let err_var = p.fresh()?;
            let mut part_vars = Vec::with_capacity(count);
            for _ in 0..count {
                part_vars.push(p.fresh()?);
            }
            let mut tup_items = Vec::with_capacity(count);
            for pv in &part_vars {
                tup_items.push(p.expr_var(pv.clone())?);
            }
            let tup = p.expr_tuple(tup_items)?;
            let mut inner = p.expr_con(ret_ty, ok_ctor.clone(), vec![tup])?;
            for i in (0..count).rev() {
                let raw_item = p.fresh()?;
                let pv_e = p.expr_var(path_var.clone())?;
                let i_lit = p.expr_int(i as u64)?;
                let idx_path = json_call(p, "index_path", vec![pv_e, i_lit])?;
                let riv = p.expr_var(raw_item.clone())?;
                let sv_e = p.expr_var(strict_var.clone())?;
                let dec_expr = part_from_json(p, derived_idx, i, riv, idx_path, sv_e)?;
                let err_item = p.fresh()?;
                let eiv = p.expr_var(err_item.clone())?;
                let dec_err = p.expr_con(ret_ty, err_ctor.clone(), vec![eiv])?;
                let dec_case = p.alloc_expr(Expr::Case {
                    expr: dec_expr,
                    arms: vec![
                        Arm {
                            ctor: ok_ctor.clone(),
                            names: vec![part_vars[i].clone()],
                            body: inner,
                        },
                        Arm {
                            ctor: err_ctor.clone(),
                            names: vec![err_item],
                            body: dec_err,
                        },
                    ],
                })?;
                let av = p.expr_var(arr_var.clone())?;
                let i_lit2 = p.expr_int(i as u64)?;
                let get_e = p.expr_prim(
                    Prim::List {
                        ty: json_ty,
                        op: ListOp::Nullary(CoreListNullaryOp::Get),
                    },
                    vec![av, i_lit2],
                )?;
                let stop = p.expr_stop("unreachable tuple index")?;
                let none_ctor = p.copy_text("None")?;
                let some_ctor = p.copy_text("Some")?;
                inner = p.alloc_expr(Expr::Case {
                    expr: get_e,
                    arms: vec![
                        Arm {
                            ctor: none_ctor,
                            names: Vec::new(),
                            body: stop,
                        },
                        Arm {
                            ctor: some_ctor,
                            names: vec![raw_item],
                            body: dec_case,
                        },
                    ],
                })?;
            }
            let vv = p.expr_var(val_var)?;
            let cnt = p.expr_int(count as u64)?;
            let pv = p.expr_var(path_var)?;
            let exp_arr = json_call(p, "expect_tuple_array", vec![vv, cnt, pv])?;
            let ev = p.expr_var(err_var.clone())?;
            let err_body = p.expr_con(ret_ty, err_ctor.clone(), vec![ev])?;
            p.alloc_expr(Expr::Case {
                expr: exp_arr,
                arms: vec![
                    Arm {
                        ctor: ok_ctor,
                        names: vec![arr_var],
                        body: inner,
                    },
                    Arm {
                        ctor: err_ctor,
                        names: vec![err_var],
                        body: err_body,
                    },
                ],
            })?
        }
        _ => {
            let ctors = lower_ctors(p, ty)?;
            if is_json_struct(p, &p.derived[derived_idx]) {
                let (labels, _) = ctor_labels(p, ty, 0, &ctors[0])?;
                let vv = p.expr_var(val_var)?;
                decode_fields_object(
                    p,
                    derived_idx,
                    0,
                    &ctors[0],
                    &labels,
                    vv,
                    &path_var,
                    &strict_var,
                )?
            } else {
                let text_src = p.types_overlay.builtin("Text");
                let int_src = p.types_overlay.builtin("Int");
                let text_ty = lower_type(p, text_src, None)?;
                let int_ty = lower_type(p, int_src, None)?;
                let json_src = json_data_source(p, "Json")?;
                let json_ty = lower_type(p, json_src, None)?;
                let json_ctors = lower_ctors(p, json_ty)?;
                let type_label = p.source_type(proof_ty_id).name.clone();

                let mut offsets = Vec::with_capacity(ctors.len());
                let mut running = 0usize;
                for c in &ctors {
                    offsets.push(running);
                    running += c.fields.len();
                }

                let mut top_arms = Vec::with_capacity(json_ctors.len());
                for jctor in &json_ctors {
                    let mut jnames = Vec::with_capacity(jctor.fields.len());
                    for _ in 0..jctor.fields.len() {
                        jnames.push(p.fresh()?);
                    }
                    let jbody = if jctor.name == "String" {
                        let str_var = jnames[0].clone();
                        let mut branch =
                            bad_enum_case(p, ret_ty, &val_var, &path_var, &type_label)?;
                        for ctor in ctors.iter().rev() {
                            if !ctor.fields.is_empty() {
                                continue;
                            }
                            let tag = snake_string(p, &ctor.name)?;
                            let sv = p.expr_var(str_var.clone())?;
                            let tv = p.expr_text(&tag)?;
                            let cond = p.expr_prim(Prim::Eq(text_ty), vec![sv, tv])?;
                            let con = p.expr_con(ty, ctor.name.clone(), Vec::new())?;
                            let ok_e = p.expr_con(ret_ty, ok_ctor.clone(), vec![con])?;
                            branch = p.expr_if(cond, ok_e, branch)?;
                        }
                        branch
                    } else if jctor.name == "Object" {
                        let map_var = jnames[0].clone();
                        let mut branch =
                            bad_enum_case(p, ret_ty, &val_var, &path_var, &type_label)?;
                        for (i, ctor) in ctors.iter().enumerate().rev() {
                            if ctor.fields.is_empty() {
                                continue;
                            }
                            let tag = snake_string(p, &ctor.name)?;
                            let (labels, unnamed) = ctor_labels(p, ty, i, ctor)?;
                            let mv = p.expr_var(map_var.clone())?;
                            let tag_e = p.expr_text(&tag)?;
                            let get_e = p.expr_prim(
                                Prim::Map {
                                    key: text_ty,
                                    value: json_ty,
                                    op: MapOp::Nullary(CoreMapNullaryOp::Get),
                                },
                                vec![mv, tag_e],
                            )?;
                            let inner_var = p.fresh()?;
                            let inner_path = p.fresh()?;
                            let payload_body = if unnamed {
                                let iv = p.expr_var(inner_var.clone())?;
                                let ipv = p.expr_var(inner_path.clone())?;
                                let sv = p.expr_var(strict_var.clone())?;
                                let dec_e =
                                    part_from_json(p, derived_idx, offsets[i], iv, ipv, sv)?;
                                let ok_v = p.fresh()?;
                                let err_v = p.fresh()?;
                                let okv_e = p.expr_var(ok_v.clone())?;
                                let wrapped = p.expr_con(ty, ctor.name.clone(), vec![okv_e])?;
                                let ok_b = p.expr_con(ret_ty, ok_ctor.clone(), vec![wrapped])?;
                                let errv_e = p.expr_var(err_v.clone())?;
                                let err_b = p.expr_con(ret_ty, err_ctor.clone(), vec![errv_e])?;
                                p.alloc_expr(Expr::Case {
                                    expr: dec_e,
                                    arms: vec![
                                        Arm {
                                            ctor: ok_ctor.clone(),
                                            names: vec![ok_v],
                                            body: ok_b,
                                        },
                                        Arm {
                                            ctor: err_ctor.clone(),
                                            names: vec![err_v],
                                            body: err_b,
                                        },
                                    ],
                                })?
                            } else {
                                let iv = p.expr_var(inner_var.clone())?;
                                decode_fields_object(
                                    p,
                                    derived_idx,
                                    offsets[i],
                                    ctor,
                                    &labels,
                                    iv,
                                    &inner_path,
                                    &strict_var,
                                )?
                            };
                            let pv_e = p.expr_var(path_var.clone())?;
                            let tag_e2 = p.expr_text(&tag)?;
                            let jpath = json_call(p, "join_path", vec![pv_e, tag_e2])?;
                            let some_body = p.expr_let(inner_path, jpath, payload_body)?;
                            let none_ctor = p.copy_text("None")?;
                            let some_ctor = p.copy_text("Some")?;
                            branch = p.alloc_expr(Expr::Case {
                                expr: get_e,
                                arms: vec![
                                    Arm {
                                        ctor: none_ctor,
                                        names: Vec::new(),
                                        body: branch,
                                    },
                                    Arm {
                                        ctor: some_ctor,
                                        names: vec![inner_var],
                                        body: some_body,
                                    },
                                ],
                            })?;
                        }
                        let mv2 = p.expr_var(map_var)?;
                        let sz = p.expr_prim(
                            Prim::Map {
                                key: text_ty,
                                value: json_ty,
                                op: MapOp::Nullary(CoreMapNullaryOp::Size),
                            },
                            vec![mv2],
                        )?;
                        let one = p.expr_int(1)?;
                        let cond = p.expr_prim(Prim::Eq(int_ty), vec![sz, one])?;
                        let else_b = bad_enum_case(p, ret_ty, &val_var, &path_var, &type_label)?;
                        p.expr_if(cond, branch, else_b)?
                    } else {
                        bad_enum_case(p, ret_ty, &val_var, &path_var, &type_label)?
                    };
                    top_arms.push(Arm {
                        ctor: jctor.name.clone(),
                        names: jnames,
                        body: jbody,
                    });
                }
                let vv = p.expr_var(val_var)?;
                p.alloc_expr(Expr::Case {
                    expr: vv,
                    arms: top_arms,
                })?
            }
        }
    };
    p.instances[inst_idx].fun.body = body;
    p.instances[inst_idx].body_set = true;
    Ok(())
}

fn schema_fields_object(
    p: &mut Lowering<'_>,
    derived_idx: usize,
    first: usize,
    ctor: &CtorDecl,
    labels: &[String],
    seen_var: &str,
) -> LowerResult<ExprId> {
    let text_src = p.types_overlay.builtin("Text");
    let text_ty = lower_type(p, text_src, None)?;
    let json_ty = p.instances[p.derived[derived_idx].instance].fun.ret;
    let mut flat = Vec::with_capacity(ctor.fields.len() * 2);
    let mut req_items = Vec::new();
    for (j, f) in ctor.fields.iter().enumerate() {
        flat.push(p.expr_text(&labels[j])?);
        let sv = p.expr_var(seen_var.to_string())?;
        flat.push(part_json_schema(p, derived_idx, first + j, sv)?);
        if !matches!(p.core_ty(f.ty), Ty::Option(_)) {
            req_items.push(p.expr_text(&labels[j])?);
        }
    }
    let props_map = p.expr_lit_map(text_ty, json_ty, flat)?;
    let req_list = p.expr_lit_list(text_ty, req_items)?;
    json_call(p, "schema_object", vec![props_map, req_list])
}

fn derive_json_schema(p: &mut Lowering<'_>, derived_idx: usize) -> LowerResult<()> {
    let (span, ty, proof_id, inst_idx) = {
        let d = &p.derived[derived_idx];
        (d.span, d.ty, d.proof, d.instance)
    };
    p.span = span;
    let text_src = p.types_overlay.builtin("Text");
    let text_ty = lower_type(p, text_src, None)?;
    let json_ty = p.instances[inst_idx].fun.ret;
    let orig_seen_var = p.instances[inst_idx].fun.params[0].name.clone();
    let is_data = matches!(p.core_ty(ty), Ty::Data(_));

    let next_seen_var = if is_data {
        p.fresh()?
    } else {
        orig_seen_var.clone()
    };

    let inner_body = match p.core_ty(ty).clone() {
        Ty::List(_) | Ty::Set(_) => {
            let sv = p.expr_var(next_seen_var.clone())?;
            let inner = part_json_schema(p, derived_idx, 0, sv)?;
            json_call(p, "schema_array", vec![inner])?
        }
        Ty::Map { .. } => {
            let sv = p.expr_var(next_seen_var.clone())?;
            let inner = part_json_schema(p, derived_idx, 1, sv)?;
            json_call(p, "schema_map", vec![inner])?
        }
        Ty::Option(_) => {
            let sv = p.expr_var(next_seen_var.clone())?;
            let inner = part_json_schema(p, derived_idx, 0, sv)?;
            json_call(p, "schema_nullable", vec![inner])?
        }
        Ty::Tuple(ref parts) => {
            let mut items = Vec::with_capacity(parts.len());
            for i in 0..parts.len() {
                let sv = p.expr_var(next_seen_var.clone())?;
                items.push(part_json_schema(p, derived_idx, i, sv)?);
            }
            let lst = p.expr_lit_list(json_ty, items)?;
            json_call(p, "schema_tuple", vec![lst])?
        }
        _ => {
            let ctors = lower_ctors(p, ty)?;
            if is_json_struct(p, &p.derived[derived_idx]) {
                let (labels, _) = ctor_labels(p, ty, 0, &ctors[0])?;
                schema_fields_object(p, derived_idx, 0, &ctors[0], &labels, &next_seen_var)?
            } else if ctors.iter().all(|c| c.fields.is_empty()) {
                let mut tags = Vec::with_capacity(ctors.len());
                for c in &ctors {
                    let t = snake_string(p, &c.name)?;
                    tags.push(p.expr_text(&t)?);
                }
                let tag_list = p.expr_lit_list(text_ty, tags)?;
                json_call(p, "schema_enum_strings", vec![tag_list])?
            } else {
                let mut variants = Vec::with_capacity(ctors.len());
                let mut first = 0usize;
                for (i, ctor) in ctors.iter().enumerate() {
                    let tag = snake_string(p, &ctor.name)?;
                    let v = if ctor.fields.is_empty() {
                        let te = p.expr_text(&tag)?;
                        let one_list = p.expr_lit_list(text_ty, vec![te])?;
                        json_call(p, "schema_enum_strings", vec![one_list])?
                    } else {
                        let (labels, unnamed) = ctor_labels(p, ty, i, ctor)?;
                        let inner = if unnamed {
                            let sv = p.expr_var(next_seen_var.clone())?;
                            part_json_schema(p, derived_idx, first, sv)?
                        } else {
                            schema_fields_object(
                                p,
                                derived_idx,
                                first,
                                ctor,
                                &labels,
                                &next_seen_var,
                            )?
                        };
                        let te = p.expr_text(&tag)?;
                        json_call(p, "schema_tagged_case", vec![te, inner])?
                    };
                    variants.push(v);
                    first += ctor.fields.len();
                }
                let var_list = p.expr_lit_list(json_ty, variants)?;
                json_call(p, "schema_any_of", vec![var_list])?
            }
        }
    };

    let body = if is_data {
        let full_key = lower_type_name(p, ty)?;
        let short_name = p.source_type(p.proof(proof_id).ty).name.clone();
        let sv1 = p.expr_var(orig_seen_var.clone())?;
        let fk1 = p.expr_text(&full_key)?;
        let cond = p.expr_prim(
            Prim::List {
                ty: text_ty,
                op: ListOp::Nullary(CoreListNullaryOp::Contains),
            },
            vec![sv1, fk1],
        )?;
        let sn_e = p.expr_text(&short_name)?;
        let then_ = json_call(p, "schema_ref", vec![sn_e])?;
        let sv2 = p.expr_var(orig_seen_var)?;
        let fk2 = p.expr_text(&full_key)?;
        let pushed = p.expr_prim(
            Prim::List {
                ty: text_ty,
                op: ListOp::Nullary(CoreListNullaryOp::Append),
            },
            vec![sv2, fk2],
        )?;
        let else_ = p.expr_let(next_seen_var, pushed, inner_body)?;
        p.expr_if(cond, then_, else_)?
    } else {
        inner_body
    };

    p.instances[inst_idx].fun.body = body;
    p.instances[inst_idx].body_set = true;
    Ok(())
}

/// Drains all pending derived ability helper functions into Core function bodies.
pub(crate) fn lower_derived_functions(p: &mut Lowering<'_>) -> LowerResult<()> {
    while p.derived_pending_head < p.derived.len() {
        p.work(1)?;
        let idx = p.derived_pending_head;
        p.derived_pending_head += 1;
        match p.derived[idx].kind {
            DerivedKind::Text => derive_text(p, idx)?,
            DerivedKind::Order => derive_order(p, idx)?,
            DerivedKind::ToJson => derive_to_json(p, idx)?,
            DerivedKind::FromJson => derive_from_json(p, idx)?,
            DerivedKind::JsonSchema => derive_json_schema(p, idx)?,
        }
    }
    Ok(())
}
