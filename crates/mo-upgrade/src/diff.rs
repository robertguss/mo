//! Structural diffing (`mo diff [--json] [--lib DIR] FILE [FILE2]`, `src/tools/diff.c` parity).
//!
//! Compares two versions of a Mo program (or Git `HEAD` vs working tree in single-argument mode)
//! across four axes:
//! 1. Authority (capabilities and attenuations),
//! 2. Public & wire surface (types, actors, functions, constants, service tools/timers, API routes),
//! 3. Durable service state schema (`migration_required`),
//! 4. Behavioral contracts (splicing OLD promises/examples/service promises plus any private
//!    helpers they call into NEW and executing them via `mo-test`).

use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use mo_base::{
    DiagBag, Diagnostic, DisplayMetrics, RenderFormat, SourceMap, Span, TerminalEncoding,
    render_diagnostic_default,
};
use mo_bytecode::load as load_bytecode;
use mo_check::{
    ApiAuthMode, ApiMethod, CheckedBody, CheckedFuncId, CheckedFunction, CheckedProgram,
    CheckedSpecKind, FunctionSignature, LibraryMode, LoadOptions, ModuleId, ServiceReply,
    SourceTypeKind, SymRef, TypeId, check_program, default_embedded_library, default_library_root,
    is_capability_name, load_bundle_with_library, load_program_bytes,
};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::MO_CLI_CHECK_SIZE;
use mo_heap::saved::write_schema;
use mo_lower::lower_services;
use mo_service::binary::{hex_encode, push_json_string, sanitize_terminal_text, sha256_bytes};
use mo_syntax::{NodeId, SyntaxKind, SyntaxTree};
use mo_test::{
    CheckOutcome, GatedRun, TestConfig, TestTotals, append_function_name, run_gated, select_tests,
};
use mo_vm::Limits;

const UNICODE_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: mo_prim::unicode16::next_grapheme_bytes,
    display_width: mo_prim::unicode16::display_width_bytes,
};

/// Options for `mo diff [--json] [--lib DIR] FILE [FILE2]`.
pub struct DiffOptions<'a> {
    /// Old file path (`None` triggers single-argument Git `HEAD` mode).
    pub old_file: Option<&'a [u8]>,
    /// New file path.
    pub new_file: &'a [u8],
    /// Emit NDJSON output.
    pub json: bool,
    /// Optional library directory (`--lib`).
    pub library: Option<&'a Path>,
    /// CLI version string for NDJSON `version` record.
    pub version: &'a str,
}

struct TempDirGuard {
    path: PathBuf,
}

impl Drop for TempDirGuard {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn run_git_capture(args: &[&str]) -> Option<Vec<u8>> {
    let out = Command::new("git")
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    out.status.success().then_some(out.stdout)
}

fn materialize_git_head(new_path: &Path) -> Option<(TempDirGuard, PathBuf)> {
    let resolved_new = fs::canonicalize(new_path).ok()?;
    let dir_part = resolved_new.parent()?;
    let dir_str = dir_part.to_str()?;

    let top_bytes = run_git_capture(&["-C", dir_str, "rev-parse", "--show-toplevel"])?;
    let top_str = std::str::from_utf8(&top_bytes)
        .ok()?
        .trim_end_matches(['\r', '\n']);
    if top_str.is_empty() {
        return None;
    }
    let top_path = fs::canonicalize(top_str).ok()?;
    let rel_path = resolved_new.strip_prefix(&top_path).ok()?;
    let rel_str = rel_path.to_str()?;
    let rel_posix = rel_str.replace('\\', "/");

    let head_spec = format!("HEAD:{rel_posix}");
    run_git_capture(&["-C", top_str, "cat-file", "-e", &head_spec])?;

    let list_bytes =
        run_git_capture(&["-C", top_str, "ls-tree", "-r", "-z", "--name-only", "HEAD"])?;

    let mut rnd = [0u8; 8];
    let _ = getrandom::fill(&mut rnd);
    let tmp_dir = std::env::temp_dir().join(format!("mo-diff-{:016x}", u64::from_le_bytes(rnd)));
    fs::create_dir_all(&tmp_dir).ok()?;
    let guard = TempDirGuard {
        path: tmp_dir.clone(),
    };

    for entry in list_bytes.split(|&b| b == 0) {
        if entry.is_empty() {
            continue;
        }
        let Ok(name) = std::str::from_utf8(entry) else {
            continue;
        };
        if !name.ends_with(".mo") {
            continue;
        }
        if name.starts_with('/') || name.split('/').any(|seg| seg == ".." || seg == ".") {
            continue;
        }
        let spec = format!("HEAD:{name}");
        let blob = run_git_capture(&["-C", top_str, "show", &spec])?;
        let dest = tmp_dir.join(name);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent).ok()?;
        }
        fs::write(&dest, &blob).ok()?;
    }

    let old_entry = tmp_dir.join(&rel_posix);
    Some((guard, old_entry))
}

struct Side {
    sources: SourceMap,
    diags: DiagBag,
    program: Option<CheckedProgram>,
    entry_service: Option<usize>,
    state_schema: [u8; 32],
    has_schema: bool,
}

fn lib_mode_for(library: Option<&Path>) -> LibraryMode {
    match library {
        Some(dir) => LibraryMode::Directory(dir.to_path_buf()),
        None => LibraryMode::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        },
    }
}

fn load_side(path: &[u8], library: Option<&Path>) -> Side {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let opts = LoadOptions {
        library: lib_mode_for(library),
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let set = load_program_bytes(&mut sources, path, &opts, &mut diags);
    let program = set
        .filter(|_| !diags.has_any())
        .and_then(|s| check_program(Some(&sources), s, &mut diags))
        .filter(|_| !diags.has_any());

    let mut side = Side {
        sources,
        diags,
        program,
        entry_service: None,
        state_schema: [0u8; 32],
        has_schema: false,
    };

    if let Some(ref prog) = side.program {
        side.entry_service = prog
            .services
            .iter()
            .position(|s| s.module == ModuleId(0) && !s.is_actor);
        if let Some(svc_idx) = side.entry_service {
            let mut ldiags = DiagBag::new();
            if let Some(lowered) = lower_services(prog, Some(&side.sources), &mut ldiags)
                && !ldiags.has_any()
            {
                let svcs = (!lowered.services.items.is_empty()).then_some(&lowered.services);
                if check_program_services(&lowered.arena, &lowered.program, svcs, &mut ldiags)
                    && let Some(wire) =
                        compile_services(&lowered.arena, &lowered.program, svcs, &mut ldiags)
                    && let Ok(bc) = load_bytecode(&wire)
                    && let Some(bc_svc) = bc.services.get(svc_idx)
                    && let Some(digest) = {
                        let mut sbuf = Vec::new();
                        write_schema(&bc.types, bc_svc.state_type, &mut sbuf)
                            .ok()
                            .map(|()| sha256_bytes(&sbuf))
                    }
                {
                    side.state_schema = digest;
                    side.has_schema = true;
                }
            }
        }
    }
    side
}

fn format_type(prog: &CheckedProgram, ty: TypeId, out: &mut String) {
    let t = prog.signatures.types.get(ty);
    match t.kind {
        SourceTypeKind::Parameter => out.push_str(&t.name),
        SourceTypeKind::Named => {
            if let Some(mid) = t.module {
                let m = prog.modules.module(mid);
                if !m.library {
                    out.push_str(&m.name);
                    out.push('.');
                }
            }
            out.push_str(&t.name);
            if !t.arguments.is_empty() {
                out.push('(');
                for (i, &arg) in t.arguments.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    format_type(prog, arg, out);
                }
                out.push(')');
            }
        }
        SourceTypeKind::Tuple => {
            out.push('(');
            for (i, &arg) in t.arguments.iter().enumerate() {
                if i > 0 {
                    out.push_str(", ");
                }
                format_type(prog, arg, out);
            }
            out.push(')');
        }
        SourceTypeKind::Function => {
            out.push_str("fn(");
            let params = t.arguments.len().saturating_sub(1);
            for i in 0..params {
                if i > 0 {
                    out.push_str(", ");
                }
                format_type(prog, t.arguments[i], out);
            }
            out.push_str(") : ");
            if let Some(&ret) = t.arguments.last() {
                format_type(prog, ret, out);
            } else {
                out.push_str("Unit");
            }
        }
    }
}

