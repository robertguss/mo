#![forbid(unsafe_code)]

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

#[test]
fn ambiguous_retained_program_returns_e1431() {
    use mo_service::binary::bundle::{BundleFile, write_bundle};
    use mo_service::binary::compile_context::write_compile_context;
    use mo_service::binary::failure::{FailureContextStatus, FailureRecord, write_failure_record};
    use mo_service::store::failures::CaptureStatus;
    use mo_service::store::programs::{ActivationStatus, StoreActivation, StoredProgram};
    use mo_service::store::{ServiceStore, StoreRecord, StoreRecordKind};
    use mo_upgrade::offline::{ReplayOptions, run_replay};

    let tmp =
        std::env::temp_dir().join(format!("mo-upgrade-ambiguous-e1431-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).expect("create temp dir");
    let db_path = tmp.join("service.sqlite");

    // Two distinct source bundles sharing the same `(image, build, schema)` key.
    let (bundle1, source1) = write_bundle(
        "main.mo",
        &[BundleFile {
            path: "main.mo".to_string(),
            content: b"module Main\n# version 1\n".to_vec(),
        }],
    )
    .expect("bundle 1");
    let (bundle2, source2) = write_bundle(
        "main.mo",
        &[BundleFile {
            path: "main.mo".to_string(),
            content: b"module Main\n# version 2\n".to_vec(),
        }],
    )
    .expect("bundle 2");
    assert_ne!(source1, source2);

    let (context, context_digest) = write_compile_context("", &[], None).expect("compile context");
    let shared_image = 42u64;
    let shared_build = [0xaa; 32];
    let shared_schema = [0xbb; 32];

    let p1 = StoredProgram {
        source: source1,
        build: shared_build,
        schema: shared_schema,
        context_digest,
        image: shared_image,
        ordinal: 0,
        bundle: bundle1,
        context: context.clone(),
    };
    let p2 = StoredProgram {
        source: source2,
        build: shared_build,
        schema: shared_schema,
        context_digest,
        image: shared_image,
        ordinal: 1,
        bundle: bundle2,
        context,
    };

    let saved_state = b"MOSV\x01\x01\x00\x00\x00\x03\x05\x00\x00\x00\x00\x00\x00\x00".to_vec();
    let snap0 = StoreRecord {
        kind: StoreRecordKind::State,
        sequence: 0,
        image: shared_image,
        bytes: saved_state.clone(),
    };

    {
        let mut store = ServiceStore::open(Some(&db_path)).expect("open store");
        store.record_program(&p1, &snap0).expect("record p1");
        let act = StoreActivation {
            previous: p1,
            candidate: p2,
            previous_state: snap0.clone(),
            candidate_state: snap0,
            migration: String::new(),
            report_json: "{}".to_string(),
        };
        assert_eq!(store.activate(&act), ActivationStatus::Activated);

        let frec = FailureRecord {
            id: 1,
            sequence: 0,
            arrival: 1_700_000_000_000_000,
            timer: false,
            handler: 0,
            name: "op".to_string(),
            read_only: false,
            arguments: Vec::new(),
            signature_version: 1,
            class: 0,
            reason: "stop".to_string(),
            limit: String::new(),
            limit_value: 0,
            function: 0,
            file: 0,
            start: 0,
            end: 0,
            work: 100,
            heap: 1024,
            stack: 1024,
            depth: 1,
            saved: 1024,
            program: shared_image,
            build: shared_build,
            label: "mo 0.3.0".to_string(),
            schema: shared_schema,
            context_status: FailureContextStatus::Complete,
            context: String::new(),
        };
        let fraw = write_failure_record(&frec).expect("encode failure");
        let fail_state = StoreRecord {
            kind: StoreRecordKind::FailureState,
            sequence: 0,
            image: shared_image,
            bytes: saved_state,
        };
        let fail_msg = StoreRecord {
            kind: StoreRecordKind::FailureMessage,
            sequence: 0,
            image: shared_image,
            bytes: fraw,
        };
        assert_eq!(
            store.capture_failure("stop", &fail_state, &fail_msg),
            CaptureStatus::Ok(1)
        );
    }

    let mut out = Vec::new();
    let mut err = Vec::new();
    let opts = ReplayOptions {
        file: None,
        failure_id: 1,
        data_dir: Some(&tmp),
        artifact: None,
        revised: None,
        recover: false,
        json: false,
        library: None,
        version: mo_def::MO_VERSION,
        build_digest: shared_build,
    };
    let status = run_replay(&opts, &mut out, &mut err);
    let _ = std::fs::remove_dir_all(&tmp);

    let err_text = String::from_utf8_lossy(&err);
    assert_ne!(
        status,
        0,
        "expected E1431 refusal, stdout={}",
        String::from_utf8_lossy(&out)
    );
    assert!(
        err_text.contains("E1431"),
        "expected E1431 UpgradeAmbiguousProgram, got stderr:\n{err_text}"
    );
    assert!(
        err_text.contains("2 retained programs with different sources match failure 1"),
        "unexpected stderr:\n{err_text}"
    );
}
