//! Mo Core IR types mirroring `formal/MoCore/Syntax.lean`, `src/core/core.h`,
//! `src/core/text_schema.def`, and `src/base/service.h`.

use mo_base::Span;
use mo_def::core_ops::{
    CoreBase, CoreBigOp, CoreByteOp, CoreBytesOp, CoreCharOp, CoreCmpOp, CoreCryptoOp, CoreDecOp,
    CoreFloatOp, CoreIntOp, CoreIoNullaryOp, CoreJsonOp, CoreListNullaryOp, CoreMapNullaryOp,
    CoreRangeOp, CoreSetOp, CoreTextOp, CoreTimeOp, CoreUnicodeOp,
};

/// Arena index for a [`Ty`] node.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TyId(pub u32);

/// Arena index for an [`Expr`] node.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ExprId(pub u32);

/// Arena index for a [`Value`] node.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ValueId(pub u32);

/// Arena owning recursive Core [`Ty`], [`Expr`], and [`Value`] nodes.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct CoreArena {
    tys: Vec<Ty>,
    exprs: Vec<Expr>,
    values: Vec<Value>,
}

impl CoreArena {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            tys: Vec::new(),
            exprs: Vec::new(),
            values: Vec::new(),
        }
    }

    pub fn alloc_ty(&mut self, ty: Ty) -> TyId {
        let id = TyId(self.tys.len() as u32);
        self.tys.push(ty);
        id
    }

    #[must_use]
    pub fn ty(&self, id: TyId) -> &Ty {
        &self.tys[id.0 as usize]
    }

    #[must_use]
    pub fn ty_opt(&self, id: TyId) -> Option<&Ty> {
        self.tys.get(id.0 as usize)
    }

    pub fn ty_mut(&mut self, id: TyId) -> &mut Ty {
        &mut self.tys[id.0 as usize]
    }

    #[must_use]
    pub fn ty_count(&self) -> usize {
        self.tys.len()
    }

    pub fn alloc_expr(&mut self, expr: Expr) -> ExprId {
        let id = ExprId(self.exprs.len() as u32);
        self.exprs.push(expr);
        id
    }

    #[must_use]
    pub fn expr(&self, id: ExprId) -> &Expr {
        &self.exprs[id.0 as usize]
    }

    #[must_use]
    pub fn expr_opt(&self, id: ExprId) -> Option<&Expr> {
        self.exprs.get(id.0 as usize)
    }

    pub fn expr_mut(&mut self, id: ExprId) -> &mut Expr {
        &mut self.exprs[id.0 as usize]
    }

    #[must_use]
    pub fn expr_count(&self) -> usize {
        self.exprs.len()
    }

    pub fn alloc_value(&mut self, value: Value) -> ValueId {
        let id = ValueId(self.values.len() as u32);
        self.values.push(value);
        id
    }

    #[must_use]
    pub fn value(&self, id: ValueId) -> &Value {
        &self.values[id.0 as usize]
    }

    #[must_use]
    pub fn value_opt(&self, id: ValueId) -> Option<&Value> {
        self.values.get(id.0 as usize)
    }

    pub fn value_mut(&mut self, id: ValueId) -> &mut Value {
        &mut self.values[id.0 as usize]
    }

    #[must_use]
    pub fn value_count(&self) -> usize {
        self.values.len()
    }
}

/// Arbitrary-precision signed integer stored in canonical decimal form:
/// non-empty ASCII digits, no leading zeros except `"0"`, and `negative == false` for zero.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CoreInteger {
    pub digits: String,
    pub negative: bool,
}

impl CoreInteger {
    #[must_use]
    pub fn zero() -> Self {
        Self {
            digits: "0".to_string(),
            negative: false,
        }
    }

    #[must_use]
    pub fn from_i64(n: i64) -> Self {
        if n < 0 {
            Self {
                digits: n.unsigned_abs().to_string(),
                negative: true,
            }
        } else {
            Self {
                digits: n.to_string(),
                negative: false,
            }
        }
    }

