//! Lossless `.core` v0.11 S-expression parser and canonical printer for all 16
//! Core document categories, matching `src/core/text.c`, `src/core/text_schema.def`,
//! and `formal/TEXT.md` byte-for-byte with explicit heap stacks (O(1) call stack
//! up to `MO_LIMIT_CORE_DEPTH = 4096`).

use mo_base::{DiagBag, Diagnostic, ErrorCode, Span};
use mo_def::MO_CORE_TEXT_VERSION;
use mo_def::core_ops::{
    CoreBase, CoreBigOp, CoreByteOp, CoreBytesOp, CoreCharOp, CoreCmpOp, CoreCryptoOp, CoreDecOp,
    CoreFloatOp, CoreIntOp, CoreIoNullaryOp, CoreListNullaryOp, CoreMapNullaryOp, CoreRangeOp,
    CoreSetOp, CoreTextOp, CoreTimeOp, CoreUnicodeOp,
};
use mo_def::limits::{
    MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES, MO_LIMIT_CORE_SCRATCH_SIZE, MO_LIMIT_CORE_TEXT_SIZE,
};
use thiserror::Error;

use crate::ir::{
    Arm, Binding, BindingMeta, BodyMeta, Bug, Cap, CapState, ClosureMeta, ConstDecl, ConstMeta,
    CoreArena, CoreDocument, CoreInteger, CoreNatural, CoreTextKind, CtorDecl, DataDecl, Effects,
    ExposedType, Expr, ExprId, FnDecl, FnKind, FnMeta, IoOp, ListOp, Lit, MapOp, Metadata,
    ModuleMeta, Outcome, Param, ParsedCore, Prim, Program, RandomState, Report, Run, Selection,
    SimOp, SourceMetadata, SpecLine, Ty, TyId, Value, ValueId, World,
};
use crate::metadata::metadata_valid;

/// Errors returned when canonical printing refuses a Core document.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum CorePrintError {
    #[error("canonical Core text exceeds node, nesting, or byte limit")]
    LimitExceeded,
    #[error("invalid compiler metadata shape")]
    InvalidMetadata,
    #[error("invalid Core integer or natural digits")]
    InvalidInteger,
    #[error("invalid Core arena reference")]
    InvalidArenaRef,
}

// ---------------------------------------------------------------------------
// S-expression pool & iterative lexer (`src/core/text.c:lex`)
// ---------------------------------------------------------------------------

#[derive(Debug)]
enum SexpKind {
    Node(Vec<u32>),
    Atom(usize, usize),
    Str(String),
}

#[derive(Debug)]
struct Sexp {
    start: u32,
    end: u32,
    kind: SexpKind,
}

struct ParseErr {
    code: ErrorCode,
    start: u32,
    end: u32,
    message: &'static str,
}

type PResult<T> = Result<T, ParseErr>;

fn err_at(code: ErrorCode, start: usize, end: usize, message: &'static str) -> ParseErr {
    ParseErr {
        code,
        start: start as u32,
        end: end as u32,
        message,
    }
}

struct SexpPool<'a> {
    input: &'a str,
    nodes: Vec<Sexp>,
}

impl<'a> SexpPool<'a> {
    fn get(&self, id: u32) -> &Sexp {
        &self.nodes[id as usize]
    }

    fn err_node(&self, id: u32, code: ErrorCode, message: &'static str) -> ParseErr {
        let sx = self.get(id);
        ParseErr {
            code,
            start: sx.start,
            end: sx.end,
            message,
        }
    }

    fn atom_str(&self, id: u32) -> Option<&'a str> {
        match self.get(id).kind {
            SexpKind::Atom(s, e) => Some(&self.input[s..e]),
            SexpKind::Node(_) | SexpKind::Str(_) => None,
        }
    }

    fn atom_is(&self, id: u32, expected: &str) -> bool {
        self.atom_str(id) == Some(expected)
    }

    fn expect_node(&self, id: u32, msg: &'static str) -> PResult<&[u32]> {
        match &self.get(id).kind {
            SexpKind::Node(children) => Ok(children),
            SexpKind::Atom(_, _) | SexpKind::Str(_) => {
                Err(self.err_node(id, ErrorCode::CoreField, msg))
            }
        }
    }

    fn expect_string(&self, id: u32) -> PResult<String> {
        match &self.get(id).kind {
            SexpKind::Str(s) => Ok(s.clone()),
            SexpKind::Node(_) | SexpKind::Atom(_, _) => {
                Err(self.err_node(id, ErrorCode::CoreField, "expected string"))
            }
        }
    }
}

const fn is_white(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r')
}

const fn is_boundary(c: u8) -> bool {
    is_white(c) || matches!(c, b'(' | b')' | b'"' | b';')
}

const fn hex_digit(c: u8) -> Option<u32> {
    match c {
        b'0'..=b'9' => Some((c - b'0') as u32),
        b'a'..=b'f' => Some((c - b'a') as u32 + 10),
        b'A'..=b'F' => Some((c - b'A') as u32 + 10),
        _ => None,
    }
}

fn skip_ws(bytes: &[u8], pos: &mut usize) {
    while *pos < bytes.len() {
        if is_white(bytes[*pos]) {
            *pos += 1;
        } else if bytes[*pos] == b';' {
            while *pos < bytes.len() && bytes[*pos] != b'\n' {
                *pos += 1;
            }
        } else {
            break;
        }
    }
}

fn read_string(input: &str, pos: &mut usize) -> PResult<String> {
    let bytes = input.as_bytes();
    let start = *pos;
    *pos += 1;
    let mut out = String::new();
    while *pos < bytes.len() {
        let b = bytes[*pos];
        if b == b'"' {
            *pos += 1;
            return Ok(out);
        }
        if b < 32 || b == 127 {
            let at = *pos;
            *pos += 1;
            return Err(err_at(
                ErrorCode::CoreEscape,
                at,
                at + 1,
                "raw control in string",
            ));
        }
        if b != b'\\' {
            // Valid UTF-8 was already verified on the full input; copy raw UTF-8 run directly.
            let run_start = *pos;
            while *pos < bytes.len() {
                let c = bytes[*pos];
                if c == b'"' || c == b'\\' || c < 32 || c == 127 {
                    break;
                }
                *pos += 1;
            }
            out.push_str(&input[run_start..*pos]);
            continue;
        }
        *pos += 1;
        if *pos == bytes.len() {
            return Err(err_at(
                ErrorCode::CoreEscape,
                start,
                *pos,
                "unterminated string escape",
            ));
        }
        let esc = bytes[*pos];
        *pos += 1;
        let ch = match esc {
            b'"' => '"',
            b'\\' => '\\',
            b'n' => '\n',
            b'r' => '\r',
            b't' => '\t',
            b'u' => {
                if *pos == bytes.len() || bytes[*pos] != b'{' {
                    if *pos < bytes.len() {
                        *pos += 1;
                    }
                    return Err(err_at(
                        ErrorCode::CoreEscape,
                        start,
                        *pos,
                        "expected Unicode escape braces",
                    ));
                }
                *pos += 1;
                let mut digits = 0usize;
                let mut decoded = 0u32;
                while *pos < bytes.len() && bytes[*pos] != b'}' {
                    let c = bytes[*pos];
                    *pos += 1;
                    let Some(d) = hex_digit(c) else {
                        return Err(err_at(
                            ErrorCode::CoreEscape,
                            start,
                            *pos,
                            "Unicode escape needs 1 to 6 hex digits",
                        ));
                    };
                    digits += 1;
                    if digits > 6 {
                        return Err(err_at(
                            ErrorCode::CoreEscape,
                            start,
                            *pos,
                            "Unicode escape needs 1 to 6 hex digits",
                        ));
                    }
                    decoded = decoded * 16 + d;
                }
                if digits == 0 || *pos == bytes.len() {
                    return Err(err_at(
                        ErrorCode::CoreEscape,
                        start,
                        *pos,
                        "incomplete Unicode escape",
                    ));
                }
                *pos += 1;
                let Some(scalar) = char::from_u32(decoded) else {
                    return Err(err_at(
                        ErrorCode::CoreEscape,
                        start,
                        *pos,
                        "not a Unicode scalar",
                    ));
                };
                scalar
            }
            _ => {
                return Err(err_at(
                    ErrorCode::CoreEscape,
                    *pos - 2,
                    *pos,
                    "unknown string escape",
                ));
            }
        };
        out.push(ch);
    }
    Err(err_at(
        ErrorCode::CoreSyntax,
        start,
        *pos,
        "unterminated string or exhausted string buffer",
    ))
}

fn lex<'a>(input: &'a str) -> PResult<(SexpPool<'a>, u32)> {
    let bytes = input.as_bytes();
    let max_nodes = MO_LIMIT_CORE_NODES as usize;
    let max_depth = MO_LIMIT_CORE_DEPTH as usize;
    let mut nodes: Vec<Sexp> = Vec::new();
    let mut stack: Vec<u32> = Vec::new();
    let mut root: Option<u32> = None;
    let mut pos = 0usize;

    loop {
        skip_ws(bytes, &mut pos);
        if pos == bytes.len() {
            break;
        }
        if bytes[pos] == b')' {
            let Some(parent_id) = stack.pop() else {
                return Err(err_at(
                    ErrorCode::CoreSyntax,
                    pos,
                    pos + 1,
                    "unexpected closing parenthesis",
                ));
            };
            pos += 1;
            nodes[parent_id as usize].end = pos as u32;
            continue;
        }
        if root.is_some() && stack.is_empty() {
            return Err(err_at(
                ErrorCode::CoreSyntax,
                pos,
                pos + 1,
                "trailing input",
            ));
        }
        if nodes.len() == max_nodes {
            return Err(err_at(
                ErrorCode::CoreTextLimit,
                pos,
                pos,
                "too many Core text nodes",
            ));
        }
        let id = nodes.len() as u32;
        let start = pos as u32;
        let b = bytes[pos];
        if b == b'(' {
            if stack.len() == max_depth {
                return Err(err_at(
                    ErrorCode::NestedTooDeeply,
                    pos,
                    pos + 1,
                    "Core text nests too deeply",
                ));
            }
            nodes.push(Sexp {
                start,
                end: start,
                kind: SexpKind::Node(Vec::new()),
            });
            if let Some(&parent) = stack.last()
                && let SexpKind::Node(ref mut children) = nodes[parent as usize].kind
            {
                children.push(id);
            } else {
                root = Some(id);
            }
            stack.push(id);
            pos += 1;
        } else if b == b'"' {
            let text = read_string(input, &mut pos)?;
            nodes.push(Sexp {
                start,
                end: pos as u32,
                kind: SexpKind::Str(text),
            });
            if let Some(&parent) = stack.last()
                && let SexpKind::Node(ref mut children) = nodes[parent as usize].kind
            {
                children.push(id);
            } else {
                root = Some(id);
            }
        } else {
            let atom_start = pos;
            while pos < bytes.len() && !is_boundary(bytes[pos]) {
                pos += 1;
            }
            nodes.push(Sexp {
                start,
                end: pos as u32,
                kind: SexpKind::Atom(atom_start, pos),
            });
            if let Some(&parent) = stack.last()
                && let SexpKind::Node(ref mut children) = nodes[parent as usize].kind
            {
                children.push(id);
            } else {
                root = Some(id);
            }
        }
    }

    if !stack.is_empty() {
        return Err(err_at(
            ErrorCode::CoreSyntax,
            pos,
            pos,
            "expected complete S-expression",
        ));
    }
    let Some(root_id) = root else {
        return Err(err_at(
            ErrorCode::CoreSyntax,
            pos,
            pos,
            "expected complete S-expression",
        ));
    };
    Ok((SexpPool { input, nodes }, root_id))
}

// ---------------------------------------------------------------------------
// Decoder helpers
// ---------------------------------------------------------------------------

struct Decoder<'a> {
    pool: &'a SexpPool<'a>,
    arena: CoreArena,
}

impl<'a> Decoder<'a> {
    fn new(pool: &'a SexpPool<'a>) -> Self {
        Self {
            pool,
            arena: CoreArena::new(),
        }
    }

    fn check_depth(&self, id: u32, depth: usize) -> PResult<()> {
        if depth > MO_LIMIT_CORE_DEPTH as usize {
            return Err(self.pool.err_node(
                id,
                ErrorCode::NestedTooDeeply,
                "Core text nests too deeply",
            ));
        }
        Ok(())
    }

