//! Offline stopped-folder history reader and journal recovery (`src/host/store-history.c` parity).

use std::path::Path;

use mo_bytecode::{FNV_OFFSET, fnv1a};
use mo_def::limits::{
    MO_LIMIT_FAILURE_KIND_BYTES, MO_LIMIT_FAILURE_RECORDS, MO_LIMIT_HISTORY_BYTES,
    MO_LIMIT_SAVED_BYTES, MO_LIMIT_STORE_CACHE_KIB,
};
use rusqlite::{
    Connection, ErrorCode as SqliteCode, MAIN_DB, OpenFlags, OptionalExtension, params,
};

use crate::binary::failure::read_failure_record;
use crate::binary::log_record::walk_log_record;
use crate::binary::{read_u32_le, read_u64_le};

use super::programs::{StoredProgram, UPGRADE_TABLES, parse_program_row, read_active_program_conn};
use super::{
    CORE_TABLES, FRAME_HEADER_BYTES, STORE_FRAME_BYTES, StoreRecord, StoreRecordKind, StoreStatus,
    TableState, claim_store_slot, configure_defensive, decode_frame, map_sqlite_error,
    read_user_version, release_store_slot, table_state,
};

/// Offline folder inspection status (`enum mo_store_folder`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FolderStatus {
    /// Folder was read cleanly.
    Ok,
    /// `service.sqlite` does not exist.
    Missing,
    /// Another process (e.g. live `mo serve`) holds the database lock.
    Owned,
    /// An unfinished write left a hot rollback journal; `--recover` is required.
    Recovery,
    /// File permissions forbid reading.
    Unreadable,
    /// File is not a valid service store or failed integrity checks.
    Damaged,
    /// Transient filesystem/SQLite I/O error.
    Io,
    /// Memory budget refused.
    Memory,
    /// Too many open stores.
    Handles,
}

/// One log record's metadata in [`StoreHistory`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HistoryEntry {
    /// Sequence number.
    pub sequence: u64,
    /// Arrival timestamp in microseconds since epoch.
    pub arrival: i64,
    /// Handler or timer index.
    pub handler: u32,
    /// `true` for timer ticks.
    pub timer: bool,
    /// Encoded argument byte lengths.
    pub argument_bytes: Vec<u32>,
}

/// One failure record summary in [`StoreHistory`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HistoryFailure {
    /// Failure id.
    pub id: u64,
    /// Failure kind label.
    pub kind: String,
    /// Whether this was the first failure of its kind.
    pub first: bool,
    /// Whether the failure's state frame header & checksum validated.
    pub state_valid: bool,
    /// State payload byte length.
    pub state_bytes: usize,
    /// Decoded failure message record.
    pub message: StoreRecord,
}

/// One upgrade history row in [`StoreHistory`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HistoryUpgrade {
    /// Upgrade ordinal.
    pub ordinal: u64,
    /// Service head sequence when upgraded.
    pub head: u64,
    /// Previous source digest (all-zero for legacy adoption).
    pub old_source: [u8; 32],
    /// New source digest.
    pub new_source: [u8; 32],
    /// Whether the previous program is retained in `previous_programs`.
    pub program_retained: bool,
}

/// Complete offline snapshot of a stopped service store (`struct mo_store_history`).
#[derive(Debug, Clone)]
pub struct StoreHistory {
    /// Active recorded program, if present.
    pub program: Option<StoredProgram>,
    /// Retained previous program identities (bundle/context empty).
    pub previous_programs: Vec<StoredProgram>,
    /// Recorded upgrades in ascending ordinal order.
    pub upgrades: Vec<HistoryUpgrade>,
    /// Total upgrade count.
    pub upgrade_count: u64,
    /// Whether the upgrade list was truncated.
    pub upgrades_truncated: bool,
    /// `true` when `--recover` rolled back a hot journal.
    pub recovered: bool,
    /// `true` if `failure` and `failure_state` tables exist.
    pub failure_tables: bool,
    /// Authoritative state snapshot (`bytes` may be empty if omitted due to `MO_LIMIT_HISTORY_BYTES`).
    pub snapshot: Option<StoreRecord>,
    /// Whether the snapshot bytes were copied (vs. omitted for budget).
    pub snapshot_copied: bool,
    /// Last committed head sequence.
    pub head: u64,
    /// First damaged log sequence, if any (`0` otherwise).
    pub damaged_sequence: u64,
    /// Retained log entry metadata.
    pub entries: Vec<HistoryEntry>,
    /// Total log row count.
    pub log_count: u32,
    /// Retained failure summaries.
    pub failures: Vec<HistoryFailure>,
    /// Total failure row count.
    pub failure_total: u32,
    /// Bytes retained within `MO_LIMIT_HISTORY_BYTES`.
    pub retained_bytes: u64,
    /// Bytes not retained due to `MO_LIMIT_HISTORY_BYTES`.
    pub left_bytes: u64,
}

