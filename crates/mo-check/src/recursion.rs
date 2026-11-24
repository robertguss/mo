//! Polymorphic recursion check (`E0084` / `E0059`) via explicit-stack Tarjan SCC over
//! generic function parameters (`src/check/recursion.c::mo_check_recursion` parity).

use std::collections::HashSet;

use mo_base::{DiagBag, Diagnostic, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_ABILITY_DEPTH};
use mo_syntax::SyntaxKind;

use crate::abilities::{AbilityArena, TypeArgument};
use crate::checked::{CheckedFunction, CheckedSpec, CheckedSpecKind};
use crate::ids::{ModuleId, ProofId, SymRef};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::signatures::Signatures;
use crate::types::SourceTypeKind;

#[derive(Debug, Clone)]
struct Edge {
    target: usize,
    caller: SymRef,
    callee: SymRef,
    grows: bool,
    span: Span,
}

struct RecursionCtx<'a> {
    interfaces: &'a ModuleInterfaces,
    signatures: &'a mut Signatures,
    abilities: &'a AbilityArena,
    functions: &'a [CheckedFunction],
    diags: &'a mut DiagBag,
    span: Span,
    sig_to_fn: Vec<usize>,
    offsets: Vec<usize>,
    masks: Vec<u32>,
    edges: Vec<Vec<Edge>>,
    seen: HashSet<ProofId>,
    failed: bool,
}

impl<'a> RecursionCtx<'a> {
    fn report(
        &mut self,
        code: ErrorCode,
        message: impl Into<String>,
        reason: Option<&'static str>,
    ) -> bool {
        let msg = message.into();
        let mut diag = Diagnostic::new(code, self.span, msg.clone()).with_primary_text(msg);
        if let Some(r) = reason {
            diag = diag.with_reason(r);
        }
        let _ = self.diags.push(diag);
        self.failed = true;
        false
    }

