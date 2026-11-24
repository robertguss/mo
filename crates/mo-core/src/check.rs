//! Bidirectional, memoized Mo Core typechecker matching `formal/MoCore/Typecheck.lean`
//! and `src/core/check.c` with explicit heap stacks (O(1) Rust call stack up to
//! `MO_LIMIT_CORE_DEPTH = 4096`) and worst-case O(log n) `BTreeMap` lookups.

use std::collections::BTreeMap;

use mo_base::{DiagBag, Diagnostic, ErrorCode, Span};
use mo_def::core_ops::{
    CoreBase, CoreBigOp, CoreByteOp, CoreBytesOp, CoreCharOp, CoreCryptoOp, CoreDecOp, CoreFloatOp,
    CoreIntOp, CoreIoNullaryOp, CoreListNullaryOp, CoreMapNullaryOp, CoreRangeOp, CoreSetOp,
    CoreTextOp, CoreTimeOp, CoreUnicodeOp,
};
use mo_def::limits::{MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES};

use crate::ir::{
    Cap, CoreArena, Expr, ExprId, FnKind, IoOp, ListOp, Lit, MapOp, Metadata, Param, Prim, Program,
    Services, SimOp, Ty, TyId,
};
use crate::metadata::metadata_valid;

// ---------------------------------------------------------------------------
// Interned types (1-based ID; 0 = unconstrained expected type in memo keys)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
struct InternTyId(u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
enum TyTag {
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
    Tuple,
    List,
    Map,
    Set,
    Data,
    Option,
    Result,
    Ordering,
    FileError,
    Entry,
    EntryKind,
    Fn,
    Cap,
    Simulation,
    SimulationError,
    NetworkError,
    SqliteValue,
    SqliteRows,
    SqliteError,
}

impl TyTag {
    const fn display_name(self) -> &'static str {
        match self {
            Self::Int => "Int",
            Self::BigInt => "BigInt",
            Self::Byte => "Byte",
            Self::Decimal => "Decimal",
            Self::Float => "Float",
            Self::Bool => "Bool",
            Self::Char => "Char",
            Self::Text => "Text",
            Self::Bytes => "Bytes",
            Self::Unit => "Unit",
            Self::Range => "Range",
            Self::Time => "Time",
            Self::Duration => "Duration",
            Self::Tuple => "Tuple",
            Self::List => "List",
            Self::Map => "Map",
            Self::Set => "Set",
            Self::Data => "Data",
            Self::Option => "Option",
            Self::Result => "Result",
            Self::Ordering => "Ordering",
            Self::FileError => "FileError",
            Self::Entry => "Entry",
            Self::EntryKind => "EntryKind",
            Self::Fn => "Fn",
            Self::Cap => "Cap",
            Self::Simulation => "Simulation",
            Self::SimulationError => "SimulationError",
            Self::NetworkError => "NetworkError",
            Self::SqliteValue => "Sqlite.Value",
            Self::SqliteRows => "Sqlite.Rows",
            Self::SqliteError => "SqliteError",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
enum InternKey {
    Compound(TyTag, Option<Cap>, Vec<InternTyId>),
}

struct InternType<'a> {
    tag: TyTag,
    parts: Vec<InternTyId>,
    name: &'a str,
    cap: Cap,
    resolved: Option<Option<usize>>, // cached index into Checker.data_infos
}

struct CtorInfo<'a> {
    name: &'a str,
    fields: Vec<InternTyId>,
}

struct DataInfo<'a> {
    ctors: Vec<CtorInfo<'a>>,
    names: BTreeMap<&'a str, usize>,
}

struct FnInfo {
    params: Vec<InternTyId>,
    ret: InternTyId,
    scope: usize,
}

#[derive(Clone, Copy)]
struct ConstInfo {
    ty: InternTyId,
    index: usize,
}

#[derive(Clone)]
enum SigArgs {
    Fixed([InternTyId; 4]),
    Repeated1(InternTyId),
    Repeated2(InternTyId, InternTyId),
    Selected(Vec<InternTyId>),
}

#[derive(Clone)]
struct Signature {
    ret: InternTyId,
    args: SigArgs,
    count: usize,
    spec: bool,
}

impl Signature {
    fn fixed(ret: InternTyId, count: usize, args: [InternTyId; 4]) -> Self {
        Self {
            ret,
            args: SigArgs::Fixed(args),
            count,
            spec: false,
        }
    }

    fn arg_ty(&self, idx: usize) -> InternTyId {
        match &self.args {
            SigArgs::Fixed(arr) => arr[idx],
            SigArgs::Repeated1(t) => *t,
            SigArgs::Repeated2(k, v) => {
                if idx.is_multiple_of(2) {
                    *k
                } else {
                    *v
                }
            }
            SigArgs::Selected(v) => v[idx],
        }
    }
}

#[derive(Clone, Copy)]
struct Answer {
    ty: Option<InternTyId>,
    expected: Option<InternTyId>,
    code: Option<ErrorCode>,
    at: Option<usize>, // index into prep_exprs
}

impl Answer {
    const fn ok(ty: InternTyId) -> Self {
        Self {
            ty: Some(ty),
            expected: None,
            code: None,
            at: None,
        }
    }

    const fn bad(at: Option<usize>, code: ErrorCode) -> Self {
        Self {
            ty: None,
            expected: None,
            code: Some(code),
            at,
        }
    }

    const fn is_ok(self) -> bool {
        self.code.is_none()
    }
}

struct PrepExpr<'a> {
    source: ExprId,
    children: Vec<usize>,
    node: usize,
    annotation: Option<InternTyId>,
    params: Vec<InternTyId>,
    signature: Option<Signature>,
    parent: Option<usize>,
    bindings: Option<BTreeMap<&'a str, InternTyId>>,
    arm_bindings: Option<BTreeMap<&'a str, InternTyId>>,
    join_scope: Option<(&'a str, usize)>,
    memo: Vec<(u32, Answer)>,
}

struct Checker<'a> {
    arena: &'a CoreArena,
    program: &'a Program,
    services: Option<&'a Services>,

    intern_types: Vec<InternType<'a>>,
    compound_map: BTreeMap<InternKey, InternTyId>,
    nominal_map: BTreeMap<&'a str, InternTyId>,
    annotation_cache: Vec<Option<InternTyId>>,

    data_infos: Vec<DataInfo<'a>>,
    data_map: BTreeMap<&'a str, usize>,
    funs_map: BTreeMap<&'a str, usize>,
    consts_map: BTreeMap<&'a str, ConstInfo>,

    nodes: usize,
    scope: usize,
    spec: bool,
    fatal: Option<ErrorCode>,
    declaration: &'a str,
    declaration_span: Option<Span>,
}

impl<'a> Checker<'a> {
    fn new(arena: &'a CoreArena, program: &'a Program, services: Option<&'a Services>) -> Self {
        Self {
            arena,
            program,
            services,
            intern_types: Vec::new(),
            compound_map: BTreeMap::new(),
            nominal_map: BTreeMap::new(),
            annotation_cache: vec![None; arena.ty_count()],
            data_infos: Vec::new(),
            data_map: BTreeMap::new(),
            funs_map: BTreeMap::new(),
            consts_map: BTreeMap::new(),
            nodes: 0,
            scope: 0,
            spec: false,
            fatal: None,
            declaration: &program.entry,
            declaration_span: None,
        }
    }

