//! Signature preparation, shared type DAG resolution, nonregular generic data detection
//! (`E0084`), native signature loading from `mo-def`, and parameter-dependency masks
//! (`src/check/signatures.c` + `src/check/recursion.c::mo_check_data_recursion` parity).

use std::collections::VecDeque;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::limits::{MO_LIMIT_NESTING_DEPTH, MO_LIMIT_PARAMETERS, MO_LIMIT_PROGRAM_CHECK_WORK};
use mo_def::{ErrorCode, HOSTOPS, PRIMS};
use mo_syntax::{DURATION_UNITS, NodeId, SyntaxKind, SyntaxTree, TokenKind, parse_module};

use crate::abilities::SourceAbility;
use crate::ids::{DataSigId, FuncSigId, ModuleId, NativeSigId, NominalId, SymRef, TypeId};
use crate::load::ModuleSet;
use crate::modules::{ModuleInterfaces, ModuleScope};
use crate::types::{
    SourceType, SourceTypeKind, TypeArena, TypeOwner, builtin_type_arity, is_capability_name,
};

/// Usage tracking entry for one `use` line or explicitly listed imported name
/// (`struct mo_import_usage`).
#[derive(Debug, Clone)]
pub struct ImportUsage {
    pub origin: NodeId,
    pub origin_span: Span,
    pub module: ModuleId,
    /// Index of the owning `use` line entry inside [`Signatures::imports`].
    pub parent: usize,
    pub used: bool,
}

/// Prepared field signature of a `struct` or `enum` case (`struct mo_field_signature`).
#[derive(Debug, Clone)]
pub struct FieldSignature {
    /// Field name, or `None` for an unnamed single-payload choice case.
    pub name: Option<String>,
    pub name_node: Option<NodeId>,
    pub name_span: Option<Span>,
    pub annotation: NodeId,
    pub annotation_span: Span,
    pub ty: TypeId,
    /// Zero-based declaration order index.
    pub index: u32,
}

/// Prepared signature of one `struct` or `enum` case (`struct mo_data_signature`).
#[derive(Debug, Clone)]
pub struct DataSignature {
    pub symbol: SymRef,
    pub ty: TypeId,
    pub parameters: Option<NodeId>,
    /// Fields sorted by `(name, index)` for binary search (or single unnamed payload).
    pub fields: Vec<FieldSignature>,
    /// Maps declaration index `0..fields.len()` to the sorted position inside `fields`.
    pub by_index: Vec<usize>,
    pub hash_parameters: u32,
    pub key_parameters: u32,
    pub unnamed: bool,
}

impl DataSignature {
    #[must_use]
    pub fn field_by_index(&self, index: usize) -> &FieldSignature {
        &self.fields[self.by_index[index]]
    }

