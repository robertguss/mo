use std::cell::{Cell, RefCell};
use std::mem::MaybeUninit;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, OwnedFd};
use std::path::Path;
use std::rc::Rc;

use mo_bytecode::Capability;
use mo_bytecode::types::TypeTable;
use mo_def::HostOpId;
use mo_def::limits::{
    MO_LIMIT_FILE_IO_BYTES, MO_LIMIT_HOST_PATH_SIZE, MO_LIMIT_OPEN_FILES, MO_LIMIT_OPEN_FOLDERS,
    MO_LIMIT_PATH_LINKS, MO_LIMIT_PATH_STEPS, MO_LIMIT_VALUE_BYTES,
};
use mo_heap::{Heap, ScratchGuard, Value};
use rustix::fs::{
    AtFlags, CWD, FileType, Mode, OFlags, RawDir, Stat, fstat, ftruncate, mkdirat, openat,
    readlinkat, renameat, statat, unlinkat,
};
use rustix::io::{Errno, fcntl_dupfd_cloexec, pread, pwrite, read, write};

use crate::HostError;
use crate::files_common::{
    EntryKind, FileProblem, errno_to_problem, find_case_tag, make_file_error, make_file_ok,
    make_names_or_entries, prepare_real_path,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum PathMode {
    Read,
    Write,
    Append,
    Folder,
    MakeFolder,
    Remove,
    Exists,
    StrictParent,
}

pub(crate) struct PathResult {
    pub(crate) fd: OwnedFd,
    pub(crate) name: String,
    pub(crate) stat: Stat,
}

struct RealFolderInner {
    fd: OwnedFd,
    pending: bool,
    forbidden: bool,
    parent: Option<Rc<RealFolder>>,
    prefix: String,
}

pub(crate) struct RealFolder {
    inner: RefCell<RealFolderInner>,
    open_folders: Rc<Cell<u32>>,
}

impl Drop for RealFolder {
    fn drop(&mut self) {
        let cur = self.open_folders.get();
        debug_assert!(cur != 0);
        self.open_folders.set(cur.saturating_sub(1));
    }
}

pub(crate) struct RealOpenFile {
    fd: OwnedFd,
    path: Value,
    open_files: Rc<Cell<u32>>,
}

impl Drop for RealOpenFile {
    fn drop(&mut self) {
        let cur = self.open_files.get();
        debug_assert!(cur != 0);
        self.open_files.set(cur.saturating_sub(1));
    }
}

pub(crate) struct ResolverState {
    pub(crate) kernel_probed: bool,
    pub(crate) kernel_available: bool,
    pub(crate) force_fallback: bool,
}

impl ResolverState {
    pub(crate) fn new() -> Self {
        Self {
            kernel_probed: false,
            kernel_available: false,
            force_fallback: false,
        }
    }
}

pub(crate) fn raise_nofile_limit_best_effort() {
    use rustix::process::{Resource, getrlimit, setrlimit};
    let mut budget = getrlimit(Resource::Nofile);
    let wanted = MO_LIMIT_OPEN_FOLDERS + MO_LIMIT_OPEN_FILES + 64 + 16 + 32 + 32;
    let target = match budget.maximum {
        Some(max) => wanted.min(max),
        None => wanted,
    };
    if budget.current.is_some_and(|cur| cur < target) {
        budget.current = Some(target);
        let _ = setrlimit(Resource::Nofile, budget);
    }
}

pub(crate) fn open_root_dir(path: &Path) -> Result<OwnedFd, String> {
    openat(
        CWD,
        path,
        OFlags::RDONLY | OFlags::DIRECTORY | OFlags::CLOEXEC,
        Mode::empty(),
    )
    .map_err(|e| e.to_string())
}

pub(crate) fn make_real_folder_cap(
    heap: &Heap,
    open_folders: &Rc<Cell<u32>>,
    root_fd: BorrowedFd<'_>,
    cap: Capability,
    ty: u32,
) -> Result<Value, HostError> {
    if u64::from(open_folders.get()) >= MO_LIMIT_OPEN_FOLDERS {
        return Err(HostError::Bug {
            reason: "too many folders open at once",
            detail: None,
        });
    }
    let fd = fcntl_dupfd_cloexec(root_fd, 0).map_err(|_| HostError::Bug {
        reason: "too many folders open at once",
        detail: None,
    })?;
    let folder = RealFolder {
        inner: RefCell::new(RealFolderInner {
            fd,
            pending: false,
            forbidden: false,
            parent: None,
            prefix: String::new(),
        }),
        open_folders: Rc::clone(open_folders),
    };
    open_folders.set(open_folders.get() + 1);
    let cap_obj = crate::FolderCap::Real(Rc::new(folder));
    Ok(heap.alloc_capability(cap, ty, Rc::new(cap_obj))?)
}

fn directory_flags() -> OFlags {
    #[cfg(target_os = "linux")]
    {
        OFlags::PATH | OFlags::DIRECTORY | OFlags::CLOEXEC
    }
    #[cfg(not(target_os = "linux"))]
    {
        OFlags::RDONLY | OFlags::DIRECTORY | OFlags::CLOEXEC
    }
}

fn mode_flags(mode: PathMode) -> OFlags {
    let common = OFlags::CLOEXEC | OFlags::NONBLOCK | OFlags::NOCTTY;
    match mode {
        PathMode::Write => OFlags::WRONLY | OFlags::CREATE | common,
        PathMode::Append => OFlags::WRONLY | OFlags::CREATE | OFlags::APPEND | common,
        _ => OFlags::RDONLY | common,
    }
}

fn check_kind(mode: PathMode, st: &Stat, trailing_slash: bool) -> Result<(), FileProblem> {
    let ft = FileType::from_raw_mode(st.st_mode);
    if (matches!(
        mode,
        PathMode::Folder | PathMode::MakeFolder | PathMode::StrictParent
    ) || trailing_slash)
        && ft != FileType::Directory
    {
        return Err(FileProblem::failed("not a folder"));
    }
    if matches!(mode, PathMode::Read | PathMode::Write | PathMode::Append)
        && ft == FileType::Directory
    {
        return Err(FileProblem::failed("is a folder"));
    }
    if matches!(mode, PathMode::Read | PathMode::Write | PathMode::Append)
        && ft != FileType::RegularFile
    {
        return Err(FileProblem::failed("not a regular file"));
    }
    Ok(())
}

fn step(steps: &mut u32) -> Result<(), FileProblem> {
    if u64::from(*steps) == MO_LIMIT_PATH_STEPS {
        return Err(FileProblem::failed("MO_LIMIT_PATH_STEPS"));
    }
    *steps += 1;
    Ok(())
}

struct Ancestor {
    dev: u64,
    ino: u64,
    name: String,
}

fn replay(
    root: BorrowedFd<'_>,
    ancestors: &[Ancestor],
    steps: &mut u32,
) -> Result<Option<OwnedFd>, FileProblem> {
    let mut current: Option<OwnedFd> = None;
    for anc in ancestors {
        step(steps)?;
        let cur_fd = current.as_ref().map_or(root, |f| f.as_fd());
        let next = openat(
            cur_fd,
            anc.name.as_str(),
            directory_flags() | OFlags::NOFOLLOW,
            Mode::empty(),
        )
        .map_err(|_| FileProblem::failed("changed while being used"))?;
        let st = fstat(&next).map_err(|_| FileProblem::failed("changed while being used"))?;
        if st.st_dev as u64 != anc.dev || st.st_ino as u64 != anc.ino {
            return Err(FileProblem::failed("changed while being used"));
        }
        current = Some(next);
    }
    Ok(current)
}

#[cfg(target_os = "linux")]
fn openat2_beneath(root: BorrowedFd<'_>, path: &str, links: bool) -> Result<OwnedFd, Errno> {
    use rustix::fs::{ResolveFlags, openat2};
    let resolve = ResolveFlags::BENEATH
        | ResolveFlags::NO_MAGICLINKS
        | if links {
            ResolveFlags::empty()
        } else {
            ResolveFlags::NO_SYMLINKS
        };
    loop {
        match openat2(
            root,
            path,
            OFlags::PATH | OFlags::CLOEXEC,
            Mode::empty(),
            resolve,
        ) {
            Err(Errno::INTR) => continue,
            other => return other,
        }
    }
}

#[cfg(target_os = "linux")]
fn resolve_kernel(
    resolver: &mut ResolverState,
    root: BorrowedFd<'_>,
    path: &str,
    mode: PathMode,
    steps: &mut u32,
) -> Option<Result<PathResult, FileProblem>> {
    if !resolver.kernel_probed {
        match openat2_beneath(root, ".", false) {
            Ok(_) => resolver.kernel_available = true,
            Err(Errno::NOSYS | Errno::INVAL | Errno::PERM) => resolver.kernel_available = false,
            Err(_) => resolver.kernel_available = true,
        }
        resolver.kernel_probed = true;
    }
    if !resolver.kernel_available || mode == PathMode::MakeFolder {
        return None;
    }

    let mut leaf_name = String::new();
    let lookup_owned: String;
    let lookup: &str = if matches!(mode, PathMode::Remove | PathMode::StrictParent) {
        let base = if path.is_empty() { "." } else { path };
        if let Some(slash) = base.rfind('/') {
            leaf_name = base[slash + 1..].to_string();
            lookup_owned = base[..slash].to_string();
            &lookup_owned
        } else {
            leaf_name = base.to_string();
            "."
        }
    } else if path.is_empty() {
        "."
    } else {
        path
    };

    let mut opened = openat2_beneath(root, lookup, false);
    if let Err(Errno::LOOP) = opened {
        if mode == PathMode::StrictParent {
            return Some(Err(FileProblem::not_allowed(true)));
        }
        match resolve_portable(root, lookup, PathMode::Exists, steps) {
            Err(p) if p.status == crate::files_common::FileStatus::NotFound => return None,
            Err(p) => return Some(Err(p)),
            Ok(_checked) => {
                opened = openat2_beneath(root, lookup, true);
            }
        }
    }

    let mut fd = match opened {
        Ok(fd) => fd,
        Err(Errno::XDEV) => return Some(Err(FileProblem::not_allowed(true))),
        Err(_) => return None,
    };

    let stat = if mode == PathMode::Remove {
        statat(&fd, leaf_name.as_str(), AtFlags::SYMLINK_NOFOLLOW)
    } else {
        fstat(&fd)
    };
    let st = match stat {
        Ok(st) => st,
        Err(e) => return Some(Err(errno_to_problem(e))),
    };
    if let Err(p) = check_kind(mode, &st, false) {
        return Some(Err(p));
    }

    if matches!(mode, PathMode::Read | PathMode::Write | PathMode::Append) {
        let proc_path = format!("/proc/self/fd/{}", fd.as_raw_fd());
        let Ok(actual) = openat(
            CWD,
            proc_path.as_str(),
            mode_flags(mode) & !OFlags::CREATE,
            Mode::empty(),
        ) else {
            return None;
        };
        let reopened = match fstat(&actual) {
            Ok(s) => s,
            Err(e) => return Some(Err(errno_to_problem(e))),
        };
        if reopened.st_dev != st.st_dev || reopened.st_ino != st.st_ino {
            return Some(Err(FileProblem::failed("changed while being used")));
        }
        fd = actual;
    }

    Some(Ok(PathResult {
        fd,
        name: leaf_name,
        stat: st,
    }))
}

fn resolve_portable(
    root: BorrowedFd<'_>,
    path: &str,
    mode: PathMode,
    steps: &mut u32,
) -> Result<PathResult, FileProblem> {
    let max_path = MO_LIMIT_HOST_PATH_SIZE as usize;
    let mut ancestors: Vec<Ancestor> = Vec::new();
    let mut name_bytes: usize = 0;
    let mut current: Option<OwnedFd> = None;
    let mut pending: Vec<u8> = path.as_bytes().to_vec();
    let mut at: usize = 0;
    let mut links: u32 = 0;
    let mut races: u32 = 0;
    let mut trailing_slash = false;

    loop {
        step(steps)?;
        while at < pending.len() && pending[at] == b'/' {
            at += 1;
        }
        if at == pending.len() {
            if mode == PathMode::StrictParent {
                return Err(FileProblem::failed("is a folder"));
            }
            let cur_fd = current.as_ref().map_or(root, |f| f.as_fd());
            let fd = fcntl_dupfd_cloexec(cur_fd, 0).map_err(errno_to_problem)?;
            let st = fstat(&fd).map_err(errno_to_problem)?;
            check_kind(mode, &st, trailing_slash)?;
            return Ok(PathResult {
                fd,
                name: ".".to_string(),
                stat: st,
            });
        }

        let mut end = at;
        while end < pending.len() && pending[end] != b'/' {
            end += 1;
        }
        let component_bytes = &pending[at..end];
        let mut next = end;
        while next < pending.len() && pending[next] == b'/' {
            next += 1;
        }
        let is_final = next == pending.len();
        if is_final && end < pending.len() && pending[end] == b'/' {
            trailing_slash = true;
        }

        if component_bytes == b"." {
            at = next;
            continue;
        }

        if component_bytes == b".." {
            let popped = ancestors
                .pop()
                .ok_or_else(|| FileProblem::not_allowed(true))?;
            name_bytes -= popped.name.len() + 1;
            while next + 2 <= pending.len()
                && pending[next] == b'.'
                && pending[next + 1] == b'.'
                && (next + 2 == pending.len() || pending[next + 2] == b'/')
            {
                let popped = ancestors
                    .pop()
                    .ok_or_else(|| FileProblem::not_allowed(true))?;
                step(steps)?;
                name_bytes -= popped.name.len() + 1;
                next += 2;
                while next < pending.len() && pending[next] == b'/' {
                    next += 1;
                }
            }
            current = replay(root, &ancestors, steps)?;
            at = next;
            continue;
        }

        let cur_fd = current.as_ref().map_or(root, |f| f.as_fd());

        if is_final && mode == PathMode::StrictParent {
            if trailing_slash {
                return Err(FileProblem::failed("not a folder"));
            }
            let fd = fcntl_dupfd_cloexec(cur_fd, 0).map_err(errno_to_problem)?;
            let st = fstat(&fd).map_err(errno_to_problem)?;
            check_kind(mode, &st, false)?;
            let name = String::from_utf8(component_bytes.to_vec())
                .map_err(|_| FileProblem::failed("invalid path"))?;
            return Ok(PathResult { fd, name, stat: st });
        }

        let mut stat_res = statat(cur_fd, component_bytes, AtFlags::SYMLINK_NOFOLLOW);
        let mut missing = matches!(stat_res, Err(Errno::NOENT));
        if missing && mode == PathMode::MakeFolder {
            match mkdirat(cur_fd, component_bytes, Mode::from_raw_mode(0o777)) {
                Ok(()) | Err(Errno::EXIST) => {}
                Err(e) => return Err(errno_to_problem(e)),
            }
            stat_res = statat(cur_fd, component_bytes, AtFlags::SYMLINK_NOFOLLOW);
            missing = false;
        }

        let creating = missing && is_final && matches!(mode, PathMode::Write | PathMode::Append);
        if creating && trailing_slash {
            return Err(FileProblem::failed("not a folder"));
        }
        let st_opt = match stat_res {
            Ok(st) => Some(st),
            Err(_) if creating => None,
            Err(e) => return Err(errno_to_problem(e)),
        };

        if let Some(st) = st_opt
            && FileType::from_raw_mode(st.st_mode) == FileType::Symlink
            && !(is_final && mode == PathMode::Remove)
        {
            if mode == PathMode::StrictParent {
                return Err(FileProblem::not_allowed(true));
            }
            if u64::from(links) == MO_LIMIT_PATH_LINKS {
                return Err(FileProblem::failed("too many links"));
            }
            let target = match readlinkat(cur_fd, component_bytes, Vec::new()) {
                Ok(t) => t,
                Err(Errno::INVAL) if u64::from(races) < MO_LIMIT_PATH_LINKS => {
                    races += 1;
                    continue;
                }
                Err(e) => return Err(errno_to_problem(e)),
            };
            links += 1;
            let target_bytes = target.as_bytes();
            if target_bytes.is_empty() || target_bytes[0] == b'/' {
                return Err(FileProblem::not_allowed(true));
            }
            let rest = &pending[next..];
            let sep = usize::from(!rest.is_empty());
            if target_bytes.len() + sep + rest.len() >= max_path {
                return Err(FileProblem::failed("path too long"));
            }
            let mut next_pending = Vec::with_capacity(target_bytes.len() + sep + rest.len());
            next_pending.extend_from_slice(target_bytes);
            if sep != 0 {
                next_pending.push(b'/');
                next_pending.extend_from_slice(rest);
            }
            pending = next_pending;
            at = 0;
            continue;
        }

        if let Some(st) = st_opt {
            if !is_final && FileType::from_raw_mode(st.st_mode) != FileType::Directory {
                return Err(FileProblem::failed("not a folder"));
            }
            if is_final && !creating {
                check_kind(mode, &st, trailing_slash)?;
                if matches!(mode, PathMode::Remove | PathMode::Exists) {
                    let fd = fcntl_dupfd_cloexec(cur_fd, 0).map_err(errno_to_problem)?;
                    let name = String::from_utf8(component_bytes.to_vec())
                        .map_err(|_| FileProblem::failed("invalid path"))?;
                    return Ok(PathResult { fd, name, stat: st });
                }
            }
        }

        let mut open_flags = if is_final {
            if matches!(mode, PathMode::Folder | PathMode::MakeFolder) {
                directory_flags()
            } else {
                mode_flags(mode)
            }
        } else {
            directory_flags()
        };
        if creating {
            open_flags |= OFlags::EXCL;
        }

        let fd = match openat(
            cur_fd,
            component_bytes,
            open_flags | OFlags::NOFOLLOW,
            Mode::from_raw_mode(0o666),
        ) {
            Ok(fd) => fd,
            Err(Errno::LOOP) if mode == PathMode::StrictParent => {
                return Err(FileProblem::not_allowed(true));
            }
            Err(e)
                if (matches!(e, Errno::LOOP | Errno::NOTDIR)
                    || (creating && e == Errno::EXIST))
                    && u64::from(races) < MO_LIMIT_PATH_LINKS =>
            {
                races += 1;
                continue;
            }
            Err(e) => return Err(errno_to_problem(e)),
        };

        let st = fstat(&fd).map_err(errno_to_problem)?;
        if is_final {
            check_kind(mode, &st, trailing_slash)?;
            let name = String::from_utf8_lossy(component_bytes).into_owned();
            return Ok(PathResult { fd, name, stat: st });
        }

        let comp_len = component_bytes.len();
        if ancestors.len() == max_path / 2 || comp_len + 1 > max_path - name_bytes {
            return Err(FileProblem::failed("path too long"));
        }
        let comp_str = String::from_utf8(component_bytes.to_vec())
            .map_err(|_| FileProblem::failed("invalid path"))?;
        ancestors.push(Ancestor {
            dev: st.st_dev as u64,
            ino: st.st_ino as u64,
            name: comp_str,
        });
        name_bytes += comp_len + 1;
        current = Some(fd);
        at = next;
    }
}

pub(crate) fn resolve(
    resolver: &mut ResolverState,
    root: BorrowedFd<'_>,
    path: &str,
    mode: PathMode,
) -> Result<PathResult, FileProblem> {
    if path.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
        return Err(FileProblem::failed("path too long"));
    }
    let mut steps = 0u32;
    #[cfg(target_os = "linux")]
    if !resolver.force_fallback
        && let Some(res) = resolve_kernel(resolver, root, path, mode, &mut steps)
    {
        return res;
    }
    resolve_portable(root, path, mode, &mut steps)
}

