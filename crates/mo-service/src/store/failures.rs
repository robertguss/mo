//! Failure capture, bounded retention/eviction, catalog, and export queries
//! (`src/host/store-failures.c` parity).

use mo_def::limits::{
    MO_LIMIT_FAILURE_KIND_BYTES, MO_LIMIT_FAILURE_MESSAGE_BYTES, MO_LIMIT_FAILURE_RECORDS,
    MO_LIMIT_FAILURE_STORE_BYTES, MO_LIMIT_FAILURE_VIEW_RECORDS,
};
use rusqlite::{OptionalExtension, params};

use crate::binary::failure::{FAILURE_ID_OFFSET, read_failure_record};

use super::{
    STORE_FRAME_BYTES, ServiceStore, StoreRecord, StoreRecordKind, StoreStatus, decode_frame,
    encode_frame, map_sqlite_error,
};

/// Outcome of capturing a diagnostic failure pair (`enum mo_store_capture_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureStatus {
    /// Captured and committed; returned id is valid.
    Ok(u64),
    /// Refused safely (rolled back and recovery head verified).
    Refused,
    /// Refused because existing diagnostic rows failed validation.
    Damaged,
    /// Write failed with uncertain commit outcome; store must be closed.
    Unsafe,
}

/// One stored failure entry (`struct mo_store_failure`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StoredFailure {
    /// Store-assigned failure id (`>= 1`).
    pub id: u64,
    /// Failure kind label.
    pub kind: String,
    /// `true` if this was the first failure of its kind.
    pub first: bool,
    /// State record (`bytes` is empty when returned from catalog metadata queries).
    pub state: StoreRecord,
    /// State frame byte length (always populated, even in catalog queries).
    pub state_bytes: usize,
    /// Failure message record.
    pub message: StoreRecord,
}

struct FailureRowMeta {
    id: u64,
    bytes: usize,
    first: bool,
    same_kind: bool,
    kind: String,
}

fn kind_valid(kind: &str) -> bool {
    !kind.is_empty()
        && kind.len() as u64 <= MO_LIMIT_FAILURE_KIND_BYTES
        && !kind.as_bytes().contains(&0)
}

#[derive(Debug, Clone, Copy)]
enum PairCheck {
    Ok,
    Damaged,
    Io,
}

impl ServiceStore {
    fn check_failure_pairs_complete(&self) -> PairCheck {
        let sql = format!(
            "SELECT (SELECT count(*) FROM (SELECT id FROM failure LIMIT {})),\
             (SELECT count(*) FROM (SELECT id FROM failure_state LIMIT {}))",
            MO_LIMIT_FAILURE_RECORDS + 1,
            MO_LIMIT_FAILURE_RECORDS + 1
        );
        let counts: Result<(i64, i64), _> = self
            .conn
            .query_row(&sql, [], |r| Ok((r.get(0)?, r.get(1)?)));
        let Ok((fc, sc)) = counts else {
            return PairCheck::Io;
        };
        if fc < 0 || fc as u64 > MO_LIMIT_FAILURE_RECORDS || fc != sc {
            return PairCheck::Damaged;
        }
        let orphan: Result<Option<i64>, _> = self
            .conn
            .query_row(
                "SELECT 1 FROM failure f LEFT JOIN failure_state s ON f.id=s.id \
                 WHERE s.id IS NULL OR f.id<=0 LIMIT 1",
                [],
                |r| r.get(0),
            )
            .optional();
        match orphan {
            Ok(None) => PairCheck::Ok,
            Ok(Some(_)) => PairCheck::Damaged,
            Err(_) => PairCheck::Io,
        }
    }

