//! `mo run` execution for both compiled `.bc` files and lowered source programs
//! (`src/tools/run.c` parity).

use std::env;
use std::fs::File;
use std::io::{Read, Write};
use std::path::PathBuf;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_bytecode::{BytecodeFile, Capability, FunctionKind, TypeKind, load as load_bytecode};
use mo_check::CheckedProgram;
use mo_core::{Metadata, Program as CoreProgram};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_ANCHOR_INSTRUCTIONS, MO_LIMIT_BYTECODE_SIZE, MO_LIMIT_DEFECT_TEXT_SIZE,
};
use mo_host::HostContext;
use mo_vm::{
    Defect, Image, Outcome, Process, find_actor_service_for_param, is_actor_service,
    render_defect_raw,
};

use crate::anchors::{check_anchors, program_has_anchors};
use crate::cli::{Cli, Command};
use crate::fs_util::{bytes_to_path, io_error_message};
use crate::render::write_defect;

fn bytecode_error(diags: &mut DiagBag, code: ErrorCode, message: &str, detail: Option<&str>) {
    let mut diag = Diagnostic::new(code, Span::NONE, message);
    if let Some(d) = detail {
        diag = diag.with_primary_text(d);
    }
    if code.info().section.is_some() {
        diag = diag.with_reason("main declares the program's entry (reference 11.2).");
    }
    let _ = diags.push(diag);
}

/// Returns whether `path` is a `.bc` file by suffix or by `MOBC` 4-byte magic on a regular file
/// (`src/tools/run.c:mo_is_bytecode_path`). Never blocks on FIFOs or device nodes.
#[must_use]
pub fn is_bytecode_path(path: &[u8]) -> bool {
    if path.ends_with(b".bc") {
        return true;
    }
    #[cfg(unix)]
    let file = {
        let Ok(fd) = rustix::fs::open(
            bytes_to_path(path).as_ref(),
            rustix::fs::OFlags::RDONLY
                | rustix::fs::OFlags::CLOEXEC
                | rustix::fs::OFlags::NOCTTY
                | rustix::fs::OFlags::NONBLOCK,
            rustix::fs::Mode::empty(),
        ) else {
            return false;
        };
        File::from(fd)
    };
    #[cfg(not(unix))]
    let file = match File::open(bytes_to_path(path)) {
        Ok(f) => f,
        Err(_) => return false,
    };

    let Ok(meta) = file.metadata() else {
        return false;
    };
    if !meta.file_type().is_file() {
        return false;
    }
    let mut magic = [0u8; 4];
    let mut reader = file;
    reader.read_exact(&mut magic).is_ok() && &magic == b"MOBC"
}

/// Validates that `main_fn` satisfies `main`'s capability entry contract
/// (`src/tools/run.c:valid_bytecode_entry`).
#[must_use]
pub fn valid_bytecode_entry(file: &BytecodeFile, main_fn: u32) -> bool {
    let Some(func) = file.functions.get(main_fn as usize) else {
        return false;
    };
    if func.kind != FunctionKind::Function || file.types.get(func.result_type).kind != TypeKind::Int
    {
        return false;
    }
    let mut seen_caps = 0u32;
    let mut seen_actors: Vec<u32> = Vec::new();
    for (i, param) in func.parameters.iter().enumerate() {
        let ty = file.types.get(param.ty);
        if ty.kind != TypeKind::Capability {
            return false;
        }
        let Some(cap) = ty.capability else {
            return false;
        };
        let cap_num = cap as u32;
        if cap_num >= 31 {
            return false;
        }
        if cap == Capability::Simulation {
            let Some(svc_idx) = find_actor_service_for_param(file, main_fn, i as u32) else {
                return false;
            };
            if seen_actors.contains(&svc_idx) {
                return false;
            }
            seen_actors.push(svc_idx);
            continue;
        }
        let bit = 1u32 << cap_num;
        if (seen_caps & bit) != 0 {
            return false;
        }
        seen_caps |= bit;
    }
    true
}

