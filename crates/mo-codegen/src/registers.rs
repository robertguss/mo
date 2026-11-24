//! Block-local exact-type register recycling and prologue deduplication matching
//! `src/codegen/registers.c`.

use mo_bytecode::{
    CodeSpan, DebugEntry, FunctionDef, FunctionKind, NO_INDEX, OPCODE_MASK, OPCODE_VARIABLE_SHIFT,
    ParamOwnership, RegisterName, TypeDef,
};
use mo_def::limits::{
    MO_LIMIT_CODEGEN_REGISTER_WORK, MO_LIMIT_FUNCTION_CODE_WORDS, MO_LIMIT_FUNCTION_INSTRUCTIONS,
    MO_LIMIT_FUNCTION_REGISTER_BLOCKS, MO_LIMIT_FUNCTION_REGISTERS,
};
use mo_def::{ErrorCode, HostOpId, Opcode, OperandAccess, OperandKind, PrimId};

#[derive(Clone, Copy)]
struct Interval {
    first: u32,
    last: u32,
    block: u32,
    slot: u32,
    next: u32,
    replaced: u32,
    written: u32,
    pinned: bool,
    closed: bool,
    borrowed: bool,
    prologue_needed: bool,
}

#[derive(Clone, Copy)]
struct Slot {
    ty: u32,
    next: u32,
    previous: u32,
    block: u32,
    clear_next: u32,
    initialized: bool,
    entry_written: bool,
}

impl Slot {
    const fn new(ty: u32) -> Self {
        Self {
            ty,
            next: NO_INDEX,
            previous: NO_INDEX,
            block: 0,
            clear_next: NO_INDEX,
            initialized: false,
            entry_written: false,
        }
    }
}

struct Allocation<'a> {
    types: &'a [TypeDef],
    param_ownership: &'a [Vec<ParamOwnership>],
    function: &'a FunctionDef,
    intervals: Vec<Interval>,
    slots: Vec<Slot>,
    starts: Vec<bool>,
    kept_prologue: Vec<bool>,
    incoming: Vec<u8>,
    heads: Vec<u32>,
    ends: Vec<u32>,
    offsets: Vec<u32>,
    code: Vec<u32>,
    capacity: u32,
    instructions: u32,
    block: u32,
    clear: u32,
    has_joins: bool,
    work: u64,
}

const fn is_terminal(op: Opcode) -> bool {
    matches!(
        op,
        Opcode::Ret | Opcode::Stop | Opcode::Fail | Opcode::TailCall | Opcode::TailCallClosure
    )
}

const fn is_transfer(op: Opcode) -> bool {
    is_terminal(op) || matches!(op, Opcode::Jump | Opcode::Branch | Opcode::Switch)
}

impl<'a> Allocation<'a> {
    fn charge(&mut self, work: u64) -> Result<(), ErrorCode> {
        if work > MO_LIMIT_CODEGEN_REGISTER_WORK - self.work {
            return Err(ErrorCode::CodegenRegisterWork);
        }
        self.work += work;
        Ok(())
    }

    fn width(&self, pc: u32) -> u32 {
        let head = self.function.code[pc as usize];
        let Some(info) = Opcode::find(head & OPCODE_MASK) else {
            debug_assert!(false, "invalid opcode");
            return 1;
        };
        let n = 1 + u32::from(info.fixed_count) + (head >> OPCODE_VARIABLE_SHIFT);
        debug_assert!(n <= (self.function.code.len() as u32) - pc);
        n
    }

    fn access(&self, pc: u32, i: u32) -> OperandAccess {
        let code = &self.function.code[pc as usize..];
        let Some(info) = Opcode::find(code[0] & OPCODE_MASK) else {
            debug_assert!(false, "invalid opcode");
            return OperandAccess::None;
        };
        let fixed_count = u32::from(info.fixed_count);
        if i < fixed_count {
            return info.fixed[i as usize].access;
        }
        let parameter = (i - fixed_count) as usize;
        match info.variable.access {
            OperandAccess::CallParameter => {
                let callee = code[if info.opcode == Opcode::Call { 2 } else { 1 }] as usize;
                if self.param_ownership[callee][parameter] == ParamOwnership::Owned {
                    OperandAccess::Consume
                } else {
                    OperandAccess::Read
                }
            }
            OperandAccess::PrimParameter => {
                let is_owned = PrimId::from_u32(code[2])
                    .is_some_and(|p| p.info().params.as_bytes()[parameter] == b'o');
                if is_owned {
                    OperandAccess::Consume
                } else {
                    OperandAccess::Read
                }
            }
            OperandAccess::HostParameter => {
                let is_owned = HostOpId::from_u32(code[2])
                    .is_some_and(|h| h.info().params.as_bytes()[parameter] == b'o');
                if is_owned {
                    OperandAccess::Consume
                } else {
                    OperandAccess::Read
                }
            }
            other => other,
        }
    }

