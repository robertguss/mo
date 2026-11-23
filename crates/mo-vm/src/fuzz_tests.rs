//! Deterministic `mo-bytecode` + `mo-vm` execution fuzz & tight-limits stress tests.
//!
//! Mutates real corpus `.bc` files and synthetic `.bc` programs covering arithmetic,
//! Perceus reuse, closures, collections, fake capabilities, simulation, infinite loops,
//! deep recursion, and defect rendering. Every buffer that passes `mo_bytecode::load`
//! is initialized and executed under tight `Limits` (`heap_size`, `stack_size`,
//! `call_depth`) and bounded instruction budgets — asserting zero panics, zero hangs,
//! and zero stack overflows.

use std::fs;
use std::io;
use std::path::PathBuf;

use mo_bytecode::{
    AnchorExpectation, BYTECODE_FLAGS_NONE, BYTECODE_HEADER_SIZE, BYTECODE_MAGIC, BYTECODE_MAJOR,
    BYTECODE_MINOR, BigIntSign, Capability, ConstantDef, ConstantValue, DebugEntry, FNV_OFFSET,
    FunctionDef, FunctionKind, GlobalDef, LayoutDef, LayoutKind, ModuleDef, NO_INDEX,
    OPCODE_VARIABLE_SHIFT, ParamOwnership, ParameterDef, RegisterName, SpecLine, TypeCase, TypeDef,
    TypeField, TypeKind, fnv1a, load,
};
use mo_def::{HostOpId, Opcode, PrimId};
use mo_heap::Value;
use mo_host::HostContext;

use crate::{Image, Limits, Outcome, Process, render_defect};

// ============================================================================
// Deterministic PRNG (SplitMix64)
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

fn emit(code: &mut Vec<u32>, op: Opcode, var_count: u32, operands: &[u32]) -> u32 {
    let pc = code.len() as u32;
    code.push((var_count << OPCODE_VARIABLE_SHIFT) | u32::from(op.number()));
    code.extend_from_slice(operands);
    pc
}

// ============================================================================
// Minimal `.bc` v0.3 wire builder for synthetic VM fuzz/stress seeds
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

    #[allow(clippy::too_many_arguments)]
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

        w(&mut body, self.strings.len() as u32);
        for s in &self.strings {
            w(&mut body, s.len() as u32);
            body.extend_from_slice(s.as_bytes());
        }

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
                TypeKind::Env => w(&mut body, t.function),
                TypeKind::Capability => w(&mut body, t.capability.unwrap() as u32),
            }
        }

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

        w(&mut body, self.globals.len() as u32);
        for g in &self.globals {
            w(&mut body, g.ty);
            w(&mut body, g.name);
            w(&mut body, g.module);
        }

        w(&mut body, self.init.len() as u32);
        for &i in &self.init {
            w(&mut body, i);
        }

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

        w(&mut body, self.files.len() as u32);
        for f in &self.files {
            w(&mut body, f.len() as u32);
            body.extend_from_slice(f);
        }

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
}

