#![forbid(unsafe_code)]

//! Thin Phase 2 front-end CLI driver (`mo-fe`) for Mo Core checking/printing,
//! surface parsing/formatting, and fast batch differential verification against
//! C `mo`, Bison `mo-grammar`, and Lean `mocore`.

use std::env;
use std::fs::{self, File};
use std::io::{self, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, Label, NO_FILE, RenderFormat, SourceError, SourceMap,
    Span, TerminalEncoding, render_diagnostic_default, render_totals_json, render_version_json,
};
use mo_bytecode::{
    AnchorExpectation, BigIntSign, BytecodeFile, ConstantValue, FunctionKind, NO_INDEX, TypeKind,
    disassemble as disassemble_bytecode, load as load_bytecode, write as write_bytecode,
};
use mo_check::{
    CheckStage, CheckedProgram, CheckedSpec, CheckedSpecKind, LibraryMode, LoadOptions, ModuleId,
    run_stage,
};
use mo_codegen::{compile as compile_core, compile_services as compile_core_services};
use mo_core::{
    CoreDocument, CoreTextKind, check_program, check_program_services, detect_kind, parse_document,
    parse_program, parse_run, print_document, print_program,
};
use mo_def::limits::{
    MO_CLI_ANCHOR_INSTRUCTIONS, MO_LIMIT_CORE_TEXT_SIZE, MO_LIMIT_DIAGNOSTICS, MO_LIMIT_FILE_SIZE,
    MO_LIMIT_PATH_SIZE,
};
use mo_def::{ErrorCode, MO_VERSION};
use mo_heap::print::value_print;
use mo_host::HostContext;
use mo_lower::{lower_services, lower_test_services};
use mo_prim::unicode16::{display_width_bytes, next_grapheme_bytes};
use mo_syntax::{FormatOptions, format_module, parse_fragment, parse_module, read_header};
use mo_vm::{Image, Limits, Outcome, Process, render_defect};

const UNICODE16_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: next_grapheme_bytes,
    display_width: display_width_bytes,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CheckKind {
    Auto,
    Program,
    Run,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PrintKind {
    Auto,
    Exact(CoreTextKind),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ParseMode {
    Module,
    Fragment,
    Header,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FormatOutMode {
    Default,
    Stdout,
    Check,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum EmitMode {
    None,
    Core,
    Bytecode,
}

fn print_usage(mut w: impl Write) -> io::Result<()> {
    writeln!(w, "usage:")?;
    writeln!(
        w,
        "  mo-fe core-check [--json] [--kind auto|program|run] [FILE.core|-]"
    )?;
    writeln!(
        w,
        "  mo-fe core-print [--json] [--kind auto|<kind>] [FILE.core|-]"
    )?;
    writeln!(
        w,
        "  mo-fe core-check-batch [--kind run|program|auto] FILE..."
    )?;
    writeln!(
        w,
        "  mo-fe core-print-batch [--kind run|program|auto|<kind>] FILE..."
    )?;
    writeln!(
        w,
        "  mo-fe codegen-core [--kind auto|program|run] [--disassemble] FILE.core [-o OUT.bc]"
    )?;
    writeln!(
        w,
        "  mo-fe codegen-core-batch [--kind run|program|auto] [--out-dir DIR] [--] FILE..."
    )?;
    writeln!(w, "  mo-fe roundtrip-bc [--check] FILE.bc")?;
    writeln!(
        w,
        "  mo-fe parse [--fragment|--header] [--json] [FILE.mo|-]"
    )?;
    writeln!(w, "  mo-fe format [--stdout|--check] [--json] [FILE.mo|-]")?;
    writeln!(w, "  mo-fe parse-batch [--fragment|--header] [--] FILE...")?;
    writeln!(w, "  mo-fe format-batch [--fragment] [--] FILE...")?;
    writeln!(
        w,
        "  mo-fe check [--stage=load|modules|signatures|full] [--emit=core|bytecode] [--emit-tests] [--library DIR|--lib DIR|--no-library] [--json] FILE.mo"
    )?;
    writeln!(
        w,
        "  mo-fe check-batch [--stage=load|modules|signatures|full] [--library DIR|--lib DIR|--no-library] [--] FILE..."
    )?;
    writeln!(
        w,
        "  mo-fe lower [--tests] [--library DIR|--lib DIR|--no-library] [--json] FILE.mo"
    )?;
    writeln!(
        w,
        "  mo-fe lower-batch [--tests] [--library DIR|--lib DIR|--no-library] [--] FILE..."
    )?;
    writeln!(
        w,
        "  mo-fe emit [--tests] [--library DIR|--lib DIR|--no-library] [--json] FILE.mo -o OUT.bc"
    )?;
    writeln!(w, "  mo-fe --core FILE.core")?;
    writeln!(w, "  mo-fe --core-test FILE.core TEST")
}

fn parse_check_kind(raw: &str) -> Result<CheckKind, String> {
    match raw {
        "auto" => Ok(CheckKind::Auto),
        "program" => Ok(CheckKind::Program),
        "run" => Ok(CheckKind::Run),
        other => Err(format!(
            "invalid --kind {other:?}: expected auto, program, or run"
        )),
    }
}

fn parse_print_kind(raw: &str) -> Result<PrintKind, String> {
    if raw == "auto" {
        return Ok(PrintKind::Auto);
    }
    CoreTextKind::from_cli_name(raw)
        .map(PrintKind::Exact)
        .ok_or_else(|| format!("invalid --kind {raw:?}"))
}

fn read_bounded(mut reader: impl Read, limit: u64) -> io::Result<Vec<u8>> {
    let mut buf = Vec::new();
    let max_read = limit.saturating_add(1);
    reader.by_ref().take(max_read).read_to_end(&mut buf)?;
    Ok(buf)
}

fn read_core_input(path: Option<&str>) -> io::Result<(String, Vec<u8>)> {
    match path {
        None | Some("-") => {
            let bytes = read_bounded(io::stdin().lock(), MO_LIMIT_CORE_TEXT_SIZE)?;
            Ok(("<stdin>".to_string(), bytes))
        }
        Some(p) => {
            let file = File::open(Path::new(p))?;
            let bytes = read_bounded(file, MO_LIMIT_CORE_TEXT_SIZE)?;
            Ok((p.to_string(), bytes))
        }
    }
}

fn render_diags(
    diags: &DiagBag,
    sources: &SourceMap,
    patch_file: Option<(u32, usize)>,
    format: RenderFormat,
    mut err: impl Write,
) -> io::Result<()> {
    for orig in diags.items() {
        let mut diag = orig.clone();
        if let Some((file_id, byte_len)) = patch_file
            && diag.primary.span.file == NO_FILE
            && (diag.primary.span.end as usize) <= byte_len
        {
            diag.primary.span.file = file_id;
        }
        match render_diagnostic_default(
            sources,
            &diag,
            format,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        ) {
            Ok(rendered) => err.write_all(&rendered)?,
            Err(_) => {
                writeln!(
                    err,
                    "error E{:04}: {}",
                    diag.code.number(),
                    diag.message.as_str_lossy()
                )?;
            }
        }
    }
    Ok(())
}

fn render_sorted_diags(
    diags: &mut DiagBag,
    sources: &SourceMap,
    format: RenderFormat,
    format_dropped_style: bool,
    mut err: impl Write,
) -> io::Result<bool> {
    diags.sort();
    let mut shown = 0u64;
    let mut dropped = u64::from(diags.dropped());
    for diag in diags.items() {
        if shown == MO_LIMIT_DIAGNOSTICS {
            dropped += 1;
            continue;
        }
        match render_diagnostic_default(
            sources,
            diag,
            format,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        ) {
            Ok(rendered) => {
                err.write_all(&rendered)?;
                shown += 1;
            }
            Err(_) => {
                dropped += 1;
            }
        }
    }
    if dropped != 0 && format == RenderFormat::Text {
        if format_dropped_style {
            writeln!(err, "mo: {dropped} diagnostics could not be shown")?;
        } else {
            writeln!(
                err,
                "{dropped} diagnostics omitted: diagnostic storage or output limit"
            )?;
        }
    }
    Ok(dropped != 0)
}

fn push_format_problem(diags: &mut DiagBag, file: u32, code: ErrorCode, path: &str, detail: &str) {
    let limit = MO_LIMIT_PATH_SIZE as usize;
    let mut message = String::with_capacity(detail.len() + 2 + path.len().min(limit) + 3);
    message.push_str(detail);
    message.push_str(": ");
    if path.len() > limit {
        // Truncate safely on byte boundary matching C `snprintf("%.*s...")`.
        let mut cut = limit;
        while cut > 0 && !path.is_char_boundary(cut) {
            cut -= 1;
        }
        message.push_str(&path[..cut]);
        message.push_str("...");
    } else {
        message.push_str(path);
    }
    let diag = Diagnostic::new(code, Span::new(file, 0, 0), message);
    let _ = diags.push(diag);
}

fn resolve_check_kind(kind: CheckKind, bytes: &[u8]) -> CoreTextKind {
    match kind {
        CheckKind::Program => CoreTextKind::Program,
        CheckKind::Run => CoreTextKind::Run,
        CheckKind::Auto => match detect_kind(bytes) {
            Some(CoreTextKind::Run) => CoreTextKind::Run,
            _ => CoreTextKind::Program,
        },
    }
}

fn resolve_print_kind(kind: PrintKind, bytes: &[u8]) -> CoreTextKind {
    match kind {
        PrintKind::Exact(k) => k,
        PrintKind::Auto => detect_kind(bytes).unwrap_or(CoreTextKind::Program),
    }
}

fn run_core_check(args: &[String], mut err: impl Write) -> u8 {
    let mut format = RenderFormat::Text;
    let mut kind = CheckKind::Auto;
    let mut path: Option<&str> = None;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--json" {
            format = RenderFormat::Json;
        } else if arg == "--kind" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --kind requires an argument");
                return 2;
            };
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if let Some(val) = arg.strip_prefix("--kind=") {
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if arg == "--help" || arg == "-h" {
            let _ = print_usage(&mut err);
            return 0;
        } else if arg != "-" && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else if path.is_some() {
            let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
            return 2;
        } else {
            path = Some(arg);
        }
        i += 1;
    }

    let (label, bytes) = match read_core_input(path) {
        Ok(res) => res,
        Err(e) => {
            let _ = writeln!(err, "error: cannot read {}: {e}", path.unwrap_or("<stdin>"));
            return 2;
        }
    };

    let target_kind = resolve_check_kind(kind, &bytes);
    let mut diags = DiagBag::new();
    let parsed_prog = match target_kind {
        CoreTextKind::Run => parse_run(&bytes, &mut diags).map(|r| (r.arena, r.value.program)),
        _ => parse_program(&bytes, &mut diags).map(|p| (p.arena, p.value)),
    };

    let Some((arena, program)) = parsed_prog else {
        let mut sources = SourceMap::new();
        let patch = sources
            .add_bytes(label.into_bytes(), bytes.clone())
            .ok()
            .map(|id| (id, bytes.len()));
        if render_diags(&diags, &sources, patch, format, &mut err).is_err() {
            return 2;
        }
        return 1;
    };

    let mut check_diags = DiagBag::new();
    if check_program(&arena, &program, &mut check_diags) {
        0
    } else {
        let empty = SourceMap::new();
        if render_diags(&check_diags, &empty, None, format, &mut err).is_err() {
            return 2;
        }
        1
    }
}

fn run_core_print(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut format = RenderFormat::Text;
    let mut kind = PrintKind::Auto;
    let mut path: Option<&str> = None;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--json" {
            format = RenderFormat::Json;
        } else if arg == "--kind" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --kind requires an argument");
                return 2;
            };
            match parse_print_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if let Some(val) = arg.strip_prefix("--kind=") {
            match parse_print_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if arg == "--help" || arg == "-h" {
            let _ = print_usage(&mut err);
            return 0;
        } else if arg != "-" && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else if path.is_some() {
            let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
            return 2;
        } else {
            path = Some(arg);
        }
        i += 1;
    }

    let (label, bytes) = match read_core_input(path) {
        Ok(res) => res,
        Err(e) => {
            let _ = writeln!(err, "error: cannot read {}: {e}", path.unwrap_or("<stdin>"));
            return 2;
        }
    };

    let target_kind = resolve_print_kind(kind, &bytes);
    let mut diags = DiagBag::new();
    let Some(parsed) = parse_document(target_kind, &bytes, &mut diags) else {
        let mut sources = SourceMap::new();
        let patch = sources
            .add_bytes(label.into_bytes(), bytes.clone())
            .ok()
            .map(|id| (id, bytes.len()));
        if render_diags(&diags, &sources, patch, format, &mut err).is_err() {
            return 2;
        }
        return 1;
    };

    match print_document(&parsed.arena, &parsed.value) {
        Ok(text) => {
            if out.write_all(text.as_bytes()).is_err() {
                return 2;
            }
            0
        }
        Err(e) => {
            let _ = writeln!(err, "error: cannot print Core document: {e}");
            1
        }
    }
}

