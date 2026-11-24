use std::fs;
use std::path::{Path, PathBuf};

use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_DIAGNOSTIC_SIZE, MO_LIMIT_DIAGNOSTICS, MO_LIMIT_DIAGNOSTICS_SIZE, MO_LIMIT_FILE_SIZE,
    MO_LIMIT_FIX_EDITS, MO_LIMIT_PATH_SIZE,
};

use crate::{
    BudgetExceeded, DiagBag, DiagText, DiagValidationError, Diagnostic, DisplayMetrics, Edit, Fix,
    Label, LineCol, ModulePathError, NO_FILE, RenderFormat, RenderLimitError, SourceError,
    SourceMap, Span, TerminalEncoding, WorkBudget, edits_overlap, module_path, render_diagnostic,
    render_diagnostic_default,
};

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

const UNICODE16_METRICS: DisplayMetrics = DisplayMetrics {
    next_grapheme: mo_prim::unicode16::next_grapheme_bytes,
    display_width: mo_prim::unicode16::display_width_bytes,
};

fn plain_diag(code: ErrorCode, span: Span, msg: &str) -> Diagnostic {
    Diagnostic::new(code, span, msg)
        .with_reason("Mo never drops a failure silently (reference 8.3).")
}

// ---------------------------------------------------------------------------
// Module paths (100% of tests/module_paths.tsv + tests/test_path.c)
// ---------------------------------------------------------------------------

#[test]
fn module_paths_match_tsv_and_edge_cases() {
    let tsv = fs::read_to_string(repo_root().join("tests/module_paths.tsv")).unwrap();
    let mut rows = 0usize;
    for line in tsv.lines() {
        let mut parts = line.split_whitespace();
        let Some(name) = parts.next() else { continue };
        let expected = parts.next().unwrap();
        let got = module_path(name.as_bytes()).unwrap();
        assert_eq!(got.as_str(), expected, "module name {name}");
        rows += 1;
    }
    assert!(rows >= 10);

    for bad in ["", ".Foo", "Foo.", "Foo..Bar", "foo", "Foo/Bar", "Foo._Bar"] {
        assert!(
            module_path(bad.as_bytes()).is_err(),
            "should reject {bad:?}"
        );
    }
    assert_eq!(module_path(b""), Err(ModulePathError::Empty));

    let limit = MO_LIMIT_PATH_SIZE as usize;
    let exact = vec![b'A'; limit - 3];
    let ok = module_path(&exact).unwrap();
    assert_eq!(ok.as_str().len(), limit);
    let over = vec![b'A'; limit - 2];
    assert_eq!(module_path(&over), Err(ModulePathError::TooLong));
}

// ---------------------------------------------------------------------------
// SourceMap positions, lines, and file reading (tests/test_sources.c)
// ---------------------------------------------------------------------------

fn check_positions(bytes: &[u8], expected: &[(u32, u32)]) {
    assert_eq!(bytes.len() + 1, expected.len());
    let mut sources = SourceMap::new();
    let file = sources
        .add_bytes(b"test.mo".to_vec(), bytes.to_vec())
        .unwrap();
    for (offset, &(line, col)) in expected.iter().enumerate() {
        let pos = sources.position(file, offset as u32).unwrap();
        assert_eq!(
            pos,
            LineCol { line, column: col },
            "offset {offset} in {bytes:?}"
        );
    }
}

