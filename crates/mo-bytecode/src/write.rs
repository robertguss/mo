//! Exact `.bc` v0.3 (`MOBC`) binary encoder and verified writer matching `src/bytecode/write.c`.

use std::collections::HashMap;
use std::fmt;

use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_BYTECODE_SIZE, MO_LIMIT_CHOICE_CASES,
    MO_LIMIT_CONSTANTS, MO_LIMIT_DEBUG_ENTRIES, MO_LIMIT_FIELDS, MO_LIMIT_FILES,
    MO_LIMIT_FUNCTION_CODE_WORDS, MO_LIMIT_FUNCTION_REGISTERS, MO_LIMIT_FUNCTIONS,
    MO_LIMIT_GLOBALS, MO_LIMIT_INITS, MO_LIMIT_LAYOUTS, MO_LIMIT_MODULE_NAMES, MO_LIMIT_MODULES,
    MO_LIMIT_SERVICE_HANDLERS, MO_LIMIT_SERVICES, MO_LIMIT_STRING_SIZE, MO_LIMIT_STRINGS,
    MO_LIMIT_TYPES,
};

use crate::{
    BYTECODE_FLAGS_NONE, BYTECODE_FLAGS_SERVICES, BYTECODE_HEADER_SIZE, BYTECODE_MAGIC,
    BYTECODE_MAJOR, BYTECODE_MINOR, BytecodeFile, ConstantValue, ErrorKind, FNV_OFFSET, LayoutKind,
    NO_INDEX, TypeField, TypeKind, fnv1a, read, verify,
};

/// Error returned by [`encode`] and [`write()`], matching `struct mo_write_error`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WriteError {
    pub reason: &'static str,
    pub kind: ErrorKind,
    pub function: Option<u32>,
}

impl WriteError {
    #[must_use]
    pub const fn invalid(reason: &'static str, function: Option<u32>) -> Self {
        Self {
            reason,
            kind: ErrorKind::Invalid,
            function,
        }
    }

    #[must_use]
    pub const fn limit(reason: &'static str, function: Option<u32>) -> Self {
        Self {
            reason,
            kind: ErrorKind::Limit,
            function,
        }
    }
}

impl fmt::Display for WriteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.function {
            Some(idx) => write!(f, "{} (function f{idx})", self.reason),
            None => f.write_str(self.reason),
        }
    }
}

impl std::error::Error for WriteError {}

struct Writer<'a> {
    file: &'a BytecodeFile,
    out: Vec<u8>,
    names: HashMap<&'a [u8], u32>,
    function: Option<u32>,
}

impl<'a> Writer<'a> {
    fn new(file: &'a BytecodeFile) -> Self {
        let mut names = HashMap::with_capacity(file.strings.len());
        for (i, s) in file.strings.iter().enumerate() {
            let bytes = s.as_bytes();
            if !bytes.is_empty() && !bytes.contains(&0) {
                names.entry(bytes).or_insert(i as u32);
            }
        }
        Self {
            file,
            out: Vec::new(),
            names,
            function: None,
        }
    }

    fn err_invalid(&self, reason: &'static str) -> WriteError {
        WriteError::invalid(reason, self.function)
    }

    fn err_limit(&self, reason: &'static str) -> WriteError {
        WriteError::limit(reason, self.function)
    }

