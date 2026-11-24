//! Checked-program IR produced by [`crate::check_program`] and consumed by Slice 2E (`mo-lower`).
//!
//! Structural parity with `src/check/check.h` (`struct mo_program`, `struct mo_checked_*`,
//! `struct mo_inferred_body`), using typed arena indices (`ids.rs`), zero raw pointers,
//! zero interior mutability, and zero lifetime sprawl.

use mo_base::Span;
use mo_syntax::{NodeId, TokenKind};

use crate::abilities::{AbilityArena, TypeArgument};
use crate::ids::{
    CheckedBindingId, CheckedConstId, CheckedExprId, CheckedFuncId, CheckedLambdaId,
    CheckedMatchId, CheckedPatternId, CheckedServiceId, CheckedSpecId, FuncSigId, ModuleId,
    ProofId, SymRef, TypeId,
};
use crate::load::ModuleSet;
use crate::modules::ModuleInterfaces;
use crate::signatures::Signatures;

/// One inferred expression record returned by [`crate::infer_function`]
/// (`struct mo_inferred_expression`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InferredExpression {
    pub syntax: NodeId,
    pub ty: TypeId,
    pub binding: Option<NodeId>,
    pub symbol: Option<SymRef>,
    pub native: Option<String>,
    pub field: Option<u32>,
    pub discarded: bool,
}

/// Intermediate body inference result (`struct mo_inferred_body`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InferredBody {
    pub items: Vec<InferredExpression>,
}

/// Resolved constructor reference (`struct mo_constructor_ref`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ConstructorRef {
    Source(SymRef),
    Builtin(&'static str),
}

/// Checked local binding or function/lambda/promise parameter (`struct mo_checked_binding`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedBinding {
    /// Canonical declaring name node, or `None` for a promise's implicit `result` parameter.
    pub syntax: Option<NodeId>,
    pub name: String,
    pub span: Span,
    pub ty: TypeId,
    pub variable: bool,
}

/// Ability proof requirement attached to an expression, pattern, try-wrapper, or anchor
/// (`struct mo_checked_ability_use`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CheckedAbilityUse {
    /// Body expression row, or `None` for a type-only generic-use requirement.
    pub operand: Option<CheckedExprId>,
    pub implementation: ProofId,
    /// Scoped callee type parameter for generic requirements; `None` for an operator/dot use.
    pub parameter: Option<TypeId>,
}

/// Normalized pattern kind (`enum mo_checked_pattern_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckedPatternKind {
    Any,
    Bind,
    Literal,
    Tuple,
    List,
    Constructor,
}

/// One sub-pattern slot (`struct mo_checked_pattern_part`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CheckedPatternPart {
    /// Tuple slot or declared field index; for list cons, `0` = head, `1` = tail.
    pub index: u32,
    pub pattern: CheckedPatternId,
}

/// Normalized, typed pattern node (`struct mo_checked_pattern`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedPattern {
    pub kind: CheckedPatternKind,
    pub syntax: NodeId,
    pub ty: TypeId,
    pub binding: Option<CheckedBindingId>,
    pub constructor: Option<ConstructorRef>,
    pub arguments: Vec<TypeArgument>,
    pub abilities: Vec<CheckedAbilityUse>,
    pub parts: Vec<CheckedPatternPart>,
}

/// One arm of a `case` expression (`struct mo_checked_arm`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedArm {
    pub syntax: NodeId,
    pub patterns: Vec<CheckedPatternId>,
    pub guard: Option<CheckedExprId>,
    pub body: CheckedExprId,
}

/// Exhaustively checked `case` match (`struct mo_checked_match`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedMatch {
    pub scrutinee: CheckedExprId,
    pub arms: Vec<CheckedArm>,
}

/// Disambiguation of `or` (`enum mo_checked_or`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckedOr {
    Absent,
    Bool,
    Option,
}

