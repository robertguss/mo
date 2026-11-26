//! Test selection, generic promise instantiation, bytecode binding, plain/example/promise/service
//! execution, and gated traced execution (`src/tools/tests.{h,c}`).

use std::io::Write;
use std::time::Instant;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_bytecode::{BytecodeFile, FunctionKind, NO_INDEX, TypeKind, load as load_bytecode};
use mo_check::{CheckedFuncId, CheckedProgram, CheckedSpec, CheckedSpecKind, ModuleId};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_ANCHOR_INSTRUCTIONS, MO_CLI_DIAGNOSTIC_SIZE, MO_CLI_TEST_WORK, MO_DOOR_PLAN_INPUTS,
    MO_DOOR_PLAN_SEQUENCES, MO_LIMIT_CORE_NODES, MO_LIMIT_DEFECT_TEXT_SIZE,
};
use mo_host::HostContext;
use mo_lower::{LoweredPromise, lower_test_services};
use mo_syntax::SyntaxKind;
use mo_vm::{ControlledOutcome, Image, Limits, Outcome, Process, VmTrace, render_defect_raw};

use crate::anchors::{check_anchors, program_has_anchors};
use crate::deadline::Deadline;
use crate::promises::{promises_supported, run_promise_test};
use crate::render::{
    Observation, ObservationKind, TestOutcome, TestResult, TestTotals, VacuityKind, runtime_span,
    write_test_result,
};
use crate::service_tests::run_service_tests;

const RECOVERY_SUFFIX: &str = ".service.recovery";

/// Configuration for `mo test` execution (`struct mo_cli` fields relevant to `mo-test`).
#[derive(Debug, Clone)]
pub struct TestConfig {
    pub json: bool,
    pub only: Vec<Vec<u8>>,
    pub module: Option<Vec<u8>>,
    pub calls: u64,
    pub seconds: u64,
    pub sequences: u64,
    pub jobs: usize,
    pub limits: Limits,
    pub door_plan: bool,
    pub gated: bool,
    pub first_failure: bool,
}

impl Default for TestConfig {
    fn default() -> Self {
        Self {
            json: false,
            only: Vec::new(),
            module: None,
            calls: MO_DOOR_PLAN_INPUTS,
            seconds: 5,
            sequences: MO_DOOR_PLAN_SEQUENCES,
            jobs: 1,
            limits: Limits::default(),
            door_plan: false,
            gated: false,
            first_failure: false,
        }
    }
}

/// Result of [`select_tests`] matching `struct mo_test_selection`.
#[derive(Debug, Clone)]
pub struct TestSelection {
    pub items: Vec<Observation>,
    /// Usage mistake message (exit status `2`), e.g. unknown `--only` or empty match.
    pub mistake: Option<&'static str>,
}

/// Outcome of one check inside a gated/traced run (`enum mo_check_outcome`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CheckOutcome {
    Passed,
    Failed,
    Stopped,
    OutOfSteps,
}

/// One planned or recorded check in a [`GatedRun`] (`struct mo_gated_check`).
#[derive(Debug, Clone)]
pub struct GatedCheck {
    pub name: String,
    pub kind: &'static str,
    pub limit: u64,
    pub steps: u64,
    pub outcome: CheckOutcome,
    pub entered: Vec<bool>,
}

/// Input and output state for [`run_gated`] (`struct mo_gated_run`).
#[derive(Debug, Clone)]
pub struct GatedRun {
    pub checks: Vec<GatedCheck>,
    /// When `false`, populate `checks` from `selection` in order; when `true`, follow `checks`.
    pub use_planned_checks: bool,
    pub record: bool,
    pub shrink: bool,
    pub stop: bool,
    pub limit: u64,
    pub start_limit: u64,
    pub started: bool,
    pub start_steps: u64,
    pub start_entered: Vec<bool>,
    pub ran: usize,
}

