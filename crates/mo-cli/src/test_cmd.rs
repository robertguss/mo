//! Thin `mo-cli` adapter over the pure-Rust `mo-test` engine (`crates/mo-test`).

use std::fs;
use std::io::Write;

use mo_base::utf8::is_valid_utf8;
use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_check::{CheckedProgram, LibraryMode, LoadOptions, check_program, load_bundle_with_library};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_BUNDLE_FILES, MO_LIMIT_BUNDLE_PATH_BYTES,
};
use mo_test::{TestConfig, TestSelection, TestTotals};

use crate::cli::Cli;
use crate::fs_util::bytes_to_path;
use crate::identity::MO_VERSION;

/// Builds a [`TestConfig`] from parsed CLI options.
#[must_use]
pub fn test_config_from_cli(cli: &Cli) -> TestConfig {
    TestConfig {
        json: cli.json,
        only: cli.only.clone(),
        module: cli.module.clone(),
        calls: u64::from(cli.calls),
        seconds: u64::from(cli.seconds),
        sequences: u64::from(cli.sequences),
        limits: cli.limits,
        door_plan: cli.door_plan,
        gated: false,
        first_failure: false,
    }
}

/// Selects tests/examples/promises/recovery observations according to `--only` and `--in`.
#[must_use]
pub fn select_tests(
    program: &CheckedProgram,
    cli: &Cli,
    diags: &mut DiagBag,
) -> Option<TestSelection> {
    let cfg = test_config_from_cli(cli);
    mo_test::select_tests(program, &cfg, diags)
}

/// Runs all selected `mo test` observations (plain tests, examples, promises, recovery, service promises).
pub fn run_tests(
    cli: &Cli,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) {
    let cfg = test_config_from_cli(cli);
    mo_test::run_tests(&cfg, program, sources, selection, output, diags, totals);
}

/// Runs `mo test --mutants` (baseline traced run + mutation testing gates).
pub fn run_gates(
    cli: &Cli,
    program: &CheckedProgram,
    sources: &SourceMap,
    selection: &TestSelection,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
) {
    let cfg = test_config_from_cli(cli);
    mo_test::run_gates(&cfg, program, sources, selection, output, diags, totals);
}

/// Lists candidate mutants and per-function counts for `mo test --mutants --list`.
pub fn list_gates(
    program: &CheckedProgram,
    sources: &SourceMap,
    json: bool,
    output: &mut dyn Write,
    diags: &mut DiagBag,
) {
    mo_test::list_gates(program, sources, json, output, diags);
}

/// Runs `mo test --strength FOLDER` against curated wrong versions.
#[expect(
    clippy::too_many_arguments,
    reason = "forwards CLI strength execution state cleanly to mo_test::run_strength"
)]
pub fn run_strength(
    cli: &Cli,
    strength_dir: &str,
    library: &LibraryMode,
    program: &CheckedProgram,
    sources: &SourceMap,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
    version_written: &mut bool,
    mistake: &mut Option<&'static str>,
) {
    let cfg = test_config_from_cli(cli);
    mo_test::run_strength(
        &cfg,
        strength_dir,
        library,
        MO_VERSION,
        program,
        sources,
        output,
        diags,
        totals,
        version_written,
        mistake,
    );
}

fn read_u32_le(bytes: &[u8], pos: &mut usize) -> Option<usize> {
    let end = pos.checked_add(4)?;
    let slice = bytes.get(*pos..end)?;
    *pos = end;
    Some(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]) as usize)
}

fn read_bundle_span<'a>(bytes: &'a [u8], pos: &mut usize, limit: usize) -> Option<&'a [u8]> {
    let len = read_u32_le(bytes, pos)?;
    if len > limit {
        return None;
    }
    let end = pos.checked_add(len)?;
    let slice = bytes.get(*pos..end)?;
    if !is_valid_utf8(slice) {
        return None;
    }
    *pos = end;
    Some(slice)
}

fn read_bundle_path<'a>(bytes: &'a [u8], pos: &mut usize) -> Option<&'a [u8]> {
    let path = read_bundle_span(bytes, pos, MO_LIMIT_BUNDLE_PATH_BYTES as usize)?;
    if path.is_empty() {
        return None;
    }
    let mut start = 0usize;
    for i in 0..=path.len() {
        if i < path.len() && (path[i] == 0 || path[i] == b'\\') {
            return None;
        }
        if i < path.len() && path[i] != b'/' {
            continue;
        }
        let part = &path[start..i];
        if part.is_empty()
            || part == b"."
            || part == b".."
            || (start == 0 && (part == b"lib" || part == b"UpgradeOld" || part == b"UpgradeOld.mo"))
        {
            return None;
        }
        start = i + 1;
    }
    Some(path)
}