pub(crate) fn dir_fd_path(dir_fd: BorrowedFd<'_>, verify_identity: bool) -> Option<String> {
    #[cfg(target_os = "linux")]
    {
        let proc_link = format!("/proc/self/fd/{}", dir_fd.as_raw_fd());
        let raw = readlinkat(CWD, proc_link.as_str(), Vec::new()).ok()?;
        let bytes = raw.as_bytes();
        if bytes.is_empty() || bytes.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
            return None;
        }
        let path = String::from_utf8(bytes.to_vec()).ok()?;
        if verify_identity {
            let dir_st = fstat(dir_fd).ok()?;
            let path_st = statat(CWD, path.as_str(), AtFlags::empty()).ok()?;
            if dir_st.st_dev != path_st.st_dev || dir_st.st_ino != path_st.st_ino {
                return None;
            }
        }
        Some(path)
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (dir_fd, verify_identity);
        None
    }
}

fn resolve_folder_root(
    resolver: &mut ResolverState,
    folder: &Rc<RealFolder>,
) -> Result<OwnedFd, FileProblem> {
    let mut pending_chain: Vec<Rc<RealFolder>> = Vec::new();
    let mut cur = Rc::clone(folder);
    loop {
        let next_parent = {
            let inner = cur.inner.borrow();
            if !inner.pending {
                None
            } else {
                if inner.forbidden {
                    return Err(FileProblem::not_allowed(false));
                }
                #[expect(
                    clippy::expect_used,
                    reason = "pending folders always retain their parent reference until resolved"
                )]
                let parent = inner
                    .parent
                    .as_ref()
                    .expect("pending folder retains parent")
                    .clone();
                Some(parent)
            }
        };
        match next_parent {
            None => break,
            Some(p) => {
                pending_chain.push(cur);
                cur = p;
            }
        }
    }

    let mut fd = {
        let inner = cur.inner.borrow();
        fcntl_dupfd_cloexec(&inner.fd, 0).map_err(errno_to_problem)?
    };

    while let Some(node) = pending_chain.pop() {
        let prefix = node.inner.borrow().prefix.clone();
        if prefix.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
            return Err(FileProblem::failed("path too long"));
        }
        let resolved = resolve(resolver, fd.as_fd(), &prefix, PathMode::Folder)?;
        let next_fd = fcntl_dupfd_cloexec(&resolved.fd, 0).map_err(errno_to_problem)?;
        {
            let mut inner = node.inner.borrow_mut();
            inner.fd = resolved.fd;
            inner.pending = false;
            inner.parent = None;
        }
        fd = next_fd;
    }
    Ok(fd)
}

