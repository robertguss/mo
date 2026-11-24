//! Pattern inference, binder checking, `case` matches, `if`/`else` conditionals, and tuple
//! `.0`/`.1` position projection (`src/check/infer-patterns.c` parity).

use mo_base::{Edit, Fix, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_NESTING_DEPTH, MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_CHECK_WORK,
};
use mo_syntax::{NodeId, SyntaxKind};

use crate::patterns::{
    KEY_FALSE, KEY_LIST_CONS, KEY_LIST_EMPTY, KEY_TRUE, KEY_TUPLE, MatchPattern, MatchPatternId,
    symbol_pattern_key,
};
use crate::types::{SourceTypeKind, builtin_case_by_name};

use super::{
    EmittedArm, EmittedMatch, EmittedMatchId, InferState, InferValue, MatchCheck, NameRef,
    SIZEOF_MATCH_CHECK, SIZEOF_PTR, SIZEOF_TYPED_PATTERN, TermId, TermShape, TypedPattern,
};

impl<'a> InferState<'a> {
    fn alloc_match_pattern(
        &mut self,
        node: NodeId,
        ty: TermId,
        key: u64,
        signature: u32,
        count: usize,
        name: String,
    ) -> Option<MatchPatternId> {
        if !self.work() {
            return None;
        }
        let count_u64 = count as u64;
        if count_u64 > MO_LIMIT_SOURCE_CHECK_WORK.saturating_sub(u64::from(self.work)) {
            let _ = self.problem(
                ErrorCode::CheckWork,
                self.span,
                "the function needs too much pattern-checking work",
                None,
                None,
            );
            return None;
        }
        if count_u64 > MO_LIMIT_PROGRAM_CHECK_WORK.saturating_sub(self.signatures.work) {
            let _ = self.problem(
                ErrorCode::CheckProgramWork,
                self.span,
                "the program needs too much pattern-checking work",
                None,
                None,
            );
            return None;
        }
        self.work += count as u32;
        self.signatures.work += count_u64;
        if !self.charge_arena(1, SIZEOF_TYPED_PATTERN) || !self.charge_arena(count, SIZEOF_PTR) {
            return None;
        }
        let mid = self.match_patterns.alloc(MatchPattern {
            name,
            key,
            signature,
            items: vec![None; count],
            data: None,
            choice: None,
        });
        self.typed_patterns.push(TypedPattern {
            match_id: mid,
            ty,
            syntax: node,
        });
        Some(mid)
    }

    fn pattern_bind(&mut self, node: NodeId, ty: TermId, group: u32, alternative: u32) -> bool {
        let kind = self.node(node).kind;
        let local_id = if kind == SyntaxKind::Name {
            let key = self.node_text(node).to_vec();
            self.names.get(&key).and_then(|s| s.active)
        } else {
            None
        };
        if alternative > 1
            && kind != SyntaxKind::Underscore
            && local_id.is_none_or(|lid| self.locals[lid.index()].pattern_group != group)
        {
            let span = self.node(node).span;
            return self.problem(
                ErrorCode::PatternAlternatives,
                span,
                "the alternatives don't bind the same names",
                Some(
                    "Arms with the same body share a line; their names must be visible \
                     throughout that body (reference 7.2).",
                ),
                None,
            );
        }
        if alternative > 1
            && let Some(lid) = local_id
            && self.locals[lid.index()].pattern_group == group
            && self.locals[lid.index()].pattern_alternative != alternative
        {
            let lty = self.locals[lid.index()].ty;
            if !self.unify(lty, ty, 0) {
                return false;
            }
            self.locals[lid.index()].pattern_alternative = alternative;
            let (lname, lbind) = {
                let l = &self.locals[lid.index()];
                (l.name, l.binding)
            };
            return self.record_syntax(
                node,
                InferValue {
                    ty: Some(ty),
                    binding: Some(lname),
                    bound: lbind,
                    ..InferValue::default()
                },
            );
        }
        if !self.bind(NameRef::Syntax(node), ty, false) {
            return false;
        }
        if kind != SyntaxKind::Underscore
            && let Some(lid) = self.active_locals_head
        {
            self.locals[lid.index()].pattern_group = group;
            self.locals[lid.index()].pattern_alternative = alternative;
        }
        true
    }

