//! `mo index`: deterministic compiler-exact JSON AST, symbol, call-graph, and spec exporter.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::io::Write;

use mo_base::{DiagBag, SourceMap, Span};
use mo_check::{
    CheckedBody, CheckedProgram, CheckedSpecKind, FunctionSignature, LibraryMode, LoadOptions,
    ModuleId, SourceTypeKind, SymRef, TypeId, check_program, default_embedded_library,
    load_program_bytes,
};
use mo_def::limits::MO_CLI_CHECK_SIZE;
use mo_syntax::SyntaxKind;
use sha2::{Digest, Sha256};

use crate::cli::Cli;
use crate::fs_util::bytes_to_path;
use crate::identity::default_library_root;
use crate::render::write_diagnostics;

pub(crate) fn push_json_str(out: &mut String, s: &str) {
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut hex = String::with_capacity(64);
    for b in digest {
        let _ = write!(hex, "{b:02x}");
    }
    hex
}

fn span_lines(sources: &SourceMap, span: Span) -> (u32, u32) {
    if !sources.is_available(span) {
        return (0, 0);
    }
    let start = sources
        .position(span.file, span.start)
        .map_or(0, |lc| lc.line);
    let end = sources
        .position(span.file, span.end)
        .map_or(start, |lc| lc.line);
    (start, end)
}

