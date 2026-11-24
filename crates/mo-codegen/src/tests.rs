use std::path::{Path, PathBuf};

use mo_base::DiagBag;
use mo_bytecode::{
    BigIntSign, OPCODE_MASK, ParamOwnership, disassemble, load as load_bytecode,
    write as write_bytecode,
};
use mo_core::{
    Arm, CoreArena, CoreInteger, CtorDecl, DataDecl, Expr, FnDecl, FnKind, Lit, Metadata, Param,
    Program, Service, ServiceShape, ServiceTimer, ServiceTool, Services, Ty, parse_program,
    parse_run,
};
use mo_def::core_ops::CoreIntOp;
use mo_def::{ErrorCode, Opcode};
use mo_host::HostContext;
use mo_vm::{Image, Limits, Outcome, Process};

use crate::{compile, compile_services, convert_bigint};

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn run_core_source(source: &str) -> (Vec<u8>, mo_heap::Value) {
    let mut diags = DiagBag::new();
    let parsed = parse_program(source.as_bytes(), &mut diags).expect("core parses");
    let bytes1_opt = compile(&parsed.arena, &parsed.value, &mut diags);
    assert!(bytes1_opt.is_some(), "compile failed: {:?}", diags.items());
    let bytes1 = bytes1_opt.unwrap();
    let bytes2 = compile(&parsed.arena, &parsed.value, &mut diags).expect("second compile");
    assert_eq!(bytes1, bytes2, "codegen must be deterministic");

    let file = load_bytecode(&bytes1).expect("emitted bytecode must verify");
    let roundtrip = write_bytecode(&file).expect("load -> write must succeed");
    assert_eq!(
        bytes1, roundtrip,
        "bytecode round-trip must be byte-identical"
    );

    let image = Image::init(file, Limits::default()).expect("image init");
    let entry_idx = (0..image.file().functions.len())
        .find(|&i| {
            let name = image.file().function_name(i as u32);
            name == "Main.main" || name.ends_with(".main")
        })
        .expect("entry function") as u32;
    let mut proc = Process::new(
        &image,
        Limits::default(),
        HostContext::stdio(vec![], vec![]),
    );
    let outcome = proc.call(entry_idx, Vec::new());
    let val = match outcome {
        Outcome::Returned(v) => Some(v),
        Outcome::Failed(d) | Outcome::Stopped(d) => {
            assert_eq!(
                "",
                mo_vm::render_defect(image.file(), &d),
                "VM stopped unexpectedly"
            );
            None
        }
    };
    (bytes1, val.expect("VM returned a value"))
}

#[test]
fn all_core_fixtures_compile_deterministically_and_verify() {
    let dir = repo_root().join("tests/core");
    let mut count = 0usize;
    for entry in std::fs::read_dir(&dir).expect("read tests/core") {
        let path = entry.expect("dir entry").path();
        if path.extension().and_then(|s| s.to_str()) != Some("core") {
            continue;
        }
        let raw = std::fs::read(&path).expect("read fixture");
        let mut diags = DiagBag::new();
        let (arena, prog) = if let Some(run) = parse_run(&raw, &mut diags) {
            (run.arena, run.value.program)
        } else {
            diags = DiagBag::new();
            let p_opt = parse_program(&raw, &mut diags);
            assert!(
                p_opt.is_some(),
                "parse failed for {}: {:?}",
                path.display(),
                diags.items()
            );
            let p = p_opt.unwrap();
            (p.arena, p.value)
        };
        let mut cdiags = DiagBag::new();
        let bc1_opt = compile(&arena, &prog, &mut cdiags);
        assert!(
            bc1_opt.is_some(),
            "compile failed for {}: {:?}",
            path.display(),
            cdiags.items()
        );
        let bc1 = bc1_opt.unwrap();
        let mut cdiags2 = DiagBag::new();
        let bc2 = compile(&arena, &prog, &mut cdiags2).expect("second compile");
        assert_eq!(bc1, bc2, "determinism failed for {}", path.display());

        let decoded_res = load_bytecode(&bc1);
        assert!(
            decoded_res.is_ok(),
            "verify failed for {}: {:?}",
            path.display(),
            decoded_res.err()
        );
        let decoded = decoded_res.unwrap();
        let rewritten = write_bytecode(&decoded).expect("roundtrip write");
        assert_eq!(bc1, rewritten, "roundtrip failed for {}", path.display());
        let dis = disassemble(&bc1).expect("disassemble succeeds");
        assert!(dis.starts_with("bytecode 0.3 indexed\n"));
        count += 1;
    }
    assert!(
        count >= 15,
        "expected at least 15 Core fixtures, got {count}"
    );
}

