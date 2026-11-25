//! Failure record message payload codec (v1), exact port of `src/base/failure.c`.

use mo_base::utf8::is_valid_utf8;
use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_EXPORT_LABEL_BYTES, MO_LIMIT_FAILURE_CONTEXT_BYTES,
    MO_LIMIT_FAILURE_KIND_BYTES, MO_LIMIT_FAILURE_MESSAGE_BYTES, MO_TIME_MAX_US, MO_TIME_MIN_US,
};

use super::{read_u32_le, read_u64_le};

/// Current failure record format version (`MO_FAILURE_VERSION`).
pub const FAILURE_VERSION: u8 = 1;
/// Byte offset of the 8-byte failure id within a failure message payload.
pub const FAILURE_ID_OFFSET: usize = 1;
/// Byte length of the failure id field.
pub const FAILURE_ID_BYTES: usize = 8;

/// Failure context completeness status (`enum mo_failure_context`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FailureContextStatus {
    /// Context was captured in full.
    Complete = 0,
    /// Context was truncated to `MO_LIMIT_FAILURE_CONTEXT_BYTES`.
    Truncated = 1,
    /// Context was unavailable (e.g. out of memory or verification error).
    Unavailable = 2,
}

impl FailureContextStatus {
    /// Parses the 1-byte context status tag.
    #[must_use]
    pub const fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Self::Complete),
            1 => Some(Self::Truncated),
            2 => Some(Self::Unavailable),
            _ => None,
        }
    }
}

/// Complete decoded failure record message payload (`src/base/failure.h:mo_failure_record`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FailureRecord {
    /// Store-assigned failure record id (`>= 1` once stored; `0` before assignment).
    pub id: u64,
    /// Service sequence number at which the failure occurred.
    pub sequence: u64,
    /// Arrival timestamp (microseconds since epoch, stored as two's complement `u64`).
    pub arrival: i64,
    /// `true` if timer tick, `false` if tool call.
    pub timer: bool,
    /// Handler or timer index.
    pub handler: u32,
    /// Tool or timer name.
    pub name: String,
    /// Whether the tool was read-only (`false` for timers).
    pub read_only: bool,
    /// Saved (`MOSV\x01`) encoded arguments.
    pub arguments: Vec<Vec<u8>>,
    /// Signature format version (always `1`).
    pub signature_version: u8,
    /// Failure class (`0..=3`: defect/limit/bug/save).
    pub class: u8,
    /// Failure reason string.
    pub reason: String,
    /// Limit name (empty if none).
    pub limit: String,
    /// Numeric limit value.
    pub limit_value: u64,
    /// Top frame function index (or `NO_INDEX`).
    pub function: u32,
    /// Top frame file index (or `NO_INDEX`).
    pub file: u32,
    /// Top frame span start byte offset.
    pub start: u32,
    /// Top frame span end byte offset.
    pub end: u32,
    /// Configured work limit.
    pub work: u64,
    /// Configured heap limit.
    pub heap: u64,
    /// Configured stack limit.
    pub stack: u64,
    /// Configured call depth limit.
    pub depth: u64,
    /// Configured saved bytes limit.
    pub saved: u64,
    /// Service image key (`program_checksum ^ ((service as u64) << 32)`).
    pub program: u64,
    /// 32-byte build runtime identity digest.
    pub build: [u8; 32],
    /// Build label string.
    pub label: String,
    /// 32-byte state schema SHA-256 digest.
    pub schema: [u8; 32],
    /// Context capture status.
    pub context_status: FailureContextStatus,
    /// Captured formatted context string.
    pub context: String,
}

fn read_text<'a>(bytes: &'a [u8], pos: &mut usize, max_len: usize) -> Option<&'a str> {
    let len = read_u32_le(bytes, pos)? as usize;
    if len > max_len {
        return None;
    }
    let slice = bytes.get(*pos..pos.checked_add(len)?)?;
    *pos += len;
    if slice.contains(&0) || !is_valid_utf8(slice) {
        return None;
    }
    std::str::from_utf8(slice).ok()
}

