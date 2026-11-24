//! Ability verification, whole-program borrow inference, and backward reference liveness
//! matching `src/codegen/ownership.c`.

use mo_bytecode::{
    ABILITY_EQUAL, ABILITY_HASH, FunctionKind, NO_INDEX, ParamOwnership, TypeKind,
    types::compute_abilities,
};
use mo_core::Expr;
use mo_def::limits::{
    MO_LIMIT_CODEGEN_LIVE_CELLS, MO_LIMIT_CODEGEN_LIVE_WORK, MO_LIMIT_CODEGEN_OWNERSHIP_EDGES,
    MO_LIMIT_CORE_DEPTH,
};
use mo_def::{ErrorCode, Opcode};

use crate::compiler::{Compiler, Edge, LiveSet, Node, NodeId};
use crate::operations::LiteralOp;

impl<'a> Compiler<'a> {
    pub fn abilities_valid(&mut self) -> bool {
        debug_assert!(self.types.finished);
        self.current_name = None;
        self.declaration = None;
        self.span = None;
        let Ok(flags) = compute_abilities(&self.types.items) else {
            return self.fail(
                ErrorCode::CodegenMemory,
                "No scratch memory for runtime ability inference.",
            );
        };
        for t in &self.types.items {
            if matches!(t.kind, TypeKind::Map | TypeKind::Set)
                && (flags[t.parameters[0] as usize] & ABILITY_HASH) == 0
            {
                return self.fail(
                    ErrorCode::CodegenRepresentation,
                    "Map keys and Set elements without Hash cannot come from lowering.",
                );
            }
        }
        for i in 0..self.constraints.len() {
            let ty = self.constraints[i].ty as usize;
            let ability = self.constraints[i].ability;
            if (flags[ty] & ability) == 0 {
                self.current_name = self.constraints[i].function_name.clone();
                self.span = self.constraints[i].span;
                self.declaration = self.constraints[i].declaration;
                return self.fail(
                    ErrorCode::CodegenRepresentation,
                    if ability == ABILITY_EQUAL {
                        "An operation on a type without Equal cannot come from lowering."
                    } else {
                        "An operation on a type without Hash cannot come from lowering."
                    },
                );
            }
        }
        true
    }

    #[must_use]
    pub fn operation_owned(n: &Node, slot: usize) -> bool {
        if n.operation.opcode == Some(Opcode::Sim) {
            return false;
        }
        n.operation.literal != LiteralOp::None
            || n.operation.params.as_bytes().get(slot) == Some(&b'o')
    }

    #[must_use]
    pub fn operand_owned(&self, node_id: NodeId, i: usize) -> bool {
        let n = self.node(node_id);
        match self.arena.expr(n.core) {
            Expr::Call { .. } => {
                self.funs[n.index as usize].parameters[i].ownership == ParamOwnership::Owned
            }
            Expr::Prim { .. } => Self::operation_owned(n, i),
            _ => true,
        }
    }

    fn seed(&mut self, id: u32) {
        if id != NO_INDEX {
            let ty = self.ownership[id as usize].ty;
            if self.is_reference(ty) {
                self.ownership[id as usize].owned = true;
            }
        }
    }

    fn depends(&mut self, from: u32, to: u32) -> bool {
        if from == NO_INDEX || to == NO_INDEX || from == to {
            return true;
        }
        let from_ty = self.ownership[from as usize].ty;
        let to_ty = self.ownership[to as usize].ty;
        if !self.is_reference(from_ty) || !self.is_reference(to_ty) {
            return true;
        }
        if (self.edges.len() as u64) >= MO_LIMIT_CODEGEN_OWNERSHIP_EDGES {
            return self.fail(
                ErrorCode::CodegenMemory,
                "Code generation exceeds its ownership graph limit.",
            );
        }
        let edge_id = self.edges.len() as u32;
        let next = self.ownership[from as usize].edges;
        self.edges.push(Edge { to, next });
        self.ownership[from as usize].edges = edge_id;
        true
    }

