//! Top-level command dispatcher and source-stack pipeline orchestration
//! (`src/tools/execute.c` parity).

use std::io::{self, Write};
use std::path::PathBuf;
use std::thread;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_bytecode::disassemble as disassemble_bytecode;
use mo_check::{
    LibraryMode, LoadOptions, check_program, default_embedded_library, load_program_bytes,
};
use mo_codegen::compile_services;
use mo_core::{check_program_services, print_program};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_BYTECODE_TEXT_SIZE, MO_LIMIT_CHECK_CPU_SECONDS,
    MO_LIMIT_CHECK_MEMORY, MO_LIMIT_CORE_TEXT_SIZE, MO_LIMIT_FIX_ROUNDS, MO_SOURCE_STACK_SIZE,
};
use mo_lower::{lower_services, lower_test_services};

use crate::anchors::program_has_anchors;
use crate::cli::{Cli, Command, EmitKind};
use crate::fix::{apply_fixes, leave_given, refuse_fix};
use crate::format::format_files;
use crate::fs_util::{FileIdentity, bytes_to_path, lstat_regular, write_bytecode_file};
use crate::identity::{
    MO_EMBEDDED_GUIDE, MO_EMBEDDED_REFERENCE, MO_VERSION, default_library_root, executable_path,
    runtime_identity_digest, version_line,
};
use crate::install::install_program;
use crate::render::{TestTotals, write_diagnostics, write_test_item, write_version};
use crate::run::{is_bytecode_path, run_bytecode, run_program};
use crate::test_cmd::{
    list_gates, run_bundle_test, run_gates, run_strength, run_tests, select_tests,
};

fn tool_error(diags: &mut DiagBag, code: ErrorCode, message: &str, detail: Option<&str>) {
    let mut d = Diagnostic::new(code, Span::NONE, message);
    if let Some(text) = detail {
        d = d.with_primary_text(text);
    }
    let _ = diags.push(d);
}

fn load_options_for(cli: &Cli) -> LoadOptions {
    let library = match &cli.library {
        Some(dir) => LibraryMode::Directory(bytes_to_path(dir).into_owned()),
        None => LibraryMode::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        },
    };
    LoadOptions {
        library,
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    }
}

struct PipelineOutcome {
    mistake: Option<&'static str>,
    version: bool,
    totals: TestTotals,
    status: i32,
}