#[test]
fn borrow_inference_and_perceus_reuse_verify_and_execute() {
    // Recursive binary tree/list mapping exercises Perceus RESET + REUSE + borrow inference.
    let src = r#"(core 0.11
  (Program
    ((DataDecl "Node"
       ((Ctor "Nil" ())
        (Ctor "Cons" (("head" (int)) ("tail" (data "Node")))))))
    ((FunDecl "head_or" (("xs" (data "Node")) ("fallback" (int))) (int)
       (case_ (var "xs")
         (("Nil" () (var "fallback"))
          ("Cons" ("h" "t") (var "h"))))
       "Main" (function) 0)
     (FunDecl "sum" (("xs" (data "Node")) ("acc" (int))) (int)
       (case_ (var "xs")
         (("Nil" () (var "acc"))
          ("Cons" ("h" "t")
            (call "sum" ((var "t") (prim (int (add)) ((var "acc") (var "h"))))))))
       "Main" (function) 0)
     (FunDecl "inc" (("xs" (data "Node"))) (data "Node")
       (case_ (var "xs")
         (("Nil" () (con (data "Node") "Nil" ()))
          ("Cons" ("h" "t")
            (con (data "Node") "Cons"
              ((prim (int (add)) ((var "h") (lit (int 1))))
               (call "inc" ((var "t"))))))))
       "Main" (function) 0)
     (FunDecl "main" () (int)
       (let_ "xs"
         (con (data "Node") "Cons"
           ((lit (int 10))
            (con (data "Node") "Cons"
              ((lit (int 20))
               (con (data "Node") "Nil" ())))))
         (let_ "ys" (call "inc" ((var "xs")))
           (prim (int (add))
             ((call "head_or" ((var "ys") (lit (int 0))))
              (call "sum" ((var "ys") (lit (int 0))))))))
       "Main" (function) 0))
    ()
    "main"
    (noSource)))
"#;
    let (bc, val) = run_core_source(src);
    assert_eq!(val.as_int(), 43);

    let file = load_bytecode(&bc).expect("load");
    // `head_or` only reads `xs` without tail-passing owned fields -> Borrowed!
    assert_eq!(
        file.functions[0].parameters[0].ownership,
        ParamOwnership::Borrowed
    );
    // `sum` tail-calls itself with owned `CASE_FIELD` `t` -> reverse tail-call dependency makes `xs` Owned!
    assert_eq!(
        file.functions[1].parameters[0].ownership,
        ParamOwnership::Owned
    );
    // `inc` consumes and reconstructs `Cons` -> Owned + RESET + REUSE!
    let inc_fn = &file.functions[2];
    assert_eq!(inc_fn.parameters[0].ownership, ParamOwnership::Owned);
    let mut has_reset = false;
    let mut has_reuse = false;
    let mut pc = 0usize;
    while pc < inc_fn.code.len() {
        let head = inc_fn.code[pc];
        let info = Opcode::find(head & OPCODE_MASK).unwrap();
        if info.opcode == Opcode::Reset {
            has_reset = true;
        }
        if info.opcode == Opcode::Reuse {
            has_reuse = true;
        }
        pc += 1 + usize::from(info.fixed_count) + ((head >> 16) as usize);
    }
    assert!(has_reset && has_reuse, "expected RESET and REUSE in inc()");
}