fn format_function_sig(prog: &CheckedProgram, sig: &FunctionSignature, out: &mut String) {
    let Some(ty_id) = sig.ty else {
        out.push_str("() : Unit");
        return;
    };
    let ft = prog.signatures.types.get(ty_id);
    out.push('(');
    let params = ft.arguments.len().saturating_sub(1);
    for i in 0..params {
        if i > 0 {
            out.push_str(", ");
        }
        format_type(prog, ft.arguments[i], out);
    }
    out.push_str(") : ");
    if let Some(&ret) = ft.arguments.last() {
        format_type(prog, ret, out);
    } else {
        out.push_str("Unit");
    }
    let ab_names = ["Equal", "Hash", "Order", "Text", "Number", "Json"];
    let mut any_req = false;
    for (i, &mask) in sig.requirements.iter().enumerate() {
        if mask == 0 {
            continue;
        }
        out.push_str(if any_req { ", " } else { " where " });
        out.push((b'A' + i as u8) as char);
        out.push_str(": ");
        any_req = true;
        let mut first = true;
        for (b, ab) in ab_names.iter().enumerate() {
            if (mask & (1u8 << b)) != 0 {
                if !first {
                    out.push_str(" + ");
                }
                out.push_str(ab);
                first = false;
            }
        }
    }
}

fn collect_caps_in_type(prog: &CheckedProgram, ty: TypeId, caps: &mut BTreeSet<String>) {
    let t = prog.signatures.types.get(ty);
    if t.kind == SourceTypeKind::Named && t.module.is_none() && is_capability_name(&t.name) {
        caps.insert(t.name.clone());
        return;
    }
    for &arg in &t.arguments {
        collect_caps_in_type(prog, arg, caps);
    }
}

fn collect_program_caps(prog: &CheckedProgram) -> Vec<String> {
    let mut caps = BTreeSet::new();
    if let Some(entry_fn) = prog.entry.function {
        let f = prog.function(entry_fn);
        for &bid in &prog.entry.capabilities {
            collect_caps_in_type(prog, f.body.binding(bid).ty, &mut caps);
        }
    }
    for f in &prog.functions {
        let sym = prog.interfaces.symbol(f.symbol);
        if !prog.modules.module(sym.module).library
            && sym.exported
            && let Some(ty) = prog.signatures.functions[f.signature.index()].ty
        {
            collect_caps_in_type(prog, ty, &mut caps);
        }
    }
    for api in &prog.apis {
        if prog.modules.module(api.module).library {
            continue;
        }
        for c in &api.capabilities {
            caps.insert(c.clone());
        }
    }
    caps.into_iter().collect()
}

fn slice_source_span(sources: &SourceMap, span: Span) -> Option<&str> {
    let sf = sources.get(span.file)?;
    let bytes = sf.bytes().get(span.start as usize..span.end as usize)?;
    std::str::from_utf8(bytes).ok()
}

fn format_attenuation_arg(
    sources: &SourceMap,
    tree: &SyntaxTree,
    body: &CheckedBody,
    expr_idx: mo_check::CheckedExprId,
) -> Option<String> {
    let e = body.expr(expr_idx);
    let node = tree.node(e.syntax);
    if node.kind == SyntaxKind::Text {
        return slice_source_span(sources, node.span).map(str::to_string);
    }
    if node.kind == SyntaxKind::List {
        let mut out = String::from("[");
        for (i, &op) in e.operands.iter().enumerate() {
            let item = body.expr(op);
            let inode = tree.node(item.syntax);
            if inode.kind != SyntaxKind::Text {
                return None;
            }
            if i > 0 {
                out.push_str(", ");
            }
            out.push_str(slice_source_span(sources, inode.span)?);
        }
        out.push(']');
        return Some(out);
    }
    None
}

fn scan_attenuations_body(
    sources: &SourceMap,
    tree: &SyntaxTree,
    body: &CheckedBody,
    out: &mut BTreeSet<String>,
) {
    for e in &body.items {
        let Some(ref native) = e.native else {
            continue;
        };
        let is_within = matches!(
            native.as_str(),
            "Files.within" | "ReadFiles.within" | "Command.within"
        );
        let is_ro = native == "Files.read_only";
        let is_hosts = matches!(
            native.as_str(),
            "Network.allow_hosts" | "Command.allow_programs"
        );
        if !is_within && !is_ro && !is_hosts {
            continue;
        }
        let mut entry = format!("{native}(");
        if (is_within || is_hosts) && e.operands.len() >= 2 {
            match format_attenuation_arg(sources, tree, body, e.operands[1]) {
                Some(arg) => entry.push_str(&arg),
                None => entry.push_str("<dynamic>"),
            }
        }
        entry.push(')');
        out.insert(entry);
    }
}

