//! Surface lexer matching `src/syntax/lexer.c` byte-for-byte.

use mo_base::utf8::decode_scalar;
use mo_base::{DiagBag, Diagnostic, Edit, Fix, SourceFile, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_INTERPOLATION_DEPTH, MO_LIMIT_LINE_LENGTH};

use crate::ast::{COMMENT_ARENA_COST, CommentAnchor, NodeId, SyntaxComment, SyntaxTree, TokenKind};

/// One lexed token with half-open byte span and 1-based starting line/column.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Token {
    pub kind: TokenKind,
    pub span: Span,
    pub line: u32,
    pub column: u32,
}

impl Token {
    #[must_use]
    pub const fn empty(file: u32) -> Self {
        Self {
            kind: TokenKind::Eof,
            span: Span::new(file, 0, 0),
            line: 0,
            column: 0,
        }
    }
}

/// Comment recorded during lexing prior to AST attachment (`struct mo_comment_storage`).
#[derive(Debug, Clone)]
pub(crate) struct RawComment {
    pub comment: SyntaxComment,
    pub preceding: Option<Token>,
    pub owner: Option<NodeId>,
}

#[derive(Debug, Clone, Copy)]
struct InterpolationFrame {
    braces: u32,
    triple: bool,
    indent: u32,
}

const LAMBDA_REASON: &str = "An anonymous function is written fn(x) ... end, on one line or several, and its parameter types are inferred (reference 5.6).";

const KEYWORDS: &[(&str, TokenKind)] = &[
    ("module", TokenKind::Module),
    ("expose", TokenKind::Expose),
    ("use", TokenKind::Use),
    ("fn", TokenKind::Fn),
    ("struct", TokenKind::Struct),
    ("enum", TokenKind::Enum),
    ("var", TokenKind::Var),
    ("if", TokenKind::If),
    ("else", TokenKind::Else),
    ("case", TokenKind::Case),
    ("for", TokenKind::For),
    ("in", TokenKind::In),
    ("while", TokenKind::While),
    ("break", TokenKind::Break),
    ("continue", TokenKind::Continue),
    ("return", TokenKind::Return),
    ("try", TokenKind::Try),
    ("and", TokenKind::And),
    ("or", TokenKind::Or),
    ("not", TokenKind::Not),
    ("is", TokenKind::Is),
    ("implies", TokenKind::Implies),
    ("true", TokenKind::True),
    ("false", TokenKind::False),
    ("stop", TokenKind::Stop),
    ("promise", TokenKind::Promise),
    ("allows", TokenKind::Allows),
    ("forbids", TokenKind::Forbids),
    ("examples", TokenKind::Examples),
    ("test", TokenKind::Test),
    ("assert", TokenKind::Assert),
    ("where", TokenKind::Where),
    ("end", TokenKind::End),
    ("service", TokenKind::Service),
    ("api", TokenKind::Api),
    ("actor", TokenKind::Actor),
];

/// Decodes one valid UTF-8 scalar at the start of `bytes`, matching `mo_syntax_decode`.
#[must_use]
pub(crate) fn decode_utf8(bytes: &[u8]) -> Option<(u32, u32)> {
    let (ch, width) = decode_scalar(bytes);
    ch.map(|c| (c as u32, width as u32))
}

const fn is_invisible(c: u32) -> bool {
    (c < 32 && c != 9 && c != 10 && c != 13)
        || c == 127
        || (c >= 128 && c <= 159)
        || c == 0xad
        || c == 0x61c
        || c == 0x180e
        || (c >= 0x200b && c <= 0x200f)
        || (c >= 0x2028 && c <= 0x202e)
        || (c >= 0x2060 && c <= 0x2064)
        || (c >= 0x2066 && c <= 0x206f)
        || c == 0xfeff
        || (c >= 0xfff9 && c <= 0xfffb)
        || c == 0xe0001
        || (c >= 0xe0020 && c <= 0xe007f)
}

const fn is_lower(c: u8) -> bool {
    c.is_ascii_lowercase()
}

const fn is_upper(c: u8) -> bool {
    c.is_ascii_uppercase()
}

const fn is_digit(c: u8) -> bool {
    c.is_ascii_digit()
}

const fn is_alnum(c: u8) -> bool {
    c.is_ascii_alphanumeric()
}

const fn continues(token: TokenKind, before: bool) -> bool {
    match token {
        TokenKind::Dot
        | TokenKind::DotDot
        | TokenKind::Eq
        | TokenKind::Ne
        | TokenKind::Lt
        | TokenKind::Le
        | TokenKind::Gt
        | TokenKind::Ge
        | TokenKind::Plus
        | TokenKind::Star
        | TokenKind::Slash
        | TokenKind::Percent
        | TokenKind::Bar
        | TokenKind::And
        | TokenKind::Or
        | TokenKind::Implies
        | TokenKind::Is
        | TokenKind::Where
        | TokenKind::TextMid => true,
        TokenKind::RParen | TokenKind::RBrack | TokenKind::RBrace | TokenKind::TextTail => before,
        TokenKind::LParen
        | TokenKind::LBrack
        | TokenKind::LBrace
        | TokenKind::TextHead
        | TokenKind::Comma
        | TokenKind::Assign
        | TokenKind::PlusEq
        | TokenKind::MinusEq
        | TokenKind::Minus
        | TokenKind::Not
        | TokenKind::Try => !before,
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
        | TokenKind::True
        | TokenKind::False
        | TokenKind::Stop
        | TokenKind::Promise
        | TokenKind::Allows
        | TokenKind::Forbids
        | TokenKind::Examples
        | TokenKind::Test
        | TokenKind::Assert
        | TokenKind::End
        | TokenKind::Service
        | TokenKind::Api
        | TokenKind::Actor
        | TokenKind::Colon => false,
    }
}

