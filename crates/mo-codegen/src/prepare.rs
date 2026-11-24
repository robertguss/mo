//! Core expression preparation, closure lifting, capture resolution, and missing-context
//! back-propagation matching `src/codegen/prepare.c`.

use mo_bytecode::{
    ConstantDef, ConstantValue, FunctionDef, FunctionKind, NO_INDEX, ParamOwnership, ParameterDef,
    TypeKind,
};
use mo_core::{Expr, ExprId, ListOp, Prim};
use mo_def::limits::{
    MO_LIMIT_CORE_DEPTH, MO_LIMIT_CORE_NODES, MO_LIMIT_FIELDS, MO_LIMIT_PARAMETERS,
};
use mo_def::{ErrorCode, Opcode, PrimId};

use crate::compiler::{Capture, Compiler, FunctionState, JoinState, LiveSet, Node, NodeId};
use crate::operations::{Expansion, LiteralOp, Operation, primitive_op, resolve_operation};
use crate::types::bare_type;

impl<'a> Compiler<'a> {
    fn binding_name(&self, func_idx: usize, node: u32, slot: u32) -> u32 {
        self.bodies[func_idx]
            .bindings
            .get(&(node, slot))
            .copied()
            .unwrap_or(NO_INDEX)
    }

    fn resolve_variable(&mut self, func_idx: usize, name: &str, depth: u32) -> Option<u32> {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        if let Some(&r) = self.bodies[func_idx].locals.get(name)
            && r != NO_INDEX
        {
            return Some(r);
        }
        let parent_idx = self.bodies[func_idx].parent?;
        let source = self.resolve_variable(parent_idx, name, depth + 1)?;
        let r = if let Some(&old) = self.bodies[func_idx].capture_index.get(&source) {
            old
        } else {
            let parent_reg = self.bodies[parent_idx].regs[source as usize];
            let reg = self.add_register(func_idx, parent_reg.ty, parent_reg.name)?;
            if (self.bodies[func_idx].captures.len() as u64) >= MO_LIMIT_FIELDS {
                self.fail(
                    ErrorCode::BytecodeTooBig,
                    "Code generation exceeds the corresponding bytecode table or function limit.",
                );
                return None;
            }
            self.bodies[func_idx].captures.push(Capture { source, reg });
            self.bodies[func_idx].capture_index.insert(source, reg);
            reg
        };
        self.bodies[func_idx].locals.insert(name.to_string(), r);
        Some(r)
    }

    fn resolve_context(
        &mut self,
        func_idx: usize,
        node_id: NodeId,
        expected: u32,
        depth: u32,
    ) -> bool {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        if expected == NO_INDEX || self.node(node_id).ty != NO_INDEX {
            return true;
        }
        let core_id = self.node(node_id).core;
        match self.arena.expr(core_id) {
            Expr::Stop(_) | Expr::Fail(_) | Expr::Jump { .. } => return true,
            Expr::Lam { .. } => {
                let lifted_idx = self.node(node_id).index as usize;
                let result = self.types.items[expected as usize].result;
                self.funs[lifted_idx].result_type = result;
                self.bodies[lifted_idx].closure_type = expected;
                if let Some(body_id) = self.bodies[lifted_idx].body
                    && !self.resolve_context(lifted_idx, body_id, result, depth + 1)
                {
                    return false;
                }
            }
            Expr::Let { .. } => {
                let child1 = self.node(node_id).children[1];
                if !self.resolve_context(func_idx, child1, expected, depth + 1) {
                    return false;
                }
                let child_ty = self.node(child1).ty;
                let child_reg = self.node(child1).reg;
                let child_tmp = self.node(child1).temporary;
                let bind0 = self.node(node_id).binds[0];
                let n = self.node_mut(node_id);
                n.ty = child_ty;
                n.reg = child_reg;
                n.temporary = child_tmp || child_reg == bind0;
                return true;
            }
            Expr::Join { .. } => {
                let children = self.node(node_id).children.clone();
                for child in children {
                    if !self.resolve_context(func_idx, child, expected, depth + 1) {
                        return false;
                    }
                }
            }
            Expr::If { .. } => {
                let children = self.node(node_id).children.clone();
                for &child in &children[1..] {
                    if !self.resolve_context(func_idx, child, expected, depth + 1) {
                        return false;
                    }
                }
            }
            Expr::Case { .. } => {
                let arms = self.node(node_id).children.clone();
                for &arm in &arms[1..] {
                    let body = self.node(arm).children[0];
                    if !self.resolve_context(func_idx, body, expected, depth + 1) {
                        return false;
                    }
                }
            }
            _ => {
                debug_assert!(false, "unexpected unresolved expression tag");
                return false;
            }
        }
        let Some(reg) = self.add_register(func_idx, expected, NO_INDEX) else {
            return false;
        };
        let n = self.node_mut(node_id);
        n.ty = expected;
        n.reg = reg;
        true
    }

