use std::path::{Path, PathBuf};
use std::time::Instant;

use mo_base::{DiagBag, ErrorCode, Span};
use mo_def::MO_CORE_TEXT_VERSION;
use mo_def::core_ops::{CoreIntOp, CoreMapNullaryOp};
use mo_def::limits::{MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES, MO_LIMIT_CORE_TEXT_SIZE};

use crate::{
    Arm, BodyMeta, Cap, ClosureMeta, ConstDecl, ConstMeta, CoreArena, CoreDocument, CoreInteger,
    CoreNatural, CorePrintError, CoreTextKind, Expr, ExprId, FnDecl, FnKind, FnMeta, Lit, MapOp,
    Metadata, ModuleMeta, Prim, Program, Selection, Service, ServiceTimer, ServiceTool, Services,
    SourceMetadata, Ty, Value, check_program, check_program_services, detect_kind, metadata_valid,
    parse_document, parse_program, parse_run, print_document, print_program, print_run,
};

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn round_trip_bytes(kind: CoreTextKind, input: &[u8], expected: &[u8]) {
    let mut diags = DiagBag::new();
    let parsed = parse_document(kind, input, &mut diags);
    assert!(
        parsed.is_some() && !diags.has_any(),
        "parse failed for kind {:?}: {:?}",
        kind,
        diags.items()
    );
    let parsed = parsed.unwrap();
    let printed = print_document(&parsed.arena, &parsed.value).unwrap();
    assert_eq!(
        printed.as_bytes(),
        expected,
        "canonical print mismatch for kind {:?}",
        kind
    );
    let again = parse_document(kind, printed.as_bytes(), &mut diags).unwrap();
    assert!(!diags.has_any());
    let printed_again = print_document(&again.arena, &again.value).unwrap();
    assert_eq!(printed_again.as_bytes(), expected);
}

fn canonical(kind: CoreTextKind, payload: &str) {
    let text = format!("(core {MO_CORE_TEXT_VERSION} {payload})\n");
    round_trip_bytes(kind, text.as_bytes(), text.as_bytes());
}

fn malformed(kind: CoreTextKind, input: &[u8], code: ErrorCode) {
    let mut diags = DiagBag::new();
    let parsed = parse_document(kind, input, &mut diags);
    assert!(
        parsed.is_none(),
        "expected parse failure {:?} for {:?}",
        code,
        String::from_utf8_lossy(input)
    );
    assert_eq!(
        diags.len(),
        1,
        "expected 1 diagnostic for {:?}, got {:?}",
        String::from_utf8_lossy(input),
        diags.items()
    );
    assert_eq!(
        diags.items()[0].code,
        code,
        "wrong code for {:?}: got {:?} ({})",
        String::from_utf8_lossy(input),
        diags.items()[0].code,
        diags.items()[0].message.as_str_lossy()
    );
    assert!((diags.items()[0].primary.span.end as usize) <= input.len());
}

fn check_program_with_services(
    payload: &str,
    expected_code: Option<ErrorCode>,
    services: Option<&Services>,
) {
    let input = format!("(core {MO_CORE_TEXT_VERSION} {payload})\n");
    let mut diags = DiagBag::new();
    let parsed = parse_program(input.as_bytes(), &mut diags);
    assert!(
        parsed.is_some() && !diags.has_any(),
        "failed to parse program: {:?}\ninput: {input}",
        diags.items()
    );
    let parsed = parsed.unwrap();
    let ok = check_program_services(&parsed.arena, &parsed.value, services, &mut diags);
    assert_eq!(
        ok,
        expected_code.is_none(),
        "unexpected check verdict for {payload}: ok={ok}, diags={:?}",
        diags.items()
    );
    if let Some(code) = expected_code {
        assert_eq!(diags.len(), 1, "expected 1 diag, got {:?}", diags.items());
        let d = &diags.items()[0];
        assert_eq!(d.code, code, "wrong diag code: {:?} vs {:?}", d.code, code);
        assert!(d.section.is_none());
        assert!(d.reason.is_some());
        assert!(
            d.message.as_str_lossy().contains("bug in Mo"),
            "missing 'bug in Mo' in {}",
            d.message.as_str_lossy()
        );
        assert_eq!(d.primary.span.file, u32::MAX);
        assert_eq!(d.primary.span.start, 0);
        assert_eq!(d.primary.span.end, 0);
        if code == ErrorCode::CoreCheckType {
            assert!(
                d.primary
                    .text
                    .as_ref()
                    .unwrap()
                    .as_str_lossy()
                    .contains("expected ")
            );
            assert!(
                d.primary
                    .text
                    .as_ref()
                    .unwrap()
                    .as_str_lossy()
                    .contains("found ")
            );
        }
    } else {
        assert!(diags.is_empty(), "unexpected diags: {:?}", diags.items());
    }
}

fn check_prog(payload: &str, expected_code: Option<ErrorCode>) {
    check_program_with_services(payload, expected_code, None);
}

fn expression(ret: &str, body: &str, expected_code: Option<ErrorCode>) {
    let payload = format!(
        "(Program () ((FunDecl \"main\" () {ret} {body} \"Main\" (function) 0)) () \"main\" (noSource))"
    );
    check_prog(&payload, expected_code);
}

#[test]
fn test_core_fixtures_round_trip_and_typecheck() {
    let dir = repo_root().join("tests/core");
    let mut entries: Vec<PathBuf> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().and_then(|s| s.to_str()) == Some("core"))
        .collect();
    entries.sort();
    assert_eq!(entries.len(), 15);
    for path in entries {
        let bytes = std::fs::read(&path).unwrap();
        assert_eq!(detect_kind(&bytes), Some(CoreTextKind::Run), "{path:?}");
        round_trip_bytes(CoreTextKind::Run, &bytes, &bytes);
        let mut diags = DiagBag::new();
        let parsed = parse_run(&bytes, &mut diags).unwrap();
        assert!(
            check_program(&parsed.arena, &parsed.value.program, &mut diags),
            "fixture {path:?} failed typecheck: {:?}",
            diags.items()
        );
        assert!(!diags.has_any());
    }
}

#[test]
fn test_fuzz_core_text_corpus_round_trip() {
    let dir = repo_root().join("fuzz/corpus/fuzz_core_text");
    let cases = [
        ("cap_state.core", CoreTextKind::CapState),
        ("command.core", CoreTextKind::Primitive),
        ("crypto.core", CoreTextKind::Primitive),
        ("entries.core", CoreTextKind::Primitive),
        ("fake_io.core", CoreTextKind::Primitive),
        ("io.core", CoreTextKind::Primitive),
        ("literal.core", CoreTextKind::Literal),
        ("metadata.core", CoreTextKind::Program),
        ("network.core", CoreTextKind::Primitive),
        ("outcome.core", CoreTextKind::Outcome),
        ("primitive.core", CoreTextKind::Primitive),
        ("program.core", CoreTextKind::Program),
        ("random_state.core", CoreTextKind::RandomState),
        ("report.core", CoreTextKind::Report),
        ("secure_random.core", CoreTextKind::Primitive),
        ("simulation.core", CoreTextKind::Primitive),
        ("sqlite.core", CoreTextKind::Primitive),
        ("type.core", CoreTextKind::Type),
        ("unicode16.core", CoreTextKind::Primitive),
        ("value.core", CoreTextKind::Value),
    ];
    let actual_count = std::fs::read_dir(&dir)
        .unwrap()
        .filter(|e| e.as_ref().unwrap().path().is_file())
        .count();
    assert_eq!(actual_count, cases.len());
    for (name, kind) in cases {
        let bytes = std::fs::read(dir.join(name)).unwrap();
        assert_eq!(detect_kind(&bytes), Some(kind), "detect_kind for {name}");
        round_trip_bytes(kind, &bytes, &bytes);
    }
}

