//! Iterative structural equality and ordering (`src/heap/generic.c` parity).

use std::cmp::Ordering;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{ABILITY_EQUAL, ABILITY_ORDER, BigIntSign, CaseShape, TypeKind};

use crate::hash::inline_payload_to_value;
use crate::{HeapObj, Value};

fn is_compound(kind: TypeKind) -> bool {
    matches!(
        kind,
        TypeKind::Tuple
            | TypeKind::Record
            | TypeKind::Choice
            | TypeKind::List
            | TypeKind::Map
            | TypeKind::Set
    )
}

fn scalar_compare(kind: TypeKind, a: &Value, b: &Value, order: bool) -> Option<Ordering> {
    match kind {
        TypeKind::Unit => Some(Ordering::Equal),
        TypeKind::Bool => match (a, b) {
            (Value::Bool(x), Value::Bool(y)) => Some(x.cmp(y)),
            _ => None,
        },
        TypeKind::Byte => match (a, b) {
            (Value::Byte(x), Value::Byte(y)) => Some(x.cmp(y)),
            _ => None,
        },
        TypeKind::Int | TypeKind::Time | TypeKind::Duration => {
            let x = match a {
                Value::Int(v) | Value::Time(v) | Value::Duration(v) => *v,
                _ => return None,
            };
            let y = match b {
                Value::Int(v) | Value::Time(v) | Value::Duration(v) => *v,
                _ => return None,
            };
            Some(x.cmp(&y))
        }
        TypeKind::Char => match (a, b) {
            (Value::Char(x), Value::Char(y)) => Some(x.cmp(y)),
            _ => None,
        },
        TypeKind::Decimal => match (a, b) {
            (Value::Decimal(x), Value::Decimal(y)) => Some(x.cmp(y)),
            _ => None,
        },
        TypeKind::Float => match (a, b) {
            (Value::Float(x), Value::Float(y)) => {
                if x.is_nan() || y.is_nan() {
                    if order {
                        Some(match (x.is_nan(), y.is_nan()) {
                            (true, true) => Ordering::Equal,
                            (true, false) => Ordering::Greater,
                            (false, true) => Ordering::Less,
                            (false, false) => unreachable!(),
                        })
                    } else {
                        // NaN != NaN for structural equality!
                        Some(Ordering::Greater)
                    }
                } else if x < y {
                    Some(Ordering::Less)
                } else if x > y {
                    Some(Ordering::Greater)
                } else {
                    Some(Ordering::Equal)
                }
            }
            _ => None,
        },
        TypeKind::Range => {
            if order {
                return None;
            }
            match (a, b) {
                (Value::Range(s1, e1), Value::Range(s2, e2)) => Some(if s1 == s2 && e1 == e2 {
                    Ordering::Equal
                } else {
                    Ordering::Greater
                }),
                _ => None,
            }
        }
        TypeKind::Text => match (a, b) {
            (Value::Ref(rx), Value::Ref(ry)) => match (rx.obj(), ry.obj()) {
                (HeapObj::Text(sx), HeapObj::Text(sy)) => Some(sx.as_bytes().cmp(sy.as_bytes())),
                _ => None,
            },
            _ => None,
        },
        TypeKind::Bytes => match (a, b) {
            (Value::Ref(rx), Value::Ref(ry)) => match (rx.obj(), ry.obj()) {
                (HeapObj::Bytes(bx), HeapObj::Bytes(by)) => Some(bx.cmp(by)),
                _ => None,
            },
            _ => None,
        },
        TypeKind::BigInt => match (a, b) {
            (Value::Ref(rx), Value::Ref(ry)) => match (rx.obj(), ry.obj()) {
                (
                    HeapObj::BigInt {
                        sign: sx,
                        magnitude: mx,
                    },
                    HeapObj::BigInt {
                        sign: sy,
                        magnitude: my,
                    },
                ) => {
                    let sign_ord = |s: BigIntSign| -> i8 {
                        match s {
                            BigIntSign::Negative => -1,
                            BigIntSign::Zero => 0,
                            BigIntSign::Positive => 1,
                        }
                    };
                    let xs = sign_ord(*sx);
                    let ys = sign_ord(*sy);
                    if xs != ys {
                        return Some(xs.cmp(&ys));
                    }
                    if xs == 0 {
                        return Some(Ordering::Equal);
                    }
                    let mag_ord = mx
                        .len()
                        .cmp(&my.len())
                        .then_with(|| mx.iter().rev().cmp(my.iter().rev()));
                    Some(if xs < 0 { mag_ord.reverse() } else { mag_ord })
                }
                _ => None,
            },
            _ => None,
        },
        _ => None,
    }
}

fn choice_tag(v: &Value) -> u32 {
    match v {
        Value::ChoiceInline { tag, .. } | Value::ChoiceRef { tag, .. } => *tag,
        _ => unreachable!("expected choice value"),
    }
}

