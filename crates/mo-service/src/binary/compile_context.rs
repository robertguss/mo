//! Compile context (`MOCC\x01`) and compile metadata (`MOCS\x01`) binary codecs,
//! exact ports of `src/base/compile-context.c` and `src/base/compile-metadata.c`.

use mo_base::utf8::is_valid_utf8;
use mo_def::limits::{
    MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_BUNDLE_FILES, MO_LIMIT_BUNDLE_PATH_BYTES,
    MO_LIMIT_SERVICE_HANDLERS, MO_LIMIT_UPGRADE_MIGRATION_NAME,
};

use super::{read_u32_le, read_u64_le, sha256_bytes};

/// Binary magic for compile contexts (`MOCC\x01`).
pub const COMPILE_CONTEXT_MAGIC: &[u8; 5] = b"MOCC\x01";

/// Binary magic for compile metadata (`MOCS\x01`).
pub const COMPILE_METADATA_MAGIC: &[u8; 5] = b"MOCS\x01";

/// One synthesized `UpgradeOld.<Name>.mo` file stored inside `MOCC\x01` or `MOCS\x01`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileFile {
    /// Path of the form `UpgradeOld.<Ident>.mo`.
    pub path: String,
    /// Valid UTF-8 source text (no NUL bytes).
    pub source: Vec<u8>,
}

/// One tool handler signature entry inside `MOCS\x01`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileToolEntry {
    /// Tool name.
    pub name: String,
    /// 32-byte SHA-256 signature digest.
    pub digest: [u8; 32],
}

/// One timer handler entry inside `MOCS\x01`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileTimerEntry {
    /// Timer name (`"every <interval>"` or handler name).
    pub name: String,
    /// Interval in microseconds (`1..=i64::MAX`).
    pub interval: u64,
}

/// Decoded `MOCS\x01` compile metadata (`src/base/compile-metadata.h:mo_compile_metadata`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileMetadata {
    /// Service bearer token environment variable name.
    pub token: String,
    /// Raw canonical `MOSV\x01` state schema bytes.
    pub schema: Vec<u8>,
    /// Synthesized `UpgradeOld.*.mo` type files (sorted by path).
    pub types: Vec<CompileFile>,
    /// Tool signature descriptors (sorted by name).
    pub tools: Vec<CompileToolEntry>,
    /// Timer descriptors (sorted by name).
    pub timers: Vec<CompileTimerEntry>,
}

/// Decoded `MOCC\x01` compile context (`src/base/compile-context.h:mo_compile_context`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileContext {
    /// Migration function name (empty if none).
    pub migration: String,
    /// Synthesized `UpgradeOld.*.mo` files (sorted by path).
    pub files: Vec<CompileFile>,
    /// Optional embedded `MOCS\x01` metadata bytes.
    pub metadata: Option<Vec<u8>>,
    /// SHA-256 digest of the serialized `MOCC\x01` bytes.
    pub digest: [u8; 32],
}

fn read_text<'a>(bytes: &'a [u8], pos: &mut usize, max_len: usize) -> Option<&'a str> {
    let n = read_u32_le(bytes, pos)? as usize;
    if n > max_len {
        return None;
    }
    let slice = bytes.get(*pos..pos.checked_add(n)?)?;
    *pos += n;
    if slice.contains(&0) || !is_valid_utf8(slice) {
        return None;
    }
    std::str::from_utf8(slice).ok()
}

fn valid_upgrade_old_path(path: &str) -> bool {
    let b = path.as_bytes();
    if b.len() < 13
        || b.len() as u64 > MO_LIMIT_BUNDLE_PATH_BYTES
        || !b.starts_with(b"UpgradeOld.")
        || !b.ends_with(b".mo")
    {
        return false;
    }
    for (j, &c) in b.iter().enumerate() {
        let ok = c.is_ascii_alphanumeric() || c == b'_' || c == b'.';
        if !ok || (j > 0 && c == b'.' && b[j - 1] == b'.') {
            return false;
        }
    }
    true
}

fn valid_handler_name(name: &str) -> bool {
    !name.is_empty() && name.bytes().all(|b| b >= 32 && b != b'"' && b != b'\\')
}

