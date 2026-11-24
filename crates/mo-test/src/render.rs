//! Terminal-safe and NDJSON renderers for `mo test` results (`src/tools/render.c` parity).

use std::env;
use std::io::Write;

use mo_base::{
    DisplayMetrics, Label, RenderLimitError, RenderWriter, SourceMap, Span, TerminalEncoding,
};
use mo_bytecode::{BytecodeFile, NO_INDEX, TypeKind};
use mo_check::{CheckedProgram, CheckedSpecKind};
use mo_def::limits::MO_CLI_DIAGNOSTIC_SIZE;
use mo_heap::print::value_print_bounded;
use mo_heap::saved::decode_value;
use mo_heap::{Heap, Value};
use mo_prim::unicode16::{display_width_bytes, next_grapheme_bytes};
use mo_vm::Defect;
use mo_vm::simulation::{SimulationOutcome, SimulationSent};

use crate::inputs::{InputExtreme, InputExtremeEnd};
use crate::recovery::{
    RecoveryCounts, RecoveryFailure, RecoveryOutcome, RecoveryResult, RecoverySequence,
    RecoveryStep, RecoveryStepKind, RecoverySupervisor,
};

/// Unicode 16.0.0 grapheme/display-width metrics matching `src/tools/render.c`.
pub const UNICODE16_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: next_grapheme_bytes,
    display_width: display_width_bytes,
};

/// Number of kill-kind buckets in [`MutantTotals`] (`promise`, `example`, `test`, `recovery`, `start`).
pub const GATE_KILL_KINDS: usize = 5;

/// Labels for the 5 mutant kill-kind buckets.
pub const GATE_KILL_NAMES: [&str; GATE_KILL_KINDS] =
    ["promise", "example", "test", "recovery", "start"];

/// Mutant gate totals (`struct mo_mutant_totals`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct MutantTotals {
    pub asked: bool,
    pub ran: bool,
    pub tried: u32,
    pub killed: [u32; GATE_KILL_KINDS],
    pub survived: u32,
    pub dropped: u32,
}

/// Summary counters accumulated while running `mo test` (`struct mo_test_totals`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TestTotals {
    pub passed: u64,
    pub failed: u64,
    pub stopped: u64,
    pub failed_examples: u64,
    pub failed_tests: u64,
    pub stopped_tests: u64,
    pub status: i32,
    pub mutants: MutantTotals,
}

/// Kind of observation reported by `mo test --list` and `mo test`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObservationKind {
    Test,
    Example,
    Promise,
    Recovery,
}

impl ObservationKind {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Test => "test",
            Self::Example => "example",
            Self::Promise => "promise",
            Self::Recovery => "recovery",
        }
    }
}

/// One selected test/example/promise/recovery item (`struct mo_observation`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Observation {
    pub name: String,
    pub kind: ObservationKind,
    pub span: Span,
    /// Index into `CheckedProgram::specs` for `test`/`example`/function `promise`.
    pub spec_index: Option<usize>,
    /// Index into `CheckedProgram::services` for service promise / recovery checks.
    pub service_index: Option<usize>,
    /// Index into `CheckedService::promises` for a service promise.
    pub service_promise_index: Option<usize>,
    /// Concrete instantiated promise name when monomorphizing a generic function's promise.
    pub instance_name: Option<String>,
    /// Index into `LoweredProgram::promises` when instantiated for a generic function.
    pub instance_index: Option<usize>,
}

/// Execution outcome for one test observation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TestOutcome {
    Passed,
    Failed,
    Stopped,
}

impl TestOutcome {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Passed => "passed",
            Self::Failed => "failed",
            Self::Stopped => "stopped",
        }
    }
}

/// Promise vacuity failure classification (`enum mo_vacuity_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum VacuityKind {
    #[default]
    None,
    Tautology,
    UnreachedImplies,
}

/// Details attached to a function-promise result (`struct mo_promise_result`).
pub struct PromiseResultReport<'a> {
    pub subject: u32,
    pub parameter_names: &'a [String],
    pub input: Option<&'a [Value]>,
    pub value: Option<&'a Value>,
    pub calls: u64,
    pub extreme: Option<InputExtreme>,
}

