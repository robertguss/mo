//! Offline failure replay (`mo replay`) and pure state/failure queries (`mo query`)
//! (`src/tools/offline.c`, `src/tools/upgrade-bundle.c:mo_upgrade_replay_set`, and
//! `src/vm/offline.c` parity).

use std::fmt::Write as _;
use std::io::Write;
use std::path::Path;

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, RenderFormat, SourceMap, Span, TerminalEncoding,
    render_diagnostic_default,
};
use mo_bytecode::{BytecodeFile, NO_INDEX, ServiceDef, TypeKind, load as load_bytecode};
use mo_check::{
    CheckedFuncId, CheckedProgram, LibraryMode, LoadOptions, QueryEntryStatus, QueryInput,
    check_program, check_query_entry, default_embedded_library, default_library_root,
    load_bundle_with_library, load_program_bytes,
};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_EXPORT_BYTES, MO_LIMIT_FAILURE_KIND_BYTES,
    MO_LIMIT_OFFLINE_VALUE_BYTES, MO_LIMIT_PATH_SIZE, MO_LIMIT_SAVED_BYTES, MO_LIMIT_SERVICE_WORK,
    MO_TIME_MAX_US, MO_TIME_MIN_US,
};
use mo_heap::print::value_print_bounded;
use mo_heap::saved::{decode_value, encode_value, encode_value_unshared, write_schema};
use mo_heap::{Heap, Value};
use mo_host::HostContext;
use mo_lower::{lower_query, lower_services};
use mo_service::binary::bundle::read_bundle;
use mo_service::binary::compile_context::read_compile_context;
use mo_service::binary::export::{export_schema_digest, read_export};
use mo_service::binary::failure::{FailureContextStatus, FailureRecord, read_failure_record};
use mo_service::binary::{hex_encode, push_json_string, sanitize_terminal_text, sha256_bytes};
use mo_service::history::{
    FailureSignature, build_refusal, folder_error_message, render_offline_value,
};
use mo_service::protocol::MO_BUILD_LABEL;
use mo_service::service_loop::failure::format_failure_context;
use mo_service::service_loop::{classify_defect, extract_choice_field, short_handler_name};
use mo_service::store::history::{read_history_pair, read_retained_program_from_file};
use mo_service::upgrade::bundle::bundle_from_path_opts;
use mo_vm::{ControlledOutcome, Defect, Image, Limits, Outcome, Process, render_defect_raw};

use crate::offline_output::{
    OfflineRefusal, OfflineRun, OutcomeStatus, ReplayClass, ReplyKind, RunOutcome, Verdict,
    classify_replay, compute_verdict, describe_handler, push_context_text, push_run_json,
    push_run_text, signature_from_record,
};
use crate::rehearse::read_bounded_regular_file;

const UNICODE_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: mo_prim::unicode16::next_grapheme_bytes,
    display_width: mo_prim::unicode16::display_width_bytes,
};

const MEMORY_NOTE: &str = "live heap occupants, such as arguments queued in the mailbox, are not reconstructed offline, so a memory failure that depended on them does not recur";

/// Options for `mo replay`.
pub struct ReplayOptions<'a> {
    /// Optional source file path (`None` looks up retained program in `--data`).
    pub file: Option<&'a [u8]>,
    /// Required failure id (`--failure`).
    pub failure_id: u64,
    /// Optional data directory (`--data`).
    pub data_dir: Option<&'a Path>,
    /// Optional export artifact path (`--export`).
    pub artifact: Option<&'a Path>,
    /// Optional revised source file path (`--fix`).
    pub revised: Option<&'a [u8]>,
    /// Whether `--recover` was passed.
    pub recover: bool,
    /// Whether `--json` was passed.
    pub json: bool,
    /// Optional library directory (`--lib`).
    pub library: Option<&'a Path>,
    /// CLI version string for NDJSON `version` record.
    pub version: &'a str,
    /// 32-byte build runtime identity digest.
    pub build_digest: [u8; 32],
}

/// Options for `mo query`.
pub struct QueryOptions<'a> {
    /// Entry source file path (`FILE`).
    pub file: &'a [u8],
    /// Query function name (`FUNCTION`, optionally module-qualified).
    pub function: &'a str,
    /// Optional failure id (`--failure`, non-zero for failure query).
    pub failure_id: u64,
    /// Optional data directory (`--data`).
    pub data_dir: Option<&'a Path>,
    /// Optional export artifact path (`--export`).
    pub artifact: Option<&'a Path>,
    /// Optional live admin Unix socket (`--admin-socket`).
    pub admin_socket: Option<&'a Path>,
    /// Whether `--recover` was passed.
    pub recover: bool,
    /// Whether `--json` was passed.
    pub json: bool,
    /// Optional library directory (`--lib`).
    pub library: Option<&'a Path>,
    /// VM memory/stack/depth limits (`--max-heap`, `--max-stack`, `--max-depth`).
    pub limits: Limits,
    /// Maximum VM instruction budget (`--max-work`).
    pub max_work: u64,
    /// CLI version string for NDJSON `version` record.
    pub version: &'a str,
    /// 32-byte build runtime identity digest.
    pub build_digest: [u8; 32],
}

/// Decoded offline input: either a failure record + pre-call state or a state export.
struct OfflineInput {
    state: Vec<u8>,
    schema: [u8; 32],
    build: [u8; 32],
    label: String,
    program: u64,
    service: u32,
    image_key: bool,
    record: Option<FailureRecord>,
}

