//! Scalar generation, shrinking, and BigInt reduction (`src/tools/inputs-scalars.c`).

use mo_bytecode::{BigIntSign, TypeKind};
use mo_def::limits::{MO_TIME_MAX_US, MO_TIME_MIN_US};
use mo_heap::{Heap, HeapObj, Value};

use super::build::{Builder, ValueRequest};
use super::{BuildMode, InputFactory, InputKind, InputLeafRequest, InputResult, input_random};

fn signed_word(word: u64) -> i64 {
    word as i64
}

fn random_int(builder: &mut Builder<'_>) -> i64 {
    let mut word = input_random(&mut builder.random);
    let bits = if builder.size < 60 {
        builder.size + 4
    } else {
        64
    };
    if bits < 64 {
        word &= (1u64 << bits) - 1;
        if (word & (1u64 << (bits - 1))) != 0 {
            word |= u64::MAX << bits;
        }
    }
    signed_word(word)
}

fn smaller(value: i128, variant: u32) -> i128 {
    debug_assert!(variant < 128);
    if variant == 0 {
        return 0;
    }
    let magnitude = value.unsigned_abs();
    if magnitude <= 1 || (variant > 1 && (magnitude >> (variant - 1)) <= 1) {
        return value;
    }
    let mut distance = magnitude >> variant;
    if distance == 0 {
        distance = 1;
    }
    let result = (magnitude - distance) as i128;
    if value < 0 { -result } else { result }
}

fn float_smaller(bits: u64, variant: u32) -> u64 {
    let sign = bits & (1u64 << 63);
    let magnitude = bits & !(1u64 << 63);
    if variant == 0 {
        return 0;
    }
    if magnitude >= 0x7ff0_0000_0000_0000 {
        return match variant {
            1 => sign | 0x3ff0_0000_0000_0000,
            2 => sign | 0x7fef_ffff_ffff_ffff,
            _ => bits,
        };
    }
    if variant <= 11 {
        let exponent = magnitude >> 52;
        let reduced = smaller(i128::from(exponent), variant) as u64;
        return sign | (magnitude - ((exponent - reduced) << 52));
    }
    sign | (smaller(i128::from(magnitude), variant - 11) as u64)
}

fn decimal_smaller(value: i128, variant: u32) -> i128 {
    if variant != 1 {
        return smaller(value, if variant == 0 { 0 } else { variant - 1 });
    }
    let mut magnitude = value.unsigned_abs();
    let mut digits = 0u32;
    while magnitude != 0 {
        magnitude /= 10;
        digits += 1;
    }
    let mut divisor: i128 = 1;
    for _ in 0..(digits / 2) {
        divisor *= 10;
    }
    value / divisor
}

fn immediate_equal(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Unit, Value::Unit) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Byte(x), Value::Byte(y)) => x == y,
        (Value::Int(x), Value::Int(y)) => x == y,
        (Value::Float(x), Value::Float(y)) => x.to_bits() == y.to_bits(),
        (Value::Char(x), Value::Char(y)) => x == y,
        (Value::Time(x), Value::Time(y)) => x == y,
        (Value::Duration(x), Value::Duration(y)) => x == y,
        (Value::Decimal(x), Value::Decimal(y)) => x == y,
        (Value::Range(s1, e1), Value::Range(s2, e2)) => s1 == s2 && e1 == e2,
        _ => false,
    }
}