fn collect_attenuations(side: &Side, prog: &CheckedProgram) -> Vec<String> {
    let mut set = BTreeSet::new();
    for f in &prog.functions {
        let sym = prog.interfaces.symbol(f.symbol);
        let m = prog.modules.module(sym.module);
        if !m.library {
            scan_attenuations_body(&side.sources, &m.syntax.tree, &f.body, &mut set);
        }
    }
    for c in &prog.constants {
        let sym = prog.interfaces.symbol(c.symbol);
        let m = prog.modules.module(sym.module);
        if !m.library {
            scan_attenuations_body(&side.sources, &m.syntax.tree, &c.initializer, &mut set);
        }
    }
    set.into_iter().collect()
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct PubItem {
    kind: &'static str,
    name: String,
    sig: String,
}

fn collect_public_surface(side: &Side, prog: &CheckedProgram) -> Vec<PubItem> {
    let mut items = Vec::new();
    for (midx, m) in prog.modules.modules.iter().enumerate() {
        if m.library {
            continue;
        }
        let mid = ModuleId(midx as u32);
        let scope = prog.interfaces.scope(mid);
        for (sidx, sym) in scope.symbols.iter().enumerate() {
            if !sym.exported {
                continue;
            }
            let sym_ref = SymRef::new(mid, mo_check::SymIdx(sidx as u32));
            if sym.decl_kind == SyntaxKind::Actor {
                let name = format!("{}.{}", m.name, sym.name);
                let mut sig = String::from("actor");
                for asvc in &prog.services {
                    if !asvc.is_actor || asvc.actor_symbol != Some(sym_ref) {
                        continue;
                    }
                    sig.push_str(" calls(");
                    for (ti, tl) in asvc.tools.iter().enumerate() {
                        if ti > 0 {
                            sig.push_str(", ");
                        }
                        let tsym = prog.interfaces.symbol(prog.function(tl.function).symbol);
                        sig.push_str(&tsym.name);
                    }
                    sig.push(')');
                }
                items.push(PubItem {
                    kind: "actor",
                    name,
                    sig,
                });
                continue;
            }
            if !matches!(sym.decl_kind, SyntaxKind::Struct | SyntaxKind::Enum) {
                continue;
            }
            let name = format!("{}.{}", m.name, sym.name);
            let mut sig = format!(
                "{} {}",
                if sym.insides { "open" } else { "opaque" },
                if sym.decl_kind == SyntaxKind::Struct {
                    "struct"
                } else {
                    "enum"
                }
            );
            if sym.insides {
                for ds in &prog.signatures.data {
                    let ds_sym = prog.interfaces.symbol(ds.symbol);
                    let belongs = ds.symbol == sym_ref
                        || (ds.symbol.module == mid
                            && ds_sym.owner == Some(mo_check::SymIdx(sidx as u32)));
                    if belongs {
                        let _ = write!(sig, " {}(", ds_sym.name);
                        for (fi, &fidx) in ds.by_index.iter().enumerate() {
                            if fi > 0 {
                                sig.push_str(", ");
                            }
                            let fl = &ds.fields[fidx];
                            if let Some(ref fname) = fl.name {
                                sig.push_str(fname);
                                sig.push_str(": ");
                            }
                            format_type(prog, fl.ty, &mut sig);
                        }
                        sig.push(')');
                    }
                }
            }
            items.push(PubItem {
                kind: "type",
                name,
                sig,
            });
        }
    }

    for f in &prog.functions {
        let sym = prog.interfaces.symbol(f.symbol);
        let m = prog.modules.module(sym.module);
        if m.library || !sym.exported {
            continue;
        }
        let name = format!("{}.{}", m.name, sym.name);
        let mut sig = String::new();
        format_function_sig(
            prog,
            &prog.signatures.functions[f.signature.index()],
            &mut sig,
        );
        items.push(PubItem {
            kind: "function",
            name,
            sig,
        });
    }

    for c in &prog.constants {
        let sym = prog.interfaces.symbol(c.symbol);
        let m = prog.modules.module(sym.module);
        if m.library || !sym.exported {
            continue;
        }
        let name = format!("{}.{}", m.name, sym.name);
        let mut sig = String::new();
        format_type(prog, c.ty, &mut sig);
        items.push(PubItem {
            kind: "constant",
            name,
            sig,
        });
    }

    if let Some(svc_idx) = side.entry_service {
        let svc = &prog.services[svc_idx];
        let m = prog.modules.module(svc.module);
        if let Some(tok_node) = svc.token_source
            && let Some(tok_text) =
                slice_source_span(&side.sources, m.syntax.tree.node(tok_node).span)
        {
            items.push(PubItem {
                kind: "service_token",
                name: "service.token".to_string(),
                sig: tok_text.to_string(),
            });
        }
        for tl in &svc.tools {
            let fn_def = prog.function(tl.function);
            let tsym = prog.interfaces.symbol(fn_def.symbol);
            let rk = match tl.reply {
                ServiceReply::ReadOnly => "read-only ",
                ServiceReply::Update => "update ",
                ServiceReply::ResultUpdate => "result-update ",
            };
            let mut sig = String::from(rk);
            format_function_sig(
                prog,
                &prog.signatures.functions[fn_def.signature.index()],
                &mut sig,
            );
            items.push(PubItem {
                kind: "tool",
                name: format!("service.tool.{}", tsym.name),
                sig,
            });
        }
        for tm in &svc.timers {
            let fn_def = prog.function(tm.function);
            let tsym = prog.interfaces.symbol(fn_def.symbol);
            let iv_text = m
                .syntax
                .tree
                .node(tm.syntax)
                .children
                .first()
                .and_then(|c| *c)
                .and_then(|id| slice_source_span(&side.sources, m.syntax.tree.node(id).span))
                .unwrap_or("");
            items.push(PubItem {
                kind: "timer",
                name: format!("service.timer.{}", tsym.name),
                sig: format!("every {iv_text}"),
            });
        }
    }

    for api in &prog.apis {
        let m = prog.modules.module(api.module);
        if m.library {
            continue;
        }
        if let Some(ref cors) = api.cors {
            items.push(PubItem {
                kind: "api_cors",
                name: "api.cors".to_string(),
                sig: cors.clone(),
            });
        }
        for ac in &api.actors {
            let mut sig = String::new();
            format_type(prog, ac.ty, &mut sig);
            items.push(PubItem {
                kind: "api_actor",
                name: format!("api.actor.{}", ac.name),
                sig,
            });
        }
        for st in &api.statics {
            items.push(PubItem {
                kind: "api_static",
                name: format!("api.static.{}", st.prefix),
                sig: st.dir.clone(),
            });
        }
        for job in &api.jobs {
            let fn_def = prog.function(job.function);
            let mut sig = String::new();
            if job.has_schedule {
                let iv_text = m
                    .syntax
                    .tree
                    .node(job.syntax)
                    .children
                    .get(2)
                    .and_then(|c| *c)
                    .and_then(|id| slice_source_span(&side.sources, m.syntax.tree.node(id).span))
                    .unwrap_or("");
                let _ = write!(sig, "[every {iv_text}] ");
            }
            format_function_sig(
                prog,
                &prog.signatures.functions[fn_def.signature.index()],
                &mut sig,
            );
            items.push(PubItem {
                kind: "api_job",
                name: format!("api.job.{}", job.name),
                sig,
            });
        }
        for rt in &api.routes {
            let method = match rt.method {
                ApiMethod::Get => "GET",
                ApiMethod::Post => "POST",
                ApiMethod::Put => "PUT",
                ApiMethod::Patch => "PATCH",
                ApiMethod::Delete => "DELETE",
            };
            let fn_def = prog.function(rt.function);
            let mut sig = String::new();
            match rt.auth_mode {
                ApiAuthMode::None => {}
                ApiAuthMode::Required => sig.push_str("[auth] "),
                ApiAuthMode::Optional => sig.push_str("[auth?] "),
            }
            format_function_sig(
                prog,
                &prog.signatures.functions[fn_def.signature.index()],
                &mut sig,
            );
            items.push(PubItem {
                kind: "api_route",
                name: format!("api.route.{method} {}", rt.path),
                sig,
            });
        }
    }

    items.sort();
    items
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ContractOutcome {
    Passed,
    Failed,
    SignatureChanged,
    Removed,
}

impl ContractOutcome {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Passed => "passed",
            Self::Failed => "failed",
            Self::SignatureChanged => "signature_changed",
            Self::Removed => "removed",
        }
    }
}

struct ContractItem {
    name: String,
    kind: &'static str,
    is_service: bool,
    old_spec_idx: usize,
    old_svc_promise_idx: usize,
    old_module: ModuleId,
    new_module: Option<ModuleId>,
    new_fn_idx: Option<CheckedFuncId>,
    outcome: ContractOutcome,
    spliced_prefix: String,
}

fn fn_body_start(tree: &SyntaxTree, fn_node: NodeId) -> u32 {
    let node = tree.node(fn_node);
    let mut at = node.span.start;
    for i in 0..5 {
        if let Some(child_id) = node.children.get(i).copied().flatten() {
            let part = tree.node(child_id);
            if i == 4 && part.kind == SyntaxKind::Sequence {
                for &sub_id in part.children.iter().flatten() {
                    at = at.max(tree.node(sub_id).span.end);
                }
            } else {
                at = at.max(part.span.end);
            }
        }
    }
    at
}

fn expr_ident_span(tree: &SyntaxTree, node_id: NodeId) -> Option<Span> {
    let syn = tree.node(node_id);
    if syn.kind == SyntaxKind::Name {
        return Some(syn.span);
    }
    if syn.kind == SyntaxKind::FieldAccess
        && let Some(first) = syn.children.first().copied().flatten()
    {
        return Some(tree.node(first).span);
    }
    if syn.kind == SyntaxKind::Postfix
        && let Some(last_id) = syn.children.last().copied().flatten()
    {
        let last = tree.node(last_id);
        if last.kind == SyntaxKind::FieldAccess
            && let Some(first) = last.children.first().copied().flatten()
        {
            return Some(tree.node(first).span);
        }
    }
    None
}

