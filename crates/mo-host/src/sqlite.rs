use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
#[cfg(unix)]
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{Capability, TypeKind};
use mo_def::HostOpId;
use mo_def::limits::{
    MO_LIMIT_HOST_PATH_SIZE, MO_LIMIT_OPEN_DATABASES, MO_LIMIT_OPEN_FOLDERS, MO_LIMIT_SQL_BYTES,
    MO_LIMIT_SQL_PARAMETERS, MO_LIMIT_SQL_STATS_STATEMENTS, MO_LIMIT_VALUE_BYTES,
};
use mo_heap::{Heap, Value};
use rusqlite::config::DbConfig;
use rusqlite::hooks::{AuthAction, AuthContext, Authorization};
use rusqlite::limits::Limit;
use rusqlite::types::{Value as SqlParam, ValueRef};
use rusqlite::{Connection, ErrorCode, OpenFlags};
#[cfg(unix)]
use rustix::fs::{AtFlags, CWD, FileType, statat};
#[cfg(unix)]
use rustix::io::{Errno, fcntl_dupfd_cloexec};

use crate::HostError;
use crate::files_common::{
    FileProblem, FileStatus, find_case_tag, plain_relative, prepare_real_path,
};
#[cfg(unix)]
use crate::real_files::{PathMode, ResolverState, dir_fd_path, resolve};

/// Serializes real-file open + initial WAL-index recovery and final close so concurrent
/// threads never race WAL header initialization (matching `src/host/sqlite.c:sqlite_open_mutex`).
static SQLITE_OPEN_MUTEX: Mutex<()> = Mutex::new(());

#[cfg(unix)]
struct RealSqliteRoot {
    fd: OwnedFd,
    open_folders: Rc<Cell<u32>>,
}

#[cfg(unix)]
impl Drop for RealSqliteRoot {
    fn drop(&mut self) {
        let cur = self.open_folders.get();
        debug_assert!(cur != 0);
        self.open_folders.set(cur.saturating_sub(1));
    }
}

#[derive(Default)]
struct FakeQueryStats {
    query_count: i64,
    exec_count: i64,
    statements: VecDeque<Value>,
}

struct FakeSqliteEntry {
    path: String,
    conn: Option<Rc<RefCell<Connection>>>,
    raw: Option<Vec<u8>>,
    stats: Rc<RefCell<FakeQueryStats>>,
}

struct FakeSqliteHub {
    entries: Vec<FakeSqliteEntry>,
}

enum SqliteCap {
    #[cfg(unix)]
    Real(RealSqliteRoot),
    Fake(Rc<RefCell<FakeSqliteHub>>),
}

enum DbHandle {
    Owned {
        conn: Option<Box<RefCell<Connection>>>,
        serialize_close: bool,
    },
    Shared {
        conn: Rc<RefCell<Connection>>,
        stats: Rc<RefCell<FakeQueryStats>>,
    },
}

pub(crate) struct DatabaseCap {
    handle: DbHandle,
    open_databases: Rc<Cell<u32>>,
}

impl Drop for DatabaseCap {
    fn drop(&mut self) {
        if let DbHandle::Owned {
            conn,
            serialize_close: true,
        } = &mut self.handle
        {
            let _guard = SQLITE_OPEN_MUTEX
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            drop(conn.take());
        }
        let cur = self.open_databases.get();
        debug_assert!(cur != 0);
        self.open_databases.set(cur.saturating_sub(1));
    }
}

impl DatabaseCap {
    fn with_conn<R>(&self, f: impl FnOnce(&mut Connection) -> R) -> R {
        match &self.handle {
            DbHandle::Owned { conn, .. } => {
                #[expect(
                    clippy::expect_used,
                    reason = "owned SQLite connection option is only taken in Drop"
                )]
                let mut borrow = conn
                    .as_ref()
                    .expect("database handle alive until drop")
                    .borrow_mut();
                f(&mut borrow)
            }
            DbHandle::Shared { conn, .. } => {
                let mut borrow = conn.borrow_mut();
                f(&mut borrow)
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Error & Result helpers
// ---------------------------------------------------------------------------

fn find_record_field(types: &TypeTable, record_ty: u32, name: &str) -> usize {
    let t = types.get(record_ty);
    debug_assert_eq!(t.kind, TypeKind::Record);
    for (i, f) in t.fields.iter().enumerate() {
        if f.name.as_deref() == Some(name) {
            return i;
        }
    }
    unreachable!("missing field {name} on record type {record_ty}");
}

fn find_sqlite_value_type(types: &TypeTable) -> u32 {
    for (i, t) in types.items.iter().enumerate() {
        if t.kind == TypeKind::Choice && t.name.as_deref() == Some("Sqlite.Value") {
            return i as u32;
        }
    }
    unreachable!("Sqlite.Value type must exist when Sqlite params are bound");
}

fn make_sqlite_ok(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    value: Value,
) -> Result<Value, HostError> {
    let ok_tag = find_case_tag(types, result_ty, "Ok");
    Ok(heap.make_case(types, result_ty, ok_tag, vec![value])?)
}

fn error_outside_folder(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    path_val: &Value,
) -> Result<Value, HostError> {
    let err_tag = find_case_tag(types, result_ty, "Error");
    let err_ty = types.get(result_ty).cases[err_tag as usize].fields[0].ty;
    let tag = find_case_tag(types, err_ty, "OutsideFolder");
    let err_val = heap.make_case(types, err_ty, tag, vec![path_val.clone()])?;
    Ok(heap.make_case(types, result_ty, err_tag, vec![err_val])?)
}

fn error_text_msg(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    case_name: &str,
    msg: &str,
    fallback: &str,
) -> Result<Value, HostError> {
    let chosen = if msg.is_empty() { fallback } else { msg };
    let err_tag = find_case_tag(types, result_ty, "Error");
    let err_ty = types.get(result_ty).cases[err_tag as usize].fields[0].ty;
    let tag = find_case_tag(types, err_ty, case_name);
    let text_val = heap.alloc_text(chosen.to_string())?;
    let err_val = heap.make_case(types, err_ty, tag, vec![text_val])?;
    Ok(heap.make_case(types, result_ty, err_tag, vec![err_val])?)
}

fn error_not_permitted(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    reason: Option<&str>,
) -> Result<Value, HostError> {
    error_text_msg(
        heap,
        types,
        result_ty,
        "NotPermitted",
        reason.unwrap_or(""),
        "statement is not permitted",
    )
}

fn error_failed(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    reason: &str,
) -> Result<Value, HostError> {
    error_text_msg(heap, types, result_ty, "SqlFailed", reason, "sqlite error")
}

fn file_error_to_sqlite(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    path_val: &Value,
    err: FileProblem,
) -> Result<Value, HostError> {
    match err.status {
        FileStatus::NotAllowed => error_outside_folder(heap, types, result_ty, path_val),
        FileStatus::NotFound => error_failed(heap, types, result_ty, "no such file or folder"),
        FileStatus::Failed(reason) => error_failed(heap, types, result_ty, &reason),
        FileStatus::NotText => error_failed(heap, types, result_ty, "invalid text"),
    }
}

fn is_sqlite_nomem(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(e, _) if e.code == ErrorCode::OutOfMemory
    )
}

fn is_sqlite_busy(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(e, _)
            if matches!(e.code, ErrorCode::DatabaseBusy | ErrorCode::DatabaseLocked)
    )
}

