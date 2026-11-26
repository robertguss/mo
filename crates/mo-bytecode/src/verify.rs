//! Full Mo Bytecode v0.3 ownership, borrow, token, known-case, and type verifier
//! (`docs/design/bytecode.md` §9, matching `src/bytecode/verify.c`,
//! `verify-instruction.c`, `verify-program.c`, and `signature.c`).

use mo_def::limits::{
    MO_LIMIT_DEF_SIGNATURE_DEPTH, MO_LIMIT_FUNCTION_REGISTER_BLOCKS, MO_LIMIT_VERIFY_WORK_PER_WORD,
};
use mo_def::{HostOpId, Opcode, OperandAccess, PrimId, SimOpId};

use crate::{
    ABILITY_EQUAL, ABILITY_HASH, BYTECODE_HEADER_SIZE, BytecodeFile, Capability, CaseShape,
    ConstantValue, ErrorKind, FunctionDef, FunctionKind, LayoutKind, LoadError, NO_INDEX,
    OPCODE_MASK, OPCODE_VARIABLE_SHIFT, ParamOwnership, TypeDef, TypeKind,
};

const PAYLOAD_OFFSET: u32 = BYTECODE_HEADER_SIZE as u32;

const STATE_UNDEFINED: u32 = 0;
const STATE_VALUE: u32 = 1;
const STATE_OWNED: u32 = 2;
const STATE_BORROWED: u32 = 3;
const STATE_MOVED: u32 = 4;
const STATE_TOKEN: u32 = 5;

const CASE_UNKNOWN: u32 = u32::MAX;
const CASE_UNREACHED: u32 = u32::MAX - 1;

struct ProgramInit {
    global_rank: Vec<u32>,
    init_rank: Vec<u32>,
    required: Vec<u32>,
}

struct Block {
    start: u32,
    end: u32,
    last: u32,
    incoming: u32,
    visited: bool,
    state: Option<Vec<u32>>,
}

#[derive(Clone, Copy)]
struct Undo {
    reg: u32,
    state: u32,
}

#[derive(Clone, Copy)]
struct WalkFrame {
    block: u32,
    edge: u32,
    mark: usize,
}

#[derive(Clone, Copy)]
struct CasePred {
    from: u32,
    edge_index: u32,
    next: u32,
}

#[derive(Clone, Copy)]
struct CaseWrite {
    block: u32,
    next: u32,
}

#[derive(Clone, Copy)]
struct CaseUse {
    block: u32,
    pc: u32,
    next: u32,
}

fn next_pc(fn_def: &FunctionDef, pc: u32) -> Result<u32, LoadError> {
    let head = fn_def.code[pc as usize];
    let Some(info) = Opcode::find(head & OPCODE_MASK) else {
        return Err(LoadError::invalid(
            0,
            "invalid instruction shape or instruction limit",
        ));
    };
    let var_count = head >> OPCODE_VARIABLE_SHIFT;
    let width = 1u32
        .checked_add(u32::from(info.fixed_count))
        .and_then(|v| v.checked_add(var_count))
        .ok_or_else(|| LoadError::invalid(0, "invalid instruction shape or instruction limit"))?;
    let end = pc
        .checked_add(width)
        .filter(|&e| (e as usize) <= fn_def.code.len())
        .ok_or_else(|| LoadError::invalid(0, "invalid instruction shape or instruction limit"))?;
    Ok(end)
}

fn is_terminal(op: Opcode) -> bool {
    matches!(
        op,
        Opcode::TailCall | Opcode::TailCallClosure | Opcode::Ret | Opcode::Stop | Opcode::Fail
    )
}

fn is_transfer(op: Opcode) -> bool {
    is_terminal(op) || matches!(op, Opcode::Jump | Opcode::Branch | Opcode::Switch)
}

fn closure_signature_matches(file: &BytecodeFile, ty: u32, func: u32) -> bool {
    let Some(t) = file.types.items.get(ty as usize) else {
        return false;
    };
    let Some(f) = file.functions.get(func as usize) else {
        return false;
    };
    if t.kind != TypeKind::Function
        || f.kind != FunctionKind::Closure
        || t.parameters.len() != f.parameters.len()
        || t.result != f.result_type
    {
        return false;
    }
    t.parameters
        .iter()
        .zip(&f.parameters)
        .all(|(&pt, p)| pt == p.ty)
}

fn cases_match_names(t: &TypeDef, names: &[&str]) -> bool {
    t.kind == TypeKind::Choice
        && t.cases.len() == names.len()
        && t.cases
            .iter()
            .zip(names)
            .all(|(c, &want)| c.name.as_str() == want)
}

fn is_ordering_type(t: &TypeDef) -> bool {
    cases_match_names(t, &["Less", "Equal", "Greater"]) && !t.has_fields
}

// ============================================================================
// Signature matching (`src/bytecode/signature.c` parity)
// ============================================================================

struct SigMatcher<'a> {
    file: &'a BytecodeFile,
    cursor: &'a [u8],
    pos: usize,
    variables: [u32; 4], // T, U, K, V
}

impl<'a> SigMatcher<'a> {
    fn new(file: &'a BytecodeFile, signature: &'a str) -> Self {
        Self {
            file,
            cursor: signature.as_bytes(),
            pos: 0,
            variables: [NO_INDEX; 4],
        }
    }