    fn ty_info(&self, id: InternTyId) -> &InternType<'a> {
        &self.intern_types[(id.0 - 1) as usize]
    }

    fn intern(
        &mut self,
        tag: TyTag,
        parts: Vec<InternTyId>,
        name: &'a str,
        cap: Cap,
    ) -> InternTyId {
        if tag == TyTag::Data {
            if let Some(&id) = self.nominal_map.get(name) {
                return id;
            }
            let id = InternTyId((self.intern_types.len() + 1) as u32);
            self.intern_types.push(InternType {
                tag,
                parts: Vec::new(),
                name,
                cap: Cap::Console,
                resolved: None,
            });
            self.nominal_map.insert(name, id);
            return id;
        }
        let key = InternKey::Compound(tag, (tag == TyTag::Cap).then_some(cap), parts.clone());
        if let Some(&id) = self.compound_map.get(&key) {
            return id;
        }
        let id = InternTyId((self.intern_types.len() + 1) as u32);
        self.intern_types.push(InternType {
            tag,
            parts,
            name: "",
            cap,
            resolved: None,
        });
        self.compound_map.insert(key, id);
        id
    }

    fn scalar(&mut self, tag: TyTag) -> InternTyId {
        self.intern(tag, Vec::new(), "", Cap::Console)
    }

    fn unary(&mut self, tag: TyTag, t: InternTyId) -> InternTyId {
        self.intern(tag, vec![t], "", Cap::Console)
    }

    fn binary(&mut self, tag: TyTag, a: InternTyId, b: InternTyId) -> InternTyId {
        self.intern(tag, vec![a, b], "", Cap::Console)
    }

    fn cap_ty(&mut self, cap: Cap) -> InternTyId {
        self.intern(TyTag::Cap, Vec::new(), "", cap)
    }

    /// Iterative type interning with exact depth and node-limit checks (`check.c:237`).
    fn annotation(&mut self, root_ty: TyId) -> Option<InternTyId> {
        enum Task {
            Enter(TyId, usize),
            BuildUnary(TyId, TyTag),
            BuildBinary(TyId, TyTag),
            BuildTuple(TyId, usize),
            BuildFn(TyId, usize),
        }

        if let Some(cached) = self
            .annotation_cache
            .get(root_ty.0 as usize)
            .copied()
            .flatten()
        {
            return Some(cached);
        }

        let max_depth = MO_LIMIT_CORE_DEPTH as usize;
        let max_nodes = MO_LIMIT_CORE_NODES as usize;
        let mut stack = vec![Task::Enter(root_ty, 0)];
        let mut results: Vec<InternTyId> = Vec::new();

        while let Some(task) = stack.pop() {
            match task {
                Task::Enter(ty_id, depth) => {
                    if depth >= max_depth {
                        self.fatal = Some(ErrorCode::CoreCheckLimit);
                        return None;
                    }
                    let Some(ty) = self.arena.ty_opt(ty_id) else {
                        self.fatal = Some(ErrorCode::CoreCheckLimit);
                        return None;
                    };
                    if let Some(cached) = self.annotation_cache[ty_id.0 as usize] {
                        results.push(cached);
                        continue;
                    }
                    self.nodes += 1;
                    if self.nodes > max_nodes {
                        self.fatal = Some(ErrorCode::CoreCheckLimit);
                        return None;
                    }
                    match ty {
                        Ty::Int => {
                            let id = self.scalar(TyTag::Int);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::BigInt => {
                            let id = self.scalar(TyTag::BigInt);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Byte => {
                            let id = self.scalar(TyTag::Byte);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Decimal => {
                            let id = self.scalar(TyTag::Decimal);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Float => {
                            let id = self.scalar(TyTag::Float);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Bool => {
                            let id = self.scalar(TyTag::Bool);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Char => {
                            let id = self.scalar(TyTag::Char);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Text => {
                            let id = self.scalar(TyTag::Text);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Bytes => {
                            let id = self.scalar(TyTag::Bytes);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Unit => {
                            let id = self.scalar(TyTag::Unit);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Range => {
                            let id = self.scalar(TyTag::Range);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Time => {
                            let id = self.scalar(TyTag::Time);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Duration => {
                            let id = self.scalar(TyTag::Duration);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Ordering => {
                            let id = self.scalar(TyTag::Ordering);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::FileError => {
                            let id = self.scalar(TyTag::FileError);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Entry => {
                            let id = self.scalar(TyTag::Entry);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::EntryKind => {
                            let id = self.scalar(TyTag::EntryKind);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Simulation => {
                            let id = self.scalar(TyTag::Simulation);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::SimulationError => {
                            let id = self.scalar(TyTag::SimulationError);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::NetworkError => {
                            let id = self.scalar(TyTag::NetworkError);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::SqliteValue => {
                            let id = self.scalar(TyTag::SqliteValue);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::SqliteRows => {
                            let id = self.scalar(TyTag::SqliteRows);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::SqliteError => {
                            let id = self.scalar(TyTag::SqliteError);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Data(name) => {
                            let id = self.intern(TyTag::Data, Vec::new(), name, Cap::Console);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::Cap(cap) => {
                            let id = self.cap_ty(*cap);
                            self.annotation_cache[ty_id.0 as usize] = Some(id);
                            results.push(id);
                        }
                        Ty::List(elem) => {
                            stack.push(Task::BuildUnary(ty_id, TyTag::List));
                            stack.push(Task::Enter(*elem, depth + 1));
                        }
                        Ty::Set(elem) => {
                            stack.push(Task::BuildUnary(ty_id, TyTag::Set));
                            stack.push(Task::Enter(*elem, depth + 1));
                        }
                        Ty::Option(elem) => {
                            stack.push(Task::BuildUnary(ty_id, TyTag::Option));
                            stack.push(Task::Enter(*elem, depth + 1));
                        }
                        Ty::Map { key, value } => {
                            stack.push(Task::BuildBinary(ty_id, TyTag::Map));
                            stack.push(Task::Enter(*value, depth + 1));
                            stack.push(Task::Enter(*key, depth + 1));
                        }
                        Ty::Result { ok, err } => {
                            stack.push(Task::BuildBinary(ty_id, TyTag::Result));
                            stack.push(Task::Enter(*err, depth + 1));
                            stack.push(Task::Enter(*ok, depth + 1));
                        }
                        Ty::Tuple(elems) => {
                            if elems.len() > max_nodes {
                                self.fatal = Some(ErrorCode::CoreCheckLimit);
                                return None;
                            }
                            stack.push(Task::BuildTuple(ty_id, elems.len()));
                            for &e in elems.iter().rev() {
                                stack.push(Task::Enter(e, depth + 1));
                            }
                        }
                        Ty::Fn { params, ret } => {
                            if params.len() + 1 > max_nodes {
                                self.fatal = Some(ErrorCode::CoreCheckLimit);
                                return None;
                            }
                            stack.push(Task::BuildFn(ty_id, params.len()));
                            stack.push(Task::Enter(*ret, depth + 1));
                            for &p in params.iter().rev() {
                                stack.push(Task::Enter(p, depth + 1));
                            }
                        }
                    }
                }
                Task::BuildUnary(ty_id, tag) => {
                    let elem = results.pop()?;
                    let id = self.unary(tag, elem);
                    self.annotation_cache[ty_id.0 as usize] = Some(id);
                    results.push(id);
                }
                Task::BuildBinary(ty_id, tag) => {
                    let b = results.pop()?;
                    let a = results.pop()?;
                    let id = self.binary(tag, a, b);
                    self.annotation_cache[ty_id.0 as usize] = Some(id);
                    results.push(id);
                }
                Task::BuildTuple(ty_id, count) => {
                    let start = results.len().saturating_sub(count);
                    let parts = results.split_off(start);
                    let id = self.intern(TyTag::Tuple, parts, "", Cap::Console);
                    self.annotation_cache[ty_id.0 as usize] = Some(id);
                    results.push(id);
                }
                Task::BuildFn(ty_id, param_count) => {
                    let ret = results.pop()?;
                    let start = results.len().saturating_sub(param_count);
                    let mut parts = results.split_off(start);
                    parts.push(ret);
                    let id = self.intern(TyTag::Fn, parts, "", Cap::Console);
                    self.annotation_cache[ty_id.0 as usize] = Some(id);
                    results.push(id);
                }
            }
        }
        results.pop()
    }

    fn parameter_types(&mut self, params: &[Param]) -> Option<Vec<InternTyId>> {
        if params.len() > MO_LIMIT_CORE_NODES as usize {
            self.fatal = Some(ErrorCode::CoreCheckLimit);
            return None;
        }
        let mut out = Vec::with_capacity(params.len());
        for p in params {
            out.push(self.annotation(p.ty)?);
        }
        Some(out)
    }

    fn literal_ty(&mut self, lit: &Lit) -> Option<InternTyId> {
        let (valid, tag) = match lit {
            Lit::Int(n) => (
                n.magnitude_le(if n.negative {
                    "9223372036854775808"
                } else {
                    "9223372036854775807"
                }),
                TyTag::Int,
            ),
            Lit::BigInt(_) => (true, TyTag::BigInt),
            Lit::Byte(n) => (!n.negative && n.magnitude_le("255"), TyTag::Byte),
            Lit::Decimal(n) => (
                n.magnitude_le("170141183460469231731687303715884105727"),
                TyTag::Decimal,
            ),
            Lit::Float(_) => (true, TyTag::Float),
            Lit::Bool(_) => (true, TyTag::Bool),
            Lit::Char(_) => (true, TyTag::Char),
            Lit::Text(_) => (true, TyTag::Text),
            Lit::Bytes(bs) => (
                bs.iter().all(|b| !b.negative && b.magnitude_le("255")),
                TyTag::Bytes,
            ),
            Lit::Unit => (true, TyTag::Unit),
            Lit::Time(n) => (
                n.magnitude_le(if n.negative {
                    "62135596800000000"
                } else {
                    "253402300799999999"
                }),
                TyTag::Time,
            ),
            Lit::Duration(n) => (
                n.magnitude_le(if n.negative {
                    "9223372036854775808"
                } else {
                    "9223372036854775807"
                }),
                TyTag::Duration,
            ),
        };
        valid.then(|| self.scalar(tag))
    }

    fn simulation_sig(&mut self, op: &SimOp) -> Option<Signature> {
        let handle = self.scalar(TyTag::Simulation);
        let dummy = handle;
        match op {
            SimOp::Advance => {
                let dur = self.scalar(TyTag::Duration);
                let unit = self.scalar(TyTag::Unit);
                Some(Signature::fixed(unit, 2, [handle, dur, dummy, dummy]))
            }
            SimOp::Crash | SimOp::Restart => {
                let unit = self.scalar(TyTag::Unit);
                Some(Signature::fixed(unit, 1, [handle, dummy, dummy, dummy]))
            }
            SimOp::Start { descriptor } => {
                let Some(services) = self.services else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                if services.items.is_empty() {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                let Some(desc_idx) = descriptor.to_usize_bounded(services.items.len() - 1) else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                let svc = &services.items[desc_idx];
                let Some(start_fn) = self.program.funs.get(svc.start as usize) else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                if start_fn.kind != FnKind::Function || !start_fn.params.is_empty() {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                let time = self.scalar(TyTag::Time);
                let mut sig = Signature::fixed(handle, 1, [time, dummy, dummy, dummy]);
                sig.spec = true;
                Some(sig)
            }
            SimOp::Send {
                descriptor,
                tool,
                arguments,
                result,
            } => {
                let Some(services) = self.services else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                if services.items.is_empty() {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                let Some(desc_idx) = descriptor.to_usize_bounded(services.items.len() - 1) else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                let svc = &services.items[desc_idx];
                let Some(start_fn) = self.program.funs.get(svc.start as usize) else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                if start_fn.kind != FnKind::Function || !start_fn.params.is_empty() {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                if svc.tools.is_empty() {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                let Some(tool_idx) = tool.to_usize_bounded(svc.tools.len() - 1) else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                let Some(tool_fn) = self.program.funs.get(svc.tools[tool_idx].function as usize)
                else {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                };
                if tool_fn.kind != FnKind::Function
                    || tool_fn.params.len() < 2
                    || arguments.len() != tool_fn.params.len() - 2
                {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                let start_ret = self.annotation(start_fn.ret)?;
                let param0 = self.annotation(tool_fn.params[0].ty)?;
                let is_time_param1 =
                    matches!(self.arena.ty_opt(tool_fn.params[1].ty), Some(Ty::Time));
                let op_ret = self.annotation(*result)?;
                let fn_ret = self.annotation(tool_fn.ret)?;
                if start_ret != param0 || !is_time_param1 || op_ret != fn_ret {
                    self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                    return None;
                }
                let sim_err = self.scalar(TyTag::SimulationError);
                let ret = self.binary(TyTag::Result, op_ret, sim_err);
                let mut selected = Vec::with_capacity(arguments.len() + 1);
                selected.push(handle);
                for (i, &arg_ty_id) in arguments.iter().enumerate() {
                    let arg_ty = self.annotation(arg_ty_id)?;
                    let expected_param = self.annotation(tool_fn.params[i + 2].ty)?;
                    if arg_ty != expected_param {
                        self.set_fatal_if_unset(ErrorCode::CoreCheckArguments);
                        return None;
                    }
                    selected.push(arg_ty);
                }
                Some(Signature {
                    ret,
                    count: selected.len(),
                    args: SigArgs::Selected(selected),
                    spec: false,
                })
            }
        }
    }

    fn set_fatal_if_unset(&mut self, code: ErrorCode) {
        if self.fatal.is_none() {
            self.fatal = Some(code);
        }
    }

    fn sig0(&mut self, ret: InternTyId) -> Signature {
        Signature::fixed(ret, 0, [ret, ret, ret, ret])
    }

    fn sig1(&mut self, ret: InternTyId, a: InternTyId) -> Signature {
        Signature::fixed(ret, 1, [a, a, a, a])
    }

    fn sig2(&mut self, ret: InternTyId, a: InternTyId, b: InternTyId) -> Signature {
        Signature::fixed(ret, 2, [a, b, a, a])
    }

    fn sig3(&mut self, ret: InternTyId, a: InternTyId, b: InternTyId, c: InternTyId) -> Signature {
        Signature::fixed(ret, 3, [a, b, c, a])
    }

    fn sig4(
        &mut self,
        ret: InternTyId,
        a: InternTyId,
        b: InternTyId,
        c: InternTyId,
        d: InternTyId,
    ) -> Signature {
        Signature::fixed(ret, 4, [a, b, c, d])
    }

    fn primitive_sig(&mut self, prim: &Prim, argc: usize) -> Option<Signature> {
        let s_int = self.scalar(TyTag::Int);
        let s_big = self.scalar(TyTag::BigInt);
        let s_byte = self.scalar(TyTag::Byte);
        let s_dec = self.scalar(TyTag::Decimal);
        let s_float = self.scalar(TyTag::Float);
        let s_bool = self.scalar(TyTag::Bool);
        let s_char = self.scalar(TyTag::Char);
        let s_text = self.scalar(TyTag::Text);
        let s_bytes = self.scalar(TyTag::Bytes);
        let s_range = self.scalar(TyTag::Range);
        let s_time = self.scalar(TyTag::Time);
        let s_dur = self.scalar(TyTag::Duration);

        let sig = match prim {
            Prim::Sim(op) => return self.simulation_sig(op),
            Prim::Int(op) => match op {
                CoreIntOp::Add
                | CoreIntOp::Sub
                | CoreIntOp::Mul
                | CoreIntOp::Div
                | CoreIntOp::Rem
                | CoreIntOp::Min
                | CoreIntOp::Max
                | CoreIntOp::Pow
                | CoreIntOp::FloorDiv
                | CoreIntOp::FloorMod
                | CoreIntOp::WrappingAdd
                | CoreIntOp::WrappingSub
                | CoreIntOp::WrappingMul
                | CoreIntOp::SaturatingAdd
                | CoreIntOp::SaturatingSub
                | CoreIntOp::SaturatingMul => self.sig2(s_int, s_int, s_int),
                CoreIntOp::Neg | CoreIntOp::Abs => self.sig1(s_int, s_int),
                CoreIntOp::CheckedAdd | CoreIntOp::CheckedSub | CoreIntOp::CheckedMul => {
                    let opt = self.unary(TyTag::Option, s_int);
                    self.sig2(opt, s_int, s_int)
                }
                CoreIntOp::ToText => self.sig1(s_text, s_int),
                CoreIntOp::ToDecimal => self.sig1(s_dec, s_int),
                CoreIntOp::ToBig => self.sig1(s_big, s_int),
                CoreIntOp::ToFloat => self.sig1(s_float, s_int),
                CoreIntOp::ToChar => {
                    let opt = self.unary(TyTag::Option, s_char);
                    self.sig1(opt, s_int)
                }
                CoreIntOp::ToByte => {
                    let opt = self.unary(TyTag::Option, s_byte);
                    self.sig1(opt, s_int)
                }
                CoreIntOp::Seconds
                | CoreIntOp::Milliseconds
                | CoreIntOp::Minutes
                | CoreIntOp::Hours
                | CoreIntOp::Days => self.sig1(s_dur, s_int),
            },
            Prim::Big(op) => match op {
                CoreBigOp::Add
                | CoreBigOp::Sub
                | CoreBigOp::Mul
                | CoreBigOp::Div
                | CoreBigOp::Rem
                | CoreBigOp::Min
                | CoreBigOp::Max
                | CoreBigOp::FloorDiv
                | CoreBigOp::FloorMod => self.sig2(s_big, s_big, s_big),
                CoreBigOp::Pow => self.sig2(s_big, s_big, s_int),
                CoreBigOp::Neg | CoreBigOp::Abs => self.sig1(s_big, s_big),
                CoreBigOp::ToInt => {
                    let opt = self.unary(TyTag::Option, s_int);
                    self.sig1(opt, s_big)
                }
                CoreBigOp::ToText => self.sig1(s_text, s_big),
            },
            Prim::Byte(op) => match op {
                CoreByteOp::ToInt => self.sig1(s_int, s_byte),
                CoreByteOp::ToText => self.sig1(s_text, s_byte),
            },
            Prim::Dec(op) => match op {
                CoreDecOp::Add
                | CoreDecOp::Sub
                | CoreDecOp::Mul
                | CoreDecOp::Div
                | CoreDecOp::Rem
                | CoreDecOp::Min
                | CoreDecOp::Max => self.sig2(s_dec, s_dec, s_dec),
                CoreDecOp::Neg | CoreDecOp::Abs | CoreDecOp::Floor | CoreDecOp::Ceil => {
                    self.sig1(s_dec, s_dec)
                }
                CoreDecOp::Round => self.sig2(s_dec, s_dec, s_int),
                CoreDecOp::ToInt => {
                    let opt = self.unary(TyTag::Option, s_int);
                    self.sig1(opt, s_dec)
                }
                CoreDecOp::ToText => self.sig1(s_text, s_dec),
                CoreDecOp::ToFixed => self.sig2(s_text, s_dec, s_int),
                CoreDecOp::ToFloat => self.sig1(s_float, s_dec),
            },
            Prim::Float(op) => match op {
                CoreFloatOp::Add
                | CoreFloatOp::Sub
                | CoreFloatOp::Mul
                | CoreFloatOp::Div
                | CoreFloatOp::Rem => self.sig2(s_float, s_float, s_float),
                CoreFloatOp::Neg
                | CoreFloatOp::Abs
                | CoreFloatOp::Sqrt
                | CoreFloatOp::Floor
                | CoreFloatOp::Ceil
                | CoreFloatOp::Round => self.sig1(s_float, s_float),
                CoreFloatOp::IsNan | CoreFloatOp::IsInfinite => self.sig1(s_bool, s_float),
                CoreFloatOp::ToDecimal => {
                    let opt = self.unary(TyTag::Option, s_dec);
                    self.sig1(opt, s_float)
                }
                CoreFloatOp::ToText => self.sig1(s_text, s_float),
            },
            Prim::Not => self.sig1(s_bool, s_bool),
            Prim::Cmp { base, .. } => {
                let t = match base {
                    CoreBase::Int => s_int,
                    CoreBase::BigInt => s_big,
                    CoreBase::Byte => s_byte,
                    CoreBase::Decimal => s_dec,
                    CoreBase::Float => s_float,
                    CoreBase::Bool => s_bool,
                    CoreBase::Char => s_char,
                    CoreBase::Text => s_text,
                    CoreBase::Bytes => s_bytes,
                    CoreBase::Time => s_time,
                    CoreBase::Duration => s_dur,
                };
                self.sig2(s_bool, t, t)
            }
            Prim::Char(op) => match op {
                CoreCharOp::Code => self.sig1(s_int, s_char),
                CoreCharOp::IsDigit | CoreCharOp::IsLetter | CoreCharOp::IsWhiteSpace => {
                    self.sig1(s_bool, s_char)
                }
                CoreCharOp::Lower | CoreCharOp::Upper => self.sig1(s_char, s_char),
                CoreCharOp::ToText => self.sig1(s_text, s_char),
            },
            Prim::Text(op) => match op {
                CoreTextOp::Concat => self.sig2(s_text, s_text, s_text),
                CoreTextOp::ByteCount | CoreTextOp::CharCount => self.sig1(s_int, s_text),
                CoreTextOp::IsEmpty => self.sig1(s_bool, s_text),
                CoreTextOp::Chars => {
                    let lc = self.unary(TyTag::List, s_char);
                    self.sig1(lc, s_text)
                }
                CoreTextOp::FromChars => {
                    let lc = self.unary(TyTag::List, s_char);
                    self.sig1(s_text, lc)
                }
                CoreTextOp::Lines | CoreTextOp::Words => {
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig1(lt, s_text)
                }
                CoreTextOp::Split => {
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig2(lt, s_text, s_text)
                }
                CoreTextOp::Trim
                | CoreTextOp::TrimStart
                | CoreTextOp::TrimEnd
                | CoreTextOp::Lower
                | CoreTextOp::Upper => self.sig1(s_text, s_text),
                CoreTextOp::StartsWith | CoreTextOp::EndsWith | CoreTextOp::Contains => {
                    self.sig2(s_bool, s_text, s_text)
                }
                CoreTextOp::DropPrefix | CoreTextOp::DropSuffix => {
                    let ot = self.unary(TyTag::Option, s_text);
                    self.sig2(ot, s_text, s_text)
                }
                CoreTextOp::Replace => self.sig3(s_text, s_text, s_text, s_text),
                CoreTextOp::Repeat | CoreTextOp::PadLeft | CoreTextOp::PadRight => {
                    self.sig2(s_text, s_text, s_int)
                }
                CoreTextOp::ToInt => {
                    let o = self.unary(TyTag::Option, s_int);
                    self.sig1(o, s_text)
                }
                CoreTextOp::ToDecimal => {
                    let o = self.unary(TyTag::Option, s_dec);
                    self.sig1(o, s_text)
                }
                CoreTextOp::ToFloat => {
                    let o = self.unary(TyTag::Option, s_float);
                    self.sig1(o, s_text)
                }
                CoreTextOp::Join => {
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig2(s_text, lt, s_text)
                }
                CoreTextOp::Bytes => self.sig1(s_bytes, s_text),
            },
            Prim::Bytes(op) => match op {
                CoreBytesOp::Size => self.sig1(s_int, s_bytes),
                CoreBytesOp::Get => {
                    let ob = self.unary(TyTag::Option, s_byte);
                    self.sig2(ob, s_bytes, s_int)
                }
                CoreBytesOp::DecodeUtf8 => {
                    let ot = self.unary(TyTag::Option, s_text);
                    self.sig1(ot, s_bytes)
                }
                CoreBytesOp::Slice => self.sig3(s_bytes, s_bytes, s_int, s_int),
                CoreBytesOp::Join => {
                    let lb = self.unary(TyTag::List, s_bytes);
                    self.sig1(s_bytes, lb)
                }
                CoreBytesOp::ToHex | CoreBytesOp::ToBase64url => self.sig1(s_text, s_bytes),
                CoreBytesOp::FromHex | CoreBytesOp::FromBase64url => {
                    let ob = self.unary(TyTag::Option, s_bytes);
                    self.sig1(ob, s_text)
                }
                CoreBytesOp::Empty => self.sig0(s_bytes),
                CoreBytesOp::Zeroed
                | CoreBytesOp::FromU16Be
                | CoreBytesOp::FromU32Be
                | CoreBytesOp::FromI64Be => self.sig1(s_bytes, s_int),
                CoreBytesOp::FromList => {
                    let lb = self.unary(TyTag::List, s_byte);
                    self.sig1(s_bytes, lb)
                }
                CoreBytesOp::ToList => {
                    let lb = self.unary(TyTag::List, s_byte);
                    self.sig1(lb, s_bytes)
                }
                CoreBytesOp::U16BeAt | CoreBytesOp::U32BeAt | CoreBytesOp::I64BeAt => {
                    let oi = self.unary(TyTag::Option, s_int);
                    self.sig2(oi, s_bytes, s_int)
                }
                CoreBytesOp::IsEmpty => self.sig1(s_bool, s_bytes),
                CoreBytesOp::CompareSliceAt => {
                    let ord = self.scalar(TyTag::Ordering);
                    let oord = self.unary(TyTag::Option, ord);
                    self.sig4(oord, s_bytes, s_int, s_int, s_bytes)
                }
            },
            Prim::Unicode16(op) => match op {
                CoreUnicodeOp::Graphemes => {
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig1(lt, s_text)
                }
                CoreUnicodeOp::DisplayWidth => self.sig1(s_int, s_text),
                CoreUnicodeOp::NormalizeNfc
                | CoreUnicodeOp::Lowercase
                | CoreUnicodeOp::Uppercase => self.sig1(s_text, s_text),
                CoreUnicodeOp::Letter | CoreUnicodeOp::Digit => self.sig1(s_bool, s_char),
            },
            Prim::Crypto(op) => match op {
                CoreCryptoOp::Sha256 => self.sig1(s_bytes, s_bytes),
                CoreCryptoOp::HmacSha256 => self.sig2(s_bytes, s_bytes, s_bytes),
                CoreCryptoOp::ConstantTimeEq => self.sig2(s_bool, s_bytes, s_bytes),
                CoreCryptoOp::SealRaw | CoreCryptoOp::OpenRaw => {
                    let res = self.binary(TyTag::Result, s_bytes, s_text);
                    self.sig4(res, s_bytes, s_bytes, s_bytes, s_bytes)
                }
                CoreCryptoOp::HashPasswordRaw => {
                    let res = self.binary(TyTag::Result, s_text, s_text);
                    self.sig2(res, s_text, s_bytes)
                }
                CoreCryptoOp::VerifyPassword => self.sig2(s_bool, s_text, s_text),
            },
            Prim::Range(op) => match op {
                CoreRangeOp::Make => self.sig2(s_range, s_int, s_int),
                CoreRangeOp::Start | CoreRangeOp::End => self.sig1(s_int, s_range),
                CoreRangeOp::ToList => {
                    let li = self.unary(TyTag::List, s_int);
                    self.sig1(li, s_range)
                }
                CoreRangeOp::Contains => self.sig2(s_bool, s_range, s_int),
                CoreRangeOp::ToText => self.sig1(s_text, s_range),
            },
            Prim::Time(op) => match op {
                CoreTimeOp::Sub => self.sig2(s_dur, s_time, s_time),
                CoreTimeOp::Add | CoreTimeOp::SubDur => self.sig2(s_time, s_time, s_dur),
                CoreTimeOp::CheckedAdd => {
                    let ot = self.unary(TyTag::Option, s_time);
                    self.sig2(ot, s_time, s_dur)
                }
                CoreTimeOp::ToText => self.sig1(s_text, s_time),
                CoreTimeOp::Parse => {
                    let ot = self.unary(TyTag::Option, s_time);
                    self.sig1(ot, s_text)
                }
                CoreTimeOp::DurAdd | CoreTimeOp::DurSub => self.sig2(s_dur, s_dur, s_dur),
                CoreTimeOp::DurMul => self.sig2(s_dur, s_dur, s_int),
                CoreTimeOp::DurNeg => self.sig1(s_dur, s_dur),
                CoreTimeOp::InMilliseconds => self.sig1(s_int, s_dur),
                CoreTimeOp::InSeconds => self.sig1(s_dec, s_dur),
                CoreTimeOp::DurToText => self.sig1(s_text, s_dur),
            },
            Prim::List { ty, op } => {
                let t = self.annotation(*ty)?;
                let col = self.unary(TyTag::List, t);
                match op {
                    ListOp::Lit(n) => {
                        let count = if n.to_usize_bounded(argc) == Some(argc) {
                            argc
                        } else {
                            argc + 1
                        };
                        Signature {
                            ret: col,
                            args: SigArgs::Repeated1(t),
                            count,
                            spec: false,
                        }
                    }
                    ListOp::Zip(other) => {
                        let v = self.annotation(*other)?;
                        let pair = self.binary(TyTag::Tuple, t, v);
                        let ret = self.unary(TyTag::List, pair);
                        let lv = self.unary(TyTag::List, v);
                        self.sig2(ret, col, lv)
                    }
                    ListOp::Nullary(nop) => match nop {
                        CoreListNullaryOp::Size => self.sig1(s_int, col),
                        CoreListNullaryOp::IsEmpty => self.sig1(s_bool, col),
                        CoreListNullaryOp::Get => {
                            let ot = self.unary(TyTag::Option, t);
                            self.sig2(ot, col, s_int)
                        }
                        CoreListNullaryOp::First | CoreListNullaryOp::Last => {
                            let ot = self.unary(TyTag::Option, t);
                            self.sig1(ot, col)
                        }
                        CoreListNullaryOp::Append | CoreListNullaryOp::Prepend => {
                            self.sig2(col, col, t)
                        }
                        CoreListNullaryOp::Concat => self.sig2(col, col, col),
                        CoreListNullaryOp::Reverse | CoreListNullaryOp::Unique => {
                            self.sig1(col, col)
                        }
                        CoreListNullaryOp::Take | CoreListNullaryOp::Drop => {
                            self.sig2(col, col, s_int)
                        }
                        CoreListNullaryOp::Set => {
                            let ocol = self.unary(TyTag::Option, col);
                            self.sig3(ocol, col, s_int, t)
                        }
                        CoreListNullaryOp::RemoveAt => {
                            let ocol = self.unary(TyTag::Option, col);
                            self.sig2(ocol, col, s_int)
                        }
                        CoreListNullaryOp::Uncons => {
                            let pair = self.binary(TyTag::Tuple, t, col);
                            let opair = self.unary(TyTag::Option, pair);
                            self.sig1(opair, col)
                        }
                        CoreListNullaryOp::Enumerate => {
                            let pair = self.binary(TyTag::Tuple, s_int, t);
                            let ret = self.unary(TyTag::List, pair);
                            self.sig1(ret, col)
                        }
                        CoreListNullaryOp::Contains => self.sig2(s_bool, col, t),
                    },
                }
            }
            Prim::Map { key, value, op } => {
                let k = self.annotation(*key)?;
                let v = self.annotation(*value)?;
                let col = self.binary(TyTag::Map, k, v);
                match op {
                    MapOp::Lit(n) => {
                        let count = if argc.is_multiple_of(2)
                            && n.to_usize_bounded(argc / 2) == Some(argc / 2)
                        {
                            argc
                        } else {
                            argc + 1
                        };
                        Signature {
                            ret: col,
                            args: SigArgs::Repeated2(k, v),
                            count,
                            spec: false,
                        }
                    }
                    MapOp::Nullary(nop) => match nop {
                        CoreMapNullaryOp::Empty => self.sig0(col),
                        CoreMapNullaryOp::Size => self.sig1(s_int, col),
                        CoreMapNullaryOp::IsEmpty => self.sig1(s_bool, col),
                        CoreMapNullaryOp::Get => {
                            let ov = self.unary(TyTag::Option, v);
                            self.sig2(ov, col, k)
                        }
                        CoreMapNullaryOp::Has => self.sig2(s_bool, col, k),
                        CoreMapNullaryOp::Set => self.sig3(col, col, k, v),
                        CoreMapNullaryOp::Remove => self.sig2(col, col, k),
                        CoreMapNullaryOp::Keys => {
                            let lk = self.unary(TyTag::List, k);
                            self.sig1(lk, col)
                        }
                        CoreMapNullaryOp::Values => {
                            let lv = self.unary(TyTag::List, v);
                            self.sig1(lv, col)
                        }
                        CoreMapNullaryOp::Entries => {
                            let pair = self.binary(TyTag::Tuple, k, v);
                            let lp = self.unary(TyTag::List, pair);
                            self.sig1(lp, col)
                        }
                        CoreMapNullaryOp::OfList => {
                            let pair = self.binary(TyTag::Tuple, k, v);
                            let lp = self.unary(TyTag::List, pair);
                            self.sig1(col, lp)
                        }
                    },
                }
            }
            Prim::Set { ty, op } => {
                let t = self.annotation(*ty)?;
                let col = self.unary(TyTag::Set, t);
                match op {
                    CoreSetOp::Empty => self.sig0(col),
                    CoreSetOp::Size => self.sig1(s_int, col),
                    CoreSetOp::IsEmpty => self.sig1(s_bool, col),
                    CoreSetOp::Has => self.sig2(s_bool, col, t),
                    CoreSetOp::Add | CoreSetOp::Remove => self.sig2(col, col, t),
                    CoreSetOp::Union | CoreSetOp::Intersection | CoreSetOp::Difference => {
                        self.sig2(col, col, col)
                    }
                    CoreSetOp::ToList => {
                        let lt = self.unary(TyTag::List, t);
                        self.sig1(lt, col)
                    }
                    CoreSetOp::OfList => {
                        let lt = self.unary(TyTag::List, t);
                        self.sig1(col, lt)
                    }
                }
            }
            Prim::Eq(ty) => {
                let t = self.annotation(*ty)?;
                self.sig2(s_bool, t, t)
            }
            Prim::Structural(ty) => {
                let t = self.annotation(*ty)?;
                self.sig1(s_text, t)
            }
            Prim::Io(op) => self.io_sig(*op),
        };
        Some(sig)
    }

    fn io_sig(&mut self, op: IoOp) -> Signature {
        let s_int = self.scalar(TyTag::Int);
        let s_bool = self.scalar(TyTag::Bool);
        let s_text = self.scalar(TyTag::Text);
        let s_bytes = self.scalar(TyTag::Bytes);
        let s_unit = self.scalar(TyTag::Unit);
        let s_time = self.scalar(TyTag::Time);
        let s_dur = self.scalar(TyTag::Duration);
        let s_ferr = self.scalar(TyTag::FileError);
        let s_entry = self.scalar(TyTag::Entry);
        let s_nerr = self.scalar(TyTag::NetworkError);
        let s_sval = self.scalar(TyTag::SqliteValue);
        let s_srows = self.scalar(TyTag::SqliteRows);
        let s_serr = self.scalar(TyTag::SqliteError);

        let files_cap =
            |this: &mut Self, ro: bool| this.cap_ty(if ro { Cap::ReadFiles } else { Cap::Files });

        let mut spec = false;
        let mut s = match op {
            IoOp::ReadText { ro } => {
                let fc = files_cap(self, ro);
                let res = self.binary(TyTag::Result, s_text, s_ferr);
                self.sig2(res, fc, s_text)
            }
            IoOp::ReadBytes { ro } => {
                let fc = files_cap(self, ro);
                let res = self.binary(TyTag::Result, s_bytes, s_ferr);
                self.sig2(res, fc, s_text)
            }
            IoOp::List { ro } => {
                let fc = files_cap(self, ro);
                let lt = self.unary(TyTag::List, s_text);
                let res = self.binary(TyTag::Result, lt, s_ferr);
                self.sig2(res, fc, s_text)
            }
            IoOp::Entries { ro } => {
                let fc = files_cap(self, ro);
                let le = self.unary(TyTag::List, s_entry);
                let res = self.binary(TyTag::Result, le, s_ferr);
                self.sig2(res, fc, s_text)
            }
            IoOp::Exists { ro } => {
                let fc = files_cap(self, ro);
                self.sig2(s_bool, fc, s_text)
            }
            IoOp::Within { ro } => {
                let fc = files_cap(self, ro);
                self.sig2(fc, fc, s_text)
            }
            IoOp::Nullary(nop) => match nop {
                CoreIoNullaryOp::ArgsList => {
                    let lt = self.unary(TyTag::List, s_text);
                    let c = self.cap_ty(Cap::Args);
                    self.sig1(lt, c)
                }
                CoreIoNullaryOp::EnvGet => {
                    let ot = self.unary(TyTag::Option, s_text);
                    let c = self.cap_ty(Cap::Env);
                    self.sig2(ot, c, s_text)
                }
                CoreIoNullaryOp::Print | CoreIoNullaryOp::PrintError => {
                    let c = self.cap_ty(Cap::Console);
                    self.sig2(s_unit, c, s_text)
                }
                CoreIoNullaryOp::ReadLine => {
                    let ot = self.unary(TyTag::Option, s_text);
                    let c = self.cap_ty(Cap::Console);
                    self.sig1(ot, c)
                }
                CoreIoNullaryOp::Printed | CoreIoNullaryOp::PrintedErrors => {
                    spec = true;
                    let lt = self.unary(TyTag::List, s_text);
                    let c = self.cap_ty(Cap::Console);
                    self.sig1(lt, c)
                }
                CoreIoNullaryOp::WriteText | CoreIoNullaryOp::AppendText => {
                    let c = self.cap_ty(Cap::Files);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig3(res, c, s_text, s_text)
                }
                CoreIoNullaryOp::Remove | CoreIoNullaryOp::MakeFolder => {
                    let c = self.cap_ty(Cap::Files);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig2(res, c, s_text)
                }
                CoreIoNullaryOp::ReadOnly => {
                    let rf = self.cap_ty(Cap::ReadFiles);
                    let f = self.cap_ty(Cap::Files);
                    self.sig1(rf, f)
                }
                CoreIoNullaryOp::Now => {
                    let c = self.cap_ty(Cap::Clock);
                    self.sig1(s_time, c)
                }
                CoreIoNullaryOp::Sleep => {
                    let c = self.cap_ty(Cap::Clock);
                    self.sig2(s_unit, c, s_dur)
                }
                CoreIoNullaryOp::RandomInt => {
                    let c = self.cap_ty(Cap::Random);
                    self.sig3(s_int, c, s_int, s_int)
                }
                CoreIoNullaryOp::ConsoleFake => {
                    spec = true;
                    let c = self.cap_ty(Cap::Console);
                    self.sig0(c)
                }
                CoreIoNullaryOp::ConsoleFakeInput => {
                    spec = true;
                    let c = self.cap_ty(Cap::Console);
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig1(c, lt)
                }
                CoreIoNullaryOp::FilesInMemory => {
                    spec = true;
                    let c = self.cap_ty(Cap::Files);
                    let m = self.binary(TyTag::Map, s_text, s_text);
                    self.sig1(c, m)
                }
                CoreIoNullaryOp::ClockFixed => {
                    spec = true;
                    let c = self.cap_ty(Cap::Clock);
                    self.sig1(c, s_time)
                }
                CoreIoNullaryOp::RandomSeeded => {
                    spec = true;
                    let c = self.cap_ty(Cap::Random);
                    self.sig1(c, s_int)
                }
                CoreIoNullaryOp::ArgsOf => {
                    spec = true;
                    let c = self.cap_ty(Cap::Args);
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig1(c, lt)
                }
                CoreIoNullaryOp::EnvOf => {
                    spec = true;
                    let c = self.cap_ty(Cap::Env);
                    let m = self.binary(TyTag::Map, s_text, s_text);
                    self.sig1(c, m)
                }
                CoreIoNullaryOp::Connect | CoreIoNullaryOp::ConnectSecure => {
                    let net = self.cap_ty(Cap::Network);
                    let conn = self.cap_ty(Cap::Connection);
                    let res = self.binary(TyTag::Result, conn, s_nerr);
                    self.sig4(res, net, s_text, s_int, s_dur)
                }
                CoreIoNullaryOp::Send => {
                    let conn = self.cap_ty(Cap::Connection);
                    let res = self.binary(TyTag::Result, s_unit, s_nerr);
                    self.sig2(res, conn, s_bytes)
                }
                CoreIoNullaryOp::Receive => {
                    let conn = self.cap_ty(Cap::Connection);
                    let res = self.binary(TyTag::Result, s_bytes, s_nerr);
                    self.sig2(res, conn, s_int)
                }
                CoreIoNullaryOp::NetworkFake => {
                    spec = true;
                    let net = self.cap_ty(Cap::Network);
                    let inner = self.binary(TyTag::Map, s_bytes, s_bytes);
                    let outer = self.binary(TyTag::Map, s_text, inner);
                    self.sig1(net, outer)
                }
                CoreIoNullaryOp::AllowHosts => {
                    let net = self.cap_ty(Cap::Network);
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig2(net, net, lt)
                }
                CoreIoNullaryOp::SqliteOpen => {
                    let sq = self.cap_ty(Cap::Sqlite);
                    let db = self.cap_ty(Cap::Database);
                    let res = self.binary(TyTag::Result, db, s_serr);
                    self.sig2(res, sq, s_text)
                }
                CoreIoNullaryOp::SqliteFake => {
                    spec = true;
                    let sq = self.cap_ty(Cap::Sqlite);
                    self.sig0(sq)
                }
                CoreIoNullaryOp::SqliteRun => {
                    let db = self.cap_ty(Cap::Database);
                    let lv = self.unary(TyTag::List, s_sval);
                    let res = self.binary(TyTag::Result, s_unit, s_serr);
                    self.sig3(res, db, s_text, lv)
                }
                CoreIoNullaryOp::SqliteQuery => {
                    let db = self.cap_ty(Cap::Database);
                    let lv = self.unary(TyTag::List, s_sval);
                    let res = self.binary(TyTag::Result, s_srows, s_serr);
                    self.sig3(res, db, s_text, lv)
                }
                CoreIoNullaryOp::SqliteExecuteRaw => {
                    let db = self.cap_ty(Cap::Database);
                    let lv = self.unary(TyTag::List, s_sval);
                    let tup = self.binary(TyTag::Tuple, s_int, s_int);
                    let res = self.binary(TyTag::Result, tup, s_serr);
                    self.sig3(res, db, s_text, lv)
                }
                CoreIoNullaryOp::CommandRunRaw => {
                    let cmd = self.cap_ty(Cap::Command);
                    let lt = self.unary(TyTag::List, s_text);
                    let tup = self.intern(
                        TyTag::Tuple,
                        vec![s_int, s_bytes, s_bytes],
                        "",
                        Cap::Console,
                    );
                    let res = self.binary(TyTag::Result, tup, s_text);
                    self.sig4(res, cmd, s_text, lt, s_dur)
                }
                CoreIoNullaryOp::CommandWithin => {
                    let cmd = self.cap_ty(Cap::Command);
                    self.sig2(cmd, cmd, s_text)
                }
                CoreIoNullaryOp::CommandAllowPrograms => {
                    let cmd = self.cap_ty(Cap::Command);
                    let lt = self.unary(TyTag::List, s_text);
                    self.sig2(cmd, cmd, lt)
                }
                CoreIoNullaryOp::CommandFake => {
                    spec = true;
                    let cmd = self.cap_ty(Cap::Command);
                    let m = self.binary(TyTag::Map, s_bytes, s_bytes);
                    self.sig1(cmd, m)
                }
                CoreIoNullaryOp::SecureRandomBytes => {
                    let sr = self.cap_ty(Cap::SecureRandom);
                    self.sig2(s_bytes, sr, s_int)
                }
                CoreIoNullaryOp::SecureRandomSeeded => {
                    spec = true;
                    let sr = self.cap_ty(Cap::SecureRandom);
                    self.sig1(sr, s_int)
                }
                CoreIoNullaryOp::WriteBytes => {
                    let f = self.cap_ty(Cap::Files);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig3(res, f, s_text, s_bytes)
                }
                CoreIoNullaryOp::Rename => {
                    let f = self.cap_ty(Cap::Files);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig3(res, f, s_text, s_text)
                }
                CoreIoNullaryOp::FileOpen => {
                    let f = self.cap_ty(Cap::Files);
                    let fh = self.cap_ty(Cap::File);
                    let res = self.binary(TyTag::Result, fh, s_ferr);
                    self.sig2(res, f, s_text)
                }
                CoreIoNullaryOp::FileReadAt => {
                    let fh = self.cap_ty(Cap::File);
                    let res = self.binary(TyTag::Result, s_bytes, s_ferr);
                    self.sig3(res, fh, s_int, s_int)
                }
                CoreIoNullaryOp::FileWriteAt => {
                    let fh = self.cap_ty(Cap::File);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig3(res, fh, s_int, s_bytes)
                }
                CoreIoNullaryOp::FileSize => {
                    let fh = self.cap_ty(Cap::File);
                    let res = self.binary(TyTag::Result, s_int, s_ferr);
                    self.sig1(res, fh)
                }
                CoreIoNullaryOp::FileTruncate => {
                    let fh = self.cap_ty(Cap::File);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig2(res, fh, s_int)
                }
                CoreIoNullaryOp::FileSync => {
                    let fh = self.cap_ty(Cap::File);
                    let res = self.binary(TyTag::Result, s_unit, s_ferr);
                    self.sig1(res, fh)
                }
            },
        };
        s.spec = spec;
        s
    }

    fn constructors(&mut self, ty_id: InternTyId) -> Option<usize> {
        if let Some(cached) = self.intern_types[(ty_id.0 - 1) as usize].resolved {
            return cached;
        }
        let tag = self.intern_types[(ty_id.0 - 1) as usize].tag;
        let name = self.intern_types[(ty_id.0 - 1) as usize].name;
        if tag == TyTag::Data {
            let found = self.data_map.get(name).copied();
            self.intern_types[(ty_id.0 - 1) as usize].resolved = Some(found);
            return found;
        }

        let parts = self.intern_types[(ty_id.0 - 1) as usize].parts.clone();
        let ctors: Option<Vec<CtorInfo<'a>>> = match tag {
            TyTag::Option => Some(vec![
                CtorInfo {
                    name: "None",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "Some",
                    fields: vec![parts[0]],
                },
            ]),
            TyTag::Result => Some(vec![
                CtorInfo {
                    name: "Ok",
                    fields: vec![parts[0]],
                },
                CtorInfo {
                    name: "Error",
                    fields: vec![parts[1]],
                },
            ]),
            TyTag::Ordering => Some(vec![
                CtorInfo {
                    name: "Less",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "Equal",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "Greater",
                    fields: Vec::new(),
                },
            ]),
            TyTag::FileError => {
                let s_text = self.scalar(TyTag::Text);
                Some(vec![
                    CtorInfo {
                        name: "NotFound",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "NotAllowed",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "NotText",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "Failed",
                        fields: vec![s_text, s_text],
                    },
                ])
            }
            TyTag::SimulationError => Some(vec![
                CtorInfo {
                    name: "ServiceDown",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "ToolFailed",
                    fields: Vec::new(),
                },
            ]),
            TyTag::NetworkError => {
                let s_text = self.scalar(TyTag::Text);
                let s_int = self.scalar(TyTag::Int);
                Some(vec![
                    CtorInfo {
                        name: "UnknownHost",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "Refused",
                        fields: vec![s_text, s_int],
                    },
                    CtorInfo {
                        name: "TimedOut",
                        fields: Vec::new(),
                    },
                    CtorInfo {
                        name: "Closed",
                        fields: Vec::new(),
                    },
                    CtorInfo {
                        name: "Broken",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "Certificate",
                        fields: vec![s_text],
                    },
                ])
            }
            TyTag::SqliteError => {
                let s_text = self.scalar(TyTag::Text);
                Some(vec![
                    CtorInfo {
                        name: "OutsideFolder",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "NotPermitted",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "SqlFailed",
                        fields: vec![s_text],
                    },
                ])
            }
            TyTag::SqliteValue => {
                let s_int = self.scalar(TyTag::Int);
                let s_float = self.scalar(TyTag::Float);
                let s_text = self.scalar(TyTag::Text);
                let s_bytes = self.scalar(TyTag::Bytes);
                Some(vec![
                    CtorInfo {
                        name: "Null",
                        fields: Vec::new(),
                    },
                    CtorInfo {
                        name: "Integer",
                        fields: vec![s_int],
                    },
                    CtorInfo {
                        name: "Real",
                        fields: vec![s_float],
                    },
                    CtorInfo {
                        name: "String",
                        fields: vec![s_text],
                    },
                    CtorInfo {
                        name: "Blob",
                        fields: vec![s_bytes],
                    },
                ])
            }
            TyTag::SqliteRows => {
                let s_text = self.scalar(TyTag::Text);
                let s_sval = self.scalar(TyTag::SqliteValue);
                let lt = self.unary(TyTag::List, s_text);
                let lv = self.unary(TyTag::List, s_sval);
                let llv = self.unary(TyTag::List, lv);
                Some(vec![CtorInfo {
                    name: "Rows",
                    fields: vec![lt, llv],
                }])
            }
            TyTag::Entry => {
                let s_text = self.scalar(TyTag::Text);
                let s_ek = self.scalar(TyTag::EntryKind);
                Some(vec![CtorInfo {
                    name: "Entry",
                    fields: vec![s_text, s_ek],
                }])
            }
            TyTag::EntryKind => Some(vec![
                CtorInfo {
                    name: "File",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "Folder",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "Link",
                    fields: Vec::new(),
                },
                CtorInfo {
                    name: "Other",
                    fields: Vec::new(),
                },
            ]),
            _ => None,
        };

        let resolved = ctors.map(|ctors| {
            let mut names = BTreeMap::new();
            for (idx, k) in ctors.iter().enumerate() {
                names.entry(k.name).or_insert(idx);
            }
            let data_id = self.data_infos.len();
            self.data_infos.push(DataInfo { ctors, names });
            data_id
        });
        self.intern_types[(ty_id.0 - 1) as usize].resolved = Some(resolved);
        resolved
    }

    /// Iterative expression preparation with exact preorder node numbering and depth checks.
    fn prepare(&mut self, root_expr: ExprId) -> Option<Vec<PrepExpr<'a>>> {
        let max_depth = MO_LIMIT_CORE_DEPTH as usize;
        let max_nodes = MO_LIMIT_CORE_NODES as usize;
        let mut preps: Vec<PrepExpr<'a>> = Vec::new();
        struct PrepTask {
            expr_id: ExprId,
            depth: usize,
            parent_slot: Option<(usize, usize)>,
        }
        let mut stack: Vec<PrepTask> = vec![PrepTask {
            expr_id: root_expr,
            depth: 0,
            parent_slot: None,
        }];

        while let Some(PrepTask {
            expr_id,
            depth,
            parent_slot,
        }) = stack.pop()
        {
            if depth >= max_depth {
                self.fatal = Some(ErrorCode::CoreCheckLimit);
                return None;
            }
            let Some(expr) = self.arena.expr_opt(expr_id) else {
                self.fatal = Some(ErrorCode::CoreCheckLimit);
                return None;
            };
            self.nodes += 1;
            if self.nodes > max_nodes {
                self.fatal = Some(ErrorCode::CoreCheckLimit);
                return None;
            }

            let prep_idx = preps.len();
            if let Some((parent_idx, slot)) = parent_slot {
                preps[parent_idx].children[slot] = prep_idx;
            }

            let mut annotation = None;
            let mut params = Vec::new();
            let mut signature = None;
            let mut child_exprs: Vec<ExprId> = Vec::new();

            match expr {
                Expr::Lit(lit) => {
                    annotation = self.literal_ty(lit);
                }
                Expr::Var(_) | Expr::Global(_) | Expr::Stop(_) => {}
                Expr::Let { bound, body, .. } => {
                    child_exprs.push(*bound);
                    child_exprs.push(*body);
                }
                Expr::Lam {
                    params: lam_params,
                    body,
                } => {
                    params = self.parameter_types(lam_params)?;
                    child_exprs.push(*body);
                }
                Expr::App { func, args } => {
                    child_exprs.reserve(1 + args.len());
                    child_exprs.push(*func);
                    child_exprs.extend_from_slice(args);
                }
                Expr::Call { args, .. } | Expr::Tuple(args) | Expr::Jump { args, .. } => {
                    child_exprs.extend_from_slice(args);
                }
                Expr::Proj { expr: inner, .. } | Expr::Fail(inner) => {
                    child_exprs.push(*inner);
                }
                Expr::Con { ty, args, .. } => {
                    annotation = self.annotation(*ty);
                    child_exprs.extend_from_slice(args);
                }
                Expr::Case { expr: scrut, arms } => {
                    child_exprs.reserve(1 + arms.len());
                    child_exprs.push(*scrut);
                    for a in arms {
                        child_exprs.push(a.body);
                    }
                }
                Expr::If { cond, then_, else_ } => {
                    child_exprs.push(*cond);
                    child_exprs.push(*then_);
                    child_exprs.push(*else_);
                }
                Expr::Prim { prim, args } => {
                    child_exprs.extend_from_slice(args);
                    signature = self.primitive_sig(prim, args.len());
                }
                Expr::Join {
                    params: j_params,
                    body,
                    rest,
                    ..
                } => {
                    params = self.parameter_types(j_params)?;
                    child_exprs.push(*body);
                    child_exprs.push(*rest);
                }
            }
            if self.fatal.is_some() {
                return None;
            }
            if child_exprs.len() > max_nodes {
                self.fatal = Some(ErrorCode::CoreCheckLimit);
                return None;
            }

            let child_count = child_exprs.len();
            preps.push(PrepExpr {
                source: expr_id,
                children: vec![0; child_count],
                node: prep_idx,
                annotation,
                params,
                signature,
                parent: parent_slot.map(|(p, _)| p),
                bindings: None,
                arm_bindings: None,
                join_scope: None,
                memo: Vec::new(),
            });

            // Push children in reverse so preorder numbering matches recursive traversal.
            for (slot, cid) in child_exprs.into_iter().enumerate().rev() {
                stack.push(PrepTask {
                    expr_id: cid,
                    depth: depth + 1,
                    parent_slot: Some((prep_idx, slot)),
                });
            }
        }

        Some(preps)
    }

    fn declarations(&mut self) -> Option<Vec<FnInfo>> {
        let p = self.program;
        if p.funs.len() > MO_LIMIT_CORE_NODES as usize
            || p.consts.len() > MO_LIMIT_CORE_NODES as usize
            || p.data.len() > MO_LIMIT_CORE_NODES as usize
        {
            self.fatal = Some(ErrorCode::CoreCheckLimit);
            return None;
        }

        for d in &p.data {
            let mut ctors = Vec::with_capacity(d.ctors.len());
            let mut names = BTreeMap::new();
            for (j, ctor) in d.ctors.iter().enumerate() {
                let fields = self.parameter_types(&ctor.fields)?;
                names.entry(ctor.name.as_str()).or_insert(j);
                ctors.push(CtorInfo {
                    name: &ctor.name,
                    fields,
                });
            }
            let data_id = self.data_infos.len();
            self.data_infos.push(DataInfo { ctors, names });
            self.data_map.entry(d.name.as_str()).or_insert(data_id);
        }

        for (i, k) in p.consts.iter().enumerate() {
            self.declaration = &k.name;
            self.declaration_span = match &p.metadata {
                Metadata::Source(m) => m.consts[i].span,
                Metadata::NoSource => None,
            };
            if self.consts_map.contains_key(k.name.as_str()) {
                self.fatal = Some(ErrorCode::CoreCheckConstantName);
                return None;
            }
            let ty = self.annotation(k.ty)?;
            self.consts_map.insert(&k.name, ConstInfo { ty, index: i });
        }

        let mut fun_infos = Vec::with_capacity(p.funs.len());
        for (i, f) in p.funs.iter().enumerate() {
            self.declaration = &f.name;
            self.declaration_span = match &p.metadata {
                Metadata::Source(m) => m.funs[i].span,
                Metadata::NoSource => None,
            };
            let params = self.parameter_types(&f.params)?;
            let ret = self.annotation(f.ret)?;
            let Some(scope) = f.const_count.to_usize_bounded(p.consts.len()) else {
                self.fatal = Some(ErrorCode::CoreCheckPrefix);
                return None;
            };
            fun_infos.push(FnInfo { params, ret, scope });
            self.funs_map.entry(f.name.as_str()).or_insert(i);
        }

        Some(fun_infos)
    }
}

// ---------------------------------------------------------------------------
// Iterative bidirectional memoized typechecker (`src/core/check.c:tc`)
// ---------------------------------------------------------------------------

enum VarScope<'a> {
    Root(BTreeMap<&'a str, InternTyId>),
    LetOrLam { expr: usize, outer: usize },
    Arm { body_expr: usize, outer: usize },
}

struct JoinScope {
    expr: usize,
    outer: Option<usize>,
}

enum TcState {
    Start,
    LetBound,
    LetBody,
    LamBody,
    AppFunc,
    AppArg {
        idx: usize,
        func_ty: InternTyId,
    },
    CallArg {
        idx: usize,
        fn_idx: usize,
    },
    TupleElem {
        idx: usize,
        elems: Vec<InternTyId>,
    },
    ProjExpr,
    ConArg {
        idx: usize,
        data_id: usize,
        ctor_idx: usize,
    },
    CaseScrutinee,
    CaseInferArm {
        arm_idx: usize,
        data_id: usize,
    },
    CaseCheckArm {
        arm_idx: usize,
        data_id: usize,
        arm_exp: InternTyId,
    },
    IfCond,
    IfExpThen,
    IfExpElse,
    IfInferThen,
    IfInferElseThenOk,
    IfInferElse,
    IfInferThenElseOk,
    PrimArg {
        idx: usize,
    },
    FailArg,
    JoinExpBody {
        inner_joins: usize,
    },
    JoinExpRest,
    JoinInferRest {
        local_ctx: usize,
        inner_joins: usize,
    },
    JoinInferBodyFromRest,
    JoinInferBodyFallback {
        inner_joins: usize,
    },
    JoinInferRestFromBody,
    JumpArg {
        idx: usize,
        join_expr: usize,
    },
}

struct TcFrame {
    expr: usize,
    ctx: usize,
    joins: Option<usize>,
    exp: Option<InternTyId>,
    state: TcState,
}

fn expect_answer(at: usize, exp: Option<InternTyId>, t: Option<InternTyId>) -> Answer {
    let Some(t) = t else {
        return Answer::bad(Some(at), ErrorCode::CoreCheckInference);
    };
    if let Some(e) = exp
        && e != t
    {
        return Answer {
            ty: Some(t),
            expected: Some(e),
            code: Some(ErrorCode::CoreCheckType),
            at: Some(at),
        };
    }
    Answer::ok(t)
}

fn lookup_var<'a>(
    scopes: &[VarScope<'a>],
    preps: &[PrepExpr<'a>],
    mut scope_id: usize,
    name: &str,
) -> Option<InternTyId> {
    loop {
        match &scopes[scope_id] {
            VarScope::Root(map) => return map.get(name).copied(),
            VarScope::LetOrLam { expr, outer } => {
                if let Some(ref map) = preps[*expr].bindings
                    && let Some(&ty) = map.get(name)
                {
                    return Some(ty);
                }
                scope_id = *outer;
            }
            VarScope::Arm { body_expr, outer } => {
                if let Some(ref map) = preps[*body_expr].arm_bindings
                    && let Some(&ty) = map.get(name)
                {
                    return Some(ty);
                }
                scope_id = *outer;
            }
        }
    }
}

fn lookup_join(
    joins: &[JoinScope],
    preps: &[PrepExpr<'_>],
    mut join_id: Option<usize>,
    name: &str,
) -> Option<usize> {
    while let Some(id) = join_id {
        let node = &joins[id];
        if let Some((jname, target)) = preps[node.expr].join_scope
            && jname == name
        {
            return Some(target);
        }
        join_id = node.outer;
    }
    None
}

fn memo_lookup(prep: &PrepExpr<'_>, exp: Option<InternTyId>) -> Option<Answer> {
    let key = exp.map_or(0, |t| t.0);
    prep.memo
        .iter()
        .find_map(|&(k, ans)| (k == key).then_some(ans))
}

fn memo_store(prep: &mut PrepExpr<'_>, exp: Option<InternTyId>, ans: Answer) {
    let key = exp.map_or(0, |t| t.0);
    prep.memo.push((key, ans));
}

fn check_body<'a>(
    c: &mut Checker<'a>,
    fun_infos: &[FnInfo],
    body: ExprId,
    params: &'a [Param],
    param_tys: &[InternTyId],
    ret_ty: InternTyId,
) -> (Answer, Vec<PrepExpr<'a>>) {
    let Some(mut preps) = c.prepare(body) else {
        let code = c.fatal.unwrap_or(ErrorCode::CoreCheckLimit);
        return (Answer::bad(None, code), Vec::new());
    };

    let mut root_map = BTreeMap::new();
    for (p, &ty) in params.iter().zip(param_tys) {
        root_map.entry(p.name.as_str()).or_insert(ty);
    }
    let mut var_scopes: Vec<VarScope<'a>> = vec![VarScope::Root(root_map)];
    let mut join_scopes: Vec<JoinScope> = Vec::new();

    let mut stack: Vec<TcFrame> = vec![TcFrame {
        expr: 0,
        ctx: 0,
        joins: None,
        exp: Some(ret_ty),
        state: TcState::Start,
    }];
    let mut child_result: Option<Answer> = None;

    macro_rules! push_child {
        ($child_expr:expr, $ctx:expr, $joins:expr, $exp:expr) => {{
            if let Some(fatal) = c.fatal {
                child_result = Some(Answer::bad(Some($child_expr), fatal));
            } else if let Some(cached) = memo_lookup(&preps[$child_expr], $exp) {
                child_result = Some(cached);
            } else {
                stack.push(TcFrame {
                    expr: $child_expr,
                    ctx: $ctx,
                    joins: $joins,
                    exp: $exp,
                    state: TcState::Start,
                });
                child_result = None;
            }
        }};
    }

    macro_rules! finish_frame {
        ($frame:expr, $ans:expr) => {{
            let final_ans = if let Some(fatal) = c.fatal {
                Answer::bad(Some($frame.expr), fatal)
            } else {
                $ans
            };
            memo_store(&mut preps[$frame.expr], $frame.exp, final_ans);
            child_result = Some(final_ans);
        }};
    }

    while let Some(mut frame) = stack.pop() {
        let e_idx = frame.expr;
        let expr = c.arena.expr(preps[e_idx].source);

        match frame.state {
            TcState::Start => {
                if let Some(fatal) = c.fatal {
                    child_result = Some(Answer::bad(Some(e_idx), fatal));
                    continue;
                }
                if let Some(cached) = memo_lookup(&preps[e_idx], frame.exp) {
                    child_result = Some(cached);
                    continue;
                }
                match expr {
                    Expr::Lit(_) => {
                        let ans = match preps[e_idx].annotation {
                            None => Answer::bad(Some(e_idx), ErrorCode::CoreCheckLiteral),
                            Some(ty) => expect_answer(e_idx, frame.exp, Some(ty)),
                        };
                        finish_frame!(frame, ans);
                    }
                    Expr::Var(name) => {
                        let ans = match lookup_var(&var_scopes, &preps, frame.ctx, name) {
                            None => Answer::bad(Some(e_idx), ErrorCode::CoreCheckVariable),
                            Some(ty) => expect_answer(e_idx, frame.exp, Some(ty)),
                        };
                        finish_frame!(frame, ans);
                    }
                    Expr::Global(name) => {
                        let ans = match c.consts_map.get(name.as_str()) {
                            Some(k) if k.index < c.scope => {
                                expect_answer(e_idx, frame.exp, Some(k.ty))
                            }
                            _ => Answer::bad(Some(e_idx), ErrorCode::CoreCheckGlobal),
                        };
                        finish_frame!(frame, ans);
                    }
                    Expr::Let { .. } => {
                        let bound_idx = preps[e_idx].children[0];
                        frame.state = TcState::LetBound;
                        let ctx = frame.ctx;
                        stack.push(frame);
                        push_child!(bound_idx, ctx, None, None);
                    }
                    Expr::Lam {
                        params: lam_params, ..
                    } => {
                        let n = lam_params.len();
                        if let Some(exp_ty) = frame.exp {
                            let info = c.ty_info(exp_ty);
                            let mut mismatch = info.tag != TyTag::Fn || info.parts.len() != n + 1;
                            if !mismatch {
                                for i in 0..n {
                                    if preps[e_idx].params[i] != info.parts[i] {
                                        mismatch = true;
                                        break;
                                    }
                                }
                            }
                            if mismatch {
                                finish_frame!(
                                    frame,
                                    Answer::bad(Some(e_idx), ErrorCode::CoreCheckLambda)
                                );
                                continue;
                            }
                        }
                        if preps[e_idx].bindings.is_none() {
                            let mut map = BTreeMap::new();
                            for (p, &ty) in lam_params.iter().zip(&preps[e_idx].params) {
                                map.entry(p.name.as_str()).or_insert(ty);
                            }
                            preps[e_idx].bindings = Some(map);
                        }
                        let local_ctx = var_scopes.len();
                        var_scopes.push(VarScope::LetOrLam {
                            expr: e_idx,
                            outer: frame.ctx,
                        });
                        let body_exp = frame.exp.map(|t| c.ty_info(t).parts[n]);
                        let body_idx = preps[e_idx].children[0];
                        frame.state = TcState::LamBody;
                        stack.push(frame);
                        push_child!(body_idx, local_ctx, None, body_exp);
                    }
                    Expr::App { .. } => {
                        let func_idx = preps[e_idx].children[0];
                        let ctx = frame.ctx;
                        frame.state = TcState::AppFunc;
                        stack.push(frame);
                        push_child!(func_idx, ctx, None, None);
                    }
                    Expr::Call { name, .. } => {
                        let Some(&fn_idx) = c.funs_map.get(name.as_str()) else {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckFunction)
                            );
                            continue;
                        };
                        let f_info = &fun_infos[fn_idx];
                        let f_decl = &c.program.funs[fn_idx];
                        if f_info.scope > c.scope {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckCallPrefix)
                            );
                            continue;
                        }
                        if !c.spec && f_decl.kind != FnKind::Function {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckSpecCall)
                            );
                            continue;
                        }
                        if preps[e_idx].children.len() != f_info.params.len() {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckArguments)
                            );
                            continue;
                        }
                        if f_info.params.is_empty() {
                            finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(f_info.ret)));
                        } else {
                            let first_arg = preps[e_idx].children[0];
                            let first_ty = f_info.params[0];
                            let ctx = frame.ctx;
                            frame.state = TcState::CallArg { idx: 0, fn_idx };
                            stack.push(frame);
                            push_child!(first_arg, ctx, None, Some(first_ty));
                        }
                    }
                    Expr::Tuple(_) => {
                        if preps[e_idx].children.is_empty() {
                            let tup_ty = c.intern(TyTag::Tuple, Vec::new(), "", Cap::Console);
                            finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(tup_ty)));
                        } else {
                            let first = preps[e_idx].children[0];
                            let count = preps[e_idx].children.len();
                            let ctx = frame.ctx;
                            frame.state = TcState::TupleElem {
                                idx: 0,
                                elems: Vec::with_capacity(count),
                            };
                            stack.push(frame);
                            push_child!(first, ctx, None, None);
                        }
                    }
                    Expr::Proj { .. } => {
                        let inner = preps[e_idx].children[0];
                        let ctx = frame.ctx;
                        frame.state = TcState::ProjExpr;
                        stack.push(frame);
                        push_child!(inner, ctx, None, None);
                    }
                    Expr::Con { ctor, .. } => {
                        let Some(ty) = preps[e_idx].annotation else {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckConstructors)
                            );
                            continue;
                        };
                        let Some(data_id) = c.constructors(ty) else {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckConstructors)
                            );
                            continue;
                        };
                        let Some(&ctor_idx) = c.data_infos[data_id].names.get(ctor.as_str()) else {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckConstructor)
                            );
                            continue;
                        };
                        let field_count = c.data_infos[data_id].ctors[ctor_idx].fields.len();
                        if preps[e_idx].children.len() != field_count {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckArguments)
                            );
                            continue;
                        }
                        if field_count == 0 {
                            finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(ty)));
                        } else {
                            let first_arg = preps[e_idx].children[0];
                            let first_ty = c.data_infos[data_id].ctors[ctor_idx].fields[0];
                            let ctx = frame.ctx;
                            frame.state = TcState::ConArg {
                                idx: 0,
                                data_id,
                                ctor_idx,
                            };
                            stack.push(frame);
                            push_child!(first_arg, ctx, None, Some(first_ty));
                        }
                    }
                    Expr::Case { .. } => {
                        let scrut = preps[e_idx].children[0];
                        let ctx = frame.ctx;
                        frame.state = TcState::CaseScrutinee;
                        stack.push(frame);
                        push_child!(scrut, ctx, None, None);
                    }
                    Expr::If { .. } => {
                        let cond = preps[e_idx].children[0];
                        let bool_ty = c.scalar(TyTag::Bool);
                        let ctx = frame.ctx;
                        frame.state = TcState::IfCond;
                        stack.push(frame);
                        push_child!(cond, ctx, None, Some(bool_ty));
                    }
                    Expr::Prim { .. } => {
                        let Some(ref sig) = preps[e_idx].signature else {
                            let code = c.fatal.unwrap_or(ErrorCode::CoreCheckArguments);
                            finish_frame!(frame, Answer::bad(Some(e_idx), code));
                            continue;
                        };
                        if sig.spec && !c.spec {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckSpecPrimitive)
                            );
                            continue;
                        }
                        if sig.count != preps[e_idx].children.len() {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckArguments)
                            );
                            continue;
                        }
                        if preps[e_idx].children.is_empty() {
                            let ret = sig.ret;
                            finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(ret)));
                        } else {
                            let first_arg = preps[e_idx].children[0];
                            let first_ty = sig.arg_ty(0);
                            let ctx = frame.ctx;
                            frame.state = TcState::PrimArg { idx: 0 };
                            stack.push(frame);
                            push_child!(first_arg, ctx, None, Some(first_ty));
                        }
                    }
                    Expr::Stop(_) => {
                        finish_frame!(frame, expect_answer(e_idx, frame.exp, frame.exp));
                    }
                    Expr::Fail(_) => {
                        if !c.spec {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckFail)
                            );
                            continue;
                        }
                        let inner = preps[e_idx].children[0];
                        let text_ty = c.scalar(TyTag::Text);
                        let ctx = frame.ctx;
                        frame.state = TcState::FailArg;
                        stack.push(frame);
                        push_child!(inner, ctx, None, Some(text_ty));
                    }
                    Expr::Join {
                        name,
                        params: j_params,
                        ..
                    } => {
                        if preps[e_idx].bindings.is_none() {
                            let mut map = BTreeMap::new();
                            for (p, &ty) in j_params.iter().zip(&preps[e_idx].params) {
                                map.entry(p.name.as_str()).or_insert(ty);
                            }
                            preps[e_idx].bindings = Some(map);
                            preps[e_idx].join_scope = Some((name.as_str(), e_idx));
                        }
                        let local_ctx = var_scopes.len();
                        var_scopes.push(VarScope::LetOrLam {
                            expr: e_idx,
                            outer: frame.ctx,
                        });
                        let inner_joins = join_scopes.len();
                        join_scopes.push(JoinScope {
                            expr: e_idx,
                            outer: frame.joins,
                        });
                        if let Some(exp_ty) = frame.exp {
                            let body_idx = preps[e_idx].children[0];
                            frame.state = TcState::JoinExpBody { inner_joins };
                            stack.push(frame);
                            push_child!(body_idx, local_ctx, Some(inner_joins), Some(exp_ty));
                        } else {
                            let rest_idx = preps[e_idx].children[1];
                            let ctx = frame.ctx;
                            frame.state = TcState::JoinInferRest {
                                local_ctx,
                                inner_joins,
                            };
                            stack.push(frame);
                            push_child!(rest_idx, ctx, Some(inner_joins), None);
                        }
                    }
                    Expr::Jump { name, .. } => {
                        let Some(join_expr) = lookup_join(&join_scopes, &preps, frame.joins, name)
                        else {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckJump)
                            );
                            continue;
                        };
                        let param_count = preps[join_expr].params.len();
                        if preps[e_idx].children.len() != param_count {
                            finish_frame!(
                                frame,
                                Answer::bad(Some(e_idx), ErrorCode::CoreCheckArguments)
                            );
                            continue;
                        }
                        if param_count == 0 {
                            finish_frame!(frame, expect_answer(e_idx, frame.exp, frame.exp));
                        } else {
                            let first_arg = preps[e_idx].children[0];
                            let first_ty = preps[join_expr].params[0];
                            let ctx = frame.ctx;
                            frame.state = TcState::JumpArg { idx: 0, join_expr };
                            stack.push(frame);
                            push_child!(first_arg, ctx, None, Some(first_ty));
                        }
                    }
                }
            }

            TcState::LetBound => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                if let Expr::Let { name, .. } = expr
                    && preps[e_idx].bindings.is_none()
                    && let Some(bound_ty) = ans.ty
                {
                    let mut map = BTreeMap::new();
                    map.insert(name.as_str(), bound_ty);
                    preps[e_idx].bindings = Some(map);
                }
                let local_ctx = var_scopes.len();
                var_scopes.push(VarScope::LetOrLam {
                    expr: e_idx,
                    outer: frame.ctx,
                });
                let body_idx = preps[e_idx].children[1];
                let joins = frame.joins;
                let exp = frame.exp;
                frame.state = TcState::LetBody;
                stack.push(frame);
                push_child!(body_idx, local_ctx, joins, exp);
            }

            TcState::LetBody => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                finish_frame!(frame, ans);
            }

            TcState::LamBody => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                if frame.exp.is_some() {
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, frame.exp));
                } else if let Some(ret_ty) = ans.ty {
                    let mut parts = preps[e_idx].params.clone();
                    parts.push(ret_ty);
                    let fn_ty = c.intern(TyTag::Fn, parts, "", Cap::Console);
                    finish_frame!(frame, expect_answer(e_idx, None, Some(fn_ty)));
                } else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckInference)
                    );
                }
            }

            TcState::AppFunc => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let Some(func_ty) = ans.ty else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckApplication)
                    );
                    continue;
                };
                let info = c.ty_info(func_ty);
                if info.tag != TyTag::Fn {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckApplication)
                    );
                    continue;
                }
                let param_count = info.parts.len() - 1;
                if preps[e_idx].children.len() - 1 != param_count {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckArguments)
                    );
                    continue;
                }
                if param_count == 0 {
                    let ret = info.parts[0];
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(ret)));
                } else {
                    let first_arg = preps[e_idx].children[1];
                    let first_ty = info.parts[0];
                    let ctx = frame.ctx;
                    frame.state = TcState::AppArg { idx: 0, func_ty };
                    stack.push(frame);
                    push_child!(first_arg, ctx, None, Some(first_ty));
                }
            }

            TcState::AppArg { idx, func_ty } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let info = c.ty_info(func_ty);
                let param_count = info.parts.len() - 1;
                let next = idx + 1;
                if next < param_count {
                    let next_arg = preps[e_idx].children[1 + next];
                    let next_ty = info.parts[next];
                    let ctx = frame.ctx;
                    frame.state = TcState::AppArg { idx: next, func_ty };
                    stack.push(frame);
                    push_child!(next_arg, ctx, None, Some(next_ty));
                } else {
                    let ret = info.parts[param_count];
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(ret)));
                }
            }

            TcState::CallArg { idx, fn_idx } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let f_info = &fun_infos[fn_idx];
                let next = idx + 1;
                if next < f_info.params.len() {
                    let next_arg = preps[e_idx].children[next];
                    let next_ty = f_info.params[next];
                    let ctx = frame.ctx;
                    frame.state = TcState::CallArg { idx: next, fn_idx };
                    stack.push(frame);
                    push_child!(next_arg, ctx, None, Some(next_ty));
                } else {
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(f_info.ret)));
                }
            }

            TcState::TupleElem { idx, mut elems } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let Some(elem_ty) = ans.ty else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckInference)
                    );
                    continue;
                };
                elems.push(elem_ty);
                let next = idx + 1;
                if next < preps[e_idx].children.len() {
                    let next_elem = preps[e_idx].children[next];
                    let ctx = frame.ctx;
                    frame.state = TcState::TupleElem { idx: next, elems };
                    stack.push(frame);
                    push_child!(next_elem, ctx, None, None);
                } else {
                    let tup_ty = c.intern(TyTag::Tuple, elems, "", Cap::Console);
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(tup_ty)));
                }
            }

            TcState::ProjExpr => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let Expr::Proj { index, .. } = expr else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckProjection)
                    );
                    continue;
                };
                let Some(tup_ty) = ans.ty else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckProjection)
                    );
                    continue;
                };
                let info = c.ty_info(tup_ty);
                if info.tag != TyTag::Tuple || info.parts.is_empty() {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckProjection)
                    );
                    continue;
                }
                let Some(part_idx) = index.to_usize_bounded(info.parts.len() - 1) else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckProjection)
                    );
                    continue;
                };
                let part_ty = info.parts[part_idx];
                finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(part_ty)));
            }

            TcState::ConArg {
                idx,
                data_id,
                ctor_idx,
            } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let next = idx + 1;
                let fields = &c.data_infos[data_id].ctors[ctor_idx].fields;
                if next < fields.len() {
                    let next_arg = preps[e_idx].children[next];
                    let next_ty = fields[next];
                    let ctx = frame.ctx;
                    frame.state = TcState::ConArg {
                        idx: next,
                        data_id,
                        ctor_idx,
                    };
                    stack.push(frame);
                    push_child!(next_arg, ctx, None, Some(next_ty));
                } else {
                    finish_frame!(
                        frame,
                        expect_answer(e_idx, frame.exp, preps[e_idx].annotation)
                    );
                }
            }

            TcState::CaseScrutinee => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let Some(scrut_ty) = ans.ty else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckConstructors)
                    );
                    continue;
                };
                let Some(data_id) = c.constructors(scrut_ty) else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckConstructors)
                    );
                    continue;
                };
                let Expr::Case { arms, .. } = expr else {
                    finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArms));
                    continue;
                };
                let ctor_count = c.data_infos[data_id].ctors.len();
                if arms.len() != ctor_count {
                    finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArms));
                    continue;
                }
                if ctor_count == 0 {
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, frame.exp));
                    continue;
                }
                if let Some(arm_exp) = frame.exp {
                    // Check arm 0 against arm_exp.
                    let ctor = &c.data_infos[data_id].ctors[0];
                    if arms[0].ctor != ctor.name || arms[0].names.len() != ctor.fields.len() {
                        finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArm));
                        continue;
                    }
                    let body_idx = preps[e_idx].children[1];
                    if preps[body_idx].arm_bindings.is_none() {
                        let mut map = BTreeMap::new();
                        for (nm, &fty) in arms[0].names.iter().zip(&ctor.fields) {
                            map.entry(nm.as_str()).or_insert(fty);
                        }
                        preps[body_idx].arm_bindings = Some(map);
                    }
                    let local_ctx = var_scopes.len();
                    var_scopes.push(VarScope::Arm {
                        body_expr: body_idx,
                        outer: frame.ctx,
                    });
                    let joins = frame.joins;
                    frame.state = TcState::CaseCheckArm {
                        arm_idx: 0,
                        data_id,
                        arm_exp,
                    };
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, joins, Some(arm_exp));
                } else {
                    // Infer arm 0.
                    let ctor = &c.data_infos[data_id].ctors[0];
                    if arms[0].ctor != ctor.name || arms[0].names.len() != ctor.fields.len() {
                        finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArm));
                        continue;
                    }
                    let body_idx = preps[e_idx].children[1];
                    if preps[body_idx].arm_bindings.is_none() {
                        let mut map = BTreeMap::new();
                        for (nm, &fty) in arms[0].names.iter().zip(&ctor.fields) {
                            map.entry(nm.as_str()).or_insert(fty);
                        }
                        preps[body_idx].arm_bindings = Some(map);
                    }
                    let local_ctx = var_scopes.len();
                    var_scopes.push(VarScope::Arm {
                        body_expr: body_idx,
                        outer: frame.ctx,
                    });
                    let joins = frame.joins;
                    frame.state = TcState::CaseInferArm {
                        arm_idx: 0,
                        data_id,
                    };
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, joins, None);
                }
            }

            TcState::CaseInferArm { arm_idx, data_id } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                let Expr::Case { arms, .. } = expr else {
                    finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArms));
                    continue;
                };
                if ans.is_ok()
                    && let Some(inferred) = ans.ty
                {
                    // Now check all arms 0..ctor_count against `inferred`.
                    let ctor = &c.data_infos[data_id].ctors[0];
                    if arms[0].ctor != ctor.name || arms[0].names.len() != ctor.fields.len() {
                        finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArm));
                        continue;
                    }
                    let body_idx = preps[e_idx].children[1];
                    let local_ctx = var_scopes.len();
                    var_scopes.push(VarScope::Arm {
                        body_expr: body_idx,
                        outer: frame.ctx,
                    });
                    let joins = frame.joins;
                    frame.state = TcState::CaseCheckArm {
                        arm_idx: 0,
                        data_id,
                        arm_exp: inferred,
                    };
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, joins, Some(inferred));
                    continue;
                }
                let next = arm_idx + 1;
                if next < c.data_infos[data_id].ctors.len() {
                    let ctor = &c.data_infos[data_id].ctors[next];
                    if arms[next].ctor != ctor.name || arms[next].names.len() != ctor.fields.len() {
                        finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArm));
                        continue;
                    }
                    let body_idx = preps[e_idx].children[next + 1];
                    if preps[body_idx].arm_bindings.is_none() {
                        let mut map = BTreeMap::new();
                        for (nm, &fty) in arms[next].names.iter().zip(&ctor.fields) {
                            map.entry(nm.as_str()).or_insert(fty);
                        }
                        preps[body_idx].arm_bindings = Some(map);
                    }
                    let local_ctx = var_scopes.len();
                    var_scopes.push(VarScope::Arm {
                        body_expr: body_idx,
                        outer: frame.ctx,
                    });
                    let joins = frame.joins;
                    frame.state = TcState::CaseInferArm {
                        arm_idx: next,
                        data_id,
                    };
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, joins, None);
                } else {
                    finish_frame!(frame, ans);
                }
            }

            TcState::CaseCheckArm {
                arm_idx,
                data_id,
                arm_exp,
            } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let next = arm_idx + 1;
                if next < c.data_infos[data_id].ctors.len() {
                    let Expr::Case { arms, .. } = expr else {
                        finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArms));
                        continue;
                    };
                    let ctor = &c.data_infos[data_id].ctors[next];
                    if arms[next].ctor != ctor.name || arms[next].names.len() != ctor.fields.len() {
                        finish_frame!(frame, Answer::bad(Some(e_idx), ErrorCode::CoreCheckArm));
                        continue;
                    }
                    let body_idx = preps[e_idx].children[next + 1];
                    if preps[body_idx].arm_bindings.is_none() {
                        let mut map = BTreeMap::new();
                        for (nm, &fty) in arms[next].names.iter().zip(&ctor.fields) {
                            map.entry(nm.as_str()).or_insert(fty);
                        }
                        preps[body_idx].arm_bindings = Some(map);
                    }
                    let local_ctx = var_scopes.len();
                    var_scopes.push(VarScope::Arm {
                        body_expr: body_idx,
                        outer: frame.ctx,
                    });
                    let joins = frame.joins;
                    frame.state = TcState::CaseCheckArm {
                        arm_idx: next,
                        data_id,
                        arm_exp,
                    };
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, joins, Some(arm_exp));
                } else {
                    finish_frame!(frame, expect_answer(e_idx, Some(arm_exp), Some(arm_exp)));
                }
            }

            TcState::IfCond => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let then_idx = preps[e_idx].children[1];
                let ctx = frame.ctx;
                let joins = frame.joins;
                if let Some(exp_ty) = frame.exp {
                    frame.state = TcState::IfExpThen;
                    stack.push(frame);
                    push_child!(then_idx, ctx, joins, Some(exp_ty));
                } else {
                    frame.state = TcState::IfInferThen;
                    stack.push(frame);
                    push_child!(then_idx, ctx, joins, None);
                }
            }

            TcState::IfExpThen => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let else_idx = preps[e_idx].children[2];
                let ctx = frame.ctx;
                let joins = frame.joins;
                let exp = frame.exp;
                frame.state = TcState::IfExpElse;
                stack.push(frame);
                push_child!(else_idx, ctx, joins, exp);
            }

            TcState::IfExpElse | TcState::IfInferElseThenOk | TcState::IfInferThenElseOk => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                finish_frame!(frame, ans);
            }

            TcState::IfInferThen => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                let else_idx = preps[e_idx].children[2];
                let ctx = frame.ctx;
                let joins = frame.joins;
                if ans.is_ok() {
                    frame.state = TcState::IfInferElseThenOk;
                    stack.push(frame);
                    push_child!(else_idx, ctx, joins, ans.ty);
                } else {
                    frame.state = TcState::IfInferElse;
                    stack.push(frame);
                    push_child!(else_idx, ctx, joins, None);
                }
            }

            TcState::IfInferElse => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let then_idx = preps[e_idx].children[1];
                let ctx = frame.ctx;
                let joins = frame.joins;
                frame.state = TcState::IfInferThenElseOk;
                stack.push(frame);
                push_child!(then_idx, ctx, joins, ans.ty);
            }

            TcState::PrimArg { idx } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let Some(ref sig) = preps[e_idx].signature else {
                    finish_frame!(
                        frame,
                        Answer::bad(Some(e_idx), ErrorCode::CoreCheckArguments)
                    );
                    continue;
                };
                let next = idx + 1;
                if next < preps[e_idx].children.len() {
                    let next_arg = preps[e_idx].children[next];
                    let next_ty = sig.arg_ty(next);
                    let ctx = frame.ctx;
                    frame.state = TcState::PrimArg { idx: next };
                    stack.push(frame);
                    push_child!(next_arg, ctx, None, Some(next_ty));
                } else {
                    let ret = sig.ret;
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, Some(ret)));
                }
            }

            TcState::FailArg => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                } else {
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, frame.exp));
                }
            }

            TcState::JoinExpBody { inner_joins } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let rest_idx = preps[e_idx].children[1];
                let ctx = frame.ctx;
                let exp = frame.exp;
                frame.state = TcState::JoinExpRest;
                stack.push(frame);
                push_child!(rest_idx, ctx, Some(inner_joins), exp);
            }

            TcState::JoinExpRest
            | TcState::JoinInferBodyFromRest
            | TcState::JoinInferRestFromBody => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                finish_frame!(frame, ans);
            }

            TcState::JoinInferRest {
                local_ctx,
                inner_joins,
            } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                let body_idx = preps[e_idx].children[0];
                if ans.is_ok() {
                    frame.state = TcState::JoinInferBodyFromRest;
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, Some(inner_joins), ans.ty);
                } else {
                    frame.state = TcState::JoinInferBodyFallback { inner_joins };
                    stack.push(frame);
                    push_child!(body_idx, local_ctx, Some(inner_joins), None);
                }
            }

            TcState::JoinInferBodyFallback { inner_joins } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let rest_idx = preps[e_idx].children[1];
                let ctx = frame.ctx;
                frame.state = TcState::JoinInferRestFromBody;
                stack.push(frame);
                push_child!(rest_idx, ctx, Some(inner_joins), ans.ty);
            }

            TcState::JumpArg { idx, join_expr } => {
                let ans = child_result
                    .unwrap_or_else(|| Answer::bad(Some(e_idx), ErrorCode::CoreCheckType));
                if !ans.is_ok() {
                    finish_frame!(frame, ans);
                    continue;
                }
                let next = idx + 1;
                if next < preps[join_expr].params.len() {
                    let next_arg = preps[e_idx].children[next];
                    let next_ty = preps[join_expr].params[next];
                    let ctx = frame.ctx;
                    frame.state = TcState::JumpArg {
                        idx: next,
                        join_expr,
                    };
                    stack.push(frame);
                    push_child!(next_arg, ctx, None, Some(next_ty));
                } else {
                    finish_frame!(frame, expect_answer(e_idx, frame.exp, frame.exp));
                }
            }
        }
    }

    let final_ans = child_result.unwrap_or(Answer::bad(None, ErrorCode::CoreCheckLimit));
    (final_ans, preps)
}