#[test]
fn test_shapes() {
    canonical(
        CoreTextKind::Type,
        "(tuple ((int) (bigInt) (byte) (decimal) (float) (bool) (char) (text) (bytes) (unit) \
         (range) (time) (duration) (ordering) (fileError) (entry) (entryKind) (list (byte)) \
         (map (text) (int)) (set (time)) (data \"Point[β]\") (option (char)) \
         (result (unit) (fileError)) (fn ((int) (text)) (bool)) (cap (console))))",
    );
    for cap in [
        "(console)",
        "(files)",
        "(readFiles)",
        "(clock)",
        "(random)",
        "(args)",
        "(env)",
        "(network)",
        "(connection)",
        "(sqlite)",
        "(database)",
        "(command)",
        "(secureRandom)",
        "(file)",
        "(process)",
    ] {
        canonical(CoreTextKind::Cap, cap);
    }
    let lits = [
        "(int -9223372036854775809)",
        "(bigInt 12345678901234567890123456789012345678901234567890)",
        "(byte -1)",
        "(decimal -170141183460469231731687303715884105728)",
        "(float 0x8000000000000000)",
        "(float 0x7ff8000000000042)",
        "(bool true)",
        "(bool false)",
        "(char \"😀\")",
        "(char \"\\u{0}\")",
        "(text \"\\u{1}\\u{1f}\\u{7f}\\n\\r\\t\\\"\\\\β\")",
        "(bytes (-1 0 255 256 999999999999999999999999999999))",
        "(unit)",
        "(time -999999999999999999999)",
        "(duration 3)",
    ];
    for lit in lits {
        canonical(CoreTextKind::Literal, lit);
        canonical(CoreTextKind::Value, lit);
    }
    canonical(
        CoreTextKind::Value,
        "(tuple ((range -3 7) (tuple ()) (list ((int 5) (int 1))) \
         (map (((text \"b\") (int 8)) ((text \"a\") (int 2)))) (set ((int 2) (int 1))) \
         (con \"Point\" ((int 4) (int 9))) \
         (closure ((\"x\" (int 3)) (\"x\" (text \"outer\"))) ((\"y\" (int))) (var \"x\") \
         ((\"C\" (bool true)))) \
         (cap (files) (none) (none)) (cap (readFiles) (some \"\") (some 0)) \
         (cap (readFiles) (some \"notes\") (some 999999999999999999999999999999))))",
    );
    canonical(CoreTextKind::Value, "(cap (console) (none) (some 2))");
    canonical(
        CoreTextKind::CapState,
        "(CapState (\"fake arg\") ((\"K\" \"first\") (\"K\" \"second\")) (\"in\\r\\n\") \
         (\"out\\n\") (\"err\") ((\"z\" \"last\") (\"a\" \"first\")) \
         -999999999999999999999999999999 \
         (RandomState 0x0000000000000000 0x8000000000000000 0xffffffffffffffff \
         0x0123456789abcdef) (\"empty/β\" \"empty\" \"empty\"))",
    );
    canonical(
        CoreTextKind::RandomState,
        "(RandomState 0x0000000000000001 0x8000000000000000 0xffffffffffffffff 0x0123456789abcdef)",
    );
    canonical(
        CoreTextKind::World,
        "(World (\"b\" \"a\") ((\"K\" \"first\") (\"K\" \"second\")) (\"line\\r\\n\") \
         (\"old out\\n\") (\"old err\") ((\"z\" \"last\") (\"a\" \"first\") (\"z\" \"duplicate\")) \
         -123456789012345678901234567890 (RandomState 0x0000000000000001 0x0000000000000002 \
         0x0000000000000003 0x0000000000000004) (\"real empty\") \
         ((CapState (\"fake\") () (\"input\\r\\n\") (\"fake out\") (\"fake err\") \
         ((\"b\" \"2\") (\"a\" \"1\") (\"b\" \"duplicate\")) -99 (RandomState 0x0000000000000005 \
         0x0000000000000006 0x0000000000000007 0x0000000000000008) (\"fake empty\")) \
         (CapState () () () () () () 0 (RandomState 0x0000000000000000 0x0000000000000000 \
         0x0000000000000000 0x0000000000000000) ())))",
    );
    canonical(
        CoreTextKind::Program,
        "(Program ((DataDecl \"Point\" ((Ctor \"Point\" ((\"x\" (int)) (\"y\" (text)))))) \
         (DataDecl \"Choice\" ((Ctor \"A\" ()) (Ctor \"B\" ((\"0\" (byte))))))) \
         ((FunDecl \"main\" ((\"v\" (int))) (text) (global \"C\") \"Main.Mod\" (function) 1) \
         (FunDecl \"t\" () (unit) (fail (lit (text \"detail\"))) \"Spec\" (test) 0) \
         (FunDecl \"e\" () (unit) (lit (unit)) \"Examples\" (example) 0) \
         (FunDecl \"p\" ((\"result\" (text))) (int) (lit (int 1)) \"Spec\" (promise) 1) \
         (FunDecl \"a\" () (int) (lit (int 2)) \"Spec\" (anchor) 0)) \
         ((ConstDecl \"C\" (text) (lit (text \"constant\")))) \"main\" (noSource))",
    );
    canonical(
        CoreTextKind::Expr,
        "(join \"loop\" ((\"i\" (int))) (if_ (lit (bool true)) (jump \"loop\" ((var \"i\"))) \
         (stop \"done\")) \
         (let_ \"f\" (lam ((\"x\" (text))) (var \"x\")) \
         (case_ (con (data \"Choice\") \"B\" ((lit (byte 255)))) \
         ((\"A\" () (call \"zero\" ())) (\"B\" (\"b\") (app (var \"f\") \
         ((proj (tuple ((prim (not) ((lit (bool false)))) (global \"C\"))) 1))))))))",
    );
    for bug in [
        "(overflow \"add\")",
        "(divByZero)",
        "(stop \"reason\")",
        "(outside \"path\")",
        "(repeatedKey)",
        "(fileFolderConflict \"a/β\")",
        "(filePathInvalid \"a\\u{0}/../β\")",
    ] {
        canonical(CoreTextKind::Bug, bug);
    }
    for outcome in [
        "(ok (cap (random) (some \"\") (some 0)) (World () () () (\"effect\") () () -7 \
         (RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) \
         () ((CapState () () () () () () 17 (RandomState 0x0000000000000005 \
         0x0000000000000006 0x0000000000000007 0x0000000000000008) ()))))",
        "(stopped (stop \"bug\") (World () () () () (\"error\") () 0 \
         (RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) \
         () ((CapState () () () (\"before stop\") () () 0 (RandomState 0x0000000000000005 \
         0x0000000000000006 0x0000000000000007 0x0000000000000008) ()))))",
        "(failed \"detail\" (World () () () () () ((\"a\" \"b\")) 0 \
         (RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) \
         () ((CapState () () () () (\"before failure\") () 0 (RandomState 0x0000000000000005 \
         0x0000000000000006 0x0000000000000007 0x0000000000000008) ()))))",
        "(jumped \"loop\" ((int 1) (text \"next\")) (World () () () () () () 0 (RandomState \
         0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) () ()))",
        "(wrong \"invariant\")",
        "(outOfFuel)",
    ] {
        canonical(CoreTextKind::Outcome, outcome);
    }
    for report in [
        "(returned \"[\\\"a\\\", \\\"b\\\\n\\\"]\" (effects (\"a\\nb\") \
         (\"error\") ((\"b\" \"2\") (\"a\" \"1\")) (\"real folder\") \
         ((CapState () () () (\"fake out\") () () 0 (RandomState 0x0000000000000001 \
         0x0000000000000002 0x0000000000000003 0x0000000000000004) (\"fake folder\")))))",
        "(stopped (divByZero) (effects (\"before\") () () () \
         ((CapState () () () () (\"fake err\") () 0 (RandomState 0x0000000000000001 \
         0x0000000000000002 0x0000000000000003 0x0000000000000004) ()))))",
        "(failed \"detail\" (effects () () () () \
         ((CapState () () () () () ((\"fake\" \"file\")) 0 (RandomState 0x0000000000000001 \
         0x0000000000000002 0x0000000000000003 0x0000000000000004) ()))))",
        "(outOfFuel)",
        "(wrong \"internal\")",
        "(parseError \"input\")",
        "(illTyped \"body\")",
        "(invalidRun \"selection\")",
        "(ioError \"unavailable\")",
    ] {
        canonical(CoreTextKind::Report, report);
    }
    canonical(
        CoreTextKind::Effects,
        "(effects (\"out\") (\"err\") ((\"z\" \"1\") (\"a\" \"2\")) (\"z/folder\") ())",
    );
    canonical(
        CoreTextKind::Selection,
        "(entry ((int 3) (cap (args) (some \"\") (none))))",
    );
    canonical(CoreTextKind::Selection, "(test \"a named example\")");
    canonical(
        CoreTextKind::Run,
        "(Run (Program () () () \"missing\" (noSource)) (World () () () () () () 0 (RandomState \
         0x0000000000000001 0x0000000000000002 0x0000000000000003 0x0000000000000004) () ()) \
         (test \"named\") 123456789012345678901234567890)",
    );
}

