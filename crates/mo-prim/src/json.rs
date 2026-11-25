//! Native RFC 8259 JSON string escaping (`Json.quote_raw`) and iterative parser (`Json.parse_raw`).

use std::borrow::Cow;
use std::collections::{HashSet, VecDeque};

use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_def::limits::MO_LIMIT_COLLECTION_SIZE;
use mo_heap::{Heap, Value};

use crate::{PrimError, check_value_bytes_fit};

/// Maximum array/object nesting depth permitted by Mo Reference §14 (`lib/json.mo`).
const MAX_JSON_DEPTH: usize = 128;

/// Token tags shared with `lib/json.mo` (`Json.parse`):
/// - `0`: `Null`
/// - `1`: `Boolean(false)`
/// - `2`: `Boolean(true)`
/// - `3`: `Number(JsonNumber)` (consumes 1 text from `texts`)
/// - `4`: `String(Text)` (consumes 1 text from `texts`)
/// - `10 + 2 * count`: `Array` of `count` elements
/// - `11 + 2 * count`: `Object` of `count` `(key, value)` entries (each key consumed from `texts`)
const TAG_NULL: i64 = 0;
const TAG_FALSE: i64 = 1;
const TAG_TRUE: i64 = 2;
const TAG_NUMBER: i64 = 3;
const TAG_STRING: i64 = 4;
const TAG_ARRAY_BASE: i64 = 10;
const TAG_OBJECT_BASE: i64 = 11;

/// Error kind codes shared with `lib/json.mo` (`JsonProblem`):
const ERR_UNEXPECTED_END: i64 = 0;
const ERR_UNEXPECTED: i64 = 1;
const ERR_BAD_NUMBER: i64 = 2;
const ERR_BAD_ESCAPE: i64 = 3;
const ERR_DUPLICATE_KEY: i64 = 4;
const ERR_TOO_DEEP: i64 = 5;

/// Small-object linear key set that promotes to a `HashSet` only above 16 keys.
enum KeySet<'a> {
    Small(Vec<Cow<'a, str>>),
    Large(HashSet<Cow<'a, str>>),
}

impl<'a> KeySet<'a> {
    fn new() -> Self {
        Self::Small(Vec::new())
    }

    /// Inserts `key` and returns `true` iff it was not already present.
    fn insert(&mut self, key: Cow<'a, str>) -> bool {
        match self {
            Self::Small(items) => {
                if items
                    .iter()
                    .any(|existing| existing.as_ref() == key.as_ref())
                {
                    return false;
                }
                if items.len() < 16 {
                    items.push(key);
                    return true;
                }
                let mut set = HashSet::with_capacity(items.len() + 1);
                for existing in items.drain(..) {
                    set.insert(existing);
                }
                let inserted = set.insert(key);
                *self = Self::Large(set);
                inserted
            }
            Self::Large(set) => set.insert(key),
        }
    }
}

enum ContainerFrame<'a> {
    Array {
        tag_slot: usize,
        count: usize,
    },
    Object {
        tag_slot: usize,
        count: usize,
        keys: KeySet<'a>,
    },
}

enum ParseFailure {
    Syntax(ParseErr),
    Prim(PrimError),
}

impl From<ParseErr> for ParseFailure {
    fn from(err: ParseErr) -> Self {
        Self::Syntax(err)
    }
}

impl From<PrimError> for ParseFailure {
    fn from(err: PrimError) -> Self {
        Self::Prim(err)
    }
}

#[derive(Debug, PartialEq, Eq)]
enum ParseErr {
    UnexpectedEnd(usize),
    Unexpected(usize, char),
    BadNumber(usize),
    BadEscape(usize),
    DuplicateKey(usize, String),
    TooDeep(usize),
}

impl ParseErr {
    fn byte_offset(&self) -> usize {
        match *self {
            Self::UnexpectedEnd(at)
            | Self::Unexpected(at, _)
            | Self::BadNumber(at)
            | Self::BadEscape(at)
            | Self::DuplicateKey(at, _)
            | Self::TooDeep(at) => at,
        }
    }

    fn into_kind_and_detail(self) -> (i64, String) {
        match self {
            Self::UnexpectedEnd(_) => (ERR_UNEXPECTED_END, String::new()),
            Self::Unexpected(_, ch) => (ERR_UNEXPECTED, ch.to_string()),
            Self::BadNumber(_) => (ERR_BAD_NUMBER, String::new()),
            Self::BadEscape(_) => (ERR_BAD_ESCAPE, String::new()),
            Self::DuplicateKey(_, key) => (ERR_DUPLICATE_KEY, key),
            Self::TooDeep(_) => (ERR_TOO_DEEP, String::new()),
        }
    }
}

