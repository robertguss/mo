//! Exact scalar literal decoding, text escape & triple-quote indentation handling,
//! unary/binary operator primitives, and `Text` ability calls (`src/lower/scalar.c` parity).

use mo_check::{AbilityImplementationKind, ModuleId, ProofId, SourceAbility};
use mo_core::{CoreInteger, Expr, ExprId, Lit, Prim, Ty, TyId};
use mo_def::ErrorCode;
use mo_def::core_ops::{
    CoreBase, CoreBigOp, CoreByteOp, CoreCharOp, CoreCmpOp, CoreDecOp, CoreFloatOp, CoreIntOp,
    CoreRangeOp, CoreTimeOp,
};
use mo_syntax::{NodeId, SyntaxKind, TokenKind};

use crate::abilities::lower_derived_text;
use crate::context::{LowerResult, Lowering, SubstId};
use crate::types::{lower_customizer, lower_proof, lower_type};

const fn digit_value(c: u8) -> u32 {
    if c >= b'a' {
        (c - b'a') as u32 + 10
    } else if c >= b'A' {
        (c - b'A') as u32 + 10
    } else {
        (c - b'0') as u32
    }
}

/// Exact base-2 / base-10 / base-16 integer and fixed-scale 18-digit decimal parser.
/// Preserves the exact `lower_work(p, used)` charging loop from `src/lower/scalar.c:number`
/// so huge BigInt literals hit `E1152` identically to C.
fn decode_number(
    p: &mut Lowering<'_>,
    bytes: &[u8],
    is_decimal: bool,
    mut negative: bool,
) -> LowerResult<CoreInteger> {
    let length = bytes.len();
    let base: u32 = if length > 2 && bytes[1] == b'x' {
        16
    } else if length > 2 && bytes[1] == b'b' {
        2
    } else {
        10
    };
    let cap = 2 * length + 20;
    p.charge_alloc(cap, 1, 1)?;
    let mut digits = vec![0u8; cap];
    let mut used = 1usize;
    let mut places = 0usize;
    let mut fraction = false;
    let start = if base == 10 { 0 } else { 2 };

    for &c in &bytes[start..] {
        if c == b'_' {
            continue;
        }
        if c == b'.' {
            fraction = true;
            continue;
        }
        let mut carry = digit_value(c);
        debug_assert!(carry < base);
        p.work(used)?;
        for d in &mut digits[..used] {
            let n = u32::from(*d) * base + carry;
            *d = (n % 10) as u8;
            carry = n / 10;
        }
        while carry != 0 {
            debug_assert!(used < cap);
            digits[used] = (carry % 10) as u8;
            used += 1;
            carry /= 10;
        }
        if fraction {
            places += 1;
        }
    }

    let mut zeros = if is_decimal {
        18usize.saturating_sub(places)
    } else {
        0
    };
    if used == 1 && digits[0] == 0 {
        negative = false;
        zeros = 0;
    }
    p.charge_alloc(used + zeros + 1, 1, 1)?;
    let mut text = String::with_capacity(used + zeros);
    for i in 0..used {
        text.push(char::from(b'0' + digits[used - i - 1]));
    }
    for _ in 0..zeros {
        text.push('0');
    }
    Ok(CoreInteger {
        digits: text,
        negative,
    })
}

fn decoded_slice(
    p: &mut Lowering<'_>,
    bytes: &[u8],
    begin: usize,
    end: usize,
    indent: usize,
    triple: bool,
    mut line_start: bool,
) -> LowerResult<String> {
    debug_assert!(begin <= end && end <= bytes.len());
    p.charge_alloc(end - begin + 1, 1, 1)?;
    let mut out = Vec::with_capacity(end - begin);
    let mut i = begin;
    while i < end {
        p.work(1)?;
        if line_start {
            let mut spaces = 0usize;
            while i < end && spaces < indent && bytes[i] == b' ' {
                i += 1;
                spaces += 1;
            }
            line_start = false;
            if i == end {
                break;
            }
        }
        let mut c = bytes[i];
        if triple && c == b'\r' {
            debug_assert!(i + 1 < end && bytes[i + 1] == b'\n');
            i += 1;
            c = b'\n';
        }
        if triple && c == b'\n' {
            line_start = true;
        }
        if c != b'\\' {
            out.push(c);
            i += 1;
            continue;
        }
        i += 1;
        debug_assert!(i < end);
        let esc = bytes[i];
        if esc == b'u' {
            i += 1;
            debug_assert!(i < end && bytes[i] == b'{');
            let mut scalar = 0u32;
            loop {
                i += 1;
                if i >= end || bytes[i] == b'}' {
                    break;
                }
                scalar = scalar * 16 + digit_value(bytes[i]);
            }
            if let Some(ch) = char::from_u32(scalar) {
                let mut buf = [0u8; 4];
                out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
            }
        } else {
            let mapped = match esc {
                b'n' => b'\n',
                b't' => b'\t',
                b'r' => b'\r',
                other => other,
            };
            out.push(mapped);
        }
        i += 1;
    }
    Ok(String::from_utf8_lossy(&out).into_owned())
}

