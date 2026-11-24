//! Service recovery sequence search, replay, and shrinking (`src/tools/recovery.{h,c}`).

use mo_bytecode::types::TypeTable;
use mo_bytecode::{BytecodeFile, CaseShape, NO_INDEX, ServiceTool, TypeKind};
use mo_def::limits::{
    MO_CLI_RECOVERY_POOL, MO_CLI_RECOVERY_RESTARTS, MO_CLI_RECOVERY_SHRINK_RUNS,
    MO_CLI_RECOVERY_STEPS, MO_LIMIT_SAVED_DEPTH, MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES,
    MO_LIMIT_SIMULATION_TICKS,
};
use mo_heap::hash::{inline_payload_to_value, siphash13};
use mo_heap::{Heap, HeapObj, Value};
use mo_vm::simulation::{
    ServiceStatus, Simulation, SimulationFailure, SimulationOutcome, SimulationReply,
    SimulationSent,
};
use mo_vm::{Image, Limits, Outcome, Process};

use crate::inputs::{InputLimits, InputResult, InputShrinker, Inputs, input_random};

/// Every recovery sequence starts at `2026-01-01T00:00:00Z`.
pub const RECOVERY_EPOCH: i64 = 1_767_225_600_000_000;
/// Suffix appended to module names for recovery observations.
pub const RECOVERY_SUFFIX: &str = ".service.recovery";
/// Public fixed SipHash-1-3 key for deterministic recovery sequence seeding.
const RECOVERY_KEY: [u64; 2] = [0x6d6f_2d72_6563_6f76, 0x6572_792d_6368_6563];
const RANDOM_INPUTS: u64 = 256;
const ARGUMENT_TRIES: u32 = 16;
const SECOND_US: i64 = 1_000_000;

/// Kind of step in a recovery sequence (`enum mo_recovery_step_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecoveryStepKind {
    Send,
    Advance,
    Crash,
    Restart,
}

/// Supervisor intervention observed on a `Send` step (`enum mo_recovery_supervisor`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecoverySupervisor {
    None,
    Restarted,
    Stopped,
}

/// One step in a recovery sequence (`struct mo_recovery_step`).
#[derive(Clone)]
pub struct RecoveryStep {
    pub kind: RecoveryStepKind,
    pub tool: u32,
    pub arguments: Vec<Value>,
    pub duration: i64,
    pub sent: SimulationSent,
    pub status: ServiceStatus,
    pub outcome: SimulationOutcome,
    pub supervisor: RecoverySupervisor,
    pub updated: bool,
    pub firings: u64,
    pub failed_firings: u32,
    pub ran: bool,
}

impl RecoveryStep {
    fn new(kind: RecoveryStepKind) -> Self {
        Self {
            kind,
            tool: 0,
            arguments: Vec::new(),
            duration: 0,
            sent: SimulationSent::Down,
            status: ServiceStatus::Down,
            outcome: SimulationOutcome::Ok,
            supervisor: RecoverySupervisor::None,
            updated: false,
            firings: 0,
            failed_firings: 0,
            ran: false,
        }
    }
}

/// A sequence of recovery steps (`struct mo_recovery_sequence`).
#[derive(Clone, Default)]
pub struct RecoverySequence {
    pub steps: Vec<RecoveryStep>,
}

/// Outcome of a recovery search or replay (`enum mo_recovery_outcome`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecoveryOutcome {
    Passed,
    Mismatch,
    Refused,
    Check,
    Stopped,
    Unavailable,
}

/// Comparison result after restart (`enum mo_recovery_compared`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecoveryCompared {
    Equal,
    Different,
    Unreadable,
}

/// Service failure details captured during simulation (`struct mo_recovery_failure`).
#[derive(Debug, Clone)]
pub struct RecoveryFailure {
    pub status: ServiceStatus,
    pub now: i64,
    pub function: u32,
    pub span: Option<(u32, u32, u32)>,
    pub reason: String,
    pub detail: Option<String>,
}

impl Default for RecoveryFailure {
    fn default() -> Self {
        Self {
            status: ServiceStatus::Ok,
            now: 0,
            function: NO_INDEX,
            span: None,
            reason: String::new(),
            detail: None,
        }
    }
}

/// Counters accumulated across recovery sequences (`struct mo_recovery_counts`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RecoveryCounts {
    pub sequences: u64,
    pub restarts: u64,
    pub commits: u64,
    pub replayed: u64,
    pub snapshots: u64,
}

/// Service promise judge verdict (`struct mo_recovery_verdict`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecoveryVerdict {
    pub check: u32,
    pub line: u32,
}

impl Default for RecoveryVerdict {
    fn default() -> Self {
        Self {
            check: NO_INDEX,
            line: 0,
        }
    }
}