fn execute_pipeline(
    cli: &Cli,
    file_idx: usize,
    sources: &mut SourceMap,
    diags: &mut DiagBag,
    output: &mut dyn Write,
    errors: &mut dyn Write,
    version: bool,
) -> PipelineOutcome {
    let mut out = PipelineOutcome {
        mistake: None,
        version,
        totals: TestTotals::default(),
        status: 0,
    };

    let entry = cli.file(file_idx);
    if cli.command == Command::Run && is_bytecode_path(entry) {
        out.status = run_bytecode(cli, entry, output, errors, diags);
        return out;
    }

    let load_opts = load_options_for(cli);
    let Some(modules) = load_program_bytes(sources, entry, &load_opts, diags) else {
        return out;
    };
    if diags.has_any() {
        return out;
    }

    let entry_module_span = modules.modules[0].syntax.span;
    let Some(program) = check_program(Some(sources), modules, diags) else {
        return out;
    };
    if diags.has_any() {
        return out;
    }

    let mut entry_service = None;
    for (idx, service) in program.services.iter().enumerate() {
        if service.module.0 == 0 && !service.is_actor {
            entry_service = Some(idx);
            if matches!(cli.command, Command::Run | Command::Install) {
                let span = program
                    .modules
                    .module(service.module)
                    .syntax
                    .tree
                    .node(service.syntax)
                    .span;
                let message = "mo run does not start services yet";
                let _ = diags.push(
                    Diagnostic::new(ErrorCode::ToolServiceUnavailable, span, message)
                        .with_primary_text(message),
                );
                return out;
            }
        }
    }

    let mut entry_api = None;
    for (idx, api) in program.apis.iter().enumerate() {
        if api.module.0 == 0 {
            entry_api = Some(idx);
            if matches!(cli.command, Command::Run | Command::Install)
                && program.entry.function.is_none()
            {
                let span = program
                    .modules
                    .module(api.module)
                    .syntax
                    .tree
                    .node(api.syntax)
                    .span;
                let message = "mo run does not start api servers; use mo serve";
                let _ = diags.push(
                    Diagnostic::new(ErrorCode::ToolServiceUnavailable, span, message)
                        .with_primary_text(message),
                );
                return out;
            }
        }
    }

    if (matches!(cli.command, Command::Run | Command::Install)
        || (cli.command == Command::Emit
            && !cli.emit_tests
            && entry_service.is_none()
            && entry_api.is_none()))
        && program.entry.function.is_none()
    {
        let message = "the entry module has no main function";
        let _ = diags.push(
            Diagnostic::new(ErrorCode::MainContract, entry_module_span, message)
                .with_primary_text(message)
                .with_reason("main declares the program's entry (reference 11.2)."),
        );
        return out;
    }

    if cli.command == Command::Test {
        if let Some(strength_bytes) = &cli.strength {
            let strength_dir = String::from_utf8_lossy(strength_bytes);
            run_strength(
                cli,
                &strength_dir,
                &load_opts.library,
                &program,
                sources,
                output,
                diags,
                &mut out.totals,
                &mut out.version,
                &mut out.mistake,
            );
            return out;
        }
        let Some(selection) = select_tests(&program, cli, diags) else {
            return out;
        };
        if let Some(mistake) = selection.mistake {
            out.mistake = Some(mistake);
            return out;
        }
        if cli.json {
            out.version = write_version(output, cli.command);
            if !out.version {
                out.status = 1;
                return out;
            }
        }
        if cli.list {
            for obs in &selection.items {
                if !write_test_item(output, cli.json, sources, obs) {
                    tool_error(
                        diags,
                        ErrorCode::ToolPipeline,
                        "test inventory could not be written",
                        None,
                    );
                    return out;
                }
            }
            if cli.mutants {
                list_gates(&program, sources, cli.json, output, diags);
            }
            return out;
        }
        if cli.mutants {
            run_gates(
                cli,
                &program,
                sources,
                &selection,
                output,
                diags,
                &mut out.totals,
            );
            return out;
        }
        run_tests(
            cli,
            &program,
            sources,
            &selection,
            output,
            diags,
            &mut out.totals,
        );
        return out;
    }

    let anchors = program_has_anchors(&program);
    let lowered_opt = if cli.emit_tests {
        lower_test_services(&program, Some(sources), diags)
    } else {
        lower_services(&program, Some(sources), diags)
    };
    let Some(lowered) = lowered_opt else {
        return out;
    };
    if diags.has_any() {
        return out;
    }
    let services_opt = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    if !check_program_services(&lowered.arena, &lowered.program, services_opt, diags) {
        return out;
    }

    let checking = matches!(cli.command, Command::Check | Command::Fix);
    let emitting = cli.command == Command::Emit;
    let installing = cli.command == Command::Install;
    if checking && cli.emit == EmitKind::None && !anchors {
        return out;
    }

    let mut wire = Vec::new();
    if anchors
        || cli.command == Command::Run
        || cli.emit == EmitKind::Bytecode
        || emitting
        || installing
    {
        let Some(compiled) =
            compile_services(&lowered.arena, &lowered.program, services_opt, diags)
        else {
            return out;
        };
        wire = compiled;
    }

    if anchors || cli.command == Command::Run || installing {
        out.status = run_program(
            cli,
            &program,
            &lowered.program,
            &wire,
            sources,
            output,
            errors,
            diags,
        );
        if out.status != 0 || diags.has_any() {
            return out;
        }
    }

    let mut printed: Option<Vec<u8>> = None;
    if cli.emit == EmitKind::Core {
        match print_program(&lowered.arena, &lowered.program) {
            Ok(text) if (text.len() as u64) <= MO_LIMIT_CORE_TEXT_SIZE => {
                printed = Some(text.into_bytes());
            }
            _ => {
                tool_error(
                    diags,
                    ErrorCode::ToolPipeline,
                    "Core text exceeds its output budget",
                    None,
                );
            }
        }
    } else if cli.emit == EmitKind::Bytecode {
        match disassemble_bytecode(&wire) {
            Ok(text) if (text.len() as u64) <= MO_LIMIT_BYTECODE_TEXT_SIZE => {
                printed = Some(text.into_bytes());
            }
            Ok(_) => {
                tool_error(
                    diags,
                    ErrorCode::ToolPipeline,
                    "bytecode text could not be emitted",
                    Some("bytecode text exceeds MO_LIMIT_BYTECODE_TEXT_SIZE"),
                );
            }
            Err(err) => {
                tool_error(
                    diags,
                    ErrorCode::ToolPipeline,
                    "bytecode text could not be emitted",
                    Some(err.reason),
                );
            }
        }
    }

    if emitting
        && !diags.has_any()
        && let Some(out_path) = &cli.output
    {
        write_bytecode_file(out_path, sources, &wire, diags);
    }
    if installing && !diags.has_any() {
        install_program(cli, sources, &wire, output, diags);
    }
    if let Some(bytes) = printed
        && !diags.has_any()
        && (output.write_all(&bytes).is_err() || output.flush().is_err())
    {
        tool_error(
            diags,
            ErrorCode::ToolPipeline,
            "compiled output could not be written",
            None,
        );
    }

    out
}