/// Builds verified synthetic `.bc` `BcBuilder` seeds covering numeric bugs, Perceus reuse,
/// closures, heap growth / OOM, stack / depth exhaustion, infinite loops, fake caps,
/// large defect variable truncation, and anchors.
fn build_vm_fuzz_builders() -> Vec<BcBuilder> {
    let mut builders = Vec::new();

    // Seed 1: Arithmetic edge cases, BigInt global init, large-Text stop truncation, anchors.
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_dec = b.add_prim(TypeKind::Decimal);
        let t_flt = b.add_prim(TypeKind::Float);
        let t_bool = b.add_prim(TypeKind::Bool);
        let t_text = b.add_prim(TypeKind::Text);
        let t_big = b.add_prim(TypeKind::BigInt);
        let t_unit = b.add_prim(TypeKind::Unit);
        let t_repeat_sig = b.add_fn_type(vec![t_text, t_int], t_text);

        let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
        let c_zero = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let c_one = b.add_const(t_int, ConstantValue::Immediate64(1i64.to_le_bytes()));
        let c_big_repeat = b.add_const(t_int, ConstantValue::Immediate64(5000i64.to_le_bytes()));
        let c_dec_max = b.add_const(t_dec, ConstantValue::Decimal128(i128::MAX.to_le_bytes()));
        let c_flt_zero = b.add_const(t_flt, ConstantValue::Immediate64(0.0f64.to_le_bytes()));
        let s_x = b.add_string("x");
        let c_x = b.add_const(t_text, ConstantValue::Text(s_x));
        let s_boom = b.add_string("boom");
        let c_boom = b.add_const(t_text, ConstantValue::Text(s_boom));
        let c_big = b.add_const(
            t_big,
            ConstantValue::BigInt {
                sign: BigIntSign::Negative,
                magnitude: vec![0xff, 0xee, 0xdd, 0x01],
            },
        );
        let g0 = b.add_global(t_big, "G_BIG");

        let mut init_code = Vec::new();
        emit(&mut init_code, Opcode::Const, 0, &[0, c_big]);
        emit(&mut init_code, Opcode::Dup, 0, &[1, 0]);
        emit(&mut init_code, Opcode::SetGlobal, 0, &[g0, 1]);
        emit(&mut init_code, Opcode::Const, 0, &[2, c_unit]);
        emit(&mut init_code, Opcode::Ret, 0, &[2]);
        let f_init = b.add_fn(
            "init_big",
            FunctionKind::Init,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_big, t_big, t_unit],
            init_code,
        );
        b.init = vec![f_init];

        // fn arith(a: Int, b: Int) -> Int: exercises checked add/div/rem and decimal/float ops.
        let mut arith = Vec::new();
        emit(&mut arith, Opcode::AddInt, 0, &[2, 0, 1]);
        emit(&mut arith, Opcode::DivInt, 0, &[3, 2, 1]);
        emit(&mut arith, Opcode::RemInt, 0, &[4, 0, 1]);
        emit(&mut arith, Opcode::Const, 0, &[5, c_flt_zero]);
        emit(&mut arith, Opcode::DivFloat, 0, &[6, 5, 5]);
        emit(&mut arith, Opcode::EqFloat, 0, &[7, 6, 6]);
        emit(&mut arith, Opcode::Const, 0, &[8, c_dec_max]);
        emit(&mut arith, Opcode::NegDecimal, 0, &[9, 8]);
        emit(&mut arith, Opcode::AddInt, 0, &[10, 3, 4]);
        emit(&mut arith, Opcode::Ret, 0, &[10]);
        let f_arith = b.add_fn(
            "arith",
            FunctionKind::Function,
            false,
            vec![
                (t_int, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![
                t_int, t_int, t_int, t_int, t_int, t_flt, t_flt, t_bool, t_dec, t_dec, t_int,
            ],
            arith,
        );

        // fn stop_with_large_text() -> Int: allocates 5000-byte Text then stops (hits truncation).
        let mut stop_large = Vec::new();
        emit(&mut stop_large, Opcode::Const, 0, &[0, c_x]);
        emit(&mut stop_large, Opcode::Dup, 0, &[4, 0]);
        emit(&mut stop_large, Opcode::Const, 0, &[1, c_big_repeat]);
        emit(
            &mut stop_large,
            Opcode::Prim,
            2,
            &[2, PrimId::TextRepeat as u32, t_repeat_sig, 4, 1],
        );
        emit(&mut stop_large, Opcode::Const, 0, &[3, c_boom]);
        let stop_end = emit(&mut stop_large, Opcode::Stop, 0, &[3]) + 2;
        let f_stop = b.add_fn(
            "stop_large",
            FunctionKind::Function,
            false,
            vec![],
            t_int,
            vec![],
            vec![t_text, t_int, t_text, t_text, t_text],
            stop_large,
        );
        let s_var = b.add_string("large");
        b.debug[f_stop as usize].names.push(RegisterName {
            reg: 2,
            name: s_var,
            code_start: 0,
            code_end: stop_end,
        });

        // Promise + Allows/Forbids anchor functions.
        let mut prom_code = Vec::new();
        emit(&mut prom_code, Opcode::Const, 0, &[3, c_zero]);
        emit(&mut prom_code, Opcode::Ret, 0, &[3]);
        let f_prom = b.add_fn(
            "Main.arith.promise.1",
            FunctionKind::Promise,
            true,
            vec![
                (t_int, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
                (t_int, ParamOwnership::Owned),
            ],
            t_int,
            vec![],
            vec![t_int, t_int, t_int, t_int],
            prom_code,
        );
        b.files.push(b"seed1.mo".to_vec());
        let s_cond = b.add_string("true");
        b.debug[f_prom as usize].subject = f_arith;
        b.debug[f_prom as usize].lines.push(SpecLine {
            file: 0,
            start: 0,
            end: 4,
            text: s_cond,
        });
        let mut allows_code = Vec::new();
        emit(&mut allows_code, Opcode::Const, 0, &[0, c_zero]);
        emit(&mut allows_code, Opcode::Ret, 0, &[0]);
        let f_allows = b.add_fn(
            "Main.arith.promise.1.allows.1",
            FunctionKind::Anchor,
            true,
            vec![],
            t_int,
            vec![],
            vec![t_int],
            allows_code,
        );
        b.debug[f_allows as usize].subject = f_prom;
        b.debug[f_allows as usize].expectation = AnchorExpectation::Allows;

        let mut forbids_code = Vec::new();
        emit(&mut forbids_code, Opcode::Const, 0, &[0, c_one]);
        emit(&mut forbids_code, Opcode::Ret, 0, &[0]);
        let f_forbids = b.add_fn(
            "Main.arith.promise.1.forbids.1",
            FunctionKind::Anchor,
            true,
            vec![],
            t_int,
            vec![],
            vec![t_int],
            forbids_code,
        );
        b.debug[f_forbids as usize].subject = f_prom;
        b.debug[f_forbids as usize].expectation = AnchorExpectation::Forbids;

        let raw = b.encode();
        load(&raw).expect("VM fuzz seed 1 must verify");
        builders.push(b);
    }

    // Seed 2: Choices, RESET/REUSE, closures, non-tail recursion, infinite loop, fake Console.
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_bool = b.add_prim(TypeKind::Bool);
        let t_text = b.add_prim(TypeKind::Text);
        let t_unit = b.add_prim(TypeKind::Unit);
        let t_console = b.add_cap(Capability::Console);
        let t_list_text = b.add_list(t_text);
        let t_fn = b.add_fn_type(vec![t_int], t_int);
        let t_env = b.add_env_type(0);
        let t_empty_sig = b.add_fn_type(vec![], t_list_text);
        let t_choice = b.add_choice(
            "Node",
            &[
                ("Leaf", vec![]),
                ("Pair", vec![(Some("a"), t_int), (Some("b"), t_text)]),
            ],
        );
        let l_pair = b.add_layout(LayoutKind::Case, t_choice, 1, NO_INDEX, vec![t_int, t_text]);

        let c_zero = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let c_one = b.add_const(t_int, ConstantValue::Immediate64(1i64.to_le_bytes()));
        let s_hi = b.add_string("hi");
        let c_hi = b.add_const(t_text, ConstantValue::Text(s_hi));

        // fn 0: closure(x: Int, env: Env) -> Int
        let mut clos = Vec::new();
        emit(&mut clos, Opcode::Field, 0, &[2, 1, 0]);
        emit(&mut clos, Opcode::Drop, 0, &[1]);
        emit(&mut clos, Opcode::AddInt, 0, &[3, 0, 2]);
        emit(&mut clos, Opcode::Ret, 0, &[3]);
        let f_clos = b.add_fn(
            "clos",
            FunctionKind::Closure,
            false,
            vec![(t_int, ParamOwnership::Owned)],
            t_int,
            vec![t_int],
            vec![t_int, t_env, t_int, t_int],
            clos,
        );
        b.add_layout(LayoutKind::Closure, t_fn, NO_INDEX, f_clos, vec![t_int]);

        // fn 1: infinite_loop() -> Int (hits instruction budget cleanly)
        let mut inf = Vec::new();
        emit(&mut inf, Opcode::Const, 0, &[0, c_zero]);
        let p_loop = emit(&mut inf, Opcode::Const, 0, &[1, c_one]);
        emit(&mut inf, Opcode::Clear, 0, &[1]);
        emit(&mut inf, Opcode::Jump, 0, &[p_loop]);
        b.add_fn(
            "infinite_loop",
            FunctionKind::Function,
            false,
            vec![],
            t_int,
            vec![],
            vec![t_int, t_int],
            inf,
        );

        // fn 2: deep_recurse(n: Int) -> Int (hits --max-depth / --max-stack cleanly)
        let mut rec = Vec::new();
        emit(&mut rec, Opcode::Const, 0, &[1, c_zero]);
        emit(&mut rec, Opcode::EqInt, 0, &[2, 0, 1]);
        let p_br = emit(&mut rec, Opcode::Branch, 0, &[2, 0, 0]);
        let p_ret = emit(&mut rec, Opcode::Ret, 0, &[0]);
        let p_rec = emit(&mut rec, Opcode::Const, 0, &[3, c_one]);
        emit(&mut rec, Opcode::SubInt, 0, &[4, 0, 3]);
        emit(&mut rec, Opcode::Call, 1, &[5, 2, 4]);
        emit(&mut rec, Opcode::AddInt, 0, &[6, 5, 3]);
        emit(&mut rec, Opcode::Ret, 0, &[6]);
        rec[p_br as usize + 2] = p_ret;
        rec[p_br as usize + 3] = p_rec;
        b.add_fn(
            "deep_recurse",
            FunctionKind::Function,
            false,
            vec![(t_int, ParamOwnership::Owned)],
            t_int,
            vec![],
            vec![t_int, t_int, t_bool, t_int, t_int, t_int, t_int],
            rec,
        );

        // fn 3: reuse_and_console() -> Unit (MakeCase + Switch + Reset + Reuse + Closure + Console.fake)
        let mut rc = Vec::new();
        emit(&mut rc, Opcode::Const, 0, &[0, c_one]);
        emit(&mut rc, Opcode::Const, 0, &[1, c_hi]);
        emit(&mut rc, Opcode::Dup, 0, &[2, 1]);
        emit(&mut rc, Opcode::MakeCase, 2, &[3, t_choice, 1, 0, 2]);
        let p_sw = emit(&mut rc, Opcode::Switch, 2, &[3, 0, 0]);
        let p_leaf = emit(&mut rc, Opcode::Drop, 0, &[3]);
        emit(&mut rc, Opcode::Jump, 0, &[0]); // patched below
        let p_pair = emit(&mut rc, Opcode::CaseField, 0, &[4, 3, 1, 0]);
        emit(&mut rc, Opcode::CaseField, 0, &[5, 3, 1, 1]);
        emit(&mut rc, Opcode::Reset, 0, &[6, 3]);
        emit(&mut rc, Opcode::Reuse, 2, &[7, 6, l_pair, 4, 5]);
        emit(&mut rc, Opcode::Drop, 0, &[7]);
        emit(&mut rc, Opcode::Clear, 0, &[4]);
        emit(&mut rc, Opcode::Clear, 0, &[5]);
        emit(&mut rc, Opcode::Clear, 0, &[6]);
        emit(&mut rc, Opcode::Clear, 0, &[7]);
        let p_done = emit(&mut rc, Opcode::MakeClosure, 1, &[8, f_clos, 0]);
        emit(&mut rc, Opcode::CallClosure, 1, &[9, 8, 0]);
        emit(
            &mut rc,
            Opcode::Prim,
            0,
            &[10, PrimId::ListEmpty as u32, t_empty_sig],
        );
        emit(
            &mut rc,
            Opcode::Host,
            1,
            &[11, HostOpId::ConsoleFakeInput as u32, 10],
        );
        emit(
            &mut rc,
            Opcode::Host,
            2,
            &[12, HostOpId::ConsolePrint as u32, 11, 1],
        );
        emit(&mut rc, Opcode::Drop, 0, &[11]);
        emit(&mut rc, Opcode::Ret, 0, &[12]);
        rc[p_sw as usize + 2] = p_leaf;
        rc[p_sw as usize + 3] = p_pair;
        rc[p_leaf as usize + 3] = p_done;
        b.add_fn(
            "reuse_and_console",
            FunctionKind::Test,
            true,
            vec![],
            t_unit,
            vec![],
            vec![
                t_int,
                t_text,
                t_text,
                t_choice,
                t_int,
                t_text,
                t_choice,
                t_choice,
                t_fn,
                t_int,
                t_list_text,
                t_console,
                t_unit,
            ],
            rc,
        );

        let raw = b.encode();
        load(&raw).expect("VM fuzz seed 2 must verify");
        builders.push(b);
    }

    // Seed 3: Infinite loop inside `Init` function (verifies `Image::init_bounded` never hangs).
    {
        let mut b = BcBuilder::new();
        let t_int = b.add_prim(TypeKind::Int);
        let t_unit = b.add_prim(TypeKind::Unit);
        let t_bool = b.add_prim(TypeKind::Bool);
        let c_zero = b.add_const(t_int, ConstantValue::Immediate64(0i64.to_le_bytes()));
        let c_unit = b.add_const(t_unit, ConstantValue::Immediate64([0; 8]));
        let c_false = b.add_const(t_bool, ConstantValue::Immediate64([0; 8]));
        let g0 = b.add_global(t_int, "G_SPIN");
        let mut cond_spin = Vec::new();
        let p_hdr = emit(&mut cond_spin, Opcode::Const, 0, &[0, c_false]);
        let p_br = emit(&mut cond_spin, Opcode::Branch, 0, &[0, 0, 0]);
        let p_exit = emit(&mut cond_spin, Opcode::Const, 0, &[1, c_zero]);
        emit(&mut cond_spin, Opcode::SetGlobal, 0, &[g0, 1]);
        emit(&mut cond_spin, Opcode::Const, 0, &[2, c_unit]);
        emit(&mut cond_spin, Opcode::Ret, 0, &[2]);
        let p_again = emit(&mut cond_spin, Opcode::Clear, 0, &[0]);
        emit(&mut cond_spin, Opcode::Jump, 0, &[p_hdr]);
        cond_spin[p_br as usize + 2] = p_exit;
        cond_spin[p_br as usize + 3] = p_again;
        let f_init = b.add_fn(
            "init_spin",
            FunctionKind::Init,
            false,
            vec![],
            t_unit,
            vec![],
            vec![t_bool, t_int, t_unit],
            cond_spin,
        );
        b.init = vec![f_init];

        let raw = b.encode();
        load(&raw).expect("VM fuzz seed 3 must verify");
        builders.push(b);
    }

    builders
}

