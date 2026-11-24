//! Canonical indexed bytecode text disassembler matching `src/bytecode/disassemble.c`.

use std::collections::HashMap;
use std::fmt::Write as _;

use mo_def::limits::MO_LIMIT_BYTECODE_TEXT_SIZE;
use mo_def::{Opcode, OperandKind};

use crate::{
    AnchorExpectation, BigIntSign, BytecodeFile, ConstantValue, FunctionKind, LayoutKind,
    LoadError, NO_INDEX, OPCODE_MASK, OPCODE_VARIABLE_SHIFT, ParamOwnership, TypeField, TypeKind,
};

const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";

const fn type_kind_name(kind: TypeKind) -> &'static str {
    match kind {
        TypeKind::Unit => "unit",
        TypeKind::Bool => "bool",
        TypeKind::Byte => "byte",
        TypeKind::Int => "int",
        TypeKind::Float => "float",
        TypeKind::Char => "char",
        TypeKind::Time => "time",
        TypeKind::Duration => "duration",
        TypeKind::Decimal => "decimal",
        TypeKind::Range => "range",
        TypeKind::Text => "text",
        TypeKind::Bytes => "bytes",
        TypeKind::BigInt => "bigint",
        TypeKind::Tuple => "tuple",
        TypeKind::List => "list",
        TypeKind::Set => "set",
        TypeKind::Map => "map",
        TypeKind::Record => "record",
        TypeKind::Choice => "choice",
        TypeKind::Function => "function",
        TypeKind::Env => "env",
        TypeKind::Capability => "capability",
    }
}

const fn function_kind_name(kind: FunctionKind) -> &'static str {
    match kind {
        FunctionKind::Function => "function",
        FunctionKind::Closure => "closure",
        FunctionKind::Init => "init",
        FunctionKind::Test => "test",
        FunctionKind::Example => "example",
        FunctionKind::Promise => "promise",
        FunctionKind::Anchor => "anchor",
    }
}

const fn expectation_name(exp: AnchorExpectation) -> &'static str {
    match exp {
        AnchorExpectation::NotAnchor => "none",
        AnchorExpectation::Allows => "allows",
        AnchorExpectation::Forbids => "forbids",
    }
}

const fn layout_kind_name(kind: LayoutKind) -> &'static str {
    match kind {
        LayoutKind::Record => "record",
        LayoutKind::Tuple => "tuple",
        LayoutKind::Case => "case",
        LayoutKind::Closure => "closure",
    }
}

const fn operand_prefix(kind: OperandKind) -> &'static str {
    match kind {
        OperandKind::None | OperandKind::Field | OperandKind::Case | OperandKind::Index => "",
        OperandKind::Register => "r",
        OperandKind::Type => "t",
        OperandKind::Layout => "L",
        OperandKind::Constant => "k",
        OperandKind::Global => "g",
        OperandKind::Function => "f",
        OperandKind::Primitive => "p",
        OperandKind::Host => "h",
        OperandKind::Target => "@",
    }
}

const fn bigint_sign_name(sign: BigIntSign) -> &'static str {
    match sign {
        BigIntSign::Zero => "zero",
        BigIntSign::Positive => "positive",
        BigIntSign::Negative => "negative",
    }
}

struct Printer<'a> {
    out: String,
    names: HashMap<&'a [u8], u32>,
}

impl<'a> Printer<'a> {
    fn new(file: &'a BytecodeFile) -> Self {
        let mut names = HashMap::with_capacity(file.strings.len());
        for (i, s) in file.strings.iter().enumerate() {
            let bytes = s.as_bytes();
            if !bytes.is_empty() && !bytes.contains(&0) {
                names.entry(bytes).or_insert(i as u32);
            }
        }
        Self {
            out: String::new(),
            names,
        }
    }

    fn reserve_checked(&self, extra: usize) -> Result<(), LoadError> {
        let limit = MO_LIMIT_BYTECODE_TEXT_SIZE as usize;
        if extra > limit.saturating_sub(self.out.len()) {
            return Err(LoadError::limit(0, "bytecode text output limit"));
        }
        Ok(())
    }

    fn text(&mut self, s: &str) -> Result<(), LoadError> {
        self.reserve_checked(s.len())?;
        self.out.push_str(s);
        Ok(())
    }