const CHECK_AGAIN: i32 = -1;

fn run_on_source_stack<F, R>(f: F) -> Result<R, String>
where
    F: FnOnce() -> R + Send,
    R: Send,
{
    let clock_mode = mo_test::deadline::thread_clock_mode();
    thread::scope(|scope| {
        let builder = thread::Builder::new()
            .name("mo-source".to_string())
            .stack_size(MO_SOURCE_STACK_SIZE as usize);
        let handle = builder
            .spawn_scoped(scope, move || {
                mo_test::set_thread_clock_mode(clock_mode);
                f()
            })
            .map_err(|e| crate::fs_util::io_error_message(&e))?;
        handle
            .join()
            .map_err(|_| "source pipeline thread panicked".to_string())
    })
}

fn check_program_cmd(
    cli: &Cli,
    file_idx: usize,
    output: &mut (dyn Write + Send),
    errors: &mut (dyn Write + Send),
    version: bool,
    fixing: Option<&FileIdentity>,
    given: Option<&[(u64, u64)]>,
) -> i32 {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();

    let work = match run_on_source_stack(|| {
        execute_pipeline(
            cli,
            file_idx,
            &mut sources,
            &mut diags,
            output,
            errors,
            version,
        )
    }) {
        Ok(w) => w,
        Err(reason) => {
            tool_error(
                &mut diags,
                ErrorCode::ToolSourceStack,
                "could not prepare the source pipeline stack",
                Some(&reason),
            );
            PipelineOutcome {
                mistake: None,
                version,
                totals: TestTotals::default(),
                status: 0,
            }
        }
    };

    let status = if let Some(original) = fixing {
        let path = cli.file(file_idx);
        match apply_fixes(path, original, &sources, &mut diags) {
            Ok(true) => CHECK_AGAIN,
            Ok(false) => 0,
            Err(problem) => refuse_fix(path, &problem, output, errors),
        }
    } else if let Some(mistake) = work.mistake {
        let _ = writeln!(errors, "{mistake}");
        let _ = errors.flush();
        2
    } else if cli.json && !work.version && work.status != 0 {
        let _ = errors.write_all(b"mo: could not write version record\n");
        let _ = errors.flush();
        1
    } else if cli.json && !work.version && !write_version(output, cli.command) {
        1
    } else {
        if let Some(given_list) = given {
            leave_given(cli, file_idx, given_list, &sources, &mut diags);
        }
        write_diagnostics(
            output,
            errors,
            cli.json,
            cli.command,
            &sources,
            &mut diags,
            Some(&work.totals),
        )
    };

    if status != 0 { status } else { work.status }
}