fn read_argument<'a>(bytes: &'a [u8], pos: &mut usize) -> Option<&'a [u8]> {
    let len = read_u32_le(bytes, pos)? as usize;
    if len < 9 || len as u64 > MO_LIMIT_FAILURE_MESSAGE_BYTES {
        return None;
    }
    let slice = bytes.get(*pos..pos.checked_add(len)?)?;
    *pos += len;
    if &slice[..5] != b"MOSV\x01" {
        return None;
    }
    Some(slice)
}

fn write_text(out: &mut Vec<u8>, s: &str, max_len: usize) -> Option<()> {
    let b = s.as_bytes();
    if b.len() > max_len || b.contains(&0) {
        return None;
    }
    let len = u32::try_from(b.len()).ok()?;
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(b);
    Some(())
}

/// Encodes a [`FailureRecord`] into its exact binary v1 payload.
///
/// Note: `rec.id` may be `0` before the store assigns and patches the id at `FAILURE_ID_OFFSET`.
#[must_use]
pub fn write_failure_record(rec: &FailureRecord) -> Option<Vec<u8>> {
    if rec.sequence > i64::MAX as u64
        || !(MO_TIME_MIN_US..=MO_TIME_MAX_US).contains(&rec.arrival)
        || rec.name.is_empty()
        || (rec.timer && (rec.read_only || !rec.arguments.is_empty()))
        || rec.arguments.len() as u64 > MO_LIMIT_BYTECODE_PARAMETERS - 2
        || rec.signature_version != 1
        || rec.class > 3
        || rec.start > rec.end
    {
        return None;
    }
    let mut out = Vec::with_capacity(512);
    out.push(FAILURE_VERSION);
    out.extend_from_slice(&rec.id.to_le_bytes());
    out.extend_from_slice(&rec.sequence.to_le_bytes());
    out.extend_from_slice(&(rec.arrival as u64).to_le_bytes());
    out.push(u8::from(rec.timer));
    out.extend_from_slice(&rec.handler.to_le_bytes());
    write_text(&mut out, &rec.name, MO_LIMIT_FAILURE_KIND_BYTES as usize)?;
    out.push(u8::from(rec.read_only));
    out.extend_from_slice(&(rec.arguments.len() as u32).to_le_bytes());
    for arg in &rec.arguments {
        if arg.len() < 9 || &arg[..5] != b"MOSV\x01" {
            return None;
        }
        let len = u32::try_from(arg.len()).ok()?;
        out.extend_from_slice(&len.to_le_bytes());
        out.extend_from_slice(arg);
    }
    out.push(rec.signature_version);
    out.push(rec.class);
    write_text(&mut out, &rec.reason, MO_LIMIT_FAILURE_KIND_BYTES as usize)?;
    write_text(&mut out, &rec.limit, MO_LIMIT_FAILURE_KIND_BYTES as usize)?;
    out.extend_from_slice(&rec.limit_value.to_le_bytes());
    out.extend_from_slice(&rec.function.to_le_bytes());
    out.extend_from_slice(&rec.file.to_le_bytes());
    out.extend_from_slice(&rec.start.to_le_bytes());
    out.extend_from_slice(&rec.end.to_le_bytes());
    out.extend_from_slice(&rec.work.to_le_bytes());
    out.extend_from_slice(&rec.heap.to_le_bytes());
    out.extend_from_slice(&rec.stack.to_le_bytes());
    out.extend_from_slice(&rec.depth.to_le_bytes());
    out.extend_from_slice(&rec.saved.to_le_bytes());
    out.extend_from_slice(&rec.program.to_le_bytes());
    out.extend_from_slice(&rec.build);
    write_text(&mut out, &rec.label, MO_LIMIT_EXPORT_LABEL_BYTES as usize)?;
    out.extend_from_slice(&rec.schema);
    out.push(rec.context_status as u8);
    write_text(
        &mut out,
        &rec.context,
        MO_LIMIT_FAILURE_CONTEXT_BYTES as usize,
    )?;
    if out.len() as u64 > MO_LIMIT_FAILURE_MESSAGE_BYTES {
        return None;
    }
    Some(out)
}