fn sync_fd(fd: BorrowedFd<'_>) -> Result<(), FileProblem> {
    #[cfg(target_os = "macos")]
    {
        match rustix::fs::fcntl_fullfsync(fd) {
            Ok(()) => return Ok(()),
            Err(Errno::NOTSUP | Errno::INVAL | Errno::NOTTY) => {}
            Err(e) => return Err(errno_to_problem(e)),
        }
    }
    loop {
        match rustix::fs::fsync(fd) {
            Ok(()) => return Ok(()),
            Err(Errno::INTR) => continue,
            Err(e) => return Err(errno_to_problem(e)),
        }
    }
}

fn sync_dir(dir_fd: BorrowedFd<'_>) -> Result<(), FileProblem> {
    let syncable = openat(
        dir_fd,
        ".",
        OFlags::RDONLY | OFlags::DIRECTORY | OFlags::CLOEXEC,
        Mode::empty(),
    )
    .map_err(errno_to_problem)?;
    sync_fd(syncable.as_fd())
}

fn open_strict_leaf(
    parent_fd: BorrowedFd<'_>,
    leaf: &str,
    access: OFlags,
) -> Result<(OwnedFd, bool), FileProblem> {
    let common = OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK | OFlags::NOCTTY;
    for _attempt in 0..4 {
        match statat(parent_fd, leaf, AtFlags::SYMLINK_NOFOLLOW) {
            Ok(st) => {
                let ft = FileType::from_raw_mode(st.st_mode);
                if ft == FileType::Symlink {
                    return Err(FileProblem::not_allowed(true));
                }
                if ft == FileType::Directory {
                    return Err(FileProblem::failed("is a folder"));
                }
                if ft != FileType::RegularFile {
                    return Err(FileProblem::failed("not a regular file"));
                }
                let fd = match openat(parent_fd, leaf, access | common, Mode::empty()) {
                    Ok(fd) => fd,
                    Err(Errno::LOOP) => return Err(FileProblem::not_allowed(true)),
                    Err(Errno::NOENT) => continue,
                    Err(e) => return Err(errno_to_problem(e)),
                };
                let st2 = fstat(&fd).map_err(errno_to_problem)?;
                let ft2 = FileType::from_raw_mode(st2.st_mode);
                if ft2 != FileType::RegularFile {
                    return Err(FileProblem::failed(if ft2 == FileType::Directory {
                        "is a folder"
                    } else {
                        "not a regular file"
                    }));
                }
                return Ok((fd, false));
            }
            Err(Errno::NOENT) => {
                match openat(
                    parent_fd,
                    leaf,
                    access | OFlags::CREATE | OFlags::EXCL | common,
                    Mode::from_raw_mode(0o644),
                ) {
                    Ok(fd) => return Ok((fd, true)),
                    Err(Errno::EXIST) => continue,
                    Err(Errno::LOOP) => return Err(FileProblem::not_allowed(true)),
                    Err(e) => return Err(errno_to_problem(e)),
                }
            }
            Err(e) => return Err(errno_to_problem(e)),
        }
    }
    Err(FileProblem::failed("changed while being used"))
}

