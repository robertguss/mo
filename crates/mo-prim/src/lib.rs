#![forbid(unsafe_code)]

//! Mo standard-library primitive dispatch over `mo_def::PrimId`.

mod collections;
mod number;
mod text;

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
        | PrimId::BytesCompareSliceAt => text::call(heap, types, prim, params, result_ty, args),

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

        // Deferred to Slice 1B (BigInt) and Slice 1C (Unicode16, Crypto)
        other => Err(PrimError::UnimplementedPrim(other)),
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

        let mut dummy = vec![Value::Unit];
        assert_eq!(
            call(&heap, &types, PrimId::BigintAdd, sig, &mut dummy)
                .err()
                .unwrap(),
            PrimError::UnimplementedPrim(PrimId::BigintAdd)
        );
        assert_eq!(
            call(&heap, &types, PrimId::Unicode16Lowercase, sig, &mut dummy)
                .err()
                .unwrap(),
            PrimError::UnimplementedPrim(PrimId::Unicode16Lowercase)
        );
        assert_eq!(
            call(&heap, &types, PrimId::CryptoSha256, sig, &mut dummy)
                .err()
                .unwrap(),
            PrimError::UnimplementedPrim(PrimId::CryptoSha256)
        );
    }
}