    /// Parses and normalizes an ASCII decimal integer (allowing leading zeros and `-0`).
    #[must_use]
    pub fn parse_decimal(raw: &str) -> Option<Self> {
        if raw.is_empty() {
            return None;
        }
        let (mut negative, rest) = match raw.strip_prefix('-') {
            Some(r) => (true, r),
            None => (false, raw),
        };
        if rest.is_empty() || !rest.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let trimmed = rest.trim_start_matches('0');
        let digits = if trimmed.is_empty() {
            negative = false;
            "0"
        } else {
            trimmed
        };
        Some(Self {
            digits: digits.to_string(),
            negative,
        })
    }

    /// Returns whether `|self| <= bound` where `bound` is a canonical decimal digit string.
    #[must_use]
    pub fn magnitude_le(&self, bound: &str) -> bool {
        self.digits.len() < bound.len()
            || (self.digits.len() == bound.len() && self.digits.as_bytes() <= bound.as_bytes())
    }

    /// Validates canonical decimal digits and normalizes into `(negative, digits_slice)`.
    #[must_use]
    pub fn canonical_parts(&self) -> Option<(bool, &str)> {
        if self.digits.is_empty() || !self.digits.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let trimmed = self.digits.trim_start_matches('0');
        if trimmed.is_empty() {
            Some((false, "0"))
        } else {
            Some((self.negative, trimmed))
        }
    }
}

/// Arbitrary-precision non-negative integer in canonical decimal form.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CoreNatural {
    pub digits: String,
}

impl CoreNatural {
    #[must_use]
    pub fn zero() -> Self {
        Self {
            digits: "0".to_string(),
        }
    }

    #[must_use]
    pub fn from_u64(n: u64) -> Self {
        Self {
            digits: n.to_string(),
        }
    }

    /// Parses and normalizes a non-negative decimal integer (`-000` normalizes to `0`).
    #[must_use]
    pub fn parse_decimal(raw: &str) -> Option<Self> {
        let int = CoreInteger::parse_decimal(raw)?;
        if int.negative {
            return None;
        }
        Some(Self { digits: int.digits })
    }

    /// Returns the numeric value if it is `<= limit`.
    #[must_use]
    pub fn to_usize_bounded(&self, limit: usize) -> Option<usize> {
        let mut value = 0usize;
        for b in self.digits.bytes() {
            if !b.is_ascii_digit() {
                return None;
            }
            let d = usize::from(b - b'0');
            if d > limit || value > (limit - d) / 10 {
                return None;
            }
            value = value * 10 + d;
        }
        Some(value)
    }

    /// Validates decimal digits and returns canonical slice without leading zeros.
    #[must_use]
    pub fn canonical_digits(&self) -> Option<&str> {
        if self.digits.is_empty() || !self.digits.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let trimmed = self.digits.trim_start_matches('0');
        Some(if trimmed.is_empty() { "0" } else { trimmed })
    }
}

/// Capability kinds in Lean order (`enum mo_core_cap_tag`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Cap {
    Console,
    Files,
    ReadFiles,
    Clock,
    Random,
    Args,
    Env,
    Network,
    Connection,
    Sqlite,
    Database,
    Command,
    SecureRandom,
    File,
}

