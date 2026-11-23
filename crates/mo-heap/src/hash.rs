//! SipHash-1-3 and iterative structural value hashing (`src/heap/generic.c` parity).

use mo_bytecode::types::TypeTable;
use mo_bytecode::{ABILITY_HASH, CaseShape, TypeKind};

use crate::{HeapObj, HeapRef, InlinePayload, Value};

#[inline]
fn sip_round(v0: &mut u64, v1: &mut u64, v2: &mut u64, v3: &mut u64) {
    *v0 = v0.wrapping_add(*v1);
    *v1 = v1.rotate_left(13);
    *v1 ^= *v0;
    *v0 = v0.rotate_left(32);
    *v2 = v2.wrapping_add(*v3);
    *v3 = v3.rotate_left(16);
    *v3 ^= *v2;
    *v0 = v0.wrapping_add(*v3);
    *v3 = v3.rotate_left(21);
    *v3 ^= *v0;
    *v2 = v2.wrapping_add(*v1);
    *v1 = v1.rotate_left(17);
    *v1 ^= *v2;
    *v2 = v2.rotate_left(32);
}

pub fn siphash13(key: [u64; 2], bytes: &[u8]) -> u64 {
    let mut v0 = 0x736f_6d65_7073_6575u64 ^ key[0];
    let mut v1 = 0x646f_7261_6e64_6f6du64 ^ key[1];
    let mut v2 = 0x6c79_6765_6e65_7261u64 ^ key[0];
    let mut v3 = 0x7465_6462_7974_6573u64 ^ key[1];

    let chunks = bytes.chunks_exact(8);
    let rem = chunks.remainder();
    for chunk in chunks {
        let m = u64::from_le_bytes(chunk.try_into().unwrap());
        v3 ^= m;
        sip_round(&mut v0, &mut v1, &mut v2, &mut v3);
        v0 ^= m;
    }

    let mut b = ((bytes.len() as u64) & 0xff) << 56;
    for (i, &byte) in rem.iter().enumerate() {
        b |= u64::from(byte) << (i * 8);
    }
    v3 ^= b;
    sip_round(&mut v0, &mut v1, &mut v2, &mut v3);
    v0 ^= b;

    v2 ^= 0xff;
    for _ in 0..3 {
        sip_round(&mut v0, &mut v1, &mut v2, &mut v3);
    }
    v0 ^ v1 ^ v2 ^ v3
}

pub fn mix(key: [u64; 2], a: u64, b: u64) -> u64 {
    let mut buf = [0u8; 16];
    buf[..8].copy_from_slice(&a.to_le_bytes());
    buf[8..].copy_from_slice(&b.to_le_bytes());
    siphash13(key, &buf)
}

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

fn atomic_hash(key: [u64; 2], ty: u32, kind: TypeKind, value: &Value) -> u64 {
    let h0 = mix(key, u64::from(ty), 0);
    match value {
        Value::Unit => mix(key, h0, 0),
        Value::Bool(b) => mix(key, h0, u64::from(*b)),
        Value::Byte(b) => mix(key, h0, u64::from(*b)),
        Value::Int(n) | Value::Time(n) | Value::Duration(n) => mix(key, h0, *n as u64),
        Value::Char(c) => mix(key, h0, u64::from(*c as u32)),
        Value::Decimal(d) => mix(key, h0, siphash13(key, &d.to_le_bytes())),
        Value::Range(s, e) => {
            let mut buf = [0u8; 16];
            buf[..8].copy_from_slice(&s.to_le_bytes());
            buf[8..].copy_from_slice(&e.to_le_bytes());
            mix(key, h0, siphash13(key, &buf))
        }
        Value::Ref(r) => {
            if let Some(cached) = r.cached_hash() {
                return cached;
            }
            let h = match r.obj() {
                HeapObj::Text(s) => mix(key, mix(key, h0, siphash13(key, s.as_bytes())), 0),
                HeapObj::Bytes(b) => mix(key, mix(key, h0, siphash13(key, b)), 0),
                HeapObj::BigInt { sign, magnitude } => mix(
                    key,
                    mix(key, h0, siphash13(key, magnitude)),
                    u64::from(*sign as u32),
                ),
                _ => unreachable!("unexpected heap obj for atomic kind {kind:?}"),
            };
            if !r.is_immortal() {
                r.set_cached_hash(Some(h));
            }
            h
        }
        _ => unreachable!("unexpected value for atomic kind {kind:?}"),
    }
}

