//! Built-in ability proofs (`Equal`, `Hash`, `Order`, `Text`, `Number`, `Json`),
//! customizer binding (`compare`, `to_text`, `to_json`, `from_json`), collection key
//! constraints, and signature-level ability checks
//! (`src/check/abilities.c` + `src/check/program.c:360-402` parity).

use std::collections::HashMap;

use mo_base::{DiagBag, Diagnostic, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_ABILITY_DEPTH, MO_LIMIT_SOURCE_CHECK_WORK,
    MO_LIMIT_SOURCE_TYPE_DEPTH,
};
use mo_syntax::SyntaxKind;

use crate::ids::{FuncSigId, ProofId, SymRef, TypeId};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::signatures::Signatures;
use crate::types::{
    SourceTypeKind, TypeOwner, builtin_case_by_key, builtin_case_by_type, is_capability_name,
};

/// The six built-in surface abilities (`enum mo_source_ability`).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SourceAbility {
    Equal = 0,
    Hash = 1,
    Order = 2,
    Text = 3,
    Number = 4,
    Json = 5,
}

impl SourceAbility {
    pub const ALL: [Self; 6] = [
        Self::Equal,
        Self::Hash,
        Self::Order,
        Self::Text,
        Self::Number,
        Self::Json,
    ];

    #[must_use]
    pub const fn index(self) -> usize {
        self as usize
    }

    #[must_use]
    pub const fn bit(self) -> u8 {
        1u8 << (self as u8)
    }

    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Equal => "Equal",
            Self::Hash => "Hash",
            Self::Order => "Order",
            Self::Text => "Text",
            Self::Number => "Number",
            Self::Json => "Json",
        }
    }

    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        match bytes {
            b"Equal" => Some(Self::Equal),
            b"Hash" => Some(Self::Hash),
            b"Order" => Some(Self::Order),
            b"Text" => Some(Self::Text),
            b"Number" => Some(Self::Number),
            b"Json" => Some(Self::Json),
            _ => None,
        }
    }
}

/// How an ability proof is realized (`enum mo_ability_implementation_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AbilityImplementationKind {
    Native,
    Derived,
    Function,
    Parameter,
}

/// Binding of one generic type parameter to its concrete type argument (`struct mo_type_argument`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TypeArgument {
    pub parameter: TypeId,
    pub ty: TypeId,
}

/// One arena-allocated ability proof node (`struct mo_ability_implementation`).
///
/// Recursive nominal proofs form a shared cyclic graph addressed by [`ProofId`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AbilityImplementation {
    pub ability: SourceAbility,
    pub ty: TypeId,
    pub kind: AbilityImplementationKind,
    pub parts: Vec<ProofId>,
    pub part_parameters: Vec<TypeId>,
    pub function: Option<SymRef>,
    pub arguments: Vec<TypeArgument>,
    pub encode_function: Option<SymRef>,
    pub encode_arguments: Vec<TypeArgument>,
    pub encode_parts: Vec<ProofId>,
    pub encode_part_parameters: Vec<TypeId>,
    pub decode_function: Option<SymRef>,
    pub decode_arguments: Vec<TypeArgument>,
    pub decode_parts: Vec<ProofId>,
    pub decode_part_parameters: Vec<TypeId>,
}

/// Arena storing [`AbilityImplementation`] proof graph nodes.
#[derive(Debug, Clone, Default)]
pub struct AbilityArena {
    proofs: Vec<AbilityImplementation>,
}

impl AbilityArena {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.proofs.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.proofs.is_empty()
    }

    #[must_use]
    pub fn get(&self, id: ProofId) -> &AbilityImplementation {
        &self.proofs[id.index()]
    }

    pub fn get_mut(&mut self, id: ProofId) -> &mut AbilityImplementation {
        &mut self.proofs[id.index()]
    }

    pub fn alloc(&mut self, ability: SourceAbility, ty: TypeId) -> ProofId {
        let id = ProofId(self.proofs.len() as u32);
        self.proofs.push(AbilityImplementation {
            ability,
            ty,
            kind: AbilityImplementationKind::Native,
            parts: Vec::new(),
            part_parameters: Vec::new(),
            function: None,
            arguments: Vec::new(),
            encode_function: None,
            encode_arguments: Vec::new(),
            encode_parts: Vec::new(),
            encode_part_parameters: Vec::new(),
            decode_function: None,
            decode_arguments: Vec::new(),
            decode_parts: Vec::new(),
            decode_part_parameters: Vec::new(),
        });
        id
    }
}

/// Diagnostic payload produced when an ability proof or constraint check fails.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AbilityFailure {
    pub code: ErrorCode,
    pub message: String,
    pub reason: Option<&'static str>,
}

#[derive(Debug, Clone, Copy, Default)]
struct CacheEntry {
    proofs: [Option<ProofId>; 6],
    constraints: bool,
}

/// Reusable ability-proof cache shared across calls using the same [`AbilityArena`].
#[derive(Debug, Clone, Default)]
pub struct AbilityCache {
    entries: HashMap<TypeId, CacheEntry>,
}

impl AbilityCache {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        self.entries.clear();
    }
}

struct BoundCustomizerPlan {
    function: SymRef,
    arguments: Vec<TypeArgument>,
    reqs: Vec<(TypeId, TypeId, SourceAbility)>,
}

