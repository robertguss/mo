//! Top-level body inference & checked-body builder (`src/check/bodies.c` parity).

use std::collections::HashMap;

use mo_base::{DiagBag, Diagnostic, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_DIAGNOSTICS, MO_LIMIT_PROGRAM_CHECK_WORK};
use mo_syntax::{NodeId, SyntaxKind, TokenKind};

use crate::abilities::AbilityArena;
use crate::checked::{
    CheckedApi, CheckedArm, CheckedBinding, CheckedBody, CheckedCondition, CheckedConstant,
    CheckedExpression, CheckedFunction, CheckedLambda, CheckedMatch, CheckedService, CheckedSpec,
    CheckedSpecKind, CheckedTry, ConstructorRef, InferredBody, InferredExpression,
};
use crate::ids::{
    CheckedBindingId, CheckedExprId, CheckedLambdaId, CheckedMatchId, FuncSigId, ModuleId, SymRef,
    TypeId,
};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::patterns::MatchPatternArena;
use crate::signatures::Signatures;
use crate::types::{SourceTypeKind, TypeOwner};

use super::{
    EnclosingContext, InferState, InferValue, NameRef, NoteId, SIZEOF_CHECKED_BINDING,
    SIZEOF_CHECKED_EXPR, SIZEOF_PTR,
};

/// Shared constant checking context passed through body inference (`struct mo_constant_context`).
#[derive(Debug, Clone, Default)]
pub struct ConstantContext {
    /// `(symbol, resolved_type)` in `(module, decl_span.start)` source order.
    pub items: Vec<(SymRef, Option<TypeId>)>,
    /// Checked initializers parallel to `items`.
    pub initializers: Vec<CheckedBody>,
    /// Indices into `items`, sorted by `SymRef` for O(log N) lookup.
    pub by_symbol: Vec<usize>,
    pub done: Vec<bool>,
    /// `(item_index, reference_span)` when a constant pauses for an unresolved dependency.
    pub pending: Option<(usize, Span)>,
}

impl ConstantContext {
    #[must_use]
    pub fn index_by_symbol(&self, symbol: SymRef) -> usize {
        let mut lo = 0usize;
        let mut hi = self.by_symbol.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.items[self.by_symbol[mid]].0 < symbol {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        debug_assert!(lo < self.by_symbol.len() && self.items[self.by_symbol[lo]].0 == symbol);
        self.by_symbol[lo]
    }
}

/// Immutable read-only view of prepared services, actors, and APIs during body checking
/// (`signatures->service_program` in C).
#[derive(Clone, Copy)]
pub struct ProgramServicesView<'a> {
    pub services: &'a [CheckedService],
    pub apis: &'a [CheckedApi],
    pub function_sigs: &'a [FuncSigId],
}

