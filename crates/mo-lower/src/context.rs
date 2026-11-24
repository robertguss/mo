//! Shared lowering state, arena/work budgets, canonical tables, expression slots, and helpers
//! (`src/lower/internal.h` + `src/lower/lower.c` primitives).

use std::collections::HashMap;

use mo_base::{DiagBag, Diagnostic, NO_FILE, Span};
use mo_check::{
    AbilityImplementation, CheckedConstId, CheckedFuncId, CheckedProgram, CheckedSpec,
    CheckedSpecId, ModuleId, ProofId, SourceType, SourceTypeKind, SymRef, TypeArena, TypeId,
    TypeOwner,
};
use mo_core::{
    BodyMeta, ConstDecl, CoreArena, CoreInteger, CoreNatural, CtorDecl, Expr, ExprId, FnDecl,
    FnKind, FnMeta, ListOp, Lit, MapOp, Param, Prim, ServiceShape, Ty, TyId,
};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_CORE_NODES, MO_LIMIT_LOWER_WORK};
use mo_syntax::{NodeId, SyntaxNode, SyntaxTree};

/// Zero-size abort marker propagated with `?` after [`Lowering::refuse`] has recorded a
/// diagnostic in [`DiagBag`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct LowerAbort;

pub(crate) type LowerResult<T> = Result<T, LowerAbort>;

/// Sentinel [`ExprId`] written into parent expressions before a CPS slot fills them.
pub(crate) const UNSET_EXPR: ExprId = ExprId(u32::MAX);

/// Public options controlling [`crate::lower_with_options`].
#[derive(Debug, Clone)]
pub struct LowerOptions {
    /// Emit promise vacuity wrappers (`mo_lower_test_services` / `mo test` mode).
    pub promise_vacuity: bool,
    /// Populate [`crate::LoweredProgram::promises`] with closed source parameter types.
    pub collect_promises: bool,
    /// Include service descriptors and data shapes (`mo_lower_services` mode).
    pub include_services: bool,
    /// Optional zero-generic query function to reserve and report its Core function index.
    pub query: Option<CheckedFuncId>,
    /// Maximum lowering work units (`MO_LIMIT_LOWER_WORK`).
    pub work_limit: u64,
    /// Maximum simulated C arena bytes (`MO_LIMIT_LOWER_WORK`).
    pub arena_limit: usize,
}

impl Default for LowerOptions {
    fn default() -> Self {
        Self {
            promise_vacuity: false,
            collect_promises: false,
            include_services: false,
            query: None,
            work_limit: MO_LIMIT_LOWER_WORK,
            arena_limit: MO_LIMIT_LOWER_WORK as usize,
        }
    }
}

/// Exact model of `src/base/arena.c` chunked bump allocation for deterministic `E1151` parity.
#[derive(Debug, Clone)]
pub(crate) struct ArenaBudget {
    limit: usize,
    total: usize,
    chunk_size: usize,
    chunk_used: usize,
    chunk_cap: usize,
}

impl ArenaBudget {
    const FIRST_CHUNK: usize = 4096;
    const LARGEST_CHUNK: usize = 1 << 20;
    const HEADER: usize = 32;

    pub(crate) const fn new(limit: usize) -> Self {
        Self {
            limit,
            total: 0,
            chunk_size: Self::FIRST_CHUNK,
            chunk_used: 0,
            chunk_cap: 0,
        }
    }

    /// Charges an allocation of `count` elements of `size` bytes with `align` alignment.
    /// Returns `false` if the arena budget is exhausted.
    pub(crate) fn alloc(&mut self, count: usize, size: usize, align: usize) -> bool {
        let Some(raw) = count.checked_mul(size) else {
            return false;
        };
        let bytes = if raw == 0 { 1 } else { raw };
        let align = align.max(1);
        let mask = align.saturating_sub(1);

        let aligned_pos = self.chunk_used.checked_add(mask).map(|v| v & !mask);
        if let Some(pos) = aligned_pos
            && let Some(end) = pos.checked_add(bytes)
            && end <= self.chunk_cap
        {
            self.chunk_used = end;
            return true;
        }

        let Some(need) = Self::HEADER
            .checked_add(mask)
            .and_then(|v| v.checked_add(bytes))
        else {
            return false;
        };
        let remaining = self.limit.saturating_sub(self.total);
        if need > remaining {
            return false;
        }
        let mut next_chunk = self.chunk_size.max(need);
        if next_chunk > remaining {
            next_chunk = remaining;
        }
        self.total += next_chunk;
        let start = (Self::HEADER + mask) & !mask;
        self.chunk_cap = next_chunk;
        self.chunk_used = start + bytes;
        if self.chunk_size < Self::LARGEST_CHUNK {
            self.chunk_size = (self.chunk_size * 2).min(Self::LARGEST_CHUNK);
        }
        true
    }
}

