use std::cell::Cell;
use std::fs;
use std::io::{ErrorKind, Read};
#[cfg(unix)]
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
#[cfg(unix)]
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::rc::Rc;
use std::time::{Duration, Instant};

use mo_bytecode::Capability;
use mo_bytecode::types::TypeTable;
use mo_def::HostOpId;
use mo_def::limits::{
    MO_LIMIT_COMMAND_ARG_BYTES, MO_LIMIT_COMMAND_ARGS, MO_LIMIT_COMMAND_DURATION_US,
    MO_LIMIT_COMMAND_MEMORY, MO_LIMIT_COMMAND_OUTPUT_BYTES, MO_LIMIT_HOST_PATH_SIZE,
    MO_LIMIT_OPEN_FOLDERS, MO_LIMIT_VALUE_BYTES,
};
use mo_heap::{Heap, ScratchGuard, Value};
#[cfg(unix)]
use rustix::fs::{Access, AtFlags, CWD, FileType, OFlags, access, fcntl_setfl, fstat, statat};
#[cfg(unix)]
use rustix::io::fcntl_dupfd_cloexec;

use crate::HostError;
use crate::files_common::{FileStatus, find_case_tag, join_path, normalize_path};
#[cfg(unix)]
use crate::real_files::{PathMode, ResolverState, dir_fd_path, resolve};

#[cfg(unix)]
struct RealCommandCap {
    fd: OwnedFd,
    pending: bool,
    forbidden: bool,
    prefix: Option<String>,
    allowed: Option<Vec<String>>,
    open_folders: Rc<Cell<u32>>,
}

#[cfg(unix)]
impl Drop for RealCommandCap {
    fn drop(&mut self) {
        let cur = self.open_folders.get();
        debug_assert!(cur != 0);
        self.open_folders.set(cur.saturating_sub(1));
    }
}

struct FakeCommandCap {
    replies: Value, // Map(Bytes, Bytes)
    prefix: Option<String>,
    forbidden: bool,
    allowed: Option<Vec<String>>,
}

enum CommandCap {
    #[cfg(unix)]
    Real(RealCommandCap),
    Fake(FakeCommandCap),
}

// ---------------------------------------------------------------------------
// Validation & result builders (exact port of src/host/command.c)
// ---------------------------------------------------------------------------

fn valid_program_name(bytes: &[u8]) -> bool {
    if bytes.is_empty() || bytes.len() > 255 {
        return false;
    }
    if bytes == b"." || bytes == b".." {
        return false;
    }
    for &ch in bytes {
        if ch <= 0x20 || ch >= 0x7f || ch == b'/' || ch == b'\\' {
            return false;
        }
    }
    true
}

fn program_allowed(allowed: &Option<Vec<String>>, prog: &str) -> bool {
    if !valid_program_name(prog.as_bytes()) {
        return false;
    }
    match allowed {
        None => true,
        Some(list) => list.iter().any(|p| p == prog),
    }
}

fn command_error_tag(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    prefix: &str,
    detail: &str,
) -> Result<Value, HostError> {
    let total = prefix.len().saturating_add(detail.len());
    if (total as u64) > MO_LIMIT_VALUE_BYTES {
        return Err(HostError::OutOfMemory {
            value_bytes_limit: true,
        });
    }
    let mut msg = String::with_capacity(total);
    msg.push_str(prefix);
    msg.push_str(detail);
    let msg_val = heap.alloc_text(msg)?;
    let err_tag = find_case_tag(types, result_ty, "Error");
    Ok(heap.make_case(types, result_ty, err_tag, vec![msg_val])?)
}