    fn scan_failure_rows(
        &self,
        target_kind: &str,
    ) -> Result<(Vec<FailureRowMeta>, usize), PairCheck> {
        let mut stmt = self
            .conn
            .prepare(
                "SELECT f.id,f.kind,f.first,length(f.message)+length(s.record) \
                 FROM failure f JOIN failure_state s ON f.id=s.id ORDER BY f.id",
            )
            .map_err(|_| PairCheck::Io)?;
        let mut rows = stmt.query([]).map_err(|_| PairCheck::Io)?;
        let mut out = Vec::new();
        let mut total = 0usize;
        while let Some(row) = rows.next().map_err(|_| PairCheck::Io)? {
            let id: i64 = row.get(0).map_err(|_| PairCheck::Damaged)?;
            let kind: String = row.get(1).map_err(|_| PairCheck::Damaged)?;
            let first: i64 = row.get(2).map_err(|_| PairCheck::Damaged)?;
            let bytes: i64 = row.get(3).map_err(|_| PairCheck::Damaged)?;
            if out.len() as u64 == MO_LIMIT_FAILURE_RECORDS
                || id <= 0
                || !kind_valid(&kind)
                || bytes <= 0
                || bytes as u64 > MO_LIMIT_FAILURE_STORE_BYTES
                || (first != 0 && first != 1)
                || (bytes as usize) > (MO_LIMIT_FAILURE_STORE_BYTES as usize).saturating_sub(total)
            {
                return Err(PairCheck::Damaged);
            }
            total += bytes as usize;
            let same_kind = kind == target_kind;
            out.push(FailureRowMeta {
                id: id as u64,
                bytes: bytes as usize,
                first: first != 0,
                same_kind,
                kind,
            });
        }
        Ok((out, total))
    }

    /// Captures a failure state + message pair under bounded retention/eviction
    /// (`mo_store_capture`).
    pub fn capture_failure(
        &mut self,
        kind: &str,
        state: &StoreRecord,
        message: &StoreRecord,
    ) -> CaptureStatus {
        if self.is_unsafe {
            return CaptureStatus::Unsafe;
        }
        if !kind_valid(kind)
            || state.kind != StoreRecordKind::FailureState
            || message.kind != StoreRecordKind::FailureMessage
            || message.bytes.len() < FAILURE_ID_OFFSET + 8
            || message.bytes.len() as u64 > MO_LIMIT_FAILURE_MESSAGE_BYTES
            || state.sequence != message.sequence
            || state.image != message.image
        {
            return CaptureStatus::Refused;
        }
        let Some(state_frame) = encode_frame(state.kind, state.sequence, state.image, &state.bytes)
        else {
            return CaptureStatus::Refused;
        };
        let entry_bytes = state_frame.len() + message.bytes.len() + STORE_FRAME_BYTES;
        if entry_bytes as u64 > MO_LIMIT_FAILURE_STORE_BYTES {
            return CaptureStatus::Refused;
        }

        if self.conn.execute_batch("BEGIN IMMEDIATE").is_err() {
            return self.rollback_capture(CaptureStatus::Refused);
        }

        match self.check_failure_pairs_complete() {
            PairCheck::Ok => {}
            PairCheck::Damaged => return self.rollback_capture(CaptureStatus::Damaged),
            PairCheck::Io => return self.rollback_capture(CaptureStatus::Refused),
        }

        let (mut rows, mut total) = match self.scan_failure_rows(kind) {
            Ok(v) => v,
            Err(PairCheck::Damaged) => return self.rollback_capture(CaptureStatus::Damaged),
            Err(PairCheck::Io | PairCheck::Ok) => {
                return self.rollback_capture(CaptureStatus::Refused);
            }
        };

        let next_id = rows.last().map_or(1u64, |r| r.id.saturating_add(1));
        if next_id > i64::MAX as u64 {
            return self.rollback_capture(CaptureStatus::Refused);
        }

        let mut patched_msg = message.bytes.clone();
        patched_msg[FAILURE_ID_OFFSET..FAILURE_ID_OFFSET + 8]
            .copy_from_slice(&next_id.to_le_bytes());
        if read_failure_record(&patched_msg).is_none() {
            return self.rollback_capture(CaptureStatus::Refused);
        }
        let Some(msg_frame) =
            encode_frame(message.kind, message.sequence, message.image, &patched_msg)
        else {
            return self.rollback_capture(CaptureStatus::Refused);
        };
        let need_bytes = state_frame.len() + msg_frame.len();

        // Retention policy (exact port of src/host/store-failures.c:capture):
        // Keep the first and latest record of each kind when possible.
        let mut same_count = 0usize;
        let mut has_first_same = false;
        for r in &rows {
            if r.same_kind {
                same_count += 1;
                if r.first {
                    has_first_same = true;
                }
            }
        }
        let is_first = !has_first_same;
        let mut drop_ids: Vec<u64> = Vec::new();
        let mut evicted_kinds_delta = 0u64;

        // If the same kind already has >= 2 rows (first + previous latest), drop its non-first row(s).
        if same_count >= 2 {
            let mut i = 0usize;
            while i < rows.len() {
                if rows[i].same_kind && !rows[i].first {
                    let removed = rows.remove(i);
                    total = total.saturating_sub(removed.bytes);
                    drop_ids.push(removed.id);
                } else {
                    i += 1;
                }
            }
        }

        // Evict oldest records until both count and byte budgets fit.
        // Pass 1: evict non-first duplicates of other kinds; Pass 2: evict oldest remaining kinds.
        while (rows.len() as u64) >= MO_LIMIT_FAILURE_RECORDS
            || total + need_bytes > MO_LIMIT_FAILURE_STORE_BYTES as usize
        {
            if let Some(pos) = rows.iter().position(|r| !r.first) {
                let removed = rows.remove(pos);
                total = total.saturating_sub(removed.bytes);
                drop_ids.push(removed.id);
            } else if !rows.is_empty() {
                let drop_kind = rows[0].kind.clone();
                let mut i = 0usize;
                let mut dropped_any = false;
                while i < rows.len() {
                    if rows[i].kind == drop_kind {
                        let removed = rows.remove(i);
                        total = total.saturating_sub(removed.bytes);
                        drop_ids.push(removed.id);
                        dropped_any = true;
                    } else {
                        i += 1;
                    }
                }
                if dropped_any {
                    evicted_kinds_delta += 1;
                }
            } else {
                return self.rollback_capture(CaptureStatus::Refused);
            }
        }

        for id in drop_ids {
            if self
                .conn
                .execute("DELETE FROM failure WHERE id=?1", params![id as i64])
                .is_err()
                || self
                    .conn
                    .execute("DELETE FROM failure_state WHERE id=?1", params![id as i64])
                    .is_err()
            {
                return self.rollback_capture(CaptureStatus::Refused);
            }
        }

        if self
            .conn
            .execute(
                "INSERT INTO failure(id,kind,first,message) VALUES(?1,?2,?3,?4)",
                params![
                    next_id as i64,
                    kind,
                    i64::from(is_first),
                    msg_frame.as_slice()
                ],
            )
            .is_err()
            || self
                .conn
                .execute(
                    "INSERT INTO failure_state(id,record) VALUES(?1,?2)",
                    params![next_id as i64, state_frame.as_slice()],
                )
                .is_err()
        {
            return self.rollback_capture(CaptureStatus::Refused);
        }

        if self.conn.execute_batch("COMMIT").is_err() {
            let _ = self.conn.execute_batch("ROLLBACK");
            self.mark_unsafe();
            return CaptureStatus::Unsafe;
        }
        self.evicted_kinds = self.evicted_kinds.saturating_add(evicted_kinds_delta);
        CaptureStatus::Ok(next_id)
    }

