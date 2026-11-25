//! Unit, corpus, limit, recovery, formatter contract, and deterministic fuzz/mutation tests
//! for `mo-syntax` (porting `tests/test_syntax.c`, `tests/test_syntax_corpus.c`, and
//! `tests/test_format.c`).

use std::path::{Path, PathBuf};

use mo_base::{DiagBag, SourceMap};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_CHOICE_CASES, MO_LIMIT_DIAGNOSTICS, MO_LIMIT_FIELDS, MO_LIMIT_INTERPOLATION_DEPTH,
    MO_LIMIT_LINE_LENGTH, MO_LIMIT_MODULE_NAMES, MO_LIMIT_NESTING_DEPTH, MO_LIMIT_PARAMETERS,
};
use mo_prim::unicode16::display_width_bytes;

use crate::{
    CommentAnchor, FormatOptions, ModuleSyntax, NodeId, ParseOptions, SyntaxComment, SyntaxKind,
    SyntaxTree, TokenKind, TreeCompareOptions, format_module, parse_fragment, parse_module,
    parse_module_with_options, read_header, read_header_with_options, trees_equal,
};

struct Fixture {
    sources: SourceMap,
    diags: DiagBag,
    module: Option<ModuleSyntax>,
}

impl Fixture {
    fn from_bytes(bytes: &[u8], fragment: bool) -> Self {
        let mut sources = SourceMap::new();
        let file = sources
            .add_bytes(b"test.mo".to_vec(), bytes.to_vec())
            .unwrap();
        let mut diags = DiagBag::new();
        let module = if fragment {
            parse_fragment(&sources, file, &mut diags)
        } else {
            parse_module(&sources, file, &mut diags)
        };
        Self {
            sources,
            diags,
            module,
        }
    }

    fn from_str(text: &str, fragment: bool) -> Self {
        Self::from_bytes(text.as_bytes(), fragment)
    }

    fn items_seq(&self) -> NodeId {
        let m = self.module.as_ref().unwrap();
        m.tree.node(m.root).children[1].unwrap()
    }

    fn item(&self, idx: usize) -> NodeId {
        let m = self.module.as_ref().unwrap();
        let items = self.items_seq();
        m.tree.node(items).children[idx].unwrap()
    }
}

fn check_spans(f: &Fixture, nid: NodeId) {
    let m = f.module.as_ref().unwrap();
    let src = f.sources.get(0).unwrap();
    let n = m.tree.node(nid);
    assert_eq!(n.span.file, 0);
    assert!(n.span.start <= n.span.end);
    assert!(n.span.end <= src.len());
    if let Some(text) = m.tree.node_text(nid) {
        assert_eq!(
            text,
            &src.bytes()[n.span.start as usize..n.span.end as usize]
        );
    }
    for &c in &n.children {
        if let Some(cid) = c {
            let child = m.tree.node(cid);
            assert!(
                child.span.start >= n.span.start,
                "parent {:?} {:?} vs child {:?} {:?}",
                n.kind,
                n.span,
                child.kind,
                child.span
            );
            assert!(
                child.span.end <= n.span.end,
                "parent {:?} {:?} vs child {:?} {:?}",
                n.kind,
                n.span,
                child.kind,
                child.span
            );
            check_spans(f, cid);
        }
    }
}

fn check_shape(tree: &SyntaxTree, nid: NodeId, kinds: &[SyntaxKind], at: &mut usize) {
    assert!(*at < kinds.len());
    let n = tree.node(nid);
    assert_eq!(n.kind, kinds[*at], "at index {}", *at);
    *at += 1;
    for &c in &n.children {
        if let Some(cid) = c {
            check_shape(tree, cid, kinds, at);
        }
    }
}

fn count_comment_spans(f: &Fixture, nid: NodeId) -> u32 {
    let m = f.module.as_ref().unwrap();
    let src = f.sources.get(0).unwrap();
    let n = m.tree.node(nid);
    let mut count = 0u32;
    for list in [&n.leading, &n.trailing, &n.ending] {
        for &cid in list {
            let c = m.tree.comment(cid);
            assert_eq!(c.span.file, 0);
            assert!(c.span.start < c.span.end && c.span.end <= src.len());
            assert_eq!(
                m.tree.comment_text(cid),
                &src.bytes()[c.span.start as usize..c.span.end as usize]
            );
            count += 1;
        }
    }
    for &ch in &n.children {
        if let Some(cid) = ch {
            count += count_comment_spans(f, cid);
        }
    }
    count
}

fn find_named_comment<'a>(
    tree: &'a SyntaxTree,
    nid: NodeId,
    text: &str,
) -> Option<(NodeId, &'a SyntaxComment)> {
    let n = tree.node(nid);
    for list in [&n.leading, &n.trailing, &n.ending] {
        for &cid in list {
            if tree.comment_text(cid) == text.as_bytes() {
                return Some((nid, tree.comment(cid)));
            }
        }
    }
    for &ch in &n.children {
        if let Some(cid) = ch
            && let Some(found) = find_named_comment(tree, cid, text)
        {
            return Some(found);
        }
    }
    None
}

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn format_opts(width: u32) -> FormatOptions {
    FormatOptions::new(width, display_width_bytes)
}

