use std::cmp::Ordering;
use std::collections::VecDeque;
use std::fmt::Write as _;

use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_def::limits::{MO_LIMIT_FILE_IO_BYTES, MO_LIMIT_VALUE_BYTES};
use mo_heap::decimal::DECIMAL_MAX;
use mo_heap::print::value_print;
use mo_heap::{Heap, InlinePayload, Value};

use crate::{PrimError, check_value_bytes_fit};

fn is_ascii_white(c: u32) -> bool {
    c == u32::from(b' ') || (9..=13).contains(&c)
}

fn ordering_value(ord: Ordering) -> Value {
    let tag = match ord {
        Ordering::Less => 0,
        Ordering::Equal => 1,
        Ordering::Greater => 2,
    };
    Value::ChoiceInline {
        tag,
        payload: InlinePayload::Empty,
    }
}

fn hex_digit_count(mut n: u32) -> usize {
    let mut digits = 1usize;
    while n >= 16 {
        n >>= 4;
        digits += 1;
    }
    digits
}

/// Quotes and escapes `s` as a valid Mo string/char literal (reference 2.1 and 9.2),
/// escaping C0/C1 controls, invisible/bidi scalars, delimiter, backslash, and `#{`.
fn quote_prim(s: &str, delimiter: u8) -> Result<String, PrimError> {
    let bytes = s.as_bytes();
    let delim_char = char::from(delimiter);
    let mut len: usize = 2;
    for (i, ch) in s.char_indices() {
        let extra = match ch {
            '\n' | '\r' | '\t' => 2,
            c if mo_def::is_invisible_scalar(c as u32) => 4 + hex_digit_count(c as u32),
            c if c == delim_char || c == '\\' || (c == '#' && bytes.get(i + 1) == Some(&b'{')) => {
                1 + c.len_utf8()
            }
            c => c.len_utf8(),
        };
        len = len.checked_add(extra).ok_or(PrimError::OutOfMemory(Some(
            "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
        )))?;
        check_value_bytes_fit(len)?;
    }

    let mut out = String::with_capacity(len);
    out.push(delim_char);
    for (i, ch) in s.char_indices() {
        match ch {
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if mo_def::is_invisible_scalar(c as u32) => {
                let _ = write!(out, "\\u{{{:x}}}", c as u32);
            }
            c => {
                if c == delim_char || c == '\\' || (c == '#' && bytes.get(i + 1) == Some(&b'{')) {
                    out.push('\\');
                }
                out.push(c);
            }
        }
    }
    out.push(delim_char);
    Ok(out)
}

fn parse_int_or_decimal(s: &[u8], maximum: u128, fractional: bool) -> Option<i128> {
    let negative = s.first() == Some(&b'-');
    let mut at = usize::from(negative);
    let mut start = at;
    let mut n: u128 = 0;
    while at < s.len() && s[at].is_ascii_digit() {
        let digit = u128::from(s[at] - b'0');
        if n > (maximum - digit) / 10 {
            return None;
        }
        n = n * 10 + digit;
        at += 1;
    }
    if at == start {
        return None;
    }
    if fractional {
        let mut places = 0u32;
        if at < s.len() && s[at] == b'.' {
            at += 1;
            start = at;
            while at < s.len() && s[at].is_ascii_digit() && places < 18 {
                let digit = u128::from(s[at] - b'0');
                if n > (maximum - digit) / 10 {
                    return None;
                }
                n = n * 10 + digit;
                at += 1;
                places += 1;
            }
            if at == start {
                return None;
            }
        }
        while places < 18 {
            if n > maximum / 10 {
                return None;
            }
            n *= 10;
            places += 1;
        }
    }
    if at != s.len() {
        return None;
    }
    if !fractional && n == (i64::MAX as u128) + 1 && !negative {
        return None;
    }
    Some(if negative { -(n as i128) } else { n as i128 })
}

