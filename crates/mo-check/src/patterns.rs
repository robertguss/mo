//! Iterative Maranget pattern exhaustiveness checking and bounded missing-case witness
//! rendering (`src/check/patterns.c` parity).
//!
//! Uses an explicit heap frame stack (zero recursion) and charges both visited matrix cells
//! and retained matrix byte costs against the shared work & arena budgets.

use std::fmt::Write as _;

use mo_base::{DiagBag, Diagnostic, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_CHECK_WORK};
use mo_syntax::SyntaxKind;

use crate::ids::{DataSigId, SymRef, TypeId};
use crate::modules::ModuleInterfaces;
use crate::signatures::Signatures;
use crate::types::{SourceTypeKind, builtin_case_by_key};

/// Arena index for a [`MatchPattern`] node inside a [`MatchPatternArena`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MatchPatternId(pub u32);

impl MatchPatternId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Key namespaces matching `src/check/patterns.c` / `src/check/infer-patterns.c`.
///
/// - `1`, `2`: `true`, `false`
/// - `3`, `4`: `[]`, `[h, ..t]`
/// - `5`: tuple
/// - `6..=40`: built-in constructors (`BUILTIN_CASES`)
/// - `256..=511`: `Byte` literal values `0..=255`
/// - `>= KEY_SYMBOL_BASE`: source nominal constructors (`SymRef`)
/// - `>= KEY_LITERAL_BASE`: non-byte scalar literals
pub const KEY_TRUE: u64 = 1;
pub const KEY_FALSE: u64 = 2;
pub const KEY_LIST_EMPTY: u64 = 3;
pub const KEY_LIST_CONS: u64 = 4;
pub const KEY_TUPLE: u64 = 5;
pub const KEY_BYTE_BASE: u64 = 256;
pub const KEY_SYMBOL_BASE: u64 = 1u64 << 40;
pub const KEY_LITERAL_BASE: u64 = 1u64 << 48;

#[must_use]
pub const fn symbol_pattern_key(sym: SymRef) -> u64 {
    KEY_SYMBOL_BASE + ((sym.module.0 as u64) << 24) + (sym.symbol.0 as u64)
}

#[must_use]
pub const fn literal_pattern_key(index: u64) -> u64 {
    KEY_LITERAL_BASE + index
}

/// One constructor/literal pattern node in the Maranget pattern matrix (`struct mo_match_pattern`).
///
/// Wildcards are represented as `None` (`Option<MatchPatternId>`).
#[derive(Debug, Clone)]
pub struct MatchPattern {
    pub name: String,
    pub key: u64,
    pub signature: u32,
    pub items: Vec<Option<MatchPatternId>>,
    pub data: Option<DataSigId>,
    pub choice: Option<SymRef>,
}

/// Arena owning [`MatchPattern`] nodes during one `case` exhaustiveness check.
#[derive(Debug, Clone, Default)]
pub struct MatchPatternArena {
    items: Vec<MatchPattern>,
}

impl MatchPatternArena {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn alloc(&mut self, pattern: MatchPattern) -> MatchPatternId {
        let id = MatchPatternId(self.items.len() as u32);
        self.items.push(pattern);
        id
    }

    #[must_use]
    pub fn get(&self, id: MatchPatternId) -> &MatchPattern {
        &self.items[id.index()]
    }

    pub fn get_mut(&mut self, id: MatchPatternId) -> &mut MatchPattern {
        &mut self.items[id.index()]
    }

    pub fn set_item(&mut self, id: MatchPatternId, slot: usize, val: Option<MatchPatternId>) {
        self.items[id.index()].items[slot] = val;
    }

    pub fn set_meta(
        &mut self,
        id: MatchPatternId,
        choice: Option<SymRef>,
        data: Option<DataSigId>,
    ) {
        let p = &mut self.items[id.index()];
        p.choice = choice;
        p.data = data;
    }
}

/// Arena-allocated singly-linked cell (`struct cell`), mirroring C's sharing across matrix rows.
#[derive(Debug, Clone, Copy)]
struct Cell {
    pattern: Option<MatchPatternId>,
    next: Option<u32>,
}

