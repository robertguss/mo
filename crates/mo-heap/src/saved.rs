//! Saved-value binary codec (`MOSV\x01`), exact port of `src/heap/saved.c`.

use std::collections::{HashSet, VecDeque};

use mo_bytecode::types::TypeTable;
use mo_bytecode::{BigIntSign, CaseShape, TypeKind};
use mo_def::limits::{
    MO_LIMIT_COLLECTION_SIZE, MO_LIMIT_SAVED_BYTES, MO_LIMIT_SAVED_DEPTH, MO_LIMIT_SAVED_NODES,
    MO_LIMIT_VALUE_BYTES, MO_TIME_MAX_US, MO_TIME_MIN_US,
};

use crate::table::{MoMap, MoSet};
use crate::{Heap, HeapObj, Value};

const SAVED_MAGIC: &[u8; 5] = b"MOSV\x01";
const SAVED_NO_TAG: u8 = 255;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SavedError {
    TooLarge,
    Depth,
    Nodes,
    Memory,
    Invalid,
}

impl SavedError {
    pub const fn refusal_message(self) -> &'static str {
        match self {
            Self::TooLarge => "MO_LIMIT_SAVED_BYTES (16777216)",
            Self::Depth => "MO_LIMIT_SAVED_DEPTH (128)",
            Self::Nodes => "MO_LIMIT_SAVED_NODES (1048576)",
            Self::Memory => "allocation (--max-heap or native memory)",
            Self::Invalid => "invalid saved value",
        }
    }
}

fn saved_tag(kind: TypeKind) -> u8 {
    match kind {
        TypeKind::Unit => 0,
        TypeKind::Bool => 1,
        TypeKind::Byte => 2,
        TypeKind::Int => 3,
        TypeKind::Float => 4,
        TypeKind::Char => 5,
        TypeKind::Time => 6,
        TypeKind::Duration => 7,
        TypeKind::Decimal => 8,
        TypeKind::Range => 9,
        TypeKind::Text => 10,
        TypeKind::Bytes => 11,
        TypeKind::BigInt => 12,
        TypeKind::Tuple => 13,
        TypeKind::List => 14,
        TypeKind::Set => 15,
        TypeKind::Map => 16,
        TypeKind::Record => 17,
        TypeKind::Choice => 18,
        TypeKind::Function | TypeKind::Env | TypeKind::Capability => SAVED_NO_TAG,
    }
}

struct Writer<'a> {
    out: &'a mut Vec<u8>,
    limit: usize,
    track_sharing: bool,
    shared: bool,
    visited: HashSet<usize>,
}

impl<'a> Writer<'a> {
    fn new(out: &'a mut Vec<u8>, limit: usize, track_sharing: bool) -> Self {
        Self {
            out,
            limit,
            track_sharing,
            shared: false,
            visited: HashSet::new(),
        }
    }

    fn append(&mut self, bytes: &[u8]) -> Result<(), SavedError> {
        if bytes.len() > self.limit.saturating_sub(self.out.len()) {
            return Err(SavedError::TooLarge);
        }
        self.out.extend_from_slice(bytes);
        Ok(())
    }

    fn write_u8(&mut self, v: u8) -> Result<(), SavedError> {
        self.append(&[v])
    }

    fn write_u32(&mut self, v: u32) -> Result<(), SavedError> {
        self.append(&v.to_le_bytes())
    }

    fn write_u64(&mut self, v: u64) -> Result<(), SavedError> {
        self.append(&v.to_le_bytes())
    }

    fn write_name(&mut self, text: Option<&str>) -> Result<(), SavedError> {
        let s = text.unwrap_or("");
        let len = u32::try_from(s.len()).map_err(|_| SavedError::TooLarge)?;
        self.write_u32(len)?;
        self.append(s.as_bytes())
    }
}