fn is_sqlite_auth(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(e, _) if e.code == ErrorCode::AuthorizationForStatementDenied
    )
}

fn sqlite_err_message(err: &rusqlite::Error) -> String {
    match err {
        rusqlite::Error::SqliteFailure(_, Some(msg)) => msg.clone(),
        other => other.to_string(),
    }
}

// ---------------------------------------------------------------------------
// Connection configuration, WAL initialization, path confinement
// ---------------------------------------------------------------------------

fn configure_connection(conn: &Connection) -> Result<(), rusqlite::Error> {
    // Defense-in-depth SQLite connection limits (`src/host/sqlite.c`).
    // Bundled rusqlite compiles without the `load_extension` feature (SQLITE_OMIT_LOAD_EXTENSION)
    // and `eval_authorizer` also explicitly denies `load_extension(...)` and user `ATTACH`.
    let _ = conn.set_db_config(DbConfig::SQLITE_DBCONFIG_DEFENSIVE, true);
    let _ = conn.set_limit(
        Limit::SQLITE_LIMIT_SQL_LENGTH,
        i32::try_from(MO_LIMIT_SQL_BYTES).unwrap_or(i32::MAX),
    );
    let _ = conn.set_limit(
        Limit::SQLITE_LIMIT_VARIABLE_NUMBER,
        i32::try_from(MO_LIMIT_SQL_PARAMETERS).unwrap_or(i32::MAX),
    );
    let _ = conn.set_limit(
        Limit::SQLITE_LIMIT_LENGTH,
        i32::try_from(MO_LIMIT_VALUE_BYTES).unwrap_or(i32::MAX),
    );
    // Allow 1 internal attach slot for `VACUUM` while `eval_authorizer` blocks all user `ATTACH`.
    let _ = conn.set_limit(Limit::SQLITE_LIMIT_ATTACHED, 1);
    conn.execute_batch("PRAGMA temp_store = MEMORY; PRAGMA foreign_keys = ON;")
}

fn check_db_header(conn: &Connection) -> Result<(), String> {
    configure_connection(conn).map_err(|e| sqlite_err_message(&e))?;
    for _ in 0..100 {
        match conn.execute_batch("SELECT count(*) FROM sqlite_schema;") {
            Ok(()) => return Ok(()),
            Err(ref e) if is_sqlite_busy(e) => std::thread::sleep(Duration::from_millis(5)),
            Err(e) => return Err(sqlite_err_message(&e)),
        }
    }
    Err("database is locked".to_string())
}

#[cfg(unix)]
fn ensure_wal_mode(conn: &Connection) -> Result<(), String> {
    for _ in 0..100 {
        let mode_res: Result<String, rusqlite::Error> =
            conn.query_row("PRAGMA journal_mode;", [], |r| r.get(0));
        match mode_res {
            Ok(mode) if mode.eq_ignore_ascii_case("wal") => return Ok(()),
            Ok(_) => {}
            Err(ref e) if is_sqlite_busy(e) => {
                std::thread::sleep(Duration::from_millis(5));
                continue;
            }
            Err(e) => return Err(sqlite_err_message(&e)),
        }
        match conn.execute_batch("PRAGMA journal_mode = WAL;") {
            Ok(()) => return Ok(()),
            Err(ref e) if is_sqlite_busy(e) => std::thread::sleep(Duration::from_millis(5)),
            Err(e) => return Err(sqlite_err_message(&e)),
        }
    }
    Err("database is locked".to_string())
}

#[cfg(unix)]
fn init_real_wal_connection(conn: &Connection) -> Result<(), String> {
    check_db_header(conn)?;
    ensure_wal_mode(conn)?;
    for _ in 0..100 {
        match conn.execute_batch("SELECT count(*) FROM sqlite_schema;") {
            Ok(()) => return Ok(()),
            Err(ref e) if is_sqlite_busy(e) => std::thread::sleep(Duration::from_millis(5)),
            Err(e) => return Err(sqlite_err_message(&e)),
        }
    }
    Err("database is locked".to_string())
}

#[cfg(unix)]
fn check_leaf_and_sidecars(dir_fd: BorrowedFd<'_>, leaf: &str) -> Result<(), (bool, &'static str)> {
    const SUFFIXES: [&str; 4] = ["", "-journal", "-wal", "-shm"];
    for (i, suffix) in SUFFIXES.iter().enumerate() {
        let candidate = format!("{leaf}{suffix}");
        if candidate.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
            return Err((false, "path too long"));
        }
        match statat(dir_fd, candidate.as_str(), AtFlags::SYMLINK_NOFOLLOW) {
            Ok(st) => {
                let ft = FileType::from_raw_mode(st.st_mode);
                if ft == FileType::Symlink {
                    return Err((true, ""));
                }
                if i == 0 {
                    if ft == FileType::Directory {
                        return Err((false, "is a folder"));
                    }
                    if ft != FileType::RegularFile {
                        return Err((false, "not a regular file"));
                    }
                }
            }
            Err(Errno::NOENT) => {}
            Err(e) => {
                let outside = matches!(e, Errno::ACCESS | Errno::PERM | Errno::LOOP);
                let reason = if e == Errno::LOOP {
                    "too many links"
                } else {
                    "unable to access path"
                };
                return Err((outside, reason));
            }
        }
    }
    Ok(())
}