fn write_all_fd(fd: BorrowedFd<'_>, bytes: &[u8], append: bool) -> Result<(), FileProblem> {
    if !append {
        loop {
            match ftruncate(fd, 0) {
                Ok(()) => break,
                Err(Errno::INTR) => continue,
                Err(e) => return Err(errno_to_problem(e)),
            }
        }
    }
    let mut at = 0usize;
    while at < bytes.len() {
        match write(fd, &bytes[at..]) {
            Ok(0) => return Err(errno_to_problem(Errno::IO)),
            Ok(n) => at += n,
            Err(Errno::INTR) => continue,
            Err(e) => return Err(errno_to_problem(e)),
        }
    }
    Ok(())
}

fn read_all_fd(
    heap: &Heap,
    types: &TypeTable,
    fd: BorrowedFd<'_>,
    as_text: bool,
    result_ty: u32,
    path_val: &Value,
) -> Result<Value, HostError> {
    let mut scratch = ScratchGuard::new(heap.budget());
    let mut bytes = Vec::new();
    let max_bytes = MO_LIMIT_VALUE_BYTES as usize;
    let mut chunk = [0u8; 8192];

    loop {
        if bytes.len() == max_bytes {
            let mut extra = [0u8; 1];
            match read(fd, &mut extra) {
                Ok(0) => break,
                Ok(_) => {
                    return Err(HostError::OutOfMemory {
                        value_bytes_limit: true,
                    });
                }
                Err(Errno::INTR) => continue,
                Err(e) => {
                    return make_file_error(heap, types, result_ty, path_val, errno_to_problem(e));
                }
            }
        }
        let room = (max_bytes - bytes.len()).min(chunk.len());
        match read(fd, &mut chunk[..room]) {
            Ok(0) => break,
            Ok(n) => {
                scratch.charge(n)?;
                bytes.extend_from_slice(&chunk[..n]);
            }
            Err(Errno::INTR) => continue,
            Err(e) => {
                return make_file_error(heap, types, result_ty, path_val, errno_to_problem(e));
            }
        }
    }
    drop(scratch);

    if as_text {
        match String::from_utf8(bytes) {
            Ok(s) => {
                let v = heap.alloc_text(s)?;
                make_file_ok(heap, types, result_ty, v)
            }
            Err(_) => make_file_error(heap, types, result_ty, path_val, FileProblem::not_text()),
        }
    } else {
        let v = heap.alloc_bytes(bytes)?;
        make_file_ok(heap, types, result_ty, v)
    }
}

