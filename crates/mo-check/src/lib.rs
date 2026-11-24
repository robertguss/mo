#![forbid(unsafe_code)]

//! Pure-Rust Mo front-end checking — Part 1:
//! whole-program module loader (including embedded `lib/`, lazy `on_use`, `mo.toml` path
//! dependencies, and root confinement), module interfaces & exposure, shared interned type DAG,
//! signature preparation & nonregular generic data detection, native signatures from `mo-def`,
//! and ability proofs & customizers.
//!
//! Parity target: `src/check/{load.c,modules.c,signatures.c,recursion.c,abilities.c}` and
//! `src/check/program.c:350-402`.
//! Depends only on `mo-def`, `mo-base`, and `mo-syntax`.

pub mod abilities;
pub mod embedded;
pub mod ids;
pub mod load;
pub mod modules;
pub mod signatures;
pub mod types;

#[cfg(test)]
mod tests;

use mo_base::{DiagBag, SourceMap};

pub use abilities::{
    AbilityArena, AbilityContext, AbilityFailure, AbilityImplementation, AbilityImplementationKind,
    SourceAbility, TypeArgument, check_signature_abilities, snake_case_name,
};
pub use embedded::{
    DEFAULT_EMBEDDED_LIBRARY, EmbeddedLibrary, EmbeddedSource, default_embedded_library,
    default_library_root,
};
pub use ids::{
    DataSigId, FuncSigId, ModuleId, NativeSigId, NominalId, ProofId, SymIdx, SymRef, TypeId,
};
pub use load::{
    LibraryMode, LoadOptions, LoadedModule, ModuleSet, ON_USE_LIBRARY_MODULES, is_loaded_on_use,
    is_reserved_on_use, load_program, load_program_sources,
};
pub use modules::{
    ImportedName, ModuleAlias, ModuleInterfaces, ModuleScope, SOURCE_BUILTINS, Symbol,
    is_source_builtin, resolve_modules,
};
pub use signatures::{
    DataSignature, FieldSignature, FunctionSignature, ImportUsage, NativeSignature,
    NominalSignature, Signatures, check_data_recursion, json_type, prepare_available_signatures,
    prepare_signatures, resolve_expression_type, resolve_service_type,
};
pub use types::{
    BUILTIN_CASES, BUILTIN_TYPES, BuiltinCase, CAPABILITY_NAMES, SourceType, SourceTypeKind,
    TypeArena, TypeOwner, builtin_case_by_key, builtin_case_by_name, builtin_case_by_text,
    builtin_case_by_type, builtin_type_arity, is_capability_name,
};

/// Front-end checking stage selectable via `mo-fe check-stage --stage=...`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckStage {
    /// Whole-program module loader (`load.rs`).
    Load,
    /// Loader + module interfaces, overloads, exposure, and imports (`modules.rs`).
    Modules,
    /// Loader + module interfaces + signatures, nonregular generic data, native table, and
    /// signature-level ability checks (`signatures.rs` + `abilities.rs`).
    Signatures,
}

impl CheckStage {
    #[must_use]
    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "load" => Some(Self::Load),
            "modules" => Some(Self::Modules),
            "signatures" => Some(Self::Signatures),
            _ => None,
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Load => "load",
            Self::Modules => "modules",
            Self::Signatures => "signatures",
        }
    }
}

/// Structured result of running [`run_stage`].
#[derive(Debug)]
pub struct StageOutput {
    pub sources: SourceMap,
    pub diags: DiagBag,
    pub modules: Option<ModuleSet>,
    pub interfaces: Option<ModuleInterfaces>,
    pub signatures: Option<Signatures>,
}

impl StageOutput {
    #[must_use]
    pub fn ok(&self) -> bool {
        !self.diags.has_any()
    }
}

/// Runs the front-end pipeline up through `stage` on `entry`, collecting all diagnostics and
/// preserving partial stage products when available.
#[must_use]
pub fn run_stage(entry: &str, options: &LoadOptions, stage: CheckStage) -> StageOutput {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let Some(modules) = load_program(&mut sources, entry, options, &mut diags) else {
        return StageOutput {
            sources,
            diags,
            modules: None,
            interfaces: None,
            signatures: None,
        };
    };
    if stage == CheckStage::Load {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: None,
            signatures: None,
        };
    }

    let Some(interfaces) = resolve_modules(&modules, &mut diags) else {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: None,
            signatures: None,
        };
    };
    if stage == CheckStage::Modules {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: Some(interfaces),
            signatures: None,
        };
    }

    let Some(mut signatures) = prepare_available_signatures(&modules, &interfaces, &mut diags)
    else {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: Some(interfaces),
            signatures: None,
        };
    };
    let _ = check_signature_abilities(&modules, &interfaces, &mut signatures, &mut diags);

    StageOutput {
        sources,
        diags,
        modules: Some(modules),
        interfaces: Some(interfaces),
        signatures: Some(signatures),
    }
}