    fn prepare_lambda(
        &mut self,
        parent_idx: usize,
        node_id: NodeId,
        preorder: u32,
        expected: u32,
        depth: u32,
    ) -> bool {
        debug_assert!(self.function_count < self.function_capacity);
        let index = self.function_count as usize;
        self.function_count += 1;
        self.node_mut(node_id).index = index as u32;

        let root = self.bodies[parent_idx].root;
        let source_kind = self.bodies[parent_idx].source_kind.clone();
        let parent_preorder = self.bodies[parent_idx].preorder;
        let parent_bindings = self.bodies[parent_idx].bindings.clone();
        let span = self.node(node_id).span;

        let mut lifted = FunctionState::new(root);
        lifted.parent = Some(parent_idx);
        lifted.source_kind = source_kind;
        lifted.preorder = parent_preorder;
        lifted.bindings = parent_bindings;
        lifted.declaration = span;
        self.bodies[index] = lifted;

        self.bodies[root].lambda_number += 1;
        let number = self.bodies[root].lambda_number;
        let named_closure = self.bodies[root].closure_names.get(&preorder).copied();
        let name_id = match named_closure {
            Some(id) => id,
            None => {
                let suffix = format!(".fn.{number}");
                let base = self.source_name_for(index);
                let Some(id) = self.intern_suffixed_name(base, &suffix) else {
                    return false;
                };
                id
            }
        };

        let core_id = self.node(node_id).core;
        let Expr::Lam { params, body } = self.arena.expr(core_id) else {
            return false;
        };
        if (params.len() as u64) > MO_LIMIT_PARAMETERS {
            return self.fail(
                ErrorCode::BytecodeTooBig,
                "Closure exceeds MO_LIMIT_PARAMETERS.",
            );
        }

        let mut bc_params = Vec::with_capacity(params.len());
        let mut param_tys = Vec::with_capacity(params.len());
        for (i, p) in params.iter().enumerate() {
            if !self.validate_name(&p.name) {
                return false;
            }
            let Some(ty) = self.codegen_type(p.ty, 0) else {
                return false;
            };
            param_tys.push(ty);
            bc_params.push(ParameterDef {
                ty,
                ownership: ParamOwnership::Owned,
            });
            let orig = self.binding_name(parent_idx, preorder, i as u32);
            if self.add_register(index, ty, orig).is_none() {
                return false;
            }
        }

        let parent_module = self.funs[parent_idx].module;
        let parent_spec = self.funs[parent_idx].spec_code;
        self.funs[index] = FunctionDef {
            name: name_id,
            module: parent_module,
            kind: FunctionKind::Closure,
            spec_code: parent_spec,
            parameters: bc_params,
            result_type: NO_INDEX,
            capture_types: Vec::new(),
            register_types: Vec::new(),
            code: Vec::new(),
        };

        let mut env_def = bare_type(TypeKind::Env);
        env_def.function = index as u32;
        let Some(env_ty) = self.intern_type(env_def) else {
            return false;
        };
        if self.add_register(index, env_ty, NO_INDEX).is_none() {
            return false;
        }

        for (i, p) in params.iter().enumerate().rev() {
            self.bodies[index].locals.insert(p.name.clone(), i as u32);
        }

        let mut result = if expected == NO_INDEX {
            NO_INDEX
        } else {
            self.types.items[expected as usize].result
        };
        let body_node = self.prepare_expr(index, *body, result, depth + 1);
        self.bodies[parent_idx].preorder = self.bodies[index].preorder;
        let Some(body_node) = body_node else {
            return false;
        };
        self.bodies[index].body = Some(body_node);
        if result == NO_INDEX {
            result = self.node(body_node).ty;
        }
        self.funs[index].result_type = result;

        let closure_ty = if result != NO_INDEX {
            let mut fn_def = bare_type(TypeKind::Function);
            fn_def.parameters = param_tys;
            fn_def.result = result;
            let Some(ty) = self.intern_type(fn_def) else {
                return false;
            };
            ty
        } else {
            NO_INDEX
        };

        let cap_count = self.bodies[index].captures.len();
        let mut capture_types = Vec::with_capacity(cap_count);
        let mut binds = Vec::with_capacity(cap_count);
        for cap in &self.bodies[index].captures {
            binds.push(cap.source);
            capture_types.push(self.bodies[index].regs[cap.reg as usize].ty);
        }
        self.funs[index].capture_types = capture_types;
        self.bodies[index].closure_type = closure_ty;
        let n = self.node_mut(node_id);
        n.ty = closure_ty;
        n.binds = binds;
        true
    }