#[test]
fn sources_positions_and_line_endings() {
    check_positions(b"ab\ncd", &[(1, 1), (1, 2), (1, 3), (2, 1), (2, 2), (2, 3)]);
    check_positions(
        b"ab\r\ncd",
        &[(1, 1), (1, 2), (1, 3), (1, 3), (2, 1), (2, 2), (2, 3)],
    );
    check_positions(
        b"a\r\n\r\n\nb\n",
        &[
            (1, 1),
            (1, 2),
            (1, 2),
            (2, 1),
            (2, 1),
            (3, 1),
            (4, 1),
            (4, 2),
            (5, 1),
        ],
    );
    check_positions(b"a\rb\r", &[(1, 1), (1, 2), (1, 3), (1, 4), (1, 5)]);
    check_positions(b"", &[(1, 1)]);
    check_positions(b"\n", &[(1, 1), (2, 1)]);
    check_positions(b"\r\n", &[(1, 1), (1, 1), (2, 1)]);
    check_positions(
        b"\tx\t\ty",
        &[(1, 1), (1, 2), (1, 3), (1, 4), (1, 5), (1, 6)],
    );

    // Valid UTF-8 multi-byte scalars.
    check_positions(
        b"\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80x",
        &[
            (1, 1),
            (1, 1),
            (1, 2),
            (1, 2),
            (1, 2),
            (1, 3),
            (1, 3),
            (1, 3),
            (1, 3),
            (1, 4),
            (1, 5),
        ],
    );
    // Invalid UTF-8 bytes count as 1 column each.
    check_positions(b"\xff\x80a", &[(1, 1), (1, 2), (1, 3), (1, 4)]);
    check_positions(b"\xc0\x80", &[(1, 1), (1, 2), (1, 3)]);
    check_positions(b"\xed\xa0\x80", &[(1, 1), (1, 2), (1, 3), (1, 4)]);
    check_positions(
        b"\xf4\x90\x80\x80",
        &[(1, 1), (1, 2), (1, 3), (1, 4), (1, 5)],
    );
    check_positions(
        b"\xef\xbf\xbf\xf4\x8f\xbf\xbf",
        &[
            (1, 1),
            (1, 1),
            (1, 1),
            (1, 2),
            (1, 2),
            (1, 2),
            (1, 2),
            (1, 3),
        ],
    );
}

#[test]
fn sources_line_spans_and_special_file_refusal() {
    let mut sources = SourceMap::new();
    let f = sources
        .add_bytes(
            b"lines.mo".to_vec(),
            b"first\r\n\nthird\n\r\n\rsixth\r".to_vec(),
        )
        .unwrap();
    fn line_str(s: &SourceMap, id: u32, line: u32) -> &[u8] {
        let sp = s.line_span(id, line).unwrap();
        &s.get(id).unwrap().bytes()[sp.start as usize..sp.end as usize]
    }
    assert_eq!(sources.get(f).unwrap().line_count(), 5);
    assert_eq!(line_str(&sources, f, 1), b"first");
    assert_eq!(line_str(&sources, f, 2), b"");
    assert_eq!(line_str(&sources, f, 3), b"third");
    assert_eq!(line_str(&sources, f, 4), b"");
    assert_eq!(line_str(&sources, f, 5), b"\rsixth\r");

    // Refuses /dev/null and directories without blocking.
    assert!(matches!(
        sources.add_file(Path::new("/dev/null")),
        Err(SourceError::Unreadable(_))
    ));
    assert!(matches!(
        sources.add_file(&repo_root()),
        Err(SourceError::Unreadable(_))
    ));

    // Refuses files larger than MO_LIMIT_FILE_SIZE.
    let over = vec![b'x'; (MO_LIMIT_FILE_SIZE as usize) + 1];
    assert_eq!(
        sources.add_bytes(b"big.mo".to_vec(), over),
        Err(SourceError::FileTooBig)
    );
}

// ---------------------------------------------------------------------------
// DiagBag validation, sorting, limits (tests/test_diag.c)
// ---------------------------------------------------------------------------

