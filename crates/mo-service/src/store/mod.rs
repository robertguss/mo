//! Durable SQLite `ServiceStore` (`src/host/store*.c` parity).
//!
//! Uses bundled `rusqlite` with `PRAGMA journal_mode=DELETE`, `PRAGMA synchronous=EXTRA`,
//! `PRAGMA locking_mode=EXCLUSIVE`, `MOST\x01` checksummed frames, failure capture/retention,
//! offline history streaming, and transactional program/upgrade recording.

pub mod failures;
pub mod history;
pub mod programs;

use std::path::Path;
use std::sync::Mutex;
use std::time::Duration;

use mo_bytecode::{FNV_OFFSET, fnv1a};
use mo_def::limits::{
    MO_LIMIT_FAILURE_MESSAGE_BYTES, MO_LIMIT_SAVED_BYTES, MO_LIMIT_SERVE_STORE_WAIT_US,
    MO_LIMIT_STORE_CACHE_KIB, MO_LIMIT_STORE_ROW, MO_LIMIT_STORE_SQL, MO_LIMIT_STORES,
};
use rusqlite::config::DbConfig;
use rusqlite::limits::Limit;
use rusqlite::{Connection, ErrorCode as SqliteCode, OpenFlags, OptionalExtension, params};

use crate::binary::{read_u32_le, read_u64_le};

/// Overhead bytes of a `MOST\x01` frame (`5 + 1 + 8 + 8 + 4 + 8 = 34`).
pub const STORE_FRAME_BYTES: usize = 34;

/// Fixed header bytes of a `MOST\x01` frame (`5 + 1 + 8 + 8 + 4 = 26`).
pub const FRAME_HEADER_BYTES: usize = 26;

/// Trailing checksum bytes of a `MOST\x01` frame (`8`).
pub const FRAME_CHECKSUM_BYTES: usize = 8;

pub(crate) const CORE_TABLES: &[(&str, &str)] = &[
    (
        "head",
        "CREATE TABLE head(id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL) STRICT",
    ),
    (
        "snapshot",
        "CREATE TABLE snapshot(id INTEGER PRIMARY KEY CHECK(id=1), record BLOB NOT NULL) STRICT",
    ),
    (
        "log",
        "CREATE TABLE log(seq INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT",
    ),
    (
        "failure",
        "CREATE TABLE failure(id INTEGER PRIMARY KEY, kind TEXT NOT NULL, first INTEGER NOT NULL, message BLOB NOT NULL) STRICT",
    ),
    (
        "failure_state",
        "CREATE TABLE failure_state(id INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT",
    ),
];

static OPEN_STORES: Mutex<u32> = Mutex::new(0);

pub(crate) fn claim_store_slot() -> bool {
    let mut guard = OPEN_STORES
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    if u64::from(*guard) >= MO_LIMIT_STORES {
        return false;
    }
    *guard += 1;
    true
}

pub(crate) fn release_store_slot() {
    let mut guard = OPEN_STORES
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    *guard = guard.saturating_sub(1);
}

/// Store operation status (`enum mo_store_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StoreStatus {
    /// Operation succeeded.
    Ok,
    /// Transient or filesystem I/O failure.
    Io,
    /// Stored bytes failed format/schema/checksum validation.
    Corrupt,
    /// Memory allocation or limit refusal.
    Memory,
    /// Empty path was supplied.
    EmptyPath,
    /// `:memory:` path was supplied where a file path is required.
    MemoryPath,
    /// More than `MO_LIMIT_STORES` stores open simultaneously.
    Handles,
    /// Another connection holds the exclusive store lock.
    Busy,
}

impl StoreStatus {
    /// Returns the canonical human-readable diagnostic text (`mo_store_status_text`).
    #[must_use]
    pub const fn text(self) -> Option<&'static str> {
        match self {
            Self::Ok => None,
            Self::Io => {
                Some("service store failed; reopen it to see whether the last change was saved")
            }
            Self::Corrupt => Some(
                "saved service data is damaged or was written by a different program; recovery refused",
            ),
            Self::Memory => Some("service store memory allocation failed"),
            Self::EmptyPath => Some("service store path is empty; use a file path"),
            Self::MemoryPath => Some(":memory: is not a service store path; use a file path"),
            Self::Handles => Some("too many open service stores; close one of the 8 open stores"),
            Self::Busy => Some(
                "another process holds the service store, such as a second mo serve or a reader of its data folder",
            ),
        }
    }
}

