#![forbid(unsafe_code)]

//! Thin CLI driver for the pure-Rust Mo runtime:
//!   - `mo-rt verify FILE.bc`
//!   - `mo-rt anchors FILE.bc`
//!   - `mo-rt run [--root DIR] [--state-dir DIR] [--max-heap N] [--max-stack N] [--max-depth N] FILE.bc [-- args...]`
//!   - `mo-rt test [--root DIR] [--state-dir DIR] [--max-heap N] [--max-stack N] [--max-depth N] [--only NAME]... [--allow-empty] FILE.bc`

use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use mo_bytecode::{AnchorExpectation, BytecodeFile, FunctionKind, NO_INDEX};
use mo_def::limits::MO_CLI_ANCHOR_INSTRUCTIONS;
use mo_host::HostContext;
use mo_vm::{Defect, Image, Limits, Outcome, Process, render_defect};

fn parse_bytes_limit(s: &str) -> Result<usize, String> {
    let idx = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    if idx == 0 {
        return Err(format!("invalid byte limit: {s:?}"));
    }
    let num: u64 = s[..idx]
        .parse()
        .map_err(|_| format!("invalid byte limit: {s:?}"))?;
    if num == 0 {
        return Err(format!("limit must be positive: {s:?}"));
    }
    let scale: u64 = match &s[idx..] {
        "" | "B" => 1,
        "KiB" => 1 << 10,
        "MiB" => 1 << 20,
        "GiB" => 1 << 30,
        _ => return Err(format!("invalid byte suffix in {s:?}")),
    };
    let total = num
        .checked_mul(scale)
        .ok_or_else(|| format!("byte limit overflow: {s:?}"))?;
    usize::try_from(total).map_err(|_| format!("byte limit overflow: {s:?}"))
}

fn parse_count_limit(s: &str) -> Result<u32, String> {
    let n: u32 = s
        .parse()
        .map_err(|_| format!("invalid count limit: {s:?}"))?;
    if n == 0 {
        return Err(format!("limit must be positive: {s:?}"));
    }
    Ok(n)
}

fn load_bc_file(path: &str) -> Result<BytecodeFile, String> {
    let bytes = fs::read(path).map_err(|e| format!("could not read {path}: {e}"))?;
    mo_bytecode::load(&bytes).map_err(|e| format!("bytecode verification failed: {e}"))
}

fn cmd_verify(args: &[String]) -> ExitCode {
    if args.len() != 1 {
        eprintln!("usage: mo-rt verify FILE.bc");
        return ExitCode::from(2);
    }
    match load_bc_file(&args[0]) {
        Ok(_) => ExitCode::SUCCESS,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            ExitCode::from(1)
        }
    }
}

struct CliOptions {
    limits: Limits,
    root: Option<PathBuf>,
    state_dir: Option<PathBuf>,
    only: Vec<String>,
    allow_empty: bool,
    path: String,
    prog_args: Vec<String>,
}

fn parse_run_or_test_args(
    args: &[String],
    allow_program_args: bool,
    allow_test_flags: bool,
) -> Result<CliOptions, String> {
    let mut limits = Limits::default();
    let mut root: Option<PathBuf> = None;
    let mut state_dir: Option<PathBuf> = None;
    let mut only: Vec<String> = Vec::new();
    let mut allow_empty = false;
    let mut i = 0usize;
    let mut file: Option<String> = None;
    let mut prog_args: Vec<String> = Vec::new();

    while i < args.len() {
        let arg = &args[i];
        if file.is_none() {
            match arg.as_str() {
                "--root" => {
                    i += 1;
                    let val = args.get(i).ok_or("--root needs a directory path")?;
                    root = Some(PathBuf::from(val));
                }
                "--state-dir" => {
                    i += 1;
                    let val = args.get(i).ok_or("--state-dir needs a directory path")?;
                    state_dir = Some(PathBuf::from(val));
                }
                "--max-heap" => {
                    i += 1;
                    let val = args.get(i).ok_or("--max-heap needs a value")?;
                    limits.heap_size = parse_bytes_limit(val)?;
                }
                "--max-stack" => {
                    i += 1;
                    let val = args.get(i).ok_or("--max-stack needs a value")?;
                    limits.stack_size = parse_bytes_limit(val)?;
                }
                "--max-depth" => {
                    i += 1;
                    let val = args.get(i).ok_or("--max-depth needs a value")?;
                    limits.call_depth = parse_count_limit(val)?;
                }
                "--only" if allow_test_flags => {
                    i += 1;
                    let val = args.get(i).ok_or("--only needs a test or example name")?;
                    only.push(val.clone());
                }
                "--allow-empty" if allow_test_flags => {
                    allow_empty = true;
                }
                "--" => {
                    i += 1;
                    let f = args.get(i).ok_or("expected FILE.bc")?;
                    file = Some(f.clone());
                }
                other if other.starts_with('-') => {
                    return Err(format!("unknown flag: {other}"));
                }
                other => {
                    file = Some(other.to_string());
                }
            }
            i += 1;
        } else {
            if !allow_program_args {
                return Err(format!("unexpected argument: {arg}"));
            }
            if arg == "--" && prog_args.is_empty() {
                prog_args.extend_from_slice(&args[i + 1..]);
            } else {
                prog_args.extend_from_slice(&args[i..]);
            }
            break;
        }
    }

    let path = file.ok_or("expected FILE.bc")?;
    Ok(CliOptions {
        limits,
        root,
        state_dir,
        only,
        allow_empty,
        path,
        prog_args,
    })
}

