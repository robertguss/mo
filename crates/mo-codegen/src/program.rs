//! Whole-program declaration lowering, global initializer synthesis, service descriptor
//! emission, and final bytecode assembly matching `src/codegen/program.c` and `src/codegen/compile.c`.

use std::collections::HashSet;

use mo_bytecode::types::TypeTable;
use mo_bytecode::validate::service_reach;
use mo_bytecode::{
    AnchorExpectation, BYTECODE_FLAGS_NONE, BYTECODE_FLAGS_SERVICES, BYTECODE_MAGIC,
    BYTECODE_MAJOR, BYTECODE_MINOR, BytecodeFile, CaseShape, ConstantDef, ConstantValue, ErrorKind,
    FunctionDef, FunctionKind, GlobalDef, Header, LayoutDef, LayoutKind, ModuleDef, NO_INDEX,
    ParamOwnership, ParameterDef, ServiceDef, ServiceTimer, ServiceTool, SpecLine, TypeCase,
    TypeExport, TypeField, TypeKind, write as write_bytecode,
};
use mo_core::{Expr, ExprId, FnKind, Metadata};
use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_CORE_DEPTH, MO_LIMIT_FIELDS, MO_LIMIT_FUNCTIONS,
    MO_LIMIT_GLOBALS, MO_LIMIT_MODULE_NAMES, MO_LIMIT_MODULES, MO_LIMIT_SERVICE_HANDLERS,
    MO_LIMIT_SERVICES, MO_LIMIT_STRING_SIZE, MO_LIMIT_TYPES,
};
use mo_def::{ErrorCode, Opcode};

use crate::compiler::{Compiler, FunctionState, RefState, SourceKind};
use crate::control::prune;
use crate::registers::allocate_registers;

fn fn_kind_to_bytecode(kind: FnKind) -> FunctionKind {
    match kind {
        FnKind::Function => FunctionKind::Function,
        FnKind::Test => FunctionKind::Test,
        FnKind::Example => FunctionKind::Example,
        FnKind::Promise => FunctionKind::Promise,
        FnKind::Anchor => FunctionKind::Anchor,
    }
}

impl<'a> Compiler<'a> {
    fn count_lambdas(&mut self, root: ExprId) -> bool {
        let mut stack = vec![(root, 0u32)];
        while let Some((expr_id, depth)) = stack.pop() {
            debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
            let next = depth + 1;
            match self.arena.expr(expr_id) {
                Expr::Lam { body, .. } => {
                    if u64::from(self.function_capacity) == MO_LIMIT_FUNCTIONS {
                        return self.fail(
                            ErrorCode::BytecodeTooBig,
                            "Lifted closures exceed MO_LIMIT_FUNCTIONS.",
                        );
                    }
                    self.function_capacity += 1;
                    stack.push((*body, next));
                }
                Expr::Let { bound, body, .. } => {
                    stack.push((*body, next));
                    stack.push((*bound, next));
                }
                Expr::App { func, args } => {
                    for &arg in args.iter().rev() {
                        stack.push((arg, next));
                    }
                    stack.push((*func, next));
                }
                Expr::Call { args, .. }
                | Expr::Tuple(args)
                | Expr::Con { args, .. }
                | Expr::Prim { args, .. }
                | Expr::Jump { args, .. } => {
                    for &arg in args.iter().rev() {
                        stack.push((arg, next));
                    }
                }
                Expr::Case { expr, arms } => {
                    for arm in arms.iter().rev() {
                        stack.push((arm.body, next));
                    }
                    stack.push((*expr, next));
                }
                Expr::If { cond, then_, else_ } => {
                    stack.push((*else_, next));
                    stack.push((*then_, next));
                    stack.push((*cond, next));
                }
                Expr::Proj { expr, .. } | Expr::Fail(expr) => {
                    stack.push((*expr, next));
                }
                Expr::Join { body, rest, .. } => {
                    stack.push((*rest, next));
                    stack.push((*body, next));
                }
                Expr::Lit(_) | Expr::Var(_) | Expr::Global(_) | Expr::Stop(_) => {}
            }
        }
        true
    }