/// Record kind stored in a `MOST\x01` frame (`enum mo_store_record_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum StoreRecordKind {
    /// Write-ahead log entry.
    Log = 1,
    /// Authoritative state snapshot.
    State = 2,
    /// Failure pre-call state snapshot.
    FailureState = 3,
    /// Failure message payload.
    FailureMessage = 4,
}

/// Decoded `MOST\x01` store record (owned payload).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StoreRecord {
    /// Record kind tag.
    pub kind: StoreRecordKind,
    /// Logical sequence number (`<= i64::MAX`).
    pub sequence: u64,
    /// Service image key (`program_checksum ^ ((service as u64) << 32)`).
    pub image: u64,
    /// Payload bytes inside the `MOST\x01` frame.
    pub bytes: Vec<u8>,
}

/// Encodes a [`StoreRecord`] into a `MOST\x01` checksummed frame (`store_frame`).
#[must_use]
pub fn encode_frame(
    kind: StoreRecordKind,
    sequence: u64,
    image: u64,
    payload: &[u8],
) -> Option<Vec<u8>> {
    if payload.len() as u64 > MO_LIMIT_SAVED_BYTES || sequence > i64::MAX as u64 {
        return None;
    }
    let mut out = Vec::with_capacity(payload.len() + STORE_FRAME_BYTES);
    out.extend_from_slice(b"MOST\x01");
    out.push(kind as u8);
    out.extend_from_slice(&sequence.to_le_bytes());
    out.extend_from_slice(&image.to_le_bytes());
    let len = u32::try_from(payload.len()).ok()?;
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(payload);
    let checksum = fnv1a(FNV_OFFSET, &out);
    out.extend_from_slice(&checksum.to_le_bytes());
    Some(out)
}

/// Validates and decodes a `MOST\x01` frame (`mo_store_record_read`).
#[must_use]
pub fn decode_frame(kind: StoreRecordKind, bytes: &[u8]) -> Option<StoreRecord> {
    if bytes.len() < STORE_FRAME_BYTES
        || bytes.len() as u64 > MO_LIMIT_SAVED_BYTES + STORE_FRAME_BYTES as u64
    {
        return None;
    }
    if kind == StoreRecordKind::FailureMessage
        && bytes.len() as u64 > MO_LIMIT_FAILURE_MESSAGE_BYTES + STORE_FRAME_BYTES as u64
    {
        return None;
    }
    if &bytes[..5] != b"MOST\x01" || bytes[5] != kind as u8 {
        return None;
    }
    let mut pos = 6usize;
    let sequence = read_u64_le(bytes, &mut pos)?;
    if sequence > i64::MAX as u64 {
        return None;
    }
    let image = read_u64_le(bytes, &mut pos)?;
    let payload_len = read_u32_le(bytes, &mut pos)? as usize;
    let payload = bytes.get(pos..pos.checked_add(payload_len)?)?;
    pos += payload_len;
    let sum = read_u64_le(bytes, &mut pos)?;
    if pos != bytes.len() || sum != fnv1a(FNV_OFFSET, &bytes[..bytes.len() - 8]) {
        return None;
    }
    Some(StoreRecord {
        kind,
        sequence,
        image,
        bytes: payload.to_vec(),
    })
}

pub(crate) fn map_sqlite_error(err: &rusqlite::Error) -> StoreStatus {
    match err {
        rusqlite::Error::SqliteFailure(ffi, _) => match ffi.code {
            SqliteCode::DatabaseCorrupt | SqliteCode::NotADatabase | SqliteCode::TooBig => {
                StoreStatus::Corrupt
            }
            SqliteCode::DatabaseBusy | SqliteCode::DatabaseLocked => StoreStatus::Busy,
            _ => StoreStatus::Io,
        },
        _ => StoreStatus::Io,
    }
}

pub(crate) fn is_sqlite_busy(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(ffi, _)
            if matches!(ffi.code, SqliteCode::DatabaseBusy | SqliteCode::DatabaseLocked)
    )
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum TableState {
    Failed,
    Absent,
    Same,
    Different,
}

