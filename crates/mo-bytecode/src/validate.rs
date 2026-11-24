use std::collections::HashSet;

use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_CHOICE_CASES, MO_LIMIT_FIELDS, MO_LIMIT_FILE_SIZE,
    MO_LIMIT_FUNCTION_INSTRUCTIONS, MO_LIMIT_SERVICE_HANDLERS, MO_TIME_MAX_US, MO_TIME_MIN_US,
};
use mo_def::{HOSTOP_COUNT, Opcode, OperandKind, PRIM_COUNT};

use crate::read::DecodedRaw;
use crate::types::compute_case_shape;
use crate::{
    AnchorExpectation, BYTECODE_HEADER_SIZE, BigIntSign, CaseShape, ConstantValue, FunctionDef,
    FunctionKind, LayoutDef, LayoutKind, LoadError, NO_INDEX, OPCODE_MASK, OPCODE_VARIABLE_SHIFT,
    ParamOwnership, ServiceDef, TypeDef, TypeField, TypeKind,
};

const PAYLOAD_OFFSET: u32 = BYTECODE_HEADER_SIZE as u32;

fn refuse(reason: &'static str) -> LoadError {
    LoadError::invalid(PAYLOAD_OFFSET, reason)
}

fn is_named(raw: &DecodedRaw, idx: u32) -> bool {
    (idx as usize) < raw.strings.len() && raw.canonical_names[idx as usize] != NO_INDEX
}

fn is_nominal(t: &TypeDef) -> bool {
    matches!(t.kind, TypeKind::Record | TypeKind::Choice)
}

