//! Hidden-type builder discovery, companion pairing (D284), recording, replay, and shrinking
//! (`src/tools/builders.{h,c}`).

use std::cell::RefCell;
use std::rc::Rc;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{BytecodeFile, CaseShape, FunctionKind, NO_INDEX, TypeKind};
use mo_check::CheckedProgram;
use mo_def::limits::{
    MO_CLI_BUILDER_ATTEMPTS, MO_CLI_BUILDER_CALLS, MO_CLI_BUILDER_COMPANIONS,
    MO_CLI_BUILDER_LEVELS, MO_CLI_BUILDER_PATH, MO_CLI_BUILDER_RECIPES,
    MO_CLI_BUILDER_SHRINK_ATTEMPTS,
};
use mo_heap::{Heap, Value};
use mo_host::HostContext;
use mo_vm::{ControlledOutcome, Image, Limits, Process};

use crate::deadline::Deadline;
use crate::inputs::{
    InputFactory, InputKind, InputLeafRequest, InputLimits, InputResult, InputShrinker, Inputs,
    input_random,
};

const BOUNDARIES: u32 = 3;
const STEPS: u32 = (MO_CLI_BUILDER_CALLS - 1) as u32;
const UNLEVELLED: u32 = u32::MAX;

#[derive(Clone)]
struct BuilderMeta {
    function: u32, // NO_INDEX for constant
    global: u32,
    /// `(case_tag_or_NO_INDEX_for_tuple, field)` pairs from result to hidden value.
    path: Vec<(u32, u32)>,
    chained: bool,
    companion_field: u32,
    companion_slot: u32,
    needs: Vec<usize>,
    parameter_types: Vec<u32>,
    name: String,
    generated: bool,
}

#[derive(Clone)]
struct BuildCall {
    builder_index: usize,
    ordinal: u64,
    children: Vec<BuildRecipe>,
    slot: u32,
    arguments: Option<Vec<Value>>,
}

#[derive(Clone)]
struct BuildRecipe {
    hidden_slot: usize,
    id: u32,
    nodes: u32,
    bytes: usize,
    calls: Vec<BuildCall>,
    locked: bool,
}

#[derive(Clone, Copy)]
struct CompanionBinding {
    origin: u32,
    parameter: usize,
    ty: u32,
}

/// Recorded hidden-type builder tree for one promise trial (`struct mo_build_trace`).
#[derive(Clone, Default)]
pub struct BuildTrace {
    ordinal: u64,
    roots: Vec<BuildRecipe>,
    next_id: u32,
    bindings: Vec<CompanionBinding>,
    pub complete: bool,
}

#[derive(Clone)]
struct CompanionEntry {
    ty: u32,
    owner_ty: u32,
    origin: u32,
    value: Value,
    taken: bool,
}

struct ReplayState {
    source_bindings: Vec<CompanionBinding>,
    scope_stack: Vec<(Vec<BuildRecipe>, usize)>,
    node: u32,
    mask: u32,
    active: u32,
    slot: u32,
    replacement: Option<Vec<Value>>,
    capture_enabled: bool,
    captured: Option<Vec<Value>>,
    work: u64,
    invalid: bool,
}

struct HiddenSlot<'a> {
    ty: u32,
    copier: Option<Inputs<'a>>,
    hashable: bool,
    builders: Vec<BuilderMeta>,
    starts: Vec<usize>,
    steps: Vec<usize>,
    level: u32,
    building: bool,
    current_value: Value,
    companion: Option<(usize, Value)>,
}

struct BuildersInner<'a> {
    image: &'a Image,
    limits: Limits,
    deadline: Rc<RefCell<Deadline>>,
    hidden: Vec<HiddenSlot<'a>>,
    companions: Vec<CompanionEntry>,
    in_trial: bool,
    recording: Option<BuildTrace>,
    record_stack: Vec<Vec<BuildRecipe>>,
    replay: Option<ReplayState>,
}

fn option_or_result(name: Option<&str>, base: &str) -> bool {
    let Some(n) = name else {
        return false;
    };
    n.strip_prefix(base)
        .is_some_and(|rest| rest.starts_with('('))
}

fn find_path(
    types: &TypeTable,
    ty: u32,
    hidden: u32,
    depth: u32,
    path: &mut Vec<(u32, u32)>,
) -> bool {
    if ty == hidden {
        return true;
    }
    if u64::from(depth) == MO_CLI_BUILDER_PATH {
        return false;
    }
    let item = types.get(ty);
    if item.kind == TypeKind::Tuple {
        for (i, &param_ty) in item.parameters.iter().enumerate() {
            path.push((NO_INDEX, i as u32));
            if find_path(types, param_ty, hidden, depth + 1, path) {
                return true;
            }
            path.pop();
        }
    } else if item.kind == TypeKind::Choice
        && (option_or_result(item.name.as_deref(), "Option")
            || option_or_result(item.name.as_deref(), "Result"))
    {
        let tag = u32::from(option_or_result(item.name.as_deref(), "Option"));
        debug_assert!(
            (tag as usize) < item.cases.len() && item.cases[tag as usize].fields.len() == 1
        );
        path.push((tag, 0));
        if find_path(
            types,
            item.cases[tag as usize].fields[0].ty,
            hidden,
            depth + 1,
            path,
        ) {
            return true;
        }
        path.pop();
    }
    false
}

fn extract_path(
    types: &TypeTable,
    heap: &Heap,
    path: &[(u32, u32)],
    mut ty: u32,
    mut value: Value,
) -> Option<Value> {
    for &(tag, field) in path {
        let item = types.get(ty);
        if tag == NO_INDEX {
            value = heap.extract_field(&value, field);
            ty = item.parameters[field as usize];
            continue;
        }
        if value.as_tag() != tag {
            return None;
        }
        value = match types.case_shape(ty, tag) {
            CaseShape::Empty => Value::Unit,
            CaseShape::Inline | CaseShape::Ref | CaseShape::Boxed => {
                heap.extract_case_field(types, ty, &value, tag, field)
            }
        };
        ty = item.cases[tag as usize].fields[field as usize].ty;
    }
    Some(value)
}

