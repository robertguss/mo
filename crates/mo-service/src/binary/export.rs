//! Immutable export artifact codec (`MOEX\x01`), exact port of `src/base/export.c`.

use mo_base::utf8::is_valid_utf8;
use mo_bytecode::{FNV_OFFSET, fnv1a};
use mo_def::limits::{
    MO_EXPORT_FIXED_BYTES, MO_LIMIT_EXPORT_BYTES, MO_LIMIT_EXPORT_IDLE_US,
    MO_LIMIT_EXPORT_LABEL_BYTES, MO_LIMIT_EXPORT_PAGE_BYTES, MO_LIMIT_EXPORT_PINS,
    MO_LIMIT_FAILURE_MESSAGE_BYTES, MO_LIMIT_SAVED_BYTES,
};

use super::failure::{FailureRecord, read_failure_record};
use super::{read_u32_le, read_u64_le, sha256_bytes};

/// Binary magic header for export artifacts.
pub const EXPORT_MAGIC: &[u8; 5] = b"MOEX\x01";

/// Maximum bytes in a single export artifact (`MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS`).
pub const MAX_SINGLE_EXPORT_BYTES: usize = (MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS) as usize;

/// Export identity header (`src/base/export.h:mo_export_identity`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExportIdentity {
    /// Export kind (`1` = live state snapshot, `2` = captured failure pair).
    pub kind: u8,
    /// Random 16-byte export identifier.
    pub id: [u8; 16],
    /// 32-byte build runtime identity digest.
    pub build: [u8; 32],
    /// Build label string.
    pub label: String,
    /// Service index within the bytecode file.
    pub service: u32,
    /// Service program checksum (`BytecodeFile::program_checksum`).
    pub program: u64,
    /// Logical service sequence number.
    pub sequence: u64,
    /// Sequence number at which state was encoded.
    pub encoded_sequence: u64,
    /// Captured failure record id (`0` for state exports).
    pub failure_id: u64,
    /// Wall clock creation timestamp in microseconds since epoch.
    pub created: i64,
}

/// One pinned export artifact held in memory for paged download (`mo_export_pin`).
#[derive(Debug, Clone)]
pub struct ExportPin {
    /// Serialized `MOEX\x01` artifact bytes.
    pub bytes: Vec<u8>,
    /// 16-byte export identifier.
    pub id: [u8; 16],
    /// Trailing 32-byte SHA-256 digest of `bytes[..len-32]`.
    pub digest: [u8; 32],
    /// Sequence number.
    pub sequence: u64,
    /// Encoded sequence number.
    pub encoded_sequence: u64,
    /// Last monotonic access timestamp in microseconds.
    pub touched: u64,
    /// Failure record id (`0` for state exports).
    pub failure_id: u64,
}

impl ExportPin {
    /// Returns `true` if this pin has been idle for at least `MO_LIMIT_EXPORT_IDLE_US`.
    #[must_use]
    pub fn is_expired(&self, now_us: u64) -> bool {
        now_us >= self.touched && now_us - self.touched >= MO_LIMIT_EXPORT_IDLE_US
    }
}

/// Decoded `MOEX\x01` export artifact (`src/base/export.h:mo_export_artifact`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExportArtifact<'a> {
    /// Identity header.
    pub identity: ExportIdentity,
    /// 32-byte state schema SHA-256 digest.
    pub schema: [u8; 32],
    /// Saved (`MOSV\x01`) state bytes.
    pub state: &'a [u8],
    /// Failure message payload bytes (empty for kind 1).
    pub message: &'a [u8],
    /// Decoded failure record (`Some` for kind 2, `None` for kind 1).
    pub failure: Option<FailureRecord>,
}

/// One paged export chunk (`src/base/export.h:mo_export_page`).
#[derive(Debug, Clone)]
pub struct ExportPage {
    /// 16-byte export identifier.
    pub id: [u8; 16],
    /// Final 32-byte SHA-256 digest (checked when `is_final` is true).
    pub digest: [u8; 32],
    /// Sequence number.
    pub sequence: u64,
    /// Encoded sequence number.
    pub encoded_sequence: u64,
    /// FNV-1a checksum of `bytes`.
    pub checksum: u64,
    /// Byte offset in the full artifact.
    pub offset: usize,
    /// Total artifact byte length.
    pub total: usize,
    /// Total page count.
    pub count: usize,
    /// Whether this is the final page.
    pub is_final: bool,
    /// Page payload bytes.
    pub bytes: Vec<u8>,
}