    fn number(&mut self, prefix: &str, value: u32) -> Result<(), LoadError> {
        let mut digits = [0u8; 10];
        let mut n = 0usize;
        let mut v = value;
        loop {
            digits[n] = b'0' + (v % 10) as u8;
            n += 1;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        digits[..n].reverse();
        self.reserve_checked(prefix.len() + n)?;
        self.out.push_str(prefix);
        for &b in &digits[..n] {
            self.out.push(char::from(b));
        }
        Ok(())
    }

    fn optional(&mut self, prefix: &str, value: u32) -> Result<(), LoadError> {
        if value == NO_INDEX {
            self.text("-")
        } else {
            self.number(prefix, value)
        }
    }

    fn quoted(&mut self, s: &str) -> Result<(), LoadError> {
        let bytes = s.as_bytes();
        self.text("\"")?;
        let mut start = 0usize;
        for (i, &c) in bytes.iter().enumerate() {
            if (0x20..=0x7e).contains(&c) && c != b'"' && c != b'\\' {
                continue;
            }
            if i > start {
                // Safe: every byte in start..i is printable ASCII.
                let chunk = &s[start..i];
                self.text(chunk)?;
            }
            if c == b'"' || c == b'\\' {
                self.reserve_checked(2)?;
                self.out.push('\\');
                self.out.push(char::from(c));
            } else {
                self.reserve_checked(4)?;
                self.out.push_str("\\x");
                self.out.push(char::from(HEX_DIGITS[usize::from(c >> 4)]));
                self.out.push(char::from(HEX_DIGITS[usize::from(c & 15)]));
            }
            start = i + 1;
        }
        if start < bytes.len() {
            let chunk = &s[start..];
            self.text(chunk)?;
        }
        self.text("\"")
    }

    fn hexadecimal(&mut self, bytes: &[u8]) -> Result<(), LoadError> {
        let needed = 5usize.saturating_add(bytes.len().saturating_mul(2));
        self.reserve_checked(needed)?;
        self.out.push_str("hex\"");
        for &b in bytes {
            self.out.push(char::from(HEX_DIGITS[usize::from(b >> 4)]));
            self.out.push(char::from(HEX_DIGITS[usize::from(b & 15)]));
        }
        self.out.push('"');
        Ok(())
    }

    fn indices(&mut self, prefix: &str, items: &[u32]) -> Result<(), LoadError> {
        self.text("[")?;
        for (i, &item) in items.iter().enumerate() {
            if i != 0 {
                self.text(", ")?;
            }
            self.number(prefix, item)?;
        }
        self.text("]")
    }

    fn name(&mut self, s: Option<&str>) -> Result<(), LoadError> {
        let idx = match s {
            None => NO_INDEX,
            Some(text) => self
                .names
                .get(text.as_bytes())
                .copied()
                .ok_or_else(|| LoadError::invalid(0, "name missing from strings"))?,
        };
        self.optional("s", idx)
    }

    fn fields(&mut self, items: &[TypeField]) -> Result<(), LoadError> {
        self.text("[")?;
        for (i, field) in items.iter().enumerate() {
            if i != 0 {
                self.text(", ")?;
            }
            self.text("(")?;
            self.name(field.name.as_deref())?;
            self.text(", ")?;
            self.number("t", field.ty)?;
            self.text(")")?;
        }
        self.text("]")
    }

    fn location(&mut self, file: u32, start: u32, end: u32) -> Result<(), LoadError> {
        self.optional("P", file)?;
        self.number(", ", start)?;
        self.number(", ", end)
    }

    fn scalar(&mut self, kind: TypeKind, bytes: &[u8]) -> Result<(), LoadError> {
        match kind {
            TypeKind::Unit => self.text("()"),
            TypeKind::Bool => {
                let is_true = bytes.iter().any(|&b| b != 0);
                self.text(if is_true { "true" } else { "false" })
            }
            TypeKind::Float => {
                self.text("bits ")?;
                self.hexadecimal(bytes)
            }
            TypeKind::Char => {
                let mut val = 0u64;
                for (i, &b) in bytes.iter().enumerate() {
                    val |= u64::from(b) << (8 * i);
                }
                let mut buf = String::with_capacity(10);
                let _ = write!(buf, "U+{val:x}");
                self.text(&buf)
            }
            TypeKind::Decimal => {
                let mut arr = [0u8; 16];
                arr.copy_from_slice(bytes);
                let signed = i128::from_le_bytes(arr);
                if signed < 0 {
                    self.text("-")?;
                }
                let mut abs = signed.unsigned_abs();
                let mut digits = [0u8; 40];
                let mut n = 0usize;
                loop {
                    digits[n] = b'0' + (abs % 10) as u8;
                    n += 1;
                    abs /= 10;
                    if abs == 0 {
                        break;
                    }
                }
                while n <= 18 {
                    digits[n] = b'0';
                    n += 1;
                }
                let mut last = 0usize;
                while last < 18 && digits[last] == b'0' {
                    last += 1;
                }
                let int_digits = n - 18;
                let frac_digits = if last == 18 { 0 } else { 1 + (18 - last) };
                self.reserve_checked(int_digits + frac_digits)?;
                for j in (18..n).rev() {
                    self.out.push(char::from(digits[j]));
                }
                if last != 18 {
                    self.out.push('.');
                    for j in (last..18).rev() {
                        self.out.push(char::from(digits[j]));
                    }
                }
                Ok(())
            }
            TypeKind::Byte | TypeKind::Int | TypeKind::Time | TypeKind::Duration => {
                let mut arr = [0u8; 8];
                arr.copy_from_slice(bytes);
                let signed = i64::from_le_bytes(arr);
                let mut buf = String::with_capacity(20);
                let _ = write!(buf, "{signed}");
                self.text(&buf)
            }
            _ => Err(LoadError::invalid(0, "invalid scalar constant type")),
        }
    }

    fn print_types(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("types\n")?;
        for (i, t) in file.types.items.iter().enumerate() {
            self.number("  t", i as u32)?;
            self.text(" ")?;
            self.text(type_kind_name(t.kind))?;
            match t.kind {
                TypeKind::Tuple | TypeKind::Function => {
                    self.text(" ")?;
                    self.indices("t", &t.parameters)?;
                    if t.kind == TypeKind::Function {
                        self.text(" ")?;
                        self.number("t", t.result)?;
                    }
                }
                TypeKind::List | TypeKind::Set | TypeKind::Map => {
                    for &p in &t.parameters {
                        self.text(" ")?;
                        self.number("t", p)?;
                    }
                }
                TypeKind::Record => {
                    self.text(" ")?;
                    self.name(t.name.as_deref())?;
                    self.text(" ")?;
                    self.fields(&t.fields)?;
                }
                TypeKind::Choice => {
                    self.text(" ")?;
                    self.name(t.name.as_deref())?;
                    self.text(" [")?;
                    for (j, c) in t.cases.iter().enumerate() {
                        if j != 0 {
                            self.text(", ")?;
                        }
                        self.text("(")?;
                        self.name(Some(c.name.as_str()))?;
                        self.text(", ")?;
                        self.fields(&c.fields)?;
                        self.text(")")?;
                    }
                    self.text("]")?;
                }
                TypeKind::Env => {
                    self.text(" ")?;
                    self.number("f", t.function)?;
                }
                TypeKind::Capability => {
                    let Some(cap) = t.capability else {
                        return Err(LoadError::invalid(0, "unknown capability"));
                    };
                    self.text(" ")?;
                    self.text(cap.name())?;
                }
                TypeKind::Unit
                | TypeKind::Bool
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
                | TypeKind::BigInt => {}
            }
            self.text("\n")?;
        }
        self.text("end\n")
    }

    fn print_layouts(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("layouts\n")?;
        for (i, l) in file.types.layouts.iter().enumerate() {
            self.number("  L", i as u32)?;
            self.text(" ")?;
            self.text(layout_kind_name(l.kind))?;
            self.text(" ")?;
            self.number("t", l.ty)?;
            match l.kind {
                LayoutKind::Case => {
                    self.text(" ")?;
                    self.number("", l.case_index)?;
                }
                LayoutKind::Closure => {
                    self.text(" ")?;
                    self.number("f", l.function)?;
                }
                LayoutKind::Record | LayoutKind::Tuple => {}
            }
            self.text(" ")?;
            self.indices("t", &l.fields)?;
            self.text("\n")?;
        }
        self.text("end\n")
    }

    fn print_constants(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("constants\n")?;
        for (i, k) in file.constants.iter().enumerate() {
            self.number("  k", i as u32)?;
            self.text(" ")?;
            self.number("t", k.ty)?;
            self.text(" ")?;
            let ty_kind = file.types.get(k.ty).kind;
            match &k.value {
                ConstantValue::Immediate64(bytes) => self.scalar(ty_kind, bytes)?,
                ConstantValue::Decimal128(bytes) => self.scalar(ty_kind, bytes)?,
                ConstantValue::Text(s) => {
                    self.text("text ")?;
                    self.number("s", *s)?;
                }
                ConstantValue::Bytes(bytes) => {
                    self.text("bytes ")?;
                    self.hexadecimal(bytes)?;
                }
                ConstantValue::BigInt { sign, magnitude } => {
                    self.text("bigint ")?;
                    self.text(bigint_sign_name(*sign))?;
                    self.text(" ")?;
                    self.hexadecimal(magnitude)?;
                }
                ConstantValue::Closure(fn_idx) => {
                    self.text("closure ")?;
                    self.number("f", *fn_idx)?;
                }
            }
            self.text("\n")?;
        }
        self.text("end\n")
    }

    fn print_functions(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("functions\n")?;
        for (i, f) in file.functions.iter().enumerate() {
            self.number("  f", i as u32)?;
            self.text(" ")?;
            self.number("s", f.name)?;
            self.text(" ")?;
            self.number("m", f.module)?;
            self.text(" ")?;
            self.text(function_kind_name(f.kind))?;
            self.number(" spec ", u32::from(f.spec_code))?;
            self.text("\n    params [")?;
            for (j, p) in f.parameters.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(t", p.ty)?;
                self.text(match p.ownership {
                    ParamOwnership::Owned => ", owned)",
                    ParamOwnership::Borrowed => ", borrowed)",
                })?;
            }
            self.text("]\n")?;
            self.number("    result t", f.result_type)?;
            self.text("\n    captures ")?;
            self.indices("t", &f.capture_types)?;
            self.text("\n    registers ")?;
            self.indices("t", &f.register_types)?;
            self.text("\n    code\n")?;

            let mut pc = 0usize;
            while pc < f.code.len() {
                let head = f.code[pc];
                pc += 1;
                let Some(op) = Opcode::find(head & OPCODE_MASK) else {
                    return Err(LoadError::invalid(0, "unknown opcode"));
                };
                let var_count = (head >> OPCODE_VARIABLE_SHIFT) as usize;
                let fixed_count = op.fixed_count as usize;
                let total = fixed_count + var_count;
                self.text("      ")?;
                self.text(op.name)?;
                for j in 0..total {
                    let kind = if j < fixed_count {
                        op.fixed[j].kind
                    } else {
                        op.variable.kind
                    };
                    self.text(if j == 0 { " " } else { ", " })?;
                    self.number(operand_prefix(kind), f.code[pc])?;
                    pc += 1;
                }
                self.text("\n")?;
            }

            self.text("    end\n  end\n")?;
        }
        self.text("end\n")
    }

    fn print_modules(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("modules\n")?;
        for (i, m) in file.modules.iter().enumerate() {
            self.number("  m", i as u32)?;
            self.text(" ")?;
            self.number("s", m.name)?;
            self.text(" types [")?;
            for (j, exp) in m.types.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(t", exp.ty)?;
                self.number(", ", u32::from(exp.contents_exposed))?;
                self.text(")")?;
            }
            self.text("] functions ")?;
            self.indices("f", &m.functions)?;
            self.text(" globals ")?;
            self.indices("g", &m.globals)?;
            self.text("\n")?;
        }
        self.text("end\n")
    }

    fn print_debug(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("debug\n")?;
        for (i, d) in file.debug.iter().enumerate() {
            self.number("  d", i as u32)?;
            self.text(" spans [")?;
            for (j, s) in d.spans.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(@", s.code_offset)?;
                self.text(", ")?;
                self.location(s.file, s.start, s.end)?;
                self.text(")")?;
            }
            self.text("] variables [")?;
            for (j, n) in d.names.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(r", n.reg)?;
                self.number(", s", n.name)?;
                self.number(", @", n.code_start)?;
                self.number(", @", n.code_end)?;
                self.text(")")?;
            }
            self.text("] text ")?;
            self.optional("s", d.text)?;
            self.text(" declaration (")?;
            self.location(d.file, d.start, d.end)?;
            self.text(") subject ")?;
            self.optional("f", d.subject)?;
            self.text(" expectation ")?;
            self.text(expectation_name(d.expectation))?;
            self.text(" lines [")?;
            for (j, l) in d.lines.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.text("(")?;
                self.location(l.file, l.start, l.end)?;
                self.number(", s", l.text)?;
                self.text(")")?;
            }
            self.text("]\n")?;
        }
        self.text("end\n")
    }

