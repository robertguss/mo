//! Offline `mo history` command (`src/tools/history.c` and `src/tools/offline-output.c` parity).

use std::fmt::Write as _;
use std::io::Write;
use std::path::Path;

use mo_def::limits::{MO_LIMIT_OFFLINE_VALUE_BYTES, MO_LIMIT_PATH_SIZE};
use mo_host::HostContext;
use mo_vm::{Limits, Outcome, Process};

use crate::binary::failure::read_failure_record;
use crate::binary::{hex_encode, push_json_string, sanitize_terminal_text};
use crate::protocol::{Pf, build_protocol_image};
use crate::store::history::{
    FolderStatus, HistoryEntry, HistoryFailure, StoreHistory, read_history,
};
use crate::store::programs::StoredProgram;

const GONE_NOTE: &str = "messages before the snapshot are gone, so a message absent from the log tail may still have run";
const PRE_CAPTURE_NOTE: &str =
    "this store predates failure capture, so it holds no failure records";
const STATE_CHECK_NOTE: &str = "the record's pre-call state failed its check";

/// Options for `mo history DIR [--recover] [--json]`.
pub struct HistoryOptions<'a> {
    /// Data folder path.
    pub folder: &'a str,
    /// Whether `--recover` was passed.
    pub recover: bool,
    /// Whether `--json` was passed.
    pub json: bool,
    /// CLI version string for the NDJSON `version` record (e.g. `"0.1.0"`).
    pub version: &'a str,
    /// 32-byte build runtime identity digest for replayability checks.
    pub build_digest: [u8; 32],
}

/// Executes `mo history` (`src/tools/history.c:mo_history`).
pub fn run_history(
    opts: &HistoryOptions<'_>,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    if opts.folder.is_empty() || opts.folder.len() as u64 > MO_LIMIT_PATH_SIZE {
        let _ = writeln!(errors, "mo history: the data folder's path is too long");
        return 2;
    }

    if opts.json {
        let mut vline = String::new();
        vline.push_str("{\"record\":\"version\",\"format\":\"1.0\",\"mo\":");
        push_json_string(&mut vline, opts.version);
        vline.push_str(",\"command\":\"history\"}\n");
        if output.write_all(vline.as_bytes()).is_err() || output.flush().is_err() {
            let _ = writeln!(errors, "mo: could not write version record");
            return 1;
        }
    }

    let db_path = Path::new(opts.folder).join("service.sqlite");
    match read_history(&db_path, opts.recover) {
        Err((status, _recovered, damaged_seq)) => {
            let msg = folder_error_message(opts.folder, status, damaged_seq);
            if opts.json {
                let mut out = String::new();
                out.push_str("{\"record\":\"history\",\"data\":");
                push_json_string(&mut out, opts.folder);
                out.push_str(",\"folder\":\"");
                out.push_str(folder_status_code(status));
                out.push_str("\",\"reason\":");
                push_json_string(&mut out, &msg);
                out.push_str(
                    "}\n{\"record\":\"totals\",\"errors\":0,\"dropped\":0,\"status\":1}\n",
                );
                let _ = output.write_all(out.as_bytes());
                let _ = output.flush();
            } else {
                let _ = writeln!(errors, "mo history: {msg}");
                let _ = errors.flush();
            }
            1
        }
        Ok(history) => {
            let rendered_state = match (&history.snapshot, history.snapshot_copied) {
                (Some(snap), true) => match render_offline_value(&snap.bytes) {
                    Some(s) => Some(s),
                    None => {
                        let _ = writeln!(errors, "mo history: the value renderer is unavailable");
                        return 1;
                    }
                },
                _ => None,
            };

            let text = if opts.json {
                let mut s = format_history_json(
                    opts.folder,
                    &history,
                    rendered_state.as_deref(),
                    opts.build_digest,
                );
                s.push_str("{\"record\":\"totals\",\"errors\":0,\"dropped\":0,\"status\":0}\n");
                s
            } else {
                format_history_text(
                    opts.folder,
                    &history,
                    rendered_state.as_deref(),
                    opts.build_digest,
                )
            };
            let _ = output.write_all(text.as_bytes());
            let _ = output.flush();
            0
        }
    }
}