    fn skip_spaces(&mut self) {
        while self.pos < self.cursor.len() && self.cursor[self.pos] == b' ' {
            self.pos += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.cursor.get(self.pos).copied()
    }

    fn take_byte(&mut self, c: u8) -> bool {
        self.skip_spaces();
        if self.peek() == Some(c) {
            self.pos += 1;
            true
        } else {
            false
        }
    }

    fn var_slot(word: &[u8]) -> Option<usize> {
        if word.len() != 1 {
            return None;
        }
        match word[0] {
            b'T' => Some(0),
            b'U' => Some(1),
            b'K' => Some(2),
            b'V' => Some(3),
            _ => None,
        }
    }

    fn has_required_layouts(&self, index: u32) -> bool {
        let t = self.file.types.get(index);
        if t.kind == TypeKind::Tuple {
            return self.file.types.find_layout(index, NO_INDEX).is_some();
        }
        for i in 0..t.cases.len() as u32 {
            if self.file.types.case_shape(index, i) == CaseShape::Boxed
                && self.file.types.find_layout(index, i).is_none()
            {
                return false;
            }
        }
        true
    }

    fn match_type(&mut self, index: u32, depth: u64) -> bool {
        debug_assert!(depth < MO_LIMIT_DEF_SIGNATURE_DEPTH);
        let Some(t) = self.file.types.items.get(index as usize) else {
            return false;
        };
        self.skip_spaces();
        if self.peek() == Some(b'(') {
            self.pos += 1;
            if t.kind != TypeKind::Tuple || !self.has_required_layouts(index) {
                return false;
            }
            for (i, &p) in t.parameters.iter().enumerate() {
                if (i != 0 && !self.take_byte(b',')) || !self.match_type(p, depth + 1) {
                    return false;
                }
            }
            return self.take_byte(b')');
        }
        let start = self.pos;
        while let Some(c) = self.peek() {
            if c.is_ascii_alphabetic() || c == b'.' {
                self.pos += 1;
            } else {
                break;
            }
        }
        let word = &self.cursor[start..self.pos];
        if let Some(slot) = Self::var_slot(word) {
            if self.variables[slot] == NO_INDEX {
                self.variables[slot] = index;
            }
            return self.variables[slot] == index;
        }

        let builtin = match word {
            b"Unit" => Some(TypeKind::Unit),
            b"Bool" => Some(TypeKind::Bool),
            b"Byte" => Some(TypeKind::Byte),
            b"Int" => Some(TypeKind::Int),
            b"Float" => Some(TypeKind::Float),
            b"Char" => Some(TypeKind::Char),
            b"Time" => Some(TypeKind::Time),
            b"Duration" => Some(TypeKind::Duration),
            b"Decimal" => Some(TypeKind::Decimal),
            b"Range" => Some(TypeKind::Range),
            b"Text" => Some(TypeKind::Text),
            b"Bytes" => Some(TypeKind::Bytes),
            b"BigInt" => Some(TypeKind::BigInt),
            b"List" => Some(TypeKind::List),
            b"Set" => Some(TypeKind::Set),
            b"Map" => Some(TypeKind::Map),
            _ => None,
        };
        if let Some(expected_kind) = builtin {
            if t.kind != expected_kind {
                return false;
            }
            if t.parameters.is_empty() {
                return true;
            }
            if !self.take_byte(b'(') {
                return false;
            }
            for (j, &p) in t.parameters.iter().enumerate() {
                if (j != 0 && !self.take_byte(b',')) || !self.match_type(p, depth + 1) {
                    return false;
                }
            }
            return self.take_byte(b')');
        }

        for cap_id in 0..Capability::COUNT as u32 {
            if let Some(cap) = Capability::from_u32(cap_id)
                && word == cap.name().as_bytes()
            {
                return t.kind == TypeKind::Capability && t.capability == Some(cap);
            }
        }

        if word == b"Ordering" {
            return is_ordering_type(t);
        }

        if word == b"Entry" {
            if t.kind != TypeKind::Record
                || t.fields.len() != 2
                || t.fields[0].name.as_deref() != Some("name")
                || t.fields[1].name.as_deref() != Some("kind")
                || self.file.types.get(t.fields[0].ty).kind != TypeKind::Text
                || self.file.types.find_layout(index, NO_INDEX).is_none()
            {
                return false;
            }
            let k = self.file.types.get(t.fields[1].ty);
            return cases_match_names(k, &["File", "Folder", "Link", "Other"]) && !k.has_fields;
        }

        if word == b"Option" || word == b"Result" {
            let is_option = word == b"Option";
            let names: &[&str] = if is_option {
                &["None", "Some"]
            } else {
                &["Ok", "Error"]
            };
            if !cases_match_names(t, names)
                || !self.has_required_layouts(index)
                || !self.take_byte(b'(')
            {
                return false;
            }
            if is_option && !t.cases[0].fields.is_empty() {
                return false;
            }
            let start_case = usize::from(is_option);
            for i in start_case..2 {
                let c = &t.cases[i];
                if c.fields.len() != 1
                    || !c.unnamed
                    || (i == 1 && !is_option && !self.take_byte(b','))
                    || !self.match_type(c.fields[0].ty, depth + 1)
                {
                    return false;
                }
            }
            return self.take_byte(b')');
        }

        if word == b"SimulationError" {
            return cases_match_names(t, &["ServiceDown", "ToolFailed"]) && !t.has_fields;
        }

        if word == b"FileError" {
            if !cases_match_names(t, &["NotFound", "NotAllowed", "NotText", "Failed"])
                || !self.has_required_layouts(index)
            {
                return false;
            }
            for (i, c) in t.cases.iter().enumerate() {
                let want_fields = if i == 3 { 2 } else { 1 };
                if c.fields.len() != want_fields
                    || c.unnamed
                    || c.fields[0].name.as_deref() != Some("path")
                    || self.file.types.get(c.fields[0].ty).kind != TypeKind::Text
                {
                    return false;
                }
                if i == 3
                    && (c.fields[1].name.as_deref() != Some("reason")
                        || self.file.types.get(c.fields[1].ty).kind != TypeKind::Text)
                {
                    return false;
                }
            }
            return true;
        }

        if word == b"NetworkError" {
            const NAMES: [&str; 6] = [
                "UnknownHost",
                "Refused",
                "TimedOut",
                "Closed",
                "Broken",
                "Certificate",
            ];
            const FIELDS: [&[&str]; 6] = [
                &["host"],
                &["host", "port"],
                &[],
                &[],
                &["reason"],
                &["reason"],
            ];
            if !cases_match_names(t, &NAMES) || !self.has_required_layouts(index) {
                return false;
            }
            for (i, c) in t.cases.iter().enumerate() {
                if c.fields.len() != FIELDS[i].len() || c.unnamed {
                    return false;
                }
                for (j, &fname) in FIELDS[i].iter().enumerate() {
                    let want_kind = if j == 1 {
                        TypeKind::Int
                    } else {
                        TypeKind::Text
                    };
                    if c.fields[j].name.as_deref() != Some(fname)
                        || self.file.types.get(c.fields[j].ty).kind != want_kind
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        if word == b"SqliteError" {
            const NAMES: [&str; 3] = ["OutsideFolder", "NotPermitted", "SqlFailed"];
            const FIELDS: [&str; 3] = ["path", "reason", "message"];
            if !cases_match_names(t, &NAMES) || !self.has_required_layouts(index) {
                return false;
            }
            for (i, c) in t.cases.iter().enumerate() {
                if c.fields.len() != 1
                    || c.unnamed
                    || c.fields[0].name.as_deref() != Some(FIELDS[i])
                    || self.file.types.get(c.fields[0].ty).kind != TypeKind::Text
                {
                    return false;
                }
            }
            return true;
        }

        if word == b"Sqlite.Value" {
            const NAMES: [&str; 5] = ["Null", "Integer", "Real", "String", "Blob"];
            const KINDS: [TypeKind; 4] = [
                TypeKind::Int,
                TypeKind::Float,
                TypeKind::Text,
                TypeKind::Bytes,
            ];
            if !cases_match_names(t, &NAMES) || !self.has_required_layouts(index) {
                return false;
            }
            if !t.cases[0].fields.is_empty() {
                return false;
            }
            for i in 1..5 {
                let c = &t.cases[i];
                if c.fields.len() != 1
                    || !c.unnamed
                    || self.file.types.get(c.fields[0].ty).kind != KINDS[i - 1]
                {
                    return false;
                }
            }
            return true;
        }

        if word == b"Sqlite.Rows" {
            if t.kind != TypeKind::Record
                || t.fields.len() != 2
                || t.fields[0].name.as_deref() != Some("columns")
                || t.fields[1].name.as_deref() != Some("rows")
                || self.file.types.find_layout(index, NO_INDEX).is_none()
            {
                return false;
            }
            let cols = self.file.types.get(t.fields[0].ty);
            let rows = self.file.types.get(t.fields[1].ty);
            if cols.kind != TypeKind::List
                || cols.parameters.len() != 1
                || self.file.types.get(cols.parameters[0]).kind != TypeKind::Text
                || rows.kind != TypeKind::List
                || rows.parameters.len() != 1
            {
                return false;
            }
            let row = self.file.types.get(rows.parameters[0]);
            if row.kind != TypeKind::List || row.parameters.len() != 1 {
                return false;
            }
            let mut sub = SigMatcher::new(self.file, "Sqlite.Value");
            return sub.match_type(row.parameters[0], depth + 1);
        }

        false
    }
}

fn match_signature(
    file: &BytecodeFile,
    signature: &str,
    parameters: &[u32],
    result: u32,
    default_reason: &'static str,
) -> Result<(), &'static str> {
    let mut m = SigMatcher::new(file, signature);
    if m.cursor[m.pos..].starts_with(b"fn(") {
        m.pos += 3;
        for (i, &param) in parameters.iter().enumerate() {
            if (i != 0 && !m.take_byte(b',')) || !m.match_type(param, 0) {
                return Err(default_reason);
            }
        }
        if !m.take_byte(b')') {
            return Err(default_reason);
        }
        if m.take_byte(b':') {
            if !m.match_type(result, 0) {
                return Err(default_reason);
            }
        } else if file.types.get(result).kind != TypeKind::Unit {
            return Err(default_reason);
        }
    } else if !parameters.is_empty() || !m.match_type(result, 0) {
        return Err(default_reason);
    }

    m.skip_spaces();
    if m.cursor[m.pos..].starts_with(b"where ") {
        m.pos += 6;
        if m.pos >= m.cursor.len() {
            return Err(default_reason);
        }
        let var_byte = &m.cursor[m.pos..m.pos + 1];
        m.pos += 1;
        let Some(slot) = SigMatcher::var_slot(var_byte) else {
            return Err(default_reason);
        };
        if m.variables[slot] == NO_INDEX || !m.take_byte(b':') {
            return Err(default_reason);
        }
        m.skip_spaces();
        let bound_ty = m.variables[slot];
        let rest = &m.cursor[m.pos..];
        if rest == b"Equal" {
            return if (file.types.abilities(bound_ty) & ABILITY_EQUAL) != 0 {
                Ok(())
            } else {
                Err("library Equal constraint")
            };
        }
        if rest == b"Hash" {
            return if (file.types.abilities(bound_ty) & ABILITY_HASH) != 0 {
                Ok(())
            } else {
                Err("library Hash constraint")
            };
        }
        if rest == b"Number" {
            let k = file.types.get(bound_ty).kind;
            return if matches!(
                k,
                TypeKind::Int | TypeKind::BigInt | TypeKind::Decimal | TypeKind::Float
            ) {
                Ok(())
            } else {
                Err("library Number constraint")
            };
        }
        return Err("library Number constraint");
    }

    if m.pos == m.cursor.len() {
        Ok(())
    } else {
        Err(default_reason)
    }
}

// ============================================================================
// Local instruction type checking (`src/bytecode/verify-instruction.c` parity)
// ============================================================================

fn field_type(file: &BytecodeFile, ty: u32, idx: u32) -> Option<u32> {
    let t = file.types.get(ty);
    match t.kind {
        TypeKind::Record => t.fields.get(idx as usize).map(|f| f.ty),
        TypeKind::Tuple => t.parameters.get(idx as usize).copied(),
        TypeKind::Env => file
            .functions
            .get(t.function as usize)
            .and_then(|f| f.capture_types.get(idx as usize).copied()),
        _ => None,
    }
}

fn verify_instruction(
    file: &BytecodeFile,
    fn_def: &FunctionDef,
    base: u32,
    pc: u32,
) -> Result<(), LoadError> {
    let offset = base + pc * 4;
    let err = |reason: &'static str| LoadError::invalid(offset, reason);
    let head = fn_def.code[pc as usize];
    let op_num = (head & OPCODE_MASK) as u16;
    let var_count = (head >> OPCODE_VARIABLE_SHIFT) as usize;
    let op = Opcode::from_u16(op_num)
        .ok_or_else(|| err("invalid instruction shape or instruction limit"))?;
    let info = op.info();
    let fixed = info.fixed_count as usize;
    let a = &fn_def.code[pc as usize + 1..];

    let reg = |r: u32| -> u32 { fn_def.register_types[r as usize] };
    let kind = |r: u32, expected: TypeKind| -> bool { file.types.get(reg(r)).kind == expected };
    let check_args = |arg_regs: &[u32], expected: &[u32]| -> bool {
        arg_regs.len() == expected.len()
            && arg_regs.iter().zip(expected).all(|(&r, &ty)| reg(r) == ty)
    };

    match op {
        Opcode::AddInt
        | Opcode::SubInt
        | Opcode::MulInt
        | Opcode::DivInt
        | Opcode::RemInt
        | Opcode::AddDecimal
        | Opcode::SubDecimal
        | Opcode::MulDecimal
        | Opcode::DivDecimal
        | Opcode::RemDecimal
        | Opcode::AddFloat
        | Opcode::SubFloat
        | Opcode::MulFloat
        | Opcode::DivFloat
        | Opcode::RemFloat => {
            let expected = match (op.number() - Opcode::AddInt.number()) % 3 {
                0 => TypeKind::Int,
                1 => TypeKind::Decimal,
                _ => TypeKind::Float,
            };
            if !kind(a[0], expected) {
                return Err(err("arithmetic result type"));
            }
            if !kind(a[1], expected) || !kind(a[2], expected) {
                return Err(err("arithmetic argument type"));
            }
        }
        Opcode::NegInt | Opcode::NegDecimal | Opcode::NegFloat | Opcode::NegDuration => {
            let expected = match op {
                Opcode::NegInt => TypeKind::Int,
                Opcode::NegDecimal => TypeKind::Decimal,
                Opcode::NegFloat => TypeKind::Float,
                Opcode::NegDuration => TypeKind::Duration,
                _ => unreachable!(),
            };
            if !kind(a[0], expected) {
                return Err(err("negation result type"));
            }
            if !kind(a[1], expected) {
                return Err(err("negation argument type"));
            }
        }
        Opcode::EqInt
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
        | Opcode::LeChoice => {
            const KINDS: [TypeKind; 9] = [
                TypeKind::Int,
                TypeKind::Decimal,
                TypeKind::Float,
                TypeKind::Bool,
                TypeKind::Byte,
                TypeKind::Char,
                TypeKind::Time,
                TypeKind::Duration,
                TypeKind::Choice,
            ];
            let expected = KINDS[((op.number() - Opcode::EqInt.number()) % 9) as usize];
            if !kind(a[0], TypeKind::Bool) {
                return Err(err("comparison result type"));
            }
            if !kind(a[1], expected) || reg(a[1]) != reg(a[2]) {
                return Err(err("comparison argument type"));
            }
            if expected == TypeKind::Choice && file.types.get(reg(a[1])).has_fields {
                return Err(err("choice comparison has fields"));
            }
        }
        Opcode::Move | Opcode::Dup => {
            if reg(a[0]) != reg(a[1]) {
                return Err(err("move/dup type mismatch"));
            }
        }
        Opcode::Drop | Opcode::Clear | Opcode::Jump | Opcode::FreeToken => {}
        Opcode::Const => {
            let c = &file.constants[a[1] as usize];
            if reg(a[0]) != c.ty {
                return Err(err("constant result type"));
            }
            if !fn_def.spec_code
                && file.types.get(c.ty).kind == TypeKind::Function
                && let ConstantValue::Closure(cf) = c.value
                && file.functions[cf as usize].spec_code
            {
                return Err(err("spec closure constant in non-spec code"));
            }
        }
        Opcode::Global => {
            if reg(a[0]) != file.globals[a[1] as usize].ty {
                return Err(err("global result type"));
            }
        }
        Opcode::SetGlobal => {
            if fn_def.kind != FunctionKind::Init {
                return Err(err("global store outside init"));
            }
            if file.globals[a[0] as usize].ty != reg(a[1]) {
                return Err(err("global store type"));
            }
        }
        Opcode::AddDuration | Opcode::SubDuration => {
            if !kind(a[0], TypeKind::Duration) {
                return Err(err("duration arithmetic result type"));
            }
            if !kind(a[1], TypeKind::Duration) || !kind(a[2], TypeKind::Duration) {
                return Err(err("duration arithmetic argument type"));
            }
        }
        Opcode::ScaleDuration => {
            if !kind(a[0], TypeKind::Duration) {
                return Err(err("duration scale result type"));
            }
            if !kind(a[1], TypeKind::Duration) || !kind(a[2], TypeKind::Int) {
                return Err(err("duration scale argument type"));
            }
        }
        Opcode::AddTime | Opcode::SubTime => {
            if !kind(a[0], TypeKind::Time) {
                return Err(err("time arithmetic result type"));
            }
            if !kind(a[1], TypeKind::Time) || !kind(a[2], TypeKind::Duration) {
                return Err(err("time arithmetic argument type"));
            }
        }
        Opcode::DiffTime => {
            if !kind(a[0], TypeKind::Duration) {
                return Err(err("time difference result type"));
            }
            if !kind(a[1], TypeKind::Time) || !kind(a[2], TypeKind::Time) {
                return Err(err("time difference argument type"));
            }
        }
        Opcode::CmpFloat => {
            if !is_ordering_type(file.types.get(reg(a[0]))) {
                return Err(err("float comparison needs Ordering"));
            }
            if !kind(a[1], TypeKind::Float) || !kind(a[2], TypeKind::Float) {
                return Err(err("float comparison argument type"));
            }
        }
        Opcode::Not => {
            if !kind(a[0], TypeKind::Bool) {
                return Err(err("not result type"));
            }
            if !kind(a[1], TypeKind::Bool) {
                return Err(err("not argument type"));
            }
        }
        Opcode::MakeRange => {
            if !kind(a[0], TypeKind::Range) {
                return Err(err("range result type"));
            }
            if !kind(a[1], TypeKind::Int) || !kind(a[2], TypeKind::Int) {
                return Err(err("range argument type"));
            }
        }
        Opcode::RangeStart | Opcode::RangeEnd => {
            if !kind(a[0], TypeKind::Int) {
                return Err(err("range projection result type"));
            }
            if !kind(a[1], TypeKind::Range) {
                return Err(err("range projection argument type"));
            }
        }
        Opcode::Branch => {
            if !kind(a[0], TypeKind::Bool) {
                return Err(err("branch needs Bool"));
            }
        }
        Opcode::Switch => {
            if !kind(a[0], TypeKind::Choice) {
                return Err(err("switch needs choice"));
            }
            if var_count != file.types.get(reg(a[0])).cases.len() {
                return Err(err("switch target count"));
            }
        }
        Opcode::Call | Opcode::TailCall => {
            let tail = op == Opcode::TailCall;
            let target = &file.functions[a[usize::from(!tail)] as usize];
            if matches!(target.kind, FunctionKind::Closure | FunctionKind::Init) {
                return Err(err("direct call target kind"));
            }
            if !fn_def.spec_code && target.spec_code {
                return Err(err("direct call to spec code"));
            }
            if var_count != target.parameters.len() {
                return Err(err("direct call argument count"));
            }
            let expected_res = if tail { fn_def.result_type } else { reg(a[0]) };
            if expected_res != target.result_type {
                return Err(err("direct call result type"));
            }
            for (i, param) in target.parameters.iter().enumerate() {
                if reg(a[fixed + i]) != param.ty {
                    return Err(err("direct call argument type"));
                }
            }
        }
        Opcode::CallClosure | Opcode::TailCallClosure => {
            let tail = op == Opcode::TailCallClosure;
            let t = file.types.get(reg(a[usize::from(!tail)]));
            if t.kind != TypeKind::Function {
                return Err(err("closure call needs function"));
            }
            let expected_res = if tail { fn_def.result_type } else { reg(a[0]) };
            if expected_res != t.result {
                return Err(err("closure call result type"));
            }
            if var_count != t.parameters.len() {
                return Err(err("closure call argument count"));
            }
            if !check_args(&a[fixed..fixed + var_count], &t.parameters) {
                return Err(err("closure call argument type"));
            }
        }
        Opcode::Ret => {
            if reg(a[0]) != fn_def.result_type {
                return Err(err("return type"));
            }
        }
        Opcode::Fail => {
            if !fn_def.spec_code {
                return Err(err("fail outside spec code"));
            }
            if !kind(a[0], TypeKind::Text) {
                return Err(err("fail needs Text"));
            }
        }
        Opcode::Stop => {
            if !kind(a[0], TypeKind::Text) {
                return Err(err("stop needs Text"));
            }
        }
        Opcode::Construct | Opcode::Reuse => {
            let layout_idx = a[if op == Opcode::Reuse { 2 } else { 1 }];
            let layout = file.types.layout(layout_idx);
            if layout.kind == LayoutKind::Closure {
                return Err(err("construct/reuse closure layout"));
            }
            if reg(a[0]) != layout.ty {
                return Err(err("construct/reuse result type"));
            }
            if var_count != layout.fields.len() {
                return Err(err("construct/reuse field count"));
            }
            if !check_args(&a[fixed..fixed + var_count], &layout.fields) {
                return Err(err("construct/reuse field type"));
            }
        }
        Opcode::MakeCase => {
            let t = file.types.get(a[1]);
            if t.kind != TypeKind::Choice {
                return Err(err("make_case needs choice"));
            }
            if (a[2] as usize) >= t.cases.len() {
                return Err(err("make_case case index"));
            }
            if reg(a[0]) != a[1] {
                return Err(err("make_case result type"));
            }
            let c = &t.cases[a[2] as usize];
            if var_count != c.fields.len() {
                return Err(err("make_case field count"));
            }
            for (i, f) in c.fields.iter().enumerate() {
                if reg(a[3 + i]) != f.ty {
                    return Err(err("make_case field type"));
                }
            }
        }
        Opcode::Field => {
            let Some(fty) = field_type(file, reg(a[1]), a[2]) else {
                return Err(err("field source or index"));
            };
            if reg(a[0]) != fty {
                return Err(err("field result type"));
            }
        }
        Opcode::CaseField => {
            let t = file.types.get(reg(a[1]));
            if t.kind != TypeKind::Choice {
                return Err(err("case_field needs choice"));
            }
            if (a[2] as usize) >= t.cases.len() {
                return Err(err("case_field case index"));
            }
            let c = &t.cases[a[2] as usize];
            if (a[3] as usize) >= c.fields.len() {
                return Err(err("case_field field index"));
            }
            if reg(a[0]) != c.fields[a[3] as usize].ty {
                return Err(err("case_field result type"));
            }
        }
        Opcode::SetField => {
            if !kind(a[1], TypeKind::Record) && !kind(a[1], TypeKind::Tuple) {
                return Err(err("set_field needs record or tuple"));
            }
            if reg(a[0]) != reg(a[1]) {
                return Err(err("set_field result type"));
            }
            let Some(fty) = field_type(file, reg(a[1]), a[2]) else {
                return Err(err("set_field field index"));
            };
            if reg(a[3]) != fty {
                return Err(err("set_field value type"));
            }
        }
        Opcode::MakeClosure => {
            if !closure_signature_matches(file, reg(a[0]), a[1]) {
                return Err(err("closure signature"));
            }
            let target = &file.functions[a[1] as usize];
            if !fn_def.spec_code && target.spec_code {
                return Err(err("spec closure in non-spec code"));
            }
            if !check_args(&a[2..2 + var_count], &target.capture_types) {
                return Err(err("closure capture types or count"));
            }
        }
        Opcode::Equal => {
            if !kind(a[0], TypeKind::Bool) {
                return Err(err("equal result type"));
            }
            if reg(a[2]) != a[1] || reg(a[3]) != a[1] {
                return Err(err("equal argument type"));
            }
            if (file.types.abilities(a[1]) & ABILITY_EQUAL) == 0 {
                return Err(err("equal lacks Equal"));
            }
        }
        Opcode::Prim => {
            let Some(prim) = PrimId::from_u32(a[1]) else {
                return Err(err("unknown primitive operation"));
            };
            let pinfo = prim.info();
            let t = file.types.get(a[2]);
            if t.kind != TypeKind::Function {
                return Err(err("primitive needs function type"));
            }
            if reg(a[0]) != t.result {
                return Err(err("primitive result type"));
            }
            if !check_args(&a[3..3 + var_count], &t.parameters) {
                return Err(err("primitive argument types or count"));
            }
            if var_count != pinfo.params.len() {
                return Err(err("primitive arity"));
            }
            match_signature(
                file,
                pinfo.signature,
                &t.parameters,
                t.result,
                "primitive signature or constraint",
            )
            .map_err(err)?;
        }
        Opcode::Host => {
            let Some(host_op) = HostOpId::from_u32(a[1]) else {
                return Err(err("unknown host operation"));
            };
            let hinfo = host_op.info();
            if var_count != hinfo.params.len() {
                return Err(err("host arity"));
            }
            if hinfo.spec_only && !fn_def.spec_code {
                return Err(err("spec host in non-spec code"));
            }
            let arg_tys: Vec<u32> = a[2..2 + var_count].iter().map(|&r| reg(r)).collect();
            match_signature(
                file,
                hinfo.signature,
                &arg_tys,
                reg(a[0]),
                "host signature or capability",
            )
            .map_err(err)?;
        }
        Opcode::Sim => {
            let Some(sim_op) = SimOpId::from_u32(a[1]) else {
                return Err(err("unknown simulation operation"));
            };
            if sim_op == SimOpId::Start {
                if !fn_def.spec_code {
                    return Err(err("simulation start in non-spec code"));
                }
                if (a[2] as usize) >= file.services.len() || a[3] != 0 {
                    return Err(err("simulation start descriptor"));
                }
                if var_count != 1 || !kind(a[4], TypeKind::Time) {
                    return Err(err("simulation start argument"));
                }
                let r0 = file.types.get(reg(a[0]));
                if r0.kind != TypeKind::Capability || r0.capability != Some(Capability::Simulation)
                {
                    return Err(err("simulation start result"));
                }
            } else {
                if var_count == 0 {
                    return Err(err("simulation handle argument"));
                }
                let handle = file.types.get(reg(a[4]));
                if handle.kind != TypeKind::Capability
                    || handle.capability != Some(Capability::Simulation)
                {
                    return Err(err("simulation handle argument"));
                }
                if sim_op == SimOpId::Send {
                    if (a[2] as usize) >= file.services.len() {
                        return Err(err("simulation send descriptor"));
                    }
                    let service = &file.services[a[2] as usize];
                    if (a[3] as usize) >= service.tools.len() {
                        return Err(err("simulation send tool"));
                    }
                    let tool = &service.tools[a[3] as usize];
                    if !check_args(&a[5..4 + var_count], &tool.argument_types) {
                        return Err(err("simulation send argument types or count"));
                    }
                    match_signature(
                        file,
                        "fn(T) : Result(T, SimulationError)",
                        &[tool.result_type],
                        reg(a[0]),
                        "simulation send result",
                    )
                    .map_err(err)?;
                } else {
                    if a[2] != 0 || a[3] != 0 {
                        return Err(err("simulation control has selector indices"));
                    }
                    if !kind(a[0], TypeKind::Unit) {
                        return Err(err("simulation control result"));
                    }
                    let args_ok = if sim_op == SimOpId::Advance {
                        var_count == 2 && kind(a[5], TypeKind::Duration)
                    } else {
                        var_count == 1
                    };
                    if !args_ok {
                        return Err(err("simulation control arguments"));
                    }
                }
            }
        }
        Opcode::Reset => {
            if !kind(a[1], TypeKind::Record)
                && !kind(a[1], TypeKind::Tuple)
                && !kind(a[1], TypeKind::Choice)
            {
                return Err(err("reset source type"));
            }
        }
    }

    Ok(())
}

// ============================================================================
// Whole-program initializer dependency summaries (`src/bytecode/verify-program.c`)
// ============================================================================

struct CallGraph {
    from: Vec<u32>,
    to: Vec<u32>,
    next: Vec<u32>,
    reverse: Vec<u32>,
    heads: Vec<u32>,
    rev_heads: Vec<u32>,
}

impl CallGraph {
    fn new(nodes: usize) -> Self {
        Self {
            from: Vec::new(),
            to: Vec::new(),
            next: Vec::new(),
            reverse: Vec::new(),
            heads: vec![NO_INDEX; nodes],
            rev_heads: vec![NO_INDEX; nodes],
        }
    }

    fn add(&mut self, u: u32, v: u32) {
        let idx = self.from.len() as u32;
        self.from.push(u);
        self.to.push(v);
        self.next.push(self.heads[u as usize]);
        self.reverse.push(self.rev_heads[v as usize]);
        self.heads[u as usize] = idx;
        self.rev_heads[v as usize] = idx;
    }
}

/// Iterative Kosaraju SCC condensation (`src/bytecode/verify-program.c:summaries`).
fn compute_required_summaries(g: &mut CallGraph, nodes: usize, required: &mut [u32]) {
    let mut stack = vec![0u32; nodes];
    let mut cursor = vec![0u32; nodes];
    let mut order = vec![0u32; nodes];
    let mut seen = vec![false; nodes];
    let mut count = 0usize;

    for root in 0..nodes as u32 {
        if seen[root as usize] {
            continue;
        }
        let mut depth = 1usize;
        stack[0] = root;
        cursor[0] = g.heads[root as usize];
        seen[root as usize] = true;
        while depth != 0 {
            let e = cursor[depth - 1];
            if e == NO_INDEX {
                depth -= 1;
                order[count] = stack[depth];
                count += 1;
                continue;
            }
            cursor[depth - 1] = g.next[e as usize];
            let child = g.to[e as usize];
            if !seen[child as usize] {
                seen[child as usize] = true;
                stack[depth] = child;
                cursor[depth] = g.heads[child as usize];
                depth += 1;
            }
        }
    }

    let mut component = vec![NO_INDEX; nodes];
    let mut maximum = vec![0u32; nodes];
    let mut components = 0u32;

    for i in (0..count).rev() {
        let root = order[i];
        if component[root as usize] != NO_INDEX {
            continue;
        }
        let mut depth = 1usize;
        stack[0] = root;
        component[root as usize] = components;
        while depth != 0 {
            depth -= 1;
            let node = stack[depth];
            if maximum[components as usize] < required[node as usize] {
                maximum[components as usize] = required[node as usize];
            }
            let mut e = g.rev_heads[node as usize];
            while e != NO_INDEX {
                let child = g.from[e as usize];
                if component[child as usize] == NO_INDEX {
                    component[child as usize] = components;
                    stack[depth] = child;
                    depth += 1;
                }
                e = g.reverse[e as usize];
            }
        }
        components += 1;
    }

    let mut dag_heads = vec![NO_INDEX; components as usize];
    let mut dag_to = vec![0u32; g.from.len()];
    let mut dag_next = vec![NO_INDEX; g.from.len()];
    for i in 0..g.from.len() {
        let from_c = component[g.from[i] as usize];
        let to_c = component[g.to[i] as usize];
        if from_c != to_c {
            debug_assert!(from_c < to_c);
            dag_to[i] = to_c;
            dag_next[i] = dag_heads[from_c as usize];
            dag_heads[from_c as usize] = i as u32;
        }
    }

    for c in (0..components as usize).rev() {
        let mut e = dag_heads[c];
        while e != NO_INDEX {
            let to_c = dag_to[e as usize] as usize;
            if maximum[c] < maximum[to_c] {
                maximum[c] = maximum[to_c];
            }
            e = dag_next[e as usize];
        }
    }

    for i in 0..nodes {
        required[i] = maximum[component[i] as usize];
    }
}

fn verify_program_init(file: &BytecodeFile) -> Result<ProgramInit, LoadError> {
    let nodes = file.functions.len();
    let mut global_rank = vec![0u32; file.globals.len()];
    let mut init_rank = vec![0u32; nodes];
    let mut required = vec![0u32; nodes];
    let mut g = CallGraph::new(nodes);

    for (i, &fn_idx) in file.init.iter().enumerate() {
        let fn_def = &file.functions[fn_idx as usize];
        let base = file.code_offset(fn_idx);
        init_rank[fn_idx as usize] = (i as u32) + 1;
        let mut target = NO_INDEX;
        let mut pc = 0u32;
        while (pc as usize) < fn_def.code.len() {
            let head = fn_def.code[pc as usize];
            let op_num = (head & OPCODE_MASK) as u16;
            if op_num == Opcode::SetGlobal.number() {
                let global = fn_def.code[pc as usize + 1];
                if target != NO_INDEX && target != global {
                    return Err(LoadError::invalid(
                        base + pc * 4,
                        "init sets different globals",
                    ));
                }
                target = global;
            }
            pc = next_pc(fn_def, pc)?;
        }
        if target == NO_INDEX {
            return Err(LoadError::invalid(base, "init has no global store"));
        }
        if global_rank[target as usize] != 0 {
            return Err(LoadError::invalid(
                PAYLOAD_OFFSET,
                "global has multiple initializers",
            ));
        }
        global_rank[target as usize] = (i as u32) + 1;
    }

    for &rank in &global_rank {
        if rank == 0 {
            return Err(LoadError::invalid(
                PAYLOAD_OFFSET,
                "global has no initializer",
            ));
        }
    }

    for (i, fn_def) in file.functions.iter().enumerate() {
        let mut pc = 0u32;
        while (pc as usize) < fn_def.code.len() {
            let head = fn_def.code[pc as usize];
            let op_num = (head & OPCODE_MASK) as u16;
            if op_num == Opcode::Global.number() {
                let rank = global_rank[fn_def.code[pc as usize + 2] as usize];
                if required[i] < rank {
                    required[i] = rank;
                }
            } else if op_num == Opcode::Call.number() {
                g.add(i as u32, fn_def.code[pc as usize + 2]);
            } else if op_num == Opcode::TailCall.number() {
                g.add(i as u32, fn_def.code[pc as usize + 1]);
            }
            pc = next_pc(fn_def, pc)?;
        }
    }

    compute_required_summaries(&mut g, nodes, &mut required);

    Ok(ProgramInit {
        global_rank,
        init_rank,
        required,
    })
}

// ============================================================================
// Per-function CFG, known-case, and linear ownership/borrow/token flow
// (`src/bytecode/verify.c` parity)
// ============================================================================

struct Flow<'a> {
    file: &'a BytecodeFile,
    fn_def: &'a FunctionDef,
    fn_idx: u32,
    base: u32,
    program: Option<&'a ProgramInit>,
    blocks: Vec<Block>,
    map: Vec<u32>,
    known: Vec<u32>,
    reset: Vec<u32>,
    replay_pc: Option<u32>,
    replay_mode: bool,
    readable: Vec<bool>,
    work: u64,
    work_limit: u64,
}

impl<'a> Flow<'a> {
    fn new(
        file: &'a BytecodeFile,
        fn_idx: u32,
        program: Option<&'a ProgramInit>,
        replay_pc: Option<u32>,
        replay_mode: bool,
    ) -> Self {
        let fn_def = &file.functions[fn_idx as usize];
        let words = (fn_def.register_types.len() as u64) + (fn_def.code.len() as u64) + 32;
        let work_limit = MO_LIMIT_VERIFY_WORK_PER_WORD.saturating_mul(words);
        let readable = if replay_mode {
            vec![false; fn_def.register_types.len()]
        } else {
            Vec::new()
        };
        Self {
            file,
            fn_def,
            fn_idx,
            base: file.code_offset(fn_idx),
            program,
            blocks: Vec::new(),
            map: Vec::new(),
            known: Vec::new(),
            reset: Vec::new(),
            replay_pc,
            replay_mode,
            readable,
            work: 0,
            work_limit,
        }
    }

    fn refuse(&self, pc: u32, reason: &'static str) -> LoadError {
        LoadError::invalid(self.base + pc * 4, reason)
    }

    fn charge(&mut self, pc: u32, steps: u64) -> Result<(), LoadError> {
        if steps > self.work_limit - self.work {
            return Err(LoadError {
                offset: self.base + pc * 4,
                reason: "verification work limit",
                kind: ErrorKind::Limit,
            });
        }
        self.work += steps;
        Ok(())
    }

    #[expect(
        clippy::expect_used,
        reason = "block opcodes were validated in build_graph"
    )]
    fn edge_count(&self, b: u32) -> u32 {
        let pc = self.blocks[b as usize].last;
        let head = self.fn_def.code[pc as usize];
        let op = Opcode::from_u16((head & OPCODE_MASK) as u16).expect("validated opcode");
        if is_terminal(op) {
            0
        } else if op == Opcode::Branch {
            2
        } else if op == Opcode::Switch {
            head >> OPCODE_VARIABLE_SHIFT
        } else {
            1
        }
    }

