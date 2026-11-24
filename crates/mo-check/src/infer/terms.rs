//! Union-find inference terms, unification, numeric masks, type previews, and map-key
//! duplicate checking (`src/check/infer-terms.c` parity).

use std::cmp::Ordering;

use mo_base::Span;
use mo_def::ErrorCode;
use mo_def::limits::MO_LIMIT_SOURCE_TYPE_DEPTH;
use mo_syntax::{NodeId, SyntaxKind};

use crate::abilities::SourceAbility;
use crate::ids::TypeId;
use crate::map_keys::{MapKeyBudget, MapKeyCategory, MapLiteralKey, canonicalize_map_literal_key};
use crate::types::{SourceTypeKind, TypeOwner};

use super::{
    Emitted, EmittedId, InferState, InferValue, NUM_ARITH, Note, NoteId, SIZEOF_EMITTED,
    SIZEOF_NOTE, SIZEOF_PTR, SIZEOF_TERM, Term, TermId, TermShape,
};

struct TypePreview {
    text: String,
}

impl TypePreview {
    const CAP: usize = 512;

    fn new() -> Self {
        Self {
            text: String::new(),
        }
    }

    fn is_full(&self) -> bool {
        self.text.len() + 4 >= Self::CAP
    }

    fn push(&mut self, s: &str) {
        if self.is_full() {
            return;
        }
        let mut bytes = s.as_bytes();
        while !bytes.is_empty() && self.text.len() + 4 < Self::CAP {
            self.text.push(char::from(bytes[0]));
            bytes = &bytes[1..];
        }
        if !bytes.is_empty() && self.text.len() + 4 <= Self::CAP {
            self.text.push_str("...");
        }
    }
}

struct InferMapBudget<'a, 'b> {
    state: &'a mut InferState<'b>,
}

impl MapKeyBudget for InferMapBudget<'_, '_> {
    fn charge_work(&mut self, amount: usize) -> bool {
        for _ in 0..amount {
            if !self.state.work() {
                return false;
            }
        }
        true
    }

    fn charge_bytes(&mut self, bytes: usize) -> bool {
        self.state.charge_arena(bytes, 1)
    }
}

#[derive(Debug, Clone)]
struct LiteralKeyEntry {
    category: u8, // 0..4 matching MapKeyCategory, 5 = Name
    bytes: Vec<u8>,
    negative: bool,
    span: Span,
    position: usize,
    version: u32,
}

impl<'a> InferState<'a> {
    pub(crate) fn make_type(&mut self, shape: TermShape) -> Option<TermId> {
        if !self.work() || !self.charge_arena(1, SIZEOF_TERM) {
            return None;
        }
        let arguments = if shape.arity != 0 {
            if !self.charge_arena(shape.arity, SIZEOF_PTR) {
                return None;
            }
            Some(vec![TermId(0); shape.arity])
        } else {
            None
        };
        let id = TermId(self.terms.len() as u32);
        self.terms.push(Term {
            shape,
            arguments,
            parent: None,
            resolved: None,
            rank: 0,
            seen: 0,
            height: 0,
            numeric: 0,
            variable: false,
            partial_default: false,
            deferred: false,
        });
        Some(id)
    }

    pub(crate) fn fresh(&mut self, numeric: u8) -> Option<TermId> {
        let id = self.make_type(TermShape::default())?;
        let t = &mut self.terms[id.index()];
        t.variable = true;
        t.numeric = numeric;
        Some(id)
    }

    pub(crate) fn native(&mut self, name: &str, arity: usize) -> Option<TermId> {
        self.make_type(TermShape {
            kind: SourceTypeKind::Named,
            name: name.to_string(),
            module: None,
            owner: TypeOwner::None,
            arity,
        })
    }

    pub(crate) fn root(&mut self, mut t: TermId) -> TermId {
        let mut at = t;
        while let Some(p) = self.terms[at.index()].parent {
            at = p;
        }
        while let Some(next) = self.terms[t.index()].parent {
            self.terms[t.index()].parent = Some(at);
            t = next;
        }
        at
    }

    pub(crate) fn named(&mut self, t: TermId, name: &str) -> bool {
        let r = self.root(t);
        let term = &self.terms[r.index()];
        !term.variable
            && term.shape.kind == SourceTypeKind::Named
            && term.shape.module.is_none()
            && term.shape.name == name
    }