    fn scan(&mut self, prologue: u32) -> Result<(), ErrorCode> {
        let code_len = self.function.code.len() as u32;
        self.starts[0] = true;
        let mut pc = 0u32;
        while pc < code_len {
            let n = self.width(pc);
            let head = self.function.code[pc as usize];
            let Some(info) = Opcode::find(head & OPCODE_MASK) else {
                return Err(ErrorCode::CodegenInvalid);
            };
            self.charge(u64::from(n))?;
            if is_transfer(info.opcode) && pc + n < code_len {
                self.starts[(pc + n) as usize] = true;
            }
            for i in 0..(n - 1) {
                let operand = if i < u32::from(info.fixed_count) {
                    info.fixed[i as usize]
                } else {
                    info.variable
                };
                if operand.kind == OperandKind::Target {
                    let target = self.function.code[(pc + 1 + i) as usize] as usize;
                    debug_assert!(target < self.function.code.len());
                    self.starts[target] = true;
                    if self.incoming[target] < 2 {
                        self.incoming[target] += 1;
                    }
                }
            }
            pc += n;
        }

        pc = 0;
        while pc < code_len {
            let n = self.width(pc);
            let head = self.function.code[pc as usize];
            let Some(info) = Opcode::find(head & OPCODE_MASK) else {
                return Err(ErrorCode::CodegenInvalid);
            };
            if !is_transfer(info.opcode)
                && pc + n < code_len
                && self.starts[(pc + n) as usize]
                && self.incoming[(pc + n) as usize] < 2
            {
                self.incoming[(pc + n) as usize] += 1;
            }
            pc += n;
        }

        self.has_joins = self.incoming[0] > 0;
        if !self.has_joins {
            for p in 1..self.function.code.len() {
                if self.starts[p] && self.incoming[p] > 1 {
                    self.has_joins = true;
                    break;
                }
            }
        }

        let mut block = 0u32;
        pc = 0;
        while pc < code_len {
            let n = self.width(pc);
            let head = self.function.code[pc as usize];
            let Some(info) = Opcode::find(head & OPCODE_MASK) else {
                return Err(ErrorCode::CodegenInvalid);
            };
            if self.starts[pc as usize] {
                block += 1;
            }
            self.charge(2 * u64::from(n))?;
            // Reads precede writes, even when the same register occurs on both sides.
            for writing in [false, true] {
                for i in 0..(n - 1) {
                    let operand = if i < u32::from(info.fixed_count) {
                        info.fixed[i as usize]
                    } else {
                        info.variable
                    };
                    if operand.kind != OperandKind::Register {
                        continue;
                    }
                    let mode = self.access(pc, i);
                    let write = matches!(mode, OperandAccess::Write | OperandAccess::TokenWrite);
                    if write != writing {
                        continue;
                    }
                    let r = self.function.code[(pc + 1 + i) as usize] as usize;
                    debug_assert!(r < self.function.register_types.len());
                    if pc < prologue {
                        if !write && self.has_joins {
                            self.intervals[r].pinned = true;
                        }
                        continue;
                    }
                    let src_borrowed = if write && info.opcode == Opcode::Move {
                        let src_r = self.function.code[(pc + 2) as usize] as usize;
                        let src = &self.intervals[src_r];
                        src.borrowed && !src.pinned && src.block == block
                    } else {
                        false
                    };
                    let is_ref = self.types[self.function.register_types[r] as usize].is_reference;
                    let v = &mut self.intervals[r];
                    if v.first == NO_INDEX {
                        v.first = pc;
                        v.written = pc + n;
                        v.block = block;
                        if !write {
                            v.pinned = true;
                        }
                    } else if v.block != block {
                        v.pinned = true;
                    }
                    if matches!(
                        mode,
                        OperandAccess::TokenWrite | OperandAccess::TokenConsume
                    ) {
                        v.pinned = true;
                    }
                    if is_ref && is_transfer(info.opcode) && !is_terminal(info.opcode) {
                        v.pinned = true;
                    }
                    v.last = pc;
                    if write {
                        v.borrowed = match info.opcode {
                            Opcode::Const | Opcode::Global => true,
                            Opcode::Move => src_borrowed,
                            _ => false,
                        };
                    }
                    v.closed = v.borrowed
                        || matches!(mode, OperandAccess::Consume | OperandAccess::Move)
                        || (write && info.opcode == Opcode::Clear);
                }
            }
            pc += n;
        }

        let inputs = self.function.parameters.len()
            + usize::from(self.function.kind == FunctionKind::Closure);
        let reg_count = self.function.register_types.len();
        self.charge(reg_count as u64)?;
        for r in 0..reg_count {
            let ty = self.function.register_types[r];
            let is_ref = self.types[ty as usize].is_reference;
            let v = &mut self.intervals[r];
            if r < inputs {
                v.pinned = true;
            }
            if v.first == NO_INDEX && !v.pinned {
                continue;
            }
            if is_ref && !v.closed {
                v.pinned = true;
            }
            if v.pinned {
                if (self.slots.len() as u64) == MO_LIMIT_FUNCTION_REGISTERS {
                    return Err(ErrorCode::TooManyRegisters);
                }
                v.slot = self.slots.len() as u32;
                self.slots.push(Slot::new(ty));
            } else {
                v.next = self.ends[v.last as usize];
                self.ends[v.last as usize] = r as u32;
            }
        }
        Ok(())
    }

