//! Safe Landlock confinement check + child process compilation for live upgrades
//! (`src/host/confine.c`, `src/host/compile.c`, and `mo emit --bundle` parity).

use std::fs::{self, OpenOptions};
use std::io::Write;
#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
#[cfg(unix)]
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, RenderFormat, SourceMap, Span, TerminalEncoding,
    render_diagnostic_default,
};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_CHECK_WALL_US, MO_LIMIT_UPGRADE_COMPILE_CPU_SECONDS, MO_LIMIT_UPGRADE_COMPILE_MEMORY,
    MO_LIMIT_UPGRADE_COMPILE_WALL_US,
};

const UNICODE_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: mo_prim::unicode16::next_grapheme_bytes,
    display_width: mo_prim::unicode16::display_width_bytes,
};

fn write_bundle_diags(sources: &SourceMap, diags: &mut DiagBag, errors: &mut dyn Write) {
    diags.sort();
    for d in diags.items() {
        if let Ok(buf) = render_diagnostic_default(
            sources,
            d,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE_METRICS,
        ) {
            let _ = errors.write_all(&buf);
        } else {
            let _ = writeln!(errors, "error E{:04}: {}", d.code as u16, d.message);
        }
    }
    let _ = errors.flush();
}

use crate::binary::compile_context::read_compile_context;
use crate::binary::sha256_bytes;
use crate::store::programs::StoredProgram;

use super::bundle::compile_bundle;
use super::{UpgradeImage, UpgradeReport, load_service_image};

/// Returns `true` if the running kernel supports Linux Landlock ABI >= 3 and Seccomp
/// `SECCOMP_RET_KILL_PROCESS` on a supported architecture.
#[must_use]
pub fn can_confine() -> bool {
    mo_host::confine::can_confine()
}

