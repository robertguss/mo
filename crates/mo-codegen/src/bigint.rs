//! Balanced Karatsuba decimal-to-binary conversion matching `src/codegen/bigint.c`.

use mo_bytecode::BigIntSign;
use mo_core::CoreInteger;
use mo_def::limits::{MO_LIMIT_CODEGEN_BIGINT_WORK, MO_LIMIT_CORE_TEXT_SIZE};

const DECIMAL_BASE: i128 = 1_000_000_000;
const MULTIPLY_LEAF: usize = 16;

/// Error returned when decimal BigInt conversion exceeds its work budget.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BigIntWorkLimit;

fn charge(work: &mut usize, amount: usize) -> Result<(), BigIntWorkLimit> {
    let limit = MO_LIMIT_CODEGEN_BIGINT_WORK as usize;
    if amount > limit.saturating_sub(*work) {
        return Err(BigIntWorkLimit);
    }
    *work += amount;
    Ok(())
}

/// Predicts coefficient work before allocating scratch (matches `fits_work` in `bigint.c`).
fn fits_work(chunks: usize, mut work: usize) -> bool {
    let limit = MO_LIMIT_CODEGEN_BIGINT_WORK as usize;
    if charge(&mut work, 2 * chunks).is_err() {
        return false;
    }
    let mut multiplication: usize = 3; // M(1)
    let mut stride: usize = 1;
    while stride < chunks {
        let groups = chunks / (2 * stride);
        let cost = multiplication + 3 * stride;
        if cost > (limit - work) / groups || charge(&mut work, groups * cost).is_err() {
            return false;
        }
        if 2 * stride < chunks && charge(&mut work, multiplication + 4 * stride).is_err() {
            return false;
        }
        multiplication = if stride < MULTIPLY_LEAF {
            4 * stride + 4 * stride * stride
        } else {
            10 * stride + 3 * multiplication
        };
        stride *= 2;
    }
    true
}

#[derive(Clone, Copy)]
struct Frame {
    a_off: usize,
    b_off: usize,
    out_off: usize,
    scratch_off: usize,
    count: usize,
    stage: u8,
}

/// Iterative Karatsuba multiplication over offsets inside `storage` (zero raw pointers,
/// zero recursion, exact work accounting matching `multiply` in `src/codegen/bigint.c`).
fn multiply(
    storage: &mut [i128],
    a_off: usize,
    b_off: usize,
    out_off: usize,
    count: usize,
    scratch_off: usize,
    work: &mut usize,
) -> Result<(), BigIntWorkLimit> {
    let mut stack = Vec::with_capacity(24);
    stack.push(Frame {
        a_off,
        b_off,
        out_off,
        scratch_off,
        count,
        stage: 0,
    });
    while let Some(top) = stack.last_mut() {
        let f = *top;
        let n = f.count;
        let half = n / 2;
        if f.stage == 0 {
            charge(work, 2 * n)?;
            storage[f.out_off..f.out_off + 2 * n].fill(0);
            if n <= MULTIPLY_LEAF {
                charge(work, n * n)?;
                for i in 0..n {
                    let ai = storage[f.a_off + i];
                    for j in 0..n {
                        let bj = storage[f.b_off + j];
                        storage[f.out_off + i + j] += ai * bj;
                    }
                }
                stack.pop();
                continue;
            }
            charge(work, 2 * half)?;
            for i in 0..half {
                storage[f.scratch_off + i] = storage[f.a_off + i] + storage[f.a_off + half + i];
                storage[f.scratch_off + half + i] =
                    storage[f.b_off + i] + storage[f.b_off + half + i];
            }
            top.stage = 1;
            stack.push(Frame {
                a_off: f.a_off,
                b_off: f.b_off,
                out_off: f.out_off,
                scratch_off: f.scratch_off + 2 * n,
                count: half,
                stage: 0,
            });
        } else if f.stage == 1 {
            top.stage = 2;
            stack.push(Frame {
                a_off: f.a_off + half,
                b_off: f.b_off + half,
                out_off: f.out_off + n,
                scratch_off: f.scratch_off + 2 * n,
                count: half,
                stage: 0,
            });
        } else if f.stage == 2 {
            top.stage = 3;
            stack.push(Frame {
                a_off: f.scratch_off,
                b_off: f.scratch_off + half,
                out_off: f.scratch_off + n,
                scratch_off: f.scratch_off + 2 * n,
                count: half,
                stage: 0,
            });
        } else {
            charge(work, 2 * n)?;
            for i in 0..n {
                storage[f.scratch_off + n + i] -=
                    storage[f.out_off + i] + storage[f.out_off + n + i];
            }
            for i in 0..n {
                storage[f.out_off + half + i] += storage[f.scratch_off + n + i];
            }
            stack.pop();
        }
    }
    Ok(())
}

