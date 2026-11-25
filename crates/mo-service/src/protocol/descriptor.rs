//! Builds the `McpType.Service` descriptor value inside the protocol heap from a compiled
//! service's bytecode metadata (`src/tools/protocol-descriptor.c` parity).

use std::collections::VecDeque;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{
    BytecodeFile, NO_INDEX, ServiceDef, ServiceShape, ServiceTool, TypeField, TypeKind,
};
use mo_heap::{Heap, Value};

use super::{Pf, ProtocolFunctions};

/// Constructed `McpType.Service` descriptor value and its protocol type index.
pub struct ProtocolDescriptor {
    /// Protocol type index for `McpType.Service`.
    pub ty: u32,
    /// Constructed value living on the protocol heap.
    pub value: Value,
}

struct Builder<'a> {
    heap: &'a Heap,
    ptypes: &'a TypeTable,
    source: &'a BytecodeFile,
    service: &'a ServiceDef,
    ids: Vec<u32>,
    queue: Vec<u32>,
    kinds: u32,
    kind: u32,
    tools: u32,
    tool: u32,
    field: u32,
    cases: u32,
    case_ty: u32,
    payload: u32,
    ints: u32,
}

impl<'a> Builder<'a> {
    fn field_type(&self, ty: u32, tag: u32, field: usize) -> u32 {
        self.ptypes.get(ty).cases[tag as usize].fields[field].ty
    }

    fn type_id(&mut self, source: u32) -> i64 {
        let idx = source as usize;
        if self.ids[idx] == NO_INDEX {
            let assigned = self.queue.len() as u32;
            self.ids[idx] = assigned;
            self.queue.push(source);
        }
        i64::from(self.ids[idx])
    }

    fn make_field(&mut self, name: &str, source_ty: u32) -> Option<Value> {
        let name_v = self.heap.alloc_text(name.to_string()).ok()?;
        let tid = self.type_id(source_ty);
        self.heap
            .make_case(self.ptypes, self.field, 0, vec![name_v, Value::Int(tid)])
            .ok()
    }

    fn make_fields(&mut self, src_fields: &[TypeField]) -> Option<Value> {
        let mut items = VecDeque::with_capacity(src_fields.len());
        for f in src_fields {
            let name = f.name.as_deref().unwrap_or("");
            items.push_back(self.make_field(name, f.ty)?);
        }
        self.heap.alloc_list(items).ok()
    }

    fn make_cases(&mut self, src_ty: u32, shape: &ServiceShape) -> Option<Value> {
        let cases = self.source.types.get(src_ty).cases.clone();
        let mut items = VecDeque::with_capacity(cases.len());
        for (i, c) in cases.iter().enumerate() {
            let payload_val = if c.fields.is_empty() {
                self.heap
                    .make_case(self.ptypes, self.payload, 0, Vec::new())
                    .ok()?
            } else if shape.unnamed.get(i).copied().unwrap_or(false) {
                let tid = self.type_id(c.fields[0].ty);
                self.heap
                    .make_case(self.ptypes, self.payload, 1, vec![Value::Int(tid)])
                    .ok()?
            } else {
                let named = self.make_fields(&c.fields)?;
                self.heap
                    .make_case(self.ptypes, self.payload, 2, vec![named])
                    .ok()?
            };
            let name_v = self.heap.alloc_text(c.name.clone()).ok()?;
            let case_val = self
                .heap
                .make_case(self.ptypes, self.case_ty, 0, vec![name_v, payload_val])
                .ok()?;
            items.push_back(case_val);
        }
        self.heap.alloc_list(items).ok()
    }