fn check_anchors(image: &Image, limits: Limits) -> Result<usize, String> {
    let anchors = image.anchor_functions();
    let file = image.file();
    for &(fn_idx, expectation) in &anchors {
        let name = file.function_name(fn_idx);
        let allows = expectation == AnchorExpectation::Allows;
        let host = HostContext::stdio(Vec::new(), Vec::new());
        let mut process = Process::new(image, limits, host);
        match process.call_bounded(fn_idx, Vec::new(), MO_CLI_ANCHOR_INSTRUCTIONS) {
            Outcome::Returned(val) => {
                let answer = val.as_int();
                if (answer <= 0) != allows {
                    return Err(if allows {
                        format!(
                            "anchor failed: allows anchor {name} was rejected (condition {answer})"
                        )
                    } else {
                        format!("anchor failed: forbids anchor {name} was accepted")
                    });
                }
            }
            Outcome::Failed(defect) | Outcome::Stopped(defect) => {
                let report = render_defect(file, &defect);
                return Err(format!("anchor stopped ({name}):\n{report}"));
            }
        }
    }
    Ok(anchors.len())
}

fn cmd_anchors(args: &[String]) -> ExitCode {
    let opts = match parse_run_or_test_args(args, false, false) {
        Ok(v) => v,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            return ExitCode::from(2);
        }
    };
    let bc = match load_bc_file(&opts.path) {
        Ok(f) => f,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            return ExitCode::from(1);
        }
    };
    if Image::anchor_functions_in_file(&bc).is_empty() {
        println!("0 anchors passed");
        return ExitCode::SUCCESS;
    }
    let image = match Image::init_bounded(bc, opts.limits, MO_CLI_ANCHOR_INSTRUCTIONS) {
        Ok(img) => img,
        Err(err) => {
            let (file, defect) = *err;
            let report = render_defect(&file, &defect);
            let _ = io::stderr().write_all(report.as_bytes());
            return ExitCode::from(70);
        }
    };
    match check_anchors(&image, opts.limits) {
        Ok(count) => {
            println!("{count} anchors passed");
            ExitCode::SUCCESS
        }
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            ExitCode::from(1)
        }
    }
}

fn cmd_run(args: &[String]) -> ExitCode {
    let opts = match parse_run_or_test_args(args, true, false) {
        Ok(v) => v,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            return ExitCode::from(2);
        }
    };

    let bc = match load_bc_file(&opts.path) {
        Ok(f) => f,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            return ExitCode::from(1);
        }
    };

    let env_vars: Vec<(String, String)> = env::vars().collect();
    let host = match make_host(
        opts.prog_args,
        env_vars,
        opts.root.as_deref(),
        opts.state_dir.as_deref(),
    ) {
        Ok(h) => h,
        Err(msg) => {
            eprintln!("mo-rt: could not open folder: {msg}");
            return ExitCode::from(1);
        }
    };

    let image = match Image::init(bc, opts.limits) {
        Ok(img) => img,
        Err(err) => {
            let (file, defect) = *err;
            let report = render_defect(&file, &defect);
            let _ = io::stderr().write_all(report.as_bytes());
            return ExitCode::from(70);
        }
    };

    let Some(main_fn) = image.find_main_entry() else {
        eprintln!("mo-rt: bytecode image has no main entry function");
        return ExitCode::from(1);
    };

    let mut process = Process::new(&image, opts.limits, host);
    match process.call_main(main_fn) {
        Outcome::Returned(val) => {
            let status = val.as_int();
            if (0..=255).contains(&status) {
                ExitCode::from(status as u8)
            } else {
                eprintln!("bug: exit status outside 0 to 255");
                ExitCode::from(70)
            }
        }
        Outcome::Failed(defect) | Outcome::Stopped(defect) => {
            let report = render_defect(image.file(), &defect);
            let _ = io::stderr().write_all(report.as_bytes());
            ExitCode::from(70)
        }
    }
}