fn starts_type_after_bracket(bytes: &[u8], mut pos: usize) -> bool {
    loop {
        while pos < bytes.len() && matches!(bytes[pos], b' ' | 9 | 10 | 13) {
            pos += 1;
        }
        if pos < bytes.len() && bytes[pos] == b'#' {
            while pos < bytes.len() && bytes[pos] != 10 && bytes[pos] != 13 {
                pos += 1;
            }
            continue;
        }
        break;
    }
    if pos >= bytes.len() {
        return false;
    }
    if is_upper(bytes[pos]) || bytes[pos] == b'(' {
        return true;
    }
    if pos + 2 <= bytes.len() && bytes[pos] == b'f' && bytes[pos + 1] == b'n' {
        let after = bytes.get(pos + 2).copied();
        return !after.is_some_and(|b| is_alnum(b) || b == b'_' || b == b'?');
    }
    false
}

/// Lexer state (`struct mo_lex`).
pub(crate) struct Lexer<'a> {
    pub source: &'a SourceFile,
    pub file: u32,
    pub pos: u32,
    pub line: u32,
    pub column: u32,
    pub last: TokenKind,
    pub lambda_parameters: bool,
    pub lambda_close: bool,
    pub line_opens: u32,
    pub line_assign: bool,
    pub preceding: Token,
    pub preceding_line: u32,
    pub ignore_comments: bool,
    pub pending: Option<Token>,
    interpolations: Vec<InterpolationFrame>,
    pub comments: Vec<RawComment>,
    triple_cache: Option<(u32, u32)>,
    pub recovery_end: u32,
    limits_reported: u8,
    pub failed: bool,
    pub fatal: bool,
    pub header_only: bool,
    pub recovering: bool,
}

impl<'a> Lexer<'a> {
    #[must_use]
    pub fn new(source: &'a SourceFile, file: u32) -> Self {
        Self {
            source,
            file,
            pos: 0,
            line: 1,
            column: 1,
            last: TokenKind::Eof,
            lambda_parameters: false,
            lambda_close: false,
            line_opens: 0,
            line_assign: false,
            preceding: Token::empty(file),
            preceding_line: 0,
            ignore_comments: false,
            pending: None,
            interpolations: Vec::new(),
            comments: Vec::new(),
            triple_cache: None,
            recovery_end: 0,
            limits_reported: 0,
            failed: false,
            fatal: false,
            header_only: false,
            recovering: false,
        }
    }

    #[must_use]
    pub fn interpolation_depth(&self) -> usize {
        self.interpolations.len()
    }

    pub fn clear_interpolations(&mut self) {
        self.interpolations.clear();
    }

    pub fn syntax_error(
        &mut self,
        diags: &mut DiagBag,
        code: ErrorCode,
        span: Span,
        message: &str,
        reason: Option<&str>,
        fix: Option<Fix>,
    ) {
        let allocation = code == ErrorCode::SyntaxNoMemory;
        if (self.failed || self.fatal) && !allocation {
            return;
        }
        self.failed = true;
        if allocation {
            self.fatal = true;
        }
        if let Some(last) = diags.items().last()
            && ((allocation && last.code == code) || (!allocation && last.primary.span == span))
        {
            return;
        }
        if self.recovering && !allocation {
            return;
        }
        let num = code.number();
        let min_limit = ErrorCode::LineTooLong.number();
        let max_limit = ErrorCode::TooManyCases.number();
        if num >= min_limit && num <= max_limit {
            let bit = 1u8 << (num - min_limit);
            if (self.limits_reported & bit) != 0 {
                return;
            }
            self.limits_reported |= bit;
        }
        let mut diag = Diagnostic::new(code, span, message);
        if let Some(r) = reason {
            diag = diag.with_reason(r);
        }
        if let Some(f) = fix {
            diag = diag.with_fix(f);
        }
        match diags.push(diag) {
            Ok(true) => {}
            Ok(false) | Err(_) => {
                self.fatal = true;
            }
        }
    }

    pub fn error(
        &mut self,
        diags: &mut DiagBag,
        code: ErrorCode,
        span: Span,
        message: &str,
        reason: Option<&str>,
    ) {
        self.syntax_error(diags, code, span, message, reason, None);
    }

    fn peek(&self, ahead: u32) -> Option<u8> {
        let bytes = self.source.bytes();
        let idx = (self.pos as usize).checked_add(ahead as usize)?;
        bytes.get(idx).copied()
    }

