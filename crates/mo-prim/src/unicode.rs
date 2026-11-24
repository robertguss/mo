use std::collections::VecDeque;

use mo_def::PrimId;
use mo_heap::{Heap, ScratchGuard, Value};

use crate::{PrimError, check_value_bytes_fit};

use crate::unicode16_data::{
    GCB_CONTROL, GCB_CR, GCB_EXTEND, GCB_L, GCB_LF, GCB_LV, GCB_LVT, GCB_PREPEND,
    GCB_REGIONAL_INDICATOR, GCB_SPACINGMARK, GCB_T, GCB_V, GCB_ZWJ, UCD_CASED, UCD_DIGIT,
    UCD_EMOJI, UCD_IGNORABLE_CASE, UCD_LETTER, UCD_PICTOGRAPHIC, UCD_VARIANT, UCD_WIDE, UCD_ZERO,
    UNICODE_ASCII, UNICODE_COMPOSE, UNICODE_DECOMPOSE, UNICODE_LOWER, UNICODE_RANGES,
    UNICODE_SCALARS, UNICODE_UPPER, UnicodeMapping,
};

const SCALAR_MASK: u32 = 0x1f_ffff;

fn properties(c: u32) -> u32 {
    if c < 128 {
        return UNICODE_ASCII[c as usize];
    }
    let mut lo = 0usize;
    let mut hi = UNICODE_RANGES.len();
    let mut result = 0u32;
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let r = UNICODE_RANGES[mid];
        if c < r.first {
            hi = mid;
        } else if c > r.last {
            lo = mid + 1;
        } else {
            result = r.properties;
            break;
        }
    }
    if (0xac00..=0xd7a3).contains(&c) {
        let gcb = if (c - 0xac00).is_multiple_of(28) {
            GCB_LV
        } else {
            GCB_LVT
        };
        result = (result & !15) | gcb;
    }
    result
}

fn find_mapping(c: u32, table: &[UnicodeMapping]) -> Option<UnicodeMapping> {
    table
        .binary_search_by_key(&c, |m| m.scalar)
        .ok()
        .map(|idx| table[idx])
}

#[derive(Default)]
struct Boundary {
    previous: u32,
    indic: u32,
    started: bool,
    odd_ri: bool,
    pictographic_extend: bool,
    pictographic_zwj: bool,
}

impl Boundary {
    /// UAX #29 revision 45, in rule order — exact port of `src/prim/unicode.c:boundary`.
    fn next(&mut self, p: u32) -> bool {
        let a = self.previous & 15;
        let b = p & 15;
        let indic = (p >> 4) & 3;
        let control_a = a == GCB_CR || a == GCB_LF || a == GCB_CONTROL;
        let control_b = b == GCB_CR || b == GCB_LF || b == GCB_CONTROL;

        let split = if !self.started {
            true
        } else if a == GCB_CR && b == GCB_LF {
            false
        } else if control_a || control_b {
            true
        } else {
            let join = (a == GCB_L && (b == GCB_L || b == GCB_V || b == GCB_LV || b == GCB_LVT))
                || ((a == GCB_LV || a == GCB_V) && (b == GCB_V || b == GCB_T))
                || ((a == GCB_LVT || a == GCB_T) && b == GCB_T)
                || b == GCB_EXTEND
                || b == GCB_ZWJ
                || b == GCB_SPACINGMARK
                || a == GCB_PREPEND
                || (self.indic == 2 && indic == 1)
                || (a == GCB_ZWJ && self.pictographic_zwj && (p & UCD_PICTOGRAPHIC) != 0)
                || (a == GCB_REGIONAL_INDICATOR && b == GCB_REGIONAL_INDICATOR && self.odd_ri);
            !join
        };

        self.odd_ri = b == GCB_REGIONAL_INDICATOR && !self.odd_ri;
        self.pictographic_zwj = b == GCB_ZWJ && self.pictographic_extend;
        if (p & UCD_PICTOGRAPHIC) != 0 {
            self.pictographic_extend = true;
        } else if b != GCB_EXTEND {
            self.pictographic_extend = false;
        }
        if indic == 1 {
            self.indic = 1;
        } else if indic == 3 {
            self.indic = if self.indic != 0 { 2 } else { 0 };
        } else if indic != 2 {
            self.indic = 0;
        }
        self.previous = p;
        self.started = true;
        split
    }
}

