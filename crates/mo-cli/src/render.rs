//! Terminal-safe and NDJSON renderers for diagnostics, runtime defects, and test
//! inventory/results (`src/tools/render.c` parity).

use std::env;
use std::io::Write;

use mo_base::{
    DiagBag, DisplayMetrics, Label, RenderFormat, RenderLimitError, RenderWriter, SourceMap, Span,
    TerminalEncoding, render_diagnostic_default, render_version_json,
};
use mo_bytecode::{BytecodeFile, NO_INDEX};
use mo_def::limits::{MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_DEFECT_TEXT_SIZE};
use mo_prim::unicode16::{display_width_bytes, next_grapheme_bytes};
use mo_vm::{Defect, render_defect_raw};

use crate::cli::Command;
use crate::identity::MO_VERSION;

/// Unicode 16.0.0 grapheme/display-width metrics matching `src/tools/render.c`.
pub const UNICODE16_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: next_grapheme_bytes,
    display_width: display_width_bytes,
};

/// Detects whether the active POSIX locale uses UTF-8 (`LC_ALL` > `LC_CTYPE` > `LANG`)
/// without calling `setlocale` or `unsafe`.
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

/// Maps a bytecode `(file_idx, start, end)` location back into a [`SourceMap`] [`Span`]
/// (`src/tools/run.c:mo_runtime_span`).
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

/// Input to [`render_test_result`].
pub struct TestResult<'a> {
    pub observation: &'a Observation,
    pub file: &'a BytecodeFile,
    pub outcome: TestOutcome,
    pub defect: Option<&'a Defect>,
    pub line: Span,
    pub microseconds: u64,
}

fn write_all_and_flush(stream: &mut dyn Write, data: &[u8]) -> bool {
    stream.write_all(data).is_ok() && stream.flush().is_ok()
}

/// Writes the leading NDJSON version record (`src/tools/render.c:mo_write_version`).
#[must_use]
pub fn write_version(output: &mut dyn Write, command: Command) -> bool {
    let Some(name) = command.json_name() else {
        return false;
    };
    let bytes = render_version_json(name, MO_VERSION);
    write_all_and_flush(output, &bytes)
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

/// Parses one length-prefixed `<label><len>:<bytes>,` part of lowering's equality failure protocol.
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
        // Never publish a partial/empty list after a variable-replay refusal.
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

    match result.outcome {
        TestOutcome::Passed => {}
        TestOutcome::Failed => {
            write_equality_failure(&mut w, result.defect, json);
            match obs.kind {
                ObservationKind::Test | ObservationKind::Promise => {
                    if result.line.file == u32::MAX {
                        w.mark_failed();
                    }
                    if json {
                        w.text(",\"line\":");
                        w.json_span(sources, result.line);
                    } else {
                        let msg = if obs.kind == ObservationKind::Test {
                            "this assertion was false"
                        } else {
                            "this promise condition was false"
                        };
                        let label = Label::with_text(result.line, msg);
                        w.text_label(sources, &label);
                    }
                }
                ObservationKind::Example => {
                    if !json {
                        let label = Label::with_text(obs.span, "this example was false");
                        w.text_label(sources, &label);
                    }
                }
                ObservationKind::Recovery => {}
            }
        }
        TestOutcome::Stopped => {
            write_stopped_result(&mut w, sources, result, json);
        }
    }

    if json {
        w.text("}\n");
    }
    w.finish()
}

/// Renders and writes one `mo test` result record (`src/tools/render.c:mo_write_test_result`).
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

