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

use crate::binary::hex_encode;
use crate::listener::signals::{install_signal_handlers, is_signalled, reset_signal};
use crate::listener::{Listener, ListenerConfig, ListenerStatus};
use crate::protocol::MO_BUILD_LABEL;
use crate::protocol::server::{AdminConfig, ProtocolServer};
use crate::service_loop::{ServiceEvent, ServiceLoop, ServiceStatus};
use crate::store::programs::{ActivationStatus, StoreActivation};
use crate::store::{ServiceStore, StoreRecord, StoreRecordKind, StoreStatus};
use crate::upgrade::bundle::bundle_from_path;
use crate::upgrade::compile::compile_image_sync;
use crate::upgrade::{
    UpgradeChildren, UpgradeReport, compare_programs, program_metadata, run_migration_state,
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
    let store = match ServiceStore::open_wait(&db_path) {
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
        Err(st) => {
            let _ = writeln!(
                errors,
                "error E1409: mo serve could not read recorded program: {}",
                st.text().unwrap_or("corrupt store")
            );
            return 1;
        }
    };

    // Determine which bundle to compile and whether this is a source change.
    let (bundle_bytes, cand_digest) = match candidate_bundle {
        Some(pair) => pair,
        None => match &recorded {
            Some(prog) => (prog.bundle.clone(), prog.source),
            None => {
                let _ = writeln!(
                    errors,
                    "error E1430: this folder has no recorded program; provide a source file"
                );
                return 1;
            }
        },
    };

    let changed = recorded.as_ref().is_some_and(|r| r.source != cand_digest);
    if changed && !opts.upgrade {
        if let Some(ref rec) = recorded {
            let _ = writeln!(
                errors,
                "error E1430: source differs from recorded digest {}; use --upgrade",
                hex_encode(&rec.source)
            );
        }
        return 1;
    }

    let children = UpgradeChildren::detect(opts.unconfined);
    let mut report = UpgradeReport::default();
    if changed && !report.check_children(children) {
        let _ = write!(errors, "{}", report.to_text());
        return 1;
    }

    // Compile candidate image.
    let old_bundle_for_mig = if changed && opts.migration.is_some() {
        recorded.as_ref().map(|r| r.bundle.as_slice())
    } else {
        None
    };
    let existing_ctx = recorded.as_ref().map(|r| r.context.as_slice());
    let next_ordinal = if changed {
        recorded.as_ref().map_or(1, |r| r.ordinal + 1)
    } else {
        recorded.as_ref().map_or(0, |r| r.ordinal)
    };

    let Some(cand_img) = compile_image_sync(
        &bundle_bytes,
        old_bundle_for_mig,
        if changed { opts.migration } else { None },
        existing_ctx,
        opts.library,
        opts.build_digest,
        next_ordinal,
        &mut report,
    ) else {
        if !changed && report.code == ErrorCode::UpgradeRecordedCompile as u16 {
            let _ = write!(errors, "{}", report.detail);
            let _ = errors.flush();
            return 70;
        }
        if !changed && report.code == ErrorCode::UpgradeTimerInterval as u16 {
            let _ = write!(errors, "{}", report.detail);
            emit_diag(
                errors,
                ErrorCode::ToolServiceStart,
                "mo serve could not start the service: service handler failed",
            );
            return 1;
        }
        let _ = write!(errors, "{}", report.to_text());
        return 1;
    };

    // Validate bearer token env var and open primary TCP listener BEFORE mutating store!
    let token_env = cand_img
        .image
        .file()
        .string(cand_img.image.file().services[cand_img.service as usize].token_env)
        .to_string();
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

    // Handle offline startup upgrade (`--upgrade`) if source changed!
    let service_loop = if changed {
        let Some(prev_prog) = recorded else {
            let _ = writeln!(errors, "error E1409: missing recorded program for upgrade");
            return 1;
        };
        let mut old_report = UpgradeReport::default();
        let Some(old_img) = compile_image_sync(
            &prev_prog.bundle,
            None,
            None,
            Some(&prev_prog.context),
            opts.library,
            opts.build_digest,
            prev_prog.ordinal,
            &mut old_report,
        ) else {
            let _ = write!(errors, "{}", old_report.to_text());
            return 1;
        };
        let old_meta = program_metadata(&prev_prog);
        let next_meta = program_metadata(&cand_img.program);
        if !compare_programs(
            &prev_prog,
            old_meta.as_ref(),
            &cand_img.program,
            next_meta.as_ref(),
            opts.migration,
            &mut report,
        ) {
            let _ = write!(errors, "{}", report.to_text());
            return 1;
        }
        // Recover old service to get authoritative state at head.
        let (mut old_svc, _) = match ServiceLoop::open(
            old_img.image,
            old_img.service,
            store,
            opts.limits,
            opts.build_digest,
            MO_BUILD_LABEL.to_string(),
            now.monotonic_us,
            false,
        ) {
            Ok(v) => v,
            Err(st) => {
                let _ = writeln!(
                    errors,
                    "error E1409: mo serve could not recover previous state: {st:?}"
                );
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
            &mut report,
        ) else {
            let _ = write!(errors, "{}", report.to_text());
            return 1;
        };
        let head = old_svc.sequence();
        let prev_snap = StoreRecord {
            kind: StoreRecordKind::State,
            sequence: head,
            image: old_svc.image_key(),
            bytes: old_svc.encoded_state().to_vec(),
        };
        let next_snap = StoreRecord {
            kind: StoreRecordKind::State,
            sequence: head,
            image: cand_img.program.image,
            bytes: migrated.clone(),
        };
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
            Err(_) => {
                let _ = writeln!(errors, "error E1409: failed to prepare upgraded state");
                return 1;
            }
        };
        if old_svc.store_mut().activate(&act) != ActivationStatus::Activated {
            let _ = writeln!(errors, "error E1409: failed to activate upgraded program");
            return 1;
        }
        old_svc.finish_swap(prepared);
        old_svc
    } else {
        // Fresh start or restart with identical source.
        let need_record = recorded.is_none();
        let (mut svc, was_empty) = match ServiceLoop::open(
            cand_img.image,
            cand_img.service,
            store,
            opts.limits,
            opts.build_digest,
            MO_BUILD_LABEL.to_string(),
            now.monotonic_us,
            need_record, // Defer initial snapshot if fresh so program + snapshot commit atomically!
        ) {
            Ok(v) => v,
            Err(ServiceStatus::Corrupt) => {
                emit_diag(
                    errors,
                    ErrorCode::ToolServiceStart,
                    "mo serve could not start the service: saved service data is damaged or was written by a different program; recovery refused",
                );
                return 1;
            }
            Err(ServiceStatus::Failed) => {
                emit_diag(
                    errors,
                    ErrorCode::ToolServiceStart,
                    "mo serve could not start the service: service handler failed",
                );
                return 1;
            }
            Err(st) => {
                emit_diag(
                    errors,
                    ErrorCode::ToolServiceStart,
                    &format!("mo serve could not start the service: {st:?}"),
                );
                return 1;
            }
        };

        if need_record || (was_empty && recorded.is_none()) {
            let snap = StoreRecord {
                kind: StoreRecordKind::State,
                sequence: svc.sequence(),
                image: svc.image_key(),
                bytes: svc.encoded_state().to_vec(),
            };
            if let Err(st) = svc.store_mut().record_program(&cand_img.program, &snap) {
                let _ = writeln!(
                    errors,
                    "error E1409: mo serve could not record initial program: {}",
                    st.text().unwrap_or("store error")
                );
                return 1;
            }
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

fn io_err_short(e: &std::io::Error) -> &'static str {
    match e.kind() {
        std::io::ErrorKind::NotFound => "No such file or directory",
        std::io::ErrorKind::PermissionDenied => "Permission denied",
        std::io::ErrorKind::NotADirectory => "Not a directory",
        _ => "I/O error",
    }
}