/// Result of one recovery search or replay (`struct mo_recovery_result`).
#[derive(Clone)]
pub struct RecoveryResult {
    pub outcome: RecoveryOutcome,
    pub counts: RecoveryCounts,
    pub resume: RecoveryCounts,
    pub ordinal: u64,
    pub sequence: RecoverySequence,
    pub step: u32,
    pub before: Vec<u8>,
    pub after: Vec<u8>,
    pub failure: RecoveryFailure,
    pub verdict: RecoveryVerdict,
    pub problem: Option<&'static str>,
}

impl Default for RecoveryResult {
    fn default() -> Self {
        Self {
            outcome: RecoveryOutcome::Passed,
            counts: RecoveryCounts::default(),
            resume: RecoveryCounts::default(),
            ordinal: 0,
            sequence: RecoverySequence::default(),
            step: NO_INDEX,
            before: Vec::new(),
            after: Vec::new(),
            failure: RecoveryFailure::default(),
            verdict: RecoveryVerdict::default(),
            problem: None,
        }
    }
}

/// Observation handed to recovery hooks after each step (`struct mo_recovery_observation`).
pub struct RecoveryObservation<'a> {
    pub ordinal: u64,
    pub index: u32,
    pub step: &'a RecoveryStep,
    pub reply: Option<&'a SimulationReply>,
    pub now: i64,
    pub before: Option<&'a [u8]>,
    pub after: Option<&'a [u8]>,
}

/// Hooks called during recovery search/replay (`struct mo_recovery_hooks`).
pub trait RecoveryHooks {
    fn observe(&mut self, _obs: &RecoveryObservation<'_>) {}
    fn check(&mut self, _obs: &RecoveryObservation<'_>) -> RecoveryVerdict {
        RecoveryVerdict::default()
    }
    fn has_check(&self) -> bool {
        false
    }
}

#[derive(Clone)]
struct PoolEntry {
    ty: u32,
    group: u32,
    value: Value,
}

/// Deterministic service recovery checker (`struct mo_recovery`).
pub struct Recovery<'a> {
    image: &'a Image,
    index: u32,
    process: Process<'a>,
    name: String,
    seed: u64,
    inputs: Vec<Option<Inputs<'a>>>,
    boundary: Vec<u64>,
    sendable: Vec<u32>,
    intervals: Vec<i64>,
    pool: Vec<PoolEntry>,
    group: u32,
    compare: bool,
}

impl<'a> Recovery<'a> {
    /// Creates a new recovery checker for service `index` (`mo_recovery_new`).
    pub fn new(image: &'a Image, index: u32, limits: Limits) -> Result<Self, InputResult> {
        let file = image.file();
        let service = &file.services[index as usize];
        let mod_name = file.string(file.modules[service.module as usize].name);
        let name = format!("{mod_name}{RECOVERY_SUFFIX}");
        let seed = siphash13(RECOVERY_KEY, name.as_bytes());
        let mut input_limits = InputLimits::default();
        let half_depth = (MO_LIMIT_SAVED_DEPTH / 2) as u32;
        if input_limits.depth > half_depth {
            input_limits.depth = half_depth;
        }
        let mut inputs = Vec::with_capacity(service.tools.len());
        let mut sendable = Vec::new();
        for (i, tool) in service.tools.iter().enumerate() {
            match Inputs::init(
                &file.types,
                &tool.argument_types,
                name.as_bytes(),
                input_limits,
                &[],
            ) {
                Ok(gen_inputs) => {
                    inputs.push(Some(gen_inputs));
                    sendable.push(i as u32);
                }
                Err(InputResult::Uninhabited) => {
                    inputs.push(None);
                }
                Err(err) => return Err(err),
            }
        }
        let mut intervals = Vec::new();
        for timer in &service.timers {
            let mut scratch = Process::new(
                image,
                limits,
                mo_host::HostContext::stdio(Vec::new(), Vec::new()),
            );
            if let Outcome::Returned(every) = scratch.call(timer.interval, Vec::new()) {
                let dur = every.as_duration();
                if dur > 0 {
                    intervals.push(dur);
                }
            }
        }
        let process = Process::new(
            image,
            limits,
            mo_host::HostContext::stdio(Vec::new(), Vec::new()),
        );
        let boundary = vec![0u64; service.tools.len()];
        Ok(Self {
            image,
            index,
            process,
            name,
            seed,
            inputs,
            boundary,
            sendable,
            intervals,
            pool: Vec::new(),
            group: 0,
            compare: true,
        })
    }

    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub fn heap(&self) -> &Heap {
        self.process.heap()
    }

    pub fn set_compare_restarts(&mut self, compare: bool) {
        self.compare = compare;
    }

