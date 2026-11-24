//! Iterative value tree builder, copier, and editor (`src/tools/inputs-build.c`).

use std::collections::VecDeque;

use mo_bytecode::{NO_INDEX, TypeKind};
use mo_def::limits::MO_LIMIT_HEAP_PROBES;
use mo_heap::table::{MoMap, MoSet};
use mo_heap::{Heap, Value};

use super::prepare::{scalar_count, skip_base, variants};
use super::scalars::scalar_build;
use super::shrink::{WalkFrame, walk_child, walk_frame_init};
use super::{
    BuildMode, EditKind, InputBudget, InputEdit, InputFactory, InputLimits, InputResult, Inputs,
    input_random,
};

pub(crate) struct ValueRequest {
    pub ty: u32,
    pub mode: BuildMode,
    pub boundary: u32,
    pub size: u32,
    pub remaining_depth: u32,
    pub source: Value,
    pub edit: Option<InputEdit>,
}

pub(crate) struct Builder<'a> {
    pub inputs: &'a Inputs<'a>,
    pub budget: InputBudget,
    pub random: u64,
    pub seen: u64,
    pub size: u32,
    pub edited: bool,
}

struct BuildFrame {
    request: ValueRequest,
    source: Option<WalkFrame>,
    cells: Vec<Value>,
    count: u32,
    cursor: u32,
    filled: u32,
    tag: u32,
    special: u32,
    variant: u32,
    nodes_before: u32,
    bytes_before: usize,
    leaf: bool,
}

fn field_type(inputs: &Inputs<'_>, frame: &BuildFrame, index: u32) -> u32 {
    let type_def = inputs.types.get(frame.request.ty);
    match type_def.kind {
        TypeKind::Choice => type_def.cases[frame.tag as usize].fields[index as usize].ty,
        TypeKind::Map => type_def.parameters[(index % 2) as usize],
        TypeKind::List | TypeKind::Set => type_def.parameters[0],
        _ => inputs.info[frame.request.ty as usize].children[index as usize],
    }
}

fn slot(inputs: &Inputs<'_>, frame: &BuildFrame, child: u32) -> usize {
    if inputs.types.get(frame.request.ty).kind == TypeKind::Map {
        if child.is_multiple_of(2) {
            (child / 2) as usize
        } else {
            (frame.count / 2 + child / 2) as usize
        }
    } else {
        child as usize
    }
}

fn special_field(
    builder: &mut Builder<'_>,
    frame: &mut BuildFrame,
    mut index: u32,
) -> Result<(), InputResult> {
    let info = &builder.inputs.info[frame.request.ty as usize];
    for i in 0..info.children.len() {
        if !builder.budget.work(1) {
            return Err(InputResult::Limit);
        }
        let count = variants(builder.inputs, frame.request.ty, i as u32);
        if index < count {
            frame.special = i as u32;
            frame.variant = index + skip_base(builder.inputs, info.children[i]);
            return Ok(());
        }
        index -= count;
    }
    unreachable!("special_field index out of range");
}

