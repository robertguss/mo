//! Service simulation engine (`Opcode::Sim` + `SimOpId`) and live actors,
//! matching `src/vm/simulation.c`, `src/vm/service.c`, `src/host/store.c`,
//! and `src/base/log-record.c`.

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::rc::Rc;
use std::sync::Mutex;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use mo_bytecode::{
    BytecodeFile, Capability, FNV_OFFSET, NO_INDEX, ParamOwnership, ServiceDef, TypeKind, fnv1a,
};
use mo_def::SimOpId;
use mo_def::limits::{
    MO_DEFAULT_CALL_DEPTH, MO_DEFAULT_STACK_SIZE, MO_LIMIT_SAVED_BYTES,
    MO_LIMIT_SERVICE_FAILURE_WINDOW_US, MO_LIMIT_SERVICE_FAILURES, MO_LIMIT_SERVICE_MAILBOX,
    MO_LIMIT_SERVICE_MESSAGE_MEMORY, MO_LIMIT_SERVICE_RESTART_WINDOW_US, MO_LIMIT_SERVICE_RESTARTS,
    MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES, MO_LIMIT_SERVICE_WORK, MO_LIMIT_SIMULATION_TICKS,
    MO_LIMIT_STORES, MO_TIME_MAX_US, MO_TIME_MIN_US,
};
use mo_heap::print::value_print;
use mo_heap::saved::{SavedError, copy_saved, decode_value, encode_value, encode_value_unshared};
use mo_heap::{Heap, Value};
use mo_host::HostContext;

use crate::{
    ControlledOutcome, Defect, ExecCtx, Frame, Globals, Image, Limits, RegState, frame_size_bytes,
};

// ---------------------------------------------------------------------------
// Global open store counter (MO_LIMIT_STORES = 8)
// ---------------------------------------------------------------------------

static OPEN_STORES: Mutex<u32> = Mutex::new(0);

#[derive(Clone, Copy, PartialEq, Eq)]
enum StoreRecordKind {
    State = 1,
    Log = 2,
}

#[derive(Clone)]
struct StoreRecord {
    kind: StoreRecordKind,
    sequence: u64,
    image: u64,
    bytes: Vec<u8>,
}

struct SimStore {
    snapshot: Option<StoreRecord>,
    log: Vec<StoreRecord>,
    head: Option<u64>,
}

impl SimStore {
    fn open() -> Result<Self, ()> {
        let mut count = OPEN_STORES
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if u64::from(*count) >= MO_LIMIT_STORES {
            return Err(());
        }
        *count += 1;
        Ok(Self {
            snapshot: None,
            log: Vec::new(),
            head: None,
        })
    }

    fn save_snapshot(&mut self, seq: u64, image: u64, bytes: Vec<u8>) -> bool {
        if bytes.len() as u64 > MO_LIMIT_SAVED_BYTES || seq > i64::MAX as u64 {
            return false;
        }
        self.snapshot = Some(StoreRecord {
            kind: StoreRecordKind::State,
            sequence: seq,
            image,
            bytes,
        });
        self.log.clear();
        self.head = Some(seq);
        true
    }

    fn append_log(&mut self, seq: u64, image: u64, bytes: Vec<u8>) -> bool {
        if bytes.len() as u64 > MO_LIMIT_SAVED_BYTES || seq > i64::MAX as u64 {
            return false;
        }
        let Some(cur_head) = self.head else {
            return false;
        };
        if seq != cur_head + 1 {
            return false;
        }
        self.log.push(StoreRecord {
            kind: StoreRecordKind::Log,
            sequence: seq,
            image,
            bytes,
        });
        self.head = Some(seq);
        true
    }

    fn load_records(&self) -> Vec<StoreRecord> {
        let mut out = Vec::with_capacity(usize::from(self.snapshot.is_some()) + self.log.len());
        if let Some(ref snap) = self.snapshot {
            out.push(snap.clone());
        }
        out.extend(self.log.iter().cloned());
        out
    }
}

impl Drop for SimStore {
    fn drop(&mut self) {
        let mut count = OPEN_STORES
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        debug_assert!(*count > 0);
        *count = count.saturating_sub(1);
    }
}

// ---------------------------------------------------------------------------
// Service status & limits (exact strings from src/vm/service.c & store.c)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceStatus {
    Ok,
    Busy,
    Down,
    Memory,
    Work,
    Defect,
    Interval,
    Store,
    SavedInvalid,
}

impl ServiceStatus {
    #[must_use]
    pub fn text(self) -> &'static str {
        match self {
            Self::Ok => "service message completed",
            Self::Busy => "service mailbox is full",
            Self::Down => "service is stopped",
            Self::Memory => "service memory limit exceeded",
            Self::Work => "service work limit exceeded",
            Self::Defect => "service handler failed",
            Self::Interval => "service timer interval must be positive",
            Self::Store => {
                "service store failed; reopen it to see whether the last change was saved"
            }
            Self::SavedInvalid => {
                "saved service data is damaged or was written by a different program; recovery refused"
            }
        }
    }
}

fn service_limits() -> Limits {
    Limits {
        stack_size: MO_DEFAULT_STACK_SIZE as usize,
        call_depth: MO_DEFAULT_CALL_DEPTH as u32,
        heap_size: (MO_LIMIT_SERVICE_MESSAGE_MEMORY - MO_DEFAULT_STACK_SIZE) as usize,
    }
}

fn image_key(file: &BytecodeFile, descriptor: u32) -> u64 {
    file.program_checksum ^ (u64::from(descriptor) << 32)
}

// ---------------------------------------------------------------------------
// Log record binary codec (25-byte header + length-prefixed args)
// ---------------------------------------------------------------------------

const LOG_RECORD_HEADER_BYTES: usize = 25;
const LOG_RECORD_LENGTH_BYTES: usize = 4;

struct LogRecordHeader {
    timer: bool,
    handler: u32,
    arrival: i64,
    count: u32,
    state_checksum: u64,
}

fn write_log_record_header(out: &mut Vec<u8>, h: &LogRecordHeader) {
    out.push(u8::from(h.timer));
    out.extend_from_slice(&h.handler.to_le_bytes());
    out.extend_from_slice(&h.arrival.to_le_bytes());
    out.extend_from_slice(&h.count.to_le_bytes());
    out.extend_from_slice(&h.state_checksum.to_le_bytes());
}

fn read_log_record_header(bytes: &[u8]) -> Option<(LogRecordHeader, usize)> {
    if bytes.len() < LOG_RECORD_HEADER_BYTES {
        return None;
    }
    let timer_b = bytes[0];
    if timer_b > 1 {
        return None;
    }
    let handler = u32::from_le_bytes([bytes[1], bytes[2], bytes[3], bytes[4]]);
    let arrival = i64::from_le_bytes([
        bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
    ]);
    let count = u32::from_le_bytes([bytes[13], bytes[14], bytes[15], bytes[16]]);
    let state_checksum = u64::from_le_bytes([
        bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23], bytes[24],
    ]);
    Some((
        LogRecordHeader {
            timer: timer_b != 0,
            handler,
            arrival,
            count,
            state_checksum,
        },
        LOG_RECORD_HEADER_BYTES,
    ))
}

// ---------------------------------------------------------------------------
// Shared program view (for executing service handlers inside ExecCtx)
// ---------------------------------------------------------------------------

pub(crate) struct ProgramView<'a> {
    pub file: &'a BytecodeFile,
    pub constants: &'a [Value],
    pub captureless: &'a HashMap<u32, Value>,
    pub globals: &'a [Option<Value>],
    pub trace: Option<&'a RefCell<crate::VmTrace>>,
}

impl<'a> ProgramView<'a> {
    pub fn from_image(image: &'a Image) -> Self {
        Self {
            file: &image.file,
            constants: &image.constants,
            captureless: &image.captureless_closures,
            globals: &image.globals,
            trace: image.trace.as_ref(),
        }
    }
}

fn invoke_service_fn(
    prog: &ProgramView<'_>,
    heap: &Heap,
    fn_idx: u32,
    args: Vec<Value>,
) -> (ServiceStatus, Option<Value>, Option<Defect>) {
    let limits = service_limits();
    let fn_def = &prog.file.functions[fn_idx as usize];
    let bytes = frame_size_bytes(fn_def.register_types.len());
    if limits.call_depth < 1 || bytes > limits.stack_size {
        return (ServiceStatus::Defect, None, None);
    }

    let mut regs = vec![None; fn_def.register_types.len()];
    let mut states = vec![RegState::Undefined; fn_def.register_types.len()];
    for (i, (param, val)) in fn_def.parameters.iter().zip(args).enumerate() {
        regs[i] = Some(val);
        states[i] = if prog.file.types.is_reference(param.ty) {
            match param.ownership {
                ParamOwnership::Owned => RegState::Owned,
                ParamOwnership::Borrowed => RegState::Borrowed,
            }
        } else {
            RegState::Value
        };
    }

    let mut frames = vec![Frame {
        function: fn_idx,
        pc: 0,
        destination: 0,
        resume_pc: 0,
        frame_bytes: bytes,
        regs,
        states,
    }];
    let mut stack_bytes = bytes;
    let mut dummy_host = HostContext::stdio(Vec::new(), Vec::new());

    if let Some(cell) = prog.trace {
        let mut tr = cell.borrow_mut();
        if let Some(ref mut entered) = tr.entered
            && let Some(slot) = entered.get_mut(fn_idx as usize)
        {
            *slot = true;
        }
    }

    let mut ctx = ExecCtx {
        file: prog.file,
        constants: prog.constants,
        captureless: prog.captureless,
        globals: Globals::ReadOnly(prog.globals),
        heap,
        host: &mut dummy_host,
        limits,
        frames: &mut frames,
        stack_bytes: &mut stack_bytes,
        work_remaining: Some(MO_LIMIT_SERVICE_WORK + 1),
        keep_running: None,
        trace: prog.trace,
        observer: None,
    };

    match ctx.run_controlled() {
        ControlledOutcome::Returned(v) => (ServiceStatus::Ok, Some(v), None),
        ControlledOutcome::Cancelled => (ServiceStatus::Work, None, None),
        ControlledOutcome::Failed(d) | ControlledOutcome::Stopped(d) => {
            let is_mem =
                d.limit_name == Some("--max-heap") || d.reason.starts_with("running out of memory");
            let status = if is_mem {
                ServiceStatus::Memory
            } else {
                ServiceStatus::Defect
            };
            (status, None, Some(d))
        }
    }
}