    fn rollback_capture(&mut self, fallback: CaptureStatus) -> CaptureStatus {
        if !self.conn.is_autocommit() && self.conn.execute_batch("ROLLBACK").is_err() {
            self.mark_unsafe();
            return CaptureStatus::Unsafe;
        }
        if self.current_head().is_err() {
            self.mark_unsafe();
            return CaptureStatus::Unsafe;
        }
        fallback
    }

    /// Reads failure catalog metadata entries after cursor `after` without loading state blobs
    /// (`mo_store_failure_catalog`). Returns `(entries, omitted_count)`.
    pub fn failure_catalog(&self, after: u64) -> Result<(Vec<StoredFailure>, u32), StoreStatus> {
        if self.is_unsafe {
            return Err(StoreStatus::Io);
        }
        match self.check_failure_pairs_complete() {
            PairCheck::Ok => {}
            PairCheck::Damaged => return Err(StoreStatus::Corrupt),
            PairCheck::Io => return Err(StoreStatus::Io),
        }
        let limit = MO_LIMIT_FAILURE_VIEW_RECORDS as i64;
        let mut stmt = self
            .conn
            .prepare(
                "SELECT f.id,f.kind,f.first,f.message,length(s.record) \
                 FROM failure f JOIN failure_state s ON f.id=s.id \
                 WHERE f.id>?1 ORDER BY f.id LIMIT ?2",
            )
            .map_err(|e| map_sqlite_error(&e))?;
        let mut rows = stmt
            .query(params![after as i64, limit])
            .map_err(|e| map_sqlite_error(&e))?;
        let mut out = Vec::new();
        while let Some(row) = rows.next().map_err(|e| map_sqlite_error(&e))? {
            let id: i64 = row.get(0).map_err(|_| StoreStatus::Corrupt)?;
            let kind: String = row.get(1).map_err(|_| StoreStatus::Corrupt)?;
            let first: i64 = row.get(2).map_err(|_| StoreStatus::Corrupt)?;
            let msg_blob: Vec<u8> = row.get(3).map_err(|_| StoreStatus::Corrupt)?;
            let state_len: i64 = row.get(4).map_err(|_| StoreStatus::Corrupt)?;
            if id <= 0 || !kind_valid(&kind) || (first != 0 && first != 1) || state_len < 0 {
                return Err(StoreStatus::Corrupt);
            }
            let message = decode_frame(StoreRecordKind::FailureMessage, &msg_blob)
                .ok_or(StoreStatus::Corrupt)?;
            let parsed = read_failure_record(&message.bytes).ok_or(StoreStatus::Corrupt)?;
            if parsed.id != id as u64 {
                return Err(StoreStatus::Corrupt);
            }
            out.push(StoredFailure {
                id: id as u64,
                kind,
                first: first != 0,
                state: StoreRecord {
                    kind: StoreRecordKind::FailureState,
                    sequence: message.sequence,
                    image: message.image,
                    bytes: Vec::new(),
                },
                state_bytes: (state_len as usize).saturating_sub(STORE_FRAME_BYTES),
                message,
            });
        }
        let total_after: i64 = self
            .conn
            .query_row(
                "SELECT count(*) FROM failure WHERE id>?1",
                params![after as i64],
                |r| r.get(0),
            )
            .map_err(|e| map_sqlite_error(&e))?;
        let omitted = (total_after.max(0) as u64).saturating_sub(out.len() as u64) as u32;
        Ok((out, omitted))
    }

