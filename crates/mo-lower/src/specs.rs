//! Specification lowering: `test`, `example`, `promise`, `allows`/`forbids` anchors,
//! and service/actor promises & anchors (`src/lower/specs.c` parity).

use mo_check::{CheckedSpec, CheckedSpecId, CheckedSpecKind};
use mo_core::{FnKind, SpecLine};
use mo_def::ErrorCode;
use mo_def::limits::MO_LIMIT_CORE_NODES;

use crate::context::{LowerResult, Lowering, SpecRef};
use crate::types::{lower_function_name, lower_instance, lower_type};

/// A library or path-dependency module's specs run only when `mo test` is given its own file
/// (making it module 0).
fn is_library_spec(p: &Lowering<'_>, spec: &CheckedSpec) -> bool {
    let m = spec.module;
    if m.0 == 0 {
        return false;
    }
    let loaded = p.program.modules.module(m);
    loaded.library || loaded.dependency
}

fn reserve_spec(
    p: &mut Lowering<'_>,
    spec_idx: usize,
    subject: Option<usize>,
) -> LowerResult<usize> {
    let spec = p.program.specs[spec_idx].clone();
    let span = p.syntax_node(spec.module, spec.syntax).span;
    p.span = span;

    // Check if this (spec_idx, subject) pair was already reserved (specs linked list in C).
    // For generic promises, multiple concrete subject instances can share one CheckedSpecId.
    let mut cur = p.spec_instances[spec_idx];
    while let Some(inst_idx) = cur {
        p.work(1)?;
        if p.instances[inst_idx].subject == subject {
            return Ok(inst_idx);
        }
        cur = None;
        // Scan existing instances with the same spec_idx if more than one subject exists.
        for (k, inst) in p.instances.iter().enumerate() {
            if inst.spec == Some(SpecRef::Program(CheckedSpecId(spec_idx as u32)))
                && inst.subject == subject
            {
                return Ok(k);
            }
        }
    }

    if p.instances.len() as u64 >= MO_LIMIT_CORE_NODES {
        return p.refuse(
            ErrorCode::LowerWork,
            "lowering exceeds its spec function limit",
        );
    }

    let owner = if spec.kind == CheckedSpecKind::Promise {
        match subject {
            Some(subj) => p.instances[subj].owner,
            None => spec.module,
        }
    } else {
        spec.module
    };
    let depth = subject.map_or(0, |s| p.instances[s].depth);
    let subst = if spec.kind == CheckedSpecKind::Promise {
        subject.and_then(|s| p.instances[s].subst)
    } else {
        None
    };

    let fn_kind = match spec.kind {
        CheckedSpecKind::Test => FnKind::Test,
        CheckedSpecKind::Example => FnKind::Example,
        CheckedSpecKind::Promise => FnKind::Promise,
        CheckedSpecKind::Anchor => FnKind::Anchor,
    };

    let ret_src = match spec.kind {
        CheckedSpecKind::Promise | CheckedSpecKind::Anchor => p.types_overlay.builtin("Int"),
        CheckedSpecKind::Test | CheckedSpecKind::Example => p.types_overlay.builtin("Unit"),
    };
    let ret = lower_type(p, ret_src, None)?;

    let (base_name, suffix) = match spec.kind {
        CheckedSpecKind::Test => (p.module_name(owner).to_string(), ".test."),
        CheckedSpecKind::Example => {
            let Some(subj_func_id) = spec.subject else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "example missing subject function",
                );
            };
            let sym = p.program.function(subj_func_id).symbol;
            (lower_function_name(p, sym)?, ".example.")
        }
        CheckedSpecKind::Promise => {
            let Some(subj_idx) = subject else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "promise missing subject instance",
                );
            };
            (p.instances[subj_idx].fun.name.clone(), ".promise.")
        }
        CheckedSpecKind::Anchor => {
            let Some(subj_idx) = subject else {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "anchor missing promise instance",
                );
            };
            let sfx = if spec.allows { ".allows." } else { ".forbids." };
            (p.instances[subj_idx].fun.name.clone(), sfx)
        }
    };
    let num = p.number_text(u64::from(spec.number))?;
    let name = p.concat_text(&base_name, suffix, &num)?;

    let inst_idx = p.push_instance(owner, name.clone(), fn_kind, ret, name, p.lower_span(span))?;

    let text_bytes = if spec.kind == CheckedSpecKind::Test {
        spec.wording.as_deref().unwrap_or(b"")
    } else {
        spec.text.bytes.as_slice()
    };
    let text_str = p.copy_text(&String::from_utf8_lossy(text_bytes))?;

    let mut lines = Vec::new();
    if spec.kind == CheckedSpecKind::Promise {
        lines.reserve(spec.conditions.len());
        for cond in &spec.conditions {
            let line_text = p.copy_text(&String::from_utf8_lossy(&cond.text.bytes))?;
            lines.push(SpecLine {
                span: p.lower_span(cond.text.span),
                text: line_text,
            });
        }
    }

    let subject_index = subject.map(|s| p.instances[s].index);
    let inst = &mut p.instances[inst_idx];
    inst.spec = Some(SpecRef::Program(CheckedSpecId(spec_idx as u32)));
    inst.subject = subject;
    inst.subst = subst;
    inst.depth = depth;
    inst.meta.text = Some(text_str);
    match spec.kind {
        CheckedSpecKind::Promise => {
            inst.meta.subject = subject_index;
            inst.meta.lines = lines;
        }
        CheckedSpecKind::Anchor => {
            inst.meta.subject = subject_index;
            inst.meta.allows = Some(spec.allows);
        }
        CheckedSpecKind::Test | CheckedSpecKind::Example => {}
    }

    if p.spec_instances[spec_idx].is_none() {
        p.spec_instances[spec_idx] = Some(inst_idx);
    }
    Ok(inst_idx)
}

