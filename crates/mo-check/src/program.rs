//! Whole-program typechecking orchestration, `test`/`example`/`promise`/`anchor` spec checking,
//! `main` entry contract (`E0079`/`E0080`), and unused-import audit (`E0093`)
//! (`src/check/program.c` parity).

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_DIAGNOSTICS, MO_LIMIT_PROGRAM_CHECK_WORK};
use mo_syntax::{NodeId, SyntaxKind, SyntaxTree, TokenKind};

use crate::abilities::{AbilityArena, AbilityContext, SourceAbility};
use crate::apis::check_apis;
use crate::captures::check_capture_arguments;
use crate::checked::{
    CheckedBody, CheckedCondition, CheckedEntry, CheckedEntryResult, CheckedFunction,
    CheckedProgram, CheckedSpec, CheckedSpecKind, SpecText,
};
use crate::constants::{check_constants, order_constants};
use crate::ids::{CheckedFuncId, CheckedSpecId, FuncSigId, ModuleId, SymRef, TypeId};
use crate::infer::bodies::fn_enclosing_for_sig;
use crate::infer::{EnclosingContext, ProgramServicesView, check_function, check_spec_body};
use crate::load::ModuleSet;
use crate::modules::{ModuleInterfaces, resolve_modules};
use crate::recursion::check_recursion;
use crate::service_promises::check_service_promises;
use crate::services::{check_services, prepare_services};
use crate::signatures::{Signatures, prepare_available_signatures};
use crate::types::{CAPABILITY_NAMES, SourceTypeKind, TypeOwner};

fn report(
    diags: &mut DiagBag,
    code: ErrorCode,
    span: Span,
    message: impl Into<String>,
    reason: Option<&str>,
) -> bool {
    let msg = message.into();
    let mut diag = Diagnostic::new(code, span, msg.clone()).with_primary_text(msg);
    if let Some(r) = reason {
        diag = diag.with_reason(r);
    }
    diags.push(diag).unwrap_or(false)
}

fn resource_failed(diags: &DiagBag) -> bool {
    if diags.dropped() != 0 {
        return true;
    }
    diags
        .items()
        .iter()
        .any(|d| d.code.number() >= 1000 && d.code != ErrorCode::CheckUnfinished)
}

fn check_type_abilities_decl(
    abilities_arena: &mut AbilityArena,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    ty: TypeId,
    span: Span,
    diags: &mut DiagBag,
) -> bool {
    if !signatures.signature_type_available(Some(ty)) {
        return true;
    }
    let arena = std::mem::take(abilities_arena);
    let mut ctx = AbilityContext::with_arena(arena, interfaces, signatures, 0);
    let ok = ctx.check_type_abilities(ty);
    let (arena, _cache, _work, err) = ctx.into_parts();
    *abilities_arena = arena;
    if ok {
        return true;
    }
    let (code, message, reason) = match err {
        Some(f) => (f.code, f.message, f.reason),
        None => (
            ErrorCode::CheckWork,
            "the function needs too much ability-checking work".to_string(),
            None,
        ),
    };
    let kept = report(diags, code, span, message, reason);
    kept && !resource_failed(diags)
}

/// Copies the exact source bytes covered by `span` (`mo_spec_copy_text`).
#[must_use]
pub(crate) fn spec_copy_text(sources: &SourceMap, span: Span) -> Option<SpecText> {
    let file = sources.get(span.file)?;
    let start = span.start as usize;
    let end = span.end as usize;
    if start > end || end > file.bytes().len() {
        return None;
    }
    Some(SpecText {
        bytes: file.bytes()[start..end].to_vec(),
        span,
    })
}

fn hex_digit(c: u8) -> u32 {
    match c {
        b'a'..=b'f' => u32::from(c - b'a') + 10,
        b'A'..=b'F' => u32::from(c - b'A') + 10,
        _ => u32::from(c - b'0'),
    }
}

