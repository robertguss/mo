//! Arena-indexed surface syntax tree, token kinds, comment anchors, duration units,
//! and iterative structural tree comparison (`src/syntax/syntax.h` & `tests/format-contract.h`).

use mo_base::Span;

/// Supported duration unit spellings (`src/syntax/format.c:mo_duration_units`).
pub const DURATION_UNITS: [&str; 5] = ["seconds", "milliseconds", "minutes", "hours", "days"];

/// Resolves a singular or plural duration unit name to its canonical plural spelling.
#[must_use]
pub fn duration_unit(text: &[u8]) -> Option<&'static str> {
    for unit in DURATION_UNITS {
        let plural = unit.len();
        if (text.len() == plural || text.len() + 1 == plural) && unit.as_bytes().starts_with(text) {
            return Some(unit);
        }
    }
    None
}

/// Surface AST node kind.
///
/// Discriminant order MUST match `enum mo_syntax_kind` in `src/syntax/syntax.h` because
/// own-line comment attachment breaks ties by ascending `SyntaxKind` when two nodes share
/// both start and end byte offsets.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SyntaxKind {
    File = 0,
    Module,
    Expose,
    ExposeAll,
    Use,
    Fn,
    Param,
    Where,
    Struct,
    Field,
    Enum,
    CaseDecl,
    Test,
    Const,
    Service,
    ServiceState,
    ServiceTools,
    ServiceEvery,
    ServiceToken,
    Actor,
    ActorCalls,
    Api,
    ApiPort,
    ApiCors,
    ApiSqlite,
    ApiActor,
    ApiAuth,
    ApiRoute,
    ApiStatic,
    ApiJob,
    ApiScope,
    ApiStatus,
    Promise,
    Anchor,
    Examples,
    Type,
    Sequence,
    ExprStmt,
    Bind,
    Var,
    AddAssign,
    SubAssign,
    Return,
    Break,
    Continue,
    Assert,
    For,
    While,
    Binder,
    If,
    Else,
    LineIf,
    Case,
    Arm,
    Lambda,
    Binary,
    Negate,
    Not,
    Try,
    Is,
    Range,
    Call,
    TypeArgs,
    NamedArg,
    FieldAccess,
    Qualified,
    Position,
    Int,
    Decimal,
    Char,
    Text,
    TextInterpolated,
    True,
    False,
    Name,
    CapName,
    Underscore,
    Group,
    Tuple,
    List,
    Rest,
    Map,
    MapEntry,
    Stop,
    Token,
    BinaryChain,
    Postfix,
    Prefix,
    IfPart,
}

/// Surface token kind, independent of Bison's generated values (`enum mo_syntax_token`).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TokenKind {
    Eof = 0,
    Error,
    Nl,
    Name,
    CapName,
    Underscore,
    Int,
    Decimal,
    Char,
    Text,
    TextHead,
    TextMid,
    TextTail,
    Module,
    Expose,
    Use,
    Fn,
    Struct,
    Enum,
    Var,
    If,
    Else,
    Case,
    For,
    In,
    While,
    Break,
    Continue,
    Return,
    Try,
    And,
    Or,
    Not,
    Is,
    Implies,
    True,
    False,
    Stop,
    Promise,
    Allows,
    Forbids,
    Examples,
    Test,
    Assert,
    Where,
    End,
    Service,
    Api,
    Actor,
    LParen,
    RParen,
    LBrack,
    RBrack,
    LBrace,
    RBrace,
    Comma,
    Colon,
    Dot,
    DotDot,
    Bar,
    Assign,
    PlusEq,
    MinusEq,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
}

impl TokenKind {
    /// Total number of `TokenKind` variants (`MO_T_PERCENT + 1`).
    pub const COUNT: usize = (Self::Percent as usize) + 1;

    /// Zero-based discriminant index in `0..Self::COUNT`.
    #[must_use]
    pub const fn as_index(self) -> usize {
        self as usize
    }

