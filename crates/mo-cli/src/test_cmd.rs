//! `mo test` Slice 3B bridge: complete observation selection (`--list`, `--in`, `--only`,
//! `--json`) and plain `test` + `example` execution (`src/tools/tests.c` parity).
//!
//! Promise/property generation, service recovery simulation, mutation testing (`--mutants`),
//! `--strength`, and upgrade bundles (`--bundle`) arrive in Slice 3C (`crates/mo-test`)
//! and are rejected with a clear diagnostic when executed in Slice 3B.

use std::io::Write;
use std::time::Instant;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_bytecode::{BytecodeFile, FunctionKind, TypeKind, load as load_bytecode};
use mo_check::{CheckedFuncId, CheckedProgram, CheckedSpec, CheckedSpecKind, ModuleId};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_ANCHOR_INSTRUCTIONS, MO_CLI_DIAGNOSTIC_SIZE, MO_CLI_TEST_WORK, MO_LIMIT_CORE_NODES,
    MO_LIMIT_DEFECT_TEXT_SIZE,
};
use mo_host::HostContext;
use mo_lower::lower_test_services;
use mo_syntax::SyntaxKind;
use mo_vm::{Image, Outcome, Process, render_defect_raw};

use crate::anchors::{check_anchors, program_has_anchors};
use crate::cli::Cli;
use crate::render::{
    Observation, ObservationKind, TestOutcome, TestResult, TestTotals, runtime_span,
    write_test_result,
};

const RECOVERY_SUFFIX: &str = ".service.recovery";

/// Result of [`select_tests`] matching `struct mo_test_selection`.
#[derive(Debug, Clone)]
pub struct TestSelection {
    pub items: Vec<Observation>,
    /// Usage mistake message (exit status `2`), e.g. unknown `--only` or empty match.
    pub mistake: Option<&'static str>,
}

fn append_function_name(out: &mut String, program: &CheckedProgram, func_id: CheckedFuncId) {
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
    cli: &Cli,
    module: ModuleId,
    full: &str,
    found: &mut [bool],
    work: &mut usize,
) -> Option<bool> {
    let mut chosen = cli.only.is_empty();
    let full_bytes = full.as_bytes();
    for (j, pattern) in cli.only.iter().enumerate() {
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
    if let Some(filter_mod) = &cli.module {
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
    cli: &Cli,
    diags: &mut DiagBag,
) -> Option<TestSelection> {
    let mut candidates = (program.specs.len() as u64) + (program.services.len() as u64);
    for svc in &program.services {
        candidates += svc.promises.len() as u64;
    }
    if candidates > MO_LIMIT_CORE_NODES {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::ToolPipeline,
            Span::NONE,
            "test inventory exceeds tool resources",
        ));
        return None;
    }

    let mut items = Vec::new();
    let mut found = vec![false; cli.only.len()];
    let mut work = MO_CLI_TEST_WORK as usize;

    for (spec_idx, spec) in program.specs.iter().enumerate() {
        let kind = match spec.kind {
            CheckedSpecKind::Anchor => continue,
            CheckedSpecKind::Test => ObservationKind::Test,
            CheckedSpecKind::Example => ObservationKind::Example,
            CheckedSpecKind::Promise => ObservationKind::Promise,
        };
        // A program's tests don't include the toolchain library's or path dependencies';
        // `mo test lib/list.mo` does.
        let m = program.modules.module(spec.module);
        if spec.module.0 != 0 && (m.library || m.dependency) {
            continue;
        }
        let Some(full) = format_spec_name(program, spec, kind, &mut work) else {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                "test inventory exceeds tool resources",
            ));
            return None;
        };
        let Some(keep) = is_selected(program, cli, spec.module, &full, &mut found, &mut work)
        else {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                "test inventory exceeds tool resources",
            ));
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
                let _ = diags.push(Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "test inventory exceeds tool resources",
                ));
                return None;
            };
            let Some(keep) = is_selected(program, cli, svc.module, &full, &mut found, &mut work)
            else {
                let _ = diags.push(Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "test inventory exceeds tool resources",
                ));
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