#[expect(
    clippy::too_many_arguments,
    reason = "body inference entry points mirror C infer.h and thread shared checker arenas"
)]
fn infer_body_core(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: Option<&mut ConstantContext>,
    services_view: Option<ProgramServicesView<'_>>,
    enclosing: EnclosingContext,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
    checked_out: Option<&mut CheckedFunction>,
    mut spec: Option<&mut CheckedSpec>,
    mut conditions: Option<&mut Vec<CheckedCondition>>,
    halt: &mut bool,
) -> Option<InferredBody> {
    debug_assert!(signatures.work <= MO_LIMIT_PROGRAM_CHECK_WORK);
    *halt = false;

    let tree = &modules.module(enclosing.module).syntax.tree;
    let syntax = enclosing.declaration;
    let written = spec.as_ref().map_or(syntax, |s| s.syntax);
    let written_span = tree.node(written).span;
    let promise = spec
        .as_ref()
        .is_some_and(|s| s.kind == CheckedSpecKind::Promise);
    let anchor = spec
        .as_ref()
        .is_some_and(|s| s.kind == CheckedSpecKind::Anchor);
    let test = spec
        .as_ref()
        .is_some_and(|s| s.kind == CheckedSpecKind::Test);
    let service_promise = (promise || anchor) && spec.as_ref().is_some_and(|s| s.subject.is_none());
    let constant = enclosing.owner_kind == SyntaxKind::Const;
    let service = matches!(
        enclosing.owner_kind,
        SyntaxKind::ServiceState | SyntaxKind::ServiceEvery
    );
    let want_checked = checked_out.is_some();
    let fn_type_id = enclosing.fn_type;
    let requirements = enclosing.requirements;
    let fn_sig_id = enclosing.func_sig_id;
    let owner_sym = enclosing.owner_symbol;

    let mut p = InferState {
        modules,
        interfaces,
        signatures,
        abilities_arena,
        ability_cache: Default::default(),
        constants,
        services_view,
        diags,
        arena_remaining,
        enclosing,
        terms: Vec::new(),
        names: HashMap::new(),
        locals: Vec::new(),
        active_locals_head: None,
        notes: Vec::new(),
        notes_head: None,
        notes_tail: None,
        emitted: Vec::new(),
        lambdas: Vec::new(),
        matches_emitted: Vec::new(),
        match_patterns: MatchPatternArena::new(),
        typed_patterns: Vec::new(),
        match_checks: Vec::new(),
        pattern_defaults: Vec::new(),
        dispatches: Vec::new(),
        ability_modules: Vec::new(),
        failures: Vec::new(),
        equal_types: Vec::new(),
        capture_loops: Vec::new(),
        capture_paths: Vec::new(),
        capture_writes: Vec::new(),
        lambda_frame: None,
        capture_loop: None,
        capture_path: None,
        capture_free: 0,
        result: super::TermId(0),
        span: written_span,
        error: None,
        error_unused: None,
        unused: None,
        type_label: None,
        expectation: None,
        node_expectation: None,
        binding_count: 0,
        depth: 0,
        loops: 0,
        lambda_depth: 0,
        work: 0,
        epoch: 0,
        pattern_group: 0,
        pattern_depth: 0,
        checked: want_checked,
        spec: spec.is_some() && !service_promise,
        keep_patterns: false,
        failed: false,
        fatal: false,
        partial: false,
        unavailable: false,
    };

    let mut finalized = false;

    let body_result: Option<(InferredBody, Option<CheckedFunction>)> = (|| {
        if !want_checked
            && let Some(specs_nid) = p.node(syntax).children.get(4).copied().flatten()
            && !p.node(specs_nid).children.is_empty()
        {
            if let Some(first_spec) = p.node(specs_nid).children[0] {
                p.span = p.node(first_spec).span;
            }
            let _ = p.unfinished();
            return None;
        }

        let instance = if anchor {
            p.function_value(fn_type_id, requirements)
        } else {
            let fn_t = p.annotation(fn_type_id, None, 0)?;
            InferValue::with_type(fn_t)
        };
        let fn_term = instance.ty?;
        let fn_arity = p.terms[fn_term.index()].shape.arity;

        p.result = if constant {
            p.fresh(0)?
        } else if spec.is_none() {
            p.argument(fn_term, fn_arity - 1)?
        } else {
            p.native(if test { "Unit" } else { "Bool" }, 0)?
        };

        let slots = if service_promise {
            fn_arity - 1
        } else {
            fn_arity
        };
        let decl_param_count = p.enclosing.param_names.len();
        let parameter_count = if constant || service {
            0
        } else if spec.is_none() {
            decl_param_count
        } else if promise {
            slots
        } else {
            0
        };

        let mut inputs: Option<Vec<NodeId>> = None;
        let mut spec_notes: Vec<Option<NoteId>> = Vec::new();
        if promise || anchor {
            let anchor_node = if anchor {
                Some(written)
            } else {
                p.node(written)
                    .children
                    .get(2)
                    .copied()
                    .flatten()
                    .and_then(|a| p.node(a).children.first().copied().flatten())
            };
            if let Some(anid) = anchor_node {
                if !p.charge_arena(slots, SIZEOF_PTR) {
                    return None;
                }
                inputs = Some(p.anchor_inputs(anid, slots)?);
            }
            let sn_count = if anchor {
                slots
            } else {
                p.node(written)
                    .children
                    .get(1)
                    .copied()
                    .flatten()
                    .map_or(0, |l| p.node(l).children.len())
            };
            if !p.charge_arena(sn_count, SIZEOF_PTR) {
                return None;
            }
            spec_notes.resize(sn_count, None);
        }

        let mut parameter_notes: Vec<Option<NoteId>> = Vec::new();
        if want_checked {
            if !p.charge_arena(parameter_count, SIZEOF_PTR) {
                return None;
            }
            parameter_notes.resize(parameter_count, None);
        }

        for i in 0..parameter_count {
            let param_ty = p.argument(fn_term, i)?;
            let name_ref = if i < decl_param_count {
                p.enclosing.param_names[i]
            } else if let Some(ref inps) = inputs {
                let nm = p.node(inps[i]).children.first().copied().flatten()?;
                NameRef::Syntax(nm)
            } else {
                NameRef::Synthetic {
                    name: "result",
                    span: Span::new(
                        written_span.file,
                        written_span.start,
                        written_span.start.saturating_add(6),
                    ),
                }
            };
            if !p.bind(name_ref, param_ty, false) {
                return None;
            }
            if promise && let Some(head) = p.active_locals_head {
                p.locals[head.index()].used = true;
            }
            if want_checked {
                parameter_notes[i] = p.notes_tail;
            }
        }

        let mut v = InferValue::none();
        if anchor {
            let values_nid = p.node(written).children.first().copied().flatten()?;
            let values_children = p.node(values_nid).children.clone();
            let inps = inputs.as_ref()?;
            for ch in &values_children {
                let item_nid = (*ch)?;
                let mut slot = 0usize;
                while inps[slot] != item_nid {
                    if !p.work() {
                        return None;
                    }
                    slot += 1;
                    debug_assert!(slot < slots);
                }
                let rhs = p.node(inps[slot]).children.get(1).copied().flatten()?;
                let expected_arg = p.argument(fn_term, slot)?;
                v = p.infer_as(rhs, Some(expected_arg), Some(ErrorCode::ArgumentType));
                v.ty?;
                spec_notes[slot] = v.note;
            }
        } else if promise {
            let lines_nid = p.node(written).children.get(1).copied().flatten()?;
            let lines_children = p.node(lines_nid).children.clone();
            for (i, ch) in lines_children.iter().enumerate() {
                let line_nid = (*ch)?;
                let line = p.infer_as(line_nid, Some(p.result), Some(ErrorCode::ConditionType));
                line.ty?;
                spec_notes[i] = line.note;
                if i == 0 {
                    v = line;
                } else {
                    let mut both = p.graph(
                        InferValue {
                            ty: Some(p.result),
                            never: v.never,
                            ..InferValue::default()
                        },
                        2,
                    );
                    both.ty?;
                    if let Some(gid) = both.graph {
                        let g = &mut p.emitted[gid.index()];
                        g.operation = TokenKind::And;
                        g.operands[0] = v.note;
                        g.operands[1] = line.note;
                    }
                    if !p.record_syntax(written, both.clone()) {
                        return None;
                    }
                    both.note = p.notes_tail;
                    v = both;
                }
            }
        } else if spec.is_some() {
            v = if test {
                let body_nid = p.node(written).children.get(1).copied().flatten()?;
                p.infer_discard(body_nid)
            } else {
                p.infer_as(written, Some(p.result), Some(ErrorCode::ConditionType))
            };
        } else if service {
            let slot = if p.enclosing.owner_kind == SyntaxKind::ServiceState {
                1
            } else {
                0
            };
            let expr_nid = p.node(syntax).children.get(slot).copied().flatten()?;
            v = p.infer_as(expr_nid, Some(p.result), Some(ErrorCode::ServiceValue));
        } else if constant {
            let expr_nid = p.node(syntax).children.get(1).copied().flatten()?;
            v = p.infer_as(expr_nid, Some(p.result), Some(ErrorCode::ResultType));
        } else {
            let body_nid = p.node(syntax).children.get(5).copied().flatten()?;
            v = if p.named(p.result, "Unit") {
                p.infer_discard(body_nid)
            } else {
                p.infer_as(body_nid, Some(p.result), Some(ErrorCode::ResultType))
            };
        }

        if v.ty.is_none() || !p.leave(None) {
            return None;
        }
        if !p.finish_dispatches() || !p.finish_wrapping() {
            return None;
        }

        let note_count = p.notes.len();
        if !p.charge_arena(note_count, 56) {
            return None;
        }
        finalized = true;
        let mut items: Vec<InferredExpression> = Vec::with_capacity(note_count);
        let _ = p.finish_notes(Some(&mut items));
        if !p.fatal && !p.finish_patterns() {
            *halt = true;
            p.diags.sort();
            return None;
        }
        if !p.finish_captures() {
            return None;
        }
        if p.error.is_some() {
            return None;
        }

        let inferred_body = InferredBody { items };
        if !want_checked {
            return Some((inferred_body, None));
        }

        // Build CheckedBody
        let note_list: Vec<NoteId> = p.iter_notes().collect();
        let total_notes = note_list.len();
        if !p.charge_arena(total_notes, SIZEOF_CHECKED_EXPR)
            || !p.charge_arena(p.binding_count as usize, SIZEOF_CHECKED_BINDING)
            || !p.charge_arena(parameter_count, SIZEOF_PTR)
            || !p.charge_arena(p.lambdas.len(), SIZEOF_PTR)
            || !p.charge_arena(total_notes, SIZEOF_PTR)
        {
            return None;
        }

        let mut expr_count = 0u32;
        let mut bindings: Vec<CheckedBinding> = Vec::with_capacity(p.binding_count as usize);
        let mut lambda_notes: Vec<NoteId> = Vec::with_capacity(p.lambdas.len());

        for &nid in &note_list {
            if let Some(gid) = p.notes[nid.index()].graph {
                p.notes[nid.index()].row_index = Some(expr_count);
                expr_count += 1;
                if p.emitted[gid.index()].lambda.is_some() {
                    lambda_notes.push(nid);
                }
            }
            if p.notes[nid.index()].binding != Some(nid) {
                continue;
            }
            let (syntax_opt, name_str, span) = if let Some(snid) = p.notes[nid.index()].syntax {
                let bytes = p.node_text(snid);
                (
                    Some(snid),
                    String::from_utf8_lossy(bytes).into_owned(),
                    p.node(snid).span,
                )
            } else if let Some((sname, sspan)) = p.notes[nid.index()].synthetic_name {
                (None, sname.to_string(), sspan)
            } else {
                (None, String::new(), Span::NONE)
            };
            if !p.charge_arena(name_str.len() + 1, 1) {
                return None;
            }
            let bidx = bindings.len() as u32;
            p.notes[nid.index()].binding_index = Some(bidx);
            let ty = p.notes[nid.index()].resolved_type?;
            let variable = p.notes[nid.index()].variable;
            bindings.push(CheckedBinding {
                syntax: syntax_opt,
                name: name_str,
                span,
                ty,
                variable,
            });
        }
        debug_assert_eq!(bindings.len() as u32, p.binding_count);
        debug_assert_eq!(lambda_notes.len(), p.lambdas.len());

        let mut notes_by_syntax: Vec<(NodeId, NoteId)> = note_list
            .iter()
            .filter_map(|&nid| p.notes[nid.index()].syntax.map(|sn| (sn, nid)))
            .collect();
        notes_by_syntax.sort_by_key(|&(sn, _)| sn.0);

        lambda_notes.sort_by(|&a, &b| {
            let sa = p.note_span(a);
            let sb = p.note_span(b);
            sa.start
                .cmp(&sb.start)
                .then_with(|| sa.end.cmp(&sb.end))
                .then_with(|| a.0.cmp(&b.0))
        });
        for (i, &lnid) in lambda_notes.iter().enumerate() {
            if let Some(gid) = p.notes[lnid.index()].graph
                && let Some(lam_id) = p.emitted[gid.index()].lambda
            {
                p.lambdas[lam_id.index()].number = (i + 1) as u32;
            }
        }

        let mut parameter_bindings = Vec::with_capacity(parameter_count);
        for pn in &parameter_notes {
            let pnid = (*pn)?;
            let bidx = p.notes[pnid.index()].binding_index?;
            parameter_bindings.push(CheckedBindingId(bidx));
        }

        let mut rows: Vec<CheckedExpression> = Vec::with_capacity(expr_count as usize);
        let mut out_patterns = Vec::new();
        let mut out_matches = Vec::new();
        let mut out_lambdas = Vec::new();

        for &nid in &note_list {
            let Some(gid) = p.notes[nid.index()].graph else {
                continue;
            };
            let row_idx = p.notes[nid.index()].row_index?;
            let op_notes = p.emitted[gid.index()].operands.clone();
            if !p.charge_arena(op_notes.len(), 4) {
                return None;
            }
            let mut operands = Vec::with_capacity(op_notes.len());
            for on in &op_notes {
                let onid = (*on)?;
                let oidx = p.notes[onid.index()].row_index?;
                debug_assert!(oidx < row_idx);
                operands.push(CheckedExprId(oidx));
            }
            let note_span = p.note_span(nid);
            p.span = note_span;
            let note_ty = p.notes[nid.index()].resolved_type?;
            if let Err(err) = p.check_type_abilities(note_ty) {
                let _ = p.problem(err.code, p.span, err.message, err.reason, None);
                return None;
            }
            if !p.finish_operator(nid)
                || !p.finish_function_use(gid)
                || !p.finish_interpolation(nid)
            {
                return None;
            }

            let try_info = if let Some(try_res_term) = p.emitted[gid.index()].try_result {
                if !p.charge_arena(1, 64) {
                    return None;
                }
                let result_ty = p.finish_type(try_res_term, 0)?;
                let is_opt = p.named(try_res_term, "Option");
                let fid = p.emitted[gid.index()].failure?;
                let (wrapper, fail_generic) = {
                    let f = &p.failures[fid.index()];
                    (f.wrapper, f.generic)
                };
                let mut arguments = Vec::new();
                let mut abilities = Vec::new();
                if let Some(fg) = fail_generic {
                    let err_arg = p.signatures.types.get(result_ty).arguments[1];
                    arguments = p.type_arguments(fg, err_arg)?;
                    abilities = p.data_requirements(&arguments)?;
                }
                Some(CheckedTry {
                    result: result_ty,
                    failure: Some(ConstructorRef::Builtin(if is_opt {
                        "None"
                    } else {
                        "Error"
                    })),
                    wrapper,
                    arguments,
                    abilities,
                })
            } else {
                None
            };

            if let Some(generic_ty) = p.emitted[gid.index()].generic
                && !p.signatures.types.get(generic_ty).arguments.is_empty()
            {
                let inst_term = p.emitted[gid.index()].instantiated?;
                let actual_ty = p.finish_type(inst_term, 0)?;
                let args = p.type_arguments(generic_ty, actual_ty)?;
                let abils = p.data_requirements(&args)?;
                p.emitted[gid.index()].arguments = args;
                p.emitted[gid.index()].abilities = abils;
            }

            let mut match_info = None;
            let mut pattern_out = None;
            if let Some(mid) = p.emitted[gid.index()].match_emitted {
                let arm_data = p.matches_emitted[mid.index()].arms.clone();
                let scrutinee = operands[0];
                let scrutinee_ty = op_notes[0].and_then(|on| p.notes[on.index()].resolved_type)?;
                let mut checked_arms = Vec::with_capacity(arm_data.len());
                for arm in &arm_data {
                    if !p.charge_arena(arm.pattern_count, SIZEOF_PTR) {
                        return None;
                    }
                    let guard = arm.guard_operand.map(|idx| operands[idx]);
                    let body = operands[arm.body_operand];
                    let pats_nid = p.node(arm.syntax).children.first().copied().flatten()?;
                    let pat_children = p.node(pats_nid).children.clone();
                    let mut checked_pats = Vec::with_capacity(arm.pattern_count);
                    for ch in pat_children.iter().take(arm.pattern_count) {
                        let pnid = (*ch)?;
                        let cpid = p.checked_pattern(
                            pnid,
                            scrutinee_ty,
                            &notes_by_syntax,
                            &mut out_patterns,
                            0,
                        )?;
                        checked_pats.push(cpid);
                    }
                    checked_arms.push(CheckedArm {
                        syntax: arm.syntax,
                        patterns: checked_pats,
                        guard,
                        body,
                    });
                }
                let cid = CheckedMatchId(out_matches.len() as u32);
                out_matches.push(CheckedMatch {
                    scrutinee,
                    arms: checked_arms,
                });
                match_info = Some(cid);
            } else if let Some(snid) = p.notes[nid.index()].syntax
                && p.node(snid).kind == SyntaxKind::Is
            {
                let pat_nid = p.node(snid).children.get(1).copied().flatten()?;
                let scrutinee_ty = op_notes[0].and_then(|on| p.notes[on.index()].resolved_type)?;
                pattern_out = Some(p.checked_pattern(
                    pat_nid,
                    scrutinee_ty,
                    &notes_by_syntax,
                    &mut out_patterns,
                    0,
                )?);
            }
            if let Some(pat_term) = p.emitted[gid.index()].pattern_type {
                let pty = p.finish_type(pat_term, 0)?;
                let snid = p.notes[nid.index()].syntax?;
                let pat_nid = p.node(snid).children.first().copied().flatten()?;
                pattern_out = Some(p.checked_pattern(
                    pat_nid,
                    pty,
                    &notes_by_syntax,
                    &mut out_patterns,
                    0,
                )?);
            }

            let lambda_info = if let Some(lam_id) = p.emitted[gid.index()].lambda {
                let lam = p.lambdas[lam_id.index()].clone();
                if !p.charge_arena(lam.parameters.len(), SIZEOF_PTR)
                    || !p.charge_arena(lam.captures.len(), SIZEOF_PTR)
                {
                    return None;
                }
                let mut params_out = Vec::with_capacity(lam.parameters.len());
                for pn in &lam.parameters {
                    params_out.push(
                        pn.and_then(|n| p.notes[n.index()].binding_index.map(CheckedBindingId)),
                    );
                }
                let mut caps_out = Vec::with_capacity(lam.captures.len());
                for c in &lam.captures {
                    let bnote = p.locals[c.local.index()].binding?;
                    let bidx = p.notes[bnote.index()].binding_index?;
                    caps_out.push(CheckedBindingId(bidx));
                }
                let clid = CheckedLambdaId(out_lambdas.len() as u32);
                out_lambdas.push(CheckedLambda {
                    number: lam.number,
                    parameters: params_out,
                    body: operands[0],
                    captures: caps_out,
                });
                Some(clid)
            } else {
                None
            };

            let g = &p.emitted[gid.index()];
            let receiver = g.receiver_slot.map(|slot| operands[slot]);
            let binding = p.notes[nid.index()]
                .binding
                .and_then(|bn| p.notes[bn.index()].binding_index.map(CheckedBindingId));
            let syntax_nid = p.notes[nid.index()].syntax.unwrap_or(written);
            rows.push(CheckedExpression {
                syntax: syntax_nid,
                ty: note_ty,
                operands,
                operation: g.operation,
                binding,
                symbol: g.symbol,
                native: g.native.clone(),
                service: g.service,
                tool: g.tool,
                field: g.field,
                receiver,
                arguments: g.arguments.clone(),
                abilities: g.abilities.clone(),
                or_kind: g.or_kind,
                try_info,
                pattern: pattern_out,
                match_info,
                lambda: lambda_info,
                assignment: g.assignment,
                never: p.notes[nid.index()].never,
            });
        }

        if promise && let Some(conds) = conditions.as_deref_mut() {
            for (i, cond) in conds.iter_mut().enumerate() {
                let sn = spec_notes[i]?;
                cond.expression = CheckedExprId(p.notes[sn.index()].row_index?);
            }
        }
        if anchor && let Some(sp) = spec.as_deref_mut() {
            if !p.charge_arena(slots, 4) {
                return None;
            }
            let _ = p.finish_type(fn_term, 0)?;
            let inst_gid = instance.graph?;
            if !p.finish_function_use(inst_gid) {
                return None;
            }
            let mut vals = Vec::with_capacity(slots);
            for sn in spec_notes.iter().take(slots) {
                let snid = (*sn)?;
                vals.push(CheckedExprId(p.notes[snid.index()].row_index?));
            }
            sp.values = vals;
            sp.arguments = p.emitted[inst_gid.index()].arguments.clone();
            sp.abilities = p.emitted[inst_gid.index()].abilities.clone();
        }

        let root = if anchor {
            None
        } else {
            let vnote = v.note?;
            Some(CheckedExprId(p.notes[vnote.index()].row_index?))
        };

        let checked_fn = CheckedFunction {
            signature: fn_sig_id.unwrap_or(FuncSigId(0)),
            symbol: owner_sym
                .unwrap_or_else(|| SymRef::new(p.enclosing.module, crate::ids::SymIdx(0))),
            parameters: parameter_bindings,
            body: CheckedBody {
                items: rows,
                root,
                capture_free_parameters: p.capture_free,
                bindings,
                patterns: out_patterns,
                matches: out_matches,
                lambdas: out_lambdas,
            },
        };
        Some((inferred_body, Some(checked_fn)))
    })();

    if let Some((inferred_body, checked_fn)) = body_result {
        if let (Some(dst), Some(src)) = (checked_out, checked_fn) {
            *dst = src;
        }
        return Some(inferred_body);
    }

    if p.constants.as_ref().is_some_and(|c| c.pending.is_some()) {
        return None;
    }
    if !p.fatal && !finalized {
        p.failed = false;
        p.partial = true;
        let _ = p.finish_notes(None);
        if !p.fatal && !p.finish_patterns() {
            *halt = true;
            p.diags.sort();
            return None;
        }
        if !p.fatal {
            let _ = p.finish_captures();
        }
    }
    let err_code_num = p.error.as_ref().map_or(0, |e| e.code.number());
    p.emit_problem();
    *halt = err_code_num >= 1000
        || (p.diags.dropped() != 0 && (p.diags.len() as u64) < MO_LIMIT_DIAGNOSTICS);
    p.diags.sort();
    None
}

