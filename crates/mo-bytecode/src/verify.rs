//! Ownership/borrow/token dataflow verifier seam.
//!
//! Structural decoding and cross-table validation run in `read` and `validate`.
//! Full dataflow verification (register type checking, linear ownership, borrow lifetimes,
//! token states, and `registers_readable` replay) lands in Slice 1B.

use crate::{BytecodeFile, LoadError};

pub fn verify_module(_file: &BytecodeFile) -> Result<(), LoadError> {
    Ok(())
}
