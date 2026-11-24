//! Lexical scopes, bindings, captures, symbol/overload/module resolution, and identifier
//! inference (`src/check/infer-names.c` parity).

use mo_base::Span;
use mo_def::ErrorCode;
use mo_def::limits::MO_LIMIT_FIELDS;
use mo_syntax::{NodeId, SyntaxKind};

use crate::ids::{DataSigId, ModuleId, NativeSigId, SymIdx, SymRef, TypeId};
use crate::modules::{ModuleScope, is_source_builtin};
use crate::types::{SourceTypeKind, TypeOwner, builtin_case_by_name, builtin_type_arity};

use super::{
    Capture, CapturePath, CapturePathId, CaptureWrite, CaptureWriteId, Dispatch, DispatchId,
    InferState, InferValue, Local, LocalId, NameRef, NameSlot, NoteId, NoteRange, SIZEOF_DISPATCH,
    SIZEOF_LOCAL, SIZEOF_NAME_SLOT, SIZEOF_PTR, TermId, UNKNOWN_MEMBER_MSG,
};

impl<'a> InferState<'a> {
    pub(crate) fn length_habit(&self, name: NodeId) -> Option<&'static str> {
        let text = self.node_text(name);
        if text == b"len" || text == b"length" {
            Some(
                "The library has no len or length: a list's length is list.size, and a text's \
                 is text.char_count or text.byte_count (reference 3.4).",
            )
        } else {
            None
        }
    }

    pub(crate) fn ensure_slot(&mut self, key: &[u8]) -> bool {
        if self.names.contains_key(key) {
            return true;
        }
        if !self.charge_arena(1, SIZEOF_NAME_SLOT) {
            return false;
        }
        self.names.insert(
            key.to_vec(),
            NameSlot {
                module_use: None,
                local_name: None,
                active: None,
                expired: None,
            },
        );
        true
    }

    fn record_binding(&mut self, name: NameRef, ty: TermId, variable: bool) -> bool {
        let v = InferValue {
            ty: Some(ty),
            binding: Some(name),
            ..InferValue::default()
        };
        let ok = match name {
            NameRef::Syntax(nid) => self.record_syntax(nid, v),
            NameRef::Synthetic { name: text, span } => self.record_synthetic(text, span, v),
        };
        if !ok {
            return false;
        }
        if self.checked && !name.is_underscore(self.tree()) {
            let last = self.notes_tail;
            if let Some(lid) = last {
                self.notes[lid.index()].binding = Some(lid);
                self.notes[lid.index()].variable = variable;
                self.binding_count += 1;
            }
        }
        true
    }

    pub(crate) fn bind(&mut self, name: NameRef, ty: TermId, variable: bool) -> bool {
        let tree = self.tree();
        if name.is_underscore(tree) {
            return self.record_binding(name, ty, variable);
        }
        let key = name.bytes(tree).to_vec();
        let name_span = name.span(tree);
        if !self.ensure_slot(&key) {
            return false;
        }
        if let Some(slot) = self.names.get(&key) {
            if let Some(active_id) = slot.active {
                let prev_span = self.locals[active_id.index()].name.span(self.tree());
                return self.problem(
                    ErrorCode::BindRepeated,
                    name_span,
                    "a visible name is bound again",
                    Some(
                        "While a local name or parameter is visible it can't be bound again, \
                         even in an inner block (reference 6.1).",
                    ),
                    Some(prev_span),
                );
            }
            if let Some(mod_span) = slot.module_use {
                return self.problem(
                    ErrorCode::BindRepeated,
                    name_span,
                    "a name means both a local and a module function",
                    Some(
                        "A function can't refer to its module's function by a name it uses for \
                         a local (reference 6.1).",
                    ),
                    Some(mod_span),
                );
            }
        }
        if !self.charge_arena(1, SIZEOF_LOCAL) {
            return false;
        }
        let local_id = LocalId(self.locals.len() as u32);
        self.locals.push(Local {
            name_key: key.clone(),
            name,
            ty,
            previous: self.active_locals_head,
            binding: None,
            writes: None,
            function: self.lambda_depth,
            version: 0,
            pattern_group: 0,
            pattern_alternative: 0,
            variable,
            used: false,
            statement: false,
        });
        if let Some(slot) = self.names.get_mut(&key) {
            slot.local_name = Some(name_span);
            slot.active = Some(local_id);
        }
        self.active_locals_head = Some(local_id);
        if !self.record_binding(name, ty, variable) {
            return false;
        }
        self.locals[local_id.index()].binding = self.notes_tail;
        true
    }

    pub(crate) fn leave(&mut self, mark: Option<LocalId>) -> bool {
        while self.active_locals_head != mark {
            let Some(lid) = self.active_locals_head else {
                break;
            };
            let (used, starts_underscore, span, key, prev) = {
                let l = &self.locals[lid.index()];
                (
                    l.used,
                    l.name_key.first() == Some(&b'_'),
                    l.name.span(self.tree()),
                    l.name_key.clone(),
                    l.previous,
                )
            };
            if !used && !starts_underscore {
                self.unused = Some(lid);
                self.defer(
                    ErrorCode::UnusedName,
                    span,
                    "a name or parameter is unused",
                    Some("A name starting with _ is unused on purpose (reference 6.1)."),
                );
            }
            if let Some(slot) = self.names.get_mut(&key) {
                slot.active = None;
                slot.expired = Some(lid);
            }
            self.active_locals_head = prev;
        }
        true
    }

    fn capture_local(&mut self, local_id: LocalId, use_span: Span) -> bool {
        let local_fn = self.locals[local_id.index()].function;
        let mut cur = self.lambda_frame;
        while let Some(lam_id) = cur {
            let depth = self.lambdas[lam_id.index()].depth;
            if local_fn >= depth {
                break;
            }
            if !self.work() {
                return false;
            }
            let cap_len = self.lambdas[lam_id.index()].captures.len();
            let mut existing_pos = None;
            for i in 0..cap_len {
                if !self.work() {
                    return false;
                }
                if self.lambdas[lam_id.index()].captures[i].local == local_id {
                    existing_pos = Some(i);
                    break;
                }
            }
            if let Some(pos) = existing_pos {
                if self.lambdas[lam_id.index()].captures[pos].use_span.start <= use_span.start {
                    cur = self.lambdas[lam_id.index()].parent;
                    continue;
                }
                self.lambdas[lam_id.index()].captures.remove(pos);
            } else {
                if (self.lambdas[lam_id.index()].captures.len() as u64) == MO_LIMIT_FIELDS {
                    return self.problem(
                        ErrorCode::TooManyFields,
                        use_span,
                        "an anonymous function captures too many values",
                        None,
                        None,
                    );
                }
                if !self.charge_arena(1, 24) {
                    return false;
                }
            }
            let cur_len = self.lambdas[lam_id.index()].captures.len();
            let mut insert_at = 0usize;
            while insert_at < cur_len
                && self.lambdas[lam_id.index()].captures[insert_at]
                    .use_span
                    .start
                    < use_span.start
            {
                if !self.work() {
                    return false;
                }
                insert_at += 1;
            }
            self.lambdas[lam_id.index()].captures.insert(
                insert_at,
                Capture {
                    local: local_id,
                    use_span,
                },
            );
            cur = self.lambdas[lam_id.index()].parent;
        }
        true
    }

    pub(crate) fn capture_write(
        &mut self,
        local_id: LocalId,
        syntax: NodeId,
        target: NodeId,
    ) -> bool {
        if !self.checked {
            return true;
        }
        if !self.charge_arena(1, 32) {
            return false;
        }
        let wid = CaptureWriteId(self.capture_writes.len() as u32);
        let next = self.locals[local_id.index()].writes;
        self.capture_writes.push(CaptureWrite {
            syntax,
            target,
            path: self.capture_path,
            next,
        });
        self.locals[local_id.index()].writes = Some(wid);
        true
    }

    pub(crate) fn push_capture_path(&mut self, group: NodeId, arm: u32) -> bool {
        if !self.checked {
            return true;
        }
        if !self.charge_arena(1, 24) {
            return false;
        }
        let pid = CapturePathId(self.capture_paths.len() as u32);
        self.capture_paths.push(CapturePath {
            group,
            arm,
            parent: self.capture_path,
        });
        self.capture_path = Some(pid);
        true
    }

    pub(crate) fn find_symbol_in(&self, module: ModuleId, name: NodeId) -> Option<(SymRef, u32)> {
        let scope = self.interfaces.scope(module);
        let idx = scope.find_symbol_index(self.node_text(name))?;
        let sym = scope.symbol(idx);
        Some((SymRef::new(module, idx), sym.overload_count))
    }

    pub(crate) fn data_type(&mut self, symbol: SymRef) -> Option<DataSigId> {
        let sym = self.interfaces.symbol(symbol);
        if self.signatures.recovering
            && matches!(
                sym.decl_kind,
                SyntaxKind::Struct | SyntaxKind::CaseDecl | SyntaxKind::Enum
            )
        {
            let owner = match sym.owner {
                Some(o) => SymRef::new(sym.module, o),
                None => symbol,
            };
            let invalid = self
                .signatures
                .nominal_signature_by_symbol(owner)
                .is_none_or(|(_, n)| n.invalid);
            if invalid {
                self.failed = true;
                self.fatal = true;
                self.unavailable = true;
                return None;
            }
        }
        self.signatures
            .data_signature_by_symbol(symbol)
            .map(|(id, _)| id)
    }

    pub(crate) fn visible_data(&mut self, owner: SymRef) -> bool {
        let sym = self.interfaces.symbol(owner);
        if sym.module == self.enclosing.module {
            return true;
        }
        if !sym.insides {
            return self.problem(
                ErrorCode::DataPrivate,
                self.span,
                "the type's fields or cases are hidden",
                Some(
                    "Outside its module a type exposed without (..) can't be built, taken apart \
                     or have its fields read (reference 10.2).",
                ),
                Some(sym.name_span),
            );
        }
        true
    }

    fn find_alias_idx(&self, scope: &ModuleScope, prefix: NodeId) -> Option<usize> {
        let target = self.node_text(prefix);
        let mut lo = 0usize;
        let mut hi = scope.aliases.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if scope.aliases[mid].name.as_bytes() < target {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < scope.aliases.len() && scope.aliases[lo].name.as_bytes() == target {
            Some(lo)
        } else {
            None
        }
    }

    pub(crate) fn prefix_module(&mut self, prefix: NodeId) -> (Option<ModuleId>, Option<usize>) {
        let scope = self.interfaces.scope(self.enclosing.module);
        if let Some(idx) = self.find_alias_idx(scope, prefix) {
            return (Some(scope.aliases[idx].module), Some(idx));
        }
        let prefix_bytes = self.node_text(prefix);
        let implicit = is_source_builtin(prefix_bytes)
            || (scope.library && scope.name.as_deref().map(str::as_bytes) == Some(prefix_bytes));
        if implicit {
            for i in 0..self.interfaces.len() {
                if !self.work() {
                    return (None, None);
                }
                let mid = ModuleId(i as u32);
                let lib = self.interfaces.scope(mid);
                if lib.library && lib.name.as_deref().map(str::as_bytes) == Some(prefix_bytes) {
                    return (Some(mid), None);
                }
            }
        }
        (None, None)
    }

    pub(crate) fn use_alias(&mut self, alias: Option<usize>, name: NodeId) -> bool {
        let Some(idx) = alias else {
            return true;
        };
        let (origin, origin_span) = {
            let a = &self.interfaces.scope(self.enclosing.module).aliases[idx];
            (a.origin, a.origin_span)
        };
        if self
            .signatures
            .use_import(self.enclosing.module, origin, origin_span)
        {
            return true;
        }
        let span = self.node(name).span;
        let _ = self.problem(
            ErrorCode::CheckProgramWork,
            span,
            "the program needs too much import-checking work",
            None,
            None,
        );
        false
    }

    pub(crate) fn definition(
        &mut self,
        mut name: NodeId,
        mut prefix: Option<NodeId>,
    ) -> Option<(SymRef, u32)> {
        if self.node(name).kind == SyntaxKind::Qualified {
            prefix = self.node(name).children.first().copied().flatten();
            name = self.node(name).children.get(1).copied().flatten()?;
        }
        let own_mod = self.enclosing.module;
        if let Some(pref) = prefix {
            let (target_mod, alias) = self.prefix_module(pref);
            if self.failed {
                return None;
            }
            if target_mod.is_some()
                && self.node(name).kind == SyntaxKind::Name
                && self.node(pref).kind == SyntaxKind::CapName
            {
                let scope = self.interfaces.scope(own_mod);
                let pref_bytes = self.node_text(pref);
                let other = scope
                    .find_symbol_index(pref_bytes)
                    .map(|idx| SymRef::new(own_mod, idx))
                    .or_else(|| scope.find_imported(pref_bytes).map(|imp| imp.symbol));
                if let Some(other_ref) = other
                    && self.interfaces.symbol(other_ref).decl_kind == SyntaxKind::CaseDecl
                {
                    let span = self.node(pref).span;
                    let _ = self.problem(
                        ErrorCode::NameClash,
                        span,
                        "a clash between names",
                        Some(
                            "A clash between names is an error; use the qualified form \
                             (reference 10.3).",
                        ),
                        None,
                    );
                    return None;
                }
            }
            let Some(target) = target_mod else {
                let ty_def = self.definition(pref, None);
                if self.failed {
                    return None;
                }
                let pref_bytes = self.node_text(pref);
                let fn_params = self.signatures.types.get(self.enclosing.fn_type).parameters;
                let parameter = pref_bytes.len() == 1
                    && pref_bytes[0].is_ascii_uppercase()
                    && (fn_params & (1u32 << (pref_bytes[0] - b'A'))) != 0;
                if ty_def.is_some() || is_source_builtin(pref_bytes) || parameter {
                    return None;
                }
                let span = self.node(pref).span;
                let _ = self.problem(
                    ErrorCode::ImportMissing,
                    span,
                    "a module prefix isn't in scope",
                    Some("Use names the modules available to this file (reference 10.3)."),
                    None,
                );
                return None;
            };
            let found = self.find_symbol_in(target, name);
            if let Some((sym_ref, _)) = found {
                let sym = self.interfaces.symbol(sym_ref);
                if let Some(owner_idx) = sym.owner {
                    let owner_ref = SymRef::new(sym.module, owner_idx);
                    if self.interfaces.symbol(owner_ref).exported && !self.visible_data(owner_ref) {
                        return None;
                    }
                }
                if !sym.exported && sym.module != own_mod {
                    let span = self.node(name).span;
                    let decl_span = sym.name_span;
                    let _ = self.problem(
                        ErrorCode::ImportPrivate,
                        span,
                        "everything not on expose is private to the module",
                        Some("Everything not on expose is private to the module (reference 10.1)."),
                        Some(decl_span),
                    );
                    return None;
                }
            }
            if !self.use_alias(alias, name) {
                return None;
            }
            return found;
        }

        if let Some(local_sym) = self.find_symbol_in(own_mod, name) {
            return Some(local_sym);
        }
        let name_bytes = self.node_text(name);
        let imp_match = self
            .interfaces
            .scope(own_mod)
            .find_imported(name_bytes)
            .map(|imp| (imp.origin, imp.origin_span, imp.symbol, imp.count));
        let Some((origin, origin_span, sym_ref, count)) = imp_match else {
            if self.node(name).kind == SyntaxKind::CapName
                && matches!(
                    name_bytes,
                    b"MissingColumn" | b"TypeMismatch" | b"InvalidValue" | b"ExpectedOneRow"
                )
            {
                for i in 0..self.interfaces.len() {
                    let mid = ModuleId(i as u32);
                    let lib = self.interfaces.scope(mid);
                    if !lib.library || lib.name.as_deref() != Some("Sqlite") {
                        continue;
                    }
                    if let Some((found_ref, cnt)) = self.find_symbol_in(mid, name)
                        && self.interfaces.symbol(found_ref).exported
                    {
                        return Some((found_ref, cnt));
                    }
                }
            }
            return None;
        };
        if !self.signatures.use_import(own_mod, origin, origin_span) {
            let span = self.node(name).span;
            let _ = self.problem(
                ErrorCode::CheckProgramWork,
                span,
                "the program needs too much import-checking work",
                None,
                None,
            );
            return None;
        }
        Some((sym_ref, count))
    }

    pub(crate) fn case_receiver(&mut self, name: NodeId) -> bool {
        if self.node(name).kind != SyntaxKind::CapName {
            return false;
        }
        let scope = self.interfaces.scope(self.enclosing.module);
        if self.find_alias_idx(scope, name).is_some() {
            return false;
        }
        if let Some(builtin) = builtin_case_by_name(self.node_text(name), None) {
            return builtin.count == 0;
        }
        let Some((sym_ref, _)) = self.definition(name, None) else {
            return false;
        };
        if self.interfaces.symbol(sym_ref).decl_kind != SyntaxKind::CaseDecl {
            return false;
        }
        let Some(data_id) = self.data_type(sym_ref) else {
            return false;
        };
        self.signatures.data[data_id.index()].fields.is_empty()
    }

    pub(crate) fn type_reading(&self, prefix: NodeId, name: NodeId, call: Option<NodeId>) -> bool {
        if builtin_type_arity(self.node_text(prefix)).is_none() {
            return false;
        }
        let name_bytes = self.node_text(name);
        let compare = name_bytes == b"compare";
        if !compare && name_bytes != b"to_text" && name_bytes != b"to_json" {
            return false;
        }
        let Some(call_nid) = call else {
            return compare;
        };
        let count = self
            .node(call_nid)
            .children
            .first()
            .copied()
            .flatten()
            .map_or(0, |args| self.node(args).children.len());
        count == if compare { 2 } else { 1 }
    }

    pub(crate) fn missing_field(
        &mut self,
        span: Span,
        name: Option<&[u8]>,
        declaration: Option<Span>,
    ) -> bool {
        let message = match name {
            None => "the case is missing its unnamed value".to_string(),
            Some(bytes) => {
                let s = String::from_utf8_lossy(bytes);
                if bytes.len() > 128 {
                    let mut end = 128;
                    while end > 0 && !s.is_char_boundary(end) {
                        end -= 1;
                    }
                    format!("the record or case is missing field '{}...'", &s[..end])
                } else {
                    format!("the record or case is missing field '{s}'")
                }
            }
        };
        self.problem(
            ErrorCode::DataArguments,
            span,
            message,
            Some("There are no optional fields; a record or case is built with every field (reference 4)."),
            declaration,
        )
    }

    pub(crate) fn native_definition(&mut self, module: &str, name: NodeId) -> Option<NativeSigId> {
        let own_mod = self.enclosing.module;
        for i in 0..self.interfaces.len() {
            if !self.work() {
                return None;
            }
            let mid = ModuleId(i as u32);
            let scope = self.interfaces.scope(mid);
            if !scope.library || scope.name.as_deref() != Some(module) {
                continue;
            }
            if let Some((sym_ref, _)) = self.find_symbol_in(mid, name) {
                let sym = self.interfaces.symbol(sym_ref);
                if sym.decl_kind == SyntaxKind::Fn && (sym.module == own_mod || sym.exported) {
                    return None;
                }
            }
        }
        let name_bytes = self.node_text(name);
        for (i, nat) in self.signatures.native.iter().enumerate() {
            if nat.module == module && nat.name.as_bytes() == name_bytes {
                return Some(NativeSigId(i as u32));
            }
        }
        None
    }

    pub(crate) fn function_value(&mut self, ty_id: TypeId, requirements: [u8; 26]) -> InferValue {
        let mut params = [None; 26];
        let Some(term) = self.annotation(ty_id, Some(&mut params), 0) else {
            return InferValue::none();
        };
        let mut out = self.graph(InferValue::with_type(term), 0);
        if out.ty.is_none() {
            return out;
        }
        let mask = self.signatures.types.get(ty_id).parameters;
        if mask == 0 {
            return out;
        }
        let count = mask.count_ones() as usize;
        if !self.charge_arena(count, SIZEOF_PTR) {
            return InferValue::none();
        }
        let mut compact = Vec::with_capacity(count);
        for (i, slot) in params.iter_mut().enumerate() {
            if (mask & (1u32 << i)) != 0 {
                let tid = match *slot {
                    Some(existing) => existing,
                    None => {
                        let Some(f) = self.fresh(0) else {
                            return InferValue::none();
                        };
                        *slot = Some(f);
                        f
                    }
                };
                compact.push(tid);
            }
        }
        out.function_type = Some(ty_id);
        out.parameters = compact.clone();
        if let Some(gid) = out.graph {
            let g = &mut self.emitted[gid.index()];
            g.function_type = Some(ty_id);
            g.requirements = requirements;
            g.parameters = compact;
        }
        out
    }

    pub(crate) fn native_identifier(&mut self, native_id: NativeSigId) -> InferValue {
        let (spec_only, name, qualified, ty_id, requirements) = {
            let nat = &self.signatures.native[native_id.index()];
            (
                nat.spec_only,
                nat.name.clone(),
                nat.qualified.clone(),
                nat.ty,
                nat.requirements,
            )
        };
        if spec_only && !self.spec {
            let observes = name == "printed" || name == "printed_errors";
            if observes {
                let _ = self.problem(
                    ErrorCode::FakeOutsideSpec,
                    self.span,
                    "printed and printed_errors are read only in a test, example or promise",
                    Some(
                        "They can be used only where every console is a fake: in tests, \
                         examples and promises (reference 12.4).",
                    ),
                    None,
                );
            } else {
                let _ = self.problem(
                    ErrorCode::FakeOutsideSpec,
                    self.span,
                    "a fake capability is made only in a test, example or promise",
                    Some(
                        "Fakes are made only in tests, examples and promises, never in a \
                         module's constants or its functions (reference 12.4).",
                    ),
                    None,
                );
            }
            return InferValue::none();
        }
        let out = self.function_value(ty_id, requirements);
        if let Some(gid) = out.graph {
            self.emitted[gid.index()].native = Some(qualified);
        }
        out
    }

    pub(crate) fn declared_function(&mut self, sym_ref: SymRef) -> InferValue {
        let mut lo = 0usize;
        let mut hi = self.signatures.functions.len();
        while lo < hi {
            if !self.work() {
                return InferValue::none();
            }
            let mid = lo + (hi - lo) / 2;
            if self.signatures.functions[mid].function < sym_ref {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        debug_assert!(
            lo < self.signatures.functions.len()
                && self.signatures.functions[lo].function == sym_ref
        );
        let (ty_opt, requirements) = {
            let sig = &self.signatures.functions[lo];
            (sig.ty, sig.requirements)
        };
        let Some(ty_id) = ty_opt else {
            debug_assert!(self.signatures.recovering);
            self.failed = true;
            self.fatal = true;
            self.unavailable = true;
            return InferValue::none();
        };
        let mut out = self.function_value(ty_id, requirements);
        out.symbol = Some(sym_ref);
        let sym = self.interfaces.symbol(sym_ref);
        let fn_scope = self.interfaces.scope(sym.module);
        if let Some(out_ty) = out.ty
            && fn_scope.library
            && fn_scope.name.as_deref() == Some("Json")
            && (sym.name == "decode" || sym.name == "decode_strict")
        {
            let Some(input) = self.fresh(0) else {
                return InferValue::none();
            };
            if let Some(args) = self.terms[out_ty.index()].arguments.as_mut() {
                args[0] = input;
            }
            out.json_decode = true;
            if let Some(gid) = out.graph {
                self.emitted[gid.index()].json_decode = true;
            }
        }
        out
    }

    pub(crate) fn select_overload(
        &mut self,
        group: SymRef,
        count: u32,
        receiver: TermId,
    ) -> Option<SymRef> {
        if count == 1 {
            return Some(group);
        }
        let r = self.root(receiver);
        if self.terms[r.index()].variable {
            return None;
        }
        let recv_owner = self.terms[r.index()].shape.owner;
        let scope = self.interfaces.scope(group.module);
        for i in 0..count {
            if !self.work() {
                return None;
            }
            let cand_idx = SymIdx(group.symbol.0 + i);
            let cand_sym = scope.symbol(cand_idx);
            if let Some(recv_sym_idx) = cand_sym.receiver
                && recv_owner == TypeOwner::Source(SymRef::new(group.module, recv_sym_idx))
            {
                return Some(SymRef::new(group.module, cand_idx));
            }
        }
        let _ = self.problem(
            ErrorCode::ArgumentType,
            self.span,
            "no overload takes this first argument's type",
            Some(
                "A call picks the function whose first parameter matches its first argument \
                 (reference 5.1).",
            ),
            None,
        );
        None
    }

    pub(crate) fn deferred_function(
        &mut self,
        name: NodeId,
        group: Option<(SymRef, u32)>,
        count: usize,
    ) -> InferValue {
        if !self.charge_arena(1, SIZEOF_DISPATCH) {
            return InferValue::none();
        }
        let Some(fresh_ty) = self.fresh(0) else {
            return InferValue::none();
        };
        let mut out = self.graph(InferValue::with_type(fresh_ty), 0);
        let Some(out_ty) = out.ty else {
            return InferValue::none();
        };
        self.terms[out_ty.index()].deferred = group.is_none();
        let did = DispatchId(self.dispatches.len() as u32);
        self.dispatches.push(Dispatch {
            name,
            group,
            count,
            ty: out_ty,
            graph: out.graph,
            note: None,
            syntax: None,
            call: None,
            arguments: None,
            receiver: InferValue::none(),
            inputs: Vec::new(),
            position: false,
            solved: false,
        });
        out.dispatch = Some(did);
        out
    }

    pub(crate) fn type_scope(&mut self, ty: TermId) -> Option<ModuleId> {
        let term = &self.terms[ty.index()];
        if let Some(sym) = term.shape.owner.as_source() {
            return Some(sym.module);
        }
        if term.shape.kind != SourceTypeKind::Named {
            return None;
        }
        let mod_name = if term.shape.name.starts_with("Sqlite.") {
            "Sqlite".to_string()
        } else {
            term.shape.name.clone()
        };
        for i in 0..self.interfaces.len() {
            if !self.work() {
                return None;
            }
            let mid = ModuleId(i as u32);
            let scope = self.interfaces.scope(mid);
            if scope.library && scope.name.as_deref() == Some(mod_name.as_str()) {
                return Some(mid);
            }
        }
        None
    }

    fn qualified_ability(&mut self, prefix: NodeId, name: NodeId) -> InferValue {
        let pref_bytes = self.node_text(prefix);
        let builtin = builtin_type_arity(pref_bytes);
        let (module, alias) = if builtin.is_some() {
            (None, None)
        } else {
            self.prefix_module(prefix)
        };
        if self.failed {
            return InferValue::none();
        }
        let bit = if pref_bytes.len() == 1 && pref_bytes[0].is_ascii_uppercase() {
            1u32 << (pref_bytes[0] - b'A')
        } else {
            0
        };
        let fn_params = self.signatures.types.get(self.enclosing.fn_type).parameters;

        let ty_opt = if let Some((bname, arity)) = builtin {
            let Some(t) = self.native(bname, arity as usize) else {
                return InferValue::none();
            };
            for i in 0..(arity as usize) {
                let Some(f) = self.fresh(0) else {
                    return InferValue::none();
                };
                if let Some(args) = self.terms[t.index()].arguments.as_mut() {
                    args[i] = f;
                }
            }
            Some(t)
        } else if module.is_some() {
            if !self.use_alias(alias, name) {
                return InferValue::none();
            }
            self.fresh(0)
        } else if (fn_params & bit) != 0 {
            let pty = self.parameter_type(self.enclosing.fn_type, bit);
            pty.and_then(|pt| self.annotation(pt, None, 0))
        } else {
            let def = self.definition(prefix, None);
            if self.failed {
                return InferValue::none();
            }
            let nom_ty = def.and_then(|(s, _)| {
                let sym = self.interfaces.symbol(s);
                if sym.owner.is_none()
                    && matches!(sym.decl_kind, SyntaxKind::Struct | SyntaxKind::Enum)
                {
                    self.signatures
                        .nominal_signature_by_symbol(s)
                        .map(|(_, n)| n.ty)
                } else {
                    None
                }
            });
            let Some(nom_ty) = nom_ty else {
                if self.definition(name, Some(prefix)).is_none() && !self.failed {
                    let _ = self.unfinished();
                }
                return InferValue::none();
            };
            let mut params = [None; 26];
            self.annotation(nom_ty, Some(&mut params), 0)
        };

        let Some(ty) = ty_opt else {
            return InferValue::none();
        };
        let out = self.ability_function(InferValue::with_type(ty), name);
        if let (Some(gid), Some(mid)) = (out.graph, module) {
            if !self.charge_arena(1, 24) {
                return InferValue::none();
            }
            self.ability_modules.push((gid, mid));
        }
        out
    }

    pub(crate) fn identifier(
        &mut self,
        mut node: NodeId,
        mut prefix: Option<NodeId>,
    ) -> InferValue {
        if self.node(node).kind == SyntaxKind::Qualified {
            prefix = self.node(node).children.first().copied().flatten();
            let Some(rhs) = self.node(node).children.get(1).copied().flatten() else {
                return InferValue::none();
            };
            node = rhs;
        }
        if let Some(pref) = prefix {
            let pref_bytes = self.node_text(pref);
            if pref_bytes == b"Simulation" {
                return self.simulation_call(node, None);
            }
            if pref_bytes == b"Actor" {
                return self.actor_call(node, None);
            }
            if pref_bytes == b"ApiTest" {
                return self.apitest_call(node, None);
            }
        }
        if prefix.is_none() && self.node(node).kind == SyntaxKind::Name {
            let key = self.node_text(node).to_vec();
            let active = self.names.get(&key).and_then(|s| s.active);
            if let Some(lid) = active {
                let span = self.node(node).span;
                if self.checked
                    && self.locals[lid.index()].function != self.lambda_depth
                    && !self.capture_local(lid, span)
                {
                    return InferValue::none();
                }
                self.locals[lid.index()].used = true;
                let (ty, name, binding) = {
                    let l = &self.locals[lid.index()];
                    (l.ty, l.name, l.binding)
                };
                return InferValue {
                    ty: Some(ty),
                    binding: Some(name),
                    bound: binding,
                    ..InferValue::default()
                };
            }
        }

        let node_bytes = self.node_text(node);
        let pref_bytes = prefix.map(|p| self.node_text(p));
        if let Some(builtin) = builtin_case_by_name(node_bytes, pref_bytes) {
            return self.builtin_construct(builtin, None, None);
        }
        if let Some(pref) = prefix
            && matches!(node_bytes, b"to_text" | b"compare" | b"to_json")
        {
            return self.qualified_ability(pref, node);
        }

        let def = self.definition(node, prefix);
        if self.failed {
            return InferValue::none();
        }
        let Some((sym_ref, count)) = def else {
            if let Some(pref) = prefix {
                let pbytes = self.node_text(pref);
                let nbytes = self.node_text(node);
                let found_nat = self.signatures.native.iter().position(|nat| {
                    nat.module.as_bytes() == pbytes && nat.name.as_bytes() == nbytes
                });
                if let Some(idx) = found_nat {
                    return self.native_identifier(NativeSigId(idx as u32));
                }
            }
            if prefix.is_some() {
                let span = self.node(node).span;
                let _ = self.problem(
                    ErrorCode::NameUnknown,
                    span,
                    UNKNOWN_MEMBER_MSG,
                    Some(
                        "T.f names a function in the module of T, or one T has through its \
                         abilities (reference 5.4, 10.3).",
                    ),
                    None,
                );
            } else if matches!(
                self.node(node).kind,
                SyntaxKind::CapName | SyntaxKind::Qualified
            ) {
                let _ = self.unfinished();
            } else {
                let key = self.node_text(node).to_vec();
                let expired = self.names.get(&key).and_then(|s| s.expired);
                if let Some(exp_id) = expired
                    && self
                        .error
                        .as_ref()
                        .is_some_and(|e| e.code == ErrorCode::UnusedName)
                    && self.error_unused == Some(exp_id)
                {
                    self.error = None;
                }
                let habit = if !self.names.contains_key(&key) {
                    self.length_habit(node)
                } else {
                    None
                };
                let span = self.node(node).span;
                let _ = self.problem(
                    ErrorCode::NameUnknown,
                    span,
                    "a name isn't visible in this block",
                    Some(habit.unwrap_or(
                        "A local name is visible from its binding to the end of its block \
                         (reference 6.1).",
                    )),
                    None,
                );
            }
            return InferValue::none();
        };

        let decl_kind = self.interfaces.symbol(sym_ref).decl_kind;
        if matches!(decl_kind, SyntaxKind::Struct | SyntaxKind::CaseDecl) {
            let Some(data_id) = self.data_type(sym_ref) else {
                debug_assert!(self.unavailable);
                return InferValue::none();
            };
            let data_ty = self.signatures.data[data_id.index()].ty;
            let owner_sym = self
                .signatures
                .types
                .get(data_ty)
                .owner
                .as_source()
                .unwrap_or(sym_ref);
            if !self.visible_data(owner_sym) {
                return InferValue::none();
            }
            if self.signatures.data[data_id.index()].fields.is_empty() {
                let mut params = [None; 26];
                let Some(inst) = self.annotation(data_ty, Some(&mut params), 0) else {
                    return InferValue::none();
                };
                let out = self.graph(
                    InferValue {
                        ty: Some(inst),
                        symbol: Some(sym_ref),
                        ..InferValue::default()
                    },
                    0,
                );
                if let Some(gid) = out.graph {
                    self.emitted[gid.index()].generic = Some(data_ty);
                    self.emitted[gid.index()].instantiated = out.ty;
                }
                return out;
            }
            let (field_name, field_span) = {
                let f = self.signatures.data[data_id.index()].field_by_index(0);
                (f.name.clone(), f.name_span)
            };
            let span = self.node(node).span;
            let _ = self.missing_field(span, field_name.as_deref().map(str::as_bytes), field_span);
            return InferValue::none();
        }

        if decl_kind == SyntaxKind::Actor {
            let span = self.node(node).span;
            let decl_span = self.interfaces.symbol(sym_ref).name_span;
            let _ = self.problem(
                ErrorCode::ActorBlock,
                span,
                "an actor type is not a value",
                Some(
                    "Receive an actor handle as a parameter, declare actor <name>: <Type> in an \
                     api block, or call Actor.start(<Type>) in a test (reference 11.7).",
                ),
                Some(decl_span),
            );
            return InferValue::none();
        }

        let is_const = decl_kind == SyntaxKind::Const;
        if decl_kind != SyntaxKind::Fn && !is_const {
            let _ = self.unfinished();
            return InferValue::none();
        }

        if prefix.is_none() && self.node(node).kind == SyntaxKind::Name {
            let key = self.node_text(node).to_vec();
            let span = self.node(node).span;
            if !self.ensure_slot(&key) {
                return InferValue::none();
            }
            if let Some(local_span) = self.names.get(&key).and_then(|s| s.local_name) {
                let _ = self.problem(
                    ErrorCode::BindRepeated,
                    span,
                    "a name means both a local and a module function",
                    Some(
                        "A function can't refer to its module's function by a name it uses for a \
                         local (reference 6.1).",
                    ),
                    Some(local_span),
                );
                return InferValue::none();
            }
            if let Some(slot) = self.names.get_mut(&key) {
                slot.module_use = Some(span);
            }
        }

        if is_const {
            let span = self.node(node).span;
            let const_res = match self.constants.as_deref_mut() {
                None => None,
                Some(consts) => {
                    let idx = consts.index_by_symbol(sym_ref);
                    if let Some(ty_id) = consts.items[idx].1 {
                        Some(Ok(ty_id))
                    } else {
                        if !consts.done[idx] {
                            consts.pending = Some((idx, span));
                        }
                        Some(Err(()))
                    }
                }
            };
            return match const_res {
                None => {
                    let _ = self.unfinished();
                    InferValue::none()
                }
                Some(Err(())) => {
                    self.failed = true;
                    self.fatal = true;
                    self.unavailable = true;
                    InferValue::none()
                }
                Some(Ok(const_ty)) => {
                    let Some(t) = self.annotation(const_ty, None, 0) else {
                        return InferValue::none();
                    };
                    self.graph(
                        InferValue {
                            ty: Some(t),
                            symbol: Some(sym_ref),
                            ..InferValue::default()
                        },
                        0,
                    )
                }
            };
        }

        if count > 1 {
            return self.deferred_function(node, Some((sym_ref, count)), count as usize);
        }
        self.declared_function(sym_ref)
    }

    pub(crate) fn infer_as(
        &mut self,
        node: NodeId,
        expected: Option<TermId>,
        code: Option<ErrorCode>,
    ) -> InferValue {
        let saved = self.expectation;
        self.expectation = code;
        let v = self.infer_expression(node, expected);
        self.expectation = saved;
        v
    }

    pub(crate) fn infer_discard(&mut self, node: NodeId) -> InferValue {
        let mut v = self.infer_expression(node, None);
        if v.ty.is_some() {
            if let Some(tail) = self.notes_tail {
                self.notes[tail.index()].discarded = !v.never;
            }
            v.ty = self.native("Unit", 0);
        }
        v
    }

    pub(crate) fn is_lambda_argument(&self, mut node: NodeId) -> bool {
        while self.node(node).kind == SyntaxKind::Group {
            let Some(inner) = self.node(node).children.first().copied().flatten() else {
                break;
            };
            node = inner;
        }
        self.node(node).kind == SyntaxKind::Lambda
    }

    pub(crate) fn order_notes(&mut self, mut before: Option<NoteId>, ranges: &[NoteRange]) {
        for r in ranges {
            if let Some(b) = before {
                self.notes[b.index()].next = r.first;
            } else {
                self.notes_head = r.first;
            }
            before = r.last;
        }
        if let Some(b) = before {
            self.notes[b.index()].next = None;
        }
        self.notes_tail = before;
    }

    pub(crate) fn note_span(&self, id: NoteId) -> Span {
        let n = &self.notes[id.index()];
        if let Some(nid) = n.syntax {
            self.node(nid).span
        } else if let Some((_, span)) = n.synthetic_name {
            span
        } else {
            Span::NONE
        }
    }
}
