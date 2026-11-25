//! Live durable service runtime (`src/vm/service.c` parity).
//!
//! Manages authoritative service state, write-ahead log persistence, periodic snapshotting,
//! crash recovery replay, timer scheduling, failure capture, supervisor restarts/stops,
//! and live image swapping.

pub mod failure;
pub mod swap;

use std::collections::VecDeque;

use mo_bytecode::{BytecodeFile, FNV_OFFSET, ParamOwnership, ServiceDef, fnv1a};
use mo_def::limits::{
    MO_LIMIT_SAVED_BYTES, MO_LIMIT_SERVICE_FAILURE_WINDOW_US, MO_LIMIT_SERVICE_FAILURES,
    MO_LIMIT_SERVICE_MAILBOX, MO_LIMIT_SERVICE_RESTART_WINDOW_US, MO_LIMIT_SERVICE_RESTARTS,
    MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, MO_LIMIT_SERVICE_WORK,
};
use mo_heap::saved::{decode_value, encode_value, encode_value_unshared};
use mo_heap::{Heap, ScratchGuard, Value};
use mo_host::HostContext;
use mo_vm::{Defect, Image, Limits, Outcome, Process};

use crate::binary::export::export_schema_digest;
use crate::binary::log_record::{LogRecordHeader, read_log_record, write_log_record};
use crate::store::failures::CaptureStatus;
use crate::store::{ServiceStore, StoreRecord, StoreRecordKind, StoreStatus};

use failure::{FailureBuildParams, build_failure_payload};

/// Status of a service lifecycle or handler operation (`enum mo_service_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceStatus {
    /// Operation succeeded.
    Ok,
    /// Handler failed with a defect, limit, or save refusal (service stayed alive or restarted).
    Failed,
    /// Service supervisor stopped the service after repeated restarts.
    Stopped,
    /// Backing store I/O failure or unsafe store.
    StoreIo,
    /// Backing store data is damaged or incompatible with this image.
    Corrupt,
    /// Out of memory or value limit refused.
    Memory,
    /// Mailbox is full.
    Full,
    /// Invalid tool index or argument count.
    Invalid,
}

/// Summary of one recently applied message kept in memory for `mo-log`.
#[derive(Debug, Clone)]
pub struct RecentLogEntry {
    /// Sequence number.
    pub sequence: u64,
    /// Arrival wall-clock timestamp in microseconds since epoch.
    pub arrival: i64,
    /// Tool or timer index.
    pub handler: u32,
    /// `true` if timer tick.
    pub timer: bool,
    /// Handler display name.
    pub name: String,
    /// Whether this was a read-only call (not persisted to WAL).
    pub read_only: bool,
    /// Encoded argument byte sizes.
    pub argument_bytes: Vec<u32>,
}

/// Timer scheduling slot.
#[derive(Debug, Clone, Copy)]
pub struct TimerSlot {
    /// Interval in microseconds.
    pub interval_us: u64,
    /// Next due monotonic timestamp in microseconds.
    pub next_due_us: u64,
}

/// Supervisor health tracking (`mo-supervisor`).
#[derive(Debug, Clone, Default)]
pub struct SupervisorState {
    /// Monotonic timestamps of recent failures within `MO_LIMIT_SERVICE_FAILURE_WINDOW_US`.
    pub failures: VecDeque<u64>,
    /// Monotonic timestamps of recent restarts within `MO_LIMIT_SERVICE_RESTART_WINDOW_US`.
    pub restarts: VecDeque<u64>,
    /// Total failures observed since start.
    pub total_failures: u64,
    /// Total restarts performed since start.
    pub total_restarts: u64,
    /// Whether the supervisor has permanently stopped the service.
    pub stopped: bool,
    /// Last stop/failure reason.
    pub last_reason: Option<String>,
    /// Last captured failure id (if any).
    pub last_failure_id: Option<u64>,
}

/// Live service event emitted to stdout/stderr observers (`struct mo_service_event`).
#[derive(Debug, Clone)]
pub enum ServiceEvent {
    /// Failure captured into the store.
    FailureCaptured {
        /// Failure record id (`0` if capture was refused).
        id: u64,
        /// Handler name.
        handler: String,
        /// Canonical one-line status reason (`service handler failed`, etc.).
        reason: String,
        /// Sanitized defect report text (never allows forged `mo serve: ` lines).
        defect_text: String,
    },
    /// Service restarted after hitting the failure window threshold.
    Restarted {
        /// Total restart count (`1..=3`).
        restarts: u64,
        /// Reason text.
        reason: String,
    },
    /// Service permanently stopped after hitting the restart window threshold.
    Stopped {
        /// Reason text.
        reason: String,
    },
}

/// Retained event summary for `mo-supervisor` and `mo-overview` (`struct mo_service_event_summary`).
#[derive(Debug, Clone)]
pub struct ServiceEventSummary {
    /// Monotonic event sequence number.
    pub number: u64,
    /// Kind string: `"failure"`, `"restart"`, `"stop"`, or `"upgrade"`.
    pub kind: &'static str,
    /// Reason string.
    pub reason: String,
    /// Handler function name (empty if none).
    pub handler_name: String,
    /// Wall clock timestamp in microseconds.
    pub wall_us: i64,
    /// Monotonic elapsed microseconds since start.
    pub elapsed_us: u64,
    /// Captured failure record id (`0` if none).
    pub record_id: u64,
    /// Reason why failure record was unavailable (empty if available).
    pub record_unavailable: String,
}

/// Runtime metrics exposed via `mo-memory` and `mo-overview`.
#[derive(Debug, Clone, Default)]
pub struct ServiceMetrics {
    /// Cumulative VM instructions executed by handlers and timers.
    pub handler_work: u64,
    /// Peak handler heap usage in bytes.
    pub handler_heap_peak: usize,
    /// Peak diagnostic scratch usage in bytes (`diagnostic_scratch_peak`).
    pub diagnostic_scratch_peak: usize,
    /// Total mutating calls applied.
    pub mutations: u64,
    /// Total read-only calls served.
    pub reads: u64,
    /// Total timer ticks applied.
    pub timer_ticks: u64,
    /// Total snapshots written.
    pub snapshots: u64,
}

/// Result of invoking a service tool handler.
#[derive(Debug)]
pub struct ToolCallResult {
    /// Outcome status.
    pub status: ServiceStatus,
    /// Encoded `MOSV\x01` raw return value of `tool.result_type` (`Ok` status only).
    pub reply_encoded: Option<Vec<u8>>,
    /// Whether service state was updated and persisted by this call.
    pub updated: bool,
    /// Sequence number after the call.
    pub sequence: u64,
    /// Failure reason message when `status != ServiceStatus::Ok`.
    pub error_message: Option<String>,
    /// Captured failure id if a failure record was persisted.
    pub failure_id: Option<u64>,
    /// VM instructions consumed.
    pub work: u64,
    /// Events produced during the call.
    pub events: Vec<ServiceEvent>,
}