    fn number_type(&mut self, t: TermId) -> u8 {
        const NAMES: [&str; 5] = ["Int", "BigInt", "Byte", "Decimal", "Float"];
        for (i, &name) in NAMES.iter().enumerate() {
            if self.named(t, name) {
                return 1u8 << i;
            }
        }
        0
    }

    pub(crate) fn type_depth(&mut self, depth: u32) -> bool {
        if u64::from(depth) < MO_LIMIT_SOURCE_TYPE_DEPTH {
            return true;
        }
        self.problem(
            ErrorCode::CheckTypeDepth,
            self.span,
            "the inferred type is nested too deeply",
            None,
            None,
        )
    }

    pub(crate) fn argument(&mut self, t: TermId, index: usize) -> Option<TermId> {
        let r = self.root(t);
        debug_assert!(index < self.terms[r.index()].shape.arity);
        if let Some(args) = &self.terms[r.index()].arguments {
            return Some(args[index]);
        }
        let resolved = self.terms[r.index()].resolved?;
        let arg_ty = self.signatures.types.get(resolved).arguments[index];
        self.annotation(arg_ty, None, 0)
    }

    fn preview_type_inner(&mut self, out: &mut TypePreview, t: TermId, depth: u32) -> bool {
        if out.is_full() {
            return true;
        }
        if !self.work() {
            return false;
        }
        let r = self.root(t);
        if u64::from(depth) == MO_LIMIT_SOURCE_TYPE_DEPTH {
            out.push("...");
            return true;
        }
        let (variable, numeric, kind, name, arity) = {
            let term = &self.terms[r.index()];
            (
                term.variable,
                term.numeric,
                term.shape.kind,
                term.shape.name.clone(),
                term.shape.arity,
            )
        };
        if variable {
            if numeric == 0 {
                out.push("?");
                return true;
            }
            const NUMBERS: [&str; 5] = ["Int", "BigInt", "Byte", "Decimal", "Float"];
            let mut first = true;
            for (i, &num_name) in NUMBERS.iter().enumerate() {
                if (numeric & (1u8 << i)) != 0 {
                    if !first {
                        out.push(" or ");
                    }
                    out.push(num_name);
                    first = false;
                }
            }
            return true;
        }
        let tuple = kind == SourceTypeKind::Tuple;
        let function = kind == SourceTypeKind::Function;
        if !tuple {
            out.push(&name);
        }
        if arity == 0 {
            return true;
        }
        out.push("(");
        let count = arity - usize::from(function);
        for i in 0..count {
            if out.is_full() {
                break;
            }
            if i != 0 {
                out.push(", ");
            }
            let Some(part) = self.argument(r, i) else {
                return false;
            };
            if !self.preview_type_inner(out, part, depth + 1) {
                return false;
            }
        }
        out.push(")");
        if function && !out.is_full() {
            out.push(" : ");
            let Some(res) = self.argument(r, count) else {
                return false;
            };
            if !self.preview_type_inner(out, res, depth + 1) {
                return false;
            }
        }
        true
    }

    pub(crate) fn preview_type_string(&mut self, t: TermId) -> Option<String> {
        let mut preview = TypePreview::new();
        if !self.preview_type_inner(&mut preview, t, 0) {
            return None;
        }
        Some(preview.text)
    }

    fn mismatch(&mut self, found: TermId, expected: TermId) -> bool {
        let mut label = TypePreview::new();
        label.push("expected ");
        if !self.preview_type_inner(&mut label, expected, 0) {
            return false;
        }
        label.push(", found ");
        if !self.preview_type_inner(&mut label, found, 0) {
            return false;
        }
        if !self.charge_arena(label.text.len() + 1, 1) {
            return false;
        }
        self.type_label = Some(label.text);
        let code = self.expectation.unwrap_or(ErrorCode::TypeMismatch);
        let (message, reason) = match code {
            ErrorCode::ConditionType => (
                "a condition isn't a yes-or-no answer",
                "Bool is a yes-or-no answer; conditions need Bool (reference 3.1).",
            ),
            ErrorCode::NumberAbility => {
                let is_text = self.named(found, "Text");
                (
                    "arithmetic is used on a type without Number",
                    if is_text {
                        "Text has no Number, so + doesn't join texts; insert them into one text \
                         instead, as in \"#{a}#{b}\" (reference 3.4)."
                    } else {
                        "Only Int, BigInt, Decimal and Float have Number; times and durations \
                         also add and subtract (reference 9.2)."
                    },
                )
            }
            ErrorCode::ByteArithmetic => (
                "Byte is used in arithmetic",
                "Byte has no arithmetic; convert it with to_int first (reference 3.2).",
            ),
            ErrorCode::ArgumentType => (
                "an argument's type doesn't match its parameter",
                "A call passes each argument to its corresponding parameter (reference 5.2).",
            ),
            ErrorCode::ResultType => (
                "a function's result doesn't match its signature",
                "A function's signature gives its types (reference 5.1).",
            ),
            ErrorCode::ServiceValue => (
                "a service value has the wrong type",
                "The start value has the declared state type (reference 11.5).",
            ),
            ErrorCode::NotFunction => (
                "a value that isn't a function is called",
                "A call applies a function to its arguments (reference 5.2).",
            ),
            _ => (
                "values of different types are combined",
                "Comparing or combining values of different types is an error (reference 3).",
            ),
        };
        self.problem(code, self.span, message, Some(reason), None)
    }

