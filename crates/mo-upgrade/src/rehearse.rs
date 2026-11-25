//! Offline upgrade rehearsal (`mo rehearse`, `src/tools/upgrade-commands.c:mo_upgrade_rehearse`).
//!
//! Validates a state export artifact (`MOEX\x01`), compiles the old bundle and candidate bundle
//! with optional `--migrate` and `--context`, checks descriptor compatibility, and executes the
//! migration under the live upgrade work/heap budget without mutating any store.

use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::path::Path;

use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_EXPORT_BYTES};
use mo_service::binary::compile_context::read_compile_context;
use mo_service::binary::export::read_export;
use mo_service::binary::hex_encode;
use mo_service::history::build_refusal;
use mo_service::upgrade::compile::compile_image_sync;
use mo_service::upgrade::{UpgradeReport, compare_programs, program_metadata, run_migration_state};

/// Options for `mo rehearse --artifact ARTIFACT --old-bundle BUNDLE --candidate FILE ...`.
pub struct RehearseOptions<'a> {
    /// Exported state artifact (`MOEX\x01`, kind == 1).
    pub artifact: &'a Path,
    /// Previous program bundle (`MOSB\x01`).
    pub old_bundle: &'a Path,
    /// Candidate program bundle (`MOSB\x01`).
    pub candidate: &'a Path,
    /// Optional migration function name (`--migrate`).
    pub migration: Option<&'a str>,
    /// Optional recorded compile context file (`MOCC\x01`, `--context`).
    pub compile_context: Option<&'a Path>,
    /// Optional explicit library directory (`--lib`).
    pub library: Option<&'a Path>,
    /// Emit JSON report on stdout.
    pub json: bool,
    /// 32-byte build runtime identity digest.
    pub build_digest: [u8; 32],
}

/// Reads a bounded regular file without blocking on FIFOs/devices (`rehearsal_bytes` parity).
#[cfg(unix)]
pub(crate) fn read_bounded_regular_file(path: &Path, limit: u64) -> Option<Vec<u8>> {
    use std::os::unix::fs::OpenOptionsExt;

    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(rustix::fs::OFlags::NONBLOCK.bits() as i32)
        .open(path)
        .ok()?;
    let meta = file.metadata().ok()?;
    if !meta.is_file() || meta.len() > limit {
        return None;
    }
    let mut out = Vec::with_capacity((meta.len() as usize).min(limit as usize));
    let mut chunk = [0u8; 8192];
    loop {
        match file.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                if (out.len() + n) as u64 > limit {
                    return None;
                }
                out.extend_from_slice(&chunk[..n]);
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(_) => return None,
        }
    }
    Some(out)
}

#[cfg(not(unix))]
pub(crate) fn read_bounded_regular_file(path: &Path, limit: u64) -> Option<Vec<u8>> {
    let meta = std::fs::metadata(path).ok()?;
    if !meta.is_file() || meta.len() > limit {
        return None;
    }
    let bytes = std::fs::read(path).ok()?;
    (bytes.len() as u64 <= limit).then_some(bytes)
}