// ---------------------------------------------------------------------------
// Service loop (exact port of src/vm/service.c)
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct ServiceTime {
    now: i64,
    elapsed: u64,
}

#[derive(Clone, Copy)]
struct ServiceTimerState {
    interval: u64,
    next: u64,
    waiting: bool,
}

struct ServiceMessage {
    arrival: i64,
    handler: u32,
    timer: bool,
    arguments: Vec<Value>,
}

struct ServiceCallResult {
    function: u32,
    result_type: u32,
    status: ServiceStatus,
    returned: bool,
    updated: bool,
    value: Option<Value>,
    new_state: Option<Value>,
    defect: Option<Defect>,
}

struct FailureInfo {
    status: ServiceStatus,
    now: i64,
    function: u32,
    defect: Option<Box<Defect>>,
}

struct StepReply {
    timer: bool,
    handler: u32,
    updated: bool,
    status: ServiceStatus,
    result_type: u32,
    value: Option<Value>,
    defect: Option<Defect>,
}

struct ServiceLoop {
    descriptor: u32,
    image_key: u64,
    heap: Heap,
    state: Value,
    timers: Vec<ServiceTimerState>,
    mailbox: VecDeque<ServiceMessage>,
    running: bool,
    sequence: u64,
    snapshot_sequence: u64,
    current_encoding: Vec<u8>,
    failures: Vec<u64>,
    restarts: Vec<u64>,
    last_clock: u64,
}

fn encoded_copy(
    heap: &Heap,
    file: &BytecodeFile,
    ty: u32,
    val: &Value,
    encoded: &mut Vec<u8>,
) -> Result<Value, SavedError> {
    encoded.clear();
    let unshared = encode_value_unshared(&file.types, ty, val, encoded)?;
    if unshared {
        Ok(val.clone())
    } else {
        decode_value(heap, &file.types, ty, encoded)
    }
}

fn extract_replacement(
    heap: &Heap,
    file: &BytecodeFile,
    service: &ServiceDef,
    timer: bool,
    handler: u32,
    result_type: u32,
    value: &Value,
) -> Option<Value> {
    if !timer && service.tools[handler as usize].read_only {
        return None;
    }
    if timer {
        return Some(value.clone());
    }
    let payload = if file.types.get(result_type).kind == TypeKind::Choice {
        if value.as_tag() != 0 {
            return None;
        }
        heap.extract_case_field(&file.types, result_type, value, 0, 0)
    } else {
        value.clone()
    };
    Some(heap.extract_field(&payload, 0))
}

#[expect(
    clippy::too_many_arguments,
    reason = "internal service handler helper forwards VM, service, and persistence state"
)]
fn call_handler(
    prog: &ProgramView<'_>,
    heap: &Heap,
    service: &ServiceDef,
    timer: bool,
    handler: u32,
    state: &Value,
    arrival: i64,
    arguments: &[Value],
    persist_buf: Option<&mut Vec<u8>>,
) -> ServiceCallResult {
    let function = if timer {
        service.timers[handler as usize].function
    } else {
        service.tools[handler as usize].function
    };
    let fn_def = &prog.file.functions[function as usize];
    let mut call_args = Vec::with_capacity(fn_def.parameters.len());
    call_args.push(state.clone());
    call_args.push(Value::Time(arrival));
    for arg in arguments {
        call_args.push(arg.clone());
    }
    let (status, value, defect) = invoke_service_fn(prog, heap, function, call_args);
    let returned = status == ServiceStatus::Ok;
    let mut result = ServiceCallResult {
        function,
        result_type: fn_def.result_type,
        status,
        returned,
        updated: false,
        value,
        new_state: None,
        defect,
    };
    if !returned {
        return result;
    }
    let Some(val_ref) = result.value.as_ref() else {
        return result;
    };
    let Some(next) = extract_replacement(
        heap,
        prog.file,
        service,
        timer,
        handler,
        fn_def.result_type,
        val_ref,
    ) else {
        return result;
    };
    if let Some(encoded) = persist_buf {
        match encoded_copy(heap, prog.file, service.state_type, &next, encoded) {
            Ok(copied) => {
                result.new_state = Some(copied);
                result.updated = true;
            }
            Err(_) => {
                result.status = ServiceStatus::Memory;
            }
        }
    } else {
        result.new_state = Some(next);
        result.updated = true;
    }
    result
}

fn record_window(times: &mut Vec<u64>, now: u64, window: u64) -> usize {
    times.retain(|&t| now.saturating_sub(t) < window);
    times.push(now);
    times.len()
}

fn advance_timer_state(timer: &mut ServiceTimerState, elapsed: u64) {
    let diff = u128::from(elapsed) - u128::from(timer.next);
    let interval = u128::from(timer.interval);
    let steps = diff / interval + 1;
    let next = u128::from(timer.next) + steps * interval;
    timer.next = u64::try_from(next).unwrap_or(u64::MAX);
}

impl ServiceLoop {
    fn open(
        prog: &ProgramView<'_>,
        descriptor: u32,
        store: Option<&mut SimStore>,
        time: ServiceTime,
    ) -> Result<Self, FailureInfo> {
        let service = &prog.file.services[descriptor as usize];
        let limits = service_limits();
        let heap = Heap::new(limits.heap_size);
        let ikey = image_key(prog.file, descriptor);

        // 1. Evaluate every timer interval function.
        let mut timers = Vec::with_capacity(service.timers.len());
        for t in &service.timers {
            let (status, val, defect) = invoke_service_fn(prog, &heap, t.interval, Vec::new());
            if status != ServiceStatus::Ok {
                return Err(FailureInfo {
                    status,
                    now: time.now,
                    function: t.interval,
                    defect: defect.map(Box::new),
                });
            }
            let Some(val) = val else {
                return Err(FailureInfo {
                    status: ServiceStatus::Defect,
                    now: time.now,
                    function: t.interval,
                    defect: None,
                });
            };
            let interval = val.as_duration();
            if interval <= 0 {
                return Err(FailureInfo {
                    status: ServiceStatus::Interval,
                    now: time.now,
                    function: t.interval,
                    defect: None,
                });
            }
            timers.push(ServiceTimerState {
                interval: interval as u64,
                next: time.elapsed,
                waiting: false,
            });
        }

        let mut sequence = 0u64;
        let mut snapshot_sequence = 0u64;
        let mut current_encoding = Vec::new();
        let mut found = false;
        let mut snapshot_due = false;
        let mut state_opt: Option<Value> = None;

        // 2. Recover from store if present.
        if let Some(ref st) = store {
            let records = st.load_records();
            for rec in records {
                if rec.image != ikey {
                    return Err(FailureInfo {
                        status: ServiceStatus::SavedInvalid,
                        now: time.now,
                        function: NO_INDEX,
                        defect: None,
                    });
                }
                if rec.kind == StoreRecordKind::State {
                    if found {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    }
                    let Ok(decoded) =
                        decode_value(&heap, &prog.file.types, service.state_type, &rec.bytes)
                    else {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    };
                    current_encoding = rec.bytes;
                    state_opt = Some(decoded);
                    found = true;
                    sequence = rec.sequence;
                    snapshot_sequence = rec.sequence;
                } else {
                    if !found || rec.sequence != sequence + 1 {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    }
                    let Some((hdr, mut pos)) = read_log_record_header(&rec.bytes) else {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    };
                    if !(MO_TIME_MIN_US..=MO_TIME_MAX_US).contains(&hdr.arrival)
                        || (hdr.handler as usize)
                            >= if hdr.timer {
                                service.timers.len()
                            } else {
                                service.tools.len()
                            }
                    {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    }
                    let expected_count = if hdr.timer {
                        0
                    } else {
                        let tool = &service.tools[hdr.handler as usize];
                        if tool.read_only {
                            return Err(FailureInfo {
                                status: ServiceStatus::SavedInvalid,
                                now: time.now,
                                function: NO_INDEX,
                                defect: None,
                            });
                        }
                        tool.argument_types.len()
                    };
                    if hdr.count as usize != expected_count {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    }
                    let mut decoded_args = Vec::with_capacity(expected_count);
                    for i in 0..expected_count {
                        if pos + LOG_RECORD_LENGTH_BYTES > rec.bytes.len() {
                            return Err(FailureInfo {
                                status: ServiceStatus::SavedInvalid,
                                now: time.now,
                                function: NO_INDEX,
                                defect: None,
                            });
                        }
                        let alen = u32::from_le_bytes([
                            rec.bytes[pos],
                            rec.bytes[pos + 1],
                            rec.bytes[pos + 2],
                            rec.bytes[pos + 3],
                        ]) as usize;
                        pos += LOG_RECORD_LENGTH_BYTES;
                        if pos + alen > rec.bytes.len() {
                            return Err(FailureInfo {
                                status: ServiceStatus::SavedInvalid,
                                now: time.now,
                                function: NO_INDEX,
                                defect: None,
                            });
                        }
                        let arg_ty = service.tools[hdr.handler as usize].argument_types[i];
                        let Ok(v) = decode_value(
                            &heap,
                            &prog.file.types,
                            arg_ty,
                            &rec.bytes[pos..pos + alen],
                        ) else {
                            return Err(FailureInfo {
                                status: ServiceStatus::SavedInvalid,
                                now: time.now,
                                function: NO_INDEX,
                                defect: None,
                            });
                        };
                        decoded_args.push(v);
                        pos += alen;
                    }
                    if pos != rec.bytes.len() {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    }
                    let mut encoded = Vec::new();
                    let Some(cur_state) = state_opt.as_ref() else {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    };
                    let call = call_handler(
                        prog,
                        &heap,
                        service,
                        hdr.timer,
                        hdr.handler,
                        cur_state,
                        hdr.arrival,
                        &decoded_args,
                        Some(&mut encoded),
                    );
                    if !call.updated {
                        let status = if !call.returned
                            && matches!(call.status, ServiceStatus::Memory | ServiceStatus::Work)
                        {
                            call.status
                        } else {
                            ServiceStatus::SavedInvalid
                        };
                        return Err(FailureInfo {
                            status,
                            now: time.now,
                            function: call.function,
                            defect: call.defect.map(Box::new),
                        });
                    }
                    if fnv1a(FNV_OFFSET, &encoded) != hdr.state_checksum {
                        return Err(FailureInfo {
                            status: ServiceStatus::SavedInvalid,
                            now: time.now,
                            function: call.function,
                            defect: None,
                        });
                    }
                    current_encoding = encoded;
                    state_opt = call.new_state;
                    sequence = rec.sequence;
                }
            }
            if found
                && sequence != snapshot_sequence
                && sequence.is_multiple_of(MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES)
            {
                snapshot_due = true;
            }
        }

        // 3. If not recovered from store, run start function.
        let mut initial_encoded = Vec::new();
        let state = if let Some(s) = state_opt {
            s
        } else {
            let (status, val, defect) = invoke_service_fn(prog, &heap, service.start, Vec::new());
            if status != ServiceStatus::Ok {
                return Err(FailureInfo {
                    status,
                    now: time.now,
                    function: service.start,
                    defect: defect.map(Box::new),
                });
            }
            let Some(raw_state) = val else {
                return Err(FailureInfo {
                    status: ServiceStatus::Defect,
                    now: time.now,
                    function: service.start,
                    defect: None,
                });
            };
            if store.is_some() {
                match encoded_copy(
                    &heap,
                    prog.file,
                    service.state_type,
                    &raw_state,
                    &mut initial_encoded,
                ) {
                    Ok(copied) => {
                        snapshot_due = true;
                        copied
                    }
                    Err(_) => {
                        return Err(FailureInfo {
                            status: ServiceStatus::Memory,
                            now: time.now,
                            function: service.start,
                            defect: None,
                        });
                    }
                }
            } else {
                raw_state
            }
        };

        // 4. Write initial or due snapshot.
        if let Some(st) = store
            && snapshot_due
        {
            let bytes = if !initial_encoded.is_empty() {
                initial_encoded.clone()
            } else {
                current_encoding.clone()
            };
            if !st.save_snapshot(sequence, ikey, bytes) {
                return Err(FailureInfo {
                    status: ServiceStatus::Store,
                    now: time.now,
                    function: NO_INDEX,
                    defect: None,
                });
            }
            snapshot_sequence = sequence;
        }
        if !initial_encoded.is_empty() {
            current_encoding = initial_encoded;
        }

        let mut svc = Self {
            descriptor,
            image_key: ikey,
            heap,
            state,
            timers,
            mailbox: VecDeque::new(),
            running: true,
            sequence,
            snapshot_sequence,
            current_encoding,
            failures: Vec::new(),
            restarts: Vec::new(),
            last_clock: time.elapsed,
        };
        svc.start_timers(time);
        Ok(svc)
    }

