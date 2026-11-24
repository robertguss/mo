//! Source file table, byte spans, and UTF-8 scalar line/column lookup
//! (`src/base/diag.c` parity).

use std::borrow::Cow;
use std::fs::{self, File};
use std::io::Read;
use std::path::Path;

use mo_def::limits::{
    MO_LIMIT_FILE_SIZE, MO_LIMIT_FILES, MO_LIMIT_PATH_SIZE, MO_LIMIT_PROGRAM_SIZE,
};
use thiserror::Error;

use crate::utf8::decode_scalar;

/// Sentinel file index indicating no source file is attached.
pub const NO_FILE: u32 = u32::MAX;

/// Half-open byte range `[start, end)` in source file `file`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Span {
    pub file: u32,
    pub start: u32,
    pub end: u32,
}

impl Span {
    pub const NONE: Self = Self {
        file: NO_FILE,
        start: 0,
        end: 0,
    };

    #[must_use]
    pub const fn new(file: u32, start: u32, end: u32) -> Self {
        Self { file, start, end }
    }

    #[must_use]
    pub const fn is_ordered(self) -> bool {
        self.start <= self.end
    }

    #[must_use]
    pub const fn len(self) -> u32 {
        self.end.saturating_sub(self.start)
    }

    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.start == self.end
    }
}

/// 1-based line and UTF-8 scalar column in a source file.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct LineCol {
    pub line: u32,
    pub column: u32,
}

/// Errors returned when adding files to a [`SourceMap`].
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum SourceError {
    #[error("too many source files (limit: {MO_LIMIT_FILES})")]
    TooManyFiles,
    #[error("source path too long (limit: {MO_LIMIT_PATH_SIZE} bytes)")]
    PathTooLong,
    #[error("source file too large (limit: {MO_LIMIT_FILE_SIZE} bytes)")]
    FileTooBig,
    #[error("program source exceeds total size limit ({MO_LIMIT_PROGRAM_SIZE} bytes)")]
    ProgramTooBig,
    #[error("source file is unreadable: {0}")]
    Unreadable(String),
}

/// One loaded source file with precomputed 0-based byte offsets of each line start.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceFile {
    path: Vec<u8>,
    bytes: Vec<u8>,
    line_starts: Vec<u32>,
}

impl SourceFile {
    #[must_use]
    pub fn path_bytes(&self) -> &[u8] {
        &self.path
    }

    #[must_use]
    pub fn path_lossy(&self) -> Cow<'_, str> {
        String::from_utf8_lossy(&self.path)
    }

    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    #[must_use]
    pub fn len(&self) -> u32 {
        self.bytes.len() as u32
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    #[must_use]
    pub fn line_count(&self) -> u32 {
        self.line_starts.len() as u32
    }

    #[must_use]
    pub fn line_starts(&self) -> &[u32] {
        &self.line_starts
    }
}

/// Collection of source files indexed by `u32` file IDs.
#[derive(Debug, Default, Clone)]
pub struct SourceMap {
    files: Vec<SourceFile>,
    total_bytes: u64,
}

