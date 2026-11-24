//! Deterministic counterexample shrinking and extreme-boundary detection (`src/tools/inputs-shrink.c`).

use mo_bytecode::{CaseShape, TypeKind};
use mo_def::limits::{MO_TIME_MAX_US, MO_TIME_MIN_US};
use mo_heap::hash::inline_payload_to_value;
use mo_heap::{Heap, HeapObj, Value};

use super::build::{Builder, ValueRequest, build_inputs};
use super::scalars::{bigint_smaller, immediate_build};
use super::{BuildMode, EditKind, InputBudget, InputEdit, InputFactory, InputResult, Inputs};

/// Which end of a scalar domain an extreme boundary hit (`enum mo_input_end`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputExtremeEnd {
    Smallest,
    Largest,
}

/// Extreme scalar boundary detected inside a counterexample (`struct mo_input_extreme`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InputExtreme {
    pub parameter: usize,
    pub ty: TypeKind,
    pub end: InputExtremeEnd,
}

pub(crate) struct WalkFrame {
    pub value: Value,
    pub ty: u32,
    pub count: u32,
    pub index: u32,
    pub items: Vec<(u32, Value)>,
    pub emitted: bool,
}

pub(crate) fn walk_frame_init(
    inputs: &Inputs<'_>,
    ty: u32,
    value: &Value,
    budget: &mut InputBudget,
) -> Result<WalkFrame, InputResult> {
    let mut frame = WalkFrame {
        value: value.clone(),
        ty,
        count: 0,
        index: 0,
        items: Vec::new(),
        emitted: false,
    };
    if inputs.info[ty as usize].factory_index.is_some() {
        return Ok(frame);
    }
    let type_def = inputs.types.get(ty);
    match type_def.kind {
        TypeKind::Record => {
            frame.count = type_def.fields.len() as u32;
            if let HeapObj::Record { fields, .. } = value.as_heap_ref().obj() {
                for (f, v) in type_def.fields.iter().zip(fields) {
                    frame.items.push((f.ty, v.clone()));
                }
            }
        }
        TypeKind::Tuple => {
            frame.count = type_def.parameters.len() as u32;
            if let HeapObj::Tuple { fields, .. } = value.as_heap_ref().obj() {
                for (&p, v) in type_def.parameters.iter().zip(fields) {
                    frame.items.push((p, v.clone()));
                }
            }
        }
        TypeKind::Choice => {
            let tag = value.as_tag();
            let case = &type_def.cases[tag as usize];
            frame.count = case.fields.len() as u32;
            match inputs.types.case_shape(ty, tag) {
                CaseShape::Empty => {}
                CaseShape::Inline => {
                    if let Value::ChoiceInline { payload, .. } = value {
                        let fty = case.fields[0].ty;
                        let val = inline_payload_to_value(*payload, inputs.types.get(fty).kind);
                        frame.items.push((fty, val));
                    }
                }
                CaseShape::Ref => {
                    if let Value::ChoiceRef { payload, .. } = value {
                        frame
                            .items
                            .push((case.fields[0].ty, Value::Ref(payload.clone())));
                    }
                }
                CaseShape::Boxed => {
                    if let Value::ChoiceRef { payload, .. } = value
                        && let HeapObj::CaseBlock { fields, .. } = payload.obj()
                    {
                        for (f, v) in case.fields.iter().zip(fields) {
                            frame.items.push((f.ty, v.clone()));
                        }
                    }
                }
            }
        }
        TypeKind::List => {
            let list = value.as_list();
            let len = list.len() as u32;
            if len > budget.left.elements {
                return Err(InputResult::Limit);
            }
            frame.count = len;
            let elem_ty = type_def.parameters[0];
            for item in list {
                frame.items.push((elem_ty, item.clone()));
            }
        }
        TypeKind::Set => {
            let set = value.as_set();
            let len = set.len() as u32;
            if len > budget.left.elements {
                return Err(InputResult::Limit);
            }
            frame.count = len;
            let elem_ty = type_def.parameters[0];
            for item in set.iter() {
                frame.items.push((elem_ty, item.clone()));
            }
        }
        TypeKind::Map => {
            let map = value.as_map();
            let len = map.len() as u32;
            if len > budget.left.elements {
                return Err(InputResult::Limit);
            }
            frame.count = len * 2;
            let kty = type_def.parameters[0];
            let vty = type_def.parameters[1];
            for (k, v) in map.iter() {
                frame.items.push((kty, k.clone()));
                frame.items.push((vty, v.clone()));
            }
        }
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
        TypeKind::Function | TypeKind::Env | TypeKind::Capability => {
            return Err(InputResult::Unsupported);
        }
    }
    Ok(frame)
}

