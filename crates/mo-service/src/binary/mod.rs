//! Binary codecs and formatting helpers shared across durable storage, exports,
//! failure diagnostics, source bundles, compile contexts, and upload staging.

pub mod bundle;
pub mod compile_context;
pub mod export;
pub mod failure;
pub mod log_record;
pub mod upload;

use std::fmt::Write as _;

use sha2::{Digest, Sha256};

/// Computes the lowercase 64-character hex string of `bytes`.
#[must_use]
pub fn hex_encode(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        let _ = write!(out, "{b:02x}");
    }
    out
}

/// Decodes a lowercase/uppercase hex string of exact byte length `N`.
#[must_use]
pub fn hex_decode_exact<const N: usize>(hex: &str) -> Option<[u8; N]> {
    if hex.len() != N * 2 {
        return None;
    }
    let mut out = [0u8; N];
    let bytes = hex.as_bytes();
    for i in 0..N {
        let hi = hex_nibble(bytes[i * 2])?;
        let lo = hex_nibble(bytes[i * 2 + 1])?;
        out[i] = (hi << 4) | lo;
    }
    Some(out)
}

fn hex_nibble(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

/// Computes SHA-256 over a single byte slice.
#[must_use]
pub fn sha256_bytes(data: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(data);
    let digest = hasher.finalize();
    let mut out = [0u8; 32];
    out.copy_from_slice(&digest);
    out
}

/// Computes SHA-256 over a sequence of chunks.
#[must_use]
pub fn sha256_chunks(chunks: &[&[u8]]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    for c in chunks {
        hasher.update(c);
    }
    let digest = hasher.finalize();
    let mut out = [0u8; 32];
    out.copy_from_slice(&digest);
    out
}

/// Appends a JSON-escaped string (with enclosing quotes) to `out`.
pub fn push_json_string(out: &mut String, s: &str) {
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Sanitizes untrusted text for terminal output, matching `src/tools/render.c:mo_render_terminal_text`.
#[must_use]
pub fn sanitize_terminal_text(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        let code = ch as u32;
        if ch == '\n' {
            out.push('\n');
        } else if ch == '\t' {
            out.push('\t');
        } else if code < 0x20 || code == 0x7f {
            let _ = write!(out, "\\x{code:02x}");
        } else if mo_def::is_invisible_scalar(code) {
            if code <= 0xfff {
                let _ = write!(out, "\\u{{{code:03x}}}");
            } else if code <= 0xffff {
                let _ = write!(out, "\\u{{{code:04x}}}");
            } else if code <= 0x000f_ffff {
                let _ = write!(out, "\\u{{{code:05x}}}");
            } else {
                let _ = write!(out, "\\u{{{code:06x}}}");
            }
        } else {
            out.push(ch);
        }
    }
    out
}

/// Reads a little-endian `u32` at `*pos` and advances `*pos` by 4.
pub fn read_u32_le(bytes: &[u8], pos: &mut usize) -> Option<u32> {
    let end = pos.checked_add(4)?;
    let s = bytes.get(*pos..end)?;
    *pos = end;
    Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// Reads a little-endian `u64` at `*pos` and advances `*pos` by 8.
pub fn read_u64_le(bytes: &[u8], pos: &mut usize) -> Option<u64> {
    let end = pos.checked_add(8)?;
    let s = bytes.get(*pos..end)?;
    *pos = end;
    Some(u64::from_le_bytes([
        s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
    ]))
}

/// Reads a little-endian `i64` at `*pos` and advances `*pos` by 8.
pub fn read_i64_le(bytes: &[u8], pos: &mut usize) -> Option<i64> {
    read_u64_le(bytes, pos).map(|v| v as i64)
}

/// Reads a little-endian `u16` at `*pos` and advances `*pos` by 2.
pub fn read_u16_le(bytes: &[u8], pos: &mut usize) -> Option<u16> {
    let end = pos.checked_add(2)?;
    let s = bytes.get(*pos..end)?;
    *pos = end;
    Some(u16::from_le_bytes([s[0], s[1]]))
}