#[test]
fn diag_bag_validation_sorting_and_limits() {
    let mut bag = DiagBag::new();
    assert!(!bag.has_any());

    // Touching edits and insertions at replacement boundaries do not overlap.
    let e_ins_l = Edit::new(Span::new(0, 10, 10), "(");
    let e_rep = Edit::new(Span::new(0, 10, 20), "x");
    let e_ins_r = Edit::new(Span::new(0, 20, 20), ")");
    assert!(!edits_overlap(&e_ins_l, &e_rep));
    assert!(!edits_overlap(&e_rep, &e_ins_r));
    assert!(!edits_overlap(&e_ins_l, &e_ins_r));

    // Same-point insertions and overlapping replacements do overlap.
    assert!(edits_overlap(
        &Edit::new(Span::new(0, 7, 7), "a"),
        &Edit::new(Span::new(0, 7, 7), "b")
    ));
    assert!(edits_overlap(
        &Edit::new(Span::new(0, 10, 20), "a"),
        &Edit::new(Span::new(0, 15, 15), "b")
    ));
    assert!(edits_overlap(
        &Edit::new(Span::new(0, 10, 20), "a"),
        &Edit::new(Span::new(0, 15, 25), "b")
    ));

    // Invalid diagnostics rejected cleanly.
    let mut bad_reason = Diagnostic::new(ErrorCode::FailureIgnored, Span::new(0, 1, 2), "x");
    assert_eq!(
        bag.push(bad_reason.clone()),
        Err(DiagValidationError::MissingReasonForCitedCode)
    );
    bad_reason.reason = Some("reason".into());
    bad_reason.section = Some("8.2");
    assert_eq!(
        bag.push(bad_reason),
        Err(DiagValidationError::SectionMismatch)
    );
    let backward = plain_diag(ErrorCode::FailureIgnored, Span::new(0, 5, 4), "back");
    assert_eq!(bag.push(backward), Err(DiagValidationError::BackwardSpan));

    let empty_fix = plain_diag(ErrorCode::FailureIgnored, Span::new(0, 0, 1), "f")
        .with_fix(Fix::new(None, Vec::new()));
    assert_eq!(bag.push(empty_fix), Err(DiagValidationError::EmptyFix));

    let too_many_edits: Vec<Edit> = (0..=(MO_LIMIT_FIX_EDITS as u32))
        .map(|i| Edit::new(Span::new(0, i, i), "x"))
        .collect();
    let over_fix = plain_diag(ErrorCode::FailureIgnored, Span::new(0, 0, 1), "f")
        .with_fix(Fix::new(None, too_many_edits));
    assert_eq!(
        bag.push(over_fix),
        Err(DiagValidationError::TooManyFixEdits)
    );

    // Stable sorting by (file, start).
    let added = [
        (1, 50, "f1 50"),
        (0, 90, "f0 90"),
        (0, 10, "f0 10 first"),
        (2, 0, "f2 0"),
        (0, 10, "f0 10 second"),
        (1, 5, "f1 5"),
        (0, 90, "f0 90 again"),
        (0, 0, "f0 0"),
    ];
    for (file, start, msg) in added {
        assert!(
            bag.push(plain_diag(
                ErrorCode::FailureIgnored,
                Span::new(file, start, start + 3),
                msg
            ))
            .unwrap()
        );
    }
    bag.sort();
    let got_msgs: Vec<String> = bag.items().iter().map(|d| d.message.to_string()).collect();
    assert_eq!(
        got_msgs,
        vec![
            "f0 0",
            "f0 10 first",
            "f0 10 second",
            "f0 90",
            "f0 90 again",
            "f1 5",
            "f1 50",
            "f2 0"
        ]
    );

    // Count limit enforcement.
    let mut full = DiagBag::new();
    for i in 0..(MO_LIMIT_DIAGNOSTICS as u32) {
        assert!(
            full.push(plain_diag(
                ErrorCode::FailureIgnored,
                Span::new(0, i, i),
                "kept"
            ))
            .unwrap()
        );
    }
    assert!(
        !full
            .push(plain_diag(
                ErrorCode::FailureIgnored,
                Span::new(0, 0, 0),
                "drop"
            ))
            .unwrap()
    );
    assert_eq!(full.len(), MO_LIMIT_DIAGNOSTICS as usize);
    assert_eq!(full.dropped(), 1);

    // Memory limit enforcement: oversized diagnostic drops cleanly and consumes no budget.
    let mut mem_bag = DiagBag::new();
    let used_before = mem_bag.used_bytes();
    let huge = Diagnostic::new(
        ErrorCode::LineTooLong,
        Span::new(0, 0, 0),
        vec![b'm'; MO_LIMIT_DIAGNOSTICS_SIZE as usize],
    );
    assert!(!mem_bag.push(huge).unwrap());
    assert_eq!(mem_bag.len(), 0);
    assert_eq!(mem_bag.dropped(), 1);
    assert_eq!(mem_bag.used_bytes(), used_before);
    assert!(mem_bag.has_any());
}

