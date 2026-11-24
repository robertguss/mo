//! 100-column Wadler source formatter (`src/syntax/format.h` & `src/syntax/format.c` parity).

use mo_def::limits::{MO_LIMIT_LINE_LENGTH, MO_LIMIT_SOURCE_INFERENCE_DEPTH};
use thiserror::Error;

use crate::ast::{
    CommentAnchor, CommentId, ModuleSyntax, NodeId, SyntaxKind, SyntaxTree, TokenKind,
    duration_unit,
};
use crate::format_doc::{DocKind, Document};

/// Display columns in a valid UTF-8 segment without line breaks.
///
/// Matches `mo_prim::unicode16::display_width_bytes` so `mo-syntax` never depends on `mo-prim`.
pub type DisplayWidthFn = fn(&[u8]) -> Option<u64>;

/// Configuration for [`format_module`].
#[derive(Debug, Clone, Copy)]
pub struct FormatOptions {
    pub width: u32,
    pub display_width: DisplayWidthFn,
}

impl FormatOptions {
    /// Default 100-column layout using the supplied Unicode display-width callback.
    #[must_use]
    pub const fn new(width: u32, display_width: DisplayWidthFn) -> Self {
        Self {
            width,
            display_width,
        }
    }
}

/// Errors returned when [`format_module`] refuses to print a module.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum FormatError {
    #[error("formatter width must be within 1..={MO_LIMIT_LINE_LENGTH}")]
    InvalidWidth,
    #[error("source printing exceeds this build's memory, depth, output or line limit")]
    LimitOrIncomplete,
}

struct CommentCursor {
    node: NodeId,
    next_idx: usize,
    counts: [u32; TokenKind::COUNT],
}

impl CommentCursor {
    const fn new(node: NodeId) -> Self {
        Self {
            node,
            next_idx: 0,
            counts: [0; TokenKind::COUNT],
        }
    }
}

struct Printer<'a> {
    tree: &'a SyntaxTree,
    doc: Document<'a>,
    depth: u32,
    cursors: Vec<CommentCursor>,
}

const FIXED_TOKENS: &[(TokenKind, &str)] = &[
    (TokenKind::Module, "module"),
    (TokenKind::Expose, "expose"),
    (TokenKind::Use, "use"),
    (TokenKind::Fn, "fn"),
    (TokenKind::Struct, "struct"),
    (TokenKind::Enum, "enum"),
    (TokenKind::Var, "var"),
    (TokenKind::If, "if"),
    (TokenKind::Else, "else"),
    (TokenKind::Case, "case"),
    (TokenKind::For, "for"),
    (TokenKind::In, "in"),
    (TokenKind::While, "while"),
    (TokenKind::Break, "break"),
    (TokenKind::Continue, "continue"),
    (TokenKind::Return, "return"),
    (TokenKind::Try, "try"),
    (TokenKind::And, "and"),
    (TokenKind::Or, "or"),
    (TokenKind::Not, "not"),
    (TokenKind::Is, "is"),
    (TokenKind::Implies, "implies"),
    (TokenKind::True, "true"),
    (TokenKind::False, "false"),
    (TokenKind::Stop, "stop"),
    (TokenKind::Promise, "promise"),
    (TokenKind::Allows, "allows"),
    (TokenKind::Forbids, "forbids"),
    (TokenKind::Examples, "examples"),
    (TokenKind::Test, "test"),
    (TokenKind::Assert, "assert"),
    (TokenKind::Where, "where"),
    (TokenKind::End, "end"),
    (TokenKind::Service, "service"),
    (TokenKind::Actor, "actor"),
    (TokenKind::Api, "api"),
    (TokenKind::LParen, "("),
    (TokenKind::RParen, ")"),
    (TokenKind::LBrack, "["),
    (TokenKind::RBrack, "]"),
    (TokenKind::LBrace, "{"),
    (TokenKind::RBrace, "}"),
    (TokenKind::Comma, ","),
    (TokenKind::Colon, ":"),
    (TokenKind::Dot, "."),
    (TokenKind::DotDot, ".."),
    (TokenKind::Bar, "|"),
    (TokenKind::Assign, "="),
    (TokenKind::PlusEq, "+="),
    (TokenKind::MinusEq, "-="),
    (TokenKind::Plus, "+"),
    (TokenKind::Minus, "-"),
    (TokenKind::Star, "*"),
    (TokenKind::Slash, "/"),
    (TokenKind::Percent, "%"),
    (TokenKind::Eq, "=="),
    (TokenKind::Ne, "!="),
    (TokenKind::Lt, "<"),
    (TokenKind::Le, "<="),
    (TokenKind::Gt, ">"),
    (TokenKind::Ge, ">="),
];

fn match_fixed_token(s: &str) -> Option<(TokenKind, usize)> {
    let mut best: Option<(TokenKind, usize)> = None;
    for &(tok, spelling) in FIXED_TOKENS {
        if s.starts_with(spelling) && best.is_none_or(|(_, len)| spelling.len() > len) {
            best = Some((tok, spelling.len()));
        }
    }
    best
}