// ---------------------------------------------------------------------------
// Diagnostic formatting (`src/core/check.c:reason`, `label`, `report`)
// ---------------------------------------------------------------------------

const fn reason_text(code: ErrorCode) -> &'static str {
    match code {
        ErrorCode::CoreCheckMetadata => "Compiler metadata has an invalid shape.",
        ErrorCode::CoreCheckConstantName => "Two Core constants have the same lookup name.",
        ErrorCode::CoreCheckEntry => "The Core entry function does not exist.",
        ErrorCode::CoreCheckEntryKind => {
            "The Core entry names spec code rather than an ordinary function."
        }
        ErrorCode::CoreCheckKind => {
            "A Core spec function has the wrong parameter or result signature."
        }
        ErrorCode::CoreCheckPrefix => {
            "A Core function requires more constants than the program declares."
        }
        ErrorCode::CoreCheckLiteral => "A Core literal is outside its type's numeric range.",
        ErrorCode::CoreCheckVariable => "A Core variable has no lexical binding.",
        ErrorCode::CoreCheckGlobal => "A Core constant is absent or outside the available prefix.",
        ErrorCode::CoreCheckType => "The Core expression's type differs from its expected type.",
        ErrorCode::CoreCheckArguments => {
            "A Core application, call, jump, constructor or primitive has the wrong argument count."
        }
        ErrorCode::CoreCheckLambda => {
            "A Core lambda's parameter signature differs from the expected function type."
        }
        ErrorCode::CoreCheckApplication => {
            "Core applies an expression whose inferred type is not a function."
        }
        ErrorCode::CoreCheckFunction => "A Core call names a function that does not exist.",
        ErrorCode::CoreCheckCallPrefix => {
            "A Core call requires constants outside the caller's available prefix."
        }
        ErrorCode::CoreCheckSpecCall => "Ordinary Core code calls a spec function.",
        ErrorCode::CoreCheckProjection => {
            "Core projects from a non-tuple or beyond the tuple's parts."
        }
        ErrorCode::CoreCheckConstructors => {
            "Core constructs or cases on a non-choice type or an undeclared data type."
        }
        ErrorCode::CoreCheckConstructor => "The Core type has no constructor with this name.",
        ErrorCode::CoreCheckArms => "A Core case does not have exactly one arm per constructor.",
        ErrorCode::CoreCheckArm => {
            "A Core case arm has the wrong constructor order or field-binding count."
        }
        ErrorCode::CoreCheckSpecPrimitive => {
            "Ordinary Core code allocates a fake or observes console output."
        }
        ErrorCode::CoreCheckInference => {
            "Core cannot infer a type here; stop, fail, jump or an empty case needs an expected type."
        }
        ErrorCode::CoreCheckFail => "Ordinary Core code contains fail.",
        ErrorCode::CoreCheckJump => "A Core jump has no in-scope join point in this tail position.",
        ErrorCode::CoreCheckLimit => "Core checking exceeds its node or nesting limit.",
        ErrorCode::CoreCheckMemory => {
            "Core checking exhausts its bounded scratch arena or system memory."
        }
        _ => "invalid compiler Core",
    }
}