fn write_schema_inner(
    types: &TypeTable,
    index: u32,
    depth: u32,
    seen: &mut [Option<u32>],
    next: &mut u32,
    w: &mut Writer<'_>,
) -> Result<(), SavedError> {
    if depth as u64 == MO_LIMIT_SAVED_DEPTH {
        return Err(SavedError::Depth);
    }
    if let Some(prev) = seen[index as usize] {
        w.write_u8(255)?;
        return w.write_u32(prev);
    }
    seen[index as usize] = Some(*next);
    *next += 1;

    let t = types.get(index);
    let tag = saved_tag(t.kind);
    if tag == SAVED_NO_TAG {
        return Err(SavedError::Invalid);
    }
    w.write_u8(tag)?;
    if matches!(t.kind, TypeKind::Record | TypeKind::Choice) {
        w.write_name(t.name.as_deref())?;
    }
    w.write_u32(t.parameters.len() as u32)?;
    for &p in &t.parameters {
        write_schema_inner(types, p, depth + 1, seen, next, w)?;
    }

    let groups = if t.kind == TypeKind::Choice {
        t.cases.len()
    } else {
        1
    };
    w.write_u32(groups as u32)?;
    for c in 0..groups {
        let fields = if t.kind == TypeKind::Choice {
            let case = &t.cases[c];
            w.write_name(Some(case.name.as_str()))?;
            w.write_u8(u8::from(case.unnamed))?;
            &case.fields
        } else {
            &t.fields
        };
        w.write_u32(fields.len() as u32)?;
        for f in fields {
            w.write_name(f.name.as_deref())?;
            write_schema_inner(types, f.ty, depth + 1, seen, next, w)?;
        }
    }
    Ok(())
}

pub fn write_schema(types: &TypeTable, ty: u32, out: &mut Vec<u8>) -> Result<(), SavedError> {
    let start = out.len();
    let mut seen = vec![None; types.items.len()];
    let mut next = 0u32;
    let mut w = Writer::new(out, start + MO_LIMIT_SAVED_BYTES as usize, false);
    if let Err(e) = write_schema_inner(types, ty, 0, &mut seen, &mut next, &mut w) {
        out.truncate(start);
        return Err(e);
    }
    Ok(())
}