    #[must_use]
    pub fn reuses_ctor(&self, ty: u32, case_id: u32, node_id: NodeId, depth: u32) -> bool {
        if u64::from(depth) >= MO_LIMIT_CORE_DEPTH {
            return false;
        }
        let n = self.node(node_id);
        match self.arena.expr(n.core) {
            Expr::Con { .. } => {
                n.ty == ty
                    && (self.types.items[ty as usize].kind == TypeKind::Record
                        || n.index == case_id)
            }
            Expr::If { .. } | Expr::Case { .. } => {
                self.reuses_ctor(ty, case_id, n.children[0], depth + 1)
            }
            Expr::Join { .. } | Expr::Jump { .. } | Expr::Lam { .. } => false,
            _ => n
                .children
                .iter()
                .any(|&child| self.reuses_ctor(ty, case_id, child, depth + 1)),
        }
    }

    fn borrow_walk(&mut self, func_idx: usize, node_id: NodeId, tail: bool, depth: u32) -> bool {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        let core_id = self.node(node_id).core;
        let reg = self.node(node_id).reg;
        let own = self.flow_id(func_idx, reg);
        let children = self.node(node_id).children.clone();

        if matches!(self.arena.expr(core_id), Expr::Case { .. }) {
            if !self.borrow_walk(func_idx, children[0], false, depth + 1) {
                return false;
            }
            let scrut_reg = self.node(children[0]).reg;
            let scrut_ty = self.node(children[0]).ty;
            let scrut_flow = self.flow_id(func_idx, scrut_reg);
            let cases = self.node(node_id).cases.clone();
            for (i, &arm_id) in children[1..].iter().enumerate() {
                let body_id = self.node(arm_id).children[0];
                let binds = self.node(arm_id).binds.clone();
                if !binds.is_empty() && self.reuses_ctor(scrut_ty, cases[i], body_id, 0) {
                    self.seed(scrut_flow);
                }
                for b in binds {
                    let field = self.flow_id(func_idx, b);
                    self.seed(field);
                }
                let body_flow = self.flow_id(func_idx, self.node(body_id).reg);
                if !self.borrow_walk(func_idx, body_id, tail, depth + 1)
                    || !self.depends(own, body_flow)
                    || !self.depends(body_flow, own)
                {
                    return false;
                }
            }
        } else {
            for (i, &child) in children.iter().enumerate() {
                let child_tail = tail
                    && match self.arena.expr(core_id) {
                        Expr::Let { .. } => i == 1,
                        Expr::If { .. } => i != 0,
                        Expr::Join { .. } => true,
                        _ => false,
                    };
                if !self.borrow_walk(func_idx, child, child_tail, depth + 1) {
                    return false;
                }
            }
        }

        match self.arena.expr(core_id) {
            Expr::Let { .. } => {
                let bound = self.flow_id(func_idx, self.node(children[0]).reg);
                let binding = self.flow_id(func_idx, self.node(node_id).binds[0]);
                if !self.depends(binding, bound) || !self.depends(bound, binding) {
                    return false;
                }
            }
            Expr::If { .. } => {
                for &child in &children[1..] {
                    let cf = self.flow_id(func_idx, self.node(child).reg);
                    if !self.depends(own, cf) || !self.depends(cf, own) {
                        return false;
                    }
                }
            }
            Expr::Join { .. } => {
                for &child in &children {
                    let cf = self.flow_id(func_idx, self.node(child).reg);
                    if !self.depends(own, cf) || !self.depends(cf, own) {
                        return false;
                    }
                }
                let binds = self.node(node_id).binds.clone();
                for b in binds {
                    let bf = self.flow_id(func_idx, b);
                    self.seed(bf);
                }
            }
            Expr::Jump { .. } => {
                for &child in &children {
                    let cf = self.flow_id(func_idx, self.node(child).reg);
                    self.seed(cf);
                }
            }
            Expr::Call { .. } => {
                let callee = self.node(node_id).index as usize;
                for (i, &child) in children.iter().enumerate() {
                    let arg = self.flow_id(func_idx, self.node(child).reg);
                    let param = self.flow_id(callee, i as u32);
                    if !self.depends(param, arg) || (tail && !self.depends(arg, param)) {
                        return false;
                    }
                }
                self.seed(own);
            }
            Expr::Prim { .. } => {
                for (i, &child) in children.iter().enumerate() {
                    if Self::operation_owned(self.node(node_id), i) {
                        let cf = self.flow_id(func_idx, self.node(child).reg);
                        self.seed(cf);
                    }
                }
                self.seed(own);
            }
            Expr::Lam { .. } => {
                let binds = self.node(node_id).binds.clone();
                for b in binds {
                    let bf = self.flow_id(func_idx, b);
                    self.seed(bf);
                }
                self.seed(own);
            }
            Expr::App { .. } | Expr::Con { .. } | Expr::Tuple(_) => {
                for &child in &children {
                    let cf = self.flow_id(func_idx, self.node(child).reg);
                    self.seed(cf);
                }
                self.seed(own);
            }
            Expr::Proj { .. } => {
                self.seed(own);
            }
            Expr::Lit(_)
            | Expr::Var(_)
            | Expr::Global(_)
            | Expr::Case { .. }
            | Expr::Stop(_)
            | Expr::Fail(_) => {}
        }

        if tail && !matches!(self.arena.expr(core_id), Expr::Stop(_)) {
            self.seed(own);
        }
        true
    }

