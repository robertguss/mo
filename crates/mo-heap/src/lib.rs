#![forbid(unsafe_code)]

//! Safe Rust Perceus reference-counted heap, `Value` representation, `HeapBudget`,
//! insertion-ordered collections, exact Decimal/Float formatting, structural equality/hashing/printing.

pub mod decimal;
pub mod equal;
pub mod float;
pub mod hash;
pub mod print;
pub mod table;

use std::any::Any;
use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::rc::Rc;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{BigIntSign, Capability, CaseShape, LayoutKind, NO_INDEX};
use mo_def::limits::MO_LIMIT_COLLECTION_SIZE;

use crate::hash::inline_payload_to_value;
use crate::table::{MoMap, MoSet};

/// Approximate overhead charged per heap allocation, matching C's 64-byte header + prefix.
const OBJECT_OVERHEAD: usize = 64;
const CELL_SIZE: usize = 16;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HeapError {
    OutOfMemory,
}

#[derive(Debug)]
struct BudgetInner {
    used: Cell<usize>,
    peak: Cell<usize>,
    limit: usize,
}

#[derive(Clone, Debug)]
pub struct HeapBudget {
    inner: Rc<BudgetInner>,
}

impl HeapBudget {
    pub fn new(limit: usize) -> Self {
        Self {
            inner: Rc::new(BudgetInner {
                used: Cell::new(0),
                peak: Cell::new(0),
                limit,
            }),
        }
    }

    pub fn used(&self) -> usize {
        self.inner.used.get()
    }

    pub fn peak(&self) -> usize {
        self.inner.peak.get()
    }

    pub fn limit(&self) -> usize {
        self.inner.limit
    }

    pub fn charge(&self, bytes: usize) -> Result<(), HeapError> {
        let cur = self.inner.used.get();
        let next = cur.checked_add(bytes).ok_or(HeapError::OutOfMemory)?;
        if next > self.inner.limit {
            return Err(HeapError::OutOfMemory);
        }
        self.inner.used.set(next);
        if next > self.inner.peak.get() {
            self.inner.peak.set(next);
        }
        Ok(())
    }

    pub fn release(&self, bytes: usize) {
        let cur = self.inner.used.get();
        self.inner.used.set(cur.saturating_sub(bytes));
    }
}

#[derive(Clone)]
pub struct CapabilityObj {
    pub kind: Capability,
    pub type_idx: u32,
    pub inner: Rc<dyn Any>,
}

#[derive(Clone)]
pub enum HeapObj {
    Text(String),
    Bytes(Vec<u8>),
    BigInt {
        sign: BigIntSign,
        magnitude: Vec<u8>,
    },
    Tuple {
        layout: u32,
        fields: Vec<Value>,
    },
    Record {
        layout: u32,
        fields: Vec<Value>,
    },
    CaseBlock {
        layout: u32,
        fields: Vec<Value>,
    },
    Closure {
        layout: u32,
        function: u32,
        captures: Vec<Value>,
    },
    List(VecDeque<Value>),
    Map(MoMap),
    Set(MoSet),
    Capability(CapabilityObj),
}

pub struct HeapNode {
    immortal: Cell<bool>,
    hash_cache: Cell<Option<u64>>,
    charged_bytes: Cell<usize>,
    budget: Rc<BudgetInner>,
    obj: HeapObj,
}

// Iterative drop guard prevents stack overflow when dropping deeply nested linked lists/trees.
thread_local! {
    static DROP_QUEUE: RefCell<Option<Vec<Value>>> = const { RefCell::new(None) };
}

