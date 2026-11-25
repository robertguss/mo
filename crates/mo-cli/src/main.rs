#![forbid(unsafe_code)]

//! Pure-Rust `mo` CLI entry point (`src/tools/main.c` parity).

use std::env;
use std::io::{self, Write};
use std::process::ExitCode;

use mo_cli::{execute_cli, parse_cli_os};

fn main() -> ExitCode {
    #[cfg(unix)]
    if let Some(code) = mo_host::confine::maybe_run_command_trampoline(env::args_os()) {
        return code;
    }
    #[cfg(unix)]
    mo_host::confine::register_self_as_trampoline();

    let mut stdout = io::stdout();
    let mut stderr = io::stderr();
    let cli = match parse_cli_os(env::args_os()) {
        Ok(c) => c,
        Err(err) => {
            let _ = writeln!(stderr, "{err}");
            let _ = stderr.flush();
            return ExitCode::from(2);
        }
    };
    let status = execute_cli(&cli, &mut stdout, &mut stderr);
    ExitCode::from(u8::try_from(status).unwrap_or(1))
}