/// Appends the canonical qualified function name (including `@Receiver` when overloaded)
/// matching `src/tools/tests.c:mo_append_function_name`.
pub fn append_function_name(out: &mut String, program: &CheckedProgram, func_id: CheckedFuncId) {
    let func = program.function(func_id);
    let sym = program.interfaces.symbol(func.symbol);
    let mod_name = &program.modules.module(sym.module).name;
    out.push_str(mod_name);
    out.push('.');
    out.push_str(&sym.name);
    let scope = program.interfaces.scope(sym.module);
    if let Some(receiver_idx) = sym.receiver
        && let Some(group) = scope.find_symbol(sym.name.as_bytes())
        && group.overload_count > 1
    {
        out.push('@');
        out.push_str(&scope.symbols[receiver_idx.index()].name);
    }
}

/// Records an `E1404` tool pipeline diagnostic (`src/tools/tests.c:mo_test_report_error`).
pub fn test_report_error(diags: &mut DiagBag, message: &str, span: Span, detail: Option<&str>) {
    let mut diag = Diagnostic::new(ErrorCode::ToolPipeline, span, message);
    if let Some(d) = detail {
        diag = diag.with_primary_text(d);
    }
    let _ = diags.push(diag);
}

fn format_spec_name(
    program: &CheckedProgram,
    spec: &CheckedSpec,
    kind: ObservationKind,
    work: &mut usize,
) -> Option<String> {
    let mut name = String::new();
    match spec.subject {
        Some(subj) => append_function_name(&mut name, program, subj),
        None => name.push_str(&program.modules.module(spec.module).name),
    }
    name.push('.');
    name.push_str(kind.as_str());
    name.push('.');
    name.push_str(&spec.number.to_string());
    let cost = name.len() + 1;
    if cost > (*work).min(MO_CLI_DIAGNOSTIC_SIZE as usize) {
        return None;
    }
    *work -= cost;
    Some(name)
}

fn format_service_name(
    program: &CheckedProgram,
    module: ModuleId,
    promise_number: Option<u32>,
    work: &mut usize,
) -> Option<String> {
    let mut name = String::from(program.modules.module(module).name.as_str());
    match promise_number {
        None => name.push_str(RECOVERY_SUFFIX),
        Some(num) => {
            name.push_str(".service.promise.");
            name.push_str(&num.to_string());
        }
    }
    let cost = name.len() + 1;
    if cost > (*work).min(MO_CLI_DIAGNOSTIC_SIZE as usize) {
        return None;
    }
    *work -= cost;
    Some(name)
}

fn is_selected(
    program: &CheckedProgram,
    config: &TestConfig,
    module: ModuleId,
    full: &str,
    found: &mut [bool],
    work: &mut usize,
) -> Option<bool> {
    let mut chosen = config.only.is_empty();
    let full_bytes = full.as_bytes();
    for (j, pattern) in config.only.iter().enumerate() {
        let cost = if full_bytes.len() == pattern.len() {
            full_bytes.len() + 1
        } else {
            1
        };
        if cost > *work {
            return None;
        }
        *work -= cost;
        if full_bytes == pattern.as_slice() {
            chosen = true;
            found[j] = true;
        }
    }
    if !chosen {
        return Some(false);
    }
    if let Some(filter_mod) = &config.module {
        let mod_name = program.modules.module(module).name.as_bytes();
        return Some(filter_mod.as_slice() == mod_name);
    }
    Some(true)
}