    fn pattern_node(
        &mut self,
        node: NodeId,
        ty: TermId,
        group: u32,
        alternative: u32,
        out: &mut Option<MatchPatternId>,
    ) -> bool {
        *out = None;
        let kind = self.node(node).kind;
        if kind == SyntaxKind::Group {
            let Some(inner) = self.node(node).children.first().copied().flatten() else {
                return false;
            };
            return self.infer_pattern(inner, ty, group, alternative, out);
        }
        if matches!(kind, SyntaxKind::Underscore | SyntaxKind::Name) {
            return self.pattern_bind(node, ty, group, alternative);
        }
        if kind == SyntaxKind::Tuple {
            let children = self.node(node).children.clone();
            let count = children.len();
            let Some(tuple) = self.make_type(TermShape {
                kind: SourceTypeKind::Tuple,
                name: "()".to_string(),
                module: None,
                owner: crate::types::TypeOwner::None,
                arity: count,
            }) else {
                return false;
            };
            let mut arg_terms = Vec::with_capacity(count);
            for i in 0..count {
                let Some(f) = self.fresh(0) else {
                    return false;
                };
                if let Some(args) = self.terms[tuple.index()].arguments.as_mut() {
                    args[i] = f;
                }
                arg_terms.push(f);
            }
            if !self.unify(ty, tuple, 0) {
                return false;
            }
            let Some(mid) =
                self.alloc_match_pattern(node, ty, KEY_TUPLE, 1, count, "tuple".to_string())
            else {
                return false;
            };
            for (i, child) in children.iter().enumerate() {
                let Some(cid) = *child else {
                    return false;
                };
                let mut sub = None;
                if !self.infer_pattern(cid, arg_terms[i], group, alternative, &mut sub) {
                    return false;
                }
                self.match_patterns_item_mut(mid, i, sub);
            }
            *out = Some(mid);
            return self.record_syntax(node, InferValue::with_type(ty));
        }
        if kind == SyntaxKind::List {
            let children = self.node(node).children.clone();
            let Some(list) = self.native("List", 1) else {
                return false;
            };
            let Some(elem_ty) = self.fresh(0) else {
                return false;
            };
            if let Some(args) = self.terms[list.index()].arguments.as_mut() {
                args[0] = elem_ty;
            }
            if !self.unify(ty, list, 0) {
                return false;
            }
            let mut length = children.len();
            let rest = length != 0
                && children[length - 1].is_some_and(|c| self.node(c).kind == SyntaxKind::Rest);
            if rest {
                length -= 1;
            }
            if !self.charge_arena(children.len(), SIZEOF_PTR) {
                return false;
            }
            let mut parts: Vec<Option<MatchPatternId>> = vec![None; length];
            for i in 0..length {
                let Some(cid) = children[i] else {
                    return false;
                };
                if !self.infer_pattern(cid, elem_ty, group, alternative, &mut parts[i]) {
                    return false;
                }
            }
            let mut tail: Option<MatchPatternId> = None;
            if rest {
                let Some(rest_node) = children[length] else {
                    return false;
                };
                let Some(inner) = self.node(rest_node).children.first().copied().flatten() else {
                    return false;
                };
                if !self.infer_pattern(inner, ty, group, alternative, &mut tail) {
                    return false;
                }
            } else {
                let Some(empty) =
                    self.alloc_match_pattern(node, ty, KEY_LIST_EMPTY, 2, 0, "[]".to_string())
                else {
                    return false;
                };
                tail = Some(empty);
            }
            for i in (0..length).rev() {
                let Some(cons) = self.alloc_match_pattern(
                    node,
                    ty,
                    KEY_LIST_CONS,
                    2,
                    2,
                    "list cell".to_string(),
                ) else {
                    return false;
                };
                self.match_patterns_item_mut(cons, 0, parts[i]);
                self.match_patterns_item_mut(cons, 1, tail);
                tail = Some(cons);
            }
            *out = tail;
            return self.record_syntax(node, InferValue::with_type(ty));
        }

        let qualified_call = kind == SyntaxKind::Postfix
            && self.node(node).children.len() == 3
            && self.node(node).children[0]
                .is_some_and(|c| self.node(c).kind == SyntaxKind::CapName)
            && self.node(node).children[1]
                .is_some_and(|c| self.node(c).kind == SyntaxKind::Qualified)
            && self.node(node).children[2].is_some_and(|c| self.node(c).kind == SyntaxKind::Call);
        if matches!(
            kind,
            SyntaxKind::CapName | SyntaxKind::Qualified | SyntaxKind::Call
        ) || qualified_call
        {
            let (name, args, prefix) = if qualified_call {
                let ch = &self.node(node).children;
                let pref = ch[0];
                let nm = ch[1]
                    .and_then(|q| self.node(q).children.first().copied().flatten())
                    .unwrap_or(node);
                let ag = ch[2].and_then(|c| self.node(c).children.first().copied().flatten());
                (nm, ag, pref)
            } else if kind == SyntaxKind::Call {
                let ch = &self.node(node).children;
                (
                    ch.first().copied().flatten().unwrap_or(node),
                    ch.get(1).copied().flatten(),
                    None,
                )
            } else {
                (node, None, None)
            };

            let builtin = if kind == SyntaxKind::Qualified {
                let p_opt = self.node(node).children.first().copied().flatten();
                let n_opt = self.node(node).children.get(1).copied().flatten();
                match (p_opt, n_opt) {
                    (Some(p), Some(n)) => {
                        builtin_case_by_name(self.node_text(n), Some(self.node_text(p)))
                    }
                    _ => None,
                }
            } else {
                builtin_case_by_name(self.node_text(name), prefix.map(|p| self.node_text(p)))
            };

            if let Some(bcase) = builtin {
                let Some(nominal) = self.builtin_type(bcase, Some(ty)) else {
                    return false;
                };
                let unnamed = bcase.count != 0 && bcase.fields[0].is_none();
                let arg_children = args.map(|a| self.node(a).children.clone());
                let bad_shape = (unnamed
                    && arg_children.as_ref().is_none_or(|c| {
                        c.len() != 1
                            || c[0].is_some_and(|x| self.node(x).kind == SyntaxKind::NamedArg)
                    }))
                    || (!unnamed && bcase.count != 0 && args.is_none());
                if bad_shape {
                    let span = self.node(node).span;
                    return self.problem(
                        ErrorCode::PatternFields,
                        span,
                        "the pattern doesn't match the case's fields",
                        Some(
                            "A case with named fields is matched by naming them; Some(p) matches \
                             one unnamed value (reference 7.3).",
                        ),
                        None,
                    );
                }
                let Some(mid) = self.alloc_match_pattern(
                    node,
                    ty,
                    u64::from(bcase.key),
                    bcase.cases,
                    bcase.count as usize,
                    bcase.name.to_string(),
                ) else {
                    return false;
                };
                let mut seen = [false; 2];
                if let Some(children) = arg_children {
                    for child in children {
                        let Some(arg) = child else {
                            return false;
                        };
                        let idx_opt = self.builtin_field(bcase, arg);
                        let Some(idx) = idx_opt else {
                            let span = self.node(arg).span;
                            return self.problem(
                                ErrorCode::PatternFields,
                                span,
                                "a matched field is absent or repeated",
                                Some(
                                    "A case is matched with its named fields; fields not named \
                                     are ignored (reference 7.3).",
                                ),
                                None,
                            );
                        };
                        let idx = idx as usize;
                        if idx >= bcase.count as usize || seen[idx] {
                            let span = self.node(arg).span;
                            return self.problem(
                                ErrorCode::PatternFields,
                                span,
                                "a matched field is absent or repeated",
                                Some(
                                    "A case is matched with its named fields; fields not named \
                                     are ignored (reference 7.3).",
                                ),
                                None,
                            );
                        }
                        seen[idx] = true;
                        let Some(field_ty) = self.builtin_part(bcase, nominal, idx as u32) else {
                            return false;
                        };
                        let pat_child = if unnamed {
                            arg
                        } else {
                            let Some(val) = self.node(arg).children.get(1).copied().flatten()
                            else {
                                return false;
                            };
                            val
                        };
                        let mut sub = None;
                        if !self.infer_pattern(pat_child, field_ty, group, alternative, &mut sub) {
                            return false;
                        }
                        self.match_patterns_item_mut(mid, idx, sub);
                    }
                }
                *out = Some(mid);
                return self.record_syntax(
                    node,
                    InferValue {
                        ty: Some(ty),
                        builtin_case: Some(bcase.name),
                        ..InferValue::default()
                    },
                );
            }

            let def = self.definition(name, prefix);
            if self.failed {
                return false;
            }
            let data_id = def.and_then(|(s, _)| self.data_type(s));
            let Some((symbol, data_id)) = def.map(|(s, _)| s).zip(data_id) else {
                let span = self.node(name).span;
                return self.problem(
                    ErrorCode::PatternConstructor,
                    span,
                    "a pattern doesn't name a record or case",
                    Some(
                        "A pattern names a case or record, a literal, a tuple or a list \
                         (reference 7.3).",
                    ),
                    None,
                );
            };
            let (data_ty, data_unnamed, data_count) = {
                let d = &self.signatures.data[data_id.index()];
                (d.ty, d.unnamed, d.fields.len())
            };
            let owner_sym = self
                .signatures
                .types
                .get(data_ty)
                .owner
                .as_source()
                .unwrap_or(symbol);
            if !self.visible_data(owner_sym) {
                return false;
            }
            let mut params = [None; 26];
            let Some(nominal) = self.annotation(data_ty, Some(&mut params), 0) else {
                return false;
            };
            if !self.unify(ty, nominal, 0) {
                return false;
            }
            let arg_children = args.map(|a| self.node(a).children.clone());
            let bad_shape = (data_unnamed
                && arg_children.as_ref().is_none_or(|c| {
                    c.len() != 1 || c[0].is_some_and(|x| self.node(x).kind == SyntaxKind::NamedArg)
                }))
                || (!data_unnamed && args.is_none() && data_count != 0);
            if bad_shape {
                let span = self.node(node).span;
                let decl_span = self.interfaces.symbol(symbol).name_span;
                return self.problem(
                    ErrorCode::PatternFields,
                    span,
                    "the pattern doesn't match the case's fields",
                    Some(
                        "A case with named fields is matched by naming them; Circle(p) is only \
                         for one unnamed value (reference 7.3).",
                    ),
                    Some(decl_span),
                );
            }
            let sym_info = self.interfaces.symbol(symbol);
            let owner_info = self.interfaces.symbol(owner_sym);
            let sig_count = if sym_info.owner.is_none() {
                1u32
            } else {
                owner_info.cases.len() as u32
            };
            let choice = if sym_info.owner.is_none() {
                None
            } else {
                Some(owner_sym)
            };
            let sym_name = sym_info.name.clone();
            let decl_span = sym_info.name_span;
            let Some(mid) = self.alloc_match_pattern(
                node,
                ty,
                symbol_pattern_key(symbol),
                sig_count,
                data_count,
                sym_name,
            ) else {
                return false;
            };
            self.match_patterns_meta_mut(mid, choice, Some(data_id));

            if !self.charge_arena(data_count, 1) {
                return false;
            }
            let mut seen = vec![false; data_count];
            if let Some(children) = arg_children {
                for child in &children {
                    let Some(arg) = *child else {
                        return false;
                    };
                    if !data_unnamed
                        && data_count == 1
                        && children.len() == 1
                        && self.node(arg).kind != SyntaxKind::NamedArg
                    {
                        let (fname, fspan) = {
                            let f = &self.signatures.data[data_id.index()].fields[0];
                            (f.name.clone().unwrap_or_default(), f.name_span)
                        };
                        let arg_span = self.node(arg).span;
                        let _ = self.problem(
                            ErrorCode::PatternFields,
                            arg_span,
                            "a matched field hasn't been named",
                            Some(
                                "A case with named fields is matched by naming them \
                                 (reference 7.3).",
                            ),
                            fspan,
                        );
                        if let Some(err) = &mut self.error
                            && err.code == ErrorCode::PatternFields
                            && err.primary.span.start == arg_span.start
                        {
                            let repl = format!("{fname}: ");
                            let edit = Edit::new(
                                Span::new(arg_span.file, arg_span.start, arg_span.start),
                                repl,
                            );
                            err.fixes
                                .push(Fix::new(Some("name the matched field".into()), vec![edit]));
                        }
                        return false;
                    }
                    let field_info = if data_unnamed {
                        let f = &self.signatures.data[data_id.index()].fields[0];
                        Some((f.index, f.ty))
                    } else if self.node(arg).kind == SyntaxKind::NamedArg {
                        let fname_nid = self.node(arg).children.first().copied().flatten();
                        fname_nid.and_then(|fnid| {
                            self.signatures.data[data_id.index()]
                                .find_field(self.node_text(fnid))
                                .map(|f| (f.index, f.ty))
                        })
                    } else {
                        None
                    };
                    let Some((fidx, fty)) = field_info else {
                        let span = self.node(arg).span;
                        return self.problem(
                            ErrorCode::PatternFields,
                            span,
                            "a matched field is absent or repeated",
                            Some(
                                "A record or case is matched with its named fields; fields not \
                                 named are ignored (reference 7.3).",
                            ),
                            Some(decl_span),
                        );
                    };
                    if seen[fidx as usize] {
                        let span = self.node(arg).span;
                        return self.problem(
                            ErrorCode::PatternFields,
                            span,
                            "a matched field is absent or repeated",
                            Some(
                                "A record or case is matched with its named fields; fields not \
                                 named are ignored (reference 7.3).",
                            ),
                            Some(decl_span),
                        );
                    }
                    seen[fidx as usize] = true;
                    let Some(field_term) = self.annotation(fty, Some(&mut params), 0) else {
                        return false;
                    };
                    let pat_child = if data_unnamed {
                        arg
                    } else {
                        let Some(val) = self.node(arg).children.get(1).copied().flatten() else {
                            return false;
                        };
                        val
                    };
                    let mut sub = None;
                    if !self.infer_pattern(pat_child, field_term, group, alternative, &mut sub) {
                        return false;
                    }
                    self.match_patterns_item_mut(mid, fidx as usize, sub);
                    if let Some(tail) = self.notes_tail {
                        self.notes[tail.index()].field_index = Some(fidx);
                    }
                }
            }
            *out = Some(mid);
            return self.record_syntax(
                node,
                InferValue {
                    ty: Some(ty),
                    symbol: Some(symbol),
                    ..InferValue::default()
                },
            );
        }

        let is_literal = matches!(
            kind,
            SyntaxKind::Int
                | SyntaxKind::Decimal
                | SyntaxKind::Text
                | SyntaxKind::Char
                | SyntaxKind::True
                | SyntaxKind::False
        ) || (kind == SyntaxKind::Negate
            && self
                .node(node)
                .children
                .first()
                .copied()
                .flatten()
                .is_some_and(|c| {
                    matches!(self.node(c).kind, SyntaxKind::Int | SyntaxKind::Decimal)
                }));
        if !is_literal {
            let span = self.node(node).span;
            return self.problem(
                ErrorCode::PatternConstructor,
                span,
                "a pattern doesn't name a record or case",
                Some(
                    "A pattern names a case or record, a literal, a tuple or a list \
                     (reference 7.3).",
                ),
                None,
            );
        }
        let saved_checked = self.checked;
        self.checked = false;
        let lit_val = self.infer_expression(node, Some(ty));
        self.checked = saved_checked;
        if lit_val.ty.is_none() {
            return false;
        }
        let is_bool = matches!(kind, SyntaxKind::True | SyntaxKind::False);
        let key = if kind == SyntaxKind::True {
            KEY_TRUE
        } else {
            KEY_FALSE
        };
        let sig = if is_bool { 2 } else { u32::MAX };
        let label = if kind == SyntaxKind::True {
            "true"
        } else if kind == SyntaxKind::False {
            "false"
        } else {
            "literal"
        };
        let Some(mid) = self.alloc_match_pattern(node, ty, key, sig, 0, label.to_string()) else {
            return false;
        };
        *out = Some(mid);
        true
    }