fn is_bracketed(kind: SyntaxKind) -> bool {
    matches!(
        kind,
        SyntaxKind::Group | SyntaxKind::Tuple | SyntaxKind::List | SyntaxKind::Map
    )
}

fn integer_is_one(bytes: &[u8]) -> bool {
    let start = if bytes.len() > 2 && bytes[0] == b'0' && matches!(bytes[1], b'x' | b'b') {
        2
    } else {
        0
    };
    let mut one = false;
    for &digit in &bytes[start..] {
        if digit == b'_' {
            continue;
        }
        if one || (digit != b'0' && digit != b'1') {
            return false;
        }
        one = digit == b'1';
    }
    one
}

impl<'a> Printer<'a> {
    fn new(tree: &'a SyntaxTree, display_width: DisplayWidthFn) -> Self {
        Self {
            tree,
            doc: Document::new(display_width),
            depth: 0,
            cursors: Vec::new(),
        }
    }

    fn op(&mut self, kind: DocKind) {
        self.doc.add(kind, b"", 0);
    }

    fn soft(&mut self, space: usize) {
        self.doc.add(DocKind::Break, b"", space);
    }

    fn emit_anchor(
        &mut self,
        cursor: &mut CommentCursor,
        kind: CommentAnchor,
        tok: TokenKind,
        index: u32,
    ) {
        let trailing = &self.tree.node(cursor.node).trailing;
        while cursor.next_idx < trailing.len() {
            let cid = trailing[cursor.next_idx];
            let c = self.tree.comment(cid);
            if c.anchor != kind || c.token != tok || c.token_index != index {
                break;
            }
            let bytes = self.tree.comment_text(cid);
            self.doc.add(DocKind::Comment, bytes, 0);
            self.op(DocKind::SuffixLine);
            cursor.next_idx += 1;
        }
    }

    fn token_anchor(&mut self, tok: TokenKind) {
        let Some(mut cursor) = self.cursors.pop() else {
            self.doc.ok = false;
            return;
        };
        let idx = cursor.counts[tok.as_index()];
        cursor.counts[tok.as_index()] += 1;
        self.emit_anchor(&mut cursor, CommentAnchor::Token, tok, idx);
        self.cursors.push(cursor);
    }

    fn text(&mut self, mut s: &'static str) {
        while !s.is_empty() {
            if let Some(rest) = s.strip_prefix(' ') {
                self.doc.add(DocKind::Space, b"", 1);
                s = rest;
                continue;
            }
            let Some((tok, len)) = match_fixed_token(s) else {
                self.doc.ok = false;
                return;
            };
            self.doc.add(DocKind::Text, &s.as_bytes()[..len], 0);
            self.token_anchor(tok);
            s = &s[len..];
        }
    }

    fn clause_word(&mut self, word: &'static str) {
        self.doc.add(DocKind::Text, word.as_bytes(), 0);
        self.token_anchor(TokenKind::Name);
        self.text(" ");
    }

    fn emit_comments(&mut self, list: &[CommentId], trailing: bool) {
        for &cid in list {
            if !trailing {
                self.op(DocKind::LineIf);
            }
            let bytes = self.tree.comment_text(cid);
            self.doc.add(
                if trailing {
                    DocKind::Comment
                } else {
                    DocKind::Text
                },
                bytes,
                0,
            );
            if !trailing {
                self.op(DocKind::Line);
            }
        }
    }

    fn statements(&mut self, seq: NodeId) {
        let children = &self.tree.node(seq).children;
        for &child in children {
            if !self.doc.ok {
                break;
            }
            let Some(cid) = child else {
                self.doc.ok = false;
                return;
            };
            if self.tree.node(cid).blank_before {
                self.op(DocKind::Blank);
            }
            self.node(cid, None);
            self.op(DocKind::LineIf);
        }
    }

    fn separated(&mut self, seq: NodeId, separator: &'static str) {
        let dotted = separator == ".";
        if dotted {
            self.op(DocKind::Group);
            self.op(DocKind::Nest);
        }
        let children = &self.tree.node(seq).children;
        for (i, &child) in children.iter().enumerate() {
            if !self.doc.ok {
                break;
            }
            if i != 0 {
                if dotted {
                    self.soft(0);
                }
                self.text(separator);
            }
            self.node_opt(child);
        }
        if dotted {
            self.op(DocKind::Unnest);
            self.op(DocKind::EndGroup);
        }
    }

