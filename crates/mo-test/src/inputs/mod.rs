//! Type-directed property input generation and shrinking (`src/tools/inputs*.c`).

mod build;
mod prepare;
mod scalars;
mod shrink;

use mo_bytecode::types::TypeTable;
use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_INPUT_BYTES, MO_LIMIT_INPUT_DEPTH,
    MO_LIMIT_INPUT_ELEMENTS, MO_LIMIT_INPUT_METADATA, MO_LIMIT_INPUT_NODES, MO_LIMIT_INPUT_WORK,
};
use mo_heap::hash::siphash13;
use mo_heap::{Heap, Value};

pub use shrink::{InputExtreme, InputExtremeEnd, InputShrinker};

/// Fixed public SipHash-1-3 key for deterministic promise input seeding (`src/tools/inputs.c:8`).
const INPUT_KEY: [u64; 2] = [0x6d6f_2d70_726f_6d69, 0x7365_2d69_6e70_7574];

/// SplitMix64 generator matching `src/tools/inputs.c:mo_input_random`.
#[must_use]
pub fn input_random(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut word = *state;
    word = (word ^ (word >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    word = (word ^ (word >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    word ^ (word >> 31)
}

fn input_stream(seed: u64, ordinal: u64) -> u64 {
    let mut bytes = [0u8; 16];
    bytes[..8].copy_from_slice(&seed.to_le_bytes());
    bytes[8..].copy_from_slice(&ordinal.to_le_bytes());
    siphash13(INPUT_KEY, &bytes)
}

/// Outcome of input preparation, generation, copying, or shrinking (`enum mo_input_result`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputResult {
    Ok,
    Done,
    Unsupported,
    Uninhabited,
    Limit,
    NoMemory,
}

/// Kind of generated trial (`enum mo_input_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputKind {
    Boundary,
    Random,
}

/// Resource limits for input generation (`struct mo_input_limits`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InputLimits {
    pub combinations: u32,
    pub depth: u32,
    pub nodes: u32,
    pub elements: u32,
    pub bytes: usize,
    pub work: u64,
    pub metadata: usize,
    pub prepare_work: u64,
}

impl Default for InputLimits {
    fn default() -> Self {
        Self {
            combinations: 200,
            depth: 64,
            nodes: 4096,
            elements: 1024,
            bytes: 64 << 10,
            work: 1 << 20,
            metadata: 64 << 20,
            prepare_work: MO_LIMIT_INPUT_WORK,
        }
    }
}

/// Request handed to a custom leaf [`InputFactory`] (`struct mo_input_request`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InputLeafRequest {
    pub kind: InputKind,
    pub boundary: u32,
    pub size: u32,
    pub seed: u64,
}