    fn file(&self) -> &'a BytecodeFile {
        self.image.file()
    }

    fn types(&self) -> &'a TypeTable {
        &self.image.file().types
    }

    /// Extracts the visible reply value and its type from a `SimulationReply` (`mo_recovery_reply`).
    #[must_use]
    pub fn extract_reply(
        &self,
        tool: u32,
        sent: SimulationSent,
        reply: &SimulationReply,
    ) -> (u32, Value) {
        debug_assert!(matches!(
            sent,
            SimulationSent::Replied | SimulationSent::Rejected
        ));
        let mut ty = reply.ty;
        let mut value = reply.reply.clone().unwrap_or(Value::Unit);
        let svc_tool = &self.file().services[self.index as usize].tools[tool as usize];
        if sent == SimulationSent::Replied && !svc_tool.read_only {
            let t = self.types().get(ty);
            if t.kind == TypeKind::Choice {
                debug_assert_eq!(value.as_tag(), 0);
                value = self
                    .heap()
                    .extract_case_field(self.types(), ty, &value, 0, 0);
                ty = t.cases[0].fields[0].ty;
            }
            let tup = self.types().get(ty);
            debug_assert_eq!(tup.kind, TypeKind::Tuple);
            debug_assert_eq!(tup.parameters.len(), 2);
            value = self.heap().extract_field(&value, 1);
            ty = tup.parameters[1];
        }
        (ty, value)
    }

    fn compare_state(sim: &Simulation<'_>, before: &[u8], after: &mut Vec<u8>) -> RecoveryCompared {
        after.clear();
        if !sim.encode(after) {
            return RecoveryCompared::Unreadable;
        }
        if after.as_slice() == before {
            RecoveryCompared::Equal
        } else {
            RecoveryCompared::Different
        }
    }

    fn harvest(&mut self, tool: u32, sent: SimulationSent, reply: &SimulationReply) {
        let (ty, value) = self.extract_reply(tool, sent, reply);
        let found = collect_leaves(self.types(), ty, &value, MO_CLI_RECOVERY_POOL as usize);
        if found.is_empty() {
            return;
        }
        let cap = MO_CLI_RECOVERY_POOL as usize;
        let evict = (self.pool.len() + found.len()).saturating_sub(cap);
        if evict != 0 {
            self.pool.drain(..evict);
        }
        let group = self.group;
        for (leaf_ty, leaf_val) in found {
            self.pool.push(PoolEntry {
                ty: leaf_ty,
                group,
                value: leaf_val,
            });
        }
        self.group += 1;
    }

    fn supply(&self, tool: &ServiceTool, group: u32, arguments: &mut [Value], apply: bool) -> u32 {
        let mut first = 0usize;
        while first < self.pool.len() && self.pool[first].group != group {
            first += 1;
        }
        let mut end = first;
        while end < self.pool.len() && self.pool[end].group == group {
            end += 1;
        }
        let mut used = [false; MO_CLI_RECOVERY_POOL as usize];
        let mut supplied = 0u32;
        for (a, &arg_ty) in tool.argument_types.iter().enumerate() {
            for (i, entry) in self.pool.iter().enumerate().take(end).skip(first) {
                if used[i] || entry.ty != arg_ty {
                    continue;
                }
                used[i] = true;
                supplied += 1;
                if apply {
                    arguments[a] = entry.value.clone();
                }
                break;
            }
        }
        supplied
    }

    fn reuse(&self, tool: &ServiceTool, random: &mut u64, arguments: &mut [Value]) {
        if self.pool.is_empty() || input_random(random).is_multiple_of(4) {
            return;
        }
        let mut best = 0u32;
        let mut candidates = Vec::new();
        let mut end = self.pool.len();
        while end != 0 {
            let group = self.pool[end - 1].group;
            while end != 0 && self.pool[end - 1].group == group {
                end -= 1;
            }
            let supplied = self.supply(tool, group, arguments, false);
            if supplied > best {
                best = supplied;
                candidates.clear();
            }
            if supplied == best && supplied != 0 {
                candidates.push(group);
            }
        }
        if candidates.is_empty() {
            return;
        }
        let pick = input_random(random);
        let group = if pick.is_multiple_of(2) {
            candidates[0]
        } else {
            candidates[((pick / 2) % candidates.len() as u64) as usize]
        };
        let _ = self.supply(tool, group, arguments, true);
    }

    fn generate_arguments(
        &mut self,
        tool: u32,
        random: &mut u64,
    ) -> Result<Vec<Value>, InputResult> {
        let Some(ref mut inputs) = self.inputs[tool as usize] else {
            return Err(InputResult::Uninhabited);
        };
        let mut last_err = InputResult::Limit;
        for _ in 0..ARGUMENT_TRIES {
            let b_count = inputs.boundaries();
            if self.boundary[tool as usize] < b_count {
                let ord = self.boundary[tool as usize];
                self.boundary[tool as usize] += 1;
                inputs.seek(ord);
            } else {
                inputs.seek(b_count + input_random(random) % RANDOM_INPUTS);
            }
            match inputs.next(self.process.heap(), &mut []) {
                Ok((vals, _)) => return Ok(vals),
                Err(InputResult::Limit) => {
                    last_err = InputResult::Limit;
                }
                Err(err) => return Err(err),
            }
        }
        Err(last_err)
    }

    fn advance_by(&self, sim: &Simulation<'_>, random: &mut u64) -> i64 {
        let mut by = if !self.intervals.is_empty() && !input_random(random).is_multiple_of(4) {
            let every =
                self.intervals[(input_random(random) % self.intervals.len() as u64) as usize];
            match input_random(random) % 6 {
                0 => every,
                1 => every - 1,
                2 => every.saturating_add(1),
                3 => mul_clamp(every, 2 + input_random(random) % 3),
                4 => mul_clamp(
                    every,
                    MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES
                        + input_random(random) % (MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 1),
                ),
                _ => {
                    let bound = mul_clamp(every, 2) as u64;
                    (input_random(random) % (bound + 1)) as i64
                }
            }
        } else {
            const SPANS: [i64; 3] = [SECOND_US, 3600 * SECOND_US, 86_400 * SECOND_US];
            let span = SPANS[(input_random(random) % 3) as usize];
            1 + (input_random(random) % span as u64) as i64
        };
        while by > 0 {
            match sim.firings(by) {
                Some(firings) if firings <= MO_LIMIT_SIMULATION_TICKS => break,
                _ => by /= 2,
            }
        }
        by
    }

    /// Searches up to `sequences` random sequences starting from `first` (`mo_recovery_search`).
    pub fn search<'h>(
        &mut self,
        first: u64,
        sequences: u64,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> RecoveryResult {
        let mut out = RecoveryResult::default();
        let mut outcome = RecoveryOutcome::Passed;

        for ordinal in first..sequences {
            out.resume = out.counts;
            out.ordinal = ordinal;
            let mut trial = TrialState::new(ordinal, true);
            outcome = self.run_trial_generate(&mut trial, &mut out, hooks.as_deref_mut());
            out.counts.commits += trial.net_commits();
            self.pool.clear();
            if outcome != RecoveryOutcome::Passed {
                break;
            }
            out.counts.sequences += 1;
            out.sequence.steps.clear();
        }
        if !matches!(
            outcome,
            RecoveryOutcome::Mismatch | RecoveryOutcome::Refused | RecoveryOutcome::Check
        ) {
            out.sequence.steps.clear();
        }
        out.outcome = outcome;
        out
    }

    /// Replays a fixed sequence against a fresh simulation (`mo_recovery_replay`).
    pub fn replay<'h>(
        &mut self,
        sequence: &mut RecoverySequence,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> RecoveryResult {
        debug_assert!(sequence.steps.len() <= MO_CLI_RECOVERY_STEPS as usize);
        for step in &mut sequence.steps {
            step.ran = false;
        }
        let mut out = RecoveryResult::default();
        let mut trial = TrialState::new(0, false);
        let mut sim = match Simulation::new(self.image, self.index, RECOVERY_EPOCH) {
            Some(s) => s,
            None => {
                out.problem = Some("an in-memory store could not be opened");
                out.outcome = RecoveryOutcome::Unavailable;
                return out;
            }
        };
        let mut outcome = self.start_sim(&mut sim, &mut trial, &mut out);
        for i in 0..sequence.steps.len() {
            if outcome != RecoveryOutcome::Passed || trial.ended {
                break;
            }
            outcome = self.run_step(
                &mut sim,
                &mut trial,
                &mut out,
                &mut sequence.steps[i],
                i as u32,
                hooks.as_deref_mut(),
            );
        }
        out.counts.commits += trial.net_commits();
        self.pool.clear();
        if outcome == RecoveryOutcome::Passed {
            out.counts.sequences = 1;
        }
        out.outcome = outcome;
        out
    }

    /// Shrinks a failing sequence in place (`mo_recovery_shrink`).
    pub fn shrink<'h>(
        &mut self,
        sequence: &mut RecoverySequence,
        failure: RecoveryOutcome,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
        verdict: RecoveryVerdict,
    ) -> u32 {
        let mut state = ShrinkState {
            failure,
            verdict,
            runs: 0,
            ended: false,
        };
        loop {
            if state.ended {
                break;
            }
            let mut progress = self.delete_steps(&mut state, sequence, hooks.as_deref_mut());
            progress |= self.shrink_arguments(&mut state, sequence, hooks.as_deref_mut());
            progress |= self.shorten_advances(&mut state, sequence, hooks.as_deref_mut());
            if !progress {
                break;
            }
        }
        state.runs
    }

    fn start_sim(
        &self,
        sim: &mut Simulation<'a>,
        trial: &mut TrialState,
        out: &mut RecoveryResult,
    ) -> RecoveryOutcome {
        let service = &self.file().services[self.index as usize];
        trial.failed = false;
        let open_out = sim.open(&mut |f| {
            trial.record_failure(service, f);
            true
        });
        if open_out == SimulationOutcome::Down {
            out.failure = trial.failure.clone();
            return RecoveryOutcome::Stopped;
        }
        let drained = sim.drain(&mut |f| {
            trial.record_failure(service, f);
            true
        });
        debug_assert_eq!(drained, SimulationOutcome::Ok);
        trial.committed += service.timers.len() as u64;
        RecoveryOutcome::Passed
    }

    fn run_trial_generate<'h>(
        &mut self,
        trial: &mut TrialState,
        out: &mut RecoveryResult,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> RecoveryOutcome {
        let mut sim = match Simulation::new(self.image, self.index, RECOVERY_EPOCH) {
            Some(s) => s,
            None => {
                out.problem = Some("an in-memory store could not be opened");
                return RecoveryOutcome::Unavailable;
            }
        };
        let open_outcome = self.start_sim(&mut sim, trial, out);
        if open_outcome != RecoveryOutcome::Passed {
            return open_outcome;
        }
        let mut bytes = [0u8; 16];
        bytes[..8].copy_from_slice(&self.seed.to_le_bytes());
        bytes[8..].copy_from_slice(&trial.ordinal.to_le_bytes());
        let mut random = siphash13(RECOVERY_KEY, &bytes);
        let length = if input_random(&mut random).is_multiple_of(4) {
            2 + (input_random(&mut random) % 7) as usize
        } else {
            9 + (input_random(&mut random) % (MO_CLI_RECOVERY_STEPS - 8)) as usize
        };
        let mut restarts = 0u64;
        loop {
            let end = length.saturating_sub(out.sequence.steps.len()) <= 3;
            if end || !sim.running() || input_random(&mut random).is_multiple_of(10) {
                let mut crash_step = RecoveryStep::new(RecoveryStepKind::Crash);
                let crash_idx = out.sequence.steps.len() as u32;
                let mut outcome = self.run_step(
                    &mut sim,
                    trial,
                    out,
                    &mut crash_step,
                    crash_idx,
                    hooks.as_deref_mut(),
                );
                out.sequence.steps.push(crash_step);
                if outcome == RecoveryOutcome::Passed {
                    let mut restart_step = RecoveryStep::new(RecoveryStepKind::Restart);
                    let restart_idx = out.sequence.steps.len() as u32;
                    outcome = self.run_step(
                        &mut sim,
                        trial,
                        out,
                        &mut restart_step,
                        restart_idx,
                        hooks.as_deref_mut(),
                    );
                    out.sequence.steps.push(restart_step);
                }
                if outcome != RecoveryOutcome::Passed {
                    return outcome;
                }
                restarts += 1;
                if end || trial.ended || restarts == MO_CLI_RECOVERY_RESTARTS {
                    return RecoveryOutcome::Passed;
                }
                continue;
            }
            let mut step = if self.sendable.is_empty()
                || input_random(&mut random).is_multiple_of(6)
            {
                let mut s = RecoveryStep::new(RecoveryStepKind::Advance);
                s.duration = self.advance_by(&sim, &mut random);
                s
            } else {
                let mut s = RecoveryStep::new(RecoveryStepKind::Send);
                s.tool = self.sendable
                    [(input_random(&mut random) % self.sendable.len() as u64) as usize];
                let arg_count = self.file().services[self.index as usize].tools[s.tool as usize]
                    .argument_types
                    .len();
                if arg_count != 0 {
                    match self.generate_arguments(s.tool, &mut random) {
                        Ok(mut args) => {
                            let tool_def =
                                &self.file().services[self.index as usize].tools[s.tool as usize];
                            self.reuse(tool_def, &mut random, &mut args);
                            s.arguments = args;
                        }
                        Err(_) => {
                            out.problem = Some("tool arguments could not be generated");
                            return RecoveryOutcome::Unavailable;
                        }
                    }
                }
                s
            };
            let idx = out.sequence.steps.len() as u32;
            let outcome = self.run_step(&mut sim, trial, out, &mut step, idx, hooks.as_deref_mut());
            out.sequence.steps.push(step);
            if outcome != RecoveryOutcome::Passed {
                return outcome;
            }
        }
    }

    fn run_step<'h>(
        &mut self,
        sim: &mut Simulation<'a>,
        trial: &mut TrialState,
        out: &mut RecoveryResult,
        step: &mut RecoveryStep,
        index: u32,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> RecoveryOutcome {
        let service = &self.file().services[self.index as usize];
        let checking = hooks.as_deref().is_some_and(RecoveryHooks::has_check);
        if checking
            && !trial.have_pre
            && !matches!(
                step.kind,
                RecoveryStepKind::Crash | RecoveryStepKind::Restart
            )
        {
            trial.pre.clear();
            trial.have_pre = sim.encode(&mut trial.pre);
        }
        trial.failed_firings = 0;
        let mut outcome = RecoveryOutcome::Passed;
        let mut reply_opt: Option<SimulationReply> = None;
        let mut restarted = false;

        match step.kind {
            RecoveryStepKind::Send => {
                let running = sim.running();
                let prev_restarts = sim.restarts();
                let (sent, reply) = sim.send(
                    &mut |f| {
                        trial.record_failure(service, f);
                        true
                    },
                    self.process.heap(),
                    step.tool,
                    &step.arguments,
                );
                debug_assert_ne!(sent, SimulationSent::Stopped);
                step.sent = sent;
                step.status = reply.status;
                step.updated = reply.updated;
                step.supervisor = if running && !sim.running() {
                    RecoverySupervisor::Stopped
                } else if sim.restarts() != prev_restarts {
                    RecoverySupervisor::Restarted
                } else {
                    RecoverySupervisor::None
                };
                if reply.updated {
                    trial.committed += 1;
                }
                if matches!(sent, SimulationSent::Replied | SimulationSent::Rejected) {
                    reply_opt = Some(reply);
                }
            }
            RecoveryStepKind::Advance => {
                let (adv_out, firings) = sim.advance(
                    &mut |f| {
                        trial.record_failure(service, f);
                        true
                    },
                    step.duration,
                );
                debug_assert_ne!(adv_out, SimulationOutcome::Stopped);
                step.outcome = adv_out;
                step.firings = firings;
                if adv_out == SimulationOutcome::Ok {
                    trial.committed += firings;
                }
                step.failed_firings = trial.failed_firings;
            }
            RecoveryStepKind::Crash => {
                out.before.clear();
                if !sim.encode(&mut out.before) {
                    out.problem = Some("the state before a crash could not be encoded");
                    outcome = RecoveryOutcome::Unavailable;
                }
                sim.crash();
                trial.have_pre = false;
            }
            RecoveryStepKind::Restart => {
                trial.failed = false;
                step.outcome = sim.open(&mut |f| {
                    trial.record_failure(service, f);
                    true
                });
                if step.outcome == SimulationOutcome::Down {
                    out.failure = trial.failure.clone();
                    if self.compare {
                        outcome = RecoveryOutcome::Refused;
                    } else {
                        trial.ended = true;
                    }
                } else if !self.compare {
                    restarted = true;
                } else {
                    match Self::compare_state(sim, &out.before, &mut out.after) {
                        RecoveryCompared::Equal => {
                            restarted = true;
                            out.counts.restarts += 1;
                            let net = trial.net_commits();
                            out.counts.replayed += net % MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES;
                            if net >= MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES {
                                out.counts.snapshots += 1;
                            }
                        }
                        RecoveryCompared::Different => {
                            outcome = RecoveryOutcome::Mismatch;
                        }
                        RecoveryCompared::Unreadable => {
                            out.problem = Some("the recovered state could not be encoded");
                            outcome = RecoveryOutcome::Unavailable;
                        }
                    }
                }
            }
        }
        step.ran = true;
        if let Some(ref mut h) = hooks {
            let obs = RecoveryObservation {
                ordinal: trial.ordinal,
                index,
                step,
                reply: reply_opt.as_ref(),
                now: sim.now(),
                before: None,
                after: None,
            };
            h.observe(&obs);
        }
        if restarted {
            let drained = sim.drain(&mut |f| {
                trial.record_failure(service, f);
                true
            });
            debug_assert_eq!(drained, SimulationOutcome::Ok);
            trial.committed += service.timers.len() as u64;
        }
        if checking
            && outcome == RecoveryOutcome::Passed
            && !trial.ended
            && step.kind != RecoveryStepKind::Crash
            && let Some(ref mut h) = hooks
        {
            let restart = step.kind == RecoveryStepKind::Restart;
            trial.post.clear();
            let after_ok = sim.encode(&mut trial.post);
            let before_slice: Option<&[u8]> = if restart {
                Some(out.before.as_slice())
            } else if trial.have_pre {
                Some(trial.pre.as_slice())
            } else {
                None
            };
            let obs = RecoveryObservation {
                ordinal: trial.ordinal,
                index,
                step,
                reply: reply_opt.as_ref(),
                now: sim.now(),
                before: before_slice,
                after: after_ok.then_some(trial.post.as_slice()),
            };
            let verdict = h.check(&obs);
            if verdict.check != NO_INDEX {
                out.verdict = verdict;
                if !restart {
                    std::mem::swap(&mut out.before, &mut trial.pre);
                }
                std::mem::swap(&mut out.after, &mut trial.post);
                outcome = RecoveryOutcome::Check;
            } else {
                std::mem::swap(&mut trial.pre, &mut trial.post);
                trial.have_pre = after_ok;
            }
        }
        if let Some(ref reply) = reply_opt
            && trial.harvest
        {
            self.harvest(step.tool, step.sent, reply);
        }
        if matches!(
            outcome,
            RecoveryOutcome::Mismatch | RecoveryOutcome::Refused | RecoveryOutcome::Check
        ) {
            out.step = index;
        }
        outcome
    }

    fn attempt_shrink<'h>(
        &mut self,
        state: &mut ShrinkState,
        best: &mut RecoverySequence,
        mut candidate: RecoverySequence,
        hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> bool {
        if state.runs as u64 == MO_CLI_RECOVERY_SHRINK_RUNS {
            state.ended = true;
            return false;
        }
        state.runs += 1;
        let res = self.replay(&mut candidate, hooks);
        let fails = res.outcome == state.failure
            && res.verdict.check == state.verdict.check
            && res.verdict.line == state.verdict.line;
        if fails {
            let cut_len = (res.step as usize) + 1;
            debug_assert!(cut_len <= candidate.steps.len());
            candidate.steps.truncate(cut_len);
            *best = candidate;
        }
        fails
    }

    fn delete_steps<'h>(
        &mut self,
        state: &mut ShrinkState,
        best: &mut RecoverySequence,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> bool {
        let mut progress = false;
        let mut size = best.steps.len() / 2;
        while size != 0 && !state.ended {
            let mut i = 0usize;
            while i < best.steps.len() && !state.ended {
                let mut end = i;
                while end < best.steps.len() && end - i < size {
                    end += step_unit(best, end);
                }
                let candidate = copy_without(best, i, end);
                if self.attempt_shrink(state, best, candidate, hooks.as_deref_mut()) {
                    progress = true;
                } else {
                    i += step_unit(best, i);
                }
            }
            size /= 2;
        }
        progress
    }

    fn shrink_arguments<'h>(
        &mut self,
        state: &mut ShrinkState,
        best: &mut RecoverySequence,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> bool {
        let mut progress = false;
        let mut i = 0usize;
        while i < best.steps.len() && !state.ended {
            if best.steps[i].kind != RecoveryStepKind::Send {
                i += 1;
                continue;
            }
            let tool = best.steps[i].tool;
            let arg_count = self.file().services[self.index as usize].tools[tool as usize]
                .argument_types
                .len();
            if arg_count == 0 {
                i += 1;
                continue;
            }
            let mut shrinker = InputShrinker::new(MO_CLI_RECOVERY_SHRINK_RUNS as u32);
            while !state.ended && i < best.steps.len() {
                let next_args = {
                    let Some(ref inputs) = self.inputs[tool as usize] else {
                        break;
                    };
                    match shrinker.next(
                        inputs,
                        &best.steps[i].arguments,
                        self.process.heap(),
                        &mut [],
                    ) {
                        Ok(args) => args,
                        Err(_) => break,
                    }
                };
                let mut candidate = best.clone();
                candidate.steps[i].arguments = next_args;
                let fails = self.attempt_shrink(state, best, candidate, hooks.as_deref_mut());
                shrinker.feedback(fails);
                progress |= fails;
            }
            i += 1;
        }
        progress
    }

    fn shorten_advances<'h>(
        &mut self,
        state: &mut ShrinkState,
        best: &mut RecoverySequence,
        mut hooks: Option<&mut (dyn RecoveryHooks + 'h)>,
    ) -> bool {
        let mut progress = false;
        let mut i = 0usize;
        while i < best.steps.len() && !state.ended {
            while !state.ended
                && i < best.steps.len()
                && best.steps[i].kind == RecoveryStepKind::Advance
                && best.steps[i].duration != 0
            {
                let mut candidate = best.clone();
                candidate.steps[i].duration /= 2;
                if !self.attempt_shrink(state, best, candidate, hooks.as_deref_mut()) {
                    break;
                }
                progress = true;
            }
            i += 1;
        }
        progress
    }
}

