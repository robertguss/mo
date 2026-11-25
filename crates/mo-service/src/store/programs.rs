//! Transactional program, activation, and adoption storage (`src/host/store-programs.c` parity).

use mo_def::limits::{
    MO_LIMIT_FAILURE_RECORDS, MO_LIMIT_UPGRADE_MIGRATION_NAME, MO_LIMIT_UPGRADE_PREVIOUS,
    MO_LIMIT_UPGRADE_REPORT_JSON_BYTES, MO_LIMIT_UPGRADE_RETAINED_BYTES,
};
use rusqlite::{Connection, OptionalExtension, params};

use crate::binary::bundle::validate_bundle;
use crate::binary::compile_context::{read_compile_context, read_compile_metadata};
use crate::binary::export::export_schema_digest;
use crate::binary::failure::read_failure_record;
use crate::binary::sha256_bytes;

use super::{
    CORE_TABLES, ServiceStore, StoreRecord, StoreRecordKind, StoreStatus, TableState, decode_frame,
    encode_frame, ensure_table_ready, map_sqlite_error, read_user_version, table_state,
};

pub(crate) const UPGRADE_TABLES: &[(&str, &str)] = &[
    (
        "program",
        "CREATE TABLE program(id INTEGER PRIMARY KEY CHECK(id=1), ordinal INTEGER NOT NULL, source BLOB NOT NULL, image BLOB NOT NULL, build BLOB NOT NULL, schema BLOB NOT NULL, bundle BLOB NOT NULL, context_digest BLOB NOT NULL, context BLOB NOT NULL) STRICT",
    ),
    (
        "upgrades",
        "CREATE TABLE upgrades(ordinal INTEGER PRIMARY KEY, head INTEGER NOT NULL, old_source BLOB NOT NULL, new_source BLOB NOT NULL, old_image BLOB NOT NULL, new_image BLOB NOT NULL, old_schema BLOB NOT NULL, new_schema BLOB NOT NULL, build BLOB NOT NULL, migration TEXT NOT NULL, report TEXT NOT NULL) STRICT",
    ),
    (
        "previous_programs",
        "CREATE TABLE previous_programs(ordinal INTEGER PRIMARY KEY, source BLOB NOT NULL, image BLOB NOT NULL, build BLOB NOT NULL, schema BLOB NOT NULL, bundle BLOB NOT NULL, context_digest BLOB NOT NULL, context BLOB NOT NULL) STRICT",
    ),
    (
        "previous_snapshots",
        "CREATE TABLE previous_snapshots(ordinal INTEGER PRIMARY KEY, record BLOB NOT NULL) STRICT",
    ),
];

/// Canonical recorded service program row (`struct mo_store_program`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StoredProgram {
    /// SHA-256 of `bundle`.
    pub source: [u8; 32],
    /// Build runtime identity digest (32 bytes).
    pub build: [u8; 32],
    /// State schema SHA-256 digest (32 bytes).
    pub schema: [u8; 32],
    /// SHA-256 of `context`.
    pub context_digest: [u8; 32],
    /// Service image key (`program_checksum ^ ((service as u64) << 32)`).
    pub image: u64,
    /// Monotonic program ordinal (`0` on initial recording, incremented on upgrades).
    pub ordinal: u64,
    /// Raw `MOSB\x01` bundle bytes.
    pub bundle: Vec<u8>,
    /// Raw `MOCC\x01` compile context bytes.
    pub context: Vec<u8>,
}

/// Atomic upgrade activation/adoption parameters (`struct mo_store_activation`).
#[derive(Debug, Clone)]
pub struct StoreActivation {
    /// Active program before the upgrade (ignored when `legacy` is true).
    pub previous: StoredProgram,
    /// Candidate program to activate.
    pub candidate: StoredProgram,
    /// Authoritative snapshot of the previous state.
    pub previous_state: StoreRecord,
    /// Authoritative snapshot of the candidate state.
    pub candidate_state: StoreRecord,
    /// Optional migration function name (empty string if none).
    pub migration: String,
    /// JSON upgrade report string.
    pub report_json: String,
}

/// Outcome of `activate` / `adopt` (`enum mo_store_activation_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ActivationStatus {
    /// Committed atomically.
    Activated,
    /// Refused cleanly without modifying the store.
    Refused,
    /// Commit failed with uncertain outcome; store must be closed.
    Unsafe,
}

