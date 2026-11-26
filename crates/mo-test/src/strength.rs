//! Promise/example strength measurement against curated wrong-version sets
//! (`src/tools/strength.{h,c}`).

use std::fs;
use std::io::Write;
use std::path::Path;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use mo_base::{
    DiagBag, Diagnostic, RenderWriter, SourceMap, Span, TerminalEncoding, render_version_json,
};
use mo_check::{
    CheckedBody, CheckedPatternId, CheckedProgram, CheckedSpecKind, ConstructorRef, LibraryMode,
    LoadOptions, ModuleId, SourceTypeKind, SymRef, TypeId, check_program, load_bundle_with_library,
};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_FILE_SIZE, MO_LIMIT_PATH_SIZE,
    MO_LIMIT_STRENGTH_VERSIONS, MO_SOURCE_STACK_SIZE,
};
use mo_syntax::{ModuleSyntax, NodeId, SyntaxKind, SyntaxTree, parse_module};
use sha2::{Digest, Sha256};

use crate::render::{TestTotals, UNICODE16_METRICS, detect_terminal_encoding};
use crate::runner::{TestConfig, TestSelection, run_tests, select_tests, test_report_error};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(usize)]
enum Outcome {
    Invalid = 0,
    ChangesIntent = 1,
    KilledByPromise = 2,
    KilledByExample = 3,
    KilledByBoth = 4,
    KilledByTest = 5,
    Survived = 6,
    Defect = 7,
    Unfinished = 8,
    NotCounted = 9,
}

const OUTCOMES: usize = 10;

const OUTCOME_NAMES: [&str; OUTCOMES] = [
    "invalid",
    "changes_intent",
    "killed_by_promise",
    "killed_by_example",
    "killed_by_both",
    "killed_by_test",
    "survived",
    "defect",
    "unfinished",
    "not_counted",
];

const OUTCOME_SENTENCES: [&str; OUTCOMES] = [
    "is invalid",
    "changes intent",
    "is killed by a promise",
    "is killed by an example",
    "is killed by a promise and an example",
    "is killed by a test",
    "survives",
    "stops on a defect",
    "did not finish its plan",
    "is not counted",
];

struct Version {
    name: String,
    sha256: [u8; 32],
    listed: bool,
    outcome: Outcome,
    reason: String,
}

struct VersionSet {
    versions: Vec<Version>,
    counted: bool,
}

fn truncate_reason(text: &str) -> String {
    // C uses `char reason[160]` (159 bytes + NUL).
    if text.len() <= 159 {
        return text.to_string();
    }
    let mut end = 159;
    while end > 0 && !text.is_char_boundary(end) {
        end -= 1;
    }
    text[..end].to_string()
}

fn join_path(folder: &str, name: &str) -> Option<String> {
    let full = format!("{folder}/{name}");
    if full.len() as u64 > MO_LIMIT_PATH_SIZE {
        None
    } else {
        Some(full)
    }
}

fn read_counted(set: &mut VersionSet, folder: &str) -> Option<&'static str> {
    let Some(file) = join_path(folder, "counted") else {
        return Some("--strength names a folder whose path is too long");
    };
    let bytes = match fs::read(&file) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return None,
        Err(_) => return Some("the counted file could not be read"),
    };
    set.counted = true;
    let mut start = 0usize;
    while start < bytes.len() {
        let Some(rel_nl) = bytes[start..].iter().position(|&b| b == b'\n') else {
            return Some("a line of the counted file is too long or does not end");
        };
        // Match C's `char line[NAME_MAX + 2]` (255 + '\n' + '\0').
        if rel_nl > 255 {
            return Some("a line of the counted file is too long or does not end");
        }
        let line_bytes = &bytes[start..start + rel_nl];
        start += rel_nl + 1;
        let Ok(line) = std::str::from_utf8(line_bytes) else {
            return Some(
                "the counted file names a wrong version the folder lacks, or names one twice",
            );
        };
        match set.versions.binary_search_by(|v| v.name.as_str().cmp(line)) {
            Ok(idx) if !set.versions[idx].listed => {
                set.versions[idx].listed = true;
            }
            _ => {
                return Some(
                    "the counted file names a wrong version the folder lacks, or names one twice",
                );
            }
        }
    }
    None
}

fn read_set(folder: &str) -> Result<VersionSet, &'static str> {
    let entries =
        fs::read_dir(folder).map_err(|_| "--strength names a folder that could not be read")?;
    let mut versions = Vec::new();
    for entry_res in entries {
        let Ok(entry) = entry_res else {
            continue;
        };
        let os_name = entry.file_name();
        let Some(name) = os_name.to_str() else {
            continue;
        };
        if name.len() < 4 || !name.ends_with(".mo") {
            continue;
        }
        if versions.len() as u64 == MO_LIMIT_STRENGTH_VERSIONS {
            return Err("the folder holds more wrong versions than mo test --strength measures");
        }
        versions.push(Version {
            name: name.to_string(),
            sha256: [0u8; 32],
            listed: false,
            outcome: Outcome::Invalid,
            reason: String::new(),
        });
    }
    if versions.is_empty() {
        return Err("--strength names a folder with no wrong versions (.mo files)");
    }
    versions.sort_by(|a, b| a.name.cmp(&b.name));
    let mut set = VersionSet {
        versions,
        counted: false,
    };
    if let Some(mistake) = read_counted(&mut set, folder) {
        return Err(mistake);
    }
    Ok(set)
}