#[test]
fn test_operations() {
    let families = [
        (
            "int",
            "add sub mul div rem neg abs min max pow floorDiv floorMod checkedAdd checkedSub \
             checkedMul wrappingAdd wrappingSub wrappingMul saturatingAdd saturatingSub \
             saturatingMul toText toDecimal toBig toFloat toChar toByte seconds milliseconds \
             minutes hours days",
        ),
        (
            "big",
            "add sub mul div rem neg abs min max pow floorDiv floorMod toInt toText",
        ),
        ("byte", "toInt toText"),
        (
            "dec",
            "add sub mul div rem neg abs min max floor ceil round toInt toText toFixed toFloat",
        ),
        (
            "float",
            "add sub mul div rem neg abs sqrt floor ceil round isNan isInfinite toDecimal toText",
        ),
        (
            "char",
            "code isDigit isLetter isWhiteSpace lower upper toText",
        ),
        (
            "text",
            "concat byteCount charCount isEmpty chars fromChars lines words split trim \
             trimStart trimEnd startsWith endsWith contains dropPrefix dropSuffix replace \
             repeat padLeft padRight lower upper toInt toDecimal toFloat join bytes",
        ),
        ("bytes", "size get decodeUtf8 slice join"),
        (
            "unicode16",
            "graphemes displayWidth normalizeNfc lowercase uppercase letter digit",
        ),
        ("range", "make start end toList contains toText"),
        (
            "time",
            "sub add subDur checkedAdd toText parse durAdd durSub durMul durNeg \
             inMilliseconds inSeconds durToText",
        ),
        (
            "list (text)",
            "size isEmpty get first last append prepend concat reverse take drop set \
             removeAt uncons enumerate contains unique",
        ),
        (
            "map (text) (int)",
            "empty size isEmpty get has set remove keys values entries ofList",
        ),
        (
            "set (int)",
            "empty size isEmpty has add remove union intersection difference toList ofList",
        ),
        (
            "io",
            "argsList envGet print printError readLine printed printedErrors writeText \
             appendText remove makeFolder readOnly now sleep randomInt consoleFake \
             consoleFakeInput filesInMemory clockFixed randomSeeded argsOf envOf",
        ),
    ];
    for (prefix, names) in families {
        for name in names.split_whitespace() {
            canonical(CoreTextKind::Primitive, &format!("({prefix} ({name}))"));
        }
    }
    for extra in [
        "(not)",
        "(eq (map (text) (list (byte))))",
        "(structural (map (text) (list (byte))))",
        "(structural (fn ((cap (files))) (unit)))",
        "(list (byte) (lit 0))",
        "(list (byte) (lit 9999999999999999999999999999))",
        "(list (byte) (zip (text)))",
        "(map (text) (int) (lit 2))",
        "(io (readText true))",
        "(io (readText false))",
        "(io (readBytes true))",
        "(io (readBytes false))",
        "(io (list true))",
        "(io (list false))",
        "(io (entries true))",
        "(io (entries false))",
        "(io (exists true))",
        "(io (exists false))",
        "(io (within true))",
        "(io (within false))",
    ] {
        canonical(CoreTextKind::Primitive, extra);
    }
    for base in [
        "int", "bigInt", "byte", "decimal", "float", "bool", "char", "text", "bytes", "time",
        "duration",
    ] {
        for cmp in ["lt", "le", "gt", "ge"] {
            canonical(CoreTextKind::Primitive, &format!("(cmp ({base}) ({cmp}))"));
        }
    }
}

#[test]
fn test_normalization() {
    round_trip_bytes(
        CoreTextKind::Literal,
        b"; before\r\n(core\t0.11(text\"\\u{000000}\\u{00000A}\\u{00007F}\\u{01F600}e\xcc\x81\")); after",
        "(core 0.11 (text \"\\u{0}\\n\\u{7f}😀e\u{0301}\"))\n".as_bytes(),
    );
    round_trip_bytes(
        CoreTextKind::Literal,
        b"(core 0.11(bytes(-0000 00000255 -00123456789012345678901234567890)))",
        b"(core 0.11 (bytes (0 255 -123456789012345678901234567890)))\n",
    );
    round_trip_bytes(
        CoreTextKind::Literal,
        b"(core 0.11(float 0x7FF800000000ABCD))",
        b"(core 0.11 (float 0x7ff800000000abcd))\n",
    );
    round_trip_bytes(
        CoreTextKind::Expr,
        b"(core 0.11 (proj(var\"v\") -000))",
        b"(core 0.11 (proj (var \"v\") 0))\n",
    );
    round_trip_bytes(
        CoreTextKind::Value,
        b"(core 0.11(cap(files)(some\"notes\")(some -000)))",
        b"(core 0.11 (cap (files) (some \"notes\") (some 0)))\n",
    );
}

#[test]
fn test_malformed() {
    let cases = [
        (CoreTextKind::Literal, "", ErrorCode::CoreSyntax),
        (
            CoreTextKind::Literal,
            "(core 0.11 (unit)",
            ErrorCode::CoreSyntax,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (unit)))",
            ErrorCode::CoreSyntax,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (unit)) (unit)",
            ErrorCode::CoreSyntax,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"a",
            ErrorCode::CoreSyntax,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"\\q\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"\\u{}\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"\\u{0000000}\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"\\u{xyz}\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"\\u{D800}\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"\\u{110000}\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"raw\ncontrol\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text \"raw\x7fcontrol\"))",
            ErrorCode::CoreEscape,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.1 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.2 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.3 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.4 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.5 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.6 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.7 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.8 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.9 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.10 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 9.9 (unit))",
            ErrorCode::CoreVersion,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (Unit))",
            ErrorCode::CoreConstructor,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (unit extra))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (text name))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (bool True))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (int +1))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (int -))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (int 1_000))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (int １２))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (float 0X8000000000000000))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (float 0x800000000000000))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (float 0x800000000000000g))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (char \"\"))",
            ErrorCode::CoreCharacter,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (char \"ab\"))",
            ErrorCode::CoreCharacter,
        ),
        (
            CoreTextKind::Literal,
            "(core 0.11 (bytes 0))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Expr,
            "(core 0.11 (proj (var \"x\") -1))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (int add))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (big (checkedAdd)))",
            ErrorCode::CoreConstructor,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (io (consoleFake true)))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (io (consolefake)))",
            ErrorCode::CoreConstructor,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (unicode16 (normalizeNFC)))",
            ErrorCode::CoreConstructor,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (unicode16 (graphemes true)))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (io (entries)))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Primitive,
            "(core 0.11 (io (makeFolder true)))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (map (((int 1)))))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Outcome,
            "(core 0.11 (outOfFuel (World () () () () () () 0 0 ())))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Program,
            "(core 0.11 (Program () () \"main\"))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Program,
            "(core 0.11 (Program () () () \"main\"))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (cap (files) \"notes\" (none)))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (cap (files) (some \"notes\")))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (cap (files) (none) (some -1)))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (cap (files) (none) (some \"0\")))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (cap (files) (some 2) (none)))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Value,
            "(core 0.11 (cap (files) (none) (none 0)))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::World,
            "(core 0.11 (World () () () () () () 0 0))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::World,
            "(core 0.11 (World () () () () () () 0 0 () ()))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::CapState,
            "(core 0.11 (CapState () () () () () () 0))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::CapState,
            "(core 0.11 (CapState () () () () () () 0 0))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::CapState,
            "(core 0.11 (CapState () () () () () () 0 -1 ()))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::RandomState,
            "(core 0.11 (RandomState 0x0000000000000001))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::RandomState,
            "(core 0.11 (RandomState 0 1 2 3))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::RandomState,
            "(core 0.11 (RandomState 0x0000000000000001 0x0000000000000002 0x0000000000000003 0x10000000000000000))",
            ErrorCode::CoreNumber,
        ),
        (
            CoreTextKind::Bug,
            "(core 0.11 (fileFolderConflict))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Bug,
            "(core 0.11 (fileFolderConflict a))",
            ErrorCode::CoreField,
        ),
        (
            CoreTextKind::Effects,
            "(core 0.11 (effects () () () ()))",
            ErrorCode::CoreArity,
        ),
        (
            CoreTextKind::Effects,
            "(core 0.11 (effects () () () (3) ()))",
            ErrorCode::CoreField,
        ),
    ];
    for (kind, text, code) in cases {
        malformed(kind, text.as_bytes(), code);
    }
    let invalid_utf8: &[&[u8]] = &[
        b"\xc0\x80",
        b"\xe0\x80\x80",
        b"\xed\xa0\x80",
        b"\xf4\x90\x80\x80",
        b"\xf0\x9f",
        b";\xff",
    ];
    for bytes in invalid_utf8 {
        malformed(CoreTextKind::Literal, bytes, ErrorCode::CoreUtf8);
    }
    let oversize = vec![b' '; (MO_LIMIT_CORE_TEXT_SIZE as usize) + 1];
    malformed(CoreTextKind::Literal, &oversize, ErrorCode::CoreTextLimit);
}