#[cfg(unix)]
fn check_cli_folder(path: &[u8], flag_label: &str, diags: &mut DiagBag) -> Option<PathBuf> {
    use crate::fs_util::errno_message;
    let p = bytes_to_path(path);
    match rustix::fs::open(
        p.as_ref(),
        rustix::fs::OFlags::RDONLY | rustix::fs::OFlags::DIRECTORY | rustix::fs::OFlags::CLOEXEC,
        rustix::fs::Mode::empty(),
    ) {
        Ok(_fd) => Some(p.into_owned()),
        Err(err) => {
            let msg = format!("could not open {flag_label} folder");
            let detail = errno_message(err);
            bytecode_error(diags, ErrorCode::ToolPipeline, &msg, Some(&detail));
            None
        }
    }
}

#[cfg(not(unix))]
fn check_cli_folder(path: &[u8], flag_label: &str, diags: &mut DiagBag) -> Option<PathBuf> {
    let p = bytes_to_path(path);
    match std::fs::metadata(p.as_ref()) {
        Ok(m) if m.is_dir() => Some(p.into_owned()),
        Ok(_) => {
            let msg = format!("could not open {flag_label} folder");
            bytecode_error(
                diags,
                ErrorCode::ToolPipeline,
                &msg,
                Some("Not a directory"),
            );
            None
        }
        Err(err) => {
            let msg = format!("could not open {flag_label} folder");
            let detail = io_error_message(&err);
            bytecode_error(diags, ErrorCode::ToolPipeline, &msg, Some(&detail));
            None
        }
    }
}

fn open_cli_folders(cli: &Cli, diags: &mut DiagBag) -> Option<(Option<PathBuf>, Option<PathBuf>)> {
    let root_dir = match &cli.root {
        Some(r) => Some(check_cli_folder(r, "--root", diags)?),
        None => None,
    };
    let state_dir = match &cli.state_dir {
        Some(s) => Some(check_cli_folder(s, "--state-dir", diags)?),
        None => None,
    };
    Some((root_dir, state_dir))
}

fn stopped_simple(
    errors: &mut dyn Write,
    reason: &str,
    limit_name: Option<&'static str>,
    limit: usize,
) -> i32 {
    let defect = Defect {
        reason: reason.to_string(),
        limit_name,
        limit,
        detail: None,
        frames: Vec::new(),
        variables: Vec::new(),
        variables_unavailable: false,
    };
    if !write_defect(errors, None, &defect) {
        let _ = errors.write_all(b"mo: could not write the defect report\n");
        let _ = errors.flush();
    }
    70
}

fn run_loaded_main(
    cli: &Cli,
    image: &Image,
    main_fn: u32,
    root_dir: Option<&PathBuf>,
    state_dir: Option<&PathBuf>,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if output.flush().is_err() || errors.flush().is_err() {
        return stopped_simple(errors, "could not prepare console output", None, 0);
    }

    let file = image.file();
    let func = &file.functions[main_fn as usize];
    let mut needs_args = false;
    let mut needs_env = false;
    for param in &func.parameters {
        match file.types.get(param.ty).capability {
            Some(Capability::Args) => needs_args = true,
            Some(Capability::Env) => needs_env = true,
            _ => {}
        }
    }

    let mut args = Vec::new();
    if needs_args {
        args.reserve(cli.arguments.len());
        for raw in &cli.arguments {
            match std::str::from_utf8(raw) {
                Ok(s) => args.push(s.to_string()),
                Err(_) => {
                    return stopped_simple(
                        errors,
                        "arguments or environment are not valid UTF-8",
                        None,
                        0,
                    );
                }
            }
        }
    }

    let mut env_pairs = Vec::new();
    if needs_env {
        for (k, v) in env::vars_os() {
            match (k.into_string(), v.into_string()) {
                (Ok(ks), Ok(vs)) => env_pairs.push((ks, vs)),
                _ => {
                    return stopped_simple(
                        errors,
                        "arguments or environment are not valid UTF-8",
                        None,
                        0,
                    );
                }
            }
        }
    }

    #[cfg(unix)]
    let host = match HostContext::stdio_with_roots(
        args,
        env_pairs,
        root_dir.map(PathBuf::as_path),
        state_dir.map(PathBuf::as_path),
    ) {
        Ok(h) => h,
        Err(_) => {
            return stopped_simple(errors, "could not open the starting folder", None, 0);
        }
    };
    #[cfg(not(unix))]
    let host = {
        let _ = (root_dir, state_dir);
        HostContext::stdio(args, env_pairs)
    };

    let mut process = Process::new(image, cli.limits, host);
    match process.call_main(main_fn) {
        Outcome::Returned(val) => {
            let status = val.as_int();
            debug_assert!((0..=255).contains(&status));
            status as i32
        }
        Outcome::Failed(defect) | Outcome::Stopped(defect) => {
            if !write_defect(errors, Some(file), &defect) {
                let _ = errors.write_all(b"mo: could not write the defect report\n");
                let _ = errors.flush();
            }
            70
        }
    }
}

