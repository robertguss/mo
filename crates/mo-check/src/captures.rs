//! Generic capability-capture checking (`E0090`) and service/query/migration purity checks
//! (`E0102`, `E1411`, `E1420`) (`src/check/captures.c` parity).

use std::collections::HashSet;

use mo_base::{DiagBag, Diagnostic, Label, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_ABILITY_DEPTH};
use mo_syntax::SyntaxKind;

use crate::abilities::{AbilityArena, SourceAbility, TypeArgument};
use crate::checked::{
    CheckedBody, CheckedConstant, CheckedFunction, CheckedService, CheckedSpec, CheckedSpecKind,
    PurityRule,
};
use crate::ids::{CheckedFuncId, ModuleId, ProofId, SymRef, TypeId};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::signatures::Signatures;
use crate::types::{SourceTypeKind, is_capability_name};

struct CapturesCtx<'a> {
    modules: &'a ModuleSet,
    interfaces: &'a ModuleInterfaces,
    signatures: &'a mut Signatures,
    abilities: &'a AbilityArena,
    functions: &'a [CheckedFunction],
    constants: &'a [CheckedConstant],
    specs: &'a [CheckedSpec],
    services: &'a [CheckedService],
    diags: &'a mut DiagBag,
    span: Span,
    free: Vec<u32>,
    seen: HashSet<ProofId>,
    reporting: bool,
    changed: bool,
    failed: bool,
    service: bool,
}

impl<'a> CapturesCtx<'a> {
    fn report_at(
        &mut self,
        code: ErrorCode,
        message: impl Into<String>,
        reason: Option<&'static str>,
        secondary: Option<Label>,
    ) -> bool {
        let msg = message.into();
        let mut diag = Diagnostic::new(code, self.span, msg.clone()).with_primary_text(msg);
        if let Some(r) = reason {
            diag = diag.with_reason(r);
        }
        if let Some(sec) = secondary {
            diag = diag.with_secondary(sec);
        }
        let _ = self.diags.push(diag);
        self.failed = true;
        false
    }

    fn report(
        &mut self,
        code: ErrorCode,
        message: impl Into<String>,
        reason: Option<&'static str>,
    ) -> bool {
        self.report_at(code, message, reason, None)
    }