fn buildable(
    types: &TypeTable,
    slots: &[Option<usize>],
    ty: u32,
    visited: &mut [bool],
    needs: &mut Vec<usize>,
) -> bool {
    if visited[ty as usize] {
        return true;
    }
    let mut stack = vec![ty];
    visited[ty as usize] = true;
    while let Some(at) = stack.pop() {
        if let Some(slot) = slots[at as usize] {
            needs.push(slot);
            continue;
        }
        let item = types.get(at);
        if matches!(
            item.kind,
            TypeKind::Function | TypeKind::Env | TypeKind::Capability
        ) {
            return false;
        }
        if item.kind == TypeKind::Record {
            for f in &item.fields {
                if !visited[f.ty as usize] {
                    visited[f.ty as usize] = true;
                    stack.push(f.ty);
                }
            }
        } else if item.kind == TypeKind::Choice {
            for case in &item.cases {
                for f in &case.fields {
                    if !visited[f.ty as usize] {
                        visited[f.ty as usize] = true;
                        stack.push(f.ty);
                    }
                }
            }
        } else if matches!(
            item.kind,
            TypeKind::Tuple | TypeKind::List | TypeKind::Set | TypeKind::Map
        ) {
            for &p in &item.parameters {
                if !visited[p as usize] {
                    visited[p as usize] = true;
                    stack.push(p);
                }
            }
        }
    }
    true
}

fn exposed_constant(program: &CheckedProgram, file: &BytecodeFile, global: u32) -> bool {
    let name = file.string(file.globals[global as usize].name);
    for c in &program.constants {
        let sym = program.interfaces.symbol(c.symbol);
        if !sym.exported {
            continue;
        }
        let mod_name = &program.modules.module(sym.module).name;
        if let Some(rest) = name.strip_prefix(mod_name.as_str())
            && let Some(sym_name) = rest.strip_prefix('.')
            && sym_name == sym.name
        {
            return true;
        }
    }
    false
}

/// Manages all hidden-type builders and companion pairing for one promise (`struct mo_builders`).
pub struct Builders<'a> {
    inner: Rc<RefCell<BuildersInner<'a>>>,
    slot_types: Vec<(usize, u32, bool)>,
}

impl<'a> Builders<'a> {
    /// Discovers hidden-type builders and levels for `image` (`mo_builders_init`).
    pub fn new(
        program: &CheckedProgram,
        image: &'a Image,
        limits: Limits,
        deadline: Rc<RefCell<Deadline>>,
    ) -> Result<Self, InputResult> {
        let file = image.file();
        let types = &file.types;
        let type_count = types.items.len();

        let mut slots: Vec<Option<usize>> = vec![None; type_count];
        let mut modules: Vec<usize> = vec![0; type_count];
        for (m, module) in file.modules.iter().enumerate() {
            for exported in &module.types {
                if exported.contents_exposed || slots[exported.ty as usize].is_some() {
                    continue;
                }
                slots[exported.ty as usize] = Some(0);
                modules[exported.ty as usize] = m;
            }
        }

        let mut hidden: Vec<HiddenSlot<'a>> = Vec::new();
        let mut copyable = Vec::new();
        for (t, slot_entry) in slots.iter_mut().enumerate() {
            if slot_entry.is_none() {
                continue;
            }
            let slot_idx = hidden.len();
            *slot_entry = Some(slot_idx);
            let ty = t as u32;
            let mut hashable = false;
            let copier = match Inputs::init(types, &[ty], b"", InputLimits::default(), &[]) {
                Ok(inp) => {
                    hashable = inp.hashable(ty);
                    copyable.push(true);
                    Some(inp)
                }
                Err(InputResult::NoMemory) => return Err(InputResult::NoMemory),
                Err(_) => {
                    copyable.push(false);
                    None
                }
            };
            hidden.push(HiddenSlot {
                ty,
                copier,
                hashable,
                builders: Vec::new(),
                starts: Vec::new(),
                steps: Vec::new(),
                level: UNLEVELLED,
                building: false,
                current_value: Value::Unit,
                companion: None,
            });
        }

        // Collect candidate builders per hidden slot.
        let mut visited = vec![false; type_count];
        for slot_idx in 0..hidden.len() {
            if !copyable[slot_idx] {
                continue;
            }
            let hidden_ty = hidden[slot_idx].ty;
            let owner = &file.modules[modules[hidden_ty as usize]];
            let mut found = Vec::new();

            for &global in &owner.globals {
                if file.globals[global as usize].ty != hidden_ty
                    || !exposed_constant(program, file, global)
                {
                    continue;
                }
                found.push(BuilderMeta {
                    function: NO_INDEX,
                    global,
                    path: Vec::new(),
                    chained: false,
                    companion_field: NO_INDEX,
                    companion_slot: NO_INDEX,
                    needs: Vec::new(),
                    parameter_types: Vec::new(),
                    name: String::new(),
                    generated: false,
                });
            }

            for &fn_idx in &owner.functions {
                let func = &file.functions[fn_idx as usize];
                let mut path = Vec::new();
                if func.kind != FunctionKind::Function
                    || func.spec_code
                    || !find_path(types, func.result_type, hidden_ty, 0, &mut path)
                {
                    continue;
                }
                let mut companion_field = NO_INDEX;
                let mut companion_slot = NO_INDEX;
                let returned = types.get(func.result_type);
                if !path.is_empty() && path[0].0 == NO_INDEX {
                    for (f, &param_ty) in returned.parameters.iter().enumerate() {
                        if f as u32 != path[0].1
                            && let Some(cslot) = slots[param_ty as usize]
                        {
                            companion_field = f as u32;
                            companion_slot = cslot as u32;
                            break;
                        }
                    }
                }
                visited.fill(false);
                let mut chained = false;
                let mut usable = true;
                let mut needs = Vec::new();
                let mut parameter_types = Vec::with_capacity(func.parameters.len());
                for p in &func.parameters {
                    parameter_types.push(p.ty);
                    if p.ty == hidden_ty {
                        chained = true;
                    } else if !buildable(types, &slots, p.ty, &mut visited, &mut needs) {
                        usable = false;
                        break;
                    }
                }
                if !usable {
                    continue;
                }
                // Validate that argument generator can be prepared with stub factories.
                let generated = !parameter_types.is_empty();
                if generated {
                    let mut stub_specs: Vec<(u32, u32, bool)> = Vec::new();
                    if chained {
                        stub_specs.push((hidden_ty, 1, hidden[slot_idx].hashable));
                    }
                    for &need_slot in &needs {
                        stub_specs.push((
                            hidden[need_slot].ty,
                            BOUNDARIES,
                            hidden[need_slot].hashable,
                        ));
                    }
                    let stubs: Vec<StubFactory> = stub_specs
                        .iter()
                        .map(|&(ty, b, h)| StubFactory {
                            ty,
                            boundaries: b,
                            hashable: h,
                        })
                        .collect();
                    let refs: Vec<&dyn InputFactory> =
                        stubs.iter().map(|s| s as &dyn InputFactory).collect();
                    let fname = file.string(func.name);
                    match Inputs::init(
                        types,
                        &parameter_types,
                        fname.as_bytes(),
                        InputLimits::default(),
                        &refs,
                    ) {
                        Ok(_) => {}
                        Err(InputResult::NoMemory) => return Err(InputResult::NoMemory),
                        Err(_) => continue,
                    }
                }
                found.push(BuilderMeta {
                    function: fn_idx,
                    global: NO_INDEX,
                    path,
                    chained,
                    companion_field,
                    companion_slot,
                    needs,
                    parameter_types,
                    name: file.string(func.name).to_string(),
                    generated,
                });
            }
            hidden[slot_idx].builders = found;
        }

        // Level assignment up to MO_CLI_BUILDER_LEVELS.
        for round in 0..MO_CLI_BUILDER_LEVELS as u32 {
            let levels: Vec<u32> = hidden.iter().map(|h| h.level).collect();
            for slot in &mut hidden {
                if slot.level != UNLEVELLED {
                    continue;
                }
                for b in &slot.builders {
                    if !b.chained && b.needs.iter().all(|&n| levels[n] < round) {
                        slot.level = round;
                        break;
                    }
                }
            }
        }
        let levels: Vec<u32> = hidden.iter().map(|h| h.level).collect();
        for slot in &mut hidden {
            if slot.level == UNLEVELLED {
                continue;
            }
            for (bi, b) in slot.builders.iter().enumerate() {
                if !b.needs.iter().all(|&n| levels[n] < slot.level) {
                    continue;
                }
                if b.chained {
                    slot.steps.push(bi);
                } else {
                    slot.starts.push(bi);
                }
            }
        }

        let slot_types: Vec<(usize, u32, bool)> = hidden
            .iter()
            .enumerate()
            .map(|(idx, h)| (idx, h.ty, h.hashable))
            .collect();

        let inner = Rc::new(RefCell::new(BuildersInner {
            image,
            limits,
            deadline,
            hidden,
            companions: Vec::new(),
            in_trial: false,
            recording: None,
            record_stack: Vec::new(),
            replay: None,
        }));
        Ok(Self { inner, slot_types })
    }