    pub fn declarations(&mut self) -> bool {
        let p = self.core;
        if let Some(svcs) = self.services
            && !svcs.items.is_empty()
            && svcs.data_shapes.len() != p.data.len()
        {
            return self.fail(
                ErrorCode::CodegenInvalid,
                "Service shapes differ from Core data table.",
            );
        }
        let funs_len = p.funs.len() as u64;
        let consts_len = p.consts.len() as u64;
        let data_len = p.data.len() as u64;
        if funs_len > MO_LIMIT_FUNCTIONS
            || consts_len > (MO_LIMIT_FUNCTIONS - funs_len) / 2
            || consts_len > MO_LIMIT_GLOBALS
            || data_len > MO_LIMIT_TYPES
        {
            return self.fail(
                ErrorCode::BytecodeTooBig,
                "Core and its initializer helpers exceed a bytecode declaration limit.",
            );
        }

        self.function_count = (p.funs.len() + 2 * p.consts.len()) as u32;
        self.function_capacity = self.function_count;
        for f in &p.funs {
            if !self.count_lambdas(f.body) {
                return false;
            }
        }
        for k in &p.consts {
            if !self.count_lambdas(k.body) {
                return false;
            }
        }

        let cap = self.function_capacity as usize;
        self.data = vec![NO_INDEX; p.data.len()];
        self.funs = (0..cap)
            .map(|_| FunctionDef {
                name: NO_INDEX,
                module: NO_INDEX,
                kind: FunctionKind::Function,
                spec_code: false,
                parameters: Vec::new(),
                result_type: NO_INDEX,
                capture_types: Vec::new(),
                register_types: Vec::new(),
                code: Vec::new(),
            })
            .collect();
        self.bodies = (0..cap).map(FunctionState::new).collect();
        self.debug = (0..cap).map(|_| Self::empty_debug()).collect();

        if let Metadata::Source(meta) = &p.metadata {
            if !meta.file_bytes.is_empty() {
                self.files.reserve(meta.file_bytes.len());
                for path in &meta.file_bytes {
                    if (path.len() as u64) > MO_LIMIT_STRING_SIZE {
                        return self.fail(
                            ErrorCode::BytecodeTooBig,
                            "Filename exceeds the bytecode MO_LIMIT_STRING_SIZE bound.",
                        );
                    }
                    self.files.push(path.clone());
                }
            } else {
                self.files.reserve(meta.files.len());
                for path in &meta.files {
                    if (path.len() as u64) > MO_LIMIT_STRING_SIZE {
                        return self.fail(
                            ErrorCode::BytecodeTooBig,
                            "Filename exceeds the bytecode MO_LIMIT_STRING_SIZE bound.",
                        );
                    }
                    self.files.push(path.as_bytes().to_vec());
                }
            }
        }

        // Pass 1: declare all nominal Core data types.
        for (i, d) in p.data.iter().enumerate() {
            let Some(name) = self.intern_name(&d.name) else {
                return false;
            };
            let decl = self.types.declare(TypeKind::Choice, name);
            let Some(ty_id) = self.type_result(decl) else {
                return false;
            };
            self.data[i] = ty_id;
            self.data_index.entry(d.name.clone()).or_insert(ty_id);
            if let Some(svcs) = self.services
                && !svcs.items.is_empty()
            {
                let sh = &svcs.data_shapes[i];
                if !self.record_service_shape(ty_id, sh.is_struct, sh.unnamed.clone()) {
                    return false;
                }
            }
        }

        // Pass 2: define cases and fields for all Core data types.
        for (i, d) in p.data.iter().enumerate() {
            if d.ctors.is_empty() {
                return self.fail(
                    ErrorCode::CodegenRepresentation,
                    "Empty data declarations cannot come from lowering.",
                );
            }
            let mut cases = Vec::with_capacity(d.ctors.len());
            let mut seen_ctors = HashSet::with_capacity(d.ctors.len());
            for ctor in &d.ctors {
                if !seen_ctors.insert(ctor.name.as_str()) {
                    return self.fail(
                        ErrorCode::CodegenRepresentation,
                        "Duplicate Core constructor names cannot come from lowering.",
                    );
                }
                if (ctor.fields.len() as u64) > MO_LIMIT_FIELDS {
                    return self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Constructor exceeds MO_LIMIT_FIELDS.",
                    );
                }
                let Some(cname) = self.intern_name(&ctor.name) else {
                    return false;
                };
                let cname_idx = self.string_index.get(cname.as_str()).copied().unwrap_or(0);
                let mut fields = Vec::with_capacity(ctor.fields.len());
                let mut seen_fields = HashSet::with_capacity(ctor.fields.len());
                for field in &ctor.fields {
                    if (field.name.is_empty() && ctor.fields.len() != 1)
                        || !seen_fields.insert(field.name.as_str())
                    {
                        return self.fail(
                            ErrorCode::CodegenRepresentation,
                            "Empty or duplicate named fields cannot come from lowering.",
                        );
                    }
                    let fname = if field.name.is_empty() {
                        None
                    } else {
                        let Some(s) = self.intern_name(&field.name) else {
                            return false;
                        };
                        Some(s)
                    };
                    let Some(fty) = self.codegen_type(field.ty, 0) else {
                        return false;
                    };
                    if self.error.is_some() {
                        return false;
                    }
                    let fname_idx = fname
                        .as_deref()
                        .and_then(|s| self.string_index.get(s).copied());
                    fields.push(TypeField {
                        name_idx: fname_idx,
                        name: fname,
                        ty: fty,
                    });
                }
                let unnamed = fields.len() == 1 && fields[0].name.is_none();
                cases.push(TypeCase {
                    name_idx: cname_idx,
                    name: cname,
                    fields,
                    unnamed,
                });
            }
            let def_res = self.types.define_choice(self.data[i], cases);
            if self.type_result(def_res).is_none() {
                return false;
            }
        }

