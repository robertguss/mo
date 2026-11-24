use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use mo_bytecode::Capability;
use mo_bytecode::types::TypeTable;
use mo_def::HostOpId;
use mo_def::limits::{MO_LIMIT_FILE_IO_BYTES, MO_LIMIT_OPEN_FILES, MO_LIMIT_VALUE_BYTES};
use mo_heap::{Heap, HeapBudget, ScratchGuard, Value};

use crate::HostError;
use crate::files_common::{
    EntryKind, FileProblem, find_case_tag, join_path, make_file_error, make_file_ok,
    make_names_or_entries, normalize_path, plain_relative,
};

const META_ENTRY_OVERHEAD: usize = 48;

pub(crate) struct FakeFsState {
    files: HashMap<String, Value>,
    folders: HashSet<String>,
    budget: HeapBudget,
    charged_meta: usize,
}

impl FakeFsState {
    fn new(budget: HeapBudget) -> Self {
        Self {
            files: HashMap::new(),
            folders: HashSet::new(),
            budget,
            charged_meta: 0,
        }
    }

    fn charge_meta(&mut self, bytes: usize) -> Result<(), HostError> {
        self.budget.charge(bytes)?;
        self.charged_meta = self.charged_meta.saturating_add(bytes);
        Ok(())
    }

    fn release_meta(&mut self, bytes: usize) {
        let n = bytes.min(self.charged_meta);
        self.budget.release(n);
        self.charged_meta -= n;
    }

    fn folder_exists(&self, path: &str) -> bool {
        path.is_empty() || self.folders.contains(path)
    }

    fn parent_folder_exists(&self, path: &str) -> bool {
        match path.rfind('/') {
            Some(slash) => self.folder_exists(&path[..slash]),
            None => true,
        }
    }

    fn blocked(&self, root_prefix: &str, full_path: &str) -> bool {
        for key in self.files.keys() {
            if (!root_prefix.is_empty() && key == root_prefix)
                || (key.len() < full_path.len()
                    && full_path.as_bytes()[key.len()] == b'/'
                    && full_path.starts_with(key.as_str()))
            {
                return true;
            }
        }
        false
    }
}

impl Drop for FakeFsState {
    fn drop(&mut self) {
        if self.charged_meta != 0 {
            self.budget.release(self.charged_meta);
            self.charged_meta = 0;
        }
    }
}

#[derive(Clone)]
pub(crate) struct FakeFolderCap {
    pub(crate) fs: Rc<RefCell<FakeFsState>>,
    pub(crate) prefix: Option<String>,
}

pub(crate) struct FakeOpenFileCap {
    pub(crate) fs: Rc<RefCell<FakeFsState>>,
    pub(crate) key: String,
    pub(crate) open_files: Rc<Cell<u32>>,
}

impl Drop for FakeOpenFileCap {
    fn drop(&mut self) {
        let cur = self.open_files.get();
        debug_assert!(cur != 0);
        self.open_files.set(cur.saturating_sub(1));
    }
}

fn collect_prefixes(path: &str, include_self: bool, out: &mut Vec<String>) {
    let bytes = path.as_bytes();
    for i in 1..=bytes.len() {
        if (i == bytes.len() && !include_self) || (i < bytes.len() && bytes[i] != b'/') {
            continue;
        }
        out.push(path[..i].to_string());
    }
}

pub(crate) fn files_in_memory(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    map_val: &Value,
) -> Result<Value, HostError> {
    let map = map_val.as_map();

    // Pass 1: validate every key is a plain relative path.
    for (k, _) in map.iter() {
        let key = k.as_text();
        if !plain_relative(key.as_bytes()) {
            return Err(HostError::Bug {
                reason: "fake has a key that is not a plain relative path",
                detail: Some(key.to_string()),
            });
        }
    }

    // Collect implied folders and verify no file is also a folder.
    let mut folders = HashSet::new();
    let mut prefixes = Vec::new();
    for (k, _) in map.iter() {
        prefixes.clear();
        collect_prefixes(k.as_text(), false, &mut prefixes);
        for p in prefixes.drain(..) {
            folders.insert(p);
        }
    }
    for (k, _) in map.iter() {
        let key = k.as_text();
        if folders.contains(key) {
            return Err(HostError::Bug {
                reason: "fake has a file that is also a folder",
                detail: Some(key.to_string()),
            });
        }
    }

    let mut fs = FakeFsState::new(heap.budget().clone());
    for folder in folders {
        fs.charge_meta(META_ENTRY_OVERHEAD + folder.len())?;
        fs.folders.insert(folder);
    }
    for (k, v) in map.iter() {
        let key = k.as_text().to_string();
        let raw_bytes = heap.alloc_bytes(v.as_text().as_bytes().to_vec())?;
        fs.charge_meta(META_ENTRY_OVERHEAD + key.len())?;
        fs.files.insert(key, raw_bytes);
    }

    let cap = crate::FolderCap::Fake(FakeFolderCap {
        fs: Rc::new(RefCell::new(fs)),
        prefix: Some(String::new()),
    });
    let _ = types;
    Ok(heap.alloc_capability(Capability::Files, result_ty, Rc::new(cap))?)
}