    pub(crate) fn or_result(&mut self, found: TermId) -> bool {
        let mut label = TypePreview::new();
        label.push("expected Option or Bool, found ");
        if !self.preview_type_inner(&mut label, found, 0) {
            return false;
        }
        if !self.charge_arena(label.text.len() + 1, 1) {
            return false;
        }
        self.type_label = Some(label.text);
        self.problem(
            ErrorCode::ConditionType,
            self.span,
            "or unwraps Option or combines Bool, not Result",
            Some(
                "or unwraps Option or combines Bool, not Result; use try, case, or assert ... is \
                 Ok(v) (reference 8.1, 8.2).",
            ),
            None,
        )
    }

    fn occurs(&mut self, needle: TermId, t: TermId, depth: u32) -> bool {
        if !self.work() || !self.type_depth(depth) {
            return true;
        }
        let r = self.root(t);
        if needle == r {
            return true;
        }
        if self.terms[r.index()].resolved.is_some() {
            let h = self.terms[r.index()].height;
            return !self.type_depth(depth + h);
        }
        if self.terms[r.index()].seen == self.epoch {
            return false;
        }
        self.terms[r.index()].seen = self.epoch;
        let arity = self.terms[r.index()].shape.arity;
        for i in 0..arity {
            let Some(arg) = self.terms[r.index()].arguments.as_ref().map(|a| a[i]) else {
                continue;
            };
            if self.occurs(needle, arg, depth + 1) {
                return true;
            }
        }
        false
    }

    fn is_enclosing_number_param(&self, term_id: TermId, numeric: u8) -> bool {
        if (numeric & NUM_ARITH) != NUM_ARITH {
            return false;
        }
        let term = &self.terms[term_id.index()];
        if term.shape.kind != SourceTypeKind::Parameter {
            return false;
        }
        let Some(owner_sym) = self.enclosing.owner_symbol else {
            return false;
        };
        if term.shape.owner != TypeOwner::Source(owner_sym) {
            return false;
        }
        let bytes = term.shape.name.as_bytes();
        if bytes.len() == 1 && bytes[0].is_ascii_uppercase() {
            let letter = (bytes[0] - b'A') as usize;
            (self.enclosing.requirements[letter] & SourceAbility::Number.bit()) != 0
        } else {
            false
        }
    }

