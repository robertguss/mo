//! Calls, dot-calls, constructors, explicit type applications, operators, `Simulation`/`Actor`/`ApiTest`
//! special forms, and `try` propagation (`src/check/infer-calls.c` parity).

use mo_base::{DiagBag, Edit, Fix};
use mo_def::{ErrorCode, SIMOPS, SimOpId};
use mo_syntax::{NodeId, SyntaxKind, TokenKind};

use crate::checked::{CheckedOr, ServiceReply};
use crate::ids::{CheckedServiceId, DataSigId, ModuleId, SymRef, TypeId};
use crate::signatures::{json_type, resolve_expression_type_in};
use crate::types::{BuiltinCase, SourceTypeKind, TypeOwner, builtin_case_by_type};

use super::{
    FailureCheck, FailureId, InferState, InferValue, NUM_ARITH, NoteRange, SIZEOF_FAILURE,
    SIZEOF_PTR, TermId, TermShape, UNKNOWN_MEMBER_MSG,
};

impl<'a> InferState<'a> {
    pub(crate) fn builtin_template(&mut self, data: &BuiltinCase) -> Option<TypeId> {
        if data.ty == "Option" {
            Some(self.signatures.types.intern_option_template())
        } else if data.ty == "Result" {
            Some(self.signatures.types.intern_result_template())
        } else {
            None
        }
    }

    pub(crate) fn builtin_type(
        &mut self,
        data: &BuiltinCase,
        expected: Option<TermId>,
    ) -> Option<TermId> {
        let ty = self.native(data.ty, data.parameters as usize)?;
        for i in 0..(data.parameters as usize) {
            let f = self.fresh(0)?;
            if let Some(args) = self.terms[ty.index()].arguments.as_mut() {
                args[i] = f;
            }
        }
        if let Some(exp) = expected
            && !self.unify(ty, exp, 0)
        {
            return None;
        }
        Some(ty)
    }

    pub(crate) fn builtin_field(&self, data: &BuiltinCase, arg: NodeId) -> Option<u32> {
        let node = self.node(arg);
        if data.fields[0].is_none() {
            return if node.kind == SyntaxKind::NamedArg {
                None
            } else {
                Some(0)
            };
        }
        if node.kind == SyntaxKind::NamedArg
            && let Some(fname) = node.children.first().copied().flatten()
        {
            let text = self.node_text(fname);
            for i in 0..(data.count as usize) {
                if data.fields[i].is_some_and(|f| f.as_bytes() == text) {
                    return Some(i as u32);
                }
            }
        }
        None
    }

    pub(crate) fn builtin_part(
        &mut self,
        data: &BuiltinCase,
        ty: TermId,
        index: u32,
    ) -> Option<TermId> {
        debug_assert!(index < data.count);
        if data.ty == "Sqlite.Value" {
            let kind = match data.name {
                "Integer" => "Int",
                "Real" => "Float",
                "String" => "Text",
                _ => "Bytes",
            };
            return self.native(kind, 0);
        }
        if data.ty == "Sqlite.Rows" {
            let element = if index == 0 {
                self.native("Text", 0)?
            } else {
                let inner = self.native("List", 1)?;
                let sv = self.native("Sqlite.Value", 0)?;
                if let Some(args) = self.terms[inner.index()].arguments.as_mut() {
                    args[0] = sv;
                }
                inner
            };
            let list = self.native("List", 1)?;
            if let Some(args) = self.terms[list.index()].arguments.as_mut() {
                args[0] = element;
            }
            return Some(list);
        }
        if data.fields[0].is_none() {
            return self.argument(ty, data.parameter as usize);
        }
        let field_ty_name = if index == 0 {
            "Text"
        } else if data.name == "Entry" {
            "EntryKind"
        } else if data.name == "Refused" {
            "Int"
        } else {
            "Text"
        };
        self.native(field_ty_name, 0)
    }

