//! Canonical source bundling from disk (`src/tools/upgrade-bundle.c` parity) and
//! in-process bundle compilation (for both `mo emit --bundle` children and `mo check --upgrade-from`).

use std::path::{Path, PathBuf};

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_bytecode::load as load_bytecode;
use mo_check::{
    CheckedProgram, LibraryMode, LoadOptions, check_migration_entry, check_program,
    default_embedded_library, default_library_root, load_bundle_with_library, load_program_bytes,
};
use mo_codegen::compile_services;
use mo_core::check_program_services;
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_UPGRADE_RESTART_HEAP, MO_LIMIT_UPGRADE_RESTART_WORK,
};
use mo_lower::{lower_query, lower_services};
use mo_vm::{Image, Limits};

use crate::binary::bundle::{BundleFile, read_bundle, valid_bundle_path, write_bundle};
use crate::binary::compile_context::{
    CompileFile, read_compile_context, read_compile_metadata, write_compile_context,
};
use crate::binary::sha256_bytes;
use crate::store::programs::StoredProgram;

use super::find_entry_service;
use super::types::{MetadataError, build_compile_metadata};

/// Builds a canonical `MOSB\x01` bundle from an entry `.mo` file on disk
/// (`mo_upgrade_bundle_with_library`).
///
/// Returns `(bundle_bytes, sha256_digest)` on success.
pub fn bundle_from_path(
    entry_path: &[u8],
    library: Option<&Path>,
    allow_upgrade_old: bool,
    diags: &mut DiagBag,
    sources: &mut SourceMap,
) -> Option<(Vec<u8>, [u8; 32])> {
    let lib_mode = match library {
        Some(dir) => LibraryMode::Directory(dir.to_path_buf()),
        None => LibraryMode::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        },
    };
    let opts = LoadOptions {
        library: lib_mode,
        upgrade: allow_upgrade_old,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let set = load_program_bytes(sources, entry_path, &opts, diags)?;
    if diags.has_any() {
        return None;
    }

    // Collect non-library, non-UpgradeOld source files and compute normalized paths relative
    // to the common root directory of the entry file.
    let entry_file_idx = set.modules[0].file;
    let entry_str = sources.get(entry_file_idx)?.path_lossy().into_owned();
    let entry_dir = Path::new(&entry_str)
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));

    // Check that the entry module declares a service (when not bundling an upgrade candidate
    // that might still be checked by the upgrade pipeline). Actually, even for serve, checking
    // the program here reports diagnostics with the real disk file path (`tests/errors/e1409`).
    let entry_span = set.modules[0].syntax.span;
    let mut has_service_node = false;
    let root = set.modules[0].syntax.tree.node(set.modules[0].syntax.root);
    if let Some(items_id) = root.children.get(1).copied().flatten() {
        for child in set.modules[0]
            .syntax
            .tree
            .node(items_id)
            .children
            .iter()
            .flatten()
        {
            if set.modules[0].syntax.tree.node(*child).kind == mo_syntax::SyntaxKind::Service {
                has_service_node = true;
            }
        }
    }
    if !has_service_node {
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ToolServiceStart,
                entry_span,
                "this module has no service to start",
            )
            .with_primary_text("a service block declares one (reference 11.5)"),
        );
        return None;
    }

    let mut files: Vec<BundleFile> = Vec::new();
    let mut entry_rel = String::new();
    for (idx, m) in set.modules.iter().enumerate() {
        if m.library || m.name == "UpgradeOld" || m.name.starts_with("UpgradeOld.") {
            continue;
        }
        let sf = sources.get(m.file)?;
        let sf_path = sf.path_lossy();
        let rel = relative_bundle_path(&entry_dir, Path::new(sf_path.as_ref()))?;
        if !valid_bundle_path(&rel) {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::UpgradeBundleFormat,
                Span::NONE,
                format!("source file path `{rel}` cannot be stored in a bundle"),
            ));
            return None;
        }
        if idx == 0 {
            entry_rel = rel.clone();
        }
        if !files.iter().any(|f| f.path == rel) {
            files.push(BundleFile {
                path: rel,
                content: sf.bytes().to_vec(),
            });
        }
    }

    if entry_rel.is_empty() {
        return None;
    }
    write_bundle(&entry_rel, &files).or_else(|| {
        let _ = diags.push(Diagnostic::new(
            ErrorCode::UpgradeBundleSize,
            Span::NONE,
            "source bundle exceeds its size limit",
        ));
        None
    })
}