    fn print_services(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        if file.services.is_empty() {
            return Ok(());
        }
        self.text("services\n")?;
        for (i, s) in file.services.iter().enumerate() {
            self.number("  S", i as u32)?;
            self.number(" m", s.module)?;
            self.number(" t", s.state_type)?;
            self.number(" f", s.start)?;
            self.number(" s", s.token_env)?;
            self.text(" tools [")?;
            for (j, t) in s.tools.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(s", t.name)?;
                self.number(", f", t.function)?;
                self.text(if t.read_only { ", 1, [" } else { ", 0, [" })?;
                for (k, &arg_ty) in t.argument_types.iter().enumerate() {
                    if k != 0 {
                        self.text(", ")?;
                    }
                    self.number("t", arg_ty)?;
                }
                self.number("], t", t.result_type)?;
                self.number(", s", t.description)?;
                self.text(", [")?;
                for (k, &arg_name) in t.argument_names.iter().enumerate() {
                    if k != 0 {
                        self.text(", ")?;
                    }
                    self.number("s", arg_name)?;
                }
                self.text("])")?;
            }
            self.text("] timers [")?;
            for (j, timer) in s.timers.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(f", timer.function)?;
                self.number(", f", timer.interval)?;
                self.text(")")?;
            }
            self.text("] shapes [")?;
            for (j, shape) in s.shapes.iter().enumerate() {
                if j != 0 {
                    self.text(", ")?;
                }
                self.number("(t", shape.ty)?;
                self.text(if shape.is_struct {
                    ", struct, ["
                } else {
                    ", enum, ["
                })?;
                for (k, &flag) in shape.unnamed.iter().enumerate() {
                    if k != 0 {
                        self.text(", ")?;
                    }
                    self.text(if flag { "1" } else { "0" })?;
                }
                self.text("])")?;
            }
            self.text("]\n")?;
        }
        self.text("end\n")
    }