/// Decodes and strictly validates a stored binary v1 [`FailureRecord`] payload (`id >= 1`).
#[must_use]
pub fn read_failure_record(bytes: &[u8]) -> Option<FailureRecord> {
    if bytes.is_empty()
        || bytes.len() as u64 > MO_LIMIT_FAILURE_MESSAGE_BYTES
        || bytes[0] != FAILURE_VERSION
    {
        return None;
    }
    let mut pos = FAILURE_ID_OFFSET;
    let id = read_u64_le(bytes, &mut pos)?;
    if id == 0 || id > i64::MAX as u64 {
        return None;
    }
    let sequence = read_u64_le(bytes, &mut pos)?;
    if sequence > i64::MAX as u64 {
        return None;
    }
    let arrival = read_u64_le(bytes, &mut pos)? as i64;
    let timer_byte = *bytes.get(pos)?;
    pos += 1;
    if timer_byte > 1 {
        return None;
    }
    let timer = timer_byte != 0;
    let handler = read_u32_le(bytes, &mut pos)?;
    let name = read_text(bytes, &mut pos, MO_LIMIT_FAILURE_KIND_BYTES as usize)?.to_string();
    if name.is_empty() {
        return None;
    }
    let readonly_byte = *bytes.get(pos)?;
    pos += 1;
    if readonly_byte > 1 || (timer && readonly_byte != 0) {
        return None;
    }
    let read_only = readonly_byte != 0;
    let count = read_u32_le(bytes, &mut pos)? as usize;
    if count as u64 > MO_LIMIT_BYTECODE_PARAMETERS - 2 || (timer && count != 0) {
        return None;
    }
    if !(MO_TIME_MIN_US..=MO_TIME_MAX_US).contains(&arrival) {
        return None;
    }
    let mut arguments = Vec::with_capacity(count);
    for _ in 0..count {
        arguments.push(read_argument(bytes, &mut pos)?.to_vec());
    }

    let signature_version = *bytes.get(pos)?;
    pos += 1;
    if signature_version != 1 {
        return None;
    }
    let class = *bytes.get(pos)?;
    pos += 1;
    if class > 3 {
        return None;
    }
    let reason = read_text(bytes, &mut pos, MO_LIMIT_FAILURE_KIND_BYTES as usize)?.to_string();
    let limit = read_text(bytes, &mut pos, MO_LIMIT_FAILURE_KIND_BYTES as usize)?.to_string();
    let limit_value = read_u64_le(bytes, &mut pos)?;
    let function = read_u32_le(bytes, &mut pos)?;
    let file = read_u32_le(bytes, &mut pos)?;
    let start = read_u32_le(bytes, &mut pos)?;
    let end = read_u32_le(bytes, &mut pos)?;
    if start > end {
        return None;
    }
    let work = read_u64_le(bytes, &mut pos)?;
    let heap = read_u64_le(bytes, &mut pos)?;
    let stack = read_u64_le(bytes, &mut pos)?;
    let depth = read_u64_le(bytes, &mut pos)?;
    let saved = read_u64_le(bytes, &mut pos)?;
    let program = read_u64_le(bytes, &mut pos)?;
    let build_slice = bytes.get(pos..pos.checked_add(32)?)?;
    pos += 32;
    let mut build = [0u8; 32];
    build.copy_from_slice(build_slice);
    let label = read_text(bytes, &mut pos, MO_LIMIT_EXPORT_LABEL_BYTES as usize)?.to_string();
    let schema_slice = bytes.get(pos..pos.checked_add(32)?)?;
    pos += 32;
    let mut schema = [0u8; 32];
    schema.copy_from_slice(schema_slice);
    let status_byte = *bytes.get(pos)?;
    pos += 1;
    let context_status = FailureContextStatus::from_u8(status_byte)?;
    let context = read_text(bytes, &mut pos, MO_LIMIT_FAILURE_CONTEXT_BYTES as usize)?.to_string();
    if pos != bytes.len() {
        return None;
    }
    Some(FailureRecord {
        id,
        sequence,
        arrival,
        timer,
        handler,
        name,
        read_only,
        arguments,
        signature_version,
        class,
        reason,
        limit,
        limit_value,
        function,
        file,
        start,
        end,
        work,
        heap,
        stack,
        depth,
        saved,
        program,
        build,
        label,
        schema,
        context_status,
        context,
    })
}
