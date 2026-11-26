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
use std::fs;
use std::io::Write;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use mo_heap::Value;
use mo_vm::{Defect, Outcome, Process};

fn format_utc_rfc3339_now() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0u64, |d| d.as_secs());
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let hour = rem / 3600;
    let min = (rem % 3600) / 60;
    let sec = rem % 60;
    // Howard Hinnant civil-from-days (1970-01-01 = day 0).
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let mut y = (yoe as i64) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    if m <= 2 {
        y += 1;
    }
    format!("{y:04}-{m:02}-{d:02}T{hour:02}:{min:02}:{sec:02}Z")
}

/// Atomically writes a server readiness JSON file (`url`, `port`, `pid`, `data_dir`, `started_at`)
/// via a sibling exclusive temporary file (`create_new`, refusing symlinks) + `sync_all` + `rename`
/// after the HTTP listener is bound.
pub fn write_ready_file(
    ready_path: &Path,
    url: &str,
    port: u16,
    data_dir: &Path,
) -> Result<(), String> {
    let resolved_data = fs::canonicalize(data_dir).unwrap_or_else(|_| data_dir.to_path_buf());
    let data_str = resolved_data.to_string_lossy();
    let pid = std::process::id();
    let started_at = format_utc_rfc3339_now();

    let mut json = String::with_capacity(256);
    json.push_str("{\"url\":");
    binary::push_json_string(&mut json, url);
    json.push_str(&format!(",\"port\":{port},\"pid\":{pid},\"data_dir\":"));
    binary::push_json_string(&mut json, &data_str);
    json.push_str(",\"started_at\":");
    binary::push_json_string(&mut json, &started_at);
    json.push_str("}\n");

    let parent = match ready_path.parent() {
        Some(p) if !p.as_os_str().is_empty() => p,
        _ => Path::new("."),
    };
    fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0u128, |d| d.as_nanos());
    let tmp_path = parent.join(format!(".mo-ready-{pid}-{nanos}.tmp"));
    publish_ready_file(ready_path, &tmp_path, json.as_bytes())
}

fn publish_ready_file(ready_path: &Path, tmp_path: &Path, json: &[u8]) -> Result<(), String> {
    let mut f = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(tmp_path)
        .map_err(|e| format!("{}: {e}", tmp_path.display()))?;
    let write_res = (|| -> std::io::Result<()> {
        f.write_all(json)?;
        f.sync_all()?;
        drop(f);
        fs::rename(tmp_path, ready_path)
    })();
    if let Err(e) = write_res {
        let _ = fs::remove_file(tmp_path);
        return Err(format!("{}: {e}", ready_path.display()));
    }
    Ok(())
}

/// Removes a stale readiness file before server bind if present, ignoring `NotFound`.
pub fn clear_ready_file(ready_path: &Path) {
    let _ = fs::remove_file(ready_path);
}

/// RAII guard removing a readiness file on clean server exit.
pub struct ReadyFileGuard<'a>(pub Option<&'a Path>);

impl Drop for ReadyFileGuard<'_> {
    fn drop(&mut self) {
        if let Some(p) = self.0 {
            let _ = fs::remove_file(p);
        }
    }
}

#[cfg(test)]
mod ready_file_tests {
    use super::*;

    #[test]
    fn temporary_file_collision_preserves_the_existing_file() -> std::io::Result<()> {
        let dir = std::env::temp_dir().join(format!("mo-ready-collision-{}", std::process::id()));
        fs::create_dir_all(&dir)?;
        let ready = dir.join("ready.json");
        let tmp = dir.join("ready.tmp");
        fs::write(&tmp, b"other publisher")?;

        let publish = publish_ready_file(&ready, &tmp, b"{}");
        let contents = fs::read(&tmp);
        fs::remove_dir_all(&dir)?;

        assert!(publish.is_err());
        assert_eq!(contents?, b"other publisher");
        Ok(())
    }
}

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
