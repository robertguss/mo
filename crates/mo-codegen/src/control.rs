//! Reachability pruning and jump-target relocation matching `src/codegen/control.c`.

use mo_bytecode::{
    CodeSpan, DebugEntry, FunctionDef, OPCODE_MASK, OPCODE_VARIABLE_SHIFT, RegisterName,
};
use mo_def::{Opcode, OperandKind};

fn instruction_width(code: &[u32], pc: usize) -> usize {
    let head = code[pc];
    let Some(info) = Opcode::find(head & OPCODE_MASK) else {
        debug_assert!(false, "invalid opcode emitted by codegen");
        return 1;
    };
    let n = 1 + usize::from(info.fixed_count) + ((head >> OPCODE_VARIABLE_SHIFT) as usize);
    debug_assert!(n <= code.len() - pc);
    n
}

/// Removes instructions unreachable from PC 0, relocates all jump targets, and projects
/// debug spans and register-name ranges onto the compacted instruction stream.
pub fn prune(function: &mut FunctionDef, debug: &mut DebugEntry) {
    let count = function.code.len();
    debug_assert!(count != 0);

    let mut reachable = vec![false; count];
    let mut queue = Vec::with_capacity(count);
    reachable[0] = true;
    queue.push(0usize);

    let mut visited = 0usize;
    let mut read_idx = 0usize;
    while read_idx < queue.len() {
        let pc = queue[read_idx];
        read_idx += 1;
        let n = instruction_width(&function.code, pc);
        let head = function.code[pc];
        let Some(info) = Opcode::find(head & OPCODE_MASK) else {
            continue;
        };
        visited += n;
        let mut visit = |target: u32| {
            let t = target as usize;
            debug_assert!(t < count);
            if !reachable[t] {
                reachable[t] = true;
                queue.push(t);
            }
        };
        match info.opcode {
            Opcode::Jump => visit(function.code[pc + 1]),
            Opcode::Branch => {
                visit(function.code[pc + 2]);
                visit(function.code[pc + 3]);
            }
            Opcode::Switch => {
                for j in 2..n {
                    visit(function.code[pc + j]);
                }
            }
            Opcode::Ret
            | Opcode::Stop
            | Opcode::Fail
            | Opcode::TailCall
            | Opcode::TailCallClosure => {}
            _ => visit((pc + n) as u32),
        }
    }

    if visited == count {
        return;
    }

    let mut offsets = vec![0u32; count + 1];
    let mut new_code = Vec::with_capacity(visited);
    let mut pc = 0usize;
    while pc < count {
        let n = instruction_width(&function.code, pc);
        offsets[pc] = new_code.len() as u32;
        if reachable[pc] {
            new_code.extend_from_slice(&function.code[pc..pc + n]);
        }
        pc += n;
    }
    offsets[count] = new_code.len() as u32;
    debug_assert_eq!(new_code.len(), visited);

    // Relocate all target operands.
    let mut out_pc = 0usize;
    while out_pc < new_code.len() {
        let head = new_code[out_pc];
        let Some(info) = Opcode::find(head & OPCODE_MASK) else {
            break;
        };
        let fixed = usize::from(info.fixed_count);
        let var = (head >> OPCODE_VARIABLE_SHIFT) as usize;
        for i in 0..fixed {
            if info.fixed[i].kind == OperandKind::Target {
                let old = new_code[out_pc + 1 + i] as usize;
                new_code[out_pc + 1 + i] = offsets[old];
            }
        }
        if info.variable.kind == OperandKind::Target {
            for i in 0..var {
                let old = new_code[out_pc + 1 + fixed + i] as usize;
                new_code[out_pc + 1 + fixed + i] = offsets[old];
            }
        }
        out_pc += 1 + fixed + var;
    }

    // Project debug spans.
    let mut new_spans: Vec<CodeSpan> = Vec::with_capacity(debug.spans.len());
    if !debug.spans.is_empty() {
        let mut source = 0usize;
        let mut old_pc = 0usize;
        while old_pc < count {
            while source + 1 < debug.spans.len()
                && (debug.spans[source + 1].code_offset as usize) <= old_pc
            {
                source += 1;
            }
            if reachable[old_pc] {
                let mut s = debug.spans[source];
                let same = new_spans.last().is_some_and(|prev| {
                    prev.file == s.file && prev.start == s.start && prev.end == s.end
                });
                if !same {
                    s.code_offset = offsets[old_pc];
                    new_spans.push(s);
                }
            }
            old_pc += instruction_width(&function.code, old_pc);
        }
    }

    // Project debug register-name ranges.
    let mut new_names: Vec<RegisterName> = Vec::with_capacity(debug.names.len());
    for &(mut n) in &debug.names {
        n.code_start = offsets[n.code_start as usize];
        n.code_end = offsets[n.code_end as usize];
        if n.code_start != n.code_end {
            new_names.push(n);
        }
    }

    function.code = new_code;
    debug.spans = new_spans;
    debug.names = new_names;
}
