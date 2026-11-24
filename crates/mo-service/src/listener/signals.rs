//! Process signal handling (`SIGTERM` / `SIGINT`) for clean server shutdown.

use std::sync::Arc;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

static STOP_FLAG: OnceLock<Arc<AtomicBool>> = OnceLock::new();

fn flag() -> &'static Arc<AtomicBool> {
    STOP_FLAG.get_or_init(|| Arc::new(AtomicBool::new(false)))
}

/// Installs process-wide `SIGTERM` and `SIGINT` handlers that set the shutdown flag.
/// Safe to call repeatedly; handlers are registered once.
pub fn install_signal_handlers() -> std::io::Result<()> {
    static INSTALLED: OnceLock<Result<(), String>> = OnceLock::new();
    let res = INSTALLED.get_or_init(|| {
        #[cfg(unix)]
        {
            let f = Arc::clone(flag());
            signal_hook::flag::register(signal_hook::consts::SIGTERM, Arc::clone(&f))
                .map_err(|e| e.to_string())?;
            signal_hook::flag::register(signal_hook::consts::SIGINT, f)
                .map_err(|e| e.to_string())?;
        }
        Ok(())
    });
    match res {
        Ok(()) => Ok(()),
        Err(msg) => Err(std::io::Error::other(msg.clone())),
    }
}

/// Returns `true` if `SIGTERM` or `SIGINT` has been received.
#[must_use]
pub fn is_signalled() -> bool {
    flag().load(Ordering::Relaxed)
}

/// Resets the shutdown flag (primarily for tests or fresh starts).
pub fn reset_signal() {
    flag().store(false, Ordering::Relaxed);
}
