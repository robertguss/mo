//! `UpgradeOld` source synthesis and `MOCS\x01` compile metadata builder
//! (`src/tools/upgrade-types.c` and `src/tools/upgrade-metadata.c` parity).

use std::fmt::Write as _;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{BytecodeFile, TypeKind};
use mo_check::{CheckedProgram, ModuleId, SourceType, SourceTypeKind, SymRef, TypeId, TypeOwner};
use mo_def::limits::{
    MO_LIMIT_SAVED_DEPTH, MO_LIMIT_SOURCE_TYPE_DEPTH, MO_LIMIT_UPGRADE_RESTART_WORK,
};
use mo_heap::saved::write_schema;
use mo_host::HostContext;
use mo_syntax::SyntaxKind;
use mo_vm::{Image, Limits, Outcome, Process};
use sha2::{Digest, Sha256};

use crate::binary::compile_context::{
    CompileFile, CompileMetadata, CompileTimerEntry, CompileToolEntry, write_compile_metadata,
};

use super::find_entry_service;

/// Synthesizes `UpgradeOld*.mo` source modules exposing the entry service's `State` type and
/// all reachable user-defined nominal types (`src/tools/upgrade-types.c:mo_upgrade_old_sources`).
#[must_use]
pub fn generate_upgrade_old_sources(program: &CheckedProgram) -> Option<Vec<CompileFile>> {
    let svc = program
        .services
        .iter()
        .find(|s| s.module.0 == 0 && !s.is_actor)?;
    let state_ty = svc.state;
    let owner = program
        .signatures
        .types
        .get(state_ty)
        .module
        .unwrap_or(ModuleId(0));

    let mut symbols: Vec<SymRef> = Vec::new();
    collect_type(program, state_ty, 0, &mut symbols)?;

    let mod_count = program.modules.modules.len();
    let mut out = Vec::new();

    for m in 0..mod_count {
        let mid = ModuleId(m as u32);
        let used = mid == owner || symbols.iter().any(|s| s.module == mid);
        if !used {
            continue;
        }
        let mut needed = vec![false; mod_count];
        for data in &program.signatures.data {
            if data.symbol.module == mid {
                let TypeOwner::Source(decl_ref) = program.signatures.types.get(data.ty).owner
                else {
                    continue;
                };
                if symbols.contains(&decl_ref) {
                    for f in &data.fields {
                        collect_imports(program, f.ty, mid, &mut needed);
                    }
                }
            }
        }
        if mid == owner {
            collect_imports(program, state_ty, mid, &mut needed);
        }

        let mut src = String::new();
        let mod_name = old_module_name(program, owner, mid);
        let _ = writeln!(src, "module {mod_name}");

        let exposed: Vec<&str> = symbols
            .iter()
            .filter(|s| s.module == mid)
            .map(|s| program.interfaces.symbol(*s).name.as_str())
            .collect();
        if !exposed.is_empty() {
            src.push_str("expose ");
            for (idx, name) in exposed.iter().enumerate() {
                if idx > 0 {
                    src.push_str(", ");
                }
                let _ = write!(src, "{name}(..)");
            }
            src.push('\n');
        }

        for (i, &need) in needed.iter().enumerate() {
            if need {
                let target = old_module_name(program, owner, ModuleId(i as u32));
                let _ = writeln!(src, "use {target}");
            }
        }

        for &sym_ref in &symbols {
            if sym_ref.module != mid {
                continue;
            }
            let sym = program.interfaces.symbol(sym_ref);
            let first = program.signatures.data.iter().find(|d| {
                matches!(
                    program.signatures.types.get(d.ty).owner,
                    TypeOwner::Source(sr) if sr == sym_ref
                )
            })?;
            let decl_kind = program
                .modules
                .module(mid)
                .syntax
                .tree
                .node(sym.declaration)
                .kind;
            let is_struct = decl_kind == SyntaxKind::Struct;

            src.push_str(if is_struct { "struct " } else { "enum " });
            format_type(program, owner, mid, first.ty, 0, &mut src)?;
            src.push('\n');
            if is_struct {
                format_fields(program, owner, mid, first, true, &mut src)?;
            } else {
                for &case_idx in &sym.cases {
                    let case_ref = SymRef {
                        module: mid,
                        symbol: case_idx,
                    };
                    let case_sym = program.interfaces.symbol(case_ref);
                    let case_data = program
                        .signatures
                        .data
                        .iter()
                        .find(|d| d.symbol == case_ref)?;
                    src.push_str(&case_sym.name);
                    format_fields(program, owner, mid, case_data, false, &mut src)?;
                    src.push('\n');
                }
            }
            src.push_str("end\n");
        }

        if mid == owner {
            src.push_str("fn upgrade_input(value: ");
            format_type(program, owner, mid, state_ty, 0, &mut src)?;
            src.push_str(") : ");
            format_type(program, owner, mid, state_ty, 0, &mut src)?;
            src.push_str("\nvalue\nend\n");
        }

        // Only modules prefixed with UpgradeOld. (i.e. UpgradeOld.mo -> stored as UpgradeOld.State or UpgradeOld.<Name>.mo)
        // Wait: match C's exact path: `module_name(&path) + ".mo"`!
        // Note: in `mo_compile_files_read`, paths must start with `"UpgradeOld."` and end with `".mo"`.
        // Wait: for `owner` module, C emits `UpgradeOld.mo` in `upgrade-types.c`, OR does `compile_files_read` allow `UpgradeOld.mo`?
        // Look at `compile-metadata.c` line 25: `f->path.length < 13 || memcmp(f->path.bytes, "UpgradeOld.", 11) != 0`!
        // WAIT: `UpgradeOld.mo` has length 13 and starts with `"UpgradeOld."` (`U-p-g-r-a-d-e-O-l-d-.-m-o` = 13 chars!!)!
        // Let's verify: `"UpgradeOld.mo".len() == 13` and starts with `"UpgradeOld."` and ends with `".mo"`! YES!!
        let path = format!("{mod_name}.mo");
        out.push(CompileFile {
            path,
            source: src.into_bytes(),
        });
    }

    out.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    Some(out)
}