    fn lex_error_from(&mut self, diags: &mut DiagBag, code: ErrorCode, message: &str, from: u32) {
        let len = self.source.len();
        let end = if from < self.pos {
            self.pos
        } else if self.pos < len {
            self.pos + 1
        } else {
            self.pos
        };
        let span = Span::new(self.file, from, end);
        let reason = code.section().map(|sec| {
            let rule = match code {
                ErrorCode::SourceUtf8 | ErrorCode::SourceInvisible => {
                    "Source is UTF-8; invisible characters are an error anywhere"
                }
                ErrorCode::SourceName => "Names are ASCII, in snake_case or CapCase",
                ErrorCode::SourceNumber => "Whole numbers may put _ between digits",
                ErrorCode::SourceText => {
                    "Text and characters use the listed escapes and Unicode scalar values"
                }
                ErrorCode::SourceIndex => {
                    "List positions are read with get, which returns an Option"
                }
                ErrorCode::SourceFunction => {
                    "A function signature uses fn and : before its result type"
                }
                ErrorCode::SourceAssignment => "Compound assignment uses only += and -=",
                ErrorCode::SourceQualification => "Module-qualified names use .",
                ErrorCode::SourceComment => "Comments start with #",
                ErrorCode::SourceOperator => "Use Mo's arithmetic operators",
                ErrorCode::SourceCall => "Dot calls chain with .",
                ErrorCode::SourceCase => "A case arm is Pattern: value",
                _ => "A line break ends a statement; use Mo's tokens",
            };
            format!("{rule} (reference {sec}).")
        });
        self.error(diags, code, span, message, reason.as_deref());
    }

    fn lex_error(&mut self, diags: &mut DiagBag, code: ErrorCode, message: &str) {
        self.lex_error_from(diags, code, message, self.pos);
    }

    fn advance(&mut self, diags: &mut DiagBag) -> bool {
        let bytes = self.source.bytes();
        let pos = self.pos as usize;
        if pos >= bytes.len() {
            return false;
        }
        let Some((c, n)) = decode_utf8(&bytes[pos..]) else {
            self.lex_error(diags, ErrorCode::SourceUtf8, "source is UTF-8");
            self.pos += 1;
            return false;
        };
        if is_invisible(c) || (c == 13 && self.peek(1) != Some(10)) {
            self.lex_error(
                diags,
                ErrorCode::SourceInvisible,
                "invisible characters are an error anywhere",
            );
            self.pos += n;
            return false;
        }
        self.pos += n;
        if c == 10 {
            self.line += 1;
            self.column = 1;
        } else if c != 13 {
            self.column += 1;
        }
        if u64::from(self.column) > MO_LIMIT_LINE_LENGTH + 1 {
            self.lex_error(diags, ErrorCode::LineTooLong, "a source line is too long");
            return false;
        }
        true
    }

    fn scan_digits(&mut self, diags: &mut DiagBag, base: u8) -> bool {
        let mut any = false;
        let mut underscore = false;
        while let Some(c) = self.peek(0) {
            let valid = match base {
                16 => is_digit(c) || (b'a'..=b'f').contains(&c) || (b'A'..=b'F').contains(&c),
                2 => c == b'0' || c == b'1',
                _ => is_digit(c),
            };
            if valid {
                any = true;
                underscore = false;
            } else if c == b'_' && any && !underscore {
                underscore = true;
            } else {
                break;
            }
            if !self.advance(diags) {
                return false;
            }
        }
        if !any || underscore || self.peek(0) == Some(b'_') {
            self.lex_error(
                diags,
                ErrorCode::SourceNumber,
                "whole numbers put _ between digits",
            );
            return false;
        }
        true
    }

    fn scan_escape(&mut self, diags: &mut DiagBag) -> bool {
        if let Some(c) = self.peek(0) {
            if matches!(c, b'n' | b't' | b'r' | b'\\' | b'"' | b'\'' | b'#') {
                return self.advance(diags);
            }
            if c == b'u' && self.peek(1) == Some(b'{') {
                if !self.advance(diags) || !self.advance(diags) {
                    return false;
                }
                let mut value = 0u32;
                let mut count = 0u32;
                while let Some(hc) = self.peek(0) {
                    let d = if is_digit(hc) {
                        u32::from(hc - b'0')
                    } else if (b'a'..=b'f').contains(&hc) {
                        u32::from(hc - b'a' + 10)
                    } else if (b'A'..=b'F').contains(&hc) {
                        u32::from(hc - b'A' + 10)
                    } else {
                        break;
                    };
                    value = value.wrapping_mul(16).wrapping_add(d);
                    count += 1;
                    if !self.advance(diags) {
                        return false;
                    }
                    if count > 6 {
                        break;
                    }
                }
                if count > 0
                    && count <= 6
                    && value <= 0x10_ffff
                    && !(0xd800..=0xdfff).contains(&value)
                    && self.peek(0) == Some(b'}')
                {
                    return self.advance(diags);
                }
            }
        }
        self.lex_error(
            diags,
            ErrorCode::SourceText,
            "unknown escape; Mo has \\n, \\t, \\r, \\\\, \\\", \\', \\# and \\u{...}",
        );
        false
    }

