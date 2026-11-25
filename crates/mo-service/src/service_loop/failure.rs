//! Failure context formatting and failure record construction (`src/vm/service-failure.c` parity).

use std::fmt::Write as _;

use mo_bytecode::{BytecodeFile, NO_INDEX};
use mo_def::limits::{
    MO_LIMIT_FAILURE_CONTEXT_BYTES, MO_LIMIT_FAILURE_FRAMES, MO_LIMIT_FAILURE_KIND_BYTES,
    MO_LIMIT_FAILURE_VALUE_BYTES, MO_LIMIT_SAVED_BYTES, MO_LIMIT_SERVICE_WORK,
};
use mo_vm::{Defect, Limits};

use crate::binary::failure::{FailureContextStatus, FailureRecord, write_failure_record};

/// Formats defect stack frames and local variables into a bounded UTF-8 context string
/// (`src/vm/service-failure.c:mo_service_context`).
#[must_use]
pub fn format_failure_context(
    _file: &BytecodeFile,
    defect: &Defect,
) -> (FailureContextStatus, String) {
    let mut out = String::new();
    let max_len = MO_LIMIT_FAILURE_CONTEXT_BYTES as usize;
    let max_val = MO_LIMIT_FAILURE_VALUE_BYTES as usize;
    let mut truncated = false;
    let mut unavailable = false;

    for (idx, frame) in defect.frames.iter().enumerate() {
        if idx as u64 >= MO_LIMIT_FAILURE_FRAMES {
            truncated = true;
            break;
        }
        let (file_idx, start, end) = frame.span.unwrap_or((NO_INDEX, 0, 0));
        let mut line = String::new();
        let _ = writeln!(
            line,
            "frame {} file {} span {}..{}",
            frame.function, file_idx, start, end
        );
        if out.len() + line.len() > max_len {
            truncated = true;
            break;
        }
        out.push_str(&line);
    }

    if !truncated && !defect.variables.is_empty() {
        for var in &defect.variables {
            let rendered = !var.truncated && var.value.len() <= max_val;
            let mut line = String::new();
            if rendered {
                let _ = writeln!(line, "variable {} available: {}", var.name, var.value);
            } else {
                unavailable = true;
                let _ = writeln!(line, "variable {} unavailable: ", var.name);
            }
            if out.len() + line.len() > max_len {
                truncated = true;
                break;
            }
            out.push_str(&line);
        }
    }

    let status = if defect.variables_unavailable && out.is_empty() {
        FailureContextStatus::Unavailable
    } else if truncated || unavailable {
        FailureContextStatus::Truncated
    } else {
        FailureContextStatus::Complete
    };
    (status, out)
}

/// Builds the canonical failure kind key string used for deduplication and retention
/// (`src/vm/service-failure.c:227-238` parity, bounded to `MO_LIMIT_FAILURE_KIND_BYTES`).
#[must_use]
#[expect(
    clippy::too_many_arguments,
    reason = "matches C failure_kind(handler, class, reason, limit, function, file, start, end)"
)]
pub fn build_failure_kind(
    handler_name: &str,
    class: u8,
    reason: &str,
    limit: &str,
    function: u32,
    file: u32,
    start: u32,
    end: u32,
) -> String {
    let raw = if class == 0 {
        format!("{handler_name}:{class}:{reason}:{limit}:{function}:{file}:{start}:{end}")
    } else {
        format!("{handler_name}:{class}:{reason}:{limit}")
    };
    let max_len = MO_LIMIT_FAILURE_KIND_BYTES as usize;
    if raw.len() <= max_len {
        return raw;
    }
    let mut cut = max_len;
    while cut > 0 && !raw.is_char_boundary(cut) {
        cut -= 1;
    }
    raw[..cut].to_string()
}

