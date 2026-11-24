//! Rewrite-fix generation for `E0103` (a service tool returning bare `State` or `Result(State, E)`)
//! (`src/check/service-fixes.c` parity).

use std::fmt::Write as _;

use mo_base::{DiagBag, Diagnostic, Edit, Fix, SourceFile, SourceMap, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_FILE_SIZE, MO_LIMIT_LINE_LENGTH, MO_LIMIT_MODULE_NAMES, MO_LIMIT_PROGRAM_CHECK_WORK,
    MO_LIMIT_PROGRAM_SIZE, MO_LIMIT_SOURCE_CHECK_WORK, MO_LIMIT_SOURCE_TYPE_DEPTH,
};
use mo_syntax::{NodeId, SyntaxKind, SyntaxTree};

use crate::checked::{CheckedBody, CheckedConstant, CheckedFunction, CheckedService, CheckedSpec};
use crate::ids::{CheckedFuncId, SymRef};
use crate::load::ModuleSet;
use crate::modules::{ModuleInterfaces, ModuleScope};
use crate::signatures::Signatures;
use crate::types::SourceTypeKind;

struct Proposal<'a> {
    source: &'a SourceFile,
    tree: &'a SyntaxTree,
    declaration: NodeId,
    handler: &'a str,
    helper: String,
    record: String,
    changes: Vec<u8>,
    work: u64,
    shared_work: &'a mut u64,
    error: Option<ErrorCode>,
}

struct TextBuilder {
    bytes: Vec<u8>,
    limit: usize,
    column: u32,
}

impl TextBuilder {
    fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
            column: 0,
        }
    }
}

impl<'a> Proposal<'a> {
    fn charge(&mut self, amount: usize) -> bool {
        let amt = amount as u64;
        if amt > MO_LIMIT_SOURCE_CHECK_WORK.saturating_sub(self.work) {
            self.error = Some(ErrorCode::CheckWork);
            return false;
        }
        if amt > MO_LIMIT_PROGRAM_CHECK_WORK.saturating_sub(*self.shared_work) {
            self.error = Some(ErrorCode::CheckProgramWork);
            return false;
        }
        self.work += amt;
        *self.shared_work += amt;
        true
    }

    fn append(&mut self, out: &mut TextBuilder, bytes: &[u8]) -> bool {
        if bytes.len() > out.limit.saturating_sub(out.bytes.len()) {
            self.error = Some(ErrorCode::FileTooBig);
            return false;
        }
        if !self.charge(bytes.len()) {
            return false;
        }
        for &ch in bytes {
            if ch == b'\n' {
                out.column = 0;
            } else if ch != b'\r' && (ch & 0xc0) != 0x80 {
                out.column += 1;
            }
            if u64::from(out.column) > MO_LIMIT_LINE_LENGTH {
                self.error = Some(ErrorCode::LineTooLong);
                return false;
            }
        }
        out.bytes.extend_from_slice(bytes);
        true
    }

    fn literal(&mut self, out: &mut TextBuilder, s: &str) -> bool {
        self.append(out, s.as_bytes())
    }

    fn annotation(&mut self, out: &mut TextBuilder, ty_node: Option<NodeId>) -> bool {
        let Some(nid) = ty_node else {
            return self.literal(out, "Unit");
        };
        let span = self.tree.node(nid).span;
        let bytes = self.source.bytes();
        let start = span.start as usize;
        let end = span.end as usize;
        debug_assert!(start <= end && end <= bytes.len());
        self.append(out, &bytes[start..end])
    }

    fn argument(&mut self, out: &mut TextBuilder, index: usize) -> bool {
        let mut buf = String::new();
        let _ = write!(&mut buf, "service_argument{index}");
        self.literal(out, &buf)
    }

    fn fresh_name(&mut self, scope: &ModuleScope, prefix: &str) -> Option<String> {
        let src = self.source.bytes();
        let count = src.len() / (prefix.len() + 1) + scope.imported.len();
        if !self.charge(count + 1) || !self.charge(src.len()) {
            return None;
        }
        let mut used = vec![false; count + 1];
        let mut i = 0usize;
        while i < src.len() {
            if !is_name_byte(src[i]) {
                i += 1;
                continue;
            }
            let start = i;
            while i < src.len() && is_name_byte(src[i]) {
                i += 1;
            }
            reserve_suffix(&mut used, count, &src[start..i], prefix.as_bytes());
        }
        for imp in &scope.imported {
            if !self.charge(imp.name.len() + 1) {
                return None;
            }
            reserve_suffix(&mut used, count, imp.name.as_bytes(), prefix.as_bytes());
        }
        let mut suffix = 0usize;
        while used[suffix] {
            debug_assert!(suffix < count);
            suffix += 1;
        }
        Some(format!("{prefix}{suffix}"))
    }

