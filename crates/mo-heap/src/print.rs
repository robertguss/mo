//! Iterative structural printing (`src/heap/print.c` parity).

use mo_bytecode::types::TypeTable;
use mo_bytecode::{BigIntSign, CaseShape, TypeKind};

use crate::float::format_float;
use crate::hash::inline_payload_to_value;
use crate::{HeapObj, Value};

pub fn format_scaled(out: &mut String, value: i128, places: usize) {
    let magnitude: u128 = if value < 0 {
        value.unsigned_abs()
    } else {
        value as u128
    };
    let mut digits = [0u8; 40];
    let mut start = digits.len();
    let mut m = magnitude;
    loop {
        start -= 1;
        digits[start] = b'0' + (m % 10) as u8;
        m /= 10;
        if m == 0 {
            break;
        }
    }
    let mut length = digits.len() - start;
    let point = (length as i32) - (places as i32);
    while length > 1 && digits[start + length - 1] == b'0' && (length as i32) > point {
        length -= 1;
    }
    if value < 0 {
        out.push('-');
    }
    if point <= 0 {
        out.push_str("0.");
        for _ in 0..(-point) {
            out.push('0');
        }
    }
    for i in 0..length {
        if point > 0 && i == point as usize {
            out.push('.');
        }
        out.push(digits[start + i] as char);
    }
}

pub fn format_decimal(out: &mut String, value: i128) {
    if value == 0 {
        out.push('0');
    } else {
        format_scaled(out, value, 18);
    }
}

pub fn format_duration(out: &mut String, micros: i64) {
    if micros == 0 {
        out.push('0');
    } else {
        format_scaled(out, i128::from(micros), 6);
    }
    out.push('s');
}

fn push_fixed_digits(out: &mut String, mut value: i64, width: usize) {
    let mut buf = [0u8; 4];
    for i in (0..width).rev() {
        buf[i] = b'0' + (value % 10) as u8;
        value /= 10;
    }
    for &b in &buf[..width] {
        out.push(b as char);
    }
}

pub fn format_time(out: &mut String, micros: i64) {
    const DAY_MICROS: i64 = 86_400_000_000;
    let mut days = micros / DAY_MICROS;
    let mut in_day = micros % DAY_MICROS;
    if in_day < 0 {
        days -= 1;
        in_day += DAY_MICROS;
    }
    let z = days + 719_468;
    let era = if z >= 0 {
        z / 146_097
    } else {
        (z - 146_096) / 146_097
    };
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let mut year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    if month <= 2 {
        year += 1;
    }
    let seconds = in_day / 1_000_000;
    push_fixed_digits(out, year, 4);
    out.push('-');
    push_fixed_digits(out, month, 2);
    out.push('-');
    push_fixed_digits(out, day, 2);
    out.push('T');
    push_fixed_digits(out, seconds / 3600, 2);
    out.push(':');
    push_fixed_digits(out, (seconds / 60) % 60, 2);
    out.push(':');
    push_fixed_digits(out, seconds % 60, 2);

    let frac = (in_day % 1_000_000) as u64;
    if frac != 0 {
        let mut digits = [0u8; 6];
        let mut f = frac;
        for i in (0..6).rev() {
            digits[i] = b'0' + (f % 10) as u8;
            f /= 10;
        }
        let mut len = 6;
        while len != 0 && digits[len - 1] == b'0' {
            len -= 1;
        }
        out.push('.');
        for &b in &digits[..len] {
            out.push(b as char);
        }
    }
    out.push('Z');
}

pub fn format_bigint(out: &mut String, sign: BigIntSign, magnitude: &[u8]) {
    crate::bigint::format_bigint(out, sign, magnitude);
}

fn push_str_bounded(out: &mut String, s: &str, limit: Option<usize>) -> bool {
    if let Some(max) = limit
        && out.len().saturating_add(s.len()) > max
    {
        return false;
    }
    out.push_str(s);
    true
}

fn push_char_bounded(out: &mut String, ch: char, limit: Option<usize>) -> bool {
    if let Some(max) = limit
        && out.len().saturating_add(ch.len_utf8()) > max
    {
        return false;
    }
    out.push(ch);
    true
}

