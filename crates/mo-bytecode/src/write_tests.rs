use mo_def::Opcode;

use crate::types::TypeTable;
use crate::{
    AnchorExpectation, BYTECODE_FLAGS_NONE, BYTECODE_HEADER_SIZE, BYTECODE_MAGIC, BYTECODE_MAJOR,
    BYTECODE_MINOR, BigIntSign, BytecodeFile, Capability, CodeSpan, ConstantDef, ConstantValue,
    DebugEntry, ErrorKind, FunctionDef, FunctionKind, GlobalDef, Header, LayoutDef, LayoutKind,
    ModuleDef, NO_INDEX, OPCODE_VARIABLE_SHIFT, ParamOwnership, ParameterDef, RegisterName,
    ServiceDef, ServiceShape, ServiceTimer, ServiceTool, SpecLine, TypeCase, TypeDef, TypeExport,
    TypeField, TypeKind, disassemble, encode, load, write,
};

fn empty_file() -> BytecodeFile {
    BytecodeFile {
        header: Header {
            magic: BYTECODE_MAGIC,
            major: BYTECODE_MAJOR,
            minor: BYTECODE_MINOR,
            flags: BYTECODE_FLAGS_NONE,
            checksum: 0,
        },
        program_checksum: 0,
        strings: Vec::new(),
        types: TypeTable::from_raw(Vec::new(), Vec::new()),
        constants: Vec::new(),
        globals: Vec::new(),
        init: Vec::new(),
        functions: Vec::new(),
        code_offsets: Vec::new(),
        modules: Vec::new(),
        files: Vec::new(),
        debug: Vec::new(),
        services: Vec::new(),
    }
}

fn bare_type(kind: TypeKind) -> TypeDef {
    TypeDef {
        kind,
        parameters: Vec::new(),
        result: 0,
        function: 0,
        capability: None,
        name_idx: None,
        name: None,
        fields: Vec::new(),
        cases: Vec::new(),
        is_reference: false,
        has_fields: false,
    }
}

fn empty_debug() -> DebugEntry {
    DebugEntry {
        spans: Vec::new(),
        names: Vec::new(),
        text: NO_INDEX,
        file: NO_INDEX,
        start: 0,
        end: 0,
        subject: NO_INDEX,
        expectation: AnchorExpectation::NotAnchor,
        lines: Vec::new(),
    }
}

fn op_head(op: Opcode, var_count: u32) -> u32 {
    u32::from(op.number()) | (var_count << OPCODE_VARIABLE_SHIFT)
}

#[test]
fn empty_file_encodes_60_bytes_and_disassembles() {
    let file = empty_file();
    let bytes = write(&file).expect("empty file writes");
    assert_eq!(bytes.len(), BYTECODE_HEADER_SIZE + 10 * 4);

    let decoded = load(&bytes).expect("empty file loads");
    let rewritten = write(&decoded).expect("empty file rewrites");
    assert_eq!(bytes, rewritten);

    let text = disassemble(&bytes).expect("empty file disassembles");
    assert_eq!(
        text,
        "bytecode 0.3 indexed\n\
         strings\nend\n\
         types\nend\n\
         layouts\nend\n\
         constants\nend\n\
         globals\nend\n\
         init\nend\n\
         functions\nend\n\
         modules\nend\n\
         files\nend\n\
         debug\nend\n"
    );
}

