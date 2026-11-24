//! Literal map-key canonicalization (`src/check/map_keys.c` parity).
//!
//! Normalizes numeric literals (decimal, hex, binary, with underscores, arbitrary `BigInt`
//! magnitudes, unary `-`, and fractional zeroes), text/triple-text literals (escapes and
//! closing-quote indentation stripping), char literals, and bool literals into canonical
//! byte strings for duplicate literal map-key detection (`E0098`).

use mo_syntax::{NodeId, SyntaxKind, SyntaxTree, TokenKind};

/// Category of a normalized literal map key (`enum mo_map_key_category`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum MapKeyCategory {
    None = 0,
    Number = 1,
    Text = 2,
    Char = 3,
    Bool = 4,
}

/// Canonicalized literal map key (`struct mo_map_literal_key`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MapLiteralKey {
    pub bytes: Vec<u8>,
    pub category: MapKeyCategory,
    pub negative: bool,
}

impl MapLiteralKey {
    #[must_use]
    pub const fn none() -> Self {
        Self {
            bytes: Vec::new(),
            category: MapKeyCategory::None,
            negative: false,
        }
    }

    #[must_use]
    pub const fn is_present(&self) -> bool {
        !matches!(self.category, MapKeyCategory::None)
    }

    /// Canonical comparison order matching `infer-terms.c:compare_map_keys`.
    #[must_use]
    pub fn cmp_canonical(&self, other: &Self) -> std::cmp::Ordering {
        self.category
            .cmp(&other.category)
            .then_with(|| self.negative.cmp(&other.negative))
            .then_with(|| self.bytes.len().cmp(&other.bytes.len()))
            .then_with(|| self.bytes.cmp(&other.bytes))
    }
}

/// Budget callbacks used by [`canonicalize_map_literal_key`].
pub trait MapKeyBudget {
    /// Charges `amount` work units; returns `false` if work budget is exhausted.
    fn charge_work(&mut self, amount: usize) -> bool;
    /// Charges `bytes` arena allocation; returns `false` if arena budget is exhausted.
    fn charge_bytes(&mut self, bytes: usize) -> bool;
}

const fn hex_digit(c: u8) -> u32 {
    if c >= b'a' {
        (c - b'a') as u32 + 10
    } else if c >= b'A' {
        (c - b'A') as u32 + 10
    } else {
        (c - b'0') as u32
    }
}

fn charge_alloc(budget: &mut impl MapKeyBudget, bytes: usize) -> bool {
    budget.charge_work(bytes) && budget.charge_bytes(bytes)
}

fn canonicalize_number(
    budget: &mut impl MapKeyBudget,
    text: &[u8],
    is_decimal: bool,
    mut negative: bool,
) -> Option<MapLiteralKey> {
    let length = text.len();
    let base: u32 = if length > 2 && text[1] == b'x' {
        16
    } else if length > 2 && text[1] == b'b' {
        2
    } else {
        10
    };
    let digit_cap = length.checked_mul(2)?.checked_add(1)?;
    if !charge_alloc(budget, digit_cap) || !budget.charge_work(length) {
        return None;
    }
    let mut digits = vec![0u8; digit_cap];
    let mut used = 0usize;
    let mut places = 0usize;

    if base == 10 {
        let mut fraction = is_decimal;
        for &c in text.iter().rev() {
            if c == b'_' {
                continue;
            }
            if c == b'.' {
                fraction = false;
                continue;
            }
            digits[used] = hex_digit(c) as u8;
            used += 1;
            if fraction {
                places += 1;
            }
        }
    } else {
        used = 1;
        digits[0] = 0;
        for &c in &text[2..] {
            if c == b'_' {
                continue;
            }
            let mut carry = hex_digit(c);
            debug_assert!(carry < base);
            if !budget.charge_work(used + 2) {
                return None;
            }
            for d in &mut digits[..used] {
                let value = u32::from(*d) * base + carry;
                *d = (value % 10) as u8;
                carry = value / 10;
            }
            while carry != 0 {
                debug_assert!(used < digit_cap);
                digits[used] = (carry % 10) as u8;
                used += 1;
                carry /= 10;
            }
        }
    }
    debug_assert!(used != 0);

    if !budget.charge_work(2 * used) {
        return None;
    }
    while used > 1 && digits[used - 1] == 0 {
        used -= 1;
    }
    let mut first = 0usize;
    if used == 1 && digits[0] == 0 {
        negative = false;
        places = 0;
    } else {
        while places != 0 && digits[first] == 0 {
            first += 1;
            used -= 1;
            places -= 1;
        }
    }

    let size = used.max(places) + 2;
    if !charge_alloc(budget, size) || !budget.charge_work(size) {
        return None;
    }
    let mut bytes = Vec::with_capacity(size);
    if used <= places {
        bytes.push(b'0');
        bytes.push(b'.');
        bytes.extend(std::iter::repeat_n(b'0', places - used));
    }
    for i in (1..=used).rev() {
        bytes.push(b'0' + digits[first + i - 1]);
        if places != 0 && i - 1 == places {
            bytes.push(b'.');
        }
    }
    debug_assert!(bytes.len() <= size);
    Some(MapLiteralKey {
        bytes,
        category: MapKeyCategory::Number,
        negative,
    })
}