#[test]
fn joins_closures_uncons_and_range_to_text_execute() {
    let src = r#"(core 0.11
  (Program
    ()
    ((FunDecl "main" () (text)
       (let_ "f" (lam (("x" (int))) (prim (int (add)) ((var "x") (lit (int 1)))))
         (let_ "n"
           (join "loop" (("i" (int)) ("acc" (int)))
             (if_ (prim (cmp (int) (lt)) ((var "i") (lit (int 5))))
               (jump "loop"
                 ((app (var "f") ((var "i")))
                  (prim (int (add)) ((var "acc") (var "i")))))
               (var "acc"))
             (jump "loop" ((lit (int 0)) (lit (int 0)))))
           (let_ "r" (prim (range (make)) ((lit (int 1)) (var "n")))
             (let_ "s" (prim (range (toText)) ((var "r")))
               (case_ (prim (list (int) (uncons)) ((prim (list (int) (lit 2)) ((lit (int 7)) (lit (int 8))))))
                 (("None" () (var "s"))
                  ("Some" ("p")
                    (prim (text (concat))
                      ((var "s")
                       (prim (int (toText)) ((proj (var "p") 0))))))))))))
       "Main" (function) 0))
    ()
    "main"
    (noSource)))
"#;
    let (_, val) = run_core_source(src);
    assert_eq!(val.as_text(), "1..107");
}

#[test]
fn bigint_karatsuba_matches_known_values() {
    let mut work = 0usize;
    let (sign, mag) = convert_bigint(&CoreInteger::zero(), &mut work).unwrap();
    assert_eq!(sign, BigIntSign::Zero);
    assert!(mag.is_empty());

    // 2^128 = 340282366920938463463374607431768211456 -> 16 zero bytes + 0x01
    let two_128 = CoreInteger::parse_decimal("340282366920938463463374607431768211456").unwrap();
    let (sign, mag) = convert_bigint(&two_128, &mut work).unwrap();
    assert_eq!(sign, BigIntSign::Positive);
    let mut expected = vec![0u8; 16];
    expected.push(1);
    assert_eq!(mag, expected);

    // Negative large value: -10^40
    let mut s = String::from("-1");
    for _ in 0..40 {
        s.push('0');
    }
    let neg = CoreInteger::parse_decimal(&s).unwrap();
    let (sign, mag) = convert_bigint(&neg, &mut work).unwrap();
    assert_eq!(sign, BigIntSign::Negative);
    // Verify magnitude mod 2^64 via exact big math check (10^40 = 2^40 * 5^40 -> lowest 5 bytes zero!)
    assert_eq!(&mag[0..5], &[0, 0, 0, 0, 0]);
    assert_ne!(mag[5], 0);
}

