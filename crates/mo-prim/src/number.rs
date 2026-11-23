use mo_bytecode::BigIntSign;
use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_def::limits::{MO_TIME_MAX_US, MO_TIME_MIN_US};
use mo_heap::decimal::{DECIMAL_MAX, DECIMAL_SCALE, DECIMAL_SCALE_U128};
use mo_heap::float::format_float;
use mo_heap::print::value_print;
use mo_heap::{Heap, Value};

use crate::PrimError;

fn canonical_nan() -> f64 {
    f64::from_bits(0x7ff8_0000_0000_0000)
}

fn bit_len(n: u128) -> u32 {
    128 - n.leading_zeros()
}

/// Exact port of `mo_prim_ratio_float` (`src/prim/number.c`).
fn ratio_float(negative: bool, n: u128, d: u128) -> f64 {
    let mut raw: u64 = if negative { 1u64 << 63 } else { 0 };
    if n != 0 {
        let mut e = (bit_len(n) as i32) - (bit_len(d) as i32);
        if (e >= 0 && n < (d << (e as u32))) || (e < 0 && (n << ((-e) as u32)) < d) {
            e -= 1;
        }
        let shift = 52 - e;
        let a = if shift >= 0 { n << (shift as u32) } else { n };
        let b = if shift < 0 { d << ((-shift) as u32) } else { d };
        let mut q = a / b;
        let r = a % b;
        if 2 * r > b || (2 * r == b && (q & 1) != 0) {
            q += 1;
        }
        raw |= (((e + 1022) as u64) << 52).wrapping_add(q as u64);
    }
    f64::from_bits(raw)
}

fn ten(places: u32) -> u128 {
    let mut r = 1u128;
    for _ in 0..places {
        r *= 10;
    }
    r
}

fn int_pow(mut a: i64, mut n: i64) -> Result<i64, PrimError> {
    if n < 0 {
        return Err(PrimError::Outside("Int.pow with a negative exponent"));
    }
    let mut result: i64 = 1;
    while n != 0 {
        if (n & 1) != 0 {
            result = result.checked_mul(a).ok_or(PrimError::Overflow("Int"))?;
        }
        n >>= 1;
        if n != 0 {
            a = a.checked_mul(a).ok_or(PrimError::Overflow("Int"))?;
        }
    }
    Ok(result)
}

fn format_fixed(negative: bool, n: u128, places: u32) -> String {
    let mut out = String::new();
    if negative && n != 0 {
        out.push('-');
    }
    let whole = n / DECIMAL_SCALE_U128;
    out.push_str(&whole.to_string());
    if places != 0 {
        out.push('.');
        let mut frac = n % DECIMAL_SCALE_U128;
        let mut divisor = DECIMAL_SCALE_U128 / 10;
        for _ in 0..places {
            out.push((b'0' + (frac / divisor) as u8) as char);
            frac %= divisor;
            divisor /= 10;
        }
    }
    out
}

fn float_to_decimal(a: f64) -> Option<i128> {
    if !a.is_finite() {
        return None;
    }
    let mut text = String::new();
    format_float(a, &mut text);
    let bytes = text.as_bytes();
    let negative = bytes.first() == Some(&b'-');
    let mut i = usize::from(negative);
    let mut coefficient: u128 = 0;
    let mut exponent: i32 = 18;
    let mut fraction = false;
    while i < bytes.len() && bytes[i] != b'e' {
        if bytes[i] == b'.' {
            fraction = true;
        } else {
            coefficient = coefficient * 10 + u128::from(bytes[i] - b'0');
            if fraction {
                exponent -= 1;
            }
        }
        i += 1;
    }
    if i < bytes.len() {
        i += 1;
        let minus = bytes[i] == b'-';
        if minus {
            i += 1;
        }
        let mut e: i32 = 0;
        while i < bytes.len() {
            e = e * 10 + i32::from(bytes[i] - b'0');
            i += 1;
        }
        exponent += if minus { -e } else { e };
    }
    for _ in 0..exponent {
        if coefficient > (DECIMAL_MAX as u128) / 10 {
            return None;
        }
        coefficient *= 10;
    }
    if exponent < -38 {
        coefficient = 0;
    } else if exponent < 0 {
        let divisor = ten((-exponent) as u32);
        let q = coefficient / divisor;
        let r = coefficient % divisor;
        coefficient = q + u128::from(2 * r > divisor || (2 * r == divisor && (q & 1) != 0));
    }
    let val = coefficient as i128;
    Some(if negative { -val } else { val })
}

