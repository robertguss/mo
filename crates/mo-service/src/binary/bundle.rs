//! Canonical source bundle codec (`MOSB\x01`), exact port of `src/base/bundle.c`.

use mo_base::utf8::is_valid_utf8;
use mo_def::limits::{MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_BUNDLE_FILES, MO_LIMIT_BUNDLE_PATH_BYTES};

use super::{read_u32_le, sha256_bytes};

/// Binary magic header for canonical source bundles.
pub const BUNDLE_MAGIC: &[u8; 5] = b"MOSB\x01";

/// One entry in a canonical source bundle.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BundleFile {
    /// Normalized relative POSIX path (e.g. `main.mo`, `sub/helper.mo`).
    pub path: String,
    /// Valid UTF-8 file contents.
    pub content: Vec<u8>,
}

/// Decoded canonical source bundle.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceBundle {
    /// Entry module path (must match one of `files`).
    pub entry: String,
    /// Sorted unique bundle files.
    pub files: Vec<BundleFile>,
    /// Canonical SHA-256 digest of the serialized bundle bytes.
    pub digest: [u8; 32],
}

/// Checks whether `path` is a valid normalized relative bundle path
/// (matching `src/base/bundle.c:bundle_path`).
#[must_use]
pub fn valid_bundle_path(path: &str) -> bool {
    let bytes = path.as_bytes();
    if bytes.is_empty() || bytes.len() as u64 > MO_LIMIT_BUNDLE_PATH_BYTES {
        return false;
    }
    let mut start = 0usize;
    for i in 0..=bytes.len() {
        if i < bytes.len() && (bytes[i] == 0 || bytes[i] == b'\\') {
            return false;
        }
        if i < bytes.len() && bytes[i] != b'/' {
            continue;
        }
        let part = &bytes[start..i];
        if part.is_empty() || part == b"." || part == b".." {
            return false;
        }
        if start == 0 && (part == b"lib" || part == b"UpgradeOld" || part == b"UpgradeOld.mo") {
            return false;
        }
        start = i + 1;
    }
    true
}

/// Canonicalizes (sorts by raw path bytes), validates, and encodes a `MOSB\x01` bundle.
///
/// Returns `(encoded_bytes, sha256_digest)` on success, or `None` if limits or invariants fail.
#[must_use]
pub fn write_bundle(entry: &str, files: &[BundleFile]) -> Option<(Vec<u8>, [u8; 32])> {
    if !valid_bundle_path(entry) || files.is_empty() || files.len() as u64 > MO_LIMIT_BUNDLE_FILES {
        return None;
    }
    let mut order: Vec<usize> = (0..files.len()).collect();
    order.sort_by(|&a, &b| files[a].path.as_bytes().cmp(files[b].path.as_bytes()));

    let mut has_entry = false;
    for (idx, &file_idx) in order.iter().enumerate() {
        let f = &files[file_idx];
        if !valid_bundle_path(&f.path) || !is_valid_utf8(&f.content) {
            return None;
        }
        if idx > 0 && files[order[idx - 1]].path.as_bytes() >= f.path.as_bytes() {
            return None;
        }
        if f.path == entry {
            has_entry = true;
        }
    }
    if !has_entry {
        return None;
    }

    let mut out = Vec::new();
    out.extend_from_slice(BUNDLE_MAGIC);
    let entry_len = u32::try_from(entry.len()).ok()?;
    out.extend_from_slice(&entry_len.to_le_bytes());
    out.extend_from_slice(entry.as_bytes());
    let count = u32::try_from(order.len()).ok()?;
    out.extend_from_slice(&count.to_le_bytes());

    for &file_idx in &order {
        let f = &files[file_idx];
        let plen = u32::try_from(f.path.len()).ok()?;
        let clen = u32::try_from(f.content.len()).ok()?;
        let add = 8usize
            .checked_add(f.path.len())?
            .checked_add(f.content.len())?;
        if (out.len().checked_add(add)?) as u64 > MO_LIMIT_BUNDLE_BYTES {
            return None;
        }
        out.extend_from_slice(&plen.to_le_bytes());
        out.extend_from_slice(f.path.as_bytes());
        out.extend_from_slice(&clen.to_le_bytes());
        out.extend_from_slice(&f.content);
    }
    if out.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        return None;
    }
    let digest = sha256_bytes(&out);
    Some((out, digest))
}

/// Validates and decodes a `MOSB\x01` bundle from raw bytes.
#[must_use]
pub fn read_bundle(bytes: &[u8]) -> Option<SourceBundle> {
    if bytes.len() < 13 || bytes.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        return None;
    }
    if &bytes[..BUNDLE_MAGIC.len()] != BUNDLE_MAGIC {
        return None;
    }
    let mut pos = BUNDLE_MAGIC.len();
    let entry_len = read_u32_le(bytes, &mut pos)? as usize;
    if entry_len == 0 || entry_len as u64 > MO_LIMIT_BUNDLE_PATH_BYTES {
        return None;
    }
    let entry_slice = bytes.get(pos..pos.checked_add(entry_len)?)?;
    pos += entry_len;
    let entry = std::str::from_utf8(entry_slice).ok()?;
    if !valid_bundle_path(entry) {
        return None;
    }
    let count = read_u32_le(bytes, &mut pos)? as usize;
    if count == 0 || count as u64 > MO_LIMIT_BUNDLE_FILES {
        return None;
    }

    let mut files = Vec::with_capacity(count);
    let mut has_entry = false;
    for idx in 0..count {
        let plen = read_u32_le(bytes, &mut pos)? as usize;
        if plen == 0 || plen as u64 > MO_LIMIT_BUNDLE_PATH_BYTES {
            return None;
        }
        let pslice = bytes.get(pos..pos.checked_add(plen)?)?;
        pos += plen;
        let path = std::str::from_utf8(pslice).ok()?;
        if !valid_bundle_path(path) {
            return None;
        }
        if idx > 0 {
            let prev: &BundleFile = &files[idx - 1];
            if prev.path.as_bytes() >= path.as_bytes() {
                return None;
            }
        }
        let clen = read_u32_le(bytes, &mut pos)? as usize;
        if clen as u64 > MO_LIMIT_BUNDLE_BYTES {
            return None;
        }
        let cslice = bytes.get(pos..pos.checked_add(clen)?)?;
        pos += clen;
        if !is_valid_utf8(cslice) {
            return None;
        }
        if path == entry {
            has_entry = true;
        }
        files.push(BundleFile {
            path: path.to_string(),
            content: cslice.to_vec(),
        });
    }
    if pos != bytes.len() || !has_entry {
        return None;
    }
    let digest = sha256_bytes(bytes);
    Some(SourceBundle {
        entry: entry.to_string(),
        files,
        digest,
    })
}

/// Validates a `MOSB\x01` bundle and returns its SHA-256 digest without retaining copies.
#[must_use]
pub fn validate_bundle(bytes: &[u8]) -> Option<[u8; 32]> {
    read_bundle(bytes).map(|b| b.digest)
}