    fn poll_timers(&mut self, time: ServiceTime) {
        self.last_clock = time.elapsed;
        for t in &mut self.timers {
            if t.waiting && t.next <= time.elapsed {
                advance_timer_state(t, time.elapsed);
            }
        }
        while (self.mailbox.len() as u64) < MO_LIMIT_SERVICE_MAILBOX {
            let mut first: Option<usize> = None;
            for (i, t) in self.timers.iter().enumerate() {
                if !t.waiting
                    && t.next <= time.elapsed
                    && first.is_none_or(|f| t.next < self.timers[f].next)
                {
                    first = Some(i);
                }
            }
            let Some(idx) = first else { break };
            self.mailbox.push_back(ServiceMessage {
                arrival: time.now,
                handler: idx as u32,
                timer: true,
                arguments: Vec::new(),
            });
            self.timers[idx].waiting = true;
            advance_timer_state(&mut self.timers[idx], time.elapsed);
        }
    }

    fn start_timers(&mut self, time: ServiceTime) {
        for t in &mut self.timers {
            t.waiting = false;
            t.next = time.elapsed;
        }
        self.poll_timers(time);
    }

    fn deadlines(&self, until: u64) -> (u64, u64) {
        debug_assert!(self.mailbox.is_empty());
        if !self.running {
            return (0, 0);
        }
        let mut count: u128 = 0;
        let mut first: u128 = u128::from(u64::MAX) + 1;
        for t in &self.timers {
            if t.next > until {
                continue;
            }
            count += (u128::from(until) - u128::from(t.next)) / u128::from(t.interval) + 1;
            if u128::from(t.next) < first {
                first = u128::from(t.next);
            }
        }
        let next = if count != 0 { first as u64 } else { 0 };
        let clamped = u64::try_from(count).unwrap_or(u64::MAX);
        (clamped, next)
    }

    fn enqueue(
        &mut self,
        file: &BytecodeFile,
        has_store: bool,
        tool: u32,
        arguments: &[Value],
        time: ServiceTime,
    ) -> ServiceStatus {
        if !self.running {
            return ServiceStatus::Down;
        }
        if (self.mailbox.len() as u64) >= MO_LIMIT_SERVICE_MAILBOX {
            return ServiceStatus::Busy;
        }
        let handler = &file.services[self.descriptor as usize].tools[tool as usize];
        let mut stored_args = Vec::with_capacity(arguments.len());
        let mut payload = LOG_RECORD_HEADER_BYTES;
        let mut encoded = Vec::new();
        for (i, arg) in arguments.iter().enumerate() {
            let arg_ty = handler.argument_types[i];
            if handler.read_only || !has_store {
                stored_args.push(arg.clone());
            } else {
                match encoded_copy(&self.heap, file, arg_ty, arg, &mut encoded) {
                    Ok(v) => {
                        payload = payload.saturating_add(LOG_RECORD_LENGTH_BYTES + encoded.len());
                        if (payload as u64) > MO_LIMIT_SAVED_BYTES {
                            return ServiceStatus::Memory;
                        }
                        stored_args.push(v);
                    }
                    Err(_) => return ServiceStatus::Memory,
                }
            }
        }
        self.mailbox.push_back(ServiceMessage {
            arrival: time.now,
            handler: tool,
            timer: false,
            arguments: stored_args,
        });
        ServiceStatus::Ok
    }

    fn persist(
        &mut self,
        file: &BytecodeFile,
        store: &mut SimStore,
        msg: &ServiceMessage,
        encoded_state: &[u8],
    ) -> bool {
        let mut bytes = Vec::new();
        let count = if msg.timer {
            0
        } else {
            file.services[self.descriptor as usize].tools[msg.handler as usize]
                .argument_types
                .len() as u32
        };
        let header = LogRecordHeader {
            timer: msg.timer,
            handler: msg.handler,
            arrival: msg.arrival,
            count,
            state_checksum: fnv1a(FNV_OFFSET, encoded_state),
        };
        write_log_record_header(&mut bytes, &header);
        let mut arg_buf = Vec::new();
        for i in 0..count as usize {
            arg_buf.clear();
            let arg_ty = file.services[self.descriptor as usize].tools[msg.handler as usize]
                .argument_types[i];
            if encode_value(&file.types, arg_ty, &msg.arguments[i], &mut arg_buf).is_err() {
                return false;
            }
            bytes.extend_from_slice(&(arg_buf.len() as u32).to_le_bytes());
            bytes.extend_from_slice(&arg_buf);
        }
        if !store.append_log(self.sequence + 1, self.image_key, bytes) {
            return false;
        }
        self.sequence += 1;
        if self
            .sequence
            .is_multiple_of(MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES)
        {
            if !store.save_snapshot(self.sequence, self.image_key, encoded_state.to_vec()) {
                return false;
            }
            self.snapshot_sequence = self.sequence;
        }
        true
    }

    fn drain_mailbox(&mut self, keep_tools: bool) {
        let old = std::mem::take(&mut self.mailbox);
        for msg in old {
            if keep_tools && !msg.timer {
                self.mailbox.push_back(msg);
            }
        }
    }

    fn supervise(&mut self, time: ServiceTime) -> bool {
        let fail_count = record_window(
            &mut self.failures,
            time.elapsed,
            MO_LIMIT_SERVICE_FAILURE_WINDOW_US,
        );
        if (fail_count as u64) != MO_LIMIT_SERVICE_FAILURES {
            return false;
        }
        self.failures.clear();
        let restart_count = record_window(
            &mut self.restarts,
            time.elapsed,
            MO_LIMIT_SERVICE_RESTART_WINDOW_US,
        );
        self.running = (restart_count as u64) != MO_LIMIT_SERVICE_RESTARTS;
        self.drain_mailbox(self.running);
        if self.running {
            self.start_timers(time);
        }
        true
    }