fn classify_open_io_error(path: &Path) -> FolderStatus {
    match std::fs::metadata(path) {
        Err(e) => match e.kind() {
            std::io::ErrorKind::NotFound => FolderStatus::Missing,
            std::io::ErrorKind::PermissionDenied => FolderStatus::Unreadable,
            _ => FolderStatus::Io,
        },
        Ok(meta) => {
            if !meta.is_file() {
                return FolderStatus::Damaged;
            }
            if meta.len() == 0 {
                return FolderStatus::Damaged;
            }
            FolderStatus::Io
        }
    }
}

fn classify_sqlite_folder(err: &rusqlite::Error, path: &Path) -> FolderStatus {
    if let rusqlite::Error::SqliteFailure(ffi, _) = err {
        return match ffi.code {
            SqliteCode::CannotOpen => classify_open_io_error(path),
            SqliteCode::ReadOnly => FolderStatus::Recovery,
            SqliteCode::PermissionDenied => FolderStatus::Unreadable,
            SqliteCode::DatabaseBusy | SqliteCode::DatabaseLocked => FolderStatus::Owned,
            SqliteCode::DatabaseCorrupt | SqliteCode::NotADatabase | SqliteCode::TooBig => {
                FolderStatus::Damaged
            }
            SqliteCode::OutOfMemory => FolderStatus::Memory,
            _ => FolderStatus::Io,
        };
    }
    FolderStatus::Io
}

fn open_offline(path: &Path, recover: bool) -> Result<(Connection, bool), FolderStatus> {
    if path.as_os_str().is_empty() || path.as_os_str() == ":memory:" {
        return Err(FolderStatus::Damaged);
    }
    match std::fs::metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err(FolderStatus::Missing),
        Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => {
            return Err(FolderStatus::Unreadable);
        }
        Err(_) => return Err(FolderStatus::Io),
        Ok(meta) => {
            if !meta.is_file() || meta.len() == 0 {
                return Err(FolderStatus::Damaged);
            }
        }
    }

    if !claim_store_slot() {
        return Err(FolderStatus::Handles);
    }

    let open_ro = || {
        let conn = Connection::open_with_flags(
            path,
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_FULL_MUTEX,
        )
        .map_err(|e| classify_sqlite_folder(&e, path))?;
        configure_defensive(&conn).map_err(store_to_folder)?;
        conn.execute_batch(&format!(
            "PRAGMA trusted_schema=OFF; PRAGMA temp_store=MEMORY; \
             PRAGMA cache_size=-{MO_LIMIT_STORE_CACHE_KIB}; BEGIN"
        ))
        .map_err(|e| classify_sqlite_folder(&e, path))?;
        // Trigger schema read to detect hot rollback journals or exclusive locks.
        match conn.query_row("SELECT count(*) FROM sqlite_schema", [], |r| {
            r.get::<_, i64>(0)
        }) {
            Ok(_) => Ok(conn),
            Err(e) => {
                let _ = conn.execute_batch("ROLLBACK");
                if let rusqlite::Error::SqliteFailure(ffi, _) = &e {
                    // SQLite returns SQLITE_READONLY (8) or SQLITE_READONLY_RECOVERY (264)
                    // when a hot rollback journal prevents a read-only open.
                    if ffi.code == SqliteCode::ReadOnly {
                        return Err(FolderStatus::Recovery);
                    }
                    if matches!(
                        ffi.code,
                        SqliteCode::DatabaseBusy | SqliteCode::DatabaseLocked
                    ) {
                        return Err(FolderStatus::Owned);
                    }
                }
                Err(classify_sqlite_folder(&e, path))
            }
        }
    };

    match open_ro() {
        Ok(conn) => Ok((conn, false)),
        Err(FolderStatus::Recovery) if recover => {
            let conn = Connection::open_with_flags(
                path,
                OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_FULL_MUTEX,
            )
            .map_err(|e| {
                release_store_slot();
                classify_sqlite_folder(&e, path)
            })?;
            if let Err(st) = configure_defensive(&conn).map_err(store_to_folder) {
                release_store_slot();
                return Err(st);
            }
            let init = format!(
                "PRAGMA trusted_schema=OFF; PRAGMA temp_store=MEMORY; \
                 PRAGMA cache_size=-{MO_LIMIT_STORE_CACHE_KIB}; BEGIN IMMEDIATE"
            );
            if let Err(e) = conn.execute_batch(&init) {
                let _ = conn.execute_batch("ROLLBACK");
                release_store_slot();
                return Err(classify_sqlite_folder(&e, path));
            }
            // Rolling back a hot journal happens on the first read/write transaction.
            if let Err(e) = conn.execute_batch("COMMIT; BEGIN") {
                let _ = conn.execute_batch("ROLLBACK");
                release_store_slot();
                return Err(classify_sqlite_folder(&e, path));
            }
            Ok((conn, true))
        }
        Err(st) => {
            release_store_slot();
            Err(st)
        }
    }
}