/// Unescapes a string or triple-quoted block literal for `test`/`promise` wording
/// (`mo_spec_wording`).
#[must_use]
pub(crate) fn spec_wording(tree: &SyntaxTree, text_node: NodeId) -> Vec<u8> {
    let bytes = tree.node_text(text_node).unwrap_or(&[]);
    let length = bytes.len();
    let quote = if length >= 6 && bytes[1] == b'"' && bytes[2] == b'"' {
        3usize
    } else {
        1usize
    };
    let mut begin = quote;
    let mut end = length.saturating_sub(quote);
    let mut indent = 0usize;
    if quote == 3 {
        while begin < end && bytes[begin] != b'\n' {
            begin += 1;
        }
        debug_assert!(begin < end);
        begin += 1;
        let mut closing = end;
        while closing > begin && bytes[closing - 1] == b' ' {
            closing -= 1;
        }
        indent = end - closing;
        end = if closing > begin { closing - 1 } else { begin };
        if end > begin && bytes[end - 1] == b'\r' {
            end -= 1;
        }
    }
    let mut out = Vec::with_capacity(length);
    let mut line_start = quote == 3;
    let mut i = begin;
    while i < end {
        if line_start {
            let mut spaces = 0usize;
            while i < end && spaces < indent && bytes[i] == b' ' {
                i += 1;
                spaces += 1;
            }
            line_start = false;
            if i == end {
                break;
            }
        }
        let mut c = bytes[i];
        if quote == 3 && c == b'\r' {
            debug_assert!(i + 1 < end && bytes[i + 1] == b'\n');
            i += 1;
            c = b'\n';
        }
        if quote == 3 && c == b'\n' {
            line_start = true;
        }
        if c != b'\\' {
            out.push(c);
            i += 1;
            continue;
        }
        i += 1;
        debug_assert!(i < end);
        c = bytes[i];
        if c != b'u' {
            out.push(match c {
                b'n' => b'\n',
                b't' => b'\t',
                b'r' => b'\r',
                other => other,
            });
            i += 1;
            continue;
        }
        i += 1;
        debug_assert!(bytes[i] == b'{');
        let mut scalar = 0u32;
        i += 1;
        while i < end && bytes[i] != b'}' {
            scalar = scalar * 16 + hex_digit(bytes[i]);
            i += 1;
        }
        i += 1;
        if scalar < 0x80 {
            out.push(scalar as u8);
        } else {
            let extra = if scalar < 0x800 {
                1u32
            } else if scalar < 0x10000 {
                2u32
            } else {
                3u32
            };
            let lead = match extra {
                1 => 0xc0u8,
                2 => 0xe0u8,
                _ => 0xf0u8,
            };
            out.push(lead | ((scalar >> (6 * extra)) as u8));
            for j in (1..=extra).rev() {
                out.push(0x80u8 | (((scalar >> (6 * (j - 1))) & 63) as u8));
            }
        }
    }
    out
}

pub(crate) fn check_imports(
    modules: &ModuleSet,
    signatures: &mut Signatures,
    diags: &mut DiagBag,
) -> bool {
    let mut valid = true;
    for i in 0..signatures.imports.len() {
        let (used, parent, origin, origin_span, module) = {
            let u = &signatures.imports[i];
            (u.used, u.parent, u.origin, u.origin_span, u.module)
        };
        if signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            let _ = report(
                diags,
                ErrorCode::CheckProgramWork,
                origin_span,
                "the program needs too much import-checking work",
                None,
            );
            return false;
        }
        signatures.work += 1;
        let tree = &modules.module(module).syntax.tree;
        let origin_node = tree.node(origin);
        let has_list = parent == i && origin_node.children.get(1).copied().flatten().is_some();
        let starts_underscore =
            parent != i && tree.node_text(origin).and_then(|b| b.first()) == Some(&b'_');
        if used || has_list || starts_underscore {
            continue;
        }
        let span = if parent == i {
            origin_node
                .children
                .first()
                .copied()
                .flatten()
                .map_or(origin_span, |c0| tree.node(c0).span)
        } else {
            origin_span
        };
        let msg = if parent == i {
            "an imported module is unused"
        } else {
            "an imported name is unused"
        };
        if !report(
            diags,
            ErrorCode::UnusedImport,
            span,
            msg,
            Some("Unused imports are errors (reference 6.1)."),
        ) && (diags.len() as u64) < MO_LIMIT_DIAGNOSTICS
        {
            return false;
        }
        valid = false;
    }
    valid
}

