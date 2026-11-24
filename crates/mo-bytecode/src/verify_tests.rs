use std::fs;
use std::path::PathBuf;

use mo_def::limits::{MO_LIMIT_FUNCTION_REGISTER_BLOCKS, MO_LIMIT_VERIFY_WORK_PER_WORD};
use mo_def::{HostOpId, Opcode, PrimId, SimOpId};

use crate::{
    AnchorExpectation, BYTECODE_FLAGS_NONE, BYTECODE_HEADER_SIZE, BYTECODE_MAGIC, BYTECODE_MAJOR,
    BYTECODE_MINOR, BigIntSign, Capability, ConstantDef, ConstantValue, DebugEntry, ErrorKind,
    FNV_OFFSET, FunctionDef, FunctionKind, GlobalDef, LayoutDef, LayoutKind, LoadError, ModuleDef,
    NO_INDEX, OPCODE_VARIABLE_SHIFT, ParamOwnership, ParameterDef, TypeCase, TypeDef, TypeField,
    TypeKind, fnv1a, load, verify::visit_readable,
};

// ============================================================================
// Wire-format `.bc` v0.3 test builder
// ============================================================================

#[derive(Default)]
struct BcBuilder {
    strings: Vec<String>,
    types: Vec<TypeDef>,
    layouts: Vec<LayoutDef>,
    constants: Vec<ConstantDef>,
    globals: Vec<GlobalDef>,
    init: Vec<u32>,
    functions: Vec<FunctionDef>,
    modules: Vec<ModuleDef>,
    files: Vec<Vec<u8>>,
    debug: Vec<DebugEntry>,
}