    fn step(
        &mut self,
        prog: &ProgramView<'_>,
        store: Option<&mut SimStore>,
        time: ServiceTime,
    ) -> Option<(StepReply, Option<FailureInfo>, bool)> {
        if !self.running {
            return None;
        }
        self.poll_timers(time);
        let msg = self.mailbox.pop_front()?;
        if msg.timer {
            self.timers[msg.handler as usize].waiting = false;
        }
        let service = &prog.file.services[self.descriptor as usize];
        let mut encoded = Vec::new();
        let call = call_handler(
            prog,
            &self.heap,
            service,
            msg.timer,
            msg.handler,
            &self.state,
            msg.arrival,
            &msg.arguments,
            if store.is_some() {
                Some(&mut encoded)
            } else {
                None
            },
        );

        let mut status = call.status;
        let mut updated = call.updated;
        let mut event_failure: Option<FailureInfo> = None;

        if call.returned {
            if status != ServiceStatus::Ok {
                // state encode failed -> Memory
            } else if updated {
                if let Some(st) = store {
                    if !self.persist(prog.file, st, &msg, &encoded) {
                        status = ServiceStatus::Store;
                        updated = false;
                        self.running = false;
                        event_failure = Some(FailureInfo {
                            status: ServiceStatus::Store,
                            now: time.now,
                            function: NO_INDEX,
                            defect: None,
                        });
                    } else {
                        self.current_encoding = encoded;
                    }
                }
                if updated && let Some(next_state) = call.new_state {
                    self.state = next_state;
                }
            }
        }

        let reply_value = if call.returned && status == ServiceStatus::Ok {
            call.value
        } else {
            None
        };
        let reply = StepReply {
            timer: msg.timer,
            handler: msg.handler,
            updated,
            status,
            result_type: call.result_type,
            value: reply_value,
            defect: call.defect,
        };

        let mut restarted = false;
        if !call.returned || status == ServiceStatus::Memory {
            restarted = self.supervise(time);
        }
        if !self.running {
            self.drain_mailbox(false);
        } else {
            self.poll_timers(time);
        }
        Some((reply, event_failure, restarted))
    }
}

// ---------------------------------------------------------------------------
// Simulation & LiveActor capability objects
// ---------------------------------------------------------------------------

struct SimulationInner {
    descriptor: u32,
    store: SimStore,
    loop_state: Option<ServiceLoop>,
    time: ServiceTime,
}

pub(crate) struct LiveActorInner {
    descriptor: u32,
    loop_state: Option<ServiceLoop>,
    base_instant: Instant,
    last_elapsed: u64,
}

enum SimHandle {
    Simulation(RefCell<SimulationInner>),
    LiveActor(RefCell<LiveActorInner>),
}

impl SimulationInner {
    fn is_running(&self) -> bool {
        self.loop_state.as_ref().is_some_and(|l| l.running)
    }

    fn format_failure(&self, file: &BytecodeFile, op: SimOpId, f: &FailureInfo) -> String {
        let service = &file.services[self.descriptor as usize];
        let tool0 = service.tools[0].function;
        let time_ty = file.functions[tool0 as usize].parameters[1].ty;
        let time_text = value_print(&file.types, time_ty, &Value::Time(f.now));
        let op_name = op.info().name;

        if f.function != NO_INDEX {
            let timer_idx = service.timers.iter().position(|t| t.interval == f.function);
            let is_start = f.function == service.start;
            let func_idx = if let Some(ti) = timer_idx {
                service.timers[ti].function
            } else {
                f.function
            };
            let fname = file.function_name(func_idx);
            let reason_str = f.defect.as_ref().map(|d| d.reason.as_str());
            let detail_str = f
                .defect
                .as_ref()
                .and_then(|d| d.detail.as_deref())
                .unwrap_or("");
            let r_sep = if reason_str.is_some() { ": " } else { "" };
            let r_txt = reason_str.unwrap_or("");
            let d_sep = if !detail_str.is_empty() { ": " } else { "" };

            if is_start {
                format!(
                    "Simulation.start at {time_text}: start value: {}{r_sep}{r_txt}{d_sep}{detail_str}",
                    f.status.text()
                )
            } else if timer_idx.is_some() {
                format!(
                    "Simulation.{op_name} at {time_text}: interval of timer {fname}: {}{r_sep}{r_txt}{d_sep}{detail_str}",
                    f.status.text()
                )
            } else {
                format!(
                    "Simulation handler {fname} at {time_text}: {}{r_sep}{r_txt}{d_sep}{detail_str}",
                    f.status.text()
                )
            }
        } else {
            format!("Simulation.{op_name} at {time_text}: {}", f.status.text())
        }
    }

    fn drain_all(
        &mut self,
        prog: &ProgramView<'_>,
        op: SimOpId,
        caller_heap: Option<&Heap>,
    ) -> Result<Option<(StepReply, Option<Value>)>, String> {
        let mut tool_reply: Option<(StepReply, Option<Value>)> = None;
        let service = &prog.file.services[self.descriptor as usize];

        loop {
            let step_res = {
                let Some(ref mut lp) = self.loop_state else {
                    break;
                };
                lp.step(prog, Some(&mut self.store), self.time)
            };
            let Some((reply, event_failure, _)) = step_res else {
                break;
            };
            if let Some(ev) = event_failure {
                return Err(self.format_failure(prog.file, op, &ev));
            }
            if reply.timer {
                if reply.status != ServiceStatus::Ok && reply.status != ServiceStatus::Store {
                    let f = FailureInfo {
                        status: reply.status,
                        now: self.time.now,
                        function: service.timers[reply.handler as usize].function,
                        defect: reply.defect.map(Box::new),
                    };
                    return Err(self.format_failure(prog.file, op, &f));
                }
            } else {
                let copied = if reply.status == ServiceStatus::Ok {
                    let (Some(to_heap), Some(raw)) = (caller_heap, reply.value.as_ref()) else {
                        return Err("Simulation reply copy-out: missing reply state".to_string());
                    };
                    match copy_saved(&prog.file.types, to_heap, reply.result_type, raw) {
                        Ok((v, _)) => Some(v),
                        Err(e) => {
                            let tool_name =
                                prog.file.string(service.tools[reply.handler as usize].name);
                            let upd = if reply.updated {
                                "; its update was committed"
                            } else {
                                ""
                            };
                            return Err(format!(
                                "Simulation reply copy-out: {}; tool {tool_name} executed{upd}; no rollback or retry",
                                e.refusal_message()
                            ));
                        }
                    }
                } else {
                    None
                };
                tool_reply = Some((reply, copied));
            }
        }
        Ok(tool_reply)
    }

    fn open_and_drain(&mut self, prog: &ProgramView<'_>, op: SimOpId) -> Result<(), String> {
        self.loop_state = None;
        match ServiceLoop::open(prog, self.descriptor, Some(&mut self.store), self.time) {
            Ok(lp) => {
                self.loop_state = Some(lp);
                self.drain_all(prog, op, None)?;
                Ok(())
            }
            Err(f) => Err(self.format_failure(prog.file, op, &f)),
        }
    }
}

// ---------------------------------------------------------------------------
// Public Simulation API (`src/vm/vm.h:mo_simulation_*` / `src/vm/simulation.c`)
// ---------------------------------------------------------------------------

/// Outcome of a simulation lifecycle operation (`enum mo_simulation_outcome`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimulationOutcome {
    Ok,
    Running,
    Down,
    Duration,
    Ticks,
    Stopped,
}

/// Outcome of sending a tool call into a simulation (`enum mo_simulation_sent`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimulationSent {
    Replied,
    Rejected,
    Down,
    ToolFailed,
    Failed,
    Argument,
    TooLarge,
    Refused,
    ReplyCopy,
    Stopped,
}

/// Failure notification delivered to simulation hooks (`struct mo_simulation_failure`).
#[derive(Debug, Clone)]
pub struct SimulationFailure {
    pub status: ServiceStatus,
    pub now: i64,
    pub function: u32,
    pub defect: Option<Defect>,
}

/// Reply payload from [`Simulation::send`] (`struct mo_simulation_reply`).
#[derive(Clone)]
pub struct SimulationReply {
    pub status: ServiceStatus,
    pub updated: bool,
    pub ty: u32,
    pub reply: Option<Value>,
    pub argument: u32,
    pub bytes: u64,
    pub refusal: Option<&'static str>,
}

impl Default for SimulationReply {
    fn default() -> Self {
        Self {
            status: ServiceStatus::Down,
            updated: false,
            ty: 0,
            reply: None,
            argument: 0,
            bytes: 0,
            refusal: None,
        }
    }
}

/// Deterministic in-memory service simulation harness (`struct mo_simulation`).
pub struct Simulation<'a> {
    image: &'a Image,
    descriptor: u32,
    store: SimStore,
    loop_state: Option<ServiceLoop>,
    time: ServiceTime,
    restarts: u64,
}

impl<'a> Simulation<'a> {
    /// Creates a new stopped simulation for service `descriptor` at `now` microseconds.
    #[must_use]
    pub fn new(image: &'a Image, descriptor: u32, now: i64) -> Option<Self> {
        let store = SimStore::open().ok()?;
        Some(Self {
            image,
            descriptor,
            store,
            loop_state: None,
            time: ServiceTime { now, elapsed: 0 },
            restarts: 0,
        })
    }

    /// Returns whether the service loop is currently running.
    #[must_use]
    pub fn running(&self) -> bool {
        self.loop_state.as_ref().is_some_and(|l| l.running)
    }

    /// Returns the current simulated wall-clock timestamp in microseconds.
    #[must_use]
    pub fn now(&self) -> i64 {
        self.time.now
    }

    /// Returns the total number of supervisor restarts (including a restart that stops the service).
    #[must_use]
    pub fn restarts(&self) -> u64 {
        self.restarts
    }

    /// Encodes the live service state into `out` (`mo_simulation_encode`).
    pub fn encode(&self, out: &mut Vec<u8>) -> bool {
        let Some(ref lp) = self.loop_state else {
            return false;
        };
        let state_ty = self.image.file.services[self.descriptor as usize].state_type;
        encode_value(&self.image.file.types, state_ty, &lp.state, out).is_ok()
    }