/// Builds the ordered observation inventory and applies `--in` / `--only` filters
/// (`src/tools/tests.c:mo_select_tests`).
#[must_use]
pub fn select_tests(
    program: &CheckedProgram,
    config: &TestConfig,
    diags: &mut DiagBag,
) -> Option<TestSelection> {
    let mut candidates = (program.specs.len() as u64) + (program.services.len() as u64);
    for svc in &program.services {
        candidates += svc.promises.len() as u64;
    }
    if candidates > MO_LIMIT_CORE_NODES {
        test_report_error(
            diags,
            "test inventory exceeds tool resources",
            Span::NONE,
            None,
        );
        return None;
    }

    let mut items = Vec::new();
    let mut found = vec![false; config.only.len()];
    let mut work = MO_CLI_TEST_WORK as usize;

    for (spec_idx, spec) in program.specs.iter().enumerate() {
        let kind = match spec.kind {
            CheckedSpecKind::Anchor => continue,
            CheckedSpecKind::Test => ObservationKind::Test,
            CheckedSpecKind::Example => ObservationKind::Example,
            CheckedSpecKind::Promise => ObservationKind::Promise,
        };
        let m = program.modules.module(spec.module);
        if spec.module.0 != 0 && (m.library || m.dependency) {
            continue;
        }
        let Some(full) = format_spec_name(program, spec, kind, &mut work) else {
            test_report_error(
                diags,
                "test inventory exceeds tool resources",
                Span::NONE,
                None,
            );
            return None;
        };
        let Some(keep) = is_selected(program, config, spec.module, &full, &mut found, &mut work)
        else {
            test_report_error(
                diags,
                "test inventory exceeds tool resources",
                Span::NONE,
                None,
            );
            return None;
        };
        if keep {
            let span = m.syntax.tree.node(spec.syntax).span;
            items.push(Observation {
                name: full,
                kind,
                span,
                spec_index: Some(spec_idx),
                service_index: None,
                service_promise_index: None,
                instance_name: None,
                instance_index: None,
            });
        }
    }

    for (svc_idx, svc) in program.services.iter().enumerate() {
        let m = program.modules.module(svc.module);
        if svc.module.0 != 0 && m.library {
            continue;
        }
        for j in 0..=svc.promises.len() {
            let (kind, promise_num, span, sp_idx) = if j < svc.promises.len() {
                let p = &svc.promises[j];
                let sp = m.syntax.tree.node(p.spec.syntax).span;
                (ObservationKind::Promise, Some(p.spec.number), sp, Some(j))
            } else {
                let sp = m.syntax.tree.node(svc.syntax).span;
                (ObservationKind::Recovery, None, sp, None)
            };
            let Some(full) = format_service_name(program, svc.module, promise_num, &mut work)
            else {
                test_report_error(
                    diags,
                    "test inventory exceeds tool resources",
                    Span::NONE,
                    None,
                );
                return None;
            };
            let Some(keep) = is_selected(program, config, svc.module, &full, &mut found, &mut work)
            else {
                test_report_error(
                    diags,
                    "test inventory exceeds tool resources",
                    Span::NONE,
                    None,
                );
                return None;
            };
            if keep {
                items.push(Observation {
                    name: full,
                    kind,
                    span,
                    spec_index: None,
                    service_index: Some(svc_idx),
                    service_promise_index: sp_idx,
                    instance_name: None,
                    instance_index: None,
                });
            }
        }
    }

    let mut mistake = None;
    if found.iter().any(|&f| !f) {
        mistake = Some("--only does not name an example, promise, test or recovery check");
    } else if items.is_empty() {
        mistake = Some("test selection matches no examples, promises, tests or recovery checks");
    }
    Some(TestSelection { items, mistake })
}

/// Expands selected generic function promises into their lowered concrete instantiations
/// (`src/tools/tests.c:mo_instantiate_tests`).
#[must_use]
pub fn instantiate_tests(
    selection: &TestSelection,
    promises: &[LoweredPromise],
    diags: &mut DiagBag,
) -> Option<TestSelection> {
    let mut items = Vec::with_capacity(selection.items.len() + promises.len());
    let mut work = MO_CLI_TEST_WORK as usize;
    for obs in &selection.items {
        if items.len() as u64 == MO_LIMIT_CORE_NODES {
            test_report_error(
                diags,
                "test instances exceed tool resources",
                Span::NONE,
                None,
            );
            return None;
        }
        let Some(spec_idx) = obs.spec_index else {
            items.push(obs.clone());
            continue;
        };
        if obs.kind != ObservationKind::Promise {
            items.push(obs.clone());
            continue;
        }
        let before = items.len();
        let length = obs.name.len();
        for (j, promise) in promises.iter().enumerate() {
            if work == 0 {
                test_report_error(
                    diags,
                    "test instances exceed tool resources",
                    Span::NONE,
                    None,
                );
                return None;
            }
            work -= 1;
            if promise.spec.index() != spec_idx {
                continue;
            }
            if length > work || items.len() as u64 == MO_LIMIT_CORE_NODES {
                test_report_error(
                    diags,
                    "test instances exceed tool resources",
                    Span::NONE,
                    None,
                );
                return None;
            }
            work -= length;
            let mut item = obs.clone();
            if promise.name != obs.name {
                item.instance_name = Some(promise.name.clone());
                item.instance_index = Some(j);
            }
            items.push(item);
        }
        if items.len() == before {
            test_report_error(
                diags,
                "selected promise has no concrete instantiation",
                obs.span,
                Some(&obs.name),
            );
            return None;
        }
    }
    Some(TestSelection {
        items,
        mistake: None,
    })
}

