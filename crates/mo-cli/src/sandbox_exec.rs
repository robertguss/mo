//! `mo sandbox-run`: capability-confined, bounded Mo script runner for Mosh sub-tools and RLM probes.

use std::cell::RefCell;
use std::io::{self, Write};
#[cfg(unix)]
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command as OsCommand, Stdio};
use std::rc::Rc;
use std::time::{Duration, Instant};

use mo_base::{DiagBag, Diagnostic, SourceMap};
use mo_bytecode::{BytecodeFile, Capability, FunctionKind, TypeKind, load as load_bytecode};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_CHECK_CPU_SECONDS, MO_LIMIT_CHECK_MEMORY,
    MO_LIMIT_DEFECT_TEXT_SIZE, MO_SOURCE_STACK_SIZE,
};
use mo_host::HostContext;
use mo_lower::lower_services;
use mo_vm::{Defect, Image, Outcome, Process, render_defect_raw};

use crate::cli::Cli;
use crate::fs_util::bytes_to_path;
use crate::identity::default_library_root;
use crate::index::push_json_str;
use crate::render::write_diagnostics;
use crate::run::find_core_entry;

fn validate_root_dir(raw: &[u8]) -> Result<PathBuf, &'static str> {
    if raw.is_empty() || raw.contains(&0) {
        return Err("invalid --root directory");
    }
    let path = bytes_to_path(raw).into_owned();
    let Ok(meta) = std::fs::metadata(&path) else {
        return Err("invalid --root directory");
    };
    if !meta.is_dir() {
        return Err("invalid --root directory");
    }
    Ok(path)
}

struct SharedBufferWriter {
    buf: Rc<RefCell<Vec<u8>>>,
}

impl Write for SharedBufferWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        let mut guard = self.buf.borrow_mut();
        guard.extend_from_slice(data);
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn parse_allowed_hosts_flag(raw: &[u8]) -> Result<Vec<String>, String> {
    let text =
        std::str::from_utf8(raw).map_err(|_| "--allow-hosts must be valid UTF-8".to_string())?;
    if text.trim().is_empty() {
        return Err("--allow-hosts requires at least one host".to_string());
    }
    let mut hosts = Vec::new();
    for part in text.split(',') {
        let trimmed = part.trim();
        if trimmed.is_empty() {
            return Err("--allow-hosts contains an empty host entry".to_string());
        }
        hosts.push(trimmed.to_string());
    }
    Ok(hosts)
}

fn check_sandbox_manifest(
    file: &BytecodeFile,
    main_fn: u32,
    read_only: bool,
    has_allowed_hosts: bool,
) -> Result<bool, String> {
    let Some(func) = file.functions.get(main_fn as usize) else {
        return Err("the entry module has no main function".to_string());
    };
    if func.kind != FunctionKind::Function || file.types.get(func.result_type).kind != TypeKind::Int
    {
        return Err("entry function does not satisfy main's capability contract".to_string());
    }
    let mut uses_network = false;
    for param in &func.parameters {
        let ty = file.types.get(param.ty);
        if ty.kind != TypeKind::Capability {
            return Err("entry function accepts a non-capability parameter".to_string());
        }
        let Some(cap) = ty.capability else {
            return Err("invalid capability parameter".to_string());
        };
        match cap {
            Capability::Command => {
                return Err("Command capability is forbidden in mo sandbox-run".to_string());
            }
            Capability::Env => {
                return Err("Env capability is forbidden in mo sandbox-run".to_string());
            }
            Capability::Files if read_only => {
                return Err(
                    "Files capability is forbidden in --read-only mode; use ReadFiles".to_string(),
                );
            }
            Capability::Sqlite if read_only => {
                return Err("Sqlite capability is forbidden in --read-only mode".to_string());
            }
            Capability::Network => {
                if !has_allowed_hosts {
                    return Err("Network capability requires --allow-hosts".to_string());
                }
                uses_network = true;
            }
            _ => {}
        }
    }
    Ok(uses_network)
}

struct SandboxReport<'a> {
    ok: bool,
    exit_code: i32,
    timed_out: bool,
    refused_reason: Option<&'a str>,
    stdout_bytes: &'a [u8],
    stderr_bytes: &'a [u8],
    defect: Option<&'a str>,
}

