//! `service` and `actor` block checking, handler & timer validation, bare-`State` rewrite
//! fixes (`E0103`), hidden-type checks (`E0106`), and query/migration entry helpers
//! (`src/check/services.c` parity).

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SERVICE_PROMISES, MO_LIMIT_SOURCE_ABILITY_DEPTH,
    MO_LIMIT_SOURCE_TYPE_DEPTH,
};
use mo_syntax::{DURATION_UNITS, NodeId, SyntaxKind};

use crate::abilities::{AbilityArena, AbilityContext};
use crate::captures::check_service_purity;
use crate::checked::{
    CheckedApi, CheckedBody, CheckedConstant, CheckedFunction, CheckedProgram, CheckedService,
    CheckedServiceTimer, CheckedServiceTool, CheckedSpec, PurityRule, QueryEntryStatus, QueryInput,
    ServiceReply,
};
use crate::ids::{CheckedFuncId, FuncSigId, ModuleId, SymIdx, SymRef, TypeId};
use crate::infer::{ConstantContext, ProgramServicesView, check_service_value};
use crate::load::ModuleSet;
use crate::map_keys::{MapKeyBudget, MapKeyCategory, canonicalize_map_literal_key};
use crate::modules::ModuleInterfaces;
use crate::service_fixes::report_service_state_error;
use crate::signatures::{Signatures, resolve_service_type};
use crate::types::SourceTypeKind;

fn is_duration_unit(bytes: &[u8]) -> bool {
    for &plural in &DURATION_UNITS {
        let pb = plural.as_bytes();
        if bytes == pb || (pb.len() > 1 && bytes == &pb[..pb.len() - 1]) {
            return true;
        }
    }
    false
}

struct ServiceCheck<'a> {
    modules: &'a ModuleSet,
    interfaces: &'a ModuleInterfaces,
    signatures: &'a mut Signatures,
    abilities_arena: &'a mut AbilityArena,
    sig_to_func: &'a [CheckedFuncId],
    diags: &'a mut DiagBag,
    arena_remaining: &'a mut usize,
    span: Span,
    equal: Vec<(TypeId, TypeId)>,
    visible: Vec<TypeId>,
    named: Vec<bool>,
    failed: bool,
}

struct ServiceMapBudget<'a, 'b> {
    state: &'a mut ServiceCheck<'b>,
}

impl MapKeyBudget for ServiceMapBudget<'_, '_> {
    fn charge_work(&mut self, amount: usize) -> bool {
        self.state.work(amount)
    }

    fn charge_bytes(&mut self, bytes: usize) -> bool {
        self.state.allocate(bytes, 1)
    }
}

impl<'a> ServiceCheck<'a> {
    fn report(
        &mut self,
        code: ErrorCode,
        message: &'static str,
        reason: Option<&'static str>,
    ) -> bool {
        if !self.failed {
            let mut diag = Diagnostic::new(code, self.span, message).with_primary_text(message);
            if let Some(r) = reason {
                diag = diag.with_reason(r);
            }
            let _ = self.diags.push(diag);
        }
        self.failed = true;
        false
    }

    fn work(&mut self, amount: usize) -> bool {
        if self.failed {
            return false;
        }
        let amt = amount as u64;
        if amt > MO_LIMIT_PROGRAM_CHECK_WORK.saturating_sub(self.signatures.work) {
            return self.report(
                ErrorCode::CheckProgramWork,
                "the program needs too much service-checking work",
                None,
            );
        }
        self.signatures.work += amt;
        true
    }

    fn allocate(&mut self, count: usize, size: usize) -> bool {
        if self.failed {
            return false;
        }
        let bytes = count.saturating_mul(size);
        let aligned = bytes.checked_add(7).map_or(usize::MAX, |n| n & !7usize);
        if aligned > *self.arena_remaining {
            return self.report(
                ErrorCode::CheckNoMemory,
                "the checking arena has no room",
                None,
            );
        }
        *self.arena_remaining -= aligned;
        true
    }