fn paths_overlap(a: &str, b: &str) -> bool {
    if a.len() == b.len() {
        return a == b;
    }
    if a.len() < b.len() {
        b.as_bytes()[a.len()] == b'/' && b.starts_with(a)
    } else {
        a.as_bytes()[b.len()] == b'/' && a.starts_with(b)
    }
}

// ---------------------------------------------------------------------------
// SQL lexical helpers & authorizer (exact port of src/host/sqlite.c)
// ---------------------------------------------------------------------------

fn skip_sql_ws_comments(bytes: &[u8]) -> &[u8] {
    let mut i = 0usize;
    let len = bytes.len();
    while i < len {
        let b = bytes[i];
        if matches!(b, b' ' | b'\t' | b'\n' | b'\r' | 0x0c | 0x0b) {
            i += 1;
        } else if i + 1 < len && b == b'-' && bytes[i + 1] == b'-' {
            i += 2;
            while i < len && bytes[i] != b'\n' {
                i += 1;
            }
        } else if i + 1 < len && b == b'/' && bytes[i + 1] == b'*' {
            i += 2;
            while i + 1 < len && !(bytes[i] == b'*' && bytes[i + 1] == b'/') {
                i += 1;
            }
            if i + 1 < len {
                i += 2;
            } else {
                i = len;
            }
        } else {
            break;
        }
    }
    &bytes[i..]
}

fn is_ident_start(b: u8) -> bool {
    b.is_ascii_alphabetic() || b == b'_'
}