/// Quotes and RFC 8259-escapes `s` into a JSON string literal (`"..."`).
pub(crate) fn quote_json_string(s: &str) -> Result<String, PrimError> {
    let bytes = s.as_bytes();
    let first_special = bytes
        .iter()
        .position(|&b| b < 0x20 || b == b'"' || b == b'\\');

    let Some(first) = first_special else {
        let total = bytes
            .len()
            .checked_add(2)
            .ok_or(PrimError::OutOfMemory(Some(
                "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
            )))?;
        check_value_bytes_fit(total)?;
        let mut out = String::with_capacity(total);
        out.push('"');
        out.push_str(s);
        out.push('"');
        return Ok(out);
    };

    // Exact output length pass so we allocate once and uphold MO_LIMIT_VALUE_BYTES up front.
    let mut total: usize = 2 + first;
    for &b in &bytes[first..] {
        let extra = match b {
            b'"' | b'\\' | b'\n' | b'\r' | b'\t' | 0x08 | 0x0C => 2,
            0x00..=0x1F => 6,
            _ => 1,
        };
        total = total.checked_add(extra).ok_or(PrimError::OutOfMemory(Some(
            "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
        )))?;
        check_value_bytes_fit(total)?;
    }

    let mut out = String::with_capacity(total);
    out.push('"');
    let mut chunk_start = 0usize;
    let mut i = first;
    while i < bytes.len() {
        let b = bytes[i];
        if b < 0x20 || b == b'"' || b == b'\\' {
            if chunk_start < i {
                out.push_str(&s[chunk_start..i]);
            }
            match b {
                b'"' => out.push_str("\\\""),
                b'\\' => out.push_str("\\\\"),
                b'\n' => out.push_str("\\n"),
                b'\r' => out.push_str("\\r"),
                b'\t' => out.push_str("\\t"),
                0x08 => out.push_str("\\b"),
                0x0C => out.push_str("\\f"),
                _ => {
                    const HEX: &[u8; 16] = b"0123456789abcdef";
                    let esc = [
                        b'\\',
                        b'u',
                        b'0',
                        b'0',
                        HEX[usize::from(b >> 4)],
                        HEX[usize::from(b & 0x0F)],
                    ];
                    if let Ok(esc_str) = std::str::from_utf8(&esc) {
                        out.push_str(esc_str);
                    }
                }
            }
            i += 1;
            chunk_start = i;
        } else {
            i += 1;
        }
    }
    if chunk_start < bytes.len() {
        out.push_str(&s[chunk_start..]);
    }
    out.push('"');
    Ok(out)
}

#[inline]
fn skip_ws(bytes: &[u8], mut pos: usize) -> usize {
    while pos < bytes.len() {
        match bytes[pos] {
            b' ' | b'\t' | b'\n' | b'\r' => pos += 1,
            _ => break,
        }
    }
    pos
}

#[inline]
fn first_char_at(s: &str, byte_pos: usize) -> char {
    s[byte_pos..].chars().next().unwrap_or('\0')
}

fn byte_offset_to_line_col(s: &str, byte_offset: usize) -> (i64, i64) {
    let mut line: i64 = 1;
    let mut col: i64 = 1;
    for ch in s[..byte_offset].chars() {
        if ch == '\n' {
            line += 1;
            col = 1;
        } else {
            col += 1;
        }
    }
    (line, col)
}

fn hex_val(ch: char) -> Option<u32> {
    match ch {
        '0'..='9' => Some(u32::from(ch as u8 - b'0')),
        'a'..='f' => Some(u32::from(ch as u8 - b'a' + 10)),
        'A'..='F' => Some(u32::from(ch as u8 - b'A' + 10)),
        _ => None,
    }
}

fn read_hex4(s: &str, start: usize, esc_pos: usize) -> Result<(u32, usize), ParseErr> {
    let mut iter = s[start..].chars();
    let mut val = 0u32;
    let mut consumed = 0usize;
    for _ in 0..4 {
        let Some(ch) = iter.next() else {
            return Err(ParseErr::UnexpectedEnd(s.len()));
        };
        let Some(d) = hex_val(ch) else {
            return Err(ParseErr::BadEscape(esc_pos));
        };
        val = (val << 4) | d;
        consumed += ch.len_utf8();
    }
    Ok((val, start + consumed))
}