pub(crate) fn decode_quoted_text(p: &mut Lowering<'_>, bytes: &[u8]) -> LowerResult<String> {
    let length = bytes.len();
    let quote = if length >= 6 && bytes.starts_with(b"\"\"\"") {
        3
    } else {
        1
    };
    debug_assert!(length >= 2 * quote);
    let mut begin = quote;
    let mut end = length - quote;
    let mut indent = 0usize;
    if quote == 3 {
        while begin < end && bytes[begin] != b'\n' {
            begin += 1;
        }
        debug_assert!(begin < end);
        begin += 1;
        let mut closing = end;
        while closing > begin && bytes[closing - 1] == b' ' {
            closing -= 1;
        }
        indent = end - closing;
        end = if closing > begin { closing - 1 } else { begin };
        if end > begin && bytes[end - 1] == b'\r' {
            end -= 1;
        }
    }
    decoded_slice(p, bytes, begin, end, indent, quote == 3, quote == 3)
}

/// Lowers one even-indexed literal segment of an interpolated text expression.
pub(crate) fn lower_text_part(
    p: &mut Lowering<'_>,
    module: ModuleId,
    owner: NodeId,
    part: usize,
) -> LowerResult<ExprId> {
    let (s_nid, head_nid, tail_nid, count) = {
        let node = p.syntax_node(module, owner);
        let count = node.children.len();
        debug_assert!(count >= 3 && part < count && part.is_multiple_of(2));
        let Some(s_nid) = node.children[part] else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "missing text interpolation part",
            );
        };
        let Some(head_nid) = node.children[0] else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "missing text interpolation head",
            );
        };
        let Some(tail_nid) = node.children[count - 1] else {
            return p.refuse(
                ErrorCode::LowerUnfinished,
                "missing text interpolation tail",
            );
        };
        (s_nid, head_nid, tail_nid, count)
    };
    let s_span = p.syntax_node(module, s_nid).span;
    let head_bytes = p.syntax_bytes(module, head_nid).to_vec();
    let tail_bytes = p.syntax_bytes(module, tail_nid).to_vec();
    let s_bytes = p.syntax_bytes(module, s_nid).to_vec();

    let triple = head_bytes.len() >= 3 && head_bytes[1] == b'"' && head_bytes[2] == b'"';
    let quote = if triple { 3usize } else { 1usize };
    let mut begin = if part == 0 { quote } else { 1 };
    let mut end = s_bytes.len() - if part + 1 == count { quote } else { 2 };
    let mut indent = 0usize;
    if triple {
        let mut closing = tail_bytes.len() - quote;
        while closing != 0 && tail_bytes[closing - 1] == b' ' {
            closing -= 1;
            indent += 1;
        }
        if part == 0 {
            while begin < end && s_bytes[begin] != b'\n' {
                begin += 1;
            }
            debug_assert!(begin < end);
            begin += 1;
        }
        if part + 1 == count {
            debug_assert!(closing > begin && tail_bytes[closing - 1] == b'\n');
            end = closing - 1;
            if end > begin && tail_bytes[end - 1] == b'\r' {
                end -= 1;
            }
        }
    }
    p.span = s_span;
    p.charge_alloc(1, 40, 8)?;
    let decoded = decoded_slice(p, &s_bytes, begin, end, indent, triple, triple && part == 0)?;
    p.alloc_expr(Expr::Lit(Lit::Text(decoded)))
}