    /// Creates one [`BuilderFactory`] per hidden type slot.
    #[must_use]
    pub fn factories(&self) -> Vec<BuilderFactory<'a>> {
        self.slot_types
            .iter()
            .map(|&(slot, ty, hashable)| BuilderFactory {
                inner: Rc::clone(&self.inner),
                slot,
                ty,
                hashable,
            })
            .collect()
    }

    /// Begins recording a promise trial (`mo_builders_begin`).
    pub fn begin(&self, ordinal: u64) {
        let mut inner = self.inner.borrow_mut();
        inner.in_trial = true;
        inner.companions.clear();
        inner.recording = Some(BuildTrace {
            ordinal,
            roots: Vec::new(),
            next_id: 0,
            bindings: Vec::new(),
            complete: true,
        });
        inner.record_stack.clear();
        inner.record_stack.push(Vec::new());
    }

    /// Applies D284 companion pairing and finalizes the recorded [`BuildTrace`] (`mo_builders_relate`).
    pub fn relate(&self, inputs: &mut [Value], types: &[u32], swap: bool) -> BuildTrace {
        let mut inner = self.inner.borrow_mut();
        let mut trace = inner.recording.take().unwrap_or_default();
        if let Some(roots) = inner.record_stack.pop() {
            trace.roots = roots;
        }
        inner.record_stack.clear();

        let inner = &mut *inner;
        if let Some(ref mut replay) = inner.replay {
            if !inputs.is_empty() {
                trace.bindings = replay.source_bindings.clone();
                for binding in &trace.bindings {
                    let param = binding.parameter;
                    let mut found = false;
                    for comp in &mut inner.companions {
                        if comp.taken || comp.origin != binding.origin || comp.ty != types[param] {
                            continue;
                        }
                        inputs[param] = comp.value.clone();
                        comp.taken = true;
                        found = true;
                        break;
                    }
                    if !found {
                        replay.invalid = true;
                    }
                }
            }
        } else if swap && !inputs.is_empty() {
            let mut anchored: Vec<u32> = Vec::new();
            for i in 0..inputs.len() {
                if anchored.contains(&types[i]) {
                    continue;
                }
                for comp in &mut inner.companions {
                    if comp.taken || comp.ty != types[i] {
                        continue;
                    }
                    inputs[i] = comp.value.clone();
                    anchored.push(comp.owner_ty);
                    if trace.complete && (trace.bindings.len() as u64) < MO_CLI_BUILDER_COMPANIONS {
                        trace.bindings.push(CompanionBinding {
                            origin: comp.origin,
                            parameter: i,
                            ty: types[i],
                        });
                    }
                    comp.taken = true;
                    break;
                }
            }
        }

        if trace.complete {
            let hidden_types: Vec<u32> = inner.hidden.iter().map(|h| h.ty).collect();
            let bindings = trace.bindings.clone();
            lock_companions(&mut trace.roots, &hidden_types, &bindings, false);
        }
        inner.companions.clear();
        inner.in_trial = false;
        trace
    }
}

struct StubFactory {
    ty: u32,
    boundaries: u32,
    hashable: bool,
}

impl InputFactory for StubFactory {
    fn ty(&self) -> u32 {
        self.ty
    }
    fn boundaries(&self) -> u32 {
        self.boundaries
    }
    fn hashable(&self) -> bool {
        self.hashable
    }
    fn build(
        &mut self,
        _heap: &Heap,
        _request: &InputLeafRequest,
        _remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        Err(InputResult::Unsupported)
    }
    fn copy(
        &mut self,
        _heap: &Heap,
        _source: &Value,
        _remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        Err(InputResult::Unsupported)
    }
}

struct ChainFactory<'a> {
    inner: Rc<RefCell<BuildersInner<'a>>>,
    slot: usize,
    ty: u32,
    hashable: bool,
}

impl InputFactory for ChainFactory<'_> {
    fn ty(&self) -> u32 {
        self.ty
    }
    fn boundaries(&self) -> u32 {
        1
    }
    fn hashable(&self) -> bool {
        self.hashable
    }
    fn build(
        &mut self,
        _heap: &Heap,
        _request: &InputLeafRequest,
        _remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        Ok(self.inner.borrow().hidden[self.slot].current_value.clone())
    }
    fn copy(
        &mut self,
        heap: &Heap,
        source: &Value,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        copy_hidden_slot(&self.inner, self.slot, heap, source, remaining)
    }
}