/// Durable live service execution loop (`struct mo_service`).
pub struct ServiceLoop {
    pub(crate) image: Image,
    pub(crate) service_idx: u32,
    pub(crate) store: ServiceStore,
    pub(crate) limits: Limits,
    pub(crate) build_digest: [u8; 32],
    pub(crate) build_label: String,
    pub(crate) schema_digest: [u8; 32],
    pub(crate) image_key: u64,
    pub(crate) state_heap: Heap,
    pub(crate) state: Value,
    pub(crate) sequence: u64,
    pub(crate) snapshot_sequence: u64,
    pub(crate) encoded_sequence: u64,
    pub(crate) encoded_state: Vec<u8>,
    pub(crate) timers: Vec<TimerSlot>,
    pub(crate) supervisor: SupervisorState,
    pub(crate) recent_log: VecDeque<RecentLogEntry>,
    pub(crate) event_log: VecDeque<ServiceEventSummary>,
    pub(crate) next_event_number: u64,
    pub(crate) metrics: ServiceMetrics,
    pub(crate) started_mono_us: u64,
}

impl ServiceLoop {
    /// Opens or recovers a durable service against `store` (`mo_service_start`).
    ///
    /// If `store` is empty, runs the service's `start` initializer and persists the initial
    /// snapshot at sequence 0 (unless `defer_initial_snapshot` is true, in which case the
    /// caller records the initial snapshot atomically with the `program` row!).
    #[expect(
        clippy::too_many_arguments,
        reason = "service initialization needs explicit image, store, limits, identity, and timestamps"
    )]
    pub fn open(
        image: Image,
        service_idx: u32,
        store: ServiceStore,
        limits: Limits,
        build_digest: [u8; 32],
        build_label: String,
        now_mono_us: u64,
        defer_initial_snapshot: bool,
    ) -> Result<(Self, bool /* was_empty */), ServiceStatus> {
        let (svc, was_empty, _replayed) = Self::open_with_expected_image(
            image,
            service_idx,
            store,
            limits,
            build_digest,
            build_label,
            now_mono_us,
            defer_initial_snapshot,
            None,
        )?;
        Ok((svc, was_empty))
    }

    /// Opens or recovers a durable service, verifying snapshot and log frames against
    /// `expected_image_key` when provided (used for verified adoption when `head.image`
    /// differs from the newly compiled image key). Returns `(service, was_empty, replayed_count)`.
    #[expect(
        clippy::too_many_arguments,
        reason = "service recovery needs explicit image, store, limits, identity, timestamps, and optional expected image key"
    )]
    pub fn open_with_expected_image(
        image: Image,
        service_idx: u32,
        mut store: ServiceStore,
        limits: Limits,
        build_digest: [u8; 32],
        build_label: String,
        now_mono_us: u64,
        defer_initial_snapshot: bool,
        expected_image_key: Option<u64>,
    ) -> Result<(Self, bool /* was_empty */, u64 /* replayed */), ServiceStatus> {
        let svc = image
            .file()
            .services
            .get(service_idx as usize)
            .ok_or(ServiceStatus::Corrupt)?;
        let image_key = image.file().program_checksum ^ (u64::from(service_idx) << 32);
        let verify_key = expected_image_key.unwrap_or(image_key);
        let state_ty = svc.state_type;
        let start_fn = svc.start;

        let records = store.load().map_err(store_to_service)?;
        let was_empty = records.is_empty();

        let state_heap = Heap::new(limits.heap_size);
        let mut replayed_count = 0u64;
        let (state, encoded_state, sequence, snapshot_sequence, recent_log) = if was_empty {
            let mut proc = Process::new(&image, limits, HostContext::stdio(Vec::new(), Vec::new()));
            let (outcome, _work) =
                proc.call_with_budget(start_fn, Vec::new(), MO_LIMIT_SERVICE_WORK);
            let init_val = match outcome {
                Outcome::Returned(v) => v,
                Outcome::Failed(defect) | Outcome::Stopped(defect) => {
                    let raw =
                        sanitize_defect_text(&mo_vm::render_defect_raw(image.file(), &defect));
                    eprint!("mo serve: failure: service: service handler failed\nbug: {raw}");
                    if !raw.ends_with('\n') {
                        eprintln!();
                    }
                    return Err(ServiceStatus::Failed);
                }
            };
            let mut enc = Vec::new();
            encode_value(&image.file().types, state_ty, &init_val, &mut enc)
                .map_err(|_| ServiceStatus::Memory)?;
            let copied = decode_value(&state_heap, &image.file().types, state_ty, &enc)
                .map_err(|_| ServiceStatus::Memory)?;
            if !defer_initial_snapshot {
                let rec = StoreRecord {
                    kind: StoreRecordKind::State,
                    sequence: 0,
                    image: image_key,
                    bytes: enc.clone(),
                };
                store.snapshot(&rec).map_err(store_to_service)?;
            }
            (copied, enc, 0u64, 0u64, VecDeque::new())
        } else {
            let snap = &records[0];
            if snap.kind != StoreRecordKind::State || snap.image != verify_key {
                return Err(ServiceStatus::Corrupt);
            }
            let mut cur_enc = snap.bytes.clone();
            let mut cur_state = decode_value(&state_heap, &image.file().types, state_ty, &cur_enc)
                .map_err(|_| ServiceStatus::Corrupt)?;
            let mut cur_seq = snap.sequence;
            let snap_seq = snap.sequence;
            let mut log_ring = VecDeque::new();

            for rec in &records[1..] {
                if rec.kind != StoreRecordKind::Log
                    || rec.image != verify_key
                    || rec.sequence != cur_seq + 1
                {
                    return Err(ServiceStatus::Corrupt);
                }
                let parsed = read_log_record(&rec.bytes).ok_or(ServiceStatus::Corrupt)?;
                let (next_enc, handler_name) = match replay_one_message(
                    &image,
                    service_idx,
                    limits,
                    &cur_enc,
                    parsed.header.timer,
                    parsed.header.handler,
                    parsed.header.arrival,
                    &parsed.arguments,
                ) {
                    Ok(v) => v,
                    Err(st) => {
                        let hname = replay_handler_label(
                            image.file(),
                            service_idx,
                            parsed.header.timer,
                            parsed.header.handler,
                        );
                        eprintln!(
                            "mo serve: failure: log record {}: {}: {}",
                            rec.sequence,
                            hname,
                            service_status_text(st)
                        );
                        return Err(st);
                    }
                };
                let checksum = fnv1a(FNV_OFFSET, &next_enc);
                if checksum != parsed.header.state_checksum {
                    eprintln!(
                        "mo serve: failure: log record {}: {}: saved service data is damaged or was written by a different program; recovery refused",
                        rec.sequence, handler_name
                    );
                    return Err(ServiceStatus::Corrupt);
                }
                cur_state = decode_value(&state_heap, &image.file().types, state_ty, &next_enc)
                    .map_err(|_| ServiceStatus::Corrupt)?;
                cur_enc = next_enc;
                cur_seq = rec.sequence;
                replayed_count += 1;

                if log_ring.len() >= 128 {
                    let _ = log_ring.pop_front();
                }
                log_ring.push_back(RecentLogEntry {
                    sequence: cur_seq,
                    arrival: parsed.header.arrival,
                    handler: parsed.header.handler,
                    timer: parsed.header.timer,
                    name: handler_name,
                    read_only: false,
                    argument_bytes: parsed.arguments.iter().map(|a| a.len() as u32).collect(),
                });
            }
            (cur_state, cur_enc, cur_seq, snap_seq, log_ring)
        };

        let schema_digest = export_schema_digest(&encoded_state).ok_or(ServiceStatus::Corrupt)?;
        let timers = eval_service_timers(&image, service_idx, limits, now_mono_us)?;

        let metrics = ServiceMetrics {
            diagnostic_scratch_peak: encoded_state.len().max(64),
            snapshots: u64::from(!was_empty),
            ..ServiceMetrics::default()
        };

        Ok((
            Self {
                image,
                service_idx,
                store,
                limits,
                build_digest,
                build_label,
                schema_digest,
                image_key,
                state_heap,
                state,
                sequence,
                snapshot_sequence,
                encoded_sequence: sequence,
                encoded_state,
                timers,
                supervisor: SupervisorState::default(),
                recent_log,
                event_log: VecDeque::new(),
                next_event_number: 1,
                metrics,
                started_mono_us: now_mono_us,
            },
            was_empty,
            replayed_count,
        ))
    }

    /// Constructs a [`ServiceLoop`] directly from already-activated state bytes at `sequence`
    /// without running recovery replay (used after empty-tail fallback startup upgrade).
    #[expect(
        clippy::too_many_arguments,
        reason = "direct service loop construction needs image, store, limits, identity, sequence, state bytes, and timestamp"
    )]
    pub fn open_from_state(
        image: Image,
        service_idx: u32,
        store: ServiceStore,
        limits: Limits,
        build_digest: [u8; 32],
        build_label: String,
        sequence: u64,
        encoded_state: Vec<u8>,
        now_mono_us: u64,
    ) -> Result<Self, ServiceStatus> {
        let svc = image
            .file()
            .services
            .get(service_idx as usize)
            .ok_or(ServiceStatus::Corrupt)?;
        let image_key = image.file().program_checksum ^ (u64::from(service_idx) << 32);
        let state_heap = Heap::new(limits.heap_size);
        let state = decode_value(
            &state_heap,
            &image.file().types,
            svc.state_type,
            &encoded_state,
        )
        .map_err(|_| ServiceStatus::Corrupt)?;
        let schema_digest = export_schema_digest(&encoded_state).ok_or(ServiceStatus::Corrupt)?;
        let timers = eval_service_timers(&image, service_idx, limits, now_mono_us)?;
        let metrics = ServiceMetrics {
            diagnostic_scratch_peak: encoded_state.len().max(64),
            snapshots: 1,
            ..ServiceMetrics::default()
        };
        Ok(Self {
            image,
            service_idx,
            store,
            limits,
            build_digest,
            build_label,
            schema_digest,
            image_key,
            state_heap,
            state,
            sequence,
            snapshot_sequence: sequence,
            encoded_sequence: sequence,
            encoded_state,
            timers,
            supervisor: SupervisorState::default(),
            recent_log: VecDeque::new(),
            event_log: VecDeque::new(),
            next_event_number: 1,
            metrics,
            started_mono_us: now_mono_us,
        })
    }

    /// Returns the active [`BytecodeFile`].
    #[must_use]
    pub fn file(&self) -> &BytecodeFile {
        self.image.file()
    }

    /// Returns the active [`Image`].
    #[must_use]
    pub fn image(&self) -> &Image {
        &self.image
    }

    /// Returns the active [`ServiceDef`].
    #[must_use]
    pub fn service_def(&self) -> &ServiceDef {
        &self.image.file().services[self.service_idx as usize]
    }

    /// Returns the active service index.
    #[must_use]
    pub fn service_index(&self) -> u32 {
        self.service_idx
    }

    /// Returns the service image key (`program_checksum ^ ((service_idx as u64) << 32)`).
    #[must_use]
    pub fn image_key(&self) -> u64 {
        self.image_key
    }

    /// Returns the 32-byte build runtime identity digest.
    #[must_use]
    pub fn build_digest(&self) -> [u8; 32] {
        self.build_digest
    }

    /// Returns the build label string.
    #[must_use]
    pub fn build_label(&self) -> &str {
        &self.build_label
    }

    /// Returns the 32-byte state schema SHA-256 digest.
    #[must_use]
    pub fn schema_digest(&self) -> [u8; 32] {
        self.schema_digest
    }

    /// Returns the current committed logical sequence number.
    #[must_use]
    pub fn sequence(&self) -> u64 {
        self.sequence
    }

    /// Returns the sequence number of the last compacted snapshot.
    #[must_use]
    pub fn snapshot_sequence(&self) -> u64 {
        self.snapshot_sequence
    }

    /// Returns the sequence number at which `encoded_state` was produced.
    #[must_use]
    pub fn encoded_sequence(&self) -> u64 {
        self.encoded_sequence
    }

    /// Returns the current authoritative `MOSV\x01`-encoded state bytes.
    #[must_use]
    pub fn encoded_state(&self) -> &[u8] {
        &self.encoded_state
    }

    /// Returns the live state value and its owning heap.
    #[must_use]
    pub fn state_value(&self) -> (&Heap, &Value) {
        (&self.state_heap, &self.state)
    }

    /// Returns the configured VM [`Limits`].
    #[must_use]
    pub fn limits(&self) -> Limits {
        self.limits
    }

    /// Returns a reference to the backing [`ServiceStore`].
    #[must_use]
    pub fn store(&self) -> &ServiceStore {
        &self.store
    }

    /// Returns a mutable reference to the backing [`ServiceStore`].
    pub fn store_mut(&mut self) -> &mut ServiceStore {
        &mut self.store
    }

    /// Returns the timer schedule slots.
    #[must_use]
    pub fn timers(&self) -> &[TimerSlot] {
        &self.timers
    }

    /// Returns supervisor health state.
    #[must_use]
    pub fn supervisor(&self) -> &SupervisorState {
        &self.supervisor
    }

    /// Returns recent applied log summaries.
    #[must_use]
    pub fn recent_log(&self) -> &VecDeque<RecentLogEntry> {
        &self.recent_log
    }

    /// Returns retained supervisor/upgrade event summaries.
    #[must_use]
    pub fn event_log(&self) -> &VecDeque<ServiceEventSummary> {
        &self.event_log
    }

    /// Records a service event summary (bounded to 64 entries).
    pub fn push_event_summary(&mut self, mut entry: ServiceEventSummary) {
        entry.number = self.next_event_number;
        self.next_event_number = self.next_event_number.saturating_add(1);
        if self.event_log.len() >= 64 {
            let _ = self.event_log.pop_front();
        }
        self.event_log.push_back(entry);
    }

    /// Returns accumulated runtime metrics.
    #[must_use]
    pub fn metrics(&self) -> &ServiceMetrics {
        &self.metrics
    }

    /// Returns `true` if the supervisor has permanently stopped the service.
    #[must_use]
    pub fn is_stopped(&self) -> bool {
        self.supervisor.stopped
    }

    /// Records a diagnostic scratch peak observation (from operator view/export encoding).
    pub fn observe_diagnostic_scratch(&mut self, bytes: usize) {
        if bytes > self.metrics.diagnostic_scratch_peak {
            self.metrics.diagnostic_scratch_peak = bytes;
        }
    }

    /// Returns the earliest due monotonic timestamp among all timers, if any.
    #[must_use]
    pub fn next_timer_deadline_us(&self) -> Option<u64> {
        if self.supervisor.stopped {
            return None;
        }
        self.timers.iter().map(|t| t.next_due_us).min()
    }

    /// Invokes tool `tool_idx` with `MOSV\x01`-encoded arguments (`mo_service_call`).
    pub fn call_tool(
        &mut self,
        tool_idx: u32,
        args_encoded: Vec<Vec<u8>>,
        arrival_wall_us: i64,
        now_mono_us: u64,
    ) -> ToolCallResult {
        if self.supervisor.stopped {
            return ToolCallResult {
                status: ServiceStatus::Stopped,
                reply_encoded: None,
                updated: false,
                sequence: self.sequence,
                error_message: self.supervisor.last_reason.clone(),
                failure_id: self.supervisor.last_failure_id,
                work: 0,
                events: Vec::new(),
            };
        }
        let file = self.image.file();
        let svc = &file.services[self.service_idx as usize];
        let Some(tool) = svc.tools.get(tool_idx as usize) else {
            return ToolCallResult {
                status: ServiceStatus::Invalid,
                reply_encoded: None,
                updated: false,
                sequence: self.sequence,
                error_message: Some("unknown service tool".to_string()),
                failure_id: None,
                work: 0,
                events: Vec::new(),
            };
        };
        if args_encoded.len() != tool.argument_types.len() {
            return ToolCallResult {
                status: ServiceStatus::Invalid,
                reply_encoded: None,
                updated: false,
                sequence: self.sequence,
                error_message: Some("wrong tool argument count".to_string()),
                failure_id: None,
                work: 0,
                events: Vec::new(),
            };
        }

        let tool_fn = tool.function;
        let read_only = tool.read_only;
        let state_ty = svc.state_type;
        let reply_ty = tool.result_type;
        let arg_types = tool.argument_types.clone();
        let tool_name = file.string(tool.name).to_string();

        let mut proc = Process::new(
            &self.image,
            self.limits,
            HostContext::stdio(Vec::new(), Vec::new()),
        );

        let state_in = match decode_value(proc.heap(), &file.types, state_ty, &self.encoded_state) {
            Ok(v) => v,
            Err(_) => {
                return ToolCallResult {
                    status: ServiceStatus::Memory,
                    reply_encoded: None,
                    updated: false,
                    sequence: self.sequence,
                    error_message: Some("failed to decode service state".to_string()),
                    failure_id: None,
                    work: 0,
                    events: Vec::new(),
                };
            }
        };

        let mut call_args = Vec::with_capacity(2 + args_encoded.len());
        call_args.push(state_in);
        call_args.push(Value::Time(arrival_wall_us));
        for (enc, &aty) in args_encoded.iter().zip(&arg_types) {
            match decode_value(proc.heap(), &file.types, aty, enc) {
                Ok(v) => call_args.push(v),
                Err(_) => {
                    return ToolCallResult {
                        status: ServiceStatus::Invalid,
                        reply_encoded: None,
                        updated: false,
                        sequence: self.sequence,
                        error_message: Some("invalid tool argument encoding".to_string()),
                        failure_id: None,
                        work: 0,
                        events: Vec::new(),
                    };
                }
            }
        }

        let (outcome, work) =
            crate::call_with_budget_isolated(&mut proc, tool_fn, call_args, MO_LIMIT_SERVICE_WORK);
        self.metrics.handler_work = self.metrics.handler_work.saturating_add(work);
        let peak = proc.memory_peak();
        if peak > self.metrics.handler_heap_peak {
            self.metrics.handler_heap_peak = peak;
        }
        let scratch = proc.heap().budget().scratch_peak();
        if scratch > self.metrics.diagnostic_scratch_peak {
            self.metrics.diagnostic_scratch_peak = scratch;
        }

        match outcome {
            Outcome::Returned(result_val) => {
                let rdef = self.image.file().types.get(reply_ty);
                let is_result = rdef.kind == mo_bytecode::TypeKind::Choice
                    && rdef
                        .name
                        .as_deref()
                        .is_some_and(|n| n.starts_with("Result("));
                let is_app_error = is_result && result_val.as_tag() == 1;

                let next_state_opt = if read_only || is_app_error {
                    None
                } else {
                    let pair_val = if is_result {
                        extract_choice_field(&self.image.file().types, reply_ty, &result_val, 0)
                    } else {
                        Some(result_val.clone())
                    };
                    let extracted = pair_val.and_then(|pv| match pv.as_heap_ref().obj() {
                        mo_heap::HeapObj::Tuple { fields, .. } if fields.len() == 2 => {
                            Some(fields[0].clone())
                        }
                        _ => None,
                    });
                    let Some(ns) = extracted else {
                        let defect = Defect {
                            reason: "mutating tool did not return a (State, Reply) pair"
                                .to_string(),
                            limit_name: None,
                            limit: 0,
                            detail: None,
                            frames: Vec::new(),
                            variables: Vec::new(),
                            variables_unavailable: false,
                        };
                        return self.handle_failure(
                            false,
                            tool_idx,
                            &tool_name,
                            read_only,
                            args_encoded,
                            arrival_wall_us,
                            now_mono_us,
                            0,
                            &defect,
                            work,
                        );
                    };
                    Some(ns)
                };

                let mut reply_enc = Vec::new();
                if encode_value(
                    &self.image.file().types,
                    reply_ty,
                    &result_val,
                    &mut reply_enc,
                )
                .is_err()
                {
                    let defect = Defect {
                        reason: "reply value could not be saved".to_string(),
                        limit_name: Some("MO_LIMIT_SAVED_BYTES"),
                        limit: MO_LIMIT_SAVED_BYTES as usize,
                        detail: None,
                        frames: Vec::new(),
                        variables: Vec::new(),
                        variables_unavailable: false,
                    };
                    return self.handle_failure(
                        false,
                        tool_idx,
                        &tool_name,
                        read_only,
                        args_encoded,
                        arrival_wall_us,
                        now_mono_us,
                        3,
                        &defect,
                        work,
                    );
                }

                let mut updated = false;
                if let Some(next_state) = next_state_opt {
                    let mut next_enc = Vec::new();
                    let mut scratch_guard = ScratchGuard::new(proc.heap().budget());
                    let _ = scratch_guard.charge(64);
                    let enc_res = encode_value_unshared(
                        &self.image.file().types,
                        state_ty,
                        &next_state,
                        &mut next_enc,
                    );
                    drop(scratch_guard);
                    self.observe_diagnostic_scratch(next_enc.len());
                    if enc_res.is_err() {
                        let defect = Defect {
                            reason: "updated state could not be saved".to_string(),
                            limit_name: Some("MO_LIMIT_SAVED_BYTES"),
                            limit: MO_LIMIT_SAVED_BYTES as usize,
                            detail: None,
                            frames: Vec::new(),
                            variables: Vec::new(),
                            variables_unavailable: false,
                        };
                        return self.handle_failure(
                            false,
                            tool_idx,
                            &tool_name,
                            read_only,
                            args_encoded,
                            arrival_wall_us,
                            now_mono_us,
                            3,
                            &defect,
                            work,
                        );
                    }

                    let next_seq = self.sequence + 1;
                    let arg_slices: Vec<&[u8]> = args_encoded.iter().map(Vec::as_slice).collect();
                    let header = LogRecordHeader {
                        state_checksum: fnv1a(FNV_OFFSET, &next_enc),
                        arrival: arrival_wall_us,
                        handler: tool_idx,
                        count: arg_slices.len() as u32,
                        timer: false,
                    };
                    let Some(log_payload) = write_log_record(&header, &arg_slices) else {
                        let defect = Defect {
                            reason: "log record exceeded MO_LIMIT_SAVED_BYTES".to_string(),
                            limit_name: Some("MO_LIMIT_SAVED_BYTES"),
                            limit: MO_LIMIT_SAVED_BYTES as usize,
                            detail: None,
                            frames: Vec::new(),
                            variables: Vec::new(),
                            variables_unavailable: false,
                        };
                        return self.handle_failure(
                            false,
                            tool_idx,
                            &tool_name,
                            read_only,
                            args_encoded,
                            arrival_wall_us,
                            now_mono_us,
                            3,
                            &defect,
                            work,
                        );
                    };

                    let log_rec = StoreRecord {
                        kind: StoreRecordKind::Log,
                        sequence: next_seq,
                        image: self.image_key,
                        bytes: log_payload,
                    };
                    if let Err(st) = self.store.append(&log_rec) {
                        return ToolCallResult {
                            status: store_to_service(st),
                            reply_encoded: None,
                            updated: false,
                            sequence: self.sequence,
                            error_message: st.text().map(str::to_string),
                            failure_id: None,
                            work,
                            events: Vec::new(),
                        };
                    }

                    let new_heap = Heap::new(self.limits.heap_size);
                    let decoded_state = match decode_value(
                        &new_heap,
                        &self.image.file().types,
                        state_ty,
                        &next_enc,
                    ) {
                        Ok(v) => v,
                        Err(_) => {
                            return ToolCallResult {
                                status: ServiceStatus::Memory,
                                reply_encoded: None,
                                updated: true,
                                sequence: next_seq,
                                error_message: Some("out of memory updating state".to_string()),
                                failure_id: None,
                                work,
                                events: Vec::new(),
                            };
                        }
                    };

                    self.state_heap = new_heap;
                    self.state = decoded_state;
                    self.encoded_state = next_enc;
                    self.sequence = next_seq;
                    self.encoded_sequence = next_seq;
                    self.metrics.mutations += 1;
                    updated = true;

                    if self.sequence - self.snapshot_sequence >= MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES
                    {
                        let snap = StoreRecord {
                            kind: StoreRecordKind::State,
                            sequence: self.sequence,
                            image: self.image_key,
                            bytes: self.encoded_state.clone(),
                        };
                        if let Err(st) = self.store.snapshot(&snap) {
                            return ToolCallResult {
                                status: store_to_service(st),
                                reply_encoded: None,
                                updated: true,
                                sequence: self.sequence,
                                error_message: st.text().map(str::to_string),
                                failure_id: None,
                                work,
                                events: Vec::new(),
                            };
                        }
                        self.snapshot_sequence = self.sequence;
                        self.metrics.snapshots += 1;
                    }

                    if self.recent_log.len() >= 128 {
                        let _ = self.recent_log.pop_front();
                    }
                    self.recent_log.push_back(RecentLogEntry {
                        sequence: self.sequence,
                        arrival: arrival_wall_us,
                        handler: tool_idx,
                        timer: false,
                        name: tool_name,
                        read_only: false,
                        argument_bytes: args_encoded.iter().map(|a| a.len() as u32).collect(),
                    });
                } else {
                    self.metrics.reads += 1;
                }

                ToolCallResult {
                    status: ServiceStatus::Ok,
                    reply_encoded: Some(reply_enc),
                    updated,
                    sequence: self.sequence,
                    error_message: None,
                    failure_id: None,
                    work,
                    events: Vec::new(),
                }
            }
            Outcome::Failed(defect) => {
                let class = classify_defect(&defect);
                self.handle_failure(
                    false,
                    tool_idx,
                    &tool_name,
                    read_only,
                    args_encoded,
                    arrival_wall_us,
                    now_mono_us,
                    class,
                    &defect,
                    work,
                )
            }
            Outcome::Stopped(defect) => {
                let class = classify_defect(&defect);
                self.handle_failure(
                    false,
                    tool_idx,
                    &tool_name,
                    read_only,
                    args_encoded,
                    arrival_wall_us,
                    now_mono_us,
                    class,
                    &defect,
                    work,
                )
            }
        }
    }

    /// Fires all due timers at `now_mono_us` / `wall_us`, returning any supervisor events.
    pub fn tick_timers(
        &mut self,
        now_mono_us: u64,
        wall_us: i64,
    ) -> Result<Vec<ServiceEvent>, ServiceStatus> {
        let mut events = Vec::new();
        let count = self.timers.len();
        for idx in 0..count {
            if self.supervisor.stopped {
                break;
            }
            if now_mono_us < self.timers[idx].next_due_us {
                continue;
            }
            let interval_us = self.timers[idx].interval_us;
            self.timers[idx].next_due_us = now_mono_us.saturating_add(interval_us);

            let timer_fn = self.service_def().timers[idx].function;
            let state_ty = self.service_def().state_type;
            let timer_name =
                short_handler_name(self.image.file().function_name(timer_fn)).to_string();

            let mut proc = Process::new(
                &self.image,
                self.limits,
                HostContext::stdio(Vec::new(), Vec::new()),
            );
            let state_in = decode_value(
                proc.heap(),
                &self.image.file().types,
                state_ty,
                &self.encoded_state,
            )
            .map_err(|_| ServiceStatus::Memory)?;

            let (outcome, work) = crate::call_with_budget_isolated(
                &mut proc,
                timer_fn,
                vec![state_in, Value::Time(wall_us)],
                MO_LIMIT_SERVICE_WORK,
            );
            self.metrics.handler_work = self.metrics.handler_work.saturating_add(work);
            let peak = proc.memory_peak();
            if peak > self.metrics.handler_heap_peak {
                self.metrics.handler_heap_peak = peak;
            }

            match outcome {
                Outcome::Returned(next_state) => {
                    let mut next_enc = Vec::new();
                    if encode_value_unshared(
                        &self.image.file().types,
                        state_ty,
                        &next_state,
                        &mut next_enc,
                    )
                    .is_err()
                    {
                        let defect = Defect {
                            reason: "timer state could not be saved".to_string(),
                            limit_name: Some("MO_LIMIT_SAVED_BYTES"),
                            limit: MO_LIMIT_SAVED_BYTES as usize,
                            detail: None,
                            frames: Vec::new(),
                            variables: Vec::new(),
                            variables_unavailable: false,
                        };
                        let res = self.handle_failure(
                            true,
                            idx as u32,
                            &timer_name,
                            false,
                            Vec::new(),
                            wall_us,
                            now_mono_us,
                            3,
                            &defect,
                            work,
                        );
                        events.extend(res.events);
                        if res.status == ServiceStatus::StoreIo {
                            return Err(ServiceStatus::StoreIo);
                        }
                        continue;
                    }

                    let next_seq = self.sequence + 1;
                    let header = LogRecordHeader {
                        state_checksum: fnv1a(FNV_OFFSET, &next_enc),
                        arrival: wall_us,
                        handler: idx as u32,
                        count: 0,
                        timer: true,
                    };
                    let log_payload =
                        write_log_record(&header, &[]).ok_or(ServiceStatus::Memory)?;
                    let log_rec = StoreRecord {
                        kind: StoreRecordKind::Log,
                        sequence: next_seq,
                        image: self.image_key,
                        bytes: log_payload,
                    };
                    self.store.append(&log_rec).map_err(store_to_service)?;

                    let new_heap = Heap::new(self.limits.heap_size);
                    let decoded_state =
                        decode_value(&new_heap, &self.image.file().types, state_ty, &next_enc)
                            .map_err(|_| ServiceStatus::Memory)?;
                    self.state_heap = new_heap;
                    self.state = decoded_state;
                    self.encoded_state = next_enc;
                    self.sequence = next_seq;
                    self.encoded_sequence = next_seq;
                    self.metrics.timer_ticks += 1;

                    if self.sequence - self.snapshot_sequence >= MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES
                    {
                        let snap = StoreRecord {
                            kind: StoreRecordKind::State,
                            sequence: self.sequence,
                            image: self.image_key,
                            bytes: self.encoded_state.clone(),
                        };
                        self.store.snapshot(&snap).map_err(store_to_service)?;
                        self.snapshot_sequence = self.sequence;
                        self.metrics.snapshots += 1;
                    }

                    if self.recent_log.len() >= 128 {
                        let _ = self.recent_log.pop_front();
                    }
                    self.recent_log.push_back(RecentLogEntry {
                        sequence: self.sequence,
                        arrival: wall_us,
                        handler: idx as u32,
                        timer: true,
                        name: timer_name,
                        read_only: false,
                        argument_bytes: Vec::new(),
                    });
                }
                Outcome::Failed(defect) | Outcome::Stopped(defect) => {
                    let class = classify_defect(&defect);
                    let res = self.handle_failure(
                        true,
                        idx as u32,
                        &timer_name,
                        false,
                        Vec::new(),
                        wall_us,
                        now_mono_us,
                        class,
                        &defect,
                        work,
                    );
                    events.extend(res.events);
                    if res.status == ServiceStatus::StoreIo {
                        return Err(ServiceStatus::StoreIo);
                    }
                }
            }
        }
        Ok(events)
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "failure capture needs full handler identity, arguments, defect, and timestamps"
    )]
    fn handle_failure(
        &mut self,
        timer: bool,
        handler: u32,
        name: &str,
        read_only: bool,
        arguments: Vec<Vec<u8>>,
        arrival_wall_us: i64,
        now_mono_us: u64,
        class: u8,
        defect: &Defect,
        work: u64,
    ) -> ToolCallResult {
        let mut events = Vec::new();
        let mut failure_id = None;
        let status_reason = match class {
            1 => "service work limit exceeded",
            2 => "service memory limit exceeded",
            _ => "service handler failed",
        };
        let defect_text =
            sanitize_defect_text(&mo_vm::render_defect_raw(self.image.file(), defect));

        if let Some((kind, payload)) = build_failure_payload(FailureBuildParams {
            sequence: self.sequence,
            arrival: arrival_wall_us,
            timer,
            handler,
            name,
            read_only,
            arguments,
            class,
            defect,
            image_key: self.image_key,
            limits: self.limits,
            build_digest: self.build_digest,
            build_label: &self.build_label,
            schema_digest: self.schema_digest,
            file: self.image.file(),
        }) {
            let state_rec = StoreRecord {
                kind: StoreRecordKind::FailureState,
                sequence: self.sequence,
                image: self.image_key,
                bytes: self.encoded_state.clone(),
            };
            let msg_rec = StoreRecord {
                kind: StoreRecordKind::FailureMessage,
                sequence: self.sequence,
                image: self.image_key,
                bytes: payload,
            };
            match self.store.capture_failure(&kind, &state_rec, &msg_rec) {
                CaptureStatus::Ok(id) => {
                    failure_id = Some(id);
                    self.supervisor.last_failure_id = Some(id);
                }
                CaptureStatus::Unsafe => {
                    self.supervisor.stopped = true;
                    self.supervisor.last_reason = Some(
                        StoreStatus::Io
                            .text()
                            .unwrap_or("service store unsafe")
                            .to_string(),
                    );
                    return ToolCallResult {
                        status: ServiceStatus::StoreIo,
                        reply_encoded: None,
                        updated: false,
                        sequence: self.sequence,
                        error_message: self.supervisor.last_reason.clone(),
                        failure_id: None,
                        work,
                        events,
                    };
                }
                CaptureStatus::Refused | CaptureStatus::Damaged => {}
            }
        }
        events.push(ServiceEvent::FailureCaptured {
            id: failure_id.unwrap_or(0),
            handler: name.to_string(),
            reason: status_reason.to_string(),
            defect_text,
        });

        let elapsed_us = now_mono_us.saturating_sub(self.started_mono_us);
        self.push_event_summary(ServiceEventSummary {
            number: 0,
            kind: "failure",
            reason: status_reason.to_string(),
            handler_name: name.to_string(),
            wall_us: arrival_wall_us,
            elapsed_us,
            record_id: failure_id.unwrap_or(0),
            record_unavailable: if failure_id.is_none() {
                "capture refused".to_string()
            } else {
                String::new()
            },
        });

        // Update supervisor failure/restart windows.
        self.supervisor.total_failures += 1;
        self.supervisor.last_reason = Some(status_reason.to_string());
        let fwin = MO_LIMIT_SERVICE_FAILURE_WINDOW_US;
        while let Some(&ts) = self.supervisor.failures.front() {
            if now_mono_us.saturating_sub(ts) > fwin {
                let _ = self.supervisor.failures.pop_front();
            } else {
                break;
            }
        }
        self.supervisor.failures.push_back(now_mono_us);

        if self.supervisor.failures.len() as u64 >= MO_LIMIT_SERVICE_FAILURES {
            self.supervisor.failures.clear();
            let rwin = MO_LIMIT_SERVICE_RESTART_WINDOW_US;
            while let Some(&ts) = self.supervisor.restarts.front() {
                if now_mono_us.saturating_sub(ts) > rwin {
                    let _ = self.supervisor.restarts.pop_front();
                } else {
                    break;
                }
            }
            self.supervisor.restarts.push_back(now_mono_us);
            self.supervisor.total_restarts += 1;
            let restart_count = self.supervisor.restarts.len() as u64;

            // Always emit restart event (including "3 of 3") before stopping!
            self.push_event_summary(ServiceEventSummary {
                number: 0,
                kind: "restart",
                reason: status_reason.to_string(),
                handler_name: name.to_string(),
                wall_us: arrival_wall_us,
                elapsed_us,
                record_id: failure_id.unwrap_or(0),
                record_unavailable: String::new(),
            });
            events.push(ServiceEvent::Restarted {
                restarts: restart_count,
                reason: status_reason.to_string(),
            });

            if restart_count >= MO_LIMIT_SERVICE_RESTARTS {
                self.supervisor.stopped = true;
                self.push_event_summary(ServiceEventSummary {
                    number: 0,
                    kind: "stop",
                    reason: status_reason.to_string(),
                    handler_name: name.to_string(),
                    wall_us: arrival_wall_us,
                    elapsed_us,
                    record_id: failure_id.unwrap_or(0),
                    record_unavailable: String::new(),
                });
                events.push(ServiceEvent::Stopped {
                    reason: status_reason.to_string(),
                });
            } else {
                let new_heap = Heap::new(self.limits.heap_size);
                if let Ok(reloaded) = decode_value(
                    &new_heap,
                    &self.image.file().types,
                    self.service_def().state_type,
                    &self.encoded_state,
                ) {
                    self.state_heap = new_heap;
                    self.state = reloaded;
                }
            }
        }

        ToolCallResult {
            status: ServiceStatus::Failed,
            reply_encoded: None,
            updated: false,
            sequence: self.sequence,
            error_message: Some(status_reason.to_string()),
            failure_id,
            work,
            events,
        }
    }

    /// Maximum mailbox capacity constant (`MO_LIMIT_SERVICE_MAILBOX`).
    #[must_use]
    pub const fn mailbox_capacity(&self) -> u64 {
        MO_LIMIT_SERVICE_MAILBOX
    }

    /// Monotonic timestamp when this service started.
    #[must_use]
    pub fn started_mono_us(&self) -> u64 {
        self.started_mono_us
    }
}