fn closure_signature_matches(raw: &DecodedRaw, ty: u32, func: u32) -> bool {
    if (ty as usize) >= raw.types.len() || (func as usize) >= raw.functions.len() {
        return false;
    }
    let t = &raw.types[ty as usize];
    let f = &raw.functions[func as usize];
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

pub(crate) fn validate_raw(raw: &DecodedRaw) -> Result<(), LoadError> {
    validate_functions(raw)?;
    validate_modules(raw)?;
    validate_types(raw)?;
    validate_layouts(raw)?;
    validate_constants(raw)?;
    validate_services(raw)?;
    for i in 0..raw.functions.len() {
        validate_code_and_debug(raw, i)?;
    }
    Ok(())
}

fn validate_functions(raw: &DecodedRaw) -> Result<(), LoadError> {
    let type_count = raw.types.len() as u32;
    let mod_count = raw.modules.len() as u32;
    for (i, f) in raw.functions.iter().enumerate() {
        if !is_named(raw, f.name) || f.module >= mod_count || f.result_type >= type_count {
            return Err(refuse("function name, module or result out of range"));
        }
        let slots = f.parameters.len() + usize::from(f.kind == FunctionKind::Closure);
        if (slots as u64) > MO_LIMIT_BYTECODE_PARAMETERS
            || f.register_types.len() < slots
            || (f.kind != FunctionKind::Closure && !f.capture_types.is_empty())
        {
            return Err(refuse("invalid function parameter/register/capture counts"));
        }
        let spec = (f.kind as u32) >= (FunctionKind::Test as u32);
        if f.kind != FunctionKind::Closure && f.spec_code != spec {
            return Err(refuse("function spec flag disagrees with kind"));
        }
        let result_kind = raw.types[f.result_type as usize].kind;
        let bad_sig = match f.kind {
            FunctionKind::Init | FunctionKind::Test | FunctionKind::Example => {
                !f.parameters.is_empty() || result_kind != TypeKind::Unit
            }
            FunctionKind::Promise => f.parameters.is_empty() || result_kind != TypeKind::Int,
            FunctionKind::Anchor => !f.parameters.is_empty() || result_kind != TypeKind::Int,
            FunctionKind::Function | FunctionKind::Closure => false,
        };
        if bad_sig {
            return Err(refuse("invalid function kind signature"));
        }
        if f.register_types.iter().any(|&rt| rt >= type_count) {
            return Err(refuse("register type out of range"));
        }
        for (j, p) in f.parameters.iter().enumerate() {
            if p.ty != f.register_types[j]
                || (f.kind != FunctionKind::Function && p.ownership != ParamOwnership::Owned)
            {
                return Err(refuse("invalid parameter type or borrowing"));
            }
        }
        if f.kind == FunctionKind::Closure {
            let env_ty = &raw.types[f.register_types[f.parameters.len()] as usize];
            if env_ty.kind != TypeKind::Env || env_ty.function != i as u32 {
                return Err(refuse("closure env mismatch"));
            }
        }
        if f.capture_types.iter().any(|&ct| ct >= type_count) {
            return Err(refuse("capture type out of range"));
        }
    }
    Ok(())
}

fn validate_modules(raw: &DecodedRaw) -> Result<(), LoadError> {
    if raw.init.len() != raw.globals.len() || raw.debug.len() != raw.functions.len() {
        return Err(refuse("init/global or debug/function count mismatch"));
    }
    let mut inits_seen = vec![false; raw.functions.len()];
    for &fn_idx in &raw.init {
        if (fn_idx as usize) >= raw.functions.len()
            || raw.functions[fn_idx as usize].kind != FunctionKind::Init
            || inits_seen[fn_idx as usize]
        {
            return Err(refuse("invalid or repeated init function"));
        }
        inits_seen[fn_idx as usize] = true;
    }
    for (i, f) in raw.functions.iter().enumerate() {
        if f.kind == FunctionKind::Init && !inits_seen[i] {
            return Err(refuse("init function absent from init section"));
        }
    }
    for g in &raw.globals {
        if (g.ty as usize) >= raw.types.len()
            || !is_named(raw, g.name)
            || (g.module as usize) >= raw.modules.len()
        {
            return Err(refuse("global type, name or module out of range"));
        }
    }
    let mut globals_seen = vec![false; raw.globals.len()];
    for (i, m) in raw.modules.iter().enumerate() {
        if !is_named(raw, m.name) {
            return Err(refuse("invalid module name"));
        }
        let mut exp_seen = HashSet::with_capacity(m.types.len());
        for exp in &m.types {
            if (exp.ty as usize) >= raw.types.len() {
                return Err(refuse("exposed type out of range"));
            }
            if !exp_seen.insert(exp.ty) {
                return Err(refuse("repeated exposed type"));
            }
        }
        for &fn_idx in &m.functions {
            if (fn_idx as usize) >= raw.functions.len()
                || raw.functions[fn_idx as usize].module != i as u32
            {
                return Err(refuse("exposed function out of range/module"));
            }
        }
        for &g_idx in &m.globals {
            if (g_idx as usize) >= raw.globals.len()
                || raw.globals[g_idx as usize].module != i as u32
                || globals_seen[g_idx as usize]
            {
                return Err(refuse("module global out of range/module or repeated"));
            }
            globals_seen[g_idx as usize] = true;
        }
    }
    if globals_seen.iter().any(|&seen| !seen) {
        return Err(refuse("global absent from module"));
    }
    Ok(())
}

fn validate_field_names(
    raw: &DecodedRaw,
    fields: &[TypeField],
    optional: bool,
) -> Result<(), LoadError> {
    let type_count = raw.types.len() as u32;
    let mut seen = HashSet::with_capacity(fields.len());
    for f in fields {
        if f.ty >= type_count {
            return Err(refuse("field type out of range"));
        }
        if optional && fields.len() == 1 && f.name_idx.is_none() {
            return Ok(());
        }
        let Some(name_idx) = f.name_idx else {
            return Err(refuse("invalid field name"));
        };
        let canon = raw.canonical_names[name_idx as usize];
        if canon == NO_INDEX {
            return Err(refuse("invalid field name"));
        }
        if !seen.insert(canon) {
            return Err(refuse("duplicate field name"));
        }
    }
    Ok(())
}

fn validate_types(raw: &DecodedRaw) -> Result<(), LoadError> {
    let type_count = raw.types.len();
    let mut state = vec![0u8; type_count];
    let mut structural: Vec<usize> = Vec::new();

    for (i, t) in raw.types.iter().enumerate() {
        if is_nominal(t) {
            if t.kind == TypeKind::Record {
                validate_field_names(raw, &t.fields, false)?;
            } else {
                let mut case_names = HashSet::with_capacity(t.cases.len());
                for c in &t.cases {
                    let canon = raw.canonical_names[c.name_idx as usize];
                    if !case_names.insert(canon) {
                        return Err(refuse("duplicate case name"));
                    }
                    validate_field_names(raw, &c.fields, true)?;
                }
            }
            state[i] = 2;
        } else {
            structural.push(i);
            if t.parameters.iter().any(|&p| (p as usize) >= type_count) {
                return Err(refuse("type index out of range"));
            }
            if (t.kind == TypeKind::Function && (t.result as usize) >= type_count)
                || (t.kind == TypeKind::Env
                    && ((t.function as usize) >= raw.functions.len()
                        || raw.functions[t.function as usize].kind != FunctionKind::Closure))
            {
                return Err(refuse("type result or env function out of range/kind"));
            }
        }
    }

    // Check structural cycles with explicit stack.
    let mut stack: Vec<(usize, usize)> = Vec::new();
    for root in 0..type_count {
        if state[root] == 2 {
            continue;
        }
        stack.push((root, 0));
        state[root] = 1;
        while let Some((node, next)) = stack.last_mut() {
            let t = &raw.types[*node];
            let deps = t.parameters.len() + usize::from(t.kind == TypeKind::Function);
            if *next < deps {
                let dep = if *next < t.parameters.len() {
                    t.parameters[*next] as usize
                } else {
                    t.result as usize
                };
                *next += 1;
                if state[dep] == 1 {
                    return Err(refuse("structural type cycle"));
                }
                if state[dep] == 0 {
                    state[dep] = 1;
                    stack.push((dep, 0));
                }
            } else {
                state[*node] = 2;
                stack.pop();
            }
        }
    }

    // Check duplicate interned structural types.
    structural.sort_by(|&a, &b| type_key(&raw.types[a]).cmp(&type_key(&raw.types[b])));
    for pair in structural.windows(2) {
        if type_key(&raw.types[pair[0]]) == type_key(&raw.types[pair[1]]) {
            return Err(refuse("duplicate interned type"));
        }
    }

    Ok(())
}

fn type_key(t: &TypeDef) -> (u32, u32, u32, u32, &[u32]) {
    let cap = t.capability.map_or(0, |c| c as u32);
    (t.kind as u32, t.result, t.function, cap, &t.parameters)
}

fn validate_layouts(raw: &DecodedRaw) -> Result<(), LoadError> {
    let type_count = raw.types.len() as u32;
    let mut keys: Vec<(u32, u32, u32, u32)> = Vec::with_capacity(raw.layouts.len());

    for l in &raw.layouts {
        if l.ty >= type_count {
            return Err(refuse("layout type out of range"));
        }
        if l.fields.iter().any(|&ft| ft >= type_count) {
            return Err(refuse("layout field type out of range"));
        }
        let t = &raw.types[l.ty as usize];
        match l.kind {
            LayoutKind::Record => {
                if t.kind != TypeKind::Record {
                    return Err(refuse("layout kind disagrees with type"));
                }
                check_layout_fields(l, &t.fields)?;
            }
            LayoutKind::Tuple => {
                if t.kind != TypeKind::Tuple || l.fields != t.parameters {
                    return Err(refuse("tuple layout fields mismatch"));
                }
            }
            LayoutKind::Case => {
                if t.kind != TypeKind::Choice
                    || (l.case_index as usize) >= t.cases.len()
                    || compute_case_shape(&raw.types, l.ty as usize, l.case_index as usize)
                        != CaseShape::Boxed
                {
                    return Err(refuse("invalid case layout"));
                }
                check_layout_fields(l, &t.cases[l.case_index as usize].fields)?;
            }
            LayoutKind::Closure => {
                if !closure_signature_matches(raw, l.ty, l.function) {
                    return Err(refuse("closure layout signature mismatch"));
                }
                let f = &raw.functions[l.function as usize];
                if l.fields != f.capture_types {
                    return Err(refuse("closure layout captures mismatch"));
                }
            }
        }
        keys.push((l.kind as u32, l.ty, l.case_index, l.function));
    }

    keys.sort_unstable();
    for pair in keys.windows(2) {
        if pair[0] == pair[1] {
            return Err(refuse("duplicate layout"));
        }
    }
    Ok(())
}

fn check_layout_fields(l: &LayoutDef, expected: &[TypeField]) -> Result<(), LoadError> {
    if l.fields.len() != expected.len() {
        return Err(refuse("layout field count mismatch"));
    }
    for (&got, want) in l.fields.iter().zip(expected) {
        if got != want.ty {
            return Err(refuse("layout field type mismatch"));
        }
    }
    Ok(())
}

fn validate_constants(raw: &DecodedRaw) -> Result<(), LoadError> {
    for k in &raw.constants {
        let kind = raw.types[k.ty as usize].kind;
        match &k.value {
            ConstantValue::Immediate64(bytes) => {
                let value = u64::from_le_bytes(*bytes);
                let invalid_scalar = (kind == TypeKind::Unit && value != 0)
                    || (kind == TypeKind::Bool && value > 1)
                    || (kind == TypeKind::Byte && value > u64::from(u8::MAX))
                    || (kind == TypeKind::Char
                        && (value >= 0x11_0000 || (0xd800..=0xdfff).contains(&value)));
                if invalid_scalar {
                    return Err(refuse("invalid scalar literal"));
                }
                if kind == TypeKind::Time {
                    let signed = i64::from_le_bytes(*bytes);
                    if !(MO_TIME_MIN_US..=MO_TIME_MAX_US).contains(&signed) {
                        return Err(refuse("time literal out of range"));
                    }
                }
            }
            ConstantValue::Decimal128(bytes) => {
                let signed = i128::from_le_bytes(*bytes);
                if signed == i128::MIN {
                    return Err(refuse("decimal literal out of range"));
                }
            }
            ConstantValue::Text(str_idx) => {
                if (*str_idx as usize) >= raw.strings.len() {
                    return Err(refuse("text string index out of range"));
                }
            }
            ConstantValue::Bytes(_) => {}
            ConstantValue::BigInt { sign, magnitude } => {
                let is_zero = *sign == BigIntSign::Zero;
                if is_zero != magnitude.is_empty() || magnitude.last() == Some(&0) {
                    return Err(refuse("noncanonical bigint literal"));
                }
            }
            ConstantValue::Closure(fn_idx) => {
                if !closure_signature_matches(raw, k.ty, *fn_idx)
                    || !raw.functions[*fn_idx as usize].capture_types.is_empty()
                {
                    return Err(refuse("invalid closure literal"));
                }
            }
        }
    }
    Ok(())
}

fn validate_operand(
    raw: &DecodedRaw,
    f: &FunctionDef,
    kind: OperandKind,
    val: u32,
    starts: &[bool],
) -> bool {
    match kind {
        OperandKind::Register => (val as usize) < f.register_types.len(),
        OperandKind::Type => (val as usize) < raw.types.len(),
        OperandKind::Layout => (val as usize) < raw.layouts.len(),
        OperandKind::Constant => (val as usize) < raw.constants.len(),
        OperandKind::Global => (val as usize) < raw.globals.len(),
        OperandKind::Function => (val as usize) < raw.functions.len(),
        OperandKind::Primitive => (val as usize) < PRIM_COUNT,
        OperandKind::Host => (val as usize) < HOSTOP_COUNT,
        OperandKind::Target => (val as usize) < f.code.len() && starts[val as usize],
        OperandKind::Field => u64::from(val) < MO_LIMIT_FIELDS,
        OperandKind::Case => u64::from(val) < MO_LIMIT_CHOICE_CASES,
        OperandKind::Index => true,
        OperandKind::None => false,
    }
}

fn valid_source_span(raw: &DecodedRaw, file: u32, start: u32, end: u32) -> bool {
    if file == NO_INDEX {
        start == 0 && end == 0
    } else {
        (file as usize) < raw.files.len() && start <= end && u64::from(end) <= MO_LIMIT_FILE_SIZE
    }
}

fn validate_code_and_debug(raw: &DecodedRaw, index: usize) -> Result<(), LoadError> {
    let f = &raw.functions[index];
    let d = &raw.debug[index];
    let pos = raw.debug_positions[index];
    let code_base = raw.code_positions[index];

    let mut starts = vec![false; f.code.len() + 1];
    let mut instructions: u64 = 0;
    let mut pc = 0usize;
    while pc < f.code.len() {
        starts[pc] = true;
        let head = f.code[pc];
        let op_num = head & OPCODE_MASK;
        let var_count = (head >> OPCODE_VARIABLE_SHIFT) as usize;
        let Some(info) = Opcode::find(op_num) else {
            return Err(LoadError::invalid(
                code_base + (pc as u32) * 4,
                "invalid instruction shape or instruction limit",
            ));
        };
        if var_count > info.variable_limit as usize {
            let offset = code_base + (pc as u32) * 4;
            return if info.variable_limit != 0 {
                Err(LoadError::limit(
                    offset,
                    "invalid instruction shape or instruction limit",
                ))
            } else {
                Err(LoadError::invalid(
                    offset,
                    "invalid instruction shape or instruction limit",
                ))
            };
        }
        let width = 1 + info.fixed_count as usize + var_count;
        if width > f.code.len() - pc {
            return Err(LoadError::invalid(
                code_base + (pc as u32) * 4,
                "invalid instruction shape or instruction limit",
            ));
        }
        instructions += 1;
        if instructions > MO_LIMIT_FUNCTION_INSTRUCTIONS {
            return Err(LoadError::limit(
                code_base + (pc as u32) * 4,
                "invalid instruction shape or instruction limit",
            ));
        }
        pc += width;
    }
    starts[f.code.len()] = true;

    // Second pass: validate all operand indices and jump targets.
    pc = 0;
    while pc < f.code.len() {
        let head = f.code[pc];
        let Some(info) = Opcode::find(head & OPCODE_MASK) else {
            return Err(LoadError::invalid(
                code_base + (pc as u32) * 4,
                "invalid instruction shape or instruction limit",
            ));
        };
        let var_count = (head >> OPCODE_VARIABLE_SHIFT) as usize;
        let total = info.fixed_count as usize + var_count;
        for j in 0..total {
            let kind = if j < info.fixed_count as usize {
                info.fixed[j].kind
            } else {
                info.variable.kind
            };
            if !validate_operand(raw, f, kind, f.code[pc + 1 + j], &starts) {
                return Err(LoadError::invalid(
                    code_base + ((pc + 1 + j) as u32) * 4,
                    "instruction index out of range or target not a boundary",
                ));
            }
        }
        pc += 1 + total;
    }

    // Debug spans
    for (j, s) in d.spans.iter().enumerate() {
        if (s.code_offset as usize) >= f.code.len()
            || !starts[s.code_offset as usize]
            || !valid_source_span(raw, s.file, s.start, s.end)
            || (j != 0 && d.spans[j - 1].code_offset >= s.code_offset)
        {
            return Err(LoadError::invalid(
                pos.spans + 4 + (j as u32) * 16,
                "invalid debug span",
            ));
        }
    }

    // Debug register name ranges
    let mut ranges: Vec<(u32, u32, u32, usize)> = Vec::with_capacity(d.names.len());
    for (j, rn) in d.names.iter().enumerate() {
        if (rn.reg as usize) >= f.register_types.len()
            || !is_named(raw, rn.name)
            || rn.code_start >= rn.code_end
            || (rn.code_end as usize) > f.code.len()
            || !starts[rn.code_start as usize]
            || !starts[rn.code_end as usize]
        {
            return Err(LoadError::invalid(
                pos.names + 4 + (j as u32) * 16,
                "invalid debug register range",
            ));
        }
        ranges.push((rn.reg, rn.code_start, rn.code_end, j));
    }
    ranges.sort_unstable_by_key(|&(reg, start, _, _)| (reg, start));
    for pair in ranges.windows(2) {
        if pair[0].0 == pair[1].0 && pair[0].2 > pair[1].1 {
            return Err(LoadError::invalid(
                pos.names + 4 + (pair[1].3 as u32) * 16,
                "overlapping debug register ranges",
            ));
        }
    }

    let is_spec = (f.kind as u32) >= (FunctionKind::Test as u32);
    if d.text != NO_INDEX && (!is_spec || (d.text as usize) >= raw.strings.len()) {
        return Err(LoadError::invalid(pos.metadata, "invalid debug text index"));
    }
    if !valid_source_span(raw, d.file, d.start, d.end) {
        return Err(LoadError::invalid(
            pos.metadata + 4,
            "invalid declaration span",
        ));
    }
    if (!d.lines.is_empty() && f.kind != FunctionKind::Promise)
        || (d.expectation != AnchorExpectation::NotAnchor && f.kind != FunctionKind::Anchor)
    {
        let offset = if !d.lines.is_empty() && f.kind != FunctionKind::Promise {
            pos.lines
        } else {
            pos.metadata + 20
        };
        return Err(LoadError::invalid(
            offset,
            "condition lines or expectation on wrong function kind",
        ));
    }
    if d.subject != NO_INDEX {
        let expected_subject_kind = match f.kind {
            FunctionKind::Promise => Some(FunctionKind::Function),
            FunctionKind::Anchor => Some(FunctionKind::Promise),
            _ => None,
        };
        let ok = expected_subject_kind.is_some_and(|want| {
            (d.subject as usize) < raw.functions.len()
                && raw.functions[d.subject as usize].kind == want
        }) && !(f.kind == FunctionKind::Promise && d.lines.is_empty())
            && !(f.kind == FunctionKind::Anchor && d.expectation == AnchorExpectation::NotAnchor);
        if !ok {
            return Err(LoadError::invalid(
                pos.metadata + 16,
                "invalid spec subject or missing linked metadata",
            ));
        }
    }
    for (j, line) in d.lines.iter().enumerate() {
        if !valid_source_span(raw, line.file, line.start, line.end)
            || (line.text as usize) >= raw.strings.len()
        {
            return Err(LoadError::invalid(
                pos.lines + 4 + (j as u32) * 16,
                "invalid condition line",
            ));
        }
    }

    Ok(())
}

fn check_service_type(
    raw: &DecodedRaw,
    root: u32,
    seen: &mut [bool],
    queue: &mut Vec<u32>,
) -> Result<(), LoadError> {
    queue.clear();
    if !seen[root as usize] {
        seen[root as usize] = true;
        queue.push(root);
    }
    let mut i = 0;
    while i < queue.len() {
        let t = &raw.types[queue[i] as usize];
        i += 1;
        if matches!(
            t.kind,
            TypeKind::Function | TypeKind::Capability | TypeKind::Env
        ) {
            return Err(refuse("service type contains a capability or function"));
        }
        for &child in &t.parameters {
            if !seen[child as usize] {
                seen[child as usize] = true;
                queue.push(child);
            }
        }
        for f in &t.fields {
            if !seen[f.ty as usize] {
                seen[f.ty as usize] = true;
                queue.push(f.ty);
            }
        }
        for c in &t.cases {
            for f in &c.fields {
                if !seen[f.ty as usize] {
                    seen[f.ty as usize] = true;
                    queue.push(f.ty);
                }
            }
        }
    }
    Ok(())
}

fn is_service_function(raw: &DecodedRaw, module: u32, idx: u32) -> bool {
    (idx as usize) < raw.functions.len()
        && raw.functions[idx as usize].module == module
        && raw.functions[idx as usize].kind == FunctionKind::Function
        && !raw.functions[idx as usize].spec_code
}

fn is_update_pair(raw: &DecodedRaw, ty: u32, state: u32) -> bool {
    let t = &raw.types[ty as usize];
    t.kind == TypeKind::Tuple && t.parameters.len() == 2 && t.parameters[0] == state
}

fn validate_services(raw: &DecodedRaw) -> Result<(), LoadError> {
    if raw.services.is_empty() {
        return Ok(());
    }
    let mut seen_types = vec![false; raw.types.len()];
    let mut tokens_seen = vec![false; raw.strings.len()];
    let mut queue = Vec::new();
    let mut modules_seen = HashSet::with_capacity(raw.services.len());

    for s in &raw.services {
        if (s.module as usize) >= raw.modules.len()
            || (s.state_type as usize) >= raw.types.len()
            || !is_service_function(raw, s.module, s.start)
            || !raw.functions[s.start as usize].parameters.is_empty()
            || raw.functions[s.start as usize].result_type != s.state_type
        {
            return Err(refuse(
                "invalid service module, start function or state type",
            ));
        }
        if !modules_seen.insert(s.module) {
            return Err(refuse("duplicate service module"));
        }
        if !is_named(raw, s.token_env) {
            return Err(refuse("invalid service token environment name"));
        }
        let token = raw.canonical_names[s.token_env as usize] as usize;
        if !tokens_seen[token] {
            for (j, &c) in raw.strings[token].as_bytes().iter().enumerate() {
                let ok = c == b'_' || c.is_ascii_uppercase() || (j != 0 && c.is_ascii_digit());
                if !ok {
                    return Err(refuse("invalid service token environment name"));
                }
            }
            tokens_seen[token] = true;
        }
        if s.tools.is_empty()
            || (s.timers.len() as u64) > MO_LIMIT_SERVICE_HANDLERS - (s.tools.len() as u64)
        {
            return Err(refuse("invalid service handler count"));
        }
        check_service_type(raw, s.state_type, &mut seen_types, &mut queue)?;

        let mut handler_seen = HashSet::with_capacity(s.tools.len() + s.timers.len());
        let mut tool_names = HashSet::with_capacity(s.tools.len());

        for tool in &s.tools {
            if !is_service_function(raw, s.module, tool.function) {
                return Err(refuse("invalid service handler function"));
            }
            if !handler_seen.insert(tool.function) {
                return Err(refuse("duplicate service handler or tool name"));
            }
            let f = &raw.functions[tool.function as usize];
            if f.parameters.len() < 2
                || f.parameters[0].ty != s.state_type
                || raw.types[f.parameters[1].ty as usize].kind != TypeKind::Time
            {
                return Err(refuse("invalid service handler parameters"));
            }
            for p in &f.parameters {
                check_service_type(raw, p.ty, &mut seen_types, &mut queue)?;
            }
            check_service_type(raw, f.result_type, &mut seen_types, &mut queue)?;

            if !is_named(raw, tool.name) {
                return Err(refuse("invalid service tool name"));
            }
            if tool.argument_types.len() != f.parameters.len() - 2
                || tool.argument_names.len() != tool.argument_types.len()
                || (tool.description as usize) >= raw.strings.len()
                || tool.result_type != f.result_type
            {
                return Err(refuse("service tool schema differs from handler"));
            }
            if !tool_names.insert(raw.canonical_names[tool.name as usize]) {
                return Err(refuse("duplicate service handler or tool name"));
            }
            let mut arg_names = HashSet::with_capacity(tool.argument_names.len());
            for (k, (&arg_ty, &arg_name)) in tool
                .argument_types
                .iter()
                .zip(&tool.argument_names)
                .enumerate()
            {
                if arg_ty != f.parameters[k + 2].ty || !is_named(raw, arg_name) {
                    return Err(refuse("service tool schema differs from handler"));
                }
                if !arg_names.insert(raw.canonical_names[arg_name as usize]) {
                    return Err(refuse("duplicate service tool parameter name"));
                }
            }
            let res_ty = &raw.types[tool.result_type as usize];
            let mut success = NO_INDEX;
            if res_ty.kind == TypeKind::Choice
                && res_ty
                    .name
                    .as_deref()
                    .is_some_and(|n| n.starts_with("Result("))
                && res_ty.cases.len() == 2
                && res_ty.cases[0].name == "Ok"
                && res_ty.cases[1].name == "Error"
                && res_ty.cases[0].unnamed
                && res_ty.cases[1].unnamed
            {
                success = res_ty.cases[0].fields[0].ty;
            }
            let update = is_update_pair(raw, tool.result_type, s.state_type)
                || (success != NO_INDEX && is_update_pair(raw, success, s.state_type));
            if tool.result_type == s.state_type
                || success == s.state_type
                || tool.read_only == update
            {
                return Err(refuse("invalid service tool result classification"));
            }
        }

        for timer in &s.timers {
            if !is_service_function(raw, s.module, timer.function) {
                return Err(refuse("invalid service handler function"));
            }
            if !handler_seen.insert(timer.function) {
                return Err(refuse("duplicate service handler or tool name"));
            }
            let f = &raw.functions[timer.function as usize];
            if f.parameters.len() < 2
                || f.parameters[0].ty != s.state_type
                || raw.types[f.parameters[1].ty as usize].kind != TypeKind::Time
            {
                return Err(refuse("invalid service handler parameters"));
            }
            for p in &f.parameters {
                check_service_type(raw, p.ty, &mut seen_types, &mut queue)?;
            }
            check_service_type(raw, f.result_type, &mut seen_types, &mut queue)?;
            if f.parameters.len() != 2
                || f.result_type != s.state_type
                || !is_service_function(raw, s.module, timer.interval)
                || !raw.functions[timer.interval as usize].parameters.is_empty()
                || raw.types[raw.functions[timer.interval as usize].result_type as usize].kind
                    != TypeKind::Duration
            {
                return Err(refuse("invalid service timer handler or interval function"));
            }
        }
    }

    let reachable = service_reach(raw);
    for (s, reach) in raw.services.iter().zip(&reachable) {
        validate_service_shapes(raw, s, reach)?;
    }

    Ok(())
}

fn validate_service_shapes(
    raw: &DecodedRaw,
    s: &ServiceDef,
    reach: &[u32],
) -> Result<(), LoadError> {
    let mut at = 0usize;
    for (i, shape) in s.shapes.iter().enumerate() {
        if (shape.ty as usize) >= raw.types.len() {
            return Err(refuse("service shape type out of range"));
        }
        if i != 0 && s.shapes[i - 1].ty >= shape.ty {
            return Err(refuse("duplicate or unsorted service shape"));
        }
        while at < reach.len() && reach[at] < shape.ty {
            at += 1;
        }
        if at == reach.len() || reach[at] != shape.ty {
            return Err(refuse("unreachable service shape"));
        }
        let t = &raw.types[shape.ty as usize];
        if shape.is_struct {
            if !shape.unnamed.is_empty() {
                return Err(refuse("struct service shape has payload flags"));
            }
            if t.kind != TypeKind::Record
                && (t.kind != TypeKind::Choice || t.cases.len() != 1 || t.cases[0].unnamed)
            {
                return Err(refuse("invalid service struct shape"));
            }
        } else {
            if t.kind != TypeKind::Choice || shape.unnamed.len() != t.cases.len() {
                return Err(refuse("invalid service enum shape"));
            }
            for (j, &flag) in shape.unnamed.iter().enumerate() {
                if flag && t.cases[j].fields.len() != 1 {
                    return Err(refuse("invalid service unnamed payload"));
                }
                if t.cases[j].unnamed && !flag {
                    return Err(refuse("service shape conflicts with unnamed case"));
                }
            }
        }
    }
    if s.shapes.len() != reach.len() {
        return Err(refuse("missing reachable service shape"));
    }
    Ok(())
}

/// Exact port of `mo_bytecode_service_reach` (`src/bytecode/service-reach.c`):
/// Tarjan SCC + 64-wide bitmask propagation across services.
fn service_reach(raw: &DecodedRaw) -> Vec<Vec<u32>> {
    let count = raw.types.len();
    let mut adj: Vec<Vec<u32>> = Vec::with_capacity(count);
    for t in &raw.types {
        let mut edges = Vec::new();
        edges.extend_from_slice(&t.parameters);
        for f in &t.fields {
            edges.push(f.ty);
        }
        for c in &t.cases {
            for f in &c.fields {
                edges.push(f.ty);
            }
        }
        adj.push(edges);
    }

    // Iterative Tarjan SCC
    let mut index = vec![0u32; count];
    let mut low = vec![0u32; count];
    let mut comp = vec![NO_INDEX; count];
    let mut cursor = vec![0usize; count];
    let mut serial = 0u32;
    let mut groups: Vec<Vec<usize>> = Vec::new();
    let mut active: Vec<usize> = Vec::new();
    let mut stack: Vec<usize> = Vec::new();

    for root in 0..count {
        if index[root] != 0 {
            continue;
        }
        serial += 1;
        index[root] = serial;
        low[root] = serial;
        active.push(root);
        stack.push(root);
        while let Some(&u) = stack.last() {
            if cursor[u] < adj[u].len() {
                let v = adj[u][cursor[u]] as usize;
                cursor[u] += 1;
                if index[v] == 0 {
                    serial += 1;
                    index[v] = serial;
                    low[v] = serial;
                    active.push(v);
                    stack.push(v);
                } else if comp[v] == NO_INDEX && index[v] < low[u] {
                    low[u] = index[v];
                }
            } else {
                stack.pop();
                if index[u] == low[u] {
                    let gid = groups.len() as u32;
                    let mut members = Vec::new();
                    while let Some(m) = active.pop() {
                        comp[m] = gid;
                        members.push(m);
                        if m == u {
                            break;
                        }
                    }
                    groups.push(members);
                }
                if let Some(&parent) = stack.last()
                    && low[u] < low[parent]
                {
                    low[parent] = low[u];
                }
            }
        }
    }

    let mut result = vec![Vec::new(); raw.services.len()];
    let mut masks = vec![0u64; groups.len()];
    let mut first = 0usize;
    while first < raw.services.len() {
        let width = (raw.services.len() - first).min(64);
        masks.fill(0);
        for i in 0..width {
            let s = &raw.services[first + i];
            for tool in &s.tools {
                for &arg in &tool.argument_types {
                    masks[comp[arg as usize] as usize] |= 1u64 << i;
                }
                masks[comp[tool.result_type as usize] as usize] |= 1u64 << i;
            }
        }
        for c in (0..groups.len()).rev() {
            let m = masks[c];
            if m == 0 {
                continue;
            }
            for &u in &groups[c] {
                for &v in &adj[u] {
                    masks[comp[v as usize] as usize] |= m;
                }
            }
        }
        for (ty_idx, t) in raw.types.iter().enumerate() {
            if !is_nominal(t) {
                continue;
            }
            let mut m = masks[comp[ty_idx] as usize];
            while m != 0 {
                let bit = m.trailing_zeros() as usize;
                result[first + bit].push(ty_idx as u32);
                m &= m - 1;
            }
        }
        first += width;
    }

    result
}
