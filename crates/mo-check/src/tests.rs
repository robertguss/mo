use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::limits::{
    MO_LIMIT_FILE_SIZE, MO_LIMIT_FILES, MO_LIMIT_MODULE_IMPORTS, MO_LIMIT_PATH_SIZE,
    MO_LIMIT_PROGRAM_SIZE,
};
use mo_def::{ErrorCode, HOSTOPS, PRIMS};
use mo_syntax::{DURATION_UNITS, SyntaxKind, parse_module};

use crate::abilities::{
    AbilityContext, AbilityImplementationKind, SourceAbility, check_signature_abilities,
    snake_case_name,
};
use crate::embedded::{DEFAULT_EMBEDDED_LIBRARY, default_embedded_library, default_library_root};
use crate::ids::{ModuleId, SymRef};
use crate::load::{
    LibraryMode, LoadOptions, LoadedModule, ModuleSet, load_program, load_program_sources,
};
use crate::modules::resolve_modules;
use crate::signatures::{
    check_data_recursion, json_type, prepare_available_signatures, prepare_signatures,
    resolve_expression_type, resolve_service_type,
};
use crate::types::{CAPABILITY_NAMES, SourceTypeKind};
use crate::{CheckStage, run_stage};

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(prefix: &str) -> Self {
        let id = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("mo-check-{prefix}-{pid}-{id}"));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        Self { path }
    }

    fn join(&self, rel: &str) -> PathBuf {
        self.path.join(rel)
    }

    fn write(&self, rel: &str, bytes: &[u8]) -> PathBuf {
        let p = self.path.join(rel);
        if let Some(parent) = p.parent() {
            fs::create_dir_all(parent).unwrap();
        }
        fs::write(&p, bytes).unwrap();
        p
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn repo_str(rel: &str) -> String {
    repo_root().join(rel).to_string_lossy().into_owned()
}

fn no_lib_options() -> LoadOptions {
    LoadOptions {
        library: LibraryMode::None,
        ..LoadOptions::default()
    }
}

fn dir_lib_options(dir: PathBuf) -> LoadOptions {
    LoadOptions {
        library: LibraryMode::Directory(dir),
        ..LoadOptions::default()
    }
}

fn snapshot_diag(diag: &Diagnostic, strip_prefix: Option<&str>) -> String {
    let mut out = String::new();
    let section = diag.section.unwrap_or("-");
    out.push_str(&format!("E{:04} {section}\n", diag.code.number()));
    let push_span = |s: &mut String, span: Span| {
        s.push_str(&format!("{}:{}..{}", span.file, span.start, span.end));
    };
    let clean_text = |text: Option<&mo_base::DiagText>| -> String {
        match text {
            None => "-".to_owned(),
            Some(t) => {
                let raw = t.as_str_lossy();
                if let Some(prefix) = strip_prefix {
                    raw.strip_prefix(prefix).unwrap_or(&raw).to_owned()
                } else {
                    raw.into_owned()
                }
            }
        }
    };
    push_span(&mut out, diag.primary.span);
    out.push('\n');
    out.push_str(&diag.message.as_str_lossy());
    out.push('\n');
    out.push_str(&clean_text(diag.reason.as_ref()));
    out.push('\n');
    out.push_str(&clean_text(diag.primary.text.as_ref()));
    out.push('\n');
    for sec in &diag.secondary {
        out.push_str("secondary ");
        push_span(&mut out, sec.span);
        out.push(' ');
        out.push_str(&clean_text(sec.text.as_ref()));
        out.push('\n');
    }
    out.push_str(&format!("fixes {}\n", diag.fixes.len()));
    for fix in &diag.fixes {
        out.push_str(&clean_text(fix.title.as_ref()));
        out.push('\n');
        for edit in &fix.edits {
            push_span(&mut out, edit.span);
            out.push('\n');
            out.push('"');
            out.push_str(&edit.replacement.as_str_lossy());
            out.push_str("\"\n");
        }
    }
    out
}

fn parse_into_module(
    sources: &mut SourceMap,
    path: &str,
    bytes: &[u8],
    id: u32,
    uses: Vec<ModuleId>,
    library: bool,
) -> LoadedModule {
    let file = sources.add_bytes(path, bytes).unwrap();
    let mut diags = DiagBag::new();
    let syntax = parse_module(sources, file, &mut diags).unwrap();
    assert!(!diags.has_any(), "unexpected syntax errors in {path}");
    let header = syntax.header.as_ref().unwrap();
    let path_node = syntax.tree.node(header.path);
    let mut name = String::new();
    for (i, ch) in path_node.children.iter().enumerate() {
        if i != 0 {
            name.push('.');
        }
        let part = syntax.tree.node_text(ch.unwrap()).unwrap();
        name.push_str(std::str::from_utf8(part).unwrap());
    }
    LoadedModule {
        id: ModuleId(id),
        name,
        file,
        library,
        dependency: false,
        uses,
        syntax,
    }
}

#[test]
fn embedded_library_matches_disk_lib() {
    let embedded = default_embedded_library();
    assert_eq!(embedded.files.len(), DEFAULT_EMBEDDED_LIBRARY.files.len());
    assert!(default_library_root().is_some());

    let lib_root = repo_root().join("lib");
    let mut disk_files: Vec<(String, Vec<u8>)> = Vec::new();
    fn walk(root: &Path, dir: &Path, out: &mut Vec<(String, Vec<u8>)>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                walk(root, &path, out);
            } else if path.extension().and_then(|e| e.to_str()) == Some("mo") {
                let rel = path
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                out.push((rel, fs::read(&path).unwrap()));
            }
        }
    }
    walk(&lib_root, &lib_root, &mut disk_files);
    disk_files.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));

    assert_eq!(
        embedded.files.len(),
        disk_files.len(),
        "embedded lib file count must equal disk lib/**/*.mo"
    );
    assert!(embedded.files.len() >= 30);
    for (emb, (rel, bytes)) in embedded.files.iter().zip(disk_files.iter()) {
        assert_eq!(emb.path, rel);
        assert_eq!(emb.bytes, bytes.as_slice());
        assert!(embedded.find(rel).is_some());
    }
    assert!(embedded.find("does-not-exist.mo").is_none());
}

#[test]
fn load_graph_matches_c() {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let entry = repo_str("tests/check/load/main.mo");
    let set = load_program(&mut sources, &entry, &no_lib_options(), &mut diags).unwrap();
    assert!(!diags.has_any());
    assert!(set.root.ends_with("tests/check/load/"));
    assert_eq!(set.len(), 3);
    assert_eq!(sources.len(), 3);
    assert!(
        sources
            .get(1)
            .unwrap()
            .path_lossy()
            .ends_with("tests/check/load/shop/order-book.mo")
    );
    assert!(
        sources
            .get(2)
            .unwrap()
            .path_lossy()
            .ends_with("tests/check/load/shared.mo")
    );
    assert_eq!(set.order, vec![ModuleId(2), ModuleId(1), ModuleId(0)]);
    let tree = &set.modules[1].syntax.tree;
    let items = tree.node(set.modules[1].syntax.root).children[1].unwrap();
    let first_decl = tree.node(items).children[0].unwrap();
    assert_eq!(tree.node(first_decl).kind, SyntaxKind::Struct);
}

