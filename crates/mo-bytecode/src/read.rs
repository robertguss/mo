use std::collections::HashMap;

use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_BYTECODE_SIZE, MO_LIMIT_CHOICE_CASES,
    MO_LIMIT_CONSTANTS, MO_LIMIT_DEBUG_ENTRIES, MO_LIMIT_FIELDS, MO_LIMIT_FILES,
    MO_LIMIT_FUNCTION_CODE_WORDS, MO_LIMIT_FUNCTION_REGISTERS, MO_LIMIT_FUNCTIONS,
    MO_LIMIT_GLOBALS, MO_LIMIT_INITS, MO_LIMIT_LAYOUTS, MO_LIMIT_MODULE_NAMES, MO_LIMIT_MODULES,
    MO_LIMIT_SERVICE_HANDLERS, MO_LIMIT_SERVICES, MO_LIMIT_STRING_SIZE, MO_LIMIT_STRINGS,
    MO_LIMIT_TYPES,
};

use crate::{
    AnchorExpectation, BYTECODE_FLAGS_SERVICES, BYTECODE_HEADER_SIZE, BYTECODE_MAGIC,
    BYTECODE_MAJOR, BYTECODE_MINOR, BigIntSign, BytecodeFile, Capability, CodeSpan, ConstantDef,
    ConstantValue, DebugEntry, FNV_OFFSET, FunctionDef, FunctionKind, GlobalDef, Header, LayoutDef,
    LayoutKind, LoadError, ModuleDef, NO_INDEX, ParamOwnership, ParameterDef, RegisterName,
    ServiceDef, ServiceShape, ServiceTimer, ServiceTool, SpecLine, TypeCase, TypeDef, TypeExport,
    TypeField, TypeKind, fnv1a, types::TypeTable, validate,
};

#[derive(Debug, Clone, Copy)]
pub(crate) struct DebugPositions {
    pub spans: u32,
    pub names: u32,
    pub metadata: u32,
    pub lines: u32,
}

pub(crate) struct DecodedRaw {
    pub header: Header,
    pub strings: Vec<String>,
    pub canonical_names: Vec<u32>,
    pub types: Vec<TypeDef>,
    pub layouts: Vec<LayoutDef>,
    pub constants: Vec<ConstantDef>,
    pub globals: Vec<GlobalDef>,
    pub init: Vec<u32>,
    pub functions: Vec<FunctionDef>,
    pub modules: Vec<ModuleDef>,
    pub files: Vec<Vec<u8>>,
    pub debug: Vec<DebugEntry>,
    pub services: Vec<ServiceDef>,
    pub code_positions: Vec<u32>,
    pub debug_positions: Vec<DebugPositions>,
}

struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
    field: u32,
}

