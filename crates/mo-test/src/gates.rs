//! Mutation testing gate runner (`src/tools/gates.{h,c}`).

use std::io::Write;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use mo_base::{DiagBag, Diagnostic, RenderWriter, SourceMap, Span, TerminalEncoding};
use mo_check::{CheckedProgram, check_program, load_program_sources};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_GATE_STEP_FLOOR, MO_LIMIT_GATE_STEP_MULTIPLE,
    MO_SOURCE_STACK_SIZE,
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

fn write_mutant_event(
    output: &mut dyn Write,
    program: &CheckedProgram,
    sources: &SourceMap,
    m: &Mutant,
    verdict: Verdict,
    by: Option<&str>,
    check: Option<&str>,
) -> bool {
    if !matches!(
        verdict,
        Verdict::Killed | Verdict::Survived | Verdict::Unrun
    ) {
        return true;
    }
    let path_bytes = sources
        .get(m.span.file)
        .map_or(&b""[..], |s| s.path_bytes());
    let func = &program.functions[m.function];
    let sym = program.interfaces.symbol(func.symbol);
    let mod_name = &program.modules.module(sym.module).name;
    let short_fn = sym.name.as_str();
    let mut fname = String::new();
    append_function_name(
        &mut fname,
        program,
        mo_check::CheckedFuncId(m.function as u32),
    );

    let mut out = RenderWriter::new(
        MO_CLI_DIAGNOSTIC_SIZE as usize,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    );
    let ev_name = if verdict == Verdict::Killed {
        "mutant_killed"
    } else {
        "mutant_survived"
    };
    out.text("{\"record\":");
    out.json_str(ev_name);
    out.text(",\"event\":");
    out.json_str(ev_name);
    out.text(",\"module\":");
    out.json_str(mod_name);
    out.text(",\"function\":");
    out.json_str(&fname);
    out.text(",\"fn\":");
    out.json_str(short_fn);
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
    if verdict == Verdict::Killed {
        let killer = check.or(by).unwrap_or("start");
        out.text(",\"by\":");
        json_opt_str(&mut out, by);
        out.text(",\"check\":");
        json_opt_str(&mut out, check);
        out.text(",\"killed_by\":");
        out.json_str(killer);
    } else {
        out.text(",\"unrun\":");
        out.text(if verdict == Verdict::Unrun {
            "true"
        } else {
            "false"
        });
    }
    out.text("}\n");
    match out.finish() {
        Ok(buf) => write_all_and_flush(output, &buf),
        Err(_) => false,
    }
}

fn write_mutant_summary(output: &mut dyn Write, mutants: &MutantTotals) -> bool {
    let killed: u64 = mutants.killed.iter().map(|&k| u64::from(k)).sum();
    let tried = u64::from(mutants.tried);
    let survived = u64::from(mutants.survived);
    let dropped = u64::from(mutants.dropped);
    let total = tried + dropped;
    let kill_rate = if tried == 0 {
        "1.0".to_string()
    } else {
        format!("{:.4}", (killed as f64) / (tried as f64))
    };

    let mut out = RenderWriter::new(
        MO_CLI_DIAGNOSTIC_SIZE as usize,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    );
    out.text("{\"record\":\"mutant_summary\",\"event\":\"mutant_summary\",\"total\":");
    out.u64_dec(total);
    out.text(",\"tried\":");
    out.u64_dec(tried);
    out.text(",\"killed\":");
    out.u64_dec(killed);
    out.text(",\"killed_by_kind\":{");
    for (i, &name) in GATE_KILL_NAMES.iter().enumerate() {
        out.text(if i == 0 { "\"" } else { ",\"" });
        out.text(name);
        out.text("\":");
        out.u64_dec(u64::from(mutants.killed[i]));
    }
    out.text("},\"survived\":");
    out.u64_dec(survived);
    out.text(",\"dropped\":");
    out.u64_dec(dropped);
    out.text(",\"kill_rate\":");
    out.text(&kill_rate);
    out.text("}\n");
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

struct MutantContext<'a> {
    config: &'a TestConfig,
    program: &'a CheckedProgram,
    sources: &'a SourceMap,
    lib_flags: &'a [bool],
    first: &'a GatedRun,
    sorted: &'a [GatedCheck],
}