    #[expect(
        clippy::expect_used,
        reason = "block opcodes were validated in build_graph"
    )]
    fn edge(&self, b: u32, e: u32) -> u32 {
        let pc = self.blocks[b as usize].last;
        let head = self.fn_def.code[pc as usize];
        let op = Opcode::from_u16((head & OPCODE_MASK) as u16).expect("validated opcode");
        let target = match op {
            Opcode::Jump => self.fn_def.code[pc as usize + 1],
            Opcode::Branch | Opcode::Switch => self.fn_def.code[(pc + 2 + e) as usize],
            _ => self.blocks[b as usize].end,
        };
        self.map[target as usize]
    }

    #[expect(
        clippy::expect_used,
        reason = "instruction opcode, prim, and host IDs were validated before ownership analysis"
    )]
    fn operand_access(&self, pc: u32, i: u32) -> OperandAccess {
        let head = self.fn_def.code[pc as usize];
        let info = Opcode::find(head & OPCODE_MASK).expect("validated opcode");
        if i < u32::from(info.fixed_count) {
            return info.fixed[i as usize].access;
        }
        let param = (i - u32::from(info.fixed_count)) as usize;
        match info.variable.access {
            OperandAccess::CallParameter => {
                let target_idx = self.fn_def.code
                    [(pc + if info.opcode == Opcode::Call { 2 } else { 1 }) as usize];
                match self.file.functions[target_idx as usize].parameters[param].ownership {
                    ParamOwnership::Owned => OperandAccess::Consume,
                    ParamOwnership::Borrowed => OperandAccess::Read,
                }
            }
            OperandAccess::PrimParameter => {
                let prim = PrimId::from_u32(self.fn_def.code[(pc + 2) as usize])
                    .expect("validated prim id");
                if prim.info().params.as_bytes()[param] == b'o' {
                    OperandAccess::Consume
                } else {
                    OperandAccess::Read
                }
            }
            OperandAccess::HostParameter => {
                let host = HostOpId::from_u32(self.fn_def.code[(pc + 2) as usize])
                    .expect("validated host id");
                if host.info().params.as_bytes()[param] == b'o' {
                    OperandAccess::Consume
                } else {
                    OperandAccess::Read
                }
            }
            other => other,
        }
    }

    fn build_graph(&mut self) -> Result<(), LoadError> {
        let code_len = self.fn_def.code.len();
        if code_len == 0 {
            return Err(self.refuse(0, "function has no entry instruction"));
        }

        // First mark exact instruction starts so synthetic BytecodeFiles cannot jump mid-instruction.
        let mut starts = vec![false; code_len + 1];
        {
            let mut pc = 0u32;
            while (pc as usize) < code_len {
                starts[pc as usize] = true;
                pc = next_pc(self.fn_def, pc)?;
            }
            starts[code_len] = true;
        }

        let mut is_block_start = vec![0u32; code_len + 1];
        is_block_start[0] = 1;

        let mut pc = 0u32;
        while (pc as usize) < code_len {
            let end = next_pc(self.fn_def, pc)?;
            self.charge(pc, u64::from(end - pc))?;
            if !self.replay_mode {
                verify_instruction(self.file, self.fn_def, self.base, pc)?;
            }
            let head = self.fn_def.code[pc as usize];
            let Some(op) = Opcode::from_u16((head & OPCODE_MASK) as u16) else {
                return Err(self.refuse(pc, "invalid opcode"));
            };
            if is_transfer(op) {
                is_block_start[end as usize] = 1;
            }
            if op == Opcode::Jump {
                let target = self.fn_def.code[pc as usize + 1] as usize;
                if target >= code_len || !starts[target] {
                    return Err(self.refuse(
                        pc + 1,
                        "instruction index out of range or target not a boundary",
                    ));
                }
                is_block_start[target] = 1;
            } else if op == Opcode::Branch || op == Opcode::Switch {
                let n = if op == Opcode::Branch {
                    2u32
                } else {
                    head >> OPCODE_VARIABLE_SHIFT
                };
                for i in 0..n {
                    let target = self.fn_def.code[(pc + 2 + i) as usize] as usize;
                    if target >= code_len || !starts[target] {
                        return Err(self.refuse(
                            pc + 2 + i,
                            "instruction index out of range or target not a boundary",
                        ));
                    }
                    is_block_start[target] = 1;
                }
            }
            if end as usize == code_len && !is_transfer(op) {
                return Err(self.refuse(pc, "function falls off code"));
            }
            pc = end;
        }

        let mut block_count = 0u32;
        pc = 0;
        while (pc as usize) < code_len {
            block_count += is_block_start[pc as usize];
            pc = next_pc(self.fn_def, pc)?;
        }

        let reg_count = self.fn_def.register_types.len() as u64;
        if reg_count.saturating_mul(u64::from(block_count)) > MO_LIMIT_FUNCTION_REGISTER_BLOCKS {
            return Err(LoadError::limit(self.base, "register-block limit"));
        }

        self.map = is_block_start;
        self.blocks = Vec::with_capacity(block_count as usize);
        self.known = vec![CASE_UNKNOWN; code_len];
        self.reset = vec![NO_INDEX; code_len];

        let mut cur_block = 0u32;
        pc = 0;
        while (pc as usize) < code_len {
            if self.map[pc as usize] != 0 {
                if pc != 0 {
                    self.blocks[cur_block as usize].end = pc;
                    cur_block += 1;
                }
                self.blocks.push(Block {
                    start: pc,
                    end: 0,
                    last: pc,
                    incoming: 0,
                    visited: false,
                    state: None,
                });
            }
            self.map[pc as usize] = cur_block;
            self.blocks[cur_block as usize].last = pc;
            pc = next_pc(self.fn_def, pc)?;
        }
        self.blocks[cur_block as usize].end = code_len as u32;

        for b in 0..block_count {
            let ec = self.edge_count(b);
            for e in 0..ec {
                let to = self.edge(b, e);
                self.blocks[to as usize].incoming += 1;
            }
        }

        // Reachability BFS from block 0.
        let mut seen = vec![false; block_count as usize];
        let mut queue = Vec::with_capacity(block_count as usize);
        queue.push(0u32);
        seen[0] = true;
        let mut q_idx = 0usize;
        while q_idx < queue.len() {
            let from = queue[q_idx];
            q_idx += 1;
            for e in 0..self.edge_count(from) {
                let to = self.edge(from, e);
                if !seen[to as usize] {
                    seen[to as usize] = true;
                    queue.push(to);
                }
            }
        }

        for (i, &was_seen) in seen.iter().enumerate() {
            if !was_seen {
                return Err(self.refuse(self.blocks[i].start, "unreachable instruction"));
            }
        }

        Ok(())
    }

    fn analyze_cases(&mut self) -> Result<(), LoadError> {
        let code_len = self.fn_def.code.len();
        let reg_count = self.fn_def.register_types.len();
        let block_count = self.blocks.len();

        let mut needed = vec![false; reg_count];
        let mut total_uses = 0usize;
        let mut any_needed = false;

        let mut pc = 0u32;
        while (pc as usize) < code_len {
            let op_num = (self.fn_def.code[pc as usize] & OPCODE_MASK) as u16;
            if op_num == Opcode::CaseField.number() || op_num == Opcode::Reset.number() {
                let r = self.fn_def.code[pc as usize + 2] as usize;
                if self.file.types.get(self.fn_def.register_types[r]).kind == TypeKind::Choice {
                    needed[r] = true;
                    any_needed = true;
                    total_uses += 1;
                }
            }
            pc = next_pc(self.fn_def, pc)?;
        }

        if any_needed {
            let mut total_edges = 0usize;
            for b in 0..block_count as u32 {
                let ec = self.edge_count(b);
                self.charge(self.blocks[b as usize].last, u64::from(ec))?;
                total_edges += ec as usize;
            }

            let mut pred_head = vec![NO_INDEX; block_count];
            let mut preds = Vec::with_capacity(total_edges);
            for b in 0..block_count as u32 {
                let ec = self.edge_count(b);
                for e in 0..ec {
                    let to = self.edge(b, e) as usize;
                    let idx = preds.len() as u32;
                    preds.push(CasePred {
                        from: b,
                        edge_index: e,
                        next: pred_head[to],
                    });
                    pred_head[to] = idx;
                }
            }

            let mut write_head = vec![NO_INDEX; reg_count];
            let mut written_in_block = vec![NO_INDEX; reg_count];
            let mut writes: Vec<CaseWrite> = Vec::new();
            let mut use_head = vec![NO_INDEX; reg_count];
            let mut uses: Vec<CaseUse> = Vec::with_capacity(total_uses);

            for b in 0..block_count as u32 {
                let mut pc = self.blocks[b as usize].start;
                let end = self.blocks[b as usize].end;
                while pc < end {
                    let Some(info) = Opcode::find(self.fn_def.code[pc as usize] & OPCODE_MASK)
                    else {
                        return Err(self.refuse(pc, "invalid opcode"));
                    };
                    self.charge(pc, 1 + u64::from(info.fixed_count))?;
                    if info.opcode == Opcode::CaseField || info.opcode == Opcode::Reset {
                        let r = self.fn_def.code[pc as usize + 2] as usize;
                        if needed[r] && written_in_block[r] != b {
                            let idx = uses.len() as u32;
                            uses.push(CaseUse {
                                block: b,
                                pc,
                                next: use_head[r],
                            });
                            use_head[r] = idx;
                        }
                    }
                    for i in 0..info.fixed_count as usize {
                        if matches!(
                            info.fixed[i].access,
                            OperandAccess::Write | OperandAccess::TokenWrite
                        ) {
                            let wreg = self.fn_def.code[pc as usize + 1 + i] as usize;
                            if needed[wreg] && written_in_block[wreg] != b {
                                written_in_block[wreg] = b;
                                let idx = writes.len() as u32;
                                writes.push(CaseWrite {
                                    block: b,
                                    next: write_head[wreg],
                                });
                                write_head[wreg] = idx;
                            }
                        }
                    }
                    pc = next_pc(self.fn_def, pc)?;
                }
            }

            let mut writes_stamp = vec![NO_INDEX; block_count];
            let mut relevant_stamp = vec![NO_INDEX; block_count];
            let mut queued_stamp = vec![NO_INDEX; block_count];
            let mut facts = vec![CASE_UNREACHED; block_count];
            let mut r_blocks = vec![0u32; block_count];
            let mut queue = vec![0u32; block_count];

            for r in 0..reg_count as u32 {
                if use_head[r as usize] == NO_INDEX {
                    continue;
                }
                let mut wi = write_head[r as usize];
                while wi != NO_INDEX {
                    let wb = writes[wi as usize].block;
                    self.charge(self.blocks[wb as usize].start, 1)?;
                    writes_stamp[wb as usize] = r;
                    wi = writes[wi as usize].next;
                }

                let mut rel_count = 0usize;
                let mut ui = use_head[r as usize];
                while ui != NO_INDEX {
                    let u = uses[ui as usize];
                    self.charge(u.pc, 1)?;
                    if relevant_stamp[u.block as usize] != r {
                        relevant_stamp[u.block as usize] = r;
                        facts[u.block as usize] = if u.block == 0 {
                            CASE_UNKNOWN
                        } else {
                            CASE_UNREACHED
                        };
                        r_blocks[rel_count] = u.block;
                        rel_count += 1;
                    }
                    ui = u.next;
                }

                let mut qi = 0usize;
                while qi < rel_count {
                    let v = r_blocks[qi];
                    qi += 1;
                    if v == 0 {
                        continue;
                    }
                    let mut pi = pred_head[v as usize];
                    while pi != NO_INDEX {
                        let pred = preds[pi as usize];
                        let u = pred.from;
                        let last_pc = self.blocks[u as usize].last;
                        self.charge(last_pc, 1)?;
                        let switches = (self.fn_def.code[last_pc as usize] & OPCODE_MASK)
                            == u32::from(Opcode::Switch.number())
                            && self.fn_def.code[last_pc as usize + 1] == r;
                        if switches || writes_stamp[u as usize] == r || u == 0 {
                            let incoming = if switches {
                                pred.edge_index
                            } else {
                                CASE_UNKNOWN
                            };
                            let cur = facts[v as usize];
                            facts[v as usize] = if cur == CASE_UNREACHED || cur == incoming {
                                incoming
                            } else {
                                CASE_UNKNOWN
                            };
                        } else if relevant_stamp[u as usize] != r {
                            relevant_stamp[u as usize] = r;
                            facts[u as usize] = CASE_UNREACHED;
                            r_blocks[rel_count] = u;
                            rel_count += 1;
                        }
                        pi = pred.next;
                    }
                }

                let mut q_start = 0usize;
                let mut q_end = 0usize;
                let mut pending = 0usize;
                for &b in &r_blocks[..rel_count] {
                    if b == 0
                        || facts[b as usize] == CASE_UNREACHED
                        || writes_stamp[b as usize] == r
                    {
                        continue;
                    }
                    let last_pc = self.blocks[b as usize].last;
                    let switches = (self.fn_def.code[last_pc as usize] & OPCODE_MASK)
                        == u32::from(Opcode::Switch.number())
                        && self.fn_def.code[last_pc as usize + 1] == r;
                    if !switches {
                        queue[q_end] = b;
                        q_end = (q_end + 1) % block_count;
                        pending += 1;
                        queued_stamp[b as usize] = r;
                    }
                }

                while pending != 0 {
                    let b = queue[q_start];
                    q_start = (q_start + 1) % block_count;
                    pending -= 1;
                    queued_stamp[b as usize] = NO_INDEX;
                    let out = facts[b as usize];
                    let last_pc = self.blocks[b as usize].last;
                    let ec = self.edge_count(b);
                    for i in 0..ec {
                        self.charge(last_pc, 1)?;
                        let to = self.edge(b, i);
                        if to == 0 || relevant_stamp[to as usize] != r {
                            continue;
                        }
                        let cur = facts[to as usize];
                        let merged = if cur == CASE_UNREACHED || cur == out {
                            out
                        } else {
                            CASE_UNKNOWN
                        };
                        if merged != cur {
                            facts[to as usize] = merged;
                            let to_last = self.blocks[to as usize].last;
                            let to_switches = (self.fn_def.code[to_last as usize] & OPCODE_MASK)
                                == u32::from(Opcode::Switch.number())
                                && self.fn_def.code[to_last as usize + 1] == r;
                            if !to_switches
                                && writes_stamp[to as usize] != r
                                && queued_stamp[to as usize] != r
                            {
                                queue[q_end] = to;
                                q_end = (q_end + 1) % block_count;
                                pending += 1;
                                queued_stamp[to as usize] = r;
                            }
                        }
                    }
                }

                let mut ui = use_head[r as usize];
                while ui != NO_INDEX {
                    let u = uses[ui as usize];
                    self.charge(u.pc, 1)?;
                    debug_assert_ne!(facts[u.block as usize], CASE_UNREACHED);
                    self.known[u.pc as usize] = facts[u.block as usize];
                    ui = u.next;
                }
            }
        }

        let mut pc = 0u32;
        while (pc as usize) < code_len {
            let Some(op) = Opcode::from_u16((self.fn_def.code[pc as usize] & OPCODE_MASK) as u16)
            else {
                return Err(self.refuse(pc, "invalid opcode"));
            };
            if op == Opcode::CaseField
                && self.known[pc as usize] != self.fn_def.code[pc as usize + 3]
            {
                return Err(self.refuse(pc, "case is not known"));
            }
            if matches!(op, Opcode::Reset | Opcode::MakeCase | Opcode::MakeClosure) {
                let (ty, tag) = match op {
                    Opcode::Reset => {
                        let ty =
                            self.fn_def.register_types[self.fn_def.code[pc as usize + 2] as usize];
                        let tag = if self.file.types.get(ty).kind == TypeKind::Choice {
                            self.known[pc as usize]
                        } else {
                            NO_INDEX
                        };
                        (ty, tag)
                    }
                    Opcode::MakeCase => (
                        self.fn_def.code[pc as usize + 2],
                        self.fn_def.code[pc as usize + 3],
                    ),
                    Opcode::MakeClosure => (
                        self.fn_def.register_types[self.fn_def.code[pc as usize + 1] as usize],
                        self.fn_def.code[pc as usize + 2],
                    ),
                    _ => unreachable!(),
                };
                let layout = self.file.types.find_layout(ty, tag);
                if op == Opcode::Reset && layout.is_none() {
                    return Err(self.refuse(pc, "reset needs a known fixed layout"));
                }
                if op == Opcode::MakeClosure && layout.is_none() {
                    return Err(self.refuse(pc, "closure layout is missing"));
                }
                if op == Opcode::MakeCase
                    && layout.is_none()
                    && self.file.types.case_shape(ty, tag) == CaseShape::Boxed
                {
                    return Err(self.refuse(pc, "boxed case layout is missing"));
                }
                self.reset[pc as usize] = layout.unwrap_or(NO_INDEX);
            }
            pc = next_pc(self.fn_def, pc)?;
        }

        Ok(())
    }

    fn step<F: FnMut(u32, &[bool]) -> bool>(
        &mut self,
        state: &mut [u32],
        seen: &mut [u32],
        consumed: &mut [u32],
        undo: &mut Vec<Undo>,
        pc: u32,
        visitor: &mut Option<F>,
    ) -> Result<(), LoadError> {
        let head = self.fn_def.code[pc as usize];
        let Some(op) = Opcode::from_u16((head & OPCODE_MASK) as u16) else {
            return Err(self.refuse(pc, "invalid opcode"));
        };
        let info = op.info();
        let fixed_count = u32::from(info.fixed_count);
        let count = fixed_count + (head >> OPCODE_VARIABLE_SHIFT);
        let a = &self.fn_def.code[pc as usize + 1..];
        let reg_count = self.fn_def.register_types.len();

        if let Some(program) = self.program
            && self.fn_def.kind == FunctionKind::Init
        {
            let needed = match op {
                Opcode::Global => program.global_rank[a[1] as usize],
                Opcode::Call => program.required[a[1] as usize],
                Opcode::TailCall => program.required[a[0] as usize],
                _ => 0,
            };
            let available = program.init_rank[self.fn_idx as usize] - 1 + state[reg_count];
            if needed > available {
                return Err(self.refuse(pc, "init reads an unset global"));
            }
        }

        if self.replay_pc == Some(pc) {
            for (slot, &st) in self.readable.iter_mut().zip(&state[..reg_count]) {
                *slot = matches!(st, STATE_VALUE | STATE_OWNED | STATE_BORROWED);
            }
        }
        if let Some(visit) = visitor.as_mut()
            && !visit(pc, &self.readable)
        {
            return Err(self.refuse(pc, "replay visitor stopped"));
        }

        let mut move_state = 0u32;
        for i in 0..count {
            let mode = self.operand_access(pc, i);
            if matches!(
                mode,
                OperandAccess::None | OperandAccess::Write | OperandAccess::TokenWrite
            ) {
                continue;
            }
            let r = a[i as usize] as usize;
            let s = state[r];
            self.charge(pc, 1)?;
            let consumes = matches!(mode, OperandAccess::TokenConsume | OperandAccess::Consume)
                || (mode == OperandAccess::Move && s == STATE_OWNED);

            if mode == OperandAccess::TokenConsume {
                if s < STATE_TOKEN {
                    return Err(self.refuse(pc + 1 + i, "register is not a token"));
                }
            } else if !matches!(s, STATE_VALUE | STATE_OWNED | STATE_BORROWED) {
                return Err(self.refuse(pc + 1 + i, "register is not readable"));
            }
            if mode == OperandAccess::Move {
                move_state = s;
            }
            let is_ref = self.file.types.is_reference(self.fn_def.register_types[r]);
            if consumes && is_ref && mode != OperandAccess::TokenConsume && s != STATE_OWNED {
                return Err(self.refuse(pc + 1 + i, "consume needs an owned reference"));
            }
            if is_ref || mode == OperandAccess::TokenConsume {
                if (seen[r] == pc + 1 && consumes) || consumed[r] == pc + 1 {
                    return Err(self.refuse(pc + 1 + i, "consumed register is repeated"));
                }
                seen[r] = pc + 1;
                if consumes {
                    consumed[r] = pc + 1;
                }
            }
            if op == Opcode::TailCall
                && i >= fixed_count
                && mode == OperandAccess::Read
                && is_ref
                && s != STATE_BORROWED
            {
                return Err(self.refuse(pc + 1 + i, "tail borrow needs a borrowed reference"));
            }
        }

        if op == Opcode::Reuse && state[a[1] as usize] != STATE_TOKEN + a[2] {
            return Err(self.refuse(pc, "reuse layout differs from token"));
        }

        for i in 0..count {
            let mode = self.operand_access(pc, i);
            if !matches!(
                mode,
                OperandAccess::TokenConsume | OperandAccess::Consume | OperandAccess::Move
            ) {
                continue;
            }
            let r = a[i as usize] as usize;
            let is_ref = self.file.types.is_reference(self.fn_def.register_types[r]);
            if mode == OperandAccess::TokenConsume || (is_ref && state[r] == STATE_OWNED) {
                undo.push(Undo {
                    reg: r as u32,
                    state: state[r],
                });
                state[r] = STATE_MOVED;
                if visitor.is_some() {
                    self.readable[r] = false;
                }
            }
        }

        for i in 0..fixed_count {
            let mode = info.fixed[i as usize].access;
            if !matches!(mode, OperandAccess::Write | OperandAccess::TokenWrite) {
                continue;
            }
            let r = a[i as usize] as usize;
            if state[r] == STATE_OWNED || state[r] >= STATE_TOKEN {
                return Err(self.refuse(pc + 1 + i, "write overwrites ownership or token"));
            }
            undo.push(Undo {
                reg: r as u32,
                state: state[r],
            });
            state[r] = if mode == OperandAccess::TokenWrite {
                STATE_TOKEN + self.reset[pc as usize]
            } else if op == Opcode::Clear {
                STATE_UNDEFINED
            } else if op == Opcode::Move {
                move_state
            } else if self.file.types.is_reference(self.fn_def.register_types[r]) {
                if matches!(op, Opcode::Const | Opcode::Global) {
                    STATE_BORROWED
                } else {
                    STATE_OWNED
                }
            } else {
                STATE_VALUE
            };
            if visitor.is_some() {
                self.readable[r] = matches!(state[r], STATE_VALUE | STATE_OWNED | STATE_BORROWED);
            }
        }

        if op == Opcode::SetGlobal {
            if state[reg_count] != 0 {
                return Err(self.refuse(pc, "init sets its global twice"));
            }
            undo.push(Undo {
                reg: reg_count as u32,
                state: 0,
            });
            state[reg_count] = 1;
        }

        if matches!(op, Opcode::Ret | Opcode::TailCall | Opcode::TailCallClosure) {
            for &s in &state[..reg_count] {
                self.charge(pc, 1)?;
                if s == STATE_OWNED || s >= STATE_TOKEN {
                    return Err(self.refuse(pc, "return leaves ownership or token"));
                }
            }
            if self.fn_def.kind == FunctionKind::Init && state[reg_count] == 0 {
                return Err(self.refuse(pc, "init returns without setting its global"));
            }
        }

        Ok(())
    }

    fn verify_ownership<F: FnMut(u32, &[bool]) -> bool>(
        &mut self,
        mut visitor: Option<F>,
    ) -> Result<(), LoadError> {
        let reg_count = self.fn_def.register_types.len();
        let slots = reg_count + 1;
        let mut state = vec![STATE_UNDEFINED; slots];
        let mut seen = vec![0u32; reg_count];
        let mut consumed = vec![0u32; reg_count];
        let mut undo: Vec<Undo> = Vec::with_capacity(self.fn_def.code.len().min(4096));

        for (r, p) in self.fn_def.parameters.iter().enumerate() {
            state[r] = if self.file.types.is_reference(self.fn_def.register_types[r]) {
                match p.ownership {
                    ParamOwnership::Owned => STATE_OWNED,
                    ParamOwnership::Borrowed => STATE_BORROWED,
                }
            } else {
                STATE_VALUE
            };
        }
        if self.fn_def.kind == FunctionKind::Closure {
            state[self.fn_def.parameters.len()] = STATE_OWNED;
        }
        if visitor.is_some() {
            for (slot, &st) in self.readable.iter_mut().zip(&state[..reg_count]) {
                *slot = matches!(st, STATE_VALUE | STATE_OWNED | STATE_BORROWED);
            }
        }

        let mut stack = Vec::with_capacity(self.blocks.len().min(1024));
        stack.push(WalkFrame {
            block: 0,
            edge: 0,
            mark: 0,
        });

        while let Some(top) = stack.last_mut() {
            let b = top.block;
            if !self.blocks[b as usize].visited {
                self.blocks[b as usize].visited = true;
                if self.blocks[b as usize].incoming > 1
                    || (b == 0 && self.blocks[b as usize].incoming != 0)
                {
                    let start = self.blocks[b as usize].start;
                    self.charge(start, slots as u64)?;
                    self.blocks[b as usize].state = Some(state.clone());
                }
                let mut pc = self.blocks[b as usize].start;
                let end = self.blocks[b as usize].end;
                while pc < end {
                    self.step(
                        &mut state,
                        &mut seen,
                        &mut consumed,
                        &mut undo,
                        pc,
                        &mut visitor,
                    )?;
                    pc = next_pc(self.fn_def, pc)?;
                }
            }

            let ec = self.edge_count(b);
            let next_edge = if let Some(top) = stack.last_mut()
                && top.edge < ec
            {
                let e = top.edge;
                top.edge = e + 1;
                Some(e)
            } else {
                None
            };
            if let Some(frame_edge) = next_edge {
                let to = self.edge(b, frame_edge);
                if !self.blocks[to as usize].visited {
                    stack.push(WalkFrame {
                        block: to,
                        edge: 0,
                        mark: undo.len(),
                    });
                } else {
                    let to_start = self.blocks[to as usize].start;
                    self.charge(to_start, slots as u64)?;
                    let Some(expected) = self.blocks[to as usize].state.as_deref() else {
                        return Err(self.refuse(to_start, "visited join block missing saved state"));
                    };
                    if state.as_slice() != expected {
                        return Err(self.refuse(to_start, "register states disagree at join"));
                    }
                }
            } else if let Some(frame) = stack.pop() {
                while undo.len() > frame.mark {
                    let Some(change) = undo.pop() else {
                        break;
                    };
                    let r = change.reg as usize;
                    state[r] = change.state;
                    if visitor.is_some() && r < reg_count {
                        self.readable[r] =
                            matches!(change.state, STATE_VALUE | STATE_OWNED | STATE_BORROWED);
                    }
                }
            }
        }

        Ok(())
    }

    fn run<F: FnMut(u32, &[bool]) -> bool>(&mut self, visitor: Option<F>) -> Result<(), LoadError> {
        self.build_graph()?;
        self.analyze_cases()?;
        self.verify_ownership(visitor)?;
        Ok(())
    }
}