/// Quotes UTF-8 bytes matching `src/heap/print.c:quote` (including C1 escapes).
pub fn quote_structural(out: &mut String, s: &str, delimiter: Option<char>) {
    let _ = quote_structural_bounded(out, s, delimiter, None);
}

fn quote_structural_bounded(
    out: &mut String,
    s: &str,
    delimiter: Option<char>,
    limit: Option<usize>,
) -> bool {
    if let Some(d) = delimiter
        && !push_char_bounded(out, d, limit)
    {
        return false;
    }
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        let is_c1 = c == 0xc2 && i + 1 < bytes.len() && (0x80..=0x9f).contains(&bytes[i + 1]);
        match c {
            b'\n' => {
                if !push_str_bounded(out, "\\n", limit) {
                    return false;
                }
                i += 1;
            }
            b'\r' => {
                if !push_str_bounded(out, "\\r", limit) {
                    return false;
                }
                i += 1;
            }
            b'\t' => {
                if !push_str_bounded(out, "\\t", limit) {
                    return false;
                }
                i += 1;
            }
            b if b < 32 || b == 127 => {
                let esc = format!("\\u{{{b:x}}}");
                if !push_str_bounded(out, &esc, limit) {
                    return false;
                }
                i += 1;
            }
            _ if is_c1 => {
                let code = bytes[i + 1];
                let esc = format!("\\u{{{code:x}}}");
                if !push_str_bounded(out, &esc, limit) {
                    return false;
                }
                i += 2;
            }
            _ => {
                let Some(ch) = s[i..].chars().next() else {
                    break;
                };
                let is_interp = c == b'#' && i + 1 < bytes.len() && bytes[i + 1] == b'{';
                if (delimiter == Some(ch) || ch == '\\' || is_interp)
                    && !push_char_bounded(out, '\\', limit)
                {
                    return false;
                }
                if !push_char_bounded(out, ch, limit) {
                    return false;
                }
                i += ch.len_utf8();
            }
        }
    }
    if let Some(d) = delimiter
        && !push_char_bounded(out, d, limit)
    {
        return false;
    }
    true
}

fn scalar_text_bounded(
    kind: TypeKind,
    value: &Value,
    nested: bool,
    limit: Option<usize>,
    out: &mut String,
) -> bool {
    match kind {
        TypeKind::Unit => push_str_bounded(out, "Unit", limit),
        TypeKind::Bool => {
            if let Value::Bool(b) = value {
                push_str_bounded(out, if *b { "true" } else { "false" }, limit)
            } else {
                true
            }
        }
        TypeKind::Byte => {
            if let Value::Byte(b) = value {
                push_str_bounded(out, &b.to_string(), limit)
            } else {
                true
            }
        }
        TypeKind::Int => {
            if let Value::Int(n) = value {
                push_str_bounded(out, &n.to_string(), limit)
            } else {
                true
            }
        }
        TypeKind::Float => {
            if let Value::Float(x) = value {
                let mut tmp = String::new();
                format_float(*x, &mut tmp);
                push_str_bounded(out, &tmp, limit)
            } else {
                true
            }
        }
        TypeKind::Char => {
            if let Value::Char(c) = value {
                let mut buf = [0u8; 4];
                let s = c.encode_utf8(&mut buf);
                if nested {
                    quote_structural_bounded(out, s, Some('\''), limit)
                } else {
                    push_str_bounded(out, s, limit)
                }
            } else {
                true
            }
        }
        TypeKind::Time => {
            if let Value::Time(t) = value {
                let mut tmp = String::new();
                format_time(&mut tmp, *t);
                push_str_bounded(out, &tmp, limit)
            } else {
                true
            }
        }
        TypeKind::Duration => {
            if let Value::Duration(d) = value {
                let mut tmp = String::new();
                format_duration(&mut tmp, *d);
                push_str_bounded(out, &tmp, limit)
            } else {
                true
            }
        }
        TypeKind::Decimal => {
            if let Value::Decimal(d) = value {
                let mut tmp = String::new();
                format_decimal(&mut tmp, *d);
                push_str_bounded(out, &tmp, limit)
            } else {
                true
            }
        }
        TypeKind::Range => {
            if let Value::Range(s, e) = value {
                push_str_bounded(out, &s.to_string(), limit)
                    && push_str_bounded(out, "..", limit)
                    && push_str_bounded(out, &e.to_string(), limit)
            } else {
                true
            }
        }
        TypeKind::Text => {
            if let Value::Ref(r) = value
                && let HeapObj::Text(s) = r.obj()
            {
                if nested {
                    quote_structural_bounded(out, s, Some('"'), limit)
                } else {
                    push_str_bounded(out, s, limit)
                }
            } else {
                true
            }
        }
        TypeKind::Bytes => {
            if let Value::Ref(r) = value
                && let HeapObj::Bytes(bytes) = r.obj()
            {
                if !push_str_bounded(out, "bytes[", limit) {
                    return false;
                }
                for (i, &b) in bytes.iter().enumerate() {
                    if i != 0 && !push_str_bounded(out, ", ", limit) {
                        return false;
                    }
                    if !push_str_bounded(out, &b.to_string(), limit) {
                        return false;
                    }
                }
                push_char_bounded(out, ']', limit)
            } else {
                true
            }
        }
        TypeKind::BigInt => {
            if let Value::Ref(r) = value
                && let HeapObj::BigInt { sign, magnitude } = r.obj()
            {
                let mut tmp = String::new();
                format_bigint(&mut tmp, *sign, magnitude);
                push_str_bounded(out, &tmp, limit)
            } else {
                true
            }
        }
        TypeKind::Function => push_str_bounded(out, "<function>", limit),
        TypeKind::Env => push_str_bounded(out, "<environment>", limit),
        TypeKind::Capability => {
            if let Value::Ref(r) = value
                && let HeapObj::Capability(cap) = r.obj()
            {
                push_char_bounded(out, '<', limit)
                    && push_str_bounded(out, cap.kind.name(), limit)
                    && push_char_bounded(out, '>', limit)
            } else {
                true
            }
        }
        _ => unreachable!("compound type handled separately"),
    }
}