/// Loads and runs a precompiled `.bc` bytecode file (`src/tools/run.c:mo_run_bytecode`).
#[must_use]
pub fn run_bytecode(
    cli: &Cli,
    path: &[u8],
    output: &mut dyn Write,
    errors: &mut dyn Write,
    diags: &mut DiagBag,
) -> i32 {
    #[cfg(unix)]
    let file = {
        match rustix::fs::open(
            bytes_to_path(path).as_ref(),
            rustix::fs::OFlags::RDONLY
                | rustix::fs::OFlags::CLOEXEC
                | rustix::fs::OFlags::NOCTTY
                | rustix::fs::OFlags::NONBLOCK,
            rustix::fs::Mode::empty(),
        ) {
            Ok(fd) => File::from(fd),
            Err(err) => {
                let detail = crate::fs_util::errno_message(err);
                bytecode_error(
                    diags,
                    ErrorCode::ToolPipeline,
                    "bytecode could not be loaded",
                    Some(&detail),
                );
                return 1;
            }
        }
    };
    #[cfg(not(unix))]
    let file = match File::open(bytes_to_path(path)) {
        Ok(f) => f,
        Err(err) => {
            let detail = io_error_message(&err);
            bytecode_error(
                diags,
                ErrorCode::ToolPipeline,
                "bytecode could not be loaded",
                Some(&detail),
            );
            return 1;
        }
    };

    let meta = match file.metadata() {
        Ok(m) if m.file_type().is_file() => m,
        Ok(_) => {
            bytecode_error(
                diags,
                ErrorCode::ToolPipeline,
                "bytecode could not be loaded",
                Some("not a regular file"),
            );
            return 1;
        }
        Err(err) => {
            let detail = io_error_message(&err);
            bytecode_error(
                diags,
                ErrorCode::ToolPipeline,
                "bytecode could not be loaded",
                Some(&detail),
            );
            return 1;
        }
    };
    if meta.len() > MO_LIMIT_BYTECODE_SIZE {
        bytecode_error(
            diags,
            ErrorCode::ToolPipeline,
            "bytecode could not be loaded",
            Some("bytecode exceeds MO_LIMIT_BYTECODE_SIZE"),
        );
        return 1;
    }

    let mut wire = Vec::with_capacity(meta.len() as usize);
    if let Err(err) = file.take(MO_LIMIT_BYTECODE_SIZE + 1).read_to_end(&mut wire) {
        let detail = io_error_message(&err);
        bytecode_error(
            diags,
            ErrorCode::ToolPipeline,
            "bytecode could not be loaded",
            Some(&detail),
        );
        return 1;
    }
    if (wire.len() as u64) > MO_LIMIT_BYTECODE_SIZE {
        bytecode_error(
            diags,
            ErrorCode::ToolPipeline,
            "bytecode could not be loaded",
            Some("bytecode exceeds MO_LIMIT_BYTECODE_SIZE"),
        );
        return 1;
    }

    let bc_file = match load_bytecode(&wire) {
        Ok(f) => f,
        Err(err) => {
            bytecode_error(
                diags,
                ErrorCode::ToolPipeline,
                "bytecode could not be loaded",
                Some(err.reason),
            );
            return 1;
        }
    };

    for (idx, svc) in bc_file.services.iter().enumerate() {
        if svc.module == 0 && !is_actor_service(&bc_file, idx) {
            bytecode_error(
                diags,
                ErrorCode::ToolServiceUnavailable,
                "mo run does not start services yet",
                None,
            );
            return 1;
        }
    }

    let main_fn = if bc_file.modules.is_empty() {
        None
    } else {
        let mod0 = bc_file.string(bc_file.modules[0].name);
        let want = format!("{mod0}.main.entry");
        bc_file.functions.iter().enumerate().find_map(|(i, f)| {
            (f.module == 0 && f.kind == FunctionKind::Function && bc_file.string(f.name) == want)
                .then_some(i as u32)
        })
    };

    let Some(main_fn) = main_fn else {
        bytecode_error(
            diags,
            ErrorCode::MainContract,
            "the entry module has no main function",
            None,
        );
        return 1;
    };

    if !valid_bytecode_entry(&bc_file, main_fn) {
        bytecode_error(
            diags,
            ErrorCode::ToolPipeline,
            "bytecode could not be loaded",
            Some("entry function does not satisfy main's capability contract"),
        );
        return 1;
    }

    let Some((root_dir, state_dir)) = open_cli_folders(cli, diags) else {
        return 1;
    };

    let image = match Image::init(bc_file, cli.limits) {
        Ok(img) => img,
        Err(boxed) => {
            let (bc_err_file, defect) = *boxed;
            if !write_defect(errors, Some(&bc_err_file), &defect) {
                let _ = errors.write_all(b"mo: could not write the initialization defect\n");
                let _ = errors.flush();
            }
            return 70;
        }
    };

    run_loaded_main(
        cli,
        &image,
        main_fn,
        root_dir.as_ref(),
        state_dir.as_ref(),
        output,
        errors,
    )
}