/// Classifies a VM [`Defect`] into the canonical failure signature class (`0` = defect,
/// `1` = work, `2` = memory).
#[must_use]
pub fn classify_defect(defect: &Defect) -> u8 {
    if defect.reason.contains("instruction budget") {
        1 // work
    } else if defect.limit_name.is_some() || defect.reason.contains("out of memory") {
        2 // memory
    } else {
        0 // defect
    }
}

/// Extracts payload field `field` from a `Choice` value (`Result`/`Option`/enum).
#[must_use]
pub fn extract_choice_field(
    types: &mo_bytecode::types::TypeTable,
    ty: u32,
    val: &Value,
    field: usize,
) -> Option<Value> {
    let tag = val.as_tag();
    let case = types.get(ty).cases.get(tag as usize)?;
    match types.case_shape(ty, tag) {
        mo_bytecode::CaseShape::Empty => None,
        mo_bytecode::CaseShape::Inline => match val {
            Value::ChoiceInline { payload, .. } if field == 0 => {
                let fty = case.fields.first()?.ty;
                Some(mo_heap::hash::inline_payload_to_value(
                    *payload,
                    types.get(fty).kind,
                ))
            }
            _ => None,
        },
        mo_bytecode::CaseShape::Ref => match val {
            Value::ChoiceRef { payload, .. } if field == 0 => Some(Value::Ref(payload.clone())),
            _ => None,
        },
        mo_bytecode::CaseShape::Boxed => match val {
            Value::ChoiceRef { payload, .. } => match payload.obj() {
                mo_heap::HeapObj::CaseBlock { fields, .. } => fields.get(field).cloned(),
                _ => None,
            },
            _ => None,
        },
    }
}