fn bind_service(file: &BytecodeFile, program: &CheckedProgram, service_idx: usize) -> Option<u32> {
    let mod_name = &program
        .modules
        .module(program.services[service_idx].module)
        .name;
    let mut found = None;
    for (i, svc) in file.services.iter().enumerate() {
        let bc_mod = file.string(file.modules[svc.module as usize].name);
        if bc_mod == mod_name.as_str() {
            if found.is_some() {
                return None;
            }
            found = Some(i as u32);
        }
    }
    found
}

fn bind_observations(
    file: &BytecodeFile,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
) -> Option<Vec<u32>> {
    #[derive(Clone)]
    struct Key {
        span: Span,
        kind: u8,
        name: String,
        selected: usize,
    }
    let fn_kind_rank = |k: FunctionKind| -> u8 {
        match k {
            FunctionKind::Test => 0,
            FunctionKind::Example => 1,
            FunctionKind::Promise => 2,
            _ => 3,
        }
    };

    let mut functions = vec![NO_INDEX; selection.items.len()];
    let mut index = Vec::new();
    for (i, obs) in selection.items.iter().enumerate() {
        let Some(spec_idx) = obs.spec_index else {
            let svc_idx = obs.service_index?;
            functions[i] = bind_service(file, program, svc_idx)?;
            continue;
        };
        let spec = &program.specs[spec_idx];
        let kind = match spec.kind {
            CheckedSpecKind::Test => fn_kind_rank(FunctionKind::Test),
            CheckedSpecKind::Example => fn_kind_rank(FunctionKind::Example),
            CheckedSpecKind::Promise => fn_kind_rank(FunctionKind::Promise),
            CheckedSpecKind::Anchor => return None,
        };
        let name = obs
            .instance_name
            .as_deref()
            .unwrap_or(&obs.name)
            .to_string();
        index.push(Key {
            span: obs.span,
            kind,
            name,
            selected: i,
        });
    }
    index.sort_by(|a, b| {
        (
            a.span.file,
            a.span.start,
            a.span.end,
            a.kind,
            a.name.as_str(),
        )
            .cmp(&(
                b.span.file,
                b.span.start,
                b.span.end,
                b.kind,
                b.name.as_str(),
            ))
    });

    for (fn_idx, func) in file.functions.iter().enumerate() {
        if !matches!(
            func.kind,
            FunctionKind::Test | FunctionKind::Example | FunctionKind::Promise
        ) {
            continue;
        }
        let dbg = &file.debug[fn_idx];
        let span = runtime_span(file, sources, dbg.file, dbg.start, dbg.end);
        let kind = fn_kind_rank(func.kind);
        let name = file.string(func.name);
        let Ok(pos) = index.binary_search_by(|k| {
            (
                k.span.file,
                k.span.start,
                k.span.end,
                k.kind,
                k.name.as_str(),
            )
                .cmp(&(span.file, span.start, span.end, kind, name))
        }) else {
            continue;
        };
        let sel_idx = index[pos].selected;
        let obs = &selection.items[sel_idx];
        let spec_idx = obs.spec_index?;
        let spec = &program.specs[spec_idx];
        let is_promise = func.kind == FunctionKind::Promise;
        let want_params = if is_promise { spec.parameters.len() } else { 0 };
        let want_ret = if is_promise {
            TypeKind::Int
        } else {
            TypeKind::Unit
        };
        if func.parameters.len() != want_params
            || file.types.get(func.result_type).kind != want_ret
            || functions[sel_idx] != NO_INDEX
        {
            return None;
        }
        functions[sel_idx] = fn_idx as u32;
    }

    if functions.contains(&NO_INDEX) {
        return None;
    }
    Some(functions)
}