fn real_listing(
    heap: &Heap,
    types: &TypeTable,
    fd: BorrowedFd<'_>,
    result_ty: u32,
    path_val: &Value,
) -> Result<Value, HostError> {
    let copy = match openat(
        fd,
        ".",
        OFlags::RDONLY | OFlags::DIRECTORY | OFlags::CLOEXEC,
        Mode::empty(),
    ) {
        Ok(c) => c,
        Err(e) => return make_file_error(heap, types, result_ty, path_val, errno_to_problem(e)),
    };

    let mut buf: Vec<MaybeUninit<u8>> = Vec::with_capacity(8192);
    buf.resize_with(8192, MaybeUninit::uninit);
    let mut items: Vec<(String, EntryKind)> = Vec::new();
    let mut scratch = ScratchGuard::new(heap.budget());

    'read: loop {
        let mut need_grow = false;
        {
            let mut iter = RawDir::new(&copy, &mut buf[..]);
            while let Some(entry_res) = iter.next() {
                let entry = match entry_res {
                    Ok(e) => e,
                    Err(Errno::INVAL) => {
                        need_grow = true;
                        break;
                    }
                    Err(e) => {
                        return make_file_error(
                            heap,
                            types,
                            result_ty,
                            path_val,
                            errno_to_problem(e),
                        );
                    }
                };
                let raw_name = entry.file_name().to_bytes();
                if raw_name == b"." || raw_name == b".." {
                    continue;
                }
                let st = match statat(&copy, entry.file_name(), AtFlags::SYMLINK_NOFOLLOW) {
                    Ok(st) => st,
                    Err(Errno::NOENT) => continue,
                    Err(e) => {
                        return make_file_error(
                            heap,
                            types,
                            result_ty,
                            path_val,
                            errno_to_problem(e),
                        );
                    }
                };
                let Ok(name_str) = std::str::from_utf8(raw_name) else {
                    return make_file_error(
                        heap,
                        types,
                        result_ty,
                        path_val,
                        FileProblem::failed("folder contains a name that is not text"),
                    );
                };
                let kind = match FileType::from_raw_mode(st.st_mode) {
                    FileType::RegularFile => EntryKind::File,
                    FileType::Directory => EntryKind::Folder,
                    FileType::Symlink => EntryKind::Link,
                    _ => EntryKind::Other,
                };
                scratch.charge(name_str.len() + 32)?;
                items.push((name_str.to_string(), kind));
            }
        }
        if need_grow {
            let new_len = buf.len().saturating_mul(2);
            buf.resize_with(new_len, MaybeUninit::uninit);
            continue 'read;
        }
        break;
    }
    drop(scratch);
    make_names_or_entries(heap, types, result_ty, items)
}