fn check_format_bytes(source: &[u8], expected: Option<&str>, width: u32) -> bool {
    let mut sources = SourceMap::new();
    let file = sources
        .add_bytes(b"fixture.mo".to_vec(), source.to_vec())
        .unwrap();
    let mut diags = DiagBag::new();
    let Some(module) = parse_fragment(&sources, file, &mut diags) else {
        return false;
    };
    if diags.has_any() {
        return false;
    }
    // Tree must not borrow sources.
    drop(sources);
    let output = format_module(&module, format_opts(width)).expect("format_module succeeded");
    if let Some(exp) = expected {
        assert_eq!(
            String::from_utf8_lossy(&output),
            exp,
            "format output mismatch on:\n{}",
            String::from_utf8_lossy(source)
        );
    }
    let mut sources2 = SourceMap::new();
    let file2 = sources2
        .add_bytes(b"formatted.mo".to_vec(), output.clone())
        .unwrap();
    let mut diags2 = DiagBag::new();
    let formatted = parse_fragment(&sources2, file2, &mut diags2).expect("reparse succeeded");
    assert!(
        !diags2.has_any(),
        "reparse diagnostics: {:?}\noutput:\n{}",
        diags2.items(),
        String::from_utf8_lossy(&output)
    );
    assert!(
        trees_equal(
            &module.tree,
            module.root,
            &formatted.tree,
            formatted.root,
            TreeCompareOptions::FORMAT_EQUIVALENCE
        ),
        "tree equivalence failed\ninput:\n{}\noutput:\n{}",
        String::from_utf8_lossy(source),
        String::from_utf8_lossy(&output)
    );
    let twice = format_module(&formatted, format_opts(width)).expect("second format succeeded");
    assert_eq!(
        output,
        twice,
        "format not idempotent\nfirst:\n{}\nsecond:\n{}",
        String::from_utf8_lossy(&output),
        String::from_utf8_lossy(&twice)
    );
    true
}

fn check_format_source(source: &str, expected: Option<&str>) {
    assert!(check_format_bytes(source.as_bytes(), expected, 100));
}

#[test]
fn ast_shapes() {
    use SyntaxKind::*;
    let cases: &[(&str, &[SyntaxKind])] = &[
        ("1 + 2 * 3", &[Binary, Int, Binary, Int, Int]),
        (
            "not a == b and c or d implies e",
            &[
                Binary, Binary, Binary, Not, Binary, Name, Name, Name, Name, Name,
            ],
        ),
        (
            "-try f(3)",
            &[Prefix, Token, Token, Call, Name, Sequence, Int],
        ),
        ("a..b", &[Range, Name, Name]),
        ("(a, b,)", &[Tuple, Name, Name]),
        ("(3)", &[Group, Int]),
        (
            "[true, false, 'é', 0xFF, 0b10, 2.5]",
            &[List, True, False, Char, Int, Int, Decimal],
        ),
        (
            "{a: 3, b: 5,}",
            &[Map, MapEntry, Name, Int, MapEntry, Name, Int],
        ),
        ("[]", &[List]),
        ("{}", &[Map]),
        (
            "Mod.Case(field: value)",
            &[
                Postfix, CapName, Qualified, CapName, Call, Sequence, NamedArg, Name, Name,
            ],
        ),
        (
            "pair.0.field",
            &[Postfix, Name, Position, Int, FieldAccess, Name],
        ),
        (
            "if c: a else if d: b else: e",
            &[LineIf, Name, Name, Sequence, IfPart, Name, Name, Else, Name],
        ),
        (
            "value is [head, ..rest]",
            &[Is, Name, List, Name, Rest, Name],
        ),
        (
            "\"hi #{name}: #{2}\"",
            &[TextInterpolated, Text, Name, Text, Int, Text],
        ),
        ("\"\"\"\n  a\n  b\n  \"\"\"", &[Text]),
        (
            "fn(x, _) x end",
            &[Lambda, Sequence, Name, Underscore, Name],
        ),
        (
            "fn()\n  return 3\nend",
            &[Lambda, Sequence, Sequence, Return, Int],
        ),
        ("stop \"why\"", &[Stop, Text]),
        (
            "case value\nSome(x) if x > 2: x\nNone:\n  stop \"absent\"\nend",
            &[
                Case, Name, Sequence, Arm, Sequence, Call, CapName, Sequence, Name, Binary, Name,
                Int, Sequence, ExprStmt, Name, Arm, Sequence, CapName, Sequence, ExprStmt, Stop,
                Text,
            ],
        ),
    ];

    for &(src, kinds) in cases {
        let wrapped = format!("x = {src}\n");
        let f = Fixture::from_str(&wrapped, true);
        assert!(
            f.module.is_some() && !f.diags.has_any(),
            "shape refused: {src}: {:?}",
            f.diags.items()
        );
        let m = f.module.as_ref().unwrap();
        let rhs = m.tree.node(f.item(0)).children[1].unwrap();
        let mut at = 0;
        check_shape(&m.tree, rhs, kinds, &mut at);
        assert_eq!(at, kinds.len(), "shape length mismatch for {src}");
        check_spans(&f, m.root);
    }
}

#[test]
fn declarations_and_statements() {
    let f = Fixture::from_str(
        "module All\n\
         struct Pair(T)\nleft: T\nright: Mod.Value\nend\n\
         enum Choice\nEmpty\nOne(Int)\nNamed(x: Int, y: Text,)\nend\n\
         fn f(a: fn(Int, Text): Bool, b: (Int, Text),): List(T) where T: Order\n\
         promise \"ordered\"\nresult == a\nallows a: 1, result: 1\nforbids a: 1, result: 2\nend\n\
         examples\nf(1) == 2\nend\n\
         var v = 1\nv += 2\nv.field -= 3\n(low, _) = (1, 2)\n\
         if a\nbreak if b\nelse if b\ncontinue\nelse\nreturn v if a\nend\n\
         for (x, (y, _)) in pairs\nassert x == y\nend\n\
         while a\na = b\nend\nreturn\nend\n\
         test \"works\"\nassert true\nend\nn = 42\n",
        false,
    );
    assert!(f.module.is_some() && !f.diags.has_any());
    let m = f.module.as_ref().unwrap();
    check_spans(&f, m.root);
    assert_eq!(m.tree.node(f.items_seq()).children.len(), 5);
    assert_eq!(m.tree.node(f.item(0)).kind, SyntaxKind::Struct);
    assert_eq!(m.tree.node(f.item(1)).kind, SyntaxKind::Enum);
    assert_eq!(m.tree.node(f.item(2)).kind, SyntaxKind::Fn);
    assert_eq!(m.tree.node(f.item(3)).kind, SyntaxKind::Test);
    assert_eq!(m.tree.node(f.item(4)).kind, SyntaxKind::Const);
}

