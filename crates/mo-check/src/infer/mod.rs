//! Bidirectional local type inference, constraint solving, pattern normalization, and
//! checked-body construction (`src/check/{infer*.c,bodies.c}` parity).

pub mod bodies;
mod calls;
mod dispatch;
mod expr;
mod finish;
mod names;
mod patterns;
mod terms;

use std::collections::HashMap;

use mo_base::{DiagBag, Diagnostic, Edit, Fix, Label, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_CHECK_WORK};
use mo_syntax::{NodeId, SyntaxKind, SyntaxNode, SyntaxTree, TokenKind};

use crate::abilities::{AbilityArena, AbilityCache, AbilityContext, TypeArgument};
use crate::checked::{CheckedAbilityUse, CheckedOr, ConstructorRef, InferredExpression};
use crate::ids::{
    CheckedPatternId, CheckedServiceId, DataSigId, FuncSigId, ModuleId, SymRef, TypeId,
};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::patterns::{MatchPatternArena, MatchPatternId};
use crate::signatures::Signatures;
use crate::types::{SourceTypeKind, TypeOwner};

pub(crate) use bodies::check_spec_body;
pub use bodies::{
    ConstantContext, ProgramServicesView, check_constant, check_function, check_service_value,
    infer_function,
};

/// Numeric literal candidate bitmask (`enum numeric`).
pub(crate) const NUM_INT: u8 = 1;
pub(crate) const NUM_BIG: u8 = 2;
pub(crate) const NUM_BYTE: u8 = 4;
pub(crate) const NUM_DECIMAL: u8 = 8;
pub(crate) const NUM_FLOAT: u8 = 16;
pub(crate) const NUM_ARITH: u8 = NUM_INT | NUM_BIG | NUM_DECIMAL | NUM_FLOAT;
pub(crate) const NUM_ALL: u8 = NUM_INT | NUM_BIG | NUM_BYTE | NUM_DECIMAL | NUM_FLOAT;

/// C-equivalent struct byte sizes on 64-bit Linux for `MO_CLI_CHECK_SIZE` (`E1058`) parity.
pub(crate) const SIZEOF_TERM: usize = 104;
pub(crate) const SIZEOF_NOTE: usize = 112;
pub(crate) const SIZEOF_EMITTED: usize = 256;
pub(crate) const SIZEOF_SOURCE_TYPE: usize = 56;
pub(crate) const SIZEOF_CHECKED_EXPR: usize = 160;
pub(crate) const SIZEOF_CHECKED_BINDING: usize = 48;
pub(crate) const SIZEOF_LOCAL: usize = 80;
pub(crate) const SIZEOF_NAME_SLOT: usize = 64;
pub(crate) const SIZEOF_DISPATCH: usize = 200;
pub(crate) const SIZEOF_FAILURE: usize = 112;
pub(crate) const SIZEOF_TYPED_PATTERN: usize = 72;
pub(crate) const SIZEOF_MATCH_CHECK: usize = 64;
pub(crate) const SIZEOF_CHECKED_PATTERN: usize = 96;
pub(crate) const SIZEOF_PTR: usize = 8;

pub(crate) const UNKNOWN_MEMBER_MSG: &str = "the module or type has no function with this name";

/// Arena index into [`InferState::terms`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct TermId(pub u32);

impl TermId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::locals`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct LocalId(pub u32);

impl LocalId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::notes`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct NoteId(pub u32);

impl NoteId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::emitted`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct EmittedId(pub u32);

impl EmittedId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::lambdas`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct EmittedLambdaId(pub u32);

impl EmittedLambdaId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::matches_emitted`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct EmittedMatchId(pub u32);

impl EmittedMatchId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::dispatches`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct DispatchId(pub u32);

impl DispatchId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::failures`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct FailureId(pub u32);

impl FailureId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::capture_loops`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct CaptureLoopId(pub u32);