    fn prepare_children(
        &mut self,
        func_idx: usize,
        node_id: NodeId,
        xs: &[ExprId],
        expected: u32,
        depth: u32,
    ) -> bool {
        let mut children = Vec::with_capacity(xs.len());
        let core_id = self.node(node_id).core;
        for (i, &child_expr) in xs.iter().enumerate() {
            let child_type = match self.arena.expr(core_id) {
                Expr::Call { .. } => {
                    let callee = self.node(node_id).index as usize;
                    self.funs[callee].parameters[i].ty
                }
                Expr::Jump { .. } => {
                    let join_idx = self.node(node_id).index as usize;
                    let target_node = self.bodies[func_idx].joins[join_idx].node;
                    let bind_reg = self.node(target_node).binds[i] as usize;
                    self.bodies[func_idx].regs[bind_reg].ty
                }
                Expr::Con { .. } => {
                    let ty = self.node(node_id).ty as usize;
                    let case_idx = self.node(node_id).index as usize;
                    let t = &self.types.items[ty];
                    if t.kind == TypeKind::Record {
                        t.fields[i].ty
                    } else {
                        t.cases[case_idx].fields[i].ty
                    }
                }
                Expr::If { .. } => {
                    if i == 0 {
                        let Some(b) = self.scalar_type(TypeKind::Bool) else {
                            return false;
                        };
                        b
                    } else {
                        expected
                    }
                }
                Expr::Prim { .. } => {
                    let n = self.node(node_id);
                    let slot = match n.operation.literal {
                        LiteralOp::List => 1,
                        LiteralOp::Map => 1 + (i % 2),
                        LiteralOp::None => i,
                    };
                    self.types.items[n.signature as usize].parameters[slot]
                }
                _ => NO_INDEX,
            };
            let Some(child_id) = self.prepare_expr(func_idx, child_expr, child_type, depth + 1)
            else {
                return false;
            };
            children.push(child_id);
        }
        self.node_mut(node_id).children = children;
        true
    }