/// Lowers any literal syntax node (with optional folded unary `-`) to a Core `Lit` expression.
pub(crate) fn lower_literal(
    p: &mut Lowering<'_>,
    module: ModuleId,
    mut node_id: NodeId,
    ty: TyId,
    mut negative: bool,
) -> LowerResult<ExprId> {
    if p.syntax_node(module, node_id).kind == SyntaxKind::Negate {
        negative = !negative;
        let Some(inner) = p
            .syntax_node(module, node_id)
            .children
            .first()
            .copied()
            .flatten()
        else {
            return p.refuse(ErrorCode::LowerUnfinished, "missing negated literal");
        };
        node_id = inner;
    }
    let kind = p.syntax_node(module, node_id).kind;
    let bytes = p.syntax_bytes(module, node_id).to_vec();
    p.charge_alloc(1, 40, 8)?;
    let lit = match p.core_ty(ty) {
        Ty::Bool => Lit::Bool(kind == SyntaxKind::True),
        Ty::Text => Lit::Text(decode_quoted_text(p, &bytes)?),
        Ty::Char => {
            let decoded = decode_quoted_text(p, &bytes)?;
            let ch = decoded.chars().next().unwrap_or('\0');
            Lit::Char(ch)
        }
        Ty::Unit => Lit::Unit,
        Ty::Float => {
            p.charge_alloc(bytes.len() * 2 + 24, 1, 1)?;
            let mut text = String::with_capacity(bytes.len() + 2);
            if negative {
                text.push('-');
            }
            if bytes.len() > 2 && (bytes[1] == b'b' || bytes[1] == b'x') {
                // Binary or hex integer literal typed as Float: convert exact integer to decimal
                // first so Rust's IEEE-754 nearest-even decimal parser matches C `strtod_l`.
                let n = decode_number(p, &bytes, false, false)?;
                text.push_str(&n.digits);
            } else {
                for &b in &bytes {
                    if b != b'_' {
                        text.push(char::from(b));
                    }
                }
            }
            let Ok(value) = text.parse::<f64>() else {
                return p.refuse(ErrorCode::LowerUnfinished, "invalid float literal");
            };
            Lit::Float(value.to_bits())
        }
        Ty::Int => Lit::Int(decode_number(p, &bytes, false, negative)?),
        Ty::BigInt => Lit::BigInt(decode_number(p, &bytes, false, negative)?),
        Ty::Byte => Lit::Byte(decode_number(p, &bytes, false, negative)?),
        Ty::Decimal => Lit::Decimal(decode_number(p, &bytes, true, negative)?),
        Ty::Range
        | Ty::Time
        | Ty::Duration
        | Ty::Bytes
        | Ty::Tuple(_)
        | Ty::List(_)
        | Ty::Map { .. }
        | Ty::Set(_)
        | Ty::Data(_)
        | Ty::Option(_)
        | Ty::Result { .. }
        | Ty::Ordering
        | Ty::FileError
        | Ty::Entry
        | Ty::EntryKind
        | Ty::Simulation
        | Ty::SimulationError
        | Ty::NetworkError
        | Ty::SqliteValue
        | Ty::SqliteRows
        | Ty::SqliteError
        | Ty::Fn { .. }
        | Ty::Cap(_) => {
            return p.refuse(ErrorCode::LowerUnfinished, "unsupported literal type");
        }
    };
    p.alloc_expr(Expr::Lit(lit))
}