// ============================================================================
// Public entry points
// ============================================================================

/// Runs full section-9 verification over an already structurally decoded `BytecodeFile`.
pub fn verify_module(file: &BytecodeFile) -> Result<(), LoadError> {
    verify_module_with_site(file).map_err(|(err, _)| err)
}

/// Runs full section-9 verification and also reports the failing function index when applicable.
pub(crate) fn verify_module_with_site(file: &BytecodeFile) -> Result<(), (LoadError, Option<u32>)> {
    // 1. Map keys and Set elements must have Hash ability.
    for t in &file.types.items {
        if matches!(t.kind, TypeKind::Set | TypeKind::Map)
            && (file.types.abilities(t.parameters[0]) & ABILITY_HASH) == 0
        {
            return Err((
                LoadError::invalid(PAYLOAD_OFFSET, "map/set key lacks Hash"),
                None,
            ));
        }
    }

    // 2. Linked promise parameters must match subject parameters + result.
    for (i, fn_def) in file.functions.iter().enumerate() {
        if fn_def.kind != FunctionKind::Promise || file.debug[i].subject == NO_INDEX {
            continue;
        }
        let subject = &file.functions[file.debug[i].subject as usize];
        let matches = fn_def.parameters.len() == subject.parameters.len() + 1
            && subject
                .parameters
                .iter()
                .enumerate()
                .all(|(j, p)| fn_def.parameters[j].ty == p.ty)
            && fn_def.parameters[subject.parameters.len()].ty == subject.result_type;
        if !matches {
            return Err((
                LoadError::invalid(PAYLOAD_OFFSET, "promise parameters differ from subject"),
                Some(i as u32),
            ));
        }
    }

    // 3. Every closure constant must have its closure layout.
    for c in &file.constants {
        if file.types.get(c.ty).kind == TypeKind::Function
            && let ConstantValue::Closure(fn_idx) = c.value
            && file.types.find_layout(c.ty, fn_idx).is_none()
        {
            return Err((
                LoadError::invalid(PAYLOAD_OFFSET, "closure constant layout is missing"),
                None,
            ));
        }
    }

    // 4. Whole-program global initializer summaries.
    let program = verify_program_init(file).map_err(|e| (e, None))?;

    // 5. Per-function CFG, instruction types, known-case, and linear ownership/borrow/token flow.
    for i in 0..file.functions.len() as u32 {
        let mut flow = Flow::new(file, i, Some(&program), None, false);
        flow.run::<fn(u32, &[bool]) -> bool>(None)
            .map_err(|e| (e, Some(i)))?;
    }

    // 6. Reject `Sqlite.stats_raw` / `Sqlite.reset_stats_raw` transitively reachable from
    // non-spec entry functions (`init`, services, or module-0 `main`).
    verify_sqlite_stats_not_in_prod(file)?;

    Ok(())
}