struct PrintFrame {
    ty: u32,
    value: Value,
    children: Vec<(Option<String>, u32, Value)>,
    next: usize,
    entered: bool,
    emitted: bool,
}

fn print_impl(
    types: &TypeTable,
    ty: u32,
    value: &Value,
    literal: bool,
    limit: Option<usize>,
    out: &mut String,
) -> bool {
    let mut stack = vec![PrintFrame {
        ty,
        value: value.clone(),
        children: Vec::new(),
        next: 0,
        entered: false,
        emitted: false,
    }];

    while !stack.is_empty() {
        let stack_len = stack.len();
        let Some(f) = stack.last_mut() else {
            break;
        };
        let t = types.get(f.ty);
        let kind = t.kind;
        let is_compound = matches!(
            kind,
            TypeKind::Tuple
                | TypeKind::Record
                | TypeKind::Choice
                | TypeKind::List
                | TypeKind::Map
                | TypeKind::Set
        );
        if !f.entered {
            f.entered = true;
            if !is_compound {
                let nested = literal || stack_len != 1;
                if !scalar_text_bounded(kind, &f.value, nested, limit, out) {
                    return false;
                }
                stack.pop();
                continue;
            }
            let header_name = match kind {
                TypeKind::Record => t.name.as_deref(),
                TypeKind::Choice => {
                    let tag = match &f.value {
                        Value::ChoiceInline { tag, .. } | Value::ChoiceRef { tag, .. } => *tag,
                        _ => unreachable!(),
                    };
                    Some(t.cases[tag as usize].name.as_str())
                }
                _ => None,
            };
            collect_print_children(types, f.ty, &f.value, &mut f.children);
            if let Some(name) = header_name {
                if !quote_structural_bounded(out, name, None, limit) {
                    return false;
                }
                if f.children.is_empty() {
                    stack.pop();
                    continue;
                }
            }
            let open = match kind {
                TypeKind::Map => '{',
                TypeKind::List | TypeKind::Set => '[',
                _ => '(',
            };
            if !push_char_bounded(out, open, limit) {
                return false;
            }
        }

        if f.next == f.children.len() {
            let close = match kind {
                TypeKind::Map => "}",
                TypeKind::Set => "].to_set",
                TypeKind::List => "]",
                _ => ")",
            };
            if !push_str_bounded(out, close, limit) {
                return false;
            }
            stack.pop();
            continue;
        }

        if f.emitted {
            let sep = if kind == TypeKind::Map && f.next % 2 == 1 {
                ": "
            } else {
                ", "
            };
            if !push_str_bounded(out, sep, limit) {
                return false;
            }
        }
        let (label, child_ty, child_val) = f.children[f.next].clone();
        f.next += 1;
        f.emitted = true;

        if let Some(lbl) = label
            && (!quote_structural_bounded(out, &lbl, None, limit)
                || !push_str_bounded(out, ": ", limit))
        {
            return false;
        }

        stack.push(PrintFrame {
            ty: child_ty,
            value: child_val,
            children: Vec::new(),
            next: 0,
            entered: false,
            emitted: false,
        });
    }
    true
}

