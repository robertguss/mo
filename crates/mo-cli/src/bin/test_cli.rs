#![forbid(unsafe_code)]

//! Pure-Rust CLI test helper matching `tests/test_cli.c` `--roundtrip-core` and
//! `--run-bytecode` helper flags so unmodified upstream `ci/cli_programs.py` and
//! `ci/cli_promises.py` run directly against pure-Rust codecs.

use std::env;
use std::fs;
use std::process::ExitCode;
use std::thread;

use mo_base::DiagBag;
use mo_bytecode::load;
use mo_core::{check_program, parse_program, print_program};
use mo_def::limits::MO_SOURCE_STACK_SIZE;
use mo_host::HostContext;
use mo_vm::{Image, Limits, Outcome, Process};

fn roundtrip_core(path: &str) -> bool {
    let Ok(bytes) = fs::read(path) else {
        return false;
    };
    let mut diags = DiagBag::new();
    let Some(parsed) = parse_program(&bytes, &mut diags) else {
        return false;
    };
    if diags.has_any() || !check_program(&parsed.arena, &parsed.value, &mut diags) {
        return false;
    }
    let Ok(printed) = print_program(&parsed.arena, &parsed.value) else {
        return false;
    };
    printed.as_bytes() == bytes.as_slice()
}

fn run_bytecode(path: &str) -> i32 {
    let Ok(bytes) = fs::read(path) else {
        return 70;
    };
    let Ok(file) = load(&bytes) else {
        return 70;
    };
    let limits = Limits::default();
    let Ok(image) = Image::init(file, limits) else {
        return 70;
    };
    let Some(entry) = image.find_main_entry() else {
        return 70;
    };
    let host = HostContext::stdio(Vec::new(), Vec::new());
    let mut process = Process::new(&image, limits, host);
    match process.call(entry, Vec::new()) {
        Outcome::Returned(v) => v.as_int() as i32,
        Outcome::Failed(_) | Outcome::Stopped(_) => 70,
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() == 3 && args[1] == "--roundtrip-core" {
        let path = args[2].clone();
        let ok = thread::Builder::new()
            .stack_size(MO_SOURCE_STACK_SIZE as usize)
            .spawn(move || roundtrip_core(&path))
            .ok()
            .and_then(|h| h.join().ok())
            .unwrap_or(false);
        return if ok {
            ExitCode::SUCCESS
        } else {
            ExitCode::from(1)
        };
    }
    if args.len() == 3 && args[1] == "--run-bytecode" {
        let code = run_bytecode(&args[2]);
        return ExitCode::from(u8::try_from(code.clamp(0, 255)).unwrap_or(70));
    }
    ExitCode::SUCCESS
}
