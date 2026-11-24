//! Compiler metadata side-table validator matching `src/core/metadata.c`
//! using an explicit heap stack so 4096-deep expressions never overflow.

use std::collections::BTreeSet;

use mo_base::Span;
use mo_def::limits::{MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES, MO_LIMIT_FILES, MO_LIMIT_MODULES};

use crate::ir::{BodyMeta, CoreArena, Expr, ExprId, FnKind, Metadata, Program, SourceMetadata};

fn name_valid(s: &str) -> bool {
    !s.is_empty() && !s.as_bytes().contains(&0)
}

fn span_valid(span: Option<Span>, files: usize) -> bool {
    match span {
        None => true,
        Some(s) => (s.file as usize) < files && s.start <= s.end,
    }
}

/// Validates preorder body metadata (`spans`, `params`, `bindings`, `closures`)
/// iteratively to match `src/core/metadata.c:body_valid`.
fn body_valid(
    arena: &CoreArena,
    meta: &BodyMeta,
    root: ExprId,
    param_count: usize,
    files: usize,
) -> bool {
    if meta.params.len() != param_count {
        return false;
    }
    for p in &meta.params {
        if let Some(name) = p
            && !name_valid(name)
        {
            return false;
        }
    }
    for pair in meta.bindings.windows(2) {
        let a = &pair[0];
        let b = &pair[1];
        if a.node > b.node || (a.node == b.node && a.slot >= b.slot) {
            return false;
        }
    }

    let max_depth = MO_LIMIT_CORE_DEPTH as usize;
    let mut node_idx = 0usize;
    let mut binding_idx = 0usize;
    let mut closure_idx = 0usize;
    let mut stack: Vec<(ExprId, usize)> = vec![(root, 0)];

    while let Some((expr_id, depth)) = stack.pop() {
        let Some(expr) = arena.expr_opt(expr_id) else {
            return false;
        };
        if depth >= max_depth || node_idx >= meta.spans.len() {
            return false;
        }
        if !span_valid(meta.spans[node_idx], files) {
            return false;
        }
        let node = node_idx;
        node_idx += 1;

        let slots: usize = match expr {
            Expr::Let { .. } => 1,
            Expr::Lam { params, .. } => params.len(),
            Expr::Join { params, .. } => params.len(),
            Expr::Case { arms, .. } => arms.iter().map(|a| a.names.len()).sum(),
            Expr::Lit(_)
            | Expr::Var(_)
            | Expr::Global(_)
            | Expr::App { .. }
            | Expr::Call { .. }
            | Expr::Tuple(_)
            | Expr::Proj { .. }
            | Expr::Con { .. }
            | Expr::If { .. }
            | Expr::Prim { .. }
            | Expr::Stop(_)
            | Expr::Fail(_)
            | Expr::Jump { .. } => 0,
        };

        while binding_idx < meta.bindings.len()
            && (meta.bindings[binding_idx].node as usize) == node
        {
            let b = &meta.bindings[binding_idx];
            binding_idx += 1;
            if (b.slot as usize) >= slots || !name_valid(&b.name) {
                return false;
            }
        }

        if matches!(expr, Expr::Lam { .. }) {
            let Some(c) = meta.closures.get(closure_idx) else {
                return false;
            };
            closure_idx += 1;
            if (c.node as usize) != node || !name_valid(&c.name) {
                return false;
            }
        }

        let next_depth = depth + 1;
        // Push children in reverse so popping visits them in exact preorder.
        match expr {
            Expr::Lit(_) | Expr::Var(_) | Expr::Global(_) | Expr::Stop(_) => {}
            Expr::Let { bound, body, .. } => {
                stack.push((*body, next_depth));
                stack.push((*bound, next_depth));
            }
            Expr::Lam { body, .. } => {
                stack.push((*body, next_depth));
            }
            Expr::App { func, args } => {
                for &arg in args.iter().rev() {
                    stack.push((arg, next_depth));
                }
                stack.push((*func, next_depth));
            }
            Expr::Call { args, .. }
            | Expr::Tuple(args)
            | Expr::Con { args, .. }
            | Expr::Prim { args, .. }
            | Expr::Jump { args, .. } => {
                for &arg in args.iter().rev() {
                    stack.push((arg, next_depth));
                }
            }
            Expr::Proj { expr: inner, .. } | Expr::Fail(inner) => {
                stack.push((*inner, next_depth));
            }
            Expr::Case { expr: scrut, arms } => {
                for arm in arms.iter().rev() {
                    stack.push((arm.body, next_depth));
                }
                stack.push((*scrut, next_depth));
            }
            Expr::If { cond, then_, else_ } => {
                stack.push((*else_, next_depth));
                stack.push((*then_, next_depth));
                stack.push((*cond, next_depth));
            }
            Expr::Join { body, rest, .. } => {
                stack.push((*rest, next_depth));
                stack.push((*body, next_depth));
            }
        }
    }

    node_idx == meta.spans.len()
        && binding_idx == meta.bindings.len()
        && closure_idx == meta.closures.len()
}