fn collect_type(
    program: &CheckedProgram,
    ty: TypeId,
    depth: u32,
    out: &mut Vec<SymRef>,
) -> Option<()> {
    if depth as u64 > MO_LIMIT_SOURCE_TYPE_DEPTH {
        return None;
    }
    let st: &SourceType = program.signatures.types.get(ty);
    for &arg in &st.arguments {
        collect_type(program, arg, depth + 1, out)?;
    }
    if st.kind != SourceTypeKind::Named {
        return Some(());
    }
    let TypeOwner::Source(sym_ref) = st.owner else {
        return Some(());
    };
    if program.modules.module(sym_ref.module).library {
        return Some(());
    }
    if out.contains(&sym_ref) {
        return Some(());
    }
    out.push(sym_ref);
    for data in &program.signatures.data {
        if matches!(
            program.signatures.types.get(data.ty).owner,
            TypeOwner::Source(sr) if sr == sym_ref
        ) {
            for j in 0..data.fields.len() {
                collect_type(program, data.field_by_index(j).ty, depth + 1, out)?;
            }
        }
    }
    Some(())
}

fn collect_imports(program: &CheckedProgram, ty: TypeId, current: ModuleId, needed: &mut [bool]) {
    let st = program.signatures.types.get(ty);
    if let Some(mid) = st.module
        && mid != current
        && st.kind != SourceTypeKind::Parameter
    {
        needed[mid.0 as usize] = true;
    }
    for &arg in &st.arguments {
        collect_imports(program, arg, current, needed);
    }
}

fn old_module_name(program: &CheckedProgram, owner: ModuleId, mid: ModuleId) -> String {
    let m = program.modules.module(mid);
    if m.library {
        m.name.clone()
    } else if mid == owner {
        "UpgradeOld".to_string()
    } else {
        format!("UpgradeOld.{}", m.name)
    }
}

fn format_type(
    program: &CheckedProgram,
    owner: ModuleId,
    current: ModuleId,
    ty: TypeId,
    depth: u32,
    out: &mut String,
) -> Option<()> {
    if depth as u64 > MO_LIMIT_SOURCE_TYPE_DEPTH {
        return None;
    }
    let st = program.signatures.types.get(ty);
    if st.kind == SourceTypeKind::Function {
        return None;
    }
    let is_tuple = st.kind == SourceTypeKind::Tuple;
    if !is_tuple {
        if let Some(mid) = st.module
            && mid != current
            && st.kind != SourceTypeKind::Parameter
        {
            out.push_str(&old_module_name(program, owner, mid));
            out.push('.');
        }
        out.push_str(&st.name);
    }
    if !st.arguments.is_empty() || is_tuple {
        out.push('(');
        for (i, &arg) in st.arguments.iter().enumerate() {
            if i > 0 {
                out.push_str(", ");
            }
            format_type(program, owner, current, arg, depth + 1, out)?;
        }
        out.push(')');
    }
    Some(())
}

fn format_fields(
    program: &CheckedProgram,
    owner: ModuleId,
    current: ModuleId,
    data: &mo_check::DataSignature,
    is_record: bool,
    out: &mut String,
) -> Option<()> {
    let count = data.fields.len();
    if !is_record && count > 0 {
        out.push('(');
    }
    for i in 0..count {
        let f = data.field_by_index(i);
        if !is_record && i > 0 {
            out.push_str(", ");
        }
        if let Some(ref name) = f.name {
            out.push_str(name);
            out.push_str(": ");
        }
        format_type(program, owner, current, f.ty, 0, out)?;
        if is_record {
            out.push('\n');
        }
    }
    if !is_record && count > 0 {
        out.push(')');
    }
    Some(())
}

/// Failure kind from building `MOCS\x01` compile metadata (`enum mo_upgrade_metadata_status`).
#[derive(Debug, Clone)]
pub enum MetadataError {
    /// Timer interval expression trapped or returned a non-positive duration.
    Timer {
        /// Timer handler name.
        name: String,
        /// Rendered defect text (or reason).
        defect: String,
    },
    /// Any other error building or encoding metadata.
    Other,
}

