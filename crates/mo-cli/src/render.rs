//! Terminal-safe and NDJSON renderers for diagnostics, runtime defects, and test
//! inventory/results (`src/tools/render.c` parity).

use std::io::Write;

use mo_base::{
    DiagBag, RenderFormat, RenderWriter, SourceMap, TerminalEncoding, render_diagnostic_default,
    render_version_json,
};
use mo_bytecode::BytecodeFile;
use mo_def::limits::{MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_DEFECT_TEXT_SIZE};
use mo_test::render_gates_summary;
use mo_vm::{Defect, render_defect_raw};

use crate::cli::Command;
use crate::identity::MO_VERSION;

pub use mo_test::{
    MutantTotals, Observation, ObservationKind, TestOutcome, TestResult, TestTotals,
    UNICODE16_METRICS, detect_terminal_encoding, render_test_result, runtime_span, write_test_item,
    write_test_result,
};

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

/// Writes a standalone `mo run` runtime defect report to `errors`
/// (`src/tools/render.c:mo_write_defect`).
#[must_use]
pub fn write_defect(errors: &mut dyn Write, file: Option<&BytecodeFile>, defect: &Defect) -> bool {
    let encoding = detect_terminal_encoding();
    let raw_full = match file {
        Some(f) => render_defect_raw(f, defect),
        None => format!("{}\n", defect.reason),
    };
    let complete = raw_full.len() <= MO_LIMIT_DEFECT_TEXT_SIZE as usize;
    let raw_slice = if complete {
        raw_full.as_bytes()
    } else {
        let mut end = MO_LIMIT_DEFECT_TEXT_SIZE as usize;
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
            if counts.mutants.asked {
                render_gates_summary(&mut w, &counts.mutants, true);
            }
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
    if ok && !json && counted && counts.mutants.asked {
        let mut mw =
            RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
        render_gates_summary(&mut mw, &counts.mutants, false);
        ok = match mw.finish() {
            Ok(buf) => write_all_and_flush(output, &buf),
            Err(_) => false,
        };
    }
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
