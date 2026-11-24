//! Service & actor promise and anchor checking (`src/check/service-promises.c` parity).

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::ErrorCode;
use mo_syntax::{NodeId, SyntaxKind, TokenKind};

use crate::abilities::AbilityArena;
use crate::checked::{
    CheckedApi, CheckedBody, CheckedCondition, CheckedService, CheckedServicePromise,
    CheckedServiceTool, CheckedSpec, CheckedSpecKind, ServiceReply,
};
use crate::ids::{FuncSigId, TypeId};
use crate::infer::{
    ConstantContext, EnclosingContext, NameRef, ProgramServicesView, check_spec_body,
};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::program::{spec_copy_text, spec_wording};
use crate::signatures::Signatures;
use crate::types::{SourceTypeKind, TypeOwner};

fn refuse(
    diags: &mut DiagBag,
    code: ErrorCode,
    span: Span,
    message: &'static str,
    reason: Option<&'static str>,
) -> bool {
    let mut diag = Diagnostic::new(code, span, message).with_primary_text(message);
    if let Some(r) = reason {
        diag = diag.with_reason(r);
    }
    let _ = diags.push(diag);
    false
}

fn is_reserved_word(bytes: &[u8]) -> bool {
    matches!(bytes, b"before" | b"after" | b"now" | b"reply")
}

fn reply_type(signatures: &mut Signatures, tool: &CheckedServiceTool, result: TypeId) -> TypeId {
    match tool.reply {
        ServiceReply::ReadOnly => result,
        ServiceReply::Update => signatures.types.get(result).arguments[1],
        ServiceReply::ResultUpdate => {
            let res_args = signatures.types.get(result).arguments.clone();
            let ok = signatures.types.get(res_args[0]).arguments[1];
            let err = res_args[1];
            signatures
                .types
                .intern_builtin_applied("Result", vec![ok, err])
        }
    }
}

fn build_service_promise_context(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    function_sigs: &[FuncSigId],
    service: &CheckedService,
    item: NodeId,
    tool: Option<usize>,
) -> EnclosingContext {
    let tree = &modules.module(service.module).syntax.tree;
    let at = tree.node(item).span;
    let time_ty = signatures.types.intern_builtin("Time");
    let bool_ty = signatures.types.intern_builtin("Bool");

    let mut param_names: Vec<NameRef> = vec![
        NameRef::Synthetic {
            name: "before",
            span: Span::new(at.file, at.start, at.start.saturating_add(6)),
        },
        NameRef::Synthetic {
            name: "after",
            span: Span::new(at.file, at.start, at.start.saturating_add(5)),
        },
        NameRef::Synthetic {
            name: "now",
            span: Span::new(at.file, at.start, at.start.saturating_add(3)),
        },
    ];
    let mut types: Vec<TypeId> = vec![service.state, service.state, time_ty];

    if let Some(ti) = tool {
        let tool_info = &service.tools[ti];
        let sig_id = function_sigs[tool_info.function.index()];
        let sig = &signatures.functions[sig_id.index()];
        let fn_sym = sig.function;
        if let Some(handler_ty_id) = sig.ty {
            let handler_args = signatures.types.get(handler_ty_id).arguments.clone();
            let decl_nid = interfaces.symbol(fn_sym).declaration;
            if let Some(params_nid) = tree.node(decl_nid).children.get(1).copied().flatten() {
                let pchildren = &tree.node(params_nid).children;
                for (i, &arg_ty) in handler_args
                    .iter()
                    .enumerate()
                    .take(handler_args.len() - 1)
                    .skip(2)
                {
                    if let Some(pnode) = pchildren.get(i).copied().flatten()
                        && let Some(pname) = tree.node(pnode).children.first().copied().flatten()
                    {
                        param_names.push(NameRef::Syntax(pname));
                        types.push(arg_ty);
                    }
                }
            }
            param_names.push(NameRef::Synthetic {
                name: "reply",
                span: Span::new(at.file, at.start, at.start.saturating_add(5)),
            });
            let rty = reply_type(signatures, tool_info, handler_args[handler_args.len() - 1]);
            types.push(rty);
        }
    }

    types.push(bool_ty);
    let fn_ty =
        signatures
            .types
            .intern(SourceTypeKind::Function, "fn", None, TypeOwner::None, types);
    EnclosingContext {
        module: service.module,
        owner_symbol: None,
        func_sig_id: None,
        declaration: item,
        owner_kind: SyntaxKind::Fn,
        fn_type: fn_ty,
        requirements: [0; 26],
        param_names,
    }
}

