//! Unit tests for `crates/mo-cli`: CLI flag matrix, embedded docs, identity digest,
//! launcher script generation, formatting, fixing, emitting, running, and testing.

use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::cli::{Command, EmitKind, parse_cli, valid_install_name};
use crate::execute::execute_cli;
use crate::identity::{
    MO_EMBEDDED_GUIDE, MO_EMBEDDED_REFERENCE, MO_VERSION, MO_VERSION_LINE, runtime_identity_hex,
    version, version_line,
};
use crate::install::{build_launcher_script, derive_tool_name};

fn argv(words: &[&str]) -> Vec<Vec<u8>> {
    words.iter().map(|s| s.as_bytes().to_vec()).collect()
}

fn temp_dir(tag: &str) -> PathBuf {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let seq = COUNTER.fetch_add(1, Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("mo-cli-unit-{tag}-{}-{seq}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).expect("create temp dir");
    dir
}

#[test]
fn version_and_embedded_docs_nonempty() {
    assert_eq!(version(), MO_VERSION);
    assert_eq!(version_line(), MO_VERSION_LINE);
    assert_eq!(version_line(), "mo 0.4.2");
    assert!(!MO_EMBEDDED_GUIDE.is_empty());
    assert!(!MO_EMBEDDED_REFERENCE.is_empty());
    let hex = runtime_identity_hex().expect("runtime digest");
    assert_eq!(hex.len(), 64);
    assert!(hex.bytes().all(|b| b.is_ascii_hexdigit()));
}

#[test]
fn cli_parser_commands_flags_and_errors() {
    let v = parse_cli(&argv(&["mo", "--version"])).expect("version");
    assert_eq!(v.command, Command::Version);
    let v2 = parse_cli(&argv(&["mo", "-V"])).expect("short version");
    assert_eq!(v2.command, Command::Version);
    assert!(parse_cli(&argv(&["mo", "--version", "extra"])).is_err());

    let c = parse_cli(&argv(&["mo", "check", "--emit=core", "main.mo"])).expect("check core");
    assert_eq!(c.command, Command::Check);
    assert_eq!(c.emit, EmitKind::Core);
    assert_eq!(c.file(0), b"main.mo");

    assert!(parse_cli(&argv(&["mo", "check", "--json", "--emit=core", "main.mo"])).is_err());
    assert!(parse_cli(&argv(&["mo", "check", "--emit=bad", "main.mo"])).is_err());

    let r = parse_cli(&argv(&[
        "mo",
        "run",
        "--max-heap",
        "16MiB",
        "--max-stack",
        "2MiB",
        "--max-depth",
        "128",
        "app.mo",
        "--not-a-mo-flag",
        "arg2",
    ]))
    .expect("run");
    assert_eq!(r.command, Command::Run);
    assert_eq!(r.limits.heap_size, 16 << 20);
    assert_eq!(r.limits.stack_size, 2 << 20);
    assert_eq!(r.limits.call_depth, 128);
    assert_eq!(r.arguments, argv(&["--not-a-mo-flag", "arg2"]));

    // Non-UTF-8 unknown command escapes as \xNN on one line.
    let bad = vec![b"mo".to_vec(), vec![0xff, b'\n', b'"']];
    let err = parse_cli(&bad).expect_err("unknown non-utf8 command");
    assert!(err.message.contains("\\xff\\x0a\\x22"), "{}", err.message);

    // Format --stdin vs files.
    let fmt = parse_cli(&argv(&["mo", "format", "--stdin"])).expect("format stdin");
    assert!(fmt.stdin_mode);
    assert!(parse_cli(&argv(&["mo", "format", "--stdin", "a.mo"])).is_err());

    // Fetch exact syntax.
    let f = parse_cli(&argv(&[
        "mo",
        "fetch",
        "--admin-socket",
        "/tmp/a.sock",
        "--export",
        "failure",
        "42",
        "--out",
        "out.bin",
    ]))
    .expect("fetch");
    assert_eq!(f.command, Command::Fetch);
    assert_eq!(f.fetch_failure, 42);

    // Install tool name validation.
    assert!(valid_install_name(b"my_tool-1.2"));
    assert!(!valid_install_name(b"-bad"));
    assert!(!valid_install_name(b".bad"));
    assert!(!valid_install_name(b"a/b"));
}

#[test]
fn derive_name_and_launcher_script() {
    assert_eq!(
        derive_tool_name(b"foo/bar/hello.mo").as_deref(),
        Some("hello")
    );
    assert_eq!(
        derive_tool_name(b"my-tool/main.mo").as_deref(),
        Some("my-tool")
    );
    let script = build_launcher_script("demo", b"/usr/local/bin/mo", Some(b"/tmp/r'1"), None)
        .expect("launcher");
    let text = String::from_utf8(script).expect("utf8 script");
    assert!(text.starts_with("#!/bin/sh\n"));
    assert!(text.contains("'/tmp/r'\\''1'"));
    assert!(text.contains(".mo-libexec/demo.bc"));
}

#[test]
fn end_to_end_check_emit_run_format_fix_test_install() {
    let dir = temp_dir("e2e");
    let main_mo = dir.join("main.mo");
    let src = "\
module Main

fn double(n: Int) : Int
  examples
    double(4) == 8
  end
  n + n
end

test \"double works\"
  assert double(5) == 10
end

fn main() : Int
  double(21)
end
";
    fs::write(&main_mo, src).expect("write main.mo");
    let main_str = main_mo.to_str().expect("utf8 path");

    // 1. mo --version
    let mut out = Vec::new();
    let mut err = Vec::new();
    let cli = parse_cli(&argv(&["mo", "--version"])).expect("parse");
    assert_eq!(execute_cli(&cli, &mut out, &mut err), 0);
    assert_eq!(out, b"mo 0.4.2\n");
    assert!(err.is_empty());

    // 2. mo check
    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&["mo", "check", main_str])).expect("parse");
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        0,
        "err={}",
        String::from_utf8_lossy(&err)
    );
    assert!(out.is_empty() && err.is_empty());

    // 3. mo run (source)
    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&["mo", "run", main_str])).expect("parse");
    assert_eq!(execute_cli(&cli, &mut out, &mut err), 42);

    // 4. mo emit + mo run (bytecode)
    let bc_path = dir.join("out.bc");
    let bc_str = bc_path.to_str().expect("utf8 path");
    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&["mo", "emit", main_str, "-o", bc_str])).expect("parse");
    assert_eq!(execute_cli(&cli, &mut out, &mut err), 0);
    assert!(bc_path.exists());

    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&["mo", "run", bc_str])).expect("parse");
    assert_eq!(execute_cli(&cli, &mut out, &mut err), 42);

    // 5. mo test --list --json
    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&["mo", "test", "--list", "--json", main_str])).expect("parse");
    assert_eq!(execute_cli(&cli, &mut out, &mut err), 0);
    let listed = String::from_utf8(out.clone()).expect("utf8");
    assert!(listed.contains("\"record\":\"version\""));
    assert!(listed.contains("Main.double.example.1"));
    assert!(listed.contains("Main.test.1"));

    // 6. mo test
    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&["mo", "test", main_str])).expect("parse");
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        0,
        "out={} err={}",
        String::from_utf8_lossy(&out),
        String::from_utf8_lossy(&err)
    );
    let tested = String::from_utf8(out.clone()).expect("utf8");
    assert!(tested.contains("passed Main.double.example.1"), "{tested}");
    assert!(tested.contains("passed Main.test.1"), "{tested}");
    assert!(
        tested.contains("2 passed, 0 failed, 0 stopped\n"),
        "{tested}"
    );

    // 7. mo install
    let bin_dir = dir.join("bin");
    let bin_str = bin_dir.to_str().expect("utf8 path");
    out.clear();
    err.clear();
    let cli = parse_cli(&argv(&[
        "mo",
        "install",
        "--name",
        "doubler",
        "--bin-dir",
        bin_str,
        main_str,
    ]))
    .expect("parse");
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        0,
        "{}",
        String::from_utf8_lossy(&err)
    );
    assert!(bin_dir.join("doubler").exists());
    assert!(bin_dir.join(".mo-libexec/doubler.bc").exists());

    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn cli_help_flags_and_serve_root_hint() {
    // Top-level --help and -h exit 0 and print usage + subcommands.
    for flag in ["--help", "-h"] {
        let cli = parse_cli(&argv(&["mo", flag])).expect("top-level help");
        assert!(cli.help);
        let mut out = Vec::new();
        let mut err = Vec::new();
        assert_eq!(execute_cli(&cli, &mut out, &mut err), 0);
        assert!(err.is_empty());
        let text = String::from_utf8(out).expect("utf8 help");
        assert!(text.contains("usage: mo <command>"), "{text}");
        assert!(text.contains("serve"), "{text}");
        assert!(text.contains("install"), "{text}");
    }

    // Every subcommand supports --help and -h and exits 0 with accepted flags derived from FLAGS_TABLE.
    let subcommands = [
        ("check", "--emit=core|bytecode"),
        ("run", "--root DIR"),
        ("test", "--only NAME"),
        ("format", "--stdin"),
        ("fix", "--lib DIR"),
        ("guide", "--reference"),
        ("emit", "-o FILE"),
        ("serve", "--data DIR"),
        ("fetch", "--admin-socket PATH"),
        ("push", "--admin-socket PATH"),
        ("replay", "--failure ID"),
        ("query", "--max-work N"),
        ("rehearse", "--candidate FILE"),
        ("history", "--recover"),
        ("diff", "--json"),
        ("install", "--bin-dir DIR"),
        ("index", "--json"),
        ("sandbox-run", "--read-only"),
    ];
    for (cmd, expected_flag) in subcommands {
        for flag in ["--help", "-h"] {
            let cli = parse_cli(&argv(&["mo", cmd, flag])).expect("subcommand help");
            assert!(cli.help, "cmd={cmd} flag={flag}");
            let mut out = Vec::new();
            let mut err = Vec::new();
            assert_eq!(execute_cli(&cli, &mut out, &mut err), 0);
            assert!(err.is_empty());
            let text = String::from_utf8(out).expect("utf8 help");
            assert!(
                text.contains(&format!("usage: mo {cmd}")),
                "cmd={cmd} text={text}"
            );
            assert!(text.contains(expected_flag), "cmd={cmd} text={text}");
        }
    }

    // `mo run app.mo --help` still forwards --help to the program's Args capability.
    let run_cli = parse_cli(&argv(&["mo", "run", "app.mo", "--help"])).expect("run with arg");
    assert!(!run_cli.help);
    assert_eq!(run_cli.arguments, argv(&["--help"]));

    // `mo serve --root` and `mo serve --state-dir` include a concise one-line hint pointing to `--data DIR`.
    for wrong in ["--root", "--state-dir"] {
        let err = parse_cli(&argv(&["mo", "serve", wrong, "/tmp/data", "app.mo"]))
            .expect_err("serve rejects --root/--state-dir");
        assert!(
            err.message.starts_with(&format!(
                "flag is not accepted by this command: \"{wrong}\""
            )),
            "{}",
            err.message
        );
        assert!(err.message.contains("--data DIR"), "{}", err.message);
        assert!(!err.message.contains('\n'), "{}", err.message);
    }
}