fn run_core_check_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut kind = CheckKind::Run;
    let mut files: Vec<&str> = Vec::new();
    let mut i = 0usize;
    let mut literal_only = false;
    while i < args.len() {
        let arg = args[i].as_str();
        if !literal_only && arg == "--" {
            literal_only = true;
        } else if !literal_only && arg == "--kind" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --kind requires an argument");
                return 2;
            };
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if !literal_only && let Some(val) = arg.strip_prefix("--kind=") {
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if !literal_only && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else {
            files.push(arg);
        }
        i += 1;
    }

    for path in files {
        let Ok(file) = File::open(Path::new(path)) else {
            let _ = writeln!(out, "8");
            return 8;
        };
        let Ok(bytes) = read_bounded(file, MO_LIMIT_CORE_TEXT_SIZE) else {
            let _ = writeln!(out, "8");
            return 8;
        };
        let target_kind = resolve_check_kind(kind, &bytes);
        let mut diags = DiagBag::new();
        let parsed = match target_kind {
            CoreTextKind::Run => parse_run(&bytes, &mut diags).map(|r| (r.arena, r.value.program)),
            _ => parse_program(&bytes, &mut diags).map(|p| (p.arena, p.value)),
        };
        let status: u8 = match parsed {
            None => 4,
            Some((arena, program)) => {
                if check_program(&arena, &program, &mut diags) {
                    0
                } else {
                    5
                }
            }
        };
        if let Some(first) = diags.items().first() {
            let detail = first.reason.as_ref().unwrap_or(&first.message);
            if writeln!(
                out,
                "{status} E{:04} {}",
                first.code.number(),
                detail.as_str_lossy()
            )
            .is_err()
            {
                return 8;
            }
        } else if writeln!(out, "{status}").is_err() {
            return 8;
        }
    }

    if out.flush().is_err() { 8 } else { 0 }
}

fn run_core_print_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut kind = PrintKind::Exact(CoreTextKind::Run);
    let mut files: Vec<&str> = Vec::new();
    let mut i = 0usize;
    let mut literal_only = false;
    while i < args.len() {
        let arg = args[i].as_str();
        if !literal_only && arg == "--" {
            literal_only = true;
        } else if !literal_only && arg == "--kind" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --kind requires an argument");
                return 2;
            };
            match parse_print_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if !literal_only && let Some(val) = arg.strip_prefix("--kind=") {
            match parse_print_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if !literal_only && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else {
            files.push(arg);
        }
        i += 1;
    }

    for path in files {
        let Ok(file) = File::open(Path::new(path)) else {
            return 8;
        };
        let Ok(bytes) = read_bounded(file, MO_LIMIT_CORE_TEXT_SIZE) else {
            return 8;
        };
        let target_kind = resolve_print_kind(kind, &bytes);
        let mut diags = DiagBag::new();
        let printed = parse_document(target_kind, &bytes, &mut diags).and_then(|parsed| {
            let doc: CoreDocument = parsed.value;
            print_document(&parsed.arena, &doc).ok()
        });
        match printed {
            Some(text) => {
                if writeln!(out, "R {}", text.len()).is_err()
                    || out.write_all(text.as_bytes()).is_err()
                {
                    return 8;
                }
            }
            None => {
                let code = diags.items().first().map_or(0, |d| d.code.number());
                if writeln!(out, "E {code}").is_err() {
                    return 8;
                }
            }
        }
    }

    if out.flush().is_err() { 8 } else { 0 }
}

fn push_source_load_diag(diags: &mut DiagBag, path: &str, err: SourceError) {
    let (code, message, reason) = match err {
        SourceError::Unreadable(_) => (
            ErrorCode::ModuleMissing,
            "a module's file can't be read",
            Some("Every module is found from its name under the root folder (reference 2.1)."),
        ),
        SourceError::FileTooBig => (ErrorCode::FileTooBig, "a source file is too big", None),
        SourceError::ProgramTooBig => (
            ErrorCode::ProgramTooBig,
            "the program's source is too big",
            None,
        ),
        SourceError::TooManyFiles => (
            ErrorCode::TooManyFiles,
            "the program has too many source files",
            None,
        ),
        SourceError::PathTooLong => (
            ErrorCode::PathTooLong,
            "a source file's path is too long",
            None,
        ),
    };
    let mut diag = Diagnostic::new(code, Span::NONE, message).with_primary_text(path);
    if let Some(r) = reason {
        diag = diag.with_reason(r);
    }
    let _ = diags.push(diag);
}

fn load_parse_source(
    path: Option<&str>,
    sources: &mut SourceMap,
    diags: &mut DiagBag,
) -> Option<u32> {
    match path {
        None | Some("-") => match read_bounded(io::stdin().lock(), MO_LIMIT_FILE_SIZE) {
            Ok(bytes) => match sources.add_bytes(b"<stdin>".to_vec(), bytes) {
                Ok(id) => Some(id),
                Err(err) => {
                    push_source_load_diag(diags, "<stdin>", err);
                    None
                }
            },
            Err(e) => {
                push_source_load_diag(diags, "<stdin>", SourceError::Unreadable(e.to_string()));
                None
            }
        },
        Some(p) => match sources.add_file(Path::new(p)) {
            Ok(id) => Some(id),
            Err(err) => {
                push_source_load_diag(diags, p, err);
                None
            }
        },
    }
}

fn run_parse(args: &[String], mut err: impl Write) -> u8 {
    let mut mode = ParseMode::Module;
    let mut seen_mode = false;
    let mut format = RenderFormat::Text;
    let mut path: Option<&str> = None;

    for arg in args {
        match arg.as_str() {
            "--fragment" => {
                if seen_mode && mode != ParseMode::Fragment {
                    let _ = writeln!(err, "error: --fragment and --header are mutually exclusive");
                    return 2;
                }
                mode = ParseMode::Fragment;
                seen_mode = true;
            }
            "--header" => {
                if seen_mode && mode != ParseMode::Header {
                    let _ = writeln!(err, "error: --fragment and --header are mutually exclusive");
                    return 2;
                }
                mode = ParseMode::Header;
                seen_mode = true;
            }
            "--json" => format = RenderFormat::Json,
            "--stdin" => {
                if path.is_some() {
                    let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
                    return 2;
                }
                path = Some("-");
            }
            "--help" | "-h" => {
                let _ = print_usage(&mut err);
                return 0;
            }
            a if a != "-" && a.starts_with('-') => {
                let _ = writeln!(err, "error: unknown flag {a:?}");
                return 2;
            }
            a => {
                if path.is_some() {
                    let _ = writeln!(err, "error: unexpected extra argument {a:?}");
                    return 2;
                }
                path = Some(a);
            }
        }
    }

    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let ok = if let Some(file) = load_parse_source(path, &mut sources, &mut diags) {
        let parsed = match mode {
            ParseMode::Module => parse_module(&sources, file, &mut diags).is_some(),
            ParseMode::Fragment => parse_fragment(&sources, file, &mut diags).is_some(),
            ParseMode::Header => read_header(&sources, file, &mut diags).is_some(),
        };
        parsed && !diags.has_any()
    } else {
        false
    };

    if ok {
        return 0;
    }
    match render_sorted_diags(&mut diags, &sources, format, false, &mut err) {
        Ok(_) => 1,
        Err(_) => 2,
    }
}

