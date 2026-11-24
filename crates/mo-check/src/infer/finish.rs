//! Type finalization, exact numeric literal range validation, captures & capability check,
//! Maranget pattern exhaustiveness & Byte-256 normalization, ability proving, `Sqlite.decode`
//! column validation, `try` error-wrapper resolution, and normalized `CheckedPattern` building
//! (`src/check/infer-finish.c` parity).

use mo_base::Label;
use mo_def::ErrorCode;
use mo_syntax::{NodeId, SyntaxKind, TokenKind};

use crate::abilities::{SourceAbility, TypeArgument};
use crate::checked::{
    CheckedAbilityUse, CheckedPattern, CheckedPatternKind, CheckedPatternPart, ConstructorRef,
    InferredExpression,
};
use crate::ids::{
    CheckedBindingId, CheckedExprId, CheckedPatternId, DataSigId, ProofId, SymRef, TypeId,
};
use crate::patterns::{CoverageOutcome, KEY_BYTE_BASE, patterns_cover};
use crate::types::{
    SourceType, SourceTypeKind, TypeOwner, builtin_case_by_key, builtin_case_by_text,
    builtin_case_by_type, is_capability_name,
};

use super::{
    EmittedId, InferState, NUM_INT, NoteId, SIZEOF_CHECKED_PATTERN, SIZEOF_PTR, SIZEOF_SOURCE_TYPE,
    TermId, TermShape,
};

/// Exact nearest-even overflow boundary for IEEE 754 binary64: `2^1024 - 2^970`.
const FLOAT_DECIMAL_BOUNDARY: &[u8] = b"\
179769313486231580793728971405303415079934132710037826936173778980444968292764750946649\
017977587207096330286416692887910946555547851940402630657488671505820681908902000708383\
676273854845817711531764475730270069855571366959622842914819860834936475292719074168444\
365510704342711559699508093042880177904174497792";

fn finite_integer(text: &[u8], base: u32) -> bool {
    let mut bits = 0u32;
    let mut smaller = false;
    for &c in text.iter().skip(2) {
        if c == b'_' {
            continue;
        }
        let digit: u32 = match c {
            b'a'..=b'f' => u32::from(c - b'a') + 10,
            b'A'..=b'F' => u32::from(c - b'A') + 10,
            _ => u32::from(c - b'0'),
        };
        let bit_count = if base == 16 { 4u32 } else { 1u32 };
        for j in (1..=bit_count).rev() {
            let bit = (digit >> (j - 1)) & 1;
            if bits == 0 && bit == 0 {
                continue;
            }
            bits += 1;
            if bits <= 54 && bit == 0 {
                smaller = true;
            }
            if bits > 1024 {
                return false;
            }
        }
    }
    bits < 1024 || smaller
}

fn finite_decimal(text: &[u8]) -> bool {
    let mut digits = 0usize;
    let mut first = 0usize;
    for (i, &c) in text.iter().enumerate() {
        if c == b'.' {
            break;
        }
        if c == b'_' || (digits == 0 && c == b'0') {
            continue;
        }
        if digits == 0 {
            first = i;
        }
        digits += 1;
    }
    if digits != FLOAT_DECIMAL_BOUNDARY.len() {
        return digits < FLOAT_DECIMAL_BOUNDARY.len();
    }
    let mut at = 0usize;
    for &c in &text[first..] {
        if c == b'.' {
            break;
        }
        if c == b'_' {
            continue;
        }
        debug_assert!(at < FLOAT_DECIMAL_BOUNDARY.len());
        if c != FLOAT_DECIMAL_BOUNDARY[at] {
            return c < FLOAT_DECIMAL_BOUNDARY[at];
        }
        at += 1;
    }
    false
}

