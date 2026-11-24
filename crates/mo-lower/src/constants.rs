//! Constant lowering and reachable constant-prefix tightening (`src/lower/constants.c` parity).

use std::collections::{HashMap, VecDeque};

use mo_check::SymRef;
use mo_core::{ConstDecl, CoreNatural, Expr, ExprId};
use mo_def::ErrorCode;
use mo_def::limits::MO_LIMIT_CORE_DEPTH;

use crate::context::{ConstRecord, LowerResult, Lowering};
use crate::expressions::lower_constant_body;
use crate::types::lower_type;

/// Canonical Core name `<Module>.<name>` for a top-level constant or non-overloaded symbol.
pub(crate) fn lower_global_name(p: &mut Lowering<'_>, symbol: SymRef) -> LowerResult<String> {
    let sym = p.program.interfaces.symbol(symbol);
    let mod_name = p.module_name(sym.module).to_string();
    let sym_name = sym.name.clone();
    p.concat_text(&mod_name, ".", &sym_name)
}

/// Lowers every top-level constant initializer in checked dependency order.
pub(crate) fn lower_constants(p: &mut Lowering<'_>) -> LowerResult<()> {
    let count = p.program.constants.len();
    p.charge_alloc(count, 64, 8)?;
    p.consts.reserve(count);

    for i in 0..count {
        p.work(1)?;
        let symbol = p.program.constants[i].symbol;
        let ty = p.program.constants[i].ty;
        let decl_span = p.program.interfaces.symbol(symbol).declaration_span;
        let owner = p.program.interfaces.symbol(symbol).module;
        p.span = decl_span;
        let name = lower_global_name(p, symbol)?;
        let meta_span = p.lower_span(decl_span);
        let core_ty = lower_type(p, ty, None)?;
        let body = lower_constant_body(p, i, owner, &name, core_ty)?;
        p.consts.push(ConstRecord {
            decl: ConstDecl {
                name: name.clone(),
                ty: core_ty,
                body,
            },
            meta_name: name,
            meta_span,
        });
    }
    Ok(())
}

struct WalkFrame {
    expr: ExprId,
    child: usize,
}

fn restrict_calls(
    p: &mut Lowering<'_>,
    globals_by_name: &HashMap<String, u32>,
    funs_by_name: &HashMap<String, usize>,
    queued: &mut [bool],
    queue: &mut VecDeque<usize>,
    body: ExprId,
    prefix: u32,
) -> LowerResult<()> {
    let mut stack = vec![WalkFrame {
        expr: body,
        child: 0,
    }];
    while let Some(frame) = stack.last_mut() {
        p.work(1)?;
        let expr_id = frame.expr;
        let child_idx = frame.child;
        frame.child += 1;

        if child_idx == 0 {
            match p.arena.expr(expr_id) {
                Expr::Global(name) => {
                    let name_len = name.len();
                    let g_idx = globals_by_name.get(name).copied();
                    p.work(1 + name_len)?;
                    let Some(idx) = g_idx else {
                        return p.refuse(
                            ErrorCode::LowerUnfinished,
                            "unknown global constant during prefix walk",
                        );
                    };
                    if idx >= prefix {
                        if let Some(sp) = p.provenance[expr_id.0 as usize].span {
                            p.span = sp;
                        }
                        return p.refuse(
                            ErrorCode::LowerUnfinished,
                            "this constant's deferred function needs globals not yet initialized",
                        );
                    }
                }
                Expr::Call { name, .. } => {
                    let name_len = name.len();
                    let f_idx = funs_by_name.get(name).copied();
                    p.work(1 + name_len)?;
                    let Some(callee_idx) = f_idx else {
                        return p.refuse(
                            ErrorCode::LowerUnfinished,
                            "unknown function call during constant prefix walk",
                        );
                    };
                    if prefix < p.instances[callee_idx].const_prefix {
                        p.instances[callee_idx].const_prefix = prefix;
                        if !queued[callee_idx] {
                            queued[callee_idx] = true;
                            queue.push_back(callee_idx);
                        }
                    }
                }
                Expr::Lit(_)
                | Expr::Var(_)
                | Expr::Let { .. }
                | Expr::Lam { .. }
                | Expr::App { .. }
                | Expr::Tuple(_)
                | Expr::Proj { .. }
                | Expr::Con { .. }
                | Expr::Case { .. }
                | Expr::If { .. }
                | Expr::Prim { .. }
                | Expr::Stop(_)
                | Expr::Fail(_)
                | Expr::Join { .. }
                | Expr::Jump { .. } => {}
            }
        }

        let Some(next) = p.expr_child(expr_id, child_idx) else {
            stack.pop();
            continue;
        };
        if stack.len() as u64 == MO_LIMIT_CORE_DEPTH {
            return p.refuse(
                ErrorCode::LowerDepth,
                "lowered body exceeds the Core path limit",
            );
        }
        stack.push(WalkFrame {
            expr: next,
            child: 0,
        });
    }
    Ok(())
}

/// Computes the exact `const_count` prefix for every lowered function via BFS propagation
/// from all constant initializers, rejecting `E1150` if a constant's deferred function reads
/// a global not yet initialized at that point.
pub(crate) fn lower_constant_prefixes(p: &mut Lowering<'_>) -> LowerResult<()> {
    let count = p.consts.len() as u32;
    if count == 0 {
        return Ok(());
    }
    let fn_count = p.instances.len();
    p.charge_alloc(fn_count, 24, 8)?;
    p.charge_alloc(count as usize, 24, 8)?;

    let mut globals_by_name: HashMap<String, u32> = HashMap::with_capacity(count as usize);
    for (i, c) in p.consts.iter().enumerate() {
        globals_by_name.insert(c.decl.name.clone(), i as u32);
    }

    let mut funs_by_name: HashMap<String, usize> = HashMap::with_capacity(fn_count);
    for idx in 0..fn_count {
        p.work(1)?;
        p.instances[idx].const_prefix = count;
        funs_by_name.insert(p.instances[idx].fun.name.clone(), idx);
    }

    let mut queued = vec![false; fn_count];
    let mut queue = VecDeque::new();
    let const_bodies: Vec<ExprId> = p.consts.iter().map(|c| c.decl.body).collect();
    for (i, body) in const_bodies.into_iter().enumerate() {
        restrict_calls(
            p,
            &globals_by_name,
            &funs_by_name,
            &mut queued,
            &mut queue,
            body,
            i as u32,
        )?;
    }

    while let Some(fn_idx) = queue.pop_front() {
        queued[fn_idx] = false;
        let body = p.instances[fn_idx].fun.body;
        let prefix = p.instances[fn_idx].const_prefix;
        restrict_calls(
            p,
            &globals_by_name,
            &funs_by_name,
            &mut queued,
            &mut queue,
            body,
            prefix,
        )?;
    }

    for idx in 0..fn_count {
        let prefix = p.instances[idx].const_prefix;
        let digits = p.number_text(u64::from(prefix))?;
        p.instances[idx].fun.const_count = CoreNatural { digits };
    }
    Ok(())
}