/// Executes a service handler offline exactly as `ServiceLoop` executes it (`src/vm/offline.c`).
#[must_use]
pub fn offline_call(
    image: &Image,
    service_idx: u32,
    handler: u32,
    state_bytes: &[u8],
    record: &FailureRecord,
) -> OfflineRun {
    let file = image.file();
    let svc = &file.services[service_idx as usize];
    let timer = record.timer;
    let limits = Limits::default();
    let mut proc = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));

    // Validate arrival timestamp and decode pre-call state and arguments.
    if record.arrival < MO_TIME_MIN_US || record.arrival > MO_TIME_MAX_US {
        return refused_run("the recorded inputs could not be decoded");
    }
    let Ok(before_state) = decode_value(proc.heap(), &file.types, svc.state_type, state_bytes)
    else {
        return refused_run("the recorded inputs could not be decoded");
    };

    let (fn_idx, call_args, read_only, reply_ty) = if timer {
        let Some(t) = svc.timers.get(handler as usize) else {
            return refused_run("the recorded inputs could not be decoded");
        };
        if !record.arguments.is_empty() {
            return refused_run("the recorded inputs could not be decoded");
        }
        (
            t.function,
            vec![before_state, Value::Time(record.arrival)],
            false,
            svc.state_type,
        )
    } else {
        let Some(tool) = svc.tools.get(handler as usize) else {
            return refused_run("the recorded inputs could not be decoded");
        };
        if record.arguments.len() != tool.argument_types.len() {
            return refused_run("the recorded inputs could not be decoded");
        }
        let mut args = Vec::with_capacity(2 + tool.argument_types.len());
        args.push(before_state);
        args.push(Value::Time(record.arrival));
        for (raw, &aty) in record.arguments.iter().zip(&tool.argument_types) {
            let Ok(v) = decode_value(proc.heap(), &file.types, aty, raw) else {
                return refused_run("the recorded inputs could not be decoded");
            };
            args.push(v);
        }
        (tool.function, args, tool.read_only, tool.result_type)
    };

    let (outcome, _work) = proc.call_with_budget(fn_idx, call_args, MO_LIMIT_SERVICE_WORK);
    match outcome {
        Outcome::Returned(result_val) => {
            let reply = reply_kind(file, timer, reply_ty, &result_val);
            if timer {
                let mut encoded = Vec::new();
                if encode_value_unshared(&file.types, svc.state_type, &result_val, &mut encoded)
                    .is_err()
                {
                    return state_size_run(file, "updated state could not be saved", reply);
                }
                let state_changed = encoded.as_slice() != state_bytes;
                return OfflineRun {
                    outcome: RunOutcome {
                        status: OutcomeStatus::Succeeded,
                        signature: None,
                        reply,
                        state_changed,
                    },
                    context_status: FailureContextStatus::Complete,
                    context: String::new(),
                    refusal: None,
                };
            }

            if read_only || reply == ReplyKind::Error {
                return OfflineRun {
                    outcome: RunOutcome {
                        status: OutcomeStatus::Succeeded,
                        signature: None,
                        reply,
                        state_changed: false,
                    },
                    context_status: FailureContextStatus::Complete,
                    context: String::new(),
                    refusal: None,
                };
            }

            // Updating tool returned Ok((next_state, reply)) or (next_state, reply).
            let rdef = file.types.get(reply_ty);
            let is_result = rdef.kind == TypeKind::Choice
                && rdef
                    .name
                    .as_deref()
                    .is_some_and(|n| n.starts_with("Result("));
            let pair_val = if is_result {
                extract_choice_field(&file.types, reply_ty, &result_val, 0)
            } else {
                Some(result_val)
            };
            let next_state = pair_val.and_then(|pv| match pv.as_heap_ref().obj() {
                mo_heap::HeapObj::Tuple { fields, .. } if fields.len() == 2 => {
                    Some(fields[0].clone())
                }
                _ => None,
            });
            let Some(ns) = next_state else {
                let defect = Defect {
                    reason: "mutating tool did not return a (State, Reply) pair".to_string(),
                    limit_name: None,
                    limit: 0,
                    detail: None,
                    frames: Vec::new(),
                    variables: Vec::new(),
                    variables_unavailable: false,
                };
                return failure_run(file, 0, &defect);
            };
            let mut encoded = Vec::new();
            if encode_value_unshared(&file.types, svc.state_type, &ns, &mut encoded).is_err() {
                return state_size_run(file, "updated state could not be saved", reply);
            }
            let state_changed = encoded.as_slice() != state_bytes;
            OfflineRun {
                outcome: RunOutcome {
                    status: OutcomeStatus::Succeeded,
                    signature: None,
                    reply,
                    state_changed,
                },
                context_status: FailureContextStatus::Complete,
                context: String::new(),
                refusal: None,
            }
        }
        Outcome::Failed(defect) | Outcome::Stopped(defect) => {
            let class = classify_defect(&defect);
            failure_run(file, class, &defect)
        }
    }
}

fn refused_run(msg: &str) -> OfflineRun {
    OfflineRun {
        outcome: RunOutcome {
            status: OutcomeStatus::Refused,
            signature: None,
            reply: ReplyKind::Ok,
            state_changed: false,
        },
        context_status: FailureContextStatus::Complete,
        context: String::new(),
        refusal: Some(msg.to_string()),
    }
}

fn state_size_run(file: &BytecodeFile, reason: &str, reply: ReplyKind) -> OfflineRun {
    let defect = Defect {
        reason: reason.to_string(),
        limit_name: Some("MO_LIMIT_SAVED_BYTES"),
        limit: MO_LIMIT_SAVED_BYTES as usize,
        detail: None,
        frames: Vec::new(),
        variables: Vec::new(),
        variables_unavailable: false,
    };
    let mut run = failure_run(file, 3, &defect);
    run.outcome.reply = reply;
    run.outcome.state_changed = true;
    run
}

fn failure_run(file: &BytecodeFile, class: u8, defect: &Defect) -> OfflineRun {
    let max_kind = MO_LIMIT_FAILURE_KIND_BYTES as usize;
    let (reason, limit, limit_value) = if class == 1 {
        (
            "work limit".to_string(),
            "MO_LIMIT_SERVICE_WORK".to_string(),
            MO_LIMIT_SERVICE_WORK,
        )
    } else {
        (
            truncate_utf8(&defect.reason, max_kind),
            defect
                .limit_name
                .map(|s| truncate_utf8(s, max_kind))
                .unwrap_or_default(),
            defect.limit as u64,
        )
    };
    let (top_fn, top_file, top_start, top_end) = match defect.frames.first() {
        Some(f) => match f.span {
            Some((file_idx, start, end)) => (f.function, file_idx, start, end),
            None => (f.function, NO_INDEX, 0, 0),
        },
        None => (NO_INDEX, NO_INDEX, 0, 0),
    };
    let sig = FailureSignature {
        class,
        reason,
        limit,
        limit_value,
        function: top_fn,
        file: top_file,
        start: top_start,
        end: top_end,
    };
    let status = match class {
        0 => OutcomeStatus::Defect,
        1 => OutcomeStatus::Work,
        2 => OutcomeStatus::Memory,
        _ => OutcomeStatus::StateSize,
    };
    let (context_status, context) = format_failure_context(file, defect);
    OfflineRun {
        outcome: RunOutcome {
            status,
            signature: Some(sig),
            reply: ReplyKind::Ok,
            state_changed: status == OutcomeStatus::StateSize,
        },
        context_status,
        context,
        refusal: None,
    }
}

fn reply_kind(file: &BytecodeFile, timer: bool, ty: u32, val: &Value) -> ReplyKind {
    let t = file.types.get(ty);
    let is_result =
        t.kind == TypeKind::Choice && t.name.as_deref().is_some_and(|n| n.starts_with("Result("));
    if !timer && is_result && val.as_tag() == 1 {
        ReplyKind::Error
    } else {
        ReplyKind::Ok
    }
}

fn truncate_utf8(s: &str, max_bytes: usize) -> String {
    if s.len() <= max_bytes {
        return s.to_string();
    }
    let mut cut = max_bytes;
    while cut > 0 && !s.is_char_boundary(cut) {
        cut -= 1;
    }
    s[..cut].to_string()
}

fn lib_mode_for(library: Option<&Path>) -> LibraryMode {
    match library {
        Some(dir) => LibraryMode::Directory(dir.to_path_buf()),
        None => LibraryMode::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        },
    }
}

fn write_version_line(out: &mut dyn Write, version: &str, cmd: &str) -> bool {
    let mut s = String::from("{\"record\":\"version\",\"format\":\"1.0\",\"mo\":");
    push_json_string(&mut s, version);
    let _ = writeln!(s, ",\"command\":\"{cmd}\"}}");
    out.write_all(s.as_bytes()).is_ok() && out.flush().is_ok()
}

fn write_totals_line(out: &mut dyn Write, errors: usize, dropped: usize, status: i32) -> bool {
    let s = format!(
        "{{\"record\":\"totals\",\"errors\":{errors},\"dropped\":{dropped},\"status\":{status}}}\n"
    );
    out.write_all(s.as_bytes()).is_ok() && out.flush().is_ok()
}

