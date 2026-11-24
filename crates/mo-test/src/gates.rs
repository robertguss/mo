//! Mutation testing gate runner (`src/tools/gates.{h,c}`).

use std::io::Write;

use mo_base::{DiagBag, Diagnostic, RenderWriter, SourceMap, Span, TerminalEncoding};
use mo_check::{CheckedProgram, check_program, load_program_sources};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_GATE_STEP_FLOOR, MO_LIMIT_GATE_STEP_MULTIPLE,
};
use mo_syntax::SyntaxKind;

use crate::mutants::{Mutant, list_mutants, module_tested, mutant_sources};
use crate::render::{
    GATE_KILL_NAMES, MutantTotals, TestTotals, UNICODE16_METRICS, detect_terminal_encoding,
};
use crate::runner::{
    CheckOutcome, GatedCheck, GatedRun, TestConfig, TestSelection, append_function_name, run_gated,
    select_tests, test_report_error,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Verdict {
    Dropped,
    Killed,
    Survived,
    Unrun,
    Broken,
}

impl Verdict {
    const fn outcome_str(self) -> &'static str {
        match self {
            Self::Dropped => "dropped",
            Self::Killed => "killed",
            Self::Survived | Self::Unrun => "survived",
            Self::Broken => "broken",
        }
    }
}

/// Computes the per-check step budget for a mutant from its baseline step count
/// (`src/tools/gates.c:mo_gate_limit`).
#[must_use]
pub const fn gate_limit(steps: u64) -> u64 {
    let scaled = if steps > u64::MAX / MO_LIMIT_GATE_STEP_MULTIPLE {
        u64::MAX
    } else {
        steps * MO_LIMIT_GATE_STEP_MULTIPLE
    };
    if scaled < MO_LIMIT_GATE_STEP_FLOOR {
        MO_LIMIT_GATE_STEP_FLOOR
    } else {
        scaled
    }
}

/// Returns whether every diagnostic in `diags` is a startup failure (anchor or test init)
/// with zero dropped diagnostics (`src/tools/gates.c:mo_gate_start_failed`).
#[must_use]
pub fn gate_start_failed(diags: &DiagBag) -> bool {
    if diags.is_empty() || diags.dropped() != 0 {
        return false;
    }
    diags.items().iter().all(|d| {
        matches!(
            d.code,
            ErrorCode::ToolAnchor | ErrorCode::ToolTestInitialization
        )
    })
}

fn describe_mutant(program: &CheckedProgram, m: &Mutant) -> String {
    let mut out = String::new();
    append_function_name(
        &mut out,
        program,
        mo_check::CheckedFuncId(m.function as u32),
    );
    out.push_str(": ");
    out.push_str(m.kind);
    out.push_str(", `");
    out.push_str(&m.original);
    if m.replacement.is_empty() {
        out.push_str("` removed");
    } else {
        out.push_str("` to `");
        out.push_str(&m.replacement);
        out.push('`');
    }
    out
}

fn json_opt_str(w: &mut RenderWriter, text: Option<&str>) {
    match text {
        Some(s) => w.json_str(s),
        None => w.text("null"),
    }
}