#[cfg(unix)]
fn make_host(
    prog_args: Vec<String>,
    env_vars: Vec<(String, String)>,
    root: Option<&Path>,
    state_dir: Option<&Path>,
) -> Result<HostContext, String> {
    HostContext::stdio_with_roots(prog_args, env_vars, root, state_dir)
}

#[cfg(not(unix))]
fn make_host(
    prog_args: Vec<String>,
    env_vars: Vec<(String, String)>,
    _root: Option<&Path>,
    _state_dir: Option<&Path>,
) -> Result<HostContext, String> {
    Ok(HostContext::stdio(prog_args, env_vars))
}

/// Computes 1-based (line, column) from byte offset, matching `src/base/diag.c:mo_sources_position`.
fn byte_offset_to_line_col(source: &[u8], offset: usize) -> (usize, usize) {
    let clamped = offset.min(source.len());
    let mut line = 1usize;
    let mut line_start = 0usize;
    for (i, &b) in source[..clamped].iter().enumerate() {
        if b == b'\n' {
            line += 1;
            line_start = i + 1;
        }
    }
    let prefix = &source[line_start..clamped];
    let col = match std::str::from_utf8(prefix) {
        Ok(s) => s.chars().count() + 1,
        Err(_) => prefix.len() + 1,
    };
    (line, col)
}

fn format_location(file: &BytecodeFile, file_idx: u32, start: u32) -> String {
    if file_idx == NO_INDEX || (file_idx as usize) >= file.files.len() {
        return "<unavailable>".to_string();
    }
    let path_bytes = &file.files[file_idx as usize];
    let path_str = String::from_utf8_lossy(path_bytes);
    if let Ok(source) = fs::read(Path::new(path_str.as_ref())) {
        let (line, col) = byte_offset_to_line_col(&source, start as usize);
        format!("{path_str}:{line}:{col}")
    } else {
        format!("{path_str}:{start}")
    }
}

fn parse_equality_detail(detail: &str) -> Option<(&str, &str)> {
    let rest = detail.strip_prefix("left ")?;
    let colon1 = rest.find(':')?;
    let len1: usize = rest[..colon1].parse().ok()?;
    let after1 = &rest[colon1 + 1..];
    if after1.len() < len1 + 1 || !after1.as_bytes()[len1].eq(&b',') {
        return None;
    }
    let left = &after1[..len1];
    let rest2 = after1[len1 + 1..].strip_prefix("right ")?;
    let colon2 = rest2.find(':')?;
    let len2: usize = rest2[..colon2].parse().ok()?;
    let after2 = &rest2[colon2 + 1..];
    if after2.len() != len2 + 1 || !after2.as_bytes()[len2].eq(&b',') {
        return None;
    }
    let right = &after2[..len2];
    Some((left, right))
}

