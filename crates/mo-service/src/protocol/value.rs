//! Bidirectional bridge between service heap `Value`s and protocol `McpValue.Value` / `McpScalar.Scalar`
//! (`src/tools/protocol-scalar.c` and `src/tools/protocol-value.c` parity).

use std::collections::VecDeque;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{CaseShape, ServiceDef, TypeKind};
use mo_def::limits::MO_LIMIT_MCP_VALUE_DEPTH;
use mo_heap::table::{MoMap, MoSet};
use mo_heap::{Heap, HeapObj, Value};

/// Conversion status (`enum mo_protocol_value_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueBridgeStatus {
    /// Conversion succeeded.
    Ok,
    /// Input value shape or scalar did not match the target type.
    Invalid,
    /// Target heap budget exceeded.
    Memory,
    /// Depth, node, byte, or work budget exceeded.
    Limit,
}

/// Shared type context for packing/unpacking values across protocol and service heaps.
pub struct ValueBridge<'a> {
    /// Protocol image's type table.
    pub protocol: &'a TypeTable,
    /// Service image's type table.
    pub service: &'a TypeTable,
    /// Service descriptor (for `shapes` struct-vs-choice classification).
    pub descriptor: &'a ServiceDef,
    /// Protocol type index of `McpValue.Value`.
    pub value_ty: u32,
    /// Optional remaining work budget.
    pub work: Option<&'a mut u64>,
    /// Optional node limit (`0` means unlimited).
    pub node_limit: usize,
}

const SCALAR_KINDS: &[TypeKind] = &[
    TypeKind::Unit,
    TypeKind::Bool,
    TypeKind::Byte,
    TypeKind::Int,
    TypeKind::Float,
    TypeKind::Char,
    TypeKind::Time,
    TypeKind::Duration,
    TypeKind::Decimal,
    TypeKind::Text,
    TypeKind::Bytes,
    TypeKind::BigInt,
    TypeKind::Range,
];

fn scalar_tag(kind: TypeKind) -> Option<u32> {
    SCALAR_KINDS
        .iter()
        .position(|&k| k == kind)
        .map(|i| i as u32)
}

fn is_composite(kind: TypeKind) -> bool {
    matches!(
        kind,
        TypeKind::Tuple
            | TypeKind::List
            | TypeKind::Set
            | TypeKind::Map
            | TypeKind::Record
            | TypeKind::Choice
    )
}

fn case_field(types: &TypeTable, ty: u32, val: &Value, field: usize) -> Option<Value> {
    let tag = val.as_tag();
    let case = types.get(ty).cases.get(tag as usize)?;
    match types.case_shape(ty, tag) {
        CaseShape::Empty => None,
        CaseShape::Inline => match val {
            Value::ChoiceInline { payload, .. } if field == 0 => {
                let fty = case.fields.first()?.ty;
                Some(mo_heap::hash::inline_payload_to_value(
                    *payload,
                    types.get(fty).kind,
                ))
            }
            _ => None,
        },
        CaseShape::Ref => match val {
            Value::ChoiceRef { payload, .. } if field == 0 => Some(Value::Ref(payload.clone())),
            _ => None,
        },
        CaseShape::Boxed => match val {
            Value::ChoiceRef { payload, .. } => match payload.obj() {
                HeapObj::CaseBlock { fields, .. } => fields.get(field).cloned(),
                _ => None,
            },
            _ => None,
        },
    }
}