#[test]
fn services_and_headers() {
    let source = "module Counter\nservice\n\
                  token from env \"COUNTER_TOKEN\"\n\
                  every interval(from) tick\n\
                  state List(Int) = []\n\
                  tools state, tools, every, token, from, env\n\
                  every 1.minute env\n\
                  end\nservice\nend\n";
    for fragment in [false, true] {
        let f = Fixture::from_str(source, fragment);
        assert!(f.module.is_some() && !f.diags.has_any());
        check_spans(&f, f.module.as_ref().unwrap().root);
    }

    // Header-only read skips broken body bytes.
    let mut sources = SourceMap::new();
    let file = sources
        .add_bytes(
            b"h.mo".to_vec(),
            b"module M\nuse A\n\xff broken body\n".to_vec(),
        )
        .unwrap();
    let mut diags = DiagBag::new();
    let parsed = read_header(&sources, file, &mut diags).expect("header_only ignores body");
    assert!(!diags.has_any());
    assert_eq!(parsed.tree.node(parsed.header.uses).children.len(), 1);

    // Header-only failure returns None and records E0008.
    let bad = sources
        .add_bytes(b"bad.mo".to_vec(), b"not a header\n".to_vec())
        .unwrap();
    assert!(read_header(&sources, bad, &mut diags).is_none());
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::SourceHeader);
}

#[test]
fn syntax_diagnostics_and_recovery() {
    let cases: &[(&[u8], ErrorCode, bool)] = &[
        (b"\"\xff\"\n", ErrorCode::SourceUtf8, true),
        (
            b"# invisible \xe2\x80\x8b\n",
            ErrorCode::SourceInvisible,
            true,
        ),
        (b"camelCase = 1\n", ErrorCode::SourceName, true),
        (b"x = 1_\n", ErrorCode::SourceNumber, true),
        (b"x = \"\\q\"\n", ErrorCode::SourceText, true),
        (b"x = 1;\n", ErrorCode::SourceToken, true),
        (b"x = (1,)\n", ErrorCode::SourceLiteral, true),
        (b"x = )\n", ErrorCode::SourceSyntax, true),
        (b"n = 1\n", ErrorCode::SourceHeader, false),
        (b"x is f(a)\n", ErrorCode::SourcePattern, true),
        (b"42 = 1\n", ErrorCode::SourceTarget, true),
        (b"_ += 1\n", ErrorCode::SourceTarget, true),
        (b"_ -= 1\n", ErrorCode::SourceTarget, true),
        (b"_.a = 1\n", ErrorCode::SourceTarget, true),
        (b"var _ = 1\n", ErrorCode::SourceSyntax, true),
        (b"module M\n_ = 1\n", ErrorCode::SourceBinding, false),
        (b"Some()\n", ErrorCode::SourceConstruction, true),
        (b"f(x: 1)\n", ErrorCode::SourceArguments, true),
        (b"struct Empty\nend\n", ErrorCode::SourceStruct, true),
        (b"enum Empty\nend\n", ErrorCode::SourceEnum, true),
        (
            b"fn f()\npromise\nend\nend\n",
            ErrorCode::SourcePromise,
            true,
        ),
        (b"case x\n42\nend\n", ErrorCode::SourceCase, true),
        (b"fn f(x Int)\nend\n", ErrorCode::SourceFunction, true),
        (
            b"fn f()\nexamples\nend\nend\n",
            ErrorCode::SourceExamples,
            true,
        ),
        (
            b"struct R\nx: List(Int, )\nend\n",
            ErrorCode::SourceType,
            true,
        ),
        (b"f(1 2)\n", ErrorCode::SourceCall, true),
        (b"if x = 1\nend\n", ErrorCode::SourceIf, true),
        (b"for x xs\nend\n", ErrorCode::SourceLoop, true),
        (b"x[1]\n", ErrorCode::SourceIndex, true),
        (b"a && b\n", ErrorCode::SourceBoolean, true),
        (b"x *= 2\n", ErrorCode::SourceAssignment, true),
        (b"M::x\n", ErrorCode::SourceQualification, true),
        (b"// comment\n", ErrorCode::SourceComment, true),
        (b"x ** 2\n", ErrorCode::SourceOperator, true),
        (b"test wrong\nend\n", ErrorCode::SourceTest, true),
        (b"module M\nx 1\n", ErrorCode::SourceBinding, false),
        (b"[1 2]\n", ErrorCode::SourceList, true),
        (b"{1 2}\n", ErrorCode::SourceMap, true),
    ];

    for &(text, code, fragment) in cases {
        let f = Fixture::from_bytes(text, fragment);
        assert!(f.module.is_some());
        assert_eq!(
            f.diags.len(),
            1,
            "expected 1 diag for {:?}, got {:?}",
            String::from_utf8_lossy(text),
            f.diags.items()
        );
        let d = &f.diags.items()[0];
        assert_eq!(d.code, code);
        assert!(d.reason.is_some());
        assert_eq!(d.section, code.section());
        assert!(d.primary.span.end as usize <= text.len());
    }

    // Recovery inside a function retains surrounding items and next declaration.
    let mistakes: &[&[u8]] = &[
        b"bad = (1 + )\n",
        b"bad = None()\n",
        b"badName = 1\n",
        b"bad = \"\\q\"\n",
        b"bad = (\n",
        b"bad = [\n",
        b"# \xff\n",
    ];
    for &mistake in mistakes {
        let mut src = Vec::new();
        src.extend_from_slice(b"module M\nfn f()\n  before = 1\n");
        src.extend_from_slice(mistake);
        src.extend_from_slice(b"  after = 2\n  return after\nend\nfn g()\n  3\nend\n");
        let f = Fixture::from_bytes(&src, false);
        assert!(f.module.is_some());
        assert_eq!(f.diags.len(), 1);
        let m = f.module.as_ref().unwrap();
        assert_eq!(m.tree.node(f.items_seq()).children.len(), 2);
        let fn0 = m.tree.node(f.item(0));
        assert!(fn0.incomplete);
        assert_eq!(m.tree.node(fn0.children[5].unwrap()).children.len(), 3);
        let fn1 = m.tree.node(f.item(1));
        assert_eq!(fn1.kind, SyntaxKind::Fn);
        assert!(!fn1.incomplete);
    }
}