/// Details attached to a service recovery / service promise result (`struct mo_recovery_report`).
pub struct RecoveryReport<'a> {
    pub heap: &'a Heap,
    pub service: u32,
    pub counts: RecoveryCounts,
    pub sequence: Option<&'a RecoverySequence>,
    pub result: &'a RecoveryResult,
    pub replies: &'a [Option<(u32, Value)>],
}

/// Input to [`render_test_result`] (`struct mo_test_result`).
pub struct TestResult<'a> {
    pub observation: &'a Observation,
    pub file: &'a BytecodeFile,
    pub outcome: TestOutcome,
    pub vacuity: VacuityKind,
    pub defect: Option<&'a Defect>,
    pub line: Span,
    pub microseconds: u64,
    pub promise: Option<&'a PromiseResultReport<'a>>,
    pub recovery: Option<&'a RecoveryReport<'a>>,
}

/// Detects whether the active POSIX locale uses UTF-8 (`LC_ALL` > `LC_CTYPE` > `LANG`).
#[must_use]
pub fn detect_terminal_encoding() -> TerminalEncoding {
    let raw = env::var_os("LC_ALL")
        .filter(|v| !v.is_empty())
        .or_else(|| env::var_os("LC_CTYPE").filter(|v| !v.is_empty()))
        .or_else(|| env::var_os("LANG").filter(|v| !v.is_empty()));
    let Some(val) = raw else {
        return TerminalEncoding::Ascii;
    };
    let s = val.to_string_lossy();
    let without_mod = s.split('@').next().unwrap_or(&s);
    if let Some((_, codeset)) = without_mod.split_once('.') {
        if codeset.eq_ignore_ascii_case("utf-8") || codeset.eq_ignore_ascii_case("utf8") {
            return TerminalEncoding::Utf8;
        }
    } else if without_mod.eq_ignore_ascii_case("utf-8") || without_mod.eq_ignore_ascii_case("utf8")
    {
        return TerminalEncoding::Utf8;
    }
    TerminalEncoding::Ascii
}

/// Maps a bytecode `(file_idx, start, end)` location back into a [`SourceMap`] [`Span`].
#[must_use]
pub fn runtime_span(
    file: &BytecodeFile,
    sources: &SourceMap,
    path_idx: u32,
    start: u32,
    end: u32,
) -> Span {
    if path_idx != NO_INDEX
        && let Some(path_bytes) = file.files.get(path_idx as usize)
    {
        for i in 0..sources.len() {
            if let Some(src) = sources.get(i)
                && src.path_bytes() == path_bytes.as_slice()
            {
                return Span::new(i, start, end);
            }
        }
    }
    Span::new(u32::MAX, start, end)
}

/// Formats `Module.fn` or `Module.fn@Receiver` matching `src/tools/tests.c:mo_append_function_name`.
#[must_use]
pub fn format_function_name(program: &CheckedProgram, func_idx: usize) -> String {
    let func = &program.functions[func_idx];
    let sym = program.interfaces.symbol(func.symbol);
    let mod_name = &program.modules.module(sym.module).name;
    let mut out = format!("{mod_name}.{}", sym.name);
    let scope = program.interfaces.scope(sym.module);
    if let Some(receiver_idx) = sym.receiver
        && let Some(group) = scope.find_symbol(sym.name.as_bytes())
        && group.overload_count > 1
    {
        out.push('@');
        out.push_str(&scope.symbols[receiver_idx.index()].name);
    }
    out
}

/// Renders mutant gate summary into `w` (`src/tools/gates.c:mo_render_gates`).
pub fn render_gates_summary(w: &mut RenderWriter, mutants: &MutantTotals, json: bool) {
    let killed: u32 = mutants.killed.iter().sum();
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
        w.u64_dec(u64::from(killed));
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

fn write_all_and_flush(stream: &mut dyn Write, data: &[u8]) -> bool {
    stream.write_all(data).is_ok() && stream.flush().is_ok()
}

/// Writes one `mo test --list` entry in text or NDJSON (`src/tools/render.c:mo_write_test`).
#[must_use]
pub fn write_test_item(
    output: &mut dyn Write,
    json: bool,
    sources: &SourceMap,
    observation: &Observation,
) -> bool {
    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };
    let mut w = RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
    if json {
        w.text("{\"record\":\"test\",\"name\":");
        w.json_str(&observation.name);
        w.text(",\"kind\":");
        w.json_str(observation.kind.as_str());
        w.text(",\"span\":");
        w.json_span(sources, observation.span);
        w.text("}\n");
    } else {
        w.safe_str(&observation.name);
        w.text("\n");
    }
    match w.finish() {
        Ok(buf) => write_all_and_flush(output, &buf),
        Err(_) => false,
    }
}

