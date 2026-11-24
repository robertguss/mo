//! Function promise eligibility preflight, vacuity analysis, randomized+boundary testing,
//! example harvesting, and shrinking (`src/tools/promises.{h,c}`).

use std::cell::RefCell;
use std::io::Write;
use std::rc::Rc;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_bytecode::{BytecodeFile, FunctionKind, NO_INDEX};
use mo_check::{
    CheckedBody, CheckedExprId, CheckedExpression, CheckedProgram, CheckedSpec, CheckedSpecKind,
    ModuleId, SourceType, SourceTypeKind, SymRef, TypeArena, TypeId,
};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_BUILDER_LEVELS, MO_CLI_BUILDER_PATH, MO_CLI_PROMISE_SHRINK_CALLS, MO_CLI_TEST_WORK,
};
use mo_heap::Value;
use mo_host::HostContext;
use mo_lower::LoweredPromise;
use mo_syntax::{SyntaxKind, TokenKind};
use mo_vm::{CallObserver, ControlledOutcome, Defect, Image, Process};

use crate::builders::{BuildTrace, BuilderShrinker, Builders};
use crate::deadline::{Deadline, DeadlineState};
use crate::fakes::Fakes;
use crate::inputs::{InputFactory, InputKind, InputLimits, InputResult, InputShrinker, Inputs};
use crate::render::{
    Observation, PromiseResultReport, TestOutcome, TestResult, TestTotals, VacuityKind,
    runtime_span, write_test_result,
};
use crate::runner::{TestConfig, TestSelection};

fn inability(diags: &mut DiagBag, observation: &Observation, message: &str, detail: Option<&str>) {
    let mut diag = Diagnostic::new(ErrorCode::ToolPipeline, observation.span, message);
    if let Some(text) = detail {
        diag = diag.with_primary_text(text);
    }
    let _ = diags.push(diag);
}

#[derive(Clone, Copy)]
struct TypeLookup<'a> {
    base: &'a TypeArena,
    extra: &'a [SourceType],
}

impl<'a> TypeLookup<'a> {
    fn get(self, id: TypeId) -> &'a SourceType {
        let idx = id.index();
        if idx < self.base.len() {
            self.base.get(id)
        } else {
            &self.extra[idx - self.base.len()]
        }
    }
}

struct Eligibility<'a> {
    types: TypeLookup<'a>,
    visited: Vec<TypeId>,
    work: &'a mut usize,
    limited: bool,
    plain: bool,
    level: u32,
    copied: bool,
}

impl<'a> Eligibility<'a> {
    fn work(&mut self) -> bool {
        if *self.work == 0 {
            self.limited = true;
            return false;
        }
        *self.work -= 1;
        true
    }

    fn push_type(&mut self, ty: TypeId) -> bool {
        if !self.work() {
            return false;
        }
        for i in 0..self.visited.len() {
            if !self.work() {
                return false;
            }
            if self.visited[i] == ty {
                return true;
            }
        }
        self.visited.push(ty);
        true
    }

    fn push_fields(&mut self, program: &CheckedProgram, sym: SymRef) -> bool {
        // Match C's work accounting on data lookup, then push all field types.
        if !self.work() {
            return false;
        }
        let Some((_, data)) = program.signatures.data_signature_by_symbol(sym) else {
            return false;
        };
        for field in &data.fields {
            if !self.push_type(field.ty) {
                return false;
            }
        }
        true
    }
}

fn hidden_type(types: TypeLookup<'_>, ty: TypeId, owner: SymRef) -> bool {
    let st = types.get(ty);
    st.kind == SourceTypeKind::Named
        && st.owner.as_source() == Some(owner)
        && st.arguments.is_empty()
}

fn yields(types: TypeLookup<'_>, ty: TypeId, owner: SymRef, depth: u32) -> bool {
    if hidden_type(types, ty, owner) {
        return true;
    }
    if u64::from(depth) == MO_CLI_BUILDER_PATH {
        return false;
    }
    let st = types.get(ty);
    if st.kind == SourceTypeKind::Tuple {
        return st
            .arguments
            .iter()
            .any(|&arg| yields(types, arg, owner, depth + 1));
    }
    st.kind == SourceTypeKind::Named
        && st.module.is_none()
        && !st.arguments.is_empty()
        && (st.name == "Option" || st.name == "Result")
        && yields(types, st.arguments[0], owner, depth + 1)
}

fn walk_supported(walk: &mut Eligibility<'_>, program: &CheckedProgram, module: ModuleId) -> bool {
    let mut p = 0;
    while p < walk.visited.len() {
        let ty = walk.visited[p];
        if !input_supported(walk, program, ty, module) {
            return false;
        }
        p += 1;
    }
    true
}

