//! Core compiler state, index-based node arena, and shared helpers matching
//! `src/codegen/internal.h` and `src/codegen/compile.c`.

use std::collections::HashMap;
use std::fmt;

use mo_base::Span;
use mo_bytecode::{
    AnchorExpectation, BigIntSign, CaseShape, CodeSpan, ConstantDef, ConstantValue, DebugEntry,
    FunctionDef, GlobalDef, LayoutDef, LayoutKind, ModuleDef, NO_INDEX, RegisterName, ServiceShape,
    TypeCase, TypeDef, TypeField, TypeKind, types::compute_case_shape,
};
use mo_core::{BodyMeta, CoreArena, ExprId, FnDecl, FnMeta, Metadata, Program, Services, TyId};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_CHOICE_CASES, MO_LIMIT_CORE_NODES, MO_LIMIT_FIELDS, MO_LIMIT_LAYOUTS,
    MO_LIMIT_STRING_SIZE, MO_LIMIT_STRINGS, MO_LIMIT_TYPES,
};

use crate::operations::Operation;

/// Typed compilation error produced by `mo-codegen`, carrying the exact `ErrorCode`,
/// diagnostic headline, technical reason, optional failing function name, and source span.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodegenError {
    pub code: ErrorCode,
    pub message: &'static str,
    pub reason: &'static str,
    pub function_name: Option<String>,
    pub span: Span,
}

impl CodegenError {
    #[must_use]
    pub const fn headline_for(code: ErrorCode) -> &'static str {
        match code {
            ErrorCode::CodegenEntry => "invalid lowered entry name: bug in Mo",
            ErrorCode::CodegenRepresentation => "invalid lowered representation: bug in Mo",
            ErrorCode::CodegenInvalid => "code generation produced invalid bytecode: bug in Mo",
            ErrorCode::CodegenUnsupported => {
                "Core code generation is not implemented for this form yet"
            }
            _ => "Core code generation reached a resource limit",
        }
    }
}

impl fmt::Display for CodegenError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.function_name {
            Some(name) => write!(f, "{} {name}: {}", self.code, self.reason),
            None => write!(f, "{} {}", self.code, self.reason),
        }
    }
}

impl std::error::Error for CodegenError {}

/// Arena index for a lowered codegen IR [`Node`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NodeId(pub u32);

/// Reference ownership state of a virtual register during code emission (`enum ref_state`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RefState {
    Undefined,
    Borrowed,
    Owned,
    Token,
    Moved,
}

/// Per-virtual-register metadata inside one function (`struct reg`).
#[derive(Debug, Clone, Copy)]
pub struct RegInfo {
    pub ty: u32,
    pub name: u32,
    pub flow: u32,
    pub previous: u32,
    pub next: u32,
    pub mark: u32,
    pub token_layout: u32,
    pub token_depth: u32,
    pub state: RefState,
    pub goal: RefState,
    pub owned: bool,
    pub protected: bool,
}

/// Undo-log entry for a single register reference-state transition (`struct change`).
#[derive(Debug, Clone, Copy)]
pub struct Change {
    pub reg: u32,
    pub previous: RefState,
}

/// Ownership flow graph node (`struct ownership`).
#[derive(Debug, Clone, Copy)]
pub struct OwnershipSlot {
    pub ty: u32,
    pub edges: u32,
    pub owned: bool,
}

/// Adjacency list edge in the ownership graph (`struct edge`).
#[derive(Debug, Clone, Copy)]
pub struct Edge {
    pub to: u32,
    pub next: u32,
}

/// Captured variable mapping from enclosing function register to closure register (`struct capture`).
#[derive(Debug, Clone, Copy)]
pub struct Capture {
    pub source: u32,
    pub reg: u32,
}

/// Deferred ability check collected during signature instantiation (`struct constraint`).
#[derive(Debug, Clone)]
pub struct Constraint {
    pub ty: u32,
    pub ability: u8,
    pub function_name: Option<String>,
    pub span: Option<Span>,
    pub declaration: Option<Span>,
}

/// Sorted set of live reference virtual registers (`struct live_set`).
#[derive(Debug, Clone, Default)]
pub struct LiveSet {
    pub items: Vec<u32>,
    pub capacity: u32,
}

impl LiveSet {
    #[must_use]
    pub fn position(&self, r: u32) -> usize {
        self.items.partition_point(|&x| x < r)
    }

