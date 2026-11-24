//! Arena-indexed CPS state, continuations, work tasks, and helper builders
//! (`src/lower/cps.h` parity, zero raw pointers / zero `Rc<RefCell<_>>`).

use mo_check::{
    CheckedArm, CheckedBinding, CheckedBindingId, CheckedBody, CheckedExprId, CheckedExpression,
    CheckedPattern, CheckedPatternId, ModuleId, TypeId,
};
use mo_core::{Arm, Expr, ExprId, Param, Prim, Ty, TyId};
use mo_syntax::{NodeId, SyntaxKind};

use crate::context::{ExprSlot, LowerResult, Lowering, SubstId, UNSET_EXPR};
use crate::types::{lower_ctors, lower_type};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct VarNodeId(pub(crate) u32);

#[derive(Debug, Clone, Copy)]
pub(crate) struct VarNode {
    pub(crate) binding: CheckedBindingId,
    pub(crate) next: Option<VarNodeId>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct LoopId(pub(crate) u32);

#[derive(Debug, Clone, Copy)]
pub(crate) struct LoopInfo {
    pub(crate) break_k: ContId,
    pub(crate) continue_k: ContId,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ContId(pub(crate) u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ArgBufId(pub(crate) u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct SplitFieldsId(pub(crate) u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct AnchorValsId(pub(crate) u32);

#[derive(Debug, Clone, Copy, Default)]
pub(crate) struct State {
    pub(crate) variables: Option<VarNodeId>,
    pub(crate) loop_info: Option<LoopId>,
    pub(crate) return_k: Option<ContId>,
    pub(crate) split_binding: Option<CheckedBindingId>,
    pub(crate) split_fields: Option<SplitFieldsId>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ContinuationKind {
    End,
    Sequence,
    Bind,
    FieldRebuild,
    Arguments,
    Jump,
    Select,
    Pattern,
    Match,
    For,
    Option,
    Try,
    Text,
    Promise,
    Anchor,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct AnchorValue {
    pub(crate) expression: CheckedExprId,
    pub(crate) slot: usize,
}

#[derive(Debug, Clone)]
pub(crate) struct Continuation {
    pub(crate) kind: ContinuationKind,
    pub(crate) next: Option<ContId>,
    pub(crate) row: Option<CheckedExprId>,
    pub(crate) at: usize,
    pub(crate) skip: usize,
    pub(crate) name: String,
    pub(crate) binding: Option<CheckedBindingId>,
    pub(crate) arguments: Option<ArgBufId>,
    pub(crate) variables: Option<VarNodeId>,
    pub(crate) result: bool,
    pub(crate) diagnostic: bool,
    pub(crate) extra: Option<ExprId>,
    pub(crate) then_: Option<ExprId>,
    pub(crate) else_: Option<ExprId>,
    pub(crate) position: String,
    pub(crate) position_type: Option<TyId>,
    pub(crate) pattern: Option<CheckedPatternId>,
    pub(crate) values: Option<AnchorValsId>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum TaskKind {
    Row,
    Finish,
    Condition,
    Pattern,
    Arm,
    Copy,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct Task {
    pub(crate) kind: TaskKind,
    pub(crate) row: CheckedExprId,
    pub(crate) state: State,
    pub(crate) continuation: Option<ContId>,
    pub(crate) out: ExprSlot,
    pub(crate) success_slot: Option<usize>,
    pub(crate) value: Option<ExprId>,
    pub(crate) then_: Option<ExprId>,
    pub(crate) else_: Option<ExprId>,
    pub(crate) pattern: Option<CheckedPatternId>,
    pub(crate) match_arm: Option<(mo_check::CheckedMatchId, usize)>,
    pub(crate) diagnostic: bool,
}

/// Arena-backed CPS body lowerer (`struct body` in `src/lower/cps.h`).
pub(crate) struct BodyLowerer<'a, 'b> {
    pub(crate) p: &'a mut Lowering<'b>,
    /// Index into `p.instances` when lowering a function/spec, or `None` when lowering a constant.
    pub(crate) instance_idx: Option<usize>,
    pub(crate) owner: ModuleId,
    pub(crate) syntax_module: ModuleId,
    pub(crate) subst: Option<SubstId>,
    pub(crate) depth: u32,
    pub(crate) meta_name: String,
    pub(crate) source: &'b CheckedBody,
    pub(crate) names: Vec<String>,
    pub(crate) var_nodes: Vec<VarNode>,
    pub(crate) loops: Vec<LoopInfo>,
    pub(crate) conts: Vec<Continuation>,
    pub(crate) arg_bufs: Vec<Vec<ExprId>>,
    pub(crate) split_fields: Vec<Vec<String>>,
    pub(crate) anchor_vals: Vec<Vec<AnchorValue>>,
    pub(crate) roots: Vec<Option<ExprId>>,
    pub(crate) tasks: Vec<Task>,
}

impl<'a, 'b> BodyLowerer<'a, 'b> {
    pub(crate) fn row(&self, id: CheckedExprId) -> &'b CheckedExpression {
        self.source.expr(id)
    }

    pub(crate) fn binding(&self, id: CheckedBindingId) -> &'b CheckedBinding {
        self.source.binding(id)
    }

    pub(crate) fn pattern(&self, id: CheckedPatternId) -> &'b CheckedPattern {
        self.source.pattern(id)
    }

    pub(crate) fn arm(&self, match_id: mo_check::CheckedMatchId, idx: usize) -> &'b CheckedArm {
        &self.source.match_info(match_id).arms[idx]
    }

    pub(crate) fn binding_name(&self, id: CheckedBindingId) -> String {
        self.names[id.index()].clone()
    }

    pub(crate) fn syntax_kind(&self, id: NodeId) -> SyntaxKind {
        self.p.syntax_node(self.syntax_module, id).kind
    }

    pub(crate) fn syntax_span(&self, id: NodeId) -> mo_base::Span {
        self.p.syntax_node(self.syntax_module, id).span
    }

    pub(crate) fn lower_ty(&mut self, src: TypeId) -> LowerResult<TyId> {
        lower_type(self.p, src, self.subst)
    }

    pub(crate) fn alloc_root_slot(&mut self) -> usize {
        let idx = self.roots.len();
        self.roots.push(None);
        idx
    }

    pub(crate) fn alloc_root_slots(&mut self, count: usize) -> LowerResult<usize> {
        self.p.charge_alloc(count, 8, 8)?;
        let base = self.roots.len();
        self.roots.resize(base + count, None);
        Ok(base)
    }

    pub(crate) fn push_var_node(
        &mut self,
        binding: CheckedBindingId,
        next: Option<VarNodeId>,
    ) -> LowerResult<VarNodeId> {
        self.p.charge_alloc(1, 16, 8)?;
        let id = VarNodeId(self.var_nodes.len() as u32);
        self.var_nodes.push(VarNode { binding, next });
        Ok(id)
    }

    pub(crate) fn alloc_loop(&mut self, info: LoopInfo) -> LowerResult<LoopId> {
        self.p.charge_alloc(1, 16, 8)?;
        let id = LoopId(self.loops.len() as u32);
        self.loops.push(info);
        Ok(id)
    }

    pub(crate) fn alloc_cont(&mut self, kind: ContinuationKind) -> LowerResult<ContId> {
        self.p.charge_alloc(1, 152, 8)?;
        let id = ContId(self.conts.len() as u32);
        self.conts.push(Continuation {
            kind,
            next: None,
            row: None,
            at: 0,
            skip: 0,
            name: String::new(),
            binding: None,
            arguments: None,
            variables: None,
            result: false,
            diagnostic: false,
            extra: None,
            then_: None,
            else_: None,
            position: String::new(),
            position_type: None,
            pattern: None,
            values: None,
        });
        Ok(id)
    }

    pub(crate) fn clone_cont(&mut self, id: ContId) -> LowerResult<ContId> {
        self.p.charge_alloc(1, 152, 8)?;
        let copy = self.conts[id.0 as usize].clone();
        let new_id = ContId(self.conts.len() as u32);
        self.conts.push(copy);
        Ok(new_id)
    }

    pub(crate) fn cont(&self, id: ContId) -> &Continuation {
        &self.conts[id.0 as usize]
    }

    pub(crate) fn cont_mut(&mut self, id: ContId) -> &mut Continuation {
        &mut self.conts[id.0 as usize]
    }

    pub(crate) fn alloc_arg_buf(&mut self, count: usize) -> LowerResult<ArgBufId> {
        self.p.charge_alloc(count, 8, 8)?;
        let id = ArgBufId(self.arg_bufs.len() as u32);
        self.arg_bufs.push(vec![UNSET_EXPR; count]);
        Ok(id)
    }

    pub(crate) fn alloc_split_fields(&mut self, names: Vec<String>) -> SplitFieldsId {
        let id = SplitFieldsId(self.split_fields.len() as u32);
        self.split_fields.push(names);
        id
    }

    pub(crate) fn alloc_anchor_vals(&mut self, vals: Vec<AnchorValue>) -> AnchorValsId {
        let id = AnchorValsId(self.anchor_vals.len() as u32);
        self.anchor_vals.push(vals);
        id
    }

    pub(crate) fn push_task(&mut self, task: Task) -> LowerResult<()> {
        self.p.charge_alloc(1, 136, 8)?;
        self.tasks.push(task);
        Ok(())
    }

    pub(crate) fn write_slot(&mut self, slot: ExprSlot, value: ExprId) {
        self.p.write_slot(&mut self.roots, slot, value);
    }

    pub(crate) fn binder_slots(&mut self, expr: ExprId, count: usize) -> LowerResult<()> {
        self.p.charge_alloc(count, 24, 8)?;
        self.p.set_binders(expr, vec![None; count]);
        Ok(())
    }

    pub(crate) fn set_binder(
        &mut self,
        expr: ExprId,
        slot: usize,
        binding: CheckedBindingId,
    ) -> LowerResult<()> {
        let bname = self.p.copy_text(&self.binding(binding).name)?;
        self.p.provenance[expr.0 as usize].binders[slot] = Some(bname);
        Ok(())
    }

    pub(crate) fn cps_let(&mut self, name: String, value: ExprId) -> LowerResult<ExprId> {
        let e = self.p.expr_let(name, value, UNSET_EXPR)?;
        self.binder_slots(e, 1)?;
        Ok(e)
    }

    pub(crate) fn cps_unary(&mut self, prim: Prim, value: ExprId) -> LowerResult<ExprId> {
        self.p.expr_prim(prim, vec![value])
    }

    pub(crate) fn cps_jump(&mut self, k: ContId, value: Option<ExprId>) -> LowerResult<ExprId> {
        let prev_span = self.p.span;
        let cont = self.cont(k).clone();
        if let Some(rid) = cont.row {
            self.p.span = self.syntax_span(self.row(rid).syntax);
        }
        let mut args = Vec::new();
        if cont.result
            && let Some(v) = value
        {
            args.push(v);
        }
        if let Some(ex) = cont.extra {
            args.push(ex);
        }
        let mut cur = cont.variables;
        while let Some(vid) = cur {
            let vn = self.var_nodes[vid.0 as usize];
            let vname = self.binding_name(vn.binding);
            args.push(self.p.expr_var(vname)?);
            cur = vn.next;
        }
        self.p.charge_alloc(args.len(), 8, 8)?;
        let e = self.p.alloc_expr(Expr::Jump {
            name: cont.name,
            args,
        })?;
        self.p.span = prev_span;
        Ok(e)
    }

    pub(crate) fn cps_join(
        &mut self,
        k: ContId,
        result: Option<TyId>,
    ) -> LowerResult<(ExprId, Option<String>)> {
        let cont = self.cont(k).clone();
        let mut params: Vec<Param> = Vec::new();
        let mut result_param_name = None;
        if let Some(res_ty) = result {
            let pname = self.p.fresh()?;
            result_param_name = Some(pname.clone());
            params.push(self.p.make_param(pname, res_ty)?);
        }
        if let Some(pos_ty) = cont.position_type {
            params.push(self.p.make_param(cont.position, pos_ty)?);
        }
        let mut var_bindings: Vec<(usize, CheckedBindingId)> = Vec::new();
        let mut cur = cont.variables;
        while let Some(vid) = cur {
            let vn = self.var_nodes[vid.0 as usize];
            let vname = self.binding_name(vn.binding);
            let vty = self.lower_ty(self.binding(vn.binding).ty)?;
            let slot = params.len();
            params.push(self.p.make_param(vname, vty)?);
            var_bindings.push((slot, vn.binding));
            cur = vn.next;
        }
        let count = params.len();
        let e = self.p.alloc_expr(Expr::Join {
            name: cont.name,
            params,
            body: UNSET_EXPR,
            rest: UNSET_EXPR,
        })?;
        self.binder_slots(e, count)?;
        for (slot, b) in var_bindings {
            self.set_binder(e, slot, b)?;
        }
        Ok((e, result_param_name))
    }

    pub(crate) fn cps_field_read(
        &mut self,
        base: TyId,
        value: ExprId,
        field: u32,
    ) -> LowerResult<ExprId> {
        if matches!(self.p.core_ty(base), Ty::Tuple(_)) {
            return self.p.expr_proj(value, u64::from(field));
        }
        let ctors = lower_ctors(self.p, base)?;
        debug_assert!(ctors.len() == 1 && (field as usize) < ctors[0].fields.len());
        let mut names = Vec::with_capacity(ctors[0].fields.len());
        for _ in 0..ctors[0].fields.len() {
            names.push(self.p.fresh()?);
        }
        let chosen = self.p.expr_var(names[field as usize].clone())?;
        self.p.alloc_expr(Expr::Case {
            expr: value,
            arms: vec![Arm {
                ctor: ctors[0].name.clone(),
                names,
                body: chosen,
            }],
        })
    }

    pub(crate) fn emit(
        &mut self,
        row: CheckedExprId,
        state: State,
        continuation: ContId,
        out: ExprSlot,
    ) -> LowerResult<()> {
        self.push_task(Task {
            kind: TaskKind::Row,
            row,
            state,
            continuation: Some(continuation),
            out,
            success_slot: None,
            value: None,
            then_: None,
            else_: None,
            pattern: None,
            match_arm: None,
            diagnostic: false,
        })
    }

    pub(crate) fn finish(
        &mut self,
        value: Option<ExprId>,
        state: State,
        continuation: ContId,
        out: ExprSlot,
    ) -> LowerResult<()> {
        self.push_task(Task {
            kind: TaskKind::Finish,
            row: CheckedExprId(0),
            state,
            continuation: Some(continuation),
            out,
            success_slot: None,
            value,
            then_: None,
            else_: None,
            pattern: None,
            match_arm: None,
            diagnostic: false,
        })
    }
}
