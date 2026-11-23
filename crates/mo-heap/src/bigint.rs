//! Pure-Rust, `#![forbid(unsafe_code)]`, zero-dependency arbitrary-precision `BigInt`
//! matching `src/heap/bigint.c` and `src/prim/bigint.c`.
//!
//! - Internal arithmetic uses little-endian base-`2^32` limbs (`Vec<u32>`).
//! - Heap/wire canonical form uses `(BigIntSign, Vec<u8>)` little-endian magnitude bytes
//!   with no trailing zero bytes (empty iff `BigIntSign::Zero`).
//! - Large multiplication uses iterative Karatsuba (explicit stack, no recursion).
//! - Large binary-to-decimal conversion uses balanced radix conversion + iterative Karatsuba.
//! - Multi-limb division uses Knuth Algorithm D normalized long division.
//! - Temporary buffers are charged against `HeapBudget` via `ScratchGuard`.

use std::cmp::Ordering;

use mo_bytecode::BigIntSign;
use mo_def::limits::MO_LIMIT_VALUE_BYTES;

use crate::{HeapBudget, HeapError, ScratchGuard};

const DECIMAL_BASE: i128 = 1_000_000_000;
const KARATSUBA_LEAF: usize = 16;
const MUL_SCHOOLBOOK_THRESHOLD: usize = 32;
const LIMB_BASE: u64 = 1u64 << 32;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BigIntError {
    DivByZero,
    NegativeExponent,
    OutOfMemory,
    ExceedsValueBytes,
}

impl From<HeapError> for BigIntError {
    fn from(_: HeapError) -> Self {
        Self::OutOfMemory
    }
}

