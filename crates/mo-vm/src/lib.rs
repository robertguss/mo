#![forbid(unsafe_code)]

//! Safe Rust Mo bytecode interpreter, image initialization, call frames, and defect reporting.

#[cfg(test)]
mod fuzz_tests;
mod number;
mod simulation;

use std::collections::HashMap;

use mo_bytecode::{
    AnchorExpectation, BytecodeFile, Capability, CaseShape, ConstantValue, FunctionKind,
    LayoutKind, OPCODE_MASK, OPCODE_VARIABLE_SHIFT, ParamOwnership, TypeKind,
};
use mo_def::limits::{MO_DEFAULT_CALL_DEPTH, MO_DEFAULT_HEAP_SIZE, MO_DEFAULT_STACK_SIZE};
use mo_def::{HostOpId, Opcode, PrimId, SimOpId};
use mo_heap::equal::value_equal;
use mo_heap::print::value_print_literal;
use mo_heap::{Heap, HeapObj, Value};
use mo_host::{HostContext, HostError};
use mo_prim::PrimError;

/// Matches C's `sizeof(struct mo_vm_frame)` on 64-bit platforms for `--max-stack` accounting.
const FRAME_HEADER_BYTES: usize = 80;
const CELL_AND_STATE_BYTES: usize = 17;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Limits {
    pub stack_size: usize,
    pub call_depth: u32,
    pub heap_size: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            stack_size: MO_DEFAULT_STACK_SIZE as usize,
            call_depth: MO_DEFAULT_CALL_DEPTH as u32,
            heap_size: MO_DEFAULT_HEAP_SIZE as usize,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DefectFrame {
    pub function: u32,
    pub pc: u32,
    pub span: Option<(u32, u32, u32)>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DefectVariable {
    pub name: String,
    pub value: String,
    pub truncated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Defect {
    pub reason: String,
    pub limit_name: Option<&'static str>,
    pub limit: usize,
    pub detail: Option<String>,
    pub frames: Vec<DefectFrame>,
    pub variables: Vec<DefectVariable>,
    pub variables_unavailable: bool,
}

pub enum Outcome {
    Returned(Value),
    Failed(Defect),
    Stopped(Defect),
}

pub(crate) enum ControlledOutcome {
    Returned(Value),
    Failed(Defect),
    Stopped(Defect),
    Cancelled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum RegState {
    Undefined,
    Value,
    Owned,
    Borrowed,
    Moved,
    Token,
}

pub(crate) struct Frame {
    pub(crate) function: u32,
    pub(crate) pc: u32,
    pub(crate) destination: u32,
    pub(crate) resume_pc: u32,
    pub(crate) frame_bytes: usize,
    pub(crate) regs: Vec<Option<Value>>,
    pub(crate) states: Vec<RegState>,
}

pub(crate) fn frame_size_bytes(register_count: usize) -> usize {
    (FRAME_HEADER_BYTES + register_count * CELL_AND_STATE_BYTES + 7) & !7
}

pub struct Image {
    file: BytecodeFile,
    _image_heap: Heap,
    constants: Vec<Value>,
    globals: Vec<Option<Value>>,
    captureless_closures: HashMap<u32, Value>,
}

impl Image {
    pub fn init(file: BytecodeFile, limits: Limits) -> Result<Self, Box<(BytecodeFile, Defect)>> {
        Self::init_with_budget(file, limits, None)
    }

    /// Initializes the image while capping total init instructions to `max_steps`.
    pub fn init_bounded(
        file: BytecodeFile,
        limits: Limits,
        max_steps: u64,
    ) -> Result<Self, Box<(BytecodeFile, Defect)>> {
        Self::init_with_budget(file, limits, Some(max_steps))
    }

    fn init_with_budget(
        file: BytecodeFile,
        limits: Limits,
        mut budget: Option<u64>,
    ) -> Result<Self, Box<(BytecodeFile, Defect)>> {
        let image_heap = Heap::new(limits.heap_size);

        // Pre-allocate immortal captureless closures for every 0-capture closure layout.
        let mut captureless_closures = HashMap::new();
        for (idx, layout) in file.types.layouts.iter().enumerate() {
            if layout.kind == LayoutKind::Closure && layout.fields.is_empty() {
                let closure =
                    match image_heap.alloc_closure(idx as u32, layout.function, Vec::new()) {
                        Ok(c) => c,
                        Err(_) => return Err(Box::new((file, oom_defect(limits.heap_size)))),
                    };
                image_heap.make_immortal(&closure);
                captureless_closures.insert(layout.function, closure);
            }
        }

        // Materialize and immortalize all constants.
        let mut constants = Vec::with_capacity(file.constants.len());
        for c in &file.constants {
            let kind = file.types.get(c.ty).kind;
            let val = match &c.value {
                ConstantValue::Immediate64(bytes) => match kind {
                    TypeKind::Unit => Value::Unit,
                    TypeKind::Bool => Value::Bool(bytes[0] != 0),
                    TypeKind::Byte => Value::Byte(bytes[0]),
                    TypeKind::Int => Value::Int(i64::from_le_bytes(*bytes)),
                    TypeKind::Float => Value::Float(f64::from_le_bytes(*bytes)),
                    TypeKind::Char => {
                        let code = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
                        Value::Char(char::from_u32(code).expect("validated char"))
                    }
                    TypeKind::Time => Value::Time(i64::from_le_bytes(*bytes)),
                    TypeKind::Duration => Value::Duration(i64::from_le_bytes(*bytes)),
                    _ => unreachable!("unexpected Immediate64 kind {kind:?}"),
                },
                ConstantValue::Decimal128(bytes) => Value::Decimal(i128::from_le_bytes(*bytes)),
                ConstantValue::Text(str_idx) => {
                    match image_heap.alloc_text(file.string(*str_idx).to_string()) {
                        Ok(v) => v,
                        Err(_) => return Err(Box::new((file, oom_defect(limits.heap_size)))),
                    }
                }
                ConstantValue::Bytes(raw) => match image_heap.alloc_bytes(raw.clone()) {
                    Ok(v) => v,
                    Err(_) => return Err(Box::new((file, oom_defect(limits.heap_size)))),
                },
                ConstantValue::BigInt { sign, magnitude } => {
                    match image_heap.alloc_bigint(*sign, magnitude.clone()) {
                        Ok(v) => v,
                        Err(_) => return Err(Box::new((file, oom_defect(limits.heap_size)))),
                    }
                }
                ConstantValue::Closure(fn_idx) => match captureless_closures.get(fn_idx) {
                    Some(v) => v.clone(),
                    None => {
                        let layout_idx = file
                            .types
                            .find_closure_layout(*fn_idx)
                            .expect("closure constant layout");
                        match image_heap.alloc_closure(layout_idx, *fn_idx, Vec::new()) {
                            Ok(v) => v,
                            Err(_) => return Err(Box::new((file, oom_defect(limits.heap_size)))),
                        }
                    }
                },
            };
            image_heap.make_immortal(&val);
            constants.push(val);
        }

        let globals = vec![None; file.globals.len()];
        let mut image = Self {
            file,
            _image_heap: image_heap,
            constants,
            globals,
            captureless_closures,
        };

        // Run init functions in order using the image's own heap.
        let init_fns: Vec<u32> = image.file.init.clone();
        for fn_idx in init_fns {
            let outcome = run_init_function(&mut image, limits, fn_idx, &mut budget);
            match outcome {
                Outcome::Returned(_) => {}
                Outcome::Failed(d) | Outcome::Stopped(d) => return Err(Box::new((image.file, d))),
            }
        }

        Ok(image)
    }

    pub fn file(&self) -> &BytecodeFile {
        &self.file
    }

    /// Finds `<module0>.main.entry`, matching `src/tools/run.c`.
    pub fn find_main_entry(&self) -> Option<u32> {
        if self.file.modules.is_empty() {
            return None;
        }
        let mod0 = self.file.string(self.file.modules[0].name);
        let want = format!("{mod0}.main.entry");
        for (i, f) in self.file.functions.iter().enumerate() {
            if f.module == 0 && f.kind == FunctionKind::Function && self.file.string(f.name) == want
            {
                return Some(i as u32);
            }
        }
        None
    }

    /// Returns all `MO_FUNCTION_TEST` and `MO_FUNCTION_EXAMPLE` functions in declaration order.
    pub fn test_and_example_functions(&self) -> Vec<u32> {
        self.file
            .functions
            .iter()
            .enumerate()
            .filter_map(|(i, f)| {
                matches!(f.kind, FunctionKind::Test | FunctionKind::Example).then_some(i as u32)
            })
            .collect()
    }

    /// Returns all promise anchor functions (`allows`/`forbids`), covering both function-level
    /// `FunctionKind::Anchor` entries and service-level `.service.promise.<N>.<allows|forbids>.<M>`
    /// functions, matching `src/tools/run.c:mo_check_anchors`.
    pub fn anchor_functions(&self) -> Vec<(u32, AnchorExpectation)> {
        Self::anchor_functions_in_file(&self.file)
    }

    /// Inspects a loaded `BytecodeFile` for promise anchor functions without running global init.
    pub fn anchor_functions_in_file(file: &BytecodeFile) -> Vec<(u32, AnchorExpectation)> {
        let mut out = Vec::new();
        for (i, f) in file.functions.iter().enumerate() {
            if f.kind == FunctionKind::Anchor {
                let exp = file.debug[i].expectation;
                if matches!(exp, AnchorExpectation::Allows | AnchorExpectation::Forbids) {
                    out.push((i as u32, exp));
                }
            } else if f.kind == FunctionKind::Function
                && f.parameters.is_empty()
                && file.types.get(f.result_type).kind == TypeKind::Int
            {
                let name = file.string(f.name);
                if let Some(idx) = name.find(".service.promise.") {
                    let tail = &name[idx + ".service.promise.".len()..];
                    if tail.contains(".allows.") {
                        out.push((i as u32, AnchorExpectation::Allows));
                    } else if tail.contains(".forbids.") {
                        out.push((i as u32, AnchorExpectation::Forbids));
                    }
                }
            }
        }
        out
    }
}

fn oom_defect(limit: usize) -> Defect {
    Defect {
        reason: "running out of memory".to_string(),
        limit_name: Some("--max-heap"),
        limit,
        detail: None,
        frames: Vec::new(),
        variables: Vec::new(),
        variables_unavailable: false,
    }
}

fn run_init_function(
    image: &mut Image,
    limits: Limits,
    fn_idx: u32,
    budget: &mut Option<u64>,
) -> Outcome {
    let mut host = HostContext::stdio(Vec::new(), Vec::new());
    let mut frames: Vec<Frame> = Vec::new();
    let mut stack_bytes: usize = 0;

    let fn_def = &image.file.functions[fn_idx as usize];
    let bytes = frame_size_bytes(fn_def.register_types.len());
    if limits.call_depth < 1 {
        return Outcome::Stopped(Defect {
            reason: "out of stack".to_string(),
            limit_name: Some("--max-depth"),
            limit: limits.call_depth as usize,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        });
    }
    if bytes > limits.stack_size {
        return Outcome::Stopped(Defect {
            reason: "out of stack".to_string(),
            limit_name: Some("--max-stack"),
            limit: limits.stack_size,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        });
    }
    frames.push(Frame {
        function: fn_idx,
        pc: 0,
        destination: 0,
        resume_pc: 0,
        frame_bytes: bytes,
        regs: vec![None; fn_def.register_types.len()],
        states: vec![RegState::Undefined; fn_def.register_types.len()],
    });
    stack_bytes += bytes;

    let mut ctx = ExecCtx {
        file: &image.file,
        constants: &image.constants,
        captureless: &image.captureless_closures,
        globals: Globals::Mutable(&mut image.globals),
        heap: &image._image_heap,
        host: &mut host,
        limits,
        frames: &mut frames,
        stack_bytes: &mut stack_bytes,
        work_remaining: *budget,
    };
    let outcome = match ctx.run_controlled() {
        ControlledOutcome::Returned(v) => Outcome::Returned(v),
        ControlledOutcome::Failed(d) => Outcome::Failed(d),
        ControlledOutcome::Stopped(d) => Outcome::Stopped(d),
        ControlledOutcome::Cancelled => Outcome::Stopped(ctx.build_defect(
            "internal error: instruction budget exhausted".to_string(),
            None,
            0,
            None,
        )),
    };
    *budget = ctx.work_remaining;
    outcome
}

pub struct Process<'a> {
    image: &'a Image,
    heap: Heap,
    limits: Limits,
    host: HostContext,
}

impl<'a> Process<'a> {
    pub fn new(image: &'a Image, limits: Limits, host: HostContext) -> Self {
        Self {
            image,
            heap: Heap::new(limits.heap_size),
            limits,
            host,
        }
    }

    pub fn call_main(&mut self, main_fn: u32) -> Outcome {
        self.call_main_inner(main_fn, None)
    }

    pub fn call_main_bounded(&mut self, main_fn: u32, max_steps: u64) -> Outcome {
        self.call_main_inner(main_fn, Some(max_steps))
    }

    fn call_main_inner(&mut self, main_fn: u32, max_steps: Option<u64>) -> Outcome {
        let fn_def = &self.image.file.functions[main_fn as usize];
        let mut caps_by_kind: HashMap<u32, Value> = HashMap::new();
        let mut args = Vec::with_capacity(fn_def.parameters.len());
        for (param_idx, p) in fn_def.parameters.iter().enumerate() {
            let cap_kind = self.image.file.types.get(p.ty).capability;
            let cap_val = if cap_kind == Some(Capability::Simulation) {
                let Some(svc_idx) = simulation::find_actor_service_for_param(
                    &self.image.file,
                    main_fn,
                    param_idx as u32,
                ) else {
                    return Outcome::Stopped(Defect {
                        reason: "unknown actor parameter in main".to_string(),
                        limit_name: None,
                        limit: 0,
                        detail: None,
                        frames: Vec::new(),
                        variables: Vec::new(),
                        variables_unavailable: false,
                    });
                };
                match simulation::make_live_actor_capability(self.image, &self.heap, p.ty, svc_idx)
                {
                    Ok(v) => v,
                    Err(reason) => {
                        return Outcome::Stopped(Defect {
                            reason,
                            limit_name: None,
                            limit: 0,
                            detail: None,
                            frames: Vec::new(),
                            variables: Vec::new(),
                            variables_unavailable: false,
                        });
                    }
                }
            } else if let Some(existing) = caps_by_kind.get(&p.ty) {
                existing.clone()
            } else {
                match self
                    .host
                    .make_real_capability(&self.heap, &self.image.file.types, p.ty)
                {
                    Ok(v) => {
                        caps_by_kind.insert(p.ty, v.clone());
                        v
                    }
                    Err(e) => {
                        return Outcome::Stopped(host_error_to_defect(e, self.limits.heap_size));
                    }
                }
            };
            args.push(cap_val);
        }
        self.call_inner(main_fn, args, max_steps)
    }

    pub fn call(&mut self, fn_idx: u32, args: Vec<Value>) -> Outcome {
        self.call_inner(fn_idx, args, None)
    }

    /// Executes `fn_idx` with at most `max_steps` VM instructions.
    pub fn call_bounded(&mut self, fn_idx: u32, args: Vec<Value>, max_steps: u64) -> Outcome {
        self.call_inner(fn_idx, args, Some(max_steps))
    }

    fn call_inner(
        &mut self,
        fn_idx: u32,
        args: Vec<Value>,
        work_remaining: Option<u64>,
    ) -> Outcome {
        let fn_def = &self.image.file.functions[fn_idx as usize];
        let bytes = frame_size_bytes(fn_def.register_types.len());
        if self.limits.call_depth < 1 {
            return Outcome::Stopped(Defect {
                reason: "out of stack".to_string(),
                limit_name: Some("--max-depth"),
                limit: self.limits.call_depth as usize,
                detail: None,
                frames: Vec::new(),
                variables: Vec::new(),
                variables_unavailable: false,
            });
        }
        if bytes > self.limits.stack_size {
            return Outcome::Stopped(Defect {
                reason: "out of stack".to_string(),
                limit_name: Some("--max-stack"),
                limit: self.limits.stack_size,
                detail: None,
                frames: Vec::new(),
                variables: Vec::new(),
                variables_unavailable: false,
            });
        }

        let mut regs = vec![None; fn_def.register_types.len()];
        let mut states = vec![RegState::Undefined; fn_def.register_types.len()];
        for (i, (param, val)) in fn_def.parameters.iter().zip(args).enumerate() {
            regs[i] = Some(val);
            states[i] = if self.image.file.types.is_reference(param.ty) {
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

        let mut ctx = ExecCtx {
            file: &self.image.file,
            constants: &self.image.constants,
            captureless: &self.image.captureless_closures,
            globals: Globals::ReadOnly(&self.image.globals),
            heap: &self.heap,
            host: &mut self.host,
            limits: self.limits,
            frames: &mut frames,
            stack_bytes: &mut stack_bytes,
            work_remaining,
        };
        match ctx.run_controlled() {
            ControlledOutcome::Returned(v) => Outcome::Returned(v),
            ControlledOutcome::Failed(d) => Outcome::Failed(d),
            ControlledOutcome::Stopped(d) => Outcome::Stopped(d),
            ControlledOutcome::Cancelled => Outcome::Stopped(ctx.build_defect(
                "internal error: instruction budget exhausted".to_string(),
                None,
                0,
                None,
            )),
        }
    }
}

fn host_error_to_defect(err: HostError, heap_limit: usize) -> Defect {
    match err {
        HostError::Bug { reason, detail } => Defect {
            reason: reason.to_string(),
            limit_name: None,
            limit: 0,
            detail,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        },
        HostError::Internal { reason } => Defect {
            reason: reason.to_string(),
            limit_name: None,
            limit: 0,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        },
        HostError::OutOfMemory { value_bytes_limit } => Defect {
            reason: if value_bytes_limit {
                "running out of memory: MO_LIMIT_VALUE_BYTES".to_string()
            } else {
                "running out of memory".to_string()
            },
            limit_name: if value_bytes_limit {
                None
            } else {
                Some("--max-heap")
            },
            limit: if value_bytes_limit { 0 } else { heap_limit },
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        },
        HostError::UnimplementedCapability(cap) => Defect {
            reason: format!("unimplemented capability: {}", cap.name()),
            limit_name: None,
            limit: 0,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
            variables_unavailable: false,
        },
    }
}

pub(crate) enum Globals<'a> {
    ReadOnly(&'a [Option<Value>]),
    Mutable(&'a mut [Option<Value>]),
}

impl Globals<'_> {
    fn as_slice(&self) -> &[Option<Value>] {
        match self {
            Globals::ReadOnly(s) => s,
            Globals::Mutable(s) => s,
        }
    }

    fn get(&self, idx: usize) -> Option<&Value> {
        self.as_slice().get(idx).and_then(|v| v.as_ref())
    }

    fn set(&mut self, idx: usize, val: Value) {
        match self {
            Globals::Mutable(s) => s[idx] = Some(val),
            Globals::ReadOnly(_) => panic!("SET_GLOBAL outside init function"),
        }
    }
}

pub(crate) struct ExecCtx<'a> {
    pub(crate) file: &'a BytecodeFile,
    pub(crate) constants: &'a [Value],
    pub(crate) captureless: &'a HashMap<u32, Value>,
    pub(crate) globals: Globals<'a>,
    pub(crate) heap: &'a Heap,
    pub(crate) host: &'a mut HostContext,
    pub(crate) limits: Limits,
    pub(crate) frames: &'a mut Vec<Frame>,
    pub(crate) stack_bytes: &'a mut usize,
    pub(crate) work_remaining: Option<u64>,
}

impl<'a> ExecCtx<'a> {
    fn build_defect(
        &self,
        reason: String,
        limit_name: Option<&'static str>,
        limit: usize,
        detail: Option<String>,
    ) -> Defect {
        let frames: Vec<DefectFrame> = self
            .frames
            .iter()
            .rev()
            .map(|f| DefectFrame {
                function: f.function,
                pc: f.pc,
                span: self.file.find_span(f.function, f.pc),
            })
            .collect();

        let mut variables = Vec::new();
        let mut variables_unavailable = false;
        if let Some(top) = self.frames.last() {
            let fn_def = &self.file.functions[top.function as usize];
            let dbg = &self.file.debug[top.function as usize];
            if !dbg.names.is_empty() {
                match self.file.registers_readable(top.function, top.pc) {
                    Err(_) => {
                        variables_unavailable = true;
                    }
                    Ok(readable) => {
                        for rn in &dbg.names {
                            let reg = rn.reg as usize;
                            if rn.code_start <= top.pc
                                && top.pc < rn.code_end
                                && readable.get(reg).copied().unwrap_or(false)
                                && matches!(
                                    top.states[reg],
                                    RegState::Value | RegState::Borrowed | RegState::Owned
                                )
                                && let Some(val) = &top.regs[reg]
                            {
                                let ty = fn_def.register_types[reg];
                                let (rendered, truncated) =
                                    format_defect_variable(&self.file.types, ty, val);
                                variables.push(DefectVariable {
                                    name: self.file.string(rn.name).to_string(),
                                    value: rendered,
                                    truncated,
                                });
                            }
                        }
                    }
                }
            }
        }

        Defect {
            reason,
            limit_name,
            limit,
            detail,
            frames,
            variables,
            variables_unavailable,
        }
    }

    fn stop(
        &self,
        reason: impl Into<String>,
        limit_name: Option<&'static str>,
        limit: usize,
        detail: Option<String>,
    ) -> ControlledOutcome {
        ControlledOutcome::Stopped(self.build_defect(reason.into(), limit_name, limit, detail))
    }

    fn fail(&self, detail: String) -> ControlledOutcome {
        ControlledOutcome::Failed(self.build_defect(
            "failed assertion".to_string(),
            None,
            0,
            Some(detail),
        ))
    }

    fn oom(&self) -> ControlledOutcome {
        self.stop(
            "running out of memory",
            Some("--max-heap"),
            self.limits.heap_size,
            None,
        )
    }

    fn write_reg(&mut self, reg: usize, value: Value, borrowed: bool) {
        let f = self.frames.last_mut().unwrap();
        let fn_def = &self.file.functions[f.function as usize];
        let ty = fn_def.register_types[reg];
        f.regs[reg] = Some(value);
        f.states[reg] = if self.file.types.is_reference(ty) {
            if borrowed {
                RegState::Borrowed
            } else {
                RegState::Owned
            }
        } else {
            RegState::Value
        };
    }

    fn consume_reg(&mut self, reg: usize) -> Value {
        let f = self.frames.last_mut().unwrap();
        let fn_def = &self.file.functions[f.function as usize];
        let ty = fn_def.register_types[reg];
        if self.file.types.is_reference(ty) {
            f.states[reg] = RegState::Moved;
            f.regs[reg].take().expect("owned register holds value")
        } else {
            f.regs[reg]
                .as_ref()
                .expect("value register holds value")
                .clone()
        }
    }

    fn read_reg(&self, reg: usize) -> &Value {
        let f = self.frames.last().unwrap();
        f.regs[reg].as_ref().expect("readable register holds value")
    }

    pub(crate) fn run_controlled(&mut self) -> ControlledOutcome {
        loop {
            if let Some(ref mut rem) = self.work_remaining {
                if *rem == 0 {
                    return ControlledOutcome::Cancelled;
                }
                *rem -= 1;
            }
            let (fn_idx, pc) = {
                let f = self.frames.last().unwrap();
                (f.function, f.pc)
            };
            let fn_def = &self.file.functions[fn_idx as usize];
            let code = &fn_def.code;
            let head = code[pc as usize];
            let op_num = (head & OPCODE_MASK) as u16;
            let var_count = (head >> OPCODE_VARIABLE_SHIFT) as usize;
            let op = Opcode::from_u16(op_num).expect("validated opcode");
            let info = op.info();
            let fixed = info.fixed_count as usize;
            let a = &code[pc as usize + 1..];
            let next_pc = pc + 1 + (fixed as u32) + (var_count as u32);

            match op {
                Opcode::AddInt
                | Opcode::SubInt
                | Opcode::MulInt
                | Opcode::DivInt
                | Opcode::RemInt
                | Opcode::NegInt
                | Opcode::AddDecimal
                | Opcode::SubDecimal
                | Opcode::MulDecimal
                | Opcode::DivDecimal
                | Opcode::RemDecimal
                | Opcode::NegDecimal
                | Opcode::AddFloat
                | Opcode::SubFloat
                | Opcode::MulFloat
                | Opcode::DivFloat
                | Opcode::RemFloat
                | Opcode::NegFloat
                | Opcode::NegDuration
                | Opcode::AddDuration
                | Opcode::SubDuration
                | Opcode::ScaleDuration
                | Opcode::AddTime
                | Opcode::SubTime
                | Opcode::DiffTime
                | Opcode::EqInt
                | Opcode::EqDecimal
                | Opcode::EqFloat
                | Opcode::EqBool
                | Opcode::EqByte
                | Opcode::EqChar
                | Opcode::EqTime
                | Opcode::EqDuration
                | Opcode::EqChoice
                | Opcode::LtInt
                | Opcode::LtDecimal
                | Opcode::LtFloat
                | Opcode::LtBool
                | Opcode::LtByte
                | Opcode::LtChar
                | Opcode::LtTime
                | Opcode::LtDuration
                | Opcode::LtChoice
                | Opcode::LeInt
                | Opcode::LeDecimal
                | Opcode::LeFloat
                | Opcode::LeBool
                | Opcode::LeByte
                | Opcode::LeChar
                | Opcode::LeTime
                | Opcode::LeDuration
                | Opcode::LeChoice
                | Opcode::CmpFloat
                | Opcode::Not
                | Opcode::MakeRange
                | Opcode::RangeStart
                | Opcode::RangeEnd => {
                    let left = self.read_reg(a[1] as usize).clone();
                    let right = if fixed == 3 {
                        Some(self.read_reg(a[2] as usize).clone())
                    } else {
                        None
                    };
                    match number::eval_number(op, &left, right.as_ref()) {
                        Ok(val) => self.write_reg(a[0] as usize, val, false),
                        Err(reason) => return self.stop(reason, None, 0, None),
                    }
                }
                Opcode::Move => {
                    let src = a[1] as usize;
                    let borrowed = self.frames.last().unwrap().states[src] == RegState::Borrowed;
                    let val = if borrowed {
                        self.read_reg(src).clone()
                    } else {
                        self.consume_reg(src)
                    };
                    self.write_reg(a[0] as usize, val, borrowed);
                }
                Opcode::Clear => {
                    let dst = a[0] as usize;
                    let f = self.frames.last_mut().unwrap();
                    f.regs[dst] = None;
                    f.states[dst] = RegState::Undefined;
                }
                Opcode::Dup => {
                    let val = self.read_reg(a[1] as usize).clone();
                    self.write_reg(a[0] as usize, val, false);
                }
                Opcode::Drop => {
                    let val = self.consume_reg(a[0] as usize);
                    drop(val);
                }
                Opcode::Const => {
                    let val = self.constants[a[1] as usize].clone();
                    self.write_reg(a[0] as usize, val, true);
                }
                Opcode::Global => {
                    let g = a[1] as usize;
                    let Some(val) = self.globals.get(g).cloned() else {
                        return self.stop("internal error: unset global", None, 0, None);
                    };
                    self.write_reg(a[0] as usize, val, true);
                }
                Opcode::SetGlobal => {
                    let val = self.consume_reg(a[1] as usize);
                    self.heap.make_immortal(&val);
                    self.globals.set(a[0] as usize, val);
                }
                Opcode::Jump => {
                    self.frames.last_mut().unwrap().pc = a[0];
                    continue;
                }
                Opcode::Branch => {
                    let cond = self.read_reg(a[0] as usize).as_bool();
                    self.frames.last_mut().unwrap().pc = if cond { a[1] } else { a[2] };
                    continue;
                }
                Opcode::Switch => {
                    let tag = self.read_reg(a[0] as usize).as_tag() as usize;
                    self.frames.last_mut().unwrap().pc = a[1 + tag];
                    continue;
                }
                Opcode::Call | Opcode::CallClosure | Opcode::TailCall | Opcode::TailCallClosure => {
                    let tail = matches!(op, Opcode::TailCall | Opcode::TailCallClosure);
                    let is_closure = matches!(op, Opcode::CallClosure | Opcode::TailCallClosure);
                    let target_word = a[usize::from(!tail)];
                    let callee_idx = if is_closure {
                        let closure_val = self.read_reg(target_word as usize);
                        let HeapObj::Closure { function, .. } = closure_val.as_heap_ref().obj()
                        else {
                            panic!("CALL_CLOSURE on non-closure");
                        };
                        *function
                    } else {
                        target_word
                    };
                    let callee = &self.file.functions[callee_idx as usize];
                    let new_frame_bytes = frame_size_bytes(callee.register_types.len());

                    // Check --max-depth and --max-stack before transferring ownership.
                    let new_depth = if tail {
                        self.frames.len()
                    } else {
                        self.frames.len() + 1
                    };
                    if (new_depth as u32) > self.limits.call_depth {
                        return self.stop(
                            "out of stack",
                            Some("--max-depth"),
                            self.limits.call_depth as usize,
                            None,
                        );
                    }
                    let base_stack = if tail {
                        *self.stack_bytes - self.frames.last().unwrap().frame_bytes
                    } else {
                        *self.stack_bytes
                    };
                    if base_stack.saturating_add(new_frame_bytes) > self.limits.stack_size {
                        return self.stop(
                            "out of stack",
                            Some("--max-stack"),
                            self.limits.stack_size,
                            None,
                        );
                    }

                    // Read all arguments before consuming any owned register so a register
                    // passed both borrowed and owned in the same call remains readable.
                    let arg_regs =
                        &a[if tail { 1 } else { 2 }..if tail { 1 } else { 2 } + var_count];
                    let mut call_args = Vec::with_capacity(var_count);
                    for &reg in arg_regs {
                        call_args.push(self.read_reg(reg as usize).clone());
                    }
                    let env_val = if is_closure {
                        Some(self.read_reg(target_word as usize).clone())
                    } else {
                        None
                    };
                    for (&reg, param) in arg_regs.iter().zip(&callee.parameters) {
                        if param.ownership == ParamOwnership::Owned {
                            let _ = self.consume_reg(reg as usize);
                        }
                    }
                    if is_closure {
                        let _ = self.consume_reg(target_word as usize);
                    }

                    let mut new_regs = vec![None; callee.register_types.len()];
                    let mut new_states = vec![RegState::Undefined; callee.register_types.len()];
                    for (i, (param, val)) in callee.parameters.iter().zip(call_args).enumerate() {
                        new_regs[i] = Some(val);
                        new_states[i] = if self.file.types.is_reference(param.ty) {
                            match param.ownership {
                                ParamOwnership::Owned => RegState::Owned,
                                ParamOwnership::Borrowed => RegState::Borrowed,
                            }
                        } else {
                            RegState::Value
                        };
                    }
                    if let Some(env) = env_val {
                        let env_slot = callee.parameters.len();
                        new_regs[env_slot] = Some(env);
                        new_states[env_slot] = RegState::Owned;
                    }

                    if tail {
                        let old = self.frames.pop().unwrap();
                        *self.stack_bytes -= old.frame_bytes;
                        *self.stack_bytes += new_frame_bytes;
                        self.frames.push(Frame {
                            function: callee_idx,
                            pc: 0,
                            destination: old.destination,
                            resume_pc: old.resume_pc,
                            frame_bytes: new_frame_bytes,
                            regs: new_regs,
                            states: new_states,
                        });
                    } else {
                        *self.stack_bytes += new_frame_bytes;
                        self.frames.push(Frame {
                            function: callee_idx,
                            pc: 0,
                            destination: a[0],
                            resume_pc: next_pc,
                            frame_bytes: new_frame_bytes,
                            regs: new_regs,
                            states: new_states,
                        });
                    }
                    continue;
                }
                Opcode::Ret => {
                    let ret_val = self.consume_reg(a[0] as usize);
                    let done = self.frames.pop().unwrap();
                    *self.stack_bytes -= done.frame_bytes;
                    if self.frames.is_empty() {
                        return ControlledOutcome::Returned(ret_val);
                    }
                    self.write_reg(done.destination as usize, ret_val, false);
                    self.frames.last_mut().unwrap().pc = done.resume_pc;
                    continue;
                }
                Opcode::Stop => {
                    let detail = self.read_reg(a[0] as usize).as_text().to_string();
                    return self.stop("stop", None, 0, Some(detail));
                }
                Opcode::Fail => {
                    let detail = self.read_reg(a[0] as usize).as_text().to_string();
                    return self.fail(detail);
                }
                Opcode::Construct => {
                    if !self.heap.can_alloc_fields(var_count) {
                        return self.oom();
                    }
                    let layout_idx = a[1];
                    let field_regs = &a[2..2 + var_count];
                    let mut fields = Vec::with_capacity(var_count);
                    for &reg in field_regs {
                        fields.push(self.consume_reg(reg as usize));
                    }
                    match self.heap.construct(&self.file.types, layout_idx, fields) {
                        Ok(val) => self.write_reg(a[0] as usize, val, false),
                        Err(_) => return self.oom(),
                    }
                }
                Opcode::MakeCase => {
                    let ty = a[1];
                    let tag = a[2];
                    if self.file.types.case_shape(ty, tag) == CaseShape::Boxed
                        && !self.heap.can_alloc_fields(var_count)
                    {
                        return self.oom();
                    }
                    let field_regs = &a[3..3 + var_count];
                    let mut fields = Vec::with_capacity(var_count);
                    for &reg in field_regs {
                        fields.push(self.consume_reg(reg as usize));
                    }
                    match self.heap.make_case(&self.file.types, ty, tag, fields) {
                        Ok(val) => self.write_reg(a[0] as usize, val, false),
                        Err(_) => return self.oom(),
                    }
                }
                Opcode::MakeClosure => {
                    let closure_fn = a[1];
                    if var_count == 0
                        && let Some(imm) = self.captureless.get(&closure_fn)
                    {
                        self.write_reg(a[0] as usize, imm.clone(), false);
                    } else {
                        if !self.heap.can_alloc_fields(var_count) {
                            return self.oom();
                        }
                        let layout_idx = self
                            .file
                            .types
                            .find_closure_layout(closure_fn)
                            .expect("closure layout");
                        let cap_regs = &a[2..2 + var_count];
                        let mut captures = Vec::with_capacity(var_count);
                        for &reg in cap_regs {
                            captures.push(self.consume_reg(reg as usize));
                        }
                        match self.heap.alloc_closure(layout_idx, closure_fn, captures) {
                            Ok(val) => self.write_reg(a[0] as usize, val, false),
                            Err(_) => return self.oom(),
                        }
                    }
                }
                Opcode::Field => {
                    let obj = self.read_reg(a[1] as usize);
                    let val = self.heap.extract_field(obj, a[2]);
                    self.write_reg(a[0] as usize, val, false);
                }
                Opcode::CaseField => {
                    let src = a[1] as usize;
                    let ty = fn_def.register_types[src];
                    let obj = self.read_reg(src);
                    let val = self
                        .heap
                        .extract_case_field(&self.file.types, ty, obj, a[2], a[3]);
                    self.write_reg(a[0] as usize, val, false);
                }
                Opcode::SetField => {
                    // Check COW heap headroom before consuming operands so OOM preserves
                    // pre-instruction register state for defect reporting.
                    let needs_copy = {
                        let r = self.read_reg(a[1] as usize).as_heap_ref();
                        if r.can_mutate_in_place() {
                            None
                        } else {
                            let count = match r.obj() {
                                HeapObj::Record { fields, .. } | HeapObj::Tuple { fields, .. } => {
                                    fields.len()
                                }
                                _ => panic!("SET_FIELD on non-Record/Tuple"),
                            };
                            Some(count)
                        }
                    };
                    if let Some(count) = needs_copy
                        && !self.heap.can_alloc_fields(count)
                    {
                        return self.oom();
                    }
                    let obj = self.consume_reg(a[1] as usize);
                    let replacement = self.consume_reg(a[3] as usize);
                    match self.heap.set_field_owned(obj, a[2], replacement) {
                        Ok(val) => self.write_reg(a[0] as usize, val, false),
                        Err(_) => return self.oom(),
                    }
                }
                Opcode::Equal => {
                    let ty = a[1];
                    let left = self.read_reg(a[2] as usize);
                    let right = self.read_reg(a[3] as usize);
                    let eq = value_equal(&self.file.types, self.heap.hash_key(), ty, left, right);
                    self.write_reg(a[0] as usize, Value::Bool(eq), false);
                }
                Opcode::Reset => {
                    let val = self.consume_reg(a[1] as usize);
                    let token = self.heap.reset(val);
                    let f = self.frames.last_mut().unwrap();
                    let dst = a[0] as usize;
                    f.regs[dst] = Some(Value::Token(token));
                    f.states[dst] = RegState::Token;
                }
                Opcode::Reuse => {
                    let tok_reg = a[1] as usize;
                    let layout_idx = a[2];
                    let field_regs = &a[3..3 + var_count];
                    let can_fit = match self.frames.last().unwrap().regs[tok_reg].as_ref() {
                        Some(Value::Token(t)) => self.heap.can_reuse(t, var_count),
                        _ => panic!("expected ReuseToken"),
                    };
                    if !can_fit {
                        return self.oom();
                    }
                    let mut fields = Vec::with_capacity(var_count);
                    for &reg in field_regs {
                        fields.push(self.consume_reg(reg as usize));
                    }
                    let token = {
                        let f = self.frames.last_mut().unwrap();
                        f.states[tok_reg] = RegState::Moved;
                        match f.regs[tok_reg].take().expect("token register") {
                            Value::Token(t) => t,
                            _ => panic!("expected ReuseToken"),
                        }
                    };
                    match self
                        .heap
                        .reuse_owned(&self.file.types, token, layout_idx, fields)
                    {
                        Ok(val) => self.write_reg(a[0] as usize, val, false),
                        Err(_) => return self.oom(),
                    }
                }
                Opcode::FreeToken => {
                    let tok_reg = a[0] as usize;
                    let f = self.frames.last_mut().unwrap();
                    f.states[tok_reg] = RegState::Moved;
                    if let Some(Value::Token(t)) = f.regs[tok_reg].take() {
                        self.heap.free_token(t);
                    }
                }
                Opcode::Prim => {
                    let prim = PrimId::from_u32(a[1]).expect("validated prim id");
                    let sig_ty = a[2];
                    let modes = prim.info().params.as_bytes();
                    let arg_regs = &a[3..3 + var_count];
                    // Read borrowed arguments first, then temporarily take owned register
                    // values without changing register states until the call succeeds so
                    // failing primitives still expose their owned variables in defects.
                    let mut prim_args = vec![Value::Unit; var_count];
                    for (i, &reg) in arg_regs.iter().enumerate() {
                        if modes[i] != b'o' {
                            prim_args[i] = self.read_reg(reg as usize).clone();
                        }
                    }
                    {
                        let f = self.frames.last_mut().unwrap();
                        for (i, &reg) in arg_regs.iter().enumerate() {
                            if modes[i] == b'o' {
                                let r = reg as usize;
                                let ty = fn_def.register_types[r];
                                prim_args[i] = if self.file.types.is_reference(ty) {
                                    f.regs[r].take().expect("owned register holds value")
                                } else {
                                    f.regs[r]
                                        .as_ref()
                                        .expect("value register holds value")
                                        .clone()
                                };
                            }
                        }
                    }
                    let result =
                        mo_prim::call(self.heap, &self.file.types, prim, sig_ty, &mut prim_args);
                    match result {
                        Ok(val) => {
                            {
                                let f = self.frames.last_mut().unwrap();
                                for (i, &reg) in arg_regs.iter().enumerate() {
                                    if modes[i] == b'o' {
                                        let r = reg as usize;
                                        let ty = fn_def.register_types[r];
                                        if self.file.types.is_reference(ty) {
                                            f.states[r] = RegState::Moved;
                                        }
                                    }
                                }
                            }
                            self.write_reg(a[0] as usize, val, false);
                        }
                        Err(err) => {
                            {
                                let f = self.frames.last_mut().unwrap();
                                for (i, &reg) in arg_regs.iter().enumerate() {
                                    if modes[i] == b'o' {
                                        let r = reg as usize;
                                        let ty = fn_def.register_types[r];
                                        if self.file.types.is_reference(ty) && f.regs[r].is_none() {
                                            f.regs[r] = Some(std::mem::replace(
                                                &mut prim_args[i],
                                                Value::Unit,
                                            ));
                                        }
                                    }
                                }
                            }
                            match err {
                                PrimError::Overflow(kind) => {
                                    let reason = match kind {
                                        "Int" => "overflow: Int",
                                        "Decimal" => "overflow: Decimal",
                                        _ => "overflow: Duration",
                                    };
                                    return self.stop(reason, None, 0, None);
                                }
                                PrimError::DivByZero(reason)
                                | PrimError::Outside(reason)
                                | PrimError::RepeatedKey(reason) => {
                                    return self.stop(reason, None, 0, None);
                                }
                                PrimError::OutOfMemory(Some(reason))
                                    if reason.contains("MO_LIMIT_VALUE_BYTES") =>
                                {
                                    return self.stop(reason, None, 0, None);
                                }
                                PrimError::OutOfMemory(_) => return self.oom(),
                                PrimError::UnimplementedPrim(p) => {
                                    return self.stop(
                                        format!(
                                            "unimplemented primitive: {}.{}",
                                            p.info().module,
                                            p.info().name
                                        ),
                                        None,
                                        0,
                                        None,
                                    );
                                }
                            }
                        }
                    }
                }
                Opcode::Host => {
                    let host_op = HostOpId::from_u32(a[1]).expect("validated hostop id");
                    let modes = host_op.info().params.as_bytes();
                    let arg_regs = &a[2..2 + var_count];
                    let result_ty = fn_def.register_types[a[0] as usize];
                    let mut host_args = Vec::with_capacity(var_count);
                    for &reg in arg_regs {
                        host_args.push(self.read_reg(reg as usize).clone());
                    }
                    match self.host.call(
                        self.heap,
                        &self.file.types,
                        host_op,
                        result_ty,
                        &host_args,
                    ) {
                        Ok(val) => {
                            for (i, &reg) in arg_regs.iter().enumerate() {
                                if modes[i] == b'o' {
                                    let _ = self.consume_reg(reg as usize);
                                }
                            }
                            self.write_reg(a[0] as usize, val, false);
                        }
                        Err(HostError::OutOfMemory {
                            value_bytes_limit: true,
                        }) => {
                            return self.stop(
                                "running out of memory: MO_LIMIT_VALUE_BYTES",
                                None,
                                0,
                                None,
                            );
                        }
                        Err(HostError::OutOfMemory {
                            value_bytes_limit: false,
                        }) => return self.oom(),
                        Err(HostError::Bug { reason, detail }) => {
                            return self.stop(reason, None, 0, detail);
                        }
                        Err(HostError::Internal { reason }) => {
                            return self.stop(reason, None, 0, None);
                        }
                        Err(HostError::UnimplementedCapability(cap)) => {
                            return self.stop(
                                format!("unimplemented capability: {}", cap.name()),
                                None,
                                0,
                                None,
                            );
                        }
                    }
                }
                Opcode::Sim => {
                    let sim_op = SimOpId::from_u32(a[1]).expect("validated simop id");
                    let descriptor = a[2];
                    let tool = a[3];
                    let arg_regs = &a[4..4 + var_count];
                    let result_ty = fn_def.register_types[a[0] as usize];
                    let mut sim_args = Vec::with_capacity(var_count);
                    for &reg in arg_regs {
                        sim_args.push(self.read_reg(reg as usize).clone());
                    }
                    let prog = simulation::ProgramView {
                        file: self.file,
                        constants: self.constants,
                        captureless: self.captureless,
                        globals: self.globals.as_slice(),
                    };
                    match simulation::exec_sim(
                        &prog, self.heap, sim_op, descriptor, tool, result_ty, &sim_args,
                    ) {
                        Ok(val) => self.write_reg(a[0] as usize, val, false),
                        Err(report) => return self.stop(report, None, 0, None),
                    }
                }
            }

            self.frames.last_mut().unwrap().pc = next_pc;
        }
    }
}

/// Formats a runtime defect report matching `src/vm/defect.c` + `src/tools/render.c:mo_write_defect`.
pub fn render_defect(file: &BytecodeFile, defect: &Defect) -> String {
    let mut out = String::new();
    out.push_str("bug: ");
    escape_defect_text(&mut out, &defect.reason);
    if let Some(limit_name) = defect.limit_name {
        out.push_str(&format!(" ({limit_name} = {})", defect.limit));
    }
    out.push('\n');

    if let Some(top) = defect.frames.first() {
        append_frame(&mut out, file, top);
    }
    if let Some(detail) = &defect.detail {
        out.push_str("  detail: ");
        escape_defect_text(&mut out, detail);
        out.push('\n');
    }

    let total = defect.frames.len();
    for (idx, frame) in defect.frames.iter().enumerate().skip(1) {
        if total > 8 && idx >= 4 && idx < total - 4 {
            if idx == 4 {
                out.push_str(&format!("  … {} frames …\n", total - 8));
            }
            continue;
        }
        append_frame(&mut out, file, frame);
    }

    if defect.variables_unavailable {
        out.push_str("  <variables unavailable>\n");
    } else {
        for var in &defect.variables {
            out.push_str("  ");
            escape_defect_text(&mut out, &var.name);
            out.push_str(" = ");
            out.push_str(&var.value);
            out.push('\n');
        }
    }

    out
}

const DEFECT_VAR_BUDGET: usize = 4096;
const DEFECT_TRUNCATED_MARKER: &str = "… <truncated>";

fn escape_quoted_scalar(bytes: &[u8], delimiter: u8) -> (usize, String) {
    let c = bytes[0];
    let count = if c < 0x80 {
        1
    } else if c < 0xe0 {
        2
    } else if c < 0xf0 {
        3
    } else {
        4
    };
    let is_interp = c == b'#' && bytes.len() > 1 && bytes[1] == b'{';
    let is_c1 = c == 0xc2 && count == 2 && (0x80..=0x9f).contains(&bytes[1]);
    let part = match c {
        b'\n' => "\\n".to_string(),
        b'\r' => "\\r".to_string(),
        b'\t' => "\\t".to_string(),
        b'\\' => "\\\\".to_string(),
        b if b == delimiter || is_interp => format!("\\{}", b as char),
        b if b < 32 || b == 127 => format!("\\u{{{b:x}}}"),
        _ if is_c1 => format!("\\u{{{:x}}}", bytes[1]),
        _ => std::str::from_utf8(&bytes[..count]).unwrap().to_string(),
    };
    (count, part)
}

fn format_defect_quoted(s: &str, delimiter: char, budget: usize) -> (String, bool) {
    let delim_byte = delimiter as u8;
    let bytes = s.as_bytes();
    let mut total = 2usize;
    let mut clipped = false;
    let mut at = 0usize;
    while at < bytes.len() {
        let (count, part) = escape_quoted_scalar(&bytes[at..], delim_byte);
        if part.len() > budget.saturating_sub(total) {
            clipped = true;
            break;
        }
        total += part.len();
        at += count;
    }
    let reserve = 2 + if clipped {
        DEFECT_TRUNCATED_MARKER.len()
    } else {
        0
    };
    if budget < reserve {
        return ("<value unavailable>".to_string(), true);
    }
    let max_body = budget - reserve;
    let mut out = String::new();
    out.push(delimiter);
    let mut written = 0usize;
    let mut i = 0usize;
    while i < bytes.len() {
        let (count, part) = escape_quoted_scalar(&bytes[i..], delim_byte);
        if written + part.len() > max_body {
            break;
        }
        out.push_str(&part);
        written += part.len();
        i += count;
    }
    if i != bytes.len() {
        out.push_str(DEFECT_TRUNCATED_MARKER);
    }
    out.push(delimiter);
    (out, clipped)
}

fn format_defect_variable(
    types: &mo_bytecode::types::TypeTable,
    ty: u32,
    val: &Value,
) -> (String, bool) {
    match types.get(ty).kind {
        TypeKind::Text => format_defect_quoted(val.as_text(), '"', DEFECT_VAR_BUDGET),
        TypeKind::Char => {
            let mut buf = [0u8; 4];
            let s = val.as_char().encode_utf8(&mut buf);
            format_defect_quoted(s, '\'', DEFECT_VAR_BUDGET)
        }
        _ => {
            let rendered = value_print_literal(types, ty, val);
            if rendered.len() > DEFECT_VAR_BUDGET {
                ("<value unavailable>".to_string(), true)
            } else {
                (rendered, false)
            }
        }
    }
}

fn append_frame(out: &mut String, file: &BytecodeFile, frame: &DefectFrame) {
    out.push_str("  at ");
    escape_defect_text(out, file.function_name(frame.function));
    if let Some((file_idx, start, end)) = frame.span {
        out.push_str(" (");
        escape_filename(out, &file.files[file_idx as usize]);
        out.push_str(&format!(":{start}..{end})"));
    }
    out.push('\n');
}

fn escape_defect_text(out: &mut String, s: &str) {
    for ch in s.chars() {
        match ch {
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 32
                || (c as u32) == 127
                || ((c as u32) >= 0x80 && (c as u32) <= 0x9f) =>
            {
                out.push_str(&format!("\\u{{{:x}}}", c as u32));
            }
            c => out.push(c),
        }
    }
}

fn escape_filename(out: &mut String, bytes: &[u8]) {
    let mut i = 0;
    while i < bytes.len() {
        if let Ok(s) = std::str::from_utf8(&bytes[i..]) {
            escape_defect_text(out, s);
            return;
        }
        let valid_up_to = std::str::from_utf8(&bytes[i..]).unwrap_err().valid_up_to();
        if valid_up_to > 0 {
            let valid = std::str::from_utf8(&bytes[i..i + valid_up_to]).unwrap();
            escape_defect_text(out, valid);
            i += valid_up_to;
        } else {
            out.push_str(&format!("\\x{:02x}", bytes[i]));
            i += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mo_bytecode::types::TypeTable;
    use mo_bytecode::{
        AnchorExpectation, BYTECODE_FLAGS_NONE, BYTECODE_MAGIC, BYTECODE_MAJOR, BYTECODE_MINOR,
        ConstantDef, DebugEntry, FunctionDef, Header, ModuleDef, NO_INDEX, ParameterDef, TypeDef,
    };

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

    fn synthetic_file(functions: Vec<FunctionDef>, constants: Vec<ConstantDef>) -> BytecodeFile {
        let types = TypeTable::finish(
            vec![
                base_type(TypeKind::Int),  // 0
                base_type(TypeKind::Bool), // 1
                base_type(TypeKind::Text), // 2
            ],
            Vec::new(),
        )
        .unwrap();
        let debug = vec![
            DebugEntry {
                spans: Vec::new(),
                names: Vec::new(),
                text: NO_INDEX,
                file: NO_INDEX,
                start: 0,
                end: 0,
                subject: NO_INDEX,
                expectation: AnchorExpectation::NotAnchor,
                lines: Vec::new(),
            };
            functions.len()
        ];
        BytecodeFile {
            header: Header {
                magic: BYTECODE_MAGIC,
                major: BYTECODE_MAJOR,
                minor: BYTECODE_MINOR,
                flags: BYTECODE_FLAGS_NONE,
                checksum: 0,
            },
            program_checksum: 0,
            strings: vec!["Main".into(), "countdown".into(), "boom".into()],
            types,
            constants,
            globals: Vec::new(),
            init: Vec::new(),
            code_offsets: vec![0; functions.len()],
            functions,
            modules: vec![ModuleDef {
                name: 0,
                types: Vec::new(),
                functions: vec![0],
                globals: Vec::new(),
            }],
            files: Vec::new(),
            debug,
            services: Vec::new(),
        }
    }

    #[test]
    fn tail_recursion_runs_in_constant_stack_depth() {
        // fn countdown(n: Int, acc: Int) -> Int:
        //   if n == 0 { acc } else { tail_call countdown(n - 1, acc + 1) }
        let mut code = Vec::new();
        // pc=0: const r2 = 0 (const 0)
        code.extend(insn(Opcode::Const, 0, &[2, 0]));
        // pc=3: eq_int r3 = r0 == r2
        code.extend(insn(Opcode::EqInt, 0, &[3, 0, 2]));
        // pc=7: branch r3 ? 11 : 13
        code.extend(insn(Opcode::Branch, 0, &[3, 11, 13]));
        // pc=11: ret r1
        code.extend(insn(Opcode::Ret, 0, &[1]));
        // pc=13: const r4 = 1 (const 1)
        code.extend(insn(Opcode::Const, 0, &[4, 1]));
        // pc=16: sub_int r5 = r0 - r4
        code.extend(insn(Opcode::SubInt, 0, &[5, 0, 4]));
        // pc=20: add_int r6 = r1 + r4
        code.extend(insn(Opcode::AddInt, 0, &[6, 1, 4]));
        // pc=24: tail_call fn0(r5, r6)
        code.extend(insn(Opcode::TailCall, 2, &[0, 5, 6]));

        let func = FunctionDef {
            name: 1,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 0,
            capture_types: Vec::new(),
            register_types: vec![0, 0, 0, 1, 0, 0, 0],
            code,
        };
        let constants = vec![
            ConstantDef {
                ty: 0,
                value: mo_bytecode::ConstantValue::Immediate64(0i64.to_le_bytes()),
            },
            ConstantDef {
                ty: 0,
                value: mo_bytecode::ConstantValue::Immediate64(1i64.to_le_bytes()),
            },
        ];

        let file = synthetic_file(vec![func], constants);
        let limits = Limits {
            call_depth: 4,
            ..Limits::default()
        };
        let image = Image::init(file, limits).unwrap();
        let mut proc = Process::new(&image, limits, HostContext::stdio(Vec::new(), Vec::new()));
        match proc.call(0, vec![Value::Int(500), Value::Int(0)]) {
            Outcome::Returned(v) => assert_eq!(v.as_int(), 500),
            Outcome::Failed(d) | Outcome::Stopped(d) => panic!("unexpected stop: {d:?}"),
        }
    }

    #[test]
    fn non_tail_recursion_hits_max_depth_limit() {
        // fn recurse(n: Int) -> Int: call recurse(n)
        let mut code = Vec::new();
        code.extend(insn(Opcode::Call, 1, &[1, 0, 0]));
        code.extend(insn(Opcode::Ret, 0, &[1]));

        let func = FunctionDef {
            name: 1,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![ParameterDef {
                ty: 0,
                ownership: ParamOwnership::Owned,
            }],
            result_type: 0,
            capture_types: Vec::new(),
            register_types: vec![0, 0],
            code,
        };
        let file = synthetic_file(vec![func], Vec::new());
        let limits = Limits {
            call_depth: 5,
            ..Limits::default()
        };
        let image = Image::init(file, limits).unwrap();
        let mut proc = Process::new(&image, limits, HostContext::stdio(Vec::new(), Vec::new()));
        match proc.call(0, vec![Value::Int(1)]) {
            Outcome::Stopped(d) => {
                assert_eq!(d.reason, "out of stack");
                assert_eq!(d.limit_name, Some("--max-depth"));
                assert_eq!(d.limit, 5);
                assert_eq!(d.frames.len(), 5);
            }
            _ => panic!("expected --max-depth stop"),
        }
    }

    #[test]
    fn vm_reset_reuse_and_set_field_unique_vs_shared() {
        use mo_bytecode::{LayoutDef, TypeField};

        // Types: 0=Int, 1=Record Pair(a: Int, b: Int) with layout 0.
        let types = TypeTable::finish(
            vec![
                base_type(TypeKind::Int),
                TypeDef {
                    kind: TypeKind::Record,
                    parameters: Vec::new(),
                    result: NO_INDEX,
                    function: NO_INDEX,
                    capability: None,
                    name_idx: None,
                    name: Some("Pair".into()),
                    fields: vec![
                        TypeField {
                            name_idx: Some(1),
                            name: Some("a".into()),
                            ty: 0,
                        },
                        TypeField {
                            name_idx: Some(2),
                            name: Some("b".into()),
                            ty: 0,
                        },
                    ],
                    cases: Vec::new(),
                    is_reference: true,
                    has_fields: true,
                },
            ],
            vec![LayoutDef {
                kind: LayoutKind::Record,
                ty: 1,
                case_index: 0,
                function: NO_INDEX,
                fields: vec![0, 0],
            }],
        )
        .unwrap();

        // fn reuse_pair(r0: Pair owned, r1: Int) -> Pair
        //   r2 = reset r0
        //   r3 = reuse r2, layout 0, (r1, r1)
        //   r4 = set_field r3, field 1, r1
        //   ret r4
        let mut code = Vec::new();
        code.extend(insn(Opcode::Reset, 0, &[2, 0]));
        code.extend(insn(Opcode::Reuse, 2, &[3, 2, 0, 1, 1]));
        code.extend(insn(Opcode::SetField, 0, &[4, 3, 1, 1]));
        code.extend(insn(Opcode::Ret, 0, &[4]));

        let func = FunctionDef {
            name: 1,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 1,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 1,
            capture_types: Vec::new(),
            register_types: vec![1, 0, 1, 1, 1],
            code,
        };
        let file = BytecodeFile {
            header: Header {
                magic: BYTECODE_MAGIC,
                major: BYTECODE_MAJOR,
                minor: BYTECODE_MINOR,
                flags: BYTECODE_FLAGS_NONE,
                checksum: 0,
            },
            program_checksum: 0,
            strings: vec!["Main".into(), "reuse_pair".into(), "b".into()],
            types,
            constants: Vec::new(),
            globals: Vec::new(),
            init: Vec::new(),
            code_offsets: vec![0],
            functions: vec![func],
            modules: vec![ModuleDef {
                name: 0,
                types: Vec::new(),
                functions: vec![0],
                globals: Vec::new(),
            }],
            files: Vec::new(),
            debug: vec![DebugEntry {
                spans: Vec::new(),
                names: Vec::new(),
                text: NO_INDEX,
                file: NO_INDEX,
                start: 0,
                end: 0,
                subject: NO_INDEX,
                expectation: AnchorExpectation::NotAnchor,
                lines: Vec::new(),
            }],
            services: Vec::new(),
        };

        let limits = Limits::default();
        let image = Image::init(file, limits).unwrap();
        let mut proc = Process::new(&image, limits, HostContext::stdio(Vec::new(), Vec::new()));

        // Unique input: RESET+REUSE+SET_FIELD reuses the exact same heap node in place.
        let p_unique = proc
            .heap
            .construct(&image.file().types, 0, vec![Value::Int(1), Value::Int(2)])
            .unwrap();
        let addr_before = format!("{:p}", p_unique.as_heap_ref().obj());
        let peak_before = proc.heap.budget().peak();
        let out_unique = match proc.call(0, vec![p_unique, Value::Int(99)]) {
            Outcome::Returned(v) => v,
            Outcome::Failed(d) | Outcome::Stopped(d) => panic!("unexpected stop: {d:?}"),
        };
        assert_eq!(format!("{:p}", out_unique.as_heap_ref().obj()), addr_before);
        assert_eq!(proc.heap.budget().peak(), peak_before);
        assert_eq!(proc.heap.extract_field(&out_unique, 0).as_int(), 99);
        assert_eq!(proc.heap.extract_field(&out_unique, 1).as_int(), 99);

        // Shared input: RESET gets empty token, REUSE allocates a new Pair, original untouched.
        let p_shared = proc
            .heap
            .construct(&image.file().types, 0, vec![Value::Int(7), Value::Int(8)])
            .unwrap();
        let keeper = p_shared.clone();
        let out_shared = match proc.call(0, vec![p_shared, Value::Int(42)]) {
            Outcome::Returned(v) => v,
            Outcome::Failed(d) | Outcome::Stopped(d) => panic!("unexpected stop: {d:?}"),
        };
        assert!(!keeper.as_heap_ref().ptr_eq(out_shared.as_heap_ref()));
        assert_eq!(proc.heap.extract_field(&keeper, 0).as_int(), 7);
        assert_eq!(proc.heap.extract_field(&keeper, 1).as_int(), 8);
        assert_eq!(proc.heap.extract_field(&out_shared, 0).as_int(), 42);
        assert_eq!(proc.heap.extract_field(&out_shared, 1).as_int(), 42);
    }

    #[test]
    fn defect_report_uses_registers_readable_and_omits_moved_or_dropped_regs() {
        use mo_bytecode::RegisterName;

        // fn stop_test(r0: Text owned, r1: Int) -> Int
        //   r2 = move r0          // r0 is now Moved (unreadable), r2 is Owned (readable)
        //   r3 = const 0 ("boom") // r3 is Borrowed (readable)
        //   r4 = move r2          // r2 is now Moved, r4 is Owned
        //   drop r4               // r4 is now Dropped (unreadable)
        //   stop r3               // only r1 (Int) and r3 ("boom") are readable!
        let mut code = Vec::new();
        code.extend(insn(Opcode::Move, 0, &[2, 0])); // pc=0..3
        code.extend(insn(Opcode::Const, 0, &[3, 0])); // pc=3..6
        code.extend(insn(Opcode::Move, 0, &[4, 2])); // pc=6..9
        code.extend(insn(Opcode::Drop, 0, &[4])); // pc=9..11
        code.extend(insn(Opcode::Stop, 0, &[3])); // pc=11..13

        let code_end = code.len() as u32;
        let func = FunctionDef {
            name: 1,
            module: 0,
            kind: FunctionKind::Function,
            spec_code: false,
            parameters: vec![
                ParameterDef {
                    ty: 2,
                    ownership: ParamOwnership::Owned,
                },
                ParameterDef {
                    ty: 0,
                    ownership: ParamOwnership::Owned,
                },
            ],
            result_type: 0,
            capture_types: Vec::new(),
            register_types: vec![2, 0, 2, 2, 2],
            code,
        };
        let constants = vec![ConstantDef {
            ty: 2,
            value: ConstantValue::Text(2),
        }];
        let mut file = synthetic_file(vec![func], constants);
        file.strings.extend([
            "moved_in".into(),
            "live_int".into(),
            "dropped_tmp".into(),
            "msg".into(),
        ]);
        file.debug[0].names = vec![
            RegisterName {
                reg: 0,
                name: 3, // moved_in
                code_start: 0,
                code_end,
            },
            RegisterName {
                reg: 1,
                name: 4, // live_int
                code_start: 0,
                code_end,
            },
            RegisterName {
                reg: 4,
                name: 5, // dropped_tmp
                code_start: 0,
                code_end,
            },
            RegisterName {
                reg: 3,
                name: 6, // msg
                code_start: 0,
                code_end,
            },
        ];

        // Confirm direct registers_readable replay agrees with defect reporting.
        let readable = file.registers_readable(0, 11).unwrap();
        assert_eq!(readable, vec![false, true, false, true, false]);

        let limits = Limits::default();
        let image = Image::init(file, limits).unwrap();
        let mut proc = Process::new(&image, limits, HostContext::stdio(Vec::new(), Vec::new()));
        let text_arg = proc.heap.alloc_text("secret".into()).unwrap();
        match proc.call(0, vec![text_arg, Value::Int(123)]) {
            Outcome::Stopped(d) => {
                assert!(!d.variables_unavailable);
                assert_eq!(d.variables.len(), 2);
                assert_eq!(d.variables[0].name, "live_int");
                assert_eq!(d.variables[0].value, "123");
                assert_eq!(d.variables[1].name, "msg");
                assert_eq!(d.variables[1].value, "\"boom\"");
            }
            _ => panic!("expected Stop outcome"),
        }
    }
}