struct HelperMap {
    fn_alias: Vec<Option<String>>,
    const_alias: Vec<Option<String>>,
    need_fn: Vec<bool>,
    need_const: Vec<bool>,
}

fn mark_helpers_in_body(
    old_p: &CheckedProgram,
    body: &CheckedBody,
    module: ModuleId,
    subject_fn: Option<CheckedFuncId>,
    hm: &mut HelperMap,
    wf: &mut Vec<usize>,
    wc: &mut Vec<usize>,
) {
    for item in &body.items {
        let Some(sym) = item.symbol else {
            continue;
        };
        if sym.module != module {
            continue;
        }
        if let Some(fidx) = old_p.functions.iter().position(|f| f.symbol == sym) {
            if subject_fn.map(CheckedFuncId::index) != Some(fidx) && !hm.need_fn[fidx] {
                hm.need_fn[fidx] = true;
                wf.push(fidx);
            }
            continue;
        }
        if let Some(cidx) = old_p.constants.iter().position(|c| c.symbol == sym)
            && !hm.need_const[cidx]
        {
            hm.need_const[cidx] = true;
            wc.push(cidx);
        }
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "source rewriting needs old side, tree, checked body, range, subject, helper map, and old/new functions"
)]
fn append_rewritten_range(
    old_s: &Side,
    old_p: &CheckedProgram,
    tree: &SyntaxTree,
    body: &CheckedBody,
    file: u32,
    range_start: u32,
    range_end: u32,
    subject_fn: Option<CheckedFuncId>,
    hm: &HelperMap,
    old_fn: Option<&CheckedFunction>,
    new_fn: Option<&CheckedFunction>,
    out: &mut Vec<u8>,
) {
    let mut edits: Vec<(u32, u32, String)> = Vec::new();
    for e in &body.items {
        let Some(sp) = expr_ident_span(tree, e.syntax) else {
            continue;
        };
        if sp.file != file || sp.start < range_start || sp.end > range_end {
            continue;
        }
        let mut rep: Option<String> = None;
        if let Some(sym) = e.symbol {
            for (fidx, f) in old_p.functions.iter().enumerate() {
                if subject_fn.map(CheckedFuncId::index) != Some(fidx)
                    && f.symbol == sym
                    && hm.need_fn[fidx]
                {
                    rep = hm.fn_alias[fidx].clone();
                    break;
                }
            }
            if rep.is_none() {
                for (cidx, c) in old_p.constants.iter().enumerate() {
                    if c.symbol == sym && hm.need_const[cidx] {
                        rep = hm.const_alias[cidx].clone();
                        break;
                    }
                }
            }
        } else if let (Some(bid), Some(ofn), Some(nfn)) = (e.binding, old_fn, new_fn) {
            for (p, &obid) in ofn.parameters.iter().enumerate() {
                if obid == bid
                    && let Some(&nbid) = nfn.parameters.get(p)
                {
                    let oname = &ofn.body.binding(obid).name;
                    let nname = &nfn.body.binding(nbid).name;
                    if oname != nname {
                        rep = Some(nname.clone());
                    }
                    break;
                }
            }
        }
        if let Some(r) = rep {
            edits.push((sp.start, sp.end, r));
        }
    }
    edits.sort_by_key(|e| e.0);
    let Some(sf) = old_s.sources.get(file) else {
        return;
    };
    let bytes = sf.bytes();
    let mut at = range_start;
    for (start, end, rep) in edits {
        if start < at {
            continue;
        }
        out.extend_from_slice(&bytes[at as usize..start as usize]);
        out.extend_from_slice(rep.as_bytes());
        at = end;
    }
    out.extend_from_slice(&bytes[at as usize..range_end as usize]);
}