    fn append(&mut self, words: &[u32]) -> Result<(), ErrorCode> {
        let count = words.len() as u32;
        self.charge(u64::from(count))?;
        if u64::from(self.instructions) == MO_LIMIT_FUNCTION_INSTRUCTIONS
            || count > self.capacity - (self.code.len() as u32)
        {
            return Err(ErrorCode::FunctionTooLong);
        }
        self.code.extend_from_slice(words);
        self.instructions += 1;
        Ok(())
    }

    fn emit_clears(&mut self) -> Result<(), ErrorCode> {
        let mut r = self.clear;
        while r != NO_INDEX {
            let prev = self.slots[r as usize].previous;
            debug_assert_ne!(prev, NO_INDEX);
            self.append(&[u32::from(Opcode::Clear.number()), prev])?;
            r = self.slots[r as usize].clear_next;
        }
        Ok(())
    }

    fn assign_slots(&mut self, prologue: u32) -> Result<(), ErrorCode> {
        let code_len = self.function.code.len() as u32;
        let mut in_entry = self.incoming[0] == 0
            && (prologue == 0 || prologue == code_len || self.incoming[prologue as usize] <= 1);
        let mut pc = prologue;
        while pc < code_len {
            let n = self.width(pc);
            let head = self.function.code[pc as usize];
            let Some(info) = Opcode::find(head & OPCODE_MASK) else {
                return Err(ErrorCode::CodegenInvalid);
            };
            self.charge(u64::from(n) + 1)?;
            if pc > prologue && self.starts[pc as usize] {
                in_entry = false;
            }
            for i in 0..(n - 1) {
                let operand = if i < u32::from(info.fixed_count) {
                    info.fixed[i as usize]
                } else {
                    info.variable
                };
                if operand.kind != OperandKind::Register {
                    continue;
                }
                let r = self.function.code[(pc + 1 + i) as usize] as usize;
                let ty = self.function.register_types[r];
                if self.intervals[r].first == pc
                    && !self.intervals[r].pinned
                    && self.intervals[r].slot == NO_INDEX
                {
                    let mut slot = self.heads[ty as usize];
                    if slot == NO_INDEX {
                        if (self.slots.len() as u64) == MO_LIMIT_FUNCTION_REGISTERS {
                            return Err(ErrorCode::TooManyRegisters);
                        }
                        slot = self.slots.len() as u32;
                        self.slots.push(Slot::new(ty));
                    } else {
                        self.heads[ty as usize] = self.slots[slot as usize].next;
                        let prev = self.slots[slot as usize].previous as usize;
                        self.intervals[prev].replaced = pc;
                    }
                    self.intervals[r].slot = slot;
                }
                let mode = self.access(pc, i);
                if in_entry
                    && mode == OperandAccess::Write
                    && info.opcode != Opcode::Clear
                    && !self.types[ty as usize].is_reference
                {
                    let slot = self.intervals[r].slot as usize;
                    debug_assert_ne!(slot as u32, NO_INDEX);
                    self.slots[slot].entry_written = true;
                }
            }
            let mut r = self.ends[pc as usize];
            while r != NO_INDEX {
                let slot = self.intervals[r as usize].slot as usize;
                let ty = self.slots[slot].ty as usize;
                self.slots[slot].previous = r;
                self.slots[slot].next = self.heads[ty];
                self.heads[ty] = slot as u32;
                r = self.intervals[r as usize].next;
            }
            if is_transfer(info.opcode) {
                in_entry = false;
            }
            pc += n;
        }

        if !self.has_joins || prologue == 0 {
            return Ok(());
        }

        // Deduplicate prologue initializers per physical immediate slot, walking backwards.
        let mut count = 0usize;
        let mut p = 0u32;
        while p < prologue {
            self.offsets[count] = p;
            count += 1;
            p += self.width(p);
        }
        self.charge(count as u64)?;
        while count != 0 {
            count -= 1;
            let pc = self.offsets[count];
            let n = self.width(pc);
            let head = self.function.code[pc as usize];
            let Some(info) = Opcode::find(head & OPCODE_MASK) else {
                return Err(ErrorCode::CodegenInvalid);
            };
            debug_assert!(info.fixed_count >= 1 && info.fixed[0].access == OperandAccess::Write);
            let r = self.function.code[(pc + 1) as usize] as usize;
            let slot = self.intervals[r].slot;
            let need = (slot != NO_INDEX
                && !self.slots[slot as usize].initialized
                && !self.slots[slot as usize].entry_written)
                || self.intervals[r].prologue_needed;
            if !need {
                continue;
            }
            debug_assert_ne!(slot, NO_INDEX);
            self.kept_prologue[pc as usize] = true;
            self.slots[slot as usize].initialized = true;
            for i in 1..(n - 1) {
                let operand = if i < u32::from(info.fixed_count) {
                    info.fixed[i as usize]
                } else {
                    info.variable
                };
                if operand.kind == OperandKind::Register {
                    let dep = self.function.code[(pc + 1 + i) as usize] as usize;
                    self.intervals[dep].prologue_needed = true;
                }
            }
        }
        Ok(())
    }