    /// Reads and validates one stored failure pair by `id` (`mo_store_failure_get`).
    pub fn failure_get(&self, id: u64) -> Result<Option<StoredFailure>, StoreStatus> {
        if self.is_unsafe {
            return Err(StoreStatus::Io);
        }
        if id == 0 || id > i64::MAX as u64 {
            return Ok(None);
        }
        match self.check_failure_pairs_complete() {
            PairCheck::Ok => {}
            PairCheck::Damaged => return Err(StoreStatus::Corrupt),
            PairCheck::Io => return Err(StoreStatus::Io),
        }
        let row: Option<(String, i64, Vec<u8>, Vec<u8>)> = self
            .conn
            .query_row(
                "SELECT f.kind,f.first,f.message,s.record \
                 FROM failure f JOIN failure_state s ON f.id=s.id WHERE f.id=?1",
                params![id as i64],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
            )
            .optional()
            .map_err(|e| map_sqlite_error(&e))?;
        let Some((kind, first, msg_blob, state_blob)) = row else {
            return Ok(None);
        };
        if !kind_valid(&kind) || (first != 0 && first != 1) {
            return Err(StoreStatus::Corrupt);
        }
        let state =
            decode_frame(StoreRecordKind::FailureState, &state_blob).ok_or(StoreStatus::Corrupt)?;
        let message =
            decode_frame(StoreRecordKind::FailureMessage, &msg_blob).ok_or(StoreStatus::Corrupt)?;
        if state.sequence != message.sequence || state.image != message.image {
            return Err(StoreStatus::Corrupt);
        }
        let parsed = read_failure_record(&message.bytes).ok_or(StoreStatus::Corrupt)?;
        if parsed.id != id {
            return Err(StoreStatus::Corrupt);
        }
        let state_bytes = state.bytes.len();
        Ok(Some(StoredFailure {
            id,
            kind,
            first: first != 0,
            state,
            state_bytes,
            message,
        }))
    }

    /// Returns `(retained_count, evicted_kinds)` (`mo_store_failure_stats`).
    pub fn failure_stats(&self) -> Result<(u32, u64), StoreStatus> {
        if self.is_unsafe {
            return Err(StoreStatus::Io);
        }
        match self.check_failure_pairs_complete() {
            PairCheck::Ok => {}
            PairCheck::Damaged => return Err(StoreStatus::Corrupt),
            PairCheck::Io => return Err(StoreStatus::Io),
        }
        let count: i64 = self
            .conn
            .query_row("SELECT count(*) FROM failure", [], |r| r.get(0))
            .map_err(|e| map_sqlite_error(&e))?;
        if count < 0 || count as u64 > MO_LIMIT_FAILURE_RECORDS {
            return Err(StoreStatus::Corrupt);
        }
        Ok((count as u32, self.evicted_kinds))
    }
}