impl<'a> InferState<'a> {
    pub(crate) fn iter_notes(&self) -> NoteIter<'_> {
        NoteIter {
            notes: &self.notes,
            cur: self.notes_head,
        }
    }

    pub(crate) fn finish_type(&mut self, t: TermId, depth: u32) -> Option<TypeId> {
        if !self.work() || !self.type_depth(depth) {
            return None;
        }
        let t = self.root(t);
        if let Some(resolved) = self.terms[t.index()].resolved {
            let height = self.terms[t.index()].height;
            if !self.type_depth(depth + height) {
                return None;
            }
            return Some(resolved);
        }
        if self.terms[t.index()].variable {
            let num = self.terms[t.index()].numeric;
            if num == 0 {
                let _ = self.problem(
                    ErrorCode::CannotInfer,
                    self.span,
                    "no use decides this expression's type",
                    Some(
                        "A function's signature gives its types; inside a body types are \
                         inferred from all their uses (reference 5.1).",
                    ),
                    None,
                );
                return None;
            }
            let default_name = if (num & NUM_INT) != 0 {
                "Int"
            } else {
                "Decimal"
            };
            let term = &mut self.terms[t.index()];
            term.variable = false;
            term.partial_default = self.partial;
            term.shape = TermShape {
                kind: SourceTypeKind::Named,
                name: default_name.to_string(),
                module: None,
                owner: TypeOwner::None,
                arity: 0,
            };
        }

        let arity = self.terms[t.index()].shape.arity;
        if arity != 0 && !self.charge_arena(arity, SIZEOF_PTR) {
            return None;
        }
        let mut arg_types = Vec::with_capacity(arity);
        let mut partial_default = self.terms[t.index()].partial_default;
        let mut height = self.terms[t.index()].height;
        for i in 0..arity {
            let child_term = self.argument(t, i)?;
            let child_ty = self.finish_type(child_term, depth + 1)?;
            let cr = self.root(child_term);
            partial_default = partial_default || self.terms[cr.index()].partial_default;
            let ch = self.terms[cr.index()].height + 1;
            if ch > height {
                height = ch;
            }
            arg_types.push(child_ty);
        }
        if !self.charge_arena(1, SIZEOF_SOURCE_TYPE) {
            return None;
        }
        let shape = self.terms[t.index()].shape.clone();
        let out_id = self.signatures.types.intern(
            shape.kind,
            shape.name,
            shape.module,
            shape.owner,
            arg_types,
        );
        let term = &mut self.terms[t.index()];
        term.partial_default = partial_default;
        term.height = height;
        term.resolved = Some(out_id);
        Some(out_id)
    }

    fn is_number_parameter(&self, ty_id: TypeId) -> bool {
        let ty = self.signatures.types.get(ty_id);
        if ty.kind != SourceTypeKind::Parameter {
            return false;
        }
        let Some(owner_sym) = self.enclosing.owner_symbol else {
            return false;
        };
        if ty.owner != TypeOwner::Source(owner_sym) {
            return false;
        }
        let Some(letter) = ty.param_letter() else {
            return false;
        };
        (self.enclosing.requirements[letter] & SourceAbility::Number.bit()) != 0
    }

    fn check_literal_range(&mut self, note_id: NoteId) -> bool {
        let (syntax_opt, ty_opt, negative) = {
            let n = &self.notes[note_id.index()];
            (n.syntax, n.resolved_type, n.negative)
        };
        let Some(node_id) = syntax_opt else {
            return true;
        };
        let node = self.node(node_id);
        if !matches!(node.kind, SyntaxKind::Int | SyntaxKind::Decimal) {
            return true;
        }
        let Some(ty_id) = ty_opt else {
            return true;
        };
        let ty_name = self.signatures.types.get(ty_id).name.as_str();
        if ty_name == "BigInt" {
            return true;
        }
        let text = self.node_text(node_id);
        let base: u32 = if text.len() > 2 && text[1] == b'x' {
            16
        } else if text.len() > 2 && text[1] == b'b' {
            2
        } else {
            10
        };
        let start = if base == 10 { 0usize } else { 2usize };
        let mut magnitude: u128 = 0;
        let mut places: u32 = 0;
        let mut fraction = false;
        let mut overflow = false;
        for &c in &text[start..] {
            if c == b'_' {
                continue;
            }
            if c == b'.' {
                fraction = true;
                continue;
            }
            let digit: u128 = match c {
                b'a'..=b'f' => u128::from(c - b'a') + 10,
                b'A'..=b'F' => u128::from(c - b'A') + 10,
                _ => u128::from(c - b'0'),
            };
            debug_assert!(digit < u128::from(base));
            if magnitude > (u128::MAX - digit) / u128::from(base) {
                overflow = true;
            } else if !overflow {
                magnitude = magnitude * u128::from(base) + digit;
            }
            if fraction {
                places += 1;
            }
        }
        let mut fits = !overflow;
        if ty_name == "Int" || self.is_number_parameter(ty_id) {
            let limit = (i64::MAX as u128) + u128::from(negative);
            fits = fits && magnitude <= limit;
        } else if ty_name == "Byte" {
            fits = fits && !negative && magnitude <= 255;
        } else if ty_name == "Decimal" {
            let maximum: u128 = (1u128 << 127) - 1;
            fits = fits && places <= 18;
            for _ in places..18 {
                if !fits {
                    break;
                }
                if magnitude > maximum / 10 {
                    fits = false;
                } else {
                    magnitude *= 10;
                }
            }
            fits = fits && magnitude <= maximum;
        } else if ty_name == "Float" {
            fits = if base == 10 {
                finite_decimal(text)
            } else {
                finite_integer(text, base)
            };
        }
        if fits {
            true
        } else {
            let span = node.span;
            self.problem(
                ErrorCode::LiteralRange,
                span,
                "a number literal doesn't fit its type",
                Some(
                    "A number literal takes its context's numeric type and must fit that type's \
                     range (reference 3.3).",
                ),
                None,
            )
        }
    }

    pub(crate) fn finish_notes(&mut self, mut items: Option<&mut Vec<InferredExpression>>) -> bool {
        let note_ids: Vec<NoteId> = self.iter_notes().collect();
        for nid in note_ids {
            let span = self.note_span(nid);
            self.span = span;
            let callee_opt = self.notes[nid.index()].callee;
            if let Some(callee_id) = callee_opt {
                let callee_sym = self.notes[callee_id.index()].symbol;
                self.notes[nid.index()].symbol = callee_sym;
                if let (Some(ng), Some(cg)) = (
                    self.notes[nid.index()].graph,
                    self.notes[callee_id.index()].graph,
                ) {
                    let copied = {
                        let ce = &self.emitted[cg.index()];
                        (
                            ce.symbol,
                            ce.function_type,
                            ce.requirements,
                            ce.parameters.clone(),
                            ce.json_decode,
                        )
                    };
                    let ge = &mut self.emitted[ng.index()];
                    ge.symbol = copied.0;
                    ge.function_type = copied.1;
                    ge.requirements = copied.2;
                    ge.parameters = copied.3;
                    ge.json_decode = copied.4;
                }
            }
            if self.partial
                && let Some(ref err) = self.error
                && span.start >= err.primary.span.start
            {
                continue;
            }
            let note_term = self.notes[nid.index()].ty;
            let resolved = self.finish_type(note_term, 0);
            self.notes[nid.index()].resolved_type = resolved;
            if resolved.is_some() {
                let _ = self.check_literal_range(nid);
                let (discarded, discard_result) = {
                    let n = &self.notes[nid.index()];
                    (n.discarded, n.discard_result)
                };
                if !self.fatal
                    && ((discarded && !self.named(note_term, "Unit"))
                        || (discard_result && self.named(note_term, "Result")))
                {
                    let is_res = self.named(note_term, "Result");
                    self.defer(
                        if is_res {
                            ErrorCode::FailureIgnored
                        } else {
                            ErrorCode::UnusedValue
                        },
                        self.span,
                        if is_res {
                            "a failure is ignored"
                        } else {
                            "a result isn't used"
                        },
                        Some(if is_res {
                            "Mo never drops a failure silently (reference 8.3)."
                        } else {
                            "A result that isn't Unit and isn't used is an error (reference 6.1)."
                        }),
                    );
                }
            }
            if self.fatal {
                return false;
            }
            self.failed = false;
            if let Some(ref mut vec) = items {
                vec.push(self.to_inferred_expr(nid));
            }
        }

        for i in 0..self.locals.len() {
            let (l_span, ends_q, l_ty) = {
                let l = &self.locals[i];
                let sp = l.name.span(self.tree());
                let eq = l.name_key.last() == Some(&b'?');
                (sp, eq, l.ty)
            };
            self.span = l_span;
            if (self.partial
                && self
                    .error
                    .as_ref()
                    .is_some_and(|e| l_span.start >= e.primary.span.start))
                || !ends_q
            {
                continue;
            }
            let resolved = self.finish_type(l_ty, 0);
            if self.fatal {
                return false;
            }
            self.failed = false;
            let Some(mut ty_id) = resolved else {
                continue;
            };
            if self.signatures.types.get(ty_id).kind == SourceTypeKind::Function
                && let Some(&last) = self.signatures.types.get(ty_id).arguments.last()
            {
                ty_id = last;
            }
            if !self.signatures.types.get(ty_id).is_builtin_named("Bool") {
                self.defer(
                    ErrorCode::BoolName,
                    self.span,
                    "a name ending in ? isn't a yes-or-no answer",
                    Some(
                        "A name ending in ? is a Bool, or a function that returns one \
                         (reference 2.3).",
                    ),
                );
            }
        }

        self.error.is_none()
    }

    fn capture_type(
        &mut self,
        ty_id: TypeId,
        capability: &mut bool,
        unknown: &mut u32,
        depth: u32,
    ) -> bool {
        if !self.work() || !self.type_depth(depth) {
            return false;
        }
        let (kind, owner, module, name, args) = {
            let t = self.signatures.types.get(ty_id);
            (
                t.kind,
                t.owner,
                t.module,
                t.name.clone(),
                t.arguments.clone(),
            )
        };
        if kind == SourceTypeKind::Function {
            return true;
        }
        if kind == SourceTypeKind::Parameter {
            let letter = (name.as_bytes()[0] - b'A') as usize;
            debug_assert!(letter < 26);
            let is_number = owner
                .as_source()
                .and_then(|s| self.signatures.function_signature_by_symbol(s))
                .is_some_and(|(_, f)| (f.requirements[letter] & SourceAbility::Number.bit()) != 0);
            if !is_number {
                *unknown |= 1u32 << letter;
            }
            return true;
        }
        if kind == SourceTypeKind::Named
            && let Some(owner_sym) = owner.as_source()
        {
            let Some((_, nom)) = self.signatures.nominal_signature_by_symbol(owner_sym) else {
                debug_assert!(self.signatures.recovering);
                return true;
            };
            let holds_cap = nom.holds_capability;
            let cap_params = nom.capture_parameters;
            let nom_args = self.signatures.types.get(nom.ty).arguments.clone();
            *capability = *capability || holds_cap;
            for (i, &arg_ty) in args.iter().enumerate() {
                let param_bits = self.signatures.types.get(nom_args[i]).parameters;
                if (cap_params & param_bits) != 0
                    && !self.capture_type(arg_ty, capability, unknown, depth + 1)
                {
                    return false;
                }
            }
            return true;
        }
        if kind == SourceTypeKind::Named && module.is_none() && is_capability_name(&name) {
            *capability = true;
        }
        for arg_ty in args {
            if !self.capture_type(arg_ty, capability, unknown, depth + 1) {
                return false;
            }
        }
        true
    }

    fn capture_assignment(&mut self, lam_idx: usize, cap_idx: usize) -> bool {
        let (local_id, use_span, lam_syntax, lam_loop, lam_path) = {
            let lam = &self.lambdas[lam_idx];
            let cap = lam.captures[cap_idx];
            (cap.local, cap.use_span, lam.syntax, lam.loop_ctx, lam.path)
        };
        if !self.locals[local_id.index()].variable {
            return true;
        }
        let local_start = self.locals[local_id.index()].name.span(self.tree()).start;
        let lam_start = self.node(lam_syntax).span.start;
        let mut cur_write = self.locals[local_id.index()].writes;
        while let Some(wid) = cur_write {
            if !self.work() {
                return false;
            }
            let w = self.capture_writes[wid.index()];
            let w_span = self.node(w.syntax).span;
            let mut repeated = false;
            let mut cur_loop = lam_loop;
            while let Some(lid) = cur_loop {
                if !self.work() {
                    return false;
                }
                let lp = self.capture_loops[lid.index()];
                let lspan = self.node(lp.syntax).span;
                repeated = repeated
                    || (local_start < lspan.start
                        && w_span.start >= lspan.start
                        && w_span.start < lspan.end);
                cur_loop = lp.parent;
            }
            if !repeated && w_span.end <= lam_start {
                cur_write = w.next;
                continue;
            }
            let mut exclusive = false;
            if !repeated && w.path.is_some() {
                let mut ca = lam_path;
                while let Some(aid) = ca {
                    let pa = self.capture_paths[aid.index()];
                    let mut cb = w.path;
                    while let Some(bid) = cb {
                        if !self.work() {
                            return false;
                        }
                        let pb = self.capture_paths[bid.index()];
                        exclusive = exclusive || (pa.group == pb.group && pa.arm != pb.arm);
                        cb = pb.parent;
                    }
                    ca = pa.parent;
                }
            }
            if exclusive {
                cur_write = w.next;
                continue;
            }
            let target_span = self.node(w.target).span;
            return self.problem(
                ErrorCode::CaptureAssignment,
                use_span,
                "an anonymous function reads a var assigned after it is made",
                Some(
                    "Copy the value into a name first; later rounds of an enclosing loop also \
                     count as later assignments (reference 5.6).",
                ),
                Some(target_span),
            );
        }
        true
    }

    pub(crate) fn finish_captures(&mut self) -> bool {
        let lam_count = self.lambdas.len();
        for lam_idx in (0..lam_count).rev() {
            let cap_count = self.lambdas[lam_idx].captures.len();
            for cap_idx in 0..cap_count {
                let cap = self.lambdas[lam_idx].captures[cap_idx];
                self.span = cap.use_span;
                if self.partial
                    && self
                        .error
                        .as_ref()
                        .is_some_and(|e| cap.use_span.start >= e.primary.span.start)
                {
                    continue;
                }
                let ty_opt = self.locals[cap.local.index()]
                    .binding
                    .and_then(|bn| self.notes[bn.index()].resolved_type);
                let Some(ty_id) = ty_opt else {
                    debug_assert!(self.error.is_some());
                    continue;
                };
                let mut capability = false;
                let mut unknown = 0u32;
                if !self.capture_type(ty_id, &mut capability, &mut unknown, 0) {
                    return false;
                }
                if capability {
                    let prev_span = self.locals[cap.local.index()].name.span(self.tree());
                    let _ = self.problem(
                        ErrorCode::CapabilityCapture,
                        cap.use_span,
                        "an anonymous function can't capture a capability",
                        Some(
                            "Anonymous functions can't capture capabilities, or values holding \
                             them (reference 11.4).",
                        ),
                        Some(prev_span),
                    );
                } else {
                    self.capture_free |= unknown;
                    if !self.capture_assignment(lam_idx, cap_idx) && self.fatal {
                        return false;
                    }
                }
                if self.fatal {
                    return false;
                }
                self.failed = false;
            }
        }
        self.error.is_none()
    }

    pub(crate) fn finish_patterns(&mut self) -> bool {
        // Normalize Byte literals to 256..=511 finite domain.
        for i in 0..self.typed_patterns.len() {
            let tp = self.typed_patterns[i].clone();
            let span = self.node(tp.syntax).span;
            if self
                .error
                .as_ref()
                .is_some_and(|e| span.end > e.primary.span.start)
            {
                continue;
            }
            if self.node(tp.syntax).kind != SyntaxKind::Int || !self.named(tp.ty, "Byte") {
                continue;
            }
            let text = self.node_text(tp.syntax);
            let base: u32 = if text.len() > 2 && text[1] == b'x' {
                16
            } else if text.len() > 2 && text[1] == b'b' {
                2
            } else {
                10
            };
            let start = if base == 10 { 0usize } else { 2usize };
            let mut value = 0u32;
            for &c in &text[start..] {
                if c == b'_' {
                    continue;
                }
                let digit: u32 = match c {
                    b'a'..=b'f' => u32::from(c - b'a') + 10,
                    b'A'..=b'F' => u32::from(c - b'A') + 10,
                    _ => u32::from(c - b'0'),
                };
                debug_assert!(digit < base && value <= (255 - digit) / base);
                value = value * base + digit;
            }
            if !self.charge_arena(16, 1) {
                return true;
            }
            let mp = self.match_patterns.get_mut(tp.match_id);
            mp.key = KEY_BYTE_BASE + u64::from(value);
            mp.signature = 256;
            mp.name = value.to_string();
        }

        // Check matches in reverse push order (matching C linked list head-prepend).
        for i in (0..self.match_checks.len()).rev() {
            let check = self.match_checks[i].clone();
            if self
                .error
                .as_ref()
                .is_some_and(|e| check.end > e.primary.span.start)
            {
                continue;
            }
            self.span = check.span;
            let ty_root = self.root(check.ty);
            let (is_var, resolved_opt, partial_def, owner) = {
                let t = &self.terms[ty_root.index()];
                (t.variable, t.resolved, t.partial_default, t.shape.owner)
            };
            if is_var || partial_def {
                continue;
            }
            let Some(resolved_ty) = resolved_opt else {
                continue;
            };
            let choice = owner
                .as_source()
                .is_some_and(|s| self.interfaces.symbol(s).decl_kind == SyntaxKind::Enum)
                || self.named(ty_root, "Option")
                || self.named(ty_root, "Result")
                || self.named(ty_root, "Ordering")
                || self.named(ty_root, "EntryKind")
                || self.named(ty_root, "FileError")
                || self.named(ty_root, "SimulationError")
                || self.named(ty_root, "NetworkError")
                || self.named(ty_root, "SqliteError")
                || self.named(ty_root, "Sqlite.Value");
            if let Some(catch_all_nid) = check.catch_all
                && choice
            {
                let span = self.node(catch_all_nid).span;
                self.defer(
                    ErrorCode::CaseCatchAll,
                    span,
                    "a choice arm is a catch-all",
                    Some(
                        "A whole arm over a choice type can't be _ or a name; list the cases \
                         (reference 7.2).",
                    ),
                );
                continue;
            }
            let mut program_work = self.signatures.work;
            let outcome = patterns_cover(
                &mut self.match_patterns,
                self.interfaces,
                self.signatures,
                &check.patterns,
                resolved_ty,
                &mut self.work,
                &mut program_work,
                self.arena_remaining,
                self.diags,
                check.span,
            );
            self.signatures.work = program_work;
            match outcome {
                CoverageOutcome::LimitFailed => return false,
                CoverageOutcome::Exhaustive => {}
                CoverageOutcome::Missing(missing) => {
                    if !self.charge_arena(8192, 1) {
                        return true;
                    }
                    let msg = format!(
                        "case doesn't cover every possible value; {}missing: {missing}",
                        if check.patterns.is_empty() {
                            "guards don't count toward coverage; "
                        } else {
                            ""
                        }
                    );
                    self.defer(
                        ErrorCode::CaseIncomplete,
                        check.span,
                        msg,
                        Some("The arms must cover every possible value (reference 7.2)."),
                    );
                    let scr_span = self.node(check.scrutinee).span;
                    if let Some(err) = self.error.as_mut()
                        && err.code == ErrorCode::CaseIncomplete
                        && err.primary.span.start == check.span.start
                    {
                        err.secondary = vec![Label::with_text(scr_span, "matched value")];
                    }
                }
            }
        }
        true
    }

    pub(crate) fn finish_operator(&mut self, note_id: NoteId) -> bool {
        let Some(gid) = self.notes[note_id.index()].graph else {
            return true;
        };
        let (json_decode, op, native, op_count, recv_slot) = {
            let g = &self.emitted[gid.index()];
            (
                g.json_decode,
                g.operation,
                g.native.clone(),
                g.operands.len(),
                g.receiver_slot,
            )
        };
        let note_span = self.note_span(note_id);
        if json_decode {
            self.span = note_span;
            let input_ty = if op_count == 0 {
                let nty = self.notes[note_id.index()].resolved_type;
                nty.and_then(|t| self.signatures.types.get(t).arguments.first().copied())
            } else {
                self.emitted[gid.index()].operands[op_count - 1]
                    .and_then(|on| self.notes[on.index()].resolved_type)
            };
            let valid_input = input_ty.is_some_and(|t| {
                let st = self.signatures.types.get(t);
                st.is_builtin_named("Text") || st.is_builtin_named("Bytes")
            });
            if !valid_input {
                return self.problem(
                    ErrorCode::ArgumentType,
                    self.span,
                    "Json.decode takes Text or Bytes",
                    Some("Pass UTF-8 JSON Text or raw Bytes to Json.decode or Json.decode_strict."),
                    None,
                );
            }
        }
        let text = native.as_deref() == Some("to_text");
        let compare = native.as_deref() == Some("compare");
        let to_json = native.as_deref() == Some("to_json");
        if (op == TokenKind::Eof && !text && !compare && !to_json)
            || matches!(
                op,
                TokenKind::And
                    | TokenKind::Or
                    | TokenKind::Implies
                    | TokenKind::Not
                    | TokenKind::DotDot
            )
        {
            return true;
        }
        let ability = if to_json {
            SourceAbility::Json
        } else if text {
            SourceAbility::Text
        } else if matches!(op, TokenKind::Eq | TokenKind::Ne) {
            SourceAbility::Equal
        } else if compare
            || matches!(
                op,
                TokenKind::Lt | TokenKind::Le | TokenKind::Gt | TokenKind::Ge
            )
        {
            SourceAbility::Order
        } else {
            SourceAbility::Number
        };
        let qualified = (text || compare || to_json) && (op_count == 0 || recv_slot.is_none());
        let operand_note = if op_count == 0 {
            note_id
        } else {
            let Some(op0) = self.emitted[gid.index()].operands[0] else {
                return false;
            };
            op0
        };
        let Some(mut ty_id) = self.notes[operand_note.index()].resolved_type else {
            return false;
        };
        if qualified {
            let ft = self.signatures.types.get(ty_id);
            debug_assert!(ft.kind == SourceTypeKind::Function && !ft.arguments.is_empty());
            ty_id = ft.arguments[0];
        }
        self.span = note_span;
        let mut module = None;
        for i in 0..self.ability_modules.len() {
            if !self.work() {
                return false;
            }
            if self.ability_modules[i].0 == gid {
                module = Some(self.ability_modules[i].1);
            }
        }
        if let Some(mod_id) = module {
            let st = self.signatures.types.get(ty_id);
            let declared_here = st.kind == SourceTypeKind::Named
                && st.owner.as_source().is_some()
                && st.module == Some(mod_id);
            if !declared_here {
                let mod_name = self.interfaces.scope(mod_id).name.as_deref().unwrap_or("");
                let ty_desc = match st.kind {
                    SourceTypeKind::Function => "functions",
                    SourceTypeKind::Tuple => "tuples",
                    SourceTypeKind::Named | SourceTypeKind::Parameter => st.name.as_str(),
                };
                self.type_label = Some(format!(
                    "{:.100} doesn't declare {:.100}",
                    mod_name, ty_desc
                ));
                let res = self.problem(
                    ErrorCode::ArgumentType,
                    self.span,
                    "a module's to_text or compare is given a type it doesn't declare",
                    Some(
                        "M.to_text(x) and M.compare(a, b) take a value of a type that the module \
                         M declares (reference 5.4).",
                    ),
                    None,
                );
                self.type_label = None;
                return res;
            }
        }
        if ability == SourceAbility::Number {
            let st = self.signatures.types.get(ty_id);
            if st.is_builtin_named("Time") || st.is_builtin_named("Duration") {
                return true;
            }
        }
        let proof = match self.prove_ability(ty_id, ability) {
            Ok(p) => p,
            Err(err) => {
                return self.problem(err.code, self.span, err.message, err.reason, None);
            }
        };
        if !self.charge_arena(1, 24) {
            return false;
        }
        let op_row = self.notes[operand_note.index()]
            .row_index
            .map(CheckedExprId);
        self.emitted[gid.index()].abilities = vec![CheckedAbilityUse {
            operand: op_row,
            implementation: proof,
            parameter: None,
        }];
        true
    }

    pub(crate) fn finish_interpolation(&mut self, note_id: NoteId) -> bool {
        let Some(snid) = self.notes[note_id.index()].syntax else {
            return true;
        };
        if self.node(snid).kind != SyntaxKind::TextInterpolated {
            return true;
        }
        let Some(gid) = self.notes[note_id.index()].graph else {
            return true;
        };
        let ops = self.emitted[gid.index()].operands.clone();
        if !self.charge_arena(ops.len(), 24) {
            return false;
        }
        let mut uses = Vec::with_capacity(ops.len());
        for on in ops {
            let Some(hole_id) = on else {
                return false;
            };
            let Some(hole_ty) = self.notes[hole_id.index()].resolved_type else {
                return false;
            };
            let hole_span = self.note_span(hole_id);
            let proof = match self.prove_ability(hole_ty, SourceAbility::Text) {
                Ok(p) => p,
                Err(err) => {
                    return self.problem(err.code, hole_span, err.message, err.reason, None);
                }
            };
            uses.push(CheckedAbilityUse {
                operand: self.notes[hole_id.index()].row_index.map(CheckedExprId),
                implementation: proof,
                parameter: None,
            });
        }
        self.emitted[gid.index()].abilities = uses;
        true
    }

    fn pattern_field_type(
        &mut self,
        data_id: DataSigId,
        actual: TypeId,
        index: usize,
    ) -> Option<TypeId> {
        let (data_ty, field_ty) = {
            let d = &self.signatures.data[data_id.index()];
            (d.ty, d.field_by_index(index).ty)
        };
        let data_args = self.signatures.types.get(data_ty).arguments.clone();
        let actual_args = self.signatures.types.get(actual).arguments.clone();
        let mut parameters = [None; 26];
        for (i, &act_arg) in actual_args.iter().enumerate() {
            let letter = self
                .signatures
                .types
                .get(data_args[i])
                .param_letter()
                .unwrap_or(0);
            parameters[letter] = Some(self.annotation(act_arg, None, 0)?);
        }
        let term = self.annotation(field_ty, Some(&mut parameters), 0)?;
        self.finish_type(term, 0)
    }

    fn finish_sqlite_decode(&mut self, gid: EmittedId, actual: TypeId) -> bool {
        let (kind, owner, params) = {
            let t = self.signatures.types.get(actual);
            (t.kind, t.owner, t.parameters)
        };
        let Some(owner_sym) = owner.as_source().filter(|&s| {
            kind == SourceTypeKind::Named
                && self.interfaces.symbol(s).decl_kind == SyntaxKind::Struct
                && params == 0
        }) else {
            return self.problem(
                ErrorCode::ArgumentType,
                self.span,
                "Sqlite.decode requires a concrete struct type",
                Some(
                    "Sqlite.decode maps SQL column names to named struct fields, so its target \
                     type T must be a concrete struct.",
                ),
                None,
            );
        };
        if !self.visible_data(owner_sym) {
            return false;
        }
        let data_id = self
            .signatures
            .data_signature_by_symbol(owner_sym)
            .map(|(id, _)| id);
        let Some(did) = data_id.filter(|_| self.signatures.signature_type_available(Some(actual)))
        else {
            debug_assert!(self.signatures.recovering);
            self.failed = true;
            self.fatal = true;
            self.unavailable = true;
            return false;
        };
        let field_count = self.signatures.data[did.index()].fields.len();
        if field_count != 0 && !self.charge_arena(field_count, 24) {
            return false;
        }
        let mut json_uses: Vec<CheckedAbilityUse> = Vec::new();
        for k in 0..field_count {
            if !self.work() {
                return false;
            }
            let Some(ft) = self.pattern_field_type(did, actual, k) else {
                return false;
            };
            let field_span = self.signatures.data[did.index()]
                .field_by_index(k)
                .name_span;
            let is_opt = {
                let s = self.signatures.types.get(ft);
                s.is_builtin_named("Option") && s.arguments.len() == 1
            };
            let ut = if is_opt {
                self.signatures.types.get(ft).arguments[0]
            } else {
                ft
            };
            if self.signatures.types.get(ut).is_builtin_named("Option") {
                return self.problem(
                    ErrorCode::ArgumentType,
                    self.span,
                    "Sqlite.decode does not support nested Option fields",
                    Some(
                        "SQLite has one SQL NULL state, so struct fields decoded from SQLite may \
                         use at most one Option(T) layer.",
                    ),
                    field_span,
                );
            }
            let (u_kind, u_mod, u_owner, u_name, u_arity) = {
                let s = self.signatures.types.get(ut);
                (
                    s.kind,
                    s.module,
                    s.owner,
                    s.name.as_str(),
                    s.arguments.len(),
                )
            };
            let direct_scalar = (u_kind == SourceTypeKind::Named
                && u_mod.is_none()
                && u_arity == 0
                && matches!(
                    u_name,
                    "Int" | "Bool" | "Float" | "Text" | "Bytes" | "Decimal" | "Time"
                ))
                || (u_kind == SourceTypeKind::Named
                    && u_owner.as_source().is_some()
                    && u_mod.is_some_and(|m| {
                        let sc = self.interfaces.scope(m);
                        sc.library && sc.name.as_deref() == Some("Date")
                    })
                    && u_name == "Date");
            if direct_scalar {
                continue;
            }
            let json_eligible = u_kind == SourceTypeKind::Tuple
                || (u_kind == SourceTypeKind::Named
                    && u_owner.as_source().is_some_and(|s| {
                        matches!(
                            self.interfaces.symbol(s).decl_kind,
                            SyntaxKind::Struct | SyntaxKind::Enum
                        )
                    }))
                || (u_kind == SourceTypeKind::Named
                    && u_mod.is_none()
                    && matches!(u_name, "List" | "Map" | "Set"));
            if !json_eligible {
                return self.problem(
                    ErrorCode::ArgumentType,
                    self.span,
                    "Sqlite.decode cannot decode a field of this type",
                    Some(
                        "Supported column types are Int, Bool, Float, Text, Bytes, Decimal, Time, \
                         Date, Option(U), and Json-capable structs, enums or collections.",
                    ),
                    field_span,
                );
            }
            let jproof = match self.prove_ability(ut, SourceAbility::Json) {
                Ok(p) => p,
                Err(err) => {
                    return self.problem(err.code, self.span, err.message, err.reason, field_span);
                }
            };
            json_uses.push(CheckedAbilityUse {
                operand: Some(CheckedExprId(k as u32)),
                implementation: jproof,
                parameter: None,
            });
        }
        self.emitted[gid.index()].abilities = json_uses;
        true
    }

    pub(crate) fn finish_function_use(&mut self, gid: EmittedId) -> bool {
        let Some(fn_ty_id) = self.emitted[gid.index()].function_type else {
            return true;
        };
        let (params_mask, reqs, param_terms, sym_opt) = {
            let g = &self.emitted[gid.index()];
            (
                self.signatures.types.get(fn_ty_id).parameters,
                g.requirements,
                g.parameters.clone(),
                g.symbol,
            )
        };
        let count = params_mask.count_ones() as usize;
        let mut proof_count = 0usize;
        for r in reqs {
            proof_count += (r & 0x3f).count_ones() as usize;
        }
        if !self.charge_arena(count, 16) || !self.charge_arena(proof_count, 24) {
            return false;
        }
        let mut arguments = Vec::with_capacity(count);
        let mut proofs = Vec::with_capacity(proof_count);
        let mut at = 0usize;
        for (i, &req_mask) in reqs.iter().enumerate() {
            let bit = 1u32 << i;
            if (params_mask & bit) == 0 {
                continue;
            }
            let Some(actual) = self.finish_type(param_terms[at], 0) else {
                return false;
            };
            let param_type = match self.parameter_type(fn_ty_id, bit) {
                Some(pt) => pt,
                None => {
                    // Phantom parameter (e.g. `Json.schema[T]()`).
                    if !self.charge_arena(1, SIZEOF_SOURCE_TYPE) {
                        return false;
                    }
                    let pname = char::from(b'A' + (i as u8)).to_string();
                    let owner = match sym_opt {
                        Some(s) => TypeOwner::Source(s),
                        None => TypeOwner::None,
                    };
                    self.signatures.types.alloc_override(SourceType {
                        kind: SourceTypeKind::Parameter,
                        name: pname,
                        module: sym_opt.map(|s| s.module),
                        owner,
                        arguments: Vec::new(),
                        height: 0,
                        parameters: bit,
                    })
                }
            };
            arguments.push(TypeArgument {
                parameter: param_type,
                ty: actual,
            });
            at += 1;
            for ability in SourceAbility::ALL {
                if (req_mask & ability.bit()) == 0 {
                    continue;
                }
                let imp = match self.prove_ability(actual, ability) {
                    Ok(p) => p,
                    Err(err) => {
                        return self.problem(err.code, self.span, err.message, err.reason, None);
                    }
                };
                proofs.push(CheckedAbilityUse {
                    operand: None,
                    implementation: imp,
                    parameter: Some(param_type),
                });
            }
        }
        self.emitted[gid.index()].arguments = arguments.clone();
        self.emitted[gid.index()].abilities = proofs;
        if let Some(sym_ref) = sym_opt {
            let sym = self.interfaces.symbol(sym_ref);
            let scope = self.interfaces.scope(sym.module);
            if scope.library
                && scope.name.as_deref() == Some("Sqlite")
                && count == 1
                && matches!(
                    sym.name.as_str(),
                    "decode" | "decode_one" | "decode_optional"
                )
            {
                return self.finish_sqlite_decode(gid, arguments[0].ty);
            }
        }
        true
    }

    pub(crate) fn type_arguments(
        &mut self,
        generic: TypeId,
        actual: TypeId,
    ) -> Option<Vec<TypeArgument>> {
        let g_args = self.signatures.types.get(generic).arguments.clone();
        if g_args.is_empty() {
            return Some(Vec::new());
        }
        let a_args = self.signatures.types.get(actual).arguments.clone();
        debug_assert_eq!(g_args.len(), a_args.len());
        if !self.charge_arena(g_args.len(), 16) {
            return None;
        }
        Some(
            g_args
                .into_iter()
                .zip(a_args)
                .map(|(parameter, ty)| TypeArgument { parameter, ty })
                .collect(),
        )
    }

    pub(crate) fn data_requirements(
        &mut self,
        arguments: &[TypeArgument],
    ) -> Option<Vec<CheckedAbilityUse>> {
        if arguments.is_empty() {
            return Some(Vec::new());
        }
        let owner = self
            .signatures
            .types
            .get(arguments[0].parameter)
            .owner
            .as_source();
        let needed = owner
            .and_then(|s| self.signatures.nominal_signature_by_symbol(s))
            .map_or(0u32, |(_, n)| n.key_parameters);
        if needed == 0 {
            return Some(Vec::new());
        }
        let mut length = 0usize;
        for arg in arguments {
            let mask = self.signatures.types.get(arg.parameter).parameters;
            if (needed & mask) != 0 {
                length += 2;
            }
        }
        if !self.charge_arena(length, 24) {
            return None;
        }
        let mut proofs = Vec::with_capacity(length);
        for arg in arguments {
            let mask = self.signatures.types.get(arg.parameter).parameters;
            if (needed & mask) == 0 {
                continue;
            }
            for ability in [SourceAbility::Equal, SourceAbility::Hash] {
                let imp: ProofId = match self.prove_ability(arg.ty, ability) {
                    Ok(p) => p,
                    Err(err) => {
                        let _ = self.problem(err.code, self.span, err.message, err.reason, None);
                        return None;
                    }
                };
                proofs.push(CheckedAbilityUse {
                    operand: None,
                    implementation: imp,
                    parameter: Some(arg.parameter),
                });
            }
        }
        debug_assert_eq!(proofs.len(), length);
        Some(proofs)
    }

    fn same_failure_type(&mut self, a: TypeId, b: TypeId, depth: u32) -> bool {
        if !self.work() || !self.type_depth(depth) {
            return false;
        }
        if a == b {
            return true;
        }
        let same_head = {
            let ta = self.signatures.types.get(a);
            let tb = self.signatures.types.get(b);
            ta.kind == tb.kind
                && ta.module == tb.module
                && ta.owner == tb.owner
                && ta.arguments.len() == tb.arguments.len()
                && ta.name == tb.name
        };
        if !same_head {
            return false;
        }
        for i in 0..self.equal_types.len() {
            if !self.work() {
                return false;
            }
            if self.equal_types[i] == (a, b) {
                return true;
            }
        }
        let arity = self.signatures.types.get(a).arguments.len();
        for i in 0..arity {
            let aa = self.signatures.types.get(a).arguments[i];
            let ba = self.signatures.types.get(b).arguments[i];
            if !self.same_failure_type(aa, ba, depth + 1) {
                return false;
            }
        }
        if !self.charge_arena(1, 24) {
            return false;
        }
        self.equal_types.push((a, b));
        true
    }

    fn failure_matches(
        &mut self,
        input: TermId,
        ty_id: TypeId,
        guesses: &mut Vec<(TermId, TypeId)>,
        depth: u32,
    ) -> bool {
        if !self.work() || !self.type_depth(depth) {
            return false;
        }
        let input = self.root(input);
        if self.terms[input.index()].variable
            && self.terms[input.index()].numeric != 0
            && self.finish_type(input, 0).is_none()
        {
            return false;
        }
        if let Some(resolved) = self.terms[input.index()].resolved {
            return self.same_failure_type(resolved, ty_id, depth);
        }
        for &(g_term, g_ty) in guesses.iter() {
            if !self.work() {
                return false;
            }
            if g_term == input {
                return self.same_failure_type(g_ty, ty_id, depth);
            }
        }
        if !self.terms[input.index()].variable {
            let same_head = {
                let s = &self.terms[input.index()].shape;
                let t = self.signatures.types.get(ty_id);
                s.kind == t.kind
                    && s.module == t.module
                    && s.owner == t.owner
                    && s.arity == t.arguments.len()
                    && s.name == t.name
            };
            if !same_head {
                return false;
            }
            let arity = self.signatures.types.get(ty_id).arguments.len();
            for i in 0..arity {
                let Some(part) = self.argument(input, i) else {
                    return false;
                };
                let targ = self.signatures.types.get(ty_id).arguments[i];
                if !self.failure_matches(part, targ, guesses, depth + 1) {
                    return false;
                }
            }
        }
        if !self.charge_arena(1, 24) {
            return false;
        }
        guesses.push((input, ty_id));
        true
    }

    pub(crate) fn finish_wrapping(&mut self) -> bool {
        for idx in (0..self.failures.len()).rev() {
            let (span, op, fallback, context_opt, input_ty) = {
                let f = &self.failures[idx];
                (f.span, f.op, f.fallback, f.context, f.input)
            };
            self.span = span;
            if !self.work() {
                return false;
            }
            let Some(context_ty) = context_opt else {
                continue;
            };
            if op != TokenKind::Eof || fallback.is_some() || self.named(input_ty, "Option") {
                continue;
            }
            let Some(from) = self.argument(input_ty, 1) else {
                return false;
            };
            let Some(to) = self.argument(context_ty, 1) else {
                return false;
            };
            let from_root = self.root(from);
            let to_root = self.root(to);
            if ((self.terms[from_root.index()].variable
                && self.terms[from_root.index()].numeric == 0)
                || self.terms[to_root.index()].variable)
                && !self.unify(from, to, 0)
            {
                return false;
            }
            let Some(target) = self.finish_type(to, 0) else {
                return false;
            };
            let mut guesses = Vec::new();
            if self.failure_matches(from, target, &mut guesses, 0) {
                let Some(actual) = self.annotation(target, None, 0) else {
                    return false;
                };
                if !self.unify(from, actual, 0) {
                    return false;
                }
                continue;
            }
            if self.failed {
                return false;
            }

            let mut selected: Option<TypeId> = None;
            let mut count = 0u32;
            let (target_owner, target_name, target_args) = {
                let t = self.signatures.types.get(target);
                (t.owner, t.name.clone(), t.arguments.clone())
            };
            if let Some(owner_sym) = target_owner.as_source()
                && self.interfaces.symbol(owner_sym).decl_kind == SyntaxKind::Enum
            {
                if !self.visible_data(owner_sym) {
                    return false;
                }
                let cases = self.interfaces.symbol(owner_sym).cases.clone();
                for cidx in cases {
                    if !self.work() {
                        return false;
                    }
                    let case_sym = SymRef::new(owner_sym.module, cidx);
                    let Some(did) = self.data_type(case_sym) else {
                        return false;
                    };
                    let (unnamed, field_cnt, data_ty) = {
                        let d = &self.signatures.data[did.index()];
                        (d.unnamed, d.fields.len(), d.ty)
                    };
                    if !unnamed || field_cnt != 1 {
                        continue;
                    }
                    let Some(field) = self.pattern_field_type(did, target, 0) else {
                        return false;
                    };
                    guesses.clear();
                    if self.failure_matches(from, field, &mut guesses, 0) {
                        count += 1;
                        selected = Some(field);
                        self.failures[idx].wrapper = Some(ConstructorRef::Source(case_sym));
                        self.failures[idx].generic = Some(data_ty);
                    }
                    if self.failed {
                        return false;
                    }
                }
            } else if target_owner == TypeOwner::None
                && let Some(first) = builtin_case_by_type(&target_name)
            {
                for i in 0..first.cases {
                    if !self.work() {
                        return false;
                    }
                    let Some(data) = builtin_case_by_key(first.first_key + i) else {
                        continue;
                    };
                    if data.count != 1 || data.fields[0].is_some() || target_args.is_empty() {
                        continue;
                    }
                    let arg_ty = target_args[data.parameter as usize];
                    guesses.clear();
                    if self.failure_matches(from, arg_ty, &mut guesses, 0) {
                        count += 1;
                        selected = Some(arg_ty);
                        self.failures[idx].wrapper = Some(ConstructorRef::Builtin(data.name));
                        self.failures[idx].generic = self.builtin_template(data);
                    }
                    if self.failed {
                        return false;
                    }
                }
            }

            if count != 1 {
                let Some(preview) = self.preview_type_string(from) else {
                    return false;
                };
                let msg = format!(
                    "try needs exactly one failure case holding {:.511} as its unnamed value; \
                     found {count}",
                    preview
                );
                return self.problem(
                    ErrorCode::TryWrapper,
                    self.span,
                    msg,
                    Some(if count == 0 {
                        "Add one case Wrap(E) to the function's failure type, where E is the \
                         source failure type; named fields don't wrap (reference 8.3)."
                    } else {
                        "Keep exactly one case with this unnamed value; make the other matching \
                         cases use named fields, or handle the failure with case (reference 8.3)."
                    }),
                    None,
                );
            }
            let Some(sel_ty) = selected else {
                return false;
            };
            let Some(actual) = self.annotation(sel_ty, None, 0) else {
                return false;
            };
            if !self.unify(from, actual, 0) {
                return false;
            }
        }
        true
    }

    fn pattern_note(
        &self,
        notes_by_syntax: &[(NodeId, NoteId)],
        mut syntax: NodeId,
    ) -> Option<NoteId> {
        while matches!(
            self.node(syntax).kind,
            SyntaxKind::Group | SyntaxKind::Binder
        ) {
            let Some(inner) = self.node(syntax).children.first().copied().flatten() else {
                break;
            };
            syntax = inner;
        }
        let pos = notes_by_syntax
            .binary_search_by_key(&syntax.0, |&(sn, _)| sn.0)
            .ok()?;
        Some(notes_by_syntax[pos].1)
    }

    fn alloc_checked_pattern(
        &mut self,
        out_patterns: &mut Vec<CheckedPattern>,
        syntax: NodeId,
        ty: TypeId,
        kind: CheckedPatternKind,
    ) -> Option<CheckedPatternId> {
        self.span = self.node(syntax).span;
        if !self.work() || !self.charge_arena(1, SIZEOF_CHECKED_PATTERN) {
            return None;
        }
        let id = CheckedPatternId(out_patterns.len() as u32);
        out_patterns.push(CheckedPattern {
            kind,
            syntax,
            ty,
            binding: None,
            constructor: None,
            arguments: Vec::new(),
            abilities: Vec::new(),
            parts: Vec::new(),
        });
        Some(id)
    }

    fn pattern_defaults_for(
        &mut self,
        data_id: DataSigId,
        ty: TypeId,
        out_patterns: &mut Vec<CheckedPattern>,
    ) -> Option<Vec<CheckedPatternId>> {
        for i in 0..self.pattern_defaults.len() {
            if !self.work() {
                return None;
            }
            if self.pattern_defaults[i].0 == data_id && self.pattern_defaults[i].1 == ty {
                return Some(self.pattern_defaults[i].2.clone());
            }
        }
        let (field_count, data_sym) = {
            let d = &self.signatures.data[data_id.index()];
            (d.fields.len(), d.symbol)
        };
        if !self.charge_arena(1, 32) || !self.charge_arena(field_count, SIZEOF_PTR) {
            return None;
        }
        let mut defaults = Vec::with_capacity(field_count);
        for i in 0..field_count {
            let field_syntax = self.signatures.data[data_id.index()]
                .field_by_index(i)
                .name_node
                .unwrap_or_else(|| self.interfaces.symbol(data_sym).declaration);
            let field_ty = self.pattern_field_type(data_id, ty, i)?;
            let pid = self.alloc_checked_pattern(
                out_patterns,
                field_syntax,
                field_ty,
                CheckedPatternKind::Any,
            )?;
            defaults.push(pid);
        }
        self.pattern_defaults.push((data_id, ty, defaults.clone()));
        Some(defaults)
    }

    pub(crate) fn checked_pattern(
        &mut self,
        syntax: NodeId,
        ty: TypeId,
        notes_by_syntax: &[(NodeId, NoteId)],
        out_patterns: &mut Vec<CheckedPattern>,
        depth: u32,
    ) -> Option<CheckedPatternId> {
        if !self.work() || !self.type_depth(depth) {
            return None;
        }
        let kind = self.node(syntax).kind;
        if matches!(kind, SyntaxKind::Group | SyntaxKind::Binder) {
            let inner = self.node(syntax).children.first().copied().flatten()?;
            return self.checked_pattern(inner, ty, notes_by_syntax, out_patterns, depth + 1);
        }
        if kind == SyntaxKind::Underscore {
            return self.alloc_checked_pattern(out_patterns, syntax, ty, CheckedPatternKind::Any);
        }
        if kind == SyntaxKind::Name {
            let note_id = self.pattern_note(notes_by_syntax, syntax)?;
            let bidx = self.notes[note_id.index()]
                .binding
                .and_then(|bn| self.notes[bn.index()].binding_index)?;
            let pid =
                self.alloc_checked_pattern(out_patterns, syntax, ty, CheckedPatternKind::Bind)?;
            out_patterns[pid.index()].binding = Some(CheckedBindingId(bidx));
            return Some(pid);
        }
        if kind == SyntaxKind::Tuple {
            let children = self.node(syntax).children.clone();
            let ty_args = self.signatures.types.get(ty).arguments.clone();
            debug_assert_eq!(ty_args.len(), children.len());
            if !self.charge_arena(children.len(), 16) {
                return None;
            }
            let pid =
                self.alloc_checked_pattern(out_patterns, syntax, ty, CheckedPatternKind::Tuple)?;
            let mut parts = Vec::with_capacity(children.len());
            for (i, ch) in children.iter().enumerate() {
                let cnid = (*ch)?;
                let sub = self.checked_pattern(
                    cnid,
                    ty_args[i],
                    notes_by_syntax,
                    out_patterns,
                    depth + 1,
                )?;
                parts.push(CheckedPatternPart {
                    index: i as u32,
                    pattern: sub,
                });
            }
            out_patterns[pid.index()].parts = parts;
            return Some(pid);
        }
        if kind == SyntaxKind::List {
            let children = self.node(syntax).children.clone();
            let mut length = children.len();
            let rest = length != 0
                && children[length - 1].is_some_and(|n| self.node(n).kind == SyntaxKind::Rest);
            if rest {
                length -= 1;
            }
            let elem_ty = self.signatures.types.get(ty).arguments[0];
            if !self.charge_arena(length, SIZEOF_PTR) {
                return None;
            }
            let mut heads = Vec::with_capacity(length);
            for ch in children.iter().take(length) {
                let cnid = (*ch)?;
                let sub =
                    self.checked_pattern(cnid, elem_ty, notes_by_syntax, out_patterns, depth + 1)?;
                heads.push(sub);
            }
            let mut tail = if rest {
                let rest_nid = children[length]?;
                let rest_inner = self.node(rest_nid).children.first().copied().flatten()?;
                self.checked_pattern(rest_inner, ty, notes_by_syntax, out_patterns, depth + 1)?
            } else {
                self.alloc_checked_pattern(out_patterns, syntax, ty, CheckedPatternKind::List)?
            };
            for i in (0..length).rev() {
                if !self.charge_arena(2, 16) {
                    return None;
                }
                let cons =
                    self.alloc_checked_pattern(out_patterns, syntax, ty, CheckedPatternKind::List)?;
                out_patterns[cons.index()].parts = vec![
                    CheckedPatternPart {
                        index: 0,
                        pattern: heads[i],
                    },
                    CheckedPatternPart {
                        index: 1,
                        pattern: tail,
                    },
                ];
                tail = cons;
            }
            return Some(tail);
        }

        let is_ctor = matches!(
            kind,
            SyntaxKind::CapName | SyntaxKind::Qualified | SyntaxKind::Call | SyntaxKind::Postfix
        );
        if !is_ctor {
            return self.alloc_checked_pattern(
                out_patterns,
                syntax,
                ty,
                CheckedPatternKind::Literal,
            );
        }

        let note_id = self.pattern_note(notes_by_syntax, syntax)?;
        let args = if kind == SyntaxKind::Call {
            self.node(syntax).children.get(1).copied().flatten()
        } else if kind == SyntaxKind::Postfix {
            self.node(syntax)
                .children
                .get(2)
                .copied()
                .flatten()
                .and_then(|c2| self.node(c2).children.first().copied().flatten())
        } else {
            None
        };

        if let Some(bc_name) = self.notes[note_id.index()].builtin_case {
            let data = builtin_case_by_text(bc_name)?;
            if !self.charge_arena(data.count as usize, 16) {
                return None;
            }
            let pid = self.alloc_checked_pattern(
                out_patterns,
                syntax,
                ty,
                CheckedPatternKind::Constructor,
            )?;
            let tmpl = self.builtin_template(data);
            let arguments = match tmpl {
                Some(g) => self.type_arguments(g, ty)?,
                None => Vec::new(),
            };
            let nominal = self.annotation(ty, None, 0)?;
            let mut parts = Vec::with_capacity(data.count as usize);
            for i in 0..data.count {
                let part_term = self.builtin_part(data, nominal, i)?;
                let field_ty = self.finish_type(part_term, 0)?;
                let any_pid = self.alloc_checked_pattern(
                    out_patterns,
                    syntax,
                    field_ty,
                    CheckedPatternKind::Any,
                )?;
                parts.push(CheckedPatternPart {
                    index: i,
                    pattern: any_pid,
                });
            }
            if let Some(args_nid) = args {
                let arg_children = self.node(args_nid).children.clone();
                for ch in arg_children {
                    let arg_nid = ch?;
                    let idx = self.builtin_field(data, arg_nid)? as usize;
                    let val_nid = if data.fields[0].is_none() {
                        arg_nid
                    } else {
                        self.node(arg_nid).children.get(1).copied().flatten()?
                    };
                    let field_ty = out_patterns[parts[idx].pattern.index()].ty;
                    parts[idx].pattern = self.checked_pattern(
                        val_nid,
                        field_ty,
                        notes_by_syntax,
                        out_patterns,
                        depth + 1,
                    )?;
                }
            }
            let pat = &mut out_patterns[pid.index()];
            pat.constructor = Some(ConstructorRef::Builtin(data.name));
            pat.arguments = arguments;
            pat.parts = parts;
            return Some(pid);
        }

        let sym_ref = self.notes[note_id.index()].symbol?;
        let data_id = self.data_type(sym_ref)?;
        let (field_count, data_ty, unnamed) = {
            let d = &self.signatures.data[data_id.index()];
            (d.fields.len(), d.ty, d.unnamed)
        };
        if !self.charge_arena(field_count, 16) {
            return None;
        }
        let pid =
            self.alloc_checked_pattern(out_patterns, syntax, ty, CheckedPatternKind::Constructor)?;
        let arguments = self.type_arguments(data_ty, ty)?;
        let abilities = self.data_requirements(&arguments)?;
        let defaults = self.pattern_defaults_for(data_id, ty, out_patterns)?;
        let mut parts: Vec<CheckedPatternPart> = defaults
            .into_iter()
            .enumerate()
            .map(|(i, p)| CheckedPatternPart {
                index: i as u32,
                pattern: p,
            })
            .collect();
        if let Some(args_nid) = args {
            let arg_children = self.node(args_nid).children.clone();
            for ch in arg_children {
                let arg_nid = ch?;
                let val_nid = if unnamed {
                    arg_nid
                } else {
                    self.node(arg_nid).children.get(1).copied().flatten()?
                };
                let sel_note = self.pattern_note(notes_by_syntax, val_nid)?;
                let idx = self.notes[sel_note.index()].field_index? as usize;
                let field_ty = out_patterns[parts[idx].pattern.index()].ty;
                parts[idx].pattern = self.checked_pattern(
                    val_nid,
                    field_ty,
                    notes_by_syntax,
                    out_patterns,
                    depth + 1,
                )?;
            }
        }
        let pat = &mut out_patterns[pid.index()];
        pat.constructor = Some(ConstructorRef::Source(sym_ref));
        pat.arguments = arguments;
        pat.abilities = abilities;
        pat.parts = parts;
        Some(pid)
    }
}

pub(crate) struct NoteIter<'a> {
    notes: &'a [super::Note],
    cur: Option<NoteId>,
}

impl Iterator for NoteIter<'_> {
    type Item = NoteId;

    fn next(&mut self) -> Option<Self::Item> {
        let id = self.cur?;
        self.cur = self.notes[id.index()].next;
        Some(id)
    }
}