    /// Human-readable token spelling used in parser `expected X, found Y` diagnostics
    /// (`src/syntax/parser.c:spelling`).
    #[must_use]
    pub const fn diagnostic_name(self) -> &'static str {
        match self {
            Self::Eof => "end of file",
            Self::Error => "invalid token",
            Self::Nl => "line break",
            Self::Name => "snake_case name",
            Self::CapName => "CapCase name",
            Self::Underscore => "_",
            Self::Int => "integer",
            Self::Decimal => "decimal",
            Self::Char => "character",
            Self::Text => "text",
            Self::TextHead | Self::TextMid => "text interpolation",
            Self::TextTail => "text tail",
            Self::Module => "module",
            Self::Expose => "expose",
            Self::Use => "use",
            Self::Fn => "fn",
            Self::Struct => "struct",
            Self::Enum => "enum",
            Self::Var => "var",
            Self::If => "if",
            Self::Else => "else",
            Self::Case => "case",
            Self::For => "for",
            Self::In => "in",
            Self::While => "while",
            Self::Break => "break",
            Self::Continue => "continue",
            Self::Return => "return",
            Self::Try => "try",
            Self::And => "and",
            Self::Or => "or",
            Self::Not => "not",
            Self::Is => "is",
            Self::Implies => "implies",
            Self::True => "true",
            Self::False => "false",
            Self::Stop => "stop",
            Self::Promise => "promise",
            Self::Allows => "allows",
            Self::Forbids => "forbids",
            Self::Examples => "examples",
            Self::Test => "test",
            Self::Assert => "assert",
            Self::Where => "where",
            Self::End => "end",
            Self::Service => "service",
            Self::Api => "api",
            Self::Actor => "actor",
            Self::LParen => "(",
            Self::RParen => ")",
            Self::LBrack => "[",
            Self::RBrack => "]",
            Self::LBrace => "{",
            Self::RBrace => "}",
            Self::Comma => ",",
            Self::Colon => ":",
            Self::Dot => ".",
            Self::DotDot => "..",
            Self::Bar => "|",
            Self::Assign => "=",
            Self::PlusEq => "+=",
            Self::MinusEq => "-=",
            Self::Eq => "==",
            Self::Ne => "!=",
            Self::Lt => "<",
            Self::Le => "<=",
            Self::Gt => ">",
            Self::Ge => ">=",
            Self::Plus => "+",
            Self::Minus => "-",
            Self::Star => "*",
            Self::Slash => "/",
            Self::Percent => "%",
        }
    }

    /// Fixed surface spelling for keywords and punctuation (`src/syntax/format.c:spellings`).
    #[must_use]
    pub const fn fixed_spelling(self) -> Option<&'static str> {
        match self {
            Self::Module => Some("module"),
            Self::Expose => Some("expose"),
            Self::Use => Some("use"),
            Self::Fn => Some("fn"),
            Self::Struct => Some("struct"),
            Self::Enum => Some("enum"),
            Self::Var => Some("var"),
            Self::If => Some("if"),
            Self::Else => Some("else"),
            Self::Case => Some("case"),
            Self::For => Some("for"),
            Self::In => Some("in"),
            Self::While => Some("while"),
            Self::Break => Some("break"),
            Self::Continue => Some("continue"),
            Self::Return => Some("return"),
            Self::Try => Some("try"),
            Self::And => Some("and"),
            Self::Or => Some("or"),
            Self::Not => Some("not"),
            Self::Is => Some("is"),
            Self::Implies => Some("implies"),
            Self::True => Some("true"),
            Self::False => Some("false"),
            Self::Stop => Some("stop"),
            Self::Promise => Some("promise"),
            Self::Allows => Some("allows"),
            Self::Forbids => Some("forbids"),
            Self::Examples => Some("examples"),
            Self::Test => Some("test"),
            Self::Assert => Some("assert"),
            Self::Where => Some("where"),
            Self::End => Some("end"),
            Self::Service => Some("service"),
            Self::Actor => Some("actor"),
            Self::Api => Some("api"),
            Self::LParen => Some("("),
            Self::RParen => Some(")"),
            Self::LBrack => Some("["),
            Self::RBrack => Some("]"),
            Self::LBrace => Some("{"),
            Self::RBrace => Some("}"),
            Self::Comma => Some(","),
            Self::Colon => Some(":"),
            Self::Dot => Some("."),
            Self::DotDot => Some(".."),
            Self::Bar => Some("|"),
            Self::Assign => Some("="),
            Self::PlusEq => Some("+="),
            Self::MinusEq => Some("-="),
            Self::Plus => Some("+"),
            Self::Minus => Some("-"),
            Self::Star => Some("*"),
            Self::Slash => Some("/"),
            Self::Percent => Some("%"),
            Self::Eq => Some("=="),
            Self::Ne => Some("!="),
            Self::Lt => Some("<"),
            Self::Le => Some("<="),
            Self::Gt => Some(">"),
            Self::Ge => Some(">="),
            Self::Eof
            | Self::Error
            | Self::Nl
            | Self::Name
            | Self::CapName
            | Self::Underscore
            | Self::Int
            | Self::Decimal
            | Self::Char
            | Self::Text
            | Self::TextHead
            | Self::TextMid
            | Self::TextTail => None,
        }
    }
}