    fn same(&mut self, left: TypeId, right: TypeId, depth: u32) -> bool {
        if !self.work(1) {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_TYPE_DEPTH {
            return self.report(
                ErrorCode::CheckTypeDepth,
                "the type is nested too deeply",
                None,
            );
        }
        if left == right {
            return true;
        }
        let same_head = {
            let l = self.signatures.types.get(left);
            let r = self.signatures.types.get(right);
            l.kind == r.kind
                && l.module == r.module
                && l.owner == r.owner
                && l.arguments.len() == r.arguments.len()
                && l.name == r.name
        };
        if !same_head {
            return false;
        }
        for i in 0..self.equal.len() {
            if !self.work(1) {
                return false;
            }
            if self.equal[i] == (left, right) {
                return true;
            }
        }
        let arity = self.signatures.types.get(left).arguments.len();
        for i in 0..arity {
            let la = self.signatures.types.get(left).arguments[i];
            let ra = self.signatures.types.get(right).arguments[i];
            if !self.same(la, ra, depth + 1) {
                return false;
            }
        }
        if !self.allocate(1, 24) {
            return false;
        }
        self.equal.push((left, right));
        true
    }

    fn native(&self, ty: TypeId, name: &str) -> bool {
        self.signatures.types.get(ty).is_builtin_named(name)
    }

    fn update_pair(&mut self, ty: TypeId, state: TypeId) -> bool {
        let (is_pair, arg0) = {
            let t = self.signatures.types.get(ty);
            (
                t.kind == SourceTypeKind::Tuple && t.arguments.len() == 2,
                t.arguments.first().copied(),
            )
        };
        is_pair && arg0.is_some_and(|a0| self.same(a0, state, 0))
    }

    fn visible(&mut self, service_mod: ModuleId, ty: TypeId, depth: u32) -> bool {
        if !self.work(1) {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_ABILITY_DEPTH {
            return self.report(
                ErrorCode::CheckAbilityDepth,
                "the type is nested too deeply",
                None,
            );
        }
        for i in 0..self.visible.len() {
            if !self.work(1) {
                return false;
            }
            if self.visible[i] == ty {
                return true;
            }
        }
        if !self.allocate(1, 16) {
            return false;
        }
        self.visible.push(ty);
        let (kind, owner, args) = {
            let t = self.signatures.types.get(ty);
            (t.kind, t.owner, t.arguments.clone())
        };
        if kind == SourceTypeKind::Named
            && let Some(owner_sym) = owner.as_source()
        {
            let sym = self.interfaces.symbol(owner_sym);
            if sym.module != service_mod && !sym.insides {
                return self.report(
                    ErrorCode::ServiceHiddenType,
                    "a service tool uses a hidden type",
                    Some(
                        "A service tool cannot accept or return a type whose fields or cases are \
                         hidden from this module. Expose the type with (..) or use a public type \
                         (reference 11.5).",
                    ),
                );
            }
        }
        for arg_ty in args {
            if !self.visible(service_mod, arg_ty, depth + 1) {
                return false;
            }
        }
        if kind != SourceTypeKind::Named {
            return true;
        }
        let Some(owner_sym) = owner.as_source() else {
            return true;
        };
        let sym = self.interfaces.symbol(owner_sym);
        let is_enum = sym.decl_kind == SyntaxKind::Enum;
        let case_refs: Vec<SymRef> = if is_enum {
            sym.cases
                .iter()
                .map(|&c| SymRef::new(sym.module, c))
                .collect()
        } else {
            vec![owner_sym]
        };
        for cref in case_refs {
            if !self.work(1) {
                return false;
            }
            let field_types: Vec<TypeId> = self
                .signatures
                .data_signature_by_symbol(cref)
                .map(|(_, d)| {
                    (0..d.fields.len())
                        .map(|j| d.field_by_index(j).ty)
                        .collect()
                })
                .unwrap_or_default();
            for fty in field_types {
                if !self.visible(service_mod, fty, depth + 1) {
                    return false;
                }
            }
        }
        true
    }

    fn handler(
        &mut self,
        service_mod: ModuleId,
        service_state: TypeId,
        name: NodeId,
        timer: bool,
    ) -> Option<(CheckedFuncId, ServiceReply)> {
        let tree = &self.modules.module(service_mod).syntax.tree;
        self.span = tree.node(name).span;
        let scope = self.interfaces.scope(service_mod);
        let name_bytes = tree.node_text(name)?;
        let mut selected: Option<FuncSigId> = None;
        if let Some(group_idx) = scope.find_symbol_index(name_bytes) {
            let slot = group_idx.index();
            if self.named[slot] {
                let _ = self.report(
                    ErrorCode::ServiceBlock,
                    "a service names a handler twice",
                    Some(
                        "Each function is named at most once in a service block (reference 11.5).",
                    ),
                );
                return None;
            }
            self.named[slot] = true;
            let overload_count = scope.symbol(group_idx).overload_count;
            for i in 0..overload_count {
                if !self.work(1) {
                    return None;
                }
                let cand_ref = SymRef::new(service_mod, SymIdx(group_idx.0 + i));
                let cand_info = self
                    .signatures
                    .function_signature_by_symbol(cand_ref)
                    .and_then(|(id, f)| f.ty.map(|t| (id, t)));
                let Some((cand_id, cand_ty)) = cand_info else {
                    continue;
                };
                let (arity, arg0) = {
                    let t = self.signatures.types.get(cand_ty);
                    (t.arguments.len(), t.arguments.first().copied())
                };
                if arity < 2 || !arg0.is_some_and(|a0| self.same(a0, service_state, 0)) {
                    continue;
                }
                debug_assert!(selected.is_none());
                selected = Some(cand_id);
            }
        }
        if self.failed {
            return None;
        }
        let Some(sig_id) = selected else {
            let _ = self.report(
                ErrorCode::ServiceHandler,
                "a service handler has no matching local function",
                Some(
                    "A handler is a function in this module whose first parameter exactly matches \
                     the service state type (reference 11.5).",
                ),
            );
            return None;
        };
        let fn_ty_id = self.signatures.functions[sig_id.index()].ty?;
        let (params_mask, fn_args) = {
            let t = self.signatures.types.get(fn_ty_id);
            (t.parameters, t.arguments.clone())
        };
        if params_mask != 0 {
            let _ = self.report(
                ErrorCode::ServiceHandler,
                "a service handler is generic",
                Some(
                    "Tools and timers have concrete signatures; call generic helpers through \
                     concrete wrappers (reference 11.5).",
                ),
            );
            return None;
        }
        if fn_args.len() < 3 || !self.native(fn_args[1], "Time") {
            let _ = self.report(
                ErrorCode::ServiceHandler,
                "a service handler has the wrong parameters",
                Some(
                    "A tool starts with (State, Time); a timer takes exactly (State, Time) \
                     (reference 11.5).",
                ),
            );
            return None;
        }
        let result = fn_args[fn_args.len() - 1];
        let bare = self.same(result, service_state, 0);
        if self.failed {
            return None;
        }
        if timer && (fn_args.len() != 3 || !bare) {
            let _ = self.report(
                ErrorCode::ServiceHandler,
                "a timer handler has the wrong signature",
                Some("A timer takes exactly (State, Time) and returns State (reference 11.5)."),
            );
            return None;
        }
        for &arg_ty in &fn_args {
            if !check_service_purity(
                self.modules,
                self.interfaces,
                self.signatures,
                self.abilities_arena,
                arg_ty,
                self.span,
                PurityRule::Service,
                self.diags,
            ) {
                return None;
            }
        }
        let fn_id = self.sig_to_func[sig_id.index()];
        let reply = if self.update_pair(result, service_state) {
            ServiceReply::Update
        } else if self.native(result, "Result")
            && self
                .signatures
                .types
                .get(result)
                .arguments
                .first()
                .copied()
                .is_some_and(|ok_ty| self.update_pair(ok_ty, service_state))
        {
            ServiceReply::ResultUpdate
        } else {
            ServiceReply::ReadOnly
        };
        if self.failed {
            return None;
        }
        Some((fn_id, reply))
    }

    fn token_source(&mut self, module: ModuleId, token: NodeId) -> Option<NodeId> {
        let tree = &self.modules.module(module).syntax.tree;
        self.span = tree.node(token).span;
        let lit = tree.node(token).children.first().copied().flatten()?;
        let value = {
            let mut budget = ServiceMapBudget { state: self };
            canonicalize_map_literal_key(&mut budget, tree, lit)?
        };
        debug_assert_eq!(value.category, MapKeyCategory::Text);
        let mut valid = !value.bytes.is_empty();
        for (i, &c) in value.bytes.iter().enumerate() {
            if !self.work(1) {
                return None;
            }
            valid =
                valid && (c.is_ascii_uppercase() || c == b'_' || (i != 0 && c.is_ascii_digit()));
        }
        if !valid {
            let _ = self.report(
                ErrorCode::ServiceToken,
                "the token source is not an environment variable name",
                Some(
                    "Use capital letters, digits and underscores, without a leading digit \
                     (reference 11.5).",
                ),
            );
            return None;
        }
        Some(lit)
    }

    fn interval(&mut self, module: ModuleId, value: NodeId) -> bool {
        let tree = &self.modules.module(module).syntax.tree;
        let vnode = tree.node(value);
        self.span = vnode.span;
        let mut valid = vnode.kind == SyntaxKind::FieldAccess
            && vnode.children.len() == 2
            && vnode.children[0].is_some_and(|c0| tree.node(c0).kind == SyntaxKind::Int);
        if valid && let (Some(number), Some(unit)) = (vnode.children[0], vnode.children[1]) {
            let unit_bytes = tree.node_text(unit).unwrap_or(&[]);
            valid = is_duration_unit(unit_bytes);
            let num_bytes = tree.node_text(number).unwrap_or(&[]);
            let start = if num_bytes.len() > 2
                && num_bytes[0] == b'0'
                && matches!(num_bytes[1], b'x' | b'b')
            {
                2usize
            } else {
                0usize
            };
            let mut positive = false;
            for &c in &num_bytes[start..] {
                if !self.work(1) {
                    return false;
                }
                positive = positive || (c != b'0' && c != b'_');
            }
            valid = valid && positive;
        }
        valid
            || self.report(
                ErrorCode::ServiceValue,
                "a service timer needs a positive duration literal",
                Some(
                    "Use a duration literal with a whole number of at least 1, such as 1.minute \
                     (reference 11.5).",
                ),
            )
    }

    fn check_service_decl(
        &mut self,
        service: &mut CheckedService,
        state_clause: NodeId,
        tools_clause: NodeId,
        token_clause: Option<NodeId>,
    ) -> bool {
        let module = service.module;
        let tree = &self.modules.module(module).syntax.tree;
        self.span = tree.node(state_clause).span;
        let Some(state_ty) = resolve_service_type(
            self.modules,
            self.interfaces,
            self.signatures,
            module,
            state_clause,
            self.diags,
        ) else {
            return false;
        };
        service.state = state_ty;

        let abil_err = {
            let arena = std::mem::take(self.abilities_arena);
            let mut ctx = AbilityContext::with_arena(arena, self.interfaces, self.signatures, 0);
            let ok = ctx.check_type_abilities(state_ty);
            let (arena, _cache, _work, err) = ctx.into_parts();
            *self.abilities_arena = arena;
            if ok { None } else { err }
        };
        if let Some(err) = abil_err {
            if !self.failed {
                let mut diag = Diagnostic::new(err.code, self.span, err.message.clone())
                    .with_primary_text(err.message);
                if let Some(r) = err.reason {
                    diag = diag.with_reason(r);
                }
                let _ = self.diags.push(diag);
            }
            self.failed = true;
            return false;
        }
        if !check_service_purity(
            self.modules,
            self.interfaces,
            self.signatures,
            self.abilities_arena,
            state_ty,
            self.span,
            PurityRule::Service,
            self.diags,
        ) {
            return false;
        }
        if !service.is_actor {
            let Some(tok_clause) = token_clause else {
                return false;
            };
            let Some(tok_lit) = self.token_source(module, tok_clause) else {
                return false;
            };
            service.token_source = Some(tok_lit);
        }

        let Some(tool_list_nid) = tree.node(tools_clause).children.first().copied().flatten()
        else {
            return false;
        };
        let tool_nodes = tree.node(tool_list_nid).children.clone();
        let mut checked_tools = Vec::with_capacity(tool_nodes.len());
        for ch in tool_nodes {
            let Some(tname) = ch else {
                return false;
            };
            let Some((func_id, reply)) = self.handler(module, state_ty, tname, false) else {
                return false;
            };
            checked_tools.push(CheckedServiceTool {
                name: tname,
                function: func_id,
                reply,
            });
        }
        service.tools = checked_tools;

        let Some(clauses_nid) = tree
            .node(service.syntax)
            .children
            .get(usize::from(service.is_actor))
            .copied()
            .flatten()
        else {
            return false;
        };
        let clauses = tree.node(clauses_nid).children.clone();
        let mut timers = Vec::new();
        for ch in clauses {
            if !self.work(1) {
                return false;
            }
            let Some(clause) = ch else {
                continue;
            };
            if tree.node(clause).kind != SyntaxKind::ServiceEvery {
                continue;
            }
            let Some(dur_nid) = tree.node(clause).children.first().copied().flatten() else {
                return false;
            };
            let Some(fn_nid) = tree.node(clause).children.get(1).copied().flatten() else {
                return false;
            };
            if !self.interval(module, dur_nid) {
                return false;
            }
            let Some((func_id, _)) = self.handler(module, state_ty, fn_nid, true) else {
                return false;
            };
            timers.push(CheckedServiceTimer {
                syntax: clause,
                function: func_id,
                interval: CheckedBody::default(),
            });
        }
        service.timers = timers;
        true
    }
}

/// Validates every `service` and `actor` block's clause structure, state type, token source,
/// tools/calls, and timers (`mo_prepare_services`).
#[must_use]
pub(crate) fn prepare_services(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    sig_to_func: &[CheckedFuncId],
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> Option<Vec<CheckedService>> {
    let mut p = ServiceCheck {
        modules,
        interfaces,
        signatures,
        abilities_arena,
        sig_to_func,
        diags,
        arena_remaining,
        span: modules.modules[0].syntax.span,
        equal: Vec::new(),
        visible: Vec::new(),
        named: Vec::new(),
        failed: false,
    };

    let mut services: Vec<CheckedService> = Vec::new();
    for m in 0..modules.len() {
        let mid = ModuleId(m as u32);
        let tree = &modules.modules[m].syntax.tree;
        let Some(items_nid) = tree
            .node(modules.modules[m].syntax.root)
            .children
            .get(1)
            .copied()
            .flatten()
        else {
            continue;
        };
        let mut syntax: Option<NodeId> = None;
        for &ch in &tree.node(items_nid).children {
            if !p.work(1) {
                return None;
            }
            let Some(item) = ch else {
                continue;
            };
            let k = tree.node(item).kind;
            if k != SyntaxKind::Service && k != SyntaxKind::Actor {
                continue;
            }
            p.span = tree.node(item).span;
            if let Some(prev) = syntax {
                let actor_err = k == SyntaxKind::Actor || tree.node(prev).kind == SyntaxKind::Actor;
                let _ = p.report(
                    if actor_err {
                        ErrorCode::ActorBlock
                    } else {
                        ErrorCode::ServiceBlock
                    },
                    if actor_err {
                        "a module declares more than one actor or service"
                    } else {
                        "a module declares two services"
                    },
                    Some(if actor_err {
                        "A module declares at most one actor or service; put additional actors in \
                         their own modules and import them (reference 11.7)."
                    } else {
                        "A module has at most one service block (reference 11.5)."
                    }),
                );
                return None;
            }
            syntax = Some(item);
        }
        let Some(svc_syntax) = syntax else {
            continue;
        };
        let is_actor = tree.node(svc_syntax).kind == SyntaxKind::Actor;
        let actor_symbol = if is_actor {
            let aname = tree.node(svc_syntax).children.first().copied().flatten()?;
            let abytes = tree.node_text(aname)?;
            let idx = interfaces.scope(mid).find_symbol_index(abytes)?;
            Some(SymRef::new(mid, idx))
        } else {
            None
        };

        let mut svc = CheckedService {
            syntax: svc_syntax,
            module: mid,
            is_actor,
            actor_symbol,
            state: TypeId(0),
            initializer: CheckedBody::default(),
            tools: Vec::new(),
            timers: Vec::new(),
            token_source: None,
            promises: Vec::new(),
        };

        p.span = tree.node(svc_syntax).span;
        let clauses_nid = tree
            .node(svc_syntax)
            .children
            .get(usize::from(is_actor))
            .copied()
            .flatten()?;
        let clauses = tree.node(clauses_nid).children.clone();
        let mut stage = 0u32;
        let mut state_clause: Option<NodeId> = None;
        let mut tools_clause: Option<NodeId> = None;
        let mut token_clause: Option<NodeId> = None;
        let mut tool_count = 0usize;
        let mut promise_count = 0u64;

        if is_actor {
            for ch in clauses {
                let Some(clause) = ch else {
                    continue;
                };
                p.span = tree.node(clause).span;
                if !p.work(1) {
                    return None;
                }
                match (stage, tree.node(clause).kind) {
                    (0, SyntaxKind::ServiceState) => {
                        state_clause = Some(clause);
                        stage = 1;
                    }
                    (1, SyntaxKind::ActorCalls) => {
                        tools_clause = Some(clause);
                        tool_count = tree
                            .node(clause)
                            .children
                            .first()
                            .copied()
                            .flatten()
                            .map_or(0, |c| tree.node(c).children.len());
                        stage = 2;
                    }
                    (2, SyntaxKind::ServiceEvery) => {}
                    (2 | 3, SyntaxKind::Promise) => {
                        stage = 3;
                        promise_count += 1;
                        if promise_count > MO_LIMIT_SERVICE_PROMISES {
                            let _ = p.report(
                                ErrorCode::TooManyServicePromises,
                                "an actor has too many promises",
                                Some("An actor has at most 64 promises."),
                            );
                            return None;
                        }
                    }
                    _ => {
                        let _ = p.report(
                            ErrorCode::ActorBlock,
                            "an actor clause is repeated or out of order",
                            Some(
                                "Write state, calls, zero or more every clauses, then any \
                                 promises, in that order (reference 11.7).",
                            ),
                        );
                        return None;
                    }
                }
            }
            if stage < 2 || tool_count == 0 {
                p.span = tree.node(svc_syntax).span;
                let _ = p.report(
                    ErrorCode::ActorBlock,
                    "an actor is missing a required clause",
                    Some(
                        "An actor needs one state clause and one nonempty calls clause, in that \
                         order (reference 11.7).",
                    ),
                );
                return None;
            }
        } else {
            for ch in clauses {
                let Some(clause) = ch else {
                    continue;
                };
                p.span = tree.node(clause).span;
                if !p.work(1) {
                    return None;
                }
                match (stage, tree.node(clause).kind) {
                    (0, SyntaxKind::ServiceState) => {
                        state_clause = Some(clause);
                        stage = 1;
                    }
                    (1, SyntaxKind::ServiceTools) => {
                        tools_clause = Some(clause);
                        tool_count = tree
                            .node(clause)
                            .children
                            .first()
                            .copied()
                            .flatten()
                            .map_or(0, |c| tree.node(c).children.len());
                        stage = 2;
                    }
                    (2, SyntaxKind::ServiceEvery) => {}
                    (2, SyntaxKind::ServiceToken) => {
                        token_clause = Some(clause);
                        stage = 3;
                    }
                    (3, SyntaxKind::Promise) => {
                        promise_count += 1;
                        if promise_count > MO_LIMIT_SERVICE_PROMISES {
                            let _ = p.report(
                                ErrorCode::TooManyServicePromises,
                                "a service has too many service promises",
                                Some("A service has at most 64 service promises."),
                            );
                            return None;
                        }
                    }
                    _ => {
                        let _ = p.report(
                            ErrorCode::ServiceBlock,
                            "a service clause is repeated or out of order",
                            Some(
                                "Write state, tools, zero or more every clauses, token, then any \
                                 service promises, in that order (reference 11.5).",
                            ),
                        );
                        return None;
                    }
                }
            }
            if stage != 3 || tool_count == 0 {
                let _ = p.report(
                    ErrorCode::ServiceBlock,
                    "a service is missing a required clause",
                    Some(
                        "A service needs one state, one nonempty tools clause and one token \
                         clause, in that order, with timers between tools and token \
                         (reference 11.5).",
                    ),
                );
                return None;
            }
        }

        let sym_count = interfaces.scope(mid).symbols.len();
        p.named = vec![false; sym_count];
        let state_c = state_clause?;
        let tools_c = tools_clause?;
        if !p.check_service_decl(&mut svc, state_c, tools_c, token_clause) {
            return None;
        }
        services.push(svc);
    }
    Some(services)
}

/// Checks service/actor state initializers, rejects bare-`State` tool returns (`E0103` with
/// rewrite fixes), checks timer duration expressions, rejects `?` tool names (`E0105`), and
/// rejects hidden types in tool signatures (`E0106`) (`mo_check_services`).
#[expect(
    clippy::too_many_arguments,
    reason = "service checking mirrors C mo_check_services and threads shared checker state"
)]
#[must_use]
pub(crate) fn check_services(
    sources: Option<&SourceMap>,
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: &mut ConstantContext,
    services: &mut [CheckedService],
    apis: &[CheckedApi],
    function_sigs: &[FuncSigId],
    functions: &[CheckedFunction],
    raw_constants: &[CheckedConstant],
    specs: &[CheckedSpec],
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> bool {
    let duration_ty = signatures.types.intern_builtin("Duration");
    let svc_count = services.len();

    for i in 0..svc_count {
        let (module, syntax, is_actor, state_ty) = (
            services[i].module,
            services[i].syntax,
            services[i].is_actor,
            services[i].state,
        );
        let tree = &modules.module(module).syntax.tree;
        let state_clause = tree
            .node(syntax)
            .children
            .get(usize::from(is_actor))
            .copied()
            .flatten()
            .and_then(|cl| tree.node(cl).children.first().copied().flatten());
        let Some(sc_nid) = state_clause else {
            return false;
        };
        let mut init_body = CheckedBody::default();
        let ok = {
            let view = ProgramServicesView {
                services,
                apis,
                function_sigs,
            };
            check_service_value(
                modules,
                interfaces,
                signatures,
                abilities_arena,
                Some(constants),
                Some(view),
                module,
                sc_nid,
                state_ty,
                diags,
                arena_remaining,
                &mut init_body,
            )
        };
        if !ok {
            return false;
        }
        services[i].initializer = init_body;

        let tool_list: Vec<(NodeId, CheckedFuncId)> = services[i]
            .tools
            .iter()
            .map(|t| (t.name, t.function))
            .collect();
        for (tool_name, tool_func) in tool_list {
            let sig_id = functions[tool_func.index()].signature;
            let Some(sig_ty) = signatures.functions[sig_id.index()].ty else {
                return false;
            };
            let Some(&result_ty) = signatures.types.get(sig_ty).arguments.last() else {
                return false;
            };
            let mut p = ServiceCheck {
                modules,
                interfaces,
                signatures,
                abilities_arena,
                sig_to_func: &[],
                diags,
                arena_remaining,
                span: tree.node(tool_name).span,
                equal: Vec::new(),
                visible: Vec::new(),
                named: Vec::new(),
                failed: false,
            };
            let bare = p.same(result_ty, state_ty, 0);
            let wrapped = p.native(result_ty, "Result")
                && p.signatures
                    .types
                    .get(result_ty)
                    .arguments
                    .first()
                    .copied()
                    .is_some_and(|ok_ty| p.same(ok_ty, state_ty, 0));
            if p.failed {
                return false;
            }
            if bare || wrapped {
                let tool_span = tree.node(tool_name).span;
                if let Some(src) = sources {
                    report_service_state_error(
                        src,
                        modules,
                        interfaces,
                        signatures,
                        functions,
                        raw_constants,
                        specs,
                        services,
                        tool_name,
                        tool_span,
                        tool_func,
                        wrapped,
                        diags,
                    );
                } else {
                    let msg = "a service tool returns only its state";
                    let _ = diags.push(
                        Diagnostic::new(ErrorCode::ServiceStateResult, tool_span, msg)
                            .with_primary_text(msg),
                    );
                }
                return false;
            }
        }

        let timer_syntaxes: Vec<NodeId> = services[i].timers.iter().map(|t| t.syntax).collect();
        for (j, timer_syntax) in timer_syntaxes.into_iter().enumerate() {
            let mut interval_body = CheckedBody::default();
            let ok = {
                let view = ProgramServicesView {
                    services,
                    apis,
                    function_sigs,
                };
                check_service_value(
                    modules,
                    interfaces,
                    signatures,
                    abilities_arena,
                    Some(constants),
                    Some(view),
                    module,
                    timer_syntax,
                    duration_ty,
                    diags,
                    arena_remaining,
                    &mut interval_body,
                )
            };
            if !ok {
                return false;
            }
            services[i].timers[j].interval = interval_body;
        }
    }

    for service in services.iter().take(svc_count) {
        let module = service.module;
        let tree = &modules.module(module).syntax.tree;
        let mut p = ServiceCheck {
            modules,
            interfaces,
            signatures,
            abilities_arena,
            sig_to_func: &[],
            diags,
            arena_remaining,
            span: Span::NONE,
            equal: Vec::new(),
            visible: Vec::new(),
            named: Vec::new(),
            failed: false,
        };
        for tool in &service.tools {
            let name_nid = tool.name;
            p.span = tree.node(name_nid).span;
            if let Some(bytes) = tree.node_text(name_nid)
                && bytes.last() == Some(&b'?')
            {
                return p.report(
                    ErrorCode::ServiceToolName,
                    "a service tool name ends in ?",
                    Some(
                        "Service tool names cannot end in ?. Rename the function, for example \
                         from paid? to is_paid (reference 11.5).",
                    ),
                );
            }
            let sig_id = functions[tool.function.index()].signature;
            let Some(sig_ty) = p.signatures.functions[sig_id.index()].ty else {
                return false;
            };
            let fn_args = p.signatures.types.get(sig_ty).arguments.clone();
            for &arg_ty in fn_args.iter().skip(2).take(fn_args.len().saturating_sub(3)) {
                if !p.visible(module, arg_ty, 0) {
                    return false;
                }
            }
            let mut result = fn_args[fn_args.len() - 1];
            let mut error_ty = None;
            if p.native(result, "Result") {
                let rargs = p.signatures.types.get(result).arguments.clone();
                error_ty = Some(rargs[1]);
                result = rargs[0];
            }
            if tool.reply != ServiceReply::ReadOnly {
                result = p.signatures.types.get(result).arguments[1];
            }
            if !p.visible(module, result, 0) {
                return false;
            }
            if let Some(err) = error_ty
                && !p.visible(module, err, 0)
            {
                return false;
            }
        }
    }

    true
}

fn refuse_query(
    p: &mut ServiceCheck<'_>,
    code: ErrorCode,
    message: &'static str,
    reason: &'static str,
) -> QueryEntryStatus {
    let _ = p.report(code, message, Some(reason));
    QueryEntryStatus::Refused
}

/// Validates a `mo query` entry function against the main module's service (`mo_check_query_entry`).
#[must_use]
pub fn check_query_entry(
    program: &mut CheckedProgram,
    name: &[u8],
    input: QueryInput,
    handler: Option<CheckedFuncId>,
    diags: &mut DiagBag,
) -> QueryEntryStatus {
    debug_assert_eq!(input == QueryInput::Failure, handler.is_some());
    let mut arena_remaining = usize::MAX / 2;
    let mut p = ServiceCheck {
        modules: &program.modules,
        interfaces: &program.interfaces,
        signatures: &mut program.signatures,
        abilities_arena: &mut program.abilities,
        sig_to_func: &[],
        diags,
        arena_remaining: &mut arena_remaining,
        span: program.modules.modules[0].syntax.span,
        equal: Vec::new(),
        visible: Vec::new(),
        named: Vec::new(),
        failed: false,
    };
    if !p.work(name.len()) {
        return QueryEntryStatus::Refused;
    }
    let mut dot = name.len();
    while dot > 0 && name[dot - 1] != b'.' {
        dot -= 1;
    }
    let module = if dot != 0 {
        let mut found = None;
        for i in 0..p.modules.len() {
            if !p.work(dot) {
                return QueryEntryStatus::Refused;
            }
            if p.modules.modules[i].name.as_bytes() == &name[..dot - 1] {
                found = Some(ModuleId(i as u32));
            }
        }
        let Some(mid) = found else {
            return QueryEntryStatus::NoModule;
        };
        mid
    } else {
        ModuleId(0)
    };
    let scope = p.interfaces.scope(module);
    let Some(group_idx) = scope.find_symbol_index(&name[dot..]) else {
        return QueryEntryStatus::NoFunction;
    };
    let group_sym = scope.symbol(group_idx);
    if group_sym.decl_kind != SyntaxKind::Fn {
        return QueryEntryStatus::NoFunction;
    }
    p.span = group_sym.name_span;
    let service = program
        .services
        .iter()
        .find(|s| s.module == ModuleId(0) && !s.is_actor);
    let Some(service) = service else {
        return refuse_query(
            &mut p,
            ErrorCode::QueryState,
            "the program has no service to query",
            "A query reads the state of the main module's service, and this program's main \
             module declares no service.",
        );
    };
    let svc_state = service.state;
    let mut selected: Option<FuncSigId> = None;
    for i in 0..group_sym.overload_count {
        if !p.work(1) {
            return QueryEntryStatus::Refused;
        }
        let cand_ref = SymRef::new(module, SymIdx(group_idx.0 + i));
        let Some((cand_id, cand_sig)) = p.signatures.function_signature_by_symbol(cand_ref) else {
            continue;
        };
        let Some(cand_ty) = cand_sig.ty else {
            continue;
        };
        let (arity, arg0) = {
            let t = p.signatures.types.get(cand_ty);
            (t.arguments.len(), t.arguments.first().copied())
        };
        if arity < 2 || !arg0.is_some_and(|a0| p.same(a0, svc_state, 0)) {
            continue;
        }
        selected = Some(cand_id);
    }
    if p.failed {
        return QueryEntryStatus::Refused;
    }
    if selected.is_none() && group_sym.overload_count == 1 {
        selected = p
            .signatures
            .function_signature_by_symbol(SymRef::new(module, group_idx))
            .map(|(id, _)| id);
    }
    let Some(sig_id) = selected else {
        return refuse_query(
            &mut p,
            ErrorCode::QueryState,
            "no overload of the query's function takes the service state",
            "A query's function takes the state type of the main module's service as its \
             first parameter.",
        );
    };
    let s = &p.signatures.functions[sig_id.index()];
    let fn_sym = s.function;
    let Some(fn_ty_id) = s.ty else {
        return QueryEntryStatus::Refused;
    };
    p.span = p.interfaces.symbol(fn_sym).name_span;
    let (params_mask, fn_args) = {
        let t = p.signatures.types.get(fn_ty_id);
        (t.parameters, t.arguments.clone())
    };
    if params_mask != 0 {
        return refuse_query(
            &mut p,
            ErrorCode::QueryGeneric,
            "a query's function is generic",
            "A query runs a compiled function, and only the instances a program uses of a \
             generic function are compiled. Query a function with concrete types that calls it.",
        );
    }
    for &arg_ty in &fn_args {
        if !check_service_purity(
            p.modules,
            p.interfaces,
            p.signatures,
            p.abilities_arena,
            arg_ty,
            p.span,
            PurityRule::Query,
            p.diags,
        ) {
            return QueryEntryStatus::Refused;
        }
    }
    let takes_state = fn_args.len() >= 2 && p.same(fn_args[0], svc_state, 0);
    if p.failed {
        return QueryEntryStatus::Refused;
    }
    if !takes_state {
        return refuse_query(
            &mut p,
            ErrorCode::QueryState,
            "a query's function does not take the service state first",
            "A query's first parameter is exactly the state type of the main module's service.",
        );
    }
    match input {
        QueryInput::State => {
            if fn_args.len() != 2 {
                return refuse_query(
                    &mut p,
                    ErrorCode::QueryInputs,
                    "a state query's function takes more than the state",
                    "A query over an exported state is given the state alone.",
                );
            }
        }
        QueryInput::Failure => {
            let Some(h_id) = handler else {
                return QueryEntryStatus::Refused;
            };
            let expected_sig = program.functions[h_id.index()].signature;
            let Some(expected_ty) = p.signatures.functions[expected_sig.index()].ty else {
                return QueryEntryStatus::Refused;
            };
            let expected_args = p.signatures.types.get(expected_ty).arguments.clone();
            let mut matched = fn_args.len() == expected_args.len();
            for i in 1..fn_args.len().saturating_sub(1) {
                if !matched {
                    break;
                }
                matched = p.same(fn_args[i], expected_args[i], 0);
            }
            if p.failed {
                return QueryEntryStatus::Refused;
            }
            if !matched {
                return refuse_query(
                    &mut p,
                    ErrorCode::QueryInputs,
                    "a failure query's function does not take the failure's inputs",
                    "A query over a failure takes the state, then Time, then the handler's \
                     arguments, with the same types in the same order.",
                );
            }
        }
    }
    let mut found_fn = None;
    for (i, func) in program.functions.iter().enumerate() {
        if !p.work(1) {
            return QueryEntryStatus::Refused;
        }
        if func.signature == sig_id {
            found_fn = Some(CheckedFuncId(i as u32));
        }
    }
    match found_fn {
        Some(id) => QueryEntryStatus::Accepted(id),
        None => QueryEntryStatus::Refused,
    }
}

/// Validates a `--migrate` upgrade function against `old_state` and the main service's new state
/// (`mo_check_migration_entry`).
#[must_use]
pub fn check_migration_entry(
    program: &mut CheckedProgram,
    name: &[u8],
    old_state: TypeId,
    diags: &mut DiagBag,
) -> QueryEntryStatus {
    let mut arena_remaining = usize::MAX / 2;
    let mut p = ServiceCheck {
        modules: &program.modules,
        interfaces: &program.interfaces,
        signatures: &mut program.signatures,
        abilities_arena: &mut program.abilities,
        sig_to_func: &[],
        diags,
        arena_remaining: &mut arena_remaining,
        span: program.modules.modules[0].syntax.span,
        equal: Vec::new(),
        visible: Vec::new(),
        named: Vec::new(),
        failed: false,
    };
    if !p.work(name.len()) {
        return QueryEntryStatus::Refused;
    }
    let mut dot = name.len();
    while dot > 0 && name[dot - 1] != b'.' {
        dot -= 1;
    }
    let module = if dot != 0 {
        let mut found = None;
        for i in 0..p.modules.len() {
            if !p.work(dot) {
                return QueryEntryStatus::Refused;
            }
            if p.modules.modules[i].name.as_bytes() == &name[..dot - 1] {
                found = Some(ModuleId(i as u32));
            }
        }
        let Some(mid) = found else {
            return QueryEntryStatus::NoModule;
        };
        mid
    } else {
        ModuleId(0)
    };
    let scope = p.interfaces.scope(module);
    let Some(group_idx) = scope.find_symbol_index(&name[dot..]) else {
        return QueryEntryStatus::NoFunction;
    };
    let group_sym = scope.symbol(group_idx);
    if group_sym.decl_kind != SyntaxKind::Fn {
        return QueryEntryStatus::NoFunction;
    }
    p.span = group_sym.name_span;
    let service = program
        .services
        .iter()
        .find(|s| s.module == ModuleId(0) && !s.is_actor);
    let Some(service) = service else {
        return refuse_query(
            &mut p,
            ErrorCode::UpgradeMigrationSignature,
            "the program has no service to migrate",
            "A migration reads the state of the main module's service, and this program's \
             main module declares no service.",
        );
    };
    let svc_state = service.state;
    let mut selected: Option<FuncSigId> = None;
    for i in 0..group_sym.overload_count {
        if !p.work(1) {
            return QueryEntryStatus::Refused;
        }
        let cand_ref = SymRef::new(module, SymIdx(group_idx.0 + i));
        let Some((cand_id, cand_sig)) = p.signatures.function_signature_by_symbol(cand_ref) else {
            continue;
        };
        let Some(cand_ty) = cand_sig.ty else {
            continue;
        };
        let (arity, arg0) = {
            let t = p.signatures.types.get(cand_ty);
            (t.arguments.len(), t.arguments.first().copied())
        };
        if arity < 2 || !arg0.is_some_and(|a0| p.same(a0, old_state, 0)) {
            continue;
        }
        selected = Some(cand_id);
    }
    if p.failed {
        return QueryEntryStatus::Refused;
    }
    if selected.is_none() && group_sym.overload_count == 1 {
        selected = p
            .signatures
            .function_signature_by_symbol(SymRef::new(module, group_idx))
            .map(|(id, _)| id);
    }
    let Some(sig_id) = selected else {
        return refuse_query(
            &mut p,
            ErrorCode::UpgradeMigrationSignature,
            "no overload of the migration's function takes the service state",
            "A migration's function takes the supplied old state as its first parameter.",
        );
    };
    let s = &p.signatures.functions[sig_id.index()];
    let fn_sym = s.function;
    let Some(fn_ty_id) = s.ty else {
        return QueryEntryStatus::Refused;
    };
    p.span = p.interfaces.symbol(fn_sym).name_span;
    let (params_mask, fn_args) = {
        let t = p.signatures.types.get(fn_ty_id);
        (t.parameters, t.arguments.clone())
    };
    if params_mask != 0 {
        return refuse_query(
            &mut p,
            ErrorCode::UpgradeMigrationSignature,
            "a migration's function is generic",
            "A migration runs a compiled function, and only the instances a program uses of a \
             generic function are compiled. Select a function with concrete types that calls it.",
        );
    }
    for &arg_ty in &fn_args {
        if !check_service_purity(
            p.modules,
            p.interfaces,
            p.signatures,
            p.abilities_arena,
            arg_ty,
            p.span,
            PurityRule::Migration,
            p.diags,
        ) {
            return QueryEntryStatus::Refused;
        }
    }
    let takes_old = fn_args.len() >= 2 && p.same(fn_args[0], old_state, 0);
    if p.failed {
        return QueryEntryStatus::Refused;
    }
    if !takes_old {
        return refuse_query(
            &mut p,
            ErrorCode::UpgradeMigrationSignature,
            "a migration's function does not take the service state first",
            "A migration's first parameter is exactly the supplied old state type.",
        );
    }
    if fn_args.len() != 2 || !p.same(fn_args[1], svc_state, 0) {
        return refuse_query(
            &mut p,
            ErrorCode::UpgradeMigrationSignature,
            "a migration must take one old state and return the new state",
            "The result must be exactly the new main service's state type.",
        );
    }
    let mut found_fn = None;
    for (i, func) in program.functions.iter().enumerate() {
        if !p.work(1) {
            return QueryEntryStatus::Refused;
        }
        if func.signature == sig_id {
            found_fn = Some(CheckedFuncId(i as u32));
        }
    }
    match found_fn {
        Some(id) => QueryEntryStatus::Accepted(id),
        None => QueryEntryStatus::Refused,
    }
}