fn collect_print_children(
    types: &TypeTable,
    ty: u32,
    value: &Value,
    out: &mut Vec<(Option<String>, u32, Value)>,
) {
    let t = types.get(ty);
    match t.kind {
        TypeKind::Choice => match value {
            Value::ChoiceInline { tag, payload } => {
                let c = &t.cases[*tag as usize];
                if c.fields.len() == 1 {
                    let f = &c.fields[0];
                    out.push((
                        f.name.clone(),
                        f.ty,
                        inline_payload_to_value(*payload, types.get(f.ty).kind),
                    ));
                }
            }
            Value::ChoiceRef { tag, payload } => {
                let c = &t.cases[*tag as usize];
                match types.case_shape(ty, *tag) {
                    CaseShape::Ref => {
                        let f = &c.fields[0];
                        out.push((f.name.clone(), f.ty, Value::Ref(payload.clone())));
                    }
                    CaseShape::Boxed => {
                        if let HeapObj::CaseBlock { fields, .. } = payload.obj() {
                            for (fdef, fval) in c.fields.iter().zip(fields) {
                                out.push((fdef.name.clone(), fdef.ty, fval.clone()));
                            }
                        }
                    }
                    CaseShape::Empty | CaseShape::Inline => {}
                }
            }
            _ => {}
        },
        TypeKind::Tuple => {
            if let Value::Ref(r) = value
                && let HeapObj::Tuple { fields, .. } = r.obj()
            {
                for (&fty, fval) in t.parameters.iter().zip(fields) {
                    out.push((None, fty, fval.clone()));
                }
            }
        }
        TypeKind::Record => {
            if let Value::Ref(r) = value
                && let HeapObj::Record { fields, .. } = r.obj()
            {
                for (fdef, fval) in t.fields.iter().zip(fields) {
                    out.push((fdef.name.clone(), fdef.ty, fval.clone()));
                }
            }
        }
        TypeKind::List => {
            if let Value::Ref(r) = value
                && let HeapObj::List(items) = r.obj()
            {
                let elem_ty = t.parameters[0];
                for item in items {
                    out.push((None, elem_ty, item.clone()));
                }
            }
        }
        TypeKind::Set => {
            if let Value::Ref(r) = value
                && let HeapObj::Set(set) = r.obj()
            {
                let elem_ty = t.parameters[0];
                for item in set.iter() {
                    out.push((None, elem_ty, item.clone()));
                }
            }
        }
        TypeKind::Map => {
            if let Value::Ref(r) = value
                && let HeapObj::Map(map) = r.obj()
            {
                let kty = t.parameters[0];
                let vty = t.parameters[1];
                for (k, v) in map.iter() {
                    out.push((None, kty, k.clone()));
                    out.push((None, vty, v.clone()));
                }
            }
        }
        _ => {}
    }
}

pub fn value_print(types: &TypeTable, ty: u32, value: &Value) -> String {
    let mut out = String::new();
    let _ = print_impl(types, ty, value, false, None, &mut out);
    out
}

pub fn value_print_literal(types: &TypeTable, ty: u32, value: &Value) -> String {
    let mut out = String::new();
    let _ = print_impl(types, ty, value, true, None, &mut out);
    out
}

/// Renders `value` up to `limit` bytes, returning `None` if formatting exceeds `limit`.
#[must_use]
pub fn value_print_bounded(
    types: &TypeTable,
    ty: u32,
    value: &Value,
    literal: bool,
    limit: usize,
) -> Option<String> {
    let mut out = String::new();
    if print_impl(types, ty, value, literal, Some(limit), &mut out) {
        Some(out)
    } else {
        None
    }
}