/// How a comment attaches to its owning syntax node (`enum mo_syntax_comment_anchor`).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CommentAnchor {
    OwnLine = 0,
    Token = 1,
    Item = 2,
}

/// Arena index for a [`SyntaxNode`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct NodeId(pub u32);

/// Arena index for a [`SyntaxComment`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CommentId(pub u32);

/// Byte range inside a [`SyntaxTree`]'s owned text pool.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TextSlice {
    start: u32,
    len: u32,
}

impl TextSlice {
    #[must_use]
    pub const fn new(start: u32, len: u32) -> Self {
        Self { start, len }
    }

    #[must_use]
    pub const fn len(self) -> u32 {
        self.len
    }

    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.len == 0
    }
}

/// One comment attached to a syntax node (`struct mo_syntax_comment`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SyntaxComment {
    pub span: Span,
    pub text: TextSlice,
    pub indentation: u32,
    pub documentation: bool,
    pub anchor: CommentAnchor,
    pub token: TokenKind,
    pub token_index: u32,
}

/// One node in the surface syntax tree (`struct mo_syntax`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SyntaxNode {
    pub kind: SyntaxKind,
    pub span: Span,
    pub text: Option<TextSlice>,
    pub token: TokenKind,
    pub children: Vec<Option<NodeId>>,
    pub leading: Vec<CommentId>,
    pub trailing: Vec<CommentId>,
    pub ending: Vec<CommentId>,
    pub incomplete: bool,
    pub blank_before: bool,
}

/// C-equivalent byte costs used to enforce `SyntaxTree`'s memory budget parity with
/// `src/syntax/{lexer,parser,comments}.c` (`E1054`).
pub(crate) const NODE_ARENA_COST: usize = 96;
pub(crate) const COMMENT_ARENA_COST: usize = 96;
pub(crate) const CHILD_PTR_COST: usize = 8;

/// Arena owning all [`SyntaxNode`]s, [`SyntaxComment`]s, and copied raw token/comment bytes.
///
/// The tree borrows nothing from source storage; callers may drop the [`mo_base::SourceMap`]
/// immediately after parsing.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SyntaxTree {
    nodes: Vec<SyntaxNode>,
    comments: Vec<SyntaxComment>,
    text_pool: Vec<u8>,
    used_bytes: usize,
    budget: usize,
}

impl SyntaxTree {
    #[must_use]
    pub const fn with_budget(budget: usize) -> Self {
        Self {
            nodes: Vec::new(),
            comments: Vec::new(),
            text_pool: Vec::new(),
            used_bytes: 0,
            budget,
        }
    }

    pub(crate) fn try_charge(&mut self, bytes: usize) -> bool {
        let aligned = bytes.checked_add(7).map(|n| n & !7usize);
        let Some(cost) = aligned else {
            return false;
        };
        let Some(next) = self.used_bytes.checked_add(cost) else {
            return false;
        };
        if next > self.budget {
            return false;
        }
        self.used_bytes = next;
        true
    }

    #[must_use]
    pub const fn used_bytes(&self) -> usize {
        self.used_bytes
    }

    #[must_use]
    pub const fn budget(&self) -> usize {
        self.budget
    }

    #[must_use]
    pub const fn remaining_budget(&self) -> usize {
        self.budget.saturating_sub(self.used_bytes)
    }

    pub(crate) fn copy_bytes(&mut self, bytes: &[u8]) -> Option<TextSlice> {
        if !self.try_charge(bytes.len()) {
            return None;
        }
        let start = u32::try_from(self.text_pool.len()).ok()?;
        let len = u32::try_from(bytes.len()).ok()?;
        self.text_pool.extend_from_slice(bytes);
        Some(TextSlice::new(start, len))
    }

