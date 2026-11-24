//! Embedded standard library sources generated from `lib/**/*.mo` at build time,
//! matching `ci/embed_library.py` and `src/tools/upgrade-bundle.c`.

use std::path::{Path, PathBuf};

/// One embedded `.mo` source file inside the standard library (`struct mo_embedded_source`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EmbeddedSource {
    /// Relative path from `lib/` using `/` separators (e.g. `"list.mo"`, `"arvo/cookie.mo"`).
    pub path: &'static str,
    /// Raw UTF-8 source bytes.
    pub bytes: &'static [u8],
}

/// Collection of embedded standard library sources (`struct mo_embedded_library`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EmbeddedLibrary {
    pub files: &'static [EmbeddedSource],
}

impl EmbeddedLibrary {
    #[must_use]
    pub const fn new(files: &'static [EmbeddedSource]) -> Self {
        Self { files }
    }

    #[must_use]
    pub fn find(&self, rel_path: &str) -> Option<&'static EmbeddedSource> {
        self.files.iter().find(|src| src.path == rel_path)
    }
}

include!(concat!(env!("OUT_DIR"), "/embedded_library.rs"));

/// Built-in embedded standard library discovered from `lib/**/*.mo` at compile time.
pub const DEFAULT_EMBEDDED_LIBRARY: EmbeddedLibrary = EmbeddedLibrary::new(EMBEDDED_LIBRARY_FILES);

/// Returns the default compiled-in standard library (`mo_default_embedded_library`).
#[must_use]
pub const fn default_embedded_library() -> &'static EmbeddedLibrary {
    &DEFAULT_EMBEDDED_LIBRARY
}

/// Returns the build's on-disk `lib/` root if it still exists, matching
/// `src/tools/upgrade-bundle.c:mo_default_library_root`.
///
/// When checking an entry file located inside `lib/`, this allows the loader to
/// recognize it as library module 0 and skip loading its embedded copy a second time.
#[must_use]
pub fn default_library_root() -> Option<PathBuf> {
    let path = Path::new(DEFAULT_LIBRARY_ROOT);
    if path.exists() {
        Some(path.to_path_buf())
    } else {
        None
    }
}