#[derive(Debug, Clone, Copy)]
struct Row {
    cells: Option<u32>,
    next: Option<u32>,
}

#[derive(Debug, Clone, Copy)]
struct Head {
    pattern: MatchPatternId,
    rest: Option<u32>,
}

struct Frame {
    parent: Option<usize>,
    defaults: Option<u32>,
    heads: Vec<Head>,
    at: usize,
    end: usize,
    width: u32,
    chosen: Option<MatchPatternId>,
    complete: bool,
}

/// C-equivalent struct byte sizes on 64-bit Linux for `MO_LIMIT_SOURCE_CHECK_WORK` parity.
const SIZEOF_CELL: usize = 16;
const SIZEOF_ROW: usize = 16;
const SIZEOF_HEAD: usize = 16;
const SIZEOF_FRAME: usize = 48;
const SIZEOF_MATCH_PATTERN: usize = 56;
const SIZEOF_PTR: usize = 8;
const SIZEOF_RENDERING: usize = 32;

struct Coverage<'a> {
    arena: &'a mut MatchPatternArena,
    interfaces: &'a ModuleInterfaces,
    signatures: &'a Signatures,
    diags: &'a mut DiagBag,
    span: Span,
    work: &'a mut u32,
    program_work: &'a mut u64,
    arena_remaining: &'a mut usize,
    cells: Vec<Cell>,
    rows: Vec<Row>,
    frames: Vec<Frame>,
    failed: bool,
}

impl<'a> Coverage<'a> {
    fn problem(&mut self, code: ErrorCode, message: &'static str) -> bool {
        if !self.failed {
            let diag = Diagnostic::new(code, self.span, message).with_primary_text(message);
            let _ = self.diags.push(diag);
        }
        self.failed = true;
        false
    }

    fn charge_work(&mut self, amount: u32) -> bool {
        if self.failed {
            return false;
        }
        if u64::from(amount) > MO_LIMIT_SOURCE_CHECK_WORK - u64::from(*self.work) {
            return self.problem(
                ErrorCode::CheckWork,
                "the function needs too much pattern-checking work",
            );
        }
        if u64::from(amount) > MO_LIMIT_PROGRAM_CHECK_WORK - *self.program_work {
            return self.problem(
                ErrorCode::CheckProgramWork,
                "the program needs too much pattern-checking work",
            );
        }
        *self.work += amount;
        *self.program_work += u64::from(amount);
        true
    }

    fn charge_alloc(&mut self, count: usize, size: usize) -> bool {
        if self.failed {
            return false;
        }
        debug_assert!(size != 0);
        if count > (MO_LIMIT_SOURCE_CHECK_WORK as usize) / size {
            return self.problem(
                ErrorCode::CheckWork,
                "the function needs too much pattern-checking work",
            );
        }
        let bytes = count * size;
        if !self.charge_work(bytes as u32) {
            return false;
        }
        let aligned = bytes.checked_add(7).map_or(usize::MAX, |n| n & !7usize);
        if aligned > *self.arena_remaining {
            return self.problem(ErrorCode::CheckNoMemory, "the checking arena has no room");
        }
        *self.arena_remaining -= aligned;
        true
    }

    fn prepend(&mut self, pattern: Option<MatchPatternId>, rest: Option<u32>) -> Option<u32> {
        if !self.charge_work(1) || !self.charge_alloc(1, SIZEOF_CELL) {
            return None;
        }
        let id = self.cells.len() as u32;
        self.cells.push(Cell {
            pattern,
            next: rest,
        });
        Some(id)
    }

    fn add_row(&mut self, head: &mut Option<u32>, cells: Option<u32>) -> bool {
        if !self.charge_work(1) || !self.charge_alloc(1, SIZEOF_ROW) {
            return false;
        }
        let id = self.rows.len() as u32;
        self.rows.push(Row { cells, next: *head });
        *head = Some(id);
        true
    }