fn render_source_snippet(
    file: &BytecodeFile,
    file_idx: u32,
    start: u32,
    end: u32,
    extend_assert: bool,
    label: &str,
) -> Option<String> {
    if file_idx == NO_INDEX || (file_idx as usize) >= file.files.len() {
        return None;
    }
    let path_str = String::from_utf8_lossy(&file.files[file_idx as usize]);
    let source = fs::read(Path::new(path_str.as_ref())).ok()?;
    let s = std::str::from_utf8(&source).ok()?;
    let mut start_u = (start as usize).min(s.len());
    let end_u = (end as usize).min(s.len());

    // Find line byte boundaries.
    let mut line_starts = vec![0usize];
    for (i, &b) in source.iter().enumerate() {
        if b == b'\n' {
            line_starts.push(i + 1);
        }
    }

    if extend_assert {
        // Match C `mo test` (`assertion_span`), which underlines the full `assert <expr>`
        // statement rather than only the `<expr>` condition span stored on `FAIL`.
        let (line_no, _) = byte_offset_to_line_col(&source, start_u);
        let lstart = line_starts[line_no - 1];
        let before = &source[lstart..start_u];
        let trimmed = before.trim_ascii_end();
        if let Some(prefix) = trimmed.strip_suffix(b"assert")
            && (prefix.is_empty() || prefix.last().is_some_and(u8::is_ascii_whitespace))
            && trimmed.len() < before.len()
        {
            start_u = lstart + prefix.len();
        }
    }

    let (first_line, _) = byte_offset_to_line_col(&source, start_u);
    let last_offset = if end_u > start_u { end_u - 1 } else { end_u };
    let (last_line, _) = byte_offset_to_line_col(&source, last_offset);

    let mut out = String::new();
    for line_no in first_line..=last_line {
        let lstart = line_starts[line_no - 1];
        let mut lend = if line_no < line_starts.len() {
            line_starts[line_no] - 1
        } else {
            source.len()
        };
        if lend > lstart && source[lend - 1] == b'\r' {
            lend -= 1;
        }
        let line_text = &s[lstart..lend];
        let mark_start = start_u.saturating_sub(lstart).min(line_text.len());
        let mark_end = if end_u < lend {
            end_u.saturating_sub(lstart)
        } else {
            line_text.len()
        };
        let col_start = line_text[..mark_start].chars().count();
        let col_end = line_text[..mark_end].chars().count().max(col_start + 1);

        let num_str = line_no.to_string();
        let pad = " ".repeat(num_str.len());
        out.push_str(&format!("  {num_str} | {line_text}\n"));
        out.push_str(&format!(
            "  {pad} | {}{}",
            " ".repeat(col_start),
            "^".repeat(col_end - col_start)
        ));
        if line_no == last_line {
            out.push(' ');
            out.push_str(label);
        }
        out.push('\n');
    }
    Some(out)
}

fn format_test_failure(
    file: &BytecodeFile,
    fn_idx: u32,
    kind: FunctionKind,
    defect: &Defect,
) -> String {
    let mut out = String::new();
    if let Some(detail) = &defect.detail
        && let Some((left, right)) = parse_equality_detail(detail)
    {
        out.push_str(&format!("  left: {left}\n  right: {right}\n"));
    }
    let dbg = &file.debug[fn_idx as usize];
    if kind == FunctionKind::Example {
        if let Some(snippet) = render_source_snippet(
            file,
            dbg.file,
            dbg.start,
            dbg.end,
            false,
            "this example was false",
        ) {
            out.push_str(&snippet);
        }
    } else if let Some(top) = defect.frames.first()
        && let Some((fidx, start, end)) = top.span
        && let Some(snippet) =
            render_source_snippet(file, fidx, start, end, true, "this assertion was false")
    {
        out.push_str(&snippet);
    }
    out
}

fn format_test_stopped(file: &BytecodeFile, defect: &Defect) -> String {
    let mut out = String::new();
    out.push_str(&format!("  bug: {}", defect.reason));
    if let Some(limit_name) = defect.limit_name {
        out.push_str(&format!(" ({limit_name} = {})", defect.limit));
    }
    out.push('\n');
    if let Some(detail) = &defect.detail {
        out.push_str(&format!("  detail: {detail}\n"));
    }
    for frame in &defect.frames {
        let name = file.function_name(frame.function);
        let loc = match frame.span {
            Some((fidx, start, _)) => format_location(file, fidx, start),
            None => "<unavailable>".to_string(),
        };
        out.push_str(&format!("  at {name}  {loc}\n"));
    }
    if defect.variables_unavailable {
        out.push_str("  <variables unavailable>\n");
    } else {
        for var in &defect.variables {
            let val = if var.truncated {
                "<value unavailable>"
            } else {
                var.value.as_str()
            };
            out.push_str(&format!("  {} = {val}\n", var.name));
        }
    }
    out
}