/// Per-hidden-type [`InputFactory`] implementation.
pub struct BuilderFactory<'a> {
    inner: Rc<RefCell<BuildersInner<'a>>>,
    slot: usize,
    ty: u32,
    hashable: bool,
}

impl InputFactory for BuilderFactory<'_> {
    fn ty(&self) -> u32 {
        self.ty
    }

    fn boundaries(&self) -> u32 {
        BOUNDARIES
    }

    fn hashable(&self) -> bool {
        self.hashable
    }

    fn build(
        &mut self,
        heap: &Heap,
        request: &InputLeafRequest,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        let replay_source = {
            let mut inner = self.inner.borrow_mut();
            if let Some(ref mut replay) = inner.replay {
                let Some((recipes, idx)) = replay.scope_stack.last_mut() else {
                    return Err(InputResult::Limit);
                };
                let src = recipes[*idx].clone();
                *idx += 1;
                Some(src)
            } else {
                None
            }
        };
        if let Some(source) = replay_source {
            return replay_build(&self.inner, self.slot, &source, heap, remaining);
        }
        build_hidden(&self.inner, self.slot, heap, request, remaining)
    }

    fn copy(
        &mut self,
        heap: &Heap,
        source: &Value,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        copy_hidden_slot(&self.inner, self.slot, heap, source, remaining)
    }
}

fn copy_hidden_slot(
    inner_rc: &Rc<RefCell<BuildersInner<'_>>>,
    slot: usize,
    destination: &Heap,
    value: &Value,
    remaining: &mut InputLimits,
) -> Result<Value, InputResult> {
    let inner = inner_rc.borrow();
    if inner.hidden[slot].starts.is_empty() {
        return Err(InputResult::Unsupported);
    }
    let Some(ref copier) = inner.hidden[slot].copier else {
        return Err(InputResult::Unsupported);
    };
    let nodes_before = remaining.nodes;
    let res = copier.copy(destination, std::slice::from_ref(value), remaining, &mut []);
    if remaining.nodes < nodes_before {
        remaining.nodes += 1;
    }
    res.map(|mut v| v.pop().unwrap_or(Value::Unit))
}