pub(crate) fn immediate_build(
    builder: &mut Builder<'_>,
    request: &ValueRequest,
) -> Result<Value, InputResult> {
    const INTS: [i64; 5] = [0, 1, -1, i64::MIN, i64::MAX];
    const TIMES: [i64; 5] = [
        0,
        MO_TIME_MIN_US,
        -62_104_060_800_000_001,
        253_370_764_800_000_000,
        MO_TIME_MAX_US,
    ];
    const FLOATS: [u64; 11] = [
        0,
        0x3ff0_0000_0000_0000,
        0xbff0_0000_0000_0000,
        0x8000_0000_0000_0000,
        0x7fef_ffff_ffff_ffff,
        0xffef_ffff_ffff_ffff,
        0x7ff0_0000_0000_0000,
        0xfff0_0000_0000_0000,
        0x7ff8_0000_0000_0000,
        1,
        0x8000_0000_0000_0001,
    ];
    const CHARS: [u32; 6] = [0, b'a' as u32, 0xe9, 0xd7ff, 0xe000, 0x10_ffff];
    const MAX_DEC: i128 = i128::MAX;
    const DECIMALS: [i128; 6] = [
        0,
        1,
        1_000_000_000_000_000_000,
        -1_000_000_000_000_000_000,
        -MAX_DEC,
        MAX_DEC,
    ];
    const RANGES: [(i64, i64); 6] = [
        (0, 0),
        (0, 1),
        (1, 0),
        (-1, 1),
        (i64::MIN, i64::MAX),
        (i64::MAX, i64::MIN),
    ];
    const BYTES: [u8; 3] = [0, 1, u8::MAX];
    const DURATIONS: [i64; 3] = [0, i64::MIN, i64::MAX];

    let kind = builder.inputs.types.get(request.ty).kind;
    let random = request.mode == BuildMode::Prng;
    let index = if request.mode == BuildMode::Sample {
        request.boundary as usize
    } else {
        0
    };
    let copy = request.mode == BuildMode::Copy;
    if copy && request.edit.is_none() {
        return Ok(request.source.clone());
    }
    let variant = request.edit.map_or(0, |e| e.variant);
    let result = match kind {
        TypeKind::Unit => Value::Unit,
        TypeKind::Bool => Value::Bool(if copy {
            false
        } else if random {
            (input_random(&mut builder.random) & 1) != 0
        } else {
            index != 0
        }),
        TypeKind::Byte => Value::Byte(if copy {
            smaller(i128::from(request.source.as_byte()), variant) as u8
        } else if random {
            input_random(&mut builder.random) as u8
        } else {
            BYTES[index]
        }),
        TypeKind::Int => Value::Int(if copy {
            smaller(i128::from(request.source.as_int()), variant) as i64
        } else if random {
            random_int(builder)
        } else {
            INTS[index]
        }),
        TypeKind::Float => {
            let bits = if copy {
                float_smaller(request.source.as_float().to_bits(), variant)
            } else if random {
                input_random(&mut builder.random)
            } else {
                FLOATS[index]
            };
            Value::Float(f64::from_bits(bits))
        }
        TypeKind::Char => {
            let scalar = if copy {
                let s = smaller(i128::from(request.source.as_char() as u32), variant) as u32;
                if (0xd800..=0xdfff).contains(&s) {
                    0xd7ff
                } else {
                    s
                }
            } else if random {
                let mut s = (input_random(&mut builder.random) % 0x10_f800) as u32;
                if s >= 0xd800 {
                    s += 0x800;
                }
                s
            } else {
                CHARS[index]
            };
            #[expect(
                clippy::expect_used,
                reason = "surrogate gap 0xd800..=0xdfff is excluded in all branches"
            )]
            let ch = char::from_u32(scalar).expect("valid Unicode scalar");
            Value::Char(ch)
        }
        TypeKind::Time => Value::Time(if copy {
            smaller(i128::from(request.source.as_time()), variant) as i64
        } else if random {
            TIMES[1] + (input_random(&mut builder.random) % 315_537_897_600_000_000) as i64
        } else {
            TIMES[index]
        }),
        TypeKind::Duration => Value::Duration(if copy {
            smaller(i128::from(request.source.as_duration()), variant) as i64
        } else if random {
            random_int(builder)
        } else {
            DURATIONS[index]
        }),
        TypeKind::Decimal => {
            let value = if copy {
                decimal_smaller(request.source.as_decimal(), variant)
            } else if random {
                let cap = if builder.size < 37 {
                    builder.size + 1
                } else {
                    38
                };
                let digits = 1 + (input_random(&mut builder.random) % u64::from(cap)) as u32;
                let scale = (input_random(&mut builder.random) % 19) as u32;
                let mut magnitude: u128 = 0;
                for _ in 0..digits {
                    magnitude = magnitude * 10 + u128::from(input_random(&mut builder.random) % 10);
                }
                for _ in scale..18 {
                    if magnitude > (MAX_DEC as u128) / 10 {
                        break;
                    }
                    magnitude *= 10;
                }
                let v = magnitude as i128;
                if (input_random(&mut builder.random) & 1) != 0 {
                    -v
                } else {
                    v
                }
            } else {
                DECIMALS[index]
            };
            Value::Decimal(value)
        }
        TypeKind::Range => {
            let (start, end) = if copy {
                let (mut s, mut e) = request.source.as_range();
                if variant < 65 {
                    s = smaller(i128::from(s), variant) as i64;
                } else {
                    e = smaller(i128::from(e), variant - 65) as i64;
                }
                (s, e)
            } else if random {
                (random_int(builder), random_int(builder))
            } else {
                RANGES[index]
            };
            Value::Range(start, end)
        }
        _ => return Err(InputResult::Unsupported),
    };
    if copy && immediate_equal(&result, &request.source) {
        return Err(InputResult::Done);
    }
    Ok(result)
}