/// Per-expression provenance side table paralleling `CoreArena`'s expression table
/// (`struct lower_node` in `src/lower/internal.h`).
#[derive(Debug, Clone, Default)]
pub(crate) struct NodeProvenance {
    pub(crate) span: Option<Span>,
    pub(crate) binders: Vec<Option<String>>,
    pub(crate) closure: Option<String>,
}

/// Safe write-once slot targeting either a scratch slot or a child field of an already-allocated
/// [`Expr`] inside [`CoreArena`]. Replaces raw `mo_core_expr *out` pointers in C CPS lowering.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ExprSlot {
    Root(usize),
    LetBody(ExprId),
    LamBody(ExprId),
    IfThen(ExprId),
    IfElse(ExprId),
    JoinBody(ExprId),
    JoinRest(ExprId),
    CaseArmBody(ExprId, usize),
}

/// Overlay on `&TypeArena` allowing lowering to synthesize temporary closed/built-in
/// [`SourceType`] nodes without mutating or cloning [`CheckedProgram`].
#[derive(Debug)]
pub(crate) struct TypeOverlay<'a> {
    base: &'a TypeArena,
    extra: Vec<SourceType>,
    builtin_cache: HashMap<(&'static str, Vec<TypeId>), TypeId>,
}

impl<'a> TypeOverlay<'a> {
    pub(crate) fn new(base: &'a TypeArena) -> Self {
        Self {
            base,
            extra: Vec::new(),
            builtin_cache: HashMap::new(),
        }
    }

    pub(crate) fn get(&self, id: TypeId) -> &SourceType {
        let idx = id.index();
        if idx < self.base.len() {
            self.base.get(id)
        } else {
            &self.extra[idx - self.base.len()]
        }
    }

    pub(crate) fn alloc(&mut self, ty: SourceType) -> TypeId {
        let id = TypeId((self.base.len() + self.extra.len()) as u32);
        self.extra.push(ty);
        id
    }

    pub(crate) fn into_extra(self) -> Vec<SourceType> {
        self.extra
    }

    pub(crate) fn builtin(&mut self, name: &'static str) -> TypeId {
        self.builtin_applied(name, Vec::new())
    }

    pub(crate) fn builtin_applied(&mut self, name: &'static str, args: Vec<TypeId>) -> TypeId {
        if let Some(&id) = self.builtin_cache.get(&(name, args.clone())) {
            return id;
        }
        let mut height = 0u32;
        let mut parameters = 0u32;
        for &a in &args {
            let child = self.get(a);
            height = height.max(child.height.saturating_add(1));
            parameters |= child.parameters;
        }
        let id = self.alloc(SourceType {
            kind: SourceTypeKind::Named,
            name: name.to_string(),
            module: None,
            owner: TypeOwner::None,
            arguments: args.clone(),
            height,
            parameters,
        });
        self.builtin_cache.insert((name, args), id);
        id
    }
}

/// Arena index for a [`Substitution`] stored in [`Lowering::substitutions`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct SubstId(pub(crate) u32);

impl SubstId {
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// One concrete ability binding carried inside a [`Substitution`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct AbilityBinding {
    pub(crate) parameter: TypeId,
    pub(crate) proof: ProofId,
    pub(crate) substitution: Option<SubstId>,
}

/// Monomorphization substitution environment mapping type parameters to concrete Core types
/// and scoped ability requirements to resolved proofs.
#[derive(Debug, Clone, Default)]
pub(crate) struct Substitution {
    pub(crate) parameters: Vec<TypeId>,
    pub(crate) types: Vec<TyId>,
    pub(crate) abilities: Vec<AbilityBinding>,
}

/// Canonical Core type table entry (`struct canonical_type` in `src/lower/types.c`).
#[derive(Debug, Clone)]
pub(crate) struct CoreTypeEntry {
    pub(crate) name: String,
    pub(crate) height: u32,
    pub(crate) closed_source: Option<TypeId>,
}

