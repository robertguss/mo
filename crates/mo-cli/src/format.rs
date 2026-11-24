//! `mo format` implementation (`src/tools/format.c` parity).

use std::io::{self, Read, Write};
use std::thread;

use mo_base::{
    DiagBag, Diagnostic, RenderFormat, SourceError, SourceMap, Span, render_diagnostic_default,
};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_DIAGNOSTICS, MO_LIMIT_FILE_SIZE, MO_LIMIT_PATH_SIZE,
    MO_SOURCE_STACK_SIZE,
};
use mo_prim::unicode16::display_width_bytes;
use mo_syntax::{FormatOptions, ParseOptions, format_module, parse_module_with_options};

use crate::cli::Cli;
use crate::fs_util::{bytes_to_path, io_error_message, lstat_regular, replace_file};
use crate::render::{UNICODE16_METRICS, detect_terminal_encoding};

fn format_problem(diags: &mut DiagBag, file: u32, code: ErrorCode, path: &[u8], detail: &str) {
    let limit = MO_LIMIT_PATH_SIZE as usize;
    let mut message = Vec::with_capacity(detail.len() + 2 + path.len().min(limit) + 3);
    message.extend_from_slice(detail.as_bytes());
    message.extend_from_slice(b": ");
    let prefix = path.len().min(limit);
    message.extend_from_slice(&path[..prefix]);
    if path.len() > limit {
        message.extend_from_slice(b"...");
    }
    let diag = Diagnostic::new(code, Span::new(file, 0, 0), message);
    let _ = diags.push(diag);
}

fn run_parse_and_format(
    sources: &SourceMap,
    diags: &mut DiagBag,
    options: FormatOptions,
) -> Result<Option<Vec<u8>>, ()> {
    thread::scope(|scope| {
        let builder = thread::Builder::new()
            .name("mo-format".to_string())
            .stack_size(MO_SOURCE_STACK_SIZE as usize);
        let handle = builder.spawn_scoped(scope, || {
            let parse_opts = ParseOptions {
                arena_limit: MO_CLI_CHECK_SIZE as usize,
            };
            let module = parse_module_with_options(sources, 0, diags, parse_opts)?;
            if diags.has_any() {
                return None;
            }
            match format_module(&module, options) {
                Ok(rendered) if (rendered.len() as u64) <= MO_LIMIT_FILE_SIZE => Some(rendered),
                Ok(_) | Err(_) => {
                    let path = sources.get(0).map_or(&[][..], |f| f.path_bytes());
                    format_problem(
                        diags,
                        0,
                        ErrorCode::ToolFormatLimit,
                        path,
                        "source printing exceeds this build's memory, depth, output or line limit",
                    );
                    None
                }
            }
        });
        match handle {
            Ok(h) => match h.join() {
                Ok(val) => Ok(val),
                Err(_) => Err(()),
            },
            Err(_) => Err(()),
        }
    })
}

fn read_regular_file_bytes(path: &[u8]) -> Result<Vec<u8>, SourceError> {
    if (path.len() as u64) > MO_LIMIT_PATH_SIZE {
        return Err(SourceError::PathTooLong);
    }
    #[cfg(unix)]
    let file = {
        let fd = rustix::fs::open(
            bytes_to_path(path).as_ref(),
            rustix::fs::OFlags::RDONLY | rustix::fs::OFlags::NONBLOCK,
            rustix::fs::Mode::empty(),
        )
        .map_err(|e| SourceError::Unreadable(e.to_string()))?;
        std::fs::File::from(fd)
    };
    #[cfg(not(unix))]
    let file = std::fs::File::open(bytes_to_path(path))
        .map_err(|e| SourceError::Unreadable(e.to_string()))?;

    let meta = file
        .metadata()
        .map_err(|e| SourceError::Unreadable(e.to_string()))?;
    if !meta.file_type().is_file() {
        return Err(SourceError::Unreadable("not a regular file".to_string()));
    }
    let mut buf = Vec::new();
    file.take(MO_LIMIT_FILE_SIZE + 1)
        .read_to_end(&mut buf)
        .map_err(|e| SourceError::Unreadable(e.to_string()))?;
    if (buf.len() as u64) > MO_LIMIT_FILE_SIZE {
        return Err(SourceError::FileTooBig);
    }
    Ok(buf)
}