fn is_keycap(c: u32) -> bool {
    c == u32::from(b'#') || c == u32::from(b'*') || (u32::from(b'0')..=u32::from(b'9')).contains(&c)
}

pub(crate) fn display_width_str(s: &str) -> u64 {
    let mut state = Boundary::default();
    let mut total = 0u64;
    let mut cluster_width = 0u32;
    let mut previous = 0u32;
    let mut before_previous = 0u32;

    let mut chars = s.chars().peekable();
    while let Some(ch) = chars.next() {
        let cp = ch as u32;
        let p = properties(cp);
        if state.next(p) {
            total += u64::from(cluster_width);
            cluster_width = 0;
            before_previous = 0;
            previous = 0;
        }
        let control = cp < 0x20 || (0x7f..=0x9f).contains(&cp);
        let mut w = if control || (p & UCD_ZERO) != 0 {
            0
        } else if (p & UCD_WIDE) != 0 {
            2
        } else {
            1
        };
        let next = chars.peek().map_or(0, |&c| c as u32);
        let emoji = (p & UCD_EMOJI) != 0
            || (next == 0xfe0f && (p & UCD_VARIANT) != 0)
            || (cp == 0x20e3
                && (is_keycap(previous) || (previous == 0xfe0f && is_keycap(before_previous))));
        if (p & 15) == GCB_REGIONAL_INDICATOR {
            w = if state.odd_ri { 1 } else { 2 };
        } else if emoji {
            w = 2;
        }
        if w > cluster_width {
            cluster_width = w;
        }
        before_previous = previous;
        previous = cp;
    }
    total + u64::from(cluster_width)
}

#[cfg(test)]
pub(crate) fn graphemes_slices(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut state = Boundary::default();
    let mut start = 0usize;
    for (idx, ch) in s.char_indices() {
        if state.next(properties(ch as u32)) {
            if idx != 0 {
                out.push(&s[start..idx]);
            }
            start = idx;
        }
    }
    if !s.is_empty() {
        out.push(&s[start..]);
    }
    out
}

fn graphemes(heap: &Heap, s: &str) -> Result<Value, PrimError> {
    let mut list = VecDeque::new();
    let mut state = Boundary::default();
    let mut start = 0usize;
    for (idx, ch) in s.char_indices() {
        if state.next(properties(ch as u32)) {
            if idx != 0 {
                list.push_back(heap.alloc_text(s[start..idx].to_string())?);
            }
            start = idx;
        }
    }
    if !s.is_empty() {
        list.push_back(heap.alloc_text(s[start..].to_string())?);
    }
    Ok(heap.alloc_list(list)?)
}

fn following_cased(rest: &str) -> bool {
    for ch in rest.chars() {
        let p = properties(ch as u32);
        if (p & UCD_IGNORABLE_CASE) == 0 {
            return (p & UCD_CASED) != 0;
        }
    }
    false
}

fn utf8_len(cp: u32) -> usize {
    if cp < 0x80 {
        1
    } else if cp < 0x800 {
        2
    } else if cp < 0x1_0000 {
        3
    } else {
        4
    }
}