fn fn_enclosing_context(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &Signatures,
    sig_id: FuncSigId,
) -> Option<EnclosingContext> {
    let sig = &signatures.functions[sig_id.index()];
    let fn_ty = sig.ty?;
    let sym = interfaces.symbol(sig.function);
    let tree = &modules.module(sym.module).syntax.tree;
    let decl = sym.declaration;
    let params_nid = tree.node(decl).children.get(1).copied().flatten();
    let mut param_names = Vec::new();
    if let Some(pnid) = params_nid {
        for &ch in &tree.node(pnid).children {
            if let Some(pnode) = ch
                && let Some(nm) = tree.node(pnode).children.first().copied().flatten()
            {
                param_names.push(NameRef::Syntax(nm));
            }
        }
    }
    Some(EnclosingContext {
        module: sym.module,
        owner_symbol: Some(sig.function),
        func_sig_id: Some(sig_id),
        declaration: decl,
        owner_kind: sym.decl_kind,
        fn_type: fn_ty,
        requirements: sig.requirements,
        param_names,
    })
}

/// Infers a single function body without emitting checked IR (`mo_infer_function`).
///
/// Used by the staged body-inference tests (`tests/check/diagnostics/*.diag` BODIES phase).
#[must_use]
pub fn infer_function(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    function: FuncSigId,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> Option<InferredBody> {
    let enclosing = fn_enclosing_context(modules, interfaces, signatures, function)?;
    let mut abilities_arena = AbilityArena::new();
    let mut halt = false;
    infer_body_core(
        modules,
        interfaces,
        signatures,
        &mut abilities_arena,
        None,
        None,
        enclosing,
        diags,
        arena_remaining,
        None,
        None,
        None,
        &mut halt,
    )
}

/// Checks a top-level function body and builds its [`CheckedFunction`] (`mo_check_function`).
#[expect(
    clippy::too_many_arguments,
    reason = "body inference entry points mirror C infer.h and thread shared checker arenas"
)]
#[must_use]
pub fn check_function(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: Option<&mut ConstantContext>,
    services_view: Option<ProgramServicesView<'_>>,
    function: FuncSigId,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
    out: &mut CheckedFunction,
    halt: &mut bool,
) -> bool {
    let Some(enclosing) = fn_enclosing_context(modules, interfaces, signatures, function) else {
        return false;
    };
    infer_body_core(
        modules,
        interfaces,
        signatures,
        abilities_arena,
        constants,
        services_view,
        enclosing,
        diags,
        arena_remaining,
        Some(out),
        None,
        None,
        halt,
    )
    .is_some()
}