/// Renders `MOSV\x01` saved value bytes using embedded `McpViews.offline_value` (`mo_offline_render`).
#[must_use]
pub fn render_offline_value(saved_bytes: &[u8]) -> Option<String> {
    let (proto, fns) = build_protocol_image().ok()?;
    let mut proc = Process::new(
        &proto,
        Limits::default(),
        HostContext::stdio(Vec::new(), Vec::new()),
    );
    let bval = proc.heap().alloc_bytes(saved_bytes.to_vec()).ok()?;
    let limit = mo_heap::Value::Int(MO_LIMIT_OFFLINE_VALUE_BYTES as i64);
    let (outcome, _) = proc.call_with_budget(
        fns.get(Pf::OfflineValue),
        vec![bval, limit],
        mo_def::limits::MO_LIMIT_MCP_REQUEST_WORK,
    );
    match outcome {
        Outcome::Returned(v) => Some(v.as_text().to_string()),
        _ => None,
    }
}

/// Returns the JSON folder status code (`mo_offline_folder_code`).
#[must_use]
pub fn folder_status_code(status: FolderStatus) -> &'static str {
    match status {
        FolderStatus::Ok => "ok",
        FolderStatus::Missing => "missing",
        FolderStatus::Owned => "owned",
        FolderStatus::Recovery => "recovery_required",
        FolderStatus::Unreadable => "unreadable",
        FolderStatus::Damaged => "damaged",
        FolderStatus::Io => "io",
        FolderStatus::Memory => "memory",
        FolderStatus::Handles => "handles",
    }
}

/// Formats the canonical offline folder error message (`src/tools/offline-output.c:mo_offline_folder_message`).
#[must_use]
pub fn folder_error_message(folder: &str, status: FolderStatus, damaged_seq: u64) -> String {
    let safe = sanitize_terminal_text(folder);
    match status {
        FolderStatus::Missing => format!("no saved data in {safe}"),
        FolderStatus::Owned => {
            format!("a running service owns {safe}; nothing was read, and nothing waited for it")
        }
        FolderStatus::Recovery => format!(
            "recovery required in {safe}: an unfinished write left a journal to roll back; run again as the folder's owner with --recover, which rolls it back by writing database pages but changes no record"
        ),
        FolderStatus::Unreadable => format!("cannot read {safe}"),
        FolderStatus::Damaged => {
            if damaged_seq != 0 {
                format!("damaged saved data in {safe}: log record {damaged_seq} failed its check")
            } else {
                format!("damaged saved data in {safe}")
            }
        }
        FolderStatus::Io => format!("could not read {safe}"),
        FolderStatus::Memory => "out of memory while reading saved data".to_string(),
        FolderStatus::Handles => "too many open stores while reading saved data".to_string(),
        FolderStatus::Ok => String::new(),
    }
}

/// Returns the build replayability refusal reason, or `None` if this build matches
/// (`src/tools/offline-output.c:mo_offline_build_refusal`).
#[must_use]
pub fn build_refusal(
    record_build: &[u8; 32],
    record_label: &str,
    current_build: &[u8; 32],
) -> Option<&'static str> {
    if *record_build == [0u8; 32] || record_label.is_empty() {
        Some("the record does not identify the build that recorded it")
    } else if record_build != current_build {
        Some("recorded by another build")
    } else {
        None
    }
}

/// Canonical failure signature fields shared across `mo history`, `mo replay`, and `mo query`
/// (`struct mo_signature`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FailureSignature {
    /// Class code: `0` = defect, `1` = work, `2` = memory, `3` = state_size.
    pub class: u8,
    /// Human-readable reason (e.g. `"stop"`, `"out of work"`).
    pub reason: String,
    /// Limit name (e.g. `"MO_LIMIT_SERVICE_WORK"`, or empty).
    pub limit: String,
    /// Numeric limit value (`0` if none).
    pub limit_value: u64,
    /// Bytecode function index.
    pub function: u32,
    /// Source file index.
    pub file: u32,
    /// Source span start byte offset.
    pub start: u32,
    /// Source span end byte offset.
    pub end: u32,
}