pub(crate) fn call_fake_folder(
    heap: &Heap,
    types: &TypeTable,
    open_files: &Rc<Cell<u32>>,
    folder: &FakeFolderCap,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    if op == HostOpId::FilesReadOnly {
        let derived = crate::FolderCap::Fake(FakeFolderCap {
            fs: Rc::clone(&folder.fs),
            prefix: folder.prefix.clone(),
        });
        return Ok(heap.alloc_capability(Capability::ReadFiles, result_ty, Rc::new(derived))?);
    }

    if op == HostOpId::FilesRename {
        return fake_rename(heap, types, folder, result_ty, args);
    }

    let raw_arg = args[1].as_text().as_bytes();
    let normalized = match (&folder.prefix, normalize_path(raw_arg)) {
        (Some(root_prefix), Ok(norm)) => {
            let joined = join_path(root_prefix, &norm);
            if (joined.len() as u64) > MO_LIMIT_VALUE_BYTES {
                return Err(HostError::OutOfMemory {
                    value_bytes_limit: false,
                });
            }
            Ok((root_prefix.as_str(), joined))
        }
        (None, _) => Err(FileProblem::not_allowed(false)),
        (Some(_), Err(p)) => Err(p),
    };

    if matches!(op, HostOpId::FilesWithin | HostOpId::ReadfilesWithin) {
        #[expect(
            clippy::expect_used,
            reason = "verified bytecode guarantees within returns a capability type"
        )]
        let cap_kind = types
            .get(result_ty)
            .capability
            .expect("within result is capability");
        let derived = crate::FolderCap::Fake(FakeFolderCap {
            fs: Rc::clone(&folder.fs),
            prefix: normalized.ok().map(|(_, full)| full),
        });
        return Ok(heap.alloc_capability(cap_kind, result_ty, Rc::new(derived))?);
    }

    let is_exists = matches!(op, HostOpId::FilesExists | HostOpId::ReadfilesExists);
    let (root_prefix, full_path) = match normalized {
        Ok(pair) => pair,
        Err(problem) => {
            if is_exists {
                return Ok(Value::Bool(false));
            }
            return make_file_error(heap, types, result_ty, &args[1], problem);
        }
    };

    let (block, root_folder, is_folder, existing_val) = {
        let fs = folder.fs.borrow();
        let block = fs.blocked(root_prefix, &full_path);
        let root_folder = fs.folder_exists(root_prefix);
        let is_folder = fs.folder_exists(&full_path);
        let existing_val = fs.files.get(&full_path).cloned();
        (block, root_folder, is_folder, existing_val)
    };
    let is_file = existing_val.is_some();

    if is_exists {
        return Ok(Value::Bool(!block && root_folder && (is_folder || is_file)));
    }

    let is_directory_op = matches!(
        op,
        HostOpId::FilesMakeFolder
            | HostOpId::FilesList
            | HostOpId::ReadfilesList
            | HostOpId::FilesEntries
            | HostOpId::ReadfilesEntries
    );
    if block || (is_directory_op && is_file) {
        return make_file_error(
            heap,
            types,
            result_ty,
            &args[1],
            FileProblem::failed("not a folder"),
        );
    }
    if !root_folder {
        return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
    }
    if !is_directory_op && is_folder {
        return make_file_error(
            heap,
            types,
            result_ty,
            &args[1],
            FileProblem::failed("is a folder"),
        );
    }
    if is_directory_op && op != HostOpId::FilesMakeFolder && !is_folder {
        return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
    }

    match op {
        HostOpId::FilesList
        | HostOpId::ReadfilesList
        | HostOpId::FilesEntries
        | HostOpId::ReadfilesEntries => fake_listing(heap, types, folder, &full_path, result_ty),
        HostOpId::FilesMakeFolder => {
            let mut to_add = Vec::new();
            {
                let fs = folder.fs.borrow();
                let mut candidates = Vec::new();
                collect_prefixes(&full_path, true, &mut candidates);
                for c in candidates {
                    if !fs.folders.contains(&c) {
                        to_add.push(c);
                    }
                }
            }
            let extra_meta: usize = to_add.iter().map(|p| META_ENTRY_OVERHEAD + p.len()).sum();
            // Allocate Ok(Unit) and charge all folder metadata BEFORE mutating state.
            let ok_val = make_file_ok(heap, types, result_ty, Value::Unit)?;
            {
                let mut fs = folder.fs.borrow_mut();
                if extra_meta != 0 {
                    fs.charge_meta(extra_meta)?;
                }
                for p in to_add {
                    fs.folders.insert(p);
                }
            }
            Ok(ok_val)
        }
        HostOpId::FilesOpen => {
            let parent_ok = folder.fs.borrow().parent_folder_exists(&full_path);
            if !parent_ok {
                return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
            }
            if u64::from(open_files.get()) >= MO_LIMIT_OPEN_FILES {
                return Err(HostError::Bug {
                    reason: "too many files open at once",
                    detail: None,
                });
            }
            let empty_val = if !is_file {
                Some(heap.alloc_bytes(Vec::new())?)
            } else {
                None
            };
            let ok_tag = find_case_tag(types, result_ty, "Ok");
            let file_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
            let handle = crate::OpenFileCap::Fake(FakeOpenFileCap {
                fs: Rc::clone(&folder.fs),
                key: full_path.clone(),
                open_files: Rc::clone(open_files),
            });
            // Increment open_files before constructing capability so its Drop always balances it.
            open_files.set(open_files.get() + 1);
            let cap_val = heap.alloc_capability(Capability::File, file_ty, Rc::new(handle))?;
            let result = make_file_ok(heap, types, result_ty, cap_val)?;
            if let Some(empty) = empty_val {
                let mut fs = folder.fs.borrow_mut();
                if !fs.files.contains_key(&full_path) {
                    fs.charge_meta(META_ENTRY_OVERHEAD + full_path.len())?;
                    fs.files.insert(full_path, empty);
                }
            }
            Ok(result)
        }
        HostOpId::FilesWriteText | HostOpId::FilesAppendText | HostOpId::FilesWriteBytes => {
            let parent_ok = folder.fs.borrow().parent_folder_exists(&full_path);
            if !parent_ok {
                return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
            }
            let stored = match op {
                HostOpId::FilesWriteBytes => args[2].clone(),
                HostOpId::FilesWriteText => {
                    heap.alloc_bytes(args[2].as_text().as_bytes().to_vec())?
                }
                HostOpId::FilesAppendText => {
                    let prev = existing_val.as_ref().map_or(&[][..], |v| v.as_bytes());
                    let add = args[2].as_text().as_bytes();
                    let total =
                        prev.len()
                            .checked_add(add.len())
                            .ok_or(HostError::OutOfMemory {
                                value_bytes_limit: false,
                            })?;
                    if (total as u64) > MO_LIMIT_VALUE_BYTES {
                        return Err(HostError::OutOfMemory {
                            value_bytes_limit: false,
                        });
                    }
                    let mut scratch = ScratchGuard::new(heap.budget());
                    scratch.charge(total)?;
                    let mut combined = Vec::with_capacity(total);
                    combined.extend_from_slice(prev);
                    combined.extend_from_slice(add);
                    drop(scratch);
                    heap.alloc_bytes(combined)?
                }
                _ => unreachable!(),
            };
            let ok_val = make_file_ok(heap, types, result_ty, Value::Unit)?;
            {
                let mut fs = folder.fs.borrow_mut();
                if !fs.files.contains_key(&full_path) {
                    fs.charge_meta(META_ENTRY_OVERHEAD + full_path.len())?;
                }
                fs.files.insert(full_path, stored);
            }
            Ok(ok_val)
        }
        HostOpId::FilesRemove => {
            if !is_file {
                return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
            }
            let ok_val = make_file_ok(heap, types, result_ty, Value::Unit)?;
            {
                let mut fs = folder.fs.borrow_mut();
                if fs.files.remove(&full_path).is_some() {
                    fs.release_meta(META_ENTRY_OVERHEAD + full_path.len());
                }
            }
            Ok(ok_val)
        }
        HostOpId::FilesReadBytes | HostOpId::ReadfilesReadBytes => match existing_val {
            Some(val) => make_file_ok(heap, types, result_ty, val),
            None => make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found()),
        },
        HostOpId::FilesReadText | HostOpId::ReadfilesReadText => match existing_val {
            None => make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found()),
            Some(val) => match std::str::from_utf8(val.as_bytes()) {
                Ok(s) => {
                    let text_val = heap.alloc_text(s.to_string())?;
                    make_file_ok(heap, types, result_ty, text_val)
                }
                Err(_) => {
                    make_file_error(heap, types, result_ty, &args[1], FileProblem::not_text())
                }
            },
        },
        _ => unreachable!("unexpected fake folder op {op:?}"),
    }
}

