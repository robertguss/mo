//! Compiler-only source metadata assembly (`src/lower/metadata.c` parity).

use mo_base::SourceMap;
use mo_core::{
    BindingMeta, BodyMeta, ClosureMeta, ConstMeta, ExposedType, Expr, ExprId, Metadata, ModuleMeta,
    Program, SourceMetadata,
};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES};

use crate::context::{LowerResult, Lowering};

struct MetaFrame {
    expr: ExprId,
    child: usize,
    entered: bool,
}

fn collect_body_meta(p: &mut Lowering<'_>, root: ExprId, meta: &mut BodyMeta) -> LowerResult<()> {
    let mut spans = Vec::new();
    let mut bindings = Vec::new();
    let mut closures = Vec::new();
    let mut stack = vec![MetaFrame {
        expr: root,
        child: 0,
        entered: false,
    }];
    let mut node = 0usize;

    while let Some(frame) = stack.last_mut() {
        p.work(1)?;
        let expr_id = frame.expr;
        if !frame.entered {
            let prov = &p.provenance[expr_id.0 as usize];
            if let Some(sp) = prov.span {
                p.span = sp;
            }
            if node as u64 == MO_LIMIT_CORE_NODES {
                return p.refuse(
                    ErrorCode::LowerWork,
                    "lowered body exceeds the Core node limit",
                );
            }
            spans.push(prov.span);
            for (slot, bname) in prov.binders.iter().enumerate() {
                if let Some(name) = bname {
                    bindings.push(BindingMeta {
                        node: node as u32,
                        slot: slot as u32,
                        name: name.clone(),
                    });
                }
            }
            if matches!(p.arena.expr(expr_id), Expr::Lam { .. }) {
                let Some(cname) = prov.closure.clone() else {
                    return p.refuse(
                        ErrorCode::LowerUnfinished,
                        "lambda missing closure metadata name",
                    );
                };
                closures.push(ClosureMeta {
                    node: node as u32,
                    name: cname,
                });
            }
            node += 1;
            frame.entered = true;
        }
        let child_idx = frame.child;
        frame.child += 1;
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
        stack.push(MetaFrame {
            expr: next,
            child: 0,
            entered: false,
        });
    }

    p.charge_alloc(spans.len(), 16, 4)?;
    p.charge_alloc(bindings.len(), 24, 8)?;
    p.charge_alloc(closures.len(), 24, 8)?;
    meta.spans = spans;
    meta.bindings = bindings;
    meta.closures = closures;
    Ok(())
}