type CaseSpec<'a> = (&'a str, Vec<(Option<&'a str>, u32)>);

impl BcBuilder {
    fn new() -> Self {
        let mut b = Self::default();
        b.add_string("Main"); // 0
        b.modules.push(ModuleDef {
            name: 0,
            types: Vec::new(),
            functions: Vec::new(),
            globals: Vec::new(),
        });
        b
    }

    fn add_string(&mut self, s: &str) -> u32 {
        if let Some(pos) = self.strings.iter().position(|x| x == s) {
            return pos as u32;
        }
        let idx = self.strings.len() as u32;
        self.strings.push(s.to_string());
        idx
    }

    fn add_prim(&mut self, kind: TypeKind) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
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
        });
        idx
    }

    fn add_cap(&mut self, cap: Capability) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Capability,
            parameters: Vec::new(),
            result: 0,
            function: 0,
            capability: Some(cap),
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_list(&mut self, elem: u32) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::List,
            parameters: vec![elem],
            result: 0,
            function: 0,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_set(&mut self, elem: u32) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Set,
            parameters: vec![elem],
            result: 0,
            function: 0,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_map(&mut self, key: u32, val: u32) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Map,
            parameters: vec![key, val],
            result: 0,
            function: 0,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_tuple(&mut self, elems: Vec<u32>) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Tuple,
            parameters: elems,
            result: 0,
            function: 0,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_fn_type(&mut self, params: Vec<u32>, result: u32) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Function,
            parameters: params,
            result,
            function: 0,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_env_type(&mut self, closure_fn: u32) -> u32 {
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Env,
            parameters: Vec::new(),
            result: 0,
            function: closure_fn,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: true,
            has_fields: false,
        });
        idx
    }

    fn add_record(&mut self, name: &str, fields: &[(&str, u32)]) -> u32 {
        let name_idx = self.add_string(name);
        let mut fdefs = Vec::with_capacity(fields.len());
        for &(fname, fty) in fields {
            let fidx = self.add_string(fname);
            fdefs.push(TypeField {
                name_idx: Some(fidx),
                name: Some(fname.to_string()),
                ty: fty,
            });
        }
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Record,
            parameters: Vec::new(),
            result: 0,
            function: 0,
            capability: None,
            name_idx: Some(name_idx),
            name: Some(name.to_string()),
            fields: fdefs,
            cases: Vec::new(),
            is_reference: true,
            has_fields: !fields.is_empty(),
        });
        idx
    }

    fn add_choice(&mut self, name: &str, cases: &[CaseSpec<'_>]) -> u32 {
        let name_idx = self.add_string(name);
        let mut cdefs = Vec::with_capacity(cases.len());
        let mut has_fields = false;
        for (cname, cfields) in cases {
            let cidx = self.add_string(cname);
            let mut fdefs = Vec::with_capacity(cfields.len());
            for &(fname, fty) in cfields {
                let (nidx, nstr) = match fname {
                    Some(s) => (Some(self.add_string(s)), Some(s.to_string())),
                    None => (None, None),
                };
                fdefs.push(TypeField {
                    name_idx: nidx,
                    name: nstr,
                    ty: fty,
                });
            }
            let unnamed = fdefs.len() == 1 && fdefs[0].name.is_none();
            has_fields |= !fdefs.is_empty();
            cdefs.push(TypeCase {
                name_idx: cidx,
                name: (*cname).to_string(),
                fields: fdefs,
                unnamed,
            });
        }
        let idx = self.types.len() as u32;
        self.types.push(TypeDef {
            kind: TypeKind::Choice,
            parameters: Vec::new(),
            result: 0,
            function: 0,
            capability: None,
            name_idx: Some(name_idx),
            name: Some(name.to_string()),
            fields: Vec::new(),
            cases: cdefs,
            is_reference: false,
            has_fields,
        });
        idx
    }

    fn add_layout(
        &mut self,
        kind: LayoutKind,
        ty: u32,
        case_index: u32,
        function: u32,
        fields: Vec<u32>,
    ) -> u32 {
        let idx = self.layouts.len() as u32;
        self.layouts.push(LayoutDef {
            kind,
            ty,
            case_index,
            function,
            fields,
        });
        idx
    }

    fn add_const(&mut self, ty: u32, value: ConstantValue) -> u32 {
        let idx = self.constants.len() as u32;
        self.constants.push(ConstantDef { ty, value });
        idx
    }

    fn add_global(&mut self, ty: u32, name: &str) -> u32 {
        let n = self.add_string(name);
        let idx = self.globals.len() as u32;
        self.globals.push(GlobalDef {
            ty,
            name: n,
            module: 0,
        });
        self.modules[0].globals.push(idx);
        idx
    }

    #[expect(
        clippy::too_many_arguments,
        reason = "test builder helper constructing synthetic bytecode functions"
    )]
    fn add_fn(
        &mut self,
        name: &str,
        kind: FunctionKind,
        spec_code: bool,
        params: Vec<(u32, ParamOwnership)>,
        result_type: u32,
        capture_types: Vec<u32>,
        register_types: Vec<u32>,
        code: Vec<u32>,
    ) -> u32 {
        let name_idx = self.add_string(name);
        let idx = self.functions.len() as u32;
        self.functions.push(FunctionDef {
            name: name_idx,
            module: 0,
            kind,
            spec_code,
            parameters: params
                .into_iter()
                .map(|(ty, ownership)| ParameterDef { ty, ownership })
                .collect(),
            result_type,
            capture_types,
            register_types,
            code,
        });
        self.debug.push(DebugEntry {
            spans: Vec::new(),
            names: Vec::new(),
            text: NO_INDEX,
            file: NO_INDEX,
            start: 0,
            end: 0,
            subject: NO_INDEX,
            expectation: AnchorExpectation::NotAnchor,
            lines: Vec::new(),
        });
        idx
    }

    fn encode(&self) -> Vec<u8> {
        let mut body = Vec::new();
        let w = |buf: &mut Vec<u8>, v: u32| buf.extend_from_slice(&v.to_le_bytes());

        // 0: STRINGS
        w(&mut body, self.strings.len() as u32);
        for s in &self.strings {
            w(&mut body, s.len() as u32);
            body.extend_from_slice(s.as_bytes());
        }

        // 1: TYPES
        w(&mut body, self.types.len() as u32);
        for t in &self.types {
            w(&mut body, t.kind as u32);
            match t.kind {
                TypeKind::Unit
                | TypeKind::Bool
                | TypeKind::Byte
                | TypeKind::Int
                | TypeKind::Float
                | TypeKind::Char
                | TypeKind::Time
                | TypeKind::Duration
                | TypeKind::Decimal
                | TypeKind::Range
                | TypeKind::Text
                | TypeKind::Bytes
                | TypeKind::BigInt => {}
                TypeKind::Tuple
                | TypeKind::List
                | TypeKind::Set
                | TypeKind::Map
                | TypeKind::Function => {
                    w(&mut body, t.parameters.len() as u32);
                    for &p in &t.parameters {
                        w(&mut body, p);
                    }
                    if t.kind == TypeKind::Function {
                        w(&mut body, t.result);
                    }
                }
                TypeKind::Record => {
                    w(&mut body, t.name_idx.unwrap());
                    w(&mut body, t.fields.len() as u32);
                    for f in &t.fields {
                        w(&mut body, f.name_idx.unwrap_or(NO_INDEX));
                        w(&mut body, f.ty);
                    }
                }
                TypeKind::Choice => {
                    w(&mut body, t.name_idx.unwrap());
                    w(&mut body, t.cases.len() as u32);
                    for c in &t.cases {
                        w(&mut body, c.name_idx);
                        w(&mut body, c.fields.len() as u32);
                        for f in &c.fields {
                            w(&mut body, f.name_idx.unwrap_or(NO_INDEX));
                            w(&mut body, f.ty);
                        }
                    }
                }
                TypeKind::Env => {
                    w(&mut body, t.function);
                }
                TypeKind::Capability => {
                    w(&mut body, t.capability.unwrap() as u32);
                }
            }
        }

        // 2: LAYOUTS
        w(&mut body, self.layouts.len() as u32);
        for l in &self.layouts {
            w(&mut body, l.kind as u32);
            w(&mut body, l.ty);
            if l.kind == LayoutKind::Case {
                w(&mut body, l.case_index);
            }
            if l.kind == LayoutKind::Closure {
                w(&mut body, l.function);
            }
            w(&mut body, l.fields.len() as u32);
            for &f in &l.fields {
                w(&mut body, f);
            }
        }

        // 3: CONSTANTS
        w(&mut body, self.constants.len() as u32);
        for c in &self.constants {
            w(&mut body, c.ty);
            match &c.value {
                ConstantValue::Immediate64(bytes) => body.extend_from_slice(bytes),
                ConstantValue::Decimal128(bytes) => body.extend_from_slice(bytes),
                ConstantValue::Text(idx) => w(&mut body, *idx),
                ConstantValue::Bytes(raw) => {
                    w(&mut body, raw.len() as u32);
                    body.extend_from_slice(raw);
                }
                ConstantValue::BigInt { sign, magnitude } => {
                    w(&mut body, *sign as u32);
                    w(&mut body, magnitude.len() as u32);
                    body.extend_from_slice(magnitude);
                }
                ConstantValue::Closure(fn_idx) => w(&mut body, *fn_idx),
            }
        }

        // 4: GLOBALS
        w(&mut body, self.globals.len() as u32);
        for g in &self.globals {
            w(&mut body, g.ty);
            w(&mut body, g.name);
            w(&mut body, g.module);
        }

        // 5: INIT
        w(&mut body, self.init.len() as u32);
        for &i in &self.init {
            w(&mut body, i);
        }

        // 6: FUNCTIONS
        w(&mut body, self.functions.len() as u32);
        for f in &self.functions {
            w(&mut body, f.name);
            w(&mut body, f.module);
            w(&mut body, f.kind as u32);
            w(&mut body, u32::from(f.spec_code));
            w(&mut body, f.parameters.len() as u32);
            for p in &f.parameters {
                w(&mut body, p.ty);
                w(&mut body, p.ownership as u32);
            }
            w(&mut body, f.result_type);
            w(&mut body, f.capture_types.len() as u32);
            for &c in &f.capture_types {
                w(&mut body, c);
            }
            w(&mut body, f.register_types.len() as u32);
            for &r in &f.register_types {
                w(&mut body, r);
            }
            w(&mut body, f.code.len() as u32);
            for &word in &f.code {
                w(&mut body, word);
            }
        }

        // 7: MODULES
        w(&mut body, self.modules.len() as u32);
        for m in &self.modules {
            w(&mut body, m.name);
            w(&mut body, m.types.len() as u32);
            for te in &m.types {
                w(&mut body, te.ty);
                w(&mut body, u32::from(te.contents_exposed));
            }
            w(&mut body, m.functions.len() as u32);
            for &f in &m.functions {
                w(&mut body, f);
            }
            w(&mut body, m.globals.len() as u32);
            for &g in &m.globals {
                w(&mut body, g);
            }
        }

        // 8: FILES
        w(&mut body, self.files.len() as u32);
        for f in &self.files {
            w(&mut body, f.len() as u32);
            body.extend_from_slice(f);
        }

        // 9: DEBUG
        w(&mut body, self.debug.len() as u32);
        for d in &self.debug {
            w(&mut body, d.spans.len() as u32);
            for s in &d.spans {
                w(&mut body, s.code_offset);
                w(&mut body, s.file);
                w(&mut body, s.start);
                w(&mut body, s.end);
            }
            w(&mut body, d.names.len() as u32);
            for n in &d.names {
                w(&mut body, n.reg);
                w(&mut body, n.name);
                w(&mut body, n.code_start);
                w(&mut body, n.code_end);
            }
            w(&mut body, d.text);
            w(&mut body, d.file);
            w(&mut body, d.start);
            w(&mut body, d.end);
            w(&mut body, d.subject);
            w(&mut body, d.expectation as u32);
            w(&mut body, d.lines.len() as u32);
            for l in &d.lines {
                w(&mut body, l.file);
                w(&mut body, l.start);
                w(&mut body, l.end);
                w(&mut body, l.text);
            }
        }

        let checksum = fnv1a(FNV_OFFSET, &body);
        let mut out = Vec::with_capacity(BYTECODE_HEADER_SIZE + body.len());
        out.extend_from_slice(&BYTECODE_MAGIC);
        out.extend_from_slice(&BYTECODE_MAJOR.to_le_bytes());
        out.extend_from_slice(&BYTECODE_MINOR.to_le_bytes());
        out.extend_from_slice(&BYTECODE_FLAGS_NONE.to_le_bytes());
        out.extend_from_slice(&checksum.to_le_bytes());
        out.extend_from_slice(&body);
        out
    }

    fn load_checked(&self) -> Result<crate::BytecodeFile, LoadError> {
        load(&self.encode())
    }
}

fn emit(code: &mut Vec<u32>, op: Opcode, var_count: u32, args: &[u32]) -> u32 {
    let pc = code.len() as u32;
    code.push((var_count << OPCODE_VARIABLE_SHIFT) | u32::from(op.number()));
    code.extend_from_slice(args);
    pc
}

fn assert_reject(b: &BcBuilder, needle: &str) {
    let err = b
        .load_checked()
        .expect_err(&format!("expected rejection containing {needle:?}"));
    assert!(
        err.reason.contains(needle),
        "expected reason containing {needle:?}, got: {err}"
    );
}

// ============================================================================
// Positive verifier tests
// ============================================================================

