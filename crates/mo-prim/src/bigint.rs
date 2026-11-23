//! All 15 `BIGINT_*` standard library primitives (`src/prim/bigint.c` parity).

use std::cmp::Ordering;
use std::collections::VecDeque;

use mo_bytecode::BigIntSign;
use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_heap::bigint::{BigInt, BigIntError, format_bigint_checked};
use mo_heap::{Heap, InlinePayload, ScratchGuard, Value};

use crate::{PrimError, check_value_bytes_fit};

pub(crate) fn sum_list(heap: &Heap, list: &VecDeque<Value>) -> Result<Value, PrimError> {
    if list.is_empty() {
        return Ok(heap.alloc_bigint(BigIntSign::Zero, Vec::new())?);
    }
    let mut acc_guard = ScratchGuard::new(heap.budget());
    let mut acc = BigInt::zero();
    for item in list {
        let mut step_guard = ScratchGuard::new(heap.budget());
        let next = read_arg(item, &mut step_guard)?;
        let sum = acc.add(&next, &mut step_guard).map_err(map_bigint_err)?;
        let sum_bytes = sum.limb_bytes();
        acc_guard.release(acc_guard.charged());
        drop(step_guard);
        acc_guard.charge(sum_bytes)?;
        acc = sum;
    }
    write_result(heap, &acc, &mut acc_guard)
}

fn map_bigint_err(err: BigIntError) -> PrimError {
    match err {
        BigIntError::DivByZero => PrimError::DivByZero("dividing by zero"),
        BigIntError::NegativeExponent => PrimError::Outside("BigInt.pow with a negative exponent"),
        BigIntError::ExceedsValueBytes => {
            PrimError::OutOfMemory(Some("BigInt exceeds MO_LIMIT_VALUE_BYTES"))
        }
        BigIntError::OutOfMemory => PrimError::OutOfMemory(None),
    }
}

fn compare_canonical(a: (BigIntSign, &[u8]), b: (BigIntSign, &[u8])) -> Ordering {
    let rank = |s: BigIntSign| -> i8 {
        match s {
            BigIntSign::Negative => -1,
            BigIntSign::Zero => 0,
            BigIntSign::Positive => 1,
        }
    };
    let sr = rank(a.0).cmp(&rank(b.0));
    if sr != Ordering::Equal {
        return sr;
    }
    if a.0 == BigIntSign::Zero {
        return Ordering::Equal;
    }
    let mag_ord =
        a.1.len()
            .cmp(&b.1.len())
            .then_with(|| a.1.iter().rev().cmp(b.1.iter().rev()));
    if a.0 == BigIntSign::Negative {
        mag_ord.reverse()
    } else {
        mag_ord
    }
}

fn read_arg(val: &Value, scratch: &mut ScratchGuard<'_>) -> Result<BigInt, PrimError> {
    let (sign, mag) = val.as_bigint();
    BigInt::from_canonical_bytes(sign, mag, scratch).map_err(map_bigint_err)
}