/// Checks a `test`, `example`, `promise`, or `anchor` spec body (`mo_check_spec_body`).
#[expect(
    clippy::too_many_arguments,
    reason = "body inference entry points mirror C infer.h and thread shared checker arenas"
)]
#[must_use]
pub(crate) fn check_spec_body(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: Option<&mut ConstantContext>,
    services_view: Option<ProgramServicesView<'_>>,
    enclosing: EnclosingContext,
    spec: &mut CheckedSpec,
    conditions: Option<&mut Vec<CheckedCondition>>,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
    halt: &mut bool,
) -> bool {
    let mut checked = CheckedFunction {
        signature: FuncSigId(0),
        symbol: SymRef::new(enclosing.module, crate::ids::SymIdx(0)),
        parameters: Vec::new(),
        body: CheckedBody::default(),
    };
    if infer_body_core(
        modules,
        interfaces,
        signatures,
        abilities_arena,
        constants,
        services_view,
        enclosing,
        diags,
        arena_remaining,
        Some(&mut checked),
        Some(spec),
        conditions,
        halt,
    )
    .is_none()
    {
        return false;
    }
    spec.body = checked.body;
    spec.parameters = checked.parameters;
    true
}

/// Checks a module constant initializer and infers its type (`mo_check_constant`).
#[expect(
    clippy::too_many_arguments,
    reason = "body inference entry points mirror C infer.h and thread shared checker arenas"
)]
#[must_use]
pub fn check_constant(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: &mut ConstantContext,
    constant_index: usize,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
    halt: &mut bool,
) -> bool {
    let sym_ref = constants.items[constant_index].0;
    let sym = interfaces.symbol(sym_ref);
    let unit = signatures.types.intern_builtin("Unit");
    let fn_ty = signatures.types.intern(
        SourceTypeKind::Function,
        "fn",
        None,
        TypeOwner::None,
        vec![unit],
    );
    let enclosing = EnclosingContext {
        module: sym.module,
        owner_symbol: Some(sym_ref),
        func_sig_id: None,
        declaration: sym.declaration,
        owner_kind: SyntaxKind::Const,
        fn_type: fn_ty,
        requirements: [0; 26],
        param_names: Vec::new(),
    };
    let mut checked = CheckedFunction {
        signature: FuncSigId(0),
        symbol: sym_ref,
        parameters: Vec::new(),
        body: CheckedBody::default(),
    };
    if infer_body_core(
        modules,
        interfaces,
        signatures,
        abilities_arena,
        Some(constants),
        None,
        enclosing,
        diags,
        arena_remaining,
        Some(&mut checked),
        None,
        None,
        halt,
    )
    .is_none()
    {
        return false;
    }
    let Some(root_id) = checked.body.root else {
        return false;
    };
    let value_ty = checked.body.expr(root_id).ty;
    if sym.name.ends_with('?') {
        let answer_ty = {
            let vt = signatures.types.get(value_ty);
            if vt.kind == SourceTypeKind::Function {
                *vt.arguments.last().unwrap_or(&value_ty)
            } else {
                value_ty
            }
        };
        if !signatures.types.get(answer_ty).is_builtin_named("Bool") {
            let diag = Diagnostic::new(
                ErrorCode::BoolName,
                sym.name_span,
                "a name ending in ? isn't a yes-or-no answer",
            )
            .with_primary_text("must be Bool or a function returning Bool")
            .with_reason(
                "A name ending in ? is a Bool, or a function that returns one (reference 2.3).",
            );
            let _ = diags.push(diag);
            *halt = diags.dropped() != 0 && (diags.len() as u64) < MO_LIMIT_DIAGNOSTICS;
            return false;
        }
    }
    constants.initializers[constant_index] = checked.body;
    constants.items[constant_index].1 = Some(value_ty);
    true
}