/// Incremental reassembler and verifier for paged `MOEX\x01` downloads (`mo_export_consumer`).
#[derive(Debug, Default)]
pub struct ExportConsumer {
    /// Reassembled bytes once started.
    pub bytes: Vec<u8>,
    /// Export identifier.
    pub id: [u8; 16],
    /// Sequence number.
    pub sequence: u64,
    /// Encoded sequence number.
    pub encoded_sequence: u64,
    /// Failure record id (`0` for kind 1).
    pub failure_id: u64,
    /// Artifact kind (`1` or `2`).
    pub kind: u8,
    /// Expected total byte length.
    pub total: usize,
    /// `true` once all pages have arrived and the full artifact verified.
    pub complete: bool,
}

/// Validates `MOSV\x01` framing and returns the SHA-256 digest of its schema bytes
/// (`src/base/export.c:mo_export_schema`).
#[must_use]
pub fn export_schema_digest(saved: &[u8]) -> Option<[u8; 32]> {
    if saved.len() < 9 || saved.len() as u64 > MO_LIMIT_SAVED_BYTES || &saved[..5] != b"MOSV\x01" {
        return None;
    }
    let mut pos = 5usize;
    let size = read_u32_le(saved, &mut pos)? as usize;
    if size == 0 {
        return None;
    }
    let schema = saved.get(pos..pos.checked_add(size)?)?;
    Some(sha256_bytes(schema))
}

/// Computes `(page_length, total_page_count)` for an export of `total` bytes at `offset`.
#[must_use]
pub fn export_page_bounds(total: usize, offset: usize) -> Option<(usize, usize)> {
    let page_sz = MO_LIMIT_EXPORT_PAGE_BYTES as usize;
    if total == 0
        || total > MAX_SINGLE_EXPORT_BYTES
        || offset >= total
        || !offset.is_multiple_of(page_sz)
    {
        return None;
    }
    let remaining = total - offset;
    let length = remaining.min(page_sz);
    let count = 1 + (total - 1) / page_sz;
    Some((length, count))
}

fn read_failure_frame<'a>(
    bytes: &'a [u8],
    pos: &mut usize,
    expected_kind: u8,
) -> Option<(&'a [u8], u64, u64)> {
    let length = read_u32_le(bytes, pos)? as usize;
    if length < 34 || length as u64 > MO_LIMIT_SAVED_BYTES + 34 {
        return None;
    }
    let frame = bytes.get(*pos..pos.checked_add(length)?)?;
    *pos += length;
    if &frame[..5] != b"MOST\x01" || frame[5] != expected_kind {
        return None;
    }
    let mut fpos = 6usize;
    let sequence = read_u64_le(frame, &mut fpos)?;
    let program = read_u64_le(frame, &mut fpos)?;
    let payload_len = read_u32_le(frame, &mut fpos)? as usize;
    let payload = frame.get(fpos..fpos.checked_add(payload_len)?)?;
    fpos += payload_len;
    let checksum = read_u64_le(frame, &mut fpos)?;
    if fpos != frame.len() || checksum != fnv1a(FNV_OFFSET, &frame[..frame.len() - 8]) {
        return None;
    }
    Some((payload, sequence, program))
}

fn write_most_frame(out: &mut Vec<u8>, kind: u8, sequence: u64, image: u64, payload: &[u8]) {
    let frame_len = (payload.len() + 34) as u32;
    out.extend_from_slice(&frame_len.to_le_bytes());
    let start = out.len();
    out.extend_from_slice(b"MOST\x01");
    out.push(kind);
    out.extend_from_slice(&sequence.to_le_bytes());
    out.extend_from_slice(&image.to_le_bytes());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(payload);
    let checksum = fnv1a(FNV_OFFSET, &out[start..]);
    out.extend_from_slice(&checksum.to_le_bytes());
}