    fn text_body(
        &mut self,
        diags: &mut DiagBag,
        start: bool,
        triple: bool,
        indent: u32,
        mut line_start: bool,
    ) -> TokenKind {
        while self.peek(0).is_some() {
            if triple && line_start {
                line_start = false;
                let mut spaces = 0u32;
                while self.peek(spaces) == Some(b' ') {
                    spaces += 1;
                }
                if self.peek(spaces) == Some(b'"')
                    && self.peek(spaces + 1) == Some(b'"')
                    && self.peek(spaces + 2) == Some(b'"')
                {
                    for _ in 0..(spaces + 3) {
                        if !self.advance(diags) {
                            return TokenKind::Error;
                        }
                    }
                    if !start {
                        self.interpolations.pop();
                    }
                    self.recovery_end = 0;
                    return if start {
                        TokenKind::Text
                    } else {
                        TokenKind::TextTail
                    };
                }
                let after = self.peek(spaces);
                if after.is_some_and(|b| b != 10 && b != 13) && spaces < indent {
                    self.lex_error(
                        diags,
                        ErrorCode::SourceText,
                        "a non-empty line has less indentation than the closing triple quote",
                    );
                    return TokenKind::Error;
                }
            }
            let Some(c) = self.peek(0) else {
                break;
            };
            if !triple && c == b'"' {
                if !self.advance(diags) {
                    return TokenKind::Error;
                }
                if !start {
                    self.interpolations.pop();
                }
                return if start {
                    TokenKind::Text
                } else {
                    TokenKind::TextTail
                };
            }
            if c == 10 || c == 13 {
                if !triple {
                    let bytes = self.source.bytes();
                    let mut at = (self.pos as usize) + 1;
                    while at < bytes.len() && bytes[at] != b'\n' && bytes[at] != b'"' {
                        at += 1;
                    }
                    if at < bytes.len() && bytes[at] == b'"' {
                        self.recovery_end = (at + 1) as u32;
                    }
                    self.lex_error(
                        diags,
                        ErrorCode::SourceText,
                        "a line break in text needs triple quotes: \"\"\"",
                    );
                    return TokenKind::Error;
                }
                if !self.advance(diags) || (c == 13 && !self.advance(diags)) {
                    return TokenKind::Error;
                }
                line_start = true;
            } else if c == b'\\' {
                if !self.advance(diags) || !self.scan_escape(diags) {
                    return TokenKind::Error;
                }
            } else if c == b'#' && self.peek(1) == Some(b'{') {
                if !self.advance(diags) || !self.advance(diags) {
                    return TokenKind::Error;
                }
                if start {
                    if (self.interpolations.len() as u64) == MO_LIMIT_INTERPOLATION_DEPTH {
                        self.lex_error(
                            diags,
                            ErrorCode::InterpolationsTooDeep,
                            "interpolations are nested too deeply",
                        );
                        return TokenKind::Error;
                    }
                    self.interpolations.push(InterpolationFrame {
                        braces: 0,
                        triple,
                        indent,
                    });
                }
                return if start {
                    TokenKind::TextHead
                } else {
                    TokenKind::TextMid
                };
            } else if !self.advance(diags) {
                return TokenKind::Error;
            }
        }
        self.lex_error(diags, ErrorCode::SourceText, "text has a closing quote");
        TokenKind::Error
    }

    fn scan_word(&mut self, diags: &mut DiagBag) -> TokenKind {
        let start = self.pos;
        let cap = self.peek(0).is_some_and(is_upper);
        let mut bad = false;
        while let Some(c) = self.peek(0) {
            if is_alnum(c) || (!cap && c == b'_') {
                bad = bad || (!cap && is_upper(c));
                if !self.advance(diags) {
                    return TokenKind::Error;
                }
            } else {
                break;
            }
        }
        if self.peek(0) == Some(b'?') {
            bad = bad || cap;
            if !self.advance(diags) {
                return TokenKind::Error;
            }
        }
        if bad || (cap && self.peek(0) == Some(b'_')) {
            self.lex_error_from(
                diags,
                ErrorCode::SourceName,
                "names are snake_case; types and modules are CapCase",
                start,
            );
            return TokenKind::Error;
        }
        if cap {
            return TokenKind::CapName;
        }
        let text = &self.source.bytes()[start as usize..self.pos as usize];
        if text == b"_" {
            return TokenKind::Underscore;
        }
        for &(word, kind) in KEYWORDS {
            if text == word.as_bytes() {
                return kind;
            }
        }
        TokenKind::Name
    }