fn create_private_work_dir() -> Result<PathBuf, String> {
    let base = std::env::temp_dir();
    let mut rnd = [0u8; 8];
    for _ in 0..32 {
        getrandom::fill(&mut rnd).map_err(|e| format!("entropy for compile dir: {e}"))?;
        let name = format!("mo-upgrade-{:016x}", u64::from_le_bytes(rnd));
        let candidate = base.join(name);
        match fs::create_dir(&candidate) {
            Ok(()) => {
                #[cfg(unix)]
                {
                    fs::set_permissions(&candidate, fs::Permissions::from_mode(0o700))
                        .map_err(|e| format!("chmod compile dir: {e}"))?;
                }
                return Ok(candidate);
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(format!("create compile dir: {e}")),
        }
    }
    Err("create compile dir: exhausted unique directory names".to_string())
}

fn create_private_diag_file(path: &Path) -> Result<fs::File, String> {
    let mut opts = OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    opts.mode(0o600);
    opts.open(path)
        .map_err(|e| format!("create diag file {}: {e}", path.display()))
}

/// Nonblocking background compilation (+ optional door-check) job spawned by live upgrade
/// (`struct compile_job`).
pub struct CompileJob {
    work_dir: PathBuf,
    executable: PathBuf,
    library: Option<PathBuf>,
    source_path: PathBuf,
    old_path: Option<PathBuf>,
    ctx_path: Option<PathBuf>,
    output_bc: PathBuf,
    output_ctx: PathBuf,
    diag_file: PathBuf,
    check_diag_file: PathBuf,
    child: Child,
    deadline: Instant,
    run_checks: bool,
    confine: bool,
    checking: bool,
    bundle: Vec<u8>,
    build_digest: [u8; 32],
    ordinal: u64,
}

/// Poll state of a running [`CompileJob`].
pub enum CompilePoll {
    /// Still compiling or running door checks.
    Running,
    /// Finished with either a compiled [`UpgradeImage`] or `(code, class, message)`.
    Finished(Box<Result<UpgradeImage, (u16, &'static str, String)>>),
}

impl CompileJob {
    /// Starts an asynchronous child compile (`<exe> emit --bundle ...`) in a private `0700` temp
    /// dir with cleared environment, followed by `<exe> test --bundle ...` when `run_checks` is
    /// `true`. When `confine` is `true`, both child stages enforce Landlock + Seccomp-BPF +
    /// `RLIMIT_AS`/`RLIMIT_CPU`/`RLIMIT_CORE` fail-closed.
    #[expect(
        clippy::too_many_arguments,
        reason = "spawning a compile child requires executable, library, bundle, old/migration/context inputs, check/confine flags, and identity"
    )]
    pub fn start(
        executable: &Path,
        library: Option<&Path>,
        bundle: Vec<u8>,
        old_bundle: Option<&[u8]>,
        migration: Option<&str>,
        context: Option<&[u8]>,
        run_checks: bool,
        confine: bool,
        build_digest: [u8; 32],
        ordinal: u64,
    ) -> Result<Self, String> {
        let work_dir = create_private_work_dir()?;

        let source_path = work_dir.join("candidate.mosb");
        fs::write(&source_path, &bundle).map_err(|e| format!("write candidate bundle: {e}"))?;

        let output_bc = work_dir.join("candidate.bc");
        let output_ctx = work_dir.join("candidate.bc.context");
        let diag_file = work_dir.join("diagnostics.txt");
        let check_diag_file = work_dir.join("check-diagnostics.txt");

        let mut cmd = Command::new(executable);
        cmd.arg("emit")
            .arg(&source_path)
            .arg("--bundle")
            .arg("-o")
            .arg(&output_bc)
            .arg("--rlimit-child");
        if confine {
            cmd.arg("--confine-child");
        }
        if let Some(lib) = library {
            cmd.arg("--lib").arg(lib);
        }
        let old_path = if let Some(old) = old_bundle {
            let p = work_dir.join("old.mosb");
            fs::write(&p, old).map_err(|e| format!("write old bundle: {e}"))?;
            cmd.arg("--old-bundle").arg(&p);
            Some(p)
        } else {
            None
        };
        if let Some(mig) = migration.filter(|m| !m.is_empty()) {
            cmd.arg("--migrate").arg(mig);
        }
        let ctx_path = if let Some(ctx) = context {
            let p = work_dir.join("context.mocc");
            fs::write(&p, ctx).map_err(|e| format!("write context: {e}"))?;
            cmd.arg("--context").arg(&p);
            Some(p)
        } else {
            None
        };
        let err_handle = create_private_diag_file(&diag_file)?;
        cmd.current_dir(&work_dir)
            .env_clear()
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::from(err_handle));

        let child = cmd
            .spawn()
            .map_err(|e| format!("spawn compiler child: {e}"))?;
        mo_host::confine::set_child_process_group(child.id());
        let deadline = Instant::now() + Duration::from_micros(MO_LIMIT_UPGRADE_COMPILE_WALL_US);
        Ok(Self {
            work_dir,
            executable: executable.to_path_buf(),
            library: library.map(Path::to_path_buf),
            source_path,
            old_path,
            ctx_path,
            output_bc,
            output_ctx,
            diag_file,
            check_diag_file,
            child,
            deadline,
            run_checks,
            confine,
            checking: false,
            bundle,
            build_digest,
            ordinal,
        })
    }

    /// Overrides the active stage wall-clock deadline (used by compile-budget tests).
    pub fn set_deadline_for_test(&mut self, deadline: Instant) {
        self.deadline = deadline;
    }

    /// Returns the diagnostics log file path of the first compile stage.
    #[must_use]
    pub fn diag_file_path(&self) -> &Path {
        &self.diag_file
    }

    fn start_check_stage(&mut self) -> Result<(), String> {
        let mut cmd = Command::new(&self.executable);
        cmd.arg("test")
            .arg(&self.source_path)
            .arg("--bundle")
            .arg("--rlimit-child");
        if self.confine {
            cmd.arg("--confine-child");
        }
        if let Some(ref lib) = self.library {
            cmd.arg("--lib").arg(lib);
        }
        if let Some(ref old_p) = self.old_path {
            cmd.arg("--old-bundle").arg(old_p);
        }
        if let Some(ref ctx_p) = self.ctx_path {
            cmd.arg("--context").arg(ctx_p);
        }
        let err_handle = create_private_diag_file(&self.check_diag_file)?;
        cmd.current_dir(&self.work_dir)
            .env_clear()
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::from(err_handle));
        self.child = cmd.spawn().map_err(|e| format!("spawn check child: {e}"))?;
        mo_host::confine::set_child_process_group(self.child.id());
        self.checking = true;
        self.deadline = Instant::now() + Duration::from_micros(MO_LIMIT_CHECK_WALL_US);
        Ok(())
    }

    /// Nonblocking poll of the child compiler/check process.
    pub fn poll(&mut self) -> CompilePoll {
        match self.child.try_wait() {
            Ok(None) => {
                if Instant::now() >= self.deadline {
                    mo_host::confine::kill_child_tree(self.child.id());
                    let _ = self.child.kill();
                    let _ = self.child.wait();
                    return if self.checking {
                        CompilePoll::Finished(Box::new(Err((
                            ErrorCode::PlanUnfinished as u16,
                            "check",
                            "the checks did not finish inside their limits".to_string(),
                        ))))
                    } else {
                        CompilePoll::Finished(Box::new(Err((
                            ErrorCode::UpgradeCompileBudget as u16,
                            "budget",
                            "candidate compilation exceeded its time limit".to_string(),
                        ))))
                    };
                }
                CompilePoll::Running
            }
            Ok(Some(status)) => {
                if !status.success() {
                    #[cfg(unix)]
                    let sig_xcpu = status.signal() == Some(24);
                    #[cfg(not(unix))]
                    let sig_xcpu = false;
                    if sig_xcpu {
                        return if self.checking {
                            CompilePoll::Finished(Box::new(Err((
                                ErrorCode::PlanUnfinished as u16,
                                "check",
                                "the checks did not finish inside their limits".to_string(),
                            ))))
                        } else {
                            CompilePoll::Finished(Box::new(Err((
                                ErrorCode::UpgradeCompileBudget as u16,
                                "budget",
                                "candidate compilation exceeded its time limit".to_string(),
                            ))))
                        };
                    }
                    let diag_path = if self.checking {
                        &self.check_diag_file
                    } else {
                        &self.diag_file
                    };
                    let raw = fs::read_to_string(diag_path).unwrap_or_default();
                    let detail = raw.trim();
                    if self.checking {
                        let code = if detail.contains("E1433:") {
                            ErrorCode::ChecksFailed as u16
                        } else {
                            ErrorCode::PlanUnfinished as u16
                        };
                        let msg = detail
                            .strip_prefix("error E1433: ")
                            .or_else(|| detail.strip_prefix("error E1432: "))
                            .unwrap_or(if detail.is_empty() {
                                "the checks did not finish inside their limits"
                            } else {
                                detail
                            });
                        return CompilePoll::Finished(Box::new(Err((
                            code,
                            "check",
                            msg.to_string(),
                        ))));
                    }
                    let code = if detail.contains("E1422:") {
                        ErrorCode::UpgradeMigrationSignature as u16
                    } else {
                        ErrorCode::UpgradeCompileDiagnostic as u16
                    };
                    let msg = if detail.is_empty() {
                        "compiler child failed or exceeded its budget".to_string()
                    } else {
                        detail.to_string()
                    };
                    return CompilePoll::Finished(Box::new(Err((code, "compile", msg))));
                }

                // Stage 1 succeeded; launch Stage 2 (`mo test --bundle`) if requested!
                if self.run_checks && !self.checking {
                    if let Err(msg) = self.start_check_stage() {
                        return CompilePoll::Finished(Box::new(Err((
                            ErrorCode::PlanUnfinished as u16,
                            "check",
                            msg,
                        ))));
                    }
                    return CompilePoll::Running;
                }

                let Ok(wire) = fs::read(&self.output_bc) else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compile",
                        "compiled candidate bytecode could not be read".to_string(),
                    ))));
                };
                let Ok(ctx_bytes) = fs::read(&self.output_ctx) else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compile",
                        "compiled candidate context could not be read".to_string(),
                    ))));
                };
                let Some(ctx) = read_compile_context(&ctx_bytes) else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compile",
                        "compiler context failed validation".to_string(),
                    ))));
                };
                let Some(meta_bytes) = ctx.metadata.as_deref() else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compile",
                        "compiled candidate metadata is missing".to_string(),
                    ))));
                };
                let Some(meta) = crate::binary::compile_context::read_compile_metadata(meta_bytes)
                else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compile",
                        "compiled candidate metadata is invalid".to_string(),
                    ))));
                };
                let (image, service) = match load_service_image(&wire, mo_vm::Limits::default()) {
                    Ok(v) => v,
                    Err(msg) => {
                        return CompilePoll::Finished(Box::new(Err((
                            ErrorCode::UpgradeCompileDiagnostic as u16,
                            "compile",
                            msg,
                        ))));
                    }
                };
                let image_key = image.file().program_checksum ^ (u64::from(service) << 32);
                let program = StoredProgram {
                    source: sha256_bytes(&self.bundle),
                    build: self.build_digest,
                    schema: sha256_bytes(&meta.schema),
                    context_digest: ctx.digest,
                    image: image_key,
                    ordinal: self.ordinal,
                    bundle: self.bundle.clone(),
                    context: ctx_bytes,
                };
                CompilePoll::Finished(Box::new(Ok(UpgradeImage {
                    image,
                    service,
                    program,
                    schema_synthesized: false,
                })))
            }
            Err(e) => CompilePoll::Finished(Box::new(Err((
                ErrorCode::UpgradeCompileDiagnostic as u16,
                "compile",
                format!("compiler child wait failed: {e}"),
            )))),
        }
    }

    /// Kills and reaps the child (and its process group) if still running.
    pub fn cancel(&mut self) {
        mo_host::confine::kill_child_tree(self.child.id());
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for CompileJob {
    fn drop(&mut self) {
        self.cancel();
        let _ = fs::remove_dir_all(&self.work_dir);
    }
}

/// Handles `mo emit FILE --bundle -o OUT [--old-bundle OLD] [--migrate M] [--context CTX] [--context-out OUT]`
/// (used both by live upgrade compile children and directly by `tests/errors/e1418`).
#[expect(
    clippy::too_many_arguments,
    reason = "CLI emit --bundle entrypoint forwards all optional bundle compilation flags"
)]
pub fn execute_emit_bundle(
    bundle_path: &Path,
    output_path: &Path,
    old_bundle_path: Option<&Path>,
    migration: Option<&str>,
    context_path: Option<&Path>,
    context_out_path: Option<&Path>,
    library: Option<&Path>,
    build_digest: [u8; 32],
    rlimit_child: bool,
    confine_child: bool,
    errors: &mut dyn Write,
) -> i32 {
    if rlimit_child || confine_child {
        mo_host::confine::enter_own_process_group();
    }
    if rlimit_child
        && let Err(step) = mo_host::confine::apply_child_rlimits(
            MO_LIMIT_UPGRADE_COMPILE_MEMORY,
            MO_LIMIT_UPGRADE_COMPILE_CPU_SECONDS,
        )
    {
        let _ = writeln!(errors, "mo: cannot confine the child: {step}");
        let _ = errors.flush();
        return 125;
    }
    if confine_child {
        let exe = std::env::current_exe()
            .ok()
            .map(|p| fs::canonicalize(&p).unwrap_or(p))
            .unwrap_or_else(|| PathBuf::from("/proc/self/exe"));
        let work_dir = output_path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        let mut read_files: Vec<&Path> = vec![bundle_path];
        if let Some(p) = old_bundle_path {
            read_files.push(p);
        }
        if let Some(p) = context_path {
            read_files.push(p);
        }
        if let Err(step) =
            mo_host::confine::confine_compile_child(&exe, work_dir, library, &read_files)
        {
            let _ = writeln!(errors, "mo: cannot confine the child: {step}");
            let _ = errors.flush();
            return 125;
        }
    }
    let emit_bad_bundle = |err: &mut dyn Write| {
        let s = SourceMap::new();
        let mut d = DiagBag::new();
        let _ = d.push(Diagnostic::new(
            ErrorCode::UpgradeBundleFormat,
            Span::NONE,
            "source bundle is malformed or unreadable",
        ));
        write_bundle_diags(&s, &mut d, err);
    };
    let Ok(bundle_bytes) = fs::read(bundle_path) else {
        emit_bad_bundle(errors);
        return 1;
    };
    let old_bytes = match old_bundle_path {
        Some(p) => match fs::read(p) {
            Ok(b) => Some(b),
            Err(_) => {
                emit_bad_bundle(errors);
                return 1;
            }
        },
        None => None,
    };
    let ctx_bytes = match context_path {
        Some(p) => match fs::read(p) {
            Ok(b) => Some(b),
            Err(_) => {
                emit_bad_bundle(errors);
                return 1;
            }
        },
        None => None,
    };

    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let compiled = compile_bundle(
        &bundle_bytes,
        old_bytes.as_deref(),
        migration,
        ctx_bytes.as_deref(),
        library,
        build_digest,
        &mut diags,
        &mut sources,
    );
    let Some(res) = compiled else {
        if let Some(first) = diags.items().first() {
            let msg = first.message.as_str_lossy();
            if first.code == ErrorCode::UpgradeRecordedCompile {
                let _ = write!(errors, "{msg}");
                if !msg.ends_with('\n') {
                    let _ = writeln!(errors);
                }
                let _ = errors.flush();
                return 70;
            }
            if first.code == ErrorCode::UpgradeTimerInterval {
                let _ = write!(errors, "{msg}");
                if !msg.ends_with('\n') {
                    let _ = writeln!(errors);
                }
                let _ = errors.flush();
                return 69;
            }
        }
        write_bundle_diags(&sources, &mut diags, errors);
        return 1;
    };

    if fs::write(output_path, &res.wire).is_err() {
        let _ = writeln!(errors, "mo: could not write compiled bytecode");
        return 1;
    }
    let default_ctx_out = {
        let mut s = output_path.as_os_str().to_os_string();
        s.push(".context");
        PathBuf::from(s)
    };
    let ctx_dest = context_out_path.unwrap_or(default_ctx_out.as_path());
    if fs::write(ctx_dest, &res.program.context).is_err() {
        let _ = writeln!(errors, "mo: could not write compile context");
        return 1;
    }
    0
}