impl ValueBridge<'_> {
    fn charge(&mut self, amount: u64) -> bool {
        let Some(ref mut w) = self.work else {
            return true;
        };
        if amount > **w {
            **w = 0;
            return false;
        }
        **w -= amount;
        true
    }

    fn scalar_ty(&self) -> u32 {
        self.protocol.get(self.value_ty).cases[0].fields[0].ty
    }

    fn is_struct(&self, svc_ty: u32) -> bool {
        self.descriptor
            .shapes
            .binary_search_by_key(&svc_ty, |s| s.ty)
            .ok()
            .is_some_and(|i| self.descriptor.shapes[i].is_struct)
    }

    fn charge_scalar(&mut self, kind: TypeKind, val: &Value, packed: bool) -> bool {
        if self.work.is_none() {
            return true;
        }
        if !self.charge(1) {
            return false;
        }
        let (eff_kind, eff_val) = if packed {
            let sty = self.scalar_ty();
            let tag = val.as_tag();
            let shape = &self.protocol.get(sty).cases[tag as usize];
            if shape.fields.is_empty() {
                return true;
            }
            let fk = self.protocol.get(shape.fields[0].ty).kind;
            let Some(inner) = case_field(self.protocol, sty, val, 0) else {
                return false;
            };
            (fk, inner)
        } else {
            (kind, val.clone())
        };
        let extra = match eff_kind {
            TypeKind::Text => eff_val.as_text().len() as u64,
            TypeKind::Bytes => eff_val.as_bytes().len() as u64,
            TypeKind::BigInt => eff_val.as_bigint().1.len() as u64,
            TypeKind::List if packed => eff_val.as_list().len() as u64,
            _ => 0,
        };
        self.charge(extra)
    }

    /// Packs a service heap `Value` into a protocol `McpValue.Value` (`mo_protocol_value_pack`).
    pub fn pack(
        &mut self,
        to: &Heap,
        svc_ty: u32,
        val: &Value,
        byte_limit: usize,
    ) -> Result<Value, ValueBridgeStatus> {
        let mut nodes = if self.node_limit == 0 {
            usize::MAX
        } else {
            self.node_limit
        };
        self.pack_inner(
            to,
            svc_ty,
            val,
            byte_limit,
            MO_LIMIT_MCP_VALUE_DEPTH as u32,
            &mut nodes,
        )
    }

    fn pack_inner(
        &mut self,
        to: &Heap,
        svc_ty: u32,
        val: &Value,
        byte_limit: usize,
        depth: u32,
        nodes: &mut usize,
    ) -> Result<Value, ValueBridgeStatus> {
        if depth == 0 || *nodes == 0 || !self.charge(1) {
            return Err(ValueBridgeStatus::Limit);
        }
        *nodes -= 1;

        let t = self.service.get(svc_ty);
        if !is_composite(t.kind) {
            if !self.charge_scalar(t.kind, val, false) {
                return Err(ValueBridgeStatus::Limit);
            }
            let sty = self.scalar_ty();
            let leaf = pack_scalar(self.protocol, sty, to, t.kind, val, byte_limit)?;
            return to
                .make_case(self.protocol, self.value_ty, 0, vec![leaf])
                .map_err(|_| ValueBridgeStatus::Memory);
        }

        let mut variant = false;
        let mut tag = 0u32;
        let children: Vec<(u32, Value)> = match t.kind {
            TypeKind::Choice => {
                tag = val.as_tag();
                variant = !self.is_struct(svc_ty);
                let case = &t.cases[tag as usize];
                let mut v = Vec::with_capacity(case.fields.len());
                for (i, f) in case.fields.iter().enumerate() {
                    let c = case_field(self.service, svc_ty, val, i)
                        .ok_or(ValueBridgeStatus::Invalid)?;
                    v.push((f.ty, c));
                }
                v
            }
            TypeKind::Record => {
                let HeapObj::Record { fields, .. } = val.as_heap_ref().obj() else {
                    return Err(ValueBridgeStatus::Invalid);
                };
                fields
                    .iter()
                    .zip(&t.fields)
                    .map(|(v, f)| (f.ty, v.clone()))
                    .collect()
            }
            TypeKind::Tuple => {
                let HeapObj::Tuple { fields, .. } = val.as_heap_ref().obj() else {
                    return Err(ValueBridgeStatus::Invalid);
                };
                fields
                    .iter()
                    .zip(&t.parameters)
                    .map(|(v, &ty)| (ty, v.clone()))
                    .collect()
            }
            TypeKind::List => {
                let ety = t.parameters[0];
                val.as_list().iter().map(|v| (ety, v.clone())).collect()
            }
            TypeKind::Set => {
                let ety = t.parameters[0];
                val.as_set().iter().map(|v| (ety, v.clone())).collect()
            }
            TypeKind::Map => {
                let kty = t.parameters[0];
                let vty = t.parameters[1];
                let m = val.as_map();
                if m.len() > (u32::MAX / 2) as usize {
                    return Err(ValueBridgeStatus::Limit);
                }
                let mut v = Vec::with_capacity(m.len() * 2);
                for (k, val_item) in m.iter() {
                    v.push((kty, k.clone()));
                    v.push((vty, val_item.clone()));
                }
                v
            }
            _ => return Err(ValueBridgeStatus::Invalid),
        };

        if children.len() > *nodes || !self.charge(children.len() as u64) {
            return Err(ValueBridgeStatus::Limit);
        }

        let mut list = VecDeque::with_capacity(children.len());
        for (cty, cval) in children {
            list.push_back(self.pack_inner(to, cty, &cval, byte_limit, depth - 1, nodes)?);
        }
        let list_val = to.alloc_list(list).map_err(|_| ValueBridgeStatus::Memory)?;
        if variant {
            to.make_case(
                self.protocol,
                self.value_ty,
                2,
                vec![Value::Int(i64::from(tag)), list_val],
            )
            .map_err(|_| ValueBridgeStatus::Memory)
        } else {
            to.make_case(self.protocol, self.value_ty, 1, vec![list_val])
                .map_err(|_| ValueBridgeStatus::Memory)
        }
    }

    /// Unpacks a protocol `McpValue.Value` into a service heap `Value` (`mo_protocol_value_unpack`).
    pub fn unpack(
        &mut self,
        to: &Heap,
        svc_ty: u32,
        val: &Value,
        byte_limit: usize,
    ) -> Result<Value, ValueBridgeStatus> {
        self.unpack_inner(to, svc_ty, val, byte_limit, MO_LIMIT_MCP_VALUE_DEPTH as u32)
    }

    fn unpack_inner(
        &mut self,
        to: &Heap,
        svc_ty: u32,
        val: &Value,
        byte_limit: usize,
        depth: u32,
    ) -> Result<Value, ValueBridgeStatus> {
        if depth == 0 || !self.charge(1) {
            return Err(ValueBridgeStatus::Limit);
        }
        let t = self.service.get(svc_ty);
        let shape = val.as_tag();
        if !is_composite(t.kind) {
            if shape != 0 {
                return Err(ValueBridgeStatus::Invalid);
            }
            let scalar = case_field(self.protocol, self.value_ty, val, 0)
                .ok_or(ValueBridgeStatus::Invalid)?;
            if !self.charge_scalar(t.kind, &scalar, true) {
                return Err(ValueBridgeStatus::Limit);
            }
            let sty = self.scalar_ty();
            return unpack_scalar(self.protocol, sty, to, t.kind, &scalar, byte_limit);
        }

        let (tag, list_val) = if t.kind == TypeKind::Choice && !self.is_struct(svc_ty) {
            if shape != 2 {
                return Err(ValueBridgeStatus::Invalid);
            }
            let choice = case_field(self.protocol, self.value_ty, val, 0)
                .ok_or(ValueBridgeStatus::Invalid)?
                .as_int();
            if choice < 0 || (choice as usize) >= t.cases.len() {
                return Err(ValueBridgeStatus::Invalid);
            }
            let list = case_field(self.protocol, self.value_ty, val, 1)
                .ok_or(ValueBridgeStatus::Invalid)?;
            (choice as u32, list)
        } else {
            if shape != 1 {
                return Err(ValueBridgeStatus::Invalid);
            }
            let list = case_field(self.protocol, self.value_ty, val, 0)
                .ok_or(ValueBridgeStatus::Invalid)?;
            (0u32, list)
        };

        let items = list_val.as_list();
        let count = items.len();
        match t.kind {
            TypeKind::Choice if count != t.cases[tag as usize].fields.len() => {
                return Err(ValueBridgeStatus::Invalid);
            }
            TypeKind::Record if count != t.fields.len() => {
                return Err(ValueBridgeStatus::Invalid);
            }
            TypeKind::Tuple if count != t.parameters.len() => {
                return Err(ValueBridgeStatus::Invalid);
            }
            TypeKind::Map if !count.is_multiple_of(2) => {
                return Err(ValueBridgeStatus::Invalid);
            }
            _ => {}
        }
        if !self.charge(count as u64) {
            return Err(ValueBridgeStatus::Limit);
        }

        let mut built = Vec::with_capacity(count);
        for (i, item) in items.iter().enumerate() {
            let child_ty = match t.kind {
                TypeKind::Choice => t.cases[tag as usize].fields[i].ty,
                TypeKind::Record => t.fields[i].ty,
                TypeKind::Tuple => t.parameters[i],
                TypeKind::Map => t.parameters[i % 2],
                _ => t.parameters[0],
            };
            built.push(self.unpack_inner(to, child_ty, item, byte_limit, depth - 1)?);
        }

        match t.kind {
            TypeKind::List => to
                .alloc_list(VecDeque::from(built))
                .map_err(|_| ValueBridgeStatus::Memory),
            TypeKind::Choice => to
                .make_case(self.service, svc_ty, tag, built)
                .map_err(|_| ValueBridgeStatus::Memory),
            TypeKind::Record => {
                let layout = self
                    .service
                    .find_record_layout(svc_ty)
                    .ok_or(ValueBridgeStatus::Invalid)?;
                to.construct(self.service, layout, built)
                    .map_err(|_| ValueBridgeStatus::Memory)
            }
            TypeKind::Tuple => {
                let layout = self
                    .service
                    .find_tuple_layout(svc_ty)
                    .ok_or(ValueBridgeStatus::Invalid)?;
                to.construct(self.service, layout, built)
                    .map_err(|_| ValueBridgeStatus::Memory)
            }
            TypeKind::Set => {
                let mut set = MoSet::new(t.parameters[0]);
                for item in built {
                    set.add(self.service, to.hash_key(), item);
                }
                if set.len() != count {
                    return Err(ValueBridgeStatus::Invalid);
                }
                to.alloc_set(set).map_err(|_| ValueBridgeStatus::Memory)
            }
            TypeKind::Map => {
                let mut map = MoMap::new(t.parameters[0], t.parameters[1]);
                let mut iter = built.into_iter();
                while let (Some(k), Some(v)) = (iter.next(), iter.next()) {
                    map.set(self.service, to.hash_key(), k, v);
                }
                if map.len() != count / 2 {
                    return Err(ValueBridgeStatus::Invalid);
                }
                to.alloc_map(map).map_err(|_| ValueBridgeStatus::Memory)
            }
            _ => Err(ValueBridgeStatus::Invalid),
        }
    }
}