    pub(crate) fn infer_pattern(
        &mut self,
        node: NodeId,
        ty: TermId,
        group: u32,
        alternative: u32,
        out: &mut Option<MatchPatternId>,
    ) -> bool {
        if !self.work() {
            return false;
        }
        debug_assert!(u64::from(self.pattern_depth) < MO_LIMIT_NESTING_DEPTH);
        self.span = self.node(node).span;
        self.pattern_depth += 1;
        let ok = self.pattern_node(node, ty, group, alternative, out);
        self.pattern_depth -= 1;
        ok
    }

    pub(crate) fn infer_condition(&mut self, node: NodeId, keep: bool) -> InferValue {
        let prev = self.keep_patterns;
        self.keep_patterns = keep;
        let boolean = self.native("Bool", 0);
        let v = match boolean {
            None => InferValue::none(),
            Some(b) => self.infer_as(node, Some(b), Some(ErrorCode::ConditionType)),
        };
        self.keep_patterns = prev;
        v
    }

    pub(crate) fn infer_match(&mut self, node: NodeId, expected: Option<TermId>) -> InferValue {
        let Some(scrutinee) = self.node(node).children.first().copied().flatten() else {
            return InferValue::none();
        };
        let Some(arms_nid) = self.node(node).children.get(1).copied().flatten() else {
            return InferValue::none();
        };
        let input = self.infer_expression(scrutinee, None);
        let result = match expected {
            Some(e) => Some(e),
            None => self.fresh(0),
        };
        let (Some(input_ty), Some(result_ty)) = (input.ty, result) else {
            return InferValue::none();
        };
        let arm_children = self.node(arms_nid).children.clone();
        let arm_count = arm_children.len();
        let mut pat_cap = 0usize;
        for ac in &arm_children {
            if let Some(arm_nid) = *ac
                && let Some(Some(alts)) = self.node(arm_nid).children.first()
            {
                pat_cap += self.node(*alts).children.len();
            }
        }
        if !self.charge_arena(1, SIZEOF_MATCH_CHECK) || !self.charge_arena(pat_cap, SIZEOF_PTR) {
            return InferValue::none();
        }
        let mut header = self.node(node).span;
        header.end = self.node(scrutinee).span.end;
        let check_idx = self.match_checks.len();
        self.match_checks.push(MatchCheck {
            patterns: Vec::with_capacity(pat_cap),
            span: header,
            end: self.node(node).span.end,
            scrutinee,
            ty: input_ty,
            catch_all: None,
        });

        let mut emitted = self.graph(InferValue::with_type(result_ty), 1 + 2 * arm_count);
        if emitted.ty.is_none() {
            return emitted;
        }
        let mut operand = 1usize;
        let match_emitted_id = if self.checked {
            if !self.charge_arena(1, 32) || !self.charge_arena(arm_count, 32) {
                return InferValue::none();
            }
            let mid = EmittedMatchId(self.matches_emitted.len() as u32);
            self.matches_emitted.push(EmittedMatch {
                arms: Vec::with_capacity(arm_count),
            });
            if let Some(gid) = emitted.graph {
                self.emitted[gid.index()].match_emitted = Some(mid);
                self.emitted[gid.index()].operands[0] = input.note;
            }
            Some(mid)
        } else {
            None
        };

        let mut never = true;
        for (i, ac) in arm_children.iter().enumerate() {
            let Some(arm_nid) = *ac else {
                return InferValue::none();
            };
            let mark = self.active_locals_head;
            let old_path = self.capture_path;
            let (alts_nid, guard_opt, body_nid) = {
                let ch = &self.node(arm_nid).children;
                (
                    ch.first().copied().flatten(),
                    ch.get(1).copied().flatten(),
                    ch.get(2).copied().flatten(),
                )
            };
            let (Some(alts_nid), Some(body_nid)) = (alts_nid, body_nid) else {
                return InferValue::none();
            };
            let alt_children = self.node(alts_nid).children.clone();
            self.pattern_group += 1;
            let group = self.pattern_group;

            for (j, alt_opt) in alt_children.iter().enumerate() {
                let Some(alt_nid) = *alt_opt else {
                    return InferValue::none();
                };
                let mut normalized = None;
                if !self.infer_pattern(alt_nid, input_ty, group, (j + 1) as u32, &mut normalized) {
                    return InferValue::none();
                }
                if normalized.is_none() && self.match_checks[check_idx].catch_all.is_none() {
                    self.match_checks[check_idx].catch_all = Some(alt_nid);
                }
                if guard_opt.is_none() {
                    self.match_checks[check_idx].patterns.push(normalized);
                }
                let mut cur = self.active_locals_head;
                while cur != mark {
                    let Some(lid) = cur else {
                        break;
                    };
                    if self.locals[lid.index()].pattern_alternative != (j + 1) as u32 {
                        let span = self.node(alt_nid).span;
                        let prev_span = self.locals[lid.index()].name.span(self.tree());
                        let _ = self.problem(
                            ErrorCode::PatternAlternatives,
                            span,
                            "the alternatives don't bind the same names",
                            Some(
                                "Arms with the same body share a line; their names must be \
                                 visible throughout that body (reference 7.2).",
                            ),
                            Some(prev_span),
                        );
                        return InferValue::none();
                    }
                    cur = self.locals[lid.index()].previous;
                }
            }

            let mut guard_operand = None;
            if let Some(guard_nid) = guard_opt {
                let guard = self.infer_condition(guard_nid, true);
                if guard.ty.is_none() {
                    return guard;
                }
                if let Some(gid) = emitted.graph {
                    guard_operand = Some(operand);
                    self.emitted[gid.index()].operands[operand] = guard.note;
                    operand += 1;
                }
            }
            if !self.push_capture_path(node, i as u32) {
                return InferValue::none();
            }
            let body = self.infer_expression(body_nid, Some(result_ty));
            if body.ty.is_none() || !self.leave(mark) {
                return InferValue::none();
            }
            self.capture_path = old_path;
            let mut body_operand = 0usize;
            if let Some(gid) = emitted.graph {
                body_operand = operand;
                self.emitted[gid.index()].operands[operand] = body.note;
                operand += 1;
            }
            if let Some(mid) = match_emitted_id {
                self.matches_emitted[mid.index()].arms.push(EmittedArm {
                    syntax: arm_nid,
                    pattern_count: alt_children.len(),
                    guard_operand,
                    body_operand,
                });
            }
            never = never && body.never;
        }
        if let Some(gid) = emitted.graph {
            self.emitted[gid.index()].operands.truncate(operand);
        }
        emitted.never = never;
        emitted
    }