#[test]
fn test_metadata_and_mutations() {
    let text = std::fs::read_to_string(repo_root().join("tests/core/Metadata.core")).unwrap();
    let mut diags = DiagBag::new();
    let mut parsed = parse_run(text.as_bytes(), &mut diags).unwrap();
    assert!(!diags.has_any());
    let m = match parsed.value.program.metadata {
        Metadata::Source(ref m) => Some(m),
        Metadata::NoSource => None,
    }
    .unwrap();
    assert_eq!(m.files.len(), 2);
    assert_eq!(m.modules.len(), 2);
    assert_eq!(m.files[1], "fixtures/main.mo");
    assert!(!m.modules[0].exposed_types[0].contents_exposed);
    assert!(m.modules[1].exposed_types[0].contents_exposed);
    assert_eq!(m.funs.len(), 7);
    assert_eq!(m.consts.len(), 1);
    let f = &m.funs[1];
    assert_eq!(f.name, "Main.main");
    assert_eq!(f.span, Some(Span::new(1, 0, 90)));
    assert_eq!(f.body.spans.len(), 17);
    assert_eq!(f.body.bindings.len(), 5);
    assert_eq!(f.body.bindings[4].node, 8);
    assert_eq!(f.body.bindings[4].slot, 0);
    assert_eq!(f.body.bindings[4].name, "q");
    assert_eq!(f.body.closures[0].node, 3);
    assert_eq!(f.body.closures[0].name, "Main.main.fn.1");
    assert_eq!(m.funs[0].body.params[0].as_deref(), Some("n"));
    assert_eq!(
        m.funs[2].text.as_deref(),
        Some("a sentence with β and \"quotes\"")
    );
    assert_eq!(m.funs[4].subject, Some(0));
    assert_eq!(m.funs[4].lines.len(), 2);
    assert_eq!(m.funs[4].lines[1].span.unwrap().start, 71);
    assert_eq!(m.funs[4].lines[1].text, "n >= 0");
    assert_eq!(m.funs[5].subject, Some(4));
    assert_eq!(m.funs[5].allows, Some(true));
    assert_eq!(m.funs[6].allows, Some(false));
    assert!(m.funs[6].body.spans[0].is_none());
    assert_eq!(m.consts[0].name, "Hidden.seed");

    let changes = [
        (
            "SourceSpan 0 0 40",
            "SourceSpan 2 0 40",
            ErrorCode::CoreField,
        ),
        (
            "SourceSpan 0 0 40",
            "SourceSpan 0 41 40",
            ErrorCode::CoreField,
        ),
        (
            "SourceSpan 0 0 40",
            "SourceSpan 0 0 4294967296",
            ErrorCode::CoreNumber,
        ),
        ("(some (SourceSpan 1 13 15))", "", ErrorCode::CoreField),
        ("((some \"n\"))", "()", ErrorCode::CoreField),
        (
            "BindingMeta 0 0 \"x\"",
            "BindingMeta 0 1 \"x\"",
            ErrorCode::CoreField,
        ),
        (
            "BindingMeta 2 0 \"f\"",
            "BindingMeta 0 0 \"f\"",
            ErrorCode::CoreField,
        ),
        (
            "BindingMeta 2 0 \"f\"",
            "BindingMeta 1 0 \"f\"",
            ErrorCode::CoreField,
        ),
        ("((3 \"Main.main.fn.1\"))", "()", ErrorCode::CoreField),
        (
            "((3 \"Main.main.fn.1\"))",
            "((4 \"Main.main.fn.1\"))",
            ErrorCode::CoreField,
        ),
        ("(0 4 5 6)", "(0 4 5)", ErrorCode::CoreField),
        ("((0 false))", "((1 false))", ErrorCode::CoreField),
        ("((0 false))", "((0 false) (0 true))", ErrorCode::CoreField),
        ("((1 true)) (1)", "((1 true)) (2)", ErrorCode::CoreField),
        ("(some 0) (none)", "(some 4) (none)", ErrorCode::CoreField),
        (
            "(some 4) (some true)",
            "(some 0) (some true)",
            ErrorCode::CoreField,
        ),
        (
            "(some 4) (some false)",
            "(some 4) (none)",
            ErrorCode::CoreField,
        ),
        ("(some \"main() == 12\")", "(none)", ErrorCode::CoreField),
        (
            "\"fixtures/main.mo\"",
            "\"fixtures/main\\u{0}.mo\"",
            ErrorCode::CoreField,
        ),
    ];
    for (old, replacement, code) in changes {
        let pos = text.find(old).unwrap();
        let mut mutated = String::with_capacity(text.len() + replacement.len());
        mutated.push_str(&text[..pos]);
        mutated.push_str(replacement);
        mutated.push_str(&text[pos + old.len()..]);
        malformed(CoreTextKind::Run, mutated.as_bytes(), code);
    }

    let printed = print_run(&parsed.arena, &parsed.value).unwrap();
    assert_eq!(printed, text);

    if let Metadata::Source(ref mut sm) = parsed.value.program.metadata {
        sm.funs[1].body.spans.pop();
    }
    assert_eq!(
        print_run(&parsed.arena, &parsed.value),
        Err(CorePrintError::InvalidMetadata)
    );
}

#[test]
fn test_typed_fields() {
    let text = "(core 0.11 (Run (Program ((DataDecl \"D\" ((Ctor \"C\" ((\"field\" (byte))))))) \
        ((FunDecl \"f\" ((\"x\" (int))) (text) (join \"j\" ((\"y\" (byte))) \
        (call \"g\" ((var \"y\"))) (jump \"j\" ((lit (byte 7))))) \"Module\" (promise) 123)) \
        ((ConstDecl \"K\" (bool) (lit (bool true)))) \"f\" (noSource)) \
        (World (\"arg\") ((\"key\" \"env\")) (\"in\") (\"out\") (\"err\") ((\"path\" \"file\")) \
        -19 (RandomState 0x0000000000000017 0x000000000000001d 0x8000000000000000 \
        0xffffffffffffffff) (\"real folder\") \
        ((CapState (\"fake arg\") ((\"fake key\" \"fake env\")) (\"fake in\") \
        (\"fake out\") (\"fake err\") ((\"fake path\" \"fake file\")) -37 \
        (RandomState 0x0000000000000029 0x000000000000002b 0x000000000000002f \
        0x0000000000000035) (\"fake folder\")) \
        (CapState () () () () () () 43 (RandomState 0x000000000000003b 0x000000000000003d \
        0x0000000000000043 0x0000000000000047) ()))) \
        (entry ((float 0x8000000000000000) (cap (readFiles) (some \"notes\") (some 1)) \
        (cap (console) (none) (none)))) 31))";
    let mut diags = DiagBag::new();
    let parsed = parse_run(text.as_bytes(), &mut diags).unwrap();
    assert!(!diags.has_any());
    let arena = &parsed.arena;
    let run = &parsed.value;
    let p = &run.program;
    assert_eq!(p.data.len(), 1);
    assert_eq!(p.funs.len(), 1);
    assert_eq!(p.consts.len(), 1);
    assert_eq!(p.entry, "f");
    let ctor = &p.data[0].ctors[0];
    assert_eq!(ctor.name, "C");
    assert_eq!(ctor.fields.len(), 1);
    assert_eq!(ctor.fields[0].name, "field");
    assert_eq!(*arena.ty(ctor.fields[0].ty), Ty::Byte);
    let f = &p.funs[0];
    assert_eq!(f.name, "f");
    assert_eq!(f.module, "Module");
    assert_eq!(*arena.ty(f.params[0].ty), Ty::Int);
    assert_eq!(*arena.ty(f.ret), Ty::Text);
    assert_eq!(f.kind, FnKind::Promise);
    assert_eq!(f.const_count.digits, "123");
    let (name, body, rest) = match arena.expr(f.body) {
        Expr::Join {
            name, body, rest, ..
        } => Some((name, body, rest)),
        _ => None,
    }
    .unwrap();
    assert_eq!(name, "j");
    assert!(matches!(arena.expr(*body), Expr::Call { name, .. } if name == "g"));
    assert!(
        matches!(arena.expr(*rest), Expr::Jump { args, .. } if matches!(arena.expr(args[0]), Expr::Lit(Lit::Byte(b)) if b.digits == "7"))
    );
    assert!(run.world.now.negative);
    assert_eq!(run.world.now.digits, "19");
    assert_eq!(run.world.random.s0, 23);
    assert_eq!(run.world.random.s1, 29);
    assert_eq!(run.world.random.s2, 0x8000_0000_0000_0000);
    assert_eq!(run.world.random.s3, u64::MAX);
    assert_eq!(run.world.fakes.len(), 2);
    let args = match run.selection {
        Selection::Entry(ref args) => Some(args),
        Selection::Test(_) => None,
    }
    .unwrap();
    assert_eq!(args.len(), 3);
    assert_eq!(*arena.value(args[0]), Value::Float(0x8000_0000_0000_0000));
    assert!(matches!(
        arena.value(args[1]),
        Value::Cap {
            cap: Cap::ReadFiles,
            root: Some(r),
            fake: Some(n),
        } if r == "notes" && n.digits == "1"
    ));
    assert_eq!(run.fuel.digits, "31");

    let prim_text = "(core 0.11 (map (text) (int) (lit 27)))";
    let prim_doc =
        parse_document(CoreTextKind::Primitive, prim_text.as_bytes(), &mut diags).unwrap();
    let (key, value, op) = match prim_doc.value {
        CoreDocument::Primitive(Prim::Map { key, value, op }) => Some((key, value, op)),
        _ => None,
    }
    .unwrap();
    assert_eq!(*prim_doc.arena.ty(key), Ty::Text);
    assert_eq!(*prim_doc.arena.ty(value), Ty::Int);
    assert_eq!(op, MapOp::Lit(CoreNatural::from_u64(27)));
    let _ = CoreMapNullaryOp::Empty;
}