fn build_vm_fuzz_seeds() -> Vec<Vec<u8>> {
    build_vm_fuzz_builders()
        .into_iter()
        .map(|b| b.encode())
        .collect()
}

fn clone_builder(b: &BcBuilder) -> BcBuilder {
    BcBuilder {
        strings: b.strings.clone(),
        types: b.types.clone(),
        layouts: b.layouts.clone(),
        constants: b.constants.clone(),
        globals: b.globals.clone(),
        init: b.init.clone(),
        functions: b.functions.clone(),
        modules: b.modules.clone(),
        files: b.files.clone(),
        debug: b.debug.clone(),
    }
}

/// Structure-aware mutation of constants, arithmetic/comparison opcodes, and branch targets
/// before wire encoding + full `mo_bytecode::load` verification, ensuring hundreds of distinct
/// mutated programs pass the verifier and execute deep inside `mo-vm`.
fn mutate_builder(b: &mut BcBuilder, rng: &mut SplitMix64) {
    let edge_ints = [
        0i64,
        1,
        -1,
        2,
        -2,
        16,
        255,
        10_000,
        i64::MAX,
        i64::MIN,
        i64::MIN + 1,
    ];
    let edge_floats = [
        0.0f64,
        -0.0,
        1.0,
        -1.0,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NAN,
        f64::MAX,
        f64::MIN_POSITIVE,
    ];
    let edge_decimals = [
        0i128,
        1,
        -1,
        1_000_000_000_000_000_000,
        i128::MAX,
        -i128::MAX,
    ];
    let int_binops = [
        Opcode::AddInt,
        Opcode::SubInt,
        Opcode::MulInt,
        Opcode::DivInt,
        Opcode::RemInt,
    ];
    let flt_binops = [
        Opcode::AddFloat,
        Opcode::SubFloat,
        Opcode::MulFloat,
        Opcode::DivFloat,
        Opcode::RemFloat,
    ];
    let int_cmps = [Opcode::EqInt, Opcode::LtInt, Opcode::LeInt];

    let edits = 1 + rng.next_usize(3);
    for _ in 0..edits {
        match rng.next_usize(4) {
            // Mutate a constant payload to boundary values (Int, Float, Bool, Decimal, BigInt).
            0 if !b.constants.is_empty() => {
                let idx = rng.next_usize(b.constants.len());
                let kind = b.types[b.constants[idx].ty as usize].kind;
                match (&mut b.constants[idx].value, kind) {
                    (ConstantValue::Immediate64(bytes), TypeKind::Int) => {
                        *bytes = edge_ints[rng.next_usize(edge_ints.len())].to_le_bytes();
                    }
                    (ConstantValue::Immediate64(bytes), TypeKind::Float) => {
                        *bytes = edge_floats[rng.next_usize(edge_floats.len())].to_le_bytes();
                    }
                    (ConstantValue::Immediate64(bytes), TypeKind::Bool) => {
                        bytes[0] = (rng.next_u64() & 1) as u8;
                    }
                    (ConstantValue::Decimal128(bytes), TypeKind::Decimal) => {
                        *bytes = edge_decimals[rng.next_usize(edge_decimals.len())].to_le_bytes();
                    }
                    (ConstantValue::BigInt { sign, magnitude }, TypeKind::BigInt) => {
                        if (rng.next_u64() & 3) == 0 {
                            *sign = BigIntSign::Zero;
                            magnitude.clear();
                        } else {
                            *sign = if (rng.next_u64() & 1) == 0 {
                                BigIntSign::Positive
                            } else {
                                BigIntSign::Negative
                            };
                            let len = 1 + rng.next_usize(12);
                            magnitude.resize(len, 0);
                            for byte in magnitude.iter_mut() {
                                *byte = (rng.next_u64() & 0xff) as u8;
                            }
                            if let Some(last) = magnitude.last_mut()
                                && *last == 0
                            {
                                *last = 1;
                            }
                        }
                    }
                    _ => {}
                }
            }
            // Swap compatible instruction opcodes inside function bodies (preserving exact operand layout).
            1 if !b.functions.is_empty() => {
                let fidx = rng.next_usize(b.functions.len());
                let code = &mut b.functions[fidx].code;
                let mut pc = 0usize;
                while pc < code.len() {
                    let head = code[pc];
                    let op_num = (head & 0xffff) as u16;
                    let var_count = (head >> OPCODE_VARIABLE_SHIFT) as usize;
                    let Some(op) = Opcode::from_u16(op_num) else {
                        break;
                    };
                    let step = 1 + (op.info().fixed_count as usize) + var_count;
                    if rng.next_usize(3) == 0 {
                        let new_op = match op {
                            Opcode::AddInt
                            | Opcode::SubInt
                            | Opcode::MulInt
                            | Opcode::DivInt
                            | Opcode::RemInt => Some(int_binops[rng.next_usize(int_binops.len())]),
                            Opcode::AddFloat
                            | Opcode::SubFloat
                            | Opcode::MulFloat
                            | Opcode::DivFloat
                            | Opcode::RemFloat => {
                                Some(flt_binops[rng.next_usize(flt_binops.len())])
                            }
                            Opcode::EqInt | Opcode::LtInt | Opcode::LeInt => {
                                Some(int_cmps[rng.next_usize(int_cmps.len())])
                            }
                            Opcode::Branch if pc + 3 < code.len() => {
                                code.swap(pc + 2, pc + 3);
                                None
                            }
                            _ => None,
                        };
                        if let Some(n) = new_op {
                            code[pc] = (head & !0xffff) | u32::from(n.number());
                        }
                    }
                    pc = pc.saturating_add(step);
                }
            }
            // Mutate register operand in a random code word (may be rejected or accepted by verifier).
            2 if !b.functions.is_empty() => {
                let fidx = rng.next_usize(b.functions.len());
                let rcount = b.functions[fidx].register_types.len();
                let code = &mut b.functions[fidx].code;
                if !code.is_empty() && rcount > 0 {
                    let pos = rng.next_usize(code.len());
                    code[pos] = rng.next_usize(rcount) as u32;
                }
            }
            // Mutate string contents (valid UTF-8) to exercise text/defect/formatting edge cases.
            _ => {
                if b.strings.len() > 1 {
                    let idx = 1 + rng.next_usize(b.strings.len() - 1);
                    let choices = ["", "a", "λ\n\t\"#{x}", "xxxxxxxxxxxxxxxx"];
                    b.strings[idx] = choices[rng.next_usize(choices.len())].to_string();
                }
            }
        }
    }
}