fn command_ok_tuple(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    status: i64,
    stdout_bytes: Vec<u8>,
    stderr_bytes: Vec<u8>,
) -> Result<Value, HostError> {
    let ok_tag = find_case_tag(types, result_ty, "Ok");
    let tup_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
    let out_val = heap.alloc_bytes(stdout_bytes)?;
    let err_val = heap.alloc_bytes(stderr_bytes)?;
    #[expect(
        clippy::expect_used,
        reason = "verified bytecode guarantees tuple layout exists"
    )]
    let layout = types
        .find_tuple_layout(tup_ty)
        .expect("tuple layout for Command.run_raw");
    let tup = heap.construct(types, layout, vec![Value::Int(status), out_val, err_val])?;
    Ok(heap.make_case(types, result_ty, ok_tag, vec![tup])?)
}

// ---------------------------------------------------------------------------
// Fake reply parser
// ---------------------------------------------------------------------------

fn parse_u64_until(bytes: &[u8], pos: &mut usize, sep: u8) -> Option<u64> {
    let start = *pos;
    let mut acc: u64 = 0;
    while *pos < bytes.len() && bytes[*pos] != sep {
        let c = bytes[*pos];
        if !c.is_ascii_digit() {
            return None;
        }
        let d = u64::from(c - b'0');
        acc = acc.checked_mul(10)?.checked_add(d)?;
        *pos += 1;
    }
    if *pos == start || *pos >= bytes.len() || bytes[*pos] != sep {
        return None;
    }
    *pos += 1;
    Some(acc)
}

fn parse_i64_until(bytes: &[u8], pos: &mut usize, sep: u8) -> Option<i64> {
    if *pos >= bytes.len() {
        return None;
    }
    let neg = bytes[*pos] == b'-';
    if neg {
        *pos += 1;
    }
    let mag = parse_u64_until(bytes, pos, sep)?;
    if neg {
        if mag > (i64::MAX as u64) + 1 {
            return None;
        }
        if mag == (i64::MAX as u64) + 1 {
            Some(i64::MIN)
        } else {
            Some(-(mag as i64))
        }
    } else if mag > i64::MAX as u64 {
        None
    } else {
        Some(mag as i64)
    }
}

// ---------------------------------------------------------------------------
// Real executable resolution, temp dir, and execution
// ---------------------------------------------------------------------------

#[cfg(unix)]
fn valid_local_bin_dir() -> Option<String> {
    let home = std::env::var("HOME").ok()?;
    if !home.starts_with('/') || home.contains(':') {
        return None;
    }
    let path = format!("{home}/.local/bin");
    if path.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
        return None;
    }
    let st = statat(CWD, path.as_str(), AtFlags::empty()).ok()?;
    if FileType::from_raw_mode(st.st_mode) == FileType::Directory {
        Some(path)
    } else {
        None
    }
}

#[cfg(unix)]
fn is_executable_regular(path: &str) -> bool {
    let Ok(st) = statat(CWD, path, AtFlags::empty()) else {
        return false;
    };
    FileType::from_raw_mode(st.st_mode) == FileType::RegularFile
        && access(path, Access::EXEC_OK).is_ok()
}

#[cfg(unix)]
fn resolve_executable(prog: &str) -> Option<String> {
    if prog == "mo"
        && let Ok(exe) = std::env::current_exe()
    {
        if exe.file_name().and_then(|n| n.to_str()) == Some("mo")
            && let Some(s) = exe.to_str()
            && s.len() < MO_LIMIT_HOST_PATH_SIZE as usize
        {
            return Some(s.to_string());
        }
        if let Some(parent) = exe.parent() {
            let candidate = parent.join("../mo");
            if let Ok(canon) = fs::canonicalize(&candidate)
                && let Some(s) = canon.to_str()
                && s.len() < MO_LIMIT_HOST_PATH_SIZE as usize
                && is_executable_regular(s)
            {
                return Some(s.to_string());
            }
        }
    }

    if let Some(local_bin) = valid_local_bin_dir() {
        let candidate = format!("{local_bin}/{prog}");
        if candidate.len() < MO_LIMIT_HOST_PATH_SIZE as usize && is_executable_regular(&candidate) {
            return Some(candidate);
        }
    }

    const DIRS: [&str; 4] = ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"];
    for dir in DIRS {
        let candidate = format!("{dir}/{prog}");
        if candidate.len() < MO_LIMIT_HOST_PATH_SIZE as usize && is_executable_regular(&candidate) {
            return Some(candidate);
        }
    }
    None
}