/// Checks or applies a BigInt shrink step (`src/tools/inputs-scalars.c:mo_input_bigint_smaller`).
pub(crate) fn bigint_smaller(source: &[u8], variant: u32, mut out: Option<&mut [u8]>) -> bool {
    let length = source.len();
    if length == 0 {
        return false;
    }
    if variant == 0 {
        if let Some(buf) = out {
            buf.fill(0);
        }
        return true;
    }
    if variant == 1 {
        let cut = length / 2;
        if cut == 0 {
            return false;
        }
        if let Some(buf) = out {
            buf[..length - cut].copy_from_slice(&source[cut..]);
            buf[length - cut..].fill(0);
        }
        return true;
    }
    let shift = (variant - 1) as usize;
    let whole = shift / 8;
    let part = shift % 8;
    if whole >= length || (whole == length - 1 && (source[whole] >> part) == 0) {
        return false;
    }
    let mut borrow: u32 = 0;
    for i in 0..length {
        let mut distance = if i + whole < length {
            u32::from(source[i + whole]) >> part
        } else {
            0
        };
        if part != 0 && i + whole + 1 < length {
            distance |= u32::from(source[i + whole + 1]) << (8 - part);
        }
        let subtract = (distance & 0xff) + borrow;
        let byte = (u32::from(source[i]).wrapping_sub(subtract) & 0xff) as u8;
        borrow = u32::from(u32::from(source[i]) < subtract);
        if let Some(ref mut buf) = out {
            buf[i] = byte;
        }
    }
    debug_assert_eq!(borrow, 0);
    true
}