impl FailureSignature {
    /// Returns the JSON class name (`mo_signature_class_name`).
    #[must_use]
    pub fn class_name(&self) -> &'static str {
        match self.class {
            0 => "defect",
            1 => "work",
            2 => "memory",
            _ => "state_size",
        }
    }

    /// Appends the canonical JSON object (`src/tools/offline-output.c:mo_offline_signature_json`).
    pub fn push_json(&self, out: &mut String, version: u32) {
        let _ = write!(
            out,
            "{{\"version\":{version},\"class\":\"{}\",\"reason\":",
            self.class_name()
        );
        push_json_string(out, &self.reason);
        out.push_str(",\"limit\":");
        push_json_string(out, &self.limit);
        let _ = write!(
            out,
            ",\"value\":\"{}\",\"function\":\"{}\",\"file\":\"{}\",\"start\":\"{}\",\"end\":\"{}\"}}",
            self.limit_value, self.function, self.file, self.start, self.end
        );
    }

    /// Formats the canonical terminal description (`src/tools/offline-output.c:mo_offline_signature_text`).
    #[must_use]
    pub fn to_text(&self) -> String {
        let mut out = String::new();
        out.push_str(self.class_name());
        if !self.reason.is_empty() {
            out.push_str(" \"");
            out.push_str(&sanitize_terminal_text(&self.reason));
            out.push('"');
        }
        if !self.limit.is_empty() {
            out.push_str(" at ");
            out.push_str(&sanitize_terminal_text(&self.limit));
            let _ = write!(out, " = {}", self.limit_value);
        }
        if self.class == 0 {
            let _ = write!(
                out,
                " in function {}, file {} bytes {}-{}",
                self.function, self.file, self.start, self.end
            );
        }
        out
    }
}

fn format_history_json(
    folder: &str,
    h: &StoreHistory,
    rendered_state: Option<&str>,
    build_digest: [u8; 32],
) -> String {
    let mut out = String::with_capacity(1024);
    out.push_str("{\"record\":\"history\",\"data\":");
    push_json_string(&mut out, folder);
    let _ = write!(out, ",\"folder\":\"ok\",\"recovered\":{}", h.recovered);

    out.push_str(",\"program\":");
    match &h.program {
        Some(p) => push_program_json(&mut out, p),
        None => out.push_str("null"),
    }
    out.push_str(",\"previous_programs\":[");
    for (i, p) in h.previous_programs.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        push_program_json(&mut out, p);
    }
    out.push_str("],\"upgrades\":[");
    for (i, u) in h.upgrades.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        let _ = write!(
            out,
            "{{\"ordinal\":\"{}\",\"head\":\"{}\",\"old_source\":",
            u.ordinal, u.head
        );
        if u.old_source == [0u8; 32] {
            out.push_str("null");
        } else {
            let _ = write!(out, "\"{}\"", hex_encode(&u.old_source));
        }
        let _ = write!(out, ",\"new_source\":\"{}\"", hex_encode(&u.new_source));
        if u.program_retained {
            out.push_str(",\"program_retained\":true}");
        } else {
            out.push_str(",\"program_retained\":false,\"reason\":\"program not retained\"}");
        }
    }
    let _ = write!(
        out,
        "],\"upgrade_records\":\"{}\",\"upgrades_truncated\":{}",
        h.upgrade_count, h.upgrades_truncated
    );

    out.push_str(",\"snapshot\":");
    match &h.snapshot {
        None => out.push_str("null"),
        Some(snap) => {
            let _ = write!(
                out,
                "{{\"sequence\":\"{}\",\"later_messages\":{},\"producer_build\":null,\"state_bytes\":\"{}\",\"state\":",
                snap.sequence,
                h.head.saturating_sub(snap.sequence),
                snap.bytes.len()
            );
            match rendered_state {
                Some(r) => out.push_str(r),
                None => out.push_str("{\"value\":null,\"complete\":false}"),
            }
            out.push('}');
        }
    }

    let _ = write!(out, ",\"head\":\"{}\",\"log\":[", h.head);
    for (i, e) in h.entries.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        push_entry_json(&mut out, e);
    }
    let _ = write!(out, "],\"log_entries\":{},\"log_note\":", h.log_count);
    push_json_string(&mut out, GONE_NOTE);
    if h.failure_tables {
        out.push_str(",\"failure_capture\":true,\"failure_note\":null");
    } else {
        out.push_str(",\"failure_capture\":false,\"failure_note\":");
        push_json_string(&mut out, PRE_CAPTURE_NOTE);
    }
    out.push_str(",\"failures\":[");
    for (i, f) in h.failures.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        push_failure_json(&mut out, f, build_digest);
    }
    let _ = writeln!(
        out,
        "],\"failure_records\":{},\"retained_bytes\":\"{}\",\"left_bytes\":\"{}\"}}",
        h.failure_total, h.retained_bytes, h.left_bytes
    );
    out
}