fn format_label_name(out: &mut String, name: &str) {
    let bytes = name.as_bytes();
    let mut n = bytes.len().min(128);
    while n < bytes.len() && n != 0 && (bytes[n] & 0xC0) == 0x80 {
        n -= 1;
    }
    out.push('"');
    let mut i = 0;
    while i < n {
        let ch = bytes[i];
        if ch < 32 || ch == 127 {
            const HEX: &[u8; 16] = b"0123456789abcdef";
            out.push_str("\\x");
            out.push(char::from(HEX[(ch >> 4) as usize]));
            out.push(char::from(HEX[(ch & 0xF) as usize]));
            i += 1;
        } else if ch == b'"' || ch == b'\\' {
            out.push('\\');
            out.push(char::from(ch));
            i += 1;
        } else if ch < 0x80 {
            out.push(char::from(ch));
            i += 1;
        } else {
            let s = &name[i..n];
            if let Some(c) = s.chars().next() {
                out.push(c);
                i += c.len_utf8();
            } else {
                i += 1;
            }
        }
    }
    if n < bytes.len() {
        out.push_str("...");
    }
    out.push('"');
}

fn format_label_type(c: &Checker<'_>, out: &mut String, root: InternTyId, remaining: &mut usize) {
    enum Piece {
        Ty(InternTyId),
        Str(&'static str),
        AfterPart { remaining_siblings: bool },
    }
    let mut stack = vec![Piece::Ty(root)];
    while let Some(piece) = stack.pop() {
        match piece {
            Piece::Str(s) => out.push_str(s),
            Piece::AfterPart { remaining_siblings } => {
                if *remaining == 0 {
                    if remaining_siblings {
                        out.push_str(", ...");
                    }
                    // Skip remaining sibling items up to closing ')'.
                    while let Some(next) = stack.pop() {
                        if matches!(next, Piece::Str(")")) {
                            out.push(')');
                            break;
                        }
                    }
                }
            }
            Piece::Ty(id) => {
                if *remaining == 0 {
                    out.push_str("...");
                    continue;
                }
                *remaining -= 1;
                let info = c.ty_info(id);
                out.push_str(info.tag.display_name());
                if info.tag == TyTag::Data {
                    out.push('(');
                    format_label_name(out, info.name);
                    out.push(')');
                } else if info.tag == TyTag::Cap {
                    out.push('(');
                    out.push_str(info.cap.display_name());
                    out.push(')');
                } else if !info.parts.is_empty() {
                    out.push('(');
                    stack.push(Piece::Str(")"));
                    let count = info.parts.len();
                    for (i, &part) in info.parts.iter().enumerate().rev() {
                        stack.push(Piece::AfterPart {
                            remaining_siblings: i + 1 < count,
                        });
                        stack.push(Piece::Ty(part));
                        if i != 0 {
                            stack.push(Piece::Str(", "));
                        }
                    }
                }
            }
        }
    }
}

fn format_label(c: &Checker<'_>, preps: &[PrepExpr<'_>], a: Answer) -> String {
    let mut out = String::from("in declaration ");
    format_label_name(&mut out, c.declaration);
    if let Some(at_idx) = a.at
        && let Some(prep) = preps.get(at_idx)
    {
        let expr = c.arena.expr(prep.source);
        let detail: Option<(&str, &str)> = match expr {
            Expr::Var(n) => Some((", variable ", n.as_str())),
            Expr::Global(n) => Some((", constant ", n.as_str())),
            Expr::Call { name, .. } => Some((", call ", name.as_str())),
            Expr::Con { ctor, .. } => Some((", constructor ", ctor.as_str())),
            Expr::Jump { name, .. } => Some((", jump ", name.as_str())),
            _ => None,
        };
        if let Some((what, name)) = detail {
            out.push_str(what);
            format_label_name(&mut out, name);
        }
        let mut cur = prep.parent;
        while let Some(p_idx) = cur {
            let parent = &preps[p_idx];
            if let Expr::Let { name, .. } = c.arena.expr(parent.source) {
                out.push_str(", in binding ");
                format_label_name(&mut out, name);
                break;
            }
            cur = parent.parent;
        }
    }
    if let (Some(exp), Some(found)) = (a.expected, a.ty) {
        out.push_str(": expected ");
        let mut rem1 = 16usize;
        format_label_type(c, &mut out, exp, &mut rem1);
        out.push_str(", found ");
        let mut rem2 = 16usize;
        format_label_type(c, &mut out, found, &mut rem2);
    }
    out
}