fn startable(
    outer: &mut Eligibility<'_>,
    program: &CheckedProgram,
    owner: SymRef,
    level: u32,
) -> bool {
    for constant in &program.constants {
        if !outer.work() {
            return false;
        }
        let sym = program.interfaces.symbol(constant.symbol);
        if sym.exported
            && sym.module == owner.module
            && hidden_type(outer.types, constant.ty, owner)
        {
            return true;
        }
    }
    for func in &program.functions {
        if !outer.work() {
            return false;
        }
        let sig = &program.signatures.functions[func.signature.index()];
        let sym = program.interfaces.symbol(func.symbol);
        let Some(sig_ty) = sig.ty else {
            continue;
        };
        let fn_st = outer.types.get(sig_ty);
        if !sym.exported
            || sym.module != owner.module
            || fn_st.parameters != 0
            || fn_st.arguments.is_empty()
            || !yields(
                outer.types,
                fn_st.arguments[fn_st.arguments.len() - 1],
                owner,
                0,
            )
        {
            continue;
        }
        let param_args: Vec<TypeId> = fn_st.arguments[..fn_st.arguments.len() - 1].to_vec();
        let types = outer.types;
        let mut inner = Eligibility {
            types,
            visited: Vec::new(),
            work: outer.work,
            limited: false,
            plain: true,
            level,
            copied: false,
        };
        let mut supported = true;
        for arg in param_args {
            if !inner.push_type(arg) {
                supported = false;
                break;
            }
        }
        supported = supported && walk_supported(&mut inner, program, owner.module);
        let limited = inner.limited;
        outer.limited |= limited;
        if supported {
            return true;
        }
        if limited {
            return false;
        }
    }
    false
}

fn insides_supported(outer: &mut Eligibility<'_>, program: &CheckedProgram, owner: SymRef) -> bool {
    let types = outer.types;
    let mut inner = Eligibility {
        types,
        visited: Vec::new(),
        work: outer.work,
        limited: false,
        plain: false,
        level: 0,
        copied: true,
    };
    let sym = program.interfaces.symbol(owner);
    let choice = sym.decl_kind == SyntaxKind::Enum;
    let mut supported = true;
    if choice {
        for &case_idx in &sym.cases {
            if !inner.push_fields(program, SymRef::new(owner.module, case_idx)) {
                supported = false;
                break;
            }
        }
    } else {
        supported = inner.push_fields(program, owner);
    }
    supported = supported && walk_supported(&mut inner, program, owner.module);
    outer.limited |= inner.limited;
    supported
}

fn input_supported(
    walk: &mut Eligibility<'_>,
    program: &CheckedProgram,
    ty: TypeId,
    module: ModuleId,
) -> bool {
    if !walk.work() {
        return false;
    }
    let st = walk.types.get(ty);
    match st.kind {
        SourceTypeKind::Function => return false,
        SourceTypeKind::Parameter => return true,
        SourceTypeKind::Tuple => {}
        SourceTypeKind::Named => {
            if st.module.is_none() {
                const SCALARS: &[&str] = &[
                    "Unit",
                    "Bool",
                    "Byte",
                    "Int",
                    "Float",
                    "Char",
                    "Time",
                    "Duration",
                    "Decimal",
                    "Range",
                    "Text",
                    "Bytes",
                    "BigInt",
                    // Capabilities, as generated fakes.
                    "Console",
                    "Files",
                    "ReadFiles",
                    "Clock",
                    "Random",
                    "Args",
                    "Env",
                    "Network",
                    "Sqlite",
                    "Command",
                    "SecureRandom",
                ];
                let argc = st.arguments.len();
                let mut supported = (argc == 1
                    && matches!(st.name.as_str(), "List" | "Set" | "Option"))
                    || (argc == 2 && matches!(st.name.as_str(), "Map" | "Result"));
                let scalar_count = if walk.plain || walk.copied {
                    SCALARS.len() - 11
                } else {
                    SCALARS.len()
                };
                if argc == 0 {
                    for &name in &SCALARS[..scalar_count] {
                        if st.name == name {
                            supported = true;
                            break;
                        }
                    }
                }
                if !supported {
                    return false;
                }
            } else {
                let Some(owner_ref) = st.owner.as_source() else {
                    return false;
                };
                let owner = program.interfaces.symbol(owner_ref);
                if !owner.insides && (owner.exported || owner.module != module) && !walk.copied {
                    let argc = st.arguments.len();
                    let level = walk.level;
                    return level != 0
                        && argc == 0
                        && startable(walk, program, owner_ref, level - 1)
                        && insides_supported(walk, program, owner_ref);
                }
                if owner.decl_kind == SyntaxKind::Enum {
                    for &case_idx in &owner.cases {
                        if !walk.push_fields(program, SymRef::new(owner_ref.module, case_idx)) {
                            return false;
                        }
                    }
                } else if !walk.push_fields(program, owner_ref) {
                    return false;
                }
            }
        }
    }
    let args: Vec<TypeId> = walk.types.get(ty).arguments.clone();
    for arg in args {
        if !walk.push_type(arg) {
            return false;
        }
    }
    true
}