fn build_pin(
    id: &ExportIdentity,
    saved: &[u8],
    message: Option<&[u8]>,
    now_us: u64,
) -> Option<ExportPin> {
    if (id.kind != 1 && id.kind != 2)
        || id.label.len() as u64 > MO_LIMIT_EXPORT_LABEL_BYTES
        || !is_valid_utf8(id.label.as_bytes())
    {
        return None;
    }
    let schema = match message {
        None => {
            if id.kind == 2 {
                let mut pos = 0usize;
                let (st, seq1, prog1) = read_failure_frame(saved, &mut pos, 3)?;
                let (msg, seq2, prog2) = read_failure_frame(saved, &mut pos, 4)?;
                if pos != saved.len() || seq1 != seq2 || prog1 != prog2 {
                    return None;
                }
                let rec = read_failure_record(msg)?;
                let sch = export_schema_digest(st)?;
                if rec.sequence != seq1 || rec.program != prog1 || rec.schema != sch {
                    return None;
                }
                sch
            } else {
                export_schema_digest(saved)?
            }
        }
        Some(msg) => {
            if id.kind != 2 || msg.len() as u64 > MO_LIMIT_FAILURE_MESSAGE_BYTES {
                return None;
            }
            export_schema_digest(saved)?
        }
    };
    let body_length = saved
        .len()
        .checked_add(message.map_or(0, |m| m.len() + 76))?;
    let total = (MO_EXPORT_FIXED_BYTES as usize)
        .checked_add(id.label.len())?
        .checked_add(body_length)?;
    if total > MAX_SINGLE_EXPORT_BYTES {
        return None;
    }

    let mut bytes = Vec::with_capacity(total);
    bytes.extend_from_slice(EXPORT_MAGIC);
    bytes.push(id.kind);
    bytes.extend_from_slice(&id.id);
    bytes.extend_from_slice(&id.program.to_le_bytes());
    bytes.extend_from_slice(&id.build);
    bytes.extend_from_slice(&(id.label.len() as u32).to_le_bytes());
    bytes.extend_from_slice(id.label.as_bytes());
    bytes.extend_from_slice(&id.service.to_le_bytes());
    bytes.extend_from_slice(&id.sequence.to_le_bytes());
    bytes.extend_from_slice(&id.encoded_sequence.to_le_bytes());
    bytes.extend_from_slice(&(id.created as u64).to_le_bytes());
    bytes.extend_from_slice(&schema);
    bytes.extend_from_slice(&(body_length as u32).to_le_bytes());
    match message {
        None => bytes.extend_from_slice(saved),
        Some(msg) => {
            let image_key = id.program ^ (u64::from(id.service) << 32);
            write_most_frame(&mut bytes, 3, id.sequence, image_key, saved);
            write_most_frame(&mut bytes, 4, id.sequence, image_key, msg);
        }
    }
    let digest = sha256_bytes(&bytes);
    bytes.extend_from_slice(&digest);
    debug_assert_eq!(bytes.len(), total);

    Some(ExportPin {
        bytes,
        id: id.id,
        digest,
        sequence: id.sequence,
        encoded_sequence: id.encoded_sequence,
        touched: now_us,
        failure_id: id.failure_id,
    })
}

/// Creates a kind-1 (state) or pre-framed kind-2 export pin (`mo_export_create`).
#[must_use]
pub fn create_export_pin(id: &ExportIdentity, saved: &[u8], now_us: u64) -> Option<ExportPin> {
    build_pin(id, saved, None, now_us)
}

/// Creates a kind-2 (failure pair) export pin from state and failure message payloads
/// (`mo_export_create_failure`).
#[must_use]
pub fn create_failure_export_pin(
    id: &ExportIdentity,
    state: &[u8],
    message: &[u8],
    now_us: u64,
) -> Option<ExportPin> {
    let rec = read_failure_record(message)?;
    let expected_prog = id.program ^ (u64::from(id.service) << 32);
    if rec.sequence != id.sequence || rec.program != expected_prog || rec.build != id.build {
        return None;
    }
    let mut pin = build_pin(id, state, Some(message), now_us)?;
    pin.failure_id = rec.id;
    Some(pin)
}