fn emit_sandbox_json(output: &mut dyn Write, r: &SandboxReport<'_>) {
    let mut out = String::with_capacity(256 + r.stdout_bytes.len() + r.stderr_bytes.len());
    out.push_str("{\"ok\":");
    out.push_str(if r.ok { "true" } else { "false" });
    out.push_str(",\"exit_code\":");
    out.push_str(&r.exit_code.to_string());
    out.push_str(",\"timed_out\":");
    out.push_str(if r.timed_out { "true" } else { "false" });
    out.push_str(",\"refused_reason\":");
    match r.refused_reason {
        Some(reason) => push_json_str(&mut out, reason),
        None => out.push_str("null"),
    }
    out.push_str(",\"stdout\":");
    push_json_str(&mut out, &String::from_utf8_lossy(r.stdout_bytes));
    out.push_str(",\"stderr\":");
    push_json_str(&mut out, &String::from_utf8_lossy(r.stderr_bytes));
    out.push_str(",\"defect\":");
    match r.defect {
        Some(d) => push_json_str(&mut out, d),
        None => out.push_str("null"),
    }
    out.push_str("}\n");
    let _ = output.write_all(out.as_bytes());
    let _ = output.flush();
}

fn fail_sandbox(
    cli: &Cli,
    output: &mut dyn Write,
    errors: &mut dyn Write,
    exit_code: i32,
    reason: &str,
) -> i32 {
    if cli.json {
        emit_sandbox_json(
            output,
            &SandboxReport {
                ok: false,
                exit_code,
                timed_out: false,
                refused_reason: Some(reason),
                stdout_bytes: b"",
                stderr_bytes: b"",
                defect: None,
            },
        );
    } else {
        let _ = writeln!(errors, "mo sandbox-run: {reason}");
        let _ = errors.flush();
    }
    exit_code
}

fn render_defect_bounded(file: Option<&BytecodeFile>, defect: &Defect) -> String {
    let raw = match file {
        Some(f) => render_defect_raw(f, defect),
        None => format!("{}\n", defect.reason),
    };
    if raw.len() <= MO_LIMIT_DEFECT_TEXT_SIZE as usize {
        return raw;
    }
    let mut end = MO_LIMIT_DEFECT_TEXT_SIZE as usize;
    while end > 0 && !raw.is_char_boundary(end) {
        end -= 1;
    }
    raw[..end].to_string()
}