fn write_all_and_flush(stream: &mut dyn Write, data: &[u8]) -> bool {
    stream.write_all(data).is_ok() && stream.flush().is_ok()
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C write_mutant signature directly"
)]
fn write_mutant(
    output: &mut dyn Write,
    json: bool,
    program: &CheckedProgram,
    sources: &SourceMap,
    m: &Mutant,
    outcome: Option<&str>,
    by: Option<&str>,
    check: Option<&str>,
) -> bool {
    let path_bytes = sources
        .get(m.span.file)
        .map_or(&b""[..], |s| s.path_bytes());
    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };
    let mut out = RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
    if json {
        let mut fname = String::new();
        append_function_name(
            &mut fname,
            program,
            mo_check::CheckedFuncId(m.function as u32),
        );
        out.text("{\"record\":\"mutant\",\"function\":");
        out.json_str(&fname);
        out.text(",\"file\":");
        out.json_bytes(path_bytes);
        out.text(",\"line\":");
        out.u64_dec(u64::from(m.position.line));
        out.text(",\"column\":");
        out.u64_dec(u64::from(m.position.column));
        out.text(",\"kind\":");
        out.json_str(m.kind);
        out.text(",\"original\":");
        out.json_str(&m.original);
        out.text(",\"replacement\":");
        out.json_str(&m.replacement);
        if let Some(outcome_str) = outcome {
            out.text(",\"outcome\":");
            out.json_str(outcome_str);
            out.text(",\"by\":");
            json_opt_str(&mut out, by);
            out.text(",\"check\":");
            json_opt_str(&mut out, check);
        }
        out.text("}\n");
    } else {
        let desc = describe_mutant(program, m);
        out.text("mutant ");
        out.safe_bytes(path_bytes);
        out.text(":");
        out.u64_dec(u64::from(m.position.line));
        out.text(":");
        out.u64_dec(u64::from(m.position.column));
        out.text(" ");
        out.safe_str(&desc);
        out.text("\n");
    }
    match out.finish() {
        Ok(buf) => write_all_and_flush(output, &buf),
        Err(_) => false,
    }
}

fn count_kill(totals: &mut MutantTotals, kind: &str) {
    for (i, &name) in GATE_KILL_NAMES.iter().enumerate() {
        if kind == name {
            totals.killed[i] += 1;
            return;
        }
    }
    unreachable!("unknown kill kind: {kind}");
}

fn build_plan(first: &GatedRun, sorted: &[GatedCheck], function: usize) -> Vec<GatedCheck> {
    let every = first.start_entered.get(function).copied().unwrap_or(false);
    let mut plan = Vec::new();
    for check in sorted {
        if every || check.entered.get(function).copied().unwrap_or(false) {
            plan.push(GatedCheck {
                name: check.name.clone(),
                kind: check.kind,
                limit: gate_limit(check.steps),
                steps: 0,
                outcome: CheckOutcome::Passed,
                entered: Vec::new(),
            });
        }
    }
    plan
}

fn report_broken(diags: &mut DiagBag, m: &Mutant, sub_diags: &DiagBag, fallback: &str) {
    let owned;
    let detail = match sub_diags.items().first() {
        Some(d) => {
            owned = d.message.as_str_lossy();
            owned.as_ref()
        }
        None => fallback,
    };
    test_report_error(diags, "a mutant could not be tried", m.span, Some(detail));
}

fn report_survived(diags: &mut DiagBag, program: &CheckedProgram, m: &Mutant, unrun: bool) {
    let mut detail = describe_mutant(program, m);
    if unrun {
        detail.push_str("; no check runs this function");
    }
    let _ = diags.push(
        Diagnostic::new(
            ErrorCode::MutantSurvived,
            m.span,
            "a mutant survived: no check noticed this change",
        )
        .with_primary_text(detail),
    );
}