    fn work(&mut self) -> bool {
        if self.failed {
            return false;
        }
        if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            return self.report(
                ErrorCode::CheckProgramWork,
                "the program needs too much capability-checking work",
                None,
            );
        }
        self.signatures.work += 1;
        true
    }

    fn stored(
        &mut self,
        ty_id: TypeId,
        parameters: &mut u32,
        capability: &mut bool,
        depth: u32,
    ) -> bool {
        if !self.work() {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_ABILITY_DEPTH {
            return self.report(
                ErrorCode::CheckAbilityDepth,
                "the type is nested too deeply",
                None,
            );
        }
        let ty = self.signatures.types.get(ty_id);
        match ty.kind {
            SourceTypeKind::Function => {
                *capability = *capability || self.service;
                return true;
            }
            SourceTypeKind::Parameter => {
                if let Some(letter) = ty.param_letter() {
                    *parameters |= 1u32 << letter;
                }
                return true;
            }
            SourceTypeKind::Named => {
                if let Some(owner_sym) = ty.owner.as_source() {
                    let Some((_, owner)) = self.signatures.nominal_signature_by_symbol(owner_sym)
                    else {
                        return true;
                    };
                    *capability = *capability
                        || owner.holds_capability
                        || (self.service && owner.holds_function);
                    let capture_params = owner.capture_parameters;
                    let owner_ty = owner.ty;
                    let arg_count = ty.arguments.len();
                    for i in 0..arg_count {
                        let owner_param_mask = self
                            .signatures
                            .types
                            .get(self.signatures.types.get(owner_ty).arguments[i])
                            .parameters;
                        let arg_ty = self.signatures.types.get(ty_id).arguments[i];
                        if (capture_params & owner_param_mask) != 0
                            && !self.stored(arg_ty, parameters, capability, depth + 1)
                        {
                            return false;
                        }
                    }
                    return true;
                }
                if ty.module.is_none() && is_capability_name(&ty.name) {
                    *capability = true;
                }
            }
            SourceTypeKind::Tuple => {}
        }
        let arg_count = self.signatures.types.get(ty_id).arguments.len();
        for i in 0..arg_count {
            let arg_ty = self.signatures.types.get(ty_id).arguments[i];
            if !self.stored(arg_ty, parameters, capability, depth + 1) {
                return false;
            }
        }
        true
    }

    fn check_use(
        &mut self,
        caller: Option<CheckedFuncId>,
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
        let target_sig = crate::ids::FuncSigId(lo as u32);
        let callee = self
            .functions
            .iter()
            .position(|f| f.signature == target_sig);
        let Some(callee_idx) = callee else {
            return true;
        };
        if self.free[callee_idx] == 0 {
            return true;
        }
        for arg in arguments {
            let Some(letter) = self.signatures.types.get(arg.parameter).param_letter() else {
                continue;
            };
            if (self.free[callee_idx] & (1u32 << letter)) == 0 {
                continue;
            }
            let mut parameters = 0u32;
            let mut capability = false;
            if !self.stored(arg.ty, &mut parameters, &mut capability, 0) {
                return false;
            }
            if capability {
                if self.reporting {
                    let sym = self.interfaces.symbol(sym_ref);
                    let truncated = truncate_utf8(&sym.name, 128);
                    let message =
                        format!("{truncated} makes an anonymous function capture a capability");
                    let label = Label::with_text(sym.declaration_span, "captures its parameter");
                    return self.report_at(
                        ErrorCode::CapabilityCapture,
                        message,
                        Some(
                            "This generic function's anonymous functions capture a value of \
                             this type argument, and anonymous functions can't capture \
                             capabilities, or values holding them (reference 11.4).",
                        ),
                        Some(label),
                    );
                }
                continue;
            }
            if parameters != 0
                && let Some(caller_id) = caller
            {
                let own_reqs = self.signatures.functions
                    [self.functions[caller_id.index()].signature.index()]
                .requirements;
                for (j, &req) in own_reqs.iter().enumerate() {
                    if (parameters & (1u32 << j)) != 0
                        && (req & SourceAbility::Number.bit()) == 0
                        && (self.free[caller_id.index()] & (1u32 << j)) == 0
                    {
                        self.free[caller_id.index()] |= 1u32 << j;
                        self.changed = true;
                    }
                }
            }
        }
        true
    }

    fn check_proof(
        &mut self,
        caller: Option<CheckedFuncId>,
        proof_id: ProofId,
        depth: u32,
    ) -> bool {
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
        if !self.check_use(caller, proof.function, &proof.arguments)
            || !self.check_use(caller, proof.encode_function, &proof.encode_arguments)
            || !self.check_use(caller, proof.decode_function, &proof.decode_arguments)
        {
            return false;
        }
        for &part in &proof.parts {
            if !self.check_proof(caller, part, depth + 1) {
                return false;
            }
        }
        for &part in &proof.encode_parts {
            if !self.check_proof(caller, part, depth + 1) {
                return false;
            }
        }
        for &part in &proof.decode_parts {
            if !self.check_proof(caller, part, depth + 1) {
                return false;
            }
        }
        true
    }

    fn check_body(
        &mut self,
        module: ModuleId,
        caller: Option<CheckedFuncId>,
        body: &CheckedBody,
    ) -> bool {
        self.seen.clear();
        let tree = &self.modules.module(module).syntax.tree;
        for row in &body.items {
            self.span = tree.node(row.syntax).span;
            if !self.work() || !self.check_use(caller, row.symbol, &row.arguments) {
                return false;
            }
            for ab in &row.abilities {
                if !self.check_proof(caller, ab.implementation, 0) {
                    return false;
                }
            }
        }
        true
    }

    fn sweep(&mut self) -> bool {
        for i in 0..self.functions.len() {
            let func_id = CheckedFuncId(i as u32);
            let module = self.interfaces.symbol(self.functions[i].symbol).module;
            if !self.check_body(module, Some(func_id), &self.functions[i].body) {
                return false;
            }
        }
        for i in 0..self.specs.len() {
            let spec = &self.specs[i];
            if spec.subject.is_none() && self.functions.is_empty() {
                continue;
            }
            let caller = spec.subject.or(Some(CheckedFuncId(0)));
            if !self.check_body(spec.module, caller, &spec.body) {
                return false;
            }
            if spec.kind == CheckedSpecKind::Anchor
                && let Some(subj_id) = spec.subject
            {
                let tree = &self.modules.module(spec.module).syntax.tree;
                self.span = tree.node(spec.syntax).span;
                let sym = self.functions[subj_id.index()].symbol;
                if !self.check_use(caller, Some(sym), &spec.arguments) {
                    return false;
                }
            }
        }
        for i in 0..self.constants.len() {
            let module = self.interfaces.symbol(self.constants[i].symbol).module;
            let caller = if self.functions.is_empty() {
                None
            } else {
                Some(CheckedFuncId(0))
            };
            if !self.check_body(module, caller, &self.constants[i].initializer) {
                return false;
            }
        }
        for i in 0..self.services.len() {
            let service = &self.services[i];
            let caller = if self.functions.is_empty() {
                None
            } else {
                Some(CheckedFuncId(0))
            };
            if !self.check_body(service.module, caller, &service.initializer) {
                return false;
            }
            for timer in &service.timers {
                if !self.check_body(service.module, caller, &timer.interval) {
                    return false;
                }
            }
            for promise in &service.promises {
                if !self.check_body(service.module, caller, &promise.spec.body) {
                    return false;
                }
                for anchor in &promise.anchors {
                    if !self.check_body(service.module, caller, &anchor.body) {
                        return false;
                    }
                }
            }
        }
        true
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

/// Rejects generic function instantiations whose anonymous functions capture a type parameter
/// instantiated with a capability or capability-holding value (`mo_check_capture_arguments`).
#[must_use]
#[expect(
    clippy::too_many_arguments,
    reason = "capture analysis mirrors C mo_check_captures and threads shared checker state"
)]
pub fn check_capture_arguments(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities: &AbilityArena,
    functions: &[CheckedFunction],
    constants: &[CheckedConstant],
    specs: &[CheckedSpec],
    services: &[CheckedService],
    diags: &mut DiagBag,
) -> bool {
    if functions.is_empty() {
        return true;
    }
    let mut free = vec![0u32; functions.len()];
    let mut any = 0u32;
    for (i, func) in functions.iter().enumerate() {
        free[i] = func.body.capture_free_parameters;
        any |= free[i];
    }
    for spec in specs {
        if let Some(subj) = spec.subject {
            free[subj.index()] |= spec.body.capture_free_parameters;
            any |= spec.body.capture_free_parameters;
        }
    }
    if any == 0 {
        return true;
    }
    let mut ctx = CapturesCtx {
        modules,
        interfaces,
        signatures,
        abilities,
        functions,
        constants,
        specs,
        services,
        diags,
        span: modules.module(ModuleId(0)).syntax.span,
        free,
        seen: HashSet::new(),
        reporting: false,
        changed: false,
        failed: false,
        service: false,
    };
    loop {
        ctx.changed = false;
        if !ctx.sweep() {
            return false;
        }
        if !ctx.changed {
            break;
        }
    }
    ctx.reporting = true;
    ctx.sweep()
}