fn copy_value_slot(
    inner_rc: &Rc<RefCell<BuildersInner<'_>>>,
    slot: usize,
    destination: &Heap,
    value: &Value,
    remaining: &mut InputLimits,
) -> Result<Value, InputResult> {
    let inner = inner_rc.borrow();
    let Some(ref copier) = inner.hidden[slot].copier else {
        return Err(InputResult::Unsupported);
    };
    let nodes_before = remaining.nodes;
    let res = copier.copy(destination, std::slice::from_ref(value), remaining, &mut []);
    if remaining.nodes < nodes_before {
        remaining.nodes += 1;
    }
    res.map(|mut v| v.pop().unwrap_or(Value::Unit))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum StepOutcome {
    Built,
    Skipped,
    Ended,
    Cancelled,
    Refused,
}

fn prepare_builder_inputs<'a>(
    inner_rc: &Rc<RefCell<BuildersInner<'a>>>,
    slot: usize,
    meta: &BuilderMeta,
) -> Result<(Inputs<'a>, Vec<Box<dyn InputFactory + 'a>>), InputResult> {
    let (image, hidden_ty, hidden_hashable, need_specs) = {
        let inner = inner_rc.borrow();
        let h = &inner.hidden[slot];
        let need_specs: Vec<(usize, u32, bool)> = meta
            .needs
            .iter()
            .map(|&n| (n, inner.hidden[n].ty, inner.hidden[n].hashable))
            .collect();
        (inner.image, h.ty, h.hashable, need_specs)
    };
    let mut factories: Vec<Box<dyn InputFactory + 'a>> = Vec::new();
    if meta.chained {
        factories.push(Box::new(ChainFactory {
            inner: Rc::clone(inner_rc),
            slot,
            ty: hidden_ty,
            hashable: hidden_hashable,
        }));
    }
    for (need_slot, ty, hashable) in need_specs {
        factories.push(Box::new(BuilderFactory {
            inner: Rc::clone(inner_rc),
            slot: need_slot,
            ty,
            hashable,
        }));
    }
    let refs: Vec<&dyn InputFactory> = factories.iter().map(Box::as_ref).collect();
    let inputs = Inputs::init(
        &image.file().types,
        &meta.parameter_types,
        meta.name.as_bytes(),
        InputLimits::default(),
        &refs,
    )?;
    Ok((inputs, factories))
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C run_builder_call internal helper parameters"
)]
fn run_builder_call<'a>(
    inner_rc: &Rc<RefCell<BuildersInner<'a>>>,
    slot: usize,
    process: &mut Process<'a>,
    builder_index: usize,
    ordinal: u64,
    has_value: &mut bool,
    value: &mut Value,
    source: Option<&BuildCall>,
) -> (StepOutcome, BuildCall) {
    let (image, deadline, meta, hidden_ty) = {
        let inner = inner_rc.borrow();
        let h = &inner.hidden[slot];
        (
            inner.image,
            Rc::clone(&inner.deadline),
            h.builders[builder_index].clone(),
            h.ty,
        )
    };

    // Push a child scope for recording nested hidden builds.
    {
        let mut inner = inner_rc.borrow_mut();
        if inner.recording.is_some() {
            inner.record_stack.push(Vec::new());
        }
    }

    let mut recorded_args: Option<Vec<Value>> = None;
    let outcome = if meta.function == NO_INDEX {
        match image.global(meta.global) {
            None => StepOutcome::Skipped,
            Some(global) => {
                inner_rc.borrow_mut().hidden[slot].companion = None;
                *value = global;
                *has_value = true;
                StepOutcome::Built
            }
        }
    } else {
        let mut args = Vec::new();
        let mut prep_outcome = StepOutcome::Built;
        if meta.generated {
            match prepare_builder_inputs(inner_rc, slot, &meta) {
                Err(InputResult::NoMemory) => prep_outcome = StepOutcome::Refused,
                Err(_) => prep_outcome = StepOutcome::Skipped,
                Ok((mut generator, mut factories)) => {
                    {
                        let mut inner = inner_rc.borrow_mut();
                        inner.hidden[slot].current_value = value.clone();
                    }
                    let (selected, frozen, capture_enabled) = {
                        let inner = inner_rc.borrow();
                        if let Some(src) = source
                            && let Some(ref replay) = inner.replay
                        {
                            let sel = replay.active == replay.node && src.slot == replay.slot;
                            let fr = if sel && replay.replacement.is_some() {
                                replay.replacement.clone()
                            } else {
                                src.arguments.clone()
                            };
                            (sel, fr, replay.capture_enabled)
                        } else {
                            (false, None, false)
                        }
                    };
                    let mut fac_refs: Vec<&mut (dyn InputFactory + 'a)> =
                        factories.iter_mut().map(Box::as_mut).collect();
                    let generated = if let Some(frozen_vals) = frozen {
                        let mut limits = InputLimits::default();
                        let res = generator.copy(
                            process.heap(),
                            &frozen_vals,
                            &mut limits,
                            &mut fac_refs,
                        );
                        // Pop the child replay scope because frozen arguments bypassed child recipes.
                        let mut inner = inner_rc.borrow_mut();
                        if let Some(ref mut replay) = inner.replay
                            && let Some((recipes, idx)) = replay.scope_stack.last_mut()
                        {
                            *idx = recipes.len();
                        }
                        match res {
                            Ok(mut copied) => {
                                if meta.chained {
                                    for (i, &pty) in meta.parameter_types.iter().enumerate() {
                                        if pty == hidden_ty {
                                            copied[i] = value.clone();
                                        }
                                    }
                                }
                                Ok(copied)
                            }
                            Err(err) => Err(err),
                        }
                    } else {
                        generator.seek(ordinal);
                        generator
                            .next(process.heap(), &mut fac_refs)
                            .map(|(v, _)| v)
                    };
                    match generated {
                        Err(InputResult::NoMemory) => prep_outcome = StepOutcome::Refused,
                        Err(_) => prep_outcome = StepOutcome::Skipped,
                        Ok(made_args) => {
                            if selected && capture_enabled {
                                let mut inner = inner_rc.borrow_mut();
                                if let Some(ref mut replay) = inner.replay {
                                    replay.captured = Some(made_args.clone());
                                }
                            }
                            if source.is_some_and(|s| s.arguments.is_some())
                                || (selected
                                    && inner_rc
                                        .borrow()
                                        .replay
                                        .as_ref()
                                        .is_some_and(|r| r.replacement.is_some()))
                            {
                                recorded_args = Some(made_args.clone());
                            }
                            args = made_args;
                        }
                    }
                }
            }
        }
        if prep_outcome != StepOutcome::Built {
            prep_outcome
        } else {
            let file = image.file();
            let func = &file.functions[meta.function as usize];
            let mut keep = || deadline.borrow_mut().running();
            let call_out = process.call_controlled(meta.function, args, Some(&mut keep), None);
            match call_out {
                ControlledOutcome::Cancelled => StepOutcome::Cancelled,
                ControlledOutcome::Failed(_) | ControlledOutcome::Stopped(_) => StepOutcome::Ended,
                ControlledOutcome::Returned(result) => {
                    match extract_path(
                        &file.types,
                        process.heap(),
                        &meta.path,
                        func.result_type,
                        result.clone(),
                    ) {
                        None => StepOutcome::Skipped,
                        Some(found) => {
                            let companion = if meta.companion_slot != NO_INDEX {
                                let comp_val =
                                    process.heap().extract_field(&result, meta.companion_field);
                                Some((meta.companion_slot as usize, comp_val))
                            } else {
                                None
                            };
                            inner_rc.borrow_mut().hidden[slot].companion = companion;
                            *value = found;
                            *has_value = true;
                            StepOutcome::Built
                        }
                    }
                }
            }
        }
    };

    let children = {
        let mut inner = inner_rc.borrow_mut();
        if inner.recording.is_some() {
            inner.record_stack.pop().unwrap_or_default()
        } else {
            Vec::new()
        }
    };

    let rec = BuildCall {
        builder_index,
        ordinal,
        children,
        slot: 0,
        arguments: recorded_args,
    };
    (outcome, rec)
}

fn random_ordinal(
    inner_rc: &Rc<RefCell<BuildersInner<'_>>>,
    slot: usize,
    meta: &BuilderMeta,
    random: &mut u64,
    size: u32,
) -> u64 {
    if !meta.generated {
        return 0;
    }
    let boundaries = match prepare_builder_inputs(inner_rc, slot, meta) {
        Ok((inp, _)) => inp.boundaries(),
        Err(_) => 0,
    };
    let grown = u64::from(size.min(64));
    boundaries + 4 * grown.saturating_sub(1) + input_random(random) % 4
}

fn keep_companion(
    inner_rc: &Rc<RefCell<BuildersInner<'_>>>,
    slot: usize,
    destination: &Heap,
    origin: u32,
) {
    let info = {
        let inner = inner_rc.borrow();
        if !inner.in_trial || (inner.companions.len() as u64) >= MO_CLI_BUILDER_COMPANIONS {
            return;
        }
        let Some((cslot, ref cval)) = inner.hidden[slot].companion else {
            return;
        };
        if let Some(ref replay) = inner.replay
            && !replay.source_bindings.iter().any(|b| b.origin == origin)
        {
            return;
        }
        Some((
            cslot,
            inner.hidden[cslot].ty,
            inner.hidden[slot].ty,
            cval.clone(),
        ))
    };
    let Some((cslot, cty, owner_ty, cval)) = info else {
        return;
    };
    let mut limits = InputLimits::default();
    if let Ok(copied) = copy_hidden_slot(inner_rc, cslot, destination, &cval, &mut limits) {
        inner_rc.borrow_mut().companions.push(CompanionEntry {
            ty: cty,
            owner_ty,
            origin,
            value: copied,
            taken: false,
        });
    }
}

fn next_recipe_id(inner_rc: &Rc<RefCell<BuildersInner<'_>>>) -> Option<u32> {
    let mut inner = inner_rc.borrow_mut();
    let rec = inner.recording.as_mut()?;
    if !rec.complete {
        return None;
    }
    if u64::from(rec.next_id) >= MO_CLI_BUILDER_RECIPES {
        rec.complete = false;
        return None;
    }
    let id = rec.next_id;
    rec.next_id += 1;
    Some(id)
}

fn build_hidden(
    inner_rc: &Rc<RefCell<BuildersInner<'_>>>,
    slot: usize,
    destination: &Heap,
    request: &InputLeafRequest,
    remaining: &mut InputLimits,
) -> Result<Value, InputResult> {
    let (image, limits, starts, steps_list) = {
        let mut inner = inner_rc.borrow_mut();
        if inner.hidden[slot].starts.is_empty() {
            return Err(InputResult::Unsupported);
        }
        debug_assert!(!inner.hidden[slot].building);
        inner.hidden[slot].building = true;
        inner.hidden[slot].companion = None;
        (
            inner.image,
            inner.limits,
            inner.hidden[slot].starts.clone(),
            inner.hidden[slot].steps.clone(),
        )
    };

    let recipe_id = next_recipe_id(inner_rc);
    let mut recipe_calls: Vec<BuildCall> = Vec::new();
    let nodes_before = remaining.nodes;
    let bytes_before = remaining.bytes;
    let boundary = request.kind == InputKind::Boundary;
    let mut random = if boundary {
        0x6275_696c_6465_7273u64.wrapping_add(u64::from(request.boundary))
    } else {
        request.seed
    };
    let mut step_count = if boundary {
        match request.boundary {
            0 => 0,
            1 => 1,
            _ => STEPS,
        }
    } else {
        (input_random(&mut random) % (u64::from(request.size.min(STEPS)) + 1)) as u32
    };
    if steps_list.is_empty() {
        step_count = 0;
    }

    let mut process: Option<Process<'_>> = None;
    let mut value = Value::Unit;
    let mut has_value = false;
    let mut result: Result<Value, InputResult> = Err(InputResult::Limit);

    let random_attempts = if boundary {
        0
    } else {
        (MO_CLI_BUILDER_ATTEMPTS / 2) as u32
    };
    let total_attempts = random_attempts + MO_CLI_BUILDER_ATTEMPTS as u32;
    for attempt in 0..total_attempts {
        if has_value {
            break;
        }
        if process.is_none() {
            process = Some(Process::new(
                image,
                limits,
                HostContext::stdio(Vec::new(), Vec::new()),
            ));
        }
        let ordered = attempt >= random_attempts;
        let rank = attempt - random_attempts;
        let start_idx = if ordered {
            starts[(rank as usize) % starts.len()]
        } else {
            starts[(input_random(&mut random) % starts.len() as u64) as usize]
        };
        let meta = inner_rc.borrow().hidden[slot].builders[start_idx].clone();
        let ordinal = if ordered {
            u64::from(rank) / starts.len() as u64
        } else {
            random_ordinal(inner_rc, slot, &meta, &mut random, request.size)
        };
        #[expect(
            clippy::expect_used,
            reason = "process was initialized immediately above when None"
        )]
        let proc_ref = process.as_mut().expect("scratch process");
        let (outcome, mut rec) = run_builder_call(
            inner_rc,
            slot,
            proc_ref,
            start_idx,
            ordinal,
            &mut has_value,
            &mut value,
            None,
        );
        match outcome {
            StepOutcome::Built => {
                if recipe_id.is_some() {
                    rec.slot = recipe_calls.len() as u32;
                    recipe_calls.push(rec);
                }
            }
            StepOutcome::Cancelled => {
                result = Err(InputResult::Limit);
                has_value = false;
                break;
            }
            StepOutcome::Refused => {
                result = Err(InputResult::NoMemory);
                has_value = false;
                break;
            }
            StepOutcome::Ended => {
                inner_rc.borrow_mut().hidden[slot].companion = None;
                process = None;
            }
            StepOutcome::Skipped => {}
        }
    }

    if has_value {
        let mut aborted = false;
        for i in 0..step_count {
            let step_idx = if boundary {
                steps_list[(i as usize) % steps_list.len()]
            } else {
                steps_list[(input_random(&mut random) % steps_list.len() as u64) as usize]
            };
            let meta = inner_rc.borrow().hidden[slot].builders[step_idx].clone();
            let ordinal = if boundary {
                1 + u64::from(i)
            } else {
                random_ordinal(inner_rc, slot, &meta, &mut random, request.size)
            };
            #[expect(
                clippy::expect_used,
                reason = "process is live whenever has_value is true"
            )]
            let proc_ref = process.as_mut().expect("scratch process");
            let (outcome, mut rec) = run_builder_call(
                inner_rc,
                slot,
                proc_ref,
                step_idx,
                ordinal,
                &mut has_value,
                &mut value,
                None,
            );
            match outcome {
                StepOutcome::Built => {
                    if recipe_id.is_some() {
                        rec.slot = recipe_calls.len() as u32;
                        recipe_calls.push(rec);
                    }
                }
                StepOutcome::Cancelled => {
                    result = Err(InputResult::Limit);
                    aborted = true;
                    break;
                }
                StepOutcome::Refused => {
                    result = Err(InputResult::NoMemory);
                    aborted = true;
                    break;
                }
                StepOutcome::Ended => break,
                StepOutcome::Skipped => {}
            }
        }
        if !aborted {
            result = copy_value_slot(inner_rc, slot, destination, &value, remaining);
            if result.is_ok() {
                if let Some(id) = recipe_id {
                    let recipe = BuildRecipe {
                        hidden_slot: slot,
                        id,
                        nodes: nodes_before - remaining.nodes,
                        bytes: bytes_before - remaining.bytes,
                        calls: recipe_calls,
                        locked: false,
                    };
                    let mut inner = inner_rc.borrow_mut();
                    if let Some(scope) = inner.record_stack.last_mut() {
                        scope.push(recipe);
                    }
                }
                keep_companion(inner_rc, slot, destination, recipe_id.unwrap_or(NO_INDEX));
            }
        }
    }

    {
        let mut inner = inner_rc.borrow_mut();
        inner.hidden[slot].companion = None;
        inner.hidden[slot].building = false;
    }
    result
}

