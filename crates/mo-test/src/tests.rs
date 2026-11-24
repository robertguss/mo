//! Unit tests for `crates/mo-test` (`deadline`, `inputs`, `mutants`, `gates`).

use mo_base::{DiagBag, Diagnostic, Span};
use mo_bytecode::types::TypeTable;
use mo_bytecode::{TypeDef, TypeKind};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_GATE_STEP_FLOOR, MO_LIMIT_GATE_STEP_MULTIPLE};
use mo_heap::Heap;

use crate::deadline::{ClockMode, Deadline, DeadlineState, set_thread_clock_mode};
use crate::gates::{gate_limit, gate_start_failed};
use crate::inputs::{InputKind, InputLimits, InputShrinker, Inputs};

#[test]
fn deadline_modes_and_none() {
    let mut none = Deadline::none();
    assert!(none.running());
    assert_eq!(none.state, DeadlineState::Running);
    assert_eq!(none.microseconds(), 0);

    set_thread_clock_mode(ClockMode::StartFailure);
    assert!(Deadline::start(1).is_err());

    set_thread_clock_mode(ClockMode::Fixed);
    let mut fixed = Deadline::start(1).unwrap_or_else(|_| Deadline::none());
    assert!(fixed.running());
    assert_eq!(fixed.microseconds(), 0);

    set_thread_clock_mode(ClockMode::Expiry);
    let mut exp = Deadline::start(1).unwrap_or_else(|_| Deadline::none());
    let mut ticks = 0u32;
    while exp.running() {
        ticks += 1;
        assert!(ticks < 2000);
    }
    assert_eq!(exp.state, DeadlineState::Expired);
    assert_eq!(exp.microseconds(), 1_000_000);

    set_thread_clock_mode(ClockMode::Failure);
    let mut fail = Deadline::start(10).unwrap_or_else(|_| Deadline::none());
    while fail.running() {}
    assert_eq!(fail.state, DeadlineState::Error);
    assert_eq!(fail.error, 5);

    set_thread_clock_mode(ClockMode::System);
}

#[test]
fn inputs_int_boundaries_determinism_and_shrinking() {
    let types = TypeTable::from_raw(
        vec![TypeDef {
            kind: TypeKind::Int,
            parameters: Vec::new(),
            result: mo_bytecode::NO_INDEX,
            function: mo_bytecode::NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        }],
        Vec::new(),
    );
    let heap = Heap::new(1024 * 1024);
    let mut gen1 = Inputs::init(
        &types,
        &[0],
        b"Main.f.promise.1",
        InputLimits::default(),
        &[],
    )
    .ok();
    let mut gen2 = Inputs::init(
        &types,
        &[0],
        b"Main.f.promise.1",
        InputLimits::default(),
        &[],
    )
    .ok();
    assert!(gen1.is_some() && gen2.is_some());
    if let (Some(g1), Some(g2)) = (gen1.as_mut(), gen2.as_mut()) {
        assert!(g1.boundaries() >= 5);
        for i in 0..12u64 {
            let a = g1.next(&heap, &mut []).ok();
            let b = g2.next(&heap, &mut []).ok();
            assert_eq!(
                a.as_ref().map(|(v, k)| (v[0].as_int(), *k)),
                b.as_ref().map(|(v, k)| (v[0].as_int(), *k))
            );
            if i == 0 {
                assert_eq!(
                    a.as_ref().map(|(v, k)| (v[0].as_int(), *k)),
                    Some((0, InputKind::Boundary))
                );
            }
        }
        let mut shrinker = InputShrinker::new(64);
        let mut best = vec![mo_heap::Value::Int(100)];
        while let Ok(cand) = shrinker.next(g1, &best, &heap, &mut []) {
            let fails = cand[0].as_int() >= 10;
            shrinker.feedback(fails);
            if fails {
                best = cand;
            }
        }
        assert_eq!(best[0].as_int(), 10);
    }
}

#[test]
fn gate_limit_and_start_failed() {
    assert_eq!(gate_limit(0), MO_LIMIT_GATE_STEP_FLOOR);
    assert_eq!(gate_limit(1), MO_LIMIT_GATE_STEP_FLOOR);
    let big = MO_LIMIT_GATE_STEP_FLOOR;
    assert_eq!(gate_limit(big), big * MO_LIMIT_GATE_STEP_MULTIPLE);
    assert_eq!(gate_limit(u64::MAX), u64::MAX);

    let mut diags = DiagBag::new();
    assert!(!gate_start_failed(&diags));
    let _ = diags.push(Diagnostic::new(ErrorCode::ToolAnchor, Span::NONE, "anchor"));
    assert!(gate_start_failed(&diags));
    let _ = diags.push(Diagnostic::new(
        ErrorCode::ToolTestInitialization,
        Span::NONE,
        "init",
    ));
    assert!(gate_start_failed(&diags));
    let _ = diags.push(Diagnostic::new(
        ErrorCode::ToolPipeline,
        Span::NONE,
        "other",
    ));
    assert!(!gate_start_failed(&diags));
}