/// Verifies that a closed type holds no capability or function value (`mo_check_service_purity`).
#[must_use]
#[expect(
    clippy::too_many_arguments,
    reason = "service purity check mirrors C check_service_purity and threads shared checker state"
)]
pub fn check_service_purity(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities: &AbilityArena,
    ty_id: TypeId,
    span: Span,
    rule: PurityRule,
    diags: &mut DiagBag,
) -> bool {
    let mut ctx = CapturesCtx {
        modules,
        interfaces,
        signatures,
        abilities,
        functions: &[],
        constants: &[],
        specs: &[],
        services: &[],
        diags,
        span,
        free: Vec::new(),
        seen: HashSet::new(),
        reporting: false,
        changed: false,
        failed: false,
        service: true,
    };
    let mut parameters = 0u32;
    let mut capability = false;
    if !ctx.stored(ty_id, &mut parameters, &mut capability, 0) {
        return false;
    }
    debug_assert_eq!(parameters, 0);
    if !capability {
        return true;
    }
    match rule {
        PurityRule::Migration => ctx.report(
            ErrorCode::UpgradeMigrationSignature,
            "a migration value holds a capability or function",
            Some(
                "A migration's old state and new result must be pure, including every nested type.",
            ),
        ),
        PurityRule::Query => ctx.report(
            ErrorCode::QueryCapability,
            "a query value holds a capability or function",
            Some(
                "A query's parameters and result cannot contain capabilities or functions, \
                 even inside records, choices, tuples or collections.",
            ),
        ),
        PurityRule::Service => ctx.report(
            ErrorCode::ServiceCapability,
            "a service value holds a capability or function",
            Some(
                "Service state, tool inputs and tool results cannot contain capabilities or \
                 functions (reference 11.5).",
            ),
        ),
    }
}