fn replay_build(
    inner_rc: &Rc<RefCell<BuildersInner<'_>>>,
    slot: usize,
    source: &BuildRecipe,
    destination: &Heap,
    remaining: &mut InputLimits,
) -> Result<Value, InputResult> {
    let (image, limits, deadline, can_run) = {
        let mut inner = inner_rc.borrow_mut();
        let Some(ref mut replay) = inner.replay else {
            return Err(InputResult::Limit);
        };
        if replay.work == 0 {
            return Err(InputResult::Limit);
        }
        replay.work -= 1;
        let dl = Rc::clone(&inner.deadline);
        (inner.image, inner.limits, dl, true)
    };
    if !can_run || !deadline.borrow_mut().running() {
        return Err(InputResult::Limit);
    }

    let parent_active = {
        let mut inner = inner_rc.borrow_mut();
        inner.hidden[slot].building = true;
        inner.hidden[slot].companion = None;
        let prev = inner.replay.as_ref().map_or(0, |r| r.active);
        if let Some(ref mut replay) = inner.replay {
            replay.active = source.id;
        }
        prev
    };

    let recording_enabled = inner_rc
        .borrow()
        .recording
        .as_ref()
        .is_some_and(|r| r.complete);
    let mut recipe_calls = Vec::new();
    let mut process = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
    let mut value = Value::Unit;
    let mut has_value = false;
    let mut result: Result<Value, InputResult> = Err(InputResult::Done);

    let (replay_node, replay_mask) = {
        let inner = inner_rc.borrow();
        inner
            .replay
            .as_ref()
            .map_or((NO_INDEX, u32::MAX), |r| (r.node, r.mask))
    };

    let mut steps_ok = true;
    for (i, item) in source.calls.iter().enumerate() {
        if i != 0 && source.id == replay_node && (replay_mask & (1u32 << (i - 1))) == 0 {
            continue;
        }
        {
            let mut inner = inner_rc.borrow_mut();
            if let Some(ref mut replay) = inner.replay {
                replay.scope_stack.push((item.children.clone(), 0));
            }
        }
        let (step, mut rec) = run_builder_call(
            inner_rc,
            slot,
            &mut process,
            item.builder_index,
            item.ordinal,
            &mut has_value,
            &mut value,
            Some(item),
        );
        {
            let mut inner = inner_rc.borrow_mut();
            if let Some(ref mut replay) = inner.replay {
                replay.scope_stack.pop();
            }
        }
        if step != StepOutcome::Built {
            steps_ok = false;
            match step {
                StepOutcome::Cancelled => result = Err(InputResult::Limit),
                StepOutcome::Refused => result = Err(InputResult::NoMemory),
                _ => {
                    if let Some(ref mut replay) = inner_rc.borrow_mut().replay {
                        replay.invalid = true;
                    }
                    result = Err(InputResult::Done);
                }
            }
            break;
        }
        if recording_enabled {
            rec.slot = item.slot;
            recipe_calls.push(rec);
        }
    }

    if steps_ok {
        if source.nodes > remaining.nodes || source.bytes > remaining.bytes {
            if let Some(ref mut replay) = inner_rc.borrow_mut().replay {
                replay.invalid = true;
            }
            result = Err(InputResult::Done);
        } else {
            let mut reserved = *remaining;
            reserved.nodes = source.nodes + 1;
            reserved.bytes = source.bytes;
            let copied = copy_value_slot(inner_rc, slot, destination, &value, &mut reserved);
            remaining.work = reserved.work;
            match copied {
                Ok(val) => {
                    remaining.nodes -= source.nodes;
                    remaining.bytes -= source.bytes;
                    if recording_enabled {
                        let rec = BuildRecipe {
                            hidden_slot: slot,
                            id: source.id,
                            nodes: source.nodes,
                            bytes: source.bytes,
                            calls: recipe_calls,
                            locked: source.locked,
                        };
                        let mut inner = inner_rc.borrow_mut();
                        if let Some(scope) = inner.record_stack.last_mut() {
                            scope.push(rec);
                        }
                    }
                    keep_companion(inner_rc, slot, destination, source.id);
                    result = Ok(val);
                }
                Err(InputResult::Limit) => {
                    if let Some(ref mut replay) = inner_rc.borrow_mut().replay {
                        replay.invalid = true;
                    }
                    result = Err(InputResult::Done);
                }
                Err(err) => result = Err(err),
            }
        }
    }

    {
        let mut inner = inner_rc.borrow_mut();
        if let Some(ref mut replay) = inner.replay {
            replay.active = parent_active;
        }
        inner.hidden[slot].companion = None;
        inner.hidden[slot].building = false;
    }
    result
}