    fn brackets(
        &mut self,
        owner: Option<NodeId>,
        boundary_owner: NodeId,
        items: &[Option<NodeId>],
        open: &'static str,
        close: &'static str,
    ) {
        let same_as_current = self
            .cursors
            .last()
            .is_some_and(|c| c.node == boundary_owner);
        let mut separate = CommentCursor::new(boundary_owner);

        self.op(DocKind::AlignLine);
        self.op(DocKind::Group);
        self.text(open);
        self.op(DocKind::Nest);

        let envelope = owner.is_some_and(|o| self.tree.node(o).kind == SyntaxKind::Call)
            && items.len() == 1
            && items[0].is_some_and(|first| {
                let first_node = self.tree.node(first);
                is_bracketed(first_node.kind)
                    || (first_node.kind == SyntaxKind::Lambda
                        && first_node
                            .children
                            .get(1)
                            .copied()
                            .flatten()
                            .is_some_and(|b| self.tree.node(b).kind == SyntaxKind::Sequence))
            });

        if !envelope && !items.is_empty() {
            self.soft(0);
        }

        let count = items.len();
        for (i, &item) in items.iter().enumerate() {
            if !self.doc.ok {
                break;
            }
            self.node_opt(item);
            if i + 1 != count {
                self.text(",");
            }
            if same_as_current {
                if let Some(mut cur) = self.cursors.pop() {
                    self.emit_anchor(&mut cur, CommentAnchor::Item, TokenKind::Eof, i as u32);
                    self.cursors.push(cur);
                }
            } else {
                self.emit_anchor(&mut separate, CommentAnchor::Item, TokenKind::Eof, i as u32);
            }
            if i + 1 != count {
                self.soft(1);
            }
        }
        if let Some(o) = owner {
            self.emit_comments(&self.tree.node(o).ending, false);
        }
        self.op(DocKind::Unnest);
        if !envelope {
            self.soft(0);
        }
        self.text(close);
        self.op(DocKind::EndGroup);
        self.op(DocKind::EndAlign);

        if !same_as_current && separate.next_idx < self.tree.node(boundary_owner).trailing.len() {
            self.doc.ok = false;
        }
    }

    fn sequence_brackets(
        &mut self,
        owner: Option<NodeId>,
        seq: NodeId,
        open: &'static str,
        close: &'static str,
    ) {
        let items = &self.tree.node(seq).children;
        self.brackets(owner, seq, items, open, close);
    }

    fn binary_step(
        &mut self,
        operator_node: Option<NodeId>,
        tok: TokenKind,
        right_opt: Option<NodeId>,
    ) {
        let Some(word) = tok.fixed_spelling() else {
            self.doc.ok = false;
            return;
        };
        let space = if tok == TokenKind::DotDot { 0 } else { 1 };
        if space != 0 {
            self.text(" ");
        }
        if let Some(op_id) = operator_node {
            self.node(op_id, None);
        } else {
            self.text(word);
        }
        let right_bracketed = right_opt.is_some_and(|r| is_bracketed(self.tree.node(r).kind));
        if right_bracketed {
            if space != 0 {
                self.text(" ");
            }
        } else {
            self.soft(space);
        }
        self.node_opt(right_opt);
    }

    fn same_target(&self, a_id: NodeId, b_id: NodeId) -> bool {
        let a = self.tree.node(a_id);
        let b = self.tree.node(b_id);
        if a.kind != b.kind
            || a.children.len() != b.children.len()
            || !a.leading.is_empty()
            || !a.trailing.is_empty()
            || !a.ending.is_empty()
            || !b.leading.is_empty()
            || !b.trailing.is_empty()
            || !b.ending.is_empty()
        {
            return false;
        }
        if a.kind == SyntaxKind::Name {
            return self.tree.node_text(a_id) == self.tree.node_text(b_id);
        }
        if a.kind != SyntaxKind::FieldAccess && a.kind != SyntaxKind::Postfix {
            return false;
        }
        for (ac, bc) in a.children.iter().zip(&b.children) {
            match (ac, bc) {
                (Some(ai), Some(bi)) => {
                    if !self.same_target(*ai, *bi) {
                        return false;
                    }
                }
                _ => return false,
            }
        }
        true
    }

    fn addition_rhs(&self, nid: NodeId) -> Option<NodeId> {
        let n = self.tree.node(nid);
        if n.kind != SyntaxKind::Bind || !n.trailing.is_empty() {
            return None;
        }
        let lhs_id = n.children.first().copied().flatten()?;
        let rhs_id = n.children.get(1).copied().flatten()?;
        let rhs = self.tree.node(rhs_id);
        if rhs.kind != SyntaxKind::Binary
            || rhs.token != TokenKind::Plus
            || !rhs.leading.is_empty()
            || !rhs.trailing.is_empty()
            || !rhs.ending.is_empty()
        {
            return None;
        }
        let rhs_left = rhs.children.first().copied().flatten()?;
        if self.same_target(lhs_id, rhs_left) {
            rhs.children.get(1).copied().flatten()
        } else {
            None
        }
    }

    fn block_body(&mut self, body_opt: Option<NodeId>, owner: NodeId) {
        self.op(DocKind::LineIf);
        self.op(DocKind::Nest);
        if let Some(body) = body_opt {
            self.statements(body);
        } else {
            self.doc.ok = false;
        }
        self.emit_comments(&self.tree.node(owner).ending, false);
        self.op(DocKind::Unnest);
    }