impl<'a> Reader<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self {
            bytes,
            offset: BYTECODE_HEADER_SIZE,
            field: BYTECODE_HEADER_SIZE as u32,
        }
    }

    fn remaining(&self) -> usize {
        self.bytes.len() - self.offset
    }

    fn word(&mut self) -> Result<u32, LoadError> {
        self.field = self.offset as u32;
        if self.remaining() < 4 {
            return Err(LoadError::invalid(self.field, "truncated word"));
        }
        let val = u32::from_le_bytes([
            self.bytes[self.offset],
            self.bytes[self.offset + 1],
            self.bytes[self.offset + 2],
            self.bytes[self.offset + 3],
        ]);
        self.offset += 4;
        Ok(val)
    }

    fn count(&mut self, limit: u64, min_entry_bytes: usize) -> Result<usize, LoadError> {
        let n = self.word()?;
        if u64::from(n) > limit {
            return Err(LoadError::limit(
                self.field,
                "count past limit or remaining bytes",
            ));
        }
        if (n as usize) > self.remaining() / min_entry_bytes {
            return Err(LoadError::invalid(
                self.field,
                "count past limit or remaining bytes",
            ));
        }
        Ok(n as usize)
    }

    fn indices(&mut self, limit: u64) -> Result<Vec<u32>, LoadError> {
        let n = self.count(limit, 4)?;
        let mut out = Vec::with_capacity(n);
        for _ in 0..n {
            out.push(self.word()?);
        }
        Ok(out)
    }

    fn boolean(&mut self) -> Result<bool, LoadError> {
        let v = self.word()?;
        match v {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(LoadError::invalid(self.field, "invalid boolean")),
        }
    }

    fn blob(&mut self) -> Result<Vec<u8>, LoadError> {
        let n = self.word()?;
        if u64::from(n) > MO_LIMIT_STRING_SIZE {
            return Err(LoadError::limit(
                self.field,
                "byte length past limit or remaining bytes",
            ));
        }
        let len = n as usize;
        if len > self.remaining() {
            return Err(LoadError::invalid(
                self.field,
                "byte length past limit or remaining bytes",
            ));
        }
        let data = self.bytes[self.offset..self.offset + len].to_vec();
        self.offset += len;
        Ok(data)
    }

    fn utf8_string(&mut self) -> Result<String, LoadError> {
        let n = self.word()?;
        if u64::from(n) > MO_LIMIT_STRING_SIZE {
            return Err(LoadError::limit(
                self.field,
                "byte length past limit or remaining bytes",
            ));
        }
        let len = n as usize;
        if len > self.remaining() {
            return Err(LoadError::invalid(
                self.field,
                "byte length past limit or remaining bytes",
            ));
        }
        let payload_start = self.offset as u32;
        let slice = &self.bytes[self.offset..self.offset + len];
        let s = std::str::from_utf8(slice)
            .map_err(|_| LoadError::invalid(payload_start, "invalid UTF-8 string"))?;
        self.offset += len;
        Ok(s.to_string())
    }

    fn canonical_name(
        &mut self,
        strings: &[String],
        canonical: &[u32],
        optional: bool,
    ) -> Result<Option<(u32, String)>, LoadError> {
        let idx = self.word()?;
        if optional && idx == NO_INDEX {
            return Ok(None);
        }
        if (idx as usize) >= strings.len() {
            return Err(LoadError::invalid(
                self.field,
                "name string index out of range",
            ));
        }
        let canon = canonical[idx as usize];
        if canon == NO_INDEX {
            return Err(LoadError::invalid(
                self.field,
                "name is empty or contains NUL",
            ));
        }
        if canon != idx {
            return Err(LoadError::invalid(
                self.field,
                "name does not use lowest string index",
            ));
        }
        Ok(Some((idx, strings[idx as usize].clone())))
    }

    fn type_fields(
        &mut self,
        strings: &[String],
        canonical: &[u32],
        optional: bool,
    ) -> Result<Vec<TypeField>, LoadError> {
        let n = self.count(MO_LIMIT_FIELDS, 8)?;
        let mut out = Vec::with_capacity(n);
        for _ in 0..n {
            let name = self.canonical_name(strings, canonical, optional && n == 1)?;
            let ty = self.word()?;
            let (name_idx, name_str) = match name {
                Some((i, s)) => (Some(i), Some(s)),
                None => (None, None),
            };
            out.push(TypeField {
                name_idx,
                name: name_str,
                ty,
            });
        }
        Ok(out)
    }
}

fn build_canonical_names(strings: &[String]) -> Vec<u32> {
    let mut lowest: HashMap<&[u8], u32> = HashMap::with_capacity(strings.len());
    let mut canonical = vec![NO_INDEX; strings.len()];
    for (i, s) in strings.iter().enumerate() {
        let bytes = s.as_bytes();
        if bytes.is_empty() || bytes.contains(&0) {
            canonical[i] = NO_INDEX;
            continue;
        }
        let idx = *lowest.entry(bytes).or_insert(i as u32);
        canonical[i] = idx;
    }
    canonical
}