fn load_all_seeds() -> Vec<Vec<u8>> {
    let mut seeds = build_vm_fuzz_seeds();
    let corpus_dir =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../fuzz/corpus/fuzz_bytecode_io");
    for entry in [
        "empty.bc",
        "all-sections.bc",
        "service.bc",
        "service-missing-function.bc",
        "verified-primitive.bc",
        "verified-return.bc",
        "verified-switch-reset.bc",
        "simulation.bc",
    ] {
        if let Ok(data) = fs::read(corpus_dir.join(entry)) {
            seeds.push(data);
        }
    }
    seeds
}

fn sandboxed_host() -> HostContext {
    HostContext::with_io(Vec::new(), Vec::new(), io::empty(), io::sink(), io::sink())
}

fn synthesize_scalar_arg(kind: TypeKind, rng: &mut SplitMix64) -> Option<Value> {
    let edge_ints = [0i64, 1, -1, 2, -2, 7, 100, i64::MAX, i64::MIN];
    let edge_floats = [
        0.0f64,
        -0.0,
        1.0,
        -1.0,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NAN,
    ];
    match kind {
        TypeKind::Unit => Some(Value::Unit),
        TypeKind::Bool => Some(Value::Bool((rng.next_u64() & 1) != 0)),
        TypeKind::Byte => Some(Value::Byte((rng.next_u64() & 0xff) as u8)),
        TypeKind::Int => Some(Value::Int(edge_ints[rng.next_usize(edge_ints.len())])),
        TypeKind::Float => Some(Value::Float(edge_floats[rng.next_usize(edge_floats.len())])),
        TypeKind::Char => Some(Value::Char('a')),
        TypeKind::Time => Some(Value::Time(0)),
        TypeKind::Duration => Some(Value::Duration(edge_ints[rng.next_usize(edge_ints.len())])),
        TypeKind::Decimal => Some(Value::Decimal(i128::from(
            edge_ints[rng.next_usize(edge_ints.len())],
        ))),
        TypeKind::Range => Some(Value::Range(0, 5)),
        _ => None,
    }
}