fn copy_leaf(
    to: &Heap,
    kind: TypeKind,
    val: &Value,
    byte_limit: usize,
) -> Result<Value, ValueBridgeStatus> {
    match kind {
        TypeKind::Unit => Ok(Value::Unit),
        TypeKind::Bool => Ok(Value::Bool(val.as_bool())),
        TypeKind::Byte => Ok(Value::Byte(val.as_byte())),
        TypeKind::Int => Ok(Value::Int(val.as_int())),
        TypeKind::Float => Ok(Value::Float(val.as_float())),
        TypeKind::Char => Ok(Value::Char(val.as_char())),
        TypeKind::Time => Ok(Value::Time(val.as_time())),
        TypeKind::Decimal => Ok(Value::Decimal(val.as_decimal())),
        TypeKind::Text => {
            let s = val.as_text();
            if s.len() > byte_limit {
                return Err(ValueBridgeStatus::Limit);
            }
            to.alloc_text(s.to_string())
                .map_err(|_| ValueBridgeStatus::Memory)
        }
        TypeKind::BigInt => {
            let (sign, mag) = val.as_bigint();
            if mag.len() > byte_limit {
                return Err(ValueBridgeStatus::Limit);
            }
            to.alloc_bigint(sign, mag.to_vec())
                .map_err(|_| ValueBridgeStatus::Memory)
        }
        _ => Err(ValueBridgeStatus::Invalid),
    }
}