#[expect(
    clippy::too_many_arguments,
    reason = "internal host file helper forwards VM and host state"
)]
fn narrow_real_folder(
    heap: &Heap,
    types: &TypeTable,
    resolver: &mut ResolverState,
    open_folders: &Rc<Cell<u32>>,
    original: &Rc<RealFolder>,
    op: HostOpId,
    result_ty: u32,
    path_res: Result<String, FileProblem>,
) -> Result<Value, HostError> {
    if u64::from(open_folders.get()) >= MO_LIMIT_OPEN_FOLDERS {
        return Err(HostError::Bug {
            reason: "too many folders open at once",
            detail: None,
        });
    }
    let readonly = op == HostOpId::FilesReadOnly;
    let (orig_pending, orig_forbidden) = {
        let inner = original.inner.borrow();
        (inner.pending, inner.forbidden)
    };

    let mut resolved_fd: Option<OwnedFd> = None;
    let mut pending = orig_pending || !readonly;
    let mut containment = false;

    if !orig_forbidden {
        match &path_res {
            Err(p) => {
                containment = p.containment;
            }
            Ok(path) => match resolve_folder_root(resolver, original) {
                Ok(parent_fd) => {
                    if readonly {
                        resolved_fd = Some(parent_fd);
                        pending = false;
                    } else {
                        match resolve(resolver, parent_fd.as_fd(), path, PathMode::Folder) {
                            Ok(r) => {
                                resolved_fd = Some(r.fd);
                                pending = false;
                            }
                            Err(p) => {
                                containment = p.containment;
                            }
                        }
                    }
                }
                Err(p) => {
                    containment = p.containment;
                }
            },
        }
    }

    let fd = match resolved_fd {
        Some(fd) => fd,
        None => {
            let inner = original.inner.borrow();
            fcntl_dupfd_cloexec(&inner.fd, 0).map_err(|_| HostError::Bug {
                reason: "too many folders open at once",
                detail: None,
            })?
        }
    };

    let forbidden = orig_forbidden || containment;
    let prefix = if pending && !readonly {
        path_res.unwrap_or_default()
    } else {
        String::new()
    };

    let new_folder = RealFolder {
        inner: RefCell::new(RealFolderInner {
            fd,
            pending,
            forbidden,
            parent: if pending {
                Some(Rc::clone(original))
            } else {
                None
            },
            prefix,
        }),
        open_folders: Rc::clone(open_folders),
    };
    open_folders.set(open_folders.get() + 1);

    #[expect(
        clippy::expect_used,
        reason = "verified bytecode guarantees narrowed result is a capability type"
    )]
    let cap_kind = types
        .get(result_ty)
        .capability
        .expect("narrowed result is capability");
    let cap_obj = crate::FolderCap::Real(Rc::new(new_folder));
    Ok(heap.alloc_capability(cap_kind, result_ty, Rc::new(cap_obj))?)
}

#[expect(
    clippy::too_many_arguments,
    reason = "internal host file helper forwards VM and host state"
)]
fn real_write_bytes_or_open(
    heap: &Heap,
    types: &TypeTable,
    resolver: &mut ResolverState,
    open_files: &Rc<Cell<u32>>,
    folder: &Rc<RealFolder>,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
    path_res: Result<String, FileProblem>,
) -> Result<Value, HostError> {
    let path = match path_res {
        Ok(p) => p,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };
    let root_fd = match resolve_folder_root(resolver, folder) {
        Ok(fd) => fd,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };
    if path.is_empty() {
        return make_file_error(
            heap,
            types,
            result_ty,
            &args[1],
            FileProblem::failed("is a folder"),
        );
    }
    let resolved = match resolve(resolver, root_fd.as_fd(), &path, PathMode::StrictParent) {
        Ok(r) => r,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };
    let parent_fd = resolved.fd;
    let leaf = resolved.name;

    if op == HostOpId::FilesOpen {
        match statat(&parent_fd, leaf.as_str(), AtFlags::SYMLINK_NOFOLLOW) {
            Ok(st) => {
                let ft = FileType::from_raw_mode(st.st_mode);
                if ft == FileType::Symlink {
                    return make_file_error(
                        heap,
                        types,
                        result_ty,
                        &args[1],
                        FileProblem::not_allowed(true),
                    );
                }
                if ft == FileType::Directory {
                    return make_file_error(
                        heap,
                        types,
                        result_ty,
                        &args[1],
                        FileProblem::failed("is a folder"),
                    );
                }
                if ft != FileType::RegularFile {
                    return make_file_error(
                        heap,
                        types,
                        result_ty,
                        &args[1],
                        FileProblem::failed("not a regular file"),
                    );
                }
            }
            Err(Errno::NOENT) => {}
            Err(e) => {
                return make_file_error(heap, types, result_ty, &args[1], errno_to_problem(e));
            }
        }
        if u64::from(open_files.get()) >= MO_LIMIT_OPEN_FILES {
            return Err(HostError::Bug {
                reason: "too many files open at once",
                detail: None,
            });
        }
    }

    let access = if op == HostOpId::FilesOpen {
        OFlags::RDWR
    } else {
        OFlags::WRONLY
    };
    let (file_fd, created) = match open_strict_leaf(parent_fd.as_fd(), &leaf, access) {
        Ok(pair) => pair,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };

    if op == HostOpId::FilesWriteBytes {
        let res = write_all_fd(file_fd.as_fd(), args[2].as_bytes(), false);
        drop(file_fd);
        let res = match res {
            Ok(()) if created => sync_dir(parent_fd.as_fd()),
            other => other,
        };
        return match res {
            Ok(()) => make_file_ok(heap, types, result_ty, Value::Unit),
            Err(p) => make_file_error(heap, types, result_ty, &args[1], p),
        };
    }

    if created && let Err(p) = sync_dir(parent_fd.as_fd()) {
        return make_file_error(heap, types, result_ty, &args[1], p);
    }
    drop(parent_fd);

    let ok_tag = find_case_tag(types, result_ty, "Ok");
    let file_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
    let handle = crate::OpenFileCap::Real(RealOpenFile {
        fd: file_fd,
        path: args[1].clone(),
        open_files: Rc::clone(open_files),
    });
    open_files.set(open_files.get() + 1);
    let cap_val = heap.alloc_capability(Capability::File, file_ty, Rc::new(handle))?;
    make_file_ok(heap, types, result_ty, cap_val)
}