pub(crate) fn program_valid(p: &StoredProgram) -> bool {
    if p.ordinal > i64::MAX as u64 {
        return false;
    }
    let Some(bundle_digest) = validate_bundle(&p.bundle) else {
        return false;
    };
    if p.source != bundle_digest {
        return false;
    }
    let Some(ctx) = read_compile_context(&p.context) else {
        return false;
    };
    if let Some(meta_bytes) = &ctx.metadata {
        let Some(meta) = read_compile_metadata(meta_bytes) else {
            return false;
        };
        if sha256_bytes(&meta.schema) != p.schema {
            return false;
        }
    }
    ctx.digest == p.context_digest
}

pub(crate) fn same_program(a: &StoredProgram, b: &StoredProgram) -> bool {
    a.ordinal == b.ordinal
        && a.image == b.image
        && a.source == b.source
        && a.build == b.build
        && a.schema == b.schema
        && a.context_digest == b.context_digest
}

pub(crate) fn parse_program_row(row: &rusqlite::Row<'_>) -> Option<StoredProgram> {
    let ordinal: i64 = row.get(0).ok()?;
    let source_v: Vec<u8> = row.get(1).ok()?;
    let image_v: Vec<u8> = row.get(2).ok()?;
    let build_v: Vec<u8> = row.get(3).ok()?;
    let schema_v: Vec<u8> = row.get(4).ok()?;
    let bundle: Vec<u8> = row.get(5).ok()?;
    let ctx_digest_v: Vec<u8> = row.get(6).ok()?;
    let context: Vec<u8> = row.get(7).ok()?;
    if ordinal < 0
        || source_v.len() != 32
        || image_v.len() != 8
        || build_v.len() != 32
        || schema_v.len() != 32
        || ctx_digest_v.len() != 32
    {
        return None;
    }
    let mut source = [0u8; 32];
    source.copy_from_slice(&source_v);
    let image = u64::from_le_bytes([
        image_v[0], image_v[1], image_v[2], image_v[3], image_v[4], image_v[5], image_v[6],
        image_v[7],
    ]);
    let mut build = [0u8; 32];
    build.copy_from_slice(&build_v);
    let mut schema = [0u8; 32];
    schema.copy_from_slice(&schema_v);
    let mut context_digest = [0u8; 32];
    context_digest.copy_from_slice(&ctx_digest_v);
    let p = StoredProgram {
        source,
        build,
        schema,
        context_digest,
        image,
        ordinal: ordinal as u64,
        bundle,
        context,
    };
    program_valid(&p).then_some(p)
}

pub(crate) fn read_active_program_conn(
    conn: &Connection,
) -> Result<Option<StoredProgram>, StoreStatus> {
    let (name, ddl) = UPGRADE_TABLES[0];
    match table_state(conn, name, ddl) {
        (TableState::Absent, _) => return Ok(None),
        (TableState::Same, _) => {}
        (TableState::Different, _) => return Err(StoreStatus::Corrupt),
        (TableState::Failed, st) => return Err(st),
    }
    let mut stmt = conn
        .prepare(
            "SELECT ordinal,source,image,build,schema,bundle,context_digest,context \
             FROM program WHERE id=1",
        )
        .map_err(|e| map_sqlite_error(&e))?;
    let mut rows = stmt.query([]).map_err(|e| map_sqlite_error(&e))?;
    let Some(row) = rows.next().map_err(|e| map_sqlite_error(&e))? else {
        return Ok(None);
    };
    let prog = parse_program_row(row).ok_or(StoreStatus::Corrupt)?;
    if rows.next().map_err(|e| map_sqlite_error(&e))?.is_some() {
        return Err(StoreStatus::Corrupt);
    }
    Ok(Some(prog))
}

fn ensure_upgrade_tables(conn: &Connection) -> Result<(), StoreStatus> {
    for &(name, ddl) in UPGRADE_TABLES {
        ensure_table_ready(conn, name, ddl)?;
    }
    Ok(())
}