fn fake_listing(
    heap: &Heap,
    types: &TypeTable,
    folder: &FakeFolderCap,
    path: &str,
    result_ty: u32,
) -> Result<Value, HostError> {
    let mut unique = HashSet::<String>::new();
    let items: Vec<(String, EntryKind)> = {
        let fs = folder.fs.borrow();
        let n = path.len();
        let collect = |key: &str, set: &mut HashSet<String>| {
            if key.len() <= n {
                return;
            }
            if n != 0 && (!key.starts_with(path) || key.as_bytes()[n] != b'/') {
                return;
            }
            let start = if n == 0 { 0 } else { n + 1 };
            let rest = &key[start..];
            let end = rest.find('/').unwrap_or(rest.len());
            set.insert(rest[..end].to_string());
        };
        for k in fs.files.keys() {
            collect(k, &mut unique);
        }
        for k in &fs.folders {
            collect(k, &mut unique);
        }
        unique
            .into_iter()
            .map(|name| {
                let full = join_path(path, &name);
                let kind = if fs.folders.contains(&full) {
                    EntryKind::Folder
                } else {
                    EntryKind::File
                };
                (name, kind)
            })
            .collect()
    };
    make_names_or_entries(heap, types, result_ty, items)
}

fn fake_rename(
    heap: &Heap,
    types: &TypeTable,
    folder: &FakeFolderCap,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    let Some(root_prefix) = folder.prefix.as_deref() else {
        return make_file_error(
            heap,
            types,
            result_ty,
            &args[1],
            FileProblem::not_allowed(false),
        );
    };
    let from_norm = match normalize_path(args[1].as_text().as_bytes()) {
        Ok(p) => p,
        Err(problem) => return make_file_error(heap, types, result_ty, &args[1], problem),
    };
    let to_norm = match normalize_path(args[2].as_text().as_bytes()) {
        Ok(p) => p,
        Err(problem) => return make_file_error(heap, types, result_ty, &args[2], problem),
    };
    let from_path = join_path(root_prefix, &from_norm);
    let to_path = join_path(root_prefix, &to_norm);
    if (from_path.len() as u64) > MO_LIMIT_VALUE_BYTES
        || (to_path.len() as u64) > MO_LIMIT_VALUE_BYTES
    {
        return Err(HostError::OutOfMemory {
            value_bytes_limit: false,
        });
    }

    let kept_val = {
        let fs = folder.fs.borrow();
        if fs.blocked(root_prefix, &from_path) {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[1],
                FileProblem::failed("not a folder"),
            );
        }
        if !fs.folder_exists(root_prefix) {
            return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
        }
        if fs.folder_exists(&from_path) {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[1],
                FileProblem::failed("is a folder"),
            );
        }
        let Some(val) = fs.files.get(&from_path).cloned() else {
            return make_file_error(heap, types, result_ty, &args[1], FileProblem::not_found());
        };
        if fs.blocked(root_prefix, &to_path) {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[2],
                FileProblem::failed("not a folder"),
            );
        }
        if fs.folder_exists(&to_path) {
            return make_file_error(
                heap,
                types,
                result_ty,
                &args[2],
                FileProblem::failed("is a folder"),
            );
        }
        if !fs.parent_folder_exists(&to_path) {
            return make_file_error(heap, types, result_ty, &args[2], FileProblem::not_found());
        }
        val
    };

    if from_path == to_path {
        return make_file_ok(heap, types, result_ty, Value::Unit);
    }

    let ok_val = make_file_ok(heap, types, result_ty, Value::Unit)?;
    {
        let mut fs = folder.fs.borrow_mut();
        let replacing = fs.files.contains_key(&to_path);
        if !replacing && to_path.len() > from_path.len() {
            fs.charge_meta(to_path.len() - from_path.len())?;
        }
        fs.files.remove(&from_path);
        fs.files.insert(to_path.clone(), kept_val);
        if replacing {
            fs.release_meta(META_ENTRY_OVERHEAD + from_path.len());
        } else if from_path.len() > to_path.len() {
            fs.release_meta(from_path.len() - to_path.len());
        }
    }
    Ok(ok_val)
}