fn frame_init(
    builder: &mut Builder<'_>,
    frame: &mut BuildFrame,
    depth: u32,
    edit: Option<InputEdit>,
) -> Result<(), InputResult> {
    let inputs = builder.inputs;
    let type_def = inputs.types.get(frame.request.ty);
    let info = &inputs.info[frame.request.ty as usize];
    if builder.budget.left.nodes == 0 || !builder.budget.work(1) {
        return Err(InputResult::Limit);
    }
    builder.budget.left.nodes -= 1;
    frame.nodes_before = builder.budget.left.nodes;
    frame.bytes_before = builder.budget.left.bytes;
    let node = builder.seen;
    builder.seen += 1;
    frame.special = NO_INDEX;
    if frame.request.mode == BuildMode::Copy
        && !builder.edited
        && let Some(ed) = edit
        && ed.node == node
    {
        frame.request.edit = Some(ed);
        builder.edited = true;
        if ed.kind == EditKind::Base {
            frame.request.mode = BuildMode::Minimum;
        }
    }
    frame.leaf = info.factory_index.is_some() || scalar_count(type_def.kind) != 0;
    frame.request.remaining_depth = builder.budget.left.depth - depth + 1;
    if frame.leaf {
        return Ok(());
    }
    if frame.request.mode == BuildMode::Copy {
        let wf = walk_frame_init(
            inputs,
            frame.request.ty,
            &frame.request.source,
            &mut builder.budget,
        )?;
        frame.count = wf.count;
        frame.source = Some(wf);
        frame.tag = if type_def.kind == TypeKind::Choice {
            frame.request.source.as_tag()
        } else {
            0
        };
        if let Some(ed) = frame.request.edit
            && ed.kind == EditKind::Erase
        {
            let mut erased = ed.count;
            if type_def.kind == TypeKind::Map {
                erased *= 2;
            }
            debug_assert!(erased <= frame.count);
            frame.count -= erased;
        }
    } else if type_def.kind == TypeKind::Choice {
        frame.tag = info.base_case;
        if frame.request.mode == BuildMode::Sample {
            if frame.request.boundary < info.case_count {
                frame.tag = info.case_tags[frame.request.boundary as usize];
            } else {
                special_field(builder, frame, frame.request.boundary - info.case_count)?;
                let mut low = 0usize;
                let mut high = type_def.cases.len();
                while low < high {
                    let mid = low + (high - low) / 2;
                    if info.case_offsets[mid] <= frame.special {
                        low = mid + 1;
                    } else {
                        high = mid;
                    }
                }
                debug_assert!(low != 0);
                frame.tag = (low - 1) as u32;
                frame.special -= info.case_offsets[frame.tag as usize];
            }
        } else if frame.request.mode == BuildMode::Prng {
            let start = (input_random(&mut builder.random) % u64::from(info.case_count)) as u32;
            let mut found = false;
            for i in 0..info.case_count {
                let tag = info.case_tags[((start + i) % info.case_count) as usize];
                if !builder.budget.work(1) {
                    return Err(InputResult::Limit);
                }
                if info.case_depths[tag as usize] <= frame.request.remaining_depth {
                    frame.tag = tag;
                    found = true;
                    break;
                }
            }
            if !found {
                return Err(InputResult::Limit);
            }
        }
        frame.count = type_def.cases[frame.tag as usize].fields.len() as u32;
    } else if matches!(type_def.kind, TypeKind::Record | TypeKind::Tuple) {
        frame.count = info.children.len() as u32;
        if frame.request.mode == BuildMode::Sample && frame.request.boundary != 0 {
            special_field(builder, frame, frame.request.boundary - 1)?;
        }
    } else {
        let mut count = 0u32;
        if frame.request.mode == BuildMode::Sample {
            count = if frame.request.boundary == 0 {
                0
            } else if frame.request.boundary == 2 {
                3
            } else {
                1
            };
            if frame.request.boundary >= 3 {
                special_field(builder, frame, frame.request.boundary - 3)?;
            }
        } else if frame.request.mode == BuildMode::Prng {
            let mut maximum = frame.request.size;
            let pcount = type_def.parameters.len() as u32;
            maximum = maximum.min(builder.budget.left.nodes / pcount);
            for &p in &type_def.parameters {
                if info.factory_index.is_none()
                    && inputs.info[p as usize].depth > builder.budget.left.depth - depth
                {
                    maximum = 0;
                }
            }
            count = (input_random(&mut builder.random) % (u64::from(maximum) + 1)) as u32;
        }
        if count > inputs.limits.elements {
            return Err(InputResult::Limit);
        }
        frame.count = if type_def.kind == TypeKind::Map {
            count * 2
        } else {
            count
        };
    }
    if frame.count > builder.budget.left.nodes || !builder.budget.work(u64::from(frame.count)) {
        return Err(InputResult::Limit);
    }
    if frame.count != 0 {
        frame.cells = vec![Value::Unit; frame.count as usize];
    }
    Ok(())
}