pub(crate) fn walk_child(_inputs: &Inputs<'_>, frame: &mut WalkFrame) -> (u32, Value) {
    let idx = frame.index as usize;
    frame.index += 1;
    frame.items[idx].clone()
}

struct InputWalk<'a> {
    inputs: &'a Inputs<'a>,
    roots: &'a [Value],
    budget: InputBudget,
    stack: Vec<WalkFrame>,
    parameter: usize,
    end: usize,
}

impl<'a> InputWalk<'a> {
    fn new(inputs: &'a Inputs<'a>, roots: &'a [Value], parameter: usize, end: usize) -> Self {
        Self {
            inputs,
            roots,
            budget: InputBudget {
                left: inputs.limits,
            },
            stack: Vec::new(),
            parameter,
            end,
        }
    }

    fn next(&mut self) -> Result<(u32, Value), InputResult> {
        while self.parameter < self.end {
            let (ty, val) = if let Some(frame) = self.stack.last_mut() {
                if !frame.emitted {
                    frame.emitted = true;
                    return Ok((frame.ty, frame.value.clone()));
                }
                if frame.index == frame.count {
                    self.stack.pop();
                    if self.stack.is_empty() {
                        self.parameter += 1;
                    }
                    continue;
                }
                walk_child(self.inputs, frame)
            } else {
                (
                    self.inputs.parameters[self.parameter],
                    self.roots[self.parameter].clone(),
                )
            };
            if self.stack.len() as u32 == self.inputs.limits.depth
                || self.budget.left.nodes == 0
                || !self.budget.work(1)
            {
                return Err(InputResult::Limit);
            }
            self.budget.left.nodes -= 1;
            let frame = walk_frame_init(self.inputs, ty, &val, &mut self.budget)?;
            self.stack.push(frame);
            let kind = self.inputs.types.get(ty).kind;
            if self.inputs.info[ty as usize].factory_index.is_none()
                && matches!(kind, TypeKind::Text | TypeKind::Bytes | TypeKind::BigInt)
            {
                let length = match val.as_heap_ref().obj() {
                    HeapObj::Text(s) => s.len(),
                    HeapObj::Bytes(b) => b.len(),
                    HeapObj::BigInt { magnitude, .. } => magnitude.len(),
                    _ => 0,
                };
                if !self.budget.bytes(length) {
                    return Err(InputResult::Limit);
                }
            }
        }
        Err(InputResult::Done)
    }
}

fn erase(length: u32, mut variant: u32, edit: &mut InputEdit) -> bool {
    if length == 0 {
        return false;
    }
    if variant == 0 {
        edit.start = 0;
        edit.count = length;
        return true;
    }
    variant -= 1;
    let mut chunk = if length == 1 { 0 } else { length.div_ceil(2) };
    while chunk != 0 {
        let pieces = 1 + (length - 1) / chunk;
        if variant < pieces {
            edit.start = variant * chunk;
            edit.count = (length - edit.start).min(chunk);
            return true;
        }
        variant -= pieces;
        chunk = if chunk == 1 { 0 } else { chunk.div_ceil(2) };
    }
    false
}