        // Modules from source metadata or synthetic `%Core`.
        match &p.metadata {
            Metadata::Source(meta) => {
                for m in &meta.modules {
                    if (self.modules.len() as u64) >= MO_LIMIT_MODULES {
                        return self.fail(
                            ErrorCode::BytecodeTooBig,
                            "Code generation exceeds the corresponding bytecode table or function limit.",
                        );
                    }
                    let mod_id = self.modules.len() as u32;
                    let Some(name_id) = self.intern_string(&m.name) else {
                        return false;
                    };
                    let exports = m
                        .exposed_types
                        .iter()
                        .map(|exp| TypeExport {
                            ty: self.data[exp.index as usize],
                            contents_exposed: exp.contents_exposed,
                        })
                        .collect();
                    self.modules.push(ModuleDef {
                        name: name_id,
                        types: exports,
                        functions: m.exposed_funs.clone(),
                        globals: m.consts.clone(),
                    });
                    self.module_index.entry(m.name.clone()).or_insert(mod_id);
                }
            }
            Metadata::NoSource => {
                if !p.data.is_empty() || !p.consts.is_empty() {
                    let Some(name_id) = self.intern_string("%Core") else {
                        return false;
                    };
                    self.modules.push(ModuleDef {
                        name: name_id,
                        types: Vec::new(),
                        functions: Vec::new(),
                        globals: Vec::new(),
                    });
                    self.module_index.insert("%Core".to_string(), 0);
                }
            }
        }

        // Declare Core functions.
        for (i, fun) in p.funs.iter().enumerate() {
            self.current_name = Some(fun.name.clone());
            let fmeta = match &p.metadata {
                Metadata::Source(meta) => Some(&meta.funs[i]),
                Metadata::NoSource => None,
            };
            self.declaration = fmeta.and_then(|m| m.span);
            self.span = self.declaration;
            if !self.validate_name(&fun.name) || !self.validate_name(&fun.module) {
                return false;
            }
            if (fun.params.len() as u64) > MO_LIMIT_BYTECODE_PARAMETERS {
                return self.fail(
                    ErrorCode::BytecodeTooBig,
                    "Function exceeds MO_LIMIT_BYTECODE_PARAMETERS.",
                );
            }
            let mod_id = if let Some(&id) = self.module_index.get(fun.module.as_str()) {
                id
            } else {
                if (self.modules.len() as u64) >= MO_LIMIT_MODULES {
                    return self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Code generation exceeds the corresponding bytecode table or function limit.",
                    );
                }
                let id = self.modules.len() as u32;
                let Some(_) = self.intern_name(&fun.module) else {
                    return false;
                };
                let Some(name_id) = self.intern_string(&fun.module) else {
                    return false;
                };
                self.modules.push(ModuleDef {
                    name: name_id,
                    types: Vec::new(),
                    functions: Vec::new(),
                    globals: Vec::new(),
                });
                self.module_index.insert(fun.module.clone(), id);
                id
            };