#[test]
fn test_limits_and_cyclic_print() {
    let depth = MO_LIMIT_CORE_DEPTH as usize;
    // Deep let chain at exact permitted depth (MO_LIMIT_CORE_DEPTH - 3 nested lets + 2 wrapper lists + leaf).
    let mut text = format!("(core {MO_CORE_TEXT_VERSION} ");
    for _ in 0..(depth - 3) {
        text.push_str("(let_ \"x\" (lit (int 0)) ");
    }
    text.push_str("(lit (int 7))");
    for _ in 0..(depth - 2) {
        text.push(')');
    }
    text.push('\n');
    round_trip_bytes(CoreTextKind::Expr, text.as_bytes(), text.as_bytes());

    // Deep prim chain.
    let mut text = format!("(core {MO_CORE_TEXT_VERSION} (let_ \"x\" (lit (int 0)) ");
    for _ in 0..((depth - 4) / 2) {
        text.push_str("(prim (int (neg)) (");
    }
    text.push_str("(lit (int 7))");
    for _ in 0..((depth - 4) / 2) {
        text.push_str("))");
    }
    text.push_str("))\n");
    round_trip_bytes(CoreTextKind::Expr, text.as_bytes(), text.as_bytes());

    // Deep type chain.
    let mut text = format!("(core {MO_CORE_TEXT_VERSION} ");
    for _ in 0..(depth - 2) {
        text.push_str("(list ");
    }
    text.push_str("(int)");
    for _ in 0..(depth - 1) {
        text.push(')');
    }
    text.push('\n');
    round_trip_bytes(CoreTextKind::Type, text.as_bytes(), text.as_bytes());

    // Depth overflow.
    let too_deep = vec![b'('; depth + 1];
    malformed(CoreTextKind::Type, &too_deep, ErrorCode::NestedTooDeeply);

    // Node limit overflow.
    let mut too_many = String::with_capacity((MO_LIMIT_CORE_NODES as usize) * 2 + 1);
    too_many.push('(');
    for _ in 0..(MO_LIMIT_CORE_NODES as usize) {
        too_many.push_str("x ");
    }
    malformed(
        CoreTextKind::Type,
        too_many.as_bytes(),
        ErrorCode::CoreTextLimit,
    );

    // Embedded NUL in string round-trips.
    let nul_prog = b"(core 0.11 (Program () () () \"a\\u{0}b\" (noSource)))";
    let mut diags = DiagBag::new();
    let parsed = parse_program(nul_prog, &mut diags).unwrap();
    assert_eq!(parsed.value.entry.as_bytes(), b"a\0b");
    let printed = print_program(&parsed.arena, &parsed.value).unwrap();
    assert_eq!(
        printed,
        "(core 0.11 (Program () () () \"a\\u{0}b\" (noSource)))\n"
    );

    // Cyclic type in arena is refused by printer.
    let mut arena = CoreArena::new();
    let id = arena.alloc_ty(Ty::Int);
    *arena.ty_mut(id) = Ty::List(id);
    assert_eq!(
        print_document(&arena, &CoreDocument::Type(id)),
        Err(CorePrintError::LimitExceeded)
    );
}

#[test]
fn test_print_byte_limit() {
    let limit = MO_LIMIT_CORE_TEXT_SIZE as usize;
    let overhead = format!("(core {MO_CORE_TEXT_VERSION} (text \"\"))\n").len();
    let arena = CoreArena::new();

    let too_big = Lit::Text("a".repeat(limit));
    assert_eq!(
        print_document(&arena, &CoreDocument::Literal(too_big)),
        Err(CorePrintError::LimitExceeded)
    );

    let exact_len = limit - overhead;
    let exact = Lit::Text("a".repeat(exact_len));
    let printed = print_document(&arena, &CoreDocument::Literal(exact)).unwrap();
    assert_eq!(printed.len(), limit);
    let mut diags = DiagBag::new();
    assert!(parse_document(CoreTextKind::Literal, printed.as_bytes(), &mut diags).is_some());
    assert!(!diags.has_any());

    let plus_one = Lit::Text("a".repeat(exact_len + 1));
    assert_eq!(
        print_document(&arena, &CoreDocument::Literal(plus_one)),
        Err(CorePrintError::LimitExceeded)
    );

    let mut escaped = "a".repeat(exact_len);
    escaped.replace_range(0..1, "\n");
    assert_eq!(
        print_document(&arena, &CoreDocument::Literal(Lit::Text(escaped))),
        Err(CorePrintError::LimitExceeded)
    );
}

#[test]
fn test_simulation_descriptors() {
    expression(
        "(fn ((simulation)) (unit))",
        "(lam ((\"s\" (simulation))) (let_ \"a\" (prim (sim (advance)) \
         ((var \"s\") (lit (duration 0)))) (let_ \"b\" (prim (sim (crash)) \
         ((var \"s\"))) (prim (sim (restart)) ((var \"s\"))))))",
        None,
    );
    expression(
        "(fn ((simulation)) (result (unit) (simulationError)))",
        "(lam ((\"s\" (simulation))) (prim (sim (send 0 0 () (unit))) ((var \"s\"))))",
        Some(ErrorCode::CoreCheckArguments),
    );
    expression(
        "(unit)",
        "(let_ \"s\" (prim (sim (start 0)) ((lit (time 0)))) (lit (unit)))",
        Some(ErrorCode::CoreCheckArguments),
    );
    let services = Services {
        items: vec![Service {
            start: 0,
            token_env: String::new(),
            tools: vec![ServiceTool {
                name: "tool".to_string(),
                function: 1,
                read_only: false,
                description: String::new(),
                argument_names: Vec::new(),
            }],
            timers: vec![ServiceTimer {
                function: 2,
                interval: 0,
            }],
        }],
        data_shapes: Vec::new(),
    };
    let ops = [
        "send 0 0 ((int)) (tuple ((int) (int)))",
        "send 1 0 ((int)) (tuple ((int) (int)))",
        "send 0 1 ((int)) (tuple ((int) (int)))",
        "send 0 2 ((int)) (tuple ((int) (int)))",
        "send 0 0 ((text)) (tuple ((int) (int)))",
        "send 0 0 ((int)) (int)",
    ];
    for (i, op) in ops.iter().enumerate() {
        let payload = format!(
            "(Program () (\
            (FunDecl \"start\" () (int) (lit (int 7)) \"Main\" (function) 0) \
            (FunDecl \"tool\" ((\"state\" (int)) (\"now\" (time)) (\"n\" (int))) \
            (tuple ((int) (int))) (tuple ((var \"state\") (var \"n\"))) \"Main\" (function) 0) \
            (FunDecl \"timer\" ((\"state\" (int)) (\"now\" (time))) (int) \
            (var \"state\") \"Main\" (function) 0) \
            (FunDecl \"helper\" ((\"s\" (simulation))) \
            (result (tuple ((int) (int))) (simulationError)) \
            (prim (sim ({op})) ((var \"s\") (lit (int 9)))) \"Main\" (function) 0) \
            (FunDecl \"main\" () (int) (lit (int 0)) \"Main\" (function) 0)) \
            () \"main\" (noSource))"
        );
        check_program_with_services(
            &payload,
            if i == 0 {
                None
            } else {
                Some(ErrorCode::CoreCheckArguments)
            },
            Some(&services),
        );
        if i == 0 {
            check_prog(&payload, Some(ErrorCode::CoreCheckArguments));
        }
    }
}

#[test]
fn test_tail_and_inference() {
    expression(
        "(int)",
        "(join \"j\" ((\"n\" (int))) (var \"n\") (jump \"j\" ((lit (int 7)))))",
        None,
    );
    expression(
        "(int)",
        "(join \"j\" () (lit (int 7)) (let_ \"x\" (jump \"j\" ()) (var \"x\")))",
        Some(ErrorCode::CoreCheckJump),
    );
    expression(
        "(fn () (int))",
        "(join \"j\" () (stop \"x\") (lam () (jump \"j\" ())))",
        Some(ErrorCode::CoreCheckJump),
    );
    expression(
        "(tuple ((int)))",
        "(tuple ((stop \"x\")))",
        Some(ErrorCode::CoreCheckInference),
    );
    expression(
        "(int)",
        "(let_ \"x\" (stop \"x\") (lit (int 1)))",
        Some(ErrorCode::CoreCheckInference),
    );
    expression(
        "(int)",
        "(let_ \"x\" (if_ (lit (bool true)) (stop \"x\") (lit (int 7))) (var \"x\"))",
        None,
    );
    expression(
        "(int)",
        "(let_ \"x\" (if_ (lit (bool true)) (lit (int 7)) (stop \"x\")) (var \"x\"))",
        None,
    );
    expression(
        "(int)",
        "(let_ \"x\" (join \"j\" () (lit (int 7)) (jump \"j\" ())) (var \"x\"))",
        None,
    );
    expression(
        "(int)",
        "(let_ \"x\" (join \"j\" () (jump \"j\" ()) (lit (int 7))) (var \"x\"))",
        None,
    );
    expression(
        "(fn ((text)) (int))",
        "(lam ((\"x\" (text))) (stop \"x\"))",
        None,
    );
    expression(
        "(int)",
        "(let_ \"x\" (case_ (con (option (int)) \"None\" ()) ((\"None\" () (stop \"\")) \
         (\"Some\" (\"n\") (var \"n\")))) (var \"x\"))",
        None,
    );
    expression(
        "(int)",
        "(let_ \"f\" (if_ (lit (bool true)) (lam () (stop \"\")) (lam () (lit (int 3)))) \
         (app (var \"f\") ()))",
        None,
    );
    expression(
        "(int)",
        "(let_ \"f\" (if_ (lit (bool true)) (lam () (if_ (lit (bool true)) (stop \"\") \
         (stop \"\"))) (lam () (lit (text \"right\")))) (app (var \"f\") ()))",
        Some(ErrorCode::CoreCheckType),
    );
    expression(
        "(text)",
        "(let_ \"x\" (if_ (lit (bool true)) (if_ (lit (bool true)) (if_ (lit (bool true)) (stop \
         \"\") (lit (int 3))) (lit (text \"middle\"))) (lit (text \"outer\"))) (var \"x\"))",
        Some(ErrorCode::CoreCheckType),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" ((\"x\" (int)) (\"x\" (text))) (int) (var \"x\") \
         \"Main\" (function) 0) (FunDecl \"main\" () (text) (lit (text \"later\")) \
         \"Main\" (function) 0)) () \"main\" (noSource))",
        None,
    );
    check_prog(
        "(Program ((DataDecl \"D\" ((Ctor \"A\" ((\"x\" (int)))) (Ctor \"A\" ((\"x\" (text)))))) \
         (DataDecl \"D\" ((Ctor \"B\" ())))) ((FunDecl \"main\" () (data \"D\") (con (data \"D\") \
         \"A\" ((lit (int 1)))) \"Main\" (function) 0)) () \"main\" (noSource))",
        None,
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" (function) 0) \
         (FunDecl \"spec\" () (unit) (let_ \"f\" (lam () (prim (io (consoleFake)) ())) \
         (lit (unit))) \"Main\" (test) 0)) () \"main\" (noSource))",
        None,
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" (function) 1)) \
         ((ConstDecl \"fake\" (fn () (cap (console))) (lam () (prim (io (consoleFake)) \
         ())))) \"main\" (noSource))",
        Some(ErrorCode::CoreCheckSpecPrimitive),
    );
}