    pub fn borrow_infer(&mut self) -> bool {
        let count = self.function_count as usize;
        for i in 0..count {
            if self.funs[i].kind == FunctionKind::Init {
                continue;
            }
            if self.funs[i].kind != FunctionKind::Function {
                let pcount = self.funs[i].parameters.len() as u32;
                for j in 0..pcount {
                    let fid = self.flow_id(i, j);
                    self.seed(fid);
                }
            }
            let cap_regs: Vec<u32> = self.bodies[i].captures.iter().map(|c| c.reg).collect();
            for r in cap_regs {
                let fid = self.flow_id(i, r);
                self.seed(fid);
            }
            if let Some(body) = self.bodies[i].body
                && !self.borrow_walk(i, body, true, 0)
            {
                return false;
            }
        }

        let mut queue = Vec::with_capacity(self.ownership.len());
        for (i, slot) in self.ownership.iter().enumerate() {
            if slot.owned {
                queue.push(i as u32);
            }
        }
        let mut at = 0usize;
        while at < queue.len() {
            let cur = queue[at] as usize;
            at += 1;
            let mut e = self.ownership[cur].edges;
            while e != NO_INDEX {
                let edge = self.edges[e as usize];
                let to = edge.to as usize;
                if !self.ownership[to].owned {
                    self.ownership[to].owned = true;
                    queue.push(edge.to);
                }
                e = edge.next;
            }
        }

        for i in 0..count {
            let pcount = self.funs[i].parameters.len();
            for j in 0..pcount {
                let ty = self.funs[i].parameters[j].ty;
                let fid = self.flow_id(i, j as u32) as usize;
                if self.is_reference(ty) && !self.ownership[fid].owned {
                    self.funs[i].parameters[j].ownership = ParamOwnership::Borrowed;
                }
            }
        }
        true
    }

    pub fn live_charge(&mut self, func_idx: usize, count: u64) -> bool {
        let f = &mut self.bodies[func_idx];
        if count > MO_LIMIT_CODEGEN_LIVE_WORK - f.live_work {
            return self.fail(
                ErrorCode::CodegenLiveWork,
                "Reference liveness exceeds MO_LIMIT_CODEGEN_LIVE_WORK.",
            );
        }
        f.live_work += count;
        true
    }

    fn live_reserve(&mut self, func_idx: usize, live: &mut LiveSet, count: u32) -> bool {
        let ref_count = self.bodies[func_idx].references.len() as u32;
        debug_assert!(count <= ref_count);
        if count <= live.capacity {
            return true;
        }
        let mut capacity = if live.capacity == 0 { 4 } else { live.capacity };
        if capacity > ref_count {
            capacity = ref_count;
        }
        while capacity < count {
            capacity = if capacity > ref_count / 2 {
                ref_count
            } else {
                capacity * 2
            };
        }
        let live_cells = self.bodies[func_idx].live_cells;
        if u64::from(capacity) > MO_LIMIT_CODEGEN_LIVE_CELLS - u64::from(live_cells) {
            return self.fail(
                ErrorCode::CodegenLiveWork,
                "Reference liveness exceeds MO_LIMIT_CODEGEN_LIVE_CELLS.",
            );
        }
        if !self.live_charge(func_idx, live.items.len() as u64) {
            return false;
        }
        live.items
            .reserve((capacity as usize).saturating_sub(live.items.len()));
        self.bodies[func_idx].live_cells += capacity;
        live.capacity = capacity;
        true
    }