fn run_format(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut out_mode = FormatOutMode::Default;
    let mut format = RenderFormat::Text;
    let mut path: Option<&str> = None;

    for arg in args {
        match arg.as_str() {
            "--stdout" => {
                if out_mode == FormatOutMode::Check {
                    let _ = writeln!(err, "error: --stdout and --check are mutually exclusive");
                    return 2;
                }
                out_mode = FormatOutMode::Stdout;
            }
            "--check" => {
                if out_mode == FormatOutMode::Stdout {
                    let _ = writeln!(err, "error: --stdout and --check are mutually exclusive");
                    return 2;
                }
                out_mode = FormatOutMode::Check;
            }
            "--json" => format = RenderFormat::Json,
            "--stdin" => {
                if path.is_some() {
                    let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
                    return 2;
                }
                path = Some("-");
            }
            "--help" | "-h" => {
                let _ = print_usage(&mut err);
                return 0;
            }
            a if a != "-" && a.starts_with('-') => {
                let _ = writeln!(err, "error: unknown flag {a:?}");
                return 2;
            }
            a => {
                if path.is_some() {
                    let _ = writeln!(err, "error: unexpected extra argument {a:?}");
                    return 2;
                }
                path = Some(a);
            }
        }
    }

    let stdin_mode = matches!(path, None | Some("-"));
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let mut formatted_bytes: Option<Vec<u8>> = None;
    let mut changed = false;

    if stdin_mode {
        match read_bounded(io::stdin().lock(), MO_LIMIT_FILE_SIZE) {
            Err(_) => {
                push_format_problem(
                    &mut diags,
                    NO_FILE,
                    ErrorCode::ToolFormatIo,
                    "<stdin>",
                    "standard input could not be read for formatting",
                );
            }
            Ok(bytes) if (bytes.len() as u64) > MO_LIMIT_FILE_SIZE => {
                push_format_problem(
                    &mut diags,
                    NO_FILE,
                    ErrorCode::FileTooBig,
                    "<stdin>",
                    "the source file could not be read for formatting",
                );
            }
            Ok(bytes) => match sources.add_bytes(b"<stdin>".to_vec(), bytes) {
                Err(err_kind) => {
                    let code = if err_kind == SourceError::FileTooBig {
                        ErrorCode::FileTooBig
                    } else {
                        ErrorCode::ToolFormatLimit
                    };
                    push_format_problem(
                        &mut diags,
                        NO_FILE,
                        code,
                        "<stdin>",
                        "the source file could not be read for formatting",
                    );
                }
                Ok(file) => {
                    if let Some(module) = parse_module(&sources, file, &mut diags)
                        && !diags.has_any()
                    {
                        match format_module(&module, FormatOptions::new(100, display_width_bytes)) {
                            Ok(rendered) => {
                                if let Some(src) = sources.get(file) {
                                    changed = rendered.as_slice() != src.bytes();
                                }
                                formatted_bytes = Some(rendered);
                            }
                            Err(_) => {
                                push_format_problem(
                                    &mut diags,
                                    file,
                                    ErrorCode::ToolFormatLimit,
                                    "<stdin>",
                                    "source printing exceeds this build's memory, depth, output or line limit",
                                );
                            }
                        }
                    }
                }
            },
        }
    } else if let Some(p) = path {
        if (p.len() as u64) > MO_LIMIT_PATH_SIZE {
            push_format_problem(
                &mut diags,
                NO_FILE,
                ErrorCode::PathTooLong,
                p,
                "the source path exceeds this build's limit",
            );
        } else {
            let regular = fs::symlink_metadata(Path::new(p))
                .map(|m| m.file_type().is_file())
                .unwrap_or(false);
            if !regular {
                push_format_problem(
                    &mut diags,
                    NO_FILE,
                    ErrorCode::ToolFormatIo,
                    p,
                    "format needs a readable regular file, not a symlink or special file",
                );
            } else {
                match sources.add_file(Path::new(p)) {
                    Err(src_err) => {
                        let code = match src_err {
                            SourceError::Unreadable(_) => ErrorCode::ToolFormatIo,
                            SourceError::FileTooBig => ErrorCode::FileTooBig,
                            SourceError::PathTooLong => ErrorCode::PathTooLong,
                            SourceError::TooManyFiles | SourceError::ProgramTooBig => {
                                ErrorCode::ToolFormatLimit
                            }
                        };
                        push_format_problem(
                            &mut diags,
                            NO_FILE,
                            code,
                            p,
                            "the source file could not be read for formatting",
                        );
                    }
                    Ok(file) => {
                        if let Some(module) = parse_module(&sources, file, &mut diags)
                            && !diags.has_any()
                        {
                            match format_module(
                                &module,
                                FormatOptions::new(100, display_width_bytes),
                            ) {
                                Ok(rendered) => {
                                    if let Some(src) = sources.get(file) {
                                        changed = rendered.as_slice() != src.bytes();
                                    }
                                    formatted_bytes = Some(rendered);
                                }
                                Err(_) => {
                                    push_format_problem(
                                        &mut diags,
                                        file,
                                        ErrorCode::ToolFormatLimit,
                                        p,
                                        "source printing exceeds this build's memory, depth, output or line limit",
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    let mut failed = diags.has_any();
    if !failed && let Some(bytes) = formatted_bytes {
        let effective = match out_mode {
            FormatOutMode::Default if stdin_mode => FormatOutMode::Stdout,
            other => other,
        };
        match effective {
            FormatOutMode::Stdout => {
                if out.write_all(&bytes).is_err() || out.flush().is_err() {
                    failed = true;
                    push_format_problem(
                        &mut diags,
                        if stdin_mode { 0 } else { NO_FILE },
                        ErrorCode::ToolFormatIo,
                        path.unwrap_or("<stdin>"),
                        "formatted output could not be written",
                    );
                }
            }
            FormatOutMode::Check => {
                if changed {
                    return 1;
                }
            }
            FormatOutMode::Default => {
                if changed
                    && let Some(p) = path
                    && let Err(e) = fs::write(Path::new(p), &bytes)
                {
                    failed = true;
                    let detail = format!("could not replace the formatted file ({e})");
                    push_format_problem(&mut diags, 0, ErrorCode::ToolFormatIo, p, &detail);
                }
            }
        }
    }

    if diags.has_any() {
        let dropped = render_sorted_diags(&mut diags, &sources, format, true, &mut err);
        if dropped.is_err() {
            return 1;
        }
        return 1;
    }
    u8::from(failed)
}

fn run_parse_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut mode = ParseMode::Module;
    let mut files: Vec<&str> = Vec::new();
    let mut literal_only = false;
    for arg in args {
        let a = arg.as_str();
        if !literal_only && a == "--" {
            literal_only = true;
        } else if !literal_only && a == "--fragment" {
            mode = ParseMode::Fragment;
        } else if !literal_only && a == "--header" {
            mode = ParseMode::Header;
        } else if !literal_only && a.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {a:?}");
            return 2;
        } else {
            files.push(a);
        }
    }

    for path in files {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        let ok = if let Some(file) = load_parse_source(Some(path), &mut sources, &mut diags) {
            let parsed = match mode {
                ParseMode::Module => parse_module(&sources, file, &mut diags).is_some(),
                ParseMode::Fragment => parse_fragment(&sources, file, &mut diags).is_some(),
                ParseMode::Header => read_header(&sources, file, &mut diags).is_some(),
            };
            parsed && !diags.has_any()
        } else {
            false
        };
        if ok {
            if writeln!(out, "0").is_err() {
                return 8;
            }
        } else {
            let code = diags.items().first().map_or(0, |d| d.code.number());
            if writeln!(out, "1 {} E{:04}", diags.len(), code).is_err() {
                return 8;
            }
        }
    }
    if out.flush().is_err() { 8 } else { 0 }
}

fn run_format_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut fragment = false;
    let mut files: Vec<&str> = Vec::new();
    let mut literal_only = false;
    for arg in args {
        let a = arg.as_str();
        if !literal_only && a == "--" {
            literal_only = true;
        } else if !literal_only && a == "--fragment" {
            fragment = true;
        } else if !literal_only && a.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {a:?}");
            return 2;
        } else {
            files.push(a);
        }
    }

    for path in files {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        let formatted = if let Some(file) = load_parse_source(Some(path), &mut sources, &mut diags)
        {
            let parsed = if fragment {
                parse_fragment(&sources, file, &mut diags)
            } else {
                parse_module(&sources, file, &mut diags)
            };
            if let Some(module) = parsed
                && !diags.has_any()
            {
                match format_module(&module, FormatOptions::new(100, display_width_bytes)) {
                    Ok(bytes) => Ok(bytes),
                    Err(_) => Err(ErrorCode::ToolFormatLimit.number()),
                }
            } else {
                Err(diags.items().first().map_or(0, |d| d.code.number()))
            }
        } else {
            Err(diags.items().first().map_or(0, |d| d.code.number()))
        };

        match formatted {
            Ok(bytes) => {
                if writeln!(out, "R {}", bytes.len()).is_err() || out.write_all(&bytes).is_err() {
                    return 8;
                }
            }
            Err(code) => {
                if writeln!(out, "E {code}").is_err() {
                    return 8;
                }
            }
        }
    }
    if out.flush().is_err() { 8 } else { 0 }
}

fn in_library(program: &CheckedProgram, module: ModuleId) -> bool {
    module.index() != 0 && program.modules.module(module).library
}

fn program_has_anchors(program: &CheckedProgram) -> bool {
    for spec in &program.specs {
        if spec.kind == CheckedSpecKind::Anchor && !in_library(program, spec.module) {
            return true;
        }
    }
    for service in &program.services {
        if in_library(program, service.module) {
            continue;
        }
        for promise in &service.promises {
            if !promise.anchors.is_empty() {
                return true;
            }
        }
    }
    false
}

fn runtime_span(
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
    Span::new(NO_FILE, start, end)
}

fn service_anchor_function(
    file: &BytecodeFile,
    module_name: &str,
    promise_number: u32,
    anchor: &CheckedSpec,
) -> Option<u32> {
    let kind = if anchor.allows { "allows" } else { "forbids" };
    let target = format!(
        "{module_name}.service.promise.{promise_number}.{kind}.{}",
        anchor.number
    );
    for (i, f) in file.functions.iter().enumerate() {
        if file.string(f.name) == target {
            return Some(i as u32);
        }
    }
    None
}

fn push_anchor_error(
    file: &BytecodeFile,
    sources: &SourceMap,
    function: Option<u32>,
    message: &str,
    detail: &str,
    line: Option<Span>,
    diags: &mut DiagBag,
) -> bool {
    let span = match function {
        Some(idx) => {
            let dbg = &file.debug[idx as usize];
            runtime_span(file, sources, dbg.file, dbg.start, dbg.end)
        }
        None => Span::NONE,
    };
    let mut diag = Diagnostic::new(ErrorCode::ToolAnchor, span, message).with_primary_text(detail);
    if let Some(line_span) = line {
        diag = diag.with_secondary(Label::with_text(
            line_span,
            "this promise condition was false",
        ));
    }
    diags.push(diag).unwrap_or(false)
}

fn check_one_anchor(
    image: &Image,
    sources: &SourceMap,
    function: u32,
    allows: bool,
    service_promise: Option<&CheckedSpec>,
    diags: &mut DiagBag,
) -> bool {
    let file = image.file();
    let host = HostContext::stdio(Vec::new(), Vec::new());
    let mut process = Process::new(image, Limits::default(), host);
    match process.call_bounded(function, Vec::new(), MO_CLI_ANCHOR_INSTRUCTIONS) {
        Outcome::Returned(val) => {
            let answer = val.as_int();
            if (answer <= 0) != allows {
                let line = if answer > 0 {
                    let idx = (answer - 1) as usize;
                    Some(match service_promise {
                        Some(spec) => spec.conditions[idx].text.span,
                        None => {
                            let subject = file.debug[function as usize].subject as usize;
                            let failed = &file.debug[subject].lines[idx];
                            runtime_span(file, sources, failed.file, failed.start, failed.end)
                        }
                    })
                } else {
                    None
                };
                let message = if allows {
                    "an allows anchor was rejected"
                } else {
                    "a forbids anchor was accepted"
                };
                return push_anchor_error(
                    file,
                    sources,
                    Some(function),
                    message,
                    "allows must satisfy the promise; forbids must fail it (reference 12.2)",
                    line,
                    diags,
                );
            }
            true
        }
        Outcome::Stopped(defect) | Outcome::Failed(defect) => {
            let report = render_defect(file, &defect);
            push_anchor_error(
                file,
                sources,
                Some(function),
                "an anchor could not be checked",
                &report,
                None,
                diags,
            )
        }
    }
}

fn check_anchors_for_wire(
    wire: &[u8],
    sources: &SourceMap,
    program: &CheckedProgram,
    diags: &mut DiagBag,
) {
    let bc = match load_bytecode(wire) {
        Ok(f) => f,
        Err(err) => {
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "compiled bytecode could not be loaded",
                )
                .with_primary_text(err.reason),
            );
            return;
        }
    };
    let image = match Image::init_bounded(bc, Limits::default(), MO_CLI_ANCHOR_INSTRUCTIONS) {
        Ok(img) => img,
        Err(boxed) => {
            let (file, defect) = *boxed;
            let report = render_defect(&file, &defect);
            let _ = push_anchor_error(
                &file,
                sources,
                None,
                "an anchor could not be checked",
                &report,
                None,
                diags,
            );
            return;
        }
    };

    let file = image.file();
    for (i, f) in file.functions.iter().enumerate() {
        if f.kind == FunctionKind::Anchor {
            let allows = file.debug[i].expectation == AnchorExpectation::Allows;
            if !check_one_anchor(&image, sources, i as u32, allows, None, diags) {
                return;
            }
        }
    }
    for service in &program.services {
        for promise in &service.promises {
            let module = promise.spec.module;
            if in_library(program, module) {
                continue;
            }
            let mod_name = &program.modules.module(module).name;
            for anchor in &promise.anchors {
                if let Some(fn_idx) =
                    service_anchor_function(file, mod_name, promise.spec.number, anchor)
                    && !check_one_anchor(
                        &image,
                        sources,
                        fn_idx,
                        anchor.allows,
                        Some(&promise.spec),
                        diags,
                    )
                {
                    return;
                }
            }
        }
    }
}

fn compile_checked_program(
    checked: &CheckedProgram,
    sources: &SourceMap,
    tests_mode: bool,
    emit: EmitMode,
    force_compile: bool,
    run_anchors: bool,
    diags: &mut DiagBag,
) -> Option<(Option<String>, Option<Vec<u8>>)> {
    let anchors = run_anchors && program_has_anchors(checked);
    let lowered = if tests_mode {
        lower_test_services(checked, Some(sources), diags)
    } else {
        lower_services(checked, Some(sources), diags)
    }?;
    if diags.has_any() {
        return None;
    }
    if !check_program_services(
        &lowered.arena,
        &lowered.program,
        Some(&lowered.services),
        diags,
    ) || diags.has_any()
    {
        return None;
    }

    if !force_compile && emit == EmitMode::None && !anchors {
        return Some((None, None));
    }

    let wire = if anchors || force_compile || emit == EmitMode::Bytecode {
        let bytes = compile_core_services(
            &lowered.arena,
            &lowered.program,
            Some(&lowered.services),
            diags,
        )?;
        if diags.has_any() {
            return None;
        }
        Some(bytes)
    } else {
        None
    };

    if anchors && let Some(bytes) = wire.as_deref() {
        check_anchors_for_wire(bytes, sources, checked, diags);
        if diags.has_any() {
            return None;
        }
    }

    let printed = match emit {
        EmitMode::None => None,
        EmitMode::Core => match print_program(&lowered.arena, &lowered.program) {
            Ok(text) => Some(text),
            Err(_) => {
                let _ = diags.push(Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "Core text exceeds its output budget",
                ));
                return None;
            }
        },
        EmitMode::Bytecode => {
            let bytes = wire.as_deref()?;
            match disassemble_bytecode(bytes) {
                Ok(text) => Some(text),
                Err(err) => {
                    let _ = diags.push(
                        Diagnostic::new(
                            ErrorCode::ToolPipeline,
                            Span::NONE,
                            "bytecode text could not be emitted",
                        )
                        .with_primary_text(err.reason),
                    );
                    return None;
                }
            }
        }
    };

    Some((printed, wire))
}

fn names_a_source(path: &Path, sources: &SourceMap) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        let Ok(out_meta) = fs::metadata(path) else {
            return false;
        };
        for i in 0..sources.len() {
            let Some(src) = sources.get(i) else {
                continue;
            };
            let src_path = src.path_lossy();
            if let Ok(in_meta) = fs::metadata(src_path.as_ref())
                && out_meta.dev() == in_meta.dev()
                && out_meta.ino() == in_meta.ino()
            {
                return true;
            }
        }
        false
    }
    #[cfg(not(unix))]
    {
        let Ok(out_canon) = fs::canonicalize(path) else {
            return false;
        };
        for i in 0..sources.len() {
            let Some(src) = sources.get(i) else {
                continue;
            };
            let src_path = src.path_lossy();
            if let Ok(in_canon) = fs::canonicalize(src_path.as_ref())
                && out_canon == in_canon
            {
                return true;
            }
        }
        false
    }
}