struct TrialState {
    ordinal: u64,
    harvest: bool,
    failed: bool,
    failure: RecoveryFailure,
    failed_firings: u32,
    committed: u64,
    lost: u64,
    ended: bool,
    pre: Vec<u8>,
    post: Vec<u8>,
    have_pre: bool,
}

impl TrialState {
    fn new(ordinal: u64, harvest: bool) -> Self {
        Self {
            ordinal,
            harvest,
            failed: false,
            failure: RecoveryFailure::default(),
            failed_firings: 0,
            committed: 0,
            lost: 0,
            ended: false,
            pre: Vec::new(),
            post: Vec::new(),
            have_pre: false,
        }
    }

    fn net_commits(&self) -> u64 {
        self.committed.saturating_sub(self.lost)
    }

    fn record_failure(&mut self, service: &mo_bytecode::ServiceDef, f: &SimulationFailure) {
        self.failed = true;
        self.failure = RecoveryFailure {
            status: f.status,
            now: f.now,
            function: f.function,
            span: f
                .defect
                .as_ref()
                .and_then(|d| d.frames.first().and_then(|fr| fr.span)),
            reason: f
                .defect
                .as_ref()
                .map_or_else(String::new, |d| d.reason.clone()),
            detail: f.defect.as_ref().and_then(|d| d.detail.clone()),
        };
        for timer in &service.timers {
            if f.function == timer.function {
                self.lost += 1;
                self.failed_firings += 1;
            }
        }
    }
}

