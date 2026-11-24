//! Numeric, comparison, logical, and range VM opcodes (`src/vm/number.c` parity).

use mo_def::Opcode;
use mo_def::limits::{MO_TIME_MAX_US, MO_TIME_MIN_US};
use mo_heap::decimal::{decimal_add, decimal_div, decimal_mul, decimal_rem, decimal_sub};
use mo_heap::{InlinePayload, Value};

pub fn eval_number(op: Opcode, a: &Value, b: Option<&Value>) -> Result<Value, &'static str> {
    match op {
        Opcode::NegInt => {
            return a
                .as_int()
                .checked_neg()
                .map(Value::Int)
                .ok_or("overflow: Int");
        }
        Opcode::NegDecimal => return Ok(Value::Decimal(-a.as_decimal())),
        Opcode::NegFloat => return Ok(Value::Float(-a.as_float())),
        Opcode::NegDuration => {
            return a
                .as_duration()
                .checked_neg()
                .map(Value::Duration)
                .ok_or("overflow: Duration");
        }
        Opcode::Not => return Ok(Value::Bool(!a.as_bool())),
        Opcode::RangeStart => return Ok(Value::Int(a.as_range().0)),
        Opcode::RangeEnd => return Ok(Value::Int(a.as_range().1)),
        _ => {}
    }

    let b = b.ok_or("internal error: missing binary operand")?;
    match op {
        // Int arithmetic
        Opcode::AddInt => a
            .as_int()
            .checked_add(b.as_int())
            .map(Value::Int)
            .ok_or("overflow: Int"),
        Opcode::SubInt => a
            .as_int()
            .checked_sub(b.as_int())
            .map(Value::Int)
            .ok_or("overflow: Int"),
        Opcode::MulInt => a
            .as_int()
            .checked_mul(b.as_int())
            .map(Value::Int)
            .ok_or("overflow: Int"),
        Opcode::DivInt => {
            let x = a.as_int();
            let y = b.as_int();
            if y == 0 {
                return Err("dividing by zero");
            }
            x.checked_div(y).map(Value::Int).ok_or("overflow: Int")
        }
        Opcode::RemInt => {
            let x = a.as_int();
            let y = b.as_int();
            if y == 0 {
                return Err("dividing by zero");
            }
            if x == i64::MIN && y == -1 {
                return Ok(Value::Int(0));
            }
            Ok(Value::Int(x % y))
        }

        // Decimal arithmetic
        Opcode::AddDecimal => decimal_add(a.as_decimal(), b.as_decimal())
            .map(Value::Decimal)
            .ok_or("overflow: Decimal"),
        Opcode::SubDecimal => decimal_sub(a.as_decimal(), b.as_decimal())
            .map(Value::Decimal)
            .ok_or("overflow: Decimal"),
        Opcode::MulDecimal => decimal_mul(a.as_decimal(), b.as_decimal())
            .map(Value::Decimal)
            .ok_or("overflow: Decimal"),
        Opcode::DivDecimal => {
            let y = b.as_decimal();
            if y == 0 {
                return Err("dividing by zero");
            }
            decimal_div(a.as_decimal(), y)
                .map(Value::Decimal)
                .ok_or("overflow: Decimal")
        }
        Opcode::RemDecimal => {
            let y = b.as_decimal();
            if y == 0 {
                return Err("dividing by zero");
            }
            Ok(Value::Decimal(decimal_rem(a.as_decimal(), y)))
        }

        // Float arithmetic
        Opcode::AddFloat => Ok(Value::Float(a.as_float() + b.as_float())),
        Opcode::SubFloat => Ok(Value::Float(a.as_float() - b.as_float())),
        Opcode::MulFloat => Ok(Value::Float(a.as_float() * b.as_float())),
        Opcode::DivFloat => Ok(Value::Float(a.as_float() / b.as_float())),
        Opcode::RemFloat => Ok(Value::Float(a.as_float() % b.as_float())),

        // Duration and Time arithmetic
        Opcode::AddDuration => a
            .as_duration()
            .checked_add(b.as_duration())
            .map(Value::Duration)
            .ok_or("overflow: Duration"),
        Opcode::SubDuration => a
            .as_duration()
            .checked_sub(b.as_duration())
            .map(Value::Duration)
            .ok_or("overflow: Duration"),
        Opcode::ScaleDuration => a
            .as_duration()
            .checked_mul(b.as_int())
            .map(Value::Duration)
            .ok_or("overflow: Duration"),
        Opcode::AddTime => {
            let sum = i128::from(a.as_time()) + i128::from(b.as_duration());
            if sum < i128::from(MO_TIME_MIN_US) || sum > i128::from(MO_TIME_MAX_US) {
                return Err("time outside years 1 to 9999");
            }
            Ok(Value::Time(sum as i64))
        }
        Opcode::SubTime => {
            let diff = i128::from(a.as_time()) - i128::from(b.as_duration());
            if diff < i128::from(MO_TIME_MIN_US) || diff > i128::from(MO_TIME_MAX_US) {
                return Err("time outside years 1 to 9999");
            }
            Ok(Value::Time(diff as i64))
        }
        Opcode::DiffTime => {
            let diff = i128::from(a.as_time()) - i128::from(b.as_time());
            i64::try_from(diff)
                .map(Value::Duration)
                .map_err(|_| "overflow: Duration")
        }

        // Comparisons: EQ
        Opcode::EqInt => Ok(Value::Bool(a.as_int() == b.as_int())),
        Opcode::EqDecimal => Ok(Value::Bool(a.as_decimal() == b.as_decimal())),
        Opcode::EqFloat => Ok(Value::Bool(a.as_float() == b.as_float())),
        Opcode::EqBool => Ok(Value::Bool(a.as_bool() == b.as_bool())),
        Opcode::EqByte => Ok(Value::Bool(a.as_byte() == b.as_byte())),
        Opcode::EqChar => Ok(Value::Bool(a.as_char() == b.as_char())),
        Opcode::EqTime => Ok(Value::Bool(a.as_time() == b.as_time())),
        Opcode::EqDuration => Ok(Value::Bool(a.as_duration() == b.as_duration())),
        Opcode::EqChoice => Ok(Value::Bool(a.as_tag() == b.as_tag())),

        // Comparisons: LT
        Opcode::LtInt => Ok(Value::Bool(a.as_int() < b.as_int())),
        Opcode::LtDecimal => Ok(Value::Bool(a.as_decimal() < b.as_decimal())),
        Opcode::LtFloat => Ok(Value::Bool(a.as_float() < b.as_float())),
        Opcode::LtBool => Ok(Value::Bool(!a.as_bool() & b.as_bool())),
        Opcode::LtByte => Ok(Value::Bool(a.as_byte() < b.as_byte())),
        Opcode::LtChar => Ok(Value::Bool(a.as_char() < b.as_char())),
        Opcode::LtTime => Ok(Value::Bool(a.as_time() < b.as_time())),
        Opcode::LtDuration => Ok(Value::Bool(a.as_duration() < b.as_duration())),
        Opcode::LtChoice => Ok(Value::Bool(a.as_tag() < b.as_tag())),

        // Comparisons: LE
        Opcode::LeInt => Ok(Value::Bool(a.as_int() <= b.as_int())),
        Opcode::LeDecimal => Ok(Value::Bool(a.as_decimal() <= b.as_decimal())),
        Opcode::LeFloat => Ok(Value::Bool(a.as_float() <= b.as_float())),
        Opcode::LeBool => Ok(Value::Bool(a.as_bool() <= b.as_bool())),
        Opcode::LeByte => Ok(Value::Bool(a.as_byte() <= b.as_byte())),
        Opcode::LeChar => Ok(Value::Bool(a.as_char() <= b.as_char())),
        Opcode::LeTime => Ok(Value::Bool(a.as_time() <= b.as_time())),
        Opcode::LeDuration => Ok(Value::Bool(a.as_duration() <= b.as_duration())),
        Opcode::LeChoice => Ok(Value::Bool(a.as_tag() <= b.as_tag())),

        // Float total-order compare (NaN > any non-NaN, NaN == NaN)
        Opcode::CmpFloat => {
            let x = a.as_float();
            let y = b.as_float();
            let tag = if x.is_nan() {
                if y.is_nan() { 1 } else { 2 }
            } else if y.is_nan() || x < y {
                0
            } else if x > y {
                2
            } else {
                1
            };
            Ok(Value::ChoiceInline {
                tag,
                payload: InlinePayload::Empty,
            })
        }

        // Range construction
        Opcode::MakeRange => Ok(Value::Range(a.as_int(), b.as_int())),

        _ => unreachable!("non-numeric opcode {op:?}"),
    }
}