#[test]
fn comment_anchors_and_optional_separators() {
    let f = Fixture::from_str(
        "module A. # dot first\nB. # dot second\nC\n\
         fn f(a: Int, b: Bool) where # where\nA: Copy, B: Copy\n\
           examples # examples\n    true\n  end # examples close\n\
           text = \"#{Pair( # interpolated open\nx: 1)}\"\n\
           x = # assign\n    not # not\n    true\n\
           y = a + # binary\n    b\n\
           z = a + # chain first\n    b + # chain second\n    c\n\
           xs. # field dot\n    first. # chain dot\n    other( # call open\n    1, 2) # call close\n\
           ( # group open\n    1) # group close\n\
           [ # list open\n    1, 2] # list close\n\
           { # map open\n    1: 2} # map close\n\
           map_text = \"#{{ # interpolated map open\n1: 2}}\"\n\
           if true\n    1\n  else if false\n    2\n  else # else\n    3\n  end # conditional close\n\
         end # function close\n",
        false,
    );
    assert!(f.module.is_some() && !f.diags.has_any());
    let m = f.module.as_ref().unwrap();
    let expected: &[(&str, SyntaxKind, TokenKind, u32)] = &[
        ("# dot first", SyntaxKind::Module, TokenKind::Dot, 0),
        ("# dot second", SyntaxKind::Module, TokenKind::Dot, 1),
        (
            "# interpolated open",
            SyntaxKind::Call,
            TokenKind::LParen,
            0,
        ),
        ("# where", SyntaxKind::Where, TokenKind::Where, 0),
        ("# examples", SyntaxKind::Examples, TokenKind::Examples, 0),
        ("# examples close", SyntaxKind::Examples, TokenKind::End, 0),
        ("# assign", SyntaxKind::Bind, TokenKind::Assign, 0),
        ("# not", SyntaxKind::Not, TokenKind::Not, 0),
        ("# binary", SyntaxKind::Binary, TokenKind::Plus, 0),
        ("# chain first", SyntaxKind::Token, TokenKind::Plus, 0),
        ("# chain second", SyntaxKind::Token, TokenKind::Plus, 0),
        ("# field dot", SyntaxKind::FieldAccess, TokenKind::Dot, 0),
        ("# chain dot", SyntaxKind::FieldAccess, TokenKind::Dot, 0),
        ("# call open", SyntaxKind::Call, TokenKind::LParen, 0),
        ("# call close", SyntaxKind::Call, TokenKind::RParen, 0),
        ("# group open", SyntaxKind::Group, TokenKind::LParen, 0),
        ("# group close", SyntaxKind::Group, TokenKind::RParen, 0),
        ("# list open", SyntaxKind::List, TokenKind::LBrack, 0),
        ("# list close", SyntaxKind::List, TokenKind::RBrack, 0),
        ("# map open", SyntaxKind::Map, TokenKind::LBrace, 0),
        (
            "# interpolated map open",
            SyntaxKind::Map,
            TokenKind::LBrace,
            0,
        ),
        ("# map close", SyntaxKind::Map, TokenKind::RBrace, 0),
        ("# else", SyntaxKind::Else, TokenKind::Else, 0),
        ("# conditional close", SyntaxKind::If, TokenKind::End, 0),
        ("# function close", SyntaxKind::Fn, TokenKind::End, 0),
    ];
    assert_eq!(count_comment_spans(&f, m.root) as usize, expected.len());
    for &(comment, owner_kind, tok, idx) in expected {
        let (owner_id, c) = find_named_comment(&m.tree, m.root, comment).unwrap();
        assert_eq!(m.tree.node(owner_id).kind, owner_kind, "{comment}");
        assert_eq!(c.anchor, CommentAnchor::Token, "{comment}");
        assert_eq!(c.token, tok, "{comment}");
        assert_eq!(c.token_index, idx, "{comment}");
    }

    // Optional trailing comma produces identical ASTs with ITEM boundary anchor.
    let pairs = [
        ("[1, # final\n]\n", "[1 # final\n]\n"),
        ("f(1, # final\n)\n", "f(1 # final\n)\n"),
        (
            "Pair(a: 1, b: 2, # final\n)\n",
            "Pair(a: 1, b: 2 # final\n)\n",
        ),
        ("(1, 2, # final\n)\n", "(1, 2 # final\n)\n"),
        ("{1: 2, # final\n}\n", "{1: 2 # final\n}\n"),
        (
            "fn f(x: Int, # final\n)\nend\n",
            "fn f(x: Int # final\n)\nend\n",
        ),
        (
            "enum E\nCase(x: Int, # final\n)\nend\n",
            "enum E\nCase(x: Int # final\n)\nend\n",
        ),
    ];
    for (with_comma, without_comma) in pairs {
        let a = Fixture::from_str(with_comma, true);
        let b = Fixture::from_str(without_comma, true);
        assert!(!a.diags.has_any() && !b.diags.has_any());
        let ma = a.module.as_ref().unwrap();
        let mb = b.module.as_ref().unwrap();
        assert!(trees_equal(
            &ma.tree,
            ma.root,
            &mb.tree,
            mb.root,
            TreeCompareOptions::EXACT
        ));
        let (_, c) = find_named_comment(&ma.tree, ma.root, "# final").unwrap();
        assert_eq!(c.anchor, CommentAnchor::Item);
    }
}