    fn wrapper(&mut self, out: &mut TextBuilder, update: bool) -> bool {
        let decl = self.tree.node(self.declaration);
        let Some(params_nid) = decl.children.get(1).copied().flatten() else {
            return false;
        };
        let result_nid = decl.children.get(2).copied().flatten();
        if !self.literal(out, "\n\n") {
            return false;
        }
        if !update {
            let record = self.record.clone();
            if !self.literal(out, "struct ")
                || !self.literal(out, &record)
                || !self.literal(out, "\nvalue: ")
                || !self.annotation(out, result_nid)
                || !self.literal(out, "\nend\n\n")
            {
                return false;
            }
        }
        let leading = self.tree.node(self.declaration).leading.clone();
        for cid in leading {
            if !self.charge(1) {
                return false;
            }
            let comment = self.tree.comment(cid);
            if comment.documentation {
                let text = self.tree.comment_text(cid);
                if !self.append(out, text) || !self.literal(out, "\n") {
                    return false;
                }
            }
        }
        let handler = self.handler;
        if !self.literal(out, "fn ") || !self.literal(out, handler) || !self.literal(out, "(\n") {
            return false;
        }
        let param_children = self.tree.node(params_nid).children.clone();
        let param_count = param_children.len();
        for (i, child) in param_children.iter().enumerate() {
            let ann = child.and_then(|p| self.tree.node(p).children.get(1).copied().flatten());
            if !self.argument(out, i)
                || !self.literal(out, ": ")
                || !self.annotation(out, ann)
                || !self.literal(out, if i + 1 == param_count { "\n" } else { ",\n" })
            {
                return false;
            }
        }
        if !self.literal(out, ") : ") {
            return false;
        }
        if update {
            if !self.literal(out, "(")
                || !self.annotation(out, result_nid)
                || !self.literal(out, ", Unit)\n(")
            {
                return false;
            }
        } else {
            let record = self.record.clone();
            if !self.literal(out, &record)
                || !self.literal(out, "\n")
                || !self.literal(out, &record)
                || !self.literal(out, "(value: ")
            {
                return false;
            }
        }
        let helper = self.helper.clone();
        if !self.literal(out, &helper) || !self.literal(out, "(\n") {
            return false;
        }
        for i in 0..param_count {
            if !self.argument(out, i)
                || !self.literal(out, if i + 1 == param_count { "\n" } else { ",\n" })
            {
                return false;
            }
        }
        self.literal(
            out,
            if update {
                "), Unit)\nend\n"
            } else {
                "))\nend\n"
            },
        )
    }

    fn collect_references(
        &mut self,
        body: &CheckedBody,
        symbol: SymRef,
        signatures: &Signatures,
    ) -> bool {
        for row in &body.items {
            if !self.charge(1) {
                return false;
            }
            if row.symbol != Some(symbol)
                || !row.operands.is_empty()
                || signatures.types.get(row.ty).kind != SourceTypeKind::Function
            {
                continue;
            }
            let mut name_nid = row.syntax;
            if self.tree.node(name_nid).kind == SyntaxKind::Postfix
                && let Some(Some(last)) = self.tree.node(name_nid).children.last()
            {
                name_nid = *last;
            }
            if matches!(
                self.tree.node(name_nid).kind,
                SyntaxKind::FieldAccess | SyntaxKind::Qualified
            ) && let Some(Some(last)) = self.tree.node(name_nid).children.last()
            {
                name_nid = *last;
            }
            let node = self.tree.node(name_nid);
            debug_assert!(node.kind == SyntaxKind::Name);
            let start = node.span.start as usize;
            if start < self.changes.len() {
                self.changes[start] = 1;
            }
        }
        true
    }

    fn build_replacement(&mut self, out: &mut TextBuilder, update: bool) -> bool {
        let src = self.source.bytes();
        if !self.charge(src.len()) {
            return false;
        }
        let handler_len = self.handler.len();
        let helper = self.helper.clone();
        let mut i = 0usize;
        while i < src.len() {
            if self.changes[i] == 1 {
                if !self.literal(out, &helper) {
                    return false;
                }
                i += handler_len;
            } else if self.changes[i] == 2 {
                i += 1;
            } else {
                let start = i;
                i += 1;
                while i < src.len() && self.changes[i] == 0 {
                    i += 1;
                }
                if !self.append(out, &src[start..i]) {
                    return false;
                }
            }
        }
        self.wrapper(out, update)
    }
}