struct TempDirGuard(PathBuf);

impl Drop for TempDirGuard {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn make_temp_dir() -> Option<TempDirGuard> {
    let base = match std::env::var("TMPDIR") {
        Ok(v) if !v.is_empty() && v.len() < 512 => v,
        _ => "/tmp".to_string(),
    };
    let mut rng = [0u8; 8];
    for _ in 0..32 {
        if getrandom::fill(&mut rng).is_err() {
            return None;
        }
        let suffix = u64::from_le_bytes(rng);
        let candidate = Path::new(&base).join(format!("mo-cmd-{suffix:016x}"));
        if candidate.to_string_lossy().len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
            return None;
        }
        match fs::create_dir(&candidate) {
            Ok(()) => {
                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    let _ = fs::set_permissions(&candidate, fs::Permissions::from_mode(0o700));
                }
                return Some(TempDirGuard(candidate));
            }
            Err(e) if e.kind() == ErrorKind::AlreadyExists => continue,
            Err(_) => return None,
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Capability construction
// ---------------------------------------------------------------------------

#[cfg(unix)]
pub(crate) fn make_real_command(
    heap: &Heap,
    open_folders: &Rc<Cell<u32>>,
    folder_fd: BorrowedFd<'_>,
    ty: u32,
) -> Result<Value, HostError> {
    if u64::from(open_folders.get()) >= MO_LIMIT_OPEN_FOLDERS {
        return Err(HostError::Bug {
            reason: "too many folders open at once",
            detail: None,
        });
    }
    let fd = fcntl_dupfd_cloexec(folder_fd, 0).map_err(|_| HostError::Bug {
        reason: "too many folders open at once",
        detail: None,
    })?;
    open_folders.set(open_folders.get() + 1);
    let cap = CommandCap::Real(RealCommandCap {
        fd,
        pending: false,
        forbidden: false,
        prefix: None,
        allowed: None,
        open_folders: Rc::clone(open_folders),
    });
    Ok(heap.alloc_capability(Capability::Command, ty, Rc::new(cap))?)
}

pub(crate) fn command_fake(
    heap: &Heap,
    result_ty: u32,
    replies: Value,
) -> Result<Value, HostError> {
    let cap = CommandCap::Fake(FakeCommandCap {
        replies,
        prefix: None,
        forbidden: false,
        allowed: None,
    });
    Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(cap))?)
}

// ---------------------------------------------------------------------------
// Operation implementations
// ---------------------------------------------------------------------------

fn command_allow_programs(
    heap: &Heap,
    open_folders: &Rc<Cell<u32>>,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    let cap = crate::downcast_cap::<CommandCap>(&args[0], "Command capability");
    let existing = match cap {
        #[cfg(unix)]
        CommandCap::Real(r) => &r.allowed,
        CommandCap::Fake(f) => &f.allowed,
    };

    let mut next_allowed: Vec<String> = Vec::new();
    for item in args[1].as_list().iter() {
        let s = item.as_text();
        if !valid_program_name(s.as_bytes()) {
            continue;
        }
        if let Some(prev) = existing
            && !prev.iter().any(|p| p == s)
        {
            continue;
        }
        if !next_allowed.iter().any(|p| p == s) {
            next_allowed.push(s.to_string());
        }
    }

    match cap {
        CommandCap::Fake(f) => {
            let new_cap = CommandCap::Fake(FakeCommandCap {
                replies: f.replies.clone(),
                prefix: f.prefix.clone(),
                forbidden: f.forbidden,
                allowed: Some(next_allowed),
            });
            Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(new_cap))?)
        }
        #[cfg(unix)]
        CommandCap::Real(r) => {
            if u64::from(open_folders.get()) >= MO_LIMIT_OPEN_FOLDERS {
                return Err(HostError::Bug {
                    reason: "too many folders open at once",
                    detail: None,
                });
            }
            let fd = fcntl_dupfd_cloexec(&r.fd, 0).map_err(|_| HostError::Bug {
                reason: "too many folders open at once",
                detail: None,
            })?;
            open_folders.set(open_folders.get() + 1);
            let new_cap = CommandCap::Real(RealCommandCap {
                fd,
                pending: r.pending,
                forbidden: r.forbidden,
                prefix: r.prefix.clone(),
                allowed: Some(next_allowed),
                open_folders: Rc::clone(open_folders),
            });
            Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(new_cap))?)
        }
    }
}