#[test]
fn source_limits_and_arena_budgets() {
    let check_boundary = |bytes: &[u8], expected: Option<ErrorCode>, fragment: bool| {
        let f = Fixture::from_bytes(bytes, fragment);
        match expected {
            None => assert!(f.module.is_some() && !f.diags.has_any()),
            Some(code) => {
                assert_eq!(
                    f.diags.len(),
                    1,
                    "expected {code}, got {:?}",
                    f.diags.items()
                );
                assert_eq!(f.diags.items()[0].code, code);
            }
        }
    };

    for extra in 0..2u64 {
        let mut line = String::from("#");
        for _ in 0..(MO_LIMIT_LINE_LENGTH - 1 + extra) {
            line.push('é');
        }
        line.push_str("\r\n");
        check_boundary(
            line.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::LineTooLong)
            },
            true,
        );

        let depth = (MO_LIMIT_NESTING_DEPTH + extra) as usize;
        let nested = format!("{}0{}\n", "[".repeat(depth), "]".repeat(depth));
        check_boundary(
            nested.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::NestedTooDeeply)
            },
            true,
        );

        let interp_depth = (MO_LIMIT_INTERPOLATION_DEPTH + extra) as usize;
        let interp = format!(
            "{}0{}\n",
            "\"#{".repeat(interp_depth),
            "}\"".repeat(interp_depth)
        );
        check_boundary(
            interp.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::InterpolationsTooDeep)
            },
            true,
        );

        let params = (MO_LIMIT_PARAMETERS + extra) as usize;
        let fn_src = format!("module M\nfn f({})\nend\n", "p: Int,\n".repeat(params));
        check_boundary(
            fn_src.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::TooManyParameters)
            },
            false,
        );

        let fields = (MO_LIMIT_FIELDS + extra) as usize;
        let struct_src = format!("struct R\n{}end\n", "f: Int\n".repeat(fields));
        check_boundary(
            struct_src.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::TooManyFields)
            },
            true,
        );

        let cases_n = (MO_LIMIT_CHOICE_CASES + extra) as usize;
        let enum_src = format!("enum E\n{}end\n", "C\n".repeat(cases_n));
        check_boundary(
            enum_src.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::TooManyCases)
            },
            true,
        );

        let names_n = (MO_LIMIT_MODULE_NAMES + extra) as usize;
        let mod_src = format!("module M\n{}", "n = 1\n".repeat(names_n));
        check_boundary(
            mod_src.as_bytes(),
            if extra == 0 {
                None
            } else {
                Some(ErrorCode::TooManyNames)
            },
            false,
        );
    }

    // Zero arena budget yields None + E1054.
    let mut sources = SourceMap::new();
    let file = sources
        .add_bytes(b"m.mo".to_vec(), b"module M\n".to_vec())
        .unwrap();
    let mut diags = DiagBag::new();
    assert!(
        parse_module_with_options(&sources, file, &mut diags, ParseOptions { arena_limit: 0 })
            .is_none()
    );
    assert_eq!(diags.len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::SyntaxNoMemory);

    // Header read with tiny budget also fails cleanly with E1054.
    let mut hdiags = DiagBag::new();
    assert!(
        read_header_with_options(&sources, file, &mut hdiags, ParseOptions { arena_limit: 0 })
            .is_none()
    );
    assert_eq!(hdiags.items()[0].code, ErrorCode::SyntaxNoMemory);

    // Exhausted diagnostics bag turns syntax error into None + dropped == 1.
    let mut full_diags = DiagBag::new();
    let filler = mo_base::Diagnostic::new(
        ErrorCode::SourceSyntax,
        mo_base::Span::new(file, 0, 0),
        "a value is an expression",
    )
    .with_reason("A value is an expression (reference 2.4).");
    for _ in 0..MO_LIMIT_DIAGNOSTICS {
        assert!(full_diags.push(filler.clone()).unwrap());
    }
    let bad = sources
        .add_bytes(b"bad.mo".to_vec(), b"module M\nn = )\n".to_vec())
        .unwrap();
    assert!(parse_module(&sources, bad, &mut full_diags).is_none());
    assert_eq!(full_diags.dropped(), 1);
}