fn lock_companions(
    recipes: &mut [BuildRecipe],
    hidden_types: &[u32],
    bindings: &[CompanionBinding],
    locked: bool,
) {
    for recipe in recipes {
        recipe.locked |= locked;
        let rty = hidden_types[recipe.hidden_slot];
        for b in bindings {
            recipe.locked |= recipe.id == b.origin || rty == b.ty;
        }
        let now_locked = recipe.locked;
        for call in &mut recipe.calls {
            lock_companions(&mut call.children, hidden_types, bindings, now_locked);
        }
    }
}

fn find_recipe(recipes: &[BuildRecipe], id: u32) -> Option<&BuildRecipe> {
    for r in recipes {
        if r.id == id {
            return Some(r);
        }
        for c in &r.calls {
            if let Some(found) = find_recipe(&c.children, id) {
                return Some(found);
            }
        }
    }
    None
}

/// Deterministic shrinker for hidden-type builder traces (`struct mo_builder_shrinker`).
pub struct BuilderShrinker<'a> {
    builders: &'a Builders<'a>,
    attempts: u32,
    node: u32,
    mask: u32,
    call: usize,
    arguments_phase: bool,
    changed: bool,
    awaiting: bool,
    current_args: Option<Vec<Value>>,
    proposed_args: Option<Vec<Value>>,
    arg_shrinker: Option<InputShrinker>,
    arg_inputs: Option<(usize, BuilderMeta)>,
}

impl<'a> BuilderShrinker<'a> {
    #[must_use]
    pub fn new(builders: &'a Builders<'a>) -> Self {
        Self {
            builders,
            attempts: 0,
            node: 0,
            mask: 0,
            call: 0,
            arguments_phase: false,
            changed: false,
            awaiting: false,
            current_args: None,
            proposed_args: None,
            arg_shrinker: None,
            arg_inputs: None,
        }
    }

    fn reset_arg_state(&mut self) {
        self.current_args = None;
        self.proposed_args = None;
        self.arg_shrinker = None;
        self.arg_inputs = None;
    }

    fn capture_arguments(
        &mut self,
        best: &BuildTrace,
        source: &BuildRecipe,
    ) -> Result<(), InputResult> {
        let call = &source.calls[self.call];
        let (image, limits, meta) = {
            let inner = self.builders.inner.borrow();
            (
                inner.image,
                inner.limits,
                inner.hidden[source.hidden_slot].builders[call.builder_index].clone(),
            )
        };
        let proc_capture = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
        let replay = ReplayState {
            source_bindings: best.bindings.clone(),
            scope_stack: Vec::new(),
            node: source.id,
            mask: u32::MAX,
            active: 0,
            slot: call.slot,
            replacement: None,
            capture_enabled: true,
            captured: None,
            work: InputLimits::default().work,
            invalid: false,
        };
        self.builders.inner.borrow_mut().replay = Some(replay);
        let mut cap_limits = InputLimits::default();
        let res = replay_build(
            &self.builders.inner,
            source.hidden_slot,
            source,
            proc_capture.heap(),
            &mut cap_limits,
        );
        let captured = self
            .builders
            .inner
            .borrow_mut()
            .replay
            .take()
            .and_then(|r| r.captured);
        match (res, captured) {
            (Ok(_), Some(args)) => {
                self.current_args = Some(args);
                self.arg_shrinker = Some(InputShrinker::new(MO_CLI_BUILDER_SHRINK_ATTEMPTS as u32));
                self.arg_inputs = Some((source.hidden_slot, meta));
                Ok(())
            }
            (Ok(_), None) => Err(InputResult::Limit),
            (Err(err), _) => Err(err),
        }
    }

