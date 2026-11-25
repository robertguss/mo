//! Recursive-descent parser with exact line recovery, fragment mode, and header-only mode
//! (`src/syntax/parser.c` parity).

use mo_base::{DiagBag, SourceFile, SourceMap, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_CHOICE_CASES, MO_LIMIT_FIELDS, MO_LIMIT_MODULE_NAMES,
    MO_LIMIT_NESTING_DEPTH, MO_LIMIT_PARAMETERS,
};

use crate::ast::{
    CHILD_PTR_COST, Header, ModuleSyntax, NodeId, ParsedHeader, SyntaxKind, SyntaxNode, SyntaxTree,
    TokenKind,
};
use crate::comments::attach_trivia;
use crate::lexer::{Lexer, Token};

/// Configuration options for parsing Mo source.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParseOptions {
    /// Maximum byte budget for the syntax arena plus comment attachment scratch (`E1054`).
    pub arena_limit: usize,
}

impl Default for ParseOptions {
    fn default() -> Self {
        Self {
            arena_limit: MO_CLI_CHECK_SIZE as usize,
        }
    }
}

/// Internal abort signal replacing `setjmp`/`longjmp` across recovery boundaries.
#[derive(Debug, Clone, Copy)]
struct ParseAbort;

#[derive(Debug, Clone, Copy, Default)]
struct ParseMeta {
    capacity: u32,
    postfix_start: u32,
    validated: u8,
    operator: Option<NodeId>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ControlHeaderKind {
    If,
    Case,
    For,
    While,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DeclHeaderKind {
    Fn,
    Struct,
    Enum,
    Actor,
    Test,
    Promise,
    Examples,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum BracketKind {
    Paren,
    Brack,
    Brace,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ItemContext {
    Module,
    Fragment,
    Statement,
    Case,
    Field,
    CaseDecl,
    Spec,
    Expr,
    Anchor,
    Service,
    Actor,
    Api,
    ApiScope,
}

const MODULE_SYNTAX_ARENA_COST: usize = 64;

struct Parser<'a, 'd> {
    lx: Lexer<'a>,
    tree: SyntaxTree,
    meta: Vec<ParseMeta>,
    diags: &'d mut DiagBag,
    current: Token,
    lookahead: Option<Token>,
    previous: Span,
    depth: u32,
    names: u32,
    depth_opener: TokenKind,
    expect_code: Option<ErrorCode>,
}

impl<'a, 'd> Parser<'a, 'd> {
    fn new(source: &'a SourceFile, file: u32, diags: &'d mut DiagBag, arena_limit: usize) -> Self {
        Self {
            lx: Lexer::new(source, file),
            tree: SyntaxTree::with_budget(arena_limit),
            meta: Vec::new(),
            diags,
            current: Token::empty(file),
            lookahead: None,
            previous: Span::new(file, 0, 0),
            depth: 0,
            names: 0,
            depth_opener: TokenKind::Eof,
            expect_code: None,
        }
    }

    fn err<T>(
        &mut self,
        code: ErrorCode,
        span: Span,
        message: &str,
        reason: Option<&str>,
    ) -> Result<T, ParseAbort> {
        self.lx.error(self.diags, code, span, message, reason);
        Err(ParseAbort)
    }

    fn out_of_memory<T>(&mut self) -> Result<T, ParseAbort> {
        let span = self.current.span;
        self.lx.error(
            self.diags,
            ErrorCode::SyntaxNoMemory,
            span,
            "the syntax arena has no room",
            None,
        );
        self.lx.fatal = true;
        Err(ParseAbort)
    }

    fn next_tok(&mut self) {
        self.previous = self.current.span;
        self.current = match self.lookahead.take() {
            Some(tok) => tok,
            None => self.lx.next_token(&mut self.tree, self.diags),
        };
    }

    fn peek_next(&mut self) -> TokenKind {
        if let Some(tok) = self.lookahead {
            return tok.kind;
        }
        let tok = self.lx.next_token(&mut self.tree, self.diags);
        self.lookahead = Some(tok);
        tok.kind
    }

    fn take(&mut self, token: TokenKind) -> bool {
        if self.current.kind != token {
            return false;
        }
        self.next_tok();
        true
    }

    fn advice(&self, span: Span) -> Option<&'static str> {
        const WORDS: &[(&[u8], &str)] = &[
            (b"def", "Mo defines a function with fn"),
            (b"func", "Mo defines a function with fn"),
            (b"function", "Mo defines a function with fn"),
            (b"fun", "Mo defines a function with fn"),
            (b"elif", "Mo writes else if"),
            (b"elsif", "Mo writes else if"),
            (
                b"then",
                "an if needs no then: the condition, a line break, the body, and end",
            ),
            (b"unless", "Mo writes if not"),
            (b"until", "Mo writes while not"),
            (b"loop", "Mo loops with for and while"),
            (b"match", "Mo matches with case"),
            (b"switch", "Mo matches with case"),
            (
                b"when",
                "Mo matches with case, with arms written Pattern: value",
            ),
            (
                b"let",
                "Mo binds a name with name = value, and a changeable one with var",
            ),
            (
                b"const",
                "Mo binds a name with name = value, and a changeable one with var",
            ),
            (b"import", "Mo imports with use"),
            (
                b"export",
                "a module lists its public names on its expose line",
            ),
        ];
        let bytes = self.lx.source.bytes();
        let start = span.start as usize;
        let end = span.end as usize;
        if start > end || end > bytes.len() {
            return None;
        }
        let slice = &bytes[start..end];
        for &(word, msg) in WORDS {
            if slice == word {
                return Some(msg);
            }
        }
        None
    }

    fn expect(&mut self, token: TokenKind) -> Result<(), ParseAbort> {
        let previous = self.previous;
        if !self.take(token) {
            let code = self.expect_code.unwrap_or(ErrorCode::SourceSyntax);
            let sec = code.section().unwrap_or("");
            let rule = match code {
                ErrorCode::SourceFunction => {
                    "A signature gives each parameter's type and uses : before its result"
                }
                ErrorCode::SourceType => "A type has a name and parenthesized type arguments",
                ErrorCode::SourceIf => {
                    "An if uses a condition, then a line break and body or : and a value"
                }
                ErrorCode::SourceLoop => "A for uses a binder followed by in; loops close with end",
                ErrorCode::SourceCall => "Arguments are passed in parentheses, separated by commas",
                ErrorCode::SourceHeader => {
                    "A module starts with its name, then expose and use lines"
                }
                ErrorCode::SourceLiteral => "A tuple has two or more elements in parentheses",
                ErrorCode::SourceList => {
                    "A list's elements are separated by commas inside brackets"
                }
                ErrorCode::SourceMap => {
                    "A map has key: value entries separated by commas inside braces"
                }
                ErrorCode::SourceBinding => "A binding gives a name a value with =",
                _ => "Use the declaration or statement's syntax and close its delimiters",
            };
            let mut message = format!(
                "expected {}, found {}",
                token.diagnostic_name(),
                self.current.kind.diagnostic_name()
            );
            let help = self
                .advice(self.current.span)
                .or_else(|| self.advice(previous));
            if let Some(h) = help {
                message.push_str("; ");
                message.push_str(h);
            }
            let reason = format!("{rule} (reference {sec}).");
            return self.err(code, self.current.span, &message, Some(&reason));
        }
        if token == TokenKind::Nl {
            self.previous = previous;
        }
        Ok(())
    }

    fn report_reserved_name(
        &mut self,
        code: ErrorCode,
        note_reserved: bool,
    ) -> Result<(), ParseAbort> {
        let kw = self.current.kind.diagnostic_name();
        let sec = code.section().unwrap_or("");
        let rule = match code {
            ErrorCode::SourceFunction => {
                "A signature gives each parameter's type and uses : before its result"
            }
            _ => "Use the declaration or statement's syntax and close its delimiters",
        };
        let message = if note_reserved {
            format!("expected snake_case name, found {kw}; {kw} is a reserved keyword")
        } else {
            format!("expected snake_case name, found {kw}")
        };
        let reason = format!("{rule} (reference {sec}).");
        self.lx
            .error(self.diags, code, self.current.span, &message, Some(&reason));
        if self.lx.fatal {
            return Err(ParseAbort);
        }
        self.lx.failed = false;
        Ok(())
    }

    fn enter(&mut self) -> Result<(), ParseAbort> {
        if u64::from(self.depth) == MO_LIMIT_NESTING_DEPTH {
            let at = self.current.span;
            self.depth_opener = if self.current.kind == TokenKind::TextHead {
                TokenKind::Eof
            } else {
                self.current.kind
            };
            if self.current.kind == TokenKind::Fn && self.expect_code == Some(ErrorCode::SourceType)
            {
                // A function type closes with ), not a block's end.
                self.depth_opener = TokenKind::LParen;
                self.next_tok();
            }
            return self.err(
                ErrorCode::NestedTooDeeply,
                at,
                "code is nested too deeply",
                None,
            );
        }
        self.depth += 1;
        Ok(())
    }

    fn node(
        &mut self,
        kind: SyntaxKind,
        mut span: Span,
        children: &[Option<NodeId>],
    ) -> Result<NodeId, ParseAbort> {
        if self.previous.end > span.end {
            span.end = self.previous.end;
        }
        let mut incomplete = false;
        for &c in children {
            if let Some(cid) = c {
                incomplete = incomplete || self.tree.node(cid).incomplete;
            }
        }
        let cap = children.len() as u32;
        let node = SyntaxNode {
            kind,
            span,
            text: None,
            token: TokenKind::Eof,
            children: children.to_vec(),
            leading: Vec::new(),
            trailing: Vec::new(),
            ending: Vec::new(),
            incomplete,
            blank_before: false,
        };
        let Some(id) = self.tree.alloc_node(node) else {
            return self.out_of_memory();
        };
        self.meta.push(ParseMeta {
            capacity: cap,
            postfix_start: 0,
            validated: 0,
            operator: None,
        });
        Ok(id)
    }

    fn sequence(&mut self) -> Result<NodeId, ParseAbort> {
        let start = Span::new(
            self.lx.file,
            self.current.span.start,
            self.current.span.start,
        );
        self.node(SyntaxKind::Sequence, start, &[])
    }

    fn append(&mut self, parent: NodeId, child: Option<NodeId>) -> Result<(), ParseAbort> {
        let count = self.tree.node(parent).children.len() as u32;
        let cap = self.meta[parent.0 as usize].capacity;
        if count == cap {
            let new_cap = if cap == 0 { 1 } else { cap.saturating_mul(2) };
            let Some(charge) = (new_cap as usize).checked_mul(CHILD_PTR_COST) else {
                return self.out_of_memory();
            };
            if !self.tree.try_charge(charge) {
                return self.out_of_memory();
            }
            self.meta[parent.0 as usize].capacity = new_cap;
        }
        let child_info = child.map(|cid| {
            let c = self.tree.node(cid);
            (c.span.start, c.span.end, c.incomplete)
        });
        let p_node = self.tree.node_mut(parent);
        p_node.children.push(child);
        if let Some((c_start, c_end, c_inc)) = child_info {
            if p_node.children.len() == 1 {
                p_node.span.start = c_start;
            }
            p_node.span.end = c_end;
            p_node.incomplete = p_node.incomplete || c_inc;
        }
        Ok(())
    }

    fn leaf(&mut self) -> Result<NodeId, ParseAbort> {
        let token = self.current;
        let kind = match token.kind {
            TokenKind::Name => SyntaxKind::Name,
            TokenKind::CapName => SyntaxKind::CapName,
            TokenKind::Int => SyntaxKind::Int,
            TokenKind::Decimal => SyntaxKind::Decimal,
            TokenKind::Char => SyntaxKind::Char,
            TokenKind::True => SyntaxKind::True,
            TokenKind::False => SyntaxKind::False,
            TokenKind::Underscore => SyntaxKind::Underscore,
            TokenKind::Text | TokenKind::TextHead | TokenKind::TextMid | TokenKind::TextTail => {
                SyntaxKind::Text
            }
            TokenKind::Eof
            | TokenKind::Error
            | TokenKind::Nl
            | TokenKind::Module
            | TokenKind::Expose
            | TokenKind::Use
            | TokenKind::Fn
            | TokenKind::Struct
            | TokenKind::Enum
            | TokenKind::Var
            | TokenKind::If
            | TokenKind::Else
            | TokenKind::Case
            | TokenKind::For
            | TokenKind::In
            | TokenKind::While
            | TokenKind::Break
            | TokenKind::Continue
            | TokenKind::Return
            | TokenKind::Try
            | TokenKind::And
            | TokenKind::Or
            | TokenKind::Not
            | TokenKind::Is
            | TokenKind::Implies
            | TokenKind::Stop
            | TokenKind::Promise
            | TokenKind::Allows
            | TokenKind::Forbids
            | TokenKind::Examples
            | TokenKind::Test
            | TokenKind::Assert
            | TokenKind::Where
            | TokenKind::End
            | TokenKind::Service
            | TokenKind::Api
            | TokenKind::Actor
            | TokenKind::LParen
            | TokenKind::RParen
            | TokenKind::LBrack
            | TokenKind::RBrack
            | TokenKind::LBrace
            | TokenKind::RBrace
            | TokenKind::Comma
            | TokenKind::Colon
            | TokenKind::Dot
            | TokenKind::DotDot
            | TokenKind::Bar
            | TokenKind::Assign
            | TokenKind::PlusEq
            | TokenKind::MinusEq
            | TokenKind::Eq
            | TokenKind::Ne
            | TokenKind::Lt
            | TokenKind::Le
            | TokenKind::Gt
            | TokenKind::Ge
            | TokenKind::Plus
            | TokenKind::Minus
            | TokenKind::Star
            | TokenKind::Slash
            | TokenKind::Percent => SyntaxKind::Token,
        };
        self.next_tok();
        let id = self.node(kind, token.span, &[])?;
        let slice = &self.lx.source.bytes()[token.span.start as usize..token.span.end as usize];
        let Some(text) = self.tree.copy_bytes(slice) else {
            return self.out_of_memory();
        };
        let n = self.tree.node_mut(id);
        n.span = token.span;
        n.token = token.kind;
        n.text = Some(text);
        Ok(id)
    }

    fn name(&mut self, kind: TokenKind) -> Result<NodeId, ParseAbort> {
        if self.current.kind != kind {
            self.expect(kind)?;
        }
        self.leaf()
    }

    fn path(&mut self) -> Result<NodeId, ParseAbort> {
        let n = self.sequence()?;
        let first = self.name(TokenKind::CapName)?;
        self.append(n, Some(first))?;
        while self.take(TokenKind::Dot) {
            let part = self.name(TokenKind::CapName)?;
            self.append(n, Some(part))?;
        }
        Ok(n)
    }

    fn header(&mut self) -> Result<(NodeId, Header), ParseAbort> {
        let start = self.current.span;
        self.expect_code = Some(ErrorCode::SourceHeader);
        if self.current.kind != TokenKind::Module {
            return self.err(
                ErrorCode::SourceHeader,
                self.current.span,
                "a file is one module, named on its first line of code",
                Some("A module starts with its name, then expose and use lines (reference 10.1)."),
            );
        }
        self.expect(TokenKind::Module)?;
        let module_path = self.path()?;
        self.expect(TokenKind::Nl)?;
        let mut expose = None;
        if self.take(TokenKind::Expose) {
            let expose_start = self.previous;
            let exp = self.sequence()?;
            self.tree.node_mut(exp).kind = SyntaxKind::Expose;
            loop {
                let cap = self.current.kind == TokenKind::CapName;
                let mut item = self.name(if cap {
                    TokenKind::CapName
                } else {
                    TokenKind::Name
                })?;
                if cap && self.take(TokenKind::LParen) {
                    self.expect(TokenKind::DotDot)?;
                    self.expect(TokenKind::RParen)?;
                    let item_span = self.tree.node(item).span;
                    item = self.node(SyntaxKind::ExposeAll, item_span, &[Some(item)])?;
                }
                self.append(exp, Some(item))?;
                if !self.take(TokenKind::Comma) {
                    break;
                }
            }
            self.tree.node_mut(exp).span.start = expose_start.start;
            self.expect(TokenKind::Nl)?;
            expose = Some(exp);
        }
        let uses = self.sequence()?;
        while self.current.kind == TokenKind::Use {
            let use_start = self.current.span;
            self.next_tok();
            let use_path = self.path()?;
            let mut items = None;
            if self.take(TokenKind::LBrace) {
                let seq = self.sequence()?;
                loop {
                    let kind = if self.current.kind == TokenKind::CapName {
                        TokenKind::CapName
                    } else {
                        TokenKind::Name
                    };
                    let item = self.name(kind)?;
                    self.append(seq, Some(item))?;
                    if !self.take(TokenKind::Comma) {
                        break;
                    }
                }
                self.expect(TokenKind::RBrace)?;
                items = Some(seq);
            }
            self.expect(TokenKind::Nl)?;
            let use_node = self.node(SyntaxKind::Use, use_start, &[Some(use_path), items])?;
            self.append(uses, Some(use_node))?;
        }
        if self.tree.node(uses).children.is_empty() {
            self.tree.node_mut(uses).span =
                Span::new(self.lx.file, self.previous.end, self.previous.end);
        }
        let n = self.node(
            SyntaxKind::Module,
            start,
            &[Some(module_path), expose, Some(uses)],
        )?;
        let header = Header {
            span: self.tree.node(n).span,
            path: module_path,
            expose,
            uses,
        };
        Ok((n, header))
    }

    fn limit(
        &mut self,
        nid: NodeId,
        maximum: u64,
        code: ErrorCode,
        message: &str,
    ) -> Result<(), ParseAbort> {
        let (count, span) = {
            let n = self.tree.node(nid);
            (n.children.len() as u64, n.span)
        };
        if count > maximum {
            return self.err(code, span, message, None);
        }
        Ok(())
    }

    fn control_header_inner(
        &mut self,
        kind: ControlHeaderKind,
        colon: bool,
    ) -> Result<(Option<NodeId>, Option<NodeId>), ParseAbort> {
        self.expect_code = Some(match kind {
            ControlHeaderKind::If => ErrorCode::SourceIf,
            ControlHeaderKind::Case => ErrorCode::SourceCase,
            ControlHeaderKind::For | ControlHeaderKind::While => ErrorCode::SourceLoop,
        });
        self.next_tok();
        let part0 = if kind == ControlHeaderKind::For {
            let b = self.binder()?;
            self.expect(TokenKind::In)?;
            Some(b)
        } else {
            None
        };
        let min_prec = match kind {
            ControlHeaderKind::Case | ControlHeaderKind::For => 0,
            ControlHeaderKind::If | ControlHeaderKind::While => 1,
        };
        let part1 = self.expr(min_prec)?;
        self.validate(part1, false)?;
        if !colon || (self.current.kind != TokenKind::Colon && self.current.kind != TokenKind::Nl) {
            self.expect(TokenKind::Nl)?;
        }
        Ok((part0, Some(part1)))
    }

    fn control_header(
        &mut self,
        kind: ControlHeaderKind,
        colon: bool,
    ) -> Result<(bool, Option<NodeId>, Option<NodeId>), ParseAbort> {
        let saved_code = self.expect_code;
        let first_line = self.current.line;
        let saved_depth = self.depth;
        match self.control_header_inner(kind, colon) {
            Ok((p0, p1)) => {
                self.expect_code = saved_code;
                Ok((true, p0, p1))
            }
            Err(ParseAbort) => {
                self.expect_code = saved_code;
                self.depth = saved_depth;
                if self.lx.fatal {
                    return Err(ParseAbort);
                }
                self.recover(first_line);
                if self.lx.fatal {
                    return Err(ParseAbort);
                }
                Ok((false, None, None))
            }
        }
    }

    fn parse_type(&mut self) -> Result<NodeId, ParseAbort> {
        let saved_code = self.expect_code;
        self.expect_code = Some(ErrorCode::SourceType);
        let mut nested =
            self.current.kind == TokenKind::Fn || self.current.kind == TokenKind::LParen;
        if nested {
            self.enter()?;
        }
        let start = self.current.span;
        let n = if self.take(TokenKind::Fn) {
            self.expect(TokenKind::LParen)?;
            let args = self.sequence()?;
            if self.current.kind != TokenKind::RParen {
                loop {
                    let arg = self.parse_type()?;
                    self.append(args, Some(arg))?;
                    if !self.take(TokenKind::Comma) {
                        break;
                    }
                }
            }
            self.expect(TokenKind::RParen)?;
            self.limit(
                args,
                MO_LIMIT_PARAMETERS,
                ErrorCode::TooManyParameters,
                "a function type has too many parameters",
            )?;
            let result = if self.take(TokenKind::Colon) {
                Some(self.parse_type()?)
            } else {
                None
            };
            let id = self.node(SyntaxKind::Type, start, &[Some(args), result])?;
            self.tree.node_mut(id).token = TokenKind::Fn;
            id
        } else if self.take(TokenKind::LParen) {
            let args = self.sequence()?;
            let first = self.parse_type()?;
            self.append(args, Some(first))?;
            self.expect(TokenKind::Comma)?;
            loop {
                let item = self.parse_type()?;
                self.append(args, Some(item))?;
                if !self.take(TokenKind::Comma) {
                    break;
                }
            }
            self.expect(TokenKind::RParen)?;
            self.limit(
                args,
                MO_LIMIT_FIELDS,
                ErrorCode::TooManyFields,
                "a tuple type has too many elements",
            )?;
            let id = self.node(SyntaxKind::Type, start, &[Some(args)])?;
            self.tree.node_mut(id).token = TokenKind::LParen;
            id
        } else {
            let mut base = self.name(TokenKind::CapName)?;
            if self.take(TokenKind::Dot) {
                let right = self.name(TokenKind::CapName)?;
                base = self.node(SyntaxKind::Qualified, start, &[Some(base), Some(right)])?;
            }
            let mut args = None;
            if self.current.kind == TokenKind::LParen {
                self.enter()?;
                nested = true;
                self.next_tok();
                let seq = self.sequence()?;
                loop {
                    let arg = self.parse_type()?;
                    self.append(seq, Some(arg))?;
                    if !self.take(TokenKind::Comma) {
                        break;
                    }
                }
                self.expect(TokenKind::RParen)?;
                args = Some(seq);
            }
            self.node(SyntaxKind::Type, start, &[Some(base), args])?
        };
        if nested {
            self.depth -= 1;
        }
        self.expect_code = saved_code;
        Ok(n)
    }

    fn type_parameters(&mut self) -> Result<Option<NodeId>, ParseAbort> {
        if !self.take(TokenKind::LParen) {
            return Ok(None);
        }
        let args = self.sequence()?;
        loop {
            let arg = self.name(TokenKind::CapName)?;
            self.append(args, Some(arg))?;
            if !self.take(TokenKind::Comma) {
                break;
            }
        }
        self.expect(TokenKind::RParen)?;
        Ok(Some(args))
    }

    fn text(&mut self) -> Result<NodeId, ParseAbort> {
        if self.current.kind == TokenKind::Text {
            return self.leaf();
        }
        if self.current.kind != TokenKind::TextHead {
            self.expect(TokenKind::Text)?;
        }
        self.enter()?;
        let start = self.current.span;
        let n = self.sequence()?;
        self.tree.node_mut(n).kind = SyntaxKind::TextInterpolated;
        let head = self.leaf()?;
        self.append(n, Some(head))?;
        loop {
            let e = self.expr(0)?;
            self.append(n, Some(e))?;
            let tail = self.current.kind == TokenKind::TextTail;
            if !tail && self.current.kind != TokenKind::TextMid {
                self.expect(TokenKind::TextTail)?;
            }
            let part = self.leaf()?;
            self.append(n, Some(part))?;
            if tail {
                break;
            }
        }
        self.tree.node_mut(n).span.start = start.start;
        self.depth -= 1;
        Ok(n)
    }

    fn line_if_tail(
        &mut self,
        start: Span,
        condition: Option<NodeId>,
    ) -> Result<NodeId, ParseAbort> {
        self.expect(TokenKind::Colon)?;
        let body = self.expr(1)?;
        let otherwise = self.sequence()?;
        loop {
            self.expect(TokenKind::Else)?;
            let at = self.previous;
            if self.take(TokenKind::Colon) {
                let else_val = self.expr(0)?;
                let else_node = self.node(SyntaxKind::Else, at, &[Some(else_val)])?;
                self.append(otherwise, Some(else_node))?;
                break;
            }
            self.expect(TokenKind::If)?;
            let c = self.expr(1)?;
            self.expect(TokenKind::Colon)?;
            let b = self.expr(1)?;
            let part = self.node(SyntaxKind::IfPart, at, &[Some(c), Some(b)])?;
            self.append(otherwise, Some(part))?;
        }
        self.node(
            SyntaxKind::LineIf,
            start,
            &[condition, Some(body), Some(otherwise)],
        )
    }

    fn if_expr(&mut self, block: bool) -> Result<NodeId, ParseAbort> {
        self.enter()?;
        let start = self.current.span;
        let (valid, _, condition) = self.control_header(ControlHeaderKind::If, true)?;
        if valid && self.current.kind == TokenKind::Colon {
            let n = self.line_if_tail(start, condition)?;
            self.depth -= 1;
            return Ok(n);
        }
        if !block {
            self.depth_opener = TokenKind::If;
            self.expect(TokenKind::Colon)?;
        }
        if valid {
            self.expect(TokenKind::Nl)?;
        }
        let body = self.statements()?;
        let parts = self.sequence()?;
        while self.current.kind == TokenKind::Else {
            let else_start = self.current.span;
            self.next_tok();
            let part = if self.current.kind == TokenKind::If {
                let (good, _, cond) = self.control_header(ControlHeaderKind::If, false)?;
                let b = self.statements()?;
                let p = self.node(SyntaxKind::IfPart, else_start, &[cond, Some(b)])?;
                if !good {
                    self.tree.node_mut(p).incomplete = true;
                }
                p
            } else {
                self.expect(TokenKind::Nl)?;
                let b = self.statements()?;
                self.node(SyntaxKind::Else, else_start, &[Some(b)])?
            };
            let is_else = self.tree.node(part).kind == SyntaxKind::Else;
            self.append(parts, Some(part))?;
            if is_else {
                break;
            }
        }
        self.expect(TokenKind::End)?;
        let prev_end = self.previous.end;
        let part_count = self.tree.node(parts).children.len();
        for i in 0..part_count {
            let next_start = if i + 1 < part_count {
                self.tree.node(parts).children[i + 1]
                    .map_or(prev_end, |id| self.tree.node(id).span.start)
            } else {
                prev_end
            };
            if let Some(pid) = self.tree.node(parts).children[i] {
                self.tree.node_mut(pid).span.end = next_start;
            }
        }
        if part_count != 0 {
            self.tree.node_mut(parts).span.end = prev_end;
        }
        let n = self.node(SyntaxKind::If, start, &[condition, Some(body), Some(parts)])?;
        if !valid {
            self.tree.node_mut(n).incomplete = true;
        }
        self.depth -= 1;
        Ok(n)
    }

    fn case_expr(&mut self) -> Result<NodeId, ParseAbort> {
        self.enter()?;
        let start = self.current.span;
        let (valid, _, value) = self.control_header(ControlHeaderKind::Case, false)?;
        let lines = self.sequence()?;
        let mut arm: Option<NodeId> = None;
        while self.current.kind != TokenKind::End && self.current.kind != TokenKind::Eof {
            let line = self.read_item(ItemContext::Case)?;
            match line {
                None => {
                    self.tree.node_mut(lines).incomplete = true;
                    if let Some(arm_id) = arm {
                        self.tree.node_mut(arm_id).incomplete = true;
                        if let Some(body_id) =
                            self.tree.node(arm_id).children.get(2).copied().flatten()
                        {
                            self.tree.node_mut(body_id).incomplete = true;
                        }
                    }
                }
                Some(line_id) if self.tree.node(line_id).kind == SyntaxKind::Arm => {
                    if let Some(prev_arm) = arm {
                        let line_start = self.tree.node(line_id).span.start;
                        self.tree.node_mut(prev_arm).span.end = line_start;
                    }
                    let body = self.sequence()?;
                    let inline_stmt = self.tree.node(line_id).children.get(2).copied().flatten();
                    if let Some(stmt_id) = inline_stmt {
                        self.append(body, Some(stmt_id))?;
                    }
                    self.tree.node_mut(line_id).children[2] = Some(body);
                    arm = Some(line_id);
                    self.append(lines, Some(line_id))?;
                }
                Some(line_id) => {
                    if let Some(arm_id) = arm {
                        let line_inc = self.tree.node(line_id).incomplete;
                        if let Some(body_id) =
                            self.tree.node(arm_id).children.get(2).copied().flatten()
                        {
                            self.append(body_id, Some(line_id))?;
                        }
                        if line_inc {
                            self.tree.node_mut(arm_id).incomplete = true;
                            self.tree.node_mut(lines).incomplete = true;
                        }
                    } else {
                        self.append(lines, Some(line_id))?;
                    }
                }
            }
        }
        if let Some(arm_id) = arm {
            self.tree.node_mut(arm_id).span.end = self.current.span.start;
        }
        if let Some(last_line) = self.tree.node(lines).children.last().copied().flatten() {
            let last_end = self.tree.node(last_line).span.end;
            self.tree.node_mut(lines).span.end = last_end;
        }
        self.expect(TokenKind::End)?;
        let n = self.node(SyntaxKind::Case, start, &[value, Some(lines)])?;
        if !valid {
            self.tree.node_mut(n).incomplete = true;
        }
        self.depth -= 1;
        Ok(n)
    }

    fn lambda(&mut self) -> Result<NodeId, ParseAbort> {
        self.enter()?;
        let start = self.current.span;
        self.expect(TokenKind::Fn)?;
        self.expect(TokenKind::LParen)?;
        let params = self.sequence()?;
        if self.current.kind != TokenKind::RParen {
            loop {
                let kind = if self.current.kind == TokenKind::Underscore {
                    TokenKind::Underscore
                } else {
                    TokenKind::Name
                };
                let param = self.name(kind)?;
                self.append(params, Some(param))?;
                if !self.take(TokenKind::Comma) {
                    break;
                }
            }
        }
        self.expect(TokenKind::RParen)?;
        let body = if self.take(TokenKind::Nl) {
            self.statements()?
        } else {
            self.expr(0)?
        };
        self.expect(TokenKind::End)?;
        self.limit(
            params,
            MO_LIMIT_PARAMETERS,
            ErrorCode::TooManyParameters,
            "a function has too many parameters",
        )?;
        let n = self.node(SyntaxKind::Lambda, start, &[Some(params), Some(body)])?;
        self.depth -= 1;
        Ok(n)
    }

    fn primary(&mut self) -> Result<NodeId, ParseAbort> {
        let token = self.current.kind;
        let start = self.current.span;
        if matches!(
            token,
            TokenKind::Int
                | TokenKind::Decimal
                | TokenKind::Char
                | TokenKind::Name
                | TokenKind::CapName
                | TokenKind::Underscore
                | TokenKind::True
                | TokenKind::False
        ) {
            return self.leaf();
        }
        if matches!(token, TokenKind::Text | TokenKind::TextHead) {
            return self.text();
        }
        if token == TokenKind::Case {
            return self.case_expr();
        }
        if token == TokenKind::Fn {
            return self.lambda();
        }
        if self.take(TokenKind::Stop) {
            let reason = self.name(TokenKind::Text)?;
            return self.node(SyntaxKind::Stop, start, &[Some(reason)]);
        }
        if let Some(bracket) = match token {
            TokenKind::LParen => Some(BracketKind::Paren),
            TokenKind::LBrack => Some(BracketKind::Brack),
            TokenKind::LBrace => Some(BracketKind::Brace),
            _ => None,
        } {
            self.enter()?;
            let saved_code = self.expect_code;
            self.expect_code = Some(match bracket {
                BracketKind::Paren => ErrorCode::SourceLiteral,
                BracketKind::Brack => ErrorCode::SourceList,
                BracketKind::Brace => ErrorCode::SourceMap,
            });
            self.next_tok();
            let n = self.sequence()?;
            self.tree.node_mut(n).kind = match bracket {
                BracketKind::Paren => SyntaxKind::Group,
                BracketKind::Brack => SyntaxKind::List,
                BracketKind::Brace => SyntaxKind::Map,
            };
            let close = match bracket {
                BracketKind::Paren => TokenKind::RParen,
                BracketKind::Brack => TokenKind::RBrack,
                BracketKind::Brace => TokenKind::RBrace,
            };
            if self.current.kind != close || bracket == BracketKind::Paren {
                loop {
                    let item_start = self.current.span;
                    let item = if bracket == BracketKind::Brack && self.take(TokenKind::DotDot) {
                        let kind = if self.current.kind == TokenKind::Underscore {
                            TokenKind::Underscore
                        } else {
                            TokenKind::Name
                        };
                        let name = self.name(kind)?;
                        self.node(SyntaxKind::Rest, item_start, &[Some(name)])?
                    } else {
                        let e = self.expr(if bracket == BracketKind::Brace { 1 } else { 0 })?;
                        if bracket == BracketKind::Brace {
                            self.expect(TokenKind::Colon)?;
                            let val = self.expr(0)?;
                            self.node(SyntaxKind::MapEntry, item_start, &[Some(e), Some(val)])?
                        } else {
                            e
                        }
                    };
                    self.append(n, Some(item))?;
                    if !self.take(TokenKind::Comma) {
                        break;
                    }
                    if bracket == BracketKind::Paren {
                        self.tree.node_mut(n).kind = SyntaxKind::Tuple;
                    }
                    if self.current.kind == close {
                        if bracket == BracketKind::Paren && self.tree.node(n).children.len() < 2 {
                            return self.err(
                                ErrorCode::SourceLiteral,
                                self.current.span,
                                "a tuple has at least two elements",
                                Some("A tuple has two or more elements in parentheses (reference 3.6)."),
                            );
                        }
                        break;
                    }
                }
            }
            self.expect(close)?;
            if self.tree.node(n).kind == SyntaxKind::Tuple {
                self.limit(
                    n,
                    MO_LIMIT_FIELDS,
                    ErrorCode::TooManyFields,
                    "a tuple has too many elements",
                )?;
            }
            let prev_end = self.previous.end;
            let node_mut = self.tree.node_mut(n);
            node_mut.span.start = start.start;
            node_mut.span.end = prev_end;
            self.depth -= 1;
            self.expect_code = saved_code;
            return Ok(n);
        }
        if matches!(token, TokenKind::Examples | TokenKind::Promise) {
            self.depth_opener = token;
            let (code, reason) = if token == TokenKind::Examples {
                (
                    ErrorCode::SourceExamples,
                    "Examples precede the function's body (reference 12.1).",
                )
            } else {
                (
                    ErrorCode::SourcePromise,
                    "Promises precede the function's body (reference 12.2).",
                )
            };
            return self.err(
                code,
                self.current.span,
                "examples and promises come first in a function's body",
                Some(reason),
            );
        }
        self.err(
            ErrorCode::SourceSyntax,
            self.current.span,
            "a value is an expression",
            Some("Statements are separated by logical line breaks (reference 2.4)."),
        )
    }

    fn postfix(&mut self) -> Result<NodeId, ParseAbort> {
        let mut n = self.primary()?;
        while matches!(
            self.current.kind,
            TokenKind::Dot | TokenKind::LParen | TokenKind::LBrack
        ) {
            let start = self.tree.node(n).span;
            let step = if self.take(TokenKind::Dot) {
                let dot = self.previous;
                let tok = self.current.kind;
                let (field_tok, step_kind) = match tok {
                    TokenKind::CapName => (TokenKind::CapName, SyntaxKind::Qualified),
                    TokenKind::Int => (TokenKind::Int, SyntaxKind::Position),
                    _ => (TokenKind::Name, SyntaxKind::FieldAccess),
                };
                let field = self.name(field_tok)?;
                self.node(step_kind, dot, &[Some(field)])?
            } else if self.current.kind == TokenKind::LBrack {
                self.enter()?;
                let saved_code = self.expect_code;
                self.expect_code = Some(ErrorCode::SourceType);
                let args_start = self.current.span;
                self.expect(TokenKind::LBrack)?;
                let args = self.sequence()?;
                loop {
                    let arg = self.parse_type()?;
                    self.append(args, Some(arg))?;
                    if !self.take(TokenKind::Comma) {
                        break;
                    }
                }
                self.expect(TokenKind::RBrack)?;
                self.limit(
                    args,
                    MO_LIMIT_PARAMETERS,
                    ErrorCode::TooManyParameters,
                    "a call has too many type arguments",
                )?;
                let s = self.node(SyntaxKind::TypeArgs, args_start, &[Some(args)])?;
                self.depth -= 1;
                self.expect_code = saved_code;
                s
            } else {
                self.enter()?;
                let saved_code = self.expect_code;
                self.expect_code = Some(ErrorCode::SourceCall);
                let call_start = self.current.span;
                self.expect(TokenKind::LParen)?;
                let args = self.sequence()?;
                if self.current.kind != TokenKind::RParen {
                    loop {
                        let mut arg = self.expr(0)?;
                        if self.tree.node(arg).kind == SyntaxKind::Name
                            && self.take(TokenKind::Colon)
                        {
                            let arg_span = self.tree.node(arg).span;
                            let val = self.expr(0)?;
                            arg =
                                self.node(SyntaxKind::NamedArg, arg_span, &[Some(arg), Some(val)])?;
                        }
                        self.append(args, Some(arg))?;
                        if !self.take(TokenKind::Comma) || self.current.kind == TokenKind::RParen {
                            break;
                        }
                    }
                }
                self.expect(TokenKind::RParen)?;
                let is_ctor = self.is_constructor(n);
                self.limit(
                    args,
                    if is_ctor {
                        MO_LIMIT_FIELDS
                    } else {
                        MO_LIMIT_PARAMETERS
                    },
                    if is_ctor {
                        ErrorCode::TooManyFields
                    } else {
                        ErrorCode::TooManyParameters
                    },
                    if is_ctor {
                        "a record or case has too many fields"
                    } else {
                        "a call has too many arguments"
                    },
                )?;
                let s = self.node(SyntaxKind::Call, call_start, &[Some(args)])?;
                self.depth -= 1;
                self.expect_code = saved_code;
                s
            };

            let n_kind = self.tree.node(n).kind;
            if n_kind != SyntaxKind::Postfix {
                if matches!(
                    n_kind,
                    SyntaxKind::Call
                        | SyntaxKind::TypeArgs
                        | SyntaxKind::Qualified
                        | SyntaxKind::Position
                        | SyntaxKind::FieldAccess
                ) {
                    let base = self.tree.node(n).children[0];
                    let payload = self.tree.node(n).children[1];
                    let postfix_start = self.meta[n.0 as usize].postfix_start;
                    {
                        let n_mut = self.tree.node_mut(n);
                        n_mut.children.clear();
                        n_mut.children.push(payload);
                        n_mut.span.start = postfix_start;
                    }
                    n = self.node(SyntaxKind::Postfix, start, &[base, Some(n)])?;
                } else {
                    let (step_kind, step_tok, step_child0, step_start) = {
                        let s = self.tree.node(step);
                        (s.kind, s.token, s.children[0], s.span.start)
                    };
                    let single = self.node(step_kind, start, &[Some(n), step_child0])?;
                    self.tree.node_mut(single).token = step_tok;
                    self.meta[single.0 as usize].postfix_start = step_start;
                    n = single;
                    continue;
                }
            }
            self.append(n, Some(step))?;
        }
        Ok(n)
    }

    const fn precedence(token: TokenKind) -> u8 {
        match token {
            TokenKind::Implies => 2,
            TokenKind::Or => 3,
            TokenKind::And => 4,
            TokenKind::Eq
            | TokenKind::Ne
            | TokenKind::Lt
            | TokenKind::Le
            | TokenKind::Gt
            | TokenKind::Ge
            | TokenKind::Is => 6,
            TokenKind::DotDot => 7,
            TokenKind::Plus | TokenKind::Minus => 8,
            TokenKind::Star | TokenKind::Slash | TokenKind::Percent => 9,
            TokenKind::Eof
            | TokenKind::Error
            | TokenKind::Nl
            | TokenKind::Name
            | TokenKind::CapName
            | TokenKind::Underscore
            | TokenKind::Int
            | TokenKind::Decimal
            | TokenKind::Char
            | TokenKind::Text
            | TokenKind::TextHead
            | TokenKind::TextMid
            | TokenKind::TextTail
            | TokenKind::Module
            | TokenKind::Expose
            | TokenKind::Use
            | TokenKind::Fn
            | TokenKind::Struct
            | TokenKind::Enum
            | TokenKind::Var
            | TokenKind::If
            | TokenKind::Else
            | TokenKind::Case
            | TokenKind::For
            | TokenKind::In
            | TokenKind::While
            | TokenKind::Break
            | TokenKind::Continue
            | TokenKind::Return
            | TokenKind::Try
            | TokenKind::Not
            | TokenKind::True
            | TokenKind::False
            | TokenKind::Stop
            | TokenKind::Promise
            | TokenKind::Allows
            | TokenKind::Forbids
            | TokenKind::Examples
            | TokenKind::Test
            | TokenKind::Assert
            | TokenKind::Where
            | TokenKind::End
            | TokenKind::Service
            | TokenKind::Api
            | TokenKind::Actor
            | TokenKind::LParen
            | TokenKind::RParen
            | TokenKind::LBrack
            | TokenKind::RBrack
            | TokenKind::LBrace
            | TokenKind::RBrace
            | TokenKind::Comma
            | TokenKind::Colon
            | TokenKind::Dot
            | TokenKind::Bar
            | TokenKind::Assign
            | TokenKind::PlusEq
            | TokenKind::MinusEq => 0,
        }
    }

    fn expr(&mut self, minimum: u8) -> Result<NodeId, ParseAbort> {
        let start = self.current.span;
        let token = self.current.kind;
        if token == TokenKind::If && minimum == 0 {
            return self.if_expr(false);
        }
        let mut last_level = 10u8;
        let mut n = if token == TokenKind::Not && minimum <= 5 {
            let prefix = self.sequence()?;
            while self.current.kind == TokenKind::Not {
                let op = self.leaf()?;
                self.append(prefix, Some(op))?;
            }
            let value = self.expr(5)?;
            let node = if self.tree.node(prefix).children.len() == 1 {
                self.node(SyntaxKind::Not, start, &[Some(value)])?
            } else {
                self.tree.node_mut(prefix).kind = SyntaxKind::Prefix;
                self.append(prefix, Some(value))?;
                prefix
            };
            last_level = 5;
            node
        } else if matches!(token, TokenKind::Minus | TokenKind::Try) {
            let prefix = self.sequence()?;
            while matches!(self.current.kind, TokenKind::Minus | TokenKind::Try) {
                let op = self.leaf()?;
                self.append(prefix, Some(op))?;
            }
            let value = self.postfix()?;
            if self.tree.node(prefix).children.len() == 1 {
                let kind = if token == TokenKind::Minus {
                    SyntaxKind::Negate
                } else {
                    SyntaxKind::Try
                };
                self.node(kind, start, &[Some(value)])?
            } else {
                self.tree.node_mut(prefix).kind = SyntaxKind::Prefix;
                self.append(prefix, Some(value))?;
                prefix
            }
        } else {
            self.postfix()?
        };

        let mut nonchain = 0u8;
        loop {
            let op_tok = self.current.kind;
            let level = Self::precedence(op_tok);
            if level == 0
                || level < minimum
                || level > last_level
                || (nonchain != 0 && level >= nonchain)
            {
                break;
            }
            let op = self.leaf()?;
            let rhs = self.expr(if op_tok == TokenKind::Is {
                10
            } else {
                level + 1
            })?;
            let n_kind = self.tree.node(n).kind;
            if level == last_level && matches!(n_kind, SyntaxKind::Binary | SyntaxKind::BinaryChain)
            {
                if n_kind == SyntaxKind::Binary {
                    let (lhs0, rhs0, tok0) = {
                        let node = self.tree.node(n);
                        (node.children[0], node.children[1], node.token)
                    };
                    let op0 = self.meta[n.0 as usize].operator;
                    let chain = self.node(SyntaxKind::BinaryChain, start, &[lhs0, op0, rhs0])?;
                    self.tree.node_mut(chain).token = tok0;
                    n = chain;
                }
                self.append(n, Some(op))?;
                self.append(n, Some(rhs))?;
            } else {
                let kind = match op_tok {
                    TokenKind::Is => SyntaxKind::Is,
                    TokenKind::DotDot => SyntaxKind::Range,
                    _ => SyntaxKind::Binary,
                };
                let bin = self.node(kind, start, &[Some(n), Some(rhs)])?;
                self.tree.node_mut(bin).token = op_tok;
                self.meta[bin.0 as usize].operator = Some(op);
                n = bin;
            }
            last_level = level;
            if matches!(level, 2 | 6 | 7) {
                nonchain = level;
            }
        }
        Ok(n)
    }

    fn binder(&mut self) -> Result<NodeId, ParseAbort> {
        let start = self.current.span;
        if self.current.kind != TokenKind::LParen {
            let kind = if self.current.kind == TokenKind::Underscore {
                TokenKind::Underscore
            } else {
                TokenKind::Name
            };
            let id = self.name(kind)?;
            return self.node(SyntaxKind::Binder, start, &[Some(id)]);
        }
        self.enter()?;
        self.next_tok();
        let n = self.sequence()?;
        self.tree.node_mut(n).kind = SyntaxKind::Tuple;
        let first = self.binder()?;
        self.append(n, Some(first))?;
        self.expect(TokenKind::Comma)?;
        loop {
            let b = self.binder()?;
            self.append(n, Some(b))?;
            if !self.take(TokenKind::Comma) {
                break;
            }
        }
        self.expect(TokenKind::RParen)?;
        let prev_end = self.previous.end;
        let node_mut = self.tree.node_mut(n);
        node_mut.span.start = start.start;
        node_mut.span.end = prev_end;
        self.depth -= 1;
        Ok(n)
    }

    fn statement(&mut self, newline: bool) -> Result<NodeId, ParseAbort> {
        let start = self.current.span;
        let token = self.current.kind;
        let n = if matches!(token, TokenKind::For | TokenKind::While) {
            self.enter()?;
            let ctrl = if token == TokenKind::For {
                ControlHeaderKind::For
            } else {
                ControlHeaderKind::While
            };
            let (valid, b, condition) = self.control_header(ctrl, false)?;
            let body = self.statements()?;
            self.expect(TokenKind::End)?;
            let loop_node = if token == TokenKind::While {
                self.node(SyntaxKind::While, start, &[condition, Some(body)])?
            } else {
                self.node(SyntaxKind::For, start, &[b, condition, Some(body)])?
            };
            if !valid {
                self.tree.node_mut(loop_node).incomplete = true;
            }
            self.depth -= 1;
            loop_node
        } else if token == TokenKind::Var {
            self.next_tok();
            let v = self.name(TokenKind::Name)?;
            self.expect(TokenKind::Assign)?;
            let value = if self.current.kind == TokenKind::If {
                self.if_expr(true)?
            } else {
                self.expr(0)?
            };
            self.node(SyntaxKind::Var, start, &[Some(v), Some(value)])?
        } else if matches!(
            token,
            TokenKind::Return | TokenKind::Break | TokenKind::Continue
        ) {
            self.next_tok();
            let mut value = if token == TokenKind::Return
                && self.current.kind != TokenKind::Nl
                && self.current.kind != TokenKind::If
            {
                Some(self.expr(0)?)
            } else {
                None
            };
            let mut guard = None;
            if self.current.kind == TokenKind::If {
                let if_start = self.current.span;
                self.next_tok();
                guard = Some(self.expr(1)?);
                if token == TokenKind::Return
                    && value.is_none()
                    && self.current.kind == TokenKind::Colon
                {
                    self.enter()?;
                    value = Some(self.line_if_tail(if_start, guard)?);
                    self.depth -= 1;
                    guard = if self.take(TokenKind::If) {
                        Some(self.expr(1)?)
                    } else {
                        None
                    };
                }
            }
            if token == TokenKind::Return {
                self.node(SyntaxKind::Return, start, &[value, guard])?
            } else {
                let kind = if token == TokenKind::Break {
                    SyntaxKind::Break
                } else {
                    SyntaxKind::Continue
                };
                self.node(kind, start, &[guard])?
            }
        } else if self.take(TokenKind::Assert) {
            let cond = self.expr(0)?;
            self.node(SyntaxKind::Assert, start, &[Some(cond)])?
        } else {
            let lhs = if self.current.kind == TokenKind::If {
                self.if_expr(true)?
            } else {
                self.expr(0)?
            };
            let op_tok = self.current.kind;
            if matches!(
                op_tok,
                TokenKind::Assign | TokenKind::PlusEq | TokenKind::MinusEq
            ) {
                self.next_tok();
                let rhs = if op_tok == TokenKind::Assign && self.current.kind == TokenKind::If {
                    self.if_expr(true)?
                } else {
                    self.expr(0)?
                };
                let kind = match op_tok {
                    TokenKind::Assign => SyntaxKind::Bind,
                    TokenKind::PlusEq => SyntaxKind::AddAssign,
                    _ => SyntaxKind::SubAssign,
                };
                self.node(kind, start, &[Some(lhs), Some(rhs)])?
            } else {
                self.node(SyntaxKind::ExprStmt, start, &[Some(lhs)])?
            }
        };
        if newline {
            self.expect(TokenKind::Nl)?;
        }
        self.tree.node_mut(n).span.end = self.previous.end;
        Ok(n)
    }

    fn statements(&mut self) -> Result<NodeId, ParseAbort> {
        let n = self.sequence()?;
        while !matches!(
            self.current.kind,
            TokenKind::End | TokenKind::Else | TokenKind::Eof
        ) {
            if matches!(
                self.current.kind,
                TokenKind::Struct | TokenKind::Enum | TokenKind::Test
            ) || (self.current.kind == TokenKind::Actor
                && self.peek_next() == TokenKind::CapName)
                || (self.current.kind == TokenKind::Fn && self.peek_next() == TokenKind::Name)
            {
                break;
            }
            match self.read_item(ItemContext::Statement)? {
                Some(item) => self.append(n, Some(item))?,
                None => self.tree.node_mut(n).incomplete = true,
            }
        }
        Ok(n)
    }

    fn clause_word(&mut self, word: &str) -> bool {
        let t = self.current;
        if t.kind != TokenKind::Name {
            return false;
        }
        let slice = &self.lx.source.bytes()[t.span.start as usize..t.span.end as usize];
        if slice != word.as_bytes() {
            return false;
        }
        self.next_tok();
        true
    }

    fn declaration_header_inner(
        &mut self,
        kind: DeclHeaderKind,
    ) -> Result<[Option<NodeId>; 4], ParseAbort> {
        self.expect_code = Some(match kind {
            DeclHeaderKind::Fn => ErrorCode::SourceFunction,
            DeclHeaderKind::Struct => ErrorCode::SourceStruct,
            DeclHeaderKind::Enum => ErrorCode::SourceEnum,
            DeclHeaderKind::Actor => ErrorCode::SourceSyntax,
            DeclHeaderKind::Test => ErrorCode::SourceTest,
            DeclHeaderKind::Promise => ErrorCode::SourcePromise,
            DeclHeaderKind::Examples => ErrorCode::SourceExamples,
        });
        let mut parts = [None; 4];
        self.next_tok();
        match kind {
            DeclHeaderKind::Fn => {
                parts[0] = Some(self.name(TokenKind::Name)?);
                self.enter()?;
                self.expect(TokenKind::LParen)?;
                let params = self.sequence()?;
                if self.current.kind != TokenKind::RParen {
                    loop {
                        if self.current.kind.is_keyword()
                            && matches!(
                                self.peek_next(),
                                TokenKind::Colon | TokenKind::Comma | TokenKind::RParen
                            )
                        {
                            let note_reserved = matches!(
                                self.current.kind,
                                TokenKind::Actor | TokenKind::Service | TokenKind::Api
                            );
                            self.report_reserved_name(ErrorCode::SourceFunction, note_reserved)?;
                            self.tree.node_mut(params).incomplete = true;
                            self.next_tok();
                            if self.take(TokenKind::Colon) {
                                let _ = self.parse_type()?;
                            }
                        } else {
                            let v = self.name(TokenKind::Name)?;
                            let v_span = self.tree.node(v).span;
                            self.expect(TokenKind::Colon)?;
                            let ty = self.parse_type()?;
                            let param =
                                self.node(SyntaxKind::Param, v_span, &[Some(v), Some(ty)])?;
                            self.append(params, Some(param))?;
                        }
                        if !self.take(TokenKind::Comma) || self.current.kind == TokenKind::RParen {
                            break;
                        }
                    }
                }
                self.expect(TokenKind::RParen)?;
                self.depth -= 1;
                parts[1] = Some(params);
                parts[2] = if self.take(TokenKind::Colon) {
                    Some(self.parse_type()?)
                } else {
                    None
                };
                if self.take(TokenKind::Where) {
                    let where_span = self.previous;
                    let wh = self.sequence()?;
                    self.tree.node_mut(wh).kind = SyntaxKind::Where;
                    loop {
                        let v = self.name(TokenKind::CapName)?;
                        let v_span = self.tree.node(v).span;
                        self.expect(TokenKind::Colon)?;
                        let ab = self.name(TokenKind::CapName)?;
                        let pair = self.node(SyntaxKind::Sequence, v_span, &[Some(v), Some(ab)])?;
                        self.append(wh, Some(pair))?;
                        if !self.take(TokenKind::Comma) {
                            break;
                        }
                    }
                    self.tree.node_mut(wh).span.start = where_span.start;
                    parts[3] = Some(wh);
                }
            }
            DeclHeaderKind::Struct | DeclHeaderKind::Enum => {
                parts[0] = Some(self.name(TokenKind::CapName)?);
                parts[1] = self.type_parameters()?;
            }
            DeclHeaderKind::Actor => {
                parts[0] = Some(self.name(TokenKind::CapName)?);
            }
            DeclHeaderKind::Test => {
                parts[0] = Some(self.name(TokenKind::Text)?);
            }
            DeclHeaderKind::Promise => {
                if matches!(self.current.kind, TokenKind::Text | TokenKind::TextHead) {
                    parts[0] = Some(self.name(TokenKind::Text)?);
                }
                if self.clause_word("on") {
                    parts[1] = Some(self.name(TokenKind::Name)?);
                }
            }
            DeclHeaderKind::Examples => {}
        }
        self.expect(TokenKind::Nl)?;
        Ok(parts)
    }

    fn declaration_header(
        &mut self,
        kind: DeclHeaderKind,
    ) -> Result<(bool, [Option<NodeId>; 4]), ParseAbort> {
        let saved_depth = self.depth;
        let first_line = self.current.line;
        let saved_code = self.expect_code;
        match self.declaration_header_inner(kind) {
            Ok(parts) => {
                self.expect_code = saved_code;
                Ok((true, parts))
            }
            Err(ParseAbort) => {
                self.depth = saved_depth;
                self.expect_code = saved_code;
                if self.lx.fatal {
                    return Err(ParseAbort);
                }
                self.recover(first_line);
                if self.lx.fatal {
                    return Err(ParseAbort);
                }
                Ok((false, [None; 4]))
            }
        }
    }

    fn anchor(&mut self) -> Result<NodeId, ParseAbort> {
        let token = self.current.kind;
        let start = self.current.span;
        self.next_tok();
        let values = self.sequence()?;
        loop {
            let v = self.name(TokenKind::Name)?;
            let v_span = self.tree.node(v).span;
            self.expect(TokenKind::Colon)?;
            let val = self.expr(0)?;
            let arg = self.node(SyntaxKind::NamedArg, v_span, &[Some(v), Some(val)])?;
            self.append(values, Some(arg))?;
            if !self.take(TokenKind::Comma) {
                break;
            }
        }
        self.expect(TokenKind::Nl)?;
        let n = self.node(SyntaxKind::Anchor, start, &[Some(values)])?;
        self.tree.node_mut(n).token = token;
        Ok(n)
    }

    fn specification_item(&mut self) -> Result<NodeId, ParseAbort> {
        self.enter()?;
        let kind_tok = self.current.kind;
        let promise = kind_tok == TokenKind::Promise;
        let start = self.current.span;
        let decl_kind = if promise {
            DeclHeaderKind::Promise
        } else {
            DeclHeaderKind::Examples
        };
        let (valid, parts) = self.declaration_header(decl_kind)?;
        let lines = self.sequence()?;
        while !matches!(
            self.current.kind,
            TokenKind::End | TokenKind::Allows | TokenKind::Forbids | TokenKind::Eof
        ) {
            match self.read_item(ItemContext::Expr)? {
                Some(line) => self.append(lines, Some(line))?,
                None => self.tree.node_mut(lines).incomplete = true,
            }
        }
        let anchors = self.sequence()?;
        if promise {
            while matches!(self.current.kind, TokenKind::Allows | TokenKind::Forbids) {
                match self.read_item(ItemContext::Anchor)? {
                    Some(a) => self.append(anchors, Some(a))?,
                    None => self.tree.node_mut(anchors).incomplete = true,
                }
            }
        }
        if valid || self.current.kind != TokenKind::Eof {
            self.expect(TokenKind::End)?;
            self.expect(TokenKind::Nl)?;
        } else {
            self.previous = self.current.span;
        }
        let n = if promise {
            self.node(
                SyntaxKind::Promise,
                start,
                &[parts[0], Some(lines), Some(anchors), parts[1]],
            )?
        } else {
            self.node(SyntaxKind::Examples, start, &[Some(lines)])?
        };
        if !valid {
            self.tree.node_mut(n).incomplete = true;
        }
        self.depth -= 1;
        Ok(n)
    }

    fn specification(&mut self) -> Result<NodeId, ParseAbort> {
        let spec = self.sequence()?;
        while matches!(self.current.kind, TokenKind::Promise | TokenKind::Examples) {
            match self.read_item(ItemContext::Spec)? {
                Some(n) => self.append(spec, Some(n))?,
                None => self.tree.node_mut(spec).incomplete = true,
            }
        }
        Ok(spec)
    }

    fn member(&mut self, record: bool) -> Result<Option<NodeId>, ParseAbort> {
        if record {
            if self.current.kind.is_keyword()
                && self.current.kind != TokenKind::End
                && matches!(self.peek_next(), TokenKind::Colon | TokenKind::Nl)
            {
                self.report_reserved_name(ErrorCode::SourceStruct, true)?;
                self.next_tok();
                if self.take(TokenKind::Colon) {
                    let _ = self.parse_type()?;
                }
                self.expect(TokenKind::Nl)?;
                return Ok(None);
            }
            let field = self.name(TokenKind::Name)?;
            let field_span = self.tree.node(field).span;
            self.expect(TokenKind::Colon)?;
            let ty = self.parse_type()?;
            let n = self.node(SyntaxKind::Field, field_span, &[Some(field), Some(ty)])?;
            self.expect(TokenKind::Nl)?;
            return Ok(Some(n));
        }
        let id = self.name(TokenKind::CapName)?;
        let id_span = self.tree.node(id).span;
        let mut fields = None;
        if self.current.kind == TokenKind::LParen {
            self.enter()?;
            self.next_tok();
            if self.current.kind == TokenKind::Name
                || (self.current.kind.is_keyword() && self.peek_next() == TokenKind::Colon)
            {
                let seq = self.sequence()?;
                loop {
                    if self.current.kind.is_keyword()
                        && matches!(
                            self.peek_next(),
                            TokenKind::Colon | TokenKind::Comma | TokenKind::RParen
                        )
                    {
                        self.report_reserved_name(ErrorCode::SourceEnum, true)?;
                        self.tree.node_mut(seq).incomplete = true;
                        self.next_tok();
                        if self.take(TokenKind::Colon) {
                            let _ = self.parse_type()?;
                        }
                    } else {
                        let field = self.name(TokenKind::Name)?;
                        let field_span = self.tree.node(field).span;
                        self.expect(TokenKind::Colon)?;
                        let ty = self.parse_type()?;
                        let f =
                            self.node(SyntaxKind::Field, field_span, &[Some(field), Some(ty)])?;
                        self.append(seq, Some(f))?;
                    }
                    if !self.take(TokenKind::Comma) || self.current.kind == TokenKind::RParen {
                        break;
                    }
                }
                self.limit(
                    seq,
                    MO_LIMIT_FIELDS,
                    ErrorCode::TooManyFields,
                    "a case has too many fields",
                )?;
                fields = Some(seq);
            } else {
                fields = Some(self.parse_type()?);
            }
            self.expect(TokenKind::RParen)?;
            self.depth -= 1;
        }
        let n = self.node(SyntaxKind::CaseDecl, id_span, &[Some(id), fields])?;
        self.expect(TokenKind::Nl)?;
        Ok(Some(n))
    }

    fn service_clause(&mut self) -> Result<NodeId, ParseAbort> {
        if self.current.kind == TokenKind::Promise {
            return self.specification_item();
        }
        let start = self.current.span;
        let n = if self.clause_word("state") {
            let annotation = self.parse_type()?;
            self.expect(TokenKind::Assign)?;
            let init = self.expr(0)?;
            self.node(
                SyntaxKind::ServiceState,
                start,
                &[Some(annotation), Some(init)],
            )?
        } else if self.clause_word("tools") {
            let names = self.sequence()?;
            loop {
                let tool = self.name(TokenKind::Name)?;
                self.append(names, Some(tool))?;
                if !self.take(TokenKind::Comma) {
                    break;
                }
            }
            self.node(SyntaxKind::ServiceTools, start, &[Some(names)])?
        } else if self.clause_word("every") {
            let interval = self.expr(0)?;
            let handler = self.name(TokenKind::Name)?;
            self.node(
                SyntaxKind::ServiceEvery,
                start,
                &[Some(interval), Some(handler)],
            )?
        } else if self.clause_word("token") {
            if !self.clause_word("from") || !self.clause_word("env") {
                return self.err(
                    ErrorCode::SourceSyntax,
                    self.current.span,
                    "a token clause is token from env followed by plain text",
                    Some("Write token from env followed by a noninterpolated text literal."),
                );
            }
            let env_name = self.name(TokenKind::Text)?;
            self.node(SyntaxKind::ServiceToken, start, &[Some(env_name)])?
        } else {
            return self.err(
                ErrorCode::SourceSyntax,
                start,
                "a service clause starts with state, tools, every, token or promise",
                Some(
                    "Put each service clause on its own logical line, then close the service with end.",
                ),
            );
        };
        self.expect(TokenKind::Nl)?;
        Ok(n)
    }

    fn actor_clause(&mut self) -> Result<NodeId, ParseAbort> {
        if self.current.kind == TokenKind::Promise {
            return self.specification_item();
        }
        let start = self.current.span;
        let n = if self.clause_word("state") {
            let annotation = self.parse_type()?;
            self.expect(TokenKind::Assign)?;
            let init = self.expr(0)?;
            self.node(
                SyntaxKind::ServiceState,
                start,
                &[Some(annotation), Some(init)],
            )?
        } else if self.clause_word("calls") {
            let names = self.sequence()?;
            loop {
                let call = self.name(TokenKind::Name)?;
                self.append(names, Some(call))?;
                if !self.take(TokenKind::Comma) {
                    break;
                }
            }
            self.node(SyntaxKind::ActorCalls, start, &[Some(names)])?
        } else if self.clause_word("every") {
            let interval = self.expr(0)?;
            let handler = self.name(TokenKind::Name)?;
            self.node(
                SyntaxKind::ServiceEvery,
                start,
                &[Some(interval), Some(handler)],
            )?
        } else if self.clause_word("token") {
            return self.err(
                ErrorCode::ActorBlock,
                start,
                "an actor has no token clause",
                Some(
                    "Remove the token clause; actors run in memory inside the program (reference 11.7).",
                ),
            );
        } else {
            return self.err(
                ErrorCode::SourceSyntax,
                start,
                "an actor clause starts with state, calls, every or promise",
                Some(
                    "Put each actor clause on its own logical line, then close the actor with end.",
                ),
            );
        };
        self.expect(TokenKind::Nl)?;
        Ok(n)
    }

    fn is_api_method_word(&self) -> bool {
        const METHODS: &[&[u8]] = &[b"get", b"post", b"put", b"patch", b"delete"];
        let t = self.current;
        if t.kind != TokenKind::Name {
            return false;
        }
        let slice = &self.lx.source.bytes()[t.span.start as usize..t.span.end as usize];
        METHODS.contains(&slice)
    }

    fn api_handler_ref(&mut self) -> Result<NodeId, ParseAbort> {
        if self.current.kind == TokenKind::CapName {
            let module = self.name(TokenKind::CapName)?;
            let mod_span = self.tree.node(module).span;
            self.expect(TokenKind::Dot)?;
            let handler = self.name(TokenKind::Name)?;
            return self.node(
                SyntaxKind::Qualified,
                mod_span,
                &[Some(module), Some(handler)],
            );
        }
        self.name(TokenKind::Name)
    }

    fn api_scope_block(&mut self, start: Span) -> Result<NodeId, ParseAbort> {
        self.enter()?;
        let prefix = self.name(TokenKind::Text)?;
        self.expect(TokenKind::Nl)?;
        let items = self.sequence()?;
        while self.current.kind != TokenKind::End && self.current.kind != TokenKind::Eof {
            match self.read_item(ItemContext::ApiScope)? {
                Some(child) => self.append(items, Some(child))?,
                None => self.tree.node_mut(items).incomplete = true,
            }
        }
        self.expect(TokenKind::End)?;
        self.depth -= 1;
        self.node(SyntaxKind::ApiScope, start, &[Some(prefix), Some(items)])
    }

    fn api_clause(&mut self, in_scope: bool) -> Result<NodeId, ParseAbort> {
        let start = self.current.span;
        if in_scope {
            let n = if self.is_api_method_word() {
                let method = self.leaf()?;
                let route_path = self.name(TokenKind::Text)?;
                let handler = self.api_handler_ref()?;
                self.node(
                    SyntaxKind::ApiRoute,
                    start,
                    &[Some(method), Some(route_path), Some(handler)],
                )?
            } else if self.clause_word("scope") {
                self.api_scope_block(start)?
            } else {
                return self.err(
                    ErrorCode::SourceSyntax,
                    start,
                    "an api scope clause starts with get, post, put, patch, delete or scope",
                    Some("Put routes or nested scope blocks inside scope ... end."),
                );
            };
            self.expect(TokenKind::Nl)?;
            return Ok(n);
        }

        let n = if self.clause_word("port") {
            let port = self.name(TokenKind::Int)?;
            self.node(SyntaxKind::ApiPort, start, &[Some(port)])?
        } else if self.clause_word("cors") {
            let origin = self.name(TokenKind::Text)?;
            self.node(SyntaxKind::ApiCors, start, &[Some(origin)])?
        } else if self.clause_word("sqlite") {
            let binding = self.name(TokenKind::Name)?;
            if !self.clause_word("from") {
                return self.err(
                    ErrorCode::SourceSyntax,
                    self.current.span,
                    "a sqlite clause is sqlite <name> from followed by plain text",
                    Some("Write sqlite db from \"app.db\" with a noninterpolated database path."),
                );
            }
            let db_path = self.name(TokenKind::Text)?;
            self.node(
                SyntaxKind::ApiSqlite,
                start,
                &[Some(binding), Some(db_path)],
            )?
        } else if self.take(TokenKind::Actor) {
            let binding = self.name(TokenKind::Name)?;
            self.expect(TokenKind::Colon)?;
            let ty = self.parse_type()?;
            self.node(SyntaxKind::ApiActor, start, &[Some(binding), Some(ty)])?
        } else if self.clause_word("auth") {
            let principal = self.parse_type()?;
            if !self.clause_word("via") {
                return self.err(
                    ErrorCode::SourceSyntax,
                    self.current.span,
                    "an auth clause is auth <Type> via <function>",
                    Some("Write auth CurrentUser via authenticate."),
                );
            }
            let handler = self.api_handler_ref()?;
            self.node(
                SyntaxKind::ApiAuth,
                start,
                &[Some(principal), Some(handler)],
            )?
        } else if self.clause_word("static") {
            let prefix = self.name(TokenKind::Text)?;
            if !self.clause_word("from") {
                return self.err(
                    ErrorCode::SourceSyntax,
                    self.current.span,
                    "a static clause is static \"/prefix\" from \"folder\"",
                    Some(
                        "Write static \"/assets\" from \"public/assets\" with plain text strings.",
                    ),
                );
            }
            let folder = self.name(TokenKind::Text)?;
            self.node(SyntaxKind::ApiStatic, start, &[Some(prefix), Some(folder)])?
        } else if self.clause_word("job") {
            let job_name = self.name(TokenKind::Text)?;
            let handler = self.api_handler_ref()?;
            let interval = if self.clause_word("every") {
                Some(self.expr(0)?)
            } else {
                None
            };
            self.node(
                SyntaxKind::ApiJob,
                start,
                &[Some(job_name), Some(handler), interval],
            )?
        } else if self.is_api_method_word() {
            let method = self.leaf()?;
            let route_path = self.name(TokenKind::Text)?;
            let handler = self.api_handler_ref()?;
            self.node(
                SyntaxKind::ApiRoute,
                start,
                &[Some(method), Some(route_path), Some(handler)],
            )?
        } else if self.clause_word("scope") {
            self.api_scope_block(start)?
        } else if self.clause_word("status") {
            let mut variant = self.name(TokenKind::CapName)?;
            if self.take(TokenKind::Dot) {
                let v_span = self.tree.node(variant).span;
                let right = self.name(TokenKind::CapName)?;
                variant =
                    self.node(SyntaxKind::Qualified, v_span, &[Some(variant), Some(right)])?;
            }
            let mut wildcards = None;
            if self.current.kind == TokenKind::LParen {
                self.enter()?;
                self.next_tok();
                let seq = self.sequence()?;
                loop {
                    let u = self.name(TokenKind::Underscore)?;
                    self.append(seq, Some(u))?;
                    if !self.take(TokenKind::Comma) {
                        break;
                    }
                }
                self.expect(TokenKind::RParen)?;
                self.limit(
                    seq,
                    MO_LIMIT_FIELDS,
                    ErrorCode::TooManyFields,
                    "a status pattern has too many fields",
                )?;
                self.depth -= 1;
                wildcards = Some(seq);
            }
            self.expect(TokenKind::Assign)?;
            let status = self.name(TokenKind::Int)?;
            self.node(
                SyntaxKind::ApiStatus,
                start,
                &[Some(variant), wildcards, Some(status)],
            )?
        } else {
            return self.err(
                ErrorCode::SourceSyntax,
                start,
                "an api clause starts with port, cors, sqlite, actor, auth, static, job, scope, get, post, put, patch, delete or status",
                Some("Put each api clause on its own logical line, then close the api block with end."),
            );
        };
        self.expect(TokenKind::Nl)?;
        Ok(n)
    }

    fn declaration(&mut self, fragment: bool) -> Result<NodeId, ParseAbort> {
        let start = self.current.span;
        let token = self.current.kind;
        if fragment && token == TokenKind::Fn && self.peek_next() == TokenKind::LParen {
            return self.statement(true);
        }
        let nested = matches!(
            token,
            TokenKind::Fn
                | TokenKind::Struct
                | TokenKind::Enum
                | TokenKind::Test
                | TokenKind::Service
                | TokenKind::Actor
                | TokenKind::Api
        );
        if nested {
            self.enter()?;
        }
        let mut valid = true;
        let n = if matches!(token, TokenKind::Service | TokenKind::Api) {
            let (item_ctx, block_kind) = if token == TokenKind::Service {
                (ItemContext::Service, SyntaxKind::Service)
            } else {
                (ItemContext::Api, SyntaxKind::Api)
            };
            self.next_tok();
            self.expect(TokenKind::Nl)?;
            let clauses = self.sequence()?;
            while self.current.kind != TokenKind::End && self.current.kind != TokenKind::Eof {
                match self.read_item(item_ctx)? {
                    Some(clause) => self.append(clauses, Some(clause))?,
                    None => self.tree.node_mut(clauses).incomplete = true,
                }
            }
            self.expect(TokenKind::End)?;
            self.expect(TokenKind::Nl)?;
            self.node(block_kind, start, &[Some(clauses)])?
        } else if token == TokenKind::Actor {
            let (ok, parts) = self.declaration_header(DeclHeaderKind::Actor)?;
            valid = ok;
            let clauses = self.sequence()?;
            while self.current.kind != TokenKind::End && self.current.kind != TokenKind::Eof {
                match self.read_item(ItemContext::Actor)? {
                    Some(clause) => self.append(clauses, Some(clause))?,
                    None => self.tree.node_mut(clauses).incomplete = true,
                }
            }
            if valid || self.current.kind != TokenKind::Eof {
                self.expect(TokenKind::End)?;
                self.expect(TokenKind::Nl)?;
            } else {
                self.previous = self.current.span;
            }
            self.node(SyntaxKind::Actor, start, &[parts[0], Some(clauses)])?
        } else if matches!(token, TokenKind::Struct | TokenKind::Enum) {
            let decl_kind = if token == TokenKind::Struct {
                DeclHeaderKind::Struct
            } else {
                DeclHeaderKind::Enum
            };
            let (ok, parts) = self.declaration_header(decl_kind)?;
            valid = ok;
            let members = self.sequence()?;
            let mem_ctx = if token == TokenKind::Struct {
                ItemContext::Field
            } else {
                ItemContext::CaseDecl
            };
            while self.current.kind != TokenKind::End && self.current.kind != TokenKind::Eof {
                match self.read_item(mem_ctx)? {
                    Some(m) => self.append(members, Some(m))?,
                    None => self.tree.node_mut(members).incomplete = true,
                }
            }
            if valid || self.current.kind != TokenKind::Eof {
                self.expect(TokenKind::End)?;
                self.expect(TokenKind::Nl)?;
            } else {
                self.previous = self.current.span;
            }
            self.limit(
                members,
                if token == TokenKind::Struct {
                    MO_LIMIT_FIELDS
                } else {
                    MO_LIMIT_CHOICE_CASES
                },
                if token == TokenKind::Struct {
                    ErrorCode::TooManyFields
                } else {
                    ErrorCode::TooManyCases
                },
                if token == TokenKind::Struct {
                    "a record has too many fields"
                } else {
                    "a choice type has too many cases"
                },
            )?;
            let kind = if token == TokenKind::Struct {
                SyntaxKind::Struct
            } else {
                SyntaxKind::Enum
            };
            self.node(kind, start, &[parts[0], parts[1], Some(members)])?
        } else if token == TokenKind::Fn {
            let (ok, parts) = self.declaration_header(DeclHeaderKind::Fn)?;
            valid = ok;
            let spec = self.specification()?;
            let body = self.statements()?;
            if valid || self.current.kind != TokenKind::Eof {
                self.expect(TokenKind::End)?;
                self.expect(TokenKind::Nl)?;
            } else {
                self.previous = self.current.span;
            }
            if let Some(params) = parts[1] {
                self.limit(
                    params,
                    MO_LIMIT_PARAMETERS,
                    ErrorCode::TooManyParameters,
                    "a function has too many parameters",
                )?;
            }
            self.node(
                SyntaxKind::Fn,
                start,
                &[
                    parts[0],
                    parts[1],
                    parts[2],
                    parts[3],
                    Some(spec),
                    Some(body),
                ],
            )?
        } else if token == TokenKind::Test {
            let (ok, parts) = self.declaration_header(DeclHeaderKind::Test)?;
            valid = ok;
            let body = self.statements()?;
            if valid || self.current.kind != TokenKind::Eof {
                self.expect(TokenKind::End)?;
                self.expect(TokenKind::Nl)?;
            } else {
                self.previous = self.current.span;
            }
            self.node(SyntaxKind::Test, start, &[parts[0], Some(body)])?
        } else if !fragment {
            self.expect_code = Some(ErrorCode::SourceBinding);
            let id = self.name(TokenKind::Name)?;
            self.expect(TokenKind::Assign)?;
            let value = self.expr(0)?;
            self.expect(TokenKind::Nl)?;
            self.node(SyntaxKind::Const, start, &[Some(id), Some(value)])?
        } else {
            self.statement(true)?
        };
        if !valid {
            self.tree.node_mut(n).incomplete = true;
        }
        if nested {
            self.depth -= 1;
        }
        Ok(n)
    }

    fn is_constructor(&self, mut nid: NodeId) -> bool {
        if self.tree.node(nid).kind == SyntaxKind::Postfix {
            let Some(last) = self.tree.node(nid).children.last().copied().flatten() else {
                return false;
            };
            nid = last;
        }
        let n = self.tree.node(nid);
        n.kind == SyntaxKind::CapName
            || (n.kind == SyntaxKind::Qualified
                && n.children
                    .last()
                    .copied()
                    .flatten()
                    .is_some_and(|c| self.tree.node(c).kind == SyntaxKind::CapName))
    }

    fn is_place(&self, nid: NodeId) -> bool {
        let n = self.tree.node(nid);
        if n.kind == SyntaxKind::Postfix {
            for &step in n.children.iter().skip(1) {
                if !step.is_some_and(|sid| self.tree.node(sid).kind == SyntaxKind::FieldAccess) {
                    return false;
                }
            }
            return n
                .children
                .first()
                .copied()
                .flatten()
                .is_some_and(|base| self.is_place(base));
        }
        n.kind == SyntaxKind::Name
            || (n.kind == SyntaxKind::FieldAccess
                && n.children
                    .first()
                    .copied()
                    .flatten()
                    .is_some_and(|base| self.is_place(base)))
    }

    fn is_tuple_names(&self, nid: NodeId) -> bool {
        let n = self.tree.node(nid);
        if n.kind != SyntaxKind::Tuple {
            return false;
        }
        for &child in &n.children {
            let Some(cid) = child else {
                return false;
            };
            let ck = self.tree.node(cid).kind;
            if ck != SyntaxKind::Name && ck != SyntaxKind::Underscore && !self.is_tuple_names(cid) {
                return false;
            }
        }
        true
    }

    fn validate_call(
        &mut self,
        call_id: NodeId,
        args_id: NodeId,
        construct: bool,
        pattern: bool,
    ) -> Result<(), ParseAbort> {
        let call_span = self.tree.node(call_id).span;
        let arg_count = self.tree.node(args_id).children.len();
        let mut named = 0usize;
        for i in 0..arg_count {
            if let Some(aid) = self.tree.node(args_id).children[i]
                && self.tree.node(aid).kind == SyntaxKind::NamedArg
            {
                named += 1;
            }
        }
        if construct {
            if arg_count == 0 || (named != arg_count && !(named == 0 && arg_count == 1)) {
                return self.err(
                    if pattern {
                        ErrorCode::SourcePattern
                    } else {
                        ErrorCode::SourceConstruction
                    },
                    call_span,
                    "a case carries no data, one unnamed value, or named fields",
                    Some(if pattern {
                        "A case with named fields is matched by naming them (reference 7.3)."
                    } else {
                        "A case is built by naming its fields or from its one unnamed value (reference 4.2)."
                    }),
                );
            }
        } else if pattern {
            return self.err(
                ErrorCode::SourcePattern,
                call_span,
                "a function call can't be matched; a pattern names a case, as in Some(x)",
                Some("A pattern matches the forms listed in reference 7.3."),
            );
        } else if named != 0 {
            return self.err(
                ErrorCode::SourceArguments,
                call_span,
                "arguments are passed by position",
                Some("Only records and cases are built with named fields (reference 5.2)."),
            );
        }
        for i in 0..arg_count {
            if let Some(arg) = self.tree.node(args_id).children[i] {
                let target = if self.tree.node(arg).kind == SyntaxKind::NamedArg {
                    self.tree.node(arg).children.get(1).copied().flatten()
                } else {
                    Some(arg)
                };
                if let Some(tid) = target {
                    self.validate(tid, pattern)?;
                }
            }
        }
        Ok(())
    }

    fn validate(&mut self, nid: NodeId, pattern: bool) -> Result<(), ParseAbort> {
        let mode = if pattern { 2u8 } else { 1u8 };
        if (self.meta[nid.0 as usize].validated & mode) != 0 {
            return Ok(());
        }
        self.meta[nid.0 as usize].validated |= mode;

        let (kind, span, incomplete, count) = {
            let n = self.tree.node(nid);
            (n.kind, n.span, n.incomplete, n.children.len())
        };

        if kind == SyntaxKind::Postfix {
            let last_opt = self.tree.node(nid).children.last().copied().flatten();
            if pattern {
                if self.is_constructor(nid) {
                    return Ok(());
                }
                let Some(last) = last_opt else {
                    return Ok(());
                };
                if self.tree.node(last).kind != SyntaxKind::Call {
                    return self.err(
                        ErrorCode::SourcePattern,
                        span,
                        "this can't be matched: a pattern is a name, _, a literal, a case, a tuple or a list",
                        Some("A pattern matches the forms listed in reference 7.3."),
                    );
                }
                let args_opt = self.tree.node(last).children.first().copied().flatten();
                let prev_opt = self.tree.node(nid).children[count - 2];
                if let (Some(args), Some(prev)) = (args_opt, prev_opt) {
                    let is_ctor = self.is_constructor(prev);
                    self.validate_call(nid, args, is_ctor, true)?;
                }
            } else {
                if let Some(base) = self.tree.node(nid).children[0] {
                    self.validate(base, false)?;
                }
                for i in 1..count {
                    let Some(step) = self.tree.node(nid).children[i] else {
                        continue;
                    };
                    let step_kind = self.tree.node(step).kind;
                    if step_kind == SyntaxKind::Call
                        && let Some(args) = self.tree.node(step).children.first().copied().flatten()
                        && let Some(prev) = self.tree.node(nid).children[i - 1]
                    {
                        let is_ctor = self.is_constructor(prev);
                        self.validate_call(step, args, is_ctor, false)?;
                    }
                    if step_kind == SyntaxKind::TypeArgs {
                        let ok = if i == 1 {
                            self.tree.node(nid).children[0]
                                .is_some_and(|c| self.tree.node(c).kind == SyntaxKind::Name)
                        } else {
                            self.tree.node(nid).children[i - 1]
                                .is_some_and(|c| self.tree.node(c).kind == SyntaxKind::FieldAccess)
                        };
                        if !ok {
                            let step_span = self.tree.node(step).span;
                            return self.err(
                                ErrorCode::SourceCall,
                                step_span,
                                "type arguments apply only to a named function",
                                Some(
                                    "Type arguments follow a function name in brackets, as in Json.decode[User](text).",
                                ),
                            );
                        }
                    }
                }
            }
            return Ok(());
        }

        if pattern {
            let neg_num = kind == SyntaxKind::Negate
                && self.tree.node(nid).children[0].is_some_and(|c| {
                    matches!(
                        self.tree.node(c).kind,
                        SyntaxKind::Int | SyntaxKind::Decimal
                    )
                });
            if matches!(
                kind,
                SyntaxKind::Name
                    | SyntaxKind::CapName
                    | SyntaxKind::Underscore
                    | SyntaxKind::Int
                    | SyntaxKind::Decimal
                    | SyntaxKind::Text
                    | SyntaxKind::Char
                    | SyntaxKind::True
                    | SyntaxKind::False
            ) || (kind == SyntaxKind::Qualified && self.is_constructor(nid))
                || neg_num
            {
                return Ok(());
            }
            if matches!(
                kind,
                SyntaxKind::Group | SyntaxKind::Tuple | SyntaxKind::List | SyntaxKind::Sequence
            ) {
                for i in 0..count {
                    let Some(cid) = self.tree.node(nid).children[i] else {
                        continue;
                    };
                    if kind == SyntaxKind::List
                        && self.tree.node(cid).kind == SyntaxKind::Rest
                        && i + 1 == count
                    {
                        continue;
                    }
                    self.validate(cid, true)?;
                }
                return Ok(());
            }
            let valid_ctor_call = kind == SyntaxKind::Call
                && self.tree.node(nid).children[0].is_some_and(|c| self.is_constructor(c));
            if !valid_ctor_call {
                return self.err(
                    ErrorCode::SourcePattern,
                    span,
                    "a pattern is a name, _, a literal, a case, a tuple or a list",
                    Some("A pattern matches the forms listed in reference 7.3."),
                );
            }
        }

        if kind == SyntaxKind::TypeArgs {
            let recv_ok = self.tree.node(nid).children[0]
                .is_some_and(|c| self.tree.node(c).kind == SyntaxKind::Name);
            if !recv_ok {
                return self.err(
                    ErrorCode::SourceCall,
                    span,
                    "type arguments apply only to a named function",
                    Some(
                        "Type arguments follow a function name in brackets, as in Json.decode[User](text).",
                    ),
                );
            }
            if let Some(c0) = self.tree.node(nid).children[0] {
                self.validate(c0, false)?;
            }
            return Ok(());
        }

        if kind == SyntaxKind::Call {
            let c0 = self.tree.node(nid).children[0];
            let c1 = self.tree.node(nid).children[1];
            if let (Some(callee), Some(args)) = (c0, c1) {
                let is_ctor = self.is_constructor(callee);
                self.validate_call(nid, args, is_ctor, pattern)?;
                if !pattern {
                    self.validate(callee, false)?;
                }
            }
            return Ok(());
        }

        if matches!(kind, SyntaxKind::Underscore | SyntaxKind::Rest) {
            return self.err(
                ErrorCode::SourcePattern,
                span,
                "_ means ignored; ..rest ends a list pattern",
                Some(
                    "Patterns use _ to ignore a value, and ..rest to end a list pattern (reference 7.3).",
                ),
            );
        }

        if matches!(
            kind,
            SyntaxKind::Type
                | SyntaxKind::Param
                | SyntaxKind::Field
                | SyntaxKind::Where
                | SyntaxKind::Expose
                | SyntaxKind::ExposeAll
                | SyntaxKind::Use
                | SyntaxKind::Binder
                | SyntaxKind::ApiPort
                | SyntaxKind::ApiCors
                | SyntaxKind::ApiSqlite
                | SyntaxKind::ApiActor
                | SyntaxKind::ApiAuth
                | SyntaxKind::ApiRoute
                | SyntaxKind::ApiStatic
                | SyntaxKind::ApiStatus
        ) {
            return Ok(());
        }

        if matches!(kind, SyntaxKind::Struct | SyntaxKind::Enum) {
            let empty_members = self.tree.node(nid).children[2]
                .is_some_and(|m| self.tree.node(m).children.is_empty());
            if !incomplete && empty_members {
                let (code, msg, reason) = if kind == SyntaxKind::Struct {
                    (
                        ErrorCode::SourceStruct,
                        "a record has named fields, each with a type",
                        "A struct declares a record type: named fields, each with a type (reference 4.1).",
                    )
                } else {
                    (
                        ErrorCode::SourceEnum,
                        "a choice type has cases",
                        "An enum declares a choice type: each value is exactly one of its cases (reference 4.2).",
                    )
                };
                return self.err(code, span, msg, Some(reason));
            }
            return Ok(());
        }

        if kind == SyntaxKind::Promise {
            let empty_lines = self.tree.node(nid).children[1]
                .is_some_and(|l| self.tree.node(l).children.is_empty());
            if !incomplete && empty_lines {
                return self.err(
                    ErrorCode::SourcePromise,
                    span,
                    "a promise is one or more Boolean lines",
                    Some("A promise's body is one or more Boolean lines (reference 12.2)."),
                );
            }
        }

        if kind == SyntaxKind::Examples {
            let empty_lines = self.tree.node(nid).children[0]
                .is_some_and(|l| self.tree.node(l).children.is_empty());
            if !incomplete && empty_lines {
                return self.err(
                    ErrorCode::SourceExamples,
                    span,
                    "an example is a Boolean line",
                    Some("An example is a Boolean line that must be true (reference 12.1)."),
                );
            }
        }

        if matches!(
            kind,
            SyntaxKind::Bind | SyntaxKind::AddAssign | SyntaxKind::SubAssign
        ) {
            let lhs_opt = self.tree.node(nid).children[0];
            let rhs_opt = self.tree.node(nid).children[1];
            if let Some(lhs) = lhs_opt {
                let ok_target = self.is_place(lhs)
                    || (kind == SyntaxKind::Bind
                        && (self.tree.node(lhs).kind == SyntaxKind::Underscore
                            || self.is_tuple_names(lhs)));
                if !ok_target {
                    let lhs_span = self.tree.node(lhs).span;
                    return self.err(
                        ErrorCode::SourceTarget,
                        lhs_span,
                        "assignment changes a var or its fields",
                        Some(
                            "A var declares a name that can be assigned again; assigning to a field builds a new record (reference 6.2).",
                        ),
                    );
                }
            }
            if let Some(rhs) = rhs_opt {
                self.validate(rhs, false)?;
            }
            return Ok(());
        }

        if kind == SyntaxKind::Is {
            let c0 = self.tree.node(nid).children[0];
            let c1 = self.tree.node(nid).children[1];
            if let Some(lhs) = c0 {
                self.validate(lhs, false)?;
            }
            if let Some(pat) = c1 {
                self.validate(pat, true)?;
            }
            return Ok(());
        }

        if kind == SyntaxKind::Case && !incomplete {
            let bad_case = self.tree.node(nid).children[1].is_some_and(|lines| {
                let l = self.tree.node(lines);
                l.children.is_empty()
                    || l.children[0].is_some_and(|c| self.tree.node(c).kind != SyntaxKind::Arm)
            });
            if bad_case {
                return self.err(
                    ErrorCode::SourceCase,
                    span,
                    "an arm is Pattern: statement",
                    Some("Case compares a value with patterns, top to bottom (reference 7.2)."),
                );
            }
        }

        if kind == SyntaxKind::For
            && let Some(c0) = self.tree.node(nid).children[0]
        {
            self.validate(c0, false)?;
        }

        if kind == SyntaxKind::Tuple {
            self.limit(
                nid,
                MO_LIMIT_FIELDS,
                ErrorCode::TooManyFields,
                "a tuple has too many elements",
            )?;
        }

        if matches!(
            kind,
            SyntaxKind::FieldAccess | SyntaxKind::Qualified | SyntaxKind::Position
        ) {
            if let Some(c0) = self.tree.node(nid).children[0] {
                self.validate(c0, false)?;
            }
            return Ok(());
        }

        let mut from = match kind {
            SyntaxKind::Fn => 4usize,
            SyntaxKind::Promise
            | SyntaxKind::Test
            | SyntaxKind::Const
            | SyntaxKind::Var
            | SyntaxKind::For
            | SyntaxKind::Lambda
            | SyntaxKind::NamedArg => 1usize,
            _ => 0usize,
        };
        if kind == SyntaxKind::Arm {
            if let Some(c0) = self.tree.node(nid).children[0] {
                self.validate(c0, true)?;
            }
            from = 1;
        }
        for i in from..count {
            if let Some(cid) = self.tree.node(nid).children[i] {
                self.validate(cid, false)?;
            }
        }
        Ok(())
    }

    fn line_start(&self) -> bool {
        let bytes = self.lx.source.bytes();
        let mut at = self.current.span.start as usize;
        while at > 0 && matches!(bytes[at - 1], b' ' | b'\t') {
            at -= 1;
        }
        at == 0 || bytes[at - 1] == b'\n'
    }

    const fn can_start(token: TokenKind) -> bool {
        matches!(
            token,
            TokenKind::Fn
                | TokenKind::Struct
                | TokenKind::Enum
                | TokenKind::Test
                | TokenKind::Service
                | TokenKind::Actor
                | TokenKind::Api
                | TokenKind::Var
                | TokenKind::If
                | TokenKind::Case
                | TokenKind::For
                | TokenKind::While
                | TokenKind::Return
                | TokenKind::Break
                | TokenKind::Continue
                | TokenKind::Assert
                | TokenKind::Name
                | TokenKind::CapName
                | TokenKind::LParen
                | TokenKind::LBrack
                | TokenKind::LBrace
                | TokenKind::Int
                | TokenKind::Decimal
                | TokenKind::Char
                | TokenKind::Text
                | TokenKind::TextHead
                | TokenKind::True
                | TokenKind::False
                | TokenKind::Minus
                | TokenKind::Try
                | TokenKind::Not
                | TokenKind::Stop
                | TokenKind::Underscore
                | TokenKind::Allows
                | TokenKind::Forbids
                | TokenKind::Promise
                | TokenKind::Examples
        )
    }

    fn recover(&mut self, first_line: u32) {
        self.lx.recovering = true;
        if self.depth_opener != TokenKind::Eof {
            let open = self.depth_opener;
            let close = match open {
                TokenKind::LParen => TokenKind::RParen,
                TokenKind::LBrack => TokenKind::RBrack,
                TokenKind::LBrace => TokenKind::RBrace,
                _ => TokenKind::End,
            };
            let mut level = 1u32;
            let mut closing_line = 0u32;
            let limited = self
                .diags
                .items()
                .last()
                .is_some_and(|d| d.code == ErrorCode::NestedTooDeeply);
            self.depth_opener = TokenKind::Eof;
            self.lx.failed = false;
            self.next_tok();
            let mut previous = open;
            while level != 0 && self.current.kind != TokenKind::Eof && !self.lx.fatal {
                let token = self.current.kind;
                if token == TokenKind::Error {
                    self.lx.resume();
                    self.lookahead = None;
                } else {
                    if close == TokenKind::End {
                        if (token == TokenKind::If && previous != TokenKind::Else)
                            || matches!(
                                token,
                                TokenKind::Fn
                                    | TokenKind::For
                                    | TokenKind::While
                                    | TokenKind::Case
                                    | TokenKind::Service
                                    | TokenKind::Actor
                                    | TokenKind::Api
                            )
                        {
                            level += 1;
                        }
                    } else if token == open {
                        level += 1;
                    }
                    if token == close {
                        level -= 1;
                        closing_line = self.current.line;
                    }
                }
                previous = token;
                self.next_tok();
            }
            if !limited
                && self.current.line == closing_line
                && self.current.kind != TokenKind::Eof
                && self.current.kind != TokenKind::Nl
            {
                self.lx.resume();
                self.lookahead = None;
                self.current = self.lx.next_token(&mut self.tree, self.diags);
            }
        }
        if self.current.kind == TokenKind::Nl {
            self.lx.clear_interpolations();
            self.lx.failed = false;
            self.next_tok();
        } else if self.current.kind != TokenKind::Error
            && self.current.line > first_line
            && self.line_start()
            && self.lx.interpolation_depth() == 0
        {
            self.lx.failed = false;
        } else if self.current.kind != TokenKind::Error
            && self.current.kind != TokenKind::Eof
            && self.current.line > first_line
        {
            // A missing opening-line delimiter may already have consumed the next
            // line's first token. Read that physical line again, not just its tail.
            let line_idx = (self.current.line - 1) as usize;
            self.lx.pos = self.lx.source.line_starts()[line_idx];
            self.lx.line = self.current.line;
            self.lx.column = 1;
            self.lx.clear_interpolations();
            self.lx.last = TokenKind::Eof;
            self.lx.pending = None;
            self.lookahead = None;
            self.lx.failed = false;
            while self
                .lx
                .comments
                .last()
                .is_some_and(|c| c.comment.span.start >= self.lx.pos)
            {
                self.lx.comments.pop();
            }
            self.current = self.lx.next_token(&mut self.tree, self.diags);
        } else {
            self.lookahead = None;
            self.lx.resume();
            self.current = self.lx.next_token(&mut self.tree, self.diags);
        }
        while !self.lx.fatal
            && self.current.kind != TokenKind::Eof
            && self.current.kind != TokenKind::End
            && self.current.kind != TokenKind::Else
            && !Self::can_start(self.current.kind)
        {
            self.lookahead = None;
            self.lx.resume();
            self.current = self.lx.next_token(&mut self.tree, self.diags);
        }
        self.lx.recovering = false;
        self.lx.failed = false;
    }

    fn arm_header(&self, first: Token, end: u32) -> bool {
        if matches!(
            first.kind,
            TokenKind::Var
                | TokenKind::Return
                | TokenKind::Break
                | TokenKind::Continue
                | TokenKind::Assert
                | TokenKind::If
                | TokenKind::For
                | TokenKind::While
        ) {
            return false;
        }
        let bytes = self.lx.source.bytes();
        let start = first.span.start as usize;
        let end = (end as usize).min(bytes.len());
        if start > end {
            return false;
        }
        let mut depth = 0u32;
        let mut quote = 0u8;
        let mut guard = false;
        let mut at = start;
        while at < end {
            let c = bytes[at];
            if quote != 0 {
                if c == b'\\' && at + 1 < end && bytes[at + 1] != b'\n' {
                    at += 1;
                } else if c == quote {
                    quote = 0;
                }
            } else if c == b'#' {
                while at + 1 < end && bytes[at + 1] != b'\n' {
                    at += 1;
                }
            } else if c == b'"' || c == b'\'' {
                quote = c;
            } else if matches!(c, b'(' | b'[' | b'{') {
                depth += 1;
            } else if matches!(c, b')' | b']' | b'}') {
                depth = depth.saturating_sub(1);
            } else if depth == 0 {
                if c == b':' {
                    return true;
                }
                if c == b'i'
                    && at > start
                    && matches!(bytes[at - 1], b' ' | b'\t')
                    && end - at > 2
                    && bytes[at + 1] == b'f'
                    && matches!(bytes[at + 2], b' ' | b'\t')
                {
                    guard = true;
                }
                if c == b'='
                    && !guard
                    && (at + 1 == end || bytes[at + 1] != b'=')
                    && (at == start || !matches!(bytes[at - 1], b'=' | b'!' | b'<' | b'>'))
                {
                    return false;
                }
            }
            at += 1;
        }
        false
    }

    fn read_item_inner(
        &mut self,
        context: ItemContext,
        start: Span,
    ) -> Result<Option<NodeId>, ParseAbort> {
        self.expect_code = match context {
            ItemContext::Field => Some(ErrorCode::SourceStruct),
            ItemContext::CaseDecl => Some(ErrorCode::SourceEnum),
            ItemContext::Expr => Some(ErrorCode::SourceExamples),
            ItemContext::Anchor => Some(ErrorCode::SourcePromise),
            ItemContext::Module
            | ItemContext::Fragment
            | ItemContext::Statement
            | ItemContext::Case
            | ItemContext::Spec
            | ItemContext::Service
            | ItemContext::Actor
            | ItemContext::Api
            | ItemContext::ApiScope => None,
        };
        let n_opt = match context {
            ItemContext::Case => {
                let mut stmt = self.statement(false)?;
                if self.tree.node(stmt).kind == SyntaxKind::ExprStmt
                    && matches!(
                        self.current.kind,
                        TokenKind::Bar | TokenKind::If | TokenKind::Colon
                    )
                {
                    let alternatives = self.sequence()?;
                    let first_alt = self.tree.node(stmt).children[0];
                    self.append(alternatives, first_alt)?;
                    while self.take(TokenKind::Bar) {
                        let alt = self.expr(10)?;
                        self.append(alternatives, Some(alt))?;
                    }
                    let guard = if self.take(TokenKind::If) {
                        Some(self.expr(1)?)
                    } else {
                        None
                    };
                    self.expect(TokenKind::Colon)?;
                    if matches!(self.current.kind, TokenKind::For | TokenKind::While) {
                        return self.err(
                            ErrorCode::SourceCase,
                            self.current.span,
                            "an arm is Pattern: statement",
                            Some("A loop starts on its own line in an arm (reference 7.2)."),
                        );
                    }
                    let body = if self.current.kind == TokenKind::Nl {
                        None
                    } else {
                        Some(self.statement(false)?)
                    };
                    stmt = self.node(SyntaxKind::Arm, start, &[Some(alternatives), guard, body])?;
                }
                self.expect(TokenKind::Nl)?;
                Some(stmt)
            }
            ItemContext::Field | ItemContext::CaseDecl => {
                self.member(context == ItemContext::Field)?
            }
            ItemContext::Service => Some(self.service_clause()?),
            ItemContext::Actor => Some(self.actor_clause()?),
            ItemContext::Api | ItemContext::ApiScope => {
                Some(self.api_clause(context == ItemContext::ApiScope)?)
            }
            ItemContext::Spec => Some(self.specification_item()?),
            ItemContext::Anchor => Some(self.anchor()?),
            ItemContext::Expr => {
                let e = self.expr(0)?;
                self.expect(TokenKind::Nl)?;
                Some(e)
            }
            ItemContext::Statement => Some(self.statement(true)?),
            ItemContext::Module | ItemContext::Fragment => {
                Some(self.declaration(context == ItemContext::Fragment)?)
            }
        };
        let Some(n) = n_opt else {
            return Ok(None);
        };
        self.validate(n, false)?;
        if matches!(context, ItemContext::Module | ItemContext::Fragment) {
            let nk = self.tree.node(n).kind;
            if matches!(
                nk,
                SyntaxKind::Fn
                    | SyntaxKind::Struct
                    | SyntaxKind::Enum
                    | SyntaxKind::Actor
                    | SyntaxKind::Const
            ) {
                self.names += 1;
                if u64::from(self.names) > MO_LIMIT_MODULE_NAMES {
                    let n_span = self.tree.node(n).span;
                    return self.err(
                        ErrorCode::TooManyNames,
                        n_span,
                        "a module declares too many names",
                        None,
                    );
                }
            }
        }
        Ok(Some(n))
    }

    fn read_item(&mut self, context: ItemContext) -> Result<Option<NodeId>, ParseAbort> {
        let saved_depth = self.depth;
        let first_line = self.current.line;
        let first = self.current;
        let start = first.span;
        let saved_code = self.expect_code;
        match self.read_item_inner(context, start) {
            Ok(n) => {
                self.expect_code = saved_code;
                Ok(n)
            }
            Err(ParseAbort) => {
                self.depth = saved_depth;
                self.expect_code = saved_code;
                if self.lx.fatal {
                    return Err(ParseAbort);
                }
                let line = self.current.line;
                let end = if line < self.lx.source.line_count() {
                    self.lx.source.line_starts()[line as usize]
                } else {
                    self.lx.source.len()
                };
                let failed_arm = context == ItemContext::Case && self.arm_header(first, end);
                self.recover(first_line);
                if self.lx.fatal {
                    return Err(ParseAbort);
                }
                if failed_arm {
                    let placeholder = self.node(SyntaxKind::Arm, start, &[None, None, None])?;
                    self.tree.node_mut(placeholder).incomplete = true;
                    return Ok(Some(placeholder));
                }
                Ok(None)
            }
        }
    }

    fn initial(&mut self, fragment: bool) -> Result<Option<(NodeId, Header)>, ParseAbort> {
        self.next_tok();
        if !fragment
            && self.current.kind != TokenKind::Module
            && self.current.kind != TokenKind::Error
        {
            self.lx.error(
                self.diags,
                ErrorCode::SourceHeader,
                self.current.span,
                "a file is one module, named on its first line of code",
                Some("A module starts with its name, then expose and use lines (reference 10.1)."),
            );
            if self.lx.fatal {
                return Err(ParseAbort);
            }
            self.lx.failed = false;
            return Ok(None);
        }
        if !fragment || self.current.kind == TokenKind::Module {
            match self.header() {
                Ok(res) => return Ok(Some(res)),
                Err(ParseAbort) => {
                    if self.lx.fatal {
                        return Err(ParseAbort);
                    }
                    self.recover(1);
                    return Ok(None);
                }
            }
        }
        Ok(None)
    }

    fn parse_run(&mut self, fragment: bool) -> Result<(NodeId, Option<Header>), ParseAbort> {
        if !self.tree.try_charge(MODULE_SYNTAX_ARENA_COST) {
            return self.out_of_memory();
        }
        let module_span = Span::new(self.lx.file, 0, self.lx.source.len());
        let errors_before = self.diags.len();
        let initial_res = self.initial(fragment)?;
        let (h_node, header) = match initial_res {
            Some((hn, h)) => (Some(hn), Some(h)),
            None => (None, None),
        };
        let incomplete_header = errors_before != self.diags.len();
        let items = self.sequence()?;
        while self.current.kind != TokenKind::Eof {
            if self.diags.len() > errors_before
                && matches!(self.current.kind, TokenKind::End | TokenKind::Else)
            {
                self.next_tok();
                if self.current.kind == TokenKind::Nl {
                    self.next_tok();
                }
                self.tree.node_mut(items).incomplete = true;
                continue;
            }
            let ctx = if fragment {
                ItemContext::Fragment
            } else {
                ItemContext::Module
            };
            match self.read_item(ctx)? {
                Some(item) => self.append(items, Some(item))?,
                None => self.tree.node_mut(items).incomplete = true,
            }
        }
        let root = self.node(SyntaxKind::File, module_span, &[h_node, Some(items)])?;
        if incomplete_header {
            self.tree.node_mut(root).incomplete = true;
        }
        Ok((root, header))
    }
}

fn parse_internal(
    sources: &SourceMap,
    file: u32,
    diags: &mut DiagBag,
    fragment: bool,
    options: ParseOptions,
) -> Option<ModuleSyntax> {
    let source = sources.get(file)?;
    let mut parser = Parser::new(source, file, diags, options.arena_limit);
    let (root, header) = parser.parse_run(fragment).ok()?;
    let module_span = Span::new(file, 0, source.len());
    let raw_comments = std::mem::take(&mut parser.lx.comments);
    if !attach_trivia(source, file, &mut parser.tree, root, raw_comments) {
        parser.lx.error(
            parser.diags,
            ErrorCode::SyntaxNoMemory,
            module_span,
            "comment attachment has no room",
            None,
        );
        return None;
    }
    Some(ModuleSyntax {
        span: module_span,
        header,
        tree: parser.tree,
        root,
    })
}

/// Parses a full Mo module file with default options (`src/syntax/parser.c:mo_parse`).
///
/// Syntax errors return a recovered partial tree alongside diagnostics. Returns `None` only
/// when the arena budget is exhausted (`E1054`) or diagnostics can no longer be recorded.
#[must_use]
pub fn parse_module(sources: &SourceMap, file: u32, diags: &mut DiagBag) -> Option<ModuleSyntax> {
    parse_internal(sources, file, diags, false, ParseOptions::default())
}

/// Parses a full Mo module file with explicit [`ParseOptions`].
#[must_use]
pub fn parse_module_with_options(
    sources: &SourceMap,
    file: u32,
    diags: &mut DiagBag,
    options: ParseOptions,
) -> Option<ModuleSyntax> {
    parse_internal(sources, file, diags, false, options)
}

/// Parses a Mo fragment (statements and declarations in any order, optional module header),
/// matching `src/syntax/parser.c:mo_parse_fragment`.
#[must_use]
pub fn parse_fragment(sources: &SourceMap, file: u32, diags: &mut DiagBag) -> Option<ModuleSyntax> {
    parse_internal(sources, file, diags, true, ParseOptions::default())
}

/// Parses a Mo fragment with explicit [`ParseOptions`].
#[must_use]
pub fn parse_fragment_with_options(
    sources: &SourceMap,
    file: u32,
    diags: &mut DiagBag,
    options: ParseOptions,
) -> Option<ModuleSyntax> {
    parse_internal(sources, file, diags, true, options)
}

/// Reads only a module's header (`module`, optional `expose`, `use` lines) without lexing or
/// parsing the module body (`src/syntax/parser.c:mo_read_header`).
#[must_use]
pub fn read_header(sources: &SourceMap, file: u32, diags: &mut DiagBag) -> Option<ParsedHeader> {
    read_header_with_options(sources, file, diags, ParseOptions::default())
}

/// Reads only a module's header with explicit [`ParseOptions`].
#[must_use]
pub fn read_header_with_options(
    sources: &SourceMap,
    file: u32,
    diags: &mut DiagBag,
    options: ParseOptions,
) -> Option<ParsedHeader> {
    let source = sources.get(file)?;
    let mut parser = Parser::new(source, file, diags, options.arena_limit);
    parser.lx.header_only = true;
    parser.next_tok();
    let (_, header) = parser.header().ok()?;
    Some(ParsedHeader {
        tree: parser.tree,
        header,
    })
}