/// Sanitizes rendered defect lines so embedded newlines inside user strings can never
/// start a forged `mo serve: ` event line at column 0 (`SERVE_FORGING`).
fn sanitize_defect_text(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len() + 16);
    for (idx, line) in raw.split('\n').enumerate() {
        if idx > 0 {
            out.push('\n');
            if !line.is_empty() && !line.starts_with("  ") {
                out.push_str("  ");
            }
        }
        out.push_str(line);
    }
    out
}

/// Extracts the short user-visible handler name from a compiled bytecode function name
/// (e.g. `"Overloaded.tick@Count"` -> `"tick"`).
#[must_use]
pub fn short_handler_name(full: &str) -> &str {
    let before_at = full.split('@').next().unwrap_or(full);
    let after_dot = before_at.rsplit('.').next().unwrap_or(before_at);
    after_dot.split('#').next().unwrap_or(after_dot)
}

pub(crate) fn eval_service_timers(
    image: &Image,
    service_idx: u32,
    limits: Limits,
    now_mono_us: u64,
) -> Result<Vec<TimerSlot>, ServiceStatus> {
    let svc = &image.file().services[service_idx as usize];
    let mut timers = Vec::with_capacity(svc.timers.len());
    for t in &svc.timers {
        let mut proc = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
        let (outcome, _) = proc.call_with_budget(t.interval, Vec::new(), MO_LIMIT_SERVICE_WORK);
        let interval_us = match outcome {
            Outcome::Returned(v) => {
                let us = v.as_duration();
                if us <= 0 {
                    return Err(ServiceStatus::Invalid);
                }
                us as u64
            }
            Outcome::Failed(defect) | Outcome::Stopped(defect) => {
                let tname = short_handler_name(image.file().function_name(t.function));
                let raw = mo_vm::render_defect_raw(image.file(), &defect);
                eprintln!("mo serve: failure: {tname}: service handler failed\nbug: {raw}");
                return Err(ServiceStatus::Failed);
            }
        };
        timers.push(TimerSlot {
            interval_us,
            next_due_us: now_mono_us,
        });
    }
    Ok(timers)
}