pub(crate) fn call_fake_file(
    heap: &Heap,
    types: &TypeTable,
    file: &FakeOpenFileCap,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    if op == HostOpId::FilesSync {
        return make_file_ok(heap, types, result_ty, Value::Unit);
    }

    match op {
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
            let current = file.fs.borrow().files.get(&file.key).cloned();
            let Some(val) = current else {
                let key_val = heap.alloc_text(file.key.clone())?;
                return make_file_error(heap, types, result_ty, &key_val, FileProblem::not_found());
            };
            let bytes = val.as_bytes();
            let slice = if count > 0 && (offset as u64) < (bytes.len() as u64) {
                let start = offset as usize;
                let avail = bytes.len() - start;
                let take = (count as usize).min(avail);
                bytes[start..start + take].to_vec()
            } else {
                Vec::new()
            };
            let out = heap.alloc_bytes(slice)?;
            make_file_ok(heap, types, result_ty, out)
        }
        HostOpId::FilesSize => {
            let len = file
                .fs
                .borrow()
                .files
                .get(&file.key)
                .map(|v| v.as_bytes().len());
            let Some(len) = len else {
                let key_val = heap.alloc_text(file.key.clone())?;
                return make_file_error(heap, types, result_ty, &key_val, FileProblem::not_found());
            };
            make_file_ok(heap, types, result_ty, Value::Int(len as i64))
        }
        HostOpId::FilesWriteAt => {
            let offset = args[1].as_int();
            if offset < 0 {
                return Err(HostError::Bug {
                    reason: "negative file offset",
                    detail: None,
                });
            }
            let current = file.fs.borrow().files.get(&file.key).cloned();
            let Some(val) = current else {
                let key_val = heap.alloc_text(file.key.clone())?;
                return make_file_error(heap, types, result_ty, &key_val, FileProblem::not_found());
            };
            let src = args[2].as_bytes();
            if src.is_empty() {
                return make_file_ok(heap, types, result_ty, Value::Unit);
            }
            let off_u64 = offset as u64;
            if off_u64 > MO_LIMIT_VALUE_BYTES || (src.len() as u64) > MO_LIMIT_VALUE_BYTES - off_u64
            {
                return Err(HostError::OutOfMemory {
                    value_bytes_limit: false,
                });
            }
            let off = offset as usize;
            let cur = val.as_bytes();
            let new_len = (off + src.len()).max(cur.len());
            let mut scratch = ScratchGuard::new(heap.budget());
            scratch.charge(new_len)?;
            let mut buf = vec![0u8; new_len];
            buf[..cur.len()].copy_from_slice(cur);
            buf[off..off + src.len()].copy_from_slice(src);
            drop(scratch);
            let updated = heap.alloc_bytes(buf)?;
            let ok_val = make_file_ok(heap, types, result_ty, Value::Unit)?;
            file.fs.borrow_mut().files.insert(file.key.clone(), updated);
            Ok(ok_val)
        }
        HostOpId::FilesTruncate => {
            let size = args[1].as_int();
            if size < 0 {
                return Err(HostError::Bug {
                    reason: "negative file size",
                    detail: None,
                });
            }
            let current = file.fs.borrow().files.get(&file.key).cloned();
            let Some(val) = current else {
                let key_val = heap.alloc_text(file.key.clone())?;
                return make_file_error(heap, types, result_ty, &key_val, FileProblem::not_found());
            };
            if (size as u64) > MO_LIMIT_VALUE_BYTES {
                return Err(HostError::OutOfMemory {
                    value_bytes_limit: false,
                });
            }
            let target = size as usize;
            let cur = val.as_bytes();
            let updated = if target <= cur.len() {
                heap.alloc_bytes(cur[..target].to_vec())?
            } else {
                let mut scratch = ScratchGuard::new(heap.budget());
                scratch.charge(target)?;
                let mut buf = vec![0u8; target];
                buf[..cur.len()].copy_from_slice(cur);
                drop(scratch);
                heap.alloc_bytes(buf)?
            };
            let ok_val = make_file_ok(heap, types, result_ty, Value::Unit)?;
            file.fs.borrow_mut().files.insert(file.key.clone(), updated);
            Ok(ok_val)
        }
        _ => unreachable!("unexpected fake file op {op:?}"),
    }
}