pub(crate) fn table_state(
    conn: &Connection,
    name: &str,
    expected_sql: &str,
) -> (TableState, StoreStatus) {
    let mut stmt = match conn.prepare("SELECT sql FROM sqlite_schema WHERE name=?1") {
        Ok(s) => s,
        Err(e) => return (TableState::Failed, map_sqlite_error(&e)),
    };
    let mut rows = match stmt.query([name]) {
        Ok(r) => r,
        Err(e) => return (TableState::Failed, map_sqlite_error(&e)),
    };
    let first = match rows.next() {
        Ok(r) => r,
        Err(e) => return (TableState::Failed, map_sqlite_error(&e)),
    };
    let Some(row) = first else {
        return (TableState::Absent, StoreStatus::Ok);
    };
    let stored: Option<String> = match row.get(0) {
        Ok(s) => s,
        Err(e) => return (TableState::Failed, map_sqlite_error(&e)),
    };
    if stored.as_deref() != Some(expected_sql) {
        return (TableState::Different, StoreStatus::Corrupt);
    }
    match rows.next() {
        Ok(None) => (TableState::Same, StoreStatus::Ok),
        Ok(Some(_)) => (TableState::Failed, StoreStatus::Corrupt),
        Err(e) => (TableState::Failed, map_sqlite_error(&e)),
    }
}

pub(crate) fn ensure_table_ready(
    conn: &Connection,
    name: &str,
    expected_sql: &str,
) -> Result<(), StoreStatus> {
    match table_state(conn, name, expected_sql) {
        (TableState::Same, _) => Ok(()),
        (TableState::Absent, _) => conn
            .execute_batch(expected_sql)
            .map_err(|e| map_sqlite_error(&e)),
        (TableState::Different, _) => Err(StoreStatus::Corrupt),
        (TableState::Failed, st) => Err(st),
    }
}

pub(crate) fn configure_defensive(conn: &Connection) -> Result<(), StoreStatus> {
    conn.set_limit(Limit::SQLITE_LIMIT_LENGTH, MO_LIMIT_STORE_ROW as i32)
        .map_err(|e| map_sqlite_error(&e))?;
    conn.set_limit(Limit::SQLITE_LIMIT_SQL_LENGTH, MO_LIMIT_STORE_SQL as i32)
        .map_err(|e| map_sqlite_error(&e))?;
    conn.set_db_config(DbConfig::SQLITE_DBCONFIG_DEFENSIVE, true)
        .map_err(|e| map_sqlite_error(&e))?;
    conn.set_db_config(DbConfig::SQLITE_DBCONFIG_ENABLE_TRIGGER, false)
        .map_err(|e| map_sqlite_error(&e))?;
    conn.set_db_config(DbConfig::SQLITE_DBCONFIG_ENABLE_VIEW, false)
        .map_err(|e| map_sqlite_error(&e))?;
    Ok(())
}

pub(crate) fn read_user_version(conn: &Connection) -> Result<i32, StoreStatus> {
    conn.query_row("PRAGMA user_version", [], |row| row.get::<_, i32>(0))
        .map_err(|e| map_sqlite_error(&e))
}

/// Exclusive handle to a durable SQLite service store (`struct mo_store`).
pub struct ServiceStore {
    pub(crate) conn: Connection,
    pub(crate) is_unsafe: bool,
    pub(crate) evicted_kinds: u64,
    slot_claimed: bool,
}

impl Drop for ServiceStore {
    fn drop(&mut self) {
        if self.slot_claimed {
            release_store_slot();
        }
    }
}

/// Snapshot and head metadata loaded from a store.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct SavedHead {
    pub(crate) head: u64,
    pub(crate) sequence: u64,
    pub(crate) image: u64,
    pub(crate) empty: bool,
}

impl ServiceStore {
    /// Opens an in-memory store (for tests and simulations) or an on-disk SQLite store
    /// without waiting for an existing lock (`mo_store_open`).
    pub fn open(path: Option<&Path>) -> Result<Self, StoreStatus> {
        Self::open_inner(path, false)
    }

    /// Opens an on-disk SQLite store, waiting up to `MO_LIMIT_SERVE_STORE_WAIT_US` for an
    /// exclusive lock (`mo_store_open_wait`).
    pub fn open_wait(path: &Path) -> Result<Self, StoreStatus> {
        Self::open_inner(Some(path), true)
    }

