#![forbid(unsafe_code)]

//! Automated deterministic replay of all 48 `fuzz/corpus/*` seed directories (377+ files)
//! against the pure-Rust Mo compiler, bytecode verifier, binary codecs, CLI parser, and
//! embedded protocol VM. Asserts zero panics, zero hangs, and 100% directory coverage.

use std::fs;
use std::path::{Path, PathBuf};

use mo_base::{DiagBag, SourceMap};
use mo_bytecode::{disassemble, load as load_bytecode, write as write_bytecode};
use mo_check::{LibraryMode, LoadOptions, check_program, load_bundle_with_library};
use mo_cli::parse_cli;
use mo_codegen::{compile as compile_core, compile_services};
use mo_core::{
    CoreTextKind, check_program as check_core_program, detect_kind, parse_document, parse_program,
    parse_run, print_document,
};
use mo_def::limits::{MO_LIMIT_MCP_REQUEST_WORK, MO_SOURCE_STACK_SIZE};
use mo_host::HostContext;
use mo_lower::lower_services;
use mo_prim::unicode16::{display_width_bytes, next_grapheme_bytes};
use mo_service::binary::bundle::read_bundle;
use mo_service::binary::compile_context::{read_compile_context, read_compile_metadata};
use mo_service::binary::export::{export_schema_digest, read_export};
use mo_service::binary::failure::read_failure_record;
use mo_service::binary::log_record::{read_log_record, walk_log_record};
use mo_service::binary::upload::UploadStage;
use mo_service::binary::{hex_decode_exact, sanitize_terminal_text, sha256_bytes};
use mo_service::listener::credentials::Bearer;
use mo_service::protocol::{Pf, ProtocolFunctions, build_protocol_image};
use mo_service::store::{StoreRecordKind, decode_frame};
use mo_syntax::{FormatOptions, format_module, parse_fragment, parse_module, read_header};
use mo_vm::{Image, Limits, Process};

fn repo_root() -> PathBuf {
    let manifest = Path::new(env!("CARGO_MANIFEST_DIR"));
    manifest
        .parent()
        .and_then(Path::parent)
        .map_or_else(|| manifest.to_path_buf(), Path::to_path_buf)
}

fn replay_mo_syntax(bytes: &[u8]) {
    let mut sources = SourceMap::new();
    let Ok(file) = sources.add_bytes(b"fuzz.mo".to_vec(), bytes.to_vec()) else {
        return;
    };
    let mut d1 = DiagBag::new();
    if let Some(parsed) = parse_module(&sources, file, &mut d1) {
        let _ = format_module(&parsed, FormatOptions::new(100, display_width_bytes));
    }
    let mut d2 = DiagBag::new();
    let _ = parse_fragment(&sources, file, &mut d2);
    let mut d3 = DiagBag::new();
    let _ = read_header(&sources, file, &mut d3);
}

fn replay_mo_frontend(bytes: &[u8], lower_and_codegen: bool) {
    replay_mo_syntax(bytes);
    let mut sources = SourceMap::new();
    if sources
        .add_bytes(b"main.mo".to_vec(), bytes.to_vec())
        .is_err()
    {
        return;
    }
    let opts = LoadOptions {
        library: LibraryMode::None,
        ..LoadOptions::default()
    };
    let mut diags = DiagBag::new();
    let Some(set) = load_bundle_with_library(&mut sources, &opts, &mut diags) else {
        return;
    };
    let Some(checked) = check_program(Some(&sources), set, &mut diags) else {
        return;
    };
    if lower_and_codegen {
        let mut ldiags = DiagBag::new();
        if let Some(lowered) = lower_services(&checked, Some(&sources), &mut ldiags) {
            let mut cdiags = DiagBag::new();
            let _ = check_core_program(&lowered.arena, &lowered.program, &mut cdiags);
            if let Some(wire) = compile_services(
                &lowered.arena,
                &lowered.program,
                Some(&lowered.services),
                &mut cdiags,
            ) {
                let _ = load_bytecode(&wire);
            }
        }
    }
}

