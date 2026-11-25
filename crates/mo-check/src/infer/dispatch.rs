//! Deferred dot-dispatch, overload selection, tuple-position projection, `or`/`try`/arithmetic
//! deferred constraint solving, and anchor input matching (`src/check/infer-dispatch.c` parity).

use mo_def::ErrorCode;
use mo_syntax::{NodeId, TokenKind};

use crate::checked::CheckedOr;
use crate::types::SourceTypeKind;

use super::{
    DispatchId, Emitted, FailureId, InferState, InferValue, NUM_ARITH, NoteId, SIZEOF_PTR, TermId,
    TermShape,
};

impl<'a> InferState<'a> {
    fn dispatch_callee(
        &mut self,
        did: DispatchId,
        selected: InferValue,
        method: bool,
    ) -> Option<NoteId> {
        let last = self.notes_tail?;
        let target_node = if method {
            self.dispatches[did.index()].name
        } else {
            self.dispatches[did.index()].syntax?
        };
        if !self.record_syntax(target_node, selected) {
            return None;
        }
        let callee = self.notes_tail?;
        self.notes[last.index()].next = None;
        self.notes_tail = Some(last);
        let recv_note = self.dispatches[did.index()].receiver.note?;
        let after = self.notes[recv_note.index()].next;
        self.notes[callee.index()].next = after;
        self.notes[recv_note.index()].next = Some(callee);
        Some(callee)
    }

    fn finish_dot(&mut self, did: DispatchId) -> bool {
        let (receiver, name_nid, arguments, call_nid, count, disp_ty, disp_note, disp_graph) = {
            let d = &self.dispatches[did.index()];
            (
                d.receiver.clone(),
                d.name,
                d.arguments,
                d.call,
                d.count,
                d.ty,
                d.note,
                d.graph,
            )
        };
        let mut method = true;
        let selected = self.member(receiver.clone(), name_nid, &mut method);
        let Some(selected_ty) = selected.ty else {
            return false;
        };
        if method
            && arguments.is_some()
            && count == 0
            && self.receiver_only(selected_ty)
            && !self.allows_empty_dot_parens(&selected)
            && let Some(site) = call_nid
        {
            return self.empty_dot_call(site);
        }
        if !method && arguments.is_none() {
            if !self.unify(disp_ty, selected_ty, 0) {
                return false;
            }
            if let Some(nid) = disp_note {
                self.notes[nid.index()].field_index = selected.field_index;
            }
            if self.checked
                && let (Some(dg), Some(sg)) = (disp_graph, selected.graph)
            {
                let copied = self.emitted[sg.index()].clone();
                self.emitted[dg.index()] = copied;
            }
            return true;
        }

        let fn_root = self.root(selected_ty);
        if self.terms[fn_root.index()].shape.kind != SourceTypeKind::Function {
            let Some(fn_dummy) = self.make_type(TermShape {
                kind: SourceTypeKind::Function,
                name: "fn".to_string(),
                module: None,
                owner: crate::types::TypeOwner::None,
                arity: 0,
            }) else {
                return false;
            };
            return self.unify_as(fn_root, fn_dummy, Some(ErrorCode::NotFunction));
        }
        let offset = usize::from(method);
        let fn_arity = self.terms[fn_root.index()].shape.arity;
        if fn_arity != count + offset + 1 {
            return self.problem(
                ErrorCode::CallArguments,
                self.span,
                "a call's arguments don't match its parameters",
                Some("Pass every argument by position (reference 5.2)."),
                None,
            );
        }
        if method {
            let Some(recv_ty) = receiver.ty else {
                return false;
            };
            let Some(p0) = self.argument(fn_root, 0) else {
                return false;
            };
            if !self.unify_as(recv_ty, p0, Some(ErrorCode::ArgumentType)) {
                return false;
            }
        }
        for i in 0..count {
            if let Some(args_nid) = arguments
                && let Some(Some(arg_nid)) = self.node(args_nid).children.get(i)
            {
                self.span = self.node(*arg_nid).span;
            }
            let (in_ty, in_never) = {
                let inp = &self.dispatches[did.index()].inputs[i];
                (inp.ty, inp.never)
            };
            if !in_never {
                let Some(in_ty) = in_ty else {
                    return false;
                };
                let Some(param_ty) = self.argument(fn_root, i + offset) else {
                    return false;
                };
                if !self.unify_as(in_ty, param_ty, Some(ErrorCode::ArgumentType)) {
                    return false;
                }
            }
        }
        self.span = self.node(name_nid).span;
        let Some(ret_ty) = self.argument(fn_root, fn_arity - 1) else {
            return false;
        };
        if !self.unify(disp_ty, ret_ty, 0) {
            return false;
        }
        if !self.checked {
            return true;
        }
        let disp_syntax = self.dispatches[did.index()].syntax;
        if !method
            && let Some(nid) = disp_note
            && self.notes[nid.index()].syntax == disp_syntax
            && let Some(call_node) = call_nid
        {
            self.notes[nid.index()].syntax = Some(call_node);
        }
        let has_callee = !method || selected.symbol.is_some();
        let function_note = if has_callee {
            let Some(cn) = self.dispatch_callee(did, selected.clone(), method) else {
                return false;
            };
            Some(cn)
        } else {
            None
        };
        let recv_extra = usize::from(method && has_callee);
        let op_count = count + 1 + recv_extra;
        if !self.charge_arena(op_count, SIZEOF_PTR) {
            return false;
        }
        let mut operands = vec![None; op_count];
        operands[0] = if has_callee {
            function_note
        } else {
            receiver.note
        };
        if recv_extra != 0 {
            operands[1] = receiver.note;
        }
        for i in 0..count {
            operands[i + 1 + recv_extra] = self.dispatches[did.index()].inputs[i].note;
        }
        let sel_emitted = selected.graph.map(|sg| self.emitted[sg.index()].clone());
        if let Some(dg) = disp_graph {
            let mut new_emitted = Emitted {
                operands,
                symbol: selected.symbol,
                receiver_slot: if method { Some(recv_extra) } else { None },
                ..Emitted::default()
            };
            if method && let Some(se) = sel_emitted {
                new_emitted.native = se.native;
                new_emitted.service = se.service;
                new_emitted.tool = se.tool;
                new_emitted.function_type = se.function_type;
                new_emitted.requirements = se.requirements;
                new_emitted.parameters = se.parameters;
                new_emitted.json_decode = se.json_decode;
            }
            self.emitted[dg.index()] = new_emitted;
        }
        if let Some(nid) = disp_note {
            self.notes[nid.index()].callee = if method { function_note } else { None };
            self.notes[nid.index()].symbol = selected.symbol;
        }
        true
    }