#[test]
fn load_memory_matches_c() {
    let programs: [([Option<&str>; 3], Option<ErrorCode>); 5] = [
        (
            [
                Some("module Main\nuse Json\nfn main() : Int\n Json.answer()\nend\n"),
                Some(
                    "module Json\nexpose answer\nuse Support\nfn answer() : Int\n Support.answer()\nend\n",
                ),
                Some("module Support\nexpose answer\nfn answer() : Int\n 37\nend\n"),
            ],
            None,
        ),
        (
            [
                Some("module Main\nuse Shared\n"),
                Some("module Support\n"),
                None,
            ],
            Some(ErrorCode::ModuleMissing),
        ),
        (
            [
                Some("module Main\nuse Json\n"),
                Some("module Json\nuse Main\n"),
                None,
            ],
            Some(ErrorCode::ModuleMissing),
        ),
        (
            [
                Some("module Main\nuse Support\n"),
                Some("module Support\nuse Main\n"),
                None,
            ],
            Some(ErrorCode::ModuleCycle),
        ),
        (
            [Some("module Main\n"), Some("module Main\n"), None],
            Some(ErrorCode::ModuleReserved),
        ),
    ];
    let paths = [
        "tests/check/load/main.mo",
        "memory/json.mo",
        "memory/support.mo",
    ];

    for (i, (files, expected_err)) in programs.iter().enumerate() {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        for (j, text) in files.iter().enumerate() {
            if let Some(src) = text {
                sources.add_bytes(paths[j], src.as_bytes()).unwrap();
            }
        }
        let lib_flags = [false, i < 3, true];
        let set = load_program_sources(&mut sources, Some(&lib_flags), &mut diags);
        if let Some(code) = expected_err {
            assert!(set.is_none(), "case {i} should fail");
            assert_eq!(diags.len(), 1);
            assert_eq!(diags.items()[0].code, *code);
        } else {
            let set = set.expect("case 0 should load");
            assert!(!diags.has_any());
            assert_eq!(set.len(), 3);
            assert_eq!(set.modules[0].uses, vec![ModuleId(1)]);
            assert_eq!(set.modules[1].uses, vec![ModuleId(2)]);
            assert_eq!(set.order, vec![ModuleId(2), ModuleId(1), ModuleId(0)]);
            assert!(!set.modules[0].library && set.modules[1].library && set.modules[2].library);
            let interfaces = resolve_modules(&set, &mut diags).unwrap();
            let mut sigs = prepare_signatures(&set, &interfaces, &mut diags).unwrap();
            assert!(check_signature_abilities(
                &set,
                &interfaces,
                &mut sigs,
                &mut diags
            ));
            assert!(!diags.has_any());
        }
    }
}

#[test]
fn interfaces_matches_c() {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let entry = repo_str("tests/check/interfaces/main.mo");
    let lib = repo_root().join("tests/check/library");
    let set = load_program(&mut sources, &entry, &dir_lib_options(lib), &mut diags).unwrap();
    assert!(!diags.has_any());
    assert_eq!(set.len(), 3);
    assert_eq!(set.modules[1].name, "LibSupport.Extra");
    assert!(set.modules[1].library && !set.modules[0].library && !set.modules[2].library);
    assert_eq!(set.modules[0].uses, vec![ModuleId(2)]);

    let names = resolve_modules(&set, &mut diags).unwrap();
    assert!(!diags.has_any());
    let scope = names.scope(ModuleId(0));
    assert_eq!(scope.aliases.len(), 1);
    assert_eq!(scope.aliases[0].name, "Money");
    assert_eq!(scope.imported.len(), 5);
    let expected = ["Choice", "Done", "Money", "Ready", "total"];
    for (imp, want) in scope.imported.iter().zip(expected.iter()) {
        assert_eq!(imp.name, *want);
    }
    let money_sym = names.symbol(scope.imported[2].symbol);
    assert!(money_sym.exported && !money_sym.insides);
    let choice_sym = names.symbol(scope.imported[0].symbol);
    assert!(choice_sym.insides);
    let done_sym = names.symbol(scope.imported[1].symbol);
    let owner_scope = names.scope(done_sym.module);
    assert_eq!(
        owner_scope.symbol(done_sym.owner.unwrap()).decl_kind,
        SyntaxKind::Enum
    );
    assert_eq!(scope.imported[4].count, 2);
    let total_mod = scope.imported[4].symbol.module;
    let total_idx = scope.imported[4].symbol.symbol.index();
    let total0 = &names.scope(total_mod).symbols[total_idx];
    let total1 = &names.scope(total_mod).symbols[total_idx + 1];
    assert_eq!(total0.decl_kind, SyntaxKind::Fn);
    assert_eq!(total1.decl_kind, SyntaxKind::Fn);
    assert_eq!(
        names.scope(total_mod).symbol(total0.receiver.unwrap()).name,
        "Money"
    );
    assert_eq!(
        names.scope(total_mod).symbol(total1.receiver.unwrap()).name,
        "Line"
    );
    assert_eq!(choice_sym.cases.len(), 2);
    assert_eq!(owner_scope.symbol(choice_sym.cases[0]).name, "Ready");
    assert_eq!(owner_scope.symbol(choice_sym.cases[1]).name, "Done");
    assert_eq!(
        owner_scope.find_symbol_index(b"Money"),
        Some(scope.imported[2].symbol.symbol)
    );
    assert_eq!(owner_scope.find_symbol_index(b"MoneyX"), None);
    assert_eq!(owner_scope.find_symbol_index(b"Mon"), None);
}

