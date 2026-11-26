//! Durable `mo serve` runner (`src/tools/serve.c` and `src/tools/upgrade-commands.c:mo_upgrade_serve` parity).

use std::io::Write;
use std::path::{Path, PathBuf};

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, RenderFormat, SourceMap, Span, TerminalEncoding,
    render_diagnostic_default,
};
use mo_def::ErrorCode;
use mo_def::limits::MO_CLI_SERVE_TICK_US;
use mo_vm::Limits;

/// Exit status when the service supervisor permanently stops the service (`D308`).
pub const MO_EXIT_SERVICE_STOPPED: i32 = 69;
/// Exit status when another process holds `service.sqlite` for the entire open wait.
pub const MO_EXIT_STORE_BUSY: i32 = 75;

const UNICODE_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: mo_prim::unicode16::next_grapheme_bytes,
    display_width: mo_prim::unicode16::display_width_bytes,
};

fn emit_diag(errors: &mut dyn Write, code: ErrorCode, msg: &str) {
    let sources = SourceMap::new();
    let d = Diagnostic::new(code, Span::NONE, msg);
    if let Ok(buf) = render_diagnostic_default(
        &sources,
        &d,
        RenderFormat::Text,
        TerminalEncoding::Utf8,
        UNICODE_METRICS,
    ) {
        let _ = errors.write_all(&buf);
        let _ = errors.flush();
    } else {
        let _ = writeln!(errors, "error E{:04}: {msg}", code as u16);
    }
}

fn emit_diag_bag(errors: &mut dyn Write, sources: &SourceMap, diags: &mut DiagBag) {
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
        }
    }
    let _ = errors.flush();
}

use crate::binary::compile_context::{
    read_compile_context, read_compile_metadata, write_compile_metadata,
};
use crate::binary::export::export_schema_digest;
use crate::binary::hex_encode;
use crate::listener::signals::{install_signal_handlers, is_signalled, reset_signal};
use crate::listener::{Listener, ListenerConfig, ListenerStatus};
use crate::protocol::MO_BUILD_LABEL;
use crate::protocol::server::{AdminConfig, ProtocolServer};
use crate::service_loop::{ServiceEvent, ServiceLoop, service_status_text};
use crate::store::programs::{ActivationStatus, StoreActivation, StoredProgram};
use crate::store::{ServiceStore, StoreRecord, StoreRecordKind, StoreStatus};
use crate::upgrade::bundle::bundle_from_path;
use crate::upgrade::compile::{CompileJob, CompilePoll, compile_image_sync};
use crate::upgrade::{
    UpgradeChildren, UpgradeImage, UpgradeReport, compare_programs, program_metadata,
    run_migration_state,
};

/// Options for starting a durable `mo serve` instance.
pub struct ServeOptions<'a> {
    /// Optional entry `.mo` file path (`None` restarts from recorded `program.bundle`).
    pub entry_file: Option<&'a [u8]>,
    /// Required data folder path (`--data`).
    pub data_dir: &'a Path,
    /// TCP port (`0` selects an ephemeral port).
    pub port: u16,
    /// Optional explicit bind address (defaults to `127.0.0.1`).
    pub bind: Option<&'a str>,
    /// Optional `--allow-host` authority.
    pub allow_host: Option<&'a str>,
    /// Optional operator Unix domain socket (`--admin-socket`).
    pub admin_socket: Option<&'a Path>,
    /// Optional path where readiness JSON is written atomically once listening (`--ready-file`).
    pub ready_file: Option<&'a Path>,
    /// Optional explicit library directory (`--lib`).
    pub library: Option<&'a Path>,
    /// Whether `--upgrade` was passed (allows changing recorded program on startup).
    pub upgrade: bool,
    /// Whether `--adopt` was passed.
    pub adopt: bool,
    /// Optional migration function name (`--migrate`).
    pub migration: Option<&'a str>,
    /// Whether `--unconfined` was passed.
    pub unconfined: bool,
    /// VM limits (`--max-heap`, `--max-stack`, `--max-depth`).
    pub limits: Limits,
    /// Running executable path (for spawning compile children).
    pub executable: PathBuf,
    /// 32-byte build runtime identity digest.
    pub build_digest: [u8; 32],
}