    fn postfix_step(&mut self, step_id: NodeId, receiver_opt: Option<NodeId>) {
        let step = self.tree.node(step_id);
        if step.kind == SyntaxKind::Call || step.kind == SyntaxKind::TypeArgs {
            if let Some(seq_id) = step.children.last().copied().flatten() {
                let (open, close) = if step.kind == SyntaxKind::TypeArgs {
                    ("[", "]")
                } else {
                    ("(", ")")
                };
                self.sequence_brackets(Some(step_id), seq_id, open, close);
            } else {
                self.doc.ok = false;
            }
            return;
        }
        let field_opt = step.children.last().copied().flatten();
        // A decimal integer directly followed by `.digits` would re-lex as a
        // Decimal literal instead of Position(Int, Int), so keep a separating
        // space when flat (or newline when broken).
        let needs_space = step.kind == SyntaxKind::Position
            && receiver_opt.is_some_and(|r| {
                self.tree.node(r).kind == SyntaxKind::Int
                    && self
                        .tree
                        .node_text(r)
                        .is_some_and(|b| !b.starts_with(b"0x") && !b.starts_with(b"0b"))
            });
        self.soft(usize::from(needs_space));
        self.text(".");
        if step.kind == SyntaxKind::FieldAccess
            && let Some(recv_id) = receiver_opt
            && let Some(field_id) = field_opt
        {
            let recv = self.tree.node(recv_id);
            let field = self.tree.node(field_id);
            if recv.kind == SyntaxKind::Int
                && !recv.incomplete
                && field.kind == SyntaxKind::Name
                && !field.incomplete
                && let Some(field_bytes) = self.tree.node_text(field_id)
                && let Some(unit) = duration_unit(field_bytes)
                && let Some(recv_bytes) = self.tree.node_text(recv_id)
            {
                let len = unit.len() - usize::from(integer_is_one(recv_bytes));
                self.node(field_id, Some(&unit.as_bytes()[..len]));
                return;
            }
        }
        self.node_opt(field_opt);
    }

    fn node_opt(&mut self, nid: Option<NodeId>) {
        match nid {
            Some(id) => self.node(id, None),
            None => self.doc.ok = false,
        }
    }

    fn node(&mut self, nid: NodeId, text_override: Option<&'a [u8]>) {
        if !self.doc.ok {
            return;
        }
        let n = self.tree.node(nid);
        self.depth += 1;
        if n.incomplete || u64::from(self.depth) > MO_LIMIT_SOURCE_INFERENCE_DEPTH {
            self.doc.ok = false;
            self.depth -= 1;
            return;
        }

        self.cursors.push(CommentCursor::new(nid));
        self.emit_comments(&n.leading, false);
        let block = matches!(
            n.kind,
            SyntaxKind::Fn
                | SyntaxKind::Struct
                | SyntaxKind::Enum
                | SyntaxKind::Test
                | SyntaxKind::Promise
                | SyntaxKind::Examples
                | SyntaxKind::For
                | SyntaxKind::While
                | SyntaxKind::If
                | SyntaxKind::Case
                | SyntaxKind::Lambda
                | SyntaxKind::Service
                | SyntaxKind::Actor
                | SyntaxKind::Api
                | SyntaxKind::ApiScope
        );
        if block {
            self.op(DocKind::AlignLine);
        }
        self.op(DocKind::Group);
        self.print_node(nid, text_override);
        self.op(DocKind::EndGroup);
        if block {
            self.op(DocKind::EndAlign);
        }
        if let Some(cur) = self.cursors.pop()
            && cur.next_idx < self.tree.node(nid).trailing.len()
        {
            self.doc.ok = false;
        }
        self.depth -= 1;
    }