#[test]
fn valid_control_flow_clear_loop_and_readable_replay() {
    let mut b = BcBuilder::new();
    let t_int = b.add_prim(TypeKind::Int);
    let t_bool = b.add_prim(TypeKind::Bool);
    let t_text = b.add_prim(TypeKind::Text);
    let c_zero = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
    let c_one = b.add_const(t_int, ConstantValue::Immediate64(1i64.to_le_bytes()));
    let s_hi = b.add_string("hi");
    let c_hi = b.add_const(t_text, ConstantValue::Text(s_hi));

    // Loop that initializes r1..r4 inside the loop body, uses them,
    // and clears r1..r4 before jumping back to loop header so register states agree!
    // r0: Int (param), r1: Bool, r2: Text, r3: Int, r4: Text (owned dup)
    let mut code = Vec::new();
    let p_hdr = emit(&mut code, Opcode::Const, 0, &[3, c_zero]);
    emit(&mut code, Opcode::EqInt, 0, &[1, 0, 3]);
    let p_br = emit(&mut code, Opcode::Branch, 0, &[1, 0, 0]);
    let p_exit = emit(&mut code, Opcode::Ret, 0, &[0]);
    let p_body = emit(&mut code, Opcode::Const, 0, &[2, c_hi]);
    emit(&mut code, Opcode::Dup, 0, &[4, 2]);
    let p_drop = emit(&mut code, Opcode::Drop, 0, &[4]);
    emit(&mut code, Opcode::Clear, 0, &[2]);
    emit(&mut code, Opcode::Clear, 0, &[4]);
    emit(&mut code, Opcode::Const, 0, &[3, c_one]);
    emit(&mut code, Opcode::SubInt, 0, &[0, 0, 3]);
    emit(&mut code, Opcode::Clear, 0, &[1]);
    emit(&mut code, Opcode::Clear, 0, &[3]);
    emit(&mut code, Opcode::Jump, 0, &[p_hdr]);
    code[p_br as usize + 2] = p_exit;
    code[p_br as usize + 3] = p_body;

    b.add_fn(
        "loop_ok",
        FunctionKind::Function,
        false,
        vec![(t_int, ParamOwnership::Owned)],
        t_int,
        Vec::new(),
        vec![t_int, t_bool, t_text, t_int, t_text],
        code,
    );

    let file = b.load_checked().expect("loop with CLEAR must verify");
    // Check `registers_readable` at p_drop (r0, r1, r2, r3, r4 all readable) vs p_hdr (only r0).
    assert_eq!(
        file.registers_readable(0, p_hdr).unwrap(),
        vec![true, false, false, false, false]
    );
    assert_eq!(
        file.registers_readable(0, p_drop).unwrap(),
        vec![true, true, true, true, true]
    );

    // Check `visit_readable` matches `registers_readable` at every visited PC.
    let mut visited = Vec::new();
    visit_readable(&file, 0, |pc, mask| {
        visited.push((pc, mask.to_vec()));
        true
    })
    .unwrap();
    assert!(!visited.is_empty());
    for (pc, mask) in visited {
        assert_eq!(file.registers_readable(0, pc).unwrap(), mask, "at pc={pc}");
    }
}

#[test]
fn valid_switch_case_field_reset_reuse_and_free_token() {
    let mut b = BcBuilder::new();
    let t_int = b.add_prim(TypeKind::Int);
    let t_pair = b.add_choice(
        "Node",
        &[
            ("Empty", vec![]),
            ("Pair", vec![(Some("a"), t_int), (Some("b"), t_int)]),
        ],
    );
    let l_pair = b.add_layout(LayoutKind::Case, t_pair, 1, NO_INDEX, vec![t_int, t_int]);

    // fn bump(r0: Node owned) -> Node
    //   switch r0 -> [empty_arm, pair_arm]
    //   empty_arm: ret r0
    //   pair_arm:
    //     r1 = case_field r0, case 1, field 0
    //     r2 = case_field r0, case 1, field 1
    //     r3 = reset r0
    //     r4 = reuse r3, l_pair, (r2, r1)
    //     ret r4
    let mut code = Vec::new();
    let p_sw = emit(&mut code, Opcode::Switch, 2, &[0, 0, 0]);
    let p_empty = emit(&mut code, Opcode::Ret, 0, &[0]);
    let p_pair = emit(&mut code, Opcode::CaseField, 0, &[1, 0, 1, 0]);
    emit(&mut code, Opcode::CaseField, 0, &[2, 0, 1, 1]);
    emit(&mut code, Opcode::Reset, 0, &[3, 0]);
    emit(&mut code, Opcode::Reuse, 2, &[4, 3, l_pair, 2, 1]);
    emit(&mut code, Opcode::Ret, 0, &[4]);
    code[p_sw as usize + 2] = p_empty;
    code[p_sw as usize + 3] = p_pair;

    b.add_fn(
        "swap_pair",
        FunctionKind::Function,
        false,
        vec![(t_pair, ParamOwnership::Owned)],
        t_pair,
        Vec::new(),
        vec![t_pair, t_int, t_int, t_pair, t_pair],
        code,
    );

    // Also test FREE_TOKEN path.
    let mut code2 = Vec::new();
    let p_sw2 = emit(&mut code2, Opcode::Switch, 2, &[0, 0, 0]);
    let p_e2 = emit(&mut code2, Opcode::Drop, 0, &[0]);
    let c_zero = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
    emit(&mut code2, Opcode::Const, 0, &[1, c_zero]);
    emit(&mut code2, Opcode::Ret, 0, &[1]);
    let p_p2 = emit(&mut code2, Opcode::CaseField, 0, &[1, 0, 1, 0]);
    emit(&mut code2, Opcode::Reset, 0, &[2, 0]);
    emit(&mut code2, Opcode::FreeToken, 0, &[2]);
    emit(&mut code2, Opcode::Ret, 0, &[1]);
    code2[p_sw2 as usize + 2] = p_e2;
    code2[p_sw2 as usize + 3] = p_p2;

    b.add_fn(
        "first_or_zero",
        FunctionKind::Function,
        false,
        vec![(t_pair, ParamOwnership::Owned)],
        t_int,
        Vec::new(),
        vec![t_pair, t_int, t_pair],
        code2,
    );

    b.load_checked()
        .expect("switch/case_field/reset/reuse/free_token must verify");
}