pub(crate) fn casing_str(s: &str, lower: bool) -> Result<String, PrimError> {
    let table = if lower { UNICODE_LOWER } else { UNICODE_UPPER };
    let mut needed = 0usize;
    let mut preceding = false;
    for (idx, ch) in s.char_indices() {
        let cp = ch as u32;
        let p = properties(cp);
        let mut m = find_mapping(cp, table);
        let mut one = cp;
        if lower && cp == 0x3a3 && preceding && !following_cased(&s[idx + ch.len_utf8()..]) {
            m = None;
            one = 0x3c2;
        }
        if (p & UCD_IGNORABLE_CASE) == 0 {
            preceding = (p & UCD_CASED) != 0;
        }
        let values: &[u32] = match m {
            None => std::slice::from_ref(&one),
            Some(entry) => {
                let start = entry.offset as usize;
                &UNICODE_SCALARS[start..start + entry.count as usize]
            }
        };
        for &v in values {
            let n = utf8_len(v);
            needed = needed.checked_add(n).ok_or(PrimError::OutOfMemory(Some(
                "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
            )))?;
            check_value_bytes_fit(needed)?;
        }
    }

    let mut out = String::with_capacity(needed);
    let mut preceding = false;
    for (idx, ch) in s.char_indices() {
        let cp = ch as u32;
        let p = properties(cp);
        let mut m = find_mapping(cp, table);
        let mut one = cp;
        if lower && cp == 0x3a3 && preceding && !following_cased(&s[idx + ch.len_utf8()..]) {
            m = None;
            one = 0x3c2;
        }
        if (p & UCD_IGNORABLE_CASE) == 0 {
            preceding = (p & UCD_CASED) != 0;
        }
        let values: &[u32] = match m {
            None => std::slice::from_ref(&one),
            Some(entry) => {
                let start = entry.offset as usize;
                &UNICODE_SCALARS[start..start + entry.count as usize]
            }
        };
        for &v in values {
            if let Some(c) = char::from_u32(v) {
                out.push(c);
            }
        }
    }
    debug_assert_eq!(out.len(), needed);
    Ok(out)
}

fn decompose(cp: u32, out: &mut [u32; 4]) -> usize {
    let hangul = cp.wrapping_sub(0xac00);
    if hangul < 11_172 {
        out[0] = 0x1100 + hangul / 588;
        out[1] = 0x1161 + (hangul % 588) / 28;
        out[2] = 0x11a7 + (hangul % 28);
        return if hangul.is_multiple_of(28) { 2 } else { 3 };
    }
    match find_mapping(cp, UNICODE_DECOMPOSE) {
        None => {
            out[0] = cp;
            1
        }
        Some(m) => {
            let count = m.count as usize;
            debug_assert!(count <= 4);
            let start = m.offset as usize;
            out[..count].copy_from_slice(&UNICODE_SCALARS[start..start + count]);
            count
        }
    }
}

fn canonical_order(scratch: &mut ScratchGuard<'_>, values: &mut [u32]) -> Result<(), PrimError> {
    let count = values.len();
    let mut temporary: Vec<u32> = Vec::new();
    let mut at = 0usize;
    while at < count {
        if (values[at] >> 21) == 0 {
            at += 1;
            continue;
        }
        let start = at;
        at += 1;
        let mut sorted = true;
        while at < count && (values[at] >> 21) != 0 {
            sorted = sorted && (values[at - 1] >> 21) <= (values[at] >> 21);
            at += 1;
        }
        let n = at - start;
        if sorted {
            continue;
        }
        if n <= 32 {
            for i in (start + 1)..at {
                let v = values[i];
                let mut j = i;
                while j > start && (values[j - 1] >> 21) > (v >> 21) {
                    values[j] = values[j - 1];
                    j -= 1;
                }
                values[j] = v;
            }
            continue;
        }
        if n > temporary.len() {
            let extra_bytes = (n - temporary.len()).saturating_mul(size_of::<u32>());
            scratch.charge(extra_bytes)?;
            temporary.resize(n, 0);
        }
        let mut counts = [0usize; 256];
        for &v in &values[start..at] {
            counts[(v >> 21) as usize] += 1;
        }
        let mut positions = [0usize; 256];
        let mut sum = 0usize;
        for i in 0..256 {
            positions[i] = sum;
            sum += counts[i];
        }
        for &v in &values[start..at] {
            let cls = (v >> 21) as usize;
            temporary[positions[cls]] = v;
            positions[cls] += 1;
        }
        values[start..at].copy_from_slice(&temporary[..n]);
    }
    Ok(())
}

