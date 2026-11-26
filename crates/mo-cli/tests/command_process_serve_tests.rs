#![forbid(unsafe_code)]

//! Integration tests for:
//! 1. `command.allow_loopback()` permitting loopback TCP (`127.0.0.1` / `::1`) inside Linux
//!    Landlock + Seccomp confinement while keeping all non-loopback TCP and UDP blocked (and
//!    keeping loopback blocked when `allow_loopback()` is not called).
//! 2. `command.spawn`, `proc.pid`, `proc.alive?`, `proc.kill()`, `proc.wait(within)`, and
//!    automatic child kill + reap when a live `Command.Process` handle is dropped without `wait`.
//! 3. `mo serve --port 0 --ready-file PATH` binding an ephemeral OS port, atomically writing the
//!    readiness JSON (`url`, `port`, `pid`, `data_dir`, `started_at`), answering live HTTP
//!    requests, and shutting down cleanly.
//! 4. Rejecting `Command.Process` on `fn main` or inside `service` state.

#[cfg(all(unix, test))]
mod unix_tests {
    use std::error::Error;
    use std::fs;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{Duration, Instant};

    static SEQ: AtomicU64 = AtomicU64::new(1);

    fn mo_bin() -> PathBuf {
        PathBuf::from(env!("CARGO_BIN_EXE_mo"))
    }

    fn temp_dir(tag: &str) -> Result<PathBuf, Box<dyn Error>> {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "mo-cmd-proc-serve-{tag}-{}-{n}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir)?;
        Ok(dir)
    }