    #[must_use]
    pub fn has(&self, r: u32) -> bool {
        let at = self.position(r);
        at < self.items.len() && self.items[at] == r
    }
}

/// Prepared expression node in the codegen arena (`struct node`).
#[derive(Debug, Clone)]
pub struct Node {
    pub core: ExprId,
    pub span: Option<Span>,
    pub ty: u32,
    pub reg: u32,
    pub index: u32,
    pub children: Vec<NodeId>,
    pub binds: Vec<u32>,
    pub cases: Vec<u32>,
    pub operation: Operation,
    pub signature: u32,
    pub last: Vec<bool>,
    pub after: LiveSet,
    pub work: LiveSet,
    pub temporary: bool,
    pub scrutinee_dead: bool,
}

/// Lexical `join` point state within one function (`struct join`).
#[derive(Debug, Clone)]
pub struct JoinState {
    pub node: NodeId,
    pub offset: u32,
    pub checkpoint: u32,
    pub live: LiveSet,
    pub used: Vec<bool>,
    pub reachable: bool,
}

/// Synthetic metadata/source reference for a function or constant helper.
#[derive(Debug, Clone)]
pub enum SourceKind {
    CoreFun(usize),
    CoreConst(usize),
}

/// Per-function compilation state (`struct function`).
#[derive(Debug, Clone)]
pub struct FunctionState {
    pub regs: Vec<RegInfo>,
    pub code: Vec<u32>,
    pub spans: Vec<CodeSpan>,
    pub names: Vec<RegisterName>,
    pub captures: Vec<Capture>,
    pub joins: Vec<JoinState>,
    pub changes: Vec<Change>,
    pub body: Option<NodeId>,
    pub locals: HashMap<String, u32>,
    pub bindings: HashMap<(u32, u32), u32>,
    pub zeros: HashMap<u32, u32>,
    pub closure_names: HashMap<u32, u32>,
    pub capture_index: HashMap<u32, u32>,
    pub join_names: HashMap<String, u32>,
    pub source_kind: Option<SourceKind>,
    pub parent: Option<usize>,
    pub root: usize,
    pub declaration: Option<Span>,
    pub lambda_number: u32,
    pub closure_type: u32,
    pub preorder: u32,
    pub instructions: u32,
    pub owned: u32,
    pub mark: u32,
    pub branch_depth: u32,
    pub live_count: u32,
    pub live_cells: u32,
    pub references: Vec<u32>,
    pub live_work: u64,
}

impl FunctionState {
    #[must_use]
    pub fn new(root: usize) -> Self {
        Self {
            regs: Vec::new(),
            code: Vec::new(),
            spans: Vec::new(),
            names: Vec::new(),
            captures: Vec::new(),
            joins: Vec::new(),
            changes: Vec::new(),
            body: None,
            locals: HashMap::new(),
            bindings: HashMap::new(),
            zeros: HashMap::new(),
            closure_names: HashMap::new(),
            capture_index: HashMap::new(),
            join_names: HashMap::new(),
            source_kind: None,
            parent: None,
            root,
            declaration: None,
            lambda_number: 0,
            closure_type: NO_INDEX,
            preorder: 0,
            instructions: 0,
            owned: NO_INDEX,
            mark: 0,
            branch_depth: 0,
            live_count: 0,
            live_cells: 0,
            references: Vec::new(),
            live_work: 0,
        }
    }
}

/// Structural type key for `TypeBuilder::intern` deduplication.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct StructuralKey {
    kind: TypeKind,
    result: u32,
    function: u32,
    capability: u32,
    parameters: Vec<u32>,
}

/// Incremental bytecode type & layout builder mirroring `struct mo_types` (`src/heap/types.c`).
#[derive(Debug, Default)]
pub struct TypeBuilder {
    pub items: Vec<TypeDef>,
    pub layouts: Vec<LayoutDef>,
    structural: HashMap<StructuralKey, u32>,
    case_layouts: HashMap<(u32, u32), u32>,
    pub finished: bool,
}