fn compose(a: u32, b: u32) -> u32 {
    if (0x1100..0x1113).contains(&a) && (0x1161..0x1176).contains(&b) {
        return 0xac00 + (a - 0x1100) * 588 + (b - 0x1161) * 28;
    }
    if (0xac00..=0xd7a3).contains(&a)
        && (a - 0xac00).is_multiple_of(28)
        && (0x11a8..0x11c3).contains(&b)
    {
        return a + b - 0x11a7;
    }
    UNICODE_COMPOSE
        .binary_search_by(|p| (p.left, p.right).cmp(&(a, b)))
        .ok()
        .map_or(0, |idx| UNICODE_COMPOSE[idx].result)
}

fn normalize_nfc(heap: &Heap, s: &str) -> Result<Value, PrimError> {
    let mut scratch = ScratchGuard::new(heap.budget());
    let mut ds = [0u32; 4];
    let mut count = 0usize;
    for ch in s.chars() {
        count += decompose(ch as u32, &mut ds);
    }
    scratch.charge(count.saturating_mul(size_of::<u32>()))?;
    let mut values = Vec::with_capacity(count);
    for ch in s.chars() {
        let n = decompose(ch as u32, &mut ds);
        for &cp in &ds[..n] {
            let ccc = (properties(cp) >> 16) & 255;
            values.push(cp | (ccc << 21));
        }
    }
    debug_assert_eq!(values.len(), count);
    canonical_order(&mut scratch, &mut values)?;

    let mut starter: Option<usize> = None;
    let mut last_class = 0u32;
    let mut wrote = 0usize;
    for i in 0..count {
        let v = values[i];
        let cp = v & SCALAR_MASK;
        let cls = v >> 21;
        let combined = match starter {
            Some(st) if last_class == 0 || last_class < cls => {
                compose(values[st] & SCALAR_MASK, cp)
            }
            _ => 0,
        };
        if let Some(st) = starter
            && combined != 0
        {
            values[st] = combined;
        } else {
            if cls == 0 {
                starter = Some(wrote);
            }
            values[wrote] = v;
            wrote += 1;
            last_class = cls;
        }
    }

    let mut needed = 0usize;
    for &v in &values[..wrote] {
        let n = utf8_len(v & SCALAR_MASK);
        needed = needed.checked_add(n).ok_or(PrimError::OutOfMemory(Some(
            "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)",
        )))?;
        check_value_bytes_fit(needed)?;
    }
    scratch.charge(needed)?;
    let mut out = String::with_capacity(needed);
    for &v in &values[..wrote] {
        if let Some(c) = char::from_u32(v & SCALAR_MASK) {
            out.push(c);
        }
    }
    debug_assert_eq!(out.len(), needed);
    drop(values);
    drop(scratch);
    Ok(heap.alloc_text(out)?)
}

pub(crate) fn is_letter(c: char) -> bool {
    (properties(c as u32) & UCD_LETTER) != 0
}

pub(crate) fn is_digit(c: char) -> bool {
    (properties(c as u32) & UCD_DIGIT) != 0
}