fn extract_seq_children(types: &TypeTable, ty: u32, v: &Value) -> Vec<(u32, Value)> {
    let t = types.get(ty);
    match t.kind {
        TypeKind::Choice => match v {
            Value::ChoiceInline { tag, payload } => {
                let c = &t.cases[*tag as usize];
                if c.fields.len() == 1 {
                    let fty = c.fields[0].ty;
                    vec![(fty, inline_payload_to_value(*payload, types.get(fty).kind))]
                } else {
                    Vec::new()
                }
            }
            Value::ChoiceRef { tag, payload } => {
                let c = &t.cases[*tag as usize];
                match types.case_shape(ty, *tag) {
                    CaseShape::Ref => vec![(c.fields[0].ty, Value::Ref(payload.clone()))],
                    CaseShape::Boxed => {
                        if let HeapObj::CaseBlock { fields, .. } = payload.obj() {
                            c.fields
                                .iter()
                                .zip(fields)
                                .map(|(f, val)| (f.ty, val.clone()))
                                .collect()
                        } else {
                            Vec::new()
                        }
                    }
                    CaseShape::Empty | CaseShape::Inline => Vec::new(),
                }
            }
            _ => Vec::new(),
        },
        TypeKind::Tuple => {
            if let Value::Ref(r) = v
                && let HeapObj::Tuple { fields, .. } = r.obj()
            {
                t.parameters
                    .iter()
                    .zip(fields)
                    .map(|(&fty, val)| (fty, val.clone()))
                    .collect()
            } else {
                Vec::new()
            }
        }
        TypeKind::Record => {
            if let Value::Ref(r) = v
                && let HeapObj::Record { fields, .. } = r.obj()
            {
                t.fields
                    .iter()
                    .zip(fields)
                    .map(|(f, val)| (f.ty, val.clone()))
                    .collect()
            } else {
                Vec::new()
            }
        }
        TypeKind::List => {
            if let Value::Ref(r) = v
                && let HeapObj::List(items) = r.obj()
            {
                let elem_ty = t.parameters[0];
                items.iter().map(|val| (elem_ty, val.clone())).collect()
            } else {
                Vec::new()
            }
        }
        _ => Vec::new(),
    }
}

pub fn value_equal(
    types: &TypeTable,
    hash_key: [u64; 2],
    ty: u32,
    left: &Value,
    right: &Value,
) -> bool {
    compare_values(types, hash_key, ty, left, right, false) == Some(Ordering::Equal)
}

pub fn value_compare(
    types: &TypeTable,
    hash_key: [u64; 2],
    ty: u32,
    left: &Value,
    right: &Value,
) -> Option<Ordering> {
    compare_values(types, hash_key, ty, left, right, true)
}

fn compare_values(
    types: &TypeTable,
    hash_key: [u64; 2],
    ty: u32,
    left: &Value,
    right: &Value,
    order: bool,
) -> Option<Ordering> {
    let required = if order { ABILITY_ORDER } else { ABILITY_EQUAL };
    if (types.abilities(ty) & required) == 0 {
        return None;
    }

    // Explicit work stack avoids recursion overflow on deep values.
    let mut stack: Vec<(u32, Value, Value)> = vec![(ty, left.clone(), right.clone())];

    while let Some((cur_ty, a, b)) = stack.pop() {
        let kind = types.get(cur_ty).kind;
        if !is_compound(kind) {
            let ord = scalar_compare(kind, &a, &b, order)?;
            if ord != Ordering::Equal {
                return Some(ord);
            }
            continue;
        }
        match kind {
            TypeKind::Choice => {
                let ta = choice_tag(&a);
                let tb = choice_tag(&b);
                if ta != tb {
                    return Some(ta.cmp(&tb));
                }
                let ca = extract_seq_children(types, cur_ty, &a);
                let cb = extract_seq_children(types, cur_ty, &b);
                for ((fty, va), (_, vb)) in ca.into_iter().zip(cb).rev() {
                    stack.push((fty, va, vb));
                }
            }
            TypeKind::Tuple | TypeKind::Record | TypeKind::List => {
                let ca = extract_seq_children(types, cur_ty, &a);
                let cb = extract_seq_children(types, cur_ty, &b);
                if !order && ca.len() != cb.len() {
                    return Some(Ordering::Greater);
                }
                let len_cmp = ca.len().cmp(&cb.len());
                let min_len = ca.len().min(cb.len());
                // Push length comparison as sentinel only if prefixes tie (for order mode).
                if order && len_cmp != Ordering::Equal {
                    // Compare common prefix first; if all equal, return len_cmp.
                    // We compare element-by-element via recursive/iterative sub-call on prefix.
                    for i in 0..min_len {
                        let ord =
                            compare_values(types, hash_key, ca[i].0, &ca[i].1, &cb[i].1, true)?;
                        if ord != Ordering::Equal {
                            return Some(ord);
                        }
                    }
                    return Some(len_cmp);
                }
                for i in (0..min_len).rev() {
                    stack.push((ca[i].0, ca[i].1.clone(), cb[i].1.clone()));
                }
            }
            TypeKind::Map => {
                if order {
                    return None;
                }
                let (Value::Ref(ra), Value::Ref(rb)) = (&a, &b) else {
                    return None;
                };
                let (HeapObj::Map(ma), HeapObj::Map(mb)) = (ra.obj(), rb.obj()) else {
                    return None;
                };
                if ma.len() != mb.len() {
                    return Some(Ordering::Greater);
                }
                let val_ty = types.get(cur_ty).parameters[1];
                for (k, va) in ma.iter() {
                    let Some(vb) = mb.get(types, hash_key, k) else {
                        return Some(Ordering::Greater);
                    };
                    stack.push((val_ty, va.clone(), vb.clone()));
                }
            }
            TypeKind::Set => {
                if order {
                    return None;
                }
                let (Value::Ref(ra), Value::Ref(rb)) = (&a, &b) else {
                    return None;
                };
                let (HeapObj::Set(sa), HeapObj::Set(sb)) = (ra.obj(), rb.obj()) else {
                    return None;
                };
                if sa.len() != sb.len() {
                    return Some(Ordering::Greater);
                }
                for elem in sa.iter() {
                    if !sb.contains(types, hash_key, elem) {
                        return Some(Ordering::Greater);
                    }
                }
            }
            _ => return None,
        }
    }

    Some(Ordering::Equal)
}