    fn open_tagged(&self, id: u32, depth: usize) -> PResult<(&'a str, &'a [u32])> {
        self.check_depth(id, depth)?;
        let children = self.pool.expect_node(id, "expected constructor list")?;
        let Some(&head) = children.first() else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreConstructor,
                "unknown Core constructor",
            ));
        };
        let Some(tag) = self.pool.atom_str(head) else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreConstructor,
                "unknown Core constructor",
            ));
        };
        Ok((tag, &children[1..]))
    }

    fn open_record(
        &self,
        id: u32,
        expected_tag: &str,
        arity: usize,
        depth: usize,
    ) -> PResult<&'a [u32]> {
        let (tag, args) = self.open_tagged(id, depth)?;
        if tag != expected_tag {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreConstructor,
                "unknown Core constructor",
            ));
        }
        if args.len() != arity {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreArity,
                "wrong Core constructor arity",
            ));
        }
        Ok(args)
    }

    fn open_untagged(&self, id: u32, arity: usize, depth: usize) -> PResult<&'a [u32]> {
        self.check_depth(id, depth)?;
        let children = self.pool.expect_node(id, "expected constructor list")?;
        if children.len() != arity {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreArity,
                "wrong Core constructor arity",
            ));
        }
        Ok(children)
    }

    fn open_list(&self, id: u32, depth: usize) -> PResult<&'a [u32]> {
        self.check_depth(id, depth)?;
        self.pool.expect_node(id, "expected list wrapper")
    }

    fn expect_arity(&self, id: u32, args: &[u32], expected: usize) -> PResult<()> {
        if args.len() != expected {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreArity,
                "wrong Core constructor arity",
            ));
        }
        Ok(())
    }

    fn decode_integer(&self, id: u32) -> PResult<CoreInteger> {
        let Some(raw) = self.pool.atom_str(id) else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreNumber,
                "expected decimal whole number",
            ));
        };
        CoreInteger::parse_decimal(raw).ok_or_else(|| {
            self.pool
                .err_node(id, ErrorCode::CoreNumber, "expected decimal digits")
        })
    }

    fn decode_natural(&self, id: u32) -> PResult<CoreNatural> {
        let Some(raw) = self.pool.atom_str(id) else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreNumber,
                "expected decimal whole number",
            ));
        };
        let int = CoreInteger::parse_decimal(raw).ok_or_else(|| {
            self.pool
                .err_node(id, ErrorCode::CoreNumber, "expected decimal digits")
        })?;
        if int.negative {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreNumber,
                "expected nonnegative whole number",
            ));
        }
        Ok(CoreNatural { digits: int.digits })
    }

    fn decode_u32(&self, id: u32) -> PResult<u32> {
        let nat = self.decode_natural(id)?;
        nat.digits.parse::<u32>().map_err(|_| {
            self.pool
                .err_node(id, ErrorCode::CoreNumber, "metadata index exceeds u32")
        })
    }

    fn decode_bits(&self, id: u32) -> PResult<u64> {
        let Some(raw) = self.pool.atom_str(id) else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreNumber,
                "expected 0x and 16 hex digits",
            ));
        };
        let bytes = raw.as_bytes();
        if bytes.len() != 18 || &bytes[..2] != b"0x" {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreNumber,
                "expected 0x and 16 hex digits",
            ));
        }
        let mut bits = 0u64;
        for &b in &bytes[2..] {
            let Some(d) = hex_digit(b) else {
                return Err(self.pool.err_node(
                    id,
                    ErrorCode::CoreNumber,
                    "invalid Float hexadecimal digit",
                ));
            };
            bits = (bits << 4) | u64::from(d);
        }
        Ok(bits)
    }

    fn decode_bool(&self, id: u32) -> PResult<bool> {
        if self.pool.atom_is(id, "true") {
            Ok(true)
        } else if self.pool.atom_is(id, "false") {
            Ok(false)
        } else {
            Err(self
                .pool
                .err_node(id, ErrorCode::CoreField, "expected true or false"))
        }
    }

    fn decode_char(&self, id: u32) -> PResult<char> {
        let SexpKind::Str(ref s) = self.pool.get(id).kind else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreCharacter,
                "expected exactly one Unicode scalar",
            ));
        };
        let mut chars = s.chars();
        let Some(ch) = chars.next() else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreCharacter,
                "expected exactly one Unicode scalar",
            ));
        };
        if chars.next().is_some() {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreCharacter,
                "expected exactly one Unicode scalar",
            ));
        }
        Ok(ch)
    }

    fn decode_string_list(&self, id: u32, depth: usize) -> PResult<Vec<String>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            out.push(self.pool.expect_string(child)?);
        }
        Ok(out)
    }

    fn decode_u32_list(&self, id: u32, depth: usize) -> PResult<Vec<u32>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            out.push(self.decode_u32(child)?);
        }
        Ok(out)
    }

    fn decode_integer_list(&self, id: u32, depth: usize) -> PResult<Vec<CoreInteger>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            out.push(self.decode_integer(child)?);
        }
        Ok(out)
    }

    fn decode_string_pairs(&self, id: u32, depth: usize) -> PResult<Vec<(String, String)>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            let pair = self.open_untagged(child, 2, depth + 1)?;
            out.push((
                self.pool.expect_string(pair[0])?,
                self.pool.expect_string(pair[1])?,
            ));
        }
        Ok(out)
    }

    fn decode_cap(&self, id: u32, depth: usize) -> PResult<Cap> {
        let (tag, args) = self.open_tagged(id, depth)?;
        let Some(cap) = Cap::from_wire(tag) else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreConstructor,
                "unknown Core constructor",
            ));
        };
        self.expect_arity(id, args, 0)?;
        Ok(cap)
    }

    /// Iterative type decoder (O(1) Rust call stack up to `MO_LIMIT_CORE_DEPTH`).
    fn decode_ty(&mut self, root_id: u32, root_depth: usize) -> PResult<TyId> {
        enum TyTask {
            Enter { id: u32, depth: usize },
            BuildList,
            BuildSet,
            BuildOption,
            BuildMap,
            BuildResult,
            BuildTuple { count: usize },
            BuildFn { param_count: usize },
        }

        let mut tasks: Vec<TyTask> = vec![TyTask::Enter {
            id: root_id,
            depth: root_depth,
        }];
        let mut results: Vec<TyId> = Vec::new();

        while let Some(task) = tasks.pop() {
            match task {
                TyTask::Enter { id, depth } => {
                    let (tag, args) = self.open_tagged(id, depth)?;
                    let nullary = match tag {
                        "int" => Some(Ty::Int),
                        "bigInt" => Some(Ty::BigInt),
                        "byte" => Some(Ty::Byte),
                        "decimal" => Some(Ty::Decimal),
                        "float" => Some(Ty::Float),
                        "bool" => Some(Ty::Bool),
                        "char" => Some(Ty::Char),
                        "text" => Some(Ty::Text),
                        "bytes" => Some(Ty::Bytes),
                        "unit" => Some(Ty::Unit),
                        "range" => Some(Ty::Range),
                        "time" => Some(Ty::Time),
                        "duration" => Some(Ty::Duration),
                        "ordering" => Some(Ty::Ordering),
                        "fileError" => Some(Ty::FileError),
                        "entry" => Some(Ty::Entry),
                        "entryKind" => Some(Ty::EntryKind),
                        "simulation" => Some(Ty::Simulation),
                        "simulationError" => Some(Ty::SimulationError),
                        "networkError" => Some(Ty::NetworkError),
                        "sqliteValue" => Some(Ty::SqliteValue),
                        "sqliteRows" => Some(Ty::SqliteRows),
                        "sqliteError" => Some(Ty::SqliteError),
                        _ => None,
                    };
                    if let Some(ty) = nullary {
                        self.expect_arity(id, args, 0)?;
                        results.push(self.arena.alloc_ty(ty));
                        continue;
                    }
                    match tag {
                        "data" => {
                            self.expect_arity(id, args, 1)?;
                            let name = self.pool.expect_string(args[0])?;
                            results.push(self.arena.alloc_ty(Ty::Data(name)));
                        }
                        "cap" => {
                            self.expect_arity(id, args, 1)?;
                            let cap = self.decode_cap(args[0], depth + 1)?;
                            results.push(self.arena.alloc_ty(Ty::Cap(cap)));
                        }
                        "list" => {
                            self.expect_arity(id, args, 1)?;
                            tasks.push(TyTask::BuildList);
                            tasks.push(TyTask::Enter {
                                id: args[0],
                                depth: depth + 1,
                            });
                        }
                        "set" => {
                            self.expect_arity(id, args, 1)?;
                            tasks.push(TyTask::BuildSet);
                            tasks.push(TyTask::Enter {
                                id: args[0],
                                depth: depth + 1,
                            });
                        }
                        "option" => {
                            self.expect_arity(id, args, 1)?;
                            tasks.push(TyTask::BuildOption);
                            tasks.push(TyTask::Enter {
                                id: args[0],
                                depth: depth + 1,
                            });
                        }
                        "map" => {
                            self.expect_arity(id, args, 2)?;
                            tasks.push(TyTask::BuildMap);
                            tasks.push(TyTask::Enter {
                                id: args[1],
                                depth: depth + 1,
                            });
                            tasks.push(TyTask::Enter {
                                id: args[0],
                                depth: depth + 1,
                            });
                        }
                        "result" => {
                            self.expect_arity(id, args, 2)?;
                            tasks.push(TyTask::BuildResult);
                            tasks.push(TyTask::Enter {
                                id: args[1],
                                depth: depth + 1,
                            });
                            tasks.push(TyTask::Enter {
                                id: args[0],
                                depth: depth + 1,
                            });
                        }
                        "tuple" => {
                            self.expect_arity(id, args, 1)?;
                            let items = self.open_list(args[0], depth + 1)?;
                            tasks.push(TyTask::BuildTuple { count: items.len() });
                            for &elem in items.iter().rev() {
                                tasks.push(TyTask::Enter {
                                    id: elem,
                                    depth: depth + 2,
                                });
                            }
                        }
                        "fn" => {
                            self.expect_arity(id, args, 2)?;
                            let params = self.open_list(args[0], depth + 1)?;
                            tasks.push(TyTask::BuildFn {
                                param_count: params.len(),
                            });
                            tasks.push(TyTask::Enter {
                                id: args[1],
                                depth: depth + 1,
                            });
                            for &p in params.iter().rev() {
                                tasks.push(TyTask::Enter {
                                    id: p,
                                    depth: depth + 2,
                                });
                            }
                        }
                        _ => {
                            return Err(self.pool.err_node(
                                id,
                                ErrorCode::CoreConstructor,
                                "unknown Core constructor",
                            ));
                        }
                    }
                }
                TyTask::BuildList => {
                    let elem = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_ty(Ty::List(elem)));
                }
                TyTask::BuildSet => {
                    let elem = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_ty(Ty::Set(elem)));
                }
                TyTask::BuildOption => {
                    let elem = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_ty(Ty::Option(elem)));
                }
                TyTask::BuildMap => {
                    let value = pop_item(&mut results, root_id)?;
                    let key = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_ty(Ty::Map { key, value }));
                }
                TyTask::BuildResult => {
                    let err = pop_item(&mut results, root_id)?;
                    let ok = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_ty(Ty::Result { ok, err }));
                }
                TyTask::BuildTuple { count } => {
                    let start = results.len().saturating_sub(count);
                    let elems = results.split_off(start);
                    results.push(self.arena.alloc_ty(Ty::Tuple(elems)));
                }
                TyTask::BuildFn { param_count } => {
                    let ret = pop_item(&mut results, root_id)?;
                    let start = results.len().saturating_sub(param_count);
                    let params = results.split_off(start);
                    results.push(self.arena.alloc_ty(Ty::Fn { params, ret }));
                }
            }
        }
        pop_item(&mut results, root_id)
    }

    fn decode_ty_list(&mut self, id: u32, depth: usize) -> PResult<Vec<TyId>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            out.push(self.decode_ty(child, depth + 1)?);
        }
        Ok(out)
    }

    fn decode_params(&mut self, id: u32, depth: usize) -> PResult<Vec<Param>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            let fields = self.open_untagged(child, 2, depth + 1)?;
            let name = self.pool.expect_string(fields[0])?;
            let ty = self.decode_ty(fields[1], depth + 2)?;
            out.push(Param { name, ty });
        }
        Ok(out)
    }

    fn decode_lit(&self, id: u32, depth: usize) -> PResult<Lit> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "int" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Int(self.decode_integer(args[0])?))
            }
            "bigInt" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::BigInt(self.decode_integer(args[0])?))
            }
            "byte" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Byte(self.decode_integer(args[0])?))
            }
            "decimal" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Decimal(self.decode_integer(args[0])?))
            }
            "float" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Float(self.decode_bits(args[0])?))
            }
            "bool" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Bool(self.decode_bool(args[0])?))
            }
            "char" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Char(self.decode_char(args[0])?))
            }
            "text" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Text(self.pool.expect_string(args[0])?))
            }
            "bytes" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Bytes(self.decode_integer_list(args[0], depth + 1)?))
            }
            "unit" => {
                self.expect_arity(id, args, 0)?;
                Ok(Lit::Unit)
            }
            "time" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Time(self.decode_integer(args[0])?))
            }
            "duration" => {
                self.expect_arity(id, args, 1)?;
                Ok(Lit::Duration(self.decode_integer(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_nullary_op<T>(
        &self,
        id: u32,
        depth: usize,
        lookup: fn(&str) -> Option<T>,
    ) -> PResult<T> {
        let (tag, args) = self.open_tagged(id, depth)?;
        let Some(op) = lookup(tag) else {
            return Err(self.pool.err_node(
                id,
                ErrorCode::CoreConstructor,
                "unknown Core constructor",
            ));
        };
        self.expect_arity(id, args, 0)?;
        Ok(op)
    }

    fn decode_prim(&mut self, id: u32, depth: usize) -> PResult<Prim> {
        let (tag, args) = self.open_tagged(id, depth)?;
        let child_depth = depth + 1;
        match tag {
            "int" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Int(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreIntOp::from_wire_name,
                )?))
            }
            "big" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Big(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreBigOp::from_wire_name,
                )?))
            }
            "byte" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Byte(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreByteOp::from_wire_name,
                )?))
            }
            "dec" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Dec(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreDecOp::from_wire_name,
                )?))
            }
            "float" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Float(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreFloatOp::from_wire_name,
                )?))
            }
            "not" => {
                self.expect_arity(id, args, 0)?;
                Ok(Prim::Not)
            }
            "cmp" => {
                self.expect_arity(id, args, 2)?;
                let base =
                    self.decode_nullary_op(args[0], child_depth, CoreBase::from_wire_name)?;
                let op = self.decode_nullary_op(args[1], child_depth, CoreCmpOp::from_wire_name)?;
                Ok(Prim::Cmp { base, op })
            }
            "char" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Char(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreCharOp::from_wire_name,
                )?))
            }
            "text" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Text(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreTextOp::from_wire_name,
                )?))
            }
            "bytes" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Bytes(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreBytesOp::from_wire_name,
                )?))
            }
            "range" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Range(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreRangeOp::from_wire_name,
                )?))
            }
            "unicode16" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Unicode16(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreUnicodeOp::from_wire_name,
                )?))
            }
            "crypto" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Crypto(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreCryptoOp::from_wire_name,
                )?))
            }
            "sim" => {
                self.expect_arity(id, args, 1)?;
                let (stag, sargs) = self.open_tagged(args[0], child_depth)?;
                let op = match stag {
                    "start" => {
                        self.expect_arity(args[0], sargs, 1)?;
                        SimOp::Start {
                            descriptor: self.decode_natural(sargs[0])?,
                        }
                    }
                    "send" => {
                        self.expect_arity(args[0], sargs, 4)?;
                        let descriptor = self.decode_natural(sargs[0])?;
                        let tool = self.decode_natural(sargs[1])?;
                        let arguments = self.decode_ty_list(sargs[2], child_depth + 1)?;
                        let result = self.decode_ty(sargs[3], child_depth + 1)?;
                        SimOp::Send {
                            descriptor,
                            tool,
                            arguments,
                            result,
                        }
                    }
                    "advance" => {
                        self.expect_arity(args[0], sargs, 0)?;
                        SimOp::Advance
                    }
                    "crash" => {
                        self.expect_arity(args[0], sargs, 0)?;
                        SimOp::Crash
                    }
                    "restart" => {
                        self.expect_arity(args[0], sargs, 0)?;
                        SimOp::Restart
                    }
                    _ => {
                        return Err(self.pool.err_node(
                            args[0],
                            ErrorCode::CoreConstructor,
                            "unknown Core constructor",
                        ));
                    }
                };
                Ok(Prim::Sim(op))
            }
            "time" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Time(self.decode_nullary_op(
                    args[0],
                    child_depth,
                    CoreTimeOp::from_wire_name,
                )?))
            }
            "list" => {
                self.expect_arity(id, args, 2)?;
                let ty = self.decode_ty(args[0], child_depth)?;
                let (ltag, largs) = self.open_tagged(args[1], child_depth)?;
                let op = if ltag == "lit" {
                    self.expect_arity(args[1], largs, 1)?;
                    ListOp::Lit(self.decode_natural(largs[0])?)
                } else if ltag == "zip" {
                    self.expect_arity(args[1], largs, 1)?;
                    ListOp::Zip(self.decode_ty(largs[0], child_depth + 1)?)
                } else if let Some(n) = CoreListNullaryOp::from_wire_name(ltag) {
                    self.expect_arity(args[1], largs, 0)?;
                    ListOp::Nullary(n)
                } else {
                    return Err(self.pool.err_node(
                        args[1],
                        ErrorCode::CoreConstructor,
                        "unknown Core constructor",
                    ));
                };
                Ok(Prim::List { ty, op })
            }
            "map" => {
                self.expect_arity(id, args, 3)?;
                let key = self.decode_ty(args[0], child_depth)?;
                let value = self.decode_ty(args[1], child_depth)?;
                let (mtag, margs) = self.open_tagged(args[2], child_depth)?;
                let op = if mtag == "lit" {
                    self.expect_arity(args[2], margs, 1)?;
                    MapOp::Lit(self.decode_natural(margs[0])?)
                } else if let Some(n) = CoreMapNullaryOp::from_wire_name(mtag) {
                    self.expect_arity(args[2], margs, 0)?;
                    MapOp::Nullary(n)
                } else {
                    return Err(self.pool.err_node(
                        args[2],
                        ErrorCode::CoreConstructor,
                        "unknown Core constructor",
                    ));
                };
                Ok(Prim::Map { key, value, op })
            }
            "set" => {
                self.expect_arity(id, args, 2)?;
                let ty = self.decode_ty(args[0], child_depth)?;
                let op = self.decode_nullary_op(args[1], child_depth, CoreSetOp::from_wire_name)?;
                Ok(Prim::Set { ty, op })
            }
            "eq" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Eq(self.decode_ty(args[0], child_depth)?))
            }
            "structural" => {
                self.expect_arity(id, args, 1)?;
                Ok(Prim::Structural(self.decode_ty(args[0], child_depth)?))
            }
            "io" => {
                self.expect_arity(id, args, 1)?;
                let (itag, iargs) = self.open_tagged(args[0], child_depth)?;
                let op = match itag {
                    "readText" => {
                        self.expect_arity(args[0], iargs, 1)?;
                        IoOp::ReadText {
                            ro: self.decode_bool(iargs[0])?,
                        }
                    }
                    "readBytes" => {
                        self.expect_arity(args[0], iargs, 1)?;
                        IoOp::ReadBytes {
                            ro: self.decode_bool(iargs[0])?,
                        }
                    }
                    "list" => {
                        self.expect_arity(args[0], iargs, 1)?;
                        IoOp::List {
                            ro: self.decode_bool(iargs[0])?,
                        }
                    }
                    "entries" => {
                        self.expect_arity(args[0], iargs, 1)?;
                        IoOp::Entries {
                            ro: self.decode_bool(iargs[0])?,
                        }
                    }
                    "exists" => {
                        self.expect_arity(args[0], iargs, 1)?;
                        IoOp::Exists {
                            ro: self.decode_bool(iargs[0])?,
                        }
                    }
                    "within" => {
                        self.expect_arity(args[0], iargs, 1)?;
                        IoOp::Within {
                            ro: self.decode_bool(iargs[0])?,
                        }
                    }
                    other => {
                        let Some(n) = CoreIoNullaryOp::from_wire_name(other) else {
                            return Err(self.pool.err_node(
                                args[0],
                                ErrorCode::CoreConstructor,
                                "unknown Core constructor",
                            ));
                        };
                        self.expect_arity(args[0], iargs, 0)?;
                        IoOp::Nullary(n)
                    }
                };
                Ok(Prim::Io(op))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    /// Iterative expression decoder (O(1) Rust call stack up to `MO_LIMIT_CORE_DEPTH`).
    fn decode_expr(&mut self, root_id: u32, root_depth: usize) -> PResult<ExprId> {
        enum ExprTask {
            Enter { id: u32, depth: usize },
            BuildLet { name: String },
            BuildLam { params: Vec<Param> },
            BuildApp { argc: usize },
            BuildCall { name: String, argc: usize },
            BuildTuple { count: usize },
            BuildProj { index: CoreNatural },
            BuildCon { ty: TyId, ctor: String, argc: usize },
            BuildCase { headers: Vec<(String, Vec<String>)> },
            BuildIf,
            BuildPrim { prim: Prim, argc: usize },
            BuildFail,
            BuildJoin { name: String, params: Vec<Param> },
            BuildJump { name: String, argc: usize },
        }

        let mut tasks = vec![ExprTask::Enter {
            id: root_id,
            depth: root_depth,
        }];
        let mut results: Vec<ExprId> = Vec::new();

        while let Some(task) = tasks.pop() {
            match task {
                ExprTask::Enter { id, depth } => {
                    let (tag, args) = self.open_tagged(id, depth)?;
                    let cd = depth + 1;
                    match tag {
                        "lit" => {
                            self.expect_arity(id, args, 1)?;
                            let lit = self.decode_lit(args[0], cd)?;
                            results.push(self.arena.alloc_expr(Expr::Lit(lit)));
                        }
                        "var" => {
                            self.expect_arity(id, args, 1)?;
                            let name = self.pool.expect_string(args[0])?;
                            results.push(self.arena.alloc_expr(Expr::Var(name)));
                        }
                        "global" => {
                            self.expect_arity(id, args, 1)?;
                            let name = self.pool.expect_string(args[0])?;
                            results.push(self.arena.alloc_expr(Expr::Global(name)));
                        }
                        "let_" => {
                            self.expect_arity(id, args, 3)?;
                            let name = self.pool.expect_string(args[0])?;
                            tasks.push(ExprTask::BuildLet { name });
                            tasks.push(ExprTask::Enter {
                                id: args[2],
                                depth: cd,
                            });
                            tasks.push(ExprTask::Enter {
                                id: args[1],
                                depth: cd,
                            });
                        }
                        "lam" => {
                            self.expect_arity(id, args, 2)?;
                            let params = self.decode_params(args[0], cd)?;
                            tasks.push(ExprTask::BuildLam { params });
                            tasks.push(ExprTask::Enter {
                                id: args[1],
                                depth: cd,
                            });
                        }
                        "app" => {
                            self.expect_arity(id, args, 2)?;
                            let call_args = self.open_list(args[1], cd)?;
                            tasks.push(ExprTask::BuildApp {
                                argc: call_args.len(),
                            });
                            for &a in call_args.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: a,
                                    depth: cd + 1,
                                });
                            }
                            tasks.push(ExprTask::Enter {
                                id: args[0],
                                depth: cd,
                            });
                        }
                        "call" => {
                            self.expect_arity(id, args, 2)?;
                            let name = self.pool.expect_string(args[0])?;
                            let call_args = self.open_list(args[1], cd)?;
                            tasks.push(ExprTask::BuildCall {
                                name,
                                argc: call_args.len(),
                            });
                            for &a in call_args.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: a,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "tuple" => {
                            self.expect_arity(id, args, 1)?;
                            let elems = self.open_list(args[0], cd)?;
                            tasks.push(ExprTask::BuildTuple { count: elems.len() });
                            for &e in elems.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: e,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "proj" => {
                            self.expect_arity(id, args, 2)?;
                            let index = self.decode_natural(args[1])?;
                            tasks.push(ExprTask::BuildProj { index });
                            tasks.push(ExprTask::Enter {
                                id: args[0],
                                depth: cd,
                            });
                        }
                        "con" => {
                            self.expect_arity(id, args, 3)?;
                            let ty = self.decode_ty(args[0], cd)?;
                            let ctor = self.pool.expect_string(args[1])?;
                            let con_args = self.open_list(args[2], cd)?;
                            tasks.push(ExprTask::BuildCon {
                                ty,
                                ctor,
                                argc: con_args.len(),
                            });
                            for &a in con_args.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: a,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "case_" => {
                            self.expect_arity(id, args, 2)?;
                            let arm_nodes = self.open_list(args[1], cd)?;
                            let mut headers = Vec::with_capacity(arm_nodes.len());
                            let mut arm_bodies = Vec::with_capacity(arm_nodes.len());
                            for &arm_node in arm_nodes {
                                let fields = self.open_untagged(arm_node, 3, cd + 1)?;
                                let ctor = self.pool.expect_string(fields[0])?;
                                let names = self.decode_string_list(fields[1], cd + 2)?;
                                headers.push((ctor, names));
                                arm_bodies.push(fields[2]);
                            }
                            tasks.push(ExprTask::BuildCase { headers });
                            for &body_id in arm_bodies.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: body_id,
                                    depth: cd + 2,
                                });
                            }
                            tasks.push(ExprTask::Enter {
                                id: args[0],
                                depth: cd,
                            });
                        }
                        "if_" => {
                            self.expect_arity(id, args, 3)?;
                            tasks.push(ExprTask::BuildIf);
                            tasks.push(ExprTask::Enter {
                                id: args[2],
                                depth: cd,
                            });
                            tasks.push(ExprTask::Enter {
                                id: args[1],
                                depth: cd,
                            });
                            tasks.push(ExprTask::Enter {
                                id: args[0],
                                depth: cd,
                            });
                        }
                        "prim" => {
                            self.expect_arity(id, args, 2)?;
                            let prim = self.decode_prim(args[0], cd)?;
                            let prim_args = self.open_list(args[1], cd)?;
                            tasks.push(ExprTask::BuildPrim {
                                prim,
                                argc: prim_args.len(),
                            });
                            for &a in prim_args.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: a,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "stop" => {
                            self.expect_arity(id, args, 1)?;
                            let msg = self.pool.expect_string(args[0])?;
                            results.push(self.arena.alloc_expr(Expr::Stop(msg)));
                        }
                        "fail" => {
                            self.expect_arity(id, args, 1)?;
                            tasks.push(ExprTask::BuildFail);
                            tasks.push(ExprTask::Enter {
                                id: args[0],
                                depth: cd,
                            });
                        }
                        "join" => {
                            self.expect_arity(id, args, 4)?;
                            let name = self.pool.expect_string(args[0])?;
                            let params = self.decode_params(args[1], cd)?;
                            tasks.push(ExprTask::BuildJoin { name, params });
                            tasks.push(ExprTask::Enter {
                                id: args[3],
                                depth: cd,
                            });
                            tasks.push(ExprTask::Enter {
                                id: args[2],
                                depth: cd,
                            });
                        }
                        "jump" => {
                            self.expect_arity(id, args, 2)?;
                            let name = self.pool.expect_string(args[0])?;
                            let jump_args = self.open_list(args[1], cd)?;
                            tasks.push(ExprTask::BuildJump {
                                name,
                                argc: jump_args.len(),
                            });
                            for &a in jump_args.iter().rev() {
                                tasks.push(ExprTask::Enter {
                                    id: a,
                                    depth: cd + 1,
                                });
                            }
                        }
                        _ => {
                            return Err(self.pool.err_node(
                                id,
                                ErrorCode::CoreConstructor,
                                "unknown Core constructor",
                            ));
                        }
                    }
                }
                ExprTask::BuildLet { name } => {
                    let body = pop_item(&mut results, root_id)?;
                    let bound = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::Let { name, bound, body }));
                }
                ExprTask::BuildLam { params } => {
                    let body = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::Lam { params, body }));
                }
                ExprTask::BuildApp { argc } => {
                    let start = results.len().saturating_sub(argc);
                    let args = results.split_off(start);
                    let func = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::App { func, args }));
                }
                ExprTask::BuildCall { name, argc } => {
                    let start = results.len().saturating_sub(argc);
                    let args = results.split_off(start);
                    results.push(self.arena.alloc_expr(Expr::Call { name, args }));
                }
                ExprTask::BuildTuple { count } => {
                    let start = results.len().saturating_sub(count);
                    let elems = results.split_off(start);
                    results.push(self.arena.alloc_expr(Expr::Tuple(elems)));
                }
                ExprTask::BuildProj { index } => {
                    let expr = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::Proj { expr, index }));
                }
                ExprTask::BuildCon { ty, ctor, argc } => {
                    let start = results.len().saturating_sub(argc);
                    let args = results.split_off(start);
                    results.push(self.arena.alloc_expr(Expr::Con { ty, ctor, args }));
                }
                ExprTask::BuildCase { headers } => {
                    let start = results.len().saturating_sub(headers.len());
                    let bodies = results.split_off(start);
                    let expr = pop_item(&mut results, root_id)?;
                    let arms = headers
                        .into_iter()
                        .zip(bodies)
                        .map(|((ctor, names), body)| Arm { ctor, names, body })
                        .collect();
                    results.push(self.arena.alloc_expr(Expr::Case { expr, arms }));
                }
                ExprTask::BuildIf => {
                    let else_ = pop_item(&mut results, root_id)?;
                    let then_ = pop_item(&mut results, root_id)?;
                    let cond = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::If { cond, then_, else_ }));
                }
                ExprTask::BuildPrim { prim, argc } => {
                    let start = results.len().saturating_sub(argc);
                    let args = results.split_off(start);
                    results.push(self.arena.alloc_expr(Expr::Prim { prim, args }));
                }
                ExprTask::BuildFail => {
                    let inner = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::Fail(inner)));
                }
                ExprTask::BuildJoin { name, params } => {
                    let rest = pop_item(&mut results, root_id)?;
                    let body = pop_item(&mut results, root_id)?;
                    results.push(self.arena.alloc_expr(Expr::Join {
                        name,
                        params,
                        body,
                        rest,
                    }));
                }
                ExprTask::BuildJump { name, argc } => {
                    let start = results.len().saturating_sub(argc);
                    let args = results.split_off(start);
                    results.push(self.arena.alloc_expr(Expr::Jump { name, args }));
                }
            }
        }
        pop_item(&mut results, root_id)
    }

    fn decode_opt_span(&self, id: u32, depth: usize) -> PResult<Option<Span>> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "none" => {
                self.expect_arity(id, args, 0)?;
                Ok(None)
            }
            "some" => {
                self.expect_arity(id, args, 1)?;
                let fields = self.open_record(args[0], "SourceSpan", 3, depth + 1)?;
                Ok(Some(Span::new(
                    self.decode_u32(fields[0])?,
                    self.decode_u32(fields[1])?,
                    self.decode_u32(fields[2])?,
                )))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_opt_string(&self, id: u32, depth: usize) -> PResult<Option<String>> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "none" => {
                self.expect_arity(id, args, 0)?;
                Ok(None)
            }
            "some" => {
                self.expect_arity(id, args, 1)?;
                Ok(Some(self.pool.expect_string(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_opt_u32(&self, id: u32, depth: usize) -> PResult<Option<u32>> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "none" => {
                self.expect_arity(id, args, 0)?;
                Ok(None)
            }
            "some" => {
                self.expect_arity(id, args, 1)?;
                Ok(Some(self.decode_u32(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_opt_bool(&self, id: u32, depth: usize) -> PResult<Option<bool>> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "none" => {
                self.expect_arity(id, args, 0)?;
                Ok(None)
            }
            "some" => {
                self.expect_arity(id, args, 1)?;
                Ok(Some(self.decode_bool(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_opt_natural(&self, id: u32, depth: usize) -> PResult<Option<CoreNatural>> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "none" => {
                self.expect_arity(id, args, 0)?;
                Ok(None)
            }
            "some" => {
                self.expect_arity(id, args, 1)?;
                Ok(Some(self.decode_natural(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_body_meta(&self, id: u32, depth: usize) -> PResult<BodyMeta> {
        let fields = self.open_record(id, "BodyMeta", 4, depth)?;
        let span_nodes = self.open_list(fields[0], depth + 1)?;
        let mut spans = Vec::with_capacity(span_nodes.len());
        for &n in span_nodes {
            spans.push(self.decode_opt_span(n, depth + 2)?);
        }
        let param_nodes = self.open_list(fields[1], depth + 1)?;
        let mut params = Vec::with_capacity(param_nodes.len());
        for &n in param_nodes {
            params.push(self.decode_opt_string(n, depth + 2)?);
        }
        let bind_nodes = self.open_list(fields[2], depth + 1)?;
        let mut bindings = Vec::with_capacity(bind_nodes.len());
        for &n in bind_nodes {
            let bf = self.open_record(n, "BindingMeta", 3, depth + 2)?;
            bindings.push(BindingMeta {
                node: self.decode_u32(bf[0])?,
                slot: self.decode_u32(bf[1])?,
                name: self.pool.expect_string(bf[2])?,
            });
        }
        let clos_nodes = self.open_list(fields[3], depth + 1)?;
        let mut closures = Vec::with_capacity(clos_nodes.len());
        for &n in clos_nodes {
            let cf = self.open_untagged(n, 2, depth + 2)?;
            closures.push(ClosureMeta {
                node: self.decode_u32(cf[0])?,
                name: self.pool.expect_string(cf[1])?,
            });
        }
        Ok(BodyMeta {
            spans,
            params,
            bindings,
            closures,
        })
    }

    fn decode_metadata(&self, id: u32, depth: usize) -> PResult<Metadata> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "noSource" => {
                self.expect_arity(id, args, 0)?;
                Ok(Metadata::NoSource)
            }
            "Metadata" => {
                self.expect_arity(id, args, 4)?;
                let files = self.decode_string_list(args[0], depth + 1)?;

                let mod_nodes = self.open_list(args[1], depth + 1)?;
                let mut modules = Vec::with_capacity(mod_nodes.len());
                for &mn in mod_nodes {
                    let mf = self.open_record(mn, "ModuleMeta", 6, depth + 2)?;
                    let name = self.pool.expect_string(mf[0])?;
                    let data = self.decode_u32_list(mf[1], depth + 3)?;
                    let funs = self.decode_u32_list(mf[2], depth + 3)?;
                    let consts = self.decode_u32_list(mf[3], depth + 3)?;
                    let exp_ty_nodes = self.open_list(mf[4], depth + 3)?;
                    let mut exposed_types = Vec::with_capacity(exp_ty_nodes.len());
                    for &et in exp_ty_nodes {
                        let ef = self.open_untagged(et, 2, depth + 4)?;
                        exposed_types.push(ExposedType {
                            index: self.decode_u32(ef[0])?,
                            contents_exposed: self.decode_bool(ef[1])?,
                        });
                    }
                    let exposed_funs = self.decode_u32_list(mf[5], depth + 3)?;
                    modules.push(ModuleMeta {
                        name,
                        data,
                        funs,
                        consts,
                        exposed_types,
                        exposed_funs,
                    });
                }

                let fun_nodes = self.open_list(args[2], depth + 1)?;
                let mut funs = Vec::with_capacity(fun_nodes.len());
                for &fn_node in fun_nodes {
                    let ff = self.open_record(fn_node, "FunMeta", 7, depth + 2)?;
                    let name = self.pool.expect_string(ff[0])?;
                    let span = self.decode_opt_span(ff[1], depth + 3)?;
                    let text = self.decode_opt_string(ff[2], depth + 3)?;
                    let body = self.decode_body_meta(ff[3], depth + 3)?;
                    let subject = self.decode_opt_u32(ff[4], depth + 3)?;
                    let allows = self.decode_opt_bool(ff[5], depth + 3)?;
                    let line_nodes = self.open_list(ff[6], depth + 3)?;
                    let mut lines = Vec::with_capacity(line_nodes.len());
                    for &ln in line_nodes {
                        let lf = self.open_record(ln, "SpecLine", 2, depth + 4)?;
                        lines.push(SpecLine {
                            span: self.decode_opt_span(lf[0], depth + 5)?,
                            text: self.pool.expect_string(lf[1])?,
                        });
                    }
                    funs.push(FnMeta {
                        name,
                        span,
                        text,
                        body,
                        subject,
                        allows,
                        lines,
                    });
                }

                let const_nodes = self.open_list(args[3], depth + 1)?;
                let mut consts = Vec::with_capacity(const_nodes.len());
                for &cn in const_nodes {
                    let cf = self.open_record(cn, "ConstMeta", 3, depth + 2)?;
                    consts.push(ConstMeta {
                        name: self.pool.expect_string(cf[0])?,
                        span: self.decode_opt_span(cf[1], depth + 3)?,
                        body: self.decode_body_meta(cf[2], depth + 3)?,
                    });
                }

                Ok(Metadata::Source(Box::new(SourceMetadata {
                    files,
                    file_bytes: Vec::new(),
                    modules,
                    funs,
                    consts,
                })))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_program(&mut self, id: u32, depth: usize) -> PResult<Program> {
        let fields = self.open_record(id, "Program", 5, depth)?;

        let data_nodes = self.open_list(fields[0], depth + 1)?;
        let mut data = Vec::with_capacity(data_nodes.len());
        for &dn in data_nodes {
            let df = self.open_record(dn, "DataDecl", 2, depth + 2)?;
            let name = self.pool.expect_string(df[0])?;
            let ctor_nodes = self.open_list(df[1], depth + 3)?;
            let mut ctors = Vec::with_capacity(ctor_nodes.len());
            for &cn in ctor_nodes {
                let cf = self.open_record(cn, "Ctor", 2, depth + 4)?;
                let cname = self.pool.expect_string(cf[0])?;
                let cfields = self.decode_params(cf[1], depth + 5)?;
                ctors.push(CtorDecl {
                    name: cname,
                    fields: cfields,
                });
            }
            data.push(DataDecl { name, ctors });
        }

        let fun_nodes = self.open_list(fields[1], depth + 1)?;
        let mut funs = Vec::with_capacity(fun_nodes.len());
        for &fn_node in fun_nodes {
            let ff = self.open_record(fn_node, "FunDecl", 7, depth + 2)?;
            let name = self.pool.expect_string(ff[0])?;
            let params = self.decode_params(ff[1], depth + 3)?;
            let ret = self.decode_ty(ff[2], depth + 3)?;
            let body = self.decode_expr(ff[3], depth + 3)?;
            let module = self.pool.expect_string(ff[4])?;
            let kind = self.decode_nullary_op(ff[5], depth + 3, FnKind::from_wire)?;
            let const_count = self.decode_natural(ff[6])?;
            funs.push(FnDecl {
                name,
                params,
                ret,
                body,
                module,
                kind,
                const_count,
            });
        }

        let const_nodes = self.open_list(fields[2], depth + 1)?;
        let mut consts = Vec::with_capacity(const_nodes.len());
        for &cn in const_nodes {
            let cf = self.open_record(cn, "ConstDecl", 3, depth + 2)?;
            let name = self.pool.expect_string(cf[0])?;
            let ty = self.decode_ty(cf[1], depth + 3)?;
            let body = self.decode_expr(cf[2], depth + 3)?;
            consts.push(ConstDecl { name, ty, body });
        }

        let entry = self.pool.expect_string(fields[3])?;
        let metadata = self.decode_metadata(fields[4], depth + 1)?;
        Ok(Program {
            data,
            funs,
            consts,
            entry,
            metadata,
        })
    }

    /// Iterative Value decoder (O(1) Rust call stack up to `MO_LIMIT_CORE_DEPTH`).
    fn decode_value(&mut self, root_id: u32, root_depth: usize) -> PResult<ValueId> {
        enum ValTask {
            Enter {
                id: u32,
                depth: usize,
            },
            BuildTuple {
                count: usize,
            },
            BuildList {
                count: usize,
            },
            BuildSet {
                count: usize,
            },
            BuildMap {
                pairs: usize,
            },
            BuildCon {
                ctor: String,
                count: usize,
            },
            BuildClosure {
                env_names: Vec<String>,
                params: Vec<Param>,
                body: ExprId,
                const_names: Vec<String>,
            },
        }

        let mut tasks = vec![ValTask::Enter {
            id: root_id,
            depth: root_depth,
        }];
        let mut results: Vec<ValueId> = Vec::new();

        while let Some(task) = tasks.pop() {
            match task {
                ValTask::Enter { id, depth } => {
                    let (tag, args) = self.open_tagged(id, depth)?;
                    let cd = depth + 1;
                    match tag {
                        "int" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Int(self.decode_integer(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "bigInt" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::BigInt(self.decode_integer(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "byte" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Byte(self.decode_integer(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "decimal" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Decimal(self.decode_integer(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "float" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Float(self.decode_bits(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "bool" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Bool(self.decode_bool(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "char" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Char(self.decode_char(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "text" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Text(self.pool.expect_string(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "bytes" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Bytes(self.decode_integer_list(args[0], cd)?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "unit" => {
                            self.expect_arity(id, args, 0)?;
                            results.push(self.arena.alloc_value(Value::Unit));
                        }
                        "time" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Time(self.decode_integer(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "duration" => {
                            self.expect_arity(id, args, 1)?;
                            let v = Value::Duration(self.decode_integer(args[0])?);
                            results.push(self.arena.alloc_value(v));
                        }
                        "range" => {
                            self.expect_arity(id, args, 2)?;
                            let start = self.decode_integer(args[0])?;
                            let end = self.decode_integer(args[1])?;
                            results.push(self.arena.alloc_value(Value::Range { start, end }));
                        }
                        "tuple" => {
                            self.expect_arity(id, args, 1)?;
                            let elems = self.open_list(args[0], cd)?;
                            tasks.push(ValTask::BuildTuple { count: elems.len() });
                            for &e in elems.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: e,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "list" => {
                            self.expect_arity(id, args, 1)?;
                            let elems = self.open_list(args[0], cd)?;
                            tasks.push(ValTask::BuildList { count: elems.len() });
                            for &e in elems.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: e,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "set" => {
                            self.expect_arity(id, args, 1)?;
                            let elems = self.open_list(args[0], cd)?;
                            tasks.push(ValTask::BuildSet { count: elems.len() });
                            for &e in elems.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: e,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "map" => {
                            self.expect_arity(id, args, 1)?;
                            let pair_nodes = self.open_list(args[0], cd)?;
                            let mut kv_ids = Vec::with_capacity(pair_nodes.len() * 2);
                            for &pn in pair_nodes {
                                let pf = self.open_untagged(pn, 2, cd + 1)?;
                                kv_ids.push(pf[0]);
                                kv_ids.push(pf[1]);
                            }
                            tasks.push(ValTask::BuildMap {
                                pairs: pair_nodes.len(),
                            });
                            for &vid in kv_ids.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: vid,
                                    depth: cd + 2,
                                });
                            }
                        }
                        "con" => {
                            self.expect_arity(id, args, 2)?;
                            let ctor = self.pool.expect_string(args[0])?;
                            let fields = self.open_list(args[1], cd)?;
                            tasks.push(ValTask::BuildCon {
                                ctor,
                                count: fields.len(),
                            });
                            for &f in fields.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: f,
                                    depth: cd + 1,
                                });
                            }
                        }
                        "closure" => {
                            self.expect_arity(id, args, 4)?;
                            let env_nodes = self.open_list(args[0], cd)?;
                            let mut env_names = Vec::with_capacity(env_nodes.len());
                            let mut env_vals = Vec::with_capacity(env_nodes.len());
                            for &bn in env_nodes {
                                let bf = self.open_untagged(bn, 2, cd + 1)?;
                                env_names.push(self.pool.expect_string(bf[0])?);
                                env_vals.push(bf[1]);
                            }
                            let params = self.decode_params(args[1], cd)?;
                            let body = self.decode_expr(args[2], cd)?;
                            let const_nodes = self.open_list(args[3], cd)?;
                            let mut const_names = Vec::with_capacity(const_nodes.len());
                            let mut const_vals = Vec::with_capacity(const_nodes.len());
                            for &bn in const_nodes {
                                let bf = self.open_untagged(bn, 2, cd + 1)?;
                                const_names.push(self.pool.expect_string(bf[0])?);
                                const_vals.push(bf[1]);
                            }
                            tasks.push(ValTask::BuildClosure {
                                env_names,
                                params,
                                body,
                                const_names,
                            });
                            for &vid in const_vals.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: vid,
                                    depth: cd + 2,
                                });
                            }
                            for &vid in env_vals.iter().rev() {
                                tasks.push(ValTask::Enter {
                                    id: vid,
                                    depth: cd + 2,
                                });
                            }
                        }
                        "cap" => {
                            self.expect_arity(id, args, 3)?;
                            let cap = self.decode_cap(args[0], cd)?;
                            let root = self.decode_opt_string(args[1], cd)?;
                            let fake = self.decode_opt_natural(args[2], cd)?;
                            results.push(self.arena.alloc_value(Value::Cap { cap, root, fake }));
                        }
                        _ => {
                            return Err(self.pool.err_node(
                                id,
                                ErrorCode::CoreConstructor,
                                "unknown Core constructor",
                            ));
                        }
                    }
                }
                ValTask::BuildTuple { count } => {
                    let start = results.len().saturating_sub(count);
                    let elems = results.split_off(start);
                    results.push(self.arena.alloc_value(Value::Tuple(elems)));
                }
                ValTask::BuildList { count } => {
                    let start = results.len().saturating_sub(count);
                    let elems = results.split_off(start);
                    results.push(self.arena.alloc_value(Value::List(elems)));
                }
                ValTask::BuildSet { count } => {
                    let start = results.len().saturating_sub(count);
                    let elems = results.split_off(start);
                    results.push(self.arena.alloc_value(Value::Set(elems)));
                }
                ValTask::BuildMap { pairs } => {
                    let start = results.len().saturating_sub(pairs * 2);
                    let flat = results.split_off(start);
                    let mut out = Vec::with_capacity(pairs);
                    let mut iter = flat.into_iter();
                    while let (Some(k), Some(v)) = (iter.next(), iter.next()) {
                        out.push((k, v));
                    }
                    results.push(self.arena.alloc_value(Value::Map(out)));
                }
                ValTask::BuildCon { ctor, count } => {
                    let start = results.len().saturating_sub(count);
                    let fields = results.split_off(start);
                    results.push(self.arena.alloc_value(Value::Con { ctor, fields }));
                }
                ValTask::BuildClosure {
                    env_names,
                    params,
                    body,
                    const_names,
                } => {
                    let c_start = results.len().saturating_sub(const_names.len());
                    let c_vals = results.split_off(c_start);
                    let e_start = results.len().saturating_sub(env_names.len());
                    let e_vals = results.split_off(e_start);
                    let env = env_names
                        .into_iter()
                        .zip(e_vals)
                        .map(|(name, value)| Binding { name, value })
                        .collect();
                    let consts = const_names
                        .into_iter()
                        .zip(c_vals)
                        .map(|(name, value)| Binding { name, value })
                        .collect();
                    results.push(self.arena.alloc_value(Value::Closure {
                        env,
                        params,
                        body,
                        consts,
                    }));
                }
            }
        }
        pop_item(&mut results, root_id)
    }

    fn decode_values(&mut self, id: u32, depth: usize) -> PResult<Vec<ValueId>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            out.push(self.decode_value(child, depth + 1)?);
        }
        Ok(out)
    }

    fn decode_random_state(&self, id: u32, depth: usize) -> PResult<RandomState> {
        let f = self.open_record(id, "RandomState", 4, depth)?;
        Ok(RandomState {
            s0: self.decode_bits(f[0])?,
            s1: self.decode_bits(f[1])?,
            s2: self.decode_bits(f[2])?,
            s3: self.decode_bits(f[3])?,
        })
    }

    fn decode_cap_state(&self, id: u32, depth: usize) -> PResult<CapState> {
        let f = self.open_record(id, "CapState", 9, depth)?;
        let cd = depth + 1;
        Ok(CapState {
            args: self.decode_string_list(f[0], cd)?,
            env: self.decode_string_pairs(f[1], cd)?,
            stdin: self.decode_string_list(f[2], cd)?,
            stdout: self.decode_string_list(f[3], cd)?,
            stderr: self.decode_string_list(f[4], cd)?,
            files: self.decode_string_pairs(f[5], cd)?,
            now: self.decode_integer(f[6])?,
            random: self.decode_random_state(f[7], cd)?,
            folders: self.decode_string_list(f[8], cd)?,
        })
    }

    fn decode_cap_states(&self, id: u32, depth: usize) -> PResult<Vec<CapState>> {
        let items = self.open_list(id, depth)?;
        let mut out = Vec::with_capacity(items.len());
        for &child in items {
            out.push(self.decode_cap_state(child, depth + 1)?);
        }
        Ok(out)
    }

    fn decode_world(&self, id: u32, depth: usize) -> PResult<World> {
        let f = self.open_record(id, "World", 10, depth)?;
        let cd = depth + 1;
        Ok(World {
            args: self.decode_string_list(f[0], cd)?,
            env: self.decode_string_pairs(f[1], cd)?,
            stdin: self.decode_string_list(f[2], cd)?,
            stdout: self.decode_string_list(f[3], cd)?,
            stderr: self.decode_string_list(f[4], cd)?,
            files: self.decode_string_pairs(f[5], cd)?,
            now: self.decode_integer(f[6])?,
            random: self.decode_random_state(f[7], cd)?,
            folders: self.decode_string_list(f[8], cd)?,
            fakes: self.decode_cap_states(f[9], cd)?,
        })
    }

    fn decode_bug(&self, id: u32, depth: usize) -> PResult<Bug> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "overflow" => {
                self.expect_arity(id, args, 1)?;
                Ok(Bug::Overflow(self.pool.expect_string(args[0])?))
            }
            "divByZero" => {
                self.expect_arity(id, args, 0)?;
                Ok(Bug::DivByZero)
            }
            "stop" => {
                self.expect_arity(id, args, 1)?;
                Ok(Bug::Stop(self.pool.expect_string(args[0])?))
            }
            "outside" => {
                self.expect_arity(id, args, 1)?;
                Ok(Bug::Outside(self.pool.expect_string(args[0])?))
            }
            "repeatedKey" => {
                self.expect_arity(id, args, 0)?;
                Ok(Bug::RepeatedKey)
            }
            "fileFolderConflict" => {
                self.expect_arity(id, args, 1)?;
                Ok(Bug::FileFolderConflict(self.pool.expect_string(args[0])?))
            }
            "filePathInvalid" => {
                self.expect_arity(id, args, 1)?;
                Ok(Bug::FilePathInvalid(self.pool.expect_string(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_outcome(&mut self, id: u32, depth: usize) -> PResult<Outcome> {
        let (tag, args) = self.open_tagged(id, depth)?;
        let cd = depth + 1;
        match tag {
            "ok" => {
                self.expect_arity(id, args, 2)?;
                let value = self.decode_value(args[0], cd)?;
                let world = self.decode_world(args[1], cd)?;
                Ok(Outcome::Ok { value, world })
            }
            "stopped" => {
                self.expect_arity(id, args, 2)?;
                let bug = self.decode_bug(args[0], cd)?;
                let world = self.decode_world(args[1], cd)?;
                Ok(Outcome::Stopped { bug, world })
            }
            "failed" => {
                self.expect_arity(id, args, 2)?;
                let detail = self.pool.expect_string(args[0])?;
                let world = self.decode_world(args[1], cd)?;
                Ok(Outcome::Failed { detail, world })
            }
            "jumped" => {
                self.expect_arity(id, args, 3)?;
                let name = self.pool.expect_string(args[0])?;
                let values = self.decode_values(args[1], cd)?;
                let world = self.decode_world(args[2], cd)?;
                Ok(Outcome::Jumped {
                    name,
                    values,
                    world,
                })
            }
            "wrong" => {
                self.expect_arity(id, args, 1)?;
                Ok(Outcome::Wrong(self.pool.expect_string(args[0])?))
            }
            "outOfFuel" => {
                self.expect_arity(id, args, 0)?;
                Ok(Outcome::OutOfFuel)
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_effects(&self, id: u32, depth: usize) -> PResult<Effects> {
        let f = self.open_record(id, "effects", 5, depth)?;
        let cd = depth + 1;
        Ok(Effects {
            stdout: self.decode_string_list(f[0], cd)?,
            stderr: self.decode_string_list(f[1], cd)?,
            files: self.decode_string_pairs(f[2], cd)?,
            folders: self.decode_string_list(f[3], cd)?,
            fakes: self.decode_cap_states(f[4], cd)?,
        })
    }

    fn decode_report(&self, id: u32, depth: usize) -> PResult<Report> {
        let (tag, args) = self.open_tagged(id, depth)?;
        let cd = depth + 1;
        match tag {
            "returned" => {
                self.expect_arity(id, args, 2)?;
                Ok(Report::Returned {
                    value: self.pool.expect_string(args[0])?,
                    effects: self.decode_effects(args[1], cd)?,
                })
            }
            "stopped" => {
                self.expect_arity(id, args, 2)?;
                Ok(Report::Stopped {
                    bug: self.decode_bug(args[0], cd)?,
                    effects: self.decode_effects(args[1], cd)?,
                })
            }
            "failed" => {
                self.expect_arity(id, args, 2)?;
                Ok(Report::Failed {
                    detail: self.pool.expect_string(args[0])?,
                    effects: self.decode_effects(args[1], cd)?,
                })
            }
            "outOfFuel" => {
                self.expect_arity(id, args, 0)?;
                Ok(Report::OutOfFuel)
            }
            "wrong" => {
                self.expect_arity(id, args, 1)?;
                Ok(Report::Wrong(self.pool.expect_string(args[0])?))
            }
            "parseError" => {
                self.expect_arity(id, args, 1)?;
                Ok(Report::ParseError(self.pool.expect_string(args[0])?))
            }
            "illTyped" => {
                self.expect_arity(id, args, 1)?;
                Ok(Report::IllTyped(self.pool.expect_string(args[0])?))
            }
            "invalidRun" => {
                self.expect_arity(id, args, 1)?;
                Ok(Report::InvalidRun(self.pool.expect_string(args[0])?))
            }
            "ioError" => {
                self.expect_arity(id, args, 1)?;
                Ok(Report::IoError(self.pool.expect_string(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_selection(&mut self, id: u32, depth: usize) -> PResult<Selection> {
        let (tag, args) = self.open_tagged(id, depth)?;
        match tag {
            "entry" => {
                self.expect_arity(id, args, 1)?;
                Ok(Selection::Entry(self.decode_values(args[0], depth + 1)?))
            }
            "test" => {
                self.expect_arity(id, args, 1)?;
                Ok(Selection::Test(self.pool.expect_string(args[0])?))
            }
            _ => {
                Err(self
                    .pool
                    .err_node(id, ErrorCode::CoreConstructor, "unknown Core constructor"))
            }
        }
    }

    fn decode_run(&mut self, id: u32, depth: usize) -> PResult<Run> {
        let f = self.open_record(id, "Run", 4, depth)?;
        let cd = depth + 1;
        let program = self.decode_program(f[0], cd)?;
        let world = self.decode_world(f[1], cd)?;
        let selection = self.decode_selection(f[2], cd)?;
        let fuel = self.decode_natural(f[3])?;
        Ok(Run {
            program,
            world,
            selection,
            fuel,
        })
    }

    fn decode_document(&mut self, kind: CoreTextKind, id: u32) -> PResult<CoreDocument> {
        let depth = 2;
        match kind {
            CoreTextKind::Type => Ok(CoreDocument::Type(self.decode_ty(id, depth)?)),
            CoreTextKind::Cap => Ok(CoreDocument::Cap(self.decode_cap(id, depth)?)),
            CoreTextKind::Literal => Ok(CoreDocument::Literal(self.decode_lit(id, depth)?)),
            CoreTextKind::Primitive => Ok(CoreDocument::Primitive(self.decode_prim(id, depth)?)),
            CoreTextKind::Expr => Ok(CoreDocument::Expr(self.decode_expr(id, depth)?)),
            CoreTextKind::Program => Ok(CoreDocument::Program(self.decode_program(id, depth)?)),
            CoreTextKind::Value => Ok(CoreDocument::Value(self.decode_value(id, depth)?)),
            CoreTextKind::World => Ok(CoreDocument::World(self.decode_world(id, depth)?)),
            CoreTextKind::Bug => Ok(CoreDocument::Bug(self.decode_bug(id, depth)?)),
            CoreTextKind::Outcome => Ok(CoreDocument::Outcome(self.decode_outcome(id, depth)?)),
            CoreTextKind::Effects => Ok(CoreDocument::Effects(self.decode_effects(id, depth)?)),
            CoreTextKind::Report => Ok(CoreDocument::Report(self.decode_report(id, depth)?)),
            CoreTextKind::Selection => {
                Ok(CoreDocument::Selection(self.decode_selection(id, depth)?))
            }
            CoreTextKind::Run => Ok(CoreDocument::Run(self.decode_run(id, depth)?)),
            CoreTextKind::CapState => Ok(CoreDocument::CapState(self.decode_cap_state(id, depth)?)),
            CoreTextKind::RandomState => Ok(CoreDocument::RandomState(
                self.decode_random_state(id, depth)?,
            )),
        }
    }
}

fn pop_item<T>(vec: &mut Vec<T>, err_node: u32) -> PResult<T> {
    vec.pop().ok_or(ParseErr {
        code: ErrorCode::CoreField,
        start: err_node,
        end: err_node,
        message: "internal decoder stack underflow",
    })
}

// ---------------------------------------------------------------------------
// Canonical printer (`src/core/text.c:mo_core_text_print`)
// ---------------------------------------------------------------------------

struct Printer<'a> {
    arena: &'a CoreArena,
    out: Vec<u8>,
    nodes: usize,
    limit_bytes: usize,
}

type PrintRes = Result<(), CorePrintError>;

impl<'a> Printer<'a> {
    fn new(arena: &'a CoreArena) -> Self {
        Self {
            arena,
            out: Vec::new(),
            nodes: 3, // outer list, core, version
            limit_bytes: MO_LIMIT_CORE_TEXT_SIZE as usize,
        }
    }

    fn push_byte(&mut self, b: u8) -> PrintRes {
        if self.out.len() >= self.limit_bytes {
            return Err(CorePrintError::LimitExceeded);
        }
        self.out.push(b);
        Ok(())
    }

    fn push_bytes(&mut self, data: &[u8]) -> PrintRes {
        if data.len() > self.limit_bytes.saturating_sub(self.out.len()) {
            return Err(CorePrintError::LimitExceeded);
        }
        self.out.extend_from_slice(data);
        Ok(())
    }

    fn push_str(&mut self, s: &str) -> PrintRes {
        self.push_bytes(s.as_bytes())
    }

    fn bump_node(&mut self) -> PrintRes {
        self.nodes += 1;
        if self.nodes > MO_LIMIT_CORE_NODES as usize {
            return Err(CorePrintError::LimitExceeded);
        }
        Ok(())
    }

    fn check_container(&mut self, depth: usize) -> PrintRes {
        self.bump_node()?;
        if depth > MO_LIMIT_CORE_DEPTH as usize {
            return Err(CorePrintError::LimitExceeded);
        }
        Ok(())
    }

    fn begin_tagged(&mut self, wire: &str, depth: usize) -> PrintRes {
        self.check_container(depth)?;
        self.bump_node()?;
        self.push_byte(b'(')?;
        self.push_str(wire)
    }

    fn begin_untagged(&mut self, depth: usize) -> PrintRes {
        self.check_container(depth)?;
        self.push_byte(b'(')
    }

    fn begin_list(&mut self, len: usize, depth: usize) -> PrintRes {
        self.check_container(depth)?;
        if len > MO_LIMIT_CORE_NODES as usize {
            return Err(CorePrintError::LimitExceeded);
        }
        self.push_byte(b'(')
    }

    fn write_string(&mut self, s: &str) -> PrintRes {
        self.bump_node()?;
        self.write_quoted(s)
    }

    fn write_quoted(&mut self, s: &str) -> PrintRes {
        if s.len() > self.limit_bytes {
            return Err(CorePrintError::LimitExceeded);
        }
        self.push_byte(b'"')?;
        for ch in s.chars() {
            match ch {
                '"' => self.push_str("\\\"")?,
                '\\' => self.push_str("\\\\")?,
                '\n' => self.push_str("\\n")?,
                '\r' => self.push_str("\\r")?,
                '\t' => self.push_str("\\t")?,
                c if (c as u32) < 32 || (c as u32) == 127 => {
                    self.push_str("\\u{")?;
                    self.write_hex_minimal(c as u32)?;
                    self.push_byte(b'}')?;
                }
                c => {
                    let mut buf = [0u8; 4];
                    self.push_str(c.encode_utf8(&mut buf))?;
                }
            }
        }
        self.push_byte(b'"')
    }

    fn write_hex_minimal(&mut self, mut n: u32) -> PrintRes {
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let mut buf = [b'0'; 8];
        let mut i = 8;
        loop {
            i -= 1;
            buf[i] = HEX[(n & 0xF) as usize];
            n >>= 4;
            if n == 0 {
                break;
            }
        }
        self.push_bytes(&buf[i..])
    }

    fn write_hex_u64(&mut self, mut n: u64) -> PrintRes {
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let mut buf = [b'0'; 16];
        for idx in (0..16).rev() {
            buf[idx] = HEX[(n & 0xF) as usize];
            n >>= 4;
        }
        self.push_str("0x")?;
        self.push_bytes(&buf)
    }

    fn write_bits(&mut self, bits: u64) -> PrintRes {
        self.bump_node()?;
        self.write_hex_u64(bits)
    }

    fn write_integer(&mut self, n: &CoreInteger) -> PrintRes {
        self.bump_node()?;
        let Some((neg, digits)) = n.canonical_parts() else {
            return Err(CorePrintError::InvalidInteger);
        };
        if neg {
            self.push_byte(b'-')?;
        }
        self.push_str(digits)
    }

    fn write_natural(&mut self, n: &CoreNatural) -> PrintRes {
        self.bump_node()?;
        let Some(digits) = n.canonical_digits() else {
            return Err(CorePrintError::InvalidInteger);
        };
        self.push_str(digits)
    }

    fn write_u32(&mut self, n: u32) -> PrintRes {
        self.bump_node()?;
        let mut buf = [0u8; 10];
        let mut i = 10;
        let mut v = n;
        loop {
            i -= 1;
            buf[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.push_bytes(&buf[i..])
    }

    fn write_bool(&mut self, b: bool) -> PrintRes {
        self.bump_node()?;
        self.push_str(if b { "true" } else { "false" })
    }

    fn write_char(&mut self, ch: char) -> PrintRes {
        self.bump_node()?;
        let mut buf = [0u8; 4];
        self.write_quoted(ch.encode_utf8(&mut buf))
    }

    fn write_string_list(&mut self, list: &[String], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, s) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_string(s)?;
        }
        self.push_byte(b')')
    }

    fn write_u32_list(&mut self, list: &[u32], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, &n) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_u32(n)?;
        }
        self.push_byte(b')')
    }

    fn write_integer_list(&mut self, list: &[CoreInteger], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, n) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_integer(n)?;
        }
        self.push_byte(b')')
    }

    fn write_string_pairs(&mut self, list: &[(String, String)], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, (k, v)) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_untagged(depth + 1)?;
            self.write_string(k)?;
            self.push_byte(b' ')?;
            self.write_string(v)?;
            self.push_byte(b')')?;
        }
        self.push_byte(b')')
    }

    fn write_cap(&mut self, cap: Cap, depth: usize) -> PrintRes {
        self.begin_tagged(cap.wire_name(), depth)?;
        self.push_byte(b')')
    }

    /// Iterative Type printer (O(1) Rust call stack up to `MO_LIMIT_CORE_DEPTH`).
    fn write_ty(&mut self, root: TyId, root_depth: usize) -> PrintRes {
        enum Task {
            Ty(TyId, usize),
            Space,
            Close,
        }
        let mut stack = vec![Task::Ty(root, root_depth)];
        while let Some(task) = stack.pop() {
            match task {
                Task::Space => self.push_byte(b' ')?,
                Task::Close => self.push_byte(b')')?,
                Task::Ty(id, depth) => {
                    let ty = self
                        .arena
                        .ty_opt(id)
                        .ok_or(CorePrintError::InvalidArenaRef)?;
                    let cd = depth + 1;
                    match ty {
                        Ty::Int => {
                            self.begin_tagged("int", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::BigInt => {
                            self.begin_tagged("bigInt", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Byte => {
                            self.begin_tagged("byte", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Decimal => {
                            self.begin_tagged("decimal", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Float => {
                            self.begin_tagged("float", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Bool => {
                            self.begin_tagged("bool", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Char => {
                            self.begin_tagged("char", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Text => {
                            self.begin_tagged("text", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Bytes => {
                            self.begin_tagged("bytes", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Unit => {
                            self.begin_tagged("unit", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Range => {
                            self.begin_tagged("range", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Time => {
                            self.begin_tagged("time", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Duration => {
                            self.begin_tagged("duration", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Ordering => {
                            self.begin_tagged("ordering", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::FileError => {
                            self.begin_tagged("fileError", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Entry => {
                            self.begin_tagged("entry", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::EntryKind => {
                            self.begin_tagged("entryKind", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Simulation => {
                            self.begin_tagged("simulation", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::SimulationError => {
                            self.begin_tagged("simulationError", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::NetworkError => {
                            self.begin_tagged("networkError", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::SqliteValue => {
                            self.begin_tagged("sqliteValue", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::SqliteRows => {
                            self.begin_tagged("sqliteRows", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::SqliteError => {
                            self.begin_tagged("sqliteError", depth)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Data(name) => {
                            self.begin_tagged("data", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b')')?;
                        }
                        Ty::Cap(cap) => {
                            self.begin_tagged("cap", depth)?;
                            self.push_byte(b' ')?;
                            self.write_cap(*cap, cd)?;
                            self.push_byte(b')')?;
                        }
                        Ty::List(elem) => {
                            self.begin_tagged("list", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Ty(*elem, cd));
                        }
                        Ty::Set(elem) => {
                            self.begin_tagged("set", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Ty(*elem, cd));
                        }
                        Ty::Option(elem) => {
                            self.begin_tagged("option", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Ty(*elem, cd));
                        }
                        Ty::Map { key, value } => {
                            self.begin_tagged("map", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Ty(*value, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Ty(*key, cd));
                        }
                        Ty::Result { ok, err } => {
                            self.begin_tagged("result", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Ty(*err, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Ty(*ok, cd));
                        }
                        Ty::Tuple(elems) => {
                            self.begin_tagged("tuple", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(elems.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &e) in elems.iter().enumerate().rev() {
                                stack.push(Task::Ty(e, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Ty::Fn { params, ret } => {
                            self.begin_tagged("fn", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(params.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Ty(*ret, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Close);
                            for (i, &p) in params.iter().enumerate().rev() {
                                stack.push(Task::Ty(p, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn write_ty_list(&mut self, list: &[TyId], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, &ty) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_ty(ty, depth + 1)?;
        }
        self.push_byte(b')')
    }

    fn write_params(&mut self, params: &[Param], depth: usize) -> PrintRes {
        self.begin_list(params.len(), depth)?;
        for (i, p) in params.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_untagged(depth + 1)?;
            self.write_string(&p.name)?;
            self.push_byte(b' ')?;
            self.write_ty(p.ty, depth + 2)?;
            self.push_byte(b')')?;
        }
        self.push_byte(b')')
    }

    fn write_lit(&mut self, lit: &Lit, depth: usize) -> PrintRes {
        let cd = depth + 1;
        match lit {
            Lit::Int(n) => {
                self.begin_tagged("int", depth)?;
                self.push_byte(b' ')?;
                self.write_integer(n)?;
            }
            Lit::BigInt(n) => {
                self.begin_tagged("bigInt", depth)?;
                self.push_byte(b' ')?;
                self.write_integer(n)?;
            }
            Lit::Byte(n) => {
                self.begin_tagged("byte", depth)?;
                self.push_byte(b' ')?;
                self.write_integer(n)?;
            }
            Lit::Decimal(n) => {
                self.begin_tagged("decimal", depth)?;
                self.push_byte(b' ')?;
                self.write_integer(n)?;
            }
            Lit::Float(bits) => {
                self.begin_tagged("float", depth)?;
                self.push_byte(b' ')?;
                self.write_bits(*bits)?;
            }
            Lit::Bool(b) => {
                self.begin_tagged("bool", depth)?;
                self.push_byte(b' ')?;
                self.write_bool(*b)?;
            }
            Lit::Char(ch) => {
                self.begin_tagged("char", depth)?;
                self.push_byte(b' ')?;
                self.write_char(*ch)?;
            }
            Lit::Text(s) => {
                self.begin_tagged("text", depth)?;
                self.push_byte(b' ')?;
                self.write_string(s)?;
            }
            Lit::Bytes(bs) => {
                self.begin_tagged("bytes", depth)?;
                self.push_byte(b' ')?;
                self.write_integer_list(bs, cd)?;
            }
            Lit::Unit => {
                self.begin_tagged("unit", depth)?;
            }
            Lit::Time(n) => {
                self.begin_tagged("time", depth)?;
                self.push_byte(b' ')?;
                self.write_integer(n)?;
            }
            Lit::Duration(n) => {
                self.begin_tagged("duration", depth)?;
                self.push_byte(b' ')?;
                self.write_integer(n)?;
            }
        }
        self.push_byte(b')')
    }

    fn write_nullary(&mut self, wire: &str, depth: usize) -> PrintRes {
        self.begin_tagged(wire, depth)?;
        self.push_byte(b')')
    }

    fn write_prim(&mut self, prim: &Prim, depth: usize) -> PrintRes {
        let cd = depth + 1;
        match prim {
            Prim::Int(op) => {
                self.begin_tagged("int", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Big(op) => {
                self.begin_tagged("big", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Byte(op) => {
                self.begin_tagged("byte", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Dec(op) => {
                self.begin_tagged("dec", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Float(op) => {
                self.begin_tagged("float", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Not => {
                self.begin_tagged("not", depth)?;
            }
            Prim::Cmp { base, op } => {
                self.begin_tagged("cmp", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(base.wire_name(), cd)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Char(op) => {
                self.begin_tagged("char", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Text(op) => {
                self.begin_tagged("text", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Bytes(op) => {
                self.begin_tagged("bytes", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Range(op) => {
                self.begin_tagged("range", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Unicode16(op) => {
                self.begin_tagged("unicode16", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Crypto(op) => {
                self.begin_tagged("crypto", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Sim(op) => {
                self.begin_tagged("sim", depth)?;
                self.push_byte(b' ')?;
                match op {
                    SimOp::Start { descriptor } => {
                        self.begin_tagged("start", cd)?;
                        self.push_byte(b' ')?;
                        self.write_natural(descriptor)?;
                        self.push_byte(b')')?;
                    }
                    SimOp::Send {
                        descriptor,
                        tool,
                        arguments,
                        result,
                    } => {
                        self.begin_tagged("send", cd)?;
                        self.push_byte(b' ')?;
                        self.write_natural(descriptor)?;
                        self.push_byte(b' ')?;
                        self.write_natural(tool)?;
                        self.push_byte(b' ')?;
                        self.write_ty_list(arguments, cd + 1)?;
                        self.push_byte(b' ')?;
                        self.write_ty(*result, cd + 1)?;
                        self.push_byte(b')')?;
                    }
                    SimOp::Advance => self.write_nullary("advance", cd)?,
                    SimOp::Crash => self.write_nullary("crash", cd)?,
                    SimOp::Restart => self.write_nullary("restart", cd)?,
                }
            }
            Prim::Time(op) => {
                self.begin_tagged("time", depth)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::List { ty, op } => {
                self.begin_tagged("list", depth)?;
                self.push_byte(b' ')?;
                self.write_ty(*ty, cd)?;
                self.push_byte(b' ')?;
                match op {
                    ListOp::Lit(n) => {
                        self.begin_tagged("lit", cd)?;
                        self.push_byte(b' ')?;
                        self.write_natural(n)?;
                        self.push_byte(b')')?;
                    }
                    ListOp::Nullary(n) => self.write_nullary(n.wire_name(), cd)?,
                    ListOp::Zip(other) => {
                        self.begin_tagged("zip", cd)?;
                        self.push_byte(b' ')?;
                        self.write_ty(*other, cd + 1)?;
                        self.push_byte(b')')?;
                    }
                }
            }
            Prim::Map { key, value, op } => {
                self.begin_tagged("map", depth)?;
                self.push_byte(b' ')?;
                self.write_ty(*key, cd)?;
                self.push_byte(b' ')?;
                self.write_ty(*value, cd)?;
                self.push_byte(b' ')?;
                match op {
                    MapOp::Nullary(n) => self.write_nullary(n.wire_name(), cd)?,
                    MapOp::Lit(n) => {
                        self.begin_tagged("lit", cd)?;
                        self.push_byte(b' ')?;
                        self.write_natural(n)?;
                        self.push_byte(b')')?;
                    }
                }
            }
            Prim::Set { ty, op } => {
                self.begin_tagged("set", depth)?;
                self.push_byte(b' ')?;
                self.write_ty(*ty, cd)?;
                self.push_byte(b' ')?;
                self.write_nullary(op.wire_name(), cd)?;
            }
            Prim::Eq(ty) => {
                self.begin_tagged("eq", depth)?;
                self.push_byte(b' ')?;
                self.write_ty(*ty, cd)?;
            }
            Prim::Structural(ty) => {
                self.begin_tagged("structural", depth)?;
                self.push_byte(b' ')?;
                self.write_ty(*ty, cd)?;
            }
            Prim::Io(op) => {
                self.begin_tagged("io", depth)?;
                self.push_byte(b' ')?;
                match op {
                    IoOp::Nullary(n) => self.write_nullary(n.wire_name(), cd)?,
                    IoOp::ReadText { ro } => {
                        self.begin_tagged("readText", cd)?;
                        self.push_byte(b' ')?;
                        self.write_bool(*ro)?;
                        self.push_byte(b')')?;
                    }
                    IoOp::ReadBytes { ro } => {
                        self.begin_tagged("readBytes", cd)?;
                        self.push_byte(b' ')?;
                        self.write_bool(*ro)?;
                        self.push_byte(b')')?;
                    }
                    IoOp::List { ro } => {
                        self.begin_tagged("list", cd)?;
                        self.push_byte(b' ')?;
                        self.write_bool(*ro)?;
                        self.push_byte(b')')?;
                    }
                    IoOp::Entries { ro } => {
                        self.begin_tagged("entries", cd)?;
                        self.push_byte(b' ')?;
                        self.write_bool(*ro)?;
                        self.push_byte(b')')?;
                    }
                    IoOp::Exists { ro } => {
                        self.begin_tagged("exists", cd)?;
                        self.push_byte(b' ')?;
                        self.write_bool(*ro)?;
                        self.push_byte(b')')?;
                    }
                    IoOp::Within { ro } => {
                        self.begin_tagged("within", cd)?;
                        self.push_byte(b' ')?;
                        self.write_bool(*ro)?;
                        self.push_byte(b')')?;
                    }
                }
            }
        }
        self.push_byte(b')')
    }

    /// Iterative Expr printer (O(1) Rust call stack up to `MO_LIMIT_CORE_DEPTH`).
    fn write_expr(&mut self, root: ExprId, root_depth: usize) -> PrintRes {
        enum Task<'a> {
            Expr(ExprId, usize),
            OpenList(usize, usize),
            ArmHeader(&'a Arm, usize),
            Natural(&'a CoreNatural),
            Space,
            Close,
        }

        let mut stack = vec![Task::Expr(root, root_depth)];
        while let Some(task) = stack.pop() {
            match task {
                Task::Space => self.push_byte(b' ')?,
                Task::Close => self.push_byte(b')')?,
                Task::OpenList(len, depth) => self.begin_list(len, depth)?,
                Task::Natural(n) => self.write_natural(n)?,
                Task::ArmHeader(arm, depth) => {
                    self.begin_untagged(depth)?;
                    self.write_string(&arm.ctor)?;
                    self.push_byte(b' ')?;
                    self.write_string_list(&arm.names, depth + 1)?;
                    self.push_byte(b' ')?;
                }
                Task::Expr(id, depth) => {
                    let expr = self
                        .arena
                        .expr_opt(id)
                        .ok_or(CorePrintError::InvalidArenaRef)?;
                    let cd = depth + 1;
                    match expr {
                        Expr::Lit(lit) => {
                            self.begin_tagged("lit", depth)?;
                            self.push_byte(b' ')?;
                            self.write_lit(lit, cd)?;
                            self.push_byte(b')')?;
                        }
                        Expr::Var(name) => {
                            self.begin_tagged("var", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b')')?;
                        }
                        Expr::Global(name) => {
                            self.begin_tagged("global", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b')')?;
                        }
                        Expr::Stop(msg) => {
                            self.begin_tagged("stop", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(msg)?;
                            self.push_byte(b')')?;
                        }
                        Expr::Let { name, bound, body } => {
                            self.begin_tagged("let_", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Expr(*body, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*bound, cd));
                        }
                        Expr::Lam { params, body } => {
                            self.begin_tagged("lam", depth)?;
                            self.push_byte(b' ')?;
                            self.write_params(params, cd)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Expr(*body, cd));
                        }
                        Expr::App { func, args } => {
                            self.begin_tagged("app", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &a) in args.iter().enumerate().rev() {
                                stack.push(Task::Expr(a, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                            stack.push(Task::OpenList(args.len(), cd));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*func, cd));
                        }
                        Expr::Call { name, args } => {
                            self.begin_tagged("call", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b' ')?;
                            self.begin_list(args.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &a) in args.iter().enumerate().rev() {
                                stack.push(Task::Expr(a, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Expr::Tuple(elems) => {
                            self.begin_tagged("tuple", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(elems.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &e) in elems.iter().enumerate().rev() {
                                stack.push(Task::Expr(e, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Expr::Proj { expr: inner, index } => {
                            self.begin_tagged("proj", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Natural(index));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*inner, cd));
                        }
                        Expr::Con { ty, ctor, args } => {
                            self.begin_tagged("con", depth)?;
                            self.push_byte(b' ')?;
                            self.write_ty(*ty, cd)?;
                            self.push_byte(b' ')?;
                            self.write_string(ctor)?;
                            self.push_byte(b' ')?;
                            self.begin_list(args.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &a) in args.iter().enumerate().rev() {
                                stack.push(Task::Expr(a, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Expr::Case { expr: scrut, arms } => {
                            self.begin_tagged("case_", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, arm) in arms.iter().enumerate().rev() {
                                stack.push(Task::Close);
                                stack.push(Task::Expr(arm.body, cd + 2));
                                stack.push(Task::ArmHeader(arm, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                            stack.push(Task::OpenList(arms.len(), cd));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*scrut, cd));
                        }
                        Expr::If { cond, then_, else_ } => {
                            self.begin_tagged("if_", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Expr(*else_, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*then_, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*cond, cd));
                        }
                        Expr::Prim { prim, args } => {
                            self.begin_tagged("prim", depth)?;
                            self.push_byte(b' ')?;
                            self.write_prim(prim, cd)?;
                            self.push_byte(b' ')?;
                            self.begin_list(args.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &a) in args.iter().enumerate().rev() {
                                stack.push(Task::Expr(a, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Expr::Fail(inner) => {
                            self.begin_tagged("fail", depth)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Expr(*inner, cd));
                        }
                        Expr::Join {
                            name,
                            params,
                            body,
                            rest,
                        } => {
                            self.begin_tagged("join", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b' ')?;
                            self.write_params(params, cd)?;
                            self.push_byte(b' ')?;
                            stack.push(Task::Close);
                            stack.push(Task::Expr(*rest, cd));
                            stack.push(Task::Space);
                            stack.push(Task::Expr(*body, cd));
                        }
                        Expr::Jump { name, args } => {
                            self.begin_tagged("jump", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(name)?;
                            self.push_byte(b' ')?;
                            self.begin_list(args.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &a) in args.iter().enumerate().rev() {
                                stack.push(Task::Expr(a, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn write_opt_span(&mut self, span: Option<Span>, depth: usize) -> PrintRes {
        match span {
            None => self.write_nullary("none", depth),
            Some(s) => {
                self.begin_tagged("some", depth)?;
                self.push_byte(b' ')?;
                self.begin_tagged("SourceSpan", depth + 1)?;
                self.push_byte(b' ')?;
                self.write_u32(s.file)?;
                self.push_byte(b' ')?;
                self.write_u32(s.start)?;
                self.push_byte(b' ')?;
                self.write_u32(s.end)?;
                self.push_str("))")
            }
        }
    }

    fn write_opt_string(&mut self, s: Option<&str>, depth: usize) -> PrintRes {
        match s {
            None => self.write_nullary("none", depth),
            Some(text) => {
                self.begin_tagged("some", depth)?;
                self.push_byte(b' ')?;
                self.write_string(text)?;
                self.push_byte(b')')
            }
        }
    }

    fn write_opt_u32(&mut self, v: Option<u32>, depth: usize) -> PrintRes {
        match v {
            None => self.write_nullary("none", depth),
            Some(n) => {
                self.begin_tagged("some", depth)?;
                self.push_byte(b' ')?;
                self.write_u32(n)?;
                self.push_byte(b')')
            }
        }
    }

    fn write_opt_bool(&mut self, v: Option<bool>, depth: usize) -> PrintRes {
        match v {
            None => self.write_nullary("none", depth),
            Some(b) => {
                self.begin_tagged("some", depth)?;
                self.push_byte(b' ')?;
                self.write_bool(b)?;
                self.push_byte(b')')
            }
        }
    }

    fn write_opt_natural(&mut self, v: Option<&CoreNatural>, depth: usize) -> PrintRes {
        match v {
            None => self.write_nullary("none", depth),
            Some(n) => {
                self.begin_tagged("some", depth)?;
                self.push_byte(b' ')?;
                self.write_natural(n)?;
                self.push_byte(b')')
            }
        }
    }

    fn write_body_meta(&mut self, body: &BodyMeta, depth: usize) -> PrintRes {
        let cd = depth + 1;
        self.begin_tagged("BodyMeta", depth)?;
        self.push_byte(b' ')?;
        self.begin_list(body.spans.len(), cd)?;
        for (i, &sp) in body.spans.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_opt_span(sp, cd + 1)?;
        }
        self.push_str(") ")?;
        self.begin_list(body.params.len(), cd)?;
        for (i, p) in body.params.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_opt_string(p.as_deref(), cd + 1)?;
        }
        self.push_str(") ")?;
        self.begin_list(body.bindings.len(), cd)?;
        for (i, b) in body.bindings.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_tagged("BindingMeta", cd + 1)?;
            self.push_byte(b' ')?;
            self.write_u32(b.node)?;
            self.push_byte(b' ')?;
            self.write_u32(b.slot)?;
            self.push_byte(b' ')?;
            self.write_string(&b.name)?;
            self.push_byte(b')')?;
        }
        self.push_str(") ")?;
        self.begin_list(body.closures.len(), cd)?;
        for (i, c) in body.closures.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_untagged(cd + 1)?;
            self.write_u32(c.node)?;
            self.push_byte(b' ')?;
            self.write_string(&c.name)?;
            self.push_byte(b')')?;
        }
        self.push_str("))")
    }

    fn write_metadata(&mut self, meta: &Metadata, depth: usize) -> PrintRes {
        match meta {
            Metadata::NoSource => self.write_nullary("noSource", depth),
            Metadata::Source(m) => {
                if !m.file_bytes.is_empty()
                    && m.file_bytes
                        .iter()
                        .any(|raw| std::str::from_utf8(raw).is_err())
                {
                    return Err(CorePrintError::InvalidMetadata);
                }
                let cd = depth + 1;
                self.begin_tagged("Metadata", depth)?;
                self.push_byte(b' ')?;
                self.write_string_list(&m.files, cd)?;
                self.push_byte(b' ')?;

                self.begin_list(m.modules.len(), cd)?;
                for (i, mm) in m.modules.iter().enumerate() {
                    if i != 0 {
                        self.push_byte(b' ')?;
                    }
                    self.begin_tagged("ModuleMeta", cd + 1)?;
                    self.push_byte(b' ')?;
                    self.write_string(&mm.name)?;
                    self.push_byte(b' ')?;
                    self.write_u32_list(&mm.data, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_u32_list(&mm.funs, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_u32_list(&mm.consts, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.begin_list(mm.exposed_types.len(), cd + 2)?;
                    for (j, et) in mm.exposed_types.iter().enumerate() {
                        if j != 0 {
                            self.push_byte(b' ')?;
                        }
                        self.begin_untagged(cd + 3)?;
                        self.write_u32(et.index)?;
                        self.push_byte(b' ')?;
                        self.write_bool(et.contents_exposed)?;
                        self.push_byte(b')')?;
                    }
                    self.push_str(") ")?;
                    self.write_u32_list(&mm.exposed_funs, cd + 2)?;
                    self.push_byte(b')')?;
                }
                self.push_str(") ")?;

                self.begin_list(m.funs.len(), cd)?;
                for (i, fm) in m.funs.iter().enumerate() {
                    if i != 0 {
                        self.push_byte(b' ')?;
                    }
                    self.begin_tagged("FunMeta", cd + 1)?;
                    self.push_byte(b' ')?;
                    self.write_string(&fm.name)?;
                    self.push_byte(b' ')?;
                    self.write_opt_span(fm.span, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_opt_string(fm.text.as_deref(), cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_body_meta(&fm.body, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_opt_u32(fm.subject, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_opt_bool(fm.allows, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.begin_list(fm.lines.len(), cd + 2)?;
                    for (j, line) in fm.lines.iter().enumerate() {
                        if j != 0 {
                            self.push_byte(b' ')?;
                        }
                        self.begin_tagged("SpecLine", cd + 3)?;
                        self.push_byte(b' ')?;
                        self.write_opt_span(line.span, cd + 4)?;
                        self.push_byte(b' ')?;
                        self.write_string(&line.text)?;
                        self.push_byte(b')')?;
                    }
                    self.push_str("))")?;
                }
                self.push_str(") ")?;

                self.begin_list(m.consts.len(), cd)?;
                for (i, cm) in m.consts.iter().enumerate() {
                    if i != 0 {
                        self.push_byte(b' ')?;
                    }
                    self.begin_tagged("ConstMeta", cd + 1)?;
                    self.push_byte(b' ')?;
                    self.write_string(&cm.name)?;
                    self.push_byte(b' ')?;
                    self.write_opt_span(cm.span, cd + 2)?;
                    self.push_byte(b' ')?;
                    self.write_body_meta(&cm.body, cd + 2)?;
                    self.push_byte(b')')?;
                }
                self.push_str("))")
            }
        }
    }

    fn write_program(&mut self, p: &Program, depth: usize) -> PrintRes {
        let cd = depth + 1;
        self.begin_tagged("Program", depth)?;
        self.push_byte(b' ')?;

        self.begin_list(p.data.len(), cd)?;
        for (i, d) in p.data.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_tagged("DataDecl", cd + 1)?;
            self.push_byte(b' ')?;
            self.write_string(&d.name)?;
            self.push_byte(b' ')?;
            self.begin_list(d.ctors.len(), cd + 2)?;
            for (j, c) in d.ctors.iter().enumerate() {
                if j != 0 {
                    self.push_byte(b' ')?;
                }
                self.begin_tagged("Ctor", cd + 3)?;
                self.push_byte(b' ')?;
                self.write_string(&c.name)?;
                self.push_byte(b' ')?;
                self.write_params(&c.fields, cd + 4)?;
                self.push_byte(b')')?;
            }
            self.push_str("))")?;
        }
        self.push_str(") ")?;

        self.begin_list(p.funs.len(), cd)?;
        for (i, f) in p.funs.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_tagged("FunDecl", cd + 1)?;
            self.push_byte(b' ')?;
            self.write_string(&f.name)?;
            self.push_byte(b' ')?;
            self.write_params(&f.params, cd + 2)?;
            self.push_byte(b' ')?;
            self.write_ty(f.ret, cd + 2)?;
            self.push_byte(b' ')?;
            self.write_expr(f.body, cd + 2)?;
            self.push_byte(b' ')?;
            self.write_string(&f.module)?;
            self.push_byte(b' ')?;
            self.write_nullary(f.kind.wire_name(), cd + 2)?;
            self.push_byte(b' ')?;
            self.write_natural(&f.const_count)?;
            self.push_byte(b')')?;
        }
        self.push_str(") ")?;

        self.begin_list(p.consts.len(), cd)?;
        for (i, c) in p.consts.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.begin_tagged("ConstDecl", cd + 1)?;
            self.push_byte(b' ')?;
            self.write_string(&c.name)?;
            self.push_byte(b' ')?;
            self.write_ty(c.ty, cd + 2)?;
            self.push_byte(b' ')?;
            self.write_expr(c.body, cd + 2)?;
            self.push_byte(b')')?;
        }
        self.push_str(") ")?;

        self.write_string(&p.entry)?;
        self.push_byte(b' ')?;
        self.write_metadata(&p.metadata, cd)?;
        self.push_byte(b')')
    }

    /// Iterative Value printer (O(1) Rust call stack up to `MO_LIMIT_CORE_DEPTH`).
    fn write_value(&mut self, root: ValueId, root_depth: usize) -> PrintRes {
        enum Task<'a> {
            Val(ValueId, usize),
            OpenUntagged(usize),
            OpenList(usize, usize),
            BindingPrefix(&'a str, usize),
            ClosureMid(&'a [Param], ExprId, usize),
            Space,
            Close,
        }

        let mut stack = vec![Task::Val(root, root_depth)];
        while let Some(task) = stack.pop() {
            match task {
                Task::Space => self.push_byte(b' ')?,
                Task::Close => self.push_byte(b')')?,
                Task::OpenUntagged(depth) => self.begin_untagged(depth)?,
                Task::OpenList(len, depth) => self.begin_list(len, depth)?,
                Task::BindingPrefix(name, depth) => {
                    self.begin_untagged(depth)?;
                    self.write_string(name)?;
                    self.push_byte(b' ')?;
                }
                Task::ClosureMid(params, body, depth) => {
                    self.push_byte(b' ')?;
                    self.write_params(params, depth)?;
                    self.push_byte(b' ')?;
                    self.write_expr(body, depth)?;
                    self.push_byte(b' ')?;
                }
                Task::Val(id, depth) => {
                    let val = self
                        .arena
                        .value_opt(id)
                        .ok_or(CorePrintError::InvalidArenaRef)?;
                    let cd = depth + 1;
                    match val {
                        Value::Int(n) => {
                            self.begin_tagged("int", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(n)?;
                            self.push_byte(b')')?;
                        }
                        Value::BigInt(n) => {
                            self.begin_tagged("bigInt", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(n)?;
                            self.push_byte(b')')?;
                        }
                        Value::Byte(n) => {
                            self.begin_tagged("byte", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(n)?;
                            self.push_byte(b')')?;
                        }
                        Value::Decimal(n) => {
                            self.begin_tagged("decimal", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(n)?;
                            self.push_byte(b')')?;
                        }
                        Value::Float(bits) => {
                            self.begin_tagged("float", depth)?;
                            self.push_byte(b' ')?;
                            self.write_bits(*bits)?;
                            self.push_byte(b')')?;
                        }
                        Value::Bool(b) => {
                            self.begin_tagged("bool", depth)?;
                            self.push_byte(b' ')?;
                            self.write_bool(*b)?;
                            self.push_byte(b')')?;
                        }
                        Value::Char(ch) => {
                            self.begin_tagged("char", depth)?;
                            self.push_byte(b' ')?;
                            self.write_char(*ch)?;
                            self.push_byte(b')')?;
                        }
                        Value::Text(s) => {
                            self.begin_tagged("text", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(s)?;
                            self.push_byte(b')')?;
                        }
                        Value::Bytes(bs) => {
                            self.begin_tagged("bytes", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer_list(bs, cd)?;
                            self.push_byte(b')')?;
                        }
                        Value::Unit => {
                            self.begin_tagged("unit", depth)?;
                            self.push_byte(b')')?;
                        }
                        Value::Time(n) => {
                            self.begin_tagged("time", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(n)?;
                            self.push_byte(b')')?;
                        }
                        Value::Duration(n) => {
                            self.begin_tagged("duration", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(n)?;
                            self.push_byte(b')')?;
                        }
                        Value::Range { start, end } => {
                            self.begin_tagged("range", depth)?;
                            self.push_byte(b' ')?;
                            self.write_integer(start)?;
                            self.push_byte(b' ')?;
                            self.write_integer(end)?;
                            self.push_byte(b')')?;
                        }
                        Value::Tuple(elems) => {
                            self.begin_tagged("tuple", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(elems.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &e) in elems.iter().enumerate().rev() {
                                stack.push(Task::Val(e, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Value::List(elems) => {
                            self.begin_tagged("list", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(elems.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &e) in elems.iter().enumerate().rev() {
                                stack.push(Task::Val(e, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Value::Set(elems) => {
                            self.begin_tagged("set", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(elems.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &e) in elems.iter().enumerate().rev() {
                                stack.push(Task::Val(e, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Value::Map(pairs) => {
                            self.begin_tagged("map", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(pairs.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &(k, v)) in pairs.iter().enumerate().rev() {
                                stack.push(Task::Close);
                                stack.push(Task::Val(v, cd + 2));
                                stack.push(Task::Space);
                                stack.push(Task::Val(k, cd + 2));
                                stack.push(Task::OpenUntagged(cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Value::Con { ctor, fields } => {
                            self.begin_tagged("con", depth)?;
                            self.push_byte(b' ')?;
                            self.write_string(ctor)?;
                            self.push_byte(b' ')?;
                            self.begin_list(fields.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, &f) in fields.iter().enumerate().rev() {
                                stack.push(Task::Val(f, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Value::Closure {
                            env,
                            params,
                            body,
                            consts,
                        } => {
                            self.begin_tagged("closure", depth)?;
                            self.push_byte(b' ')?;
                            self.begin_list(env.len(), cd)?;
                            stack.push(Task::Close);
                            stack.push(Task::Close);
                            for (i, b) in consts.iter().enumerate().rev() {
                                stack.push(Task::Close);
                                stack.push(Task::Val(b.value, cd + 2));
                                stack.push(Task::BindingPrefix(&b.name, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                            stack.push(Task::OpenList(consts.len(), cd));
                            stack.push(Task::ClosureMid(params, *body, cd));
                            stack.push(Task::Close);
                            for (i, b) in env.iter().enumerate().rev() {
                                stack.push(Task::Close);
                                stack.push(Task::Val(b.value, cd + 2));
                                stack.push(Task::BindingPrefix(&b.name, cd + 1));
                                if i != 0 {
                                    stack.push(Task::Space);
                                }
                            }
                        }
                        Value::Cap { cap, root, fake } => {
                            self.begin_tagged("cap", depth)?;
                            self.push_byte(b' ')?;
                            self.write_cap(*cap, cd)?;
                            self.push_byte(b' ')?;
                            self.write_opt_string(root.as_deref(), cd)?;
                            self.push_byte(b' ')?;
                            self.write_opt_natural(fake.as_ref(), cd)?;
                            self.push_byte(b')')?;
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn write_values(&mut self, list: &[ValueId], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, &v) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_value(v, depth + 1)?;
        }
        self.push_byte(b')')
    }

    fn write_random_state(&mut self, r: &RandomState, depth: usize) -> PrintRes {
        self.begin_tagged("RandomState", depth)?;
        self.push_byte(b' ')?;
        self.write_bits(r.s0)?;
        self.push_byte(b' ')?;
        self.write_bits(r.s1)?;
        self.push_byte(b' ')?;
        self.write_bits(r.s2)?;
        self.push_byte(b' ')?;
        self.write_bits(r.s3)?;
        self.push_byte(b')')
    }

    fn write_cap_state(&mut self, s: &CapState, depth: usize) -> PrintRes {
        let cd = depth + 1;
        self.begin_tagged("CapState", depth)?;
        self.push_byte(b' ')?;
        self.write_string_list(&s.args, cd)?;
        self.push_byte(b' ')?;
        self.write_string_pairs(&s.env, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&s.stdin, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&s.stdout, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&s.stderr, cd)?;
        self.push_byte(b' ')?;
        self.write_string_pairs(&s.files, cd)?;
        self.push_byte(b' ')?;
        self.write_integer(&s.now)?;
        self.push_byte(b' ')?;
        self.write_random_state(&s.random, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&s.folders, cd)?;
        self.push_byte(b')')
    }

    fn write_cap_states(&mut self, list: &[CapState], depth: usize) -> PrintRes {
        self.begin_list(list.len(), depth)?;
        for (i, s) in list.iter().enumerate() {
            if i != 0 {
                self.push_byte(b' ')?;
            }
            self.write_cap_state(s, depth + 1)?;
        }
        self.push_byte(b')')
    }

    fn write_world(&mut self, w: &World, depth: usize) -> PrintRes {
        let cd = depth + 1;
        self.begin_tagged("World", depth)?;
        self.push_byte(b' ')?;
        self.write_string_list(&w.args, cd)?;
        self.push_byte(b' ')?;
        self.write_string_pairs(&w.env, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&w.stdin, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&w.stdout, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&w.stderr, cd)?;
        self.push_byte(b' ')?;
        self.write_string_pairs(&w.files, cd)?;
        self.push_byte(b' ')?;
        self.write_integer(&w.now)?;
        self.push_byte(b' ')?;
        self.write_random_state(&w.random, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&w.folders, cd)?;
        self.push_byte(b' ')?;
        self.write_cap_states(&w.fakes, cd)?;
        self.push_byte(b')')
    }

    fn write_bug(&mut self, bug: &Bug, depth: usize) -> PrintRes {
        match bug {
            Bug::Overflow(d) => {
                self.begin_tagged("overflow", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Bug::DivByZero => self.begin_tagged("divByZero", depth)?,
            Bug::Stop(d) => {
                self.begin_tagged("stop", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Bug::Outside(d) => {
                self.begin_tagged("outside", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Bug::RepeatedKey => self.begin_tagged("repeatedKey", depth)?,
            Bug::FileFolderConflict(d) => {
                self.begin_tagged("fileFolderConflict", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Bug::FilePathInvalid(d) => {
                self.begin_tagged("filePathInvalid", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
        }
        self.push_byte(b')')
    }

    fn write_outcome(&mut self, o: &Outcome, depth: usize) -> PrintRes {
        let cd = depth + 1;
        match o {
            Outcome::Ok { value, world } => {
                self.begin_tagged("ok", depth)?;
                self.push_byte(b' ')?;
                self.write_value(*value, cd)?;
                self.push_byte(b' ')?;
                self.write_world(world, cd)?;
            }
            Outcome::Stopped { bug, world } => {
                self.begin_tagged("stopped", depth)?;
                self.push_byte(b' ')?;
                self.write_bug(bug, cd)?;
                self.push_byte(b' ')?;
                self.write_world(world, cd)?;
            }
            Outcome::Failed { detail, world } => {
                self.begin_tagged("failed", depth)?;
                self.push_byte(b' ')?;
                self.write_string(detail)?;
                self.push_byte(b' ')?;
                self.write_world(world, cd)?;
            }
            Outcome::Jumped {
                name,
                values,
                world,
            } => {
                self.begin_tagged("jumped", depth)?;
                self.push_byte(b' ')?;
                self.write_string(name)?;
                self.push_byte(b' ')?;
                self.write_values(values, cd)?;
                self.push_byte(b' ')?;
                self.write_world(world, cd)?;
            }
            Outcome::Wrong(d) => {
                self.begin_tagged("wrong", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Outcome::OutOfFuel => {
                self.begin_tagged("outOfFuel", depth)?;
            }
        }
        self.push_byte(b')')
    }

    fn write_effects(&mut self, e: &Effects, depth: usize) -> PrintRes {
        let cd = depth + 1;
        self.begin_tagged("effects", depth)?;
        self.push_byte(b' ')?;
        self.write_string_list(&e.stdout, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&e.stderr, cd)?;
        self.push_byte(b' ')?;
        self.write_string_pairs(&e.files, cd)?;
        self.push_byte(b' ')?;
        self.write_string_list(&e.folders, cd)?;
        self.push_byte(b' ')?;
        self.write_cap_states(&e.fakes, cd)?;
        self.push_byte(b')')
    }

    fn write_report(&mut self, r: &Report, depth: usize) -> PrintRes {
        let cd = depth + 1;
        match r {
            Report::Returned { value, effects } => {
                self.begin_tagged("returned", depth)?;
                self.push_byte(b' ')?;
                self.write_string(value)?;
                self.push_byte(b' ')?;
                self.write_effects(effects, cd)?;
            }
            Report::Stopped { bug, effects } => {
                self.begin_tagged("stopped", depth)?;
                self.push_byte(b' ')?;
                self.write_bug(bug, cd)?;
                self.push_byte(b' ')?;
                self.write_effects(effects, cd)?;
            }
            Report::Failed { detail, effects } => {
                self.begin_tagged("failed", depth)?;
                self.push_byte(b' ')?;
                self.write_string(detail)?;
                self.push_byte(b' ')?;
                self.write_effects(effects, cd)?;
            }
            Report::OutOfFuel => self.begin_tagged("outOfFuel", depth)?,
            Report::Wrong(d) => {
                self.begin_tagged("wrong", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Report::ParseError(d) => {
                self.begin_tagged("parseError", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Report::IllTyped(d) => {
                self.begin_tagged("illTyped", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Report::InvalidRun(d) => {
                self.begin_tagged("invalidRun", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
            Report::IoError(d) => {
                self.begin_tagged("ioError", depth)?;
                self.push_byte(b' ')?;
                self.write_string(d)?;
            }
        }
        self.push_byte(b')')
    }

    fn write_selection(&mut self, sel: &Selection, depth: usize) -> PrintRes {
        match sel {
            Selection::Entry(args) => {
                self.begin_tagged("entry", depth)?;
                self.push_byte(b' ')?;
                self.write_values(args, depth + 1)?;
            }
            Selection::Test(name) => {
                self.begin_tagged("test", depth)?;
                self.push_byte(b' ')?;
                self.write_string(name)?;
            }
        }
        self.push_byte(b')')
    }

    fn write_run(&mut self, run: &Run, depth: usize) -> PrintRes {
        let cd = depth + 1;
        self.begin_tagged("Run", depth)?;
        self.push_byte(b' ')?;
        self.write_program(&run.program, cd)?;
        self.push_byte(b' ')?;
        self.write_world(&run.world, cd)?;
        self.push_byte(b' ')?;
        self.write_selection(&run.selection, cd)?;
        self.push_byte(b' ')?;
        self.write_natural(&run.fuel)?;
        self.push_byte(b')')
    }

    fn write_document(&mut self, doc: &CoreDocument) -> PrintRes {
        self.push_str("(core ")?;
        self.push_str(MO_CORE_TEXT_VERSION)?;
        self.push_byte(b' ')?;
        let depth = 2;
        match doc {
            CoreDocument::Type(ty) => self.write_ty(*ty, depth)?,
            CoreDocument::Cap(cap) => self.write_cap(*cap, depth)?,
            CoreDocument::Literal(lit) => self.write_lit(lit, depth)?,
            CoreDocument::Primitive(prim) => self.write_prim(prim, depth)?,
            CoreDocument::Expr(expr) => self.write_expr(*expr, depth)?,
            CoreDocument::Program(prog) => self.write_program(prog, depth)?,
            CoreDocument::Value(val) => self.write_value(*val, depth)?,
            CoreDocument::World(w) => self.write_world(w, depth)?,
            CoreDocument::Bug(b) => self.write_bug(b, depth)?,
            CoreDocument::Outcome(o) => self.write_outcome(o, depth)?,
            CoreDocument::Effects(e) => self.write_effects(e, depth)?,
            CoreDocument::Report(r) => self.write_report(r, depth)?,
            CoreDocument::Selection(s) => self.write_selection(s, depth)?,
            CoreDocument::Run(run) => self.write_run(run, depth)?,
            CoreDocument::CapState(cs) => self.write_cap_state(cs, depth)?,
            CoreDocument::RandomState(rs) => self.write_random_state(rs, depth)?,
        }
        self.push_str(")\n")?;
        match doc {
            CoreDocument::Program(prog) if !metadata_valid(self.arena, prog) => {
                return Err(CorePrintError::InvalidMetadata);
            }
            CoreDocument::Run(run) if !metadata_valid(self.arena, &run.program) => {
                return Err(CorePrintError::InvalidMetadata);
            }
            _ => {}
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

fn record_error(diags: &mut DiagBag, err: ParseErr) {
    let diag = Diagnostic::new(err.code, Span::new(0, err.start, err.end), err.message);
    let _ = diags.push(diag);
}

/// Parses a `.core` document of explicit category `kind`, recording at most one
/// diagnostic into `diags` on failure (exact `src/core/text.c:mo_core_text_parse` parity).
/// Parses any of the 16 Core document kinds from UTF-8 bytes, recording at most one
/// diagnostic into `diags` on failure (exact `src/core/text.c:mo_core_text_parse` parity).
pub fn parse_document(
    kind: CoreTextKind,
    bytes: &[u8],
    diags: &mut DiagBag,
) -> Option<ParsedCore<CoreDocument>> {
    parse_document_with_scratch_limit(kind, bytes, MO_LIMIT_CORE_SCRATCH_SIZE, diags)
}

/// Parses a Core document while enforcing an explicit scratch byte ceiling (`E1017`).
pub fn parse_document_with_scratch_limit(
    kind: CoreTextKind,
    bytes: &[u8],
    scratch_limit: u64,
    diags: &mut DiagBag,
) -> Option<ParsedCore<CoreDocument>> {
    if (bytes.len() as u64) > MO_LIMIT_CORE_TEXT_SIZE {
        record_error(
            diags,
            err_at(
                ErrorCode::CoreTextLimit,
                0,
                0,
                "Core text file is too large",
            ),
        );
        return None;
    }
    let input = match std::str::from_utf8(bytes) {
        Ok(s) => s,
        Err(e) => {
            let start = e.valid_up_to();
            record_error(
                diags,
                err_at(
                    ErrorCode::CoreUtf8,
                    start,
                    start + 1,
                    "input is not valid UTF-8",
                ),
            );
            return None;
        }
    };

    let (pool, root_id) = match lex(input) {
        Ok(v) => v,
        Err(err) => {
            record_error(diags, err);
            return None;
        }
    };

    let lex_scratch = (pool.nodes.len() as u64)
        .saturating_mul(std::mem::size_of::<Sexp>() as u64)
        .saturating_add(input.len() as u64);
    if lex_scratch > scratch_limit {
        record_error(
            diags,
            err_at(
                ErrorCode::CoreNoMemory,
                0,
                0,
                "Core parser exceeded its scratch memory limit",
            ),
        );
        return None;
    }

    let root_children = match &pool.get(root_id).kind {
        SexpKind::Node(cs) if cs.len() == 3 && pool.atom_is(cs[0], "core") => cs,
        _ => {
            record_error(
                diags,
                pool.err_node(
                    root_id,
                    ErrorCode::CoreSyntax,
                    "expected (core version payload)",
                ),
            );
            return None;
        }
    };

    if !pool.atom_is(root_children[1], MO_CORE_TEXT_VERSION) {
        record_error(
            diags,
            pool.err_node(
                root_children[1],
                ErrorCode::CoreVersion,
                "unsupported Core text version",
            ),
        );
        return None;
    }

    let payload_id = root_children[2];
    let mut decoder = Decoder::new(&pool);
    let doc = match decoder.decode_document(kind, payload_id) {
        Ok(d) => d,
        Err(err) => {
            record_error(diags, err);
            return None;
        }
    };

    let arena_scratch = (decoder.arena.expr_count() as u64)
        .saturating_mul(std::mem::size_of::<Expr>() as u64)
        .saturating_add(
            (decoder.arena.ty_count() as u64).saturating_mul(std::mem::size_of::<Ty>() as u64),
        );
    if lex_scratch.saturating_add(arena_scratch) > scratch_limit {
        record_error(
            diags,
            err_at(
                ErrorCode::CoreNoMemory,
                0,
                0,
                "Core parser exceeded its scratch memory limit",
            ),
        );
        return None;
    }

    let meta_ok = match &doc {
        CoreDocument::Program(p) => metadata_valid(&decoder.arena, p),
        CoreDocument::Run(r) => metadata_valid(&decoder.arena, &r.program),
        _ => true,
    };
    if !meta_ok {
        record_error(
            diags,
            pool.err_node(
                payload_id,
                ErrorCode::CoreField,
                "invalid compiler metadata shape",
            ),
        );
        return None;
    }

    if print_document(&decoder.arena, &doc).is_err() {
        record_error(
            diags,
            pool.err_node(
                payload_id,
                ErrorCode::CoreTextLimit,
                "canonical Core text exceeds a limit",
            ),
        );
        return None;
    }

    Some(ParsedCore {
        arena: decoder.arena,
        value: doc,
    })
}

/// Parses a `Program` document.
pub fn parse_program(bytes: &[u8], diags: &mut DiagBag) -> Option<ParsedCore<Program>> {
    let parsed = parse_document(CoreTextKind::Program, bytes, diags)?;
    match parsed.value {
        CoreDocument::Program(program) => Some(ParsedCore {
            arena: parsed.arena,
            value: program,
        }),
        _ => None,
    }
}

/// Parses a `Run` document.
pub fn parse_run(bytes: &[u8], diags: &mut DiagBag) -> Option<ParsedCore<Run>> {
    let parsed = parse_document(CoreTextKind::Run, bytes, diags)?;
    match parsed.value {
        CoreDocument::Run(run) => Some(ParsedCore {
            arena: parsed.arena,
            value: run,
        }),
        _ => None,
    }
}

/// Canonically prints any of the 16 Core document kinds to UTF-8 bytes including trailing `\n`.
pub fn print_document(arena: &CoreArena, doc: &CoreDocument) -> Result<String, CorePrintError> {
    let mut printer = Printer::new(arena);
    printer.write_document(doc)?;
    String::from_utf8(printer.out).map_err(|_| CorePrintError::LimitExceeded)
}

/// Canonically prints a `Program` document.
pub fn print_program(arena: &CoreArena, program: &Program) -> Result<String, CorePrintError> {
    let mut printer = Printer::new(arena);
    printer.push_str("(core ")?;
    printer.push_str(MO_CORE_TEXT_VERSION)?;
    printer.push_byte(b' ')?;
    printer.write_program(program, 2)?;
    printer.push_str(")\n")?;
    if !metadata_valid(arena, program) {
        return Err(CorePrintError::InvalidMetadata);
    }
    String::from_utf8(printer.out).map_err(|_| CorePrintError::LimitExceeded)
}

/// Canonically prints a `Run` document.
pub fn print_run(arena: &CoreArena, run: &Run) -> Result<String, CorePrintError> {
    let mut printer = Printer::new(arena);
    printer.push_str("(core ")?;
    printer.push_str(MO_CORE_TEXT_VERSION)?;
    printer.push_byte(b' ')?;
    printer.write_run(run, 2)?;
    printer.push_str(")\n")?;
    if !metadata_valid(arena, &run.program) {
        return Err(CorePrintError::InvalidMetadata);
    }
    String::from_utf8(printer.out).map_err(|_| CorePrintError::LimitExceeded)
}

/// Best-effort CLI helper for `--kind auto` that inspects the top-level payload
/// constructor of a `.core` document. Never replaces explicit category parsing
/// in library code.
#[must_use]
pub fn detect_kind(bytes: &[u8]) -> Option<CoreTextKind> {
    if (bytes.len() as u64) > MO_LIMIT_CORE_TEXT_SIZE {
        return None;
    }
    let input = std::str::from_utf8(bytes).ok()?;
    let (pool, root_id) = lex(input).ok()?;
    let SexpKind::Node(root_cs) = &pool.get(root_id).kind else {
        return None;
    };
    if root_cs.len() != 3 || !pool.atom_is(root_cs[0], "core") {
        return None;
    }
    let payload_id = root_cs[2];
    let SexpKind::Node(payload_cs) = &pool.get(payload_id).kind else {
        return None;
    };
    let head = pool.atom_str(*payload_cs.first()?)?;
    let arity = payload_cs.len() - 1;
    match head {
        "Run" => Some(CoreTextKind::Run),
        "Program" => Some(CoreTextKind::Program),
        "World" => Some(CoreTextKind::World),
        "CapState" => Some(CoreTextKind::CapState),
        "RandomState" => Some(CoreTextKind::RandomState),
        "effects" => Some(CoreTextKind::Effects),
        "returned" | "parseError" | "illTyped" | "invalidRun" | "ioError" => {
            Some(CoreTextKind::Report)
        }
        "ok" | "jumped" => Some(CoreTextKind::Outcome),
        "stopped" | "failed" => {
            // Disambiguate Outcome (second arg is World) vs Report (second arg is effects).
            let second = payload_cs.get(2)?;
            if let SexpKind::Node(sc) = &pool.get(*second).kind
                && let Some(&sh) = sc.first()
                && pool.atom_is(sh, "effects")
            {
                Some(CoreTextKind::Report)
            } else {
                Some(CoreTextKind::Outcome)
            }
        }
        "overflow" | "divByZero" | "outside" | "repeatedKey" | "fileFolderConflict"
        | "filePathInvalid" => Some(CoreTextKind::Bug),
        "entry" | "test" => Some(CoreTextKind::Selection),
        "io" | "unicode16" | "crypto" | "sim" | "big" | "dec" | "not" | "cmp" | "eq"
        | "structural" => Some(CoreTextKind::Primitive),
        "lit" | "var" | "global" | "let_" | "lam" | "app" | "call" | "proj" | "case_" | "if_"
        | "prim" | "fail" | "join" | "jump" => Some(CoreTextKind::Expr),
        "console" | "files" | "readFiles" | "clock" | "random" | "args" | "env" | "network"
        | "connection" | "sqlite" | "database" | "command" | "secureRandom" | "file" => {
            Some(CoreTextKind::Cap)
        }
        "closure" => Some(CoreTextKind::Value),
        "ordering" | "fileError" | "entryKind" | "simulation" | "simulationError"
        | "networkError" | "sqliteValue" | "sqliteRows" | "sqliteError" | "data" | "option"
        | "result" | "fn" => Some(CoreTextKind::Type),
        "list" | "map" | "set" => {
            // Primitive has 2 (list/set) or 3 (map) args; Type has 1 (list/set) or 2 (map) args
            // with Ty children; Value has 1 arg (list of Values or Value pairs).
            if (head == "list" && arity == 2)
                || (head == "set" && arity == 2)
                || (head == "map" && arity == 3)
            {
                Some(CoreTextKind::Primitive)
            } else if head == "map" && arity == 2 {
                Some(CoreTextKind::Type)
            } else {
                let first = payload_cs.get(1)?;
                if let SexpKind::Node(fc) = &pool.get(*first).kind
                    && let Some(&fh) = fc.first()
                    && pool.atom_str(fh).is_some()
                {
                    Some(CoreTextKind::Type)
                } else {
                    Some(CoreTextKind::Value)
                }
            }
        }
        "tuple" => {
            // Type: (tuple (<ty>...)), Value: (tuple (<val>...)), Expr: (tuple (<expr>...)).
            let mut d = DiagBag::new();
            if parse_document(CoreTextKind::Type, bytes, &mut d).is_some() {
                Some(CoreTextKind::Type)
            } else {
                let mut d2 = DiagBag::new();
                if parse_document(CoreTextKind::Value, bytes, &mut d2).is_some() {
                    Some(CoreTextKind::Value)
                } else {
                    Some(CoreTextKind::Expr)
                }
            }
        }
        "int" | "bigInt" | "byte" | "decimal" | "float" | "bool" | "char" | "text" | "bytes"
        | "unit" | "time" | "duration" | "range" => {
            if arity == 0 {
                if head == "unit" {
                    Some(CoreTextKind::Literal)
                } else {
                    Some(CoreTextKind::Type)
                }
            } else if head == "range" && arity == 2 {
                Some(CoreTextKind::Value)
            } else if arity == 1 {
                let arg = payload_cs[1];
                if matches!(pool.get(arg).kind, SexpKind::Node(_)) && head != "bytes" {
                    Some(CoreTextKind::Primitive)
                } else {
                    Some(CoreTextKind::Literal)
                }
            } else {
                None
            }
        }
        "con" => {
            if arity == 3 {
                Some(CoreTextKind::Expr)
            } else {
                Some(CoreTextKind::Value)
            }
        }
        "cap" => {
            if arity == 1 {
                Some(CoreTextKind::Type)
            } else {
                Some(CoreTextKind::Value)
            }
        }
        "stop" => Some(CoreTextKind::Expr),
        "outOfFuel" | "wrong" => Some(CoreTextKind::Outcome),
        _ => None,
    }
}