    fn work(&mut self) -> bool {
        if self.failed {
            return false;
        }
        if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            return self.report(
                ErrorCode::CheckProgramWork,
                "the program needs too much recursion-checking work",
                None,
            );
        }
        self.signatures.work += 1;
        true
    }

    fn parameter_node(&self, function: usize, letter: usize) -> usize {
        let mask = self.masks[function];
        debug_assert!(letter < 26 && (mask & (1u32 << letter)) != 0);
        let mut node = self.offsets[function];
        for i in 0..letter {
            if (mask & (1u32 << i)) != 0 {
                node += 1;
            }
        }
        node
    }

    fn add_edges(
        &mut self,
        caller: usize,
        symbol: Option<SymRef>,
        arguments: &[TypeArgument],
    ) -> bool {
        let Some(sym_ref) = symbol else {
            return true;
        };
        if self.interfaces.symbol(sym_ref).decl_kind != SyntaxKind::Fn || arguments.is_empty() {
            return true;
        }
        let mut lo = 0usize;
        let mut hi = self.signatures.functions.len();
        while lo < hi {
            if !self.work() {
                return false;
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
        let callee_fn = self.sig_to_fn[lo];
        let caller_sym = self.functions[caller].symbol;
        for arg in arguments {
            let Some(target_letter) = self.signatures.types.get(arg.parameter).param_letter()
            else {
                continue;
            };
            let arg_ty = self.signatures.types.get(arg.ty);
            let arg_params = arg_ty.parameters;
            let grows = arg_ty.kind != SourceTypeKind::Parameter;
            for letter in 0..26 {
                if !self.work() {
                    return false;
                }
                if (arg_params & (1u32 << letter)) == 0 {
                    continue;
                }
                let source = self.parameter_node(caller, letter);
                let target = self.parameter_node(callee_fn, target_letter);
                self.edges[source].push(Edge {
                    target,
                    caller: caller_sym,
                    callee: sym_ref,
                    grows,
                    span: self.span,
                });
            }
        }
        true
    }

    fn walk_proof(&mut self, caller: usize, proof_id: ProofId, depth: u32) -> bool {
        if !self.work() {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_ABILITY_DEPTH {
            return self.report(
                ErrorCode::CheckAbilityDepth,
                "the ability dependency path is too deep",
                None,
            );
        }
        if !self.seen.insert(proof_id) {
            return true;
        }
        let proof = self.abilities.get(proof_id);
        if !self.add_edges(caller, proof.function, &proof.arguments)
            || !self.add_edges(caller, proof.encode_function, &proof.encode_arguments)
            || !self.add_edges(caller, proof.decode_function, &proof.decode_arguments)
        {
            return false;
        }
        for &part in &proof.parts {
            if !self.walk_proof(caller, part, depth + 1) {
                return false;
            }
        }
        for &part in &proof.encode_parts {
            if !self.walk_proof(caller, part, depth + 1) {
                return false;
            }
        }
        for &part in &proof.decode_parts {
            if !self.walk_proof(caller, part, depth + 1) {
                return false;
            }
        }
        true
    }

    fn tarjan_components(&mut self, count: usize) -> Option<Vec<u32>> {
        let mut index = vec![u32::MAX; count];
        let mut low = vec![0u32; count];
        let mut component = vec![u32::MAX; count];
        let mut stack: Vec<usize> = Vec::with_capacity(count);
        let mut frames: Vec<(usize, usize)> = Vec::new();
        let mut next = 0u32;
        let mut groups = 0u32;

        for start in 0..count {
            if index[start] != u32::MAX {
                continue;
            }
            index[start] = next;
            low[start] = next;
            next += 1;
            stack.push(start);
            frames.push((start, 0));

            while let Some((node, edge_idx)) = frames.last_mut() {
                if !self.work() {
                    return None;
                }
                let node = *node;
                if *edge_idx < self.edges[node].len() {
                    let rev_idx = self.edges[node].len() - 1 - *edge_idx;
                    let target = self.edges[node][rev_idx].target;
                    *edge_idx += 1;
                    if index[target] == u32::MAX {
                        index[target] = next;
                        low[target] = next;
                        next += 1;
                        stack.push(target);
                        frames.push((target, 0));
                    } else if component[target] == u32::MAX && index[target] < low[node] {
                        low[node] = index[target];
                    }
                    continue;
                }
                if low[node] == index[node] {
                    while let Some(member) = stack.pop() {
                        component[member] = groups;
                        if member == node {
                            break;
                        }
                    }
                    groups += 1;
                }
                frames.pop();
                if let Some(&(parent, _)) = frames.last()
                    && low[node] < low[parent]
                {
                    low[parent] = low[node];
                }
            }
        }
        Some(component)
    }

    fn check_cycles(&mut self, count: usize) -> bool {
        let Some(component) = self.tarjan_components(count) else {
            return false;
        };
        let mut first: Option<(SymRef, SymRef, Span)> = None;
        for (i, list) in self.edges.iter().enumerate() {
            for edge in list.iter().rev() {
                if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
                    return self.report(
                        ErrorCode::CheckProgramWork,
                        "the program needs too much recursion-checking work",
                        None,
                    );
                }
                self.signatures.work += 1;
                if !edge.grows || component[i] != component[edge.target] {
                    continue;
                }
                let replace = match first {
                    None => true,
                    Some((_, _, fspan)) => {
                        edge.span.file < fspan.file
                            || (edge.span.file == fspan.file && edge.span.start < fspan.start)
                    }
                };
                if replace {
                    first = Some((edge.caller, edge.callee, edge.span));
                }
            }
        }
        let Some((caller_sym, callee_sym, span)) = first else {
            return true;
        };
        self.span = span;
        let caller_name = truncate_utf8(&self.interfaces.symbol(caller_sym).name, 128);
        let callee_name = truncate_utf8(&self.interfaces.symbol(callee_sym).name, 128);
        let message = format!(
            "a generic recursion builds a bigger type: {caller_name} calls {callee_name} in a \
             type-parameter cycle"
        );
        self.report(
            ErrorCode::PolymorphicRecursion,
            message,
            Some(
                "A generic function can't reach itself with a bigger type built from its \
                 parameter (reference 5.5).",
            ),
        )
    }
}

fn truncate_utf8(s: &str, max_bytes: usize) -> &str {
    if s.len() <= max_bytes {
        return s;
    }
    let mut end = max_bytes;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

/// Checks for growing polymorphic recursion cycles across generic function bodies,
/// function promises, and ability proof customizer callbacks (`mo_check_recursion`).
#[must_use]
pub fn check_recursion(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities: &AbilityArena,
    functions: &[CheckedFunction],
    specs: &[CheckedSpec],
    diags: &mut DiagBag,
) -> bool {
    let mut sig_to_fn = vec![0usize; signatures.functions.len()];
    let mut offsets = vec![0usize; functions.len()];
    let mut masks = vec![0u32; functions.len()];
    let mut count = 0usize;

    for (i, func) in functions.iter().enumerate() {
        sig_to_fn[func.signature.index()] = i;
        offsets[i] = count;
        let Some(ty_id) = signatures.functions[func.signature.index()].ty else {
            continue;
        };
        let mask = signatures.types.get(ty_id).parameters;
        masks[i] = mask;
        for j in 0..26 {
            if signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
                let diag = Diagnostic::new(
                    ErrorCode::CheckProgramWork,
                    modules.module(ModuleId(0)).syntax.span,
                    "the program needs too much recursion-checking work",
                )
                .with_primary_text("the program needs too much recursion-checking work");
                let _ = diags.push(diag);
                return false;
            }
            signatures.work += 1;
            if (mask & (1u32 << j)) != 0 {
                count += 1;
            }
        }
    }
    if count == 0 {
        return true;
    }

    let mut ctx = RecursionCtx {
        interfaces,
        signatures,
        abilities,
        functions,
        diags,
        span: modules.module(ModuleId(0)).syntax.span,
        sig_to_fn,
        offsets,
        masks,
        edges: vec![Vec::new(); count],
        seen: HashSet::new(),
        failed: false,
    };

    for i in 0..(functions.len() + specs.len()) {
        let (caller, module, body) = if i < functions.len() {
            let sym = functions[i].symbol;
            (i, interfaces.symbol(sym).module, &functions[i].body)
        } else {
            let spec = &specs[i - functions.len()];
            if spec.kind != CheckedSpecKind::Promise {
                continue;
            }
            let Some(subj) = spec.subject else {
                continue;
            };
            (subj.index(), spec.module, &spec.body)
        };
        if ctx.masks[caller] == 0 {
            continue;
        }
        ctx.seen.clear();
        let tree = &modules.module(module).syntax.tree;
        for row in &body.items {
            ctx.span = tree.node(row.syntax).span;
            if !ctx.work() || !ctx.add_edges(caller, row.symbol, &row.arguments) {
                return false;
            }
            for ab in &row.abilities {
                if !ctx.walk_proof(caller, ab.implementation, 0) {
                    return false;
                }
            }
        }
    }

    ctx.check_cycles(count)
}
