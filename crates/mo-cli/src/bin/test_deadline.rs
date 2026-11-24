#![forbid(unsafe_code)]

//! Pure-Rust black-box deadline test driver matching `tests/test_deadline.c` CLI modes
//! (`fixed`, `expiry`, `failure`, `start-failure`) so unmodified upstream `ci/cli_promises.py`
//! runs directly against the pure-Rust `mo` engine.

use std::env;
use std::ffi::OsString;
use std::io::{self, Write};
use std::process::ExitCode;

use mo_cli::{execute_cli, parse_cli_os};
use mo_test::{ClockMode, set_thread_clock_mode};

fn main() -> ExitCode {
    let args: Vec<OsString> = env::args_os().collect();
    if args.len() <= 1 {
        return ExitCode::SUCCESS;
    }
    let mode_str = args[1].to_string_lossy();
    let mode = match mode_str.as_ref() {
        "start-failure" => ClockMode::StartFailure,
        "fixed" => ClockMode::Fixed,
        "failure" => ClockMode::Failure,
        _ => ClockMode::Expiry,
    };
    set_thread_clock_mode(mode);

    let cli = match parse_cli_os(&args[1..]) {
        Ok(cli) => cli,
        Err(err) => {
            let mut stderr = io::stderr();
            let _ = writeln!(stderr, "{err}");
            let _ = stderr.flush();
            return ExitCode::from(2);
        }
    };
    let mut stdout = io::stdout();
    let mut stderr = io::stderr();
    let status = execute_cli(&cli, &mut stdout, &mut stderr);
    ExitCode::from(u8::try_from(status.clamp(0, 255)).unwrap_or(1))
}
