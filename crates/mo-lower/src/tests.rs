//! Unit and deterministic fuzz smoke tests for `mo-lower` (`tests/test_lower.c` &
//! `ci/check_lower_source.py` coverage).

use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use mo_base::{DiagBag, SourceMap};
use mo_check::{LibraryMode, LoadOptions, check_program, load_program, load_program_sources};
use mo_core::{check_program_services, print_program};
use mo_def::ErrorCode;

use crate::{
    LowerOptions, lower_for_tests, lower_services, lower_test_services, lower_with_options,
};

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(prefix: &str) -> Self {
        let id = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let path = std::env::temp_dir().join(format!("mo-lower-{prefix}-{pid}-{id}"));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).expect("create temp dir");
        Self { path }
    }

    fn write(&self, rel: &str, contents: &str) -> PathBuf {
        let p = self.path.join(rel);
        fs::write(&p, contents).expect("write temp source");
        p
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn check_and_lower(source: &str, with_lib: bool, tests_mode: bool) -> (Option<String>, DiagBag) {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let _tmp_guard;
    let modules = if with_lib {
        let tmp = TempDir::new("lib");
        let path = tmp.write("main.mo", source);
        _tmp_guard = Some(tmp);
        let options = LoadOptions {
            library: LibraryMode::default_embedded(),
            ..LoadOptions::default()
        };
        let path_str = path.to_string_lossy().into_owned();
        load_program(&mut sources, &path_str, &options, &mut diags)
    } else {
        _tmp_guard = None;
        let _ = sources
            .add_bytes(b"main.mo".to_vec(), source.as_bytes().to_vec())
            .expect("add source");
        load_program_sources(&mut sources, None, &mut diags)
    };
    let Some(modules) = modules else {
        return (None, diags);
    };
    let Some(checked) = check_program(Some(&sources), modules, &mut diags) else {
        return (None, diags);
    };
    let lowered = if tests_mode {
        lower_test_services(&checked, Some(&sources), &mut diags)
    } else {
        lower_services(&checked, Some(&sources), &mut diags)
    };
    let Some(lowered) = lowered else {
        return (None, diags);
    };
    let svc = (!lowered.services.items.is_empty()).then_some(&lowered.services);
    let ok = check_program_services(&lowered.arena, &lowered.program, svc, &mut diags);
    assert!(
        ok,
        "lowered Core failed mo_core::check_program_services: {:?}",
        diags.items()
    );
    let printed = print_program(&lowered.arena, &lowered.program).expect("print_program");
    (Some(printed), diags)
}

#[test]
fn lowers_simple_main_and_validates_in_mo_core() {
    let src = "module Main\n\nfn main() : Int\n  40 + 2\nend\n";
    let (core, diags) = check_and_lower(src, false, false);
    assert!(!diags.has_any(), "unexpected diags: {:?}", diags.items());
    let core = core.expect("core text");
    assert!(core.contains("Main.main"));
    assert!(core.contains("%entry"));
}

#[test]
fn lowers_generics_patterns_loops_and_derived_abilities() {
    let src = r#"module Main

struct Pair(A, B)
  first: A
  second: B
end

enum Shape
  Circle(Int)
  Rect(width: Int, height: Int)
end

fn swap(p: Pair(A, B)) : Pair(B, A)
  Pair(first: p.second, second: p.first)
end

fn describe(s: Shape) : Text
  case s
    Circle(r): "c=#{r}"
    Rect(width: w, height: h) if w == h: "sq=#{w}"
    Rect(width: w, height: h): "rect=#{w}x#{h}"
  end
end

fn sum_range(n: Int) : Int
  var total = 0
  for i in 0..n
    total += i
  end
  total
end

fn main() : Int
  p = swap(Pair(first: 1, second: "hi"))
  t = p.to_text
  _ = describe(Circle(3))
  _ = describe(Rect(width: 2, height: 2))
  _ = (Pair(first: 1, second: 2) < Pair(first: 1, second: 3))
  if t.empty?
    1
  else
    sum_range(5)
  end
end
"#;
    let (core, diags) = check_and_lower(src, false, false);
    assert!(!diags.has_any(), "unexpected diags: {:?}", diags.items());
    let core = core.expect("core text");
    assert!(core.contains("Main::Main.swap("));
    assert!(core.contains(".%to_text("));
    assert!(core.contains(".%compare("));
}

#[test]
fn lowers_scalars_float_negative_min_and_triple_text() {
    let src = r#"module Main

fn take_float(x: Float) : Float
  x
end

fn take_decimal(x: Decimal) : Decimal
  x
end

fn take_byte(x: Byte) : Byte
  x
end

fn take_big(x: BigInt) : BigInt
  x
end

fn main() : Int
  min_i = -9223372036854775808
  _ = min_i
  f1 = take_float(-0.0)
  f2 = take_float(0b1010)
  f3 = take_float(0x10)
  _ = (f1, f2, f3)
  d = take_decimal(-12.345)
  b = take_byte(255)
  big = take_big(-0x1_0000_0000_0000_0000)
  ch = '\u{1f600}'
  s = """
    hello #{42}
      world
    """
  _ = (d, b, big, ch, s)
  0
end
"#;
    let (core, diags) = check_and_lower(src, false, false);
    assert!(!diags.has_any(), "unexpected diags: {:?}", diags.items());
    let core = core.expect("core text");
    assert!(core.contains("-9223372036854775808"));
    assert!(core.contains("-12345000000000000000"));
}

#[test]
fn lowers_specs_promises_anchors_and_vacuity() {
    let src = r#"module Main

fn double(x: Int) : Int
  examples
    double(2) == 4
  end
  promise "non-negative stays non-negative"
    x >= 0 implies result >= 0
    allows x: 3, result: 6
    forbids x: 3, result: -1
  end
  x * 2
end

test "double works"
  assert double(5) == 10
end
"#;
    let (core, diags) = check_and_lower(src, false, true);
    assert!(!diags.has_any(), "unexpected diags: {:?}", diags.items());
    let core = core.expect("core text");
    assert!(core.contains("Main.double.example.1"));
    assert!(core.contains("Main.double.promise.1"));
    assert!(core.contains("Main.double.promise.1.allows.1"));
    assert!(core.contains("Main.double.promise.1.forbids.1"));
    assert!(core.contains("Main.test.1"));

    // Verify `lower_for_tests` collects concrete promise parameter source types.
    let mut sources = SourceMap::new();
    let mut diags2 = DiagBag::new();
    let _ = sources
        .add_bytes(b"main.mo".to_vec(), src.as_bytes().to_vec())
        .expect("add");
    let mods = load_program_sources(&mut sources, None, &mut diags2).expect("load");
    let checked = check_program(Some(&sources), mods, &mut diags2).expect("check");
    let lowered = lower_for_tests(&checked, Some(&sources), &mut diags2).expect("lower");
    assert_eq!(lowered.promises.len(), 1);
    assert_eq!(lowered.promises[0].name, "Main.double.promise.1");
    assert_eq!(lowered.promises[0].parameters.len(), 1);
    assert_eq!(lowered.promises[0].parameters[0].name, "Int");
}

#[test]
fn lowers_json_ability_and_schema() {
    let src = r#"module Main

use Json

struct Item
  name: Text
  count: Int
  note: Option(Text)
end

fn main() : Int
  item = Item(name: "apple", count: 3, note: None)
  j = item.to_json
  decoded = Json.from_json[Item](j)
  _ = Json.schema[Item]()
  if decoded == Ok(item): 0 else: 1
end
"#;
    let (core, diags) = check_and_lower(src, true, false);
    assert!(!diags.has_any(), "unexpected diags: {:?}", diags.items());
    let core = core.expect("core text");
    assert!(core.contains(".%to_json("));
    assert!(core.contains(".%from_json("));
    assert!(core.contains(".%json_schema("));
}

#[test]
fn rejects_e1150_deferred_constant_forward_global() {
    let src = "module Main\n\nhandler = fn() base end\n\nbase = 13\n\nfn main() : Int\n  handler()\nend\n";
    let (core, diags) = check_and_lower(src, false, false);
    assert!(core.is_none());
    assert_eq!(diags.items().len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::LowerUnfinished);
    assert_eq!(
        diags.items()[0].message.as_str_lossy(),
        "this constant's deferred function needs globals not yet initialized"
    );
}

#[test]
fn rejects_e1151_arena_budget_exhausted() {
    let src = "module Main\n\nfn main() : Int\n  42\nend\n";
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let _ = sources
        .add_bytes(b"main.mo".to_vec(), src.as_bytes().to_vec())
        .expect("add");
    let mods = load_program_sources(&mut sources, None, &mut diags).expect("load");
    let checked = check_program(Some(&sources), mods, &mut diags).expect("check");
    let opts = LowerOptions {
        arena_limit: 128,
        ..LowerOptions::default()
    };
    let lowered = lower_with_options(&checked, Some(&sources), &opts, &mut diags);
    assert!(lowered.is_none());
    assert_eq!(diags.items()[0].code, ErrorCode::LowerNoMemory);
    assert_eq!(
        diags.items()[0].message.as_str_lossy(),
        "the lowering arena has no room"
    );
}

#[test]
fn rejects_e1152_work_limit_exhausted() {
    let src = "module Main\n\nfn main() : Int\n  42\nend\n";
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let _ = sources
        .add_bytes(b"main.mo".to_vec(), src.as_bytes().to_vec())
        .expect("add");
    let mods = load_program_sources(&mut sources, None, &mut diags).expect("load");
    let checked = check_program(Some(&sources), mods, &mut diags).expect("check");
    let opts = LowerOptions {
        work_limit: 5,
        ..LowerOptions::default()
    };
    let lowered = lower_with_options(&checked, Some(&sources), &opts, &mut diags);
    assert!(lowered.is_none());
    assert_eq!(diags.items()[0].code, ErrorCode::LowerWork);
    assert_eq!(
        diags.items()[0].message.as_str_lossy(),
        "lowering exceeds its work limit"
    );
}

#[test]
fn rejects_e1153_deep_generic_instantiation() {
    let mut src = String::from("module Main\n\nfn g0(x: T) : T\n  x\nend\n\n");
    for i in 1..=70u32 {
        src.push_str(&format!("fn g{i}(x: T) : T\n  g{}(x)\nend\n\n", i - 1));
    }
    src.push_str("fn main() : Int\n  g70(0)\nend\n");
    let (core, diags) = check_and_lower(&src, false, false);
    assert!(core.is_none());
    assert_eq!(diags.items().len(), 1);
    assert_eq!(diags.items()[0].code, ErrorCode::LowerDepth);
    assert_eq!(
        diags.items()[0].message.as_str_lossy(),
        "generic instantiation is nested too deeply"
    );
}

#[test]
fn deterministic_fuzz_smoke_never_panics() {
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let next = |s: &mut u64| -> u64 {
        *s ^= *s << 13;
        *s ^= *s >> 7;
        *s ^= *s << 17;
        *s
    };
    for case in 0..48 {
        let mut src = String::from("module Main\n\nfn main() : Int\n  var acc = 0\n");
        let n_stmts = (next(&mut state) % 12) + 1;
        for s in 0..n_stmts {
            let pick = next(&mut state) % 6;
            match pick {
                0 => src.push_str(&format!("  acc += {}\n", next(&mut state) % 7)),
                1 => src.push_str(&format!(
                    "  if acc > {}\n    acc -= 1\n  else\n    acc += 2\n  end\n",
                    s
                )),
                2 => src.push_str("  for i in 0..3\n    acc += i\n  end\n"),
                3 => src.push_str(&format!(
                    "  x_{s} = [1, 2, 3]\n  for v in x_{s}\n    acc += v\n  end\n"
                )),
                4 => src.push_str(&format!(
                    "  c_{s} = case acc\n    0: 1\n    1 | 2: 3\n    _: 4\n  end\n  acc += c_{s}\n"
                )),
                _ => src.push_str(&format!("  f_{s} = fn(k) k + 1 end\n  acc = f_{s}(acc)\n")),
            }
        }
        src.push_str("  acc % 100\nend\n");
        let (core, diags) = check_and_lower(&src, false, false);
        assert!(
            core.is_some() && !diags.has_any(),
            "fuzz case {case} failed: {:?}\n{src}",
            diags.items()
        );
    }
}