fn command_within_fake(
    heap: &Heap,
    orig: &FakeCommandCap,
    result_ty: u32,
    raw: &str,
) -> Result<Value, HostError> {
    if orig.forbidden {
        let new_cap = CommandCap::Fake(FakeCommandCap {
            replies: orig.replies.clone(),
            prefix: orig.prefix.clone(),
            forbidden: true,
            allowed: orig.allowed.clone(),
        });
        return Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(new_cap))?);
    }
    let Ok(norm) = normalize_path(raw.as_bytes()) else {
        let new_cap = CommandCap::Fake(FakeCommandCap {
            replies: orig.replies.clone(),
            prefix: Some(raw.to_string()),
            forbidden: true,
            allowed: orig.allowed.clone(),
        });
        return Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(new_cap))?);
    };
    let joined = match &orig.prefix {
        Some(prev) if !prev.is_empty() => join_path(prev, &norm),
        _ => norm,
    };
    let prefix = if joined.is_empty() {
        None
    } else {
        Some(joined)
    };
    let new_cap = CommandCap::Fake(FakeCommandCap {
        replies: orig.replies.clone(),
        prefix,
        forbidden: false,
        allowed: orig.allowed.clone(),
    });
    Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(new_cap))?)
}

#[cfg(unix)]
fn command_within_real(
    heap: &Heap,
    resolver: &mut ResolverState,
    open_folders: &Rc<Cell<u32>>,
    orig: &RealCommandCap,
    result_ty: u32,
    raw: &str,
) -> Result<Value, HostError> {
    if u64::from(open_folders.get()) >= MO_LIMIT_OPEN_FOLDERS {
        return Err(HostError::Bug {
            reason: "too many folders open at once",
            detail: None,
        });
    }
    let dup_orig = || {
        fcntl_dupfd_cloexec(&orig.fd, 0).map_err(|_| HostError::Bug {
            reason: "too many folders open at once",
            detail: None,
        })
    };
    let make_cap = |fd: OwnedFd,
                    pending: bool,
                    forbidden: bool,
                    prefix: Option<String>|
     -> Result<Value, HostError> {
        open_folders.set(open_folders.get() + 1);
        let cap = CommandCap::Real(RealCommandCap {
            fd,
            pending,
            forbidden,
            prefix,
            allowed: orig.allowed.clone(),
            open_folders: Rc::clone(open_folders),
        });
        Ok(heap.alloc_capability(Capability::Command, result_ty, Rc::new(cap))?)
    };

    if orig.forbidden {
        return make_cap(dup_orig()?, false, true, orig.prefix.clone());
    }

    let Ok(norm) = normalize_path(raw.as_bytes()) else {
        return make_cap(dup_orig()?, false, true, Some(raw.to_string()));
    };
    let joined = if orig.pending {
        match &orig.prefix {
            Some(prev) if !prev.is_empty() => join_path(prev, &norm),
            _ => norm,
        }
    } else {
        norm
    };

    if joined.is_empty() {
        return make_cap(dup_orig()?, false, false, None);
    }

    if joined.len() < MO_LIMIT_HOST_PATH_SIZE as usize {
        match resolve(resolver, orig.fd.as_fd(), &joined, PathMode::Folder) {
            Ok(resolved) => return make_cap(resolved.fd, false, false, None),
            Err(rerr) if rerr.containment => {
                return make_cap(dup_orig()?, false, true, Some(raw.to_string()));
            }
            Err(_) => {}
        }
    }

    make_cap(dup_orig()?, true, false, Some(joined))
}