fn parse_failure_part<'a>(input: &'a [u8], label: &[u8]) -> Option<(&'a [u8], &'a [u8])> {
    let rest = input.strip_prefix(label)?;
    let mut length = 0usize;
    let mut digits = 0usize;
    let mut pos = 0usize;
    while pos < rest.len() && rest[pos].is_ascii_digit() {
        let digit = usize::from(rest[pos] - b'0');
        if length > (usize::MAX - digit) / 10 || (digits != 0 && length == 0) {
            return None;
        }
        length = length * 10 + digit;
        pos += 1;
        digits += 1;
    }
    if digits == 0 || pos >= rest.len() || rest[pos] != b':' {
        return None;
    }
    let after_colon = &rest[pos + 1..];
    if length >= after_colon.len() {
        return None;
    }
    let payload = &after_colon[..length];
    let after_payload = &after_colon[length..];
    let tail = after_payload.strip_prefix(b",")?;
    Some((payload, tail))
}

fn write_equality_failure(w: &mut RenderWriter, defect: Option<&Defect>, json: bool) {
    let Some(defect) = defect else {
        return;
    };
    let Some(detail) = defect.detail.as_deref() else {
        return;
    };
    let raw = detail.as_bytes();
    if !raw.starts_with(b"left ") {
        return;
    }
    let Some((left, rest)) = parse_failure_part(raw, b"left ") else {
        w.mark_failed();
        return;
    };
    let Some((right, tail)) = parse_failure_part(rest, b"right ") else {
        w.mark_failed();
        return;
    };
    if !tail.is_empty() {
        w.mark_failed();
        return;
    }
    if json {
        w.text(",\"left\":");
        w.json_bytes(left);
        w.text(",\"right\":");
        w.json_bytes(right);
    } else {
        w.text("  left: ");
        w.safe_bytes(left);
        w.text("\n  right: ");
        w.safe_bytes(right);
        w.text("\n");
    }
}

fn value_text(
    w: &mut RenderWriter,
    file: &BytecodeFile,
    ty: u32,
    val: &Value,
    limit: usize,
    json: bool,
) {
    let Some(literal) = value_print_bounded(&file.types, ty, val, true, limit) else {
        w.mark_failed();
        return;
    };
    if json {
        w.json_str(&literal);
    } else {
        w.safe_str(&literal);
    }
}

fn extreme_description(extreme: InputExtreme) -> &'static str {
    let smallest = extreme.end == InputExtremeEnd::Smallest;
    match extreme.ty {
        TypeKind::Int => {
            if smallest {
                "Int's smallest value"
            } else {
                "Int's largest value"
            }
        }
        TypeKind::Decimal => {
            if smallest {
                "Decimal's smallest value"
            } else {
                "Decimal's largest value"
            }
        }
        TypeKind::Time => {
            if smallest {
                "Time's smallest value"
            } else {
                "Time's largest value"
            }
        }
        TypeKind::Duration => {
            if smallest {
                "Duration's smallest value"
            } else {
                "Duration's largest value"
            }
        }
        _ => unreachable!("unexpected extreme type"),
    }
}

fn vacuity_label(vacuity: VacuityKind) -> &'static str {
    match vacuity {
        VacuityKind::Tautology => "this promise condition is always true",
        VacuityKind::UnreachedImplies => {
            "this promise condition was vacuous: the left side of implies was never true"
        }
        VacuityKind::None => unreachable!(),
    }
}