fn is_declaration(kind: SyntaxKind) -> bool {
    matches!(
        kind,
        SyntaxKind::Fn | SyntaxKind::Const | SyntaxKind::Struct | SyntaxKind::Enum
    )
}

fn node_text_eq(
    a_tree: &SyntaxTree,
    a: Option<NodeId>,
    b_tree: &SyntaxTree,
    b: Option<NodeId>,
) -> bool {
    let (Some(an), Some(bn)) = (a, b) else {
        return false;
    };
    a_tree.node_text(an) == b_tree.node_text(bn)
}

fn body_start(tree: &SyntaxTree, fn_node: NodeId) -> u32 {
    let node = tree.node(fn_node);
    let mut at = node.span.start;
    for i in 0..5.min(node.children.len()) {
        let Some(part_id) = node.children[i] else {
            continue;
        };
        let part = tree.node(part_id);
        if i == 4 && part.kind == SyntaxKind::Sequence {
            for child_opt in &part.children {
                if let Some(cid) = *child_opt {
                    let end = tree.node(cid).span.end;
                    if end > at {
                        at = end;
                    }
                }
            }
        } else if part.span.end > at {
            at = part.span.end;
        }
    }
    at
}

fn module_named(tree: &SyntaxTree, path_node: Option<NodeId>, name: &str) -> bool {
    let Some(pid) = path_node else {
        return false;
    };
    let path = tree.node(pid);
    if path.children.is_empty() {
        return false;
    }
    let mut built = String::new();
    for (i, child_opt) in path.children.iter().enumerate() {
        let Some(cid) = *child_opt else {
            return false;
        };
        let Some(bytes) = tree.node_text(cid) else {
            return false;
        };
        let Ok(part) = std::str::from_utf8(bytes) else {
            return false;
        };
        if i != 0 {
            built.push('.');
        }
        built.push_str(part);
    }
    built == name
}

struct Splice {
    start: u32,
    end: u32,
    bytes: Vec<u8>,
}

fn apply_replacement(
    program: &CheckedProgram,
    sources: &SourceMap,
    v: &mut Version,
    wrong_sources: &SourceMap,
    replacement: &ModuleSyntax,
) -> Option<SourceMap> {
    let mut target_module = None;
    for (idx, m) in program.modules.modules.iter().enumerate() {
        if !m.library
            && module_named(
                &replacement.tree,
                replacement.header.as_ref().map(|h| h.path),
                &m.name,
            )
        {
            target_module = Some(idx);
            break;
        }
    }
    let Some(module_idx) = target_module else {
        v.reason = truncate_reason("its module header names none of the program's modules");
        return None;
    };
    let file_id = program.modules.modules[module_idx].syntax.span.file;
    let mut parse_diags = DiagBag::new();
    let original = parse_module(sources, file_id, &mut parse_diags)?;
    if parse_diags.has_any() {
        return None;
    }
    let rep_root = replacement.tree.node(replacement.root);
    let orig_root = original.tree.node(original.root);
    let rep_items_id = rep_root.children.get(1).copied().flatten()?;
    let orig_items_id = orig_root.children.get(1).copied().flatten()?;
    let rep_items: Vec<NodeId> = replacement
        .tree
        .node(rep_items_id)
        .children
        .iter()
        .copied()
        .flatten()
        .collect();
    let orig_items: Vec<NodeId> = original
        .tree
        .node(orig_items_id)
        .children
        .iter()
        .copied()
        .flatten()
        .collect();
    if rep_items.is_empty() {
        v.reason = truncate_reason("it replaces no declarations");
        return None;
    }
    let wrong_bytes = wrong_sources.get(0)?.bytes();
    let mut splices = Vec::with_capacity(rep_items.len());
    for (i, &item_id) in rep_items.iter().enumerate() {
        let item = replacement.tree.node(item_id);
        if !is_declaration(item.kind) {
            v.reason =
                truncate_reason("it holds something other than functions, constants and types");
            return None;
        }
        let item_name = item.children.first().copied().flatten();
        let mut target = None;
        let mut matches = 0u32;
        for &orig_id in &orig_items {
            let on = original.tree.node(orig_id);
            let on_name = on.children.first().copied().flatten();
            if on.kind == item.kind
                && node_text_eq(&original.tree, on_name, &replacement.tree, item_name)
            {
                if matches == 0 {
                    target = Some(orig_id);
                }
                matches += 1;
            }
        }
        for &prev_id in &rep_items[..i] {
            let pn = replacement.tree.node(prev_id);
            let pn_name = pn.children.first().copied().flatten();
            if node_text_eq(&replacement.tree, pn_name, &replacement.tree, item_name) {
                matches = 0;
            }
        }
        let Some(target_id) = target.filter(|_| matches == 1) else {
            v.reason = truncate_reason(
                "a declaration it replaces is missing, overloaded or replaced twice",
            );
            return None;
        };
        let is_fn = item.kind == SyntaxKind::Fn;
        let from = if is_fn {
            body_start(&replacement.tree, item_id)
        } else {
            item.span.start
        };
        let target_node = original.tree.node(target_id);
        let to_start = if is_fn {
            body_start(&original.tree, target_id)
        } else {
            target_node.span.start
        };
        let rep_slice = wrong_bytes.get(from as usize..item.span.end as usize)?;
        splices.push(Splice {
            start: to_start,
            end: target_node.span.end,
            bytes: rep_slice.to_vec(),
        });
    }
    splices.sort_by_key(|s| s.start);
    let orig_bytes = sources.get(file_id)?.bytes();
    let mut spliced = Vec::new();
    let mut at = 0usize;
    for sp in &splices {
        let start = sp.start as usize;
        let end = sp.end as usize;
        if start < at || end > orig_bytes.len() {
            v.reason = truncate_reason("the replaced program exceeds the source limits");
            return None;
        }
        spliced.extend_from_slice(&orig_bytes[at..start]);
        spliced.extend_from_slice(&sp.bytes);
        at = end;
    }
    spliced.extend_from_slice(&orig_bytes[at..]);
    if spliced.len() as u64 > MO_LIMIT_FILE_SIZE {
        v.reason = truncate_reason("the replaced program exceeds the source limits");
        return None;
    }

    let mut out = SourceMap::new();
    for (k, m) in program.modules.modules.iter().enumerate() {
        if m.library {
            continue;
        }
        let src = sources.get(m.syntax.span.file)?;
        let bytes = if k == module_idx {
            spliced.clone()
        } else {
            src.bytes().to_vec()
        };
        if out.add_bytes(src.path_bytes().to_vec(), bytes).is_err() {
            v.reason = truncate_reason("the replaced program exceeds the source limits");
            return None;
        }
    }
    Some(out)
}