#[test]
fn corpus_diagnostics_byte_for_byte() {
    #[derive(Clone, Copy)]
    enum Phase {
        Loading,
        Interfaces,
        Signatures,
        SignatureAbilities,
    }
    let cases: &[(&str, &str, Phase)] = &[
        ("e0025-and/main.mo", "e0025-and.diag", Phase::Loading),
        ("e0025-not/main.mo", "e0025-not.diag", Phase::Loading),
        (
            "e0025-or-lambda/main.mo",
            "e0025-or-lambda.diag",
            Phase::Loading,
        ),
        (
            "e0025-or-assign/main.mo",
            "e0025-or-assign.diag",
            Phase::Loading,
        ),
        ("e0034/main.mo", "e0034.diag", Phase::Loading),
        ("e0035/main.mo", "e0035.diag", Phase::Loading),
        ("e0036/main.mo", "e0036.diag", Phase::Loading),
        ("e0037/json.mo", "e0037.diag", Phase::Loading),
        ("e0096/main.mo", "e0096.diag", Phase::Loading),
        ("e0097/main.mo", "e0097.diag", Phase::Loading),
        ("e0038.mo", "e0038.diag", Phase::Interfaces),
        (
            "e0038-import-repeat.mo",
            "e0038-import-repeat.diag",
            Phase::Interfaces,
        ),
        ("e0039.mo", "e0039.diag", Phase::Interfaces),
        ("e0040.mo", "e0040.diag", Phase::Interfaces),
        ("e0041.mo", "e0041.diag", Phase::Interfaces),
        ("e0042.mo", "e0042.diag", Phase::Interfaces),
        ("e0043.mo", "e0043.diag", Phase::Interfaces),
        ("e0044.mo", "e0044.diag", Phase::Interfaces),
        ("e0045.mo", "e0045.diag", Phase::Interfaces),
        ("e0045-network.mo", "e0045-network.diag", Phase::Interfaces),
        ("e0046.mo", "e0046.diag", Phase::Interfaces),
        ("e0047.mo", "e0047.diag", Phase::Interfaces),
        ("e0048.mo", "e0048.diag", Phase::Signatures),
        ("e0049.mo", "e0049.diag", Phase::Signatures),
        ("e0062.mo", "e0062.diag", Phase::Signatures),
        ("e0066.mo", "e0066.diag", Phase::Signatures),
        ("e0081.mo", "e0081.diag", Phase::Signatures),
        ("e0082.mo", "e0082.diag", Phase::Signatures),
        ("e0084.mo", "e0084.diag", Phase::Signatures),
        ("e0080.mo", "e0080.diag", Phase::SignatureAbilities),
    ];

    let root = repo_root();
    let prefix = format!("{}/", root.to_string_lossy());

    for &(input, expected_file, phase) in cases {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        let input_path = root.join("tests/check/diagnostics").join(input);
        let input_str = input_path.to_string_lossy().into_owned();

        match phase {
            Phase::Loading => {
                assert!(
                    load_program(&mut sources, &input_str, &no_lib_options(), &mut diags).is_none(),
                    "{input}: expected loading failure"
                );
            }
            Phase::Interfaces | Phase::Signatures | Phase::SignatureAbilities => {
                let bytes0 = fs::read(&input_path).unwrap();
                let second_rel = if input == "e0047.mo" {
                    "tests/check/interfaces/hidden.mo"
                } else {
                    "tests/check/interfaces/money.mo"
                };
                let bytes1 = fs::read(root.join(second_rel)).unwrap();
                let mut m0 =
                    parse_into_module(&mut sources, &input_str, &bytes0, 0, Vec::new(), false);
                let use_count = m0
                    .syntax
                    .header
                    .as_ref()
                    .map_or(0, |h| m0.syntax.tree.node(h.uses).children.len());
                m0.uses = vec![ModuleId(1); use_count];
                let include_second =
                    !(matches!(phase, Phase::SignatureAbilities) && use_count == 0);
                let mut modules = vec![m0];
                if include_second {
                    let m1 =
                        parse_into_module(&mut sources, second_rel, &bytes1, 1, Vec::new(), false);
                    modules.push(m1);
                }
                let order = if modules.len() == 2 {
                    vec![ModuleId(1), ModuleId(0)]
                } else {
                    vec![ModuleId(0)]
                };
                let set = ModuleSet {
                    root: String::new(),
                    modules,
                    order,
                    manifests: Vec::new(),
                };
                let names = resolve_modules(&set, &mut diags);
                match phase {
                    Phase::Interfaces => {
                        assert!(names.is_none(), "{input}: expected interface failure");
                    }
                    Phase::Signatures => {
                        let names = names.expect("interfaces should pass");
                        assert!(
                            prepare_signatures(&set, &names, &mut diags).is_none(),
                            "{input}: expected signature failure"
                        );
                    }
                    Phase::SignatureAbilities => {
                        let names = names.expect("interfaces should pass");
                        let mut sigs = prepare_available_signatures(&set, &names, &mut diags)
                            .expect("signatures should prepare");
                        assert!(check_signature_abilities(
                            &set, &names, &mut sigs, &mut diags
                        ));
                    }
                    Phase::Loading => {}
                }
            }
        }

        assert_eq!(
            diags.len(),
            1,
            "{input}: expected 1 diagnostic, got {}",
            diags.len()
        );
        assert_eq!(diags.dropped(), 0);
        let actual = snapshot_diag(&diags.items()[0], Some(&prefix));
        let expected =
            fs::read_to_string(root.join("tests/check/diagnostics").join(expected_file)).unwrap();
        assert_eq!(actual, expected, "diagnostic snapshot mismatch on {input}");
    }
}

#[test]
fn samples_pass_embedded_and_disk_library() {
    let files = [
        "account",
        "expenses",
        "find",
        "greet",
        "inventory",
        "job-queue",
        "json-query",
        "kv",
        "notes-tool",
        "rate-limit",
        "todo",
        "wc",
    ];
    let lib_dir = repo_root().join("lib");
    for name in files {
        let entry = repo_str(&format!("examples/{name}.mo"));
        for options in [LoadOptions::default(), dir_lib_options(lib_dir.clone())] {
            let out = run_stage(&entry, &options, CheckStage::Signatures);
            assert!(
                out.ok(),
                "sample {name} failed: {:?}",
                out.diags.items().first().map(|d| (&d.code, &d.message))
            );
            assert!(out.modules.is_some());
            assert!(out.interfaces.is_some());
            assert!(out.signatures.is_some());
        }
    }
}

#[test]
fn confined_files_and_nonblocking_fifo() {
    let ws = TempDir::new("confined");
    let outside = ws.write("outside/other.mo", b"module Other\n");
    let entry = ws.write("root/main.mo", b"module Main\nuse Other\n");
    let linked = ws.join("root/other.mo");

    #[cfg(unix)]
    {
        std::os::unix::fs::symlink(&outside, &linked).unwrap();
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        assert!(
            load_program(
                &mut sources,
                entry.to_str().unwrap(),
                &no_lib_options(),
                &mut diags
            )
            .is_none()
        );
        assert_eq!(diags.len(), 1);
        assert_eq!(diags.items()[0].code, ErrorCode::ModuleOutsideRoot);
        assert_eq!(sources.len(), 1);

        // Symlink to /dev/null is outside root as well.
        fs::remove_file(&linked).unwrap();
        std::os::unix::fs::symlink("/dev/null", &linked).unwrap();
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        assert!(
            load_program(
                &mut sources,
                entry.to_str().unwrap(),
                &no_lib_options(),
                &mut diags
            )
            .is_none()
        );
        assert_eq!(diags.len(), 1);
        assert_eq!(diags.items()[0].code, ErrorCode::ModuleOutsideRoot);

        // Named pipe (FIFO) under root must fail nonblockingly as unreadable (E0034).
        fs::remove_file(&linked).unwrap();
        let status = std::process::Command::new("mkfifo")
            .arg(&linked)
            .status()
            .unwrap();
        assert!(status.success());
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        assert!(
            load_program(
                &mut sources,
                entry.to_str().unwrap(),
                &no_lib_options(),
                &mut diags
            )
            .is_none()
        );
        assert_eq!(diags.len(), 1);
        assert_eq!(diags.items()[0].code, ErrorCode::ModuleMissing);

        // Regular file with `//.//` normalized path succeeds.
        fs::remove_file(&linked).unwrap();
        fs::write(&linked, b"module Other\n").unwrap();
        let normalized = format!("{}//.//main.mo", ws.join("root").to_string_lossy());
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        let set = load_program(&mut sources, &normalized, &no_lib_options(), &mut diags).unwrap();
        assert_eq!(set.len(), 2);
        assert_eq!(
            sources.get(0).unwrap().path_lossy(),
            entry.to_string_lossy()
        );

        // Entry itself being a symlink outside root is allowed.
        fs::remove_file(&entry).unwrap();
        let target = ws.write("outside/main.mo", b"module Main\nuse Other\n");
        std::os::unix::fs::symlink(&target, &entry).unwrap();
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();
        let set = load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &no_lib_options(),
            &mut diags,
        );
        assert!(set.is_some() && !diags.has_any());
    }
}