enum DeriveTarget {
    Encode {
        function: SymRef,
        arguments: Vec<TypeArgument>,
        part_parameters: Vec<TypeId>,
        parts: Vec<ProofId>,
    },
    Decode {
        function: SymRef,
        arguments: Vec<TypeArgument>,
        part_parameters: Vec<TypeId>,
        parts: Vec<ProofId>,
        complete_json: bool,
    },
    CustomizerFn {
        function: SymRef,
        arguments: Vec<TypeArgument>,
        part_parameters: Vec<TypeId>,
        parts: Vec<ProofId>,
    },
    Parts {
        parts: Vec<ProofId>,
    },
}

enum SubProofItem {
    Prove(TypeId, SourceAbility, u32),
    Fixed(ProofId),
}

enum ProveFrame {
    CheckEqual {
        ty_id: TypeId,
        depth: u32,
    },
    RunCustomizer {
        proof_id: ProofId,
        ty_id: TypeId,
        ability: SourceAbility,
        depth: u32,
    },
    RunJsonDecode {
        proof_id: ProofId,
        ty_id: TypeId,
        depth: u32,
        from_fn: FuncSigId,
        has_encode: bool,
    },
    RunDerive {
        proof_id: ProofId,
        ty_id: TypeId,
        ability: SourceAbility,
        depth: u32,
    },
    Collect {
        proof_id: ProofId,
        target: DeriveTarget,
        pending: Vec<SubProofItem>,
        next: usize,
    },
}

/// Stateful ability prover and constraint checker (`struct mo_ability_context`).
///
/// Emits no diagnostics directly; failures are recorded in [`Self::error`] so the caller
/// controls source spans and diagnostic ordering.
pub struct AbilityContext<'a> {
    pub arena: AbilityArena,
    pub interfaces: &'a ModuleInterfaces,
    pub signatures: &'a mut Signatures,
    pub work: u32,
    pub cache: AbilityCache,
    pub error: Option<AbilityFailure>,
}

impl<'a> AbilityContext<'a> {
    #[must_use]
    pub fn new(interfaces: &'a ModuleInterfaces, signatures: &'a mut Signatures) -> Self {
        Self::with_arena(AbilityArena::new(), interfaces, signatures, 0)
    }

    #[must_use]
    pub fn with_arena(
        arena: AbilityArena,
        interfaces: &'a ModuleInterfaces,
        signatures: &'a mut Signatures,
        work: u32,
    ) -> Self {
        Self::with_parts(arena, AbilityCache::new(), interfaces, signatures, work)
    }

    #[must_use]
    pub fn with_parts(
        arena: AbilityArena,
        cache: AbilityCache,
        interfaces: &'a ModuleInterfaces,
        signatures: &'a mut Signatures,
        work: u32,
    ) -> Self {
        Self {
            arena,
            interfaces,
            signatures,
            work,
            cache,
            error: None,
        }
    }

    /// Extracts the arena, cache, work counter, and any failure from this context.
    #[must_use]
    pub fn into_parts(self) -> (AbilityArena, AbilityCache, u32, Option<AbilityFailure>) {
        (self.arena, self.cache, self.work, self.error)
    }

    /// Formats `ty_id` into a bounded preview string (`<= 512` bytes), respecting depth/work limits.
    #[must_use]
    pub fn preview_type_string(&mut self, ty_id: TypeId) -> Option<String> {
        let mut preview = TypePreview::new();
        if !self.preview_type(&mut preview, ty_id, 0) {
            return None;
        }
        Some(preview.text)
    }

    /// Discards cached proofs and any recorded error so provisional cyclic edges from a failed
    /// proof cannot leak into later declarations (`program.c:99-104`).
    pub fn reset_cache(&mut self) {
        self.cache.clear();
        self.error = None;
    }

    fn fail(
        &mut self,
        code: ErrorCode,
        message: impl Into<String>,
        reason: Option<&'static str>,
    ) -> bool {
        if self.error.is_none() {
            self.error = Some(AbilityFailure {
                code,
                message: message.into(),
                reason,
            });
        }
        false
    }

    fn charge_work(&mut self, depth: u32) -> bool {
        if self.error.is_some() {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_TYPE_DEPTH {
            return self.fail(
                ErrorCode::CheckTypeDepth,
                "the ability type path is too deep",
                None,
            );
        }
        if u64::from(self.work) == MO_LIMIT_SOURCE_CHECK_WORK {
            return self.fail(
                ErrorCode::CheckWork,
                "the function needs too much ability-checking work",
                None,
            );
        }
        if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            return self.fail(
                ErrorCode::CheckProgramWork,
                "the program needs too much ability-checking work",
                None,
            );
        }
        self.work += 1;
        self.signatures.work += 1;
        true
    }

    fn ensure_cached(&mut self, ty_id: TypeId) -> bool {
        let height = self.signatures.types.get(ty_id).height;
        if !self.charge_work(height) {
            return false;
        }
        if self.cache.entries.contains_key(&ty_id) {
            return true;
        }
        let arg_count = self.signatures.types.get(ty_id).arguments.len();
        for i in 0..arg_count {
            let arg = self.signatures.types.get(ty_id).arguments[i];
            if !self.ensure_cached(arg) {
                return false;
            }
        }
        self.cache.entries.entry(ty_id).or_default();
        true
    }