#[expect(
    clippy::too_many_arguments,
    reason = "spec checking mirrors C mo_check_specs and threads shared checker state"
)]
fn check_specs(
    sources: &SourceMap,
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: &mut crate::infer::ConstantContext,
    services_view: ProgramServicesView<'_>,
    functions: &[CheckedFunction],
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> Option<Vec<CheckedSpec>> {
    let mut specs: Vec<CheckedSpec> = Vec::new();
    let mut anchor_parent_raw: Vec<Option<usize>> = Vec::new();

    for (i, func) in functions.iter().enumerate() {
        let sig_id = func.signature;
        let sym = interfaces.symbol(func.symbol);
        let mod_id = sym.module;
        let tree = &modules.module(mod_id).syntax.tree;
        let Some(written_nid) = tree
            .node(sym.declaration)
            .children
            .get(4)
            .copied()
            .flatten()
        else {
            continue;
        };
        let written_children = tree.node(written_nid).children.clone();
        let mut example_number = 0u32;
        let mut promise_number = 0u32;

        for ch in written_children {
            let item = ch?;
            let item_kind = tree.node(item).kind;
            if item_kind == SyntaxKind::Examples {
                let lines_nid = tree.node(item).children.first().copied().flatten()?;
                let lines_children = tree.node(lines_nid).children.clone();
                for lch in lines_children {
                    let line = lch?;
                    example_number += 1;
                    let text = spec_copy_text(sources, tree.node(line).span)?;
                    let mut spec = CheckedSpec {
                        kind: CheckedSpecKind::Example,
                        syntax: line,
                        module: mod_id,
                        number: example_number,
                        subject: Some(CheckedFuncId(i as u32)),
                        promise: None,
                        text,
                        wording: None,
                        parameters: Vec::new(),
                        body: CheckedBody::default(),
                        arguments: Vec::new(),
                        abilities: Vec::new(),
                        values: Vec::new(),
                        conditions: Vec::new(),
                        allows: false,
                    };
                    let enclosing = fn_enclosing_for_sig(modules, interfaces, signatures, sig_id)?;
                    let mut halt = false;
                    if !check_spec_body(
                        modules,
                        interfaces,
                        signatures,
                        abilities_arena,
                        Some(constants),
                        Some(services_view),
                        enclosing,
                        &mut spec,
                        None,
                        diags,
                        arena_remaining,
                        &mut halt,
                    ) && halt
                    {
                        return None;
                    }
                    specs.push(spec);
                    anchor_parent_raw.push(None);
                }
                continue;
            }
            debug_assert_eq!(item_kind, SyntaxKind::Promise);
            if let Some(on_nid) = tree.node(item).children.get(3).copied().flatten() {
                let _ = report(
                    diags,
                    ErrorCode::ServicePromise,
                    tree.node(on_nid).span,
                    "a tool promise is outside a service block",
                    Some(
                        "A promise on a tool is written in the service block, after token \
                         (reference 12.2).",
                    ),
                );
                return None;
            }
            promise_number += 1;
            let promise_raw_idx = specs.len();
            let item_node = tree.node(item);
            let wording_node = item_node.children.first().copied().flatten();
            let header_end = match wording_node {
                Some(wnid) => tree.node(wnid).span.end,
                None => item_node.span.start.saturating_add(7),
            };
            let header = Span::new(item_node.span.file, item_node.span.start, header_end);
            let text = spec_copy_text(sources, header)?;
            let wording = wording_node.map(|wnid| spec_wording(tree, wnid));
            let lines_nid = item_node.children.get(1).copied().flatten()?;
            let lines_children = tree.node(lines_nid).children.clone();
            let mut conditions = Vec::with_capacity(lines_children.len());
            for lch in &lines_children {
                let lnid = (*lch)?;
                let ctext = spec_copy_text(sources, tree.node(lnid).span)?;
                conditions.push(CheckedCondition {
                    text: ctext,
                    expression: crate::ids::CheckedExprId(u32::MAX),
                });
            }
            let mut promise_spec = CheckedSpec {
                kind: CheckedSpecKind::Promise,
                syntax: item,
                module: mod_id,
                number: promise_number,
                subject: Some(CheckedFuncId(i as u32)),
                promise: None,
                text,
                wording,
                parameters: Vec::new(),
                body: CheckedBody::default(),
                arguments: Vec::new(),
                abilities: Vec::new(),
                values: Vec::new(),
                conditions: Vec::new(),
                allows: false,
            };
            let enclosing = fn_enclosing_for_sig(modules, interfaces, signatures, sig_id)?;
            let mut halt = false;
            let ok = check_spec_body(
                modules,
                interfaces,
                signatures,
                abilities_arena,
                Some(constants),
                Some(services_view),
                enclosing.clone(),
                &mut promise_spec,
                Some(&mut conditions),
                diags,
                arena_remaining,
                &mut halt,
            );
            promise_spec.conditions = conditions;
            specs.push(promise_spec);
            anchor_parent_raw.push(None);
            if !ok {
                if halt {
                    return None;
                }
                continue;
            }
            let anchors_nid = item_node.children.get(2).copied().flatten()?;
            let anchor_children = tree.node(anchors_nid).children.clone();
            let mut allows_n = 0u32;
            let mut forbids_n = 0u32;
            for ach in anchor_children {
                let line = ach?;
                let allow = tree.node(line).token == TokenKind::Allows;
                let num = if allow {
                    allows_n += 1;
                    allows_n
                } else {
                    forbids_n += 1;
                    forbids_n
                };
                let atext = spec_copy_text(sources, tree.node(line).span)?;
                let mut anchor_spec = CheckedSpec {
                    kind: CheckedSpecKind::Anchor,
                    syntax: line,
                    module: mod_id,
                    number: num,
                    subject: Some(CheckedFuncId(i as u32)),
                    promise: Some(CheckedSpecId(promise_raw_idx as u32)),
                    text: atext,
                    wording: None,
                    parameters: Vec::new(),
                    body: CheckedBody::default(),
                    arguments: Vec::new(),
                    abilities: Vec::new(),
                    values: Vec::new(),
                    conditions: Vec::new(),
                    allows: allow,
                };
                if !check_spec_body(
                    modules,
                    interfaces,
                    signatures,
                    abilities_arena,
                    Some(constants),
                    Some(services_view),
                    enclosing.clone(),
                    &mut anchor_spec,
                    None,
                    diags,
                    arena_remaining,
                    &mut halt,
                ) && halt
                {
                    return None;
                }
                specs.push(anchor_spec);
                anchor_parent_raw.push(Some(promise_raw_idx));
            }
        }
    }

    let unit_ty = signatures.types.intern_builtin("Unit");
    let test_fn_ty = signatures.types.intern(
        SourceTypeKind::Function,
        "fn",
        None,
        TypeOwner::None,
        vec![unit_ty],
    );
    for m in 0..modules.len() {
        let mid = ModuleId(m as u32);
        let tree = &modules.modules[m].syntax.tree;
        let Some(items_nid) = tree
            .node(modules.modules[m].syntax.root)
            .children
            .get(1)
            .copied()
            .flatten()
        else {
            continue;
        };
        let items_children = tree.node(items_nid).children.clone();
        let mut number = 0u32;
        for ch in items_children {
            let item = ch?;
            if tree.node(item).kind != SyntaxKind::Test {
                continue;
            }
            number += 1;
            let name_nid = tree.node(item).children.first().copied().flatten()?;
            let item_span = tree.node(item).span;
            let header = Span::new(
                item_span.file,
                item_span.start,
                tree.node(name_nid).span.end,
            );
            let text = spec_copy_text(sources, header)?;
            let wording = Some(spec_wording(tree, name_nid));
            let mut spec = CheckedSpec {
                kind: CheckedSpecKind::Test,
                syntax: item,
                module: mid,
                number,
                subject: None,
                promise: None,
                text,
                wording,
                parameters: Vec::new(),
                body: CheckedBody::default(),
                arguments: Vec::new(),
                abilities: Vec::new(),
                values: Vec::new(),
                conditions: Vec::new(),
                allows: false,
            };
            let enclosing = EnclosingContext {
                module: mid,
                owner_symbol: None,
                func_sig_id: None,
                declaration: item,
                owner_kind: SyntaxKind::Test,
                fn_type: test_fn_ty,
                requirements: [0; 26],
                param_names: Vec::new(),
            };
            let mut halt = false;
            if !check_spec_body(
                modules,
                interfaces,
                signatures,
                abilities_arena,
                Some(constants),
                Some(services_view),
                enclosing,
                &mut spec,
                None,
                diags,
                arena_remaining,
                &mut halt,
            ) && halt
            {
                return None;
            }
            specs.push(spec);
            anchor_parent_raw.push(None);
        }
    }

    if diags.has_any() {
        return None;
    }

    // Sort specs in source order `(module, syntax_span.start)` and remap anchor `promise` IDs.
    let mut order: Vec<usize> = (0..specs.len()).collect();
    order.sort_by(|&a, &b| {
        let sa = &specs[a];
        let sb = &specs[b];
        let span_a = modules
            .module(sa.module)
            .syntax
            .tree
            .node(sa.syntax)
            .span
            .start;
        let span_b = modules
            .module(sb.module)
            .syntax
            .tree
            .node(sb.syntax)
            .span
            .start;
        sa.module.cmp(&sb.module).then_with(|| span_a.cmp(&span_b))
    });
    let mut new_index = vec![0usize; specs.len()];
    for (pos, &old) in order.iter().enumerate() {
        new_index[old] = pos;
    }
    let mut ordered = Vec::with_capacity(specs.len());
    let mut slots: Vec<Option<CheckedSpec>> = specs.into_iter().map(Some).collect();
    for &old in &order {
        let mut s = slots[old].take()?;
        if let Some(parent_old) = anchor_parent_raw[old] {
            s.promise = Some(CheckedSpecId(new_index[parent_old] as u32));
        }
        ordered.push(s);
    }
    Some(ordered)
}

/// Runs the complete Mo typechecker over a loaded [`ModuleSet`] (`mo_check` / `check_program`).
///
/// Always sorts diagnostics before returning. Returns `Some(CheckedProgram)` iff checking
/// succeeded with zero diagnostics.
#[must_use]
pub fn check_program(
    sources: Option<&SourceMap>,
    modules: ModuleSet,
    diags: &mut DiagBag,
) -> Option<CheckedProgram> {
    let out = check_program_inner(sources, modules, diags);
    diags.sort();
    out
}

fn check_program_inner(
    sources: Option<&SourceMap>,
    modules: ModuleSet,
    diags: &mut DiagBag,
) -> Option<CheckedProgram> {
    debug_assert!(!modules.modules.is_empty());
    if diags.has_any() {
        return None;
    }
    let mut arena_remaining = modules.arena_remaining;
    let interfaces = resolve_modules(&modules, diags)?;
    let mut signatures = prepare_available_signatures(&modules, &interfaces, diags)?;
    // Account for C's `mo_resolve_modules` + `mo_prepare_signatures` allocations from the shared
    // checking arena (`MO_CLI_CHECK_SIZE`).
    let mut pre_check_bytes = 0usize;
    for scope in &interfaces.scopes {
        pre_check_bytes = pre_check_bytes
            .saturating_add(scope.symbols.len().saturating_mul(96))
            .saturating_add(scope.imported.len().saturating_mul(48))
            .saturating_add(scope.aliases.len().saturating_mul(32));
    }
    pre_check_bytes = pre_check_bytes
        .saturating_add(signatures.functions.len().saturating_mul(128))
        .saturating_add(signatures.data.len().saturating_mul(64))
        .saturating_add(signatures.nominals.len().saturating_mul(64))
        .saturating_add(signatures.native.len().saturating_mul(80))
        .saturating_add(signatures.types.len().saturating_mul(72));
    arena_remaining = arena_remaining.saturating_sub(pre_check_bytes);
    let mut abilities_arena = AbilityArena::new();

    // Signature-level ability checks (functions + data fields).
    for i in 0..signatures.functions.len() {
        let (fn_sym, fn_ty_opt) = {
            let f = &signatures.functions[i];
            (f.function, f.ty)
        };
        let Some(fn_ty_id) = fn_ty_opt else {
            continue;
        };
        if !signatures.signature_type_available(Some(fn_ty_id)) {
            signatures.functions[i].ty = None;
            continue;
        }
        let sym = interfaces.symbol(fn_sym);
        let tree = &modules.module(sym.module).syntax.tree;
        let decl = tree.node(sym.declaration);
        let params_nid = decl.children.get(1).copied().flatten();
        let ret_nid = decl.children.get(2).copied().flatten();
        let param_count = params_nid.map_or(0, |p| tree.node(p).children.len());
        let fn_args = signatures.types.get(fn_ty_id).arguments.clone();
        for (j, &arg_ty) in fn_args.iter().enumerate() {
            let ann = if j < param_count {
                params_nid
                    .and_then(|p| tree.node(p).children[j])
                    .and_then(|pn| tree.node(pn).children.get(1).copied().flatten())
            } else {
                ret_nid
            };
            let span = ann.map_or(sym.name_span, |a| tree.node(a).span);
            let before = diags.len();
            if !check_type_abilities_decl(
                &mut abilities_arena,
                &interfaces,
                &mut signatures,
                arg_ty,
                span,
                diags,
            ) {
                return None;
            }
            if diags.len() != before {
                signatures.functions[i].ty = None;
                signatures.recovering = true;
                break;
            }
        }
    }

    signatures
        .customizers
        .retain(|&sig_id| signatures.functions[sig_id.index()].ty.is_some());

    for i in 0..signatures.data.len() {
        let (data_sym, data_ty, field_info): (SymRef, TypeId, Vec<(TypeId, Span)>) = {
            let d = &signatures.data[i];
            let fields = (0..d.fields.len())
                .map(|j| {
                    let f = d.field_by_index(j);
                    (f.ty, f.annotation_span)
                })
                .collect();
            (d.symbol, d.ty, fields)
        };
        let sym_span = interfaces.symbol(data_sym).name_span;
        if !check_type_abilities_decl(
            &mut abilities_arena,
            &interfaces,
            &mut signatures,
            data_ty,
            sym_span,
            diags,
        ) {
            return None;
        }
        for (fty, fspan) in field_info {
            if !check_type_abilities_decl(
                &mut abilities_arena,
                &interfaces,
                &mut signatures,
                fty,
                fspan,
                diags,
            ) {
                return None;
            }
        }
    }

    // Count specs and validate sources presence + spec work budget.
    for m in 0..modules.len() {
        let tree = &modules.modules[m].syntax.tree;
        let Some(items_nid) = tree
            .node(modules.modules[m].syntax.root)
            .children
            .get(1)
            .copied()
            .flatten()
        else {
            continue;
        };
        for &ch in &tree.node(items_nid).children {
            let Some(item) = ch else {
                continue;
            };
            let kind = tree.node(item).kind;
            if kind == SyntaxKind::Test && sources.is_none() {
                let _ = report(
                    diags,
                    ErrorCode::CheckUnfinished,
                    tree.node(item).span,
                    "specifications need their original sources",
                    None,
                );
                return None;
            }
            if kind != SyntaxKind::Fn {
                continue;
            }
            let Some(specs_nid) = tree.node(item).children.get(4).copied().flatten() else {
                continue;
            };
            for &sch in &tree.node(specs_nid).children {
                let Some(spec) = sch else {
                    continue;
                };
                if sources.is_none() {
                    let _ = report(
                        diags,
                        ErrorCode::CheckUnfinished,
                        tree.node(spec).span,
                        "this specification rule is not implemented",
                        None,
                    );
                    return None;
                }
                let lines = if tree.node(spec).kind == SyntaxKind::Examples {
                    tree.node(spec)
                        .children
                        .first()
                        .copied()
                        .flatten()
                        .map_or(0u64, |c| tree.node(c).children.len() as u64)
                } else {
                    1u64 + tree
                        .node(spec)
                        .children
                        .get(2)
                        .copied()
                        .flatten()
                        .map_or(0u64, |c| tree.node(c).children.len() as u64)
                };
                if lines > MO_LIMIT_PROGRAM_CHECK_WORK.saturating_sub(signatures.work) {
                    let _ = report(
                        diags,
                        ErrorCode::CheckProgramWork,
                        tree.node(spec).span,
                        "the program needs too much spec-checking work",
                        None,
                    );
                    return None;
                }
                signatures.work += lines;
            }
        }
    }

    // Order functions by `(module, decl_span.start)`.
    let fn_count = signatures.functions.len();
    let mut order: Vec<FuncSigId> = (0..fn_count).map(|i| FuncSigId(i as u32)).collect();
    order.sort_by(|&a, &b| {
        let sa = interfaces.symbol(signatures.functions[a.index()].function);
        let sb = interfaces.symbol(signatures.functions[b.index()].function);
        sa.module
            .cmp(&sb.module)
            .then_with(|| sa.declaration_span.start.cmp(&sb.declaration_span.start))
    });
    let mut sig_to_func = vec![CheckedFuncId(0); fn_count];
    for (func_idx, &sig_id) in order.iter().enumerate() {
        sig_to_func[sig_id.index()] = CheckedFuncId(func_idx as u32);
    }

    // Constants
    let mut constants = check_constants(
        &modules,
        &interfaces,
        &mut signatures,
        &mut abilities_arena,
        diags,
        &mut arena_remaining,
    )?;

    // Services, actors, APIs (only when signatures are clean, matching C `!signatures->recovering`).
    let mut services = Vec::new();
    let mut apis = Vec::new();
    let mut has_service_program = false;
    if !signatures.recovering {
        services = prepare_services(
            &modules,
            &interfaces,
            &mut signatures,
            &mut abilities_arena,
            &sig_to_func,
            diags,
            &mut arena_remaining,
        )?;
        apis = check_apis(
            &modules,
            &interfaces,
            &mut signatures,
            &mut abilities_arena,
            &services,
            &sig_to_func,
            diags,
            &mut arena_remaining,
        )?;
        has_service_program = true;
    }

    // Check every top-level function body.
    let mut functions: Vec<CheckedFunction> = order
        .iter()
        .map(|&sig_id| CheckedFunction {
            signature: sig_id,
            symbol: signatures.functions[sig_id.index()].function,
            parameters: Vec::new(),
            body: CheckedBody::default(),
        })
        .collect();
    let mut main_idx: Option<usize> = None;
    for (i, &sig_id) in order.iter().enumerate() {
        if signatures.functions[sig_id.index()].ty.is_none() {
            continue;
        }
        let fn_sym = signatures.functions[sig_id.index()].function;
        let sym = interfaces.symbol(fn_sym);
        if sym.module == ModuleId(0) && sym.name == "main" {
            main_idx = Some(i);
        }
        let view = has_service_program.then_some(ProgramServicesView {
            services: &services,
            apis: &apis,
            function_sigs: &order,
        });
        let mut halt = false;
        if !check_function(
            &modules,
            &interfaces,
            &mut signatures,
            &mut abilities_arena,
            Some(&mut constants),
            view,
            sig_id,
            diags,
            &mut arena_remaining,
            &mut functions[i],
            &mut halt,
        ) && halt
        {
            return None;
        }
    }
    diags.sort();
    if diags.has_any() {
        return None;
    }

    // Check `main` entry contract (`E0079` / `E0080`).
    let mut checked_entry = CheckedEntry::default();
    if let Some(m_idx) = main_idx {
        let entry = &functions[m_idx];
        let entry_sym = interfaces.symbol(entry.symbol);
        let entry_span = entry_sym.name_span;
        let mut capabilities = 0u32;
        for (i, &param_bid) in entry.parameters.iter().enumerate() {
            let ptype = entry.body.binding(param_bid).ty;
            let pt = signatures.types.get(ptype);
            if pt.kind == SourceTypeKind::Named
                && let Some(owner_sym) = pt.owner.as_source()
                && interfaces.symbol(owner_sym).decl_kind == SyntaxKind::Actor
            {
                let duplicate = entry.parameters[..i].iter().any(|&prev_bid| {
                    let prev_ty = entry.body.binding(prev_bid).ty;
                    signatures.types.get(prev_ty).owner.as_source() == Some(owner_sym)
                });
                if duplicate {
                    let _ = report(
                        diags,
                        ErrorCode::MainContract,
                        entry_span,
                        "main has an invalid signature",
                        Some(
                            "main lists capabilities and actors, at most one of each type \
                             (reference 11.2, 11.7).",
                        ),
                    );
                    return None;
                }
                continue;
            }
            let mut cap_bit = 0u32;
            for (j, &cap_name) in CAPABILITY_NAMES.iter().enumerate() {
                if !matches!(
                    cap_name,
                    "Simulation" | "Connection" | "Sqlite.Database" | "Files.File" | "ApiTest.App"
                ) && pt.is_builtin_named(cap_name)
                {
                    cap_bit = 1u32 << j;
                }
            }
            if cap_bit == 0 || (capabilities & cap_bit) != 0 {
                let _ = report(
                    diags,
                    ErrorCode::MainContract,
                    entry_span,
                    "main has an invalid signature",
                    Some("main lists capabilities, at most one of each type (reference 11.2)."),
                );
                return None;
            }
            capabilities |= cap_bit;
        }
        let fn_ty_id = signatures.functions[entry.signature.index()].ty?;
        let fn_args = signatures.types.get(fn_ty_id).arguments.clone();
        let result_ty_id = fn_args[fn_args.len() - 1];
        let result_ty = signatures.types.get(result_ty_id);
        let is_unit = result_ty.is_builtin_named("Unit");
        let is_status = result_ty.is_builtin_named("Int");
        let is_wrapped = result_ty.is_builtin_named("Result")
            && result_ty.arguments.len() == 2
            && signatures
                .types
                .get(result_ty.arguments[0])
                .is_builtin_named("Unit")
            && signatures.types.get(result_ty.arguments[1]).parameters == 0;
        if !is_unit && !is_status && !is_wrapped {
            let _ = report(
                diags,
                ErrorCode::MainContract,
                entry_span,
                "main has an invalid signature",
                Some("main returns nothing, an Int, or Result(Unit, E) (reference 11.2)."),
            );
            return None;
        }
        let error_text = if is_wrapped {
            let err_ty_id = signatures.types.get(result_ty_id).arguments[1];
            let arena = std::mem::take(&mut abilities_arena);
            let mut ctx = AbilityContext::with_arena(arena, &interfaces, &mut signatures, 0);
            let proof = ctx.prove_ability(err_ty_id, SourceAbility::Text);
            let (arena, _cache, _work, err) = ctx.into_parts();
            abilities_arena = arena;
            let Some(pid) = proof else {
                let decl_node = modules
                    .module(entry_sym.module)
                    .syntax
                    .tree
                    .node(entry_sym.declaration);
                let span = decl_node
                    .children
                    .get(2)
                    .copied()
                    .flatten()
                    .map_or(entry_span, |a| {
                        modules.module(entry_sym.module).syntax.tree.node(a).span
                    });
                let (code, msg, rsn) = match err {
                    Some(f) => (f.code, f.message, f.reason),
                    None => (
                        ErrorCode::CheckWork,
                        "the function needs too much ability-checking work".to_string(),
                        None,
                    ),
                };
                let _ = report(diags, code, span, msg, rsn);
                return None;
            };
            Some(pid)
        } else {
            None
        };
        checked_entry = CheckedEntry {
            function: Some(CheckedFuncId(m_idx as u32)),
            result: if is_unit {
                CheckedEntryResult::Unit
            } else if is_status {
                CheckedEntryResult::Status
            } else {
                CheckedEntryResult::Result
            },
            capabilities: entry.parameters.clone(),
            error_text,
        };
    }

    // Specs (`test`, `example`, `promise`, `anchor`).
    let specs = if let Some(src) = sources {
        let view = ProgramServicesView {
            services: &services,
            apis: &apis,
            function_sigs: &order,
        };
        check_specs(
            src,
            &modules,
            &interfaces,
            &mut signatures,
            &mut abilities_arena,
            &mut constants,
            view,
            &functions,
            diags,
            &mut arena_remaining,
        )?
    } else {
        Vec::new()
    };

    // Services initializers, bare-State `E0103` rewrite fixes, timer intervals, visibility.
    let raw_constants = crate::infer::bodies::collect_checked_constants(
        &constants,
        &(0..constants.items.len()).collect::<Vec<_>>(),
    );
    if !check_services(
        sources,
        &modules,
        &interfaces,
        &mut signatures,
        &mut abilities_arena,
        &mut constants,
        &mut services,
        &apis,
        &order,
        &functions,
        &raw_constants,
        &specs,
        diags,
        &mut arena_remaining,
    ) {
        return None;
    }

    // Service & actor promises.
    if let Some(src) = sources
        && !check_service_promises(
            src,
            &modules,
            &interfaces,
            &mut signatures,
            &mut abilities_arena,
            &mut constants,
            &mut services,
            &apis,
            &order,
            diags,
            &mut arena_remaining,
        )
    {
        return None;
    }

    // Polymorphic recursion check (`E0084` / `E0059`).
    if !check_recursion(
        &modules,
        &interfaces,
        &mut signatures,
        &abilities_arena,
        &functions,
        &specs,
        diags,
    ) {
        return None;
    }

    // Generic capability-capture flow (`E0090`).
    if !check_capture_arguments(
        &modules,
        &interfaces,
        &mut signatures,
        &abilities_arena,
        &functions,
        &raw_constants,
        &specs,
        &services,
        diags,
    ) {
        return None;
    }

    // Unused imports audit (`E0093`).
    if !check_imports(&modules, &mut signatures, diags) {
        return None;
    }

    // Constant dependency topological sort (`E0089` / `E1103`).
    let ordered_constants = order_constants(
        &modules,
        &interfaces,
        &mut signatures,
        &abilities_arena,
        &functions,
        &constants,
        diags,
        &mut arena_remaining,
    )?;

    Some(CheckedProgram {
        modules,
        interfaces,
        signatures,
        abilities: abilities_arena,
        functions,
        constants: ordered_constants,
        specs,
        services,
        apis,
        entry: checked_entry,
    })
}