impl Cap {
    #[must_use]
    pub const fn wire_name(self) -> &'static str {
        match self {
            Self::Console => "console",
            Self::Files => "files",
            Self::ReadFiles => "readFiles",
            Self::Clock => "clock",
            Self::Random => "random",
            Self::Args => "args",
            Self::Env => "env",
            Self::Network => "network",
            Self::Connection => "connection",
            Self::Sqlite => "sqlite",
            Self::Database => "database",
            Self::Command => "command",
            Self::SecureRandom => "secureRandom",
            Self::File => "file",
        }
    }

    #[must_use]
    pub fn from_wire(s: &str) -> Option<Self> {
        match s {
            "console" => Some(Self::Console),
            "files" => Some(Self::Files),
            "readFiles" => Some(Self::ReadFiles),
            "clock" => Some(Self::Clock),
            "random" => Some(Self::Random),
            "args" => Some(Self::Args),
            "env" => Some(Self::Env),
            "network" => Some(Self::Network),
            "connection" => Some(Self::Connection),
            "sqlite" => Some(Self::Sqlite),
            "database" => Some(Self::Database),
            "command" => Some(Self::Command),
            "secureRandom" => Some(Self::SecureRandom),
            "file" => Some(Self::File),
            _ => None,
        }
    }

    /// Surface name used in Core checker diagnostic type labels (`src/core/check.c:1693`).
    #[must_use]
    pub const fn display_name(self) -> &'static str {
        match self {
            Self::Console => "Console",
            Self::Files => "Files",
            Self::ReadFiles => "ReadFiles",
            Self::Clock => "Clock",
            Self::Random => "Random",
            Self::Args => "Args",
            Self::Env => "Env",
            Self::Network => "Network",
            Self::Connection => "Connection",
            Self::Sqlite => "Sqlite",
            Self::Database => "Sqlite.Database",
            Self::Command => "Command",
            Self::SecureRandom => "SecureRandom",
            Self::File => "Files.File",
        }
    }
}

/// Core types (`struct mo_core_ty`, all 32 variants).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Ty {
    Int,
    BigInt,
    Byte,
    Decimal,
    Float,
    Bool,
    Char,
    Text,
    Bytes,
    Unit,
    Range,
    Time,
    Duration,
    Tuple(Vec<TyId>),
    List(TyId),
    Map { key: TyId, value: TyId },
    Set(TyId),
    Data(String),
    Option(TyId),
    Result { ok: TyId, err: TyId },
    Ordering,
    FileError,
    Entry,
    EntryKind,
    Simulation,
    SimulationError,
    NetworkError,
    SqliteValue,
    SqliteRows,
    SqliteError,
    Fn { params: Vec<TyId>, ret: TyId },
    Cap(Cap),
}

/// Core literals (`struct mo_core_lit`, all 12 variants).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Lit {
    Int(CoreInteger),
    BigInt(CoreInteger),
    Byte(CoreInteger),
    Decimal(CoreInteger),
    Float(u64),
    Bool(bool),
    Char(char),
    Text(String),
    Bytes(Vec<CoreInteger>),
    Unit,
    Time(CoreInteger),
    Duration(CoreInteger),
}

/// List primitive operation (`struct mo_core_list_op`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ListOp {
    Lit(CoreNatural),
    Nullary(CoreListNullaryOp),
    Zip(TyId),
}

/// Map primitive operation (`struct mo_core_map_op`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MapOp {
    Nullary(CoreMapNullaryOp),
    Lit(CoreNatural),
}

/// IO primitive operation (`struct mo_core_io_op`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoOp {
    Nullary(CoreIoNullaryOp),
    ReadText { ro: bool },
    ReadBytes { ro: bool },
    List { ro: bool },
    Entries { ro: bool },
    Exists { ro: bool },
    Within { ro: bool },
}

/// Simulation primitive operation (`struct mo_core_sim_op`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SimOp {
    Start {
        descriptor: CoreNatural,
    },
    Send {
        descriptor: CoreNatural,
        tool: CoreNatural,
        arguments: Vec<TyId>,
        result: TyId,
    },
    Advance,
    Crash,
    Restart,
}

/// Core primitive operations (`struct mo_core_prim`, all 21 families).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Prim {
    Int(CoreIntOp),
    Big(CoreBigOp),
    Byte(CoreByteOp),
    Dec(CoreDecOp),
    Float(CoreFloatOp),
    Not,
    Cmp { base: CoreBase, op: CoreCmpOp },
    Char(CoreCharOp),
    Text(CoreTextOp),
    Bytes(CoreBytesOp),
    Range(CoreRangeOp),
    Unicode16(CoreUnicodeOp),
    Crypto(CoreCryptoOp),
    Json(CoreJsonOp),
    Sim(SimOp),
    Time(CoreTimeOp),
    List { ty: TyId, op: ListOp },
    Map { key: TyId, value: TyId, op: MapOp },
    Set { ty: TyId, op: CoreSetOp },
    Eq(TyId),
    Structural(TyId),
    Io(IoOp),
}