fn classify(totals: &TestTotals, diags: &DiagBag) -> Outcome {
    let promise = totals.failed > totals.failed_examples + totals.failed_tests;
    let example = totals.failed_examples != 0;
    if promise && example {
        return Outcome::KilledByBoth;
    }
    if promise {
        return Outcome::KilledByPromise;
    }
    if example {
        return Outcome::KilledByExample;
    }
    let unfinished = diags
        .items()
        .iter()
        .any(|d| d.code != ErrorCode::ToolTestInitialization);
    if unfinished || diags.dropped() != 0 {
        return Outcome::Unfinished;
    }
    if totals.stopped > totals.stopped_tests {
        return Outcome::Defect;
    }
    if totals.failed_tests != 0 || totals.stopped_tests != 0 {
        return Outcome::KilledByTest;
    }
    if !diags.is_empty() {
        Outcome::Defect
    } else {
        Outcome::Survived
    }
}

struct Reach {
    fns: Vec<bool>,
    consts: Vec<bool>,
    types: Vec<Vec<bool>>,
}

fn mark_type(p: &CheckedProgram, ty: TypeId, r: &mut Reach) {
    let st = p.signatures.types.get(ty);
    if st.kind == SourceTypeKind::Named
        && let Some(sym) = st.owner.as_source()
        && let Some(mod_id) = st.module
        && (mod_id.0 as usize) < p.modules.modules.len()
        && !p.modules.module(mod_id).library
    {
        let idx = sym.symbol.index();
        if idx < r.types[mod_id.0 as usize].len() {
            r.types[mod_id.0 as usize][idx] = true;
        }
    }
    let args: Vec<TypeId> = st.arguments.clone();
    for arg in args {
        mark_type(p, arg, r);
    }
}

fn mark_symbol(
    p: &CheckedProgram,
    sym_ref: SymRef,
    subject_fn: Option<usize>,
    r: &mut Reach,
    work_fns: &mut Vec<usize>,
    work_consts: &mut Vec<usize>,
) {
    let mod_idx = sym_ref.module.0 as usize;
    if mod_idx >= p.modules.modules.len() || p.modules.module(sym_ref.module).library {
        return;
    }
    for (f, func) in p.functions.iter().enumerate() {
        if func.symbol == sym_ref {
            if Some(f) != subject_fn && !r.fns[f] {
                r.fns[f] = true;
                work_fns.push(f);
            }
            return;
        }
    }
    for (c, constant) in p.constants.iter().enumerate() {
        if constant.symbol == sym_ref {
            if !r.consts[c] {
                r.consts[c] = true;
                work_consts.push(c);
            }
            return;
        }
    }
    let sym = p.interfaces.symbol(sym_ref);
    let target = sym
        .owner
        .map_or_else(|| sym_ref.symbol.index(), |o| o.index());
    if target < r.types[mod_idx].len() {
        r.types[mod_idx][target] = true;
    }
}