    fn live_use(&mut self, func_idx: usize, live: &mut LiveSet, r: u32) -> bool {
        if r == NO_INDEX || !self.is_reference(self.bodies[func_idx].regs[r as usize].ty) {
            return true;
        }
        debug_assert!(r < self.bodies[func_idx].live_count);
        if !self.live_charge(func_idx, 33) {
            return false;
        }
        let at = live.position(r);
        if at < live.items.len() && live.items[at] == r {
            return true;
        }
        let len = live.items.len() as u32;
        if !self.live_charge(func_idx, 1 + u64::from(len - at as u32))
            || !self.live_reserve(func_idx, live, len + 1)
        {
            return false;
        }
        live.items.insert(at, r);
        true
    }

    fn live_kill(&mut self, func_idx: usize, live: &mut LiveSet, r: u32) -> bool {
        if !self.live_charge(func_idx, 33) {
            return false;
        }
        let at = live.position(r);
        if at == live.items.len() || live.items[at] != r {
            return true;
        }
        let len = live.items.len() as u32;
        if !self.live_charge(func_idx, u64::from(len - at as u32)) {
            return false;
        }
        live.items.remove(at);
        true
    }

    fn live_copy(&mut self, func_idx: usize, to: &mut LiveSet, from: &LiveSet) -> bool {
        let len = from.items.len() as u32;
        if !self.live_charge(func_idx, 1 + u64::from(len)) || !self.live_reserve(func_idx, to, len)
        {
            return false;
        }
        to.items.clear();
        to.items.extend_from_slice(&from.items);
        true
    }

    fn live_clear(&mut self, func_idx: usize, live: &mut LiveSet) -> bool {
        if !self.live_charge(func_idx, 1) {
            return false;
        }
        live.items.clear();
        true
    }

    fn live_union(
        &mut self,
        func_idx: usize,
        to: &mut LiveSet,
        from: &LiveSet,
        changed: &mut bool,
    ) -> bool {
        let to_len = to.items.len();
        let from_len = from.items.len();
        if !self.live_charge(func_idx, 1 + 2 * ((to_len + from_len) as u64)) {
            return false;
        }
        let mut a = 0usize;
        let mut added = 0usize;
        for &fb in &from.items {
            while a < to_len && to.items[a] < fb {
                a += 1;
            }
            if a == to_len || to.items[a] != fb {
                added += 1;
            }
        }
        if added == 0 {
            return true;
        }
        let new_len = to_len + added;
        if !self.live_reserve(func_idx, to, new_len as u32) {
            return false;
        }
        to.items.resize(new_len, 0);
        let mut ai = to_len;
        let mut bi = from_len;
        let mut end = new_len;
        while bi != 0 {
            if ai != 0 && to.items[ai - 1] > from.items[bi - 1] {
                end -= 1;
                ai -= 1;
                to.items[end] = to.items[ai];
            } else {
                if ai != 0 && to.items[ai - 1] == from.items[bi - 1] {
                    ai -= 1;
                }
                end -= 1;
                bi -= 1;
                to.items[end] = from.items[bi];
            }
        }
        debug_assert_eq!(end, ai);
        *changed = true;
        true
    }