fn fix_files(cli: &Cli, output: &mut (dyn Write + Send), errors: &mut (dyn Write + Send)) -> i32 {
    let mut status = 0i32;
    for i in 0..cli.files.len() {
        let mut result = CHECK_AGAIN;
        for _ in 0..MO_LIMIT_FIX_ROUNDS {
            if result != CHECK_AGAIN {
                break;
            }
            let Some(st) = lstat_regular(cli.file(i)) else {
                break;
            };
            result = check_program_cmd(cli, i, output, errors, false, Some(&st), None);
        }
        if result > status {
            status = result;
        }
    }

    let given: Vec<(u64, u64)> = (0..cli.files.len())
        .map(|i| lstat_regular(cli.file(i)).map_or((0, 0), |st| (st.dev, st.ino)))
        .collect();

    for i in 0..cli.files.len() {
        let path = cli.file(i);
        let result = if lstat_regular(path).is_some() {
            check_program_cmd(cli, i, output, errors, false, None, Some(&given))
        } else {
            refuse_fix(
                path,
                "fix needs a readable regular file, not a symlink or special file",
                output,
                errors,
            )
        };
        if result > status {
            status = result;
        }
    }
    status
}

#[cfg(not(unix))]
fn deferred_command_error(errors: &mut dyn Write, label: &str, slice: &str) -> i32 {
    let _ = writeln!(
        errors,
        "mo: {label} is not yet implemented in the pure-Rust CLI (scheduled for Phase 3 {slice})"
    );
    let _ = errors.flush();
    1
}

