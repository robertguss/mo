//! Unit tests for `mo-upgrade`: offline verdict classification, rehearsal refusals, and structural diffing.

use std::path::Path;

use mo_service::history::FailureSignature;
use mo_upgrade::diff::{DiffOptions, run_diff};
use mo_upgrade::offline_output::{
    OutcomeStatus, ReplayClass, ReplyKind, RunOutcome, Verdict, classify_replay, compute_verdict,
    signature_match,
};
use mo_upgrade::rehearse::{RehearseOptions, run_rehearse};

fn defect_signature(start: u32, end: u32) -> FailureSignature {
    FailureSignature {
        class: 0,
        reason: "division by zero".to_string(),
        limit: String::new(),
        limit_value: 0,
        function: 0,
        file: 0,
        start,
        end,
    }
}

#[test]
fn replay_and_fix_verdict_classification() {
    let recorded = defect_signature(10, 20);
    let same = defect_signature(10, 20);
    let other = defect_signature(24, 30);

    assert!(signature_match(&recorded, &same));
    assert!(!signature_match(&recorded, &other));

    let reproduced = RunOutcome {
        status: OutcomeStatus::Defect,
        signature: Some(same),
        reply: ReplyKind::Ok,
        state_changed: false,
    };
    let different = RunOutcome {
        status: OutcomeStatus::Defect,
        signature: Some(other),
        reply: ReplyKind::Ok,
        state_changed: false,
    };
    let succeeded = RunOutcome {
        status: OutcomeStatus::Succeeded,
        signature: None,
        reply: ReplyKind::Ok,
        state_changed: false,
    };
    let refused = RunOutcome {
        status: OutcomeStatus::Refused,
        signature: None,
        reply: ReplyKind::Ok,
        state_changed: false,
    };

    assert_eq!(
        classify_replay(&recorded, &reproduced),
        ReplayClass::Reproduced
    );
    assert_eq!(
        classify_replay(&recorded, &different),
        ReplayClass::Different
    );
    assert_eq!(
        classify_replay(&recorded, &succeeded),
        ReplayClass::NotReproduced
    );
    assert_eq!(classify_replay(&recorded, &refused), ReplayClass::Refused);

    assert_eq!(
        compute_verdict(ReplayClass::Reproduced, &succeeded),
        Verdict::Verified
    );
    assert_eq!(
        compute_verdict(ReplayClass::Reproduced, &reproduced),
        Verdict::NotFixed
    );
    assert_eq!(
        compute_verdict(ReplayClass::NotReproduced, &succeeded),
        Verdict::Unverified
    );
    assert_eq!(
        compute_verdict(ReplayClass::Refused, &succeeded),
        Verdict::Refused
    );
    assert_eq!(
        compute_verdict(ReplayClass::Reproduced, &refused),
        Verdict::Refused
    );

    // Work signatures compare solely by limit_value (matching `src/base/verdict.c`).
    let work_a = FailureSignature {
        class: 1,
        reason: "work limit".to_string(),
        limit: "MO_LIMIT_SERVICE_WORK".to_string(),
        limit_value: 1_048_576,
        function: 0,
        file: 0,
        start: 10,
        end: 20,
    };
    let mut work_b = work_a.clone();
    work_b.start = 99;
    assert!(signature_match(&work_a, &work_b));
}

#[test]
fn rehearse_refuses_invalid_artifact_matching_golden() {
    let tmp = std::env::temp_dir().join(format!("mo-upgrade-unit-rehearse-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&tmp);
    let invalid = tmp.join("invalid.moex");
    std::fs::write(&invalid, b"invalid").expect("write invalid export");

    let mut out = Vec::new();
    let mut err = Vec::new();
    let opts = RehearseOptions {
        artifact: &invalid,
        old_bundle: Path::new("missing-old"),
        candidate: Path::new("missing-candidate"),
        migration: None,
        compile_context: None,
        library: None,
        json: false,
        build_digest: [0u8; 32],
    };
    let status = run_rehearse(&opts, &mut out, &mut err);
    let _ = std::fs::remove_dir_all(&tmp);

    assert_eq!(status, 1);
    assert!(out.is_empty());
    assert_eq!(
        String::from_utf8(err).expect("utf8 stderr"),
        "error E1425: cannot read a complete state export\n  rehearsal is not upgrade acceptance\n"
    );
}

#[test]
fn diff_reports_added_and_changed_public_functions() {
    let tmp = std::env::temp_dir().join(format!("mo-upgrade-unit-diff-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&tmp);
    let old_dir = tmp.join("old");
    let new_dir = tmp.join("new");
    let _ = std::fs::create_dir_all(&old_dir);
    let _ = std::fs::create_dir_all(&new_dir);
    let old_path = old_dir.join("main.mo");
    let new_path = new_dir.join("main.mo");
    std::fs::write(
        &old_path,
        "module Main\nexpose add\n\nfn add(a: Int, b: Int) : Int\n  a + b\nend\n",
    )
    .expect("write old.mo");
    std::fs::write(
        &new_path,
        "module Main\nexpose add, sub\n\nfn add(a: Int, b: Int) : Int\n  a + b\nend\n\nfn sub(a: Int, b: Int) : Int\n  a - b\nend\n",
    )
    .expect("write new.mo");

    let old_bytes = old_path.as_os_str().as_encoded_bytes();
    let new_bytes = new_path.as_os_str().as_encoded_bytes();
    let mut out = Vec::new();
    let mut err = Vec::new();
    let opts = DiffOptions {
        old_file: Some(old_bytes),
        new_file: new_bytes,
        library: None,
        json: true,
        version: "0.3.0",
    };
    let status = run_diff(&opts, &mut out, &mut err);
    let _ = std::fs::remove_dir_all(&tmp);

    assert_eq!(status, 0, "stderr: {}", String::from_utf8_lossy(&err));
    assert!(err.is_empty());
    let stdout = String::from_utf8(out).expect("utf8 stdout");
    assert!(stdout.contains("\"record\":\"diff\""), "{stdout}");
    assert!(
        stdout.contains("\"added\":[{\"kind\":\"function\",\"name\":\"Main.sub\""),
        "{stdout}"
    );
    assert!(stdout.contains("\"record\":\"totals\""), "{stdout}");
}