fn is_safe_fuzz_main(image: &Image, main_fn: u32) -> bool {
    let file = image.file();
    file.functions[main_fn as usize].parameters.iter().all(|p| {
        matches!(
            file.types.get(p.ty).capability,
            Some(
                Capability::Args
                    | Capability::Env
                    | Capability::Console
                    | Capability::Random
                    | Capability::SecureRandom
                    | Capability::Simulation
            )
        )
    })
}

#[derive(Default)]
struct RunStats {
    init_ok: usize,
    init_stopped: usize,
    calls_returned: usize,
    calls_failed: usize,
    calls_stopped: usize,
    hit_oom: usize,
    hit_stack: usize,
    hit_depth: usize,
    hit_budget: usize,
}

fn record_defect(file: &mo_bytecode::BytecodeFile, d: &crate::Defect, stats: &mut RunStats) {
    let rendered = render_defect(file, d);
    assert!(!rendered.is_empty());
    if d.limit_name == Some("--max-heap") || d.reason.contains("out of memory") {
        stats.hit_oom += 1;
    } else if d.limit_name == Some("--max-stack") {
        stats.hit_stack += 1;
    } else if d.limit_name == Some("--max-depth") {
        stats.hit_depth += 1;
    } else if d.reason.contains("instruction budget") {
        stats.hit_budget += 1;
    }
}

