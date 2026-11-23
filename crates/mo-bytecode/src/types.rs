use std::collections::HashMap;

use mo_def::limits::MO_LIMIT_HEAP_WORK;

use crate::{
    ABILITY_EQUAL, ABILITY_HASH, ABILITY_ORDER, CaseShape, LayoutDef, LayoutKind, LoadError,
    TypeDef, TypeKind,
};

#[derive(Debug, Clone)]
pub struct TypeTable {
    pub items: Vec<TypeDef>,
    pub layouts: Vec<LayoutDef>,
    pub abilities: Vec<u8>,
    case_layouts: HashMap<(u32, u32), u32>,
    closure_layouts: HashMap<u32, u32>,
    tuple_layouts: HashMap<u32, u32>,
    record_layouts: HashMap<u32, u32>,
}

impl TypeTable {
    pub fn finish(mut items: Vec<TypeDef>, layouts: Vec<LayoutDef>) -> Result<Self, LoadError> {
        // Compute reference classification for every type (matches src/heap/types.c).
        let shapes_ref: Vec<bool> = (0..items.len())
            .map(|i| {
                let t = &items[i];
                if t.kind == TypeKind::Choice {
                    (0..t.cases.len()).any(|j| {
                        let shape = compute_case_shape(&items, i, j);
                        matches!(shape, CaseShape::Ref | CaseShape::Boxed)
                    })
                } else {
                    (t.kind as u32) >= (TypeKind::Text as u32)
                }
            })
            .collect();
        for (item, is_ref) in items.iter_mut().zip(shapes_ref) {
            item.is_reference = is_ref;
        }

        let abilities = compute_abilities(&items)?;

        let mut case_layouts = HashMap::new();
        let mut closure_layouts = HashMap::new();
        let mut tuple_layouts = HashMap::new();
        let mut record_layouts = HashMap::new();

        for (idx, layout) in layouts.iter().enumerate() {
            let idx = idx as u32;
            match layout.kind {
                LayoutKind::Case => {
                    case_layouts.insert((layout.ty, layout.case_index), idx);
                }
                LayoutKind::Closure => {
                    if closure_layouts.insert(layout.function, idx).is_some() {
                        return Err(LoadError::invalid(20, "ambiguous closure layout"));
                    }
                }
                LayoutKind::Tuple => {
                    tuple_layouts.insert(layout.ty, idx);
                }
                LayoutKind::Record => {
                    record_layouts.insert(layout.ty, idx);
                }
            }
        }

        Ok(Self {
            items,
            layouts,
            abilities,
            case_layouts,
            closure_layouts,
            tuple_layouts,
            record_layouts,
        })
    }

    pub fn get(&self, idx: u32) -> &TypeDef {
        &self.items[idx as usize]
    }

    pub fn layout(&self, idx: u32) -> &LayoutDef {
        &self.layouts[idx as usize]
    }

    pub fn is_reference(&self, ty: u32) -> bool {
        self.items[ty as usize].is_reference
    }

    pub fn case_shape(&self, ty: u32, case_idx: u32) -> CaseShape {
        compute_case_shape(&self.items, ty as usize, case_idx as usize)
    }

    pub fn abilities(&self, ty: u32) -> u8 {
        self.abilities[ty as usize]
    }

    pub fn find_case_layout(&self, ty: u32, case_idx: u32) -> Option<u32> {
        self.case_layouts.get(&(ty, case_idx)).copied()
    }

    pub fn find_closure_layout(&self, function: u32) -> Option<u32> {
        self.closure_layouts.get(&function).copied()
    }

    pub fn find_tuple_layout(&self, ty: u32) -> Option<u32> {
        self.tuple_layouts.get(&ty).copied()
    }

    pub fn find_record_layout(&self, ty: u32) -> Option<u32> {
        self.record_layouts.get(&ty).copied()
    }
}