    pub(crate) fn unify(&mut self, a: TermId, b: TermId, depth: u32) -> bool {
        if !self.work() || !self.type_depth(depth) {
            return false;
        }
        let mut a = self.root(a);
        let mut b = self.root(b);
        if a == b
            || (self.terms[a.index()].resolved.is_some()
                && self.terms[a.index()].resolved == self.terms[b.index()].resolved)
        {
            return true;
        }
        let a_num = self.terms[a.index()].numeric;
        let b_num = self.terms[b.index()].numeric;
        let numeric = if a_num == 0 {
            b_num
        } else if b_num == 0 {
            a_num
        } else {
            a_num & b_num
        };
        if a_num != 0 && b_num != 0 && numeric == 0 {
            return self.mismatch(a, b);
        }
        let a_number = self.is_enclosing_number_param(a, numeric);
        let b_number = self.is_enclosing_number_param(b, numeric);
        let a_var = self.terms[a.index()].variable;
        let b_var = self.terms[b.index()].variable;
        if (!a_var && numeric != 0 && !a_number && (self.number_type(a) & numeric) == 0)
            || (!b_var && numeric != 0 && !b_number && (self.number_type(b) & numeric) == 0)
        {
            return self.mismatch(a, b);
        }
        if a_var && !b_var {
            std::mem::swap(&mut a, &mut b);
        }
        if self.terms[b.index()].variable {
            debug_assert!(self.epoch < u32::MAX);
            self.epoch += 1;
            if self.occurs(b, a, 0) {
                return if self.failed {
                    false
                } else {
                    self.mismatch(a, b)
                };
            }
        } else {
            let same_shape = {
                let sa = &self.terms[a.index()].shape;
                let sb = &self.terms[b.index()].shape;
                sa.kind == sb.kind
                    && sa.module == sb.module
                    && sa.owner == sb.owner
                    && sa.arity == sb.arity
                    && sa.name == sb.name
            };
            if !same_shape {
                return self.mismatch(a, b);
            }
            let arity = self.terms[a.index()].shape.arity;
            for i in 0..arity {
                let Some(x) = self.argument(a, i) else {
                    return false;
                };
                let Some(y) = self.argument(b, i) else {
                    return false;
                };
                if !self.unify(x, y, depth + 1) {
                    return false;
                }
            }
        }
        let a_var = self.terms[a.index()].variable;
        let b_var = self.terms[b.index()].variable;
        let a_rank = self.terms[a.index()].rank;
        let b_rank = self.terms[b.index()].rank;
        if a_var == b_var && a_rank < b_rank {
            std::mem::swap(&mut a, &mut b);
        }
        let b_deferred = self.terms[b.index()].deferred;
        self.terms[b.index()].parent = Some(a);
        self.terms[a.index()].numeric = numeric;
        self.terms[a.index()].deferred = self.terms[a.index()].deferred || b_deferred;
        if self.terms[a.index()].rank == self.terms[b.index()].rank {
            self.terms[a.index()].rank += 1;
        }
        true
    }

    pub(crate) fn unify_as(
        &mut self,
        found: TermId,
        expected: TermId,
        code: Option<ErrorCode>,
    ) -> bool {
        let saved = self.expectation;
        self.expectation = code;
        let valid = self.unify(found, expected, 0);
        self.expectation = saved;
        valid
    }

    pub(crate) fn same_type(&mut self, a: TermId, b: TermId, depth: u32) -> bool {
        let mut a = self.root(a);
        let mut b = self.root(b);
        if a == b
            || (self.terms[a.index()].resolved.is_some()
                && self.terms[a.index()].resolved == self.terms[b.index()].resolved)
        {
            return true;
        }
        if u64::from(depth) >= MO_LIMIT_SOURCE_TYPE_DEPTH {
            return false;
        }
        if self.terms[a.index()].variable && !self.terms[b.index()].variable {
            std::mem::swap(&mut a, &mut b);
        }
        if self.terms[b.index()].variable {
            let b_num = self.terms[b.index()].numeric;
            let a_var = self.terms[a.index()].variable;
            let a_num = self.terms[a.index()].numeric;
            if b_num == 0 || (a_var && a_num == 0) {
                return true;
            }
            let mask = if a_var { a_num } else { self.number_type(a) };
            return (mask & b_num) != 0;
        }
        let same_shape = {
            let sa = &self.terms[a.index()].shape;
            let sb = &self.terms[b.index()].shape;
            sa.kind == sb.kind
                && sa.module == sb.module
                && sa.owner == sb.owner
                && sa.arity == sb.arity
                && sa.name == sb.name
        };
        if !same_shape {
            return false;
        }
        let arity = self.terms[a.index()].shape.arity;
        for i in 0..arity {
            let Some(x) = self.argument(a, i) else {
                return false;
            };
            let Some(y) = self.argument(b, i) else {
                return false;
            };
            if !self.same_type(x, y, depth + 1) {
                return false;
            }
        }
        true
    }

