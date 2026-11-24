//! Expression tree walker (`src/check/infer.c` parity).

use mo_base::{Edit, Fix, Span};
use mo_def::ErrorCode;
use mo_def::limits::MO_LIMIT_SOURCE_INFERENCE_DEPTH;
use mo_syntax::{NodeId, SyntaxKind, TokenKind};

use crate::types::{SourceTypeKind, builtin_case_by_name};

use super::{
    CaptureLoop, CaptureLoopId, EmittedLambda, EmittedLambdaId, FailureCheck, FailureId,
    InferState, InferValue, NUM_ALL, NUM_ARITH, NUM_DECIMAL, NUM_FLOAT, NameRef, SIZEOF_FAILURE,
    SIZEOF_PTR, TermId, TermShape,
};

impl<'a> InferState<'a> {
    fn infer_node(&mut self, node: NodeId, expected: Option<TermId>) -> InferValue {
        let kind = self.node(node).kind;
        match kind {
            SyntaxKind::Sequence => self.infer_sequence(node, expected),
            SyntaxKind::ExprStmt | SyntaxKind::Group => {
                let Some(inner) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                self.infer_as(inner, expected, self.node_expectation)
            }
            SyntaxKind::TextInterpolated => {
                let children = self.node(node).children.clone();
                let count = children.len() / 2;
                let Some(text_ty) = self.native("Text", 0) else {
                    return InferValue::none();
                };
                let mut out = self.graph(InferValue::with_type(text_ty), count);
                if out.ty.is_none() {
                    return out;
                }
                for i in 0..count {
                    let Some(hole_nid) = children.get(2 * i + 1).copied().flatten() else {
                        return InferValue::none();
                    };
                    let hole = self.infer_expression(hole_nid, None);
                    if hole.ty.is_none() {
                        return InferValue::none();
                    }
                    if let Some(gid) = out.graph {
                        self.emitted[gid.index()].operands[i] = hole.note;
                    }
                    out.never = out.never || hole.never;
                }
                out
            }
            SyntaxKind::Name | SyntaxKind::CapName | SyntaxKind::Qualified => {
                self.identifier(node, None)
            }
            SyntaxKind::FieldAccess => {
                let Some(lhs) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(rhs) = self.node(node).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                if self.node(lhs).kind == SyntaxKind::CapName
                    && (!self.case_receiver(lhs) || self.type_reading(lhs, rhs, None))
                {
                    return self.identifier(rhs, Some(lhs));
                }
                let recv = self.infer_expression(lhs, None);
                let mut called = false;
                self.field_value(recv, rhs, node, None, &mut called)
            }
            SyntaxKind::Int => match self.fresh(NUM_ALL) {
                Some(t) => InferValue::with_type(t),
                None => InferValue::none(),
            },
            SyntaxKind::Decimal => match self.fresh(NUM_DECIMAL | NUM_FLOAT) {
                Some(t) => InferValue::with_type(t),
                None => InferValue::none(),
            },
            SyntaxKind::Char => match self.native("Char", 0) {
                Some(t) => InferValue::with_type(t),
                None => InferValue::none(),
            },
            SyntaxKind::Text => match self.native("Text", 0) {
                Some(t) => InferValue::with_type(t),
                None => InferValue::none(),
            },
            SyntaxKind::True | SyntaxKind::False => match self.native("Bool", 0) {
                Some(t) => InferValue::with_type(t),
                None => InferValue::none(),
            },
            SyntaxKind::Bind | SyntaxKind::Var | SyntaxKind::AddAssign | SyntaxKind::SubAssign => {
                self.infer_bind_or_assign(node, kind)
            }
            SyntaxKind::Binary | SyntaxKind::Range => {
                let tok = self.node(node).token;
                let logical = matches!(tok, TokenKind::And | TokenKind::Or | TokenKind::Implies);
                let mark = self.active_locals_head;
                let Some(lhs) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(rhs) = self.node(node).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                let left = if logical && tok != TokenKind::Or {
                    self.infer_condition(lhs, true)
                } else {
                    self.infer_expression(lhs, None)
                };
                let result = self.infer_binary(tok, left, rhs, expected);
                if result.ty.is_none() || (logical && tok != TokenKind::And && !self.leave(mark)) {
                    return InferValue::none();
                }
                result
            }
            SyntaxKind::BinaryChain => {
                let children = self.node(node).children.clone();
                let Some(first) = children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(op_node) = children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                let op = self.node(op_node).token;
                let mut result = if op == TokenKind::And {
                    self.infer_condition(first, true)
                } else {
                    self.infer_expression(first, None)
                };
                let mut i = 1usize;
                while i < children.len() && result.ty.is_some() {
                    let Some(step_op_nid) = children[i] else {
                        return InferValue::none();
                    };
                    let Some(step_rhs_nid) = children.get(i + 1).copied().flatten() else {
                        return InferValue::none();
                    };
                    let step_op = self.node(step_op_nid).token;
                    let step_expected = if i + 2 == children.len() {
                        expected
                    } else {
                        None
                    };
                    result = self.infer_binary(step_op, result, step_rhs_nid, step_expected);
                    if self.checked && result.ty.is_some() && i + 2 < children.len() {
                        if !self.record_syntax(step_op_nid, result.clone()) {
                            return InferValue::none();
                        }
                        result.note = self.notes_tail;
                    }
                    i += 2;
                }
                result
            }
            SyntaxKind::Call => {
                let Some(callee) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(args) = self.node(node).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                if let Some(builtin) = builtin_case_by_name(self.node_text(callee), None) {
                    return self.builtin_construct(builtin, Some(args), expected);
                }
                let callee_kind = self.node(callee).kind;
                if matches!(callee_kind, SyntaxKind::CapName | SyntaxKind::Qualified) {
                    let def = self.definition(callee, None);
                    if self.failed {
                        return InferValue::none();
                    }
                    if let Some((sym_ref, _)) = def
                        && let Some(data_id) = self.data_type(sym_ref)
                    {
                        return self.construct(data_id, args, expected);
                    }
                }
                let callee_val = self.infer_expression(callee, None);
                self.infer_call(callee_val, Some(args), None)
            }
            SyntaxKind::TypeArgs => {
                let Some(callee_nid) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(args_nid) = self.node(node).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                let callee_kind = self.node(callee_nid).kind;
                let callee = if matches!(callee_kind, SyntaxKind::Name | SyntaxKind::Qualified) {
                    self.identifier(callee_nid, None)
                } else {
                    self.infer_expression(callee_nid, None)
                };
                let mut applied = self.type_application(callee, args_nid);
                if self.checked
                    && applied.ty.is_some()
                    && applied.note.is_some()
                    && let Some(prev_gid) = applied.graph
                {
                    let prev = self.emitted[prev_gid.index()].clone();
                    applied.note = None;
                    applied = self.graph(applied, 0);
                    if let Some(new_gid) = applied.graph {
                        let g = &mut self.emitted[new_gid.index()];
                        g.symbol = prev.symbol;
                        g.native = prev.native;
                        g.service = prev.service;
                        g.tool = prev.tool;
                        g.function_type = prev.function_type;
                        g.requirements = prev.requirements;
                        g.parameters = prev.parameters;
                        g.json_decode = prev.json_decode;
                    }
                }
                applied
            }
            SyntaxKind::Position => {
                let Some(recv_nid) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(idx_nid) = self.node(node).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                let recv = self.infer_expression(recv_nid, None);
                self.infer_position(recv, idx_nid)
            }
            SyntaxKind::Postfix => self.infer_postfix(node, expected),
            SyntaxKind::Lambda => self.infer_lambda(node, expected),
            SyntaxKind::If | SyntaxKind::LineIf => self.infer_conditional(node, expected),
            SyntaxKind::Case => self.infer_match(node, expected),
            SyntaxKind::Is => {
                let Some(lhs) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let Some(pat) = self.node(node).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                let input = self.infer_expression(lhs, None);
                let Some(in_ty) = input.ty else {
                    return InferValue::none();
                };
                let mut normalized = None;
                if !self.infer_pattern(pat, in_ty, 0, 1, &mut normalized) {
                    return InferValue::none();
                }
                let Some(bool_ty) = self.native("Bool", 0) else {
                    return InferValue::none();
                };
                let out = self.graph(
                    InferValue {
                        ty: Some(bool_ty),
                        never: input.never,
                        ..InferValue::default()
                    },
                    1,
                );
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[0] = input.note;
                }
                out
            }
            SyntaxKind::Return => {
                let mark = self.active_locals_head;
                let value_nid = self.node(node).children.first().copied().flatten();
                let guard_nid = self.node(node).children.get(1).copied().flatten();
                let guard = if let Some(gnid) = guard_nid {
                    let g = self.infer_condition(gnid, true);
                    if g.ty.is_none() {
                        return g;
                    }
                    g
                } else {
                    InferValue::none()
                };
                let value = if let Some(vnid) = value_nid {
                    self.infer_as(vnid, Some(self.result), Some(ErrorCode::ResultType))
                } else {
                    match self.native("Unit", 0) {
                        Some(u) => InferValue::with_type(u),
                        None => InferValue::none(),
                    }
                };
                let Some(val_ty) = value.ty else {
                    return InferValue::none();
                };
                if !self.unify_as(val_ty, self.result, Some(ErrorCode::ResultType))
                    || !self.leave(mark)
                {
                    return InferValue::none();
                }
                let count = usize::from(value_nid.is_some()) + usize::from(guard_nid.is_some());
                let Some(unit_ty) = self.native("Unit", 0) else {
                    return InferValue::none();
                };
                let out = self.graph(
                    InferValue {
                        ty: Some(unit_ty),
                        never: guard_nid.is_none(),
                        ..InferValue::default()
                    },
                    count,
                );
                if let Some(gid) = out.graph {
                    if let Some(gnote) = guard.note {
                        self.emitted[gid.index()].operands[0] = Some(gnote);
                    }
                    if let Some(vnote) = value.note {
                        self.emitted[gid.index()].operands[count - 1] = Some(vnote);
                    }
                }
                out
            }
            SyntaxKind::Stop => {
                let Some(text_ty) = self.native("Text", 0) else {
                    return InferValue::none();
                };
                let Some(reason_nid) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let reason = self.infer_expression(reason_nid, Some(text_ty));
                if reason.ty.is_none() {
                    return InferValue::none();
                }
                let res_ty = match expected {
                    Some(e) => e,
                    None => {
                        let Some(u) = self.native("Unit", 0) else {
                            return InferValue::none();
                        };
                        u
                    }
                };
                let out = self.graph(
                    InferValue {
                        ty: Some(res_ty),
                        never: true,
                        ..InferValue::default()
                    },
                    1,
                );
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[0] = reason.note;
                }
                out
            }
            SyntaxKind::Assert => {
                let Some(cond_nid) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let test = self.infer_condition(cond_nid, true);
                if test.ty.is_none() {
                    return InferValue::none();
                }
                let Some(unit_ty) = self.native("Unit", 0) else {
                    return InferValue::none();
                };
                let out = self.graph(InferValue::with_type(unit_ty), 1);
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[0] = test.note;
                }
                out
            }
            SyntaxKind::For | SyntaxKind::While => self.infer_loop(node, kind),
            SyntaxKind::Break | SyntaxKind::Continue => {
                if self.loops == 0 {
                    let span = self.node(node).span;
                    let _ = self.problem(
                        ErrorCode::ExitOutsideLoop,
                        span,
                        "break or continue is outside a loop",
                        Some(
                            "Break leaves the loop and continue starts its next round \
                             (reference 7.5).",
                        ),
                        None,
                    );
                    return InferValue::none();
                }
                let guard_nid = self.node(node).children.first().copied().flatten();
                let guard = if let Some(gnid) = guard_nid {
                    let Some(cond_ty) = self.native("Bool", 0) else {
                        return InferValue::none();
                    };
                    let g = self.infer_as(gnid, Some(cond_ty), Some(ErrorCode::ConditionType));
                    if g.ty.is_none() {
                        return InferValue::none();
                    }
                    g
                } else {
                    InferValue::none()
                };
                let Some(unit_ty) = self.native("Unit", 0) else {
                    return InferValue::none();
                };
                let out = self.graph(
                    InferValue {
                        ty: Some(unit_ty),
                        never: guard_nid.is_none(),
                        ..InferValue::default()
                    },
                    usize::from(guard.ty.is_some()),
                );
                if let Some(gid) = out.graph
                    && guard.ty.is_some()
                {
                    self.emitted[gid.index()].operands[0] = guard.note;
                }
                out
            }
            SyntaxKind::List | SyntaxKind::Tuple | SyntaxKind::Map => {
                self.infer_collection(node, kind, expected)
            }
            SyntaxKind::Try => {
                let Some(inner) = self.node(node).children.first().copied().flatten() else {
                    return InferValue::none();
                };
                let input = self.infer_expression(inner, None);
                let sp = self.node(node).span;
                self.span = Span::new(sp.file, sp.start, sp.start.saturating_add(3));
                self.infer_propagate(input, expected)
            }
            SyntaxKind::Negate | SyntaxKind::Not | SyntaxKind::Prefix => {
                self.infer_prefix(node, kind, expected)
            }
            SyntaxKind::File
            | SyntaxKind::Module
            | SyntaxKind::Expose
            | SyntaxKind::ExposeAll
            | SyntaxKind::Use
            | SyntaxKind::Fn
            | SyntaxKind::Param
            | SyntaxKind::Where
            | SyntaxKind::Struct
            | SyntaxKind::Field
            | SyntaxKind::Enum
            | SyntaxKind::CaseDecl
            | SyntaxKind::Test
            | SyntaxKind::Const
            | SyntaxKind::Service
            | SyntaxKind::ServiceState
            | SyntaxKind::ServiceTools
            | SyntaxKind::ServiceEvery
            | SyntaxKind::ServiceToken
            | SyntaxKind::Actor
            | SyntaxKind::ActorCalls
            | SyntaxKind::Api
            | SyntaxKind::ApiPort
            | SyntaxKind::ApiCors
            | SyntaxKind::ApiSqlite
            | SyntaxKind::ApiActor
            | SyntaxKind::ApiStatic
            | SyntaxKind::ApiJob
            | SyntaxKind::ApiScope
            | SyntaxKind::ApiAuth
            | SyntaxKind::ApiRoute
            | SyntaxKind::ApiStatus
            | SyntaxKind::Promise
            | SyntaxKind::Anchor
            | SyntaxKind::Examples
            | SyntaxKind::Type
            | SyntaxKind::Binder
            | SyntaxKind::Else
            | SyntaxKind::Arm
            | SyntaxKind::NamedArg
            | SyntaxKind::Underscore
            | SyntaxKind::Rest
            | SyntaxKind::MapEntry
            | SyntaxKind::Token
            | SyntaxKind::IfPart => {
                let _ = self.unfinished();
                InferValue::none()
            }
        }
    }

    fn infer_bind_or_assign(&mut self, node: NodeId, kind: SyntaxKind) -> InferValue {
        let Some(name_nid) = self.node(node).children.first().copied().flatten() else {
            return InferValue::none();
        };
        let Some(rhs_nid) = self.node(node).children.get(1).copied().flatten() else {
            return InferValue::none();
        };
        let name_kind = self.node(name_nid).kind;
        if matches!(name_kind, SyntaxKind::FieldAccess | SyntaxKind::Postfix) {
            let mut base = name_nid;
            let mut fields = true;
            while matches!(
                self.node(base).kind,
                SyntaxKind::FieldAccess | SyntaxKind::Postfix
            ) {
                if self.node(base).kind == SyntaxKind::Postfix {
                    for ch in self.node(base).children.iter().skip(1) {
                        fields = fields
                            && ch.is_some_and(|cid| self.node(cid).kind == SyntaxKind::FieldAccess);
                    }
                }
                let Some(next_base) = self.node(base).children.first().copied().flatten() else {
                    break;
                };
                base = next_base;
            }
            let variable_id = if self.node(base).kind == SyntaxKind::Name {
                let key = self.node_text(base).to_vec();
                self.names.get(&key).and_then(|s| s.active)
            } else {
                None
            };
            let is_var = variable_id.is_some_and(|lid| self.locals[lid.index()].variable);
            if !fields || !is_var {
                let span = self.node(name_nid).span;
                let prev = variable_id.map(|lid| self.locals[lid.index()].name.span(self.tree()));
                let _ = self.problem(
                    ErrorCode::NotVariable,
                    span,
                    "a field is assigned without an assignable var",
                    Some(
                        "A var or its fields can be assigned within the function that declares it \
                         (reference 6.2).",
                    ),
                    prev,
                );
                return InferValue::none();
            }
            let Some(var_id) = variable_id else {
                return InferValue::none();
            };
            if self.locals[var_id.index()].function != self.lambda_depth {
                debug_assert!(self.lambda_depth != 0);
                let span = self.node(name_nid).span;
                let prev = self.locals[var_id.index()].name.span(self.tree());
                let _ = self.problem(
                    ErrorCode::LambdaWrite,
                    span,
                    "an anonymous function can't assign an outer var",
                    Some(
                        "An anonymous function can read names in scope and assign its own local \
                         var, but can't assign a var from an enclosing function (reference 5.6).",
                    ),
                    Some(prev),
                );
                return InferValue::none();
            }
            if !self.capture_write(var_id, node, name_nid) {
                return InferValue::none();
            }
            self.locals[var_id.index()].version += 1;
            let var_name = self.locals[var_id.index()].name;
            let var_binding = self.locals[var_id.index()].binding;
            let target = self.infer_expression(name_nid, None);
            let Some(target_ty) = target.ty else {
                return InferValue::none();
            };
            if let Some(tail) = self.notes_tail {
                self.notes[tail.index()].binding_name = Some(var_name);
            }
            let mut rhs = if kind == SyntaxKind::Bind {
                self.infer_expression(rhs_nid, Some(target_ty))
            } else {
                let op = if kind == SyntaxKind::AddAssign {
                    TokenKind::Plus
                } else {
                    TokenKind::Minus
                };
                self.infer_binary(op, target.clone(), rhs_nid, Some(target_ty))
            };
            if rhs.ty.is_none() {
                return InferValue::none();
            }
            if self.checked && kind != SyntaxKind::Bind {
                if !self.record_syntax(node, rhs.clone()) {
                    return InferValue::none();
                }
                rhs.note = self.notes_tail;
            }
            let Some(unit_ty) = self.native("Unit", 0) else {
                return InferValue::none();
            };
            let out = self.graph(
                InferValue {
                    ty: Some(unit_ty),
                    binding: Some(var_name),
                    bound: var_binding,
                    field_index: target.field_index,
                    never: rhs.never,
                    ..InferValue::default()
                },
                2,
            );
            if let Some(gid) = out.graph {
                let g = &mut self.emitted[gid.index()];
                g.assignment = true;
                g.operands[0] = target.note;
                g.operands[1] = rhs.note;
            }
            return out;
        }

        if !matches!(
            name_kind,
            SyntaxKind::Name | SyntaxKind::Tuple | SyntaxKind::Underscore
        ) {
            let _ = self.unfinished();
            return InferValue::none();
        }
        let local_id = if name_kind == SyntaxKind::Name {
            let key = self.node_text(name_nid).to_vec();
            self.names.get(&key).and_then(|s| s.active)
        } else {
            None
        };
        let assignment = local_id
            .is_some_and(|lid| self.locals[lid.index()].variable && kind != SyntaxKind::Var);
        if let Some(lid) = local_id
            && !assignment
        {
            let (l_statement, l_fn, l_ty, l_span) = {
                let l = &self.locals[lid.index()];
                (l.statement, l.function, l.ty, l.name.span(self.tree()))
            };
            let statement = l_statement && l_fn == self.lambda_depth && kind != SyntaxKind::Var;
            let mut to_var = statement;
            let mut differs = false;
            if statement && kind == SyntaxKind::Bind {
                let rhs = self.infer_expression(rhs_nid, None);
                if self.fatal {
                    return InferValue::none();
                }
                to_var = rhs.ty.is_some_and(|rty| self.same_type(rty, l_ty, 0));
                if self.fatal {
                    return InferValue::none();
                }
                differs = rhs.ty.is_some() && !to_var;
            }
            let name_span = self.node(name_nid).span;
            let reason = if to_var {
                "While a name is visible it can't be bound again; to assign it again, declare it \
                 with var where it is first bound (reference 6.2)."
            } else if differs {
                "While a name is visible it can't be bound again; this value has another type, so \
                 bind it to a new name instead (reference 6.1)."
            } else {
                "While a local name or parameter is visible it can't be bound again \
                 (reference 6.1)."
            };
            let _ = self.problem(
                ErrorCode::BindRepeated,
                name_span,
                "a visible name is bound again",
                Some(reason),
                Some(l_span),
            );
            if to_var
                && let Some(err) = self.error.as_mut()
                && err.code == ErrorCode::BindRepeated
                && err.primary.span.start == name_span.start
            {
                let edit = Edit::new(Span::new(l_span.file, l_span.start, l_span.start), "var ");
                err.fixes
                    .push(Fix::new(Some("declare it with var".into()), vec![edit]));
            }
            return InferValue::none();
        }
        if matches!(kind, SyntaxKind::AddAssign | SyntaxKind::SubAssign) && !assignment {
            let span = self.node(name_nid).span;
            let _ = self.problem(
                ErrorCode::NotVariable,
                span,
                "a name that isn't var is assigned",
                Some("+= and -= assign a var or its fields (reference 6.2)."),
                None,
            );
            return InferValue::none();
        }
        if assignment && let Some(lid) = local_id {
            if self.locals[lid.index()].function != self.lambda_depth {
                debug_assert!(self.lambda_depth != 0);
                let span = self.node(name_nid).span;
                let prev = self.locals[lid.index()].name.span(self.tree());
                let _ = self.problem(
                    ErrorCode::LambdaWrite,
                    span,
                    "an anonymous function can't assign an outer var",
                    Some(
                        "An anonymous function can read names in scope and assign its own local \
                         var, but can't assign a var from an enclosing function (reference 5.6).",
                    ),
                    Some(prev),
                );
                return InferValue::none();
            }
            if !self.capture_write(lid, node, name_nid) {
                return InferValue::none();
            }
            self.locals[lid.index()].version += 1;
        }
        let mut target = InferValue::none();
        if assignment && let Some(lid) = local_id {
            let (l_ty, l_name, l_binding) = {
                let l = &self.locals[lid.index()];
                (l.ty, l.name, l.binding)
            };
            target = InferValue {
                ty: Some(l_ty),
                binding: Some(l_name),
                bound: l_binding,
                ..InferValue::default()
            };
            if kind != SyntaxKind::Bind {
                target = self.graph(target, 0);
            }
            if target.ty.is_none() || !self.record_syntax(name_nid, target.clone()) {
                return InferValue::none();
            }
            target.note = self.notes_tail;
        }
        let mut rhs = if assignment
            && kind != SyntaxKind::Bind
            && let Some(lid) = local_id
        {
            self.locals[lid.index()].used = true;
            let l_ty = self.locals[lid.index()].ty;
            let op = if kind == SyntaxKind::AddAssign {
                TokenKind::Plus
            } else {
                TokenKind::Minus
            };
            self.infer_binary(op, target, rhs_nid, Some(l_ty))
        } else {
            let expected_rhs = if assignment {
                local_id.map(|lid| self.locals[lid.index()].ty)
            } else {
                None
            };
            self.infer_expression(rhs_nid, expected_rhs)
        };
        let Some(rhs_ty) = rhs.ty else {
            return InferValue::none();
        };
        if self.checked && assignment && kind != SyntaxKind::Bind {
            if !self.record_syntax(node, rhs.clone()) {
                return InferValue::none();
            }
            rhs.note = self.notes_tail;
        }
        if !assignment {
            let ok = if kind == SyntaxKind::Var {
                self.bind(NameRef::Syntax(name_nid), rhs_ty, true)
            } else {
                self.infer_binder(name_nid, rhs_ty)
            };
            if !ok {
                return InferValue::none();
            }
        }
        if !assignment
            && kind == SyntaxKind::Bind
            && name_kind == SyntaxKind::Underscore
            && !rhs.never
            && let Some(rnote) = rhs.note
        {
            self.notes[rnote.index()].discard_result = true;
        }
        if !assignment
            && kind == SyntaxKind::Bind
            && name_kind == SyntaxKind::Name
            && let Some(head) = self.active_locals_head
        {
            self.locals[head.index()].statement = true;
        }
        let Some(unit_ty) = self.native("Unit", 0) else {
            return InferValue::none();
        };
        let bound_note = if assignment {
            local_id.and_then(|lid| self.locals[lid.index()].binding)
        } else if name_kind == SyntaxKind::Tuple {
            None
        } else {
            self.notes_tail.and_then(|t| self.notes[t.index()].binding)
        };
        let result = self.graph(
            InferValue {
                ty: Some(unit_ty),
                binding: if assignment {
                    local_id.map(|lid| self.locals[lid.index()].name)
                } else {
                    None
                },
                bound: bound_note,
                never: rhs.never,
                ..InferValue::default()
            },
            1,
        );
        if let Some(gid) = result.graph {
            let g = &mut self.emitted[gid.index()];
            g.operands[0] = rhs.note;
            g.assignment = assignment;
            if name_kind == SyntaxKind::Tuple {
                g.pattern_type = Some(rhs_ty);
            }
        }
        result
    }

    fn infer_postfix(&mut self, node: NodeId, expected: Option<TermId>) -> InferValue {
        let children = self.node(node).children.clone();
        let Some(c0) = children.first().copied().flatten() else {
            return InferValue::none();
        };
        let Some(c1) = children.get(1).copied().flatten() else {
            return InferValue::none();
        };
        let c0_kind = self.node(c0).kind;
        let c1_kind = self.node(c1).kind;
        let qualified = c0_kind == SyntaxKind::CapName
            && (c1_kind == SyntaxKind::Qualified
                || (c1_kind == SyntaxKind::FieldAccess && {
                    let c1_name = self.node(c1).children.first().copied().flatten();
                    let call_arg = children
                        .get(2)
                        .copied()
                        .flatten()
                        .filter(|&n| self.node(n).kind == SyntaxKind::Call);
                    !self.case_receiver(c0)
                        || c1_name.is_some_and(|n| self.type_reading(c0, n, call_arg))
                }));
        let mut start = if qualified { 2usize } else { 1usize };
        let Some(name_nid) = (if qualified {
            self.node(c1).children.first().copied().flatten()
        } else {
            Some(c0)
        }) else {
            return InferValue::none();
        };
        let c0_bytes = self.node_text(c0);
        let simulation = qualified && c0_bytes == b"Simulation";
        let actor_op = qualified && c0_bytes == b"Actor";
        let apitest = qualified && c0_bytes == b"ApiTest";
        let start_call_args = children
            .get(start)
            .copied()
            .flatten()
            .filter(|&n| self.node(n).kind == SyntaxKind::Call)
            .and_then(|n| self.node(n).children.first().copied().flatten());
        let builtin = if start_call_args.is_some() {
            builtin_case_by_name(
                self.node_text(name_nid),
                if qualified {
                    Some(self.node_text(c0))
                } else {
                    None
                },
            )
        } else {
            None
        };
        let name_bytes = self.node_text(name_nid);
        let ability = qualified && matches!(name_bytes, b"to_text" | b"compare" | b"to_json");
        let mut data = None;
        if !simulation
            && !actor_op
            && !apitest
            && builtin.is_none()
            && (self.node(name_nid).kind == SyntaxKind::CapName || qualified)
            && !ability
            && start_call_args.is_some()
        {
            let def = self.definition(name_nid, if qualified { Some(c0) } else { None });
            if self.failed {
                return InferValue::none();
            }
            data = def.and_then(|(s, _)| self.data_type(s));
        }
        let mut v = if simulation {
            self.simulation_call(name_nid, start_call_args)
        } else if actor_op {
            self.actor_call(name_nid, start_call_args)
        } else if apitest {
            self.apitest_call(name_nid, start_call_args)
        } else if let Some(b) = builtin {
            let exp = if start + 1 == children.len() {
                expected
            } else {
                None
            };
            self.builtin_construct(b, start_call_args, exp)
        } else if let Some(data_id) = data {
            let Some(args) = start_call_args else {
                return InferValue::none();
            };
            let exp = if start + 1 == children.len() {
                expected
            } else {
                None
            };
            self.construct(data_id, args, exp)
        } else if qualified {
            self.identifier(name_nid, Some(c0))
        } else {
            self.infer_expression(c0, None)
        };
        if simulation || actor_op || apitest || data.is_some() || builtin.is_some() {
            start += 1;
        }
        let standalone_type_args = start + 1 == children.len()
            && children
                .get(start)
                .copied()
                .flatten()
                .is_some_and(|n| self.node(n).kind == SyntaxKind::TypeArgs);
        if self.checked
            && v.ty.is_some()
            && v.note.is_none()
            && start < children.len()
            && !standalone_type_args
        {
            if v.graph.is_none() {
                v = self.graph(v, 0);
            }
            let Some(prev_node) = children.get(start - 1).copied().flatten() else {
                return InferValue::none();
            };
            if v.ty.is_none() || !self.record_syntax(prev_node, v.clone()) {
                return InferValue::none();
            }
            v.note = self.notes_tail;
        }
        let mut i = start;
        while i < children.len() && v.ty.is_some() {
            let Some(step) = children[i] else {
                return InferValue::none();
            };
            let step_kind = self.node(step).kind;
            match step_kind {
                SyntaxKind::Position => {
                    let Some(idx_nid) = self.node(step).children.first().copied().flatten() else {
                        return InferValue::none();
                    };
                    v = self.infer_position(v, idx_nid);
                }
                SyntaxKind::TypeArgs => {
                    let Some(args_nid) = self.node(step).children.first().copied().flatten() else {
                        return InferValue::none();
                    };
                    v = self.type_application(v, args_nid);
                    i += 1;
                    continue;
                }
                SyntaxKind::Call => {
                    let args_nid = self.node(step).children.first().copied().flatten();
                    v = self.infer_call(v, args_nid, None);
                }
                SyntaxKind::FieldAccess => {
                    let Some(member_name) = self.node(step).children.first().copied().flatten()
                    else {
                        return InferValue::none();
                    };
                    let next_is_targs = children
                        .get(i + 1)
                        .copied()
                        .flatten()
                        .is_some_and(|n| self.node(n).kind == SyntaxKind::TypeArgs);
                    if next_is_targs {
                        let Some(targs_step) = children[i + 1] else {
                            return InferValue::none();
                        };
                        let receiver = v;
                        let Some(recv_ty) = receiver.ty else {
                            return InferValue::none();
                        };
                        let recv_root = self.root(recv_ty);
                        if self.terms[recv_root.index()].variable {
                            let span = self.node(member_name).span;
                            let _ = self.problem(
                                ErrorCode::CannotInfer,
                                span,
                                "no use decides this receiver's type before type arguments",
                                Some(
                                    "Give the receiver a known type before calling a generic dot \
                                     method.",
                                ),
                                None,
                            );
                            return InferValue::none();
                        }
                        let mut called = true;
                        let mut method = self.member(receiver.clone(), member_name, &mut called);
                        if method.ty.is_none() {
                            return InferValue::none();
                        }
                        if !called {
                            let span = self.node(targs_step).span;
                            let _ = self.problem(
                                ErrorCode::TypeArguments,
                                span,
                                "a record field doesn't take type arguments",
                                Some("Type arguments instantiate a generic function."),
                                None,
                            );
                            return InferValue::none();
                        }
                        let Some(targs_list) =
                            self.node(targs_step).children.first().copied().flatten()
                        else {
                            return InferValue::none();
                        };
                        method = self.type_application(method, targs_list);
                        let Some(method_ty) = method.ty else {
                            return InferValue::none();
                        };
                        i += 1;
                        let mut site = None;
                        if let Some(Some(call_step)) = children.get(i + 1)
                            && self.node(*call_step).kind == SyntaxKind::Call
                        {
                            site = Some(*call_step);
                            i += 1;
                        }
                        let args =
                            site.and_then(|s| self.node(s).children.first().copied().flatten());
                        if let (Some(args_nid), Some(site_nid)) = (args, site)
                            && self.node(args_nid).children.is_empty()
                            && self.receiver_only(method_ty)
                            && !self.empty_dot_call(site_nid)
                        {
                            return InferValue::none();
                        }
                        if self.checked && method.symbol.is_some() {
                            if !self.record_syntax(member_name, method.clone()) {
                                return InferValue::none();
                            }
                            method.note = self.notes_tail;
                        }
                        v = self.infer_call(method, args, Some(&receiver));
                    } else {
                        let site = children
                            .get(i + 1)
                            .copied()
                            .flatten()
                            .filter(|&n| self.node(n).kind == SyntaxKind::Call);
                        let mut called = false;
                        v = self.field_value(v, member_name, step, site, &mut called);
                        if called && site.is_some() {
                            i += 1;
                        }
                    }
                }
                _ => {
                    let _ = self.unfinished();
                    return InferValue::none();
                }
            }
            if self.checked && v.ty.is_some() && i + 1 < children.len() {
                if !self.record_syntax(step, v.clone()) {
                    return InferValue::none();
                }
                v.note = self.notes_tail;
            }
            i += 1;
        }
        v
    }

    fn infer_lambda(&mut self, node: NodeId, expected: Option<TermId>) -> InferValue {
        let Some(params_nid) = self.node(node).children.first().copied().flatten() else {
            return InferValue::none();
        };
        let Some(body_nid) = self.node(node).children.get(1).copied().flatten() else {
            return InferValue::none();
        };
        let param_children = self.node(params_nid).children.clone();
        let param_count = param_children.len();
        let Some(fn_ty) = self.make_type(TermShape {
            kind: SourceTypeKind::Function,
            name: "fn".to_string(),
            module: None,
            owner: crate::types::TypeOwner::None,
            arity: param_count + 1,
        }) else {
            return InferValue::none();
        };
        let mut arg_terms = Vec::with_capacity(param_count + 1);
        for i in 0..=param_count {
            let Some(f) = self.fresh(0) else {
                return InferValue::none();
            };
            if let Some(args) = self.terms[fn_ty.index()].arguments.as_mut() {
                args[i] = f;
            }
            arg_terms.push(f);
        }
        if let Some(exp) = expected
            && !self.unify_as(fn_ty, exp, self.node_expectation)
        {
            return InferValue::none();
        }
        let emitted = self.graph(InferValue::with_type(fn_ty), 1);
        if emitted.ty.is_none() {
            return emitted;
        }
        let mut new_lam_id = None;
        if self.checked {
            if !self.charge_arena(1, 96) || !self.charge_arena(param_count, SIZEOF_PTR) {
                return InferValue::none();
            }
            let lam_id = EmittedLambdaId(self.lambdas.len() as u32);
            self.lambdas.push(EmittedLambda {
                number: 0,
                parameters: vec![None; param_count],
                parent: self.lambda_frame,
                captures: Vec::new(),
                syntax: node,
                loop_ctx: self.capture_loop,
                path: self.capture_path,
                depth: self.lambda_depth + 1,
            });
            if let Some(gid) = emitted.graph {
                self.emitted[gid.index()].lambda = Some(lam_id);
            }
            new_lam_id = Some(lam_id);
        }
        let mark = self.active_locals_head;
        let old_result = self.result;
        let old_frame = self.lambda_frame;
        if self.checked {
            self.lambda_frame = new_lam_id;
        }
        let old_loop = self.capture_loop;
        self.capture_loop = None;
        let old_loops = self.loops;
        self.loops = 0;
        self.lambda_depth += 1;
        self.result = arg_terms[param_count];
        for (i, ch) in param_children.iter().enumerate() {
            let Some(pnid) = *ch else {
                return InferValue::none();
            };
            if !self.bind(NameRef::Syntax(pnid), arg_terms[i], false) {
                return InferValue::none();
            }
            if let Some(lam_id) = new_lam_id {
                let bound = self.notes_tail.and_then(|t| self.notes[t.index()].binding);
                self.lambdas[lam_id.index()].parameters[i] = bound;
            }
        }
        let body = if self.named(self.result, "Unit") {
            self.infer_discard(body_nid)
        } else {
            self.infer_as(body_nid, Some(self.result), Some(ErrorCode::ResultType))
        };
        if body.ty.is_none() || !self.leave(mark) {
            return InferValue::none();
        }
        self.result = old_result;
        self.lambda_frame = old_frame;
        self.capture_loop = old_loop;
        self.loops = old_loops;
        self.lambda_depth -= 1;
        if let Some(gid) = emitted.graph {
            self.emitted[gid.index()].operands[0] = body.note;
        }
        emitted
    }

    fn infer_loop(&mut self, node: NodeId, kind: SyntaxKind) -> InferValue {
        let mark = self.active_locals_head;
        let mut binding = None;
        let mut element = None;
        let body_index = if kind == SyntaxKind::For { 2 } else { 1 };
        let old_loop = self.capture_loop;
        let mut loop_id = None;
        if self.checked {
            if !self.charge_arena(1, 24) {
                return InferValue::none();
            }
            let id = CaptureLoopId(self.capture_loops.len() as u32);
            self.capture_loops.push(CaptureLoop {
                syntax: node,
                parent: old_loop,
            });
            loop_id = Some(id);
            if kind == SyntaxKind::While {
                self.capture_loop = loop_id;
            }
        }
        let test = if kind == SyntaxKind::While {
            let Some(cond_nid) = self.node(node).children.first().copied().flatten() else {
                return InferValue::none();
            };
            self.infer_condition(cond_nid, true)
        } else {
            let Some(iter_nid) = self.node(node).children.get(1).copied().flatten() else {
                return InferValue::none();
            };
            self.infer_expression(iter_nid, None)
        };
        let Some(test_ty) = test.ty else {
            return InferValue::none();
        };
        if kind == SyntaxKind::While {
            let Some(boolean) = self.native("Bool", 0) else {
                return InferValue::none();
            };
            if !self.unify_as(test_ty, boolean, Some(ErrorCode::ConditionType)) {
                return InferValue::none();
            }
        } else {
            let container = self.root(test_ty);
            if self.named(container, "Range") {
                element = self.native("Int", 0);
            } else if self.named(container, "List") || self.named(container, "Set") {
                element = self.argument(container, 0);
            } else if self.named(container, "Map") {
                if let Some(tup) = self.make_type(TermShape {
                    kind: SourceTypeKind::Tuple,
                    name: "()".to_string(),
                    module: None,
                    owner: crate::types::TypeOwner::None,
                    arity: 2,
                }) {
                    let Some(k) = self.argument(container, 0) else {
                        return InferValue::none();
                    };
                    let Some(v) = self.argument(container, 1) else {
                        return InferValue::none();
                    };
                    if let Some(args) = self.terms[tup.index()].arguments.as_mut() {
                        args[0] = k;
                        args[1] = v;
                    }
                    element = Some(tup);
                }
            } else {
                let (c_var, c_num) = {
                    let t = &self.terms[container.index()];
                    (t.variable, t.numeric)
                };
                if c_var && c_num == 0 {
                    let _ = self.unfinished();
                } else {
                    let span = self
                        .node(node)
                        .children
                        .get(body_index - 1)
                        .copied()
                        .flatten()
                        .map_or(self.span, |n| self.node(n).span);
                    let _ = self.problem(
                        ErrorCode::NotIterable,
                        span,
                        "for is given a value that can't be iterated",
                        Some("For walks a list, map, set or range (reference 7.5)."),
                        None,
                    );
                }
            }
            let Some(el_ty) = element else {
                return InferValue::none();
            };
            let Some(binder_nid) = self.node(node).children.first().copied().flatten() else {
                return InferValue::none();
            };
            if !self.infer_binder(binder_nid, el_ty) {
                return InferValue::none();
            }
            let mut nm = binder_nid;
            while self.node(nm).kind == SyntaxKind::Binder {
                let Some(inner) = self.node(nm).children.first().copied().flatten() else {
                    break;
                };
                nm = inner;
            }
            if self.node(nm).kind != SyntaxKind::Tuple {
                binding = self.notes_tail.and_then(|t| self.notes[t.index()].binding);
            }
        }
        self.capture_loop = loop_id;
        self.loops += 1;
        let Some(unit_ty) = self.native("Unit", 0) else {
            self.loops -= 1;
            self.capture_loop = old_loop;
            return InferValue::none();
        };
        let Some(body_nid) = self.node(node).children.get(body_index).copied().flatten() else {
            self.loops -= 1;
            self.capture_loop = old_loop;
            return InferValue::none();
        };
        let body = self.infer_discard(body_nid);
        self.loops -= 1;
        self.capture_loop = old_loop;
        if body.ty.is_none() || !self.leave(mark) {
            return InferValue::none();
        }
        let out = self.graph(
            InferValue {
                ty: Some(unit_ty),
                bound: binding,
                ..InferValue::default()
            },
            2,
        );
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.operands[0] = test.note;
            g.operands[1] = body.note;
            g.pattern_type = element;
        }
        out
    }

    fn infer_collection(
        &mut self,
        node: NodeId,
        kind: SyntaxKind,
        expected: Option<TermId>,
    ) -> InferValue {
        let children = self.node(node).children.clone();
        let count = children.len();
        let arity = match kind {
            SyntaxKind::Tuple => count,
            SyntaxKind::Map => 2,
            _ => 1,
        };
        let Some(t) = self.make_type(TermShape {
            kind: if kind == SyntaxKind::Tuple {
                SourceTypeKind::Tuple
            } else {
                SourceTypeKind::Named
            },
            name: match kind {
                SyntaxKind::Tuple => "()".to_string(),
                SyntaxKind::Map => "Map".to_string(),
                _ => "List".to_string(),
            },
            module: None,
            owner: crate::types::TypeOwner::None,
            arity,
        }) else {
            return InferValue::none();
        };
        let mut arg_terms = Vec::with_capacity(arity);
        for i in 0..arity {
            let Some(f) = self.fresh(0) else {
                return InferValue::none();
            };
            if let Some(args) = self.terms[t.index()].arguments.as_mut() {
                args[i] = f;
            }
            arg_terms.push(f);
        }
        if let Some(exp) = expected
            && !self.unify_as(t, exp, self.node_expectation)
        {
            return InferValue::none();
        }
        let mut out = self.graph(
            InferValue::with_type(t),
            if kind == SyntaxKind::Map {
                2 * count
            } else {
                count
            },
        );
        if out.ty.is_none() {
            return out;
        }
        let mut versions = Vec::new();
        if kind == SyntaxKind::Map && count > 1 {
            if !self.charge_arena(count, 4) {
                return InferValue::none();
            }
            versions.resize(count, 0u32);
        }
        for (i, ch) in children.iter().enumerate() {
            let Some(part_nid) = *ch else {
                return InferValue::none();
            };
            if kind == SyntaxKind::Map {
                let Some(mut key_nid) = self.node(part_nid).children.first().copied().flatten()
                else {
                    return InferValue::none();
                };
                while self.node(key_nid).kind == SyntaxKind::Group {
                    let Some(inner) = self.node(key_nid).children.first().copied().flatten() else {
                        break;
                    };
                    key_nid = inner;
                }
                if !versions.is_empty() && self.node(key_nid).kind == SyntaxKind::Name {
                    let key_bytes = self.node_text(key_nid).to_vec();
                    if let Some(active) = self.names.get(&key_bytes).and_then(|s| s.active)
                        && self.locals[active.index()].variable
                    {
                        versions[i] = self.locals[active.index()].version;
                    }
                }
                for (j, &arg_term) in arg_terms.iter().enumerate().take(2) {
                    let Some(sub_nid) = self.node(part_nid).children.get(j).copied().flatten()
                    else {
                        return InferValue::none();
                    };
                    let v = self.infer_expression(sub_nid, Some(arg_term));
                    if v.ty.is_none() {
                        return InferValue::none();
                    }
                    if let Some(gid) = out.graph {
                        self.emitted[gid.index()].operands[2 * i + j] = v.note;
                    }
                    out.never = out.never || v.never;
                }
            } else {
                let target_arg = if kind == SyntaxKind::Tuple {
                    arg_terms[i]
                } else {
                    arg_terms[0]
                };
                let v = self.infer_expression(part_nid, Some(target_arg));
                if v.ty.is_none() {
                    return InferValue::none();
                }
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[i] = v.note;
                }
                out.never = out.never || v.never;
            }
        }
        if kind == SyntaxKind::Map && !self.check_map_keys(node, &versions) {
            return InferValue::none();
        }
        out
    }

    fn infer_prefix(
        &mut self,
        node: NodeId,
        kind: SyntaxKind,
        expected: Option<TermId>,
    ) -> InferValue {
        let children = self.node(node).children.clone();
        let Some(operand) = children.last().copied().flatten() else {
            return InferValue::none();
        };
        let mut v = self.infer_expression(operand, None);
        if v.ty.is_none() {
            return v;
        }
        let negative = kind == SyntaxKind::Negate
            || (kind == SyntaxKind::Prefix
                && children.len() >= 2
                && children[children.len() - 2]
                    .is_some_and(|n| self.node(n).token == TokenKind::Minus));
        let op_kind = self.node(operand).kind;
        if negative
            && matches!(op_kind, SyntaxKind::Int | SyntaxKind::Decimal)
            && let Some(tail) = self.notes_tail
        {
            self.notes[tail.index()].negative = true;
        }
        let count = if kind == SyntaxKind::Prefix {
            children.len() - 1
        } else {
            1
        };
        for i in (1..=count).rev() {
            let op = if kind == SyntaxKind::Prefix {
                let Some(op_nid) = children[i - 1] else {
                    return InferValue::none();
                };
                self.node(op_nid).token
            } else if kind == SyntaxKind::Not {
                TokenKind::Not
            } else {
                TokenKind::Minus
            };
            if op == TokenKind::Try {
                if let Some(op_nid) = children[i - 1] {
                    self.span = self.node(op_nid).span;
                }
                v = self.infer_propagate(v, if i == 1 { expected } else { None });
                if v.ty.is_none() {
                    return v;
                }
                if self.checked && i != 1 {
                    let Some(op_nid) = children[i - 1] else {
                        return InferValue::none();
                    };
                    if !self.record_syntax(op_nid, v.clone()) {
                        return InferValue::none();
                    }
                    v.note = self.notes_tail;
                }
                continue;
            }
            let Some(v_ty) = v.ty else {
                return InferValue::none();
            };
            if op == TokenKind::Minus && !self.checked && self.named(v_ty, "Duration") {
                let _ = self.unfinished();
                return InferValue::none();
            }
            let vr = self.root(v_ty);
            let (vr_var, vr_num, vr_def, vr_kind) = {
                let t = &self.terms[vr.index()];
                (t.variable, t.numeric, t.deferred, t.shape.kind)
            };
            if op == TokenKind::Minus && self.checked && vr_var && vr_num == 0 && vr_def {
                if !self.charge_arena(1, SIZEOF_FAILURE) {
                    return InferValue::none();
                }
                let Some(result) = self.fresh(0) else {
                    return InferValue::none();
                };
                self.terms[result.index()].deferred = true;
                let span = self.node(node).span;
                let fid = FailureId(self.failures.len() as u32);
                self.failures.push(FailureCheck {
                    input: v_ty,
                    value: result,
                    context: None,
                    fallback: None,
                    input_note: None,
                    graph: None,
                    span,
                    wrapper: None,
                    generic: None,
                    op: TokenKind::Minus,
                    code: if i == 1 { self.node_expectation } else { None },
                    solved: false,
                    never: false,
                });
                let _ = fid;
                v.ty = Some(result);
            } else {
                let wanted = if op == TokenKind::Not {
                    self.native("Bool", 0)
                } else if op == TokenKind::Minus {
                    if (self.checked && self.named(v_ty, "Duration"))
                        || vr_kind == SourceTypeKind::Parameter
                    {
                        Some(v_ty)
                    } else {
                        self.fresh(NUM_ARITH)
                    }
                } else {
                    None
                };
                let Some(wanted_ty) = wanted else {
                    let _ = self.unfinished();
                    return InferValue::none();
                };
                let code = if op == TokenKind::Not {
                    ErrorCode::ConditionType
                } else if self.named(v_ty, "Byte") {
                    ErrorCode::ByteArithmetic
                } else {
                    ErrorCode::NumberAbility
                };
                if !self.unify_as(v_ty, wanted_ty, Some(code)) {
                    return InferValue::none();
                }
            }
            if self.checked {
                let input_note = v.note;
                v = self.graph(
                    InferValue {
                        ty: v.ty,
                        never: v.never,
                        ..InferValue::default()
                    },
                    1,
                );
                if v.ty.is_none() {
                    return v;
                }
                if let Some(gid) = v.graph {
                    let g = &mut self.emitted[gid.index()];
                    g.operands[0] = input_note;
                    g.operation = op;
                }
                if i != 1 {
                    let Some(op_nid) = children[i - 1] else {
                        return InferValue::none();
                    };
                    if !self.record_syntax(op_nid, v.clone()) {
                        return InferValue::none();
                    }
                    v.note = self.notes_tail;
                }
            }
        }
        v
    }

    pub(crate) fn infer_expression(
        &mut self,
        node: NodeId,
        expected: Option<TermId>,
    ) -> InferValue {
        if !self.work() {
            return InferValue::none();
        }
        let n = self.node(node);
        debug_assert!(!n.incomplete);
        let kind = n.kind;
        let span = n.span;
        let tok = n.token;
        let mark = self.active_locals_head;
        let keep = self.keep_patterns;
        let propagates = kind == SyntaxKind::Is
            || kind == SyntaxKind::Group
            || (kind == SyntaxKind::Binary && tok == TokenKind::And)
            || (kind == SyntaxKind::BinaryChain
                && n.children
                    .get(1)
                    .copied()
                    .flatten()
                    .is_some_and(|c| self.node(c).token == TokenKind::And));
        self.keep_patterns = kind == SyntaxKind::Group && keep;
        self.span = span;
        if u64::from(self.depth) == MO_LIMIT_SOURCE_INFERENCE_DEPTH {
            let _ = self.problem(
                ErrorCode::CheckSyntaxDepth,
                span,
                "the body needs too many inference frames",
                None,
                None,
            );
            return InferValue::none();
        }
        let code = self.expectation;
        let saved = self.node_expectation;
        self.node_expectation = code;
        self.expectation = None;
        self.depth += 1;
        let mut v = self.infer_node(node, expected);
        self.depth -= 1;
        self.expectation = code;
        self.node_expectation = saved;
        self.keep_patterns = keep;
        self.span = span;
        let Some(v_ty) = v.ty else {
            return InferValue::none();
        };
        if (propagates && !keep && !self.leave(mark))
            || (expected.is_some() && !v.never && !self.unify(v_ty, expected.unwrap_or(v_ty), 0))
        {
            return InferValue::none();
        }
        if self.checked && matches!(kind, SyntaxKind::ExprStmt | SyntaxKind::Group) {
            let operand = v.note;
            v = self.graph(
                InferValue {
                    ty: Some(v_ty),
                    never: v.never,
                    ..InferValue::default()
                },
                1,
            );
            if let Some(gid) = v.graph {
                self.emitted[gid.index()].operands[0] = operand;
            }
        } else if self.checked && v.graph.is_none() {
            v = self.graph(v, 0);
        }
        if v.ty.is_none() || !self.record_syntax(node, v.clone()) {
            return InferValue::none();
        }
        v.note = self.notes_tail;
        v
    }
}