#[test]
fn valid_closures_tail_calls_globals_and_spec_hostops() {
    let mut b = BcBuilder::new();
    let t_int = b.add_prim(TypeKind::Int);
    let t_unit = b.add_prim(TypeKind::Unit);
    let t_text = b.add_prim(TypeKind::Text);
    let t_console = b.add_cap(Capability::Console);
    let t_fn_int = b.add_fn_type(vec![t_int], t_int);
    let t_env_clos = b.add_env_type(0); // closure will be fn 0

    let c_one = b.add_const(t_int, ConstantValue::Immediate64(1i64.to_le_bytes()));
    let c_two = b.add_const(t_int, ConstantValue::Immediate64(2i64.to_le_bytes()));
    let s_hello = b.add_string("hello");
    let c_hello = b.add_const(t_text, ConstantValue::Text(s_hello));

    // fn 0: closure(r0: Int, r1: env(fn0) owned) -> Int { r2 = field r1, 0; drop r1; r3 = r0 + r2; ret r3 }
    let mut clos_code = Vec::new();
    emit(&mut clos_code, Opcode::Field, 0, &[2, 1, 0]);
    emit(&mut clos_code, Opcode::Drop, 0, &[1]);
    emit(&mut clos_code, Opcode::AddInt, 0, &[3, 0, 2]);
    emit(&mut clos_code, Opcode::Ret, 0, &[3]);
    let fn_clos = b.add_fn(
        "adder_clos",
        FunctionKind::Closure,
        false,
        vec![(t_int, ParamOwnership::Owned)],
        t_int,
        vec![t_int],
        vec![t_int, t_env_clos, t_int, t_int],
        clos_code,
    );
    b.add_layout(
        LayoutKind::Closure,
        t_fn_int,
        NO_INDEX,
        fn_clos,
        vec![t_int],
    );

    // Globals g0, g1 with init_g0 and init_g1 (init_g1 reads g0 via helper).
    let g0 = b.add_global(t_int, "G0");
    let g1 = b.add_global(t_int, "G1");

    // fn 1: helper reads g0
    let mut help_code = Vec::new();
    emit(&mut help_code, Opcode::Global, 0, &[0, g0]);
    emit(&mut help_code, Opcode::Ret, 0, &[0]);
    let fn_help = b.add_fn(
        "read_g0",
        FunctionKind::Function,
        false,
        vec![],
        t_int,
        vec![],
        vec![t_int],
        help_code,
    );

    // fn 2: init_g0 sets g0 = 1
    let mut i0_code = Vec::new();
    emit(&mut i0_code, Opcode::Const, 0, &[0, c_one]);
    emit(&mut i0_code, Opcode::SetGlobal, 0, &[g0, 0]);
    let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
    emit(&mut i0_code, Opcode::Const, 0, &[1, c_unit]);
    emit(&mut i0_code, Opcode::Ret, 0, &[1]);
    let fn_i0 = b.add_fn(
        "init_g0",
        FunctionKind::Init,
        false,
        vec![],
        t_unit,
        vec![],
        vec![t_int, t_unit],
        i0_code,
    );

    // fn 3: init_g1 calls helper, makes closure capturing g0, tail-calls? no, calls closure, sets g1
    let mut i1_code = Vec::new();
    emit(&mut i1_code, Opcode::Call, 0, &[0, fn_help]);
    emit(&mut i1_code, Opcode::MakeClosure, 1, &[1, fn_clos, 0]);
    emit(&mut i1_code, Opcode::Const, 0, &[2, c_two]);
    emit(&mut i1_code, Opcode::CallClosure, 1, &[3, 1, 2]);
    emit(&mut i1_code, Opcode::SetGlobal, 0, &[g1, 3]);
    emit(&mut i1_code, Opcode::Const, 0, &[4, c_unit]);
    emit(&mut i1_code, Opcode::Ret, 0, &[4]);
    let fn_i1 = b.add_fn(
        "init_g1",
        FunctionKind::Init,
        false,
        vec![],
        t_unit,
        vec![],
        vec![t_int, t_fn_int, t_int, t_int, t_unit],
        i1_code,
    );
    b.init = vec![fn_i0, fn_i1];

    // fn 4: spec Test using `spec_only` hostop `Console.fake_input` + `Console.print`
    let t_list_text = b.add_list(t_text);
    let t_list_empty_sig = b.add_fn_type(vec![], t_list_text);
    let mut test_code = Vec::new();
    emit(
        &mut test_code,
        Opcode::Prim,
        0,
        &[0, PrimId::ListEmpty as u32, t_list_empty_sig],
    );
    emit(
        &mut test_code,
        Opcode::Host,
        1,
        &[1, HostOpId::ConsoleFakeInput as u32, 0],
    );
    emit(&mut test_code, Opcode::Const, 0, &[2, c_hello]);
    emit(
        &mut test_code,
        Opcode::Host,
        2,
        &[3, HostOpId::ConsolePrint as u32, 1, 2],
    );
    emit(&mut test_code, Opcode::Drop, 0, &[1]);
    emit(&mut test_code, Opcode::Ret, 0, &[3]);
    b.add_fn(
        "spec_test",
        FunctionKind::Test,
        true,
        vec![],
        t_unit,
        vec![],
        vec![t_list_text, t_console, t_text, t_unit],
        test_code,
    );

    b.load_checked()
        .expect("closures, globals/init, and spec_only hostop in test must verify");
}

#[test]
fn older_fuzz_corpus_seeds_rejected_cleanly_for_version() {
    let corpus =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../fuzz/corpus/fuzz_bytecode_io");
    for name in [
        "empty.bc",
        "all-sections.bc",
        "service.bc",
        "service-missing-function.bc",
        "simulation.bc",
        "verified-primitive.bc",
        "verified-return.bc",
        "verified-switch-reset.bc",
    ] {
        let bytes = fs::read(corpus.join(name)).expect("read corpus seed");
        let err = load(&bytes).expect_err("v0.1/v0.2 seeds must be rejected by v0.3 loader");
        assert!(
            err.reason.contains("unsupported version"),
            "seed {name}: unexpected error {err}"
        );
    }
}

// ============================================================================
// Negative verifier tests: CFG, ownership/borrow, tokens, types, spec, limits
// ============================================================================

#[test]
fn reject_cfg_structural_errors() {
    // 1. Empty function
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        b.add_fn(
            "empty",
            FunctionKind::Function,
            false,
            vec![],
            t_int,
            vec![],
            vec![t_int],
            vec![],
        );
        assert_reject(&b, "no entry instruction");
    }

    // 2. Fallthrough past code end
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let c0 = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Const, 0, &[0, c0]);
        b.add_fn(
            "falloff",
            FunctionKind::Function,
            false,
            vec![],
            t_int,
            vec![],
            vec![t_int],
            code,
        );
        assert_reject(&b, "falls off code");
    }

    // 3. Mid-instruction jump target
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let c0 = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Jump, 0, &[3]); // pc=0..2; next insn is at pc=2 (len 3), so pc=3 is mid-insn!
        emit(&mut code, Opcode::Const, 0, &[0, c0]); // pc=2..5
        emit(&mut code, Opcode::Ret, 0, &[0]); // pc=5..7
        b.add_fn(
            "mid_jump",
            FunctionKind::Function,
            false,
            vec![],
            t_int,
            vec![],
            vec![t_int],
            code,
        );
        assert_reject(&b, "target not a boundary");
    }

    // 4. Unreachable instruction
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Ret, 0, &[0]);
        emit(&mut code, Opcode::Ret, 0, &[0]);
        b.add_fn(
            "dead",
            FunctionKind::Function,
            false,
            vec![(t_int, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_int],
            code,
        );
        assert_reject(&b, "unreachable");
    }
}

#[test]
fn reject_ownership_borrow_and_linear_errors() {
    // 1. Uninitialized register read
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Ret, 0, &[0]);
        b.add_fn(
            "uninit",
            FunctionKind::Function,
            false,
            vec![],
            t_int,
            vec![],
            vec![t_int],
            code,
        );
        assert_reject(&b, "not readable");
    }

    // 2. Use-after-move on owned reference
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Move, 0, &[1, 0]);
        emit(&mut code, Opcode::Move, 0, &[2, 0]); // r0 already moved!
        emit(&mut code, Opcode::Drop, 0, &[2]);
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "uam",
            FunctionKind::Function,
            false,
            vec![(t_text, ParamOwnership::Owned)],
            t_text,
            vec![],
            vec![t_text, t_text, t_text],
            code,
        );
        assert_reject(&b, "not readable");
    }

    // 3. Double drop
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_int = b.add_prim(TypeKind::Int);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Drop, 0, &[0]);
        emit(&mut code, Opcode::Drop, 0, &[0]);
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "double_drop",
            FunctionKind::Function,
            false,
            vec![
                (t_text, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_text, t_int],
            code,
        );
        assert_reject(&b, "not readable");
    }

    // 4. Overwriting owned reference without dropping
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let s = b.add_string("x");
        let c = b.add_const(t_text, ConstantValue::Text(s));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Const, 0, &[0, c]); // overwrites owned r0!
        emit(&mut code, Opcode::Dup, 0, &[1, 0]);
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "overwrite_owned",
            FunctionKind::Function,
            false,
            vec![(t_text, ParamOwnership::Owned)],
            t_text,
            vec![],
            vec![t_text, t_text],
            code,
        );
        assert_reject(&b, "overwrites ownership");
    }

    // 5. Leaked owned reference on RET
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_int = b.add_prim(TypeKind::Int);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Ret, 0, &[1]); // r0 (owned Text) leaked!
        b.add_fn(
            "leak_ret",
            FunctionKind::Function,
            false,
            vec![
                (t_text, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_text, t_int],
            code,
        );
        assert_reject(&b, "return leaves ownership");
    }

    // 6. Consuming a borrowed parameter (`DROP` on borrowed param)
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_int = b.add_prim(TypeKind::Int);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Drop, 0, &[0]); // r0 is Borrowed!
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "consume_borrowed",
            FunctionKind::Function,
            false,
            vec![
                (t_text, ParamOwnership::Borrowed),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_text, t_int],
            code,
        );
        assert_reject(&b, "consume needs an owned reference");
    }

    // 7. Returning a borrowed constant directly (consumes borrowed ref)
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let s = b.add_string("x");
        let c = b.add_const(t_text, ConstantValue::Text(s));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Const, 0, &[0, c]);
        emit(&mut code, Opcode::Ret, 0, &[0]);
        b.add_fn(
            "ret_borrowed",
            FunctionKind::Function,
            false,
            vec![],
            t_text,
            vec![],
            vec![t_text],
            code,
        );
        assert_reject(&b, "consume needs an owned reference");
    }

    // 8. Tail-call passing owned ref to borrowed parameter
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_int = b.add_prim(TypeKind::Int);
        let c0 = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut callee_code = Vec::new();
        emit(&mut callee_code, Opcode::Const, 0, &[1, c0]);
        emit(&mut callee_code, Opcode::Ret, 0, &[1]);
        let fn_callee = b.add_fn(
            "callee",
            FunctionKind::Function,
            false,
            vec![(t_text, ParamOwnership::Borrowed)],
            t_int,
            vec![],
            vec![t_text, t_int],
            callee_code,
        );
        let mut caller_code = Vec::new();
        emit(&mut caller_code, Opcode::TailCall, 1, &[fn_callee, 0]);
        b.add_fn(
            "caller",
            FunctionKind::Function,
            false,
            vec![(t_text, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_text],
            caller_code,
        );
        assert_reject(&b, "tail borrow needs a borrowed reference");
    }

    // 9. Repeated consumed register in one instruction
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_tup = b.add_tuple(vec![t_text, t_text]);
        let l_tup = b.add_layout(
            LayoutKind::Tuple,
            t_tup,
            NO_INDEX,
            NO_INDEX,
            vec![t_text, t_text],
        );
        let mut code = Vec::new();
        emit(&mut code, Opcode::Construct, 2, &[1, l_tup, 0, 0]); // r0 consumed twice!
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "repeat_consume",
            FunctionKind::Function,
            false,
            vec![(t_text, ParamOwnership::Owned)],
            t_tup,
            vec![],
            vec![t_text, t_tup],
            code,
        );
        assert_reject(&b, "consumed register is repeated");
    }
}