fn parse_digits(s: &[u8], start: usize, count: usize) -> Option<i64> {
    let mut n = 0i64;
    for i in 0..count {
        let c = s[start + i];
        if !c.is_ascii_digit() {
            return None;
        }
        n = n * 10 + i64::from(c - b'0');
    }
    Some(n)
}

fn days_from_civil(mut year: i64, month: i64, day: i64) -> i64 {
    if month <= 2 {
        year -= 1;
    }
    let era = if year >= 0 {
        year / 400
    } else {
        (year - 399) / 400
    };
    let yoe = year - era * 400;
    let mp = if month > 2 { month - 3 } else { month + 9 };
    let doy = (153 * mp + 2) / 5 + day - 1;
    era * 146_097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719_468
}

fn parse_time(s: &[u8]) -> Option<i64> {
    let n = s.len();
    if !(20..=32).contains(&n)
        || s[4] != b'-'
        || s[7] != b'-'
        || (s[10] != b'T' && s[10] != b't')
        || s[13] != b':'
        || s[16] != b':'
    {
        return None;
    }
    let y = parse_digits(s, 0, 4)?;
    let m = parse_digits(s, 5, 2)?;
    let d = parse_digits(s, 8, 2)?;
    let hh = parse_digits(s, 11, 2)?;
    let mm = parse_digits(s, 14, 2)?;
    let ss = parse_digits(s, 17, 2)?;
    let leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    let last = match m {
        2 => {
            if leap {
                29
            } else {
                28
            }
        }
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    };
    if !(1..=12).contains(&m) || d < 1 || d > last || hh > 23 || mm > 59 || ss > 59 {
        return None;
    }
    let mut at = 19;
    let mut frac = 0i64;
    if at < n && s[at] == b'.' {
        at += 1;
        let mut places = 0u32;
        while at < n && s[at].is_ascii_digit() {
            if places == 6 {
                return None;
            }
            frac = frac * 10 + i64::from(s[at] - b'0');
            at += 1;
            places += 1;
        }
        if places == 0 {
            return None;
        }
        for _ in places..6 {
            frac *= 10;
        }
    }
    let offset = if n - at == 1 && (s[at] == b'Z' || s[at] == b'z') {
        0
    } else if n - at == 6 && (s[at] == b'+' || s[at] == b'-') && s[at + 3] == b':' {
        let oh = parse_digits(s, at + 1, 2)?;
        let om = parse_digits(s, at + 4, 2)?;
        if oh > 23 || om > 59 {
            return None;
        }
        (oh * 3600 + om * 60) * if s[at] == b'-' { -1 } else { 1 }
    } else {
        return None;
    };
    let t = i128::from(days_from_civil(y, m, d) * 86_400 + hh * 3600 + mm * 60 + ss - offset)
        * 1_000_000
        + i128::from(frac);
    if t < i128::from(MO_TIME_MIN_US) || t > i128::from(MO_TIME_MAX_US) {
        return None;
    }
    Some(t as i64)
}

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    params: &[u32],
    result_ty: u32,
    args: &[Value],
) -> Result<Value, PrimError> {
    match prim {
        PrimId::IntAbs => {
            let a = args[0].as_int();
            let v = a.checked_abs().ok_or(PrimError::Overflow("Int"))?;
            Ok(Value::Int(v))
        }
        PrimId::IntMin => Ok(Value::Int(args[0].as_int().min(args[1].as_int()))),
        PrimId::IntMax => Ok(Value::Int(args[0].as_int().max(args[1].as_int()))),
        PrimId::IntPow => Ok(Value::Int(int_pow(args[0].as_int(), args[1].as_int())?)),
        PrimId::IntFloorDiv | PrimId::IntFloorMod => {
            let a = args[0].as_int();
            let b = args[1].as_int();
            if b == 0 {
                return Err(PrimError::DivByZero("dividing by zero"));
            }
            if b == -1 {
                if prim == PrimId::IntFloorDiv {
                    let v = a.checked_neg().ok_or(PrimError::Overflow("Int"))?;
                    return Ok(Value::Int(v));
                }
                return Ok(Value::Int(0));
            }
            let mut q = a / b;
            let mut r = a % b;
            if r != 0 && ((r < 0) != (b < 0)) {
                q -= 1;
                r += b;
            }
            Ok(Value::Int(if prim == PrimId::IntFloorDiv { q } else { r }))
        }
        PrimId::IntWrappingAdd => Ok(Value::Int(args[0].as_int().wrapping_add(args[1].as_int()))),
        PrimId::IntWrappingSub => Ok(Value::Int(args[0].as_int().wrapping_sub(args[1].as_int()))),
        PrimId::IntWrappingMul => Ok(Value::Int(args[0].as_int().wrapping_mul(args[1].as_int()))),
        PrimId::IntSaturatingAdd => Ok(Value::Int(
            args[0].as_int().saturating_add(args[1].as_int()),
        )),
        PrimId::IntSaturatingSub => Ok(Value::Int(
            args[0].as_int().saturating_sub(args[1].as_int()),
        )),
        PrimId::IntSaturatingMul => Ok(Value::Int(
            args[0].as_int().saturating_mul(args[1].as_int()),
        )),
        PrimId::IntCheckedAdd => {
            let v = args[0]
                .as_int()
                .checked_add(args[1].as_int())
                .map(Value::Int);
            Ok(heap.make_option(types, result_ty, v)?)
        }
        PrimId::IntCheckedSub => {
            let v = args[0]
                .as_int()
                .checked_sub(args[1].as_int())
                .map(Value::Int);
            Ok(heap.make_option(types, result_ty, v)?)
        }
        PrimId::IntCheckedMul => {
            let v = args[0]
                .as_int()
                .checked_mul(args[1].as_int())
                .map(Value::Int);
            Ok(heap.make_option(types, result_ty, v)?)
        }
        PrimId::IntToDecimal => Ok(Value::Decimal(i128::from(args[0].as_int()) * DECIMAL_SCALE)),
        PrimId::IntToFloat => {
            let a = args[0].as_int();
            Ok(Value::Float(ratio_float(
                a < 0,
                a.unsigned_abs() as u128,
                1,
            )))
        }
        PrimId::IntToBig => {
            let a = args[0].as_int();
            let mut mag = a.unsigned_abs();
            let mut bytes = Vec::with_capacity(8);
            while mag != 0 {
                bytes.push((mag & 0xff) as u8);
                mag >>= 8;
            }
            let sign = if a == 0 {
                BigIntSign::Zero
            } else if a < 0 {
                BigIntSign::Negative
            } else {
                BigIntSign::Positive
            };
            Ok(heap.alloc_bigint(sign, bytes)?)
        }
        PrimId::IntToByte => {
            let a = args[0].as_int();
            let opt = u8::try_from(a).ok().map(Value::Byte);
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::IntToChar => {
            let a = args[0].as_int();
            let opt = u32::try_from(a)
                .ok()
                .and_then(char::from_u32)
                .map(Value::Char);
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::IntToText
        | PrimId::ByteToText
        | PrimId::DecimalToText
        | PrimId::FloatToText
        | PrimId::TimeToText
        | PrimId::DurationToText => {
            let text = value_print(types, params[0], &args[0]);
            Ok(heap.alloc_text(text)?)
        }
        PrimId::IntSeconds
        | PrimId::IntMilliseconds
        | PrimId::IntMinutes
        | PrimId::IntHours
        | PrimId::IntDays => {
            let scale: i128 = match prim {
                PrimId::IntSeconds => 1_000_000,
                PrimId::IntMilliseconds => 1_000,
                PrimId::IntMinutes => 60_000_000,
                PrimId::IntHours => 3_600_000_000,
                PrimId::IntDays => 86_400_000_000,
                _ => unreachable!(),
            };
            let prod = i128::from(args[0].as_int()) * scale;
            let dur = i64::try_from(prod).map_err(|_| PrimError::Overflow("Duration"))?;
            Ok(Value::Duration(dur))
        }

        PrimId::ByteToInt => Ok(Value::Int(i64::from(args[0].as_byte()))),

        PrimId::DecimalAbs => Ok(Value::Decimal(args[0].as_decimal().abs())),
        PrimId::DecimalMin => Ok(Value::Decimal(
            args[0].as_decimal().min(args[1].as_decimal()),
        )),
        PrimId::DecimalMax => Ok(Value::Decimal(
            args[0].as_decimal().max(args[1].as_decimal()),
        )),
        PrimId::DecimalToInt => {
            let a = args[0].as_decimal();
            let opt = if a % DECIMAL_SCALE == 0 {
                i64::try_from(a / DECIMAL_SCALE).ok().map(Value::Int)
            } else {
                None
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::DecimalToFloat => {
            let a = args[0].as_decimal();
            Ok(Value::Float(ratio_float(
                a < 0,
                a.unsigned_abs(),
                DECIMAL_SCALE_U128,
            )))
        }
        PrimId::DecimalRound | PrimId::DecimalToFixed => {
            let a = args[0].as_decimal();
            let p = args[1].as_int();
            if !(0..=18).contains(&p) {
                return Err(PrimError::Outside(if prim == PrimId::DecimalRound {
                    "Decimal.round with places outside 0 to 18"
                } else {
                    "Decimal.to_fixed with places outside 0 to 18"
                }));
            }
            let places = p as u32;
            let mag = a.unsigned_abs();
            let divisor = ten(18 - places);
            let rounded =
                (mag / divisor + u128::from(mag % divisor >= divisor / 2 + divisor % 2)) * divisor;
            if prim == PrimId::DecimalToFixed {
                return Ok(heap.alloc_text(format_fixed(a < 0, rounded, places))?);
            }
            if rounded > DECIMAL_MAX as u128 {
                return Err(PrimError::Overflow("Decimal"));
            }
            let v = rounded as i128;
            Ok(Value::Decimal(if a < 0 { -v } else { v }))
        }
        PrimId::DecimalFloor | PrimId::DecimalCeil => {
            let a = args[0].as_decimal();
            let mag = a.unsigned_abs();
            let away = if prim == PrimId::DecimalFloor {
                a < 0
            } else {
                a >= 0
            };
            let rounded = (mag / DECIMAL_SCALE_U128
                + u128::from(away && !mag.is_multiple_of(DECIMAL_SCALE_U128)))
                * DECIMAL_SCALE_U128;
            if rounded > DECIMAL_MAX as u128 {
                return Err(PrimError::Overflow("Decimal"));
            }
            let v = rounded as i128;
            Ok(Value::Decimal(if a < 0 { -v } else { v }))
        }

        PrimId::FloatAbs
        | PrimId::FloatFloor
        | PrimId::FloatCeil
        | PrimId::FloatRound
        | PrimId::FloatSqrt => {
            let a = args[0].as_float();
            let mut r = match prim {
                PrimId::FloatAbs => a.abs(),
                PrimId::FloatFloor => a.floor(),
                PrimId::FloatCeil => a.ceil(),
                PrimId::FloatRound => a.round(),
                PrimId::FloatSqrt => a.sqrt(),
                _ => unreachable!(),
            };
            if r.is_nan() {
                r = canonical_nan();
            }
            Ok(Value::Float(r))
        }
        PrimId::FloatIsNan => Ok(Value::Bool(args[0].as_float().is_nan())),
        PrimId::FloatIsInfinite => Ok(Value::Bool(args[0].as_float().is_infinite())),
        PrimId::FloatToDecimal => {
            let opt = float_to_decimal(args[0].as_float()).map(Value::Decimal);
            Ok(heap.make_option(types, result_ty, opt)?)
        }

        PrimId::TimeCheckedAdd => {
            let sum = i128::from(args[0].as_time()) + i128::from(args[1].as_duration());
            let opt = if sum >= i128::from(MO_TIME_MIN_US) && sum <= i128::from(MO_TIME_MAX_US) {
                Some(Value::Time(sum as i64))
            } else {
                None
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::TimeParse => {
            let opt = parse_time(args[0].as_text().as_bytes()).map(Value::Time);
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::DurationInMilliseconds => Ok(Value::Int(args[0].as_duration() / 1000)),
        PrimId::DurationInSeconds => Ok(Value::Decimal(
            i128::from(args[0].as_duration()) * 1_000_000_000_000,
        )),

        _ => unreachable!("unexpected numeric primitive {prim:?}"),
    }
}