pub fn decode(bytes: &[u8]) -> Result<BytecodeFile, LoadError> {
    if bytes.len() as u64 > MO_LIMIT_BYTECODE_SIZE {
        return Err(LoadError::limit(0, "invalid header or bytecode size"));
    }
    if bytes.len() < BYTECODE_HEADER_SIZE || bytes[0..4] != BYTECODE_MAGIC {
        return Err(LoadError::invalid(0, "invalid header or bytecode size"));
    }
    let major = u16::from_le_bytes([bytes[4], bytes[5]]);
    let minor = u16::from_le_bytes([bytes[6], bytes[7]]);
    if major != BYTECODE_MAJOR || minor != BYTECODE_MINOR {
        let offset = if major != BYTECODE_MAJOR { 4 } else { 6 };
        return Err(LoadError::invalid(offset, "unsupported version or flags"));
    }
    let flags = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]);
    if flags > BYTECODE_FLAGS_SERVICES {
        return Err(LoadError::invalid(8, "unsupported version or flags"));
    }
    let expected = u64::from_le_bytes([
        bytes[12], bytes[13], bytes[14], bytes[15], bytes[16], bytes[17], bytes[18], bytes[19],
    ]);
    let checksum = fnv1a(FNV_OFFSET, &bytes[BYTECODE_HEADER_SIZE..]);
    if checksum != expected {
        return Err(LoadError::invalid(12, "checksum mismatch"));
    }

    let header = Header {
        magic: BYTECODE_MAGIC,
        major,
        minor,
        flags,
        checksum,
    };

    let mut r = Reader::new(bytes);

    // Section 0: STRINGS
    let string_count = r.count(MO_LIMIT_STRINGS, 4)?;
    let mut strings = Vec::with_capacity(string_count);
    for _ in 0..string_count {
        strings.push(r.utf8_string()?);
    }
    let canonical_names = build_canonical_names(&strings);

    // Section 1: TYPES
    let type_count = r.count(MO_LIMIT_TYPES, 4)?;
    let mut types = Vec::with_capacity(type_count);
    for _ in 0..type_count {
        let raw_kind = r.word()?;
        let kind = TypeKind::from_u32(raw_kind)
            .ok_or_else(|| LoadError::invalid(r.field, "unknown type kind"))?;
        let mut td = TypeDef {
            kind,
            parameters: Vec::new(),
            result: 0,
            function: 0,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        };
        match kind {
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
                let count_offset = r.offset as u32;
                let limit = if kind == TypeKind::Function {
                    MO_LIMIT_BYTECODE_PARAMETERS
                } else {
                    MO_LIMIT_FIELDS
                };
                let params = r.indices(limit)?;
                let invalid_count = (kind == TypeKind::Tuple && params.len() < 2)
                    || ((kind == TypeKind::List || kind == TypeKind::Set) && params.len() != 1)
                    || (kind == TypeKind::Map && params.len() != 2);
                if invalid_count {
                    return Err(LoadError::invalid(
                        count_offset,
                        "invalid type parameter count",
                    ));
                }
                td.parameters = params;
                if kind == TypeKind::Function {
                    td.result = r.word()?;
                }
            }
            TypeKind::Record => {
                let (idx, name) = r
                    .canonical_name(&strings, &canonical_names, false)?
                    .ok_or_else(|| LoadError::invalid(r.field, "missing record name"))?;
                td.name_idx = Some(idx);
                td.name = Some(name);
                td.fields = r.type_fields(&strings, &canonical_names, false)?;
            }
            TypeKind::Choice => {
                let (idx, name) = r
                    .canonical_name(&strings, &canonical_names, false)?
                    .ok_or_else(|| LoadError::invalid(r.field, "missing choice name"))?;
                td.name_idx = Some(idx);
                td.name = Some(name);
                let case_count = r.count(MO_LIMIT_CHOICE_CASES, 8)?;
                if case_count == 0 {
                    return Err(LoadError::invalid(r.field, "choice has no cases"));
                }
                let mut cases = Vec::with_capacity(case_count);
                for _ in 0..case_count {
                    let (c_idx, c_name) = r
                        .canonical_name(&strings, &canonical_names, false)?
                        .ok_or_else(|| LoadError::invalid(r.field, "missing choice case name"))?;
                    let fields = r.type_fields(&strings, &canonical_names, true)?;
                    let unnamed = fields.len() == 1 && fields[0].name.is_none();
                    td.has_fields |= !fields.is_empty();
                    cases.push(TypeCase {
                        name_idx: c_idx,
                        name: c_name,
                        fields,
                        unnamed,
                    });
                }
                td.cases = cases;
            }
            TypeKind::Env => {
                td.function = r.word()?;
            }
            TypeKind::Capability => {
                let raw_cap = r.word()?;
                let cap = Capability::from_u32(raw_cap)
                    .ok_or_else(|| LoadError::invalid(r.field, "unknown capability"))?;
                td.capability = Some(cap);
            }
        }
        types.push(td);
    }

    // Section 2: LAYOUTS
    let layout_count = r.count(MO_LIMIT_LAYOUTS, 12)?;
    let mut layouts = Vec::with_capacity(layout_count);
    for _ in 0..layout_count {
        let raw_kind = r.word()?;
        let kind = LayoutKind::from_u32(raw_kind)
            .ok_or_else(|| LoadError::invalid(r.field, "unknown layout kind"))?;
        let ty = r.word()?;
        let case_index = if kind == LayoutKind::Case {
            r.word()?
        } else {
            NO_INDEX
        };
        let function = if kind == LayoutKind::Closure {
            r.word()?
        } else {
            NO_INDEX
        };
        let fields = r.indices(MO_LIMIT_FIELDS)?;
        layouts.push(LayoutDef {
            kind,
            ty,
            case_index,
            function,
            fields,
        });
    }

    // Section 3: CONSTANTS
    let constant_count = r.count(MO_LIMIT_CONSTANTS, 8)?;
    let mut constants = Vec::with_capacity(constant_count);
    for _ in 0..constant_count {
        let ty = r.word()?;
        if (ty as usize) >= types.len() {
            return Err(LoadError::invalid(r.field, "constant type out of range"));
        }
        let kind = types[ty as usize].kind;
        let value = if (kind as u32) <= (TypeKind::Decimal as u32) {
            let n = if kind == TypeKind::Decimal { 16 } else { 8 };
            if n > r.remaining() {
                return Err(LoadError::invalid(r.offset as u32, "truncated literal"));
            }
            if kind == TypeKind::Decimal {
                let mut buf = [0u8; 16];
                buf.copy_from_slice(&r.bytes[r.offset..r.offset + 16]);
                r.offset += 16;
                ConstantValue::Decimal128(buf)
            } else {
                let mut buf = [0u8; 8];
                buf.copy_from_slice(&r.bytes[r.offset..r.offset + 8]);
                r.offset += 8;
                ConstantValue::Immediate64(buf)
            }
        } else if kind == TypeKind::Text {
            ConstantValue::Text(r.word()?)
        } else if kind == TypeKind::Bytes {
            ConstantValue::Bytes(r.blob()?)
        } else if kind == TypeKind::BigInt {
            let raw_sign = r.word()?;
            let sign = BigIntSign::from_u32(raw_sign)
                .ok_or_else(|| LoadError::invalid(r.field, "invalid bigint sign"))?;
            let magnitude = r.blob()?;
            ConstantValue::BigInt { sign, magnitude }
        } else if kind == TypeKind::Function {
            ConstantValue::Closure(r.word()?)
        } else {
            return Err(LoadError::invalid(r.field, "type has no literal encoding"));
        };
        constants.push(ConstantDef { ty, value });
    }

    // Section 4: GLOBALS
    let global_count = r.count(MO_LIMIT_GLOBALS, 12)?;
    let mut globals = Vec::with_capacity(global_count);
    for _ in 0..global_count {
        let ty = r.word()?;
        let name = r.word()?;
        let module = r.word()?;
        globals.push(GlobalDef { ty, name, module });
    }

    // Section 5: INIT
    let init = r.indices(MO_LIMIT_INITS)?;

    // Section 6: FUNCTIONS
    let function_count = r.count(MO_LIMIT_FUNCTIONS, 36)?;
    let mut functions = Vec::with_capacity(function_count);
    let mut code_positions = Vec::with_capacity(function_count);
    for _ in 0..function_count {
        let name = r.word()?;
        let module = r.word()?;
        let raw_kind = r.word()?;
        let kind = FunctionKind::from_u32(raw_kind)
            .ok_or_else(|| LoadError::invalid(r.field, "unknown function kind"))?;
        let spec_code = r.boolean()?;
        let param_count = r.count(MO_LIMIT_BYTECODE_PARAMETERS, 8)?;
        let mut parameters = Vec::with_capacity(param_count);
        for _ in 0..param_count {
            let ty = r.word()?;
            let raw_own = r.word()?;
            let ownership = ParamOwnership::from_u32(raw_own)
                .ok_or_else(|| LoadError::invalid(r.field, "invalid parameter ownership"))?;
            parameters.push(ParameterDef { ty, ownership });
        }
        let result_type = r.word()?;
        let capture_types = r.indices(MO_LIMIT_FIELDS)?;
        let register_types = r.indices(MO_LIMIT_FUNCTION_REGISTERS)?;
        code_positions.push((r.offset + 4) as u32);
        let code = r.indices(MO_LIMIT_FUNCTION_CODE_WORDS)?;
        functions.push(FunctionDef {
            name,
            module,
            kind,
            spec_code,
            parameters,
            result_type,
            capture_types,
            register_types,
            code,
        });
    }

    // Section 7: MODULES
    let module_count = r.count(MO_LIMIT_MODULES, 16)?;
    let mut modules = Vec::with_capacity(module_count);
    for _ in 0..module_count {
        let name = r.word()?;
        let exp_count = r.count(MO_LIMIT_MODULE_NAMES, 8)?;
        let mut exp_types = Vec::with_capacity(exp_count);
        for _ in 0..exp_count {
            let ty = r.word()?;
            let contents_exposed = r.boolean()?;
            exp_types.push(TypeExport {
                ty,
                contents_exposed,
            });
        }
        let funcs = r.indices(MO_LIMIT_MODULE_NAMES)?;
        let globs = r.indices(MO_LIMIT_MODULE_NAMES)?;
        modules.push(ModuleDef {
            name,
            types: exp_types,
            functions: funcs,
            globals: globs,
        });
    }

    // Section 8: FILES
    let files_start = r.offset;
    let file_count = r.count(MO_LIMIT_FILES, 5)?;
    let mut files = Vec::with_capacity(file_count);
    for _ in 0..file_count {
        let data = r.blob()?;
        if data.is_empty() || data.contains(&0) {
            return Err(LoadError::invalid(
                r.field,
                "filename must be nonempty and contain no NUL",
            ));
        }
        files.push(data);
    }
    let files_end = r.offset;

    // Section 9: DEBUG
    let debug_count = r.count(MO_LIMIT_FUNCTIONS, 36)?;
    let mut debug = Vec::with_capacity(debug_count);
    let mut debug_positions = Vec::with_capacity(debug_count);
    for _ in 0..debug_count {
        let spans_pos = r.offset as u32;
        let span_count = r.count(MO_LIMIT_DEBUG_ENTRIES, 16)?;
        let mut spans = Vec::with_capacity(span_count);
        for _ in 0..span_count {
            let code_offset = r.word()?;
            let file = r.word()?;
            let start = r.word()?;
            let end = r.word()?;
            spans.push(CodeSpan {
                code_offset,
                file,
                start,
                end,
            });
        }
        let names_pos = r.offset as u32;
        let name_count = r.count(MO_LIMIT_DEBUG_ENTRIES, 16)?;
        let mut names = Vec::with_capacity(name_count);
        for _ in 0..name_count {
            let reg = r.word()?;
            let name = r.word()?;
            let code_start = r.word()?;
            let code_end = r.word()?;
            names.push(RegisterName {
                reg,
                name,
                code_start,
                code_end,
            });
        }
        let metadata_pos = r.offset as u32;
        let text = r.word()?;
        let file = r.word()?;
        let start = r.word()?;
        let end = r.word()?;
        let subject = r.word()?;
        let raw_exp = r.word()?;
        let expectation = AnchorExpectation::from_u32(raw_exp)
            .ok_or_else(|| LoadError::invalid(r.field, "invalid anchor expectation"))?;
        let lines_pos = r.offset as u32;
        let line_count = r.count(MO_LIMIT_DEBUG_ENTRIES, 16)?;
        let mut lines = Vec::with_capacity(line_count);
        for _ in 0..line_count {
            let l_file = r.word()?;
            let l_start = r.word()?;
            let l_end = r.word()?;
            let l_text = r.word()?;
            lines.push(SpecLine {
                file: l_file,
                start: l_start,
                end: l_end,
                text: l_text,
            });
        }
        debug_positions.push(DebugPositions {
            spans: spans_pos,
            names: names_pos,
            metadata: metadata_pos,
            lines: lines_pos,
        });
        debug.push(DebugEntry {
            spans,
            names,
            text,
            file,
            start,
            end,
            subject,
            expectation,
            lines,
        });
    }

    // Section 10: SERVICES (optional, present iff flags == 1)
    let mut services = Vec::new();
    if flags == BYTECODE_FLAGS_SERVICES {
        let service_count = r.count(MO_LIMIT_SERVICES, 28)?;
        if service_count == 0 {
            return Err(LoadError::invalid(r.field, "empty service extension"));
        }
        services.reserve(service_count);
        for _ in 0..service_count {
            let module = r.word()?;
            let state_type = r.word()?;
            let start = r.word()?;
            let token_env = r.word()?;
            let tool_count = r.count(MO_LIMIT_SERVICE_HANDLERS, 28)?;
            let mut tools = Vec::with_capacity(tool_count);
            for _ in 0..tool_count {
                let t_name = r.word()?;
                let function = r.word()?;
                let read_only = r.boolean()?;
                let argument_types = r.indices(MO_LIMIT_BYTECODE_PARAMETERS - 2)?;
                let result_type = r.word()?;
                let description = r.word()?;
                let argument_names = r.indices(MO_LIMIT_BYTECODE_PARAMETERS - 2)?;
                tools.push(ServiceTool {
                    name: t_name,
                    function,
                    read_only,
                    argument_types,
                    result_type,
                    description,
                    argument_names,
                });
            }
            let timer_count = r.count(MO_LIMIT_SERVICE_HANDLERS, 8)?;
            let mut timers = Vec::with_capacity(timer_count);
            for _ in 0..timer_count {
                let function = r.word()?;
                let interval = r.word()?;
                timers.push(ServiceTimer { function, interval });
            }
            let shape_count = r.count(MO_LIMIT_TYPES, 12)?;
            let mut shapes = Vec::with_capacity(shape_count);
            for _ in 0..shape_count {
                let ty = r.word()?;
                let is_struct = r.boolean()?;
                let case_count = r.count(MO_LIMIT_FIELDS, 4)?;
                let mut unnamed = Vec::with_capacity(case_count);
                for _ in 0..case_count {
                    unnamed.push(r.boolean()?);
                }
                shapes.push(ServiceShape {
                    ty,
                    is_struct,
                    unnamed,
                });
            }
            services.push(ServiceDef {
                module,
                state_type,
                start,
                token_env,
                tools,
                timers,
                shapes,
            });
        }
    }

    if r.offset != bytes.len() {
        return Err(LoadError::invalid(r.offset as u32, "trailing bytes"));
    }

    let raw = DecodedRaw {
        header,
        strings,
        canonical_names,
        types,
        layouts,
        constants,
        globals,
        init,
        functions,
        modules,
        files,
        debug,
        services,
        code_positions,
        debug_positions,
    };

    validate::validate_raw(&raw)?;

    let type_table = TypeTable::finish(raw.types, raw.layouts)?;
    let program_checksum = fnv1a(
        fnv1a(FNV_OFFSET, &bytes[BYTECODE_HEADER_SIZE..files_start]),
        &bytes[files_end..],
    );

    Ok(BytecodeFile {
        header: raw.header,
        program_checksum,
        strings: raw.strings,
        types: type_table,
        constants: raw.constants,
        globals: raw.globals,
        init: raw.init,
        functions: raw.functions,
        code_offsets: raw.code_positions,
        modules: raw.modules,
        files: raw.files,
        debug: raw.debug,
        services: raw.services,
    })
}