fn write_bytecode_file(path_str: &str, sources: &SourceMap, wire: &[u8], diags: &mut DiagBag) {
    let path = Path::new(path_str);
    if names_a_source(path, sources) {
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                "bytecode can't replace a source file",
            )
            .with_primary_text(path_str),
        );
        return;
    }
    let mut file = match File::create(path) {
        Ok(f) => f,
        Err(e) => {
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "bytecode could not be written to the file",
                )
                .with_primary_text(e.to_string()),
            );
            return;
        }
    };
    let is_regular = file.metadata().is_ok_and(|m| m.is_file());
    let write_res = file.write_all(wire).and_then(|()| file.flush());
    drop(file);
    if let Err(e) = write_res {
        if is_regular {
            let _ = fs::remove_file(path);
        }
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                "bytecode could not be written to the file",
            )
            .with_primary_text(e.to_string()),
        );
    }
}

fn emit_check_diagnostics(
    command: &str,
    sources: &SourceMap,
    diags: &mut DiagBag,
    format: RenderFormat,
    mut out: impl Write,
    mut err: impl Write,
) -> u8 {
    diags.sort();
    if format == RenderFormat::Json {
        if out
            .write_all(&render_version_json(command, MO_VERSION))
            .is_err()
        {
            let _ = writeln!(err, "mo: could not write version record");
            return 1;
        }
        let mut written = 0u64;
        let mut dropped = diags.dropped();
        let mut ok = true;
        for diag in diags.items() {
            match render_diagnostic_default(
                sources,
                diag,
                RenderFormat::Json,
                TerminalEncoding::Utf8,
                UNICODE16_METRICS,
            ) {
                Ok(rendered) => {
                    if out.write_all(&rendered).is_ok() {
                        written += 1;
                    } else {
                        ok = false;
                        break;
                    }
                }
                Err(_) => {
                    dropped += 1;
                }
            }
        }
        let status = i32::from(diags.has_any() || dropped != 0 || !ok);
        if ok {
            ok = out
                .write_all(&render_totals_json(written, u64::from(dropped), status))
                .is_ok()
                && out.flush().is_ok();
        }
        if !ok {
            let _ = writeln!(err, "mo: could not write diagnostics");
            return 1;
        }
        return status as u8;
    }

    let mut dropped = diags.dropped();
    let mut ok = true;
    for diag in diags.items() {
        match render_diagnostic_default(
            sources,
            diag,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        ) {
            Ok(rendered) => {
                if err.write_all(&rendered).is_err() {
                    ok = false;
                    break;
                }
            }
            Err(_) => {
                dropped += 1;
            }
        }
    }
    if ok && dropped != 0 {
        ok = writeln!(
            err,
            "{dropped} diagnostics omitted: diagnostic storage or output limit"
        )
        .is_ok();
    }
    if !ok || err.flush().is_err() {
        let _ = writeln!(err, "mo: could not write diagnostics");
        return 1;
    }
    u8::from(diags.has_any() || dropped != 0)
}