impl Drop for HeapNode {
    fn drop(&mut self) {
        if !self.immortal.get() {
            let b = self.charged_bytes.get();
            let cur = self.budget.used.get();
            self.budget.used.set(cur.saturating_sub(b));
        }

        let mut children = Vec::new();
        match &mut self.obj {
            HeapObj::Tuple { fields, .. }
            | HeapObj::Record { fields, .. }
            | HeapObj::CaseBlock { fields, .. } => {
                children.append(fields);
            }
            HeapObj::Closure { captures, .. } => {
                children.append(captures);
            }
            HeapObj::List(items) => {
                children.extend(items.drain(..));
            }
            HeapObj::Text(_)
            | HeapObj::Bytes(_)
            | HeapObj::BigInt { .. }
            | HeapObj::Map(_)
            | HeapObj::Set(_)
            | HeapObj::Capability(_) => return,
        }
        if children.is_empty() {
            return;
        }

        // If a drop loop is already active on this thread, append to it instead of recursing.
        let queued = DROP_QUEUE.with(|q| {
            let mut borrow = q.borrow_mut();
            if let Some(vec) = borrow.as_mut() {
                vec.append(&mut children);
                true
            } else {
                *borrow = Some(children);
                false
            }
        });
        if queued {
            return;
        }

        loop {
            let next = DROP_QUEUE.with(|q| q.borrow_mut().as_mut().and_then(Vec::pop));
            match next {
                Some(val) => drop(val),
                None => {
                    DROP_QUEUE.with(|q| *q.borrow_mut() = None);
                    break;
                }
            }
        }
    }
}

#[derive(Clone)]
pub struct HeapRef(Rc<HeapNode>);

impl HeapRef {
    pub fn obj(&self) -> &HeapObj {
        &self.0.obj
    }

    pub fn is_immortal(&self) -> bool {
        self.0.immortal.get()
    }

    pub fn can_mutate_in_place(&self) -> bool {
        !self.0.immortal.get() && Rc::strong_count(&self.0) == 1
    }

    pub fn get_mut_unique(&mut self) -> Option<&mut HeapObj> {
        if self.0.immortal.get() {
            return None;
        }
        Rc::get_mut(&mut self.0).map(|node| {
            node.hash_cache.set(None);
            &mut node.obj
        })
    }

    pub fn cached_hash(&self) -> Option<u64> {
        self.0.hash_cache.get()
    }

    pub fn set_cached_hash(&self, h: Option<u64>) {
        self.0.hash_cache.set(h);
    }

    pub fn update_charge(&self, new_bytes: usize) -> Result<(), HeapError> {
        let old = self.0.charged_bytes.get();
        if new_bytes > old {
            let diff = new_bytes - old;
            let cur = self.0.budget.used.get();
            let next = cur.checked_add(diff).ok_or(HeapError::OutOfMemory)?;
            if next > self.0.budget.limit {
                return Err(HeapError::OutOfMemory);
            }
            self.0.budget.used.set(next);
            if next > self.0.budget.peak.get() {
                self.0.budget.peak.set(next);
            }
        } else {
            let diff = old - new_bytes;
            let cur = self.0.budget.used.get();
            self.0.budget.used.set(cur.saturating_sub(diff));
        }
        self.0.charged_bytes.set(new_bytes);
        Ok(())
    }