/// Resolved `try` propagation metadata (`struct mo_checked_try`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedTry {
    pub result: TypeId,
    pub failure: Option<ConstructorRef>,
    pub wrapper: Option<ConstructorRef>,
    pub arguments: Vec<TypeArgument>,
    pub abilities: Vec<CheckedAbilityUse>,
}

/// Checked anonymous function (`struct mo_checked_lambda`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedLambda {
    /// One-based source order within the containing function/constant/spec.
    pub number: u32,
    /// `None` for `_` ignored lambda parameters.
    pub parameters: Vec<Option<CheckedBindingId>>,
    pub body: CheckedExprId,
    /// Captured bindings in first-use order (capture by value).
    pub captures: Vec<CheckedBindingId>,
}

/// Typed expression node in written postorder (`struct mo_checked_expression`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedExpression {
    pub syntax: NodeId,
    pub ty: TypeId,
    pub operands: Vec<CheckedExprId>,
    pub operation: TokenKind,
    pub binding: Option<CheckedBindingId>,
    pub symbol: Option<SymRef>,
    pub native: Option<String>,
    pub service: Option<CheckedServiceId>,
    pub tool: Option<u32>,
    pub field: Option<u32>,
    pub receiver: Option<CheckedExprId>,
    pub arguments: Vec<TypeArgument>,
    pub abilities: Vec<CheckedAbilityUse>,
    pub or_kind: CheckedOr,
    pub try_info: Option<CheckedTry>,
    pub pattern: Option<CheckedPatternId>,
    pub match_info: Option<CheckedMatchId>,
    pub lambda: Option<CheckedLambdaId>,
    pub assignment: bool,
    pub never: bool,
}

/// Checked body arena owning expressions, bindings, patterns, matches, and lambdas
/// (`struct mo_checked_body`).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CheckedBody {
    pub items: Vec<CheckedExpression>,
    /// Root expression row; `None` only for an anchor's argument-only body.
    pub root: Option<CheckedExprId>,
    /// Bitmask of `'A'..='Z'` type parameter letters captured by anonymous functions.
    pub capture_free_parameters: u32,
    pub bindings: Vec<CheckedBinding>,
    pub patterns: Vec<CheckedPattern>,
    pub matches: Vec<CheckedMatch>,
    pub lambdas: Vec<CheckedLambda>,
}

impl CheckedBody {
    #[must_use]
    pub fn expr(&self, id: CheckedExprId) -> &CheckedExpression {
        &self.items[id.index()]
    }

    #[must_use]
    pub fn binding(&self, id: CheckedBindingId) -> &CheckedBinding {
        &self.bindings[id.index()]
    }

    #[must_use]
    pub fn pattern(&self, id: CheckedPatternId) -> &CheckedPattern {
        &self.patterns[id.index()]
    }

    #[must_use]
    pub fn match_info(&self, id: CheckedMatchId) -> &CheckedMatch {
        &self.matches[id.index()]
    }

    #[must_use]
    pub fn lambda(&self, id: CheckedLambdaId) -> &CheckedLambda {
        &self.lambdas[id.index()]
    }
}

/// Checked top-level function (`struct mo_checked_function`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedFunction {
    pub signature: FuncSigId,
    pub symbol: SymRef,
    /// Parameter bindings in signature order (indices into `body.bindings`).
    pub parameters: Vec<CheckedBindingId>,
    pub body: CheckedBody,
}

/// Checked module constant (`struct mo_checked_constant`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedConstant {
    pub symbol: SymRef,
    pub ty: TypeId,
    pub initializer: CheckedBody,
}

/// Service tool reply shape (`enum mo_service_reply`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ServiceReply {
    ReadOnly,
    Update,
    ResultUpdate,
}

/// Checked service/actor tool or call handler (`struct mo_checked_service_tool`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedServiceTool {
    pub name: NodeId,
    pub function: CheckedFuncId,
    pub reply: ServiceReply,
}