fn is_ident_cont(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

fn sql_is_vacuum_into(sql: &str) -> bool {
    let mut rest = skip_sql_ws_comments(sql.as_bytes());
    if rest.is_empty() || !is_ident_start(rest[0]) {
        return false;
    }
    let end1 = rest
        .iter()
        .position(|&b| !is_ident_cont(b))
        .unwrap_or(rest.len());
    if !rest[..end1].eq_ignore_ascii_case(b"VACUUM") {
        return false;
    }
    rest = skip_sql_ws_comments(&rest[end1..]);
    if rest.is_empty() || !is_ident_start(rest[0]) {
        return false;
    }
    let end2 = rest
        .iter()
        .position(|&b| !is_ident_cont(b))
        .unwrap_or(rest.len());
    if rest[..end2].eq_ignore_ascii_case(b"INTO") {
        return true;
    }
    rest = skip_sql_ws_comments(&rest[end2..]);
    if rest.first() == Some(&b';') {
        return false;
    }
    if !rest.is_empty() && is_ident_start(rest[0]) {
        let end3 = rest
            .iter()
            .position(|&b| !is_ident_cont(b))
            .unwrap_or(rest.len());
        if rest[..end3].eq_ignore_ascii_case(b"INTO") {
            return true;
        }
    }
    false
}

/// Checks if a SQL string contains anything after the first top-level semicolon
/// besides whitespace and comments (catching `SELECT 1;;` as well as `SELECT 1; SELECT 2`).
fn has_trailing_tokens_after_first_stmt(sql: &str) -> bool {
    let b = sql.as_bytes();
    let mut i = 0usize;
    let len = b.len();
    while i < len {
        match b[i] {
            b'-' if i + 1 < len && b[i + 1] == b'-' => {
                i += 2;
                while i < len && b[i] != b'\n' {
                    i += 1;
                }
            }
            b'/' if i + 1 < len && b[i + 1] == b'*' => {
                i += 2;
                while i + 1 < len && !(b[i] == b'*' && b[i + 1] == b'/') {
                    i += 1;
                }
                i = if i + 1 < len { i + 2 } else { len };
            }
            b'\'' | b'"' | b'`' => {
                let quote = b[i];
                i += 1;
                while i < len {
                    if b[i] == quote {
                        i += 1;
                        if i < len && b[i] == quote {
                            i += 1;
                            continue;
                        }
                        break;
                    }
                    i += 1;
                }
            }
            b'[' => {
                i += 1;
                while i < len && b[i] != b']' {
                    i += 1;
                }
                if i < len {
                    i += 1;
                }
            }
            b';' => {
                // Note: CREATE TRIGGER ... BEGIN ...; END; has embedded semicolons;
                // only treat a semicolon as statement terminator when not inside BEGIN..END.
                // However, let's defer trigger multi-semicolon handling to rusqlite::prepare
                // if `BEGIN` appeared before this semicolon.
                let prefix = &b[..i];
                if contains_trigger_keyword(prefix) {
                    i += 1;
                    continue;
                }
                return !skip_sql_ws_comments(&b[i + 1..]).is_empty();
            }
            _ => i += 1,
        }
    }
    false
}

fn contains_trigger_keyword(prefix: &[u8]) -> bool {
    let mut i = 0usize;
    let len = prefix.len();
    while i < len {
        match prefix[i] {
            b'-' if i + 1 < len && prefix[i + 1] == b'-' => {
                i += 2;
                while i < len && prefix[i] != b'\n' {
                    i += 1;
                }
            }
            b'/' if i + 1 < len && prefix[i + 1] == b'*' => {
                i += 2;
                while i + 1 < len && !(prefix[i] == b'*' && prefix[i + 1] == b'/') {
                    i += 1;
                }
                i = if i + 1 < len { i + 2 } else { len };
            }
            b'\'' | b'"' | b'`' => {
                let quote = prefix[i];
                i += 1;
                while i < len {
                    if prefix[i] == quote {
                        i += 1;
                        if i < len && prefix[i] == quote {
                            i += 1;
                            continue;
                        }
                        break;
                    }
                    i += 1;
                }
            }
            b'[' => {
                i += 1;
                while i < len && prefix[i] != b']' {
                    i += 1;
                }
                if i < len {
                    i += 1;
                }
            }
            b if is_ident_start(b) => {
                let start = i;
                while i < len && is_ident_cont(prefix[i]) {
                    i += 1;
                }
                if prefix[start..i].eq_ignore_ascii_case(b"TRIGGER") {
                    return true;
                }
            }
            _ => i += 1,
        }
    }
    false
}

struct AuthState {
    in_step: AtomicBool,
    vacuum_into: bool,
    denied_reason: Mutex<Option<&'static str>>,
}

impl AuthState {
    fn new(vacuum_into: bool) -> Arc<Self> {
        Arc::new(Self {
            in_step: AtomicBool::new(false),
            vacuum_into,
            denied_reason: Mutex::new(None),
        })
    }

    fn deny(&self, reason: &'static str) -> Authorization {
        let mut guard = self
            .denied_reason
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if guard.is_none() {
            *guard = Some(reason);
        }
        Authorization::Deny
    }

    fn denied(&self) -> Option<&'static str> {
        *self
            .denied_reason
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

fn eval_authorizer(state: &AuthState, ctx: AuthContext<'_>) -> Authorization {
    match ctx.action {
        AuthAction::Attach { filename } => {
            if state.in_step.load(Ordering::Relaxed) && !state.vacuum_into && filename.is_empty() {
                Authorization::Allow
            } else {
                state.deny("ATTACH is not permitted")
            }
        }
        AuthAction::Detach { database_name } => {
            if state.in_step.load(Ordering::Relaxed)
                && !state.vacuum_into
                && database_name.starts_with("vacuum_")
            {
                Authorization::Allow
            } else {
                state.deny("DETACH is not permitted")
            }
        }
        AuthAction::Pragma { pragma_name, .. }
            if pragma_name.eq_ignore_ascii_case("temp_store")
                || pragma_name.eq_ignore_ascii_case("temp_store_directory") =>
        {
            state.deny("PRAGMA temp_store is not permitted")
        }
        AuthAction::Function { function_name }
            if function_name.eq_ignore_ascii_case("load_extension") =>
        {
            state.deny("load_extension is not permitted")
        }
        _ => Authorization::Allow,
    }
}

struct ClearAuthorizerGuard<'a>(&'a Connection);

impl Drop for ClearAuthorizerGuard<'_> {
    fn drop(&mut self) {
        let _ = self
            .0
            .authorizer(None::<fn(AuthContext<'_>) -> Authorization>);
    }
}

// ---------------------------------------------------------------------------
// Capability construction & open
// ---------------------------------------------------------------------------

#[cfg(unix)]
pub(crate) fn make_real_sqlite(
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
    let root = RealSqliteRoot {
        fd,
        open_folders: Rc::clone(open_folders),
    };
    Ok(heap.alloc_capability(Capability::Sqlite, ty, Rc::new(SqliteCap::Real(root)))?)
}

pub(crate) fn sqlite_fake(
    heap: &Heap,
    result_ty: u32,
    databases: Option<&Value>,
) -> Result<Value, HostError> {
    let mut entries: Vec<FakeSqliteEntry> = Vec::new();
    if let Some(databases) = databases {
        for (key_val, val_val) in databases.as_map().iter() {
            let key_bytes = key_val.as_text().as_bytes();
            if key_bytes.len() >= MO_LIMIT_HOST_PATH_SIZE as usize
                || key_bytes.contains(&0)
                || !plain_relative(key_bytes)
            {
                return Err(HostError::Bug {
                    reason: "invalid database path",
                    detail: None,
                });
            }
            let Ok(norm) = prepare_real_path(key_bytes) else {
                return Err(HostError::Bug {
                    reason: "invalid database path",
                    detail: None,
                });
            };
            if norm.is_empty() {
                return Err(HostError::Bug {
                    reason: "invalid database path",
                    detail: None,
                });
            }
            if entries.iter().any(|e| paths_overlap(&norm, &e.path)) {
                return Err(HostError::Bug {
                    reason: "conflicting database path",
                    detail: None,
                });
            }
            let raw_bytes = val_val.as_bytes();
            let mut entry = FakeSqliteEntry {
                path: norm,
                conn: None,
                raw: None,
                stats: Rc::new(RefCell::new(FakeQueryStats::default())),
            };
            if !raw_bytes.is_empty() {
                let valid_magic =
                    raw_bytes.len() >= 100 && &raw_bytes[..16] == b"SQLite format 3\0";
                let mut loaded = None;
                if valid_magic && let Ok(mut conn) = Connection::open_in_memory() {
                    let mut cursor = std::io::Cursor::new(raw_bytes);
                    if conn
                        .deserialize_read_exact(
                            rusqlite::MAIN_DB,
                            &mut cursor,
                            raw_bytes.len(),
                            false,
                        )
                        .is_ok()
                        && check_db_header(&conn).is_ok()
                    {
                        loaded = Some(Rc::new(RefCell::new(conn)));
                    }
                }
                if let Some(conn) = loaded {
                    entry.conn = Some(conn);
                } else {
                    entry.raw = Some(raw_bytes.to_vec());
                }
            }
            entries.push(entry);
        }
    }

    let hub = Rc::new(RefCell::new(FakeSqliteHub { entries }));
    Ok(heap.alloc_capability(Capability::Sqlite, result_ty, Rc::new(SqliteCap::Fake(hub)))?)
}

fn wrap_database(
    heap: &Heap,
    types: &TypeTable,
    open_databases: &Rc<Cell<u32>>,
    result_ty: u32,
    handle: DbHandle,
) -> Result<Value, HostError> {
    if u64::from(open_databases.get()) >= MO_LIMIT_OPEN_DATABASES {
        return Err(HostError::Bug {
            reason: "too many databases open at once",
            detail: None,
        });
    }
    let ok_tag = find_case_tag(types, result_ty, "Ok");
    let db_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
    open_databases.set(open_databases.get() + 1);
    let cap = DatabaseCap {
        handle,
        open_databases: Rc::clone(open_databases),
    };
    let db_val = heap.alloc_capability(Capability::Database, db_ty, Rc::new(cap))?;
    make_sqlite_ok(heap, types, result_ty, db_val)
}

fn open_ephemeral(
    heap: &Heap,
    types: &TypeTable,
    open_databases: &Rc<Cell<u32>>,
    result_ty: u32,
    is_fake: bool,
) -> Result<Value, HostError> {
    if u64::from(open_databases.get()) >= MO_LIMIT_OPEN_DATABASES {
        return Err(HostError::Bug {
            reason: "too many databases open at once",
            detail: None,
        });
    }
    let conn = match Connection::open_in_memory() {
        Ok(c) => c,
        Err(ref e) if is_sqlite_nomem(e) => {
            return Err(HostError::OutOfMemory {
                value_bytes_limit: false,
            });
        }
        Err(e) => return error_failed(heap, types, result_ty, &sqlite_err_message(&e)),
    };
    if let Err(e) = configure_connection(&conn) {
        return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
    }
    let handle = if is_fake {
        DbHandle::Shared {
            conn: Rc::new(RefCell::new(conn)),
            stats: Rc::new(RefCell::new(FakeQueryStats::default())),
        }
    } else {
        DbHandle::Owned {
            conn: Some(Box::new(RefCell::new(conn))),
            serialize_close: false,
        }
    };
    wrap_database(heap, types, open_databases, result_ty, handle)
}

fn open_fake(
    heap: &Heap,
    types: &TypeTable,
    open_databases: &Rc<Cell<u32>>,
    hub: &Rc<RefCell<FakeSqliteHub>>,
    result_ty: u32,
    norm: &str,
) -> Result<Value, HostError> {
    let mut hub_mut = hub.borrow_mut();
    let found_idx = hub_mut.entries.iter().position(|e| e.path == norm);
    if found_idx.is_none() {
        for e in &hub_mut.entries {
            if norm.len() < e.path.len()
                && e.path.as_bytes()[norm.len()] == b'/'
                && e.path.starts_with(norm)
            {
                return error_failed(heap, types, result_ty, "is a folder");
            }
            if e.path.len() < norm.len()
                && norm.as_bytes()[e.path.len()] == b'/'
                && norm.starts_with(&e.path)
            {
                return error_failed(heap, types, result_ty, "not a folder");
            }
        }
    }

    if u64::from(open_databases.get()) >= MO_LIMIT_OPEN_DATABASES {
        return Err(HostError::Bug {
            reason: "too many databases open at once",
            detail: None,
        });
    }

    let (shared_conn, shared_stats) = if let Some(idx) = found_idx {
        let entry = &mut hub_mut.entries[idx];
        if entry.conn.is_none() && entry.raw.is_some() {
            return error_failed(heap, types, result_ty, "file is not a database");
        }
        let rc = if let Some(ref existing) = entry.conn {
            Rc::clone(existing)
        } else {
            let conn = Connection::open_in_memory().map_err(|_| HostError::OutOfMemory {
                value_bytes_limit: false,
            })?;
            configure_connection(&conn).map_err(|_| HostError::OutOfMemory {
                value_bytes_limit: false,
            })?;
            let rc = Rc::new(RefCell::new(conn));
            entry.conn = Some(Rc::clone(&rc));
            rc
        };
        (rc, Rc::clone(&entry.stats))
    } else {
        let conn = Connection::open_in_memory().map_err(|_| HostError::OutOfMemory {
            value_bytes_limit: false,
        })?;
        configure_connection(&conn).map_err(|_| HostError::OutOfMemory {
            value_bytes_limit: false,
        })?;
        let rc = Rc::new(RefCell::new(conn));
        let stats = Rc::new(RefCell::new(FakeQueryStats::default()));
        hub_mut.entries.push(FakeSqliteEntry {
            path: norm.to_string(),
            conn: Some(Rc::clone(&rc)),
            raw: None,
            stats: Rc::clone(&stats),
        });
        (rc, stats)
    };
    drop(hub_mut);

    wrap_database(
        heap,
        types,
        open_databases,
        result_ty,
        DbHandle::Shared {
            conn: shared_conn,
            stats: shared_stats,
        },
    )
}

#[cfg(unix)]
#[expect(
    clippy::too_many_arguments,
    reason = "internal SQLite open helper forwards host state and resolved path parameters"
)]
fn open_real(
    heap: &Heap,
    types: &TypeTable,
    resolver: &mut ResolverState,
    open_databases: &Rc<Cell<u32>>,
    root: &RealSqliteRoot,
    result_ty: u32,
    path_val: &Value,
    norm: &str,
) -> Result<Value, HostError> {
    let (parent, leaf) = match norm.rfind('/') {
        Some(pos) => (&norm[..pos], &norm[pos + 1..]),
        None => (".", norm),
    };
    let resolved = match resolve(resolver, root.fd.as_fd(), parent, PathMode::Folder) {
        Ok(r) => r,
        Err(perr) => return file_error_to_sqlite(heap, types, result_ty, path_val, perr),
    };
    let dir_fd = resolved.fd;

    if let Err((outside, reason)) = check_leaf_and_sidecars(dir_fd.as_fd(), leaf) {
        return if outside {
            error_outside_folder(heap, types, result_ty, path_val)
        } else {
            error_failed(heap, types, result_ty, reason)
        };
    }

    let Some(dir_path) = dir_fd_path(dir_fd.as_fd(), true) else {
        return error_failed(heap, types, result_ty, "unable to resolve database folder");
    };
    let full_path = if dir_path == "/" {
        format!("/{leaf}")
    } else {
        format!("{dir_path}/{leaf}")
    };
    if full_path.len() >= MO_LIMIT_HOST_PATH_SIZE as usize {
        return error_failed(heap, types, result_ty, "path too long");
    }

    if u64::from(open_databases.get()) >= MO_LIMIT_OPEN_DATABASES {
        return Err(HostError::Bug {
            reason: "too many databases open at once",
            detail: None,
        });
    }

    let open_flags = OpenFlags::SQLITE_OPEN_READ_WRITE
        | OpenFlags::SQLITE_OPEN_CREATE
        | OpenFlags::SQLITE_OPEN_NOFOLLOW
        | OpenFlags::SQLITE_OPEN_NO_MUTEX;

    let guard = SQLITE_OPEN_MUTEX
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let conn = match Connection::open_with_flags(&full_path, open_flags) {
        Ok(c) => c,
        Err(e) => {
            drop(guard);
            let after_outside = check_leaf_and_sidecars(dir_fd.as_fd(), leaf)
                .err()
                .is_some_and(|(o, _)| o);
            if is_sqlite_nomem(&e) {
                return Err(HostError::OutOfMemory {
                    value_bytes_limit: false,
                });
            }
            return if after_outside {
                error_outside_folder(heap, types, result_ty, path_val)
            } else {
                error_failed(heap, types, result_ty, &sqlite_err_message(&e))
            };
        }
    };

    if let Err((outside, reason)) = check_leaf_and_sidecars(dir_fd.as_fd(), leaf) {
        drop(conn);
        drop(guard);
        return if outside {
            error_outside_folder(heap, types, result_ty, path_val)
        } else {
            error_failed(heap, types, result_ty, reason)
        };
    }

    let identity_ok = match (
        statat(dir_fd.as_fd(), leaf, AtFlags::SYMLINK_NOFOLLOW),
        statat(CWD, full_path.as_str(), AtFlags::SYMLINK_NOFOLLOW),
    ) {
        (Ok(leaf_st), Ok(full_st)) => {
            if leaf_st.st_dev != full_st.st_dev
                || leaf_st.st_ino != full_st.st_ino
                || FileType::from_raw_mode(full_st.st_mode) != FileType::RegularFile
            {
                Err(true)
            } else if leaf_st.st_size > 0 && leaf_st.st_size < 100 {
                Err(false)
            } else {
                Ok(())
            }
        }
        _ => Err(true),
    };
    if let Err(outside) = identity_ok {
        drop(conn);
        drop(guard);
        return if outside {
            error_outside_folder(heap, types, result_ty, path_val)
        } else {
            error_failed(heap, types, result_ty, "file is not a database")
        };
    }
    drop(dir_fd);

    let _ = conn.busy_timeout(Duration::from_millis(5000));
    if let Err(msg) = init_real_wal_connection(&conn) {
        drop(conn);
        drop(guard);
        return error_failed(heap, types, result_ty, &msg);
    }
    drop(guard);

    wrap_database(
        heap,
        types,
        open_databases,
        result_ty,
        DbHandle::Owned {
            conn: Some(Box::new(RefCell::new(conn))),
            serialize_close: true,
        },
    )
}