    pub(crate) fn alloc_node(&mut self, node: SyntaxNode) -> Option<NodeId> {
        let child_cost = node.children.len().checked_mul(CHILD_PTR_COST)?;
        let total = NODE_ARENA_COST.checked_add(child_cost)?;
        if !self.try_charge(total) {
            return None;
        }
        let id = NodeId(u32::try_from(self.nodes.len()).ok()?);
        self.nodes.push(node);
        Some(id)
    }

    pub(crate) fn alloc_comment(&mut self, comment: SyntaxComment) -> Option<CommentId> {
        let id = CommentId(u32::try_from(self.comments.len()).ok()?);
        self.comments.push(comment);
        Some(id)
    }

    #[must_use]
    pub fn node(&self, id: NodeId) -> &SyntaxNode {
        &self.nodes[id.0 as usize]
    }

    #[must_use]
    pub fn node_opt(&self, id: NodeId) -> Option<&SyntaxNode> {
        self.nodes.get(id.0 as usize)
    }

    pub(crate) fn node_mut(&mut self, id: NodeId) -> &mut SyntaxNode {
        &mut self.nodes[id.0 as usize]
    }

    #[must_use]
    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }

    #[must_use]
    pub fn comment(&self, id: CommentId) -> &SyntaxComment {
        &self.comments[id.0 as usize]
    }

    #[must_use]
    pub fn comment_count(&self) -> usize {
        self.comments.len()
    }

    #[must_use]
    pub fn text_slice(&self, slice: TextSlice) -> &[u8] {
        let start = slice.start as usize;
        let end = start + (slice.len as usize);
        &self.text_pool[start..end]
    }

    #[must_use]
    pub fn node_text(&self, id: NodeId) -> Option<&[u8]> {
        self.node(id).text.map(|s| self.text_slice(s))
    }

    #[must_use]
    pub fn comment_text(&self, id: CommentId) -> &[u8] {
        self.text_slice(self.comment(id).text)
    }
}

/// Parsed module header handles (`struct mo_header`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub span: Span,
    pub path: NodeId,
    pub expose: Option<NodeId>,
    pub uses: NodeId,
}

/// Result of [`crate::read_header`], pairing the arena with the header handles.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedHeader {
    pub tree: SyntaxTree,
    pub header: Header,
}

/// Complete or recovered surface module tree (`struct mo_module_syntax`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModuleSyntax {
    pub span: Span,
    pub header: Option<Header>,
    pub tree: SyntaxTree,
    pub root: NodeId,
}

/// Equivalence options for [`trees_equal`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TreeCompareOptions {
    /// Treat `x += y` as structurally equivalent to `x = x + y` (omitting comments on the
    /// synthesized repeated target), matching `tests/format-contract.h:format_tree`.
    pub normalize_add_assign: bool,
    /// Normalize singular/plural duration units (`1.second` vs `1.seconds`) after an integer
    /// literal receiver, matching `tests/format-contract.h:format_tree`.
    pub normalize_duration_aliases: bool,
    /// Treat the first body statement after a non-empty function specification as always
    /// preceded by a blank line (reference 12).
    pub normalize_spec_blank: bool,
}

impl TreeCompareOptions {
    /// Exact structural equality including comment anchors and `blank_before`
    /// (`tests/test_syntax.c:same_tree`).
    pub const EXACT: Self = Self {
        normalize_add_assign: false,
        normalize_duration_aliases: false,
        normalize_spec_blank: false,
    };

    /// Formatter round-trip contract equality (`tests/format-contract.h:format_tree(..., true)`).
    pub const FORMAT_EQUIVALENCE: Self = Self {
        normalize_add_assign: true,
        normalize_duration_aliases: true,
        normalize_spec_blank: true,
    };
}