fn write_result(
    heap: &Heap,
    val: &BigInt,
    scratch: &mut ScratchGuard<'_>,
) -> Result<Value, PrimError> {
    let (sign, mag) = val.to_canonical_bytes(scratch).map_err(map_bigint_err)?;
    Ok(heap.alloc_bigint(sign, mag)?)
}

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    result_ty: u32,
    args: &mut [Value],
) -> Result<Value, PrimError> {
    match prim {
        PrimId::BigintToText => {
            let (sign, mag) = args[0].as_bigint();
            let mut out = String::new();
            format_bigint_checked(&mut out, sign, mag, Some(heap.budget()))?;
            check_value_bytes_fit(out.len())?;
            Ok(heap.alloc_text(out)?)
        }
        PrimId::BigintCompare => {
            let ord = compare_canonical(args[0].as_bigint(), args[1].as_bigint());
            let tag = match ord {
                Ordering::Less => 0,
                Ordering::Equal => 1,
                Ordering::Greater => 2,
            };
            Ok(Value::ChoiceInline {
                tag,
                payload: InlinePayload::Empty,
            })
        }
        PrimId::BigintMin => {
            let ord = compare_canonical(args[0].as_bigint(), args[1].as_bigint());
            Ok(if ord == Ordering::Greater {
                args[1].clone()
            } else {
                args[0].clone()
            })
        }
        PrimId::BigintMax => {
            let ord = compare_canonical(args[0].as_bigint(), args[1].as_bigint());
            Ok(if ord == Ordering::Less {
                args[1].clone()
            } else {
                args[0].clone()
            })
        }
        PrimId::BigintToInt => {
            let (sign, mag) = args[0].as_bigint();
            let opt = match sign {
                BigIntSign::Zero => Some(Value::Int(0)),
                BigIntSign::Positive | BigIntSign::Negative if mag.len() <= 8 => {
                    let mut val: u64 = 0;
                    for &b in mag.iter().rev() {
                        val = (val << 8) | u64::from(b);
                    }
                    let negative = sign == BigIntSign::Negative;
                    let limit = if negative {
                        1u64 << 63
                    } else {
                        i64::MAX as u64
                    };
                    if val <= limit {
                        let signed = if negative {
                            (0u64.wrapping_sub(val)) as i64
                        } else {
                            val as i64
                        };
                        Some(Value::Int(signed))
                    } else {
                        None
                    }
                }
                _ => None,
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::BigintNeg => {
            let (sign, mag) = args[0].as_bigint();
            let neg_sign = match sign {
                BigIntSign::Zero => BigIntSign::Zero,
                BigIntSign::Positive => BigIntSign::Negative,
                BigIntSign::Negative => BigIntSign::Positive,
            };
            Ok(heap.alloc_bigint(neg_sign, mag.to_vec())?)
        }
        PrimId::BigintAbs => {
            let (sign, mag) = args[0].as_bigint();
            let abs_sign = match sign {
                BigIntSign::Zero => BigIntSign::Zero,
                BigIntSign::Positive | BigIntSign::Negative => BigIntSign::Positive,
            };
            Ok(heap.alloc_bigint(abs_sign, mag.to_vec())?)
        }
        PrimId::BigintAdd => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let a = read_arg(&args[0], &mut scratch)?;
            let b = read_arg(&args[1], &mut scratch)?;
            let res = a.add(&b, &mut scratch).map_err(map_bigint_err)?;
            write_result(heap, &res, &mut scratch)
        }
        PrimId::BigintSub => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let a = read_arg(&args[0], &mut scratch)?;
            let b = read_arg(&args[1], &mut scratch)?;
            let res = a.sub(&b, &mut scratch).map_err(map_bigint_err)?;
            write_result(heap, &res, &mut scratch)
        }
        PrimId::BigintMul => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let a = read_arg(&args[0], &mut scratch)?;
            let b = read_arg(&args[1], &mut scratch)?;
            let res = a.mul(&b, &mut scratch).map_err(map_bigint_err)?;
            write_result(heap, &res, &mut scratch)
        }
        PrimId::BigintDiv | PrimId::BigintRem => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let a = read_arg(&args[0], &mut scratch)?;
            let b = read_arg(&args[1], &mut scratch)?;
            let (q, r) = a.div_rem_trunc(&b, &mut scratch).map_err(map_bigint_err)?;
            let chosen = if prim == PrimId::BigintDiv { &q } else { &r };
            write_result(heap, chosen, &mut scratch)
        }
        PrimId::BigintFloorDiv | PrimId::BigintFloorMod => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let a = read_arg(&args[0], &mut scratch)?;
            let b = read_arg(&args[1], &mut scratch)?;
            let (q, r) = a.div_rem_floor(&b, &mut scratch).map_err(map_bigint_err)?;
            let chosen = if prim == PrimId::BigintFloorDiv {
                &q
            } else {
                &r
            };
            write_result(heap, chosen, &mut scratch)
        }
        PrimId::BigintPow => {
            let exp = args[1].as_int();
            if exp < 0 {
                return Err(PrimError::Outside("BigInt.pow with a negative exponent"));
            }
            let mut scratch = ScratchGuard::new(heap.budget());
            let base = read_arg(&args[0], &mut scratch)?;
            let res = base.pow(exp, &mut scratch).map_err(map_bigint_err)?;
            write_result(heap, &res, &mut scratch)
        }
        _ => unreachable!("unexpected BigInt primitive {prim:?}"),
    }
}