fn real_rename(
    heap: &Heap,
    types: &TypeTable,
    resolver: &mut ResolverState,
    folder: &Rc<RealFolder>,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    let from_path = match prepare_real_path(args[1].as_text().as_bytes()) {
        Ok(p) => p,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };
    let to_path = match prepare_real_path(args[2].as_text().as_bytes()) {
        Ok(p) => p,
        Err(p) => return make_file_error(heap, types, result_ty, &args[2], p),
    };
    let root_fd = match resolve_folder_root(resolver, folder) {
        Ok(fd) => fd,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };
    if from_path.is_empty() {
        return make_file_error(
            heap,
            types,
            result_ty,
            &args[1],
            FileProblem::failed("is a folder"),
        );
    }
    if to_path.is_empty() {
        return make_file_error(
            heap,
            types,
            result_ty,
            &args[2],
            FileProblem::failed("is a folder"),
        );
    }
    let from_res = match resolve(
        resolver,
        root_fd.as_fd(),
        &from_path,
        PathMode::StrictParent,
    ) {
        Ok(r) => r,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };
    let from_st = match statat(
        &from_res.fd,
        from_res.name.as_str(),
        AtFlags::SYMLINK_NOFOLLOW,
    ) {
        Ok(st) => st,
        Err(e) => return make_file_error(heap, types, result_ty, &args[1], errno_to_problem(e)),
    };
    match FileType::from_raw_mode(from_st.st_mode) {
        FileType::Symlink => {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[1],
                FileProblem::not_allowed(true),
            );
        }
        FileType::Directory => {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[1],
                FileProblem::failed("is a folder"),
            );
        }
        FileType::RegularFile => {}
        _ => {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[1],
                FileProblem::failed("not a regular file"),
            );
        }
    }

    let to_res = match resolve(resolver, root_fd.as_fd(), &to_path, PathMode::StrictParent) {
        Ok(r) => r,
        Err(p) => return make_file_error(heap, types, result_ty, &args[2], p),
    };
    match statat(&to_res.fd, to_res.name.as_str(), AtFlags::SYMLINK_NOFOLLOW) {
        Ok(st) => match FileType::from_raw_mode(st.st_mode) {
            FileType::Symlink => {
                return make_file_error(
                    heap,
                    types,
                    result_ty,
                    &args[2],
                    FileProblem::not_allowed(true),
                );
            }
            FileType::Directory => {
                return make_file_error(
                    heap,
                    types,
                    result_ty,
                    &args[2],
                    FileProblem::failed("is a folder"),
                );
            }
            FileType::RegularFile => {}
            _ => {
                return make_file_error(
                    heap,
                    types,
                    result_ty,
                    &args[2],
                    FileProblem::failed("not a regular file"),
                );
            }
        },
        Err(Errno::NOENT) => {}
        Err(e) => return make_file_error(heap, types, result_ty, &args[2], errno_to_problem(e)),
    }

    if let Err(e) = renameat(
        &from_res.fd,
        from_res.name.as_str(),
        &to_res.fd,
        to_res.name.as_str(),
    ) {
        return make_file_error(heap, types, result_ty, &args[2], errno_to_problem(e));
    }
    if let Err(p) = sync_dir(to_res.fd.as_fd()) {
        return make_file_error(heap, types, result_ty, &args[2], p);
    }
    let same_dir = match (fstat(&from_res.fd), fstat(&to_res.fd)) {
        (Ok(a), Ok(b)) => a.st_dev == b.st_dev && a.st_ino == b.st_ino,
        (Err(e), _) | (_, Err(e)) => {
            return make_file_error(heap, types, result_ty, &args[2], errno_to_problem(e));
        }
    };
    if !same_dir && let Err(p) = sync_dir(from_res.fd.as_fd()) {
        return make_file_error(heap, types, result_ty, &args[2], p);
    }
    make_file_ok(heap, types, result_ty, Value::Unit)
}

#[expect(
    clippy::too_many_arguments,
    reason = "internal host file dispatch forwards VM and host state"
)]
pub(crate) fn call_real_folder(
    heap: &Heap,
    types: &TypeTable,
    resolver: &mut ResolverState,
    open_folders: &Rc<Cell<u32>>,
    open_files: &Rc<Cell<u32>>,
    folder: &Rc<RealFolder>,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    if op == HostOpId::FilesRename {
        return real_rename(heap, types, resolver, folder, result_ty, args);
    }
    let path_res = if op == HostOpId::FilesReadOnly {
        Ok(String::new())
    } else {
        prepare_real_path(args[1].as_text().as_bytes())
    };
    if matches!(
        op,
        HostOpId::FilesReadOnly | HostOpId::FilesWithin | HostOpId::ReadfilesWithin
    ) {
        return narrow_real_folder(
            heap,
            types,
            resolver,
            open_folders,
            folder,
            op,
            result_ty,
            path_res,
        );
    }
    if matches!(op, HostOpId::FilesWriteBytes | HostOpId::FilesOpen) {
        return real_write_bytes_or_open(
            heap, types, resolver, open_files, folder, op, result_ty, args, path_res,
        );
    }

    let is_exists = matches!(op, HostOpId::FilesExists | HostOpId::ReadfilesExists);
    let mode = match op {
        HostOpId::FilesExists | HostOpId::ReadfilesExists => PathMode::Exists,
        HostOpId::FilesWriteText => PathMode::Write,
        HostOpId::FilesAppendText => PathMode::Append,
        HostOpId::FilesRemove => PathMode::Remove,
        HostOpId::FilesMakeFolder => PathMode::MakeFolder,
        HostOpId::FilesList
        | HostOpId::ReadfilesList
        | HostOpId::FilesEntries
        | HostOpId::ReadfilesEntries => PathMode::Folder,
        HostOpId::FilesReadText
        | HostOpId::ReadfilesReadText
        | HostOpId::FilesReadBytes
        | HostOpId::ReadfilesReadBytes => PathMode::Read,
        _ => unreachable!("unexpected real folder op {op:?}"),
    };

    let resolved = path_res.and_then(|path| {
        let root_fd = resolve_folder_root(resolver, folder)?;
        resolve(resolver, root_fd.as_fd(), &path, mode)
    });

    if is_exists {
        return Ok(Value::Bool(resolved.is_ok()));
    }
    let resolved = match resolved {
        Ok(r) => r,
        Err(p) => return make_file_error(heap, types, result_ty, &args[1], p),
    };

    match mode {
        PathMode::Read => {
            let as_text = matches!(op, HostOpId::FilesReadText | HostOpId::ReadfilesReadText);
            read_all_fd(
                heap,
                types,
                resolved.fd.as_fd(),
                as_text,
                result_ty,
                &args[1],
            )
        }
        PathMode::Folder => real_listing(heap, types, resolved.fd.as_fd(), result_ty, &args[1]),
        PathMode::Write | PathMode::Append => {
            let res = write_all_fd(
                resolved.fd.as_fd(),
                args[2].as_text().as_bytes(),
                mode == PathMode::Append,
            );
            match res {
                Ok(()) => make_file_ok(heap, types, result_ty, Value::Unit),
                Err(p) => make_file_error(heap, types, result_ty, &args[1], p),
            }
        }
        PathMode::Remove => {
            if FileType::from_raw_mode(resolved.stat.st_mode) == FileType::Directory {
                return make_file_error(
                    heap,
                    types,
                    result_ty,
                    &args[1],
                    FileProblem::failed("is a folder"),
                );
            }
            match unlinkat(&resolved.fd, resolved.name.as_str(), AtFlags::empty()) {
                Ok(()) => make_file_ok(heap, types, result_ty, Value::Unit),
                Err(e) => make_file_error(heap, types, result_ty, &args[1], errno_to_problem(e)),
            }
        }
        PathMode::MakeFolder => make_file_ok(heap, types, result_ty, Value::Unit),
        PathMode::Exists | PathMode::StrictParent => unreachable!(),
    }
}