fn try_run_contracts(
    opts: &DiffOptions<'_>,
    old_s: &Side,
    old_p: &CheckedProgram,
    new_s: &Side,
    new_p: &CheckedProgram,
    items: &mut [ContractItem],
    active: &[bool],
) -> bool {
    let mut hm = HelperMap {
        fn_alias: vec![None; old_p.functions.len()],
        const_alias: vec![None; old_p.constants.len()],
        need_fn: vec![false; old_p.functions.len()],
        need_const: vec![false; old_p.constants.len()],
    };

    for (k, item) in items.iter().enumerate() {
        if !active[k] {
            continue;
        }
        let mut wf = Vec::new();
        let mut wc = Vec::new();
        let (body, subj) = if item.is_service {
            let Some(esvc) = old_s.entry_service else {
                continue;
            };
            (
                &old_p.services[esvc].promises[item.old_svc_promise_idx]
                    .spec
                    .body,
                None,
            )
        } else {
            let sp = &old_p.specs[item.old_spec_idx];
            (&sp.body, sp.subject)
        };
        mark_helpers_in_body(
            old_p,
            body,
            item.old_module,
            subj,
            &mut hm,
            &mut wf,
            &mut wc,
        );
        let (mut fi, mut ci) = (0usize, 0usize);
        while fi < wf.len() || ci < wc.len() {
            while fi < wf.len() {
                let f = wf[fi];
                fi += 1;
                mark_helpers_in_body(
                    old_p,
                    &old_p.functions[f].body,
                    item.old_module,
                    subj,
                    &mut hm,
                    &mut wf,
                    &mut wc,
                );
            }
            while ci < wc.len() {
                let c = wc[ci];
                ci += 1;
                mark_helpers_in_body(
                    old_p,
                    &old_p.constants[c].initializer,
                    item.old_module,
                    subj,
                    &mut hm,
                    &mut wf,
                    &mut wc,
                );
            }
        }
    }

    for (f, func) in old_p.functions.iter().enumerate() {
        if hm.need_fn[f] {
            let orig = &old_p.interfaces.symbol(func.symbol).name;
            let suffix = if orig.ends_with('?') { "?" } else { "" };
            hm.fn_alias[f] = Some(format!("mo_diff_old_fn_{f}{suffix}"));
        }
    }
    for (c, need) in hm.need_const.iter().enumerate() {
        if *need {
            hm.const_alias[c] = Some(format!("mo_diff_old_const_{c}"));
        }
    }

    let mut added_fn_promises = vec![0u32; new_p.functions.len()];
    let mut added_fn_examples = vec![0u32; new_p.functions.len()];
    let mut added_svc_promises = 0u32;

    let mut synth_sources = SourceMap::new();
    for (midx, nm) in new_p.modules.modules.iter().enumerate() {
        if nm.library {
            continue;
        }
        let mid = ModuleId(midx as u32);
        let Some(nsrc) = new_s.sources.get(nm.syntax.span.file) else {
            return false;
        };
        let nbytes = nsrc.bytes();
        let mut inserts: Vec<(u32, Vec<u8>)> = Vec::new();

        for (k, item) in items.iter_mut().enumerate() {
            if !active[k] || item.new_module != Some(mid) {
                continue;
            }
            let om_tree = &old_p.modules.module(item.old_module).syntax.tree;
            if item.is_service {
                let (Some(old_esvc), Some(new_esvc)) = (old_s.entry_service, new_s.entry_service)
                else {
                    continue;
                };
                let old_svc = &old_p.services[old_esvc];
                let new_svc = &new_p.services[new_esvc];
                let sp = &old_svc.promises[item.old_svc_promise_idx];
                let mut snippet = Vec::new();
                snippet.extend_from_slice(b"\n  promise \"contract\"");
                if let Some(tidx) = sp.tool {
                    let tname = &old_p
                        .interfaces
                        .symbol(old_p.function(old_svc.tools[tidx as usize].function).symbol)
                        .name;
                    snippet.extend_from_slice(b" on ");
                    snippet.extend_from_slice(tname.as_bytes());
                }
                snippet.push(b'\n');
                for cond in &sp.spec.conditions {
                    let cs = cond.text.span;
                    snippet.extend_from_slice(b"    ");
                    append_rewritten_range(
                        old_s,
                        old_p,
                        om_tree,
                        &sp.spec.body,
                        cs.file,
                        cs.start,
                        cs.end,
                        None,
                        &hm,
                        None,
                        None,
                        &mut snippet,
                    );
                    snippet.push(b'\n');
                }
                snippet.extend_from_slice(b"  end\n");
                added_svc_promises += 1;
                let ord = new_svc.promises.len() as u32 + added_svc_promises;
                item.spliced_prefix = format!("{}.service.promise.{ord}", nm.name);
                let svc_end = nm.syntax.tree.node(new_svc.syntax).span.end;
                inserts.push((svc_end.saturating_sub(3), snippet));
            } else {
                let Some(new_fn_id) = item.new_fn_idx else {
                    continue;
                };
                let sp = &old_p.specs[item.old_spec_idx];
                let Some(old_subj_id) = sp.subject else {
                    continue;
                };
                let old_fn = old_p.function(old_subj_id);
                let new_fn = new_p.function(new_fn_id);
                let new_decl = new_p.interfaces.symbol(new_fn.symbol).declaration;
                let insert_at = fn_body_start(&nm.syntax.tree, new_decl);
                let existing = new_p
                    .specs
                    .iter()
                    .filter(|s| s.subject == Some(new_fn_id) && s.kind == sp.kind)
                    .count() as u32;
                let mut snippet = Vec::new();
                let mut name_buf = String::new();
                append_function_name(&mut name_buf, new_p, new_fn_id);
                if sp.kind == CheckedSpecKind::Promise {
                    snippet.extend_from_slice(b"\n  promise \"contract\"\n");
                    for cond in &sp.conditions {
                        let cs = cond.text.span;
                        snippet.extend_from_slice(b"    ");
                        append_rewritten_range(
                            old_s,
                            old_p,
                            om_tree,
                            &sp.body,
                            cs.file,
                            cs.start,
                            cs.end,
                            Some(old_subj_id),
                            &hm,
                            Some(old_fn),
                            Some(new_fn),
                            &mut snippet,
                        );
                        snippet.push(b'\n');
                    }
                    snippet.extend_from_slice(b"  end\n");
                    added_fn_promises[new_fn_id.index()] += 1;
                    let ord = existing + added_fn_promises[new_fn_id.index()];
                    let _ = write!(name_buf, ".promise.{ord}");
                } else {
                    let es = sp.text.span;
                    snippet.extend_from_slice(b"\n  examples\n    ");
                    append_rewritten_range(
                        old_s,
                        old_p,
                        om_tree,
                        &sp.body,
                        es.file,
                        es.start,
                        es.end,
                        Some(old_subj_id),
                        &hm,
                        Some(old_fn),
                        Some(new_fn),
                        &mut snippet,
                    );
                    snippet.extend_from_slice(b"\n  end\n");
                    added_fn_examples[new_fn_id.index()] += 1;
                    let ord = existing + added_fn_examples[new_fn_id.index()];
                    let _ = write!(name_buf, ".example.{ord}");
                }
                item.spliced_prefix = name_buf;
                inserts.push((insert_at, snippet));
            }
        }

        // Append needed OLD helpers belonging to the matching OLD module.
        if let Some(old_m_idx) = old_p
            .modules
            .modules
            .iter()
            .position(|om| !om.library && om.name == nm.name)
        {
            let old_mid = ModuleId(old_m_idx as u32);
            let om_tree = &old_p.modules.module(old_mid).syntax.tree;
            for (cidx, c) in old_p.constants.iter().enumerate() {
                if !hm.need_const[cidx] || c.symbol.module != old_mid {
                    continue;
                }
                let Some(ref alias) = hm.const_alias[cidx] else {
                    continue;
                };
                let decl = om_tree.node(old_p.interfaces.symbol(c.symbol).declaration);
                let Some(rhs_id) = decl.children.get(1).copied().flatten() else {
                    continue;
                };
                let rhs = om_tree.node(rhs_id);
                let mut snippet = Vec::new();
                snippet.push(b'\n');
                snippet.extend_from_slice(alias.as_bytes());
                snippet.extend_from_slice(b" = ");
                append_rewritten_range(
                    old_s,
                    old_p,
                    om_tree,
                    &c.initializer,
                    rhs.span.file,
                    rhs.span.start,
                    rhs.span.end,
                    None,
                    &hm,
                    None,
                    None,
                    &mut snippet,
                );
                snippet.push(b'\n');
                inserts.push((nbytes.len() as u32, snippet));
            }
            for (fidx, f) in old_p.functions.iter().enumerate() {
                if !hm.need_fn[fidx] || f.symbol.module != old_mid {
                    continue;
                }
                let Some(ref alias) = hm.fn_alias[fidx] else {
                    continue;
                };
                let decl_id = old_p.interfaces.symbol(f.symbol).declaration;
                let decl = om_tree.node(decl_id);
                let Some(name_id) = decl.children.first().copied().flatten() else {
                    continue;
                };
                let name_syn = om_tree.node(name_id);
                let Some(osrc) = old_s.sources.get(decl.span.file) else {
                    continue;
                };
                let bstart = fn_body_start(om_tree, decl_id);
                let header_end = decl
                    .children
                    .get(4)
                    .copied()
                    .flatten()
                    .and_then(|seq_id| om_tree.node(seq_id).children.first().copied().flatten())
                    .map_or(bstart, |first_id| om_tree.node(first_id).span.start);
                let mut snippet = Vec::new();
                snippet.extend_from_slice(b"\nfn ");
                snippet.extend_from_slice(alias.as_bytes());
                snippet.extend_from_slice(
                    &osrc.bytes()[name_syn.span.end as usize..header_end as usize],
                );
                snippet.push(b'\n');
                append_rewritten_range(
                    old_s,
                    old_p,
                    om_tree,
                    &f.body,
                    decl.span.file,
                    bstart,
                    decl.span.end,
                    None,
                    &hm,
                    None,
                    None,
                    &mut snippet,
                );
                snippet.push(b'\n');
                inserts.push((nbytes.len() as u32, snippet));
            }
        }

        inserts.sort_by_key(|ins| ins.0);
        let mut file_buf = Vec::with_capacity(nbytes.len() + 1024);
        let mut at = 0usize;
        for (off, bytes) in inserts {
            let off = off as usize;
            if off >= at && off <= nbytes.len() {
                file_buf.extend_from_slice(&nbytes[at..off]);
                file_buf.extend_from_slice(&bytes);
                at = off;
            }
        }
        file_buf.extend_from_slice(&nbytes[at..]);
        let _ = synth_sources.add_bytes(nsrc.path_lossy().as_bytes().to_vec(), file_buf);
    }

    let mut synth_diags = DiagBag::new();
    let load_opts = LoadOptions {
        library: lib_mode_for(opts.library),
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let Some(set) = load_bundle_with_library(&mut synth_sources, &load_opts, &mut synth_diags)
        .filter(|_| !synth_diags.has_any())
    else {
        return false;
    };
    let Some(prog) = check_program(Some(&synth_sources), set, &mut synth_diags)
        .filter(|_| !synth_diags.has_any())
    else {
        return false;
    };

    let cfg = TestConfig {
        json: false,
        only: Vec::new(),
        module: None,
        calls: 200,
        seconds: 2,
        sequences: 200,
        limits: Limits::default(),
        door_plan: false,
        gated: true,
        first_failure: false,
    };
    let Some(mut sel) = select_tests(&prog, &cfg, &mut synth_diags) else {
        return false;
    };
    if sel.mistake.is_some() || synth_diags.has_any() {
        return false;
    }
    sel.items.retain(|obs| {
        items
            .iter()
            .enumerate()
            .any(|(k, item)| active[k] && obs.name == item.spliced_prefix)
    });

    let mut quiet = std::io::sink();
    let mut totals = TestTotals::default();
    let mut gated = GatedRun {
        checks: Vec::new(),
        use_planned_checks: false,
        record: false,
        shrink: true,
        stop: false,
        limit: u64::MAX,
        start_limit: u64::MAX,
        started: false,
        start_steps: 0,
        start_entered: Vec::new(),
        ran: 0,
    };
    run_gated(
        &cfg,
        &prog,
        &synth_sources,
        &sel,
        &mut quiet,
        &mut synth_diags,
        &mut totals,
        &mut gated,
    );

    for (k, item) in items.iter_mut().enumerate() {
        if !active[k] {
            continue;
        }
        let mut saw_any = false;
        let mut all_passed = true;
        for chk in &gated.checks {
            let gn = chk.name.as_str();
            let mut same = gn == item.spliced_prefix;
            if !same
                && let Some(pos) = gn.find("::")
                && let Some(dot_pos) = item.spliced_prefix.find(".promise.")
            {
                let tail = &gn[pos + 2..];
                let fn_prefix = &item.spliced_prefix[..dot_pos];
                let dot_suffix = &item.spliced_prefix[dot_pos..];
                same = tail.starts_with(fn_prefix)
                    && tail.as_bytes().get(fn_prefix.len()) == Some(&b'(')
                    && tail[fn_prefix.len()..].contains(dot_suffix);
            }
            if same {
                saw_any = true;
                if chk.outcome != CheckOutcome::Passed {
                    all_passed = false;
                }
            }
        }
        item.outcome = if saw_any && all_passed {
            ContractOutcome::Passed
        } else {
            ContractOutcome::Failed
        };
    }

    true
}

fn evaluate_contracts(
    opts: &DiffOptions<'_>,
    old_s: &Side,
    old_p: &CheckedProgram,
    new_s: &Side,
    new_p: &CheckedProgram,
) -> Vec<ContractItem> {
    let mut items = Vec::new();
    let mut active = Vec::new();
    let mut fn_prom_ord = vec![0u32; old_p.functions.len()];
    let mut fn_ex_ord = vec![0u32; old_p.functions.len()];

    for (i, sp) in old_p.specs.iter().enumerate() {
        if old_p.modules.module(sp.module).library
            || !matches!(sp.kind, CheckedSpecKind::Promise | CheckedSpecKind::Example)
        {
            continue;
        }
        let Some(subj_id) = sp.subject else {
            continue;
        };
        let ord = if sp.kind == CheckedSpecKind::Promise {
            fn_prom_ord[subj_id.index()] += 1;
            fn_prom_ord[subj_id.index()]
        } else {
            fn_ex_ord[subj_id.index()] += 1;
            fn_ex_ord[subj_id.index()]
        };
        let mut cname = String::new();
        append_function_name(&mut cname, old_p, subj_id);
        let _ = write!(
            cname,
            "{}{ord}",
            if sp.kind == CheckedSpecKind::Promise {
                ".promise."
            } else {
                ".example."
            }
        );

        let old_fn = old_p.function(subj_id);
        let old_fn_name = &old_p.interfaces.symbol(old_fn.symbol).name;
        let mod_name = &old_p.modules.module(sp.module).name;
        let new_mod = new_p
            .modules
            .modules
            .iter()
            .position(|m| !m.library && m.name == *mod_name)
            .map(|idx| ModuleId(idx as u32));

        let mut outcome = ContractOutcome::Removed;
        let mut matched_fn = None;
        if let Some(nmid) = new_mod {
            let mut osig = String::new();
            format_function_sig(
                old_p,
                &old_p.signatures.functions[old_fn.signature.index()],
                &mut osig,
            );
            let mut any_same_name = false;
            for (nf_idx, nf) in new_p.functions.iter().enumerate() {
                let nsym = new_p.interfaces.symbol(nf.symbol);
                if nsym.module != nmid || nsym.name != *old_fn_name {
                    continue;
                }
                any_same_name = true;
                let mut nsig = String::new();
                format_function_sig(
                    new_p,
                    &new_p.signatures.functions[nf.signature.index()],
                    &mut nsig,
                );
                if osig == nsig {
                    matched_fn = Some(CheckedFuncId(nf_idx as u32));
                }
            }
            if matched_fn.is_some() {
                outcome = ContractOutcome::Passed;
            } else if any_same_name {
                outcome = ContractOutcome::SignatureChanged;
            }
        }

        items.push(ContractItem {
            name: cname,
            kind: if sp.kind == CheckedSpecKind::Promise {
                "promise"
            } else {
                "example"
            },
            is_service: false,
            old_spec_idx: i,
            old_svc_promise_idx: 0,
            old_module: sp.module,
            new_module: new_mod,
            new_fn_idx: matched_fn,
            outcome,
            spliced_prefix: String::new(),
        });
        active.push(matched_fn.is_some());
    }

    if let Some(old_esvc) = old_s.entry_service {
        let old_svc = &old_p.services[old_esvc];
        let old_mod_name = &old_p.modules.module(old_svc.module).name;
        for (j, sp) in old_svc.promises.iter().enumerate() {
            let cname = format!("{old_mod_name}.service.promise.{}", j + 1);
            let mut outcome = ContractOutcome::Removed;
            if let Some(new_esvc) = new_s.entry_service {
                let new_svc = &new_p.services[new_esvc];
                let state_same = old_s.has_schema
                    && new_s.has_schema
                    && old_s.state_schema == new_s.state_schema;
                match sp.tool {
                    None => {
                        outcome = if state_same {
                            ContractOutcome::Passed
                        } else {
                            ContractOutcome::SignatureChanged
                        };
                    }
                    Some(tidx) => {
                        let tname = &old_p
                            .interfaces
                            .symbol(old_p.function(old_svc.tools[tidx as usize].function).symbol)
                            .name;
                        let new_t = new_svc.tools.iter().position(|nt| {
                            new_p
                                .interfaces
                                .symbol(new_p.function(nt.function).symbol)
                                .name
                                == *tname
                        });
                        match new_t {
                            None => outcome = ContractOutcome::Removed,
                            Some(nt_idx) => {
                                if !state_same
                                    || old_svc.tools[tidx as usize].reply
                                        != new_svc.tools[nt_idx].reply
                                {
                                    outcome = ContractOutcome::SignatureChanged;
                                } else {
                                    let ofn = old_p.function(old_svc.tools[tidx as usize].function);
                                    let nfn = new_p.function(new_svc.tools[nt_idx].function);
                                    let mut osig = String::new();
                                    let mut nsig = String::new();
                                    format_function_sig(
                                        old_p,
                                        &old_p.signatures.functions[ofn.signature.index()],
                                        &mut osig,
                                    );
                                    format_function_sig(
                                        new_p,
                                        &new_p.signatures.functions[nfn.signature.index()],
                                        &mut nsig,
                                    );
                                    let mut params_same = ofn.parameters.len()
                                        == nfn.parameters.len()
                                        && osig == nsig;
                                    for p in 2..ofn.parameters.len() {
                                        if !params_same {
                                            break;
                                        }
                                        params_same = ofn.body.binding(ofn.parameters[p]).name
                                            == nfn.body.binding(nfn.parameters[p]).name;
                                    }
                                    outcome = if params_same {
                                        ContractOutcome::Passed
                                    } else {
                                        ContractOutcome::SignatureChanged
                                    };
                                }
                            }
                        }
                    }
                }
            }
            items.push(ContractItem {
                name: cname,
                kind: "service_promise",
                is_service: true,
                old_spec_idx: 0,
                old_svc_promise_idx: j,
                old_module: ModuleId(0),
                new_module: Some(ModuleId(0)),
                new_fn_idx: None,
                outcome,
                spliced_prefix: String::new(),
            });
            active.push(outcome == ContractOutcome::Passed);
        }
    }

    if active.iter().any(|&a| a)
        && !try_run_contracts(opts, old_s, old_p, new_s, new_p, &mut items, &active)
    {
        // Batch synthesis failed: run each active contract individually (`src/tools/diff.c:1453`).
        for k in 0..items.len() {
            if !active[k] {
                continue;
            }
            let mut single = vec![false; items.len()];
            single[k] = true;
            if !try_run_contracts(opts, old_s, old_p, new_s, new_p, &mut items, &single) {
                items[k].outcome = ContractOutcome::SignatureChanged;
            }
        }
    }

    items
}

fn push_str_array_json(out: &mut String, items: &[String]) {
    out.push('[');
    for (i, item) in items.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        push_json_string(out, item);
    }
    out.push(']');
}