fn test_report_error(diags: &mut DiagBag, message: &str, span: Span, detail: Option<&str>) {
    let mut diag = Diagnostic::new(ErrorCode::ToolPipeline, span, message);
    if let Some(d) = detail {
        diag = diag.with_primary_text(d);
    }
    let _ = diags.push(diag);
}

fn bind_plain_observations(
    file: &BytecodeFile,
    sources: &SourceMap,
    selection: &TestSelection,
) -> Option<Vec<u32>> {
    let mut functions = vec![u32::MAX; selection.items.len()];
    for (idx, obs) in selection.items.iter().enumerate() {
        let want_kind = match obs.kind {
            ObservationKind::Test => FunctionKind::Test,
            ObservationKind::Example => FunctionKind::Example,
            ObservationKind::Promise | ObservationKind::Recovery => return None,
        };
        let mut matched = None;
        for (fn_idx, func) in file.functions.iter().enumerate() {
            if func.kind != want_kind {
                continue;
            }
            if file.string(func.name) != obs.name.as_str() {
                continue;
            }
            let dbg = &file.debug[fn_idx];
            let span = runtime_span(file, sources, dbg.file, dbg.start, dbg.end);
            if span != obs.span {
                continue;
            }
            if !func.parameters.is_empty()
                || file.types.get(func.result_type).kind != TypeKind::Unit
                || matched.is_some()
            {
                return None;
            }
            matched = Some(fn_idx as u32);
        }
        functions[idx] = matched?;
    }
    Some(functions)
}

/// Finds the smallest `SyntaxKind::Assert` statement in `spec.body` enclosing `failed`
/// (`src/tools/tests.c:assertion_span`).
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

/// Executes selected plain `test` and `example` observations (`src/tools/tests.c:mo_run_tests`).
///
/// Rejects promise and service-recovery observations with a clear diagnostic until Slice 3C.
pub fn run_tests(
    cli: &Cli,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) {
    for obs in &selection.items {
        if matches!(
            obs.kind,
            ObservationKind::Promise | ObservationKind::Recovery
        ) {
            test_report_error(
                diags,
                "promise and service recovery test execution is deferred to Phase 3 Slice 3C (crates/mo-test)",
                obs.span,
                Some(&obs.name),
            );
            return;
        }
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
    let Some(wire) = compile_services(&lowered.arena, &lowered.program, services_opt, diags) else {
        return;
    };

    let bc_file = match load_bytecode(&wire) {
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

    let Some(functions) = bind_plain_observations(&bc_file, sources, selection) else {
        test_report_error(
            diags,
            "selected observations could not be bound to compiled functions",
            Span::NONE,
            None,
        );
        return;
    };

    let anchors = program_has_anchors(program);
    let init_res = if anchors {
        Image::init_bounded(bc_file, cli.limits, MO_CLI_ANCHOR_INSTRUCTIONS)
    } else {
        Image::init(bc_file, cli.limits)
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
            return;
        }
    };

    if !check_anchors(&image, cli, sources, program, diags) {
        return;
    }

    let file = image.file();
    for (idx, obs) in selection.items.iter().enumerate() {
        let fn_idx = functions[idx];
        let start = Instant::now();
        let host = HostContext::stdio(Vec::new(), Vec::new());
        let mut process = Process::new(&image, cli.limits, host);
        let outcome_raw = process.call(fn_idx, Vec::new());
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
            defect: defect_opt.as_ref(),
            line,
            microseconds: elapsed_us,
        };
        if !write_test_result(output, cli.json, sources, &result) {
            test_report_error(
                diags,
                "test result could not be rendered or written completely",
                obs.span,
                Some(&obs.name),
            );
            break;
        }
    }
}