#[test]
fn test_refusals() {
    let cases = [
        ("(int)", "(var \"absent\")", ErrorCode::CoreCheckVariable),
        ("(int)", "(global \"absent\")", ErrorCode::CoreCheckGlobal),
        ("(int)", "(lit (bool true))", ErrorCode::CoreCheckType),
        (
            "(int)",
            "(prim (int (add)) ((lit (int 1))))",
            ErrorCode::CoreCheckArguments,
        ),
        (
            "(fn ((int)) (int))",
            "(lam ((\"x\" (text))) (lit (int 1)))",
            ErrorCode::CoreCheckLambda,
        ),
        (
            "(int)",
            "(app (lit (int 1)) ())",
            ErrorCode::CoreCheckApplication,
        ),
        (
            "(int)",
            "(call \"absent\" ())",
            ErrorCode::CoreCheckFunction,
        ),
        (
            "(int)",
            "(proj (tuple ((lit (text \"a\")) (lit (int 7)))) 2)",
            ErrorCode::CoreCheckProjection,
        ),
        (
            "(int)",
            "(con (int) \"X\" ())",
            ErrorCode::CoreCheckConstructors,
        ),
        (
            "(option (int))",
            "(con (option (int)) \"Error\" ())",
            ErrorCode::CoreCheckConstructor,
        ),
        (
            "(int)",
            "(case_ (con (option (int)) \"None\" ()) ())",
            ErrorCode::CoreCheckArms,
        ),
        (
            "(int)",
            "(case_ (con (option (int)) \"None\" ()) ((\"Some\" (\"x\") (var \"x\")) (\"None\" () \
             (lit (int 7)))))",
            ErrorCode::CoreCheckArm,
        ),
        (
            "(cap (console))",
            "(prim (io (consoleFake)) ())",
            ErrorCode::CoreCheckSpecPrimitive,
        ),
        (
            "(int)",
            "(fail (lit (text \"detail\")))",
            ErrorCode::CoreCheckFail,
        ),
        ("(int)", "(jump \"absent\" ())", ErrorCode::CoreCheckJump),
    ];
    for (ret, body, code) in cases {
        expression(ret, body, Some(code));
    }
    check_prog(
        "(Program () () () \"absent\" (noSource))",
        Some(ErrorCode::CoreCheckEntry),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (unit) (lit (unit)) \"Main\" (test) 0)) () \"main\" (noSource))",
        Some(ErrorCode::CoreCheckEntryKind),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (int) (lit (int 1)) \"Main\" (function) 0) \
         (FunDecl \"spec\" () (int) (lit (int 1)) \"Main\" (test) 0)) () \"main\" (noSource))",
        Some(ErrorCode::CoreCheckKind),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (int) (lit (int 1)) \"Main\" (function) 1)) () \"main\" (noSource))",
        Some(ErrorCode::CoreCheckPrefix),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (int) (lit (int 1)) \"Main\" (function) 0)) \
         ((ConstDecl \"c\" (int) (lit (int 1))) (ConstDecl \"c\" (int) (lit (int 2)))) \
         \"main\" (noSource))",
        Some(ErrorCode::CoreCheckConstantName),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (int) (call \"f\" ()) \"Main\" (function) 0) \
         (FunDecl \"f\" () (int) (lit (int 1)) \"Main\" (function) 1)) \
         ((ConstDecl \"c\" (int) (lit (int 2)))) \"main\" (noSource))",
        Some(ErrorCode::CoreCheckCallPrefix),
    );
    check_prog(
        "(Program () ((FunDecl \"main\" () (int) (call \"spec\" ()) \"Main\" (function) 0) \
         (FunDecl \"spec\" () (int) (lit (int 1)) \"Main\" (anchor) 0)) () \"main\" (noSource))",
        Some(ErrorCode::CoreCheckSpecCall),
    );
}

#[test]
fn test_literal_boundaries() {
    let cases = [
        ("int", "-9223372036854775808", "-9223372036854775809"),
        ("int", "9223372036854775807", "9223372036854775808"),
        ("byte", "255", "256"),
        ("byte", "0", "-1"),
        (
            "decimal",
            "170141183460469231731687303715884105727",
            "170141183460469231731687303715884105728",
        ),
        (
            "decimal",
            "-170141183460469231731687303715884105727",
            "-170141183460469231731687303715884105728",
        ),
        ("time", "-62135596800000000", "-62135596800000001"),
        ("time", "253402300799999999", "253402300800000000"),
        ("duration", "-9223372036854775808", "9223372036854775808"),
    ];
    for (ty, valid, invalid) in cases {
        let ret = format!("({ty})");
        expression(&ret, &format!("(lit ({ty} {valid}))"), None);
        expression(
            &ret,
            &format!("(lit ({ty} {invalid}))"),
            Some(ErrorCode::CoreCheckLiteral),
        );
    }
    expression("(bytes)", "(lit (bytes (0 255)))", None);
    expression(
        "(bytes)",
        "(lit (bytes (0 256)))",
        Some(ErrorCode::CoreCheckLiteral),
    );
    expression(
        "(bytes)",
        "(lit (bytes (-1 255)))",
        Some(ErrorCode::CoreCheckLiteral),
    );
    expression("(float)", "(lit (float 0x7ff8000000000001))", None);
    expression(
        "(list (int))",
        "(prim (list (int) (lit 999999999999999999999999999)) ())",
        Some(ErrorCode::CoreCheckArguments),
    );
    expression(
        "(map (text) (int))",
        "(prim (map (text) (int) (lit 999999999999999999999999999)) ())",
        Some(ErrorCode::CoreCheckArguments),
    );
}

#[test]
fn test_limits_and_metadata_in_checker() {
    let depth = MO_LIMIT_CORE_DEPTH as usize;
    let mut arena = CoreArena::new();
    let ty_int = arena.alloc_ty(Ty::Int);
    let leaf = arena.alloc_expr(Expr::Stop(String::new()));
    let mut nodes: Vec<ExprId> = Vec::with_capacity(depth);
    let mut body = leaf;
    for _ in 0..depth {
        body = arena.alloc_expr(Expr::Join {
            name: String::new(),
            params: Vec::new(),
            body: leaf,
            rest: body,
        });
        nodes.push(body);
    }
    let mut p = Program {
        data: Vec::new(),
        funs: vec![FnDecl {
            name: "main".to_string(),
            params: Vec::new(),
            ret: ty_int,
            body,
            module: String::new(),
            kind: FnKind::Function,
            const_count: CoreNatural::zero(),
        }],
        consts: Vec::new(),
        entry: "main".to_string(),
        metadata: Metadata::NoSource,
    };
    let mut d = DiagBag::new();
    assert!(!check_program(&arena, &p, &mut d));
    assert_eq!(d.len(), 1);
    assert_eq!(d.items()[0].code, ErrorCode::CoreCheckLimit);
    assert!(
        d.items()[0]
            .message
            .as_str_lossy()
            .contains("resource limit")
    );
    assert!(!d.items()[0].message.as_str_lossy().contains("bug in Mo"));

    // At the permitted boundary (`nodes[depth - 2]`).
    p.funs[0].body = nodes[depth - 2];
    assert!(check_program(&arena, &p, &mut d));
    assert_eq!(d.len(), 1);

    // Invalid empty source metadata is rejected with E1051.
    p.metadata = Metadata::Source(Box::default());
    assert!(!check_program(&arena, &p, &mut d));
    assert_eq!(d.len(), 2);
    assert_eq!(d.items()[1].code, ErrorCode::CoreCheckMetadata);

    // A name's bytes are not nodes.
    let big_ty = arena.alloc_ty(Ty::Data("x".repeat((MO_LIMIT_CORE_NODES as usize) + 1)));
    p.funs[0].ret = big_ty;
    p.metadata = Metadata::NoSource;
    assert!(check_program(&arena, &p, &mut d));
    assert_eq!(d.len(), 2);

    // Metadata depth & duplicate module checks.
    p.funs[0].ret = ty_int;
    p.funs[0].module = "Main".to_string();
    p.metadata = Metadata::Source(Box::new(SourceMetadata {
        files: vec!["main.mo".to_string()],
        file_bytes: Vec::new(),
        modules: vec![ModuleMeta {
            name: "Main".to_string(),
            data: Vec::new(),
            funs: vec![0],
            consts: Vec::new(),
            exposed_types: Vec::new(),
            exposed_funs: Vec::new(),
        }],
        funs: vec![FnMeta {
            name: "Main.main".to_string(),
            span: None,
            text: None,
            body: BodyMeta {
                spans: vec![None; 2 * depth - 1],
                params: Vec::new(),
                bindings: Vec::new(),
                closures: Vec::new(),
            },
            subject: None,
            allows: None,
            lines: Vec::new(),
        }],
        consts: Vec::new(),
    }));
    assert!(metadata_valid(&arena, &p));

    p.funs[0].body = nodes[depth - 1];
    if let Metadata::Source(ref mut sm) = p.metadata {
        sm.funs[0].body.spans.push(None);
        sm.funs[0].body.spans.push(None);
    }
    assert!(!metadata_valid(&arena, &p));

    p.funs[0].body = leaf;
    if let Metadata::Source(ref mut sm) = p.metadata {
        sm.funs[0].body.spans = vec![None];
        sm.modules.push(ModuleMeta {
            name: "Other".to_string(),
            data: Vec::new(),
            funs: Vec::new(),
            consts: Vec::new(),
            exposed_types: Vec::new(),
            exposed_funs: Vec::new(),
        });
        sm.modules.push(ModuleMeta {
            name: "Main".to_string(),
            data: Vec::new(),
            funs: Vec::new(),
            consts: Vec::new(),
            exposed_types: Vec::new(),
            exposed_funs: Vec::new(),
        });
    }
    assert!(!metadata_valid(&arena, &p));
    if let Metadata::Source(ref mut sm) = p.metadata {
        sm.modules[2].name = "Third".to_string();
    }
    assert!(metadata_valid(&arena, &p));
}

#[test]
fn test_source_span_and_nested_spans() {
    let text = format!(
        "(core {MO_CORE_TEXT_VERSION} \
         (Program () ((FunDecl \"main\" () (tuple ((int) (int))) (tuple ((lit (int \
         7)) (var \"missing\"))) \"Main\" (function) 0)) () \"main\" (Metadata (\"main.mo\") \
         ((ModuleMeta \"Main\" () (0) () () ())) ((FunMeta \"Main.main\" (none) (none) \
         (BodyMeta ((none) (none) (some (SourceSpan 0 29 40))) () () ()) (none) (none) ())) \
         ())))"
    );
    let mut diags = DiagBag::new();
    let mut parsed = parse_program(text.as_bytes(), &mut diags).unwrap();
    assert!(!diags.has_any());
    assert!(!check_program(&parsed.arena, &parsed.value, &mut diags));
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::CoreCheckVariable);
    assert_eq!(diags.items()[0].primary.span, Span::new(0, 29, 40));
    assert!(
        diags.items()[0]
            .primary
            .text
            .as_ref()
            .unwrap()
            .as_str_lossy()
            .contains("main")
    );
    assert!(
        diags.items()[0]
            .primary
            .text
            .as_ref()
            .unwrap()
            .as_str_lossy()
            .contains("missing")
    );

    if let Metadata::Source(ref mut sm) = parsed.value.metadata {
        sm.funs[0].span = Some(Span::new(0, 10, 20));
        sm.funs[0].body.spans[2] = None;
    }
    assert!(!check_program(&parsed.arena, &parsed.value, &mut diags));
    assert_eq!(diags.len(), 2);
    assert_eq!(diags.items()[1].primary.span, Span::new(0, 10, 20));

    parsed.value.funs[0].kind = FnKind::Test;
    if let Metadata::Source(ref mut sm) = parsed.value.metadata {
        sm.funs[0].text = Some("spec".to_string());
    }
    assert!(!check_program(&parsed.arena, &parsed.value, &mut diags));
    assert_eq!(diags.len(), 3);
    assert_eq!(diags.items()[2].code, ErrorCode::CoreCheckEntryKind);
    assert_eq!(diags.items()[2].primary.span.start, 10);

    parsed.value.funs[0].kind = FnKind::Function;
    if let Metadata::Source(ref mut sm) = parsed.value.metadata {
        sm.funs[0].text = None;
    }
    parsed.value.funs[0].const_count = CoreNatural::from_u64(1);
    assert!(!check_program(&parsed.arena, &parsed.value, &mut diags));
    assert_eq!(diags.len(), 4);
    assert_eq!(diags.items()[3].code, ErrorCode::CoreCheckPrefix);
    assert_eq!(diags.items()[3].primary.span.start, 10);

    parsed.value.funs[0].const_count = CoreNatural::zero();
    parsed.value.entry = "absent".to_string();
    assert!(!check_program(&parsed.arena, &parsed.value, &mut diags));
    assert_eq!(diags.len(), 5);
    assert_eq!(diags.items()[4].code, ErrorCode::CoreCheckEntry);
    assert!(
        diags.items()[4]
            .primary
            .text
            .as_ref()
            .unwrap()
            .as_str_lossy()
            .contains("absent")
    );

    // Nested spans for lam, join, case, and const.
    let mut arena = CoreArena::new();
    let integer = arena.alloc_ty(Ty::Int);
    let callable = arena.alloc_ty(Ty::Fn {
        params: Vec::new(),
        ret: integer,
    });
    let option = arena.alloc_ty(Ty::Option(integer));
    let missing = arena.alloc_expr(Expr::Var("missing".to_string()));
    let stop = arena.alloc_expr(Expr::Stop(String::new()));
    let con = arena.alloc_expr(Expr::Con {
        ty: option,
        ctor: "None".to_string(),
        args: Vec::new(),
    });
    let roots = [
        arena.alloc_expr(Expr::Lam {
            params: Vec::new(),
            body: missing,
        }),
        arena.alloc_expr(Expr::Join {
            name: "j".to_string(),
            params: Vec::new(),
            body: missing,
            rest: stop,
        }),
        arena.alloc_expr(Expr::Case {
            expr: con,
            arms: vec![
                Arm {
                    ctor: "None".to_string(),
                    names: Vec::new(),
                    body: missing,
                },
                Arm {
                    ctor: "Some".to_string(),
                    names: vec!["n".to_string()],
                    body: stop,
                },
            ],
        }),
    ];
    for i in 0..4usize {
        let is_const = i == 3;
        let span_count = if i == 0 {
            2
        } else if i == 2 {
            4
        } else {
            3
        };
        let mut spans = vec![None; span_count];
        spans[if i == 2 { 2 } else { 1 }] = Some(Span::new(0, 29, 40));
        let body_meta = BodyMeta {
            spans,
            params: Vec::new(),
            bindings: Vec::new(),
            closures: if i == 0 {
                vec![ClosureMeta {
                    node: 0,
                    name: "Main.lambda".to_string(),
                }]
            } else {
                Vec::new()
            },
        };
        let p = Program {
            data: Vec::new(),
            funs: vec![FnDecl {
                name: "main".to_string(),
                params: Vec::new(),
                ret: if i == 0 { callable } else { integer },
                body: if is_const { stop } else { roots[i] },
                module: "Main".to_string(),
                kind: FnKind::Function,
                const_count: CoreNatural::zero(),
            }],
            consts: if is_const {
                vec![ConstDecl {
                    name: "c".to_string(),
                    ty: integer,
                    body: roots[1],
                }]
            } else {
                Vec::new()
            },
            entry: "main".to_string(),
            metadata: Metadata::Source(Box::new(SourceMetadata {
                files: vec!["main.mo".to_string()],
                file_bytes: Vec::new(),
                modules: vec![ModuleMeta {
                    name: "Main".to_string(),
                    data: Vec::new(),
                    funs: vec![0],
                    consts: if is_const { vec![0] } else { Vec::new() },
                    exposed_types: Vec::new(),
                    exposed_funs: Vec::new(),
                }],
                funs: vec![FnMeta {
                    name: "Main.main".to_string(),
                    span: None,
                    text: None,
                    body: if is_const {
                        BodyMeta {
                            spans: vec![None],
                            ..BodyMeta::default()
                        }
                    } else {
                        body_meta.clone()
                    },
                    subject: None,
                    allows: None,
                    lines: Vec::new(),
                }],
                consts: if is_const {
                    vec![ConstMeta {
                        name: "Main.c".to_string(),
                        span: None,
                        body: body_meta,
                    }]
                } else {
                    Vec::new()
                },
            })),
        };
        let mut d = DiagBag::new();
        assert!(metadata_valid(&arena, &p));
        assert!(!check_program(&arena, &p, &mut d));
        assert_eq!(d.len(), 1);
        assert_eq!(d.items()[0].code, ErrorCode::CoreCheckVariable);
        assert_eq!(d.items()[0].primary.span, Span::new(0, 29, 40));
        assert!(
            d.items()[0]
                .primary
                .text
                .as_ref()
                .unwrap()
                .as_str_lossy()
                .contains(if is_const { "\"c\"" } else { "\"main\"" })
        );
        assert!(
            d.items()[0]
                .primary
                .text
                .as_ref()
                .unwrap()
                .as_str_lossy()
                .contains("missing")
        );
    }
}