/// Synchronously compiles a bundle in-process into an [`UpgradeImage`] (used by `mo serve`
/// startup, `mo check --upgrade-from`, and `mo rehearse`).
///
/// Maps compiler diagnostic errors to `E1419` (`UpgradeCompileDiagnostic`), preserving
/// `E1422` (`UpgradeMigrationSignature`), `E1427` (`UpgradeTimerInterval`), and `E1429`
/// (`UpgradeRecordedCompile` for initializer failure) matching `src/tools/upgrade.c:243-261`.
#[expect(
    clippy::too_many_arguments,
    reason = "synchronous bundle compile forwards all inputs and populates UpgradeReport"
)]
pub fn compile_image_sync(
    bundle_bytes: &[u8],
    old_bundle: Option<&[u8]>,
    migration: Option<&str>,
    context: Option<&[u8]>,
    library: Option<&Path>,
    build_digest: [u8; 32],
    ordinal: u64,
    report: &mut UpgradeReport,
) -> Option<UpgradeImage> {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let compiled = compile_bundle(
        bundle_bytes,
        old_bundle,
        migration,
        context,
        library,
        build_digest,
        &mut diags,
        &mut sources,
    );
    let Some(mut res) = compiled else {
        let mut detail = String::new();
        let mut has_mig_sig = false;
        let mut has_init_fail = false;
        let mut has_timer_fail = false;
        for d in diags.items() {
            if d.code == ErrorCode::UpgradeMigrationSignature {
                has_mig_sig = true;
            } else if d.code == ErrorCode::UpgradeRecordedCompile {
                has_init_fail = true;
            } else if d.code == ErrorCode::UpgradeTimerInterval {
                has_timer_fail = true;
            }
            if matches!(
                d.code,
                ErrorCode::UpgradeRecordedCompile | ErrorCode::UpgradeTimerInterval
            ) {
                let msg = d.message.as_str_lossy();
                detail.push_str(&msg);
                if !msg.ends_with('\n') {
                    detail.push('\n');
                }
            } else {
                detail.push_str(&format!("error E{:04}: {}\n", d.code as u16, d.message));
            }
        }
        let code = if has_init_fail {
            ErrorCode::UpgradeRecordedCompile as u16
        } else if has_timer_fail {
            ErrorCode::UpgradeTimerInterval as u16
        } else if has_mig_sig {
            ErrorCode::UpgradeMigrationSignature as u16
        } else {
            ErrorCode::UpgradeCompileDiagnostic as u16
        };
        let summary = detail.trim();
        let msg = if summary.is_empty() {
            "candidate source failed to compile".to_string()
        } else {
            summary.to_string()
        };
        report.detail = detail;
        report.refuse(code, "compile", msg);
        return None;
    };
    res.program.ordinal = ordinal;
    Some(UpgradeImage {
        image: res.image,
        service: res.service_idx,
        program: res.program,
        schema_synthesized: res.schema_synthesized,
    })
}