/// Computes canonical `MOCS\x01` compile metadata for a checked + compiled service program
/// (`src/tools/upgrade-metadata.c:mo_upgrade_metadata`).
pub fn build_compile_metadata(
    program: &CheckedProgram,
    image: &Image,
) -> Result<Vec<u8>, MetadataError> {
    let file = image.file();
    let svc_idx = find_entry_service(file).ok_or(MetadataError::Other)?;
    let svc = &file.services[svc_idx as usize];

    let mut schema = Vec::new();
    write_schema(&file.types, svc.state_type, &mut schema).map_err(|_| MetadataError::Other)?;

    let old_files = generate_upgrade_old_sources(program).ok_or(MetadataError::Other)?;
    let token = file.string(svc.token_env).to_string();

    let mut flags = vec![0u8; file.types.items.len()];
    for s in &svc.shapes {
        flags[s.ty as usize] = if s.is_struct { 1 } else { 2 };
    }

    let mut tools = Vec::with_capacity(svc.tools.len());
    for tool in &svc.tools {
        let mut hasher = Sha256::new();
        hasher.update((u32::from(tool.read_only)).to_le_bytes());
        let desc = file.string(tool.description);
        hasher.update((desc.len() as u32).to_le_bytes());
        hasher.update(desc.as_bytes());
        hasher.update((tool.argument_types.len() as u32).to_le_bytes());
        for (&aname_idx, &aty) in tool.argument_names.iter().zip(&tool.argument_types) {
            let aname = file.string(aname_idx);
            hasher.update((aname.len() as u32).to_le_bytes());
            hasher.update(aname.as_bytes());
            hash_type(&mut hasher, &file.types, aty, &flags).ok_or(MetadataError::Other)?;
        }
        hash_type(&mut hasher, &file.types, tool.result_type, &flags)
            .ok_or(MetadataError::Other)?;
        let digest_raw = hasher.finalize();
        let mut digest = [0u8; 32];
        digest.copy_from_slice(&digest_raw);
        tools.push(CompileToolEntry {
            name: file.string(tool.name).to_string(),
            digest,
        });
    }

    let mut timers = Vec::with_capacity(svc.timers.len());
    let mut proc = Process::new(
        image,
        Limits::default(),
        HostContext::stdio(Vec::new(), Vec::new()),
    );
    for timer in &svc.timers {
        let name = extract_timer_name(file, timer.function);
        let (outcome, _) =
            proc.call_with_budget(timer.interval, Vec::new(), MO_LIMIT_UPGRADE_RESTART_WORK);
        let dur = match outcome {
            Outcome::Returned(v) => {
                let d = v.as_duration();
                if d <= 0 {
                    return Err(MetadataError::Timer {
                        name,
                        defect: "timer interval must be positive\n".to_string(),
                    });
                }
                d as u64
            }
            Outcome::Failed(d) | Outcome::Stopped(d) => {
                let defect = mo_vm::render_defect_raw(file, &d);
                return Err(MetadataError::Timer { name, defect });
            }
        };
        timers.push(CompileTimerEntry {
            name,
            interval: dur,
        });
    }

    let meta = CompileMetadata {
        token,
        schema,
        types: old_files,
        tools,
        timers,
    };
    write_compile_metadata(&meta).ok_or(MetadataError::Other)
}

fn hash_type(hasher: &mut Sha256, types: &TypeTable, ty: u32, flags: &[u8]) -> Option<()> {
    let mut schema = Vec::new();
    write_schema(types, ty, &mut schema).ok()?;
    let schema_digest = Sha256::digest(&schema);
    hasher.update(schema_digest);
    let mut seen = vec![false; types.items.len()];
    hash_shapes(types, ty, flags, &mut seen, hasher, 0)
}

fn hash_shapes(
    types: &TypeTable,
    index: u32,
    flags: &[u8],
    seen: &mut [bool],
    hasher: &mut Sha256,
    depth: u32,
) -> Option<()> {
    if depth as u64 == MO_LIMIT_SAVED_DEPTH {
        return None;
    }
    let idx = index as usize;
    if seen[idx] {
        return Some(());
    }
    seen[idx] = true;
    hasher.update([flags[idx]]);
    let t = types.get(index);
    for &p in &t.parameters {
        hash_shapes(types, p, flags, seen, hasher, depth + 1)?;
    }
    if t.kind == TypeKind::Record {
        for f in &t.fields {
            hash_shapes(types, f.ty, flags, seen, hasher, depth + 1)?;
        }
    } else if t.kind == TypeKind::Choice {
        for c in &t.cases {
            for f in &c.fields {
                hash_shapes(types, f.ty, flags, seen, hasher, depth + 1)?;
            }
        }
    }
    Some(())
}

fn extract_timer_name(file: &BytecodeFile, fn_idx: u32) -> String {
    let full = file.function_name(fn_idx);
    let prefix = full.split('@').next().unwrap_or(full);
    prefix.rsplit('.').next().unwrap_or(prefix).to_string()
}
