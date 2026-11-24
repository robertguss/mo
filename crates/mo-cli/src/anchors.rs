//! Promise anchor checking wrappers delegating to [`mo_test::anchors`].

use mo_base::{DiagBag, SourceMap};
use mo_check::CheckedProgram;
use mo_vm::Image;

use crate::cli::Cli;

pub use mo_test::anchors::{program_has_anchors, service_function};

/// Checks every function and service promise anchor in `image`, recording `E1405`
/// diagnostics into `diags` (`src/tools/run.c:mo_check_anchors`).
#[must_use]
pub fn check_anchors(
    image: &Image,
    cli: &Cli,
    sources: &SourceMap,
    program: &CheckedProgram,
    diags: &mut DiagBag,
) -> bool {
    mo_test::anchors::check_anchors(image, cli.limits, sources, program, diags)
}