            let full_name = if let Some(m) = fmeta {
                let Some(id) = self.intern_string(&m.name) else {
                    return false;
                };
                id
            } else {
                let total_len = fun.module.len() + 1 + fun.name.len();
                if (total_len as u64) > MO_LIMIT_STRING_SIZE {
                    return self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Generated function name exceeds its limit.",
                    );
                }
                let combined = format!("{}.{}", fun.module, fun.name);
                if self.intern_name(&combined).is_none() {
                    return false;
                }
                let Some(id) = self.intern_string(&combined) else {
                    return false;
                };
                id
            };

            let mut params = Vec::with_capacity(fun.params.len());
            for param in &fun.params {
                if !self.validate_name(&param.name) {
                    return false;
                }
                let Some(pty) = self.codegen_type(param.ty, 0) else {
                    return false;
                };
                params.push(ParameterDef {
                    ty: pty,
                    ownership: ParamOwnership::Owned,
                });
            }
            let Some(ret_ty) = self.codegen_type(fun.ret, 0) else {
                return false;
            };
            self.funs[i] = FunctionDef {
                name: full_name,
                module: mod_id,
                kind: fn_kind_to_bytecode(fun.kind),
                spec_code: fun.kind != FnKind::Function,
                parameters: params,
                result_type: ret_ty,
                capture_types: Vec::new(),
                register_types: Vec::new(),
                code: Vec::new(),
            };
            self.bodies[i].source_kind = Some(SourceKind::CoreFun(i));
            self.fun_index.entry(fun.name.clone()).or_insert(i as u32);
        }

        self.error.is_none()
    }

    pub fn setup_globals(&mut self) -> bool {
        let p = self.core;
        if p.consts.is_empty() {
            return true;
        }
        self.globals = vec![
            GlobalDef {
                ty: NO_INDEX,
                name: NO_INDEX,
                module: 0,
            };
            p.consts.len()
        ];
        self.init = vec![NO_INDEX; p.consts.len()];

        match &p.metadata {
            Metadata::Source(meta) => {
                for (mod_idx, m) in meta.modules.iter().enumerate() {
                    for &c_idx in &m.consts {
                        self.globals[c_idx as usize].module = mod_idx as u32;
                    }
                }
            }
            Metadata::NoSource => {
                if (p.consts.len() as u64) > MO_LIMIT_MODULE_NAMES {
                    return self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Source-less constants exceed MO_LIMIT_MODULE_NAMES in %Core.",
                    );
                }
                self.modules[0].globals = (0..p.consts.len() as u32).collect();
                for g in &mut self.globals {
                    g.module = 0;
                }
            }
        }

        let Some(unit_ty) = self.scalar_type(TypeKind::Unit) else {
            return false;
        };
        let Some(unit_const) = self.intern_constant(ConstantDef {
            ty: unit_ty,
            value: ConstantValue::Immediate64([0u8; 8]),
        }) else {
            return false;
        };
        self.unit_constant = unit_const;

        for (i, k) in p.consts.iter().enumerate() {
            self.current_name = Some(k.name.clone());
            let cmeta = match &p.metadata {
                Metadata::Source(meta) => Some(&meta.consts[i]),
                Metadata::NoSource => None,
            };
            self.declaration = cmeta.and_then(|m| m.span);
            self.span = self.declaration;
            if !self.validate_name(&k.name) {
                return false;
            }
            let full_name = cmeta.map_or(k.name.as_str(), |m| m.name.as_str());
            let Some(g_name) = self.intern_string(full_name) else {
                return false;
            };
            let Some(g_ty) = self.codegen_type(k.ty, 0) else {
                return false;
            };
            self.globals[i].name = g_name;
            self.globals[i].ty = g_ty;

            let value_idx = p.funs.len() + 2 * i;
            let init_idx = value_idx + 1;
            self.init[i] = init_idx as u32;

            let Some(val_name) = self.intern_suffixed_name(full_name, ".init.%value") else {
                return false;
            };
            let Some(init_name) = self.intern_suffixed_name(full_name, ".init") else {
                return false;
            };
            let mod_id = self.globals[i].module;
            self.funs[value_idx] = FunctionDef {
                name: val_name,
                module: mod_id,
                kind: FunctionKind::Function,
                spec_code: false,
                parameters: Vec::new(),
                result_type: g_ty,
                capture_types: Vec::new(),
                register_types: Vec::new(),
                code: Vec::new(),
            };
            self.funs[init_idx] = FunctionDef {
                name: init_name,
                module: mod_id,
                kind: FunctionKind::Init,
                spec_code: false,
                parameters: Vec::new(),
                result_type: unit_ty,
                capture_types: Vec::new(),
                register_types: Vec::new(),
                code: Vec::new(),
            };
            self.bodies[value_idx].source_kind = Some(SourceKind::CoreConst(i));
            self.bodies[init_idx].source_kind = Some(SourceKind::CoreConst(i));
            self.global_index.entry(k.name.clone()).or_insert(i as u32);
        }
        self.error.is_none()
    }

    pub fn prepare_bodies(&mut self) -> bool {
        let declared = self.function_count as usize;
        for i in 0..declared {
            self.current_name = Some(self.source_name_for(i).to_string());
            let decl = match &self.core.metadata {
                Metadata::Source(meta) => match self.bodies[i].source_kind.as_ref() {
                    Some(SourceKind::CoreFun(idx)) => meta.funs[*idx].span,
                    Some(SourceKind::CoreConst(idx)) => meta.consts[*idx].span,
                    None => None,
                },
                Metadata::NoSource => None,
            };
            self.declaration = decl;
            self.span = decl;
            self.bodies[i].declaration = decl;
            self.bodies[i].root = i;

            if self.funs[i].kind == FunctionKind::Init {
                continue;
            }

            if let Some(bm) = self.body_meta_for(i) {
                for b in &bm.bindings {
                    let Some(name_id) = self.intern_string(&b.name) else {
                        return false;
                    };
                    self.bodies[i]
                        .bindings
                        .entry((b.node, b.slot))
                        .or_insert(name_id);
                }
                for cl in &bm.closures {
                    let Some(name_id) = self.intern_string(&cl.name) else {
                        return false;
                    };
                    self.bodies[i]
                        .closure_names
                        .entry(cl.node)
                        .or_insert(name_id);
                }
            }

            let pcount = self.funs[i].parameters.len();
            for j in 0..pcount {
                let original = match self.body_meta_for(i).and_then(|bm| bm.params[j].as_deref()) {
                    Some(pname) => {
                        let Some(id) = self.intern_string(pname) else {
                            return false;
                        };
                        id
                    }
                    None => NO_INDEX,
                };
                let pty = self.funs[i].parameters[j].ty;
                if self.add_register(i, pty, original).is_none() {
                    return false;
                }
            }
            if let Some(fun) = self.source_fun_for(i) {
                for (j, param) in fun.params.iter().enumerate().rev() {
                    self.bodies[i].locals.insert(param.name.clone(), j as u32);
                }
            }

            let body_expr = self.source_body_for(i);
            let expected = self.funs[i].result_type;
            let Some(body_node) = self.prepare_expr(i, body_expr, expected, 0) else {
                return false;
            };
            self.bodies[i].body = Some(body_node);
        }

        let fin = self.types.finish();
        if self.type_result(fin).is_none() {
            return false;
        }

        let total_funs = self.function_count as usize;
        for i in declared..total_funs {
            let fields = self.funs[i].capture_types.clone();
            let ty = self.bodies[i].closure_type;
            let res = self.types.add_layout(LayoutDef {
                kind: LayoutKind::Closure,
                ty,
                case_index: NO_INDEX,
                function: i as u32,
                fields,
            });
            if self.type_result(res).is_none() {
                return false;
            }
        }

        let type_count = self.types.items.len();
        self.layouts = vec![NO_INDEX; type_count];
        for i in 0..type_count {
            let kind = self.types.items[i].kind;
            match kind {
                TypeKind::Tuple => {
                    let fields = self.types.items[i].parameters.clone();
                    let res = self.types.add_layout(LayoutDef {
                        kind: LayoutKind::Tuple,
                        ty: i as u32,
                        case_index: NO_INDEX,
                        function: NO_INDEX,
                        fields,
                    });
                    let Some(lid) = self.type_result(res) else {
                        return false;
                    };
                    self.layouts[i] = lid;
                }
                TypeKind::Record => {
                    let fields = self.types.items[i].fields.iter().map(|f| f.ty).collect();
                    let res = self.types.add_layout(LayoutDef {
                        kind: LayoutKind::Record,
                        ty: i as u32,
                        case_index: NO_INDEX,
                        function: NO_INDEX,
                        fields,
                    });
                    let Some(lid) = self.type_result(res) else {
                        return false;
                    };
                    self.layouts[i] = lid;
                }
                TypeKind::Choice => {
                    let case_count = self.types.items[i].cases.len();
                    for j in 0..case_count {
                        if self.types.case_shape(i as u32, j as u32) != CaseShape::Boxed {
                            continue;
                        }
                        let fields = self.types.items[i].cases[j]
                            .fields
                            .iter()
                            .map(|f| f.ty)
                            .collect();
                        let res = self.types.add_layout(LayoutDef {
                            kind: LayoutKind::Case,
                            ty: i as u32,
                            case_index: j as u32,
                            function: NO_INDEX,
                            fields,
                        });
                        if self.type_result(res).is_none() {
                            return false;
                        }
                    }
                }
                _ => {}
            }
        }
        true
    }

    pub fn entry_valid(&mut self) -> bool {
        let Some(&entry_idx) = self.fun_index.get(self.core.entry.as_str()) else {
            return self.fail(
                ErrorCode::CodegenEntry,
                "The entry's full runtime name is not unique in the image.",
            );
        };
        self.current_name = Some(self.core.funs[entry_idx as usize].name.clone());
        let decl = match &self.core.metadata {
            Metadata::Source(meta) => meta.funs[entry_idx as usize].span,
            Metadata::NoSource => None,
        };
        self.declaration = decl;
        self.span = decl;
        let full_name = self.funs[entry_idx as usize].name;
        for i in 0..self.function_count {
            if i != entry_idx && self.funs[i as usize].name == full_name {
                return self.fail(
                    ErrorCode::CodegenEntry,
                    "The entry's full runtime name is not unique in the image.",
                );
            }
        }
        true
    }

    fn emit_initializer(&mut self, func_idx: usize) -> bool {
        let global = ((func_idx - self.core.funs.len()) / 2) as u32;
        let g_ty = self.globals[global as usize].ty;
        let ret_ty = self.funs[func_idx].result_type;
        let Some(value) = self.add_register(func_idx, g_ty, NO_INDEX) else {
            return false;
        };
        let Some(unit) = self.add_register(func_idx, ret_ty, NO_INDEX) else {
            return false;
        };
        let source = self
            .body_meta_for(func_idx)
            .and_then(|bm| bm.spans.first().copied().flatten());
        let decl = self.declaration;
        self.emit(
            func_idx,
            source,
            Opcode::Call,
            &[value, (func_idx - 1) as u32],
            &[],
        )
        .is_some()
            && self
                .emit(func_idx, decl, Opcode::SetGlobal, &[global, value], &[])
                .is_some()
            && self
                .emit(
                    func_idx,
                    decl,
                    Opcode::Const,
                    &[unit, self.unit_constant],
                    &[],
                )
                .is_some()
            && self
                .emit(func_idx, decl, Opcode::Ret, &[unit], &[])
                .is_some()
    }

    fn build_service_descriptors(&mut self) -> Option<Vec<ServiceDef>> {
        let Some(svcs) = self.services else {
            return Some(Vec::new());
        };
        if svcs.items.is_empty() {
            return Some(Vec::new());
        }
        self.current_name = None;
        self.declaration = None;
        self.span = None;
        if (svcs.items.len() as u64) > MO_LIMIT_SERVICES {
            self.fail(
                ErrorCode::BytecodeTooBig,
                "Service count exceeds MO_LIMIT_SERVICES.",
            );
            return None;
        }
        let core_fun_count = self.core.funs.len() as u32;
        let mut out = Vec::with_capacity(svcs.items.len());
        for s in &svcs.items {
            if s.start >= core_fun_count {
                self.current_name = None;
                self.declaration = None;
                self.span = None;
                self.fail(
                    ErrorCode::CodegenInvalid,
                    "Service function index is out of range.",
                );
                return None;
            }
            let start_idx = s.start as usize;
            self.current_name = Some(self.source_name_for(start_idx).to_string());
            self.declaration = self.bodies[start_idx].declaration;
            self.span = self.declaration;
            if (s.tools.len() as u64) > MO_LIMIT_SERVICE_HANDLERS
                || (s.timers.len() as u64) > MO_LIMIT_SERVICE_HANDLERS - (s.tools.len() as u64)
            {
                self.fail(
                    ErrorCode::BytecodeTooBig,
                    "Service handler count exceeds MO_LIMIT_SERVICE_HANDLERS.",
                );
                return None;
            }
            let token_env = self.intern_string(&s.token_env)?;
            let mut tools = Vec::with_capacity(s.tools.len());
            for t in &s.tools {
                if t.function >= core_fun_count
                    || self.funs[t.function as usize].parameters.len() < 2
                {
                    self.fail(
                        ErrorCode::CodegenInvalid,
                        "Invalid service handler signature.",
                    );
                    return None;
                }
                let fn_def = &self.funs[t.function as usize];
                let count = fn_def.parameters.len() - 2;
                if count != t.argument_names.len() {
                    self.fail(
                        ErrorCode::CodegenInvalid,
                        "Invalid service handler signature.",
                    );
                    return None;
                }
                let arg_types: Vec<u32> = fn_def.parameters[2..].iter().map(|p| p.ty).collect();
                let res_ty = fn_def.result_type;
                let mut arg_names = Vec::with_capacity(count);
                for aname in &t.argument_names {
                    arg_names.push(self.intern_string(aname)?);
                }
                let tname = self.intern_string(&t.name)?;
                let desc = self.intern_string(&t.description)?;
                tools.push(ServiceTool {
                    name: tname,
                    function: t.function,
                    read_only: t.read_only,
                    argument_types: arg_types,
                    result_type: res_ty,
                    description: desc,
                    argument_names: arg_names,
                });
            }
            let mut timers = Vec::with_capacity(s.timers.len());
            for timer in &s.timers {
                if timer.function >= core_fun_count || timer.interval >= core_fun_count {
                    self.fail(
                        ErrorCode::CodegenInvalid,
                        "Service function index is out of range.",
                    );
                    return None;
                }
                timers.push(ServiceTimer {
                    function: timer.function,
                    interval: timer.interval,
                });
            }
            out.push(ServiceDef {
                module: self.funs[start_idx].module,
                state_type: self.funs[start_idx].result_type,
                start: s.start,
                token_env,
                tools,
                timers,
                shapes: Vec::new(),
            });
        }
        self.current_name = None;
        self.declaration = None;
        self.span = None;

        // Populate reachable shapes via mo_bytecode::validate::service_reach (Tarjan SCC).
        let mut by_type: Vec<Option<usize>> = vec![None; self.types.items.len()];
        for (idx, sh) in self.shapes.iter().enumerate() {
            debug_assert!((sh.ty as usize) < by_type.len() && by_type[sh.ty as usize].is_none());
            by_type[sh.ty as usize] = Some(idx);
        }
        let reachable = service_reach(&self.types.items, &out);
        for (svc, reach) in out.iter_mut().zip(reachable) {
            svc.shapes = Vec::with_capacity(reach.len());
            for ty in reach {
                let Some(sh_idx) = by_type[ty as usize] else {
                    self.fail(ErrorCode::CodegenInvalid, "missing reachable service shape");
                    return None;
                };
                svc.shapes.push(self.shapes[sh_idx].clone());
            }
        }
        Some(out)
    }

    pub fn emit_all(&mut self) -> Option<Vec<u8>> {
        let count = self.function_count as usize;
        // Snapshot finalized parameter ownership for register allocation.
        let param_ownership: Vec<Vec<ParamOwnership>> = self.funs[..count]
            .iter()
            .map(|f| f.parameters.iter().map(|p| p.ownership).collect())
            .collect();

        for i in 0..count {
            self.current_name = Some(self.source_name_for(i).to_string());
            let decl = self.bodies[i].declaration;
            self.declaration = decl;
            let kind = self.funs[i].kind;
            if kind != FunctionKind::Init && !self.live_infer(i) {
                return None;
            }
            self.span = decl;

            let pcount = self.funs[i].parameters.len();
            for j in 0..pcount {
                let p = self.funs[i].parameters[j];
                if self.is_reference(p.ty) {
                    let owned = p.ownership == ParamOwnership::Owned;
                    self.bodies[i].regs[j].state = if owned {
                        RefState::Owned
                    } else {
                        RefState::Borrowed
                    };
                    if owned {
                        self.owner_add(i, j as u32);
                    }
                }
            }
            let is_closure = kind == FunctionKind::Closure;
            let inputs = pcount + usize::from(is_closure);
            if is_closure {
                self.bodies[i].regs[pcount].state = RefState::Owned;
                self.owner_add(i, pcount as u32);
            }
            let initial_regs = self.bodies[i].regs.len();
            for j in inputs..initial_regs {
                if !self.initialize_register(i, j as u32, 0) {
                    return None;
                }
            }
            let capture_start = self.bodies[i].code.len() as u32;
            let cap_regs: Vec<u32> = self.bodies[i].captures.iter().map(|c| c.reg).collect();
            for (j, &r) in cap_regs.iter().enumerate() {
                self.emit(i, decl, Opcode::Field, &[r, pcount as u32, j as u32], &[])?;
                if self.is_reference(self.bodies[i].regs[r as usize].ty) {
                    self.owner_add(i, r);
                }
            }

            if kind == FunctionKind::Init {
                if !self.emit_initializer(i) {
                    return None;
                }
            } else if let Some(body_node) = self.bodies[i].body {
                let mut live = self.reachable_walk(i, body_node, 0);
                if !self.generate(i, body_node, true, 0, &mut live) {
                    return None;
                }
                debug_assert!(!live);
            }

            let end_pc = self.bodies[i].code.len() as u32;
            for j in 0..pcount {
                if !self.record_named(i, j as u32, 0, end_pc) {
                    return None;
                }
            }
            for (j, &r) in cap_regs.iter().enumerate() {
                if !self.record_named(i, r, capture_start + 4 * (j as u32), end_pc) {
                    return None;
                }
            }

            self.funs[i].register_types = self.bodies[i].regs.iter().map(|r| r.ty).collect();
            self.funs[i].code = std::mem::take(&mut self.bodies[i].code);

            let decl_span = self.codegen_span(decl);
            let mut dbg = Self::empty_debug();
            dbg.spans = std::mem::take(&mut self.bodies[i].spans);
            dbg.names = std::mem::take(&mut self.bodies[i].names);
            dbg.file = decl_span.file;
            dbg.start = decl_span.start;
            dbg.end = decl_span.end;

            if !is_closure && let Some(fmeta) = self.fn_meta_for(i) {
                if let Some(txt) = fmeta.text.as_deref() {
                    dbg.text = self.intern_string(txt)?;
                }
                if let Some(subj) = fmeta.subject {
                    dbg.subject = subj;
                    if kind == FunctionKind::Promise {
                        let subject = &self.funs[subj as usize];
                        let matches = self.funs[i].parameters.len() == subject.parameters.len() + 1
                            && subject
                                .parameters
                                .iter()
                                .enumerate()
                                .all(|(j, sp)| self.funs[i].parameters[j].ty == sp.ty)
                            && self.funs[i].parameters[subject.parameters.len()].ty
                                == subject.result_type;
                        if !matches {
                            self.fail(
                                ErrorCode::CodegenRepresentation,
                                "Promise metadata parameters differ from its subject.",
                            );
                            return None;
                        }
                    }
                }
                if let Some(allows) = fmeta.allows {
                    dbg.expectation = if allows {
                        AnchorExpectation::Allows
                    } else {
                        AnchorExpectation::Forbids
                    };
                }
                let mut lines = Vec::with_capacity(fmeta.lines.len());
                for line in &fmeta.lines {
                    let ls = self.codegen_span(line.span);
                    let lt = self.intern_string(&line.text)?;
                    if !self.debug_add() {
                        return None;
                    }
                    lines.push(SpecLine {
                        file: ls.file,
                        start: ls.start,
                        end: ls.end,
                        text: lt,
                    });
                }
                dbg.lines = lines;
            }
            if self.error.is_some() {
                return None;
            }

            prune(&mut self.funs[i], &mut dbg);
            if let Err(code) = allocate_registers(
                &self.types.items,
                &param_ownership,
                &mut self.funs[i],
                &mut dbg,
                capture_start,
            ) {
                let reason = match code {
                    ErrorCode::TooManyRegisters => {
                        "Function exceeds MO_LIMIT_FUNCTION_REGISTERS after recycling."
                    }
                    ErrorCode::BytecodeTooBig => {
                        "Function exceeds MO_LIMIT_FUNCTION_REGISTER_BLOCKS after recycling."
                    }
                    ErrorCode::FunctionTooLong => {
                        "Recycled code exceeds MO_LIMIT_FUNCTION_INSTRUCTIONS or MO_LIMIT_FUNCTION_CODE_WORDS."
                    }
                    ErrorCode::CodegenRegisterWork => {
                        "Register recycling exceeds MO_LIMIT_CODEGEN_REGISTER_WORK."
                    }
                    _ => "No memory for code generation's register recycling pass.",
                };
                self.fail(code, reason);
                return None;
            }
            self.debug[i] = dbg;
        }

        self.current_name = None;
        self.declaration = None;
        self.span = None;

        let services = self.build_service_descriptors()?;
        let flags = if services.is_empty() {
            BYTECODE_FLAGS_NONE
        } else {
            BYTECODE_FLAGS_SERVICES
        };

        let funs = self.funs.drain(..count).collect();
        let debug = self.debug.drain(..count).collect();
        let file = BytecodeFile {
            header: Header {
                magic: BYTECODE_MAGIC,
                major: BYTECODE_MAJOR,
                minor: BYTECODE_MINOR,
                flags,
                checksum: 0,
            },
            program_checksum: 0,
            strings: std::mem::take(&mut self.strings),
            types: TypeTable::from_raw(
                std::mem::take(&mut self.types.items),
                std::mem::take(&mut self.types.layouts),
            ),
            constants: std::mem::take(&mut self.constants),
            globals: std::mem::take(&mut self.globals),
            init: std::mem::take(&mut self.init),
            functions: funs,
            code_offsets: Vec::new(),
            modules: std::mem::take(&mut self.modules),
            files: std::mem::take(&mut self.files),
            debug,
            services,
        };

        match write_bytecode(&file) {
            Ok(bytes) => Some(bytes),
            Err(err) => {
                if let Some(fidx) = err.function
                    && (fidx as usize) < self.bodies.len()
                {
                    let idx = fidx as usize;
                    self.current_name = Some(self.source_name_for(idx).to_string());
                    self.declaration = self.bodies[idx].declaration;
                    self.span = self.declaration;
                }
                let code = match err.kind {
                    ErrorKind::NoMemory => ErrorCode::CodegenMemory,
                    ErrorKind::Limit => ErrorCode::BytecodeTooBig,
                    ErrorKind::Invalid => ErrorCode::CodegenInvalid,
                };
                self.fail(code, err.reason);
                None
            }
        }
    }
}