fn blob_build(
    builder: &mut Builder<'_>,
    heap: &Heap,
    request: &ValueRequest,
) -> Result<Value, InputResult> {
    let kind = builder.inputs.types.get(request.ty).kind;
    let copy = request.mode == BuildMode::Copy;
    if copy {
        let (src_bytes, mut sign): (&[u8], BigIntSign) = match request.source.as_heap_ref().obj() {
            HeapObj::Text(s) => (s.as_bytes(), BigIntSign::Zero),
            HeapObj::Bytes(b) => (b.as_slice(), BigIntSign::Zero),
            HeapObj::BigInt { sign, magnitude } => (magnitude.as_slice(), *sign),
            _ => unreachable!("blob copy on non-blob"),
        };
        let mut length = src_bytes.len();
        if let Some(edit) = request.edit {
            if kind == TypeKind::BigInt {
                if length == 0 {
                    return Err(InputResult::Done);
                }
                if !builder.budget.work(length as u64) {
                    return Err(InputResult::Limit);
                }
                let buf = if edit.variant == 0 {
                    length = 0;
                    sign = BigIntSign::Zero;
                    Vec::new()
                } else {
                    let mut tmp = vec![0u8; length];
                    let changed = bigint_smaller(src_bytes, edit.variant, Some(&mut tmp));
                    debug_assert!(changed);
                    while length != 0 && tmp[length - 1] == 0 {
                        length -= 1;
                    }
                    tmp.truncate(length);
                    if length == 0 {
                        sign = BigIntSign::Zero;
                    }
                    tmp
                };
                if !builder.budget.bytes(length) {
                    return Err(InputResult::Limit);
                }
                return heap
                    .alloc_bigint(sign, buf)
                    .map_err(|_| InputResult::NoMemory);
            }
            let (start, end) = if kind == TypeKind::Text {
                if !builder.budget.work(length as u64) {
                    return Err(InputResult::Limit);
                }
                let mut scalar = 0u32;
                let mut s = length;
                let mut e = length;
                for (i, &b) in src_bytes.iter().enumerate() {
                    if (b & 0xc0) != 0x80 {
                        if scalar == edit.start {
                            s = i;
                        }
                        if scalar == edit.start + edit.count {
                            e = i;
                        }
                        scalar += 1;
                    }
                }
                (s, e)
            } else {
                (edit.start as usize, (edit.start + edit.count) as usize)
            };
            debug_assert!(start <= end && end <= length);
            let shortened = length - (end - start);
            if !builder.budget.bytes(shortened) {
                return Err(InputResult::Limit);
            }
            let mut out = Vec::with_capacity(shortened);
            out.extend_from_slice(&src_bytes[..start]);
            out.extend_from_slice(&src_bytes[end..]);
            return match kind {
                TypeKind::Text => {
                    #[expect(
                        clippy::expect_used,
                        reason = "UTF-8 slicing on scalar boundaries preserves valid UTF-8"
                    )]
                    let s = String::from_utf8(out).expect("valid UTF-8 slice");
                    heap.alloc_text(s).map_err(|_| InputResult::NoMemory)
                }
                TypeKind::Bytes => heap.alloc_bytes(out).map_err(|_| InputResult::NoMemory),
                _ => unreachable!(),
            };
        }
        if !builder.budget.bytes(length) {
            return Err(InputResult::Limit);
        }
        return match kind {
            TypeKind::Text => {
                let s = std::str::from_utf8(src_bytes)
                    .unwrap_or_default()
                    .to_string();
                heap.alloc_text(s).map_err(|_| InputResult::NoMemory)
            }
            TypeKind::Bytes => heap
                .alloc_bytes(src_bytes.to_vec())
                .map_err(|_| InputResult::NoMemory),
            TypeKind::BigInt => heap
                .alloc_bigint(sign, src_bytes.to_vec())
                .map_err(|_| InputResult::NoMemory),
            _ => unreachable!(),
        };
    }

    let index = if request.mode == BuildMode::Sample {
        request.boundary
    } else {
        0
    };
    let random = request.mode == BuildMode::Prng;
    let length = if random {
        let cap = (u64::from(request.size) * if kind == TypeKind::BigInt { 16 } else { 4 })
            .min(builder.budget.left.bytes as u64);
        (input_random(&mut builder.random) % (cap + 1)) as usize
    } else if index == 0 {
        0
    } else if index < 3 {
        1
    } else if kind == TypeKind::BigInt {
        128
    } else {
        1024
    };
    if !builder.budget.bytes(length) {
        return Err(InputResult::Limit);
    }
    let mut bytes = vec![0u8; length];
    if length != 0 {
        if random {
            for b in &mut bytes {
                *b = input_random(&mut builder.random) as u8;
            }
        } else {
            bytes.fill(if kind == TypeKind::Text {
                b'x'
            } else {
                u8::MAX
            });
        }
    }
    match kind {
        TypeKind::Text => {
            if !random && index == 1 {
                bytes[0] = b'a';
            }
            if !random && index == 2 {
                if !builder.budget.bytes(1) {
                    return Err(InputResult::Limit);
                }
                return heap
                    .alloc_text("\u{e9}".to_string())
                    .map_err(|_| InputResult::NoMemory);
            }
            if random {
                const SCALARS: [&[u8]; 4] = [
                    b"a",
                    "\u{e9}".as_bytes(),
                    "\u{20ac}".as_bytes(),
                    "\u{1f600}".as_bytes(),
                ];
                let mut i = 0;
                while i < length {
                    let mut scalar = (input_random(&mut builder.random) % 4) as usize;
                    if SCALARS[scalar].len() > length - i {
                        scalar = 0;
                    }
                    let slice = SCALARS[scalar];
                    bytes[i..i + slice.len()].copy_from_slice(slice);
                    i += slice.len();
                }
            }
            #[expect(
                clippy::expect_used,
                reason = "generated bytes consist exclusively of valid UTF-8 scalars"
            )]
            let s = String::from_utf8(bytes).expect("valid generated UTF-8");
            heap.alloc_text(s).map_err(|_| InputResult::NoMemory)
        }
        TypeKind::Bytes => {
            if !random && index == 1 {
                bytes[0] = 0;
            }
            heap.alloc_bytes(bytes).map_err(|_| InputResult::NoMemory)
        }
        TypeKind::BigInt => {
            let mut sign = BigIntSign::Zero;
            if length != 0 {
                if !random && index < 3 {
                    bytes[0] = 1;
                }
                if bytes[length - 1] == 0 {
                    bytes[length - 1] = 1;
                }
                sign = if random {
                    if (input_random(&mut builder.random) & 1) != 0 {
                        BigIntSign::Negative
                    } else {
                        BigIntSign::Positive
                    }
                } else if index == 2 || index == 4 {
                    BigIntSign::Negative
                } else {
                    BigIntSign::Positive
                };
            }
            heap.alloc_bigint(sign, bytes)
                .map_err(|_| InputResult::NoMemory)
        }
        _ => unreachable!(),
    }
}