/// Lowers a native unary/binary operator token to a Core [`Prim`].
pub(crate) fn lower_operator(
    p: &mut Lowering<'_>,
    operation: TokenKind,
    left: TyId,
    right: Option<TyId>,
) -> LowerResult<Prim> {
    if operation == TokenKind::Not {
        return Ok(Prim::Not);
    }
    if matches!(operation, TokenKind::Eq | TokenKind::Ne) {
        return Ok(Prim::Eq(left));
    }
    if matches!(
        operation,
        TokenKind::Lt | TokenKind::Le | TokenKind::Gt | TokenKind::Ge
    ) {
        let op = match operation {
            TokenKind::Lt => CoreCmpOp::Lt,
            TokenKind::Le => CoreCmpOp::Le,
            TokenKind::Gt => CoreCmpOp::Gt,
            _ => CoreCmpOp::Ge,
        };
        let base = match p.core_ty(left) {
            Ty::Int => CoreBase::Int,
            Ty::BigInt => CoreBase::BigInt,
            Ty::Byte => CoreBase::Byte,
            Ty::Decimal => CoreBase::Decimal,
            Ty::Float => CoreBase::Float,
            Ty::Bool => CoreBase::Bool,
            Ty::Char => CoreBase::Char,
            Ty::Text => CoreBase::Text,
            Ty::Bytes => CoreBase::Bytes,
            Ty::Time => CoreBase::Time,
            Ty::Duration => CoreBase::Duration,
            Ty::Unit
            | Ty::Range
            | Ty::Tuple(_)
            | Ty::List(_)
            | Ty::Map { .. }
            | Ty::Set(_)
            | Ty::Data(_)
            | Ty::Option(_)
            | Ty::Result { .. }
            | Ty::Ordering
            | Ty::FileError
            | Ty::Entry
            | Ty::EntryKind
            | Ty::Simulation
            | Ty::SimulationError
            | Ty::NetworkError
            | Ty::SqliteValue
            | Ty::SqliteRows
            | Ty::SqliteError
            | Ty::Fn { .. }
            | Ty::Cap(_) => {
                return p.refuse(
                    ErrorCode::LowerUnfinished,
                    "derived ordering is not lowered yet",
                );
            }
        };
        return Ok(Prim::Cmp { base, op });
    }
    if operation == TokenKind::DotDot {
        return Ok(Prim::Range(CoreRangeOp::Make));
    }

    let index = match operation {
        TokenKind::Plus | TokenKind::PlusEq => 0u8,
        TokenKind::Minus | TokenKind::MinusEq => {
            if right.is_none() {
                5u8
            } else {
                1u8
            }
        }
        TokenKind::Star => 2u8,
        TokenKind::Slash => 3u8,
        _ => 4u8,
    };

    let left_ty = p.core_ty(left).clone();
    match left_ty {
        Ty::Int => Ok(Prim::Int(match index {
            0 => CoreIntOp::Add,
            1 => CoreIntOp::Sub,
            2 => CoreIntOp::Mul,
            3 => CoreIntOp::Div,
            4 => CoreIntOp::Rem,
            _ => CoreIntOp::Neg,
        })),
        Ty::BigInt => Ok(Prim::Big(match index {
            0 => CoreBigOp::Add,
            1 => CoreBigOp::Sub,
            2 => CoreBigOp::Mul,
            3 => CoreBigOp::Div,
            4 => CoreBigOp::Rem,
            _ => CoreBigOp::Neg,
        })),
        Ty::Decimal => Ok(Prim::Dec(match index {
            0 => CoreDecOp::Add,
            1 => CoreDecOp::Sub,
            2 => CoreDecOp::Mul,
            3 => CoreDecOp::Div,
            4 => CoreDecOp::Rem,
            _ => CoreDecOp::Neg,
        })),
        Ty::Float => Ok(Prim::Float(match index {
            0 => CoreFloatOp::Add,
            1 => CoreFloatOp::Sub,
            2 => CoreFloatOp::Mul,
            3 => CoreFloatOp::Div,
            4 => CoreFloatOp::Rem,
            _ => CoreFloatOp::Neg,
        })),
        Ty::Time => {
            let right_is_time = right.is_some_and(|r| matches!(p.core_ty(r), Ty::Time));
            let op = if index == 0 {
                CoreTimeOp::Add
            } else if right_is_time {
                CoreTimeOp::Sub
            } else {
                CoreTimeOp::SubDur
            };
            Ok(Prim::Time(op))
        }
        Ty::Duration => {
            let op = match index {
                0 => CoreTimeOp::DurAdd,
                1 => CoreTimeOp::DurSub,
                2 => CoreTimeOp::DurMul,
                _ => CoreTimeOp::DurNeg,
            };
            Ok(Prim::Time(op))
        }
        Ty::Byte
        | Ty::Bool
        | Ty::Char
        | Ty::Text
        | Ty::Bytes
        | Ty::Unit
        | Ty::Range
        | Ty::Tuple(_)
        | Ty::List(_)
        | Ty::Map { .. }
        | Ty::Set(_)
        | Ty::Data(_)
        | Ty::Option(_)
        | Ty::Result { .. }
        | Ty::Ordering
        | Ty::FileError
        | Ty::Entry
        | Ty::EntryKind
        | Ty::Simulation
        | Ty::SimulationError
        | Ty::NetworkError
        | Ty::SqliteValue
        | Ty::SqliteRows
        | Ty::SqliteError
        | Ty::Fn { .. }
        | Ty::Cap(_) => p.refuse(
            ErrorCode::LowerUnfinished,
            "this numeric operation is not lowered yet",
        ),
    }
}

