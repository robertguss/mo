#![forbid(unsafe_code)]

//! Pure-Rust structural diffing, offline failure replay & state query, and upgrade rehearsal
//! (`src/tools/diff.c`, `src/tools/offline*.c`, `src/tools/upgrade-commands.c:mo_upgrade_rehearse`,
//! `src/vm/offline.c`, and `src/base/verdict.c` parity).

pub mod diff;
pub mod offline;
pub mod offline_output;
pub mod rehearse;

pub use diff::{DiffOptions, run_diff};
pub use offline::{QueryOptions, ReplayOptions, run_query, run_replay};
pub use rehearse::{RehearseOptions, run_rehearse};