    fn open_inner(path: Option<&Path>, wait: bool) -> Result<Self, StoreStatus> {
        let mut existed_empty = false;
        if let Some(p) = path {
            let s = p.as_os_str();
            if s.is_empty() {
                return Err(StoreStatus::EmptyPath);
            }
            if s == ":memory:" {
                return Err(StoreStatus::MemoryPath);
            }
            if let Ok(meta) = std::fs::metadata(p)
                && meta.len() == 0
            {
                existed_empty = true;
                if !wait {
                    return Err(StoreStatus::Corrupt);
                }
            }
        }

        if !claim_store_slot() {
            return Err(StoreStatus::Handles);
        }

        let flags = OpenFlags::SQLITE_OPEN_READ_WRITE
            | OpenFlags::SQLITE_OPEN_CREATE
            | OpenFlags::SQLITE_OPEN_FULL_MUTEX;
        let conn = match path {
            Some(p) => {
                #[cfg(unix)]
                let prev_mask = rustix::process::umask(rustix::fs::Mode::from_bits_truncate(0o077));
                let res = Connection::open_with_flags(p, flags);
                #[cfg(unix)]
                let _ = rustix::process::umask(prev_mask);
                res
            }
            None => Connection::open_with_flags(":memory:", flags),
        };
        let conn = match conn {
            Ok(c) => c,
            Err(_) => {
                release_store_slot();
                return Err(StoreStatus::Io);
            }
        };

        let store = Self {
            conn,
            is_unsafe: false,
            evicted_kinds: 0,
            slot_claimed: true,
        };

        configure_defensive(&store.conn)?;
        if wait {
            store
                .conn
                .busy_timeout(Duration::from_millis(MO_LIMIT_SERVE_STORE_WAIT_US / 1000))
                .map_err(|_| StoreStatus::Io)?;
        }

        let init_sql = format!(
            "PRAGMA trusted_schema=OFF; PRAGMA journal_mode=DELETE;\
             PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY;\
             PRAGMA cache_size=-{MO_LIMIT_STORE_CACHE_KIB};\
             PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE"
        );
        if let Err(err) = store.conn.execute_batch(&init_sql) {
            let _ = store.conn.execute_batch("ROLLBACK");
            return Err(if is_sqlite_busy(&err) {
                StoreStatus::Busy
            } else {
                StoreStatus::Io
            });
        }

        if existed_empty && let Some(p) = path {
            let Ok(meta) = std::fs::metadata(p) else {
                let _ = store.conn.execute_batch("ROLLBACK");
                return Err(StoreStatus::Io);
            };
            if meta.len() == 0 {
                let _ = store.conn.execute_batch("ROLLBACK");
                return Err(StoreStatus::Corrupt);
            }
        }

        for &(name, ddl) in CORE_TABLES {
            if let Err(st) = ensure_table_ready(&store.conn, name, ddl) {
                let _ = store.conn.execute_batch("ROLLBACK");
                return Err(st);
            }
        }
        if let Err(err) = store.conn.execute_batch("COMMIT") {
            let _ = store.conn.execute_batch("ROLLBACK");
            return Err(if is_sqlite_busy(&err) {
                StoreStatus::Busy
            } else {
                StoreStatus::Io
            });
        }
        if wait {
            store
                .conn
                .busy_timeout(Duration::ZERO)
                .map_err(|_| StoreStatus::Io)?;
        }
        Ok(store)
    }

    /// Marks the store unsafe after an uncertain write failure.
    pub(crate) fn mark_unsafe(&mut self) {
        self.is_unsafe = true;
    }

    /// Returns `true` if a prior write failed with an uncertain commit outcome.
    #[must_use]
    pub fn is_unsafe(&self) -> bool {
        self.is_unsafe
    }

    /// Reads the current committed head sequence (`store_current_head`).
    pub(crate) fn current_head(&self) -> Result<Option<u64>, StoreStatus> {
        let mut stmt = self
            .conn
            .prepare("SELECT id, seq FROM head")
            .map_err(|e| map_sqlite_error(&e))?;
        let mut rows = stmt.query([]).map_err(|e| map_sqlite_error(&e))?;
        let Some(row) = rows.next().map_err(|e| map_sqlite_error(&e))? else {
            return Ok(None);
        };
        let id: i64 = row.get(0).map_err(|_| StoreStatus::Corrupt)?;
        let seq: i64 = row.get(1).map_err(|_| StoreStatus::Corrupt)?;
        if id != 1 || seq < 0 {
            return Err(StoreStatus::Corrupt);
        }
        if rows.next().map_err(|e| map_sqlite_error(&e))?.is_some() {
            return Err(StoreStatus::Corrupt);
        }
        Ok(Some(seq as u64))
    }