fn parse_string(s: &str, quote_pos: usize) -> Result<(Cow<'_, str>, usize), ParseErr> {
    let bytes = s.as_bytes();
    let start = quote_pos + 1;
    let mut i = start;
    while i < bytes.len() {
        let b = bytes[i];
        if b == b'"' {
            return Ok((Cow::Borrowed(&s[start..i]), i + 1));
        }
        if b == b'\\' {
            break;
        }
        if b < 0x20 {
            return Err(ParseErr::Unexpected(i, char::from(b)));
        }
        i += 1;
    }
    if i >= bytes.len() {
        return Err(ParseErr::UnexpectedEnd(bytes.len()));
    }

    // Slow path: string contains at least one escape sequence.
    let mut out = String::with_capacity(i - start + 16);
    out.push_str(&s[start..i]);
    let mut chunk_start = i;
    while i < bytes.len() {
        let b = bytes[i];
        if b == b'"' {
            if chunk_start < i {
                out.push_str(&s[chunk_start..i]);
            }
            return Ok((Cow::Owned(out), i + 1));
        }
        if b == b'\\' {
            if chunk_start < i {
                out.push_str(&s[chunk_start..i]);
            }
            let esc_pos = i;
            let Some(&esc) = bytes.get(i + 1) else {
                return Err(ParseErr::UnexpectedEnd(bytes.len()));
            };
            match esc {
                b'"' => {
                    out.push('"');
                    i += 2;
                }
                b'\\' => {
                    out.push('\\');
                    i += 2;
                }
                b'/' => {
                    out.push('/');
                    i += 2;
                }
                b'b' => {
                    out.push('\u{0008}');
                    i += 2;
                }
                b'f' => {
                    out.push('\u{000C}');
                    i += 2;
                }
                b'n' => {
                    out.push('\n');
                    i += 2;
                }
                b'r' => {
                    out.push('\r');
                    i += 2;
                }
                b't' => {
                    out.push('\t');
                    i += 2;
                }
                b'u' => {
                    let (code, after_hi) = read_hex4(s, i + 2, esc_pos)?;
                    if (0xD800..0xDC00).contains(&code) {
                        if bytes.get(after_hi) != Some(&b'\\')
                            || bytes.get(after_hi + 1) != Some(&b'u')
                        {
                            return Err(ParseErr::BadEscape(esc_pos));
                        }
                        let (low, after_lo) = read_hex4(s, after_hi + 2, esc_pos)?;
                        if !(0xDC00..0xE000).contains(&low) {
                            return Err(ParseErr::BadEscape(esc_pos));
                        }
                        let scalar = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        let Some(ch) = char::from_u32(scalar) else {
                            return Err(ParseErr::BadEscape(esc_pos));
                        };
                        out.push(ch);
                        i = after_lo;
                    } else {
                        let Some(ch) = char::from_u32(code) else {
                            return Err(ParseErr::BadEscape(esc_pos));
                        };
                        out.push(ch);
                        i = after_hi;
                    }
                }
                _ => return Err(ParseErr::BadEscape(esc_pos)),
            }
            chunk_start = i;
        } else if b < 0x20 {
            return Err(ParseErr::Unexpected(i, char::from(b)));
        } else {
            i += 1;
        }
    }
    Err(ParseErr::UnexpectedEnd(bytes.len()))
}

fn match_word(s: &str, start: usize, word: &[u8]) -> Result<usize, ParseErr> {
    let bytes = s.as_bytes();
    let mut pos = start;
    for &wanted in word {
        match bytes.get(pos) {
            Some(&got) if got == wanted => pos += 1,
            Some(_) => return Err(ParseErr::Unexpected(pos, first_char_at(s, pos))),
            None => return Err(ParseErr::UnexpectedEnd(bytes.len())),
        }
    }
    Ok(pos)
}

fn consume_digits(bytes: &[u8], from: usize) -> Result<usize, ParseErr> {
    if !bytes.get(from).is_some_and(u8::is_ascii_digit) {
        return Err(ParseErr::BadNumber(from));
    }
    let mut at = from + 1;
    while bytes.get(at).is_some_and(u8::is_ascii_digit) {
        at += 1;
    }
    Ok(at)
}