fn read_compile_files(bytes: &[u8], pos: &mut usize, count: usize) -> Option<Vec<CompileFile>> {
    if count as u64 > MO_LIMIT_BUNDLE_FILES {
        return None;
    }
    let mut files = Vec::with_capacity(count);
    for i in 0..count {
        let path = read_text(bytes, pos, MO_LIMIT_BUNDLE_PATH_BYTES as usize)?;
        if !valid_upgrade_old_path(path) {
            return None;
        }
        let source = read_text(bytes, pos, MO_LIMIT_BUNDLE_BYTES as usize)?;
        if i > 0 {
            let prev: &CompileFile = &files[i - 1];
            if prev.path.as_bytes() >= path.as_bytes() {
                return None;
            }
        }
        files.push(CompileFile {
            path: path.to_string(),
            source: source.as_bytes().to_vec(),
        });
    }
    Some(files)
}

fn write_text(out: &mut Vec<u8>, bytes: &[u8]) -> Option<()> {
    if bytes.contains(&0) || !is_valid_utf8(bytes) {
        return None;
    }
    let len = u32::try_from(bytes.len()).ok()?;
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(bytes);
    Some(())
}

/// Encodes `MOCS\x01` compile metadata.
#[must_use]
pub fn write_compile_metadata(meta: &CompileMetadata) -> Option<Vec<u8>> {
    if meta.token.is_empty()
        || meta.schema.is_empty()
        || meta.types.len() as u64 > MO_LIMIT_BUNDLE_FILES
        || meta.tools.len() as u64 > MO_LIMIT_SERVICE_HANDLERS
        || meta.timers.len() as u64 > MO_LIMIT_SERVICE_HANDLERS
    {
        return None;
    }

    let mut types = meta.types.clone();
    types.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    for (i, f) in types.iter().enumerate() {
        if !valid_upgrade_old_path(&f.path) {
            return None;
        }
        if i > 0 && types[i - 1].path.as_bytes() >= f.path.as_bytes() {
            return None;
        }
    }

    let mut tools = meta.tools.clone();
    tools.sort_by(|a, b| a.name.as_bytes().cmp(b.name.as_bytes()));
    for (i, t) in tools.iter().enumerate() {
        if !valid_handler_name(&t.name) {
            return None;
        }
        if i > 0 && tools[i - 1].name.as_bytes() >= t.name.as_bytes() {
            return None;
        }
    }

    let mut timers = meta.timers.clone();
    timers.sort_by(|a, b| a.name.as_bytes().cmp(b.name.as_bytes()));
    for (i, t) in timers.iter().enumerate() {
        if !valid_handler_name(&t.name) || t.interval == 0 || t.interval > i64::MAX as u64 {
            return None;
        }
        if i > 0 && timers[i - 1].name.as_bytes() >= t.name.as_bytes() {
            return None;
        }
    }

    let mut out = Vec::new();
    out.extend_from_slice(COMPILE_METADATA_MAGIC);
    write_text(&mut out, meta.token.as_bytes())?;
    let schema_len = u32::try_from(meta.schema.len()).ok()?;
    out.extend_from_slice(&schema_len.to_le_bytes());
    out.extend_from_slice(&meta.schema);
    out.extend_from_slice(&(types.len() as u32).to_le_bytes());
    for f in &types {
        write_text(&mut out, f.path.as_bytes())?;
        write_text(&mut out, &f.source)?;
    }
    out.extend_from_slice(&(tools.len() as u32).to_le_bytes());
    for t in &tools {
        write_text(&mut out, t.name.as_bytes())?;
        out.extend_from_slice(&t.digest);
    }
    out.extend_from_slice(&(timers.len() as u32).to_le_bytes());
    for t in &timers {
        write_text(&mut out, t.name.as_bytes())?;
        out.extend_from_slice(&t.interval.to_le_bytes());
    }
    if out.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        return None;
    }
    Some(out)
}