fn write_promise_result(w: &mut RenderWriter, result: &TestResult<'_>, json: bool) {
    let Some(promise) = result.promise else {
        return;
    };
    if result.outcome == TestOutcome::Passed {
        return;
    }
    let subject = &result.file.functions[promise.subject as usize];
    if let Some(input) = promise.input {
        if json {
            w.text(",\"input\":[");
        }
        for (i, val) in input.iter().enumerate() {
            if !w.is_ok() {
                break;
            }
            let name = promise.parameter_names.get(i).map_or("", String::as_str);
            if json {
                if i != 0 {
                    w.text(",");
                }
                w.text("{\"name\":");
                w.json_str(name);
                w.text(",\"value\":");
            } else {
                w.text("  input ");
                w.safe_str(name);
                w.text(" = ");
            }
            value_text(w, result.file, subject.parameters[i].ty, val, 4096, json);
            w.text(if json { "}" } else { "\n" });
        }
        if json {
            w.text("]");
        }
    }
    if result.outcome == TestOutcome::Failed {
        if let Some(val) = promise.value {
            w.text(if json { ",\"result\":" } else { "  result: " });
            value_text(w, result.file, subject.result_type, val, 4096, json);
            if !json {
                w.text("\n");
            }
        }
        w.text(if json { ",\"calls\":" } else { "  calls: " });
        w.u64_dec(promise.calls);
        if !json {
            w.text("\n");
        }
        if json && result.vacuity != VacuityKind::None {
            w.text(",\"vacuous\":true");
        }
    }
    if let Some(extreme) = promise.extreme {
        let name = promise
            .parameter_names
            .get(extreme.parameter)
            .map_or("", String::as_str);
        let desc = extreme_description(extreme);
        if json {
            w.text(",\"extreme\":{\"name\":");
            w.json_str(name);
            w.text(",\"description\":");
            w.json_str(desc);
            w.text("}");
        } else {
            w.text("  ");
            w.safe_str(name);
            w.text(" is ");
            w.safe_str(desc);
            w.text(
                "\n  fix: return a failure for such inputs, or take a type whose module \
                 keeps its values in range (reference 12.2).\n",
            );
        }
    }
}

fn write_stopped_result(
    w: &mut RenderWriter,
    sources: &SourceMap,
    result: &TestResult<'_>,
    json: bool,
) {
    let Some(defect) = result.defect else {
        w.mark_failed();
        return;
    };
    let top_span = defect
        .frames
        .first()
        .and_then(|f| f.span)
        .map_or(Span::NONE, |(f, s, e)| {
            runtime_span(result.file, sources, f, s, e)
        });
    if json {
        w.text(",\"defect\":{\"reason\":");
        w.json_str(&defect.reason);
        if let Some(limit_name) = defect.limit_name {
            w.text(",\"limit\":{\"name\":");
            w.json_str(limit_name);
            w.text(",\"value\":\"");
            w.u64_dec(defect.limit as u64);
            w.text("\"}");
        }
        if let Some(detail) = &defect.detail {
            w.text(",\"detail\":");
            w.json_str(detail);
        }
        w.text(",\"span\":");
        w.json_span(sources, top_span);
        w.text(",\"stack\":[");
    } else {
        w.text("  bug: ");
        w.safe_str(&defect.reason);
        if let Some(limit_name) = defect.limit_name {
            w.text(" (");
            w.safe_str(limit_name);
            w.text(" = ");
            w.u64_dec(defect.limit as u64);
            w.text(")");
        }
        w.text("\n");
        if let Some(detail) = &defect.detail {
            w.text("  detail: ");
            w.safe_str(detail);
            w.text("\n");
        }
    }

    for (idx, frame) in defect.frames.iter().enumerate() {
        if !w.is_ok() {
            break;
        }
        let fn_name = result.file.function_name(frame.function);
        let fspan = frame.span.map_or(Span::NONE, |(f, s, e)| {
            runtime_span(result.file, sources, f, s, e)
        });
        if json {
            if idx != 0 {
                w.text(",");
            }
            w.text("{\"function\":");
            w.json_str(fn_name);
            w.text(",\"span\":");
            w.json_span(sources, fspan);
            w.text("}");
        } else {
            w.text("  at ");
            w.safe_str(fn_name);
            w.text("  ");
            w.write_location(sources, fspan);
            w.text("\n");
        }
    }

    if json {
        w.text("],\"variables\":[");
    }
    if defect.variables_unavailable {
        w.mark_failed();
    } else {
        for (idx, var) in defect.variables.iter().enumerate() {
            if !w.is_ok() {
                break;
            }
            let shown_value = if var.truncated {
                "<value unavailable>"
            } else {
                var.value.as_str()
            };
            if json {
                if idx != 0 {
                    w.text(",");
                }
                w.text("{\"name\":");
                w.json_str(&var.name);
                w.text(",\"value\":");
                w.json_str(shown_value);
                w.text("}");
            } else {
                w.text("  ");
                w.safe_str(&var.name);
                w.text(" = ");
                w.safe_str(shown_value);
                w.text("\n");
            }
        }
    }
    if json {
        w.text("]}");
    }
}

