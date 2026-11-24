//! CapCase module name to kebab-case file path conversion (`src/base/path.c` parity).

use std::fmt;
use std::path::Path;

use mo_def::limits::MO_LIMIT_PATH_SIZE;
use thiserror::Error;

/// Validated relative `.mo` file path derived from a Mo module name.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ModulePath(String);

impl ModulePath {
    #[must_use]
    pub fn as_str(&self) -> &str {
        &self.0
    }

    #[must_use]
    pub fn into_string(self) -> String {
        self.0
    }
}

impl AsRef<str> for ModulePath {
    fn as_ref(&self) -> &str {
        &self.0
    }
}

impl AsRef<Path> for ModulePath {
    fn as_ref(&self) -> &Path {
        Path::new(&self.0)
    }
}

impl fmt::Display for ModulePath {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// Errors produced when converting a module name to a file path.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum ModulePathError {
    #[error("module name is empty")]
    Empty,
    #[error("module name contains an invalid segment or character")]
    InvalidSegment,
    #[error("module path exceeds MO_LIMIT_PATH_SIZE")]
    TooLong,
}

/// Converts a dotted CapCase Mo module name (e.g. `HttpServer.UTF8`) to its
/// relative kebab-case file path (e.g. `http-server/utf8.mo`).
///
/// Matches `src/base/path.c:mo_module_path` and `tests/module_paths.tsv` exactly.
pub fn module_path(name: &[u8]) -> Result<ModulePath, ModulePathError> {
    let limit = MO_LIMIT_PATH_SIZE as usize;
    if name.is_empty() {
        return Err(ModulePathError::Empty);
    }
    if name.len() > limit {
        return Err(ModulePathError::TooLong);
    }

    let mut out = String::with_capacity(name.len().saturating_add(16).min(limit));
    let mut start = true;
    for (i, &c) in name.iter().enumerate() {
        if c == b'.' {
            if start || i + 1 == name.len() {
                return Err(ModulePathError::InvalidSegment);
            }
            if out.len() == limit {
                return Err(ModulePathError::TooLong);
            }
            out.push('/');
            start = true;
            continue;
        }
        let is_cap = c.is_ascii_uppercase();
        let is_low = c.is_ascii_lowercase();
        let is_dig = c.is_ascii_digit();
        if (start && !is_cap) || (!is_cap && !is_low && !is_dig) {
            return Err(ModulePathError::InvalidSegment);
        }
        let word = !start
            && is_cap
            && (name[i - 1].is_ascii_lowercase()
                || name[i - 1].is_ascii_digit()
                || (i + 1 < name.len() && name[i + 1].is_ascii_lowercase()));
        if word {
            if out.len() == limit {
                return Err(ModulePathError::TooLong);
            }
            out.push('-');
        }
        if out.len() == limit {
            return Err(ModulePathError::TooLong);
        }
        let mapped = if is_cap { c + (b'a' - b'A') } else { c };
        out.push(char::from(mapped));
        start = false;
    }

    if out.len() > limit - 3 {
        return Err(ModulePathError::TooLong);
    }
    out.push_str(".mo");
    Ok(ModulePath(out))
}