fn command_run_fake(
    heap: &Heap,
    types: &TypeTable,
    fake: &FakeCommandCap,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    if fake.forbidden {
        let esc = fake.prefix.as_deref().unwrap_or("");
        return command_error_tag(heap, types, result_ty, "outside_folder:", esc);
    }
    let prog = args[1].as_text();
    if !program_allowed(&fake.allowed, prog) {
        return command_error_tag(heap, types, result_ty, "not_permitted:", prog);
    }
    let cmd_args = args[2].as_list();
    for a in cmd_args.iter() {
        let ab = a.as_text().as_bytes();
        if ab.len() as u64 > MO_LIMIT_COMMAND_ARG_BYTES || ab.contains(&0) {
            return command_error_tag(
                heap,
                types,
                result_ty,
                "spawn_failed:invalid command argument",
                "",
            );
        }
    }

    let cwd = fake.prefix.as_deref().unwrap_or("");
    let mut key_bytes = Vec::new();
    key_bytes.extend_from_slice(cwd.len().to_string().as_bytes());
    key_bytes.push(b':');
    key_bytes.extend_from_slice(cwd.as_bytes());
    key_bytes.push(b'\n');
    key_bytes.extend_from_slice(prog.len().to_string().as_bytes());
    key_bytes.push(b':');
    key_bytes.extend_from_slice(prog.as_bytes());
    key_bytes.push(b'\n');
    key_bytes.extend_from_slice(cmd_args.len().to_string().as_bytes());
    key_bytes.push(b'\n');
    for a in cmd_args.iter() {
        let ab = a.as_text().as_bytes();
        key_bytes.extend_from_slice(ab.len().to_string().as_bytes());
        key_bytes.push(b':');
        key_bytes.extend_from_slice(ab);
        key_bytes.push(b'\n');
    }

    let key_val = heap.alloc_bytes(key_bytes)?;
    let Some(reply_val) = fake
        .replies
        .as_map()
        .get(types, heap.hash_key(), &key_val)
        .cloned()
    else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:unexpected command in fake: ",
            prog,
        );
    };

    let val = reply_val.as_bytes();
    let mut pos = 0usize;
    let Some(status) = parse_i64_until(val, &mut pos, b':') else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:malformed fake command reply",
            "",
        );
    };
    let Some(out_len) = parse_u64_until(val, &mut pos, b':') else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:malformed fake command reply",
            "",
        );
    };
    let Some(err_len) = parse_u64_until(val, &mut pos, b'\n') else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:malformed fake command reply",
            "",
        );
    };
    if out_len > MO_LIMIT_COMMAND_OUTPUT_BYTES
        || err_len > MO_LIMIT_COMMAND_OUTPUT_BYTES
        || pos + (out_len as usize) + (err_len as usize) != val.len()
    {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:malformed fake command reply",
            "",
        );
    }
    let out_slice = val[pos..pos + out_len as usize].to_vec();
    let err_slice = val[pos + out_len as usize..].to_vec();
    command_ok_tuple(heap, types, result_ty, status, out_slice, err_slice)
}

