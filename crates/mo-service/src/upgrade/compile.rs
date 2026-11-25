//! Safe Landlock confinement check + child process compilation for live upgrades
//! (`src/host/confine.c`, `src/host/compile.c`, and `mo emit --bundle` parity).

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, RenderFormat, SourceMap, Span, TerminalEncoding,
    render_diagnostic_default,
};
use mo_def::ErrorCode;
use mo_def::limits::MO_LIMIT_UPGRADE_COMPILE_WALL_US;

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

/// Returns `true` if the running kernel supports Linux Landlock ABI >= 3
/// (`src/host/confine.c:mo_compile_can_confine`).
#[must_use]
pub fn can_confine() -> bool {
    #[cfg(target_os = "linux")]
    {
        use landlock::{ABI, Access, AccessFs, Ruleset, RulesetAttr};
        Ruleset::default()
            .handle_access(AccessFs::from_all(ABI::V3))
            .and_then(Ruleset::create)
            .is_ok()
    }
    #[cfg(not(target_os = "linux"))]
    {
        false
    }
}

/// Applies Landlock filesystem confinement to the current child process when invoked for
/// `--bundle` compilation on Linux (zero `unsafe`).
pub fn confine_self_for_bundle(work_dir: &Path, library: Option<&Path>) {
    #[cfg(target_os = "linux")]
    {
        use landlock::{
            ABI, Access, AccessFs, PathBeneath, PathFd, Ruleset, RulesetAttr, RulesetCreatedAttr,
        };
        let abi = ABI::V3;
        let Ok(ruleset) = Ruleset::default()
            .handle_access(AccessFs::from_all(abi))
            .and_then(Ruleset::create)
        else {
            return;
        };
        let read_only = AccessFs::from_read(abi);
        let read_write = AccessFs::from_all(abi);
        let mut created = ruleset;
        for sys in ["/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/dev/null"] {
            if let Ok(fd) = PathFd::new(sys) {
                if let Ok(next) = created.add_rule(PathBeneath::new(fd, read_only)) {
                    created = next;
                } else {
                    return;
                }
            }
        }
        if let Some(lib_dir) = library
            && let Ok(fd) = PathFd::new(lib_dir)
        {
            match created.add_rule(PathBeneath::new(fd, read_only)) {
                Ok(next) => created = next,
                Err(_) => return,
            }
        }
        if let Ok(fd) = PathFd::new(work_dir) {
            match created.add_rule(PathBeneath::new(fd, read_write)) {
                Ok(next) => created = next,
                Err(_) => return,
            }
        }
        let _ = created.restrict_self();
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (work_dir, library);
    }
}

/// Nonblocking background compilation job spawned by live upgrade commit (`struct compile_job`).
pub struct CompileJob {
    work_dir: PathBuf,
    output_bc: PathBuf,
    output_ctx: PathBuf,
    diag_file: PathBuf,
    child: Child,
    deadline: Instant,
    bundle: Vec<u8>,
    build_digest: [u8; 32],
    ordinal: u64,
}

/// Poll state of a running [`CompileJob`].
pub enum CompilePoll {
    /// Still compiling.
    Running,
    /// Finished with either a compiled [`UpgradeImage`] or an error message/code.
    Finished(Box<Result<UpgradeImage, (u16, String, String)>>),
}