    fn rewrite(&mut self, prologue: u32) -> Result<(), ErrorCode> {
        let code_len = self.function.code.len() as u32;
        self.block = 0;
        self.clear = NO_INDEX;
        for slot in &mut self.slots {
            slot.previous = NO_INDEX;
        }

        let mut pc = 0u32;
        while pc < code_len {
            let n = self.width(pc);
            let head = self.function.code[pc as usize];
            let Some(info) = Opcode::find(head & OPCODE_MASK) else {
                return Err(ErrorCode::CodegenInvalid);
            };
            self.charge(u64::from(n) + 1)?;
            if self.starts[pc as usize] {
                self.block += 1;
                self.clear = NO_INDEX;
            }
            self.offsets[pc as usize] = self.code.len() as u32;
            if pc < prologue {
                if self.kept_prologue[pc as usize] {
                    self.append(&self.function.code[pc as usize..(pc + n) as usize])?;
                }
                pc += n;
                continue;
            }
            for i in 0..(n - 1) {
                let operand = if i < u32::from(info.fixed_count) {
                    info.fixed[i as usize]
                } else {
                    info.variable
                };
                if operand.kind != OperandKind::Register {
                    continue;
                }
                let r = self.function.code[(pc + 1 + i) as usize] as usize;
                let v = &self.intervals[r];
                if v.first != pc || v.pinned {
                    continue;
                }
                let ty = self.function.register_types[r];
                let slot = v.slot as usize;
                debug_assert_ne!(v.slot, NO_INDEX);
                if self.types[ty as usize].is_reference && self.slots[slot].block != self.block {
                    self.slots[slot].block = self.block;
                    self.slots[slot].clear_next = self.clear;
                    self.clear = slot as u32;
                }
            }
            let end = pc + n == code_len || self.starts[(pc + n) as usize];
            if end && is_transfer(info.opcode) && !is_terminal(info.opcode) {
                self.emit_clears()?;
            }
            self.append(&self.function.code[pc as usize..(pc + n) as usize])?;
            let mut r = self.ends[pc as usize];
            while r != NO_INDEX {
                let slot = self.intervals[r as usize].slot as usize;
                self.slots[slot].previous = r;
                r = self.intervals[r as usize].next;
            }
            if end && !is_transfer(info.opcode) {
                self.emit_clears()?;
            }
            pc += n;
        }
        self.offsets[code_len as usize] = self.code.len() as u32;
        Ok(())
    }
}

