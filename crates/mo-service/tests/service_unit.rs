#![forbid(unsafe_code)]

//! Unit tests for `mo-service` binary codecs, credentials, SQLite durable store,
//! offline history reader, and embedded protocol compilation.

use mo_service::binary::bundle::{BundleFile, read_bundle, valid_bundle_path, write_bundle};
use mo_service::binary::compile_context::{
    CompileFile, CompileMetadata, CompileTimerEntry, CompileToolEntry, read_compile_context,
    read_compile_metadata, write_compile_context, write_compile_metadata,
};
use mo_service::binary::export::{ExportIdentity, create_export_pin, read_export};
use mo_service::binary::failure::{
    FailureContextStatus, FailureRecord, read_failure_record, write_failure_record,
};
use mo_service::binary::log_record::{
    LogRecordHeader, read_log_record, walk_log_record, write_log_record,
};
use mo_service::binary::upload::{UploadPageStatus, UploadStage};
use mo_service::binary::{hex_decode_exact, hex_encode, sanitize_terminal_text, sha256_bytes};
use mo_service::listener::credentials::Bearer;
use mo_service::protocol::build_protocol_image;
use mo_service::store::history::read_history;
use mo_service::store::programs::StoredProgram;
use mo_service::store::{ServiceStore, StoreRecord, StoreRecordKind, decode_frame, encode_frame};

#[test]
fn bundle_codec_roundtrip_and_rejections() {
    assert!(valid_bundle_path("main.mo"));
    assert!(valid_bundle_path("sub/helper.mo"));
    assert!(!valid_bundle_path(""));
    assert!(!valid_bundle_path("/abs.mo"));
    assert!(!valid_bundle_path("../escape.mo"));
    assert!(!valid_bundle_path("sub//dup.mo"));

    let files = vec![
        BundleFile {
            path: "helper.mo".to_string(),
            content: b"module Helper\n".to_vec(),
        },
        BundleFile {
            path: "main.mo".to_string(),
            content: b"module Main\n".to_vec(),
        },
    ];
    let (wire, digest) = write_bundle("main.mo", &files).expect("valid bundle");
    let parsed = read_bundle(&wire).expect("decode valid bundle");
    assert_eq!(parsed.entry, "main.mo");
    assert_eq!(parsed.digest, digest);
    assert_eq!(parsed.files.len(), 2);

    // Corrupting one byte breaks checksum / structure validation.
    let mut bad = wire.clone();
    bad[0] ^= 0xff;
    assert!(read_bundle(&bad).is_none());
}

#[test]
fn compile_metadata_and_context_roundtrip() {
    let meta = CompileMetadata {
        token: "APP_TOKEN".to_string(),
        schema: vec![3],
        types: vec![CompileFile {
            path: "UpgradeOld.mo".to_string(),
            source: b"module UpgradeOld\n".to_vec(),
        }],
        tools: vec![CompileToolEntry {
            name: "ping".to_string(),
            digest: [0x11; 32],
        }],
        timers: vec![CompileTimerEntry {
            name: "tick".to_string(),
            interval: 1_000_000,
        }],
    };
    let meta_wire = write_compile_metadata(&meta).expect("encode MOCS");
    let decoded_meta = read_compile_metadata(&meta_wire).expect("decode MOCS");
    assert_eq!(decoded_meta, meta);

    let (ctx_wire, ctx_digest) =
        write_compile_context("migrate", &meta.types, Some(&meta_wire)).expect("encode MOCC");
    let decoded_ctx = read_compile_context(&ctx_wire).expect("decode MOCC");
    assert_eq!(decoded_ctx.migration, "migrate");
    assert_eq!(decoded_ctx.digest, ctx_digest);
    assert_eq!(decoded_ctx.files, meta.types);
    assert_eq!(decoded_ctx.metadata.as_deref(), Some(meta_wire.as_slice()));

    // Non-positive timer interval is refused.
    let mut bad_meta = meta;
    bad_meta.timers[0].interval = 0;
    assert!(write_compile_metadata(&bad_meta).is_none());
}

#[test]
fn export_pin_and_read_roundtrip() {
    let saved = b"MOSV\x01\x01\x00\x00\x00\x03\x05\x00\x00\x00\x00\x00\x00\x00";
    let id = ExportIdentity {
        kind: 1,
        id: [0x12; 16],
        build: [0x22; 32],
        label: "mo 0.3.0".to_string(),
        service: 0,
        program: 0xdead_beef_cafe_babe,
        sequence: 7,
        encoded_sequence: 7,
        failure_id: 0,
        created: 1_700_000_000_000_000,
    };
    let pin = create_export_pin(&id, saved, 1_000).expect("pin state export");
    let art = read_export(&pin.bytes).expect("read MOEX");
    assert_eq!(art.identity.sequence, 7);
    assert_eq!(art.identity.program, 0xdead_beef_cafe_babe);
    assert_eq!(art.state, saved);
    assert_eq!(art.identity.label, "mo 0.3.0");

    let mut tampered = pin.bytes.clone();
    let last = tampered.len() - 1;
    tampered[last] ^= 0x01;
    assert!(read_export(&tampered).is_none());
}