    pub(crate) fn annotation(
        &mut self,
        ty_id: TypeId,
        mut parameters: Option<&mut [Option<TermId>; 26]>,
        depth: u32,
    ) -> Option<TermId> {
        if depth == 0 && !self.signatures.signature_type_available(Some(ty_id)) {
            self.failed = true;
            self.fatal = true;
            self.unavailable = true;
            return None;
        }
        if !self.type_depth(depth) {
            return None;
        }
        let (kind, name, module, owner, arity, params_mask, height) = {
            let ty = self.signatures.types.get(ty_id);
            (
                ty.kind,
                ty.name.clone(),
                ty.module,
                ty.owner,
                ty.arguments.len(),
                ty.parameters,
                ty.height,
            )
        };
        if parameters.is_none() || params_mask == 0 {
            if !self.work()
                || !self.type_depth(depth + height)
                || !self.charge_arena(1, SIZEOF_TERM)
            {
                return None;
            }
            let id = TermId(self.terms.len() as u32);
            self.terms.push(Term {
                shape: TermShape {
                    kind,
                    name,
                    module,
                    owner,
                    arity,
                },
                arguments: None,
                parent: None,
                resolved: Some(ty_id),
                rank: 0,
                seen: 0,
                height,
                numeric: 0,
                variable: false,
                partial_default: false,
                deferred: false,
            });
            return Some(id);
        }
        if kind == SourceTypeKind::Parameter
            && let Some(params) = parameters.as_deref_mut()
        {
            let letter = (name.as_bytes()[0] - b'A') as usize;
            debug_assert!(letter < 26);
            if let Some(existing) = params[letter] {
                return Some(existing);
            }
            let fresh_id = self.fresh(0)?;
            params[letter] = Some(fresh_id);
            return Some(fresh_id);
        }
        let id = self.make_type(TermShape {
            kind,
            name,
            module,
            owner,
            arity,
        })?;
        for i in 0..arity {
            let child_ty = self.signatures.types.get(ty_id).arguments[i];
            let child_term = self.annotation(child_ty, parameters.as_deref_mut(), depth + 1)?;
            if let Some(args) = self.terms[id.index()].arguments.as_mut() {
                args[i] = child_term;
            }
        }
        Some(id)
    }

    pub(crate) fn graph(&mut self, mut v: InferValue, count: usize) -> InferValue {
        if !self.checked || v.ty.is_none() {
            return v;
        }
        if !self.work()
            || !self.charge_arena(1, SIZEOF_EMITTED)
            || !self.charge_arena(count, SIZEOF_PTR)
        {
            return InferValue::none();
        }
        let id = EmittedId(self.emitted.len() as u32);
        self.emitted.push(Emitted {
            operands: vec![None; count],
            ..Emitted::default()
        });
        v.graph = Some(id);
        v
    }

    pub(crate) fn record_syntax(&mut self, node: NodeId, v: InferValue) -> bool {
        self.record_inner(Some(node), None, v)
    }

    pub(crate) fn record_synthetic(
        &mut self,
        name: &'static str,
        span: Span,
        v: InferValue,
    ) -> bool {
        self.record_inner(None, Some((name, span)), v)
    }