fn load_source_bundle(path_bytes: &[u8], sources: &mut SourceMap, diags: &mut DiagBag) -> bool {
    let Ok(bytes) = fs::read(bytes_to_path(path_bytes).as_ref()) else {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeBundleFormat,
            Span::NONE,
            "source bundle is malformed or unreadable",
        ));
        return false;
    };
    let ok = parse_and_add_bundle(&bytes, sources);
    if !ok {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeBundleFormat,
            Span::NONE,
            "source bundle is malformed or unreadable",
        ));
    }
    ok
}

fn parse_and_add_bundle(bytes: &[u8], sources: &mut SourceMap) -> bool {
    if bytes.len() < 13
        || bytes.len() as u64 > MO_LIMIT_BUNDLE_BYTES
        || !bytes.starts_with(b"MOSB\x01")
    {
        return false;
    }
    let mut pos = 5usize;
    let Some(entry) = read_bundle_path(bytes, &mut pos) else {
        return false;
    };
    let Some(count) = read_u32_le(bytes, &mut pos) else {
        return false;
    };
    if count == 0 || count as u64 > MO_LIMIT_BUNDLE_FILES {
        return false;
    }
    let mut files: Vec<(&[u8], &[u8])> = Vec::with_capacity(count);
    let mut entry_idx = None;
    for i in 0..count {
        let Some(fpath) = read_bundle_path(bytes, &mut pos) else {
            return false;
        };
        let Some(fsrc) = read_bundle_span(bytes, &mut pos, MO_LIMIT_BUNDLE_BYTES as usize) else {
            return false;
        };
        if i != 0 && files[i - 1].0 >= fpath {
            return false;
        }
        if fpath == entry {
            entry_idx = Some(i);
        }
        files.push((fpath, fsrc));
    }
    if pos != bytes.len() {
        return false;
    }
    let Some(entry_pos) = entry_idx else {
        return false;
    };
    for n in 0..count {
        let idx = if n == 0 {
            entry_pos
        } else if n <= entry_pos {
            n - 1
        } else {
            n
        };
        let (fpath, fsrc) = files[idx];
        if sources.add_bytes(fpath.to_vec(), fsrc.to_vec()).is_err() {
            return false;
        }
    }
    true
}

/// Runs door-plan checks over a `MOSB\1` source bundle (`mo test --bundle FILE`,
/// `src/tools/upgrade-bundle.c:compile_bundle` + `run_checks`).
pub fn run_bundle_test(
    cli: &Cli,
    library: &LibraryMode,
    sources: &mut SourceMap,
    diags: &mut DiagBag,
) {
    if !load_source_bundle(cli.file(0), sources, diags) {
        return;
    }
    let candidate_count = sources.len();
    let load_opts = LoadOptions {
        library: library.clone(),
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let Some(set) = load_bundle_with_library(sources, &load_opts, diags) else {
        return;
    };
    for m in &set.modules {
        if m.syntax.span.file < candidate_count
            && (m.name == "UpgradeOld" || m.name.starts_with("UpgradeOld."))
        {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::UpgradeBundleFormat,
                Span::NONE,
                "candidate declares the reserved UpgradeOld namespace",
            ));
        }
    }
    if diags.has_any() {
        return;
    }
    let Some(program) = check_program(Some(sources), set, diags) else {
        return;
    };
    if diags.has_any() {
        return;
    }

    let mut run_diags = DiagBag::new();
    let mut results = Vec::new();
    let mut totals = TestTotals::default();
    let selection = select_tests(&program, cli, &mut run_diags);
    if let Some(ref sel) = selection {
        run_tests(
            cli,
            &program,
            sources,
            sel,
            &mut results,
            &mut run_diags,
            &mut totals,
        );
    }
    let failed = totals.failed != 0
        || totals.stopped != 0
        || run_diags
            .items()
            .iter()
            .any(|d| d.code == ErrorCode::ToolTestInitialization);

    let mut first_fail_line = String::new();
    if failed {
        for raw_line in results.split(|&b| b == b'\n') {
            if raw_line.starts_with(b"failed ") || raw_line.starts_with(b"stopped ") {
                let mut len = raw_line.len().min(511);
                while len != 0 && !is_valid_utf8(&raw_line[..len]) {
                    len -= 1;
                }
                if let Ok(s) = std::str::from_utf8(&raw_line[..len]) {
                    first_fail_line = s.to_string();
                }
                break;
            }
        }
    }

    if failed || !run_diags.is_empty() || run_diags.dropped() != 0 || selection.is_none() {
        let fallback_diag;
        let reason: &str = if !first_fail_line.is_empty() {
            &first_fail_line
        } else if let Some(first) = run_diags.items().first() {
            fallback_diag = first.message.as_str_lossy();
            fallback_diag.as_ref()
        } else {
            "a check did not finish"
        };
        let (code, msg) = if failed {
            (ErrorCode::ChecksFailed, "the door's checks failed")
        } else {
            (ErrorCode::PlanUnfinished, "the plan did not finish")
        };
        let _ = diags.push(Diagnostic::new(code, Span::NONE, msg).with_primary_text(reason));
    }
}