#[test]
fn log_and_failure_records_roundtrip() {
    let hdr = LogRecordHeader {
        state_checksum: 0x1234_5678,
        arrival: 1_700_000_000_000_000,
        handler: 2,
        count: 2,
        timer: false,
    };
    let a0 = b"MOSV\x01\x01\x00\x00\x00\x03\x05\x00\x00\x00\x00\x00\x00\x00";
    let a1 = b"MOSV\x01\x01\x00\x00\x00\x03\x06\x00\x00\x00\x00\x00\x00\x00";
    let raw = write_log_record(&hdr, &[a0.as_slice(), a1.as_slice()]).expect("encode log record");
    let parsed = read_log_record(&raw).expect("decode log record");
    assert_eq!(parsed.header, hdr);
    assert_eq!(parsed.arguments, vec![a0.as_slice(), a1.as_slice()]);

    let walked = walk_log_record(raw.len(), |off, dst| {
        if off + dst.len() > raw.len() {
            return false;
        }
        dst.copy_from_slice(&raw[off..off + dst.len()]);
        true
    })
    .expect("walk log record");
    assert_eq!(walked.header, hdr);
    assert_eq!(
        walked.argument_bytes,
        vec![a0.len() as u32, a1.len() as u32]
    );

    let frec = FailureRecord {
        id: 4,
        sequence: 9,
        arrival: 1_700_000_000_000_000,
        timer: false,
        handler: 1,
        name: "add".to_string(),
        read_only: false,
        arguments: vec![a0.to_vec()],
        signature_version: 1,
        class: 0,
        reason: "stop".to_string(),
        limit: String::new(),
        limit_value: 0,
        function: 0,
        file: 0,
        start: 10,
        end: 20,
        work: 1_000_000,
        heap: 1_048_576,
        stack: 4096,
        depth: 64,
        saved: 16_777_216,
        program: 42,
        build: [0xbb; 32],
        label: "mo 0.3.0".to_string(),
        schema: [0xcc; 32],
        context_status: FailureContextStatus::Complete,
        context: "x = 1".to_string(),
    };
    let fraw = write_failure_record(&frec).expect("encode failure record");
    let fdec = read_failure_record(&fraw).expect("decode failure record");
    assert_eq!(fdec, frec);
}

#[test]
fn upload_stage_paging_and_hex_helpers() {
    let mut stage = UploadStage::open(10).expect("open stage");
    assert_eq!(stage.expected_total(), 10);
    assert!(!stage.is_complete());
    assert_eq!(stage.push_page(0, b"hello"), UploadPageStatus::Accepted);
    assert_eq!(stage.push_page(0, b"hello"), UploadPageStatus::Duplicate);
    assert_eq!(stage.push_page(0, b"other"), UploadPageStatus::Invalid);
    assert_eq!(stage.push_page(5, b"world"), UploadPageStatus::Accepted);
    assert!(stage.is_complete());
    assert_eq!(stage.bytes(), b"helloworld");

    let digest = sha256_bytes(b"helloworld");
    let hex = hex_encode(&digest);
    assert_eq!(hex_decode_exact::<32>(&hex), Some(digest));
    assert_eq!(sanitize_terminal_text("ok\x1b[31m"), "ok\\x1b[31m");
    assert_eq!(
        sanitize_terminal_text("a\u{009b}b\u{202e}c\x7f"),
        "a\\u{09b}b\\u{202e}c\\x7f"
    );
}

#[test]
fn bearer_validation_and_redacted_debug() {
    assert!(Bearer::from_bytes(b"").is_none());
    assert!(Bearer::from_bytes(b"has space").is_none());
    assert!(Bearer::from_bytes(b"has\nnewline").is_none());

    let b = Bearer::from_bytes(b"SecretToken_0123").expect("valid bearer");
    assert!(b.check(b"SecretToken_0123"));
    assert!(!b.check(b"SecretToken_0124"));
    assert!(!b.check(b"SecretToken_012"));
    let dbg = format!("{b:?}");
    assert!(!dbg.contains("SecretToken"));
}