    fn substitute(
        &mut self,
        ty_id: TypeId,
        generic_id: TypeId,
        actual_id: TypeId,
        depth: u32,
    ) -> Option<TypeId> {
        if !self.charge_work(depth) {
            return None;
        }
        let (params_mask, kind, arg_count) = {
            let ty = self.signatures.types.get(ty_id);
            (ty.parameters, ty.kind, ty.arguments.len())
        };
        if params_mask == 0 {
            return Some(ty_id);
        }
        if kind == SourceTypeKind::Parameter {
            let gen_count = self.signatures.types.get(generic_id).arguments.len();
            for i in 0..gen_count {
                if self.signatures.types.get(generic_id).arguments[i] == ty_id {
                    return Some(self.signatures.types.get(actual_id).arguments[i]);
                }
            }
            return Some(ty_id);
        }
        let mut new_args = Vec::with_capacity(arg_count);
        let mut changed = false;
        let mut max_height = 0u32;
        for i in 0..arg_count {
            let orig = self.signatures.types.get(ty_id).arguments[i];
            let sub = self.substitute(orig, generic_id, actual_id, depth + 1)?;
            changed = changed || sub != orig;
            let child_h = self.signatures.types.get(sub).height.saturating_add(1);
            if child_h > max_height {
                max_height = child_h;
            }
            new_args.push(sub);
        }
        if !changed {
            return Some(ty_id);
        }
        if !self.charge_work(max_height) {
            return None;
        }
        let (name, module, owner) = {
            let ty = self.signatures.types.get(ty_id);
            (ty.name.clone(), ty.module, ty.owner)
        };
        Some(
            self.signatures
                .types
                .intern(kind, name, module, owner, new_args),
        )
    }

    fn preview_type(&mut self, out: &mut TypePreview, ty_id: TypeId, depth: u32) -> bool {
        if out.is_full() {
            return true;
        }
        if !self.charge_work(depth) {
            return false;
        }
        let (kind, name, arg_count) = {
            let ty = self.signatures.types.get(ty_id);
            (ty.kind, ty.name.clone(), ty.arguments.len())
        };
        let tuple = kind == SourceTypeKind::Tuple;
        let function = kind == SourceTypeKind::Function;
        if !tuple {
            out.push(&name);
        }
        if arg_count == 0 {
            return true;
        }
        out.push("(");
        let count = arg_count - usize::from(function);
        for i in 0..count {
            if out.is_full() {
                break;
            }
            if i != 0 {
                out.push(", ");
            }
            let arg = self.signatures.types.get(ty_id).arguments[i];
            if !self.preview_type(out, arg, depth + 1) {
                return false;
            }
        }
        out.push(")");
        if function {
            out.push(" : ");
            let ret = self.signatures.types.get(ty_id).arguments[count];
            if !self.preview_type(out, ret, depth + 1) {
                return false;
            }
        }
        true
    }

    fn missing(&mut self, ty_id: TypeId, ability: SourceAbility) -> bool {
        let mut preview = TypePreview::new();
        if !self.preview_type(&mut preview, ty_id, 0) {
            return false;
        }
        let message = format!("type {} doesn't have {}", preview.as_str(), ability.name());
        self.fail(
            ErrorCode::AbilityMissing,
            message,
            Some(
                "An operation needs its type's ability; functions and capabilities have no \
                 abilities (reference 9.2).",
            ),
        )
    }

    fn parameter_requirements(&mut self, ty_id: TypeId) -> u8 {
        let (letter, owner, param_mask) = {
            let ty = self.signatures.types.get(ty_id);
            let Some(letter) = ty.param_letter() else {
                return 0;
            };
            (letter, ty.owner, ty.parameters)
        };
        match owner {
            TypeOwner::Source(sym) => {
                if let Some((_, fn_sig)) = self.signatures.function_signature_by_symbol(sym) {
                    return fn_sig.requirements[letter];
                }
                if let Some((_, data)) = self.signatures.nominal_signature_by_symbol(sym)
                    && (data.key_parameters & param_mask) != 0
                {
                    return SourceAbility::Hash.bit() | SourceAbility::Equal.bit();
                }
                0
            }
            TypeOwner::Native(native_id) => {
                if !self.charge_work(0) {
                    return 0;
                }
                self.signatures.native[native_id.index()].requirements[letter]
            }
            TypeOwner::None | TypeOwner::BuiltinOption | TypeOwner::BuiltinResult => 0,
        }
    }

    fn plan_customizer_fn(
        &self,
        ty_id: TypeId,
        fn_id: FuncSigId,
        receiver_id: TypeId,
    ) -> Option<BoundCustomizerPlan> {
        let fn_sym = self.signatures.functions[fn_id.index()].function;
        let fn_reqs = self.signatures.functions[fn_id.index()].requirements;
        let arg_count = self.signatures.types.get(ty_id).arguments.len();
        debug_assert!(arg_count <= 26);

        let mut arguments: Vec<TypeArgument> = Vec::with_capacity(arg_count);
        for j in 0..arg_count {
            let param_id = self.signatures.types.get(receiver_id).arguments[j];
            let actual_id = self.signatures.types.get(ty_id).arguments[j];
            arguments.push(TypeArgument {
                parameter: param_id,
                ty: actual_id,
            });
        }
        arguments.sort_by_key(|arg| {
            self.signatures
                .types
                .get(arg.parameter)
                .name
                .as_bytes()
                .first()
                .copied()
                .unwrap_or(0)
        });

        let mut reqs = Vec::new();
        for arg in &arguments {
            let letter = self.signatures.types.get(arg.parameter).param_letter()?;
            for ab in SourceAbility::ALL {
                if (fn_reqs[letter] & ab.bit()) == 0 {
                    continue;
                }
                reqs.push((arg.parameter, arg.ty, ab));
            }
        }
        Some(BoundCustomizerPlan {
            function: fn_sym,
            arguments,
            reqs,
        })
    }