/// Reserves all promises attached to `subject_idx` whenever a concrete function instance is
/// lowered after spec tables have been initialized.
pub(crate) fn lower_subject_specs(p: &mut Lowering<'_>, subject_idx: usize) -> LowerResult<()> {
    if p.promise_heads.is_empty() {
        return Ok(());
    }
    let Some(src_id) = p.instances[subject_idx].source else {
        return Ok(());
    };
    let mut cur = p.promise_heads[src_id.index()];
    while let Some(spec_idx) = cur {
        p.work(1)?;
        let next = p.promise_next[spec_idx];
        let _ = reserve_spec(p, spec_idx, Some(subject_idx))?;
        cur = next;
    }
    Ok(())
}

fn reserve_service_spec(
    p: &mut Lowering<'_>,
    spec_ref: SpecRef,
    promise_inst: Option<usize>,
) -> LowerResult<usize> {
    let (module, syntax, number, allows) = {
        let s = p.spec_ref(spec_ref);
        (s.module, s.syntax, s.number, s.allows)
    };
    let span = p.syntax_node(module, syntax).span;
    p.span = span;
    if p.instances.len() as u64 >= MO_LIMIT_CORE_NODES {
        return p.refuse(
            ErrorCode::LowerWork,
            "lowering exceeds its spec function limit",
        );
    }
    let int_src = p.types_overlay.builtin("Int");
    let ret = lower_type(p, int_src, None)?;
    let num = p.number_text(u64::from(number))?;
    let name = match promise_inst {
        None => {
            let mod_name = p.module_name(module).to_string();
            p.concat_text(&mod_name, ".service.promise.", &num)?
        }
        Some(pidx) => {
            let pname = p.instances[pidx].fun.name.clone();
            let sfx = if allows { ".allows." } else { ".forbids." };
            p.concat_text(&pname, sfx, &num)?
        }
    };
    let idx = p.push_instance(
        module,
        name.clone(),
        FnKind::Function,
        ret,
        name,
        p.lower_span(span),
    )?;
    p.instances[idx].spec = Some(spec_ref);
    p.instances[idx].subject = promise_inst;
    Ok(idx)
}