fn push_program_json(out: &mut String, p: &StoredProgram) {
    let _ = write!(
        out,
        "{{\"ordinal\":\"{}\",\"image\":\"{}\",\"source\":\"{}\",\"build\":\"{}\",\"schema\":\"{}\",\"context_digest\":\"{}\"}}",
        p.ordinal,
        p.image,
        hex_encode(&p.source),
        hex_encode(&p.build),
        hex_encode(&p.schema),
        hex_encode(&p.context_digest)
    );
}

fn push_entry_json(out: &mut String, e: &HistoryEntry) {
    let total: u64 = e.argument_bytes.iter().map(|&n| u64::from(n)).sum();
    let _ = write!(
        out,
        "{{\"handler\":null,\"handler_index\":{},\"timer\":{},\"arrival_us\":\"{}\",\"sequence\":\"{}\",\"argument_bytes\":\"{}\",\"argument_sizes\":[",
        e.handler, e.timer, e.arrival, e.sequence, total
    );
    for (i, &sz) in e.argument_bytes.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        let _ = write!(out, "\"{sz}\"");
    }
    out.push_str("],\"size_unavailable_reason\":null}");
}

fn push_failure_json(out: &mut String, f: &HistoryFailure, build_digest: [u8; 32]) {
    let _ = write!(out, "{{\"id\":\"{}\",\"kind\":", f.id);
    push_json_string(out, &f.kind);
    let Some(r) = read_failure_record(&f.message.bytes).filter(|r| r.id == f.id) else {
        out.push_str(
            ",\"unreadable\":true,\"replayable_here\":false,\"not_replayable\":\"the record's message does not decode\"}",
        );
        return;
    };
    out.push_str(",\"handler\":");
    push_json_string(out, &r.name);
    let _ = write!(
        out,
        ",\"arrival\":\"{}\",\"state_bytes\":\"{}\",\"message_bytes\":\"{}\",\"signature\":",
        r.arrival,
        f.state_bytes,
        f.message.bytes.len()
    );
    let sig = FailureSignature {
        class: r.class,
        reason: r.reason.clone(),
        limit: r.limit.clone(),
        limit_value: r.limit_value,
        function: r.function,
        file: r.file,
        start: r.start,
        end: r.end,
    };
    sig.push_json(out, u32::from(r.signature_version));
    out.push_str(",\"build_label\":");
    push_json_string(out, &r.label);
    let _ = write!(out, ",\"build\":\"{}\"", hex_encode(&r.build));
    let refusal = if !f.state_valid {
        Some(STATE_CHECK_NOTE)
    } else {
        build_refusal(&r.build, &r.label, &build_digest)
    };
    match refusal {
        None => out.push_str(",\"replayable_here\":true,\"not_replayable\":null}"),
        Some(reason) => {
            out.push_str(",\"replayable_here\":false,\"not_replayable\":");
            push_json_string(out, reason);
            out.push('}');
        }
    }
}