fn pack_scalar(
    protocol: &TypeTable,
    scalar_ty: u32,
    to: &Heap,
    kind: TypeKind,
    val: &Value,
    byte_limit: usize,
) -> Result<Value, ValueBridgeStatus> {
    let tag = scalar_tag(kind).ok_or(ValueBridgeStatus::Invalid)?;
    let fields = match kind {
        TypeKind::Unit => Vec::new(),
        TypeKind::Range => {
            let (s, e) = val.as_range();
            vec![Value::Int(s), Value::Int(e)]
        }
        TypeKind::Duration => vec![Value::Int(val.as_duration())],
        TypeKind::Bytes => {
            let b = val.as_bytes();
            if b.len() > byte_limit {
                return Err(ValueBridgeStatus::Limit);
            }
            let list: VecDeque<Value> = b.iter().copied().map(Value::Byte).collect();
            let list_val = to.alloc_list(list).map_err(|_| ValueBridgeStatus::Memory)?;
            vec![list_val]
        }
        _ => vec![copy_leaf(to, kind, val, byte_limit)?],
    };
    to.make_case(protocol, scalar_ty, tag, fields)
        .map_err(|_| ValueBridgeStatus::Memory)
}

fn unpack_scalar(
    protocol: &TypeTable,
    scalar_ty: u32,
    to: &Heap,
    kind: TypeKind,
    val: &Value,
    byte_limit: usize,
) -> Result<Value, ValueBridgeStatus> {
    let tag = scalar_tag(kind).ok_or(ValueBridgeStatus::Invalid)?;
    if val.as_tag() != tag {
        return Err(ValueBridgeStatus::Invalid);
    }
    match kind {
        TypeKind::Unit => Ok(Value::Unit),
        TypeKind::Range => {
            let s = case_field(protocol, scalar_ty, val, 0)
                .ok_or(ValueBridgeStatus::Invalid)?
                .as_int();
            let e = case_field(protocol, scalar_ty, val, 1)
                .ok_or(ValueBridgeStatus::Invalid)?
                .as_int();
            Ok(Value::Range(s, e))
        }
        TypeKind::Duration => {
            let d = case_field(protocol, scalar_ty, val, 0)
                .ok_or(ValueBridgeStatus::Invalid)?
                .as_int();
            Ok(Value::Duration(d))
        }
        TypeKind::Bytes => {
            let list_val =
                case_field(protocol, scalar_ty, val, 0).ok_or(ValueBridgeStatus::Invalid)?;
            let list = list_val.as_list();
            if list.len() > byte_limit {
                return Err(ValueBridgeStatus::Limit);
            }
            let bytes: Vec<u8> = list.iter().map(Value::as_byte).collect();
            to.alloc_bytes(bytes).map_err(|_| ValueBridgeStatus::Memory)
        }
        _ => {
            let inner =
                case_field(protocol, scalar_ty, val, 0).ok_or(ValueBridgeStatus::Invalid)?;
            copy_leaf(to, kind, &inner, byte_limit)
        }
    }
}