struct ShrinkState {
    failure: RecoveryOutcome,
    verdict: RecoveryVerdict,
    runs: u32,
    ended: bool,
}

fn step_unit(sequence: &RecoverySequence, i: usize) -> usize {
    if sequence.steps[i].kind == RecoveryStepKind::Crash {
        2
    } else {
        1
    }
}

fn copy_without(from: &RecoverySequence, skip: usize, end: usize) -> RecoverySequence {
    let mut steps = Vec::with_capacity(from.steps.len().saturating_sub(end - skip));
    for (i, step) in from.steps.iter().enumerate() {
        if i >= skip && i < end {
            continue;
        }
        let mut copy = RecoveryStep::new(step.kind);
        copy.tool = step.tool;
        copy.duration = step.duration;
        copy.arguments = step.arguments.clone();
        steps.push(copy);
    }
    RecoverySequence { steps }
}

fn mul_clamp(every: i64, multiple: u64) -> i64 {
    if (every as u64) > (i64::MAX as u64) / multiple {
        i64::MAX
    } else {
        every * (multiple as i64)
    }
}

fn is_leaf_kind(kind: TypeKind) -> bool {
    matches!(
        kind,
        TypeKind::Bool
            | TypeKind::Byte
            | TypeKind::Int
            | TypeKind::Float
            | TypeKind::Char
            | TypeKind::Time
            | TypeKind::Duration
            | TypeKind::Decimal
            | TypeKind::Range
            | TypeKind::Text
            | TypeKind::Bytes
            | TypeKind::BigInt
    )
}