fn replay_core_doc(bytes: &[u8], run_codegen: bool) {
    if let Some(kind) = detect_kind(bytes) {
        let mut diags = DiagBag::new();
        if let Some(parsed) = parse_document(kind, bytes, &mut diags) {
            let _ = print_document(&parsed.arena, &parsed.value);
        }
    }
    let mut d_prog = DiagBag::new();
    if let Some(p) = parse_program(bytes, &mut d_prog) {
        let mut cd = DiagBag::new();
        let ok = check_core_program(&p.arena, &p.value, &mut cd);
        if ok && run_codegen {
            let mut gd = DiagBag::new();
            if let Some(wire) = compile_core(&p.arena, &p.value, &mut gd) {
                let _ = load_bytecode(&wire);
            }
        }
    }
    let mut d_run = DiagBag::new();
    if let Some(r) = parse_run(bytes, &mut d_run) {
        let mut cd = DiagBag::new();
        let ok = check_core_program(&r.arena, &r.value.program, &mut cd);
        if ok && run_codegen {
            let mut gd = DiagBag::new();
            if let Some(wire) = compile_core(&r.arena, &r.value.program, &mut gd) {
                let _ = load_bytecode(&wire);
            }
        }
    }
    // Also exercise explicit kinds on small inputs to cover malformed documents cleanly.
    if bytes.len() <= 64 * 1024 {
        let mut d_lit = DiagBag::new();
        let _ = parse_document(CoreTextKind::Program, bytes, &mut d_lit);
    }
}

fn replay_protocol_vm(proto: &Image, fns: &ProtocolFunctions, bytes: &[u8]) {
    let text = String::from_utf8_lossy(bytes).into_owned();
    let limits = Limits {
        heap_size: 8 * 1024 * 1024,
        ..Limits::default()
    };
    let mut proc = Process::new(proto, limits, HostContext::stdio(Vec::new(), Vec::new()));
    if let Ok(arg) = proc.heap().alloc_text(text) {
        let _ = proc.call_with_budget(fns.get(Pf::FetchRead), vec![arg], MO_LIMIT_MCP_REQUEST_WORK);
    }
}

fn replay_upgrade_pages(bytes: &[u8]) {
    let total = if bytes.len() >= 4 {
        u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as usize
    } else {
        bytes.len()
    };
    if let Some(mut stage) = UploadStage::open(total.min(1024 * 1024)) {
        let payload = if bytes.len() > 4 { &bytes[4..] } else { bytes };
        let _ = stage.push_page(0, payload);
    }
    if let Ok(s) = std::str::from_utf8(bytes) {
        let _ = hex_decode_exact::<32>(s);
    }
}

fn replay_low_level_seed(bytes: &[u8]) {
    let _ = sha256_bytes(bytes);
    let _ = Bearer::from_bytes(bytes);
    let _ = display_width_bytes(bytes);
    let mut off = 0usize;
    while off < bytes.len() {
        let Some(step) = next_grapheme_bytes(&bytes[off..]) else {
            break;
        };
        if step == 0 {
            break;
        }
        off += step;
    }
    if let Ok(s) = std::str::from_utf8(bytes) {
        let _ = sanitize_terminal_text(s);
    }
    let _ = export_schema_digest(bytes);
    let _ = load_bytecode(bytes);
}