pub(crate) fn format_source_type(prog: &CheckedProgram, ty: TypeId, out: &mut String) {
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
                    format_source_type(prog, arg, out);
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
                format_source_type(prog, arg, out);
            }
            out.push(')');
        }
        SourceTypeKind::Function => {
            out.push_str("fn(");
            let params = t.arguments.len().saturating_sub(1);
            for (i, &arg) in t.arguments.iter().take(params).enumerate() {
                if i > 0 {
                    out.push_str(", ");
                }
                format_source_type(prog, arg, out);
            }
            out.push_str(") : ");
            if let Some(&ret) = t.arguments.last() {
                format_source_type(prog, ret, out);
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
    for (i, &arg) in ft.arguments.iter().take(params).enumerate() {
        if i > 0 {
            out.push_str(", ");
        }
        format_source_type(prog, arg, out);
    }
    out.push_str(") : ");
    if let Some(&ret) = ft.arguments.last() {
        format_source_type(prog, ret, out);
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

fn is_capability_name(name: &str) -> bool {
    matches!(
        name,
        "Args"
            | "Env"
            | "Console"
            | "Files"
            | "ReadFiles"
            | "Clock"
            | "Random"
            | "SecureRandom"
            | "Network"
            | "Sqlite"
            | "Command"
    )
}

fn collect_caps_in_type(prog: &CheckedProgram, ty: TypeId, caps: &mut BTreeSet<String>) {
    let t = prog.signatures.types.get(ty);
    if t.kind == SourceTypeKind::Named && t.module.is_none() && is_capability_name(&t.name) {
        caps.insert(t.name.clone());
        return;
    }
    if t.kind == SourceTypeKind::Named
        && t.module
            .is_some_and(|m| prog.modules.module(m).name == "Sqlite")
        && t.name == "Database"
    {
        caps.insert("Sqlite.Database".to_string());
    }
    for &arg in &t.arguments {
        collect_caps_in_type(prog, arg, caps);
    }
}

fn collect_body_calls(prog: &CheckedProgram, body: &CheckedBody) -> (Vec<String>, Vec<String>) {
    let mut calls = BTreeSet::new();
    let mut native = BTreeSet::new();
    for expr in &body.items {
        if let Some(sym_ref) = expr.symbol {
            let sym = prog.interfaces.symbol(sym_ref);
            let m = prog.modules.module(sym.module);
            calls.insert(format!("{}.{}", m.name, sym.name));
        }
        if let Some(svc_id) = expr.service
            && let Some(tool_idx) = expr.tool
        {
            let svc = prog.service(svc_id);
            if let Some(tool) = svc.tools.get(tool_idx as usize) {
                let func = prog.function(tool.function);
                let sym = prog.interfaces.symbol(func.symbol);
                let m = prog.modules.module(sym.module);
                calls.insert(format!("{}.{}", m.name, sym.name));
            }
        }
        if let Some(ref nat) = expr.native {
            native.insert(nat.clone());
        }
    }
    (calls.into_iter().collect(), native.into_iter().collect())
}

fn push_str_array(out: &mut String, items: &[String]) {
    out.push('[');
    for (i, item) in items.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        push_json_str(out, item);
    }
    out.push(']');
}

fn render_program_index(prog: &CheckedProgram, sources: &SourceMap) -> String {
    let mut out = String::with_capacity(8192);
    out.push_str("{\"schema_version\":1,\"entry_module\":");
    let entry_name = prog.modules.modules.first().map_or("", |m| m.name.as_str());
    push_json_str(&mut out, entry_name);

    // Pre-index function promises & examples per CheckedFuncId index.
    let mut fn_promises: BTreeMap<usize, Vec<String>> = BTreeMap::new();
    let mut fn_examples: BTreeMap<usize, u32> = BTreeMap::new();
    for spec in &prog.specs {
        if let Some(subj) = spec.subject {
            match spec.kind {
                CheckedSpecKind::Promise => {
                    let wording = spec
                        .wording
                        .as_deref()
                        .and_then(|b| std::str::from_utf8(b).ok())
                        .or_else(|| std::str::from_utf8(&spec.text.bytes).ok())
                        .unwrap_or("")
                        .trim()
                        .to_string();
                    fn_promises.entry(subj.index()).or_default().push(wording);
                }
                CheckedSpecKind::Example => {
                    *fn_examples.entry(subj.index()).or_default() += 1;
                }
                CheckedSpecKind::Test | CheckedSpecKind::Anchor => {}
            }
        }
    }

    // Build lookup from SymRef to DataSignature.
    let data_by_sym: BTreeMap<SymRef, usize> = prog
        .signatures
        .data
        .iter()
        .enumerate()
        .map(|(i, d)| (d.symbol, i))
        .collect();

    // Modules list (entry module + all non-library project/dependency modules).
    out.push_str(",\"modules\":[");
    let mut first_mod = true;
    for m in &prog.modules.modules {
        if m.library && m.id.0 != 0 {
            continue;
        }
        if !first_mod {
            out.push(',');
        }
        first_mod = false;

        let scope = prog.interfaces.scope(m.id);
        let sf = sources.get(m.file);
        let path_str = sf.map(|s| s.path_lossy().into_owned()).unwrap_or_default();
        let file_sha256 = sf.map(|s| sha256_hex(s.bytes())).unwrap_or_default();

        out.push_str("{\"name\":");
        push_json_str(&mut out, &m.name);
        out.push_str(",\"path\":");
        push_json_str(&mut out, &path_str);
        out.push_str(",\"sha256\":");
        push_json_str(&mut out, &file_sha256);
        let _ = write!(
            out,
            ",\"library\":{},\"dependency\":{}",
            m.library, m.dependency
        );

        let uses: Vec<String> = m
            .uses
            .iter()
            .map(|&uid| prog.modules.module(uid).name.clone())
            .collect();
        out.push_str(",\"uses\":");
        push_str_array(&mut out, &uses);

        let exports: Vec<String> = scope
            .symbols
            .iter()
            .filter(|s| s.exported && s.owner.is_none())
            .map(|s| s.name.clone())
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect();
        out.push_str(",\"exports\":");
        push_str_array(&mut out, &exports);

        // Structs
        out.push_str(",\"structs\":[");
        let mut first_struct = true;
        for (sym_i, sym) in scope.symbols.iter().enumerate() {
            if sym.decl_kind != SyntaxKind::Struct {
                continue;
            }
            if !first_struct {
                out.push(',');
            }
            first_struct = false;
            let (line, end_line) = span_lines(sources, sym.declaration_span);
            out.push_str("{\"name\":");
            push_json_str(&mut out, &sym.name);
            let _ = write!(
                out,
                ",\"exported\":{},\"line\":{},\"end_line\":{}",
                sym.exported, line, end_line
            );
            out.push_str(",\"fields\":[");
            let sym_ref = SymRef::new(m.id, mo_check::SymIdx(sym_i as u32));
            if let Some(&di) = data_by_sym.get(&sym_ref) {
                let ds = &prog.signatures.data[di];
                for fi in 0..ds.fields.len() {
                    if fi > 0 {
                        out.push(',');
                    }
                    let field = ds.field_by_index(fi);
                    let mut ty_str = String::new();
                    format_source_type(prog, field.ty, &mut ty_str);
                    out.push_str("{\"name\":");
                    push_json_str(&mut out, field.name.as_deref().unwrap_or(""));
                    out.push_str(",\"type\":");
                    push_json_str(&mut out, &ty_str);
                    out.push('}');
                }
            }
            out.push_str("]}");
        }
        out.push(']');

        // Enums
        out.push_str(",\"enums\":[");
        let mut first_enum = true;
        for sym in &scope.symbols {
            if sym.decl_kind != SyntaxKind::Enum {
                continue;
            }
            if !first_enum {
                out.push(',');
            }
            first_enum = false;
            let (line, end_line) = span_lines(sources, sym.declaration_span);
            out.push_str("{\"name\":");
            push_json_str(&mut out, &sym.name);
            let _ = write!(
                out,
                ",\"exported\":{},\"line\":{},\"end_line\":{}",
                sym.exported, line, end_line
            );
            out.push_str(",\"cases\":[");
            for (ci, &case_idx) in sym.cases.iter().enumerate() {
                if ci > 0 {
                    out.push(',');
                }
                let case_sym = &scope.symbols[case_idx.index()];
                out.push_str("{\"name\":");
                push_json_str(&mut out, &case_sym.name);
                out.push_str(",\"fields\":[");
                let case_ref = SymRef::new(m.id, case_idx);
                if let Some(&di) = data_by_sym.get(&case_ref) {
                    let ds = &prog.signatures.data[di];
                    for fi in 0..ds.fields.len() {
                        if fi > 0 {
                            out.push(',');
                        }
                        let field = ds.field_by_index(fi);
                        let mut ty_str = String::new();
                        format_source_type(prog, field.ty, &mut ty_str);
                        out.push_str("{\"name\":");
                        push_json_str(&mut out, field.name.as_deref().unwrap_or(""));
                        out.push_str(",\"type\":");
                        push_json_str(&mut out, &ty_str);
                        out.push('}');
                    }
                }
                out.push_str("]}");
            }
            out.push_str("]}");
        }
        out.push(']');

        // Constants
        out.push_str(",\"constants\":[");
        let mut first_const = true;
        for c in &prog.constants {
            if c.symbol.module != m.id {
                continue;
            }
            if !first_const {
                out.push(',');
            }
            first_const = false;
            let sym = prog.interfaces.symbol(c.symbol);
            let (line, end_line) = span_lines(sources, sym.declaration_span);
            let mut ty_str = String::new();
            format_source_type(prog, c.ty, &mut ty_str);
            out.push_str("{\"name\":");
            push_json_str(&mut out, &sym.name);
            out.push_str(",\"type\":");
            push_json_str(&mut out, &ty_str);
            let _ = write!(
                out,
                ",\"exported\":{},\"line\":{},\"end_line\":{}}}",
                sym.exported, line, end_line
            );
        }
        out.push(']');

        // Functions
        out.push_str(",\"functions\":[");
        let mut first_fn = true;
        for (fn_i, f) in prog.functions.iter().enumerate() {
            if f.symbol.module != m.id {
                continue;
            }
            if !first_fn {
                out.push(',');
            }
            first_fn = false;
            let sym = prog.interfaces.symbol(f.symbol);
            let (line, end_line) = span_lines(sources, sym.declaration_span);
            let sig = &prog.signatures.functions[f.signature.index()];
            let mut sig_str = String::new();
            format_function_sig(prog, sig, &mut sig_str);
            let mut caps = BTreeSet::new();
            if let Some(ty_id) = sig.ty {
                collect_caps_in_type(prog, ty_id, &mut caps);
            }
            let cap_list: Vec<String> = caps.into_iter().collect();
            let (calls, native_calls) = collect_body_calls(prog, &f.body);
            let promises = fn_promises.get(&fn_i).map(Vec::as_slice).unwrap_or(&[]);
            let examples = fn_examples.get(&fn_i).copied().unwrap_or(0);

            out.push_str("{\"name\":");
            push_json_str(&mut out, &sym.name);
            out.push_str(",\"signature\":");
            push_json_str(&mut out, &sig_str);
            let _ = write!(
                out,
                ",\"exported\":{},\"line\":{},\"end_line\":{},\"examples\":{}",
                sym.exported, line, end_line, examples
            );
            out.push_str(",\"capabilities\":");
            push_str_array(&mut out, &cap_list);
            out.push_str(",\"calls\":");
            push_str_array(&mut out, &calls);
            out.push_str(",\"native_calls\":");
            push_str_array(&mut out, &native_calls);
            out.push_str(",\"promises\":");
            push_str_array(&mut out, promises);
            out.push('}');
        }
        out.push(']');

        // Services & Actors
        out.push_str(",\"services\":[");
        let mut first_svc = true;
        for svc in &prog.services {
            if svc.module != m.id {
                continue;
            }
            if !first_svc {
                out.push(',');
            }
            first_svc = false;
            let span = m.syntax.tree.node(svc.syntax).span;
            let (line, end_line) = span_lines(sources, span);
            let svc_name = svc.actor_symbol.map_or_else(
                || "service".to_string(),
                |s| prog.interfaces.symbol(s).name.clone(),
            );
            let mut state_ty = String::new();
            format_source_type(prog, svc.state, &mut state_ty);
            out.push_str("{\"name\":");
            push_json_str(&mut out, &svc_name);
            out.push_str(",\"state_type\":");
            push_json_str(&mut out, &state_ty);
            let _ = write!(
                out,
                ",\"is_actor\":{},\"line\":{},\"end_line\":{}",
                svc.is_actor, line, end_line
            );
            out.push_str(",\"tools\":[");
            for (ti, tool) in svc.tools.iter().enumerate() {
                if ti > 0 {
                    out.push(',');
                }
                let t_func = prog.function(tool.function);
                let t_sym = prog.interfaces.symbol(t_func.symbol);
                out.push_str("{\"function\":");
                push_json_str(&mut out, &t_sym.name);
                out.push('}');
            }
            out.push_str("]}");
        }
        out.push(']');

        // API blocks
        out.push_str(",\"apis\":[");
        let mut first_api = true;
        for api in &prog.apis {
            if api.module != m.id {
                continue;
            }
            if !first_api {
                out.push(',');
            }
            first_api = false;
            let span = m.syntax.tree.node(api.syntax).span;
            let (line, end_line) = span_lines(sources, span);
            let _ = write!(out, "{{\"line\":{line},\"end_line\":{end_line}");
            out.push_str(",\"capabilities\":");
            push_str_array(&mut out, &api.capabilities);
            out.push_str(",\"routes\":[");
            for (ri, r) in api.routes.iter().enumerate() {
                if ri > 0 {
                    out.push(',');
                }
                let fn_name = prog.interfaces.symbol(prog.function(r.function).symbol);
                out.push_str("{\"method\":");
                push_json_str(&mut out, &format!("{:?}", r.method));
                out.push_str(",\"path\":");
                push_json_str(&mut out, &r.path);
                out.push_str(",\"function\":");
                push_json_str(&mut out, &fn_name.name);
                out.push('}');
            }
            out.push(']');
            out.push_str(",\"jobs\":[");
            for (ji, j) in api.jobs.iter().enumerate() {
                if ji > 0 {
                    out.push(',');
                }
                let fn_name = prog.interfaces.symbol(prog.function(j.function).symbol);
                out.push_str("{\"name\":");
                push_json_str(&mut out, &j.name);
                out.push_str(",\"function\":");
                push_json_str(&mut out, &fn_name.name);
                out.push('}');
            }
            out.push_str("]}");
        }
        out.push(']');

        // Tests / Specs
        out.push_str(",\"specs\":[");
        let mut first_spec = true;
        for spec in &prog.specs {
            if spec.module != m.id {
                continue;
            }
            if !first_spec {
                out.push(',');
            }
            first_spec = false;
            let span = m.syntax.tree.node(spec.syntax).span;
            let (line, end_line) = span_lines(sources, span);
            let kind = match spec.kind {
                CheckedSpecKind::Test => "test",
                CheckedSpecKind::Example => "example",
                CheckedSpecKind::Promise => "promise",
                CheckedSpecKind::Anchor => "anchor",
            };
            let label = spec
                .wording
                .as_deref()
                .and_then(|b| std::str::from_utf8(b).ok())
                .or_else(|| std::str::from_utf8(&spec.text.bytes).ok())
                .unwrap_or("")
                .trim()
                .to_string();
            let subject = spec.subject.map(|fid| {
                let s = prog.interfaces.symbol(prog.function(fid).symbol);
                let sm = prog.modules.module(s.module);
                format!("{}.{}", sm.name, s.name)
            });
            let (calls, _) = collect_body_calls(prog, &spec.body);
            out.push_str("{\"kind\":");
            push_json_str(&mut out, kind);
            out.push_str(",\"label\":");
            push_json_str(&mut out, &label);
            out.push_str(",\"subject\":");
            match subject {
                Some(ref s) => push_json_str(&mut out, s),
                None => out.push_str("null"),
            }
            let _ = write!(out, ",\"line\":{line},\"end_line\":{end_line}");
            out.push_str(",\"calls\":");
            push_str_array(&mut out, &calls);
            out.push('}');
        }
        out.push(']');
        out.push('}');
    }
    out.push(']');
    out.push('}');
    out.push('\n');
    let _ = ModuleId(0);
    out
}

/// Executes `mo index [--json] [--lib DIR] FILE` and writes a deterministic JSON symbol/call/test graph.
#[must_use]
pub fn run_index(
    cli: &Cli,
    output: &mut (dyn Write + Send),
    errors: &mut (dyn Write + Send),
) -> i32 {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();

    let library = match &cli.library {
        Some(dir) => LibraryMode::Directory(bytes_to_path(dir).into_owned()),
        None => LibraryMode::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        },
    };
    let load_opts = LoadOptions {
        library,
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };

    let entry = cli.file(0);
    let Some(modules) = load_program_bytes(&mut sources, entry, &load_opts, &mut diags) else {
        return write_diagnostics(
            output,
            errors,
            cli.json,
            cli.command,
            &sources,
            &mut diags,
            None,
        );
    };
    if diags.has_any() {
        return write_diagnostics(
            output,
            errors,
            cli.json,
            cli.command,
            &sources,
            &mut diags,
            None,
        );
    }

    let Some(program) = check_program(Some(&sources), modules, &mut diags) else {
        return write_diagnostics(
            output,
            errors,
            cli.json,
            cli.command,
            &sources,
            &mut diags,
            None,
        );
    };
    if diags.has_any() {
        return write_diagnostics(
            output,
            errors,
            cli.json,
            cli.command,
            &sources,
            &mut diags,
            None,
        );
    }

    let rendered = render_program_index(&program, &sources);
    if output.write_all(rendered.as_bytes()).is_err() || output.flush().is_err() {
        let _ = writeln!(errors, "mo index: could not write output");
        let _ = errors.flush();
        return 1;
    }
    0
}