// ---------------------------------------------------------------------------
// Golden text & JSON rendering (tests/cli/render-{text,json}.golden + test_cli.c)
// ---------------------------------------------------------------------------

fn build_golden_fixture() -> (SourceMap, Diagnostic) {
    let mut sources = SourceMap::new();
    let source = b"\t\xe7\x95\x8ce\xcc\x81\r\nsecond\nthird\nfourth\nfifth\nsixth\n";
    let f0 = sources
        .add_bytes(b"bad\xff\"\\.mo".to_vec(), source.to_vec())
        .unwrap();
    let f1 = sources
        .add_bytes(b"other.mo".to_vec(), b"abc\nxyz".to_vec())
        .unwrap();
    assert_eq!(f0, 0);
    assert_eq!(f1, 1);

    let diag = Diagnostic {
        code: ErrorCode::NameClash,
        section: Some("10.3"),
        message: DiagText::from(b"quoted \"headline\"\\\t\xff".as_slice()),
        reason: Some(DiagText::from(
            "A clash between names is an error (reference 10.3).",
        )),
        primary: Label::with_text(Span::new(0, 1, source.len() as u32), "wide span"),
        secondary: vec![
            Label::with_text(
                Span::new(0, source.len() as u32, source.len() as u32),
                "insert at EOF",
            ),
            Label::bare(Span::new(NO_FILE, 0, 0)),
        ],
        fixes: vec![Fix::new(
            Some(DiagText::from("replace together")),
            vec![
                Edit::new(Span::new(1, 2, 3), "C\nZ"),
                Edit::new(Span::new(1, 1, 2), "B"),
                Edit::new(Span::new(1, 1, 1), "+"),
            ],
        )],
    };
    (sources, diag)
}

#[test]
fn render_matches_text_and_json_goldens_byte_for_byte() {
    let (sources, diag) = build_golden_fixture();

    let got_text = render_diagnostic_default(
        &sources,
        &diag,
        RenderFormat::Text,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    )
    .unwrap();
    let want_text = fs::read(repo_root().join("tests/cli/render-text.golden")).unwrap();
    assert_eq!(
        got_text,
        want_text,
        "text golden mismatch:\nGOT:\n{}\nWANT:\n{}",
        String::from_utf8_lossy(&got_text),
        String::from_utf8_lossy(&want_text)
    );

    let got_json = render_diagnostic_default(
        &sources,
        &diag,
        RenderFormat::Json,
        TerminalEncoding::Utf8,
        UNICODE16_METRICS,
    )
    .unwrap();
    let want_json = fs::read(repo_root().join("tests/cli/render-json.golden")).unwrap();
    assert_eq!(
        got_json,
        want_json,
        "json golden mismatch:\nGOT:\n{}\nWANT:\n{}",
        String::from_utf8_lossy(&got_json),
        String::from_utf8_lossy(&want_json)
    );
}

