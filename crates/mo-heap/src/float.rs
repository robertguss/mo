//! Exact shortest binary64 decimal formatting (`src/heap/float.c` parity).
//!
//! All arithmetic fits in 1,536 bits (48 x u32 words); no locale, floating arithmetic,
//! or libc conversion.

use std::cmp::Ordering;

const FLOAT_WORDS: usize = 48;

#[derive(Clone, Copy)]
struct FloatNat {
    words: [u32; FLOAT_WORDS],
    length: usize,
}

impl Default for FloatNat {
    fn default() -> Self {
        Self {
            words: [0; FLOAT_WORDS],
            length: 0,
        }
    }
}

fn nat_set(value: u64, shift: usize) -> FloatNat {
    let mut out = FloatNat::default();
    if value == 0 {
        return out;
    }
    let mut offset = shift / 32;
    let bits = shift % 32;
    let mut wide = u128::from(value) << bits;
    while wide != 0 {
        debug_assert!(offset < FLOAT_WORDS);
        out.words[offset] = wide as u32;
        offset += 1;
        wide >>= 32;
    }
    out.length = offset;
    out
}

fn nat_compare(a: &FloatNat, b: &FloatNat) -> Ordering {
    if a.length != b.length {
        return a.length.cmp(&b.length);
    }
    for i in (0..a.length).rev() {
        if a.words[i] != b.words[i] {
            return a.words[i].cmp(&b.words[i]);
        }
    }
    Ordering::Equal
}

fn nat_scale(a: &FloatNat, scale: u64) -> FloatNat {
    let mut result = FloatNat::default();
    let mut carry: u128 = 0;
    for i in 0..a.length {
        carry += u128::from(a.words[i]) * u128::from(scale);
        result.words[i] = carry as u32;
        carry >>= 32;
    }
    result.length = a.length;
    while carry != 0 {
        debug_assert!(result.length < FLOAT_WORDS);
        result.words[result.length] = carry as u32;
        result.length += 1;
        carry >>= 32;
    }
    while result.length != 0 && result.words[result.length - 1] == 0 {
        result.length -= 1;
    }
    result
}

fn nat_multiply(a: &FloatNat, b: &FloatNat) -> FloatNat {
    let mut out = FloatNat::default();
    debug_assert!(a.length + b.length <= FLOAT_WORDS);
    if a.length == 0 || b.length == 0 {
        return out;
    }
    for i in 0..a.length {
        let mut carry: u64 = 0;
        for j in 0..b.length {
            carry += u64::from(a.words[i]) * u64::from(b.words[j]) + u64::from(out.words[i + j]);
            out.words[i + j] = carry as u32;
            carry >>= 32;
        }
        out.words[i + b.length] = carry as u32;
    }
    out.length = a.length + b.length;
    while out.length != 0 && out.words[out.length - 1] == 0 {
        out.length -= 1;
    }
    out
}

fn binary_parts(bits: u64) -> (u64, i32) {
    let e = (bits >> 52) as u32;
    let mut mantissa = bits & 0x000f_ffff_ffff_ffff;
    let mut exponent = if e == 0 { -1074 } else { (e as i32) - 1075 };
    if e == 2047 {
        mantissa = 1;
        exponent = 1024;
    } else if e != 0 {
        mantissa |= 1u64 << 52;
    }
    (mantissa, exponent)
}

fn binary_ratio(mantissa: u64, exponent: i32) -> (FloatNat, FloatNat) {
    let n = nat_set(mantissa, if exponent > 0 { exponent as usize } else { 0 });
    let d = nat_set(
        1,
        if exponent < 0 {
            (-exponent) as usize
        } else {
            0
        },
    );
    (n, d)
}

fn midpoint(a: u64, b: u64) -> (FloatNat, FloatNat) {
    let (am, ae) = binary_parts(a);
    let (bm, be) = binary_parts(b);
    let e = ae.min(be);
    let m = (am << ((ae - e) as u32)) + (bm << ((be - e) as u32));
    binary_ratio(m, e - 1)
}

fn nearest(n: &FloatNat, d: &FloatNat) -> u64 {
    let mut low: u64 = 0;
    let mut high: u64 = 1_000_000_000_000_000_000;
    while low + 1 < high {
        let middle = low + (high - low) / 2;
        let product = nat_scale(d, middle);
        if nat_compare(&product, n) != Ordering::Greater {
            low = middle;
        } else {
            high = middle;
        }
    }
    let twice = nat_scale(n, 2);
    let product = nat_scale(d, 2 * low + 1);
    let cmp = nat_compare(&twice, &product);
    if cmp == Ordering::Greater || (cmp == Ordering::Equal && !low.is_multiple_of(2)) {
        low + 1
    } else {
        low
    }
}

