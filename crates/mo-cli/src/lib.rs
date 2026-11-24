#![forbid(unsafe_code)]

//! Unified pure-Rust `mo` CLI library and binary (`crates/mo-cli`).
//!
//! Connects the Phase 2 pure-Rust front end (`mo-base`, `mo-syntax`, `mo-check`,
//! `mo-lower`, `mo-core`), Slice 3A pure-Rust codegen (`mo-codegen`, `mo-bytecode`),
//! and Phase 1 pure-Rust runtime (`mo-def`, `mo-heap`, `mo-prim`, `mo-host`, `mo-vm`)
//! into a standalone `mo` executable while exposing a clean library surface for
//! Slices 3C (`mo-test`), 3D (`mo-service`), and 3E (`mo-upgrade`).

pub mod anchors;
pub mod cli;
pub mod execute;
pub mod fix;
pub mod format;
pub mod fs_util;
pub mod identity;
pub mod install;
pub mod render;
pub mod run;
pub mod test_cmd;

#[cfg(test)]
mod tests;

pub use cli::{Cli, CliParseError, Command, EmitKind, parse_cli, parse_cli_os, valid_install_name};
pub use execute::{execute_cli, resolve_library_dir};
pub use identity::{
    DEFAULT_BUILD_LIBRARY_ROOT, IdentityError, MO_EMBEDDED_GUIDE, MO_EMBEDDED_REFERENCE,
    MO_VERSION, MO_VERSION_LINE, default_library_root, executable_path, runtime_identity_digest,
    runtime_identity_hex, version, version_line,
};
pub use render::{
    Observation, ObservationKind, TestOutcome, TestResult, TestTotals, UNICODE16_METRICS,
    detect_terminal_encoding, render_test_result, runtime_span, write_defect, write_diagnostics,
    write_test_item, write_test_result, write_version,
};