// ---------------------------------------------------------------------------
// SQL execution (run, execute_raw, query)
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum SqlExecMode {
    Run,
    Raw,
    Query,
}

fn convert_params(heap: &Heap, types: &TypeTable, params: &Value) -> Vec<SqlParam> {
    let list = params.as_list();
    if list.is_empty() {
        return Vec::new();
    }
    let val_ty = find_sqlite_value_type(types);
    let cases = &types.get(val_ty).cases;
    let mut out = Vec::with_capacity(list.len());
    for elem in list.iter() {
        let tag = elem.as_tag();
        let cname = cases[tag as usize].name.as_str();
        let v = match cname {
            "Null" => SqlParam::Null,
            "Integer" => {
                let p = heap.extract_case_field(types, val_ty, elem, tag, 0);
                SqlParam::Integer(p.as_int())
            }
            "Real" => {
                let p = heap.extract_case_field(types, val_ty, elem, tag, 0);
                SqlParam::Real(p.as_float())
            }
            "String" => {
                let p = heap.extract_case_field(types, val_ty, elem, tag, 0);
                SqlParam::Text(p.as_text().to_string())
            }
            "Blob" => {
                let p = heap.extract_case_field(types, val_ty, elem, tag, 0);
                SqlParam::Blob(p.as_bytes().to_vec())
            }
            other => unreachable!("unknown Sqlite.Value case {other}"),
        };
        out.push(v);
    }
    out
}

