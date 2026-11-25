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