#[test]
fn library_roots_and_reserved_collisions() {
    let ws = TempDir::new("roots");
    let entry = ws.write("main.mo", b"module Main\nuse Helper\n");
    let helper = ws.write("helper.mo", b"module Helper\nanswer = 1\n");
    let library = ws.join("lib");

    // Nonexistent library directory -> E1102.
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &dir_lib_options(library.clone()),
            &mut diags,
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::LibraryUnreadable);

    // Library module referencing missing library helper -> E0034 under canonical library root.
    let support = ws.write("lib/support.mo", b"module Support\nuse Helper\n");
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &dir_lib_options(library.clone()),
            &mut diags,
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::ModuleMissing);
    assert_eq!(diags.items()[0].primary.span.file, 1);

    // Adding lib/helper.mo makes both Main and Support resolve to library Helper.
    let lib_helper = ws.write("lib/helper.mo", b"module Helper\nanswer = 2\n");
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let set = load_program(
        &mut sources,
        entry.to_str().unwrap(),
        &dir_lib_options(library.clone()),
        &mut diags,
    )
    .unwrap();
    assert!(!diags.has_any());
    let main_target = set.modules[0].uses[0];
    assert!(set.module(main_target).library);
    assert_eq!(set.modules[2].uses[0], main_target);

    // Library entry file is marked as library module 0 and not loaded twice.
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let set = load_program(
        &mut sources,
        support.to_str().unwrap(),
        &dir_lib_options(library.clone()),
        &mut diags,
    )
    .unwrap();
    assert_eq!(set.len(), 2);
    assert!(set.modules[0].library && set.modules[1].library);
    assert_eq!(set.modules[0].name, "Support");
    assert_eq!(set.modules[1].name, "Helper");

    // Program file with same module name as library file -> E0037.
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            helper.to_str().unwrap(),
            &dir_lib_options(library.clone()),
            &mut diags,
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::ModuleReserved);

    // Wrong module name inside library -> E0035.
    fs::write(&lib_helper, b"module Wrong\n").unwrap();
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &dir_lib_options(library),
            &mut diags,
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::ModuleName);
}

#[test]
fn loading_labels_cycles_mismatch_and_long_path() {
    let ws = TempDir::new("labels");
    let entry = ws.write("main.mo", b"module Main\nuse Second\n");
    ws.write("second.mo", b"module Second\nuse Third\n");
    ws.write("third.mo", b"module Third\nuse Main\n");

    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &no_lib_options(),
            &mut diags
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    let diag = &diags.items()[0];
    assert_eq!(diag.code, ErrorCode::ModuleCycle);
    assert_eq!(diag.primary.span.file, 2);
    assert_eq!(diag.primary.span.start, 17);
    assert_eq!(diag.primary.text.as_ref().unwrap().as_str_lossy(), "Main");
    assert_eq!(diag.secondary.len(), 2);
    assert_eq!(diag.secondary[0].span.file, 0);
    assert_eq!(
        diag.secondary[0].text.as_ref().unwrap().as_str_lossy(),
        "Second"
    );
    assert_eq!(diag.secondary[1].span.file, 1);
    assert_eq!(
        diag.secondary[1].text.as_ref().unwrap().as_str_lossy(),
        "Third"
    );

    // Module name mismatch with secondary label.
    fs::write(&entry, b"module Main\nuse XmlReader\n").unwrap();
    let xml = ws.write("xml-reader.mo", b"module XMLReader\n");
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &no_lib_options(),
            &mut diags
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::ModuleName);
    assert_eq!(diags.items()[0].secondary.len(), 1);
    assert_eq!(diags.items()[0].secondary[0].span.file, 1);
    assert_eq!(diags.items()[0].secondary[0].span.start, 7);

    // Path conversion overflow -> E1004.
    fs::remove_file(&xml).unwrap();
    let mut long_use = String::from("module Main\nuse ");
    for _ in 0..(MO_LIMIT_PATH_SIZE - 2) {
        long_use.push('A');
    }
    long_use.push('\n');
    fs::write(&entry, long_use.as_bytes()).unwrap();
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    assert!(
        load_program(
            &mut sources,
            entry.to_str().unwrap(),
            &no_lib_options(),
            &mut diags
        )
        .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::PathTooLong);
    assert_eq!(diags.items()[0].primary.span.start, 16);
}

#[test]
fn independent_errors_and_name_rules_with_fixes() {
    let mut sources = SourceMap::new();
    let first = "module Main\nzeta = 1\nzeta = 2\nalpha = 1\nalpha = 2\n";
    let second = "module Elsewhere\nexpose missing\n";
    let m0 = parse_into_module(
        &mut sources,
        "main.mo",
        first.as_bytes(),
        0,
        Vec::new(),
        false,
    );
    let m1 = parse_into_module(
        &mut sources,
        "elsewhere.mo",
        second.as_bytes(),
        1,
        Vec::new(),
        false,
    );
    let set = ModuleSet {
        root: String::new(),
        modules: vec![m0, m1],
        order: vec![ModuleId(0), ModuleId(1)],
        manifests: Vec::new(),
    };
    let mut diags = DiagBag::new();
    assert!(resolve_modules(&set, &mut diags).is_none());
    assert_eq!(diags.len(), 3);
    assert_eq!(diags.items()[0].code, ErrorCode::NameClash);
    assert_eq!(
        diags.items()[0].primary.span.start as usize,
        first.find("zeta = 2").unwrap()
    );
    assert_eq!(
        diags.items()[1].primary.span.start as usize,
        first.find("alpha = 2").unwrap()
    );
    assert_eq!(diags.items()[2].code, ErrorCode::ExposeName);
    assert_eq!(diags.items()[2].primary.span.file, 1);

    // Direct case import fix (E0042) produces valid source across all three list positions.
    let money = "module Money\nexpose Choice(..), Money\nenum Choice\n Ready\n Done\nend\nstruct Money\n value: Int\nend\n";
    for imports in ["Ready", "Ready, Choice", "Choice, Ready"] {
        let input = format!("module Main\nuse Money{{{imports}}}\n");
        let mut s = SourceMap::new();
        let m0 = parse_into_module(
            &mut s,
            "main.mo",
            input.as_bytes(),
            0,
            vec![ModuleId(1)],
            false,
        );
        let m1 = parse_into_module(&mut s, "money.mo", money.as_bytes(), 1, Vec::new(), false);
        let set = ModuleSet {
            root: String::new(),
            modules: vec![m0, m1],
            order: vec![ModuleId(1), ModuleId(0)],
            manifests: Vec::new(),
        };
        let mut d = DiagBag::new();
        assert!(resolve_modules(&set, &mut d).is_none());
        assert_eq!(d.len(), 1);
        assert_eq!(d.items()[0].code, ErrorCode::ImportCase);
        assert_eq!(d.items()[0].fixes.len(), 1);
        let edit = &d.items()[0].fixes[0].edits[0];
        let mut fixed = String::new();
        fixed.push_str(&input[..edit.span.start as usize]);
        fixed.push_str(&edit.replacement.as_str_lossy());
        fixed.push_str(&input[edit.span.end as usize..]);

        let mut s2 = SourceMap::new();
        let fm0 = parse_into_module(
            &mut s2,
            "fixed.mo",
            fixed.as_bytes(),
            0,
            vec![ModuleId(1)],
            false,
        );
        let fm1 = parse_into_module(&mut s2, "money.mo", money.as_bytes(), 1, Vec::new(), false);
        let fset = ModuleSet {
            root: String::new(),
            modules: vec![fm0, fm1],
            order: vec![ModuleId(1), ModuleId(0)],
            manifests: Vec::new(),
        };
        let mut d2 = DiagBag::new();
        assert!(resolve_modules(&fset, &mut d2).is_some() && !d2.has_any());
    }
}