impl SourceMap {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            files: Vec::new(),
            total_bytes: 0,
        }
    }

    #[must_use]
    pub fn len(&self) -> u32 {
        self.files.len() as u32
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.files.is_empty()
    }

    #[must_use]
    pub const fn total_bytes(&self) -> u64 {
        self.total_bytes
    }

    #[must_use]
    pub fn get(&self, file: u32) -> Option<&SourceFile> {
        self.files.get(file as usize)
    }

    fn check_room(&self, path_bytes: &[u8]) -> Result<(), SourceError> {
        if (self.files.len() as u64) >= MO_LIMIT_FILES {
            return Err(SourceError::TooManyFiles);
        }
        if (path_bytes.len() as u64) > MO_LIMIT_PATH_SIZE {
            return Err(SourceError::PathTooLong);
        }
        Ok(())
    }

    /// Registers an in-memory source file with arbitrary path bytes.
    pub fn add_bytes(
        &mut self,
        path: impl Into<Vec<u8>>,
        bytes: impl Into<Vec<u8>>,
    ) -> Result<u32, SourceError> {
        let path = path.into();
        self.check_room(&path)?;
        let bytes = bytes.into();
        let len = bytes.len() as u64;
        if len > MO_LIMIT_FILE_SIZE {
            return Err(SourceError::FileTooBig);
        }
        if len > MO_LIMIT_PROGRAM_SIZE.saturating_sub(self.total_bytes) {
            return Err(SourceError::ProgramTooBig);
        }

        let line_starts = find_line_starts(&bytes);
        let id = self.files.len() as u32;
        self.files.push(SourceFile {
            path,
            bytes,
            line_starts,
        });
        self.total_bytes += len;
        Ok(id)
    }

    /// Reads a regular file from disk (refusing directories, FIFOs, devices, and
    /// symlinks to special files without blocking) and registers it.
    pub fn add_file(&mut self, path: &Path) -> Result<u32, SourceError> {
        let path_bytes = path_to_bytes(path);
        self.check_room(&path_bytes)?;

        // Check metadata before opening so FIFOs/pipes cannot block `File::open`.
        let pre_meta = fs::metadata(path).map_err(|e| SourceError::Unreadable(e.to_string()))?;
        if !pre_meta.is_file() {
            return Err(SourceError::Unreadable("not a regular file".to_string()));
        }

        let file = File::open(path).map_err(|e| SourceError::Unreadable(e.to_string()))?;
        let open_meta = file
            .metadata()
            .map_err(|e| SourceError::Unreadable(e.to_string()))?;
        if !open_meta.is_file() {
            return Err(SourceError::Unreadable("not a regular file".to_string()));
        }

        let mut buf = Vec::new();
        let mut reader = file.take(MO_LIMIT_FILE_SIZE + 1);
        reader
            .read_to_end(&mut buf)
            .map_err(|e| SourceError::Unreadable(e.to_string()))?;
        if (buf.len() as u64) > MO_LIMIT_FILE_SIZE {
            return Err(SourceError::FileTooBig);
        }

        self.add_bytes(path_bytes, buf)
    }

    /// Returns whether `span` points to a valid in-bounds range in this source map.
    #[must_use]
    pub fn is_available(&self, span: Span) -> bool {
        let Some(source) = self.get(span.file) else {
            return false;
        };
        span.start <= span.end && span.end <= source.len()
    }

    /// Converts byte offset `offset` in `file` to 1-based `(line, column)`.
    ///
    /// Matches `src/base/diag.c:mo_sources_position` exactly:
    /// - `\n` starts a new line.
    /// - The `\r` in `\r\n` does not advance the column (shares the `\n` column).
    /// - Valid UTF-8 scalars count as 1 column; offsets inside a scalar take that scalar's column.
    /// - Invalid UTF-8 bytes count as 1 column each.
    #[must_use]
    pub fn position(&self, file: u32, offset: u32) -> Option<LineCol> {
        let source = self.get(file)?;
        if offset > source.len() {
            return None;
        }
        let idx = line_index(&source.line_starts, offset);
        let mut column = 1u32;
        let mut at = source.line_starts[idx] as usize;
        let target = offset as usize;
        let bytes = &source.bytes;
        while at < target {
            let (_, size) = decode_scalar(&bytes[at..]);
            if size > target - at {
                break;
            }
            let is_crlf = bytes[at] == b'\r' && at + 1 < bytes.len() && bytes[at + 1] == b'\n';
            if !is_crlf {
                column += 1;
            }
            at += size;
        }
        Some(LineCol {
            line: (idx as u32) + 1,
            column,
        })
    }

    /// Returns the byte span of 1-based `line` in `file`, excluding its trailing
    /// `\n` or `\r\n` (a trailing bare `\r` on the final line is preserved).
    ///
    /// Matches `src/base/diag.c:mo_sources_line` exactly.
    #[must_use]
    pub fn line_span(&self, file: u32, line: u32) -> Option<Span> {
        let source = self.get(file)?;
        if line < 1 || line > source.line_count() {
            return None;
        }
        let idx = (line - 1) as usize;
        let start = source.line_starts[idx];
        if line == source.line_count() {
            return Some(Span::new(file, start, source.len()));
        }
        let mut end = source.line_starts[idx + 1] - 1;
        if end > start && source.bytes[(end - 1) as usize] == b'\r' {
            end -= 1;
        }
        Some(Span::new(file, start, end))
    }
}

fn find_line_starts(bytes: &[u8]) -> Vec<u32> {
    let mut starts = vec![0u32];
    for (i, &b) in bytes.iter().enumerate() {
        if b == b'\n' {
            starts.push((i + 1) as u32);
        }
    }
    starts
}

fn line_index(line_starts: &[u32], offset: u32) -> usize {
    let mut low = 0usize;
    let mut high = line_starts.len();
    while high - low > 1 {
        let mid = low + (high - low) / 2;
        if line_starts[mid] <= offset {
            low = mid;
        } else {
            high = mid;
        }
    }
    low
}

#[cfg(unix)]
fn path_to_bytes(path: &Path) -> Vec<u8> {
    use std::os::unix::ffi::OsStrExt;
    path.as_os_str().as_bytes().to_vec()
}

#[cfg(not(unix))]
fn path_to_bytes(path: &Path) -> Vec<u8> {
    path.to_string_lossy().as_bytes().to_vec()
}
