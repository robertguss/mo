//! Module constant inference, cycle detection, and dependency-based topological ordering
//! (`src/check/constants.c` parity).

use std::cmp::Reverse;
use std::collections::BinaryHeap;

use mo_base::{DiagBag, Diagnostic, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_ABILITY_DEPTH};
use mo_syntax::SyntaxKind;

use crate::abilities::{AbilityArena, AbilityImplementationKind, SourceAbility};
use crate::checked::{CheckedBody, CheckedConstant, CheckedFunction};
use crate::ids::{CheckedExprId, ModuleId, ProofId, SymIdx, SymRef};
use crate::infer::{ConstantContext, check_constant};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::signatures::Signatures;
use crate::types::SourceTypeKind;

fn report(
    diags: &mut DiagBag,
    code: ErrorCode,
    span: Span,
    message: &'static str,
    reason: Option<&'static str>,
) -> bool {
    let mut diag = Diagnostic::new(code, span, message).with_primary_text(message);
    if let Some(r) = reason {
        diag = diag.with_reason(r);
    }
    let _ = diags.push(diag);
    false
}

fn work(signatures: &mut Signatures, diags: &mut DiagBag, span: Span) -> bool {
    if signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
        return report(
            diags,
            ErrorCode::CheckProgramWork,
            span,
            "the program needs too much constant-checking work",
            None,
        );
    }
    signatures.work += 1;
    true
}

fn charge_arena(
    arena_remaining: &mut usize,
    diags: &mut DiagBag,
    span: Span,
    count: usize,
    size: usize,
) -> bool {
    let bytes = count.saturating_mul(size);
    let aligned = bytes.checked_add(7).map_or(usize::MAX, |n| n & !7usize);
    if aligned > *arena_remaining {
        return report(
            diags,
            ErrorCode::CheckNoMemory,
            span,
            "the checking arena has no room",
            None,
        );
    }
    *arena_remaining -= aligned;
    true
}

/// Resolves and typechecks every module constant in dependency order, detecting type cycles
/// (`E0089`) (`mo_check_constants`).
#[must_use]
pub(crate) fn check_constants(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> Option<ConstantContext> {
    let mut span = Span::NONE;
    let mut symbols: Vec<SymRef> = Vec::new();
    for i in 0..interfaces.len() {
        let mid = ModuleId(i as u32);
        let scope = interfaces.scope(mid);
        for j in 0..scope.symbols.len() {
            if !work(signatures, diags, span) {
                return None;
            }
            let sym = &scope.symbols[j];
            if sym.decl_kind == SyntaxKind::Const {
                span = sym.declaration_span;
                symbols.push(SymRef::new(mid, SymIdx(j as u32)));
            }
        }
    }
    let count = symbols.len();
    if !charge_arena(arena_remaining, diags, span, count, 64) {
        return None;
    }

    symbols.sort_by(|&a, &b| {
        let sa = interfaces.symbol(a);
        let sb = interfaces.symbol(b);
        sa.module
            .cmp(&sb.module)
            .then_with(|| sa.declaration_span.start.cmp(&sb.declaration_span.start))
    });

    let items: Vec<(SymRef, Option<crate::ids::TypeId>)> =
        symbols.iter().map(|&s| (s, None)).collect();
    let mut by_symbol: Vec<usize> = (0..count).collect();
    by_symbol.sort_by_key(|&i| items[i].0);

    let mut constants = ConstantContext {
        items,
        initializers: vec![CheckedBody::default(); count],
        by_symbol,
        done: vec![false; count],
        pending: None,
    };

    let mut active = vec![false; count];
    let mut stack: Vec<usize> = Vec::with_capacity(count);

    for start in 0..count {
        if constants.done[start] {
            continue;
        }
        stack.clear();
        stack.push(start);
        active[start] = true;
        while let Some(&index) = stack.last() {
            if !work(signatures, diags, span) {
                return None;
            }
            constants.pending = None;
            let mut halt = false;
            let valid = check_constant(
                modules,
                interfaces,
                signatures,
                abilities_arena,
                &mut constants,
                index,
                diags,
                arena_remaining,
                &mut halt,
            );
            if !valid && halt {
                return None;
            }
            if let Some((target, pending_span)) = constants.pending {
                debug_assert!(target < count && !constants.done[target]);
                if active[target] {
                    let _ = report(
                        diags,
                        ErrorCode::ConstantCycle,
                        pending_span,
                        "constants depend on each other in a cycle",
                        Some(
                            "Constants are computed in the order of their dependencies; a cycle \
                             is an error (reference 10.4).",
                        ),
                    );
                    return None;
                }
                debug_assert!(stack.len() < count);
                stack.push(target);
                active[target] = true;
            } else {
                constants.done[index] = true;
                active[index] = false;
                stack.pop();
            }
        }
    }

    Some(constants)
}

#[derive(Debug, Clone, Copy)]
struct Edge {
    target: usize,
    span: Span,
}

#[derive(Debug, Clone)]
struct Vertex {
    edges: Vec<Edge>,
    unknown: bool,
    span: Span,
}

impl Default for Vertex {
    fn default() -> Self {
        Self {
            edges: Vec::new(),
            unknown: false,
            span: Span::NONE,
        }
    }
}

struct Dependencies<'a> {
    modules: &'a ModuleSet,
    interfaces: &'a ModuleInterfaces,
    signatures: &'a mut Signatures,
    abilities: &'a AbilityArena,
    functions: &'a [CheckedFunction],
    constants: &'a ConstantContext,
    diags: &'a mut DiagBag,
    arena_remaining: &'a mut usize,
    vertices: Vec<Vertex>,
    sig_to_func: Vec<usize>,
    seen_proofs: Vec<ProofId>,
}