#[test]
fn formatter_rules_and_fixed_points() {
    // Duration units singular/plural normalization.
    check_format_source(
        "1.seconds\n1.milliseconds\n1.minutes\n1.hours\n1.days\n\
         0.second\n2.millisecond\n2.minute\n2.hour\n2.day\n",
        Some(
            "1.second\n1.millisecond\n1.minute\n1.hour\n1.day\n\
             0.seconds\n2.milliseconds\n2.minutes\n2.hours\n2.days\n",
        ),
    );
    // += rewrite and comment deferral.
    check_format_source(
        "module Main\nfn f()\nvar x=1\nx=x+2\nx\nend\n",
        Some("module Main\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n"),
    );
    check_format_source(
        "module Main\nfn f()\n  var x = 1\n  x = x + # keep\n    2\nend\n",
        Some("module Main\nfn f()\n  var x = 1\n  x = x +  # keep\n    2\nend\n"),
    );
    // Inline comment alignment with CJK display width.
    check_format_source(
        "a=\"界\" # wide\nb=\"xx\" # ascii\nc=0 # short\n",
        Some("a = \"界\"  # wide\nb = \"xx\"  # ascii\nc = 0     # short\n"),
    );
    // 100-column boundary with 77 display columns / 101 UTF-8 bytes.
    check_format_source(
        "x=f(\"界界界界界界界界界界界界界界界界界界界界界界界界\",\"abcdefghijklmnop\")\n",
        Some("x = f(\"界界界界界界界界界界界界界界界界界界界界界界界界\", \"abcdefghijklmnop\")\n"),
    );
    // Exact 87 vs 88 column dot break boundary.
    for length in 87..=88 {
        let lit = "x".repeat(length);
        let src = format!("x=\"{lit}\".length\n");
        let exp = if length == 87 {
            format!("x = \"{lit}\".length\n")
        } else {
            format!("x = \"{lit}\"\n  .length\n")
        };
        check_format_source(&src, Some(&exp));
    }
    // Reference 12 blank line after spec block.
    check_format_source(
        "fn f(x: Int) : Int\n  examples\n    f(1) == 2\n  end\n  x + 1\nend\n",
        Some("fn f(x: Int) : Int\n  examples\n    f(1) == 2\n  end\n\n  x + 1\nend\n"),
    );
    // Lambda-list & type args.
    check_format_source(
        "f = fn(x) [x, x] end\nx = Json.decode[User](raw)\n",
        Some("f = fn(x) [x, x] end\nx = Json.decode[User](raw)\n"),
    );
    // API clauses.
    check_format_source(
        "api\njob \"mercury.sync\" MercurySync.run every 15.minutes\n\
         job \"tick\" tick every 1.seconds\n\
         job \"csv.import\" CsvImport.run\n\
         get \"/health\" health\nend\n",
        Some(
            "api\n  job \"mercury.sync\" MercurySync.run every 15.minutes\n\
             \x20 job \"tick\" tick every 1.second\n\
             \x20 job \"csv.import\" CsvImport.run\n\
             \x20 get \"/health\" health\nend\n",
        ),
    );
}

#[test]
fn grammar_corpus_and_reference_blocks() {
    let root = repo_root();
    let good_dir = root.join("grammar/tests/good");
    let bad_dir = root.join("grammar/tests/bad");
    let bad_files_dir = root.join("grammar/tests/bad-files");

    let mut good_count = 0usize;
    for entry in std::fs::read_dir(&good_dir).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().and_then(|s| s.to_str()) != Some("mo") {
            continue;
        }
        let bytes = std::fs::read(&path).unwrap();
        let f = Fixture::from_bytes(&bytes, true);
        assert!(
            f.module.is_some() && !f.diags.has_any(),
            "good grammar fixture failed: {}: {:?}",
            path.display(),
            f.diags.items()
        );
        check_spans(&f, f.module.as_ref().unwrap().root);
        assert!(check_format_bytes(&bytes, None, 100));
        good_count += 1;
    }
    assert_eq!(good_count, 13);

    let mut bad_count = 0usize;
    for entry in std::fs::read_dir(&bad_dir).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().and_then(|s| s.to_str()) != Some("mo") {
            continue;
        }
        let bytes = std::fs::read(&path).unwrap();
        let f = Fixture::from_bytes(&bytes, true);
        assert_eq!(
            f.diags.len(),
            1,
            "bad grammar fixture expected 1 diag: {}: {:?}",
            path.display(),
            f.diags.items()
        );
        bad_count += 1;
    }
    assert_eq!(bad_count, 90);

    let mut bad_file_count = 0usize;
    for entry in std::fs::read_dir(&bad_files_dir).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().and_then(|s| s.to_str()) != Some("mo") {
            continue;
        }
        let bytes = std::fs::read(&path).unwrap();
        let f = Fixture::from_bytes(&bytes, false);
        assert_eq!(
            f.diags.len(),
            1,
            "bad-files fixture expected 1 diag: {}: {:?}",
            path.display(),
            f.diags.items()
        );
        bad_file_count += 1;
    }
    assert_eq!(bad_file_count, 3);

    // Every ```mo block in docs/reference/README.md parses and satisfies format contract.
    let ref_bytes = std::fs::read(root.join("docs/reference/README.md")).unwrap();
    let mut pos = 0usize;
    let mut inside = false;
    let mut start = 0usize;
    let mut blocks = 0usize;
    while pos < ref_bytes.len() {
        let mut end = pos;
        while end < ref_bytes.len() && ref_bytes[end] != b'\n' {
            end += 1;
        }
        let line = &ref_bytes[pos..end];
        if !inside && line == b"```mo" {
            inside = true;
            start = end + 1;
        } else if inside && line == b"```" {
            let block = &ref_bytes[start..pos];
            assert!(
                check_format_bytes(block, None, 100),
                "reference block {blocks} failed"
            );
            blocks += 1;
            inside = false;
        }
        pos = if end < ref_bytes.len() { end + 1 } else { end };
    }
    assert!(!inside);
    assert!(blocks >= 50);
}

struct XorShift64(u64);

impl XorShift64 {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    fn range(&mut self, upper: usize) -> usize {
        if upper == 0 {
            0
        } else {
            (self.next() as usize) % upper
        }
    }
}