fn duration_text(w: &mut RenderWriter, microseconds: i64) {
    debug_assert!(microseconds >= 0);
    let us = microseconds as u64;
    w.u64_dec(us / 1_000_000);
    let mut frac = us % 1_000_000;
    if frac != 0 {
        let mut digits = [0u8; 6];
        for i in (0..6).rev() {
            digits[i] = b'0' + (frac % 10) as u8;
            frac /= 10;
        }
        let mut len = 6usize;
        while digits[len - 1] == b'0' {
            len -= 1;
        }
        w.text(".");
        w.bytes(&digits[..len]);
    }
    w.text("s");
}

fn recovery_failure_text(w: &mut RenderWriter, failure: &RecoveryFailure, json: bool) {
    let mut reason = String::from(failure.status.text());
    if !failure.reason.is_empty() {
        reason.push_str(": ");
        reason.push_str(&failure.reason);
    }
    if let Some(ref detail) = failure.detail
        && !detail.is_empty()
    {
        reason.push_str(": ");
        reason.push_str(detail);
    }
    if json {
        w.json_str(&reason);
    } else {
        w.safe_str(&reason);
    }
}

fn write_recovery_send(
    w: &mut RenderWriter,
    result: &TestResult<'_>,
    report: &RecoveryReport<'_>,
    step: &RecoveryStep,
    index: usize,
    json: bool,
) {
    let tool = &result.file.services[report.service as usize].tools[step.tool as usize];
    let name = result.file.string(tool.name);
    if json {
        w.text("\"send\",\"tool\":");
        w.json_str(name);
        w.text(",\"arguments\":[");
    } else {
        w.safe_str(name);
        w.text("(");
    }
    for (a, &arg_ty) in tool.argument_types.iter().enumerate() {
        if a != 0 {
            w.text(if json { "," } else { ", " });
        }
        value_text(
            w,
            result.file,
            arg_ty,
            &step.arguments[a],
            MO_CLI_DIAGNOSTIC_SIZE as usize,
            json,
        );
    }
    let (outcome, has_reply) = match step.sent {
        SimulationSent::Replied | SimulationSent::Rejected => ("replied", true),
        SimulationSent::ToolFailed => ("ToolFailed", false),
        SimulationSent::Down => ("ServiceDown", false),
        SimulationSent::Failed => ("failed", false),
        SimulationSent::Argument | SimulationSent::TooLarge | SimulationSent::Refused => {
            (if json { "refused" } else { "not admitted" }, false)
        }
        SimulationSent::ReplyCopy => (
            if json {
                "reply_refused"
            } else {
                "reply not copied"
            },
            false,
        ),
        SimulationSent::Stopped => unreachable!(),
    };
    if json {
        w.text("],\"outcome\":");
        w.json_str(outcome);
        if has_reply && let Some((rty, rval)) = report.replies.get(index).and_then(Option::as_ref) {
            w.text(",\"reply\":");
            value_text(
                w,
                result.file,
                *rty,
                rval,
                MO_CLI_DIAGNOSTIC_SIZE as usize,
                true,
            );
        }
        w.text(if step.updated {
            ",\"committed\":true"
        } else {
            ",\"committed\":false"
        });
    } else {
        w.text(")  ");
        w.text(outcome);
        if has_reply && let Some((rty, rval)) = report.replies.get(index).and_then(Option::as_ref) {
            w.text(" ");
            value_text(
                w,
                result.file,
                *rty,
                rval,
                MO_CLI_DIAGNOSTIC_SIZE as usize,
                false,
            );
        }
        if step.sent == SimulationSent::Failed {
            w.text(": ");
            w.safe_str(step.status.text());
        }
        if step.updated {
            w.text(", committed");
        }
    }
    if step.supervisor != RecoverySupervisor::None {
        let stopped = step.supervisor == RecoverySupervisor::Stopped;
        w.text(match (json, stopped) {
            (true, true) => ",\"supervisor\":\"stopped\"",
            (true, false) => ",\"supervisor\":\"restarted\"",
            (false, true) => ", service stopped",
            (false, false) => ", service restarted",
        });
    }
}