fn mark_pattern(
    p: &CheckedProgram,
    body: &CheckedBody,
    pat_id: CheckedPatternId,
    subject_fn: Option<usize>,
    r: &mut Reach,
    work_fns: &mut Vec<usize>,
    work_consts: &mut Vec<usize>,
) {
    let pat = body.pattern(pat_id);
    mark_type(p, pat.ty, r);
    if let Some(ConstructorRef::Source(sym)) = pat.constructor {
        mark_symbol(p, sym, subject_fn, r, work_fns, work_consts);
    }
    for part in &pat.parts {
        mark_pattern(p, body, part.pattern, subject_fn, r, work_fns, work_consts);
    }
}

fn scan_body(
    p: &CheckedProgram,
    body: &CheckedBody,
    subject_fn: Option<usize>,
    r: &mut Reach,
    work_fns: &mut Vec<usize>,
    work_consts: &mut Vec<usize>,
) {
    for expr in &body.items {
        mark_type(p, expr.ty, r);
        if let Some(sym) = expr.symbol {
            mark_symbol(p, sym, subject_fn, r, work_fns, work_consts);
        }
        if let Some(pat_id) = expr.pattern {
            mark_pattern(p, body, pat_id, subject_fn, r, work_fns, work_consts);
        }
        if let Some(match_id) = expr.match_info {
            let m = body.match_info(match_id);
            for arm in &m.arms {
                for &pat_id in &arm.patterns {
                    mark_pattern(p, body, pat_id, subject_fn, r, work_fns, work_consts);
                }
            }
        }
    }
}

fn hash_bytes(h: &mut Sha256, b: &[u8]) {
    let n = b.len();
    let len = [(n >> 24) as u8, (n >> 16) as u8, (n >> 8) as u8, n as u8];
    h.update(len);
    if !b.is_empty() {
        h.update(b);
    }
}

fn hash_str(h: &mut Sha256, s: &str) {
    hash_bytes(h, s.as_bytes());
}

fn hash_fn_without_specs(
    h: &mut Sha256,
    program: &CheckedProgram,
    sources: &SourceMap,
    module: ModuleId,
    decl_id: NodeId,
) {
    let tree = &program.modules.module(module).syntax.tree;
    let decl = tree.node(decl_id);
    let Some(src) = sources.get(decl.span.file) else {
        return;
    };
    let bytes = src.bytes();
    let specs_node = decl
        .children
        .get(4)
        .copied()
        .flatten()
        .map(|id| tree.node(id));
    let Some(specs) = specs_node.filter(|n| !n.children.is_empty()) else {
        hash_bytes(h, &bytes[decl.span.start as usize..decl.span.end as usize]);
        return;
    };
    let first_id = specs.children.iter().copied().flatten().next();
    let last_id = specs.children.iter().rev().copied().flatten().next();
    let (Some(first), Some(last)) = (first_id, last_id) else {
        hash_bytes(h, &bytes[decl.span.start as usize..decl.span.end as usize]);
        return;
    };
    let first_spec = tree.node(first).span.start;
    let last_spec = tree.node(last).span.end;
    hash_bytes(h, &bytes[decl.span.start as usize..first_spec as usize]);
    hash_bytes(h, &bytes[last_spec as usize..decl.span.end as usize]);
}