    #[must_use]
    pub fn find_field(&self, name: &[u8]) -> Option<&FieldSignature> {
        if self.unnamed {
            return None;
        }
        let mut lo = 0usize;
        let mut hi = self.fields.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let field_name = self.fields[mid].name.as_deref().unwrap_or("").as_bytes();
            if field_name < name {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.fields.len()
            && self.fields[lo].name.as_deref().map(str::as_bytes) == Some(name)
        {
            Some(&self.fields[lo])
        } else {
            None
        }
    }
}

/// Prepared aggregate metadata for one nominal type (`struct`, `enum`, or `actor`)
/// (`struct mo_nominal_signature`).
#[derive(Debug, Clone)]
pub struct NominalSignature {
    pub symbol: SymRef,
    pub ty: TypeId,
    pub field_count: u32,
    pub hash_parameters: u32,
    pub key_parameters: u32,
    /// Parameters stored in values, excluding phantom and function types.
    pub capture_parameters: u32,
    pub case_count: u32,
    pub holds_capability: bool,
    pub holds_function: bool,
    pub invalid: bool,
}

/// Prepared signature of one top-level source function (`struct mo_function_signature`).
#[derive(Debug, Clone)]
pub struct FunctionSignature {
    pub function: SymRef,
    /// Function type (`SourceTypeKind::Function`), or `None` when recovery discarded an invalid signature.
    pub ty: Option<TypeId>,
    /// Per-parameter (`'A'..='Z'`) required ability bitmask.
    pub requirements: [u8; 26],
}

/// Prepared signature of one native primitive or host capability operation
/// (`struct mo_native_signature`).
#[derive(Debug, Clone)]
pub struct NativeSignature {
    pub module: String,
    pub name: String,
    pub qualified: String,
    pub ty: TypeId,
    pub requirements: [u8; 26],
    pub spec_only: bool,
}

/// All prepared signatures and interned types for a program (`struct mo_signatures`).
#[derive(Debug, Clone)]
pub struct Signatures {
    pub types: TypeArena,
    /// Sorted by `function` (`SymRef`).
    pub functions: Vec<FunctionSignature>,
    /// Sorted by `symbol` (`SymRef`).
    pub data: Vec<DataSignature>,
    /// Sorted by `symbol` (`SymRef`).
    pub nominals: Vec<NominalSignature>,
    /// Sorted by `(customizer_target_owner, function_name)`.
    pub customizers: Vec<FuncSigId>,
    /// All native signatures (`PRIMS`, `HOSTOPS`, `Sqlite.in_memory`, singular duration aliases).
    pub native: Vec<NativeSignature>,
    /// Sorted by `(module, origin_span.start)`.
    pub imports: Vec<ImportUsage>,
    /// Shared program-wide check work counter (`MO_LIMIT_PROGRAM_CHECK_WORK`).
    pub work: u64,
    /// True when one or more declarations failed and checking is in diagnostic recovery mode.
    pub recovering: bool,
}

impl Signatures {
    /// Credits the import route selected by name/type lookup (`mo_use_import`).
    /// Returns `false` when the shared program work budget is exhausted.
    #[must_use]
    pub fn use_import(&mut self, module: ModuleId, origin: NodeId, origin_span: Span) -> bool {
        let mut lo = 0usize;
        let mut hi = self.imports.len();
        while lo < hi {
            if self.work == MO_LIMIT_PROGRAM_CHECK_WORK {
                return false;
            }
            self.work += 1;
            let mid = lo + (hi - lo) / 2;
            let at = &self.imports[mid];
            if at.module < module
                || (at.module == module && at.origin_span.start < origin_span.start)
            {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        debug_assert!(lo < self.imports.len() && self.imports[lo].origin == origin);
        if lo < self.imports.len() && self.imports[lo].origin == origin {
            self.imports[lo].used = true;
            let parent = self.imports[lo].parent;
            self.imports[parent].used = true;
        }
        true
    }

    #[must_use]
    pub fn data_signature_by_symbol(&self, symbol: SymRef) -> Option<(DataSigId, &DataSignature)> {
        let mut lo = 0usize;
        let mut hi = self.data.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.data[mid].symbol < symbol {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.data.len() && self.data[lo].symbol == symbol {
            Some((DataSigId(lo as u32), &self.data[lo]))
        } else {
            None
        }
    }

    #[must_use]
    pub fn function_signature_by_symbol(
        &self,
        symbol: SymRef,
    ) -> Option<(FuncSigId, &FunctionSignature)> {
        let mut lo = 0usize;
        let mut hi = self.functions.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.functions[mid].function < symbol {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.functions.len() && self.functions[lo].function == symbol {
            Some((FuncSigId(lo as u32), &self.functions[lo]))
        } else {
            None
        }
    }

    #[must_use]
    pub fn nominal_signature_by_symbol(
        &self,
        symbol: SymRef,
    ) -> Option<(NominalId, &NominalSignature)> {
        let mut lo = 0usize;
        let mut hi = self.nominals.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.nominals[mid].symbol < symbol {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.nominals.len() && self.nominals[lo].symbol == symbol {
            Some((NominalId(lo as u32), &self.nominals[lo]))
        } else {
            None
        }
    }

    /// Returns whether `ty` is present and references no invalid nominal types
    /// (`mo_signature_type_available`).
    #[must_use]
    pub fn signature_type_available(&mut self, ty: Option<TypeId>) -> bool {
        let Some(ty) = ty else {
            return false;
        };
        if !self.recovering {
            return true;
        }
        self.type_available_inner(ty)
    }

    fn type_available_inner(&mut self, ty: TypeId) -> bool {
        if self.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            return false;
        }
        self.work += 1;
        let node = self.types.get(ty);
        if node.kind == SourceTypeKind::Named
            && let TypeOwner::Source(sym) = node.owner
        {
            match self.nominal_signature_by_symbol(sym) {
                Some((_, owner)) if !owner.invalid => {}
                _ => return false,
            }
        }
        let arg_count = node.arguments.len();
        for i in 0..arg_count {
            let arg = self.types.get(ty).arguments[i];
            if !self.type_available_inner(arg) {
                return false;
            }
        }
        true
    }

    /// Returns the nominal target type of a validated customizer function (`customizer_target`).
    #[must_use]
    pub fn customizer_target(
        &self,
        interfaces: &ModuleInterfaces,
        fn_id: FuncSigId,
    ) -> Option<TypeId> {
        let sig = &self.functions[fn_id.index()];
        let ty_id = sig.ty?;
        let fn_ty = self.types.get(ty_id);
        let sym = interfaces.symbol(sig.function);
        if sym.name == "from_json" {
            let result_id = *fn_ty.arguments.last()?;
            let result = self.types.get(result_id);
            debug_assert_eq!(result.arguments.len(), 2);
            result.arguments.first().copied()
        } else {
            fn_ty.arguments.first().copied()
        }
    }

    fn customizer_target_owner(
        &self,
        interfaces: &ModuleInterfaces,
        fn_id: FuncSigId,
    ) -> TypeOwner {
        self.customizer_target(interfaces, fn_id)
            .map_or(TypeOwner::None, |id| self.types.get(id).owner)
    }

    /// Binary search for a customizer function by receiver type and customizer name
    /// (`mo_customizer_signature_by_name`).
    #[must_use]
    pub fn customizer_signature_by_name(
        &self,
        interfaces: &ModuleInterfaces,
        ty: TypeId,
        name: &str,
    ) -> Option<(FuncSigId, &FunctionSignature)> {
        let target_owner = self.types.get(ty).owner;
        let mut lo = 0usize;
        let mut hi = self.customizers.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let fn_id = self.customizers[mid];
            let at_owner = self.customizer_target_owner(interfaces, fn_id);
            let at_name = interfaces
                .symbol(self.functions[fn_id.index()].function)
                .name
                .as_str();
            let order = at_owner.cmp(&target_owner).then_with(|| at_name.cmp(name));
            if order.is_lt() {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo == self.customizers.len() {
            return None;
        }
        let fn_id = self.customizers[lo];
        let at_owner = self.customizer_target_owner(interfaces, fn_id);
        let at_name = interfaces
            .symbol(self.functions[fn_id.index()].function)
            .name
            .as_str();
        if at_owner == target_owner && at_name == name {
            Some((fn_id, &self.functions[fn_id.index()]))
        } else {
            None
        }
    }

    /// Binary search for a `to_text` or `compare` customizer (`mo_customizer_signature`).
    #[must_use]
    pub fn customizer_signature(
        &self,
        interfaces: &ModuleInterfaces,
        ty: TypeId,
        ability: SourceAbility,
    ) -> Option<(FuncSigId, &FunctionSignature)> {
        let name = if ability == SourceAbility::Text {
            "to_text"
        } else {
            "compare"
        };
        self.customizer_signature_by_name(interfaces, ty, name)
    }

    /// Looks up a native signature by `(module, name)`.
    #[must_use]
    pub fn find_native(&self, module: &str, name: &str) -> Option<(NativeSigId, &NativeSignature)> {
        self.native
            .iter()
            .enumerate()
            .find(|(_, sig)| sig.module == module && sig.name == name)
            .map(|(i, sig)| (NativeSigId(i as u32), sig))
    }
}

/// Looks up a nominal type declared in the standard-library `Json` module (`mo_json_type`).
#[must_use]
pub fn json_type(
    interfaces: &ModuleInterfaces,
    signatures: &Signatures,
    name: &str,
) -> Option<TypeId> {
    for (i, scope) in interfaces.scopes.iter().enumerate() {
        if !scope.library || scope.name.as_deref() != Some("Json") {
            continue;
        }
        let sym_idx = scope.find_symbol_index(name.as_bytes())?;
        let sym_ref = SymRef::new(ModuleId(i as u32), sym_idx);
        let (_, nominal) = signatures.nominal_signature_by_symbol(sym_ref)?;
        return Some(nominal.ty);
    }
    None
}

#[derive(Debug, Clone, Copy)]
enum ResolveOwner {
    Symbol { sym: SymRef, decl_kind: SyntaxKind },
    Native { id: NativeSigId },
    Synthetic { module: ModuleId },
}

impl ResolveOwner {
    const fn module(self) -> Option<ModuleId> {
        match self {
            Self::Symbol { sym, .. } => Some(sym.module),
            Self::Native { .. } => None,
            Self::Synthetic { module } => Some(module),
        }
    }

    const fn allows_fresh_fn_param(self) -> bool {
        matches!(
            self,
            Self::Symbol {
                decl_kind: SyntaxKind::Fn,
                ..
            } | Self::Native { .. }
        )
    }

    const fn type_owner(self) -> TypeOwner {
        match self {
            Self::Symbol { sym, .. } => TypeOwner::Source(sym),
            Self::Native { id } => TypeOwner::Native(id),
            Self::Synthetic { .. } => TypeOwner::None,
        }
    }
}

struct Preparation<'a, 'd> {
    set: &'a ModuleSet,
    interfaces: &'a ModuleInterfaces,
    diags: &'d mut DiagBag,
    owner: ResolveOwner,
    parameters: [Option<TypeId>; 26],
    signatures: &'a mut Signatures,
    span: Span,
    native_span: Span,
    depth: u32,
    failed: bool,
    native: bool,
}

impl<'a, 'd> Preparation<'a, 'd> {
    fn problem(
        &mut self,
        code: ErrorCode,
        span: Span,
        message: impl Into<String>,
        reason: Option<&'static str>,
    ) -> bool {
        let span = if self.native { self.native_span } else { span };
        let message = message.into();
        let label = match code {
            ErrorCode::TypeName => "not a type in scope".to_owned(),
            ErrorCode::TypeArguments => "type parameters don't match".to_owned(),
            ErrorCode::BoolName => "must be Bool or a function returning Bool".to_owned(),
            ErrorCode::FieldRepeated => "this field is declared twice".to_owned(),
            ErrorCode::DataParameters => {
                "type parameters must be distinct single letters".to_owned()
            }
            _ => message.clone(),
        };
        let mut diag = Diagnostic::new(code, span, message).with_primary_text(label);
        if let Some(reason_text) = reason {
            diag = diag.with_reason(reason_text);
        }
        let kept = self.diags.push(diag).unwrap_or(false);
        self.failed = !kept;
        false
    }

    fn use_origin(&mut self, span: Span, origin: NodeId, origin_span: Span) -> bool {
        let Some(module) = self.owner.module() else {
            return true;
        };
        if self.signatures.use_import(module, origin, origin_span) {
            return true;
        }
        let _ = self.problem(
            ErrorCode::CheckProgramWork,
            span,
            "the program needs too much import-checking work",
            None,
        );
        self.failed = true;
        false
    }

    fn sqlite_library_symbol(&self, member: &[u8]) -> Option<SymRef> {
        for (i, lib) in self.interfaces.scopes.iter().enumerate() {
            if !lib.library || lib.name.as_deref() != Some("Sqlite") {
                continue;
            }
            let idx = lib.find_symbol_index(member)?;
            let sym = lib.symbol(idx);
            return sym.exported.then_some(SymRef::new(ModuleId(i as u32), idx));
        }
        None
    }

    fn api_library_symbol(&self, member: &[u8]) -> Option<SymRef> {
        for (i, lib) in self.interfaces.scopes.iter().enumerate() {
            if !lib.library || lib.name.as_deref() != Some("Api") {
                continue;
            }
            let idx = lib.find_symbol_index(member)?;
            let sym = lib.symbol(idx);
            return sym.exported.then_some(SymRef::new(ModuleId(i as u32), idx));
        }
        None
    }

    fn network_library_symbol(&self, member: &[u8]) -> Option<SymRef> {
        for (i, lib) in self.interfaces.scopes.iter().enumerate() {
            if !lib.library || lib.name.as_deref() != Some("Network") {
                continue;
            }
            let idx = lib.find_symbol_index(member)?;
            let sym = lib.symbol(idx);
            return sym.exported.then_some(SymRef::new(ModuleId(i as u32), idx));
        }
        None
    }

    fn module_member(
        &mut self,
        scope: &ModuleScope,
        name_span: Span,
        prefix: &[u8],
        member: &[u8],
    ) -> Result<Option<SymRef>, ()> {
        if prefix == b"ApiTest" && member == b"RouteSpec" {
            return Ok(self.api_library_symbol(b"RouteSpec"));
        }
        let Some(used) = scope.find_alias(prefix) else {
            if prefix == b"Sqlite" {
                return Ok(self.sqlite_library_symbol(member));
            }
            if prefix == b"Network" {
                return Ok(self.network_library_symbol(member));
            }
            return Ok(None);
        };
        let target_mod = used.module;
        let used_origin = used.origin;
        let used_origin_span = used.origin_span;
        let target_scope = self.interfaces.scope(target_mod);
        let Some(sym_idx) = target_scope.find_symbol_index(member) else {
            return Ok(None);
        };
        let sym = target_scope.symbol(sym_idx);
        if !sym.exported {
            let _ = self.problem(
                ErrorCode::ImportPrivate,
                name_span,
                "everything not on expose is private to the module",
                Some("Everything not on expose is private to the module (reference 10.1)."),
            );
            self.failed = true;
            return Err(());
        }
        if !self.use_origin(name_span, used_origin, used_origin_span) {
            return Err(());
        }
        Ok(Some(SymRef::new(target_mod, sym_idx)))
    }

    fn own_type(
        &self,
        scope: &ModuleScope,
        name_kind: SyntaxKind,
        name_text: &[u8],
    ) -> Option<SymRef> {
        if name_kind != SyntaxKind::CapName {
            return None;
        }
        let used = scope.find_alias(name_text)?;
        let target_scope = self.interfaces.scope(used.module);
        let sym_idx = target_scope.find_symbol_index(name_text)?;
        let sym = target_scope.symbol(sym_idx);
        if sym.exported
            && sym.owner.is_none()
            && matches!(
                sym.decl_kind,
                SyntaxKind::Struct | SyntaxKind::Enum | SyntaxKind::Actor
            )
        {
            Some(SymRef::new(used.module, sym_idx))
        } else {
            None
        }
    }

    fn definition(&mut self, tree: &SyntaxTree, name_nid: NodeId) -> Result<Option<SymRef>, ()> {
        let Some(mod_id) = self.owner.module() else {
            return Ok(None);
        };
        let scope = self.interfaces.scope(mod_id);
        let name_node = tree.node(name_nid);
        if name_node.kind == SyntaxKind::Qualified {
            let Some(prefix_nid) = name_node.children.first().copied().flatten() else {
                return Ok(None);
            };
            let Some(member_nid) = name_node.children.get(1).copied().flatten() else {
                return Ok(None);
            };
            let prefix = tree.node_text(prefix_nid).unwrap_or(b"");
            let member = tree.node_text(member_nid).unwrap_or(b"");
            return self.module_member(scope, name_node.span, prefix, member);
        }
        let name_text = tree.node_text(name_nid).unwrap_or(b"");
        let mut symbol = scope
            .find_symbol_index(name_text)
            .map(|idx| SymRef::new(mod_id, idx));
        let mut origin: Option<(NodeId, Span)> = None;
        if symbol.is_none()
            && let Some(imp) = scope.find_imported(name_text)
        {
            symbol = Some(imp.symbol);
            origin = Some((imp.origin, imp.origin_span));
        }
        let Some(sym_ref) = symbol else {
            if name_node.kind == SyntaxKind::CapName && name_text == b"SqliteDecodeError" {
                return Ok(self.sqlite_library_symbol(name_text));
            }
            if name_node.kind == SyntaxKind::CapName {
                return self.module_member(scope, name_node.span, name_text, name_text);
            }
            return Ok(None);
        };
        let own = self.own_type(scope, name_node.kind, name_text);
        let cmd_clash = if name_node.kind == SyntaxKind::CapName && name_text == b"Command" {
            scope.find_alias(b"Command").is_some_and(|cmd_alias| {
                let target = self.interfaces.scope(cmd_alias.module);
                target.library && target.name.as_deref() == Some("Command")
            })
        } else {
            false
        };
        if (own.is_some() && own != Some(sym_ref)) || cmd_clash {
            let _ = self.problem(
                ErrorCode::NameClash,
                name_node.span,
                "a clash between names",
                Some("A clash between names is an error; use the qualified form (reference 10.3)."),
            );
            self.failed = true;
            return Err(());
        }
        if let Some((orig_nid, orig_span)) = origin
            && !self.use_origin(name_node.span, orig_nid, orig_span)
        {
            return Err(());
        }
        Ok(Some(sym_ref))
    }

    fn resolve(&mut self, tree: &SyntaxTree, node: Option<NodeId>) -> Option<TypeId> {
        debug_assert!(u64::from(self.depth) < MO_LIMIT_NESTING_DEPTH);
        self.depth += 1;
        let result = self.resolve_type(tree, node);
        self.depth -= 1;
        result
    }

    fn resolve_type(&mut self, tree: &SyntaxTree, node: Option<NodeId>) -> Option<TypeId> {
        let Some(nid) = node else {
            return Some(self.signatures.types.intern_builtin("Unit"));
        };
        let syn = tree.node(nid);
        self.span = syn.span;
        if syn.token == TokenKind::Fn || syn.token == TokenKind::LParen {
            let is_fn = syn.token == TokenKind::Fn;
            let seq_nid = syn.children.first().copied().flatten()?;
            let seq_children = &tree.node(seq_nid).children;
            let mut arguments = Vec::with_capacity(seq_children.len() + usize::from(is_fn));
            for &child in seq_children {
                arguments.push(self.resolve(tree, child)?);
            }
            if is_fn {
                let ret_nid = syn.children.get(1).copied().flatten();
                arguments.push(self.resolve(tree, ret_nid)?);
            }
            let kind = if is_fn {
                SourceTypeKind::Function
            } else {
                SourceTypeKind::Tuple
            };
            let name = if is_fn { "fn" } else { "()" };
            return Some(self.signatures.types.intern(
                kind,
                name,
                None,
                TypeOwner::None,
                arguments,
            ));
        }

        let name_nid = syn.children.first().copied().flatten()?;
        let args_nid = syn.children.get(1).copied().flatten();
        let count = args_nid.map_or(0u32, |a| tree.node(a).children.len() as u32);
        let name_node = tree.node(name_nid);
        let name_text = tree.node_text(name_nid).unwrap_or(b"");

        if name_node.kind == SyntaxKind::CapName
            && name_text.len() == 1
            && name_text[0].is_ascii_uppercase()
            && let Some(param_ty) = self.parameters[(name_text[0] - b'A') as usize]
        {
            if count == 0 {
                return Some(param_ty);
            }
            let _ = self.problem(
                ErrorCode::TypeArguments,
                syn.span,
                "a type parameter is used as a generic type",
                Some(
                    "A type parameter is a single capital letter, not a generic type constructor \
                     (reference 9.1).",
                ),
            );
            return None;
        }

        let symbol = match self.definition(tree, name_nid) {
            Ok(sym) => sym,
            Err(()) => return None,
        };

        let mut ty_name: Option<String> = None;
        let mut ty_module: Option<ModuleId> = None;
        let mut ty_owner = TypeOwner::None;
        let mut arity = 0u32;

        if let Some(sym_ref) = symbol {
            let sym = self.interfaces.symbol(sym_ref);
            if sym.owner.is_none()
                && matches!(
                    sym.decl_kind,
                    SyntaxKind::Struct | SyntaxKind::Enum | SyntaxKind::Actor
                )
            {
                ty_name = Some(sym.name.clone());
                ty_module = Some(sym.module);
                ty_owner = TypeOwner::Source(sym_ref);
                if sym.decl_kind != SyntaxKind::Actor {
                    let decl_tree = &self.set.module(sym.module).syntax.tree;
                    let params = decl_tree
                        .node(sym.declaration)
                        .children
                        .get(1)
                        .copied()
                        .flatten();
                    arity = params.map_or(0, |p| decl_tree.node(p).children.len() as u32);
                }
            }
        } else if name_node.kind == SyntaxKind::CapName {
            if let Some((builtin_name, builtin_arity)) = builtin_type_arity(name_text) {
                ty_name = Some(builtin_name.to_owned());
                arity = builtin_arity;
            }
            if ty_name.is_none() && name_text == b"Command" {
                match self.owner.module() {
                    None => {
                        ty_name = Some("Command".to_owned());
                    }
                    Some(mod_id) => {
                        let scope = self.interfaces.scope(mod_id);
                        if scope.library && scope.name.as_deref() == Some("Command") {
                            ty_name = Some("Command".to_owned());
                        } else if let Some(used) = scope.find_alias(name_text) {
                            let target = self.interfaces.scope(used.module);
                            if target.library && target.name.as_deref() == Some("Command") {
                                let orig = used.origin;
                                let orig_span = used.origin_span;
                                if !self.use_origin(name_node.span, orig, orig_span) {
                                    return None;
                                }
                                ty_name = Some("Command".to_owned());
                            }
                        }
                    }
                }
            }
            if ty_name.is_none()
                && name_text.len() == 1
                && name_text[0].is_ascii_uppercase()
                && self.owner.allows_fresh_fn_param()
            {
                if count != 0 {
                    let _ = self.problem(
                        ErrorCode::TypeArguments,
                        syn.span,
                        "a type parameter is used as a generic type",
                        Some(
                            "A type parameter is a single capital letter, not a generic type \
                             constructor (reference 9.1).",
                        ),
                    );
                    return None;
                }
                let letter = char::from(name_text[0]).to_string();
                return Some(self.signatures.types.intern(
                    SourceTypeKind::Parameter,
                    letter,
                    self.owner.module(),
                    self.owner.type_owner(),
                    Vec::new(),
                ));
            }
        } else if name_node.kind == SyntaxKind::Qualified
            && let Some(lhs_nid) = name_node.children.first().copied().flatten()
            && let Some(rhs_nid) = name_node.children.get(1).copied().flatten()
        {
            let lhs = tree.node_text(lhs_nid).unwrap_or(b"");
            let rhs = tree.node_text(rhs_nid).unwrap_or(b"");
            if rhs == b"Command" || rhs == b"Process" {
                let resolved_name = if rhs == b"Process" {
                    "Command.Process"
                } else {
                    "Command"
                };
                match self.owner.module() {
                    None if lhs == b"Command" => {
                        ty_name = Some(resolved_name.to_owned());
                    }
                    None => {}
                    Some(mod_id) => {
                        let scope = self.interfaces.scope(mod_id);
                        if scope.library
                            && scope.name.as_deref() == Some("Command")
                            && lhs == b"Command"
                        {
                            ty_name = Some(resolved_name.to_owned());
                        } else if let Some(used) = scope.find_alias(lhs) {
                            let target = self.interfaces.scope(used.module);
                            if target.library && target.name.as_deref() == Some("Command") {
                                let orig = used.origin;
                                let orig_span = used.origin_span;
                                if !self.use_origin(name_node.span, orig, orig_span) {
                                    return None;
                                }
                                ty_name = Some(resolved_name.to_owned());
                            }
                        }
                    }
                }
            } else if lhs == b"Sqlite" {
                ty_name = match rhs {
                    b"Database" => Some("Sqlite.Database".to_owned()),
                    b"Value" => Some("Sqlite.Value".to_owned()),
                    b"Rows" => Some("Sqlite.Rows".to_owned()),
                    _ => None,
                };
            } else if lhs == b"Files" && rhs == b"File" {
                ty_name = Some("Files.File".to_owned());
            } else if lhs == b"ApiTest" && rhs == b"App" {
                ty_name = Some("ApiTest.App".to_owned());
            }
        }

        let Some(ty_name) = ty_name else {
            let _ = self.problem(
                ErrorCode::TypeName,
                name_node.span,
                "a type name isn't in scope",
                Some("Use brings a module's exposed type names into scope (reference 10.3)."),
            );
            return None;
        };

        if count != arity {
            let _ = self.problem(
                ErrorCode::TypeArguments,
                syn.span,
                "a type's type parameters don't match",
                Some(
                    "List(T), Map(K, V) and declared generic types take type parameters \
                     (reference 9.1).",
                ),
            );
            return None;
        }

        let mut arguments = Vec::with_capacity(count as usize);
        if let Some(args) = args_nid {
            for &arg_child in &tree.node(args).children {
                arguments.push(self.resolve(tree, arg_child)?);
            }
        }
        Some(self.signatures.types.intern(
            SourceTypeKind::Named,
            ty_name,
            ty_module,
            ty_owner,
            arguments,
        ))
    }

    fn question_name(&mut self, tree: &SyntaxTree, name_nid: NodeId, ty: TypeId) -> bool {
        let name_text = tree.node_text(name_nid).unwrap_or(b"");
        if !name_text.ends_with(b"?") {
            return true;
        }
        let mut check_ty = self.signatures.types.get(ty);
        if check_ty.kind == SourceTypeKind::Function
            && let Some(&ret_id) = check_ty.arguments.last()
        {
            check_ty = self.signatures.types.get(ret_id);
        }
        if check_ty.is_builtin_named("Bool") {
            return true;
        }
        self.problem(
            ErrorCode::BoolName,
            tree.node(name_nid).span,
            "a name ending in ? isn't a yes-or-no answer",
            Some("A name ending in ? is a Bool, or a function that returns one (reference 2.3)."),
        )
    }

    fn dependency_work(&mut self) -> bool {
        if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            return self.problem(
                ErrorCode::CheckProgramWork,
                self.span,
                "the program needs too much signature-checking work",
                None,
            );
        }
        self.signatures.work += 1;
        true
    }

    fn dependencies(&mut self, ty_id: TypeId, keys: bool, out: &mut u32) -> bool {
        if !self.dependency_work() {
            return false;
        }
        let ty = self.signatures.types.get(ty_id);
        let ty_params = ty.parameters;
        if ty_params == 0 {
            return true;
        }
        let ty_kind = ty.kind;
        let ty_owner = ty.owner;
        let ty_module = ty.module;
        let arg_count = ty.arguments.len();
        if ty_kind == SourceTypeKind::Parameter {
            if !keys {
                *out |= ty_params;
            }
            return true;
        }
        if !keys && ty_kind == SourceTypeKind::Function {
            return true;
        }
        if ty_kind == SourceTypeKind::Named
            && let TypeOwner::Source(sym) = ty_owner
        {
            let owner_info = self
                .signatures
                .nominal_signature_by_symbol(sym)
                .map(|(_, nom)| {
                    (
                        if keys {
                            nom.key_parameters
                        } else {
                            nom.hash_parameters
                        },
                        nom.ty,
                    )
                });
            if let Some((needed, nom_ty_id)) = owner_info {
                for i in 0..arg_count {
                    let param_mask = self
                        .signatures
                        .types
                        .get(self.signatures.types.get(nom_ty_id).arguments[i])
                        .parameters;
                    let arg_ty = self.signatures.types.get(ty_id).arguments[i];
                    if (needed & param_mask) != 0 && !self.dependencies(arg_ty, false, out) {
                        return false;
                    }
                }
            } else {
                debug_assert!(self.signatures.recovering);
                return true;
            }
        }
        let is_builtin_map_or_set = {
            let t = self.signatures.types.get(ty_id);
            t.kind == SourceTypeKind::Named
                && ty_module.is_none()
                && keys
                && (t.name == "Map" || t.name == "Set")
        };
        if is_builtin_map_or_set {
            let key_arg = self.signatures.types.get(ty_id).arguments[0];
            if !self.dependencies(key_arg, false, out) {
                return false;
            }
        }
        let recurse_children = keys || ty_owner == TypeOwner::None;
        if recurse_children {
            for i in 0..arg_count {
                let arg_ty = self.signatures.types.get(ty_id).arguments[i];
                if !self.dependencies(arg_ty, keys, out) {
                    return false;
                }
            }
        }
        true
    }

    fn capture_dependencies(
        &mut self,
        ty_id: TypeId,
        parameters: &mut u32,
        capability: &mut bool,
        function: &mut bool,
    ) -> bool {
        if !self.dependency_work() {
            return false;
        }
        let ty = self.signatures.types.get(ty_id);
        let ty_kind = ty.kind;
        let ty_params = ty.parameters;
        let ty_owner = ty.owner;
        let ty_module = ty.module;
        let arg_count = ty.arguments.len();
        if ty_kind == SourceTypeKind::Parameter {
            *parameters |= ty_params;
            return true;
        }
        if ty_kind == SourceTypeKind::Function {
            *function = true;
            return true;
        }
        if ty_kind == SourceTypeKind::Named
            && let TypeOwner::Source(sym) = ty_owner
        {
            let owner_info = self
                .signatures
                .nominal_signature_by_symbol(sym)
                .map(|(_, nom)| {
                    (
                        nom.holds_capability,
                        nom.holds_function,
                        nom.capture_parameters,
                        nom.ty,
                    )
                });
            let Some((holds_cap, holds_fn, cap_params, nom_ty_id)) = owner_info else {
                debug_assert!(self.signatures.recovering);
                return true;
            };
            *capability = *capability || holds_cap;
            *function = *function || holds_fn;
            for i in 0..arg_count {
                let param_mask = self
                    .signatures
                    .types
                    .get(self.signatures.types.get(nom_ty_id).arguments[i])
                    .parameters;
                let arg_ty = self.signatures.types.get(ty_id).arguments[i];
                if (cap_params & param_mask) != 0
                    && !self.capture_dependencies(arg_ty, parameters, capability, function)
                {
                    return false;
                }
            }
            return true;
        }
        if ty_kind == SourceTypeKind::Named
            && ty_module.is_none()
            && is_capability_name(&self.signatures.types.get(ty_id).name)
        {
            *capability = true;
        }
        for i in 0..arg_count {
            let arg_ty = self.signatures.types.get(ty_id).arguments[i];
            if !self.capture_dependencies(arg_ty, parameters, capability, function) {
                return false;
            }
        }
        true
    }

    fn watch_dependencies(
        &mut self,
        ty_id: TypeId,
        data_idx: usize,
        watchers: &mut [Vec<usize>],
    ) -> bool {
        if !self.dependency_work() {
            return false;
        }
        let ty = self.signatures.types.get(ty_id);
        let arg_count = ty.arguments.len();
        if ty.kind == SourceTypeKind::Named
            && let TypeOwner::Source(sym) = ty.owner
        {
            let Some((nom_id, _)) = self.signatures.nominal_signature_by_symbol(sym) else {
                debug_assert!(self.signatures.recovering);
                return true;
            };
            watchers[nom_id.index()].push(data_idx);
        }
        for i in 0..arg_count {
            let arg_ty = self.signatures.types.get(ty_id).arguments[i];
            if !self.watch_dependencies(arg_ty, data_idx, watchers) {
                return false;
            }
        }
        true
    }

    fn nominal_index(&mut self) {
        let mut raw: Vec<NominalSignature> = Vec::new();
        for data in &self.signatures.data {
            let Some(owner_sym) = self.signatures.types.get(data.ty).owner.as_source() else {
                continue;
            };
            raw.push(NominalSignature {
                symbol: owner_sym,
                ty: data.ty,
                field_count: data.fields.len() as u32,
                hash_parameters: 0,
                key_parameters: 0,
                capture_parameters: 0,
                case_count: 1,
                holds_capability: false,
                holds_function: false,
                invalid: false,
            });
        }
        for (m, scope) in self.interfaces.scopes.iter().enumerate() {
            for (j, sym) in scope.symbols.iter().enumerate() {
                if sym.decl_kind != SyntaxKind::Actor {
                    continue;
                }
                let sym_ref = SymRef::new(ModuleId(m as u32), crate::ids::SymIdx(j as u32));
                let actor_ty = self.signatures.types.intern(
                    SourceTypeKind::Named,
                    sym.name.as_str(),
                    Some(sym.module),
                    TypeOwner::Source(sym_ref),
                    Vec::new(),
                );
                raw.push(NominalSignature {
                    symbol: sym_ref,
                    ty: actor_ty,
                    field_count: 0,
                    hash_parameters: 0,
                    key_parameters: 0,
                    capture_parameters: 0,
                    case_count: 0,
                    holds_capability: true,
                    holds_function: false,
                    invalid: false,
                });
            }
        }
        raw.sort_by_key(|n| n.symbol);
        let mut merged: Vec<NominalSignature> = Vec::new();
        for item in raw {
            if let Some(last) = merged.last_mut()
                && last.symbol == item.symbol
            {
                last.field_count = last.field_count.saturating_add(item.field_count);
                last.case_count += 1;
            } else {
                merged.push(item);
            }
        }
        for nom in &mut merged {
            let sym = self.interfaces.symbol(nom.symbol);
            let expected_cases = match sym.decl_kind {
                SyntaxKind::Enum => sym.cases.len() as u32,
                SyntaxKind::Actor => 0,
                _ => 1,
            };
            nom.invalid = nom.case_count != expected_cases;
        }
        self.signatures.nominals = merged;
    }

    fn data_dependencies(&mut self) -> bool {
        let data_count = self.signatures.data.len();
        if data_count == 0 {
            return true;
        }
        let mut watchers: Vec<Vec<usize>> = vec![Vec::new(); self.signatures.nominals.len()];
        let mut queue: VecDeque<usize> = VecDeque::with_capacity(data_count);
        let mut pending: Vec<bool> = vec![true; data_count];
        for i in 0..data_count {
            queue.push_back(i);
            let field_count = self.signatures.data[i].fields.len();
            for j in 0..field_count {
                let f_ty = self.signatures.data[i].fields[j].ty;
                if !self.watch_dependencies(f_ty, i, &mut watchers) {
                    return false;
                }
            }
        }
        while let Some(index) = queue.pop_front() {
            pending[index] = false;
            let data_sym = self.signatures.data[index].symbol;
            let data_ty = self.signatures.data[index].ty;
            self.span = self.interfaces.symbol(data_sym).declaration_span;
            let Some(owner_sym) = self.signatures.types.get(data_ty).owner.as_source() else {
                continue;
            };
            let Some((nom_id, _)) = self.signatures.nominal_signature_by_symbol(owner_sym) else {
                continue;
            };
            let owner_index = nom_id.index();
            let prev_invalid = self.signatures.nominals[owner_index].invalid;
            let mut hash = self.signatures.data[index].hash_parameters;
            let mut keys = self.signatures.data[index].key_parameters;
            let mut captures = self.signatures.nominals[owner_index].capture_parameters;
            let mut capability = self.signatures.nominals[owner_index].holds_capability;
            let mut function = self.signatures.nominals[owner_index].holds_function;

            let field_count = self.signatures.data[index].fields.len();
            for j in 0..field_count {
                let field_ty = self.signatures.data[index].fields[j].ty;
                if !self.signatures.signature_type_available(Some(field_ty)) {
                    self.signatures.nominals[owner_index].invalid = true;
                }
                if !self.dependencies(field_ty, false, &mut hash)
                    || !self.dependencies(field_ty, true, &mut keys)
                    || !self.capture_dependencies(
                        field_ty,
                        &mut captures,
                        &mut capability,
                        &mut function,
                    )
                {
                    return false;
                }
            }
            self.signatures.data[index].hash_parameters = hash;
            self.signatures.data[index].key_parameters = keys;
            let owner = &mut self.signatures.nominals[owner_index];
            if owner.invalid == prev_invalid
                && (hash & !owner.hash_parameters) == 0
                && (keys & !owner.key_parameters) == 0
                && captures == owner.capture_parameters
                && capability == owner.holds_capability
                && function == owner.holds_function
            {
                continue;
            }
            owner.hash_parameters |= hash;
            owner.key_parameters |= keys;
            owner.capture_parameters = captures;
            owner.holds_capability = capability;
            owner.holds_function = function;

            for &dep_data in watchers[owner_index].iter().rev() {
                if !self.dependency_work() {
                    return false;
                }
                if !pending[dep_data] {
                    pending[dep_data] = true;
                    queue.push_back(dep_data);
                }
            }
        }
        true
    }

    fn requirements(
        &mut self,
        tree: &SyntaxTree,
        fn_decl: NodeId,
        ty_id: TypeId,
    ) -> Option<[u8; 26]> {
        let mut out = [0u8; 26];
        let mut keys = 0u32;
        if !self.dependencies(ty_id, true, &mut keys) {
            return None;
        }
        for (i, slot) in out.iter_mut().enumerate() {
            if (keys & (1u32 << i)) != 0 {
                *slot |= SourceAbility::Hash.bit();
            }
        }
        let ty_parameters = self.signatures.types.get(ty_id).parameters;
        let where_nid = tree.node(fn_decl).children.get(3).copied().flatten();
        if let Some(where_node_id) = where_nid {
            for &clause_opt in &tree.node(where_node_id).children {
                let Some(clause_nid) = clause_opt else {
                    continue;
                };
                let clause = tree.node(clause_nid);
                let name_nid = clause.children.first().copied().flatten()?;
                let ability_nid = clause.children.get(1).copied().flatten()?;
                let name_text = tree.node_text(name_nid).unwrap_or(b"");
                let letter = name_text
                    .first()
                    .copied()
                    .filter(u8::is_ascii_uppercase)
                    .map(|b| (b - b'A') as usize);
                let valid_letter = name_text.len() == 1
                    && letter.is_some_and(|l| (ty_parameters & (1u32 << l)) != 0);
                if !valid_letter {
                    let _ = self.problem(
                        ErrorCode::AbilityRequirement,
                        tree.node(name_nid).span,
                        "where doesn't name a type parameter in this signature",
                        Some(
                            "A single capital letter in a signature is a type parameter, and \
                             where asks for abilities (reference 5.5).",
                        ),
                    );
                    return None;
                }
                let ability_text = tree.node_text(ability_nid).unwrap_or(b"");
                let Some(ability) = SourceAbility::from_bytes(ability_text) else {
                    let _ = self.problem(
                        ErrorCode::AbilityRequirement,
                        tree.node(ability_nid).span,
                        "where names an ability other than the six built in",
                        Some(
                            "Where asks for one of Equal, Hash, Order, Text, Number or Json \
                             (reference 5.5).",
                        ),
                    );
                    return None;
                };
                if let Some(l) = letter {
                    out[l] |= ability.bit();
                }
            }
        }
        for slot in &mut out {
            if (*slot & (SourceAbility::Hash.bit() | SourceAbility::Order.bit())) != 0 {
                *slot |= SourceAbility::Equal.bit();
            }
        }
        Some(out)
    }

    fn is_json_nominal(&self, ty_id: TypeId, name: &str) -> bool {
        let ty = self.signatures.types.get(ty_id);
        let Some(mod_id) = ty.module else {
            return false;
        };
        let scope = self.interfaces.scope(mod_id);
        ty.kind == SourceTypeKind::Named
            && ty.owner.as_source().is_some()
            && ty.arguments.is_empty()
            && ty.name == name
            && scope.library
            && scope.name.as_deref() == Some("Json")
    }

    fn customizer(&mut self, fn_sym: SymRef, ty_id: TypeId) -> bool {
        let sym = self.interfaces.symbol(fn_sym);
        let fn_name = sym.name.as_str();
        let fn_module = sym.module;
        let name_span = sym.name_span;
        let fn_ty = self.signatures.types.get(ty_id);
        let fn_params_mask = fn_ty.parameters;
        let arg_count = fn_ty.arguments.len();
        let Some(&result_id) = fn_ty.arguments.last() else {
            return false;
        };

        let to_json = fn_name == "to_json";
        let from_json = fn_name == "from_json";
        if to_json || from_json {
            let scope = self.interfaces.scope(fn_module);
            let result = self.signatures.types.get(result_id);
            if from_json
                && scope.library
                && scope.name.as_deref() == Some("Json")
                && result.is_builtin_named("Result")
                && result.arguments.len() == 2
                && self.signatures.types.get(result.arguments[0]).kind == SourceTypeKind::Parameter
            {
                return true;
            }
            let mut valid = arg_count == 2;
            let mut receiver_id: Option<TypeId> = None;
            if to_json && valid {
                receiver_id = Some(self.signatures.types.get(ty_id).arguments[0]);
                valid = self.is_json_nominal(result_id, "Json");
            } else if from_json && valid {
                let first_arg = self.signatures.types.get(ty_id).arguments[0];
                let res = self.signatures.types.get(result_id);
                valid = self.is_json_nominal(first_arg, "Json")
                    && res.is_builtin_named("Result")
                    && res.arguments.len() == 2
                    && self
                        .signatures
                        .types
                        .get(res.arguments[1])
                        .is_builtin_named("Text");
                if valid {
                    receiver_id = Some(res.arguments[0]);
                }
            }
            if let Some(rec_id) = receiver_id {
                let rec = self.signatures.types.get(rec_id);
                let owned = rec.kind == SourceTypeKind::Named
                    && rec.owner.as_source().is_some()
                    && rec.module == Some(fn_module);
                valid = valid && owned;
                let mut parameters = 0u32;
                for &arg_id in &rec.arguments {
                    if !valid {
                        break;
                    }
                    let arg = self.signatures.types.get(arg_id);
                    valid =
                        arg.kind == SourceTypeKind::Parameter && (parameters & arg.parameters) == 0;
                    parameters |= arg.parameters;
                }
                if valid && parameters == fn_params_mask {
                    return true;
                }
            }
            return self.problem(
                ErrorCode::AbilityCustomizer,
                name_span,
                if to_json {
                    "to_json doesn't have the type's JSON signature"
                } else {
                    "from_json doesn't have the type's JSON signature"
                },
                Some(
                    "A type customizes Json in its own module with to_json(T): Json and/or \
                     from_json(Json): Result(T, Text) (reference 9.2).",
                ),
            );
        }

        let compare = fn_name == "compare";
        if !compare && fn_name != "to_text" {
            return true;
        }
        let expected_count = if compare { 3 } else { 2 };
        let mut valid = arg_count == expected_count;
        if valid {
            let rec_id = self.signatures.types.get(ty_id).arguments[0];
            let rec = self.signatures.types.get(rec_id);
            let res = self.signatures.types.get(result_id);
            let scope = self.interfaces.scope(fn_module);
            let library = rec.is_builtin_named("FileError")
                && scope.library
                && scope.name.as_deref() == Some("FileError")
                && !compare;
            let owned = rec.owner.as_source().is_some() && rec.module == Some(fn_module);
            let second_matches =
                !compare || self.signatures.types.get(ty_id).arguments[1] == rec_id;
            valid = rec.kind == SourceTypeKind::Named
                && (owned || library)
                && second_matches
                && res.is_builtin_named(if compare { "Ordering" } else { "Text" });
            let mut parameters = 0u32;
            for &arg_id in &rec.arguments {
                if !valid {
                    break;
                }
                let arg = self.signatures.types.get(arg_id);
                valid = arg.kind == SourceTypeKind::Parameter && (parameters & arg.parameters) == 0;
                parameters |= arg.parameters;
            }
            if valid && parameters == fn_params_mask {
                return true;
            }
        }
        self.problem(
            ErrorCode::AbilityCustomizer,
            name_span,
            if compare {
                "compare doesn't have the type's order signature"
            } else {
                "to_text doesn't have the type's text signature"
            },
            Some(
                "A type customizes Order or Text in its own module with compare(T, T): Ordering \
                 or to_text(T): Text (reference 9.2). This stage reserves these names for \
                 customizers; it doesn't certify unrelated functions with these names.",
            ),
        )
    }

    fn data_signature(&mut self, sym_ref: SymRef) -> Option<DataSignature> {
        let sym = self.interfaces.symbol(sym_ref);
        let owner_ref = match sym.owner {
            Some(owner_idx) => SymRef::new(sym.module, owner_idx),
            None => sym_ref,
        };
        let owner = self.interfaces.symbol(owner_ref);
        self.owner = ResolveOwner::Symbol {
            sym: owner_ref,
            decl_kind: owner.decl_kind,
        };
        self.span = sym.declaration_span;
        self.parameters = [None; 26];

        let tree = &self.set.module(owner.module).syntax.tree;
        let owner_decl = tree.node(owner.declaration);
        let parameters_nid = owner_decl.children.get(1).copied().flatten();
        let arity = parameters_nid.map_or(0usize, |p| tree.node(p).children.len());
        let mut type_args = Vec::with_capacity(arity);
        if let Some(params_id) = parameters_nid {
            for &child in &tree.node(params_id).children {
                let param_nid = child?;
                let text = tree.node_text(param_nid).unwrap_or(b"");
                let letter_idx = text
                    .first()
                    .copied()
                    .filter(u8::is_ascii_uppercase)
                    .map(|b| (b - b'A') as usize);
                let Some(l) = letter_idx
                    .filter(|&l| text.len() == 1 && arity <= 26 && self.parameters[l].is_none())
                else {
                    let _ = self.problem(
                        ErrorCode::DataParameters,
                        tree.node(param_nid).span,
                        "a type parameter isn't a distinct single capital letter",
                        Some(
                            "Your own types take type parameters, which are single capital \
                             letters (reference 9.1; names in reference 2.3).",
                        ),
                    );
                    return None;
                };
                let letter = char::from(text[0]).to_string();
                let arg_ty = self.signatures.types.intern(
                    SourceTypeKind::Parameter,
                    letter,
                    Some(owner.module),
                    TypeOwner::Source(owner_ref),
                    Vec::new(),
                );
                self.parameters[l] = Some(arg_ty);
                type_args.push(arg_ty);
            }
        }

        let data_ty = self.signatures.types.intern(
            SourceTypeKind::Named,
            owner.name.as_str(),
            Some(owner.module),
            TypeOwner::Source(owner_ref),
            type_args,
        );

        let sym_decl = tree.node(sym.declaration);
        let fields_slot = if sym.decl_kind == SyntaxKind::Struct {
            2
        } else {
            1
        };
        let fields_nid = sym_decl.children.get(fields_slot).copied().flatten();
        let unnamed = fields_nid.is_some_and(|f| tree.node(f).kind == SyntaxKind::Type);
        let count = match fields_nid {
            None => 0usize,
            Some(_) if unnamed => 1usize,
            Some(f) => tree.node(f).children.len(),
        };

        let mut items = Vec::with_capacity(count);
        for i in 0..count {
            let fields_root = fields_nid?;
            let (name_nid, ann_nid) = if unnamed {
                (None, fields_root)
            } else {
                let field_nid = tree.node(fields_root).children[i]?;
                let field_node = tree.node(field_nid);
                (
                    field_node.children.first().copied().flatten(),
                    field_node.children.get(1).copied().flatten()?,
                )
            };
            let field_ty = self.resolve(tree, Some(ann_nid))?;
            if let Some(nn) = name_nid
                && !self.question_name(tree, nn, field_ty)
            {
                return None;
            }
            let name = name_nid
                .map(|nn| String::from_utf8_lossy(tree.node_text(nn).unwrap_or(b"")).into_owned());
            let name_span = name_nid.map(|nn| tree.node(nn).span);
            items.push(FieldSignature {
                name,
                name_node: name_nid,
                name_span,
                annotation: ann_nid,
                annotation_span: tree.node(ann_nid).span,
                ty: field_ty,
                index: i as u32,
            });
        }

        if !unnamed {
            items.sort_by(|a, b| {
                a.name
                    .as_deref()
                    .unwrap_or("")
                    .as_bytes()
                    .cmp(b.name.as_deref().unwrap_or("").as_bytes())
                    .then_with(|| a.index.cmp(&b.index))
            });
            for i in 1..items.len() {
                if items[i - 1].name == items[i].name {
                    let span = items[i].name_span.unwrap_or(items[i].annotation_span);
                    let _ = self.problem(
                        ErrorCode::FieldRepeated,
                        span,
                        "a record or case declares the same field twice",
                        Some("Records and cases have named fields, each with one type (reference 4)."),
                    );
                    return None;
                }
            }
        }

        let mut by_index = vec![0usize; count];
        for (sorted_pos, field) in items.iter().enumerate() {
            by_index[field.index as usize] = sorted_pos;
        }

        Some(DataSignature {
            symbol: sym_ref,
            ty: data_ty,
            parameters: parameters_nid,
            fields: items,
            by_index,
            hash_parameters: 0,
            key_parameters: 0,
            unnamed,
        })
    }

    fn native_signature(
        &mut self,
        native_id: NativeSigId,
        module_name: &str,
        name: &str,
        qualified: &str,
        type_text: &str,
        spec_only: bool,
    ) -> Option<NativeSignature> {
        let mut source = String::from("module Native\nfn interface(");
        let bytes = type_text.as_bytes();
        let is_function = bytes.starts_with(b"fn(");
        let mut pos = 0usize;
        if is_function {
            pos = 3;
            let mut param_idx = 0u32;
            while pos < bytes.len() && bytes[pos] != b')' {
                if param_idx != 0 {
                    source.push_str(", ");
                }
                source.push_str("_p");
                source.push_str(&param_idx.to_string());
                source.push_str(": ");
                param_idx += 1;
                let first = pos;
                let mut depth = 0u32;
                while pos < bytes.len()
                    && !(depth == 0 && (bytes[pos] == b',' || bytes[pos] == b')'))
                {
                    if bytes[pos] == b'(' {
                        depth += 1;
                    } else if bytes[pos] == b')' {
                        depth = depth.saturating_sub(1);
                    }
                    pos += 1;
                }
                debug_assert!(pos < bytes.len());
                source.push_str(&type_text[first..pos]);
                if pos < bytes.len() && bytes[pos] == b',' {
                    pos += 1;
                    while pos < bytes.len() && bytes[pos] == b' ' {
                        pos += 1;
                    }
                }
            }
            if pos < bytes.len() {
                pos += 1;
            }
        }
        source.push_str(if is_function { ")" } else { ") : " });
        source.push_str(&type_text[pos..]);
        source.push_str("\nend\n");

        let mut tmp_sources = SourceMap::new();
        let Ok(file) = tmp_sources.add_bytes("native.mo", source.as_bytes()) else {
            let _ = self.problem(
                ErrorCode::CheckNoMemory,
                self.span,
                "the checking arena has no room",
                None,
            );
            return None;
        };
        let mut tmp_diags = DiagBag::new();
        let Some(parsed) = parse_module(&tmp_sources, file, &mut tmp_diags) else {
            let _ = self.problem(
                ErrorCode::CheckNoMemory,
                self.span,
                "the checking arena has no room",
                None,
            );
            return None;
        };
        debug_assert!(
            !tmp_diags.has_any(),
            "native signature parse error for {qualified}: {type_text}"
        );

        let tree = &parsed.tree;
        let items_nid = tree.node(parsed.root).children.get(1).copied().flatten()?;
        let fn_nid = tree.node(items_nid).children.first().copied().flatten()?;
        let params_nid = tree.node(fn_nid).children.get(1).copied().flatten()?;
        let ret_nid = tree.node(fn_nid).children.get(2).copied().flatten();

        self.owner = ResolveOwner::Native { id: native_id };
        self.parameters = [None; 26];

        let param_children = &tree.node(params_nid).children;
        let mut arguments = Vec::with_capacity(param_children.len() + 1);
        for &p_child in param_children {
            let p_nid = p_child?;
            let ann = tree.node(p_nid).children.get(1).copied().flatten();
            arguments.push(self.resolve(tree, ann)?);
        }
        arguments.push(self.resolve(tree, ret_nid)?);

        let fn_ty = self.signatures.types.intern(
            SourceTypeKind::Function,
            "fn",
            None,
            TypeOwner::None,
            arguments.clone(),
        );
        let final_ty = if is_function { fn_ty } else { arguments[0] };
        let reqs = self.requirements(tree, fn_nid, final_ty)?;
        Some(NativeSignature {
            module: module_name.to_owned(),
            name: name.to_owned(),
            qualified: qualified.to_owned(),
            ty: final_ty,
            requirements: reqs,
            spec_only,
        })
    }
}

/// Checks for nonregular generic data type-parameter cycles (`E0084`, `mo_check_data_recursion`
/// in `src/check/recursion.c:303-381`).
pub fn check_data_recursion(
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    diags: &mut DiagBag,
) -> bool {
    struct Edge {
        target: usize,
        caller: SymRef,
        callee: SymRef,
        grows: bool,
        span: Span,
    }

    struct DataRecursion<'a, 'd> {
        interfaces: &'a ModuleInterfaces,
        signatures: &'a mut Signatures,
        diags: &'d mut DiagBag,
        span: Span,
        offsets: Vec<usize>,
        masks: Vec<u32>,
        edges: Vec<Vec<Edge>>,
        failed: bool,
    }

    impl DataRecursion<'_, '_> {
        fn report(
            &mut self,
            code: ErrorCode,
            message: String,
            reason: Option<&'static str>,
        ) -> bool {
            let mut diag =
                Diagnostic::new(code, self.span, message.clone()).with_primary_text(message);
            if let Some(r) = reason {
                diag = diag.with_reason(r);
            }
            let _ = self.diags.push(diag);
            self.failed = true;
            false
        }

        fn work(&mut self) -> bool {
            if self.failed {
                return false;
            }
            if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
                return self.report(
                    ErrorCode::CheckProgramWork,
                    "the program needs too much recursion-checking work".to_owned(),
                    None,
                );
            }
            self.signatures.work += 1;
            true
        }

        fn parameter_node(&self, nom_idx: usize, letter: usize) -> usize {
            let mask = self.masks[nom_idx];
            debug_assert!(letter < 26 && (mask & (1u32 << letter)) != 0);
            let mut node = self.offsets[nom_idx];
            for i in 0..letter {
                if (mask & (1u32 << i)) != 0 {
                    node += 1;
                }
            }
            node
        }

        fn data_edges(&mut self, caller: usize, ty_id: TypeId) -> bool {
            if !self.work() {
                return false;
            }
            if !self.signatures.signature_type_available(Some(ty_id)) {
                return true;
            }
            let (kind, owner, arg_count) = {
                let ty = self.signatures.types.get(ty_id);
                (ty.kind, ty.owner, ty.arguments.len())
            };
            if kind == SourceTypeKind::Named
                && let TypeOwner::Source(callee_sym) = owner
                && let Some((callee_id, callee_ty)) = self
                    .signatures
                    .nominal_signature_by_symbol(callee_sym)
                    .map(|(id, nom)| (id.index(), nom.ty))
            {
                let caller_sym = self.signatures.nominals[caller].symbol;
                for i in 0..arg_count {
                    let arg_id = self.signatures.types.get(ty_id).arguments[i];
                    let target_param_id = self.signatures.types.get(callee_ty).arguments[i];
                    let Some(target_letter) =
                        self.signatures.types.get(target_param_id).param_letter()
                    else {
                        continue;
                    };
                    let (arg_params, arg_kind) = {
                        let a = self.signatures.types.get(arg_id);
                        (a.parameters, a.kind)
                    };
                    for letter in 0..26 {
                        if !self.work() {
                            return false;
                        }
                        if (arg_params & (1u32 << letter)) == 0 {
                            continue;
                        }
                        let source = self.parameter_node(caller, letter);
                        let target = self.parameter_node(callee_id, target_letter);
                        self.edges[source].push(Edge {
                            target,
                            caller: caller_sym,
                            callee: callee_sym,
                            grows: arg_kind != SourceTypeKind::Parameter,
                            span: self.span,
                        });
                    }
                }
            }
            for i in 0..arg_count {
                let arg_id = self.signatures.types.get(ty_id).arguments[i];
                if !self.data_edges(caller, arg_id) {
                    return false;
                }
            }
            true
        }

        fn components(&mut self, count: usize) -> Option<Vec<u32>> {
            let mut index = vec![u32::MAX; count];
            let mut low = vec![0u32; count];
            let mut component = vec![u32::MAX; count];
            let mut stack: Vec<usize> = Vec::with_capacity(count);
            let mut frames: Vec<(usize, usize)> = Vec::with_capacity(count);
            let mut next = 0u32;
            let mut groups = 0u32;

            for start in 0..count {
                if index[start] != u32::MAX {
                    continue;
                }
                index[start] = next;
                low[start] = next;
                next += 1;
                stack.push(start);
                frames.push((start, self.edges[start].len()));

                while let Some(&(node, remaining)) = frames.last() {
                    if !self.work() {
                        return None;
                    }
                    if remaining > 0 {
                        let edge_idx = remaining - 1;
                        if let Some(top) = frames.last_mut() {
                            top.1 = edge_idx;
                        }
                        let target = self.edges[node][edge_idx].target;
                        if index[target] == u32::MAX {
                            index[target] = next;
                            low[target] = next;
                            next += 1;
                            stack.push(target);
                            frames.push((target, self.edges[target].len()));
                        } else if component[target] == u32::MAX && index[target] < low[node] {
                            low[node] = index[target];
                        }
                        continue;
                    }
                    if low[node] == index[node] {
                        while let Some(member) = stack.pop() {
                            component[member] = groups;
                            if member == node {
                                break;
                            }
                        }
                        groups += 1;
                    }
                    frames.pop();
                    if let Some(&(parent, _)) = frames.last()
                        && low[node] < low[parent]
                    {
                        low[parent] = low[node];
                    }
                }
            }
            Some(component)
        }
    }

    let nominal_count = signatures.nominals.len();
    let mut p = DataRecursion {
        interfaces,
        signatures,
        diags,
        span: Span::new(u32::MAX, 0, 0),
        offsets: vec![0; nominal_count],
        masks: vec![0; nominal_count],
        edges: Vec::new(),
        failed: false,
    };

    let mut count = 0usize;
    for i in 0..nominal_count {
        p.offsets[i] = count;
        let mask = p
            .signatures
            .types
            .get(p.signatures.nominals[i].ty)
            .parameters;
        p.masks[i] = mask;
        for j in 0..26 {
            if !p.work() {
                return false;
            }
            if (mask & (1u32 << j)) != 0 {
                count += 1;
            }
        }
    }
    if count == 0 {
        return true;
    }
    p.edges = (0..count).map(|_| Vec::new()).collect();

    let data_count = p.signatures.data.len();
    for i in 0..data_count {
        let data_ty = p.signatures.data[i].ty;
        let Some(owner_sym) = p.signatures.types.get(data_ty).owner.as_source() else {
            continue;
        };
        let Some((owner_id, invalid)) = p
            .signatures
            .nominal_signature_by_symbol(owner_sym)
            .map(|(id, nom)| (id.index(), nom.invalid))
        else {
            continue;
        };
        if invalid {
            continue;
        }
        let field_count = p.signatures.data[i].fields.len();
        for j in 0..field_count {
            let (ann_span, field_ty) = {
                let field = p.signatures.data[i].field_by_index(j);
                (field.annotation_span, field.ty)
            };
            p.span = ann_span;
            if !p.data_edges(owner_id, field_ty) {
                return false;
            }
        }
    }

    let Some(component) = p.components(count) else {
        return false;
    };

    let mut first: Option<(usize, usize)> = None;
    for i in 0..count {
        let edge_len = p.edges[i].len();
        for rev_idx in (0..edge_len).rev() {
            if !p.work() {
                return false;
            }
            let edge = &p.edges[i][rev_idx];
            if !edge.grows || component[i] != component[edge.target] {
                continue;
            }
            let replace = match first {
                None => true,
                Some((fi, fe)) => {
                    let prev = &p.edges[fi][fe];
                    edge.span.file < prev.span.file
                        || (edge.span.file == prev.span.file && edge.span.start < prev.span.start)
                }
            };
            if replace {
                first = Some((i, rev_idx));
            }
        }
    }

    let Some((fi, fe)) = first else {
        return true;
    };
    let edge = &p.edges[fi][fe];
    p.span = edge.span;
    let caller_name = truncate_128(&p.interfaces.symbol(edge.caller).name);
    let callee_name = truncate_128(&p.interfaces.symbol(edge.callee).name);
    let message = format!(
        "a generic type holds itself at a bigger type: {caller_name} holds {callee_name} in a \
         type-parameter cycle"
    );
    p.report(
        ErrorCode::DataRecursion,
        message,
        Some(
            "A generic type can't hold itself at a bigger type built from its parameter; it \
             would need types without end (reference 9.1).",
        ),
    )
}

fn truncate_128(s: &str) -> &str {
    if s.len() <= 128 {
        return s;
    }
    let mut end = 128;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

fn is_fatal_signature_failure(diags: &DiagBag) -> bool {
    if diags.dropped() != 0 || diags.is_empty() {
        return true;
    }
    diags
        .items()
        .last()
        .is_none_or(|last| last.code.number() >= 1000)
}

/// Prepares all data, nominal, function, customizer, and native signatures, allowing
/// per-declaration diagnostic recovery (`mo_prepare_available_signatures`).
#[must_use]
pub fn prepare_available_signatures(
    set: &ModuleSet,
    interfaces: &ModuleInterfaces,
    diags: &mut DiagBag,
) -> Option<Signatures> {
    debug_assert_eq!(interfaces.len(), set.len());
    let mut out = Signatures {
        types: TypeArena::new(),
        functions: Vec::new(),
        data: Vec::new(),
        nominals: Vec::new(),
        customizers: Vec::new(),
        native: Vec::new(),
        imports: Vec::new(),
        work: 0,
        recovering: false,
    };

    let mut p = Preparation {
        set,
        interfaces,
        diags,
        owner: ResolveOwner::Synthetic {
            module: ModuleId(0),
        },
        parameters: [None; 26],
        signatures: &mut out,
        span: Span::new(u32::MAX, 0, 0),
        native_span: set.modules[0].syntax.span,
        depth: 0,
        failed: false,
        native: false,
    };

    // Build import usage table in exact (module, origin.start) order.
    for (mod_idx, module) in set.modules.iter().enumerate() {
        let tree = &module.syntax.tree;
        let Some(uses_nid) = module.syntax.header.as_ref().map(|h| h.uses) else {
            continue;
        };
        for &use_opt in &tree.node(uses_nid).children {
            let Some(use_nid) = use_opt else {
                continue;
            };
            let use_node = tree.node(use_nid);
            let names_nid = use_node.children.get(1).copied().flatten();
            let added = 1u64 + names_nid.map_or(0u64, |n| tree.node(n).children.len() as u64);
            if added > MO_LIMIT_PROGRAM_CHECK_WORK - p.signatures.work {
                let _ = p.problem(
                    ErrorCode::CheckProgramWork,
                    use_node.span,
                    "the program needs too much import-checking work",
                    None,
                );
                p.diags.sort();
                return None;
            }
            p.signatures.work += added;
            let parent = p.signatures.imports.len();
            p.signatures.imports.push(ImportUsage {
                origin: use_nid,
                origin_span: use_node.span,
                module: ModuleId(mod_idx as u32),
                parent,
                used: false,
            });
            if let Some(names) = names_nid {
                for &name_opt in &tree.node(names).children {
                    let Some(name_nid) = name_opt else {
                        continue;
                    };
                    p.signatures.imports.push(ImportUsage {
                        origin: name_nid,
                        origin_span: tree.node(name_nid).span,
                        module: ModuleId(mod_idx as u32),
                        parent,
                        used: false,
                    });
                }
            }
        }
    }

    // 1. Prepare all struct and enum-case data signatures.
    for (m, scope) in interfaces.scopes.iter().enumerate() {
        for (j, sym) in scope.symbols.iter().enumerate() {
            if !matches!(sym.decl_kind, SyntaxKind::Struct | SyntaxKind::CaseDecl) {
                continue;
            }
            let sym_ref = SymRef::new(ModuleId(m as u32), crate::ids::SymIdx(j as u32));
            if let Some(data_sig) = p.data_signature(sym_ref) {
                p.signatures.data.push(data_sig);
            } else {
                if is_fatal_signature_failure(p.diags) {
                    p.diags.sort();
                    return None;
                }
                p.failed = false;
                p.signatures.recovering = true;
            }
        }
    }

    // 2. Build nominal index and compute fixed-point dependency masks.
    p.nominal_index();
    if !p.data_dependencies() {
        p.diags.sort();
        return None;
    }

    // 3. Check nonregular generic data recursion (E0084) with recovery.
    let before = p.diags.len();
    if !check_data_recursion(interfaces, p.signatures, p.diags) {
        let Some(error_span) = p
            .diags
            .items()
            .last()
            .filter(|d| {
                p.diags.dropped() == 0
                    && p.diags.len() != before
                    && d.code == ErrorCode::DataRecursion
            })
            .map(|d| d.primary.span)
        else {
            p.diags.sort();
            return None;
        };
        p.signatures.recovering = true;
        let data_count = p.signatures.data.len();
        for i in 0..data_count {
            let decl_span = interfaces
                .symbol(p.signatures.data[i].symbol)
                .declaration_span;
            if error_span.file == decl_span.file
                && error_span.start >= decl_span.start
                && error_span.end <= decl_span.end
                && let Some(owner_sym) = p
                    .signatures
                    .types
                    .get(p.signatures.data[i].ty)
                    .owner
                    .as_source()
                && let Some((nom_id, _)) = p.signatures.nominal_signature_by_symbol(owner_sym)
            {
                p.signatures.nominals[nom_id.index()].invalid = true;
            }
        }
        if !p.data_dependencies() {
            p.diags.sort();
            return None;
        }
    }

    // 4. Prepare all top-level function signatures.
    p.parameters = [None; 26];
    for (m, scope) in interfaces.scopes.iter().enumerate() {
        let tree = &set.module(ModuleId(m as u32)).syntax.tree;
        for (j, sym) in scope.symbols.iter().enumerate() {
            if sym.decl_kind != SyntaxKind::Fn {
                continue;
            }
            let fn_ref = SymRef::new(ModuleId(m as u32), crate::ids::SymIdx(j as u32));
            let sig_idx = p.signatures.functions.len();
            p.signatures.functions.push(FunctionSignature {
                function: fn_ref,
                ty: None,
                requirements: [0u8; 26],
            });
            p.owner = ResolveOwner::Symbol {
                sym: fn_ref,
                decl_kind: SyntaxKind::Fn,
            };
            p.span = sym.declaration_span;

            let fn_node = tree.node(sym.declaration);
            let resolved = (|| -> Option<(TypeId, [u8; 26])> {
                let params_nid = fn_node.children.get(1).copied().flatten()?;
                let param_children = &tree.node(params_nid).children;
                debug_assert!(param_children.len() <= MO_LIMIT_PARAMETERS as usize);

                let mut arguments = Vec::with_capacity(param_children.len() + 1);
                for &param_opt in param_children {
                    let param_nid = param_opt?;
                    let param_node = tree.node(param_nid);
                    let param_name = param_node.children.first().copied().flatten()?;
                    let param_ann = param_node.children.get(1).copied().flatten();
                    let param_ty = p.resolve(tree, param_ann)?;
                    if !p.question_name(tree, param_name, param_ty) {
                        return None;
                    }
                    arguments.push(param_ty);
                }
                let ret_nid = fn_node.children.get(2).copied().flatten();
                arguments.push(p.resolve(tree, ret_nid)?);

                let mut fn_ty = p.signatures.types.intern(
                    SourceTypeKind::Function,
                    "fn",
                    None,
                    TypeOwner::None,
                    arguments,
                );
                let fn_name_nid = fn_node.children.first().copied().flatten()?;
                if !p.question_name(tree, fn_name_nid, fn_ty) {
                    return None;
                }
                if scope.library && scope.name.as_deref() == Some("Json") && sym.name == "schema" {
                    let mut schema_ty: SourceType = p.signatures.types.get(fn_ty).clone();
                    schema_ty.parameters = 1u32 << (b'T' - b'A');
                    fn_ty = p.signatures.types.alloc_override(schema_ty);
                }
                let reqs = p.requirements(tree, sym.declaration, fn_ty)?;
                if !p.customizer(fn_ref, fn_ty) {
                    return None;
                }
                Some((fn_ty, reqs))
            })();

            match resolved {
                Some((fn_ty, reqs)) => {
                    p.signatures.functions[sig_idx].ty = Some(fn_ty);
                    p.signatures.functions[sig_idx].requirements = reqs;
                }
                None => {
                    if is_fatal_signature_failure(p.diags) {
                        p.diags.sort();
                        return None;
                    }
                    p.failed = false;
                    p.signatures.recovering = true;
                }
            }
        }
    }

    // 5. Index valid customizers sorted by (target_owner, function_name).
    let mut customizers: Vec<FuncSigId> = Vec::new();
    for (i, sig) in p.signatures.functions.iter().enumerate() {
        if sig.ty.is_none() {
            continue;
        }
        let fn_id = FuncSigId(i as u32);
        let fn_name = interfaces.symbol(sig.function).name.as_str();
        let include = fn_name == "to_text"
            || fn_name == "compare"
            || fn_name == "to_json"
            || (fn_name == "from_json"
                && p.signatures.customizer_target_owner(interfaces, fn_id) != TypeOwner::None);
        if include {
            customizers.push(fn_id);
        }
    }
    customizers.sort_by(|&a, &b| {
        let oa = p.signatures.customizer_target_owner(interfaces, a);
        let ob = p.signatures.customizer_target_owner(interfaces, b);
        let na = interfaces
            .symbol(p.signatures.functions[a.index()].function)
            .name
            .as_str();
        let nb = interfaces
            .symbol(p.signatures.functions[b.index()].function)
            .name
            .as_str();
        oa.cmp(&ob).then_with(|| na.cmp(nb))
    });
    p.signatures.customizers = customizers;

    // 6. Prepare all native signatures from mo-def PRIMS + HOSTOPS + Sqlite.in_memory + duration units.
    p.native = true;
    p.native_span = set.modules[0].syntax.span;
    let total_native = PRIMS.len() + HOSTOPS.len() + 1 + DURATION_UNITS.len();
    p.signatures.native.reserve(total_native);

    for prim in PRIMS {
        let id = NativeSigId(p.signatures.native.len() as u32);
        let qualified = format!("{}.{}", prim.module, prim.name);
        let Some(sig) = p.native_signature(
            id,
            prim.module,
            prim.name,
            &qualified,
            prim.signature,
            false,
        ) else {
            p.diags.sort();
            return None;
        };
        p.signatures.native.push(sig);
    }
    for op in HOSTOPS {
        let id = NativeSigId(p.signatures.native.len() as u32);
        let qualified = format!("{}.{}", op.module, op.name);
        let Some(sig) = p.native_signature(
            id,
            op.module,
            op.name,
            &qualified,
            op.signature,
            op.spec_only,
        ) else {
            p.diags.sort();
            return None;
        };
        p.signatures.native.push(sig);
    }
    {
        let id = NativeSigId(p.signatures.native.len() as u32);
        let Some(sig) = p.native_signature(
            id,
            "Sqlite",
            "in_memory",
            "Sqlite.fake",
            "fn() : Sqlite",
            true,
        ) else {
            p.diags.sort();
            return None;
        };
        p.signatures.native.push(sig);
    }
    for &plural in &DURATION_UNITS {
        debug_assert!(plural.len() > 1);
        let singular = &plural[..plural.len() - 1];
        let qualified = format!("Int.{plural}");
        let id = NativeSigId(p.signatures.native.len() as u32);
        let Some(sig) =
            p.native_signature(id, "Int", singular, &qualified, "fn(Int) : Duration", false)
        else {
            p.diags.sort();
            return None;
        };
        p.signatures.native.push(sig);
    }

    p.diags.sort();
    Some(out)
}

/// Prepares signatures, returning `None` if any diagnostic was recorded (`mo_prepare_signatures`).
#[must_use]
pub fn prepare_signatures(
    set: &ModuleSet,
    interfaces: &ModuleInterfaces,
    diags: &mut DiagBag,
) -> Option<Signatures> {
    let out = prepare_available_signatures(set, interfaces, diags)?;
    if diags.has_any() { None } else { Some(out) }
}

/// Resolves a `service` state / `api` auth / `api` actor type clause (`mo_resolve_service_type`).
#[must_use]
pub fn resolve_service_type(
    set: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    module: ModuleId,
    clause: NodeId,
    diags: &mut DiagBag,
) -> Option<TypeId> {
    let tree = &set.module(module).syntax.tree;
    let node = tree.node(clause);
    debug_assert!(matches!(
        node.kind,
        SyntaxKind::ServiceState | SyntaxKind::ApiAuth | SyntaxKind::ApiActor
    ));
    let target = if node.kind == SyntaxKind::ApiActor {
        node.children.get(1).copied().flatten()
    } else {
        node.children.first().copied().flatten()
    };
    let mut p = Preparation {
        set,
        interfaces,
        diags,
        owner: ResolveOwner::Synthetic { module },
        parameters: [None; 26],
        signatures,
        span: node.span,
        native_span: set.modules[0].syntax.span,
        depth: 0,
        failed: false,
        native: false,
    };
    p.resolve(tree, target)
}

fn collect_function_parameters(
    types: &TypeArena,
    ty_id: TypeId,
    owner: TypeOwner,
    out: &mut [Option<TypeId>; 26],
) {
    let ty = types.get(ty_id);
    if ty.parameters == 0 {
        return;
    }
    if ty.kind == SourceTypeKind::Parameter {
        if ty.owner == owner
            && let Some(letter) = ty.param_letter()
        {
            out[letter] = Some(ty_id);
        }
        return;
    }
    for &arg in &ty.arguments {
        collect_function_parameters(types, arg, owner, out);
    }
}

/// Resolves an explicit type annotation inside a function body (`mo_resolve_expression_type`).
///
/// Unknown single capital letters report `E0048` rather than inventing fresh function parameters.
#[must_use]
pub fn resolve_expression_type(
    set: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    enclosing: FuncSigId,
    syntax: NodeId,
    diags: &mut DiagBag,
) -> Option<TypeId> {
    let module = signatures.functions[enclosing.index()].function.module;
    resolve_expression_type_in(
        set,
        interfaces,
        signatures,
        module,
        Some(enclosing),
        syntax,
        diags,
    )
}

/// Resolves an explicit type annotation inside any module body (function, test, constant, spec,
/// or service initializer).
#[must_use]
pub fn resolve_expression_type_in(
    set: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    module: ModuleId,
    enclosing: Option<FuncSigId>,
    syntax: NodeId,
    diags: &mut DiagBag,
) -> Option<TypeId> {
    let mut parameters = [None; 26];
    if let Some(enclosing_id) = enclosing {
        let fn_sym = signatures.functions[enclosing_id.index()].function;
        let fn_ty = signatures.functions[enclosing_id.index()].ty;
        if let Some(ty_id) = fn_ty {
            collect_function_parameters(
                &signatures.types,
                ty_id,
                TypeOwner::Source(fn_sym),
                &mut parameters,
            );
        }
    }
    let tree = &set.module(module).syntax.tree;
    let span = tree.node(syntax).span;
    let mut p = Preparation {
        set,
        interfaces,
        diags,
        owner: ResolveOwner::Synthetic { module },
        parameters,
        signatures,
        span,
        native_span: set.modules[0].syntax.span,
        depth: 0,
        failed: false,
        native: false,
    };
    p.resolve(tree, Some(syntax))
}
