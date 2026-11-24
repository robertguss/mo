//! Strict UTF-8 scalar decoding matching `src/base/utf8.h`, `src/base/diag.c`,
//! and `src/tools/render.c`.

/// Decodes one UTF-8 scalar at the start of `bytes`.
///
/// A valid scalar returns `(Some(ch), 1..=4)`.
/// An invalid leading or continuation byte sequence consumes exactly 1 byte
/// and returns `(None, 1)`.
/// An empty slice returns `(None, 0)`.
#[must_use]
pub fn decode_scalar(bytes: &[u8]) -> (Option<char>, usize) {
    let Some(&first) = bytes.first() else {
        return (None, 0);
    };
    if first < 0x80 {
        return (Some(char::from(first)), 1);
    }
    let (width, mut value, min_value) = if (0xC2..=0xDF).contains(&first) {
        (2usize, u32::from(first & 0x1F), 0x80u32)
    } else if (0xE0..=0xEF).contains(&first) {
        (3usize, u32::from(first & 0x0F), 0x800u32)
    } else if (0xF0..=0xF4).contains(&first) {
        (4usize, u32::from(first & 0x07), 0x1_0000u32)
    } else {
        return (None, 1);
    };
    if bytes.len() < width {
        return (None, 1);
    }
    for &cont in &bytes[1..width] {
        if (cont & 0xC0) != 0x80 {
            return (None, 1);
        }
        value = (value << 6) | u32::from(cont & 0x3F);
    }
    if value < min_value || value > 0x10_FFFF || (0xD800..=0xDFFF).contains(&value) {
        return (None, 1);
    }
    match char::from_u32(value) {
        Some(ch) => (Some(ch), width),
        None => (None, 1),
    }
}

/// Returns whether `bytes` is strictly valid UTF-8.
#[must_use]
pub fn is_valid_utf8(bytes: &[u8]) -> bool {
    std::str::from_utf8(bytes).is_ok()
}