#[test]
fn rich_module_round_trips_byte_for_byte_and_disassembles() {
    let mut f = empty_file();
    f.strings = vec![
        "Main".to_string(),               // s0
        "Main.main".to_string(),          // s1
        "Main.closure".to_string(),       // s2
        "Main.g".to_string(),             // s3
        "Main.g.init".to_string(),        // s4
        "Pair".to_string(),               // s5
        "first".to_string(),              // s6
        "second".to_string(),             // s7
        "Choice".to_string(),             // s8
        "None".to_string(),               // s9
        "Some".to_string(),               // s10
        "hi \"\\ \n\u{00e9}".to_string(), // s11 (exercises quote/backslash/ctrl/UTF-8 escaping)
        "Pair".to_string(), // s12 (duplicate of s5 — writer must pick lowest index s5)
    ];

    let mut rec = bare_type(TypeKind::Record);
    rec.name = Some("Pair".to_string());
    rec.fields = vec![
        TypeField {
            name_idx: None,
            name: Some("first".to_string()),
            ty: 3, // int
        },
        TypeField {
            name_idx: None,
            name: Some("second".to_string()),
            ty: 10, // text
        },
    ];

    let mut choice = bare_type(TypeKind::Choice);
    choice.name = Some("Choice".to_string());
    choice.cases = vec![
        TypeCase {
            name_idx: 9,
            name: "None".to_string(),
            fields: Vec::new(),
            unnamed: false,
        },
        TypeCase {
            name_idx: 10,
            name: "Some".to_string(),
            fields: vec![TypeField {
                name_idx: None,
                name: None,
                ty: 8, // decimal -> Boxed case
            }],
            unnamed: true,
        },
    ];

    let mut fn_ty = bare_type(TypeKind::Function);
    fn_ty.parameters = vec![3];
    fn_ty.result = 3;

    let mut env_ty = bare_type(TypeKind::Env);
    env_ty.function = 2;

    let mut cap_ty = bare_type(TypeKind::Capability);
    cap_ty.capability = Some(Capability::Console);

    let mut list_ty = bare_type(TypeKind::List);
    list_ty.parameters = vec![3];

    let mut map_ty = bare_type(TypeKind::Map);
    map_ty.parameters = vec![10, 3];

    let types = vec![
        bare_type(TypeKind::Unit),     // t0
        bare_type(TypeKind::Bool),     // t1
        bare_type(TypeKind::Byte),     // t2
        bare_type(TypeKind::Int),      // t3
        bare_type(TypeKind::Float),    // t4
        bare_type(TypeKind::Char),     // t5
        bare_type(TypeKind::Time),     // t6
        bare_type(TypeKind::Duration), // t7
        bare_type(TypeKind::Decimal),  // t8
        bare_type(TypeKind::Bytes),    // t9
        bare_type(TypeKind::Text),     // t10
        bare_type(TypeKind::BigInt),   // t11
        rec,                           // t12
        choice,                        // t13
        fn_ty,                         // t14
        env_ty,                        // t15
        cap_ty,                        // t16
        list_ty,                       // t17
        map_ty,                        // t18
    ];

    let layouts = vec![
        LayoutDef {
            kind: LayoutKind::Record,
            ty: 12,
            case_index: NO_INDEX,
            function: NO_INDEX,
            fields: vec![3, 10],
        },
        LayoutDef {
            kind: LayoutKind::Case,
            ty: 13,
            case_index: 1,
            function: NO_INDEX,
            fields: vec![8],
        },
        LayoutDef {
            kind: LayoutKind::Closure,
            ty: 14,
            case_index: NO_INDEX,
            function: 2,
            fields: Vec::new(),
        },
    ];
    f.types = TypeTable::from_raw(types, layouts);

    let mut char_buf = [0u8; 8];
    char_buf[..4].copy_from_slice(&0x1f600u32.to_le_bytes());

    f.constants = vec![
        ConstantDef {
            ty: 0,
            value: ConstantValue::Immediate64([0u8; 8]),
        },
        ConstantDef {
            ty: 1,
            value: ConstantValue::Immediate64(1u64.to_le_bytes()),
        },
        ConstantDef {
            ty: 2,
            value: ConstantValue::Immediate64(255u64.to_le_bytes()),
        },
        ConstantDef {
            ty: 3,
            value: ConstantValue::Immediate64((-42i64).to_le_bytes()),
        },
        ConstantDef {
            ty: 4,
            value: ConstantValue::Immediate64(1.5f64.to_bits().to_le_bytes()),
        },
        ConstantDef {
            ty: 5,
            value: ConstantValue::Immediate64(char_buf),
        },
        ConstantDef {
            ty: 6,
            value: ConstantValue::Immediate64((-1_000_000i64).to_le_bytes()),
        },
        ConstantDef {
            ty: 7,
            value: ConstantValue::Immediate64(500i64.to_le_bytes()),
        },
        ConstantDef {
            ty: 8,
            value: ConstantValue::Decimal128((-1_500_000_000_000_000_000i128).to_le_bytes()),
        },
        ConstantDef {
            ty: 8,
            value: ConstantValue::Decimal128(1i128.to_le_bytes()),
        },
        ConstantDef {
            ty: 8,
            value: ConstantValue::Decimal128(0i128.to_le_bytes()),
        },
        ConstantDef {
            ty: 10,
            value: ConstantValue::Text(11),
        },
        ConstantDef {
            ty: 9,
            value: ConstantValue::Bytes(vec![0xde, 0xad, 0xbe, 0xef]),
        },
        ConstantDef {
            ty: 11,
            value: ConstantValue::BigInt {
                sign: BigIntSign::Zero,
                magnitude: Vec::new(),
            },
        },
        ConstantDef {
            ty: 11,
            value: ConstantValue::BigInt {
                sign: BigIntSign::Negative,
                magnitude: vec![0x01, 0x02],
            },
        },
        ConstantDef {
            ty: 14,
            value: ConstantValue::Closure(2),
        },
    ];

    f.globals = vec![GlobalDef {
        ty: 3,
        name: 3,
        module: 0,
    }];
    f.init = vec![1];

    // f0: Main.main() -> Unit
    let f0 = FunctionDef {
        name: 1,
        module: 0,
        kind: FunctionKind::Function,
        spec_code: false,
        parameters: Vec::new(),
        result_type: 0,
        capture_types: Vec::new(),
        register_types: vec![0],
        code: vec![op_head(Opcode::Const, 0), 0, 0, op_head(Opcode::Ret, 0), 0],
    };

    // f1: Main.g.init() -> Unit
    let f1 = FunctionDef {
        name: 4,
        module: 0,
        kind: FunctionKind::Init,
        spec_code: false,
        parameters: Vec::new(),
        result_type: 0,
        capture_types: Vec::new(),
        register_types: vec![3, 0],
        code: vec![
            op_head(Opcode::Const, 0),
            0,
            3,
            op_head(Opcode::SetGlobal, 0),
            0,
            0,
            op_head(Opcode::Const, 0),
            1,
            0,
            op_head(Opcode::Ret, 0),
            1,
        ],
    };

    // f2: Main.closure(x: Int) -> Int
    let f2 = FunctionDef {
        name: 2,
        module: 0,
        kind: FunctionKind::Closure,
        spec_code: false,
        parameters: vec![ParameterDef {
            ty: 3,
            ownership: ParamOwnership::Owned,
        }],
        result_type: 3,
        capture_types: Vec::new(),
        register_types: vec![3, 15],
        code: vec![op_head(Opcode::Drop, 0), 1, op_head(Opcode::Ret, 0), 0],
    };

    f.functions = vec![f0, f1, f2];
    f.modules = vec![ModuleDef {
        name: 0,
        types: vec![TypeExport {
            ty: 12,
            contents_exposed: true,
        }],
        functions: vec![0],
        globals: vec![0],
    }];
    f.files = vec![b"main.mo".to_vec()];
    let mut d0 = empty_debug();
    d0.spans.push(CodeSpan {
        code_offset: 0,
        file: 0,
        start: 0,
        end: 7,
    });
    d0.names.push(RegisterName {
        reg: 0,
        name: 6,
        code_start: 0,
        code_end: 5,
    });
    d0.file = 0;
    d0.start = 0;
    d0.end = 7;
    f.debug = vec![d0, empty_debug(), empty_debug()];

    let bytes = write(&f).expect("rich module writes and verifies");
    let loaded = load(&bytes).expect("rich module loads");
    let bytes2 = write(&loaded).expect("re-write succeeds");
    assert_eq!(
        bytes, bytes2,
        "load -> write must be byte-for-byte identical"
    );

    let text = disassemble(&bytes).expect("disassemble succeeds");
    assert!(
        text.contains(r#"  s11 "hi \"\\ \x0a\xc3\xa9""#),
        "got:\n{text}"
    );
    assert!(
        text.contains("  t12 record s5 [(s6, t3), (s7, t10)]\n"),
        "lowest string index s5 expected:\n{text}"
    );
    assert!(
        text.contains("  t13 choice s8 [(s9, []), (s10, [(-, t8)])]\n"),
        "got:\n{text}"
    );
    assert!(text.contains("  k3 t3 -42\n"), "got:\n{text}");
    assert!(text.contains("  k5 t5 U+1f600\n"), "got:\n{text}");
    assert!(text.contains("  k8 t8 -1.5\n"), "got:\n{text}");
    assert!(
        text.contains("  k9 t8 0.000000000000000001\n"),
        "got:\n{text}"
    );
    assert!(text.contains("  k10 t8 0\n"), "got:\n{text}");
    assert!(
        text.contains("  k12 t9 bytes hex\"deadbeef\"\n"),
        "got:\n{text}"
    );
    assert!(
        text.contains("  k13 t11 bigint zero hex\"\"\n"),
        "got:\n{text}"
    );
    assert!(
        text.contains("  k14 t11 bigint negative hex\"0102\"\n"),
        "got:\n{text}"
    );
    assert!(text.contains("  k15 t14 closure f2\n"), "got:\n{text}");
    assert!(text.contains("  t16 capability Console\n"), "got:\n{text}");
}

#[test]
fn services_section_and_verifier_function_site() {
    // 1. Verify WriteError reports function site when verification fails inside f0.
    let mut bad = empty_file();
    bad.strings = vec!["Main".to_string(), "Main.bad".to_string()];
    bad.types = TypeTable::from_raw(vec![bare_type(TypeKind::Unit)], Vec::new());
    bad.functions = vec![FunctionDef {
        name: 1,
        module: 0,
        kind: FunctionKind::Function,
        spec_code: false,
        parameters: Vec::new(),
        result_type: 0,
        capture_types: Vec::new(),
        register_types: vec![0],
        // Returns uninitialized register r0 — passes encode, fails verify in f0!
        code: vec![op_head(Opcode::Ret, 0), 0],
    }];
    bad.modules = vec![ModuleDef {
        name: 0,
        types: Vec::new(),
        functions: vec![0],
        globals: Vec::new(),
    }];
    bad.debug = vec![empty_debug()];

    assert!(
        encode(&bad).is_ok(),
        "structural encode accepts uninitialized ret"
    );
    let err = write(&bad).expect_err("full verify must reject uninitialized ret");
    assert_eq!(err.kind, ErrorKind::Invalid);
    assert_eq!(err.function, Some(0));

    // 2. Verify services section round-trips and disassembles.
    let mut svc = empty_file();
    svc.strings = vec![
        "Svc".to_string(),       // s0
        "Svc.start".to_string(), // s1
        "Svc.ping".to_string(),  // s2
        "Svc.tick".to_string(),  // s3
        "Svc.every".to_string(), // s4
        "SVC_TOKEN".to_string(), // s5
        "ping".to_string(),      // s6
        "Ping tool".to_string(), // s7
        "State".to_string(),     // s8
        "State".to_string(),     // s9 (duplicate -> canonical s8)
        "count".to_string(),     // s10
    ];
    let mut state_choice = bare_type(TypeKind::Choice);
    state_choice.name = Some("State".to_string());
    state_choice.cases = vec![TypeCase {
        name_idx: 8,
        name: "State".to_string(),
        fields: vec![TypeField {
            name_idx: Some(10),
            name: Some("count".to_string()),
            ty: 0, // Int
        }],
        unnamed: false,
    }];
    svc.types = TypeTable::from_raw(
        vec![
            bare_type(TypeKind::Int),      // t0
            bare_type(TypeKind::Time),     // t1
            bare_type(TypeKind::Duration), // t2
            state_choice,                  // t3
        ],
        Vec::new(),
    );
    svc.constants = vec![
        ConstantDef {
            ty: 0,
            value: ConstantValue::Immediate64(0i64.to_le_bytes()),
        },
        ConstantDef {
            ty: 2,
            value: ConstantValue::Immediate64(1_000_000i64.to_le_bytes()),
        },
    ];
    // f0: start() -> State
    let f_start = FunctionDef {
        name: 1,
        module: 0,
        kind: FunctionKind::Function,
        spec_code: false,
        parameters: Vec::new(),
        result_type: 3,
        capture_types: Vec::new(),
        register_types: vec![0, 3],
        code: vec![
            op_head(Opcode::Const, 0),
            0,
            0,
            op_head(Opcode::MakeCase, 1),
            1,
            3,
            0,
            0,
            op_head(Opcode::Ret, 0),
            1,
        ],
    };
    // f1: ping(s: State, now: Time) -> Int (read-only)
    let f_ping = FunctionDef {
        name: 2,
        module: 0,
        kind: FunctionKind::Function,
        spec_code: false,
        parameters: vec![
            ParameterDef {
                ty: 3,
                ownership: ParamOwnership::Owned,
            },
            ParameterDef {
                ty: 1,
                ownership: ParamOwnership::Owned,
            },
        ],
        result_type: 0,
        capture_types: Vec::new(),
        register_types: vec![3, 1, 0],
        code: vec![op_head(Opcode::Const, 0), 2, 0, op_head(Opcode::Ret, 0), 2],
    };
    // f2: tick(s: State, now: Time) -> State
    let f_tick = FunctionDef {
        name: 3,
        module: 0,
        kind: FunctionKind::Function,
        spec_code: false,
        parameters: vec![
            ParameterDef {
                ty: 3,
                ownership: ParamOwnership::Owned,
            },
            ParameterDef {
                ty: 1,
                ownership: ParamOwnership::Owned,
            },
        ],
        result_type: 3,
        capture_types: Vec::new(),
        register_types: vec![3, 1],
        code: vec![op_head(Opcode::Ret, 0), 0],
    };
    // f3: every() -> Duration
    let f_every = FunctionDef {
        name: 4,
        module: 0,
        kind: FunctionKind::Function,
        spec_code: false,
        parameters: Vec::new(),
        result_type: 2,
        capture_types: Vec::new(),
        register_types: vec![2],
        code: vec![op_head(Opcode::Const, 0), 0, 1, op_head(Opcode::Ret, 0), 0],
    };
    svc.functions = vec![f_start, f_ping, f_tick, f_every];
    svc.modules = vec![ModuleDef {
        name: 0,
        types: Vec::new(),
        functions: vec![0, 1, 2, 3],
        globals: Vec::new(),
    }];
    svc.debug = vec![empty_debug(), empty_debug(), empty_debug(), empty_debug()];
    svc.services = vec![ServiceDef {
        module: 0,
        state_type: 3,
        start: 0,
        token_env: 5,
        tools: vec![ServiceTool {
            name: 6,
            function: 1,
            read_only: true,
            argument_types: Vec::new(),
            result_type: 0,
            description: 7,
            argument_names: Vec::new(),
        }],
        timers: vec![ServiceTimer {
            function: 2,
            interval: 3,
        }],
        shapes: Vec::new(), // tool args/result are empty -> Int (no nominal reachable)
    }];

    let bytes = write(&svc).expect("service bytecode writes and verifies");
    let loaded = load(&bytes).expect("service bytecode loads");
    let rewritten = write(&loaded).expect("service bytecode rewrites");
    assert_eq!(bytes, rewritten);
    let text = disassemble(&bytes).expect("service bytecode disassembles");
    assert!(
        text.contains(
            "services\n  S0 m0 t3 f0 s5 tools [(s6, f1, 1, [], t0, s7, [])] timers [(f2, f3)] shapes []\nend\n"
        ),
        "got:\n{text}"
    );
    // Unused SpecLine/ServiceShape exercise
    let _ = SpecLine {
        file: NO_INDEX,
        start: 0,
        end: 0,
        text: 0,
    };
    let _ = ServiceShape {
        ty: 3,
        is_struct: true,
        unnamed: Vec::new(),
    };
}
