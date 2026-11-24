#![forbid(unsafe_code)]

//! Pure-Rust `mo test` engine (`src/tools/{deadline,inputs*,fakes,builders,promises,recovery,service-tests,mutants,gates,strength,tests}.c`).
//!
//! Provides observation selection, deterministic type-directed property input generation and
//! shrinking, fake capability synthesis, hidden-type builder discovery & replay, service recovery
//! and service promise simulation, mutation testing gates (`--mutants`), and curated wrong-version
//! strength measurement (`--strength`).

pub mod anchors;
pub mod builders;
pub mod deadline;
pub mod fakes;
pub mod gates;
pub mod inputs;
pub mod mutants;
pub mod promises;
pub mod recovery;
pub mod render;
pub mod runner;
pub mod service_tests;
pub mod strength;

#[cfg(test)]
mod tests;

pub use anchors::{check_anchors, program_has_anchors, service_function};
pub use deadline::{ClockMode, Deadline, DeadlineState, set_thread_clock_mode};
pub use gates::{gate_limit, gate_start_failed, list_gates, render_gates_summary, run_gates};
pub use render::{
    GATE_KILL_KINDS, GATE_KILL_NAMES, MutantTotals, Observation, ObservationKind,
    PromiseResultReport, RecoveryReport, TestOutcome, TestResult, TestTotals, UNICODE16_METRICS,
    VacuityKind, detect_terminal_encoding, render_test_result, runtime_span, write_test_item,
    write_test_result,
};
pub use runner::{
    CheckOutcome, GatedCheck, GatedRun, TestConfig, TestSelection, append_function_name,
    instantiate_tests, run_gated, run_tests, select_tests, test_report_error,
};
pub use strength::run_strength;