fn decode_utf8_scalar(bytes: &[u8]) -> Option<(u32, usize)> {
    let b0 = *bytes.first()?;
    if b0 < 0x80 {
        return Some((u32::from(b0), 1));
    }
    if (0xc2..=0xdf).contains(&b0) {
        let b1 = *bytes.get(1)?;
        if (b1 & 0xc0) != 0x80 {
            return None;
        }
        return Some((((u32::from(b0 & 0x1f)) << 6) | u32::from(b1 & 0x3f), 2));
    }
    if (0xe0..=0xef).contains(&b0) {
        let b1 = *bytes.get(1)?;
        let b2 = *bytes.get(2)?;
        if (b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80 {
            return None;
        }
        let cp =
            ((u32::from(b0 & 0x0f)) << 12) | ((u32::from(b1 & 0x3f)) << 6) | u32::from(b2 & 0x3f);
        if cp < 0x800 || (0xd800..=0xdfff).contains(&cp) {
            return None;
        }
        return Some((cp, 3));
    }
    if (0xf0..=0xf4).contains(&b0) {
        let b1 = *bytes.get(1)?;
        let b2 = *bytes.get(2)?;
        let b3 = *bytes.get(3)?;
        if (b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80 || (b3 & 0xc0) != 0x80 {
            return None;
        }
        let cp = ((u32::from(b0 & 0x07)) << 18)
            | ((u32::from(b1 & 0x3f)) << 12)
            | ((u32::from(b2 & 0x3f)) << 6)
            | u32::from(b3 & 0x3f);
        if !(0x1_0000..=0x10_ffff).contains(&cp) {
            return None;
        }
        return Some((cp, 4));
    }
    None
}

/// Public Unicode 16.0.0 grapheme-boundary and display-width helpers shared with
/// diagnostic renderers (`mo-base`, `mo-fe`).
pub mod unicode16 {
    use super::{Boundary, decode_utf8_scalar, display_width_str, properties};

    /// Byte length of the next grapheme cluster in `s` (0 if `s` is empty).
    #[must_use]
    pub fn next_grapheme(s: &str) -> usize {
        if s.is_empty() {
            return 0;
        }
        let mut state = Boundary::default();
        for (idx, ch) in s.char_indices() {
            if state.next(properties(ch as u32)) && idx != 0 {
                return idx;
            }
        }
        s.len()
    }

    /// Byte length of the next grapheme cluster in `bytes`, or `None` if `bytes`
    /// is empty or invalid UTF-8 is encountered before the cluster ends.
    /// Matches `src/prim/unicode.c:mo_unicode16_next_grapheme`.
    #[must_use]
    pub fn next_grapheme_bytes(bytes: &[u8]) -> Option<usize> {
        if bytes.is_empty() {
            return None;
        }
        let mut state = Boundary::default();
        let mut at = 0usize;
        while at < bytes.len() {
            let here = at;
            let (cp, width) = decode_utf8_scalar(&bytes[at..])?;
            at += width;
            if state.next(properties(cp)) && here != 0 {
                return Some(here);
            }
        }
        Some(bytes.len())
    }

    /// Terminal display column width of `s` under Unicode 16.0.0.
    #[must_use]
    pub fn display_width(s: &str) -> u64 {
        display_width_str(s)
    }

    /// Terminal display column width of `bytes`, or `None` if `bytes` is not valid UTF-8.
    /// Matches `src/prim/unicode.c:mo_unicode16_display_width`.
    #[must_use]
    pub fn display_width_bytes(bytes: &[u8]) -> Option<u64> {
        let s = std::str::from_utf8(bytes).ok()?;
        Some(display_width_str(s))
    }
}

pub fn call(heap: &Heap, prim: PrimId, args: &[Value]) -> Result<Value, PrimError> {
    match prim {
        PrimId::Unicode16Letter => Ok(Value::Bool(is_letter(args[0].as_char()))),
        PrimId::Unicode16Digit => Ok(Value::Bool(is_digit(args[0].as_char()))),
        PrimId::Unicode16Graphemes => graphemes(heap, args[0].as_text()),
        PrimId::Unicode16DisplayWidth => {
            let width = display_width_str(args[0].as_text());
            Ok(Value::Int(i64::try_from(width).unwrap_or(i64::MAX)))
        }
        PrimId::Unicode16NormalizeNfc => normalize_nfc(heap, args[0].as_text()),
        PrimId::Unicode16Lowercase => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let s = casing_str(args[0].as_text(), true)?;
            scratch.charge(s.len())?;
            drop(scratch);
            Ok(heap.alloc_text(s)?)
        }
        PrimId::Unicode16Uppercase => {
            let mut scratch = ScratchGuard::new(heap.budget());
            let s = casing_str(args[0].as_text(), false)?;
            scratch.charge(s.len())?;
            drop(scratch);
            Ok(heap.alloc_text(s)?)
        }
        _ => unreachable!("non-Unicode16 primitive dispatched to unicode::call"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unicode16_basics_and_edge_cases() {
        let heap = Heap::new(4 * 1024 * 1024);

        // Lowercase / Uppercase including German ß and Greek Final Sigma.
        assert_eq!(casing_str("Ça", true).unwrap(), "ça");
        assert_eq!(casing_str("Straße", false).unwrap(), "STRASSE");
        assert_eq!(casing_str("ΟΣ", true).unwrap(), "ος");
        assert_eq!(casing_str("ΟΣΟΣ", true).unwrap(), "οσος");
        assert_eq!(casing_str("ΟΣ'Α", true).unwrap(), "οσ'α");
        assert_eq!(casing_str("ΟΣ'", true).unwrap(), "ος'");
        assert_eq!(casing_str("Σ", true).unwrap(), "σ");
        assert_eq!(casing_str("AΣ\u{0345}", true).unwrap(), "aς\u{0345}");
        assert_eq!(casing_str("AΣ\u{0345}A", true).unwrap(), "aσ\u{0345}a");

        // Graphemes & display width (flags, keycaps, controls, combining marks).
        assert_eq!(graphemes_slices("e\u{0301}🇨🇦"), vec!["e\u{0301}", "🇨🇦"]);
        assert_eq!(display_width_str("e\u{0301}🇨🇦"), 3);
        assert_eq!(display_width_str("\x00\t\n\r\n\x1f\x7f\u{80}\u{9f}"), 0);
        assert_eq!(display_width_str("🇺"), 1);
        assert_eq!(display_width_str("🇺🇸"), 2);
        assert_eq!(display_width_str("🇺🇸🇨"), 3);
        assert_eq!(display_width_str("🇺🇸🇨🇦"), 4);
        assert_eq!(display_width_str("1\u{fe0f}\u{20e3}"), 2);
        assert_eq!(display_width_str("1\u{20e3}"), 2);
        assert_eq!(display_width_str("1\u{fe0e}\u{20e3}"), 1);
        assert_eq!(display_width_str("\u{20e3}"), 0);

        // NFC normalization including Hangul & adversarial long combining mark run (>32 marks).
        let nfc = normalize_nfc(&heap, "e\u{0301}").unwrap();
        assert_eq!(nfc.as_text(), "\u{00e9}");

        let hangul = normalize_nfc(&heap, "\u{1100}\u{1161}\u{11a8}").unwrap();
        assert_eq!(hangul.as_text(), "\u{ac01}");

        // 40 combining marks in descending CCC order exercises the counting-sort path.
        let mut marks = String::from("a");
        for _ in 0..20 {
            marks.push('\u{0301}'); // ccc 230
            marks.push('\u{0323}'); // ccc 220
        }
        let sorted = normalize_nfc(&heap, &marks).unwrap();
        // NFC should compose a + first \u{0323} -> \u{1ea1}, followed by remaining 19 \u{0323} then 20 \u{0301}.
        let mut expected = String::from("\u{1ea1}");
        for _ in 0..19 {
            expected.push('\u{0323}');
        }
        for _ in 0..20 {
            expected.push('\u{0301}');
        }
        assert_eq!(sorted.as_text(), expected);

        // letter? and digit?
        assert!(is_letter('é'));
        assert!(is_letter('ǅ'));
        assert!(is_letter('ʰ'));
        assert!(is_letter('界'));
        assert!(is_letter('\u{10400}'));
        assert!(!is_letter('١'));
        assert!(is_digit('١'));
        assert!(is_digit('\u{10d40}'));
        assert!(!is_digit('²'));
        assert!(!is_digit('Ⅳ'));
        assert!(!is_letter('\u{0345}'));
    }
}
