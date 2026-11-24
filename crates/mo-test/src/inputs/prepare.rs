//! Type graph analysis, productivity ranks, and boundary counts (`src/tools/inputs-prepare.c`).

use mo_bytecode::{CaseShape, LayoutKind, NO_INDEX, TypeKind};
use mo_def::limits::MO_LIMIT_INPUT_DEPTH;

use super::{InputBudget, InputFactory, InputResult, Inputs, TypeInfo};

pub(crate) fn scalar_count(kind: TypeKind) -> u32 {
    match kind {
        TypeKind::Unit => 1,
        TypeKind::Bool => 2,
        TypeKind::Byte | TypeKind::Duration => 3,
        TypeKind::Int | TypeKind::Time | TypeKind::BigInt => 5,
        TypeKind::Float => 11,
        TypeKind::Char | TypeKind::Decimal | TypeKind::Range => 6,
        TypeKind::Text | TypeKind::Bytes => 4,
        TypeKind::Tuple
        | TypeKind::List
        | TypeKind::Set
        | TypeKind::Map
        | TypeKind::Record
        | TypeKind::Choice
        | TypeKind::Function
        | TypeKind::Env
        | TypeKind::Capability => 0,
    }
}

pub(crate) fn skip_base(inputs: &Inputs<'_>, ty: u32) -> u32 {
    let info = &inputs.info[ty as usize];
    if info.factory_index.is_none()
        && inputs.types.get(ty).kind == TypeKind::Choice
        && info.case_tags[0] != info.base_case
    {
        0
    } else {
        1
    }
}