    fn argument_candidate(&mut self, best: &BuildTrace) -> Result<BuildRecipe, InputResult> {
        while self.node != 0 {
            let Some(source) = find_recipe(&best.roots, self.node - 1).cloned() else {
                self.node -= 1;
                self.call = 0;
                continue;
            };
            if source.locked || self.call >= source.calls.len() {
                self.node -= 1;
                self.call = 0;
                continue;
            }
            let generated = {
                let inner = self.builders.inner.borrow();
                inner.hidden[source.hidden_slot].builders[source.calls[self.call].builder_index]
                    .generated
            };
            if !generated {
                self.call += 1;
                continue;
            }
            if self.arg_shrinker.is_none() {
                self.capture_arguments(best, &source)?;
            }
            let Some((slot, meta)) = self.arg_inputs.clone() else {
                return Err(InputResult::Limit);
            };
            let (image, limits) = {
                let inner = self.builders.inner.borrow();
                (inner.image, inner.limits)
            };
            let proc_prop = Process::new(image, limits, HostContext::stdio(Vec::new(), Vec::new()));
            let (gen_inputs, mut factories) =
                prepare_builder_inputs(&self.builders.inner, slot, &meta)?;
            let mut fac_refs: Vec<&mut (dyn InputFactory + 'a)> =
                factories.iter_mut().map(Box::as_mut).collect();
            #[expect(
                clippy::expect_used,
                reason = "arg_shrinker and current_args are set by capture_arguments"
            )]
            let res = self.arg_shrinker.as_mut().expect("shrinker").next(
                &gen_inputs,
                self.current_args.as_ref().expect("current args"),
                proc_prop.heap(),
                &mut fac_refs,
            );
            match res {
                Ok(proposed) => {
                    self.proposed_args = Some(proposed);
                    return Ok(source);
                }
                Err(InputResult::Done) => {
                    self.reset_arg_state();
                    self.call += 1;
                }
                Err(err) => return Err(err),
            }
        }
        Err(InputResult::Done)
    }

    /// Produces the next shrunk input tuple and its recorded [`BuildTrace`] (`mo_builder_shrink_next`).
    pub fn next<'f>(
        &mut self,
        inputs: &mut Inputs<'_>,
        best: &BuildTrace,
        destination: &Heap,
        factories: &mut [&mut (dyn InputFactory + 'f)],
    ) -> Result<(Vec<Value>, BuildTrace), InputResult> {
        debug_assert!(!self.awaiting);
        if !best.complete {
            return Err(InputResult::Limit);
        }
        let source = if !self.arguments_phase {
            let mut found = None;
            while self.node < best.next_id {
                if let Some(src) = find_recipe(&best.roots, self.node)
                    && !src.locked
                    && self.mask < (1u32 << (src.calls.len() - 1)) - 1
                {
                    found = Some(src.clone());
                    break;
                }
                self.node += 1;
                self.mask = 0;
            }
            if let Some(src) = found {
                src
            } else {
                self.arguments_phase = true;
                self.call = 0;
                match self.argument_candidate(best) {
                    Ok(src) => src,
                    Err(InputResult::Done) if self.changed => {
                        self.arguments_phase = false;
                        self.changed = false;
                        self.node = 0;
                        self.mask = 0;
                        return Err(InputResult::Uninhabited);
                    }
                    Err(err) => return Err(err),
                }
            }
        } else {
            match self.argument_candidate(best) {
                Ok(src) => src,
                Err(InputResult::Done) if self.changed => {
                    self.arguments_phase = false;
                    self.changed = false;
                    self.node = 0;
                    self.mask = 0;
                    return Err(InputResult::Uninhabited);
                }
                Err(err) => return Err(err),
            }
        };

        if u64::from(self.attempts) == MO_CLI_BUILDER_SHRINK_ATTEMPTS {
            return Err(InputResult::Limit);
        }
        self.attempts += 1;

        let replay = ReplayState {
            source_bindings: best.bindings.clone(),
            scope_stack: vec![(best.roots.clone(), 0)],
            node: source.id,
            mask: if self.arguments_phase {
                u32::MAX
            } else {
                self.mask
            },
            active: 0,
            slot: if self.arguments_phase {
                source.calls[self.call].slot
            } else {
                0
            },
            replacement: if self.arguments_phase {
                self.proposed_args.clone()
            } else {
                None
            },
            capture_enabled: false,
            captured: None,
            work: InputLimits::default().work,
            invalid: false,
        };
        self.builders.begin(best.ordinal);
        self.builders.inner.borrow_mut().replay = Some(replay);
        inputs.seek(best.ordinal);
        let generated = inputs.next(destination, factories);
        let mut made = match generated {
            Ok((vals, _)) => vals,
            Err(err) => {
                let _ = self.builders.relate(&mut [], inputs.parameters(), false);
                let invalid = self
                    .builders
                    .inner
                    .borrow_mut()
                    .replay
                    .take()
                    .is_some_and(|r| r.invalid);
                self.awaiting = true;
                self.feedback(false);
                return Err(if err == InputResult::Done && invalid {
                    InputResult::Uninhabited
                } else {
                    err
                });
            }
        };
        let mut pending = self.builders.relate(&mut made, inputs.parameters(), false);
        let invalid = self
            .builders
            .inner
            .borrow_mut()
            .replay
            .take()
            .is_some_and(|r| r.invalid);
        pending.next_id = best.next_id;

        if invalid || !pending.complete {
            self.awaiting = true;
            self.feedback(false);
            return Err(if pending.complete {
                InputResult::Uninhabited
            } else {
                InputResult::Limit
            });
        }
        self.awaiting = true;
        Ok((made, pending))
    }

    /// Reports whether the last shrunk builder candidate still failed (`mo_builder_shrink_feedback`).
    pub fn feedback(&mut self, still_fails: bool) {
        debug_assert!(self.awaiting);
        self.awaiting = false;
        if self.arguments_phase {
            if let Some(ref mut s) = self.arg_shrinker {
                s.feedback(still_fails);
            }
            if still_fails {
                self.current_args = self.proposed_args.take();
                self.changed = true;
            } else {
                self.proposed_args = None;
            }
            return;
        }
        if still_fails {
            self.node = 0;
            self.mask = 0;
        } else {
            self.mask += 1;
        }
    }
}
