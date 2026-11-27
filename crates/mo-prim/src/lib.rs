#![forbid(unsafe_code)]

//! Mo standard-library primitive dispatch over `mo_def::PrimId`.

mod bigint;
mod collections;
mod crypto;
mod json;
mod number;
mod text;
mod unicode;
#[rustfmt::skip]
mod unicode16_data;

pub use unicode::unicode16;

use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_heap::{Heap, HeapError, Value};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PrimError {
    Overflow(&'static str),
    DivByZero(&'static str),
    Outside(&'static str),
    RepeatedKey(&'static str),
    OutOfMemory(Option<&'static str>),
    UnimplementedPrim(PrimId),
}

impl From<HeapError> for PrimError {
    fn from(_: HeapError) -> Self {
        Self::OutOfMemory(None)
    }
}

pub(crate) fn check_value_bytes_fit(len: usize) -> Result<(), PrimError> {
    if (len as u64) <= mo_def::limits::MO_LIMIT_VALUE_BYTES {
        Ok(())
    } else {
        Err(PrimError::OutOfMemory(Some(
            "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
        )))
    }
}

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    sig_ty: u32,
    args: &mut [Value],
) -> Result<Value, PrimError> {
    let sig = types.get(sig_ty);
    let params = &sig.parameters;
    let result_ty = sig.result;

    match prim {
        // Int, Byte, Decimal, Float, Time, Duration
        PrimId::IntAbs
        | PrimId::IntMin
        | PrimId::IntMax
        | PrimId::IntPow
        | PrimId::IntFloorDiv
        | PrimId::IntFloorMod
        | PrimId::IntWrappingAdd
        | PrimId::IntWrappingSub
        | PrimId::IntWrappingMul
        | PrimId::IntSaturatingAdd
        | PrimId::IntSaturatingSub
        | PrimId::IntSaturatingMul
        | PrimId::IntCheckedAdd
        | PrimId::IntCheckedSub
        | PrimId::IntCheckedMul
        | PrimId::IntToDecimal
        | PrimId::IntToFloat
        | PrimId::IntToBig
        | PrimId::IntToByte
        | PrimId::IntToChar
        | PrimId::IntToText
        | PrimId::IntSeconds
        | PrimId::IntMilliseconds
        | PrimId::IntMinutes
        | PrimId::IntHours
        | PrimId::IntDays
        | PrimId::ByteToInt
        | PrimId::ByteToText
        | PrimId::DecimalAbs
        | PrimId::DecimalMin
        | PrimId::DecimalMax
        | PrimId::DecimalRound
        | PrimId::DecimalFloor
        | PrimId::DecimalCeil
        | PrimId::DecimalToInt
        | PrimId::DecimalToFloat
        | PrimId::DecimalToFixed
        | PrimId::DecimalToText
        | PrimId::FloatAbs
        | PrimId::FloatFloor
        | PrimId::FloatCeil
        | PrimId::FloatRound
        | PrimId::FloatSqrt
        | PrimId::FloatIsNan
        | PrimId::FloatIsInfinite
        | PrimId::FloatToDecimal
        | PrimId::FloatToText
        | PrimId::TimeCheckedAdd
        | PrimId::TimeToText
        | PrimId::TimeParse
        | PrimId::DurationInMilliseconds
        | PrimId::DurationInSeconds
        | PrimId::DurationToText => number::call(heap, types, prim, params, result_ty, args),

        // Text, Char, Bytes
        PrimId::TextConcat
        | PrimId::TextCompare
        | PrimId::TextByteCount
        | PrimId::TextCharCount
        | PrimId::TextIsEmpty
        | PrimId::TextChars
        | PrimId::TextBytes
        | PrimId::TextLines
        | PrimId::TextWords
        | PrimId::TextSplit
        | PrimId::TextTrim
        | PrimId::TextTrimStart
        | PrimId::TextTrimEnd
        | PrimId::TextStartsWith
        | PrimId::TextEndsWith
        | PrimId::TextContains
        | PrimId::TextDropPrefix
        | PrimId::TextDropSuffix
        | PrimId::TextReplace
        | PrimId::TextRepeat
        | PrimId::TextPadLeft
        | PrimId::TextPadRight
        | PrimId::TextAsciiLowercase
        | PrimId::TextAsciiUppercase
        | PrimId::TextToInt
        | PrimId::TextToDecimal
        | PrimId::TextToFloat
        | PrimId::TextFromChars
        | PrimId::TextQuoted
        | PrimId::TextStructural
        | PrimId::CharCode
        | PrimId::CharIsAsciiDigit
        | PrimId::CharIsAsciiLetter
        | PrimId::CharIsAsciiWhiteSpace
        | PrimId::CharAsciiLowercase
        | PrimId::CharAsciiUppercase
        | PrimId::CharToText
        | PrimId::CharQuoted
        | PrimId::BytesCompare
        | PrimId::BytesSize
        | PrimId::BytesGet
        | PrimId::BytesDecodeUtf8
        | PrimId::BytesSlice
        | PrimId::BytesJoin
        | PrimId::BytesToHex
        | PrimId::BytesFromHex
        | PrimId::BytesToBase64url
        | PrimId::BytesFromBase64url
        | PrimId::BytesEmpty
        | PrimId::BytesZeroed
        | PrimId::BytesFromList
        | PrimId::BytesToList
        | PrimId::BytesFromU16Be
        | PrimId::BytesFromU32Be
        | PrimId::BytesFromI64Be
        | PrimId::BytesU16BeAt
        | PrimId::BytesU32BeAt
        | PrimId::BytesI64BeAt
        | PrimId::BytesIsEmpty
        | PrimId::BytesCompareSliceAt
        | PrimId::BytesDotU16BeBiased
        | PrimId::BytesDotU8Biased => text::call(heap, types, prim, params, result_ty, args),

        // List, Map, Set, Range
        PrimId::ListEmpty
        | PrimId::ListSize
        | PrimId::ListIsEmpty
        | PrimId::ListGet
        | PrimId::ListFirst
        | PrimId::ListLast
        | PrimId::ListAppend
        | PrimId::ListPrepend
        | PrimId::ListConcat
        | PrimId::ListReverse
        | PrimId::ListTake
        | PrimId::ListDrop
        | PrimId::ListSet
        | PrimId::ListRemoveAt
        | PrimId::ListEnumerate
        | PrimId::ListZip
        | PrimId::ListContains
        | PrimId::ListUnique
        | PrimId::ListToSet
        | PrimId::ListSum
        | PrimId::ListJoin
        | PrimId::ListToMap
        | PrimId::MapEmpty
        | PrimId::MapLiteralInsert
        | PrimId::MapSize
        | PrimId::MapIsEmpty
        | PrimId::MapGet
        | PrimId::MapHas
        | PrimId::MapSet
        | PrimId::MapRemove
        | PrimId::MapKeys
        | PrimId::MapValues
        | PrimId::MapEntries
        | PrimId::SetEmpty
        | PrimId::SetSize
        | PrimId::SetIsEmpty
        | PrimId::SetHas
        | PrimId::SetAdd
        | PrimId::SetRemove
        | PrimId::SetUnion
        | PrimId::SetIntersection
        | PrimId::SetDifference
        | PrimId::SetToList
        | PrimId::RangeToList
        | PrimId::RangeContains => collections::call(heap, types, prim, params, result_ty, args),

        // BigInt (15 primitives)
        PrimId::BigintAdd
        | PrimId::BigintSub
        | PrimId::BigintMul
        | PrimId::BigintDiv
        | PrimId::BigintRem
        | PrimId::BigintNeg
        | PrimId::BigintCompare
        | PrimId::BigintAbs
        | PrimId::BigintMin
        | PrimId::BigintMax
        | PrimId::BigintPow
        | PrimId::BigintFloorDiv
        | PrimId::BigintFloorMod
        | PrimId::BigintToInt
        | PrimId::BigintToText => bigint::call(heap, types, prim, result_ty, args),

        // Unicode16 (7 primitives)
        PrimId::Unicode16Graphemes
        | PrimId::Unicode16DisplayWidth
        | PrimId::Unicode16NormalizeNfc
        | PrimId::Unicode16Lowercase
        | PrimId::Unicode16Uppercase
        | PrimId::Unicode16Letter
        | PrimId::Unicode16Digit => unicode::call(heap, prim, args),

        // Crypto (9 primitives)
        PrimId::CryptoSha256
        | PrimId::CryptoHmacSha256
        | PrimId::CryptoConstantTimeEq
        | PrimId::CryptoSealRaw
        | PrimId::CryptoOpenRaw
        | PrimId::CryptoHashPasswordRaw
        | PrimId::CryptoVerifyPassword
        | PrimId::CryptoHmacSha1
        | PrimId::CryptoVerifySignatureRaw => crypto::call(heap, types, prim, result_ty, args),

        // Json (2 primitives)
        PrimId::JsonQuoteRaw | PrimId::JsonParseRaw => {
            json::call(heap, types, prim, result_ty, args)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mo_bytecode::{NO_INDEX, TypeDef, TypeKind};

    fn make_sig_table(params: Vec<u32>, result: u32, extra: Vec<TypeDef>) -> (TypeTable, u32) {
        let mut items = extra;
        let sig_idx = items.len() as u32;
        items.push(TypeDef {
            kind: TypeKind::Function,
            parameters: params,
            result,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        });
        (TypeTable::finish(items, Vec::new()).unwrap(), sig_idx)
    }

    fn prim_ty(kind: TypeKind) -> TypeDef {
        TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        }
    }

    #[test]
    fn int_and_range_prims_and_overflow_checks() {
        let heap = Heap::new(1024 * 1024);
        let mut list_int = prim_ty(TypeKind::List);
        list_int.parameters = vec![0];
        let (types, sig) = make_sig_table(
            vec![0, 0],
            0,
            vec![prim_ty(TypeKind::Int), prim_ty(TypeKind::Range), list_int],
        );

        let mut args = vec![Value::Int(-42)];
        assert_eq!(
            call(&heap, &types, PrimId::IntAbs, sig, &mut args)
                .unwrap()
                .as_int(),
            42
        );

        let mut min_arg = vec![Value::Int(i64::MIN)];
        assert_eq!(
            call(&heap, &types, PrimId::IntAbs, sig, &mut min_arg)
                .err()
                .unwrap(),
            PrimError::Overflow("Int")
        );

        let mut div_zero = vec![Value::Int(10), Value::Int(0)];
        assert_eq!(
            call(&heap, &types, PrimId::IntFloorDiv, sig, &mut div_zero)
                .err()
                .unwrap(),
            PrimError::DivByZero("dividing by zero")
        );

        let mut range_args = vec![Value::Range(3, 6)];
        let list = call(&heap, &types, PrimId::RangeToList, sig, &mut range_args).unwrap();
        let vals: Vec<i64> = list.as_list().iter().map(Value::as_int).collect();
        assert_eq!(vals, vec![3, 4, 5]);
    }

    #[test]
    fn text_and_bytes_prims_and_unimplemented_errors() {
        let heap = Heap::new(1024 * 1024);
        let (types, sig) = make_sig_table(
            vec![0, 0],
            0,
            vec![prim_ty(TypeKind::Text), prim_ty(TypeKind::Bytes)],
        );

        let mut concat_args = vec![
            heap.alloc_text("hello ".into()).unwrap(),
            heap.alloc_text("mo".into()).unwrap(),
        ];
        let joined = call(&heap, &types, PrimId::TextConcat, sig, &mut concat_args).unwrap();
        assert_eq!(joined.as_text(), "hello mo");

        let mut bad_repeat = vec![heap.alloc_text("a".into()).unwrap(), Value::Int(-1)];
        assert_eq!(
            call(&heap, &types, PrimId::TextRepeat, sig, &mut bad_repeat)
                .err()
                .unwrap(),
            PrimError::Outside("Text.repeat with a negative count")
        );
        // Owned arg must remain untouched on precondition error so VM defects show it.
        assert_eq!(bad_repeat[0].as_text(), "a");

        let mut bytes_arg = vec![heap.alloc_bytes(vec![0xde, 0xad, 0xbe, 0xef]).unwrap()];
        let hex = call(&heap, &types, PrimId::BytesToHex, sig, &mut bytes_arg).unwrap();
        assert_eq!(hex.as_text(), "deadbeef");

        let mut u_arg = vec![heap.alloc_text("Ça".into()).unwrap()];
        let lowered = call(&heap, &types, PrimId::Unicode16Lowercase, sig, &mut u_arg).unwrap();
        assert_eq!(lowered.as_text(), "ça");

        let mut sha_arg = vec![heap.alloc_bytes(b"abc".to_vec()).unwrap()];
        let digest = call(&heap, &types, PrimId::CryptoSha256, sig, &mut sha_arg).unwrap();
        assert_eq!(digest.as_bytes().len(), 32);
    }

    #[test]
    fn bigint_all_15_prims_and_list_sum() {
        use mo_bytecode::{BigIntSign, TypeCase, TypeField};
        use std::collections::VecDeque;

        let heap = Heap::new(4 * 1024 * 1024);
        let int_ty = prim_ty(TypeKind::Int); // 0
        let big_ty = prim_ty(TypeKind::BigInt); // 1
        let text_ty = prim_ty(TypeKind::Text); // 2
        let opt_int_ty = TypeDef {
            kind: TypeKind::Choice,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: Some("Option".into()),
            fields: Vec::new(),
            cases: vec![
                TypeCase {
                    name_idx: 0,
                    name: "None".into(),
                    fields: Vec::new(),
                    unnamed: false,
                },
                TypeCase {
                    name_idx: 0,
                    name: "Some".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: 0,
                    }],
                    unnamed: true,
                },
            ],
            is_reference: false,
            has_fields: false,
        }; // 3
        let mut list_big_ty = prim_ty(TypeKind::List); // 4
        list_big_ty.parameters = vec![1];

        let mut items = vec![int_ty, big_ty, text_ty, opt_int_ty, list_big_ty];
        let sig_big2 = items.len() as u32;
        items.push(TypeDef {
            kind: TypeKind::Function,
            parameters: vec![1, 1],
            result: 1,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        });
        let sig_to_int = items.len() as u32;
        items.push(TypeDef {
            kind: TypeKind::Function,
            parameters: vec![1],
            result: 3,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        });
        let sig_to_text = items.len() as u32;
        items.push(TypeDef {
            kind: TypeKind::Function,
            parameters: vec![1],
            result: 2,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        });
        let sig_list_sum = items.len() as u32;
        items.push(TypeDef {
            kind: TypeKind::Function,
            parameters: vec![4],
            result: 1,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        });
        let types = TypeTable::finish(items, Vec::new()).unwrap();

        let mk = |n: i64| -> Value {
            let mut args = vec![Value::Int(n)];
            call(&heap, &types, PrimId::IntToBig, sig_big2, &mut args).unwrap()
        };
        let to_text = |v: &Value| -> String {
            let mut args = vec![v.clone()];
            call(&heap, &types, PrimId::BigintToText, sig_to_text, &mut args)
                .unwrap()
                .as_text()
                .to_string()
        };

        let a = mk(-7);
        let b = mk(3);
        let zero = heap.alloc_bigint(BigIntSign::Zero, Vec::new()).unwrap();

        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintAdd,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-4"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintSub,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-10"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintMul,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-21"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintDiv,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-2"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintRem,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-1"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintFloorDiv,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-3"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintFloorMod,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "2"
        );
        assert_eq!(
            call(
                &heap,
                &types,
                PrimId::BigintDiv,
                sig_big2,
                &mut [a.clone(), zero.clone()]
            )
            .err(),
            Some(PrimError::DivByZero("dividing by zero"))
        );
        assert_eq!(
            to_text(&call(&heap, &types, PrimId::BigintNeg, sig_big2, &mut [a.clone()]).unwrap()),
            "7"
        );
        assert_eq!(
            to_text(&call(&heap, &types, PrimId::BigintAbs, sig_big2, &mut [a.clone()]).unwrap()),
            "7"
        );
        assert_eq!(
            call(
                &heap,
                &types,
                PrimId::BigintCompare,
                sig_big2,
                &mut [a.clone(), b.clone()]
            )
            .unwrap()
            .as_tag(),
            0
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintMin,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "-7"
        );
        assert_eq!(
            to_text(
                &call(
                    &heap,
                    &types,
                    PrimId::BigintMax,
                    sig_big2,
                    &mut [a.clone(), b.clone()]
                )
                .unwrap()
            ),
            "3"
        );

        // Pow + negative exponent error + to_int boundary checks (i64::MIN / i64::MAX / overflow).
        let two = mk(2);
        let big63 = call(
            &heap,
            &types,
            PrimId::BigintPow,
            sig_big2,
            &mut [two.clone(), Value::Int(63)],
        )
        .unwrap();
        assert_eq!(to_text(&big63), "9223372036854775808");
        assert_eq!(
            call(
                &heap,
                &types,
                PrimId::BigintPow,
                sig_big2,
                &mut [two.clone(), Value::Int(-1)]
            )
            .err(),
            Some(PrimError::Outside("BigInt.pow with a negative exponent"))
        );
        // +2^63 does not fit in i64 -> None (tag 0), -2^63 fits -> Some(i64::MIN) (tag 1).
        let opt_overflow = call(
            &heap,
            &types,
            PrimId::BigintToInt,
            sig_to_int,
            &mut [big63.clone()],
        )
        .unwrap();
        assert_eq!(opt_overflow.as_tag(), 0);
        let neg63 = call(
            &heap,
            &types,
            PrimId::BigintNeg,
            sig_big2,
            &mut [big63.clone()],
        )
        .unwrap();
        let opt_min = call(
            &heap,
            &types,
            PrimId::BigintToInt,
            sig_to_int,
            &mut [neg63.clone()],
        )
        .unwrap();
        assert_eq!(opt_min.as_tag(), 1);
        assert_eq!(
            heap.extract_case_field(&types, 3, &opt_min, 1, 0).as_int(),
            i64::MIN
        );

        // List.sum on BigInt (empty -> 0, non-empty -> exact sum).
        let empty = heap.alloc_list(VecDeque::new()).unwrap();
        assert_eq!(
            to_text(&call(&heap, &types, PrimId::ListSum, sig_list_sum, &mut [empty]).unwrap()),
            "0"
        );
        let list = heap
            .alloc_list(VecDeque::from([big63.clone(), big63, mk(-8)]))
            .unwrap();
        assert_eq!(
            to_text(&call(&heap, &types, PrimId::ListSum, sig_list_sum, &mut [list]).unwrap()),
            "18446744073709551608"
        );
    }
}
