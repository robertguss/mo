use std::borrow::Cow;
use std::collections::VecDeque;

use mo_bytecode::TypeKind;
use mo_bytecode::types::TypeTable;
use mo_heap::{Heap, Value};

use crate::HostError;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum FileStatus {
    NotFound,
    NotAllowed,
    NotText,
    Failed(Cow<'static, str>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct FileProblem {
    pub status: FileStatus,
    pub containment: bool,
}

impl FileProblem {
    pub fn not_found() -> Self {
        Self {
            status: FileStatus::NotFound,
            containment: false,
        }
    }

    pub fn not_allowed(containment: bool) -> Self {
        Self {
            status: FileStatus::NotAllowed,
            containment,
        }
    }

    pub fn not_text() -> Self {
        Self {
            status: FileStatus::NotText,
            containment: false,
        }
    }

    pub fn failed(reason: impl Into<Cow<'static, str>>) -> Self {
        Self {
            status: FileStatus::Failed(reason.into()),
            containment: false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EntryKind {
    File,
    Folder,
    Link,
    Other,
}

impl EntryKind {
    fn case_name(self) -> &'static str {
        match self {
            EntryKind::File => "File",
            EntryKind::Folder => "Folder",
            EntryKind::Link => "Link",
            EntryKind::Other => "Other",
        }
    }
}

/// Normalizes a relative capability path, rejecting empty paths, NUL bytes, leading/trailing/
/// doubled slashes, and `..` escapes above the capability root.
pub(crate) fn normalize_path(bytes: &[u8]) -> Result<String, FileProblem> {
    if bytes.is_empty() || bytes.contains(&0) {
        return Err(FileProblem::not_allowed(true));
    }
    let mut out = Vec::with_capacity(bytes.len());
    let mut at = 0usize;
    let length = bytes.len();
    while at < length {
        let mut end = at;
        while end < length && bytes[end] != b'/' {
            end += 1;
        }
        let n = end - at;
        if n == 0 || (end == length - 1 && bytes[end] == b'/') {
            return Err(FileProblem::not_allowed(true));
        }
        let seg = &bytes[at..end];
        if seg == b".." {
            if out.is_empty() {
                return Err(FileProblem::not_allowed(true));
            }
            while let Some(&last) = out.last() {
                if last == b'/' {
                    out.pop();
                    break;
                }
                out.pop();
            }
        } else if seg != b"." {
            if !out.is_empty() {
                out.push(b'/');
            }
            out.extend_from_slice(seg);
        }
        at = end + 1;
    }
    #[expect(
        clippy::expect_used,
        reason = "normalized segments of valid UTF-8 input preserve valid UTF-8"
    )]
    Ok(String::from_utf8(out).expect("normalized slice of valid UTF-8 is valid UTF-8"))
}

/// Exact port of `src/host/real-files.c:prepare_real_path`.
pub(crate) fn prepare_real_path(bytes: &[u8]) -> Result<String, FileProblem> {
    if bytes.contains(&0) {
        return Err(FileProblem::not_allowed(true));
    }
    normalize_path(bytes)
}

/// Returns whether `bytes` is a plain relative path (no NUL bytes, empty/`.`/`..` segments).
pub(crate) fn plain_relative(bytes: &[u8]) -> bool {
    if bytes.contains(&0) {
        return false;
    }
    let mut start = 0usize;
    let length = bytes.len();
    for end in 0..=length {
        if end < length && bytes[end] != b'/' {
            continue;
        }
        let n = end - start;
        if n == 0 || (bytes[start] == b'.' && (n == 1 || (n == 2 && bytes[start + 1] == b'.'))) {
            return false;
        }
        start = end + 1;
    }
    true
}

/// Exact port of `src/host/files.c:mo_host_join`.
pub(crate) fn join_path(prefix: &str, path: &str) -> String {
    if prefix.is_empty() {
        path.to_string()
    } else if path.is_empty() {
        prefix.to_string()
    } else {
        let mut out = String::with_capacity(prefix.len() + 1 + path.len());
        out.push_str(prefix);
        out.push('/');
        out.push_str(path);
        out
    }
}

pub(crate) fn find_case_tag(types: &TypeTable, choice_ty: u32, name: &str) -> u32 {
    let t = types.get(choice_ty);
    debug_assert_eq!(t.kind, TypeKind::Choice);
    for (i, c) in t.cases.iter().enumerate() {
        if c.name == name {
            return i as u32;
        }
    }
    unreachable!("missing case {name} on choice type {choice_ty}");
}

pub(crate) fn make_file_ok(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    value: Value,
) -> Result<Value, HostError> {
    let ok_tag = find_case_tag(types, result_ty, "Ok");
    Ok(heap.make_case(types, result_ty, ok_tag, vec![value])?)
}

pub(crate) fn make_file_error(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    path_val: &Value,
    problem: FileProblem,
) -> Result<Value, HostError> {
    let err_tag = find_case_tag(types, result_ty, "Error");
    let err_ty = types.get(result_ty).cases[err_tag as usize].fields[0].ty;
    let err_val = match problem.status {
        FileStatus::NotFound => {
            let tag = find_case_tag(types, err_ty, "NotFound");
            heap.make_case(types, err_ty, tag, vec![path_val.clone()])?
        }
        FileStatus::NotAllowed => {
            let tag = find_case_tag(types, err_ty, "NotAllowed");
            heap.make_case(types, err_ty, tag, vec![path_val.clone()])?
        }
        FileStatus::NotText => {
            let tag = find_case_tag(types, err_ty, "NotText");
            heap.make_case(types, err_ty, tag, vec![path_val.clone()])?
        }
        FileStatus::Failed(reason) => {
            let tag = find_case_tag(types, err_ty, "Failed");
            let reason_val = heap.alloc_text(reason.into_owned())?;
            heap.make_case(types, err_ty, tag, vec![path_val.clone(), reason_val])?
        }
    };
    Ok(heap.make_case(types, result_ty, err_tag, vec![err_val])?)
}

pub(crate) fn make_names_or_entries(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    mut items: Vec<(String, EntryKind)>,
) -> Result<Value, HostError> {
    items.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    let ok_tag = find_case_tag(types, result_ty, "Ok");
    let list_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
    let elem_ty = types.get(list_ty).parameters[0];
    let is_records = types.get(elem_ty).kind == TypeKind::Record;

    let mut out_list = VecDeque::with_capacity(items.len());
    if is_records {
        #[expect(
            clippy::expect_used,
            reason = "verified bytecode guarantees Entry record layout exists"
        )]
        let layout = types
            .find_record_layout(elem_ty)
            .expect("Entry record layout must exist");
        let kind_ty = types.get(elem_ty).fields[1].ty;
        for (name, kind) in items {
            let name_val = heap.alloc_text(name)?;
            let kind_tag = find_case_tag(types, kind_ty, kind.case_name());
            let kind_val = heap.make_case(types, kind_ty, kind_tag, Vec::new())?;
            let rec = heap.construct(types, layout, vec![name_val, kind_val])?;
            out_list.push_back(rec);
        }
    } else {
        for (name, _) in items {
            out_list.push_back(heap.alloc_text(name)?);
        }
    }
    let list_val = heap.alloc_list(out_list)?;
    make_file_ok(heap, types, result_ty, list_val)
}

#[cfg(unix)]
pub(crate) fn errno_to_problem(err: rustix::io::Errno) -> FileProblem {
    use rustix::io::Errno;
    match err {
        Errno::NOENT => FileProblem::not_found(),
        Errno::ACCESS | Errno::PERM | Errno::XDEV => FileProblem::not_allowed(false),
        Errno::NOTDIR => FileProblem::failed("not a folder"),
        Errno::ISDIR => FileProblem::failed("is a folder"),
        Errno::LOOP => FileProblem::failed("too many links"),
        other => FileProblem::failed(other.to_string()),
    }
}