fn run_confined_subprocess(
    cli: &Cli,
    exe: &Path,
    output: &mut (dyn Write + Send),
    errors: &mut (dyn Write + Send),
) -> i32 {
    let mut cmd = OsCommand::new(exe);
    cmd.arg("sandbox-run")
        .arg("--confine-child")
        .arg("--rlimit-child");
    if cli.read_only {
        cmd.arg("--read-only");
    }
    if let Some(ref hosts) = cli.allow_hosts {
        cmd.arg("--allow-hosts");
        cmd.arg(bytes_to_path(hosts).as_os_str());
    }
    if let Some(ref root) = cli.root {
        cmd.arg("--root");
        cmd.arg(bytes_to_path(root).as_os_str());
    }
    if let Some(ref lib) = cli.library {
        cmd.arg("--lib");
        cmd.arg(bytes_to_path(lib).as_os_str());
    }
    cmd.arg("--max-heap")
        .arg(cli.limits.heap_size.to_string())
        .arg("--max-stack")
        .arg(cli.limits.stack_size.to_string())
        .arg("--max-depth")
        .arg(cli.limits.call_depth.to_string())
        .arg("--max-work")
        .arg(cli.max_work.to_string());
    cmd.arg(bytes_to_path(cli.file(0)).as_os_str());
    if !cli.arguments.is_empty() {
        cmd.arg("--");
        for arg in &cli.arguments {
            cmd.arg(bytes_to_path(arg).as_os_str());
        }
    }
    cmd.stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(unix)]
    cmd.process_group(0);

    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            return fail_sandbox(
                cli,
                output,
                errors,
                125,
                &format!("could not spawn sandbox child: {e}"),
            );
        }
    };

    let out_reader = child.stdout.take().map(|mut p| {
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            let _ = std::io::Read::read_to_end(&mut p, &mut buf);
            buf
        })
    });
    let err_reader = child.stderr.take().map(|mut p| {
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            let _ = std::io::Read::read_to_end(&mut p, &mut buf);
            buf
        })
    });

    let timeout_secs = if cli.timeout_seconds > 0 {
        u64::from(cli.timeout_seconds)
    } else {
        30
    };
    let deadline = Instant::now() + Duration::from_secs(timeout_secs);
    let mut timed_out = false;
    let status = loop {
        match child.try_wait() {
            Ok(Some(st)) => break Some(st),
            Ok(None) => {
                if Instant::now() >= deadline {
                    timed_out = true;
                    mo_host::confine::kill_child_tree(child.id());
                    let _ = child.kill();
                    break child.wait().ok();
                }
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(_) => break None,
        }
    };

    let out_bytes = out_reader.and_then(|h| h.join().ok()).unwrap_or_default();
    let err_bytes = err_reader.and_then(|h| h.join().ok()).unwrap_or_default();

    if timed_out {
        if cli.json {
            emit_sandbox_json(
                output,
                &SandboxReport {
                    ok: false,
                    exit_code: 124,
                    timed_out: true,
                    refused_reason: Some("sandbox execution timed out"),
                    stdout_bytes: &out_bytes,
                    stderr_bytes: &err_bytes,
                    defect: None,
                },
            );
        } else {
            let _ = output.write_all(&out_bytes);
            let _ = errors.write_all(&err_bytes);
            let _ = writeln!(errors, "mo sandbox-run: timed out after {timeout_secs}s");
            let _ = errors.flush();
        }
        return 124;
    }

    let code = status.and_then(|st| st.code()).unwrap_or(1);
    if cli.json {
        let refused = if code == 3 {
            Some(String::from_utf8_lossy(&err_bytes).trim().to_string())
        } else {
            None
        };
        let defect = if code == 70 {
            Some(String::from_utf8_lossy(&err_bytes).into_owned())
        } else {
            None
        };
        emit_sandbox_json(
            output,
            &SandboxReport {
                ok: code == 0,
                exit_code: code,
                timed_out: false,
                refused_reason: refused.as_deref(),
                stdout_bytes: &out_bytes,
                stderr_bytes: &err_bytes,
                defect: defect.as_deref(),
            },
        );
    } else {
        let _ = output.write_all(&out_bytes);
        let _ = output.flush();
        let _ = errors.write_all(&err_bytes);
        let _ = errors.flush();
    }
    code
}