fn spec_digests(program: &CheckedProgram, sources: &SourceMap) -> Vec<[u8; 32]> {
    let mut total = 0usize;
    for s in &program.specs {
        if !program.modules.module(s.module).library
            && matches!(s.kind, CheckedSpecKind::Promise | CheckedSpecKind::Example)
        {
            total += 1;
        }
    }
    for svc in &program.services {
        if !program.modules.module(svc.module).library {
            total += svc.promises.len();
        }
    }
    if total == 0 {
        return Vec::new();
    }

    let mut r = Reach {
        fns: vec![false; program.functions.len()],
        consts: vec![false; program.constants.len()],
        types: program
            .modules
            .modules
            .iter()
            .enumerate()
            .map(|(m, _)| vec![false; program.interfaces.scope(ModuleId(m as u32)).symbols.len()])
            .collect(),
    };
    let mut digests = Vec::with_capacity(total);

    let spec_count = program.specs.len();
    let total_groups = spec_count + program.services.len();
    for i in 0..total_groups {
        let is_service_group = i >= spec_count;
        let svc_idx = if is_service_group { i - spec_count } else { 0 };
        if is_service_group
            && program
                .modules
                .module(program.services[svc_idx].module)
                .library
        {
            continue;
        }
        let sub_count = if is_service_group {
            program.services[svc_idx].promises.len()
        } else {
            1
        };
        for sub in 0..sub_count {
            let (s, sp_anchors) = if is_service_group {
                let sp = &program.services[svc_idx].promises[sub];
                (&sp.spec, Some(&sp.anchors))
            } else {
                let spec = &program.specs[i];
                if program.modules.module(spec.module).library
                    || !matches!(
                        spec.kind,
                        CheckedSpecKind::Promise | CheckedSpecKind::Example
                    )
                {
                    continue;
                }
                (spec, None)
            };

            r.fns.fill(false);
            r.consts.fill(false);
            for t in &mut r.types {
                t.fill(false);
            }
            let mut work_fns = Vec::new();
            let mut work_consts = Vec::new();
            let subject_fn = if is_service_group {
                None
            } else {
                s.subject.map(|id| id.index())
            };
            scan_body(
                program,
                &s.body,
                subject_fn,
                &mut r,
                &mut work_fns,
                &mut work_consts,
            );
            if !is_service_group && s.kind == CheckedSpecKind::Promise {
                for a in &program.specs {
                    if a.kind == CheckedSpecKind::Anchor
                        && a.promise == Some(mo_check::CheckedSpecId(i as u32))
                    {
                        scan_body(
                            program,
                            &a.body,
                            subject_fn,
                            &mut r,
                            &mut work_fns,
                            &mut work_consts,
                        );
                    }
                }
            } else if let Some(anchors) = sp_anchors {
                for a in anchors {
                    scan_body(
                        program,
                        &a.body,
                        subject_fn,
                        &mut r,
                        &mut work_fns,
                        &mut work_consts,
                    );
                }
            }

            let mut fi = 0usize;
            let mut ci = 0usize;
            while fi < work_fns.len() || ci < work_consts.len() {
                while fi < work_fns.len() {
                    let f = work_fns[fi];
                    fi += 1;
                    scan_body(
                        program,
                        &program.functions[f].body,
                        subject_fn,
                        &mut r,
                        &mut work_fns,
                        &mut work_consts,
                    );
                }
                while ci < work_consts.len() {
                    let c = work_consts[ci];
                    ci += 1;
                    scan_body(
                        program,
                        &program.constants[c].initializer,
                        subject_fn,
                        &mut r,
                        &mut work_fns,
                        &mut work_consts,
                    );
                }
            }

            let mut h = Sha256::new();
            // Match C's `enum mo_checked_spec_kind`: EXAMPLE=0, TEST=1, PROMISE=2, ANCHOR=3; service=3.
            let kind_tag: u8 = if is_service_group {
                3
            } else {
                match s.kind {
                    CheckedSpecKind::Example => 0,
                    CheckedSpecKind::Test => 1,
                    CheckedSpecKind::Promise => 2,
                    CheckedSpecKind::Anchor => 3,
                }
            };
            h.update([kind_tag]);
            hash_str(&mut h, &program.modules.module(s.module).name);
            if !is_service_group && let Some(subj_id) = s.subject {
                let sym = program.interfaces.symbol(program.function(subj_id).symbol);
                hash_str(&mut h, &sym.name);
            }
            hash_bytes(&mut h, &s.text.bytes);
            for (f, &reached) in r.fns.iter().enumerate() {
                if reached {
                    let sym = program.interfaces.symbol(program.functions[f].symbol);
                    hash_str(&mut h, &program.modules.module(sym.module).name);
                    hash_fn_without_specs(&mut h, program, sources, sym.module, sym.declaration);
                }
            }
            for (c, &reached) in r.consts.iter().enumerate() {
                if reached {
                    let sym = program.interfaces.symbol(program.constants[c].symbol);
                    let decl = program
                        .modules
                        .module(sym.module)
                        .syntax
                        .tree
                        .node(sym.declaration);
                    if let Some(src) = sources.get(decl.span.file) {
                        hash_str(&mut h, &program.modules.module(sym.module).name);
                        hash_bytes(
                            &mut h,
                            &src.bytes()[decl.span.start as usize..decl.span.end as usize],
                        );
                    }
                }
            }
            for (m, reached_types) in r.types.iter().enumerate() {
                let mod_id = ModuleId(m as u32);
                let scope = program.interfaces.scope(mod_id);
                let tree = &program.modules.module(mod_id).syntax.tree;
                for (k, &reached) in reached_types.iter().enumerate() {
                    if reached {
                        let decl = tree.node(scope.symbols[k].declaration);
                        if let Some(src) = sources.get(decl.span.file) {
                            hash_str(&mut h, &program.modules.module(mod_id).name);
                            hash_bytes(
                                &mut h,
                                &src.bytes()[decl.span.start as usize..decl.span.end as usize],
                            );
                        }
                    }
                }
            }
            let digest: [u8; 32] = h.finalize().into();
            digests.push(digest);
        }
    }
    digests.sort_unstable();
    digests
}

fn select_checks(
    program: &CheckedProgram,
    config: &TestConfig,
    diags: &mut DiagBag,
) -> Option<TestSelection> {
    let mut selection = select_tests(program, config, diags)?;
    selection.mistake = None;
    Some(selection)
}

struct StrengthContext<'a> {
    config: &'a TestConfig,
    strength_dir: &'a str,
    library: &'a LibraryMode,
    program: &'a CheckedProgram,
    sources: &'a SourceMap,
    baseline_digests: &'a [[u8; 32]],
    counted_file: bool,
}

fn measure_version(ctx: &StrengthContext<'_>, base: &Version) -> Version {
    let mut v = Version {
        name: base.name.clone(),
        sha256: base.sha256,
        listed: base.listed,
        outcome: Outcome::Invalid,
        reason: String::new(),
    };
    let counted = !ctx.counted_file || v.listed;
    measure_one(
        ctx.config,
        ctx.strength_dir,
        ctx.library,
        ctx.program,
        ctx.sources,
        ctx.baseline_digests,
        &mut v,
        counted,
    );
    v
}