/// Verifies every selected concrete function promise has supported input types
/// (`src/tools/promises.c:mo_promises_supported`).
#[must_use]
pub fn promises_supported(
    program: &CheckedProgram,
    selection: &TestSelection,
    lowered_promises: Option<(&[LoweredPromise], &[SourceType])>,
    diags: &mut DiagBag,
) -> bool {
    let mut work = MO_CLI_TEST_WORK as usize;
    let types = TypeLookup {
        base: &program.signatures.types,
        extra: lowered_promises.map_or(&[], |(_, extra)| extra),
    };
    for obs in &selection.items {
        let Some(spec_idx) = obs.spec_index else {
            continue;
        };
        let spec = &program.specs[spec_idx];
        if spec.kind != CheckedSpecKind::Promise {
            continue;
        }
        let Some(subj_id) = spec.subject else {
            continue;
        };
        let subject = program.function(subj_id);
        let subj_sig = &program.signatures.functions[subject.signature.index()];
        let Some(subj_ty_id) = subj_sig.ty else {
            continue;
        };
        let subj_ty = program.signatures.types.get(subj_ty_id);
        let instance = obs
            .instance_index
            .and_then(|idx| lowered_promises.and_then(|(list, _)| list.get(idx)));
        if subj_ty.parameters != 0 && instance.is_none() {
            continue;
        }
        let mut walk = Eligibility {
            types,
            visited: Vec::new(),
            work: &mut work,
            limited: false,
            plain: false,
            level: MO_CLI_BUILDER_LEVELS as u32,
            copied: false,
        };
        let mut supported = true;
        if let Some(inst) = instance {
            for &param_ty in &inst.parameter_ids {
                if !walk.push_type(param_ty) {
                    supported = false;
                    break;
                }
            }
        } else {
            for &binding_id in &subject.parameters {
                let param_ty = subject.body.binding(binding_id).ty;
                if !walk.push_type(param_ty) {
                    supported = false;
                    break;
                }
            }
        }
        let subj_mod = program.interfaces.symbol(subject.symbol).module;
        supported = supported && walk_supported(&mut walk, program, subj_mod);
        if !supported {
            if walk.limited {
                inability(
                    diags,
                    obs,
                    "promise input preflight exceeds its resource budget",
                    Some(&obs.name),
                );
                return false;
            }
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolUnavailable,
                    obs.span,
                    "this build cannot generate inputs for this promise",
                )
                .with_primary_text(obs.name.as_str()),
            );
            return false;
        }
    }
    true
}

/// Static vacuity analysis plan for a function or service promise (`struct mo_promise_vacuity`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct PromiseVacuity {
    pub tautology_line: u32,
    pub required_mask: u64,
}

fn unwrap_checked<'a>(
    program: &CheckedProgram,
    module: ModuleId,
    body: &'a CheckedBody,
    mut id: CheckedExprId,
) -> &'a CheckedExpression {
    let tree = &program.modules.module(module).syntax.tree;
    loop {
        let expr = body.expr(id);
        let kind = tree.node(expr.syntax).kind;
        if (kind == SyntaxKind::Group || kind == SyntaxKind::ExprStmt) && expr.operands.len() == 1 {
            id = expr.operands[0];
        } else {
            return expr;
        }
    }
}

fn type_may_hold_float(program: &CheckedProgram, ty: TypeId, depth: u32) -> bool {
    if depth > 8 {
        return true;
    }
    let st = program.signatures.types.get(ty);
    if matches!(
        st.kind,
        SourceTypeKind::Parameter | SourceTypeKind::Function
    ) {
        return true;
    }
    for &arg in &st.arguments {
        if type_may_hold_float(program, arg, depth + 1) {
            return true;
        }
    }
    if st.kind == SourceTypeKind::Named {
        if st.module.is_none() {
            return st.name == "Float";
        }
        let Some(owner_ref) = st.owner.as_source() else {
            return true;
        };
        let owner = program.interfaces.symbol(owner_ref);
        if owner.decl_kind == SyntaxKind::Enum {
            for &case_idx in &owner.cases {
                let sym = SymRef::new(owner_ref.module, case_idx);
                if let Some((_, data)) = program.signatures.data_signature_by_symbol(sym) {
                    for f in &data.fields {
                        if type_may_hold_float(program, f.ty, depth + 1) {
                            return true;
                        }
                    }
                }
            }
        } else if let Some((_, data)) = program.signatures.data_signature_by_symbol(owner_ref) {
            for f in &data.fields {
                if type_may_hold_float(program, f.ty, depth + 1) {
                    return true;
                }
            }
        }
    }
    false
}

