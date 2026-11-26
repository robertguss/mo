#![forbid(unsafe_code)]

//! End-to-end confinement & resource-limit verification tests for `Command.run_raw`,
//! `mo emit --bundle --confine-child --rlimit-child`, and `mo test --bundle --confine-child --rlimit-child`.

#[cfg(all(unix, test))]
mod unix_confinement {
    use std::fs;
    use std::os::unix::fs::PermissionsExt;
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use std::time::{Duration, Instant};

    use mo_def::ErrorCode;
    use mo_service::binary::bundle::{BundleFile, write_bundle};
    use mo_service::upgrade::compile::{CompileJob, CompilePoll};

    fn mo_bin() -> PathBuf {
        PathBuf::from(env!("CARGO_BIN_EXE_mo"))
    }

    fn unique_temp_dir(tag: &str) -> std::io::Result<PathBuf> {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(1);
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir =
            std::env::temp_dir().join(format!("mo-confine-test-{tag}-{}-{n}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir)?;
        Ok(dir)
    }

    fn run_mo_with_root(root: &Path, src: &str) -> std::io::Result<std::process::Output> {
        let prog = root.join("main.mo");
        fs::write(&prog, src)?;
        Command::new(mo_bin())
            .arg("run")
            .arg("--root")
            .arg(root)
            .arg(&prog)
            .env("MO_SECRET_CANARY", "top-secret-bearer-token")
            .output()
    }

    #[test]
    fn command_child_enforces_rlimits_env_scrub_landlock_seccomp_and_pgrp_kill() {
        let dir = unique_temp_dir("cmd").expect("temp dir");
        fs::write(dir.join("inside.txt"), "hello-inside\n").expect("write inside.txt");

        let outside_dir = unique_temp_dir("cmd-outside").expect("outside dir");
        let outside_write = outside_dir.join("escape.txt");
        let pid_file = dir.join("grandchild.pid");

        // 1. Allowed workdir read/write, rlimits, env scrub, and confinement probes.
        let probe_src = format!(
            r#"module Main
use Command

fn main(console: Console, cmd: Command) : Unit
  sh = cmd.allow_programs(["sh"])
  r1 = sh.run("sh", ["-c", "cat inside.txt && printf ok > written.txt && printf tmpok > \"$TMPDIR/t.txt\" && cat \"$TMPDIR/t.txt\""], 5.seconds)
  case r1
    Ok(out): console.print("r1:#{{out.stdout.trim}}")
    Error(_): console.print("r1-err")
  end

  r2 = sh.run("sh", ["-c", "printf 'canary=%s;core=%s;cpu=%s;as=%s\n' \"${{MO_SECRET_CANARY:-}}\" \"$(ulimit -c)\" \"$(ulimit -t)\" \"$(ulimit -v)\""], 5.seconds)
  case r2
    Ok(out): console.print("r2:#{{out.stdout.trim}}")
    Error(_): console.print("r2-err")
  end

  r3 = sh.run("sh", ["-c", "cat /etc/passwd >/dev/null 2>&1; echo $?"], 5.seconds)
  case r3
    Ok(out): console.print("r3:#{{out.stdout.trim}}")
    Error(_): console.print("r3-err")
  end

  r4 = sh.run("sh", ["-c", "printf x > '{outside_write}' 2>/dev/null; echo $?"], 5.seconds)
  case r4
    Ok(out): console.print("r4:#{{out.stdout.trim}}")
    Error(_): console.print("r4-err")
  end

  r5 = sh.run("sh", ["-c", "sleep 30 & echo $! > '{pid_file}'; wait"], 200.milliseconds)
  case r5
    Ok(_): console.print("r5:ok")
    Error(Command.TimedOut): console.print("r5:timed_out")
    Error(_): console.print("r5:other-err")
  end
end
"#,
            outside_write = outside_write.display(),
            pid_file = pid_file.display(),
        );

        let out = run_mo_with_root(&dir, &probe_src).expect("spawn mo run");
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(
            out.status.success(),
            "mo run failed (exit {:?})\nstdout:\n{stdout}\nstderr:\n{stderr}",
            out.status.code()
        );

        assert!(
            stdout.contains("r1:hello-inside\ntmpok"),
            "stdout:\n{stdout}"
        );
        assert_eq!(
            fs::read_to_string(dir.join("written.txt")).expect("written.txt"),
            "ok"
        );

        // Verify env canary scrubbed and core=0, cpu=6 (5s + 1), as <= 2097152 KiB (2 GiB).
        let r2_line = stdout
            .lines()
            .find(|l| l.starts_with("r2:"))
            .expect("r2 line");
        assert!(r2_line.contains("canary=;"), "canary leaked: {r2_line}");
        assert!(r2_line.contains("core=0;"), "RLIMIT_CORE not 0: {r2_line}");
        assert!(r2_line.contains("cpu=6;"), "RLIMIT_CPU not 6: {r2_line}");
        let as_str = r2_line.split("as=").nth(1).expect("as field");
        let as_kib: u64 = as_str.parse().expect("numeric ulimit -v");
        assert!(
            as_kib > 0 && as_kib <= 2_097_152,
            "RLIMIT_AS unexpected: {as_kib} KiB"
        );

        // On Linux with confinement available, /etc/passwd read and outside write MUST fail.
        if mo_host::confine::can_confine() {
            assert!(
                !stdout.contains("r3:0\n"),
                "confined command read /etc/passwd!\nstdout:\n{stdout}"
            );
            assert!(
                !stdout.contains("r4:0\n") && !outside_write.exists(),
                "confined command wrote outside workdir!\nstdout:\n{stdout}"
            );

            // Also verify seccomp blocks TCP socket creation in confined commands (using python3 if present).
            if Path::new("/usr/bin/python3").exists() {
                let net_src = r#"module Main
use Command

fn main(console: Console, cmd: Command) : Unit
  py = cmd.allow_programs(["python3"])
  r = py.run("python3", ["-c", "import socket\ntry:\n s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n print('OPENED')\nexcept OSError as e:\n print('BLOCKED', e.errno)\n"], 5.seconds)
  case r
    Ok(out): console.print(out.stdout.trim)
    Error(_): console.print("ERR")
  end
end
"#;
                let net_out = run_mo_with_root(&dir, net_src).expect("spawn mo run");
                let net_stdout = String::from_utf8_lossy(&net_out.stdout);
                assert!(
                    net_out.status.success() && net_stdout.contains("BLOCKED 1"),
                    "expected seccomp EPERM (1) on AF_INET socket, got:\nstdout={net_stdout}\nstderr={}",
                    String::from_utf8_lossy(&net_out.stderr)
                );
            }
        }

        // Verify timeout killed the background grandchild `sleep 30` via process group!
        assert!(stdout.contains("r5:timed_out"), "stdout:\n{stdout}");
        if let Ok(pid_txt) = fs::read_to_string(&pid_file)
            && let Ok(gpid) = pid_txt.trim().parse::<i32>()
        {
            let mut alive = true;
            for _ in 0..20 {
                if !Path::new(&format!("/proc/{gpid}")).exists() {
                    alive = false;
                    break;
                }
                std::thread::sleep(Duration::from_millis(25));
            }
            assert!(
                !alive,
                "grandchild process {gpid} survived Command timeout (process group was not killed)"
            );
        }

        let _ = fs::remove_dir_all(&dir);
        let _ = fs::remove_dir_all(&outside_dir);
    }

    #[test]
    fn allow_loopback_blocks_non_tcp_protocols_and_narrows_ports() {
        use std::io::{Read, Write};
        use std::net::TcpListener;

        if !mo_host::confine::can_confine() || !Path::new("/usr/bin/python3").exists() {
            return;
        }

        let dir = unique_temp_dir("loopback-narrow").expect("temp dir");
        let listener_a = TcpListener::bind("127.0.0.1:0").expect("bind port A");
        let port_a = listener_a.local_addr().expect("addr A").port();
        let listener_b = TcpListener::bind("127.0.0.1:0").expect("bind port B");
        let port_b = listener_b.local_addr().expect("addr B").port();

        let stop_srv = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let stop_srv_bg = std::sync::Arc::clone(&stop_srv);
        listener_a
            .set_nonblocking(true)
            .expect("nonblocking listener_a");
        let srv_a = std::thread::spawn(move || -> std::io::Result<()> {
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
            let mut stream = loop {
                match listener_a.accept() {
                    Ok((s, _)) => break s,
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        if stop_srv_bg.load(std::sync::atomic::Ordering::Relaxed)
                            || std::time::Instant::now() >= deadline
                        {
                            return Ok(());
                        }
                        std::thread::sleep(std::time::Duration::from_millis(10));
                    }
                    Err(e) => return Err(e),
                }
            };
            stream.set_nonblocking(false)?;
            let mut buf = [0u8; 32];
            let n = stream.read(&mut buf)?;
            stream.write_all(&buf[..n])?;
            stream.flush()
        });

        let py_script = format!(
            r#"import socket
for proto, label in [(132, 'SCTP'), (262, 'MPTCP')]:
  try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM, proto)
    print(label + '_OPEN')
    s.close()
  except OSError as e:
    print(label + '_BLOCKED', e.errno)
sa = socket.create_connection(('127.0.0.1', {port_a}), timeout=2.0)
sa.sendall(b'ping-a')
print('PORT_A_OK:' + sa.recv(32).decode())
sa.close()
try:
  sb = socket.create_connection(('127.0.0.1', {port_b}), timeout=1.0)
  print('PORT_B_OPEN')
  sb.close()
except OSError as e:
  print('PORT_B_BLOCKED', e.errno)
try:
  bl = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
  bl.bind(('127.0.0.1', 0))
  print('BIND_OPEN')
  bl.close()
except OSError as e:
  print('BIND_BLOCKED', e.errno)
"#
        );
        let mo_src = format!(
            r#"module Main
use Command

fn main(console: Console, cmd: Command, net: Network) : Unit
  narrow = cmd.allow_programs(["python3"]).allow_loopback(net.allow_hosts(["127.0.0.1:{port_a}"]))
  r = narrow.run("python3", ["-c", {script_lit:?}], 5.seconds)
  case r
    Ok(o):
      console.print("STATUS:#{{o.status}}")
      console.print(o.stdout.trim)
      if not o.stderr.trim.empty?
        console.print("STDERR:\n#{{o.stderr.trim}}")
      end
    Error(e): console.print("RUN_ERR:#{{e}}")
  end
end
"#,
            port_a = port_a,
            script_lit = py_script,
        );

        let out = run_mo_with_root(&dir, &mo_src).expect("run mo");
        stop_srv.store(true, std::sync::atomic::Ordering::Relaxed);
        let _ = srv_a.join();
        drop(listener_b);

        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(
            out.status.success(),
            "mo run failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
        );
        assert!(
            stdout.contains("STATUS:0"),
            "expected python3 status 0, got:\n{stdout}"
        );
        assert!(
            stdout.contains("SCTP_BLOCKED 1"),
            "expected seccomp EPERM (1) for SCTP protocol 132, got:\n{stdout}"
        );
        assert!(
            stdout.contains("MPTCP_BLOCKED 1"),
            "expected seccomp EPERM (1) for MPTCP protocol 262, got:\n{stdout}"
        );
        assert!(
            stdout.contains("PORT_A_OK:ping-a"),
            "expected allowed loopback port {port_a} to succeed, got:\n{stdout}"
        );
        assert!(
            stdout.contains("PORT_B_BLOCKED 13"),
            "expected disallowed loopback port {port_b} to fail with EACCES(13), got:\n{stdout}"
        );
        assert!(
            stdout.contains("BIND_BLOCKED 13"),
            "expected bind to fail with EACCES(13) when port-narrowed, got:\n{stdout}"
        );

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn compile_job_runs_both_emit_and_test_stages_confined_with_rlimits_and_timeout() {
        let service_src = br#"module Main

struct Count
  value: Int
end

service
  state Int = 0
  tools read_count, inc
  token from env "COUNTER_TOKEN"
end

fn read_count(state: Int, _at: Time) : Count
  Count(value: state)
end

fn inc(state: Int, _at: Time, delta: Int) : (Int, Int)
  next = state + delta
  (next, next)
end

test "counter increments"
  t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
  sim = Simulation.start(Main, t0)
  assert Simulation.send(sim, Main.inc, 2) == Ok((2, 2))
  assert Simulation.send(sim, Main.read_count) == Ok(Count(value: 2))
end
"#;
        let (bundle, digest) = write_bundle(
            "main.mo",
            &[BundleFile {
                path: "main.mo".to_string(),
                content: service_src.to_vec(),
            }],
        )
        .expect("write service bundle");

        let confine = mo_host::confine::can_confine();
        let mut job = CompileJob::start(
            &mo_bin(),
            None,
            bundle.clone(),
            None,
            None,
            None,
            true,
            confine,
            [0xaa; 32],
            1,
        )
        .expect("start confined compile + door-check job");

        // Verify diagnostics file has strict 0600 permissions.
        let mode = fs::metadata(job.diag_file_path())
            .expect("diag metadata")
            .permissions()
            .mode()
            & 0o777;
        assert_eq!(mode, 0o600, "diag file permissions must be 0600");

        let deadline = Instant::now() + Duration::from_secs(30);
        let result = loop {
            match job.poll() {
                CompilePoll::Running => {
                    assert!(Instant::now() < deadline, "CompileJob hung");
                    std::thread::sleep(Duration::from_millis(10));
                }
                CompilePoll::Finished(res) => break *res,
            }
        };
        let img = result.expect("confined Stage 1 emit + Stage 2 test must succeed");
        assert_eq!(img.program.source, digest);

        // Verify wall-clock budget exhaustion returns E1420 (UpgradeCompileBudget) and kills child.
        let hang_dir = unique_temp_dir("hang").expect("hang dir");
        let hang_script = hang_dir.join("hang.sh");
        fs::write(
            &hang_script,
            "#!/bin/sh\n[ -z \"$MO_SECRET_CANARY\" ] || exit 99\nwhile :; do :; done\n",
        )
        .expect("write hang.sh");
        fs::set_permissions(&hang_script, fs::Permissions::from_mode(0o700)).expect("chmod");

        let mut hang_job = CompileJob::start(
            &hang_script,
            None,
            bundle,
            None,
            None,
            None,
            false,
            false,
            [0xbb; 32],
            2,
        )
        .expect("start hang job");
        hang_job.set_deadline_for_test(Instant::now() + Duration::from_millis(80));

        let err = loop {
            match hang_job.poll() {
                CompilePoll::Running => std::thread::sleep(Duration::from_millis(10)),
                CompilePoll::Finished(res) => match *res {
                    Ok(_) => unreachable!("expected budget refusal"),
                    Err(e) => break e,
                },
            }
        };
        assert_eq!(err.0, ErrorCode::UpgradeCompileBudget.number());
        assert_eq!(err.1, "budget");

        let _ = fs::remove_dir_all(&hang_dir);
    }
}