    fn word(&mut self, value: u32) -> Result<(), WriteError> {
        let max = MO_LIMIT_BYTECODE_SIZE as usize;
        if self.out.len() > max - 4 {
            return Err(self.err_limit("bytecode/output limit or no memory"));
        }
        self.out.extend_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn raw(&mut self, bytes: &[u8]) -> Result<(), WriteError> {
        let max = MO_LIMIT_BYTECODE_SIZE as usize;
        if bytes.len() > max - self.out.len() {
            return Err(self.err_limit("invalid bytes, bytecode/output limit or no memory"));
        }
        self.out.extend_from_slice(bytes);
        Ok(())
    }

    fn count(&mut self, n: usize, limit: u64) -> Result<(), WriteError> {
        if (n as u64) > limit {
            return Err(self.err_limit("invalid array or count past limit"));
        }
        self.word(n as u32)
    }

    fn indices(&mut self, items: &[u32], limit: u64) -> Result<(), WriteError> {
        self.count(items.len(), limit)?;
        for &item in items {
            self.word(item)?;
        }
        Ok(())
    }

    fn name(&mut self, text: Option<&str>, optional: bool) -> Result<(), WriteError> {
        let Some(text) = text else {
            if optional {
                return self.word(NO_INDEX);
            }
            return Err(self.err_invalid("invalid name"));
        };
        if text.is_empty() {
            return Err(self.err_invalid("invalid name"));
        }
        let bytes = text.as_bytes();
        if (bytes.len() as u64) > MO_LIMIT_STRING_SIZE {
            return Err(self.err_limit("name missing from strings or past limit"));
        }
        let Some(&idx) = self.names.get(bytes) else {
            return Err(self.err_invalid("name missing from strings or past limit"));
        };
        self.word(idx)
    }

    fn fields(&mut self, items: &[TypeField], optional: bool) -> Result<(), WriteError> {
        self.count(items.len(), MO_LIMIT_FIELDS)?;
        let allow_unnamed = optional && items.len() == 1;
        for field in items {
            self.name(field.name.as_deref(), allow_unnamed)?;
            self.word(field.ty)?;
        }
        Ok(())
    }

    fn write_types(&mut self) -> Result<(), WriteError> {
        let types = &self.file.types.items;
        self.count(types.len(), MO_LIMIT_TYPES)?;
        for t in types {
            self.word(t.kind as u32)?;
            match t.kind {
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
                TypeKind::Tuple
                | TypeKind::List
                | TypeKind::Set
                | TypeKind::Map
                | TypeKind::Function => {
                    let limit = if t.kind == TypeKind::Function {
                        MO_LIMIT_BYTECODE_PARAMETERS
                    } else {
                        MO_LIMIT_FIELDS
                    };
                    self.indices(&t.parameters, limit)?;
                    if t.kind == TypeKind::Function {
                        self.word(t.result)?;
                    }
                }
                TypeKind::Record => {
                    self.name(t.name.as_deref(), false)?;
                    self.fields(&t.fields, false)?;
                }
                TypeKind::Choice => {
                    self.name(t.name.as_deref(), false)?;
                    self.count(t.cases.len(), MO_LIMIT_CHOICE_CASES)?;
                    for case in &t.cases {
                        self.name(Some(case.name.as_str()), false)?;
                        self.fields(&case.fields, true)?;
                    }
                }
                TypeKind::Env => {
                    self.word(t.function)?;
                }
                TypeKind::Capability => {
                    let Some(cap) = t.capability else {
                        return Err(self.err_invalid("unknown capability"));
                    };
                    self.word(cap as u32)?;
                }
            }
        }
        Ok(())
    }

    fn write_constants(&mut self) -> Result<(), WriteError> {
        let constants = &self.file.constants;
        let types = &self.file.types.items;
        self.count(constants.len(), MO_LIMIT_CONSTANTS)?;
        for k in constants {
            let Some(ty_def) = types.get(k.ty as usize) else {
                return Err(self.err_invalid("constant type out of range"));
            };
            self.word(k.ty)?;
            match (ty_def.kind, &k.value) {
                (
                    TypeKind::Unit
                    | TypeKind::Bool
                    | TypeKind::Byte
                    | TypeKind::Int
                    | TypeKind::Float
                    | TypeKind::Char
                    | TypeKind::Time
                    | TypeKind::Duration,
                    ConstantValue::Immediate64(bytes),
                ) => {
                    self.raw(bytes)?;
                }
                (TypeKind::Decimal, ConstantValue::Decimal128(bytes)) => {
                    self.raw(bytes)?;
                }
                (TypeKind::Text, ConstantValue::Text(idx)) => {
                    self.word(*idx)?;
                }
                (TypeKind::Function, ConstantValue::Closure(fn_idx)) => {
                    self.word(*fn_idx)?;
                }
                (TypeKind::Bytes, ConstantValue::Bytes(bytes)) => {
                    self.count(bytes.len(), MO_LIMIT_STRING_SIZE)?;
                    self.raw(bytes)?;
                }
                (TypeKind::BigInt, ConstantValue::BigInt { sign, magnitude }) => {
                    self.word(*sign as u32)?;
                    self.count(magnitude.len(), MO_LIMIT_STRING_SIZE)?;
                    self.raw(magnitude)?;
                }
                (
                    TypeKind::Range
                    | TypeKind::Tuple
                    | TypeKind::List
                    | TypeKind::Set
                    | TypeKind::Map
                    | TypeKind::Record
                    | TypeKind::Choice
                    | TypeKind::Env
                    | TypeKind::Capability,
                    _,
                ) => {
                    return Err(self.err_invalid("type has no literal encoding"));
                }
                _ => {
                    return Err(self.err_invalid("constant encoding disagrees with type"));
                }
            }
        }
        Ok(())
    }

    fn write_sections(&mut self) -> Result<(), WriteError> {
        let f = self.file;

        // Section 0: STRINGS
        self.count(f.strings.len(), MO_LIMIT_STRINGS)?;
        for s in &f.strings {
            let bytes = s.as_bytes();
            self.count(bytes.len(), MO_LIMIT_STRING_SIZE)?;
            self.raw(bytes)?;
        }

        // Section 1: TYPES
        self.write_types()?;

        // Section 2: LAYOUTS
        let layouts = &f.types.layouts;
        self.count(layouts.len(), MO_LIMIT_LAYOUTS)?;
        for layout in layouts {
            self.word(layout.kind as u32)?;
            self.word(layout.ty)?;
            match layout.kind {
                LayoutKind::Case => self.word(layout.case_index)?,
                LayoutKind::Closure => self.word(layout.function)?,
                LayoutKind::Record | LayoutKind::Tuple => {}
            }
            self.indices(&layout.fields, MO_LIMIT_FIELDS)?;
        }

        // Section 3: CONSTANTS
        self.write_constants()?;

        // Section 4: GLOBALS
        self.count(f.globals.len(), MO_LIMIT_GLOBALS)?;
        for g in &f.globals {
            self.word(g.ty)?;
            self.word(g.name)?;
            self.word(g.module)?;
        }

        // Section 5: INIT
        self.indices(&f.init, MO_LIMIT_INITS)?;

        // Section 6: FUNCTIONS
        self.count(f.functions.len(), MO_LIMIT_FUNCTIONS)?;
        for (i, func) in f.functions.iter().enumerate() {
            self.function = Some(i as u32);
            self.word(func.name)?;
            self.word(func.module)?;
            self.word(func.kind as u32)?;
            self.word(u32::from(func.spec_code))?;
            self.count(func.parameters.len(), MO_LIMIT_BYTECODE_PARAMETERS)?;
            for p in &func.parameters {
                self.word(p.ty)?;
                self.word(p.ownership as u32)?;
            }
            self.word(func.result_type)?;
            self.indices(&func.capture_types, MO_LIMIT_FIELDS)?;
            self.indices(&func.register_types, MO_LIMIT_FUNCTION_REGISTERS)?;
            self.indices(&func.code, MO_LIMIT_FUNCTION_CODE_WORDS)?;
        }
        self.function = None;

        // Section 7: MODULES
        self.count(f.modules.len(), MO_LIMIT_MODULES)?;
        for m in &f.modules {
            self.word(m.name)?;
            self.count(m.types.len(), MO_LIMIT_MODULE_NAMES)?;
            for exp in &m.types {
                self.word(exp.ty)?;
                self.word(u32::from(exp.contents_exposed))?;
            }
            self.indices(&m.functions, MO_LIMIT_MODULE_NAMES)?;
            self.indices(&m.globals, MO_LIMIT_MODULE_NAMES)?;
        }

        // Section 8: FILES
        self.count(f.files.len(), MO_LIMIT_FILES)?;
        for path in &f.files {
            self.count(path.len(), MO_LIMIT_STRING_SIZE)?;
            self.raw(path)?;
        }

        // Section 9: DEBUG
        self.count(f.debug.len(), MO_LIMIT_FUNCTIONS)?;
        for (i, d) in f.debug.iter().enumerate() {
            self.function = if i < f.functions.len() {
                Some(i as u32)
            } else {
                None
            };
            self.count(d.spans.len(), MO_LIMIT_DEBUG_ENTRIES)?;
            for s in &d.spans {
                self.word(s.code_offset)?;
                self.word(s.file)?;
                self.word(s.start)?;
                self.word(s.end)?;
            }
            self.count(d.names.len(), MO_LIMIT_DEBUG_ENTRIES)?;
            for n in &d.names {
                self.word(n.reg)?;
                self.word(n.name)?;
                self.word(n.code_start)?;
                self.word(n.code_end)?;
            }
            self.word(d.text)?;
            self.word(d.file)?;
            self.word(d.start)?;
            self.word(d.end)?;
            self.word(d.subject)?;
            self.word(d.expectation as u32)?;
            self.count(d.lines.len(), MO_LIMIT_DEBUG_ENTRIES)?;
            for l in &d.lines {
                self.word(l.file)?;
                self.word(l.start)?;
                self.word(l.end)?;
                self.word(l.text)?;
            }
        }
        self.function = None;

        // Section 10: SERVICES (present iff non-empty)
        if !f.services.is_empty() {
            self.count(f.services.len(), MO_LIMIT_SERVICES)?;
            for s in &f.services {
                self.word(s.module)?;
                self.word(s.state_type)?;
                self.word(s.start)?;
                self.word(s.token_env)?;
                self.count(s.tools.len(), MO_LIMIT_SERVICE_HANDLERS)?;
                for tool in &s.tools {
                    self.word(tool.name)?;
                    self.word(tool.function)?;
                    self.word(u32::from(tool.read_only))?;
                    self.indices(&tool.argument_types, MO_LIMIT_BYTECODE_PARAMETERS - 2)?;
                    self.word(tool.result_type)?;
                    self.word(tool.description)?;
                    self.indices(&tool.argument_names, MO_LIMIT_BYTECODE_PARAMETERS - 2)?;
                }
                self.count(s.timers.len(), MO_LIMIT_SERVICE_HANDLERS)?;
                for timer in &s.timers {
                    self.word(timer.function)?;
                    self.word(timer.interval)?;
                }
                self.count(s.shapes.len(), MO_LIMIT_TYPES)?;
                for shape in &s.shapes {
                    self.word(shape.ty)?;
                    self.word(u32::from(shape.is_struct))?;
                    self.count(shape.unnamed.len(), MO_LIMIT_FIELDS)?;
                    for &flag in &shape.unnamed {
                        self.word(u32::from(flag))?;
                    }
                }
            }
        }

        Ok(())
    }

    fn finish(mut self) -> Result<Vec<u8>, WriteError> {
        let flags = if self.file.services.is_empty() {
            BYTECODE_FLAGS_NONE
        } else {
            BYTECODE_FLAGS_SERVICES
        };
        let mut header = [0u8; BYTECODE_HEADER_SIZE];
        header[0..4].copy_from_slice(&BYTECODE_MAGIC);
        header[4..6].copy_from_slice(&BYTECODE_MAJOR.to_le_bytes());
        header[6..8].copy_from_slice(&BYTECODE_MINOR.to_le_bytes());
        header[8..12].copy_from_slice(&flags.to_le_bytes());
        self.raw(&header)?;
        self.write_sections()?;

        let checksum = fnv1a(FNV_OFFSET, &self.out[BYTECODE_HEADER_SIZE..]);
        self.out[12..20].copy_from_slice(&checksum.to_le_bytes());
        Ok(self.out)
    }
}

/// Encodes `file` to `.bc` v0.3 bytes and structurally validates the encoded bytes
/// via [`read::decode`] (matching `mo_bytecode_encode` in `src/bytecode/write.c`).
pub fn encode(file: &BytecodeFile) -> Result<Vec<u8>, WriteError> {
    let bytes = Writer::new(file).finish()?;
    if let Err(err) = read::decode(&bytes) {
        return Err(WriteError {
            reason: err.reason,
            kind: err.kind,
            function: None,
        });
    }
    Ok(bytes)
}

/// Encodes `file` to `.bc` v0.3 bytes and runs full section-9 verification
/// (matching `mo_bytecode_write` in `src/bytecode/write.c`).
pub fn write(file: &BytecodeFile) -> Result<Vec<u8>, WriteError> {
    let bytes = Writer::new(file).finish()?;
    let decoded = read::decode(&bytes).map_err(|err| WriteError {
        reason: err.reason,
        kind: err.kind,
        function: None,
    })?;
    verify::verify_module_with_site(&decoded).map_err(|(err, func)| WriteError {
        reason: err.reason,
        kind: err.kind,
        function: func.filter(|&i| (i as usize) < file.functions.len()),
    })?;
    Ok(bytes)
}