#[test]
fn test_large_programs_and_parser_node_limit() {
    // 1. Parser node limit: exact boundary (MO_LIMIT_CORE_NODES - 26) vs +1.
    for extra in 0..2usize {
        let mut s = format!(
            "(core {MO_CORE_TEXT_VERSION} (Program () ((FunDecl \"main\" () (bytes) (lit (bytes ("
        );
        let count = (MO_LIMIT_CORE_NODES as usize) - 26 + extra;
        for _ in 0..count {
            s.push_str("0 ");
        }
        s.push_str("))) \"Main\" (function) 0)) () \"main\" (noSource)))");
        let mut diags = DiagBag::new();
        let parsed = parse_program(s.as_bytes(), &mut diags);
        if extra == 0 {
            let parsed = parsed.unwrap();
            assert!(!diags.has_any());
            assert!(check_program(&parsed.arena, &parsed.value, &mut diags));
            assert!(!diags.has_any());
        } else {
            assert!(parsed.is_none());
            assert_eq!(diags.len(), 1);
            assert_eq!(diags.items()[0].code, ErrorCode::CoreTextLimit);
        }
    }

    // 2. Large nominal name (2MB) + 160k `case_` nodes must typecheck quickly (O(n log n)).
    let name = "d".repeat(2_000_000);
    let count = 160_000usize;
    let mut s = String::with_capacity(MO_LIMIT_CORE_TEXT_SIZE as usize);
    s.push_str(&format!(
        "(core {MO_CORE_TEXT_VERSION} (Program ((DataDecl \""
    ));
    s.push_str(&name);
    s.push_str("\" ())) ((FunDecl \"main\" ((\"d\" (data \"");
    s.push_str(&name);
    s.push_str("\"))) (list (unit)) (prim (list (unit) (lit ");
    s.push_str(&count.to_string());
    s.push_str(")) (");
    for _ in 0..count {
        s.push_str("(case_ (var \"d\") ()) ");
    }
    s.push_str(")) \"Main\" (function) 0)) () \"main\" (noSource)))");
    let mut diags = DiagBag::new();
    let parsed = parse_program(s.as_bytes(), &mut diags).unwrap();
    assert!(!diags.has_any());
    let start = Instant::now();
    assert!(check_program(&parsed.arena, &parsed.value, &mut diags));
    let elapsed = start.elapsed();
    assert!(!diags.has_any());
    assert!(
        elapsed.as_secs_f64() < 4.0,
        "2MB nominal + 160k case_ took too long: {elapsed:?}"
    );
}