fn constant_bool_expr(
    program: &CheckedProgram,
    module: ModuleId,
    body: &CheckedBody,
    id: CheckedExprId,
    depth: u32,
) -> Option<bool> {
    if depth > 64 {
        return None;
    }
    let tree = &program.modules.module(module).syntax.tree;
    let r = unwrap_checked(program, module, body, id);
    let kind = tree.node(r.syntax).kind;
    if kind == SyntaxKind::True {
        return Some(true);
    }
    if kind == SyntaxKind::False {
        return Some(false);
    }
    if r.operation == TokenKind::Not && r.operands.len() == 1 {
        return constant_bool_expr(program, module, body, r.operands[0], depth + 1).map(|v| !v);
    }
    if r.operation == TokenKind::And && r.operands.len() == 2 {
        let left = constant_bool_expr(program, module, body, r.operands[0], depth + 1);
        if left == Some(false) {
            return Some(false);
        }
        if left == Some(true) {
            return constant_bool_expr(program, module, body, r.operands[1], depth + 1);
        }
        return None;
    }
    if r.operation == TokenKind::Or
        && r.or_kind != mo_check::CheckedOr::Option
        && r.operands.len() == 2
    {
        let left = constant_bool_expr(program, module, body, r.operands[0], depth + 1);
        if left == Some(true) {
            return Some(true);
        }
        let right = constant_bool_expr(program, module, body, r.operands[1], depth + 1);
        if right == Some(true) {
            return Some(true);
        }
        if left == Some(false) && right == Some(false) {
            return Some(false);
        }
        return None;
    }
    if r.operation == TokenKind::Implies && r.operands.len() == 2 {
        let left = constant_bool_expr(program, module, body, r.operands[0], depth + 1);
        if left == Some(false) {
            return Some(true);
        }
        let right = constant_bool_expr(program, module, body, r.operands[1], depth + 1);
        if right == Some(true) {
            return Some(true);
        }
        if left == Some(true) && right == Some(false) {
            return Some(false);
        }
        return None;
    }
    if matches!(r.operation, TokenKind::Eq | TokenKind::Le | TokenKind::Ge) && r.operands.len() == 2
    {
        let a = unwrap_checked(program, module, body, r.operands[0]);
        let b = unwrap_checked(program, module, body, r.operands[1]);
        if a.binding.is_some()
            && a.binding == b.binding
            && a.operands.is_empty()
            && b.operands.is_empty()
            && !type_may_hold_float(program, a.ty, 0)
        {
            return Some(true);
        }
        let a_node = tree.node(a.syntax);
        let b_node = tree.node(b.syntax);
        if matches!(
            a_node.kind,
            SyntaxKind::Int | SyntaxKind::Char | SyntaxKind::Text
        ) && a_node.kind == b_node.kind
            && a.operands.is_empty()
            && b.operands.is_empty()
            && a_node.text == b_node.text
        {
            return Some(true);
        }
    }
    None
}

/// Computes static tautology and `implies` vacuity requirements for a promise spec
/// (`src/tools/promises.c:mo_promise_vacuity_plan`).
#[must_use]
pub fn promise_vacuity_plan(program: &CheckedProgram, spec: &CheckedSpec) -> PromiseVacuity {
    let mut plan = PromiseVacuity::default();
    for (i, cond) in spec.conditions.iter().enumerate() {
        if plan.tautology_line == 0
            && constant_bool_expr(program, spec.module, &spec.body, cond.expression, 0)
                == Some(true)
        {
            plan.tautology_line = (i + 1) as u32;
        }
        let r = unwrap_checked(program, spec.module, &spec.body, cond.expression);
        if i < 62 && r.operation == TokenKind::Implies {
            plan.required_mask |= 1u64 << i;
        }
    }
    plan
}

/// Returns the 1-based condition index of the first `implies` premise never exercised
/// (`src/tools/promises.c:mo_vacuity_missing_line`).
#[must_use]
pub fn vacuity_missing_line(required: u64, seen: u64) -> u32 {
    let missing = required & !seen;
    debug_assert!(missing != 0);
    missing.trailing_zeros() + 1
}