/// Renders and writes a runtime defect to `errors`, escaping hostile terminal sequences
/// according to the active locale (`src/tools/render.c:mo_write_defect`).
#[must_use]
pub fn write_defect(errors: &mut dyn Write, file: Option<&BytecodeFile>, defect: &Defect) -> bool {
    let encoding = detect_terminal_encoding();
    let max_raw = MO_LIMIT_DEFECT_TEXT_SIZE as usize;
    let raw_full = match file {
        Some(bc) => render_defect_raw(bc, defect),
        None => {
            let mut s = String::new();
            s.push_str(&defect.reason);
            if let Some(limit_name) = defect.limit_name {
                s.push_str(&format!(" ({limit_name} = {})", defect.limit));
            }
            s.push('\n');
            if let Some(detail) = &defect.detail {
                s.push_str("  detail: ");
                s.push_str(detail);
                s.push('\n');
            }
            s
        }
    };
    let complete = raw_full.len() <= max_raw;
    let raw_slice = if complete {
        raw_full.as_bytes()
    } else {
        let mut end = max_raw;
        while end > 0 && !raw_full.is_char_boundary(end) {
            end -= 1;
        }
        &raw_full.as_bytes()[..end]
    };

    let mut w = RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
    w.text("bug: ");
    if raw_slice.is_empty() {
        w.safe_str(&defect.reason);
        w.text("\n");
    } else {
        w.terminal_bytes(raw_slice);
    }
    if !complete {
        w.text("  <report incomplete>\n");
    }
    match w.finish() {
        Ok(buf) => write_all_and_flush(errors, &buf),
        Err(_) => false,
    }
}

/// Sorts and writes diagnostics (and optional `mo test` totals) in text or NDJSON format,
/// returning the process exit status (`src/tools/render.c:mo_write_diagnostics`).
#[must_use]
pub fn write_diagnostics(
    output: &mut dyn Write,
    errors: &mut dyn Write,
    json: bool,
    command: Command,
    sources: &SourceMap,
    diags: &mut DiagBag,
    totals: Option<&TestTotals>,
) -> i32 {
    diags.sort();
    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };
    let format = if json {
        RenderFormat::Json
    } else {
        RenderFormat::Text
    };

    let mut written = 0u64;
    let mut dropped = u64::from(diags.dropped());
    let mut ok = true;

    for diag in diags.items() {
        if !ok {
            break;
        }
        match render_diagnostic_default(sources, diag, format, encoding, UNICODE16_METRICS) {
            Ok(rendered) => {
                let target: &mut dyn Write = if json { output } else { errors };
                ok = write_all_and_flush(target, &rendered);
                if ok {
                    written += 1;
                }
            }
            Err(_) => {
                dropped += 1;
            }
        }
    }

    let counts = totals.copied().unwrap_or_default();
    let mut status =
        if diags.has_any() || dropped != 0 || !ok || counts.failed != 0 || counts.stopped != 0 {
            1
        } else {
            0
        };
    if status == 0 {
        status = counts.status;
    }

    let mut w = RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
    if json {
        w.text("{\"record\":\"totals\",\"errors\":");
        w.u64_dec(written);
        w.text(",\"dropped\":");
        w.u64_dec(dropped);
        if command == Command::Test {
            w.text(",\"passed\":");
            w.u64_dec(counts.passed);
            w.text(",\"failed\":");
            w.u64_dec(counts.failed);
            w.text(",\"stopped\":");
            w.u64_dec(counts.stopped);
        }
        w.text(",\"status\":");
        w.u64_dec(u64::from(u32::try_from(status).unwrap_or(1)));
        w.text("}\n");
    } else if dropped != 0 {
        w.u64_dec(dropped);
        w.text(" diagnostics omitted: diagnostic storage or output limit\n");
    }

    if ok {
        match w.finish() {
            Ok(buf) if !buf.is_empty() => {
                let target: &mut dyn Write = if json { output } else { errors };
                ok = write_all_and_flush(target, &buf);
            }
            Ok(_) => {}
            Err(_) => {
                ok = false;
            }
        }
    }

    let counted = counts.passed != 0 || counts.failed != 0 || counts.stopped != 0;
    if ok && !json && counted {
        let mut tw =
            RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
        tw.u64_dec(counts.passed);
        tw.text(" passed, ");
        tw.u64_dec(counts.failed);
        tw.text(" failed, ");
        tw.u64_dec(counts.stopped);
        tw.text(" stopped\n");
        ok = match tw.finish() {
            Ok(buf) => write_all_and_flush(output, &buf),
            Err(_) => false,
        };
    }

    if !ok {
        let _ = write_all_and_flush(errors, b"mo: could not write diagnostics\n");
        return 1;
    }
    status
}