    /// Opens (starts or recovers) the service from its in-memory store (`mo_simulation_open`).
    pub fn open(
        &mut self,
        on_failure: &mut dyn FnMut(&SimulationFailure) -> bool,
    ) -> SimulationOutcome {
        if self.running() {
            return SimulationOutcome::Running;
        }
        self.loop_state = None;
        let prog = ProgramView::from_image(self.image);
        match ServiceLoop::open(&prog, self.descriptor, Some(&mut self.store), self.time) {
            Ok(lp) => {
                self.loop_state = Some(lp);
                SimulationOutcome::Ok
            }
            Err(f) => {
                let failure = SimulationFailure {
                    status: f.status,
                    now: f.now,
                    function: f.function,
                    defect: f.defect.map(|d| *d),
                };
                let _ = on_failure(&failure);
                SimulationOutcome::Down
            }
        }
    }

    fn drain_internal(
        &mut self,
        on_failure: &mut dyn FnMut(&SimulationFailure) -> bool,
        caller_heap: Option<&Heap>,
        reply_out: Option<&mut SimulationReply>,
    ) -> (SimulationOutcome, bool, bool) {
        let prog = ProgramView::from_image(self.image);
        let service = &self.image.file.services[self.descriptor as usize];
        let mut stopped = false;
        let mut replied = false;
        let mut refused = false;
        let mut reply_slot = reply_out;

        while !stopped {
            let step_res = {
                let Some(ref mut lp) = self.loop_state else {
                    break;
                };
                lp.step(&prog, Some(&mut self.store), self.time)
            };
            let Some((step_reply, event_failure, restarted)) = step_res else {
                break;
            };
            if restarted {
                self.restarts = self.restarts.saturating_add(1);
            }
            if let Some(ev) = event_failure {
                let failure = SimulationFailure {
                    status: ev.status,
                    now: ev.now,
                    function: ev.function,
                    defect: ev.defect.map(|d| *d),
                };
                if !on_failure(&failure) {
                    stopped = true;
                }
            }
            if step_reply.timer {
                if step_reply.status != ServiceStatus::Ok
                    && step_reply.status != ServiceStatus::Store
                    && !stopped
                {
                    let failure = SimulationFailure {
                        status: step_reply.status,
                        now: self.time.now,
                        function: service.timers[step_reply.handler as usize].function,
                        defect: step_reply.defect,
                    };
                    if !on_failure(&failure) {
                        stopped = true;
                    }
                }
            } else {
                replied = true;
                if let Some(out) = reply_slot.as_deref_mut() {
                    out.status = step_reply.status;
                    out.updated = step_reply.updated;
                    out.ty = step_reply.result_type;
                    if step_reply.status == ServiceStatus::Ok {
                        if let (Some(to_heap), Some(raw)) = (caller_heap, step_reply.value.as_ref())
                        {
                            match copy_saved(
                                &self.image.file.types,
                                to_heap,
                                step_reply.result_type,
                                raw,
                            ) {
                                Ok((copied, _)) => {
                                    out.reply = Some(copied);
                                }
                                Err(err) => {
                                    out.reply = None;
                                    out.refusal = Some(err.refusal_message());
                                    refused = true;
                                    stopped = true;
                                }
                            }
                        } else {
                            out.reply = None;
                            refused = true;
                            stopped = true;
                        }
                    }
                }
            }
        }
        let outcome = if stopped {
            SimulationOutcome::Stopped
        } else {
            SimulationOutcome::Ok
        };
        (outcome, replied, refused)
    }

    /// Drains any pending mailbox/timer work (`mo_simulation_drain`).
    pub fn drain(
        &mut self,
        on_failure: &mut dyn FnMut(&SimulationFailure) -> bool,
    ) -> SimulationOutcome {
        if self.loop_state.is_none() {
            return SimulationOutcome::Ok;
        }
        self.drain_internal(on_failure, None, None).0
    }

    /// Sends a tool call into the simulation and drains until it replies (`mo_simulation_send`).
    pub fn send(
        &mut self,
        on_failure: &mut dyn FnMut(&SimulationFailure) -> bool,
        heap: &Heap,
        tool: u32,
        arguments: &[Value],
    ) -> (SimulationSent, SimulationReply) {
        let mut out = SimulationReply::default();
        if !self.running() {
            return (SimulationSent::Down, out);
        }
        if self.drain(on_failure) != SimulationOutcome::Ok {
            return (SimulationSent::Stopped, out);
        }
        let file = &self.image.file;
        let handler = &file.services[self.descriptor as usize].tools[tool as usize];
        let mut copied = Vec::with_capacity(handler.argument_types.len());
        let mut message_bytes: u64 = LOG_RECORD_HEADER_BYTES as u64;
        {
            let Some(ref lp) = self.loop_state else {
                return (SimulationSent::Down, out);
            };
            for (i, &arg_ty) in handler.argument_types.iter().enumerate() {
                match copy_saved(&file.types, &lp.heap, arg_ty, &arguments[i]) {
                    Ok((val, enc_len)) => {
                        copied.push(val);
                        message_bytes = message_bytes.saturating_add(4 + enc_len as u64);
                    }
                    Err(err) => {
                        out.argument = i as u32;
                        out.refusal = Some(err.refusal_message());
                        return (SimulationSent::Argument, out);
                    }
                }
            }
        }
        if !handler.read_only && message_bytes > MO_LIMIT_SAVED_BYTES {
            out.bytes = message_bytes;
            return (SimulationSent::TooLarge, out);
        }
        let enq_status = {
            let Some(ref mut lp) = self.loop_state else {
                return (SimulationSent::Down, out);
            };
            lp.enqueue(file, true, tool, &copied, self.time)
        };
        drop(copied);
        out.status = enq_status;
        if enq_status == ServiceStatus::Memory {
            return (SimulationSent::Refused, out);
        }
        if enq_status == ServiceStatus::Ok {
            let (drained, replied, refused) =
                self.drain_internal(on_failure, Some(heap), Some(&mut out));
            if refused {
                return (SimulationSent::ReplyCopy, out);
            }
            if drained != SimulationOutcome::Ok {
                out.reply = None;
                return (SimulationSent::Stopped, out);
            }
            debug_assert!(replied);
        }
        let sent = match out.status {
            ServiceStatus::Ok => {
                if out.updated || handler.read_only {
                    SimulationSent::Replied
                } else {
                    SimulationSent::Rejected
                }
            }
            ServiceStatus::Down => SimulationSent::Down,
            ServiceStatus::Defect | ServiceStatus::Work | ServiceStatus::Memory => {
                SimulationSent::ToolFailed
            }
            ServiceStatus::Busy
            | ServiceStatus::Interval
            | ServiceStatus::Store
            | ServiceStatus::SavedInvalid => SimulationSent::Failed,
        };
        (sent, out)
    }

    /// Counts how many timer firings advancing by `by` microseconds would trigger (`mo_simulation_firings`).
    #[must_use]
    pub fn firings(&self, by: i64) -> Option<u64> {
        if by < 0 || i128::from(self.time.now) + i128::from(by) > i128::from(MO_TIME_MAX_US) {
            return None;
        }
        let count = if self.running() {
            self.loop_state
                .as_ref()
                .map_or(0, |lp| lp.deadlines(self.time.elapsed + by as u64).0)
        } else {
            0
        };
        Some(count)
    }

    /// Advances simulated time by `by` microseconds, firing due timers (`mo_simulation_advance`).
    pub fn advance(
        &mut self,
        on_failure: &mut dyn FnMut(&SimulationFailure) -> bool,
        by: i64,
    ) -> (SimulationOutcome, u64) {
        if by < 0 || i128::from(self.time.now) + i128::from(by) > i128::from(MO_TIME_MAX_US) {
            return (SimulationOutcome::Duration, 0);
        }
        let until = self.time.elapsed + by as u64;
        let target = self.time.now + by;
        let mut firings = 0u64;
        if self.running() {
            if self.drain(on_failure) != SimulationOutcome::Ok {
                return (SimulationOutcome::Stopped, 0);
            }
            firings = self.firings(by).unwrap_or(0);
            if firings > MO_LIMIT_SIMULATION_TICKS {
                return (SimulationOutcome::Ticks, firings);
            }
            loop {
                let (due, next) = match self.loop_state.as_ref() {
                    Some(lp) if lp.running => lp.deadlines(until),
                    _ => (0, 0),
                };
                if due == 0 {
                    break;
                }
                debug_assert!(next >= self.time.elapsed);
                self.time.now += (next - self.time.elapsed) as i64;
                self.time.elapsed = next;
                if self.drain(on_failure) != SimulationOutcome::Ok {
                    return (SimulationOutcome::Stopped, firings);
                }
            }
        }
        self.time = ServiceTime {
            now: target,
            elapsed: until,
        };
        (SimulationOutcome::Ok, firings)
    }

    /// Crashes the running service loop without touching its durable store (`mo_simulation_crash`).
    pub fn crash(&mut self) {
        self.loop_state = None;
    }
}

// ---------------------------------------------------------------------------
// LiveActor support (matches src/tools/run.c & src/vm/simulation.c)
// ---------------------------------------------------------------------------

pub(crate) fn find_actor_service_for_param(
    file: &BytecodeFile,
    main_fn: u32,
    param_idx: u32,
) -> Option<u32> {
    let dbg = &file.debug[main_fn as usize];
    for rn in &dbg.names {
        if rn.reg != param_idx || rn.code_start != 0 {
            continue;
        }
        let nm = file.string(rn.name);
        let mod_name = nm.strip_prefix("%actor.")?;
        for (s, svc) in file.services.iter().enumerate() {
            if file.string(svc.token_env) != "_MO_ACTOR" {
                continue;
            }
            let mname = file.string(file.modules[svc.module as usize].name);
            if mname == mod_name {
                return Some(s as u32);
            }
        }
        return None;
    }
    None
}