#[expect(
    clippy::too_many_arguments,
    reason = "diagnostic renderer takes sources, diag bag, format flags, version metadata, and output streams"
)]
fn emit_diags_and_finish(
    sources: &SourceMap,
    diags: &mut DiagBag,
    json: bool,
    version_written: bool,
    version: &str,
    cmd: &str,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    diags.sort();
    if json {
        if !version_written && !write_version_line(output, version, cmd) {
            let _ = writeln!(errors, "mo: could not write version record");
            return 1;
        }
        for d in diags.items() {
            if let Ok(buf) = render_diagnostic_default(
                sources,
                d,
                RenderFormat::Json,
                TerminalEncoding::Utf8,
                UNICODE_METRICS,
            ) {
                let _ = output.write_all(&buf);
            }
        }
        let _ = write_totals_line(output, diags.len(), diags.dropped() as usize, 1);
    } else {
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
    1
}

/// Reads an export artifact file (`read_artifact` in `src/tools/offline.c`).
fn read_artifact_input(path: &Path, wanted_failure: u64) -> Result<OfflineInput, OfflineRefusal> {
    let Some(bytes) = read_bounded_regular_file(path, MO_LIMIT_EXPORT_BYTES) else {
        let reason = match std::fs::metadata(path) {
            Ok(m) if !m.is_file() => Some("not a regular file".to_string()),
            Ok(m) if m.len() > MO_LIMIT_EXPORT_BYTES => {
                return Err(OfflineRefusal::new(
                    "format",
                    "the artifact is larger than any Mo export",
                    None,
                    None,
                ));
            }
            Err(e) => Some(e.to_string()),
            _ => None,
        };
        return Err(OfflineRefusal::new(
            "format",
            "the artifact cannot be read",
            None,
            reason,
        ));
    };
    let Some(exp) = read_export(&bytes) else {
        return Err(OfflineRefusal::new(
            "format",
            "the artifact is not a complete Mo export: its format, a frame or its digest failed its check",
            None,
            None,
        ));
    };
    let is_failure = exp.identity.kind == 2;
    if wanted_failure != 0 && !is_failure {
        return Err(OfflineRefusal::new(
            "kind",
            "the artifact holds an exported state, not a failure; fetch one with mo fetch --admin-socket PATH --export failure ID --out FILE",
            Some("state".to_string()),
            Some("failure".to_string()),
        ));
    }
    if wanted_failure == 0 && is_failure {
        return Err(OfflineRefusal::new(
            "kind",
            "the artifact holds a failure; give --failure ID to query its inputs",
            Some("failure".to_string()),
            Some("state".to_string()),
        ));
    }
    if is_failure && exp.identity.failure_id != wanted_failure {
        let mut r = OfflineRefusal::new(
            "failure",
            "the artifact holds another failure",
            Some(exp.identity.failure_id.to_string()),
            Some(wanted_failure.to_string()),
        );
        r.found_label = "asked for";
        return Err(r);
    }
    Ok(OfflineInput {
        state: exp.state.to_vec(),
        schema: exp.schema,
        build: exp.identity.build,
        label: exp.identity.label,
        program: exp.identity.program,
        service: exp.identity.service,
        image_key: false,
        record: exp.failure,
    })
}

/// Reads a failure pair from a stopped data folder (`read_folder` in `src/tools/offline.c`).
fn read_folder_input(
    folder: &Path,
    recover: bool,
    wanted_failure: u64,
) -> Result<OfflineInput, OfflineRefusal> {
    let folder_str = folder.to_string_lossy();
    if folder_str.len() as u64 > MO_LIMIT_PATH_SIZE {
        return Err(OfflineRefusal::new(
            "format",
            "the data folder's path is too long",
            None,
            None,
        ));
    }
    let db_path = folder.join("service.sqlite");
    let pair = read_history_pair(&db_path, recover, wanted_failure).map_err(|st| {
        OfflineRefusal::new(
            "folder",
            folder_error_message(&folder_str, st, 0),
            None,
            None,
        )
    })?;
    if !pair.failure_tables {
        return Err(OfflineRefusal::new(
            "folder",
            "this store predates failure capture, so it holds no failure records",
            None,
            None,
        ));
    }
    if !pair.found {
        let ids = if pair.retained.is_empty() {
            "none".to_string()
        } else {
            pair.retained
                .iter()
                .map(ToString::to_string)
                .collect::<Vec<_>>()
                .join(", ")
        };
        let mut r = OfflineRefusal::new(
            "failure",
            "the folder holds no failure record with that id; ids are never reused, so a missing one was replaced or evicted",
            None,
            Some(ids),
        );
        r.found_label = "retained ids";
        return Err(r);
    }
    let (Some(state_rec), Some(msg_rec)) = (pair.state, pair.message) else {
        return Err(OfflineRefusal::new(
            "format",
            "the failure record's message does not decode",
            None,
            None,
        ));
    };
    let Some(rec) = read_failure_record(&msg_rec.bytes).filter(|r| r.id == wanted_failure) else {
        return Err(OfflineRefusal::new(
            "format",
            "the failure record's message does not decode",
            None,
            None,
        ));
    };
    let state_schema = export_schema_digest(&state_rec.bytes);
    if msg_rec.sequence != rec.sequence
        || msg_rec.image != rec.program
        || state_schema != Some(rec.schema)
    {
        return Err(OfflineRefusal::new(
            "format",
            "the failure record's pre-call state does not belong to its message",
            None,
            None,
        ));
    }
    Ok(OfflineInput {
        state: state_rec.bytes,
        schema: rec.schema,
        build: rec.build,
        label: rec.label.clone(),
        program: rec.program,
        service: 0,
        image_key: true,
        record: Some(rec),
    })
}

/// Fetches a live state export from `--admin-socket` (`read_live` in `src/tools/offline.c`).
#[cfg(unix)]
fn read_live_input(socket: &Path, wanted_failure: u64) -> Result<OfflineInput, OfflineRefusal> {
    let bytes = mo_service::fetch::fetch_live_export(
        socket,
        (wanted_failure != 0).then_some(wanted_failure),
    )
    .map_err(|msg| {
        OfflineRefusal::new(
            "live",
            msg,
            None,
            Some(socket.to_string_lossy().into_owned()),
        )
    })?;
    let Some(exp) = read_export(&bytes) else {
        return Err(OfflineRefusal::new(
            "format",
            "the live export failed its format, frame or digest check",
            None,
            None,
        ));
    };
    let is_failure = exp.identity.kind == 2;
    if wanted_failure != 0 && !is_failure {
        return Err(OfflineRefusal::new(
            "kind",
            "the live export returned a state instead of a failure",
            Some("state".to_string()),
            Some("failure".to_string()),
        ));
    }
    if wanted_failure == 0 && is_failure {
        return Err(OfflineRefusal::new(
            "kind",
            "the live export returned a failure instead of a state",
            Some("failure".to_string()),
            Some("state".to_string()),
        ));
    }
    if is_failure && exp.identity.failure_id != wanted_failure {
        let mut r = OfflineRefusal::new(
            "failure",
            "the live export returned another failure",
            Some(exp.identity.failure_id.to_string()),
            Some(wanted_failure.to_string()),
        );
        r.found_label = "asked for";
        return Err(r);
    }
    Ok(OfflineInput {
        state: exp.state.to_vec(),
        schema: exp.schema,
        build: exp.identity.build,
        label: exp.identity.label,
        program: exp.identity.program,
        service: exp.identity.service,
        image_key: false,
        record: exp.failure,
    })
}

fn check_build_compat(
    input: &OfflineInput,
    current_build: &[u8; 32],
) -> Result<(), OfflineRefusal> {
    if input.build == [0u8; 32] || input.label.is_empty() {
        return Err(OfflineRefusal::new(
            "build",
            "the record does not identify the build that recorded it, so no build can replay it; it is evidence only",
            None,
            None,
        ));
    }
    if build_refusal(&input.build, &input.label, current_build).is_some() {
        let rec = format!(
            "{} (sha256 {})",
            sanitize_terminal_text(&input.label),
            hex_encode(&input.build)
        );
        let found = format!("{MO_BUILD_LABEL} (sha256 {})", hex_encode(current_build));
        return Err(OfflineRefusal::new(
            "build",
            "recorded by another build; run the mo that recorded it (sha256sum finds it among the deployed executables)",
            Some(rec),
            Some(found),
        ));
    }
    Ok(())
}

fn check_program_compat(
    input: &OfflineInput,
    file: &BytecodeFile,
    service_idx: u32,
) -> Result<(), OfflineRefusal> {
    let found = file.program_checksum ^ (u64::from(service_idx) << 32);
    let recorded = if input.image_key {
        input.program
    } else {
        input.program ^ (u64::from(input.service) << 32)
    };
    if recorded == found {
        return Ok(());
    }
    Err(OfflineRefusal::new(
        "program",
        "FILE is not the program that recorded the failure; it must be the untouched source, since any byte change in any module, a comment or mo format included, changes the program identity",
        Some(format!("{recorded:016x}")),
        Some(format!("{found:016x}")),
    ))
}

fn check_handler_compat(
    rec: &FailureRecord,
    file: &BytecodeFile,
    svc: &ServiceDef,
) -> Result<(), OfflineRefusal> {
    let timer = rec.timer;
    let count = if timer {
        svc.timers.len()
    } else {
        svc.tools.len()
    };
    let in_range = (rec.handler as usize) < count;
    let (found_name, found_ro, found_args) = if in_range {
        let fidx = if timer {
            svc.timers[rec.handler as usize].function
        } else {
            svc.tools[rec.handler as usize].function
        };
        let bare = short_handler_name(file.function_name(fidx));
        let ro = !timer && svc.tools[rec.handler as usize].read_only;
        let args = if timer {
            0
        } else {
            svc.tools[rec.handler as usize].argument_types.len() as u32
        };
        (bare, ro, args)
    } else {
        ("", false, 0)
    };
    if in_range
        && found_name == rec.name
        && found_ro == rec.read_only
        && found_args == rec.arguments.len() as u32
    {
        return Ok(());
    }
    let rec_desc = describe_handler(
        &rec.name,
        timer,
        rec.read_only,
        rec.handler,
        rec.arguments.len() as u32,
    );
    let found_desc = if in_range {
        describe_handler(found_name, timer, found_ro, rec.handler, found_args)
    } else {
        "no such handler".to_string()
    };
    Err(OfflineRefusal::new(
        "handler",
        "FILE's service has no handler like the recorded one",
        Some(rec_desc),
        Some(found_desc),
    ))
}

fn check_schema_compat(
    file: &BytecodeFile,
    ty: u32,
    recorded: &[u8; 32],
    check: &'static str,
    message: &str,
) -> Result<(), OfflineRefusal> {
    let mut sbuf = Vec::new();
    if write_schema(&file.types, ty, &mut sbuf).is_err() {
        return Err(OfflineRefusal::new(
            check,
            "the type has no saved form",
            None,
            None,
        ));
    }
    let found = sha256_bytes(&sbuf);
    if &found == recorded {
        return Ok(());
    }
    Err(OfflineRefusal::new(
        check,
        message,
        Some(hex_encode(recorded)),
        Some(hex_encode(&found)),
    ))
}

fn check_arguments_compat(
    file: &BytecodeFile,
    rec: &FailureRecord,
    types: &[u32],
    check: &'static str,
) -> Result<(), OfflineRefusal> {
    if rec.arguments.len() < types.len() {
        return Err(OfflineRefusal::new(
            check,
            "the record holds fewer arguments than the handler takes",
            None,
            None,
        ));
    }
    for (i, &ty) in types.iter().enumerate() {
        let Some(recorded) = export_schema_digest(&rec.arguments[i]) else {
            return Err(OfflineRefusal::new(
                check,
                "the record holds fewer arguments than the handler takes",
                None,
                None,
            ));
        };
        let msg = match i {
            0 => "argument 1's type differs from the recorded one",
            1 => "argument 2's type differs from the recorded one",
            2 => "argument 3's type differs from the recorded one",
            _ => "an argument's type differs from the recorded one",
        };
        check_schema_compat(file, ty, &recorded, check, msg)?;
    }
    if rec.arguments.len() > types.len() {
        return Err(OfflineRefusal::new(
            check,
            "the record holds more arguments than the handler takes",
            None,
            None,
        ));
    }
    Ok(())
}

fn check_budgets_compat(rec: &FailureRecord) -> Result<(), OfflineRefusal> {
    let limits = Limits::default();
    let pairs: [(&str, u64, u64); 5] = [
        ("work", rec.work, MO_LIMIT_SERVICE_WORK),
        ("heap", rec.heap, limits.heap_size as u64),
        ("stack", rec.stack, limits.stack_size as u64),
        ("depth", rec.depth, u64::from(limits.call_depth)),
        ("saved bytes", rec.saved, MO_LIMIT_SAVED_BYTES),
    ];
    for (name, recorded, expected) in pairs {
        if recorded != expected {
            return Err(OfflineRefusal::new(
                "budgets",
                "the record's budgets differ from this build's, so it is damaged or crafted; replay never runs with budgets read from a file",
                Some(format!("{name} = {recorded}")),
                Some(format!("{name} = {expected}")),
            ));
        }
    }
    Ok(())
}

fn find_main_service_checked(program: &CheckedProgram) -> Option<usize> {
    program
        .services
        .iter()
        .position(|s| s.module == mo_check::ModuleId(0) && !s.is_actor)
}

fn load_replay_sources(
    opts: &ReplayOptions<'_>,
    sources: &mut SourceMap,
    diags: &mut DiagBag,
) -> Option<mo_check::ModuleSet> {
    let lib_mode = lib_mode_for(opts.library);
    if let Some(file_bytes) = opts.file {
        // Compile via canonical bundle (matching `mo_upgrade_source_set`) so imported modules
        // produce the exact same bytecode checksum as live `mo serve`!
        let mut tmp_sources = SourceMap::new();
        let (bundle_bytes, _) = bundle_from_path_opts(
            file_bytes,
            opts.library,
            false,
            false,
            diags,
            &mut tmp_sources,
        )?;
        let parsed = read_bundle(&bundle_bytes)?;
        if let Some(entry_f) = parsed.files.iter().find(|f| f.path == parsed.entry) {
            let _ = sources.add_bytes(entry_f.path.as_bytes().to_vec(), entry_f.content.clone());
        }
        for f in &parsed.files {
            if f.path != parsed.entry {
                let _ = sources.add_bytes(f.path.as_bytes().to_vec(), f.content.clone());
            }
        }
        let load_opts = LoadOptions {
            library: lib_mode,
            upgrade: false,
            syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
        };
        return load_bundle_with_library(sources, &load_opts, diags);
    }

    // No FILE: load retained program matching `(image, build, schema)` from `--data DIR`
    // (`src/tools/upgrade-bundle.c:mo_upgrade_replay_set`).
    let folder = opts.data_dir?;
    let folder_str = folder.to_string_lossy();
    if folder_str.len() as u64 > MO_LIMIT_PATH_SIZE {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeSourceMismatch,
            Span::NONE,
            "the data folder path is too long",
        ));
        return None;
    }
    let db_path = folder.join("service.sqlite");
    let pair = read_history_pair(&db_path, opts.recover, opts.failure_id).ok();
    let rec = pair
        .as_ref()
        .filter(|p| p.found)
        .and_then(|p| p.message.as_ref())
        .and_then(|m| read_failure_record(&m.bytes));
    let retained = rec.as_ref().and_then(|r| {
        read_retained_program_from_file(&db_path, r.program, &r.build, &r.schema).ok()
    });
    let Some(matched) = retained else {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeSourceMismatch,
            Span::NONE,
            format!(
                "no retained program matches failure {}; replay it with its source FILE",
                opts.failure_id
            ),
        ));
        return None;
    };
    if matched.matches == 0 {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeSourceMismatch,
            Span::NONE,
            format!(
                "no retained program matches failure {}; replay it with its source FILE",
                opts.failure_id
            ),
        ));
        return None;
    }
    if matched.distinct {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeAmbiguousProgram,
            Span::NONE,
            format!(
                "{} retained programs with different sources match failure {}; replay it with the intended source FILE",
                matched.matches, opts.failure_id
            ),
        ));
        return None;
    }
    let prog = matched.program?;
    let (Some(parsed_bundle), Some(parsed_ctx)) = (
        read_bundle(&prog.bundle),
        read_compile_context(&prog.context),
    ) else {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeInvalidValue,
            Span::NONE,
            "the retained program or its context is invalid",
        ));
        return None;
    };
    if let Some(entry_f) = parsed_bundle
        .files
        .iter()
        .find(|f| f.path == parsed_bundle.entry)
    {
        let _ = sources.add_bytes(entry_f.path.as_bytes().to_vec(), entry_f.content.clone());
    }
    for f in &parsed_bundle.files {
        if f.path != parsed_bundle.entry {
            let _ = sources.add_bytes(f.path.as_bytes().to_vec(), f.content.clone());
        }
    }
    for f in &parsed_ctx.files {
        let _ = sources.add_bytes(f.path.as_bytes().to_vec(), f.source.clone());
    }
    let load_opts = LoadOptions {
        library: lib_mode,
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    load_bundle_with_library(sources, &load_opts, diags)
}