struct MutantTrial {
    verdict: Verdict,
    by: Option<&'static str>,
    check: Option<String>,
    broken_diags: DiagBag,
    broken_fallback: &'static str,
}

fn try_mutant_isolated(ctx: &MutantContext<'_>, m: &Mutant) -> MutantTrial {
    let plan = build_plan(ctx.first, ctx.sorted, m.function);
    let mut sub_diags = DiagBag::new();
    let Some(mut mutated_sources) = mutant_sources(ctx.program, ctx.sources, m) else {
        return MutantTrial {
            verdict: Verdict::Broken,
            by: None,
            check: None,
            broken_diags: sub_diags,
            broken_fallback: "its sources could not be built",
        };
    };
    let Some(set) = load_program_sources(&mut mutated_sources, Some(ctx.lib_flags), &mut sub_diags)
    else {
        return MutantTrial {
            verdict: Verdict::Broken,
            by: None,
            check: None,
            broken_diags: sub_diags,
            broken_fallback: "it could not be loaded",
        };
    };
    if sub_diags.has_any() {
        return MutantTrial {
            verdict: Verdict::Broken,
            by: None,
            check: None,
            broken_diags: sub_diags,
            broken_fallback: "it could not be loaded",
        };
    }
    let Some(checked) = check_program(Some(&mutated_sources), set, &mut sub_diags) else {
        return MutantTrial {
            verdict: Verdict::Dropped,
            by: None,
            check: None,
            broken_diags: DiagBag::new(),
            broken_fallback: "",
        };
    };
    if sub_diags.has_any() {
        return MutantTrial {
            verdict: Verdict::Dropped,
            by: None,
            check: None,
            broken_diags: DiagBag::new(),
            broken_fallback: "",
        };
    }
    let start_hit = ctx
        .first
        .start_entered
        .get(m.function)
        .copied()
        .unwrap_or(false);
    if plan.is_empty() && !start_hit {
        return MutantTrial {
            verdict: Verdict::Unrun,
            by: None,
            check: None,
            broken_diags: DiagBag::new(),
            broken_fallback: "",
        };
    }
    let Some(selection) = select_tests(&checked, ctx.config, &mut sub_diags) else {
        return MutantTrial {
            verdict: Verdict::Broken,
            by: None,
            check: None,
            broken_diags: sub_diags,
            broken_fallback: "its checks could not be selected",
        };
    };
    let mut run = GatedRun {
        checks: plan,
        use_planned_checks: true,
        record: false,
        shrink: false,
        stop: true,
        limit: 0,
        start_limit: gate_limit(ctx.first.start_steps),
        started: false,
        start_steps: 0,
        start_entered: Vec::new(),
        ran: 0,
    };
    let mut totals = TestTotals::default();
    let mut sink = std::io::sink();
    run_gated(
        ctx.config,
        &checked,
        &mutated_sources,
        &selection,
        &mut sink,
        &mut sub_diags,
        &mut totals,
        &mut run,
    );
    if !run.started && gate_start_failed(&sub_diags) {
        MutantTrial {
            verdict: Verdict::Killed,
            by: Some("start"),
            check: None,
            broken_diags: DiagBag::new(),
            broken_fallback: "",
        }
    } else if run.started && run.ran != 0 && run.checks[run.ran - 1].outcome != CheckOutcome::Passed
    {
        let killer = &run.checks[run.ran - 1];
        MutantTrial {
            verdict: Verdict::Killed,
            by: Some(killer.kind),
            check: Some(killer.name.clone()),
            broken_diags: DiagBag::new(),
            broken_fallback: "",
        }
    } else if run.started && !sub_diags.has_any() && run.ran == run.checks.len() {
        MutantTrial {
            verdict: Verdict::Survived,
            by: None,
            check: None,
            broken_diags: DiagBag::new(),
            broken_fallback: "",
        }
    } else {
        MutantTrial {
            verdict: Verdict::Broken,
            by: None,
            check: None,
            broken_diags: sub_diags,
            broken_fallback: "its checks did not finish",
        }
    }
}