fn execute_verified_image(
    file: mo_bytecode::BytecodeFile,
    limits: Limits,
    max_steps: u64,
    rng: &mut SplitMix64,
    stats: &mut RunStats,
) {
    let image = match Image::init_bounded(file, limits, max_steps) {
        Ok(img) => {
            stats.init_ok += 1;
            img
        }
        Err(err) => {
            let (f, d) = *err;
            stats.init_stopped += 1;
            record_defect(&f, &d, stats);
            return;
        }
    };

    // Exercise safe `main` entry if present.
    if let Some(main_fn) = image.find_main_entry()
        && is_safe_fuzz_main(&image, main_fn)
    {
        let mut proc = Process::new(&image, limits, sandboxed_host());
        match proc.call_main_bounded(main_fn, max_steps) {
            Outcome::Returned(_) => stats.calls_returned += 1,
            Outcome::Failed(d) => {
                stats.calls_failed += 1;
                record_defect(image.file(), &d, stats);
            }
            Outcome::Stopped(d) => {
                stats.calls_stopped += 1;
                record_defect(image.file(), &d, stats);
            }
        }
    }

    // Exercise callable functions whose arguments can be synthesized purely from scalars.
    let fcount = image.file().functions.len().min(16);
    for fn_idx in 0..fcount {
        let f = &image.file().functions[fn_idx];
        if f.kind == FunctionKind::Closure || f.kind == FunctionKind::Init {
            continue;
        }
        let mut args = Vec::with_capacity(f.parameters.len());
        let mut can_call = true;
        for p in &f.parameters {
            let kind = image.file().types.get(p.ty).kind;
            match synthesize_scalar_arg(kind, rng) {
                Some(v) => args.push(v),
                None => {
                    can_call = false;
                    break;
                }
            }
        }
        if !can_call {
            continue;
        }
        let mut proc = Process::new(&image, limits, sandboxed_host());
        match proc.call_bounded(fn_idx as u32, args, max_steps) {
            Outcome::Returned(_) => stats.calls_returned += 1,
            Outcome::Failed(d) => {
                stats.calls_failed += 1;
                record_defect(image.file(), &d, stats);
            }
            Outcome::Stopped(d) => {
                stats.calls_stopped += 1;
                record_defect(image.file(), &d, stats);
            }
        }
    }
}

