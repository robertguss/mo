//! Live service image swap helpers (`src/vm/service-swap.c` parity).
//!
//! Designed so both `mo-service`'s live upgrade protocol and `crates/mo-upgrade`
//! can atomically swap a running service's image, state, timers, and schema without losing
//! committed progress or failing half-way.

use mo_def::limits::MO_LIMIT_SERVICE_WORK;
use mo_heap::Heap;
use mo_heap::saved::decode_value;
use mo_host::HostContext;
use mo_vm::{Image, Outcome, Process};

use crate::binary::export::export_schema_digest;

use super::{ServiceLoop, ServiceStatus, TimerSlot, short_handler_name};

/// Pre-validated candidate state prepared off the hot path before committing a live swap.
pub struct PreparedSwap {
    /// New initialized bytecode image.
    pub image: Image,
    /// Service index within `image`.
    pub service_idx: u32,
    /// Fresh state heap containing the decoded new state.
    pub state_heap: Heap,
    /// Decoded state value in `state_heap`.
    pub state_value: mo_heap::Value,
    /// Encoded `MOSV\x01` bytes of `state_value`.
    pub encoded_state: Vec<u8>,
    /// 32-byte schema digest of the new state type.
    pub schema_digest: [u8; 32],
    /// New service image key (`program_checksum ^ ((service_idx as u64) << 32)`).
    pub image_key: u64,
    /// Recomputed timer schedule for the new service (preserving phase by timer name).
    pub timers: Vec<TimerSlot>,
}

impl ServiceLoop {
    /// Validates and prepares a candidate image and migrated/compatible encoded state
    /// without mutating the running service (`mo_service_swap_prepare`).
    pub fn prepare_swap(
        &self,
        new_image: Image,
        new_service_idx: u32,
        new_encoded_state: Vec<u8>,
        now_mono_us: u64,
    ) -> Result<PreparedSwap, ServiceStatus> {
        let file = new_image.file();
        let svc = file
            .services
            .get(new_service_idx as usize)
            .ok_or(ServiceStatus::Corrupt)?;
        let state_heap = Heap::new(self.limits.heap_size);
        let state_value =
            decode_value(&state_heap, &file.types, svc.state_type, &new_encoded_state)
                .map_err(|_| ServiceStatus::Corrupt)?;
        let schema_digest =
            export_schema_digest(&new_encoded_state).ok_or(ServiceStatus::Corrupt)?;
        let image_key = file.program_checksum ^ (u64::from(new_service_idx) << 32);

        // Evaluate candidate timer intervals and preserve phase (`next_due_us`) for any timer
        // whose short function name matches an existing timer (`src/vm/service-swap.c:132-148`).
        let old_file = self.image.file();
        let old_svc = &old_file.services[self.service_idx as usize];
        let mut timers = Vec::with_capacity(svc.timers.len());
        for t in &svc.timers {
            let mut proc = Process::new(
                &new_image,
                self.limits,
                HostContext::stdio(Vec::new(), Vec::new()),
            );
            let (outcome, _) = proc.call_with_budget(t.interval, Vec::new(), MO_LIMIT_SERVICE_WORK);
            let interval_us = match outcome {
                Outcome::Returned(v) => {
                    let us = v.as_duration();
                    if us <= 0 {
                        return Err(ServiceStatus::Invalid);
                    }
                    us as u64
                }
                Outcome::Failed(_) | Outcome::Stopped(_) => return Err(ServiceStatus::Failed),
            };

            let new_name = short_handler_name(file.function_name(t.function));
            let preserved_due = old_svc
                .timers
                .iter()
                .zip(self.timers.iter())
                .find(|(ot, _)| short_handler_name(old_file.function_name(ot.function)) == new_name)
                .map(|(_, slot)| slot.next_due_us);
            let next_due_us =
                preserved_due.unwrap_or_else(|| now_mono_us.saturating_add(interval_us));
            timers.push(TimerSlot {
                interval_us,
                next_due_us,
            });
        }

        Ok(PreparedSwap {
            image: new_image,
            service_idx: new_service_idx,
            state_heap,
            state_value,
            encoded_state: new_encoded_state,
            schema_digest,
            image_key,
            timers,
        })
    }

    /// Commits a [`PreparedSwap`] into the live service immediately after `ServiceStore::activate`
    /// has committed on disk (`mo_service_swap_finish`).
    pub fn finish_swap(&mut self, prepared: PreparedSwap) {
        self.image = prepared.image;
        self.service_idx = prepared.service_idx;
        self.state_heap = prepared.state_heap;
        self.state = prepared.state_value;
        self.encoded_state = prepared.encoded_state;
        self.encoded_sequence = self.sequence;
        self.snapshot_sequence = self.sequence;
        self.schema_digest = prepared.schema_digest;
        self.image_key = prepared.image_key;
        self.timers = prepared.timers;
        self.supervisor.failures.clear();
    }
}