impl TypeBuilder {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn intern(&mut self, mut def: TypeDef) -> Result<u32, ErrorCode> {
        if self.finished {
            return Err(ErrorCode::CodegenInvalid);
        }
        let key = StructuralKey {
            kind: def.kind,
            result: def.result,
            function: def.function,
            capability: def.capability.map_or(0, |c| c as u32),
            parameters: def.parameters.clone(),
        };
        if let Some(&id) = self.structural.get(&key) {
            return Ok(id);
        }
        if (self.items.len() as u64) >= MO_LIMIT_TYPES {
            return Err(ErrorCode::BytecodeTooBig);
        }
        let id = self.items.len() as u32;
        def.is_reference = (def.kind as u32) >= (TypeKind::Text as u32);
        self.items.push(def);
        self.structural.insert(key, id);
        Ok(id)
    }

    pub fn declare(&mut self, kind: TypeKind, name: String) -> Result<u32, ErrorCode> {
        if self.finished || !matches!(kind, TypeKind::Record | TypeKind::Choice) {
            return Err(ErrorCode::CodegenInvalid);
        }
        if (self.items.len() as u64) >= MO_LIMIT_TYPES {
            return Err(ErrorCode::BytecodeTooBig);
        }
        let id = self.items.len() as u32;
        self.items.push(TypeDef {
            kind,
            parameters: Vec::new(),
            result: 0,
            function: 0,
            capability: None,
            name_idx: None,
            name: Some(name),
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: kind == TypeKind::Record,
            has_fields: false,
        });
        Ok(id)
    }

    pub fn define_record(&mut self, id: u32, fields: Vec<TypeField>) -> Result<(), ErrorCode> {
        if self.finished || (fields.len() as u64) > MO_LIMIT_FIELDS {
            return Err(ErrorCode::BytecodeTooBig);
        }
        let Some(t) = self.items.get_mut(id as usize) else {
            return Err(ErrorCode::CodegenInvalid);
        };
        if t.kind != TypeKind::Record {
            return Err(ErrorCode::CodegenInvalid);
        }
        t.has_fields = !fields.is_empty();
        t.fields = fields;
        Ok(())
    }

    pub fn define_choice(&mut self, id: u32, cases: Vec<TypeCase>) -> Result<(), ErrorCode> {
        if self.finished {
            return Err(ErrorCode::CodegenInvalid);
        }
        if cases.is_empty() {
            return Err(ErrorCode::CodegenInvalid);
        }
        if (cases.len() as u64) > MO_LIMIT_CHOICE_CASES {
            return Err(ErrorCode::BytecodeTooBig);
        }
        let Some(t) = self.items.get_mut(id as usize) else {
            return Err(ErrorCode::CodegenInvalid);
        };
        if t.kind != TypeKind::Choice {
            return Err(ErrorCode::CodegenInvalid);
        }
        t.has_fields = cases.iter().any(|c| !c.fields.is_empty());
        t.cases = cases;
        Ok(())
    }

    pub fn finish(&mut self) -> Result<(), ErrorCode> {
        if self.finished {
            return Err(ErrorCode::CodegenInvalid);
        }
        let is_refs: Vec<bool> = (0..self.items.len())
            .map(|i| {
                let t = &self.items[i];
                if t.kind == TypeKind::Choice {
                    (0..t.cases.len()).any(|j| {
                        matches!(
                            compute_case_shape(&self.items, i, j),
                            CaseShape::Ref | CaseShape::Boxed
                        )
                    })
                } else {
                    (t.kind as u32) >= (TypeKind::Text as u32)
                }
            })
            .collect();
        for (item, is_ref) in self.items.iter_mut().zip(is_refs) {
            item.is_reference = is_ref;
        }
        self.finished = true;
        Ok(())
    }

    pub fn add_layout(&mut self, layout: LayoutDef) -> Result<u32, ErrorCode> {
        if !self.finished {
            return Err(ErrorCode::CodegenInvalid);
        }
        if (self.layouts.len() as u64) >= MO_LIMIT_LAYOUTS {
            return Err(ErrorCode::BytecodeTooBig);
        }
        let id = self.layouts.len() as u32;
        if layout.kind == LayoutKind::Case {
            self.case_layouts.insert((layout.ty, layout.case_index), id);
        }
        self.layouts.push(layout);
        Ok(id)
    }

    #[must_use]
    pub fn is_reference(&self, ty: u32) -> bool {
        ty != NO_INDEX && self.items[ty as usize].is_reference
    }