fn replay_seed(dir_name: &str, bytes: &[u8], proto: &Image, fns: &ProtocolFunctions) -> bool {
    match dir_name {
        "fuzz_sources" | "fuzz_syntax" | "fuzz_grammar" => replay_mo_syntax(bytes),
        "fuzz_check" | "fuzz_bodies" | "fuzz_mutants" | "fuzz_strength" => {
            replay_mo_frontend(bytes, false);
        }
        "fuzz_program" | "fuzz_lower" => replay_mo_frontend(bytes, true),
        "fuzz_core_text" | "fuzz_core_check" => replay_core_doc(bytes, false),
        "fuzz_codegen" => replay_core_doc(bytes, true),
        "fuzz_bytecode_io" => {
            if let Ok(bc) = load_bytecode(bytes)
                && let Ok(rewritten) = write_bytecode(&bc)
            {
                let _ = disassemble(&rewritten);
            }
        }
        "fuzz_bytecode_io_large" => {
            let _ = load_bytecode(bytes);
        }
        "fuzz_artifact" | "fuzz_export" => {
            let _ = read_export(bytes);
        }
        "fuzz_bundle" => {
            let _ = read_bundle(bytes);
        }
        "fuzz_compile_context" => {
            let _ = read_compile_context(bytes);
            let _ = read_compile_metadata(bytes);
        }
        "fuzz_log_record" => {
            let _ = read_log_record(bytes);
            let _ = walk_log_record(bytes.len(), |off, dst| {
                if off + dst.len() > bytes.len() {
                    return false;
                }
                dst.copy_from_slice(&bytes[off..off + dst.len()]);
                true
            });
        }
        "fuzz_service_failure" => {
            let _ = read_failure_record(bytes);
        }
        "fuzz_service_log" => {
            let _ = decode_frame(StoreRecordKind::Log, bytes);
            let _ = read_log_record(bytes);
        }
        "fuzz_service_snapshot" => {
            let _ = decode_frame(StoreRecordKind::State, bytes);
            let _ = export_schema_digest(bytes);
        }
        "fuzz_saved_state" | "fuzz_upgrade_schema" => {
            let _ = export_schema_digest(bytes);
            let _ = read_compile_metadata(bytes);
            let _ = decode_frame(StoreRecordKind::State, bytes);
        }
        "fuzz_upgrade_pages" => replay_upgrade_pages(bytes),
        "fuzz_history" => {
            for kind in [
                StoreRecordKind::State,
                StoreRecordKind::Log,
                StoreRecordKind::FailureState,
                StoreRecordKind::FailureMessage,
            ] {
                let _ = decode_frame(kind, bytes);
            }
            let _ = read_failure_record(bytes);
            let _ = read_log_record(bytes);
        }
        "fuzz_cli" => {
            let argv: Vec<Vec<u8>> = bytes.split(|&b| b == 0).map(<[u8]>::to_vec).collect();
            let _ = parse_cli(&argv);
        }
        "fuzz_fetch" => {
            let _ = read_export(bytes);
            let _ = read_bundle(bytes);
            replay_protocol_vm(proto, fns, bytes);
        }
        "fuzz_api_server"
        | "fuzz_http"
        | "fuzz_http_client"
        | "fuzz_protocol_server"
        | "fuzz_mcp_number"
        | "fuzz_mcp_rpc"
        | "fuzz_mcp_value"
        | "fuzz_mcp_views" => {
            let _ = export_schema_digest(bytes);
            replay_protocol_vm(proto, fns, bytes);
        }
        "fuzz_command" | "fuzz_crypto" | "fuzz_heap" | "fuzz_host" | "fuzz_host_real"
        | "fuzz_inputs" | "fuzz_network" | "fuzz_network_fake" | "fuzz_prim" | "fuzz_sqlite"
        | "fuzz_tls" | "fuzz_types" => {
            replay_low_level_seed(bytes);
        }
        _ => return false,
    }
    true
}

#[test]
fn replay_all_fuzz_corpus_directories_without_panic() {
    let handle = std::thread::Builder::new()
        .name("mo-corpus-replay".to_string())
        .stack_size(MO_SOURCE_STACK_SIZE as usize)
        .spawn(|| {
            let corpus_root = repo_root().join("fuzz/corpus");
            let mut dirs: Vec<PathBuf> = fs::read_dir(&corpus_root)
                .expect("read fuzz/corpus")
                .filter_map(Result::ok)
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect();
            dirs.sort();
            assert!(
                dirs.len() >= 48,
                "expected >= 48 fuzz/corpus directories, found {}",
                dirs.len()
            );

            let (proto, fns) = build_protocol_image().expect("embedded protocol image");
            let mut total_files = 0usize;
            for dir in &dirs {
                let dir_name = dir
                    .file_name()
                    .and_then(|s| s.to_str())
                    .expect("utf-8 corpus dir name");
                let mut files: Vec<PathBuf> = fs::read_dir(dir)
                    .expect("read corpus dir")
                    .filter_map(Result::ok)
                    .map(|e| e.path())
                    .filter(|p| p.is_file())
                    .collect();
                files.sort();
                assert!(!files.is_empty(), "empty corpus dir: {dir_name}");
                for file in &files {
                    let bytes = fs::read(file).expect("read corpus seed");
                    assert!(
                        replay_seed(dir_name, &bytes, &proto, &fns),
                        "unhandled fuzz/corpus directory: {dir_name}"
                    );
                    total_files += 1;
                }
            }
            assert!(
                total_files >= 377,
                "expected >= 377 corpus seeds, replayed {total_files}"
            );
        })
        .expect("spawn corpus replay thread");
    handle.join().expect("corpus replay thread must not panic");
}