/// One monomorphized nominal `struct`/`enum` declaration (`struct data_instance`).
#[derive(Debug, Clone)]
pub(crate) struct DataInstance {
    pub(crate) source_ty: TypeId,
    pub(crate) owner: ModuleId,
    pub(crate) subst: Option<SubstId>,
    pub(crate) name: String,
    pub(crate) ctors: Vec<CtorDecl>,
    pub(crate) shape: ServiceShape,
}

/// Reference to a source spec item (either program-level or attached to a service/actor).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum SpecRef {
    Program(CheckedSpecId),
    ServicePromise {
        service: usize,
        promise: usize,
    },
    ServiceAnchor {
        service: usize,
        promise: usize,
        anchor: usize,
    },
}

/// One lowered or pending top-level Core function instance (`struct instance`).
#[derive(Debug, Clone)]
pub(crate) struct Instance {
    pub(crate) source: Option<CheckedFuncId>,
    pub(crate) spec: Option<SpecRef>,
    /// For a promise: concrete subject function instance; for an anchor: concrete promise instance.
    pub(crate) subject: Option<usize>,
    pub(crate) subst: Option<SubstId>,
    pub(crate) owner: ModuleId,
    pub(crate) depth: u32,
    pub(crate) index: u32,
    pub(crate) const_prefix: u32,
    pub(crate) fun: FnDecl,
    pub(crate) body_set: bool,
    pub(crate) meta: FnMeta,
}

/// Which derived helper function a [`DerivedEntry`] represents (`enum derived_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum DerivedKind {
    Text,
    Order,
    ToJson,
    FromJson,
    JsonSchema,
}

/// Memoized derived ability function (`struct derived` in `src/lower/abilities.c`).
#[derive(Debug, Clone)]
pub(crate) struct DerivedEntry {
    pub(crate) kind: DerivedKind,
    pub(crate) ty: TyId,
    pub(crate) proof: ProofId,
    pub(crate) subst: Option<SubstId>,
    pub(crate) owner: ModuleId,
    pub(crate) depth: u32,
    pub(crate) span: Span,
    pub(crate) instance: usize,
}

/// One lowered top-level constant and its metadata (`p->constants` + `p->constant_bodies`).
#[derive(Debug, Clone)]
pub(crate) struct ConstRecord {
    pub(crate) decl: ConstDecl,
    pub(crate) meta_name: String,
    pub(crate) meta_span: Option<Span>,
}

/// Complete mutable state of one `mo_lower` pass (`struct lowering`).
pub(crate) struct Lowering<'a> {
    pub(crate) program: &'a CheckedProgram,
    pub(crate) diags: &'a mut DiagBag,
    pub(crate) options: &'a LowerOptions,
    pub(crate) arena: CoreArena,
    pub(crate) provenance: Vec<NodeProvenance>,
    pub(crate) types_overlay: TypeOverlay<'a>,
    pub(crate) budget: ArenaBudget,
    pub(crate) work: u64,
    pub(crate) nodes: u32,
    pub(crate) serial: u64,
    pub(crate) span: Span,
    pub(crate) substitutions: Vec<Substitution>,

    // Canonical Core types (indexed parallel to `arena.tys`)
    pub(crate) core_types: Vec<CoreTypeEntry>,
    pub(crate) core_types_by_name: HashMap<String, TyId>,
    pub(crate) type_cache: HashMap<(TypeId, Option<SubstId>), TyId>,
    pub(crate) cache_count: u32,

    // Monomorphized data & function instances
    pub(crate) data_instances: Vec<DataInstance>,
    pub(crate) data_by_ty: HashMap<TyId, usize>,
    pub(crate) instances: Vec<Instance>,
    pub(crate) instance_index: HashMap<(CheckedFuncId, ModuleId, Vec<TyId>), usize>,
    pub(crate) func_by_sym: HashMap<SymRef, CheckedFuncId>,
    pub(crate) const_by_sym: HashMap<SymRef, CheckedConstId>,

    // Derived ability helpers
    pub(crate) derived: Vec<DerivedEntry>,
    pub(crate) derived_index: HashMap<(DerivedKind, TyId, ModuleId), usize>,
    pub(crate) derived_pending_head: usize,

    // Constants and spec indices
    pub(crate) consts: Vec<ConstRecord>,
    /// Maps program `CheckedSpecId` index to its lowered instance index (for tests/examples/non-generic).
    pub(crate) spec_instances: Vec<Option<usize>>,
    /// Per-function linked list of generic/monomorphized subject instances for promise expansion.
    pub(crate) promise_heads: Vec<Option<usize>>,
    pub(crate) promise_next: Vec<Option<usize>>,
}