fn encode_inner(
    types: &TypeTable,
    ty: u32,
    val: &Value,
    w: &mut Writer<'_>,
    depth: u32,
    nodes: &mut u32,
) -> Result<(), SavedError> {
    if depth as u64 == MO_LIMIT_SAVED_DEPTH {
        return Err(SavedError::Depth);
    }
    if *nodes as u64 == MO_LIMIT_SAVED_NODES {
        return Err(SavedError::Nodes);
    }
    *nodes += 1;

    if w.track_sharing && !w.shared {
        let heap_ref = match val {
            Value::Ref(r) => Some(r),
            Value::ChoiceRef { tag, payload } if types.case_shape(ty, *tag) == CaseShape::Boxed => {
                Some(payload)
            }
            _ => None,
        };
        if let Some(r) = heap_ref
            && (r.is_immortal() || !w.visited.insert(r.node_id()))
        {
            w.shared = true;
        }
    }

    let t = types.get(ty);
    match t.kind {
        TypeKind::Unit => Ok(()),
        TypeKind::Bool => w.write_u8(u8::from(val.as_bool())),
        TypeKind::Byte => w.write_u8(val.as_byte()),
        TypeKind::Char => w.write_u32(val.as_char() as u32),
        TypeKind::Int => w.write_u64(val.as_int() as u64),
        TypeKind::Time => w.write_u64(val.as_time() as u64),
        TypeKind::Duration => w.write_u64(val.as_duration() as u64),
        TypeKind::Float => w.write_u64(val.as_float().to_bits()),
        TypeKind::Decimal => {
            let bits = val.as_decimal() as u128;
            w.write_u64(bits as u64)?;
            w.write_u64((bits >> 64) as u64)
        }
        TypeKind::Range => {
            let (start, end) = val.as_range();
            w.write_u64(start as u64)?;
            w.write_u64(end as u64)
        }
        TypeKind::Text => {
            let b = val.as_text().as_bytes();
            let len = u32::try_from(b.len()).map_err(|_| SavedError::TooLarge)?;
            w.write_u32(len)?;
            w.append(b)
        }
        TypeKind::Bytes => {
            let b = val.as_bytes();
            let len = u32::try_from(b.len()).map_err(|_| SavedError::TooLarge)?;
            w.write_u32(len)?;
            w.append(b)
        }
        TypeKind::BigInt => {
            let (sign, mag) = val.as_bigint();
            w.write_u8(sign as u8)?;
            let len = u32::try_from(mag.len()).map_err(|_| SavedError::TooLarge)?;
            w.write_u32(len)?;
            w.append(mag)
        }
        TypeKind::Map => {
            let map = val.as_map();
            w.write_u32(map.len() as u32)?;
            for (k, v) in map.iter() {
                encode_inner(types, t.parameters[0], k, w, depth + 1, nodes)?;
                encode_inner(types, t.parameters[1], v, w, depth + 1, nodes)?;
            }
            Ok(())
        }
        TypeKind::Set => {
            let set = val.as_set();
            w.write_u32(set.len() as u32)?;
            for item in set.iter() {
                encode_inner(types, t.parameters[0], item, w, depth + 1, nodes)?;
            }
            Ok(())
        }
        TypeKind::List => {
            let list = val.as_list();
            w.write_u32(list.len() as u32)?;
            for item in list.iter() {
                encode_inner(types, t.parameters[0], item, w, depth + 1, nodes)?;
            }
            Ok(())
        }
        TypeKind::Record => {
            let HeapObj::Record { fields, .. } = val.as_heap_ref().obj() else {
                return Err(SavedError::Invalid);
            };
            w.write_u32(fields.len() as u32)?;
            for (f, field_def) in fields.iter().zip(&t.fields) {
                encode_inner(types, field_def.ty, f, w, depth + 1, nodes)?;
            }
            Ok(())
        }
        TypeKind::Tuple => {
            let HeapObj::Tuple { fields, .. } = val.as_heap_ref().obj() else {
                return Err(SavedError::Invalid);
            };
            w.write_u32(fields.len() as u32)?;
            for (f, &param_ty) in fields.iter().zip(&t.parameters) {
                encode_inner(types, param_ty, f, w, depth + 1, nodes)?;
            }
            Ok(())
        }
        TypeKind::Choice => {
            let tag = val.as_tag();
            w.write_u32(tag)?;
            let case = &t.cases[tag as usize];
            w.write_u32(case.fields.len() as u32)?;
            match types.case_shape(ty, tag) {
                CaseShape::Empty => Ok(()),
                CaseShape::Inline => match val {
                    Value::ChoiceInline { payload, .. } => {
                        let fty = case.fields[0].ty;
                        let child =
                            crate::hash::inline_payload_to_value(*payload, types.get(fty).kind);
                        encode_inner(types, fty, &child, w, depth + 1, nodes)
                    }
                    _ => Err(SavedError::Invalid),
                },
                CaseShape::Ref => match val {
                    Value::ChoiceRef { payload, .. } => {
                        let fty = case.fields[0].ty;
                        let child = Value::Ref(payload.clone());
                        encode_inner(types, fty, &child, w, depth + 1, nodes)
                    }
                    _ => Err(SavedError::Invalid),
                },
                CaseShape::Boxed => match val {
                    Value::ChoiceRef { payload, .. } => {
                        let HeapObj::CaseBlock { fields, .. } = payload.obj() else {
                            return Err(SavedError::Invalid);
                        };
                        for (f, field_def) in fields.iter().zip(&case.fields) {
                            encode_inner(types, field_def.ty, f, w, depth + 1, nodes)?;
                        }
                        Ok(())
                    }
                    _ => Err(SavedError::Invalid),
                },
            }
        }
        TypeKind::Function | TypeKind::Env | TypeKind::Capability => Err(SavedError::Invalid),
    }
}

pub fn encode_value_unshared(
    types: &TypeTable,
    ty: u32,
    val: &Value,
    out: &mut Vec<u8>,
) -> Result<bool, SavedError> {
    let mut shape = Vec::new();
    write_schema(types, ty, &mut shape)?;

    let mut bytes = Vec::new();
    let mut w = Writer::new(&mut bytes, MO_LIMIT_SAVED_BYTES as usize, true);
    w.append(SAVED_MAGIC)?;
    let shape_len = u32::try_from(shape.len()).map_err(|_| SavedError::TooLarge)?;
    w.write_u32(shape_len)?;
    w.append(&shape)?;
    let mut nodes = 0u32;
    encode_inner(types, ty, val, &mut w, 0, &mut nodes)?;
    let unshared = !w.shared;
    out.extend_from_slice(&bytes);
    Ok(unshared)
}