impl<'a> Dependencies<'a> {
    fn add_edge(&mut self, from: usize, target: usize, span: Span) -> bool {
        if !work(self.signatures, self.diags, span)
            || !charge_arena(self.arena_remaining, self.diags, span, 1, 24)
        {
            return false;
        }
        self.vertices[from].edges.push(Edge { target, span });
        true
    }

    fn function_vertex(&self, symbol: SymRef) -> Option<usize> {
        let (sig_id, _) = self.signatures.function_signature_by_symbol(symbol)?;
        Some(self.constants.items.len() + self.sig_to_func[sig_id.index()])
    }

    fn library_symbol(&self, symbol: Option<SymRef>) -> bool {
        let Some(sym_ref) = symbol else {
            return false;
        };
        let sym = self.interfaces.symbol(sym_ref);
        sym.decl_kind == SyntaxKind::Fn && self.interfaces.scope(sym.module).library
    }

    fn library_body(&self, from: usize) -> bool {
        let count = self.constants.items.len();
        from >= count
            && self
                .interfaces
                .scope(self.functions[from - count].symbol.module)
                .library
    }

    fn callee(
        &mut self,
        initial_body: &CheckedBody,
        initial_module: ModuleId,
        mut at: CheckedExprId,
        allow_local_lambda: bool,
    ) -> Option<(CalleeTarget, Option<CheckedExprId>)> {
        let mut cur_const: Option<usize> = None;
        loop {
            let (body, module) = match cur_const {
                None => (initial_body, initial_module),
                Some(cidx) => (
                    &self.constants.initializers[cidx],
                    self.constants.items[cidx].0.module,
                ),
            };
            let tree = &self.modules.module(module).syntax.tree;
            let row = body.expr(at);
            let span = tree.node(row.syntax).span;
            if !work(self.signatures, self.diags, span) {
                return None;
            }
            if row.operands.is_empty()
                && let Some(sym_ref) = row.symbol
            {
                let decl_kind = self.interfaces.symbol(sym_ref).decl_kind;
                if decl_kind == SyntaxKind::Fn {
                    let v = self.function_vertex(sym_ref)?;
                    return Some((CalleeTarget::Known(v), None));
                }
                if decl_kind == SyntaxKind::Const {
                    let cidx = self.constants.index_by_symbol(sym_ref);
                    cur_const = Some(cidx);
                    at = self.constants.initializers[cidx].root?;
                    continue;
                }
            }
            if row.operands.is_empty() && row.native.is_some() {
                return Some((CalleeTarget::Native, None));
            }
            if allow_local_lambda && cur_const.is_none() && row.lambda.is_some() {
                debug_assert_eq!(row.operands.len(), 1);
                return Some((CalleeTarget::Native, Some(row.operands[0])));
            }
            if tree.node(row.syntax).kind == SyntaxKind::Group && row.operands.len() == 1 {
                at = row.operands[0];
                continue;
            }
            if let Some(binding_id) = row.binding
                && !body.binding(binding_id).variable
            {
                let mut found = None;
                for (i, def) in body.items.iter().enumerate() {
                    if !work(self.signatures, self.diags, span) {
                        return None;
                    }
                    if def.binding == Some(binding_id)
                        && def.operands.len() == 1
                        && !def.assignment
                        && tree.node(def.syntax).kind == SyntaxKind::Bind
                    {
                        found = Some(i);
                        break;
                    }
                }
                if let Some(idx) = found {
                    at = body.items[idx].operands[0];
                    continue;
                }
            }
            return Some((CalleeTarget::Unknown, None));
        }
    }