/// Decodes and fully verifies a `MOEX\x01` export artifact (`mo_export_read`).
#[must_use]
pub fn read_export(bytes: &[u8]) -> Option<ExportArtifact<'_>> {
    if bytes.len() < MO_EXPORT_FIXED_BYTES as usize
        || bytes.len() > MAX_SINGLE_EXPORT_BYTES
        || &bytes[..5] != EXPORT_MAGIC
        || (bytes[5] != 1 && bytes[5] != 2)
    {
        return None;
    }
    let body_end = bytes.len() - 32;
    let actual = sha256_bytes(&bytes[..body_end]);
    if actual != bytes[body_end..] {
        return None;
    }

    let kind = bytes[5];
    let mut id_bytes = [0u8; 16];
    id_bytes.copy_from_slice(&bytes[6..22]);
    let mut pos = 22usize;
    let program = read_u64_le(bytes, &mut pos)?;
    let mut build = [0u8; 32];
    build.copy_from_slice(&bytes[30..62]);
    pos = 62;
    let label_len = read_u32_le(bytes, &mut pos)? as usize;
    if label_len as u64 > MO_LIMIT_EXPORT_LABEL_BYTES {
        return None;
    }
    let label_slice = bytes.get(pos..pos.checked_add(label_len)?)?;
    pos += label_len;
    let label = std::str::from_utf8(label_slice).ok()?.to_string();
    let service = read_u32_le(bytes, &mut pos)?;
    let sequence = read_u64_le(bytes, &mut pos)?;
    let encoded_sequence = read_u64_le(bytes, &mut pos)?;
    let created = read_u64_le(bytes, &mut pos)? as i64;
    let schema_slice = bytes.get(pos..pos.checked_add(32)?)?;
    pos += 32;
    let mut schema = [0u8; 32];
    schema.copy_from_slice(schema_slice);
    let size = read_u32_le(bytes, &mut pos)? as usize;
    let saved = bytes.get(pos..pos.checked_add(size)?)?;
    pos += size;
    if pos != body_end {
        return None;
    }

    let (state, message, failure, failure_id) = if kind == 2 {
        let mut bpos = 0usize;
        let (st, seq1, prog1) = read_failure_frame(saved, &mut bpos, 3)?;
        let (msg, seq2, prog2) = read_failure_frame(saved, &mut bpos, 4)?;
        if bpos != saved.len() || seq1 != seq2 || prog1 != prog2 {
            return None;
        }
        let rec = read_failure_record(msg)?;
        let computed_schema = export_schema_digest(st)?;
        let expected_prog = program ^ (u64::from(service) << 32);
        if rec.sequence != seq1
            || rec.program != prog1
            || computed_schema != rec.schema
            || computed_schema != schema
            || rec.program != expected_prog
            || rec.sequence != sequence
            || encoded_sequence != sequence
            || rec.build != build
        {
            return None;
        }
        let fid = rec.id;
        (st, msg, Some(rec), fid)
    } else {
        let computed_schema = export_schema_digest(saved)?;
        if computed_schema != schema {
            return None;
        }
        (saved, &[][..], None, 0)
    };

    Some(ExportArtifact {
        identity: ExportIdentity {
            kind,
            id: id_bytes,
            build,
            label,
            service,
            program,
            sequence,
            encoded_sequence,
            failure_id,
            created,
        },
        schema,
        state,
        message,
        failure,
    })
}

impl ExportConsumer {
    /// Accepts and verifies one sequential export page (`mo_export_accept`).
    #[must_use]
    pub fn accept(&mut self, page: &ExportPage) -> bool {
        let Some((expected_len, expected_count)) = export_page_bounds(page.total, page.offset)
        else {
            return false;
        };
        if self.complete
            || page.total < MO_EXPORT_FIXED_BYTES as usize
            || page.bytes.len() != expected_len
            || page.count != expected_count
            || page.is_final != (expected_len == page.total - page.offset)
            || fnv1a(FNV_OFFSET, &page.bytes) != page.checksum
        {
            return false;
        }
        if self.bytes.is_empty() && self.total == 0 {
            if page.offset != 0 {
                return false;
            }
            self.bytes = Vec::with_capacity(page.total.min(262_144));
            self.id = page.id;
            self.sequence = page.sequence;
            self.encoded_sequence = page.encoded_sequence;
            self.total = page.total;
        } else if page.offset != self.bytes.len()
            || page.total != self.total
            || page.sequence != self.sequence
            || page.encoded_sequence != self.encoded_sequence
            || page.id != self.id
        {
            return false;
        }
        self.bytes.extend_from_slice(&page.bytes);
        if page.is_final {
            if self.bytes.len() != self.total || self.bytes[self.total - 32..] != page.digest {
                return false;
            }
            let Some(art) = read_export(&self.bytes) else {
                return false;
            };
            if art.identity.id != self.id
                || art.identity.sequence != self.sequence
                || art.identity.encoded_sequence != self.encoded_sequence
            {
                return false;
            }
            self.kind = art.identity.kind;
            self.failure_id = art.identity.failure_id;
            self.complete = true;
        }
        true
    }
}