    #[test]
    fn command_allow_loopback_permits_loopback_and_blocks_non_loopback()
    -> Result<(), Box<dyn Error>> {
        if !mo_host::confine::can_confine() {
            return Ok(());
        }
        assert!(
            Path::new("/usr/bin/python3").exists(),
            "/usr/bin/python3 is required when Linux confinement is enabled"
        );

        let dir = temp_dir("loopback")?;

        // Bind a real loopback TCP echo listener BEFORE spawning the confined child.
        let listener = TcpListener::bind("127.0.0.1:0")?;
        let loop_port = listener.local_addr()?.port();
        let server_thread = std::thread::spawn(move || -> std::io::Result<()> {
            let (mut stream, _) = listener.accept()?;
            let mut buf = [0u8; 64];
            let n = stream.read(&mut buf)?;
            stream.write_all(&buf[..n])?;
            stream.flush()?;
            Ok(())
        });

        // Also bind a wildcard `0.0.0.0:0` listener to prove Landlock does NOT whitelist wildcard ports.
        let wildcard_listener = TcpListener::bind("0.0.0.0:0")?;
        let wildcard_port = wildcard_listener.local_addr()?.port();

        let main_mo = dir.join("main.mo");
        let src = format!(
            r##"module Main
use Command

fn main(console: Console, cmd: Command, net: Network) : Unit
  py_default = cmd.allow_programs(["python3"])
  py_loop = py_default.allow_loopback(net)
  py_disallowed = py_default.allow_loopback(net.allow_hosts(["example.com"]))

  # 1. Without allow_loopback(net), even loopback TCP socket creation is blocked with EPERM (1).
  r_def = py_default.run(
    "python3",
    [
      "-c",
      "import socket\ntry:\n s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n print('DEF_OPEN')\nexcept OSError as e:\n print('DEF_BLOCKED', e.errno)\n"
    ],
    5.seconds
  )
  case r_def
    Ok(o): console.print(o.stdout.trim)
    Error(_): console.print("DEF_ERR")
  end

  # 1b. With a non-loopback Network allowlist, allow_loopback fails closed!
  r_disallowed = py_disallowed.run(
    "python3",
    [
      "-c",
      "import socket\ntry:\n s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n print('DISALLOWED_OPEN')\nexcept OSError as e:\n print('DISALLOWED_BLOCKED', e.errno)\n"
    ],
    5.seconds
  )
  case r_disallowed
    Ok(o): console.print(o.stdout.trim)
    Error(_): console.print("DISALLOWED_ERR")
  end

  # 2. With allow_loopback(net), connecting to 127.0.0.1:{loop_port} succeeds, while:
  #    - connecting to external IP 1.1.1.1:80 is blocked by Landlock ConnectTcp (EACCES = 13)
  #    - connecting to wildcard listener port 127.0.0.1:{wildcard_port} is blocked by Landlock (EACCES = 13)
  #    - creating a UDP socket (SOCK_DGRAM) or AF_UNIX socket is blocked by Seccomp (EPERM = 1)
  script = "import socket\ns = socket.create_connection(('127.0.0.1', {loop_port}), timeout=2.0)\ns.sendall(b'ping-loopback')\nreply = s.recv(64).decode()\ns.close()\nprint('LOOP_OK:' + reply)\ntry:\n socket.create_connection(('1.1.1.1', 80), timeout=1.0)\n print('EXT_OPEN')\nexcept OSError as e:\n print('EXT_BLOCKED', e.errno)\ntry:\n socket.create_connection(('127.0.0.1', {wildcard_port}), timeout=1.0)\n print('WILD_OPEN')\nexcept OSError as e:\n print('WILD_BLOCKED', e.errno)\ntry:\n socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n print('UDP_OPEN')\nexcept OSError as e:\n print('UDP_BLOCKED', e.errno)\ntry:\n socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n print('UNIX_OPEN')\nexcept OSError as e:\n print('UNIX_BLOCKED', e.errno)\n"

  r_loop = py_loop.run("python3", ["-c", script], 5.seconds)
  case r_loop
    Ok(o):
      console.print(o.stdout.trim)
      if not o.stderr.empty?
        console.print("STDERR:#{{o.stderr.trim}}")
      end
    Error(e): console.print("LOOP_RUN_ERR:#{{e}}")
  end
end
"##,
            loop_port = loop_port,
            wildcard_port = wildcard_port,
        );
        fs::write(&main_mo, src)?;

        let out = Command::new(mo_bin())
            .arg("run")
            .arg("--root")
            .arg(&dir)
            .arg(&main_mo)
            .output()?;
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(
            out.status.success(),
            "mo run failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
        );
        let _ = server_thread.join();
        drop(wildcard_listener);

        assert!(
            stdout.contains("DEF_BLOCKED 1"),
            "expected default Command to block AF_INET socket with EPERM(1), got:\n{stdout}"
        );
        assert!(
            stdout.contains("DISALLOWED_BLOCKED 1"),
            "expected non-loopback Network allowlist to keep AF_INET blocked with EPERM(1), got:\n{stdout}"
        );
        assert!(
            stdout.contains("LOOP_OK:ping-loopback"),
            "expected loopback TCP connect+echo to succeed, got:\n{stdout}"
        );
        assert!(
            stdout.contains("EXT_BLOCKED 13"),
            "expected non-loopback 1.1.1.1:80 connect to be blocked with EACCES(13), got:\n{stdout}"
        );
        assert!(
            stdout.contains("WILD_BLOCKED 13"),
            "expected wildcard 0.0.0.0 listener port to be blocked with EACCES(13), got:\n{stdout}"
        );
        assert!(
            stdout.contains("UDP_BLOCKED 1"),
            "expected SOCK_DGRAM UDP socket to be blocked with EPERM(1), got:\n{stdout}"
        );
        assert!(
            stdout.contains("UNIX_BLOCKED 1"),
            "expected AF_UNIX socket() to be blocked with EPERM(1), got:\n{stdout}"
        );

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }

    #[test]
    fn command_spawn_supervises_waits_kills_and_reaps_on_drop() -> Result<(), Box<dyn Error>> {
        let dir = temp_dir("spawn")?;
        let dropped_pid_file = dir.join("dropped.pid");
        let main_mo = dir.join("main.mo");

        fs::write(
            &main_mo,
            r##"module Main
use Command

fn spawn_and_drop_without_wait(cmd: Command, files: Files) : Int
  case cmd.spawn("sh", ["-c", "sleep 30"])
    Error(_): stop "spawn failed"
    Ok(proc):
      p = proc.pid
      assert proc.alive?
      assert files.write_text("dropped.pid", "#{p}") == Ok(Unit)
      p
  end
end

fn main(console: Console, cmd: Command, files: Files) : Unit
  sh = cmd.allow_programs(["sh"])

  # 1. Normal spawn + wait collects stdout/stderr and exit status.
  case sh.spawn("sh", ["-c", "printf 'hello-spawn'; printf 'err-spawn' >&2; exit 7"])
    Error(_): stop "spawn 1 failed"
    Ok(p1):
      assert p1.pid > 0
      case p1.wait(5.seconds)
        Error(_): stop "wait 1 failed"
        Ok(out):
          console.print("p1:#{out.status}:#{out.stdout}:#{out.stderr}")
          assert not p1.alive?
      end
  end

  # 2. Explicit kill() terminates a long-running child and marks alive? false.
  case sh.spawn("sh", ["-c", "sleep 30"])
    Error(_): stop "spawn 2 failed"
    Ok(p2):
      assert p2.alive?
      assert p2.kill() == Ok(Unit)
      assert not p2.alive?
      console.print("p2:killed")
  end

  # 3. Dropping a live Command.Process handle on scope exit kills and reaps the child!
  dropped_pid = spawn_and_drop_without_wait(sh, files)
  console.print("p3:#{dropped_pid}")
end
"##,
        )?;

        let out = Command::new(mo_bin())
            .arg("run")
            .arg("--root")
            .arg(&dir)
            .arg(&main_mo)
            .output()?;
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(
            out.status.success(),
            "mo run failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
        );
        assert!(
            stdout.contains("p1:7:hello-spawn:err-spawn"),
            "stdout:\n{stdout}"
        );
        assert!(stdout.contains("p2:killed"), "stdout:\n{stdout}");

        let pid_str = fs::read_to_string(&dropped_pid_file)?;
        let pid: i32 = pid_str.trim().parse()?;
        let mut gone = false;
        for _ in 0..40 {
            if !Path::new(&format!("/proc/{pid}")).exists() {
                gone = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        assert!(
            gone,
            "dropped Command.Process PID {pid} was not killed and reaped!"
        );

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }

    #[test]
    fn mo_serve_ephemeral_port_zero_and_ready_file_live_orchestration() -> Result<(), Box<dyn Error>>
    {
        let dir = temp_dir("serve-ready")?;
        let data_dir = dir.join("data");
        fs::create_dir_all(&data_dir)?;
        let app_mo = dir.join("app.mo");
        fs::write(
            &app_mo,
            r#"module App

api
  port 8080
  get "/health" health
end

fn health() : Text
  "ready-ok"
end
"#,
        )?;

        // Orchestrate `mo serve --port 0 --ready-file ready.json` via `Command.allow_loopback().spawn("mo", ...)`
        // from an actual Mo program!
        let runner_mo = dir.join("runner.mo");
        fs::write(
            &runner_mo,
            r##"module Runner
use Command
use Http
use Json

fn main(console: Console, cmd: Command, files: Files, clock: Clock, net: Network) : Unit
  mo_cmd = cmd.allow_programs(["mo"]).allow_loopback(net)
  srv = case mo_cmd.spawn(
    "mo",
    ["serve", "app.mo", "--port", "0", "--data", "data", "--ready-file", "ready.json"]
  )
    Ok(p): p
    Error(e):
      console.error("failed to spawn mo serve: #{e}")
      stop "failed to spawn mo serve"
  end

  var ready_text = ""
  for _ in 0..200
    if files.exists?("ready.json")
      case files.read_text("ready.json")
        Ok(t):
          if not t.empty?
            ready_text = t
            break
          end
        Error(_): Unit
      end
    end
    clock.sleep(25.milliseconds)
  end
  assert not ready_text.empty?

  doc = case Json.parse(ready_text)
    Ok(j): j
    Error(_):
      console.error("ready.json was not valid JSON: #{ready_text}")
      stop "ready.json was not valid JSON"
  end
  url = doc.text_at("url") or stop "missing url"
  port = doc.int_at("port") or 0
  pid = doc.int_at("pid") or 0
  data_path = doc.text_at("data_dir") or ""
  started_at = doc.text_at("started_at") or ""

  assert port > 0 and port != 8080
  assert pid > 0
  assert url == "http://127.0.0.1:#{port}"
  assert data_path.ends_with?("/data")
  assert started_at.ends_with?("Z")

  # Send a real live HTTP request to the bound ephemeral URL.
  res = case Http.get("#{url}/health").send(net)
    Ok(r): r
    Error(e):
      console.error("live HTTP request failed: #{e}")
      stop "live HTTP request failed"
  end
  assert res.status == 200
  assert res.text == Some("\"ready-ok\"")

  assert srv.alive?
  assert srv.kill() == Ok(Unit)
  assert not srv.alive?
  console.print("SERVE_READY_OK:#{port}")
end
"##,
        )?;

        let bin_dir = mo_bin()
            .parent()
            .ok_or("missing mo parent dir")?
            .to_path_buf();
        let path_env = match std::env::var("PATH") {
            Ok(existing) if !existing.is_empty() => format!("{}:{existing}", bin_dir.display()),
            _ => bin_dir.display().to_string(),
        };

        let out = Command::new(mo_bin())
            .env("PATH", &path_env)
            .arg("run")
            .arg("--root")
            .arg(&dir)
            .arg(&runner_mo)
            .output()?;
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(
            out.status.success() && stdout.contains("SERVE_READY_OK:"),
            "expected live mo serve --port 0 --ready-file orchestration to succeed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
        );

        // Also verify direct Rust-side spawn + SIGTERM clean shutdown of `mo serve --port 0 --ready-file`.
        let ready2 = dir.join("ready2.json");
        let mut child = Command::new(mo_bin())
            .current_dir(&dir)
            .args([
                "serve",
                "app.mo",
                "--port",
                "0",
                "--data",
                "data",
                "--ready-file",
                "ready2.json",
            ])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::piped())
            .spawn()?;

        let deadline = Instant::now() + Duration::from_secs(10);
        while !ready2.exists() {
            assert!(
                Instant::now() < deadline,
                "timed out waiting for ready2.json"
            );
            std::thread::sleep(Duration::from_millis(20));
        }
        let raw_json = fs::read_to_string(&ready2)?;
        assert!(raw_json.contains("\"url\":\"http://127.0.0.1:"));
        let port_part = raw_json
            .split("\"port\":")
            .nth(1)
            .and_then(|s| s.split(',').next())
            .ok_or("missing port")?;
        let port: u16 = port_part.parse()?;
        assert!(port > 0);

        let mut stream = TcpStream::connect(("127.0.0.1", port))?;
        stream
            .write_all(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")?;
        let mut resp = String::new();
        stream.read_to_string(&mut resp)?;
        assert!(resp.starts_with("HTTP/1.1 200 OK"), "resp:\n{resp}");
        assert!(resp.ends_with("\"ready-ok\""), "resp:\n{resp}");

        // Send SIGTERM via `kill -TERM <pid>` for clean shutdown (exit status 0).
        let _ = Command::new("/bin/kill")
            .args(["-TERM", &child.id().to_string()])
            .status();
        let status = child.wait()?;
        assert_eq!(status.code(), Some(0), "expected clean exit 0 on SIGTERM");
        assert!(
            !ready2.exists(),
            "expected ready2.json to be removed on clean shutdown"
        );

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }

    #[test]
    fn command_process_wait_bounds_and_kills_grandchildren() -> Result<(), Box<dyn Error>> {
        let dir = temp_dir("grandchild")?;
        let pid_file = dir.join("gc.pid");
        let main_mo = dir.join("main.mo");
        fs::write(
            &main_mo,
            r##"module Main
use Command

fn spawn_null_bg(cmd: Command) : Unit
  p = case cmd.spawn("sh", ["-c", "sleep 29 >/dev/null 2>&1 & echo $! > gc.pid; echo leader-done"])
    Ok(h): h
    Error(_): stop "spawn null bg"
  end
  assert p.wait(2.seconds) is Ok(out) and out.stdout.contains?("leader-done")
end

fn main(console: Console, cmd: Command) : Unit
  sh = cmd.allow_programs(["sh"])

  # 1. Background grandchild holding stdout/stderr MUST NOT cause wait(500ms) to block until child exits!
  p1 = case sh.spawn("sh", ["-c", "sleep 7 & echo leader-done"])
    Ok(h): h
    Error(_): stop "spawn pipe holder"
  end
  res1 = p1.wait(500.milliseconds)
  assert res1 == Error(Command.TimedOut)
  console.print("WAIT_BOUNDED_OK")

  # 2. Background grandchild with redirected stdio MUST be killed before leader reaps!
  spawn_null_bg(sh)
  console.print("GC_KILLED_OK")
end
"##,
        )?;

        let start = Instant::now();
        let out = Command::new(mo_bin())
            .arg("run")
            .arg("--root")
            .arg(&dir)
            .arg(&main_mo)
            .output()?;
        let elapsed = start.elapsed();
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(
            out.status.success(),
            "mo run failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
        );
        assert!(stdout.contains("WAIT_BOUNDED_OK"), "stdout:\n{stdout}");
        assert!(stdout.contains("GC_KILLED_OK"), "stdout:\n{stdout}");
        assert!(
            elapsed < Duration::from_secs(4),
            "expected bounded wait to complete well under 4s, took {elapsed:?}"
        );

        let gc_pid: i32 = fs::read_to_string(&pid_file)?.trim().parse()?;
        let mut gone = false;
        for _ in 0..40 {
            if !Path::new(&format!("/proc/{gc_pid}")).exists() {
                gone = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        assert!(gone, "grandchild PID {gc_pid} survived wait/drop!");

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }

    #[test]
    fn command_fake_pid_is_per_fake_deterministic() -> Result<(), Box<dyn Error>> {
        let dir = temp_dir("fake-pid")?;
        let main_mo = dir.join("main.mo");
        fs::write(
            &main_mo,
            r#"module Main
use Command

fn label(x: Int) : Text
  if x > 0: "pos" else: "non"
end

test "per-fake PID starts at 10000 deterministically without inflating mutant kills"
  _ = label(5)
  c1 = Command.fake(Command.replies([(Command.spec("echo", ["a"]), Command.ok_output("a"))]))
  c2 = Command.fake(Command.replies([(Command.spec("echo", ["b"]), Command.ok_output("b"))]))
  p1 = case c1.spawn("echo", ["a"])
    Ok(h): h
    Error(_): stop "p1"
  end
  p2 = case c2.spawn("echo", ["b"])
    Ok(h): h
    Error(_): stop "p2"
  end
  assert p1.pid == 10000
  assert p2.pid == 10000
end
"#,
        )?;

        let out = Command::new(mo_bin())
            .args(["test", "--json", "--jobs", "1", "--mutants"])
            .arg(&main_mo)
            .output()?;
        // Label's 3 mutants must survive (not falsely killed by incrementing global fake PID).
        assert_eq!(out.status.code(), Some(1), "mutants should survive");
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(
            stdout.contains("\"survived\":3"),
            "expected 3 surviving mutants (per-fake PID), got:\n{stdout}"
        );

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }

    #[test]
    fn command_process_rejected_on_main_and_service_state() -> Result<(), Box<dyn Error>> {
        let dir = temp_dir("reject")?;

        let bad_main = dir.join("bad-main.mo");
        fs::write(
            &bad_main,
            "module BadMain\nuse Command\n\nfn main(_p: Command.Process)\nend\n",
        )?;
        let out1 = Command::new(mo_bin())
            .arg("check")
            .arg(&bad_main)
            .output()?;
        assert_eq!(out1.status.code(), Some(1));
        assert!(
            String::from_utf8_lossy(&out1.stderr).contains("E0079"),
            "stderr:\n{}",
            String::from_utf8_lossy(&out1.stderr)
        );

        let bad_svc = dir.join("bad-svc.mo");
        fs::write(
            &bad_svc,
            r#"module BadSvc
use Command

struct State
  proc: Option(Command.Process)
end

service
  state State = State(proc: None)
  tools ping
  token from env "TOK"
end

fn ping(_s: State, _at: Time) : Text
  "pong"
end
"#,
        )?;
        let out2 = Command::new(mo_bin()).arg("check").arg(&bad_svc).output()?;
        assert_eq!(out2.status.code(), Some(1));
        assert!(
            String::from_utf8_lossy(&out2.stderr).contains("E0104"),
            "stderr:\n{}",
            String::from_utf8_lossy(&out2.stderr)
        );

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }
}
