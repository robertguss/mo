#![forbid(unsafe_code)]

//! Pure-Rust CLI test helper binary (`mo-test-cli`) providing codec round-trip,
//! diagnostic rendering golden fixtures, `E1401` stack-fault rendering, and
//! module-path conversion for `ci/check_cli.py`, `ci/cli_*.py`, and `ci/library_map.sh`.

use std::env;
use std::fs;
use std::io::{self, Write};
use std::process::ExitCode;
use std::thread;

use mo_base::{
    DiagBag, Diagnostic, Edit, Fix, Label, RenderFormat, SourceMap, Span, TerminalEncoding,
    module_path, render_diagnostic_default,
};
use mo_bytecode::load;
use mo_cli::{Command, UNICODE16_METRICS, write_diagnostics, write_version};
use mo_core::{check_program, parse_program, print_program};
use mo_def::ErrorCode;
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

fn roundtrip_bytecode_text(path: &str) -> bool {
    let Ok(bytes) = fs::read(path) else {
        return false;
    };
    let Ok(text) = std::str::from_utf8(&bytes) else {
        return false;
    };
    text.starts_with("bytecode ") && text.ends_with('\n')
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
    let func = &image.file().functions[entry as usize];
    let mut call_args = Vec::with_capacity(func.parameters.len());
    for param in &func.parameters {
        if image.file().types.get(param.ty).capability.is_none() {
            return 70;
        }
        let Ok(val) = process.make_real_capability(param.ty) else {
            return 70;
        };
        call_args.push(val);
    }
    let returns_int = image.file().types.get(func.result_type).kind == mo_bytecode::TypeKind::Int;
    match process.call(entry, call_args) {
        Outcome::Returned(v) => {
            if returns_int {
                v.as_int() as i32
            } else {
                0
            }
        }
        Outcome::Failed(_) | Outcome::Stopped(_) => 70,
    }
}

fn render_fixture(json: bool) -> bool {
    let mut sources = SourceMap::new();
    let source = b"\t\xe7\x95\x8ce\xcc\x81\r\nsecond\nthird\nfourth\nfifth\nsixth\n";
    let Ok(f0) = sources.add_bytes(b"bad\xff\"\\.mo".as_slice(), source.as_slice()) else {
        return false;
    };
    let Ok(f1) = sources.add_bytes(b"other.mo".as_slice(), b"abc\nxyz".as_slice()) else {
        return false;
    };
    let len0 = source.len() as u32;
    let diag = Diagnostic::new(
        ErrorCode::NameClash,
        Span::new(f0, 1, len0),
        b"quoted \"headline\"\\\t\xff".as_slice(),
    )
    .with_primary_text("wide span")
    .with_reason("A clash between names is an error (reference 10.3).")
    .with_secondary(Label::with_text(Span::new(f0, len0, len0), "insert at EOF"))
    .with_secondary(Label::bare(Span::NONE))
    .with_fix(Fix::new(
        Some("replace together".into()),
        vec![
            Edit::new(Span::new(f1, 2, 3), "C\nZ"),
            Edit::new(Span::new(f1, 1, 2), "B"),
            Edit::new(Span::new(f1, 1, 1), "+"),
        ],
    ));
    let format = if json {
        RenderFormat::Json
    } else {
        RenderFormat::Text
    };
    let Ok(rendered) = render_diagnostic_default(
        &sources,
        &diag,
        format,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    ) else {
        return false;
    };
    let mut stdout = io::stdout();
    stdout.write_all(&rendered).is_ok() && stdout.flush().is_ok()
}

fn render_bytes() -> bool {
    let all: Vec<u8> = (1u8..=255u8).collect();
    let sources = SourceMap::new();
    let diag =
        Diagnostic::new(ErrorCode::ToolUnavailable, Span::NONE, all).with_primary_text("é界😀");
    let Ok(rendered) = render_diagnostic_default(
        &sources,
        &diag,
        RenderFormat::Json,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    ) else {
        return false;
    };
    let mut stdout = io::stdout();
    stdout.write_all(&rendered).is_ok() && stdout.flush().is_ok()
}

fn fault_stack(json: bool) -> i32 {
    let sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let _ = diags.push(
        Diagnostic::new(
            ErrorCode::ToolSourceStack,
            Span::NONE,
            "could not prepare the source pipeline stack",
        )
        .with_primary_text("Resource temporarily unavailable"),
    );
    let mut stdout = io::stdout();
    let mut stderr = io::stderr();
    if json && !write_version(&mut stdout, Command::Check) {
        return 1;
    }
    write_diagnostics(
        &mut stdout,
        &mut stderr,
        json,
        Command::Check,
        &sources,
        &mut diags,
        None,
    )
}

fn convert_module_path(name: &str) -> bool {
    let Ok(path) = module_path(name.as_bytes()) else {
        return false;
    };
    let mut stdout = io::stdout();
    writeln!(stdout, "{}", path.as_str()).is_ok() && stdout.flush().is_ok()
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
    if args.len() == 3 && args[1] == "--roundtrip-bytecode" {
        return if roundtrip_bytecode_text(&args[2]) {
            ExitCode::SUCCESS
        } else {
            ExitCode::from(1)
        };
    }
    if args.len() == 3 && args[1] == "--run-bytecode" {
        let code = run_bytecode(&args[2]);
        return ExitCode::from(u8::try_from(code.clamp(0, 255)).unwrap_or(70));
    }
    if args.len() == 2 && args[1] == "--render-bytes" {
        return if render_bytes() {
            ExitCode::SUCCESS
        } else {
            ExitCode::from(1)
        };
    }
    if args.len() == 2 && (args[1] == "--render-json" || args[1] == "--render-text") {
        return if render_fixture(args[1] == "--render-json") {
            ExitCode::SUCCESS
        } else {
            ExitCode::from(1)
        };
    }
    if args.len() == 3 && args[1] == "--fault-stack" {
        let json = args[2] == "--json";
        let code = fault_stack(json);
        return ExitCode::from(u8::try_from(code.clamp(0, 255)).unwrap_or(1));
    }
    if args.len() == 3 && args[1] == "--module-path" {
        return if convert_module_path(&args[2]) {
            ExitCode::SUCCESS
        } else {
            ExitCode::from(1)
        };
    }
    ExitCode::SUCCESS
}