/// Populates [`Program::metadata`] with `files`, per-function/per-constant preorder body
/// side tables, and source-order sorted module ownership & export lists.
pub(crate) fn lower_metadata(
    p: &mut Lowering<'_>,
    sources: Option<&SourceMap>,
    core: &mut Program,
) -> LowerResult<()> {
    let files: Vec<String> = if let Some(src_map) = sources {
        let mut out = Vec::with_capacity(src_map.len() as usize);
        for i in 0..src_map.len() {
            let path = src_map
                .get(i)
                .map(|f| f.path_lossy().into_owned())
                .unwrap_or_default();
            out.push(p.copy_text(&path)?);
        }
        out
    } else {
        // Fallback when called without SourceMap: reconstruct paths from module table.
        let max_file = p
            .program
            .modules
            .modules
            .iter()
            .map(|m| m.file as usize + 1)
            .max()
            .unwrap_or(0);
        let mut out = vec![String::from("<memory>"); max_file];
        for m in &p.program.modules.modules {
            out[m.file as usize] = format!("{}.mo", m.name);
        }
        out
    };

    let mut total_nodes = p.core_types.len();
    let fn_count = p.instances.len();
    let mut fun_metas = Vec::with_capacity(fn_count);
    for idx in 0..fn_count {
        let body = p.instances[idx].fun.body;
        let mut meta = p.instances[idx].meta.clone();
        collect_body_meta(p, body, &mut meta.body)?;
        if meta.body.spans.len() > (MO_LIMIT_CORE_NODES as usize).saturating_sub(total_nodes) {
            return p.refuse(
                ErrorCode::LowerWork,
                "lowered document exceeds the Core node limit",
            );
        }
        total_nodes += meta.body.spans.len();
        fun_metas.push(meta);
    }

    let const_count = p.consts.len();
    let mut const_metas = Vec::with_capacity(const_count);
    for idx in 0..const_count {
        let body = p.consts[idx].decl.body;
        let mut body_meta = BodyMeta::default();
        collect_body_meta(p, body, &mut body_meta)?;
        if body_meta.spans.len() > (MO_LIMIT_CORE_NODES as usize).saturating_sub(total_nodes) {
            return p.refuse(
                ErrorCode::LowerWork,
                "lowered document exceeds the Core node limit",
            );
        }
        total_nodes += body_meta.spans.len();
        const_metas.push(ConstMeta {
            name: p.consts[idx].meta_name.clone(),
            span: p.consts[idx].meta_span,
            body: body_meta,
        });
    }

    let mod_count = p.program.modules.len();
    let mut modules: Vec<ModuleMeta> = Vec::with_capacity(mod_count);
    for m in 0..mod_count {
        let name = p.copy_text(&p.program.modules.modules[m].name)?;
        modules.push(ModuleMeta {
            name,
            data: Vec::new(),
            funs: Vec::new(),
            consts: Vec::new(),
            exposed_types: Vec::new(),
            exposed_funs: Vec::new(),
        });
    }

    // Sort function instances per module in exact C `function_order`.
    let mut fn_order: Vec<usize> = (0..fn_count).collect();
    fn_order.sort_by(|&a, &b| {
        let x = &p.instances[a];
        let y = &p.instances[b];
        if x.owner != y.owner {
            return x.owner.cmp(&y.owner);
        }
        let x_gen = x.source.is_none() && x.spec.is_none();
        let y_gen = y.source.is_none() && y.spec.is_none();
        if x_gen != y_gen {
            return x_gen.cmp(&y_gen);
        }
        if !x_gen {
            let xm = match x.source {
                Some(fid) => {
                    p.program
                        .interfaces
                        .symbol(p.program.function(fid).symbol)
                        .module
                }
                None => x.spec.map_or(x.owner, |sr| p.spec_module(sr)),
            };
            let ym = match y.source {
                Some(fid) => {
                    p.program
                        .interfaces
                        .symbol(p.program.function(fid).symbol)
                        .module
                }
                None => y.spec.map_or(y.owner, |sr| p.spec_module(sr)),
            };
            if xm != ym {
                return xm.cmp(&ym);
            }
            let xs = x.meta.span.map_or(0, |s| s.start);
            let ys = y.meta.span.map_or(0, |s| s.start);
            if xs != ys {
                return xs.cmp(&ys);
            }
        }
        x.index.cmp(&y.index)
    });
    for idx in fn_order {
        p.work(1)?;
        let inst = &p.instances[idx];
        let m = &mut modules[inst.owner.index()];
        m.funs.push(inst.index);
        if let Some(fid) = inst.source {
            let sym = p.program.interfaces.symbol(p.program.function(fid).symbol);
            if sym.module == inst.owner && sym.exported {
                m.exposed_funs.push(inst.index);
            }
        }
    }

    // Sort data instances per module in exact C `data_order`.
    let data_count = p.data_instances.len();
    let mut data_order: Vec<usize> = (0..data_count).collect();
    data_order.sort_by(|&a, &b| {
        let x = &p.data_instances[a];
        let y = &p.data_instances[b];
        if x.owner != y.owner {
            return x.owner.cmp(&y.owner);
        }
        let xs = p
            .source_type(x.source_ty)
            .owner
            .as_source()
            .map_or(0, |sym| {
                p.program.interfaces.symbol(sym).declaration_span.start
            });
        let ys = p
            .source_type(y.source_ty)
            .owner
            .as_source()
            .map_or(0, |sym| {
                p.program.interfaces.symbol(sym).declaration_span.start
            });
        xs.cmp(&ys).then_with(|| a.cmp(&b))
    });
    for idx in data_order {
        p.work(1)?;
        let d = &p.data_instances[idx];
        let m = &mut modules[d.owner.index()];
        m.data.push(idx as u32);
        if let Some(sym_ref) = p.source_type(d.source_ty).owner.as_source() {
            let sym = p.program.interfaces.symbol(sym_ref);
            if sym.exported {
                m.exposed_types.push(ExposedType {
                    index: idx as u32,
                    contents_exposed: sym.insides,
                });
            }
        }
    }

    // Sort constants per module in exact C `constant_order`.
    let mut const_order: Vec<usize> = (0..const_count).collect();
    const_order.sort_by(|&a, &b| {
        let sa = p.program.interfaces.symbol(p.program.constants[a].symbol);
        let sb = p.program.interfaces.symbol(p.program.constants[b].symbol);
        sa.module
            .cmp(&sb.module)
            .then_with(|| sa.declaration_span.start.cmp(&sb.declaration_span.start))
    });
    for idx in const_order {
        p.work(1)?;
        let sym = p.program.interfaces.symbol(p.program.constants[idx].symbol);
        modules[sym.module.index()].consts.push(idx as u32);
    }

    core.metadata = Metadata::Source(SourceMetadata {
        files,
        modules,
        funs: fun_metas,
        consts: const_metas,
    });
    Ok(())
}