/// Executes `mo rehearse` (`src/tools/upgrade-commands.c:mo_upgrade_rehearse`).
pub fn run_rehearse(
    opts: &RehearseOptions<'_>,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    let mut report = UpgradeReport {
        class: "artifact".to_string(),
        message: "cannot read a complete state export".to_string(),
        rehearsal: true,
        ..UpgradeReport::default()
    };

    let Some(artifact_bytes) = read_bounded_regular_file(opts.artifact, MO_LIMIT_EXPORT_BYTES)
    else {
        report.refuse(
            ErrorCode::UpgradeInvalidValue as u16,
            "artifact",
            "cannot read a complete state export",
        );
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    };
    let Some(exp) = read_export(&artifact_bytes).filter(|e| e.identity.kind == 1) else {
        report.refuse(
            ErrorCode::UpgradeInvalidValue as u16,
            "artifact",
            "cannot read a complete state export",
        );
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    };

    if build_refusal(&exp.identity.build, &exp.identity.label, &opts.build_digest).is_some() {
        report.refuse(
            ErrorCode::UpgradeSourceMismatch as u16,
            "build",
            format!(
                "export build {} differs from executing build {}",
                hex_encode(&exp.identity.build),
                hex_encode(&opts.build_digest)
            ),
        );
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    }

    let Some(old_bundle) = read_bounded_regular_file(opts.old_bundle, MO_LIMIT_BUNDLE_BYTES) else {
        report.refuse(
            ErrorCode::UpgradeBundleFormat as u16,
            "bundle",
            "bundle or compilation context cannot be read or validated",
        );
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    };
    let Some(candidate) = read_bounded_regular_file(opts.candidate, MO_LIMIT_BUNDLE_BYTES) else {
        report.refuse(
            ErrorCode::UpgradeBundleFormat as u16,
            "bundle",
            "bundle or compilation context cannot be read or validated",
        );
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    };
    let recorded_context = match opts.compile_context {
        None => None,
        Some(ctx_path) => {
            let Some(bytes) = read_bounded_regular_file(ctx_path, MO_LIMIT_BUNDLE_BYTES) else {
                report.refuse(
                    ErrorCode::UpgradeBundleFormat as u16,
                    "bundle",
                    "bundle or compilation context cannot be read or validated",
                );
                return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
            };
            if read_compile_context(&bytes).is_none() {
                report.refuse(
                    ErrorCode::UpgradeBundleFormat as u16,
                    "bundle",
                    "bundle or compilation context cannot be read or validated",
                );
                return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
            }
            Some(bytes)
        }
    };

    let retained = recorded_context.as_deref();
    let Some(old_img) = compile_image_sync(
        &old_bundle,
        None,
        None,
        retained,
        opts.library,
        opts.build_digest,
        0,
        &mut report,
    ) else {
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    };

    let exp_image_key = exp.identity.program ^ (u64::from(exp.identity.service) << 32);
    if old_img.program.image != exp_image_key
        || old_img.service != exp.identity.service
        || old_img.program.schema != exp.schema
    {
        report.refuse(
            ErrorCode::UpgradeSourceMismatch as u16,
            "program",
            format!(
                "export program {:016x} differs from old bundle program {:016x} (or service/schema differs)",
                exp.identity.program, old_img.program.image
            ),
        );
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    }

    let next_old = if retained.is_some() && opts.migration.is_none() {
        None
    } else {
        Some(old_bundle.as_slice())
    };
    let Some(next_img) = compile_image_sync(
        &candidate,
        next_old,
        opts.migration,
        retained,
        opts.library,
        opts.build_digest,
        1,
        &mut report,
    ) else {
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    };

    let old_meta = program_metadata(&old_img.program);
    let next_meta = program_metadata(&next_img.program);
    if !compare_programs(
        &old_img.program,
        old_meta.as_ref(),
        &next_img.program,
        next_meta.as_ref(),
        opts.migration,
        &mut report,
    ) {
        return emit_rehearsal(opts.json, opts.migration, &report, output, errors);
    }

    let old_state_type_name = old_img
        .image
        .file()
        .types
        .get(old_img.image.file().services[old_img.service as usize].state_type)
        .name
        .as_deref();

    if run_migration_state(
        &next_img.image,
        next_img.service,
        opts.migration,
        old_state_type_name,
        exp.state,
        true,
        old_img.schema_synthesized || next_img.schema_synthesized,
        &mut report,
    )
    .is_some()
    {
        report.state_valid = true;
        report.head = exp.identity.sequence;
        report.message = "migrated state is valid; rehearsal is not upgrade acceptance".to_string();
    }

    emit_rehearsal(opts.json, opts.migration, &report, output, errors)
}

fn emit_rehearsal(
    json: bool,
    migration: Option<&str>,
    report: &UpgradeReport,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if json {
        let mut s = report.to_json(migration);
        s.push('\n');
        let _ = output.write_all(s.as_bytes());
        let _ = output.flush();
    } else if report.code == 0 {
        let _ = output.write_all(report.to_text().as_bytes());
        let _ = output.flush();
    } else {
        let _ = errors.write_all(report.to_text().as_bytes());
        let _ = errors.flush();
    }
    if report.code == 0 { 0 } else { 1 }
}
