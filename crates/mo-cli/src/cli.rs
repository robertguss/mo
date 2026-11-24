//! Pure-Rust CLI parser matching `src/tools/cli.c` byte-for-byte across all
//! 17 commands, all 44 flags, option budgets, value parsers, and cross-flag rules.

use std::borrow::Cow;
use std::ffi::OsStr;
use std::path::Path;

use mo_def::limits::{
    MO_CLI_ARGUMENT_BYTES, MO_CLI_ARGUMENT_SIZE, MO_CLI_ARGUMENTS, MO_DOOR_PLAN_INPUTS,
    MO_DOOR_PLAN_SEQUENCES, MO_LIMIT_API_WORKERS, MO_LIMIT_API_WORKERS_MAX, MO_LIMIT_CHECK_WALL_US,
    MO_LIMIT_INSTALL_NAME, MO_LIMIT_QUERY_WORK,
};
use mo_vm::Limits;
use thiserror::Error;

use crate::fs_util::{bytes_to_path, os_str_to_bytes};

/// All 17 top-level `mo` commands (`enum mo_command` in `src/tools/tools.h`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Command {
    Check = 0,
    Run = 1,
    Test = 2,
    Format = 3,
    Fix = 4,
    Guide = 5,
    Emit = 6,
    Serve = 7,
    Fetch = 8,
    Push = 9,
    Replay = 10,
    Query = 11,
    Rehearse = 12,
    History = 13,
    Diff = 14,
    Install = 15,
    Version = 16,
}

impl Command {
    const fn bit(self) -> u32 {
        1u32 << (self as u8)
    }

    /// Canonical lowercase name used in JSON version records (`src/tools/render.c:command_name`).
    #[must_use]
    pub const fn json_name(self) -> Option<&'static str> {
        match self {
            Self::Check => Some("check"),
            Self::Test => Some("test"),
            Self::Replay => Some("replay"),
            Self::Rehearse => Some("rehearse"),
            Self::Query => Some("query"),
            Self::History => Some("history"),
            Self::Diff => Some("diff"),
            Self::Push => Some("push"),
            Self::Run
            | Self::Format
            | Self::Fix
            | Self::Guide
            | Self::Emit
            | Self::Serve
            | Self::Fetch
            | Self::Install
            | Self::Version => None,
        }
    }

    /// Human-readable command name.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Check => "check",
            Self::Run => "run",
            Self::Test => "test",
            Self::Format => "format",
            Self::Fix => "fix",
            Self::Guide => "guide",
            Self::Emit => "emit",
            Self::Serve => "serve",
            Self::Fetch => "fetch",
            Self::Push => "push",
            Self::Replay => "replay",
            Self::Query => "query",
            Self::Rehearse => "rehearse",
            Self::History => "history",
            Self::Diff => "diff",
            Self::Install => "install",
            Self::Version => "--version",
        }
    }
}

/// Output mode selected by `--emit=...` on `mo check`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum EmitKind {
    #[default]
    None,
    Core,
    Bytecode,
}

/// Parsed command-line invocation (`struct mo_cli` in `src/tools/tools.h`).
///
/// Stores raw OS byte vectors for paths and forwarded arguments so non-UTF-8
/// filenames on Unix never panic or lose fidelity.
#[derive(Debug, Clone)]
pub struct Cli {
    pub command: Command,
    pub json: bool,
    pub list: bool,
    pub reference: bool,
    pub emit: EmitKind,
    pub limits: Limits,
    pub calls: u32,
    pub sequences: u32,
    pub seconds: u32,
    pub port: u16,
    pub has_port: bool,
    pub workers: u32,
    pub has_workers: bool,
    pub watch: bool,
    pub bind_host: Option<Vec<u8>>,
    pub data: Option<Vec<u8>>,
    pub allow_host: Option<Vec<u8>>,
    pub admin_socket: Option<Vec<u8>>,
    pub push_base: Option<Vec<u8>>,
    pub output: Option<Vec<u8>>,
    pub failure: u64,
    pub fetch_failure: u64,
    pub artifact: Option<Vec<u8>>,
    pub revised: Option<Vec<u8>>,
    pub recover: bool,
    pub max_work: u64,
    pub source_bundle: bool,
    pub upgrade_from: Option<Vec<u8>>,
    pub old_bundle: Option<Vec<u8>>,
    pub migration: Option<Vec<u8>>,
    pub upgrade: bool,
    pub adopt: bool,
    pub unconfined: bool,
    pub compile_context: Option<Vec<u8>>,
    pub candidate: Option<Vec<u8>>,
    pub strength: Option<Vec<u8>>,
    pub mutants: bool,
    pub door_plan: bool,
    pub library: Option<Vec<u8>>,
    pub root: Option<Vec<u8>>,
    pub state_dir: Option<Vec<u8>>,
    pub install_name: Option<Vec<u8>>,
    pub bin_dir: Option<Vec<u8>>,
    pub stdin_mode: bool,
    pub emit_tests: bool,
    pub module: Option<Vec<u8>>,
    pub only: Vec<Vec<u8>>,
    pub files: Vec<Vec<u8>>,
    pub function: Option<Vec<u8>>,
    pub arguments: Vec<Vec<u8>>,
}