    fn record_inner(
        &mut self,
        syntax: Option<NodeId>,
        synthetic_name: Option<(&'static str, Span)>,
        v: InferValue,
    ) -> bool {
        if !self.charge_arena(1, SIZEOF_NOTE) {
            return false;
        }
        if let Some(gid) = v.graph {
            if let Some(sym) = v.symbol {
                self.emitted[gid.index()].symbol = Some(sym);
            }
            if let Some(bc) = v.builtin_case {
                self.emitted[gid.index()].native = Some(bc.to_string());
            }
        }
        let Some(ty) = v.ty else {
            return false;
        };
        let nid = NoteId(self.notes.len() as u32);
        self.notes.push(Note {
            syntax,
            synthetic_name,
            resolved_type: None,
            binding_name: v.binding,
            symbol: v.symbol,
            field_index: v.field_index,
            builtin_case: v.builtin_case,
            never: v.never,
            ty,
            next: None,
            graph: v.graph,
            binding: v.bound,
            callee: v.callee,
            row_index: None,
            binding_index: None,
            variable: false,
            discarded: false,
            discard_result: false,
            negative: false,
        });
        if let Some(did) = v.dispatch
            && self.dispatches[did.index()].note.is_none()
        {
            self.dispatches[did.index()].note = Some(nid);
        }
        if let Some(tail) = self.notes_tail {
            self.notes[tail.index()].next = Some(nid);
        } else {
            self.notes_head = Some(nid);
        }
        self.notes_tail = Some(nid);
        true
    }

    pub(crate) fn parameter_type(&self, ty_id: TypeId, bit: u32) -> Option<TypeId> {
        let ty = self.signatures.types.get(ty_id);
        if (ty.parameters & bit) == 0 {
            return None;
        }
        if ty.kind == SourceTypeKind::Parameter {
            return Some(ty_id);
        }
        for &arg in &ty.arguments {
            if let Some(found) = self.parameter_type(arg, bit) {
                return Some(found);
            }
        }
        None
    }

    fn map_key_order(&mut self, a: &LiteralKeyEntry, b: &LiteralKeyEntry) -> Option<Ordering> {
        if !self.work() {
            return None;
        }
        if a.category != b.category {
            return Some(a.category.cmp(&b.category));
        }
        if a.version != b.version {
            return Some(a.version.cmp(&b.version));
        }
        if a.negative != b.negative {
            return Some(if a.negative {
                Ordering::Less
            } else {
                Ordering::Greater
            });
        }
        if a.bytes.len() != b.bytes.len() {
            return Some(a.bytes.len().cmp(&b.bytes.len()));
        }
        for _ in 0..a.bytes.len() {
            if !self.work() {
                return None;
            }
        }
        Some(a.bytes.cmp(&b.bytes))
    }

    pub(crate) fn check_map_keys(&mut self, node: NodeId, versions: &[u32]) -> bool {
        let entry_count = self.node(node).children.len();
        if entry_count < 2 {
            return true;
        }
        if !self.charge_arena(entry_count * 2, 48) {
            return false;
        }
        let mut keys: Vec<LiteralKeyEntry> = Vec::with_capacity(entry_count);
        for (i, child) in self.node(node).children.iter().enumerate() {
            let Some(entry_nid) = *child else {
                continue;
            };
            let Some(mut key_nid) = self.node(entry_nid).children.first().copied().flatten() else {
                continue;
            };
            while self.node(key_nid).kind == SyntaxKind::Group {
                let Some(inner) = self.node(key_nid).children.first().copied().flatten() else {
                    break;
                };
                key_nid = inner;
            }
            let key_span = self.node(key_nid).span;
            self.span = key_span;
            let tree = self.tree();
            let literal: MapLiteralKey = {
                let mut budget = InferMapBudget { state: self };
                let Some(val) = canonicalize_map_literal_key(&mut budget, tree, key_nid) else {
                    return false;
                };
                val
            };
            if self.node(key_nid).kind == SyntaxKind::Name {
                keys.push(LiteralKeyEntry {
                    category: 5,
                    bytes: self.node_text(key_nid).to_vec(),
                    negative: false,
                    span: key_span,
                    position: i,
                    version: versions[i],
                });
            } else if literal.category != MapKeyCategory::None {
                keys.push(LiteralKeyEntry {
                    category: literal.category as u8,
                    bytes: literal.bytes,
                    negative: literal.negative,
                    span: key_span,
                    position: i,
                    version: versions[i],
                });
            }
        }

        let count = keys.len();
        if count < 2 {
            return true;
        }
        let mut scratch = keys.clone();
        let mut width = 1usize;
        while width < count {
            let mut begin = 0usize;
            while begin < count {
                let mid = (begin + width).min(count);
                let end = (mid + width).min(count);
                let mut left = begin;
                let mut right = mid;
                for item in &mut scratch[begin..end] {
                    let take_left = if right == end {
                        true
                    } else if left < mid {
                        let Some(ord) = self.map_key_order(&keys[left], &keys[right]) else {
                            return false;
                        };
                        ord != Ordering::Greater
                    } else {
                        false
                    };
                    if take_left {
                        *item = keys[left].clone();
                        left += 1;
                    } else {
                        *item = keys[right].clone();
                        right += 1;
                    }
                }
                begin += 2 * width;
            }
            std::mem::swap(&mut keys, &mut scratch);
            width *= 2;
        }

        let mut first_span: Option<Span> = None;
        let mut repeated: Option<(Span, usize)> = None;
        for i in 1..count {
            let Some(ord) = self.map_key_order(&keys[i - 1], &keys[i]) else {
                return false;
            };
            if ord == Ordering::Equal && repeated.is_none_or(|(_, pos)| keys[i].position < pos) {
                first_span = Some(keys[i - 1].span);
                repeated = Some((keys[i].span, keys[i].position));
            }
        }
        if let (Some((rep_span, _)), Some(first)) = (repeated, first_span) {
            return self.problem(
                ErrorCode::MapKeyRepeated,
                rep_span,
                "a map literal gives the same key twice",
                Some("Each key in a map literal must be different (reference 3.8)."),
                Some(first),
            );
        }
        true
    }
}