fn live_wall_now_us() -> i64 {
    let dur = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let us = i64::try_from(dur.as_micros()).unwrap_or(MO_TIME_MAX_US);
    us.clamp(MO_TIME_MIN_US, MO_TIME_MAX_US)
}

impl LiveActorInner {
    fn clock(&mut self) -> ServiceTime {
        let elapsed = self.base_instant.elapsed().as_micros() as u64;
        if elapsed >= self.last_elapsed {
            self.last_elapsed = elapsed;
        }
        ServiceTime {
            now: live_wall_now_us(),
            elapsed: self.last_elapsed,
        }
    }
}

pub(crate) fn make_live_actor_capability(
    image: &Image,
    heap: &Heap,
    ty: u32,
    service_idx: u32,
) -> Result<Value, String> {
    let prog = ProgramView::from_image(image);
    let base_instant = Instant::now();
    let time = ServiceTime {
        now: live_wall_now_us(),
        elapsed: 0,
    };
    let lp = ServiceLoop::open(&prog, service_idx, None, time)
        .map_err(|f| format!("could not start actor: {}", f.status.text()))?;
    let actor = SimHandle::LiveActor(RefCell::new(LiveActorInner {
        descriptor: service_idx,
        loop_state: Some(lp),
        base_instant,
        last_elapsed: 0,
    }));
    heap.alloc_capability(Capability::Simulation, ty, Rc::new(actor))
        .map_err(|_| "running out of memory".to_string())
}

// ---------------------------------------------------------------------------
// Opcode::Sim execution entry point
// ---------------------------------------------------------------------------

fn make_sim_send_error(
    heap: &Heap,
    file: &BytecodeFile,
    result_type: u32,
    is_down: bool,
    alloc_msg: String,
) -> Result<Value, String> {
    let err_ty = file.types.get(result_type).cases[1].fields[0].ty;
    let err_tag = if is_down { 0 } else { 1 };
    let err_val = heap
        .make_case(&file.types, err_ty, err_tag, Vec::new())
        .map_err(|_| alloc_msg.clone())?;
    heap.make_case(&file.types, result_type, 1, vec![err_val])
        .map_err(|_| alloc_msg)
}