    pub fn prepare_expr(
        &mut self,
        func_idx: usize,
        expr_id: ExprId,
        expected: u32,
        depth: u32,
    ) -> Option<NodeId> {
        if u64::from(depth) >= MO_LIMIT_CORE_DEPTH {
            self.fail(
                ErrorCode::NestedTooDeeply,
                "Core expression nesting exceeds MO_LIMIT_CORE_DEPTH.",
            );
            return None;
        }
        let preorder = self.bodies[func_idx].preorder;
        self.bodies[func_idx].preorder += 1;
        let span = self
            .body_meta_for(func_idx)
            .and_then(|bm| bm.spans.get(preorder as usize).copied().flatten());
        self.span = span;

        let node_id = self.alloc_node(Node {
            core: expr_id,
            span,
            ty: NO_INDEX,
            reg: NO_INDEX,
            index: NO_INDEX,
            children: Vec::new(),
            binds: Vec::new(),
            cases: Vec::new(),
            operation: Operation::default(),
            signature: NO_INDEX,
            last: Vec::new(),
            after: LiveSet::default(),
            work: LiveSet::default(),
            temporary: true,
            scrutinee_dead: false,
        });

        match self.arena.expr(expr_id) {
            Expr::Lit(lit) => {
                let (ty, idx) = self.intern_literal(lit)?;
                let n = self.node_mut(node_id);
                n.ty = ty;
                n.index = idx;
            }
            Expr::Var(name) => {
                let reg = self.resolve_variable(func_idx, name, 0)?;
                let ty = self.bodies[func_idx].regs[reg as usize].ty;
                let n = self.node_mut(node_id);
                n.reg = reg;
                n.ty = ty;
                n.temporary = false;
                return Some(node_id);
            }
            Expr::Global(name) => {
                let idx = *self.global_index.get(name.as_str())?;
                let ty = self.globals[idx as usize].ty;
                let n = self.node_mut(node_id);
                n.index = idx;
                n.ty = ty;
            }
            Expr::Let { name, bound, body } => {
                if !self.validate_name(name) {
                    return None;
                }
                let child0 = self.prepare_expr(func_idx, *bound, NO_INDEX, depth + 1)?;
                let child0_ty = self.node(child0).ty;
                let orig = self.binding_name(func_idx, preorder, 0);
                let bind0 = self.add_register(func_idx, child0_ty, orig)?;
                let saved = self.bodies[func_idx].locals.insert(name.clone(), bind0);
                let child1_opt = self.prepare_expr(func_idx, *body, expected, depth + 1);
                match saved {
                    Some(prev) => {
                        self.bodies[func_idx].locals.insert(name.clone(), prev);
                    }
                    None => {
                        self.bodies[func_idx].locals.remove(name.as_str());
                    }
                }
                let child1 = child1_opt?;
                let c1_ty = self.node(child1).ty;
                let c1_reg = self.node(child1).reg;
                let c1_tmp = self.node(child1).temporary;
                let n = self.node_mut(node_id);
                n.children = vec![child0, child1];
                n.binds = vec![bind0];
                n.ty = c1_ty;
                n.reg = c1_reg;
                n.temporary = c1_tmp || c1_reg == bind0;
                return Some(node_id);
            }
            Expr::Call { name, args } => {
                let idx = *self.fun_index.get(name.as_str())?;
                let ret_ty = self.funs[idx as usize].result_type;
                let n = self.node_mut(node_id);
                n.index = idx;
                n.ty = ret_ty;
                if !self.prepare_children(func_idx, node_id, args, NO_INDEX, depth) {
                    return None;
                }
            }
            Expr::If { cond, then_, else_ } => {
                let list = [*cond, *then_, *else_];
                if !self.prepare_children(func_idx, node_id, &list, expected, depth) {
                    return None;
                }
                let c1 = self.node(node_id).children[1];
                let c2 = self.node(node_id).children[2];
                let mut ty = if self.node(c1).ty != NO_INDEX {
                    self.node(c1).ty
                } else {
                    self.node(c2).ty
                };
                if ty == NO_INDEX {
                    ty = expected;
                }
                self.node_mut(node_id).ty = ty;
                if !self.resolve_context(func_idx, c1, ty, depth + 1)
                    || !self.resolve_context(func_idx, c2, ty, depth + 1)
                {
                    return None;
                }
            }
            Expr::Con { ty, ctor, args } => {
                let bc_ty = self.codegen_type(*ty, 0)?;
                let case_idx = self.lookup_constructor(bc_ty, ctor)?;
                let n = self.node_mut(node_id);
                n.ty = bc_ty;
                n.index = case_idx;
                if !self.prepare_children(func_idx, node_id, args, NO_INDEX, depth) {
                    return None;
                }
            }
            Expr::Case { expr: scrut, arms } => {
                let scrut_node = self.prepare_expr(func_idx, *scrut, NO_INDEX, depth + 1)?;
                let scrut_ty = self.node(scrut_node).ty;
                debug_assert_ne!(scrut_ty, NO_INDEX);

                let mut children = Vec::with_capacity(1 + arms.len());
                children.push(scrut_node);
                let mut cases = Vec::with_capacity(arms.len());
                let mut slot = 0u32;
                let mut result_ty = NO_INDEX;

                for arm in arms {
                    let case_id = self.lookup_constructor(scrut_ty, &arm.ctor)?;
                    cases.push(case_id);
                    let field_tys: Vec<u32> = {
                        let t = &self.types.items[scrut_ty as usize];
                        if t.kind == TypeKind::Record {
                            t.fields.iter().map(|f| f.ty).collect()
                        } else {
                            t.cases[case_id as usize]
                                .fields
                                .iter()
                                .map(|f| f.ty)
                                .collect()
                        }
                    };
                    let mut regs = Vec::with_capacity(field_tys.len());
                    for (j, &fty) in field_tys.iter().enumerate() {
                        self.span = span;
                        if !self.validate_name(&arm.names[j]) {
                            return None;
                        }
                        let orig = self.binding_name(func_idx, preorder, slot);
                        slot += 1;
                        regs.push(self.add_register(func_idx, fty, orig)?);
                    }
                    let mut saved = vec![None; field_tys.len()];
                    for j in (0..field_tys.len()).rev() {
                        saved[j] = self.bodies[func_idx]
                            .locals
                            .insert(arm.names[j].clone(), regs[j]);
                    }
                    let body_opt = self.prepare_expr(func_idx, arm.body, expected, depth + 1);
                    for (prev, name) in saved.into_iter().zip(arm.names.iter()) {
                        match prev {
                            Some(prev_reg) => {
                                self.bodies[func_idx].locals.insert(name.clone(), prev_reg);
                            }
                            None => {
                                self.bodies[func_idx].locals.remove(name.as_str());
                            }
                        }
                    }
                    let body_id = body_opt?;
                    let body_clone = self.node(body_id).clone();
                    let body_ty = body_clone.ty;
                    // Arm wrapper holds pattern registers independently of its body's binders.
                    let wrapper_id = self.alloc_node(Node {
                        children: vec![body_id],
                        binds: regs,
                        cases: Vec::new(),
                        last: Vec::new(),
                        after: LiveSet::default(),
                        work: LiveSet::default(),
                        scrutinee_dead: false,
                        ..body_clone
                    });
                    children.push(wrapper_id);
                    if result_ty == NO_INDEX {
                        result_ty = body_ty;
                    }
                }

                if result_ty == NO_INDEX {
                    result_ty = expected;
                }
                for &wrapper_id in &children[1..] {
                    let body_id = self.node(wrapper_id).children[0];
                    if !self.resolve_context(func_idx, body_id, result_ty, depth + 1) {
                        return None;
                    }
                }
                let n = self.node_mut(node_id);
                n.children = children;
                n.cases = cases;
                n.ty = result_ty;
            }
            Expr::Stop(msg) => {
                let text_ty = self.scalar_type(TypeKind::Text)?;
                let sid = self.intern_string(msg)?;
                let kid = self.intern_constant(ConstantDef {
                    ty: text_ty,
                    value: ConstantValue::Text(sid),
                })?;
                let reg = self.add_register(func_idx, text_ty, NO_INDEX)?;
                let n = self.node_mut(node_id);
                n.index = kid;
                n.reg = reg;
                return Some(node_id);
            }
            Expr::Tuple(items) => {
                if items.len() < 2 {
                    self.fail(
                        ErrorCode::CodegenRepresentation,
                        "Zero/one-element tuples cannot come from lowering.",
                    );
                    return None;
                }
                if !self.prepare_children(func_idx, node_id, items, NO_INDEX, depth) {
                    return None;
                }
                let params: Vec<u32> = self
                    .node(node_id)
                    .children
                    .iter()
                    .map(|&c| self.node(c).ty)
                    .collect();
                let mut td = bare_type(TypeKind::Tuple);
                td.parameters = params;
                let ty = self.intern_type(td)?;
                self.node_mut(node_id).ty = ty;
            }
            Expr::Proj { expr: inner, index } => {
                if !self.prepare_children(func_idx, node_id, &[*inner], NO_INDEX, depth) {
                    return None;
                }
                let mut idx = 0u32;
                for &b in index.digits.as_bytes() {
                    idx = idx * 10 + u32::from(b - b'0');
                }
                let child0 = self.node(node_id).children[0];
                let tuple_ty = self.node(child0).ty as usize;
                let elem_ty = self.types.items[tuple_ty].parameters[idx as usize];
                let n = self.node_mut(node_id);
                n.index = idx;
                n.ty = elem_ty;
            }
            Expr::Fail(inner) => {
                if !self.prepare_children(func_idx, node_id, &[*inner], NO_INDEX, depth) {
                    return None;
                }
                return Some(node_id);
            }
            Expr::Prim { prim, args } => {
                if let Prim::Sim(sim) = prim {
                    let sig = self.simulation_signature(sim)?;
                    let res_ty = self.types.items[sig as usize].result;
                    let n = self.node_mut(node_id);
                    n.operation.opcode = Some(Opcode::Sim);
                    n.signature = sig;
                    n.ty = res_ty;
                    if !self.prepare_children(func_idx, node_id, args, NO_INDEX, depth) {
                        return None;
                    }
                } else {
                    let Some(op) = resolve_operation(prim) else {
                        self.fail_unsupported("The Core primitive has no code generation route.");
                        return None;
                    };
                    let mut vars = [None; 4];
                    match prim {
                        Prim::List { ty, op: list_op } => {
                            vars[0] = Some(*ty);
                            if let ListOp::Zip(u) = list_op {
                                vars[1] = Some(*u);
                            }
                        }
                        Prim::Map { key, value, .. } => {
                            vars[2] = Some(*key);
                            vars[3] = Some(*value);
                        }
                        Prim::Set { ty, .. } | Prim::Eq(ty) | Prim::Structural(ty) => {
                            vars[0] = Some(*ty);
                        }
                        _ => {}
                    }
                    let sig = self.operation_signature(&op, vars)?;
                    let res_ty = if op.compare {
                        self.scalar_type(TypeKind::Bool)?
                    } else {
                        self.types.items[sig as usize].result
                    };
                    {
                        let n = self.node_mut(node_id);
                        n.operation = op;
                        n.signature = sig;
                        n.ty = res_ty;
                    }
                    if !self.prepare_children(func_idx, node_id, args, NO_INDEX, depth) {
                        return None;
                    }
                    match op.expansion {
                        Expansion::RangeToText => {
                            let conv = primitive_op(PrimId::IntToText);
                            let cat = primitive_op(PrimId::TextConcat);
                            let sig0 = self.operation_signature(&conv, vars)?;
                            let sig1 = self.operation_signature(&cat, vars)?;
                            let int_ty = self.scalar_type(TypeKind::Int)?;
                            let sep = self.intern_string("..")?;
                            let sep_k = self.intern_constant(ConstantDef {
                                ty: res_ty,
                                value: ConstantValue::Text(sep),
                            })?;
                            let mut binds = Vec::with_capacity(6);
                            for i in 0..6 {
                                let bty = if i < 2 { int_ty } else { res_ty };
                                binds.push(self.add_register(func_idx, bty, NO_INDEX)?);
                            }
                            let n = self.node_mut(node_id);
                            n.cases = vec![sig0, sig1];
                            n.index = sep_k;
                            n.binds = binds;
                        }
                        Expansion::ListUncons => {
                            let first = primitive_op(PrimId::ListFirst);
                            let rest = primitive_op(PrimId::ListDrop);
                            let sig0 = self.operation_signature(&first, vars)?;
                            let sig1 = self.operation_signature(&rest, vars)?;
                            let option_ty = self.types.items[sig0 as usize].result;
                            let pair_ty = self.types.items[res_ty as usize].cases[1].fields[0].ty;
                            let elem_ty =
                                self.types.items[option_ty as usize].cases[1].fields[0].ty;
                            let list_ty = self.types.items[sig as usize].parameters[0];
                            let int_ty = self.types.items[sig1 as usize].parameters[1];
                            let one_k = self.intern_constant(ConstantDef {
                                ty: int_ty,
                                value: ConstantValue::Immediate64(1u64.to_le_bytes()),
                            })?;
                            let bind_tys = [option_ty, elem_ty, list_ty, int_ty, pair_ty];
                            let mut binds = Vec::with_capacity(5);
                            for bty in bind_tys {
                                binds.push(self.add_register(func_idx, bty, NO_INDEX)?);
                            }
                            let n = self.node_mut(node_id);
                            n.cases = vec![sig0, sig1];
                            n.index = one_k;
                            n.binds = binds;
                        }
                        Expansion::None => {}
                    }
                    if op.compare {
                        let ord_ty = self.types.items[sig as usize].result;
                        let r0 = self.add_register(func_idx, ord_ty, NO_INDEX)?;
                        let r1 = self.add_register(func_idx, ord_ty, NO_INDEX)?;
                        self.node_mut(node_id).binds = vec![r0, r1];
                    }
                    if op.literal != LiteralOp::None {
                        let is_map = op.literal == LiteralOp::Map;
                        let empty = primitive_op(if is_map {
                            PrimId::MapEmpty
                        } else {
                            PrimId::ListEmpty
                        });
                        let empty_sig = self.operation_signature(&empty, vars)?;
                        let bind_count =
                            self.node(node_id).children.len() / if is_map { 2 } else { 1 };
                        let mut binds = Vec::with_capacity(bind_count);
                        for _ in 0..bind_count {
                            binds.push(self.add_register(func_idx, res_ty, NO_INDEX)?);
                        }
                        let n = self.node_mut(node_id);
                        n.index = empty_sig;
                        n.binds = binds;
                    }
                }
            }
            Expr::Lam { .. } => {
                if !self.prepare_lambda(func_idx, node_id, preorder, expected, depth) {
                    return None;
                }
            }
            Expr::App { func, args } => {
                let func_node = self.prepare_expr(func_idx, *func, NO_INDEX, depth + 1)?;
                let func_ty = self.node(func_node).ty as usize;
                let res_ty = self.types.items[func_ty].result;
                let param_tys = self.types.items[func_ty].parameters.clone();
                let mut children = Vec::with_capacity(1 + args.len());
                children.push(func_node);
                for (i, &arg) in args.iter().enumerate() {
                    children.push(self.prepare_expr(func_idx, arg, param_tys[i], depth + 1)?);
                }
                let n = self.node_mut(node_id);
                n.children = children;
                n.ty = res_ty;
            }
            Expr::Join {
                name,
                params,
                body,
                rest,
            } => {
                if !self.validate_name(name) {
                    return None;
                }
                if (self.bodies[func_idx].joins.len() as u64) >= MO_LIMIT_CORE_NODES {
                    self.fail(
                        ErrorCode::CodegenMemory,
                        "Code generation exceeds its ownership graph limit.",
                    );
                    return None;
                }
                let join_idx = self.bodies[func_idx].joins.len() as u32;
                self.bodies[func_idx].joins.push(JoinState {
                    node: node_id,
                    offset: NO_INDEX,
                    checkpoint: NO_INDEX,
                    live: LiveSet::default(),
                    used: Vec::new(),
                    reachable: false,
                });
                let mut binds = Vec::with_capacity(params.len());
                for (i, p) in params.iter().enumerate() {
                    if !self.validate_name(&p.name) {
                        return None;
                    }
                    let ty = self.codegen_type(p.ty, 0)?;
                    let orig = self.binding_name(func_idx, preorder, i as u32);
                    binds.push(self.add_register(func_idx, ty, orig)?);
                }
                self.node_mut(node_id).index = join_idx;
                self.node_mut(node_id).binds = binds.clone();

                let prior_join = self.bodies[func_idx]
                    .join_names
                    .insert(name.clone(), join_idx);
                let mut saved = vec![None; params.len()];
                for i in (0..params.len()).rev() {
                    saved[i] = self.bodies[func_idx]
                        .locals
                        .insert(params[i].name.clone(), binds[i]);
                }
                let child0_opt = self.prepare_expr(func_idx, *body, expected, depth + 1);
                for i in 0..params.len() {
                    match saved[i] {
                        Some(prev) => {
                            self.bodies[func_idx]
                                .locals
                                .insert(params[i].name.clone(), prev);
                        }
                        None => {
                            self.bodies[func_idx].locals.remove(params[i].name.as_str());
                        }
                    }
                }
                let child1_opt = match child0_opt {
                    Some(_) => self.prepare_expr(func_idx, *rest, expected, depth + 1),
                    None => None,
                };
                match prior_join {
                    Some(prev) => {
                        self.bodies[func_idx].join_names.insert(name.clone(), prev);
                    }
                    None => {
                        self.bodies[func_idx].join_names.remove(name.as_str());
                    }
                }
                let (child0, child1) = (child0_opt?, child1_opt?);
                let ty = if expected != NO_INDEX {
                    expected
                } else if self.node(child1).ty != NO_INDEX {
                    self.node(child1).ty
                } else {
                    self.node(child0).ty
                };
                self.node_mut(node_id).children = vec![child0, child1];
                self.node_mut(node_id).ty = ty;
                if !self.resolve_context(func_idx, child0, ty, depth + 1)
                    || !self.resolve_context(func_idx, child1, ty, depth + 1)
                {
                    return None;
                }
            }
            Expr::Jump { name, args } => {
                let join_idx = *self.bodies[func_idx].join_names.get(name.as_str())?;
                debug_assert_ne!(join_idx, NO_INDEX);
                self.node_mut(node_id).index = join_idx;
                if !self.prepare_children(func_idx, node_id, args, NO_INDEX, depth) {
                    return None;
                }
                let target_node = self.bodies[func_idx].joins[join_idx as usize].node;
                let target_binds = self.node(target_node).binds.clone();
                let mut binds = Vec::with_capacity(args.len());
                for bind_reg in target_binds {
                    let ty = self.bodies[func_idx].regs[bind_reg as usize].ty;
                    binds.push(self.add_register(func_idx, ty, NO_INDEX)?);
                }
                self.node_mut(node_id).binds = binds;
                return Some(node_id);
            }
        }

        if self.error.is_some() {
            return None;
        }
        let ty = self.node(node_id).ty;
        if ty != NO_INDEX {
            let reg = self.add_register(func_idx, ty, NO_INDEX)?;
            self.node_mut(node_id).reg = reg;
        }
        Some(node_id)
    }
}