const fn is_name_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_' || byte >= 0x80
}

fn reserve_suffix(used: &mut [bool], count: usize, name: &[u8], prefix: &[u8]) {
    if name.len() <= prefix.len() || !name.starts_with(prefix) {
        return;
    }
    let mut suffix = 0usize;
    for &b in &name[prefix.len()..] {
        if !b.is_ascii_digit() || suffix > count / 10 {
            return;
        }
        suffix = suffix * 10 + usize::from(b - b'0');
        if suffix > count {
            return;
        }
    }
    used[suffix] = true;
}

/// Emits `E0103` for a service tool returning bare `State` or `Result(State, E)`, attaching
/// the two full-file rewrite fixes when eligible (`mo_service_state_error`).
#[expect(
    clippy::too_many_arguments,
    reason = "service fix synthesis mirrors the C entry point and threads shared checker state"
)]
pub fn report_service_state_error(
    sources: &SourceMap,
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    functions: &[CheckedFunction],
    constants: &[CheckedConstant],
    specs: &[CheckedSpec],
    services: &[CheckedService],
    tool_name: NodeId,
    tool_span: Span,
    function: CheckedFuncId,
    result_state: bool,
    diags: &mut DiagBag,
) {
    let _ = tool_name;
    let sym_ref = functions[function.index()].symbol;
    let sym = interfaces.symbol(sym_ref);
    let scope = interfaces.scope(sym.module);
    let predicate = sym.name.ends_with('?');
    let tree = &modules.module(sym.module).syntax.tree;

    let default_primary = if result_state {
        "a tool returns Result((State, reply), E)"
    } else {
        "a timer returns this; a tool returns (State, reply)"
    };
    let reason = if result_state {
        "A tool changes the state only by returning (state, reply) or Result((state, reply), E). \
         A Result holding the state alone is a read-only reply, so the new state would never be \
         saved (reference 11.5)."
    } else if predicate {
        "A tool changes the state only by returning (state, reply). Returning the state alone is \
         how a timer changes it, so a tool's result would never be saved. A name ending in ? \
         must return Bool, so rename the function before changing its result (reference 11.5)."
    } else if sym.exported {
        "A tool changes the state only by returning (state, reply). Returning the state alone is \
         how a timer changes it, so a tool's result would never be saved. Other modules call \
         this function, so change its result by hand (reference 11.5)."
    } else {
        "A tool changes the state only by returning (state, reply). Returning the state alone is \
         how a timer changes it, so a tool's result would never be saved (reference 11.5)."
    };

    let mut diag = Diagnostic::new(
        ErrorCode::ServiceStateResult,
        tool_span,
        "a service tool returns only its state",
    )
    .with_primary_text(default_primary)
    .with_reason(reason);

    let Some(source) = sources.get(tool_span.file) else {
        let _ = diags.push(diag);
        return;
    };

    let sig_id = functions[function.index()].signature;
    let sig_ty = signatures.functions[sig_id.index()].ty;
    let result_height = sig_ty.map_or(0, |tid| {
        let args = &signatures.types.get(tid).arguments;
        args.last()
            .map_or(0, |&ret| signatures.types.get(ret).height)
    });

    let mut shared_work = signatures.work;
    let mut prop = Proposal {
        source,
        tree,
        declaration: sym.declaration,
        handler: &sym.name,
        helper: String::new(),
        record: String::new(),
        changes: Vec::new(),
        work: 0,
        shared_work: &mut shared_work,
        error: None,
    };

    let decl = tree.node(sym.declaration);
    let state_ann = if result_state {
        decl.children
            .get(1)
            .copied()
            .flatten()
            .and_then(|ps| tree.node(ps).children.first().copied().flatten())
            .and_then(|p0| tree.node(p0).children.get(1).copied().flatten())
    } else {
        decl.children.get(2).copied().flatten()
    };

    let mut label = TextBuilder::new(MO_LIMIT_FILE_SIZE as usize);
    let prefix_str = if result_state {
        "a tool returns Result(("
    } else {
        "a timer returns this; a tool returns ("
    };
    let suffix_str = if result_state {
        ", reply), E)"
    } else {
        ", reply)"
    };
    if !prop.literal(&mut label, prefix_str)
        || !prop.annotation(&mut label, state_ann)
        || !prop.literal(&mut label, suffix_str)
        || !prop.charge(label.bytes.len() + 1)
    {
        let err_code = prop.error.unwrap_or(ErrorCode::CheckWork);
        signatures.work = *prop.shared_work;
        let _ = diags.push(Diagnostic::new(
            err_code,
            tool_span,
            "the service state fixes exceed checking resources",
        ));
        return;
    }
    diag.primary.text = Some(label.bytes.into());

    if result_state || sym.exported || predicate {
        signatures.work = *prop.shared_work;
        let _ = diags.push(diag);
        return;
    }

    let fail = |err_code: ErrorCode, work_out: u64, sigs: &mut Signatures, diags: &mut DiagBag| {
        sigs.work = work_out;
        let _ = diags.push(Diagnostic::new(
            err_code,
            tool_span,
            "the service state fixes exceed checking resources",
        ));
    };

    if (scope.symbols.len() as u64) > MO_LIMIT_MODULE_NAMES - 2 {
        fail(
            ErrorCode::TooManyNames,
            *prop.shared_work,
            signatures,
            diags,
        );
        return;
    }
    if u64::from(result_height) >= MO_LIMIT_SOURCE_TYPE_DEPTH - 2 {
        fail(
            ErrorCode::CheckTypeDepth,
            *prop.shared_work,
            signatures,
            diags,
        );
        return;
    }

    let helper_prefix = format!("service_tool_{}_", function.0);
    let Some(helper_name) = prop.fresh_name(scope, &helper_prefix) else {
        fail(
            prop.error.unwrap_or(ErrorCode::CheckWork),
            *prop.shared_work,
            signatures,
            diags,
        );
        return;
    };
    prop.helper = helper_name;

    let record_prefix = format!("ServiceReply{}Case", function.0);
    let Some(record_name) = prop.fresh_name(scope, &record_prefix) else {
        fail(
            prop.error.unwrap_or(ErrorCode::CheckWork),
            *prop.shared_work,
            signatures,
            diags,
        );
        return;
    };
    prop.record = record_name;

    let src_len = source.bytes().len();
    if !prop.charge(src_len) {
        fail(
            prop.error.unwrap_or(ErrorCode::CheckWork),
            *prop.shared_work,
            signatures,
            diags,
        );
        return;
    }
    prop.changes = vec![0u8; src_len];
    if let Some(Some(name_nid)) = decl.children.first() {
        let start = tree.node(*name_nid).span.start as usize;
        if start < prop.changes.len() {
            prop.changes[start] = 1;
        }
    }
    let leading = decl.leading.clone();
    for cid in leading {
        let comment = tree.comment(cid);
        let clen = (comment.span.end - comment.span.start) as usize;
        if !prop.charge(clen + 1) {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
        if comment.documentation {
            let start = comment.span.start as usize;
            let end = (comment.span.end as usize).min(prop.changes.len());
            for byte in &mut prop.changes[start..end] {
                *byte = 2;
            }
        }
    }

    for func in functions {
        if !prop.charge(1)
            || (interfaces.symbol(func.symbol).module == sym.module
                && !prop.collect_references(&func.body, sym_ref, signatures))
        {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
    }
    for constant in constants {
        if !prop.charge(1)
            || (interfaces.symbol(constant.symbol).module == sym.module
                && !prop.collect_references(&constant.initializer, sym_ref, signatures))
        {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
    }
    for spec in specs {
        if !prop.charge(1)
            || (spec.module == sym.module
                && !prop.collect_references(&spec.body, sym_ref, signatures))
        {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
    }
    for svc in services {
        if !prop.charge(1)
            || (svc.module == sym.module
                && !prop.collect_references(&svc.initializer, sym_ref, signatures))
        {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
    }

    let program_base = sources.total_bytes().saturating_sub(src_len as u64);
    let titles = [
        "return (state, Unit) to save the new state",
        "return the state inside a record, as read-only data",
    ];
    let mut fixes = Vec::with_capacity(2);
    for (i, title) in titles.iter().enumerate() {
        let mut out = TextBuilder::new(MO_LIMIT_FILE_SIZE as usize);
        if !prop.build_replacement(&mut out, i == 0) {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
        let length = out.bytes.len();
        if (length as u64) > MO_LIMIT_PROGRAM_SIZE.saturating_sub(program_base) {
            fail(
                ErrorCode::ProgramTooBig,
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
        if !prop.charge(length + 1) {
            fail(
                prop.error.unwrap_or(ErrorCode::CheckWork),
                *prop.shared_work,
                signatures,
                diags,
            );
            return;
        }
        let edit = Edit::new(Span::new(tool_span.file, 0, src_len as u32), out.bytes);
        fixes.push(Fix::new(Some((*title).into()), vec![edit]));
    }

    signatures.work = *prop.shared_work;
    for fix in fixes {
        diag = diag.with_fix(fix);
    }
    let _ = diags.push(diag);
}