    pub fn layout_index(&self) -> Option<u32> {
        match &self.0.obj {
            HeapObj::Tuple { layout, .. }
            | HeapObj::Record { layout, .. }
            | HeapObj::CaseBlock { layout, .. }
            | HeapObj::Closure { layout, .. } => Some(*layout),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub enum InlinePayload {
    Empty,
    Unit,
    Bool(bool),
    Byte(u8),
    Int(i64),
    Float(f64),
    Char(char),
    Time(i64),
    Duration(i64),
    Tag(u32),
}

#[derive(Clone)]
pub struct ReuseToken {
    pub layout: u32,
    pub slot: Option<HeapRef>,
}

#[derive(Clone)]
pub enum Value {
    Unit,
    Bool(bool),
    Byte(u8),
    Int(i64),
    Float(f64),
    Char(char),
    Time(i64),
    Duration(i64),
    Decimal(i128),
    Range(i64, i64),
    ChoiceInline { tag: u32, payload: InlinePayload },
    ChoiceRef { tag: u32, payload: HeapRef },
    Ref(HeapRef),
    Token(ReuseToken),
}

impl Value {
    pub fn as_bool(&self) -> bool {
        match self {
            Value::Bool(b) => *b,
            _ => panic!("expected Bool"),
        }
    }

    pub fn as_byte(&self) -> u8 {
        match self {
            Value::Byte(b) => *b,
            _ => panic!("expected Byte"),
        }
    }

    pub fn as_int(&self) -> i64 {
        match self {
            Value::Int(n) => *n,
            _ => panic!("expected Int"),
        }
    }

    pub fn as_float(&self) -> f64 {
        match self {
            Value::Float(x) => *x,
            _ => panic!("expected Float"),
        }
    }

    pub fn as_char(&self) -> char {
        match self {
            Value::Char(c) => *c,
            _ => panic!("expected Char"),
        }
    }

    pub fn as_time(&self) -> i64 {
        match self {
            Value::Time(t) => *t,
            _ => panic!("expected Time"),
        }
    }

    pub fn as_duration(&self) -> i64 {
        match self {
            Value::Duration(d) => *d,
            _ => panic!("expected Duration"),
        }
    }

    pub fn as_decimal(&self) -> i128 {
        match self {
            Value::Decimal(d) => *d,
            _ => panic!("expected Decimal"),
        }
    }

    pub fn as_range(&self) -> (i64, i64) {
        match self {
            Value::Range(s, e) => (*s, *e),
            _ => panic!("expected Range"),
        }
    }

    pub fn as_tag(&self) -> u32 {
        match self {
            Value::ChoiceInline { tag, .. } | Value::ChoiceRef { tag, .. } => *tag,
            _ => panic!("expected Choice"),
        }
    }

    pub fn as_heap_ref(&self) -> &HeapRef {
        match self {
            Value::Ref(r) => r,
            _ => panic!("expected Ref"),
        }
    }

    pub fn as_text(&self) -> &str {
        match self.as_heap_ref().obj() {
            HeapObj::Text(s) => s.as_str(),
            _ => panic!("expected Text"),
        }
    }

    pub fn as_bytes(&self) -> &[u8] {
        match self.as_heap_ref().obj() {
            HeapObj::Bytes(b) => b.as_slice(),
            _ => panic!("expected Bytes"),
        }
    }

    pub fn as_list(&self) -> &VecDeque<Value> {
        match self.as_heap_ref().obj() {
            HeapObj::List(l) => l,
            _ => panic!("expected List"),
        }
    }

    pub fn as_map(&self) -> &MoMap {
        match self.as_heap_ref().obj() {
            HeapObj::Map(m) => m,
            _ => panic!("expected Map"),
        }
    }

    pub fn as_set(&self) -> &MoSet {
        match self.as_heap_ref().obj() {
            HeapObj::Set(s) => s,
            _ => panic!("expected Set"),
        }
    }

    pub fn as_capability(&self) -> &CapabilityObj {
        match self.as_heap_ref().obj() {
            HeapObj::Capability(c) => c,
            _ => panic!("expected Capability"),
        }
    }

    pub fn to_inline_payload(&self) -> InlinePayload {
        match self {
            Value::Unit => InlinePayload::Unit,
            Value::Bool(b) => InlinePayload::Bool(*b),
            Value::Byte(b) => InlinePayload::Byte(*b),
            Value::Int(n) => InlinePayload::Int(*n),
            Value::Float(x) => InlinePayload::Float(*x),
            Value::Char(c) => InlinePayload::Char(*c),
            Value::Time(t) => InlinePayload::Time(*t),
            Value::Duration(d) => InlinePayload::Duration(*d),
            Value::ChoiceInline { tag, .. } => InlinePayload::Tag(*tag),
            _ => panic!("value cannot be stored as inline choice payload"),
        }
    }
}

pub struct Heap {
    budget: HeapBudget,
    hash_key: [u64; 2],
}

impl Heap {
    pub fn new(limit: usize) -> Self {
        Self {
            budget: HeapBudget::new(limit),
            hash_key: [0x0706_0504_0302_0100, 0x0f0e_0d0c_0b0a_0908],
        }
    }

    pub fn budget(&self) -> &HeapBudget {
        &self.budget
    }

    pub fn hash_key(&self) -> [u64; 2] {
        self.hash_key
    }

    fn alloc_node(&self, payload_bytes: usize, obj: HeapObj) -> Result<HeapRef, HeapError> {
        let total = OBJECT_OVERHEAD
            .checked_add(payload_bytes)
            .ok_or(HeapError::OutOfMemory)?;
        self.budget.charge(total)?;
        Ok(HeapRef(Rc::new(HeapNode {
            immortal: Cell::new(false),
            hash_cache: Cell::new(None),
            charged_bytes: Cell::new(total),
            budget: Rc::clone(&self.budget.inner),
            obj,
        })))
    }

    pub fn alloc_text(&self, s: String) -> Result<Value, HeapError> {
        let len = s.len();
        Ok(Value::Ref(self.alloc_node(len, HeapObj::Text(s))?))
    }

    pub fn alloc_bytes(&self, bytes: Vec<u8>) -> Result<Value, HeapError> {
        let len = bytes.len();
        Ok(Value::Ref(self.alloc_node(len, HeapObj::Bytes(bytes))?))
    }

    pub fn alloc_bigint(&self, sign: BigIntSign, magnitude: Vec<u8>) -> Result<Value, HeapError> {
        let len = magnitude.len() + 8;
        Ok(Value::Ref(
            self.alloc_node(len, HeapObj::BigInt { sign, magnitude })?,
        ))
    }

    pub fn alloc_list(&self, items: VecDeque<Value>) -> Result<Value, HeapError> {
        if items.len() as u64 > MO_LIMIT_COLLECTION_SIZE {
            return Err(HeapError::OutOfMemory);
        }
        let bytes = items.len().saturating_mul(CELL_SIZE);
        Ok(Value::Ref(self.alloc_node(bytes, HeapObj::List(items))?))
    }

    pub fn alloc_map(&self, map: MoMap) -> Result<Value, HeapError> {
        let bytes = map.len().saturating_mul(CELL_SIZE * 2);
        Ok(Value::Ref(self.alloc_node(bytes, HeapObj::Map(map))?))
    }

    pub fn alloc_set(&self, set: MoSet) -> Result<Value, HeapError> {
        let bytes = set.len().saturating_mul(CELL_SIZE);
        Ok(Value::Ref(self.alloc_node(bytes, HeapObj::Set(set))?))
    }

    pub fn alloc_capability(
        &self,
        kind: Capability,
        type_idx: u32,
        inner: Rc<dyn Any>,
    ) -> Result<Value, HeapError> {
        Ok(Value::Ref(self.alloc_node(
            64,
            HeapObj::Capability(CapabilityObj {
                kind,
                type_idx,
                inner,
            }),
        )?))
    }

    pub fn alloc_closure(
        &self,
        layout: u32,
        function: u32,
        captures: Vec<Value>,
    ) -> Result<Value, HeapError> {
        let bytes = captures.len().saturating_mul(CELL_SIZE);
        Ok(Value::Ref(self.alloc_node(
            bytes,
            HeapObj::Closure {
                layout,
                function,
                captures,
            },
        )?))
    }

    pub fn construct(
        &self,
        types: &TypeTable,
        layout_idx: u32,
        fields: Vec<Value>,
    ) -> Result<Value, HeapError> {
        let layout = types.layout(layout_idx);
        let bytes = fields.len().saturating_mul(CELL_SIZE);
        match layout.kind {
            LayoutKind::Record => Ok(Value::Ref(self.alloc_node(
                bytes,
                HeapObj::Record {
                    layout: layout_idx,
                    fields,
                },
            )?)),
            LayoutKind::Tuple => Ok(Value::Ref(self.alloc_node(
                bytes,
                HeapObj::Tuple {
                    layout: layout_idx,
                    fields,
                },
            )?)),
            LayoutKind::Case => {
                let block = self.alloc_node(
                    bytes,
                    HeapObj::CaseBlock {
                        layout: layout_idx,
                        fields,
                    },
                )?;
                Ok(Value::ChoiceRef {
                    tag: layout.case_index,
                    payload: block,
                })
            }
            LayoutKind::Closure => {
                panic!("construct does not build closures");
            }
        }
    }

    pub fn make_case(
        &self,
        types: &TypeTable,
        ty: u32,
        tag: u32,
        mut fields: Vec<Value>,
    ) -> Result<Value, HeapError> {
        match types.case_shape(ty, tag) {
            CaseShape::Empty => Ok(Value::ChoiceInline {
                tag,
                payload: InlinePayload::Empty,
            }),
            CaseShape::Inline => {
                let v = fields.pop().expect("inline case has 1 field");
                Ok(Value::ChoiceInline {
                    tag,
                    payload: v.to_inline_payload(),
                })
            }
            CaseShape::Ref => {
                let v = fields.pop().expect("ref case has 1 field");
                let r = match v {
                    Value::Ref(r) => r,
                    _ => panic!("ref case payload must be a Ref"),
                };
                Ok(Value::ChoiceRef { tag, payload: r })
            }
            CaseShape::Boxed => {
                let layout_idx = types
                    .find_case_layout(ty, tag)
                    .expect("boxed case layout must exist");
                self.construct(types, layout_idx, fields)
            }
        }
    }

    pub fn make_option(
        &self,
        types: &TypeTable,
        result_ty: u32,
        value: Option<Value>,
    ) -> Result<Value, HeapError> {
        match value {
            None => self.make_case(types, result_ty, 0, Vec::new()),
            Some(v) => self.make_case(types, result_ty, 1, vec![v]),
        }
    }

    pub fn make_result(
        &self,
        types: &TypeTable,
        result_ty: u32,
        value: Result<Value, Value>,
    ) -> Result<Value, HeapError> {
        match value {
            Ok(v) => self.make_case(types, result_ty, 0, vec![v]),
            Err(e) => self.make_case(types, result_ty, 1, vec![e]),
        }
    }

    pub fn extract_field(&self, object: &Value, field_idx: u32) -> Value {
        let idx = field_idx as usize;
        match object.as_heap_ref().obj() {
            HeapObj::Record { fields, .. }
            | HeapObj::Tuple { fields, .. }
            | HeapObj::CaseBlock { fields, .. } => fields[idx].clone(),
            HeapObj::Closure { captures, .. } => captures[idx].clone(),
            _ => panic!("FIELD on non-aggregate object"),
        }
    }

    pub fn extract_case_field(
        &self,
        types: &TypeTable,
        ty: u32,
        object: &Value,
        case_idx: u32,
        field_idx: u32,
    ) -> Value {
        match types.case_shape(ty, case_idx) {
            CaseShape::Empty => panic!("CASE_FIELD on empty case"),
            CaseShape::Inline => match object {
                Value::ChoiceInline { tag, payload } => {
                    debug_assert_eq!(*tag, case_idx);
                    let fty = types.get(ty).cases[case_idx as usize].fields[0].ty;
                    inline_payload_to_value(*payload, types.get(fty).kind)
                }
                _ => panic!("expected ChoiceInline"),
            },
            CaseShape::Ref => match object {
                Value::ChoiceRef { tag, payload } => {
                    debug_assert_eq!(*tag, case_idx);
                    Value::Ref(payload.clone())
                }
                _ => panic!("expected ChoiceRef"),
            },
            CaseShape::Boxed => match object {
                Value::ChoiceRef { tag, payload } => {
                    debug_assert_eq!(*tag, case_idx);
                    if let HeapObj::CaseBlock { fields, .. } = payload.obj() {
                        fields[field_idx as usize].clone()
                    } else {
                        panic!("expected CaseBlock payload")
                    }
                }
                _ => panic!("expected ChoiceRef"),
            },
        }
    }

    pub fn set_field_owned(
        &self,
        mut object: Value,
        field_idx: u32,
        replacement: Value,
    ) -> Result<Value, HeapError> {
        let idx = field_idx as usize;
        let Value::Ref(ref mut r) = object else {
            panic!("SET_FIELD on non-Ref");
        };
        if let Some(obj) = r.get_mut_unique() {
            match obj {
                HeapObj::Record { fields, .. } | HeapObj::Tuple { fields, .. } => {
                    fields[idx] = replacement;
                    return Ok(object);
                }
                _ => panic!("SET_FIELD on non-Record/Tuple"),
            }
        }
        // Shared or immortal: allocate a fresh copy with the updated field.
        let (is_record, layout, mut fields) = match r.obj() {
            HeapObj::Record { layout, fields } => (true, *layout, fields.clone()),
            HeapObj::Tuple { layout, fields } => (false, *layout, fields.clone()),
            _ => panic!("SET_FIELD on non-Record/Tuple"),
        };
        fields[idx] = replacement;
        let bytes = fields.len().saturating_mul(CELL_SIZE);
        let new_obj = if is_record {
            HeapObj::Record { layout, fields }
        } else {
            HeapObj::Tuple { layout, fields }
        };
        Ok(Value::Ref(self.alloc_node(bytes, new_obj)?))
    }

    pub fn reset(&self, value: Value) -> ReuseToken {
        let mut r = match value {
            Value::Ref(r) => r,
            Value::ChoiceRef { payload, .. } => payload,
            _ => panic!("RESET on non-reference"),
        };
        let layout = r.layout_index().expect("RESET requires fixed layout");
        if let Some(obj) = r.get_mut_unique() {
            match obj {
                HeapObj::Record { fields, .. }
                | HeapObj::Tuple { fields, .. }
                | HeapObj::CaseBlock { fields, .. } => {
                    fields.clear();
                }
                _ => panic!("RESET on closure or built-in"),
            }
            ReuseToken {
                layout,
                slot: Some(r),
            }
        } else {
            drop(r);
            ReuseToken { layout, slot: None }
        }
    }

    pub fn reuse_owned(
        &self,
        types: &TypeTable,
        mut token: ReuseToken,
        layout_idx: u32,
        fields: Vec<Value>,
    ) -> Result<Value, HeapError> {
        debug_assert_eq!(token.layout, layout_idx);
        let layout = types.layout(layout_idx);
        if let Some(mut slot) = token.slot.take() {
            let new_bytes = OBJECT_OVERHEAD + fields.len().saturating_mul(CELL_SIZE);
            slot.update_charge(new_bytes)?;
            let obj = slot.get_mut_unique().expect("token slot must be unique");
            match layout.kind {
                LayoutKind::Record => {
                    *obj = HeapObj::Record {
                        layout: layout_idx,
                        fields,
                    };
                    return Ok(Value::Ref(slot));
                }
                LayoutKind::Tuple => {
                    *obj = HeapObj::Tuple {
                        layout: layout_idx,
                        fields,
                    };
                    return Ok(Value::Ref(slot));
                }
                LayoutKind::Case => {
                    *obj = HeapObj::CaseBlock {
                        layout: layout_idx,
                        fields,
                    };
                    return Ok(Value::ChoiceRef {
                        tag: layout.case_index,
                        payload: slot,
                    });
                }
                LayoutKind::Closure => panic!("REUSE cannot build closure"),
            }
        }
        self.construct(types, layout_idx, fields)
    }

    pub fn free_token(&self, mut token: ReuseToken) {
        token.slot = None;
        token.layout = NO_INDEX;
    }

    /// Recursively marks a value and every heap object reachable from it as immortal.
    pub fn make_immortal(&self, value: &Value) {
        let mut stack: Vec<HeapRef> = Vec::new();
        push_if_ref(value, &mut stack);
        while let Some(r) = stack.pop() {
            if r.0.immortal.get() {
                continue;
            }
            r.0.immortal.set(true);
            match r.obj() {
                HeapObj::Tuple { fields, .. }
                | HeapObj::Record { fields, .. }
                | HeapObj::CaseBlock { fields, .. } => {
                    for f in fields {
                        push_if_ref(f, &mut stack);
                    }
                }
                HeapObj::Closure { captures, .. } => {
                    for c in captures {
                        push_if_ref(c, &mut stack);
                    }
                }
                HeapObj::List(items) => {
                    for item in items {
                        push_if_ref(item, &mut stack);
                    }
                }
                HeapObj::Map(map) => {
                    for (k, v) in map.iter() {
                        push_if_ref(k, &mut stack);
                        push_if_ref(v, &mut stack);
                    }
                }
                HeapObj::Set(set) => {
                    for item in set.iter() {
                        push_if_ref(item, &mut stack);
                    }
                }
                HeapObj::Text(_)
                | HeapObj::Bytes(_)
                | HeapObj::BigInt { .. }
                | HeapObj::Capability(_) => {}
            }
        }
    }

    pub fn text_concat(&self, mut left: Value, right: &Value) -> Result<Value, HeapError> {
        let rhs = right.as_text();
        if rhs.is_empty() {
            return Ok(left);
        }
        let Value::Ref(ref mut r) = left else {
            panic!("expected Text");
        };
        if let Some(HeapObj::Text(s)) = r.get_mut_unique() {
            let new_len = s
                .len()
                .checked_add(rhs.len())
                .ok_or(HeapError::OutOfMemory)?;
            r.update_charge(OBJECT_OVERHEAD + new_len)?;
            if let Some(HeapObj::Text(s)) = r.get_mut_unique() {
                s.push_str(rhs);
            }
            return Ok(left);
        }
        let lhs = left.as_text();
        let mut combined = String::with_capacity(lhs.len() + rhs.len());
        combined.push_str(lhs);
        combined.push_str(rhs);
        self.alloc_text(combined)
    }

    pub fn text_slice(
        &self,
        mut text: Value,
        start: usize,
        len: usize,
    ) -> Result<Value, HeapError> {
        if start == 0 && len == text.as_text().len() {
            return Ok(text);
        }
        let Value::Ref(ref mut r) = text else {
            panic!("expected Text");
        };
        if r.can_mutate_in_place() {
            r.update_charge(OBJECT_OVERHEAD + len)?;
            if let Some(HeapObj::Text(s)) = r.get_mut_unique() {
                s.truncate(start + len);
                if start != 0 {
                    s.drain(..start);
                }
            }
            return Ok(text);
        }
        let sliced = text.as_text()[start..start + len].to_string();
        self.alloc_text(sliced)
    }

    pub fn list_append(&self, mut list: Value, elem: Value) -> Result<Value, HeapError> {
        let len = list.as_list().len();
        if (len as u64) >= MO_LIMIT_COLLECTION_SIZE {
            return Err(HeapError::OutOfMemory);
        }
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if r.can_mutate_in_place() {
            r.update_charge(OBJECT_OVERHEAD + (len + 1) * CELL_SIZE)?;
            if let Some(HeapObj::List(items)) = r.get_mut_unique() {
                items.push_back(elem);
            }
            return Ok(list);
        }
        let mut items = list.as_list().clone();
        items.push_back(elem);
        self.alloc_list(items)
    }

    pub fn list_prepend(&self, mut list: Value, elem: Value) -> Result<Value, HeapError> {
        let len = list.as_list().len();
        if (len as u64) >= MO_LIMIT_COLLECTION_SIZE {
            return Err(HeapError::OutOfMemory);
        }
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if r.can_mutate_in_place() {
            r.update_charge(OBJECT_OVERHEAD + (len + 1) * CELL_SIZE)?;
            if let Some(HeapObj::List(items)) = r.get_mut_unique() {
                items.push_front(elem);
            }
            return Ok(list);
        }
        let mut items = list.as_list().clone();
        items.push_front(elem);
        self.alloc_list(items)
    }

    pub fn list_concat(&self, mut list: Value, right: &Value) -> Result<Value, HeapError> {
        let rhs = right.as_list();
        if rhs.is_empty() {
            return Ok(list);
        }
        let new_len = list.as_list().len().saturating_add(rhs.len());
        if (new_len as u64) > MO_LIMIT_COLLECTION_SIZE {
            return Err(HeapError::OutOfMemory);
        }
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if r.can_mutate_in_place() {
            // Collect clones first in case left and right alias (verified bytecode forbids alias on owned+borrowed,
            // but cloning avoids borrow conflict cleanly).
            let rhs_items: Vec<Value> = rhs.iter().cloned().collect();
            r.update_charge(OBJECT_OVERHEAD + new_len * CELL_SIZE)?;
            if let Some(HeapObj::List(items)) = r.get_mut_unique() {
                items.extend(rhs_items);
            }
            return Ok(list);
        }
        let mut items = VecDeque::with_capacity(new_len);
        items.extend(list.as_list().iter().cloned());
        items.extend(rhs.iter().cloned());
        self.alloc_list(items)
    }

    pub fn list_slice(
        &self,
        mut list: Value,
        start: usize,
        count: usize,
    ) -> Result<Value, HeapError> {
        let old_len = list.as_list().len();
        if start == 0 && count == old_len {
            return Ok(list);
        }
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if r.can_mutate_in_place() {
            if let Some(HeapObj::List(items)) = r.get_mut_unique() {
                items.truncate(start + count);
                if start != 0 {
                    items.drain(..start);
                }
            }
            r.update_charge(OBJECT_OVERHEAD + count * CELL_SIZE)?;
            return Ok(list);
        }
        let items: VecDeque<Value> = list
            .as_list()
            .iter()
            .skip(start)
            .take(count)
            .cloned()
            .collect();
        self.alloc_list(items)
    }

    pub fn list_reverse(&self, mut list: Value) -> Result<Value, HeapError> {
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if let Some(HeapObj::List(items)) = r.get_mut_unique() {
            let slice = items.make_contiguous();
            slice.reverse();
            return Ok(list);
        }
        let items: VecDeque<Value> = list.as_list().iter().rev().cloned().collect();
        self.alloc_list(items)
    }

    pub fn list_set(&self, mut list: Value, index: usize, elem: Value) -> Result<Value, HeapError> {
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if let Some(HeapObj::List(items)) = r.get_mut_unique() {
            items[index] = elem;
            return Ok(list);
        }
        let mut items = list.as_list().clone();
        items[index] = elem;
        self.alloc_list(items)
    }

    pub fn list_remove_at(&self, mut list: Value, index: usize) -> Result<Value, HeapError> {
        let old_len = list.as_list().len();
        let Value::Ref(ref mut r) = list else {
            panic!("expected List");
        };
        if r.can_mutate_in_place() {
            if let Some(HeapObj::List(items)) = r.get_mut_unique() {
                items.remove(index);
            }
            r.update_charge(OBJECT_OVERHEAD + (old_len - 1) * CELL_SIZE)?;
            return Ok(list);
        }
        let mut items = list.as_list().clone();
        items.remove(index);
        self.alloc_list(items)
    }
}

fn push_if_ref(value: &Value, stack: &mut Vec<HeapRef>) {
    match value {
        Value::Ref(r) => stack.push(r.clone()),
        Value::ChoiceRef { payload, .. } => stack.push(payload.clone()),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::decimal::{DECIMAL_SCALE, decimal_div, decimal_mul};
    use crate::float::format_float;
    use crate::print::{format_decimal, format_duration, format_time};

    #[test]
    fn budget_enforces_limit_and_reclaims_on_drop() {
        let heap = Heap::new(200);
        {
            let _a = heap.alloc_text("hello world".to_string()).unwrap();
            assert!(heap.budget().used() >= 64 + 11);
            assert!(heap.alloc_bytes(vec![0u8; 200]).is_err());
        }
        assert_eq!(heap.budget().used(), 0);
    }

    #[test]
    fn immortal_objects_are_not_mutated_in_place() {
        let heap = Heap::new(1024 * 1024);
        let s = heap.alloc_text("abc".to_string()).unwrap();
        heap.make_immortal(&s);
        assert!(s.as_heap_ref().is_immortal());
        assert!(!s.as_heap_ref().can_mutate_in_place());
        let rhs = heap.alloc_text("def".to_string()).unwrap();
        let combined = heap.text_concat(s.clone(), &rhs).unwrap();
        assert_eq!(s.as_text(), "abc");
        assert_eq!(combined.as_text(), "abcdef");
    }

    #[test]
    fn float_formatting_matches_spec() {
        let cases = [
            (0.0, "0"),
            (-0.0, "-0"),
            (f64::NAN, "nan"),
            (f64::INFINITY, "inf"),
            (f64::NEG_INFINITY, "-inf"),
            (1.5, "1.5"),
            (0.1, "0.1"),
            (0.1 + 0.2, "0.30000000000000004"),
            (1e-6, "0.000001"),
            (1e-7, "1e-7"),
            (1e-5, "0.00001"),
            (1e20, "100000000000000000000"),
            (1e21, "1e21"),
        ];
        for (val, expected) in cases {
            let mut out = String::new();
            format_float(val, &mut out);
            assert_eq!(out, expected, "for {val:?}");
        }
    }

    #[test]
    fn decimal_mul_div_ties_to_even() {
        let half = DECIMAL_SCALE / 2;
        let one_and_half = DECIMAL_SCALE + half;
        let prod = decimal_mul(half, one_and_half).unwrap();
        let mut s = String::new();
        format_decimal(&mut s, prod);
        assert_eq!(s, "0.75");

        let one = DECIMAL_SCALE;
        let three = 3 * DECIMAL_SCALE;
        let third = decimal_div(one, three).unwrap();
        let mut s2 = String::new();
        format_decimal(&mut s2, third);
        assert_eq!(s2, "0.333333333333333333");
    }

    #[test]
    fn time_and_duration_formatting() {
        let mut t = String::new();
        format_time(&mut t, 0);
        assert_eq!(t, "1970-01-01T00:00:00Z");

        let mut d = String::new();
        format_duration(&mut d, 1_500_000);
        assert_eq!(d, "1.5s");
    }
}