#[test]
fn reject_cfg_join_mismatches() {
    // 1. Branch join with ownership mismatch (one branch drops r1, other doesn't)
    {
        let mut b = BcBuilder::new();
        let t_bool = b.add_prim(TypeKind::Bool);
        let t_text = b.add_prim(TypeKind::Text);
        let t_int = b.add_prim(TypeKind::Int);
        let c0 = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut code = Vec::new();
        let p_br = emit(&mut code, Opcode::Branch, 0, &[0, 0, 0]);
        let p_then = emit(&mut code, Opcode::Drop, 0, &[1]);
        let p_j1 = emit(&mut code, Opcode::Jump, 0, &[0]);
        let p_else = emit(&mut code, Opcode::Jump, 0, &[0]);
        let p_join = emit(&mut code, Opcode::Const, 0, &[2, c0]);
        emit(&mut code, Opcode::Ret, 0, &[2]);
        code[p_br as usize + 2] = p_then;
        code[p_br as usize + 3] = p_else;
        code[p_j1 as usize + 1] = p_join;
        code[p_else as usize + 1] = p_join;

        b.add_fn(
            "join_mismatch",
            FunctionKind::Function,
            false,
            vec![
                (t_bool, ParamOwnership::Owned),
                (t_text, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_bool, t_text, t_int],
            code,
        );
        assert_reject(&b, "register states disagree at join");
    }

    // 2. Loop backedge without CLEAR (register uninit on entry, value on backedge)
    {
        let mut b = BcBuilder::new();
        let t_bool = b.add_prim(TypeKind::Bool);
        let t_int = b.add_prim(TypeKind::Int);
        let c0 = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut code = Vec::new();
        let p_hdr = emit(&mut code, Opcode::Branch, 0, &[0, 0, 0]);
        let p_exit = emit(&mut code, Opcode::Const, 0, &[1, c0]);
        emit(&mut code, Opcode::Ret, 0, &[1]);
        let p_body = emit(&mut code, Opcode::Const, 0, &[1, c0]);
        emit(&mut code, Opcode::Jump, 0, &[p_hdr]);
        code[p_hdr as usize + 2] = p_exit;
        code[p_hdr as usize + 3] = p_body;

        b.add_fn(
            "loop_no_clear",
            FunctionKind::Function,
            false,
            vec![(t_bool, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_bool, t_int],
            code,
        );
        assert_reject(&b, "register states disagree at join");
    }
}

#[test]
fn reject_token_and_case_field_errors() {
    // 1. CASE_FIELD without dominating SWITCH
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_opt = b.add_choice("Opt", &[("None", vec![]), ("Some", vec![(None, t_int)])]);
        let mut code = Vec::new();
        emit(&mut code, Opcode::CaseField, 0, &[1, 0, 1, 0]);
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "bad_cf",
            FunctionKind::Function,
            false,
            vec![(t_opt, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_opt, t_int],
            code,
        );
        assert_reject(&b, "case is not known");
    }

    // 2. REUSE with mismatched layout vs RESET token
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_r1 = b.add_record("R1", &[("a", t_int)]);
        let t_r2 = b.add_record("R2", &[("a", t_int), ("b", t_int)]);
        let _l1 = b.add_layout(LayoutKind::Record, t_r1, NO_INDEX, NO_INDEX, vec![t_int]);
        let l2 = b.add_layout(
            LayoutKind::Record,
            t_r2,
            NO_INDEX,
            NO_INDEX,
            vec![t_int, t_int],
        );
        let c0 = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Const, 0, &[1, c0]);
        emit(&mut code, Opcode::Reset, 0, &[2, 0]);
        emit(&mut code, Opcode::Reuse, 2, &[3, 2, l2, 1, 1]);
        emit(&mut code, Opcode::Ret, 0, &[3]);
        b.add_fn(
            "bad_reuse_layout",
            FunctionKind::Function,
            false,
            vec![(t_r1, ParamOwnership::Owned)],
            t_r2,
            vec![],
            vec![t_r1, t_int, t_r1, t_r2],
            code,
        );
        assert_reject(&b, "reuse layout differs from token");
    }

    // 3. FREE_TOKEN on a non-token register
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_r1 = b.add_record("R1", &[("a", t_int)]);
        let _l1 = b.add_layout(LayoutKind::Record, t_r1, NO_INDEX, NO_INDEX, vec![t_int]);
        let mut code = Vec::new();
        emit(&mut code, Opcode::FreeToken, 0, &[0]); // r0 is Owned, not Token!
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "bad_free",
            FunctionKind::Function,
            false,
            vec![
                (t_r1, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_r1, t_int],
            code,
        );
        assert_reject(&b, "not a token");
    }

    // 4. Leaked reuse token on RET
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_r1 = b.add_record("R1", &[("a", t_int)]);
        let _l1 = b.add_layout(LayoutKind::Record, t_r1, NO_INDEX, NO_INDEX, vec![t_int]);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Reset, 0, &[2, 0]);
        emit(&mut code, Opcode::Ret, 0, &[1]); // token in r2 leaked!
        b.add_fn(
            "leak_tok",
            FunctionKind::Function,
            false,
            vec![
                (t_r1, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_r1, t_int, t_r1],
            code,
        );
        assert_reject(&b, "return leaves ownership or token");
    }

    // 5. RESET on a non-boxed choice case
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_opt = b.add_choice("Opt", &[("None", vec![]), ("Some", vec![(None, t_int)])]);
        let mut code = Vec::new();
        let p_sw = emit(&mut code, Opcode::Switch, 2, &[0, 0, 0]);
        let p_none = emit(&mut code, Opcode::Ret, 0, &[0]);
        let p_some = emit(&mut code, Opcode::Reset, 0, &[1, 0]); // Some(Int) is Inline, not Boxed!
        emit(&mut code, Opcode::FreeToken, 0, &[1]);
        emit(&mut code, Opcode::Ret, 0, &[0]);
        code[p_sw as usize + 2] = p_none;
        code[p_sw as usize + 3] = p_some;
        b.add_fn(
            "reset_inline",
            FunctionKind::Function,
            false,
            vec![(t_opt, ParamOwnership::Owned)],
            t_opt,
            vec![],
            vec![t_opt, t_opt],
            code,
        );
        assert_reject(&b, "reset needs a known fixed layout");
    }
}