/// Decodes and validates `MOCS\x01` compile metadata.
#[must_use]
pub fn read_compile_metadata(bytes: &[u8]) -> Option<CompileMetadata> {
    if bytes.len() < 25 || bytes.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        return None;
    }
    if &bytes[..5] != COMPILE_METADATA_MAGIC {
        return None;
    }
    let mut pos = 5usize;
    let token = read_text(bytes, &mut pos, MO_LIMIT_BUNDLE_BYTES as usize)?.to_string();
    if token.is_empty() {
        return None;
    }
    let schema_len = read_u32_le(bytes, &mut pos)? as usize;
    if schema_len == 0 {
        return None;
    }
    let schema = bytes.get(pos..pos.checked_add(schema_len)?)?.to_vec();
    pos += schema_len;

    let type_count = read_u32_le(bytes, &mut pos)? as usize;
    let types = read_compile_files(bytes, &mut pos, type_count)?;

    let tool_count = read_u32_le(bytes, &mut pos)? as usize;
    if tool_count as u64 > MO_LIMIT_SERVICE_HANDLERS {
        return None;
    }
    let mut tools = Vec::with_capacity(tool_count);
    for i in 0..tool_count {
        let name = read_text(bytes, &mut pos, MO_LIMIT_BUNDLE_BYTES as usize)?;
        if !valid_handler_name(name) {
            return None;
        }
        if i > 0 {
            let prev: &CompileToolEntry = &tools[i - 1];
            if prev.name.as_bytes() >= name.as_bytes() {
                return None;
            }
        }
        let dslice = bytes.get(pos..pos.checked_add(32)?)?;
        pos += 32;
        let mut digest = [0u8; 32];
        digest.copy_from_slice(dslice);
        tools.push(CompileToolEntry {
            name: name.to_string(),
            digest,
        });
    }

    let timer_count = read_u32_le(bytes, &mut pos)? as usize;
    if timer_count as u64 > MO_LIMIT_SERVICE_HANDLERS {
        return None;
    }
    let mut timers = Vec::with_capacity(timer_count);
    for i in 0..timer_count {
        let name = read_text(bytes, &mut pos, MO_LIMIT_BUNDLE_BYTES as usize)?;
        if !valid_handler_name(name) {
            return None;
        }
        if i > 0 {
            let prev: &CompileTimerEntry = &timers[i - 1];
            if prev.name.as_bytes() >= name.as_bytes() {
                return None;
            }
        }
        let interval = read_u64_le(bytes, &mut pos)?;
        if interval == 0 || interval > i64::MAX as u64 {
            return None;
        }
        timers.push(CompileTimerEntry {
            name: name.to_string(),
            interval,
        });
    }
    if pos != bytes.len() {
        return None;
    }
    Some(CompileMetadata {
        token,
        schema,
        types,
        tools,
        timers,
    })
}

/// Encodes a `MOCC\x01` compile context, returning `(bytes, sha256_digest)`.
#[must_use]
pub fn write_compile_context(
    migration: &str,
    files: &[CompileFile],
    metadata: Option<&[u8]>,
) -> Option<(Vec<u8>, [u8; 32])> {
    if migration.len() as u64 > MO_LIMIT_UPGRADE_MIGRATION_NAME
        || files.len() as u64 > MO_LIMIT_BUNDLE_FILES
        || (files.is_empty() && !migration.is_empty())
    {
        return None;
    }
    let mut sorted = files.to_vec();
    sorted.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    for (i, f) in sorted.iter().enumerate() {
        if !valid_upgrade_old_path(&f.path) {
            return None;
        }
        if i > 0 && sorted[i - 1].path.as_bytes() >= f.path.as_bytes() {
            return None;
        }
    }
    if let Some(meta) = metadata {
        read_compile_metadata(meta)?;
    }

    let mut out = Vec::new();
    out.extend_from_slice(COMPILE_CONTEXT_MAGIC);
    write_text(&mut out, migration.as_bytes())?;
    out.extend_from_slice(&(sorted.len() as u32).to_le_bytes());
    for f in &sorted {
        write_text(&mut out, f.path.as_bytes())?;
        write_text(&mut out, &f.source)?;
    }
    if let Some(meta) = metadata {
        let mlen = u32::try_from(meta.len()).ok()?;
        out.extend_from_slice(&mlen.to_le_bytes());
        out.extend_from_slice(meta);
    }
    if out.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        return None;
    }
    let digest = sha256_bytes(&out);
    Some((out, digest))
}

/// Validates and decodes a `MOCC\x01` compile context.
#[must_use]
pub fn read_compile_context(bytes: &[u8]) -> Option<CompileContext> {
    if bytes.len() < 13 || bytes.len() as u64 > MO_LIMIT_BUNDLE_BYTES {
        return None;
    }
    if &bytes[..5] != COMPILE_CONTEXT_MAGIC {
        return None;
    }
    let mut pos = 5usize;
    let migration =
        read_text(bytes, &mut pos, MO_LIMIT_UPGRADE_MIGRATION_NAME as usize)?.to_string();
    let count = read_u32_le(bytes, &mut pos)? as usize;
    let files = read_compile_files(bytes, &mut pos, count)?;
    let metadata = if pos != bytes.len() {
        let size = read_u32_le(bytes, &mut pos)? as usize;
        let slice = bytes.get(pos..pos.checked_add(size)?)?;
        pos += size;
        read_compile_metadata(slice)?;
        Some(slice.to_vec())
    } else {
        None
    };
    if pos != bytes.len() || (files.is_empty() && !migration.is_empty()) {
        return None;
    }
    let digest = sha256_bytes(bytes);
    Some(CompileContext {
        migration,
        files,
        metadata,
        digest,
    })
}