fn cmd_test(args: &[String]) -> ExitCode {
    let opts = match parse_run_or_test_args(args, false, true) {
        Ok(v) => v,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            return ExitCode::from(2);
        }
    };

    if (opts.root.is_some() || opts.state_dir.is_some())
        && let Err(msg) = make_host(
            Vec::new(),
            Vec::new(),
            opts.root.as_deref(),
            opts.state_dir.as_deref(),
        )
    {
        eprintln!("mo-rt: could not open folder: {msg}");
        return ExitCode::from(1);
    }

    let bc = match load_bc_file(&opts.path) {
        Ok(f) => f,
        Err(msg) => {
            eprintln!("mo-rt: {msg}");
            return ExitCode::from(1);
        }
    };

    let image = match Image::init(bc, opts.limits) {
        Ok(img) => img,
        Err(err) => {
            let (file, defect) = *err;
            let report = render_defect(&file, &defect);
            let _ = io::stderr().write_all(report.as_bytes());
            return ExitCode::from(70);
        }
    };

    if let Err(msg) = check_anchors(&image, opts.limits) {
        eprintln!("mo-rt: {msg}");
        return ExitCode::from(1);
    }

    let file = image.file();
    let all_tests = image.test_and_example_functions();
    let tests: Vec<u32> = if opts.only.is_empty() {
        all_tests
    } else {
        let mut matched = vec![false; opts.only.len()];
        let mut selected = Vec::new();
        for fn_idx in all_tests {
            let name = file.string(file.functions[fn_idx as usize].name);
            let mut keep = false;
            for (j, want) in opts.only.iter().enumerate() {
                if name == want {
                    matched[j] = true;
                    keep = true;
                }
            }
            if keep {
                selected.push(fn_idx);
            }
        }
        if let Some((j, _)) = matched.iter().enumerate().find(|(_, m)| !**m) {
            eprintln!(
                "mo-rt: --only does not name a test or example in bytecode: {}",
                opts.only[j]
            );
            return ExitCode::from(2);
        }
        selected
    };

    if tests.is_empty() {
        if opts.allow_empty && opts.only.is_empty() {
            println!("0 passed, 0 failed, 0 stopped");
            return ExitCode::SUCCESS;
        }
        eprintln!("mo-rt: bytecode contains no tests or examples");
        return ExitCode::from(2);
    }

    let mut passed = 0u32;
    let mut failed = 0u32;
    let mut stopped = 0u32;
    let stdout = io::stdout();
    let mut out = stdout.lock();

    for fn_idx in tests {
        let fn_def = &file.functions[fn_idx as usize];
        let dbg = &file.debug[fn_idx as usize];
        let name = file.string(fn_def.name);
        let loc = format_location(file, dbg.file, dbg.start);

        let host = match make_host(
            Vec::new(),
            Vec::new(),
            opts.root.as_deref(),
            opts.state_dir.as_deref(),
        ) {
            Ok(h) => h,
            Err(msg) => {
                eprintln!("mo-rt: could not open folder: {msg}");
                return ExitCode::from(1);
            }
        };
        let mut process = Process::new(&image, opts.limits, host);
        match process.call(fn_idx, Vec::new()) {
            Outcome::Returned(_) => {
                passed += 1;
                let _ = writeln!(out, "passed {name}  {loc}");
            }
            Outcome::Failed(defect) => {
                failed += 1;
                let _ = writeln!(out, "failed {name}  {loc}");
                let extra = format_test_failure(file, fn_idx, fn_def.kind, &defect);
                let _ = write!(out, "{extra}");
            }
            Outcome::Stopped(defect) => {
                stopped += 1;
                let _ = writeln!(out, "stopped {name}  {loc}");
                let extra = format_test_stopped(file, &defect);
                let _ = write!(out, "{extra}");
            }
        }
    }

    let _ = writeln!(out, "{passed} passed, {failed} failed, {stopped} stopped");
    if failed == 0 && stopped == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::from(1)
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        eprintln!("usage: mo-rt <verify|anchors|run|test> ...");
        return ExitCode::from(2);
    }
    match args[0].as_str() {
        "verify" => cmd_verify(&args[1..]),
        "anchors" => cmd_anchors(&args[1..]),
        "run" => cmd_run(&args[1..]),
        "test" => cmd_test(&args[1..]),
        other => {
            eprintln!("mo-rt: unknown command {other:?} (expected verify, anchors, run, or test)");
            ExitCode::from(2)
        }
    }
}