fn read_row_value(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    val_ty: u32,
    vref: ValueRef<'_>,
) -> Result<Result<Value, Value>, HostError> {
    match vref {
        ValueRef::Null => {
            let tag = find_case_tag(types, val_ty, "Null");
            Ok(Ok(heap.make_case(types, val_ty, tag, Vec::new())?))
        }
        ValueRef::Integer(n) => {
            let tag = find_case_tag(types, val_ty, "Integer");
            Ok(Ok(heap.make_case(
                types,
                val_ty,
                tag,
                vec![Value::Int(n)],
            )?))
        }
        ValueRef::Real(x) => {
            let tag = find_case_tag(types, val_ty, "Real");
            Ok(Ok(heap.make_case(
                types,
                val_ty,
                tag,
                vec![Value::Float(x)],
            )?))
        }
        ValueRef::Text(bytes) => {
            let Ok(s) = std::str::from_utf8(bytes) else {
                let err = error_failed(heap, types, result_ty, "result text is not valid UTF-8")?;
                return Ok(Err(err));
            };
            let text_val = heap.alloc_text(s.to_string())?;
            let tag = find_case_tag(types, val_ty, "String");
            Ok(Ok(heap.make_case(types, val_ty, tag, vec![text_val])?))
        }
        ValueRef::Blob(bytes) => {
            let blob_val = heap.alloc_bytes(bytes.to_vec())?;
            let tag = find_case_tag(types, val_ty, "Blob");
            Ok(Ok(heap.make_case(types, val_ty, tag, vec![blob_val])?))
        }
    }
}