impl Default for Cli {
    fn default() -> Self {
        Self {
            command: Command::Check,
            json: false,
            list: false,
            reference: false,
            emit: EmitKind::None,
            limits: Limits::default(),
            calls: 200,
            sequences: 200,
            seconds: 2,
            port: 0,
            has_port: false,
            workers: MO_LIMIT_API_WORKERS as u32,
            has_workers: false,
            watch: false,
            bind_host: None,
            data: None,
            allow_host: None,
            admin_socket: None,
            push_base: None,
            output: None,
            failure: 0,
            fetch_failure: 0,
            artifact: None,
            revised: None,
            recover: false,
            max_work: MO_LIMIT_QUERY_WORK,
            source_bundle: false,
            upgrade_from: None,
            old_bundle: None,
            migration: None,
            upgrade: false,
            adopt: false,
            unconfined: false,
            compile_context: None,
            candidate: None,
            strength: None,
            mutants: false,
            door_plan: false,
            library: None,
            root: None,
            state_dir: None,
            install_name: None,
            bin_dir: None,
            stdin_mode: false,
            emit_tests: false,
            module: None,
            only: Vec::new(),
            files: Vec::new(),
            function: None,
            arguments: Vec::new(),
        }
    }
}

impl Cli {
    /// Returns the `index`-th positional file argument as raw OS bytes.
    #[must_use]
    pub fn file(&self, index: usize) -> &[u8] {
        &self.files[index]
    }

    /// Returns the `index`-th positional file argument as an OS [`Path`].
    #[must_use]
    pub fn file_path(&self, index: usize) -> Cow<'_, Path> {
        bytes_to_path(&self.files[index])
    }
}

/// Error returned when CLI parsing rejects an invocation (exit status `2`).
#[derive(Debug, Clone, PartialEq, Eq, Error)]
#[error("{message}")]
pub struct CliParseError {
    pub message: String,
}

impl CliParseError {
    fn new(message: String) -> Self {
        Self { message }
    }
}

const ERROR_BUF_LIMIT: usize = 1023;

fn push_bounded(out: &mut String, text: &str) {
    for ch in text.chars() {
        let len = ch.len_utf8();
        if out.len() + len > ERROR_BUF_LIMIT {
            break;
        }
        out.push(ch);
    }
}

/// Quotes a bounded identifying prefix of arbitrary argv bytes onto one line
/// (`src/tools/cli.c:error_argument`).
fn push_error_argument(out: &mut String, arg: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    push_bounded(out, " \"");
    let prefix_len = arg.len().min(256);
    for &c in &arg[..prefix_len] {
        if !(0x20..0x7f).contains(&c) || c == b'"' || c == b'\\' {
            let esc = [
                b'\\',
                b'x',
                HEX[(c >> 4) as usize],
                HEX[(c & 0x0f) as usize],
            ];
            if let Ok(s) = std::str::from_utf8(&esc) {
                push_bounded(out, s);
            }
        } else {
            let buf = [c];
            if let Ok(s) = std::str::from_utf8(&buf) {
                push_bounded(out, s);
            }
        }
    }
    if arg.len() > 256 {
        push_bounded(out, "...");
    }
    push_bounded(out, "\"");
}

fn refuse(message: &str, argument: Option<&[u8]>, operand: Option<&[u8]>) -> CliParseError {
    let mut out = String::new();
    push_bounded(&mut out, message);
    if let Some(arg) = argument {
        push_bounded(&mut out, ":");
        push_error_argument(&mut out, arg);
    }
    if let Some(op) = operand {
        push_error_argument(&mut out, op);
    }
    CliParseError::new(out)
}

fn refuse_maximum(prefix: &str, maximum: u64, suffix: &str, arg: &[u8]) -> CliParseError {
    let mut out = String::new();
    push_bounded(&mut out, prefix);
    push_bounded(&mut out, &maximum.to_string());
    push_bounded(&mut out, suffix);
    push_bounded(&mut out, ":");
    push_error_argument(&mut out, arg);
    CliParseError::new(out)
}

fn check_option_budget(
    arg: &[u8],
    count: &mut u32,
    total: &mut usize,
) -> Result<(), CliParseError> {
    let length = arg.len();
    if (length as u64) > MO_CLI_ARGUMENT_SIZE {
        return Err(refuse_maximum(
            "command option is too long (maximum ",
            MO_CLI_ARGUMENT_SIZE,
            " bytes)",
            arg,
        ));
    }
    if u64::from(*count) == MO_CLI_ARGUMENTS {
        return Err(refuse_maximum(
            "too many command options (maximum ",
            MO_CLI_ARGUMENTS,
            " words)",
            arg,
        ));
    }
    *count += 1;
    *total = total.saturating_add(length + 1);
    if (*total as u64) > MO_CLI_ARGUMENT_BYTES {
        return Err(refuse_maximum(
            "command options are too large (maximum ",
            MO_CLI_ARGUMENT_BYTES,
            " bytes)",
            arg,
        ));
    }
    Ok(())
}