    fn print_node(&mut self, nid: NodeId, text_override: Option<&'a [u8]>) {
        let n = self.tree.node(nid);
        let child = |idx: usize| n.children.get(idx).copied().flatten();
        match n.kind {
            SyntaxKind::File => {
                if let Some(header) = child(0) {
                    self.node(header, None);
                    self.op(DocKind::LineIf);
                }
                if let Some(items) = child(1) {
                    self.statements(items);
                } else {
                    self.doc.ok = false;
                }
                self.emit_comments(&n.ending, false);
            }
            SyntaxKind::Module => {
                self.text("module ");
                if let Some(path) = child(0) {
                    self.separated(path, ".");
                } else {
                    self.doc.ok = false;
                }
                if let Some(expose) = child(1) {
                    self.op(DocKind::LineIf);
                    self.node(expose, None);
                }
                if let Some(uses) = child(2) {
                    for &u in &self.tree.node(uses).children {
                        self.op(DocKind::LineIf);
                        self.node_opt(u);
                    }
                } else {
                    self.doc.ok = false;
                }
                self.emit_comments(&n.ending, false);
            }
            SyntaxKind::Expose => {
                self.text("expose ");
                for (i, &c) in n.children.iter().enumerate() {
                    if i != 0 {
                        self.text(",");
                        self.soft(1);
                    }
                    self.node_opt(c);
                }
            }
            SyntaxKind::ExposeAll => {
                self.node_opt(child(0));
                self.text("(..)");
            }
            SyntaxKind::Use => {
                self.text("use ");
                if let Some(path) = child(0) {
                    self.separated(path, ".");
                } else {
                    self.doc.ok = false;
                }
                if let Some(items) = child(1) {
                    self.sequence_brackets(None, items, "{", "}");
                }
            }
            SyntaxKind::Fn => {
                self.op(DocKind::Group);
                self.text("fn ");
                self.node_opt(child(0));
                if let Some(params) = child(1) {
                    self.sequence_brackets(None, params, "(", ")");
                } else {
                    self.doc.ok = false;
                }
                if let Some(ret) = child(2) {
                    self.text(" : ");
                    self.node(ret, None);
                }
                if let Some(wh) = child(3) {
                    self.op(DocKind::Nest);
                    self.soft(1);
                    self.node(wh, None);
                    self.op(DocKind::Unnest);
                }
                self.op(DocKind::EndGroup);
                self.op(DocKind::LineIf);
                self.op(DocKind::Nest);
                if let (Some(spec), Some(body)) = (child(4), child(5)) {
                    let spec_children = &self.tree.node(spec).children;
                    for &sc in spec_children {
                        let Some(sc_id) = sc else {
                            self.doc.ok = false;
                            break;
                        };
                        if self.tree.node(sc_id).blank_before {
                            self.op(DocKind::Blank);
                        }
                        self.node(sc_id, None);
                        self.op(DocKind::LineIf);
                    }
                    let body_children = &self.tree.node(body).children;
                    if !spec_children.is_empty()
                        && let Some(first_body) = body_children.first().copied().flatten()
                        && !self.tree.node(first_body).blank_before
                    {
                        self.op(DocKind::Blank);
                    }
                    self.statements(body);
                } else {
                    self.doc.ok = false;
                }
                self.emit_comments(&n.ending, false);
                self.op(DocKind::Unnest);
                self.text("end");
            }
            SyntaxKind::Param | SyntaxKind::Field | SyntaxKind::NamedArg | SyntaxKind::MapEntry => {
                self.node_opt(child(0));
                self.text(": ");
                self.node_opt(child(1));
            }
            SyntaxKind::Where => {
                self.text("where ");
                for (i, &c) in n.children.iter().enumerate() {
                    if i != 0 {
                        self.text(",");
                        self.soft(1);
                    }
                    if let Some(pair) = c {
                        let p = self.tree.node(pair);
                        self.node_opt(p.children.first().copied().flatten());
                        self.text(": ");
                        self.node_opt(p.children.get(1).copied().flatten());
                    } else {
                        self.doc.ok = false;
                    }
                }
            }
            SyntaxKind::Struct | SyntaxKind::Enum => {
                self.text(if n.kind == SyntaxKind::Struct {
                    "struct "
                } else {
                    "enum "
                });
                self.node_opt(child(0));
                if let Some(tparams) = child(1) {
                    self.sequence_brackets(None, tparams, "(", ")");
                }
                self.block_body(child(2), nid);
                self.text("end");
            }
            SyntaxKind::CaseDecl => {
                self.node_opt(child(0));
                if let Some(fields) = child(1) {
                    if self.tree.node(fields).kind == SyntaxKind::Sequence {
                        self.sequence_brackets(None, fields, "(", ")");
                    } else {
                        self.brackets(None, nid, &[Some(fields)], "(", ")");
                    }
                }
            }
            SyntaxKind::Test => {
                self.text("test ");
                self.node_opt(child(0));
                self.block_body(child(1), nid);
                self.text("end");
            }
            SyntaxKind::Service => {
                self.text("service");
                self.block_body(child(0), nid);
                self.text("end");
            }
            SyntaxKind::Actor => {
                self.text("actor ");
                self.node_opt(child(0));
                self.block_body(child(1), nid);
                self.text("end");
            }
            SyntaxKind::ServiceState => {
                self.clause_word("state");
                self.node_opt(child(0));
                self.text(" = ");
                self.node_opt(child(1));
            }
            SyntaxKind::ServiceTools | SyntaxKind::ActorCalls => {
                self.clause_word(if n.kind == SyntaxKind::ActorCalls {
                    "calls"
                } else {
                    "tools"
                });
                if let Some(names) = child(0) {
                    for (i, &c) in self.tree.node(names).children.iter().enumerate() {
                        if i != 0 {
                            self.text(",");
                            self.soft(1);
                        }
                        self.node_opt(c);
                    }
                } else {
                    self.doc.ok = false;
                }
            }
            SyntaxKind::ServiceEvery => {
                self.clause_word("every");
                self.node_opt(child(0));
                self.text(" ");
                self.node_opt(child(1));
            }
            SyntaxKind::ServiceToken => {
                self.clause_word("token");
                self.clause_word("from");
                self.clause_word("env");
                self.node_opt(child(0));
            }
            SyntaxKind::Api => {
                self.text("api");
                self.block_body(child(0), nid);
                self.text("end");
            }
            SyntaxKind::ApiPort => {
                self.clause_word("port");
                self.node_opt(child(0));
            }
            SyntaxKind::ApiCors => {
                self.clause_word("cors");
                self.node_opt(child(0));
            }
            SyntaxKind::ApiSqlite => {
                self.clause_word("sqlite");
                self.node_opt(child(0));
                self.text(" ");
                self.clause_word("from");
                self.node_opt(child(1));
            }
            SyntaxKind::ApiActor => {
                self.text("actor ");
                self.node_opt(child(0));
                self.text(": ");
                self.node_opt(child(1));
            }
            SyntaxKind::ApiAuth => {
                self.clause_word("auth");
                self.node_opt(child(0));
                self.text(" ");
                self.clause_word("via");
                self.node_opt(child(1));
            }
            SyntaxKind::ApiRoute => {
                self.node_opt(child(0));
                self.text(" ");
                self.node_opt(child(1));
                self.text(" ");
                self.node_opt(child(2));
            }
            SyntaxKind::ApiStatic => {
                self.clause_word("static");
                self.node_opt(child(0));
                self.text(" ");
                self.clause_word("from");
                self.node_opt(child(1));
            }
            SyntaxKind::ApiJob => {
                self.clause_word("job");
                self.node_opt(child(0));
                self.text(" ");
                self.node_opt(child(1));
                if let Some(interval) = child(2) {
                    self.text(" ");
                    self.clause_word("every");
                    self.node(interval, None);
                }
            }
            SyntaxKind::ApiScope => {
                self.clause_word("scope");
                self.node_opt(child(0));
                self.block_body(child(1), nid);
                self.text("end");
            }
            SyntaxKind::ApiStatus => {
                self.clause_word("status");
                self.node_opt(child(0));
                if let Some(wildcards) = child(1) {
                    self.sequence_brackets(None, wildcards, "(", ")");
                }
                self.text(" = ");
                self.node_opt(child(2));
            }
            SyntaxKind::Promise | SyntaxKind::Examples => {
                let promise = n.kind == SyntaxKind::Promise;
                self.text(if promise { "promise" } else { "examples" });
                if promise && let Some(title) = child(0) {
                    self.text(" ");
                    self.node(title, None);
                }
                if promise && let Some(subject) = child(3) {
                    self.text(" ");
                    self.clause_word("on");
                    self.node(subject, None);
                }
                self.op(DocKind::LineIf);
                self.op(DocKind::Nest);
                if let Some(lines) = child(if promise { 1 } else { 0 }) {
                    self.statements(lines);
                } else {
                    self.doc.ok = false;
                }
                if promise {
                    if let Some(anchors) = child(2) {
                        self.statements(anchors);
                    } else {
                        self.doc.ok = false;
                    }
                }
                self.emit_comments(&n.ending, false);
                self.op(DocKind::Unnest);
                self.text("end");
            }
            SyntaxKind::Anchor => {
                self.text(if n.token == TokenKind::Allows {
                    "allows "
                } else {
                    "forbids "
                });
                if let Some(vals) = child(0) {
                    self.separated(vals, ", ");
                } else {
                    self.doc.ok = false;
                }
            }
            SyntaxKind::Type => {
                if n.token == TokenKind::Fn || n.token == TokenKind::LParen {
                    if n.token == TokenKind::Fn {
                        self.text("fn");
                    }
                    if let Some(args) = child(0) {
                        self.sequence_brackets(None, args, "(", ")");
                    } else {
                        self.doc.ok = false;
                    }
                    if n.token == TokenKind::Fn
                        && let Some(res) = child(1)
                    {
                        self.text(" : ");
                        self.node(res, None);
                    }
                } else {
                    self.node_opt(child(0));
                    if let Some(args) = child(1) {
                        self.sequence_brackets(None, args, "(", ")");
                    }
                }
            }
            SyntaxKind::Sequence => {
                self.separated(nid, ", ");
            }
            SyntaxKind::ExprStmt | SyntaxKind::Binder => {
                self.node_opt(child(0));
            }
            SyntaxKind::Const
            | SyntaxKind::Bind
            | SyntaxKind::Var
            | SyntaxKind::AddAssign
            | SyntaxKind::SubAssign => {
                let rewritten = self.addition_rhs(nid);
                self.op(DocKind::Group);
                if n.kind == SyntaxKind::Var {
                    self.text("var ");
                }
                self.node_opt(child(0));
                self.text(if rewritten.is_some() || n.kind == SyntaxKind::AddAssign {
                    " +="
                } else if n.kind == SyntaxKind::SubAssign {
                    " -="
                } else {
                    " ="
                });
                let rhs = rewritten.or_else(|| child(1));
                self.soft(1);
                self.op(DocKind::EndGroup);
                self.node_opt(rhs);
            }
            SyntaxKind::Return | SyntaxKind::Break | SyntaxKind::Continue => {
                let ret = n.kind == SyntaxKind::Return;
                self.text(if ret {
                    "return"
                } else if n.kind == SyntaxKind::Break {
                    "break"
                } else {
                    "continue"
                });
                if ret && let Some(val) = child(0) {
                    self.text(" ");
                    self.node(val, None);
                }
                if let Some(guard) = child(if ret { 1 } else { 0 }) {
                    self.text(" if ");
                    self.node(guard, None);
                }
            }
            SyntaxKind::Assert => {
                self.text("assert ");
                self.node_opt(child(0));
            }
            SyntaxKind::For | SyntaxKind::While => {
                let is_for = n.kind == SyntaxKind::For;
                self.text(if is_for { "for " } else { "while " });
                self.node_opt(child(0));
                if is_for {
                    self.text(" in ");
                    self.node_opt(child(1));
                }
                self.block_body(child(if is_for { 2 } else { 1 }), nid);
                self.text("end");
            }
            SyntaxKind::If | SyntaxKind::LineIf => {
                let block = n.kind == SyntaxKind::If;
                self.text("if ");
                self.node_opt(child(0));
                if block {
                    self.block_body(child(1), nid);
                } else {
                    self.text(": ");
                    self.node_opt(child(1));
                }
                if let Some(parts) = child(2) {
                    for &part in &self.tree.node(parts).children {
                        if block {
                            self.op(DocKind::LineIf);
                        } else {
                            self.text(" ");
                        }
                        self.node_opt(part);
                    }
                } else {
                    self.doc.ok = false;
                }
                if block {
                    self.op(DocKind::LineIf);
                    self.text("end");
                }
            }
            SyntaxKind::Else | SyntaxKind::IfPart => {
                let branch = n.kind == SyntaxKind::IfPart;
                self.text(if branch { "else if " } else { "else" });
                if branch {
                    self.node_opt(child(0));
                }
                let body_opt = child(if branch { 1 } else { 0 });
                if let Some(body) = body_opt {
                    if self.tree.node(body).kind == SyntaxKind::Sequence {
                        self.block_body(Some(body), nid);
                    } else {
                        self.text(": ");
                        self.node(body, None);
                    }
                } else {
                    self.doc.ok = false;
                }
            }
            SyntaxKind::Case => {
                self.text("case ");
                self.node_opt(child(0));
                self.block_body(child(1), nid);
                self.text("end");
            }
            SyntaxKind::Arm => {
                if let Some(alts) = child(0) {
                    self.separated(alts, " | ");
                } else {
                    self.doc.ok = false;
                }
                if let Some(guard) = child(1) {
                    self.text(" if ");
                    self.node(guard, None);
                }
                self.text(":");
                let Some(body_id) = child(2) else {
                    self.doc.ok = false;
                    return;
                };
                let body = self.tree.node(body_id);
                let first_stmt = if body.children.len() == 1 {
                    body.children[0]
                } else {
                    None
                };
                let inline_body = first_stmt.is_some_and(|stmt_id| {
                    let stmt = self.tree.node(stmt_id);
                    if !stmt.leading.is_empty() {
                        return false;
                    }
                    let only_id = if stmt.kind == SyntaxKind::ExprStmt {
                        stmt.children.first().copied().flatten().unwrap_or(stmt_id)
                    } else {
                        stmt_id
                    };
                    !matches!(
                        self.tree.node(only_id).kind,
                        SyntaxKind::For | SyntaxKind::While | SyntaxKind::Case | SyntaxKind::If
                    )
                });
                if inline_body {
                    self.text(" ");
                    self.node_opt(first_stmt);
                    self.op(DocKind::LineIf);
                    self.op(DocKind::Nest);
                    self.emit_comments(&n.ending, false);
                    self.op(DocKind::Unnest);
                } else {
                    self.block_body(Some(body_id), nid);
                }
            }
            SyntaxKind::Lambda => {
                self.text("fn");
                if let Some(params) = child(0) {
                    self.sequence_brackets(None, params, "(", ")");
                } else {
                    self.doc.ok = false;
                }
                if let Some(body) = child(1) {
                    if self.tree.node(body).kind == SyntaxKind::Sequence {
                        self.block_body(Some(body), nid);
                        self.text("end");
                    } else {
                        self.text(" ");
                        self.node(body, None);
                        self.text(" end");
                    }
                } else {
                    self.doc.ok = false;
                }
            }
            SyntaxKind::Binary | SyntaxKind::Is | SyntaxKind::Range => {
                self.node_opt(child(0));
                self.op(DocKind::Nest);
                self.binary_step(None, n.token, child(1));
                self.op(DocKind::Unnest);
            }
            SyntaxKind::BinaryChain => {
                self.node_opt(child(0));
                self.op(DocKind::Nest);
                let mut i = 1usize;
                while i + 1 < n.children.len() {
                    let op_opt = n.children[i];
                    let rhs_opt = n.children[i + 1];
                    if let Some(op_id) = op_opt {
                        let tok = self.tree.node(op_id).token;
                        self.binary_step(Some(op_id), tok, rhs_opt);
                    } else {
                        self.doc.ok = false;
                    }
                    i += 2;
                }
                self.op(DocKind::Unnest);
            }
            SyntaxKind::Negate | SyntaxKind::Not | SyntaxKind::Try => {
                self.text(match n.kind {
                    SyntaxKind::Negate => "-",
                    SyntaxKind::Not => "not ",
                    _ => "try ",
                });
                let operand = child(0);
                if n.kind == SyntaxKind::Negate
                    && operand.is_some_and(|o| self.tree.node(o).kind == SyntaxKind::Negate)
                {
                    self.text(" ");
                }
                self.node_opt(operand);
            }
            SyntaxKind::Prefix => {
                let count = n.children.len();
                if count == 0 {
                    self.doc.ok = false;
                    return;
                }
                for i in 0..(count - 1) {
                    self.node_opt(n.children[i]);
                    let cur_tok = n.children[i].map(|id| self.tree.node(id).token);
                    let next_tok = n.children[i + 1].map(|id| self.tree.node(id).token);
                    if cur_tok != Some(TokenKind::Minus) || next_tok == Some(TokenKind::Minus) {
                        self.text(" ");
                    }
                }
                self.node_opt(n.children[count - 1]);
            }
            SyntaxKind::Call
            | SyntaxKind::TypeArgs
            | SyntaxKind::FieldAccess
            | SyntaxKind::Qualified
            | SyntaxKind::Position => {
                let receiver = if n.children.len() == 2 {
                    let r = child(0);
                    self.node_opt(r);
                    r
                } else {
                    None
                };
                let nest = n.kind != SyntaxKind::Call && n.kind != SyntaxKind::TypeArgs;
                if nest {
                    self.op(DocKind::Nest);
                }
                self.postfix_step(nid, receiver);
                if nest {
                    self.op(DocKind::Unnest);
                }
            }
            SyntaxKind::Postfix => {
                let base = child(0);
                self.node_opt(base);
                self.op(DocKind::Nest);
                for i in 1..n.children.len() {
                    let Some(step_id) = n.children[i] else {
                        self.doc.ok = false;
                        break;
                    };
                    self.cursors.push(CommentCursor::new(step_id));
                    self.emit_comments(&self.tree.node(step_id).leading, false);
                    self.op(DocKind::Group);
                    self.postfix_step(step_id, if i == 1 { base } else { None });
                    self.op(DocKind::EndGroup);
                    if let Some(cur) = self.cursors.pop()
                        && cur.next_idx < self.tree.node(step_id).trailing.len()
                    {
                        self.doc.ok = false;
                    }
                }
                self.op(DocKind::Unnest);
            }
            SyntaxKind::Int
            | SyntaxKind::Decimal
            | SyntaxKind::Char
            | SyntaxKind::Text
            | SyntaxKind::True
            | SyntaxKind::False
            | SyntaxKind::Name
            | SyntaxKind::CapName
            | SyntaxKind::Underscore
            | SyntaxKind::Token => {
                let bytes = match text_override {
                    Some(over) => over,
                    None => match self.tree.node_text(nid) {
                        Some(b) => b,
                        None => {
                            self.doc.ok = false;
                            return;
                        }
                    },
                };
                self.doc.add(DocKind::Text, bytes, 0);
                self.token_anchor(n.token);
            }
            SyntaxKind::TextInterpolated => {
                for &c in &n.children {
                    self.node_opt(c);
                }
            }
            SyntaxKind::Group | SyntaxKind::Tuple | SyntaxKind::List | SyntaxKind::Map => {
                let (open, close) = match n.kind {
                    SyntaxKind::List => ("[", "]"),
                    SyntaxKind::Map => ("{", "}"),
                    _ => ("(", ")"),
                };
                self.brackets(Some(nid), nid, &n.children, open, close);
            }
            SyntaxKind::Rest => {
                self.text("..");
                self.node_opt(child(0));
            }
            SyntaxKind::Stop => {
                self.text("stop ");
                self.node_opt(child(0));
            }
        }
    }
}

/// Formats a complete, successfully parsed [`ModuleSyntax`] into canonical Mo source bytes.
///
/// Mirrors `src/syntax/format.c:mo_format` byte-for-byte.
pub fn format_module(
    module: &ModuleSyntax,
    options: FormatOptions,
) -> Result<Vec<u8>, FormatError> {
    if options.width == 0 || u64::from(options.width) > MO_LIMIT_LINE_LENGTH {
        return Err(FormatError::InvalidWidth);
    }
    let mut printer = Printer::new(&module.tree, options.display_width);
    printer.node(module.root, None);
    printer
        .doc
        .render(options)
        .ok_or(FormatError::LimitOrIncomplete)
}