struct Trial<'a> {
    process: Option<Process<'a>>,
    trace: BuildTrace,
    input: Vec<Value>,
    value: Option<Value>,
    defect: Option<Defect>,
    outcome: TestOutcome,
    condition: u32,
    seen_mask: u64,
    has_input: bool,
    returned: bool,
}

impl<'a> Default for Trial<'a> {
    fn default() -> Self {
        Self {
            process: None,
            trace: BuildTrace::default(),
            input: Vec::new(),
            value: None,
            defect: None,
            outcome: TestOutcome::Passed,
            condition: 0,
            seen_mask: 0,
            has_input: false,
            returned: false,
        }
    }
}

fn evaluate(
    trial: &mut Trial<'_>,
    file: &BytecodeFile,
    promise_fn: u32,
    deadline: &Rc<RefCell<Deadline>>,
) -> bool {
    let subject_fn = file.debug[promise_fn as usize].subject;
    let Some(process) = trial.process.as_mut() else {
        return false;
    };
    let subject_args = trial.input.clone();
    let mut keep = || deadline.borrow_mut().running();
    let outcome = process.call_controlled(subject_fn, subject_args, Some(&mut keep), None);
    let outcome = match outcome {
        ControlledOutcome::Returned(val) => {
            let mut promise_args = trial.input.clone();
            promise_args.push(val.clone());
            trial.value = Some(val);
            let mut keep2 = || deadline.borrow_mut().running();
            match process.call_controlled(promise_fn, promise_args, Some(&mut keep2), None) {
                ControlledOutcome::Returned(ans) => {
                    let cond = ans.as_int();
                    trial.returned = true;
                    if cond > 0 {
                        debug_assert!(
                            cond as u64 <= file.debug[promise_fn as usize].lines.len() as u64
                        );
                        trial.condition = cond as u32;
                        trial.seen_mask = 0;
                        trial.outcome = TestOutcome::Failed;
                    } else {
                        trial.condition = 0;
                        trial.seen_mask = cond.wrapping_neg() as u64;
                        trial.outcome = TestOutcome::Passed;
                    }
                    return true;
                }
                other => other,
            }
        }
        other => other,
    };
    match outcome {
        ControlledOutcome::Stopped(defect) => {
            trial.defect = Some(defect);
            trial.outcome = TestOutcome::Stopped;
            true
        }
        ControlledOutcome::Failed(defect) => {
            trial.defect = Some(defect);
            trial.outcome = TestOutcome::Failed;
            true
        }
        ControlledOutcome::Cancelled => false,
        ControlledOutcome::Returned(_) => unreachable!(),
    }
}

fn interrupted(diags: &mut DiagBag, observation: &Observation, deadline: &Deadline) {
    if deadline.state == DeadlineState::Error {
        let err = std::io::Error::from_raw_os_error(deadline.error);
        let msg = err.to_string();
        inability(
            diags,
            observation,
            "promise clock could not be read",
            Some(&msg),
        );
    } else {
        inability(
            diags,
            observation,
            "promise trial interrupted before its outcome was known",
            Some("the promise's monotonic deadline expired"),
        );
    }
}