pub(crate) fn find_core_entry(core: &CoreProgram, file: &BytecodeFile) -> Option<u32> {
    let Metadata::Source(meta) = &core.metadata else {
        return None;
    };
    for (i, f) in core.funs.iter().enumerate() {
        if f.name != core.entry {
            continue;
        }
        let target_name = &meta.funs[i].name;
        for (j, bc_fn) in file.functions.iter().enumerate() {
            if file.string(bc_fn.name) == target_name.as_str() {
                return Some(j as u32);
            }
        }
    }
    None
}

/// Loads compiled bytecode from a source compilation, runs global init and anchors, and
/// (when `cli.command == Command::Run`) runs `main` (`src/tools/run.c:mo_run_program`).
#[must_use]
#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_run_program pipeline boundary cleanly without wrapper allocation"
)]
pub fn run_program(
    cli: &Cli,
    program: &CheckedProgram,
    core: &CoreProgram,
    wire: &[u8],
    sources: &SourceMap,
    output: &mut dyn Write,
    errors: &mut dyn Write,
    diags: &mut DiagBag,
) -> i32 {
    let (root_dir, state_dir) = if cli.command == Command::Run {
        let Some(folders) = open_cli_folders(cli, diags) else {
            return 1;
        };
        folders
    } else {
        (None, None)
    };

    let bc_file = match load_bytecode(wire) {
        Ok(f) => f,
        Err(err) => {
            bytecode_error(
                diags,
                ErrorCode::ToolPipeline,
                "compiled bytecode could not be loaded",
                Some(err.reason),
            );
            return 1;
        }
    };

    let main_fn = find_core_entry(core, &bc_file);
    let anchors = program_has_anchors(program);
    let init_res = if anchors {
        Image::init_bounded(bc_file, cli.limits, MO_CLI_ANCHOR_INSTRUCTIONS)
    } else {
        Image::init(bc_file, cli.limits)
    };

    let image = match init_res {
        Ok(img) => img,
        Err(boxed) => {
            let (bc_err_file, defect) = *boxed;
            if anchors {
                let raw = render_defect_raw(&bc_err_file, &defect);
                let detail = if raw.len() <= MO_LIMIT_DEFECT_TEXT_SIZE as usize {
                    raw.as_str()
                } else {
                    "anchor execution could not complete; defect report unavailable"
                };
                let _ = diags.push(
                    Diagnostic::new(
                        ErrorCode::ToolAnchor,
                        Span::NONE,
                        "an anchor could not be checked",
                    )
                    .with_primary_text(detail),
                );
                return 1;
            }
            if !write_defect(errors, Some(&bc_err_file), &defect) {
                let _ = errors.write_all(b"mo: could not write the initialization defect\n");
                let _ = errors.flush();
            }
            return 70;
        }
    };

    if !check_anchors(&image, cli, sources, program, diags) {
        return 1;
    }

    if matches!(
        cli.command,
        Command::Check | Command::Emit | Command::Install
    ) {
        return 0;
    }

    let Some(main_fn) = main_fn else {
        bytecode_error(
            diags,
            ErrorCode::ToolPipeline,
            "compiled bytecode has no entry function",
            None,
        );
        return 1;
    };

    run_loaded_main(
        cli,
        &image,
        main_fn,
        root_dir.as_ref(),
        state_dir.as_ref(),
        output,
        errors,
    )
}