fn encode_utf8(c: u32, out: &mut Vec<u8>) {
    if c < 0x80 {
        out.push(c as u8);
    } else if c < 0x800 {
        out.push((0xc0 | (c >> 6)) as u8);
        out.push((0x80 | (c & 63)) as u8);
    } else if c < 0x10000 {
        out.push((0xe0 | (c >> 12)) as u8);
        out.push((0x80 | ((c >> 6) & 63)) as u8);
        out.push((0x80 | (c & 63)) as u8);
    } else {
        out.push((0xf0 | (c >> 18)) as u8);
        out.push((0x80 | ((c >> 12) & 63)) as u8);
        out.push((0x80 | ((c >> 6) & 63)) as u8);
        out.push((0x80 | (c & 63)) as u8);
    }
}

fn canonicalize_decoded(
    budget: &mut impl MapKeyBudget,
    text: &[u8],
    is_char: bool,
) -> Option<MapLiteralKey> {
    let length = text.len();
    let quote = if length >= 6 && text[0] == b'"' && text[1] == b'"' && text[2] == b'"' {
        3
    } else {
        1
    };
    debug_assert!(length >= 2 * quote);
    if !budget.charge_work(length.saturating_mul(3)) {
        return None;
    }
    let mut begin = quote;
    let mut end = length - quote;
    let mut indent = 0usize;
    if quote == 3 {
        while begin < end && text[begin] != b'\n' {
            begin += 1;
        }
        debug_assert!(begin < end);
        begin += 1;
        let mut closing = end;
        while closing > begin && text[closing - 1] == b' ' {
            closing -= 1;
        }
        indent = end - closing;
        end = if closing > begin { closing - 1 } else { begin };
        if end > begin && text[end - 1] == b'\r' {
            end -= 1;
        }
    }
    if !charge_alloc(budget, (end - begin) + 1) {
        return None;
    }
    let mut bytes = Vec::with_capacity(end - begin);
    let mut line_start = quote == 3;
    let mut i = begin;
    while i < end {
        if line_start {
            let mut spaces = 0usize;
            while i < end && spaces < indent && text[i] == b' ' {
                i += 1;
                spaces += 1;
            }
            line_start = false;
            if i == end {
                break;
            }
        }
        let mut c = text[i];
        if quote == 3 && c == b'\r' {
            debug_assert!(i + 1 < end && text[i + 1] == b'\n');
            i += 1;
            c = b'\n';
        }
        if quote == 3 && c == b'\n' {
            line_start = true;
        }
        if c != b'\\' {
            bytes.push(c);
            i += 1;
            continue;
        }
        i += 1;
        debug_assert!(i < end);
        c = text[i];
        if c == b'u' {
            i += 1;
            debug_assert!(i < end && text[i] == b'{');
            let mut scalar = 0u32;
            i += 1;
            while i < end && text[i] != b'}' {
                scalar = scalar * 16 + hex_digit(text[i]);
                i += 1;
            }
            encode_utf8(scalar, &mut bytes);
        } else {
            let decoded = match c {
                b'n' => b'\n',
                b't' => b'\t',
                b'r' => b'\r',
                other => other,
            };
            bytes.push(decoded);
        }
        i += 1;
    }
    Some(MapLiteralKey {
        bytes,
        category: if is_char {
            MapKeyCategory::Char
        } else {
            MapKeyCategory::Text
        },
        negative: false,
    })
}

/// Canonicalizes `node` if it is a compile-time literal map key (`mo_map_literal_key`).
///
/// Returns `None` if `budget` is exhausted; returns `Some(MapLiteralKey::none())` when `node`
/// is not a literal key.
#[must_use]
pub fn canonicalize_map_literal_key(
    budget: &mut impl MapKeyBudget,
    tree: &SyntaxTree,
    mut node_id: NodeId,
) -> Option<MapLiteralKey> {
    let mut negative = false;
    let mut numeric = false;
    loop {
        let node = tree.node(node_id);
        match node.kind {
            SyntaxKind::Group => {
                if !budget.charge_work(1) {
                    return None;
                }
                debug_assert_eq!(node.children.len(), 1);
                node_id = node.children[0]?;
            }
            SyntaxKind::Negate => {
                if !budget.charge_work(1) {
                    return None;
                }
                debug_assert_eq!(node.children.len(), 1);
                negative = !negative;
                numeric = true;
                node_id = node.children[0]?;
            }
            SyntaxKind::Prefix => {
                if !budget.charge_work(1) {
                    return None;
                }
                debug_assert!(node.children.len() >= 3);
                if !budget.charge_work(node.children.len() - 1) {
                    return None;
                }
                for child in &node.children[..node.children.len() - 1] {
                    let cid = (*child)?;
                    if tree.node(cid).token != TokenKind::Minus {
                        return Some(MapLiteralKey::none());
                    }
                    negative = !negative;
                }
                numeric = true;
                node_id = (*node.children.last()?)?;
            }
            _ => break,
        }
    }

    let node = tree.node(node_id);
    match node.kind {
        SyntaxKind::Int | SyntaxKind::Decimal => {
            let text = tree.node_text(node_id)?;
            canonicalize_number(budget, text, node.kind == SyntaxKind::Decimal, negative)
        }
        _ if numeric => Some(MapLiteralKey::none()),
        SyntaxKind::Text | SyntaxKind::Char => {
            let text = tree.node_text(node_id)?;
            canonicalize_decoded(budget, text, node.kind == SyntaxKind::Char)
        }
        SyntaxKind::True => Some(MapLiteralKey {
            bytes: vec![b'1'],
            category: MapKeyCategory::Bool,
            negative: false,
        }),
        SyntaxKind::False => Some(MapLiteralKey {
            bytes: vec![b'0'],
            category: MapKeyCategory::Bool,
            negative: false,
        }),
        _ => Some(MapLiteralKey::none()),
    }
}