fn apply_mutant_trial(
    ctx: &MutantContext<'_>,
    json: bool,
    m: &Mutant,
    trial: &MutantTrial,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) -> bool {
    match trial.verdict {
        Verdict::Dropped => totals.mutants.dropped += 1,
        Verdict::Killed => {
            totals.mutants.tried += 1;
            if let Some(k) = trial.by {
                count_kill(&mut totals.mutants, k);
            }
        }
        Verdict::Survived | Verdict::Unrun => {
            totals.mutants.tried += 1;
            totals.mutants.survived += 1;
            report_survived(diags, ctx.program, m, trial.verdict == Verdict::Unrun);
        }
        Verdict::Broken => {
            report_broken(diags, m, &trial.broken_diags, trial.broken_fallback);
            return false;
        }
    }
    if json {
        let wrote_base = write_mutant(
            output,
            true,
            ctx.program,
            ctx.sources,
            m,
            Some(trial.verdict.outcome_str()),
            trial.by,
            trial.check.as_deref(),
        );
        let wrote_event = wrote_base
            && write_mutant_event(
                output,
                ctx.program,
                ctx.sources,
                m,
                trial.verdict,
                trial.by,
                trial.check.as_deref(),
            );
        if !wrote_event {
            test_report_error(
                diags,
                "a mutant's record could not be written",
                m.span,
                None,
            );
            return false;
        }
    }
    true
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

    let lib_flags: Vec<bool> = program
        .modules
        .modules
        .iter()
        .map(|md| md.library)
        .collect();
    let mut serial_cfg = config.clone();
    serial_cfg.jobs = 1;
    let ctx = MutantContext {
        config: &serial_cfg,
        program,
        sources,
        lib_flags: &lib_flags,
        first: &first,
        sorted: &sorted,
    };

    let workers = config.jobs.max(1).min(mutants.len().max(1));
    let mut finished = true;
    if workers <= 1 || mutants.len() <= 1 {
        for m in &mutants {
            let trial = try_mutant_isolated(&ctx, m);
            if !apply_mutant_trial(&ctx, config.json, m, &trial, output, diags, totals) {
                finished = false;
                break;
            }
        }
    } else {
        let next_idx = AtomicUsize::new(0);
        let broken_at = AtomicUsize::new(mutants.len());
        let slots: Mutex<Vec<Option<MutantTrial>>> =
            Mutex::new((0..mutants.len()).map(|_| None).collect());

        let work_loop = || {
            loop {
                let idx = next_idx.fetch_add(1, Ordering::Relaxed);
                if idx >= broken_at.load(Ordering::Relaxed) || idx >= mutants.len() {
                    break;
                }
                let trial = try_mutant_isolated(&ctx, &mutants[idx]);
                if trial.verdict == Verdict::Broken {
                    broken_at.fetch_min(idx, Ordering::Relaxed);
                }
                if let Ok(mut guard) = slots.lock() {
                    guard[idx] = Some(trial);
                }
            }
        };

        std::thread::scope(|s| {
            for _ in 1..workers {
                let _ = std::thread::Builder::new()
                    .name("mo-mutant-worker".to_string())
                    .stack_size(MO_SOURCE_STACK_SIZE as usize)
                    .spawn_scoped(s, work_loop);
            }
            work_loop();
        });

        let collected = match slots.into_inner() {
            Ok(v) => v,
            Err(poisoned) => poisoned.into_inner(),
        };
        for (m, slot) in mutants.iter().zip(collected.iter()) {
            let Some(trial) = slot else {
                test_report_error(diags, "a mutant could not be tried", m.span, None);
                finished = false;
                break;
            };
            if !apply_mutant_trial(&ctx, config.json, m, trial, output, diags, totals) {
                finished = false;
                break;
            }
        }
    }

    if finished {
        totals.mutants.ran = true;
        if config.json && !write_mutant_summary(output, &totals.mutants) {
            test_report_error(
                diags,
                "a mutant's record could not be written",
                Span::NONE,
                None,
            );
            totals.mutants.ran = false;
        }
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