fn compile_revision(
    revised_path: &[u8],
    library: Option<&Path>,
    errors: &mut dyn Write,
) -> Result<(Image, u32), OfflineRefusal> {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let opts = LoadOptions {
        library: lib_mode_for(library),
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let compiled = (|| -> Option<(Vec<u8>, Option<usize>)> {
        let set = load_program_bytes(&mut sources, revised_path, &opts, &mut diags)?;
        if diags.has_any() {
            return None;
        }
        let prog = check_program(Some(&sources), set, &mut diags)?;
        if diags.has_any() {
            return None;
        }
        let svc_idx = find_main_service_checked(&prog);
        let lowered = lower_services(&prog, Some(&sources), &mut diags)?;
        if diags.has_any() {
            return None;
        }
        let svcs = (!lowered.services.items.is_empty()).then_some(&lowered.services);
        if !check_program_services(&lowered.arena, &lowered.program, svcs, &mut diags) {
            return None;
        }
        let wire = compile_services(&lowered.arena, &lowered.program, svcs, &mut diags)?;
        (!diags.has_any()).then_some((wire, svc_idx))
    })();

    let Some((wire, svc_opt)) = compiled else {
        diags.sort();
        for d in diags.items() {
            if let Ok(buf) = render_diagnostic_default(
                &sources,
                d,
                RenderFormat::Text,
                TerminalEncoding::Utf8,
                UNICODE_METRICS,
            ) {
                let _ = errors.write_all(&buf);
            }
        }
        let _ = errors.flush();
        return Err(OfflineRefusal::new(
            "revised_program",
            "REVISED does not compile; mo check REVISED shows why",
            None,
            None,
        ));
    };
    let Some(svc_idx) = svc_opt else {
        return Err(OfflineRefusal::new(
            "revised_program",
            "REVISED's main module declares no service",
            None,
            None,
        ));
    };
    let Ok(bc) = load_bytecode(&wire) else {
        return Err(OfflineRefusal::new(
            "revised_program",
            "REVISED's compiled program could not start",
            None,
            None,
        ));
    };
    let Ok(img) = Image::init(bc, Limits::default()) else {
        return Err(OfflineRefusal::new(
            "revised_program",
            "REVISED's compiled program could not start",
            None,
            None,
        ));
    };
    Ok((img, svc_idx as u32))
}

fn check_revision_compat(
    rec: &FailureRecord,
    rev_file: &BytecodeFile,
    rev_svc_idx: u32,
    state_schema: &[u8; 32],
) -> Result<u32, OfflineRefusal> {
    let svc = &rev_file.services[rev_svc_idx as usize];
    check_schema_compat(
        rev_file,
        svc.state_type,
        state_schema,
        "revised_state_schema",
        "the fix changes the state type, so its saved state could not be read",
    )?;
    let timer = rec.timer;
    let count = if timer {
        svc.timers.len()
    } else {
        svc.tools.len()
    };
    let mut found_idx: Option<u32> = None;
    for i in 0..count {
        let fidx = if timer {
            svc.timers[i].function
        } else {
            svc.tools[i].function
        };
        if short_handler_name(rev_file.function_name(fidx)) == rec.name {
            found_idx = Some(i as u32);
        }
    }
    let Some(handler_idx) = found_idx else {
        let rec_desc = describe_handler(
            &rec.name,
            timer,
            rec.read_only,
            rec.handler,
            rec.arguments.len() as u32,
        );
        return Err(OfflineRefusal::new(
            "revised_handler",
            if timer {
                "REVISED's service has no timer of the recorded handler's name"
            } else {
                "REVISED's service has no tool of the recorded handler's name"
            },
            Some(rec_desc),
            Some("none".to_string()),
        ));
    };
    let ro = !timer && svc.tools[handler_idx as usize].read_only;
    let args = if timer {
        0
    } else {
        svc.tools[handler_idx as usize].argument_types.len() as u32
    };
    if ro != rec.read_only || args != rec.arguments.len() as u32 {
        let rec_desc = describe_handler(
            &rec.name,
            timer,
            rec.read_only,
            rec.handler,
            rec.arguments.len() as u32,
        );
        let found_desc = describe_handler(&rec.name, timer, ro, handler_idx, args);
        return Err(OfflineRefusal::new(
            "revised_handler",
            "the revised handler's shape differs: a fix keeps the handler's kind, its result shape class and its arguments",
            Some(rec_desc),
            Some(found_desc),
        ));
    }
    if !timer {
        check_arguments_compat(
            rev_file,
            rec,
            &svc.tools[handler_idx as usize].argument_types,
            "revised_handler",
        )?;
    }
    Ok(handler_idx)
}

fn format_value_literal_text(file: &BytecodeFile, ty: u32, saved: &[u8]) -> String {
    let heap = Heap::new(Limits::default().heap_size);
    let Ok(val) = decode_value(&heap, &file.types, ty, saved) else {
        return "<unavailable: does not decode>".to_string();
    };
    match value_print_bounded(
        &file.types,
        ty,
        &val,
        true,
        MO_LIMIT_OFFLINE_VALUE_BYTES as usize,
    ) {
        Some(printed) => sanitize_terminal_text(&printed),
        None => {
            "<unavailable: longer than MO_LIMIT_OFFLINE_VALUE_BYTES; --json shows it truncated>"
                .to_string()
        }
    }
}

fn render_saved_json(saved: &[u8]) -> String {
    render_offline_value(saved).unwrap_or_else(|| "{\"value\":null,\"complete\":false}".to_string())
}

/// Executes `mo replay` (`src/tools/offline.c:replay` + `mo_offline_program`).
pub fn run_replay(opts: &ReplayOptions<'_>, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let set = load_replay_sources(opts, &mut sources, &mut diags);
    let Some(set) = set.filter(|_| !diags.has_any()) else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "replay",
            output,
            errors,
        );
    };
    let Some(program) = check_program(Some(&sources), set, &mut diags).filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "replay",
            output,
            errors,
        );
    };
    let service_opt = find_main_service_checked(&program);
    let Some(lowered) =
        lower_services(&program, Some(&sources), &mut diags).filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "replay",
            output,
            errors,
        );
    };
    let svcs = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    if !check_program_services(&lowered.arena, &lowered.program, svcs, &mut diags) {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "replay",
            output,
            errors,
        );
    }
    let Some(wire) = compile_services(&lowered.arena, &lowered.program, svcs, &mut diags)
        .filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "replay",
            output,
            errors,
        );
    };

    if opts.json && !write_version_line(output, opts.version, "replay") {
        let _ = writeln!(errors, "mo: could not write version record");
        return 1;
    }

    let bc = match load_bytecode(&wire) {
        Ok(b) => b,
        Err(e) => {
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "compiled bytecode could not be loaded",
                )
                .with_primary_text(e.to_string()),
            );
            return emit_diags_and_finish(
                &sources,
                &mut diags,
                opts.json,
                opts.json,
                opts.version,
                "replay",
                output,
                errors,
            );
        }
    };
    let image = match Image::init(bc, Limits::default()) {
        Ok(img) => img,
        Err(box_err) => {
            let (bc_err, defect) = *box_err;
            let _ = write!(errors, "bug: {}", render_defect_raw(&bc_err, &defect));
            let _ = errors.flush();
            if opts.json {
                let _ = write_totals_line(output, 0, 0, 70);
            }
            return 70;
        }
    };

    let revising = opts.revised.is_some();
    type ReplayPrep = (OfflineInput, u32, Option<(Image, u32, u32)>);
    let result = (|| -> Result<ReplayPrep, OfflineRefusal> {
        let input = if let Some(art) = opts.artifact {
            read_artifact_input(art, opts.failure_id)?
        } else if let Some(dir) = opts.data_dir {
            read_folder_input(dir, opts.recover, opts.failure_id)?
        } else {
            return Err(OfflineRefusal::new(
                "format",
                "replay requires --data or --export",
                None,
                None,
            ));
        };
        check_build_compat(&input, &opts.build_digest)?;
        let Some(svc_idx) = service_opt else {
            return Err(OfflineRefusal::new(
                "program",
                "FILE's main module declares no service",
                None,
                None,
            ));
        };
        let svc_idx = svc_idx as u32;
        let file = image.file();
        let svc = &file.services[svc_idx as usize];
        let Some(ref rec) = input.record else {
            return Err(OfflineRefusal::new(
                "format",
                "the failure record is missing",
                None,
                None,
            ));
        };
        check_program_compat(&input, file, svc_idx)?;
        check_handler_compat(rec, file, svc)?;
        check_schema_compat(
            file,
            svc.state_type,
            &input.schema,
            "state_schema",
            "FILE's state type differs from the recorded state's",
        )?;
        if !rec.timer {
            check_arguments_compat(
                file,
                rec,
                &svc.tools[rec.handler as usize].argument_types,
                "argument_schema",
            )?;
        }
        let rev = if let Some(rev_path) = opts.revised {
            let (rev_img, rev_svc) = compile_revision(rev_path, opts.library, errors)?;
            let rev_handler = check_revision_compat(rec, rev_img.file(), rev_svc, &input.schema)?;
            Some((rev_img, rev_svc, rev_handler))
        } else {
            None
        };
        check_budgets_compat(rec)?;
        Ok((input, svc_idx, rev))
    })();

    let (input, svc_idx, rev_opt) = match result {
        Ok(v) => v,
        Err(refusal) => {
            if opts.json {
                let mut out = format!(
                    "{{\"record\":\"original\",\"failure\":\"{}\",\"outcome\":\"refused\",\"refusal\":",
                    opts.failure_id
                );
                refusal.push_json(&mut out);
                out.push_str("}\n");
                if revising {
                    out.push_str("{\"record\":\"revised\",\"verdict\":\"refused\"}\n");
                }
                let _ = output.write_all(out.as_bytes());
                let _ = write_totals_line(output, 0, 0, 1);
            } else {
                let _ = errors.write_all(refusal.to_text_line("mo replay").as_bytes());
                let _ = errors.flush();
            }
            return 1;
        }
    };

    let Some(ref rec) = input.record else {
        return 1;
    };
    let original_run = offline_call(&image, svc_idx, rec.handler, &input.state, rec);
    let recorded_sig = signature_from_record(rec);
    let class = classify_replay(&recorded_sig, &original_run.outcome);

    let (revised_run_opt, rev_handler_idx, verdict) = match &rev_opt {
        Some((rev_img, rev_svc, rev_h)) => {
            let r = offline_call(rev_img, *rev_svc, *rev_h, &input.state, rec);
            let v = compute_verdict(class, &r.outcome);
            (Some(r), *rev_h, v)
        }
        None => (None, 0, Verdict::Refused),
    };

    let show_mem_note = class == ReplayClass::NotReproduced && recorded_sig.class == 2;
    let status = if revising {
        i32::from(verdict != Verdict::Verified)
    } else {
        i32::from(class != ReplayClass::Reproduced)
    };

    if opts.json {
        let mut out = String::with_capacity(1024);
        let _ = write!(
            out,
            "{{\"record\":\"original\",\"failure\":\"{}\",\"handler\":",
            rec.id
        );
        push_json_string(&mut out, &rec.name);
        let _ = write!(
            out,
            ",\"handler_index\":{},\"timer\":{},\"read_only\":{},\"sequence\":\"{}\",\"arrival_us\":\"{}\",\"state\":{},\"arguments\":[",
            rec.handler,
            rec.timer,
            rec.read_only,
            rec.sequence,
            rec.arrival,
            render_saved_json(&input.state)
        );
        for (i, arg) in rec.arguments.iter().enumerate() {
            if i > 0 {
                out.push(',');
            }
            out.push_str(&render_saved_json(arg));
        }
        out.push_str("],\"recorded\":{\"signature\":");
        recorded_sig.push_json(&mut out, u32::from(rec.signature_version));
        let _ = write!(
            out,
            ",\"context_status\":\"{}\",\"context\":",
            crate::offline_output::context_status_str(rec.context_status)
        );
        push_json_string(&mut out, &rec.context);
        let _ = write!(out, "}},\"outcome\":\"{}\",\"run\":", class.as_str());
        push_run_json(&mut out, &original_run);
        out.push_str(",\"note\":");
        if show_mem_note {
            push_json_string(&mut out, MEMORY_NOTE);
        } else {
            out.push_str("null");
        }
        out.push_str("}\n");

        if let (Some(rev_run), Some(rev_path)) = (&revised_run_opt, opts.revised) {
            out.push_str("{\"record\":\"revised\",\"file\":");
            push_json_string(&mut out, &String::from_utf8_lossy(rev_path));
            out.push_str(",\"handler\":");
            push_json_string(&mut out, &rec.name);
            let _ = write!(out, ",\"handler_index\":{rev_handler_idx},\"run\":");
            push_run_json(&mut out, rev_run);
            let _ = writeln!(out, ",\"verdict\":\"{}\"}}", verdict.as_str());
        }
        let _ = output.write_all(out.as_bytes());
        let _ = write_totals_line(output, 0, 0, status);
    } else {
        let file = image.file();
        let svc = &file.services[svc_idx as usize];
        let mut out = String::with_capacity(1024);
        let _ = writeln!(
            out,
            "replay of failure {}: {}, sequence {}, arrival {} us",
            rec.id,
            describe_handler(
                &rec.name,
                rec.timer,
                rec.read_only,
                rec.handler,
                rec.arguments.len() as u32
            ),
            rec.sequence,
            rec.arrival
        );
        let _ = writeln!(
            out,
            "state: {}",
            format_value_literal_text(file, svc.state_type, &input.state)
        );
        if !rec.timer {
            for (i, arg) in rec.arguments.iter().enumerate() {
                let ty = svc.tools[rec.handler as usize].argument_types[i];
                let _ = writeln!(
                    out,
                    "argument {}: {}",
                    i + 1,
                    format_value_literal_text(file, ty, arg)
                );
            }
        }
        let _ = writeln!(out, "recorded: {}", recorded_sig.to_text());
        push_context_text(&mut out, "recorded stack", rec.context_status, &rec.context);
        push_run_text(&mut out, "offline", &original_run);
        let _ = writeln!(out, "outcome: {}", class.as_str());
        if show_mem_note {
            let _ = writeln!(out, "note: {MEMORY_NOTE}");
        }
        if let Some(ref rev_run) = revised_run_opt {
            push_run_text(&mut out, "revised", rev_run);
            let _ = writeln!(out, "verdict: {}", verdict.as_str());
        }
        let _ = output.write_all(out.as_bytes());
        let _ = output.flush();
    }

    status
}

