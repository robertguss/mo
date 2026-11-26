//! Mapping from Mo Core primitives (`mo_core::Prim`) to bytecode opcodes,
//! `PrimId`, `HostOpId`, and inline expansions matching `src/codegen/operations.c`.

use mo_core::{IoOp, ListOp, MapOp, Prim};
use mo_def::core_ops::{
    CoreBase, CoreBigOp, CoreByteOp, CoreBytesOp, CoreCharOp, CoreCmpOp, CoreCryptoOp, CoreDecOp,
    CoreFloatOp, CoreIntOp, CoreIoNullaryOp, CoreJsonOp, CoreListNullaryOp, CoreMapNullaryOp,
    CoreRangeOp, CoreSetOp, CoreTextOp, CoreTimeOp, CoreUnicodeOp,
};
use mo_def::{HostOpId, Opcode, PrimId};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Expansion {
    #[default]
    None,
    RangeToText,
    ListUncons,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LiteralOp {
    #[default]
    None,
    List,
    Map,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Operation {
    pub opcode: Option<Opcode>,
    pub row: u32,
    pub signature: &'static str,
    pub params: &'static str,
    pub expansion: Expansion,
    pub literal: LiteralOp,
    pub compare: bool,
    pub reverse: bool,
}

impl Default for Operation {
    fn default() -> Self {
        Self {
            opcode: None,
            row: 0,
            signature: "",
            params: "",
            expansion: Expansion::None,
            literal: LiteralOp::None,
            compare: false,
            reverse: false,
        }
    }
}

#[must_use]
pub const fn primitive_op(id: PrimId) -> Operation {
    let info = id.info();
    Operation {
        opcode: Some(Opcode::Prim),
        row: id as u32,
        signature: info.signature,
        params: info.params,
        expansion: Expansion::None,
        literal: LiteralOp::None,
        compare: false,
        reverse: false,
    }
}

#[must_use]
pub const fn host_op(id: HostOpId) -> Operation {
    let info = id.info();
    Operation {
        opcode: Some(Opcode::Host),
        row: id as u32,
        signature: info.signature,
        params: info.params,
        expansion: Expansion::None,
        literal: LiteralOp::None,
        compare: false,
        reverse: false,
    }
}

const fn native(opcode: Opcode, signature: &'static str, params: &'static str) -> Operation {
    Operation {
        opcode: Some(opcode),
        row: 0,
        signature,
        params,
        expansion: Expansion::None,
        literal: LiteralOp::None,
        compare: false,
        reverse: false,
    }
}

fn comparison(base: CoreBase, cmp_op: CoreCmpOp) -> Operation {
    let inclusive = matches!(cmp_op, CoreCmpOp::Le | CoreCmpOp::Ge);
    let reverse = matches!(cmp_op, CoreCmpOp::Gt | CoreCmpOp::Ge);
    let mut out = match base {
        CoreBase::BigInt => {
            let mut op = primitive_op(PrimId::BigintCompare);
            op.compare = true;
            op
        }
        CoreBase::Text => {
            let mut op = primitive_op(PrimId::TextCompare);
            op.compare = true;
            op
        }
        CoreBase::Bytes => {
            let mut op = primitive_op(PrimId::BytesCompare);
            op.compare = true;
            op
        }
        CoreBase::Int => native(
            if inclusive {
                Opcode::LeInt
            } else {
                Opcode::LtInt
            },
            "fn(Int, Int) : Bool",
            "bb",
        ),
        CoreBase::Byte => native(
            if inclusive {
                Opcode::LeByte
            } else {
                Opcode::LtByte
            },
            "fn(Byte, Byte) : Bool",
            "bb",
        ),
        CoreBase::Decimal => native(
            if inclusive {
                Opcode::LeDecimal
            } else {
                Opcode::LtDecimal
            },
            "fn(Decimal, Decimal) : Bool",
            "bb",
        ),
        CoreBase::Float => native(
            if inclusive {
                Opcode::LeFloat
            } else {
                Opcode::LtFloat
            },
            "fn(Float, Float) : Bool",
            "bb",
        ),
        CoreBase::Bool => native(
            if inclusive {
                Opcode::LeBool
            } else {
                Opcode::LtBool
            },
            "fn(Bool, Bool) : Bool",
            "bb",
        ),
        CoreBase::Char => native(
            if inclusive {
                Opcode::LeChar
            } else {
                Opcode::LtChar
            },
            "fn(Char, Char) : Bool",
            "bb",
        ),
        CoreBase::Time => native(
            if inclusive {
                Opcode::LeTime
            } else {
                Opcode::LtTime
            },
            "fn(Time, Time) : Bool",
            "bb",
        ),
        CoreBase::Duration => native(
            if inclusive {
                Opcode::LeDuration
            } else {
                Opcode::LtDuration
            },
            "fn(Duration, Duration) : Bool",
            "bb",
        ),
    };
    out.reverse = reverse;
    out
}

/// Resolves a non-simulation Core [`Prim`] to its bytecode [`Operation`].
/// Returns `None` for `Prim::Sim`, which is handled directly in `prepare.rs` and `generate.rs`.
#[must_use]
pub fn resolve_operation(prim: &Prim) -> Option<Operation> {
    let id = match prim {
        Prim::Sim(_) => return None,
        Prim::Int(op) => match op {
            CoreIntOp::Add => return Some(native(Opcode::AddInt, "fn(Int, Int) : Int", "bb")),
            CoreIntOp::Sub => return Some(native(Opcode::SubInt, "fn(Int, Int) : Int", "bb")),
            CoreIntOp::Mul => return Some(native(Opcode::MulInt, "fn(Int, Int) : Int", "bb")),
            CoreIntOp::Div => return Some(native(Opcode::DivInt, "fn(Int, Int) : Int", "bb")),
            CoreIntOp::Rem => return Some(native(Opcode::RemInt, "fn(Int, Int) : Int", "bb")),
            CoreIntOp::Neg => return Some(native(Opcode::NegInt, "fn(Int) : Int", "b")),
            CoreIntOp::Abs => PrimId::IntAbs,
            CoreIntOp::Min => PrimId::IntMin,
            CoreIntOp::Max => PrimId::IntMax,
            CoreIntOp::Pow => PrimId::IntPow,
            CoreIntOp::FloorDiv => PrimId::IntFloorDiv,
            CoreIntOp::FloorMod => PrimId::IntFloorMod,
            CoreIntOp::CheckedAdd => PrimId::IntCheckedAdd,
            CoreIntOp::CheckedSub => PrimId::IntCheckedSub,
            CoreIntOp::CheckedMul => PrimId::IntCheckedMul,
            CoreIntOp::WrappingAdd => PrimId::IntWrappingAdd,
            CoreIntOp::WrappingSub => PrimId::IntWrappingSub,
            CoreIntOp::WrappingMul => PrimId::IntWrappingMul,
            CoreIntOp::SaturatingAdd => PrimId::IntSaturatingAdd,
            CoreIntOp::SaturatingSub => PrimId::IntSaturatingSub,
            CoreIntOp::SaturatingMul => PrimId::IntSaturatingMul,
            CoreIntOp::ToText => PrimId::IntToText,
            CoreIntOp::ToDecimal => PrimId::IntToDecimal,
            CoreIntOp::ToBig => PrimId::IntToBig,
            CoreIntOp::ToFloat => PrimId::IntToFloat,
            CoreIntOp::ToChar => PrimId::IntToChar,
            CoreIntOp::ToByte => PrimId::IntToByte,
            CoreIntOp::Seconds => PrimId::IntSeconds,
            CoreIntOp::Milliseconds => PrimId::IntMilliseconds,
            CoreIntOp::Minutes => PrimId::IntMinutes,
            CoreIntOp::Hours => PrimId::IntHours,
            CoreIntOp::Days => PrimId::IntDays,
        },
        Prim::Big(op) => match op {
            CoreBigOp::Add => PrimId::BigintAdd,
            CoreBigOp::Sub => PrimId::BigintSub,
            CoreBigOp::Mul => PrimId::BigintMul,
            CoreBigOp::Div => PrimId::BigintDiv,
            CoreBigOp::Rem => PrimId::BigintRem,
            CoreBigOp::Neg => PrimId::BigintNeg,
            CoreBigOp::Abs => PrimId::BigintAbs,
            CoreBigOp::Min => PrimId::BigintMin,
            CoreBigOp::Max => PrimId::BigintMax,
            CoreBigOp::Pow => PrimId::BigintPow,
            CoreBigOp::FloorDiv => PrimId::BigintFloorDiv,
            CoreBigOp::FloorMod => PrimId::BigintFloorMod,
            CoreBigOp::ToInt => PrimId::BigintToInt,
            CoreBigOp::ToText => PrimId::BigintToText,
        },
        Prim::Byte(op) => match op {
            CoreByteOp::ToInt => PrimId::ByteToInt,
            CoreByteOp::ToText => PrimId::ByteToText,
        },
        Prim::Dec(op) => match op {
            CoreDecOp::Add => {
                return Some(native(
                    Opcode::AddDecimal,
                    "fn(Decimal, Decimal) : Decimal",
                    "bb",
                ));
            }
            CoreDecOp::Sub => {
                return Some(native(
                    Opcode::SubDecimal,
                    "fn(Decimal, Decimal) : Decimal",
                    "bb",
                ));
            }
            CoreDecOp::Mul => {
                return Some(native(
                    Opcode::MulDecimal,
                    "fn(Decimal, Decimal) : Decimal",
                    "bb",
                ));
            }
            CoreDecOp::Div => {
                return Some(native(
                    Opcode::DivDecimal,
                    "fn(Decimal, Decimal) : Decimal",
                    "bb",
                ));
            }
            CoreDecOp::Rem => {
                return Some(native(
                    Opcode::RemDecimal,
                    "fn(Decimal, Decimal) : Decimal",
                    "bb",
                ));
            }
            CoreDecOp::Neg => {
                return Some(native(Opcode::NegDecimal, "fn(Decimal) : Decimal", "b"));
            }
            CoreDecOp::Abs => PrimId::DecimalAbs,
            CoreDecOp::Min => PrimId::DecimalMin,
            CoreDecOp::Max => PrimId::DecimalMax,
            CoreDecOp::Floor => PrimId::DecimalFloor,
            CoreDecOp::Ceil => PrimId::DecimalCeil,
            CoreDecOp::Round => PrimId::DecimalRound,
            CoreDecOp::ToInt => PrimId::DecimalToInt,
            CoreDecOp::ToText => PrimId::DecimalToText,
            CoreDecOp::ToFixed => PrimId::DecimalToFixed,
            CoreDecOp::ToFloat => PrimId::DecimalToFloat,
        },
        Prim::Float(op) => match op {
            CoreFloatOp::Add => {
                return Some(native(Opcode::AddFloat, "fn(Float, Float) : Float", "bb"));
            }
            CoreFloatOp::Sub => {
                return Some(native(Opcode::SubFloat, "fn(Float, Float) : Float", "bb"));
            }
            CoreFloatOp::Mul => {
                return Some(native(Opcode::MulFloat, "fn(Float, Float) : Float", "bb"));
            }
            CoreFloatOp::Div => {
                return Some(native(Opcode::DivFloat, "fn(Float, Float) : Float", "bb"));
            }
            CoreFloatOp::Rem => {
                return Some(native(Opcode::RemFloat, "fn(Float, Float) : Float", "bb"));
            }
            CoreFloatOp::Neg => return Some(native(Opcode::NegFloat, "fn(Float) : Float", "b")),
            CoreFloatOp::Abs => PrimId::FloatAbs,
            CoreFloatOp::Sqrt => PrimId::FloatSqrt,
            CoreFloatOp::Floor => PrimId::FloatFloor,
            CoreFloatOp::Ceil => PrimId::FloatCeil,
            CoreFloatOp::Round => PrimId::FloatRound,
            CoreFloatOp::IsNan => PrimId::FloatIsNan,
            CoreFloatOp::IsInfinite => PrimId::FloatIsInfinite,
            CoreFloatOp::ToDecimal => PrimId::FloatToDecimal,
            CoreFloatOp::ToText => PrimId::FloatToText,
        },
        Prim::Not => return Some(native(Opcode::Not, "fn(Bool) : Bool", "b")),
        Prim::Cmp { base, op } => return Some(comparison(*base, *op)),
        Prim::Char(op) => match op {
            CoreCharOp::Code => PrimId::CharCode,
            CoreCharOp::IsDigit => PrimId::CharIsAsciiDigit,
            CoreCharOp::IsLetter => PrimId::CharIsAsciiLetter,
            CoreCharOp::IsWhiteSpace => PrimId::CharIsAsciiWhiteSpace,
            CoreCharOp::Lower => PrimId::CharAsciiLowercase,
            CoreCharOp::Upper => PrimId::CharAsciiUppercase,
            CoreCharOp::ToText => PrimId::CharToText,
            CoreCharOp::Quoted => PrimId::CharQuoted,
        },
        Prim::Text(op) => match op {
            CoreTextOp::Concat => PrimId::TextConcat,
            CoreTextOp::ByteCount => PrimId::TextByteCount,
            CoreTextOp::CharCount => PrimId::TextCharCount,
            CoreTextOp::IsEmpty => PrimId::TextIsEmpty,
            CoreTextOp::Chars => PrimId::TextChars,
            CoreTextOp::FromChars => PrimId::TextFromChars,
            CoreTextOp::Lines => PrimId::TextLines,
            CoreTextOp::Words => PrimId::TextWords,
            CoreTextOp::Split => PrimId::TextSplit,
            CoreTextOp::Trim => PrimId::TextTrim,
            CoreTextOp::TrimStart => PrimId::TextTrimStart,
            CoreTextOp::TrimEnd => PrimId::TextTrimEnd,
            CoreTextOp::StartsWith => PrimId::TextStartsWith,
            CoreTextOp::EndsWith => PrimId::TextEndsWith,
            CoreTextOp::Contains => PrimId::TextContains,
            CoreTextOp::DropPrefix => PrimId::TextDropPrefix,
            CoreTextOp::DropSuffix => PrimId::TextDropSuffix,
            CoreTextOp::Replace => PrimId::TextReplace,
            CoreTextOp::Repeat => PrimId::TextRepeat,
            CoreTextOp::PadLeft => PrimId::TextPadLeft,
            CoreTextOp::PadRight => PrimId::TextPadRight,
            CoreTextOp::Lower => PrimId::TextAsciiLowercase,
            CoreTextOp::Upper => PrimId::TextAsciiUppercase,
            CoreTextOp::ToInt => PrimId::TextToInt,
            CoreTextOp::ToDecimal => PrimId::TextToDecimal,
            CoreTextOp::ToFloat => PrimId::TextToFloat,
            CoreTextOp::Join => PrimId::ListJoin,
            CoreTextOp::Bytes => PrimId::TextBytes,
            CoreTextOp::Quoted => PrimId::TextQuoted,
        },
        Prim::Bytes(op) => match op {
            CoreBytesOp::Size => PrimId::BytesSize,
            CoreBytesOp::Get => PrimId::BytesGet,
            CoreBytesOp::DecodeUtf8 => PrimId::BytesDecodeUtf8,
            CoreBytesOp::Slice => PrimId::BytesSlice,
            CoreBytesOp::Join => PrimId::BytesJoin,
            CoreBytesOp::ToHex => PrimId::BytesToHex,
            CoreBytesOp::FromHex => PrimId::BytesFromHex,
            CoreBytesOp::ToBase64url => PrimId::BytesToBase64url,
            CoreBytesOp::FromBase64url => PrimId::BytesFromBase64url,
            CoreBytesOp::Empty => PrimId::BytesEmpty,
            CoreBytesOp::Zeroed => PrimId::BytesZeroed,
            CoreBytesOp::FromList => PrimId::BytesFromList,
            CoreBytesOp::ToList => PrimId::BytesToList,
            CoreBytesOp::FromU16Be => PrimId::BytesFromU16Be,
            CoreBytesOp::FromU32Be => PrimId::BytesFromU32Be,
            CoreBytesOp::FromI64Be => PrimId::BytesFromI64Be,
            CoreBytesOp::U16BeAt => PrimId::BytesU16BeAt,
            CoreBytesOp::U32BeAt => PrimId::BytesU32BeAt,
            CoreBytesOp::I64BeAt => PrimId::BytesI64BeAt,
            CoreBytesOp::IsEmpty => PrimId::BytesIsEmpty,
            CoreBytesOp::CompareSliceAt => PrimId::BytesCompareSliceAt,
        },
        Prim::Unicode16(op) => match op {
            CoreUnicodeOp::Graphemes => PrimId::Unicode16Graphemes,
            CoreUnicodeOp::DisplayWidth => PrimId::Unicode16DisplayWidth,
            CoreUnicodeOp::NormalizeNfc => PrimId::Unicode16NormalizeNfc,
            CoreUnicodeOp::Lowercase => PrimId::Unicode16Lowercase,
            CoreUnicodeOp::Uppercase => PrimId::Unicode16Uppercase,
            CoreUnicodeOp::Letter => PrimId::Unicode16Letter,
            CoreUnicodeOp::Digit => PrimId::Unicode16Digit,
        },
        Prim::Crypto(op) => match op {
            CoreCryptoOp::Sha256 => PrimId::CryptoSha256,
            CoreCryptoOp::HmacSha256 => PrimId::CryptoHmacSha256,
            CoreCryptoOp::ConstantTimeEq => PrimId::CryptoConstantTimeEq,
            CoreCryptoOp::SealRaw => PrimId::CryptoSealRaw,
            CoreCryptoOp::OpenRaw => PrimId::CryptoOpenRaw,
            CoreCryptoOp::HashPasswordRaw => PrimId::CryptoHashPasswordRaw,
            CoreCryptoOp::VerifyPassword => PrimId::CryptoVerifyPassword,
        },
        Prim::Json(op) => match op {
            CoreJsonOp::QuoteRaw => PrimId::JsonQuoteRaw,
            CoreJsonOp::ParseRaw => PrimId::JsonParseRaw,
        },
        Prim::Range(op) => match op {
            CoreRangeOp::Make => {
                return Some(native(Opcode::MakeRange, "fn(Int, Int) : Range", "bb"));
            }
            CoreRangeOp::Start => return Some(native(Opcode::RangeStart, "fn(Range) : Int", "b")),
            CoreRangeOp::End => return Some(native(Opcode::RangeEnd, "fn(Range) : Int", "b")),
            CoreRangeOp::ToText => {
                return Some(Operation {
                    opcode: None,
                    row: 0,
                    signature: "fn(Range) : Text",
                    params: "b",
                    expansion: Expansion::RangeToText,
                    literal: LiteralOp::None,
                    compare: false,
                    reverse: false,
                });
            }
            CoreRangeOp::ToList => PrimId::RangeToList,
            CoreRangeOp::Contains => PrimId::RangeContains,
        },
        Prim::Time(op) => match op {
            CoreTimeOp::Sub => {
                return Some(native(Opcode::DiffTime, "fn(Time, Time) : Duration", "bb"));
            }
            CoreTimeOp::Add => {
                return Some(native(Opcode::AddTime, "fn(Time, Duration) : Time", "bb"));
            }
            CoreTimeOp::SubDur => {
                return Some(native(Opcode::SubTime, "fn(Time, Duration) : Time", "bb"));
            }
            CoreTimeOp::DurAdd => {
                return Some(native(
                    Opcode::AddDuration,
                    "fn(Duration, Duration) : Duration",
                    "bb",
                ));
            }
            CoreTimeOp::DurSub => {
                return Some(native(
                    Opcode::SubDuration,
                    "fn(Duration, Duration) : Duration",
                    "bb",
                ));
            }
            CoreTimeOp::DurMul => {
                return Some(native(
                    Opcode::ScaleDuration,
                    "fn(Duration, Int) : Duration",
                    "bb",
                ));
            }
            CoreTimeOp::DurNeg => {
                return Some(native(Opcode::NegDuration, "fn(Duration) : Duration", "b"));
            }
            CoreTimeOp::CheckedAdd => PrimId::TimeCheckedAdd,
            CoreTimeOp::ToText => PrimId::TimeToText,
            CoreTimeOp::Parse => PrimId::TimeParse,
            CoreTimeOp::InMilliseconds => PrimId::DurationInMilliseconds,
            CoreTimeOp::InSeconds => PrimId::DurationInSeconds,
            CoreTimeOp::DurToText => PrimId::DurationToText,
        },
        Prim::List { op, .. } => match op {
            ListOp::Lit(_) => {
                let mut out = primitive_op(PrimId::ListAppend);
                out.literal = LiteralOp::List;
                return Some(out);
            }
            ListOp::Zip(_) => return Some(primitive_op(PrimId::ListZip)),
            ListOp::Nullary(n) => match n {
                CoreListNullaryOp::Uncons => {
                    return Some(Operation {
                        opcode: None,
                        row: 0,
                        signature: "fn(List(T)) : Option((T, List(T)))",
                        params: "o",
                        expansion: Expansion::ListUncons,
                        literal: LiteralOp::None,
                        compare: false,
                        reverse: false,
                    });
                }
                CoreListNullaryOp::Size => PrimId::ListSize,
                CoreListNullaryOp::IsEmpty => PrimId::ListIsEmpty,
                CoreListNullaryOp::Get => PrimId::ListGet,
                CoreListNullaryOp::First => PrimId::ListFirst,
                CoreListNullaryOp::Last => PrimId::ListLast,
                CoreListNullaryOp::Append => PrimId::ListAppend,
                CoreListNullaryOp::Prepend => PrimId::ListPrepend,
                CoreListNullaryOp::Concat => PrimId::ListConcat,
                CoreListNullaryOp::Reverse => PrimId::ListReverse,
                CoreListNullaryOp::Take => PrimId::ListTake,
                CoreListNullaryOp::Drop => PrimId::ListDrop,
                CoreListNullaryOp::Set => PrimId::ListSet,
                CoreListNullaryOp::RemoveAt => PrimId::ListRemoveAt,
                CoreListNullaryOp::Enumerate => PrimId::ListEnumerate,
                CoreListNullaryOp::Contains => PrimId::ListContains,
                CoreListNullaryOp::Unique => PrimId::ListUnique,
            },
        },
        Prim::Map { op, .. } => match op {
            MapOp::Lit(_) => {
                let mut out = primitive_op(PrimId::MapLiteralInsert);
                out.literal = LiteralOp::Map;
                return Some(out);
            }
            MapOp::Nullary(n) => match n {
                CoreMapNullaryOp::Empty => PrimId::MapEmpty,
                CoreMapNullaryOp::Size => PrimId::MapSize,
                CoreMapNullaryOp::IsEmpty => PrimId::MapIsEmpty,
                CoreMapNullaryOp::Get => PrimId::MapGet,
                CoreMapNullaryOp::Has => PrimId::MapHas,
                CoreMapNullaryOp::Set => PrimId::MapSet,
                CoreMapNullaryOp::Remove => PrimId::MapRemove,
                CoreMapNullaryOp::Keys => PrimId::MapKeys,
                CoreMapNullaryOp::Values => PrimId::MapValues,
                CoreMapNullaryOp::Entries => PrimId::MapEntries,
                CoreMapNullaryOp::OfList => PrimId::ListToMap,
            },
        },
        Prim::Set { op, .. } => match op {
            CoreSetOp::Empty => PrimId::SetEmpty,
            CoreSetOp::Size => PrimId::SetSize,
            CoreSetOp::IsEmpty => PrimId::SetIsEmpty,
            CoreSetOp::Has => PrimId::SetHas,
            CoreSetOp::Add => PrimId::SetAdd,
            CoreSetOp::Remove => PrimId::SetRemove,
            CoreSetOp::Union => PrimId::SetUnion,
            CoreSetOp::Intersection => PrimId::SetIntersection,
            CoreSetOp::Difference => PrimId::SetDifference,
            CoreSetOp::ToList => PrimId::SetToList,
            CoreSetOp::OfList => PrimId::ListToSet,
        },
        Prim::Eq(_) => return Some(native(Opcode::Equal, "fn(T, T) : Bool", "bb")),
        Prim::Structural(_) => PrimId::TextStructural,
        Prim::Io(op) => {
            let host = match op {
                IoOp::Nullary(n) => match n {
                    CoreIoNullaryOp::ArgsList => HostOpId::ArgsList,
                    CoreIoNullaryOp::EnvGet => HostOpId::EnvGet,
                    CoreIoNullaryOp::Print => HostOpId::ConsolePrint,
                    CoreIoNullaryOp::PrintError => HostOpId::ConsoleError,
                    CoreIoNullaryOp::ReadLine => HostOpId::ConsoleReadLine,
                    CoreIoNullaryOp::Printed => HostOpId::ConsolePrinted,
                    CoreIoNullaryOp::PrintedErrors => HostOpId::ConsolePrintedErrors,
                    CoreIoNullaryOp::WriteText => HostOpId::FilesWriteText,
                    CoreIoNullaryOp::AppendText => HostOpId::FilesAppendText,
                    CoreIoNullaryOp::Remove => HostOpId::FilesRemove,
                    CoreIoNullaryOp::MakeFolder => HostOpId::FilesMakeFolder,
                    CoreIoNullaryOp::ReadOnly => HostOpId::FilesReadOnly,
                    CoreIoNullaryOp::Now => HostOpId::ClockNow,
                    CoreIoNullaryOp::Sleep => HostOpId::ClockSleep,
                    CoreIoNullaryOp::RandomInt => HostOpId::RandomInt,
                    CoreIoNullaryOp::ConsoleFake => HostOpId::ConsoleFake,
                    CoreIoNullaryOp::ConsoleFakeInput => HostOpId::ConsoleFakeInput,
                    CoreIoNullaryOp::FilesInMemory => HostOpId::FilesInMemory,
                    CoreIoNullaryOp::ClockFixed => HostOpId::ClockFixed,
                    CoreIoNullaryOp::RandomSeeded => HostOpId::RandomSeeded,
                    CoreIoNullaryOp::ArgsOf => HostOpId::ArgsOf,
                    CoreIoNullaryOp::EnvOf => HostOpId::EnvOf,
                    CoreIoNullaryOp::Connect => HostOpId::NetworkConnect,
                    CoreIoNullaryOp::ConnectSecure => HostOpId::NetworkConnectSecure,
                    CoreIoNullaryOp::Send => HostOpId::ConnectionSend,
                    CoreIoNullaryOp::Receive => HostOpId::ConnectionReceive,
                    CoreIoNullaryOp::NetworkFake => HostOpId::NetworkFake,
                    CoreIoNullaryOp::AllowHosts => HostOpId::NetworkAllowHosts,
                    CoreIoNullaryOp::SqliteOpen => HostOpId::SqliteOpen,
                    CoreIoNullaryOp::SqliteFake => HostOpId::SqliteFake,
                    CoreIoNullaryOp::SqliteRun => HostOpId::SqliteRun,
                    CoreIoNullaryOp::SqliteQuery => HostOpId::SqliteQuery,
                    CoreIoNullaryOp::SqliteExecuteRaw => HostOpId::SqliteExecuteRaw,
                    CoreIoNullaryOp::CommandRunRaw => HostOpId::CommandRunRaw,
                    CoreIoNullaryOp::CommandWithin => HostOpId::CommandWithin,
                    CoreIoNullaryOp::CommandAllowPrograms => HostOpId::CommandAllowPrograms,
                    CoreIoNullaryOp::CommandFake => HostOpId::CommandFake,
                    CoreIoNullaryOp::SecureRandomBytes => HostOpId::SecurerandomBytes,
                    CoreIoNullaryOp::SecureRandomSeeded => HostOpId::SecurerandomSeeded,
                    CoreIoNullaryOp::WriteBytes => HostOpId::FilesWriteBytes,
                    CoreIoNullaryOp::Rename => HostOpId::FilesRename,
                    CoreIoNullaryOp::FileOpen => HostOpId::FilesOpen,
                    CoreIoNullaryOp::FileReadAt => HostOpId::FilesReadAt,
                    CoreIoNullaryOp::FileWriteAt => HostOpId::FilesWriteAt,
                    CoreIoNullaryOp::FileSize => HostOpId::FilesSize,
                    CoreIoNullaryOp::FileTruncate => HostOpId::FilesTruncate,
                    CoreIoNullaryOp::FileSync => HostOpId::FilesSync,
                    CoreIoNullaryOp::FileLockShared => HostOpId::FilesLockShared,
                    CoreIoNullaryOp::FileLockExclusive => HostOpId::FilesLockExclusive,
                    CoreIoNullaryOp::FileUnlock => HostOpId::FilesUnlock,
                    CoreIoNullaryOp::ReadFileOpen => HostOpId::ReadfilesOpen,
                    CoreIoNullaryOp::NetworkFakeScripted => HostOpId::NetworkFakeScripted,
                    CoreIoNullaryOp::NetworkRecordedCalls => HostOpId::NetworkRecordedCalls,
                    CoreIoNullaryOp::SqliteStatsRaw => HostOpId::SqliteStatsRaw,
                    CoreIoNullaryOp::SqliteResetStatsRaw => HostOpId::SqliteResetStatsRaw,
                    CoreIoNullaryOp::CommandAllowLoopback => HostOpId::CommandAllowLoopback,
                    CoreIoNullaryOp::CommandSpawnRaw => HostOpId::CommandSpawnRaw,
                    CoreIoNullaryOp::CommandProcessPid => HostOpId::CommandProcessPid,
                    CoreIoNullaryOp::CommandProcessAlive => HostOpId::CommandProcessAlive,
                    CoreIoNullaryOp::CommandProcessKillRaw => HostOpId::CommandProcessKillRaw,
                    CoreIoNullaryOp::CommandProcessWaitRaw => HostOpId::CommandProcessWaitRaw,
                },
                IoOp::ReadText { ro: false } => HostOpId::FilesReadText,
                IoOp::ReadText { ro: true } => HostOpId::ReadfilesReadText,
                IoOp::ReadBytes { ro: false } => HostOpId::FilesReadBytes,
                IoOp::ReadBytes { ro: true } => HostOpId::ReadfilesReadBytes,
                IoOp::List { ro: false } => HostOpId::FilesList,
                IoOp::List { ro: true } => HostOpId::ReadfilesList,
                IoOp::Entries { ro: false } => HostOpId::FilesEntries,
                IoOp::Entries { ro: true } => HostOpId::ReadfilesEntries,
                IoOp::Exists { ro: false } => HostOpId::FilesExists,
                IoOp::Exists { ro: true } => HostOpId::ReadfilesExists,
                IoOp::Within { ro: false } => HostOpId::FilesWithin,
                IoOp::Within { ro: true } => HostOpId::ReadfilesWithin,
            };
            return Some(host_op(host));
        }
    };
    Some(primitive_op(id))
}
