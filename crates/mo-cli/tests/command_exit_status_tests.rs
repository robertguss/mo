#![forbid(unsafe_code)]

use std::error::Error;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn mo_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_mo"))
}

#[test]
#[cfg(unix)]
fn command_spawn_reports_exec_failure_and_preserves_real_exit_125() -> Result<(), Box<dyn Error>> {
    use std::os::unix::fs::PermissionsExt;

    if !mo_host::confine::can_confine() {
        return Ok(());
    }
    let dir = std::env::temp_dir().join(format!("mo-exit-125-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    let local_bin = dir.join(".local/bin");
    fs::create_dir_all(&local_bin)?;
    let script = local_bin.join("missing-interpreter");
    fs::write(&script, "#!/nonexistent/mo-test-interpreter\n")?;
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700))?;
    fs::write(
        dir.join("main.mo"),
        r#"module Main
use Command

fn main(cmd: Command) : Unit
  sh = cmd.allow_programs(["python3", "missing-interpreter"])
  assert sh.spawn("missing-interpreter", []) is Error(Command.SpawnFailed(reason: reason))
  assert reason.contains?("exec failed")
  assert sh.spawn("python3", ["-c", "raise SystemExit(125)"]) is Ok(proc)
  assert proc.wait(5.seconds) is Ok(out)
  assert out.status == 125
end
"#,
    )?;
    let out = Command::new(mo_bin())
        .current_dir(&dir)
        .env("HOME", &dir)
        .args(["run", "main.mo"])
        .output()?;
    assert!(
        out.status.success(),
        "stdout: {}
stderr: {}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    fs::remove_dir_all(dir)?;
    Ok(())
}
