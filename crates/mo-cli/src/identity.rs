//! Runtime identity, embedded docs, and executable self-path resolution
//! (`src/base/runtime-identity.c` & `src/tools/cli.c` parity).

use std::env;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use mo_def::limits::MO_LIMIT_PATH_SIZE;
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::fs_util::path_to_bytes;

include!(concat!(env!("OUT_DIR"), "/embedded_docs.rs"));

/// Tool version string (`mo_def::MO_VERSION`).
pub const MO_VERSION: &str = mo_def::MO_VERSION;

/// Formatted `--version` output line without trailing newline.
pub const MO_VERSION_LINE: &str = mo_def::MO_VERSION_BANNER;

/// Returns the version string (`"0.4.2"`).
#[must_use]
pub const fn version() -> &'static str {
    MO_VERSION
}

/// Returns the version banner (`"mo 0.4.2"`).
#[must_use]
pub const fn version_line() -> &'static str {
    MO_VERSION_LINE
}

/// Returns the repository `lib/` root recorded at build time if it still exists as a directory.
#[must_use]
pub fn default_library_root() -> Option<PathBuf> {
    let candidate = Path::new(DEFAULT_BUILD_LIBRARY_ROOT);
    if candidate.is_dir() {
        return Some(candidate.to_path_buf());
    }
    mo_check::default_library_root()
}

/// Error produced when computing the executable's SHA-256 runtime identity digest.
#[derive(Debug, Clone, Error)]
pub enum IdentityError {
    #[error("could not locate the running mo executable")]
    ExecutableNotFound,
    #[error("could not read the running mo executable: {0}")]
    ReadFailed(String),
}

/// Resolves the canonical path to the currently running `mo` executable
/// (`src/base/runtime-identity.c:mo_runtime_executable_path`).
///
/// Returns `None` if the executable cannot be resolved, is not an executable
/// regular file, or exceeds `MO_LIMIT_PATH_SIZE - 1` bytes.
#[must_use]
pub fn executable_path() -> Option<Vec<u8>> {
    let raw = env::current_exe().ok()?;
    let resolved = fs::canonicalize(&raw).ok()?;
    let meta = fs::metadata(&resolved).ok()?;
    if !meta.is_file() {
        return None;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if meta.permissions().mode() & 0o111 == 0 {
            return None;
        }
    }
    let bytes = path_to_bytes(&resolved);
    if bytes.is_empty() || (bytes.len() as u64) >= MO_LIMIT_PATH_SIZE {
        return None;
    }
    Some(bytes)
}

static RUNTIME_DIGEST: OnceLock<Result<[u8; 32], IdentityError>> = OnceLock::new();

/// Computes and caches the SHA-256 digest of the running `mo` executable
/// (`src/base/runtime-identity.c:mo_runtime_identity_digest`).
pub fn runtime_identity_digest() -> Result<[u8; 32], IdentityError> {
    RUNTIME_DIGEST
        .get_or_init(|| {
            let exe = env::current_exe().map_err(|_| IdentityError::ExecutableNotFound)?;
            let resolved = fs::canonicalize(&exe).unwrap_or(exe);
            let mut file =
                File::open(&resolved).map_err(|e| IdentityError::ReadFailed(e.to_string()))?;
            let mut hasher = Sha256::new();
            let mut buf = [0u8; 16 * 1024];
            loop {
                let n = file
                    .read(&mut buf)
                    .map_err(|e| IdentityError::ReadFailed(e.to_string()))?;
                if n == 0 {
                    break;
                }
                hasher.update(&buf[..n]);
            }
            let out: [u8; 32] = hasher.finalize().into();
            Ok(out)
        })
        .clone()
}

/// Formats the cached SHA-256 runtime identity digest as 64 lowercase hex characters.
pub fn runtime_identity_hex() -> Result<String, IdentityError> {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let digest = runtime_identity_digest()?;
    let mut out = String::with_capacity(64);
    for b in digest {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    Ok(out)
}