    fn live_walk(
        &mut self,
        func_idx: usize,
        node_id: NodeId,
        live: &mut LiveSet,
        depth: u32,
    ) -> bool {
        debug_assert!(u64::from(depth) < MO_LIMIT_CORE_DEPTH);
        let core_id = self.node(node_id).core;
        let span = self.node(node_id).span;
        let child_count = self.node(node_id).children.len();
        let bind_count = self.node(node_id).binds.len();
        self.span = span;

        if !self.live_charge(
            func_idx,
            33 * (2 + 3 * (child_count as u64) + 2 * (bind_count as u64)),
        ) {
            return false;
        }
        if self.node(node_id).last.is_empty() {
            let count = if matches!(self.arena.expr(core_id), Expr::Lam { .. }) {
                bind_count
            } else {
                child_count
            };
            self.node_mut(node_id).last = vec![false; count.max(1)];
        }

        if matches!(self.arena.expr(core_id), Expr::Var(_)) {
            return true;
        }

        if matches!(self.arena.expr(core_id), Expr::Let { .. }) {
            let child0 = self.node(node_id).children[0];
            let child1 = self.node(node_id).children[1];
            let bind0 = self.node(node_id).binds[0];
            if !self.live_walk(func_idx, child1, live, depth + 1)
                || !self.live_kill(func_idx, live, bind0)
            {
                return false;
            }
            let r = self.node(child0).reg;
            self.node_mut(node_id).last[0] = !live.has(r);
            return self.live_use(func_idx, live, r)
                && self.live_walk(func_idx, child0, live, depth + 1);
        }

        let reg = self.node(node_id).reg;
        if !self.live_kill(func_idx, live, reg) {
            return false;
        }

        let is_branch = matches!(
            self.arena.expr(core_id),
            Expr::If { .. } | Expr::Case { .. }
        );
        let is_join = matches!(self.arena.expr(core_id), Expr::Join { .. });
        if is_branch || is_join {
            let mut after = std::mem::take(&mut self.node_mut(node_id).after);
            let ok = self.live_copy(func_idx, &mut after, live);
            if !ok {
                self.node_mut(node_id).after = after;
                return false;
            }
            if is_branch {
                let is_choice = matches!(self.arena.expr(core_id), Expr::Case { .. });
                let children = self.node(node_id).children.clone();
                if !self.live_clear(func_idx, live) {
                    self.node_mut(node_id).after = after;
                    return false;
                }
                let mut work = std::mem::take(&mut self.node_mut(node_id).work);
                let scrut_reg = self.node(children[0]).reg;
                let mut branch_ok = true;
                for (offset, &arm_id) in children[1..].iter().enumerate() {
                    let i = offset + 1;
                    let body_id = if is_choice {
                        self.node(arm_id).children[0]
                    } else {
                        arm_id
                    };
                    if !self.live_copy(func_idx, &mut work, &after) {
                        branch_ok = false;
                        break;
                    }
                    let body_reg = self.node(body_id).reg;
                    self.node_mut(node_id).last[i] = !work.has(body_reg);
                    if !self.live_use(func_idx, &mut work, body_reg)
                        || !self.live_walk(func_idx, body_id, &mut work, depth + 1)
                    {
                        branch_ok = false;
                        break;
                    }
                    if is_choice {
                        let arm_binds = self.node(arm_id).binds.clone();
                        if !self.live_charge(func_idx, arm_binds.len() as u64) {
                            branch_ok = false;
                            break;
                        }
                        for b in arm_binds {
                            if !self.live_kill(func_idx, &mut work, b) {
                                branch_ok = false;
                                break;
                            }
                        }
                        if !branch_ok {
                            break;
                        }
                        self.node_mut(arm_id).scrutinee_dead = !work.has(scrut_reg);
                    }
                    let mut ignored = false;
                    if !self.live_union(func_idx, live, &work, &mut ignored) {
                        branch_ok = false;
                        break;
                    }
                }
                self.node_mut(node_id).after = after;
                self.node_mut(node_id).work = work;
                if !branch_ok {
                    return false;
                }
                return self.live_use(func_idx, live, scrut_reg)
                    && self.live_walk(func_idx, children[0], live, depth + 1);
            }

            // Join node
            let join_idx = self.node(node_id).index as usize;
            let binds = self.node(node_id).binds.clone();
            let child0 = self.node(node_id).children[0];
            let child1 = self.node(node_id).children[1];
            if self.bodies[func_idx].joins[join_idx].used.len() != binds.len() {
                self.bodies[func_idx].joins[join_idx].used = vec![false; binds.len()];
            }
            let mut work = std::mem::take(&mut self.node_mut(node_id).work);
            let mut loop_ok = true;
            loop {
                if !self.live_copy(func_idx, &mut work, &after) {
                    loop_ok = false;
                    break;
                }
                let c0_reg = self.node(child0).reg;
                self.node_mut(node_id).last[0] = !work.has(c0_reg);
                if !self.live_use(func_idx, &mut work, c0_reg)
                    || !self.live_walk(func_idx, child0, &mut work, depth + 1)
                {
                    loop_ok = false;
                    break;
                }
                for (i, &b) in binds.iter().enumerate() {
                    self.bodies[func_idx].joins[join_idx].used[i] = work.has(b);
                    if !self.live_kill(func_idx, &mut work, b) {
                        loop_ok = false;
                        break;
                    }
                }
                if !loop_ok {
                    break;
                }
                let mut target_live =
                    std::mem::take(&mut self.bodies[func_idx].joins[join_idx].live);
                let mut changed = false;
                let union_ok = self.live_union(func_idx, &mut target_live, &work, &mut changed);
                self.bodies[func_idx].joins[join_idx].live = target_live;
                if !union_ok {
                    loop_ok = false;
                    break;
                }
                if !changed {
                    break;
                }
            }
            self.node_mut(node_id).work = work;
            if !loop_ok {
                self.node_mut(node_id).after = after;
                return false;
            }
            let copy_ok = self.live_copy(func_idx, live, &after);
            self.node_mut(node_id).after = after;
            if !copy_ok {
                return false;
            }
            let c1_reg = self.node(child1).reg;
            self.node_mut(node_id).last[1] = !live.has(c1_reg);
            return self.live_use(func_idx, live, c1_reg)
                && self.live_walk(func_idx, child1, live, depth + 1);
        }

        if matches!(self.arena.expr(core_id), Expr::Jump { .. }) {
            let join_idx = self.node(node_id).index as usize;
            let target_live = self.bodies[func_idx].joins[join_idx].live.clone();
            if !self.live_copy(func_idx, live, &target_live) {
                return false;
            }
        } else if matches!(self.arena.expr(core_id), Expr::Stop(_) | Expr::Fail(_))
            && !self.live_clear(func_idx, live)
        {
            return false;
        }

        if matches!(self.arena.expr(core_id), Expr::Lam { .. }) {
            let binds = self.node(node_id).binds.clone();
            for (i, &r) in binds.iter().enumerate().rev() {
                self.node_mut(node_id).last[i] = !live.has(r);
                if !self.live_use(func_idx, live, r) {
                    return false;
                }
            }
            return true;
        }

        let children = self.node(node_id).children.clone();
        for (i, &child) in children.iter().enumerate() {
            if !self.operand_owned(node_id, i) {
                let r = self.node(child).reg;
                if !self.live_use(func_idx, live, r) {
                    return false;
                }
            }
        }
        for (i, &child) in children.iter().enumerate().rev() {
            let r = self.node(child).reg;
            self.node_mut(node_id).last[i] = !live.has(r);
            if !self.live_use(func_idx, live, r) {
                return false;
            }
        }
        for &child in children.iter().rev() {
            if !self.live_walk(func_idx, child, live, depth + 1) {
                return false;
            }
        }
        true
    }

    pub fn live_infer(&mut self, func_idx: usize) -> bool {
        let reg_count = self.bodies[func_idx].regs.len() as u32;
        self.bodies[func_idx].live_count = reg_count;
        if !self.live_charge(func_idx, 2 * u64::from(reg_count)) {
            return false;
        }
        let mut refs = Vec::new();
        for i in 0..reg_count {
            let ty = self.bodies[func_idx].regs[i as usize].ty;
            if self.is_reference(ty) {
                refs.push(i);
            }
        }
        self.bodies[func_idx].references = refs;
        let Some(body_id) = self.bodies[func_idx].body else {
            return true;
        };
        let body_reg = self.node(body_id).reg;
        let mut live = LiveSet::default();
        self.live_use(func_idx, &mut live, body_reg)
            && self.live_walk(func_idx, body_id, &mut live, 0)
    }
}