struct HashFrame<'a> {
    ty: u32,
    value: Value,
    cache_node: Option<HeapRef>,
    hash: u64,
    entry_hash: u64,
    next: usize,
    children: Vec<(u32, Value)>,
    active_count: usize,
    entered: bool,
    _marker: std::marker::PhantomData<&'a ()>,
}

pub fn value_hash(types: &TypeTable, key: [u64; 2], ty: u32, value: &Value) -> Option<u64> {
    if (types.abilities(ty) & ABILITY_HASH) == 0 {
        return None;
    }
    let root_kind = types.get(ty).kind;
    if !is_compound(root_kind) {
        return Some(atomic_hash(key, ty, root_kind, value));
    }

    let mut stack = vec![HashFrame {
        ty,
        value: value.clone(),
        cache_node: None,
        hash: 0,
        entry_hash: 0,
        next: 0,
        children: Vec::new(),
        active_count: 0,
        entered: false,
        _marker: std::marker::PhantomData,
    }];

    let mut result = 0u64;
    while let Some(f) = stack.last_mut() {
        let kind = types.get(f.ty).kind;
        if !f.entered {
            if matches!(
                kind,
                TypeKind::Float | TypeKind::Function | TypeKind::Env | TypeKind::Capability
            ) {
                return None;
            }
            // Check hash cache for heap-backed compound objects.
            let cache_ref = match (&f.value, kind) {
                (Value::Ref(r), _) => Some(r.clone()),
                (Value::ChoiceRef { tag, payload }, TypeKind::Choice)
                    if types.case_shape(f.ty, *tag) == CaseShape::Boxed =>
                {
                    Some(payload.clone())
                }
                _ => None,
            };
            if let Some(r) = &cache_ref
                && let Some(cached) = r.cached_hash()
            {
                result = cached;
                stack.pop();
                fold_into_parent(types, key, &mut stack, result);
                continue;
            }
            f.cache_node = cache_ref;
            let tag = match &f.value {
                Value::ChoiceInline { tag, .. } | Value::ChoiceRef { tag, .. } => u64::from(*tag),
                _ => 0,
            };
            f.hash = mix(key, u64::from(f.ty), tag);
            f.entered = true;
            if !is_compound(kind) {
                result = atomic_hash(key, f.ty, kind, &f.value);
                if let Some(r) = &f.cache_node
                    && !r.is_immortal()
                {
                    r.set_cached_hash(Some(result));
                }
                stack.pop();
                fold_into_parent(types, key, &mut stack, result);
                continue;
            }
            collect_children(types, f.ty, &f.value, &mut f.children, &mut f.active_count);
        }

        if f.next == f.children.len() {
            let done_hash = mix(key, f.hash, f.active_count as u64);
            if let Some(r) = &f.cache_node
                && !r.is_immortal()
            {
                r.set_cached_hash(Some(done_hash));
            }
            result = done_hash;
            stack.pop();
            fold_into_parent(types, key, &mut stack, result);
            continue;
        }

        let (child_ty, child_val) = f.children[f.next].clone();
        f.next += 1;
        stack.push(HashFrame {
            ty: child_ty,
            value: child_val,
            cache_node: None,
            hash: 0,
            entry_hash: 0,
            next: 0,
            children: Vec::new(),
            active_count: 0,
            entered: false,
            _marker: std::marker::PhantomData,
        });
    }

    Some(result)
}