fn harvest_example_mask(
    image: &Image,
    config: &TestConfig,
    deadline: &Rc<RefCell<Deadline>>,
    subject: u32,
    promise: u32,
    seen_mask: &mut u64,
    required_mask: u64,
) {
    let file = image.file();
    let subject_name = file.string(file.functions[subject as usize].name);
    for fn_idx in 0..file.functions.len() {
        if (*seen_mask & required_mask) == required_mask || !deadline.borrow_mut().running() {
            break;
        }
        if file.functions[fn_idx].kind != FunctionKind::Anchor
            || file.debug[fn_idx].subject != promise
            || file.debug[fn_idx].expectation != mo_bytecode::AnchorExpectation::Allows
        {
            continue;
        }
        let host = HostContext::stdio(Vec::new(), Vec::new());
        let mut proc = Process::new(image, config.limits, host);
        let mut keep = || deadline.borrow_mut().running();
        if let ControlledOutcome::Returned(ans) =
            proc.call_controlled(fn_idx as u32, Vec::new(), Some(&mut keep), None)
        {
            let cond = ans.as_int();
            if cond <= 0 {
                *seen_mask |= cond.wrapping_neg() as u64;
            }
        }
    }
    if (*seen_mask & required_mask) == required_mask || !deadline.borrow_mut().running() {
        return;
    }
    let direct_prefix = format!("{subject_name}.example.");
    for pass in 0..2u8 {
        if (*seen_mask & required_mask) == required_mask || !deadline.borrow_mut().running() {
            break;
        }
        for ex in 0..file.functions.len() {
            if (*seen_mask & required_mask) == required_mask || !deadline.borrow_mut().running() {
                break;
            }
            let fk = file.functions[ex].kind;
            if fk != FunctionKind::Example && fk != FunctionKind::Test {
                continue;
            }
            let ex_name = file.string(file.functions[ex].name);
            let direct = fk == FunctionKind::Example
                && ex_name.len() > direct_prefix.len()
                && ex_name.starts_with(&direct_prefix);
            if direct != (pass == 0) {
                continue;
            }
            let host = HostContext::stdio(Vec::new(), Vec::new());
            let mut proc = Process::new(image, config.limits, host);
            let mut observer = CallObserver::new(subject);
            let mut keep = || deadline.borrow_mut().running();
            let ex_out =
                proc.call_controlled(ex as u32, Vec::new(), Some(&mut keep), Some(&mut observer));
            if matches!(ex_out, ControlledOutcome::Returned(_)) {
                for call in &observer.calls {
                    if (*seen_mask & required_mask) == required_mask
                        || !deadline.borrow_mut().running()
                    {
                        break;
                    }
                    let mut call_args = call.arguments.clone();
                    call_args.push(call.result.clone());
                    let mut keep2 = || deadline.borrow_mut().running();
                    match proc.call_controlled(promise, call_args, Some(&mut keep2), None) {
                        ControlledOutcome::Returned(ans) => {
                            let cond = ans.as_int();
                            if cond <= 0 {
                                *seen_mask |= cond.wrapping_neg() as u64;
                            }
                        }
                        _ => break,
                    }
                }
            }
        }
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C write_result signature for promise reporting"
)]
fn write_result(
    config: &TestConfig,
    program: &CheckedProgram,
    sources: &SourceMap,
    observation: &Observation,
    file: &BytecodeFile,
    promise: u32,
    inputs: Option<&Inputs<'_>>,
    best: &Trial<'_>,
    vacuity: VacuityKind,
    vacuity_line: u32,
    calls: u64,
    deadline: &Deadline,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) -> bool {
    let subject = file.debug[promise as usize].subject;
    let param_names: Vec<String> = observation
        .spec_index
        .map(|spec_idx| {
            let spec = &program.specs[spec_idx];
            spec.parameters
                .iter()
                .map(|&b| spec.body.binding(b).name.clone())
                .collect()
        })
        .unwrap_or_default();

    let mut outcome = if vacuity != VacuityKind::None {
        TestOutcome::Failed
    } else {
        TestOutcome::Passed
    };
    let mut line = Span::NONE;
    if vacuity != VacuityKind::None
        && let Some(l) = file.debug[promise as usize]
            .lines
            .get((vacuity_line - 1) as usize)
    {
        line = runtime_span(file, sources, l.file, l.start, l.end);
    }

    let mut extreme = None;
    let mut has_best = false;
    if best.process.is_some() {
        has_best = true;
        outcome = best.outcome;
        if best.condition != 0 {
            if let Some(l) = file.debug[promise as usize]
                .lines
                .get((best.condition - 1) as usize)
            {
                line = runtime_span(file, sources, l.file, l.start, l.end);
            }
        } else if outcome == TestOutcome::Failed
            && let Some(defect) = best.defect.as_ref()
            && let Some((f, s, e)) = defect.frames.first().and_then(|fr| fr.span)
        {
            line = runtime_span(file, sources, f, s, e);
        }
        if outcome == TestOutcome::Stopped
            && let Some(inp) = inputs
        {
            let param_count = file.functions[subject as usize].parameters.len();
            for p in 0..param_count {
                match inp.extreme(&best.input, p) {
                    Ok(ex) => {
                        extreme = Some(ex);
                        break;
                    }
                    Err(InputResult::Done) => {}
                    Err(_) => {
                        inability(
                            diags,
                            observation,
                            "promise input extreme could not be inspected",
                            None,
                        );
                        return true;
                    }
                }
            }
        }
    }

    match outcome {
        TestOutcome::Passed => totals.passed += 1,
        TestOutcome::Failed => totals.failed += 1,
        TestOutcome::Stopped => totals.stopped += 1,
    }

    let details = PromiseResultReport {
        subject,
        parameter_names: &param_names,
        input: if has_best { Some(&best.input) } else { None },
        value: if has_best { best.value.as_ref() } else { None },
        calls,
        extreme,
    };
    let result = TestResult {
        observation,
        file,
        outcome,
        vacuity,
        defect: if has_best { best.defect.as_ref() } else { None },
        line,
        microseconds: deadline.microseconds(),
        promise: Some(&details),
        recovery: None,
    };
    if write_test_result(output, config.json, sources, &result) {
        return true;
    }
    inability(
        diags,
        observation,
        "promise result could not be rendered or written completely",
        None,
    );
    false
}