fn store_to_folder(st: StoreStatus) -> FolderStatus {
    match st {
        StoreStatus::Ok => FolderStatus::Ok,
        StoreStatus::Corrupt | StoreStatus::EmptyPath | StoreStatus::MemoryPath => {
            FolderStatus::Damaged
        }
        StoreStatus::Busy => FolderStatus::Owned,
        StoreStatus::Memory => FolderStatus::Memory,
        StoreStatus::Handles => FolderStatus::Handles,
        StoreStatus::Io => FolderStatus::Io,
    }
}

struct OfflineGuard {
    conn: Connection,
}

impl Drop for OfflineGuard {
    fn drop(&mut self) {
        let _ = self.conn.execute_batch("ROLLBACK");
        release_store_slot();
    }
}

/// Reads only the active recorded program from a stopped store file without modifying it
/// (`mo_store_program_read_file`).
pub fn read_program_from_file(path: &Path) -> Result<Option<StoredProgram>, FolderStatus> {
    let (conn, _recovered) = open_offline(path, false)?;
    let guard = OfflineGuard { conn };
    read_active_program_conn(&guard.conn).map_err(store_to_folder)
}

/// Streams and validates a `MOST\x01` frame via SQLite incremental blob I/O without loading
/// the full blob into RAM, returning `(sequence, image, payload_len)` on success.
fn verify_most_blob_streaming(
    conn: &Connection,
    table: &str,
    column: &str,
    rowid: i64,
    expected_kind: StoreRecordKind,
) -> Option<(u64, u64, usize)> {
    let blob = conn.blob_open(MAIN_DB, table, column, rowid, true).ok()?;
    let total = blob.len();
    if total < STORE_FRAME_BYTES || total as u64 > MO_LIMIT_SAVED_BYTES + STORE_FRAME_BYTES as u64 {
        return None;
    }
    let mut header = [0u8; FRAME_HEADER_BYTES];
    blob.read_at_exact(&mut header, 0).ok()?;
    if &header[..5] != b"MOST\x01" || header[5] != expected_kind as u8 {
        return None;
    }
    let mut pos = 6usize;
    let sequence = read_u64_le(&header, &mut pos)?;
    if sequence > i64::MAX as u64 {
        return None;
    }
    let image = read_u64_le(&header, &mut pos)?;
    let payload_len = read_u32_le(&header, &mut pos)? as usize;
    if payload_len + STORE_FRAME_BYTES != total {
        return None;
    }

    let mut hash = fnv1a(FNV_OFFSET, &header);
    let mut buf = [0u8; 8192];
    let mut offset = FRAME_HEADER_BYTES;
    let payload_end = FRAME_HEADER_BYTES + payload_len;
    while offset < payload_end {
        let take = (payload_end - offset).min(buf.len());
        blob.read_at_exact(&mut buf[..take], offset).ok()?;
        hash = fnv1a(hash, &buf[..take]);
        offset += take;
    }
    let mut tail = [0u8; 8];
    blob.read_at_exact(&mut tail, payload_end).ok()?;
    let stored = u64::from_le_bytes(tail);
    if hash != stored {
        return None;
    }
    Some((sequence, image, payload_len))
}