fn run_check_stage(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut stage = CheckStage::Full;
    let mut library = LibraryMode::default_embedded();
    let mut format = RenderFormat::Text;
    let mut emit = EmitMode::None;
    let mut emit_tests = false;
    let mut path: Option<&str> = None;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--json" {
            format = RenderFormat::Json;
        } else if arg == "--no-library" {
            library = LibraryMode::None;
        } else if arg == "--emit-tests" {
            emit_tests = true;
        } else if arg == "--emit" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --emit requires an argument");
                return 2;
            };
            match val.as_str() {
                "core" => emit = EmitMode::Core,
                "bytecode" => emit = EmitMode::Bytecode,
                other => {
                    let _ = writeln!(
                        err,
                        "error: unsupported --emit {other:?}: expected core or bytecode"
                    );
                    return 2;
                }
            }
        } else if let Some(val) = arg.strip_prefix("--emit=") {
            match val {
                "core" => emit = EmitMode::Core,
                "bytecode" => emit = EmitMode::Bytecode,
                other => {
                    let _ = writeln!(
                        err,
                        "error: unsupported --emit {other:?}: expected core or bytecode"
                    );
                    return 2;
                }
            }
        } else if arg == "--library" || arg == "--lib" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an argument");
                return 2;
            };
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if let Some(val) = arg
            .strip_prefix("--library=")
            .or_else(|| arg.strip_prefix("--lib="))
        {
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if arg == "--stage" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --stage requires an argument");
                return 2;
            };
            let Some(parsed) = CheckStage::parse(val) else {
                let _ = writeln!(
                    err,
                    "error: invalid --stage {val:?}: expected load, modules, signatures, or full"
                );
                return 2;
            };
            stage = parsed;
        } else if let Some(val) = arg.strip_prefix("--stage=") {
            let Some(parsed) = CheckStage::parse(val) else {
                let _ = writeln!(
                    err,
                    "error: invalid --stage {val:?}: expected load, modules, signatures, or full"
                );
                return 2;
            };
            stage = parsed;
        } else if arg == "--help" || arg == "-h" {
            let _ = print_usage(&mut err);
            return 0;
        } else if arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else if path.is_some() {
            let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
            return 2;
        } else {
            path = Some(arg);
        }
        i += 1;
    }

    if format == RenderFormat::Json && emit != EmitMode::None {
        let _ = writeln!(
            err,
            "--json and --emit cannot be combined: \"--json\" \"--emit\""
        );
        return 2;
    }

    let Some(entry) = path else {
        let _ = writeln!(err, "error: check requires a FILE.mo argument");
        return 2;
    };

    let options = LoadOptions {
        library,
        ..LoadOptions::default()
    };
    let mut result = run_stage(entry, &options, stage);
    let mut emitted_text = None;
    if stage == CheckStage::Full
        && result.ok()
        && let Some(checked) = result.program.as_ref()
        && let Some((printed, _)) = compile_checked_program(
            checked,
            &result.sources,
            emit_tests,
            emit,
            false,
            true,
            &mut result.diags,
        )
    {
        emitted_text = printed;
    }

    if let Some(text) = emitted_text
        && !result.diags.has_any()
        && format == RenderFormat::Text
        && (out.write_all(text.as_bytes()).is_err() || out.flush().is_err())
    {
        let _ = result.diags.push(Diagnostic::new(
            ErrorCode::ToolPipeline,
            Span::NONE,
            "compiled output could not be written",
        ));
    }

    emit_check_diagnostics(
        "check",
        &result.sources,
        &mut result.diags,
        format,
        out,
        err,
    )
}

fn run_check_stage_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut stage = CheckStage::Full;
    let mut library = LibraryMode::default_embedded();
    let mut files: Vec<&str> = Vec::new();
    let mut literal_only = false;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if !literal_only && arg == "--" {
            literal_only = true;
        } else if !literal_only && arg == "--no-library" {
            library = LibraryMode::None;
        } else if !literal_only && (arg == "--library" || arg == "--lib") {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an argument");
                return 2;
            };
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if !literal_only
            && let Some(val) = arg
                .strip_prefix("--library=")
                .or_else(|| arg.strip_prefix("--lib="))
        {
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if !literal_only && arg == "--stage" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --stage requires an argument");
                return 2;
            };
            let Some(parsed) = CheckStage::parse(val) else {
                let _ = writeln!(
                    err,
                    "error: invalid --stage {val:?}: expected load, modules, signatures, or full"
                );
                return 2;
            };
            stage = parsed;
        } else if !literal_only && let Some(val) = arg.strip_prefix("--stage=") {
            let Some(parsed) = CheckStage::parse(val) else {
                let _ = writeln!(
                    err,
                    "error: invalid --stage {val:?}: expected load, modules, signatures, or full"
                );
                return 2;
            };
            stage = parsed;
        } else if !literal_only && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else {
            files.push(arg);
        }
        i += 1;
    }

    let options = LoadOptions {
        library,
        ..LoadOptions::default()
    };
    for path in files {
        let mut result = run_stage(path, &options, stage);
        if stage == CheckStage::Full
            && result.ok()
            && let Some(checked) = result.program.as_ref()
        {
            let _ = compile_checked_program(
                checked,
                &result.sources,
                false,
                EmitMode::None,
                false,
                true,
                &mut result.diags,
            );
        }
        if result.ok() {
            if writeln!(out, "0").is_err() {
                return 8;
            }
        } else {
            result.diags.sort();
            let code = result.diags.items().first().map_or(0, |d| d.code.number());
            if writeln!(out, "1 {} E{:04}", result.diags.len(), code).is_err() {
                return 8;
            }
        }
    }
    if out.flush().is_err() { 8 } else { 0 }
}

fn run_lower(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut tests_mode = false;
    let mut library = LibraryMode::default_embedded();
    let mut format = RenderFormat::Text;
    let mut path: Option<&str> = None;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--tests" {
            tests_mode = true;
        } else if arg == "--json" {
            format = RenderFormat::Json;
        } else if arg == "--no-library" {
            library = LibraryMode::None;
        } else if arg == "--library" || arg == "--lib" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an argument");
                return 2;
            };
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if let Some(val) = arg
            .strip_prefix("--library=")
            .or_else(|| arg.strip_prefix("--lib="))
        {
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if arg == "--help" || arg == "-h" {
            let _ = print_usage(&mut err);
            return 0;
        } else if arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else if path.is_some() {
            let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
            return 2;
        } else {
            path = Some(arg);
        }
        i += 1;
    }

    let Some(entry) = path else {
        let _ = writeln!(err, "error: lower requires a FILE.mo argument");
        return 2;
    };

    let options = LoadOptions {
        library,
        ..LoadOptions::default()
    };
    let mut result = run_stage(entry, &options, CheckStage::Full);
    let mut emitted = None;
    if result.ok()
        && let Some(checked) = result.program.as_ref()
        && let Some((printed, _)) = compile_checked_program(
            checked,
            &result.sources,
            tests_mode,
            EmitMode::Core,
            false,
            false,
            &mut result.diags,
        )
    {
        emitted = printed;
    }

    if let Some(text) = emitted
        && !result.diags.has_any()
    {
        if out.write_all(text.as_bytes()).is_err() || out.flush().is_err() {
            let _ = writeln!(err, "mo: compiled output could not be written");
            return 1;
        }
        return 0;
    }

    emit_check_diagnostics(
        "lower",
        &result.sources,
        &mut result.diags,
        format,
        out,
        err,
    )
}

fn run_lower_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut tests_mode = false;
    let mut library = LibraryMode::default_embedded();
    let mut files: Vec<&str> = Vec::new();
    let mut literal_only = false;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if !literal_only && arg == "--" {
            literal_only = true;
        } else if !literal_only && arg == "--tests" {
            tests_mode = true;
        } else if !literal_only && arg == "--no-library" {
            library = LibraryMode::None;
        } else if !literal_only && (arg == "--library" || arg == "--lib") {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an argument");
                return 2;
            };
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if !literal_only
            && let Some(val) = arg
                .strip_prefix("--library=")
                .or_else(|| arg.strip_prefix("--lib="))
        {
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if !literal_only && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else {
            files.push(arg);
        }
        i += 1;
    }

    let options = LoadOptions {
        library,
        ..LoadOptions::default()
    };
    for path in files {
        let mut result = run_stage(path, &options, CheckStage::Full);
        let emitted = if result.ok()
            && let Some(checked) = result.program.as_ref()
        {
            compile_checked_program(
                checked,
                &result.sources,
                tests_mode,
                EmitMode::Core,
                false,
                false,
                &mut result.diags,
            )
            .and_then(|(printed, _)| printed)
        } else {
            None
        };
        match emitted {
            Some(text) if !result.diags.has_any() => {
                if writeln!(out, "R {}", text.len()).is_err()
                    || out.write_all(text.as_bytes()).is_err()
                {
                    return 8;
                }
            }
            _ => {
                result.diags.sort();
                let code = result.diags.items().first().map_or(0, |d| d.code.number());
                if writeln!(out, "E {code}").is_err() {
                    return 8;
                }
            }
        }
    }
    if out.flush().is_err() { 8 } else { 0 }
}