fn candidate(
    walk: &mut InputWalk<'_>,
    ty: u32,
    value: &Value,
    variant: u32,
    edit: &mut InputEdit,
) -> Result<(), InputResult> {
    let info = &walk.inputs.info[ty as usize];
    if info.factory_index.is_some() {
        if variant == u32::MAX {
            return Err(InputResult::Done);
        }
        edit.kind = EditKind::Factory;
        return Ok(());
    }
    let kind = walk.inputs.types.get(ty).kind;
    let mut count = 3u32;
    let mut length = 0u32;
    match kind {
        TypeKind::Unit
        | TypeKind::Record
        | TypeKind::Tuple
        | TypeKind::Function
        | TypeKind::Env
        | TypeKind::Capability => return Err(InputResult::Done),
        TypeKind::Bool => count = 1,
        TypeKind::Byte => count = 9,
        TypeKind::Int | TypeKind::Time | TypeKind::Duration => count = 65,
        TypeKind::Float => count = 75,
        TypeKind::Char => count = 22,
        TypeKind::Decimal => count = 129,
        TypeKind::Range => count = 130,
        TypeKind::BigInt => {
            let data = match value.as_heap_ref().obj() {
                HeapObj::BigInt { magnitude, .. } => magnitude.as_slice(),
                _ => &[],
            };
            if data.is_empty() {
                return Err(InputResult::Done);
            }
            let total = 2 + (data.len() as u32) * 8;
            while edit.variant < total {
                if !walk.budget.work(data.len() as u64) {
                    return Err(InputResult::Limit);
                }
                if bigint_smaller(data, edit.variant, None) {
                    edit.kind = EditKind::Scalar;
                    return Ok(());
                }
                edit.variant += 1;
            }
            return Err(InputResult::Done);
        }
        TypeKind::Text => {
            let bytes = value.as_text().as_bytes();
            if !walk.budget.work(bytes.len() as u64) {
                return Err(InputResult::Limit);
            }
            for &b in bytes {
                if (b & 0xc0) != 0x80 {
                    length += 1;
                }
            }
        }
        TypeKind::Bytes => {
            length = value.as_bytes().len() as u32;
        }
        TypeKind::List => {
            length = value.as_list().len() as u32;
        }
        TypeKind::Set => {
            length = value.as_set().len() as u32;
        }
        TypeKind::Map => {
            length = value.as_map().len() as u32;
        }
        TypeKind::Choice => {
            if variant != 0 || value.as_tag() == info.base_case {
                return Err(InputResult::Done);
            }
            edit.kind = EditKind::Base;
            return Ok(());
        }
    }
    if matches!(
        kind,
        TypeKind::Text | TypeKind::Bytes | TypeKind::List | TypeKind::Set | TypeKind::Map
    ) {
        edit.kind = EditKind::Erase;
        return if erase(length, variant, edit) {
            Ok(())
        } else {
            Err(InputResult::Done)
        };
    }
    if variant >= count {
        return Err(InputResult::Done);
    }
    edit.kind = EditKind::Scalar;
    let mut builder = Builder {
        inputs: walk.inputs,
        budget: InputBudget {
            left: walk.inputs.limits,
        },
        random: 0,
        seen: 0,
        size: 0,
        edited: false,
    };
    while edit.variant < count {
        if !walk.budget.work(1) {
            return Err(InputResult::Limit);
        }
        let req = ValueRequest {
            ty,
            mode: BuildMode::Copy,
            boundary: 0,
            size: 0,
            remaining_depth: 0,
            source: value.clone(),
            edit: Some(*edit),
        };
        match immediate_build(&mut builder, &req) {
            Ok(_) => return Ok(()),
            Err(InputResult::Done) => edit.variant += 1,
            Err(err) => return Err(err),
        }
    }
    Err(InputResult::Done)
}

/// Stateful deterministic shrinker (`struct mo_input_shrinker`).
#[derive(Debug, Clone)]
pub struct InputShrinker {
    budget: u32,
    calls: u32,
    node: u64,
    variant: u32,
    changed: bool,
    awaiting: bool,
    numeric: bool,
}

impl InputShrinker {
    /// Creates a shrinker bounded by `budget` candidate evaluations (`mo_input_shrink_init`).
    #[must_use]
    pub fn new(budget: u32) -> Self {
        Self {
            budget,
            calls: 0,
            node: 0,
            variant: 0,
            changed: false,
            awaiting: false,
            numeric: false,
        }
    }