fn write_recovery_advance(w: &mut RenderWriter, step: &RecoveryStep, json: bool) {
    w.text(if json {
        "\"advance\",\"by\":\""
    } else {
        "advance "
    });
    duration_text(w, step.duration);
    let refused = match step.outcome {
        SimulationOutcome::Ticks => {
            Some("more firings fell due than MO_LIMIT_SIMULATION_TICKS; none ran")
        }
        SimulationOutcome::Duration => Some("the clock would pass year 9999; nothing ran"),
        _ => None,
    };
    if json {
        w.text("\",\"firings\":");
        w.u64_dec(step.firings);
        if step.failed_firings != 0 {
            w.text(",\"failed_firings\":");
            w.u64_dec(u64::from(step.failed_firings));
        }
        if let Some(msg) = refused {
            w.text(",\"refused\":");
            w.json_str(msg);
        }
        return;
    }
    w.text("  ");
    if let Some(msg) = refused {
        w.text(msg);
        return;
    }
    w.u64_dec(step.firings);
    w.text(if step.firings == 1 {
        " timer firing"
    } else {
        " timer firings"
    });
    if step.failed_firings != 0 {
        w.text(", ");
        w.u64_dec(u64::from(step.failed_firings));
        w.text(" failed");
    }
}

fn decode_state_literal(
    file: &BytecodeFile,
    report: &RecoveryReport<'_>,
    saved: &[u8],
) -> Option<String> {
    let state_ty = file.services[report.service as usize].state_type;
    let val = decode_value(report.heap, &file.types, state_ty, saved).ok()?;
    value_print_bounded(
        &file.types,
        state_ty,
        &val,
        true,
        MO_CLI_DIAGNOSTIC_SIZE as usize,
    )
}