impl CaptureLoopId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::capture_paths`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct CapturePathId(pub u32);

impl CapturePathId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Arena index into [`InferState::capture_writes`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct CaptureWriteId(pub u32);

impl CaptureWriteId {
    #[must_use]
    pub(crate) const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Either a real syntax node name or a synthetic promise parameter (`result`, `before`, `after`,
/// `now`, `reply`) so we never mutate or fabricate [`SyntaxTree`] nodes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum NameRef {
    Syntax(NodeId),
    Synthetic { name: &'static str, span: Span },
}

impl NameRef {
    #[must_use]
    pub(crate) fn bytes(self, tree: &SyntaxTree) -> &[u8] {
        match self {
            Self::Syntax(nid) => tree.node_text(nid).unwrap_or(&[]),
            Self::Synthetic { name, .. } => name.as_bytes(),
        }
    }

    #[must_use]
    pub(crate) fn span(self, tree: &SyntaxTree) -> Span {
        match self {
            Self::Syntax(nid) => tree.node(nid).span,
            Self::Synthetic { span, .. } => span,
        }
    }

    #[must_use]
    pub(crate) const fn node_id(self) -> Option<NodeId> {
        match self {
            Self::Syntax(nid) => Some(nid),
            Self::Synthetic { .. } => None,
        }
    }

    #[must_use]
    pub(crate) fn is_underscore(self, tree: &SyntaxTree) -> bool {
        match self {
            Self::Syntax(nid) => tree.node(nid).kind == SyntaxKind::Underscore,
            Self::Synthetic { .. } => false,
        }
    }
}

/// Shape of an inference term before resolution (`struct mo_source_type` fields inside `struct term`).
#[derive(Debug, Clone)]
pub(crate) struct TermShape {
    pub kind: SourceTypeKind,
    pub name: String,
    pub module: Option<ModuleId>,
    pub owner: TypeOwner,
    pub arity: usize,
}

impl Default for TermShape {
    fn default() -> Self {
        Self {
            kind: SourceTypeKind::Named,
            name: String::new(),
            module: None,
            owner: TypeOwner::None,
            arity: 0,
        }
    }
}

/// One node in the union-find inference term DAG (`struct term`).
#[derive(Debug, Clone)]
pub(crate) struct Term {
    pub shape: TermShape,
    /// `None` for lazy frozen annotations backed by `resolved`.
    pub arguments: Option<Vec<TermId>>,
    pub parent: Option<TermId>,
    pub resolved: Option<TypeId>,
    pub rank: u32,
    pub seen: u32,
    pub height: u32,
    pub numeric: u8,
    pub variable: bool,
    pub partial_default: bool,
    pub deferred: bool,
}

/// Per-name slot in the lexical scope index (`struct name_slot`).
#[derive(Debug, Clone)]
pub(crate) struct NameSlot {
    pub module_use: Option<Span>,
    pub local_name: Option<Span>,
    pub active: Option<LocalId>,
    pub expired: Option<LocalId>,
}

/// One local variable or parameter binding (`struct local`).
#[derive(Debug, Clone)]
pub(crate) struct Local {
    pub name_key: Vec<u8>,
    pub name: NameRef,
    pub ty: TermId,
    pub previous: Option<LocalId>,
    pub binding: Option<NoteId>,
    pub writes: Option<CaptureWriteId>,
    pub function: u32,
    pub version: u32,
    pub pattern_group: u32,
    pub pattern_alternative: u32,
    pub variable: bool,
    pub used: bool,
    pub statement: bool,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct Capture {
    pub local: LocalId,
    pub use_span: Span,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct CaptureLoop {
    pub syntax: NodeId,
    pub parent: Option<CaptureLoopId>,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct CapturePath {
    pub group: NodeId,
    pub arm: u32,
    pub parent: Option<CapturePathId>,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct CaptureWrite {
    pub syntax: NodeId,
    pub target: NodeId,
    pub path: Option<CapturePathId>,
    pub next: Option<CaptureWriteId>,
}

#[derive(Debug, Clone)]
pub(crate) struct EmittedLambda {
    pub number: u32,
    pub parameters: Vec<Option<NoteId>>,
    pub parent: Option<EmittedLambdaId>,
    pub captures: Vec<Capture>,
    pub syntax: NodeId,
    pub loop_ctx: Option<CaptureLoopId>,
    pub path: Option<CapturePathId>,
    pub depth: u32,
}

#[derive(Debug, Clone)]
pub(crate) struct EmittedArm {
    pub syntax: NodeId,
    pub pattern_count: usize,
    pub guard_operand: Option<usize>,
    pub body_operand: usize,
}

#[derive(Debug, Clone)]
pub(crate) struct EmittedMatch {
    pub arms: Vec<EmittedArm>,
}

#[derive(Debug, Clone)]
pub(crate) struct Emitted {
    pub operands: Vec<Option<NoteId>>,
    pub operation: TokenKind,
    pub symbol: Option<SymRef>,
    pub native: Option<String>,
    pub service: Option<CheckedServiceId>,
    pub tool: Option<u32>,
    pub field: Option<u32>,
    pub receiver_slot: Option<usize>,
    pub arguments: Vec<TypeArgument>,
    pub abilities: Vec<CheckedAbilityUse>,
    pub or_kind: CheckedOr,
    pub assignment: bool,
    pub lambda: Option<EmittedLambdaId>,
    pub match_emitted: Option<EmittedMatchId>,
    pub generic: Option<TypeId>,
    pub instantiated: Option<TermId>,
    pub pattern_type: Option<TermId>,
    pub try_result: Option<TermId>,
    pub failure: Option<FailureId>,
    pub function_type: Option<TypeId>,
    pub requirements: [u8; 26],
    pub parameters: Vec<TermId>,
    pub json_decode: bool,
}

impl Default for Emitted {
    fn default() -> Self {
        Self {
            operands: Vec::new(),
            operation: TokenKind::Eof,
            symbol: None,
            native: None,
            service: None,
            tool: None,
            field: None,
            receiver_slot: None,
            arguments: Vec::new(),
            abilities: Vec::new(),
            or_kind: CheckedOr::Absent,
            assignment: false,
            lambda: None,
            match_emitted: None,
            generic: None,
            instantiated: None,
            pattern_type: None,
            try_result: None,
            failure: None,
            function_type: None,
            requirements: [0; 26],
            parameters: Vec::new(),
            json_decode: false,
        }
    }
}

/// One note recorded for an expression, pattern, or binding (`struct note`).
#[derive(Debug, Clone)]
pub(crate) struct Note {
    pub syntax: Option<NodeId>,
    pub synthetic_name: Option<(&'static str, Span)>,
    pub resolved_type: Option<TypeId>,
    pub binding_name: Option<NameRef>,
    pub symbol: Option<SymRef>,
    pub field_index: Option<u32>,
    pub builtin_case: Option<&'static str>,
    pub never: bool,
    pub ty: TermId,
    pub next: Option<NoteId>,
    pub graph: Option<EmittedId>,
    pub binding: Option<NoteId>,
    pub callee: Option<NoteId>,
    pub row_index: Option<u32>,
    pub binding_index: Option<u32>,
    pub variable: bool,
    pub discarded: bool,
    pub discard_result: bool,
    pub negative: bool,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct NoteRange {
    pub first: Option<NoteId>,
    pub last: Option<NoteId>,
}

/// Transient value returned during expression inference (`struct value`).
#[derive(Debug, Clone, Default)]
pub(crate) struct InferValue {
    pub ty: Option<TermId>,
    pub binding: Option<NameRef>,
    pub symbol: Option<SymRef>,
    pub field_index: Option<u32>,
    pub builtin_case: Option<&'static str>,
    pub graph: Option<EmittedId>,
    pub note: Option<NoteId>,
    pub bound: Option<NoteId>,
    pub callee: Option<NoteId>,
    pub dispatch: Option<DispatchId>,
    pub function_type: Option<TypeId>,
    pub parameters: Vec<TermId>,
    pub json_decode: bool,
    pub never: bool,
}

impl InferValue {
    #[must_use]
    pub(crate) fn none() -> Self {
        Self::default()
    }

    #[must_use]
    pub(crate) fn with_type(ty: TermId) -> Self {
        Self {
            ty: Some(ty),
            ..Self::default()
        }
    }
}

#[derive(Debug, Clone)]
pub(crate) struct Dispatch {
    pub name: NodeId,
    pub group: Option<(SymRef, u32)>,
    pub count: usize,
    pub ty: TermId,
    pub graph: Option<EmittedId>,
    pub note: Option<NoteId>,
    pub syntax: Option<NodeId>,
    pub call: Option<NodeId>,
    pub arguments: Option<NodeId>,
    pub receiver: InferValue,
    pub inputs: Vec<InferValue>,
    pub position: bool,
    pub solved: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct FailureCheck {
    pub input: TermId,
    pub value: TermId,
    pub context: Option<TermId>,
    pub fallback: Option<TermId>,
    pub input_note: Option<NoteId>,
    pub graph: Option<EmittedId>,
    pub span: Span,
    pub wrapper: Option<ConstructorRef>,
    pub generic: Option<TypeId>,
    pub op: TokenKind,
    pub code: Option<ErrorCode>,
    pub solved: bool,
    pub never: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct TypedPattern {
    pub match_id: MatchPatternId,
    pub ty: TermId,
    pub syntax: NodeId,
}

#[derive(Debug, Clone)]
pub(crate) struct MatchCheck {
    pub patterns: Vec<Option<MatchPatternId>>,
    pub span: Span,
    pub end: u32,
    pub scrutinee: NodeId,
    pub ty: TermId,
    pub catch_all: Option<NodeId>,
}

/// Synthetic or source enclosing-function metadata passed into inference.
#[derive(Debug, Clone)]
pub(crate) struct EnclosingContext {
    pub module: ModuleId,
    pub owner_symbol: Option<SymRef>,
    pub func_sig_id: Option<FuncSigId>,
    pub declaration: NodeId,
    pub owner_kind: SyntaxKind,
    pub fn_type: TypeId,
    pub requirements: [u8; 26],
    /// Parameter name refs when checking a normal function, function promise, or service promise.
    pub param_names: Vec<NameRef>,
}

/// Enclosing state for one body inference/checking run (`struct inference`).
pub(crate) struct InferState<'a> {
    pub modules: &'a ModuleSet,
    pub interfaces: &'a ModuleInterfaces,
    pub signatures: &'a mut Signatures,
    pub abilities_arena: &'a mut AbilityArena,
    pub ability_cache: AbilityCache,
    pub constants: Option<&'a mut ConstantContext>,
    pub services_view: Option<ProgramServicesView<'a>>,
    pub diags: &'a mut DiagBag,
    pub arena_remaining: &'a mut usize,
    pub enclosing: EnclosingContext,

    pub terms: Vec<Term>,
    pub names: HashMap<Vec<u8>, NameSlot>,
    pub locals: Vec<Local>,
    pub active_locals_head: Option<LocalId>,
    pub notes: Vec<Note>,
    pub notes_head: Option<NoteId>,
    pub notes_tail: Option<NoteId>,
    pub emitted: Vec<Emitted>,
    pub lambdas: Vec<EmittedLambda>,
    pub matches_emitted: Vec<EmittedMatch>,
    pub match_patterns: MatchPatternArena,
    pub typed_patterns: Vec<TypedPattern>,
    pub match_checks: Vec<MatchCheck>,
    pub pattern_defaults: Vec<(DataSigId, TypeId, Vec<CheckedPatternId>)>,
    pub dispatches: Vec<Dispatch>,
    pub ability_modules: Vec<(EmittedId, ModuleId)>,
    pub failures: Vec<FailureCheck>,
    pub equal_types: Vec<(TypeId, TypeId)>,
    pub capture_loops: Vec<CaptureLoop>,
    pub capture_paths: Vec<CapturePath>,
    pub capture_writes: Vec<CaptureWrite>,
    pub lambda_frame: Option<EmittedLambdaId>,
    pub capture_loop: Option<CaptureLoopId>,
    pub capture_path: Option<CapturePathId>,
    pub capture_free: u32,
    pub result: TermId,

    pub span: Span,
    pub error: Option<Diagnostic>,
    pub error_unused: Option<LocalId>,
    pub unused: Option<LocalId>,
    pub type_label: Option<String>,
    pub expectation: Option<ErrorCode>,
    pub node_expectation: Option<ErrorCode>,

    pub binding_count: u32,
    pub depth: u32,
    pub loops: u32,
    pub lambda_depth: u32,
    pub work: u32,
    pub epoch: u32,
    pub pattern_group: u32,
    pub pattern_depth: u32,

    pub checked: bool,
    pub spec: bool,
    pub keep_patterns: bool,
    pub failed: bool,
    pub fatal: bool,
    pub partial: bool,
    pub unavailable: bool,
}

impl<'a> InferState<'a> {
    #[must_use]
    pub(crate) fn tree(&self) -> &'a SyntaxTree {
        &self.modules.module(self.enclosing.module).syntax.tree
    }

    #[must_use]
    pub(crate) fn node(&self, id: NodeId) -> &'a SyntaxNode {
        self.tree().node(id)
    }

    #[must_use]
    pub(crate) fn node_text(&self, id: NodeId) -> &'a [u8] {
        self.tree().node_text(id).unwrap_or(&[])
    }

    pub(crate) fn prove_ability(
        &mut self,
        ty: TypeId,
        ability: crate::abilities::SourceAbility,
    ) -> Result<crate::ids::ProofId, crate::abilities::AbilityFailure> {
        let arena = std::mem::take(self.abilities_arena);
        let cache = std::mem::take(&mut self.ability_cache);
        let mut ctx =
            AbilityContext::with_parts(arena, cache, self.interfaces, self.signatures, self.work);
        let proof = ctx.prove_ability(ty, ability);
        let (arena, cache, work, err) = ctx.into_parts();
        *self.abilities_arena = arena;
        self.ability_cache = cache;
        self.work = work;
        match proof {
            Some(pid) => Ok(pid),
            None => Err(err.unwrap_or_else(|| crate::abilities::AbilityFailure {
                code: ErrorCode::CheckWork,
                message: "the function needs too much ability-checking work".to_string(),
                reason: None,
            })),
        }
    }

    pub(crate) fn check_type_abilities(
        &mut self,
        ty: TypeId,
    ) -> Result<(), crate::abilities::AbilityFailure> {
        let arena = std::mem::take(self.abilities_arena);
        let cache = std::mem::take(&mut self.ability_cache);
        let mut ctx =
            AbilityContext::with_parts(arena, cache, self.interfaces, self.signatures, self.work);
        let ok = ctx.check_type_abilities(ty);
        let (arena, cache, work, err) = ctx.into_parts();
        *self.abilities_arena = arena;
        self.ability_cache = cache;
        self.work = work;
        if ok {
            Ok(())
        } else {
            Err(err.unwrap_or_else(|| crate::abilities::AbilityFailure {
                code: ErrorCode::CheckWork,
                message: "the function needs too much ability-checking work".to_string(),
                reason: None,
            }))
        }
    }

    pub(crate) fn charge_arena(&mut self, count: usize, elem_size: usize) -> bool {
        if self.failed {
            return false;
        }
        let bytes = count.saturating_mul(elem_size);
        let aligned = bytes.checked_add(7).map_or(usize::MAX, |n| n & !7usize);
        if aligned > *self.arena_remaining {
            let _ = self.problem(
                ErrorCode::CheckNoMemory,
                self.span,
                "the checking arena has no room",
                None,
                None,
            );
            return false;
        }
        *self.arena_remaining -= aligned;
        true
    }

    pub(crate) fn work(&mut self) -> bool {
        if self.failed {
            return false;
        }
        if u64::from(self.work) == MO_LIMIT_SOURCE_CHECK_WORK {
            return self.problem(
                ErrorCode::CheckWork,
                self.span,
                "the function needs too much type-checking work",
                None,
                None,
            );
        }
        if self.signatures.work == MO_LIMIT_PROGRAM_CHECK_WORK {
            return self.problem(
                ErrorCode::CheckProgramWork,
                self.span,
                "the program needs too much type-checking work",
                None,
                None,
            );
        }
        self.work += 1;
        self.signatures.work += 1;
        true
    }

    pub(crate) fn unfinished(&mut self) -> bool {
        self.problem(
            ErrorCode::CheckUnfinished,
            self.span,
            "this body rule is not implemented by the staged checker",
            None,
            None,
        )
    }

    pub(crate) fn problem(
        &mut self,
        code: ErrorCode,
        span: Span,
        message: impl Into<String>,
        reason: Option<&str>,
        previous: Option<Span>,
    ) -> bool {
        if self.fatal || (self.partial && code == ErrorCode::CannotInfer) {
            return false;
        }
        self.failed = true;
        self.fatal = code.number() >= 1000 && code != ErrorCode::CheckUnfinished;
        if !self.fatal
            && let Some(existing) = &self.error
            && existing.primary.span.start <= span.start
        {
            return false;
        }
        let message_str = message.into();
        let label_str: String = match code {
            ErrorCode::NameUnknown => {
                if message_str == UNKNOWN_MEMBER_MSG {
                    "no function of this name".to_string()
                } else {
                    "not visible in this block".to_string()
                }
            }
            ErrorCode::BindRepeated => "this name already has a meaning".to_string(),
            ErrorCode::UnusedName => "never read".to_string(),
            ErrorCode::TypeMismatch
            | ErrorCode::ConditionType
            | ErrorCode::NumberAbility
            | ErrorCode::ByteArithmetic
            | ErrorCode::ArgumentType
            | ErrorCode::ResultType
            | ErrorCode::ServiceValue
            | ErrorCode::NotFunction => self
                .type_label
                .clone()
                .unwrap_or_else(|| message_str.clone()),
            ErrorCode::CannotInfer => "no use decides the type".to_string(),
            ErrorCode::BoolName => "must be Bool or a function returning Bool".to_string(),
            ErrorCode::CallArguments => "pass one positional argument per parameter".to_string(),
            ErrorCode::UnusedValue => "not Unit and not used".to_string(),
            ErrorCode::NotVariable => "no assignable var here".to_string(),
            ErrorCode::LambdaWrite => "an anonymous function can't assign an outer var".to_string(),
            ErrorCode::CapabilityCapture => "pass capabilities as parameters instead".to_string(),
            ErrorCode::CaptureAssignment => "copy this value into a name first".to_string(),
            ErrorCode::ExitOutsideLoop => "no enclosing loop in this function".to_string(),
            ErrorCode::MapKeyRepeated => "this key is already in the literal".to_string(),
            ErrorCode::DotCallParentheses => "the function takes only the receiver".to_string(),
            ErrorCode::FakeOutsideSpec => "outside any test, example or promise".to_string(),
            ErrorCode::LiteralRange => "outside its inferred numeric range".to_string(),
            ErrorCode::TuplePosition => "part index is outside this tuple".to_string(),
            ErrorCode::FailureIgnored => {
                "handle this Result or pass its failure upward".to_string()
            }
            ErrorCode::DataArguments => "supplied fields don't match the declaration".to_string(),
            ErrorCode::DataPrivate => "the type's insides aren't exposed".to_string(),
            ErrorCode::FieldMissing => "no field with this name".to_string(),
            ErrorCode::PatternFields => "matched fields don't fit this constructor".to_string(),
            ErrorCode::CaseCatchAll => "covers cases without naming them".to_string(),
            ErrorCode::PatternAlternatives => "the alternatives bind different names".to_string(),
            ErrorCode::CaseIncomplete => "a possible value isn't covered".to_string(),
            ErrorCode::PatternConstructor => "not a record or case constructor".to_string(),
            _ => message_str.clone(),
        };

        let mut diag = Diagnostic::new(code, span, message_str).with_primary_text(label_str);
        if let Some(r) = reason {
            diag = diag.with_reason(r);
        }
        if let Some(prev_span) = previous {
            let sec_text = match code {
                ErrorCode::BindRepeated => "the other meaning of this name",
                ErrorCode::CaptureAssignment => "assigned here",
                ErrorCode::MapKeyRepeated => "the first equal key",
                _ => "declared here",
            };
            diag = diag.with_secondary(Label::with_text(prev_span, sec_text));
        }
        self.error = Some(diag);
        self.error_unused = self.unused;
        false
    }

    pub(crate) fn defer(
        &mut self,
        code: ErrorCode,
        span: Span,
        message: impl Into<String>,
        reason: Option<&str>,
    ) {
        let _ = self.problem(code, span, message, reason, None);
        self.failed = self.fatal;
    }

    pub(crate) fn emit_problem(&mut self) {
        if self.unavailable && self.error.is_none() {
            return;
        }
        let Some(mut diag) = self.error.take() else {
            return;
        };
        if diag.code == ErrorCode::UnusedName {
            let offer_fix = if let Some(lid) = self.error_unused {
                let local = &self.locals[lid.index()];
                if !local.variable && local.pattern_alternative <= 1 {
                    let mut candidate = Vec::with_capacity(local.name_key.len() + 1);
                    candidate.push(b'_');
                    candidate.extend_from_slice(&local.name_key);
                    let scope = self.interfaces.scope(self.enclosing.module);
                    !self.names.contains_key(&candidate)
                        && scope.imported.is_empty()
                        && scope.find_symbol(&candidate).is_none()
                } else {
                    false
                }
            } else {
                false
            };
            if offer_fix {
                let span = diag.primary.span;
                let edit = Edit::new(Span::new(span.file, span.start, span.start), "_");
                diag = diag.with_fix(Fix::new(
                    Some("mark the name unused on purpose".into()),
                    vec![edit],
                ));
            }
        }
        let _ = self.diags.push(diag);
    }

    pub(crate) fn to_inferred_expr(&self, note_id: NoteId) -> InferredExpression {
        let n = &self.notes[note_id.index()];
        let syntax = n.syntax.unwrap_or(self.enclosing.declaration);
        let ty = n.resolved_type.unwrap_or(self.enclosing.fn_type);
        InferredExpression {
            syntax,
            ty,
            binding: n.binding_name.and_then(NameRef::node_id),
            symbol: n.symbol,
            native: n
                .builtin_case
                .map(str::to_string)
                .or_else(|| n.graph.and_then(|g| self.emitted[g.index()].native.clone())),
            field: n.field_index,
            discarded: n.discarded,
        }
    }
}