    pub(crate) fn infer_conditional(
        &mut self,
        node: NodeId,
        expected: Option<TermId>,
    ) -> InferValue {
        let (cond_nid, then_nid, parts_nid) = {
            let ch = &self.node(node).children;
            (
                ch.first().copied().flatten(),
                ch.get(1).copied().flatten(),
                ch.get(2).copied().flatten(),
            )
        };
        let (Some(cond_nid), Some(then_nid), Some(parts_nid)) = (cond_nid, then_nid, parts_nid)
        else {
            return InferValue::none();
        };
        let part_children = self.node(parts_nid).children.clone();
        let has_else = part_children
            .last()
            .copied()
            .flatten()
            .is_some_and(|c| self.node(c).kind == SyntaxKind::Else);
        let result = if has_else {
            match expected {
                Some(e) => Some(e),
                None => self.fresh(0),
            }
        } else {
            self.native("Unit", 0)
        };
        let condition = self.native("Bool", 0);
        let (Some(mut result_ty), Some(cond_ty)) = (result, condition) else {
            return InferValue::none();
        };

        let op_count = 2 + 2 * part_children.len() - usize::from(has_else);
        let mut emitted = self.graph(InferValue::with_type(result_ty), op_count);
        if emitted.ty.is_none() {
            return emitted;
        }
        let mut operand = 0usize;
        let mark = self.active_locals_head;
        let prev_keep = self.keep_patterns;
        self.keep_patterns = true;
        let test = self.infer_as(cond_nid, Some(cond_ty), Some(ErrorCode::ConditionType));
        self.keep_patterns = prev_keep;
        if test.ty.is_none() {
            return InferValue::none();
        }
        if let Some(gid) = emitted.graph {
            self.emitted[gid.index()].operands[operand] = test.note;
            operand += 1;
        }
        let old_path = self.capture_path;
        if !self.push_capture_path(node, 0) {
            return InferValue::none();
        }
        let branch = if has_else {
            self.infer_as(then_nid, Some(result_ty), self.node_expectation)
        } else {
            self.infer_discard(then_nid)
        };
        if branch.ty.is_none() || !self.leave(mark) {
            return InferValue::none();
        }
        self.capture_path = old_path;
        if let Some(gid) = emitted.graph {
            self.emitted[gid.index()].operands[operand] = branch.note;
            operand += 1;
        }
        let mut never = has_else && branch.never;
        for (i, pc) in part_children.iter().enumerate() {
            let Some(part_nid) = *pc else {
                return InferValue::none();
            };
            let part_kind = self.node(part_nid).kind;
            if part_kind == SyntaxKind::IfPart {
                let Some(part_cond) = self.node(part_nid).children.first().copied().flatten()
                else {
                    return InferValue::none();
                };
                self.keep_patterns = true;
                let t = self.infer_as(part_cond, Some(cond_ty), Some(ErrorCode::ConditionType));
                self.keep_patterns = prev_keep;
                if t.ty.is_none() {
                    return InferValue::none();
                }
                if let Some(gid) = emitted.graph {
                    self.emitted[gid.index()].operands[operand] = t.note;
                    operand += 1;
                }
            }
            let body_idx = if part_kind == SyntaxKind::Else { 0 } else { 1 };
            let Some(body_nid) = self
                .node(part_nid)
                .children
                .get(body_idx)
                .copied()
                .flatten()
            else {
                return InferValue::none();
            };
            if !self.push_capture_path(node, (i + 1) as u32) {
                return InferValue::none();
            }
            let br = if has_else {
                self.infer_as(body_nid, Some(result_ty), self.node_expectation)
            } else {
                self.infer_discard(body_nid)
            };
            if br.ty.is_none() || !self.leave(mark) {
                return InferValue::none();
            }
            self.capture_path = old_path;
            if let Some(gid) = emitted.graph {
                self.emitted[gid.index()].operands[operand] = br.note;
                operand += 1;
            }
            never = never && br.never;
        }
        if never {
            result_ty = match expected {
                Some(e) => e,
                None => {
                    let Some(u) = self.native("Unit", 0) else {
                        return InferValue::none();
                    };
                    u
                }
            };
        }
        emitted.ty = Some(result_ty);
        emitted.never = never;
        emitted
    }

