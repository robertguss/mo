#![forbid(unsafe_code)]

//! Thin Phase 2 front-end CLI driver (`mo-fe`) for Mo Core checking/printing,
//! surface parsing/formatting, and fast batch differential verification against
//! C `mo`, Bison `mo-grammar`, and Lean `mocore`.

use std::env;
use std::fs::{self, File};
use std::io::{self, BufWriter, Read, Write};
use std::path::Path;
use std::process::ExitCode;

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, NO_FILE, RenderFormat, SourceError, SourceMap, Span,
    TerminalEncoding, render_diagnostic_default,
};
use mo_core::{
    CoreDocument, CoreTextKind, check_program, detect_kind, parse_document, parse_program,
    parse_run, print_document,
};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_CORE_TEXT_SIZE, MO_LIMIT_DIAGNOSTICS, MO_LIMIT_FILE_SIZE, MO_LIMIT_PATH_SIZE,
};
use mo_prim::unicode16::{display_width_bytes, next_grapheme_bytes};
use mo_syntax::{FormatOptions, format_module, parse_fragment, parse_module, read_header};

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
        "  mo-fe parse [--fragment|--header] [--json] [FILE.mo|-]"
    )?;
    writeln!(w, "  mo-fe format [--stdout|--check] [--json] [FILE.mo|-]")?;
    writeln!(w, "  mo-fe parse-batch [--fragment|--header] [--] FILE...")?;
    writeln!(w, "  mo-fe format-batch [--fragment] [--] FILE...")
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

fn dispatch(args: &[String], out: impl Write, mut err: impl Write) -> u8 {
    let Some(cmd) = args.first().map(String::as_str) else {
        let _ = print_usage(&mut err);
        return 2;
    };
    let rest = &args[1..];
    match cmd {
        "core-check" => run_core_check(rest, err),
        "core-print" => run_core_print(rest, out, err),
        "core-check-batch" => run_core_check_batch(rest, out, err),
        "core-print-batch" => run_core_print_batch(rest, out, err),
        "parse" => run_parse(rest, err),
        "format" => run_format(rest, out, err),
        "parse-batch" => run_parse_batch(rest, out, err),
        "format-batch" => run_format_batch(rest, out, err),
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
    let args: Vec<String> = env::args().skip(1).collect();
    let stdout = io::stdout();
    let stderr = io::stderr();
    let code = dispatch(
        &args,
        BufWriter::new(stdout.lock()),
        BufWriter::new(stderr.lock()),
    );
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
}
