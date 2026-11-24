#![forbid(unsafe_code)]

//! Pure-Rust Mo surface lexer, recursive-descent parser with exact error recovery,
//! comment/trivia attachment, and 100-column Wadler source formatter.
//!
//! Parity target: `src/syntax/{syntax.h,internal.h,lexer.c,parser.c,comments.c,format-doc.c,format.c}`.
//! Depends only on `mo-def` and `mo-base` (the formatter accepts a display-width callback so
//! `mo-syntax` never depends on `mo-prim`).

pub mod ast;
mod comments;
pub mod format;
mod format_doc;
pub mod lexer;
pub mod parser;

#[cfg(test)]
mod tests;

pub use ast::{
    CommentAnchor, CommentId, DURATION_UNITS, Header, ModuleSyntax, NodeId, ParsedHeader,
    SyntaxComment, SyntaxKind, SyntaxNode, SyntaxTree, TextSlice, TokenKind, TreeCompareOptions,
    duration_unit, trees_equal,
};
pub use format::{DisplayWidthFn, FormatError, FormatOptions, format_module};
pub use parser::{
    ParseOptions, parse_fragment, parse_fragment_with_options, parse_module,
    parse_module_with_options, read_header, read_header_with_options,
};