    pub(crate) fn builtin_construct(
        &mut self,
        data: &'static BuiltinCase,
        args: Option<NodeId>,
        expected: Option<TermId>,
    ) -> InferValue {
        let Some(ty) = self.builtin_type(data, expected) else {
            return InferValue::none();
        };
        let arg_children = args
            .map(|a| self.node(a).children.clone())
            .unwrap_or_default();
        let arg_count = arg_children.len() as u32;
        let args_span = args.map_or(self.span, |a| self.node(a).span);
        if arg_count < data.count {
            let mut provided = [false; 2];
            for ch in &arg_children {
                if let Some(arg_nid) = *ch
                    && let Some(idx) = self.builtin_field(data, arg_nid)
                    && idx < data.count
                {
                    provided[idx as usize] = true;
                }
            }
            for (i, &is_provided) in provided.iter().enumerate().take(data.count as usize) {
                if is_provided {
                    continue;
                }
                let _ = self.missing_field(args_span, data.fields[i].map(str::as_bytes), None);
                return InferValue::none();
            }
            debug_assert!(false);
        }
        if arg_count != data.count {
            let _ = self.problem(
                ErrorCode::DataArguments,
                args_span,
                "the case hasn't been given every field",
                Some(
                    "A case is built with every named field or its one unnamed value \
                     (reference 4).",
                ),
                None,
            );
            return InferValue::none();
        }
        let mut seen = [false; 2];
        let mut out = self.graph(
            InferValue {
                ty: Some(ty),
                builtin_case: Some(data.name),
                ..InferValue::default()
            },
            data.count as usize,
        );
        if out.ty.is_none() {
            return out;
        }
        if let Some(gid) = out.graph {
            let tmpl = self.builtin_template(data);
            self.emitted[gid.index()].generic = tmpl;
            self.emitted[gid.index()].instantiated = Some(ty);
        }
        for (i, ch) in arg_children.iter().enumerate() {
            let Some(arg_nid) = *ch else {
                return InferValue::none();
            };
            let idx_opt = self.builtin_field(data, arg_nid);
            let Some(idx) = idx_opt.filter(|&k| !seen[k as usize]) else {
                let span = self.node(arg_nid).span;
                let _ = self.problem(
                    ErrorCode::DataArguments,
                    span,
                    "a supplied field doesn't match the case",
                    Some(
                        "A case is built with every named field once or its one unnamed value \
                         (reference 4).",
                    ),
                    None,
                );
                return InferValue::none();
            };
            seen[idx as usize] = true;
            let Some(field_ty) = self.builtin_part(data, ty, idx) else {
                return InferValue::none();
            };
            let val_nid = if self.node(arg_nid).kind == SyntaxKind::NamedArg {
                let Some(rhs) = self.node(arg_nid).children.get(1).copied().flatten() else {
                    return InferValue::none();
                };
                rhs
            } else {
                arg_nid
            };
            let mut v = self.infer_expression(val_nid, Some(field_ty));
            if v.ty.is_none() {
                return InferValue::none();
            }
            if self.checked {
                if self.node(arg_nid).kind == SyntaxKind::NamedArg {
                    let supplied = self.graph(
                        InferValue {
                            ty: v.ty,
                            never: v.never,
                            ..InferValue::default()
                        },
                        1,
                    );
                    if supplied.ty.is_none() {
                        return supplied;
                    }
                    if let Some(sgid) = supplied.graph {
                        self.emitted[sgid.index()].field = Some(idx);
                        self.emitted[sgid.index()].operands[0] = v.note;
                    }
                    if !self.record_syntax(arg_nid, supplied) {
                        return InferValue::none();
                    }
                    v.note = self.notes_tail;
                }
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[i] = v.note;
                }
            }
            out.never = out.never || v.never;
        }
        out
    }

    pub(crate) fn construct(
        &mut self,
        data_id: DataSigId,
        args: NodeId,
        expected: Option<TermId>,
    ) -> InferValue {
        let (data_sym, data_ty, owner_sym, field_count, unnamed) = {
            let d = &self.signatures.data[data_id.index()];
            let owner = self
                .signatures
                .types
                .get(d.ty)
                .owner
                .as_source()
                .unwrap_or(d.symbol);
            (d.symbol, d.ty, owner, d.fields.len(), d.unnamed)
        };
        if !self.visible_data(owner_sym) {
            return InferValue::none();
        }
        let mut parameters = [None; 26];
        let Some(ty) = self.annotation(data_ty, Some(&mut parameters), 0) else {
            return InferValue::none();
        };
        if let Some(exp) = expected
            && !self.unify(ty, exp, 0)
        {
            return InferValue::none();
        }
        let arg_children = self.node(args).children.clone();
        let arg_count = arg_children.len();
        let mut out = self.graph(
            InferValue {
                ty: Some(ty),
                symbol: Some(data_sym),
                ..InferValue::default()
            },
            arg_count,
        );
        if out.ty.is_none() {
            return out;
        }
        if let Some(gid) = out.graph {
            self.emitted[gid.index()].generic = Some(data_ty);
            self.emitted[gid.index()].instantiated = Some(ty);
        }
        if !self.charge_arena(field_count, 1)
            || !self.charge_arena(arg_count, SIZEOF_PTR)
            || !self.charge_arena(arg_count, 16)
        {
            return InferValue::none();
        }
        let mut seen = vec![false; field_count];
        let mut selected: Vec<(u32, TypeId)> = Vec::with_capacity(arg_count);
        for ch in &arg_children {
            let Some(arg_nid) = *ch else {
                return InferValue::none();
            };
            let arg_kind = self.node(arg_nid).kind;
            let matched = if unnamed && arg_kind != SyntaxKind::NamedArg {
                self.signatures.data[data_id.index()]
                    .fields
                    .first()
                    .map(|f| (f.index, f.ty, f.name_span))
            } else if arg_kind == SyntaxKind::NamedArg {
                self.node(arg_nid)
                    .children
                    .first()
                    .copied()
                    .flatten()
                    .and_then(|fname| {
                        let bytes = self.node_text(fname);
                        self.signatures.data[data_id.index()]
                            .find_field(bytes)
                            .map(|f| (f.index, f.ty, f.name_span))
                    })
            } else {
                None
            };
            let Some((fidx, fty, fspan)) = matched else {
                let span = self.node(arg_nid).span;
                let decl_span = self.interfaces.symbol(data_sym).name_span;
                let _ = self.problem(
                    ErrorCode::DataArguments,
                    span,
                    "a supplied field doesn't match the record or case",
                    Some(
                        "A record or case is built by naming every field once, or from its one \
                         unnamed value (reference 4).",
                    ),
                    Some(decl_span),
                );
                return InferValue::none();
            };
            if seen[fidx as usize] {
                let span = self.node(arg_nid).span;
                let _ = self.problem(
                    ErrorCode::DataArguments,
                    span,
                    "a supplied field doesn't match the record or case",
                    Some(
                        "A record or case is built by naming every field once, or from its one \
                         unnamed value (reference 4).",
                    ),
                    fspan,
                );
                return InferValue::none();
            }
            seen[fidx as usize] = true;
            selected.push((fidx, fty));
        }
        for (i, &was_seen) in seen.iter().enumerate() {
            if was_seen {
                continue;
            }
            let (fname, fspan) = {
                let f = self.signatures.data[data_id.index()].field_by_index(i);
                (f.name.clone(), f.name_span)
            };
            let span = self.node(args).span;
            let _ = self.missing_field(span, fname.as_deref().map(str::as_bytes), fspan);
            return InferValue::none();
        }
        let mut ranges = vec![
            NoteRange {
                first: None,
                last: None,
            };
            arg_count
        ];
        let mut never = false;
        let before = self.notes_tail;
        for pass in 0..2 {
            for (i, ch) in arg_children.iter().enumerate() {
                let Some(arg_nid) = *ch else {
                    return InferValue::none();
                };
                let value_nid = if unnamed {
                    arg_nid
                } else {
                    let Some(rhs) = self.node(arg_nid).children.get(1).copied().flatten() else {
                        return InferValue::none();
                    };
                    rhs
                };
                if self.is_lambda_argument(value_nid) != (pass != 0) {
                    continue;
                }
                let (fidx, fty) = selected[i];
                let Some(wanted) = self.annotation(fty, Some(&mut parameters), 0) else {
                    return InferValue::none();
                };
                let previous = self.notes_tail;
                let mut v = self.infer_as(value_nid, Some(wanted), Some(ErrorCode::ArgumentType));
                if v.ty.is_none() {
                    return InferValue::none();
                }
                if let Some(tail) = self.notes_tail {
                    self.notes[tail.index()].field_index = Some(fidx);
                }
                if self.checked {
                    if !unnamed {
                        let supplied = self.graph(
                            InferValue {
                                ty: v.ty,
                                never: v.never,
                                ..InferValue::default()
                            },
                            1,
                        );
                        if supplied.ty.is_none() {
                            return supplied;
                        }
                        if let Some(sgid) = supplied.graph {
                            self.emitted[sgid.index()].field = Some(fidx);
                            self.emitted[sgid.index()].operands[0] = v.note;
                        }
                        if !self.record_syntax(arg_nid, supplied) {
                            return InferValue::none();
                        }
                        v.note = self.notes_tail;
                    }
                    if let Some(gid) = out.graph {
                        self.emitted[gid.index()].operands[i] = v.note;
                    }
                }
                let first = match previous {
                    None => self.notes_head,
                    Some(prev) => self.notes[prev.index()].next,
                };
                ranges[i] = NoteRange {
                    first,
                    last: self.notes_tail,
                };
                never = never || v.never;
            }
        }
        self.order_notes(before, &ranges);
        out.never = never;
        out
    }

    pub(crate) fn ability_function(&mut self, receiver: InferValue, name: NodeId) -> InferValue {
        let Some(recv_ty) = receiver.ty else {
            return InferValue::none();
        };
        let nbytes = self.node_text(name);
        let text = nbytes == b"to_text";
        let json = nbytes == b"to_json";
        let result_term = if json {
            let Some(json_ty) = json_type(self.interfaces, self.signatures, "Json") else {
                let span = self.node(name).span;
                let _ = self.problem(
                    ErrorCode::ImportMissing,
                    span,
                    "to_json needs use Json",
                    Some("Bring the Json module into scope with use Json (reference 9.2, 10.3)."),
                    None,
                );
                return InferValue::none();
            };
            self.annotation(json_ty, None, 0)
        } else {
            self.native(if text { "Text" } else { "Ordering" }, 0)
        };
        let Some(res_term) = result_term else {
            return InferValue::none();
        };
        let arity = if text || json { 2 } else { 3 };
        let Some(fn_ty) = self.make_type(TermShape {
            kind: SourceTypeKind::Function,
            name: "fn".to_string(),
            module: None,
            owner: TypeOwner::None,
            arity,
        }) else {
            return InferValue::none();
        };
        if let Some(args) = self.terms[fn_ty.index()].arguments.as_mut() {
            args[0] = recv_ty;
            if !text && !json {
                args[1] = recv_ty;
            }
            args[arity - 1] = res_term;
        }
        let out = self.graph(InferValue::with_type(fn_ty), 0);
        if let Some(gid) = out.graph {
            self.emitted[gid.index()].native = Some(
                if json {
                    "to_json"
                } else if text {
                    "to_text"
                } else {
                    "compare"
                }
                .to_string(),
            );
        }
        out
    }

    pub(crate) fn member(&mut self, v: InferValue, name: NodeId, called: &mut bool) -> InferValue {
        *called = true;
        let Some(v_ty) = v.ty else {
            return v;
        };
        self.span = self.node(name).span;
        let ty = self.root(v_ty);
        debug_assert!(!self.terms[ty.index()].variable);

        let (ty_var, ty_kind, ty_module, ty_owner, ty_name, ty_arity) = {
            let t = &self.terms[ty.index()];
            (
                t.variable,
                t.shape.kind,
                t.shape.module,
                t.shape.owner,
                t.shape.name.clone(),
                t.shape.arity,
            )
        };

        let nbytes = self.node_text(name);
        if !ty_var && ty_kind == SourceTypeKind::Named && ty_module.is_none() {
            if ty_name == "Entry" && (nbytes == b"name" || nbytes == b"kind") {
                *called = false;
                let index = if nbytes == b"name" { 0u32 } else { 1u32 };
                if let Some(data) = builtin_case_by_type("Entry")
                    && let Some(part_ty) = self.builtin_part(data, ty, index)
                {
                    let out = self.graph(
                        InferValue {
                            ty: Some(part_ty),
                            never: v.never,
                            ..InferValue::default()
                        },
                        1,
                    );
                    if let Some(gid) = out.graph {
                        let g = &mut self.emitted[gid.index()];
                        g.field = Some(index);
                        g.native = Some(if index == 0 {
                            "Entry.name".to_string()
                        } else {
                            "Entry.kind".to_string()
                        });
                        g.operands[0] = v.note;
                    }
                    return out;
                }
                return InferValue::none();
            }
            if ty_name == "Sqlite.Rows" && (nbytes == b"columns" || nbytes == b"rows") {
                *called = false;
                let index = if nbytes == b"columns" { 0u32 } else { 1u32 };
                if let Some(data) = builtin_case_by_type("Sqlite.Rows")
                    && let Some(part_ty) = self.builtin_part(data, ty, index)
                {
                    let out = self.graph(
                        InferValue {
                            ty: Some(part_ty),
                            never: v.never,
                            ..InferValue::default()
                        },
                        1,
                    );
                    if let Some(gid) = out.graph {
                        let g = &mut self.emitted[gid.index()];
                        g.field = Some(index);
                        g.native = Some(if index == 0 {
                            "Sqlite.Rows.columns".to_string()
                        } else {
                            "Sqlite.Rows.rows".to_string()
                        });
                        g.operands[0] = v.note;
                    }
                    return out;
                }
                return InferValue::none();
            }
            if ty_name == "ApiTest.App" {
                let op_info = match nbytes {
                    b"send" => Some(("ApiTest.App.send", 3usize)),
                    b"run_job" => Some(("ApiTest.App.run_job", 4usize)),
                    b"routes" => Some(("ApiTest.App.routes", 2usize)),
                    b"openapi_json" => Some(("ApiTest.App.openapi_json", 2usize)),
                    _ => None,
                };
                if let Some((native_name, fn_arity)) = op_info {
                    let Some(fn_term) = self.make_type(TermShape {
                        kind: SourceTypeKind::Function,
                        name: "fn".to_string(),
                        module: None,
                        owner: TypeOwner::None,
                        arity: fn_arity,
                    }) else {
                        return InferValue::none();
                    };
                    if let Some(args_mut) = self.terms[fn_term.index()].arguments.as_mut() {
                        args_mut[0] = ty;
                    }
                    match nbytes {
                        b"send" => {
                            let Some(req_src) = self.find_lib_type("Api", "Request") else {
                                return InferValue::none();
                            };
                            let Some(res_src) = self.find_lib_type("Api", "Response") else {
                                return InferValue::none();
                            };
                            let Some(req_term) = self.annotation(req_src, None, 0) else {
                                return InferValue::none();
                            };
                            let Some(res_term) = self.annotation(res_src, None, 0) else {
                                return InferValue::none();
                            };
                            if let Some(args_mut) = self.terms[fn_term.index()].arguments.as_mut() {
                                args_mut[1] = req_term;
                                args_mut[2] = res_term;
                            }
                        }
                        b"run_job" => {
                            let Some(json_src) = self.find_lib_type("Json", "Json") else {
                                return InferValue::none();
                            };
                            let Some(text_term) = self.native("Text", 0) else {
                                return InferValue::none();
                            };
                            let Some(unit_term) = self.native("Unit", 0) else {
                                return InferValue::none();
                            };
                            let Some(json_term) = self.annotation(json_src, None, 0) else {
                                return InferValue::none();
                            };
                            let Some(res_term) = self.native("Result", 2) else {
                                return InferValue::none();
                            };
                            if let Some(r_args) = self.terms[res_term.index()].arguments.as_mut() {
                                r_args[0] = unit_term;
                                r_args[1] = text_term;
                            }
                            if let Some(args_mut) = self.terms[fn_term.index()].arguments.as_mut() {
                                args_mut[1] = text_term;
                                args_mut[2] = json_term;
                                args_mut[3] = res_term;
                            }
                        }
                        b"routes" => {
                            let Some(rs_src) = self.find_lib_type("Api", "RouteSpec") else {
                                return InferValue::none();
                            };
                            let Some(rs_term) = self.annotation(rs_src, None, 0) else {
                                return InferValue::none();
                            };
                            let Some(list_term) = self.native("List", 1) else {
                                return InferValue::none();
                            };
                            if let Some(l_args) = self.terms[list_term.index()].arguments.as_mut() {
                                l_args[0] = rs_term;
                            }
                            if let Some(args_mut) = self.terms[fn_term.index()].arguments.as_mut() {
                                args_mut[1] = list_term;
                            }
                        }
                        _ => {
                            let Some(text_term) = self.native("Text", 0) else {
                                return InferValue::none();
                            };
                            if let Some(args_mut) = self.terms[fn_term.index()].arguments.as_mut() {
                                args_mut[1] = text_term;
                            }
                        }
                    }
                    let out = self.graph(InferValue::with_type(fn_term), 0);
                    if let Some(gid) = out.graph {
                        self.emitted[gid.index()].native = Some(native_name.to_string());
                    }
                    return out;
                }
            }
            let module = if ty_name == "Sqlite.Database" {
                "Sqlite"
            } else if ty_name == "Files.File" {
                "Files"
            } else if ty_name == "Command.Process" {
                "Command"
            } else {
                ty_name.as_str()
            };
            if let Some(nat_id) = self.native_definition(module, name) {
                return self.native_identifier(nat_id);
            }
        }

        if !ty_var
            && ty_kind == SourceTypeKind::Named
            && let Some(owner_sym) = ty_owner.as_source()
            && self.interfaces.symbol(owner_sym).decl_kind == SyntaxKind::Actor
            && let Some(view) = self.services_view
        {
            let mut found_svc = None;
            for (i, svc) in view.services.iter().enumerate() {
                if !self.work() {
                    return InferValue::none();
                }
                if svc.is_actor && svc.actor_symbol == Some(owner_sym) {
                    found_svc = Some(i);
                    break;
                }
            }
            if let Some(svc_idx) = found_svc {
                let tool_count = view.services[svc_idx].tools.len();
                let svc_mod = view.services[svc_idx].module;
                for ti in 0..tool_count {
                    if !self.work() {
                        return InferValue::none();
                    }
                    let tool_name_nid = view.services[svc_idx].tools[ti].name;
                    let tool_bytes = self
                        .modules
                        .module(svc_mod)
                        .syntax
                        .tree
                        .node_text(tool_name_nid)
                        .unwrap_or(&[]);
                    if self.node_text(name) != tool_bytes {
                        continue;
                    }
                    let fn_idx = view.services[svc_idx].tools[ti].function;
                    let reply_kind = view.services[svc_idx].tools[ti].reply;
                    let sig_id = view.function_sigs[fn_idx.index()];
                    let Some(sig_ty_id) = self.signatures.functions[sig_id.index()].ty else {
                        return InferValue::none();
                    };
                    let sig_args = self.signatures.types.get(sig_ty_id).arguments.clone();
                    debug_assert!(sig_args.len() >= 3);
                    let fn_arity = sig_args.len() - 1;
                    let Some(fn_term) = self.make_type(TermShape {
                        kind: SourceTypeKind::Function,
                        name: "fn".to_string(),
                        module: None,
                        owner: TypeOwner::None,
                        arity: fn_arity,
                    }) else {
                        return InferValue::none();
                    };
                    if let Some(args) = self.terms[fn_term.index()].arguments.as_mut() {
                        args[0] = ty;
                    }
                    for k in 2..(sig_args.len() - 1) {
                        let Some(arg_t) = self.annotation(sig_args[k], None, 0) else {
                            return InferValue::none();
                        };
                        if let Some(args) = self.terms[fn_term.index()].arguments.as_mut() {
                            args[k - 1] = arg_t;
                        }
                    }
                    let raw_ret = sig_args[sig_args.len() - 1];
                    let reply_term = match reply_kind {
                        ServiceReply::ReadOnly => self.annotation(raw_ret, None, 0),
                        ServiceReply::Update => {
                            let r1 = self.signatures.types.get(raw_ret).arguments[1];
                            self.annotation(r1, None, 0)
                        }
                        ServiceReply::ResultUpdate => {
                            let res_args = self.signatures.types.get(raw_ret).arguments.clone();
                            let ok_pair_1 = self.signatures.types.get(res_args[0]).arguments[1];
                            let err_ty = res_args[1];
                            if let Some(rt) = self.native("Result", 2)
                                && let Some(a0) = self.annotation(ok_pair_1, None, 0)
                                && let Some(a1) = self.annotation(err_ty, None, 0)
                            {
                                if let Some(args) = self.terms[rt.index()].arguments.as_mut() {
                                    args[0] = a0;
                                    args[1] = a1;
                                }
                                Some(rt)
                            } else {
                                None
                            }
                        }
                    };
                    let Some(reply_t) = reply_term else {
                        return InferValue::none();
                    };
                    if let Some(args) = self.terms[fn_term.index()].arguments.as_mut() {
                        args[fn_arity - 1] = reply_t;
                    }
                    let out = self.graph(InferValue::with_type(fn_term), 0);
                    if let Some(gid) = out.graph {
                        let g = &mut self.emitted[gid.index()];
                        g.native = Some("Actor.call".to_string());
                        g.service = Some(CheckedServiceId(svc_idx as u32));
                        g.tool = Some(ti as u32);
                    }
                    return out;
                }
            }
        }

        let owner_sym_opt = if ty_var { None } else { ty_owner.as_source() };
        let data_id = owner_sym_opt.and_then(|s| self.data_type(s));
        let field_info = data_id.and_then(|did| {
            self.signatures.data[did.index()]
                .find_field(self.node_text(name))
                .map(|f| (f.index, f.ty))
        });
        let visible = data_id.is_some()
            && owner_sym_opt.is_some_and(|os| {
                ty_module == Some(self.enclosing.module) || self.interfaces.symbol(os).insides
            });

        if (field_info.is_none() || !visible)
            && matches!(self.node_text(name), b"to_text" | b"compare" | b"to_json")
        {
            return self.ability_function(v, name);
        }

        if field_info.is_none() || !visible {
            let owner_mod = self.type_scope(ty);
            if self.failed {
                return InferValue::none();
            }
            let mut method_found = owner_mod.and_then(|m| self.find_symbol_in(m, name));
            let mut plausible = method_found.is_some_and(|(s, _)| {
                let sym = self.interfaces.symbol(s);
                sym.decl_kind == SyntaxKind::Fn
                    && (sym.module == self.enclosing.module || sym.exported)
            });
            if !plausible {
                method_found = self.definition(name, None);
                plausible = method_found
                    .is_some_and(|(s, _)| self.interfaces.symbol(s).decl_kind == SyntaxKind::Fn);
            }
            if plausible && let Some((mref, cnt)) = method_found {
                let Some(selected) = self.select_overload(mref, cnt, ty) else {
                    return InferValue::none();
                };
                return self.declared_function(selected);
            }
        }

        let Some(owner_sym) = owner_sym_opt else {
            let mut habit = self.length_habit(name);
            if self.named(ty, "Simulation") {
                habit = Some(
                    "Simulation operations are called through the module, as in \
                     Simulation.crash(sim), not as dot calls (reference 12.4).",
                );
            } else if self.named(ty, "ApiTest.App") {
                habit = Some(
                    "ApiTest.App's operations are app.send(req), app.run_job(name, payload), \
                     app.routes() and app.openapi_json().",
                );
            }
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::NameUnknown,
                span,
                "no visible function has this dot-call name",
                Some(habit.unwrap_or(
                    "A dot call looks in the type's module, then among the names in scope \
                     (reference 5.4).",
                )),
                None,
            );
            return InferValue::none();
        };

        if self.interfaces.symbol(owner_sym).decl_kind == SyntaxKind::Actor {
            let span = self.node(name).span;
            let decl_span = self.interfaces.symbol(owner_sym).name_span;
            let _ = self.problem(
                ErrorCode::NameUnknown,
                span,
                "the actor declares no such call",
                Some(
                    "Actor dot calls invoke handlers listed in the actor's calls clause, or \
                     visible functions taking the actor handle first (reference 11.7).",
                ),
                Some(decl_span),
            );
            return InferValue::none();
        }

        if !self.visible_data(owner_sym) {
            return InferValue::none();
        }
        let Some((field_idx, field_ty_id)) = field_info else {
            let span = self.node(name).span;
            let decl_span = self.interfaces.symbol(owner_sym).name_span;
            let _ = self.problem(
                ErrorCode::FieldMissing,
                span,
                "the record has no such field",
                Some("A struct declares named fields, each with a type (reference 4.1)."),
                Some(decl_span),
            );
            return InferValue::none();
        };
        *called = false;
        let Some(did) = data_id else {
            return InferValue::none();
        };
        let (data_ty_id, params_nid, owner_mod) = {
            let d = &self.signatures.data[did.index()];
            (d.ty, d.parameters, d.symbol.module)
        };
        let mut parameters = [None; 26];
        for i in 0..ty_arity {
            let Some(pnid) = params_nid else {
                return InferValue::none();
            };
            let param_child = self
                .modules
                .module(owner_mod)
                .syntax
                .tree
                .node(pnid)
                .children[i];
            let Some(pc) = param_child else {
                return InferValue::none();
            };
            let letter_byte = self
                .modules
                .module(owner_mod)
                .syntax
                .tree
                .node_text(pc)
                .and_then(|b| b.first().copied())
                .unwrap_or(b'A');
            let Some(arg_t) = self.argument(ty, i) else {
                return InferValue::none();
            };
            parameters[(letter_byte - b'A') as usize] = Some(arg_t);
            if self.failed {
                return InferValue::none();
            }
        }
        let Some(projected_ty) = self.annotation(field_ty_id, Some(&mut parameters), 0) else {
            return InferValue::none();
        };
        let out = self.graph(
            InferValue {
                ty: Some(projected_ty),
                field_index: Some(field_idx),
                never: v.never,
                ..InferValue::default()
            },
            1,
        );
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.field = Some(field_idx);
            g.operands[0] = v.note;
            g.generic = Some(data_ty_id);
            g.instantiated = Some(ty);
        }
        out
    }

    pub(crate) fn receiver_only(&mut self, ty: TermId) -> bool {
        let r = self.root(ty);
        let t = &self.terms[r.index()];
        !t.variable && t.shape.kind == SourceTypeKind::Function && t.shape.arity == 2
    }

    pub(crate) fn allows_empty_dot_parens(&self, resolved: &InferValue) -> bool {
        if let Some(sym_ref) = resolved.symbol {
            let sym = self.interfaces.symbol(sym_ref);
            let scope = self.interfaces.scope(sym.module);
            if scope.library
                && matches!(
                    (scope.name.as_deref(), sym.name.as_str()),
                    (Some("Network"), "recorded_calls")
                        | (Some("Sqlite"), "stats" | "reset_stats")
                        | (Some("Command"), "allow_loopback" | "kill")
                )
            {
                return true;
            }
        }
        let Some(gid) = resolved.graph else {
            return false;
        };
        matches!(
            self.emitted[gid.index()].native.as_deref(),
            Some(
                "ApiTest.App.routes"
                    | "ApiTest.App.openapi_json"
                    | "Sqlite.stats"
                    | "Sqlite.reset_stats"
                    | "Command.allow_loopback"
                    | "Command.kill"
            )
        )
    }

    pub(crate) fn empty_dot_call(&mut self, site: NodeId) -> bool {
        let span = self.node(site).span;
        let _ = self.problem(
            ErrorCode::DotCallParentheses,
            span,
            "write x.f, not x.f()",
            Some("x.f, with no other arguments, calls f(x) (reference 5.4)."),
            None,
        );
        if let Some(err) = self.error.as_mut()
            && err.code == ErrorCode::DotCallParentheses
            && err.primary.span.start == span.start
        {
            err.fixes.push(Fix::new(
                Some("remove the parentheses".into()),
                vec![Edit::new(span, "")],
            ));
        }
        false
    }

    pub(crate) fn field_value(
        &mut self,
        receiver: InferValue,
        name: NodeId,
        syntax: NodeId,
        site: Option<NodeId>,
        called: &mut bool,
    ) -> InferValue {
        let Some(recv_ty) = receiver.ty else {
            return receiver;
        };
        let args = site.and_then(|s| self.node(s).children.first().copied().flatten());
        let r = self.root(recv_ty);
        if self.terms[r.index()].variable {
            *called = true;
            let arg_children = args
                .map(|a| self.node(a).children.clone())
                .unwrap_or_default();
            let count = arg_children.len();
            let mut out = self.deferred_function(name, None, count);
            if out.ty.is_none() {
                return out;
            }
            if !self.charge_arena(count, 64) {
                return InferValue::none();
            }
            let Some(did) = out.dispatch else {
                return InferValue::none();
            };
            {
                let d = &mut self.dispatches[did.index()];
                d.syntax = Some(syntax);
                d.call = site;
                d.arguments = args;
                d.receiver = receiver.clone();
            }
            out.never = receiver.never;
            let mut inputs = Vec::with_capacity(count);
            for ch in &arg_children {
                let Some(arg_nid) = *ch else {
                    return InferValue::none();
                };
                if self.node(arg_nid).kind == SyntaxKind::NamedArg {
                    let span = self.node(arg_nid).span;
                    let _ = self.problem(
                        ErrorCode::CallArguments,
                        span,
                        "a function is called with named fields",
                        Some("Arguments are passed by position (reference 5.2)."),
                        None,
                    );
                    return InferValue::none();
                }
                let inp = self.infer_expression(arg_nid, None);
                if inp.ty.is_none() {
                    return InferValue::none();
                }
                out.never = out.never || inp.never;
                inputs.push(inp);
            }
            self.dispatches[did.index()].inputs = inputs;
            return out;
        }
        let mut resolved = self.member(receiver.clone(), name, called);
        let Some(res_ty) = resolved.ty else {
            return resolved;
        };
        if !*called {
            return resolved;
        }
        if let (Some(args_nid), Some(site_nid)) = (args, site)
            && self.node(args_nid).children.is_empty()
            && self.receiver_only(res_ty)
            && !self.allows_empty_dot_parens(&resolved)
            && !self.empty_dot_call(site_nid)
        {
            return InferValue::none();
        }
        if self.checked && resolved.symbol.is_some() {
            if !self.record_syntax(name, resolved.clone()) {
                return InferValue::none();
            }
            resolved.note = self.notes_tail;
        }
        self.infer_call(resolved, args, Some(&receiver))
    }

    pub(crate) fn simulation_call(&mut self, name: NodeId, args: Option<NodeId>) -> InferValue {
        let nbytes = self.node_text(name);
        let Some(sim_info) = SIMOPS.iter().find(|op| op.name.as_bytes() == nbytes) else {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::NameUnknown,
                span,
                UNKNOWN_MEMBER_MSG,
                Some(
                    "Simulation's operations are start, send, advance, crash and restart \
                     (reference 12.4).",
                ),
                None,
            );
            return InferValue::none();
        };
        let Some(args_nid) = args else {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                "a simulation operation needs a direct call",
                Some(
                    "Simulation operations are not values; call one directly, as in \
                     Simulation.crash(sim) (reference 12.4).",
                ),
                None,
            );
            if let Some(err) = self.error.as_mut()
                && err.code == ErrorCode::CallArguments
                && err.primary.span.start == span.start
            {
                err.primary.text = Some("call this operation directly".into());
            }
            return InferValue::none();
        };
        let op = sim_info.op;
        if op == SimOpId::Start && !self.spec {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::FakeOutsideSpec,
                span,
                "a fake capability is made only in a test, example or promise",
                Some(
                    "Fakes are made only in tests, examples and promises, never in a module's \
                     constants or its functions (reference 12.4).",
                ),
                None,
            );
            return InferValue::none();
        }
        let arg_children = self.node(args_nid).children.clone();
        let arg_count = arg_children.len();
        let selector = match op {
            SimOpId::Start => Some(0usize),
            SimOpId::Send => Some(1usize),
            SimOpId::Advance | SimOpId::Crash | SimOpId::Restart => None,
        };
        let mut count = match op {
            SimOpId::Start | SimOpId::Advance => 2usize,
            SimOpId::Send | SimOpId::Crash | SimOpId::Restart => 1usize,
        };
        let bad_args = |this: &mut Self| -> InferValue {
            let reason = match op {
                SimOpId::Start => "Simulation.start takes a service module and a start Time.",
                SimOpId::Send => {
                    "Simulation.send takes the handle, Module.tool, then the tool's arguments \
                     after State and Time, which send supplies."
                }
                SimOpId::Advance => "Simulation.advance takes the handle and a Duration.",
                SimOpId::Crash => "Simulation.crash takes only the handle.",
                SimOpId::Restart => "Simulation.restart takes only the handle.",
            };
            let span = this.node(args_nid).span;
            let _ = this.problem(
                ErrorCode::CallArguments,
                span,
                "a simulation call's arguments don't match its parameters",
                Some(reason),
                None,
            );
            InferValue::none()
        };
        if (op == SimOpId::Send && arg_count < 2) || (op != SimOpId::Send && arg_count != count) {
            return bad_args(self);
        }

        let mut service_index = 0usize;
        let mut tool_index = 0usize;
        let mut sig_args_opt: Option<Vec<TypeId>> = None;

        if let Some(sel_idx) = selector {
            let mut prefix = arg_children.get(sel_idx).copied().flatten();
            let mut tool_nid = None;
            if op == SimOpId::Send
                && let Some(pnid) = prefix
            {
                let pnode = self.node(pnid);
                if pnode.children.len() == 2
                    && matches!(pnode.kind, SyntaxKind::FieldAccess | SyntaxKind::Qualified)
                {
                    tool_nid = pnode.children[1];
                    prefix = pnode.children[0];
                } else if pnode.kind == SyntaxKind::Postfix
                    && pnode.children.len() == 2
                    && let Some(c1) = pnode.children[1]
                    && matches!(
                        self.node(c1).kind,
                        SyntaxKind::FieldAccess | SyntaxKind::Qualified
                    )
                {
                    tool_nid = self.node(c1).children.first().copied().flatten();
                    prefix = pnode.children[0];
                } else {
                    prefix = None;
                }
            }
            let mut target_mod = None;
            let mut alias = None;
            if let Some(pnid) = prefix
                && self.node(pnid).kind == SyntaxKind::CapName
            {
                let own_mod = self.enclosing.module;
                let own_scope = self.interfaces.scope(own_mod);
                let pbytes = self.node_text(pnid);
                let current = if let Some(ref own_name) = own_scope.name {
                    own_name.as_bytes() == pbytes
                } else if let Some(hdr_path) = self
                    .modules
                    .module(own_mod)
                    .syntax
                    .header
                    .as_ref()
                    .map(|h| h.path)
                {
                    let hnode = self.node(hdr_path);
                    hnode.children.len() == 1
                        && hnode.children[0].is_some_and(|c0| self.node_text(c0) == pbytes)
                } else {
                    false
                };
                if current {
                    target_mod = Some(own_mod);
                } else {
                    let (tm, al) = self.prefix_module(pnid);
                    target_mod = tm;
                    alias = al;
                }
            }
            let mut found_svc = false;
            let mut named_actor = false;
            if let (Some(tmod), Some(view)) = (target_mod, self.services_view) {
                for (i, svc) in view.services.iter().enumerate() {
                    if !self.work() {
                        return InferValue::none();
                    }
                    if svc.module == tmod {
                        if svc.is_actor {
                            named_actor = true;
                            break;
                        }
                        found_svc = true;
                        service_index = i;
                        break;
                    }
                }
            }
            if found_svc
                && op == SimOpId::Send
                && let (Some(tnid), Some(view)) = (tool_nid, self.services_view)
            {
                let svc_mod = view.services[service_index].module;
                let tool_count = view.services[service_index].tools.len();
                for ti in 0..tool_count {
                    if !self.work() {
                        return InferValue::none();
                    }
                    let cand_nid = view.services[service_index].tools[ti].name;
                    let cand_bytes = self
                        .modules
                        .module(svc_mod)
                        .syntax
                        .tree
                        .node_text(cand_nid)
                        .unwrap_or(&[]);
                    if self.node_text(tnid) == cand_bytes {
                        tool_index = ti;
                        let fn_id = view.services[service_index].tools[ti].function;
                        let sig_id = view.function_sigs[fn_id.index()];
                        if let Some(sig_ty) = self.signatures.functions[sig_id.index()].ty {
                            let sargs = self.signatures.types.get(sig_ty).arguments.clone();
                            count = sargs.len() - 1;
                            sig_args_opt = Some(sargs);
                        }
                        break;
                    }
                }
            }
            if !found_svc || (op == SimOpId::Send && sig_args_opt.is_none()) {
                let start = op == SimOpId::Start;
                let span = prefix.map_or_else(|| self.node(args_nid).span, |n| self.node(n).span);
                let _ = self.problem(
                    ErrorCode::ServiceHandler,
                    span,
                    if start {
                        "Simulation.start must name a service module"
                    } else {
                        "Simulation.send must name a registered tool"
                    },
                    Some(if named_actor {
                        "That module declares an actor, not a service; use Actor.start and actor \
                         dot calls or Actor.send instead (reference 11.7)."
                    } else if start {
                        "Name the current module or one it uses directly, as in \
                         Simulation.start(TaskQueue, at); that module must declare a service \
                         (reference 12.4)."
                    } else {
                        "Write the service module and one of its tools directly, as in \
                         TaskQueue.submit; timers and other functions are not tools \
                         (reference 12.4)."
                    }),
                    None,
                );
                return InferValue::none();
            }
            if let Some(pnid) = prefix
                && !self.use_alias(alias, pnid)
            {
                return InferValue::none();
            }
        }
        if arg_count != count {
            return bad_args(self);
        }
        let result_ty = match op {
            SimOpId::Start => self.native("Simulation", 0),
            SimOpId::Send => self.native("Result", 2),
            SimOpId::Advance | SimOpId::Crash | SimOpId::Restart => self.native("Unit", 0),
        };
        let Some(result_ty) = result_ty else {
            return InferValue::none();
        };
        if op == SimOpId::Send {
            let Some(ref sargs) = sig_args_opt else {
                return InferValue::none();
            };
            let Some(ok_t) = self.annotation(sargs[sargs.len() - 1], None, 0) else {
                return InferValue::none();
            };
            let Some(err_t) = self.native("SimulationError", 0) else {
                return InferValue::none();
            };
            if let Some(args_mut) = self.terms[result_ty.index()].arguments.as_mut() {
                args_mut[0] = ok_t;
                args_mut[1] = err_t;
            }
        }
        let emit_count = count - usize::from(selector.is_some());
        let mut out = self.graph(InferValue::with_type(result_ty), emit_count);
        if out.ty.is_none() {
            return out;
        }
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.native = Some(format!("{}.{}", sim_info.module, sim_info.name));
            g.service = Some(CheckedServiceId(service_index as u32));
            g.tool = Some(tool_index as u32);
        }
        let mut emitted_idx = 0usize;
        for (i, ch) in arg_children.iter().enumerate().take(count) {
            if selector == Some(i) {
                continue;
            }
            let Some(arg_nid) = *ch else {
                return InferValue::none();
            };
            let wanted = if op == SimOpId::Start {
                self.native("Time", 0)
            } else if i == 0 {
                self.native("Simulation", 0)
            } else if op == SimOpId::Advance {
                self.native("Duration", 0)
            } else if let Some(ref sargs) = sig_args_opt {
                self.annotation(sargs[i], None, 0)
            } else {
                None
            };
            let Some(wanted_ty) = wanted else {
                return InferValue::none();
            };
            let input = self.infer_as(arg_nid, Some(wanted_ty), Some(ErrorCode::ArgumentType));
            if input.ty.is_none() {
                return InferValue::none();
            }
            if let Some(gid) = out.graph {
                self.emitted[gid.index()].operands[emitted_idx] = input.note;
            }
            emitted_idx += 1;
            out.never = out.never || input.never;
        }
        out
    }

    fn resolve_actor_type_arg(&mut self, arg: NodeId) -> Option<SymRef> {
        let n = self.node(arg);
        if n.kind == SyntaxKind::CapName {
            return self.definition(arg, None).map(|(s, _)| s);
        }
        if n.kind == SyntaxKind::Qualified {
            let p = n.children.first().copied().flatten()?;
            let c = n.children.get(1).copied().flatten()?;
            return self.definition(c, Some(p)).map(|(s, _)| s);
        }
        if n.kind == SyntaxKind::Postfix
            && n.children.len() == 2
            && let Some(c0) = n.children[0]
            && let Some(c1) = n.children[1]
            && self.node(c0).kind == SyntaxKind::CapName
            && self.node(c1).kind == SyntaxKind::Qualified
        {
            let inner = self.node(c1).children.first().copied().flatten()?;
            return self.definition(inner, Some(c0)).map(|(s, _)| s);
        }
        None
    }

    pub(crate) fn actor_call(&mut self, name: NodeId, args: Option<NodeId>) -> InferValue {
        const OPS: [(&str, &str); 6] = [
            ("start", "Actor.start"),
            ("start_at", "Actor.start_at"),
            ("send", "Actor.send"),
            ("advance", "Actor.advance"),
            ("crash", "Actor.crash"),
            ("restart", "Actor.restart"),
        ];
        let nbytes = self.node_text(name);
        let Some(op) = OPS.iter().position(|(m, _)| m.as_bytes() == nbytes) else {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::NameUnknown,
                span,
                UNKNOWN_MEMBER_MSG,
                Some(
                    "Actor's operations are start, start_at, send, advance, crash and restart \
                     (reference 11.7).",
                ),
                None,
            );
            return InferValue::none();
        };
        let Some(args_nid) = args else {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                "an Actor operation needs a direct call",
                Some(
                    "Actor operations are not values; call one directly, as in \
                     Actor.start(RateLimiter) (reference 11.7).",
                ),
                None,
            );
            return InferValue::none();
        };
        // op == 2 is Actor.send
        if op != 2 && !self.spec {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::FakeOutsideSpec,
                span,
                "a fake capability is made only in a test, example or promise",
                Some(
                    "Actor test controls (start, start_at, advance, crash, restart) are allowed \
                     only in tests, examples and promises (reference 11.7, 12.4).",
                ),
                None,
            );
            return InferValue::none();
        }
        let arg_children = self.node(args_nid).children.clone();
        let arg_count = arg_children.len();

        // start (0) / start_at (1)
        if op == 0 || op == 1 {
            let expected_argc = if op == 0 { 1 } else { 2 };
            if arg_count != expected_argc {
                let span = self.node(args_nid).span;
                let _ = self.problem(
                    ErrorCode::CallArguments,
                    span,
                    "an Actor call's arguments don't match its parameters",
                    Some(if op == 0 {
                        "Actor.start takes an actor type name."
                    } else {
                        "Actor.start_at takes an actor type name and a start Time."
                    }),
                    None,
                );
                return InferValue::none();
            }
            let Some(a0) = arg_children[0] else {
                return InferValue::none();
            };
            let actor_sym = self.resolve_actor_type_arg(a0);
            if self.failed {
                return InferValue::none();
            }
            let mut service_index = None;
            if let (Some(asym), Some(view)) = (actor_sym, self.services_view)
                && self.interfaces.symbol(asym).decl_kind == SyntaxKind::Actor
            {
                for (i, svc) in view.services.iter().enumerate() {
                    if !self.work() {
                        return InferValue::none();
                    }
                    if svc.is_actor && svc.actor_symbol == Some(asym) {
                        service_index = Some(i);
                        break;
                    }
                }
            }
            let (Some(svc_idx), Some(asym)) = (service_index, actor_sym) else {
                let span = self.node(a0).span;
                let _ = self.problem(
                    ErrorCode::ActorBlock,
                    span,
                    "Actor.start must name a visible actor type",
                    Some(
                        "Write Actor.start(RateLimiter) or Actor.start(Mod.RateLimiter) naming an \
                         actor declared in scope (reference 11.7).",
                    ),
                    None,
                );
                return InferValue::none();
            };
            let nom_ty = self
                .signatures
                .nominal_signature_by_symbol(asym)
                .map(|(_, n)| n.ty);
            let Some(nom_ty_id) = nom_ty else {
                return InferValue::none();
            };
            let Some(actor_term) = self.annotation(nom_ty_id, None, 0) else {
                return InferValue::none();
            };
            let emit_argc = if op == 0 { 0 } else { 1 };
            let mut out = self.graph(InferValue::with_type(actor_term), emit_argc);
            if out.ty.is_none() {
                return out;
            }
            if let Some(gid) = out.graph {
                self.emitted[gid.index()].native = Some(OPS[op].1.to_string());
                self.emitted[gid.index()].service = Some(CheckedServiceId(svc_idx as u32));
            }
            if op == 1 {
                let Some(a1) = arg_children[1] else {
                    return InferValue::none();
                };
                let Some(time_ty) = self.native("Time", 0) else {
                    return InferValue::none();
                };
                let tval = self.infer_as(a1, Some(time_ty), Some(ErrorCode::ArgumentType));
                if tval.ty.is_none() {
                    return InferValue::none();
                }
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[0] = tval.note;
                }
                out.never = tval.never;
            }
            return out;
        }

        // send (2), advance (3), crash (4), restart (5)
        if (op == 2 && arg_count < 2)
            || (op == 3 && arg_count != 2)
            || ((op == 4 || op == 5) && arg_count != 1)
        {
            let span = self.node(args_nid).span;
            let reason = match op {
                2 => "Actor.send takes the actor handle, ActorType.call, and the call's arguments.",
                3 => "Actor.advance takes the actor handle and a Duration.",
                _ => "Actor.crash and Actor.restart take only the actor handle.",
            };
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                "an Actor call's arguments don't match its parameters",
                Some(reason),
                None,
            );
            return InferValue::none();
        }
        let Some(a0) = arg_children[0] else {
            return InferValue::none();
        };
        let handle = self.infer_expression(a0, None);
        let Some(h_ty) = handle.ty else {
            return InferValue::none();
        };
        let hroot = self.root(h_ty);
        let h_owner = {
            let t = &self.terms[hroot.index()];
            if !t.variable && t.shape.kind == SourceTypeKind::Named {
                t.shape.owner.as_source()
            } else {
                None
            }
        };
        let mut service_index = None;
        if let (Some(hos), Some(view)) = (h_owner, self.services_view)
            && self.interfaces.symbol(hos).decl_kind == SyntaxKind::Actor
        {
            for (i, svc) in view.services.iter().enumerate() {
                if !self.work() {
                    return InferValue::none();
                }
                if svc.is_actor && svc.actor_symbol == Some(hos) {
                    service_index = Some(i);
                    break;
                }
            }
        }
        let Some(svc_idx) = service_index else {
            let span = self.node(a0).span;
            let _ = self.problem(
                ErrorCode::ArgumentType,
                span,
                "Actor operations require an actor handle as the first argument",
                Some("Pass a value whose type is a declared actor (reference 11.7)."),
                None,
            );
            return InferValue::none();
        };

        if op != 2 {
            let Some(unit_ty) = self.native("Unit", 0) else {
                return InferValue::none();
            };
            let emit_argc = if op == 3 { 2 } else { 1 };
            let mut out = self.graph(
                InferValue {
                    ty: Some(unit_ty),
                    never: handle.never,
                    ..InferValue::default()
                },
                emit_argc,
            );
            if out.ty.is_none() {
                return out;
            }
            if let Some(gid) = out.graph {
                let g = &mut self.emitted[gid.index()];
                g.native = Some(OPS[op].1.to_string());
                g.service = Some(CheckedServiceId(svc_idx as u32));
                g.operands[0] = handle.note;
            }
            if op == 3 {
                let Some(a1) = arg_children[1] else {
                    return InferValue::none();
                };
                let Some(dur_ty) = self.native("Duration", 0) else {
                    return InferValue::none();
                };
                let dval = self.infer_as(a1, Some(dur_ty), Some(ErrorCode::ArgumentType));
                if dval.ty.is_none() {
                    return InferValue::none();
                }
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[1] = dval.note;
                }
                out.never = out.never || dval.never;
            }
            return out;
        }

        // Actor.send(handle, Prefix.call_name, ...args)
        let Some(sel) = arg_children[1] else {
            return InferValue::none();
        };
        let (prefix, call_name) = {
            let sn = self.node(sel);
            if sn.children.len() == 2
                && matches!(sn.kind, SyntaxKind::FieldAccess | SyntaxKind::Qualified)
            {
                (sn.children[0], sn.children[1])
            } else if sn.kind == SyntaxKind::Postfix
                && sn.children.len() == 2
                && let Some(c1) = sn.children[1]
                && matches!(
                    self.node(c1).kind,
                    SyntaxKind::FieldAccess | SyntaxKind::Qualified
                )
            {
                (
                    sn.children[0],
                    self.node(c1).children.first().copied().flatten(),
                )
            } else {
                (None, None)
            }
        };
        let Some(view) = self.services_view else {
            return InferValue::none();
        };
        let svc_mod = view.services[svc_idx].module;
        let actor_sym = view.services[svc_idx].actor_symbol;
        let mut prefix_matches = false;
        if let Some(pnid) = prefix {
            let pkind = self.node(pnid).kind;
            if pkind == SyntaxKind::CapName {
                let own_mod = self.enclosing.module;
                let own_scope = self.interfaces.scope(own_mod);
                let pbytes = self.node_text(pnid);
                let current_mod = own_mod == svc_mod
                    && (own_scope.name.as_deref().map(str::as_bytes) == Some(pbytes)
                        || self
                            .modules
                            .module(own_mod)
                            .syntax
                            .header
                            .as_ref()
                            .map(|h| h.path)
                            .is_some_and(|hp| {
                                let hn = self.node(hp);
                                hn.children.len() == 1
                                    && hn.children[0].is_some_and(|c0| self.node_text(c0) == pbytes)
                            }));
                let same_actor_name = svc_mod == own_mod
                    && actor_sym
                        .is_some_and(|asym| self.interfaces.symbol(asym).name.as_bytes() == pbytes);
                if current_mod || same_actor_name {
                    prefix_matches = true;
                } else {
                    let (mod_target, alias) = self.prefix_module(pnid);
                    if mod_target == Some(svc_mod) {
                        if !self.use_alias(alias, pnid) {
                            return InferValue::none();
                        }
                        prefix_matches = true;
                    } else {
                        let def = self.definition(pnid, None);
                        if self.failed {
                            return InferValue::none();
                        }
                        if def.map(|(s, _)| s) == actor_sym {
                            prefix_matches = true;
                        }
                    }
                }
            } else if pkind == SyntaxKind::Qualified
                || (pkind == SyntaxKind::Postfix
                    && self.node(pnid).children.len() == 2
                    && self.node(pnid).children[0]
                        .is_some_and(|c0| self.node(c0).kind == SyntaxKind::CapName)
                    && self.node(pnid).children[1]
                        .is_some_and(|c1| self.node(c1).kind == SyntaxKind::Qualified))
            {
                let sym = self.resolve_actor_type_arg(pnid);
                if self.failed {
                    return InferValue::none();
                }
                if sym == actor_sym {
                    prefix_matches = true;
                }
            }
        }

        let mut tool_index = None;
        let mut sig_args_opt: Option<Vec<TypeId>> = None;
        let mut reply_kind = ServiceReply::ReadOnly;
        if prefix_matches && let Some(cnid) = call_name {
            let tool_count = view.services[svc_idx].tools.len();
            for ti in 0..tool_count {
                if !self.work() {
                    return InferValue::none();
                }
                let cand_nid = view.services[svc_idx].tools[ti].name;
                let cand_bytes = self
                    .modules
                    .module(svc_mod)
                    .syntax
                    .tree
                    .node_text(cand_nid)
                    .unwrap_or(&[]);
                if self.node_text(cnid) == cand_bytes {
                    tool_index = Some(ti);
                    let fn_id = view.services[svc_idx].tools[ti].function;
                    reply_kind = view.services[svc_idx].tools[ti].reply;
                    let sig_id = view.function_sigs[fn_id.index()];
                    if let Some(sig_ty) = self.signatures.functions[sig_id.index()].ty {
                        sig_args_opt = Some(self.signatures.types.get(sig_ty).arguments.clone());
                    }
                    break;
                }
            }
        }
        let (Some(tidx), Some(sig_args)) = (tool_index, sig_args_opt) else {
            let span = self.node(sel).span;
            let _ = self.problem(
                ErrorCode::ActorBlock,
                span,
                "Actor.send must name one of the actor's calls",
                Some(
                    "Write ActorType.call_name or Module.call_name matching the handle's actor \
                     type (reference 11.7).",
                ),
                None,
            );
            return InferValue::none();
        };
        let expected_total = sig_args.len() - 1;
        if arg_count != expected_total {
            let span = self.node(args_nid).span;
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                "an Actor.send call's arguments don't match its parameters",
                Some(
                    "Actor.send takes the handle, ActorType.call, then the call's arguments after \
                     State and Time.",
                ),
                None,
            );
            return InferValue::none();
        }
        let raw_ret = sig_args[sig_args.len() - 1];
        let proj_reply = match reply_kind {
            ServiceReply::ReadOnly => self.annotation(raw_ret, None, 0),
            ServiceReply::Update => {
                let r1 = self.signatures.types.get(raw_ret).arguments[1];
                self.annotation(r1, None, 0)
            }
            ServiceReply::ResultUpdate => {
                let res_args = self.signatures.types.get(raw_ret).arguments.clone();
                let ok_pair_1 = self.signatures.types.get(res_args[0]).arguments[1];
                let err_ty = res_args[1];
                if let Some(rt) = self.native("Result", 2)
                    && let Some(a0) = self.annotation(ok_pair_1, None, 0)
                    && let Some(a1) = self.annotation(err_ty, None, 0)
                {
                    if let Some(args_mut) = self.terms[rt.index()].arguments.as_mut() {
                        args_mut[0] = a0;
                        args_mut[1] = a1;
                    }
                    Some(rt)
                } else {
                    None
                }
            }
        };
        let Some(proj_t) = proj_reply else {
            return InferValue::none();
        };
        let Some(res_term) = self.native("Result", 2) else {
            return InferValue::none();
        };
        let Some(sim_err) = self.native("SimulationError", 0) else {
            return InferValue::none();
        };
        if let Some(args_mut) = self.terms[res_term.index()].arguments.as_mut() {
            args_mut[0] = proj_t;
            args_mut[1] = sim_err;
        }
        let emit_argc = expected_total - 1;
        let mut out = self.graph(
            InferValue {
                ty: Some(res_term),
                never: handle.never,
                ..InferValue::default()
            },
            emit_argc,
        );
        if out.ty.is_none() {
            return out;
        }
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.native = Some("Actor.send".to_string());
            g.service = Some(CheckedServiceId(svc_idx as u32));
            g.tool = Some(tidx as u32);
            g.operands[0] = handle.note;
        }
        for i in 2..expected_total {
            let Some(anid) = arg_children[i] else {
                return InferValue::none();
            };
            let Some(wanted) = self.annotation(sig_args[i], None, 0) else {
                return InferValue::none();
            };
            let aval = self.infer_as(anid, Some(wanted), Some(ErrorCode::ArgumentType));
            if aval.ty.is_none() {
                return InferValue::none();
            }
            if let Some(gid) = out.graph {
                self.emitted[gid.index()].operands[i - 1] = aval.note;
            }
            out.never = out.never || aval.never;
        }
        out
    }

    fn find_lib_type(&self, mod_name: &str, name: &str) -> Option<TypeId> {
        for i in 0..self.interfaces.len() {
            let mid = ModuleId(i as u32);
            let scope = self.interfaces.scope(mid);
            if !scope.library || scope.name.as_deref() != Some(mod_name) {
                continue;
            }
            let idx = scope.find_symbol_index(name.as_bytes())?;
            let sym_ref = SymRef::new(mid, idx);
            return self
                .signatures
                .nominal_signature_by_symbol(sym_ref)
                .map(|(_, n)| n.ty);
        }
        None
    }

    pub(crate) fn apitest_call(&mut self, name: NodeId, args: Option<NodeId>) -> InferValue {
        let nbytes = self.node_text(name);
        let is_start = nbytes == b"start";
        let is_send = nbytes == b"send";
        let is_job = nbytes == b"run_job";
        if !is_start && !is_send && !is_job {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::NameUnknown,
                span,
                UNKNOWN_MEMBER_MSG,
                Some(
                    "ApiTest's operations are start(Module, ...capabilities), \
                     send(Module, req, ...capabilities) and \
                     run_job(Module, name, payload, ...capabilities).",
                ),
                None,
            );
            return InferValue::none();
        }
        let Some(args_nid) = args else {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                if is_start {
                    "ApiTest.start needs a direct call"
                } else if is_send {
                    "ApiTest.send needs a direct call"
                } else {
                    "ApiTest.run_job needs a direct call"
                },
                Some(if is_start {
                    "Call ApiTest.start(Module) directly."
                } else if is_send {
                    "Call ApiTest.send(Module, req) directly."
                } else {
                    "Call ApiTest.run_job(Module, name, payload) directly."
                }),
                None,
            );
            return InferValue::none();
        };
        if !self.spec {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::FakeOutsideSpec,
                span,
                "a fake capability is made only in a test, example or promise",
                Some(
                    "ApiTest runs only in tests, examples and promises, never in a module's \
                     constants or its functions (reference 12.4).",
                ),
                None,
            );
            return InferValue::none();
        }
        let min_args = if is_job {
            3usize
        } else if is_send {
            2usize
        } else {
            1usize
        };
        let arg_children = self.node(args_nid).children.clone();
        let arg_count = arg_children.len();
        if arg_count < min_args {
            let span = self.node(args_nid).span;
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                if is_start {
                    "ApiTest.start needs a module"
                } else if is_send {
                    "ApiTest.send needs a module and a Request"
                } else {
                    "ApiTest.run_job needs a module, job name and Json payload"
                },
                Some(if is_start {
                    "Write ApiTest.start(Module) with optional fake capabilities."
                } else if is_send {
                    "Write ApiTest.send(Module, req) with optional fake capabilities."
                } else {
                    "Write ApiTest.run_job(Module, \"job.name\", payload) with optional fake \
                     capabilities."
                }),
                None,
            );
            return InferValue::none();
        }
        let Some(prefix) = arg_children[0] else {
            return InferValue::none();
        };
        let mut target_mod = None;
        let mut alias = None;
        if self.node(prefix).kind == SyntaxKind::CapName {
            let own_mod = self.enclosing.module;
            let own_scope = self.interfaces.scope(own_mod);
            let pbytes = self.node_text(prefix);
            let current = if let Some(ref own_name) = own_scope.name {
                own_name.as_bytes() == pbytes
            } else if let Some(hp) = self
                .modules
                .module(own_mod)
                .syntax
                .header
                .as_ref()
                .map(|h| h.path)
            {
                let hn = self.node(hp);
                hn.children.len() == 1
                    && hn.children[0].is_some_and(|c0| self.node_text(c0) == pbytes)
            } else {
                false
            };
            if current {
                target_mod = Some(own_mod);
            } else {
                let (tm, al) = self.prefix_module(prefix);
                target_mod = tm;
                alias = al;
            }
        }
        let mut api_index = None;
        if let (Some(tmod), Some(view)) = (target_mod, self.services_view) {
            for (i, api) in view.apis.iter().enumerate() {
                if !self.work() {
                    return InferValue::none();
                }
                if api.module == tmod {
                    api_index = Some(i);
                    break;
                }
            }
        }
        let Some(api_idx) = api_index else {
            let span = self.node(prefix).span;
            let _ = self.problem(
                ErrorCode::ApiBlock,
                span,
                if is_start {
                    "ApiTest.start must name an api module"
                } else if is_send {
                    "ApiTest.send must name an api module"
                } else {
                    "ApiTest.run_job must name an api module"
                },
                Some(
                    "Name the current module or one it uses directly, and that module must \
                     declare an api block.",
                ),
                None,
            );
            return InferValue::none();
        };
        if !self.use_alias(alias, prefix) {
            return InferValue::none();
        }

        let mut out = if is_start {
            if self.find_lib_type("Api", "Request").is_none()
                || self.find_lib_type("Api", "Response").is_none()
                || self.find_lib_type("Api", "RouteSpec").is_none()
                || self.find_lib_type("Json", "Json").is_none()
            {
                let span = self.node(name).span;
                let _ = self.problem(
                    ErrorCode::ImportMissing,
                    span,
                    "ApiTest.start needs the Api and Json library modules",
                    Some("Load the program with the standard library enabled."),
                    None,
                );
                return InferValue::none();
            }
            let Some(app_term) = self.native("ApiTest.App", 0) else {
                return InferValue::none();
            };
            let o = self.graph(InferValue::with_type(app_term), arg_count - 1);
            if o.ty.is_none() {
                return o;
            }
            if let Some(gid) = o.graph {
                self.emitted[gid.index()].native = Some("ApiTest.start".to_string());
                self.emitted[gid.index()].service = Some(CheckedServiceId(api_idx as u32));
            }
            o
        } else if is_send {
            let Some(req_src) = self.find_lib_type("Api", "Request") else {
                let span = self.node(name).span;
                let _ = self.problem(
                    ErrorCode::ImportMissing,
                    span,
                    "ApiTest.send needs the Api library module",
                    Some("Bring Api into scope with use Api."),
                    None,
                );
                return InferValue::none();
            };
            let Some(res_src) = self.find_lib_type("Api", "Response") else {
                let span = self.node(name).span;
                let _ = self.problem(
                    ErrorCode::ImportMissing,
                    span,
                    "ApiTest.send needs the Api library module",
                    Some("Bring Api into scope with use Api."),
                    None,
                );
                return InferValue::none();
            };
            let Some(req_term) = self.annotation(req_src, None, 0) else {
                return InferValue::none();
            };
            let Some(res_term) = self.annotation(res_src, None, 0) else {
                return InferValue::none();
            };
            let mut o = self.graph(InferValue::with_type(res_term), arg_count - 1);
            if o.ty.is_none() {
                return o;
            }
            if let Some(gid) = o.graph {
                self.emitted[gid.index()].native = Some("ApiTest.send".to_string());
                self.emitted[gid.index()].service = Some(CheckedServiceId(api_idx as u32));
            }
            let Some(a1) = arg_children[1] else {
                return InferValue::none();
            };
            let req_val = self.infer_as(a1, Some(req_term), Some(ErrorCode::ArgumentType));
            if req_val.ty.is_none() {
                return InferValue::none();
            }
            if let Some(gid) = o.graph {
                self.emitted[gid.index()].operands[0] = req_val.note;
            }
            o.never = req_val.never;
            o
        } else {
            let Some(json_src) = self.find_lib_type("Json", "Json") else {
                let span = self.node(name).span;
                let _ = self.problem(
                    ErrorCode::ImportMissing,
                    span,
                    "ApiTest.run_job needs the Json library module",
                    Some("Bring Json into scope with use Json."),
                    None,
                );
                return InferValue::none();
            };
            let Some(text_term) = self.native("Text", 0) else {
                return InferValue::none();
            };
            let Some(unit_term) = self.native("Unit", 0) else {
                return InferValue::none();
            };
            let Some(json_term) = self.annotation(json_src, None, 0) else {
                return InferValue::none();
            };
            let Some(res_term) = self.native("Result", 2) else {
                return InferValue::none();
            };
            if let Some(args_mut) = self.terms[res_term.index()].arguments.as_mut() {
                args_mut[0] = unit_term;
                args_mut[1] = text_term;
            }
            let mut o = self.graph(InferValue::with_type(res_term), arg_count - 1);
            if o.ty.is_none() {
                return o;
            }
            if let Some(gid) = o.graph {
                self.emitted[gid.index()].native = Some("ApiTest.run_job".to_string());
                self.emitted[gid.index()].service = Some(CheckedServiceId(api_idx as u32));
            }
            let Some(a1) = arg_children[1] else {
                return InferValue::none();
            };
            let Some(a2) = arg_children[2] else {
                return InferValue::none();
            };
            let name_val = self.infer_as(a1, Some(text_term), Some(ErrorCode::ArgumentType));
            if name_val.ty.is_none() {
                return InferValue::none();
            }
            let payload_val = self.infer_as(a2, Some(json_term), Some(ErrorCode::ArgumentType));
            if payload_val.ty.is_none() {
                return InferValue::none();
            }
            if let Some(gid) = o.graph {
                self.emitted[gid.index()].operands[0] = name_val.note;
                self.emitted[gid.index()].operands[1] = payload_val.note;
            }
            o.never = name_val.never || payload_val.never;
            o
        };

        const ALLOWED_CAPS: [&str; 10] = [
            "Sqlite",
            "Network",
            "Clock",
            "Random",
            "Files",
            "ReadFiles",
            "Env",
            "Console",
            "Command",
            "SecureRandom",
        ];
        let mut seen_caps = 0u32;
        let mut seen_actors = 0u32;
        let Some(view) = self.services_view else {
            return InferValue::none();
        };
        let api_actors: Vec<Option<SymRef>> = view.apis[api_idx]
            .actors
            .iter()
            .map(|a| self.signatures.types.get(a.ty).owner.as_source())
            .collect();
        for (i, ch) in arg_children.iter().enumerate().skip(min_args) {
            let Some(arg_nid) = *ch else {
                return InferValue::none();
            };
            let cap_val = self.infer_expression(arg_nid, None);
            let Some(cty) = cap_val.ty else {
                return InferValue::none();
            };
            let croot = self.root(cty);
            let (c_var, c_kind, c_mod, c_owner, c_name) = {
                let t = &self.terms[croot.index()];
                (
                    t.variable,
                    t.shape.kind,
                    t.shape.module,
                    t.shape.owner,
                    t.shape.name.clone(),
                )
            };
            if !c_var
                && c_kind == SourceTypeKind::Named
                && let Some(owner_sym) = c_owner.as_source()
                && self.interfaces.symbol(owner_sym).decl_kind == SyntaxKind::Actor
            {
                let actor_slot = api_actors.iter().position(|&a| a == Some(owner_sym));
                let Some(aslot) = actor_slot else {
                    let span = self.node(arg_nid).span;
                    let _ = self.problem(
                        ErrorCode::ArgumentType,
                        span,
                        "ApiTest.send passes an actor not declared by this api",
                        Some(
                            "Declare actor <name>: <ActorType> in the api block to pass that \
                             actor handle.",
                        ),
                        None,
                    );
                    return InferValue::none();
                };
                if (seen_actors & (1u32 << aslot)) != 0 {
                    let span = self.node(arg_nid).span;
                    let _ = self.problem(
                        ErrorCode::CallArguments,
                        span,
                        "ApiTest.send passes the same actor type twice",
                        Some("Pass at most one handle of each declared actor type."),
                        None,
                    );
                    return InferValue::none();
                }
                seen_actors |= 1u32 << aslot;
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].operands[i - 1] = cap_val.note;
                }
                out.never = out.never || cap_val.never;
                continue;
            }
            let slot = if !c_var && c_kind == SourceTypeKind::Named && c_mod.is_none() {
                ALLOWED_CAPS.iter().position(|&c| c == c_name)
            } else {
                None
            };
            let Some(cslot) = slot else {
                let span = self.node(arg_nid).span;
                let _ = self.problem(
                    ErrorCode::ArgumentType,
                    span,
                    "ApiTest.send optional arguments must be capabilities or declared actors",
                    Some(
                        "Pass fake capabilities such as Sqlite.in_memory() or actor handles from \
                         Actor.start(...).",
                    ),
                    None,
                );
                return InferValue::none();
            };
            if (seen_caps & (1u32 << cslot)) != 0 {
                let span = self.node(arg_nid).span;
                let _ = self.problem(
                    ErrorCode::CallArguments,
                    span,
                    "ApiTest.send passes the same capability type twice",
                    Some("Pass at most one capability of each type."),
                    None,
                );
                return InferValue::none();
            }
            seen_caps |= 1u32 << cslot;
            if let Some(gid) = out.graph {
                self.emitted[gid.index()].operands[i - 1] = cap_val.note;
            }
            out.never = out.never || cap_val.never;
        }
        out
    }

    pub(crate) fn infer_sequence(&mut self, node: NodeId, expected: Option<TermId>) -> InferValue {
        let children = self.node(node).children.clone();
        let Some(unit_ty) = self.native("Unit", 0) else {
            return InferValue::none();
        };
        let mut last = InferValue::with_type(unit_ty);
        let emitted = self.graph(last.clone(), children.len());
        if emitted.ty.is_none() {
            return InferValue::none();
        }
        let mut never = false;
        for (i, ch) in children.iter().enumerate() {
            let Some(stmt_nid) = *ch else {
                return InferValue::none();
            };
            let step_expected = if i + 1 == children.len() && !never {
                expected
            } else {
                None
            };
            last = self.infer_as(stmt_nid, step_expected, self.node_expectation);
            if last.ty.is_none() {
                return InferValue::none();
            }
            if let Some(gid) = emitted.graph {
                self.emitted[gid.index()].operands[i] = last.note;
            }
            never = never || last.never;
            if i + 1 != children.len()
                && let Some(tail) = self.notes_tail
            {
                self.notes[tail.index()].discarded = !last.never;
            }
        }
        last.never = never;
        last.graph = emitted.graph;
        last
    }

    pub(crate) fn infer_call(
        &mut self,
        callee: InferValue,
        args: Option<NodeId>,
        receiver: Option<&InferValue>,
    ) -> InferValue {
        let Some(callee_ty) = callee.ty else {
            return InferValue::none();
        };
        let arg_children = args
            .map(|a| self.node(a).children.clone())
            .unwrap_or_default();
        let count = arg_children.len();
        let mut t = self.root(callee_ty);
        if self.terms[t.index()].variable {
            let Some(fn_ty) = self.make_type(TermShape {
                kind: SourceTypeKind::Function,
                name: "fn".to_string(),
                module: None,
                owner: TypeOwner::None,
                arity: count + 1,
            }) else {
                return InferValue::none();
            };
            for i in 0..=count {
                let Some(f) = self.fresh(0) else {
                    return InferValue::none();
                };
                if let Some(args_mut) = self.terms[fn_ty.index()].arguments.as_mut() {
                    args_mut[i] = f;
                }
            }
            if !self.unify(t, fn_ty, 0) {
                return InferValue::none();
            }
            t = self.root(fn_ty);
        }
        if self.terms[t.index()].shape.kind != SourceTypeKind::Function {
            if let Some(binding_ref) = callee.binding
                && let Some(bnid) = binding_ref.node_id()
                && let Some((sym_ref, _)) = self.definition(bnid, None)
                && self.interfaces.symbol(sym_ref).decl_kind == SyntaxKind::Fn
            {
                let prev_span = binding_ref.span(self.tree());
                let _ = self.problem(
                    ErrorCode::BindRepeated,
                    self.span,
                    "a name means both a local and a module function",
                    Some(
                        "A function can't refer to a module function by a name it uses for a \
                         local (reference 6.1).",
                    ),
                    Some(prev_span),
                );
                return InferValue::none();
            }
            let Some(fn_dummy) = self.make_type(TermShape {
                kind: SourceTypeKind::Function,
                name: "fn".to_string(),
                module: None,
                owner: TypeOwner::None,
                arity: 0,
            }) else {
                return InferValue::none();
            };
            let _ = self.unify_as(t, fn_dummy, Some(ErrorCode::NotFunction));
            return InferValue::none();
        }
        let offset = usize::from(receiver.is_some());
        let fn_arity = self.terms[t.index()].shape.arity;
        if fn_arity != count + offset + 1 {
            let span = match args {
                Some(a) => {
                    let s = self.node(a).span;
                    if s.start == s.end { self.span } else { s }
                }
                None => self.span,
            };
            let _ = self.problem(
                ErrorCode::CallArguments,
                span,
                "a call's arguments don't match its parameters",
                Some(
                    "A function has no default or optional parameters; pass every argument by \
                     position (reference 5.2).",
                ),
                None,
            );
            return InferValue::none();
        }
        let mut never = callee.never;
        if let Some(recv) = receiver {
            let Some(recv_ty) = recv.ty else {
                return InferValue::none();
            };
            let Some(p0) = self.argument(t, 0) else {
                return InferValue::none();
            };
            if !self.unify_as(recv_ty, p0, Some(ErrorCode::ArgumentType)) {
                return InferValue::none();
            }
            never = never || recv.never;
        }
        for ch in &arg_children {
            let Some(anid) = *ch else {
                return InferValue::none();
            };
            if self.node(anid).kind == SyntaxKind::NamedArg {
                let span = self.node(anid).span;
                let _ = self.problem(
                    ErrorCode::CallArguments,
                    span,
                    "a function is called with named fields",
                    Some(
                        "Arguments are passed by position; only records and cases are built with \
                         named fields (reference 5.2).",
                    ),
                    None,
                );
                return InferValue::none();
            }
        }
        if !self.charge_arena(count, 16) {
            return InferValue::none();
        }
        let mut ranges = vec![
            NoteRange {
                first: None,
                last: None,
            };
            count
        ];
        let mut result = InferValue::none();
        let declared_dot = receiver.is_some() && callee.note.is_some();
        if self.checked {
            let extra = usize::from(declared_dot);
            let Some(ret_ty) = self.argument(t, fn_arity - 1) else {
                return InferValue::none();
            };
            result = self.graph(InferValue::with_type(ret_ty), count + 1 + extra);
            if result.ty.is_none() {
                return result;
            }
            let callee_emitted = callee.graph.map(|cg| self.emitted[cg.index()].clone());
            if let Some(gid) = result.graph {
                let g = &mut self.emitted[gid.index()];
                g.operands[0] = if receiver.is_none() || declared_dot {
                    callee.note
                } else {
                    receiver.and_then(|r| r.note)
                };
                if declared_dot {
                    g.operands[1] = receiver.and_then(|r| r.note);
                }
                g.symbol = callee.symbol;
                if let Some(ce) = callee_emitted {
                    g.native = ce.native;
                    g.service = ce.service;
                    g.tool = ce.tool;
                    g.function_type = ce.function_type;
                    g.requirements = ce.requirements;
                    g.parameters = ce.parameters;
                    g.json_decode = ce.json_decode;
                }
                if receiver.is_some() {
                    g.receiver_slot = Some(extra);
                }
            }
        }
        result.json_decode = callee.json_decode;
        let before = self.notes_tail;
        for pass in 0..2 {
            for (i, ch) in arg_children.iter().enumerate() {
                let Some(anid) = *ch else {
                    return InferValue::none();
                };
                if self.is_lambda_argument(anid) != (pass != 0) {
                    continue;
                }
                let previous = self.notes_tail;
                let Some(param_ty) = self.argument(t, i + offset) else {
                    return InferValue::none();
                };
                let arg = self.infer_as(anid, Some(param_ty), Some(ErrorCode::ArgumentType));
                if arg.ty.is_none() {
                    return InferValue::none();
                }
                let first = match previous {
                    None => self.notes_head,
                    Some(prev) => self.notes[prev.index()].next,
                };
                ranges[i] = NoteRange {
                    first,
                    last: self.notes_tail,
                };
                if let Some(gid) = result.graph {
                    let slot = i + if declared_dot { 2 } else { 1 };
                    self.emitted[gid.index()].operands[slot] = arg.note;
                }
                never = never || arg.never;
            }
        }
        self.order_notes(before, &ranges);
        if !self.checked {
            result.ty = self.argument(t, fn_arity - 1);
        }
        result.callee = callee.note;
        result.never = never;
        result
    }

    pub(crate) fn type_application(&mut self, callee: InferValue, args: NodeId) -> InferValue {
        if callee.ty.is_none() {
            return InferValue::none();
        }
        let Some(fn_ty_id) = callee
            .function_type
            .filter(|_| !callee.parameters.is_empty())
        else {
            let span = self.node(args).span;
            let _ = self.problem(
                ErrorCode::TypeArguments,
                span,
                "this function doesn't take type arguments",
                Some(
                    "Bracketed type arguments instantiate a generic function's type parameters, \
                     as in Json.decode[User](text) (reference 5.5).",
                ),
                None,
            );
            return InferValue::none();
        };
        let expected_count = self.signatures.types.get(fn_ty_id).parameters.count_ones() as usize;
        let arg_children = self.node(args).children.clone();
        if expected_count != arg_children.len() {
            let span = self.node(args).span;
            let _ = self.problem(
                ErrorCode::TypeArguments,
                span,
                "a function's type arguments don't match its type parameters",
                Some(
                    "Pass one type argument for each type parameter of the generic function \
                     (reference 5.5).",
                ),
                None,
            );
            return InferValue::none();
        }
        for (i, ch) in arg_children.iter().enumerate() {
            let Some(arg_nid) = *ch else {
                return InferValue::none();
            };
            let mut local_diags = DiagBag::new();
            let resolved = resolve_expression_type_in(
                self.modules,
                self.interfaces,
                self.signatures,
                self.enclosing.module,
                self.enclosing.func_sig_id,
                arg_nid,
                &mut local_diags,
            );
            if resolved.is_none() || local_diags.has_any() {
                if let Some(d) = local_diags.items().first() {
                    if d.code.number() >= 1000 {
                        self.fatal = true;
                    }
                    let msg = d.message.as_str_lossy().into_owned();
                    let rsn = d.reason.as_ref().map(|r| r.as_str_lossy().into_owned());
                    let _ = self.problem(d.code, d.primary.span, msg, rsn.as_deref(), None);
                } else {
                    let span = self.node(arg_nid).span;
                    self.fatal = true;
                    let _ = self.problem(
                        ErrorCode::CheckNoMemory,
                        span,
                        "the checking arena has no room",
                        None,
                        None,
                    );
                }
                return InferValue::none();
            }
            let Some(resolved_ty) = resolved else {
                return InferValue::none();
            };
            let Some(term) = self.annotation(resolved_ty, None, 0) else {
                return InferValue::none();
            };
            if let Err(err) = self.check_type_abilities(resolved_ty) {
                let span = self.node(arg_nid).span;
                let _ = self.problem(err.code, span, err.message, err.reason, None);
                return InferValue::none();
            }
            if !self.unify(callee.parameters[i], term, 0) {
                return InferValue::none();
            }
        }
        callee
    }

    pub(crate) fn infer_binary(
        &mut self,
        op: TokenKind,
        a: InferValue,
        right: NodeId,
        expected: Option<TermId>,
    ) -> InferValue {
        let Some(a_ty) = a.ty else {
            return InferValue::none();
        };
        let logical = matches!(op, TokenKind::And | TokenKind::Or | TokenKind::Implies);
        let comparison = matches!(
            op,
            TokenKind::Eq
                | TokenKind::Ne
                | TokenKind::Lt
                | TokenKind::Le
                | TokenKind::Gt
                | TokenKind::Ge
        );
        let a_root = self.root(a_ty);
        if op == TokenKind::Or && self.terms[a_root.index()].variable {
            let span = self.span;
            let fallback = self.infer_expression(right, expected);
            let Some(fb_ty) = fallback.ty else {
                return InferValue::none();
            };
            let Some(out_ty) = self.fresh(0) else {
                return InferValue::none();
            };
            let out = self.graph(
                InferValue {
                    ty: Some(out_ty),
                    never: a.never,
                    ..InferValue::default()
                },
                2,
            );
            if out.ty.is_none() || !self.charge_arena(1, SIZEOF_FAILURE) {
                return InferValue::none();
            }
            self.failures.push(FailureCheck {
                input: a_ty,
                value: out_ty,
                context: None,
                fallback: Some(fb_ty),
                input_note: None,
                graph: out.graph,
                span,
                wrapper: None,
                generic: None,
                op: TokenKind::Eof,
                code: None,
                solved: false,
                never: fallback.never,
            });
            if let Some(gid) = out.graph {
                let g = &mut self.emitted[gid.index()];
                g.operands[0] = a.note;
                g.operands[1] = fallback.note;
                g.operation = TokenKind::Or;
            }
            return out;
        }
        if logical && op == TokenKind::Or && self.named(a_ty, "Option") {
            let Some(payload) = self.argument(a_ty, 0) else {
                return InferValue::none();
            };
            if let Some(exp) = expected
                && !self.unify_as(payload, exp, self.node_expectation)
            {
                return InferValue::none();
            }
            let fallback = self.infer_expression(right, Some(payload));
            if fallback.ty.is_none() {
                return fallback;
            }
            let out = self.graph(
                InferValue {
                    ty: Some(payload),
                    never: a.never,
                    ..InferValue::default()
                },
                2,
            );
            if let Some(gid) = out.graph {
                let g = &mut self.emitted[gid.index()];
                g.operands[0] = a.note;
                g.operands[1] = fallback.note;
                g.operation = TokenKind::Or;
                g.or_kind = CheckedOr::Option;
            }
            return out;
        }
        if op == TokenKind::Or && self.named(a_ty, "Result") {
            let _ = self.or_result(a_ty);
            return InferValue::none();
        }
        let operand = if logical {
            self.native("Bool", 0)
        } else if op == TokenKind::DotDot {
            self.native("Int", 0)
        } else {
            Some(a_ty)
        };
        let Some(operand_ty) = operand else {
            return InferValue::none();
        };
        if !self.unify_as(
            a_ty,
            operand_ty,
            if logical {
                Some(ErrorCode::ConditionType)
            } else {
                None
            },
        ) {
            return InferValue::none();
        }
        let mut right_type = operand_ty;
        let mut result_ty = if logical || comparison {
            let Some(b) = self.native("Bool", 0) else {
                return InferValue::none();
            };
            b
        } else if op == TokenKind::DotDot {
            let Some(r) = self.native("Range", 0) else {
                return InferValue::none();
            };
            r
        } else {
            operand_ty
        };
        let mut b = InferValue::none();
        if !logical && !comparison && op != TokenKind::DotDot {
            let op_root = self.root(operand_ty);
            let op_kind = self.terms[op_root.index()].shape.kind;
            if !self.checked
                && (self.named(operand_ty, "Time")
                    || self.named(operand_ty, "Duration")
                    || op_kind == SourceTypeKind::Parameter)
            {
                let _ = self.unfinished();
                return InferValue::none();
            }
            if self.named(operand_ty, "Time") && matches!(op, TokenKind::Plus | TokenKind::Minus) {
                let Some(duration) = self.native("Duration", 0) else {
                    return InferValue::none();
                };
                if op == TokenKind::Plus {
                    if let Some(exp) = expected
                        && !self.unify_as(operand_ty, exp, self.node_expectation)
                    {
                        return InferValue::none();
                    }
                    right_type = duration;
                } else {
                    let span = self.span;
                    b = self.infer_expression(right, None);
                    let Some(b_ty) = b.ty else {
                        return InferValue::none();
                    };
                    let br = self.root(b_ty);
                    let (b_var, b_num) = (
                        self.terms[br.index()].variable,
                        self.terms[br.index()].numeric,
                    );
                    if !b_var || b_num != 0 {
                        if self.named(b_ty, "Time") {
                            result_ty = duration;
                        } else {
                            if !self.unify(b_ty, duration, 0) {
                                return InferValue::none();
                            }
                            result_ty = operand_ty;
                        }
                        if let Some(exp) = expected
                            && !self.unify_as(result_ty, exp, self.node_expectation)
                        {
                            return InferValue::none();
                        }
                    } else {
                        let Some(res_f) = self.fresh(0) else {
                            return InferValue::none();
                        };
                        result_ty = res_f;
                        if !self.charge_arena(1, SIZEOF_FAILURE) {
                            return InferValue::none();
                        }
                        if let Some(exp) = expected
                            && !self.unify(result_ty, exp, 0)
                        {
                            return InferValue::none();
                        }
                        let rr = self.root(result_ty);
                        self.terms[rr.index()].deferred = true;
                        self.failures.push(FailureCheck {
                            input: operand_ty,
                            value: result_ty,
                            context: None,
                            fallback: Some(b_ty),
                            input_note: None,
                            graph: None,
                            span,
                            wrapper: None,
                            generic: None,
                            op: TokenKind::Minus,
                            code: self.node_expectation,
                            solved: false,
                            never: false,
                        });
                    }
                }
            } else if self.named(operand_ty, "Duration")
                && matches!(op, TokenKind::Plus | TokenKind::Minus | TokenKind::Star)
            {
                if let Some(exp) = expected
                    && !self.unify_as(operand_ty, exp, self.node_expectation)
                {
                    return InferValue::none();
                }
                if op == TokenKind::Star {
                    let Some(int_ty) = self.native("Int", 0) else {
                        return InferValue::none();
                    };
                    right_type = int_ty;
                }
            } else if self.checked
                && matches!(op, TokenKind::Plus | TokenKind::Minus | TokenKind::Star)
                && self.terms[op_root.index()].variable
                && self.terms[op_root.index()].numeric == 0
                && self.terms[op_root.index()].deferred
            {
                let span = self.span;
                b = self.infer_expression(right, None);
                let Some(b_ty) = b.ty else {
                    return InferValue::none();
                };
                let Some(res_f) = self.fresh(0) else {
                    return InferValue::none();
                };
                result_ty = res_f;
                if !self.charge_arena(1, SIZEOF_FAILURE) {
                    return InferValue::none();
                }
                if let Some(exp) = expected
                    && !self.unify(result_ty, exp, 0)
                {
                    return InferValue::none();
                }
                let rr = self.root(result_ty);
                self.terms[rr.index()].deferred = true;
                self.failures.push(FailureCheck {
                    input: operand_ty,
                    value: result_ty,
                    context: None,
                    fallback: Some(b_ty),
                    input_note: None,
                    graph: None,
                    span,
                    wrapper: None,
                    generic: None,
                    op,
                    code: self.node_expectation,
                    solved: false,
                    never: false,
                });
            } else {
                let numeric = if op_kind == SourceTypeKind::Parameter {
                    Some(operand_ty)
                } else {
                    self.fresh(NUM_ARITH)
                };
                let Some(num_ty) = numeric else {
                    return InferValue::none();
                };
                let code = if self.named(operand_ty, "Byte") {
                    ErrorCode::ByteArithmetic
                } else {
                    ErrorCode::NumberAbility
                };
                if !self.unify_as(operand_ty, num_ty, Some(code)) {
                    return InferValue::none();
                }
                if let Some(exp) = expected
                    && !self.unify_as(operand_ty, exp, self.node_expectation)
                {
                    return InferValue::none();
                }
            }
        }
        if b.ty.is_none() {
            b = if logical {
                self.infer_condition(right, op == TokenKind::And)
            } else {
                self.infer_expression(right, Some(right_type))
            };
        }
        if b.ty.is_none() {
            return InferValue::none();
        }
        let out = self.graph(
            InferValue {
                ty: Some(result_ty),
                never: a.never || (!logical && b.never),
                ..InferValue::default()
            },
            2,
        );
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.operands[0] = a.note;
            g.operands[1] = b.note;
            g.operation = op;
            g.or_kind = if op == TokenKind::Or {
                CheckedOr::Bool
            } else {
                CheckedOr::Absent
            };
        }
        out
    }

    pub(crate) fn infer_propagate(
        &mut self,
        input: InferValue,
        expected: Option<TermId>,
    ) -> InferValue {
        let Some(in_ty) = input.ty else {
            return input;
        };
        if matches!(
            self.enclosing.owner_kind,
            SyntaxKind::Const | SyntaxKind::ServiceState | SyntaxKind::ServiceEvery
        ) && self.lambda_depth == 0
        {
            let _ = self.problem(
                ErrorCode::TryResult,
                self.span,
                "try has no enclosing function returning Option or Result",
                Some(
                    "try passes a failure upwards from the nearest function or anonymous function \
                     (reference 8.3).",
                ),
                None,
            );
            return InferValue::none();
        }
        let out_ty = match expected {
            Some(e) => e,
            None => {
                let Some(f) = self.fresh(0) else {
                    return InferValue::none();
                };
                f
            }
        };
        let out = self.graph(
            InferValue {
                ty: Some(out_ty),
                never: input.never,
                ..InferValue::default()
            },
            1,
        );
        if out.ty.is_none() || !self.charge_arena(1, SIZEOF_FAILURE) {
            return InferValue::none();
        }
        let fid = FailureId(self.failures.len() as u32);
        self.failures.push(FailureCheck {
            input: in_ty,
            value: out_ty,
            context: Some(self.result),
            fallback: None,
            input_note: input.note,
            graph: out.graph,
            span: self.span,
            wrapper: None,
            generic: None,
            op: TokenKind::Eof,
            code: None,
            solved: false,
            never: input.never,
        });
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.operands[0] = input.note;
            g.try_result = Some(self.result);
            g.failure = Some(fid);
        }
        let res_root = self.root(self.result);
        if !self.terms[res_root.index()].variable
            && (self.named(in_ty, "Option") || self.named(in_ty, "Result"))
            && !self.solve_failure(fid, false)
        {
            return InferValue::none();
        }
        out
    }
}