    fn scan_literal(&mut self, diags: &mut DiagBag) -> TokenKind {
        let Some(c) = self.peek(0) else {
            return TokenKind::Error;
        };
        if is_digit(c) {
            let start = self.pos;
            let mut kind = TokenKind::Int;
            if self.last == TokenKind::Dot {
                while self.peek(0).is_some_and(is_digit) {
                    if !self.advance(diags) {
                        return TokenKind::Error;
                    }
                }
            } else if c == b'0' && matches!(self.peek(1), Some(b'x' | b'b')) {
                let base = if self.peek(1) == Some(b'x') { 16 } else { 2 };
                if !self.advance(diags) || !self.advance(diags) || !self.scan_digits(diags, base) {
                    return TokenKind::Error;
                }
            } else {
                if !self.scan_digits(diags, 10) {
                    return TokenKind::Error;
                }
                if c == b'0' && self.pos - start > 1 {
                    self.lex_error(
                        diags,
                        ErrorCode::SourceNumber,
                        "a whole number doesn't start with 0",
                    );
                    return TokenKind::Error;
                }
                if self.peek(0) == Some(b'.') && self.peek(1).is_some_and(is_digit) {
                    if !self.advance(diags) || !self.scan_digits(diags, 10) {
                        return TokenKind::Error;
                    }
                    kind = TokenKind::Decimal;
                }
            }
            if self.peek(0).is_some_and(|b| is_alnum(b) || b == b'_') {
                self.lex_error(
                    diags,
                    ErrorCode::SourceNumber,
                    "Mo has no number suffixes or exponents",
                );
                return TokenKind::Error;
            }
            return kind;
        }
        if c == b'\'' {
            if !self.advance(diags) {
                return TokenKind::Error;
            }
            let Some(ch) = self.peek(0) else {
                self.lex_error(
                    diags,
                    ErrorCode::SourceText,
                    "a Char is one Unicode character",
                );
                return TokenKind::Error;
            };
            if ch == 10 || ch == 13 || ch == b'\'' {
                self.lex_error(
                    diags,
                    ErrorCode::SourceText,
                    "a Char is one Unicode character",
                );
                return TokenKind::Error;
            }
            if !self.advance(diags) || (ch == b'\\' && !self.scan_escape(diags)) {
                return TokenKind::Error;
            }
            if self.peek(0) != Some(b'\'') {
                self.lex_error(
                    diags,
                    ErrorCode::SourceText,
                    "a Char is one Unicode character",
                );
                return TokenKind::Error;
            }
            if !self.advance(diags) {
                return TokenKind::Error;
            }
            return TokenKind::Char;
        }
        let triple = self.peek(1) == Some(b'"') && self.peek(2) == Some(b'"');
        if !self.advance(diags) {
            return TokenKind::Error;
        }
        let mut indent = 0u32;
        if triple {
            if !self.advance(diags) || !self.advance(diags) {
                return TokenKind::Error;
            }
            while matches!(self.peek(0), Some(b' ' | 9)) {
                if !self.advance(diags) {
                    return TokenKind::Error;
                }
            }
            if !matches!(self.peek(0), Some(10 | 13)) {
                self.lex_error(
                    diags,
                    ErrorCode::SourceText,
                    "text on several lines starts at the end of a line",
                );
                return TokenKind::Error;
            }
            let need_scan = match self.triple_cache {
                None => true,
                Some((close, _)) => close < self.pos,
            };
            if need_scan {
                let bytes = self.source.bytes();
                let mut at = self.pos as usize;
                let mut found_close = self.source.len();
                let mut found_indent = 0u32;
                while at < bytes.len() {
                    while at < bytes.len() && bytes[at] != 10 {
                        at += 1;
                    }
                    if at == bytes.len() {
                        break;
                    }
                    at += 1;
                    let mut spaces = 0usize;
                    while at + spaces < bytes.len() && bytes[at + spaces] == b' ' {
                        spaces += 1;
                    }
                    if bytes[at + spaces..].starts_with(b"\"\"\"") {
                        found_close = at as u32;
                        found_indent = spaces as u32;
                        break;
                    }
                }
                self.triple_cache = Some((found_close, found_indent));
            }
            let (triple_close, triple_indent) = self.triple_cache.unwrap_or((self.source.len(), 0));
            if triple_close == self.source.len() {
                self.recovery_end = self.source.len();
                self.lex_error(
                    diags,
                    ErrorCode::SourceText,
                    "text has a closing triple quote",
                );
                return TokenKind::Error;
            }
            indent = triple_indent;
            self.recovery_end = triple_close + indent + 3;
            if self.peek(0) == Some(13) && !self.advance(diags) {
                return TokenKind::Error;
            }
            if !self.advance(diags) {
                return TokenKind::Error;
            }
        }
        self.text_body(diags, true, triple, indent, triple)
    }

    fn boolean_error(
        &mut self,
        diags: &mut DiagBag,
        start: u32,
        length: u32,
        message: &str,
        word: &str,
        fixable: bool,
    ) {
        let bytes = self.source.bytes();
        let end = start + length;
        let before = if start == 0 {
            b' '
        } else {
            bytes[(start - 1) as usize]
        };
        let after = if (end as usize) == bytes.len() {
            b' '
        } else {
            bytes[end as usize]
        };
        let left = if length == 2 {
            !matches!(before, b' ' | 9 | 10 | 13)
        } else {
            is_alnum(before) || before == b'_' || before == b'?'
        };
        let right = !matches!(after, b' ' | 9 | 10 | 13);
        let replacement = format!(
            "{}{word}{}",
            if left { " " } else { "" },
            if right { " " } else { "" }
        );
        let fix = if fixable {
            Some(Fix::new(
                Some(format!("write {word}").into()),
                vec![Edit::new(Span::new(self.file, start, end), replacement)],
            ))
        } else {
            None
        };
        self.syntax_error(
            diags,
            ErrorCode::SourceBoolean,
            Span::new(self.file, start, start + 1),
            message,
            Some("Boolean expressions use and, or and not (reference 3.1)."),
            fix,
        );
    }