fn run_emit(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut tests_mode = false;
    let mut library = LibraryMode::default_embedded();
    let mut format = RenderFormat::Text;
    let mut path: Option<&str> = None;
    let mut output_path: Option<&str> = None;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--tests" {
            tests_mode = true;
        } else if arg == "--json" {
            format = RenderFormat::Json;
        } else if arg == "--no-library" {
            library = LibraryMode::None;
        } else if arg == "-o" || arg == "--output" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an output path");
                return 2;
            };
            output_path = Some(val.as_str());
        } else if let Some(val) = arg.strip_prefix("--output=") {
            output_path = Some(val);
        } else if arg == "--library" || arg == "--lib" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an argument");
                return 2;
            };
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if let Some(val) = arg
            .strip_prefix("--library=")
            .or_else(|| arg.strip_prefix("--lib="))
        {
            library = LibraryMode::Directory(PathBuf::from(val));
        } else if arg == "--help" || arg == "-h" {
            let _ = print_usage(&mut err);
            return 0;
        } else if arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else if path.is_some() {
            let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
            return 2;
        } else {
            path = Some(arg);
        }
        i += 1;
    }

    let Some(entry) = path else {
        let _ = writeln!(err, "error: emit requires a FILE.mo argument");
        return 2;
    };
    let Some(out_target) = output_path else {
        let _ = writeln!(err, "error: emit requires -o OUT.bc");
        return 2;
    };

    let options = LoadOptions {
        library,
        ..LoadOptions::default()
    };
    let mut result = run_stage(entry, &options, CheckStage::Full);
    let mut wire = None;
    if result.ok()
        && let Some(checked) = result.program.as_ref()
    {
        let has_entry_service = checked
            .services
            .iter()
            .any(|s| s.module.index() == 0 && !s.is_actor);
        let has_entry_api = checked.apis.iter().any(|a| a.module.index() == 0);
        if !tests_mode && !has_entry_service && !has_entry_api && checked.entry.function.is_none() {
            let span = checked.modules.module(ModuleId(0)).syntax.span;
            let _ = result.diags.push(
                Diagnostic::new(
                    ErrorCode::MainContract,
                    span,
                    "the entry module has no main function",
                )
                .with_primary_text("the entry module has no main function")
                .with_reason("main declares the program's entry (reference 11.2)."),
            );
        } else if let Some((_, bytes)) = compile_checked_program(
            checked,
            &result.sources,
            tests_mode,
            EmitMode::None,
            true,
            true,
            &mut result.diags,
        ) {
            wire = bytes;
        }
    }

    if let Some(bytes) = wire
        && !result.diags.has_any()
    {
        if out_target == "-" {
            if out.write_all(&bytes).is_err() || out.flush().is_err() {
                let _ = writeln!(err, "mo: compiled output could not be written");
                return 1;
            }
            return 0;
        }
        write_bytecode_file(out_target, &result.sources, &bytes, &mut result.diags);
    }

    emit_check_diagnostics("emit", &result.sources, &mut result.diags, format, out, err)
}

fn run_codegen_core(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut kind = CheckKind::Auto;
    let mut disasm = false;
    let mut output_path: Option<&str> = None;
    let mut path: Option<&str> = None;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--disassemble" {
            disasm = true;
        } else if arg == "-o" || arg == "--output" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: {arg} requires an argument");
                return 2;
            };
            output_path = Some(val.as_str());
        } else if arg == "--kind" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --kind requires an argument");
                return 2;
            };
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if let Some(val) = arg.strip_prefix("--kind=") {
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if arg != "-" && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else if path.is_some() {
            let _ = writeln!(err, "error: unexpected extra argument {arg:?}");
            return 2;
        } else {
            path = Some(arg);
        }
        i += 1;
    }

    let (label, bytes) = match read_core_input(path) {
        Ok(res) => res,
        Err(e) => {
            let _ = writeln!(err, "error: cannot read {}: {e}", path.unwrap_or("<stdin>"));
            return 2;
        }
    };
    let target_kind = resolve_check_kind(kind, &bytes);
    let mut diags = DiagBag::new();
    let parsed = match target_kind {
        CoreTextKind::Run => parse_run(&bytes, &mut diags).map(|r| (r.arena, r.value.program)),
        _ => parse_program(&bytes, &mut diags).map(|p| (p.arena, p.value)),
    };
    let Some((arena, program)) = parsed else {
        let mut sources = SourceMap::new();
        let patch = sources
            .add_bytes(label.into_bytes(), bytes.clone())
            .ok()
            .map(|id| (id, bytes.len()));
        let _ = render_diags(&diags, &sources, patch, RenderFormat::Text, &mut err);
        return 1;
    };
    let mut cdiags = DiagBag::new();
    let Some(wire) = compile_core(&arena, &program, &mut cdiags) else {
        let empty = SourceMap::new();
        let _ = render_diags(&cdiags, &empty, None, RenderFormat::Text, &mut err);
        return 1;
    };
    if disasm {
        match disassemble_bytecode(&wire) {
            Ok(text) => {
                if out.write_all(text.as_bytes()).is_err() || out.flush().is_err() {
                    return 2;
                }
            }
            Err(e) => {
                let _ = writeln!(err, "error: disassemble failed: {e}");
                return 1;
            }
        }
    } else if let Some(dest) = output_path {
        if dest == "-" {
            if out.write_all(&wire).is_err() || out.flush().is_err() {
                return 2;
            }
        } else if let Err(e) = fs::write(dest, &wire) {
            let _ = writeln!(err, "error: cannot write {dest}: {e}");
            return 1;
        }
    }
    0
}

fn run_codegen_core_batch(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut kind = CheckKind::Auto;
    let mut bigints = false;
    let mut out_dir: Option<PathBuf> = None;
    let mut files: Vec<&str> = Vec::new();
    let mut literal_only = false;
    let mut i = 0usize;
    while i < args.len() {
        let arg = args[i].as_str();
        if !literal_only && arg == "--" {
            literal_only = true;
        } else if !literal_only && arg == "--bigints" {
            bigints = true;
        } else if !literal_only && arg == "--out-dir" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --out-dir requires an argument");
                return 2;
            };
            out_dir = Some(PathBuf::from(val));
        } else if !literal_only && let Some(val) = arg.strip_prefix("--out-dir=") {
            out_dir = Some(PathBuf::from(val));
        } else if !literal_only && arg == "--kind" {
            i += 1;
            let Some(val) = args.get(i) else {
                let _ = writeln!(err, "error: --kind requires an argument");
                return 2;
            };
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if !literal_only && let Some(val) = arg.strip_prefix("--kind=") {
            match parse_check_kind(val) {
                Ok(k) => kind = k,
                Err(msg) => {
                    let _ = writeln!(err, "error: {msg}");
                    return 2;
                }
            }
        } else if !literal_only && arg.starts_with('-') {
            let _ = writeln!(err, "error: unknown flag {arg:?}");
            return 2;
        } else {
            files.push(arg);
        }
        i += 1;
    }

    for (idx, path) in files.iter().enumerate() {
        let Ok(file) = File::open(Path::new(path)) else {
            let _ = writeln!(out, "8");
            return 8;
        };
        let Ok(bytes) = read_bounded(file, MO_LIMIT_CORE_TEXT_SIZE) else {
            let _ = writeln!(out, "8");
            return 8;
        };
        let target_kind = resolve_check_kind(kind, &bytes);
        let mut diags = DiagBag::new();
        let parsed = match target_kind {
            CoreTextKind::Run => parse_run(&bytes, &mut diags).map(|r| (r.arena, r.value.program)),
            _ => parse_program(&bytes, &mut diags).map(|p| (p.arena, p.value)),
        };
        let Some((arena, program)) = parsed else {
            let code = diags.items().first().map_or(0, |d| d.code.number());
            if writeln!(out, "4 E{code:04}").is_err() {
                return 8;
            }
            continue;
        };
        let mut cdiags1 = DiagBag::new();
        let Some(bc1) = compile_core(&arena, &program, &mut cdiags1) else {
            let first = cdiags1.items().first();
            let code = first.map_or(0, |d| d.code.number());
            let detail = first
                .and_then(|d| d.reason.as_ref().or(Some(&d.message)))
                .map_or_else(String::new, |t| t.as_str_lossy().into_owned());
            if writeln!(out, "5 E{code:04} {detail}").is_err() {
                return 8;
            }
            continue;
        };
        let mut cdiags2 = DiagBag::new();
        let Some(bc2) = compile_core(&arena, &program, &mut cdiags2) else {
            if writeln!(out, "6 nondeterministic-failure").is_err() {
                return 8;
            }
            continue;
        };
        if bc1 != bc2 {
            if writeln!(out, "6 nondeterministic-bytes").is_err() {
                return 8;
            }
            continue;
        }
        let Ok(decoded) = load_bytecode(&bc1) else {
            if writeln!(out, "6 verify-failed").is_err() {
                return 8;
            }
            continue;
        };
        let Ok(rewritten) = write_bytecode(&decoded) else {
            if writeln!(out, "6 rewrite-failed").is_err() {
                return 8;
            }
            continue;
        };
        if bc1 != rewritten {
            if writeln!(out, "6 roundtrip-mismatch").is_err() {
                return 8;
            }
            continue;
        }
        if let Some(dir) = out_dir.as_ref() {
            let dest = dir.join(format!("{idx}.bc"));
            if fs::write(&dest, &bc1).is_err() {
                return 8;
            }
        }
        if bigints {
            let Some((sign, mag)) = decoded.constants.iter().find_map(|c| match &c.value {
                ConstantValue::BigInt { sign, magnitude } => Some((*sign, magnitude.as_slice())),
                _ => None,
            }) else {
                if writeln!(out, "6 missing-bigint-constant").is_err() {
                    return 8;
                }
                continue;
            };
            let sign_num = match sign {
                BigIntSign::Zero => 0u8,
                BigIntSign::Positive => 1u8,
                BigIntSign::Negative => 2u8,
            };
            let mut hex = String::with_capacity(mag.len().saturating_mul(2));
            for &b in mag {
                let _ = std::fmt::Write::write_fmt(&mut hex, format_args!("{b:02x}"));
            }
            if writeln!(out, "0 {sign_num}:{hex}").is_err() {
                return 8;
            }
        } else if writeln!(out, "0 {}", bc1.len()).is_err() {
            return 8;
        }
    }
    if out.flush().is_err() { 8 } else { 0 }
}