#[test]
fn test_core_depth_boundary() {
    let build_depth = |depth: usize, lets: bool| -> (CoreArena, Program) {
        let mut arena = CoreArena::new();
        let ty_int = arena.alloc_ty(Ty::Int);
        let lit7 = arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::from_i64(7))));
        let lit0 = arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::zero())));
        let mut cur = lit7;
        for i in 1..depth {
            if lets {
                cur = arena.alloc_expr(Expr::Let {
                    name: format!("x{i}"),
                    bound: lit0,
                    body: cur,
                });
            } else {
                cur = arena.alloc_expr(Expr::Prim {
                    prim: Prim::Int(CoreIntOp::Neg),
                    args: vec![cur],
                });
            }
        }
        let span_count = if lets { 2 * depth - 1 } else { depth };
        let prog = Program {
            data: Vec::new(),
            funs: vec![FnDecl {
                name: "main".to_string(),
                params: Vec::new(),
                ret: ty_int,
                body: cur,
                module: "M".to_string(),
                kind: FnKind::Function,
                const_count: CoreNatural::zero(),
            }],
            consts: Vec::new(),
            entry: "main".to_string(),
            metadata: Metadata::Source(Box::new(SourceMetadata {
                files: Vec::new(),
                file_bytes: Vec::new(),
                modules: vec![ModuleMeta {
                    name: "M".to_string(),
                    data: Vec::new(),
                    funs: vec![0],
                    consts: Vec::new(),
                    exposed_types: Vec::new(),
                    exposed_funs: Vec::new(),
                }],
                funs: vec![FnMeta {
                    name: "M.main".to_string(),
                    span: None,
                    text: None,
                    body: BodyMeta {
                        spans: vec![None; span_count],
                        params: Vec::new(),
                        bindings: Vec::new(),
                        closures: Vec::new(),
                    },
                    subject: None,
                    allows: None,
                    lines: Vec::new(),
                }],
                consts: Vec::new(),
            })),
        };
        (arena, prog)
    };

    let limit = MO_LIMIT_CORE_DEPTH as usize;
    for lets in [true, false] {
        let (arena, mut p) = build_depth(limit, lets);
        assert!(metadata_valid(&arena, &p));
        let mut d = DiagBag::new();
        assert!(check_program(&arena, &p, &mut d) && !d.has_any());
        if let Metadata::Source(ref mut sm) = p.metadata {
            sm.funs[0].body.spans.pop();
        }
        assert!(!metadata_valid(&arena, &p));

        let (arena2, mut p2) = build_depth(limit + 1, lets);
        assert!(!metadata_valid(&arena2, &p2));
        p2.metadata = Metadata::NoSource;
        let mut d2 = DiagBag::new();
        assert!(!check_program(&arena2, &p2, &mut d2));
        assert_eq!(d2.len(), 1);
        assert_eq!(d2.items()[0].code, ErrorCode::CoreCheckLimit);
    }
}

#[test]
fn test_deterministic_fuzz_smoke_codec_and_checker() {
    let mut seeds: Vec<Vec<u8>> = Vec::new();
    for rel in [
        "tests/core/Greet.core",
        "tests/core/Metadata.core",
        "tests/core/Fakes.core",
    ] {
        seeds.push(std::fs::read(repo_root().join(rel)).unwrap());
    }
    let fuzz_dir = repo_root().join("fuzz/corpus/fuzz_core_text");
    for name in ["literal.core", "type.core", "program.core", "value.core"] {
        seeds.push(std::fs::read(fuzz_dir.join(name)).unwrap());
    }
    let mut state = 0x9e37_79b9_7f4a_7c15_u64;
    let next_u64 = |s: &mut u64| -> u64 {
        let mut x = *s;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *s = x;
        x
    };
    let all_kinds = [
        CoreTextKind::Run,
        CoreTextKind::Program,
        CoreTextKind::Type,
        CoreTextKind::Expr,
        CoreTextKind::Literal,
        CoreTextKind::Primitive,
        CoreTextKind::Value,
    ];
    for iter in 0..400usize {
        let mut buf = seeds[iter % seeds.len()].clone();
        let edits = (next_u64(&mut state) % 6) + 1;
        for _ in 0..edits {
            if buf.is_empty() {
                break;
            }
            let pos = (next_u64(&mut state) as usize) % buf.len();
            match next_u64(&mut state) % 4 {
                0 => buf[pos] = (next_u64(&mut state) & 0xff) as u8,
                1 => {
                    let end = (pos + ((next_u64(&mut state) as usize) % 16) + 1).min(buf.len());
                    buf.drain(pos..end);
                }
                2 => {
                    let b = match next_u64(&mut state) % 6 {
                        0 => b'(',
                        1 => b')',
                        2 => b'"',
                        3 => b'\\',
                        4 => b';',
                        _ => b'0',
                    };
                    buf.insert(pos, b);
                }
                _ => {
                    let slice_end = (pos + 4).min(buf.len());
                    let chunk = buf[pos..slice_end].to_vec();
                    buf.splice(pos..pos, chunk);
                }
            }
        }
        let kind = all_kinds[iter % all_kinds.len()];
        let mut diags = DiagBag::new();
        if let Some(parsed) = parse_document(kind, &buf, &mut diags) {
            let printed = print_document(&parsed.arena, &parsed.value);
            assert!(printed.is_ok(), "parsed doc must print cleanly");
            match parsed.value {
                CoreDocument::Run(ref r) => {
                    let mut cd = DiagBag::new();
                    let _ = check_program(&parsed.arena, &r.program, &mut cd);
                }
                CoreDocument::Program(ref p) => {
                    let mut cd = DiagBag::new();
                    let _ = check_program(&parsed.arena, p, &mut cd);
                }
                _ => {}
            }
        }
    }
}

#[test]
fn test_core_memory_error_codes_exercised() {
    let bytes = std::fs::read(repo_root().join("tests/core/Greet.core")).unwrap();

    // 1. E1017 (CoreNoMemory): real parser refuses when scratch budget is exhausted.
    let mut parse_diags = DiagBag::new();
    assert!(
        crate::parse_document_with_scratch_limit(CoreTextKind::Run, &bytes, 64, &mut parse_diags)
            .is_none()
    );
    assert!(
        parse_diags
            .items()
            .iter()
            .any(|d| d.code == ErrorCode::CoreNoMemory),
        "expected E1017 CoreNoMemory, got {:?}",
        parse_diags.items()
    );

    // 2. E1053 (CoreCheckMemory): real Core typechecker refuses when scratch budget is exhausted.
    let mut ok_diags = DiagBag::new();
    let parsed = parse_run(&bytes, &mut ok_diags).expect("Greet.core parses");
    let mut chk_diags = DiagBag::new();
    assert!(!crate::check_program_with_scratch_limit(
        &parsed.arena,
        &parsed.value.program,
        None,
        64,
        &mut chk_diags
    ));
    assert!(
        chk_diags
            .items()
            .iter()
            .any(|d| d.code == ErrorCode::CoreCheckMemory),
        "expected E1053 CoreCheckMemory, got {:?}",
        chk_diags.items()
    );
}