fn relative_bundle_path(base: &Path, path: &Path) -> Option<String> {
    if let Ok(rel) = path.strip_prefix(base) {
        let s = rel.to_str()?;
        if !s.is_empty() {
            return Some(s.replace('\\', "/"));
        }
    }
    // Fallback to canonicalized paths if one was relative and one absolute.
    if let (Ok(cb), Ok(cp)) = (std::fs::canonicalize(base), std::fs::canonicalize(path))
        && let Ok(rel) = cp.strip_prefix(&cb)
    {
        let s = rel.to_str()?;
        if !s.is_empty() {
            return Some(s.replace('\\', "/"));
        }
    }
    path.file_name()
        .and_then(|n| n.to_str())
        .map(str::to_string)
}

/// Output of compiling a `MOSB\x01` bundle in-process.
pub struct CompiledBundle {
    /// Compiled `.bc` wire bytes.
    pub wire: Vec<u8>,
    /// Initialized [`Image`].
    pub image: Image,
    /// Primary service index.
    pub service_idx: u32,
    /// Canonical [`StoredProgram`] (ordinal set to `0` by caller if needed).
    pub program: StoredProgram,
}

/// Compiles a `MOSB\x01` bundle (optionally against an `old_bundle_or_metadata` and `migration`,
/// or a pre-recorded `compile_context`) into bytecode, initialized [`Image`], and canonical
/// `MOCC\x01` context with embedded `MOCS\x01` metadata.
#[expect(
    clippy::too_many_arguments,
    reason = "bundle compilation takes bundle, optional old bundle, migration, context, library, identity, and diagnostics"
)]
pub fn compile_bundle(
    bundle_bytes: &[u8],
    old_bundle_or_meta: Option<&[u8]>,
    migration: Option<&str>,
    existing_context: Option<&[u8]>,
    library: Option<&Path>,
    build_digest: [u8; 32],
    diags: &mut DiagBag,
    sources: &mut SourceMap,
) -> Option<CompiledBundle> {
    let parsed_bundle = match read_bundle(bundle_bytes) {
        Some(b) => b,
        None => {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::UpgradeBundleFormat,
                Span::NONE,
                "source bundle is malformed or unreadable",
            ));
            return None;
        }
    };

    let lib_mode = match library {
        Some(dir) => LibraryMode::Directory(dir.to_path_buf()),
        None => LibraryMode::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        },
    };

    // Determine UpgradeOld files to inject (from old_bundle_or_meta or existing_context).
    let mut old_files: Vec<CompileFile> = Vec::new();
    let has_migration = migration.is_some_and(|m| !m.is_empty());
    if let Some(old_raw) = old_bundle_or_meta {
        if old_raw.starts_with(b"MOCS\x01") {
            let meta = read_compile_metadata(old_raw)?;
            old_files = meta.types;
        } else {
            // Compile old bundle to extract its UpgradeOld sources.
            let old_parsed = read_bundle(old_raw)?;
            let mut old_sources = SourceMap::new();
            let mut old_diags = DiagBag::new();
            if let Some(entry_f) = old_parsed.files.iter().find(|f| f.path == old_parsed.entry) {
                let _ = old_sources
                    .add_bytes(entry_f.path.as_bytes().to_vec(), entry_f.content.clone());
            }
            for f in &old_parsed.files {
                if f.path != old_parsed.entry {
                    let _ = old_sources.add_bytes(f.path.as_bytes().to_vec(), f.content.clone());
                }
            }
            let old_opts = LoadOptions {
                library: lib_mode.clone(),
                upgrade: true,
                syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
            };
            let old_set = load_bundle_with_library(&mut old_sources, &old_opts, &mut old_diags)?;
            if old_diags.has_any() {
                return None;
            }
            let old_prog = check_program(Some(&old_sources), old_set, &mut old_diags)?;
            if old_diags.has_any() {
                return None;
            }
            old_files = super::types::generate_upgrade_old_sources(&old_prog)?;
        }
    } else if let Some(ctx_raw) = existing_context
        && let Some(ctx) = read_compile_context(ctx_raw)
    {
        old_files = ctx.files;
    }

    if let Some(entry_f) = parsed_bundle
        .files
        .iter()
        .find(|f| f.path == parsed_bundle.entry)
    {
        let _ = sources.add_bytes(entry_f.path.as_bytes().to_vec(), entry_f.content.clone());
    }
    for f in &parsed_bundle.files {
        if f.path != parsed_bundle.entry {
            let _ = sources.add_bytes(f.path.as_bytes().to_vec(), f.content.clone());
        }
    }
    for f in &old_files {
        let _ = sources.add_bytes(f.path.as_bytes().to_vec(), f.source.clone());
    }

    let opts = LoadOptions {
        library: lib_mode,
        upgrade: false,
        syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
    };
    let set = load_bundle_with_library(sources, &opts, diags)?;
    if diags.has_any() {
        return None;
    }
    let mut program: CheckedProgram = check_program(Some(sources), set, diags)?;
    if diags.has_any() {
        return None;
    }

    let mut migration_fn_id = None;
    if let Some(mig_name) = migration.filter(|m| !m.is_empty()) {
        // Extract old_state TypeId from synthesized UpgradeOld.upgrade_input(value: OldState) : OldState.
        let old_state = program
            .modules
            .modules
            .iter()
            .position(|m| m.name == "UpgradeOld")
            .and_then(|midx| {
                let mid = mo_check::ModuleId(midx as u32);
                program.signatures.functions.iter().find_map(|sig| {
                    if sig.function.module == mid
                        && program.interfaces.symbol(sig.function).name == "upgrade_input"
                    {
                        let fn_ty = program.signatures.types.get(sig.ty?);
                        fn_ty.arguments.first().copied()
                    } else {
                        None
                    }
                })
            });
        let Some(old_state_ty) = old_state else {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::UpgradeMigrationSignature,
                Span::NONE,
                "migration requires the previous program's UpgradeOld state type",
            ));
            return None;
        };
        let status = check_migration_entry(&mut program, mig_name.as_bytes(), old_state_ty, diags);
        match status {
            mo_check::QueryEntryStatus::Accepted(fn_id) if !diags.has_any() => {
                migration_fn_id = Some(fn_id);
            }
            _ => {
                if !diags.has_any() {
                    let _ = diags.push(Diagnostic::new(
                        ErrorCode::UpgradeMigrationSignature,
                        Span::NONE,
                        format!(
                            "migration function `{mig_name}` was not found or has invalid signature"
                        ),
                    ));
                }
                return None;
            }
        }
    }

    let lowered = match migration_fn_id {
        Some(fn_id) => lower_query(&program, Some(sources), fn_id, diags)?,
        None => lower_services(&program, Some(sources), diags)?,
    };
    if diags.has_any() {
        return None;
    }
    let services_opt = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    if !check_program_services(&lowered.arena, &lowered.program, services_opt, diags) {
        return None;
    }
    let wire = compile_services(&lowered.arena, &lowered.program, services_opt, diags)?;
    if diags.has_any() {
        return None;
    }

    let bc = match load_bytecode(&wire) {
        Ok(b) => b,
        Err(e) => {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::ToolPipeline,
                Span::NONE,
                format!("compiled bytecode failed verification: {e}"),
            ));
            return None;
        }
    };
    let Some(service_idx) = find_entry_service(&bc) else {
        let _ = diags.push(
            Diagnostic::new(
                ErrorCode::ToolServiceStart,
                program.modules.modules[0].syntax.span,
                "this module has no service to start",
            )
            .with_primary_text("a service block declares one (reference 11.5)"),
        );
        return None;
    };

    let init_limits = Limits {
        heap_size: MO_LIMIT_UPGRADE_RESTART_HEAP as usize,
        ..Limits::default()
    };
    let image = match Image::init_bounded(bc, init_limits, MO_LIMIT_UPGRADE_RESTART_WORK) {
        Ok(img) => img,
        Err(b) => {
            let raw = mo_vm::render_defect_raw(&b.0, &b.1);
            let _ = diags.push(Diagnostic::new(
                ErrorCode::UpgradeRecordedCompile,
                Span::NONE,
                format!("bug: {raw}"),
            ));
            return None;
        }
    };

    let meta_bytes = match build_compile_metadata(&program, &image) {
        Ok(b) => b,
        Err(MetadataError::Timer { name, defect }) => {
            let _ = diags.push(Diagnostic::new(
                ErrorCode::UpgradeTimerInterval,
                Span::NONE,
                format!("mo serve: failure: {name}: service handler failed\nbug: {defect}"),
            ));
            return None;
        }
        Err(MetadataError::Other) => return None,
    };
    let parsed_meta = read_compile_metadata(&meta_bytes)?;
    let schema_digest = sha256_bytes(&parsed_meta.schema);

    let mig_str = if has_migration {
        migration.unwrap_or("")
    } else {
        ""
    };
    let ctx_files = if has_migration { &old_files[..] } else { &[] };
    let (ctx_bytes, ctx_digest) = write_compile_context(mig_str, ctx_files, Some(&meta_bytes))?;

    let image_key = image.file().program_checksum ^ (u64::from(service_idx) << 32);
    let stored = StoredProgram {
        source: parsed_bundle.digest,
        build: build_digest,
        schema: schema_digest,
        context_digest: ctx_digest,
        image: image_key,
        ordinal: 0,
        bundle: bundle_bytes.to_vec(),
        context: ctx_bytes,
    };

    Some(CompiledBundle {
        wire,
        image,
        service_idx,
        program: stored,
    })
}