    pub(crate) fn infer_binder(&mut self, node: NodeId, ty: TermId) -> bool {
        let kind = self.node(node).kind;
        if kind == SyntaxKind::Binder {
            let Some(inner) = self.node(node).children.first().copied().flatten() else {
                return false;
            };
            return self.infer_binder(inner, ty);
        }
        if kind != SyntaxKind::Tuple {
            return self.bind(NameRef::Syntax(node), ty, false);
        }
        let children = self.node(node).children.clone();
        let count = children.len();
        let Some(tuple) = self.make_type(TermShape {
            kind: SourceTypeKind::Tuple,
            name: "()".to_string(),
            module: None,
            owner: crate::types::TypeOwner::None,
            arity: count,
        }) else {
            return false;
        };
        let mut arg_terms = Vec::with_capacity(count);
        for i in 0..count {
            let Some(f) = self.fresh(0) else {
                return false;
            };
            if let Some(args) = self.terms[tuple.index()].arguments.as_mut() {
                args[i] = f;
            }
            arg_terms.push(f);
        }
        if !self.unify(tuple, ty, 0) {
            return false;
        }
        for (i, child) in children.iter().enumerate() {
            let Some(cid) = *child else {
                return false;
            };
            if !self.infer_binder(cid, arg_terms[i]) {
                return false;
            }
        }
        true
    }