/// Checked service timer clause (`struct mo_checked_service_timer`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedServiceTimer {
    pub syntax: NodeId,
    pub function: CheckedFuncId,
    pub interval: CheckedBody,
}

/// Exact original source bytes copied for a spec/condition (`struct mo_spec_text`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpecText {
    pub bytes: Vec<u8>,
    pub span: Span,
}

impl Default for SpecText {
    fn default() -> Self {
        Self {
            bytes: Vec::new(),
            span: Span::NONE,
        }
    }
}

/// One Bool condition row inside a promise (`struct mo_checked_condition`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedCondition {
    pub text: SpecText,
    pub expression: CheckedExprId,
}

/// Kind of checked specification item (`enum mo_checked_spec_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckedSpecKind {
    Example,
    Test,
    Promise,
    Anchor,
}

/// Checked `example`, `test`, `promise`, or `allows`/`forbids` anchor (`struct mo_checked_spec`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedSpec {
    pub kind: CheckedSpecKind,
    pub syntax: NodeId,
    pub module: ModuleId,
    pub number: u32,
    pub subject: Option<CheckedFuncId>,
    pub promise: Option<CheckedSpecId>,
    pub text: SpecText,
    pub wording: Option<Vec<u8>>,
    pub parameters: Vec<CheckedBindingId>,
    pub body: CheckedBody,
    pub arguments: Vec<TypeArgument>,
    pub abilities: Vec<CheckedAbilityUse>,
    pub values: Vec<CheckedExprId>,
    pub conditions: Vec<CheckedCondition>,
    pub allows: bool,
}

/// Checked service/actor promise and its anchors (`struct mo_checked_service_promise`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedServicePromise {
    pub spec: CheckedSpec,
    /// Tool index in `CheckedService::tools`, or `None` for an every-step invariant.
    pub tool: Option<u32>,
    pub anchors: Vec<CheckedSpec>,
}

/// Checked `service` or `actor` declaration (`struct mo_checked_service`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedService {
    pub syntax: NodeId,
    pub module: ModuleId,
    pub is_actor: bool,
    pub actor_symbol: Option<SymRef>,
    pub state: TypeId,
    pub initializer: CheckedBody,
    pub tools: Vec<CheckedServiceTool>,
    pub timers: Vec<CheckedServiceTimer>,
    pub token_source: Option<NodeId>,
    pub promises: Vec<CheckedServicePromise>,
}

/// HTTP verb for an `api` route (`enum mo_api_method`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ApiMethod {
    Get,
    Post,
    Put,
    Patch,
    Delete,
}

/// Classification of one parameter in an `api` handler/auth/job (`enum mo_api_param_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ApiParamKind {
    Auth,
    AuthOptional,
    Request,
    SqliteDb,
    Actor,
    Capability,
    Path,
    QueryRequired,
    QueryOptional,
    Body,
}

/// Auth mode of an `api` route (`enum mo_api_auth_mode`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ApiAuthMode {
    None,
    Required,
    Optional,
}

/// Reply kind of an `api` route (`enum mo_api_reply_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ApiReplyKind {
    Response,
    ResultResponse,
    Unit,
    Json,
    ResultUnit,
    ResultJson,
}