pub(crate) fn variants(inputs: &Inputs<'_>, parent: u32, field: u32) -> u32 {
    let container = &inputs.info[parent as usize];
    let child = container.children[field as usize];
    let info = &inputs.info[child as usize];
    if container.depth > inputs.limits.depth || info.depth >= inputs.limits.depth || info.count == 0
    {
        return 0;
    }
    let kind = inputs.types.get(parent).kind;
    if matches!(kind, TypeKind::List | TypeKind::Map | TypeKind::Set) && container.count == 1 {
        return 0;
    }
    if kind == TypeKind::Choice {
        let mut low = 0usize;
        let mut high = inputs.types.get(parent).cases.len();
        while low < high {
            let mid = low + (high - low) / 2;
            if container.case_offsets[mid] <= field {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        debug_assert!(low != 0);
        if container.case_depths[low - 1] > inputs.limits.depth {
            return 0;
        }
    }
    if info.order != 0 && info.order < inputs.info[parent as usize].order {
        info.count - skip_base(inputs, child)
    } else if info.count > 1 || !info.children.is_empty() {
        1
    } else {
        0
    }
}

fn edges(
    inputs: &mut Inputs<'_>,
    factories: &[&dyn InputFactory],
    index: u32,
    budget: &mut InputBudget,
) -> Result<(), InputResult> {
    let type_def = inputs.types.get(index);
    let info = &mut inputs.info[index as usize];
    if let Some(fi) = info.factory_index {
        info.depth = 1;
        info.count = factories[fi].boundaries();
        info.hashable = factories[fi].hashable();
        return Ok(());
    }
    info.hashable = type_def.kind != TypeKind::Float;
    info.count = scalar_count(type_def.kind);
    info.depth = if info.count == 0 { NO_INDEX } else { 1 };
    let mut count: u64 = 0;
    match type_def.kind {
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
        | TypeKind::BigInt => return Ok(()),
        TypeKind::Function | TypeKind::Env | TypeKind::Capability => {
            return Err(InputResult::Unsupported);
        }
        TypeKind::Tuple | TypeKind::List | TypeKind::Set | TypeKind::Map => {
            count = type_def.parameters.len() as u64;
            if type_def.kind != TypeKind::Tuple {
                info.depth = 1;
                info.count = 3;
            } else {
                info.count = 1;
            }
        }
        TypeKind::Record => {
            count = type_def.fields.len() as u64;
            info.count = 1;
        }
        TypeKind::Choice => {
            let case_count = type_def.cases.len();
            if !budget.work(case_count as u64) {
                return Err(InputResult::Limit);
            }
            info.case_offsets = vec![0; case_count + 1];
            info.case_layouts = vec![NO_INDEX; case_count];
            info.case_tags = vec![0; case_count];
            info.case_depths = vec![NO_INDEX; case_count];
            for (i, case) in type_def.cases.iter().enumerate() {
                info.case_offsets[i] = count as u32;
                count += case.fields.len() as u64;
                if count > u64::from(u32::MAX) {
                    return Err(InputResult::Limit);
                }
            }
            info.case_offsets[case_count] = count as u32;
            info.count = case_count as u32;
        }
    }
    if !budget.work(count) {
        return Err(InputResult::Limit);
    }
    let child_count = count as usize;
    info.children = Vec::with_capacity(child_count);
    match type_def.kind {
        TypeKind::Record => {
            for f in &type_def.fields {
                info.children.push(f.ty);
            }
        }
        TypeKind::Choice => {
            for case in &type_def.cases {
                for f in &case.fields {
                    info.children.push(f.ty);
                }
            }
        }
        _ => {
            info.children.extend_from_slice(&type_def.parameters);
        }
    }
    Ok(())
}

fn fields_depth(info_table: &[TypeInfo], children: &[u32], start: usize, end: usize) -> u32 {
    let mut depth = 0u32;
    for &child_ty in &children[start..end] {
        let child = info_table[child_ty as usize].depth;
        if child == NO_INDEX {
            return NO_INDEX;
        }
        if child > depth {
            depth = child;
        }
    }
    if u64::from(depth) < MO_LIMIT_INPUT_DEPTH {
        depth + 1
    } else {
        (MO_LIMIT_INPUT_DEPTH + 1) as u32
    }
}

fn ranks(
    inputs: &mut Inputs<'_>,
    graph: &[u32],
    budget: &mut InputBudget,
) -> Result<(), InputResult> {
    let mut settled = false;
    let count = graph.len();
    for _pass in 0..=count {
        if settled {
            break;
        }
        let mut changed = false;
        for &ty in graph {
            let type_def = inputs.types.get(ty);
            let child_count = inputs.info[ty as usize].children.len();
            if !budget.work((child_count + type_def.cases.len() + 1) as u64) {
                return Err(InputResult::Limit);
            }
            if inputs.info[ty as usize].factory_index.is_some() {
                continue;
            }
            if inputs.info[ty as usize].hashable {
                let all_hashable = inputs.info[ty as usize]
                    .children
                    .iter()
                    .all(|&c| inputs.info[c as usize].hashable);
                if !all_hashable {
                    inputs.info[ty as usize].hashable = false;
                    changed = true;
                }
            }
            let cur_depth = inputs.info[ty as usize].depth;
            let cur_base = inputs.info[ty as usize].base_case;
            let (depth, base) = match type_def.kind {
                TypeKind::Record | TypeKind::Tuple => {
                    let d = fields_depth(
                        &inputs.info,
                        &inputs.info[ty as usize].children,
                        0,
                        child_count,
                    );
                    (d, cur_base)
                }
                TypeKind::Choice => {
                    let mut best_d = NO_INDEX;
                    let mut best_c = cur_base;
                    for c in 0..type_def.cases.len() {
                        let start = inputs.info[ty as usize].case_offsets[c] as usize;
                        let end = inputs.info[ty as usize].case_offsets[c + 1] as usize;
                        let cand = fields_depth(
                            &inputs.info,
                            &inputs.info[ty as usize].children,
                            start,
                            end,
                        );
                        if cand < best_d {
                            best_d = cand;
                            best_c = c as u32;
                        }
                    }
                    (best_d, best_c)
                }
                _ => (cur_depth, cur_base),
            };
            if depth != cur_depth || base != cur_base {
                inputs.info[ty as usize].depth = depth;
                inputs.info[ty as usize].base_case = base;
                changed = true;
            }
        }
        settled = !changed;
    }
    if !settled {
        return Err(InputResult::Limit);
    }
    for &param in &inputs.parameters {
        let depth = inputs.info[param as usize].depth;
        if depth == NO_INDEX {
            return Err(InputResult::Uninhabited);
        }
        if depth > inputs.limits.depth {
            return Err(InputResult::Limit);
        }
    }
    for &ty in graph {
        let type_def = inputs.types.get(ty);
        let child_count = inputs.info[ty as usize].children.len();
        if !budget.work((child_count + type_def.cases.len() + 1) as u64) {
            return Err(InputResult::Limit);
        }
        if inputs.info[ty as usize].depth > inputs.limits.depth {
            inputs.info[ty as usize].count = 0;
            continue;
        }
        if inputs.info[ty as usize].factory_index.is_some() {
            continue;
        }
        match type_def.kind {
            TypeKind::Choice => {
                let mut valid_count = 0u32;
                for c in 0..type_def.cases.len() {
                    let start = inputs.info[ty as usize].case_offsets[c] as usize;
                    let end = inputs.info[ty as usize].case_offsets[c + 1] as usize;
                    let cd =
                        fields_depth(&inputs.info, &inputs.info[ty as usize].children, start, end);
                    inputs.info[ty as usize].case_depths[c] = cd;
                    if cd <= inputs.limits.depth {
                        inputs.info[ty as usize].case_tags[valid_count as usize] = c as u32;
                        valid_count += 1;
                    }
                }
                inputs.info[ty as usize].case_count = valid_count;
                inputs.info[ty as usize].count = valid_count;
            }
            TypeKind::List | TypeKind::Set | TypeKind::Map => {
                let too_deep = inputs.info[ty as usize]
                    .children
                    .iter()
                    .any(|&c| inputs.info[c as usize].depth >= inputs.limits.depth);
                if too_deep {
                    inputs.info[ty as usize].count = 1;
                }
            }
            _ => {}
        }
        if matches!(type_def.kind, TypeKind::Map | TypeKind::Set)
            && !inputs.info[type_def.parameters[0] as usize].hashable
        {
            return Err(InputResult::Unsupported);
        }
    }
    Ok(())
}

fn counts(
    inputs: &mut Inputs<'_>,
    graph: &[u32],
    budget: &mut InputBudget,
) -> Result<(), InputResult> {
    let mut stack: Vec<(u32, usize)> = Vec::with_capacity(graph.len());
    let mut order = 0u32;
    for &root in graph {
        if inputs.info[root as usize].visit != 0 {
            continue;
        }
        inputs.info[root as usize].visit = 1;
        stack.push((root, 0));
        while let Some((ty, child_idx)) = stack.last_mut() {
            if !budget.work(1) {
                return Err(InputResult::Limit);
            }
            let cur_ty = *ty;
            let child_count = inputs.info[cur_ty as usize].children.len();
            if *child_idx < child_count {
                let child = inputs.info[cur_ty as usize].children[*child_idx];
                *child_idx += 1;
                if inputs.info[child as usize].visit == 0 {
                    inputs.info[child as usize].visit = 1;
                    stack.push((child, 0));
                }
                continue;
            }
            order += 1;
            inputs.info[cur_ty as usize].order = order;
            for c in 0..child_count {
                let added = u64::from(variants(inputs, cur_ty, c as u32));
                let total = u64::from(inputs.info[cur_ty as usize].count) + added;
                inputs.info[cur_ty as usize].count = total.min(u64::from(u32::MAX)) as u32;
            }
            inputs.info[cur_ty as usize].visit = 2;
            stack.pop();
        }
    }
    Ok(())
}

pub(crate) fn prepare(
    inputs: &mut Inputs<'_>,
    factories: &[&dyn InputFactory],
    name_work: usize,
) -> Result<(), InputResult> {
    if inputs.parameters.is_empty() {
        return Ok(());
    }
    let type_count = inputs.types.items.len();
    let mut budget = InputBudget {
        left: super::InputLimits {
            work: inputs.limits.prepare_work - name_work as u64,
            ..inputs.limits
        },
    };
    if !budget.work(type_count as u64) {
        return Err(InputResult::Limit);
    }
    inputs.info = vec![TypeInfo::default(); type_count];
    for (fi, factory) in factories.iter().enumerate() {
        let ty = factory.ty() as usize;
        debug_assert!(ty < type_count && factory.boundaries() != 0);
        debug_assert!(inputs.info[ty].factory_index.is_none());
        inputs.info[ty].factory_index = Some(fi);
    }
    let mut graph = Vec::with_capacity(type_count);
    for &param in &inputs.parameters {
        if !inputs.info[param as usize].reachable {
            inputs.info[param as usize].reachable = true;
            graph.push(param);
        }
    }
    let mut i = 0;
    while i < graph.len() {
        let ty = graph[i];
        edges(inputs, factories, ty, &mut budget)?;
        let child_len = inputs.info[ty as usize].children.len();
        for c in 0..child_len {
            let child = inputs.info[ty as usize].children[c];
            if !inputs.info[child as usize].reachable {
                inputs.info[child as usize].reachable = true;
                graph.push(child);
            }
        }
        i += 1;
    }
    ranks(inputs, &graph, &mut budget)?;
    let layout_count = inputs.types.layouts.len();
    if !budget.work(layout_count as u64) {
        return Err(InputResult::Limit);
    }
    for (li, layout) in inputs.types.layouts.iter().enumerate() {
        let info = &mut inputs.info[layout.ty as usize];
        if !info.reachable || info.factory_index.is_some() {
            continue;
        }
        match layout.kind {
            LayoutKind::Record | LayoutKind::Tuple => info.layout = li as u32,
            LayoutKind::Case => info.case_layouts[layout.case_index as usize] = li as u32,
            LayoutKind::Closure => {}
        }
    }
    for &ty in &graph {
        let type_def = inputs.types.get(ty);
        let info = &inputs.info[ty as usize];
        if info.factory_index.is_some() || info.depth > inputs.limits.depth {
            continue;
        }
        if matches!(type_def.kind, TypeKind::Record | TypeKind::Tuple) && info.layout == NO_INDEX {
            return Err(InputResult::Unsupported);
        }
        if type_def.kind == TypeKind::Choice {
            if !budget.work(type_def.cases.len() as u64) {
                return Err(InputResult::Limit);
            }
            for c in 0..type_def.cases.len() {
                if info.case_depths[c] <= inputs.limits.depth
                    && inputs.types.case_shape(ty, c as u32) == CaseShape::Boxed
                    && info.case_layouts[c] == NO_INDEX
                {
                    return Err(InputResult::Unsupported);
                }
            }
        }
    }
    counts(inputs, &graph, &mut budget)
}