#[test]
fn function_signatures_and_native_table_parity() {
    let text = "module Main\n\
                fn identity(value: T) : T\n  value\nend\n\
                fn other(value: T) : T\n  value\nend\n\
                fn visit(value: fn(Int))\n  value(1)\nend\n";
    let mut sources = SourceMap::new();
    let m0 = parse_into_module(
        &mut sources,
        "main.mo",
        text.as_bytes(),
        0,
        Vec::new(),
        false,
    );
    let set = ModuleSet {
        root: String::new(),
        modules: vec![m0],
        order: vec![ModuleId(0)],
        manifests: Vec::new(),
    };
    let mut diags = DiagBag::new();
    let interfaces = resolve_modules(&set, &mut diags).unwrap();
    let sigs = prepare_signatures(&set, &interfaces, &mut diags).unwrap();
    assert!(!diags.has_any());
    assert_eq!(sigs.functions.len(), 3);

    let id_ty = sigs.types.get(sigs.functions[0].ty.unwrap());
    assert_eq!(
        interfaces.symbol(sigs.functions[0].function).name,
        "identity"
    );
    assert_eq!(id_ty.kind, SourceTypeKind::Function);
    assert_eq!(id_ty.arguments.len(), 2);
    assert_eq!(
        sigs.types.get(id_ty.arguments[0]).kind,
        SourceTypeKind::Parameter
    );
    assert_eq!(sigs.types.get(id_ty.arguments[0]).name, "T");
    assert_eq!(id_ty.arguments[0], id_ty.arguments[1]);

    let other_ty = sigs.types.get(sigs.functions[1].ty.unwrap());
    assert_ne!(other_ty.arguments[0], id_ty.arguments[0]);
    assert_eq!(other_ty.arguments[0], other_ty.arguments[1]);

    let visit_ty = sigs.types.get(sigs.functions[2].ty.unwrap());
    let cb_ty = sigs.types.get(visit_ty.arguments[0]);
    assert_eq!(cb_ty.kind, SourceTypeKind::Function);
    assert_eq!(sigs.types.get(cb_ty.arguments[1]).name, "Unit");
    assert_eq!(cb_ty.arguments[1], visit_ty.arguments[1]);

    // Verify full native table loaded cleanly from mo-def PRIMS + HOSTOPS + Sqlite.in_memory + 5 duration units.
    assert_eq!(
        sigs.native.len(),
        PRIMS.len() + HOSTOPS.len() + 1 + DURATION_UNITS.len()
    );
    let (_, in_mem) = sigs.find_native("Sqlite", "in_memory").unwrap();
    assert_eq!(in_mem.qualified, "Sqlite.fake");
    assert!(in_mem.spec_only);
    for unit in DURATION_UNITS {
        let singular = &unit[..unit.len() - 1];
        let (_, dur) = sigs.find_native("Int", singular).unwrap();
        assert_eq!(dur.qualified, format!("Int.{unit}"));
        assert!(!dur.spec_only);
    }
    // Verify non-fn native constants (e.g. Set.empty, Bytes.empty) are not wrapped as Function types.
    let (_, set_empty) = sigs.find_native("Set", "empty").unwrap();
    assert_eq!(sigs.types.get(set_empty.ty).kind, SourceTypeKind::Named);
    assert_eq!(sigs.types.get(set_empty.ty).name, "Set");
    let (_, bytes_empty) = sigs.find_native("Bytes", "empty").unwrap();
    assert_eq!(sigs.types.get(bytes_empty.ty).kind, SourceTypeKind::Named);
    assert_eq!(sigs.types.get(bytes_empty.ty).name, "Bytes");
}