impl ServiceStore {
    /// Reads the current head sequence and authoritative snapshot frame without replaying log
    /// entries (`src/tools/upgrade-commands.c:read_restart_head` helper).
    pub fn read_head_snapshot(&self) -> Result<Option<(u64, StoreRecord)>, StoreStatus> {
        if self.is_unsafe {
            return Err(StoreStatus::Io);
        }
        let Some(head) = self.current_head()? else {
            return Ok(None);
        };
        let snap_blob: Option<Vec<u8>> = self
            .conn
            .query_row("SELECT record FROM snapshot WHERE id=1", [], |r| r.get(0))
            .optional()
            .map_err(|e| map_sqlite_error(&e))?;
        let Some(bytes) = snap_blob else {
            return Err(StoreStatus::Corrupt);
        };
        let snap = decode_frame(StoreRecordKind::State, &bytes).ok_or(StoreStatus::Corrupt)?;
        if snap.sequence > head {
            return Err(StoreStatus::Corrupt);
        }
        Ok(Some((head, snap)))
    }

    /// Reads the active recorded program from the open store (`mo_store_program_read`).
    pub fn read_program(&self) -> Result<Option<StoredProgram>, StoreStatus> {
        if self.is_unsafe {
            return Err(StoreStatus::Io);
        }
        read_active_program_conn(&self.conn)
    }

