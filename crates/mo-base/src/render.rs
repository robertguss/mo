//! Diagnostic text and JSON renderers matching `src/tools/render.c` byte-for-byte.

use mo_def::ErrorCode;
use mo_def::limits::{MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_FIX_EDITS};
use thiserror::Error;

use crate::diag::{DiagText, Diagnostic, Edit, Fix, Label};
use crate::source::{SourceMap, Span};
use crate::utf8::decode_scalar;

/// Output format for [`render_diagnostic`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RenderFormat {
    Text,
    Json,
}

/// Terminal encoding mode for text rendering (JSON is always UTF-8).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TerminalEncoding {
    Utf8,
    Ascii,
}

/// Grapheme-cluster and display-width callbacks used by text diagnostic rendering.
///
/// Supplying `mo_prim::unicode16::{next_grapheme_bytes, display_width_bytes}` gives
/// exact Unicode 16.0.0 parity with `src/tools/render.c` without coupling `mo-base`
/// directly to `mo-prim`.
#[derive(Debug, Clone, Copy)]
pub struct DisplayMetrics {
    pub next_grapheme: fn(&[u8]) -> Option<usize>,
    pub display_width: fn(&[u8]) -> Option<u64>,
}

impl DisplayMetrics {
    /// Minimal scalar-level fallback when Unicode 16 tables are not supplied.
    pub const BASIC: Self = Self {
        next_grapheme: basic_next_grapheme,
        display_width: basic_display_width,
    };
}

fn basic_next_grapheme(bytes: &[u8]) -> Option<usize> {
    if bytes.is_empty() {
        return None;
    }
    let (ch, len) = decode_scalar(bytes);
    ch.map(|_| len)
}

fn basic_display_width(bytes: &[u8]) -> Option<u64> {
    let s = std::str::from_utf8(bytes).ok()?;
    let mut width = 0u64;
    for ch in s.chars() {
        let cp = ch as u32;
        if cp >= 0x20 && !(0x7F..=0x9F).contains(&cp) {
            width += 1;
        }
    }
    Some(width)
}

/// Returned when a rendered diagnostic exceeds its configured byte cap.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
#[error("rendered diagnostic exceeds byte limit ({limit})")]
pub struct RenderLimitError {
    pub limit: usize,
}

/// Bounded output writer shared by diagnostic, test-result, and defect renderers.
pub struct RenderWriter {
    out: Vec<u8>,
    limit: usize,
    ok: bool,
    utf8: bool,
    metrics: DisplayMetrics,
}

impl RenderWriter {
    #[must_use]
    pub fn new(limit: usize, encoding: TerminalEncoding, metrics: DisplayMetrics) -> Self {
        Self {
            out: Vec::new(),
            limit,
            ok: true,
            utf8: matches!(encoding, TerminalEncoding::Utf8),
            metrics,
        }
    }

    #[must_use]
    pub fn is_ok(&self) -> bool {
        self.ok
    }

    pub fn mark_failed(&mut self) {
        self.ok = false;
    }

    pub fn finish(self) -> Result<Vec<u8>, RenderLimitError> {
        if self.ok {
            Ok(self.out)
        } else {
            Err(RenderLimitError { limit: self.limit })
        }
    }

    pub fn bytes(&mut self, data: &[u8]) {
        if !self.ok {
            return;
        }
        if data.len() > self.limit.saturating_sub(self.out.len()) {
            self.ok = false;
            return;
        }
        self.out.extend_from_slice(data);
    }

    pub fn text(&mut self, s: &str) {
        self.bytes(s.as_bytes());
    }