impl<'a> Lowering<'a> {
    pub(crate) fn new(
        program: &'a CheckedProgram,
        options: &'a LowerOptions,
        diags: &'a mut DiagBag,
    ) -> LowerResult<Self> {
        let mut budget = ArenaBudget::new(options.arena_limit);
        if !budget.alloc(1, 256, 8) {
            let diag = Diagnostic::new(
                ErrorCode::LowerNoMemory,
                Span::NONE,
                "the lowering arena has no room",
            );
            let _ = diags.push(diag);
            return Err(LowerAbort);
        }
        Ok(Self {
            program,
            diags,
            options,
            arena: CoreArena::new(),
            provenance: Vec::new(),
            types_overlay: TypeOverlay::new(&program.signatures.types),
            budget,
            work: 0,
            nodes: 0,
            serial: 0,
            span: Span::NONE,
            substitutions: Vec::new(),
            core_types: Vec::new(),
            core_types_by_name: HashMap::new(),
            type_cache: HashMap::new(),
            cache_count: 0,
            data_instances: Vec::new(),
            data_by_ty: HashMap::new(),
            instances: Vec::new(),
            instance_index: HashMap::new(),
            func_by_sym: HashMap::new(),
            const_by_sym: HashMap::new(),
            derived: Vec::new(),
            derived_index: HashMap::new(),
            derived_pending_head: 0,
            consts: Vec::new(),
            spec_instances: Vec::new(),
            promise_heads: Vec::new(),
            promise_next: Vec::new(),
        })
    }

    pub(crate) fn refuse<T>(&mut self, code: ErrorCode, message: &'static str) -> LowerResult<T> {
        let diag = Diagnostic::new(code, self.span, message);
        let _ = self.diags.push(diag);
        Err(LowerAbort)
    }

    pub(crate) fn work(&mut self, amount: usize) -> LowerResult<()> {
        let amt = amount as u64;
        if amt > self.options.work_limit.saturating_sub(self.work) {
            return self.refuse(ErrorCode::LowerWork, "lowering exceeds its work limit");
        }
        self.work += amt;
        Ok(())
    }

    /// Charges both `work` (`count * size`) and the simulated C arena budget (`E1151`).
    pub(crate) fn charge_alloc(
        &mut self,
        count: usize,
        size: usize,
        align: usize,
    ) -> LowerResult<()> {
        debug_assert!(size != 0);
        let Some(bytes) = count.checked_mul(size) else {
            return self.refuse(
                ErrorCode::LowerNoMemory,
                "the lowering allocation is too large",
            );
        };
        self.work(bytes)?;
        if !self.budget.alloc(count, size, align) {
            return self.refuse(ErrorCode::LowerNoMemory, "the lowering arena has no room");
        }
        Ok(())
    }

    pub(crate) fn alloc_subst(&mut self, subst: Substitution) -> LowerResult<SubstId> {
        self.charge_alloc(subst.parameters.len(), 8, 8)?;
        self.charge_alloc(subst.types.len(), 8, 8)?;
        self.charge_alloc(subst.abilities.len(), 24, 8)?;
        let id = SubstId(self.substitutions.len() as u32);
        self.substitutions.push(subst);
        Ok(id)
    }

    pub(crate) fn subst(&self, id: SubstId) -> &Substitution {
        &self.substitutions[id.index()]
    }

    pub(crate) fn source_type(&self, id: TypeId) -> &SourceType {
        self.types_overlay.get(id)
    }

    pub(crate) fn proof(&self, id: ProofId) -> &AbilityImplementation {
        self.program.abilities.get(id)
    }

    pub(crate) fn module_name(&self, module: ModuleId) -> &str {
        &self.program.modules.module(module).name
    }

    pub(crate) fn module_tree(&self, module: ModuleId) -> &SyntaxTree {
        &self.program.modules.module(module).syntax.tree
    }