fn next_child(builder: &Builder<'_>, frame: &mut BuildFrame) -> Option<ValueRequest> {
    let type_def = builder.inputs.types.get(frame.request.ty);
    if frame.request.mode == BuildMode::Copy {
        let wf = frame.source.as_mut()?;
        if wf.index == wf.count {
            return None;
        }
        let index = wf.index;
        let (child_ty, child_val) = walk_child(builder.inputs, wf);
        let mut ty = child_ty;
        if let Some(ed) = frame.request.edit
            && ed.kind == EditKind::Erase
        {
            let entry = if type_def.kind == TypeKind::Map {
                index / 2
            } else {
                index
            };
            if entry >= ed.start && entry - ed.start < ed.count {
                ty = NO_INDEX;
            }
        }
        return Some(ValueRequest {
            ty,
            mode: BuildMode::Copy,
            boundary: 0,
            size: 0,
            remaining_depth: 0,
            source: child_val,
            edit: None,
        });
    }
    if frame.cursor == frame.count {
        return None;
    }
    let index = frame.cursor;
    frame.cursor += 1;
    let ty = field_type(builder.inputs, frame, index);
    let mut mode = if frame.request.mode == BuildMode::Prng {
        BuildMode::Prng
    } else {
        BuildMode::Minimum
    };
    let size = frame.request.size / (frame.count + 1);
    let parameter = match type_def.kind {
        TypeKind::Map => index % 2,
        TypeKind::List | TypeKind::Set => 0,
        _ => index,
    };
    let mut boundary = 0u32;
    if parameter == frame.special {
        mode = BuildMode::Sample;
        boundary = frame.variant;
    } else if frame.request.mode == BuildMode::Sample
        && frame.request.boundary == 2
        && matches!(
            type_def.kind,
            TypeKind::List | TypeKind::Set | TypeKind::Map
        )
    {
        let child = &builder.inputs.info[ty as usize];
        let entry = if type_def.kind == TypeKind::Map {
            index / 2
        } else {
            index
        };
        mode = BuildMode::Sample;
        if child.order >= builder.inputs.info[frame.request.ty as usize].order {
            mode = if entry % 2 == 0 {
                BuildMode::Minimum
            } else {
                BuildMode::Sample
            };
            boundary = skip_base(builder.inputs, ty);
        } else {
            boundary = entry % child.count;
        }
    }
    Some(ValueRequest {
        ty,
        mode,
        boundary,
        size,
        remaining_depth: 0,
        source: Value::Unit,
        edit: None,
    })
}