    pub(crate) fn load_snapshot_row(
        &self,
    ) -> Result<(SavedHead, Option<StoreRecord>), StoreStatus> {
        let version = read_user_version(&self.conn)?;
        let head_opt = self.current_head()?;
        let mut snap_stmt = self
            .conn
            .prepare("SELECT id, record FROM snapshot")
            .map_err(|e| map_sqlite_error(&e))?;
        let mut snap_rows = snap_stmt.query([]).map_err(|e| map_sqlite_error(&e))?;
        let first_snap = snap_rows.next().map_err(|e| map_sqlite_error(&e))?;

        let Some(head_seq) = head_opt else {
            if first_snap.is_some() || version != 0 {
                return Err(StoreStatus::Corrupt);
            }
            let has_log: Option<i64> = self
                .conn
                .query_row("SELECT 1 FROM log LIMIT 1", [], |r| r.get(0))
                .optional()
                .map_err(|e| map_sqlite_error(&e))?;
            if has_log.is_some() {
                return Err(StoreStatus::Corrupt);
            }
            return Ok((
                SavedHead {
                    head: 0,
                    sequence: 0,
                    image: 0,
                    empty: true,
                },
                None,
            ));
        };

        if version != 1 && version != 2 {
            return Err(StoreStatus::Corrupt);
        }
        let Some(row) = first_snap else {
            return Err(StoreStatus::Corrupt);
        };
        let id: i64 = row.get(0).map_err(|_| StoreStatus::Corrupt)?;
        let blob: Vec<u8> = row.get(1).map_err(|_| StoreStatus::Corrupt)?;
        if id != 1 {
            return Err(StoreStatus::Corrupt);
        }
        if snap_rows
            .next()
            .map_err(|e| map_sqlite_error(&e))?
            .is_some()
        {
            return Err(StoreStatus::Corrupt);
        }
        let rec = decode_frame(StoreRecordKind::State, &blob).ok_or(StoreStatus::Corrupt)?;
        if rec.sequence > head_seq {
            return Err(StoreStatus::Corrupt);
        }
        let sh = SavedHead {
            head: head_seq,
            sequence: rec.sequence,
            image: rec.image,
            empty: false,
        };
        Ok((sh, Some(rec)))
    }

    /// Loads the snapshot and every consecutive log record in one read transaction
    /// (`MO_STORE_LOAD`). Returns an empty `Vec` for a fresh store.
    pub fn load(&mut self) -> Result<Vec<StoreRecord>, StoreStatus> {
        if self.is_unsafe {
            return Err(StoreStatus::Io);
        }
        self.conn
            .execute_batch("BEGIN")
            .map_err(|e| map_sqlite_error(&e))?;
        let res = self.load_in_tx();
        let _ = self.conn.execute_batch("COMMIT");
        res
    }

    fn load_in_tx(&self) -> Result<Vec<StoreRecord>, StoreStatus> {
        let (sh, snap_opt) = self.load_snapshot_row()?;
        if sh.empty {
            return Ok(Vec::new());
        }
        let Some(snap) = snap_opt else {
            return Err(StoreStatus::Corrupt);
        };
        let mut out = vec![snap];
        let mut expected = sh.sequence.checked_add(1).ok_or(StoreStatus::Corrupt)?;

        let mut stmt = self
            .conn
            .prepare("SELECT seq, record FROM log ORDER BY seq")
            .map_err(|e| map_sqlite_error(&e))?;
        let mut rows = stmt.query([]).map_err(|e| map_sqlite_error(&e))?;
        while let Some(row) = rows.next().map_err(|e| map_sqlite_error(&e))? {
            let seq: i64 = row.get(0).map_err(|_| StoreStatus::Corrupt)?;
            let blob: Vec<u8> = row.get(1).map_err(|_| StoreStatus::Corrupt)?;
            if seq < 0 || seq as u64 != expected || expected > sh.head {
                return Err(StoreStatus::Corrupt);
            }
            let rec = decode_frame(StoreRecordKind::Log, &blob).ok_or(StoreStatus::Corrupt)?;
            if rec.sequence != expected || rec.image != sh.image {
                return Err(StoreStatus::Corrupt);
            }
            out.push(rec);
            expected = expected.checked_add(1).ok_or(StoreStatus::Corrupt)?;
        }
        if expected - 1 != sh.head {
            return Err(StoreStatus::Corrupt);
        }
        Ok(out)
    }