fn compile_and_run_sandboxed(
    cli: &Cli,
    hosts: Option<Vec<String>>,
    output: &mut (dyn Write + Send),
    errors: &mut (dyn Write + Send),
) -> i32 {
    let library = match &cli.library {
        Some(dir) => mo_check::LibraryMode::Directory(bytes_to_path(dir).into_owned()),
        None => mo_check::LibraryMode::Embedded {
            library: mo_check::default_embedded_library(),
            entry_root: default_library_root(),
        },
    };
    let load_opts = mo_check::LoadOptions {
        library,
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };

    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let entry = cli.file(0);

    let render_diag = |sources: &SourceMap, diags: &mut DiagBag| -> (i32, Vec<u8>) {
        let mut d_out = Vec::new();
        let mut d_err = Vec::new();
        let status = write_diagnostics(
            &mut d_out,
            &mut d_err,
            false,
            cli.command,
            sources,
            diags,
            None,
        );
        d_err.extend_from_slice(&d_out);
        (status, d_err)
    };

    type CompileErr = (i32, Vec<u8>);
    type LoweredOut = (Vec<u8>, mo_core::Program, Vec<PathBuf>);
    type CompiledOut = (BytecodeFile, u32, Vec<PathBuf>);

    let compile_res: Result<CompiledOut, CompileErr> = std::thread::scope(|scope| {
        let builder = std::thread::Builder::new()
            .name("mo-sandbox-compile".to_string())
            .stack_size(MO_SOURCE_STACK_SIZE as usize);
        let handle = builder
            .spawn_scoped(scope, || -> Result<LoweredOut, CompileErr> {
                let Some(modules) =
                    mo_check::load_program_bytes(&mut sources, entry, &load_opts, &mut diags)
                else {
                    return Err(render_diag(&sources, &mut diags));
                };
                if diags.has_any() {
                    return Err(render_diag(&sources, &mut diags));
                }
                let file_paths: Vec<PathBuf> = (0..sources.len())
                    .filter_map(|id| {
                        sources
                            .get(id)
                            .map(|sf| bytes_to_path(sf.path_bytes()).into_owned())
                    })
                    .collect();
                let entry_span = modules.modules[0].syntax.span;
                let Some(program) = mo_check::check_program(Some(&sources), modules, &mut diags)
                else {
                    return Err(render_diag(&sources, &mut diags));
                };
                if diags.has_any() {
                    return Err(render_diag(&sources, &mut diags));
                }
                if program.entry.function.is_none() {
                    let _ = diags.push(Diagnostic::new(
                        ErrorCode::MainContract,
                        entry_span,
                        "the entry module has no main function",
                    ));
                    return Err(render_diag(&sources, &mut diags));
                }
                let Some(lowered) = lower_services(&program, Some(&sources), &mut diags) else {
                    return Err(render_diag(&sources, &mut diags));
                };
                let services_opt =
                    (!lowered.services.items.is_empty()).then_some(&lowered.services);
                if !check_program_services(
                    &lowered.arena,
                    &lowered.program,
                    services_opt,
                    &mut diags,
                ) {
                    return Err(render_diag(&sources, &mut diags));
                }
                let Some(wire) =
                    compile_services(&lowered.arena, &lowered.program, services_opt, &mut diags)
                else {
                    return Err(render_diag(&sources, &mut diags));
                };
                Ok((wire, lowered.program, file_paths))
            })
            .map_err(|_| (1i32, b"sandbox compiler thread spawn failed".to_vec()))?;
        let (wire, core, file_paths) = handle
            .join()
            .map_err(|_| (1i32, b"sandbox compiler thread panicked".to_vec()))??;
        let bc = load_bytecode(&wire).map_err(|_| (1i32, b"bytecode load failed".to_vec()))?;
        let main_fn =
            find_core_entry(&core, &bc).ok_or_else(|| (1i32, b"missing main entry".to_vec()))?;
        Ok((bc, main_fn, file_paths))
    });

    let (bc_file, main_fn, loaded_paths) = match compile_res {
        Ok(ok) => ok,
        Err((status, err_bytes)) => {
            if cli.json {
                emit_sandbox_json(
                    output,
                    &SandboxReport {
                        ok: false,
                        exit_code: status,
                        timed_out: false,
                        refused_reason: Some("program failed compilation check"),
                        stdout_bytes: b"",
                        stderr_bytes: &err_bytes,
                        defect: None,
                    },
                );
            } else {
                let _ = errors.write_all(&err_bytes);
                let _ = errors.flush();
            }
            return status;
        }
    };

    let uses_network =
        match check_sandbox_manifest(&bc_file, main_fn, cli.read_only, hosts.is_some()) {
            Ok(net) => net,
            Err(reason) => return fail_sandbox(cli, output, errors, 3, &reason),
        };

    let root_dir: Option<PathBuf> = match &cli.root {
        Some(raw) => match validate_root_dir(raw) {
            Ok(p) => Some(p),
            Err(msg) => return fail_sandbox(cli, output, errors, 2, msg),
        },
        None => None,
    };

    if cli.rlimit_child
        && let Err(step) =
            mo_host::confine::apply_child_rlimits(MO_LIMIT_CHECK_MEMORY, MO_LIMIT_CHECK_CPU_SECONDS)
    {
        return fail_sandbox(
            cli,
            output,
            errors,
            125,
            &format!("rlimit setup failed: {step}"),
        );
    }

    if cli.confine_child {
        let exe = std::env::current_exe()
            .ok()
            .map(|p| std::fs::canonicalize(&p).unwrap_or(p))
            .unwrap_or_else(|| PathBuf::from("/proc/self/exe"));
        let lib_dir = cli.library.as_deref().map(bytes_to_path);
        let read_refs: Vec<&Path> = loaded_paths.iter().map(PathBuf::as_path).collect();
        let effective_root = root_dir.as_deref().or_else(|| Some(Path::new(".")));
        if let Err(step) = mo_host::confine::confine_sandbox_child(
            &exe,
            effective_root,
            cli.read_only,
            uses_network,
            lib_dir.as_deref(),
            &read_refs,
        ) {
            return fail_sandbox(
                cli,
                output,
                errors,
                125,
                &format!("cannot confine child: {step}"),
            );
        }
    }

    let image = match Image::init(bc_file, cli.limits) {
        Ok(img) => img,
        Err(boxed) => {
            let (bc_err_file, defect) = *boxed;
            let text = render_defect_bounded(Some(&bc_err_file), &defect);
            if cli.json {
                emit_sandbox_json(
                    output,
                    &SandboxReport {
                        ok: false,
                        exit_code: 70,
                        timed_out: false,
                        refused_reason: None,
                        stdout_bytes: b"",
                        stderr_bytes: b"",
                        defect: Some(&text),
                    },
                );
            } else {
                let _ = write!(errors, "bug: {text}");
                let _ = errors.flush();
            }
            return 70;
        }
    };

    let mut args = Vec::with_capacity(cli.arguments.len());
    for raw in &cli.arguments {
        match std::str::from_utf8(raw) {
            Ok(s) => args.push(s.to_string()),
            Err(_) => return fail_sandbox(cli, output, errors, 2, "arguments are not valid UTF-8"),
        }
    }

    let captured_out = Rc::new(RefCell::new(Vec::new()));
    let captured_err = Rc::new(RefCell::new(Vec::new()));

    let mut host = if cli.json {
        HostContext::with_io(
            args,
            Vec::new(),
            io::empty(),
            SharedBufferWriter {
                buf: Rc::clone(&captured_out),
            },
            SharedBufferWriter {
                buf: Rc::clone(&captured_err),
            },
        )
    } else {
        HostContext::stdio(args, Vec::new())
    };
    host.set_read_only_sandbox(cli.read_only);
    if let Some(ref h) = hosts
        && let Err(e) = host.set_allowed_hosts(h)
    {
        return fail_sandbox(cli, output, errors, 2, &e);
    }
    #[cfg(unix)]
    if let Some(ref root) = root_dir
        && let Err(e) = host.set_root_dir(root)
    {
        return fail_sandbox(cli, output, errors, 2, &e);
    }

    let mut process = Process::new(&image, cli.limits, host);
    let outcome = process.call_main_bounded(main_fn, cli.max_work);
    let (ok, code, defect_opt) = match outcome {
        Outcome::Returned(val) => {
            let st = val.as_int() as i32;
            (st == 0, st, None)
        }
        Outcome::Failed(d) | Outcome::Stopped(d) => {
            let text = render_defect_bounded(Some(image.file()), &d);
            (false, 70, Some(text))
        }
    };

    if cli.json {
        let ob = captured_out.borrow();
        let eb = captured_err.borrow();
        emit_sandbox_json(
            output,
            &SandboxReport {
                ok,
                exit_code: code,
                timed_out: false,
                refused_reason: None,
                stdout_bytes: &ob,
                stderr_bytes: &eb,
                defect: defect_opt.as_deref(),
            },
        );
    } else if let Some(ref def) = defect_opt {
        let _ = write!(errors, "bug: {def}");
        let _ = errors.flush();
    }
    code
}

/// Runs `mo sandbox-run` with fail-closed capability manifest checks, Landlock/rlimit
/// confinement (when executed via the `mo` CLI binary), `--timeout`, and `--max-work` bounds.
#[must_use]
pub fn run_sandbox_run(
    cli: &Cli,
    output: &mut (dyn Write + Send),
    errors: &mut (dyn Write + Send),
) -> i32 {
    let hosts = match &cli.allow_hosts {
        Some(raw) => match parse_allowed_hosts_flag(raw) {
            Ok(h) => Some(h),
            Err(e) => return fail_sandbox(cli, output, errors, 2, &e),
        },
        None => None,
    };

    if !cli.confine_child
        && !cli.unconfined
        && let Some(exe) = mo_host::confine::trampoline_exe()
    {
        return run_confined_subprocess(cli, exe, output, errors);
    }

    compile_and_run_sandboxed(cli, hosts, output, errors)
}