fn run_roundtrip_bc(args: &[String], mut out: impl Write, mut err: impl Write) -> u8 {
    let mut check_only = false;
    let mut paths: Vec<&str> = Vec::new();
    let mut literal_only = false;
    for arg in args {
        match arg.as_str() {
            "--" if !literal_only => literal_only = true,
            "--check" if !literal_only => check_only = true,
            a if !literal_only && a.starts_with('-') => {
                let _ = writeln!(err, "error: unknown flag {a:?}");
                return 2;
            }
            a => paths.push(a),
        }
    }
    if paths.is_empty() {
        let _ = writeln!(
            err,
            "error: roundtrip-bc requires at least one FILE.bc argument"
        );
        return 2;
    }
    if !check_only && paths.len() > 1 {
        let _ = writeln!(
            err,
            "error: roundtrip-bc without --check accepts only one FILE.bc"
        );
        return 2;
    }
    for file_path in paths {
        let raw = match fs::read(file_path) {
            Ok(b) => b,
            Err(e) => {
                let _ = writeln!(err, "error: cannot read {file_path}: {e}");
                return 1;
            }
        };
        let decoded = match load_bytecode(&raw) {
            Ok(f) => f,
            Err(e) => {
                let _ = writeln!(err, "error: {file_path}: bytecode load failed: {e}");
                return 1;
            }
        };
        let rewritten = match write_bytecode(&decoded) {
            Ok(b) => b,
            Err(e) => {
                let _ = writeln!(err, "error: {file_path}: bytecode write failed: {e}");
                return 1;
            }
        };
        if raw != rewritten {
            let _ = writeln!(
                err,
                "error: {file_path}: bytecode round-trip mismatch (in {} bytes, out {} bytes)",
                raw.len(),
                rewritten.len()
            );
            return 1;
        }
        if !check_only {
            match disassemble_bytecode(&rewritten) {
                Ok(text) => {
                    if out.write_all(text.as_bytes()).is_err() || out.flush().is_err() {
                        return 2;
                    }
                }
                Err(e) => {
                    let _ = writeln!(err, "error: {file_path}: disassemble failed: {e}");
                    return 1;
                }
            }
        }
    }
    0
}

fn run_core_exec(
    args: &[String],
    with_test_name: bool,
    mut out: impl Write,
    mut err: impl Write,
) -> u8 {
    let expected = if with_test_name { 2 } else { 1 };
    if args.len() != expected {
        let _ = writeln!(
            err,
            "error: expected {} argument(s)",
            if with_test_name {
                "FILE.core TEST"
            } else {
                "FILE.core"
            }
        );
        return 2;
    }
    let path = args[0].as_str();
    let test_name = if with_test_name {
        Some(args[1].as_str())
    } else {
        None
    };

    let (label, bytes) = match read_core_input(Some(path)) {
        Ok(res) => res,
        Err(e) => {
            let _ = writeln!(err, "error: cannot read {path}: {e}");
            return 2;
        }
    };
    let target_kind = resolve_check_kind(CheckKind::Auto, &bytes);
    let mut diags = DiagBag::new();
    let parsed = match target_kind {
        CoreTextKind::Run => parse_run(&bytes, &mut diags).map(|r| (r.arena, r.value.program)),
        _ => parse_program(&bytes, &mut diags).map(|p| (p.arena, p.value)),
    };
    let Some((arena, program)) = parsed else {
        let mut sources = SourceMap::new();
        let patch = sources
            .add_bytes(label.into_bytes(), bytes.clone())
            .ok()
            .map(|id| (id, bytes.len()));
        let _ = render_diags(&diags, &sources, patch, RenderFormat::Text, &mut err);
        return 1;
    };
    let mut chk_diags = DiagBag::new();
    if !check_program(&arena, &program, &mut chk_diags) {
        let empty = SourceMap::new();
        let _ = render_diags(&chk_diags, &empty, None, RenderFormat::Text, &mut err);
        return 1;
    }
    let mut cdiags = DiagBag::new();
    let Some(wire) = compile_core(&arena, &program, &mut cdiags) else {
        if cdiags
            .items()
            .iter()
            .any(|d| d.code == ErrorCode::CodegenUnsupported)
        {
            if writeln!(out, "unsupported").is_err() || out.flush().is_err() {
                return 2;
            }
            return 77;
        }
        let empty = SourceMap::new();
        let _ = render_diags(&cdiags, &empty, None, RenderFormat::Text, &mut err);
        return 1;
    };
    let file = match load_bytecode(&wire) {
        Ok(f) => f,
        Err(e) => {
            let _ = writeln!(err, "error: bytecode load failed: {e}");
            return 1;
        }
    };
    let target_name = test_name.unwrap_or(program.entry.as_str());
    let Some(fn_idx) = program.funs.iter().position(|f| f.name == target_name) else {
        let _ = writeln!(err, "error: entry function {target_name:?} not found");
        return 1;
    };
    if fn_idx >= file.functions.len() {
        let _ = writeln!(
            err,
            "error: entry function index {fn_idx} out of range ({})",
            file.functions.len()
        );
        return 1;
    }
    if !file.functions[fn_idx].parameters.is_empty() {
        let _ = writeln!(
            err,
            "error: entry function {target_name:?} expects {} parameters",
            file.functions[fn_idx].parameters.len()
        );
        return 1;
    }

    const CORE_EXEC_BUDGET: u64 = 250_000;
    let limits = Limits::default();
    let image = match Image::init_bounded_detailed(file, limits, CORE_EXEC_BUDGET) {
        Ok(img) => img,
        Err(boxed) => {
            let (_, is_failed, defect) = *boxed;
            let kind = if is_failed { "failed" } else { "stopped" };
            let detail = defect.detail.as_deref().unwrap_or(&defect.reason);
            if writeln!(out, "{kind}").is_err()
                || writeln!(out, "{detail}").is_err()
                || out.flush().is_err()
            {
                return 2;
            }
            return 0;
        }
    };
    let mut proc = Process::new(&image, limits, HostContext::stdio(Vec::new(), Vec::new()));
    let (outcome, _) = proc.call_with_budget(fn_idx as u32, Vec::new(), CORE_EXEC_BUDGET);
    let write_res = match outcome {
        Outcome::Returned(val) => {
            let res_ty = image.file().functions[fn_idx].result_type;
            let text = if image.file().types.get(res_ty).kind == TypeKind::Float {
                format!("0x{:016x}", val.as_float().to_bits())
            } else {
                value_print(&image.file().types, res_ty, &val)
            };
            writeln!(out, "returned").and_then(|()| writeln!(out, "{text}"))
        }
        Outcome::Failed(defect) => {
            let detail = defect.detail.as_deref().unwrap_or(&defect.reason);
            writeln!(out, "failed").and_then(|()| writeln!(out, "{detail}"))
        }
        Outcome::Stopped(defect) => {
            let detail = defect.detail.as_deref().unwrap_or(&defect.reason);
            writeln!(out, "stopped").and_then(|()| writeln!(out, "{detail}"))
        }
    };
    if write_res.is_err() || out.flush().is_err() {
        return 2;
    }
    0
}

fn dispatch(args: &[String], out: impl Write, mut err: impl Write) -> u8 {
    let Some(cmd) = args.first().map(String::as_str) else {
        let _ = print_usage(&mut err);
        return 2;
    };
    let rest = &args[1..];
    match cmd {
        "--core" => run_core_exec(rest, false, out, err),
        "--core-test" => run_core_exec(rest, true, out, err),
        "core-check" => run_core_check(rest, err),
        "core-print" => run_core_print(rest, out, err),
        "core-check-batch" => run_core_check_batch(rest, out, err),
        "core-print-batch" => run_core_print_batch(rest, out, err),
        "codegen-core" => run_codegen_core(rest, out, err),
        "codegen-core-batch" => run_codegen_core_batch(rest, out, err),
        "roundtrip-bc" => run_roundtrip_bc(rest, out, err),
        "parse" => run_parse(rest, err),
        "format" => run_format(rest, out, err),
        "parse-batch" => run_parse_batch(rest, out, err),
        "format-batch" => run_format_batch(rest, out, err),
        "check" | "check-stage" => run_check_stage(rest, out, err),
        "check-batch" | "check-stage-batch" => run_check_stage_batch(rest, out, err),
        "lower" => run_lower(rest, out, err),
        "lower-batch" => run_lower_batch(rest, out, err),
        "emit" => run_emit(rest, out, err),
        "--help" | "-h" | "help" => {
            let _ = print_usage(out);
            0
        }
        other => {
            let _ = writeln!(err, "error: unknown subcommand {other:?}");
            let _ = print_usage(&mut err);
            2
        }
    }
}