    #[must_use]
    pub fn case_shape(&self, ty: u32, case_idx: u32) -> CaseShape {
        compute_case_shape(&self.items, ty as usize, case_idx as usize)
    }

    #[must_use]
    pub fn find_case_layout(&self, ty: u32, case_idx: u32) -> Option<u32> {
        self.case_layouts.get(&(ty, case_idx)).copied()
    }
}

/// Hash key for constant deduplication matching `codegen_constant` in `src/codegen/types.c`.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum ConstantKey {
    Immediate64(u32, [u8; 8]),
    Decimal128(u32, [u8; 16]),
    Text(u32, u32),
    Closure(u32, u32),
    Bytes(u32, Vec<u8>),
    BigInt(u32, BigIntSign, Vec<u8>),
}

impl ConstantKey {
    #[must_use]
    pub fn from_def(k: &ConstantDef) -> Self {
        match &k.value {
            ConstantValue::Immediate64(b) => Self::Immediate64(k.ty, *b),
            ConstantValue::Decimal128(b) => Self::Decimal128(k.ty, *b),
            ConstantValue::Text(s) => Self::Text(k.ty, *s),
            ConstantValue::Closure(f) => Self::Closure(k.ty, *f),
            ConstantValue::Bytes(b) => Self::Bytes(k.ty, b.clone()),
            ConstantValue::BigInt { sign, magnitude } => {
                Self::BigInt(k.ty, *sign, magnitude.clone())
            }
        }
    }
}

/// Whole-program compiler context (`struct compiler`).
pub struct Compiler<'a> {
    pub arena: &'a CoreArena,
    pub core: &'a Program,
    pub services: Option<&'a Services>,
    pub types: TypeBuilder,
    pub nodes: Vec<Node>,
    pub strings: Vec<String>,
    pub string_index: HashMap<String, u32>,
    pub constants: Vec<ConstantDef>,
    pub constant_index: HashMap<ConstantKey, u32>,
    pub bigint_index: [HashMap<String, u32>; 2],
    pub modules: Vec<ModuleDef>,
    pub module_index: HashMap<String, u32>,
    pub data: Vec<u32>,
    pub data_index: HashMap<String, u32>,
    pub fun_index: HashMap<String, u32>,
    pub global_index: HashMap<String, u32>,
    pub builtin_index: HashMap<(u8, u32, u32), u32>,
    pub annotation_index: HashMap<TyId, u32>,
    pub operation_index: HashMap<[u32; 7], u32>,
    pub constructors: Vec<HashMap<String, u32>>,
    pub layouts: Vec<u32>,
    pub files: Vec<Vec<u8>>,
    pub range_int: u32,
    pub function_count: u32,
    pub function_capacity: u32,
    pub unit_constant: u32,
    pub globals: Vec<GlobalDef>,
    pub init: Vec<u32>,
    pub funs: Vec<FunctionDef>,
    pub debug: Vec<DebugEntry>,
    pub bodies: Vec<FunctionState>,
    pub ownership: Vec<OwnershipSlot>,
    pub edges: Vec<Edge>,
    pub constraints: Vec<Constraint>,
    pub shapes: Vec<ServiceShape>,
    pub debug_entries: u32,
    pub current_name: Option<String>,
    pub span: Option<Span>,
    pub declaration: Option<Span>,
    pub error: Option<(ErrorCode, &'static str)>,
    pub bigint_work: usize,
    #[cfg(test)]
    pub ownership_slot_limit: u64,
}

impl<'a> Compiler<'a> {
    #[must_use]
    pub fn new(arena: &'a CoreArena, core: &'a Program, services: Option<&'a Services>) -> Self {
        Self {
            arena,
            core,
            services,
            types: TypeBuilder::new(),
            nodes: Vec::new(),
            strings: Vec::new(),
            string_index: HashMap::new(),
            constants: Vec::new(),
            constant_index: HashMap::new(),
            bigint_index: [HashMap::new(), HashMap::new()],
            modules: Vec::new(),
            module_index: HashMap::new(),
            data: Vec::new(),
            data_index: HashMap::new(),
            fun_index: HashMap::new(),
            global_index: HashMap::new(),
            builtin_index: HashMap::new(),
            annotation_index: HashMap::new(),
            operation_index: HashMap::new(),
            constructors: Vec::new(),
            layouts: Vec::new(),
            files: Vec::new(),
            range_int: NO_INDEX,
            function_count: 0,
            function_capacity: 0,
            unit_constant: NO_INDEX,
            globals: Vec::new(),
            init: Vec::new(),
            funs: Vec::new(),
            debug: Vec::new(),
            bodies: Vec::new(),
            ownership: Vec::new(),
            edges: Vec::new(),
            constraints: Vec::new(),
            shapes: Vec::new(),
            debug_entries: 0,
            current_name: None,
            span: None,
            declaration: None,
            error: None,
            bigint_work: 0,
            #[cfg(test)]
            ownership_slot_limit: mo_def::limits::MO_LIMIT_CODEGEN_OWNERSHIP_SLOTS,
        }
    }