/// Checks a `service` / `actor` initial `state` value or `every` duration (`mo_check_service_value`).
#[expect(
    clippy::too_many_arguments,
    reason = "body inference entry points mirror C infer.h and thread shared checker arenas"
)]
#[must_use]
pub fn check_service_value(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: Option<&mut ConstantContext>,
    services_view: Option<ProgramServicesView<'_>>,
    module: ModuleId,
    clause: NodeId,
    expected: TypeId,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
    out: &mut CheckedBody,
) -> bool {
    let clause_kind = modules.module(module).syntax.tree.node(clause).kind;
    debug_assert!(matches!(
        clause_kind,
        SyntaxKind::ServiceState | SyntaxKind::ServiceEvery
    ));
    let fn_ty = signatures.types.intern(
        SourceTypeKind::Function,
        "fn",
        None,
        TypeOwner::None,
        vec![expected],
    );
    let enclosing = EnclosingContext {
        module,
        owner_symbol: None,
        func_sig_id: None,
        declaration: clause,
        owner_kind: clause_kind,
        fn_type: fn_ty,
        requirements: [0; 26],
        param_names: Vec::new(),
    };
    let mut checked = CheckedFunction {
        signature: FuncSigId(0),
        symbol: SymRef::new(module, crate::ids::SymIdx(0)),
        parameters: Vec::new(),
        body: CheckedBody::default(),
    };
    let mut halt = false;
    if infer_body_core(
        modules,
        interfaces,
        signatures,
        abilities_arena,
        constants,
        services_view,
        enclosing,
        diags,
        arena_remaining,
        Some(&mut checked),
        None,
        None,
        &mut halt,
    )
    .is_none()
    {
        return false;
    }
    *out = checked.body;
    true
}

/// Converts a finalized `ConstantContext` into owned `Vec<CheckedConstant>` in `order`.
pub(crate) fn collect_checked_constants(
    constants: &ConstantContext,
    order: &[usize],
) -> Vec<CheckedConstant> {
    order
        .iter()
        .map(|&idx| CheckedConstant {
            symbol: constants.items[idx].0,
            ty: constants.items[idx].1.unwrap_or(TypeId(0)),
            initializer: constants.initializers[idx].clone(),
        })
        .collect()
}

pub(crate) fn fn_enclosing_for_sig(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &Signatures,
    sig_id: FuncSigId,
) -> Option<EnclosingContext> {
    fn_enclosing_context(modules, interfaces, signatures, sig_id)
}