fn write_version_diff(out: &mut dyn Write, version: &str) -> bool {
    let mut s = String::from("{\"record\":\"version\",\"format\":\"1.0\",\"mo\":");
    push_json_string(&mut s, version);
    s.push_str(",\"command\":\"diff\"}\n");
    out.write_all(s.as_bytes()).is_ok() && out.flush().is_ok()
}

fn write_diags_diff(
    sources: &SourceMap,
    diags: &mut DiagBag,
    json: bool,
    version: &str,
    output: &mut dyn Write,
    errors: &mut dyn Write,
) -> i32 {
    diags.sort();
    if json {
        if !write_version_diff(output, version) {
            return 1;
        }
        for d in diags.items() {
            if let Ok(buf) = render_diagnostic_default(
                sources,
                d,
                RenderFormat::Json,
                TerminalEncoding::Utf8,
                UNICODE_METRICS,
            ) {
                let _ = output.write_all(&buf);
            }
        }
        let totals = format!(
            "{{\"record\":\"totals\",\"errors\":{},\"dropped\":{},\"status\":1}}\n",
            diags.len(),
            diags.dropped()
        );
        let _ = output.write_all(totals.as_bytes());
        let _ = output.flush();
    } else {
        for d in diags.items() {
            if let Ok(buf) = render_diagnostic_default(
                sources,
                d,
                RenderFormat::Text,
                TerminalEncoding::Utf8,
                UNICODE_METRICS,
            ) {
                let _ = errors.write_all(&buf);
            }
        }
        let _ = errors.flush();
    }
    1
}