fn finish<'f>(
    builder: &mut Builder<'_>,
    heap: &Heap,
    frame: &mut BuildFrame,
    factories: &mut [&mut (dyn InputFactory + 'f)],
) -> Result<Value, InputResult> {
    if frame.leaf {
        return scalar_build(builder, heap, &frame.request, factories);
    }
    debug_assert_eq!(frame.filled, frame.count);
    let types = builder.inputs.types;
    let type_def = types.get(frame.request.ty);
    let cells = std::mem::take(&mut frame.cells);
    match type_def.kind {
        TypeKind::Map => {
            let cost = u64::from(frame.nodes_before - builder.budget.left.nodes)
                + (frame.bytes_before - builder.budget.left.bytes) as u64;
            if !builder.budget.work(cost * (2 * MO_LIMIT_HEAP_PROBES + 1)) {
                return Err(InputResult::Limit);
            }
            let half = (frame.count / 2) as usize;
            let mut map = MoMap::new(type_def.parameters[0], type_def.parameters[1]);
            let mut iter = cells.into_iter();
            let keys: Vec<Value> = iter.by_ref().take(half).collect();
            let vals: Vec<Value> = iter.collect();
            for (k, v) in keys.into_iter().zip(vals) {
                map.set(types, heap.hash_key(), k, v);
            }
            heap.alloc_map(map).map_err(|_| InputResult::NoMemory)
        }
        TypeKind::Set => {
            let cost = u64::from(frame.nodes_before - builder.budget.left.nodes)
                + (frame.bytes_before - builder.budget.left.bytes) as u64;
            if !builder.budget.work(cost * (2 * MO_LIMIT_HEAP_PROBES + 1)) {
                return Err(InputResult::Limit);
            }
            let mut set = MoSet::new(type_def.parameters[0]);
            for elem in cells {
                set.add(types, heap.hash_key(), elem);
            }
            heap.alloc_set(set).map_err(|_| InputResult::NoMemory)
        }
        TypeKind::List => {
            let list: VecDeque<Value> = cells.into_iter().collect();
            heap.alloc_list(list).map_err(|_| InputResult::NoMemory)
        }
        TypeKind::Choice => heap
            .make_case(types, frame.request.ty, frame.tag, cells)
            .map_err(|_| InputResult::NoMemory),
        TypeKind::Record | TypeKind::Tuple => {
            let layout = builder.inputs.info[frame.request.ty as usize].layout;
            heap.construct(types, layout, cells)
                .map_err(|_| InputResult::NoMemory)
        }
        _ => unreachable!(),
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_input_build signature directly"
)]
pub(crate) fn build_inputs<'f>(
    inputs: &Inputs<'_>,
    destination: &Heap,
    source: Option<&[Value]>,
    boundaries: Option<&[u32]>,
    size: u32,
    seed: u64,
    edit: Option<InputEdit>,
    left: &mut InputLimits,
    factories: &mut [&mut (dyn InputFactory + 'f)],
) -> Result<Vec<Value>, InputResult> {
    debug_assert!(left.depth <= inputs.limits.depth);
    let mut builder = Builder {
        inputs,
        budget: InputBudget { left: *left },
        random: seed,
        seen: 0,
        size,
        edited: false,
    };
    let mut stack: Vec<BuildFrame> = Vec::new();
    let mut roots: Vec<Value> = Vec::with_capacity(inputs.parameters.len());
    let mut completed = 0usize;
    let mut result: Result<(), InputResult> = Ok(());

    while completed < inputs.parameters.len() {
        if stack.is_empty() {
            if builder.budget.left.depth == 0 {
                result = Err(InputResult::Limit);
                break;
            }
            let mode = if source.is_some() {
                BuildMode::Copy
            } else if boundaries.is_some() {
                BuildMode::Sample
            } else {
                BuildMode::Prng
            };
            let mut frame = BuildFrame {
                request: ValueRequest {
                    ty: inputs.parameters[completed],
                    mode,
                    boundary: boundaries.map_or(0, |b| b[completed]),
                    size,
                    remaining_depth: 0,
                    source: source.map_or(Value::Unit, |s| s[completed].clone()),
                    edit: None,
                },
                source: None,
                cells: Vec::new(),
                count: 0,
                cursor: 0,
                filled: 0,
                tag: 0,
                special: NO_INDEX,
                variant: 0,
                nodes_before: 0,
                bytes_before: 0,
                leaf: false,
            };
            if let Err(err) = frame_init(&mut builder, &mut frame, 1, edit) {
                result = Err(err);
                break;
            }
            stack.push(frame);
        }

        let next_req = {
            #[expect(
                clippy::expect_used,
                reason = "stack is non-empty after pushing root frame above"
            )]
            let frame = stack.last_mut().expect("active build frame");
            if !frame.leaf {
                next_child(&builder, frame)
            } else {
                None
            }
        };

        if let Some(child_req) = next_req {
            if child_req.ty == NO_INDEX {
                continue;
            }
            if stack.len() as u32 == builder.budget.left.depth {
                result = Err(InputResult::Limit);
                break;
            }
            let new_depth = (stack.len() + 1) as u32;
            let mut frame = BuildFrame {
                request: child_req,
                source: None,
                cells: Vec::new(),
                count: 0,
                cursor: 0,
                filled: 0,
                tag: 0,
                special: NO_INDEX,
                variant: 0,
                nodes_before: 0,
                bytes_before: 0,
                leaf: false,
            };
            if let Err(err) = frame_init(&mut builder, &mut frame, new_depth, edit) {
                result = Err(err);
                break;
            }
            stack.push(frame);
            continue;
        }

        #[expect(
            clippy::expect_used,
            reason = "stack is non-empty when finishing current build frame"
        )]
        let mut frame = stack.pop().expect("active build frame");
        let val = match finish(&mut builder, destination, &mut frame, factories) {
            Ok(v) => v,
            Err(err) => {
                result = Err(err);
                break;
            }
        };
        if let Some(parent) = stack.last_mut() {
            let idx = slot(inputs, parent, parent.filled);
            parent.filled += 1;
            parent.cells[idx] = val;
        } else {
            roots.push(val);
            completed += 1;
        }
    }

    *left = builder.budget.left;
    result?;
    debug_assert!(edit.is_none() || builder.edited);
    Ok(roots)
}