fn parse_number(s: &str, start: usize) -> Result<(&str, usize), ParseErr> {
    let bytes = s.as_bytes();
    let mut at = start;
    if bytes.get(at) == Some(&b'-') {
        at += 1;
    }
    if bytes.get(at) == Some(&b'0') {
        at += 1;
    } else {
        at = consume_digits(bytes, at)?;
    }
    if bytes.get(at) == Some(&b'.') {
        at = consume_digits(bytes, at + 1)?;
    }
    if matches!(bytes.get(at), Some(b'e' | b'E')) {
        at += 1;
        if matches!(bytes.get(at), Some(b'+' | b'-')) {
            at += 1;
        }
        at = consume_digits(bytes, at)?;
    }
    if bytes.get(at).is_some_and(u8::is_ascii_digit) {
        return Err(ParseErr::BadNumber(at));
    }
    Ok((&s[start..at], at))
}

fn push_tag(tags: &mut Vec<Value>, tag: i64) -> Result<(), PrimError> {
    if (tags.len() as u64) >= MO_LIMIT_COLLECTION_SIZE {
        return Err(PrimError::OutOfMemory(None));
    }
    tags.push(Value::Int(tag));
    Ok(())
}

fn push_text(heap: &Heap, texts: &mut Vec<Value>, text: &str) -> Result<(), PrimError> {
    if (texts.len() as u64) >= MO_LIMIT_COLLECTION_SIZE {
        return Err(PrimError::OutOfMemory(None));
    }
    check_value_bytes_fit(text.len())?;
    texts.push(heap.alloc_text(text.to_owned())?);
    Ok(())
}

fn container_tag(base: i64, count: usize) -> Result<i64, PrimError> {
    let c = i64::try_from(count).map_err(|_| PrimError::OutOfMemory(None))?;
    c.checked_mul(2)
        .and_then(|v| v.checked_add(base))
        .ok_or(PrimError::OutOfMemory(None))
}