    pub(crate) fn syntax_node(&self, module: ModuleId, id: NodeId) -> &SyntaxNode {
        self.module_tree(module).node(id)
    }

    pub(crate) fn syntax_bytes(&self, module: ModuleId, id: NodeId) -> &[u8] {
        self.module_tree(module).node_text(id).unwrap_or(b"")
    }

    pub(crate) fn syntax_str(&self, module: ModuleId, id: NodeId) -> String {
        String::from_utf8_lossy(self.syntax_bytes(module, id)).into_owned()
    }

    pub(crate) fn spec_ref(&self, spec_ref: SpecRef) -> &CheckedSpec {
        match spec_ref {
            SpecRef::Program(id) => self.program.spec(id),
            SpecRef::ServicePromise { service, promise } => {
                &self.program.services[service].promises[promise].spec
            }
            SpecRef::ServiceAnchor {
                service,
                promise,
                anchor,
            } => &self.program.services[service].promises[promise].anchors[anchor],
        }
    }

    pub(crate) fn copy_text(&mut self, text: &str) -> LowerResult<String> {
        self.charge_alloc(text.len() + 1, 1, 1)?;
        Ok(text.to_string())
    }

    pub(crate) fn number_text(&mut self, number: u64) -> LowerResult<String> {
        let s = number.to_string();
        self.charge_alloc(s.len() + 1, 1, 1)?;
        Ok(s)
    }

    pub(crate) fn concat_text(&mut self, a: &str, sep: &str, b: &str) -> LowerResult<String> {
        let len = a.len() + sep.len() + b.len();
        self.charge_alloc(len + 1, 1, 1)?;
        let mut out = String::with_capacity(len);
        out.push_str(a);
        out.push_str(sep);
        out.push_str(b);
        Ok(out)
    }

    pub(crate) fn fresh(&mut self) -> LowerResult<String> {
        let n = self.serial;
        self.serial += 1;
        let _ = self.copy_text("%")?;
        let num = self.number_text(n)?;
        self.concat_text("%", "", &num)
    }

    pub(crate) fn lower_span(&self, span: Span) -> Option<Span> {
        if span.file == NO_FILE {
            None
        } else {
            Some(span)
        }
    }

    pub(crate) fn expr_lit_list(
        &mut self,
        elem_ty: TyId,
        items: Vec<ExprId>,
    ) -> LowerResult<ExprId> {
        let digits = self.number_text(items.len() as u64)?;
        self.expr_prim(
            Prim::List {
                ty: elem_ty,
                op: ListOp::Lit(CoreNatural { digits }),
            },
            items,
        )
    }

    pub(crate) fn expr_lit_map(
        &mut self,
        key: TyId,
        value: TyId,
        flat_items: Vec<ExprId>,
    ) -> LowerResult<ExprId> {
        let digits = self.number_text((flat_items.len() / 2) as u64)?;
        self.expr_prim(
            Prim::Map {
                key,
                value,
                op: MapOp::Lit(CoreNatural { digits }),
            },
            flat_items,
        )
    }

    /// Allocates a Core [`Expr`] together with its provenance slot and charges C-equivalent
    /// `struct lower_node` (160 bytes) + node limit (`MO_LIMIT_CORE_NODES`).
    pub(crate) fn alloc_expr(&mut self, expr: Expr) -> LowerResult<ExprId> {
        if self.nodes as u64 == MO_LIMIT_CORE_NODES {
            return self.refuse(ErrorCode::LowerWork, "lowering exceeds its Core node limit");
        }
        self.nodes += 1;
        // C `struct lower_node` is 160 bytes on 64-bit platforms (`mo_core_expr` + span + binders + closure).
        self.charge_alloc(1, 160, 8)?;
        let id = self.arena.alloc_expr(expr);
        self.provenance.push(NodeProvenance {
            span: self.lower_span(self.span),
            binders: Vec::new(),
            closure: None,
        });
        Ok(id)
    }

    pub(crate) fn set_binders(&mut self, expr: ExprId, binders: Vec<Option<String>>) {
        self.provenance[expr.0 as usize].binders = binders;
    }

    pub(crate) fn set_closure(&mut self, expr: ExprId, name: String) {
        self.provenance[expr.0 as usize].closure = Some(name);
    }

    pub(crate) fn expr_var(&mut self, name: String) -> LowerResult<ExprId> {
        self.alloc_expr(Expr::Var(name))
    }

