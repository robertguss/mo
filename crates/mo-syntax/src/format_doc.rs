//! Arena-owned Wadler document IR with cached flat widths and consecutive inline-comment
//! alignment (`src/syntax/format-doc.h` & `src/syntax/format-doc.c` parity).

use mo_def::limits::{
    MO_FORMAT_DOCUMENT_DEPTH, MO_FORMAT_DOCUMENT_SIZE, MO_LIMIT_FILE_SIZE, MO_LIMIT_LINE_LENGTH,
};

use crate::format::{DisplayWidthFn, FormatOptions};

/// C `sizeof(struct mo_doc)` on 64-bit platforms so `MO_FORMAT_DOCUMENT_SIZE` (128 MiB)
/// matches `tests/errors/e1403` (`generate_format_limit`) identically.
const C_MO_DOC_SIZE: usize = 88;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DocKind {
    Text,
    Comment,
    Break,
    Space,
    Line,
    LineIf,
    SuffixLine,
    Blank,
    Group,
    EndGroup,
    Nest,
    Unnest,
    AlignLine,
    EndAlign,
}

struct DocNode<'a> {
    kind: DocKind,
    bytes: &'a [u8],
    break_space: usize,
    suffix_offset: usize,
    flat_width: u32,
    saved_indent: u32,
    suffix_line: u32,
    suffix_column: u32,
    suffix_characters: u32,
    padding: u32,
    flat: bool,
    ends_line: bool,
    parent: Option<usize>,
}

const fn add_width(a: u32, b: u32) -> u32 {
    a.saturating_add(b)
}

pub(crate) struct Document<'a> {
    nodes: Vec<DocNode<'a>>,
    group: Option<usize>,
    align: Option<usize>,
    display_width: DisplayWidthFn,
    depth: u32,
    align_depth: u32,
    pub ok: bool,
}

impl<'a> Document<'a> {
    pub fn new(display_width: DisplayWidthFn) -> Self {
        Self {
            nodes: Vec::new(),
            group: None,
            align: None,
            display_width,
            depth: 0,
            align_depth: 0,
            ok: true,
        }
    }

    fn measure(&mut self, bytes: &[u8]) -> u32 {
        match (self.display_width)(bytes) {
            Some(w) => u32::try_from(w).unwrap_or_else(|_| {
                self.ok = false;
                u32::MAX
            }),
            None => {
                self.ok = false;
                u32::MAX
            }
        }
    }

    pub fn add(&mut self, kind: DocKind, bytes: &'a [u8], break_space: usize) {
        if !self.ok {
            return;
        }
        let next_count = self.nodes.len() + 1;
        if next_count.saturating_mul(C_MO_DOC_SIZE) > (MO_FORMAT_DOCUMENT_SIZE as usize) {
            self.ok = false;
            return;
        }
        let idx = self.nodes.len();
        let mut node = DocNode {
            kind,
            bytes,
            break_space,
            suffix_offset: 0,
            flat_width: 0,
            saved_indent: 0,
            suffix_line: 0,
            suffix_column: 0,
            suffix_characters: 0,
            padding: 0,
            flat: false,
            ends_line: false,
            parent: None,
        };

        match kind {
            DocKind::Text => {
                node.flat_width = if bytes.contains(&b'\n') {
                    u32::MAX
                } else {
                    self.measure(bytes)
                };
            }
            DocKind::Break | DocKind::Space => {
                node.flat_width = break_space as u32;
            }
            DocKind::Comment => {
                let w = self.measure(bytes);
                node.flat_width = add_width(2, w);
            }
            DocKind::SuffixLine => {
                node.ends_line = true;
            }
            DocKind::Line | DocKind::LineIf | DocKind::Blank => {
                node.flat_width = u32::MAX;
            }
            DocKind::Group => {
                self.depth += 1;
                if u64::from(self.depth) > MO_FORMAT_DOCUMENT_DEPTH {
                    self.ok = false;
                }
                node.parent = self.group;
                self.group = Some(idx);
                self.nodes.push(node);
                return;
            }
            DocKind::EndGroup => {
                let Some(g_idx) = self.group else {
                    self.ok = false;
                    return;
                };
                if self.depth == 0 {
                    self.ok = false;
                    return;
                }
                node.parent = Some(g_idx);
                node.flat_width = self.nodes[g_idx].flat_width;
                node.ends_line = self.nodes[g_idx].ends_line;
                self.group = self.nodes[g_idx].parent;
                self.depth -= 1;
            }
            DocKind::Nest | DocKind::Unnest => {}
            DocKind::AlignLine => {
                self.align_depth += 1;
                if u64::from(self.align_depth) > MO_FORMAT_DOCUMENT_DEPTH {
                    self.ok = false;
                }
                node.parent = self.align;
                self.align = Some(idx);
            }
            DocKind::EndAlign => {
                let Some(a_idx) = self.align else {
                    self.ok = false;
                    return;
                };
                if self.align_depth == 0 {
                    self.ok = false;
                    return;
                }
                node.parent = Some(a_idx);
                self.align = self.nodes[a_idx].parent;
                self.align_depth -= 1;
            }
        }

        let d_flat = node.flat_width;
        let d_ends = node.ends_line;
        self.nodes.push(node);

        if let Some(enclosing_idx) = self.group {
            // A line-ending comment at the end of a group doesn't break its
            // preceding code. More code after it makes a flat group impossible.
            let enc = &mut self.nodes[enclosing_idx];
            if enc.ends_line && d_flat != 0 {
                enc.flat_width = u32::MAX;
            }
            enc.flat_width = add_width(enc.flat_width, d_flat);
            enc.ends_line = enc.ends_line || d_ends;
        }
    }