fn replay_handler_label(
    file: &BytecodeFile,
    service_idx: u32,
    timer: bool,
    handler: u32,
) -> String {
    let svc = &file.services[service_idx as usize];
    if timer {
        svc.timers
            .get(handler as usize)
            .map(|t| short_handler_name(file.function_name(t.function)).to_string())
            .unwrap_or_else(|| "service".to_string())
    } else {
        svc.tools
            .get(handler as usize)
            .map(|t| file.string(t.name).to_string())
            .unwrap_or_else(|| "service".to_string())
    }
}

/// Human-readable description for [`ServiceStatus`] (`mo_service_status_text`).
#[must_use]
pub fn service_status_text(st: ServiceStatus) -> &'static str {
    match st {
        ServiceStatus::Ok => "ok",
        ServiceStatus::Failed => "service handler failed",
        ServiceStatus::Stopped => "service stopped",
        ServiceStatus::StoreIo => "service store I/O failed",
        ServiceStatus::Corrupt => {
            "saved service data is damaged or was written by a different program; recovery refused"
        }
        ServiceStatus::Memory => "service ran out of memory",
        ServiceStatus::Full => "service mailbox is full",
        ServiceStatus::Invalid => "invalid service call",
    }
}

fn store_to_service(st: StoreStatus) -> ServiceStatus {
    match st {
        StoreStatus::Ok => ServiceStatus::Ok,
        StoreStatus::Corrupt | StoreStatus::EmptyPath | StoreStatus::MemoryPath => {
            ServiceStatus::Corrupt
        }
        StoreStatus::Memory => ServiceStatus::Memory,
        StoreStatus::Io | StoreStatus::Handles | StoreStatus::Busy => ServiceStatus::StoreIo,
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "replay helper needs image, service index, limits, encoded state, and log record fields"
)]
fn replay_one_message(
    image: &Image,
    service_idx: u32,
    limits: Limits,
    state_enc: &[u8],
    timer: bool,
    handler: u32,
    arrival: i64,
    args: &[&[u8]],
) -> Result<(Vec<u8>, String), ServiceStatus> {
    let file = image.file();
    let svc = &file.services[service_idx as usize];
    let state_ty = svc.state_type;
    let mut proc = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
    let state_in = decode_value(proc.heap(), &file.types, state_ty, state_enc)
        .map_err(|_| ServiceStatus::Corrupt)?;

    let (fn_idx, fn_name, call_args) = if timer {
        let t = svc
            .timers
            .get(handler as usize)
            .ok_or(ServiceStatus::Corrupt)?;
        if !args.is_empty() {
            return Err(ServiceStatus::Corrupt);
        }
        (
            t.function,
            file.function_name(t.function).to_string(),
            vec![state_in, Value::Time(arrival)],
        )
    } else {
        let tool = svc
            .tools
            .get(handler as usize)
            .ok_or(ServiceStatus::Corrupt)?;
        if tool.read_only || args.len() != tool.argument_types.len() {
            return Err(ServiceStatus::Corrupt);
        }
        let mut v = Vec::with_capacity(2 + args.len());
        v.push(state_in);
        v.push(Value::Time(arrival));
        for (enc, &aty) in args.iter().zip(&tool.argument_types) {
            let decoded = decode_value(proc.heap(), &file.types, aty, enc)
                .map_err(|_| ServiceStatus::Corrupt)?;
            v.push(decoded);
        }
        (tool.function, file.string(tool.name).to_string(), v)
    };

    // Sanity check parameter ownership count matches.
    let _ = ParamOwnership::Owned;
    let (outcome, _) = proc.call_with_budget(fn_idx, call_args, MO_LIMIT_SERVICE_WORK);
    let out_val = match outcome {
        Outcome::Returned(v) => v,
        Outcome::Failed(_) | Outcome::Stopped(_) => return Err(ServiceStatus::Corrupt),
    };
    let next_state = if timer {
        out_val
    } else {
        let reply_ty = svc.tools[handler as usize].result_type;
        let rdef = file.types.get(reply_ty);
        let is_result = rdef.kind == mo_bytecode::TypeKind::Choice
            && rdef
                .name
                .as_deref()
                .is_some_and(|n| n.starts_with("Result("));
        let pair_val = if is_result {
            if out_val.as_tag() != 0 {
                return Err(ServiceStatus::Corrupt);
            }
            extract_choice_field(&file.types, reply_ty, &out_val, 0)
                .ok_or(ServiceStatus::Corrupt)?
        } else {
            out_val
        };
        match pair_val.as_heap_ref().obj() {
            mo_heap::HeapObj::Tuple { fields, .. } if fields.len() == 2 => fields[0].clone(),
            _ => return Err(ServiceStatus::Corrupt),
        }
    };
    let mut next_enc = Vec::new();
    encode_value(&file.types, state_ty, &next_state, &mut next_enc)
        .map_err(|_| ServiceStatus::Corrupt)?;
    Ok((next_enc, fn_name))
}