/// Parameters needed to construct a binary v1 failure message payload.
pub struct FailureBuildParams<'a> {
    /// Service sequence number at which the failure occurred.
    pub sequence: u64,
    /// Arrival timestamp in microseconds since epoch.
    pub arrival: i64,
    /// `true` for a timer tick, `false` for a tool call.
    pub timer: bool,
    /// Tool or timer index.
    pub handler: u32,
    /// Tool or timer name.
    pub name: &'a str,
    /// Whether the tool is read-only.
    pub read_only: bool,
    /// Encoded `MOSV\x01` arguments (empty for timers).
    pub arguments: Vec<Vec<u8>>,
    /// Failure class (`0` = defect, `1` = limit, `2` = bug, `3` = save).
    pub class: u8,
    /// Failure defect details.
    pub defect: &'a Defect,
    /// Service image key (`program_checksum ^ ((service as u64) << 32)`).
    pub image_key: u64,
    /// Configured VM limits.
    pub limits: Limits,
    /// 32-byte runtime identity digest.
    pub build_digest: [u8; 32],
    /// Build label string.
    pub build_label: &'a str,
    /// 32-byte state schema SHA-256 digest.
    pub schema_digest: [u8; 32],
    /// Bytecode file for stack frame symbolication.
    pub file: &'a BytecodeFile,
}

/// Builds `(kind_string, encoded_v1_payload_bytes)` ready to pass to `ServiceStore::capture_failure`.
#[must_use]
pub fn build_failure_payload(params: FailureBuildParams<'_>) -> Option<(String, Vec<u8>)> {
    let (reason, limit, limit_value) = if params.class == 1 {
        (
            "work limit".to_string(),
            "MO_LIMIT_SERVICE_WORK".to_string(),
            MO_LIMIT_SERVICE_WORK,
        )
    } else {
        (
            truncate_str(&params.defect.reason, MO_LIMIT_FAILURE_KIND_BYTES as usize),
            params
                .defect
                .limit_name
                .map(|s| truncate_str(s, MO_LIMIT_FAILURE_KIND_BYTES as usize))
                .unwrap_or_default(),
            params.defect.limit as u64,
        )
    };

    let (top_fn, top_file, top_start, top_end) = match params.defect.frames.first() {
        Some(f) => match f.span {
            Some((file_idx, start, end)) => (f.function, file_idx, start, end),
            None => (f.function, NO_INDEX, 0, 0),
        },
        None => (NO_INDEX, NO_INDEX, 0, 0),
    };
    let kind = build_failure_kind(
        params.name,
        params.class,
        &reason,
        &limit,
        top_fn,
        top_file,
        top_start,
        top_end,
    );
    let (context_status, context) = format_failure_context(params.file, params.defect);

    let rec = FailureRecord {
        id: 0, // Store assigns id and patches FAILURE_ID_OFFSET atomically.
        sequence: params.sequence,
        arrival: params.arrival,
        timer: params.timer,
        handler: params.handler,
        name: truncate_str(params.name, MO_LIMIT_FAILURE_KIND_BYTES as usize),
        read_only: params.read_only,
        arguments: params.arguments,
        signature_version: 1,
        class: params.class,
        reason,
        limit,
        limit_value,
        function: top_fn,
        file: top_file,
        start: top_start,
        end: top_end,
        work: MO_LIMIT_SERVICE_WORK,
        heap: params.limits.heap_size as u64,
        stack: params.limits.stack_size as u64,
        depth: u64::from(params.limits.call_depth),
        saved: MO_LIMIT_SAVED_BYTES,
        program: params.image_key,
        build: params.build_digest,
        label: params.build_label.to_string(),
        schema: params.schema_digest,
        context_status,
        context,
    };

    let payload = write_failure_record(&rec)?;
    Some((kind, payload))
}

fn truncate_str(s: &str, max_bytes: usize) -> String {
    if s.len() <= max_bytes {
        return s.to_string();
    }
    let mut cut = max_bytes;
    while cut > 0 && !s.is_char_boundary(cut) {
        cut -= 1;
    }
    s[..cut].to_string()
}