/// Positive decimal integer parser matching `src/tools/cli.c:number`.
fn parse_number(text: &[u8], bytes: bool, maximum: u64) -> Option<u64> {
    let mut value = 0u64;
    let mut i = 0usize;
    while i < text.len() && text[i].is_ascii_digit() {
        let digit = u64::from(text[i] - b'0');
        if value > (maximum - digit) / 10 {
            return None;
        }
        value = value * 10 + digit;
        i += 1;
    }
    if i == 0 || value == 0 {
        return None;
    }
    let scale = if i < text.len() {
        if !bytes {
            return None;
        }
        match &text[i..] {
            b"B" => 1u64,
            b"KiB" => 1u64 << 10,
            b"MiB" => 1u64 << 20,
            b"GiB" => 1u64 << 30,
            _ => return None,
        }
    } else {
        1u64
    };
    if value > maximum / scale {
        return None;
    }
    Some(value * scale)
}

fn parse_port(text: &[u8]) -> Option<u16> {
    if text.is_empty() {
        return None;
    }
    let mut value = 0u32;
    for &b in text {
        if !b.is_ascii_digit() {
            return None;
        }
        value = value * 10 + u32::from(b - b'0');
        if value > u32::from(u16::MAX) {
            return None;
        }
    }
    Some(value as u16)
}

fn parse_workers(text: &[u8]) -> Option<u32> {
    if text.is_empty() {
        return None;
    }
    let mut value = 0u32;
    for &b in text {
        if !b.is_ascii_digit() {
            return None;
        }
        value = value * 10 + u32::from(b - b'0');
        if u64::from(value) > MO_LIMIT_API_WORKERS_MAX {
            return None;
        }
    }
    if value == 0 { None } else { Some(value) }
}

fn is_valid_ipv4(text: &[u8]) -> bool {
    let mut i = 0usize;
    for part in 0..4 {
        if part > 0 {
            if text.get(i) != Some(&b'.') {
                return false;
            }
            i += 1;
        }
        if !text.get(i).is_some_and(u8::is_ascii_digit) {
            return false;
        }
        let start = i;
        let mut octet = 0u32;
        while i < text.len() && text[i].is_ascii_digit() {
            octet = octet * 10 + u32::from(text[i] - b'0');
            i += 1;
            if octet > 255 {
                return false;
            }
        }
        if i - start > 1 && text[start] == b'0' {
            return false;
        }
    }
    i == text.len()
}

fn parse_failure_id(text: &[u8]) -> Option<u64> {
    if text.is_empty() {
        return None;
    }
    let mut id = 0u64;
    let max = i64::MAX as u64;
    for &b in text {
        if !b.is_ascii_digit() {
            return None;
        }
        let digit = u64::from(b - b'0');
        if id > (max - digit) / 10 {
            return None;
        }
        id = id * 10 + digit;
    }
    if id == 0 { None } else { Some(id) }
}

