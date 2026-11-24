//! `mo fix` implementation (`src/tools/fix.c` & `src/tools/execute.c:fix_files` parity).

use std::io::Write;

use mo_base::{DiagBag, Diagnostic, Edit, Fix, SourceMap, Span, edits_overlap};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_FILE_SIZE, MO_LIMIT_FIX_ROUND_EDITS, MO_LIMIT_PATH_SIZE};
use thiserror::Error;

use crate::cli::{Cli, Command};
use crate::fs_util::{FileIdentity, replace_file, stat_identity};
use crate::render::write_diagnostics;

/// Error returned when applying fixes produces a file exceeding `MO_LIMIT_FILE_SIZE`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
#[error("the fixed file exceeds this build's file size limit")]
pub struct FixSizeLimitError;

fn fix_fits(fix: &Fix, file: u32, length: u32, taken: &[&Edit]) -> bool {
    let max_edits = MO_LIMIT_FIX_ROUND_EDITS as usize;
    if fix.edits.len() > max_edits.saturating_sub(taken.len()) {
        return false;
    }
    for edit in &fix.edits {
        if edit.span.file != file {
            return false;
        }
        debug_assert!(edit.span.start <= edit.span.end && edit.span.end <= length);
        if taken.iter().any(|prev| edits_overlap(edit, prev)) {
            return false;
        }
    }
    true
}

/// Applies every non-overlapping single-fix diagnostic targeting `file` into a fresh byte buffer,
/// matching `src/tools/fix.c:mo_fix_source`.
///
/// Returns `Ok((fixed_bytes, applied_fix_count))`, or `Err(FixSizeLimitError)` if the
/// resulting buffer would exceed `MO_LIMIT_FILE_SIZE`.
pub fn fix_source(
    sources: &SourceMap,
    file: u32,
    diags: &mut DiagBag,
) -> Result<(Vec<u8>, u32), FixSizeLimitError> {
    let Some(source) = sources.get(file) else {
        return Ok((Vec::new(), 0));
    };
    let src_bytes = source.bytes();
    let src_len = src_bytes.len() as u32;
    diags.sort();

    let mut taken: Vec<&Edit> = Vec::new();
    let mut applied = 0u32;
    for diag in diags.items() {
        if diag.fixes.len() != 1 {
            continue;
        }
        let fix = &diag.fixes[0];
        if !fix_fits(fix, file, src_len, &taken) {
            continue;
        }
        for edit in &fix.edits {
            taken.push(edit);
        }
        applied += 1;
    }

    taken.sort_by_key(|e| (e.span.start, e.span.end));

    let max_size = MO_LIMIT_FILE_SIZE as usize;
    let mut out = Vec::with_capacity(src_bytes.len().min(max_size));
    let mut at = 0usize;
    for edit in taken {
        let start = edit.span.start as usize;
        let end = edit.span.end as usize;
        let prefix = &src_bytes[at..start];
        let repl = edit.replacement.as_bytes();
        if prefix.len() > max_size.saturating_sub(out.len()) {
            return Err(FixSizeLimitError);
        }
        out.extend_from_slice(prefix);
        if repl.len() > max_size.saturating_sub(out.len()) {
            return Err(FixSizeLimitError);
        }
        out.extend_from_slice(repl);
        at = end;
    }
    let tail = &src_bytes[at..];
    if tail.len() > max_size.saturating_sub(out.len()) {
        return Err(FixSizeLimitError);
    }
    out.extend_from_slice(tail);
    Ok((out, applied))
}

/// Applies fixes for source 0 and atomically replaces `path`.
///
/// Returns `Ok(true)` if rewritten, `Ok(false)` if no fix applied, or `Err(problem)`
/// describing why rewriting failed.
pub fn apply_fixes(
    path: &[u8],
    original: &FileIdentity,
    sources: &SourceMap,
    diags: &mut DiagBag,
) -> Result<bool, String> {
    if sources.is_empty() {
        return Ok(false);
    }
    let (fixed, applied) = fix_source(sources, 0, diags).map_err(|e| e.to_string())?;
    if applied == 0 {
        return Ok(false);
    }
    match replace_file(path, &fixed, original) {
        Ok(()) => Ok(true),
        Err(reason) => Err(format!("could not replace the fixed file ({reason})")),
    }
}

/// Reports `E1407` alone for a file that `mo fix` cannot read or replace
/// (`src/tools/execute.c:refuse_fix`).
#[must_use]
pub fn refuse_fix(
    path: &[u8],
    detail: &str,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    let sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let limit = MO_LIMIT_PATH_SIZE as usize;
    let mut message = Vec::with_capacity(detail.len() + 2 + path.len().min(limit) + 3);
    message.extend_from_slice(detail.as_bytes());
    message.extend_from_slice(b": ");
    let prefix = path.len().min(limit);
    message.extend_from_slice(&path[..prefix]);
    if path.len() > limit {
        message.extend_from_slice(b"...");
    }
    let _ = diags.push(Diagnostic::new(ErrorCode::ToolFixIo, Span::NONE, message));
    write_diagnostics(
        output,
        errors,
        false,
        Command::Fix,
        &sources,
        &mut diags,
        None,
    )
}

/// Filters out diagnostics whose primary file is another file passed on the `mo fix` command line
/// (matched by `(dev, ino)` so different paths to the same file deduplicate),
/// matching `src/tools/execute.c:leave_given`.
pub fn leave_given(
    cli: &Cli,
    file_index: usize,
    given: &[(u64, u64)],
    sources: &SourceMap,
    diags: &mut DiagBag,
) {
    diags.retain(|diag| {
        let source = diag.primary.span.file;
        if source == 0 || source >= sources.len() {
            return true;
        }
        let Some(src) = sources.get(source) else {
            return true;
        };
        let Some((dev, ino)) = stat_identity(src.path_bytes()) else {
            return true;
        };
        for (j, &(gdev, gino)) in given.iter().enumerate() {
            if j != file_index && j < cli.files.len() && gdev == dev && gino == ino {
                return false;
            }
        }
        true
    });
}