fn try_mutant(
    config: &TestConfig,
    program: &CheckedProgram,
    sources: &SourceMap,
    first: &GatedRun,
    sorted: &[GatedCheck],
    m: &Mutant,
    diags: &mut DiagBag,
) -> (Verdict, Option<&'static str>, Option<String>) {
    let plan = build_plan(first, sorted, m.function);
    let mut sub_diags = DiagBag::new();
    let Some(mut mutated_sources) = mutant_sources(program, sources, m) else {
        report_broken(diags, m, &sub_diags, "its sources could not be built");
        return (Verdict::Broken, None, None);
    };
    let lib_flags: Vec<bool> = program
        .modules
        .modules
        .iter()
        .map(|md| md.library)
        .collect();
    let Some(set) = load_program_sources(&mut mutated_sources, Some(&lib_flags), &mut sub_diags)
    else {
        report_broken(diags, m, &sub_diags, "it could not be loaded");
        return (Verdict::Broken, None, None);
    };
    if sub_diags.has_any() {
        report_broken(diags, m, &sub_diags, "it could not be loaded");
        return (Verdict::Broken, None, None);
    }
    let Some(checked) = check_program(Some(&mutated_sources), set, &mut sub_diags) else {
        return (Verdict::Dropped, None, None);
    };
    if sub_diags.has_any() {
        return (Verdict::Dropped, None, None);
    }
    let start_hit = first
        .start_entered
        .get(m.function)
        .copied()
        .unwrap_or(false);
    if plan.is_empty() && !start_hit {
        return (Verdict::Unrun, None, None);
    }
    let Some(selection) = select_tests(&checked, config, &mut sub_diags) else {
        report_broken(diags, m, &sub_diags, "its checks could not be selected");
        return (Verdict::Broken, None, None);
    };
    let mut run = GatedRun {
        checks: plan,
        use_planned_checks: true,
        record: false,
        shrink: false,
        stop: true,
        limit: 0,
        start_limit: gate_limit(first.start_steps),
        started: false,
        start_steps: 0,
        start_entered: Vec::new(),
        ran: 0,
    };
    let mut totals = TestTotals::default();
    let mut sink = std::io::sink();
    run_gated(
        config,
        &checked,
        &mutated_sources,
        &selection,
        &mut sink,
        &mut sub_diags,
        &mut totals,
        &mut run,
    );
    if !run.started && gate_start_failed(&sub_diags) {
        (Verdict::Killed, Some("start"), None)
    } else if run.started && run.ran != 0 && run.checks[run.ran - 1].outcome != CheckOutcome::Passed
    {
        let killer = &run.checks[run.ran - 1];
        (
            Verdict::Killed,
            Some(killer.kind),
            Some(killer.name.clone()),
        )
    } else if run.started && !sub_diags.has_any() && run.ran == run.checks.len() {
        (Verdict::Survived, None, None)
    } else {
        report_broken(diags, m, &sub_diags, "its checks did not finish");
        (Verdict::Broken, None, None)
    }
}

/// Runs all selected checks with tracing and then tries every source mutant
/// (`src/tools/gates.c:mo_gates`).
pub fn run_gates(
    config: &TestConfig,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) {
    totals.mutants = MutantTotals {
        asked: true,
        ..MutantTotals::default()
    };
    let mut first = GatedRun {
        checks: Vec::new(),
        use_planned_checks: false,
        record: true,
        shrink: true,
        stop: false,
        limit: 0,
        start_limit: u64::MAX,
        started: false,
        start_steps: 0,
        start_entered: Vec::new(),
        ran: 0,
    };
    run_gated(
        config, program, sources, selection, output, diags, totals, &mut first,
    );
    let ran = first.started && first.ran == first.checks.len() && !diags.has_any();
    if !ran || totals.failed != 0 || totals.stopped != 0 {
        return;
    }
    let mutants = match list_mutants(program, sources) {
        Ok(list) => list,
        Err(problem) => {
            test_report_error(
                diags,
                "the mutants could not be listed",
                Span::NONE,
                Some(problem),
            );
            return;
        }
    };
    let mut sorted = first.checks.clone();
    sorted.sort_by(|a, b| a.steps.cmp(&b.steps).then_with(|| a.name.cmp(&b.name)));

    let mut finished = true;
    for m in &mutants {
        let (verdict, kind, check) =
            try_mutant(config, program, sources, &first, &sorted, m, diags);
        match verdict {
            Verdict::Dropped => totals.mutants.dropped += 1,
            Verdict::Killed => {
                totals.mutants.tried += 1;
                if let Some(k) = kind {
                    count_kill(&mut totals.mutants, k);
                }
            }
            Verdict::Survived | Verdict::Unrun => {
                totals.mutants.tried += 1;
                totals.mutants.survived += 1;
                report_survived(diags, program, m, verdict == Verdict::Unrun);
            }
            Verdict::Broken => {
                finished = false;
                break;
            }
        }
        if config.json
            && !write_mutant(
                output,
                true,
                program,
                sources,
                m,
                Some(verdict.outcome_str()),
                kind,
                check.as_deref(),
            )
        {
            test_report_error(
                diags,
                "a mutant's record could not be written",
                m.span,
                None,
            );
            finished = false;
            break;
        }
    }
    if finished {
        totals.mutants.ran = true;
    }
}