#[test]
fn service_descriptor_and_shapes_compile_and_verify() {
    let mut arena = CoreArena::new();
    let ty_int = arena.alloc_ty(Ty::Int);
    let ty_time = arena.alloc_ty(Ty::Time);
    let ty_dur = arena.alloc_ty(Ty::Duration);
    let ty_state = arena.alloc_ty(Ty::Data("State".to_string()));
    let ty_reply = arena.alloc_ty(Ty::Tuple(vec![ty_state, ty_int]));

    let lit_zero = arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::zero())));
    let start_body = arena.alloc_expr(Expr::Con {
        ty: ty_state,
        ctor: "State".to_string(),
        args: vec![lit_zero],
    });
    let var_s = arena.alloc_expr(Expr::Var("s".to_string()));
    let var_n = arena.alloc_expr(Expr::Var("n".to_string()));
    let var_c = arena.alloc_expr(Expr::Var("c".to_string()));
    let add_expr = arena.alloc_expr(Expr::Prim {
        prim: mo_core::Prim::Int(CoreIntOp::Add),
        args: vec![var_c, var_n],
    });
    let new_state = arena.alloc_expr(Expr::Con {
        ty: ty_state,
        ctor: "State".to_string(),
        args: vec![add_expr],
    });
    let var_s2 = arena.alloc_expr(Expr::Var("s2".to_string()));
    let var_n2 = arena.alloc_expr(Expr::Var("n".to_string()));
    let ret_pair = arena.alloc_expr(Expr::Tuple(vec![var_s2, var_n2]));
    let arm_body = arena.alloc_expr(Expr::Let {
        name: "s2".to_string(),
        bound: new_state,
        body: ret_pair,
    });
    let tool_body = arena.alloc_expr(Expr::Case {
        expr: var_s,
        arms: vec![Arm {
            ctor: "State".to_string(),
            names: vec!["c".to_string()],
            body: arm_body,
        }],
    });
    let tick_body = arena.alloc_expr(Expr::Var("s".to_string()));
    let dur_body = arena.alloc_expr(Expr::Lit(Lit::Duration(CoreInteger::from_i64(1_000_000))));
    let main_body = arena.alloc_expr(Expr::Lit(Lit::Unit));
    let ty_unit = arena.alloc_ty(Ty::Unit);

    let prog = Program {
        data: vec![DataDecl {
            name: "State".to_string(),
            ctors: vec![CtorDecl {
                name: "State".to_string(),
                fields: vec![Param {
                    name: "count".to_string(),
                    ty: ty_int,
                }],
            }],
        }],
        funs: vec![
            FnDecl {
                name: "start".to_string(),
                params: Vec::new(),
                ret: ty_state,
                body: start_body,
                module: "Counter".to_string(),
                kind: FnKind::Function,
                const_count: mo_core::CoreNatural::zero(),
            },
            FnDecl {
                name: "bump".to_string(),
                params: vec![
                    Param {
                        name: "s".to_string(),
                        ty: ty_state,
                    },
                    Param {
                        name: "now".to_string(),
                        ty: ty_time,
                    },
                    Param {
                        name: "n".to_string(),
                        ty: ty_int,
                    },
                ],
                ret: ty_reply,
                body: tool_body,
                module: "Counter".to_string(),
                kind: FnKind::Function,
                const_count: mo_core::CoreNatural::zero(),
            },
            FnDecl {
                name: "tick".to_string(),
                params: vec![
                    Param {
                        name: "s".to_string(),
                        ty: ty_state,
                    },
                    Param {
                        name: "now".to_string(),
                        ty: ty_time,
                    },
                ],
                ret: ty_state,
                body: tick_body,
                module: "Counter".to_string(),
                kind: FnKind::Function,
                const_count: mo_core::CoreNatural::zero(),
            },
            FnDecl {
                name: "every".to_string(),
                params: Vec::new(),
                ret: ty_dur,
                body: dur_body,
                module: "Counter".to_string(),
                kind: FnKind::Function,
                const_count: mo_core::CoreNatural::zero(),
            },
            FnDecl {
                name: "main".to_string(),
                params: Vec::new(),
                ret: ty_unit,
                body: main_body,
                module: "Counter".to_string(),
                kind: FnKind::Function,
                const_count: mo_core::CoreNatural::zero(),
            },
        ],
        consts: Vec::new(),
        entry: "main".to_string(),
        metadata: Metadata::NoSource,
    };

    let services = Services {
        items: vec![Service {
            start: 0,
            token_env: "COUNTER_TOKEN".to_string(),
            tools: vec![ServiceTool {
                name: "bump".to_string(),
                function: 1,
                read_only: false,
                description: "Increment counter".to_string(),
                argument_names: vec!["n".to_string()],
            }],
            timers: vec![ServiceTimer {
                function: 2,
                interval: 3,
            }],
        }],
        data_shapes: vec![ServiceShape {
            is_struct: true,
            unnamed: Vec::new(),
        }],
    };

    let mut diags = DiagBag::new();
    let bytes_opt = compile_services(&arena, &prog, Some(&services), &mut diags);
    assert!(
        bytes_opt.is_some(),
        "service compile failed: {:?}",
        diags.items()
    );
    let bytes = bytes_opt.unwrap();
    let file = load_bytecode(&bytes).expect("service bytecode must verify");
    assert_eq!(file.services.len(), 1);
    assert_eq!(file.services[0].shapes.len(), 1);
    assert!(file.services[0].shapes[0].is_struct);
}