/// Reply kind of an `api` job (`enum mo_api_job_reply_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ApiJobReplyKind {
    Unit,
    ResultUnit,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiSqlite {
    pub syntax: NodeId,
    pub name: String,
    pub path: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiStatic {
    pub syntax: NodeId,
    pub prefix: String,
    pub dir: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiActor {
    pub syntax: NodeId,
    pub name: String,
    pub ty: TypeId,
    pub service_index: CheckedServiceId,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiParam {
    pub kind: ApiParamKind,
    pub name: String,
    pub ty: TypeId,
    pub index: u32,
    pub json: Option<ProofId>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiAuth {
    pub syntax: NodeId,
    pub principal: TypeId,
    pub function: CheckedFuncId,
    pub params: Vec<CheckedApiParam>,
    pub error_type: TypeId,
    pub error_json: Option<ProofId>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiRoute {
    pub syntax: NodeId,
    pub method: ApiMethod,
    pub path: String,
    pub function: CheckedFuncId,
    pub auth_mode: ApiAuthMode,
    pub params: Vec<CheckedApiParam>,
    pub reply: ApiReplyKind,
    pub ok_type: Option<TypeId>,
    pub ok_json: Option<ProofId>,
    pub error_type: Option<TypeId>,
    pub error_json: Option<ProofId>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiStatus {
    pub syntax: NodeId,
    pub error_enum: SymRef,
    pub variant: SymRef,
    pub status_code: u16,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApiJob {
    pub syntax: NodeId,
    pub name: String,
    pub function: CheckedFuncId,
    pub has_schedule: bool,
    pub interval_us: u64,
    pub params: Vec<CheckedApiParam>,
    pub reply: ApiJobReplyKind,
    pub error_type: Option<TypeId>,
    pub error_text: Option<ProofId>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedApi {
    pub syntax: NodeId,
    pub module: ModuleId,
    pub has_port: bool,
    pub port: u16,
    pub cors: Option<String>,
    pub sqlites: Vec<CheckedApiSqlite>,
    pub actors: Vec<CheckedApiActor>,
    pub statics: Vec<CheckedApiStatic>,
    pub jobs: Vec<CheckedApiJob>,
    pub auth: Option<CheckedApiAuth>,
    pub routes: Vec<CheckedApiRoute>,
    pub statuses: Vec<CheckedApiStatus>,
    pub capabilities: Vec<String>,
}

/// Entry-point return contract (`enum mo_checked_entry_result`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckedEntryResult {
    Unit,
    Status,
    Result,
}

/// Checked `main` entry contract (`struct mo_checked_entry`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CheckedEntry {
    pub function: Option<CheckedFuncId>,
    pub result: CheckedEntryResult,
    pub capabilities: Vec<CheckedBindingId>,
    pub error_text: Option<ProofId>,
}

impl Default for CheckedEntry {
    fn default() -> Self {
        Self {
            function: None,
            result: CheckedEntryResult::Unit,
            capabilities: Vec::new(),
            error_text: None,
        }
    }
}

/// Rule variant for [`crate::captures::check_service_purity`] (`enum mo_purity_rule`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum PurityRule {
    Service,
    Query,
    Migration,
}

/// Query entry input shape (`enum mo_query_input`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum QueryInput {
    State,
    Failure,
}

/// Result of [`crate::check_query_entry`] and [`crate::check_migration_entry`]
/// (`enum mo_query_entry`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum QueryEntryStatus {
    Accepted(CheckedFuncId),
    Refused,
    NoModule,
    NoFunction,
}

/// Complete, certified checked program (`struct mo_program`), ready for lowering in Slice 2E.
#[derive(Debug, Clone)]
pub struct CheckedProgram {
    pub modules: ModuleSet,
    pub interfaces: ModuleInterfaces,
    pub signatures: Signatures,
    pub abilities: AbilityArena,
    pub functions: Vec<CheckedFunction>,
    pub constants: Vec<CheckedConstant>,
    pub specs: Vec<CheckedSpec>,
    pub services: Vec<CheckedService>,
    pub apis: Vec<CheckedApi>,
    pub entry: CheckedEntry,
}

impl CheckedProgram {
    #[must_use]
    pub fn function(&self, id: CheckedFuncId) -> &CheckedFunction {
        &self.functions[id.index()]
    }

    #[must_use]
    pub fn constant(&self, id: CheckedConstId) -> &CheckedConstant {
        &self.constants[id.index()]
    }

    #[must_use]
    pub fn spec(&self, id: CheckedSpecId) -> &CheckedSpec {
        &self.specs[id.index()]
    }

    #[must_use]
    pub fn service(&self, id: CheckedServiceId) -> &CheckedService {
        &self.services[id.index()]
    }
}