#[expect(
    clippy::too_many_arguments,
    reason = "matches C measure_one signature directly"
)]
fn measure_one(
    config: &TestConfig,
    strength_dir: &str,
    library: &LibraryMode,
    program: &CheckedProgram,
    sources: &SourceMap,
    baseline_digests: &[[u8; 32]],
    v: &mut Version,
    counted: bool,
) {
    v.outcome = Outcome::Invalid;
    let Some(file_path) = join_path(strength_dir, &v.name) else {
        v.reason = truncate_reason("it could not be read");
        return;
    };
    let mut wrong_sources = SourceMap::new();
    if wrong_sources.add_file(Path::new(&file_path)).is_err() {
        v.reason = truncate_reason("it could not be read");
        return;
    }
    let Some(wrong_src) = wrong_sources.get(0) else {
        v.reason = truncate_reason("it could not be read");
        return;
    };
    let mut hasher = Sha256::new();
    hasher.update(wrong_src.bytes());
    v.sha256 = hasher.finalize().into();
    if !counted {
        v.outcome = Outcome::NotCounted;
        return;
    }
    let mut diags = DiagBag::new();
    let replacement = parse_module(&wrong_sources, 0, &mut diags);
    let Some(rep) = replacement
        .as_ref()
        .filter(|r| !diags.has_any() && r.header.is_some())
    else {
        let msg = if replacement.is_some() && !diags.has_any() {
            "it has no module header"
        } else {
            "it does not parse"
        };
        v.reason = truncate_reason(msg);
        return;
    };
    let Some(mut replaced_sources) = apply_replacement(program, sources, v, &wrong_sources, rep)
    else {
        return;
    };
    let load_opts = LoadOptions {
        library: library.clone(),
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let checked = load_bundle_with_library(&mut replaced_sources, &load_opts, &mut diags)
        .filter(|_| !diags.has_any())
        .and_then(|set| check_program(Some(&replaced_sources), set, &mut diags));
    let Some(checked) = checked.filter(|_| !diags.has_any()) else {
        if let Some(first) = diags.items().first() {
            let formatted = format!("E{:04}: {}", first.code.number(), first.message);
            v.reason = truncate_reason(&formatted);
        } else {
            v.reason = truncate_reason("it does not check");
        }
        return;
    };
    let digests = spec_digests(&checked, &replaced_sources);
    if digests != baseline_digests {
        v.outcome = Outcome::ChangesIntent;
        return;
    }
    let selection = select_checks(&checked, config, &mut diags);
    let mut totals = TestTotals::default();
    if let Some(sel) = selection {
        let mut sink = std::io::sink();
        run_tests(
            config,
            &checked,
            &replaced_sources,
            &sel,
            &mut sink,
            &mut diags,
            &mut totals,
        );
    }
    v.outcome = classify(&totals, &diags);
}

fn hex_sha256(bytes: &[u8; 32]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(64);
    for &b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

fn render_strength(
    strength_dir: &str,
    json: bool,
    set: &VersionSet,
    original: bool,
) -> Option<Vec<u8>> {
    let mut counts = [0u64; OUTCOMES];
    let mut killed = 0u64;
    let mut any = 0u64;
    let mut scored = 0u64;
    if original {
        for v in &set.versions {
            counts[v.outcome as usize] += 1;
            let kill = matches!(
                v.outcome,
                Outcome::KilledByPromise | Outcome::KilledByExample | Outcome::KilledByBoth
            );
            if kill {
                killed += 1;
            }
            if kill || v.outcome == Outcome::KilledByTest {
                any += 1;
            }
            if kill
                || v.outcome == Outcome::KilledByTest
                || v.outcome == Outcome::Survived
                || (v.listed && matches!(v.outcome, Outcome::ChangesIntent | Outcome::Invalid))
            {
                scored += 1;
            }
        }
    }
    let rate = if scored == 0 {
        "null".to_string()
    } else if json {
        format!("{:.4}", (killed as f64) / (scored as f64))
    } else {
        format!("{:.1}%", 100.0 * (killed as f64) / (scored as f64))
    };

    let encoding = if json {
        TerminalEncoding::Utf8
    } else {
        detect_terminal_encoding()
    };
    let mut w = RenderWriter::new(MO_CLI_DIAGNOSTIC_SIZE as usize, encoding, UNICODE16_METRICS);
    if json {
        w.text("{\"record\":\"strength\",\"version\":2,\"set\":");
        w.json_str(strength_dir);
        w.text(",\"counted_file\":");
        w.text(if set.counted { "true" } else { "false" });
        w.text(",\"original\":");
        w.text(if original { "\"passed\"" } else { "\"failed\"" });
        w.text(",\"versions\":[");
        if original {
            for (i, v) in set.versions.iter().enumerate() {
                w.text(if i == 0 { "{\"name\":" } else { ",{\"name\":" });
                w.json_str(&v.name);
                w.text(",\"sha256\":\"");
                w.text(&hex_sha256(&v.sha256));
                w.text("\",\"outcome\":\"");
                w.text(OUTCOME_NAMES[v.outcome as usize]);
                w.text("\"");
                if v.outcome == Outcome::Invalid {
                    w.text(",\"reason\":");
                    w.json_str(&v.reason);
                }
                w.text("}");
            }
        }
        w.text("],\"counts\":{");
        for i in 0..OUTCOMES {
            w.text(if i == 0 { "\"" } else { ",\"" });
            w.text(OUTCOME_NAMES[i]);
            w.text("\":");
            w.u64_dec(counts[i]);
        }
        w.text("},\"killed\":");
        w.u64_dec(killed);
        w.text(",\"killed_any\":");
        w.u64_dec(any);
        w.text(",\"scored\":");
        w.u64_dec(scored);
        w.text(",\"rate\":");
        w.text(&rate);
        w.text(",\"partial\":");
        w.text(if original && counts[Outcome::Unfinished as usize] == 0 {
            "false"
        } else {
            "true"
        });
        w.text("}\n");
    } else if !original {
        w.text("strength: the program fails its own checks, so no wrong version was measured\n");
    } else {
        for v in &set.versions {
            w.text("strength: ");
            w.safe_str(&v.name);
            w.text(" ");
            w.text(OUTCOME_SENTENCES[v.outcome as usize]);
            if v.outcome == Outcome::Invalid {
                w.text(": ");
                w.safe_str(&v.reason);
            }
            w.text("\n");
        }
        w.text("strength: ");
        w.u64_dec(killed);
        w.text(" of ");
        w.u64_dec(scored);
        w.text(" wrong versions killed (");
        w.text(if scored == 0 { "no rate" } else { &rate });
        w.text("), ");
        w.u64_dec(any);
        w.text(" by any check; ");
        let tail = [
            Outcome::Invalid,
            Outcome::ChangesIntent,
            Outcome::Defect,
            Outcome::Unfinished,
            Outcome::NotCounted,
        ];
        for (idx, kind) in tail.iter().enumerate() {
            w.u64_dec(counts[*kind as usize]);
            w.text(" ");
            w.text(OUTCOME_NAMES[*kind as usize]);
            w.text(if idx + 1 == tail.len() { "\n" } else { ", " });
        }
    }
    w.finish().ok()
}

fn write_strength_events(
    output: &mut dyn Write,
    strength_dir: &str,
    set: &VersionSet,
    original: bool,
) -> bool {
    let mut counts = [0u64; OUTCOMES];
    let mut killed = 0u64;
    let mut any = 0u64;
    let mut scored = 0u64;
    if original {
        for v in &set.versions {
            counts[v.outcome as usize] += 1;
            let kill = matches!(
                v.outcome,
                Outcome::KilledByPromise | Outcome::KilledByExample | Outcome::KilledByBoth
            );
            let kill_any = kill || v.outcome == Outcome::KilledByTest;
            let is_scored = kill_any
                || v.outcome == Outcome::Survived
                || (v.listed && matches!(v.outcome, Outcome::ChangesIntent | Outcome::Invalid));
            if kill {
                killed += 1;
            }
            if kill_any {
                any += 1;
            }
            if is_scored {
                scored += 1;
            }

            let mut cw = RenderWriter::new(
                MO_CLI_DIAGNOSTIC_SIZE as usize,
                TerminalEncoding::Utf8,
                UNICODE16_METRICS,
            );
            cw.text("{\"record\":\"strength_case\",\"event\":\"strength_case\",\"set\":");
            cw.json_str(strength_dir);
            cw.text(",\"name\":");
            cw.json_str(&v.name);
            cw.text(",\"sha256\":\"");
            cw.text(&hex_sha256(&v.sha256));
            cw.text("\",\"outcome\":\"");
            cw.text(OUTCOME_NAMES[v.outcome as usize]);
            cw.text("\",\"killed\":");
            cw.text(if kill { "true" } else { "false" });
            cw.text(",\"killed_any\":");
            cw.text(if kill_any { "true" } else { "false" });
            cw.text(",\"scored\":");
            cw.text(if is_scored { "true" } else { "false" });
            if v.outcome == Outcome::Invalid {
                cw.text(",\"reason\":");
                cw.json_str(&v.reason);
            }
            cw.text("}\n");
            let Ok(buf) = cw.finish() else {
                return false;
            };
            if output.write_all(&buf).is_err() || output.flush().is_err() {
                return false;
            }
        }
    }

    let kill_rate = if scored == 0 {
        "null".to_string()
    } else {
        format!("{:.4}", (killed as f64) / (scored as f64))
    };
    let partial = !(original && counts[Outcome::Unfinished as usize] == 0);

    let mut sw = RenderWriter::new(
        MO_CLI_DIAGNOSTIC_SIZE as usize,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    );
    sw.text("{\"record\":\"strength_summary\",\"event\":\"strength_summary\",\"set\":");
    sw.json_str(strength_dir);
    sw.text(",\"original\":");
    sw.text(if original { "\"passed\"" } else { "\"failed\"" });
    sw.text(",\"scored\":");
    sw.u64_dec(scored);
    sw.text(",\"killed\":");
    sw.u64_dec(killed);
    sw.text(",\"killed_any\":");
    sw.u64_dec(any);
    sw.text(",\"survived\":");
    sw.u64_dec(counts[Outcome::Survived as usize]);
    sw.text(",\"kill_rate\":");
    sw.text(&kill_rate);
    sw.text(",\"partial\":");
    sw.text(if partial { "true" } else { "false" });
    sw.text("}\n");
    let Ok(buf) = sw.finish() else {
        return false;
    };
    output.write_all(&buf).is_ok() && output.flush().is_ok()
}

fn emit_unfinished(diags: &mut DiagBag, detail: &str) {
    let _ = diags.push(
        Diagnostic::new(
            ErrorCode::PlanUnfinished,
            Span::NONE,
            "the plan did not finish",
        )
        .with_primary_text(detail),
    );
}

/// Runs `mo test --strength FOLDER` (`src/tools/strength.c:mo_strength`).
#[expect(
    clippy::too_many_arguments,
    reason = "matches C mo_strength run context fields"
)]
pub fn run_strength(
    config: &TestConfig,
    strength_dir: &str,
    library: &LibraryMode,
    mo_version: &str,
    program: &CheckedProgram,
    sources: &SourceMap,
    output: &mut dyn Write,
    diags: &mut DiagBag,
    totals: &mut TestTotals,
    version_written: &mut bool,
    mistake: &mut Option<&'static str>,
) {
    let mut set = match read_set(strength_dir) {
        Ok(s) => s,
        Err(m) => {
            *mistake = Some(m);
            return;
        }
    };
    let baseline_digests = spec_digests(program, sources);
    if baseline_digests.is_empty() {
        *mistake = Some("the program has no promises or examples for --strength to measure");
        return;
    }
    let Some(selection) = select_checks(program, config, diags) else {
        return;
    };
    if config.json {
        let ver_bytes = render_version_json("test", mo_version);
        let ok = output.write_all(&ver_bytes).is_ok() && output.flush().is_ok();
        *version_written = ok;
        if !ok {
            return;
        }
    }
    let before = diags.len();
    run_tests(config, program, sources, &selection, output, diags, totals);
    let original =
        totals.failed == 0 && totals.stopped == 0 && diags.len() == before && diags.dropped() == 0;
    if original {
        let counted_file = set.counted;
        let mut serial_cfg = config.clone();
        serial_cfg.jobs = 1;
        let workers = config.jobs.max(1).min(set.versions.len().max(1));
        if workers <= 1 || set.versions.len() <= 1 {
            for v in &mut set.versions {
                let counted = !counted_file || v.listed;
                measure_one(
                    &serial_cfg,
                    strength_dir,
                    library,
                    program,
                    sources,
                    &baseline_digests,
                    v,
                    counted,
                );
            }
        } else {
            let ctx = StrengthContext {
                config: &serial_cfg,
                strength_dir,
                library,
                program,
                sources,
                baseline_digests: &baseline_digests,
                counted_file,
            };
            let len = set.versions.len();
            let next_idx = AtomicUsize::new(0);
            let slots: Mutex<Vec<Option<Version>>> = Mutex::new((0..len).map(|_| None).collect());
            let base_versions = &set.versions;
            let work_loop = || {
                loop {
                    let idx = next_idx.fetch_add(1, Ordering::Relaxed);
                    if idx >= len {
                        break;
                    }
                    let measured = measure_version(&ctx, &base_versions[idx]);
                    if let Ok(mut guard) = slots.lock() {
                        guard[idx] = Some(measured);
                    }
                }
            };
            std::thread::scope(|s| {
                for _ in 1..workers {
                    let _ = std::thread::Builder::new()
                        .name("mo-strength-worker".to_string())
                        .stack_size(MO_SOURCE_STACK_SIZE as usize)
                        .spawn_scoped(s, work_loop);
                }
                work_loop();
            });
            let collected = match slots.into_inner() {
                Ok(v) => v,
                Err(poisoned) => poisoned.into_inner(),
            };
            for (dst, slot) in set.versions.iter_mut().zip(collected) {
                match slot {
                    Some(m) => *dst = m,
                    None => {
                        test_report_error(
                            diags,
                            "a wrong version could not be measured",
                            Span::NONE,
                            None,
                        );
                        dst.outcome = Outcome::Unfinished;
                    }
                }
            }
        }
    }
    let mut written = render_strength(strength_dir, config.json, &set, original)
        .is_some_and(|buf| output.write_all(&buf).is_ok() && output.flush().is_ok());
    if written && config.json {
        written = write_strength_events(output, strength_dir, &set, original);
    }
    if !written {
        test_report_error(
            diags,
            "the strength record could not be written",
            Span::NONE,
            None,
        );
    }
    let open = if original {
        set.versions
            .iter()
            .filter(|v| v.outcome == Outcome::Unfinished)
            .count()
    } else {
        0
    };
    if !original && diags.len() != before {
        emit_unfinished(diags, "the measured program's own checks did not finish");
    } else if open != 0 {
        emit_unfinished(
            diags,
            "a wrong version's checks did not finish, so the rate is partial",
        );
    }
}