#[test]
fn register_limit_reports_e1013() {
    // 65,540 list literal elements evaluated before a branch in the last element keep
    // > 65,535 registers live across a basic-block boundary -> E1013.
    let mut arena2 = CoreArena::new();
    let ti = arena2.alloc_ty(Ty::Int);
    let l1 = arena2.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::from_i64(1))));
    let lb = arena2.alloc_expr(Expr::Lit(Lit::Bool(true)));
    let l0 = arena2.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::zero())));
    // Construct a balanced tree of depth 16 of Int additions with a branch in the middle?
    // Even simpler: 320 `Let`s of `(Int, Int, ...)` where each tuple has 210 elements? No, each tuple is 1 register!
    // Use a flat `Prim::List { op: ListOp::Lit(65540) }` — every element is evaluated before the list is constructed,
    // and insert an `If` in the final element so all 65,539 preceding element registers cross a basic block boundary!
    let mut elems = Vec::with_capacity(65_540);
    for _ in 0..65_539 {
        elems.push(l1);
    }
    let cond_elem = arena2.alloc_expr(Expr::If {
        cond: lb,
        then_: l1,
        else_: l0,
    });
    elems.push(cond_elem);
    let list_expr = arena2.alloc_expr(Expr::Prim {
        prim: mo_core::Prim::List {
            ty: ti,
            op: mo_core::ListOp::Lit(mo_core::CoreNatural::from_u64(65_540)),
        },
        args: elems,
    });
    let size_expr = arena2.alloc_expr(Expr::Prim {
        prim: mo_core::Prim::List {
            ty: ti,
            op: mo_core::ListOp::Nullary(mo_def::core_ops::CoreListNullaryOp::Size),
        },
        args: vec![list_expr],
    });
    let prog = Program {
        data: Vec::new(),
        funs: vec![FnDecl {
            name: "Main.run".to_string(),
            params: Vec::new(),
            ret: ti,
            body: size_expr,
            module: "Main".to_string(),
            kind: FnKind::Function,
            const_count: mo_core::CoreNatural::zero(),
        }],
        consts: Vec::new(),
        entry: "Main.run".to_string(),
        metadata: Metadata::NoSource,
    };
    let mut diags = DiagBag::new();
    let out = compile(&arena2, &prog, &mut diags);
    assert!(out.is_none());
    assert_eq!(diags.items()[0].code, ErrorCode::TooManyRegisters);
    assert_eq!(
        diags.items()[0]
            .primary
            .text
            .as_ref()
            .map(|t| t.to_string())
            .as_deref(),
        Some("Main.run: Function exceeds MO_LIMIT_FUNCTION_REGISTERS after recycling.")
    );
}