    fn head_present(&mut self, frame_idx: Option<usize>, key: u64) -> Option<bool> {
        if !self.charge_work(1) {
            return None;
        }
        let Some(fi) = frame_idx else {
            return Some(false);
        };
        let mut lo = 0usize;
        let mut hi = self.frames[fi].heads.len();
        while lo < hi {
            if !self.charge_work(1) {
                return None;
            }
            let mid = lo + (hi - lo) / 2;
            let mid_key = self.arena.get(self.frames[fi].heads[mid].pattern).key;
            if mid_key < key {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        let present = lo < self.frames[fi].heads.len()
            && self.arena.get(self.frames[fi].heads[lo].pattern).key == key;
        Some(present)
    }

    fn missing_head(
        &mut self,
        frame_idx: Option<usize>,
        first_choice: Option<SymRef>,
        first_key: u64,
        first_signature: u32,
    ) -> Option<MatchPatternId> {
        let head_count = frame_idx.map_or(0, |fi| self.frames[fi].heads.len());
        let mut name = String::from("<another value>");
        let mut key = 0u64;
        let mut count = 0usize;

        if let Some(owner_ref) = first_choice {
            if !self.charge_alloc(4096, 1) {
                return None;
            }
            let owner = self.interfaces.symbol(owner_ref);
            let scope = self.interfaces.scope(owner.module);
            let mut text = String::new();
            let mut missing = 0u32;
            let mut shown = 0u32;
            for i in 0..owner.cases.len() {
                let sym_idx = owner.cases[i];
                let sym_ref = SymRef::new(owner.module, sym_idx);
                let sym = &scope.symbols[sym_idx.index()];
                let present = self.head_present(frame_idx, symbol_pattern_key(sym_ref))?;
                if present {
                    continue;
                }
                missing += 1;
                if shown < 8 && text.len() < 3800 {
                    if shown != 0 {
                        append_bounded(&mut text, " | ");
                    }
                    let sym_name = &sym.name;
                    if sym_name.len() > 128 {
                        let mut end = 128;
                        while end > 0 && !sym_name.is_char_boundary(end) {
                            end -= 1;
                        }
                        append_bounded(&mut text, &sym_name[..end]);
                        append_bounded(&mut text, "...");
                    } else {
                        append_bounded(&mut text, sym_name);
                    }
                    // Check if the enum case has payload (children[1] is present).
                    let has_payload = self
                        .signatures
                        .data_signature_by_symbol(sym_ref)
                        .is_some_and(|(_, d)| !d.fields.is_empty());
                    if has_payload {
                        append_bounded(&mut text, "(..)");
                    }
                    shown += 1;
                }
            }
            if missing > shown {
                let _ = write!(&mut text, " and {} more", missing - shown);
            }
            name = text;
        } else if let Some(domain) = u32::try_from(first_key).ok().and_then(builtin_case_by_key) {
            if !self.charge_alloc(4096, 1) {
                return None;
            }
            let mut text = String::new();
            for i in 0..domain.cases {
                let candidate = u64::from(domain.first_key + i);
                let found = self.head_present(frame_idx, candidate)?;
                if !found {
                    let missing = builtin_case_by_key(domain.first_key + i)?;
                    if !text.is_empty() {
                        append_bounded(&mut text, " | ");
                    }
                    qualify(&mut text, Some(missing));
                    append_bounded(&mut text, missing.name);
                    if missing.count != 0 {
                        append_bounded(&mut text, "(..)");
                    }
                }
            }
            name = text;
        } else if first_signature == 256 {
            let mut seen = [false; 256];
            if let Some(fi) = frame_idx {
                for h in &self.frames[fi].heads {
                    let hk = self.arena.get(h.pattern).key;
                    debug_assert!((KEY_BYTE_BASE..KEY_BYTE_BASE + 256).contains(&hk));
                    seen[(hk - KEY_BYTE_BASE) as usize] = true;
                }
            }
            for (i, &present) in seen.iter().enumerate() {
                if !present {
                    if !self.charge_alloc(16, 1) {
                        return None;
                    }
                    name = i.to_string();
                    break;
                }
            }
        } else if first_signature == 2 {
            if first_key == KEY_TRUE || first_key == KEY_FALSE {
                name = if head_count == 0 {
                    "false | true".to_string()
                } else if first_key == KEY_TRUE {
                    "false".to_string()
                } else {
                    "true".to_string()
                };
            } else if first_key == KEY_LIST_EMPTY || first_key == KEY_LIST_CONS {
                if head_count == 0 {
                    name = "[] | [_, .._]".to_string();
                    key = 0;
                    count = 0;
                } else if first_key == KEY_LIST_EMPTY {
                    name = "[_, .._]".to_string();
                    key = KEY_LIST_CONS;
                    count = 2;
                } else {
                    name = "[]".to_string();
                    key = KEY_LIST_EMPTY;
                    count = 0;
                }
            }
        }

        if !self.charge_alloc(1, SIZEOF_MATCH_PATTERN) || !self.charge_alloc(count, SIZEOF_PTR) {
            return None;
        }
        Some(self.arena.alloc(MatchPattern {
            name,
            key,
            signature: 0,
            items: vec![None; count],
            data: None,
            choice: None,
        }))
    }

    fn partition(&mut self, rows: Option<u32>, width: u32, parent: Option<usize>) -> Option<usize> {
        let mut count = 0usize;
        let mut cur = rows;
        while let Some(r_idx) = cur {
            if !self.charge_work(1) {
                return None;
            }
            let row = self.rows[r_idx as usize];
            let cell = self.cells[row.cells? as usize];
            if cell.pattern.is_some() {
                count += 1;
            }
            cur = row.next;
        }

        if !self.charge_alloc(1, SIZEOF_FRAME) || !self.charge_alloc(count, SIZEOF_HEAD) {
            return None;
        }

        let mut defaults = None;
        let mut heads = Vec::with_capacity(count);
        let mut cur = rows;
        while let Some(r_idx) = cur {
            let row = self.rows[r_idx as usize];
            let cell = self.cells[row.cells? as usize];
            if let Some(pat) = cell.pattern {
                heads.push(Head {
                    pattern: pat,
                    rest: cell.next,
                });
            } else if !self.add_row(&mut defaults, cell.next) {
                return None;
            }
            cur = row.next;
        }

        let complete = if count == 0 {
            false
        } else {
            let mut n = count as u32;
            while n != 0 {
                if !self.charge_work(count as u32) {
                    return None;
                }
                n >>= 1;
            }
            heads.sort_by_key(|h| self.arena.get(h.pattern).key);
            let mut distinct = 1u32;
            for i in 1..count {
                if self.arena.get(heads[i].pattern).key != self.arena.get(heads[i - 1].pattern).key
                {
                    distinct += 1;
                }
            }
            distinct == self.arena.get(heads[0].pattern).signature
        };

        if self.failed {
            return None;
        }
        let idx = self.frames.len();
        self.frames.push(Frame {
            parent,
            defaults,
            heads,
            at: 0,
            end: 0,
            width,
            chosen: None,
            complete,
        });
        Some(idx)
    }

    fn specialize_cells(
        &mut self,
        pattern: Option<MatchPatternId>,
        arity: usize,
        mut rest: Option<u32>,
    ) -> Option<Option<u32>> {
        for i in (0..arity).rev() {
            let sub = pattern.and_then(|pid| self.arena.get(pid).items[i]);
            let next = self.prepend(sub, rest)?;
            rest = Some(next);
        }
        Some(rest)
    }

    fn specialize(&mut self, frame_idx: usize) -> Option<Option<u32>> {
        let at = self.frames[frame_idx].at;
        let chosen_id = self.frames[frame_idx].heads[at].pattern;
        let chosen_key = self.arena.get(chosen_id).key;
        let chosen_arity = self.arena.get(chosen_id).items.len();
        let mut end = at + 1;
        while end < self.frames[frame_idx].heads.len()
            && self
                .arena
                .get(self.frames[frame_idx].heads[end].pattern)
                .key
                == chosen_key
        {
            end += 1;
        }
        self.frames[frame_idx].chosen = Some(chosen_id);
        self.frames[frame_idx].end = end;

        let mut out = None;
        for i in at..end {
            let h = self.frames[frame_idx].heads[i];
            let cells = self.specialize_cells(Some(h.pattern), chosen_arity, h.rest)?;
            if !self.add_row(&mut out, cells) {
                return None;
            }
        }
        let mut cur = self.frames[frame_idx].defaults;
        while let Some(r_idx) = cur {
            let row = self.rows[r_idx as usize];
            let cells = self.specialize_cells(None, chosen_arity, row.cells)?;
            if !self.add_row(&mut out, cells) {
                return None;
            }
            cur = row.next;
        }
        Some(out)
    }

    fn render(&mut self, witness_cell: u32) -> Option<String> {
        if !self.charge_alloc(4096, 1) || !self.charge_alloc(1, SIZEOF_RENDERING) {
            return None;
        }
        let root_pat = self.cells[witness_cell as usize].pattern;
        debug_assert!(self.cells[witness_cell as usize].next.is_none());

        struct RenderFrame {
            pattern: Option<MatchPatternId>,
            at: usize,
            opened: bool,
        }

        let mut stack = vec![RenderFrame {
            pattern: root_pat,
            at: 0,
            opened: false,
        }];
        let mut text = String::new();

        while let Some(top) = stack.last_mut() {
            if text.len() >= 4000 {
                break;
            }
            if !self.charge_work(1) {
                return None;
            }
            let Some(pid) = top.pattern else {
                append_bounded(&mut text, "_");
                stack.pop();
                continue;
            };
            let pat = self.arena.get(pid);
            let pat_key = pat.key;
            let pat_count = pat.items.len();
            if pat_count == 0 {
                let builtin = u32::try_from(pat_key).ok().and_then(builtin_case_by_key);
                qualify(&mut text, builtin);
                append_bounded(&mut text, &pat.name);
                stack.pop();
                continue;
            }
            if !top.opened {
                if pat_key != KEY_LIST_CONS && pat_key != KEY_TUPLE {
                    let builtin = u32::try_from(pat_key).ok().and_then(builtin_case_by_key);
                    qualify(&mut text, builtin);
                    append_bounded(&mut text, &pat.name);
                }
                append_bounded(&mut text, if pat_key == KEY_LIST_CONS { "[" } else { "(" });
                top.opened = true;
            }
            if top.at == pat_count {
                append_bounded(&mut text, if pat_key == KEY_LIST_CONS { "]" } else { ")" });
                stack.pop();
                continue;
            }
            if pat_key == KEY_LIST_CONS && top.at == 1 {
                let tail = pat.items[1];
                match tail {
                    None => {
                        append_bounded(&mut text, ", .._");
                        top.at = 2;
                        continue;
                    }
                    Some(tid) if self.arena.get(tid).key == KEY_LIST_EMPTY => {
                        top.at = 2;
                        continue;
                    }
                    Some(tid) => {
                        debug_assert_eq!(self.arena.get(tid).key, KEY_LIST_CONS);
                        append_bounded(&mut text, ", ");
                        top.pattern = Some(tid);
                        top.at = 0;
                        continue;
                    }
                }
            }
            if top.at != 0 {
                append_bounded(&mut text, ", ");
            }
            if let Some(data_id) = pat.data {
                let d = &self.signatures.data[data_id.index()];
                if !d.unnamed
                    && let Some(field_name) = d.field_by_index(top.at).name.as_deref()
                {
                    append_bounded(&mut text, field_name);
                    append_bounded(&mut text, ": ");
                }
            }
            if let Some(builtin) = u32::try_from(pat_key).ok().and_then(builtin_case_by_key)
                && let Some(field_name) = builtin.fields[0].and_then(|_| builtin.fields[top.at])
            {
                append_bounded(&mut text, field_name);
                append_bounded(&mut text, ": ");
            }
            let child_pat = pat.items[top.at];
            top.at += 1;
            if !self.charge_alloc(1, SIZEOF_RENDERING) {
                return None;
            }
            stack.push(RenderFrame {
                pattern: child_pat,
                at: 0,
                opened: false,
            });
        }

        if !stack.is_empty() {
            text.push_str("...");
        }
        Some(text)
    }
}

fn append_bounded(text: &mut String, s: &str) {
    let room = 4000usize.saturating_sub(text.len());
    if room == 0 {
        return;
    }
    if s.len() <= room {
        text.push_str(s);
    } else {
        let mut end = room;
        while end > 0 && !s.is_char_boundary(end) {
            end -= 1;
        }
        text.push_str(&s[..end]);
    }
}

fn qualify(text: &mut String, builtin: Option<&'static crate::types::BuiltinCase>) {
    if let Some(data) = builtin
        && let Some(module) = data.module
    {
        append_bounded(text, module);
        append_bounded(text, ".");
    }
}

/// Outcome of [`patterns_cover`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CoverageOutcome {
    /// Patterns exhaustively cover the subject type.
    Exhaustive,
    /// Patterns miss at least one case, described by the bounded witness preview.
    Missing(String),
    /// A work or arena limit was exceeded (already reported to `diags`).
    LimitFailed,
}

/// Checks whether `patterns` exhaustively cover `subject_ty` (`mo_patterns_cover`).
#[must_use]
#[expect(
    clippy::too_many_arguments,
    reason = "Maranget coverage entry threads shared arenas, budgets, and diagnostics without wrapper overhead"
)]
pub fn patterns_cover(
    arena: &mut MatchPatternArena,
    interfaces: &ModuleInterfaces,
    signatures: &Signatures,
    patterns: &[Option<MatchPatternId>],
    subject_ty: TypeId,
    work: &mut u32,
    program_work: &mut u64,
    arena_remaining: &mut usize,
    diags: &mut DiagBag,
    span: Span,
) -> CoverageOutcome {
    let mut cov = Coverage {
        arena,
        interfaces,
        signatures,
        diags,
        span,
        work,
        program_work,
        arena_remaining,
        cells: Vec::new(),
        rows: Vec::new(),
        frames: Vec::new(),
        failed: false,
    };

    let mut rows = None;
    for &pat in patterns {
        let Some(cell) = cov.prepend(pat, None) else {
            return CoverageOutcome::LimitFailed;
        };
        if !cov.add_row(&mut rows, Some(cell)) {
            return CoverageOutcome::LimitFailed;
        }
    }

    if patterns.is_empty() {
        let ty = cov.signatures.types.get(subject_ty);
        let mut domain_choice = None;
        let mut domain_key = 0u64;
        let mut domain_sig = 0u32;

        if let Some(sym_ref) = ty.owner.as_source()
            && cov.interfaces.symbol(sym_ref).decl_kind == SyntaxKind::Enum
        {
            domain_choice = Some(sym_ref);
        } else if ty.kind == SourceTypeKind::Named && ty.module.is_none() {
            for key in 6u32..=40u32 {
                if let Some(builtin) = builtin_case_by_key(key)
                    && builtin.ty == ty.name
                {
                    domain_key = u64::from(key);
                    break;
                }
            }
            if ty.name == "Bool" {
                domain_key = KEY_TRUE;
                domain_sig = 2;
            } else if ty.name == "List" {
                domain_key = KEY_LIST_EMPTY;
                domain_sig = 2;
            }
        }

        if domain_key != 0 || domain_choice.is_some() {
            let Some(hole) = cov.missing_head(None, domain_choice, domain_key, domain_sig) else {
                return CoverageOutcome::LimitFailed;
            };
            let Some(witness) = cov.prepend(Some(hole), None) else {
                return CoverageOutcome::LimitFailed;
            };
            let Some(rendered) = cov.render(witness) else {
                return CoverageOutcome::LimitFailed;
            };
            return if cov.failed {
                CoverageOutcome::LimitFailed
            } else {
                CoverageOutcome::Missing(rendered)
            };
        }
        return CoverageOutcome::Missing("every value".to_string());
    }

    let mut width = 1u32;
    let mut stack: Option<usize> = None;

    loop {
        if !cov.charge_work(1) {
            return CoverageOutcome::LimitFailed;
        }
        if rows.is_none() {
            let mut witness: Option<u32> = None;
            for _ in 0..width {
                let Some(next) = cov.prepend(None, witness) else {
                    return CoverageOutcome::LimitFailed;
                };
                witness = Some(next);
            }
            let mut cur_frame = stack;
            while let Some(fi) = cur_frame {
                let complete = cov.frames[fi].complete;
                let head_count = cov.frames[fi].heads.len();
                let mut chosen = if complete || head_count == 0 {
                    cov.frames[fi].chosen
                } else {
                    let first_id = cov.frames[fi].heads[0].pattern;
                    let (choice, key, sig) = {
                        let p = cov.arena.get(first_id);
                        (p.choice, p.key, p.signature)
                    };
                    let Some(hole) = cov.missing_head(Some(fi), choice, key, sig) else {
                        return CoverageOutcome::LimitFailed;
                    };
                    Some(hole)
                };
                if cov.failed {
                    return CoverageOutcome::LimitFailed;
                }
                if complete {
                    let Some(chosen_id) = chosen else {
                        return CoverageOutcome::LimitFailed;
                    };
                    let (name, key, signature, arity, data, choice) = {
                        let p = cov.arena.get(chosen_id);
                        (
                            p.name.clone(),
                            p.key,
                            p.signature,
                            p.items.len(),
                            p.data,
                            p.choice,
                        )
                    };
                    if !cov.charge_alloc(1, SIZEOF_MATCH_PATTERN)
                        || !cov.charge_alloc(arity, SIZEOF_PTR)
                        || !cov.charge_work(arity as u32)
                    {
                        return CoverageOutcome::LimitFailed;
                    }
                    let mut items = Vec::with_capacity(arity);
                    for _ in 0..arity {
                        let Some(w_idx) = witness else {
                            return CoverageOutcome::LimitFailed;
                        };
                        let cell = cov.cells[w_idx as usize];
                        items.push(cell.pattern);
                        witness = cell.next;
                    }
                    let node_id = cov.arena.alloc(MatchPattern {
                        name,
                        key,
                        signature,
                        items,
                        data,
                        choice,
                    });
                    chosen = Some(node_id);
                }
                let Some(next) = cov.prepend(chosen, witness) else {
                    return CoverageOutcome::LimitFailed;
                };
                witness = Some(next);
                cur_frame = cov.frames[fi].parent;
            }
            let Some(root_cell) = witness else {
                return CoverageOutcome::LimitFailed;
            };
            let Some(rendered) = cov.render(root_cell) else {
                return CoverageOutcome::LimitFailed;
            };
            return if cov.failed {
                CoverageOutcome::LimitFailed
            } else {
                CoverageOutcome::Missing(rendered)
            };
        }

        if width != 0 {
            let Some(fi) = cov.partition(rows, width, stack) else {
                return CoverageOutcome::LimitFailed;
            };
            stack = Some(fi);
            if !cov.frames[fi].complete {
                rows = cov.frames[fi].defaults;
                width -= 1;
                continue;
            }
            let Some(next_rows) = cov.specialize(fi) else {
                return CoverageOutcome::LimitFailed;
            };
            rows = next_rows;
            let chosen_arity = cov.frames[fi]
                .chosen
                .map_or(0, |cid| cov.arena.get(cid).items.len() as u32);
            width = cov.frames[fi].width - 1 + chosen_arity;
            continue;
        }

        while let Some(fi) = stack {
            if cov.frames[fi].complete && cov.frames[fi].end < cov.frames[fi].heads.len() {
                cov.frames[fi].at = cov.frames[fi].end;
                let Some(next_rows) = cov.specialize(fi) else {
                    return CoverageOutcome::LimitFailed;
                };
                rows = next_rows;
                let chosen_arity = cov.frames[fi]
                    .chosen
                    .map_or(0, |cid| cov.arena.get(cid).items.len() as u32);
                width = cov.frames[fi].width - 1 + chosen_arity;
                break;
            }
            stack = cov.frames[fi].parent;
        }
        if stack.is_none() {
            return CoverageOutcome::Exhaustive;
        }
    }
}