pub(crate) fn call_real_file(
    heap: &Heap,
    types: &TypeTable,
    file: &RealOpenFile,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    match op {
        HostOpId::FilesSync => match sync_fd(file.fd.as_fd()) {
            Ok(()) => make_file_ok(heap, types, result_ty, Value::Unit),
            Err(p) => make_file_error(heap, types, result_ty, &file.path, p),
        },
        HostOpId::FilesSize => {
            let st = match fstat(&file.fd) {
                Ok(s) => s,
                Err(e) => {
                    return make_file_error(
                        heap,
                        types,
                        result_ty,
                        &file.path,
                        errno_to_problem(e),
                    );
                }
            };
            if st.st_size < 0 {
                return make_file_error(
                    heap,
                    types,
                    result_ty,
                    &file.path,
                    FileProblem::failed("negative file size"),
                );
            }
            make_file_ok(heap, types, result_ty, Value::Int(st.st_size))
        }
        HostOpId::FilesTruncate => {
            let size = args[1].as_int();
            if size < 0 {
                return Err(HostError::Bug {
                    reason: "negative file size",
                    detail: None,
                });
            }
            loop {
                match ftruncate(&file.fd, size as u64) {
                    Ok(()) => return make_file_ok(heap, types, result_ty, Value::Unit),
                    Err(Errno::INTR) => continue,
                    Err(e) => {
                        return make_file_error(
                            heap,
                            types,
                            result_ty,
                            &file.path,
                            errno_to_problem(e),
                        );
                    }
                }
            }
        }
        HostOpId::FilesWriteAt => {
            let offset = args[1].as_int();
            if offset < 0 {
                return Err(HostError::Bug {
                    reason: "negative file offset",
                    detail: None,
                });
            }
            let bytes = args[2].as_bytes();
            if (bytes.len() as u64) > (i64::MAX as u64) - (offset as u64) {
                return make_file_error(
                    heap,
                    types,
                    result_ty,
                    &file.path,
                    errno_to_problem(Errno::FBIG),
                );
            }
            let mut at = 0usize;
            while at < bytes.len() {
                match pwrite(&file.fd, &bytes[at..], (offset as u64) + (at as u64)) {
                    Ok(0) => {
                        return make_file_error(
                            heap,
                            types,
                            result_ty,
                            &file.path,
                            errno_to_problem(Errno::IO),
                        );
                    }
                    Ok(n) => at += n,
                    Err(Errno::INTR) => continue,
                    Err(e) => {
                        return make_file_error(
                            heap,
                            types,
                            result_ty,
                            &file.path,
                            errno_to_problem(e),
                        );
                    }
                }
            }
            make_file_ok(heap, types, result_ty, Value::Unit)
        }
        HostOpId::FilesReadAt => {
            let offset = args[1].as_int();
            let count = args[2].as_int();
            if offset < 0 {
                return Err(HostError::Bug {
                    reason: "negative file offset",
                    detail: None,
                });
            }
            if count < 0 || (count as u64) > MO_LIMIT_FILE_IO_BYTES {
                return Err(HostError::Bug {
                    reason: "read count outside 0 to the read limit",
                    detail: None,
                });
            }
            if count == 0 {
                let empty = heap.alloc_bytes(Vec::new())?;
                return make_file_ok(heap, types, result_ty, empty);
            }
            let want = count as usize;
            let mut scratch = ScratchGuard::new(heap.budget());
            scratch.charge(want)?;
            let mut buf = vec![0u8; want];
            let mut got = 0usize;
            while got < want {
                let avail = (i64::MAX as u64) - (offset as u64) - (got as u64);
                if avail == 0 {
                    break;
                }
                let remaining = want - got;
                let chunk = if (remaining as u64) < avail {
                    remaining
                } else {
                    avail as usize
                };
                match pread(
                    &file.fd,
                    &mut buf[got..got + chunk],
                    (offset as u64) + (got as u64),
                ) {
                    Ok(0) => break,
                    Ok(n) => got += n,
                    Err(Errno::INTR) => continue,
                    Err(e) => {
                        return make_file_error(
                            heap,
                            types,
                            result_ty,
                            &file.path,
                            errno_to_problem(e),
                        );
                    }
                }
            }
            buf.truncate(got);
            drop(scratch);
            let out = heap.alloc_bytes(buf)?;
            make_file_ok(heap, types, result_ty, out)
        }
        _ => unreachable!("unexpected real file op {op:?}"),
    }
}