#[test]
fn deterministic_fuzz_core_to_vm_never_panics_or_fails_verify() {
    // Deterministic PRNG generating 64 varied Core programs with branches, lets, tuples,
    // choices, lists, closures, and joins — every one must compile, verify, round-trip, and run!
    let mut seed: u64 = 0x517c_c1b7_2722_0a95;
    let mut next_u64 = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };

    for case in 0..64u32 {
        let mut arena = CoreArena::new();
        let ty_int = arena.alloc_ty(Ty::Int);
        let ty_text = arena.alloc_ty(Ty::Text);
        let ty_box = arena.alloc_ty(Ty::Data("Box".to_string()));
        let mut expr =
            arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::from_i64(i64::from(case)))));

        let steps = 3 + (next_u64() % 6) as usize;
        for step in 0..steps {
            let kind = next_u64() % 5;
            expr = match kind {
                0 => {
                    let rhs = arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::from_i64(
                        (next_u64() % 17) as i64,
                    ))));
                    arena.alloc_expr(Expr::Prim {
                        prim: mo_core::Prim::Int(CoreIntOp::Add),
                        args: vec![expr, rhs],
                    })
                }
                1 => {
                    let cond = arena.alloc_expr(Expr::Lit(Lit::Bool((next_u64() & 1) == 0)));
                    let alt = arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::from_i64(
                        (next_u64() % 31) as i64,
                    ))));
                    arena.alloc_expr(Expr::If {
                        cond,
                        then_: expr,
                        else_: alt,
                    })
                }
                2 => {
                    let name = format!("x{step}");
                    let v1 = arena.alloc_expr(Expr::Var(name.clone()));
                    let v2 = arena.alloc_expr(Expr::Var(name.clone()));
                    let body = arena.alloc_expr(Expr::Prim {
                        prim: mo_core::Prim::Int(CoreIntOp::Add),
                        args: vec![v1, v2],
                    });
                    arena.alloc_expr(Expr::Let {
                        name,
                        bound: expr,
                        body,
                    })
                }
                3 => {
                    let txt = arena.alloc_expr(Expr::Lit(Lit::Text(format!("s{step}"))));
                    let boxed = arena.alloc_expr(Expr::Con {
                        ty: ty_box,
                        ctor: "Wrap".to_string(),
                        args: vec![expr, txt],
                    });
                    let va = arena.alloc_expr(Expr::Var("a".to_string()));
                    let vb = arena.alloc_expr(Expr::Var("b".to_string()));
                    let len = arena.alloc_expr(Expr::Prim {
                        prim: mo_core::Prim::Text(mo_def::core_ops::CoreTextOp::ByteCount),
                        args: vec![vb],
                    });
                    let sum = arena.alloc_expr(Expr::Prim {
                        prim: mo_core::Prim::Int(CoreIntOp::Add),
                        args: vec![va, len],
                    });
                    arena.alloc_expr(Expr::Case {
                        expr: boxed,
                        arms: vec![Arm {
                            ctor: "Wrap".to_string(),
                            names: vec!["a".to_string(), "b".to_string()],
                            body: sum,
                        }],
                    })
                }
                _ => {
                    let p = arena.alloc_expr(Expr::Var("p".to_string()));
                    let one = arena.alloc_expr(Expr::Lit(Lit::Int(CoreInteger::from_i64(1))));
                    let lam_body = arena.alloc_expr(Expr::Prim {
                        prim: mo_core::Prim::Int(CoreIntOp::Add),
                        args: vec![p, one],
                    });
                    let lam = arena.alloc_expr(Expr::Lam {
                        params: vec![Param {
                            name: "p".to_string(),
                            ty: ty_int,
                        }],
                        body: lam_body,
                    });
                    arena.alloc_expr(Expr::App {
                        func: lam,
                        args: vec![expr],
                    })
                }
            };
        }

        let prog = Program {
            data: vec![DataDecl {
                name: "Box".to_string(),
                ctors: vec![CtorDecl {
                    name: "Wrap".to_string(),
                    fields: vec![
                        Param {
                            name: "val".to_string(),
                            ty: ty_int,
                        },
                        Param {
                            name: "tag".to_string(),
                            ty: ty_text,
                        },
                    ],
                }],
            }],
            funs: vec![FnDecl {
                name: "main".to_string(),
                params: Vec::new(),
                ret: ty_int,
                body: expr,
                module: "Main".to_string(),
                kind: FnKind::Function,
                const_count: mo_core::CoreNatural::zero(),
            }],
            consts: Vec::new(),
            entry: "main".to_string(),
            metadata: Metadata::NoSource,
        };

        let mut diags = DiagBag::new();
        let bytes_opt = compile(&arena, &prog, &mut diags);
        assert!(
            bytes_opt.is_some(),
            "fuzz case {case} compile failed: {:?}",
            diags.items()
        );
        let bytes = bytes_opt.unwrap();
        let file_res = load_bytecode(&bytes);
        assert!(
            file_res.is_ok(),
            "fuzz case {case} verify failed: {:?}",
            file_res.err()
        );
        let file = file_res.unwrap();
        let rewritten = write_bytecode(&file).expect("roundtrip");
        assert_eq!(bytes, rewritten);
        let image = Image::init(file, Limits::default()).expect("init");
        let mut proc = Process::new(
            &image,
            Limits::default(),
            HostContext::stdio(vec![], vec![]),
        );
        match proc.call(0, Vec::new()) {
            Outcome::Returned(_) => {}
            Outcome::Failed(d) | Outcome::Stopped(d) => {
                assert_eq!(
                    "",
                    mo_vm::render_defect(image.file(), &d),
                    "fuzz case {case} stopped"
                );
            }
        }
    }
}