    fn finish_position(&mut self, did: DispatchId) -> bool {
        let (recv_ty, recv_note, name_nid, disp_ty, disp_graph) = {
            let d = &self.dispatches[did.index()];
            (d.receiver.ty, d.receiver.note, d.name, d.ty, d.graph)
        };
        let Some(recv_ty) = recv_ty else {
            return false;
        };
        let r = self.root(recv_ty);
        let Some((part_ty, part_idx)) = self.tuple_part(r, name_nid) else {
            return false;
        };
        if !self.unify(disp_ty, part_ty, 0) {
            return false;
        }
        if !self.checked {
            return true;
        }
        if !self.charge_arena(1, SIZEOF_PTR) {
            return false;
        }
        if let Some(dg) = disp_graph {
            self.emitted[dg.index()] = Emitted {
                operands: vec![recv_note],
                field: Some(part_idx),
                ..Emitted::default()
            };
        }
        true
    }

    fn failure_container(&mut self, option: bool) -> Option<TermId> {
        let arity = if option { 1 } else { 2 };
        let out = self.native(if option { "Option" } else { "Result" }, arity)?;
        for i in 0..arity {
            let f = self.fresh(0)?;
            if let Some(args) = self.terms[out.index()].arguments.as_mut() {
                args[i] = f;
            }
        }
        Some(out)
    }