fn assertion_span(program: &CheckedProgram, spec: &CheckedSpec, failed: Span) -> Span {
    let tree = &program.modules.module(spec.module).syntax.tree;
    let mut best = Span::NONE;
    for expr in &spec.body.items {
        let node = tree.node(expr.syntax);
        if node.kind == SyntaxKind::Assert
            && node.span.file == failed.file
            && node.span.start <= failed.start
            && node.span.end >= failed.end
            && (best.file == u32::MAX
                || (node.span.end - node.span.start) < (best.end - best.start))
        {
            best = node.span;
        }
    }
    best
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C run_one signature across plain, promise, and service observations"
)]
fn run_one(
    config: &TestConfig,
    program: &CheckedProgram,
    image: &Image,
    sources: &SourceMap,
    items: &[Observation],
    function: u32,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) -> bool {
    let obs = &items[0];
    if obs.spec_index.is_none() {
        return run_service_tests(
            config.json,
            config.sequences,
            config.first_failure,
            config.limits,
            program,
            image,
            sources,
            items,
            function,
            output,
            diags,
            totals,
        );
    }
    if obs.kind == ObservationKind::Promise {
        return run_promise_test(
            config, program, image, sources, obs, function, output, diags, totals,
        );
    }

    let file = image.file();
    let start = Instant::now();
    let host = HostContext::stdio(Vec::new(), Vec::new());
    let mut process = Process::new(image, config.limits, host);

    let outcome_raw = if !config.door_plan || config.gated {
        process.call(function, Vec::new())
    } else {
        let ctrl = match Deadline::start(u32::try_from(config.seconds).unwrap_or(u32::MAX)) {
            Ok(mut deadline) => {
                let mut keep = || deadline.running();
                process.call_controlled(function, Vec::new(), Some(&mut keep), None)
            }
            Err(_) => ControlledOutcome::Cancelled,
        };
        match ctrl {
            ControlledOutcome::Cancelled => {
                test_report_error(
                    diags,
                    "example interrupted before its outcome was known",
                    obs.span,
                    Some(&obs.name),
                );
                return true;
            }
            ControlledOutcome::Returned(v) => Outcome::Returned(v),
            ControlledOutcome::Failed(d) => Outcome::Failed(d),
            ControlledOutcome::Stopped(d) => Outcome::Stopped(d),
        }
    };
    let elapsed_us = start.elapsed().as_micros().min(u128::from(u64::MAX)) as u64;

    let (outcome, defect_opt, line) = match outcome_raw {
        Outcome::Returned(_) => {
            totals.passed += 1;
            (TestOutcome::Passed, None, Span::NONE)
        }
        Outcome::Failed(defect) => {
            totals.failed += 1;
            if obs.kind == ObservationKind::Example {
                totals.failed_examples += 1;
            } else if obs.kind == ObservationKind::Test {
                totals.failed_tests += 1;
            }
            let line = if obs.kind == ObservationKind::Test
                && let Some(spec_idx) = obs.spec_index
            {
                let failed_span = defect
                    .frames
                    .first()
                    .and_then(|f| f.span)
                    .map_or(Span::NONE, |(f, s, e)| runtime_span(file, sources, f, s, e));
                assertion_span(program, &program.specs[spec_idx], failed_span)
            } else {
                Span::NONE
            };
            (TestOutcome::Failed, Some(defect), line)
        }
        Outcome::Stopped(defect) => {
            totals.stopped += 1;
            if obs.kind == ObservationKind::Test {
                totals.stopped_tests += 1;
            }
            (TestOutcome::Stopped, Some(defect), Span::NONE)
        }
    };

    let result = TestResult {
        observation: obs,
        file,
        outcome,
        vacuity: VacuityKind::None,
        defect: defect_opt.as_ref(),
        line,
        microseconds: elapsed_us,
        promise: None,
        recovery: None,
    };
    let shown = write_test_result(output, config.json, sources, &result);
    if !shown {
        test_report_error(
            diags,
            "test result could not be rendered or written completely",
            obs.span,
            Some(&obs.name),
        );
    }
    shown
}

fn observation_full_name(obs: &Observation) -> &str {
    obs.instance_name.as_deref().unwrap_or(&obs.name)
}

fn compute_owners(
    file: &BytecodeFile,
    program: &CheckedProgram,
    sources: &SourceMap,
) -> Vec<Option<usize>> {
    let mut owners = vec![None; file.functions.len()];
    for (b, func) in file.functions.iter().enumerate() {
        if func.spec_code {
            continue;
        }
        let dbg = &file.debug[b];
        let code = runtime_span(file, sources, dbg.file, dbg.start, dbg.end);
        for (i, cf) in program.functions.iter().enumerate() {
            let sym = program.interfaces.symbol(cf.symbol);
            let span = sym.declaration_span;
            if span.file == code.file && span.start <= code.start && code.end <= span.end {
                owners[b] = Some(i);
                break;
            }
        }
    }
    owners
}