    pub fn render(mut self, options: FormatOptions) -> Option<Vec<u8>> {
        if !self.ok
            || self.group.is_some()
            || self.depth != 0
            || self.align.is_some()
            || self.align_depth != 0
        {
            return None;
        }
        let max_output = MO_LIMIT_FILE_SIZE as usize;
        let mut w = Writer {
            out: Vec::new(),
            max_output,
            group: None,
            pending: Vec::new(),
            comments: Vec::new(),
            display_width: self.display_width,
            indent: 0,
            column: 0,
            characters: 0,
            line_indent: 0,
            line: 1,
        };

        for idx in 0..self.nodes.len() {
            let kind = self.nodes[idx].kind;
            let ok = match kind {
                DocKind::Text => {
                    let data = self.nodes[idx].bytes;
                    w.write_bytes(data)
                }
                DocKind::Comment => {
                    w.pending.push(idx);
                    true
                }
                DocKind::Break => {
                    let group_broken = w.group.is_some_and(|g| !self.nodes[g].flat);
                    if !w.pending.is_empty() || group_broken {
                        w.newline(&mut self.nodes, false)
                    } else if self.nodes[idx].break_space != 0 && w.column != 0 {
                        w.write_bytes(b" ")
                    } else {
                        true
                    }
                }
                DocKind::Space => {
                    if w.column != 0 {
                        w.write_bytes(b" ")
                    } else {
                        true
                    }
                }
                DocKind::Line => w.newline(&mut self.nodes, true),
                DocKind::LineIf | DocKind::SuffixLine => w.newline(&mut self.nodes, false),
                DocKind::Blank => {
                    w.newline(&mut self.nodes, false) && w.newline(&mut self.nodes, true)
                }
                DocKind::Group => {
                    let column = if w.column != 0 { w.column } else { w.indent };
                    let parent_flat = w.group.is_some_and(|g| self.nodes[g].flat);
                    let flat_width = self.nodes[idx].flat_width;
                    self.nodes[idx].flat = parent_flat
                        || (column <= options.width && flat_width <= options.width - column);
                    w.group = Some(idx);
                    true
                }
                DocKind::EndGroup => {
                    let g_idx = self.nodes[idx].parent?;
                    w.group = self.nodes[g_idx].parent;
                    true
                }
                DocKind::Nest => {
                    w.indent = w.indent.saturating_add(2);
                    true
                }
                DocKind::Unnest => {
                    if w.indent < 2 {
                        return None;
                    }
                    w.indent -= 2;
                    true
                }
                DocKind::AlignLine => {
                    self.nodes[idx].saved_indent = w.indent;
                    if w.column != 0 {
                        w.indent = w.line_indent;
                    }
                    true
                }
                DocKind::EndAlign => {
                    let a_idx = self.nodes[idx].parent?;
                    w.indent = self.nodes[a_idx].saved_indent;
                    true
                }
            };
            if !ok {
                return None;
            }
        }
        if !w.newline(&mut self.nodes, false) || !w.align_comments(&mut self.nodes) {
            return None;
        }
        Some(w.out)
    }
}

struct Writer {
    out: Vec<u8>,
    max_output: usize,
    group: Option<usize>,
    pending: Vec<usize>,
    comments: Vec<usize>,
    display_width: DisplayWidthFn,
    indent: u32,
    column: u32,
    characters: u32,
    line_indent: u32,
    line: u32,
}