    pub(crate) fn solve_failure(&mut self, fid: FailureId, defaults: bool) -> bool {
        let (
            span,
            op,
            input_orig,
            value_ty,
            context_opt,
            fallback_opt,
            input_note,
            graph_opt,
            code_opt,
            never_flag,
        ) = {
            let f = &self.failures[fid.index()];
            (
                f.span,
                f.op,
                f.input,
                f.value,
                f.context,
                f.fallback,
                f.input_note,
                f.graph,
                f.code,
                f.never,
            )
        };
        self.span = span;
        let mut input = self.root(input_orig);

        if op != TokenKind::Eof {
            if self.terms[input.index()].variable
                && defaults
                && self.terms[input.index()].numeric != 0
                && self.finish_type(input, 0).is_none()
            {
                return false;
            }
            input = self.root(input_orig);
            let right_opt = fallback_opt.map(|fb| self.root(fb));
            let Some(fallback) = fallback_opt else {
                if self.terms[input.index()].variable {
                    return true;
                }
                if !self.named(input, "Duration") {
                    let numeric =
                        if self.terms[input.index()].shape.kind == SourceTypeKind::Parameter {
                            input
                        } else {
                            let Some(f) = self.fresh(NUM_ARITH) else {
                                return false;
                            };
                            f
                        };
                    let err_code = if self.named(input, "Byte") {
                        ErrorCode::ByteArithmetic
                    } else {
                        ErrorCode::NumberAbility
                    };
                    if !self.unify_as(input, numeric, Some(err_code)) {
                        return false;
                    }
                }
                if !self.unify_as(input, value_ty, code_opt) {
                    return false;
                }
                self.failures[fid.index()].solved = true;
                return true;
            };
            if self.named(input, "Time") && matches!(op, TokenKind::Plus | TokenKind::Minus) {
                let Some(duration) = self.native("Duration", 0) else {
                    return false;
                };
                if op == TokenKind::Plus {
                    if !self.unify(fallback, duration, 0)
                        || !self.unify_as(input, value_ty, code_opt)
                    {
                        return false;
                    }
                    self.failures[fid.index()].solved = true;
                    return true;
                }
                let Some(right) = right_opt else {
                    return false;
                };
                if self.terms[right.index()].variable && self.terms[right.index()].numeric == 0 {
                    return true;
                }
                let diff = self.named(right, "Time");
                if !self.unify(fallback, if diff { input } else { duration }, 0)
                    || !self.unify_as(if diff { duration } else { input }, value_ty, code_opt)
                {
                    return false;
                }
                self.failures[fid.index()].solved = true;
                return true;
            }
            if self.named(input, "Duration")
                && matches!(op, TokenKind::Plus | TokenKind::Minus | TokenKind::Star)
            {
                let wanted = if op == TokenKind::Star {
                    let Some(int_ty) = self.native("Int", 0) else {
                        return false;
                    };
                    int_ty
                } else {
                    input
                };
                if !self.unify(fallback, wanted, 0) || !self.unify_as(input, value_ty, code_opt) {
                    return false;
                }
                self.failures[fid.index()].solved = true;
                return true;
            }
            if self.terms[input.index()].variable {
                return true;
            }
            let numeric = if self.terms[input.index()].shape.kind == SourceTypeKind::Parameter {
                input
            } else {
                let Some(f) = self.fresh(NUM_ARITH) else {
                    return false;
                };
                f
            };
            let err_code = if self.named(input, "Byte") {
                ErrorCode::ByteArithmetic
            } else {
                ErrorCode::NumberAbility
            };
            if !self.unify_as(input, numeric, Some(err_code))
                || !self.unify(fallback, input, 0)
                || !self.unify_as(input, value_ty, code_opt)
            {
                return false;
            }
            self.failures[fid.index()].solved = true;
            return true;
        }

        if let Some(fallback) = fallback_opt {
            if self.terms[input.index()].variable
                && defaults
                && self.terms[input.index()].numeric != 0
                && self.finish_type(input, 0).is_none()
            {
                return false;
            }
            let r = self.root(input);
            if self.terms[r.index()].variable {
                return true;
            }
            let option = self.named(r, "Option");
            if !option && self.named(r, "Result") {
                return self.or_result(r);
            }
            let expected_val = if option {
                let Some(a0) = self.argument(r, 0) else {
                    return false;
                };
                a0
            } else {
                let Some(b) = self.native("Bool", 0) else {
                    return false;
                };
                b
            };
            if (!option && !self.unify_as(r, expected_val, Some(ErrorCode::ConditionType)))
                || !self.unify(value_ty, expected_val, 0)
                || (!never_flag && !self.unify(fallback, expected_val, 0))
            {
                return false;
            }
            if let Some(gid) = graph_opt {
                self.emitted[gid.index()].or_kind = if option {
                    CheckedOr::Option
                } else {
                    CheckedOr::Bool
                };
            }
            self.failures[fid.index()].solved = true;
            return true;
        }

        if self.terms[input.index()].variable && self.terms[input.index()].numeric != 0 {
            if !defaults {
                return true;
            }
            if self.finish_type(input, 0).is_none() {
                return false;
            }
            input = self.root(input);
        }
        let Some(context_orig) = context_opt else {
            return false;
        };
        let mut context = self.root(context_orig);
        let mut option = self.named(input, "Option");
        let mut result = self.named(input, "Result");
        if never_flag
            && (self.named(context, "Option") || self.named(context, "Result"))
            && !option
            && !result
        {
            let ctx_opt = self.named(context, "Option");
            let Some(container) = self.failure_container(ctx_opt) else {
                return false;
            };
            input = container;
            self.failures[fid.index()].input = input;
            if let Some(in_note) = input_note {
                self.notes[in_note.index()].ty = input;
            }
            option = ctx_opt;
            result = !option;
        }
        if self.terms[context.index()].variable && (option || result) {
            let Some(wanted) = self.failure_container(option) else {
                return false;
            };
            if !self.unify(context, wanted, 0) {
                return false;
            }
            context = self.root(context);
        }
        if self.terms[input.index()].variable
            && (self.named(context, "Option") || self.named(context, "Result"))
        {
            let ctx_opt = self.named(context, "Option");
            let Some(wanted) = self.failure_container(ctx_opt) else {
                return false;
            };
            if !self.unify(input, wanted, 0) {
                return false;
            }
            input = self.root(input);
            option = self.named(input, "Option");
            result = !option;
        }
        if self.terms[input.index()].variable || self.terms[context.index()].variable {
            return true;
        }
        if !option && !result {
            return self.problem(
                ErrorCode::TryInput,
                self.span,
                "try needs a Result or an Option",
                Some("try takes the value out of an Ok or Some (reference 8.3)."),
                None,
            );
        }
        if !self.named(context, if option { "Option" } else { "Result" }) {
            return self.problem(
                ErrorCode::TryResult,
                self.span,
                "try needs a function returning the same failure kind",
                Some(
                    "try works on a Result in a function returning Result, or on an Option in a \
                     function returning Option; otherwise handle the failure with case \
                     (reference 8.3).",
                ),
                None,
            );
        }
        let Some(val) = self.argument(input, 0) else {
            return false;
        };
        if !self.unify(value_ty, val, 0) {
            return false;
        }
        self.failures[fid.index()].solved = true;
        true
    }