/// Exact port of `mo_case_shape` (`src/heap/types.c`).
pub fn compute_case_shape(items: &[TypeDef], ty: usize, case_idx: usize) -> CaseShape {
    let type_case = &items[ty].cases[case_idx];
    if type_case.fields.is_empty() {
        return CaseShape::Empty;
    }
    if type_case.fields.len() != 1 {
        return CaseShape::Boxed;
    }
    let field = &items[type_case.fields[0].ty as usize];
    if field.kind == TypeKind::Choice {
        return if field.has_fields {
            CaseShape::Boxed
        } else {
            CaseShape::Inline
        };
    }
    if matches!(field.kind, TypeKind::Decimal | TypeKind::Range) {
        return CaseShape::Boxed;
    }
    if (field.kind as u32) <= (TypeKind::Duration as u32) {
        CaseShape::Inline
    } else {
        CaseShape::Ref
    }
}

fn initial_abilities(kind: TypeKind) -> u8 {
    match kind {
        TypeKind::Function | TypeKind::Env | TypeKind::Capability => 0,
        TypeKind::Float => ABILITY_EQUAL | ABILITY_ORDER,
        TypeKind::Range | TypeKind::Map | TypeKind::Set => ABILITY_EQUAL | ABILITY_HASH,
        _ => ABILITY_EQUAL | ABILITY_HASH | ABILITY_ORDER,
    }
}

/// Exact port of `mo_heap_abilities` (`src/heap/abilities.c`).
fn compute_abilities(items: &[TypeDef]) -> Result<Vec<u8>, LoadError> {
    let n = items.len();
    let mut work: u64 = n as u64;
    let mut reverse_edges: Vec<Vec<u32>> = vec![Vec::new(); n];
    let mut abilities: Vec<u8> = Vec::with_capacity(n);
    let mut queue: Vec<u32> = Vec::new();

    for (i, t) in items.iter().enumerate() {
        let ab = initial_abilities(t.kind);
        abilities.push(ab);
        if ab != (ABILITY_EQUAL | ABILITY_HASH | ABILITY_ORDER) {
            queue.push(i as u32);
        }
        match t.kind {
            TypeKind::Record => {
                work = work.saturating_add(t.fields.len() as u64);
                if work > MO_LIMIT_HEAP_WORK {
                    return Err(LoadError::limit(
                        20,
                        "type ability computation exceeded work limit",
                    ));
                }
                for f in &t.fields {
                    reverse_edges[f.ty as usize].push(i as u32);
                }
            }
            TypeKind::Tuple | TypeKind::List | TypeKind::Map | TypeKind::Set => {
                work = work.saturating_add(t.parameters.len() as u64);
                if work > MO_LIMIT_HEAP_WORK {
                    return Err(LoadError::limit(
                        20,
                        "type ability computation exceeded work limit",
                    ));
                }
                for &p in &t.parameters {
                    reverse_edges[p as usize].push(i as u32);
                }
            }
            TypeKind::Choice => {
                work = work.saturating_add(t.cases.len() as u64);
                if work > MO_LIMIT_HEAP_WORK {
                    return Err(LoadError::limit(
                        20,
                        "type ability computation exceeded work limit",
                    ));
                }
                for c in &t.cases {
                    work = work.saturating_add(c.fields.len() as u64);
                    if work > MO_LIMIT_HEAP_WORK {
                        return Err(LoadError::limit(
                            20,
                            "type ability computation exceeded work limit",
                        ));
                    }
                    for f in &c.fields {
                        reverse_edges[f.ty as usize].push(i as u32);
                    }
                }
            }
            _ => {}
        }
    }

    let mut read_idx = 0;
    while read_idx < queue.len() {
        let child = queue[read_idx] as usize;
        read_idx += 1;
        let child_ab = abilities[child];
        for &parent in &reverse_edges[child] {
            let p = parent as usize;
            let next = abilities[p] & child_ab;
            if next != abilities[p] {
                abilities[p] = next;
                queue.push(parent);
            }
        }
    }

    Ok(abilities)
}