fn parse_document_tokens(s: &str, heap: &Heap) -> Result<(Vec<Value>, Vec<Value>), ParseFailure> {
    let bytes = s.as_bytes();
    let mut pos = skip_ws(bytes, 0);
    let mut tags: Vec<Value> = Vec::new();
    let mut texts: Vec<Value> = Vec::new();
    let mut stack: Vec<ContainerFrame<'_>> = Vec::new();

    loop {
        let Some(&b) = bytes.get(pos) else {
            return Err(ParseErr::UnexpectedEnd(bytes.len()).into());
        };
        match b {
            b'"' => {
                let (cow, next) = parse_string(s, pos)?;
                push_tag(&mut tags, TAG_STRING)?;
                push_text(heap, &mut texts, cow.as_ref())?;
                pos = next;
            }
            b'n' => {
                pos = match_word(s, pos, b"null")?;
                push_tag(&mut tags, TAG_NULL)?;
            }
            b't' => {
                pos = match_word(s, pos, b"true")?;
                push_tag(&mut tags, TAG_TRUE)?;
            }
            b'f' => {
                pos = match_word(s, pos, b"false")?;
                push_tag(&mut tags, TAG_FALSE)?;
            }
            b'-' | b'0'..=b'9' => {
                let (num, next) = parse_number(s, pos)?;
                push_tag(&mut tags, TAG_NUMBER)?;
                push_text(heap, &mut texts, num)?;
                pos = next;
            }
            b'[' => {
                if stack.len() >= MAX_JSON_DEPTH {
                    return Err(ParseErr::TooDeep(pos).into());
                }
                let tag_slot = tags.len();
                push_tag(&mut tags, TAG_ARRAY_BASE)?;
                let after = skip_ws(bytes, pos + 1);
                if bytes.get(after) == Some(&b']') {
                    pos = after + 1;
                } else {
                    stack.push(ContainerFrame::Array { tag_slot, count: 0 });
                    pos = after;
                    continue;
                }
            }
            b'{' => {
                if stack.len() >= MAX_JSON_DEPTH {
                    return Err(ParseErr::TooDeep(pos).into());
                }
                let tag_slot = tags.len();
                push_tag(&mut tags, TAG_OBJECT_BASE)?;
                let after = skip_ws(bytes, pos + 1);
                if bytes.get(after) == Some(&b'}') {
                    pos = after + 1;
                } else {
                    if after >= bytes.len() {
                        return Err(ParseErr::UnexpectedEnd(bytes.len()).into());
                    }
                    if bytes[after] != b'"' {
                        return Err(ParseErr::Unexpected(after, first_char_at(s, after)).into());
                    }
                    let (key_cow, after_key) = parse_string(s, after)?;
                    let mut keys = KeySet::new();
                    push_text(heap, &mut texts, key_cow.as_ref())?;
                    keys.insert(key_cow);
                    let colon = skip_ws(bytes, after_key);
                    if colon >= bytes.len() {
                        return Err(ParseErr::UnexpectedEnd(bytes.len()).into());
                    }
                    if bytes[colon] != b':' {
                        return Err(ParseErr::Unexpected(colon, first_char_at(s, colon)).into());
                    }
                    stack.push(ContainerFrame::Object {
                        tag_slot,
                        count: 0,
                        keys,
                    });
                    pos = skip_ws(bytes, colon + 1);
                    continue;
                }
            }
            _ => return Err(ParseErr::Unexpected(pos, first_char_at(s, pos)).into()),
        }

        // Complete pending containers until we either reach the next sibling or finish the root.
        loop {
            let Some(frame) = stack.last_mut() else {
                let end = skip_ws(bytes, pos);
                if end < bytes.len() {
                    return Err(ParseErr::Unexpected(end, first_char_at(s, end)).into());
                }
                return Ok((tags, texts));
            };
            match frame {
                ContainerFrame::Array { tag_slot, count } => {
                    *count += 1;
                    let next = skip_ws(bytes, pos);
                    match bytes.get(next) {
                        Some(&b',') => {
                            pos = skip_ws(bytes, next + 1);
                            break;
                        }
                        Some(&b']') => {
                            tags[*tag_slot] = Value::Int(container_tag(TAG_ARRAY_BASE, *count)?);
                            pos = next + 1;
                            stack.pop();
                        }
                        Some(_) => {
                            return Err(ParseErr::Unexpected(next, first_char_at(s, next)).into());
                        }
                        None => return Err(ParseErr::UnexpectedEnd(bytes.len()).into()),
                    }
                }
                ContainerFrame::Object {
                    tag_slot,
                    count,
                    keys,
                } => {
                    *count += 1;
                    let next = skip_ws(bytes, pos);
                    match bytes.get(next) {
                        Some(&b',') => {
                            let key_pos = skip_ws(bytes, next + 1);
                            if key_pos >= bytes.len() {
                                return Err(ParseErr::UnexpectedEnd(bytes.len()).into());
                            }
                            if bytes[key_pos] != b'"' {
                                return Err(ParseErr::Unexpected(
                                    key_pos,
                                    first_char_at(s, key_pos),
                                )
                                .into());
                            }
                            let (key_cow, after_key) = parse_string(s, key_pos)?;
                            if !keys.insert(key_cow.clone()) {
                                return Err(
                                    ParseErr::DuplicateKey(key_pos, key_cow.into_owned()).into()
                                );
                            }
                            push_text(heap, &mut texts, key_cow.as_ref())?;
                            let colon = skip_ws(bytes, after_key);
                            if colon >= bytes.len() {
                                return Err(ParseErr::UnexpectedEnd(bytes.len()).into());
                            }
                            if bytes[colon] != b':' {
                                return Err(
                                    ParseErr::Unexpected(colon, first_char_at(s, colon)).into()
                                );
                            }
                            pos = skip_ws(bytes, colon + 1);
                            break;
                        }
                        Some(&b'}') => {
                            tags[*tag_slot] = Value::Int(container_tag(TAG_OBJECT_BASE, *count)?);
                            pos = next + 1;
                            stack.pop();
                        }
                        Some(_) => {
                            return Err(ParseErr::Unexpected(next, first_char_at(s, next)).into());
                        }
                        None => return Err(ParseErr::UnexpectedEnd(bytes.len()).into()),
                    }
                }
            }
        }
    }
}

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    result_ty: u32,
    args: &mut [Value],
) -> Result<Value, PrimError> {
    match prim {
        PrimId::JsonQuoteRaw => {
            let quoted = quote_json_string(args[0].as_text())?;
            Ok(heap.alloc_text(quoted)?)
        }
        PrimId::JsonParseRaw => {
            let src = args[0].as_text();
            let res_def = types.get(result_ty);
            let ok_tuple_ty = res_def.cases[0].fields[0].ty;
            let err_tuple_ty = res_def.cases[1].fields[0].ty;
            match parse_document_tokens(src, heap) {
                Ok((tags, texts)) => {
                    let Some(ok_layout) = types.find_tuple_layout(ok_tuple_ty) else {
                        return Err(PrimError::Outside(
                            "missing Ok tuple layout for Json.parse_raw",
                        ));
                    };
                    let tags_val = heap.alloc_list(VecDeque::from(tags))?;
                    let texts_val = heap.alloc_list(VecDeque::from(texts))?;
                    let pair = heap.construct(types, ok_layout, vec![tags_val, texts_val])?;
                    Ok(heap.make_result(types, result_ty, Ok(pair))?)
                }
                Err(ParseFailure::Prim(e)) => Err(e),
                Err(ParseFailure::Syntax(err)) => {
                    let Some(err_layout) = types.find_tuple_layout(err_tuple_ty) else {
                        return Err(PrimError::Outside(
                            "missing Error tuple layout for Json.parse_raw",
                        ));
                    };
                    let (line, col) = byte_offset_to_line_col(src, err.byte_offset());
                    let (kind, detail) = err.into_kind_and_detail();
                    check_value_bytes_fit(detail.len())?;
                    let detail_val = heap.alloc_text(detail)?;
                    let quad = heap.construct(
                        types,
                        err_layout,
                        vec![
                            Value::Int(line),
                            Value::Int(col),
                            Value::Int(kind),
                            detail_val,
                        ],
                    )?;
                    Ok(heap.make_result(types, result_ty, Err(quad))?)
                }
            }
        }
        _ => Err(PrimError::UnimplementedPrim(prim)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quote_fast_and_escaped_paths() {
        assert_eq!(quote_json_string("hello").unwrap(), "\"hello\"");
        assert_eq!(
            quote_json_string("say \"hi\"\n").unwrap(),
            "\"say \\\"hi\\\"\\n\""
        );
        assert_eq!(
            quote_json_string("tab\tbell\u{7}/é").unwrap(),
            "\"tab\\tbell\\u0007/é\""
        );
        assert_eq!(
            quote_json_string("\u{1f}\u{0} \u{8}\u{c}\r\\").unwrap(),
            "\"\\u001f\\u0000 \\b\\f\\r\\\\\""
        );
    }

    #[test]
    fn parse_exact_error_positions_and_surrogates() {
        let heap = Heap::new(1 << 20);
        let check_err = |src: &str, want: (i64, i64, i64, &str)| {
            let got = match parse_document_tokens(src, &heap) {
                Err(ParseFailure::Syntax(err)) => {
                    let (l, c) = byte_offset_to_line_col(src, err.byte_offset());
                    let (k, d) = err.into_kind_and_detail();
                    Some((l, c, k, d))
                }
                _ => None,
            };
            assert_eq!(
                got,
                Some((want.0, want.1, want.2, want.3.to_owned())),
                "input={src:?}"
            );
        };

        check_err("", (1, 1, ERR_UNEXPECTED_END, ""));
        check_err("  \n ", (2, 2, ERR_UNEXPECTED_END, ""));
        check_err("tru", (1, 4, ERR_UNEXPECTED_END, ""));
        check_err("trux", (1, 4, ERR_UNEXPECTED, "x"));
        check_err("[1,]", (1, 4, ERR_UNEXPECTED, "]"));
        check_err("{\"a\": 1,}", (1, 9, ERR_UNEXPECTED, "}"));
        check_err("-01", (1, 3, ERR_BAD_NUMBER, ""));
        check_err("01", (1, 2, ERR_BAD_NUMBER, ""));
        check_err("\"\\ud834\"", (1, 2, ERR_BAD_ESCAPE, ""));
        check_err("\"\\u12", (1, 6, ERR_UNEXPECTED_END, ""));
        check_err(
            "[{\"a\": [1], \"b\": {\"a\": 2}, \"a\": 3}]",
            (1, 28, ERR_DUPLICATE_KEY, "a"),
        );
        let deep_bad = format!("{}{{}}", "{\"a\":".repeat(128));
        check_err(&deep_bad, (1, 641, ERR_TOO_DEEP, ""));
    }

    #[test]
    fn parse_large_object_promotes_key_set() {
        let heap = Heap::new(1 << 20);
        let mut fields = Vec::new();
        for i in 0..25 {
            fields.push(format!("\"k{i}\":{i}"));
        }
        let ok_src = format!("{{{}}}", fields.join(","));
        let (tags, texts) = parse_document_tokens(&ok_src, &heap)
            .ok()
            .expect("25 distinct keys parse");
        assert_eq!(tags.len(), 26);
        assert_eq!(tags[0].as_int(), TAG_OBJECT_BASE + 50);
        assert_eq!(texts.len(), 50);

        let dup_src = format!("{{{},\"k3\":99}}", fields.join(","));
        assert!(matches!(
            parse_document_tokens(&dup_src, &heap),
            Err(ParseFailure::Syntax(ParseErr::DuplicateKey(_, k))) if k == "k3"
        ));
    }
}