pub fn encode_value(
    types: &TypeTable,
    ty: u32,
    val: &Value,
    out: &mut Vec<u8>,
) -> Result<(), SavedError> {
    let mut shape = Vec::new();
    write_schema(types, ty, &mut shape)?;

    let mut bytes = Vec::new();
    let mut w = Writer::new(&mut bytes, MO_LIMIT_SAVED_BYTES as usize, false);
    w.append(SAVED_MAGIC)?;
    let shape_len = u32::try_from(shape.len()).map_err(|_| SavedError::TooLarge)?;
    w.write_u32(shape_len)?;
    w.append(&shape)?;
    let mut nodes = 0u32;
    encode_inner(types, ty, val, &mut w, 0, &mut nodes)?;
    out.extend_from_slice(&bytes);
    Ok(())
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct Decoder<'a> {
    heap: &'a Heap,
    types: &'a TypeTable,
    bytes: &'a [u8],
    pos: usize,
    nodes: u32,
}

impl<'a> Decoder<'a> {
    fn read_slice(&mut self, n: usize) -> Result<&'a [u8], SavedError> {
        if n > self.bytes.len().saturating_sub(self.pos) {
            return Err(SavedError::Invalid);
        }
        let s = &self.bytes[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }

    fn read_u8(&mut self) -> Result<u8, SavedError> {
        Ok(self.read_slice(1)?[0])
    }

    fn read_u32(&mut self) -> Result<u32, SavedError> {
        let s = self.read_slice(4)?;
        Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }

    fn read_u64(&mut self) -> Result<u64, SavedError> {
        let s = self.read_slice(8)?;
        Ok(u64::from_le_bytes([
            s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
        ]))
    }
}