/// Runs a durable service via `mo serve` (`mo_upgrade_serve` + `mo_serve_image`).
#[cfg(unix)]
pub fn run_serve_service(
    opts: &ServeOptions<'_>,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    reset_signal();
    if let Err(e) = install_signal_handlers() {
        emit_diag(
            errors,
            ErrorCode::ToolServiceStart,
            &format!("mo serve could not watch for stop signals: {e}"),
        );
        return 1;
    }

    // Validate data folder exists and is a directory BEFORE opening SQLite.
    match std::fs::metadata(opts.data_dir) {
        Ok(m) if m.is_dir() => {}
        Ok(_) => {
            emit_diag(
                errors,
                ErrorCode::ToolServiceStart,
                &format!(
                    "mo serve can't use the data folder {}: Not a directory",
                    opts.data_dir.display()
                ),
            );
            return 1;
        }
        Err(e) => {
            emit_diag(
                errors,
                ErrorCode::ToolServiceStart,
                &format!(
                    "mo serve can't use the data folder {}: {}",
                    opts.data_dir.display(),
                    io_err_short(&e)
                ),
            );
            return 1;
        }
    }

    // Bundle entry file (if provided) before touching the database.
    let mut candidate_bundle: Option<(Vec<u8>, [u8; 32])> = None;
    if let Some(entry_raw) = opts.entry_file {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        match bundle_from_path(entry_raw, opts.library, true, &mut diags, &mut sources) {
            Some(pair) => candidate_bundle = Some(pair),
            None => {
                if diags.has_any() {
                    emit_diag_bag(errors, &sources, &mut diags);
                } else {
                    emit_diag(
                        errors,
                        ErrorCode::ToolServiceStart,
                        "mo serve could not bundle the entry source",
                    );
                }
                return 1;
            }
        }
    }

    let db_path = opts.data_dir.join("service.sqlite");
    let mut store = match ServiceStore::open_wait(&db_path) {
        Ok(s) => s,
        Err(StoreStatus::Busy) => {
            let _ = writeln!(
                errors,
                "mo serve: could not open saved state at {}: {}; stopped waiting",
                db_path.display(),
                StoreStatus::Busy.text().unwrap_or("store busy")
            );
            return MO_EXIT_STORE_BUSY;
        }
        Err(st) => {
            emit_diag(
                errors,
                ErrorCode::ToolServiceStart,
                &format!(
                    "mo serve could not open saved state at {}: {}",
                    db_path.display(),
                    st.text().unwrap_or("store error")
                ),
            );
            return 1;
        }
    };

    let recorded = match store.read_program() {
        Ok(p) => p,
        Err(_) => {
            let mut r = UpgradeReport::default();
            r.refuse(
                ErrorCode::UpgradeInvalidValue as u16,
                "store",
                "recorded program or saved state is invalid",
            );
            let _ = write!(errors, "{}", r.to_text());
            return 1;
        }
    };

    // Inspect store head & snapshot without replaying yet (`src/tools/upgrade-commands.c:280`).
    let restart_records = match store.load() {
        Ok(recs) => recs,
        Err(_) => {
            let mut r = UpgradeReport::default();
            r.refuse(
                ErrorCode::UpgradeInvalidValue as u16,
                "store",
                "recorded program or saved state is invalid",
            );
            let _ = write!(errors, "{}", r.to_text());
            return 1;
        }
    };
    let head_info: Option<(StoreRecord, u64, u64, u64)> = if restart_records.is_empty() {
        None
    } else {
        let last_image = restart_records[restart_records.len() - 1].image;
        let last_seq = restart_records[restart_records.len() - 1].sequence;
        let log_records = (restart_records.len() - 1) as u64;
        let snap = restart_records[0].clone();
        Some((snap, last_image, last_seq, log_records))
    };

    // Determine which bundle to compile and whether this is a source change.
    let (bundle_bytes, cand_digest) = match candidate_bundle {
        Some(pair) => pair,
        None => match &recorded {
            Some(prog) => (prog.bundle.clone(), prog.source),
            None => {
                let mut r = UpgradeReport::default();
                r.refuse(
                    ErrorCode::UpgradeSourceMismatch as u16,
                    "source",
                    "this folder has no recorded program; provide a source file",
                );
                let _ = write!(errors, "{}", r.to_text());
                return 1;
            }
        },
    };

    let mut report = UpgradeReport {
        candidate: Some(cand_digest),
        base: recorded.as_ref().map(|r| r.source),
        ..UpgradeReport::default()
    };

    let changed = recorded.as_ref().is_some_and(|r| r.source != cand_digest);
    if changed && !opts.upgrade {
        if let Some(ref rec) = recorded {
            report.refuse(
                ErrorCode::UpgradeSourceMismatch as u16,
                "source",
                format!(
                    "source differs from recorded digest {}; use --upgrade",
                    hex_encode(&rec.source)
                ),
            );
            let _ = write!(errors, "{}", report.to_text());
        }
        return 1;
    }

    let children = UpgradeChildren::detect(opts.unconfined);
    if changed && !report.check_children(children) {
        let _ = write!(errors, "{}", report.to_text());
        return 1;
    }

    // If source changed, compile the old recorded program first; if it fails to compile and the
    // log tail is empty, fall back to its recorded `MOCS\x01` metadata (and schema-only fallback).
    let mut old_compiled: Option<UpgradeImage> = None;
    let mut fallback_meta: Option<Vec<u8>> = None;
    let mut fallback_state = false;
    if changed {
        let Some(ref prev_prog) = recorded else {
            report.refuse(
                ErrorCode::UpgradeInvalidValue as u16,
                "store",
                "missing recorded program for upgrade",
            );
            let _ = write!(errors, "{}", report.to_text());
            return 1;
        };
        let mut old_report = UpgradeReport::default();
        if let Some(img) = compile_image_sync(
            &prev_prog.bundle,
            None,
            None,
            Some(&prev_prog.context),
            opts.library,
            opts.build_digest,
            prev_prog.ordinal,
            &mut old_report,
        ) {
            old_compiled = Some(img);
        } else {
            let empty_tail = head_info
                .as_ref()
                .is_some_and(|(snap, _, seq, _)| snap.sequence == *seq);
            let raw_meta = read_compile_context(&prev_prog.context).and_then(|c| c.metadata);
            if !empty_tail || raw_meta.is_none() || program_metadata(prev_prog).is_none() {
                report.refuse(
                    ErrorCode::UpgradeRecordedCompile as u16,
                    "compile",
                    "recorded source cannot compile and its log tail cannot be replayed",
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            }
            fallback_meta = raw_meta;
            fallback_state = true;
        }
    }

    let requested_migration = if changed { opts.migration } else { None };
    let next_ordinal = if changed {
        recorded.as_ref().map_or(1, |r| r.ordinal + 1)
    } else {
        recorded.as_ref().map_or(0, |r| r.ordinal)
    };

    let cand_old_input: Option<&[u8]> = if requested_migration.is_none() {
        None
    } else if fallback_state {
        fallback_meta.as_deref()
    } else {
        recorded.as_ref().map(|r| r.bundle.as_slice())
    };
    let cand_ctx_input: Option<&[u8]> = if !fallback_state || requested_migration.is_none() {
        recorded.as_ref().map(|r| r.context.as_slice())
    } else {
        None
    };

    let mut cand_img = compile_image_sync(
        &bundle_bytes,
        cand_old_input,
        requested_migration,
        cand_ctx_input,
        opts.library,
        opts.build_digest,
        next_ordinal,
        &mut report,
    );
    let mut schema_synthesized = cand_img.as_ref().is_some_and(|c| c.schema_synthesized);

    // Retry with schema-only synthesized UpgradeOld when metadata-types fallback failed.
    if cand_img.is_none()
        && fallback_state
        && requested_migration.is_some()
        && let Some(ref prev_prog) = recorded
        && let Some(schema_only) = build_schema_only_metadata(prev_prog)
    {
        report.code = 0;
        report.message.clear();
        report.detail.clear();
        cand_img = compile_image_sync(
            &bundle_bytes,
            Some(&schema_only),
            requested_migration,
            None,
            opts.library,
            opts.build_digest,
            next_ordinal,
            &mut report,
        );
        if cand_img.is_some() {
            schema_synthesized = true;
        }
    }

    // For a changed startup upgrade, also run the door checks child (`mo test --bundle`) matching
    // `src/tools/upgrade-commands.c:362` (`check = changed`).
    if changed
        && cand_img.is_some()
        && let Err((code, class, msg)) = run_startup_check_child(
            &opts.executable,
            opts.library,
            &bundle_bytes,
            if schema_synthesized {
                recorded.as_ref().and_then(build_schema_only_metadata)
            } else {
                cand_old_input.map(<[u8]>::to_vec)
            }
            .as_deref(),
            requested_migration,
            if schema_synthesized {
                None
            } else {
                cand_ctx_input
            },
            children == UpgradeChildren::Confined,
            opts.build_digest,
            next_ordinal,
        )
    {
        report.refuse(code, class, msg);
        let _ = write!(errors, "{}", report.to_text());
        return 1;
    }

    let Some(mut cand_img) = cand_img else {
        if recorded.is_none() && head_info.is_none() {
            if report.code == ErrorCode::UpgradeRecordedCompile as u16 {
                let _ = write!(errors, "{}", report.detail);
                let _ = errors.flush();
                return 70;
            }
            if report.code == ErrorCode::UpgradeTimerInterval as u16 {
                let _ = write!(errors, "{}", report.detail);
                emit_diag(
                    errors,
                    ErrorCode::ToolServiceStart,
                    "mo serve could not start the service: service handler failed",
                );
                return 1;
            }
        }
        if let Some(ref rec) = recorded
            && !changed
        {
            let diag_msg = report.message.clone();
            report.refuse(
                ErrorCode::UpgradeRecordedCompile as u16,
                "compile",
                format!(
                    "recorded source from build {} could not compile: {diag_msg}",
                    hex_encode(&rec.build)
                ),
            );
        }
        let _ = write!(errors, "{}", report.to_text());
        return 1;
    };

    if changed && let Some(ref prev_prog) = recorded {
        let old_meta = program_metadata(prev_prog);
        let next_meta = program_metadata(&cand_img.program);
        if !compare_programs(
            prev_prog,
            old_meta.as_ref(),
            &cand_img.program,
            next_meta.as_ref(),
            opts.migration,
            &mut report,
        ) {
            let _ = write!(errors, "{}", report.to_text());
            return 1;
        }
    }

    // Validate bearer token env var and open primary TCP listener BEFORE mutating store!
    let token_env = cand_img
        .image
        .file()
        .string(cand_img.image.file().services[cand_img.service as usize].token_env)
        .to_string();
    if let Some(ready_path) = opts.ready_file {
        crate::clear_ready_file(ready_path);
    }
    let listener = match Listener::open_tcp(
        &ListenerConfig {
            address: opts.bind.map(str::to_string),
            port: opts.port,
            expose: false,
        },
        Some(&token_env),
    ) {
        Ok(l) => l,
        Err(ListenerStatus::Credential) => {
            emit_diag(
                errors,
                ErrorCode::ToolServiceStart,
                &format!(
                    "mo serve could not read the token from environment variable {token_env}: it is missing or invalid"
                ),
            );
            return 1;
        }
        Err(st) => {
            emit_diag(
                errors,
                ErrorCode::ToolServiceStart,
                &format!(
                    "mo serve could not listen on port {}: {}",
                    opts.port,
                    st.text().unwrap_or("listen failed")
                ),
            );
            return 1;
        }
    };

    let admin_cfg = match opts.admin_socket {
        Some(sock_path) => match Listener::open_unix(sock_path) {
            Ok(adm_listener) => Some(AdminConfig {
                listener: adm_listener,
                executable: opts.executable.clone(),
                library: opts.library.map(Path::to_path_buf),
                children,
            }),
            Err(ListenerStatus::Config) => {
                let _ = writeln!(
                    errors,
                    "error E1409: mo serve could not open admin socket at {}: the socket path is empty or longer than UNIX_PATH_MAX",
                    sock_path.display()
                );
                return 1;
            }
            Err(st) => {
                let _ = writeln!(
                    errors,
                    "error E1409: mo serve could not open admin socket at {}: {}",
                    sock_path.display(),
                    st.text().unwrap_or("open failed")
                );
                return 1;
            }
        },
        None => None,
    };

    let now = listener.now();

    // Refuse implicit legacy adoption if image differs without `--adopt` (`upgrade-commands.c:436`).
    if recorded.is_none()
        && let Some((_, head_img, _, _)) = &head_info
        && *head_img != cand_img.program.image
        && !opts.adopt
    {
        report.refuse(
            ErrorCode::UpgradeSourceMismatch as u16,
            "adoption",
            "legacy image differs; explicit --adopt requires verified replay",
        );
        let _ = write!(errors, "{}", report.to_text());
        return 1;
    }

    let expected_recovery_image = head_info.as_ref().map(|(_, img, _, _)| *img);

    // Handle offline startup upgrade (`--upgrade`), adoption, or unchanged restart!
    let service_loop = if changed {
        let Some(prev_prog) = recorded else {
            report.refuse(
                ErrorCode::UpgradeInvalidValue as u16,
                "store",
                "missing recorded program for upgrade",
            );
            let _ = write!(errors, "{}", report.to_text());
            return 1;
        };
        if fallback_state {
            // Empty-tail fallback: skip old ServiceLoop recovery, migrate directly from snapshot!
            let Some((head_snap, _, head_seq, _)) = head_info else {
                report.refuse(
                    ErrorCode::UpgradeInvalidValue as u16,
                    "store",
                    "missing snapshot for fallback upgrade",
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            };
            let Some(migrated) = run_migration_state(
                &cand_img.image,
                cand_img.service,
                opts.migration,
                None,
                &head_snap.bytes,
                false,
                schema_synthesized,
                &mut report,
            ) else {
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            };
            let prev_snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: head_seq,
                image: prev_prog.image,
                bytes: head_snap.bytes,
            };
            let next_snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: head_seq,
                image: cand_img.program.image,
                bytes: migrated.clone(),
            };
            cand_img.program.ordinal = prev_prog.ordinal + 1;
            report.activated = true;
            report.ordinal = cand_img.program.ordinal;
            report.head = head_seq;
            report.message = format!("upgraded to ordinal {}", cand_img.program.ordinal);
            let act = StoreActivation {
                previous: prev_prog,
                candidate: cand_img.program.clone(),
                previous_state: prev_snap,
                candidate_state: next_snap,
                migration: opts.migration.unwrap_or("").to_string(),
                report_json: report.to_json(opts.migration),
            };
            let mut store = store;
            if store.activate(&act) != ActivationStatus::Activated {
                report.refuse(
                    ErrorCode::UpgradeInvalidValue as u16,
                    "store",
                    "activation failed; reopen the folder before retrying",
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            }
            let _ = write!(output, "{}", report.to_text());
            let _ = output.flush();
            match ServiceLoop::open_from_state(
                cand_img.image,
                cand_img.service,
                store,
                opts.limits,
                opts.build_digest,
                MO_BUILD_LABEL.to_string(),
                head_seq,
                migrated,
                now.monotonic_us,
            ) {
                Ok(svc) => svc,
                Err(st) => {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "restart",
                        service_status_text(st),
                    );
                    let _ = write!(errors, "{}", report.to_text());
                    return 1;
                }
            }
        } else {
            let Some(old_img) = old_compiled else {
                report.refuse(
                    ErrorCode::UpgradeRecordedCompile as u16,
                    "compile",
                    "recorded source failed to compile",
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            };
            // Recover old service to get authoritative state at head.
            let (mut old_svc, _, _) = match ServiceLoop::open_with_expected_image(
                old_img.image,
                old_img.service,
                store,
                opts.limits,
                opts.build_digest,
                MO_BUILD_LABEL.to_string(),
                now.monotonic_us,
                false,
                expected_recovery_image,
            ) {
                Ok(v) => v,
                Err(st) => {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "replay",
                        service_status_text(st),
                    );
                    let _ = write!(errors, "{}", report.to_text());
                    return 1;
                }
            };
            let old_state_type_name = old_svc
                .file()
                .types
                .get(old_svc.service_def().state_type)
                .name
                .as_deref();
            let Some(migrated) = run_migration_state(
                &cand_img.image,
                cand_img.service,
                opts.migration,
                old_state_type_name,
                old_svc.encoded_state(),
                false,
                false,
                &mut report,
            ) else {
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            };
            let head = old_svc.sequence();
            let prev_snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: head,
                image: prev_prog.image,
                bytes: old_svc.encoded_state().to_vec(),
            };
            let next_snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: head,
                image: cand_img.program.image,
                bytes: migrated.clone(),
            };
            cand_img.program.ordinal = prev_prog.ordinal + 1;
            report.activated = true;
            report.ordinal = cand_img.program.ordinal;
            report.head = head;
            report.message = format!("upgraded to ordinal {}", cand_img.program.ordinal);
            let act = StoreActivation {
                previous: prev_prog,
                candidate: cand_img.program.clone(),
                previous_state: prev_snap,
                candidate_state: next_snap,
                migration: opts.migration.unwrap_or("").to_string(),
                report_json: report.to_json(opts.migration),
            };
            let prepared = match old_svc.prepare_swap(
                cand_img.image,
                cand_img.service,
                migrated,
                now.monotonic_us,
            ) {
                Ok(p) => p,
                Err(st) => {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "swap",
                        service_status_text(st),
                    );
                    let _ = write!(errors, "{}", report.to_text());
                    return 1;
                }
            };
            if old_svc.store_mut().activate(&act) != ActivationStatus::Activated {
                report.refuse(
                    ErrorCode::UpgradeInvalidValue as u16,
                    "store",
                    "activation failed; reopen the folder before retrying",
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            }
            old_svc.finish_swap(prepared);
            let _ = write!(output, "{}", report.to_text());
            let _ = output.flush();
            old_svc
        }
    } else {
        // Fresh start, verified adoption, or unchanged restart.
        let need_record = recorded.is_none() && head_info.is_none();
        let (mut svc, _was_empty, replayed) = match ServiceLoop::open_with_expected_image(
            cand_img.image,
            cand_img.service,
            store,
            opts.limits,
            opts.build_digest,
            MO_BUILD_LABEL.to_string(),
            now.monotonic_us,
            need_record,
            expected_recovery_image,
        ) {
            Ok(v) => v,
            Err(st) => {
                if recorded.is_none() && head_info.is_none() {
                    emit_diag(
                        errors,
                        ErrorCode::ToolServiceStart,
                        &format!(
                            "mo serve could not start the service: {}",
                            service_status_text(st)
                        ),
                    );
                } else {
                    report.refuse(
                        ErrorCode::UpgradeInvalidValue as u16,
                        "replay",
                        service_status_text(st),
                    );
                    let _ = write!(errors, "{}", report.to_text());
                }
                return 1;
            }
        };

        let adopted = head_info.as_ref().is_some_and(|(_, head_img, _, _)| {
            recorded.is_none()
                || *head_img != cand_img.program.image
                || recorded
                    .as_ref()
                    .is_some_and(|r| r.build != cand_img.program.build)
        });

        if adopted {
            let Some((head_snap, _, _, _)) = head_info else {
                return 1;
            };
            cand_img.program.ordinal = recorded.as_ref().map_or(1, |r| r.ordinal + 1);
            let suffix = if replayed == 0 {
                "; zero records were verified because the tail is empty"
            } else {
                ""
            };
            report.class = "adoption".to_string();
            report.message =
                format!("program adopted after verifying {replayed} log records{suffix}");
            report.activated = true;
            report.ordinal = cand_img.program.ordinal;
            report.head = svc.sequence();

            let previous_program = match recorded.clone() {
                Some(r) => r,
                None => {
                    let Some(old_schema) = export_schema_digest(&head_snap.bytes) else {
                        report.refuse(
                            ErrorCode::UpgradeInvalidValue as u16,
                            "store",
                            "legacy snapshot schema is invalid",
                        );
                        let _ = write!(errors, "{}", report.to_text());
                        return 1;
                    };
                    StoredProgram {
                        source: [0u8; 32],
                        build: [0u8; 32],
                        schema: old_schema,
                        context_digest: [0u8; 32],
                        image: head_snap.image,
                        ordinal: 0,
                        bundle: Vec::new(),
                        context: Vec::new(),
                    }
                }
            };
            let current_snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: svc.sequence(),
                image: cand_img.program.image,
                bytes: svc.encoded_state().to_vec(),
            };
            let act = StoreActivation {
                previous: previous_program,
                candidate: cand_img.program.clone(),
                previous_state: head_snap,
                candidate_state: current_snap,
                migration: "adopted".to_string(),
                report_json: report.to_json(None),
            };
            if svc.store_mut().adopt(&act, recorded.is_none()) != ActivationStatus::Activated {
                report.refuse(
                    ErrorCode::UpgradeInvalidValue as u16,
                    "store",
                    "verified adoption failed; reopen the folder before retrying",
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            }
            let _ = write!(output, "{}", report.to_text());
            let _ = output.flush();
        } else if need_record {
            let snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: svc.sequence(),
                image: svc.image_key(),
                bytes: svc.encoded_state().to_vec(),
            };
            cand_img.program.ordinal = 0;
            if let Err(st) = svc.store_mut().record_program(&cand_img.program, &snap) {
                report.refuse(
                    ErrorCode::UpgradeInvalidValue as u16,
                    "store",
                    format!(
                        "program recording failed: {}",
                        st.text().unwrap_or("reopen the folder before retrying")
                    ),
                );
                let _ = write!(errors, "{}", report.to_text());
                return 1;
            }
        } else if let Some(ref rec) = recorded
            && opts.upgrade
        {
            report.already_active = true;
            report.class = "compatible".to_string();
            report.message = "candidate source is already active".to_string();
            report.ordinal = rec.ordinal;
            report.head = svc.sequence();
            let _ = write!(output, "{}", report.to_text());
            let _ = output.flush();
        }
        svc
    };

    let bound_port = listener.port();
    let numeric = format!("127.0.0.1:{bound_port}");
    let named = format!("localhost:{bound_port}");
    let mut authorities = vec![numeric.clone(), named];
    if let Some(ah) = opts.allow_host {
        authorities.push(ah.to_string());
    }

    let has_admin = admin_cfg.is_some();
    let event_sink = Box::new(|ev: &ServiceEvent| match ev {
        ServiceEvent::FailureCaptured {
            id,
            handler,
            reason,
            defect_text,
        } => {
            eprint!("mo serve: failure: {handler}: {reason}\nbug: {defect_text}");
            if !defect_text.ends_with('\n') {
                eprintln!();
            }
            if *id > 0 {
                eprintln!("  record: {id}");
            }
        }
        ServiceEvent::Restarted { restarts, reason } => {
            eprintln!("mo serve: restart: {restarts} of 3: {reason}");
        }
        ServiceEvent::Stopped { reason } => {
            eprintln!("mo serve: stop: {reason}");
        }
    });

    let mut server =
        match ProtocolServer::new(service_loop, listener, authorities, admin_cfg, event_sink) {
            Ok(s) => s,
            Err(msg) => {
                let _ = writeln!(
                    errors,
                    "error E1409: mo serve could not start the service: {msg}"
                );
                return 1;
            }
        };

    if has_admin && children != UpgradeChildren::Confined {
        let msg = if children == UpgradeChildren::Refused {
            "refused, because this host cannot confine the compile children; --unconfined accepts upgrades unconfined"
        } else {
            "the compile children are not confined (--unconfined)"
        };
        let _ = writeln!(errors, "mo serve: upgrades: {msg}");
        let _ = errors.flush();
    }

    if let Some(ready_path) = opts.ready_file
        && let Err(err) = crate::write_ready_file(
            ready_path,
            &format!("http://{numeric}"),
            bound_port,
            opts.data_dir,
        )
    {
        let _ = writeln!(
            errors,
            "error E1409: mo serve could not write ready file: {err}"
        );
        return 1;
    }
    let _ready_guard = crate::ReadyFileGuard(opts.ready_file);

    if writeln!(output, "mo serve: listening on http://{numeric}/mcp").is_err()
        || output.flush().is_err()
    {
        let _ = writeln!(
            errors,
            "error E1409: mo serve could not write its listening line"
        );
        return 1;
    }

    // Main poll loop.
    loop {
        if is_signalled() || server.service_stopped() || server.store_unsafe() {
            server.stop_accepting();
            if !server.has_connections() {
                break;
            }
        }
        let until_us = server
            .now_monotonic_us()
            .saturating_add(MO_CLI_SERVE_TICK_US);
        if let Err(msg) = server.poll_once(until_us) {
            let _ = writeln!(errors, "mo serve: stop: {msg}");
            return 70;
        }
    }

    if server.store_unsafe() {
        let _ = writeln!(
            errors,
            "mo serve: stop: {}",
            StoreStatus::Io.text().unwrap_or(
                "service store failed; reopen it to see whether the last change was saved"
            )
        );
        let _ = errors.flush();
        return 70;
    }
    if server.service_stopped() {
        return MO_EXIT_SERVICE_STOPPED;
    }
    let _ = writeln!(errors, "mo serve: stop: a signal asked the service to stop");
    let _ = errors.flush();
    0
}