/// Custom leaf generator for capabilities or hidden built types (`struct mo_input_factory`).
pub trait InputFactory {
    /// Bytecode type index this factory handles.
    fn ty(&self) -> u32;
    /// Number of boundary values (> 0).
    fn boundaries(&self) -> u32;
    /// Whether generated values are hashable (usable as Map/Set keys).
    fn hashable(&self) -> bool;
    /// Generates a new value.
    fn build(
        &mut self,
        heap: &Heap,
        request: &InputLeafRequest,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult>;
    /// Copies an existing value into `heap`.
    fn copy(
        &mut self,
        heap: &Heap,
        source: &Value,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult>;
    /// Produces the `variant`-th shrink candidate, or `Err(InputResult::Done)` if none.
    fn shrink(
        &mut self,
        _heap: &Heap,
        _source: &Value,
        _variant: u32,
        _remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        Err(InputResult::Done)
    }
}

#[derive(Clone)]
pub(crate) struct TypeInfo {
    pub factory_index: Option<usize>,
    pub children: Vec<u32>,
    pub case_offsets: Vec<u32>,
    pub case_layouts: Vec<u32>,
    pub case_tags: Vec<u32>,
    pub case_depths: Vec<u32>,
    pub case_count: u32,
    pub count: u32,
    pub depth: u32,
    pub base_case: u32,
    pub layout: u32,
    pub order: u32,
    pub visit: u8,
    pub reachable: bool,
    pub hashable: bool,
}

impl Default for TypeInfo {
    fn default() -> Self {
        Self {
            factory_index: None,
            children: Vec::new(),
            case_offsets: Vec::new(),
            case_layouts: Vec::new(),
            case_tags: Vec::new(),
            case_depths: Vec::new(),
            case_count: 0,
            count: 0,
            depth: mo_bytecode::NO_INDEX,
            base_case: mo_bytecode::NO_INDEX,
            layout: mo_bytecode::NO_INDEX,
            order: 0,
            visit: 0,
            reachable: false,
            hashable: false,
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct InputBudget {
    pub left: InputLimits,
}

impl InputBudget {
    pub fn work(&mut self, amount: u64) -> bool {
        if amount > self.left.work {
            return false;
        }
        self.left.work -= amount;
        true
    }

    pub fn bytes(&mut self, amount: usize) -> bool {
        if amount > self.left.bytes || !self.work(amount as u64) {
            return false;
        }
        self.left.bytes -= amount;
        true
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EditKind {
    Scalar,
    Erase,
    Base,
    Factory,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct InputEdit {
    pub node: u64,
    pub kind: EditKind,
    pub variant: u32,
    pub start: u32,
    pub count: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BuildMode {
    Minimum,
    Sample,
    Prng,
    Copy,
}

/// Prepared deterministic input generator (`struct mo_inputs`).
pub struct Inputs<'a> {
    pub(crate) types: &'a TypeTable,
    pub(crate) limits: InputLimits,
    pub(crate) parameters: Vec<u32>,
    pub(crate) counts: Vec<u32>,
    pub(crate) info: Vec<TypeInfo>,
    pub(crate) seed: u64,
    pub(crate) ordinal: u64,
    pub(crate) axes: u32,
    pub(crate) boundaries: u32,
}

impl<'a> Inputs<'a> {
    /// Prepares a generator for `parameters` seeded by `promise_name` (`mo_inputs_init`).
    pub fn init(
        types: &'a TypeTable,
        parameters: &[u32],
        promise_name: &[u8],
        limits: InputLimits,
        factories: &[&dyn InputFactory],
    ) -> Result<Self, InputResult> {
        debug_assert!(parameters.len() <= MO_LIMIT_BYTECODE_PARAMETERS as usize);
        if u64::from(limits.depth) > MO_LIMIT_INPUT_DEPTH
            || u64::from(limits.nodes) > MO_LIMIT_INPUT_NODES
            || u64::from(limits.elements) > MO_LIMIT_INPUT_ELEMENTS
            || (limits.bytes as u64) > MO_LIMIT_INPUT_BYTES
            || limits.work > MO_LIMIT_INPUT_WORK
            || (limits.metadata as u64) > MO_LIMIT_INPUT_METADATA
            || limits.prepare_work > MO_LIMIT_INPUT_WORK
            || (promise_name.len() as u64) > limits.prepare_work
        {
            return Err(InputResult::Limit);
        }
        let seed = siphash13(INPUT_KEY, promise_name);
        let mut inputs = Self {
            types,
            limits,
            parameters: parameters.to_vec(),
            counts: vec![0; parameters.len()],
            info: Vec::new(),
            seed,
            ordinal: 0,
            axes: 0,
            boundaries: 0,
        };
        prepare::prepare(&mut inputs, factories, promise_name.len())?;
        let mut product: u64 = 1;
        let mut axes: u64 = 1;
        let cap = u64::from(limits.combinations);
        for (i, &param) in parameters.iter().enumerate() {
            let variants = u64::from(inputs.info[param as usize].count);
            debug_assert!(variants != 0);
            inputs.counts[i] = variants as u32;
            axes = axes.saturating_add(variants - 1);
            product = if product > cap / variants {
                cap
            } else {
                product * variants
            };
        }
        inputs.axes = axes.min(cap) as u32;
        inputs.boundaries = product.min(cap) as u32;
        Ok(inputs)
    }

    /// Generates the next tuple of argument values (`mo_inputs_next`).
    pub fn next<'f>(
        &mut self,
        destination: &Heap,
        factories: &mut [&mut (dyn InputFactory + 'f)],
    ) -> Result<(Vec<Value>, InputKind), InputResult> {
        if self.ordinal == u64::MAX || (self.parameters.is_empty() && self.ordinal != 0) {
            return Err(InputResult::Done);
        }
        let boundary = self.ordinal < u64::from(self.boundaries);
        let mut variants = vec![0u32; self.parameters.len()];
        let mut work = self.limits.work;
        if boundary && !self.boundary_variants(&mut variants, &mut work) {
            return Err(InputResult::Limit);
        }
        let size = if boundary {
            0
        } else {
            (1 + (self.ordinal - u64::from(self.boundaries)) / 4)
                .min(u64::from(self.limits.elements)) as u32
        };
        let mut left = self.limits;
        left.work = work;
        let values = build::build_inputs(
            self,
            destination,
            None,
            boundary.then_some(variants.as_slice()),
            size,
            input_stream(self.seed, self.ordinal),
            None,
            &mut left,
            factories,
        )?;
        let kind = if boundary {
            InputKind::Boundary
        } else {
            InputKind::Random
        };
        self.ordinal += 1;
        Ok((values, kind))
    }

    /// Advances the ordinal by one without building values (`mo_inputs_skip`).
    pub fn skip(&mut self) {
        debug_assert!(self.ordinal != u64::MAX);
        self.ordinal += 1;
    }

    /// Sets the generator ordinal directly (`mo_inputs_seek`).
    pub fn seek(&mut self, ordinal: u64) {
        debug_assert!(ordinal != u64::MAX);
        self.ordinal = ordinal;
    }

    /// Returns the current generator ordinal.
    #[must_use]
    pub const fn ordinal(&self) -> u64 {
        self.ordinal
    }

    /// Returns the number of boundary trials (`mo_inputs_boundaries`).
    #[must_use]
    pub fn boundaries(&self) -> u64 {
        u64::from(self.boundaries)
    }

    /// Copies a full argument tuple into `destination` within `remaining` (`mo_inputs_copy`).
    pub fn copy<'f>(
        &self,
        destination: &Heap,
        source: &[Value],
        remaining: &mut InputLimits,
        factories: &mut [&mut (dyn InputFactory + 'f)],
    ) -> Result<Vec<Value>, InputResult> {
        let mut left = self.limits;
        left.depth = left.depth.min(remaining.depth);
        left.nodes = left.nodes.min(remaining.nodes);
        left.bytes = left.bytes.min(remaining.bytes);
        left.work = left.work.min(remaining.work);
        let before = left;
        let result = build::build_inputs(
            self,
            destination,
            Some(source),
            None,
            0,
            0,
            None,
            &mut left,
            factories,
        );
        remaining.nodes -= before.nodes - left.nodes;
        remaining.bytes -= before.bytes - left.bytes;
        remaining.work -= before.work - left.work;
        result
    }

    /// Returns whether `ty` was found reachable and hashable during preparation (`mo_inputs_hashable`).
    #[must_use]
    pub fn hashable(&self, ty: u32) -> bool {
        let info = &self.info[ty as usize];
        info.reachable && info.hashable
    }

    /// Returns the number of parameters.
    #[must_use]
    pub fn parameter_count(&self) -> usize {
        self.parameters.len()
    }

    /// Returns the parameter type indices.
    #[must_use]
    pub fn parameters(&self) -> &[u32] {
        &self.parameters
    }

    /// Checks whether `values[parameter]` contains an extreme scalar boundary (`mo_inputs_extreme`).
    pub fn extreme(&self, values: &[Value], parameter: usize) -> Result<InputExtreme, InputResult> {
        shrink::find_extreme(self, values, parameter)
    }

    fn boundary_variants(&self, variants: &mut [u32], work: &mut u64) -> bool {
        variants.fill(0);
        if self.ordinal < u64::from(self.axes) {
            let mut rank = self.ordinal;
            if rank == 0 {
                return true;
            }
            rank -= 1;
            for (slot, &count) in variants.iter_mut().zip(&self.counts) {
                let span = u64::from(count - 1);
                if rank < span {
                    *slot = 1 + rank as u32;
                    return true;
                }
                rank -= span;
            }
            unreachable!("axis rank out of range");
        }
        let mut index = self.ordinal - u64::from(self.axes) + 1;
        let mut weight: u64 = 1;
        for i in (0..self.parameters.len()).rev() {
            let count = u64::from(self.counts[i]);
            let mut digit: u64 = 1;
            while digit < count && digit <= index / weight {
                if *work == 0 {
                    return false;
                }
                *work -= 1;
                index += 1;
                digit += 1;
            }
            if weight > index / count {
                break;
            }
            weight *= count;
        }
        for i in (0..self.parameters.len()).rev() {
            let count = u64::from(self.counts[i]);
            variants[i] = (index % count) as u32;
            index /= count;
        }
        debug_assert_eq!(index, 0);
        true
    }
}