/// Validates an installed tool name (`src/tools/cli.c:mo_valid_install_name`).
#[must_use]
pub fn valid_install_name(name: &[u8]) -> bool {
    if name.is_empty() || (name.len() as u64) > MO_LIMIT_INSTALL_NAME {
        return false;
    }
    let c0 = name[0];
    let ok0 = c0.is_ascii_alphanumeric() || c0 == b'_';
    if !ok0 {
        return false;
    }
    name[1..]
        .iter()
        .all(|&c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-' || c == b'.')
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum FlagId {
    Json = 0,
    List,
    Reference,
    Only,
    In,
    MaxStack,
    MaxDepth,
    MaxHeap,
    Calls,
    Seconds,
    Output,
    Port,
    Data,
    AllowHost,
    Emit,
    Failure,
    Export,
    Fix,
    Recover,
    MaxWork,
    Bundle,
    UpgradeFrom,
    OldBundle,
    Migrate,
    Upgrade,
    Adopt,
    Unconfined,
    Context,
    Artifact,
    Candidate,
    AdminSocket,
    Base,
    Strength,
    Mutants,
    Lib,
    Root,
    StateDir,
    Name,
    BinDir,
    Stdin,
    Bind,
    Workers,
    Watch,
    Tests,
}

impl FlagId {
    const fn bit(self) -> u64 {
        1u64 << (self as u8)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FlagValue {
    None,
    Text,
    Bytes,
    Count,
    Port,
    Workers,
    Id,
}

struct FlagSpec {
    id: FlagId,
    name: &'static [u8],
    prefix: bool,
    repeated: bool,
    value: FlagValue,
    maximum: u64,
    commands: u32,
}

const fn cmd_mask(cmds: &[Command]) -> u32 {
    let mut mask = 0u32;
    let mut i = 0;
    while i < cmds.len() {
        mask |= cmds[i].bit();
        i += 1;
    }
    mask
}

const FLAGS_TABLE: &[FlagSpec] = &[
    FlagSpec {
        id: FlagId::Json,
        name: b"--json",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[
            Command::Check,
            Command::Test,
            Command::Replay,
            Command::Query,
            Command::History,
            Command::Rehearse,
            Command::Push,
            Command::Diff,
        ]),
    },
    FlagSpec {
        id: FlagId::List,
        name: b"--list",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::Reference,
        name: b"--reference",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Guide]),
    },
    FlagSpec {
        id: FlagId::Only,
        name: b"--only",
        prefix: false,
        repeated: true,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::In,
        name: b"--in",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::MaxStack,
        name: b"--max-stack",
        prefix: false,
        repeated: false,
        value: FlagValue::Bytes,
        maximum: usize::MAX as u64,
        commands: cmd_mask(&[Command::Run, Command::Test, Command::Query]),
    },
    FlagSpec {
        id: FlagId::MaxDepth,
        name: b"--max-depth",
        prefix: false,
        repeated: false,
        value: FlagValue::Count,
        maximum: u32::MAX as u64,
        commands: cmd_mask(&[Command::Run, Command::Test, Command::Query]),
    },
    FlagSpec {
        id: FlagId::MaxHeap,
        name: b"--max-heap",
        prefix: false,
        repeated: false,
        value: FlagValue::Bytes,
        maximum: usize::MAX as u64,
        commands: cmd_mask(&[Command::Run, Command::Test, Command::Query]),
    },
    FlagSpec {
        id: FlagId::Calls,
        name: b"--calls",
        prefix: false,
        repeated: false,
        value: FlagValue::Count,
        maximum: u32::MAX as u64,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::Seconds,
        name: b"--seconds",
        prefix: false,
        repeated: false,
        value: FlagValue::Count,
        maximum: u32::MAX as u64,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::Output,
        name: b"-o",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Emit]),
    },
    FlagSpec {
        id: FlagId::Port,
        name: b"--port",
        prefix: false,
        repeated: false,
        value: FlagValue::Port,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Data,
        name: b"--data",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve, Command::Replay, Command::Query]),
    },
    FlagSpec {
        id: FlagId::AllowHost,
        name: b"--allow-host",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Emit,
        name: b"--emit=",
        prefix: true,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Check]),
    },
    FlagSpec {
        id: FlagId::Failure,
        name: b"--failure",
        prefix: false,
        repeated: false,
        value: FlagValue::Id,
        maximum: 0,
        commands: cmd_mask(&[Command::Replay, Command::Query]),
    },
    FlagSpec {
        id: FlagId::Export,
        name: b"--export",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Replay, Command::Query]),
    },
    FlagSpec {
        id: FlagId::Fix,
        name: b"--fix",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Replay]),
    },
    FlagSpec {
        id: FlagId::Recover,
        name: b"--recover",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Replay, Command::Query, Command::History]),
    },
    FlagSpec {
        id: FlagId::MaxWork,
        name: b"--max-work",
        prefix: false,
        repeated: false,
        value: FlagValue::Count,
        maximum: u64::MAX,
        commands: cmd_mask(&[Command::Query]),
    },
    FlagSpec {
        id: FlagId::Bundle,
        name: b"--bundle",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Emit, Command::Test]),
    },
    FlagSpec {
        id: FlagId::UpgradeFrom,
        name: b"--upgrade-from",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Check]),
    },
    FlagSpec {
        id: FlagId::OldBundle,
        name: b"--old-bundle",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Emit, Command::Test, Command::Rehearse]),
    },
    FlagSpec {
        id: FlagId::Migrate,
        name: b"--migrate",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[
            Command::Check,
            Command::Serve,
            Command::Emit,
            Command::Rehearse,
            Command::Push,
        ]),
    },
    FlagSpec {
        id: FlagId::Upgrade,
        name: b"--upgrade",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Adopt,
        name: b"--adopt",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Unconfined,
        name: b"--unconfined",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Context,
        name: b"--context",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Emit, Command::Test, Command::Rehearse]),
    },
    FlagSpec {
        id: FlagId::Artifact,
        name: b"--artifact",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Rehearse]),
    },
    FlagSpec {
        id: FlagId::Candidate,
        name: b"--candidate",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Rehearse]),
    },
    FlagSpec {
        id: FlagId::AdminSocket,
        name: b"--admin-socket",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve, Command::Push, Command::Query]),
    },
    FlagSpec {
        id: FlagId::Base,
        name: b"--base",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Push]),
    },
    FlagSpec {
        id: FlagId::Strength,
        name: b"--strength",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::Mutants,
        name: b"--mutants",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Test]),
    },
    FlagSpec {
        id: FlagId::Lib,
        name: b"--lib",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[
            Command::Check,
            Command::Run,
            Command::Test,
            Command::Fix,
            Command::Emit,
            Command::Serve,
            Command::Replay,
            Command::Query,
            Command::Push,
            Command::Diff,
            Command::Install,
        ]),
    },
    FlagSpec {
        id: FlagId::Root,
        name: b"--root",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Run, Command::Install]),
    },
    FlagSpec {
        id: FlagId::StateDir,
        name: b"--state-dir",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Run, Command::Install]),
    },
    FlagSpec {
        id: FlagId::Name,
        name: b"--name",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Install]),
    },
    FlagSpec {
        id: FlagId::BinDir,
        name: b"--bin-dir",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Install]),
    },
    FlagSpec {
        id: FlagId::Stdin,
        name: b"--stdin",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Format]),
    },
    FlagSpec {
        id: FlagId::Bind,
        name: b"--bind",
        prefix: false,
        repeated: false,
        value: FlagValue::Text,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Workers,
        name: b"--workers",
        prefix: false,
        repeated: false,
        value: FlagValue::Workers,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Watch,
        name: b"--watch",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Serve]),
    },
    FlagSpec {
        id: FlagId::Tests,
        name: b"--tests",
        prefix: false,
        repeated: false,
        value: FlagValue::None,
        maximum: 0,
        commands: cmd_mask(&[Command::Emit]),
    },
];

