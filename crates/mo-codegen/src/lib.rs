#![forbid(unsafe_code)]

//! Pure-Rust Mo Core → `.bc` v0.3 bytecode compiler with whole-program borrow inference,
//! Perceus reference counting & in-place reuse (`RESET`/`REUSE`/`FREE_TOKEN`/`SET_FIELD`),
//! reachability pruning, and block-local exact-type register recycling.

mod bigint;
mod compiler;
mod control;
mod generate;
mod operations;
mod ownership;
mod prepare;
mod program;
mod registers;
mod types;

#[cfg(test)]
mod tests;

use mo_base::{DiagBag, Diagnostic, Span};
use mo_core::{CoreArena, Program, Services, check_program_services};
use mo_def::ErrorCode;

pub use crate::bigint::{BigIntWorkLimit, convert_bigint};
pub use crate::compiler::CodegenError;

/// Compiles a Mo Core [`Program`] to verified `.bc` v0.3 bytes without services,
/// recording any Core checker or codegen diagnostic into `diags`.
pub fn compile(arena: &CoreArena, core: &Program, diags: &mut DiagBag) -> Option<Vec<u8>> {
    compile_services(arena, core, None, diags)
}

/// Compiles a Mo Core [`Program`] (with optional service descriptors) to verified `.bc` v0.3 bytes,
/// matching `mo_compile_services` (`src/codegen/compile.c`).
pub fn compile_services(
    arena: &CoreArena,
    core: &Program,
    services: Option<&Services>,
    diags: &mut DiagBag,
) -> Option<Vec<u8>> {
    if !check_program_services(arena, core, services, diags) {
        return None;
    }
    match compile_checked(arena, core, services) {
        Ok(bytes) => Some(bytes),
        Err(err) => {
            let label = match &err.function_name {
                Some(name) => format!("{name}: {}", err.reason),
                None => err.reason.to_string(),
            };
            let diag = Diagnostic::new(err.code, err.span, err.message)
                .with_reason(err.reason)
                .with_primary_text(label);
            let _ = diags.push(diag);
            None
        }
    }
}

/// Compiles an already Core-checked program to `.bc` v0.3 bytes, returning a typed [`CodegenError`]
/// on refusal.
pub fn compile_checked(
    arena: &CoreArena,
    core: &Program,
    services: Option<&Services>,
) -> Result<Vec<u8>, CodegenError> {
    let mut c = compiler::Compiler::new(arena, core, services);
    let ok = c.declarations()
        && c.setup_globals()
        && c.prepare_bodies()
        && c.entry_valid()
        && c.abilities_valid()
        && c.borrow_infer();
    let bytes = if ok { c.emit_all() } else { None };
    if let Some(out) = bytes {
        return Ok(out);
    }
    let (code, reason) = c
        .error
        .unwrap_or((ErrorCode::CodegenInvalid, "code generation failed"));
    let span = c.span.or(c.declaration).unwrap_or(Span::NONE);
    Err(CodegenError {
        code,
        message: CodegenError::headline_for(code),
        reason,
        function_name: c.current_name,
        span,
    })
}