#[test]
fn mo_index_emits_compiler_exact_symbols_edges_and_specs() {
    let dir = temp_dir("index");
    let main_path = dir.join("app.mo");
    fs::write(
        &main_path,
        "module App\n\n\
         struct User\n  id: Int\n  name: Text\nend\n\n\
         enum Role\n  Admin\n  Member(level: Int)\nend\n\n\
         fn helper(x: Int) : Int\n  examples\n    helper(2) == 4\n  end\n  promise \"doubles inputs\"\n    result == x * 2\n  end\n  x * 2\nend\n\n\
         fn greet(console: Console, u: User) : Int\n  v = helper(u.id)\n  console.print(\"hi #{u.name}\")\n  v\nend\n\n\
         test \"helper works\"\n  assert helper(3) == 6\nend\n",
    )
    .expect("write app.mo");

    let main_str = main_path.to_str().expect("utf8 path");
    let cli = parse_cli(&argv(&["mo", "index", "--json", main_str])).expect("parse index");
    let mut out = Vec::new();
    let mut err = Vec::new();
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        0,
        "out={} err={}",
        String::from_utf8_lossy(&out),
        String::from_utf8_lossy(&err)
    );
    let json = String::from_utf8(out).expect("utf8 index json");
    assert!(json.contains("\"schema_version\":1"), "{json}");
    assert!(json.contains("\"entry_module\":\"App\""), "{json}");
    assert!(json.contains("\"structs\":[{\"name\":\"User\""), "{json}");
    assert!(json.contains("\"enums\":[{\"name\":\"Role\""), "{json}");
    assert!(json.contains("\"name\":\"greet\""), "{json}");
    assert!(json.contains("\"capabilities\":[\"Console\"]"), "{json}");
    assert!(json.contains("\"calls\":[\"App.helper\"]"), "{json}");
    assert!(json.contains("\"examples\":1"), "{json}");
    assert!(json.contains("\"kind\":\"test\""), "{json}");
    assert!(json.contains("\"label\":\"helper works\""), "{json}");
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn mo_sandbox_run_enforces_manifest_readonly_and_max_work() {
    let dir = temp_dir("sandbox");
    let data_dir = dir.join("work");
    fs::create_dir_all(&data_dir).expect("create work dir");
    fs::write(data_dir.join("note.txt"), "hello sandbox").expect("write note.txt");

    // 1. ReadFiles + Console succeeds in --read-only --json.
    let ok_path = dir.join("sandbox-ok.mo");
    fs::write(
        &ok_path,
        "module SandboxOk\n\n\
         fn main(console: Console, rf: ReadFiles) : Int\n\
           case rf.read_text(\"note.txt\")\n\
             Ok(t): console.print(t)\n\
             Error(_): console.print(\"err\")\n\
           end\n\
           0\n\
         end\n",
    )
    .expect("write sandbox-ok.mo");
    let cli = parse_cli(&argv(&[
        "mo",
        "sandbox-run",
        "--read-only",
        "--json",
        "--root",
        data_dir.to_str().unwrap(),
        ok_path.to_str().unwrap(),
    ]))
    .expect("parse sandbox ok");
    let mut out = Vec::new();
    let mut err = Vec::new();
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        0,
        "out={} err={}",
        String::from_utf8_lossy(&out),
        String::from_utf8_lossy(&err)
    );
    let out_str = String::from_utf8(out).expect("utf8");
    assert!(out_str.contains("\"ok\":true"), "{out_str}");
    assert!(
        out_str.contains("\"stdout\":\"hello sandbox\\n\""),
        "{out_str}"
    );

    // 2. Files refused under --read-only.
    let files_path = dir.join("sandbox-files.mo");
    fs::write(
        &files_path,
        "module SandboxFiles\n\n\
         fn main(files: Files) : Int\n\
           case files.write_text(\"x\", \"y\")\n\
             Ok(_): 0\n\
             Error(_): 1\n\
           end\n\
         end\n",
    )
    .expect("write sandbox-files.mo");
    let cli = parse_cli(&argv(&[
        "mo",
        "sandbox-run",
        "--read-only",
        "--json",
        files_path.to_str().unwrap(),
    ]))
    .expect("parse sandbox files");
    let mut out = Vec::new();
    let mut err = Vec::new();
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        3,
        "out={} err={}",
        String::from_utf8_lossy(&out),
        String::from_utf8_lossy(&err)
    );
    let out_str = String::from_utf8(out).expect("utf8");
    assert!(
        out_str.contains("Files capability is forbidden in --read-only mode"),
        "{out_str}"
    );

    // 3. Command always refused.
    let cmd_path = dir.join("sandbox-cmd.mo");
    fs::write(
        &cmd_path,
        "module SandboxCmd\nuse Command\n\n\
         fn main(c: Command) : Int\n\
           case c.run(\"echo\", [\"hi\"], 1.second)\n\
             Ok(_): 0\n\
             Error(_): 1\n\
           end\n\
         end\n",
    )
    .expect("write sandbox-cmd.mo");
    let cli = parse_cli(&argv(&[
        "mo",
        "sandbox-run",
        "--json",
        cmd_path.to_str().unwrap(),
    ]))
    .expect("parse sandbox cmd");
    let mut out = Vec::new();
    let mut err = Vec::new();
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        3,
        "out={} err={}",
        String::from_utf8_lossy(&out),
        String::from_utf8_lossy(&err)
    );
    let out_str = String::from_utf8(out).expect("utf8");
    assert!(
        out_str.contains("Command capability is forbidden"),
        "{out_str}"
    );

    // 4. Network refused without --allow-hosts.
    let net_path = dir.join("sandbox-net.mo");
    fs::write(
        &net_path,
        "module SandboxNet\n\n\
         fn main(net: Network) : Int\n\
           _scoped = net.allow_hosts([\"example.com\"])\n\
           0\n\
         end\n",
    )
    .expect("write sandbox-net.mo");
    let cli = parse_cli(&argv(&[
        "mo",
        "sandbox-run",
        "--json",
        net_path.to_str().unwrap(),
    ]))
    .expect("parse sandbox net");
    let mut out = Vec::new();
    let mut err = Vec::new();
    assert_eq!(
        execute_cli(&cli, &mut out, &mut err),
        3,
        "out={} err={}",
        String::from_utf8_lossy(&out),
        String::from_utf8_lossy(&err)
    );
    let out_str = String::from_utf8(out).expect("utf8");
    assert!(
        out_str.contains("Network capability requires --allow-hosts"),
        "{out_str}"
    );

    // 5. --max-work stops infinite loops cleanly.
    let loop_path = dir.join("sandbox-loop.mo");
    fs::write(
        &loop_path,
        "module SandboxLoop\n\n\
         fn main() : Int\n\
           var i = 0\n\
           while i >= 0\n\
             i += 1\n\
           end\n\
           0\n\
         end\n",
    )
    .expect("write sandbox-loop.mo");
    let cli = parse_cli(&argv(&[
        "mo",
        "sandbox-run",
        "--max-work",
        "200",
        "--json",
        loop_path.to_str().unwrap(),
    ]))
    .expect("parse sandbox loop");
    let mut out = Vec::new();
    let mut err = Vec::new();
    assert_eq!(execute_cli(&cli, &mut out, &mut err), 70);
    let out_str = String::from_utf8(out).expect("utf8");
    assert!(out_str.contains("\"ok\":false"), "{out_str}");
    assert!(out_str.contains("\"exit_code\":70"), "{out_str}");

    let _ = fs::remove_dir_all(&dir);
}