pub(crate) fn exec_sim(
    prog: &ProgramView<'_>,
    caller_heap: &Heap,
    sim_op: SimOpId,
    descriptor: u32,
    tool: u32,
    result_type: u32,
    args: &[Value],
) -> Result<Value, String> {
    let file = prog.file;
    if sim_op == SimOpId::Start {
        let start_time = args[0].as_time();
        let Ok(store) = SimStore::open() else {
            return Err("Simulation.start: MO_LIMIT_STORES (8) or store memory limit".to_string());
        };
        let mut inner = SimulationInner {
            descriptor,
            store,
            loop_state: None,
            time: ServiceTime {
                now: start_time,
                elapsed: 0,
            },
        };
        inner.open_and_drain(prog, SimOpId::Start)?;
        let handle = SimHandle::Simulation(RefCell::new(inner));
        return caller_heap
            .alloc_capability(Capability::Simulation, result_type, Rc::new(handle))
            .map_err(|_| "Simulation.start: handle allocation failed".to_string());
    }

    #[expect(
        clippy::expect_used,
        reason = "verified bytecode guarantees Simulation capability operand"
    )]
    let handle = args[0]
        .as_capability()
        .inner
        .downcast_ref::<SimHandle>()
        .expect("Simulation capability");

    match handle {
        SimHandle::LiveActor(cell) => {
            debug_assert_eq!(sim_op, SimOpId::Send);
            let mut actor = cell.borrow_mut();
            let service = &file.services[descriptor as usize];
            let handler = &service.tools[tool as usize];
            let tool_name = file.string(handler.name);
            if descriptor != actor.descriptor {
                return Err("Actor call selected mismatched actor descriptor".to_string());
            }

            let is_running = actor.loop_state.as_ref().is_some_and(|l| l.running);
            if !is_running {
                return make_sim_send_error(
                    caller_heap,
                    file,
                    result_type,
                    true,
                    "Actor call: allocation failed while returning error".to_string(),
                );
            }

            // Drain any pending messages before admitting new call.
            loop {
                let now = actor.clock();
                let stepped = actor
                    .loop_state
                    .as_mut()
                    .and_then(|lp| lp.step(prog, None, now));
                if stepped.is_none() {
                    break;
                }
            }
            if !actor.loop_state.as_ref().is_some_and(|l| l.running) {
                return make_sim_send_error(
                    caller_heap,
                    file,
                    result_type,
                    true,
                    "Actor call: allocation failed while returning error".to_string(),
                );
            }

            let tool_args = &args[1..];
            let mut copied = Vec::with_capacity(handler.argument_types.len());
            let mut message_bytes: u64 = LOG_RECORD_HEADER_BYTES as u64;
            {
                let Some(lp) = actor.loop_state.as_ref() else {
                    return make_sim_send_error(
                        caller_heap,
                        file,
                        result_type,
                        true,
                        "Actor call: allocation failed while returning error".to_string(),
                    );
                };
                for (i, &arg_ty) in handler.argument_types.iter().enumerate() {
                    match copy_saved(&file.types, &lp.heap, arg_ty, &tool_args[i]) {
                        Ok((v, enc_len)) => {
                            copied.push(v);
                            message_bytes += 4 + enc_len as u64;
                        }
                        Err(e) => {
                            let aname = file.string(handler.argument_names[i]);
                            return Err(format!(
                                "Actor argument {aname} copy-in: {}; nothing was queued or committed",
                                e.refusal_message()
                            ));
                        }
                    }
                }
            }
            if !handler.read_only && message_bytes > MO_LIMIT_SAVED_BYTES {
                return Err(format!(
                    "Actor admission: aggregate message is {message_bytes} bytes, exceeding MO_LIMIT_SAVED_BYTES ({MO_LIMIT_SAVED_BYTES})"
                ));
            }
            let now = actor.clock();
            let Some(lp) = actor.loop_state.as_mut() else {
                return make_sim_send_error(
                    caller_heap,
                    file,
                    result_type,
                    true,
                    "Actor call: allocation failed while returning error".to_string(),
                );
            };
            let enq_status = lp.enqueue(file, false, tool, &copied, now);
            drop(copied);
            if enq_status == ServiceStatus::Memory {
                return Err("Actor admission refused; mailbox or memory full".to_string());
            }
            if enq_status != ServiceStatus::Ok {
                return match enq_status {
                    ServiceStatus::Down => make_sim_send_error(
                        caller_heap,
                        file,
                        result_type,
                        true,
                        "Actor call: allocation failed while returning error".to_string(),
                    ),
                    other => Err(format!("Actor call: {}", other.text())),
                };
            }

            let mut reply_opt: Option<StepReply> = None;
            loop {
                let now = actor.clock();
                let Some((r, _, _)) = actor
                    .loop_state
                    .as_mut()
                    .and_then(|lp| lp.step(prog, None, now))
                else {
                    break;
                };
                if !r.timer {
                    reply_opt = Some(r);
                }
            }

            let Some(reply) = reply_opt else {
                return make_sim_send_error(
                    caller_heap,
                    file,
                    result_type,
                    true,
                    "Actor call: allocation failed while returning error".to_string(),
                );
            };
            match reply.status {
                ServiceStatus::Ok => {
                    let Some(raw) = reply.value.as_ref() else {
                        return Err("Actor call: ok reply missing value".to_string());
                    };
                    let (copied_reply, _) =
                        copy_saved(&file.types, caller_heap, reply.result_type, raw).map_err(
                            |e| {
                                format!(
                                    "Actor reply copy-out: {}; call {tool_name} executed",
                                    e.refusal_message()
                                )
                            },
                        )?;
                    caller_heap
                        .make_case(&file.types, result_type, 0, vec![copied_reply])
                        .map_err(|_| "Actor reply copy-out: allocation failed".to_string())
                }
                ServiceStatus::Down => make_sim_send_error(
                    caller_heap,
                    file,
                    result_type,
                    true,
                    "Actor call: allocation failed while returning error".to_string(),
                ),
                ServiceStatus::Defect | ServiceStatus::Work | ServiceStatus::Memory => {
                    make_sim_send_error(
                        caller_heap,
                        file,
                        result_type,
                        false,
                        "Actor call: allocation failed while returning error".to_string(),
                    )
                }
                other => Err(format!("Actor call: {}", other.text())),
            }
        }

        SimHandle::Simulation(cell) => {
            let mut sim = cell.borrow_mut();
            match sim_op {
                SimOpId::Start => unreachable!(),
                SimOpId::Send => {
                    let service = &file.services[descriptor as usize];
                    let handler = &service.tools[tool as usize];
                    let tool_name = file.string(handler.name);
                    if descriptor != sim.descriptor {
                        let sel_mod = file.string(file.modules[service.module as usize].name);
                        let act_mod = file.string(
                            file.modules[file.services[sim.descriptor as usize].module as usize]
                                .name,
                        );
                        return Err(format!(
                            "Simulation.send selected {sel_mod}.{tool_name}, but this simulation runs {act_mod}; nothing was queued or committed"
                        ));
                    }
                    if !sim.is_running() {
                        return make_sim_send_error(
                            caller_heap,
                            file,
                            result_type,
                            true,
                            "Simulation.send: allocation failed while returning ServiceDown; nothing was queued or committed".to_string(),
                        );
                    }
                    sim.drain_all(prog, SimOpId::Send, None)?;

                    let tool_args = &args[1..];
                    let mut copied = Vec::with_capacity(handler.argument_types.len());
                    let mut message_bytes: u64 = LOG_RECORD_HEADER_BYTES as u64;
                    {
                        let Some(lp) = sim.loop_state.as_ref() else {
                            return make_sim_send_error(
                                caller_heap,
                                file,
                                result_type,
                                true,
                                "Simulation.send: allocation failed while returning ServiceDown; nothing was queued or committed".to_string(),
                            );
                        };
                        for (i, &arg_ty) in handler.argument_types.iter().enumerate() {
                            match copy_saved(&file.types, &lp.heap, arg_ty, &tool_args[i]) {
                                Ok((v, enc_len)) => {
                                    copied.push(v);
                                    message_bytes += 4 + enc_len as u64;
                                }
                                Err(e) => {
                                    let aname = file.string(handler.argument_names[i]);
                                    return Err(format!(
                                        "Simulation argument {aname} copy-in: {}; nothing was queued or committed",
                                        e.refusal_message()
                                    ));
                                }
                            }
                        }
                    }
                    if !handler.read_only && message_bytes > MO_LIMIT_SAVED_BYTES {
                        return Err(format!(
                            "Simulation admission: aggregate saved message is {message_bytes} bytes, exceeding MO_LIMIT_SAVED_BYTES ({MO_LIMIT_SAVED_BYTES}); nothing was queued or committed; no handler ran"
                        ));
                    }
                    let sim_time = sim.time;
                    let Some(lp) = sim.loop_state.as_mut() else {
                        return make_sim_send_error(
                            caller_heap,
                            file,
                            result_type,
                            true,
                            "Simulation.send: allocation failed while returning ServiceDown; nothing was queued or committed".to_string(),
                        );
                    };
                    let enq_status = lp.enqueue(file, true, tool, &copied, sim_time);
                    drop(copied);
                    if enq_status == ServiceStatus::Memory {
                        return Err(
                            "Simulation admission refused; nothing was queued or committed; no handler ran".to_string(),
                        );
                    }
                    if enq_status != ServiceStatus::Ok {
                        return match enq_status {
                            ServiceStatus::Down => make_sim_send_error(
                                caller_heap,
                                file,
                                result_type,
                                true,
                                "Simulation.send: allocation failed while returning ServiceDown; nothing was queued or committed".to_string(),
                            ),
                            other => Err(format!("Simulation.send: {}", other.text())),
                        };
                    }

                    let Some((reply, copied_reply)) =
                        sim.drain_all(prog, SimOpId::Send, Some(caller_heap))?
                    else {
                        return Err("Simulation.send: tool call produced no reply".to_string());
                    };
                    match reply.status {
                        ServiceStatus::Ok => {
                            let Some(val) = copied_reply else {
                                return Err("Simulation.send: ok reply missing value".to_string());
                            };
                            let upd = if reply.updated {
                                "; its update was committed"
                            } else {
                                ""
                            };
                            caller_heap
                                .make_case(&file.types, result_type, 0, vec![val])
                                .map_err(|_| {
                                    format!(
                                        "Simulation reply copy-out: allocation; tool {tool_name} executed{upd}; no rollback or retry"
                                    )
                                })
                        }
                        ServiceStatus::Down => make_sim_send_error(
                            caller_heap,
                            file,
                            result_type,
                            true,
                            "Simulation.send: allocation failed while returning ServiceDown; nothing was queued or committed".to_string(),
                        ),
                        ServiceStatus::Defect | ServiceStatus::Work | ServiceStatus::Memory => {
                            make_sim_send_error(
                                caller_heap,
                                file,
                                result_type,
                                false,
                                format!(
                                    "Simulation.send: allocation failed while returning ToolFailed; tool {tool_name} failed and nothing was committed"
                                ),
                            )
                        }
                        other => Err(format!("Simulation.send: {}", other.text())),
                    }
                }
                SimOpId::Advance => {
                    let by = args[1].as_duration();
                    if by < 0
                        || i128::from(sim.time.now) + i128::from(by) > i128::from(MO_TIME_MAX_US)
                    {
                        return Err(
                            "Simulation.advance needs a nonnegative duration ending no later than year 9999".to_string(),
                        );
                    }
                    let until = sim.time.elapsed.saturating_add(by as u64);
                    let target = sim.time.now + by;
                    if sim.is_running() {
                        sim.drain_all(prog, SimOpId::Advance, None)?;
                        let (firings, _) = sim
                            .loop_state
                            .as_ref()
                            .map_or((0, 0), |lp| lp.deadlines(until));
                        if firings > MO_LIMIT_SIMULATION_TICKS {
                            return Err(format!(
                                "Simulation.advance: {firings} firings exceed MO_LIMIT_SIMULATION_TICKS ({MO_LIMIT_SIMULATION_TICKS}); no handler ran, the clock did not move and nothing was committed"
                            ));
                        }
                        loop {
                            let (count, next) = sim
                                .loop_state
                                .as_ref()
                                .map_or((0, 0), |lp| lp.deadlines(until));
                            if count == 0 {
                                break;
                            }
                            debug_assert!(next >= sim.time.elapsed);
                            sim.time.now += (next - sim.time.elapsed) as i64;
                            sim.time.elapsed = next;
                            sim.drain_all(prog, SimOpId::Advance, None)?;
                        }
                    }
                    sim.time = ServiceTime {
                        now: target,
                        elapsed: until,
                    };
                    Ok(Value::Unit)
                }
                SimOpId::Crash => {
                    sim.loop_state = None;
                    Ok(Value::Unit)
                }
                SimOpId::Restart => {
                    if sim.is_running() {
                        return Err(
                            "Simulation.restart needs a crashed or stopped service".to_string()
                        );
                    }
                    sim.open_and_drain(prog, SimOpId::Restart)?;
                    Ok(Value::Unit)
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mo_bytecode::types::TypeTable;
    use mo_bytecode::{
        AnchorExpectation, BYTECODE_FLAGS_SERVICES, BYTECODE_MAGIC, BYTECODE_MAJOR, BYTECODE_MINOR,
        ConstantDef, ConstantValue, DebugEntry, FunctionDef, FunctionKind, Header, LayoutDef,
        LayoutKind, ModuleDef, OPCODE_VARIABLE_SHIFT, ParameterDef, ServiceTimer, ServiceTool,
        TypeCase, TypeDef, TypeField,
    };
    use mo_def::Opcode;

    fn insn(op: Opcode, var_count: u32, operands: &[u32]) -> Vec<u32> {
        let mut words = Vec::with_capacity(1 + operands.len());
        words.push((var_count << OPCODE_VARIABLE_SHIFT) | u32::from(op.number()));
        words.extend_from_slice(operands);
        words
    }

    fn base_type(kind: TypeKind) -> TypeDef {
        TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        }
    }

    /// Builds a synthetic service with:
    /// - state: Int (starts at 10)
    /// - tool 0 `add(delta: Int) -> (Int, Int)` (updating)
    /// - tool 1 `get() -> Int` (read-only)
    /// - tool 2 `boom() -> (Int, Int)` (stops with defect — tests supervisor)
    /// - timer 0 `tick` every 1_000_000 us (adds 1 to state)
    fn make_service_image() -> Image {
        // Types:
        // 0: Int
        // 1: Time
        // 2: Duration
        // 3: Unit
        // 4: Text
        // 5: Tuple(Int, Int) (layout 0)
        // 6: SimulationError (Choice: ServiceDown, ToolFailed)
        // 7: Result((Int, Int), SimulationError)
        // 8: Result(Int, SimulationError)
        // 9: Capability(Simulation)
        let mut tup = base_type(TypeKind::Tuple);
        tup.parameters = vec![0, 0];

        let mut sim_err = base_type(TypeKind::Choice);
        sim_err.name = Some("SimulationError".into());
        sim_err.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "ServiceDown".into(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "ToolFailed".into(),
                fields: vec![],
                unnamed: false,
            },
        ];

        let make_res = |ok_ty: u32| {
            let mut r = base_type(TypeKind::Choice);
            r.name = Some("Result".into());
            r.has_fields = true;
            r.cases = vec![
                TypeCase {
                    name_idx: 0,
                    name: "Ok".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: ok_ty,
                    }],
                    unnamed: true,
                },
                TypeCase {
                    name_idx: 1,
                    name: "Error".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: 6,
                    }],
                    unnamed: true,
                },
            ];
            r
        };

        let mut sim_cap = base_type(TypeKind::Capability);
        sim_cap.capability = Some(Capability::Simulation);

        let types = TypeTable::finish(
            vec![
                base_type(TypeKind::Int),      // 0
                base_type(TypeKind::Time),     // 1
                base_type(TypeKind::Duration), // 2
                base_type(TypeKind::Unit),     // 3
                base_type(TypeKind::Text),     // 4
                tup,                           // 5
                sim_err,                       // 6
                make_res(5),                   // 7
                make_res(0),                   // 8
                sim_cap,                       // 9
            ],
            vec![LayoutDef {
                kind: LayoutKind::Tuple,
                ty: 5,
                case_index: 0,
                function: NO_INDEX,
                fields: vec![0, 0],
            }],
        )
        .unwrap();

        // Constants: 0=Int(10), 1=Int(1), 2=Duration(1_000_000), 3=Text("kaboom")
        let constants = vec![
            ConstantDef {
                ty: 0,
                value: ConstantValue::Immediate64(10i64.to_le_bytes()),
            },
            ConstantDef {
                ty: 0,
                value: ConstantValue::Immediate64(1i64.to_le_bytes()),
            },
            ConstantDef {
                ty: 2,
                value: ConstantValue::Immediate64(1_000_000i64.to_le_bytes()),
            },
            ConstantDef {
                ty: 4,
                value: ConstantValue::Text(6),
            },
        ];

        // fn 0: start() -> Int = 10
        let mut c0 = Vec::new();
        c0.extend(insn(Opcode::Const, 0, &[0, 0]));
        c0.extend(insn(Opcode::Ret, 0, &[0]));
        let f_start = FunctionDef {
            name: 1,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![],
            result_type: 0,
            capture_types: vec![],
            register_types: vec![0],
            code: c0,
        };

        // fn 1: add(state: Int, now: Time, delta: Int) -> (Int, Int)
        let mut c1 = Vec::new();
        c1.extend(insn(Opcode::AddInt, 0, &[3, 0, 2]));
        c1.extend(insn(Opcode::Construct, 2, &[4, 0, 3, 3]));
        c1.extend(insn(Opcode::Ret, 0, &[4]));
        let f_add = FunctionDef {
            name: 2,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 1,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 5,
            capture_types: vec![],
            register_types: vec![0, 1, 0, 0, 5],
            code: c1,
        };

        // fn 2: get(state: Int, now: Time) -> Int
        let mut c2 = Vec::new();
        c2.extend(insn(Opcode::Ret, 0, &[0]));
        let f_get = FunctionDef {
            name: 3,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 1,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 0,
            capture_types: vec![],
            register_types: vec![0, 1],
            code: c2,
        };

        // fn 3: boom(state: Int, now: Time) -> (Int, Int) => stop "kaboom"
        let mut c3 = Vec::new();
        c3.extend(insn(Opcode::Const, 0, &[2, 3]));
        c3.extend(insn(Opcode::Stop, 0, &[2]));
        let f_boom = FunctionDef {
            name: 4,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 1,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 5,
            capture_types: vec![],
            register_types: vec![0, 1, 4],
            code: c3,
        };

        // fn 4: tick_interval() -> Duration = 1s
        let mut c4 = Vec::new();
        c4.extend(insn(Opcode::Const, 0, &[0, 2]));
        c4.extend(insn(Opcode::Ret, 0, &[0]));
        let f_interval = FunctionDef {
            name: 5,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![],
            result_type: 2,
            capture_types: vec![],
            register_types: vec![2],
            code: c4,
        };

        // fn 5: tick(state: Int, now: Time) -> Int = state + 1
        let mut c5 = Vec::new();
        c5.extend(insn(Opcode::Const, 0, &[2, 1]));
        c5.extend(insn(Opcode::AddInt, 0, &[3, 0, 2]));
        c5.extend(insn(Opcode::Ret, 0, &[3]));
        let f_tick = FunctionDef {
            name: 5,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 1,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 0,
            capture_types: vec![],
            register_types: vec![0, 1, 0, 0],
            code: c5,
        };

        let functions = vec![f_start, f_add, f_get, f_boom, f_interval, f_tick];
        let debug = vec![
            DebugEntry {
                spans: vec![],
                names: vec![],
                text: NO_INDEX,
                file: NO_INDEX,
                start: 0,
                end: 0,
                subject: NO_INDEX,
                expectation: AnchorExpectation::NotAnchor,
                lines: vec![],
            };
            functions.len()
        ];

        let service = ServiceDef {
            module: 0,
            state_type: 0,
            start: 0,
            token_env: 7,
            tools: vec![
                ServiceTool {
                    name: 2,
                    function: 1,
                    read_only: false,
                    argument_types: vec![0],
                    result_type: 5,
                    description: 0,
                    argument_names: vec![8],
                },
                ServiceTool {
                    name: 3,
                    function: 2,
                    read_only: true,
                    argument_types: vec![],
                    result_type: 0,
                    description: 0,
                    argument_names: vec![],
                },
                ServiceTool {
                    name: 4,
                    function: 3,
                    read_only: false,
                    argument_types: vec![],
                    result_type: 5,
                    description: 0,
                    argument_names: vec![],
                },
            ],
            timers: vec![ServiceTimer {
                function: 5,
                interval: 4,
            }],
            shapes: vec![],
        };

        let file = BytecodeFile {
            header: Header {
                magic: BYTECODE_MAGIC,
                major: BYTECODE_MAJOR,
                minor: BYTECODE_MINOR,
                flags: BYTECODE_FLAGS_SERVICES,
                checksum: 0,
            },
            program_checksum: 0x1234_5678,
            strings: vec![
                "Counter".into(), // 0
                "start".into(),   // 1
                "add".into(),     // 2
                "get".into(),     // 3
                "boom".into(),    // 4
                "tick".into(),    // 5
                "kaboom".into(),  // 6
                "".into(),        // 7
                "delta".into(),   // 8
            ],
            types,
            constants,
            globals: vec![],
            init: vec![],
            code_offsets: vec![0; functions.len()],
            functions,
            modules: vec![ModuleDef {
                name: 0,
                types: vec![],
                functions: vec![0, 1, 2, 3, 4, 5],
                globals: vec![],
            }],
            files: vec![],
            debug,
            services: vec![service],
        };
        Image::init(file, Limits::default()).unwrap()
    }

    #[test]
    fn simulation_lifecycle_timers_durability_crash_restart_and_supervisor() {
        let image = make_service_image();
        let prog = ProgramView::from_image(&image);
        let heap = Heap::new(16 << 20);

        // 1. Start at t = 0: initial state 10 + startup timer tick -> 11.
        let sim = exec_sim(&prog, &heap, SimOpId::Start, 0, 0, 9, &[Value::Time(0)]).unwrap();
        let g0 = exec_sim(
            &prog,
            &heap,
            SimOpId::Send,
            0,
            1,
            8,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        assert_eq!(
            heap.extract_case_field(&image.file.types, 8, &g0, 0, 0)
                .as_int(),
            11
        );

        // 2. Updating tool add(5) -> state becomes 16.
        let a1 = exec_sim(
            &prog,
            &heap,
            SimOpId::Send,
            0,
            0,
            7,
            &[sim.clone(), Value::Int(5)],
        )
        .unwrap();
        let tup = heap.extract_case_field(&image.file.types, 7, &a1, 0, 0);
        assert_eq!(heap.extract_field(&tup, 0).as_int(), 16);

        // 3. Advance by 3 seconds -> 3 timer firings -> state becomes 19.
        exec_sim(
            &prog,
            &heap,
            SimOpId::Advance,
            0,
            0,
            3,
            &[sim.clone(), Value::Duration(3_000_000)],
        )
        .unwrap();
        let g1 = exec_sim(
            &prog,
            &heap,
            SimOpId::Send,
            0,
            1,
            8,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        assert_eq!(
            heap.extract_case_field(&image.file.types, 8, &g1, 0, 0)
                .as_int(),
            19
        );

        // Negative advance duration rejected; restart while running rejected.
        assert!(
            exec_sim(
                &prog,
                &heap,
                SimOpId::Advance,
                0,
                0,
                3,
                &[sim.clone(), Value::Duration(-1)]
            )
            .is_err()
        );
        assert_eq!(
            exec_sim(
                &prog,
                &heap,
                SimOpId::Restart,
                0,
                0,
                3,
                std::slice::from_ref(&sim)
            )
            .err()
            .as_deref(),
            Some("Simulation.restart needs a crashed or stopped service")
        );

        // 4. Crash -> Send returns Error(ServiceDown) (tag 0).
        exec_sim(
            &prog,
            &heap,
            SimOpId::Crash,
            0,
            0,
            3,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        let down = exec_sim(
            &prog,
            &heap,
            SimOpId::Send,
            0,
            1,
            8,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        assert_eq!(down.as_tag(), 1);
        let err_val = heap.extract_case_field(&image.file.types, 8, &down, 1, 0);
        assert_eq!(err_val.as_tag(), 0); // ServiceDown

        // 5. Restart replays snapshot + log (19) and runs startup timer tick (+1 -> 20)!
        exec_sim(
            &prog,
            &heap,
            SimOpId::Restart,
            0,
            0,
            3,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        let g2 = exec_sim(
            &prog,
            &heap,
            SimOpId::Send,
            0,
            1,
            8,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        assert_eq!(
            heap.extract_case_field(&image.file.types, 8, &g2, 0, 0)
                .as_int(),
            20
        );

        // 6. Supervisor: 5 failures in 60s -> restart (runs startup tick +1), 3 restarts in 600s -> stop!
        for _ in 0..15 {
            let r = exec_sim(
                &prog,
                &heap,
                SimOpId::Send,
                0,
                2,
                7,
                std::slice::from_ref(&sim),
            )
            .unwrap();
            assert_eq!(r.as_tag(), 1);
            let e = heap.extract_case_field(&image.file.types, 7, &r, 1, 0);
            assert_eq!(e.as_tag(), 1); // ToolFailed
        }
        // After 15 failures (3 restarts), service is now stopped -> ServiceDown.
        let stopped = exec_sim(
            &prog,
            &heap,
            SimOpId::Send,
            0,
            1,
            8,
            std::slice::from_ref(&sim),
        )
        .unwrap();
        assert_eq!(stopped.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&image.file.types, 8, &stopped, 1, 0)
                .as_tag(),
            0
        );
        drop(sim);

        // 7. MO_LIMIT_STORES (8) limit + reclaim on drop.
        let mut sims = Vec::new();
        for _ in 0..MO_LIMIT_STORES {
            sims.push(exec_sim(&prog, &heap, SimOpId::Start, 0, 0, 9, &[Value::Time(0)]).unwrap());
        }
        assert_eq!(
            exec_sim(&prog, &heap, SimOpId::Start, 0, 0, 9, &[Value::Time(0)])
                .err()
                .as_deref(),
            Some("Simulation.start: MO_LIMIT_STORES (8) or store memory limit")
        );
        sims.clear();
        // Store slots reclaimed after drop.
        let _reopened = exec_sim(&prog, &heap, SimOpId::Start, 0, 0, 9, &[Value::Time(0)]).unwrap();
    }
}