    pub fn fail(&mut self, code: ErrorCode, reason: &'static str) -> bool {
        if self.error.is_none() {
            self.error = Some((code, reason));
        }
        false
    }

    pub fn fail_unsupported(&mut self, reason: &'static str) -> bool {
        self.fail(ErrorCode::CodegenUnsupported, reason)
    }

    pub fn alloc_node(&mut self, node: Node) -> NodeId {
        let id = NodeId(self.nodes.len() as u32);
        self.nodes.push(node);
        id
    }

    #[must_use]
    pub fn node(&self, id: NodeId) -> &Node {
        &self.nodes[id.0 as usize]
    }

    pub fn node_mut(&mut self, id: NodeId) -> &mut Node {
        &mut self.nodes[id.0 as usize]
    }

    #[must_use]
    pub fn has_source_meta(&self) -> bool {
        matches!(self.core.metadata, Metadata::Source(_))
    }

    #[must_use]
    pub fn fn_meta_for(&self, func_idx: usize) -> Option<&'a FnMeta> {
        let Metadata::Source(meta) = &self.core.metadata else {
            return None;
        };
        match self.bodies[func_idx].source_kind.as_ref()? {
            SourceKind::CoreFun(idx) => meta.funs.get(*idx),
            SourceKind::CoreConst(_) => None,
        }
    }

    #[must_use]
    pub fn body_meta_for(&self, func_idx: usize) -> Option<&'a BodyMeta> {
        let Metadata::Source(meta) = &self.core.metadata else {
            return None;
        };
        match self.bodies[func_idx].source_kind.as_ref()? {
            SourceKind::CoreFun(idx) => meta.funs.get(*idx).map(|m| &m.body),
            SourceKind::CoreConst(idx) => meta.consts.get(*idx).map(|m| &m.body),
        }
    }

    #[must_use]
    pub fn source_name_for(&self, func_idx: usize) -> &'a str {
        match self.bodies[func_idx].source_kind.as_ref() {
            Some(SourceKind::CoreFun(idx)) => &self.core.funs[*idx].name,
            Some(SourceKind::CoreConst(idx)) => &self.core.consts[*idx].name,
            None => "",
        }
    }

    #[must_use]
    pub fn source_body_for(&self, func_idx: usize) -> ExprId {
        match self.bodies[func_idx].source_kind.as_ref() {
            Some(SourceKind::CoreFun(idx)) => self.core.funs[*idx].body,
            Some(SourceKind::CoreConst(idx)) => self.core.consts[*idx].body,
            None => ExprId(0),
        }
    }

    #[must_use]
    pub fn source_fun_for(&self, func_idx: usize) -> Option<&'a FnDecl> {
        match self.bodies[func_idx].source_kind.as_ref() {
            Some(SourceKind::CoreFun(idx)) => Some(&self.core.funs[*idx]),
            _ => None,
        }
    }

    pub fn intern_string(&mut self, s: &str) -> Option<u32> {
        if let Some(&id) = self.string_index.get(s) {
            return Some(id);
        }
        if (s.len() as u64) > MO_LIMIT_STRING_SIZE {
            self.fail(
                ErrorCode::BytecodeTooBig,
                "A generated string exceeds MO_LIMIT_STRING_SIZE.",
            );
            return None;
        }
        if (self.strings.len() as u64) >= MO_LIMIT_STRINGS {
            self.fail(
                ErrorCode::BytecodeTooBig,
                "Code generation exceeds the corresponding bytecode table or function limit.",
            );
            return None;
        }
        let id = self.strings.len() as u32;
        let owned = s.to_string();
        self.strings.push(owned.clone());
        self.string_index.insert(owned, id);
        Some(id)
    }

    pub fn validate_name(&mut self, s: &str) -> bool {
        if !s.is_empty() && !s.as_bytes().contains(&0) {
            true
        } else {
            self.fail(
                ErrorCode::CodegenRepresentation,
                "Empty or NUL-containing Core names cannot come from lowering.",
            )
        }
    }

    pub fn intern_name(&mut self, s: &str) -> Option<String> {
        if !self.validate_name(s) {
            return None;
        }
        self.intern_string(s)?;
        Some(s.to_string())
    }

    pub fn intern_suffixed_name(&mut self, base: &str, suffix: &str) -> Option<u32> {
        if !self.validate_name(base) {
            return None;
        }
        let extra = suffix.len();
        if (base.len() as u64) > MO_LIMIT_STRING_SIZE - (extra as u64) {
            self.fail(
                ErrorCode::BytecodeTooBig,
                "Generated initializer name exceeds MO_LIMIT_STRING_SIZE.",
            );
            return None;
        }
        let mut combined = String::with_capacity(base.len() + extra);
        combined.push_str(base);
        combined.push_str(suffix);
        self.intern_string(&combined)
    }

    pub fn record_service_shape(&mut self, ty: u32, is_struct: bool, unnamed: Vec<bool>) -> bool {
        if self.services.is_none_or(|s| s.items.is_empty()) {
            return true;
        }
        if (self.shapes.len() as u64) >= MO_LIMIT_TYPES {
            return self.fail(
                ErrorCode::BytecodeTooBig,
                "Code generation exceeds the corresponding bytecode table or function limit.",
            );
        }
        self.shapes.push(ServiceShape {
            ty,
            is_struct,
            unnamed,
        });
        true
    }

    pub fn add_register(&mut self, func_idx: usize, ty: u32, original: u32) -> Option<u32> {
        if ty == NO_INDEX {
            return None;
        }
        #[cfg(test)]
        let limit = self.ownership_slot_limit;
        #[cfg(not(test))]
        let limit = mo_def::limits::MO_LIMIT_CODEGEN_OWNERSHIP_SLOTS;
        if (self.bodies[func_idx].regs.len() as u64) >= limit
            || (self.ownership.len() as u64) >= limit
        {
            self.fail(
                ErrorCode::CodegenMemory,
                "Code generation exceeds its ownership graph limit.",
            );
            return None;
        }
        let id = self.bodies[func_idx].regs.len() as u32;
        let flow = self.ownership.len() as u32;
        self.ownership.push(OwnershipSlot {
            ty,
            edges: NO_INDEX,
            owned: false,
        });
        self.bodies[func_idx].regs.push(RegInfo {
            ty,
            name: original,
            flow,
            previous: NO_INDEX,
            next: NO_INDEX,
            mark: 0,
            token_layout: NO_INDEX,
            token_depth: 0,
            state: RefState::Undefined,
            goal: RefState::Undefined,
            owned: false,
            protected: false,
        });
        Some(id)
    }

    #[must_use]
    pub fn flow_id(&self, func_idx: usize, reg: u32) -> u32 {
        if reg == NO_INDEX {
            NO_INDEX
        } else {
            self.bodies[func_idx].regs[reg as usize].flow
        }
    }

    #[must_use]
    pub fn is_reference(&self, ty: u32) -> bool {
        self.types.is_reference(ty)
    }

    pub fn push_constraint(&mut self, ty: u32, ability: u8) -> bool {
        if (self.constraints.len() as u64) >= MO_LIMIT_CORE_NODES {
            return self.fail(
                ErrorCode::CodegenMemory,
                "Code generation exceeds its ownership graph limit.",
            );
        }
        self.constraints.push(Constraint {
            ty,
            ability,
            function_name: self.current_name.clone(),
            span: self.span,
            declaration: self.declaration,
        });
        true
    }

    #[must_use]
    pub fn empty_debug() -> DebugEntry {
        DebugEntry {
            spans: Vec::new(),
            names: Vec::new(),
            text: NO_INDEX,
            file: NO_INDEX,
            start: 0,
            end: 0,
            subject: NO_INDEX,
            expectation: AnchorExpectation::NotAnchor,
            lines: Vec::new(),
        }
    }
}
