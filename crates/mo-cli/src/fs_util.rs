//! Safe OS filesystem utilities: raw OS byte paths, nonblocking regular-file
//! checks, atomic sibling replacement (`src/tools/fix.c:mo_replace_file`),
//! atomic install writes (`src/tools/install.c:write_atomic_file`), and
//! bytecode output writes (`src/tools/execute.c:write_bytecode`).

use std::borrow::Cow;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::Path;
#[cfg(not(unix))]
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_HOST_PATH_SIZE, MO_LIMIT_PATH_SIZE};

#[cfg(unix)]
use std::os::unix::ffi::OsStrExt;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;

/// Converts raw OS path bytes into a [`Path`] without lossy UTF-8 replacement on Unix.
#[must_use]
pub fn bytes_to_path(bytes: &[u8]) -> Cow<'_, Path> {
    #[cfg(unix)]
    {
        Cow::Borrowed(Path::new(OsStr::from_bytes(bytes)))
    }
    #[cfg(not(unix))]
    {
        Cow::Owned(PathBuf::from(String::from_utf8_lossy(bytes).into_owned()))
    }
}

/// Extracts raw OS path bytes from a [`Path`].
#[must_use]
pub fn path_to_bytes(path: &Path) -> Vec<u8> {
    os_str_to_bytes(path.as_os_str())
}

/// Extracts raw OS bytes from an [`OsStr`].
#[must_use]
pub fn os_str_to_bytes(s: &OsStr) -> Vec<u8> {
    #[cfg(unix)]
    {
        s.as_bytes().to_vec()
    }
    #[cfg(not(unix))]
    {
        s.to_string_lossy().into_owned().into_bytes()
    }
}

/// Formats an OS error without Rust's trailing `" (os error N)"`, matching `strerror(errno)`.
#[must_use]
pub fn io_error_message(err: &io::Error) -> String {
    let text = err.to_string();
    if let Some(pos) = text.rfind(" (os error ")
        && text.ends_with(')')
    {
        return text[..pos].to_string();
    }
    text
}

/// Formats a `rustix::io::Errno` matching `strerror(errno)`.
#[cfg(unix)]
#[must_use]
pub fn errno_message(errno: rustix::io::Errno) -> String {
    io_error_message(&io::Error::from(errno))
}

/// Device, inode, and permission bits captured from `lstat` before modifying a file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FileIdentity {
    pub dev: u64,
    pub ino: u64,
    pub mode: u32,
}

/// Checks via `lstat` (never opening or blocking on a FIFO/device) that `path` is a
/// regular file (not a symlink or special file) and fits within `MO_LIMIT_PATH_SIZE`.
#[must_use]
pub fn lstat_regular(path: &[u8]) -> Option<FileIdentity> {
    if (path.len() as u64) > MO_LIMIT_PATH_SIZE {
        return None;
    }
    let meta = fs::symlink_metadata(bytes_to_path(path)).ok()?;
    if !meta.file_type().is_file() {
        return None;
    }
    #[cfg(unix)]
    {
        Some(FileIdentity {
            dev: meta.dev(),
            ino: meta.ino(),
            mode: meta.mode(),
        })
    }
    #[cfg(not(unix))]
    {
        Some(FileIdentity {
            dev: 0,
            ino: 0,
            mode: 0o644,
        })
    }
}

/// Returns `(dev, ino)` for an existing file following symlinks (`stat`).
#[must_use]
pub fn stat_identity(path: &[u8]) -> Option<(u64, u64)> {
    let meta = fs::metadata(bytes_to_path(path)).ok()?;
    #[cfg(unix)]
    {
        Some((meta.dev(), meta.ino()))
    }
    #[cfg(not(unix))]
    {
        let _ = meta;
        None
    }
}

/// Returns whether `path` refers to the same file (by `dev`/`ino`) as any source in `sources`.
#[must_use]
pub fn names_a_source(path: &[u8], sources: &SourceMap) -> bool {
    let Some((out_dev, out_ino)) = stat_identity(path) else {
        return false;
    };
    for i in 0..sources.len() {
        if let Some(src) = sources.get(i)
            && let Some((in_dev, in_ino)) = stat_identity(src.path_bytes())
            && out_dev == in_dev
            && out_ino == in_ino
        {
            return true;
        }
    }
    false
}

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