fn named_tool(modules: &ModuleSet, service: &CheckedService, name: NodeId) -> Option<usize> {
    let tree = &modules.module(service.module).syntax.tree;
    let target = tree.node_text(name)?;
    for (i, tool) in service.tools.iter().enumerate() {
        if tree.node_text(tool.name) == Some(target) {
            return Some(i);
        }
    }
    None
}

#[expect(
    clippy::too_many_arguments,
    reason = "service promise checking mirrors C mo_check_service_promises and threads shared checker state"
)]
fn check_one_promise(
    sources: &SourceMap,
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: &mut ConstantContext,
    services: &[CheckedService],
    apis: &[CheckedApi],
    function_sigs: &[FuncSigId],
    service_idx: usize,
    item: NodeId,
    number: u32,
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
    halt: &mut bool,
) -> Option<CheckedServicePromise> {
    *halt = true;
    let service = &services[service_idx];
    let tree = &modules.module(service.module).syntax.tree;
    let item_node = tree.node(item);
    let on = item_node.children.get(3).copied().flatten();
    let tool = on.and_then(|o| named_tool(modules, service, o));
    if let Some(on_nid) = on
        && tool.is_none()
    {
        *halt = false;
        let _ = refuse(
            diags,
            ErrorCode::ServicePromise,
            tree.node(on_nid).span,
            "a service promise names no tool of its service",
            Some("on names one of the tools in this service block (reference 12.2)."),
        );
        return None;
    }
    if let Some(ti) = tool {
        let sig_id = function_sigs[service.tools[ti].function.index()];
        let fn_sym = signatures.functions[sig_id.index()].function;
        let decl_nid = interfaces.symbol(fn_sym).declaration;
        if let Some(params_nid) = tree.node(decl_nid).children.get(1).copied().flatten() {
            for &ch in tree.node(params_nid).children.iter().skip(2) {
                if let Some(pnode) = ch
                    && let Some(pname) = tree.node(pnode).children.first().copied().flatten()
                    && let Some(bytes) = tree.node_text(pname)
                    && is_reserved_word(bytes)
                {
                    *halt = false;
                    let _ = refuse(
                        diags,
                        ErrorCode::ServicePromise,
                        tree.node(pname).span,
                        "a tool parameter has a name a service promise reads",
                        Some(
                            "A tool promise reads before, after, now and reply, so its tool's \
                             parameters need other names (reference 12.2).",
                        ),
                    );
                    return None;
                }
            }
        }
    }

    let enclosing = build_service_promise_context(
        modules,
        interfaces,
        signatures,
        function_sigs,
        service,
        item,
        tool,
    );

    let wording_node = item_node.children.first().copied().flatten();
    let header_end = if let Some(on_nid) = on {
        tree.node(on_nid).span.end
    } else if let Some(wnid) = wording_node {
        tree.node(wnid).span.end
    } else {
        item_node.span.start.saturating_add(7)
    };
    let header = Span::new(item_node.span.file, item_node.span.start, header_end);
    let lines_nid = item_node.children.get(1).copied().flatten()?;
    let lines_children = &tree.node(lines_nid).children;

    let text = spec_copy_text(sources, header)?;
    let wording = wording_node.map(|wnid| spec_wording(tree, wnid));
    let mut conditions = Vec::with_capacity(lines_children.len());
    for &ch in lines_children {
        let lnid = ch?;
        let ctext = spec_copy_text(sources, tree.node(lnid).span)?;
        conditions.push(CheckedCondition {
            text: ctext,
            expression: crate::ids::CheckedExprId(u32::MAX),
        });
    }

    let mut spec = CheckedSpec {
        kind: CheckedSpecKind::Promise,
        syntax: item,
        module: service.module,
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

    let view = ProgramServicesView {
        services,
        apis,
        function_sigs,
    };
    if !check_spec_body(
        modules,
        interfaces,
        signatures,
        abilities_arena,
        Some(constants),
        Some(view),
        enclosing.clone(),
        &mut spec,
        Some(&mut conditions),
        diags,
        arena_remaining,
        halt,
    ) {
        return None;
    }
    spec.conditions = conditions;

    *halt = true;
    let written_anchors_nid = item_node.children.get(2).copied().flatten()?;
    let anchor_children = &tree.node(written_anchors_nid).children;
    let mut anchors = Vec::with_capacity(anchor_children.len());
    let mut allows_n = 0u32;
    let mut forbids_n = 0u32;
    let mut valid = true;

    for &ch in anchor_children {
        let line = ch?;
        let allow = tree.node(line).token == TokenKind::Allows;
        let num = if allow {
            allows_n += 1;
            allows_n
        } else {
            forbids_n += 1;
            forbids_n
        };
        let atext = spec_copy_text(sources, tree.node(line).span).unwrap_or_default();
        let mut anchor_spec = CheckedSpec {
            kind: CheckedSpecKind::Anchor,
            syntax: line,
            module: service.module,
            number: num,
            subject: None,
            promise: None,
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
            Some(view),
            enclosing.clone(),
            &mut anchor_spec,
            None,
            diags,
            arena_remaining,
            halt,
        ) {
            if *halt {
                return None;
            }
            valid = false;
            *halt = true;
        }
        anchors.push(anchor_spec);
    }

    *halt = false;
    if !valid {
        return None;
    }
    Some(CheckedServicePromise {
        spec,
        tool: tool.map(|t| t as u32),
        anchors,
    })
}

/// Checks every `promise` (and its `allows`/`forbids` anchors) declared inside every `service`
/// and `actor` (`mo_check_service_promises`).
#[expect(
    clippy::too_many_arguments,
    reason = "service promise checking mirrors C mo_check_service_promises and threads shared checker state"
)]
#[must_use]
pub(crate) fn check_service_promises(
    sources: &SourceMap,
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    constants: &mut ConstantContext,
    services: &mut [CheckedService],
    apis: &[CheckedApi],
    function_sigs: &[FuncSigId],
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> bool {
    let service_count = services.len();
    for i in 0..service_count {
        let (module, syntax, is_actor) =
            (services[i].module, services[i].syntax, services[i].is_actor);
        let tree = &modules.module(module).syntax.tree;
        let clauses_nid = tree.node(syntax).children[usize::from(is_actor)];
        let Some(cnid) = clauses_nid else {
            continue;
        };
        let promise_nodes: Vec<NodeId> = tree
            .node(cnid)
            .children
            .iter()
            .filter_map(|&c| c.filter(|&id| tree.node(id).kind == SyntaxKind::Promise))
            .collect();
        if promise_nodes.is_empty() {
            continue;
        }
        let mut checked_promises = Vec::with_capacity(promise_nodes.len());
        for (idx, item) in promise_nodes.into_iter().enumerate() {
            let mut halt = false;
            match check_one_promise(
                sources,
                modules,
                interfaces,
                signatures,
                abilities_arena,
                constants,
                services,
                apis,
                function_sigs,
                i,
                item,
                (idx + 1) as u32,
                diags,
                arena_remaining,
                &mut halt,
            ) {
                Some(p) => checked_promises.push(p),
                None if halt => return false,
                None => {}
            }
        }
        services[i].promises = checked_promises;
    }
    !diags.has_any()
}