fn exec_sql(
    heap: &Heap,
    types: &TypeTable,
    mode: SqlExecMode,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    let db_cap = crate::downcast_cap::<DatabaseCap>(&args[0], "Sqlite.Database capability");
    let sql = args[1].as_text();
    let sql_bytes = sql.as_bytes();
    if sql_bytes.len() as u64 > MO_LIMIT_SQL_BYTES {
        return error_failed(heap, types, result_ty, "SQL statement too large");
    }
    if sql_bytes.contains(&0) {
        return error_failed(heap, types, result_ty, "SQL contains NUL byte");
    }
    if args[2].as_list().len() as u64 > MO_LIMIT_SQL_PARAMETERS {
        return error_failed(heap, types, result_ty, "too many SQL parameters");
    }
    if skip_sql_ws_comments(sql_bytes).is_empty() {
        return error_failed(heap, types, result_ty, "empty SQL statement");
    }

    let auth = AuthState::new(sql_is_vacuum_into(sql));

    db_cap.with_conn(|conn| {
        let auth_cb = Arc::clone(&auth);
        let _ = conn.authorizer(Some(move |ctx: AuthContext<'_>| {
            eval_authorizer(&auth_cb, ctx)
        }));
        let _clear_guard = ClearAuthorizerGuard(conn);

        let mut stmt = match conn.prepare(sql) {
            Ok(s) => s,
            Err(e) => {
                if let Some(denied) = auth.denied() {
                    return error_not_permitted(heap, types, result_ty, Some(denied));
                }
                if is_sqlite_auth(&e) {
                    return error_not_permitted(heap, types, result_ty, None);
                }
                if matches!(e, rusqlite::Error::MultipleStatement) {
                    return error_failed(
                        heap,
                        types,
                        result_ty,
                        "multiple SQL statements are not permitted",
                    );
                }
                if is_sqlite_nomem(&e) {
                    return Err(HostError::OutOfMemory {
                        value_bytes_limit: false,
                    });
                }
                return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
            }
        };

        if has_trailing_tokens_after_first_stmt(sql) {
            return error_failed(
                heap,
                types,
                result_ty,
                "multiple SQL statements are not permitted",
            );
        }

        let expected_params = stmt.parameter_count();
        let supplied_params = args[2].as_list().len();
        if supplied_params != expected_params {
            return error_failed(heap, types, result_ty, "SQL parameter count mismatch");
        }
        let sql_params = convert_params(heap, types, &args[2]);
        for (idx, param) in sql_params.iter().enumerate() {
            if let Err(e) = stmt.raw_bind_parameter(idx + 1, param) {
                if is_sqlite_nomem(&e) {
                    return Err(HostError::OutOfMemory {
                        value_bytes_limit: false,
                    });
                }
                return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
            }
        }

        if let DbHandle::Shared { stats, .. } = &db_cap.handle {
            let mut st = stats.borrow_mut();
            if mode == SqlExecMode::Query {
                st.query_count = st.query_count.saturating_add(1);
            } else {
                st.exec_count = st.exec_count.saturating_add(1);
            }
            if st.statements.len() < MO_LIMIT_SQL_STATS_STATEMENTS as usize {
                st.statements.push_back(args[1].clone());
            }
        }

        auth.in_step.store(true, Ordering::Relaxed);

        if mode != SqlExecMode::Query {
            let total_before = conn.total_changes();
            let step_res: Result<(), rusqlite::Error> = (|| {
                let mut rows = stmt.raw_query();
                while rows.next()?.is_some() {}
                Ok(())
            })();
            let fin_res = stmt.finalize();

            if let Some(denied) = auth.denied() {
                return error_not_permitted(heap, types, result_ty, Some(denied));
            }
            if let Err(e) = step_res {
                if is_sqlite_auth(&e) {
                    return error_not_permitted(heap, types, result_ty, None);
                }
                if is_sqlite_nomem(&e) {
                    return Err(HostError::OutOfMemory {
                        value_bytes_limit: false,
                    });
                }
                return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
            }
            if let Err(e) = fin_res {
                if is_sqlite_auth(&e) {
                    return error_not_permitted(heap, types, result_ty, None);
                }
                if is_sqlite_nomem(&e) {
                    return Err(HostError::OutOfMemory {
                        value_bytes_limit: false,
                    });
                }
                return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
            }

            if mode == SqlExecMode::Raw {
                let changes = (conn.total_changes() - total_before) as i64;
                let last_id = conn.last_insert_rowid();
                let ok_tag = find_case_tag(types, result_ty, "Ok");
                let tup_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
                #[expect(
                    clippy::expect_used,
                    reason = "verified bytecode guarantees tuple layout for execute_raw"
                )]
                let layout = types
                    .find_tuple_layout(tup_ty)
                    .expect("tuple layout for execute_raw");
                let tup = heap.construct(
                    types,
                    layout,
                    vec![Value::Int(changes), Value::Int(last_id)],
                )?;
                return make_sqlite_ok(heap, types, result_ty, tup);
            }
            return make_sqlite_ok(heap, types, result_ty, Value::Unit);
        }

        // Query mode
        let ok_tag = find_case_tag(types, result_ty, "Ok");
        let rows_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;
        let col_field = find_record_field(types, rows_ty, "columns");
        let rows_field = find_record_field(types, rows_ty, "rows");
        let list_list_val_ty = types.get(rows_ty).fields[rows_field].ty;
        let list_val_ty = types.get(list_list_val_ty).parameters[0];
        let val_ty = types.get(list_val_ty).parameters[0];

        let col_count = stmt.column_count();
        let mut columns = VecDeque::with_capacity(col_count);
        for c in 0..col_count {
            let cname = match stmt.column_name(c) {
                Ok(name) => name,
                Err(rusqlite::Error::Utf8Error(..)) => {
                    return error_failed(heap, types, result_ty, "column name is not valid UTF-8");
                }
                Err(_) => "",
            };
            columns.push_back(heap.alloc_text(cname.to_string())?);
        }
        let columns_val = heap.alloc_list(columns)?;

        let mut rows_vec: VecDeque<Value> = VecDeque::new();
        let mut step_err: Option<rusqlite::Error> = None;
        let mut value_err: Option<Value> = None;
        {
            let mut rows = stmt.raw_query();
            loop {
                match rows.next() {
                    Ok(Some(row)) => {
                        let mut row_items = VecDeque::with_capacity(col_count);
                        for c in 0..col_count {
                            let vref = match row.get_ref(c) {
                                Ok(v) => v,
                                Err(e) => {
                                    step_err = Some(e);
                                    break;
                                }
                            };
                            match read_row_value(heap, types, result_ty, val_ty, vref)? {
                                Ok(v) => row_items.push_back(v),
                                Err(err_val) => {
                                    value_err = Some(err_val);
                                    break;
                                }
                            }
                        }
                        if step_err.is_some() || value_err.is_some() {
                            break;
                        }
                        rows_vec.push_back(heap.alloc_list(row_items)?);
                    }
                    Ok(None) => break,
                    Err(e) => {
                        step_err = Some(e);
                        break;
                    }
                }
            }
        }
        let fin_res = stmt.finalize();

        if let Some(err_val) = value_err {
            return Ok(err_val);
        }
        if let Some(denied) = auth.denied() {
            return error_not_permitted(heap, types, result_ty, Some(denied));
        }
        if let Some(e) = step_err {
            if is_sqlite_auth(&e) {
                return error_not_permitted(heap, types, result_ty, None);
            }
            if is_sqlite_nomem(&e) {
                return Err(HostError::OutOfMemory {
                    value_bytes_limit: false,
                });
            }
            return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
        }
        if let Err(e) = fin_res {
            if is_sqlite_auth(&e) {
                return error_not_permitted(heap, types, result_ty, None);
            }
            if is_sqlite_nomem(&e) {
                return Err(HostError::OutOfMemory {
                    value_bytes_limit: false,
                });
            }
            return error_failed(heap, types, result_ty, &sqlite_err_message(&e));
        }

        let rows_list_val = heap.alloc_list(rows_vec)?;
        let mut rec_fields = vec![Value::Unit; 2];
        rec_fields[col_field] = columns_val;
        rec_fields[rows_field] = rows_list_val;
        #[expect(
            clippy::expect_used,
            reason = "verified bytecode guarantees Sqlite.Rows record layout"
        )]
        let layout = types
            .find_record_layout(rows_ty)
            .expect("Sqlite.Rows record layout");
        let rec = heap.construct(types, layout, rec_fields)?;
        make_sqlite_ok(heap, types, result_ty, rec)
    })
}

