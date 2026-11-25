#![forbid(unsafe_code)]

//! Integration tests for `mo test --jobs N`, structured NDJSON events under
//! `mo test --json --mutants` (`mutant_killed`, `mutant_survived`, `mutant_summary`), and
//! structured NDJSON events under `mo test --json --strength` (`strength_case`, `strength_summary`).

use std::error::Error;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};

static SEQ: AtomicU64 = AtomicU64::new(1);

fn mo_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_mo"))
}

fn repo_root() -> Result<PathBuf, Box<dyn Error>> {
    Ok(Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .ok_or("missing workspace root")?
        .to_path_buf())
}

fn temp_dir(tag: &str) -> Result<PathBuf, Box<dyn Error>> {
    let n = SEQ.fetch_add(1, Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("mo-test-ndjson-{tag}-{}-{n}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir)?;
    Ok(dir)
}

fn strip_microseconds(ndjson: &str) -> Vec<String> {
    ndjson
        .lines()
        .map(|line| {
            if let Some(pos) = line.find(",\"microseconds\":") {
                let rest = &line[pos + ",\"microseconds\":".len()..];
                let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
                format!("{}{}", &line[..pos], &rest[digits..])
            } else {
                line.to_string()
            }
        })
        .collect()
}

#[test]
fn mutants_ndjson_reports_surviving_mutant_and_kill_rate() -> Result<(), Box<dyn Error>> {
    let survivor = repo_root()?.join("tests/errors/e1439/main.mo");
    let out = Command::new(mo_bin())
        .args(["test", "--json", "--jobs", "2", "--mutants"])
        .arg(&survivor)
        .output()?;
    assert_eq!(out.status.code(), Some(1), "surviving mutant must exit 1");
    assert!(out.stderr.is_empty(), "json mode writes nothing to stderr");

    let stdout = String::from_utf8(out.stdout)?;
    let lines: Vec<&str> = stdout.lines().collect();
    assert!(lines.len() >= 6, "stdout:\n{stdout}");
    assert!(lines[0].contains("\"record\":\"version\""));
    assert!(
        lines
            .last()
            .is_some_and(|l| l.contains("\"record\":\"totals\""))
    );

    let killed_events: Vec<&str> = lines
        .iter()
        .copied()
        .filter(|l| l.contains("\"record\":\"mutant_killed\""))
        .collect();
    let survived_events: Vec<&str> = lines
        .iter()
        .copied()
        .filter(|l| l.contains("\"record\":\"mutant_survived\""))
        .collect();
    let summary_events: Vec<&str> = lines
        .iter()
        .copied()
        .filter(|l| l.contains("\"record\":\"mutant_summary\""))
        .collect();

    assert_eq!(killed_events.len(), 1, "stdout:\n{stdout}");
    assert_eq!(survived_events.len(), 1, "stdout:\n{stdout}");
    assert_eq!(summary_events.len(), 1, "stdout:\n{stdout}");

    assert!(killed_events[0].contains("\"event\":\"mutant_killed\""));
    assert!(killed_events[0].contains("\"module\":\"Main\""));
    assert!(killed_events[0].contains("\"function\":\"Main.small\""));
    assert!(killed_events[0].contains("\"fn\":\"small\""));
    assert!(killed_events[0].contains("\"by\":\"test\""));
    assert!(killed_events[0].contains("\"killed_by\":\"Main.test.1\""));

    assert!(survived_events[0].contains("\"event\":\"mutant_survived\""));
    assert!(survived_events[0].contains("\"module\":\"Main\""));
    assert!(survived_events[0].contains("\"function\":\"Main.small\""));
    assert!(survived_events[0].contains("\"fn\":\"small\""));
    assert!(survived_events[0].contains("\"line\":9"));
    assert!(survived_events[0].contains("\"kind\":\"boundary\""));
    assert!(survived_events[0].contains("\"unrun\":false"));

    assert!(summary_events[0].contains("\"tried\":2"));
    assert!(summary_events[0].contains("\"killed\":1"));
    assert!(summary_events[0].contains("\"survived\":1"));
    assert!(summary_events[0].contains("\"kill_rate\":0.5000"));
    Ok(())
}

#[test]
fn mutants_ndjson_all_killed_and_parallel_jobs_deterministic() -> Result<(), Box<dyn Error>> {
    let dir = temp_dir("parallel-mutants")?;
    let main_mo = dir.join("main.mo");
    fs::write(
        &main_mo,
        r#"module Main

fn strictly_below?(x: Int, limit: Int) : Bool
  examples
    strictly_below?(4, 5)
    not strictly_below?(5, 5)
    not strictly_below?(6, 5)
  end

  x < limit
end

fn allowed?(active: Bool, blocked: Bool) : Bool
  promise
    result == (active and not blocked)
  end

  active and not blocked
end

fn linear(x: Int) : Int
  examples
    linear(0) == 3
    linear(4) == 11
  end

  x * 2 + 3
end

test "all helpers"
  assert strictly_below?(1, 2)
  assert not strictly_below?(2, 2)
  assert allowed?(true, false)
  assert not allowed?(true, true)
  assert linear(5) == 13
end
"#,
    )?;

    let json_j1 = Command::new(mo_bin())
        .args(["test", "--json", "--jobs", "1", "--mutants"])
        .arg(&main_mo)
        .output()?;
    let json_j4 = Command::new(mo_bin())
        .args(["test", "--json", "--jobs", "4", "--mutants"])
        .arg(&main_mo)
        .output()?;
    assert_eq!(json_j1.status.code(), Some(0));
    assert_eq!(json_j4.status.code(), Some(0));

    let s1 = String::from_utf8(json_j1.stdout)?;
    let s4 = String::from_utf8(json_j4.stdout)?;
    assert_eq!(
        strip_microseconds(&s1),
        strip_microseconds(&s4),
        "parallel --jobs 4 NDJSON must match serial --jobs 1 deterministically"
    );
    assert!(s4.contains("\"record\":\"mutant_summary\""));
    assert!(s4.contains("\"survived\":0"));
    assert!(s4.contains("\"kill_rate\":1.0000"));

    // Human-readable text output without --json is byte-for-byte identical across --jobs 1 and --jobs 4.
    let txt_default = Command::new(mo_bin())
        .args(["test", "--mutants"])
        .arg(&main_mo)
        .output()?;
    let txt_j1 = Command::new(mo_bin())
        .args(["test", "--jobs", "1", "--mutants"])
        .arg(&main_mo)
        .output()?;
    let txt_j4 = Command::new(mo_bin())
        .args(["test", "--jobs", "4", "--mutants"])
        .arg(&main_mo)
        .output()?;
    assert_eq!(txt_default.status.code(), Some(0));
    assert_eq!(txt_default.stdout, txt_j1.stdout);
    assert_eq!(txt_j1.stdout, txt_j4.stdout);
    assert!(!String::from_utf8_lossy(&txt_j4.stdout).contains("\"record\":"));

    // Reject invalid --jobs values with CLI exit code 2.
    for bad in ["0", "-1", "257", "abc"] {
        let bad_out = Command::new(mo_bin())
            .args(["test", "--jobs", bad, "--mutants"])
            .arg(&main_mo)
            .output()?;
        assert_eq!(bad_out.status.code(), Some(2), "bad --jobs {bad}");
    }

    let _ = fs::remove_dir_all(&dir);
    Ok(())
}

#[test]
fn strength_ndjson_and_parallel_jobs_deterministic() -> Result<(), Box<dyn Error>> {
    let root = repo_root()?;
    let program = root.join("tests/cli/strength/main.mo");
    let versions = root.join("tests/cli/strength/versions");
    let prog_str = program.to_str().ok_or("non-utf8 program path")?;
    let ver_str = versions.to_str().ok_or("non-utf8 versions path")?;

    let j1 = Command::new(mo_bin())
        .current_dir(&root)
        .args([
            "test",
            "--json",
            "--jobs",
            "1",
            prog_str,
            "--strength",
            ver_str,
        ])
        .output()?;
    let j4 = Command::new(mo_bin())
        .current_dir(&root)
        .args([
            "test",
            "--json",
            "--jobs",
            "4",
            prog_str,
            "--strength",
            ver_str,
        ])
        .output()?;
    assert_eq!(j1.status.code(), Some(0));
    assert_eq!(j4.status.code(), Some(0));

    let s1 = String::from_utf8(j1.stdout)?;
    let s4 = String::from_utf8(j4.stdout)?;
    assert_eq!(
        strip_microseconds(&s1),
        strip_microseconds(&s4),
        "parallel --jobs 4 strength NDJSON must match serial --jobs 1"
    );

    let case_lines: Vec<&str> = s4
        .lines()
        .filter(|l| l.contains("\"record\":\"strength_case\""))
        .collect();
    assert_eq!(
        case_lines.len(),
        8,
        "expected 8 strength_case events:\n{s4}"
    );
    assert!(case_lines.iter().any(|l| {
        l.contains("\"name\":\"a-negative.mo\"")
            && l.contains("\"outcome\":\"killed_by_promise\"")
            && l.contains("\"killed\":true")
    }));
    assert!(case_lines.iter().any(|l| {
        l.contains("\"name\":\"d-seven.mo\"")
            && l.contains("\"outcome\":\"survived\"")
            && l.contains("\"killed\":false")
    }));
    assert!(case_lines.iter().any(|l| {
        l.contains("\"name\":\"f-text.mo\"")
            && l.contains("\"outcome\":\"invalid\"")
            && l.contains("\"reason\":")
    }));

    let summary_lines: Vec<&str> = s4
        .lines()
        .filter(|l| l.contains("\"record\":\"strength_summary\""))
        .collect();
    assert_eq!(summary_lines.len(), 1, "s4:\n{s4}");
    assert!(summary_lines[0].contains("\"scored\":4"));
    assert!(summary_lines[0].contains("\"killed\":3"));
    assert!(summary_lines[0].contains("\"survived\":1"));
    assert!(summary_lines[0].contains("\"kill_rate\":0.7500"));
    assert!(summary_lines[0].contains("\"partial\":false"));
    Ok(())
}