    /// Atomically records the initial program and authoritative snapshot (`mo_store_program_record`).
    pub fn record_program(
        &mut self,
        program: &StoredProgram,
        snapshot: &StoreRecord,
    ) -> Result<(), StoreStatus> {
        if self.is_unsafe
            || !program_valid(program)
            || snapshot.kind != StoreRecordKind::State
            || snapshot.image != program.image
        {
            return Err(StoreStatus::Corrupt);
        }
        let frame = encode_frame(
            snapshot.kind,
            snapshot.sequence,
            snapshot.image,
            &snapshot.bytes,
        )
        .ok_or(StoreStatus::Corrupt)?;

        if self.conn.execute_batch("BEGIN IMMEDIATE").is_err() {
            self.mark_unsafe();
            return Err(StoreStatus::Io);
        }
        let res = (|| -> Result<(), StoreStatus> {
            ensure_upgrade_tables(&self.conn)?;
            let existing = read_active_program_conn(&self.conn)?;
            let head = self.current_head()?;
            let version = read_user_version(&self.conn)?;

            if let Some(prev) = existing {
                if prev.source != program.source || prev.ordinal != program.ordinal {
                    return Err(StoreStatus::Corrupt);
                }
                let Some(h) = head else {
                    return Err(StoreStatus::Corrupt);
                };
                if snapshot.sequence != h {
                    return Err(StoreStatus::Corrupt);
                }
                self.conn
                    .execute(
                        "UPDATE program SET image=?1,build=?2,schema=?3,bundle=?4,\
                         context_digest=?5,context=?6 WHERE id=1",
                        params![
                            program.image.to_le_bytes().as_slice(),
                            program.build.as_slice(),
                            program.schema.as_slice(),
                            program.bundle.as_slice(),
                            program.context_digest.as_slice(),
                            program.context.as_slice(),
                        ],
                    )
                    .map_err(|e| map_sqlite_error(&e))?;
                self.conn
                    .execute(
                        "UPDATE snapshot SET record=?1 WHERE id=1",
                        params![frame.as_slice()],
                    )
                    .map_err(|e| map_sqlite_error(&e))?;
                self.conn
                    .execute(
                        "DELETE FROM log WHERE seq<=?1",
                        params![snapshot.sequence as i64],
                    )
                    .map_err(|e| map_sqlite_error(&e))?;
            } else {
                if program.ordinal != 0 {
                    return Err(StoreStatus::Corrupt);
                }
                match head {
                    None => {
                        if snapshot.sequence != 0 || version != 0 {
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
                    }
                    Some(h) => {
                        if snapshot.sequence != h || (version != 1 && version != 2) {
                            return Err(StoreStatus::Corrupt);
                        }
                        self.conn
                            .execute(
                                "UPDATE snapshot SET record=?1 WHERE id=1",
                                params![frame.as_slice()],
                            )
                            .map_err(|e| map_sqlite_error(&e))?;
                        self.conn
                            .execute(
                                "DELETE FROM log WHERE seq<=?1",
                                params![snapshot.sequence as i64],
                            )
                            .map_err(|e| map_sqlite_error(&e))?;
                    }
                }
                self.conn
                    .execute(
                        "INSERT INTO program(id,ordinal,source,image,build,schema,bundle,context_digest,context) \
                         VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8)",
                        params![
                            program.ordinal as i64,
                            program.source.as_slice(),
                            program.image.to_le_bytes().as_slice(),
                            program.build.as_slice(),
                            program.schema.as_slice(),
                            program.bundle.as_slice(),
                            program.context_digest.as_slice(),
                            program.context.as_slice(),
                        ],
                    )
                    .map_err(|e| map_sqlite_error(&e))?;
                self.conn
                    .execute_batch("PRAGMA user_version=2")
                    .map_err(|e| map_sqlite_error(&e))?;
            }
            self.conn
                .execute_batch("COMMIT")
                .map_err(|e| map_sqlite_error(&e))
        })();
        if let Err(st) = res {
            let _ = self.conn.execute_batch("ROLLBACK");
            self.mark_unsafe();
            return Err(st);
        }
        Ok(())
    }

    /// Atomically activates a candidate upgrade (`mo_store_activate`).
    pub fn activate(&mut self, act: &StoreActivation) -> ActivationStatus {
        self.apply_activation(act, false, false)
    }

    /// Atomically adopts a new program on startup (`mo_store_adopt`).
    pub fn adopt(&mut self, act: &StoreActivation, legacy: bool) -> ActivationStatus {
        self.apply_activation(act, true, legacy)
    }

    fn apply_activation(
        &mut self,
        act: &StoreActivation,
        is_adopt: bool,
        legacy: bool,
    ) -> ActivationStatus {
        if self.is_unsafe {
            return ActivationStatus::Unsafe;
        }
        if !program_valid(&act.candidate)
            || (!legacy && !program_valid(&act.previous))
            || act.migration.len() as u64 > MO_LIMIT_UPGRADE_MIGRATION_NAME
            || act.report_json.len() as u64 > MO_LIMIT_UPGRADE_REPORT_JSON_BYTES
            || act.previous_state.kind != StoreRecordKind::State
            || act.candidate_state.kind != StoreRecordKind::State
            || act.candidate_state.image != act.candidate.image
        {
            return ActivationStatus::Refused;
        }
        let legacy_old_schema = if legacy {
            let Some(s) = export_schema_digest(&act.previous_state.bytes) else {
                return ActivationStatus::Refused;
            };
            if s != act.candidate.schema {
                return ActivationStatus::Refused;
            }
            s
        } else {
            act.previous.schema
        };
        let Some(prev_frame) = encode_frame(
            act.previous_state.kind,
            act.previous_state.sequence,
            act.previous_state.image,
            &act.previous_state.bytes,
        ) else {
            return ActivationStatus::Refused;
        };
        let Some(cand_frame) = encode_frame(
            act.candidate_state.kind,
            act.candidate_state.sequence,
            act.candidate_state.image,
            &act.candidate_state.bytes,
        ) else {
            return ActivationStatus::Refused;
        };

        if self.conn.execute_batch("BEGIN IMMEDIATE").is_err() {
            return self.rollback_activation();
        }

        let res = (|| -> Result<(), ()> {
            ensure_upgrade_tables(&self.conn).map_err(|_| ())?;
            let existing = read_active_program_conn(&self.conn).map_err(|_| ())?;
            let head = self.current_head().map_err(|_| ())?.ok_or(())?;
            if act.candidate_state.sequence != head {
                return Err(());
            }

            if legacy {
                if !is_adopt || existing.is_some() || act.candidate.ordinal != 1 {
                    return Err(());
                }
                let stored_snap: Option<Vec<u8>> = self
                    .conn
                    .query_row("SELECT record FROM snapshot WHERE id=1", [], |r| r.get(0))
                    .optional()
                    .map_err(|_| ())?;
                let Some(sb) = stored_snap else {
                    return Err(());
                };
                let decoded = decode_frame(StoreRecordKind::State, &sb).ok_or(())?;
                if decoded != act.previous_state {
                    return Err(());
                }
            } else {
                let Some(cur) = existing else {
                    return Err(());
                };
                if !same_program(&cur, &act.previous)
                    || act.candidate.ordinal != cur.ordinal.checked_add(1).ok_or(())?
                {
                    return Err(());
                }
                if is_adopt {
                    let stored_snap: Option<Vec<u8>> = self
                        .conn
                        .query_row("SELECT record FROM snapshot WHERE id=1", [], |r| r.get(0))
                        .optional()
                        .map_err(|_| ())?;
                    let Some(sb) = stored_snap else {
                        return Err(());
                    };
                    let decoded = decode_frame(StoreRecordKind::State, &sb).ok_or(())?;
                    if decoded != act.previous_state {
                        return Err(());
                    }
                } else if act.previous_state.sequence != head
                    || act.previous_state.image != cur.image
                {
                    return Err(());
                }

                // Store previous program under `candidate.ordinal` (`src/host/store-programs.c:482`).
                self.conn
                    .execute(
                        "INSERT INTO previous_programs(ordinal,source,image,build,schema,bundle,context_digest,context) \
                         VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
                        params![
                            act.candidate.ordinal as i64,
                            cur.source.as_slice(),
                            cur.image.to_le_bytes().as_slice(),
                            cur.build.as_slice(),
                            cur.schema.as_slice(),
                            cur.bundle.as_slice(),
                            cur.context_digest.as_slice(),
                            cur.context.as_slice(),
                        ],
                    )
                    .map_err(|_| ())?;
            }

            // Store previous snapshot under `candidate.ordinal` (`src/host/store-programs.c:485`).
            self.conn
                .execute(
                    "INSERT INTO previous_snapshots(ordinal,record) VALUES(?1,?2)",
                    params![act.candidate.ordinal as i64, prev_frame.as_slice()],
                )
                .map_err(|_| ())?;

            retain_programs(&self.conn, act.candidate.ordinal)?;

            let old_source = if legacy {
                [0u8; 32]
            } else {
                act.previous.source
            };
            let old_image = if legacy {
                act.previous_state.image
            } else {
                act.previous.image
            };
            let old_schema = legacy_old_schema;

            self.conn
                .execute(
                    "INSERT INTO upgrades(ordinal,head,old_source,new_source,old_image,new_image,\
                     old_schema,new_schema,build,migration,report) \
                     VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                    params![
                        act.candidate.ordinal as i64,
                        head as i64,
                        old_source.as_slice(),
                        act.candidate.source.as_slice(),
                        old_image.to_le_bytes().as_slice(),
                        act.candidate.image.to_le_bytes().as_slice(),
                        old_schema.as_slice(),
                        act.candidate.schema.as_slice(),
                        act.candidate.build.as_slice(),
                        act.migration.as_str(),
                        act.report_json.as_str(),
                    ],
                )
                .map_err(|_| ())?;

            if legacy {
                self.conn
                    .execute(
                        "INSERT INTO program(id,ordinal,source,image,build,schema,bundle,context_digest,context) \
                         VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8)",
                        params![
                            act.candidate.ordinal as i64,
                            act.candidate.source.as_slice(),
                            act.candidate.image.to_le_bytes().as_slice(),
                            act.candidate.build.as_slice(),
                            act.candidate.schema.as_slice(),
                            act.candidate.bundle.as_slice(),
                            act.candidate.context_digest.as_slice(),
                            act.candidate.context.as_slice(),
                        ],
                    )
                    .map_err(|_| ())?;
            } else {
                self.conn
                    .execute(
                        "UPDATE program SET ordinal=?1,source=?2,image=?3,build=?4,schema=?5,\
                         bundle=?6,context_digest=?7,context=?8 WHERE id=1",
                        params![
                            act.candidate.ordinal as i64,
                            act.candidate.source.as_slice(),
                            act.candidate.image.to_le_bytes().as_slice(),
                            act.candidate.build.as_slice(),
                            act.candidate.schema.as_slice(),
                            act.candidate.bundle.as_slice(),
                            act.candidate.context_digest.as_slice(),
                            act.candidate.context.as_slice(),
                        ],
                    )
                    .map_err(|_| ())?;
            }

            self.conn
                .execute(
                    "UPDATE snapshot SET record=?1 WHERE id=1",
                    params![cand_frame.as_slice()],
                )
                .map_err(|_| ())?;
            self.conn
                .execute("DELETE FROM log WHERE seq<=?1", params![head as i64])
                .map_err(|_| ())?;
            self.conn
                .execute_batch("PRAGMA user_version=2")
                .map_err(|_| ())?;
            Ok(())
        })();

        if res.is_err() {
            return self.rollback_activation();
        }
        if self.conn.execute_batch("COMMIT").is_err() {
            let _ = self.conn.execute_batch("ROLLBACK");
            self.mark_unsafe();
            return ActivationStatus::Unsafe;
        }
        ActivationStatus::Activated
    }