    fn punctuation(&mut self, diags: &mut DiagBag) -> TokenKind {
        const PAIRS: &[([u8; 2], TokenKind)] = &[
            (*b"..", TokenKind::DotDot),
            (*b"+=", TokenKind::PlusEq),
            (*b"-=", TokenKind::MinusEq),
            (*b"==", TokenKind::Eq),
            (*b"!=", TokenKind::Ne),
            (*b"<=", TokenKind::Le),
            (*b">=", TokenKind::Ge),
        ];
        const BAD: &[(&[u8], &str, ErrorCode)] = &[
            (
                b"::",
                "Mo separates a module from a name with ., not ::",
                ErrorCode::SourceQualification,
            ),
            (
                b"...",
                "a range is written with .., not ...",
                ErrorCode::SourceOperator,
            ),
            (
                b"=>",
                "a case arm is written Pattern: value, with no =>",
                ErrorCode::SourceCase,
            ),
            (
                b"->",
                "a function's result type follows :, not ->",
                ErrorCode::SourceFunction,
            ),
            (b"**", "Mo has no ** operator", ErrorCode::SourceOperator),
            (
                b"*=",
                "Mo has += and -=, and no other compound assignment",
                ErrorCode::SourceAssignment,
            ),
            (b"//", "comments start with #", ErrorCode::SourceComment),
            (
                b"/=",
                "Mo has += and -=, and no other compound assignment",
                ErrorCode::SourceAssignment,
            ),
            (
                b"|>",
                "Mo has no pipe operator; chain calls with dots, as in text.trim.lines",
                ErrorCode::SourceCall,
            ),
        ];

        let p0 = self.peek(0);
        let p1 = self.peek(1);
        if matches!(p0, Some(b'|' | b'&')) && p1 == p0 {
            let value = matches!(
                self.last,
                TokenKind::Name
                    | TokenKind::CapName
                    | TokenKind::Int
                    | TokenKind::Decimal
                    | TokenKind::Char
                    | TokenKind::Text
                    | TokenKind::TextTail
                    | TokenKind::True
                    | TokenKind::False
                    | TokenKind::RParen
                    | TokenKind::RBrack
                    | TokenKind::RBrace
            );
            let bar = p0 == Some(b'|');
            self.boolean_error(
                diags,
                self.pos,
                2,
                if bar {
                    "Mo writes or instead of ||"
                } else {
                    "Mo writes and instead of &&"
                },
                if bar { "or" } else { "and" },
                value && self.peek(2) != Some(b'='),
            );
            return TokenKind::Error;
        }
        if p0 == Some(b'=')
            && p1 == Some(b'>')
            && (self.line_opens > 0 || self.line_assign || self.lambda_close)
        {
            self.error(
                diags,
                ErrorCode::SourceLambda,
                Span::new(self.file, self.pos, self.pos + 2),
                "Mo has no => functions: write fn(x) ... end",
                Some(LAMBDA_REASON),
            );
            return TokenKind::Error;
        }
        let rest = &self.source.bytes()[self.pos as usize..];
        for &(pat, msg, code) in BAD {
            if rest.starts_with(pat) {
                self.lex_error(diags, code, msg);
                return TokenKind::Error;
            }
        }

        let mut kind = TokenKind::Error;
        if let (Some(a), Some(b)) = (p0, p1) {
            for &(pair, k) in PAIRS {
                if a == pair[0] && b == pair[1] {
                    kind = k;
                    if !self.advance(diags) {
                        return TokenKind::Error;
                    }
                    break;
                }
            }
        }
        if kind == TokenKind::Error
            && let Some(c) = self.peek(0)
        {
            kind = match c {
                b'(' => TokenKind::LParen,
                b')' => TokenKind::RParen,
                b'[' => TokenKind::LBrack,
                b']' => TokenKind::RBrack,
                b'{' => TokenKind::LBrace,
                b'}' => TokenKind::RBrace,
                b',' => TokenKind::Comma,
                b':' => TokenKind::Colon,
                b'.' => TokenKind::Dot,
                b'|' => TokenKind::Bar,
                b'=' => TokenKind::Assign,
                b'<' => TokenKind::Lt,
                b'>' => TokenKind::Gt,
                b'+' => TokenKind::Plus,
                b'-' => TokenKind::Minus,
                b'*' => TokenKind::Star,
                b'/' => TokenKind::Slash,
                b'%' => TokenKind::Percent,
                _ => TokenKind::Error,
            };
        }

        if kind == TokenKind::Bar
            && matches!(
                self.last,
                TokenKind::LParen
                    | TokenKind::LBrack
                    | TokenKind::LBrace
                    | TokenKind::Comma
                    | TokenKind::Colon
                    | TokenKind::Assign
            )
        {
            self.error(
                diags,
                ErrorCode::SourceLambda,
                Span::new(self.file, self.pos, self.pos + 1),
                "Mo has no |x| functions: write fn(x) ... end",
                Some(LAMBDA_REASON),
            );
            return TokenKind::Error;
        }

        if kind == TokenKind::Error {
            let start = self.pos;
            let c = self.peek(0);
            if !self.advance(diags) {
                return TokenKind::Error;
            }
            if c == Some(b'!')
                && self.preceding.span.end == start
                && matches!(
                    self.last,
                    TokenKind::Name
                        | TokenKind::CapName
                        | TokenKind::RParen
                        | TokenKind::RBrack
                        | TokenKind::RBrace
                        | TokenKind::Text
                        | TokenKind::TextTail
                )
            {
                self.error(
                    diags,
                    ErrorCode::SourceUnwrap,
                    Span::new(self.file, start, self.pos),
                    "Mo has no postfix ! to unwrap a value: use case, with stop \"why it can't happen\"",
                    Some(
                        "There is no unwrap: where a failure truly can't happen, the code says why, with stop (reference 8.4).",
                    ),
                );
                return TokenKind::Error;
            }
            if c == Some(b'!') {
                self.boolean_error(diags, start, 1, "Mo writes not instead of !", "not", true);
                return TokenKind::Error;
            }
            let (code, message, reason) = match c {
                Some(b';') => (
                    ErrorCode::SourceToken,
                    "a line break ends a statement; Mo has no semicolons",
                    "A line break ends a statement; use Mo's tokens (reference 2.4).",
                ),
                Some(b'&') => (
                    ErrorCode::SourceOperator,
                    "Mo has no & operator",
                    "Use Mo's arithmetic operators (reference 3.2).",
                ),
                _ => (
                    ErrorCode::SourceToken,
                    "this character isn't part of Mo",
                    "A line break ends a statement; use Mo's tokens (reference 2.4).",
                ),
            };
            self.error(
                diags,
                code,
                Span::new(self.file, start, self.pos),
                message,
                Some(reason),
            );
            return TokenKind::Error;
        }

        if !self.advance(diags) {
            return TokenKind::Error;
        }
        if let Some(top) = self.interpolations.last_mut() {
            if kind == TokenKind::LBrace {
                top.braces += 1;
            } else if kind == TokenKind::RBrace {
                top.braces = top.braces.saturating_sub(1);
            }
        }
        kind
    }