    fn plan_derive_data(
        &mut self,
        ty_id: TypeId,
        ability: SourceAbility,
        depth: u32,
    ) -> Option<Vec<SubProofItem>> {
        let owner_sym = self.signatures.types.get(ty_id).owner.as_source()?;
        let Some((invalid, field_count)) = self
            .signatures
            .nominal_signature_by_symbol(owner_sym)
            .map(|(_, n)| (n.invalid, n.field_count))
        else {
            debug_assert!(self.signatures.recovering);
            return Some(Vec::new());
        };
        if invalid {
            debug_assert!(self.signatures.recovering);
            return Some(Vec::new());
        }
        let owner = self.interfaces.symbol(owner_sym);
        let is_enum = owner.decl_kind == SyntaxKind::Enum;
        let cases = if is_enum { owner.cases.len() } else { 1 };

        if ability == SourceAbility::Json && is_enum {
            let mut tags: Vec<String> = Vec::with_capacity(cases);
            for i in 0..cases {
                let case_sym_ref = SymRef::new(owner.module, owner.cases[i]);
                let tag = snake_case_name(&self.interfaces.symbol(case_sym_ref).name);
                for existing in &tags {
                    if !self.charge_work(0) {
                        return None;
                    }
                    if existing == &tag {
                        let _ = self.missing(ty_id, ability);
                        return None;
                    }
                }
                tags.push(tag);
            }
        }

        let mut items = Vec::with_capacity(field_count as usize);
        for i in 0..cases {
            let case_sym_ref = if is_enum {
                SymRef::new(owner.module, owner.cases[i])
            } else {
                owner_sym
            };
            if !self.charge_work(0) {
                return None;
            }
            let Some((data_id, _)) = self.signatures.data_signature_by_symbol(case_sym_ref) else {
                debug_assert!(self.signatures.recovering);
                return Some(items);
            };
            let (data_ty, data_field_count) = {
                let d = &self.signatures.data[data_id.index()];
                (d.ty, d.fields.len())
            };
            for k in 0..data_field_count {
                let field_ty = self.signatures.data[data_id.index()].field_by_index(k).ty;
                let sub_ty = self.substitute(field_ty, data_ty, ty_id, 0)?;
                items.push(SubProofItem::Prove(sub_ty, ability, depth + 1));
            }
        }
        debug_assert_eq!(items.len(), field_count as usize);
        Some(items)
    }

    fn plan_derive_native(
        &mut self,
        ty_id: TypeId,
        ability: SourceAbility,
        depth: u32,
    ) -> Option<Vec<SubProofItem>> {
        let ty_name = self.signatures.types.get(ty_id).name.clone();
        let first = builtin_case_by_type(&ty_name)?;

        let text_ty = self.signatures.types.intern_builtin("Text");
        let kind_ty = self.signatures.types.intern_builtin("EntryKind");
        let int_ty = self.signatures.types.intern_builtin("Int");
        let float_ty = self.signatures.types.intern_builtin("Float");
        let bytes_ty = self.signatures.types.intern_builtin("Bytes");
        let sqlite_val_ty = self.signatures.types.intern_builtin("Sqlite.Value");
        let list_text_ty = self
            .signatures
            .types
            .intern_builtin_applied("List", vec![text_ty]);
        let list_val_ty = self
            .signatures
            .types
            .intern_builtin_applied("List", vec![sqlite_val_ty]);
        let list_rows_ty = self
            .signatures
            .types
            .intern_builtin_applied("List", vec![list_val_ty]);

        let mut items = Vec::new();
        for i in 0..first.cases {
            let data = builtin_case_by_key(first.first_key + i)?;
            for j in 0..data.count {
                if ty_name == "Sqlite.Value"
                    && data.name == "Blob"
                    && ability == SourceAbility::Text
                {
                    let blob_text = self.arena.alloc(SourceAbility::Text, bytes_ty);
                    items.push(SubProofItem::Fixed(blob_text));
                    continue;
                }
                let field_ty = if ty_name == "Sqlite.Value" {
                    match data.name {
                        "Integer" => int_ty,
                        "Real" => float_ty,
                        "String" => text_ty,
                        _ => bytes_ty,
                    }
                } else if ty_name == "Sqlite.Rows" {
                    if j == 0 { list_text_ty } else { list_rows_ty }
                } else if j == 0 {
                    text_ty
                } else if ty_name == "Entry" {
                    kind_ty
                } else if data.name == "Refused" {
                    int_ty
                } else {
                    text_ty
                };
                items.push(SubProofItem::Prove(field_ty, ability, depth + 1));
            }
        }
        Some(items)
    }

