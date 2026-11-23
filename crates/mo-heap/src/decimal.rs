//! Exact 128-bit fixed-point Decimal arithmetic (`src/base/decimal.c` parity).
//!
//! A Mo `Decimal` stores `value * 10^-18` in `i128`, bounded to `-(2^127 - 1) .. +(2^127 - 1)`
//! (rejecting `i128::MIN`). Multiplication and division use 256-bit intermediates with
//! round-to-nearest, ties-to-even.

pub const DECIMAL_SCALE: i128 = 1_000_000_000_000_000_000;
pub const DECIMAL_SCALE_U128: u128 = 1_000_000_000_000_000_000;
pub const DECIMAL_MAX: i128 = i128::MAX;

#[derive(Clone, Copy, Default)]
struct Wide {
    words: [u64; 4],
}

fn magnitude(value: i128) -> u128 {
    debug_assert!(value >= -DECIMAL_MAX);
    if value < 0 {
        (-value) as u128
    } else {
        value as u128
    }
}

fn multiply(a: u128, b: u128) -> Wide {
    let x = [a as u64, (a >> 64) as u64];
    let y = [b as u64, (b >> 64) as u64];
    let mut out = Wide::default();
    for (i, &xi) in x.iter().enumerate() {
        let mut carry: u128 = 0;
        for (j, &yj) in y.iter().enumerate() {
            let product = u128::from(xi) * u128::from(yj) + u128::from(out.words[i + j]) + carry;
            out.words[i + j] = product as u64;
            carry = product >> 64;
        }
        out.words[i + 2] = carry as u64;
    }
    out
}

fn divide_step(remainder: &mut u128, bit: bool, denominator: u128) -> bool {
    debug_assert!(*remainder < denominator && denominator < (1u128 << 127));
    *remainder = (*remainder << 1) | u128::from(bit);
    if *remainder < denominator {
        return false;
    }
    *remainder -= denominator;
    true
}

fn round_quotient(
    mut quotient: Wide,
    remainder: u128,
    denominator: u128,
    negative: bool,
) -> Option<i128> {
    debug_assert!(remainder < denominator && denominator < (1u128 << 127));
    let twice = remainder * 2;
    if twice > denominator || (twice == denominator && (quotient.words[0] & 1) != 0) {
        for i in 0..4 {
            quotient.words[i] = quotient.words[i].wrapping_add(1);
            if quotient.words[i] != 0 {
                break;
            }
            if i == 3 {
                return None;
            }
        }
    }
    if quotient.words[2] != 0 || quotient.words[3] != 0 || (quotient.words[1] >> 63) != 0 {
        return None;
    }
    let value = ((u128::from(quotient.words[1]) << 64) | u128::from(quotient.words[0])) as i128;
    Some(if negative { -value } else { value })
}

fn rounded(numerator: Wide, denominator: u128, negative: bool) -> Option<i128> {
    debug_assert!(denominator != 0 && denominator < (1u128 << 127));
    let mut quotient = Wide::default();
    let mut remainder: u128 = 0;
    for bit in (0..256).rev() {
        let b = ((numerator.words[bit / 64] >> (bit % 64)) & 1) != 0;
        if divide_step(&mut remainder, b, denominator) {
            quotient.words[bit / 64] |= 1u64 << (bit % 64);
        }
    }
    round_quotient(quotient, remainder, denominator, negative)
}

pub fn decimal_add(a: i128, b: i128) -> Option<i128> {
    if (b > 0 && a > DECIMAL_MAX - b) || (b < 0 && a < -DECIMAL_MAX - b) {
        None
    } else {
        Some(a + b)
    }
}

pub fn decimal_sub(a: i128, b: i128) -> Option<i128> {
    decimal_add(a, -b)
}

pub fn decimal_mul(left: i128, right: i128) -> Option<i128> {
    rounded(
        multiply(magnitude(left), magnitude(right)),
        DECIMAL_SCALE_U128,
        (left < 0) != (right < 0),
    )
}

pub fn decimal_div(left: i128, right: i128) -> Option<i128> {
    rounded(
        multiply(magnitude(left), DECIMAL_SCALE_U128),
        magnitude(right),
        (left < 0) != (right < 0),
    )
}

pub fn decimal_rem(left: i128, right: i128) -> i128 {
    left % right
}