#[test]
fn vm_stress_tight_limits_and_bounded_work_never_panic() {
    let seeds = load_all_seeds();
    let mut rng = SplitMix64(0x1e_575e_5500_c0de);
    let mut stats = RunStats::default();

    let limit_matrix = [
        Limits {
            heap_size: 64,
            stack_size: 128,
            call_depth: 1,
        },
        Limits {
            heap_size: 512,
            stack_size: 512,
            call_depth: 3,
        },
        Limits {
            heap_size: 4096,
            stack_size: 2048,
            call_depth: 8,
        },
        Limits {
            heap_size: 65_536,
            stack_size: 16_384,
            call_depth: 32,
        },
    ];

    for seed in &seeds {
        for &limits in &limit_matrix {
            if let Ok(file) = load(seed) {
                execute_verified_image(file, limits, 2_000, &mut rng, &mut stats);
            }
        }
    }

    assert!(stats.calls_returned > 0, "expected some calls to return");
    assert!(stats.calls_stopped > 0, "expected some calls to stop");
    assert!(stats.hit_oom > 0, "expected tight heap_size to trigger OOM");
    assert!(
        stats.hit_stack > 0,
        "expected tight stack_size to trigger --max-stack"
    );
    assert!(
        stats.hit_depth > 0,
        "expected tight call_depth to trigger --max-depth"
    );
    assert!(
        stats.hit_budget > 0,
        "expected infinite loops to trigger instruction budget"
    );

    // Confirm Seed 1's anchors and truncated large-Text defect variable.
    let file1 = load(&seeds[0]).expect("seed 1 loads");
    let img1 = Image::init_bounded(file1, Limits::default(), 100_000).expect("seed 1 inits");
    let anchors = img1.anchor_functions();
    assert_eq!(anchors.len(), 2);
    assert_eq!(anchors[0].1, AnchorExpectation::Allows);
    assert_eq!(anchors[1].1, AnchorExpectation::Forbids);
    let mut proc1 = Process::new(&img1, Limits::default(), sandboxed_host());
    match proc1.call_bounded(2, Vec::new(), 100_000) {
        Outcome::Stopped(defect) => {
            assert_eq!(defect.variables.len(), 1);
            assert_eq!(defect.variables[0].name, "large");
            assert!(
                defect.variables[0].truncated,
                "5000-byte Text variable must be marked truncated for mo test parity"
            );
            assert!(defect.variables[0].value.contains("… <truncated>"));
        }
        _ => panic!("expected stop_large to stop with truncated variable"),
    }
}

