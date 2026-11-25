//! Service write-ahead log record payload codec (`src/base/log-record.c` parity).
//!
//! Layout inside its `MOST\x01` frame:
//! `timer (1)` | `handler (4)` | `arrival (8)` | `count (4)` | `state_checksum (8)` |
//! `count * (len (4) | arg_bytes)`.

use mo_def::limits::{MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_SAVED_BYTES};

use super::{read_i64_le, read_u32_le, read_u64_le};

/// Fixed byte length of a log record header (`1 + 4 + 8 + 4 + 8 = 25`).
pub const LOG_RECORD_HEADER_BYTES: usize = 25;

/// Maximum arguments per tool call in a log record (`MO_LIMIT_BYTECODE_PARAMETERS - 2`).
pub const LOG_RECORD_MAX_ARGUMENTS: usize = (MO_LIMIT_BYTECODE_PARAMETERS - 2) as usize;

/// Parsed log record header (`src/base/log-record.h:mo_log_record_header`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LogRecordHeader {
    /// FNV-1a checksum of the saved state produced by this message.
    pub state_checksum: u64,
    /// Arrival timestamp in microseconds since Unix epoch.
    pub arrival: i64,
    /// Tool or timer index.
    pub handler: u32,
    /// Number of encoded arguments following the header.
    pub count: u32,
    /// `true` for a timer tick, `false` for a tool call.
    pub timer: bool,
}

/// Decoded log record payload (borrowing argument byte slices).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogRecord<'a> {
    /// Header fields.
    pub header: LogRecordHeader,
    /// Saved (`MOSV\x01`) encoded argument slices.
    pub arguments: Vec<&'a [u8]>,
}

/// Log record metadata without retaining argument bytes (`mo_log_record_metadata`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogRecordMetadata {
    /// Header fields.
    pub header: LogRecordHeader,
    /// Byte length of each argument slice.
    pub argument_bytes: Vec<u32>,
}

/// Encodes a log record header and argument slices.
#[must_use]
pub fn write_log_record(header: &LogRecordHeader, arguments: &[&[u8]]) -> Option<Vec<u8>> {
    if arguments.len() != header.count as usize || arguments.len() > LOG_RECORD_MAX_ARGUMENTS {
        return None;
    }
    let mut total = LOG_RECORD_HEADER_BYTES;
    for arg in arguments {
        total = total.checked_add(4)?.checked_add(arg.len())?;
    }
    if total as u64 > MO_LIMIT_SAVED_BYTES {
        return None;
    }
    let mut out = Vec::with_capacity(total);
    out.push(u8::from(header.timer));
    out.extend_from_slice(&header.handler.to_le_bytes());
    out.extend_from_slice(&header.arrival.to_le_bytes());
    out.extend_from_slice(&header.count.to_le_bytes());
    out.extend_from_slice(&header.state_checksum.to_le_bytes());
    for arg in arguments {
        let len = u32::try_from(arg.len()).ok()?;
        out.extend_from_slice(&len.to_le_bytes());
        out.extend_from_slice(arg);
    }
    Some(out)
}

/// Reads a [`LogRecordHeader`] from `bytes` starting at `*pos`.
#[must_use]
pub fn read_log_record_header(bytes: &[u8], pos: &mut usize) -> Option<LogRecordHeader> {
    let start = *pos;
    let timer_byte = *bytes.get(*pos)?;
    if timer_byte > 1 {
        return None;
    }
    *pos += 1;
    let Some(handler) = read_u32_le(bytes, pos) else {
        *pos = start;
        return None;
    };
    let Some(arrival) = read_i64_le(bytes, pos) else {
        *pos = start;
        return None;
    };
    let Some(count) = read_u32_le(bytes, pos) else {
        *pos = start;
        return None;
    };
    let Some(state_checksum) = read_u64_le(bytes, pos) else {
        *pos = start;
        return None;
    };
    Some(LogRecordHeader {
        state_checksum,
        arrival,
        handler,
        count,
        timer: timer_byte != 0,
    })
}

/// Decodes a complete log record payload from `bytes`.
#[must_use]
pub fn read_log_record(bytes: &[u8]) -> Option<LogRecord<'_>> {
    if bytes.len() < LOG_RECORD_HEADER_BYTES || bytes.len() as u64 > MO_LIMIT_SAVED_BYTES {
        return None;
    }
    let mut pos = 0usize;
    let header = read_log_record_header(bytes, &mut pos)?;
    if header.count as usize > LOG_RECORD_MAX_ARGUMENTS {
        return None;
    }
    let mut arguments = Vec::with_capacity(header.count as usize);
    for _ in 0..header.count {
        let len = read_u32_le(bytes, &mut pos)? as usize;
        let slice = bytes.get(pos..pos.checked_add(len)?)?;
        pos += len;
        arguments.push(slice);
    }
    if pos != bytes.len() {
        return None;
    }
    Some(LogRecord { header, arguments })
}

/// Walks a log record payload of `length` bytes using a streaming `read_at(offset, buf)` callback
/// without buffering argument bytes (`src/base/log-record.c:mo_log_record_walk`).
#[must_use]
pub fn walk_log_record<F>(length: usize, mut read_at: F) -> Option<LogRecordMetadata>
where
    F: FnMut(usize, &mut [u8]) -> bool,
{
    if length < LOG_RECORD_HEADER_BYTES {
        return None;
    }
    let mut hbuf = [0u8; LOG_RECORD_HEADER_BYTES];
    if !read_at(0, &mut hbuf) {
        return None;
    }
    let mut hpos = 0usize;
    let header = read_log_record_header(&hbuf, &mut hpos)?;
    if header.count as usize > LOG_RECORD_MAX_ARGUMENTS {
        return None;
    }
    let mut argument_bytes = Vec::with_capacity(header.count as usize);
    let mut offset = LOG_RECORD_HEADER_BYTES;
    for _ in 0..header.count {
        if length - offset < 4 {
            return None;
        }
        let mut prefix = [0u8; 4];
        if !read_at(offset, &mut prefix) {
            return None;
        }
        offset += 4;
        let n = u32::from_le_bytes(prefix);
        if (n as usize) > length - offset {
            return None;
        }
        offset += n as usize;
        argument_bytes.push(n);
    }
    if offset != length {
        return None;
    }
    Some(LogRecordMetadata {
        header,
        argument_bytes,
    })
}