fn find_query_handler_checked(
    program: &CheckedProgram,
    svc_idx: usize,
    rec: &FailureRecord,
) -> Option<CheckedFuncId> {
    let svc = &program.services[svc_idx];
    if rec.timer {
        svc.timers.iter().find_map(|t| {
            let sym = program
                .interfaces
                .symbol(program.function(t.function).symbol);
            (sym.name == rec.name).then_some(t.function)
        })
    } else {
        svc.tools.iter().find_map(|t| {
            let sym = program
                .interfaces
                .symbol(program.function(t.function).symbol);
            (sym.name == rec.name).then_some(t.function)
        })
    }
}

fn read_query_input(opts: &QueryOptions<'_>) -> Result<OfflineInput, OfflineRefusal> {
    #[cfg(unix)]
    if let Some(sock) = opts.admin_socket {
        return read_live_input(sock, opts.failure_id);
    }
    if let Some(art) = opts.artifact {
        read_artifact_input(art, opts.failure_id)
    } else if let Some(dir) = opts.data_dir {
        read_folder_input(dir, opts.recover, opts.failure_id)
    } else {
        Err(OfflineRefusal::new(
            "format",
            "query requires --data, --export, or --admin-socket",
            None,
            None,
        ))
    }
}

fn emit_query_refusal(
    opts: &QueryOptions<'_>,
    version_written: bool,
    refusal: &OfflineRefusal,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if opts.json {
        if !version_written && !write_version_line(output, opts.version, "query") {
            let _ = writeln!(errors, "mo: could not write version record");
            return 1;
        }
        let mut out = String::from("{\"record\":\"query\",\"function\":");
        push_json_string(&mut out, opts.function);
        out.push_str(",\"outcome\":\"refused\",\"refusal\":");
        refusal.push_json(&mut out);
        out.push_str("}\n");
        let _ = output.write_all(out.as_bytes());
        let _ = write_totals_line(output, 0, 0, 1);
    } else {
        let _ = errors.write_all(refusal.to_text_line("mo query").as_bytes());
        let _ = errors.flush();
    }
    1
}