    fn print_file(&mut self, file: &BytecodeFile) -> Result<(), LoadError> {
        self.text("bytecode 0.3 indexed\nstrings\n")?;
        for (i, s) in file.strings.iter().enumerate() {
            self.number("  s", i as u32)?;
            self.text(" ")?;
            self.quoted(s)?;
            self.text("\n")?;
        }
        self.text("end\n")?;
        self.print_types(file)?;
        self.print_layouts(file)?;
        self.print_constants(file)?;
        self.text("globals\n")?;
        for (i, g) in file.globals.iter().enumerate() {
            self.number("  g", i as u32)?;
            self.number(" t", g.ty)?;
            self.number(" s", g.name)?;
            self.number(" m", g.module)?;
            self.text("\n")?;
        }
        self.text("end\ninit\n")?;
        for (i, &fn_idx) in file.init.iter().enumerate() {
            self.number("  i", i as u32)?;
            self.number(" f", fn_idx)?;
            self.text("\n")?;
        }
        self.text("end\n")?;
        self.print_functions(file)?;
        self.print_modules(file)?;
        self.text("files\n")?;
        for (i, path) in file.files.iter().enumerate() {
            self.number("  P", i as u32)?;
            self.text(" ")?;
            self.hexadecimal(path)?;
            self.text("\n")?;
        }
        self.text("end\n")?;
        self.print_debug(file)?;
        self.print_services(file)
    }
}

/// Verifies `bytes` with [`crate::load`] and renders its canonical `bytecode 0.3 indexed`
/// disassembly (matching `mo_bytecode_disassemble` in `src/bytecode/disassemble.c`).
pub fn disassemble(bytes: &[u8]) -> Result<String, LoadError> {
    let file = crate::load(bytes)?;
    disassemble_file(&file)
}

/// Renders the canonical `bytecode 0.3 indexed` disassembly of an already verified [`BytecodeFile`].
pub fn disassemble_file(file: &BytecodeFile) -> Result<String, LoadError> {
    let mut printer = Printer::new(file);
    printer.print_file(file)?;
    Ok(printer.out)
}