    pub(crate) fn finish_dispatches(&mut self) -> bool {
        let mut defaults = false;
        loop {
            let mut progress = false;
            for i in 0..self.dispatches.len() {
                if !self.work() {
                    return false;
                }
                if self.dispatches[i].solved {
                    continue;
                }
                let did = DispatchId(i as u32);
                let (name_nid, group, position, disp_ty, disp_note, disp_graph) = {
                    let d = &self.dispatches[i];
                    (d.name, d.group, d.position, d.ty, d.note, d.graph)
                };
                self.span = self.node(name_nid).span;

                let Some((group_sym, group_count)) = group else {
                    let Some(recv_ty) = self.dispatches[i].receiver.ty else {
                        return false;
                    };
                    let mut r = self.root(recv_ty);
                    if self.terms[r.index()].variable
                        && defaults
                        && self.terms[r.index()].numeric != 0
                        && self.finish_type(r, 0).is_none()
                    {
                        return false;
                    }
                    r = self.root(r);
                    if self.terms[r.index()].variable {
                        continue;
                    }
                    let ok = if position {
                        self.finish_position(did)
                    } else {
                        self.finish_dot(did)
                    };
                    if !ok {
                        return false;
                    }
                    self.dispatches[i].solved = true;
                    progress = true;
                    continue;
                };

                let t = self.root(disp_ty);
                if self.terms[t.index()].variable {
                    continue;
                }
                let (tkind, tarity) = {
                    let term = &self.terms[t.index()];
                    (term.shape.kind, term.shape.arity)
                };
                if tkind != SourceTypeKind::Function || tarity < 2 {
                    return self.problem(
                        ErrorCode::CallArguments,
                        self.span,
                        "an overloaded function needs a first argument",
                        Some("Overloads differ in their first parameter's type (reference 5.1)."),
                        None,
                    );
                }
                let is_from_json = self.interfaces.symbol(group_sym).name == "from_json";
                let receiver = if is_from_json {
                    let Some(ret) = self.argument(t, tarity - 1) else {
                        return false;
                    };
                    let rr = self.root(ret);
                    if self.terms[rr.index()].variable {
                        continue;
                    }
                    if self.named(rr, "Result") {
                        let Some(a0) = self.argument(rr, 0) else {
                            return false;
                        };
                        a0
                    } else {
                        rr
                    }
                } else {
                    let Some(a0) = self.argument(t, 0) else {
                        return false;
                    };
                    a0
                };
                let recv_root = self.root(receiver);
                if self.terms[recv_root.index()].variable
                    && self.terms[recv_root.index()].numeric == 0
                {
                    continue;
                }
                let Some(symbol) = self.select_overload(group_sym, group_count, receiver) else {
                    if !self.failed {
                        return self.problem(
                            ErrorCode::ArgumentType,
                            self.span,
                            "no overload takes this first argument's type",
                            Some("Overloads take local record or choice types (reference 5.1)."),
                            None,
                        );
                    }
                    return false;
                };
                let selected = self.declared_function(symbol);
                let Some(sel_ty) = selected.ty else {
                    return false;
                };
                if !self.unify_as(sel_ty, t, Some(ErrorCode::ArgumentType)) {
                    return false;
                }
                if let Some(nid) = disp_note {
                    self.notes[nid.index()].symbol = Some(symbol);
                }
                if self.checked
                    && let (Some(dg), Some(sg)) = (disp_graph, selected.graph)
                {
                    let mut copied = self.emitted[sg.index()].clone();
                    copied.symbol = Some(symbol);
                    self.emitted[dg.index()] = copied;
                }
                self.dispatches[i].solved = true;
                progress = true;
            }

            for i in 0..self.failures.len() {
                if !self.work() {
                    return false;
                }
                if !self.failures[i].solved {
                    if !self.solve_failure(FailureId(i as u32), defaults) {
                        return false;
                    }
                    if self.failures[i].solved {
                        progress = true;
                    }
                }
            }

            if !progress && !defaults {
                defaults = true;
                progress = true;
            } else if !progress {
                for i in 0..self.failures.len() {
                    if self.failures[i].solved || self.failures[i].op == TokenKind::Eof {
                        continue;
                    }
                    let (span, op, inp, val_ty, fb_opt, code_opt) = {
                        let f = &self.failures[i];
                        (f.span, f.op, f.input, f.value, f.fallback, f.code)
                    };
                    self.span = span;
                    if op == TokenKind::Minus && self.named(inp, "Time") {
                        let vr = self.root(val_ty);
                        if !self.named(vr, "Duration") && !self.named(vr, "Time") {
                            continue;
                        }
                        let Some(duration) = self.native("Duration", 0) else {
                            return false;
                        };
                        let diff = self.named(vr, "Duration");
                        let Some(fb) = fb_opt else {
                            return false;
                        };
                        if !self.unify(fb, if diff { inp } else { duration }, 0)
                            || !self.unify_as(if diff { duration } else { inp }, val_ty, code_opt)
                        {
                            return false;
                        }
                        self.failures[i].solved = true;
                        progress = true;
                        continue;
                    }
                    let ir = self.root(inp);
                    if !self.terms[ir.index()].variable {
                        continue;
                    }
                    let Some(numeric) = self.fresh(NUM_ARITH) else {
                        return false;
                    };
                    if !self.unify_as(inp, numeric, Some(ErrorCode::NumberAbility))
                        || (fb_opt.is_some_and(|fb| !self.unify(fb, inp, 0)))
                        || !self.unify_as(inp, val_ty, code_opt)
                    {
                        return false;
                    }
                    self.failures[i].solved = true;
                    progress = true;
                }
            }
            if !progress {
                break;
            }
        }

        for i in 0..self.dispatches.len() {
            if !self.dispatches[i].solved {
                let name_nid = self.dispatches[i].name;
                let span = self.node(name_nid).span;
                return if self.dispatches[i].position {
                    self.problem(
                        ErrorCode::CannotInfer,
                        span,
                        "no use decides this expression's type",
                        Some(
                            "A function's signature gives its types; inside a body types are \
                             inferred from all their uses (reference 5.1).",
                        ),
                        None,
                    )
                } else {
                    self.problem(
                        ErrorCode::CannotInfer,
                        span,
                        "no use decides this dispatch's receiver type",
                        Some(
                            "Give the value a type, or write the call another way \
                             (reference 5.4).",
                        ),
                        None,
                    )
                };
            }
        }
        for i in 0..self.failures.len() {
            if !self.failures[i].solved {
                let (span, has_op) = {
                    let f = &self.failures[i];
                    (f.span, f.op != TokenKind::Eof)
                };
                return self.problem(
                    ErrorCode::CannotInfer,
                    span,
                    if has_op {
                        "no use decides this expression's type"
                    } else {
                        "no use decides this failure expression's type"
                    },
                    Some(if has_op {
                        "A function's signature gives its types; inside a body types are \
                         inferred from all their uses (reference 5.1)."
                    } else {
                        "or needs Bool or Option, and try needs Option or Result in the \
                         enclosing function (reference 8)."
                    }),
                    None,
                );
            }
        }
        true
    }