    pub(crate) fn expr_global(&mut self, name: String) -> LowerResult<ExprId> {
        self.alloc_expr(Expr::Global(name))
    }

    pub(crate) fn expr_unit(&mut self) -> LowerResult<ExprId> {
        self.charge_alloc(1, 32, 8)?;
        self.alloc_expr(Expr::Lit(Lit::Unit))
    }

    pub(crate) fn expr_bool(&mut self, value: bool) -> LowerResult<ExprId> {
        self.charge_alloc(1, 32, 8)?;
        self.alloc_expr(Expr::Lit(Lit::Bool(value)))
    }

    pub(crate) fn expr_int(&mut self, value: u64) -> LowerResult<ExprId> {
        self.charge_alloc(1, 32, 8)?;
        let digits = self.number_text(value)?;
        self.alloc_expr(Expr::Lit(Lit::Int(CoreInteger {
            digits,
            negative: false,
        })))
    }

    pub(crate) fn expr_text(&mut self, text: &str) -> LowerResult<ExprId> {
        self.charge_alloc(1, 32, 8)?;
        let s = self.copy_text(text)?;
        self.alloc_expr(Expr::Lit(Lit::Text(s)))
    }

    pub(crate) fn expr_prim(&mut self, prim: Prim, args: Vec<ExprId>) -> LowerResult<ExprId> {
        self.charge_alloc(args.len(), 8, 8)?;
        self.alloc_expr(Expr::Prim { prim, args })
    }

    pub(crate) fn expr_call(&mut self, name: String, args: Vec<ExprId>) -> LowerResult<ExprId> {
        self.charge_alloc(args.len(), 8, 8)?;
        self.alloc_expr(Expr::Call { name, args })
    }

    pub(crate) fn expr_con(
        &mut self,
        ty: TyId,
        ctor: String,
        args: Vec<ExprId>,
    ) -> LowerResult<ExprId> {
        self.charge_alloc(args.len(), 8, 8)?;
        self.alloc_expr(Expr::Con { ty, ctor, args })
    }

    pub(crate) fn expr_let(
        &mut self,
        name: String,
        bound: ExprId,
        body: ExprId,
    ) -> LowerResult<ExprId> {
        self.alloc_expr(Expr::Let { name, bound, body })
    }

    pub(crate) fn expr_if(
        &mut self,
        cond: ExprId,
        then_: ExprId,
        else_: ExprId,
    ) -> LowerResult<ExprId> {
        self.alloc_expr(Expr::If { cond, then_, else_ })
    }

    pub(crate) fn expr_stop(&mut self, msg: &str) -> LowerResult<ExprId> {
        let s = self.copy_text(msg)?;
        self.alloc_expr(Expr::Stop(s))
    }

    pub(crate) fn expr_tuple(&mut self, items: Vec<ExprId>) -> LowerResult<ExprId> {
        self.charge_alloc(items.len(), 8, 8)?;
        self.alloc_expr(Expr::Tuple(items))
    }

    pub(crate) fn expr_proj(&mut self, expr: ExprId, index: u64) -> LowerResult<ExprId> {
        let digits = self.number_text(index)?;
        self.alloc_expr(Expr::Proj {
            expr,
            index: CoreNatural { digits },
        })
    }

    pub(crate) fn expr_fail(&mut self, inner: ExprId) -> LowerResult<ExprId> {
        self.alloc_expr(Expr::Fail(inner))
    }

    /// Writes `value` into `slot` (either into `roots` or directly into an [`Expr`] in `arena`).
    pub(crate) fn write_slot(
        &mut self,
        roots: &mut [Option<ExprId>],
        slot: ExprSlot,
        value: ExprId,
    ) {
        match slot {
            ExprSlot::Root(idx) => roots[idx] = Some(value),
            ExprSlot::LetBody(id) => {
                if let Expr::Let { body, .. } = self.arena.expr_mut(id) {
                    *body = value;
                }
            }
            ExprSlot::LamBody(id) => {
                if let Expr::Lam { body, .. } = self.arena.expr_mut(id) {
                    *body = value;
                }
            }
            ExprSlot::IfThen(id) => {
                if let Expr::If { then_, .. } = self.arena.expr_mut(id) {
                    *then_ = value;
                }
            }
            ExprSlot::IfElse(id) => {
                if let Expr::If { else_, .. } = self.arena.expr_mut(id) {
                    *else_ = value;
                }
            }
            ExprSlot::JoinBody(id) => {
                if let Expr::Join { body, .. } = self.arena.expr_mut(id) {
                    *body = value;
                }
            }
            ExprSlot::JoinRest(id) => {
                if let Expr::Join { rest, .. } = self.arena.expr_mut(id) {
                    *rest = value;
                }
            }
            ExprSlot::CaseArmBody(id, arm_idx) => {
                if let Expr::Case { arms, .. } = self.arena.expr_mut(id) {
                    arms[arm_idx].body = value;
                }
            }
        }
    }