#[test]
fn store_frame_durability_programs_and_offline_history() {
    let rec = StoreRecord {
        kind: StoreRecordKind::State,
        sequence: 0,
        image: 42,
        bytes: b"state-0".to_vec(),
    };
    let framed =
        encode_frame(rec.kind, rec.sequence, rec.image, &rec.bytes).expect("encode MOST frame");
    let dec = decode_frame(StoreRecordKind::State, &framed).expect("decode MOST frame");
    assert_eq!(dec, rec);

    let dir = std::env::temp_dir().join(format!("mo-service-unit-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("mkdir temp dir");
    let db_path = dir.join("service.sqlite");

    {
        let mut store = ServiceStore::open(Some(&db_path)).expect("open fresh store");
        let (bundle, source) = write_bundle(
            "main.mo",
            &[BundleFile {
                path: "main.mo".to_string(),
                content: b"module Main\n".to_vec(),
            }],
        )
        .expect("encode bundle");
        let (context, context_digest) =
            write_compile_context("", &[], None).expect("encode empty context");
        let prog = StoredProgram {
            ordinal: 0,
            source,
            image: 42,
            build: [0xbb; 32],
            schema: [0xcc; 32],
            bundle,
            context_digest,
            context,
        };
        store
            .record_program(&prog, &rec)
            .expect("record program + snapshot");
        let loaded_prog = store
            .read_program()
            .expect("read program")
            .expect("present");
        assert_eq!(loaded_prog.source, prog.source);
        assert_eq!(loaded_prog.image, 42);

        let log_payload = write_log_record(
            &LogRecordHeader {
                state_checksum: 0,
                arrival: 100,
                handler: 0,
                count: 0,
                timer: false,
            },
            &[],
        )
        .expect("log payload");
        let log_rec = StoreRecord {
            kind: StoreRecordKind::Log,
            sequence: 1,
            image: 42,
            bytes: log_payload,
        };
        store.append(&log_rec).expect("append seq 1");
        let replayed = store.load().expect("load records");
        assert_eq!(replayed.len(), 2);
        assert_eq!(replayed[0].sequence, 0);
        assert_eq!(replayed[1].sequence, 1);
    }

    let hist = read_history(&db_path, false).expect("read offline history");
    assert_eq!(hist.head, 1);
    assert_eq!(hist.log_count, 1);
    assert!(hist.program.is_some());
    assert!(hist.snapshot.is_some());

    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn embedded_protocol_image_compiles_and_initializes() {
    let (img, _fns) = build_protocol_image().expect("embedded protocol compiles cleanly");
    assert!(!img.file().functions.is_empty());
}

#[test]
fn upgrade_protocol_and_compile_error_codes_exercised() {
    use mo_def::ErrorCode;
    use mo_service::upgrade::compile::{CompileJob, CompilePoll};
    use mo_service::upgrade::{BaseCheck, UpgradeChildren, UpgradeReport};
    use std::time::{Duration, Instant};

    // 1. E1416 (UpgradeStaleBase): stale base refused naming the active digest.
    let active = [0x11u8; 32];
    let wrong_base = [0x22u8; 32];
    let cand = [0x33u8; 32];
    let mut rep_stale = UpgradeReport::default();
    assert_eq!(
        rep_stale.check_base(&active, &wrong_base, &cand),
        BaseCheck::StaleBase
    );
    assert_eq!(rep_stale.code, ErrorCode::UpgradeStaleBase as u16);
    assert_eq!(rep_stale.class, "stale_base");
    let stale_json = rep_stale.to_json(None);
    assert!(stale_json.contains("\"code\":\"E1416\""), "{stale_json}");
    assert!(
        stale_json.contains(&hex_encode(&active)),
        "expected active digest in {stale_json}"
    );

    // Retry of already-active candidate returns already_active (not E1416).
    let mut rep_active = UpgradeReport::default();
    assert_eq!(
        rep_active.check_base(&active, &wrong_base, &active),
        BaseCheck::AlreadyActive
    );
    assert!(rep_active.already_active);

    // 2. E1438 (UpgradeUnconfined): Refused children mode rejects with confinement diagnostic.
    let mut rep_unconf = UpgradeReport::default();
    assert!(!rep_unconf.check_children(UpgradeChildren::Refused));
    assert_eq!(rep_unconf.code, ErrorCode::UpgradeUnconfined as u16);
    assert_eq!(rep_unconf.class, "confinement");
    let unconf_json = rep_unconf.to_json(None);
    assert!(unconf_json.contains("\"code\":\"E1438\""), "{unconf_json}");
    assert!(unconf_json.contains("--unconfined"), "{unconf_json}");

    // 3. E1420 (UpgradeCompileBudget): real CompileJob wall-clock budget exhaustion.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("mo-svc-budget-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("create temp dir");
        let script = dir.join("hang.sh");
        std::fs::write(&script, "#!/bin/sh\nwhile :; do :; done\n").expect("write hang.sh");
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o700))
            .expect("chmod hang.sh");
        let (bundle, _) = write_bundle(
            "main.mo",
            &[BundleFile {
                path: "main.mo".to_string(),
                content: b"module Main\n".to_vec(),
            }],
        )
        .expect("write bundle");
        let mut job = CompileJob::start(
            &script, None, bundle, None, None, None, false, false, [0x44; 32], 1,
        )
        .expect("start hang job");
        job.set_deadline_for_test(Instant::now() + Duration::from_millis(80));
        let err = loop {
            match job.poll() {
                CompilePoll::Running => std::thread::sleep(Duration::from_millis(10)),
                CompilePoll::Finished(res) => match *res {
                    Ok(_) => unreachable!("expected E1420 compile budget refusal"),
                    Err(e) => break e,
                },
            }
        };
        assert_eq!(err.0, ErrorCode::UpgradeCompileBudget as u16);
        assert_eq!(err.1, "budget");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