impl CompileJob {
    /// Starts an asynchronous child compile (`<exe> emit --bundle ...`) in a private temp dir.
    #[expect(
        clippy::too_many_arguments,
        reason = "spawning a compile child requires executable, library, bundle, old/migration/context inputs, and identity"
    )]
    pub fn start(
        executable: &Path,
        library: Option<&Path>,
        bundle: Vec<u8>,
        old_bundle: Option<&[u8]>,
        migration: Option<&str>,
        context: Option<&[u8]>,
        build_digest: [u8; 32],
        ordinal: u64,
    ) -> Result<Self, String> {
        let mut rnd = [0u8; 8];
        let _ = getrandom::fill(&mut rnd);
        let name = format!("mo-upgrade-{:016x}", u64::from_le_bytes(rnd));
        let work_dir = std::env::temp_dir().join(name);
        fs::create_dir_all(&work_dir).map_err(|e| format!("create compile dir: {e}"))?;

        let source_path = work_dir.join("candidate.mosb");
        fs::write(&source_path, &bundle).map_err(|e| format!("write candidate bundle: {e}"))?;

        let output_bc = work_dir.join("candidate.bc");
        let output_ctx = work_dir.join("candidate.bc.context");
        let diag_file = work_dir.join("diagnostics.txt");

        let mut cmd = Command::new(executable);
        cmd.arg("emit")
            .arg(&source_path)
            .arg("--bundle")
            .arg("-o")
            .arg(&output_bc);
        if let Some(lib) = library {
            cmd.arg("--lib").arg(lib);
        }
        if let Some(old) = old_bundle {
            let old_path = work_dir.join("old.mosb");
            fs::write(&old_path, old).map_err(|e| format!("write old bundle: {e}"))?;
            cmd.arg("--old-bundle").arg(&old_path);
        }
        if let Some(mig) = migration.filter(|m| !m.is_empty()) {
            cmd.arg("--migrate").arg(mig);
        }
        if let Some(ctx) = context {
            let ctx_path = work_dir.join("context.mocc");
            fs::write(&ctx_path, ctx).map_err(|e| format!("write context: {e}"))?;
            cmd.arg("--context").arg(&ctx_path);
        }
        let err_handle =
            fs::File::create(&diag_file).map_err(|e| format!("create diag file: {e}"))?;
        cmd.stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::from(err_handle));

        let child = cmd
            .spawn()
            .map_err(|e| format!("spawn compiler child: {e}"))?;
        let deadline = Instant::now() + Duration::from_micros(MO_LIMIT_UPGRADE_COMPILE_WALL_US);
        Ok(Self {
            work_dir,
            output_bc,
            output_ctx,
            diag_file,
            child,
            deadline,
            bundle,
            build_digest,
            ordinal,
        })
    }

    /// Nonblocking poll of the child compiler process.
    pub fn poll(&mut self) -> CompilePoll {
        match self.child.try_wait() {
            Ok(None) => {
                if Instant::now() >= self.deadline {
                    let _ = self.child.kill();
                    let _ = self.child.wait();
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileBudget as u16,
                        "candidate compilation exceeded its time limit".to_string(),
                        String::new(),
                    ))));
                }
                CompilePoll::Running
            }
            Ok(Some(status)) => {
                let diag = fs::read_to_string(&self.diag_file).unwrap_or_default();
                if !status.success() {
                    let code = if status.code() == Some(70) {
                        ErrorCode::UpgradeRecordedCompile as u16
                    } else {
                        ErrorCode::UpgradeCompileDiagnostic as u16
                    };
                    return CompilePoll::Finished(Box::new(Err((
                        code,
                        "candidate source failed to compile".to_string(),
                        diag,
                    ))));
                }
                let Ok(wire) = fs::read(&self.output_bc) else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compiled candidate bytecode could not be read".to_string(),
                        diag,
                    ))));
                };
                let Ok(ctx_bytes) = fs::read(&self.output_ctx) else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeCompileDiagnostic as u16,
                        "compiled candidate context could not be read".to_string(),
                        diag,
                    ))));
                };
                let Some(ctx) = read_compile_context(&ctx_bytes) else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeInvalidValue as u16,
                        "compiled candidate context is invalid".to_string(),
                        diag,
                    ))));
                };
                let Some(meta_bytes) = ctx.metadata.as_deref() else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeInvalidValue as u16,
                        "compiled candidate metadata is missing".to_string(),
                        diag,
                    ))));
                };
                let Some(meta) = crate::binary::compile_context::read_compile_metadata(meta_bytes)
                else {
                    return CompilePoll::Finished(Box::new(Err((
                        ErrorCode::UpgradeInvalidValue as u16,
                        "compiled candidate metadata is invalid".to_string(),
                        diag,
                    ))));
                };
                let (image, service) = match load_service_image(&wire, mo_vm::Limits::default()) {
                    Ok(v) => v,
                    Err(msg) => {
                        return CompilePoll::Finished(Box::new(Err((
                            ErrorCode::UpgradeRecordedCompile as u16,
                            msg,
                            diag,
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
                format!("compiler child wait failed: {e}"),
                String::new(),
            )))),
        }
    }

    /// Kills and reaps the child if still running.
    pub fn cancel(&mut self) {
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
    errors: &mut dyn Write,
) -> i32 {
    if let Some(parent) = output_path.parent() {
        confine_self_for_bundle(parent, library);
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
/// startup and `mo check --upgrade-from`).
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
        let code = diags
            .items()
            .first()
            .map_or(ErrorCode::UpgradeCompileDiagnostic as u16, |d| {
                d.code as u16
            });
        let mut detail = String::new();
        for d in diags.items() {
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
        report.detail = detail;
        report.refuse(code, "compile", "candidate source failed to compile");
        return None;
    };
    res.program.ordinal = ordinal;
    Some(UpgradeImage {
        image: res.image,
        service: res.service_idx,
        program: res.program,
        schema_synthesized: false,
    })
}