/// Runs one function promise observation (`src/tools/promises.c:mo_test_promise`).
#[must_use]
#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_test_promise signature directly"
)]
pub fn run_promise_test(
    config: &TestConfig,
    program: &CheckedProgram,
    image: &Image,
    sources: &SourceMap,
    observation: &Observation,
    function: u32,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) -> bool {
    let file = image.file();
    let subject = file.debug[function as usize].subject;
    debug_assert!(subject != NO_INDEX);
    let ordinary = &file.functions[subject as usize];

    let deadline = Rc::new(RefCell::new(Deadline::none()));
    if !config.gated {
        match Deadline::start(u32::try_from(config.seconds).unwrap_or(u32::MAX)) {
            Ok(d) => {
                *deadline.borrow_mut() = d;
            }
            Err(err) => {
                let d = Deadline::with_error(err);
                interrupted(diags, observation, &d);
                return true;
            }
        }
    }

    let Some(spec_idx) = observation.spec_index else {
        return false;
    };
    let vacuity = promise_vacuity_plan(program, &program.specs[spec_idx]);
    if vacuity.tautology_line != 0 {
        let empty = Trial::default();
        return write_result(
            config,
            program,
            sources,
            observation,
            file,
            function,
            None,
            &empty,
            VacuityKind::Tautology,
            vacuity.tautology_line,
            0,
            &deadline.borrow(),
            output,
            diags,
            totals,
        );
    }

    let parameters: Vec<u32> = ordinary.parameters.iter().map(|p| p.ty).collect();
    let limits = InputLimits::default();
    let fakes = Fakes::new(&file.types);
    let builders = match Builders::new(program, image, config.limits, Rc::clone(&deadline)) {
        Ok(b) => b,
        Err(_) => {
            inability(
                diags,
                observation,
                "promise input generator could not be prepared",
                None,
            );
            return true;
        }
    };

    let mut factories: Vec<Box<dyn InputFactory + '_>> = Vec::new();
    for f in fakes.factories(&file.types) {
        factories.push(Box::new(f));
    }
    for b in builders.factories() {
        factories.push(Box::new(b));
    }

    let mut inputs = {
        let fac_refs: Vec<&dyn InputFactory> = factories.iter().map(|b| b.as_ref()).collect();
        match Inputs::init(
            &file.types,
            &parameters,
            observation.name.as_bytes(),
            limits,
            &fac_refs,
        ) {
            Ok(inp) => inp,
            Err(_) => {
                inability(
                    diags,
                    observation,
                    "promise input generator could not be prepared",
                    None,
                );
                return true;
            }
        }
    };

    let mut best = Trial::default();
    let mut calls = 0u64;
    let mut random_trials = 0u64;
    let mut seen_mask = 0u64;
    let mut known = true;
    let mut exhausted_inputs = false;

    while calls < config.calls && deadline.borrow_mut().running() {
        let host = HostContext::stdio(Vec::new(), Vec::new());
        let mut candidate = Trial {
            process: Some(Process::new(image, config.limits, host)),
            ..Trial::default()
        };
        let mut kind = InputKind::Boundary;
        let generated = if let Some(proc) = candidate.process.as_mut() {
            fakes.reset_pending();
            builders.begin(inputs.ordinal());
            let mut fac_muts: Vec<&mut (dyn InputFactory + '_)> =
                factories.iter_mut().map(Box::as_mut).collect();
            let res = match inputs.next(proc.heap(), &mut fac_muts) {
                Ok((vals, k)) => {
                    candidate.input = vals;
                    kind = k;
                    InputResult::Ok
                }
                Err(err) => err,
            };
            let ok = res == InputResult::Ok;
            let relate = ok && kind == InputKind::Random && {
                let r = random_trials;
                random_trials += 1;
                r % 2 == 1
            };
            if ok {
                candidate.trace = builders.relate(&mut candidate.input, &parameters, relate);
            } else {
                let mut empty = Vec::new();
                let _ = builders.relate(&mut empty, &[], false);
            }
            res
        } else {
            InputResult::NoMemory
        };
        candidate.has_input = generated == InputResult::Ok;
        if generated == InputResult::Done {
            exhausted_inputs = true;
            break;
        }
        if generated != InputResult::Ok {
            if !deadline.borrow_mut().running() {
                break;
            }
            inability(
                diags,
                observation,
                "promise input could not be generated",
                None,
            );
            known = false;
            break;
        }
        calls += 1;
        let resolved = evaluate(&mut candidate, file, function, &deadline);
        if !resolved {
            interrupted(diags, observation, &deadline.borrow());
            known = false;
            break;
        } else if candidate.outcome != TestOutcome::Passed {
            best = candidate;
            fakes.keep();
            break;
        } else {
            seen_mask |= candidate.seen_mask;
        }
    }

    if calls == config.calls {
        exhausted_inputs = true;
    }

    if known && best.process.is_some() && best.condition != 0 && !config.first_failure {
        let mut shrinker = InputShrinker::new(MO_CLI_PROMISE_SHRINK_CALLS as u32);
        let mut builder_shrinker = BuilderShrinker::new(&builders);
        let mut building = true;
        let mut shrinking = 0u32;
        while shrinking < MO_CLI_PROMISE_SHRINK_CALLS as u32 && deadline.borrow_mut().running() {
            let host = HostContext::stdio(Vec::new(), Vec::new());
            let mut candidate = Trial {
                process: Some(Process::new(image, config.limits, host)),
                ..Trial::default()
            };
            let generated = if let Some(proc) = candidate.process.as_mut() {
                fakes.reset_pending();
                let mut fac_muts: Vec<&mut (dyn InputFactory + '_)> =
                    factories.iter_mut().map(Box::as_mut).collect();
                let mut res = InputResult::Ok;
                if building {
                    match builder_shrinker.next(
                        &mut inputs,
                        &best.trace,
                        proc.heap(),
                        &mut fac_muts,
                    ) {
                        Ok((bvals, btrace)) => {
                            candidate.input = bvals;
                            candidate.trace = btrace;
                            res = InputResult::Ok;
                        }
                        Err(InputResult::Done) => {
                            building = false;
                            best.trace = BuildTrace::default();
                        }
                        Err(err) => {
                            res = err;
                        }
                    }
                }
                if !building {
                    match shrinker.next(&inputs, &best.input, proc.heap(), &mut fac_muts) {
                        Ok(svals) => {
                            candidate.input = svals;
                            res = InputResult::Ok;
                        }
                        Err(err) => {
                            res = err;
                        }
                    }
                }
                res
            } else {
                InputResult::NoMemory
            };
            candidate.has_input = generated == InputResult::Ok;
            if building && generated == InputResult::Uninhabited {
                continue;
            }
            if generated != InputResult::Ok {
                if generated != InputResult::Done && deadline.borrow_mut().running() {
                    let msg = if building && !best.trace.complete {
                        "promise shrinking is limited because builder recording is incomplete"
                    } else {
                        "promise shrink input could not be generated"
                    };
                    inability(diags, observation, msg, None);
                }
                break;
            }
            shrinking += 1;
            calls += 1;
            if !evaluate(&mut candidate, file, function, &deadline) {
                break;
            }
            let fails = candidate.outcome != TestOutcome::Passed;
            if building {
                builder_shrinker.feedback(fails);
            } else {
                shrinker.feedback(fails);
            }
            if fails {
                best = candidate;
                fakes.keep();
                if best.condition == 0 {
                    break;
                }
            }
        }
    }

    if deadline.borrow().state == DeadlineState::Error && known {
        interrupted(diags, observation, &deadline.borrow());
        known = best.process.is_some();
    }
    let cut = calls == 0 || (config.door_plan && best.process.is_none() && calls < config.calls);
    if known && cut && deadline.borrow().state == DeadlineState::Expired {
        interrupted(diags, observation, &deadline.borrow());
        known = false;
    }

    let mut vacuity_kind = VacuityKind::None;
    let mut vacuity_line = 0u32;
    if known
        && best.process.is_none()
        && exhausted_inputs
        && (seen_mask & vacuity.required_mask) != vacuity.required_mask
    {
        harvest_example_mask(
            image,
            config,
            &deadline,
            subject,
            function,
            &mut seen_mask,
            vacuity.required_mask,
        );
        if (seen_mask & vacuity.required_mask) != vacuity.required_mask
            && (deadline.borrow_mut().running() || config.gated)
        {
            vacuity_kind = VacuityKind::UnreachedImplies;
            vacuity_line = vacuity_missing_line(vacuity.required_mask, seen_mask);
        }
    }

    if known {
        write_result(
            config,
            program,
            sources,
            observation,
            file,
            function,
            Some(&inputs),
            &best,
            vacuity_kind,
            vacuity_line,
            calls,
            &deadline.borrow(),
            output,
            diags,
            totals,
        )
    } else {
        true
    }
}