fn decode_inner(d: &mut Decoder<'_>, ty: u32, depth: u32) -> Result<Value, SavedError> {
    if depth as u64 == MO_LIMIT_SAVED_DEPTH {
        return Err(SavedError::Depth);
    }
    if d.nodes as u64 == MO_LIMIT_SAVED_NODES {
        return Err(SavedError::Nodes);
    }
    d.nodes += 1;

    let t = d.types.get(ty);
    match t.kind {
        TypeKind::Unit => Ok(Value::Unit),
        TypeKind::Bool => match d.read_u8()? {
            0 => Ok(Value::Bool(false)),
            1 => Ok(Value::Bool(true)),
            _ => Err(SavedError::Invalid),
        },
        TypeKind::Byte => Ok(Value::Byte(d.read_u8()?)),
        TypeKind::Char => {
            let code = d.read_u32()?;
            let ch = char::from_u32(code).ok_or(SavedError::Invalid)?;
            Ok(Value::Char(ch))
        }
        TypeKind::Int => Ok(Value::Int(d.read_u64()? as i64)),
        TypeKind::Float => Ok(Value::Float(f64::from_bits(d.read_u64()?))),
        TypeKind::Time => {
            let v = d.read_u64()? as i64;
            if !(MO_TIME_MIN_US..=MO_TIME_MAX_US).contains(&v) {
                return Err(SavedError::Invalid);
            }
            Ok(Value::Time(v))
        }
        TypeKind::Duration => Ok(Value::Duration(d.read_u64()? as i64)),
        TypeKind::Decimal => {
            let lo = d.read_u64()?;
            let hi = d.read_u64()?;
            let bits = (u128::from(hi) << 64) | u128::from(lo);
            if bits == (1u128 << 127) {
                return Err(SavedError::Invalid);
            }
            Ok(Value::Decimal(bits as i128))
        }
        TypeKind::Range => {
            let start = d.read_u64()? as i64;
            let end = d.read_u64()? as i64;
            Ok(Value::Range(start, end))
        }
        TypeKind::Text => {
            let len = d.read_u32()? as usize;
            if len as u64 > MO_LIMIT_VALUE_BYTES {
                return Err(SavedError::Invalid);
            }
            let raw = d.read_slice(len)?;
            let s = std::str::from_utf8(raw).map_err(|_| SavedError::Invalid)?;
            d.heap
                .alloc_text(s.to_string())
                .map_err(|_| SavedError::Memory)
        }
        TypeKind::Bytes => {
            let len = d.read_u32()? as usize;
            if len as u64 > MO_LIMIT_VALUE_BYTES {
                return Err(SavedError::Invalid);
            }
            let raw = d.read_slice(len)?;
            d.heap
                .alloc_bytes(raw.to_vec())
                .map_err(|_| SavedError::Memory)
        }
        TypeKind::BigInt => {
            let sign_byte = d.read_u8()?;
            let sign = BigIntSign::from_u32(u32::from(sign_byte)).ok_or(SavedError::Invalid)?;
            let len = d.read_u32()? as usize;
            if len as u64 > MO_LIMIT_VALUE_BYTES {
                return Err(SavedError::Invalid);
            }
            let mag = d.read_slice(len)?;
            if !crate::bigint::is_canonical(sign, mag) {
                return Err(SavedError::Invalid);
            }
            d.heap
                .alloc_bigint(sign, mag.to_vec())
                .map_err(|_| SavedError::Memory)
        }
        TypeKind::List => {
            let count = d.read_u32()? as u64;
            if count > MO_LIMIT_COLLECTION_SIZE || count > MO_LIMIT_SAVED_NODES - u64::from(d.nodes)
            {
                return Err(SavedError::Invalid);
            }
            let elem_ty = t.parameters[0];
            let mut items = VecDeque::with_capacity(count as usize);
            for _ in 0..count {
                items.push_back(decode_inner(d, elem_ty, depth + 1)?);
            }
            d.heap.alloc_list(items).map_err(|_| SavedError::Memory)
        }
        TypeKind::Set => {
            let count = d.read_u32()? as u64;
            if count > MO_LIMIT_COLLECTION_SIZE || count > MO_LIMIT_SAVED_NODES - u64::from(d.nodes)
            {
                return Err(SavedError::Invalid);
            }
            let elem_ty = t.parameters[0];
            let mut set = MoSet::new(elem_ty);
            for _ in 0..count {
                let item = decode_inner(d, elem_ty, depth + 1)?;
                set.add(d.types, d.heap.hash_key(), item);
            }
            if set.len() as u64 != count {
                return Err(SavedError::Invalid);
            }
            d.heap.alloc_set(set).map_err(|_| SavedError::Memory)
        }
        TypeKind::Map => {
            let count = d.read_u32()? as u64;
            let cells = count.saturating_mul(2);
            if count > MO_LIMIT_COLLECTION_SIZE || cells > MO_LIMIT_SAVED_NODES - u64::from(d.nodes)
            {
                return Err(SavedError::Invalid);
            }
            let key_ty = t.parameters[0];
            let val_ty = t.parameters[1];
            let mut map = MoMap::new(key_ty, val_ty);
            for _ in 0..count {
                let k = decode_inner(d, key_ty, depth + 1)?;
                let v = decode_inner(d, val_ty, depth + 1)?;
                map.set(d.types, d.heap.hash_key(), k, v);
            }
            if map.len() as u64 != count {
                return Err(SavedError::Invalid);
            }
            d.heap.alloc_map(map).map_err(|_| SavedError::Memory)
        }
        TypeKind::Record => {
            let count = d.read_u32()? as usize;
            if count != t.fields.len() || (count as u64) > MO_LIMIT_SAVED_NODES - u64::from(d.nodes)
            {
                return Err(SavedError::Invalid);
            }
            let mut fields = Vec::with_capacity(count);
            for f in &t.fields {
                fields.push(decode_inner(d, f.ty, depth + 1)?);
            }
            let layout = d.types.find_record_layout(ty).ok_or(SavedError::Invalid)?;
            d.heap
                .construct(d.types, layout, fields)
                .map_err(|_| SavedError::Memory)
        }
        TypeKind::Tuple => {
            let count = d.read_u32()? as usize;
            if count != t.parameters.len()
                || (count as u64) > MO_LIMIT_SAVED_NODES - u64::from(d.nodes)
            {
                return Err(SavedError::Invalid);
            }
            let mut fields = Vec::with_capacity(count);
            for &param_ty in &t.parameters {
                fields.push(decode_inner(d, param_ty, depth + 1)?);
            }
            let layout = d.types.find_tuple_layout(ty).ok_or(SavedError::Invalid)?;
            d.heap
                .construct(d.types, layout, fields)
                .map_err(|_| SavedError::Memory)
        }
        TypeKind::Choice => {
            let tag = d.read_u32()?;
            if (tag as usize) >= t.cases.len() {
                return Err(SavedError::Invalid);
            }
            let case = &t.cases[tag as usize];
            let count = d.read_u32()? as usize;
            if count != case.fields.len()
                || (count as u64) > MO_LIMIT_SAVED_NODES - u64::from(d.nodes)
            {
                return Err(SavedError::Invalid);
            }
            let mut fields = Vec::with_capacity(count);
            for f in &case.fields {
                fields.push(decode_inner(d, f.ty, depth + 1)?);
            }
            d.heap
                .make_case(d.types, ty, tag, fields)
                .map_err(|_| SavedError::Memory)
        }
        TypeKind::Function | TypeKind::Env | TypeKind::Capability => Err(SavedError::Invalid),
    }
}