/// Lists every candidate mutant and per-function mutant count (`mo test --mutants --list`,
/// `src/tools/gates.c:mo_gates_list`).
pub fn list_gates(
    program: &CheckedProgram,
    sources: &SourceMap,
    json: bool,
    output: &mut dyn Write,
    diags: &mut DiagBag,
) {
    let mutants = match list_mutants(program, sources) {
        Ok(list) => list,
        Err(problem) => {
            test_report_error(
                diags,
                "the mutants could not be listed",
                Span::NONE,
                Some(problem),
            );
            return;
        }
    };
    let mut ok = true;
    for m in &mutants {
        if !ok {
            break;
        }
        ok = write_mutant(output, json, program, sources, m, None, None, None);
    }
    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };
    for (f_idx, func) in program.functions.iter().enumerate() {
        if !ok {
            break;
        }
        let sym = program.interfaces.symbol(func.symbol);
        if !module_tested(program, sym.module) || sym.decl_kind != SyntaxKind::Fn {
            continue;
        }
        let count = mutants.iter().filter(|m| m.function == f_idx).count() as u64;
        let mut fname = String::new();
        append_function_name(&mut fname, program, mo_check::CheckedFuncId(f_idx as u32));
        let mut out =
            RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
        if json {
            out.text("{\"record\":\"function\",\"name\":");
            out.json_str(&fname);
            out.text(",\"mutants\":");
            out.u64_dec(count);
            out.text("}\n");
        } else {
            out.text("function ");
            out.safe_str(&fname);
            out.text(": ");
            out.u64_dec(count);
            out.text(if count == 1 {
                " mutant\n"
            } else {
                " mutants\n"
            });
        }
        ok = match out.finish() {
            Ok(buf) => write_all_and_flush(output, &buf),
            Err(_) => false,
        };
    }
    if !ok {
        test_report_error(
            diags,
            "the mutants could not be listed",
            Span::NONE,
            Some("their records could not be written"),
        );
    }
}

/// Renders the mutant gate summary fragment for `totals` (both JSON and text),
/// matching `src/tools/gates.c:mo_render_gates`.
pub fn render_gates_summary(w: &mut RenderWriter, mutants: &MutantTotals, json: bool) {
    let killed: u64 = mutants.killed.iter().map(|&k| u64::from(k)).sum();
    if !mutants.ran {
        if json {
            w.text(",\"mutants\":null");
        } else {
            w.text("mutants: did not run: a check did not pass\n");
        }
        return;
    }
    if json {
        w.text(",\"mutants\":{\"tried\":");
        w.u64_dec(u64::from(mutants.tried));
        w.text(",\"killed\":{");
        for (i, &name) in GATE_KILL_NAMES.iter().enumerate() {
            w.text(if i == 0 { "\"" } else { ",\"" });
            w.text(name);
            w.text("\":");
            w.u64_dec(u64::from(mutants.killed[i]));
        }
        w.text("},\"survived\":");
        w.u64_dec(u64::from(mutants.survived));
        w.text(",\"dropped\":");
        w.u64_dec(u64::from(mutants.dropped));
        w.text("}");
    } else {
        w.text("mutants: ");
        w.u64_dec(u64::from(mutants.tried));
        w.text(" tried, ");
        w.u64_dec(killed);
        w.text(" killed (");
        for (i, &name) in GATE_KILL_NAMES.iter().enumerate() {
            if i != 0 {
                w.text(", ");
            }
            w.u64_dec(u64::from(mutants.killed[i]));
            w.text(" ");
            w.text(name);
        }
        w.text("), ");
        w.u64_dec(u64::from(mutants.survived));
        w.text(" survived, ");
        w.u64_dec(u64::from(mutants.dropped));
        w.text(" dropped\n");
    }
}
