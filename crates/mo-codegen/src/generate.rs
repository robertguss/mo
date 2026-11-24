//! Instruction emission, Perceus reference-counting / reuse placement, and CFG generation
//! matching `src/codegen/generate.c`.

use mo_base::Span;
use mo_bytecode::{
    CaseShape, CodeSpan, ConstantDef, ConstantValue, NO_INDEX, OPCODE_VARIABLE_SHIFT,
    ParamOwnership, RegisterName, TypeKind,
};
use mo_core::{Expr, Prim, SimOp};
use mo_def::core_ops::CoreCmpOp;
use mo_def::limits::{
    MO_LIMIT_CORE_DEPTH, MO_LIMIT_DEBUG_ENTRIES, MO_LIMIT_FILE_SIZE, MO_LIMIT_FUNCTION_CODE_WORDS,
    MO_LIMIT_FUNCTION_INSTRUCTIONS,
};
use mo_def::{ErrorCode, HostOpId, Opcode, OperandAccess, OperandKind, PrimId};

use crate::compiler::{Change, Compiler, LiveSet, NodeId, RefState};
use crate::operations::{Expansion, LiteralOp, primitive_op};
use crate::types::selector_u32;

impl<'a> Compiler<'a> {
    pub fn codegen_span(&mut self, span: Option<Span>) -> CodeSpan {
        match span {
            Some(s) => {
                if u64::from(s.end) > MO_LIMIT_FILE_SIZE {
                    self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Source span exceeds the bytecode MO_LIMIT_FILE_SIZE bound.",
                    );
                    return CodeSpan {
                        code_offset: 0,
                        file: NO_INDEX,
                        start: 0,
                        end: 0,
                    };
                }
                CodeSpan {
                    code_offset: 0,
                    file: s.file,
                    start: s.start,
                    end: s.end,
                }
            }
            None => CodeSpan {
                code_offset: 0,
                file: NO_INDEX,
                start: 0,
                end: 0,
            },
        }
    }

    pub fn debug_add(&mut self) -> bool {
        if u64::from(self.debug_entries) == MO_LIMIT_DEBUG_ENTRIES {
            return self.fail(
                ErrorCode::BytecodeTooBig,
                "Debug information exceeds MO_LIMIT_DEBUG_ENTRIES.",
            );
        }
        self.debug_entries += 1;
        true
    }

    fn set_ref_state(&mut self, func_idx: usize, r: u32, value: RefState) -> bool {
        let ty = self.bodies[func_idx].regs[r as usize].ty;
        let prev = self.bodies[func_idx].regs[r as usize].state;
        if !self.is_reference(ty) || prev == value {
            return true;
        }
        if (self.bodies[func_idx].changes.len() as u64) >= 2 * MO_LIMIT_FUNCTION_CODE_WORDS {
            return self.fail(
                ErrorCode::CodegenMemory,
                "Code generation exceeds its ownership graph limit.",
            );
        }
        self.bodies[func_idx].changes.push(Change {
            reg: r,
            previous: prev,
        });
        self.bodies[func_idx].regs[r as usize].state = value;
        true
    }

    fn consume_operand(
        &mut self,
        func_idx: usize,
        r: u32,
        access: OperandAccess,
        fixed: &[u32],
        op: Opcode,
        slot: usize,
    ) -> bool {
        let owned = match access {
            OperandAccess::Consume | OperandAccess::TokenConsume => true,
            OperandAccess::Move => self.bodies[func_idx].regs[r as usize].state == RefState::Owned,
            OperandAccess::CallParameter => {
                let callee = fixed[if op == Opcode::Call { 1 } else { 0 }] as usize;
                self.funs[callee].parameters[slot].ownership == ParamOwnership::Owned
            }
            OperandAccess::PrimParameter => {
                PrimId::from_u32(fixed[1]).is_some_and(|p| p.info().params.as_bytes()[slot] == b'o')
            }
            OperandAccess::HostParameter => HostOpId::from_u32(fixed[1])
                .is_some_and(|h| h.info().params.as_bytes()[slot] == b'o'),
            OperandAccess::None
            | OperandAccess::Write
            | OperandAccess::Read
            | OperandAccess::TokenWrite => false,
        };
        !owned || self.set_ref_state(func_idx, r, RefState::Moved)
    }

    fn transfer_states(
        &mut self,
        func_idx: usize,
        op: Opcode,
        fixed: &[u32],
        variable: &[u32],
    ) -> bool {
        let info = op.info();
        let result = match op {
            Opcode::Const | Opcode::Global => RefState::Borrowed,
            Opcode::Move => self.bodies[func_idx].regs[fixed[1] as usize].state,
            Opcode::Clear => RefState::Undefined,
            _ => RefState::Owned,
        };
        for i in 0..usize::from(info.fixed_count) {
            if info.fixed[i].kind == OperandKind::Register
                && !self.consume_operand(func_idx, fixed[i], info.fixed[i].access, fixed, op, i)
            {
                return false;
            }
        }
        if info.variable.kind == OperandKind::Register {
            for (i, &r) in variable.iter().enumerate() {
                if !self.consume_operand(func_idx, r, info.variable.access, fixed, op, i) {
                    return false;
                }
            }
        }
        for (&reg, spec) in fixed
            .iter()
            .zip(info.fixed.iter())
            .take(usize::from(info.fixed_count))
        {
            if spec.kind == OperandKind::Register {
                match spec.access {
                    OperandAccess::Write if !self.set_ref_state(func_idx, reg, result) => {
                        return false;
                    }
                    OperandAccess::TokenWrite
                        if !self.set_ref_state(func_idx, reg, RefState::Token) =>
                    {
                        return false;
                    }
                    OperandAccess::None
                    | OperandAccess::Write
                    | OperandAccess::Read
                    | OperandAccess::Consume
                    | OperandAccess::Move
                    | OperandAccess::CallParameter
                    | OperandAccess::PrimParameter
                    | OperandAccess::HostParameter
                    | OperandAccess::TokenWrite
                    | OperandAccess::TokenConsume => {}
                }
            }
        }
        true
    }

    pub fn emit(
        &mut self,
        func_idx: usize,
        source: Option<Span>,
        op: Opcode,
        fixed: &[u32],
        variable: &[u32],
    ) -> Option<u32> {
        self.span = source;
        if u64::from(self.bodies[func_idx].instructions) == MO_LIMIT_FUNCTION_INSTRUCTIONS {
            self.fail(
                ErrorCode::FunctionTooLong,
                "Function exceeds MO_LIMIT_FUNCTION_INSTRUCTIONS.",
            );
            return None;
        }
        let words_needed = 1 + fixed.len() + variable.len();
        let offset = self.bodies[func_idx].code.len() as u32;
        if (words_needed as u64) > MO_LIMIT_FUNCTION_CODE_WORDS - u64::from(offset) {
            self.fail(
                ErrorCode::FunctionTooLong,
                "Code generation exceeds the corresponding bytecode table or function limit.",
            );
            return None;
        }
        self.bodies[func_idx].instructions += 1;
        let head = u32::from(op.number()) | ((variable.len() as u32) << OPCODE_VARIABLE_SHIFT);
        let code = &mut self.bodies[func_idx].code;
        code.push(head);
        code.extend_from_slice(fixed);
        code.extend_from_slice(variable);

        if self.has_source_meta() {
            let mut current = self.codegen_span(source);
            if self.error.is_some() {
                return None;
            }
            let same = self.bodies[func_idx].spans.last().is_some_and(|prev| {
                prev.file == current.file && prev.start == current.start && prev.end == current.end
            });
            if !same {
                if (self.bodies[func_idx].spans.len() as u64) >= MO_LIMIT_DEBUG_ENTRIES {
                    self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Code generation exceeds the corresponding bytecode table or function limit.",
                    );
                    return None;
                }
                if !self.debug_add() {
                    return None;
                }
                current.code_offset = offset;
                self.bodies[func_idx].spans.push(current);
            }
        }

        if !self.transfer_states(func_idx, op, fixed, variable) {
            return None;
        }
        Some(offset)
    }

    pub fn record_named(
        &mut self,
        func_idx: usize,
        reg: u32,
        code_start: u32,
        code_end: u32,
    ) -> bool {
        let name = self.bodies[func_idx].regs[reg as usize].name;
        if name == NO_INDEX || code_start == code_end {
            return true;
        }
        if (self.bodies[func_idx].names.len() as u64) >= MO_LIMIT_DEBUG_ENTRIES {
            return self.fail(
                ErrorCode::BytecodeTooBig,
                "Code generation exceeds the corresponding bytecode table or function limit.",
            );
        }
        if !self.debug_add() {
            return false;
        }
        self.bodies[func_idx].names.push(RegisterName {
            reg,
            name,
            code_start,
            code_end,
        });
        true
    }

    pub fn owner_add(&mut self, func_idx: usize, r: u32) {
        let f = &mut self.bodies[func_idx];
        debug_assert!(!f.regs[r as usize].owned);
        let head = f.owned;
        f.regs[r as usize].owned = true;
        f.regs[r as usize].previous = NO_INDEX;
        f.regs[r as usize].next = head;
        if head != NO_INDEX {
            f.regs[head as usize].previous = r;
        }
        f.owned = r;
    }

    fn owner_remove(&mut self, func_idx: usize, r: u32) {
        let f = &mut self.bodies[func_idx];
        if !f.regs[r as usize].owned {
            return;
        }
        let prev = f.regs[r as usize].previous;
        let next = f.regs[r as usize].next;
        if prev == NO_INDEX {
            f.owned = next;
        } else {
            f.regs[prev as usize].next = next;
        }
        if next != NO_INDEX {
            f.regs[next as usize].previous = prev;
        }
        f.regs[r as usize].owned = false;
    }

    fn drop_reg(&mut self, func_idx: usize, r: u32, span: Option<Span>) -> bool {
        if r == NO_INDEX || !self.bodies[func_idx].regs[r as usize].owned {
            return true;
        }
        let op = if self.bodies[func_idx].regs[r as usize].state == RefState::Token {
            Opcode::FreeToken
        } else {
            Opcode::Drop
        };
        if self.emit(func_idx, span, op, &[r], &[]).is_none() {
            return false;
        }
        self.owner_remove(func_idx, r);
        true
    }

    fn cleanup(&mut self, func_idx: usize, span: Option<Span>) -> bool {
        while self.bodies[func_idx].owned != NO_INDEX {
            let head = self.bodies[func_idx].owned;
            if !self.drop_reg(func_idx, head, span) {
                return false;
            }
        }
        true
    }

    fn restore(&mut self, func_idx: usize, checkpoint: usize) {
        debug_assert!(checkpoint <= self.bodies[func_idx].changes.len());
        while self.bodies[func_idx].changes.len() != checkpoint {
            let Some(change) = self.bodies[func_idx].changes.pop() else {
                break;
            };
            let was_owner = matches!(change.previous, RefState::Owned | RefState::Token);
            let is_owner = self.bodies[func_idx].regs[change.reg as usize].owned;
            if is_owner && !was_owner {
                self.owner_remove(func_idx, change.reg);
            } else if !is_owner && was_owner {
                self.owner_add(func_idx, change.reg);
            }
            self.bodies[func_idx].regs[change.reg as usize].state = change.previous;
        }
    }

    fn forget_dead(
        &mut self,
        func_idx: usize,
        live: &LiveSet,
        keep: u32,
        span: Option<Span>,
        instructions: bool,
    ) -> bool {
        let ref_count = self.bodies[func_idx].references.len();
        if !self.live_charge(func_idx, (ref_count + live.items.len()) as u64) {
            return false;
        }
        let mut at = 0usize;
        for j in 0..ref_count {
            let i = self.bodies[func_idx].references[j];
            while at < live.items.len() && live.items[at] < i {
                at += 1;
            }
            let r = self.bodies[func_idx].regs[i as usize];
            if i == keep
                || (at < live.items.len() && live.items[at] == i)
                || r.protected
                || r.state == RefState::Undefined
            {
                continue;
            }
            if instructions {
                if !self.drop_reg(func_idx, i, span)
                    || self
                        .emit(func_idx, span, Opcode::Clear, &[i], &[])
                        .is_none()
                {
                    return false;
                }
            } else {
                self.owner_remove(func_idx, i);
                if !self.set_ref_state(func_idx, i, RefState::Undefined) {
                    return false;
                }
            }
        }
        true
    }

    fn normalize(
        &mut self,
        func_idx: usize,
        checkpoint: usize,
        keep: u32,
        span: Option<Span>,
        live: Option<&LiveSet>,
    ) -> bool {
        debug_assert!(checkpoint <= self.bodies[func_idx].changes.len());
        if let Some(live_set) = live
            && !self.forget_dead(func_idx, live_set, keep, span, true)
        {
            return false;
        }
        self.bodies[func_idx].mark += 1;
        let mark = self.bodies[func_idx].mark;
        let end = self.bodies[func_idx].changes.len();
        for i in checkpoint..end {
            let change = self.bodies[func_idx].changes[i];
            let r = &mut self.bodies[func_idx].regs[change.reg as usize];
            if r.mark != mark {
                r.mark = mark;
                r.goal = change.previous;
            }
        }
        for i in (checkpoint..end).rev() {
            let id = self.bodies[func_idx].changes[i].reg;
            if self.bodies[func_idx].regs[id as usize].mark != mark {
                continue;
            }
            self.bodies[func_idx].regs[id as usize].mark = 0;
            if live.is_some() && !self.live_charge(func_idx, 33) {
                return false;
            }
            let r = self.bodies[func_idx].regs[id as usize];
            let live_count = self.bodies[func_idx].live_count;
            if id == keep
                || r.protected
                || r.state == r.goal
                || live.is_some_and(|l| id < live_count && !l.has(id))
            {
                continue;
            }
            debug_assert_eq!(r.goal, RefState::Undefined);
            if matches!(r.state, RefState::Owned | RefState::Token) {
                debug_assert!(r.owned);
                if !self.drop_reg(func_idx, id, span) {
                    return false;
                }
            }
            if self
                .emit(func_idx, span, Opcode::Clear, &[id], &[])
                .is_none()
            {
                return false;
            }
        }
        true
    }

    fn merged(&mut self, func_idx: usize, r: u32) -> bool {
        let ty = self.bodies[func_idx].regs[r as usize].ty;
        if !self.is_reference(ty) {
            return true;
        }
        let owned = self.ownership[self.flow_id(func_idx, r) as usize].owned;
        if !self.set_ref_state(
            func_idx,
            r,
            if owned {
                RefState::Owned
            } else {
                RefState::Borrowed
            },
        ) {
            return false;
        }
        if owned {
            self.owner_add(func_idx, r);
        }
        true
    }

    fn copy_reg(
        &mut self,
        func_idx: usize,
        to: u32,
        from: u32,
        owned: bool,
        span: Option<Span>,
        last: bool,
    ) -> bool {
        let from_reg = self.bodies[func_idx].regs[from as usize];
        let is_ref = self.is_reference(from_reg.ty);
        let is_move = is_ref && owned && last && from_reg.owned;
        let op = if is_ref && owned && !is_move {
            Opcode::Dup
        } else {
            Opcode::Move
        };
        debug_assert!(!is_ref || owned || !from_reg.owned);
        if self.emit(func_idx, span, op, &[to, from], &[]).is_none() {
            return false;
        }
        if is_move {
            self.owner_remove(func_idx, from);
        }
        if is_ref && owned {
            self.owner_add(func_idx, to);
        }
        true
    }

    fn argument(
        &mut self,
        func_idx: usize,
        r: u32,
        owned: bool,
        span: Option<Span>,
        last: bool,
    ) -> Option<u32> {
        let reg = self.bodies[func_idx].regs[r as usize];
        if !self.is_reference(reg.ty) || !owned || (last && reg.owned) {
            return Some(r);
        }
        let tmp = self.add_register(func_idx, reg.ty, NO_INDEX)?;
        if !self.copy_reg(func_idx, tmp, r, true, span, false) {
            return None;
        }
        Some(tmp)
    }

    pub fn initialize_register(&mut self, func_idx: usize, reg: u32, depth: u32) -> bool {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        let ty = self.bodies[func_idx].regs[reg as usize].ty;
        if self.is_reference(ty) {
            return true;
        }
        let kind = self.types.items[ty as usize].kind;
        let decl = self.declaration;
        if kind == TypeKind::Range {
            let Some(field) = self.add_register(func_idx, self.range_int, NO_INDEX) else {
                return false;
            };
            if !self.initialize_register(func_idx, field, depth + 1) {
                return false;
            }
            return self
                .emit(func_idx, decl, Opcode::MakeRange, &[reg, field, field], &[])
                .is_some();
        }
        if kind == TypeKind::Choice {
            let first_fields = self.types.items[ty as usize].cases[0].fields.clone();
            let var_args = if !first_fields.is_empty() {
                let field_ty = first_fields[0].ty;
                let field = if let Some(&old) = self.bodies[func_idx].zeros.get(&field_ty) {
                    old
                } else {
                    let Some(f_reg) = self.add_register(func_idx, field_ty, NO_INDEX) else {
                        return false;
                    };
                    if !self.initialize_register(func_idx, f_reg, depth + 1) {
                        return false;
                    }
                    self.bodies[func_idx].zeros.insert(field_ty, f_reg);
                    f_reg
                };
                vec![field]
            } else {
                Vec::new()
            };
            return self
                .emit(func_idx, decl, Opcode::MakeCase, &[reg, ty, 0], &var_args)
                .is_some();
        }
        let value = if kind == TypeKind::Decimal {
            ConstantValue::Decimal128([0u8; 16])
        } else {
            ConstantValue::Immediate64([0u8; 8])
        };
        let Some(kid) = self.intern_constant(ConstantDef { ty, value }) else {
            return false;
        };
        self.emit(func_idx, decl, Opcode::Const, &[reg, kid], &[])
            .is_some()
    }

    fn emit_library(
        &mut self,
        func_idx: usize,
        row: PrimId,
        signature: u32,
        result: u32,
        args: &[u32],
        span: Option<Span>,
    ) -> bool {
        let op = primitive_op(row);
        debug_assert_eq!(
            args.len(),
            self.types.items[signature as usize].parameters.len()
        );
        for (i, &a) in args.iter().enumerate() {
            if op.params.as_bytes()[i] == b'o'
                && self.is_reference(self.bodies[func_idx].regs[a as usize].ty)
            {
                debug_assert!(self.bodies[func_idx].regs[a as usize].owned);
                self.owner_remove(func_idx, a);
            }
        }
        if self
            .emit(
                func_idx,
                span,
                Opcode::Prim,
                &[result, row as u32, signature],
                args,
            )
            .is_none()
        {
            return false;
        }
        if self.is_reference(self.bodies[func_idx].regs[result as usize].ty) {
            self.owner_add(func_idx, result);
        }
        true
    }

    fn emit_range_text(&mut self, func_idx: usize, node_id: NodeId, value: u32) -> bool {
        let n = self.node(node_id).clone();
        let b = &n.binds;
        self.emit(func_idx, n.span, Opcode::RangeStart, &[b[0], value], &[])
            .is_some()
            && self
                .emit(func_idx, n.span, Opcode::RangeEnd, &[b[1], value], &[])
                .is_some()
            && self
                .emit(func_idx, n.span, Opcode::Const, &[b[4], n.index], &[])
                .is_some()
            && self.emit_library(
                func_idx,
                PrimId::IntToText,
                n.cases[0],
                b[2],
                &[b[0]],
                n.span,
            )
            && self.emit_library(
                func_idx,
                PrimId::IntToText,
                n.cases[0],
                b[3],
                &[b[1]],
                n.span,
            )
            && self.emit_library(
                func_idx,
                PrimId::TextConcat,
                n.cases[1],
                b[5],
                &[b[2], b[4]],
                n.span,
            )
            && self.emit_library(
                func_idx,
                PrimId::TextConcat,
                n.cases[1],
                n.reg,
                &[b[5], b[3]],
                n.span,
            )
            && self.drop_reg(func_idx, b[3], n.span)
    }

    fn emit_uncons(&mut self, func_idx: usize, node_id: NodeId, list: u32) -> bool {
        let n = self.node(node_id).clone();
        let (first, head, rest, one, pair) =
            (n.binds[0], n.binds[1], n.binds[2], n.binds[3], n.binds[4]);
        if !self.emit_library(
            func_idx,
            PrimId::ListFirst,
            n.cases[0],
            first,
            &[list],
            n.span,
        ) {
            return false;
        }
        debug_assert!(
            self.bodies[func_idx].regs[list as usize].owned
                && !self.bodies[func_idx].regs[list as usize].protected
        );
        self.bodies[func_idx].regs[list as usize].protected = true;
        let checkpoint = self.bodies[func_idx].changes.len();
        let Some(branch) = self.emit(func_idx, n.span, Opcode::Switch, &[first], &[0, 0]) else {
            return false;
        };
        let arm0_pc = self.bodies[func_idx].code.len() as u32;
        self.bodies[func_idx].code[(branch + 2) as usize] = arm0_pc;
        if !self.drop_reg(func_idx, list, n.span)
            || self
                .emit(func_idx, n.span, Opcode::MakeCase, &[n.reg, n.ty, 0], &[])
                .is_none()
        {
            return false;
        }
        self.owner_add(func_idx, n.reg);
        let Some(skip) = self.emit(func_idx, n.span, Opcode::Jump, &[0], &[]) else {
            return false;
        };
        self.restore(func_idx, checkpoint);
        let arm1_pc = self.bodies[func_idx].code.len() as u32;
        self.bodies[func_idx].code[(branch + 3) as usize] = arm1_pc;
        if self
            .emit(
                func_idx,
                n.span,
                Opcode::CaseField,
                &[head, first, 1, 0],
                &[],
            )
            .is_none()
            || self
                .emit(func_idx, n.span, Opcode::Const, &[one, n.index], &[])
                .is_none()
        {
            return false;
        }
        if self.is_reference(self.bodies[func_idx].regs[head as usize].ty) {
            self.owner_add(func_idx, head);
        }
        if !self.emit_library(
            func_idx,
            PrimId::ListDrop,
            n.cases[1],
            rest,
            &[list, one],
            n.span,
        ) {
            return false;
        }
        let pair_ty = self.bodies[func_idx].regs[pair as usize].ty as usize;
        let pair_layout = self.layouts[pair_ty];
        self.owner_remove(func_idx, head);
        self.owner_remove(func_idx, rest);
        if self
            .emit(
                func_idx,
                n.span,
                Opcode::Construct,
                &[pair, pair_layout],
                &[head, rest],
            )
            .is_none()
            || self
                .emit(
                    func_idx,
                    n.span,
                    Opcode::MakeCase,
                    &[n.reg, n.ty, 1],
                    &[pair],
                )
                .is_none()
        {
            return false;
        }
        self.owner_add(func_idx, n.reg);
        if !self.normalize(func_idx, checkpoint, n.reg, n.span, None) {
            return false;
        }
        let after_pc = self.bodies[func_idx].code.len() as u32;
        self.bodies[func_idx].code[(skip + 1) as usize] = after_pc;
        self.restore(func_idx, checkpoint);
        self.bodies[func_idx].regs[list as usize].protected = false;
        self.owner_remove(func_idx, list);
        self.set_ref_state(func_idx, list, RefState::Moved)
            && self.merged(func_idx, n.reg)
            && self.drop_reg(func_idx, first, n.span)
    }

    pub fn reachable_walk(&mut self, func_idx: usize, node_id: NodeId, depth: u32) -> bool {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        let core_id = self.node(node_id).core;
        let children = self.node(node_id).children.clone();
        match self.arena.expr(core_id) {
            Expr::Join { .. } => {
                let join_idx = self.node(node_id).index as usize;
                let mut live = self.reachable_walk(func_idx, children[1], depth + 1);
                if self.bodies[func_idx].joins[join_idx].reachable {
                    live |= self.reachable_walk(func_idx, children[0], depth + 1);
                }
                live
            }
            Expr::If { .. } => {
                if !self.reachable_walk(func_idx, children[0], depth + 1) {
                    return false;
                }
                let mut live = false;
                for &arm in &children[1..] {
                    live |= self.reachable_walk(func_idx, arm, depth + 1);
                }
                live
            }
            Expr::Case { .. } => {
                if !self.reachable_walk(func_idx, children[0], depth + 1) {
                    return false;
                }
                let mut live = false;
                for &arm in &children[1..] {
                    let body = self.node(arm).children[0];
                    live |= self.reachable_walk(func_idx, body, depth + 1);
                }
                live
            }
            Expr::Jump { .. } => {
                for &child in &children {
                    if !self.reachable_walk(func_idx, child, depth + 1) {
                        return false;
                    }
                }
                let join_idx = self.node(node_id).index as usize;
                self.bodies[func_idx].joins[join_idx].reachable = true;
                false
            }
            Expr::Fail(_) | Expr::Stop(_) => {
                for &child in &children {
                    if !self.reachable_walk(func_idx, child, depth + 1) {
                        return false;
                    }
                }
                false
            }
            _ => {
                for &child in &children {
                    if !self.reachable_walk(func_idx, child, depth + 1) {
                        return false;
                    }
                }
                true
            }
        }
    }

    fn case_layout(&self, ty: u32, case_index: u32) -> u32 {
        if self.types.items[ty as usize].kind != TypeKind::Choice {
            return self.layouts[ty as usize];
        }
        if self.types.case_shape(ty, case_index) != CaseShape::Boxed {
            return NO_INDEX;
        }
        self.types
            .find_case_layout(ty, case_index)
            .unwrap_or(NO_INDEX)
    }

    fn find_token(&self, func_idx: usize, layout: u32) -> u32 {
        if layout == NO_INDEX {
            return NO_INDEX;
        }
        let f = &self.bodies[func_idx];
        let mut r = f.owned;
        while r != NO_INDEX {
            let reg = &f.regs[r as usize];
            if reg.state == RefState::Token
                && reg.token_layout == layout
                && reg.token_depth == f.branch_depth
                && !reg.protected
            {
                return r;
            }
            r = reg.next;
        }
        NO_INDEX
    }

    fn record_set_field_slot(
        &self,
        arm_id: NodeId,
        body_id: NodeId,
        record_type: u32,
        s_reg: u32,
    ) -> u32 {
        let arm = self.node(arm_id);
        let body = self.node(body_id);
        if !matches!(self.arena.expr(body.core), Expr::Con { .. })
            || body.ty != record_type
            || body.children.len() != arm.binds.len()
            || arm.binds.is_empty()
        {
            return NO_INDEX;
        }
        let mut changed = NO_INDEX;
        for (j, &child_id) in body.children.iter().enumerate() {
            let child = self.node(child_id);
            if !matches!(self.arena.expr(child.core), Expr::Var(_)) {
                return NO_INDEX;
            }
            if child.reg == arm.binds[j] {
                if !body.last[j] {
                    return NO_INDEX;
                }
                continue;
            }
            if changed != NO_INDEX || child.reg == s_reg || arm.binds.contains(&child.reg) {
                return NO_INDEX;
            }
            changed = j as u32;
        }
        changed
    }

    pub fn generate(
        &mut self,
        func_idx: usize,
        node_id: NodeId,
        tail: bool,
        depth: u32,
        live: &mut bool,
    ) -> bool {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        let core_id = self.node(node_id).core;
        let span = self.node(node_id).span;
        let reg = self.node(node_id).reg;
        let ty = self.node(node_id).ty;
        let index = self.node(node_id).index;
        self.span = span;
        *live = true;

        match self.arena.expr(core_id) {
            Expr::Lit(_) => {
                if self
                    .emit(func_idx, span, Opcode::Const, &[reg, index], &[])
                    .is_none()
                {
                    return false;
                }
            }
            Expr::Var(_) => {}
            Expr::Global(_) => {
                if self
                    .emit(func_idx, span, Opcode::Global, &[reg, index], &[])
                    .is_none()
                {
                    return false;
                }
            }
            Expr::Let { .. } => {
                let child0 = self.node(node_id).children[0];
                let child1 = self.node(node_id).children[1];
                let bind0 = self.node(node_id).binds[0];
                let last0 = self.node(node_id).last[0];
                if !self.generate(func_idx, child0, false, depth + 1, live) || !*live {
                    return self.error.is_none();
                }
                let start = self.bodies[func_idx].code.len() as u32;
                let owned = self.ownership[self.flow_id(func_idx, bind0) as usize].owned;
                let c0_reg = self.node(child0).reg;
                let c0_tmp = self.node(child0).temporary;
                if !self.copy_reg(func_idx, bind0, c0_reg, owned, span, last0)
                    || (c0_tmp && !self.drop_reg(func_idx, c0_reg, span))
                    || !self.generate(func_idx, child1, tail, depth + 1, live)
                {
                    return false;
                }
                if *live && bind0 != reg && !self.drop_reg(func_idx, bind0, span) {
                    return false;
                }
                let end = self.bodies[func_idx].code.len() as u32;
                return self.record_named(func_idx, bind0, start, end);
            }
            Expr::Lam { .. } => {
                let binds = self.node(node_id).binds.clone();
                if binds.is_empty() {
                    let Some(kid) = self.intern_constant(ConstantDef {
                        ty,
                        value: ConstantValue::Closure(index),
                    }) else {
                        return false;
                    };
                    if self
                        .emit(func_idx, span, Opcode::Const, &[reg, kid], &[])
                        .is_none()
                    {
                        return false;
                    }
                } else {
                    let last = self.node(node_id).last.clone();
                    let mut cap_regs = Vec::with_capacity(binds.len());
                    for (i, &b) in binds.iter().enumerate() {
                        let Some(r) = self.argument(func_idx, b, true, span, last[i]) else {
                            return false;
                        };
                        self.owner_remove(func_idx, r);
                        cap_regs.push(r);
                    }
                    if self
                        .emit(
                            func_idx,
                            span,
                            Opcode::MakeClosure,
                            &[reg, index],
                            &cap_regs,
                        )
                        .is_none()
                    {
                        return false;
                    }
                    self.owner_add(func_idx, reg);
                }
            }
            Expr::App { .. } => {
                let children = self.node(node_id).children.clone();
                let last = self.node(node_id).last.clone();
                for &child in &children {
                    if !self.generate(func_idx, child, false, depth + 1, live) || !*live {
                        return self.error.is_none();
                    }
                }
                let mut arg_regs = Vec::with_capacity(children.len());
                for (i, &child) in children.iter().enumerate() {
                    let c_reg = self.node(child).reg;
                    let Some(r) = self.argument(func_idx, c_reg, true, span, last[i]) else {
                        return false;
                    };
                    arg_regs.push(r);
                }
                for &r in &arg_regs {
                    self.owner_remove(func_idx, r);
                }
                if tail && !self.cleanup(func_idx, span) {
                    return false;
                }
                let emitted = if tail {
                    self.emit(
                        func_idx,
                        span,
                        Opcode::TailCallClosure,
                        &[arg_regs[0]],
                        &arg_regs[1..],
                    )
                } else {
                    self.emit(
                        func_idx,
                        span,
                        Opcode::CallClosure,
                        &[reg, arg_regs[0]],
                        &arg_regs[1..],
                    )
                };
                if emitted.is_none() {
                    return false;
                }
                *live = !tail;
                if *live {
                    if self.is_reference(ty) {
                        self.owner_add(func_idx, reg);
                    }
                    for &child in &children {
                        if self.node(child).temporary
                            && !self.drop_reg(func_idx, self.node(child).reg, span)
                        {
                            return false;
                        }
                    }
                }
            }
            Expr::Call { .. } | Expr::Con { .. } | Expr::Tuple(_) => {
                let children = self.node(node_id).children.clone();
                let last = self.node(node_id).last.clone();
                for &child in &children {
                    if !self.generate(func_idx, child, false, depth + 1, live) || !*live {
                        return self.error.is_none();
                    }
                }
                let mut arg_regs = Vec::with_capacity(children.len());
                for (i, &child) in children.iter().enumerate() {
                    let c_reg = self.node(child).reg;
                    let is_owned = self.operand_owned(node_id, i);
                    let Some(r) = self.argument(func_idx, c_reg, is_owned, span, last[i]) else {
                        return false;
                    };
                    arg_regs.push(r);
                }
                let mut fixed = [reg, index, 0];
                let mut fixed_len = 2usize;
                let mut op = Opcode::Call;
                let mut reused_token = NO_INDEX;

                match self.arena.expr(core_id) {
                    Expr::Con { .. } => {
                        let is_rec = self.types.items[ty as usize].kind == TypeKind::Record;
                        let lay = if is_rec {
                            self.layouts[ty as usize]
                        } else {
                            self.case_layout(ty, index)
                        };
                        reused_token = self.find_token(func_idx, lay);
                        if reused_token != NO_INDEX {
                            op = Opcode::Reuse;
                            fixed = [reg, reused_token, lay];
                            fixed_len = 3;
                        } else if is_rec {
                            op = Opcode::Construct;
                            fixed[1] = lay;
                        } else {
                            op = Opcode::MakeCase;
                            fixed = [reg, ty, index];
                            fixed_len = 3;
                        }
                    }
                    Expr::Tuple(_) => {
                        let lay = self.layouts[ty as usize];
                        reused_token = self.find_token(func_idx, lay);
                        if reused_token != NO_INDEX {
                            op = Opcode::Reuse;
                            fixed = [reg, reused_token, lay];
                            fixed_len = 3;
                        } else {
                            op = Opcode::Construct;
                            fixed[1] = lay;
                        }
                    }
                    _ if tail => {
                        op = Opcode::TailCall;
                        fixed[0] = index;
                        fixed_len = 1;
                        *live = false;
                    }
                    _ => {}
                }

                if reused_token != NO_INDEX {
                    self.owner_remove(func_idx, reused_token);
                }
                for (i, &r) in arg_regs.iter().enumerate() {
                    if self.operand_owned(node_id, i) {
                        self.owner_remove(func_idx, r);
                    }
                }
                if op == Opcode::TailCall && !self.cleanup(func_idx, span) {
                    return false;
                }
                if self
                    .emit(func_idx, span, op, &fixed[..fixed_len], &arg_regs)
                    .is_none()
                {
                    return false;
                }
                if *live {
                    if self.is_reference(ty) {
                        self.owner_add(func_idx, reg);
                    }
                    for &child in &children {
                        if self.node(child).temporary
                            && !self.drop_reg(func_idx, self.node(child).reg, span)
                        {
                            return false;
                        }
                    }
                }
            }
            Expr::If { .. } | Expr::Case { .. } => {
                let children = self.node(node_id).children.clone();
                if !self.generate(func_idx, children[0], false, depth + 1, live) || !*live {
                    return self.error.is_none();
                }
                let is_choice = matches!(self.arena.expr(core_id), Expr::Case { .. });
                let s_reg = self.node(children[0]).reg;
                let s_type = self.node(children[0]).ty;
                let s_tmp = self.node(children[0]).temporary;

                if is_choice && self.types.items[s_type as usize].kind == TypeKind::Record {
                    debug_assert_eq!(children.len(), 2);
                    let arm_id = children[1];
                    let body_id = self.node(arm_id).children[0];
                    let arm_binds = self.node(arm_id).binds.clone();
                    let s_info = self.bodies[func_idx].regs[s_reg as usize];
                    let can_consume = s_info.state == RefState::Owned
                        && s_info.owned
                        && !s_info.protected
                        && self.node(arm_id).scrutinee_dead;
                    if can_consume {
                        let set_slot = self.record_set_field_slot(arm_id, body_id, s_type, s_reg);
                        if set_slot != NO_INDEX {
                            let val_node = self.node(body_id).children[set_slot as usize];
                            let val_reg = self.node(val_node).reg;
                            let val_span = self.node(body_id).span;
                            let val_last = self.node(body_id).last[set_slot as usize];
                            let Some(val) =
                                self.argument(func_idx, val_reg, true, val_span, val_last)
                            else {
                                return false;
                            };
                            self.owner_remove(func_idx, s_reg);
                            self.owner_remove(func_idx, val);
                            if self
                                .emit(
                                    func_idx,
                                    span,
                                    Opcode::SetField,
                                    &[reg, s_reg, set_slot, val],
                                    &[],
                                )
                                .is_none()
                            {
                                return false;
                            }
                            self.owner_add(func_idx, reg);
                            // Fall through to tail return handling below.
                            return self.finish_tail(func_idx, node_id, tail, live);
                        }
                    }
                    for (j, &b) in arm_binds.iter().enumerate() {
                        if self
                            .emit(func_idx, span, Opcode::Field, &[b, s_reg, j as u32], &[])
                            .is_none()
                        {
                            return false;
                        }
                        if self.is_reference(self.bodies[func_idx].regs[b as usize].ty) {
                            self.owner_add(func_idx, b);
                        }
                    }
                    let mut tok = NO_INDEX;
                    let lay = self.layouts[s_type as usize];
                    if can_consume
                        && !arm_binds.is_empty()
                        && lay != NO_INDEX
                        && self.reuses_ctor(s_type, 0, body_id, 0)
                    {
                        let Some(t) = self.add_register(func_idx, s_type, NO_INDEX) else {
                            return false;
                        };
                        tok = t;
                        if self
                            .emit(func_idx, span, Opcode::Reset, &[tok, s_reg], &[])
                            .is_none()
                        {
                            return false;
                        }
                        self.owner_remove(func_idx, s_reg);
                        let bdepth = self.bodies[func_idx].branch_depth;
                        let tok_r = &mut self.bodies[func_idx].regs[tok as usize];
                        tok_r.token_layout = lay;
                        tok_r.token_depth = bdepth;
                        self.owner_add(func_idx, tok);
                    }
                    let start = self.bodies[func_idx].code.len() as u32;
                    if !self.generate(func_idx, body_id, tail, depth + 1, live) {
                        return false;
                    }
                    if *live {
                        let owned = self.ownership[self.flow_id(func_idx, reg) as usize].owned;
                        let body_reg = self.node(body_id).reg;
                        let body_tmp = self.node(body_id).temporary;
                        let last1 = self.node(node_id).last[1];
                        if !self.copy_reg(func_idx, reg, body_reg, owned, span, last1)
                            || (body_tmp && !self.drop_reg(func_idx, body_reg, span))
                            || (s_tmp && !self.drop_reg(func_idx, s_reg, span))
                        {
                            return false;
                        }
                        if tok != NO_INDEX
                            && (!self.drop_reg(func_idx, tok, span)
                                || self
                                    .emit(func_idx, span, Opcode::Clear, &[tok], &[])
                                    .is_none())
                        {
                            return false;
                        }
                        for &b in &arm_binds {
                            if !self.drop_reg(func_idx, b, span) {
                                return false;
                            }
                        }
                    }
                    let end = self.bodies[func_idx].code.len() as u32;
                    for &b in &arm_binds {
                        if !self.record_named(func_idx, b, start, end) {
                            return false;
                        }
                    }
                    return self.finish_tail(func_idx, node_id, tail, live);
                }

                let arms = children.len() - 1;
                let mut targets = vec![0u32; arms];
                let mut jumps = vec![NO_INDEX; arms];
                let at = if is_choice {
                    self.emit(func_idx, span, Opcode::Switch, &[s_reg], &targets)
                } else {
                    self.emit(func_idx, span, Opcode::Branch, &[s_reg, 0, 0], &[])
                };
                let Some(at) = at else {
                    return false;
                };

                let checkpoint = self.bodies[func_idx].changes.len();
                let mut any = false;
                let cases = self.node(node_id).cases.clone();
                let last = self.node(node_id).last.clone();
                let after = self.node(node_id).after.clone();
                self.bodies[func_idx].branch_depth += 1;
                for i in 0..arms {
                    self.restore(func_idx, checkpoint);
                    let case_id = if is_choice { cases[i] } else { i as u32 };
                    let start = self.bodies[func_idx].code.len() as u32;
                    targets[case_id as usize] = start;
                    let arm_id = children[i + 1];
                    let body_id = if is_choice {
                        self.node(arm_id).children[0]
                    } else {
                        arm_id
                    };
                    let arm_binds = if is_choice {
                        self.node(arm_id).binds.clone()
                    } else {
                        Vec::new()
                    };
                    if is_choice {
                        for (j, &b) in arm_binds.iter().enumerate() {
                            if self
                                .emit(
                                    func_idx,
                                    span,
                                    Opcode::CaseField,
                                    &[b, s_reg, case_id, j as u32],
                                    &[],
                                )
                                .is_none()
                            {
                                return false;
                            }
                            if self.is_reference(self.bodies[func_idx].regs[b as usize].ty) {
                                self.owner_add(func_idx, b);
                            }
                        }
                        let s_info = self.bodies[func_idx].regs[s_reg as usize];
                        let lay = if !arm_binds.is_empty() {
                            self.case_layout(s_type, case_id)
                        } else {
                            NO_INDEX
                        };
                        if lay != NO_INDEX
                            && s_info.state == RefState::Owned
                            && s_info.owned
                            && !s_info.protected
                            && self.node(arm_id).scrutinee_dead
                            && self.reuses_ctor(s_type, case_id, body_id, 0)
                        {
                            let Some(tok) = self.add_register(func_idx, s_type, NO_INDEX) else {
                                return false;
                            };
                            if self
                                .emit(func_idx, span, Opcode::Reset, &[tok, s_reg], &[])
                                .is_none()
                            {
                                return false;
                            }
                            self.owner_remove(func_idx, s_reg);
                            let bdepth = self.bodies[func_idx].branch_depth;
                            let tok_r = &mut self.bodies[func_idx].regs[tok as usize];
                            tok_r.token_layout = lay;
                            tok_r.token_depth = bdepth;
                            self.owner_add(func_idx, tok);
                        }
                    }
                    if !self.generate(func_idx, body_id, tail, depth + 1, live) {
                        return false;
                    }
                    if is_choice {
                        let end = self.bodies[func_idx].code.len() as u32;
                        for &b in &arm_binds {
                            if !self.record_named(func_idx, b, start, end) {
                                return false;
                            }
                        }
                    }
                    if *live {
                        any = true;
                        let owned = reg != NO_INDEX
                            && self.ownership[self.flow_id(func_idx, reg) as usize].owned;
                        let body_reg = self.node(body_id).reg;
                        if !self.copy_reg(func_idx, reg, body_reg, owned, span, last[i + 1])
                            || !self.normalize(func_idx, checkpoint, reg, span, Some(&after))
                        {
                            return false;
                        }
                        let Some(j_off) = self.emit(func_idx, span, Opcode::Jump, &[0], &[]) else {
                            return false;
                        };
                        jumps[i] = j_off;
                    }
                }
                self.bodies[func_idx].branch_depth -= 1;
                let end_pc = self.bodies[func_idx].code.len() as u32;
                let code = &mut self.bodies[func_idx].code;
                code[(at as usize) + 2..(at as usize) + 2 + arms].copy_from_slice(&targets);
                for j_off in jumps {
                    if j_off != NO_INDEX {
                        code[(j_off as usize) + 1] = end_pc;
                    }
                }
                self.restore(func_idx, checkpoint);
                if any
                    && (!self.forget_dead(func_idx, &after, reg, span, false)
                        || !self.merged(func_idx, reg))
                {
                    return false;
                }
                *live = any;
            }
            Expr::Proj { .. } => {
                let child0 = self.node(node_id).children[0];
                if !self.generate(func_idx, child0, false, depth + 1, live) || !*live {
                    return self.error.is_none();
                }
                let c0_reg = self.node(child0).reg;
                let c0_tmp = self.node(child0).temporary;
                if self
                    .emit(func_idx, span, Opcode::Field, &[reg, c0_reg, index], &[])
                    .is_none()
                {
                    return false;
                }
                if self.is_reference(ty) {
                    self.owner_add(func_idx, reg);
                }
                if c0_tmp && !self.drop_reg(func_idx, c0_reg, span) {
                    return false;
                }
            }
            Expr::Prim { prim, .. } => {
                let children = self.node(node_id).children.clone();
                let last = self.node(node_id).last.clone();
                let op = self.node(node_id).operation;
                let sig = self.node(node_id).signature;
                for &child in &children {
                    if !self.generate(func_idx, child, false, depth + 1, live) || !*live {
                        return self.error.is_none();
                    }
                }
                let mut arg_regs = Vec::with_capacity(children.len());
                for (i, &child) in children.iter().enumerate() {
                    let c_reg = self.node(child).reg;
                    let is_owned = Self::operation_owned(self.node(node_id), i);
                    let Some(r) = self.argument(func_idx, c_reg, is_owned, span, last[i]) else {
                        return false;
                    };
                    arg_regs.push(r);
                }

                if op.expansion == Expansion::RangeToText {
                    if !self.emit_range_text(func_idx, node_id, arg_regs[0]) {
                        return false;
                    }
                } else if op.expansion == Expansion::ListUncons {
                    if !self.emit_uncons(func_idx, node_id, arg_regs[0]) {
                        return false;
                    }
                } else if op.literal != LiteralOp::None {
                    let is_map = op.literal == LiteralOp::Map;
                    let width = if is_map { 2usize } else { 1usize };
                    let binds = self.node(node_id).binds.clone();
                    let mut builder = if binds.is_empty() { reg } else { binds[0] };
                    let empty_row = if is_map {
                        PrimId::MapEmpty as u32
                    } else {
                        PrimId::ListEmpty as u32
                    };
                    if self
                        .emit(
                            func_idx,
                            span,
                            Opcode::Prim,
                            &[builder, empty_row, index],
                            &[],
                        )
                        .is_none()
                    {
                        return false;
                    }
                    self.owner_add(func_idx, builder);
                    for i in 0..binds.len() {
                        let dest = if i + 1 == binds.len() {
                            reg
                        } else {
                            binds[i + 1]
                        };
                        self.owner_remove(func_idx, builder);
                        let mut call_args = Vec::with_capacity(width + 1);
                        call_args.push(builder);
                        for j in 0..width {
                            let r = arg_regs[i * width + j];
                            self.owner_remove(func_idx, r);
                            call_args.push(r);
                        }
                        if self
                            .emit(
                                func_idx,
                                span,
                                Opcode::Prim,
                                &[dest, op.row, sig],
                                &call_args,
                            )
                            .is_none()
                        {
                            return false;
                        }
                        self.owner_add(func_idx, dest);
                        builder = dest;
                    }
                } else {
                    for (i, &r) in arg_regs.iter().enumerate() {
                        if Self::operation_owned(self.node(node_id), i) {
                            self.owner_remove(func_idx, r);
                        }
                    }
                    if op.reverse {
                        debug_assert_eq!(arg_regs.len(), 2);
                        arg_regs.swap(0, 1);
                    }
                    let Some(opcode) = op.opcode else {
                        self.fail(ErrorCode::CodegenInvalid, "missing operation opcode");
                        return false;
                    };
                    match opcode {
                        Opcode::Sim => {
                            let Prim::Sim(sim) = prim else {
                                return false;
                            };
                            let (tag_num, desc, tool) = match sim {
                                SimOp::Start { descriptor } => {
                                    (0u32, selector_u32(descriptor), 0u32)
                                }
                                SimOp::Send {
                                    descriptor, tool, ..
                                } => (1u32, selector_u32(descriptor), selector_u32(tool)),
                                SimOp::Advance => (2u32, 0u32, 0u32),
                                SimOp::Crash => (3u32, 0u32, 0u32),
                                SimOp::Restart => (4u32, 0u32, 0u32),
                            };
                            if self
                                .emit(
                                    func_idx,
                                    span,
                                    Opcode::Sim,
                                    &[reg, tag_num, desc, tool],
                                    &arg_regs,
                                )
                                .is_none()
                            {
                                return false;
                            }
                        }
                        Opcode::Prim => {
                            let dst = if op.compare {
                                self.node(node_id).binds[0]
                            } else {
                                reg
                            };
                            if self
                                .emit(func_idx, span, Opcode::Prim, &[dst, op.row, sig], &arg_regs)
                                .is_none()
                            {
                                return false;
                            }
                        }
                        Opcode::Host => {
                            let dst = if op.compare {
                                self.node(node_id).binds[0]
                            } else {
                                reg
                            };
                            if self
                                .emit(func_idx, span, Opcode::Host, &[dst, op.row], &arg_regs)
                                .is_none()
                            {
                                return false;
                            }
                        }
                        Opcode::Equal => {
                            let eq_ty = self.types.items[sig as usize].parameters[0];
                            if self
                                .emit(
                                    func_idx,
                                    span,
                                    Opcode::Equal,
                                    &[reg, eq_ty, arg_regs[0], arg_regs[1]],
                                    &[],
                                )
                                .is_none()
                            {
                                return false;
                            }
                        }
                        other => {
                            let mut fixed = Vec::with_capacity(1 + arg_regs.len());
                            fixed.push(reg);
                            fixed.extend_from_slice(&arg_regs);
                            if self.emit(func_idx, span, other, &fixed, &[]).is_none() {
                                return false;
                            }
                        }
                    }
                    if op.compare {
                        let b0 = self.node(node_id).binds[0];
                        let b1 = self.node(node_id).binds[1];
                        let ord_ty = self.types.items[sig as usize].result;
                        let cmp_opcode = match prim {
                            Prim::Cmp {
                                op: CoreCmpOp::Le | CoreCmpOp::Ge,
                                ..
                            } => Opcode::LeChoice,
                            _ => Opcode::LtChoice,
                        };
                        if self
                            .emit(func_idx, span, Opcode::MakeCase, &[b1, ord_ty, 1], &[])
                            .is_none()
                            || self
                                .emit(func_idx, span, cmp_opcode, &[reg, b0, b1], &[])
                                .is_none()
                        {
                            return false;
                        }
                    }
                    if self.is_reference(ty) {
                        self.owner_add(func_idx, reg);
                    }
                }
                for &child in &children {
                    if self.node(child).temporary
                        && !self.drop_reg(func_idx, self.node(child).reg, span)
                    {
                        return false;
                    }
                }
            }
            Expr::Fail(_) => {
                let child0 = self.node(node_id).children[0];
                if !self.generate(func_idx, child0, false, depth + 1, live) || !*live {
                    return self.error.is_none();
                }
                let c0_reg = self.node(child0).reg;
                if self
                    .emit(func_idx, span, Opcode::Fail, &[c0_reg], &[])
                    .is_none()
                {
                    return false;
                }
                *live = false;
            }
            Expr::Stop(_) => {
                if self
                    .emit(func_idx, span, Opcode::Const, &[reg, index], &[])
                    .is_none()
                    || self
                        .emit(func_idx, span, Opcode::Stop, &[reg], &[])
                        .is_none()
                {
                    return false;
                }
                *live = false;
            }
            Expr::Join { .. } => {
                let Some(skip) = self.emit(func_idx, span, Opcode::Jump, &[0], &[]) else {
                    return false;
                };
                let checkpoint = self.bodies[func_idx].changes.len();
                self.bodies[func_idx].branch_depth += 1;
                let join_idx = index as usize;
                let cont_pc = self.bodies[func_idx].code.len() as u32;
                self.bodies[func_idx].joins[join_idx].offset = cont_pc;
                self.bodies[func_idx].joins[join_idx].checkpoint = checkpoint as u32;
                let join_live = self.bodies[func_idx].joins[join_idx].live.clone();
                if !self.forget_dead(func_idx, &join_live, NO_INDEX, span, false) {
                    return false;
                }
                let binds = self.node(node_id).binds.clone();
                let used = self.bodies[func_idx].joins[join_idx].used.clone();
                for (i, &b) in binds.iter().enumerate() {
                    if self.is_reference(self.bodies[func_idx].regs[b as usize].ty) {
                        if !self.set_ref_state(func_idx, b, RefState::Owned) {
                            return false;
                        }
                        self.owner_add(func_idx, b);
                        if !used[i] && !self.drop_reg(func_idx, b, span) {
                            return false;
                        }
                    }
                }
                let start = self.bodies[func_idx].code.len() as u32;
                let reachable = self.bodies[func_idx].joins[join_idx].reachable;
                let child0 = self.node(node_id).children[0];
                let child1 = self.node(node_id).children[1];
                let last0 = self.node(node_id).last[0];
                let last1 = self.node(node_id).last[1];
                let after = self.node(node_id).after.clone();
                if reachable && !self.generate(func_idx, child0, tail, depth + 1, live) {
                    return false;
                }
                let mut exit = NO_INDEX;
                let any = reachable && *live;
                if any {
                    let owned = self.ownership[self.flow_id(func_idx, reg) as usize].owned;
                    let c0_reg = self.node(child0).reg;
                    if !self.copy_reg(func_idx, reg, c0_reg, owned, span, last0)
                        || !self.normalize(func_idx, checkpoint, reg, span, Some(&after))
                    {
                        return false;
                    }
                    let Some(e_off) = self.emit(func_idx, span, Opcode::Jump, &[0], &[]) else {
                        return false;
                    };
                    exit = e_off;
                }
                let cont_end = self.bodies[func_idx].code.len() as u32;
                for &b in &binds {
                    if !self.record_named(func_idx, b, start, cont_end) {
                        return false;
                    }
                }
                self.restore(func_idx, checkpoint);
                let rest_pc = self.bodies[func_idx].code.len() as u32;
                self.bodies[func_idx].code[(skip + 1) as usize] = rest_pc;
                if !self.generate(func_idx, child1, tail, depth + 1, live) {
                    return false;
                }
                if *live {
                    let owned = self.ownership[self.flow_id(func_idx, reg) as usize].owned;
                    let c1_reg = self.node(child1).reg;
                    if !self.copy_reg(func_idx, reg, c1_reg, owned, span, last1)
                        || !self.normalize(func_idx, checkpoint, reg, span, Some(&after))
                    {
                        return false;
                    }
                }
                if exit != NO_INDEX {
                    let join_end = self.bodies[func_idx].code.len() as u32;
                    self.bodies[func_idx].code[(exit + 1) as usize] = join_end;
                }
                *live |= any;
                self.restore(func_idx, checkpoint);
                self.bodies[func_idx].branch_depth -= 1;
                if *live
                    && (!self.forget_dead(func_idx, &after, reg, span, false)
                        || !self.merged(func_idx, reg))
                {
                    return false;
                }
            }
            Expr::Jump { .. } => {
                let join_idx = index as usize;
                let target_offset = self.bodies[func_idx].joins[join_idx].offset;
                let target_checkpoint = self.bodies[func_idx].joins[join_idx].checkpoint as usize;
                let target_node = self.bodies[func_idx].joins[join_idx].node;
                let target_live = self.bodies[func_idx].joins[join_idx].live.clone();
                let target_reachable = self.bodies[func_idx].joins[join_idx].reachable;
                debug_assert_ne!(target_offset, NO_INDEX);

                let children = self.node(node_id).children.clone();
                let binds = self.node(node_id).binds.clone();
                let last = self.node(node_id).last.clone();
                let target_binds = self.node(target_node).binds.clone();

                for &child in &children {
                    if !self.generate(func_idx, child, false, depth + 1, live) || !*live {
                        return self.error.is_none();
                    }
                }
                for (i, &child) in children.iter().enumerate() {
                    let c_reg = self.node(child).reg;
                    if !self.copy_reg(func_idx, binds[i], c_reg, true, span, last[i]) {
                        return false;
                    }
                }
                for &b in &binds {
                    self.bodies[func_idx].regs[b as usize].protected = true;
                }
                if !self.normalize(
                    func_idx,
                    target_checkpoint,
                    NO_INDEX,
                    span,
                    Some(&target_live),
                ) {
                    return false;
                }
                for i in 0..binds.len() {
                    let to = target_binds[i];
                    let from = binds[i];
                    if self
                        .emit(func_idx, span, Opcode::Move, &[to, from], &[])
                        .is_none()
                    {
                        return false;
                    }
                    self.bodies[func_idx].regs[from as usize].protected = false;
                    let from_ty = self.bodies[func_idx].regs[from as usize].ty;
                    if self.is_reference(from_ty) {
                        self.owner_remove(func_idx, from);
                        self.owner_add(func_idx, to);
                        if self
                            .emit(func_idx, span, Opcode::Clear, &[from], &[])
                            .is_none()
                        {
                            return false;
                        }
                    }
                }
                debug_assert!(target_reachable);
                if self
                    .emit(func_idx, span, Opcode::Jump, &[target_offset], &[])
                    .is_none()
                {
                    return false;
                }
                *live = false;
            }
        }

        self.finish_tail(func_idx, node_id, tail, live)
    }

    fn finish_tail(
        &mut self,
        func_idx: usize,
        node_id: NodeId,
        tail: bool,
        live: &mut bool,
    ) -> bool {
        if *live && tail {
            let reg = self.node(node_id).reg;
            let span = self.node(node_id).span;
            let Some(result) = self.argument(func_idx, reg, true, span, true) else {
                return false;
            };
            self.owner_remove(func_idx, result);
            if !self.cleanup(func_idx, span)
                || self
                    .emit(func_idx, span, Opcode::Ret, &[result], &[])
                    .is_none()
            {
                return false;
            }
            *live = false;
        }
        true
    }
}