/// Runs block-local register recycling on `function` and updates `debug` spans and names.
pub fn allocate_registers(
    types: &[TypeDef],
    param_ownership: &[Vec<ParamOwnership>],
    function: &mut FunctionDef,
    debug: &mut DebugEntry,
    prologue: u32,
) -> Result<(), ErrorCode> {
    let registers = function.register_types.len();
    let words = function.code.len();
    let capacity_u64 = (words as u64) + 2 * (registers as u64);
    let capacity = capacity_u64.min(MO_LIMIT_FUNCTION_CODE_WORDS) as u32;

    let mut a = Allocation {
        types,
        param_ownership,
        function,
        intervals: vec![
            Interval {
                first: NO_INDEX,
                last: 0,
                block: 0,
                slot: NO_INDEX,
                next: NO_INDEX,
                replaced: words as u32,
                written: 0,
                pinned: false,
                closed: false,
                borrowed: false,
                prologue_needed: false,
            };
            registers
        ],
        slots: Vec::with_capacity(registers.min(MO_LIMIT_FUNCTION_REGISTERS as usize)),
        starts: vec![false; words],
        kept_prologue: vec![false; (prologue as usize).max(1)],
        incoming: vec![0u8; words],
        heads: vec![NO_INDEX; types.len()],
        ends: vec![NO_INDEX; words],
        offsets: vec![0u32; words + 1],
        code: Vec::with_capacity(words),
        capacity,
        instructions: 0,
        block: 0,
        clear: NO_INDEX,
        has_joins: false,
        work: 0,
    };

    a.charge((registers as u64) + 2 * (words as u64) + (types.len() as u64))?;
    a.scan(prologue)?;
    a.assign_slots(prologue)?;
    a.rewrite(prologue)?;
    a.charge(
        2 * (words as u64)
            + 2 * (a.code.len() as u64)
            + (a.slots.len() as u64)
            + (debug.spans.len() as u64)
            + 2 * (debug.names.len() as u64),
    )?;

    let mut blocks = 0u64;
    let mut pc = 0u32;
    while (pc as usize) < words {
        if a.starts[pc as usize] {
            blocks += 1;
        }
        pc += a.width(pc);
    }
    if blocks * (a.slots.len() as u64) > MO_LIMIT_FUNCTION_REGISTER_BLOCKS {
        return Err(ErrorCode::BytecodeTooBig);
    }

    let new_types: Vec<u32> = a.slots.iter().map(|s| s.ty).collect();
    let mut new_code = std::mem::take(&mut a.code);
    let mut out_pc = 0usize;
    while out_pc < new_code.len() {
        let head = new_code[out_pc];
        let Some(info) = Opcode::find(head & OPCODE_MASK) else {
            return Err(ErrorCode::CodegenInvalid);
        };
        let fixed = usize::from(info.fixed_count);
        let var = (head >> OPCODE_VARIABLE_SHIFT) as usize;
        let total = fixed + var;
        for i in 0..total {
            let operand = if i < fixed {
                info.fixed[i]
            } else {
                info.variable
            };
            if operand.kind == OperandKind::Target {
                let old = new_code[out_pc + 1 + i] as usize;
                new_code[out_pc + 1 + i] = a.offsets[old];
            } else if operand.kind == OperandKind::Register {
                let old = new_code[out_pc + 1 + i] as usize;
                let slot = a.intervals[old].slot;
                debug_assert_ne!(slot, NO_INDEX);
                new_code[out_pc + 1 + i] = slot;
            }
        }
        out_pc += 1 + total;
    }

    // Project debug spans.
    let mut new_spans: Vec<CodeSpan> = Vec::with_capacity(debug.spans.len());
    if !debug.spans.is_empty() {
        let mut source = 0usize;
        let mut old_pc = 0u32;
        while (old_pc as usize) < words {
            while source + 1 < debug.spans.len() && debug.spans[source + 1].code_offset <= old_pc {
                source += 1;
            }
            let next = old_pc + a.width(old_pc);
            if a.offsets[old_pc as usize] != a.offsets[next as usize] {
                let mut s = debug.spans[source];
                let same = new_spans.last().is_some_and(|prev| {
                    prev.file == s.file && prev.start == s.start && prev.end == s.end
                });
                if !same {
                    s.code_offset = a.offsets[old_pc as usize];
                    new_spans.push(s);
                }
            }
            old_pc = next;
        }
    }

    // Project debug register names.
    let mut new_names: Vec<RegisterName> = Vec::with_capacity(debug.names.len());
    for &(mut name) in &debug.names {
        let v = &a.intervals[name.reg as usize];
        if v.slot == NO_INDEX {
            continue;
        }
        if !v.pinned {
            if name.code_start < v.written {
                name.code_start = v.written;
            }
            if name.code_end > v.replaced {
                name.code_end = v.replaced;
            }
        }
        if name.code_start >= name.code_end {
            continue;
        }
        name.reg = v.slot;
        name.code_start = a.offsets[name.code_start as usize];
        name.code_end = a.offsets[name.code_end as usize];
        if name.code_start < name.code_end {
            new_names.push(name);
        }
    }

    let _ = a.intervals;
    function.code = new_code;
    function.register_types = new_types;
    debug.spans = new_spans;
    debug.names = new_names;
    Ok(())
}
