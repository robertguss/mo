#![forbid(unsafe_code)]

//! Thin Phase 2 front-end CLI driver (`mo-fe`) for Mo Core checking, canonical
//! printing, and fast batch differential verification against C and Lean `mocore`.

use std::env;
use std::fs::File;
use std::io::{self, BufWriter, Read, Write};
use std::path::Path;
use std::process::ExitCode;

use mo_base::{
    DiagBag, DisplayMetrics, NO_FILE, RenderFormat, SourceMap, TerminalEncoding,
    render_diagnostic_default,
};
use mo_core::{
    CoreDocument, CoreTextKind, check_program, detect_kind, parse_document, parse_program,
    parse_run, print_document,
};
use mo_def::limits::MO_LIMIT_CORE_TEXT_SIZE;
use mo_prim::unicode16::{display_width_bytes, next_grapheme_bytes};

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
    )
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

fn read_bounded(mut reader: impl Read) -> io::Result<Vec<u8>> {
    let mut buf = Vec::new();
    let max_read = MO_LIMIT_CORE_TEXT_SIZE.saturating_add(1);
    reader.by_ref().take(max_read).read_to_end(&mut buf)?;
    Ok(buf)
}

fn read_input(path: Option<&str>) -> io::Result<(String, Vec<u8>)> {
    match path {
        None | Some("-") => {
            let bytes = read_bounded(io::stdin().lock())?;
            Ok(("<stdin>".to_string(), bytes))
        }
        Some(p) => {
            let file = File::open(Path::new(p))?;
            let bytes = read_bounded(file)?;
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

    let (label, bytes) = match read_input(path) {
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

    let (label, bytes) = match read_input(path) {
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
        let Ok(bytes) = read_bounded(file) else {
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
        let Ok(bytes) = read_bounded(file) else {
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
}