    /// Returns the `at`-th child of `expr` in canonical Core preorder (`lower_child`).
    pub(crate) fn expr_child(&self, expr: ExprId, at: usize) -> Option<ExprId> {
        match self.arena.expr(expr) {
            Expr::Lit(_) | Expr::Var(_) | Expr::Global(_) | Expr::Stop(_) => None,
            Expr::Let { bound, body, .. } => match at {
                0 => Some(*bound),
                1 => Some(*body),
                _ => None,
            },
            Expr::Lam { body, .. } => (at == 0).then_some(*body),
            Expr::App { func, args } => {
                if at == 0 {
                    Some(*func)
                } else {
                    args.get(at - 1).copied()
                }
            }
            Expr::Call { args, .. }
            | Expr::Tuple(args)
            | Expr::Con { args, .. }
            | Expr::Prim { args, .. }
            | Expr::Jump { args, .. } => args.get(at).copied(),
            Expr::Proj { expr: inner, .. } | Expr::Fail(inner) => (at == 0).then_some(*inner),
            Expr::Case { expr: scrut, arms } => {
                if at == 0 {
                    Some(*scrut)
                } else {
                    arms.get(at - 1).map(|arm| arm.body)
                }
            }
            Expr::If { cond, then_, else_ } => match at {
                0 => Some(*cond),
                1 => Some(*then_),
                2 => Some(*else_),
                _ => None,
            },
            Expr::Join { body, rest, .. } => match at {
                0 => Some(*body),
                1 => Some(*rest),
                _ => None,
            },
        }
    }

    /// Allocates a new top-level [`Instance`] slot in `self.instances`, enforcing `MO_LIMIT_CORE_NODES`.
    pub(crate) fn push_instance(
        &mut self,
        owner: ModuleId,
        name: String,
        kind: FnKind,
        ret: TyId,
        meta_name: String,
        meta_span: Option<Span>,
    ) -> LowerResult<usize> {
        if self.instances.len() as u64 >= MO_LIMIT_CORE_NODES {
            return self.refuse(
                ErrorCode::LowerWork,
                "lowering exceeds its function instance limit",
            );
        }
        self.charge_alloc(1, 256, 8)?;
        let index = self.instances.len() as u32;
        let mod_name = self.module_name(owner).to_string();
        let module = self.copy_text(&mod_name)?;
        self.instances.push(Instance {
            source: None,
            spec: None,
            subject: None,
            subst: None,
            owner,
            depth: 0,
            index,
            const_prefix: 0,
            fun: FnDecl {
                name,
                params: Vec::new(),
                ret,
                body: UNSET_EXPR,
                module,
                kind,
                const_count: CoreNatural::zero(),
            },
            body_set: false,
            meta: FnMeta {
                name: meta_name,
                span: meta_span,
                text: None,
                body: BodyMeta::default(),
                subject: None,
                allows: None,
                lines: Vec::new(),
            },
        });
        Ok(index as usize)
    }

    /// Convenience helper to create a Core [`Param`] and charge its allocation.
    pub(crate) fn make_param(&mut self, name: String, ty: TyId) -> LowerResult<Param> {
        self.charge_alloc(1, 24, 8)?;
        Ok(Param { name, ty })
    }

    /// Returns the Core [`Ty`] corresponding to `ty_id`.
    pub(crate) fn core_ty(&self, ty_id: TyId) -> &Ty {
        self.arena.ty(ty_id)
    }

    /// Returns the declaring [`ModuleId`] for a [`SpecRef`].
    pub(crate) fn spec_module(&self, spec_ref: SpecRef) -> ModuleId {
        self.spec_ref(spec_ref).module
    }
}