    fn find_shape(&self, src_ty: u32) -> Option<&'a ServiceShape> {
        self.service
            .shapes
            .binary_search_by_key(&src_ty, |s| s.ty)
            .ok()
            .map(|i| &self.service.shapes[i])
    }

    fn make_nominal(&mut self, src_ty: u32) -> Option<Value> {
        let shape = self.find_shape(src_ty)?;
        let t = self.source.types.get(src_ty);
        if !shape.is_struct {
            let cases = self.make_cases(src_ty, shape)?;
            return self
                .heap
                .make_case(self.ptypes, self.kind, 18, vec![cases])
                .ok();
        }
        let src_fields = if t.kind == TypeKind::Record {
            t.fields.clone()
        } else {
            t.cases.first()?.fields.clone()
        };
        let fields = self.make_fields(&src_fields)?;
        self.heap
            .make_case(self.ptypes, self.kind, 17, vec![fields])
            .ok()
    }

    fn make_kind(&mut self, src_ty: u32) -> Option<Value> {
        let t = self.source.types.get(src_ty);
        let tag = match t.kind {
            TypeKind::Unit => 0,
            TypeKind::Bool => 1,
            TypeKind::Byte => 2,
            TypeKind::Int => 3,
            TypeKind::Float => 4,
            TypeKind::Char => 5,
            TypeKind::Time => 6,
            TypeKind::Duration => 7,
            TypeKind::Decimal => 8,
            TypeKind::Text => 9,
            TypeKind::Bytes => 10,
            TypeKind::BigInt => 11,
            TypeKind::Range => 16,
            TypeKind::Record | TypeKind::Choice => return self.make_nominal(src_ty),
            TypeKind::Tuple => {
                let params = t.parameters.clone();
                let mut list = VecDeque::with_capacity(params.len());
                for p in params {
                    list.push_back(Value::Int(self.type_id(p)));
                }
                let list_val = self.heap.alloc_list(list).ok()?;
                return self
                    .heap
                    .make_case(self.ptypes, self.kind, 12, vec![list_val])
                    .ok();
            }
            TypeKind::List => {
                let tid = self.type_id(t.parameters[0]);
                return self
                    .heap
                    .make_case(self.ptypes, self.kind, 13, vec![Value::Int(tid)])
                    .ok();
            }
            TypeKind::Set => {
                let tid = self.type_id(t.parameters[0]);
                return self
                    .heap
                    .make_case(self.ptypes, self.kind, 14, vec![Value::Int(tid)])
                    .ok();
            }
            TypeKind::Map => {
                let k = self.type_id(t.parameters[0]);
                let v = self.type_id(t.parameters[1]);
                return self
                    .heap
                    .make_case(
                        self.ptypes,
                        self.kind,
                        15,
                        vec![Value::Int(k), Value::Int(v)],
                    )
                    .ok();
            }
            TypeKind::Function | TypeKind::Env | TypeKind::Capability => return None,
        };
        let _ = self.ints;
        self.heap
            .make_case(self.ptypes, self.kind, tag, Vec::new())
            .ok()
    }

    fn make_tool(&mut self, tool: &ServiceTool) -> Option<Value> {
        let name = self
            .heap
            .alloc_text(self.source.string(tool.name).to_string())
            .ok()?;
        let desc = self
            .heap
            .alloc_text(self.source.string(tool.description).to_string())
            .ok()?;
        let mut arg_list = VecDeque::with_capacity(tool.argument_types.len());
        for (&aname_idx, &aty) in tool.argument_names.iter().zip(&tool.argument_types) {
            let aname = self.source.string(aname_idx);
            arg_list.push_back(self.make_field(aname, aty)?);
        }
        let args_val = self.heap.alloc_list(arg_list).ok()?;

        let mut reply_ty = tool.result_type;
        let err_opt_ty = self.field_type(self.tool, 0, 5);
        let result_def = self.source.types.get(reply_ty);
        let err_val = if result_def.kind == TypeKind::Choice
            && result_def
                .name
                .as_deref()
                .is_some_and(|n| n.starts_with("Result("))
            && result_def.cases.len() == 2
            && result_def.cases[0].name == "Ok"
            && result_def.cases[1].name == "Error"
            && result_def.cases[0].unnamed
            && result_def.cases[1].unnamed
        {
            reply_ty = result_def.cases[0].fields[0].ty;
            let err_ty = result_def.cases[1].fields[0].ty;
            let err_tid = self.type_id(err_ty);
            self.heap
                .make_case(self.ptypes, err_opt_ty, 1, vec![Value::Int(err_tid)])
                .ok()?
        } else {
            self.heap
                .make_case(self.ptypes, err_opt_ty, 0, Vec::new())
                .ok()?
        };

        if !tool.read_only {
            reply_ty = self.source.types.get(reply_ty).parameters[1];
        }
        let reply_tid = self.type_id(reply_ty);
        self.heap
            .make_case(
                self.ptypes,
                self.tool,
                0,
                vec![
                    name,
                    desc,
                    Value::Bool(tool.read_only),
                    args_val,
                    Value::Int(reply_tid),
                    err_val,
                ],
            )
            .ok()
    }
}

/// Builds a `McpType.Service` descriptor on `heap` for `service_idx` of `source`
/// (`mo_protocol_descriptor`).
#[must_use]
pub fn build_descriptor(
    protocol_file: &BytecodeFile,
    fns: &ProtocolFunctions,
    heap: &Heap,
    source: &BytecodeFile,
    service_idx: u32,
) -> Option<ProtocolDescriptor> {
    let service = source.services.get(service_idx as usize)?;
    let ptypes = &protocol_file.types;
    let catalog_fn = &protocol_file.functions[fns.get(Pf::Catalog) as usize];
    let root = catalog_fn.parameters[0].ty;

    let kinds = ptypes.get(root).cases[0].fields[0].ty;
    let kind = ptypes.get(kinds).parameters[0];
    let tools = ptypes.get(root).cases[0].fields[1].ty;
    let tool = ptypes.get(tools).parameters[0];
    let fields = ptypes.get(tool).cases[0].fields[3].ty;
    let field = ptypes.get(fields).parameters[0];
    let cases = ptypes.get(kind).cases[18].fields[0].ty;
    let case_ty = ptypes.get(cases).parameters[0];
    let payload = ptypes.get(case_ty).cases[0].fields[1].ty;
    let ints = ptypes.get(kind).cases[12].fields[0].ty;

    let mut b = Builder {
        heap,
        ptypes,
        source,
        service,
        ids: vec![NO_INDEX; source.types.items.len()],
        queue: Vec::new(),
        kinds,
        kind,
        tools,
        tool,
        field,
        cases,
        case_ty,
        payload,
        ints,
    };

    let mut tool_vals = VecDeque::with_capacity(service.tools.len());
    for t in &service.tools {
        tool_vals.push_back(b.make_tool(t)?);
    }
    let tools_list = heap.alloc_list(tool_vals).ok()?;

    let mut kind_vals = VecDeque::new();
    let mut i = 0usize;
    while i < b.queue.len() {
        let src_ty = b.queue[i];
        kind_vals.push_back(b.make_kind(src_ty)?);
        i += 1;
    }
    let kinds_list = heap.alloc_list(kind_vals).ok()?;

    let value = heap
        .make_case(ptypes, root, 0, vec![kinds_list, tools_list])
        .ok()?;
    let _ = (b.kinds, b.tools, b.cases);
    Some(ProtocolDescriptor { ty: root, value })
}
