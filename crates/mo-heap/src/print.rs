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
    if sign == BigIntSign::Zero || magnitude.is_empty() {
        out.push('0');
        return;
    }
    if sign == BigIntSign::Negative {
        out.push('-');
    }
    // Convert little-endian base-256 bytes into base 10^9 chunks.
    let mut limbs: Vec<u32> = Vec::with_capacity(magnitude.len().div_ceil(4));
    for chunk in magnitude.chunks(4) {
        let mut buf = [0u8; 4];
        buf[..chunk.len()].copy_from_slice(chunk);
        limbs.push(u32::from_le_bytes(buf));
    }
    let mut groups: Vec<u32> = Vec::new();
    while !limbs.is_empty() {
        let mut rem: u64 = 0;
        for limb in limbs.iter_mut().rev() {
            let cur = (rem << 32) | u64::from(*limb);
            *limb = (cur / 1_000_000_000) as u32;
            rem = cur % 1_000_000_000;
        }
        groups.push(rem as u32);
        while limbs.last() == Some(&0) {
            limbs.pop();
        }
    }
    if let Some((&most, rest)) = groups.split_last() {
        out.push_str(&most.to_string());
        for &g in rest.iter().rev() {
            let s = format!("{g:09}");
            out.push_str(&s);
        }
    }
}

/// Quotes UTF-8 bytes matching `src/heap/print.c:quote` (including C1 escapes).
pub fn quote_structural(out: &mut String, s: &str, delimiter: Option<char>) {
    if let Some(d) = delimiter {
        out.push(d);
    }
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        let is_c1 = c == 0xc2 && i + 1 < bytes.len() && (0x80..=0x9f).contains(&bytes[i + 1]);
        match c {
            b'\n' => {
                out.push_str("\\n");
                i += 1;
            }
            b'\r' => {
                out.push_str("\\r");
                i += 1;
            }
            b'\t' => {
                out.push_str("\\t");
                i += 1;
            }
            b if b < 32 || b == 127 => {
                out.push_str(&format!("\\u{{{b:x}}}"));
                i += 1;
            }
            _ if is_c1 => {
                let code = bytes[i + 1];
                out.push_str(&format!("\\u{{{code:x}}}"));
                i += 2;
            }
            _ => {
                let ch = s[i..].chars().next().unwrap();
                let is_interp = c == b'#' && i + 1 < bytes.len() && bytes[i + 1] == b'{';
                if delimiter == Some(ch) || ch == '\\' || is_interp {
                    out.push('\\');
                }
                out.push(ch);
                i += ch.len_utf8();
            }
        }
    }
    if let Some(d) = delimiter {
        out.push(d);
    }
}

fn scalar_text(kind: TypeKind, value: &Value, nested: bool, out: &mut String) {
    match kind {
        TypeKind::Unit => out.push_str("Unit"),
        TypeKind::Bool => {
            if let Value::Bool(b) = value {
                out.push_str(if *b { "true" } else { "false" });
            }
        }
        TypeKind::Byte => {
            if let Value::Byte(b) = value {
                out.push_str(&b.to_string());
            }
        }
        TypeKind::Int => {
            if let Value::Int(n) = value {
                out.push_str(&n.to_string());
            }
        }
        TypeKind::Float => {
            if let Value::Float(x) = value {
                format_float(*x, out);
            }
        }
        TypeKind::Char => {
            if let Value::Char(c) = value {
                let mut buf = [0u8; 4];
                let s = c.encode_utf8(&mut buf);
                if nested {
                    quote_structural(out, s, Some('\''));
                } else {
                    out.push_str(s);
                }
            }
        }
        TypeKind::Time => {
            if let Value::Time(t) = value {
                format_time(out, *t);
            }
        }
        TypeKind::Duration => {
            if let Value::Duration(d) = value {
                format_duration(out, *d);
            }
        }
        TypeKind::Decimal => {
            if let Value::Decimal(d) = value {
                format_decimal(out, *d);
            }
        }
        TypeKind::Range => {
            if let Value::Range(s, e) = value {
                out.push_str(&s.to_string());
                out.push_str("..");
                out.push_str(&e.to_string());
            }
        }
        TypeKind::Text => {
            if let Value::Ref(r) = value
                && let HeapObj::Text(s) = r.obj()
            {
                if nested {
                    quote_structural(out, s, Some('"'));
                } else {
                    out.push_str(s);
                }
            }
        }
        TypeKind::Bytes => {
            if let Value::Ref(r) = value
                && let HeapObj::Bytes(bytes) = r.obj()
            {
                out.push_str("bytes[");
                for (i, &b) in bytes.iter().enumerate() {
                    if i != 0 {
                        out.push_str(", ");
                    }
                    out.push_str(&b.to_string());
                }
                out.push(']');
            }
        }
        TypeKind::BigInt => {
            if let Value::Ref(r) = value
                && let HeapObj::BigInt { sign, magnitude } = r.obj()
            {
                format_bigint(out, *sign, magnitude);
            }
        }
        TypeKind::Function => out.push_str("<function>"),
        TypeKind::Env => out.push_str("<environment>"),
        TypeKind::Capability => {
            if let Value::Ref(r) = value
                && let HeapObj::Capability(cap) = r.obj()
            {
                out.push('<');
                out.push_str(cap.kind.name());
                out.push('>');
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

fn print_impl(types: &TypeTable, ty: u32, value: &Value, literal: bool, out: &mut String) {
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
        let f = stack.last_mut().unwrap();
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
                scalar_text(kind, &f.value, nested, out);
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
                quote_structural(out, name, None);
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
            out.push(open);
        }

        if f.next == f.children.len() {
            let close = match kind {
                TypeKind::Map => "}",
                TypeKind::Set => "].to_set",
                TypeKind::List => "]",
                _ => ")",
            };
            out.push_str(close);
            stack.pop();
            continue;
        }

        if f.emitted {
            let sep = if kind == TypeKind::Map && f.next % 2 == 1 {
                ": "
            } else {
                ", "
            };
            out.push_str(sep);
        }
        let (label, child_ty, child_val) = f.children[f.next].clone();
        f.next += 1;
        f.emitted = true;

        if let Some(lbl) = label {
            quote_structural(out, &lbl, None);
            out.push_str(": ");
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
    print_impl(types, ty, value, false, &mut out);
    out
}

pub fn value_print_literal(types: &TypeTable, ty: u32, value: &Value) -> String {
    let mut out = String::new();
    print_impl(types, ty, value, true, &mut out);
    out
}
