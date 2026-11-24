#![forbid(unsafe_code)]

//! Foundation types for the pure-Rust Mo front-end:
//! source map and spans, diagnostics and text/JSON renderers, CapCase-to-kebab
//! module paths, work budgets, and strict UTF-8 scalar decoding.

pub mod budget;
pub mod diag;
pub mod path;
pub mod render;
pub mod source;
pub mod utf8;

#[cfg(test)]
mod tests;

pub use budget::{BudgetExceeded, WorkBudget};
pub use diag::{
    DiagBag, DiagText, DiagValidationError, Diagnostic, Edit, Fix, Label, edits_overlap,
};
pub use mo_def::{ErrorCode, ErrorCodeInfo};
pub use path::{ModulePath, ModulePathError, module_path};
pub use render::{
    DisplayMetrics, RenderFormat, RenderLimitError, TerminalEncoding, render_diagnostic,
    render_diagnostic_default, render_totals_json, render_version_json,
};
pub use source::{LineCol, NO_FILE, SourceError, SourceFile, SourceMap, Span};