#[test]
fn package_manifests_full_parity() {
    let ws = TempDir::new("manifest");
    ws.write(
        "dep_leaf/leaf-mod.mo",
        b"module LeafMod\nexpose leaf_val\nfn leaf_val() : Int\n  10\nend\n",
    );
    ws.write(
        "dep_lib/lib/lib-pkg.mo",
        b"module LibPkg\nexpose lib_val\nfn lib_val() : Int\n  20\nend\n",
    );
    ws.write(
        "dep_src/mo.toml",
        b"# transitive\n[package]\nname = \"dep-src\"\nversion = \"0.1.0\"\n[dependencies]\nleaf = { path = \"../dep_leaf\" }\n",
    );
    ws.write(
        "dep_src/src/alpha.mo",
        b"module Alpha\nexpose alpha_val\nuse Alpha.Sub\nuse LeafMod\nfn alpha_val() : Int\n  Sub.sub_val() + LeafMod.leaf_val()\nend\n",
    );
    ws.write(
        "dep_src/src/alpha/sub.mo",
        b"module Alpha.Sub\nexpose sub_val\nfn sub_val() : Int\n  5\nend\n",
    );
    ws.write(
        "dep_custom/custom/custom-mod.mo",
        b"module CustomMod\nexpose custom_val\nfn custom_val() : Int\n  7\nend\n",
    );

    let manifest = ws.write(
        "app/mo.toml",
        b"# app\n[package]\nname = \"my-app\"\n\n[dependencies]\n\
          alpha = { path = \"../dep_src\" } # inline\n\
          lib_pkg = { path = \"../dep_lib\" }\n\
          custom = { path = \"../dep_custom\", src = \"custom\" }\n",
    );
    ws.write(
        "app/cmd/local-helper.mo",
        b"module LocalHelper\nexpose local_val\nfn local_val() : Int\n  1\nend\n",
    );
    let entry = ws.write(
        "app/cmd/main.mo",
        b"module Main\nuse Alpha\nuse LibPkg\nuse CustomMod\nuse LocalHelper\n\
          fn main() : Int\n  Alpha.alpha_val() + LibPkg.lib_val() + CustomMod.custom_val() + LocalHelper.local_val()\nend\n",
    );

    let out = run_stage(
        entry.to_str().unwrap(),
        &LoadOptions::default(),
        CheckStage::Signatures,
    );
    assert!(out.ok(), "manifest app failed: {:?}", out.diags.items());
    let set = out.modules.unwrap();
    assert_eq!(set.manifests.len(), 2);
    assert!(
        set.modules
            .iter()
            .any(|m| m.name == "Alpha" && m.dependency)
    );
    assert!(
        set.modules
            .iter()
            .any(|m| m.name == "LeafMod" && m.dependency)
    );
    assert!(
        set.modules
            .iter()
            .all(|m| !(m.name == "Main" && m.dependency))
    );

    // Local shadow & Arvo.Cookie override via mo.toml.
    ws.write(
        "dep_arvo/src/arvo/cookie.mo",
        b"module Arvo.Cookie\nexpose external_arvo\nfn external_arvo() : Int\n  99\nend\n",
    );
    fs::write(
        &manifest,
        b"[package]\nname = \"override-app\"\n[dependencies]\narvo = { path = \"../dep_arvo\" }\nalpha = { path = \"../dep_src\" }\n",
    )
    .unwrap();
    let local_alpha = ws.write(
        "app/cmd/alpha.mo",
        b"module Alpha\nexpose local_alpha\nfn local_alpha() : Int\n  42\nend\n",
    );
    fs::write(
        &entry,
        b"module Main\nuse Alpha\nuse Arvo.Cookie\nfn main() : Int\n  Alpha.local_alpha() + Cookie.external_arvo()\nend\n",
    )
    .unwrap();
    let out = run_stage(
        entry.to_str().unwrap(),
        &LoadOptions::default(),
        CheckStage::Signatures,
    );
    assert!(out.ok(), "override app failed: {:?}", out.diags.items());
    fs::remove_file(&local_alpha).unwrap();

    // Ambiguous dependency module -> E1110.
    ws.write(
        "dep_dup1/shared.mo",
        b"module Shared\nexpose v\nfn v() : Int\n  1\nend\n",
    );
    ws.write(
        "dep_dup2/shared.mo",
        b"module Shared\nexpose v\nfn v() : Int\n  2\nend\n",
    );
    fs::write(
        &manifest,
        b"[package]\nname = \"ambig\"\n[dependencies]\nd1 = { path = \"../dep_dup1\" }\nd2 = { path = \"../dep_dup2\" }\n",
    )
    .unwrap();
    fs::write(
        &entry,
        b"module Main\nuse Shared\nfn main() : Int\n  Shared.v()\nend\n",
    )
    .unwrap();
    let out = run_stage(
        entry.to_str().unwrap(),
        &LoadOptions::default(),
        CheckStage::Load,
    );
    assert_eq!(out.diags.len(), 1);
    assert_eq!(out.diags.items()[0].code, ErrorCode::PackageManifest);

    // Circular mo.toml dependency -> E1110.
    ws.write(
        "cyc_a/mo.toml",
        b"[package]\nname = \"a\"\n[dependencies]\nb = { path = \"../cyc_b\" }\n",
    );
    ws.write(
        "cyc_b/mo.toml",
        b"[package]\nname = \"b\"\n[dependencies]\na = { path = \"../cyc_a\" }\n",
    );
    let cyc_entry = ws.write("cyc_a/main.mo", b"module Main\nfn main()\nend\n");
    let out = run_stage(
        cyc_entry.to_str().unwrap(),
        &LoadOptions::default(),
        CheckStage::Load,
    );
    assert_eq!(out.diags.len(), 1);
    assert_eq!(out.diags.items()[0].code, ErrorCode::PackageManifest);

    // Malformed manifests + NUL byte -> E1110.
    let bad_manifests: [&[u8]; 10] = [
        b"[broken\n",
        b"[workspace]\nname = \"x\"\n",
        b"orphan = \"x\"\n",
        b"[package]\nversion = \"0.1.0\"\n",
        b"[package]\nname = \"\"\n",
        b"[package]\nname = \"app\"\n[dependencies]\na = { path = \"../dep_leaf\" }\na = { path = \"../dep_lib\" }\n",
        b"[package]\nname = \"app\"\n[dependencies]\na = { path = \"\" }\n",
        b"[package]\nname = \"app\"\n[dependencies]\na = { path = \"../no_such_dir\" }\n",
        b"[package]\nname = \"app\"\n[dependencies]\na = {}\n",
        b"[package]\nname = \"app\"\0\n",
    ];
    fs::write(&entry, b"module Main\nfn main()\nend\n").unwrap();
    for bad in bad_manifests {
        fs::write(&manifest, bad).unwrap();
        let out = run_stage(
            entry.to_str().unwrap(),
            &LoadOptions::default(),
            CheckStage::Load,
        );
        assert_eq!(out.diags.len(), 1);
        assert_eq!(out.diags.items()[0].code, ErrorCode::PackageManifest);
    }
}