/// Atomically replaces `path` with `contents` via a sibling `.mo-write-XXXXXX` file,
/// preserving permission bits and refusing replacement if `path`'s `dev`/`ino` changed
/// (`src/tools/fix.c:mo_replace_file`).
#[cfg(unix)]
pub fn replace_file(path: &[u8], contents: &[u8], original: &FileIdentity) -> Result<(), String> {
    use rustix::fs::{Mode, OFlags, fchmod, fsync, open};
    use rustix::io::Errno;

    let dir_len = path
        .iter()
        .rposition(|&b| b == b'/')
        .map_or(0, |slash| slash + 1);
    let pid = std::process::id();

    let mut tmp_path = Vec::new();
    let mut fd_opt = None;
    for attempt in 0u64..128 {
        let seq = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
        let tag = (u64::from(pid)
            .wrapping_mul(2_654_435_761)
            .wrapping_add(seq.wrapping_mul(31))
            .wrapping_add(attempt))
            & 0xFF_FFFF;
        tmp_path.clear();
        tmp_path.extend_from_slice(&path[..dir_len]);
        tmp_path.extend_from_slice(format!(".mo-write-{tag:06x}").as_bytes());
        match open(
            bytes_to_path(&tmp_path).as_ref(),
            OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::CLOEXEC,
            Mode::from_raw_mode(0o600),
        ) {
            Ok(fd) => {
                fd_opt = Some(fd);
                break;
            }
            Err(Errno::EXIST) => continue,
            Err(err) => return Err(errno_message(err)),
        }
    }
    let Some(fd) = fd_opt else {
        return Err(errno_message(Errno::EXIST));
    };

    let write_res: Result<(), String> = (|| {
        let mut file = File::from(fd);
        file.write_all(contents).map_err(|e| io_error_message(&e))?;
        file.flush().map_err(|e| io_error_message(&e))?;
        fchmod(&file, Mode::from_raw_mode(original.mode & 0o777)).map_err(errno_message)?;
        fsync(&file).map_err(errno_message)?;
        drop(file);

        let current =
            fs::symlink_metadata(bytes_to_path(path)).map_err(|e| io_error_message(&e))?;
        if current.dev() != original.dev || current.ino() != original.ino {
            return Err(errno_message(Errno::BUSY));
        }
        fs::rename(bytes_to_path(&tmp_path), bytes_to_path(path))
            .map_err(|e| io_error_message(&e))?;
        Ok(())
    })();

    if let Err(err) = write_res {
        let _ = fs::remove_file(bytes_to_path(&tmp_path));
        return Err(err);
    }
    Ok(())
}

#[cfg(not(unix))]
pub fn replace_file(path: &[u8], contents: &[u8], _original: &FileIdentity) -> Result<(), String> {
    fs::write(bytes_to_path(path), contents).map_err(|e| io_error_message(&e))
}

/// Atomically writes `bytes` to `dest_path` inside `dir` with permissions `mode`,
/// refusing non-regular existing targets (`src/tools/install.c:write_atomic_file`).
#[cfg(unix)]
pub fn write_atomic_file(
    dir: &[u8],
    name: &str,
    dest_path: &[u8],
    bytes: &[u8],
    mode: u32,
) -> Result<(), String> {
    use rustix::fs::{Mode, OFlags, fchmod, open};
    use rustix::io::Errno;

    if let Ok(existing) = fs::symlink_metadata(bytes_to_path(dest_path))
        && !existing.file_type().is_file()
    {
        let err = if existing.file_type().is_dir() {
            Errno::ISDIR
        } else {
            Errno::EXIST
        };
        return Err(errno_message(err));
    }

    let pid = std::process::id();
    let mut tmp_path = Vec::new();
    let mut fd_opt = None;
    for _ in 0..64 {
        let seq = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed) + 1;
        tmp_path.clear();
        tmp_path.extend_from_slice(dir);
        tmp_path.extend_from_slice(format!("/.{name}.tmp.{pid}.{seq}").as_bytes());
        if (tmp_path.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE {
            return Err(errno_message(Errno::NAMETOOLONG));
        }
        match open(
            bytes_to_path(&tmp_path).as_ref(),
            OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::CLOEXEC,
            Mode::from_raw_mode(0o600),
        ) {
            Ok(fd) => {
                fd_opt = Some(fd);
                break;
            }
            Err(Errno::EXIST) => continue,
            Err(err) => return Err(errno_message(err)),
        }
    }
    let Some(fd) = fd_opt else {
        return Err(errno_message(Errno::EXIST));
    };

    let res: Result<(), String> = (|| {
        let mut file = File::from(fd);
        file.write_all(bytes).map_err(|e| io_error_message(&e))?;
        file.flush().map_err(|e| io_error_message(&e))?;
        fchmod(&file, Mode::from_raw_mode(mode)).map_err(errno_message)?;
        drop(file);
        fs::rename(bytes_to_path(&tmp_path), bytes_to_path(dest_path))
            .map_err(|e| io_error_message(&e))?;
        Ok(())
    })();

    if let Err(err) = res {
        let _ = fs::remove_file(bytes_to_path(&tmp_path));
        return Err(err);
    }
    Ok(())
}

#[cfg(not(unix))]
pub fn write_atomic_file(
    _dir: &[u8],
    _name: &str,
    dest_path: &[u8],
    bytes: &[u8],
    _mode: u32,
) -> Result<(), String> {
    fs::write(bytes_to_path(dest_path), bytes).map_err(|e| io_error_message(&e))
}

/// Writes compiled `.bc` bytes to `path`, refusing to overwrite any source file
/// and removing a partial regular file on write error while never deleting devices
/// such as `/dev/full` (`src/tools/execute.c:write_bytecode`).
pub fn write_bytecode_file(path: &[u8], sources: &SourceMap, wire: &[u8], diags: &mut DiagBag) {
    if names_a_source(path, sources) {
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                "bytecode can't replace a source file",
            )
            .with_primary_text(path),
        );
        return;
    }
    let path_os = bytes_to_path(path);
    let mut file = match File::create(path_os.as_ref()) {
        Ok(f) => f,
        Err(err) => {
            let _ = diags.push(
                Diagnostic::new(
                    ErrorCode::ToolPipeline,
                    Span::NONE,
                    "bytecode could not be written to the file",
                )
                .with_primary_text(io_error_message(&err)),
            );
            return;
        }
    };
    let regular = file.metadata().is_ok_and(|m| m.file_type().is_file());
    let write_err = file.write_all(wire).and_then(|()| file.flush()).err();
    drop(file);
    if let Some(err) = write_err {
        if regular {
            let _ = fs::remove_file(path_os.as_ref());
        }
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                "bytecode could not be written to the file",
            )
            .with_primary_text(io_error_message(&err)),
        );
    }
}