#[test]
fn deterministic_lexer_parser_formatter_fuzz_smoke() {
    let root = repo_root();
    let mut seeds: Vec<Vec<u8>> = Vec::new();
    for dir in [
        root.join("grammar/tests/good"),
        root.join("grammar/tests/bad"),
        root.join("examples"),
        root.join("fuzz/corpus/fuzz_syntax"),
    ] {
        if !dir.is_dir() {
            continue;
        }
        let mut entries: Vec<PathBuf> = std::fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.is_file())
            .collect();
        entries.sort();
        for p in entries {
            if let Ok(bytes) = std::fs::read(&p)
                && bytes.len() <= 8192
            {
                seeds.push(bytes);
            }
        }
    }
    assert!(!seeds.is_empty());

    const TOKENS: &[&[u8]] = &[
        b"(",
        b")",
        b"[",
        b"]",
        b"{",
        b"}",
        b",",
        b":",
        b".",
        b"..",
        b"#",
        b"##",
        b"#{",
        b"\"\"\"",
        b"\"",
        b"'",
        b"\n",
        b"\r\n",
        b" ",
        b"  ",
        b" # note\n",
        b"fn",
        b"end",
        b"if",
        b"else",
        b"case",
        b"service",
        b"api",
        b"actor",
        b"module",
        b"use",
        b"expose",
        b"=",
        b"+=",
        b"-=",
        b"=>",
        b"||",
        b"&&",
        b"!",
        b"\\",
        b"\\u{200b}",
        b"\xff",
    ];

    let mut rng = XorShift64(0x5eed_cafe_1234_9876);
    let mut accepted = 0usize;

    for iter in 0..1500usize {
        let mut buf = seeds[rng.range(seeds.len())].clone();
        let edits = if iter % 5 == 0 { 1 } else { 1 + rng.range(4) };
        for _ in 0..edits {
            match rng.range(6) {
                0 if !buf.is_empty() => {
                    // Flip byte.
                    let i = rng.range(buf.len());
                    buf[i] ^= 1u8 << (rng.next() % 8);
                }
                1 if !buf.is_empty() => {
                    // Delete slice.
                    let a = rng.range(buf.len());
                    let b = a + 1 + rng.range((buf.len() - a).min(16));
                    buf.drain(a..b.min(buf.len()));
                }
                2 => {
                    // Insert token.
                    let tok = TOKENS[rng.range(TOKENS.len())];
                    let pos = rng.range(buf.len() + 1);
                    buf.splice(pos..pos, tok.iter().copied());
                }
                3 if !buf.is_empty() => {
                    // Duplicate a short slice.
                    let a = rng.range(buf.len());
                    let len = 1 + rng.range((buf.len() - a).min(24));
                    let slice = buf[a..a + len].to_vec();
                    let pos = rng.range(buf.len() + 1);
                    buf.splice(pos..pos, slice);
                }
                4 if !buf.is_empty() => {
                    // Insert whitespace or line comment at a newline boundary (keeps valid syntax valid).
                    let pos = rng.range(buf.len());
                    if let Some(nl) = buf[pos..].iter().position(|&b| b == b'\n') {
                        let at = pos + nl;
                        let insert: &[u8] = match rng.range(3) {
                            0 => b"  # inline",
                            1 => b"\n# own line\n",
                            _ => b"\n\n",
                        };
                        buf.splice(at..at, insert.iter().copied());
                    }
                }
                _ => {
                    // Splice from another seed.
                    let other = &seeds[rng.range(seeds.len())];
                    if !other.is_empty() {
                        let a = rng.range(other.len());
                        let len = 1 + rng.range((other.len() - a).min(32));
                        let pos = rng.range(buf.len() + 1);
                        buf.splice(pos..pos, other[a..a + len].iter().copied());
                    }
                }
            }
            if buf.len() > 16384 {
                buf.truncate(16384);
            }
        }

        let fragment = (iter % 2) == 0;
        let mut sources = SourceMap::new();
        let Ok(file) = sources.add_bytes(b"fuzz.mo".to_vec(), buf.clone()) else {
            continue;
        };
        let mut diags = DiagBag::new();
        let parsed = if fragment {
            parse_fragment(&sources, file, &mut diags)
        } else {
            parse_module(&sources, file, &mut diags)
        };
        if let Some(module) = parsed
            && !diags.has_any()
        {
            accepted += 1;
            if let Ok(once) = format_module(&module, format_opts(100)) {
                let mut s2 = SourceMap::new();
                let f2 = s2
                    .add_bytes(b"fmt.mo".to_vec(), once.clone())
                    .expect("formatted output fits SourceMap");
                let mut d2 = DiagBag::new();
                let m2 = if fragment {
                    parse_fragment(&s2, f2, &mut d2)
                } else {
                    parse_module(&s2, f2, &mut d2)
                }
                .expect("formatted output must reparse without fatal error");
                assert!(
                    !d2.has_any(),
                    "formatted fuzz output failed reparse: {:?}\n{}",
                    d2.items(),
                    String::from_utf8_lossy(&once)
                );
                assert!(
                    trees_equal(
                        &module.tree,
                        module.root,
                        &m2.tree,
                        m2.root,
                        TreeCompareOptions::FORMAT_EQUIVALENCE
                    ),
                    "fuzz iter={iter} fragment={fragment} format changed tree:\n--- INPUT ---\n{}\n--- OUTPUT ---\n{}",
                    String::from_utf8_lossy(&buf),
                    String::from_utf8_lossy(&once)
                );
                let twice =
                    format_module(&m2, format_opts(100)).expect("second format must succeed");
                assert_eq!(once, twice, "fuzz format not idempotent");
            }
        }
    }
    assert!(
        accepted > 50,
        "expected >= 50 accepted mutated seeds, got {accepted}"
    );
}