#[test]
fn reject_type_ability_spec_and_global_init_errors() {
    // 1. `spec_only` hostop (`Console.fake_input`) in non-spec function
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_list = b.add_list(t_text);
        let t_console = b.add_cap(Capability::Console);
        let t_empty_sig = b.add_fn_type(vec![], t_list);
        let mut code = Vec::new();
        emit(
            &mut code,
            Opcode::Prim,
            0,
            &[0, PrimId::ListEmpty as u32, t_empty_sig],
        );
        emit(
            &mut code,
            Opcode::Host,
            1,
            &[1, HostOpId::ConsoleFakeInput as u32, 0],
        );
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "bad_spec_host",
            FunctionKind::Function,
            false, // NOT spec_code!
            vec![],
            t_console,
            vec![],
            vec![t_list, t_console],
            code,
        );
        assert_reject(&b, "spec host in non-spec code");
    }

    // 2. `FAIL` outside spec_code
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_unit = b.add_prim(TypeKind::Unit);
        let s = b.add_string("nope");
        let c = b.add_const(t_text, ConstantValue::Text(s));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Const, 0, &[0, c]);
        emit(&mut code, Opcode::Fail, 0, &[0]);
        b.add_fn(
            "bad_fail",
            FunctionKind::Function,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_text],
            code,
        );
        assert_reject(&b, "fail outside spec code");
    }

    // 3. `SIM` START outside spec_code
    {
        let mut b = BcBuilder::new();
        let t_sim = b.add_cap(Capability::Simulation);
        let t_time = b.add_prim(TypeKind::Time);
        let c_t = b.add_const(t_time, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let mut code = Vec::new();
        emit(&mut code, Opcode::Const, 0, &[1, c_t]);
        emit(
            &mut code,
            Opcode::Sim,
            1,
            &[0, SimOpId::Start as u32, 0, 0, 1],
        );
        emit(&mut code, Opcode::Ret, 0, &[0]);
        b.add_fn(
            "bad_sim",
            FunctionKind::Function,
            false,
            vec![],
            t_sim,
            vec![],
            vec![t_sim, t_time],
            code,
        );
        assert_reject(&b, "simulation start in non-spec code");
    }

    // 4. `EQUAL` on a type without Equal ability (Function)
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_bool = b.add_prim(TypeKind::Bool);
        let t_fn = b.add_fn_type(vec![t_int], t_int);
        let mut code = Vec::new();
        emit(&mut code, Opcode::Equal, 0, &[2, t_fn, 0, 1]);
        emit(&mut code, Opcode::Ret, 0, &[2]);
        b.add_fn(
            "bad_eq",
            FunctionKind::Function,
            false,
            vec![
                (t_fn, ParamOwnership::Borrowed),
                (t_fn, ParamOwnership::Borrowed),
            ],
            t_bool,
            vec![],
            vec![t_fn, t_fn, t_bool],
            code,
        );
        assert_reject(&b, "equal lacks Equal");
    }

    // 5. `Map` key lacking `Hash` ability (Float)
    {
        let mut b = BcBuilder::new();
        let t_float = b.add_prim(TypeKind::Float);
        let t_int = b.add_prim(TypeKind::Int);
        let _t_map = b.add_map(t_float, t_int);
        assert_reject(&b, "lacks Hash");
    }

    // 6. `Set` element lacking `Hash` ability (Function)
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_fn = b.add_fn_type(vec![t_int], t_int);
        let _t_set = b.add_set(t_fn);
        assert_reject(&b, "lacks Hash");
    }

    // 7. `PRIM` signature mismatch (`Int.abs` with Text)
    {
        let mut b = BcBuilder::new();
        let t_text = b.add_prim(TypeKind::Text);
        let t_sig = b.add_fn_type(vec![t_text], t_text);
        let mut code = Vec::new();
        emit(
            &mut code,
            Opcode::Prim,
            1,
            &[1, PrimId::IntAbs as u32, t_sig, 0],
        );
        emit(&mut code, Opcode::Ret, 0, &[1]);
        b.add_fn(
            "bad_prim",
            FunctionKind::Function,
            false,
            vec![(t_text, ParamOwnership::Borrowed)],
            t_text,
            vec![],
            vec![t_text, t_text],
            code,
        );
        assert_reject(&b, "primitive signature or constraint");
    }

    // 8. `SET_GLOBAL` outside init function
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_unit = b.add_prim(TypeKind::Unit);
        let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
        let c_zero = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let g0 = b.add_global(t_int, "G0");
        let mut i0 = Vec::new();
        emit(&mut i0, Opcode::Const, 0, &[0, c_zero]);
        emit(&mut i0, Opcode::SetGlobal, 0, &[g0, 0]);
        emit(&mut i0, Opcode::Const, 0, &[1, c_unit]);
        emit(&mut i0, Opcode::Ret, 0, &[1]);
        let f_i0 = b.add_fn(
            "init_g0",
            FunctionKind::Init,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_int, t_unit],
            i0,
        );
        b.init = vec![f_i0];
        let mut code = Vec::new();
        emit(&mut code, Opcode::SetGlobal, 0, &[g0, 0]);
        emit(&mut code, Opcode::Ret, 0, &[0]);
        b.add_fn(
            "bad_setg",
            FunctionKind::Function,
            false,
            vec![(t_int, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_int],
            code,
        );
        assert_reject(&b, "global store outside init");
    }

    // 9. Init reads an unset global (global 1 before global 1 is initialized)
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_unit = b.add_prim(TypeKind::Unit);
        let g0 = b.add_global(t_int, "G0");
        let g1 = b.add_global(t_int, "G1");
        let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
        let c_one = b.add_const(t_int, ConstantValue::Immediate64(1i64.to_le_bytes()));

        let mut i0_code = Vec::new();
        emit(&mut i0_code, Opcode::Global, 0, &[0, g1]); // g1 not set yet!
        emit(&mut i0_code, Opcode::SetGlobal, 0, &[g0, 0]);
        emit(&mut i0_code, Opcode::Const, 0, &[1, c_unit]);
        emit(&mut i0_code, Opcode::Ret, 0, &[1]);
        let fn_i0 = b.add_fn(
            "init_g0",
            FunctionKind::Init,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_int, t_unit],
            i0_code,
        );

        let mut i1_code = Vec::new();
        emit(&mut i1_code, Opcode::Const, 0, &[0, c_one]);
        emit(&mut i1_code, Opcode::SetGlobal, 0, &[g1, 0]);
        emit(&mut i1_code, Opcode::Const, 0, &[1, c_unit]);
        emit(&mut i1_code, Opcode::Ret, 0, &[1]);
        let fn_i1 = b.add_fn(
            "init_g1",
            FunctionKind::Init,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_int, t_unit],
            i1_code,
        );
        b.init = vec![fn_i0, fn_i1];
        assert_reject(&b, "init reads an unset global");
    }

    // 10. Init function returns without setting its global
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_unit = b.add_prim(TypeKind::Unit);
        let _g0 = b.add_global(t_int, "G0");
        let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
        let mut i0_code = Vec::new();
        emit(&mut i0_code, Opcode::Const, 0, &[0, c_unit]);
        emit(&mut i0_code, Opcode::Ret, 0, &[0]);
        let fn_i0 = b.add_fn(
            "init_none",
            FunctionKind::Init,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_unit],
            i0_code,
        );
        b.init = vec![fn_i0];
        assert_reject(&b, "init has no global store");
    }
}