struct CompareVisit {
    node: Option<NodeId>,
    sum: bool,
    omit_comments: bool,
    spec_body: bool,
    blank: bool,
    literal_step: bool,
    duration_name: bool,
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn serialize_comments(out: &mut Vec<u8>, tree: &SyntaxTree, comments: &[CommentId]) {
    for &cid in comments {
        let c = tree.comment(cid);
        let bytes = tree.comment_text(cid);
        push_u32(out, bytes.len() as u32);
        push_u32(out, u32::from(c.documentation));
        push_u32(out, c.anchor as u32);
        push_u32(out, c.token as u32);
        push_u32(out, c.token_index);
        out.extend_from_slice(bytes);
    }
    push_u32(out, u32::MAX);
}

fn serialize_tree(tree: &SyntaxTree, root: NodeId, opts: TreeCompareOptions) -> Vec<u8> {
    let mut out = Vec::new();
    let mut stack = vec![CompareVisit {
        node: Some(root),
        sum: false,
        omit_comments: false,
        spec_body: false,
        blank: false,
        literal_step: false,
        duration_name: false,
    }];
    while let Some(visit) = stack.pop() {
        let Some(nid) = visit.node else {
            push_u32(&mut out, u32::MAX);
            continue;
        };
        let n = tree.node(nid);
        let increment = opts.normalize_add_assign && n.kind == SyntaxKind::AddAssign;
        let kind = if visit.sum {
            SyntaxKind::Binary
        } else if increment {
            SyntaxKind::Bind
        } else {
            n.kind
        };
        let token = if visit.sum { TokenKind::Plus } else { n.token };
        let blank = if visit.sum {
            0u32
        } else if opts.normalize_spec_blank && visit.blank {
            1u32
        } else {
            u32::from(n.blank_before)
        };
        push_u32(&mut out, kind as u32);
        push_u32(&mut out, token as u32);
        push_u32(&mut out, n.children.len() as u32);
        push_u32(&mut out, blank);
        match n.text {
            None => push_u32(&mut out, 0),
            Some(slice) => {
                let mut bytes = tree.text_slice(slice);
                if opts.normalize_duration_aliases
                    && visit.duration_name
                    && n.kind == SyntaxKind::Name
                {
                    const SINGULAR: [&[u8]; 5] =
                        [b"millisecond", b"second", b"minute", b"hour", b"day"];
                    for s in SINGULAR {
                        if bytes.len() == s.len() + 1
                            && bytes.starts_with(s)
                            && bytes[s.len()] == b's'
                        {
                            bytes = &bytes[..s.len()];
                            break;
                        }
                    }
                }
                push_u32(&mut out, bytes.len() as u32);
                out.extend_from_slice(bytes);
            }
        }
        if visit.sum || visit.omit_comments {
            serialize_comments(&mut out, tree, &[]);
            serialize_comments(&mut out, tree, &[]);
            serialize_comments(&mut out, tree, &[]);
        } else {
            serialize_comments(&mut out, tree, &n.leading);
            serialize_comments(&mut out, tree, &n.trailing);
            serialize_comments(&mut out, tree, &n.ending);
        }
        let count = n.children.len();
        for i in (1..=count).rev() {
            let child_node = if increment && !visit.sum && i == 2 {
                Some(nid)
            } else {
                n.children[i - 1]
            };
            let child_spec_body = n.kind == SyntaxKind::Fn
                && i == 6
                && n.children
                    .get(4)
                    .copied()
                    .flatten()
                    .is_some_and(|spec| !tree.node(spec).children.is_empty());
            let first_is_int = n
                .children
                .first()
                .copied()
                .flatten()
                .is_some_and(|c| tree.node(c).kind == SyntaxKind::Int);
            let child_literal_step = n.kind == SyntaxKind::Postfix && i == 2 && first_is_int;
            let child_duration_name = n.kind == SyntaxKind::FieldAccess
                && i == count
                && (visit.literal_step || (count == 2 && first_is_int));
            stack.push(CompareVisit {
                node: child_node,
                sum: increment && !visit.sum && i == 2,
                omit_comments: visit.omit_comments || (visit.sum && i == 1),
                spec_body: child_spec_body,
                blank: visit.spec_body && i == 1,
                literal_step: child_literal_step,
                duration_name: child_duration_name,
            });
        }
    }
    out
}

/// Compares two surface syntax trees structurally, ignoring source byte spans.
#[must_use]
pub fn trees_equal(
    a: &SyntaxTree,
    a_root: NodeId,
    b: &SyntaxTree,
    b_root: NodeId,
    opts: TreeCompareOptions,
) -> bool {
    serialize_tree(a, a_root, opts) == serialize_tree(b, b_root, opts)
}