fn mark_members(indices: &[u32], seen: &mut [u8]) -> bool {
    for &n in indices {
        let idx = n as usize;
        if idx >= seen.len() || seen[idx] != 0 {
            return false;
        }
        seen[idx] = 1;
    }
    true
}

fn modules_valid(program: &Program, meta: &SourceMetadata) -> bool {
    let mut module_names: BTreeSet<&str> = BTreeSet::new();
    for m in &meta.modules {
        if !name_valid(&m.name) || !module_names.insert(m.name.as_str()) {
            return false;
        }
    }

    let max_nodes = MO_LIMIT_CORE_NODES as usize;
    if program.data.len() > max_nodes
        || program.funs.len() > max_nodes
        || program.consts.len() > max_nodes
    {
        return false;
    }

    let mut seen_data = vec![0u8; program.data.len()];
    let mut seen_funs = vec![0u8; program.funs.len()];
    let mut seen_consts = vec![0u8; program.consts.len()];
    let mut data_owner = vec![0u32; program.data.len()];

    for (mod_idx, m) in meta.modules.iter().enumerate() {
        if !name_valid(&m.name)
            || !mark_members(&m.data, &mut seen_data)
            || !mark_members(&m.funs, &mut seen_funs)
            || !mark_members(&m.consts, &mut seen_consts)
        {
            return false;
        }
        for &d_idx in &m.data {
            data_owner[d_idx as usize] = mod_idx as u32;
        }
        for &f_idx in &m.funs {
            if program.funs[f_idx as usize].module != m.name {
                return false;
            }
        }
        for exp_ty in &m.exposed_types {
            let idx = exp_ty.index as usize;
            if idx >= seen_data.len() || seen_data[idx] != 1 || data_owner[idx] != mod_idx as u32 {
                return false;
            }
            seen_data[idx] = 3;
        }
        for &f_idx in &m.exposed_funs {
            let idx = f_idx as usize;
            if idx >= seen_funs.len()
                || seen_funs[idx] != 1
                || program.funs[idx].kind != FnKind::Function
                || program.funs[idx].module != m.name
            {
                return false;
            }
            seen_funs[idx] = 3;
        }
    }

    seen_data.iter().all(|&b| b != 0)
        && seen_funs.iter().all(|&b| b != 0)
        && seen_consts.iter().all(|&b| b != 0)
}

/// Validates a Core program's metadata side tables, matching `src/core/metadata.c:mo_core_metadata_valid`.
#[must_use]
pub fn metadata_valid(arena: &CoreArena, program: &Program) -> bool {
    let Metadata::Source(meta) = &program.metadata else {
        return true;
    };
    if (meta.files.len() as u64) > MO_LIMIT_FILES
        || (meta.modules.len() as u64) > MO_LIMIT_MODULES
        || meta.funs.len() != program.funs.len()
        || meta.consts.len() != program.consts.len()
    {
        return false;
    }
    for file in &meta.files {
        if !name_valid(file) {
            return false;
        }
    }
    if !modules_valid(program, meta) {
        return false;
    }
    let file_count = meta.files.len();
    for (c_meta, c_decl) in meta.consts.iter().zip(&program.consts) {
        if !name_valid(&c_meta.name)
            || !span_valid(c_meta.span, file_count)
            || !body_valid(arena, &c_meta.body, c_decl.body, 0, file_count)
        {
            return false;
        }
    }
    for (f_meta, f_decl) in meta.funs.iter().zip(&program.funs) {
        let is_spec = f_decl.kind != FnKind::Function;
        if !name_valid(&f_meta.name)
            || !span_valid(f_meta.span, file_count)
            || f_meta.text.is_some() != is_spec
            || !body_valid(
                arena,
                &f_meta.body,
                f_decl.body,
                f_decl.params.len(),
                file_count,
            )
            || f_meta.allows.is_some() != (f_decl.kind == FnKind::Anchor)
            || (!f_meta.lines.is_empty()) != (f_decl.kind == FnKind::Promise)
        {
            return false;
        }
        for line in &f_meta.lines {
            if !span_valid(line.span, file_count) {
                return false;
            }
        }
        let linked = matches!(f_decl.kind, FnKind::Promise | FnKind::Anchor);
        if f_meta.subject.is_some() != linked {
            return false;
        }
        if let Some(subj) = f_meta.subject {
            let expected_kind = if f_decl.kind == FnKind::Promise {
                FnKind::Function
            } else {
                FnKind::Promise
            };
            let Some(target) = program.funs.get(subj as usize) else {
                return false;
            };
            if target.kind != expected_kind {
                return false;
            }
        }
    }
    true
}