fn attribute_entered(entered: &[bool], owners: &[Option<usize>], fn_count: usize) -> Vec<bool> {
    let mut out = vec![false; fn_count];
    for (b, &hit) in entered.iter().enumerate() {
        if hit && let Some(owner) = owners[b] {
            out[owner] = true;
        }
    }
    out
}

fn order_gated_checks(
    selection: &TestSelection,
    run: &mut GatedRun,
    diags: &mut DiagBag,
) -> Option<Vec<usize>> {
    if !run.use_planned_checks {
        let default_limit = if run.limit == 0 { u64::MAX } else { run.limit };
        run.checks = selection
            .items
            .iter()
            .map(|obs| GatedCheck {
                name: observation_full_name(obs).to_string(),
                kind: obs.kind.as_str(),
                limit: default_limit,
                steps: 0,
                outcome: CheckOutcome::Passed,
                entered: Vec::new(),
            })
            .collect();
    }
    let mut order = Vec::with_capacity(run.checks.len());
    let mut work = MO_CLI_TEST_WORK as usize;
    for check in &run.checks {
        let mut found = None;
        for (i, obs) in selection.items.iter().enumerate() {
            if observation_full_name(obs) == check.name.as_str() {
                found = Some(i);
                break;
            }
            if work == 0 {
                test_report_error(
                    diags,
                    "gated checks exceed tool resources",
                    Span::NONE,
                    None,
                );
                return None;
            }
            work -= 1;
        }
        let Some(idx) = found else {
            test_report_error(
                diags,
                "a planned check is not in the program",
                Span::NONE,
                Some(&check.name),
            );
            return None;
        };
        order.push(idx);
    }
    Some(order)
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_test_program orchestration with optional gated trace"
)]
fn test_program(
    config: &TestConfig,
    program: &CheckedProgram,
    selection: &TestSelection,
    wire: &[u8],
    sources: &SourceMap,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
    gated: Option<&mut GatedRun>,
) {
    let bc_file = match load_bytecode(wire) {
        Ok(f) => f,
        Err(err) => {
            test_report_error(
                diags,
                "compiled bytecode could not be loaded",
                Span::NONE,
                Some(err.reason),
            );
            return;
        }
    };

    let Some(functions) = bind_observations(&bc_file, program, sources, selection) else {
        test_report_error(
            diags,
            "selected observations could not be bound to compiled functions",
            Span::NONE,
            None,
        );
        return;
    };

    let mut gated = gated;
    let mut gated_order = Vec::new();
    let mut gated_owners = Vec::new();
    let mut initial_trace = None;
    if let Some(ref mut run) = gated {
        let Some(ord) = order_gated_checks(selection, run, diags) else {
            return;
        };
        gated_order = ord;
        let entered = run.record.then(|| vec![false; bc_file.functions.len()]);
        if run.record {
            gated_owners = compute_owners(&bc_file, program, sources);
        }
        initial_trace = Some(VmTrace {
            steps: 0,
            limit: run.start_limit,
            entered,
        });
    }

    let anchors = program_has_anchors(program);
    let init_res = if let Some(trace) = initial_trace {
        Image::init_traced(bc_file, config.limits, trace)
    } else if anchors {
        Image::init_bounded(bc_file, config.limits, MO_CLI_ANCHOR_INSTRUCTIONS)
    } else {
        Image::init(bc_file, config.limits)
    };

    let image = match init_res {
        Ok(img) => img,
        Err(boxed) => {
            let (bc_err_file, defect) = *boxed;
            let raw = render_defect_raw(&bc_err_file, &defect);
            let detail = if raw.len() <= MO_LIMIT_DEFECT_TEXT_SIZE as usize {
                raw
            } else {
                defect.reason.clone()
            };
            let span = defect
                .frames
                .first()
                .and_then(|f| f.span)
                .map_or(Span::NONE, |(f, s, e)| {
                    runtime_span(&bc_err_file, sources, f, s, e)
                });
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolTestInitialization,
                    span,
                    "program initialization stopped before tests could run",
                )
                .with_primary_text(detail),
            );
            if let Some(run) = gated {
                run.started = false;
            }
            return;
        }
    };

    let started = check_anchors(&image, config.limits, sources, program, diags);
    if let Some(ref mut run) = gated {
        run.started = started;
        run.start_steps = image.trace_steps();
        if run.record
            && let Some(ent) = image.trace_entered()
        {
            run.start_entered = attribute_entered(&ent, &gated_owners, program.functions.len());
        }
    }
    if !started {
        return;
    }

    if let Some(run) = gated {
        for (k, &sel_idx) in gated_order.iter().enumerate() {
            let check_limit = run.checks[k].limit;
            image.set_trace_check(check_limit, run.record);
            let before = *totals;
            let more = run_one(
                config,
                program,
                &image,
                sources,
                &selection.items[sel_idx..=sel_idx],
                functions[sel_idx],
                output,
                diags,
                totals,
            );
            let steps = image.trace_steps();
            let check = &mut run.checks[k];
            check.steps = steps;
            if steps > check.limit {
                check.outcome = CheckOutcome::OutOfSteps;
            } else if totals.passed != before.passed {
                check.outcome = CheckOutcome::Passed;
            } else if totals.failed != before.failed {
                check.outcome = CheckOutcome::Failed;
            } else if totals.stopped != before.stopped {
                check.outcome = CheckOutcome::Stopped;
            } else {
                return;
            }
            if run.record
                && let Some(ent) = image.trace_entered()
            {
                check.entered = attribute_entered(&ent, &gated_owners, program.functions.len());
            }
            run.ran = k + 1;
            if !more || (run.stop && check.outcome != CheckOutcome::Passed) {
                return;
            }
        }
    } else {
        let mut i = 0usize;
        while i < selection.items.len() {
            let obs = &selection.items[i];
            let mut count = 1usize;
            while obs.spec_index.is_none()
                && i + count < selection.items.len()
                && selection.items[i + count].service_index == obs.service_index
            {
                count += 1;
            }
            if !run_one(
                config,
                program,
                &image,
                sources,
                &selection.items[i..i + count],
                functions[i],
                output,
                diags,
                totals,
            ) {
                break;
            }
            i += count;
        }
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C run_tests internal pipeline entry"
)]
fn run_tests_inner(
    config: &TestConfig,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
    gated: Option<&mut GatedRun>,
) {
    if !promises_supported(program, selection, None, diags) {
        return;
    }
    let Some(lowered) = lower_test_services(program, Some(sources), diags) else {
        return;
    };
    if diags.has_any() {
        return;
    }
    let services_opt = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    if !check_program_services(&lowered.arena, &lowered.program, services_opt, diags) {
        return;
    }
    let Some(instantiated) = instantiate_tests(selection, &lowered.promises, diags) else {
        return;
    };
    if !promises_supported(
        program,
        &instantiated,
        Some((&lowered.promises, &lowered.extra_types)),
        diags,
    ) {
        return;
    }
    let Some(wire) = compile_services(&lowered.arena, &lowered.program, services_opt, diags) else {
        return;
    };
    test_program(
        config,
        program,
        &instantiated,
        &wire,
        sources,
        output,
        diags,
        totals,
        gated,
    );
}

/// Runs all selected `mo test` observations (`src/tools/tests.c:mo_run_tests`).
pub fn run_tests(
    config: &TestConfig,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) {
    run_tests_inner(
        config, program, sources, selection, output, diags, totals, None,
    );
}

/// Runs selected `mo test` observations under VM step/coverage tracing (`src/tools/tests.c:mo_run_gated`).
#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_run_gated signature directly"
)]
pub fn run_gated(
    config: &TestConfig,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
    run: &mut GatedRun,
) {
    let mut gated_cfg = config.clone();
    gated_cfg.gated = true;
    gated_cfg.first_failure = !run.shrink;
    run.started = false;
    run.start_steps = 0;
    run.start_entered.clear();
    run.ran = 0;
    run_tests_inner(
        &gated_cfg,
        program,
        sources,
        selection,
        output,
        diags,
        totals,
        Some(run),
    );
}