    pub(crate) fn anchor_inputs(&mut self, anchor: NodeId, slots: usize) -> Option<Vec<NodeId>> {
        let values_nid = self.node(anchor).children.first().copied().flatten()?;
        let values_children = self.node(values_nid).children.clone();
        let param_count = self.enclosing.param_names.len();
        let mut inputs: Vec<Option<NodeId>> = vec![None; slots];
        let tree = self.tree();

        for child in values_children {
            let item = child?;
            let name_nid = self.node(item).children.first().copied().flatten()?;
            let name_bytes = self.node_text(name_nid);
            let mut at = param_count;
            if slots == param_count || name_bytes != b"result" {
                at = 0;
                while at < param_count {
                    if !self.work() {
                        return None;
                    }
                    if self.enclosing.param_names[at].bytes(tree) == name_bytes {
                        break;
                    }
                    at += 1;
                }
                if at == param_count {
                    let span = self.node(name_nid).span;
                    let _ = self.problem(
                        ErrorCode::AnchorArguments,
                        span,
                        "an anchor names a parameter or result",
                        Some("Anchors give every parameter and result a value (reference 12.2)."),
                        None,
                    );
                    return None;
                }
            }
            if let Some(prev_item) = inputs[at] {
                let span = self.node(name_nid).span;
                let prev_span = self
                    .node(prev_item)
                    .children
                    .first()
                    .copied()
                    .flatten()
                    .map(|n| self.node(n).span);
                let _ = self.problem(
                    ErrorCode::AnchorArguments,
                    span,
                    "an anchor gives a name more than one value",
                    Some("Anchors give every parameter and result one value (reference 12.2)."),
                    prev_span,
                );
                return None;
            }
            inputs[at] = Some(item);
        }

        let mut out = Vec::with_capacity(slots);
        for slot in inputs {
            let Some(item) = slot else {
                let span = self.node(anchor).span;
                let _ = self.problem(
                    ErrorCode::AnchorArguments,
                    span,
                    "an anchor gives every parameter and result a value",
                    Some("No parameter or result can be omitted (reference 12.2)."),
                    None,
                );
                return None;
            };
            out.push(item);
        }
        Some(out)
    }
}