#[test]
fn vm_fuzz_mutated_verified_bytecode_never_panics() {
    let builders = build_vm_fuzz_builders();
    let mut seeds = load_all_seeds();
    // Cap oversized corpus entries so 2,500 mutations stay fast while preserving header/section diversity.
    for seed in &mut seeds {
        if seed.len() > 8192 {
            let truncated = seed[..4096].to_vec();
            *seed = truncated;
        }
    }

    let interesting_words: [u32; 12] = [
        0,
        1,
        2,
        3,
        4,
        7,
        86,
        87,
        255,
        0x7fff_ffff,
        0xffff_fffe,
        u32::MAX,
    ];

    let mut rng = SplitMix64(0x1e_f022_7000_beef);
    let mut stats = RunStats::default();
    let mut load_accepted = 0usize;
    let mut load_rejected = 0usize;

    let iterations = 2_500usize;
    for iter in 0..iterations {
        let mut buf = if iter % 3 == 0 {
            // Structure-aware mutations yield a high rate of verifier-accepted mutant programs
            // that execute deeply inside `mo-vm` with mutated constants, opcodes, and branches.
            let mut mutated = clone_builder(&builders[rng.next_usize(builders.len())]);
            mutate_builder(&mut mutated, &mut rng);
            mutated.encode()
        } else {
            // Raw wire-level mutations stress header/section decoding, structural validation,
            // ownership verification, and execution of any surviving mutated image.
            let mut raw = seeds[rng.next_usize(seeds.len())].clone();
            let mutations = 1 + rng.next_usize(3);
            for _ in 0..mutations {
                match rng.next_usize(7) {
                    0 if raw.len() > BYTECODE_HEADER_SIZE => {
                        let pos =
                            BYTECODE_HEADER_SIZE + rng.next_usize(raw.len() - BYTECODE_HEADER_SIZE);
                        raw[pos] ^= 1u8 << rng.next_usize(8);
                    }
                    1 if !raw.is_empty() => {
                        let pos = rng.next_usize(raw.len());
                        raw[pos] = (rng.next_u64() & 0xff) as u8;
                    }
                    2 if raw.len() >= BYTECODE_HEADER_SIZE + 4 => {
                        let words = (raw.len() - BYTECODE_HEADER_SIZE) / 4;
                        let off = BYTECODE_HEADER_SIZE + rng.next_usize(words) * 4;
                        let val = interesting_words[rng.next_usize(interesting_words.len())];
                        raw[off..off + 4].copy_from_slice(&val.to_le_bytes());
                    }
                    3 if !raw.is_empty() => {
                        let new_len = rng.next_usize(raw.len());
                        raw.truncate(new_len);
                    }
                    4 => {
                        let extra = 1 + rng.next_usize(12);
                        for _ in 0..extra {
                            raw.push((rng.next_u64() & 0xff) as u8);
                        }
                    }
                    5 if !raw.is_empty() => {
                        let other = &seeds[rng.next_usize(seeds.len())];
                        if !other.is_empty() {
                            let cut_a = rng.next_usize(raw.len());
                            let cut_b = rng.next_usize(other.len());
                            raw.truncate(cut_a);
                            raw.extend_from_slice(&other[cut_b..]);
                        }
                    }
                    _ => {
                        if raw.len() > BYTECODE_HEADER_SIZE + 4 {
                            let start = BYTECODE_HEADER_SIZE
                                + rng.next_usize(raw.len() - BYTECODE_HEADER_SIZE);
                            let del = (1 + rng.next_usize(8)).min(raw.len() - start);
                            raw.drain(start..start + del);
                        }
                    }
                }
            }
            if iter % 10 != 0 {
                recompute_checksum(&mut raw);
            }
            raw
        };

        // Occasionally flip a header byte even on structure-aware runs.
        if iter % 47 == 0 && buf.len() >= 12 {
            buf[0] ^= 1;
        }

        match load(&buf) {
            Ok(file) => {
                load_accepted += 1;
                let limits = Limits {
                    heap_size: match rng.next_usize(4) {
                        0 => 256,
                        1 => 2048,
                        2 => 16_384,
                        _ => 65_536,
                    },
                    stack_size: match rng.next_usize(3) {
                        0 => 256,
                        1 => 1024,
                        _ => 8192,
                    },
                    call_depth: match rng.next_usize(3) {
                        0 => 2,
                        1 => 6,
                        _ => 24,
                    },
                };
                execute_verified_image(file, limits, 2_000, &mut rng, &mut stats);
            }
            Err(_) => {
                load_rejected += 1;
            }
        }
    }

    assert_eq!(load_accepted + load_rejected, iterations);
    assert!(
        load_rejected > iterations / 3,
        "expected many malformed mutations to be rejected (got {load_rejected}/{iterations})"
    );
    assert!(
        load_accepted >= 200,
        "expected >= 200 verified mutated images to reach VM execution (got {load_accepted})"
    );
    assert!(
        stats.calls_returned + stats.calls_failed + stats.calls_stopped + stats.init_stopped >= 500,
        "expected >= 500 executed mutated VM runs"
    );
}