fn parse_float(s: &str) -> Option<f64> {
    let bytes = s.as_bytes();
    if bytes == b"nan" {
        return Some(f64::from_bits(0x7ff8_0000_0000_0000));
    }
    if bytes == b"inf" {
        return Some(f64::INFINITY);
    }
    if bytes == b"-inf" {
        return Some(f64::NEG_INFINITY);
    }
    let negative = bytes.first() == Some(&b'-');
    let mut at = usize::from(negative);
    let mut start = at;
    let mut nonzero = false;
    let mut significant: usize = 0;
    while at < bytes.len() && bytes[at].is_ascii_digit() {
        if bytes[at] != b'0' {
            nonzero = true;
        }
        if nonzero {
            significant += 1;
        }
        at += 1;
    }
    if at == start {
        return None;
    }
    let mut fraction: usize = 0;
    if at < bytes.len() && bytes[at] == b'.' {
        at += 1;
        start = at;
        while at < bytes.len() && bytes[at].is_ascii_digit() {
            if bytes[at] != b'0' {
                nonzero = true;
            }
            if nonzero {
                significant += 1;
            }
            at += 1;
        }
        fraction = at - start;
        if fraction == 0 {
            return None;
        }
    }
    let mut exponent: i64 = 0;
    if at < bytes.len() && matches!(bytes[at], b'e' | b'E') {
        at += 1;
        let minus = at < bytes.len() && bytes[at] == b'-';
        if at < bytes.len() && matches!(bytes[at], b'-' | b'+') {
            at += 1;
        }
        start = at;
        while at < bytes.len() && bytes[at].is_ascii_digit() {
            if (exponent as u64) <= MO_LIMIT_VALUE_BYTES {
                exponent = exponent * 10 + i64::from(bytes[at] - b'0');
            }
            at += 1;
        }
        if at == start {
            return None;
        }
        if minus {
            exponent = -exponent;
        }
    }
    if at != bytes.len() {
        return None;
    }
    let order = (significant as i64) + exponent - (fraction as i64);
    if !nonzero || order < -323 {
        return Some(if negative { -0.0 } else { 0.0 });
    }
    if order > 309 {
        return None;
    }
    let val = s.parse::<f64>().ok()?;
    val.is_finite().then_some(val)
}

fn hex_nibble(ch: u8) -> Option<u8> {
    match ch {
        b'0'..=b'9' => Some(ch - b'0'),
        b'a'..=b'f' => Some(ch - b'a' + 10),
        b'A'..=b'F' => Some(ch - b'A' + 10),
        _ => None,
    }
}