    fn rollback_activation(&mut self) -> ActivationStatus {
        if !self.conn.is_autocommit() && self.conn.execute_batch("ROLLBACK").is_err() {
            self.mark_unsafe();
            return ActivationStatus::Unsafe;
        }
        if self.current_head().is_err() {
            self.mark_unsafe();
            return ActivationStatus::Unsafe;
        }
        ActivationStatus::Refused
    }
}

/// Enforces `MO_LIMIT_UPGRADE_PREVIOUS` retention while keeping any older previous program
/// pinned by a retained failure record (`src/host/store-programs.c:retain_programs`).
fn retain_programs(conn: &Connection, ordinal: u64) -> Result<(), ()> {
    // Load all retained failure identities once if the failure table exists.
    let mut failure_pins: Vec<(u64, [u8; 32], [u8; 32])> = Vec::new();
    if table_state(conn, CORE_TABLES[3].0, CORE_TABLES[3].1).0 == TableState::Same {
        let mut fstmt = conn
            .prepare("SELECT message FROM failure ORDER BY id")
            .map_err(|_| ())?;
        let mut frows = fstmt.query([]).map_err(|_| ())?;
        while let Some(row) = frows.next().map_err(|_| ())? {
            let blob: Vec<u8> = row.get(0).map_err(|_| ())?;
            let frame = decode_frame(StoreRecordKind::FailureMessage, &blob).ok_or(())?;
            let parsed = read_failure_record(&frame.bytes).ok_or(())?;
            failure_pins.push((parsed.program, parsed.build, parsed.schema));
        }
    }

    let mut to_remove: Vec<u64> = Vec::new();
    {
        let mut stmt = conn
            .prepare(
                "SELECT ordinal,source,image,build,schema,bundle,context_digest,context \
                 FROM previous_programs ORDER BY ordinal DESC",
            )
            .map_err(|_| ())?;
        let mut rows = stmt.query([]).map_err(|_| ())?;
        let mut count = 0u64;
        let max_rows = MO_LIMIT_FAILURE_RECORDS + MO_LIMIT_UPGRADE_PREVIOUS + 1;
        while let Some(row) = rows.next().map_err(|_| ())? {
            count += 1;
            let prog = parse_program_row(row).ok_or(())?;
            if count > max_rows || prog.ordinal == 0 || prog.ordinal > ordinal {
                return Err(());
            }
            if count <= MO_LIMIT_UPGRADE_PREVIOUS {
                continue;
            }
            let pinned = failure_pins.iter().any(|(img, bld, sch)| {
                *img == prog.image && *bld == prog.build && *sch == prog.schema
            });
            if !pinned {
                to_remove.push(prog.ordinal);
            }
        }
    }

    for rem in to_remove {
        conn.execute(
            "DELETE FROM previous_programs WHERE ordinal=?1",
            params![rem as i64],
        )
        .map_err(|_| ())?;
    }
    if ordinal > MO_LIMIT_UPGRADE_PREVIOUS {
        conn.execute(
            "DELETE FROM previous_snapshots WHERE ordinal<=?1",
            params![(ordinal - MO_LIMIT_UPGRADE_PREVIOUS) as i64],
        )
        .map_err(|_| ())?;
    }
    let total_snap_bytes: i64 = conn
        .query_row(
            "SELECT coalesce(sum(length(record)),0) FROM previous_snapshots",
            [],
            |r| r.get(0),
        )
        .map_err(|_| ())?;
    if total_snap_bytes < 0 || total_snap_bytes as u64 > MO_LIMIT_UPGRADE_RETAINED_BYTES {
        return Err(());
    }
    Ok(())
}
