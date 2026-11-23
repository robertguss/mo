#![forbid(unsafe_code)]

//! Safe Rust Mo bytecode interpreter, image initialization, call frames, and defect reporting.

mod number;

use std::collections::HashMap;

use mo_bytecode::{
    BytecodeFile, ConstantValue, FunctionKind, LayoutKind, OPCODE_MASK, OPCODE_VARIABLE_SHIFT,
    ParamOwnership, TypeKind,
};
use mo_def::limits::{MO_DEFAULT_CALL_DEPTH, MO_DEFAULT_HEAP_SIZE, MO_DEFAULT_STACK_SIZE};
use mo_def::{HostOpId, Opcode, PrimId};
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
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Defect {
    pub reason: String,
    pub limit_name: Option<&'static str>,
    pub limit: usize,
    pub detail: Option<String>,
    pub frames: Vec<DefectFrame>,
    pub variables: Vec<DefectVariable>,
}

pub enum Outcome {
    Returned(Value),
    Failed(Defect),
    Stopped(Defect),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RegState {
    Undefined,
    Value,
    Owned,
    Borrowed,
    Moved,
    Token,
}

struct Frame {
    function: u32,
    pc: u32,
    destination: u32,
    resume_pc: u32,
    frame_bytes: usize,
    regs: Vec<Option<Value>>,
    states: Vec<RegState>,
}

fn frame_size_bytes(register_count: usize) -> usize {
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
                ConstantValue::Closure(fn_idx) => captureless_closures
                    .get(fn_idx)
                    .cloned()
                    .unwrap_or_else(|| {
                        let layout_idx = file
                            .types
                            .find_closure_layout(*fn_idx)
                            .expect("closure constant layout");
                        image_heap
                            .alloc_closure(layout_idx, *fn_idx, Vec::new())
                            .expect("closure allocation")
                    }),
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
            let outcome = run_init_function(&mut image, limits, fn_idx);
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
}

fn oom_defect(limit: usize) -> Defect {
    Defect {
        reason: "running out of memory".to_string(),
        limit_name: Some("--max-heap"),
        limit,
        detail: None,
        frames: Vec::new(),
        variables: Vec::new(),
    }
}

fn run_init_function(image: &mut Image, limits: Limits, fn_idx: u32) -> Outcome {
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
    };
    ctx.run()
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
        let fn_def = &self.image.file.functions[main_fn as usize];
        let mut caps_by_kind: HashMap<u32, Value> = HashMap::new();
        let mut args = Vec::with_capacity(fn_def.parameters.len());
        for p in &fn_def.parameters {
            let cap_val = if let Some(existing) = caps_by_kind.get(&p.ty) {
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
        self.call(main_fn, args)
    }

    pub fn call(&mut self, fn_idx: u32, args: Vec<Value>) -> Outcome {
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
        };
        ctx.run()
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
        },
        HostError::Internal { reason } => Defect {
            reason: reason.to_string(),
            limit_name: None,
            limit: 0,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
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
        },
        HostError::Unimplemented(op) => Defect {
            reason: format!(
                "unimplemented host operation: {}.{}",
                op.info().module,
                op.info().name
            ),
            limit_name: None,
            limit: 0,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
        },
        HostError::UnimplementedCapability(cap) => Defect {
            reason: format!("unimplemented capability: {}", cap.name()),
            limit_name: None,
            limit: 0,
            detail: None,
            frames: Vec::new(),
            variables: Vec::new(),
        },
    }
}

enum Globals<'a> {
    ReadOnly(&'a [Option<Value>]),
    Mutable(&'a mut [Option<Value>]),
}

impl Globals<'_> {
    fn get(&self, idx: usize) -> Option<&Value> {
        let slice: &[Option<Value>] = match self {
            Globals::ReadOnly(s) => s,
            Globals::Mutable(s) => s,
        };
        slice.get(idx).and_then(|v| v.as_ref())
    }

    fn set(&mut self, idx: usize, val: Value) {
        match self {
            Globals::Mutable(s) => s[idx] = Some(val),
            Globals::ReadOnly(_) => panic!("SET_GLOBAL outside init function"),
        }
    }
}

struct ExecCtx<'a> {
    file: &'a BytecodeFile,
    constants: &'a [Value],
    captureless: &'a HashMap<u32, Value>,
    globals: Globals<'a>,
    heap: &'a Heap,
    host: &'a mut HostContext,
    limits: Limits,
    frames: &'a mut Vec<Frame>,
    stack_bytes: &'a mut usize,
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
        if let Some(top) = self.frames.last() {
            let fn_def = &self.file.functions[top.function as usize];
            let dbg = &self.file.debug[top.function as usize];
            for rn in &dbg.names {
                if rn.code_start <= top.pc
                    && top.pc < rn.code_end
                    && matches!(
                        top.states[rn.reg as usize],
                        RegState::Value | RegState::Borrowed | RegState::Owned
                    )
                    && let Some(val) = &top.regs[rn.reg as usize]
                {
                    let ty = fn_def.register_types[rn.reg as usize];
                    let rendered = value_print_literal(&self.file.types, ty, val);
                    variables.push(DefectVariable {
                        name: self.file.string(rn.name).to_string(),
                        value: rendered,
                    });
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
        }
    }

    fn stop(
        &self,
        reason: impl Into<String>,
        limit_name: Option<&'static str>,
        limit: usize,
        detail: Option<String>,
    ) -> Outcome {
        Outcome::Stopped(self.build_defect(reason.into(), limit_name, limit, detail))
    }

    fn fail(&self, detail: String) -> Outcome {
        Outcome::Failed(self.build_defect("failed assertion".to_string(), None, 0, Some(detail)))
    }

    fn oom(&self) -> Outcome {
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

    fn run(&mut self) -> Outcome {
        loop {
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

            if op.number() >= Opcode::AddInt.number() && op.number() <= Opcode::RangeEnd.number() {
                let left = self.read_reg(a[1] as usize).clone();
                let right = if fixed == 3 {
                    Some(self.read_reg(a[2] as usize).clone())
                } else {
                    None
                };
                match number::eval_number(op, &left, right.as_ref()) {
                    Ok(val) => {
                        self.write_reg(a[0] as usize, val, false);
                        self.frames.last_mut().unwrap().pc = next_pc;
                        continue;
                    }
                    Err(reason) => return self.stop(reason, None, 0, None),
                }
            }

            match op {
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
                        return Outcome::Returned(ret_val);
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
                        Err(HostError::Unimplemented(op_id)) => {
                            return self.stop(
                                format!(
                                    "unimplemented host operation: {}.{}",
                                    op_id.info().module,
                                    op_id.info().name
                                ),
                                None,
                                0,
                                None,
                            );
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
                    return self.stop("unimplemented opcode: sim", None, 0, None);
                }
                _ => unreachable!("handled numeric opcodes above"),
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

    for var in &defect.variables {
        out.push_str("  ");
        escape_defect_text(&mut out, &var.name);
        out.push_str(" = ");
        out.push_str(&var.value);
        out.push('\n');
    }

    out
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
}