#[test]
fn abilities_customizers_cycles_and_depth_limit() {
    assert_eq!(snake_case_name("FooBar"), "foo_bar");
    assert_eq!(snake_case_name("HTTPServer"), "http_server");
    assert_eq!(snake_case_name("FOOBar"), "foo_bar");

    // 1. Recursive nominal type, customizers (compare/to_text/to_json/from_json), Json enum
    //    snake_case collision, capabilities, actors, functions, Map/Set key Hash.
    let ws = TempDir::new("abilities");
    let entry = ws.write(
        "main.mo",
        b"module Main\nuse Json{Json}\n\
          struct Custom(T)\n  value: T\nend\n\
          fn to_text(c: Custom(T)) : Text where T: Text\n  \"#{c.value}\"\nend\n\
          fn compare(a: Custom(T), b: Custom(T)) : Ordering where T: Order\n  Equal\nend\n\
          fn to_json(c: Custom(T)) : Json where T: Json\n  Json.Null\nend\n\
          fn from_json(_j: Json) : Result(Custom(T), Text) where T: Json\n  Error(\"x\")\nend\n\
          enum Tree(T)\n  Leaf(T)\n  Branch(left: Tree(T), right: Tree(T))\nend\n\
          enum ClashJson\n  FooBar\n  FOOBar\nend\n\
          actor Worker\n  calls ping\nend\n\
          fn ping() : Int\n  1\nend\n\
          fn use_types(\n\
            _c: Custom(Int),\n\
            _t: Tree(Int),\n\
            _j: ClashJson,\n\
            _w: Worker,\n\
            _f: fn(Int) : Int,\n\
            _m_int: Map(Int, Text),\n\
            _m_txt: Map(Text, Int)\n\
          ) : Unit\n  Unit\nend\n",
    );
    let out = run_stage(
        entry.to_str().unwrap(),
        &LoadOptions::default(),
        CheckStage::Signatures,
    );
    assert!(out.ok(), "setup failed: {:?}", out.diags.items());
    let interfaces = out.interfaces.unwrap();
    let mut sigs = out.signatures.unwrap();

    // Check Json.Json and Json.JsonNumber native lookup + Json.schema phantom T parameter mask.
    let json_ty = json_type(&interfaces, &sigs, "Json").unwrap();
    let json_num_ty = json_type(&interfaces, &sigs, "JsonNumber").unwrap();
    let use_sym = SymRef::new(
        ModuleId(0),
        interfaces
            .scope(ModuleId(0))
            .find_symbol_index(b"use_types")
            .unwrap(),
    );
    let (_, use_sig) = sigs.function_signature_by_symbol(use_sym).unwrap();
    let params = sigs.types.get(use_sig.ty.unwrap()).arguments.clone();
    let custom_int = params[0];
    let tree_int = params[1];
    let clash_json = params[2];
    let worker_ty = params[3];
    let fn_ty = params[4];
    let map_int_txt = params[5];
    let map_txt_int = params[6];

    let mut ctx = AbilityContext::new(&interfaces, &mut sigs);

    // Custom(Int): Text, Order, Json should bind Function customizers; Equal/Hash derived.
    let p_text = ctx.prove_ability(custom_int, SourceAbility::Text).unwrap();
    assert_eq!(
        ctx.arena.get(p_text).kind,
        AbilityImplementationKind::Function
    );
    assert!(ctx.arena.get(p_text).function.is_some());
    assert_eq!(ctx.arena.get(p_text).parts.len(), 1);

    let p_order = ctx.prove_ability(custom_int, SourceAbility::Order).unwrap();
    assert_eq!(
        ctx.arena.get(p_order).kind,
        AbilityImplementationKind::Function
    );

    let p_json = ctx.prove_ability(custom_int, SourceAbility::Json).unwrap();
    assert_eq!(
        ctx.arena.get(p_json).kind,
        AbilityImplementationKind::Function
    );
    assert!(ctx.arena.get(p_json).encode_function.is_some());
    assert!(ctx.arena.get(p_json).decode_function.is_some());

    // Recursive Tree(Int): Equal, Hash, Order, Text, Json all succeed via cyclic graph!
    for ab in [
        SourceAbility::Equal,
        SourceAbility::Hash,
        SourceAbility::Order,
        SourceAbility::Text,
        SourceAbility::Json,
    ] {
        let p = ctx.prove_ability(tree_int, ab).unwrap();
        assert_eq!(ctx.arena.get(p).kind, AbilityImplementationKind::Derived);
    }

    // Json.Json & Json.JsonNumber have native Json.
    let p_j = ctx.prove_ability(json_ty, SourceAbility::Json).unwrap();
    assert_eq!(ctx.arena.get(p_j).kind, AbilityImplementationKind::Native);
    let p_jn = ctx.prove_ability(json_num_ty, SourceAbility::Json).unwrap();
    assert_eq!(ctx.arena.get(p_jn).kind, AbilityImplementationKind::Native);

    // ClashJson has Equal, but fails Json due to snake_case tag collision ("foo_bar").
    assert!(
        ctx.prove_ability(clash_json, SourceAbility::Equal)
            .is_some()
    );
    assert!(ctx.prove_ability(clash_json, SourceAbility::Json).is_none());
    assert_eq!(ctx.error.as_ref().unwrap().code, ErrorCode::AbilityMissing);
    ctx.reset_cache();

    // Map(Text, Int) has Json, Map(Int, Text) lacks Json.
    assert!(
        ctx.prove_ability(map_txt_int, SourceAbility::Json)
            .is_some()
    );
    assert!(
        ctx.prove_ability(map_int_txt, SourceAbility::Json)
            .is_none()
    );
    assert_eq!(ctx.error.as_ref().unwrap().code, ErrorCode::AbilityMissing);
    ctx.reset_cache();

    // Actors, functions, and all 15 capabilities lack every ability.
    for denied_ty in [worker_ty, fn_ty] {
        assert!(ctx.prove_ability(denied_ty, SourceAbility::Equal).is_none());
        assert_eq!(ctx.error.as_ref().unwrap().code, ErrorCode::AbilityMissing);
        ctx.reset_cache();
    }
    for cap in CAPABILITY_NAMES {
        let cap_ty = ctx.signatures.types.intern_builtin(cap);
        assert!(ctx.prove_ability(cap_ty, SourceAbility::Equal).is_none());
        assert_eq!(ctx.error.as_ref().unwrap().code, ErrorCode::AbilityMissing);
        ctx.reset_cache();
    }

    // Sqlite.Value has Text (Blob uses standalone native proof), while Bytes lacks Text.
    let sqlite_val = ctx.signatures.types.intern_builtin("Sqlite.Value");
    let bytes_ty = ctx.signatures.types.intern_builtin("Bytes");
    assert!(ctx.prove_ability(sqlite_val, SourceAbility::Text).is_some());
    assert!(ctx.prove_ability(bytes_ty, SourceAbility::Text).is_none());
    assert_eq!(ctx.error.as_ref().unwrap().code, ErrorCode::AbilityMissing);
    ctx.reset_cache();

    // 2. Ability dependency depth limit (E1108): 2048 nested structs in Set(Option(R0)).
    let mut deep_src = String::from("module Main\n");
    for i in 0..2048 {
        if i + 1 < 2048 {
            deep_src.push_str(&format!("struct R{i}\n  next: Option(R{})\nend\n", i + 1));
        } else {
            deep_src.push_str(&format!("struct R{i}\n  next: Option(Int)\nend\n"));
        }
    }
    deep_src.push_str("fn keys(_x: Set(Option(R0))) : Unit\n  Unit\nend\n");
    let deep_entry = ws.write("deep/main.mo", deep_src.as_bytes());
    let deep_out = run_stage(
        deep_entry.to_str().unwrap(),
        &no_lib_options(),
        CheckStage::Signatures,
    );
    assert!(!deep_out.ok());
    assert_eq!(deep_out.diags.items()[0].code, ErrorCode::CheckAbilityDepth);
}

#[test]
fn nonregular_mutual_data_recursion_and_service_expr_type_resolvers() {
    let ws = TempDir::new("recursion");
    // Mutual nonregular generic data cycle: A(T) -> B(List(T)) -> A(T) -> E0084.
    let mut_entry = ws.write(
        "mutual/main.mo",
        b"module Main\nstruct A(T)\n  b: B(List(T))\nend\nstruct B(U)\n  a: A(U)\nend\n",
    );
    let out = run_stage(
        mut_entry.to_str().unwrap(),
        &no_lib_options(),
        CheckStage::Signatures,
    );
    assert!(!out.ok());
    assert_eq!(out.diags.items()[0].code, ErrorCode::DataRecursion);
    assert!(
        out.diags.items()[0]
            .message
            .as_str_lossy()
            .contains("A holds B")
    );

    // Implicit loading of Api/Json/Date/Sqlite when source mentions `api` and `Sqlite`.
    let api_entry = ws.write(
        "implicit/main.mo",
        b"module Main\n\
          fn helper(_db: Sqlite.Database, _r: Sqlite.Rows, _v: Sqlite.Value, _f: Files.File) : Unit\n\
            Unit\n\
          end\n\
          fn main()\n\
          end\n",
    );
    let api_out = run_stage(
        api_entry.to_str().unwrap(),
        &LoadOptions::default(),
        CheckStage::Signatures,
    );
    assert!(
        api_out.ok(),
        "implicit Sqlite failed: {:?}",
        api_out.diags.items()
    );
    let set = api_out.modules.unwrap();
    assert!(set.modules.iter().any(|m| m.name == "Sqlite"));
    assert!(set.modules.iter().any(|m| m.name == "Json"));
    assert!(set.modules.iter().any(|m| m.name == "Date"));

    // Test resolve_service_type and resolve_expression_type.
    let svc_entry = ws.write(
        "svc/main.mo",
        b"module Main\nstruct State\n  n: Int\nend\n\
          service\n  state State = State(n: 0)\nend\n\
          fn id(x: T) : T\n  x\nend\n",
    );
    let svc_out = run_stage(
        svc_entry.to_str().unwrap(),
        &no_lib_options(),
        CheckStage::Signatures,
    );
    assert!(svc_out.ok());
    let svc_set = svc_out.modules.unwrap();
    let svc_ifaces = svc_out.interfaces.unwrap();
    let mut svc_sigs = svc_out.signatures.unwrap();
    let tree = &svc_set.modules[0].syntax.tree;
    let items_nid = tree.node(svc_set.modules[0].syntax.root).children[1].unwrap();
    let svc_nid = tree.node(items_nid).children[1].unwrap();
    let svc_clauses = tree.node(svc_nid).children[0].unwrap();
    let state_clause = tree.node(svc_clauses).children[0].unwrap();
    let mut d = DiagBag::new();
    let state_ty = resolve_service_type(
        &svc_set,
        &svc_ifaces,
        &mut svc_sigs,
        ModuleId(0),
        state_clause,
        &mut d,
    )
    .unwrap();
    assert_eq!(svc_sigs.types.get(state_ty).name, "State");
    assert!(!d.has_any());

    // Expression type resolution inside `fn id(x: T) : T` resolves `T` to `id`'s parameter.
    let fn_sym = SymRef::new(
        ModuleId(0),
        svc_ifaces
            .scope(ModuleId(0))
            .find_symbol_index(b"id")
            .unwrap(),
    );
    let (fn_id, _) = svc_sigs.function_signature_by_symbol(fn_sym).unwrap();
    let fn_decl = tree.node(svc_ifaces.symbol(fn_sym).declaration);
    let ret_ann = fn_decl.children[2].unwrap();
    let expr_ty =
        resolve_expression_type(&svc_set, &svc_ifaces, &mut svc_sigs, fn_id, ret_ann, &mut d)
            .unwrap();
    assert_eq!(svc_sigs.types.get(expr_ty).kind, SourceTypeKind::Parameter);
    assert_eq!(svc_sigs.types.get(expr_ty).name, "T");
    assert!(!d.has_any());
    assert!(check_data_recursion(&svc_ifaces, &mut svc_sigs, &mut d));
}