/// Named typed parameter or field (`struct mo_core_param`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Param {
    pub name: String,
    pub ty: TyId,
}

/// Pattern-matching arm in a `case_` expression (`struct mo_core_arm`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Arm {
    pub ctor: String,
    pub names: Vec<String>,
    pub body: ExprId,
}

/// Core expression (`struct mo_core_expr`, all 17 variants).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Expr {
    Lit(Lit),
    Var(String),
    Global(String),
    Let {
        name: String,
        bound: ExprId,
        body: ExprId,
    },
    Lam {
        params: Vec<Param>,
        body: ExprId,
    },
    App {
        func: ExprId,
        args: Vec<ExprId>,
    },
    Call {
        name: String,
        args: Vec<ExprId>,
    },
    Tuple(Vec<ExprId>),
    Proj {
        expr: ExprId,
        index: CoreNatural,
    },
    Con {
        ty: TyId,
        ctor: String,
        args: Vec<ExprId>,
    },
    Case {
        expr: ExprId,
        arms: Vec<Arm>,
    },
    If {
        cond: ExprId,
        then_: ExprId,
        else_: ExprId,
    },
    Prim {
        prim: Prim,
        args: Vec<ExprId>,
    },
    Stop(String),
    Fail(ExprId),
    Join {
        name: String,
        params: Vec<Param>,
        body: ExprId,
        rest: ExprId,
    },
    Jump {
        name: String,
        args: Vec<ExprId>,
    },
}

/// Constructor declaration in a [`DataDecl`] (`struct mo_core_ctor`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CtorDecl {
    pub name: String,
    pub fields: Vec<Param>,
}

/// Nominal algebraic data declaration (`struct mo_core_data`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataDecl {
    pub name: String,
    pub ctors: Vec<CtorDecl>,
}

/// Function kind (`enum mo_core_fun_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FnKind {
    Function,
    Test,
    Example,
    Promise,
    Anchor,
}

impl FnKind {
    #[must_use]
    pub const fn wire_name(self) -> &'static str {
        match self {
            Self::Function => "function",
            Self::Test => "test",
            Self::Example => "example",
            Self::Promise => "promise",
            Self::Anchor => "anchor",
        }
    }

    #[must_use]
    pub fn from_wire(s: &str) -> Option<Self> {
        match s {
            "function" => Some(Self::Function),
            "test" => Some(Self::Test),
            "example" => Some(Self::Example),
            "promise" => Some(Self::Promise),
            "anchor" => Some(Self::Anchor),
            _ => None,
        }
    }
}

/// Top-level function declaration (`struct mo_core_fun`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FnDecl {
    pub name: String,
    pub params: Vec<Param>,
    pub ret: TyId,
    pub body: ExprId,
    pub module: String,
    pub kind: FnKind,
    pub const_count: CoreNatural,
}

/// Top-level constant declaration (`struct mo_core_const`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConstDecl {
    pub name: String,
    pub ty: TyId,
    pub body: ExprId,
}

