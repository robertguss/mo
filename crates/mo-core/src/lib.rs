#![forbid(unsafe_code)]

//! Mo Core intermediate representation, lossless `.core` v0.11 S-expression
//! codec (all 16 document kinds), compiler metadata validator, and bidirectional
//! Core typechecker matching `formal/MoCore` and `src/core/{text,metadata,check}.c`.

pub mod check;
pub mod codec;
pub mod ir;
pub mod metadata;

#[cfg(test)]
mod tests;

pub use check::{check_program, check_program_services, check_program_with_scratch_limit};
pub use codec::{
    CorePrintError, detect_kind, parse_document, parse_document_with_scratch_limit, parse_program,
    parse_run, print_document, print_program, print_run,
};
pub use ir::*;
pub use metadata::metadata_valid;
pub use mo_def::MO_CORE_TEXT_VERSION;