// ---------------------------------------------------------------------------
// Public dispatch
// ---------------------------------------------------------------------------

pub(crate) fn call_sqlite(
    heap: &Heap,
    types: &TypeTable,
    #[cfg(unix)] resolver: &mut ResolverState,
    open_databases: &Rc<Cell<u32>>,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    match op {
        HostOpId::SqliteOpen => {
            let cap = crate::downcast_cap::<SqliteCap>(&args[0], "Sqlite capability");
            let raw = args[1].as_text();
            if raw == ":memory:" {
                let is_fake = matches!(cap, SqliteCap::Fake(_));
                return open_ephemeral(heap, types, open_databases, result_ty, is_fake);
            }
            let norm = match prepare_real_path(raw.as_bytes()) {
                Ok(n) => n,
                Err(nerr) => return file_error_to_sqlite(heap, types, result_ty, &args[1], nerr),
            };
            if norm.is_empty() {
                return error_failed(heap, types, result_ty, "is a folder");
            }
            match cap {
                SqliteCap::Fake(hub) => {
                    open_fake(heap, types, open_databases, hub, result_ty, &norm)
                }
                #[cfg(unix)]
                SqliteCap::Real(root) => open_real(
                    heap,
                    types,
                    resolver,
                    open_databases,
                    root,
                    result_ty,
                    &args[1],
                    &norm,
                ),
            }
        }
        HostOpId::SqliteRun => exec_sql(heap, types, SqlExecMode::Run, result_ty, args),
        HostOpId::SqliteExecuteRaw => exec_sql(heap, types, SqlExecMode::Raw, result_ty, args),
        HostOpId::SqliteQuery => exec_sql(heap, types, SqlExecMode::Query, result_ty, args),
        HostOpId::SqliteStatsRaw => {
            let db_cap = crate::downcast_cap::<DatabaseCap>(&args[0], "Sqlite.Database capability");
            let DbHandle::Shared { stats, .. } = &db_cap.handle else {
                return Err(HostError::Bug {
                    reason: "Sqlite.Database.stats is only available on fake or in-memory test databases",
                    detail: None,
                });
            };
            let st = stats.borrow();
            let stmts_val = heap.alloc_list(st.statements.clone())?;
            #[expect(
                clippy::expect_used,
                reason = "verified bytecode guarantees tuple layout for Sqlite.stats_raw"
            )]
            let layout = types
                .find_tuple_layout(result_ty)
                .expect("tuple layout for Sqlite.stats_raw");
            Ok(heap.construct(
                types,
                layout,
                vec![
                    Value::Int(st.query_count),
                    Value::Int(st.exec_count),
                    stmts_val,
                ],
            )?)
        }
        HostOpId::SqliteResetStatsRaw => {
            let db_cap = crate::downcast_cap::<DatabaseCap>(&args[0], "Sqlite.Database capability");
            let DbHandle::Shared { stats, .. } = &db_cap.handle else {
                return Err(HostError::Bug {
                    reason: "Sqlite.Database.reset_stats is only available on fake or in-memory test databases",
                    detail: None,
                });
            };
            *stats.borrow_mut() = FakeQueryStats::default();
            Ok(Value::Unit)
        }
        _ => unreachable!("non-sqlite hostop routed to sqlite module"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn trigger_keyword_scanner_ignores_strings_and_comments_and_limits_apply() {
        assert!(has_trailing_tokens_after_first_stmt("SELECT 'TRIGGER';;"));
        assert!(has_trailing_tokens_after_first_stmt(
            "SELECT \"TRIGGER\"; SELECT 2"
        ));
        assert!(has_trailing_tokens_after_first_stmt(
            "SELECT /* TRIGGER */ 1;;"
        ));
        assert!(has_trailing_tokens_after_first_stmt(
            "-- TRIGGER\nSELECT 1; SELECT 2"
        ));
        assert!(!has_trailing_tokens_after_first_stmt(
            "CREATE TRIGGER t AFTER INSERT ON a BEGIN INSERT INTO b VALUES(1); END;"
        ));

        let conn = Connection::open_in_memory().expect("in-memory sqlite");
        configure_connection(&conn).expect("configure_connection");
        assert_eq!(
            conn.limit(Limit::SQLITE_LIMIT_SQL_LENGTH),
            Ok(i32::try_from(MO_LIMIT_SQL_BYTES).unwrap_or(i32::MAX))
        );
        assert_eq!(
            conn.limit(Limit::SQLITE_LIMIT_VARIABLE_NUMBER),
            Ok(i32::try_from(MO_LIMIT_SQL_PARAMETERS).unwrap_or(i32::MAX))
        );
        assert!(conn.limit(Limit::SQLITE_LIMIT_LENGTH).is_ok_and(|n| n > 0));
        assert_eq!(conn.limit(Limit::SQLITE_LIMIT_ATTACHED), Ok(1));
    }
}