fn lower_service_promises(p: &mut Lowering<'_>) -> LowerResult<()> {
    let service_count = p.program.services.len();
    for s_idx in 0..service_count {
        let promise_count = p.program.services[s_idx].promises.len();
        for p_idx in 0..promise_count {
            p.work(1)?;
            if is_library_spec(p, &p.program.services[s_idx].promises[p_idx].spec) {
                continue;
            }
            let pf = reserve_service_spec(
                p,
                SpecRef::ServicePromise {
                    service: s_idx,
                    promise: p_idx,
                },
                None,
            )?;
            let anchor_count = p.program.services[s_idx].promises[p_idx].anchors.len();
            for a_idx in 0..anchor_count {
                let _ = reserve_service_spec(
                    p,
                    SpecRef::ServiceAnchor {
                        service: s_idx,
                        promise: p_idx,
                        anchor: a_idx,
                    },
                    Some(pf),
                )?;
            }
        }
    }
    Ok(())
}

/// Reserves all program-level specs (tests, examples, anchors, promises on all discovered
/// instances) and service/actor promises & anchors.
pub(crate) fn lower_specs(p: &mut Lowering<'_>) -> LowerResult<()> {
    let spec_count = p.program.specs.len();
    let fn_count = p.program.functions.len();
    p.charge_alloc(spec_count, 16, 8)?;
    p.charge_alloc(fn_count, 4, 4)?;
    p.spec_instances = vec![None; spec_count];
    p.promise_heads = vec![None; fn_count];
    p.promise_next = vec![None; spec_count];

    for i in (0..spec_count).rev() {
        p.work(1)?;
        let spec = &p.program.specs[i];
        if spec.kind != CheckedSpecKind::Promise || is_library_spec(p, spec) {
            continue;
        }
        let Some(subj) = spec.subject else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "promise missing subject function",
            );
        };
        p.promise_next[i] = p.promise_heads[subj.index()];
        p.promise_heads[subj.index()] = Some(i);
    }

    for i in 0..spec_count {
        p.work(1)?;
        let (kind, module, syntax, subject_opt, promise_opt, arguments, abilities, is_lib) = {
            let s = &p.program.specs[i];
            (
                s.kind,
                s.module,
                s.syntax,
                s.subject,
                s.promise,
                s.arguments.clone(),
                s.abilities.clone(),
                is_library_spec(p, s),
            )
        };
        if is_lib {
            continue;
        }
        p.span = p.syntax_node(module, syntax).span;
        match kind {
            CheckedSpecKind::Example | CheckedSpecKind::Test => {
                let _ = reserve_spec(p, i, None)?;
            }
            CheckedSpecKind::Anchor => {
                let Some(subj_func) = subject_opt else {
                    return p.refuse(ErrorCode::LowerUnfinished, "anchor missing subject");
                };
                let Some(promise_id) = promise_opt else {
                    return p.refuse(ErrorCode::LowerUnfinished, "anchor missing promise");
                };
                let sym = p.program.function(subj_func).symbol;
                let depth = u32::from(!arguments.is_empty());
                let subj_inst =
                    lower_instance(p, sym, &arguments, None, &abilities, module, depth)?;
                let promise_inst = reserve_spec(p, promise_id.index(), Some(subj_inst))?;
                let _ = reserve_spec(p, i, Some(promise_inst))?;
            }
            CheckedSpecKind::Promise => {}
        }
    }

    // Snapshot current instance count: `lower_subject_specs` appends new spec instances to
    // `p.instances`, which do not have `source.is_some()`.
    let initial_len = p.instances.len();
    for idx in 0..initial_len {
        if p.instances[idx].source.is_some() {
            lower_subject_specs(p, idx)?;
        }
    }
    lower_service_promises(p)?;
    Ok(())
}