    fn start_prove(
        &mut self,
        ty_id: TypeId,
        ability: SourceAbility,
        depth: u32,
        stack: &mut Vec<ProveFrame>,
    ) -> Option<ProofId> {
        if !self.charge_work(0) {
            return None;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_ABILITY_DEPTH {
            let _ = self.fail(
                ErrorCode::CheckAbilityDepth,
                "the ability dependency path is too deep",
                None,
            );
            return None;
        }
        if !self.ensure_cached(ty_id) {
            return None;
        }
        if let Some(existing) = self
            .cache
            .entries
            .get(&ty_id)
            .and_then(|e| e.proofs[ability.index()])
        {
            return Some(existing);
        }

        let proof_id = self.arena.alloc(ability, ty_id);
        let ty_kind = self.signatures.types.get(ty_id).kind;
        if ty_kind == SourceTypeKind::Parameter {
            let mut mask = self.parameter_requirements(ty_id);
            if self.error.is_some() {
                return None;
            }
            if ability == SourceAbility::Equal
                && (mask & (SourceAbility::Hash.bit() | SourceAbility::Order.bit())) != 0
            {
                mask |= SourceAbility::Equal.bit();
            }
            if (mask & ability.bit()) == 0 {
                let _ = self.missing(ty_id, ability);
                return None;
            }
            self.arena.get_mut(proof_id).kind = AbilityImplementationKind::Parameter;
            self.cache.entries.entry(ty_id).or_default().proofs[ability.index()] = Some(proof_id);
            return Some(proof_id);
        }

        let denied = {
            let ty = self.signatures.types.get(ty_id);
            let is_actor = ty
                .owner
                .as_source()
                .is_some_and(|sym| self.interfaces.symbol(sym).decl_kind == SyntaxKind::Actor);
            let numeric = ty.is_builtin_named("Int")
                || ty.is_builtin_named("BigInt")
                || ty.is_builtin_named("Decimal")
                || ty.is_builtin_named("Float");
            let is_cap = ty.kind == SourceTypeKind::Named
                && ty.module.is_none()
                && is_capability_name(&ty.name);
            ty.kind == SourceTypeKind::Function
                || is_actor
                || is_cap
                || (ability == SourceAbility::Number && !numeric)
                || (ability == SourceAbility::Hash && ty.is_builtin_named("Float"))
                || (ability == SourceAbility::Text && ty.is_builtin_named("Bytes"))
                || (ability == SourceAbility::Order
                    && (ty.is_builtin_named("Range")
                        || ty.is_builtin_named("Map")
                        || ty.is_builtin_named("Set")))
                || (ability == SourceAbility::Json
                    && (ty.is_builtin_named("Bytes")
                        || ty.is_builtin_named("Range")
                        || ty.is_builtin_named("Duration")
                        || (ty.is_builtin_named("Map")
                            && !self
                                .signatures
                                .types
                                .get(ty.arguments[0])
                                .is_builtin_named("Text"))))
        };
        if denied {
            let _ = self.missing(ty_id, ability);
            return None;
        }

        // Publish before descending: recursive nominal proofs form a shared graph.
        self.cache.entries.entry(ty_id).or_default().proofs[ability.index()] = Some(proof_id);

        // Json.Json and Json.JsonNumber have native Json ability.
        if ability == SourceAbility::Json {
            let ty = self.signatures.types.get(ty_id);
            if ty.owner.as_source().is_some()
                && let Some(mod_id) = ty.module
            {
                let scope = self.interfaces.scope(mod_id);
                if scope.library
                    && scope.name.as_deref() == Some("Json")
                    && (ty.name == "Json" || ty.name == "JsonNumber")
                {
                    return Some(proof_id);
                }
            }
        }

        stack.push(ProveFrame::RunDerive {
            proof_id,
            ty_id,
            ability,
            depth,
        });
        stack.push(ProveFrame::RunCustomizer {
            proof_id,
            ty_id,
            ability,
            depth,
        });
        if matches!(ability, SourceAbility::Hash | SourceAbility::Order) {
            stack.push(ProveFrame::CheckEqual { ty_id, depth });
        }
        Some(proof_id)
    }

    fn prove_inner(
        &mut self,
        root_ty: TypeId,
        root_ability: SourceAbility,
        root_depth: u32,
    ) -> Option<ProofId> {
        let mut stack: Vec<ProveFrame> = Vec::new();
        let root_proof = self.start_prove(root_ty, root_ability, root_depth, &mut stack)?;

        while let Some(frame) = stack.pop() {
            match frame {
                ProveFrame::CheckEqual { ty_id, depth } => {
                    let _ = self.start_prove(ty_id, SourceAbility::Equal, depth, &mut stack)?;
                }
                ProveFrame::RunCustomizer {
                    proof_id,
                    ty_id,
                    ability,
                    depth,
                } => {
                    let has_decl = self.signatures.types.get(ty_id).owner.as_source().is_some();
                    if ability == SourceAbility::Json {
                        if !has_decl {
                            continue;
                        }
                        let to_fn = self
                            .signatures
                            .customizer_signature_by_name(self.interfaces, ty_id, "to_json")
                            .map(|(id, _)| id);
                        let from_fn = self
                            .signatures
                            .customizer_signature_by_name(self.interfaces, ty_id, "from_json")
                            .map(|(id, _)| id);
                        if let Some(f_id) = from_fn {
                            stack.push(ProveFrame::RunJsonDecode {
                                proof_id,
                                ty_id,
                                depth,
                                from_fn: f_id,
                                has_encode: to_fn.is_some(),
                            });
                        }
                        if let Some(t_id) = to_fn {
                            let fn_ty = self.signatures.functions[t_id.index()].ty?;
                            let receiver_id = self.signatures.types.get(fn_ty).arguments[0];
                            let plan = self.plan_customizer_fn(ty_id, t_id, receiver_id)?;
                            let pending: Vec<SubProofItem> = plan
                                .reqs
                                .iter()
                                .map(|&(_, arg_ty, ab)| SubProofItem::Prove(arg_ty, ab, depth + 1))
                                .collect();
                            let part_parameters: Vec<TypeId> =
                                plan.reqs.iter().map(|&(p, _, _)| p).collect();
                            stack.push(ProveFrame::Collect {
                                proof_id,
                                target: DeriveTarget::Encode {
                                    function: plan.function,
                                    arguments: plan.arguments,
                                    part_parameters,
                                    parts: Vec::with_capacity(pending.len()),
                                },
                                pending,
                                next: 0,
                            });
                        }
                        continue;
                    }
                    let file_error = self
                        .signatures
                        .types
                        .get(ty_id)
                        .is_builtin_named("FileError")
                        && ability == SourceAbility::Text;
                    if (!has_decl && !file_error)
                        || (ability != SourceAbility::Text && ability != SourceAbility::Order)
                    {
                        continue;
                    }
                    if let Some((fn_id, _)) =
                        self.signatures
                            .customizer_signature(self.interfaces, ty_id, ability)
                    {
                        let fn_ty = self.signatures.functions[fn_id.index()].ty?;
                        let receiver_id = self.signatures.types.get(fn_ty).arguments[0];
                        let plan = self.plan_customizer_fn(ty_id, fn_id, receiver_id)?;
                        let pending: Vec<SubProofItem> = plan
                            .reqs
                            .iter()
                            .map(|&(_, arg_ty, ab)| SubProofItem::Prove(arg_ty, ab, depth + 1))
                            .collect();
                        let part_parameters: Vec<TypeId> =
                            plan.reqs.iter().map(|&(p, _, _)| p).collect();
                        stack.push(ProveFrame::Collect {
                            proof_id,
                            target: DeriveTarget::CustomizerFn {
                                function: plan.function,
                                arguments: plan.arguments,
                                part_parameters,
                                parts: Vec::with_capacity(pending.len()),
                            },
                            pending,
                            next: 0,
                        });
                    }
                }
                ProveFrame::RunJsonDecode {
                    proof_id,
                    ty_id,
                    depth,
                    from_fn,
                    has_encode,
                } => {
                    let fn_ty = self.signatures.functions[from_fn.index()].ty?;
                    let &ret_id = self.signatures.types.get(fn_ty).arguments.last()?;
                    let receiver_id = self.signatures.types.get(ret_id).arguments[0];
                    let plan = self.plan_customizer_fn(ty_id, from_fn, receiver_id)?;
                    let pending: Vec<SubProofItem> = plan
                        .reqs
                        .iter()
                        .map(|&(_, arg_ty, ab)| SubProofItem::Prove(arg_ty, ab, depth + 1))
                        .collect();
                    let part_parameters: Vec<TypeId> =
                        plan.reqs.iter().map(|&(p, _, _)| p).collect();
                    stack.push(ProveFrame::Collect {
                        proof_id,
                        target: DeriveTarget::Decode {
                            function: plan.function,
                            arguments: plan.arguments,
                            part_parameters,
                            parts: Vec::with_capacity(pending.len()),
                            complete_json: has_encode,
                        },
                        pending,
                        next: 0,
                    });
                }
                ProveFrame::RunDerive {
                    proof_id,
                    ty_id,
                    ability,
                    depth,
                } => {
                    if self.arena.get(proof_id).kind == AbilityImplementationKind::Function {
                        continue;
                    }
                    let has_decl = self.signatures.types.get(ty_id).owner.as_source().is_some();
                    if has_decl {
                        self.arena.get_mut(proof_id).kind = AbilityImplementationKind::Derived;
                        let pending = self.plan_derive_data(ty_id, ability, depth)?;
                        stack.push(ProveFrame::Collect {
                            proof_id,
                            target: DeriveTarget::Parts {
                                parts: Vec::with_capacity(pending.len()),
                            },
                            pending,
                            next: 0,
                        });
                        continue;
                    }
                    let is_derived_native = {
                        let ty = self.signatures.types.get(ty_id);
                        ty.kind == SourceTypeKind::Named
                            && ty.module.is_none()
                            && matches!(
                                ty.name.as_str(),
                                "Entry"
                                    | "FileError"
                                    | "Ordering"
                                    | "EntryKind"
                                    | "SimulationError"
                                    | "NetworkError"
                                    | "SqliteError"
                                    | "Sqlite.Value"
                                    | "Sqlite.Rows"
                            )
                    };
                    if is_derived_native {
                        self.arena.get_mut(proof_id).kind = AbilityImplementationKind::Derived;
                        let pending = self.plan_derive_native(ty_id, ability, depth)?;
                        stack.push(ProveFrame::Collect {
                            proof_id,
                            target: DeriveTarget::Parts {
                                parts: Vec::with_capacity(pending.len()),
                            },
                            pending,
                            next: 0,
                        });
                        continue;
                    }
                    let (kind, arg_count) = {
                        let ty = self.signatures.types.get(ty_id);
                        (ty.kind, ty.arguments.len())
                    };
                    if kind == SourceTypeKind::Tuple || arg_count != 0 {
                        self.arena.get_mut(proof_id).kind = AbilityImplementationKind::Derived;
                        let mut pending = Vec::with_capacity(arg_count);
                        for i in 0..arg_count {
                            let arg = self.signatures.types.get(ty_id).arguments[i];
                            pending.push(SubProofItem::Prove(arg, ability, depth + 1));
                        }
                        stack.push(ProveFrame::Collect {
                            proof_id,
                            target: DeriveTarget::Parts {
                                parts: Vec::with_capacity(arg_count),
                            },
                            pending,
                            next: 0,
                        });
                    }
                }
                ProveFrame::Collect {
                    proof_id,
                    mut target,
                    pending,
                    next,
                } => {
                    if next < pending.len() {
                        let item = match pending[next] {
                            SubProofItem::Prove(sub_ty, sub_ab, sub_depth) => {
                                let before_len = stack.len();
                                let sub_id =
                                    self.start_prove(sub_ty, sub_ab, sub_depth, &mut stack)?;
                                // Reorder: push Collect underneath newly pushed child work so
                                // child work completes before the next sibling is started.
                                let child_frames: Vec<ProveFrame> =
                                    stack.drain(before_len..).collect();
                                match &mut target {
                                    DeriveTarget::Encode { parts, .. }
                                    | DeriveTarget::Decode { parts, .. }
                                    | DeriveTarget::CustomizerFn { parts, .. }
                                    | DeriveTarget::Parts { parts } => parts.push(sub_id),
                                }
                                stack.push(ProveFrame::Collect {
                                    proof_id,
                                    target,
                                    pending,
                                    next: next + 1,
                                });
                                stack.extend(child_frames);
                                continue;
                            }
                            SubProofItem::Fixed(sub_id) => sub_id,
                        };
                        match &mut target {
                            DeriveTarget::Encode { parts, .. }
                            | DeriveTarget::Decode { parts, .. }
                            | DeriveTarget::CustomizerFn { parts, .. }
                            | DeriveTarget::Parts { parts } => parts.push(item),
                        }
                        stack.push(ProveFrame::Collect {
                            proof_id,
                            target,
                            pending,
                            next: next + 1,
                        });
                    } else {
                        let p = self.arena.get_mut(proof_id);
                        match target {
                            DeriveTarget::Encode {
                                function,
                                arguments,
                                part_parameters,
                                parts,
                            } => {
                                p.encode_function = Some(function);
                                p.encode_arguments = arguments;
                                p.encode_parts = parts;
                                p.encode_part_parameters = part_parameters;
                            }
                            DeriveTarget::Decode {
                                function,
                                arguments,
                                part_parameters,
                                parts,
                                complete_json,
                            } => {
                                p.decode_function = Some(function);
                                p.decode_arguments = arguments;
                                p.decode_parts = parts;
                                p.decode_part_parameters = part_parameters;
                                if complete_json {
                                    p.kind = AbilityImplementationKind::Function;
                                }
                            }
                            DeriveTarget::CustomizerFn {
                                function,
                                arguments,
                                part_parameters,
                                parts,
                            } => {
                                p.kind = AbilityImplementationKind::Function;
                                p.function = Some(function);
                                p.arguments = arguments;
                                p.parts = parts;
                                p.part_parameters = part_parameters;
                            }
                            DeriveTarget::Parts { parts } => {
                                p.parts = parts;
                            }
                        }
                    }
                }
            }
        }
        Some(root_proof)
    }

    /// Proves that `ty` satisfies `ability` (`mo_prove_ability`).
    #[must_use]
    pub fn prove_ability(&mut self, ty: TypeId, ability: SourceAbility) -> Option<ProofId> {
        self.prove_inner(ty, ability, 0)
    }

    fn check_constraints(&mut self, ty_id: TypeId, depth: u32) -> bool {
        if !self.charge_work(depth) {
            return false;
        }
        if !self.ensure_cached(ty_id) {
            return false;
        }
        if self
            .cache
            .entries
            .get(&ty_id)
            .is_some_and(|e| e.constraints)
        {
            return true;
        }
        self.cache.entries.entry(ty_id).or_default().constraints = true;

        let is_map_or_set = {
            let ty = self.signatures.types.get(ty_id);
            ty.is_builtin_named("Map") || ty.is_builtin_named("Set")
        };
        if is_map_or_set {
            let key_ty = self.signatures.types.get(ty_id).arguments[0];
            if self.prove_inner(key_ty, SourceAbility::Hash, 0).is_none() {
                return false;
            }
        }
        let arg_count = self.signatures.types.get(ty_id).arguments.len();
        for i in 0..arg_count {
            let arg = self.signatures.types.get(ty_id).arguments[i];
            if !self.check_constraints(arg, depth + 1) {
                return false;
            }
        }
        let nom_sym = {
            let ty = self.signatures.types.get(ty_id);
            if ty.kind == SourceTypeKind::Named {
                ty.owner.as_source()
            } else {
                None
            }
        };
        if let Some(sym) = nom_sym {
            let nom_info = self
                .signatures
                .nominal_signature_by_symbol(sym)
                .map(|(_, data)| (data.key_parameters, data.ty));
            let Some((key_params, nom_ty)) = nom_info else {
                debug_assert!(self.signatures.recovering);
                return true;
            };
            for j in 0..arg_count {
                let param_mask = self
                    .signatures
                    .types
                    .get(self.signatures.types.get(nom_ty).arguments[j])
                    .parameters;
                let arg_ty = self.signatures.types.get(ty_id).arguments[j];
                if (key_params & param_mask) != 0
                    && self.prove_inner(arg_ty, SourceAbility::Hash, 0).is_none()
                {
                    return false;
                }
            }
        }
        true
    }

    /// Checks all collection-key (`Map`/`Set` `Hash`) and nominal key-parameter constraints
    /// reachable from `ty` (`mo_check_type_abilities`).
    #[must_use]
    pub fn check_type_abilities(&mut self, ty: TypeId) -> bool {
        self.check_constraints(ty, 0)
    }
}

struct TypePreview {
    text: String,
}

impl TypePreview {
    const CAP: usize = 512;