fn b64url_value(ch: u8) -> Option<u32> {
    match ch {
        b'A'..=b'Z' => Some(u32::from(ch - b'A')),
        b'a'..=b'z' => Some(u32::from(ch - b'a') + 26),
        b'0'..=b'9' => Some(u32::from(ch - b'0') + 52),
        b'-' => Some(62),
        b'_' => Some(63),
        _ => None,
    }
}

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    params: &[u32],
    result_ty: u32,
    args: &mut [Value],
) -> Result<Value, PrimError> {
    match prim {
        PrimId::TextStructural => {
            let s = value_print(types, params[0], &args[0]);
            check_value_bytes_fit(s.len())?;
            Ok(heap.alloc_text(s)?)
        }
        PrimId::TextQuoted => {
            let q = quote_prim(args[0].as_text(), b'"')?;
            Ok(heap.alloc_text(q)?)
        }
        PrimId::TextConcat => {
            let total = args[0]
                .as_text()
                .len()
                .saturating_add(args[1].as_text().len());
            check_value_bytes_fit(total)?;
            let [lhs, rhs, ..] = args else { unreachable!() };
            Ok(heap.text_concat(lhs, rhs)?)
        }
        PrimId::TextCompare => Ok(ordering_value(
            args[0]
                .as_text()
                .as_bytes()
                .cmp(args[1].as_text().as_bytes()),
        )),
        PrimId::TextByteCount => Ok(Value::Int(args[0].as_text().len() as i64)),
        PrimId::TextCharCount => Ok(Value::Int(args[0].as_text().chars().count() as i64)),
        PrimId::TextIsEmpty => Ok(Value::Bool(args[0].as_text().is_empty())),
        PrimId::TextBytes => Ok(heap.alloc_bytes(args[0].as_text().as_bytes().to_vec())?),
        PrimId::TextChars => {
            let items: VecDeque<Value> = args[0].as_text().chars().map(Value::Char).collect();
            Ok(heap.alloc_list(items)?)
        }
        PrimId::TextLines => {
            let text = args[0].as_text();
            let bytes = text.as_bytes();
            let mut items = VecDeque::new();
            let mut start = 0usize;
            for (i, &b) in bytes.iter().enumerate() {
                if b == b'\n' {
                    let mut end = i;
                    if end > start && bytes[end - 1] == b'\r' {
                        end -= 1;
                    }
                    let piece = text[start..end].to_string();
                    items.push_back(heap.alloc_text(piece)?);
                    start = i + 1;
                }
            }
            if start < bytes.len() {
                let piece = text[start..].to_string();
                items.push_back(heap.alloc_text(piece)?);
            }
            Ok(heap.alloc_list(items)?)
        }
        PrimId::TextWords => {
            let text = args[0].as_text();
            let bytes = text.as_bytes();
            let mut items = VecDeque::new();
            let mut start = 0usize;
            for (i, &b) in bytes.iter().enumerate() {
                if is_ascii_white(u32::from(b)) {
                    if i > start {
                        let piece = text[start..i].to_string();
                        items.push_back(heap.alloc_text(piece)?);
                    }
                    start = i + 1;
                }
            }
            if start < bytes.len() {
                let piece = text[start..].to_string();
                items.push_back(heap.alloc_text(piece)?);
            }
            Ok(heap.alloc_list(items)?)
        }
        PrimId::TextSplit => {
            let text = args[0].as_text();
            let sep = args[1].as_text();
            if sep.is_empty() {
                return Err(PrimError::Outside("Text.split with an empty separator"));
            }
            let mut items = VecDeque::new();
            for part in text.split(sep) {
                items.push_back(heap.alloc_text(part.to_string())?);
            }
            Ok(heap.alloc_list(items)?)
        }
        PrimId::TextTrim | PrimId::TextTrimStart | PrimId::TextTrimEnd => {
            let bytes = args[0].as_text().as_bytes();
            let mut start = 0usize;
            let mut end = bytes.len();
            if prim != PrimId::TextTrimEnd {
                while start < end && is_ascii_white(u32::from(bytes[start])) {
                    start += 1;
                }
            }
            if prim != PrimId::TextTrimStart {
                while end > start && is_ascii_white(u32::from(bytes[end - 1])) {
                    end -= 1;
                }
            }
            Ok(heap.text_slice(&mut args[0], start, end - start)?)
        }
        PrimId::TextStartsWith => Ok(Value::Bool(
            args[0].as_text().starts_with(args[1].as_text()),
        )),
        PrimId::TextEndsWith => Ok(Value::Bool(args[0].as_text().ends_with(args[1].as_text()))),
        PrimId::TextContains => Ok(Value::Bool(args[0].as_text().contains(args[1].as_text()))),
        PrimId::TextDropPrefix | PrimId::TextDropSuffix => {
            let is_prefix = prim == PrimId::TextDropPrefix;
            let pat_len = args[1].as_text().len();
            let text_len = args[0].as_text().len();
            let found = if is_prefix {
                args[0].as_text().starts_with(args[1].as_text())
            } else {
                args[0].as_text().ends_with(args[1].as_text())
            };
            if !found {
                return Ok(heap.make_option(types, result_ty, None)?);
            }
            let start = if is_prefix { pat_len } else { 0 };
            let sliced = heap.text_slice(&mut args[0], start, text_len - pat_len)?;
            Ok(heap.make_option(types, result_ty, Some(sliced))?)
        }
        PrimId::TextReplace => {
            let pat = args[1].as_text();
            if pat.is_empty() {
                return Err(PrimError::Outside("Text.replace of an empty text"));
            }
            let text = args[0].as_text();
            let rep = args[2].as_text();
            if !text.contains(pat) {
                return Ok(std::mem::replace(&mut args[0], Value::Unit));
            }
            let count = text.matches(pat).count();
            let new_len = (text.len() - count * pat.len())
                .checked_add(count.saturating_mul(rep.len()))
                .ok_or(PrimError::OutOfMemory(Some(
                    "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
                )))?;
            check_value_bytes_fit(new_len)?;
            let replaced = text.replace(pat, rep);
            Ok(heap.alloc_text(replaced)?)
        }
        PrimId::TextRepeat => {
            let count = args[1].as_int();
            if count < 0 {
                return Err(PrimError::Outside("Text.repeat with a negative count"));
            }
            let text = args[0].as_text();
            let count_u = count as u64;
            if !text.is_empty() && count_u > MO_LIMIT_VALUE_BYTES / (text.len() as u64) {
                return Err(PrimError::OutOfMemory(Some(
                    "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
                )));
            }
            let repeated = text.repeat(count as usize);
            Ok(heap.alloc_text(repeated)?)
        }
        PrimId::TextPadLeft | PrimId::TextPadRight => {
            let text = args[0].as_text();
            let width = args[1].as_int();
            let chars = text.chars().count() as i64;
            let spaces = if width > chars {
                (width - chars) as usize
            } else {
                0
            };
            let total = text
                .len()
                .checked_add(spaces)
                .ok_or(PrimError::OutOfMemory(Some(
                    "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
                )))?;
            check_value_bytes_fit(total)?;
            let mut out = String::with_capacity(total);
            if prim == PrimId::TextPadLeft {
                for _ in 0..spaces {
                    out.push(' ');
                }
                out.push_str(text);
            } else {
                out.push_str(text);
                for _ in 0..spaces {
                    out.push(' ');
                }
            }
            Ok(heap.alloc_text(out)?)
        }
        PrimId::TextAsciiLowercase => Ok(heap.text_ascii_case(&mut args[0], false)?),
        PrimId::TextAsciiUppercase => Ok(heap.text_ascii_case(&mut args[0], true)?),
        PrimId::TextToInt => {
            let opt =
                parse_int_or_decimal(args[0].as_text().as_bytes(), (i64::MAX as u128) + 1, false)
                    .map(|n| Value::Int(n as i64));
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::TextToDecimal => {
            let opt = parse_int_or_decimal(args[0].as_text().as_bytes(), DECIMAL_MAX as u128, true)
                .map(Value::Decimal);
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::TextToFloat => {
            let opt = parse_float(args[0].as_text()).map(Value::Float);
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::TextFromChars => {
            let list = args[0].as_list();
            let mut len = 0usize;
            for v in list {
                len = len.saturating_add(v.as_char().len_utf8());
                check_value_bytes_fit(len)?;
            }
            let mut out = String::with_capacity(len);
            for v in list {
                out.push(v.as_char());
            }
            Ok(heap.alloc_text(out)?)
        }

        // Char primitives
        PrimId::CharCode => Ok(Value::Int(i64::from(args[0].as_char() as u32))),
        PrimId::CharIsAsciiDigit => Ok(Value::Bool(args[0].as_char().is_ascii_digit())),
        PrimId::CharIsAsciiLetter => Ok(Value::Bool(args[0].as_char().is_ascii_alphabetic())),
        PrimId::CharIsAsciiWhiteSpace => Ok(Value::Bool(is_ascii_white(args[0].as_char() as u32))),
        PrimId::CharAsciiLowercase => Ok(Value::Char(args[0].as_char().to_ascii_lowercase())),
        PrimId::CharAsciiUppercase => Ok(Value::Char(args[0].as_char().to_ascii_uppercase())),
        PrimId::CharToText => {
            let mut buf = [0u8; 4];
            let s = args[0].as_char().encode_utf8(&mut buf).to_string();
            Ok(heap.alloc_text(s)?)
        }
        PrimId::CharQuoted => {
            let mut buf = [0u8; 4];
            let s = args[0].as_char().encode_utf8(&mut buf);
            let q = quote_prim(s, b'\'')?;
            Ok(heap.alloc_text(q)?)
        }

        // Bytes primitives
        PrimId::BytesCompare => Ok(ordering_value(args[0].as_bytes().cmp(args[1].as_bytes()))),
        PrimId::BytesSize => Ok(Value::Int(args[0].as_bytes().len() as i64)),
        PrimId::BytesIsEmpty => Ok(Value::Bool(args[0].as_bytes().is_empty())),
        PrimId::BytesGet => {
            let bytes = args[0].as_bytes();
            let idx = args[1].as_int();
            let opt = if idx >= 0 && (idx as usize) < bytes.len() {
                Some(Value::Byte(bytes[idx as usize]))
            } else {
                None
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::BytesDecodeUtf8 => {
            let opt = match std::str::from_utf8(args[0].as_bytes()) {
                Ok(s) => Some(heap.alloc_text(s.to_string())?),
                Err(_) => None,
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::BytesSlice => {
            let bytes = args[0].as_bytes();
            let from = args[1].as_int();
            let to = args[2].as_int();
            if from < 0 || to < 0 {
                return Err(PrimError::Outside("Bytes.slice with a negative index"));
            }
            let end = (to as u64).min(bytes.len() as u64) as usize;
            let start = (from as u64).min(end as u64) as usize;
            Ok(heap.alloc_bytes(bytes[start..end].to_vec())?)
        }
        PrimId::BytesJoin => {
            let list = args[0].as_list();
            let mut total = 0usize;
            for item in list {
                total = total.saturating_add(item.as_bytes().len());
                check_value_bytes_fit(total)?;
            }
            let mut out = Vec::with_capacity(total);
            for item in list {
                out.extend_from_slice(item.as_bytes());
            }
            Ok(heap.alloc_bytes(out)?)
        }
        PrimId::BytesToHex => {
            const HEX: &[u8; 16] = b"0123456789abcdef";
            let bytes = args[0].as_bytes();
            if (bytes.len() as u64) > MO_LIMIT_VALUE_BYTES / 2 {
                return Err(PrimError::OutOfMemory(Some(
                    "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
                )));
            }
            let mut out = String::with_capacity(bytes.len() * 2);
            for &b in bytes {
                out.push(HEX[(b >> 4) as usize] as char);
                out.push(HEX[(b & 0x0f) as usize] as char);
            }
            Ok(heap.alloc_text(out)?)
        }
        PrimId::BytesFromHex => {
            let bytes = args[0].as_text().as_bytes();
            if !bytes.len().is_multiple_of(2) {
                return Ok(heap.make_option(types, result_ty, None)?);
            }
            let mut out = Vec::with_capacity(bytes.len() / 2);
            for pair in bytes.as_chunks::<2>().0 {
                let (Some(hi), Some(lo)) = (hex_nibble(pair[0]), hex_nibble(pair[1])) else {
                    return Ok(heap.make_option(types, result_ty, None)?);
                };
                out.push((hi << 4) | lo);
            }
            let v = heap.alloc_bytes(out)?;
            Ok(heap.make_option(types, result_ty, Some(v))?)
        }
        PrimId::BytesToBase64url => {
            const TABLE: &[u8; 64] =
                b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            let bytes = args[0].as_bytes();
            let full = bytes.len() / 3;
            let rem = bytes.len() % 3;
            let extra = if rem == 0 { 0 } else { rem + 1 };
            let out_len = full
                .checked_mul(4)
                .and_then(|x| x.checked_add(extra))
                .ok_or(PrimError::OutOfMemory(Some(
                    "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
                )))?;
            check_value_bytes_fit(out_len)?;
            let mut out = String::with_capacity(out_len);
            for chunk in bytes.as_chunks::<3>().0 {
                let v =
                    (u32::from(chunk[0]) << 16) | (u32::from(chunk[1]) << 8) | u32::from(chunk[2]);
                out.push(TABLE[((v >> 18) & 0x3f) as usize] as char);
                out.push(TABLE[((v >> 12) & 0x3f) as usize] as char);
                out.push(TABLE[((v >> 6) & 0x3f) as usize] as char);
                out.push(TABLE[(v & 0x3f) as usize] as char);
            }
            if rem == 1 {
                let b0 = u32::from(bytes[full * 3]);
                out.push(TABLE[(b0 >> 2) as usize] as char);
                out.push(TABLE[((b0 & 0x03) << 4) as usize] as char);
            } else if rem == 2 {
                let b0 = u32::from(bytes[full * 3]);
                let b1 = u32::from(bytes[full * 3 + 1]);
                out.push(TABLE[(b0 >> 2) as usize] as char);
                out.push(TABLE[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
                out.push(TABLE[((b1 & 0x0f) << 2) as usize] as char);
            }
            Ok(heap.alloc_text(out)?)
        }
        PrimId::BytesFromBase64url => {
            let bytes = args[0].as_text().as_bytes();
            let rem = bytes.len() % 4;
            if rem == 1 {
                return Ok(heap.make_option(types, result_ty, None)?);
            }
            let full = bytes.len() / 4;
            let out_len = full * 3 + if rem == 0 { 0 } else { rem - 1 };
            let mut out = Vec::with_capacity(out_len);
            for chunk in bytes.as_chunks::<4>().0 {
                let (Some(v0), Some(v1), Some(v2), Some(v3)) = (
                    b64url_value(chunk[0]),
                    b64url_value(chunk[1]),
                    b64url_value(chunk[2]),
                    b64url_value(chunk[3]),
                ) else {
                    return Ok(heap.make_option(types, result_ty, None)?);
                };
                let v = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
                out.push(((v >> 16) & 0xff) as u8);
                out.push(((v >> 8) & 0xff) as u8);
                out.push((v & 0xff) as u8);
            }
            if rem == 2 {
                let (Some(v0), Some(v1)) = (
                    b64url_value(bytes[full * 4]),
                    b64url_value(bytes[full * 4 + 1]),
                ) else {
                    return Ok(heap.make_option(types, result_ty, None)?);
                };
                if (v1 & 0x0f) != 0 {
                    return Ok(heap.make_option(types, result_ty, None)?);
                }
                out.push(((v0 << 2) | (v1 >> 4)) as u8);
            } else if rem == 3 {
                let (Some(v0), Some(v1), Some(v2)) = (
                    b64url_value(bytes[full * 4]),
                    b64url_value(bytes[full * 4 + 1]),
                    b64url_value(bytes[full * 4 + 2]),
                ) else {
                    return Ok(heap.make_option(types, result_ty, None)?);
                };
                if (v2 & 0x03) != 0 {
                    return Ok(heap.make_option(types, result_ty, None)?);
                }
                out.push(((v0 << 2) | (v1 >> 4)) as u8);
                out.push((((v1 & 0x0f) << 4) | (v2 >> 2)) as u8);
            }
            let v = heap.alloc_bytes(out)?;
            Ok(heap.make_option(types, result_ty, Some(v))?)
        }
        PrimId::BytesEmpty => Ok(heap.alloc_bytes(Vec::new())?),
        PrimId::BytesZeroed => {
            let count = args[0].as_int();
            if count < 0 {
                return Err(PrimError::Outside("Bytes.zeroed with a negative count"));
            }
            if (count as u64) > MO_LIMIT_FILE_IO_BYTES {
                return Err(PrimError::Outside("Bytes.zeroed count exceeds limit"));
            }
            Ok(heap.alloc_bytes(vec![0u8; count as usize])?)
        }
        PrimId::BytesFromList => {
            let bytes: Vec<u8> = args[0].as_list().iter().map(Value::as_byte).collect();
            Ok(heap.alloc_bytes(bytes)?)
        }
        PrimId::BytesToList => {
            let items: VecDeque<Value> = args[0]
                .as_bytes()
                .iter()
                .copied()
                .map(Value::Byte)
                .collect();
            Ok(heap.alloc_list(items)?)
        }
        PrimId::BytesFromU16Be => {
            let n = args[0].as_int();
            if !(0..=65535).contains(&n) {
                return Err(PrimError::Outside(
                    "Bytes.from_u16_be value outside 0 to 65535",
                ));
            }
            Ok(heap.alloc_bytes((n as u16).to_be_bytes().to_vec())?)
        }
        PrimId::BytesFromU32Be => {
            let n = args[0].as_int();
            if !(0..=4_294_967_295).contains(&n) {
                return Err(PrimError::Outside(
                    "Bytes.from_u32_be value outside 0 to 4294967295",
                ));
            }
            Ok(heap.alloc_bytes((n as u32).to_be_bytes().to_vec())?)
        }
        PrimId::BytesFromI64Be => {
            let n = args[0].as_int();
            Ok(heap.alloc_bytes(n.to_be_bytes().to_vec())?)
        }
        PrimId::BytesU16BeAt | PrimId::BytesU32BeAt | PrimId::BytesI64BeAt => {
            let bytes = args[0].as_bytes();
            let offset = args[1].as_int();
            let width = match prim {
                PrimId::BytesU16BeAt => 2,
                PrimId::BytesU32BeAt => 4,
                PrimId::BytesI64BeAt => 8,
                _ => unreachable!(),
            };
            let opt = if offset >= 0
                && (offset as u64) <= bytes.len() as u64
                && bytes.len() - (offset as usize) >= width
            {
                let p = &bytes[offset as usize..offset as usize + width];
                let val = match width {
                    2 => i64::from(u16::from_be_bytes([p[0], p[1]])),
                    4 => i64::from(u32::from_be_bytes([p[0], p[1], p[2], p[3]])),
                    8 => i64::from_be_bytes([p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]]),
                    _ => unreachable!(),
                };
                Some(Value::Int(val))
            } else {
                None
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::BytesCompareSliceAt => {
            let bytes = args[0].as_bytes();
            let offset = args[1].as_int();
            let len = args[2].as_int();
            let other = args[3].as_bytes();
            if offset < 0
                || len < 0
                || (offset as u64) > bytes.len() as u64
                || bytes.len() - (offset as usize) < len as usize
            {
                return Ok(heap.make_option(types, result_ty, None)?);
            }
            let slice = &bytes[offset as usize..offset as usize + len as usize];
            let ord = ordering_value(slice.cmp(other));
            Ok(heap.make_option(types, result_ty, Some(ord))?)
        }
        PrimId::BytesDotU16BeBiased | PrimId::BytesDotU8Biased => {
            let a = args[0].as_bytes();
            let a_off = args[1].as_int();
            let b = args[2].as_bytes();
            let b_off = args[3].as_int();
            let count = args[4].as_int();
            let bias = args[5].as_int();
            let width = if prim == PrimId::BytesDotU16BeBiased {
                2
            } else {
                1
            };
            let dot = bytes_dot_biased(a, a_off, b, b_off, count, bias, width).map(Value::Int);
            Ok(heap.make_option(types, result_ty, dot)?)
        }
        _ => unreachable!("unexpected text/char/bytes primitive {prim:?}"),
    }
}

/// Computes `sum_{i=0}^{count-1} (elem(a, i) - bias) * (elem(b, i) - bias)` for
/// big-endian `u16` (`width == 2`) or `u8` (`width == 1`) elements starting at `a_off`
/// and `b_off`, returning `None` when bounds are invalid or the sum overflows `i64`.
fn bytes_dot_biased(
    a: &[u8],
    a_off: i64,
    b: &[u8],
    b_off: i64,
    count: i64,
    bias: i64,
    width: usize,
) -> Option<i64> {
    let a_off = usize::try_from(a_off).ok()?;
    let b_off = usize::try_from(b_off).ok()?;
    let count_u = usize::try_from(count).ok()?;
    let byte_len = count_u.checked_mul(width)?;
    let a_end = a_off.checked_add(byte_len)?;
    let b_end = b_off.checked_add(byte_len)?;
    let a_slice = a.get(a_off..a_end)?;
    let b_slice = b.get(b_off..b_end)?;
    if count_u == 0 {
        return Some(0);
    }

    let max_elem = if width == 2 { 65_535 } else { 255 };
    // When bias stays within [0, max_elem] and count_u <= 1_000_000, every term has
    // |term| <= 65_535^2, so intermediate and final sums cannot overflow i64.
    if (0..=max_elem).contains(&bias) && count_u <= 1_000_000 {
        let mut acc: i64 = 0;
        if width == 2 {
            for (ca, cb) in a_slice.chunks_exact(2).zip(b_slice.chunks_exact(2)) {
                let va = i64::from(u16::from_be_bytes([ca[0], ca[1]]));
                let vb = i64::from(u16::from_be_bytes([cb[0], cb[1]]));
                acc += (va - bias) * (vb - bias);
            }
        } else {
            for (&va, &vb) in a_slice.iter().zip(b_slice.iter()) {
                acc += (i64::from(va) - bias) * (i64::from(vb) - bias);
            }
        }
        return Some(acc);
    }

    bytes_dot_biased_exact(a_slice, b_slice, bias, width)
}

/// Exact i128 fallback for `bytes_dot_biased`; outside `[0, max_elem]`, every product
/// shares positive sign so any i128 overflow stays permanently above `i64::MAX`.
fn bytes_dot_biased_exact(a_slice: &[u8], b_slice: &[u8], bias: i64, width: usize) -> Option<i64> {
    let bias128 = i128::from(bias);
    let mut acc: i128 = 0;
    if width == 2 {
        for (ca, cb) in a_slice.chunks_exact(2).zip(b_slice.chunks_exact(2)) {
            let va = i128::from(u16::from_be_bytes([ca[0], ca[1]]));
            let vb = i128::from(u16::from_be_bytes([cb[0], cb[1]]));
            acc = acc.checked_add((va - bias128) * (vb - bias128))?;
        }
    } else {
        for (&va, &vb) in a_slice.iter().zip(b_slice.iter()) {
            acc = acc.checked_add((i128::from(va) - bias128) * (i128::from(vb) - bias128))?;
        }
    }
    i64::try_from(acc).ok()
}

#[cfg(test)]
mod tests {
    use super::{bytes_dot_biased, quote_prim};

    #[test]
    fn quote_prim_escapes_c0_c1_and_invisible_scalars() {
        let raw = "a\0\x07\x7f\u{80}\u{9f}\u{ad}\u{61c}\u{180e}\u{200b}\u{202e}\u{2060}\u{feff}\u{fff9}\u{e0001}\u{e0020}#{x}\\\"'\n\r\tz";
        let q = quote_prim(raw, b'"').expect("quote text");
        assert_eq!(
            q,
            "\"a\\u{0}\\u{7}\\u{7f}\\u{80}\\u{9f}\\u{ad}\\u{61c}\\u{180e}\\u{200b}\\u{202e}\\u{2060}\\u{feff}\\u{fff9}\\u{e0001}\\u{e0020}\\#{x}\\\\\\\"'\\n\\r\\tz\""
        );
        assert_eq!(quote_prim("\u{80}", b'\'').expect("c1 char"), "'\\u{80}'");
        assert_eq!(
            quote_prim("\u{200b}", b'\'').expect("zwsp char"),
            "'\\u{200b}'"
        );
        assert_eq!(
            quote_prim("\u{202e}", b'\'').expect("bidi char"),
            "'\\u{202e}'"
        );
        assert_eq!(quote_prim("'", b'\'').expect("apos"), "'\\''");
        assert_eq!(quote_prim("\"", b'\'').expect("dquote"), "'\"'");
    }

    #[test]
    fn bytes_dot_biased_matches_exact_bounds_and_overflow() {
        let a16 = [9, 0x80, 0x00, 0x7f, 0xfe];
        let b16 = [0x7f, 0xfe, 0x80, 0x00];
        assert_eq!(bytes_dot_biased(&a16, 1, &b16, 0, 2, 32767, 2), Some(-2));
        assert_eq!(bytes_dot_biased(&a16, 5, &b16, 4, 0, i64::MIN, 2), Some(0));
        assert_eq!(bytes_dot_biased(&a16, 1, &b16, 0, 3, 0, 2), None);
        assert_eq!(
            bytes_dot_biased(&[0, 1], 0, &[0, 1], 0, 1, i64::MIN, 2),
            None
        );

        let a8 = [9, 130, 125];
        let b8 = [129, 126];
        assert_eq!(bytes_dot_biased(&a8, 1, &b8, 0, 2, 127, 1), Some(8));
        assert_eq!(bytes_dot_biased(&a8, -1, &b8, 0, 1, 0, 1), None);
        assert_eq!(bytes_dot_biased(&a8, 0, &b8, 0, -1, 0, 1), None);
        assert_eq!(bytes_dot_biased(&a8, 0, &b8, 0, i64::MAX, 0, 1), None);
    }
}