fn set_flag(
    out: &mut Cli,
    id: FlagId,
    arg: &[u8],
    operand: Option<&[u8]>,
    n: u64,
) -> Result<(), CliParseError> {
    let op_bytes = || operand.map(<[u8]>::to_vec);
    let op_slice = operand.unwrap_or(&[]);
    match id {
        FlagId::Artifact => out.artifact = op_bytes(),
        FlagId::Candidate => out.candidate = op_bytes(),
        FlagId::AdminSocket => out.admin_socket = op_bytes(),
        FlagId::Base => out.push_base = op_bytes(),
        FlagId::Mutants => out.mutants = true,
        FlagId::Lib => out.library = op_bytes(),
        FlagId::Root => out.root = op_bytes(),
        FlagId::StateDir => out.state_dir = op_bytes(),
        FlagId::Name => {
            if !valid_install_name(op_slice) {
                return Err(refuse(
                    "invalid tool name (expected ASCII letters, digits, '.', '_' or '-', \
                     starting with a letter, digit or '_')",
                    Some(arg),
                    operand,
                ));
            }
            out.install_name = op_bytes();
        }
        FlagId::BinDir => out.bin_dir = op_bytes(),
        FlagId::Stdin => out.stdin_mode = true,
        FlagId::Bind => {
            if !is_valid_ipv4(op_slice) {
                return Err(refuse(
                    "invalid bind address (expected an IPv4 address such as 127.0.0.1 or 0.0.0.0)",
                    Some(arg),
                    operand,
                ));
            }
            out.bind_host = op_bytes();
        }
        FlagId::Workers => {
            let Some(workers) = parse_workers(op_slice) else {
                return Err(refuse(
                    "invalid worker count (expected a decimal integer from 1 to 64)",
                    Some(arg),
                    operand,
                ));
            };
            out.workers = workers;
            out.has_workers = true;
        }
        FlagId::Watch => out.watch = true,
        FlagId::Tests => out.emit_tests = true,
        FlagId::Strength => out.strength = op_bytes(),
        FlagId::Context => out.compile_context = op_bytes(),
        FlagId::Bundle => out.source_bundle = true,
        FlagId::UpgradeFrom => out.upgrade_from = op_bytes(),
        FlagId::OldBundle => out.old_bundle = op_bytes(),
        FlagId::Migrate => out.migration = op_bytes(),
        FlagId::Upgrade => out.upgrade = true,
        FlagId::Adopt => out.adopt = true,
        FlagId::Unconfined => out.unconfined = true,
        FlagId::Json => out.json = true,
        FlagId::List => out.list = true,
        FlagId::Reference => out.reference = true,
        FlagId::Only => {
            if let Some(op) = op_bytes() {
                out.only.push(op);
            }
        }
        FlagId::In => out.module = op_bytes(),
        FlagId::MaxStack => out.limits.stack_size = n as usize,
        FlagId::MaxDepth => out.limits.call_depth = n as u32,
        FlagId::MaxHeap => out.limits.heap_size = n as usize,
        FlagId::Calls => out.calls = n as u32,
        FlagId::Seconds => out.seconds = n as u32,
        FlagId::Output => out.output = op_bytes(),
        FlagId::Port => {
            let Some(port) = parse_port(op_slice) else {
                return Err(refuse(
                    "invalid port (expected a decimal integer from 0 to 65535)",
                    Some(arg),
                    operand,
                ));
            };
            out.port = port;
            out.has_port = true;
        }
        FlagId::Data => out.data = op_bytes(),
        FlagId::AllowHost => out.allow_host = op_bytes(),
        FlagId::Emit => match &arg[7..] {
            b"core" => out.emit = EmitKind::Core,
            b"bytecode" => out.emit = EmitKind::Bytecode,
            _ => return Err(refuse("--emit must be core or bytecode", Some(arg), None)),
        },
        FlagId::Failure => {
            let Some(fid) = parse_failure_id(op_slice) else {
                return Err(refuse("failure id must be positive", Some(arg), operand));
            };
            out.failure = fid;
        }
        FlagId::Export => out.artifact = op_bytes(),
        FlagId::Fix => out.revised = op_bytes(),
        FlagId::Recover => out.recover = true,
        FlagId::MaxWork => out.max_work = n,
    }
    Ok(())
}