#[test]
fn reject_verifier_limits_register_blocks_and_work_budget() {
    // 1. MO_LIMIT_FUNCTION_REGISTER_BLOCKS:
    // Exact boundary passes; +1 block fails with ErrorKind::Limit.
    for extra in [0usize, 1usize] {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let regs = 2048usize;
        let blocks = (MO_LIMIT_FUNCTION_REGISTER_BLOCKS as usize / regs) + extra;
        let mut code = Vec::with_capacity(blocks * 2);
        for i in 0..blocks - 1 {
            let next_pc = ((i + 1) * 2) as u32;
            emit(&mut code, Opcode::Jump, 0, &[next_pc]);
        }
        emit(&mut code, Opcode::Ret, 0, &[0]);

        b.add_fn(
            "reg_block_boundary",
            FunctionKind::Function,
            false,
            vec![(t_int, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_int; regs],
            code,
        );
        if extra == 0 {
            b.load_checked()
                .expect("exact register-block boundary must pass");
        } else {
            let err = b
                .load_checked()
                .expect_err("register-block limit + 1 must fail");
            assert_eq!(err.kind, ErrorKind::Limit);
            assert!(err.reason.contains("register-block"), "{err}");
        }
    }

    // 2. MO_LIMIT_VERIFY_WORK_PER_WORD:
    // Many branches into one join block stay below MO_LIMIT_FUNCTION_REGISTER_BLOCKS
    // while exhausting the wire-proportional work budget.
    {
        let mut b = BcBuilder::new();
        let t_bool = b.add_prim(TypeKind::Bool);
        let t_int = b.add_prim(TypeKind::Int);
        let regs = 1024usize;
        let branches = 1000usize;
        assert!(
            ((branches + 1) * regs) as u64 <= MO_LIMIT_FUNCTION_REGISTER_BLOCKS,
            "must stay within register-block limit to isolate work limit"
        );
        let words = (regs + branches * 4 + 2 + 32) as u64;
        assert!(
            ((branches * (regs + 1)) as u64) > words * MO_LIMIT_VERIFY_WORK_PER_WORD,
            "join checks must exceed work_limit"
        );
        let join_pc = (branches * 4) as u32;
        let mut code = Vec::with_capacity(branches * 4 + 2);
        for i in 0..branches {
            let next_pc = ((i + 1) * 4) as u32;
            emit(&mut code, Opcode::Branch, 0, &[0, join_pc, next_pc]);
        }
        emit(&mut code, Opcode::Ret, 0, &[1]);

        let mut reg_types = vec![t_int; regs];
        reg_types[0] = t_bool;
        b.add_fn(
            "work_bomb",
            FunctionKind::Function,
            false,
            vec![
                (t_bool, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            reg_types,
            code,
        );
        let err = b
            .load_checked()
            .expect_err("verifier work budget must fail");
        assert_eq!(err.kind, ErrorKind::Limit);
        assert!(err.reason.contains("work limit"), "{err}");
    }
}

// ============================================================================
// Deterministic `.bc` loader & verifier mutation fuzz test (>= 5,000 iters)
// ============================================================================

struct SplitMix64(u64);

impl SplitMix64 {
    fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    fn next_usize(&mut self, bound: usize) -> usize {
        (self.next_u64() as usize) % bound
    }
}

fn recompute_checksum(buf: &mut [u8]) {
    if buf.len() >= BYTECODE_HEADER_SIZE {
        let sum = fnv1a(FNV_OFFSET, &buf[BYTECODE_HEADER_SIZE..]);
        buf[12..20].copy_from_slice(&sum.to_le_bytes());
    }
}

fn synthetic_fuzz_seeds() -> Vec<Vec<u8>> {
    let mut seeds = Vec::new();

    // Seed A: BigInt + Text + Bytes + closure + switch + reset/reuse + global/init.
    let mut b = BcBuilder::new();
    let t_int = b.add_prim(TypeKind::Int);
    let t_big = b.add_prim(TypeKind::BigInt);
    let t_text = b.add_prim(TypeKind::Text);
    let t_bytes = b.add_prim(TypeKind::Bytes);
    let t_unit = b.add_prim(TypeKind::Unit);
    let t_choice = b.add_choice(
        "Boxed",
        &[
            ("Empty", vec![]),
            ("Two", vec![(Some("x"), t_int), (Some("y"), t_big)]),
        ],
    );
    let l_two = b.add_layout(LayoutKind::Case, t_choice, 1, NO_INDEX, vec![t_int, t_big]);
    let c_big = b.add_const(
        t_big,
        ConstantValue::BigInt {
            sign: BigIntSign::Negative,
            magnitude: vec![0x34, 0x12, 0xff, 0x01],
        },
    );
    let s_hi = b.add_string("hello-fuzz");
    let _c_text = b.add_const(t_text, ConstantValue::Text(s_hi));
    let _c_bytes = b.add_const(t_bytes, ConstantValue::Bytes(vec![1, 2, 3, 4]));
    let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
    let g0 = b.add_global(t_big, "G_BIG");

    let mut i0 = Vec::new();
    emit(&mut i0, Opcode::Const, 0, &[0, c_big]);
    emit(&mut i0, Opcode::Dup, 0, &[1, 0]);
    emit(&mut i0, Opcode::SetGlobal, 0, &[g0, 1]);
    emit(&mut i0, Opcode::Const, 0, &[2, c_unit]);
    emit(&mut i0, Opcode::Ret, 0, &[2]);
    let f_i0 = b.add_fn(
        "init_big",
        FunctionKind::Init,
        false,
        vec![],
        t_unit,
        vec![],
        vec![t_big, t_big, t_unit],
        i0,
    );
    b.init = vec![f_i0];

    let mut f1 = Vec::new();
    let p_sw = emit(&mut f1, Opcode::Switch, 2, &[0, 0, 0]);
    let p_e = emit(&mut f1, Opcode::Ret, 0, &[0]);
    let p_t = emit(&mut f1, Opcode::CaseField, 0, &[1, 0, 1, 0]);
    emit(&mut f1, Opcode::CaseField, 0, &[2, 0, 1, 1]);
    emit(&mut f1, Opcode::Reset, 0, &[3, 0]);
    emit(&mut f1, Opcode::Reuse, 2, &[4, 3, l_two, 1, 2]);
    emit(&mut f1, Opcode::Ret, 0, &[4]);
    f1[p_sw as usize + 2] = p_e;
    f1[p_sw as usize + 3] = p_t;
    b.add_fn(
        "rebuild",
        FunctionKind::Function,
        false,
        vec![(t_choice, ParamOwnership::Owned)],
        t_choice,
        vec![],
        vec![t_choice, t_int, t_big, t_choice, t_choice],
        f1,
    );
    let raw = b.encode();
    assert!(load(&raw).is_ok(), "synthetic fuzz seed A must be valid");
    seeds.push(raw);

    // Seed B: closures, Map/Set/List, primitives, spec Test + Console.fake_input + loop/CLEAR.
    let mut b2 = BcBuilder::new();
    let t_int = b2.add_prim(TypeKind::Int);
    let t_bool = b2.add_prim(TypeKind::Bool);
    let t_text = b2.add_prim(TypeKind::Text);
    let t_unit = b2.add_prim(TypeKind::Unit);
    let t_console = b2.add_cap(Capability::Console);
    let t_list_text = b2.add_list(t_text);
    let _t_map = b2.add_map(t_text, t_int);
    let _t_set = b2.add_set(t_int);
    let t_fn = b2.add_fn_type(vec![t_int], t_int);
    let t_env = b2.add_env_type(0);
    let t_empty_sig = b2.add_fn_type(vec![], t_list_text);
    let c0 = b2.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
    let c1 = b2.add_const(t_int, ConstantValue::Immediate64(1i64.to_le_bytes()));
    let s_msg = b2.add_string("fuzz");
    let c_msg = b2.add_const(t_text, ConstantValue::Text(s_msg));

    let mut clos = Vec::new();
    emit(&mut clos, Opcode::Field, 0, &[2, 1, 0]);
    emit(&mut clos, Opcode::Drop, 0, &[1]);
    emit(&mut clos, Opcode::AddInt, 0, &[3, 0, 2]);
    emit(&mut clos, Opcode::Ret, 0, &[3]);
    let f_clos = b2.add_fn(
        "clos",
        FunctionKind::Closure,
        false,
        vec![(t_int, ParamOwnership::Owned)],
        t_int,
        vec![t_int],
        vec![t_int, t_env, t_int, t_int],
        clos,
    );
    b2.add_layout(LayoutKind::Closure, t_fn, NO_INDEX, f_clos, vec![t_int]);

    let mut looper = Vec::new();
    let p_h = emit(&mut looper, Opcode::Const, 0, &[2, c0]);
    emit(&mut looper, Opcode::EqInt, 0, &[1, 0, 2]);
    let p_br = emit(&mut looper, Opcode::Branch, 0, &[1, 0, 0]);
    let p_ex = emit(&mut looper, Opcode::Ret, 0, &[0]);
    let p_bd = emit(&mut looper, Opcode::MakeClosure, 1, &[3, f_clos, 0]);
    emit(&mut looper, Opcode::Const, 0, &[4, c1]);
    emit(&mut looper, Opcode::CallClosure, 1, &[5, 3, 4]);
    emit(&mut looper, Opcode::SubInt, 0, &[0, 5, 4]);
    emit(&mut looper, Opcode::SubInt, 0, &[0, 0, 4]);
    emit(&mut looper, Opcode::Clear, 0, &[1]);
    emit(&mut looper, Opcode::Clear, 0, &[2]);
    emit(&mut looper, Opcode::Clear, 0, &[3]);
    emit(&mut looper, Opcode::Clear, 0, &[4]);
    emit(&mut looper, Opcode::Clear, 0, &[5]);
    emit(&mut looper, Opcode::Jump, 0, &[p_h]);
    looper[p_br as usize + 2] = p_ex;
    looper[p_br as usize + 3] = p_bd;
    b2.add_fn(
        "looper",
        FunctionKind::Function,
        false,
        vec![(t_int, ParamOwnership::Owned)],
        t_int,
        vec![],
        vec![t_int, t_bool, t_int, t_fn, t_int, t_int],
        looper,
    );

    let mut tst = Vec::new();
    emit(
        &mut tst,
        Opcode::Prim,
        0,
        &[0, PrimId::ListEmpty as u32, t_empty_sig],
    );
    emit(
        &mut tst,
        Opcode::Host,
        1,
        &[1, HostOpId::ConsoleFakeInput as u32, 0],
    );
    emit(&mut tst, Opcode::Const, 0, &[2, c_msg]);
    emit(
        &mut tst,
        Opcode::Host,
        2,
        &[3, HostOpId::ConsolePrint as u32, 1, 2],
    );
    emit(&mut tst, Opcode::Drop, 0, &[1]);
    emit(&mut tst, Opcode::Ret, 0, &[3]);
    b2.add_fn(
        "spec_fuzz",
        FunctionKind::Test,
        true,
        vec![],
        t_unit,
        vec![],
        vec![t_list_text, t_console, t_text, t_unit],
        tst,
    );
    let raw2 = b2.encode();
    assert!(load(&raw2).is_ok(), "synthetic fuzz seed B must be valid");
    seeds.push(raw2);

    seeds
}

#[test]
fn fuzz_bc_mutations_never_panic() {
    let corpus_dir =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../fuzz/corpus/fuzz_bytecode_io");
    let mut seeds = synthetic_fuzz_seeds();
    for entry in [
        "empty.bc",
        "all-sections.bc",
        "service.bc",
        "service-missing-function.bc",
        "verified-primitive.bc",
        "verified-return.bc",
        "verified-switch-reset.bc",
    ] {
        let data = fs::read(corpus_dir.join(entry)).expect("read corpus seed");
        seeds.push(data);
    }
    // Also include a truncated slice of `simulation.bc` (full file is 150 KB; slicing keeps
    // 6,000 mutations fast while still exercising the simulation/service wire layout).
    if let Ok(sim) = fs::read(corpus_dir.join("simulation.bc")) {
        seeds.push(sim[..sim.len().min(4096)].to_vec());
    }

    let interesting_words: [u32; 12] = [
        0,
        1,
        2,
        3,
        4,
        86,
        87,
        255,
        0x0001_0000,
        0x7fff_ffff,
        0xffff_fffe,
        u32::MAX,
    ];

    let mut rng = SplitMix64(0x51a7_1b00_f022_c0de);
    let iterations = 6_000usize;
    let mut accepted = 0usize;
    let mut rejected = 0usize;

    for iter in 0..iterations {
        let base_idx = rng.next_usize(seeds.len());
        let mut buf = seeds[base_idx].clone();

        let mutations = 1 + rng.next_usize(4);
        for _ in 0..mutations {
            match rng.next_usize(8) {
                // Bit flip
                0 if !buf.is_empty() => {
                    let pos = rng.next_usize(buf.len());
                    let bit = 1u8 << rng.next_usize(8);
                    buf[pos] ^= bit;
                }
                // Random byte overwrite
                1 if !buf.is_empty() => {
                    let pos = rng.next_usize(buf.len());
                    buf[pos] = (rng.next_u64() & 0xff) as u8;
                }
                // Word-aligned interesting boundary value (targets counts, opcodes, indices, offsets)
                2 if buf.len() >= BYTECODE_HEADER_SIZE + 4 => {
                    let word_count = (buf.len() - BYTECODE_HEADER_SIZE) / 4;
                    let off = BYTECODE_HEADER_SIZE + rng.next_usize(word_count) * 4;
                    let val = interesting_words[rng.next_usize(interesting_words.len())];
                    buf[off..off + 4].copy_from_slice(&val.to_le_bytes());
                }
                // Truncation
                3 if !buf.is_empty() => {
                    let new_len = rng.next_usize(buf.len());
                    buf.truncate(new_len);
                }
                // Append trailing junk
                4 => {
                    let extra = 1 + rng.next_usize(16);
                    for _ in 0..extra {
                        buf.push((rng.next_u64() & 0xff) as u8);
                    }
                }
                // Splice prefix of this seed with suffix of another seed
                5 if !buf.is_empty() => {
                    let other = &seeds[rng.next_usize(seeds.len())];
                    if !other.is_empty() {
                        let cut_a = rng.next_usize(buf.len());
                        let cut_b = rng.next_usize(other.len());
                        buf.truncate(cut_a);
                        buf.extend_from_slice(&other[cut_b..]);
                    }
                }
                // Small byte deletion inside payload
                6 if buf.len() > BYTECODE_HEADER_SIZE + 4 => {
                    let start =
                        BYTECODE_HEADER_SIZE + rng.next_usize(buf.len() - BYTECODE_HEADER_SIZE);
                    let del = (1 + rng.next_usize(8)).min(buf.len() - start);
                    buf.drain(start..start + del);
                }
                // Header mutation (magic/version/flags)
                _ => {
                    if buf.len() >= 12 {
                        let pos = rng.next_usize(12);
                        buf[pos] = (rng.next_u64() & 0xff) as u8;
                    }
                }
            }
        }

        // 85% of iterations repair the checksum so mutations penetrate past header validation
        // directly into section decoding, structural validation, and dataflow verification.
        if iter % 7 != 0 {
            recompute_checksum(&mut buf);
        }

        match load(&buf) {
            Ok(file) => {
                accepted += 1;
                // Every accepted mutated file must also replay `registers_readable` cleanly.
                for (fi, f) in file.functions.iter().enumerate().take(8) {
                    if !f.code.is_empty() {
                        let mask = file
                            .registers_readable(fi as u32, 0)
                            .expect("verified mutated image must replay pc=0");
                        assert_eq!(mask.len(), f.register_types.len());
                    }
                }
            }
            Err(err) => {
                rejected += 1;
                assert!(!err.reason.is_empty());
                assert!(
                    (err.offset as usize) <= buf.len(),
                    "error offset {} past buffer length {} ({})",
                    err.offset,
                    buf.len(),
                    err
                );
            }
        }
    }

    assert_eq!(accepted + rejected, iterations);
    assert!(
        rejected > iterations / 2,
        "expected most random mutations to be rejected"
    );
}
