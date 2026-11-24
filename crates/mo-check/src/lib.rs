#![forbid(unsafe_code)]

//! Pure-Rust Mo front-end typechecker (`src/check/` full parity).
//!
//! Covers the entire front-end checking pipeline:
//! - Whole-program module loader (`load`) including embedded `lib/`, lazy `on_use`, `mo.toml`
//!   path dependencies, and root confinement.
//! - Module interfaces, overloads, exposure, and imports (`modules`).
//! - Shared interned type DAG (`types`), signature preparation & nonregular generic data
//!   detection, and native signature table from `mo-def` (`signatures`).
//! - Built-in ability proofs & customizers (`abilities`).
//! - Bidirectional local body inference (`infer`), iterative Maranget pattern exhaustiveness
//!   (`patterns`), map-literal key canonicalization (`map_keys`).
//! - Constant dependency & cycle checking (`constants`), polymorphic recursion (`recursion`),
//!   generic capability-capture flow & service purity (`captures`).
//! - `service`/`actor` blocks (`services`), bare-`State` rewrite fixes (`service_fixes`),
//!   service promises & anchors (`service_promises`), HTTP `api` blocks (`apis`),
//!   `test`/`example`/`promise`/`anchor` specs, `main` entry contract, and unused imports
//!   (`program`).
//!
//! Depends only on `mo-def`, `mo-base`, and `mo-syntax`.

pub mod abilities;
pub mod apis;
pub mod captures;
pub mod checked;
pub mod constants;
pub mod embedded;
pub mod ids;
pub mod infer;
pub mod load;
pub mod map_keys;
pub mod modules;
pub mod patterns;
pub mod program;
pub mod recursion;
pub mod service_fixes;
pub mod service_promises;
pub mod services;
pub mod signatures;
pub mod types;

#[cfg(test)]
mod tests;

use mo_base::{DiagBag, SourceMap};

pub use abilities::{
    AbilityArena, AbilityCache, AbilityContext, AbilityFailure, AbilityImplementation,
    AbilityImplementationKind, SourceAbility, TypeArgument, check_signature_abilities,
    snake_case_name,
};
pub use captures::{check_capture_arguments, check_service_purity};
pub use checked::{
    ApiAuthMode, ApiJobReplyKind, ApiMethod, ApiParamKind, ApiReplyKind, CheckedAbilityUse,
    CheckedApi, CheckedApiActor, CheckedApiAuth, CheckedApiJob, CheckedApiParam, CheckedApiRoute,
    CheckedApiSqlite, CheckedApiStatic, CheckedApiStatus, CheckedArm, CheckedBinding, CheckedBody,
    CheckedCondition, CheckedConstant, CheckedEntry, CheckedEntryResult, CheckedExpression,
    CheckedFunction, CheckedLambda, CheckedMatch, CheckedOr, CheckedPattern, CheckedPatternKind,
    CheckedPatternPart, CheckedProgram, CheckedService, CheckedServicePromise, CheckedServiceTimer,
    CheckedServiceTool, CheckedSpec, CheckedSpecKind, CheckedTry, ConstructorRef, InferredBody,
    InferredExpression, PurityRule, QueryEntryStatus, QueryInput, ServiceReply, SpecText,
};
pub use embedded::{
    DEFAULT_EMBEDDED_LIBRARY, EmbeddedLibrary, EmbeddedSource, default_embedded_library,
    default_library_root,
};
pub use ids::{
    CheckedApiId, CheckedBindingId, CheckedConstId, CheckedExprId, CheckedFuncId, CheckedLambdaId,
    CheckedMatchId, CheckedPatternId, CheckedServiceId, CheckedSpecId, DataSigId, FuncSigId,
    ModuleId, NativeSigId, NominalId, ProofId, SymIdx, SymRef, TypeId,
};
pub use infer::{
    ConstantContext, ProgramServicesView, check_constant, check_function, check_service_value,
    infer_function,
};
pub use load::{
    LibraryMode, LoadOptions, LoadedModule, ModuleSet, ON_USE_LIBRARY_MODULES, is_loaded_on_use,
    is_reserved_on_use, load_program, load_program_sources,
};
pub use map_keys::{MapKeyBudget, MapKeyCategory, MapLiteralKey, canonicalize_map_literal_key};
pub use modules::{
    ImportedName, ModuleAlias, ModuleInterfaces, ModuleScope, SOURCE_BUILTINS, Symbol,
    is_source_builtin, resolve_modules,
};
pub use patterns::{
    CoverageOutcome, MatchPattern, MatchPatternArena, MatchPatternId, patterns_cover,
};
pub use program::check_program;
pub use recursion::check_recursion;
pub use services::{check_migration_entry, check_query_entry};
pub use signatures::{
    DataSignature, FieldSignature, FunctionSignature, ImportUsage, NativeSignature,
    NominalSignature, Signatures, check_data_recursion, json_type, prepare_available_signatures,
    prepare_signatures, resolve_expression_type, resolve_expression_type_in, resolve_service_type,
};
pub use types::{
    BUILTIN_CASES, BUILTIN_TYPES, BuiltinCase, CAPABILITY_NAMES, SourceType, SourceTypeKind,
    TypeArena, TypeOwner, builtin_case_by_key, builtin_case_by_name, builtin_case_by_text,
    builtin_case_by_type, builtin_type_arity, is_capability_name,
};

/// Front-end checking stage selectable via `mo-fe check --stage=...`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckStage {
    /// Whole-program module loader (`load.rs`).
    Load,
    /// Loader + module interfaces, overloads, exposure, and imports (`modules.rs`).
    Modules,
    /// Loader + module interfaces + signatures, nonregular generic data, native table, and
    /// signature-level ability checks (`signatures.rs` + `abilities.rs`).
    Signatures,
    /// Complete front-end typechecker (`program.rs` -> `CheckedProgram`).
    Full,
}

impl CheckStage {
    #[must_use]
    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "load" => Some(Self::Load),
            "modules" => Some(Self::Modules),
            "signatures" => Some(Self::Signatures),
            "full" => Some(Self::Full),
            _ => None,
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Load => "load",
            Self::Modules => "modules",
            Self::Signatures => "signatures",
            Self::Full => "full",
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
    pub program: Option<CheckedProgram>,
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
            program: None,
        };
    };
    if stage == CheckStage::Load {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: None,
            signatures: None,
            program: None,
        };
    }

    if stage == CheckStage::Full {
        let program = check_program(Some(&sources), modules, &mut diags);
        return StageOutput {
            sources,
            diags,
            modules: None,
            interfaces: None,
            signatures: None,
            program,
        };
    }

    let Some(interfaces) = resolve_modules(&modules, &mut diags) else {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: None,
            signatures: None,
            program: None,
        };
    };
    if stage == CheckStage::Modules {
        return StageOutput {
            sources,
            diags,
            modules: Some(modules),
            interfaces: Some(interfaces),
            signatures: None,
            program: None,
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
            program: None,
        };
    };
    let _ = check_signature_abilities(&modules, &interfaces, &mut signatures, &mut diags);

    StageOutput {
        sources,
        diags,
        modules: Some(modules),
        interfaces: Some(interfaces),
        signatures: Some(signatures),
        program: None,
    }
}