fn normalize(
    storage: &mut [i128],
    off: usize,
    count: usize,
    work: &mut usize,
) -> Result<(), BigIntWorkLimit> {
    charge(work, count)?;
    let mut carry: i128 = 0;
    for slot in &mut storage[off..off + count] {
        carry += *slot;
        debug_assert!(carry >= 0);
        *slot = carry & i128::from(u32::MAX);
        carry >>= 32;
    }
    debug_assert_eq!(carry, 0);
    Ok(())
}

/// Converts a canonical decimal [`CoreInteger`] into `(BigIntSign, Vec<u8>)` little-endian
/// canonical magnitude bytes, enforcing `MO_LIMIT_CODEGEN_BIGINT_WORK`.
pub fn convert_bigint(
    integer: &CoreInteger,
    work: &mut usize,
) -> Result<(BigIntSign, Vec<u8>), BigIntWorkLimit> {
    let bytes = integer.digits.as_bytes();
    let length = bytes.len();
    debug_assert!(length != 0 && (length as u64) <= MO_LIMIT_CORE_TEXT_SIZE);
    charge(work, length)?;
    if length == 1 && bytes[0] == b'0' {
        return Ok((BigIntSign::Zero, Vec::new()));
    }

    let mut chunks: usize = 1;
    let needed_chunks = length.div_ceil(9);
    while chunks < needed_chunks {
        chunks *= 2;
    }
    debug_assert!(chunks <= (1usize << 20));
    if !fits_work(chunks, *work) {
        return Err(BigIntWorkLimit);
    }

    let mut storage = vec![0i128; 9 * chunks];
    let mut current_off = 0usize;
    let mut next_off = chunks;
    let power_off = 2 * chunks;
    let product_off = 3 * chunks;
    let scratch_off = 5 * chunks;

    charge(work, chunks)?;
    let mut at = length;
    let mut i = 0usize;
    while at != 0 {
        let start = at.saturating_sub(9);
        let mut value: u32 = 0;
        for &b in &bytes[start..at] {
            debug_assert!(b.is_ascii_digit());
            value = value * 10 + u32::from(b - b'0');
        }
        storage[current_off + i] = i128::from(value);
        at = start;
        i += 1;
    }

    storage[power_off] = DECIMAL_BASE;
    let mut stride: usize = 1;
    while stride < chunks {
        let mut offset = 0usize;
        while offset < chunks {
            multiply(
                &mut storage,
                current_off + offset + stride,
                power_off,
                next_off + offset,
                stride,
                scratch_off,
                work,
            )?;
            charge(work, stride)?;
            for k in 0..stride {
                storage[next_off + offset + k] += storage[current_off + offset + k];
            }
            normalize(&mut storage, next_off + offset, 2 * stride, work)?;
            offset += 2 * stride;
        }
        std::mem::swap(&mut current_off, &mut next_off);
        if 2 * stride < chunks {
            multiply(
                &mut storage,
                power_off,
                power_off,
                product_off,
                stride,
                scratch_off,
                work,
            )?;
            normalize(&mut storage, product_off, 2 * stride, work)?;
            charge(work, 2 * stride)?;
            storage.copy_within(product_off..product_off + 2 * stride, power_off);
        }
        stride *= 2;
    }

    charge(work, chunks)?;
    let mut limbs = chunks;
    while limbs != 0 && storage[current_off + limbs - 1] == 0 {
        limbs -= 1;
    }
    debug_assert!(limbs != 0);
    charge(work, 4 * limbs)?;
    let mut magnitude = vec![0u8; 4 * limbs];
    for (idx, out_byte) in magnitude.iter_mut().enumerate() {
        let limb = storage[current_off + idx / 4] as u32;
        *out_byte = (limb >> (8 * (idx % 4))) as u8;
    }
    while magnitude.last() == Some(&0) {
        magnitude.pop();
    }
    let sign = if integer.negative {
        BigIntSign::Negative
    } else {
        BigIntSign::Positive
    };
    Ok((sign, magnitude))
}