#[test]
fn render_covers_long_lines_utf8_ascii_controls_split_crlf_and_limits() {
    // 1. Long line + multi-line elision (test_cli.c:long_line)
    let mut sources = SourceMap::new();
    let mut long_src = vec![b'a'; 600];
    long_src[500] = b'Z';
    let f_long = sources.add_bytes(b"long.mo".to_vec(), long_src).unwrap();
    let d_long = Diagnostic::new(
        ErrorCode::ToolUnavailable,
        Span::new(f_long, 500, 501),
        "long line",
    )
    .with_primary_text("here");
    let out = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_long,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert!(out.contains("long.mo:1:501"));
    assert!(out.contains("  1 | ...aaaaaaaaaaaaZ"));
    assert!(out.contains("    |                ^ here"));

    let f_rows = sources
        .add_bytes(
            b"rows.mo".to_vec(),
            b"X\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\nX\n".to_vec(),
        )
        .unwrap();
    let mut d_rows = d_long;
    d_rows.primary.span = Span::new(f_rows, 14, 29);
    let out = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_rows,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert!(
        out.contains("  9 | X\n    | ^\n     | ...\n  14 | X\n     | ^\n  15 | X\n     | ^ here\n")
    );

    // 2. UTF-8 display width & grapheme clusters (test_cli.c:utf8_display)
    let f_utf8 = sources
        .add_bytes(
            "café/main.mo".as_bytes().to_vec(),
            "a\té界e\u{0301}🇫🇷👍🏽1️⃣ Z\n".as_bytes().to_vec(),
        )
        .unwrap();
    let d_utf8 = Diagnostic::new(
        ErrorCode::NameUnknown,
        Span::new(f_utf8, 34, 35),
        "UTF-8 café",
    )
    .with_primary_text("日本語")
    .with_secondary(Label::with_text(Span::new(f_utf8, 5, 6), "inside UTF-8"))
    .with_secondary(Label::with_text(Span::new(f_utf8, 10, 18), "flag"))
    .with_secondary(Label::with_text(Span::new(f_utf8, 18, 26), "skin tone"))
    .with_secondary(Label::with_text(Span::new(f_utf8, 26, 33), "keycap"));
    let out = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_utf8,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert!(out.contains("UTF-8 café  café/main.mo:1:15"));
    assert!(out.contains("  1 | a   é界e\u{0301}🇫🇷👍🏽1️⃣ Z\n"));
    assert!(out.contains("    |                ^ 日本語\n"));
    assert!(out.contains("    |      ^^ inside UTF-8\n"));
    assert!(out.contains("    |         ^^ flag\n"));
    assert!(out.contains("    |           ^^ skin tone\n"));
    assert!(out.contains("    |             ^^ keycap\n"));

    // 3. ASCII terminal encoding vs UTF-8 JSON (test_cli.c:ascii_locale)
    let f_ascii = sources
        .add_bytes("café.mo".as_bytes().to_vec(), "a\téÛ\n".as_bytes().to_vec())
        .unwrap();
    let d_ascii = Diagnostic::new(ErrorCode::ToolUnavailable, Span::new(f_ascii, 4, 6), "José")
        .with_primary_text("Û")
        .with_fix(Fix::new(
            Some("日本語".into()),
            vec![Edit::new(Span::new(f_ascii, 2, 4), "界")],
        ));
    let out_ascii = render_diagnostic_default(
        &sources,
        &d_ascii,
        RenderFormat::Text,
        TerminalEncoding::Ascii,
        UNICODE16_METRICS,
    )
    .unwrap();
    assert!(out_ascii.iter().all(|&b| b < 128));
    let out_ascii_str = String::from_utf8(out_ascii).unwrap();
    assert!(out_ascii_str.contains("Jos\\u{0e9}  caf\\u{0e9}.mo:1:4"));
    assert!(out_ascii_str.contains("  1 | a   \\u{0e9}\\u{0db}\n"));
    assert!(out_ascii_str.contains("    |            ^^^^^^^ \\u{0db}\n"));
    assert!(out_ascii_str.contains("Fix: \\u{65e5}\\u{672c}\\u{8a9e}"));
    assert!(out_ascii_str.contains("  1 | a   \\u{754c}\\u{0db}\n"));

    let out_json = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_ascii,
            RenderFormat::Json,
            TerminalEncoding::Ascii,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert!(out_json.contains("\"headline\":\"José\""));

    // 4. Control & bidi escaping in text & JSON (test_cli.c:control_display)
    let controls: &[u8] = b"\x01\t\r\n\x7f\xff\xc2\x85\xc2\xad\xd8\x9c\xe1\xa0\x8e\
\xe2\x80\x8b\xe2\x80\xa8\xe2\x81\xa0\xe2\x81\xa6\
\xef\xbb\xbf\xef\xbf\xb9\xf3\xa0\x80\x81\xf3\xa0\x80\xa0";
    let d_ctrl = Diagnostic::new(ErrorCode::ToolUnavailable, Span::NONE, controls);
    let ctrl_text = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_ctrl,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert_eq!(
        ctrl_text,
        concat!(
            "error E1400: \\x01    \\x0d\\x0a\\x7f\\xff\\u{085}\\u{0ad}\\u{61c}\\u{180e}",
            "\\u{200b}\\u{2028}\\u{2060}\\u{2066}\\u{feff}\\u{fff9}\\u{e0001}\\u{e0020}",
            "  <unavailable>\n\n\n"
        )
    );
    let ctrl_json = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_ctrl,
            RenderFormat::Json,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert!(ctrl_json.contains(
        "\\u0001\\u0009\\u000d\\u000a\\u007f\\u00ff\\u0085\\u00ad\\u061c\
\\u180e\\u200b\\u2028\\u2060\\u2066\\ufeff\\ufff9\\udb40\\udc01\\udb40\\udc20"
    ));

    // 5. Split CRLF fix (test_cli.c:split_crlf_fix)
    let f_crlf = sources
        .add_bytes(b"split.mo".to_vec(), b"Z\r\n\r\n".to_vec())
        .unwrap();
    let d_crlf = Diagnostic::new(
        ErrorCode::ToolUnavailable,
        Span::new(f_crlf, 0, 1),
        "split CRLF",
    )
    .with_fix(Fix::new(
        Some("replace".into()),
        vec![Edit::new(Span::new(f_crlf, 0, 2), "A")],
    ));
    let crlf_text = String::from_utf8(
        render_diagnostic_default(
            &sources,
            &d_crlf,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .unwrap(),
    )
    .unwrap();
    assert_eq!(
        crlf_text,
        "error E1400: split CRLF  split.mo:1:1\n\n  1 | Z\n    | ^\n\nFix: replace\n  split.mo:1:1\n  1 | A\n\n"
    );

    // 6. Byte limit refusal (test_cli.c:omission)
    let small = Diagnostic::new(ErrorCode::ToolUnavailable, Span::NONE, "cannot run yet");
    assert_eq!(
        render_diagnostic(
            &sources,
            &small,
            RenderFormat::Json,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
            20,
        ),
        Err(RenderLimitError { limit: 20 })
    );
    let too_large = Diagnostic::new(
        ErrorCode::ToolUnavailable,
        Span::NONE,
        vec![b'x'; MO_CLI_DIAGNOSTIC_SIZE as usize],
    );
    assert!(
        render_diagnostic_default(
            &sources,
            &too_large,
            RenderFormat::Text,
            TerminalEncoding::Utf8,
            UNICODE16_METRICS,
        )
        .is_err()
    );
}

#[test]
fn work_budget_tracks_and_refuses_overflow() {
    let mut b = WorkBudget::new(3);
    assert_eq!(b.remaining(), 3);
    b.step().unwrap();
    b.charge(2).unwrap();
    assert_eq!(b.used(), 3);
    assert_eq!(b.remaining(), 0);
    assert_eq!(b.step(), Err(BudgetExceeded { limit: 3 }));
    assert_eq!(b.charge(u64::MAX), Err(BudgetExceeded { limit: 3 }));
}