// ---------------------------------------------------------------------------
// Compiler-only metadata side tables
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BindingMeta {
    pub node: u32,
    pub slot: u32,
    pub name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClosureMeta {
    pub node: u32,
    pub name: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BodyMeta {
    pub spans: Vec<Option<Span>>,
    pub params: Vec<Option<String>>,
    pub bindings: Vec<BindingMeta>,
    pub closures: Vec<ClosureMeta>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpecLine {
    pub span: Option<Span>,
    pub text: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FnMeta {
    pub name: String,
    pub span: Option<Span>,
    pub text: Option<String>,
    pub body: BodyMeta,
    pub subject: Option<u32>,
    pub allows: Option<bool>,
    pub lines: Vec<SpecLine>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConstMeta {
    pub name: String,
    pub span: Option<Span>,
    pub body: BodyMeta,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExposedType {
    pub index: u32,
    pub contents_exposed: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModuleMeta {
    pub name: String,
    pub data: Vec<u32>,
    pub funs: Vec<u32>,
    pub consts: Vec<u32>,
    pub exposed_types: Vec<ExposedType>,
    pub exposed_funs: Vec<u32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SourceMetadata {
    pub files: Vec<String>,
    /// Optional raw OS path bytes corresponding 1-to-1 with [`Self::files`] when lowered
    /// from a [`mo_base::SourceMap`]. Empty when parsed from UTF-8 Core text.
    pub file_bytes: Vec<Vec<u8>>,
    pub modules: Vec<ModuleMeta>,
    pub funs: Vec<FnMeta>,
    pub consts: Vec<ConstMeta>,
}

/// Explicit source metadata envelope (`noSource` vs `Metadata`).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub enum Metadata {
    #[default]
    NoSource,
    Source(Box<SourceMetadata>),
}

/// Complete Core program (`struct mo_core`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Program {
    pub data: Vec<DataDecl>,
    pub funs: Vec<FnDecl>,
    pub consts: Vec<ConstDecl>,
    pub entry: String,
    pub metadata: Metadata,
}

// ---------------------------------------------------------------------------
// Compiler service descriptors (`src/base/service.h`)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceTool {
    pub name: String,
    pub function: u32,
    pub read_only: bool,
    pub description: String,
    pub argument_names: Vec<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ServiceTimer {
    pub function: u32,
    pub interval: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Service {
    pub start: u32,
    pub token_env: String,
    pub tools: Vec<ServiceTool>,
    pub timers: Vec<ServiceTimer>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceShape {
    pub is_struct: bool,
    pub unnamed: Vec<bool>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Services {
    pub items: Vec<Service>,
    pub data_shapes: Vec<ServiceShape>,
}

// ---------------------------------------------------------------------------
// Lossless evaluation / transport documents (Value, World, Run, Report, ...)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Binding {
    pub name: String,
    pub value: ValueId,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    Int(CoreInteger),
    BigInt(CoreInteger),
    Byte(CoreInteger),
    Decimal(CoreInteger),
    Float(u64),
    Bool(bool),
    Char(char),
    Text(String),
    Bytes(Vec<CoreInteger>),
    Unit,
    Time(CoreInteger),
    Duration(CoreInteger),
    Range {
        start: CoreInteger,
        end: CoreInteger,
    },
    Tuple(Vec<ValueId>),
    List(Vec<ValueId>),
    Map(Vec<(ValueId, ValueId)>),
    Set(Vec<ValueId>),
    Con {
        ctor: String,
        fields: Vec<ValueId>,
    },
    Closure {
        env: Vec<Binding>,
        params: Vec<Param>,
        body: ExprId,
        consts: Vec<Binding>,
    },
    Cap {
        cap: Cap,
        root: Option<String>,
        fake: Option<CoreNatural>,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RandomState {
    pub s0: u64,
    pub s1: u64,
    pub s2: u64,
    pub s3: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CapState {
    pub args: Vec<String>,
    pub env: Vec<(String, String)>,
    pub stdin: Vec<String>,
    pub stdout: Vec<String>,
    pub stderr: Vec<String>,
    pub files: Vec<(String, String)>,
    pub now: CoreInteger,
    pub random: RandomState,
    pub folders: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct World {
    pub args: Vec<String>,
    pub env: Vec<(String, String)>,
    pub stdin: Vec<String>,
    pub stdout: Vec<String>,
    pub stderr: Vec<String>,
    pub files: Vec<(String, String)>,
    pub now: CoreInteger,
    pub random: RandomState,
    pub folders: Vec<String>,
    pub fakes: Vec<CapState>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Bug {
    Overflow(String),
    DivByZero,
    Stop(String),
    Outside(String),
    RepeatedKey,
    FileFolderConflict(String),
    FilePathInvalid(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    Ok {
        value: ValueId,
        world: World,
    },
    Stopped {
        bug: Bug,
        world: World,
    },
    Failed {
        detail: String,
        world: World,
    },
    Jumped {
        name: String,
        values: Vec<ValueId>,
        world: World,
    },
    Wrong(String),
    OutOfFuel,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Effects {
    pub stdout: Vec<String>,
    pub stderr: Vec<String>,
    pub files: Vec<(String, String)>,
    pub folders: Vec<String>,
    pub fakes: Vec<CapState>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Report {
    Returned { value: String, effects: Effects },
    Stopped { bug: Bug, effects: Effects },
    Failed { detail: String, effects: Effects },
    OutOfFuel,
    Wrong(String),
    ParseError(String),
    IllTyped(String),
    InvalidRun(String),
    IoError(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Selection {
    Entry(Vec<ValueId>),
    Test(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Run {
    pub program: Program,
    pub world: World,
    pub selection: Selection,
    pub fuel: CoreNatural,
}

/// All 16 wire document categories (`enum mo_core_text_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CoreTextKind {
    Type,
    Cap,
    Literal,
    Primitive,
    Expr,
    Program,
    Value,
    World,
    Bug,
    Outcome,
    Effects,
    Report,
    Selection,
    Run,
    CapState,
    RandomState,
}

impl CoreTextKind {
    #[must_use]
    pub const fn cli_name(self) -> &'static str {
        match self {
            Self::Type => "type",
            Self::Cap => "cap",
            Self::Literal => "literal",
            Self::Primitive => "primitive",
            Self::Expr => "expr",
            Self::Program => "program",
            Self::Value => "value",
            Self::World => "world",
            Self::Bug => "bug",
            Self::Outcome => "outcome",
            Self::Effects => "effects",
            Self::Report => "report",
            Self::Selection => "selection",
            Self::Run => "run",
            Self::CapState => "cap-state",
            Self::RandomState => "random-state",
        }
    }

    #[must_use]
    pub fn from_cli_name(s: &str) -> Option<Self> {
        match s {
            "type" => Some(Self::Type),
            "cap" => Some(Self::Cap),
            "literal" => Some(Self::Literal),
            "primitive" => Some(Self::Primitive),
            "expr" => Some(Self::Expr),
            "program" => Some(Self::Program),
            "value" => Some(Self::Value),
            "world" => Some(Self::World),
            "bug" => Some(Self::Bug),
            "outcome" => Some(Self::Outcome),
            "effects" => Some(Self::Effects),
            "report" => Some(Self::Report),
            "selection" => Some(Self::Selection),
            "run" => Some(Self::Run),
            "cap-state" => Some(Self::CapState),
            "random-state" => Some(Self::RandomState),
            _ => None,
        }
    }
}

/// Tagged union of any of the 16 `.core` document payloads.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CoreDocument {
    Type(TyId),
    Cap(Cap),
    Literal(Lit),
    Primitive(Prim),
    Expr(ExprId),
    Program(Program),
    Value(ValueId),
    World(World),
    Bug(Bug),
    Outcome(Outcome),
    Effects(Effects),
    Report(Report),
    Selection(Selection),
    Run(Run),
    CapState(CapState),
    RandomState(RandomState),
}

impl CoreDocument {
    #[must_use]
    pub const fn kind(&self) -> CoreTextKind {
        match self {
            Self::Type(_) => CoreTextKind::Type,
            Self::Cap(_) => CoreTextKind::Cap,
            Self::Literal(_) => CoreTextKind::Literal,
            Self::Primitive(_) => CoreTextKind::Primitive,
            Self::Expr(_) => CoreTextKind::Expr,
            Self::Program(_) => CoreTextKind::Program,
            Self::Value(_) => CoreTextKind::Value,
            Self::World(_) => CoreTextKind::World,
            Self::Bug(_) => CoreTextKind::Bug,
            Self::Outcome(_) => CoreTextKind::Outcome,
            Self::Effects(_) => CoreTextKind::Effects,
            Self::Report(_) => CoreTextKind::Report,
            Self::Selection(_) => CoreTextKind::Selection,
            Self::Run(_) => CoreTextKind::Run,
            Self::CapState(_) => CoreTextKind::CapState,
            Self::RandomState(_) => CoreTextKind::RandomState,
        }
    }
}

/// Owned parsed Core document paired with its [`CoreArena`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedCore<T> {
    pub arena: CoreArena,
    pub value: T,
}