    pub(crate) fn tuple_part(&mut self, t: TermId, index: NodeId) -> Option<(TermId, u32)> {
        let bytes = self.node_text(index);
        let mut at = 0u32;
        let mut fits = true;
        for &c in bytes {
            if c == b'_' {
                continue;
            }
            if !c.is_ascii_digit() {
                fits = false;
                break;
            }
            let digit = u32::from(c - b'0');
            if at > (u32::MAX - digit) / 10 {
                fits = false;
                break;
            }
            at = at * 10 + digit;
        }
        let (kind, arity) = {
            let term = &self.terms[t.index()];
            (term.shape.kind, term.shape.arity)
        };
        if !fits || kind != SourceTypeKind::Tuple || (at as usize) >= arity {
            let span = self.node(index).span;
            let _ = self.problem(
                ErrorCode::TuplePosition,
                span,
                "the tuple has no such part",
                Some("Tuple parts are read with .0, .1 and so on (reference 3.6)."),
                None,
            );
            return None;
        }
        let part_ty = self.argument(t, at as usize)?;
        Some((part_ty, at))
    }

    pub(crate) fn infer_position(&mut self, v: InferValue, index: NodeId) -> InferValue {
        let Some(v_ty) = v.ty else {
            return v;
        };
        let t = self.root(v_ty);
        if self.terms[t.index()].variable {
            let mut out = self.deferred_function(index, None, 0);
            if out.ty.is_none() {
                return out;
            }
            if let Some(did) = out.dispatch {
                self.dispatches[did.index()].receiver = v.clone();
                self.dispatches[did.index()].position = true;
            }
            out.never = v.never;
            return out;
        }
        let Some((part_ty, at)) = self.tuple_part(t, index) else {
            return InferValue::none();
        };
        let out = self.graph(
            InferValue {
                ty: Some(part_ty),
                never: v.never,
                ..InferValue::default()
            },
            1,
        );
        if let Some(gid) = out.graph {
            self.emitted[gid.index()].operands[0] = v.note;
            self.emitted[gid.index()].field = Some(at);
        }
        out
    }

    fn match_patterns_item_mut(
        &mut self,
        id: MatchPatternId,
        slot: usize,
        val: Option<MatchPatternId>,
    ) {
        self.match_patterns.set_item(id, slot, val);
    }

    fn match_patterns_meta_mut(
        &mut self,
        id: MatchPatternId,
        choice: Option<crate::ids::SymRef>,
        data: Option<crate::ids::DataSigId>,
    ) {
        self.match_patterns.set_meta(id, choice, data);
    }
}