fn main() -> ExitCode {
    mo_host::confine::register_self_as_trampoline();
    let args: Vec<String> = env::args().skip(1).collect();
    let run = move || -> u8 {
        let stdout = io::stdout();
        let stderr = io::stderr();
        dispatch(
            &args,
            BufWriter::new(stdout.lock()),
            BufWriter::new(stderr.lock()),
        )
    };
    let code = match std::thread::Builder::new()
        .name("mo-fe-main".to_string())
        .stack_size(mo_def::limits::MO_SOURCE_STACK_SIZE as usize)
        .spawn(run)
    {
        Ok(handle) => handle.join().unwrap_or(1),
        Err(_) => 1,
    };
    ExitCode::from(code)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn repo_root() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .to_path_buf()
    }

    #[test]
    fn cli_core_check_and_print_on_fixtures() {
        let greet = repo_root().join("tests/core/Greet.core");
        let greet_str = greet.to_str().unwrap().to_string();
        let raw = std::fs::read(&greet).unwrap();

        let mut out = Vec::new();
        let mut err = Vec::new();
        let code = dispatch(
            &["core-check".to_string(), greet_str.clone()],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert!(out.is_empty());
        assert!(err.is_empty());

        let code = dispatch(
            &["core-print".to_string(), greet_str.clone()],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert_eq!(out, raw);
        assert!(err.is_empty());

        out.clear();
        let code = dispatch(
            &["core-check-batch".to_string(), greet_str.clone()],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0);
        assert_eq!(out, b"0\n");

        out.clear();
        let code = dispatch(
            &["core-print-batch".to_string(), greet_str],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0);
        let expected_header = format!("R {}\n", raw.len());
        assert!(out.starts_with(expected_header.as_bytes()));
        assert_eq!(&out[expected_header.len()..], raw.as_slice());
    }

    #[test]
    fn cli_core_check_json_error_and_explicit_kind() {
        let bad_prog = repo_root().join("fuzz/corpus/fuzz_core_check/constant-fake.core");
        let bad_str = bad_prog.to_str().unwrap().to_string();
        let mut out = Vec::new();
        let mut err = Vec::new();
        let code = dispatch(
            &[
                "core-check".to_string(),
                "--json".to_string(),
                "--kind=program".to_string(),
                bad_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 1);
        let err_text = String::from_utf8(err).unwrap();
        assert!(err_text.contains("\"code\":\"E1021\""), "got: {err_text}");

        // Batch mode rejects version 0.8 with status 4 and E1021.
        let mut batch_out = Vec::new();
        let mut batch_err = Vec::new();
        let code = dispatch(
            &["core-check-batch".to_string(), bad_str],
            &mut batch_out,
            &mut batch_err,
        );
        assert_eq!(code, 0);
        let line = String::from_utf8(batch_out).unwrap();
        assert!(line.starts_with("4 E1021 "), "got: {line}");
    }

    #[test]
    fn cli_parse_and_format_on_examples_and_errors() {
        let greet = repo_root().join("examples/greet.mo");
        let greet_str = greet.to_str().unwrap().to_string();
        let raw = std::fs::read(&greet).unwrap();

        let mut out = Vec::new();
        let mut err = Vec::new();
        let code = dispatch(
            &["parse".to_string(), greet_str.clone()],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert!(out.is_empty() && err.is_empty());

        let code = dispatch(
            &[
                "parse".to_string(),
                "--header".to_string(),
                greet_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));

        let code = dispatch(
            &[
                "format".to_string(),
                "--stdout".to_string(),
                greet_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert_eq!(out, raw);

        out.clear();
        let code = dispatch(
            &["format".to_string(), "--check".to_string(), greet_str],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0);

        // Missing file under `format` emits exact E1402 diagnostic.
        let missing = "tests/errors/e1402/nowhere.mo".to_string();
        let expected_e1402 =
            std::fs::read(repo_root().join("tests/errors/e1402/expected")).unwrap();
        err.clear();
        let code = dispatch(&["format".to_string(), missing], &mut out, &mut err);
        assert_eq!(code, 1);
        assert_eq!(err, expected_e1402);
    }

    #[test]
    fn cli_check_stages_and_batch_parity() {
        let greet = repo_root().join("examples/greet.mo");
        let greet_str = greet.to_str().unwrap().to_string();
        let mut out = Vec::new();
        let mut err = Vec::new();

        for stage in ["load", "modules", "signatures"] {
            out.clear();
            err.clear();
            let code = dispatch(
                &[
                    "check".to_string(),
                    format!("--stage={stage}"),
                    greet_str.clone(),
                ],
                &mut out,
                &mut err,
            );
            assert_eq!(
                code,
                0,
                "stage={stage}, stderr: {}",
                String::from_utf8_lossy(&err)
            );
            assert!(out.is_empty() && err.is_empty());
        }

        // Batch mode on examples/greet.mo + tests/errors/e0084/main.mo.
        let e0084 = repo_root().join("tests/errors/e0084/main.mo");
        let e0084_str = e0084.to_str().unwrap().to_string();
        out.clear();
        err.clear();
        let code = dispatch(
            &[
                "check-batch".to_string(),
                "--stage=signatures".to_string(),
                greet_str,
                e0084_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        let lines: Vec<&str> = std::str::from_utf8(&out).unwrap().lines().collect();
        assert_eq!(lines, vec!["0", "1 1 E0084"]);

        // Single-file JSON mode on E0084 emits version, diagnostic, and totals on stdout.
        out.clear();
        err.clear();
        let code = dispatch(
            &[
                "check".to_string(),
                "--stage=signatures".to_string(),
                "--json".to_string(),
                e0084_str,
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 1);
        assert!(
            err.is_empty(),
            "unexpected stderr: {}",
            String::from_utf8_lossy(&err)
        );
        let json_out = String::from_utf8(out).unwrap();
        assert!(
            json_out.contains("\"record\":\"version\""),
            "got: {json_out}"
        );
        assert!(json_out.contains("\"code\":\"E0084\""), "got: {json_out}");
        assert!(
            json_out.contains("\"record\":\"totals\""),
            "got: {json_out}"
        );
    }

    #[test]
    fn cli_lower_and_emit_core_and_e1150() {
        let greet = repo_root().join("examples/greet.mo");
        let greet_str = greet.to_str().unwrap().to_string();
        let mut out = Vec::new();
        let mut err = Vec::new();

        // `mo-fe check --emit=core` emits valid canonical Core text.
        let code = dispatch(
            &[
                "check".to_string(),
                "--emit=core".to_string(),
                greet_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert!(err.is_empty());
        let emitted = String::from_utf8(out.clone()).unwrap();
        assert!(emitted.contains("(core 0.11 (Program "));
        assert!(emitted.contains("Greet.main"));

        // `mo-fe lower --tests` emits specs/examples/tests in Core text.
        out.clear();
        err.clear();
        let code = dispatch(
            &[
                "lower".to_string(),
                "--tests".to_string(),
                greet_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        let test_core = String::from_utf8(out.clone()).unwrap();
        assert!(test_core.contains("Greet.greeting.example."));
        assert!(test_core.contains("Greet.test."));

        // `mo-fe lower-batch` frames valid Core output with `R <len>\n`.
        out.clear();
        err.clear();
        let code = dispatch(&["lower-batch".to_string(), greet_str], &mut out, &mut err);
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert!(out.starts_with(b"R "));

        // `mo-fe check` surfaces lowering error E1150.
        let e1150 = repo_root().join("tests/errors/e1150/main.mo");
        let e1150_str = e1150.to_str().unwrap().to_string();
        out.clear();
        err.clear();
        let code = dispatch(&["check".to_string(), e1150_str], &mut out, &mut err);
        assert_eq!(code, 1);
        let err_text = String::from_utf8(err).unwrap();
        assert!(err_text.contains("E1150"), "got: {err_text}");
    }

    #[test]
    fn cli_emit_bytecode_disasm_and_anchor_errors_exact_parity() {
        let greet = repo_root().join("examples/greet.mo");
        let greet_str = greet.to_str().unwrap().to_string();
        let mut out = Vec::new();
        let mut err = Vec::new();

        // 1. `mo-fe check --emit=bytecode` prints canonical indexed bytecode disassembly.
        let code = dispatch(
            &[
                "check".to_string(),
                "--emit=bytecode".to_string(),
                greet_str.clone(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert!(err.is_empty());
        let dis = String::from_utf8(out.clone()).unwrap();
        assert!(dis.starts_with("bytecode 0.3 indexed\n"), "got: {dis}");
        assert!(dis.contains("Greet.main.entry"), "got: {dis}");

        // 2. `mo-fe emit FILE.mo -o -` produces verified MOBC v0.3 bytecode runnable on mo-vm.
        out.clear();
        err.clear();
        let code = dispatch(
            &[
                "emit".to_string(),
                greet_str,
                "-o".to_string(),
                "-".to_string(),
            ],
            &mut out,
            &mut err,
        );
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert!(err.is_empty());
        assert!(out.starts_with(b"MOBC"));
        let decoded = load_bytecode(&out).expect("emitted bytecode must verify");
        let rewritten = write_bytecode(&decoded).expect("roundtrip write");
        assert_eq!(out, rewritten);

        // 3. `e1405` and `e1405-forbids` match C `mo check` expectations byte-for-byte.
        let root_prefix = format!("{}/", repo_root().display());
        for case in ["e1405", "e1405-forbids"] {
            let abs_path = repo_root()
                .join(format!("tests/errors/{case}/main.mo"))
                .to_str()
                .unwrap()
                .to_string();
            let expected =
                std::fs::read_to_string(repo_root().join(format!("tests/errors/{case}/expected")))
                    .unwrap();
            out.clear();
            err.clear();
            let code = dispatch(&["check".to_string(), abs_path], &mut out, &mut err);
            assert_eq!(code, 1, "case {case}");
            assert!(out.is_empty(), "case {case} unexpected stdout");
            let normalized = String::from_utf8(err.clone())
                .unwrap()
                .replace(&root_prefix, "");
            assert_eq!(normalized, expected, "case {case} mismatch");
        }
    }

    #[test]
    fn cli_core_exec_differential_protocol() {
        let core_path = repo_root().join("tests/differential/entry-returns-one.core");
        let core_str = core_path.to_str().unwrap().to_string();
        let mut out = Vec::new();
        let mut err = Vec::new();
        let code = dispatch(&["--core".to_string(), core_str], &mut out, &mut err);
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert_eq!(String::from_utf8(out).unwrap(), "returned\n1\n");

        // Verify IEEE 754 negative-zero hex bit printing and fail/stop outcomes.
        let tmp = std::env::temp_dir().join(format!("mo-fe-core-exec-{}.core", std::process::id()));
        std::fs::write(
            &tmp,
            "(core 0.11 (Program () ((FunDecl \"entry\" () (float) (lit (float 0x8000000000000000)) \"Generated\" (function) 0)) () \"entry\" (noSource)))\n",
        )
        .unwrap();
        let mut out = Vec::new();
        let mut err = Vec::new();
        let code = dispatch(
            &["--core".to_string(), tmp.to_str().unwrap().to_string()],
            &mut out,
            &mut err,
        );
        let _ = std::fs::remove_file(&tmp);
        assert_eq!(code, 0, "stderr: {}", String::from_utf8_lossy(&err));
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "returned\n0x8000000000000000\n"
        );
    }
}