pub fn decode_value(
    heap: &Heap,
    types: &TypeTable,
    ty: u32,
    bytes: &[u8],
) -> Result<Value, SavedError> {
    if bytes.len() as u64 > MO_LIMIT_SAVED_BYTES || bytes.len() < 9 || &bytes[..5] != SAVED_MAGIC {
        return Err(SavedError::Invalid);
    }
    let mut d = Decoder {
        heap,
        types,
        bytes,
        pos: 5,
        nodes: 0,
    };
    let shape_len = d.read_u32()? as usize;
    let shape_bytes = d.read_slice(shape_len)?;
    let mut expected_shape = Vec::new();
    write_schema(types, ty, &mut expected_shape)?;
    if expected_shape.as_slice() != shape_bytes {
        return Err(SavedError::Invalid);
    }
    let val = decode_inner(&mut d, ty, 0)?;
    if d.pos != bytes.len() {
        return Err(SavedError::Invalid);
    }
    Ok(val)
}

/// Copies a value between heaps through the saved codec, matching `mo_value_copy_saved`.
/// Returns `(decoded_value, encoded_byte_length)`.
pub fn copy_saved(
    types: &TypeTable,
    to_heap: &Heap,
    ty: u32,
    val: &Value,
) -> Result<(Value, usize), SavedError> {
    let mut bytes = Vec::new();
    encode_value(types, ty, val, &mut bytes)?;
    let encoded_len = bytes.len();
    let decoded = decode_value(to_heap, types, ty, &bytes).map_err(|_| SavedError::Memory)?;
    Ok((decoded, encoded_len))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::equal::value_equal;
    use mo_bytecode::{LayoutDef, LayoutKind, NO_INDEX, TypeCase, TypeDef, TypeField};

    fn base_type(kind: TypeKind) -> TypeDef {
        TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        }
    }

    #[test]
    fn saved_codec_round_trips_scalars_collections_records_and_choices() {
        let mut items = vec![
            base_type(TypeKind::Unit),     // 0
            base_type(TypeKind::Bool),     // 1
            base_type(TypeKind::Byte),     // 2
            base_type(TypeKind::Int),      // 3
            base_type(TypeKind::Float),    // 4
            base_type(TypeKind::Char),     // 5
            base_type(TypeKind::Time),     // 6
            base_type(TypeKind::Duration), // 7
            base_type(TypeKind::Decimal),  // 8
            base_type(TypeKind::Range),    // 9
            base_type(TypeKind::Text),     // 10
            base_type(TypeKind::Bytes),    // 11
            base_type(TypeKind::BigInt),   // 12
        ];
        let mut list_int = base_type(TypeKind::List);
        list_int.parameters = vec![3];
        items.push(list_int); // 13

        let mut map_ti = base_type(TypeKind::Map);
        map_ti.parameters = vec![10, 3];
        items.push(map_ti); // 14

        let mut opt_text = base_type(TypeKind::Choice);
        opt_text.name = Some("Option".into());
        opt_text.has_fields = true;
        opt_text.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "None".into(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "Some".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: 10,
                }],
                unnamed: true,
            },
        ];
        items.push(opt_text); // 15

        let mut rec = base_type(TypeKind::Record);
        rec.name = Some("State".into());
        rec.fields = vec![
            TypeField {
                name_idx: None,
                name: Some("n".into()),
                ty: 3,
            },
            TypeField {
                name_idx: None,
                name: Some("m".into()),
                ty: 14,
            },
            TypeField {
                name_idx: None,
                name: Some("o".into()),
                ty: 15,
            },
        ];
        items.push(rec); // 16

        let layouts = vec![LayoutDef {
            kind: LayoutKind::Record,
            ty: 16,
            case_index: 0,
            function: NO_INDEX,
            fields: vec![3, 14, 15],
        }];
        let types = TypeTable::finish(items, layouts).unwrap();
        let heap = Heap::new(16 << 20);

        let mut m = MoMap::new(10, 3);
        m.set(
            &types,
            heap.hash_key(),
            heap.alloc_text("a".into()).unwrap(),
            Value::Int(1),
        );
        m.set(
            &types,
            heap.hash_key(),
            heap.alloc_text("b".into()).unwrap(),
            Value::Int(2),
        );
        let mv = heap.alloc_map(m).unwrap();
        let ov = heap
            .make_option(&types, 15, Some(heap.alloc_text("hi".into()).unwrap()))
            .unwrap();
        let state = heap
            .construct(&types, 0, vec![Value::Int(42), mv, ov])
            .unwrap();

        let mut encoded = Vec::new();
        let unshared = encode_value_unshared(&types, 16, &state, &mut encoded).unwrap();
        assert!(unshared);
        assert_eq!(&encoded[..5], b"MOSV\x01");

        let heap2 = Heap::new(16 << 20);
        let decoded = decode_value(&heap2, &types, 16, &encoded).unwrap();
        assert!(value_equal(&types, heap2.hash_key(), 16, &state, &decoded));

        // Corrupt trailing byte rejected.
        let mut bad = encoded.clone();
        bad.push(0);
        assert_eq!(
            decode_value(&heap2, &types, 16, &bad).err(),
            Some(SavedError::Invalid)
        );

        // Shared DAG node detected as !unshared.
        let s = heap.alloc_text("shared".into()).unwrap();
        let mut pair_list_ty = vec![base_type(TypeKind::Text)];
        let mut lt = base_type(TypeKind::List);
        lt.parameters = vec![0];
        pair_list_ty.push(lt);
        let types2 = TypeTable::finish(pair_list_ty, Vec::new()).unwrap();
        let shared_list = heap.alloc_list(VecDeque::from([s.clone(), s])).unwrap();
        let mut buf2 = Vec::new();
        assert!(!encode_value_unshared(&types2, 1, &shared_list, &mut buf2).unwrap());
    }

    #[test]
    fn saved_codec_enforces_depth_and_scalar_validation() {
        let mut items = vec![base_type(TypeKind::Bool), base_type(TypeKind::Decimal)];
        let mut node = base_type(TypeKind::Choice);
        node.name = Some("Node".into());
        node.has_fields = true;
        node.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "End".into(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "Wrap".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: 2,
                }],
                unnamed: true,
            },
        ];
        items.push(node);
        let layouts = vec![LayoutDef {
            kind: LayoutKind::Case,
            ty: 2,
            case_index: 1,
            function: NO_INDEX,
            fields: vec![2],
        }];
        let types = TypeTable::finish(items, layouts).unwrap();
        let heap = Heap::new(16 << 20);

        // Depth 128 rejected.
        let mut cur = heap.make_case(&types, 2, 0, vec![]).unwrap();
        for _ in 0..128 {
            cur = heap.make_case(&types, 2, 1, vec![cur]).unwrap();
        }
        let mut out = Vec::new();
        assert_eq!(
            encode_value(&types, 2, &cur, &mut out),
            Err(SavedError::Depth)
        );

        // Invalid Bool byte (2) rejected on decode.
        let mut bool_bytes = Vec::new();
        encode_value(&types, 0, &Value::Bool(true), &mut bool_bytes).unwrap();
        *bool_bytes.last_mut().unwrap() = 2;
        assert_eq!(
            decode_value(&heap, &types, 0, &bool_bytes).err(),
            Some(SavedError::Invalid)
        );

        // Invalid Decimal (i128::MIN) rejected on decode.
        let mut dec_bytes = Vec::new();
        encode_value(&types, 1, &Value::Decimal(0), &mut dec_bytes).unwrap();
        let len = dec_bytes.len();
        dec_bytes[len - 16..].copy_from_slice(&(1u128 << 127).to_le_bytes());
        assert_eq!(
            decode_value(&heap, &types, 1, &dec_bytes).err(),
            Some(SavedError::Invalid)
        );
    }
}