fn round_trips(
    coefficient: u64,
    exponent: i32,
    bits: u64,
    lower_n: &FloatNat,
    lower_d: &FloatNat,
    upper_n: &FloatNat,
    upper_d: &FloatNat,
) -> bool {
    let mut n = nat_set(coefficient, 0);
    let mut d = nat_set(1, 0);
    if exponent > 0 {
        for _ in 0..exponent {
            n = nat_scale(&n, 10);
        }
    } else {
        for _ in 0..(-exponent) {
            d = nat_scale(&d, 10);
        }
    }
    let lower = nat_compare(&nat_multiply(&n, lower_d), &nat_multiply(lower_n, &d));
    let upper = nat_compare(&nat_multiply(&n, upper_d), &nat_multiply(upper_n, &d));
    let even = bits.is_multiple_of(2);
    let lower_ok = lower == Ordering::Greater || (lower == Ordering::Equal && even);
    let upper_ok = upper == Ordering::Less || (upper == Ordering::Equal && even);
    lower_ok && upper_ok
}

pub fn format_float(value: f64, out: &mut String) {
    let bits = value.to_bits();
    let magnitude = bits & 0x7fff_ffff_ffff_ffff;
    if magnitude > 0x7ff0_0000_0000_0000 {
        out.push_str("nan");
        return;
    }
    if (bits >> 63) != 0 {
        out.push('-');
    }
    if magnitude == 0x7ff0_0000_0000_0000 {
        out.push_str("inf");
        return;
    }
    if magnitude == 0 {
        out.push('0');
        return;
    }

    let (mantissa, binary_exponent) = binary_parts(magnitude);
    let (mut n, mut d) = binary_ratio(mantissa, binary_exponent);
    let (lower_n, lower_d) = midpoint(magnitude - 1, magnitude);
    let (upper_n, upper_d) = midpoint(magnitude, magnitude + 1);

    let mut k: i32 = 0;
    if nat_compare(&n, &d) == Ordering::Less {
        loop {
            n = nat_scale(&n, 10);
            k -= 1;
            if nat_compare(&n, &d) != Ordering::Less {
                break;
            }
        }
    } else {
        loop {
            let next = nat_scale(&d, 10);
            if nat_compare(&n, &next) == Ordering::Less {
                break;
            }
            d = next;
            k += 1;
        }
    }

    let mut coefficient: u64 = 0;
    let mut exponent: i32 = k;
    let mut found = false;
    for digits in 1..=17i32 {
        let q = nearest(&n, &d);
        let candidates = [q, q.saturating_sub(1), q + 1];
        exponent = k - digits + 1;
        for cand in candidates {
            if round_trips(
                cand, exponent, magnitude, &lower_n, &lower_d, &upper_n, &upper_d,
            ) {
                coefficient = cand;
                found = true;
                break;
            }
        }
        if found {
            break;
        }
        n = nat_scale(&n, 10);
    }
    debug_assert!(found && coefficient != 0);

    while coefficient.is_multiple_of(10) {
        coefficient /= 10;
        exponent += 1;
    }

    let mut buf = [0u8; 20];
    let mut start = buf.len();
    let mut c = coefficient;
    while c != 0 {
        start -= 1;
        buf[start] = b'0' + (c % 10) as u8;
        c /= 10;
    }
    let digits = &buf[start..];
    let length = digits.len();
    let point = (length as i32) + exponent;

    if !(-5..=21).contains(&point) {
        out.push(digits[0] as char);
        if length > 1 {
            out.push('.');
            for &b in &digits[1..] {
                out.push(b as char);
            }
        }
        out.push('e');
        out.push_str(&(point - 1).to_string());
        return;
    }
    if point <= 0 {
        out.push_str("0.");
        for _ in 0..(-point) {
            out.push('0');
        }
        for &b in digits {
            out.push(b as char);
        }
        return;
    }
    let total = length.max(point as usize);
    for i in 0..total {
        if i == point as usize {
            out.push('.');
        }
        let ch = digits.get(i).copied().map_or('0', |b| b as char);
        out.push(ch);
    }
}