/// Executes `mo diff [--json] [--lib DIR] FILE [FILE2]` (`src/tools/diff.c:mo_diff`).
pub fn run_diff(opts: &DiffOptions<'_>, output: &mut dyn Write, errors: &mut dyn Write) -> i32 {
    let _git_guard: Option<TempDirGuard>;
    let git_old_bytes: Vec<u8>;
    let old_path: &[u8] = match opts.old_file {
        Some(old) => {
            _git_guard = None;
            old
        }
        None => {
            let new_str = String::from_utf8_lossy(opts.new_file);
            match materialize_git_head(Path::new(new_str.as_ref())) {
                Some((guard, path)) => {
                    _git_guard = Some(guard);
                    git_old_bytes = path.as_os_str().as_encoded_bytes().to_vec();
                    &git_old_bytes
                }
                None => {
                    let sources = SourceMap::new();
                    let mut diags = DiagBag::new();
                    let _ = diags.push(
                        Diagnostic::new(
                            ErrorCode::ToolPipeline,
                            Span::NONE,
                            "mo diff with one file needs Git HEAD to hold that file; pass OLD NEW explicitly",
                        )
                        .with_primary_text(new_str.as_ref()),
                    );
                    return write_diags_diff(
                        &sources,
                        &mut diags,
                        opts.json,
                        opts.version,
                        output,
                        errors,
                    );
                }
            }
        }
    };

    let mut old_s = load_side(old_path, opts.library);
    if old_s.program.is_none() {
        return write_diags_diff(
            &old_s.sources,
            &mut old_s.diags,
            opts.json,
            opts.version,
            output,
            errors,
        );
    }
    let mut new_s = load_side(opts.new_file, opts.library);
    if new_s.program.is_none() {
        return write_diags_diff(
            &new_s.sources,
            &mut new_s.diags,
            opts.json,
            opts.version,
            output,
            errors,
        );
    }

    let (Some(old_p), Some(new_p)) = (old_s.program.as_ref(), new_s.program.as_ref()) else {
        return 1;
    };

    // 1. Authority delta
    let old_caps = collect_program_caps(old_p);
    let new_caps = collect_program_caps(new_p);
    let added_caps: Vec<String> = new_caps
        .iter()
        .filter(|c| !old_caps.contains(c))
        .cloned()
        .collect();
    let removed_caps: Vec<String> = old_caps
        .iter()
        .filter(|c| !new_caps.contains(c))
        .cloned()
        .collect();

    let old_att = collect_attenuations(&old_s, old_p);
    let new_att = collect_attenuations(&new_s, new_p);
    let added_att: Vec<String> = new_att
        .iter()
        .filter(|a| !old_att.contains(a))
        .cloned()
        .collect();
    let removed_att: Vec<String> = old_att
        .iter()
        .filter(|a| !new_att.contains(a))
        .cloned()
        .collect();

    // 2. Public & wire surface delta
    let old_pub = collect_public_surface(&old_s, old_p);
    let new_pub = collect_public_surface(&new_s, new_p);
    let mut old_matched = vec![false; old_pub.len()];
    let mut new_matched = vec![false; new_pub.len()];
    for (i, op) in old_pub.iter().enumerate() {
        for (j, np) in new_pub.iter().enumerate() {
            if !new_matched[j] && op == np {
                old_matched[i] = true;
                new_matched[j] = true;
                break;
            }
        }
    }
    let mut changed_pub: Vec<(usize, usize)> = Vec::new();
    for (i, op) in old_pub.iter().enumerate() {
        if old_matched[i] {
            continue;
        }
        for (j, np) in new_pub.iter().enumerate() {
            if !new_matched[j] && op.kind == np.kind && op.name == np.name {
                old_matched[i] = true;
                new_matched[j] = true;
                changed_pub.push((i, j));
                break;
            }
        }
    }

    // 3. State delta
    let has_service = old_s.entry_service.is_some() || new_s.entry_service.is_some();
    let migration_required = old_s.entry_service.is_some()
        && new_s.entry_service.is_some()
        && old_s.state_schema != new_s.state_schema;
    let state_changed =
        (old_s.entry_service.is_some() != new_s.entry_service.is_some()) || migration_required;
    let mut old_state_type = String::new();
    if let Some(es) = old_s.entry_service {
        format_type(old_p, old_p.services[es].state, &mut old_state_type);
    }
    let mut new_state_type = String::new();
    if let Some(es) = new_s.entry_service {
        format_type(new_p, new_p.services[es].state, &mut new_state_type);
    }

    // 4. Behavioral contracts
    let contracts = evaluate_contracts(opts, &old_s, old_p, &new_s, new_p);
    let passed_n = contracts
        .iter()
        .filter(|c| c.outcome == ContractOutcome::Passed)
        .count();
    let failed_n = contracts
        .iter()
        .filter(|c| c.outcome == ContractOutcome::Failed)
        .count();
    let sig_changed_n = contracts
        .iter()
        .filter(|c| c.outcome == ContractOutcome::SignatureChanged)
        .count();
    let removed_n = contracts
        .iter()
        .filter(|c| c.outcome == ContractOutcome::Removed)
        .count();

    let exit_code = i32::from(failed_n != 0);
    let mut out = String::with_capacity(4096);

    if opts.json {
        if !write_version_diff(output, opts.version) {
            return 1;
        }
        out.push_str("{\"record\":\"diff\",\"authority\":{\"capabilities\":{\"before\":");
        push_str_array_json(&mut out, &old_caps);
        out.push_str(",\"after\":");
        push_str_array_json(&mut out, &new_caps);
        out.push_str(",\"added\":");
        push_str_array_json(&mut out, &added_caps);
        out.push_str(",\"removed\":");
        push_str_array_json(&mut out, &removed_caps);
        out.push_str("},\"attenuations\":{\"added\":");
        push_str_array_json(&mut out, &added_att);
        out.push_str(",\"removed\":");
        push_str_array_json(&mut out, &removed_att);
        out.push_str("}},\"public\":{\"added\":[");

        let mut first = true;
        for (j, np) in new_pub.iter().enumerate() {
            if new_matched[j] {
                continue;
            }
            if !first {
                out.push(',');
            }
            first = false;
            out.push_str("{\"kind\":");
            push_json_string(&mut out, np.kind);
            out.push_str(",\"name\":");
            push_json_string(&mut out, &np.name);
            out.push_str(",\"signature\":");
            push_json_string(&mut out, &np.sig);
            out.push('}');
        }
        out.push_str("],\"removed\":[");
        first = true;
        for (i, op) in old_pub.iter().enumerate() {
            if old_matched[i] {
                continue;
            }
            if !first {
                out.push(',');
            }
            first = false;
            out.push_str("{\"kind\":");
            push_json_string(&mut out, op.kind);
            out.push_str(",\"name\":");
            push_json_string(&mut out, &op.name);
            out.push_str(",\"signature\":");
            push_json_string(&mut out, &op.sig);
            out.push('}');
        }
        out.push_str("],\"changed\":[");
        for (idx, &(oi, ni)) in changed_pub.iter().enumerate() {
            if idx > 0 {
                out.push(',');
            }
            let op = &old_pub[oi];
            let np = &new_pub[ni];
            out.push_str("{\"kind\":");
            push_json_string(&mut out, np.kind);
            out.push_str(",\"name\":");
            push_json_string(&mut out, &np.name);
            out.push_str(",\"before\":");
            push_json_string(&mut out, &op.sig);
            out.push_str(",\"after\":");
            push_json_string(&mut out, &np.sig);
            out.push('}');
        }
        let _ = write!(
            out,
            "]}},\"state\":{{\"has_service\":{has_service},\"changed\":{state_changed},\"migration_required\":{migration_required},\"before\":"
        );
        if old_s.entry_service.is_some() {
            out.push_str("{\"type\":");
            push_json_string(&mut out, &old_state_type);
            let _ = write!(out, ",\"schema\":\"{}\"}}", hex_encode(&old_s.state_schema));
        } else {
            out.push_str("null");
        }
        out.push_str(",\"after\":");
        if new_s.entry_service.is_some() {
            out.push_str("{\"type\":");
            push_json_string(&mut out, &new_state_type);
            let _ = write!(out, ",\"schema\":\"{}\"}}", hex_encode(&new_s.state_schema));
        } else {
            out.push_str("null");
        }
        let _ = write!(
            out,
            "}},\"contracts\":{{\"passed\":{passed_n},\"failed\":{failed_n},\"signature_changed\":{sig_changed_n},\"removed\":{removed_n},\"items\":["
        );
        for (i, c) in contracts.iter().enumerate() {
            if i > 0 {
                out.push(',');
            }
            out.push_str("{\"name\":");
            push_json_string(&mut out, &c.name);
            out.push_str(",\"kind\":");
            push_json_string(&mut out, c.kind);
            let _ = write!(out, ",\"outcome\":\"{}\"}}", c.outcome.as_str());
        }
        let _ = writeln!(out, "]}}}}");
        let _ = writeln!(
            out,
            "{{\"record\":\"totals\",\"errors\":0,\"dropped\":0,\"status\":{exit_code}}}"
        );
        if output.write_all(out.as_bytes()).is_err() || output.flush().is_err() {
            return 1;
        }
        exit_code
    } else {
        out.push_str("authority:\n");
        if added_caps.is_empty()
            && removed_caps.is_empty()
            && added_att.is_empty()
            && removed_att.is_empty()
        {
            out.push_str("  unchanged\n");
        } else {
            for c in &added_caps {
                let _ = writeln!(out, "  + capability {c}");
            }
            for c in &removed_caps {
                let _ = writeln!(out, "  - capability {c}");
            }
            for a in &added_att {
                let _ = writeln!(out, "  + attenuation {}", sanitize_terminal_text(a));
            }
            for a in &removed_att {
                let _ = writeln!(out, "  - attenuation {}", sanitize_terminal_text(a));
            }
        }

        out.push_str("public:\n");
        let any_pub = !changed_pub.is_empty()
            || new_matched.iter().any(|&m| !m)
            || old_matched.iter().any(|&m| !m);
        if !any_pub {
            out.push_str("  unchanged\n");
        } else {
            for (j, np) in new_pub.iter().enumerate() {
                if !new_matched[j] {
                    let _ = writeln!(
                        out,
                        "  + {} {}: {}",
                        np.kind,
                        sanitize_terminal_text(&np.name),
                        sanitize_terminal_text(&np.sig)
                    );
                }
            }
            for (i, op) in old_pub.iter().enumerate() {
                if !old_matched[i] {
                    let _ = writeln!(
                        out,
                        "  - {} {}: {}",
                        op.kind,
                        sanitize_terminal_text(&op.name),
                        sanitize_terminal_text(&op.sig)
                    );
                }
            }
            for &(oi, ni) in &changed_pub {
                let op = &old_pub[oi];
                let np = &new_pub[ni];
                let _ = writeln!(
                    out,
                    "  ~ {} {}: {} -> {}",
                    np.kind,
                    sanitize_terminal_text(&np.name),
                    sanitize_terminal_text(&op.sig),
                    sanitize_terminal_text(&np.sig)
                );
            }
        }

        out.push_str("state:\n");
        if !has_service {
            out.push_str("  no service\n");
        } else if !state_changed {
            let _ = writeln!(
                out,
                "  unchanged ({})",
                sanitize_terminal_text(&new_state_type)
            );
        } else if migration_required {
            let _ = writeln!(
                out,
                "  changed: {} -> {} (migration required)",
                sanitize_terminal_text(&old_state_type),
                sanitize_terminal_text(&new_state_type)
            );
        } else if old_s.entry_service.is_none() {
            let _ = writeln!(
                out,
                "  + service state {}",
                sanitize_terminal_text(&new_state_type)
            );
        } else {
            let _ = writeln!(
                out,
                "  - service state {}",
                sanitize_terminal_text(&old_state_type)
            );
        }

        let _ = writeln!(
            out,
            "contracts: {passed_n} passed, {failed_n} failed, {sig_changed_n} signature_changed, {removed_n} removed"
        );
        for c in &contracts {
            if c.outcome == ContractOutcome::Passed {
                continue;
            }
            let _ = writeln!(
                out,
                "  {} {}",
                c.outcome.as_str(),
                sanitize_terminal_text(&c.name)
            );
        }

        if output.write_all(out.as_bytes()).is_err() || output.flush().is_err() {
            let _ = writeln!(errors, "mo diff: could not write output");
            return 1;
        }
        exit_code
    }
}