fn build_schema_only_metadata(program: &StoredProgram) -> Option<Vec<u8>> {
    let ctx = read_compile_context(&program.context)?;
    let raw = ctx.metadata.as_deref()?;
    let mut meta = read_compile_metadata(raw)?;
    meta.types.clear();
    write_compile_metadata(&meta)
}

#[expect(
    clippy::too_many_arguments,
    reason = "startup door-check helper forwards compile job inputs"
)]
fn run_startup_check_child(
    executable: &Path,
    library: Option<&Path>,
    bundle: &[u8],
    old_bundle: Option<&[u8]>,
    migration: Option<&str>,
    context: Option<&[u8]>,
    confine: bool,
    build_digest: [u8; 32],
    ordinal: u64,
) -> Result<(), (u16, &'static str, String)> {
    let mut job = CompileJob::start(
        executable,
        library,
        bundle.to_vec(),
        old_bundle,
        migration,
        context,
        true,
        confine,
        build_digest,
        ordinal,
    )
    .map_err(|msg| (ErrorCode::UpgradeCompileDiagnostic as u16, "compile", msg))?;
    loop {
        match job.poll() {
            CompilePoll::Running => std::thread::sleep(std::time::Duration::from_millis(10)),
            CompilePoll::Finished(res) => match *res {
                Ok(_) => return Ok(()),
                Err(e) => return Err(e),
            },
        }
    }
}

fn io_err_short(e: &std::io::Error) -> &'static str {
    match e.kind() {
        std::io::ErrorKind::NotFound => "No such file or directory",
        std::io::ErrorKind::PermissionDenied => "Permission denied",
        std::io::ErrorKind::NotADirectory => "Not a directory",
        _ => "I/O error",
    }
}