fn format_one_file(
    path: &[u8],
    options: FormatOptions,
    sources: &mut SourceMap,
    diags: &mut DiagBag,
) {
    if (path.len() as u64) > MO_LIMIT_PATH_SIZE {
        format_problem(
            diags,
            u32::MAX,
            ErrorCode::PathTooLong,
            path,
            "the source path exceeds this build's limit",
        );
        return;
    }
    // Check via lstat before opening: a FIFO or device must never block the command.
    let Some(original) = lstat_regular(path) else {
        format_problem(
            diags,
            u32::MAX,
            ErrorCode::ToolFormatIo,
            path,
            "format needs a readable regular file, not a symlink or special file",
        );
        return;
    };
    let raw = match read_regular_file_bytes(path) {
        Ok(bytes) => bytes,
        Err(err) => {
            let code = match err {
                SourceError::Unreadable(_) => ErrorCode::ToolFormatIo,
                SourceError::FileTooBig => ErrorCode::FileTooBig,
                SourceError::PathTooLong => ErrorCode::PathTooLong,
                SourceError::ProgramTooBig | SourceError::TooManyFiles => {
                    ErrorCode::ToolFormatLimit
                }
            };
            format_problem(
                diags,
                u32::MAX,
                code,
                path,
                "the source file could not be read for formatting",
            );
            return;
        }
    };
    let file_id = match sources.add_bytes(path.to_vec(), raw) {
        Ok(id) => id,
        Err(err) => {
            let code = match err {
                SourceError::Unreadable(_) => ErrorCode::ToolFormatIo,
                SourceError::FileTooBig => ErrorCode::FileTooBig,
                SourceError::PathTooLong => ErrorCode::PathTooLong,
                SourceError::ProgramTooBig | SourceError::TooManyFiles => {
                    ErrorCode::ToolFormatLimit
                }
            };
            format_problem(
                diags,
                u32::MAX,
                code,
                path,
                "the source file could not be read for formatting",
            );
            return;
        }
    };
    let rendered = match run_parse_and_format(sources, diags, options) {
        Ok(val) => val,
        Err(()) => {
            format_problem(
                diags,
                file_id,
                ErrorCode::ToolFormatLimit,
                path,
                "the source stack could not be prepared for formatting",
            );
            return;
        }
    };
    if let Some(output_bytes) = rendered
        && let Some(src) = sources.get(file_id)
        && output_bytes.as_slice() != src.bytes()
        && let Err(reason) = replace_file(src.path_bytes(), &output_bytes, &original)
    {
        let detail = format!("could not replace the formatted file ({reason})");
        format_problem(diags, file_id, ErrorCode::ToolFormatIo, path, &detail);
    }
}

fn write_format_diags(
    sources: &SourceMap,
    diags: &mut DiagBag,
    errors: &mut dyn Write,
    shown: &mut u64,
    dropped: &mut u64,
) -> bool {
    let encoding = detect_terminal_encoding();
    *dropped += u64::from(diags.dropped());
    diags.sort();
    for diag in diags.items() {
        if *shown == MO_LIMIT_DIAGNOSTICS {
            *dropped += 1;
            continue;
        }
        match render_diagnostic_default(
            sources,
            diag,
            RenderFormat::Text,
            encoding,
            UNICODE16_METRICS,
        ) {
            Ok(rendered) => {
                if errors.write_all(&rendered).is_err() || errors.flush().is_err() {
                    return false;
                }
                *shown += 1;
            }
            Err(_) => {
                *dropped += 1;
            }
        }
    }
    true
}

fn format_stdin(options: FormatOptions, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();

    let mut input = Vec::new();
    let read_res = io::stdin()
        .take(MO_LIMIT_FILE_SIZE + 1)
        .read_to_end(&mut input);

    let mut wrote_out = true;
    match read_res {
        Err(err) => {
            let _ = io_error_message(&err);
            format_problem(
                &mut diags,
                u32::MAX,
                ErrorCode::ToolFormatIo,
                b"<stdin>",
                "standard input could not be read for formatting",
            );
        }
        Ok(_) if (input.len() as u64) > MO_LIMIT_FILE_SIZE => {
            format_problem(
                &mut diags,
                u32::MAX,
                ErrorCode::FileTooBig,
                b"<stdin>",
                "the source file could not be read for formatting",
            );
        }
        Ok(_) => match sources.add_bytes(b"<stdin>".to_vec(), input) {
            Err(err) => {
                let code = match err {
                    SourceError::FileTooBig => ErrorCode::FileTooBig,
                    _ => ErrorCode::ToolFormatLimit,
                };
                format_problem(
                    &mut diags,
                    u32::MAX,
                    code,
                    b"<stdin>",
                    "the source file could not be read for formatting",
                );
            }
            Ok(file_id) => match run_parse_and_format(&sources, &mut diags, options) {
                Err(()) => {
                    format_problem(
                        &mut diags,
                        file_id,
                        ErrorCode::ToolFormatLimit,
                        b"<stdin>",
                        "the source stack could not be prepared for formatting",
                    );
                }
                Ok(Some(formatted)) if !diags.has_any() => {
                    if output.write_all(&formatted).is_err() || output.flush().is_err() {
                        wrote_out = false;
                        format_problem(
                            &mut diags,
                            file_id,
                            ErrorCode::ToolFormatIo,
                            b"<stdin>",
                            "formatted output could not be written",
                        );
                    }
                }
                Ok(_) => {}
            },
        },
    }

    let failed = !wrote_out || diags.has_any();
    let mut shown = 0u64;
    let mut dropped = 0u64;
    if !write_format_diags(&sources, &mut diags, errors, &mut shown, &mut dropped) {
        return 1;
    }
    if dropped != 0
        && (writeln!(errors, "mo: {dropped} diagnostics could not be shown").is_err()
            || errors.flush().is_err())
    {
        return 1;
    }
    if failed || dropped != 0 { 1 } else { 0 }
}

/// Runs `mo format` on stdin or on each independent file in `cli.files`
/// (`src/tools/format.c:mo_format_files`).
#[must_use]
pub fn format_files(cli: &Cli, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let options = FormatOptions::new(100, display_width_bytes);
    if cli.stdin_mode {
        return format_stdin(options, output, errors);
    }
    let mut shown = 0u64;
    let mut dropped = 0u64;
    let mut failed = false;
    for path in &cli.files {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        format_one_file(path, options, &mut sources, &mut diags);
        failed = failed || diags.has_any();
        if !write_format_diags(&sources, &mut diags, errors, &mut shown, &mut dropped) {
            return 1;
        }
    }
    if dropped != 0
        && (writeln!(errors, "mo: {dropped} diagnostics could not be shown").is_err()
            || errors.flush().is_err())
    {
        return 1;
    }
    if failed || dropped != 0 { 1 } else { 0 }
}
