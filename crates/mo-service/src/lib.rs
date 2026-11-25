#![forbid(unsafe_code)]

//! Durable Mo service runtime, SQLite store, MCP/HTTP protocol server, operator views,
//! immutable export/fetch, offline history, live upgrades, and multi-worker `api` server.

pub mod api_server;
pub mod binary;
pub mod fetch;
pub mod history;
pub mod listener;
pub mod protocol;
pub mod serve;
pub mod service_loop;
pub mod store;
pub mod upgrade;

use std::any::Any;
use std::panic::{AssertUnwindSafe, catch_unwind};

use mo_heap::Value;
use mo_vm::{Defect, Outcome, Process};

/// Extracts a concise human-readable message from a caught panic payload.
pub(crate) fn panic_payload_message(payload: Box<dyn Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unexpected internal panic".to_string()
    }
}

/// Runs `proc.call_with_budget` inside `catch_unwind` so an unexpected panic in a handler
/// cannot bring down the worker thread or `mo serve` process.
pub(crate) fn call_with_budget_isolated(
    proc: &mut Process<'_>,
    fn_idx: u32,
    args: Vec<Value>,
    budget: u64,
) -> (Outcome, u64) {
    match catch_unwind(AssertUnwindSafe(|| {
        proc.call_with_budget(fn_idx, args, budget)
    })) {
        Ok(res) => res,
        Err(payload) => {
            let msg = panic_payload_message(payload);
            (
                Outcome::Stopped(Defect {
                    reason: format!("internal panic in handler: {msg}"),
                    limit_name: None,
                    limit: 0,
                    detail: None,
                    frames: Vec::new(),
                    variables: Vec::new(),
                    variables_unavailable: true,
                }),
                0,
            )
        }
    }
}

#[cfg(test)]
mod panic_isolation_tests {
    use super::*;

    #[test]
    fn catch_unwind_isolates_handler_panics() {
        let res = catch_unwind(AssertUnwindSafe(|| -> i32 {
            #[expect(
                clippy::panic,
                reason = "intentional panic to verify panic-isolation helper extracts message cleanly"
            )]
            {
                panic!("simulated handler bug");
            }
        }));
        let msg = panic_payload_message(res.expect_err("must catch panic"));
        assert_eq!(msg, "simulated handler bug");
    }
}