fn write_recovery_failed(
    w: &mut RenderWriter,
    sources: &SourceMap,
    result: &TestResult<'_>,
    report: &RecoveryReport<'_>,
    sequence: &RecoverySequence,
    json: bool,
) {
    let run = report.result;
    let promise = run.outcome == RecoveryOutcome::Check;
    if json {
        w.text(",\"steps\":[");
    }
    for (i, step) in sequence.steps.iter().enumerate() {
        if !w.is_ok() {
            break;
        }
        if json {
            w.text(if i == 0 {
                "{\"action\":"
            } else {
                ",{\"action\":"
            });
        } else {
            w.text("  ");
            w.u64_dec((i + 1) as u64);
            w.text(". ");
        }
        match step.kind {
            RecoveryStepKind::Send => write_recovery_send(w, result, report, step, i, json),
            RecoveryStepKind::Advance => write_recovery_advance(w, step, json),
            RecoveryStepKind::Crash => w.text(if json { "\"crash\"" } else { "crash" }),
            RecoveryStepKind::Restart => {
                if promise {
                    w.text(if json { "\"restart\"" } else { "restart" });
                } else if step.outcome == SimulationOutcome::Down {
                    w.text(if json {
                        "\"restart\",\"recovered\":\"refused\""
                    } else {
                        "restart  recovery refused the store: "
                    });
                    if !json {
                        recovery_failure_text(w, &run.failure, false);
                    }
                } else if i as u32 == run.step {
                    w.text(if json {
                        "\"restart\",\"recovered\":\"different\""
                    } else {
                        "restart  recovered a different state"
                    });
                } else {
                    w.text(if json {
                        "\"restart\",\"recovered\":\"same\""
                    } else {
                        "restart  recovered the same state"
                    });
                }
            }
        }
        if promise && i as u32 == run.step {
            w.text(if json { ",\"broke\":" } else { "  broke " });
            if json {
                w.json_str(&result.observation.name);
            } else {
                w.safe_str(&result.observation.name);
            }
        }
        w.text(if json { "}" } else { "\n" });
    }

    let before = decode_state_literal(result.file, report, &run.before);
    let mismatch = run.outcome == RecoveryOutcome::Mismatch;
    let after = if mismatch || promise {
        decode_state_literal(result.file, report, &run.after)
    } else {
        Some(String::new())
    };
    let (Some(before_text), Some(after_text)) = (before, after) else {
        w.mark_failed();
        return;
    };
    let first = if promise && sequence.steps[run.step as usize].kind != RecoveryStepKind::Restart {
        run.step + 1
    } else {
        run.step
    };
    if json {
        w.text("],\"before\":");
        w.json_str(&before_text);
        if mismatch || promise {
            w.text(",\"after\":");
            w.json_str(&after_text);
        } else {
            w.text(",\"refusal\":");
            recovery_failure_text(w, &run.failure, true);
        }
    } else {
        w.text("  state before step ");
        w.u64_dec(u64::from(first));
        w.text(": ");
        w.safe_str(&before_text);
        w.text("\n");
        if mismatch || promise {
            w.text("  state after step ");
            w.u64_dec(u64::from(run.step + 1));
            w.text(": ");
            w.safe_str(&after_text);
            w.text("\n");
        }
    }
    if mismatch && before_text == after_text {
        let mut at = 0usize;
        while at < run.before.len() && at < run.after.len() && run.before[at] == run.after[at] {
            at += 1;
        }
        w.text(if json {
            ",\"differ_at\":"
        } else {
            "  the saved encodings first differ at byte "
        });
        w.u64_dec(at as u64);
        if !json {
            w.text("\n");
        }
    }
    if promise && result.outcome == TestOutcome::Stopped {
        write_stopped_result(w, sources, result, json);
    } else if promise && json {
        w.text(",\"line\":");
        w.json_span(sources, result.line);
    } else if promise {
        w.text_label(
            sources,
            &Label::with_text(result.line, "this promise condition was false"),
        );
    }
}

fn write_recovery_stopped(
    w: &mut RenderWriter,
    sources: &SourceMap,
    result: &TestResult<'_>,
    report: &RecoveryReport<'_>,
    json: bool,
) {
    let failure = &report.result.failure;
    let file = result.file;
    let service = &file.services[report.service as usize];
    let reason = if failure.reason.is_empty() {
        failure.status.text()
    } else {
        failure.reason.as_str()
    };
    let span = failure
        .span
        .map_or(Span::NONE, |(f, s, e)| runtime_span(file, sources, f, s, e));
    if json {
        w.text(",\"defect\":{\"reason\":");
        w.json_str(reason);
        if let Some(ref detail) = failure.detail
            && !detail.is_empty()
        {
            w.text(",\"detail\":");
            w.json_str(detail);
        }
        w.text(",\"span\":");
        w.json_span(sources, span);
        w.text(",\"stack\":[],\"variables\":[]}");
        return;
    }
    w.text("  bug: ");
    w.safe_str(reason);
    w.text("\n");
    if let Some(ref detail) = failure.detail
        && !detail.is_empty()
    {
        w.text("  detail: ");
        w.safe_str(detail);
        w.text("\n");
    }
    let timer = service
        .timers
        .iter()
        .position(|t| t.interval == failure.function);
    if failure.function == service.start {
        w.text("  in the start value");
    } else if let Some(ti) = timer {
        let name = file.function_name(service.timers[ti].function);
        w.text("  in the interval of timer ");
        w.safe_str(name);
    } else {
        return;
    }
    if failure.span.is_some() {
        w.text("  ");
        w.write_location(sources, span);
    }
    w.text("\n");
}