fn collect_leaves(
    types: &TypeTable,
    root_ty: u32,
    root_val: &Value,
    room: usize,
) -> Vec<(u32, Value)> {
    let mut found = Vec::new();
    let mut stack: Vec<(Vec<(u32, Value)>, usize)> = Vec::new();
    let mut ty = root_ty;
    let mut val = root_val.clone();

    loop {
        let t = types.get(ty);
        if is_leaf_kind(t.kind) {
            found.push((ty, val));
        } else if t.kind != TypeKind::Unit {
            let mut children = Vec::new();
            match t.kind {
                TypeKind::Tuple => {
                    if let HeapObj::Tuple { fields, .. } = val.as_heap_ref().obj() {
                        for (&p, v) in t.parameters.iter().zip(fields) {
                            children.push((p, v.clone()));
                        }
                    }
                }
                TypeKind::Record => {
                    if let HeapObj::Record { fields, .. } = val.as_heap_ref().obj() {
                        for (f, v) in t.fields.iter().zip(fields) {
                            children.push((f.ty, v.clone()));
                        }
                    }
                }
                TypeKind::Choice => {
                    let tag = val.as_tag();
                    let case = &t.cases[tag as usize];
                    match types.case_shape(ty, tag) {
                        CaseShape::Empty => {}
                        CaseShape::Inline => {
                            if let Value::ChoiceInline { payload, .. } = &val {
                                let fty = case.fields[0].ty;
                                children.push((
                                    fty,
                                    inline_payload_to_value(*payload, types.get(fty).kind),
                                ));
                            }
                        }
                        CaseShape::Ref => {
                            if let Value::ChoiceRef { payload, .. } = &val {
                                children.push((case.fields[0].ty, Value::Ref(payload.clone())));
                            }
                        }
                        CaseShape::Boxed => {
                            if let Value::ChoiceRef { payload, .. } = &val
                                && let HeapObj::CaseBlock { fields, .. } = payload.obj()
                            {
                                for (f, v) in case.fields.iter().zip(fields) {
                                    children.push((f.ty, v.clone()));
                                }
                            }
                        }
                    }
                }
                TypeKind::List => {
                    let elem_ty = t.parameters[0];
                    for item in val.as_list() {
                        children.push((elem_ty, item.clone()));
                    }
                }
                TypeKind::Set => {
                    let elem_ty = t.parameters[0];
                    for item in val.as_set().iter() {
                        children.push((elem_ty, item.clone()));
                    }
                }
                TypeKind::Map => {
                    let kty = t.parameters[0];
                    let vty = t.parameters[1];
                    for (k, v) in val.as_map().iter() {
                        children.push((kty, k.clone()));
                        children.push((vty, v.clone()));
                    }
                }
                _ => {}
            }
            stack.push((children, 0));
        }

        while let Some((children, idx)) = stack.last() {
            if *idx == children.len() {
                stack.pop();
            } else {
                break;
            }
        }
        if stack.is_empty() || found.len() == room {
            return found;
        }
        #[expect(
            clippy::expect_used,
            reason = "stack checked non-empty immediately above"
        )]
        let (children, idx) = stack.last_mut().expect("non-empty walk stack");
        let (next_ty, next_val) = children[*idx].clone();
        *idx += 1;
        ty = next_ty;
        val = next_val;
    }
}