impl Writer {
    fn append_byte(&mut self, byte: u8) -> bool {
        if self.out.len() >= self.max_output {
            return false;
        }
        self.out.push(byte);
        true
    }

    fn append_bytes(&mut self, data: &[u8]) -> bool {
        if data.len() > self.max_output.saturating_sub(self.out.len()) {
            return false;
        }
        self.out.extend_from_slice(data);
        true
    }

    fn write_bytes(&mut self, data: &[u8]) -> bool {
        if data.is_empty() {
            return true;
        }
        if self.column == 0 && data[0] != b'\n' {
            if u64::from(self.indent) > MO_LIMIT_LINE_LENGTH {
                return false;
            }
            let indent = self.indent as usize;
            if indent > self.max_output.saturating_sub(self.out.len()) {
                return false;
            }
            self.out.resize(self.out.len() + indent, b' ');
            self.column = self.indent;
            self.characters = self.indent;
            self.line_indent = self.indent;
        }
        if !self.append_bytes(data) {
            return false;
        }
        let mut start = 0usize;
        for (i, &b) in data.iter().enumerate() {
            if b == b'\n' {
                start = i + 1;
                self.characters = 0;
                self.line += 1;
            } else if b != b'\r' && (b & 0xc0) != 0x80 {
                self.characters += 1;
                if u64::from(self.characters) > MO_LIMIT_LINE_LENGTH {
                    return false;
                }
            }
        }
        if start != 0 {
            self.column = 0;
        }
        let Some(width) = (self.display_width)(&data[start..]) else {
            return false;
        };
        let Ok(width32) = u32::try_from(width) else {
            return false;
        };
        self.column = add_width(self.column, width32);
        true
    }

    fn newline(&mut self, nodes: &mut [DocNode<'_>], always: bool) -> bool {
        let pending = std::mem::take(&mut self.pending);
        let count = pending.len();
        for (i, idx) in pending.into_iter().enumerate() {
            let inline_comment = self.column != 0;
            nodes[idx].suffix_offset = self.out.len();
            nodes[idx].suffix_line = self.line;
            nodes[idx].suffix_column = self.column;
            if self.column != 0 && !self.write_bytes(b"  ") {
                return false;
            }
            let comment_bytes = nodes[idx].bytes;
            if !self.write_bytes(comment_bytes) {
                return false;
            }
            nodes[idx].suffix_characters = self.characters;
            if inline_comment {
                self.comments.push(idx);
            }
            if i + 1 < count {
                if !self.append_byte(b'\n') {
                    return false;
                }
                self.column = 0;
                self.characters = 0;
                self.line += 1;
            }
        }
        if always || self.column != 0 {
            if !self.append_byte(b'\n') {
                return false;
            }
            self.column = 0;
            self.characters = 0;
            self.line += 1;
        }
        true
    }

    fn align_comments(&mut self, nodes: &mut [DocNode<'_>]) -> bool {
        let mut changed = false;
        let mut i = 0usize;
        while i < self.comments.len() {
            let mut last = i;
            let mut widest = nodes[self.comments[i]].suffix_column;
            while last + 1 < self.comments.len()
                && nodes[self.comments[last + 1]].suffix_line
                    == nodes[self.comments[last]].suffix_line + 1
            {
                last += 1;
                let col = nodes[self.comments[last]].suffix_column;
                if col > widest {
                    widest = col;
                }
            }
            for &idx in &self.comments[i..=last] {
                let pad = widest - nodes[idx].suffix_column;
                nodes[idx].padding = pad;
                if u64::from(pad) > MO_LIMIT_LINE_LENGTH - u64::from(nodes[idx].suffix_characters) {
                    return false;
                }
                changed = changed || pad != 0;
            }
            i = last + 1;
        }
        if !changed {
            return true;
        }

        let mut aligned = Vec::with_capacity(self.out.len());
        let mut at = 0usize;
        for &idx in &self.comments {
            let pad = nodes[idx].padding as usize;
            if pad == 0 {
                continue;
            }
            let offset = nodes[idx].suffix_offset;
            if offset < at || offset > self.out.len() {
                return false;
            }
            let chunk_len = offset - at;
            if chunk_len + pad > self.max_output.saturating_sub(aligned.len()) {
                return false;
            }
            aligned.extend_from_slice(&self.out[at..offset]);
            aligned.resize(aligned.len() + pad, b' ');
            at = offset;
        }
        let tail_len = self.out.len() - at;
        if tail_len > self.max_output.saturating_sub(aligned.len()) {
            return false;
        }
        aligned.extend_from_slice(&self.out[at..]);
        self.out = aligned;
        true
    }
}