fn verify_sqlite_stats_not_in_prod(file: &BytecodeFile) -> Result<(), (LoadError, Option<u32>)> {
    let n = file.functions.len();
    let mut reaches = vec![false; n];
    let mut edges: Vec<Vec<u32>> = vec![Vec::new(); n];

    for (i, fn_def) in file.functions.iter().enumerate() {
        let mut pc = 0u32;
        while (pc as usize) < fn_def.code.len() {
            let head = fn_def.code[pc as usize];
            let op_num = (head & OPCODE_MASK) as u16;
            let a = &fn_def.code[pc as usize + 1..];
            if op_num == Opcode::Host.number() {
                if let Some(host_op) = HostOpId::from_u32(a[1])
                    && matches!(
                        host_op,
                        HostOpId::SqliteStatsRaw | HostOpId::SqliteResetStatsRaw
                    )
                {
                    reaches[i] = true;
                }
            } else if op_num == Opcode::Call.number() {
                edges[i].push(a[1]);
            } else if op_num == Opcode::TailCall.number() {
                edges[i].push(a[0]);
            } else if op_num == Opcode::MakeClosure.number() {
                edges[i].push(a[1]);
            } else if op_num == Opcode::Const.number() {
                let c_idx = a[1] as usize;
                if let Some(c) = file.constants.get(c_idx)
                    && file.types.get(c.ty).kind == TypeKind::Function
                    && let ConstantValue::Closure(callee) = c.value
                {
                    edges[i].push(callee);
                }
            }
            pc = next_pc(fn_def, pc).map_err(|e| (e, Some(i as u32)))?;
        }
    }

    loop {
        let mut changed = false;
        for i in 0..n {
            if reaches[i] {
                continue;
            }
            if edges[i].iter().any(|&callee| reaches[callee as usize]) {
                reaches[i] = true;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }

    for &init_fn in &file.init {
        if reaches[init_fn as usize] {
            return Err((
                LoadError::invalid(PAYLOAD_OFFSET, "Sqlite.stats reachable from init"),
                Some(init_fn),
            ));
        }
    }
    for svc in &file.services {
        if reaches[svc.start as usize] {
            return Err((
                LoadError::invalid(PAYLOAD_OFFSET, "Sqlite.stats reachable from service start"),
                Some(svc.start),
            ));
        }
        for tool in &svc.tools {
            if reaches[tool.function as usize] {
                return Err((
                    LoadError::invalid(PAYLOAD_OFFSET, "Sqlite.stats reachable from service tool"),
                    Some(tool.function),
                ));
            }
        }
        for timer in &svc.timers {
            if reaches[timer.function as usize] {
                return Err((
                    LoadError::invalid(PAYLOAD_OFFSET, "Sqlite.stats reachable from service timer"),
                    Some(timer.function),
                ));
            }
        }
    }
    for (i, fn_def) in file.functions.iter().enumerate() {
        if !fn_def.spec_code
            && fn_def.module == 0
            && file.function_name(i as u32) == "main"
            && reaches[i]
        {
            return Err((
                LoadError::invalid(PAYLOAD_OFFSET, "Sqlite.stats reachable from main"),
                Some(i as u32),
            ));
        }
    }
    Ok(())
}

/// Replays dataflow verification over a single function and returns the pre-instruction
/// readability mask for every register at instruction boundary `pc`
/// (matches `mo_image_registers_readable` in `src/bytecode/image.c`).
pub fn registers_readable(
    file: &BytecodeFile,
    function_idx: u32,
    pc: u32,
) -> Result<Vec<bool>, LoadError> {
    registers_readable_with_work(file, function_idx, pc).map(|(mask, _)| mask)
}

/// Like [`registers_readable`], also returning the verifier step count (for bounded-work tests).
pub fn registers_readable_with_work(
    file: &BytecodeFile,
    function_idx: u32,
    pc: u32,
) -> Result<(Vec<bool>, u64), LoadError> {
    let Some(fn_def) = file.functions.get(function_idx as usize) else {
        return Err(LoadError::invalid(0, "invalid replay function"));
    };
    let mut is_boundary = false;
    let mut at = 0u32;
    while (at as usize) < fn_def.code.len() {
        if at == pc {
            is_boundary = true;
            break;
        }
        at = next_pc(fn_def, at)?;
    }
    if !is_boundary {
        return Err(LoadError::invalid(0, "invalid replay instruction"));
    }

    let mut flow = Flow::new(file, function_idx, None, Some(pc), true);
    flow.run::<fn(u32, &[bool]) -> bool>(None)?;
    Ok((flow.readable, flow.work))
}

/// Streaming readability replay visiting every instruction boundary once in DFS order
/// (matches `mo_image_visit_readable` in `src/bytecode/verify.c`).
pub fn visit_readable<F>(file: &BytecodeFile, function_idx: u32, visit: F) -> Result<(), LoadError>
where
    F: FnMut(u32, &[bool]) -> bool,
{
    if (function_idx as usize) >= file.functions.len() {
        return Err(LoadError::invalid(0, "invalid replay function"));
    }
    let mut flow = Flow::new(file, function_idx, None, None, true);
    flow.run(Some(visit))
}