    fn proof_edges(&mut self, from: usize, proof: ProofId, span: Span, depth: u32) -> bool {
        let trusted = self.library_body(from);
        if !work(self.signatures, self.diags, span) {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_ABILITY_DEPTH {
            return report(
                self.diags,
                ErrorCode::CheckAbilityDepth,
                span,
                "the ability dependency path is too deep",
                None,
            );
        }
        for i in 0..self.seen_proofs.len() {
            if !work(self.signatures, self.diags, span) {
                return false;
            }
            if self.seen_proofs[i] == proof {
                return true;
            }
        }
        if !charge_arena(self.arena_remaining, self.diags, span, 1, 16) {
            return false;
        }
        self.seen_proofs.push(proof);
        let imp = self.abilities.get(proof);
        if imp.kind == AbilityImplementationKind::Function
            && let Some(fn_sym) = imp.function
            && let Some(target) = self.function_vertex(fn_sym)
            && !self.add_edge(from, target, span)
        {
            return false;
        }
        if let Some(enc_fn) = imp.encode_function
            && let Some(target) = self.function_vertex(enc_fn)
            && !self.add_edge(from, target, span)
        {
            return false;
        }
        if let Some(dec_fn) = imp.decode_function
            && let Some(target) = self.function_vertex(dec_fn)
            && !self.add_edge(from, target, span)
        {
            return false;
        }
        if imp.kind == AbilityImplementationKind::Parameter
            && !trusted
            && matches!(
                imp.ability,
                SourceAbility::Text | SourceAbility::Order | SourceAbility::Json
            )
        {
            self.vertices[from].unknown = true;
            self.vertices[from].span = span;
        }
        for &part in &imp.parts {
            if !self.proof_edges(from, part, span, depth + 1) {
                return false;
            }
        }
        for &part in &imp.encode_parts {
            if !self.proof_edges(from, part, span, depth + 1) {
                return false;
            }
        }
        for &part in &imp.decode_parts {
            if !self.proof_edges(from, part, span, depth + 1) {
                return false;
            }
        }
        true
    }

    fn body_edges(&mut self, from: usize, body: &CheckedBody, module: ModuleId) -> bool {
        let Some(root_id) = body.root else {
            return true;
        };
        let tree = &self.modules.module(module).syntax.tree;
        let init_span = tree.node(body.expr(root_id).syntax).span;
        let n = body.items.len();
        if !charge_arena(self.arena_remaining, self.diags, init_span, n, 5) {
            return false;
        }
        let mut seen = vec![false; n];
        let mut stack: Vec<CheckedExprId> = Vec::with_capacity(n);
        stack.push(root_id);
        seen[root_id.index()] = true;
        self.seen_proofs.clear();

        while let Some(expr_id) = stack.pop() {
            let row = body.expr(expr_id);
            let span = tree.node(row.syntax).span;
            if !work(self.signatures, self.diags, span) {
                return false;
            }
            if row.lambda.is_some() {
                continue;
            }
            if let Some(sym_ref) = row.symbol
                && self.interfaces.symbol(sym_ref).decl_kind == SyntaxKind::Const
            {
                let target = self.constants.index_by_symbol(sym_ref);
                if !self.add_edge(from, target, span) {
                    return false;
                }
            }
            let mut syntax_nid = row.syntax;
            if tree.node(syntax_nid).kind == SyntaxKind::Postfix
                && let Some(last) = tree.node(syntax_nid).children.last().copied().flatten()
            {
                syntax_nid = last;
            }
            let syntax_kind = tree.node(syntax_nid).kind;
            let callable = !row.operands.is_empty()
                && self
                    .signatures
                    .types
                    .get(body.expr(row.operands[0]).ty)
                    .kind
                    == SourceTypeKind::Function;
            let is_fn = row
                .symbol
                .is_some_and(|s| self.interfaces.symbol(s).decl_kind == SyntaxKind::Fn);
            let is_const = row
                .symbol
                .is_some_and(|s| self.interfaces.symbol(s).decl_kind == SyntaxKind::Const);
            if row.native.is_none()
                && ((syntax_kind == SyntaxKind::Call
                    && callable
                    && (row.symbol.is_none() || is_fn || is_const))
                    || (is_fn && !row.operands.is_empty() && syntax_kind != SyntaxKind::Group))
            {
                let Some((target, _)) = self.callee(body, module, row.operands[0], false) else {
                    return false;
                };
                match target {
                    CalleeTarget::Unknown => {
                        if !self.library_body(from) {
                            self.vertices[from].unknown = true;
                            self.vertices[from].span = span;
                        }
                    }
                    CalleeTarget::Known(t) => {
                        if !self.add_edge(from, t, span) {
                            return false;
                        }
                    }
                    CalleeTarget::Native => {}
                }
            }
            let library_call = self.library_symbol(row.symbol) && !row.operands.is_empty();
            if library_call {
                let first = if row.receiver.is_none() && callable {
                    1usize
                } else {
                    0usize
                };
                for &op_id in &row.operands[first..] {
                    if self.signatures.types.get(body.expr(op_id).ty).kind
                        != SourceTypeKind::Function
                    {
                        continue;
                    }
                    let Some((target, local_lambda)) = self.callee(body, module, op_id, true)
                    else {
                        return false;
                    };
                    if let Some(lam_body) = local_lambda {
                        if !seen[lam_body.index()] {
                            seen[lam_body.index()] = true;
                            stack.push(lam_body);
                        }
                    } else {
                        match target {
                            CalleeTarget::Unknown => {
                                if !self.library_body(from) {
                                    self.vertices[from].unknown = true;
                                    self.vertices[from].span = span;
                                }
                            }
                            CalleeTarget::Known(t) => {
                                if !self.add_edge(from, t, span) {
                                    return false;
                                }
                            }
                            CalleeTarget::Native => {}
                        }
                    }
                }
            }
            for abil in &row.abilities {
                let imp_ability = self.abilities.get(abil.implementation).ability;
                if (abil.operand.is_some()
                    || (library_call
                        && matches!(
                            imp_ability,
                            SourceAbility::Text | SourceAbility::Order | SourceAbility::Json
                        )))
                    && !self.proof_edges(from, abil.implementation, span, 0)
                {
                    return false;
                }
            }
            for &op_id in &row.operands {
                if seen[op_id.index()] {
                    continue;
                }
                seen[op_id.index()] = true;
                stack.push(op_id);
            }
        }
        true
    }
}

#[derive(Debug, Clone, Copy)]
enum CalleeTarget {
    Known(usize),
    Native,
    Unknown,
}

/// Computes initialization order for all module constants, reporting `E0089` on cycles or
/// `E1103` on unresolved higher-order calls (`mo_order_constants`).
#[expect(
    clippy::too_many_arguments,
    reason = "constant dependency ordering threads shared checker arenas and program state"
)]
#[must_use]
pub(crate) fn order_constants(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities: &AbilityArena,
    functions: &[CheckedFunction],
    constants: &ConstantContext,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> Option<Vec<CheckedConstant>> {
    let count = constants.items.len();
    if count == 0 {
        return Some(Vec::new());
    }
    let init_span = interfaces.symbol(constants.items[0].0).declaration_span;
    let total_vertices = count + functions.len();
    if !charge_arena(arena_remaining, diags, init_span, total_vertices, 48) {
        return None;
    }

    let mut sig_to_func = vec![0usize; signatures.functions.len()];
    for (i, func) in functions.iter().enumerate() {
        sig_to_func[func.signature.index()] = i;
    }

    let mut deps = Dependencies {
        modules,
        interfaces,
        signatures,
        abilities,
        functions,
        constants,
        diags,
        arena_remaining,
        vertices: vec![Vertex::default(); total_vertices],
        sig_to_func,
        seen_proofs: Vec::new(),
    };

    for i in 0..total_vertices {
        let (body, module) = if i < count {
            (&constants.initializers[i], constants.items[i].0.module)
        } else {
            let f = &functions[i - count];
            (&f.body, f.symbol.module)
        };
        if !deps.body_edges(i, body, module) {
            return None;
        }
    }

    let mut reverse: Vec<Vec<usize>> = vec![Vec::new(); count];
    let mut seen = vec![0usize; total_vertices];
    let mut stack: Vec<usize> = Vec::with_capacity(total_vertices);
    let mut pending = vec![0usize; count];

    for i in 0..count {
        let epoch = i + 1;
        stack.clear();
        stack.push(i);
        seen[i] = epoch;
        while let Some(at) = stack.pop() {
            if deps.vertices[at].unknown {
                let _ = report(
                    deps.diags,
                    ErrorCode::CheckUnfinished,
                    deps.vertices[at].span,
                    "this constant's call dependencies are not implemented",
                    None,
                );
                return None;
            }
            for idx in 0..deps.vertices[at].edges.len() {
                let edge = deps.vertices[at].edges[idx];
                if !work(deps.signatures, deps.diags, edge.span) {
                    return None;
                }
                let target = edge.target;
                if target < count && (target == i || seen[target] != epoch) {
                    if !charge_arena(deps.arena_remaining, deps.diags, edge.span, 1, 24) {
                        return None;
                    }
                    reverse[target].push(i);
                    pending[i] += 1;
                }
                if seen[target] == epoch {
                    continue;
                }
                seen[target] = epoch;
                if target >= count {
                    stack.push(target);
                }
            }
        }
    }

    let mut heap: BinaryHeap<Reverse<usize>> = BinaryHeap::new();
    for (i, &deg) in pending.iter().enumerate() {
        if deg == 0 {
            heap.push(Reverse(i));
        }
    }
    let mut order = Vec::with_capacity(count);
    while let Some(Reverse(at)) = heap.pop() {
        order.push(at);
        for &dependent in &reverse[at] {
            debug_assert!(pending[dependent] != 0);
            pending[dependent] -= 1;
            if pending[dependent] == 0 {
                heap.push(Reverse(dependent));
            }
        }
    }
    if order.len() != count {
        let first = pending.iter().position(|&d| d != 0).unwrap_or(0);
        let sym = interfaces.symbol(constants.items[first].0);
        let _ = report(
            deps.diags,
            ErrorCode::ConstantCycle,
            sym.name_span,
            "constants depend on each other in a cycle",
            Some(
                "A constant depends on the constants it uses, directly or through the functions \
                 it calls; a cycle is an error (reference 10.4).",
            ),
        );
        return None;
    }

    Some(crate::infer::bodies::collect_checked_constants(
        constants, &order,
    ))
}