/// Executes a parsed [`Cli`] command, writing to `output` and `errors` and returning the
/// POSIX process exit code (`src/tools/execute.c:mo_cli_execute`).
#[must_use]
pub fn execute_cli(
    cli: &Cli,
    output: &mut (dyn Write + Send),
    errors: &mut (dyn Write + Send),
) -> i32 {
    if cli.source_bundle {
        if cli.command == Command::Test {
            if cli.rlimit_child || cli.confine_child {
                mo_host::confine::enter_own_process_group();
            }
            if cli.rlimit_child
                && let Err(step) = mo_host::confine::apply_child_rlimits(
                    MO_LIMIT_CHECK_MEMORY,
                    MO_LIMIT_CHECK_CPU_SECONDS,
                )
            {
                let _ = writeln!(errors, "mo: cannot confine the child: {step}");
                let _ = errors.flush();
                return 125;
            }
            let load_opts = load_options_for(cli);
            if cli.confine_child {
                let exe = std::env::current_exe()
                    .ok()
                    .map(|p| std::fs::canonicalize(&p).unwrap_or(p))
                    .unwrap_or_else(|| PathBuf::from("/proc/self/exe"));
                let bundle_path = bytes_to_path(cli.file(0));
                let work_dir = bundle_path
                    .parent()
                    .filter(|p| !p.as_os_str().is_empty())
                    .unwrap_or_else(|| std::path::Path::new("."));
                let old_bundle = cli.old_bundle.as_deref().map(bytes_to_path);
                let ctx_in = cli.compile_context.as_deref().map(bytes_to_path);
                let lib_dir = cli.library.as_deref().map(bytes_to_path);
                let mut read_files: Vec<&std::path::Path> = vec![bundle_path.as_ref()];
                if let Some(ref p) = old_bundle {
                    read_files.push(p.as_ref());
                }
                if let Some(ref p) = ctx_in {
                    read_files.push(p.as_ref());
                }
                if let Err(step) = mo_host::confine::confine_compile_child(
                    &exe,
                    work_dir,
                    lib_dir.as_deref(),
                    &read_files,
                ) {
                    let _ = writeln!(errors, "mo: cannot confine the child: {step}");
                    let _ = errors.flush();
                    return 125;
                }
            }
            let mut sources = SourceMap::new();
            let mut diags = DiagBag::new();
            let run_res = run_on_source_stack(|| {
                run_bundle_test(cli, &load_opts.library, &mut sources, &mut diags);
            });
            if let Err(reason) = run_res {
                tool_error(
                    &mut diags,
                    ErrorCode::ToolSourceStack,
                    "could not prepare the source pipeline stack",
                    Some(&reason),
                );
            }
            return write_diagnostics(
                output,
                errors,
                false,
                Command::Emit,
                &sources,
                &mut diags,
                None,
            );
        }
        if cli.command == Command::Emit {
            let bundle_path = bytes_to_path(cli.file(0));
            let Some(out_raw) = cli.output.as_deref() else {
                let _ = writeln!(errors, "emit needs -o FILE: \"emit\"");
                return 2;
            };
            let out_path = bytes_to_path(out_raw);
            let old_bundle = cli.old_bundle.as_deref().map(bytes_to_path);
            let migration = cli
                .migration
                .as_deref()
                .and_then(|b| std::str::from_utf8(b).ok());
            let ctx_in = cli.compile_context.as_deref().map(bytes_to_path);
            let ctx_out_buf = {
                let mut os = out_path.as_os_str().to_os_string();
                os.push(".context");
                PathBuf::from(os)
            };
            let lib_dir = cli.library.as_deref().map(bytes_to_path);
            let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
            return mo_service::upgrade::compile::execute_emit_bundle(
                bundle_path.as_ref(),
                out_path.as_ref(),
                old_bundle.as_deref(),
                migration,
                ctx_in.as_deref(),
                Some(ctx_out_buf.as_path()),
                lib_dir.as_deref(),
                build_digest,
                cli.rlimit_child,
                cli.confine_child,
                errors,
            );
        }
        let _ = writeln!(errors, "mo: --bundle is only supported on emit and test");
        let _ = errors.flush();
        return 2;
    }
    if let Some(old_raw) = cli.upgrade_from.as_deref() {
        let old_path = bytes_to_path(old_raw);
        let migration = cli
            .migration
            .as_deref()
            .and_then(|b| std::str::from_utf8(b).ok());
        let lib_dir = cli.library.as_deref().map(bytes_to_path);
        let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
        let opts = mo_service::upgrade::commands::UpgradeCheckOptions {
            candidate_path: cli.file(0),
            old_path: old_path.as_ref(),
            migration,
            library: lib_dir.as_deref(),
            json: cli.json,
            build_digest,
        };
        return mo_service::upgrade::commands::run_upgrade_check(&opts, output, errors);
    }
    match cli.command {
        Command::Serve => {
            #[cfg(unix)]
            {
                let is_api = cli.files.len() == 1
                    && !cli.upgrade
                    && !cli.adopt
                    && cli.migration.is_none()
                    && cli.admin_socket.is_none()
                    && mo_service::api_server::probe_entry_api(cli.file(0));
                if is_api {
                    let data_dir = cli.data.as_deref().map(bytes_to_path);
                    let bind_host = cli
                        .bind_host
                        .as_deref()
                        .and_then(|b| std::str::from_utf8(b).ok());
                    let lib_dir = cli.library.as_deref().map(bytes_to_path);
                    let opts = mo_service::api_server::ApiServeOptions {
                        entry_file: cli.file(0),
                        data_dir: data_dir.as_deref(),
                        port: cli.has_port.then_some(cli.port),
                        bind_host,
                        workers: cli.has_workers.then_some(cli.workers),
                        watch: cli.watch,
                        library: lib_dir.as_deref(),
                        limits: cli.limits,
                    };
                    return mo_service::api_server::run_serve_api(&opts, output, errors);
                }
                if cli.bind_host.is_some() || cli.has_workers || cli.watch {
                    let _ = writeln!(
                        errors,
                        "--bind, --workers and --watch apply only to api servers: \"serve\""
                    );
                    let _ = errors.flush();
                    return 2;
                }
                if !cli.has_port {
                    let _ = writeln!(errors, "serve needs --port N: \"serve\"");
                    let _ = errors.flush();
                    return 2;
                }
                let Some(data_raw) = cli.data.as_deref() else {
                    let _ = writeln!(errors, "serve needs --data DIR: \"serve\"");
                    let _ = errors.flush();
                    return 2;
                };
                let data_dir = bytes_to_path(data_raw);
                let allow_host = cli
                    .allow_host
                    .as_deref()
                    .and_then(|b| std::str::from_utf8(b).ok());
                let admin_socket = cli.admin_socket.as_deref().map(bytes_to_path);
                let lib_dir = cli.library.as_deref().map(bytes_to_path);
                let migration = cli
                    .migration
                    .as_deref()
                    .and_then(|b| std::str::from_utf8(b).ok());
                let executable = executable_path()
                    .map(|b| bytes_to_path(&b).into_owned())
                    .or_else(|| std::env::current_exe().ok())
                    .unwrap_or_else(|| PathBuf::from("mo"));
                let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
                let opts = mo_service::serve::ServeOptions {
                    entry_file: (!cli.files.is_empty()).then(|| cli.file(0)),
                    data_dir: data_dir.as_ref(),
                    port: cli.port,
                    bind: None,
                    allow_host,
                    admin_socket: admin_socket.as_deref(),
                    library: lib_dir.as_deref(),
                    upgrade: cli.upgrade,
                    adopt: cli.adopt,
                    migration,
                    unconfined: cli.unconfined,
                    limits: cli.limits,
                    executable,
                    build_digest,
                };
                return mo_service::serve::run_serve_service(&opts, output, errors);
            }
            #[cfg(not(unix))]
            {
                return deferred_command_error(errors, "serve", "Unix platforms only");
            }
        }
        Command::Fetch => {
            #[cfg(unix)]
            {
                let Some(sock_raw) = cli.admin_socket.as_deref() else {
                    let _ = writeln!(
                        errors,
                        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE"
                    );
                    return 2;
                };
                let Some(out_raw) = cli.output.as_deref() else {
                    let _ = writeln!(
                        errors,
                        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE"
                    );
                    return 2;
                };
                let sock_path = bytes_to_path(sock_raw);
                let out_path = bytes_to_path(out_raw);
                let opts = mo_service::fetch::FetchOptions {
                    admin_socket: sock_path.as_ref(),
                    output_path: out_path.as_ref(),
                    failure_id: (cli.fetch_failure > 0).then_some(cli.fetch_failure),
                    json: cli.json,
                };
                return mo_service::fetch::run_fetch(&opts, output, errors);
            }
            #[cfg(not(unix))]
            {
                return deferred_command_error(errors, "fetch", "Unix platforms only");
            }
        }
        Command::Push => {
            #[cfg(unix)]
            {
                let Some(sock_raw) = cli.admin_socket.as_deref() else {
                    let _ = writeln!(
                        errors,
                        "usage: mo push FILE --admin-socket PATH [--base DIGEST] [--migrate Module.function] [--json]"
                    );
                    return 2;
                };
                let sock_path = bytes_to_path(sock_raw);
                let base_hex = cli
                    .push_base
                    .as_deref()
                    .and_then(|b| std::str::from_utf8(b).ok());
                let migration = cli
                    .migration
                    .as_deref()
                    .and_then(|b| std::str::from_utf8(b).ok());
                let lib_dir = cli.library.as_deref().map(bytes_to_path);
                let opts = mo_service::upgrade::commands::PushOptions {
                    candidate_path: cli.file(0),
                    admin_socket: sock_path.as_ref(),
                    base_hex,
                    migration,
                    library: lib_dir.as_deref(),
                    json: cli.json,
                };
                return mo_service::upgrade::commands::run_push(&opts, output, errors);
            }
            #[cfg(not(unix))]
            {
                return deferred_command_error(errors, "push", "Unix platforms only");
            }
        }
        Command::Replay => {
            let data_dir = cli.data.as_deref().map(bytes_to_path);
            let artifact = cli.artifact.as_deref().map(bytes_to_path);
            let lib_dir = cli.library.as_deref().map(bytes_to_path);
            let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
            let opts = mo_upgrade::ReplayOptions {
                file: (!cli.files.is_empty()).then(|| cli.file(0)),
                failure_id: cli.failure,
                data_dir: data_dir.as_deref(),
                artifact: artifact.as_deref(),
                revised: cli.revised.as_deref(),
                recover: cli.recover,
                json: cli.json,
                library: lib_dir.as_deref(),
                version: MO_VERSION,
                build_digest,
            };
            return run_on_source_stack(|| mo_upgrade::run_replay(&opts, output, errors))
                .unwrap_or_else(|reason| {
                    let _ = writeln!(
                        errors,
                        "mo replay: could not prepare pipeline stack: {reason}"
                    );
                    1
                });
        }
        Command::Query => {
            let Some(fn_raw) = cli.function.as_deref() else {
                let _ = writeln!(errors, "mo query: missing query function name");
                return 2;
            };
            let Ok(function) = std::str::from_utf8(fn_raw) else {
                let _ = writeln!(errors, "mo query: function name is not valid UTF-8");
                return 2;
            };
            let data_dir = cli.data.as_deref().map(bytes_to_path);
            let artifact = cli.artifact.as_deref().map(bytes_to_path);
            let admin_socket = cli.admin_socket.as_deref().map(bytes_to_path);
            let lib_dir = cli.library.as_deref().map(bytes_to_path);
            let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
            let opts = mo_upgrade::QueryOptions {
                file: cli.file(0),
                function,
                failure_id: cli.failure,
                data_dir: data_dir.as_deref(),
                artifact: artifact.as_deref(),
                admin_socket: admin_socket.as_deref(),
                recover: cli.recover,
                json: cli.json,
                library: lib_dir.as_deref(),
                limits: cli.limits,
                max_work: cli.max_work,
                version: MO_VERSION,
                build_digest,
            };
            return run_on_source_stack(|| mo_upgrade::run_query(&opts, output, errors))
                .unwrap_or_else(|reason| {
                    let _ = writeln!(
                        errors,
                        "mo query: could not prepare pipeline stack: {reason}"
                    );
                    1
                });
        }
        Command::Rehearse => {
            let (Some(art_raw), Some(old_raw), Some(cand_raw)) = (
                cli.artifact.as_deref(),
                cli.old_bundle.as_deref(),
                cli.candidate.as_deref(),
            ) else {
                let _ = writeln!(
                    errors,
                    "usage: mo rehearse --artifact FILE --old-bundle FILE --candidate FILE [--migrate M] [--context FILE] [--json]"
                );
                return 2;
            };
            let artifact = bytes_to_path(art_raw);
            let old_bundle = bytes_to_path(old_raw);
            let candidate = bytes_to_path(cand_raw);
            let migration = cli
                .migration
                .as_deref()
                .and_then(|b| std::str::from_utf8(b).ok());
            let compile_context = cli.compile_context.as_deref().map(bytes_to_path);
            let lib_dir = cli.library.as_deref().map(bytes_to_path);
            let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
            let opts = mo_upgrade::RehearseOptions {
                artifact: artifact.as_ref(),
                old_bundle: old_bundle.as_ref(),
                candidate: candidate.as_ref(),
                migration,
                compile_context: compile_context.as_deref(),
                library: lib_dir.as_deref(),
                json: cli.json,
                build_digest,
            };
            return run_on_source_stack(|| mo_upgrade::run_rehearse(&opts, output, errors))
                .unwrap_or_else(|reason| {
                    let _ = writeln!(
                        errors,
                        "mo rehearse: could not prepare pipeline stack: {reason}"
                    );
                    1
                });
        }
        Command::History => {
            let folder_cow = String::from_utf8_lossy(cli.file(0));
            let build_digest = runtime_identity_digest().unwrap_or([0u8; 32]);
            let opts = mo_service::history::HistoryOptions {
                folder: folder_cow.as_ref(),
                recover: cli.recover,
                json: cli.json,
                version: MO_VERSION,
                build_digest,
            };
            return mo_service::history::run_history(&opts, output, errors);
        }
        Command::Diff => {
            let lib_dir = cli.library.as_deref().map(bytes_to_path);
            let (old_file, new_file) = if cli.files.len() == 2 {
                (Some(cli.file(0)), cli.file(1))
            } else {
                (None, cli.file(0))
            };
            let opts = mo_upgrade::DiffOptions {
                old_file,
                new_file,
                json: cli.json,
                library: lib_dir.as_deref(),
                version: MO_VERSION,
            };
            return run_on_source_stack(|| mo_upgrade::run_diff(&opts, output, errors))
                .unwrap_or_else(|reason| {
                    let _ = writeln!(
                        errors,
                        "mo diff: could not prepare pipeline stack: {reason}"
                    );
                    1
                });
        }
        Command::Version => {
            let ok = writeln!(output, "{}", version_line()).is_ok() && output.flush().is_ok();
            if !ok {
                let _ = errors.write_all(b"mo: could not write version\n");
                let _ = errors.flush();
                return 1;
            }
            return 0;
        }
        Command::Guide => {
            let data = if cli.reference {
                MO_EMBEDDED_REFERENCE.as_bytes()
            } else {
                if MO_EMBEDDED_GUIDE.is_empty() {
                    let _ = errors.write_all(
                        b"the agent guide isn't written yet; use mo guide --reference\n",
                    );
                    let _ = errors.flush();
                    return 1;
                }
                MO_EMBEDDED_GUIDE.as_bytes()
            };
            let res = output.write_all(data).and_then(|()| output.flush());
            return match res {
                Ok(()) => 0,
                Err(err) if err.kind() == io::ErrorKind::BrokenPipe => 0,
                Err(_) => {
                    let _ = errors.write_all(b"mo: could not write guide\n");
                    let _ = errors.flush();
                    1
                }
            };
        }
        Command::Format => {
            return format_files(cli, output, errors);
        }
        Command::Fix => {
            return fix_files(cli, output, errors);
        }
        Command::Check | Command::Run | Command::Test | Command::Emit | Command::Install => {}
    }

    // Test writes its version record once its selection is checked.
    let version = cli.json && cli.command != Command::Test && cli.command != Command::Query;
    if version && !write_version(output, cli.command) {
        let _ = errors.write_all(b"mo: could not write version record\n");
        let _ = errors.flush();
        return 1;
    }
    check_program_cmd(cli, 0, output, errors, version, None, None)
}

/// Resolves the library directory mode that `execute_cli` uses for a given `Cli`
/// (reusable helper for Slices 3C–3E).
#[must_use]
pub fn resolve_library_dir(cli: &Cli) -> Option<PathBuf> {
    cli.library
        .as_deref()
        .map(|raw| bytes_to_path(raw).into_owned())
        .or_else(default_library_root)
}