fn fold_into_parent(
    types: &TypeTable,
    key: [u64; 2],
    stack: &mut [HashFrame<'_>],
    child_hash: u64,
) {
    if let Some(parent) = stack.last_mut() {
        let pk = types.get(parent.ty).kind;
        match pk {
            TypeKind::Set => {
                parent.hash = parent.hash.wrapping_add(child_hash);
            }
            TypeKind::Map => {
                if parent.next % 2 == 1 {
                    parent.entry_hash = child_hash;
                } else {
                    parent.hash = parent
                        .hash
                        .wrapping_add(mix(key, parent.entry_hash, child_hash));
                }
            }
            _ => {
                parent.hash = mix(key, parent.hash, child_hash);
            }
        }
    }
}

pub(crate) fn inline_payload_to_value(payload: InlinePayload, ty_kind: TypeKind) -> Value {
    match payload {
        InlinePayload::Empty | InlinePayload::Unit => Value::Unit,
        InlinePayload::Bool(b) => Value::Bool(b),
        InlinePayload::Byte(b) => Value::Byte(b),
        InlinePayload::Int(n) => Value::Int(n),
        InlinePayload::Float(x) => Value::Float(x),
        InlinePayload::Char(c) => Value::Char(c),
        InlinePayload::Time(t) => Value::Time(t),
        InlinePayload::Duration(d) => Value::Duration(d),
        InlinePayload::Tag(t) => {
            debug_assert_eq!(ty_kind, TypeKind::Choice);
            Value::ChoiceInline {
                tag: t,
                payload: InlinePayload::Empty,
            }
        }
    }
}

fn collect_children(
    types: &TypeTable,
    ty: u32,
    value: &Value,
    out: &mut Vec<(u32, Value)>,
    active_count: &mut usize,
) {
    let t = types.get(ty);
    match t.kind {
        TypeKind::Choice => match value {
            Value::ChoiceInline { tag, payload } => {
                let c = &t.cases[*tag as usize];
                if c.fields.len() == 1 {
                    let fty = c.fields[0].ty;
                    out.push((fty, inline_payload_to_value(*payload, types.get(fty).kind)));
                }
                *active_count = c.fields.len();
            }
            Value::ChoiceRef { tag, payload } => {
                let c = &t.cases[*tag as usize];
                match types.case_shape(ty, *tag) {
                    CaseShape::Ref => {
                        out.push((c.fields[0].ty, Value::Ref(payload.clone())));
                    }
                    CaseShape::Boxed => {
                        if let HeapObj::CaseBlock { fields, .. } = payload.obj() {
                            for (fdef, fval) in c.fields.iter().zip(fields) {
                                out.push((fdef.ty, fval.clone()));
                            }
                        }
                    }
                    CaseShape::Empty | CaseShape::Inline => {}
                }
                *active_count = c.fields.len();
            }
            _ => unreachable!("non-choice value for choice type"),
        },
        TypeKind::Tuple => {
            if let Value::Ref(r) = value
                && let HeapObj::Tuple { fields, .. } = r.obj()
            {
                for (&fty, fval) in t.parameters.iter().zip(fields) {
                    out.push((fty, fval.clone()));
                }
                *active_count = fields.len();
            }
        }
        TypeKind::Record => {
            if let Value::Ref(r) = value
                && let HeapObj::Record { fields, .. } = r.obj()
            {
                for (fdef, fval) in t.fields.iter().zip(fields) {
                    out.push((fdef.ty, fval.clone()));
                }
                *active_count = fields.len();
            }
        }
        TypeKind::List => {
            if let Value::Ref(r) = value
                && let HeapObj::List(items) = r.obj()
            {
                let elem_ty = t.parameters[0];
                for item in items {
                    out.push((elem_ty, item.clone()));
                }
                *active_count = items.len();
            }
        }
        TypeKind::Set => {
            if let Value::Ref(r) = value
                && let HeapObj::Set(set) = r.obj()
            {
                let elem_ty = t.parameters[0];
                for item in set.iter() {
                    out.push((elem_ty, item.clone()));
                }
                *active_count = set.len();
            }
        }
        TypeKind::Map => {
            if let Value::Ref(r) = value
                && let HeapObj::Map(map) = r.obj()
            {
                let kty = t.parameters[0];
                let vty = t.parameters[1];
                for (k, v) in map.iter() {
                    out.push((kty, k.clone()));
                    out.push((vty, v.clone()));
                }
                *active_count = map.len();
            }
        }
        _ => {}
    }
}