fn report(
    c: &Checker<'_>,
    preps: &[PrepExpr<'_>],
    diags: &mut DiagBag,
    code: ErrorCode,
    span: Option<Span>,
    a: Answer,
) -> bool {
    let resource = matches!(code, ErrorCode::CoreCheckLimit | ErrorCode::CoreCheckMemory);
    let primary_span = span.or(c.declaration_span).unwrap_or(Span::NONE);
    let label_text = format_label(c, preps, a);
    let diag = Diagnostic::new(
        code,
        primary_span,
        if resource {
            "Core checker resource limit exceeded"
        } else {
            "invalid compiler Core (bug in Mo)"
        },
    )
    .with_reason(reason_text(code))
    .with_primary_text(label_text);
    let _ = diags.push(diag);
    false
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/// Typechecks a Core program with optional compiler service descriptors,
/// recording at most one diagnostic in `diags` on failure.
///
/// Matches `src/core/check.c:mo_core_check_services`.
pub fn check_program_services(
    arena: &CoreArena,
    program: &Program,
    services: Option<&Services>,
    diags: &mut DiagBag,
) -> bool {
    let mut c = Checker::new(arena, program, services);
    if !metadata_valid(arena, program) {
        return report(
            &c,
            &[],
            diags,
            ErrorCode::CoreCheckMetadata,
            None,
            Answer::bad(None, ErrorCode::CoreCheckMetadata),
        );
    }

    let Some(fun_infos) = c.declarations() else {
        let code = c.fatal.unwrap_or(ErrorCode::CoreCheckLimit);
        return report(&c, &[], diags, code, None, Answer::bad(None, code));
    };

    let entry_idx = c.funs_map.get(program.entry.as_str()).copied();
    c.declaration = &program.entry;
    c.declaration_span = match (&program.metadata, entry_idx) {
        (Metadata::Source(m), Some(idx)) => m.funs[idx].span,
        _ => None,
    };
    let Some(e_idx) = entry_idx else {
        return report(
            &c,
            &[],
            diags,
            ErrorCode::CoreCheckEntry,
            None,
            Answer::bad(None, ErrorCode::CoreCheckEntry),
        );
    };
    if program.funs[e_idx].kind != FnKind::Function {
        return report(
            &c,
            &[],
            diags,
            ErrorCode::CoreCheckEntryKind,
            None,
            Answer::bad(None, ErrorCode::CoreCheckEntryKind),
        );
    }

    for (i, k) in program.consts.iter().enumerate() {
        c.declaration = &k.name;
        c.declaration_span = match &program.metadata {
            Metadata::Source(m) => m.consts[i].span,
            Metadata::NoSource => None,
        };
        c.scope = i;
        c.spec = false;
        let Some(const_info) = c.consts_map.get(k.name.as_str()).copied() else {
            return report(
                &c,
                &[],
                diags,
                ErrorCode::CoreCheckGlobal,
                None,
                Answer::bad(None, ErrorCode::CoreCheckGlobal),
            );
        };
        let (ans, preps) = check_body(&mut c, &fun_infos, k.body, &[], &[], const_info.ty);
        if let Some(code) = ans.code {
            let span = match &program.metadata {
                Metadata::Source(m) => match ans.at {
                    None => m.consts[i].span,
                    Some(at_idx) => m.consts[i].body.spans[preps[at_idx].node],
                },
                Metadata::NoSource => None,
            };
            return report(&c, &preps, diags, code, span, ans);
        }
    }

    for (i, f) in program.funs.iter().enumerate() {
        c.declaration = &f.name;
        c.declaration_span = match &program.metadata {
            Metadata::Source(m) => m.funs[i].span,
            Metadata::NoSource => None,
        };
        let f_info = &fun_infos[i];
        let ret_tag = c.ty_info(f_info.ret).tag;
        let kind_ok = match f.kind {
            FnKind::Function => true,
            FnKind::Test | FnKind::Example => f.params.is_empty() && ret_tag == TyTag::Unit,
            FnKind::Promise => !f.params.is_empty() && ret_tag == TyTag::Int,
            FnKind::Anchor => f.params.is_empty() && ret_tag == TyTag::Int,
        };
        if !kind_ok {
            let span = match &program.metadata {
                Metadata::Source(m) => m.funs[i].span,
                Metadata::NoSource => None,
            };
            return report(
                &c,
                &[],
                diags,
                ErrorCode::CoreCheckKind,
                span,
                Answer::bad(None, ErrorCode::CoreCheckKind),
            );
        }
        c.scope = f_info.scope;
        c.spec = f.kind != FnKind::Function;
        let (ans, preps) = check_body(
            &mut c,
            &fun_infos,
            f.body,
            &f.params,
            &f_info.params,
            f_info.ret,
        );
        if let Some(code) = ans.code {
            let span = match &program.metadata {
                Metadata::Source(m) => match ans.at {
                    None => m.funs[i].span,
                    Some(at_idx) => m.funs[i].body.spans[preps[at_idx].node],
                },
                Metadata::NoSource => None,
            };
            return report(&c, &preps, diags, code, span, ans);
        }
    }

    true
}

/// Typechecks a Core program without service descriptors (`src/core/check.c:mo_core_check`).
pub fn check_program(arena: &CoreArena, program: &Program, diags: &mut DiagBag) -> bool {
    check_program_services(arena, program, None, diags)
}