/// Validates canonical `(BigIntSign, &[u8])` representation (`src/heap/value.c:mo_value_bigint`).
pub fn is_canonical(sign: BigIntSign, magnitude: &[u8]) -> bool {
    if (magnitude.len() as u64) > MO_LIMIT_VALUE_BYTES {
        return false;
    }
    let is_zero = sign == BigIntSign::Zero;
    if is_zero != magnitude.is_empty() {
        return false;
    }
    magnitude.last() != Some(&0)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BigInt {
    negative: bool,
    limbs: Vec<u32>,
}

impl BigInt {
    pub fn zero() -> Self {
        Self {
            negative: false,
            limbs: Vec::new(),
        }
    }

    pub fn limb_bytes(&self) -> usize {
        self.limbs.len().saturating_mul(std::mem::size_of::<u32>())
    }

    pub fn from_i64(v: i64) -> Self {
        if v == 0 {
            return Self::zero();
        }
        let mag = v.unsigned_abs();
        let lo = mag as u32;
        let hi = (mag >> 32) as u32;
        let limbs = if hi == 0 { vec![lo] } else { vec![lo, hi] };
        Self {
            negative: v < 0,
            limbs,
        }
    }

    pub fn from_canonical_bytes(
        sign: BigIntSign,
        magnitude: &[u8],
        scratch: &mut ScratchGuard<'_>,
    ) -> Result<Self, BigIntError> {
        if !is_canonical(sign, magnitude) {
            return Err(BigIntError::ExceedsValueBytes);
        }
        if sign == BigIntSign::Zero {
            return Ok(Self::zero());
        }
        let limb_count = magnitude.len().div_ceil(4);
        scratch.charge(limb_count.saturating_mul(std::mem::size_of::<u32>()))?;
        let mut limbs = Vec::with_capacity(limb_count);
        for chunk in magnitude.chunks(4) {
            let mut buf = [0u8; 4];
            buf[..chunk.len()].copy_from_slice(chunk);
            limbs.push(u32::from_le_bytes(buf));
        }
        let mut out = Self {
            negative: sign == BigIntSign::Negative,
            limbs,
        };
        out.trim();
        Ok(out)
    }

    pub fn to_canonical_bytes(
        &self,
        scratch: &mut ScratchGuard<'_>,
    ) -> Result<(BigIntSign, Vec<u8>), BigIntError> {
        if self.limbs.is_empty() {
            return Ok((BigIntSign::Zero, Vec::new()));
        }
        let max_bytes = self
            .limbs
            .len()
            .checked_mul(4)
            .ok_or(BigIntError::ExceedsValueBytes)?;
        let top = *self.limbs.last().unwrap();
        let trailing_zeros = (top.leading_zeros() / 8) as usize;
        let byte_len = max_bytes - trailing_zeros;
        if (byte_len as u64) > MO_LIMIT_VALUE_BYTES {
            return Err(BigIntError::ExceedsValueBytes);
        }
        scratch.charge(byte_len)?;
        let mut bytes = Vec::with_capacity(byte_len);
        for &limb in &self.limbs {
            bytes.extend_from_slice(&limb.to_le_bytes());
        }
        bytes.truncate(byte_len);
        let sign = if self.negative {
            BigIntSign::Negative
        } else {
            BigIntSign::Positive
        };
        Ok((sign, bytes))
    }

    pub fn is_zero(&self) -> bool {
        self.limbs.is_empty()
    }

    pub fn is_negative(&self) -> bool {
        self.negative && !self.limbs.is_empty()
    }

    fn trim(&mut self) {
        while self.limbs.last() == Some(&0) {
            self.limbs.pop();
        }
        if self.limbs.is_empty() {
            self.negative = false;
        }
    }

    pub fn neg(&self, scratch: &mut ScratchGuard<'_>) -> Result<Self, BigIntError> {
        scratch.charge(self.limbs.len().saturating_mul(std::mem::size_of::<u32>()))?;
        Ok(Self {
            negative: !self.limbs.is_empty() && !self.negative,
            limbs: self.limbs.clone(),
        })
    }

    pub fn abs(&self, scratch: &mut ScratchGuard<'_>) -> Result<Self, BigIntError> {
        scratch.charge(self.limbs.len().saturating_mul(std::mem::size_of::<u32>()))?;
        Ok(Self {
            negative: false,
            limbs: self.limbs.clone(),
        })
    }

    pub fn to_i64_exact(&self) -> Option<i64> {
        if self.limbs.is_empty() {
            return Some(0);
        }
        if self.limbs.len() > 2 {
            return None;
        }
        let mag = u64::from(self.limbs[0])
            | (if self.limbs.len() == 2 {
                u64::from(self.limbs[1]) << 32
            } else {
                0
            });
        if self.negative {
            let max_neg = 1u64 << 63;
            if mag > max_neg {
                None
            } else if mag == max_neg {
                Some(i64::MIN)
            } else {
                Some(-(mag as i64))
            }
        } else if mag > i64::MAX as u64 {
            None
        } else {
            Some(mag as i64)
        }
    }

    pub fn add(&self, other: &Self, scratch: &mut ScratchGuard<'_>) -> Result<Self, BigIntError> {
        add_signed(
            &self.limbs,
            self.negative,
            &other.limbs,
            other.negative,
            scratch,
        )
    }

    pub fn sub(&self, other: &Self, scratch: &mut ScratchGuard<'_>) -> Result<Self, BigIntError> {
        add_signed(
            &self.limbs,
            self.negative,
            &other.limbs,
            !other.limbs.is_empty() && !other.negative,
            scratch,
        )
    }

    pub fn mul(&self, other: &Self, scratch: &mut ScratchGuard<'_>) -> Result<Self, BigIntError> {
        if self.limbs.is_empty() || other.limbs.is_empty() {
            return Ok(Self::zero());
        }
        let min_bytes = (self.limbs.len() + other.limbs.len() - 2)
            .checked_mul(4)
            .and_then(|v| v.checked_add(1))
            .ok_or(BigIntError::ExceedsValueBytes)?;
        if (min_bytes as u64) > MO_LIMIT_VALUE_BYTES {
            return Err(BigIntError::ExceedsValueBytes);
        }

        let (a, b) = if self.limbs.len() >= other.limbs.len() {
            (self.limbs.as_slice(), other.limbs.as_slice())
        } else {
            (other.limbs.as_slice(), self.limbs.as_slice())
        };
        let out_len = a.len() + b.len();
        scratch.charge(out_len.saturating_mul(std::mem::size_of::<u32>()))?;
        let mut out_limbs = vec![0u32; out_len];

        if b.len() <= MUL_SCHOOLBOOK_THRESHOLD {
            for (j, &bj) in b.iter().enumerate() {
                let mut carry: u64 = 0;
                for (i, &ai) in a.iter().enumerate() {
                    let word = u64::from(ai) * u64::from(bj) + u64::from(out_limbs[i + j]) + carry;
                    out_limbs[i + j] = word as u32;
                    carry = word >> 32;
                }
                out_limbs[a.len() + j] = carry as u32;
            }
        } else {
            let n = a.len().max(b.len()).next_power_of_two();
            let total_cells = n.checked_mul(8).ok_or(BigIntError::OutOfMemory)?;
            let mem_bytes = total_cells
                .checked_mul(std::mem::size_of::<i128>())
                .ok_or(BigIntError::OutOfMemory)?;
            scratch.charge(mem_bytes)?;
            let mut mem = vec![0i128; total_cells];
            for (i, &v) in a.iter().enumerate() {
                mem[i] = i128::from(v);
            }
            for (i, &v) in b.iter().enumerate() {
                mem[n + i] = i128::from(v);
            }
            karatsuba_i128(&mut mem, 0, n, 2 * n, 4 * n, n);
            let mut carry: i128 = 0;
            for (i, dst) in out_limbs.iter_mut().enumerate() {
                carry += mem[2 * n + i];
                debug_assert!(carry >= 0);
                *dst = ((carry as u128) & 0xffff_ffff) as u32;
                carry >>= 32;
            }
            debug_assert_eq!(carry, 0);
        }

        let mut res = Self {
            negative: self.negative != other.negative,
            limbs: out_limbs,
        };
        res.trim();
        Ok(res)
    }

    /// Truncated division toward zero (`BigInt.%div` and `BigInt.%rem`).
    pub fn div_rem_trunc(
        &self,
        other: &Self,
        scratch: &mut ScratchGuard<'_>,
    ) -> Result<(Self, Self), BigIntError> {
        if other.limbs.is_empty() {
            return Err(BigIntError::DivByZero);
        }
        let (mut q, mut r) = div_mod_mag(&self.limbs, &other.limbs, scratch)?;
        q.negative = !q.limbs.is_empty() && (self.negative != other.negative);
        r.negative = !r.limbs.is_empty() && self.negative;
        Ok((q, r))
    }

    /// Floor division toward negative infinity (`BigInt.floor_div` and `BigInt.floor_mod`).
    pub fn div_rem_floor(
        &self,
        other: &Self,
        scratch: &mut ScratchGuard<'_>,
    ) -> Result<(Self, Self), BigIntError> {
        if other.limbs.is_empty() {
            return Err(BigIntError::DivByZero);
        }
        let (mut q, mut r) = div_mod_mag(&self.limbs, &other.limbs, scratch)?;
        if self.negative != other.negative && !r.limbs.is_empty() {
            q = add_signed(&q.limbs, false, &[1], false, scratch)?;
            r = add_signed(&other.limbs, false, &r.limbs, true, scratch)?;
        }
        q.negative = !q.limbs.is_empty() && (self.negative != other.negative);
        r.negative = !r.limbs.is_empty() && other.negative;
        Ok((q, r))
    }

    /// Integer exponentiation (`BigInt.pow`).
    pub fn pow(
        &self,
        mut exponent: i64,
        scratch: &mut ScratchGuard<'_>,
    ) -> Result<Self, BigIntError> {
        if exponent < 0 {
            return Err(BigIntError::NegativeExponent);
        }
        if exponent == 0 {
            return Ok(Self::from_i64(1));
        }
        if self.limbs.is_empty() || (self.limbs.len() == 1 && self.limbs[0] == 1) {
            return Ok(Self {
                negative: self.negative && (exponent & 1) != 0,
                limbs: self.limbs.clone(),
            });
        }
        let mut result = Self::from_i64(1);
        let mut base = self.clone();
        while exponent != 0 {
            if (exponent & 1) != 0 {
                result = result.mul(&base, scratch)?;
            }
            exponent >>= 1;
            if exponent != 0 {
                base = base.mul(&base, scratch)?;
            }
        }
        Ok(result)
    }
}

impl Ord for BigInt {
    fn cmp(&self, other: &Self) -> Ordering {
        let sa = if self.limbs.is_empty() {
            0i8
        } else if self.negative {
            -1
        } else {
            1
        };
        let sb = if other.limbs.is_empty() {
            0i8
        } else if other.negative {
            -1
        } else {
            1
        };
        if sa != sb {
            return sa.cmp(&sb);
        }
        if sa == 0 {
            return Ordering::Equal;
        }
        let mag = cmp_mag(&self.limbs, &other.limbs);
        if sa < 0 { mag.reverse() } else { mag }
    }
}

impl PartialOrd for BigInt {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

fn cmp_mag(a: &[u32], b: &[u32]) -> Ordering {
    a.len()
        .cmp(&b.len())
        .then_with(|| a.iter().rev().cmp(b.iter().rev()))
}

fn add_signed(
    a: &[u32],
    a_neg: bool,
    b: &[u32],
    b_neg: bool,
    scratch: &mut ScratchGuard<'_>,
) -> Result<BigInt, BigIntError> {
    let mut out = if a_neg == b_neg {
        let count = a.len().max(b.len());
        scratch.charge((count + 1).saturating_mul(std::mem::size_of::<u32>()))?;
        let mut limbs = vec![0u32; count + 1];
        let mut carry: u64 = 0;
        for (i, dst) in limbs[..count].iter_mut().enumerate() {
            let av = a.get(i).copied().unwrap_or(0);
            let bv = b.get(i).copied().unwrap_or(0);
            carry += u64::from(av) + u64::from(bv);
            *dst = carry as u32;
            carry >>= 32;
        }
        limbs[count] = carry as u32;
        BigInt {
            negative: a_neg,
            limbs,
        }
    } else {
        let (hi, hi_neg, lo) = if cmp_mag(a, b) == Ordering::Less {
            (b, b_neg, a)
        } else {
            (a, a_neg, b)
        };
        scratch.charge(hi.len().saturating_mul(std::mem::size_of::<u32>()))?;
        let mut limbs = vec![0u32; hi.len()];
        let mut borrow: u64 = 0;
        for i in 0..hi.len() {
            let sub = u64::from(lo.get(i).copied().unwrap_or(0)) + borrow;
            let hv = u64::from(hi[i]);
            limbs[i] = hv.wrapping_sub(sub) as u32;
            borrow = u64::from(hv < sub);
        }
        debug_assert_eq!(borrow, 0);
        BigInt {
            negative: hi_neg,
            limbs,
        }
    };
    out.trim();
    Ok(out)
}

#[derive(Clone, Copy)]
struct KaratsubaFrame {
    a: usize,
    b: usize,
    out: usize,
    scratch: usize,
    count: usize,
    stage: u8,
}

/// Iterative Karatsuba over `i128` arrays (`src/prim/bigint.c` & `src/heap/bigint.c` parity).
/// `mem[scratch .. scratch + 4*count]` must be disjoint from `a`, `b`, and `out`.
fn karatsuba_i128(mem: &mut [i128], a: usize, b: usize, out: usize, scratch: usize, count: usize) {
    let mut stack = [KaratsubaFrame {
        a: 0,
        b: 0,
        out: 0,
        scratch: 0,
        count: 0,
        stage: 0,
    }; 32];
    let mut depth = 1usize;
    stack[0] = KaratsubaFrame {
        a,
        b,
        out,
        scratch,
        count,
        stage: 0,
    };

    while depth != 0 {
        let f = &mut stack[depth - 1];
        let n = f.count;
        let half = n / 2;
        match f.stage {
            0 => {
                mem[f.out..f.out + 2 * n].fill(0);
                if n <= KARATSUBA_LEAF {
                    for i in 0..n {
                        let ai = mem[f.a + i];
                        for j in 0..n {
                            mem[f.out + i + j] += ai * mem[f.b + j];
                        }
                    }
                    depth -= 1;
                    continue;
                }
                for i in 0..half {
                    mem[f.scratch + i] = mem[f.a + i] + mem[f.a + half + i];
                    mem[f.scratch + half + i] = mem[f.b + i] + mem[f.b + half + i];
                }
                let child = KaratsubaFrame {
                    a: f.a,
                    b: f.b,
                    out: f.out,
                    scratch: f.scratch + 2 * n,
                    count: half,
                    stage: 0,
                };
                f.stage = 1;
                stack[depth] = child;
                depth += 1;
            }
            1 => {
                let child = KaratsubaFrame {
                    a: f.a + half,
                    b: f.b + half,
                    out: f.out + n,
                    scratch: f.scratch + 2 * n,
                    count: half,
                    stage: 0,
                };
                f.stage = 2;
                stack[depth] = child;
                depth += 1;
            }
            2 => {
                let child = KaratsubaFrame {
                    a: f.scratch,
                    b: f.scratch + half,
                    out: f.scratch + n,
                    scratch: f.scratch + 2 * n,
                    count: half,
                    stage: 0,
                };
                f.stage = 3;
                stack[depth] = child;
                depth += 1;
            }
            _ => {
                for i in 0..n {
                    let mid = mem[f.scratch + n + i] - (mem[f.out + i] + mem[f.out + n + i]);
                    mem[f.scratch + n + i] = mid;
                }
                for i in 0..n {
                    mem[f.out + half + i] += mem[f.scratch + n + i];
                }
                depth -= 1;
            }
        }
    }
}

/// Unsigned magnitude division (`a = q * b + r`, `0 <= r < b`, Knuth Algorithm D).
fn div_mod_mag(
    a: &[u32],
    b: &[u32],
    scratch: &mut ScratchGuard<'_>,
) -> Result<(BigInt, BigInt), BigIntError> {
    debug_assert!(!b.is_empty());
    if cmp_mag(a, b) == Ordering::Less {
        scratch.charge(a.len().saturating_mul(std::mem::size_of::<u32>()))?;
        return Ok((
            BigInt::zero(),
            BigInt {
                negative: false,
                limbs: a.to_vec(),
            },
        ));
    }

    let q_len = a.len() - b.len() + 1;
    let r_len = b.len();
    scratch.charge((q_len + r_len).saturating_mul(std::mem::size_of::<u32>()))?;
    let mut q = vec![0u32; q_len];
    let mut r = vec![0u32; r_len];

    if b.len() == 1 {
        let d = u64::from(b[0]);
        let mut carry: u64 = 0;
        for i in (0..a.len()).rev() {
            let n = (carry << 32) | u64::from(a[i]);
            q[i] = (n / d) as u32;
            carry = n % d;
        }
        r[0] = carry as u32;
    } else {
        let n = b.len();
        let shift = b[n - 1].leading_zeros();
        scratch.charge((a.len() + 1 + n).saturating_mul(std::mem::size_of::<u32>()))?;
        let mut u = vec![0u32; a.len() + 1];
        let mut v = vec![0u32; n];

        let mut carry: u64 = 0;
        for i in 0..a.len() {
            carry |= u64::from(a[i]) << shift;
            u[i] = carry as u32;
            carry >>= 32;
        }
        u[a.len()] = carry as u32;

        carry = 0;
        for i in 0..n {
            carry |= u64::from(b[i]) << shift;
            v[i] = carry as u32;
            carry >>= 32;
        }
        debug_assert_eq!(carry, 0);

        let vn1 = u64::from(v[n - 1]);
        let vn2 = u64::from(v[n - 2]);
        for j in (0..q_len).rev() {
            let numerator = (u64::from(u[j + n]) << 32) | u64::from(u[j + n - 1]);
            let mut estimate = numerator / vn1;
            let mut remainder = numerator % vn1;
            while estimate >= LIMB_BASE
                || (remainder < LIMB_BASE
                    && estimate * vn2 > (remainder << 32) + u64::from(u[j + n - 2]))
            {
                estimate -= 1;
                remainder += vn1;
                if remainder >= LIMB_BASE {
                    break;
                }
            }

            let mut borrow: u64 = 0;
            for i in 0..n {
                let p = estimate * u64::from(v[i]) + borrow;
                let low = p as u32;
                borrow = (p >> 32) + u64::from(u[j + i] < low);
                u[j + i] = u[j + i].wrapping_sub(low);
            }
            let neg = u64::from(u[j + n]) < borrow;
            u[j + n] = u[j + n].wrapping_sub(borrow as u32);
            if neg {
                estimate -= 1;
                let mut add_carry: u64 = 0;
                for i in 0..n {
                    add_carry += u64::from(u[j + i]) + u64::from(v[i]);
                    u[j + i] = add_carry as u32;
                    add_carry >>= 32;
                }
                u[j + n] = u[j + n].wrapping_add(add_carry as u32);
            }
            q[j] = estimate as u32;
        }

        for i in 0..n {
            let word = (u64::from(u[i + 1]) << 32) | u64::from(u[i]);
            r[i] = (word >> shift) as u32;
        }
    }

    let mut q_big = BigInt {
        negative: false,
        limbs: q,
    };
    let mut r_big = BigInt {
        negative: false,
        limbs: r,
    };
    q_big.trim();
    r_big.trim();
    Ok((q_big, r_big))
}

fn normalize_decimal(digits: &mut [i128]) {
    let mut carry: i128 = 0;
    for d in digits.iter_mut() {
        carry += *d;
        debug_assert!(carry >= 0);
        *d = carry % DECIMAL_BASE;
        carry /= DECIMAL_BASE;
    }
    debug_assert_eq!(carry, 0);
}

/// Formats canonical `(sign, magnitude)` into decimal text using balanced radix conversion
/// and iterative Karatsuba for large magnitudes (`src/heap/bigint.c` parity), charging
/// temporary scratch against `budget` when provided.
pub fn format_bigint_checked(
    out: &mut String,
    sign: BigIntSign,
    magnitude: &[u8],
    budget: Option<&HeapBudget>,
) -> Result<(), HeapError> {
    if sign == BigIntSign::Zero || magnitude.is_empty() {
        out.push('0');
        return Ok(());
    }

    let limb_count = magnitude.len().div_ceil(4);
    if limb_count <= 16 {
        // Fast path for small/medium BigInts (<= 512 bits): avoids Karatsuba overhead.
        let mut limbs = [0u32; 16];
        for (idx, chunk) in magnitude.chunks(4).enumerate() {
            let mut buf = [0u8; 4];
            buf[..chunk.len()].copy_from_slice(chunk);
            limbs[idx] = u32::from_le_bytes(buf);
        }
        let mut len = limb_count;
        let mut groups = [0u32; 20];
        let mut group_count = 0usize;
        while len != 0 {
            let mut rem: u64 = 0;
            for limb in limbs[..len].iter_mut().rev() {
                let cur = (rem << 32) | u64::from(*limb);
                *limb = (cur / 1_000_000_000) as u32;
                rem = cur % 1_000_000_000;
            }
            groups[group_count] = rem as u32;
            group_count += 1;
            while len != 0 && limbs[len - 1] == 0 {
                len -= 1;
            }
        }
        if sign == BigIntSign::Negative {
            out.push('-');
        }
        if group_count != 0 {
            out.push_str(&groups[group_count - 1].to_string());
            for &g in groups[..group_count - 1].iter().rev() {
                push_9_digits(out, g);
            }
        }
        return Ok(());
    }

    // Balanced radix conversion + iterative Karatsuba (`src/heap/bigint.c`).
    let chunks = limb_count.next_power_of_two();
    let total_cells = chunks.checked_mul(12).ok_or(HeapError::OutOfMemory)?;
    let mem_bytes = total_cells
        .checked_mul(std::mem::size_of::<i128>())
        .ok_or(HeapError::OutOfMemory)?;
    let _guard = match budget {
        Some(b) => {
            let mut g = ScratchGuard::new(b);
            g.charge(mem_bytes)?;
            Some(g)
        }
        None => None,
    };

    let mut storage = vec![0i128; total_cells];
    let mut cur_off = 0usize;
    let mut next_off = 2 * chunks;
    let power_off = 4 * chunks;
    let product_off = 6 * chunks;
    let scratch_off = 8 * chunks;

    for i in 0..chunks {
        let mut word: u64 = 0;
        for j in 0..4 {
            let idx = 4 * i + j;
            if idx < magnitude.len() {
                word |= u64::from(magnitude[idx]) << (8 * j);
            }
        }
        storage[cur_off + 2 * i] = i128::from(word % 1_000_000_000);
        storage[cur_off + 2 * i + 1] = i128::from(word / 1_000_000_000);
    }

    storage[power_off] = 294_967_296;
    storage[power_off + 1] = 4;

    let mut stride = 2usize;
    while stride < 2 * chunks {
        let mut offset = 0usize;
        while offset < 2 * chunks {
            karatsuba_i128(
                &mut storage,
                cur_off + offset + stride,
                power_off,
                next_off + offset,
                scratch_off,
                stride,
            );
            for i in 0..stride {
                storage[next_off + offset + i] += storage[cur_off + offset + i];
            }
            normalize_decimal(&mut storage[next_off + offset..next_off + offset + 2 * stride]);
            offset += 2 * stride;
        }
        std::mem::swap(&mut cur_off, &mut next_off);
        if 2 * stride < 2 * chunks {
            karatsuba_i128(
                &mut storage,
                power_off,
                power_off,
                product_off,
                scratch_off,
                stride,
            );
            normalize_decimal(&mut storage[product_off..product_off + 2 * stride]);
            storage.copy_within(product_off..product_off + 2 * stride, power_off);
        }
        stride *= 2;
    }

    let mut length = 2 * chunks;
    while length != 0 && storage[cur_off + length - 1] == 0 {
        length -= 1;
    }
    if sign == BigIntSign::Negative {
        out.push('-');
    }
    if length != 0 {
        length -= 1;
        out.push_str(&(storage[cur_off + length] as u64).to_string());
        while length != 0 {
            length -= 1;
            push_9_digits(out, storage[cur_off + length] as u32);
        }
    }
    Ok(())
}

/// Infallible convenience wrapper used by structural printing (no budget limit).
pub fn format_bigint(out: &mut String, sign: BigIntSign, magnitude: &[u8]) {
    let _ = format_bigint_checked(out, sign, magnitude, None);
}

fn push_9_digits(out: &mut String, mut digit: u32) {
    let mut buf = [0u8; 9];
    for i in (0..9).rev() {
        buf[i] = b'0' + (digit % 10) as u8;
        digit /= 10;
    }
    for b in buf {
        out.push(b as char);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Heap;

    fn big_to_str(n: &BigInt, budget: &HeapBudget) -> String {
        let mut s = ScratchGuard::new(budget);
        let (sign, mag) = n.to_canonical_bytes(&mut s).unwrap();
        let mut out = String::new();
        format_bigint_checked(&mut out, sign, &mag, Some(budget)).unwrap();
        out
    }

    #[test]
    fn canonical_validation_and_roundtrip() {
        assert!(is_canonical(BigIntSign::Zero, &[]));
        assert!(!is_canonical(BigIntSign::Zero, &[1]));
        assert!(!is_canonical(BigIntSign::Positive, &[]));
        assert!(!is_canonical(BigIntSign::Negative, &[]));
        assert!(!is_canonical(BigIntSign::Positive, &[1, 0]));
        assert!(is_canonical(BigIntSign::Positive, &[0, 1]));

        let heap = Heap::new(1024 * 1024);
        let mut scratch = ScratchGuard::new(heap.budget());
        for v in [0i64, 1, -1, 255, 256, -256, i64::MAX, i64::MIN] {
            let b = BigInt::from_i64(v);
            assert_eq!(b.to_i64_exact(), Some(v));
            let (sign, mag) = b.to_canonical_bytes(&mut scratch).unwrap();
            assert!(is_canonical(sign, &mag));
            let decoded = BigInt::from_canonical_bytes(sign, &mag, &mut scratch).unwrap();
            assert_eq!(decoded, b);
            assert_eq!(big_to_str(&decoded, heap.budget()), v.to_string());
        }
    }

    #[test]
    fn to_i64_exact_boundaries() {
        let heap = Heap::new(1024 * 1024);
        let mut s = ScratchGuard::new(heap.budget());
        let one = BigInt::from_i64(1);
        let max = BigInt::from_i64(i64::MAX);
        let min = BigInt::from_i64(i64::MIN);

        assert_eq!(max.to_i64_exact(), Some(i64::MAX));
        assert_eq!(max.add(&one, &mut s).unwrap().to_i64_exact(), None);
        assert_eq!(min.to_i64_exact(), Some(i64::MIN));
        assert_eq!(min.sub(&one, &mut s).unwrap().to_i64_exact(), None);
        assert_eq!(
            min.add(&one, &mut s).unwrap().to_i64_exact(),
            Some(i64::MIN + 1)
        );
    }

    #[test]
    fn trunc_vs_floor_division_and_modulo_all_sign_combinations() {
        let heap = Heap::new(1024 * 1024);
        let mut s = ScratchGuard::new(heap.budget());
        let cases = [
            // (a, b, trunc_q, trunc_r, floor_q, floor_r)
            (7i64, 3i64, 2i64, 1i64, 2i64, 1i64),
            (-7, 3, -2, -1, -3, 2),
            (7, -3, -2, 1, -3, -2),
            (-7, -3, 2, -1, 2, -1),
            (6, 3, 2, 0, 2, 0),
            (-6, 3, -2, 0, -2, 0),
            (6, -3, -2, 0, -2, 0),
            (-6, -3, 2, 0, 2, 0),
            (0, 5, 0, 0, 0, 0),
            (0, -5, 0, 0, 0, 0),
            (1, 5, 0, 1, 0, 1),
            (-1, 5, 0, -1, -1, 4),
            (1, -5, 0, 1, -1, -4),
            (-1, -5, 0, -1, 0, -1),
        ];
        for (a, b, tq, tr, fq, fr) in cases {
            let ba = BigInt::from_i64(a);
            let bb = BigInt::from_i64(b);
            let (q1, r1) = ba.div_rem_trunc(&bb, &mut s).unwrap();
            assert_eq!(q1.to_i64_exact(), Some(tq), "trunc div {a} / {b}");
            assert_eq!(r1.to_i64_exact(), Some(tr), "trunc rem {a} % {b}");
            let (q2, r2) = ba.div_rem_floor(&bb, &mut s).unwrap();
            assert_eq!(q2.to_i64_exact(), Some(fq), "floor div {a} / {b}");
            assert_eq!(r2.to_i64_exact(), Some(fr), "floor mod {a} % {b}");
        }

        // Division by zero
        let zero = BigInt::zero();
        let seven = BigInt::from_i64(7);
        assert_eq!(
            seven.div_rem_trunc(&zero, &mut s),
            Err(BigIntError::DivByZero)
        );
        assert_eq!(
            seven.div_rem_floor(&zero, &mut s),
            Err(BigIntError::DivByZero)
        );
    }

    #[test]
    fn multi_limb_carry_borrow_mul_karatsuba_div_and_pow() {
        let heap = Heap::new(16 * 1024 * 1024);
        let mut s = ScratchGuard::new(heap.budget());

        // 2^64 and 2^128
        let two = BigInt::from_i64(2);
        let two64 = two.pow(64, &mut s).unwrap();
        assert_eq!(big_to_str(&two64, heap.budget()), "18446744073709551616");
        let two128 = two64.mul(&two64, &mut s).unwrap();
        assert_eq!(
            big_to_str(&two128, heap.budget()),
            "340282366920938463463374607431768211456"
        );

        // Multi-limb carry/borrow across limb boundaries (2^128 - 1 + 1 == 2^128)
        let one = BigInt::from_i64(1);
        let all_ones = two128.sub(&one, &mut s).unwrap();
        assert_eq!(all_ones.limbs, vec![u32::MAX, u32::MAX, u32::MAX, u32::MAX]);
        assert_eq!(all_ones.add(&one, &mut s).unwrap(), two128);

        // Exercise Karatsuba multiplication (> 32 limbs -> > 1024 bits) AND balanced decimal format (> 16 limbs)
        let big_a = BigInt::from_i64(3).pow(900, &mut s).unwrap();
        let big_b = BigInt::from_i64(7).pow(600, &mut s).unwrap();
        assert!(big_a.limbs.len() > 32 && big_b.limbs.len() > 32);
        let prod = big_a.mul(&big_b, &mut s).unwrap();
        let (q, r) = prod.div_rem_trunc(&big_a, &mut s).unwrap();
        assert_eq!(q, big_b);
        assert!(r.is_zero());

        // Verify Knuth Algorithm D with high-bit divisor and quotient estimate adjustment / borrow-back
        // Classic Knuth D edge case: divisor with [0, 0x8000_0000] and dividend with max limbs.
        let divisor = BigInt {
            negative: false,
            limbs: vec![u32::MAX, u32::MAX, 0x8000_0000],
        };
        let dividend = BigInt {
            negative: false,
            limbs: vec![0, 0, u32::MAX, u32::MAX, 0x7fff_ffff],
        };
        let (dq, dr) = dividend.div_rem_trunc(&divisor, &mut s).unwrap();
        let recomposed = dq.mul(&divisor, &mut s).unwrap().add(&dr, &mut s).unwrap();
        assert_eq!(recomposed, dividend);
        assert!(dr < divisor);

        // Verify balanced radix decimal formatting matches naive powers of 10
        let ten_1000 = BigInt::from_i64(10).pow(1000, &mut s).unwrap();
        let ten_1000_str = big_to_str(&ten_1000, heap.budget());
        assert_eq!(ten_1000_str.len(), 1001);
        assert!(ten_1000_str.starts_with('1'));
        assert!(ten_1000_str[1..].bytes().all(|b| b == b'0'));

        // Fast-path pow for -1, 0, 1 even with huge exponents, and negative exponent error
        assert_eq!(
            BigInt::from_i64(-1)
                .pow(i64::MAX, &mut s)
                .unwrap()
                .to_i64_exact(),
            Some(-1)
        );
        assert_eq!(
            BigInt::from_i64(-1)
                .pow(i64::MAX - 1, &mut s)
                .unwrap()
                .to_i64_exact(),
            Some(1)
        );
        assert_eq!(
            BigInt::zero().pow(0, &mut s).unwrap().to_i64_exact(),
            Some(1)
        );
        assert_eq!(
            BigInt::zero().pow(i64::MAX, &mut s).unwrap().to_i64_exact(),
            Some(0)
        );
        assert_eq!(two.pow(-1, &mut s), Err(BigIntError::NegativeExponent));
    }

    #[test]
    fn heap_budget_oom_is_enforced_and_reclaimed() {
        let heap = Heap::new(2048);
        {
            let mut scratch = ScratchGuard::new(heap.budget());
            let two = BigInt::from_i64(2);
            // 2^100000 needs ~12.5 KiB per value, far past 2048 B budget.
            let res = two.pow(100_000, &mut scratch);
            assert_eq!(res, Err(BigIntError::OutOfMemory));
        }
        // ScratchGuard must release every temporary byte on Drop!
        assert_eq!(heap.budget().used(), 0);
    }
}