pub(crate) fn scalar_build<'f>(
    builder: &mut Builder<'_>,
    heap: &Heap,
    request: &ValueRequest,
    factories: &mut [&mut (dyn InputFactory + 'f)],
) -> Result<Value, InputResult> {
    if let Some(fi) = builder.inputs.info[request.ty as usize].factory_index {
        let mut remaining = builder.budget.left;
        remaining.depth = request.remaining_depth;
        let result = if request.mode == BuildMode::Copy {
            if let Some(edit) = request.edit {
                factories[fi].shrink(heap, &request.source, edit.variant, &mut remaining)
            } else {
                factories[fi].copy(heap, &request.source, &mut remaining)
            }
        } else {
            let leaf = InputLeafRequest {
                kind: if request.mode == BuildMode::Prng {
                    InputKind::Random
                } else {
                    InputKind::Boundary
                },
                boundary: if request.mode == BuildMode::Sample {
                    request.boundary
                } else {
                    0
                },
                size: request.size,
                seed: input_random(&mut builder.random),
            };
            factories[fi].build(heap, &leaf, &mut remaining)
        };
        builder.budget.left.nodes = remaining.nodes;
        builder.budget.left.bytes = remaining.bytes;
        builder.budget.left.work = remaining.work;
        return result;
    }
    let kind = builder.inputs.types.get(request.ty).kind;
    if matches!(kind, TypeKind::Text | TypeKind::Bytes | TypeKind::BigInt) {
        blob_build(builder, heap, request)
    } else {
        immediate_build(builder, request)
    }
}