fn format_history_text(
    folder: &str,
    h: &StoreHistory,
    rendered_state: Option<&str>,
    build_digest: [u8; 32],
) -> String {
    let mut out = String::new();
    let _ = write!(out, "history of {}", sanitize_terminal_text(folder));
    if h.recovered {
        out.push_str(" (recovered: a journal was rolled back, writing database pages but changing no record)\n");
    } else {
        out.push('\n');
    }

    match &h.program {
        None => out.push_str("program: not recorded (legacy folder)\n"),
        Some(p) => {
            let _ = writeln!(
                out,
                "program: source {}, context {}, activation {}",
                hex_encode(&p.source),
                hex_encode(&p.context_digest),
                p.ordinal
            );
            for u in &h.upgrades {
                let _ = write!(out, "upgrade {} at head {}: ", u.ordinal, u.head);
                if u.old_source == [0u8; 32] {
                    out.push_str("source unavailable (legacy)");
                } else {
                    out.push_str(&hex_encode(&u.old_source));
                }
                let _ = writeln!(
                    out,
                    " -> {} ({})",
                    hex_encode(&u.new_source),
                    if u.program_retained {
                        "program retained"
                    } else {
                        "program not retained"
                    }
                );
            }
            if h.upgrades_truncated {
                out.push_str("more upgrade records not copied: over the history bound\n");
            }
        }
    }

    match &h.snapshot {
        None => out.push_str("snapshot: none; the service's first start never finished\n"),
        Some(snap) => {
            let _ = writeln!(
                out,
                "snapshot: as of sequence {}, {} later messages not applied; producer build not recorded",
                snap.sequence,
                h.head.saturating_sub(snap.sequence)
            );
            let _ = write!(out, "state ({} saved bytes): ", snap.bytes.len());
            match rendered_state {
                Some(r) => out.push_str(&sanitize_terminal_text(r)),
                None => out.push_str("not copied: over the history bound"),
            }
            out.push('\n');
        }
    }

    let _ = writeln!(
        out,
        "log tail: {} {} after the snapshot",
        h.log_count,
        if h.log_count == 1 {
            "message"
        } else {
            "messages"
        }
    );
    for e in &h.entries {
        let _ = write!(
            out,
            "  {}: {} {} (name not recorded in the log), arrival {} us, {} {}",
            e.sequence,
            if e.timer { "timer" } else { "tool" },
            e.handler,
            e.arrival,
            e.argument_bytes.len(),
            if e.argument_bytes.len() == 1 {
                "argument"
            } else {
                "arguments"
            }
        );
        for (i, &sz) in e.argument_bytes.iter().enumerate() {
            let _ = write!(
                out,
                "{}{sz}{}",
                if i == 0 { " (" } else { ", " },
                if i + 1 == e.argument_bytes.len() {
                    " bytes)"
                } else {
                    ""
                }
            );
        }
        out.push('\n');
    }
    if h.entries.len() < h.log_count as usize {
        let _ = writeln!(
            out,
            "  {} more not copied: over the history bound",
            h.log_count as usize - h.entries.len()
        );
    }
    let _ = writeln!(out, "note: {GONE_NOTE}");

    if !h.failure_tables {
        let _ = writeln!(out, "failures: none; {PRE_CAPTURE_NOTE}");
    } else {
        let _ = writeln!(out, "failures: {} retained", h.failure_total);
        for f in &h.failures {
            let _ = write!(out, "  {}: ", f.id);
            let Some(r) = read_failure_record(&f.message.bytes).filter(|r| r.id == f.id) else {
                out.push_str("unreadable record (its message does not decode); not replayable\n");
                continue;
            };
            let sig = FailureSignature {
                class: r.class,
                reason: r.reason.clone(),
                limit: r.limit.clone(),
                limit_value: r.limit_value,
                function: r.function,
                file: r.file,
                start: r.start,
                end: r.end,
            };
            let _ = write!(
                out,
                "{} ({}), {}\n     recorded by {} (sha256 {}); ",
                sanitize_terminal_text(&r.name),
                if r.timer { "timer" } else { "tool" },
                sig.to_text(),
                sanitize_terminal_text(&r.label),
                hex_encode(&r.build)
            );
            let refusal = if !f.state_valid {
                Some(STATE_CHECK_NOTE)
            } else {
                build_refusal(&r.build, &r.label, &build_digest)
            };
            if let Some(reason) = refusal {
                let _ = writeln!(out, "not replayable here: {reason}");
            } else {
                out.push_str("replayable here\n");
            }
        }
        if h.failures.len() < h.failure_total as usize {
            let _ = writeln!(
                out,
                "  {} more not copied: over the history bound",
                h.failure_total as usize - h.failures.len()
            );
        }
    }

    if h.left_bytes != 0 {
        let _ = writeln!(
            out,
            "copied {} bytes and left {} bytes over MO_LIMIT_HISTORY_BYTES",
            h.retained_bytes, h.left_bytes
        );
    }
    out
}