#[cfg(unix)]
fn command_run_real(
    heap: &Heap,
    types: &TypeTable,
    resolver: &mut ResolverState,
    root: &RealCommandCap,
    result_ty: u32,
    args: &[Value],
    timeout_us: i64,
) -> Result<Value, HostError> {
    if root.forbidden {
        let esc = root.prefix.as_deref().unwrap_or("");
        return command_error_tag(heap, types, result_ty, "outside_folder:", esc);
    }
    let prog = args[1].as_text();
    if !program_allowed(&root.allowed, prog) {
        return command_error_tag(heap, types, result_ty, "not_permitted:", prog);
    }

    let resolved_dir_fd: Option<OwnedFd> = if root.pending {
        let rel = root.prefix.as_deref().unwrap_or("");
        if rel.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
            return command_error_tag(heap, types, result_ty, "spawn_failed:path too long", "");
        }
        match resolve(resolver, root.fd.as_fd(), rel, PathMode::Folder) {
            Ok(r) => Some(r.fd),
            Err(rerr) if rerr.containment || rerr.status == FileStatus::NotAllowed => {
                return command_error_tag(heap, types, result_ty, "outside_folder:", rel);
            }
            Err(_) => {
                return command_error_tag(
                    heap,
                    types,
                    result_ty,
                    "spawn_failed:working folder unavailable",
                    "",
                );
            }
        }
    } else {
        None
    };
    let workdir_fd: BorrowedFd<'_> = match resolved_dir_fd.as_ref() {
        Some(fd) => fd.as_fd(),
        None => root.fd.as_fd(),
    };

    let cmd_args = args[2].as_list();
    let mut arg_strs = Vec::with_capacity(cmd_args.len());
    for a in cmd_args.iter() {
        let s = a.as_text();
        if s.len() as u64 > MO_LIMIT_COMMAND_ARG_BYTES || s.as_bytes().contains(&0) {
            return command_error_tag(
                heap,
                types,
                result_ty,
                "spawn_failed:invalid command argument",
                "",
            );
        }
        arg_strs.push(s);
    }

    let Some(exe_path) = resolve_executable(prog) else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:program not found: ",
            prog,
        );
    };
    let Some(workdir_path) = dir_fd_path(workdir_fd, false) else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:could not resolve working folder",
            "",
        );
    };
    let Some(tmp_guard) = make_temp_dir() else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:could not create temporary folder",
            "",
        );
    };

    let Ok(workdir_stat) = fstat(workdir_fd) else {
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:could not resolve working folder",
            "",
        );
    };

    let path_env = match valid_local_bin_dir() {
        Some(local_bin) => format!("{local_bin}:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"),
        None => "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin".to_string(),
    };

    let deadline = Instant::now() + Duration::from_micros(timeout_us as u64);
    let cpu_seconds = ((timeout_us as u64).saturating_add(999_999) / 1_000_000).saturating_add(1);
    let can_confine = crate::confine::can_confine();

    let mut cmd = if let Some(trampoline) = crate::confine::trampoline_exe() {
        let mut c = Command::new(trampoline);
        c.arg(crate::confine::INTERNAL_COMMAND_TRAMPOLINE_ARG)
            .arg("--dev")
            .arg(workdir_stat.st_dev.to_string())
            .arg("--ino")
            .arg(workdir_stat.st_ino.to_string())
            .arg("--workdir")
            .arg(&workdir_path)
            .arg("--tmpdir")
            .arg(&tmp_guard.0)
            .arg("--cpu")
            .arg(cpu_seconds.to_string())
            .arg("--mem")
            .arg(MO_LIMIT_COMMAND_MEMORY.to_string())
            .arg("--ppid")
            .arg(std::process::id().to_string())
            .arg(if can_confine {
                "--confine"
            } else {
                "--no-confine"
            })
            .arg("--exe")
            .arg(&exe_path)
            .arg("--")
            .args(&arg_strs);
        c
    } else {
        let mut c = Command::new(&exe_path);
        c.args(&arg_strs);
        c
    };

    let mut child = match cmd
        .current_dir(&workdir_path)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .env_clear()
        .env("PATH", &path_env)
        .env("HOME", &workdir_path)
        .env("TMPDIR", &tmp_guard.0)
        .env("LANG", "C.UTF-8")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            return command_error_tag(heap, types, result_ty, "spawn_failed:fork failed", "");
        }
    };
    crate::confine::set_child_process_group(child.id());

    let mut out_pipe = child.stdout.take();
    let mut err_pipe = child.stderr.take();
    if let Some(ref p) = out_pipe {
        let _ = fcntl_setfl(p.as_fd(), OFlags::NONBLOCK);
    }
    if let Some(ref p) = err_pipe {
        let _ = fcntl_setfl(p.as_fd(), OFlags::NONBLOCK);
    }

    let mut io_scratch = ScratchGuard::new(heap.budget());
    let mut out_buf: Vec<u8> = Vec::new();
    let mut err_buf: Vec<u8> = Vec::new();
    let mut exit_status = None;
    let mut timed_out = false;
    let mut too_large = false;
    let mut oom = false;
    let max_out = MO_LIMIT_COMMAND_OUTPUT_BYTES as usize;

    while out_pipe.is_some() || err_pipe.is_some() || exit_status.is_none() {
        let now = Instant::now();
        if now >= deadline {
            if exit_status.is_none()
                && let Ok(Some(st)) = child.try_wait()
            {
                exit_status = Some(st);
            }
            if exit_status.is_none() {
                timed_out = true;
                break;
            }
        }

        if out_pipe.is_some() || err_pipe.is_some() {
            let rem = deadline.saturating_duration_since(now);
            let wait = if exit_status.is_some() {
                Duration::ZERO
            } else {
                rem.min(Duration::from_millis(20))
            };
            let ts = rustix::event::Timespec {
                tv_sec: i64::try_from(wait.as_secs()).unwrap_or(i64::MAX),
                tv_nsec: i64::from(wait.subsec_nanos()),
            };

            let mut poll_ready = [false; 2];
            {
                let mut pfds = Vec::with_capacity(2);
                let mut indices = Vec::with_capacity(2);
                if let Some(ref p) = out_pipe {
                    pfds.push(rustix::event::PollFd::new(p, rustix::event::PollFlags::IN));
                    indices.push(0usize);
                }
                if let Some(ref p) = err_pipe {
                    pfds.push(rustix::event::PollFd::new(p, rustix::event::PollFlags::IN));
                    indices.push(1usize);
                }
                match rustix::event::poll(&mut pfds, Some(&ts)) {
                    Ok(_) => {
                        for (pfd, idx) in pfds.iter().zip(indices) {
                            let rev = pfd.revents();
                            if rev.intersects(
                                rustix::event::PollFlags::IN
                                    | rustix::event::PollFlags::HUP
                                    | rustix::event::PollFlags::ERR,
                            ) {
                                poll_ready[idx] = true;
                            }
                        }
                    }
                    Err(rustix::io::Errno::INTR) => {}
                    Err(_) => break,
                }
            }

            fn drain_pipe<R: Read>(
                pipe_opt: &mut Option<R>,
                buf: &mut Vec<u8>,
                max_out: usize,
                io_scratch: &mut ScratchGuard<'_>,
            ) -> (bool, bool) {
                let mut hit_large = false;
                let mut hit_oom = false;
                let mut close_pipe = false;
                let mut chunk = [0u8; 8192];
                if let Some(pipe) = pipe_opt.as_mut() {
                    loop {
                        match pipe.read(&mut chunk) {
                            Ok(0) => {
                                close_pipe = true;
                                break;
                            }
                            Ok(n) => {
                                if n > max_out - buf.len() {
                                    hit_large = true;
                                    break;
                                }
                                if io_scratch.charge(n).is_err() {
                                    hit_oom = true;
                                    break;
                                }
                                buf.extend_from_slice(&chunk[..n]);
                            }
                            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
                            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                            Err(_) => {
                                close_pipe = true;
                                break;
                            }
                        }
                    }
                }
                if close_pipe {
                    *pipe_opt = None;
                }
                (hit_large, hit_oom)
            }

            if poll_ready[0] {
                let (l, o) = drain_pipe(&mut out_pipe, &mut out_buf, max_out, &mut io_scratch);
                too_large |= l;
                oom |= o;
            }
            if !too_large && !oom && poll_ready[1] {
                let (l, o) = drain_pipe(&mut err_pipe, &mut err_buf, max_out, &mut io_scratch);
                too_large |= l;
                oom |= o;
            }
            if too_large || oom {
                break;
            }
        }

        if exit_status.is_none() {
            match child.try_wait() {
                Ok(Some(st)) => exit_status = Some(st),
                Ok(None) if out_pipe.is_none() && err_pipe.is_none() => {
                    std::thread::sleep(Duration::from_millis(2));
                }
                _ => {}
            }
        }
    }

    if exit_status.is_none() {
        crate::confine::kill_child_tree(child.id());
        let _ = child.kill();
        exit_status = child.wait().ok();
    }
    drop(out_pipe);
    drop(err_pipe);
    drop(tmp_guard);

    if oom {
        drop(io_scratch);
        return Err(HostError::OutOfMemory {
            value_bytes_limit: false,
        });
    }
    if too_large {
        drop(io_scratch);
        return command_error_tag(heap, types, result_ty, "output_too_large", "");
    }
    let Some(st) = exit_status else {
        drop(io_scratch);
        return command_error_tag(heap, types, result_ty, "spawn_failed:child wait failed", "");
    };
    if timed_out || st.signal() == Some(24) {
        drop(io_scratch);
        return command_error_tag(heap, types, result_ty, "timed_out", "");
    }
    if st.code() == Some(125) {
        drop(io_scratch);
        return command_error_tag(
            heap,
            types,
            result_ty,
            "spawn_failed:could not confine command child",
            "",
        );
    }

    let exit_code = match st.code() {
        Some(c) => i64::from(c),
        None => 128 + i64::from(st.signal().unwrap_or(0)),
    };
    drop(io_scratch);
    command_ok_tuple(heap, types, result_ty, exit_code, out_buf, err_buf)
}