#[test]
fn reserved_keyword_field_and_param_recovery() {
    // 1. Struct field named `actor: Text` -> single E0013 diagnostic mentioning reserved keyword,
    //    no E0007 cascade, subsequent fields and functions parse cleanly.
    let f_struct = Fixture::from_str(
        "module Main\n\n\
         struct AuditEvent\n\
           id: Int\n\
           actor: Text\n\
           action: Text\n\
         end\n\n\
         fn helper(x: Int) : Int\n\
           x + 1\n\
         end\n",
        false,
    );
    assert_eq!(
        f_struct.diags.len(),
        1,
        "expected 1 diagnostic, got: {:?}",
        f_struct.diags.items()
    );
    let d = &f_struct.diags.items()[0];
    assert_eq!(d.code, ErrorCode::SourceStruct);
    let m1 = d.message.as_str_lossy();
    assert!(
        m1.contains("actor") && m1.contains("reserved keyword"),
        "unexpected message: {m1}"
    );

    // 2. Single-line and multi-line function parameters named `actor: Text` ->
    //    single E0017 diagnostic per misuse, zero E0007 actor-clause cascade.
    let f_fn = Fixture::from_str(
        "module Main\n\n\
         fn log_event(\n\
           id: Int,\n\
           actor: Text,\n\
           count: Int\n\
         ) : Int\n\
           a = id + count\n\
           b = a * 2\n\
           b + 1\n\
         end\n\n\
         fn after_ok(n: Int) : Int\n\
           n\n\
         end\n",
        false,
    );
    assert_eq!(
        f_fn.diags.len(),
        1,
        "expected 1 diagnostic without E0007 cascade, got: {:?}",
        f_fn.diags.items()
    );
    let d2 = &f_fn.diags.items()[0];
    assert_eq!(d2.code, ErrorCode::SourceFunction);
    let m2 = d2.message.as_str_lossy();
    assert!(
        m2.contains("actor") && m2.contains("reserved keyword"),
        "unexpected message: {m2}"
    );
    assert!(
        f_fn.diags
            .items()
            .iter()
            .all(|item| item.code != ErrorCode::SourceSyntax),
        "must not cascade into E0007: {:?}",
        f_fn.diags.items()
    );

    // 3. `service` and `api` parameters and enum named fields also recover cleanly.
    let f_enum = Fixture::from_str(
        "module Main\n\n\
         enum Audit\n\
           Recorded(actor: Text, count: Int)\n\
         end\n\n\
         fn dispatch(service: Text, api: Text) : Int\n\
           1\n\
         end\n",
        false,
    );
    assert_eq!(
        f_enum.diags.len(),
        3,
        "expected 3 crisp diagnostics, got: {:?}",
        f_enum.diags.items()
    );
    assert_eq!(f_enum.diags.items()[0].code, ErrorCode::SourceEnum);
    assert_eq!(f_enum.diags.items()[1].code, ErrorCode::SourceFunction);
    assert_eq!(f_enum.diags.items()[2].code, ErrorCode::SourceFunction);
    for item in f_enum.diags.items() {
        let msg = item.message.as_str_lossy();
        assert!(
            msg.contains("reserved keyword"),
            "unexpected message: {msg}"
        );
    }
}

#[test]
fn misaligned_own_line_comments_preserve_position_and_equivalence() {
    // 1. Over-indented comment before `x = 1` must stay above `x = 1`, never teleporting into
    //    the inner `if` block that happens to share column 4.
    let over_indented = "module Main\n\nfn f(flag: Bool) : Int\n    # before x\n  x = 1\n  if flag\n    x + 2\n  else\n    x + 3\n  end\nend\n";
    let over_indented_expected = "module Main\n\nfn f(flag: Bool) : Int\n  # before x\n  x = 1\n  if flag\n    x + 2\n  else\n    x + 3\n  end\nend\n";
    check_format_source(over_indented, Some(over_indented_expected));

    // 2. Column-0 comment before the first `case` arm stays above the first arm, never
    //    teleporting after the last arm.
    let case_comment = "module Main\n\nfn f(x: Int) : Int\n  case x\n# before first arm\n    1: 10\n    _: 20\n  end\nend\n";
    let case_comment_expected = "module Main\n\nfn f(x: Int) : Int\n  case x\n    # before first arm\n    1: 10\n    _: 20\n  end\nend\n";
    check_format_source(case_comment, Some(case_comment_expected));

    // 3. Branch-boundary comments preserve whether they end the previous branch or lead the next.
    let branch_comments = "module Main\n\nfn f(a: Bool, b: Bool) : Int\n  if a\n    x = 1\n    # end of then\n  # before else if\n  else if b\n    y = 2\n    # end of else if\n  # before else\n  else\n    z = 3\n    # end of else\n  end\n  0\nend\n";
    check_format_source(branch_comments, Some(branch_comments));

    // 4. Case-arm boundary comments preserve ending-arm vs leading-next-arm placement.
    let arm_comments = "module Main\n\nfn f(x: Int) : Int\n  case x\n    1:\n      a = 1\n      a + 1\n      # end of arm 1\n    # before arm 2\n    _: 2\n  end\nend\n";
    check_format_source(arm_comments, Some(arm_comments));
    let single_stmt_arm = "module Main\n\nfn f(x: Int) : Int\n  case x\n    1: a = 1\n      # end of arm 1\n    # before arm 2\n    _: 2\n  end\nend\n";
    check_format_source(single_stmt_arm, Some(single_stmt_arm));
}

#[test]
fn alternating_branch_comment_indentation_does_not_reorder_comments() {
    let source = "module Main\n\nfn f(flag: Bool) : Int\n  if flag\n    1\n    # first\n  # second\n    # third\n  else\n    2\n  end\nend\n";
    let expected = "module Main\n\nfn f(flag: Bool) : Int\n  if flag\n    1\n    # first\n  # second\n  # third\n  else\n    2\n  end\nend\n";
    check_format_source(source, Some(expected));
}