fn emit_query_stop(
    opts: &QueryOptions<'_>,
    file: Option<&BytecodeFile>,
    defect: &Defect,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if let Some(bc) = file {
        let _ = write!(errors, "bug: {}", render_defect_raw(bc, defect));
    } else if let Some(lname) = defect.limit_name {
        let _ = writeln!(
            errors,
            "bug: {} ({lname} = {})",
            defect.reason, defect.limit
        );
    } else {
        let _ = writeln!(errors, "bug: {}", defect.reason);
    }
    let _ = errors.flush();
    if opts.json {
        let mut out = String::from("{\"record\":\"query\",\"function\":");
        push_json_string(&mut out, opts.function);
        out.push_str(",\"outcome\":\"stopped\",\"reason\":");
        push_json_string(&mut out, &defect.reason);
        out.push_str(",\"limit\":");
        match defect.limit_name {
            Some(l) => push_json_string(&mut out, l),
            None => out.push_str("null"),
        }
        out.push_str("}\n");
        let _ = output.write_all(out.as_bytes());
        let _ = write_totals_line(output, 0, 0, 70);
    }
    70
}

/// Executes `mo query` (`src/tools/offline.c:query` + `mo_offline_program`).
pub fn run_query(opts: &QueryOptions<'_>, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let load_opts = LoadOptions {
        library: lib_mode_for(opts.library),
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let Some(set) = load_program_bytes(&mut sources, opts.file, &load_opts, &mut diags)
        .filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "query",
            output,
            errors,
        );
    };
    let Some(mut program) =
        check_program(Some(&sources), set, &mut diags).filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "query",
            output,
            errors,
        );
    };

    let service_opt = find_main_service_checked(&program);
    let query_kind = if opts.failure_id == 0 {
        QueryInput::State
    } else {
        QueryInput::Failure
    };

    // A failure query reads its input & checks build & handler BEFORE `check_query_entry`;
    // a state query runs `check_query_entry` BEFORE reading its input (`src/tools/offline.c:1130`).
    let mut prefetched_input: Option<OfflineInput> = None;
    let mut handler_fn_id: Option<CheckedFuncId> = None;
    if query_kind == QueryInput::Failure {
        let input = match read_query_input(opts) {
            Ok(i) => i,
            Err(r) => return emit_query_refusal(opts, false, &r, output, errors),
        };
        if let Err(r) = check_build_compat(&input, &opts.build_digest) {
            return emit_query_refusal(opts, false, &r, output, errors);
        }
        let Some(svc_idx) = service_opt else {
            let r = OfflineRefusal::new(
                "handler",
                "FILE's main module declares no service",
                None,
                None,
            );
            return emit_query_refusal(opts, false, &r, output, errors);
        };
        let Some(ref rec) = input.record else {
            let r = OfflineRefusal::new("format", "the failure record is missing", None, None);
            return emit_query_refusal(opts, false, &r, output, errors);
        };
        let Some(hf) = find_query_handler_checked(&program, svc_idx, rec) else {
            let desc = describe_handler(
                &rec.name,
                rec.timer,
                rec.read_only,
                rec.handler,
                rec.arguments.len() as u32,
            );
            let r = OfflineRefusal::new(
                "handler",
                "FILE's service has no handler of the failure's name and kind",
                Some(desc),
                Some("none".to_string()),
            );
            return emit_query_refusal(opts, false, &r, output, errors);
        };
        handler_fn_id = Some(hf);
        prefetched_input = Some(input);
    }

    let entry = check_query_entry(
        &mut program,
        opts.function.as_bytes(),
        query_kind,
        handler_fn_id,
        &mut diags,
    );
    let query_fn_id = match entry {
        QueryEntryStatus::Accepted(id) if !diags.has_any() => id,
        QueryEntryStatus::NoModule => {
            let _ = writeln!(errors, "mo query: no module of that name in FILE's program");
            let _ = errors.flush();
            return 2;
        }
        QueryEntryStatus::NoFunction => {
            let _ = writeln!(errors, "mo query: no function of that name in that module");
            let _ = errors.flush();
            return 2;
        }
        _ => {
            return emit_diags_and_finish(
                &sources,
                &mut diags,
                opts.json,
                false,
                opts.version,
                "query",
                output,
                errors,
            );
        }
    };

    let Some(lowered) =
        lower_query(&program, Some(&sources), query_fn_id, &mut diags).filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "query",
            output,
            errors,
        );
    };
    let Some(core_index) = lowered.query_index else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "query",
            output,
            errors,
        );
    };
    let svcs = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    if !check_program_services(&lowered.arena, &lowered.program, svcs, &mut diags) {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "query",
            output,
            errors,
        );
    }
    let Some(wire) = compile_services(&lowered.arena, &lowered.program, svcs, &mut diags)
        .filter(|_| !diags.has_any())
    else {
        return emit_diags_and_finish(
            &sources,
            &mut diags,
            opts.json,
            false,
            opts.version,
            "query",
            output,
            errors,
        );
    };

    if opts.json && !write_version_line(output, opts.version, "query") {
        let _ = writeln!(errors, "mo: could not write version record");
        return 1;
    }

    let bc = match load_bytecode(&wire) {
        Ok(b) => b,
        Err(e) => {
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "compiled bytecode could not be loaded",
                )
                .with_primary_text(e.to_string()),
            );
            return emit_diags_and_finish(
                &sources,
                &mut diags,
                opts.json,
                opts.json,
                opts.version,
                "query",
                output,
                errors,
            );
        }
    };
    let image = match Image::init(bc, Limits::default()) {
        Ok(img) => img,
        Err(box_err) => {
            let (bc_err, defect) = *box_err;
            let _ = write!(errors, "bug: {}", render_defect_raw(&bc_err, &defect));
            let _ = errors.flush();
            if opts.json {
                let _ = write_totals_line(output, 0, 0, 70);
            }
            return 70;
        }
    };

    let input = match prefetched_input {
        Some(i) => i,
        None => match read_query_input(opts) {
            Ok(i) => i,
            Err(r) => return emit_query_refusal(opts, opts.json, &r, output, errors),
        },
    };

    let file = image.file();
    let Some(svc_idx) = service_opt else {
        let r = OfflineRefusal::new(
            "program",
            "FILE's main module declares no service",
            None,
            None,
        );
        return emit_query_refusal(opts, opts.json, &r, output, errors);
    };
    let svc = &file.services[svc_idx];
    let qfn = &file.functions[core_index as usize];

    if let Err(r) = check_build_compat(&input, &opts.build_digest) {
        return emit_query_refusal(opts, opts.json, &r, output, errors);
    }
    if let Err(r) = check_schema_compat(
        file,
        svc.state_type,
        &input.schema,
        "state_schema",
        "FILE's state type differs from the exported state's",
    ) {
        return emit_query_refusal(opts, opts.json, &r, output, errors);
    }
    if let Some(ref rec) = input.record {
        let arg_tys: Vec<u32> = qfn.parameters.iter().skip(2).map(|p| p.ty).collect();
        if let Err(r) = check_arguments_compat(file, rec, &arg_tys, "argument_schema") {
            return emit_query_refusal(opts, opts.json, &r, output, errors);
        }
    }

    let mut proc = Process::new(
        &image,
        opts.limits,
        HostContext::stdio(Vec::new(), Vec::new()),
    );
    let mut call_args = Vec::with_capacity(qfn.parameters.len());
    let decoded_ok = (|| -> bool {
        let Ok(st_val) = decode_value(proc.heap(), &file.types, qfn.parameters[0].ty, &input.state)
        else {
            return false;
        };
        call_args.push(st_val);
        if let Some(ref rec) = input.record {
            call_args.push(Value::Time(rec.arrival));
            for (i, param) in qfn.parameters.iter().enumerate().skip(2) {
                let Some(raw) = rec.arguments.get(i - 2) else {
                    return false;
                };
                let Ok(v) = decode_value(proc.heap(), &file.types, param.ty, raw) else {
                    return false;
                };
                call_args.push(v);
            }
        }
        true
    })();

    if !decoded_ok {
        let defect = Defect {
            reason: "running out of memory".to_string(),
            limit_name: Some("--max-heap"),
            limit: opts.limits.heap_size,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        };
        return emit_query_stop(opts, Some(file), &defect, output, errors);
    }

    let mut remaining = opts.max_work.saturating_add(1);
    let mut keep = || {
        if remaining == 0 {
            false
        } else {
            remaining -= 1;
            true
        }
    };
    let outcome = proc.call_controlled(core_index, call_args, Some(&mut keep), None);
    match outcome {
        ControlledOutcome::Returned(result_val) => {
            if opts.json {
                let mut saved = Vec::new();
                let rendered = if encode_value(
                    &file.types,
                    qfn.result_type,
                    &result_val,
                    &mut saved,
                )
                .is_ok()
                {
                    render_saved_json(&saved)
                } else {
                    "{\"value\":null,\"complete\":false}".to_string()
                };
                let mut out = String::from("{\"record\":\"query\",\"function\":");
                push_json_string(&mut out, opts.function);
                let _ = writeln!(out, ",\"outcome\":\"returned\",\"result\":{rendered}}}");
                let _ = output.write_all(out.as_bytes());
                let _ = write_totals_line(output, 0, 0, 0);
            } else {
                let text = match value_print_bounded(
                    &file.types,
                    qfn.result_type,
                    &result_val,
                    false,
                    MO_LIMIT_OFFLINE_VALUE_BYTES as usize,
                ) {
                    Some(p) => sanitize_terminal_text(&p),
                    None => "<unavailable: longer than MO_LIMIT_OFFLINE_VALUE_BYTES; --json shows it truncated>".to_string(),
                };
                let _ = writeln!(output, "{text}");
                let _ = output.flush();
            }
            0
        }
        ControlledOutcome::Cancelled => {
            let defect = Defect {
                reason: "running out of work".to_string(),
                limit_name: Some("--max-work"),
                limit: opts.max_work.min(usize::MAX as u64) as usize,
                detail: None,
                frames: Vec::new(),
                variables: Vec::new(),
                variables_unavailable: false,
            };
            emit_query_stop(opts, Some(file), &defect, output, errors)
        }
        ControlledOutcome::Failed(defect) | ControlledOutcome::Stopped(defect) => {
            emit_query_stop(opts, Some(file), &defect, output, errors)
        }
    }
}