    pub fn next_token(&mut self, tree: &mut SyntaxTree, diags: &mut DiagBag) -> Token {
        let mut token = Token {
            kind: TokenKind::Eof,
            span: Span::new(self.file, self.pos, self.pos),
            line: self.line,
            column: self.column,
        };
        if self.fatal || self.failed {
            token.kind = TokenKind::Error;
            return token;
        }
        if let Some(pending) = self.pending.take() {
            self.last = pending.kind;
            return pending;
        }

        let mut newline = false;
        let mut nl = token;
        let depth = self.interpolations.len();
        let mut bad_gap = false;

        loop {
            let c = self.peek(0);
            if matches!(c, Some(b' ' | 9 | 10 | 13)) {
                if matches!(c, Some(10 | 13)) && !newline {
                    newline = true;
                    nl.span.start = self.pos;
                    nl.line = self.line;
                    nl.column = self.column;
                }
                if !self.advance(diags) {
                    bad_gap = true;
                    break;
                }
            } else if c == Some(b'#') {
                if self.ignore_comments {
                    while self.peek(0).is_some_and(|b| b != 10 && b != 13) {
                        if !self.advance(diags) {
                            bad_gap = true;
                            break;
                        }
                    }
                    if bad_gap {
                        break;
                    }
                    continue;
                }
                if !tree.try_charge(COMMENT_ARENA_COST) {
                    self.lex_error(
                        diags,
                        ErrorCode::SyntaxNoMemory,
                        "the syntax arena has no room",
                    );
                    token.kind = TokenKind::Error;
                    return token;
                }
                let preceding = if self.preceding.kind != TokenKind::Eof
                    && self.preceding_line == self.line
                    && self.preceding.span.end <= self.pos
                {
                    Some(self.preceding)
                } else {
                    None
                };
                let comment_start = self.pos;
                let indentation = self.column.saturating_sub(1);
                let documentation = self.peek(1) == Some(b'#');
                while self.peek(0).is_some_and(|b| b != 10 && b != 13) {
                    if !self.advance(diags) {
                        bad_gap = true;
                        break;
                    }
                }
                if bad_gap {
                    break;
                }
                let comment_end = self.pos;
                let bytes = &self.source.bytes()[comment_start as usize..comment_end as usize];
                let Some(text) = tree.copy_bytes(bytes) else {
                    self.lex_error(
                        diags,
                        ErrorCode::SyntaxNoMemory,
                        "the syntax arena has no room",
                    );
                    self.fatal = true;
                    token.kind = TokenKind::Error;
                    return token;
                };
                let (anchor, tok_kind) = match preceding {
                    None => (CommentAnchor::OwnLine, TokenKind::Eof),
                    Some(prev) => (CommentAnchor::Token, prev.kind),
                };
                self.comments.push(RawComment {
                    comment: SyntaxComment {
                        span: Span::new(self.file, comment_start, comment_end),
                        text,
                        indentation,
                        documentation,
                        anchor,
                        token: tok_kind,
                        token_index: 0,
                    },
                    preceding,
                    owner: None,
                });
            } else {
                break;
            }
        }

        if bad_gap {
            token.kind = TokenKind::Error;
            token.span.start = self.pos;
            token.line = self.line;
            token.column = self.column;
        } else {
            token.span.start = self.pos;
            token.line = self.line;
            token.column = self.column;
            let c = self.peek(0);
            if self.header_only
                && newline
                && depth == 0
                && self.last != TokenKind::Eof
                && !continues(self.last, false)
                && !matches!(
                    c,
                    Some(
                        b'.' | b')'
                            | b']'
                            | b'}'
                            | b'+'
                            | b'*'
                            | b'/'
                            | b'%'
                            | b'|'
                            | b'<'
                            | b'>'
                            | b'='
                    )
                )
            {
                let mut length = 0u32;
                while self.peek(length).is_some_and(is_lower) {
                    length += 1;
                }
                let slice = &self.source.bytes()[self.pos as usize..(self.pos + length) as usize];
                let after = self.peek(length);
                let header_word = (slice == b"use" || slice == b"expose")
                    && !after.is_some_and(|b| is_alnum(b) || b == b'_' || b == b'?');
                if !header_word {
                    let mut eof_tok = token;
                    eof_tok.kind = TokenKind::Eof;
                    eof_tok.span.end = eof_tok.span.start;
                    self.pending = Some(eof_tok);
                    nl.kind = TokenKind::Nl;
                    nl.span.end = nl.span.start;
                    self.last = TokenKind::Nl;
                    return nl;
                }
            }

            token.kind = match c {
                None => TokenKind::Eof,
                Some(b) if is_lower(b) || is_upper(b) || b == b'_' => self.scan_word(diags),
                Some(b) if is_digit(b) || b == b'"' || b == b'\'' => self.scan_literal(diags),
                Some(b'[')
                    if !newline
                        && ((self.last == TokenKind::Name
                            && !starts_type_after_bracket(
                                self.source.bytes(),
                                (self.pos as usize) + 1,
                            ))
                            || self.last == TokenKind::CapName
                            || (self.last == TokenKind::RParen && !self.lambda_close)
                            || self.last == TokenKind::RBrack
                            || self.last == TokenKind::Text
                            || self.last == TokenKind::TextTail) =>
                {
                    self.lex_error(
                        diags,
                        ErrorCode::SourceIndex,
                        "Mo has no indexing with [...]: items.get(i) and map.get(key) return an Option",
                    );
                    TokenKind::Error
                }
                Some(b'}') if depth > 0 && self.interpolations[depth - 1].braces == 0 => {
                    if !self.advance(diags) {
                        TokenKind::Error
                    } else {
                        let frame = self.interpolations[depth - 1];
                        self.text_body(diags, false, frame.triple, frame.indent, false)
                    }
                }
                Some(_) => self.punctuation(diags),
            };
        }

        // Lambda parameters cannot nest: only names, _ and commas are allowed.
        self.lambda_close = token.kind == TokenKind::RParen && self.lambda_parameters;
        self.lambda_parameters = (token.kind == TokenKind::LParen && self.last == TokenKind::Fn)
            || (self.lambda_parameters
                && matches!(
                    token.kind,
                    TokenKind::Name | TokenKind::Underscore | TokenKind::Comma
                ));
        token.span.end = self.pos;
        if token.kind != TokenKind::Eof && token.kind != TokenKind::Error {
            self.preceding = token;
            self.preceding_line = self.line;
        }
        if token.kind == TokenKind::Eof && depth > 0 {
            self.lex_error(
                diags,
                ErrorCode::SourceText,
                "an interpolation is closed with }",
            );
            token.kind = TokenKind::Error;
        }
        let split = self.last != TokenKind::Eof
            && self.last != TokenKind::Nl
            && (token.kind == TokenKind::Eof
                || (newline && !continues(self.last, false) && !continues(token.kind, true)));
        if split || self.last == TokenKind::Eof || self.last == TokenKind::Nl {
            self.line_opens = 0;
            self.line_assign = false;
        }
        if matches!(
            token.kind,
            TokenKind::LParen | TokenKind::LBrack | TokenKind::LBrace
        ) {
            self.line_opens += 1;
        } else if matches!(
            token.kind,
            TokenKind::RParen | TokenKind::RBrack | TokenKind::RBrace
        ) && self.line_opens > 0
        {
            self.line_opens -= 1;
        } else if token.kind == TokenKind::Assign {
            self.line_assign = true;
        }
        if split {
            self.pending = Some(token);
            nl.kind = TokenKind::Nl;
            nl.span.end = nl.span.start;
            self.last = TokenKind::Nl;
            return nl;
        }
        self.last = token.kind;
        token
    }

    pub fn resume(&mut self) {
        let bytes = self.source.bytes();
        let len = self.source.len();
        while self.pos < len && (self.pos < self.recovery_end || bytes[self.pos as usize] != b'\n')
        {
            if bytes[self.pos as usize] == b'\n' {
                self.line += 1;
            }
            self.pos += 1;
        }
        if self.pos < len {
            self.pos += 1;
            self.line += 1;
        }
        self.column = 1;
        self.last = TokenKind::Eof;
        self.lambda_parameters = false;
        self.lambda_close = false;
        self.line_opens = 0;
        self.line_assign = false;
        self.pending = None;
        self.interpolations.clear();
        self.recovery_end = 0;
        self.failed = false;
    }
}