/// Lowers native `Text` conversion (`lower_error_text` in `src/lower/scalar.c`).
pub(crate) fn lower_error_text(
    p: &mut Lowering<'_>,
    ty: TyId,
    value: ExprId,
) -> LowerResult<ExprId> {
    let tag = p.core_ty(ty).clone();
    match tag {
        Ty::Text => Ok(value),
        Ty::Unit => p.expr_text("Unit"),
        Ty::Bool => {
            let t = p.expr_text("true")?;
            let f = p.expr_text("false")?;
            p.expr_if(value, t, f)
        }
        Ty::Int => p.expr_prim(Prim::Int(CoreIntOp::ToText), vec![value]),
        Ty::BigInt => p.expr_prim(Prim::Big(CoreBigOp::ToText), vec![value]),
        Ty::Byte => p.expr_prim(Prim::Byte(CoreByteOp::ToText), vec![value]),
        Ty::Decimal => p.expr_prim(Prim::Dec(CoreDecOp::ToText), vec![value]),
        Ty::Float => p.expr_prim(Prim::Float(CoreFloatOp::ToText), vec![value]),
        Ty::Char => p.expr_prim(Prim::Char(CoreCharOp::ToText), vec![value]),
        Ty::Range => p.expr_prim(Prim::Range(CoreRangeOp::ToText), vec![value]),
        Ty::Time => p.expr_prim(Prim::Time(CoreTimeOp::ToText), vec![value]),
        Ty::Duration => p.expr_prim(Prim::Time(CoreTimeOp::DurToText), vec![value]),
        Ty::Bytes => p.expr_prim(Prim::Structural(ty), vec![value]),
        Ty::Tuple(_)
        | Ty::List(_)
        | Ty::Map { .. }
        | Ty::Set(_)
        | Ty::Data(_)
        | Ty::Option(_)
        | Ty::Result { .. }
        | Ty::Ordering
        | Ty::FileError
        | Ty::Entry
        | Ty::EntryKind
        | Ty::Simulation
        | Ty::SimulationError
        | Ty::NetworkError
        | Ty::SqliteValue
        | Ty::SqliteRows
        | Ty::SqliteError
        | Ty::Fn { .. }
        | Ty::Cap(_) => p.refuse(
            ErrorCode::LowerUnfinished,
            "this error text implementation is not lowered yet",
        ),
    }
}

/// Lowers any `Text` ability proof (`Native`, `Derived`, `Function`, or `Parameter`).
pub(crate) fn lower_to_text(
    p: &mut Lowering<'_>,
    mut proof: ProofId,
    mut subst: Option<SubstId>,
    owner: ModuleId,
    depth: u32,
    value: ExprId,
) -> LowerResult<ExprId> {
    debug_assert_eq!(p.proof(proof).ability, SourceAbility::Text);
    lower_proof(p, &mut proof, &mut subst)?;
    let kind = p.proof(proof).kind;
    let proof_ty = p.proof(proof).ty;
    match kind {
        AbilityImplementationKind::Native => {
            let core_ty = lower_type(p, proof_ty, subst)?;
            lower_error_text(p, core_ty, value)
        }
        AbilityImplementationKind::Derived => {
            lower_derived_text(p, proof, subst, owner, depth, value)
        }
        AbilityImplementationKind::Function => {
            let inst = lower_customizer(p, proof, subst, owner, depth)?;
            let name = p.instances[inst].fun.name.clone();
            p.expr_call(name, vec![value])
        }
        AbilityImplementationKind::Parameter => p.refuse(
            ErrorCode::LowerUnfinished,
            "derived or abstract text implementation is not lowered yet",
        ),
    }
}