/// Parses an iterator of OS arguments (including `argv[0]`) into a [`Cli`].
pub fn parse_cli_os<I, S>(args: I) -> Result<Cli, CliParseError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let raw: Vec<Vec<u8>> = args
        .into_iter()
        .map(|s| os_str_to_bytes(s.as_ref()))
        .collect();
    parse_cli(&raw)
}

/// Parses raw OS byte argv (including `argv[0]`) into a [`Cli`], matching `src/tools/cli.c:mo_cli_parse`.
pub fn parse_cli(argv: &[Vec<u8>]) -> Result<Cli, CliParseError> {
    let mut out = Cli::default();
    if argv.len() < 2 {
        return Err(refuse(
            "expected a command: check, run, test, format, fix, guide, emit, serve, \
             fetch, push, replay, query, rehearse, history, diff or install",
            None,
            None,
        ));
    }
    let command = argv[1].as_slice();
    let mut count = 0u32;
    let mut total = 0usize;
    check_option_budget(command, &mut count, &mut total)?;
    // The command is length-bounded, but isn't an option word.
    count = 0;
    total = 0;

    if command == b"--version" || command == b"-V" {
        out.command = Command::Version;
        if argv.len() != 2 {
            return Err(refuse(
                "--version takes no other arguments",
                Some(&argv[2]),
                None,
            ));
        }
        return Ok(out);
    }

    if command == b"fetch" {
        out.command = Command::Fetch;
        let mut seen_socket = false;
        let mut seen_export = false;
        let mut seen_out = false;
        let fetch_usage =
            "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE";
        let mut i = 2usize;
        while i < argv.len() {
            let arg = argv[i].as_slice();
            check_option_budget(arg, &mut count, &mut total)?;
            if arg == b"--admin-socket" && !seen_socket && i + 1 < argv.len() {
                seen_socket = true;
                i += 1;
                let val = argv[i].as_slice();
                check_option_budget(val, &mut count, &mut total)?;
                if val.is_empty() || val[0] == b'-' {
                    return Err(refuse(fetch_usage, None, None));
                }
                out.admin_socket = Some(val.to_vec());
            } else if arg == b"--out" && !seen_out && i + 1 < argv.len() {
                seen_out = true;
                i += 1;
                let val = argv[i].as_slice();
                check_option_budget(val, &mut count, &mut total)?;
                if val.is_empty() || val[0] == b'-' {
                    return Err(refuse(fetch_usage, None, None));
                }
                out.output = Some(val.to_vec());
            } else if arg == b"--export" && !seen_export && i + 1 < argv.len() {
                seen_export = true;
                i += 1;
                let kind = argv[i].as_slice();
                check_option_budget(kind, &mut count, &mut total)?;
                if kind == b"state" {
                    out.fetch_failure = 0;
                } else if kind == b"failure" && i + 1 < argv.len() {
                    i += 1;
                    let id = argv[i].as_slice();
                    check_option_budget(id, &mut count, &mut total)?;
                    let Some(fid) = parse_failure_id(id) else {
                        return Err(refuse("failure id must be positive", None, None));
                    };
                    out.fetch_failure = fid;
                } else {
                    return Err(refuse(fetch_usage, None, None));
                }
            } else {
                return Err(refuse(fetch_usage, None, None));
            }
            i += 1;
        }
        if !seen_socket || !seen_export || !seen_out {
            return Err(refuse(fetch_usage, None, None));
        }
        return Ok(out);
    }

    const COMMAND_NAMES: &[(&[u8], Command)] = &[
        (b"check", Command::Check),
        (b"run", Command::Run),
        (b"test", Command::Test),
        (b"format", Command::Format),
        (b"fix", Command::Fix),
        (b"guide", Command::Guide),
        (b"emit", Command::Emit),
        (b"serve", Command::Serve),
        (b"replay", Command::Replay),
        (b"query", Command::Query),
        (b"rehearse", Command::Rehearse),
        (b"history", Command::History),
        (b"push", Command::Push),
        (b"diff", Command::Diff),
        (b"install", Command::Install),
    ];

    let Some((_, cmd)) = COMMAND_NAMES.iter().find(|(name, _)| *name == command) else {
        return Err(refuse(
            "unknown command (expected check, run, test, format, fix, guide, emit, serve, \
             fetch, push, replay, query, rehearse, history, diff or install)",
            Some(command),
            None,
        ));
    };
    out.command = *cmd;

    let mut flags = true;
    let mut seen = 0u64;
    let mut i = 2usize;
    while i < argv.len() {
        let arg = argv[i].as_slice();
        if flags && arg == b"--" {
            flags = false;
            i += 1;
            continue;
        }
        if flags && arg.first() == Some(&b'-') {
            check_option_budget(arg, &mut count, &mut total)?;
            let Some(spec) = FLAGS_TABLE.iter().find(|spec| {
                if spec.prefix {
                    arg.starts_with(spec.name)
                } else {
                    arg == spec.name
                }
            }) else {
                return Err(refuse("unknown flag", Some(arg), None));
            };
            if (spec.commands & out.command.bit()) == 0 {
                return Err(refuse(
                    "flag is not accepted by this command",
                    Some(arg),
                    None,
                ));
            }
            let bit = spec.id.bit();
            if !spec.repeated && (seen & bit) != 0 {
                return Err(refuse(
                    "flag may be given only once (except --only)",
                    Some(arg),
                    None,
                ));
            }
            seen |= bit;
            let operand = if spec.value != FlagValue::None {
                if i + 1 == argv.len() {
                    return Err(refuse("flag needs a nonempty value", Some(arg), None));
                }
                i += 1;
                let op = argv[i].as_slice();
                check_option_budget(op, &mut count, &mut total)?;
                let numeric = spec.value != FlagValue::Text;
                if op.is_empty() || (op[0] == b'-' && (!numeric || op.get(1) == Some(&b'-'))) {
                    return Err(refuse("flag needs a nonempty value", Some(arg), Some(op)));
                }
                Some(op)
            } else {
                None
            };
            let n = if matches!(spec.value, FlagValue::Bytes | FlagValue::Count) {
                let op = operand.unwrap_or(&[]);
                let Some(val) = parse_number(op, spec.value == FlagValue::Bytes, spec.maximum)
                else {
                    return Err(refuse(
                        "invalid limit (expected a positive decimal integer; bytes \
                         may use B, KiB, MiB or GiB)",
                        Some(arg),
                        operand,
                    ));
                };
                val
            } else {
                0
            };
            set_flag(&mut out, spec.id, arg, operand, n)?;
            i += 1;
            continue;
        }

        if arg.is_empty() {
            return Err(refuse("file name must not be empty", Some(arg), None));
        }
        if out.command == Command::Guide {
            return Err(refuse("guide takes no files", Some(arg), None));
        }
        if !out.files.is_empty()
            && matches!(
                out.command,
                Command::Check
                    | Command::Test
                    | Command::Emit
                    | Command::Serve
                    | Command::Replay
                    | Command::Push
                    | Command::Install
            )
        {
            return Err(refuse("command takes exactly one file", Some(arg), None));
        }
        if out.files.len() == 2 && out.command == Command::Diff {
            return Err(refuse("diff takes one or two files", Some(arg), None));
        }
        if !out.files.is_empty() && out.command == Command::History {
            return Err(refuse(
                "history takes exactly one data folder",
                Some(arg),
                None,
            ));
        }
        if out.files.len() == 2 && out.command == Command::Query {
            return Err(refuse(
                "query takes exactly one file and one function",
                Some(arg),
                None,
            ));
        }
        out.files.push(arg.to_vec());
        if out.command == Command::Run {
            out.arguments = argv[i + 1..].to_vec();
            break;
        }
        i += 1;
    }

    if out.command == Command::Format && out.stdin_mode && !out.files.is_empty() {
        return Err(refuse(
            "format --stdin takes no files",
            Some(b"--stdin"),
            None,
        ));
    }
    if out.command == Command::History && out.files.is_empty() {
        return Err(refuse("history needs a data folder", Some(command), None));
    }
    if !matches!(
        out.command,
        Command::Guide | Command::Serve | Command::Rehearse
    ) && out.files.is_empty()
        && !(out.command == Command::Format && out.stdin_mode)
        && !(out.command == Command::Replay && out.data.is_some())
    {
        return Err(refuse("command needs a file", Some(command), None));
    }
    if out.command == Command::Query && out.files.len() != 2 {
        return Err(refuse(
            "query needs a file and a function",
            Some(command),
            None,
        ));
    }
    if out.command == Command::Query {
        out.function = Some(out.files[1].clone());
    }
    let replay = out.command == Command::Replay;
    let query = out.command == Command::Query;
    if replay && out.failure == 0 {
        return Err(refuse("replay needs --failure ID", Some(command), None));
    }
    if replay && (out.data.is_none() == out.artifact.is_none()) {
        return Err(refuse(
            "give exactly one of --data DIR and --export ARTIFACT",
            Some(command),
            None,
        ));
    }
    let query_sources = u32::from(out.data.is_some())
        + u32::from(out.artifact.is_some())
        + u32::from(out.admin_socket.is_some());
    if query && query_sources != 1 {
        return Err(refuse(
            "give exactly one of --data DIR, --export ARTIFACT and --admin-socket PATH",
            Some(command),
            None,
        ));
    }
    if query && out.data.is_some() && out.failure == 0 {
        return Err(refuse(
            "a query over a data folder needs --failure ID",
            Some(command),
            None,
        ));
    }
    if out.recover && (out.artifact.is_some() || out.admin_socket.is_some()) {
        return Err(refuse(
            "--recover reads a data folder, not --export or --admin-socket",
            Some(b"--recover"),
            None,
        ));
    }
    if out.command == Command::Emit && out.output.is_none() {
        return Err(refuse("emit needs -o FILE", Some(command), None));
    }
    if out.emit_tests && out.source_bundle {
        return Err(refuse(
            "--tests and --bundle cannot be combined",
            Some(b"--tests"),
            Some(b"--bundle"),
        ));
    }

    let planned = FlagId::List.bit() | FlagId::Only.bit() | FlagId::In.bit() | FlagId::Calls.bit();
    let limited = FlagId::Only.bit()
        | FlagId::In.bit()
        | FlagId::Calls.bit()
        | FlagId::Seconds.bit()
        | FlagId::MaxStack.bit()
        | FlagId::MaxDepth.bit()
        | FlagId::MaxHeap.bit();

    if out.mutants && (out.source_bundle || out.strength.is_some() || (seen & limited) != 0) {
        return Err(refuse(
            "--mutants runs every check and tries every mutant; it takes no \
             --only, --in, --calls, --seconds, --max-stack, --max-depth, --max-heap, \
             --bundle or --strength",
            Some(b"--mutants"),
            None,
        ));
    }
    let checks = out.command == Command::Test && out.source_bundle;
    if checks
        && (out.json || out.strength.is_some() || (seen & (planned | FlagId::Seconds.bit())) != 0)
    {
        return Err(refuse(
            "test --bundle runs upgrade checks over the whole bundle; \
             it takes no --list, --json, --only, --in, --calls, --seconds or --strength",
            Some(b"--bundle"),
            None,
        ));
    }
    if checks {
        out.seconds = (MO_LIMIT_CHECK_WALL_US / 1_000_000) as u32;
    }
    if out.command == Command::Serve && out.files.is_empty() {
        if (seen & FlagId::Port.bit()) == 0 {
            return Err(refuse("serve needs --port N", Some(command), None));
        }
        if (seen & FlagId::Data.bit()) == 0 {
            return Err(refuse("serve needs --data DIR", Some(command), None));
        }
    }
    if out.command == Command::Serve
        && out.files.is_empty()
        && (out.upgrade || out.adopt || out.migration.is_some())
    {
        return Err(refuse(
            "upgrade or adoption needs a source file",
            Some(command),
            None,
        ));
    }
    if out.command == Command::Serve
        && out.files.is_empty()
        && (out.bind_host.is_some() || out.has_workers || out.watch)
    {
        return Err(refuse(
            "--bind, --workers and --watch apply only to api servers",
            Some(command),
            None,
        ));
    }
    if out.command == Command::Push && out.admin_socket.is_none() {
        return Err(refuse(
            "usage: mo push FILE --admin-socket PATH [--base DIGEST] \
             [--migrate Module.function] [--json]",
            Some(command),
            None,
        ));
    }
    if out.migration.is_some()
        && out.command != Command::Push
        && !(out.upgrade
            || out.upgrade_from.is_some()
            || out.old_bundle.is_some()
            || out.compile_context.is_some())
    {
        return Err(refuse(
            "--migrate needs --upgrade or an old program",
            Some(b"--migrate"),
            None,
        ));
    }
    if out.command == Command::Rehearse
        && (!out.files.is_empty()
            || out.artifact.is_none()
            || out.old_bundle.is_none()
            || out.candidate.is_none())
    {
        return Err(refuse(
            "rehearse needs --artifact, --old-bundle and --candidate, and no files",
            Some(command),
            None,
        ));
    }
    if out.old_bundle.is_some() && !out.source_bundle && out.command != Command::Rehearse {
        return Err(refuse(
            "--old-bundle needs --bundle",
            Some(b"--old-bundle"),
            None,
        ));
    }
    if out.json && out.emit != EmitKind::None {
        return Err(refuse(
            "--json and --emit cannot be combined",
            Some(b"--json"),
            Some(b"--emit"),
        ));
    }
    if out.strength.is_some() && (seen & planned) != 0 {
        return Err(refuse(
            "--strength runs a fixed plan over the whole program; it takes no --list, \
             --only, --in or --calls",
            Some(b"--strength"),
            None,
        ));
    }
    out.door_plan = out.strength.is_some() || checks;
    if out.door_plan {
        out.calls = MO_DOOR_PLAN_INPUTS as u32;
    }
    out.sequences = if out.door_plan {
        MO_DOOR_PLAN_SEQUENCES as u32
    } else {
        out.calls
    };
    Ok(out)
}