#[test]
fn source_size_and_path_limits() {
    let ws = TempDir::new("limits");
    let mut big = vec![b' '; (MO_LIMIT_FILE_SIZE as usize) + 1];
    for i in (4095..(MO_LIMIT_FILE_SIZE as usize)).step_by(4096) {
        big[i] = b'\n';
    }
    big[..10].copy_from_slice(b"module M0\n");
    let entry = ws.write("m0.mo", &big);
    let out = run_stage(entry.to_str().unwrap(), &no_lib_options(), CheckStage::Load);
    assert_eq!(out.diags.len(), 1);
    assert_eq!(out.diags.items()[0].code, ErrorCode::FileTooBig);

    let long_path = "x".repeat((MO_LIMIT_PATH_SIZE as usize) + 1);
    let out = run_stage(&long_path, &no_lib_options(), CheckStage::Load);
    assert_eq!(out.diags.len(), 1);
    assert_eq!(out.diags.items()[0].code, ErrorCode::PathTooLong);

    // Program size limit (9 * 8 MiB > 64 MiB).
    let mut main_src = String::from("module Main\n");
    for i in 0..9 {
        main_src.push_str(&format!("use M{i}\n"));
    }
    let prog_entry = ws.write("prog/main.mo", main_src.as_bytes());
    let mut chunk = vec![b' '; MO_LIMIT_FILE_SIZE as usize];
    for i in (4095..(MO_LIMIT_FILE_SIZE as usize)).step_by(4096) {
        chunk[i] = b'\n';
    }
    for i in 0..9 {
        let hdr = format!("module M{i}\n");
        chunk[..hdr.len()].copy_from_slice(hdr.as_bytes());
        ws.write(&format!("prog/m{i}.mo"), &chunk);
        chunk[..hdr.len()].fill(b' ');
    }
    let out = run_stage(
        prog_entry.to_str().unwrap(),
        &no_lib_options(),
        CheckStage::Load,
    );
    assert_eq!(out.diags.len(), 1);
    assert_eq!(out.diags.items()[0].code, ErrorCode::ProgramTooBig);
    assert_eq!(MO_LIMIT_PROGRAM_SIZE, 64 << 20);
    assert_eq!(MO_LIMIT_FILES, 4096);
    assert_eq!(MO_LIMIT_MODULE_IMPORTS, 1 << 20);
}

#[test]
fn deterministic_fuzz_smoke_loader_modules_signatures_abilities() {
    let mut seed = 0x5EED_2C01_u64;
    let mut next_u64 = || -> u64 {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };

    let type_atoms = [
        "Int",
        "Float",
        "Text",
        "Bool",
        "Bytes",
        "Range",
        "Console",
        "T",
        "U",
        "A",
        "Box(Int)",
        "Box(T)",
        "List(T)",
        "Map(T, Int)",
        "Set(Float)",
        "Set(T)",
        "Option(A)",
        "Result(T, Text)",
        "Unknown",
        "fn(Int) : Bool",
        "(Int, T)",
        "Missing(Int, Int)",
    ];
    let abilities = ["Equal", "Hash", "Order", "Text", "Number", "Json", "Bogus"];
    let fn_names = [
        "work",
        "ready?",
        "to_text",
        "compare",
        "to_json",
        "from_json",
        "dup",
    ];

    for round in 0..200 {
        let mut sources = SourceMap::new();
        let mut diags = DiagBag::new();

        let mut mod_a = String::from("module A\n");
        if next_u64() % 2 == 0 {
            mod_a.push_str("expose Box(..), Choice(..), work\n");
        } else {
            mod_a.push_str("expose Box, Choice\n");
        }
        if next_u64() % 5 == 0 {
            mod_a.push_str("use Main\n");
        }
        let field_ty = type_atoms[(next_u64() as usize) % type_atoms.len()];
        mod_a.push_str(&format!(
            "struct Box(T)\n  item: {field_ty}\nend\nenum Choice\n  One\n  Two\nend\n\
             fn work(x: Int) : Int\n  x\nend\n"
        ));

        let mut main_src = String::from("module Main\n");
        match next_u64() % 4 {
            0 => main_src.push_str("use A\n"),
            1 => main_src.push_str("use A{Box, Choice}\n"),
            2 => main_src.push_str("use A{One}\n"),
            _ => main_src.push_str("use A{Box, Box}\n"),
        }
        let p_ty = type_atoms[(next_u64() as usize) % type_atoms.len()];
        let r_ty = type_atoms[(next_u64() as usize) % type_atoms.len()];
        let f_name = fn_names[(next_u64() as usize) % fn_names.len()];
        let ab = abilities[(next_u64() as usize) % abilities.len()];
        let where_clause = if next_u64() % 2 == 0 {
            format!(" where T: {ab}")
        } else {
            String::new()
        };
        main_src.push_str(&format!(
            "struct Local(T)\n  x: {p_ty}\nend\n\
             fn {f_name}(a: {p_ty}) : {r_ty}{where_clause}\n  stop \"x\"\nend\n"
        ));
        if round % 7 == 0 {
            main_src.push_str("struct Cycle(T)\n  next: Cycle(List(T))\nend\n");
        }

        sources.add_bytes("main.mo", main_src.as_bytes()).unwrap();
        sources.add_bytes("a.mo", mod_a.as_bytes()).unwrap();
        if let Some(set) = load_program_sources(&mut sources, None, &mut diags)
            && let Some(ifaces) = resolve_modules(&set, &mut diags)
            && let Some(mut sigs) = prepare_available_signatures(&set, &ifaces, &mut diags)
        {
            let _ = check_signature_abilities(&set, &ifaces, &mut sigs, &mut diags);
            let mut ctx = AbilityContext::new(&ifaces, &mut sigs);
            let data_tys: Vec<_> = ctx.signatures.data.iter().map(|d| d.ty).collect();
            for ty in data_tys {
                for ability in SourceAbility::ALL {
                    let _ = ctx.prove_ability(ty, ability);
                    ctx.reset_cache();
                }
            }
        }
    }
}