    /// Appends one log record at `sequence == head + 1` (`MO_STORE_APPEND`).
    pub fn append(&mut self, record: &StoreRecord) -> Result<(), StoreStatus> {
        if self.is_unsafe || record.kind != StoreRecordKind::Log {
            return Err(StoreStatus::Io);
        }
        let frame = encode_frame(record.kind, record.sequence, record.image, &record.bytes)
            .ok_or(StoreStatus::Corrupt)?;
        if self.conn.execute_batch("BEGIN IMMEDIATE").is_err() {
            self.mark_unsafe();
            return Err(StoreStatus::Io);
        }
        let ok = (|| -> Result<(), StoreStatus> {
            let Some(head) = self.current_head()? else {
                return Err(StoreStatus::Corrupt);
            };
            if record.sequence != head.wrapping_add(1) {
                return Err(StoreStatus::Corrupt);
            }
            self.conn
                .execute(
                    "INSERT INTO log(seq,record) VALUES(?1,?2)",
                    params![record.sequence as i64, frame.as_slice()],
                )
                .map_err(|e| map_sqlite_error(&e))?;
            let changed = self
                .conn
                .execute(
                    "UPDATE head SET seq=?1 WHERE id=1",
                    params![record.sequence as i64],
                )
                .map_err(|e| map_sqlite_error(&e))?;
            if changed != 1 {
                return Err(StoreStatus::Corrupt);
            }
            self.conn
                .execute_batch("COMMIT")
                .map_err(|e| map_sqlite_error(&e))
        })();
        if let Err(st) = ok {
            let _ = self.conn.execute_batch("ROLLBACK");
            self.mark_unsafe();
            return Err(st);
        }
        Ok(())
    }

    /// Writes an authoritative state snapshot at `sequence` (0 on initial start, or `== head`)
    /// and prunes covered log rows atomically (`MO_STORE_SNAPSHOT`).
    pub fn snapshot(&mut self, record: &StoreRecord) -> Result<(), StoreStatus> {
        if self.is_unsafe || record.kind != StoreRecordKind::State {
            return Err(StoreStatus::Io);
        }
        let frame = encode_frame(record.kind, record.sequence, record.image, &record.bytes)
            .ok_or(StoreStatus::Corrupt)?;
        if self.conn.execute_batch("BEGIN IMMEDIATE").is_err() {
            self.mark_unsafe();
            return Err(StoreStatus::Io);
        }
        let ok = (|| -> Result<(), StoreStatus> {
            let head = self.current_head()?;
            match head {
                None => {
                    if record.sequence != 0 {
                        return Err(StoreStatus::Corrupt);
                    }
                    let version = read_user_version(&self.conn)?;
                    if version != 0 {
                        return Err(StoreStatus::Corrupt);
                    }
                    self.conn
                        .execute("INSERT INTO head(id,seq) VALUES(1,0)", [])
                        .map_err(|e| map_sqlite_error(&e))?;
                    self.conn
                        .execute(
                            "INSERT INTO snapshot(id,record) VALUES(1,?1)",
                            params![frame.as_slice()],
                        )
                        .map_err(|e| map_sqlite_error(&e))?;
                    self.conn
                        .execute_batch("PRAGMA user_version=1")
                        .map_err(|e| map_sqlite_error(&e))?;
                }
                Some(h) => {
                    if record.sequence != h {
                        return Err(StoreStatus::Corrupt);
                    }
                    let changed = self
                        .conn
                        .execute(
                            "UPDATE snapshot SET record=?1 WHERE id=1",
                            params![frame.as_slice()],
                        )
                        .map_err(|e| map_sqlite_error(&e))?;
                    if changed != 1 {
                        return Err(StoreStatus::Corrupt);
                    }
                    self.conn
                        .execute(
                            "DELETE FROM log WHERE seq<=?1",
                            params![record.sequence as i64],
                        )
                        .map_err(|e| map_sqlite_error(&e))?;
                }
            }
            self.conn
                .execute_batch("COMMIT")
                .map_err(|e| map_sqlite_error(&e))
        })();
        if let Err(st) = ok {
            let _ = self.conn.execute_batch("ROLLBACK");
            self.mark_unsafe();
            return Err(st);
        }
        Ok(())
    }
}