    /// Returns the next smaller candidate derived from `best` (`mo_input_shrink_next`).
    pub fn next<'f>(
        &mut self,
        inputs: &Inputs<'_>,
        best: &[Value],
        destination: &Heap,
        factories: &mut [&mut (dyn InputFactory + 'f)],
    ) -> Result<Vec<Value>, InputResult> {
        debug_assert!(!self.awaiting);
        if self.calls == self.budget {
            return Err(InputResult::Done);
        }
        let mut cursor = self.clone();
        let mut walk = InputWalk::new(inputs, best, 0, inputs.parameters.len());
        let mut node = 0u64;
        loop {
            let (ty, val) = match walk.next() {
                Ok(pair) => pair,
                Err(InputResult::Done) if cursor.changed => {
                    let work = walk.budget.left.work;
                    walk = InputWalk::new(inputs, best, 0, inputs.parameters.len());
                    walk.budget.left.work = work;
                    cursor.node = 0;
                    node = 0;
                    cursor.variant = 0;
                    cursor.changed = false;
                    continue;
                }
                Err(InputResult::Done) => {
                    *self = cursor;
                    return Err(InputResult::Done);
                }
                Err(err) => return Err(err),
            };
            let this_node = node;
            node += 1;
            if this_node < cursor.node {
                continue;
            }
            loop {
                if !walk.budget.work(1) {
                    return Err(InputResult::Limit);
                }
                let mut edit = InputEdit {
                    node: cursor.node,
                    kind: EditKind::Scalar,
                    variant: cursor.variant,
                    start: 0,
                    count: 0,
                };
                match candidate(&mut walk, ty, &val, cursor.variant, &mut edit) {
                    Ok(()) => {}
                    Err(InputResult::Done) => {
                        cursor.node += 1;
                        cursor.variant = 0;
                        break;
                    }
                    Err(err) => return Err(err),
                }
                cursor.variant = edit.variant;
                let mut left = inputs.limits;
                left.work = walk.budget.left.work;
                match build_inputs(
                    inputs,
                    destination,
                    Some(best),
                    None,
                    0,
                    0,
                    Some(edit),
                    &mut left,
                    factories,
                ) {
                    Ok(out) => {
                        walk.budget.left.work = left.work;
                        cursor.calls += 1;
                        cursor.awaiting = true;
                        cursor.numeric = edit.kind == EditKind::Scalar;
                        *self = cursor;
                        return Ok(out);
                    }
                    Err(InputResult::Done) => {
                        walk.budget.left.work = left.work;
                        if edit.kind == EditKind::Factory {
                            cursor.node += 1;
                            cursor.variant = 0;
                            break;
                        }
                        cursor.variant += 1;
                        continue;
                    }
                    Err(err) => return Err(err),
                }
            }
        }
    }

    /// Reports whether the last candidate still reproduced the failure (`mo_input_shrink_feedback`).
    pub fn feedback(&mut self, still_fails: bool) {
        debug_assert!(self.awaiting);
        self.awaiting = false;
        if still_fails {
            self.changed = true;
            if !self.numeric {
                self.variant = 0;
            }
        } else {
            self.variant += 1;
        }
    }
}

pub(crate) fn find_extreme(
    inputs: &Inputs<'_>,
    values: &[Value],
    parameter: usize,
) -> Result<InputExtreme, InputResult> {
    let mut walk = InputWalk::new(inputs, values, parameter, parameter + 1);
    loop {
        let (ty, value) = walk.next()?;
        if inputs.info[ty as usize].factory_index.is_some() {
            continue;
        }
        let mut kind = inputs.types.get(ty).kind;
        let end = match kind {
            TypeKind::Int | TypeKind::Duration | TypeKind::Range => {
                let number = match kind {
                    TypeKind::Range => {
                        let (s, e) = value.as_range();
                        kind = TypeKind::Int;
                        if s == i64::MIN || s == i64::MAX { s } else { e }
                    }
                    TypeKind::Duration => value.as_duration(),
                    _ => value.as_int(),
                };
                if number != i64::MIN && number != i64::MAX {
                    continue;
                }
                if number == i64::MIN {
                    InputExtremeEnd::Smallest
                } else {
                    InputExtremeEnd::Largest
                }
            }
            TypeKind::Time => {
                let number = value.as_time();
                if number != MO_TIME_MIN_US && number != MO_TIME_MAX_US {
                    continue;
                }
                if number < 0 {
                    InputExtremeEnd::Smallest
                } else {
                    InputExtremeEnd::Largest
                }
            }
            TypeKind::Decimal => {
                let max = i128::MAX;
                let number = value.as_decimal();
                if number != -max && number != max {
                    continue;
                }
                if number < 0 {
                    InputExtremeEnd::Smallest
                } else {
                    InputExtremeEnd::Largest
                }
            }
            _ => continue,
        };
        return Ok(InputExtreme {
            parameter,
            ty: kind,
            end,
        });
    }
}