fn write_recovery_result(
    w: &mut RenderWriter,
    sources: &SourceMap,
    result: &TestResult<'_>,
    report: &RecoveryReport<'_>,
    json: bool,
) {
    if json {
        w.text(",\"sequences\":");
        w.u64_dec(report.counts.sequences);
        if result.observation.service_promise_index.is_none() {
            w.text(",\"restarts\":");
            w.u64_dec(report.counts.restarts);
        }
    }
    if result.outcome == TestOutcome::Passed {
        return;
    }
    let Some(sequence) = report.sequence else {
        if result.vacuity != VacuityKind::None {
            if json {
                w.text(",\"vacuous\":true,\"line\":");
                w.json_span(sources, result.line);
            } else {
                w.text("  sequences: ");
                w.u64_dec(report.counts.sequences);
                w.text("\n");
                w.text_label(
                    sources,
                    &Label::with_text(result.line, vacuity_label(result.vacuity)),
                );
            }
            return;
        }
        write_recovery_stopped(w, sources, result, report, json);
        return;
    };
    if !json {
        w.text("  sequences: ");
        w.u64_dec(report.counts.sequences);
        w.text("\n");
    }
    write_recovery_failed(w, sources, result, report, sequence, json);
}

/// Renders one `mo test` result record within `MO_CLI_DIAGNOSTIC_SIZE`
/// (`src/tools/render.c:mo_render_test_result`).
pub fn render_test_result(
    sources: &SourceMap,
    result: &TestResult<'_>,
    json: bool,
    encoding: TerminalEncoding,
) -> Result<Vec<u8>, RenderLimitError> {
    let mut w = RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
    let obs = result.observation;
    let outcome = result.outcome.as_str();
    if json {
        w.text("{\"record\":\"result\",\"name\":");
        w.json_str(&obs.name);
        w.text(",\"kind\":");
        w.json_str(obs.kind.as_str());
        w.text(",\"span\":");
        w.json_span(sources, obs.span);
        w.text(",\"outcome\":");
        w.json_str(outcome);
        w.text(",\"microseconds\":");
        w.u64_dec(result.microseconds);
    } else {
        w.text(outcome);
        w.text(" ");
        w.safe_str(&obs.name);
        w.text("  ");
        w.write_location(sources, obs.span);
        w.text("\n");
    }

    if let Some(ref inst) = obs.instance_name {
        w.text(if json {
            ",\"instance\":"
        } else {
            "  instance: "
        });
        if json {
            w.json_str(inst);
        } else {
            w.safe_str(inst);
            w.text("\n");
        }
    }

    if result.promise.is_some() {
        write_promise_result(&mut w, result, json);
    }

    if let Some(report) = result.recovery {
        write_recovery_result(&mut w, sources, result, report, json);
    } else if result.outcome == TestOutcome::Failed {
        write_equality_failure(&mut w, result.defect, json);
        if matches!(obs.kind, ObservationKind::Test | ObservationKind::Promise) {
            if result.line.file == u32::MAX {
                w.mark_failed();
            }
            if json {
                w.text(",\"line\":");
                w.json_span(sources, result.line);
            } else {
                let message = match obs.kind {
                    ObservationKind::Test => "this assertion was false",
                    ObservationKind::Promise => {
                        if result.vacuity != VacuityKind::None {
                            vacuity_label(result.vacuity)
                        } else {
                            "this promise condition was false"
                        }
                    }
                    _ => unreachable!(),
                };
                w.text_label(sources, &Label::with_text(result.line, message));
            }
        } else if !json {
            w.text_label(
                sources,
                &Label::with_text(obs.span, "this example was false"),
            );
        }
    } else if result.outcome == TestOutcome::Stopped {
        write_stopped_result(&mut w, sources, result, json);
    }

    if json {
        w.text("}\n");
    }
    w.finish()
}

/// Renders and writes one `mo test` result record to `output`.
#[must_use]
pub fn write_test_result(
    output: &mut dyn Write,
    json: bool,
    sources: &SourceMap,
    result: &TestResult<'_>,
) -> bool {
    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };
    match render_test_result(sources, result, json, encoding) {
        Ok(buf) => write_all_and_flush(output, &buf),
        Err(_) => false,
    }
}

/// Helper to convert [`CheckedSpecKind`] to [`ObservationKind`].
#[must_use]
pub fn spec_observation_kind(kind: CheckedSpecKind) -> ObservationKind {
    match kind {
        CheckedSpecKind::Test => ObservationKind::Test,
        CheckedSpecKind::Example => ObservationKind::Example,
        CheckedSpecKind::Promise => ObservationKind::Promise,
        CheckedSpecKind::Anchor => unreachable!("anchors are not observations"),
    }
}