    fn new() -> Self {
        Self {
            text: String::new(),
        }
    }

    fn is_full(&self) -> bool {
        self.text.len() + 4 >= Self::CAP
    }

    fn push(&mut self, s: &str) {
        let mut bytes = s.as_bytes();
        while !bytes.is_empty() && self.text.len() + 4 < Self::CAP {
            let b = bytes[0];
            self.text.push(char::from(b));
            bytes = &bytes[1..];
        }
        if !bytes.is_empty() && self.text.len() + 4 <= Self::CAP {
            self.text.push_str("...");
        }
    }

    fn as_str(&self) -> &str {
        &self.text
    }
}

/// Converts a PascalCase enum case name to its `snake_case` JSON tag
/// (`src/check/abilities.c:snake_case_name`).
#[must_use]
pub fn snake_case_name(name: &str) -> String {
    let bytes = name.as_bytes();
    let mut out = String::with_capacity(bytes.len() + 4);
    for (i, &c) in bytes.iter().enumerate() {
        let cap = c.is_ascii_uppercase();
        let word = i != 0
            && cap
            && (bytes[i - 1].is_ascii_lowercase()
                || bytes[i - 1].is_ascii_digit()
                || (i + 1 < bytes.len() && bytes[i + 1].is_ascii_lowercase()));
        if word {
            out.push('_');
        }
        out.push(char::from(if cap { c + (b'a' - b'A') } else { c }));
    }
    out
}

fn resource_failed(diags: &DiagBag) -> bool {
    if diags.dropped() != 0 {
        return true;
    }
    diags
        .items()
        .iter()
        .any(|d| d.code.number() >= 1000 && d.code != ErrorCode::CheckUnfinished)
}

fn check_type_ability_at(
    ctx: &mut AbilityContext<'_>,
    ty: TypeId,
    span: Span,
    diags: &mut DiagBag,
) -> bool {
    if !ctx.signatures.signature_type_available(Some(ty)) || ctx.check_type_abilities(ty) {
        return true;
    }
    let Some(failure) = ctx.error.take() else {
        return false;
    };
    let mut diag = Diagnostic::new(failure.code, span, failure.message.clone())
        .with_primary_text(failure.message);
    if let Some(reason) = failure.reason {
        diag = diag.with_reason(reason);
    }
    let kept = diags.push(diag).unwrap_or(false);
    ctx.reset_cache();
    kept && !resource_failed(diags)
}

/// Runs signature-level ability checks over every function signature and data/field signature
/// (`src/check/program.c:360-402`), updating invalid functions and customizers in place.
///
/// Returns `false` if a resource limit or diagnostic cap stopped checking early.
#[must_use]
pub fn check_signature_abilities(
    set: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    diags: &mut DiagBag,
) -> bool {
    let mut abilities = AbilityContext::new(interfaces, signatures);

    let fn_count = abilities.signatures.functions.len();
    for i in 0..fn_count {
        abilities.work = 0;
        let fn_ty_opt = abilities.signatures.functions[i].ty;
        if !abilities.signatures.signature_type_available(fn_ty_opt) {
            abilities.signatures.functions[i].ty = None;
            continue;
        }
        let Some(fn_ty) = fn_ty_opt else {
            continue;
        };
        let fn_sym = abilities.signatures.functions[i].function;
        let sym = interfaces.symbol(fn_sym);
        let tree = &set.module(sym.module).syntax.tree;
        let decl = tree.node(sym.declaration);
        let Some(params_nid) = decl.children.get(1).copied().flatten() else {
            continue;
        };
        let param_children = &tree.node(params_nid).children;
        let ret_ann = decl.children.get(2).copied().flatten();
        let name_span = sym.name_span;

        let arg_count = abilities.signatures.types.get(fn_ty).arguments.len();
        for j in 0..arg_count {
            let arg_ty = abilities.signatures.types.get(fn_ty).arguments[j];
            let ann_nid = if j < param_children.len() {
                param_children[j].and_then(|p| tree.node(p).children.get(1).copied().flatten())
            } else {
                ret_ann
            };
            let span = ann_nid.map_or(name_span, |n| tree.node(n).span);
            let before = diags.len();
            if !check_type_ability_at(&mut abilities, arg_ty, span, diags) {
                diags.sort();
                return false;
            }
            if diags.len() != before {
                abilities.signatures.functions[i].ty = None;
                abilities.signatures.recovering = true;
                break;
            }
        }
    }

    // Filter customizers whose function signatures failed signature-level ability checks.
    let kept_customizers: Vec<FuncSigId> = abilities
        .signatures
        .customizers
        .iter()
        .copied()
        .filter(|id| abilities.signatures.functions[id.index()].ty.is_some())
        .collect();
    abilities.signatures.customizers = kept_customizers;

    let data_count = abilities.signatures.data.len();
    for i in 0..data_count {
        abilities.work = 0;
        let (data_ty, data_sym, field_count) = {
            let d = &abilities.signatures.data[i];
            (d.ty, d.symbol, d.fields.len())
        };
        let name_span = interfaces.symbol(data_sym).name_span;
        if !check_type_ability_at(&mut abilities, data_ty, name_span, diags) {
            diags.sort();
            return false;
        }
        for j in 0..field_count {
            let (field_ty, field_span) = {
                let field = abilities.signatures.data[i].field_by_index(j);
                (field.ty, field.annotation_span)
            };
            if !check_type_ability_at(&mut abilities, field_ty, field_span, diags) {
                diags.sort();
                return false;
            }
        }
    }

    diags.sort();
    true
}