/// Reads a stopped service's full history (`mo_store_history_read`).
///
/// On error, returns `(status, recovered, damaged_sequence)`.
pub fn read_history(path: &Path, recover: bool) -> Result<StoreHistory, (FolderStatus, bool, u64)> {
    let (conn, recovered) = open_offline(path, recover).map_err(|st| (st, false, 0))?;
    let guard = OfflineGuard { conn };

    // Verify core tables: head, snapshot, log must exist with exact DDL.
    for &(name, ddl) in &CORE_TABLES[..3] {
        match table_state(&guard.conn, name, ddl) {
            (TableState::Same, _) => {}
            (TableState::Failed, st) => return Err((store_to_folder(st), recovered, 0)),
            _ => return Err((FolderStatus::Damaged, recovered, 0)),
        }
    }
    // Failure tables: both present or both absent.
    let f0 = table_state(&guard.conn, CORE_TABLES[3].0, CORE_TABLES[3].1);
    let f1 = table_state(&guard.conn, CORE_TABLES[4].0, CORE_TABLES[4].1);
    let failure_tables = match (f0.0, f1.0) {
        (TableState::Same, TableState::Same) => true,
        (TableState::Absent, TableState::Absent) => false,
        (TableState::Failed, _) => return Err((store_to_folder(f0.1), recovered, 0)),
        (_, TableState::Failed) => return Err((store_to_folder(f1.1), recovered, 0)),
        _ => return Err((FolderStatus::Damaged, recovered, 0)),
    };

    let version =
        read_user_version(&guard.conn).map_err(|st| (store_to_folder(st), recovered, 0))?;

    // Read head & snapshot.
    let head_row: Option<(i64, i64)> = guard
        .conn
        .query_row("SELECT id, seq FROM head", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .optional()
        .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;

    let mut retained_bytes = 0u64;
    let mut left_bytes = 0u64;
    let budget = MO_LIMIT_HISTORY_BYTES;

    let (has_snapshot, head, snap_seq, snap_image, snapshot, snapshot_copied) = match head_row {
        None => {
            if version != 0 {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            let any_snap: Option<i64> = guard
                .conn
                .query_row("SELECT 1 FROM snapshot LIMIT 1", [], |r| r.get(0))
                .optional()
                .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
            let any_log: Option<i64> = guard
                .conn
                .query_row("SELECT 1 FROM log LIMIT 1", [], |r| r.get(0))
                .optional()
                .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
            if any_snap.is_some() || any_log.is_some() {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            (false, 0u64, 0u64, 0u64, None, false)
        }
        Some((id, seq)) => {
            if id != 1 || seq < 0 || (version != 1 && version != 2) {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            let head = seq as u64;
            let snap_blob: Vec<u8> = guard
                .conn
                .query_row("SELECT record FROM snapshot WHERE id=1", [], |r| r.get(0))
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let rec = decode_frame(StoreRecordKind::State, &snap_blob).ok_or((
                FolderStatus::Damaged,
                recovered,
                0,
            ))?;
            if rec.sequence > head {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            let sseq = rec.sequence;
            let simg = rec.image;
            let bytes_len = rec.bytes.len() as u64;
            if retained_bytes + bytes_len <= budget {
                retained_bytes += bytes_len;
                (true, head, sseq, simg, Some(rec), true)
            } else {
                left_bytes += bytes_len;
                let stub = StoreRecord {
                    kind: rec.kind,
                    sequence: rec.sequence,
                    image: rec.image,
                    bytes: Vec::new(),
                };
                (true, head, sseq, simg, Some(stub), false)
            }
        }
    };

    // Walk every log row via streaming blob I/O.
    let mut entries = Vec::new();
    let mut log_count = 0u32;
    if has_snapshot {
        let mut expected = snap_seq
            .checked_add(1)
            .ok_or((FolderStatus::Damaged, recovered, 0))?;
        let mut stmt = guard
            .conn
            .prepare("SELECT rowid, seq FROM log ORDER BY seq")
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        let mut rows = stmt
            .query([])
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        while let Some(row) = rows
            .next()
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?
        {
            let rowid: i64 = row
                .get(0)
                .map_err(|_| (FolderStatus::Damaged, recovered, expected))?;
            let seq: i64 = row
                .get(1)
                .map_err(|_| (FolderStatus::Damaged, recovered, expected))?;
            if seq < 0 || seq as u64 != expected || expected > head {
                return Err((FolderStatus::Damaged, recovered, expected));
            }
            let Some((fseq, fimg, payload_len)) = verify_most_blob_streaming(
                &guard.conn,
                "log",
                "record",
                rowid,
                StoreRecordKind::Log,
            ) else {
                return Err((FolderStatus::Damaged, recovered, expected));
            };
            if fseq != expected || fimg != snap_image {
                return Err((FolderStatus::Damaged, recovered, expected));
            }
            let blob = guard
                .conn
                .blob_open(MAIN_DB, "log", "record", rowid, true)
                .map_err(|_| (FolderStatus::Damaged, recovered, expected))?;
            let Some(meta) = walk_log_record(payload_len, |off, out| {
                blob.read_at_exact(out, FRAME_HEADER_BYTES + off).is_ok()
            }) else {
                return Err((FolderStatus::Damaged, recovered, expected));
            };
            log_count = log_count.saturating_add(1);
            let entry_cost = 64u64 + (meta.argument_bytes.len() as u64) * 4;
            if retained_bytes + entry_cost <= budget {
                retained_bytes += entry_cost;
                entries.push(HistoryEntry {
                    sequence: expected,
                    arrival: meta.header.arrival,
                    handler: meta.header.handler,
                    timer: meta.header.timer,
                    argument_bytes: meta.argument_bytes,
                });
            } else {
                left_bytes += entry_cost;
            }
            expected =
                expected
                    .checked_add(1)
                    .ok_or((FolderStatus::Damaged, recovered, expected))?;
        }
        if expected - 1 != head {
            return Err((FolderStatus::Damaged, recovered, expected));
        }
    }

    // Read failures if failure tables exist.
    let mut failures = Vec::new();
    let mut failure_total = 0u32;
    if failure_tables {
        let mut stmt = guard
            .conn
            .prepare(
                "SELECT f.id,f.kind,f.first,f.message,s.rowid,length(s.record) \
                 FROM failure f JOIN failure_state s ON f.id=s.id ORDER BY f.id",
            )
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        let mut rows = stmt
            .query([])
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        while let Some(row) = rows
            .next()
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?
        {
            let id: i64 = row
                .get(0)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let kind: String = row
                .get(1)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let first: i64 = row
                .get(2)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let msg_blob: Vec<u8> = row
                .get(3)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let s_rowid: i64 = row
                .get(4)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let s_len: i64 = row
                .get(5)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            if id <= 0
                || kind.is_empty()
                || kind.len() as u64 > MO_LIMIT_FAILURE_KIND_BYTES
                || (first != 0 && first != 1)
                || s_len < 0
                || failure_total as u64 >= MO_LIMIT_FAILURE_RECORDS
            {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            let message = decode_frame(StoreRecordKind::FailureMessage, &msg_blob).ok_or((
                FolderStatus::Damaged,
                recovered,
                0,
            ))?;
            let parsed =
                read_failure_record(&message.bytes).ok_or((FolderStatus::Damaged, recovered, 0))?;
            if parsed.id != id as u64 {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            let state_check = verify_most_blob_streaming(
                &guard.conn,
                "failure_state",
                "record",
                s_rowid,
                StoreRecordKind::FailureState,
            );
            let (state_valid, state_bytes) = match state_check {
                Some((sseq, simg, plen)) if sseq == message.sequence && simg == message.image => {
                    (true, plen)
                }
                _ => (false, (s_len as usize).saturating_sub(STORE_FRAME_BYTES)),
            };
            failure_total += 1;
            let cost = message.bytes.len() as u64 + kind.len() as u64 + 64;
            if retained_bytes + cost <= budget {
                retained_bytes += cost;
                failures.push(HistoryFailure {
                    id: id as u64,
                    kind,
                    first: first != 0,
                    state_valid,
                    state_bytes,
                    message,
                });
            } else {
                left_bytes += cost;
            }
        }
    }

    // Read program / previous_programs / upgrades if present.
    let program =
        read_active_program_conn(&guard.conn).map_err(|st| (store_to_folder(st), recovered, 0))?;
    let mut previous_programs = Vec::new();
    let mut upgrades = Vec::new();
    let mut upgrade_count = 0u64;
    let mut upgrades_truncated = false;

    if table_state(&guard.conn, UPGRADE_TABLES[2].0, UPGRADE_TABLES[2].1).0 == TableState::Same {
        let mut stmt = guard
            .conn
            .prepare(
                "SELECT ordinal,source,image,build,schema,bundle,context_digest,context \
                 FROM previous_programs ORDER BY ordinal",
            )
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        let mut rows = stmt
            .query([])
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        while let Some(row) = rows
            .next()
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?
        {
            let mut p = parse_program_row(row).ok_or((FolderStatus::Damaged, recovered, 0))?;
            p.bundle.clear();
            p.context.clear();
            previous_programs.push(p);
        }
    }

    if table_state(&guard.conn, UPGRADE_TABLES[1].0, UPGRADE_TABLES[1].1).0 == TableState::Same {
        let mut stmt = guard
            .conn
            .prepare("SELECT ordinal,head,old_source,new_source FROM upgrades ORDER BY ordinal")
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        let mut rows = stmt
            .query([])
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?;
        while let Some(row) = rows
            .next()
            .map_err(|e| (store_to_folder(map_sqlite_error(&e)), recovered, 0))?
        {
            let ordinal: i64 = row
                .get(0)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let uhead: i64 = row
                .get(1)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let old_s: Vec<u8> = row
                .get(2)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            let new_s: Vec<u8> = row
                .get(3)
                .map_err(|_| (FolderStatus::Damaged, recovered, 0))?;
            if ordinal < 0 || uhead < 0 || old_s.len() != 32 || new_s.len() != 32 {
                return Err((FolderStatus::Damaged, recovered, 0));
            }
            let mut old_source = [0u8; 32];
            old_source.copy_from_slice(&old_s);
            let mut new_source = [0u8; 32];
            new_source.copy_from_slice(&new_s);
            upgrade_count += 1;
            let cost = 96u64;
            if retained_bytes + cost <= budget {
                retained_bytes += cost;
                let retained = previous_programs
                    .iter()
                    .any(|p| p.ordinal + 1 == ordinal as u64 && p.source == old_source);
                upgrades.push(HistoryUpgrade {
                    ordinal: ordinal as u64,
                    head: uhead as u64,
                    old_source,
                    new_source,
                    program_retained: retained,
                });
            } else {
                left_bytes += cost;
                upgrades_truncated = true;
            }
        }
    }

    let _ = params![];
    Ok(StoreHistory {
        program,
        previous_programs,
        upgrades,
        upgrade_count,
        upgrades_truncated,
        recovered,
        failure_tables,
        snapshot,
        snapshot_copied,
        head,
        damaged_sequence: 0,
        entries,
        log_count,
        failures,
        failure_total,
        retained_bytes,
        left_bytes,
    })
}