    pub fn u64_dec(&mut self, n: u64) {
        if !self.ok {
            return;
        }
        let mut buf = [0u8; 20];
        let mut i = buf.len();
        let mut v = n;
        loop {
            i -= 1;
            buf[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.bytes(&buf[i..]);
    }

    pub fn json_bytes(&mut self, data: &[u8]) {
        json_bytes(self, data);
    }

    pub fn json_str(&mut self, s: &str) {
        json_bytes(self, s.as_bytes());
    }

    pub fn json_span(&mut self, sources: &SourceMap, span: Span) {
        json_span(self, sources, span, None, false);
    }

    pub fn safe_bytes(&mut self, data: &[u8]) {
        safe_bytes(self, data);
    }

    pub fn safe_str(&mut self, s: &str) {
        safe_bytes(self, s.as_bytes());
    }

    /// Writes terminal-safe text while preserving `\n` line breaks (`src/tools/render.c:mo_render_terminal_text`).
    pub fn terminal_bytes(&mut self, data: &[u8]) {
        let mut c = DisplayCursor {
            bytes: data,
            at: 0,
            safe_end: 0,
        };
        let mut cells = 0u32;
        while self.ok && c.at < c.bytes.len() {
            if c.bytes[c.at] == b'\n' {
                self.text("\n");
                c.at += 1;
                cells = 0;
            } else {
                let p = inspect(&mut c, cells, self.utf8, self.metrics);
                write_piece(self, &c.bytes[c.at..], &p);
                cells = cells.saturating_add(p.width);
                c.at += p.used;
            }
        }
    }

    pub fn write_location(&mut self, sources: &SourceMap, span: Span) {
        write_location(self, sources, span);
    }

    pub fn text_label(&mut self, sources: &SourceMap, label: &Label) {
        text_label(self, sources, label);
    }

    fn repeat(&mut self, ch: u8, count: u32) {
        if !self.ok {
            return;
        }
        let count = count as usize;
        if count > self.limit.saturating_sub(self.out.len()) {
            self.ok = false;
            return;
        }
        self.out.resize(self.out.len() + count, ch);
    }

    fn hex(&mut self, n: u32, digits: usize) {
        const HEX: &[u8; 16] = b"0123456789abcdef";
        if !self.ok {
            return;
        }
        let mut buf = [b'0'; 8];
        let digits = digits.min(8);
        let mut v = n;
        for idx in (0..digits).rev() {
            buf[idx] = HEX[(v & 0xF) as usize];
            v >>= 4;
        }
        self.bytes(&buf[..digits]);
    }
}

type BoundedWriter = RenderWriter;

/// Reference 2.1 refused characters plus line/terminal controls (`src/tools/render.c:escaped`).
const fn is_escaped_cp(c: u32) -> bool {
    c < 32
        || c == 127
        || (c >= 128 && c <= 159)
        || c == u32::MAX
        || c == 0xAD
        || c == 0x61C
        || c == 0x180E
        || (c >= 0x200B && c <= 0x200F)
        || (c >= 0x2028 && c <= 0x202E)
        || (c >= 0x2060 && c <= 0x2064)
        || (c >= 0x2066 && c <= 0x206F)
        || c == 0xFEFF
        || (c >= 0xFFF9 && c <= 0xFFFB)
        || c == 0xE_0001
        || (c >= 0xE_0020 && c <= 0xE_007F)
}

fn scalar_u32(bytes: &[u8]) -> (u32, usize) {
    let (ch, n) = decode_scalar(bytes);
    match ch {
        Some(c) => (c as u32, n),
        None => (u32::MAX, n),
    }
}

fn write_code(w: &mut BoundedWriter, code: ErrorCode) {
    let n = code.number();
    w.text("E");
    if n < 1000 {
        w.text("0");
    }
    if n < 100 {
        w.text("0");
    }
    if n < 10 {
        w.text("0");
    }
    w.u64_dec(u64::from(n));
}

fn json_bytes(w: &mut BoundedWriter, s: &[u8]) {
    w.text("\"");
    let mut i = 0usize;
    while w.ok && i < s.len() {
        let (mut value, n) = scalar_u32(&s[i..]);
        if s[i] == b'"' || s[i] == b'\\' {
            w.text("\\");
            w.bytes(&s[i..i + 1]);
        } else if is_escaped_cp(value) {
            if value == u32::MAX {
                value = u32::from(s[i]);
            }
            w.text("\\u");
            if value <= 0xFFFF {
                w.hex(value, 4);
            } else {
                value -= 0x1_0000;
                w.hex(0xD800 + (value >> 10), 4);
                w.text("\\u");
                w.hex(0xDC00 + (value & 0x3FF), 4);
            }
        } else {
            w.bytes(&s[i..i + n]);
        }
        i += n;
    }
    w.text("\"");
}

fn json_opt_bytes(w: &mut BoundedWriter, s: Option<&[u8]>) {
    match s {
        None => w.text("null"),
        Some(bytes) => json_bytes(w, bytes),
    }
}

fn json_span(
    w: &mut BoundedWriter,
    sources: &SourceMap,
    span: Span,
    label: Option<&DiagText>,
    labeled: bool,
) {
    w.text("{\"file\":");
    let found = sources.is_available(span);
    let path = if found {
        sources.get(span.file).map(|f| f.path_bytes())
    } else {
        None
    };
    json_opt_bytes(w, path);
    w.text(",\"file_bytes\":");
    if let Some(path_bytes) = path {
        w.text("\"");
        for &b in path_bytes {
            if !w.ok {
                break;
            }
            w.hex(u32::from(b), 2);
        }
        w.text("\"");
    } else {
        w.text("null");
    }
    w.text(",\"start\":");
    w.u64_dec(u64::from(span.start));
    w.text(",\"end\":");
    w.u64_dec(u64::from(span.end));

    let positions = if found {
        sources
            .position(span.file, span.start)
            .zip(sources.position(span.file, span.end))
    } else {
        None
    };
    let keys = [
        ",\"line\":",
        ",\"column\":",
        ",\"end_line\":",
        ",\"end_column\":",
    ];
    let vals = positions.map(|(s, e)| [s.line, s.column, e.line, e.column]);
    for (idx, key) in keys.iter().enumerate() {
        w.text(key);
        if let Some(arr) = vals {
            w.u64_dec(u64::from(arr[idx]));
        } else {
            w.text("null");
        }
    }
    if labeled {
        w.text(",\"label\":");
        json_opt_bytes(w, label.map(DiagText::as_bytes));
    }
    w.text("}");
}

fn json_diag(w: &mut BoundedWriter, sources: &SourceMap, d: &Diagnostic) {
    w.text("{\"record\":\"diagnostic\",\"code\":\"");
    write_code(w, d.code);
    w.text("\",\"headline\":");
    json_bytes(w, d.message.as_bytes());
    w.text(",\"reason\":");
    let reason = if d.section.is_some() {
        d.reason.as_ref().map(DiagText::as_bytes)
    } else {
        None
    };
    json_opt_bytes(w, reason);
    w.text(",\"section\":");
    json_opt_bytes(w, d.section.map(str::as_bytes));
    w.text(",\"primary\":");
    json_span(w, sources, d.primary.span, d.primary.text.as_ref(), true);
    w.text(",\"secondary\":[");
    for (i, sec) in d.secondary.iter().enumerate() {
        if !w.ok {
            break;
        }
        if i != 0 {
            w.text(",");
        }
        json_span(w, sources, sec.span, sec.text.as_ref(), true);
    }
    w.text("],\"fixes\":[");
    for (i, fix) in d.fixes.iter().enumerate() {
        if !w.ok {
            break;
        }
        if i != 0 {
            w.text(",");
        }
        w.text("{\"title\":");
        json_opt_bytes(w, fix.title.as_ref().map(DiagText::as_bytes));
        w.text(",\"edits\":[");
        for (j, edit) in fix.edits.iter().enumerate() {
            if !w.ok {
                break;
            }
            if j != 0 {
                w.text(",");
            }
            w.text("{\"span\":");
            json_span(w, sources, edit.span, None, false);
            w.text(",\"text\":");
            json_bytes(w, edit.replacement.as_bytes());
            w.text("}");
        }
        w.text("]}");
    }
    w.text("]}\n");
}

struct DisplayPiece {
    used: usize,
    width: u32,
    value: u32,
    plain: bool,
}

struct DisplayCursor<'a> {
    bytes: &'a [u8],
    at: usize,
    safe_end: usize,
}

fn plain_width(metrics: DisplayMetrics, s: &[u8]) -> u32 {
    let width = (metrics.display_width)(s).unwrap_or(0);
    u32::try_from(width).unwrap_or(u32::MAX)
}

fn inspect(
    c: &mut DisplayCursor<'_>,
    cells: u32,
    utf8: bool,
    metrics: DisplayMetrics,
) -> DisplayPiece {
    let s = &c.bytes[c.at..];
    let (value, used) = scalar_u32(s);
    if !is_escaped_cp(value) && (utf8 || value < 128) {
        if c.safe_end <= c.at {
            c.safe_end = c.at + used;
            while c.safe_end < c.bytes.len() {
                let (v, n) = scalar_u32(&c.bytes[c.safe_end..]);
                if is_escaped_cp(v) || (!utf8 && v >= 128) {
                    break;
                }
                c.safe_end += n;
            }
        }
        let cluster_len = (metrics.next_grapheme)(&c.bytes[c.at..c.safe_end]).unwrap_or(used);
        let width = plain_width(metrics, &s[..cluster_len]);
        DisplayPiece {
            used: cluster_len,
            width,
            value,
            plain: true,
        }
    } else {
        let width = if value == u32::from(b'\t') {
            4 - (cells % 4)
        } else if value < 0x80 || value == u32::MAX {
            4
        } else if value <= 0xFFF {
            7
        } else if value <= 0xFFFF {
            8
        } else if value <= 0xF_FFFF {
            9
        } else {
            10
        };
        DisplayPiece {
            used,
            width,
            value,
            plain: false,
        }
    }
}

fn write_piece(w: &mut BoundedWriter, s: &[u8], p: &DisplayPiece) {
    if p.plain {
        w.bytes(&s[..p.used]);
    } else if p.value == u32::from(b'\t') {
        w.repeat(b' ', p.width);
    } else if p.value < 0x80 || p.value == u32::MAX {
        w.text("\\x");
        w.hex(u32::from(s[0]), 2);
    } else {
        w.text("\\u{");
        w.hex(p.value, (p.width - 4) as usize);
        w.text("}");
    }
}

fn safe_bytes(w: &mut BoundedWriter, s: &[u8]) {
    let mut c = DisplayCursor {
        bytes: s,
        at: 0,
        safe_end: 0,
    };
    let mut cells = 0u32;
    while w.ok && c.at < c.bytes.len() {
        let p = inspect(&mut c, cells, w.utf8, w.metrics);
        write_piece(w, &c.bytes[c.at..], &p);
        cells = cells.saturating_add(p.width);
        c.at += p.used;
    }
}

fn write_location(w: &mut BoundedWriter, sources: &SourceMap, span: Span) {
    if !sources.is_available(span) {
        w.text("<unavailable>");
        return;
    }
    let Some(file) = sources.get(span.file) else {
        w.text("<unavailable>");
        return;
    };
    let Some(pos) = sources.position(span.file, span.start) else {
        w.text("<unavailable>");
        return;
    };
    safe_bytes(w, file.path_bytes());
    w.text(":");
    w.u64_dec(u64::from(pos.line));
    w.text(":");
    w.u64_dec(u64::from(pos.column));
}

fn line_prefix(w: &mut BoundedWriter, mut line: u32, number: bool) {
    w.text("  ");
    if number {
        w.u64_dec(u64::from(line));
    } else {
        loop {
            w.text(" ");
            line /= 10;
            if line == 0 {
                break;
            }
        }
    }
    w.text(" | ");
}

fn source_line(
    w: &mut BoundedWriter,
    s: &[u8],
    line: u32,
    start: usize,
    end: usize,
    mark: bool,
    label: Option<&DiagText>,
) {
    let mut c = DisplayCursor {
        bytes: s,
        at: 0,
        safe_end: 0,
    };
    let mut context = [0usize; 12];
    let mut count = 0usize;
    let mut focus_cells = 0u32;
    while c.at < start && c.at < s.len() {
        let p = inspect(&mut c, focus_cells, w.utf8, w.metrics);
        if p.used > start - c.at {
            break;
        }
        if count == 12 {
            context.copy_within(1..12, 0);
            count -= 1;
        }
        context[count] = c.at;
        count += 1;
        focus_cells = focus_cells.saturating_add(p.width);
        c.at += p.used;
    }

    let mut at = 0usize;
    if focus_cells > 100 || c.at > 1024 {
        at = c.at;
        for &ctx in &context[..count] {
            if c.at - ctx <= 1024 {
                at = ctx;
                break;
            }
        }
    }

    c.at = at;
    c.safe_end = at;
    line_prefix(w, line, true);
    let mut cells = 0u32;
    let mut first = u32::MAX;
    let mut last = 0u32;
    let mut row_bytes = 0usize;
    if at != 0 {
        w.text("...");
        cells = 3;
        row_bytes = 3;
    }

    while w.ok && c.at < c.bytes.len() && cells < 148 {
        let p = inspect(&mut c, cells, w.utf8, w.metrics);
        let emitted = if p.plain { p.used } else { p.width as usize };
        if p.width > 148 - cells || emitted > 4093 - row_bytes {
            break;
        }
        write_piece(w, &s[c.at..], &p);
        if (c.at < end && c.at + p.used > start)
            || (start == end && c.at <= start && c.at + p.used > start)
        {
            if first == u32::MAX {
                first = cells;
            }
            last = cells.saturating_add(p.width);
        }
        cells = cells.saturating_add(p.width);
        row_bytes += emitted;
        c.at += p.used;
    }
    if c.at < c.bytes.len() {
        w.text("...");
    }
    if first == u32::MAX {
        first = cells;
        last = cells + 1;
    }
    if last <= first {
        last = first + 1;
    }
    w.text("\n");
    if mark {
        line_prefix(w, line, false);
        w.repeat(b' ', first);
        w.repeat(b'^', last - first);
        if let Some(text) = label {
            w.text(" ");
            safe_bytes(w, text.as_bytes());
        }
        w.text("\n");
    }
}

fn text_label(w: &mut BoundedWriter, sources: &SourceMap, label: &Label) {
    let span = label.span;
    if !sources.is_available(span) {
        if let Some(text) = &label.text {
            w.text("  ");
            safe_bytes(w, text.as_bytes());
            w.text("\n");
        }
        return;
    }
    let Some(file) = sources.get(span.file) else {
        return;
    };
    let Some(first) = sources.position(span.file, span.start) else {
        return;
    };
    let last_offset = if span.end > span.start {
        span.end - 1
    } else {
        span.end
    };
    let Some(last) = sources.position(span.file, last_offset) else {
        return;
    };

    let mut line = first.line;
    while w.ok && line <= last.line {
        if line == first.line + 2 && last.line - first.line >= 4 {
            line_prefix(w, line, false);
            w.text("...\n");
            line = last.line - 1;
        }
        let Some(row) = sources.line_span(span.file, line) else {
            break;
        };
        let row_len = (row.end - row.start) as usize;
        let start = if span.start > row.start {
            (span.start - row.start) as usize
        } else {
            0
        }
        .min(row_len);
        let end = if span.end < row.end {
            (span.end - row.start) as usize
        } else {
            row_len
        };
        let row_bytes = &file.bytes()[row.start as usize..row.end as usize];
        source_line(
            w,
            row_bytes,
            line,
            start,
            end,
            true,
            if line == last.line {
                label.text.as_ref()
            } else {
                None
            },
        );
        line += 1;
    }
}

fn fixed_lines(w: &mut BoundedWriter, s: &[u8], mut line: u32, focus: usize) {
    let mut at = 0usize;
    while w.ok && at <= s.len() {
        let mut end = at;
        while end < s.len() && s[end] != b'\n' {
            end += 1;
        }
        let mut trimmed = end;
        if end < s.len() && trimmed > at && s[trimmed - 1] == b'\r' {
            trimmed -= 1;
        }
        source_line(
            w,
            &s[at..trimmed],
            line,
            if at == 0 { focus } else { 0 },
            0,
            false,
            None,
        );
        line += 1;
        if end == s.len() {
            break;
        }
        at = end + 1;
    }
}

fn text_fix(w: &mut BoundedWriter, sources: &SourceMap, fix: &Fix) {
    debug_assert!((fix.edits.len() as u64) <= MO_LIMIT_FIX_EDITS);
    let mut edits: Vec<&Edit> = fix.edits.iter().collect();
    edits.sort_by_key(|e| (e.span.file, e.span.start, e.span.end));

    w.text("\nFix:");
    if let Some(title) = &fix.title {
        w.text(" ");
        safe_bytes(w, title.as_bytes());
    }
    w.text("\n");

    let mut i = 0usize;
    while w.ok && i < edits.len() {
        let file_id = edits[i].span.file;
        let mut stop = i + 1;
        while stop < edits.len() && edits[stop].span.file == file_id {
            stop += 1;
        }
        if !sources.is_available(edits[i].span) || !sources.is_available(edits[stop - 1].span) {
            w.text("  <unavailable>\n");
            i = stop;
            continue;
        }
        w.text("  ");
        write_location(w, sources, edits[i].span);
        w.text("\n");

        let Some(p) = sources.position(file_id, edits[i].span.start) else {
            w.ok = false;
            return;
        };
        let Some(q) = sources.position(file_id, edits[stop - 1].span.end) else {
            w.ok = false;
            return;
        };
        let Some(p_line) = sources.line_span(file_id, p.line) else {
            w.ok = false;
            return;
        };
        let Some(q_line) = sources.line_span(file_id, q.line) else {
            w.ok = false;
            return;
        };
        let Some(file) = sources.get(file_id) else {
            w.ok = false;
            return;
        };

        let mut at = p_line.start as usize;
        let focus = (edits[i].span.start - p_line.start) as usize;
        let end = q_line.end.max(edits[stop - 1].span.end) as usize;
        let original = file.bytes();
        let fixed_cap = MO_CLI_DIAGNOSTIC_SIZE as usize;
        let mut fixed = Vec::new();
        let mut ok = true;

        for edit in &edits[i..stop] {
            let span = edit.span;
            if !sources.is_available(span) || (span.start as usize) < at {
                ok = false;
                break;
            }
            let prefix_len = (span.start as usize) - at;
            let repl = edit.replacement.as_bytes();
            let add_len = prefix_len.saturating_add(repl.len());
            if add_len > fixed_cap.saturating_sub(fixed.len()) {
                ok = false;
                break;
            }
            fixed.extend_from_slice(&original[at..span.start as usize]);
            fixed.extend_from_slice(repl);
            at = span.end as usize;
        }
        if ok {
            let tail_len = end.saturating_sub(at);
            if tail_len > fixed_cap.saturating_sub(fixed.len()) {
                ok = false;
            } else {
                fixed.extend_from_slice(&original[at..end]);
            }
        }
        if ok {
            fixed_lines(w, &fixed, p.line, focus);
        } else {
            w.ok = false;
        }
        i = stop;
    }
}

/// Renders one diagnostic in text or JSON format, enforcing `limit` bytes.
///
/// Exact port of `src/tools/render.c:mo_render_diagnostic`.
pub fn render_diagnostic(
    sources: &SourceMap,
    diag: &Diagnostic,
    format: RenderFormat,
    encoding: TerminalEncoding,
    metrics: DisplayMetrics,
    limit: usize,
) -> Result<Vec<u8>, RenderLimitError> {
    let mut w = RenderWriter::new(limit, encoding, metrics);
    match format {
        RenderFormat::Json => json_diag(&mut w, sources, diag),
        RenderFormat::Text => {
            w.text("error ");
            write_code(&mut w, diag.code);
            w.text(": ");
            safe_bytes(&mut w, diag.message.as_bytes());
            w.text("  ");
            write_location(&mut w, sources, diag.primary.span);
            w.text("\n\n");
            text_label(&mut w, sources, &diag.primary);
            for sec in &diag.secondary {
                if !w.ok {
                    break;
                }
                w.text("\n  --> ");
                write_location(&mut w, sources, sec.span);
                w.text("\n");
                text_label(&mut w, sources, sec);
            }
            if diag.section.is_some() {
                w.text("\n");
                if let Some(reason) = &diag.reason {
                    safe_bytes(&mut w, reason.as_bytes());
                }
                w.text("\n");
            }
            for fix in &diag.fixes {
                if !w.ok {
                    break;
                }
                text_fix(&mut w, sources, fix);
            }
            w.text("\n");
        }
    }
    if w.ok {
        Ok(w.out)
    } else {
        Err(RenderLimitError { limit })
    }
}

/// Convenience wrapper using `MO_CLI_DIAGNOSTIC_SIZE` as the byte limit.
pub fn render_diagnostic_default(
    sources: &SourceMap,
    diag: &Diagnostic,
    format: RenderFormat,
    encoding: TerminalEncoding,
    metrics: DisplayMetrics,
) -> Result<Vec<u8>, RenderLimitError> {
    render_diagnostic(
        sources,
        diag,
        format,
        encoding,
        metrics,
        MO_CLI_DIAGNOSTIC_SIZE as usize,
    )
}

/// Renders the leading NDJSON version record matching `src/tools/render.c:mo_write_version`.
#[must_use]
pub fn render_version_json(command: &str, version: &str) -> Vec<u8> {
    let mut w = RenderWriter::new(
        MO_CLI_DIAGNOSTIC_SIZE as usize,
        TerminalEncoding::Utf8,
        DisplayMetrics::BASIC,
    );
    w.text("{\"record\":\"version\",\"format\":\"1.0\",\"mo\":");
    json_bytes(&mut w, version.as_bytes());
    w.text(",\"command\":");
    json_bytes(&mut w, command.as_bytes());
    w.text("}\n");
    w.out
}

/// Renders the trailing NDJSON totals record matching `src/tools/render.c:mo_write_diagnostics`.
#[must_use]
pub fn render_totals_json(errors: u64, dropped: u64, status: i32) -> Vec<u8> {
    let mut w = RenderWriter::new(
        MO_CLI_DIAGNOSTIC_SIZE as usize,
        TerminalEncoding::Utf8,
        DisplayMetrics::BASIC,
    );
    w.text("{\"record\":\"totals\",\"errors\":");
    w.u64_dec(errors);
    w.text(",\"dropped\":");
    w.u64_dec(dropped);
    w.text(",\"status\":");
    w.u64_dec(u64::from(u32::try_from(status).unwrap_or(1)));
    w.text("}\n");
    w.out
}