// ---------------------------------------------------------------------------
// Public dispatch
// ---------------------------------------------------------------------------

pub(crate) fn call_command(
    heap: &Heap,
    types: &TypeTable,
    #[cfg(unix)] resolver: &mut ResolverState,
    open_folders: &Rc<Cell<u32>>,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    match op {
        HostOpId::CommandAllowPrograms => {
            command_allow_programs(heap, open_folders, result_ty, args)
        }
        HostOpId::CommandWithin => {
            let cap = crate::downcast_cap::<CommandCap>(&args[0], "Command capability");
            let raw = args[1].as_text();
            match cap {
                CommandCap::Fake(f) => command_within_fake(heap, f, result_ty, raw),
                #[cfg(unix)]
                CommandCap::Real(r) => {
                    command_within_real(heap, resolver, open_folders, r, result_ty, raw)
                }
            }
        }
        HostOpId::CommandRunRaw => {
            let timeout_us = args[3].as_duration();
            if !(1000..=MO_LIMIT_COMMAND_DURATION_US).contains(&timeout_us) {
                return Err(HostError::Bug {
                    reason: "timeout below 1ms or above 1 hour",
                    detail: None,
                });
            }
            if args[2].as_list().len() as u64 > MO_LIMIT_COMMAND_ARGS {
                return Err(HostError::Bug {
                    reason: "too many command arguments",
                    detail: None,
                });
            }
            let cap = crate::downcast_cap::<CommandCap>(&args[0], "Command capability");
            match cap {
                CommandCap::Fake(f) => command_run_fake(heap, types, f, result_ty, args),
                #[cfg(unix)]
                CommandCap::Real(r) => {
                    command_run_real(heap, types, resolver, r, result_ty, args, timeout_us)
                }
            }
        }
        _ => unreachable!("non-command hostop routed to command module"),
    }
}
