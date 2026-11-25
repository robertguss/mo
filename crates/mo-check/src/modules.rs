//! Module interfaces, declarations, overload ownership, exposure, and explicit imports
//! (`src/check/modules.c` parity).

use mo_base::{DiagBag, Diagnostic, Edit, Fix, Label, Span};
use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_MODULE_IMPORTS, MO_LIMIT_PROGRAM_IMPORTS};
use mo_syntax::{NodeId, SyntaxKind, SyntaxTree, TokenKind};

use crate::ids::{ModuleId, SymIdx, SymRef};
use crate::load::ModuleSet;

/// Names always in scope (reference 10.5), including native choice cases
/// (`src/check/modules.c:mo_source_builtin`).
pub const SOURCE_BUILTINS: [&str; 55] = [
    "Bool",
    "Int",
    "BigInt",
    "Byte",
    "Decimal",
    "Float",
    "Text",
    "Char",
    "Bytes",
    "Unit",
    "List",
    "Map",
    "Set",
    "Range",
    "Time",
    "Duration",
    "Option",
    "None",
    "Some",
    "Result",
    "Ok",
    "Error",
    "Ordering",
    "Less",
    "Equal",
    "Greater",
    "Args",
    "Env",
    "Console",
    "Files",
    "ReadFiles",
    "Clock",
    "Random",
    "SecureRandom",
    "FileError",
    "NotFound",
    "NotAllowed",
    "NotText",
    "Failed",
    "Entry",
    "EntryKind",
    "File",
    "Folder",
    "Link",
    "Other",
    "Simulation",
    "SimulationError",
    "ServiceDown",
    "ToolFailed",
    "Network",
    "Connection",
    "NetworkError",
    "Sqlite",
    "SqliteError",
    "Actor",
];

/// Returns whether `name` is one of the 55 names always in scope (reference 10.5).
#[must_use]
pub fn is_source_builtin(name: &[u8]) -> bool {
    SOURCE_BUILTINS.iter().any(|&b| b.as_bytes() == name)
}

/// One top-level declaration or choice case in a module (`struct mo_symbol`).
#[derive(Debug, Clone)]
pub struct Symbol {
    pub name: String,
    pub declaration: NodeId,
    pub declaration_span: Span,
    pub name_node: NodeId,
    pub name_span: Span,
    pub decl_kind: SyntaxKind,
    /// Owning choice type's symbol index in the same module, or `None`.
    pub owner: Option<SymIdx>,
    /// First parameter's local nominal type symbol index (or `from_json` Ok type), or `None`.
    pub receiver: Option<SymIdx>,
    /// Name-group size on its first overload member; 1 on later members.
    pub overload_count: u32,
    /// Choice's case symbol indices in source declaration order.
    pub cases: Vec<SymIdx>,
    pub module: ModuleId,
    pub exported: bool,
    /// Whether type fields/cases are exposed via `(..)`.
    pub insides: bool,
}

/// One explicitly imported name (or expanded exposed choice case) (`struct mo_imported_name`).
#[derive(Debug, Clone)]
pub struct ImportedName {
    pub name: String,
    /// First symbol of the imported overload group (all overloads are adjacent in `target_module`).
    pub symbol: SymRef,
    pub count: u32,
    /// Syntax node of the written name in the importing module's `use` list.
    pub origin: NodeId,
    pub origin_span: Span,
}

/// One imported module prefix (`struct mo_module_alias`).
#[derive(Debug, Clone)]
pub struct ModuleAlias {
    pub name: String,
    pub module: ModuleId,
    /// Syntax node of the `use` line in the importing module.
    pub origin: NodeId,
    pub origin_span: Span,
}

/// Symbol, import, and alias tables for one module (`struct mo_module_scope`).
#[derive(Debug, Clone)]
pub struct ModuleScope {
    pub name: Option<String>,
    pub library: bool,
    /// Sorted by `(name, declaration_span.start)`.
    pub symbols: Vec<Symbol>,
    /// Sorted by `(name, origin_span.start)`.
    pub imported: Vec<ImportedName>,
    /// Sorted by `(name, origin_span.start)`.
    pub aliases: Vec<ModuleAlias>,
}

impl ModuleScope {
    /// Binary search for the first symbol with `name` (`mo_find_symbol`).
    #[must_use]
    pub fn find_symbol_index(&self, name: &[u8]) -> Option<SymIdx> {
        let mut lo = 0usize;
        let mut hi = self.symbols.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.symbols[mid].name.as_bytes() < name {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.symbols.len() && self.symbols[lo].name.as_bytes() == name {
            Some(SymIdx(lo as u32))
        } else {
            None
        }
    }

    #[must_use]
    pub fn find_symbol(&self, name: &[u8]) -> Option<&Symbol> {
        self.find_symbol_index(name)
            .map(|idx| &self.symbols[idx.index()])
    }

    #[must_use]
    pub fn symbol(&self, idx: SymIdx) -> &Symbol {
        &self.symbols[idx.index()]
    }

    /// Binary search for an imported module alias (`alias`).
    #[must_use]
    pub fn find_alias(&self, name: &[u8]) -> Option<&ModuleAlias> {
        let mut lo = 0usize;
        let mut hi = self.aliases.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.aliases[mid].name.as_bytes() < name {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.aliases.len() && self.aliases[lo].name.as_bytes() == name {
            Some(&self.aliases[lo])
        } else {
            None
        }
    }

    /// Binary search for an explicitly imported name.
    #[must_use]
    pub fn find_imported(&self, name: &[u8]) -> Option<&ImportedName> {
        let mut lo = 0usize;
        let mut hi = self.imported.len();
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.imported[mid].name.as_bytes() < name {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if lo < self.imported.len() && self.imported[lo].name.as_bytes() == name {
            Some(&self.imported[lo])
        } else {
            None
        }
    }
}

/// Resolved module interfaces across the entire loaded program (`struct mo_module_interfaces`).
#[derive(Debug, Clone)]
pub struct ModuleInterfaces {
    pub scopes: Vec<ModuleScope>,
}

impl ModuleInterfaces {
    #[must_use]
    pub fn len(&self) -> usize {
        self.scopes.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.scopes.is_empty()
    }

    #[must_use]
    pub fn scope(&self, module: ModuleId) -> &ModuleScope {
        &self.scopes[module.index()]
    }

    #[must_use]
    pub fn symbol(&self, sym: SymRef) -> &Symbol {
        &self.scopes[sym.module.index()].symbols[sym.symbol.index()]
    }
}

#[derive(Debug, Clone, Copy, Default)]
struct Requested {
    /// `requesting_module + 1`, or `0` if unused.
    module: u32,
    /// Explicitly listed by `requesting_module + 1`, including later `use` lines.
    listed: u32,
    origin: Option<(NodeId, Span)>,
}

struct Resolver<'a, 'd> {
    set: &'a ModuleSet,
    diags: &'d mut DiagBag,
    scopes: Vec<ModuleScope>,
    requested: Vec<Vec<Requested>>,
    valid: Vec<bool>,
    expanded: u64,
    failed: bool,
    errors: bool,
    import_limit: bool,
    module_imports_limit: u64,
    program_imports_limit: u64,
}

impl<'a, 'd> Resolver<'a, 'd> {
    fn error(
        &mut self,
        code: ErrorCode,
        span: Span,
        message: &str,
        reason: Option<&str>,
        name: &str,
        previous: Option<Span>,
    ) -> bool {
        self.errors = true;
        let mut diag = Diagnostic::new(code, span, message).with_primary_text(name);
        if let Some(r) = reason {
            diag = diag.with_reason(r);
        }
        if let Some(prev) = previous {
            diag = diag.with_secondary(Label::with_text(prev, "declared or imported here"));
        }
        let kept = self.diags.push(diag).unwrap_or(false);
        self.failed = !kept;
        kept
    }

    fn reserved(&mut self, span: Span, name: &str) -> bool {
        self.error(
            ErrorCode::AlwaysInScope,
            span,
            "a name clashes with a name always in scope",
            Some(
                "A clash between names is an error (reference 10.3); these names are always \
                 in scope (reference 10.5).",
            ),
            name,
            None,
        )
    }

    fn declarations(&mut self, mod_idx: usize) -> bool {
        let mod_id = ModuleId(mod_idx as u32);
        let module = &self.set.modules[mod_idx];
        let tree = &module.syntax.tree;
        let Some(Some(items_nid)) = tree.node(module.syntax.root).children.get(1) else {
            return true;
        };
        let items = &tree.node(*items_nid).children;

        // Track original insertion index in a parallel vector for remapping owners and
        // preserving source declaration order of enum cases.
        let mut raw_symbols: Vec<(Symbol, usize, Option<usize>)> = Vec::new();
        for child in items {
            let Some(nid) = *child else {
                continue;
            };
            let node = tree.node(nid);
            if matches!(
                node.kind,
                SyntaxKind::Test | SyntaxKind::Service | SyntaxKind::Api
            ) {
                continue;
            }
            let owner_orig = raw_symbols.len();
            let Some(sym) = make_symbol(tree, mod_id, nid) else {
                continue;
            };
            raw_symbols.push((sym, owner_orig, None));
            if node.kind == SyntaxKind::Enum
                && let Some(Some(cases_nid)) = node.children.get(2)
            {
                for case_child in &tree.node(*cases_nid).children {
                    if let Some(case_nid) = *case_child
                        && let Some(case_sym) = make_symbol(tree, mod_id, case_nid)
                    {
                        let orig = raw_symbols.len();
                        raw_symbols.push((case_sym, orig, Some(owner_orig)));
                    }
                }
            }
        }

        raw_symbols.sort_by(|(a, _, _), (b, _, _)| {
            a.name
                .as_bytes()
                .cmp(b.name.as_bytes())
                .then_with(|| a.declaration_span.start.cmp(&b.declaration_span.start))
        });

        let count = raw_symbols.len();
        let mut remap = vec![0usize; count];
        for (sorted_idx, (_, orig_idx, _)) in raw_symbols.iter().enumerate() {
            remap[*orig_idx] = sorted_idx;
        }

        let mut symbols: Vec<Symbol> = raw_symbols
            .into_iter()
            .map(|(mut sym, _, raw_owner)| {
                sym.owner = raw_owner.map(|orig| SymIdx(remap[orig] as u32));
                sym
            })
            .collect();

        // Populate each enum's `cases` in original source declaration order.
        for &sorted_idx in &remap {
            if let Some(owner_idx) = symbols[sorted_idx].owner {
                symbols[owner_idx.index()]
                    .cases
                    .push(SymIdx(sorted_idx as u32));
            }
        }

        // Compute receiver nominal type for every function declaration.
        for i in 0..count {
            if symbols[i].decl_kind != SyntaxKind::Fn {
                continue;
            }
            let decl_node = tree.node(symbols[i].declaration);
            let params_nid = decl_node.children.get(1).copied().flatten();
            let param_count = params_nid.map_or(0, |p| tree.node(p).children.len());
            let first_type = params_nid
                .and_then(|p| tree.node(p).children.first().copied().flatten())
                .and_then(|param| tree.node(param).children.get(1).copied().flatten());

            if let Some(ty_nid) = first_type {
                let ty_node = tree.node(ty_nid);
                if ty_node.token != TokenKind::Fn
                    && ty_node.token != TokenKind::LParen
                    && let Some(name_nid) = ty_node.children.first().copied().flatten()
                    && tree.node(name_nid).kind == SyntaxKind::CapName
                    && let Some(text) = tree.node_text(name_nid)
                    && let Some(rec_idx) = find_symbol_in_slice(&symbols, text)
                    && matches!(
                        symbols[rec_idx].decl_kind,
                        SyntaxKind::Struct | SyntaxKind::Enum
                    )
                {
                    symbols[i].receiver = Some(SymIdx(rec_idx as u32));
                }
            }

            if symbols[i].receiver.is_none()
                && symbols[i].name == "from_json"
                && param_count == 1
                && let Some(ret_nid) = decl_node.children.get(2).copied().flatten()
            {
                let ret_node = tree.node(ret_nid);
                if ret_node.token != TokenKind::Fn
                    && ret_node.token != TokenKind::LParen
                    && let Some(ret_name_nid) = ret_node.children.first().copied().flatten()
                    && tree.node(ret_name_nid).kind == SyntaxKind::CapName
                    && tree.node_text(ret_name_nid) == Some(b"Result")
                    && find_symbol_in_slice(&symbols, b"Result").is_none()
                    && let Some(ret_args_nid) = ret_node.children.get(1).copied().flatten()
                    && tree.node(ret_args_nid).children.len() == 2
                    && let Some(ok_nid) = tree.node(ret_args_nid).children[0]
                {
                    let ok_node = tree.node(ok_nid);
                    if ok_node.token != TokenKind::Fn
                        && ok_node.token != TokenKind::LParen
                        && let Some(ok_name_nid) = ok_node.children.first().copied().flatten()
                        && tree.node(ok_name_nid).kind == SyntaxKind::CapName
                        && let Some(ok_text) = tree.node_text(ok_name_nid)
                        && let Some(rec_idx) = find_symbol_in_slice(&symbols, ok_text)
                        && matches!(
                            symbols[rec_idx].decl_kind,
                            SyntaxKind::Struct | SyntaxKind::Enum
                        )
                    {
                        symbols[i].receiver = Some(SymIdx(rec_idx as u32));
                    }
                }
            }
        }

        // Check name clashes, always-in-scope clashes, and overload receiver ownership.
        let mut seen = vec![0usize; count];
        let mut i = 0usize;
        while i < count {
            let mut end = i + 1;
            while end < count && symbols[end].name == symbols[i].name {
                end += 1;
            }
            let mut functions = true;
            let mut all_cases = true;
            for sym in &symbols[i..end] {
                functions = functions && sym.decl_kind == SyntaxKind::Fn;
                all_cases = all_cases && sym.owner.is_some();
            }

            if is_source_builtin(symbols[i].name.as_bytes()) {
                let first_span = symbols[i].name_span;
                let first_name = symbols[i].name.clone();
                let kept = if all_cases {
                    self.error(
                        ErrorCode::CaseClash,
                        first_span,
                        "a choice case clashes with another name",
                        Some(
                            "No case reuses a type or case that is always in scope \
                             (reference 4.2).",
                        ),
                        &first_name,
                        None,
                    )
                } else {
                    self.reserved(first_span, &first_name)
                };
                if !kept {
                    return false;
                }
            } else if end - i > 1 && !functions {
                let first_span = symbols[i].name_span;
                let first_name = symbols[i].name.clone();
                for sym in &symbols[i + 1..end] {
                    let kept = if all_cases {
                        self.error(
                            ErrorCode::CaseClash,
                            sym.name_span,
                            "a choice case clashes with another name",
                            Some("A module's cases share one name space (reference 4.2)."),
                            &first_name,
                            Some(first_span),
                        )
                    } else {
                        self.error(
                            ErrorCode::NameClash,
                            sym.name_span,
                            "a clash between names",
                            Some("A clash between names is an error (reference 10.3)."),
                            &first_name,
                            Some(first_span),
                        )
                    };
                    if !kept {
                        return false;
                    }
                }
            } else if end - i > 1 {
                symbols[i].overload_count = (end - i) as u32;
                for j in i..end {
                    let rec = symbols[j].receiver;
                    let dup = rec.is_none_or(|r| seen[r.index()] == i + 1);
                    if dup {
                        let other_span = symbols[if j == i { i + 1 } else { i }].name_span;
                        let name = symbols[j].name.clone();
                        let span = symbols[j].name_span;
                        if !self.error(
                            ErrorCode::Overload,
                            span,
                            "the functions don't have different local receiver types",
                            Some(
                                "Two functions share a name only when their first parameters are \
                                 different types declared in that module (reference 5.1).",
                            ),
                            &name,
                            Some(other_span),
                        ) {
                            return false;
                        }
                        break;
                    }
                    if let Some(r) = rec {
                        seen[r.index()] = i + 1;
                    }
                }
            }
            i = end;
        }

        self.requested[mod_idx] = vec![Requested::default(); count];
        self.scopes[mod_idx].symbols = symbols;
        true
    }

    fn expose(&mut self, mod_idx: usize) -> bool {
        let module = &self.set.modules[mod_idx];
        let tree = &module.syntax.tree;
        if let Some(list_nid) = module.syntax.header.and_then(|h| h.expose) {
            for item_opt in &tree.node(list_nid).children {
                let Some(item_nid) = *item_opt else {
                    continue;
                };
                let item_node = tree.node(item_nid);
                let all = item_node.kind == SyntaxKind::ExposeAll;
                let node_nid = if all {
                    let Some(inner) = item_node.children.first().copied().flatten() else {
                        continue;
                    };
                    inner
                } else {
                    item_nid
                };
                let node_span = tree.node(node_nid).span;
                let Some(name_bytes) = tree.node_text(node_nid) else {
                    continue;
                };
                let found = self.scopes[mod_idx].find_symbol_index(name_bytes);
                let Some(sym_idx) = found else {
                    let name = String::from_utf8_lossy(name_bytes).into_owned();
                    if !self.error(
                        ErrorCode::ExposeName,
                        node_span,
                        "an exposed name doesn't exist",
                        Some("A module's expose line lists its public names (reference 10.1)."),
                        &name,
                        None,
                    ) {
                        return false;
                    }
                    continue;
                };
                let (owner, decl_kind, sym_name, sym_name_span, owner_name_span) = {
                    let scope = &self.scopes[mod_idx];
                    let sym = scope.symbol(sym_idx);
                    let owner_span = sym.owner.map(|o| scope.symbol(o).name_span);
                    (
                        sym.owner,
                        sym.decl_kind,
                        sym.name.clone(),
                        sym.name_span,
                        owner_span,
                    )
                };
                if let Some(owner_span) = owner_name_span {
                    if !self.error(
                        ErrorCode::ExposeInsides,
                        item_node.span,
                        "a case is exposed without its type",
                        Some(
                            "Exposing a type without (..) hides its fields or cases \
                             (reference 10.2).",
                        ),
                        &sym_name,
                        Some(owner_span),
                    ) {
                        return false;
                    }
                    continue;
                }
                let _ = owner;
                if all && decl_kind == SyntaxKind::Actor {
                    if !self.error(
                        ErrorCode::ExposeInsides,
                        item_node.span,
                        "an actor has no insides to expose",
                        Some("Expose an actor by name without (..) (reference 11.7)."),
                        &sym_name,
                        Some(sym_name_span),
                    ) {
                        return false;
                    }
                    continue;
                }
                let scope = &mut self.scopes[mod_idx];
                let first = &scope.symbols[sym_idx.index()];
                if first.exported && (!all || first.insides) {
                    continue;
                }
                let target_name = first.name.clone();
                let mut at = sym_idx.index();
                while at < scope.symbols.len() && scope.symbols[at].name == target_name {
                    scope.symbols[at].exported = true;
                    scope.symbols[at].insides = scope.symbols[at].insides || all;
                    at += 1;
                }
            }
        }

        let scope = &mut self.scopes[mod_idx];
        let insides_flags: Vec<bool> = scope.symbols.iter().map(|s| s.insides).collect();
        for sym in &mut scope.symbols {
            if let Some(owner) = sym.owner {
                sym.exported = insides_flags[owner.index()];
            }
        }
        true
    }

    fn case_import(
        &mut self,
        target_mod: usize,
        sym_idx: SymIdx,
        tree: &SyntaxTree,
        names_nid: NodeId,
        index: usize,
        importing_mod: usize,
    ) -> bool {
        let names_children = &tree.node(names_nid).children;
        let Some(node_nid) = names_children[index] else {
            return true;
        };
        let node_span = tree.node(node_nid).span;

        let (sym_name, sym_exported, owner_idx, owner_name, owner_name_span) = {
            let other = &self.scopes[target_mod];
            let sym = other.symbol(sym_idx);
            let Some(o_idx) = sym.owner else {
                return true;
            };
            let owner = other.symbol(o_idx);
            (
                sym.name.clone(),
                sym.exported,
                o_idx,
                owner.name.clone(),
                owner.name_span,
            )
        };

        let already =
            self.requested[target_mod][owner_idx.index()].listed == (importing_mod as u32) + 1;
        let offer = sym_exported && (!already || names_children.len() > 1);

        let fix = if offer {
            if already && names_children.len() > 1 {
                let start = if index == 0 {
                    node_span.start
                } else {
                    names_children[index - 1].map_or(node_span.start, |p| tree.node(p).span.end)
                };
                let end = if index == 0 {
                    names_children[1].map_or(node_span.end, |n| tree.node(n).span.start)
                } else {
                    node_span.end
                };
                Some(Fix::new(
                    Some("remove the case; its type is already imported".into()),
                    vec![Edit::new(Span::new(node_span.file, start, end), "")],
                ))
            } else {
                Some(Fix::new(
                    Some("import the choice type instead".into()),
                    vec![Edit::new(node_span, owner_name)],
                ))
            }
        } else {
            None
        };

        let mut diag = Diagnostic::new(
            if sym_exported {
                ErrorCode::ImportCase
            } else {
                ErrorCode::ImportHiddenCase
            },
            node_span,
            if sym_exported {
                "a case is imported without its type"
            } else {
                "the choice's cases are hidden"
            },
        )
        .with_primary_text(sym_name)
        .with_reason(if sym_exported {
            "Importing a choice type brings its exposed cases with it (reference 10.3)."
        } else {
            "Exposing a type without (..) hides its fields or cases (reference 10.2)."
        })
        .with_secondary(Label::with_text(owner_name_span, "the owning choice type"));

        if let Some(f) = fix {
            diag = diag.with_fix(f);
        }
        self.errors = true;
        let kept = self.diags.push(diag).unwrap_or(false);
        self.failed = !kept;
        kept
    }

    fn imports(&mut self, mod_idx: usize) -> bool {
        let module = &self.set.modules[mod_idx];
        let tree = &module.syntax.tree;
        let Some(uses_nid) = module.syntax.header.map(|h| h.uses) else {
            return true;
        };
        let uses_children = &tree.node(uses_nid).children;
        let mod_marker = (mod_idx as u32) + 1;

        // First pass: mark all explicitly listed target symbols for case-import fix decisions.
        for (i, use_opt) in uses_children.iter().enumerate() {
            let Some(use_nid) = *use_opt else {
                continue;
            };
            let target_mod = module.uses[i].index();
            let Some(names_nid) = tree.node(use_nid).children.get(1).copied().flatten() else {
                continue;
            };
            if !self.valid[target_mod] {
                continue;
            }
            for name_opt in &tree.node(names_nid).children {
                if let Some(name_nid) = *name_opt
                    && let Some(text) = tree.node_text(name_nid)
                    && let Some(sym_idx) = self.scopes[target_mod].find_symbol_index(text)
                {
                    self.requested[target_mod][sym_idx.index()].listed = mod_marker;
                }
            }
        }

        // Second pass: validate aliases and explicit import items.
        let mut aliases: Vec<ModuleAlias> = Vec::with_capacity(uses_children.len());
        let mut import_count: u64 = 0;

        for (i, use_opt) in uses_children.iter().enumerate() {
            let Some(use_nid) = *use_opt else {
                continue;
            };
            let use_node = tree.node(use_nid);
            let target_mod = module.uses[i].index();
            let Some(path_nid) = use_node.children.first().copied().flatten() else {
                continue;
            };
            let path_children = &tree.node(path_nid).children;
            let Some(last_nid) = path_children.last().copied().flatten() else {
                continue;
            };
            let last_node = tree.node(last_nid);
            let Some(alias_bytes) = tree.node_text(last_nid) else {
                continue;
            };
            let alias = String::from_utf8_lossy(alias_bytes).into_owned();
            if is_source_builtin(alias.as_bytes()) {
                if !self.reserved(last_node.span, &alias) {
                    return false;
                }
            } else {
                aliases.push(ModuleAlias {
                    name: alias,
                    module: ModuleId(target_mod as u32),
                    origin: use_nid,
                    origin_span: use_node.span,
                });
            }

            let Some(names_nid) = use_node.children.get(1).copied().flatten() else {
                continue;
            };
            if !self.valid[target_mod] {
                continue;
            }
            let names_node = tree.node(names_nid);
            for (j, name_opt) in names_node.children.iter().enumerate() {
                let Some(node_nid) = *name_opt else {
                    continue;
                };
                let node_span = tree.node(node_nid).span;
                let Some(node_bytes) = tree.node_text(node_nid) else {
                    continue;
                };
                let found = self.scopes[target_mod].find_symbol_index(node_bytes);
                let text = match found {
                    Some(sym_idx) => self.scopes[target_mod].symbol(sym_idx).name.clone(),
                    None => String::from_utf8_lossy(node_bytes).into_owned(),
                };
                if is_source_builtin(text.as_bytes()) {
                    if !self.reserved(node_span, &text) {
                        return false;
                    }
                    continue;
                }
                let Some(sym_idx) = found else {
                    if !self.error(
                        ErrorCode::ImportMissing,
                        node_span,
                        "the imported name doesn't exist",
                        Some("Use brings in names declared by the module (reference 10.3)."),
                        &text,
                        None,
                    ) {
                        return false;
                    }
                    continue;
                };
                let (exported, owner, sym_name_span, insides, case_count) = {
                    let sym = self.scopes[target_mod].symbol(sym_idx);
                    (
                        sym.exported,
                        sym.owner,
                        sym.name_span,
                        sym.insides,
                        sym.cases.len() as u64,
                    )
                };
                if !exported && owner.is_none() {
                    if !self.error(
                        ErrorCode::ImportPrivate,
                        node_span,
                        "the imported name is private",
                        Some("Everything not on expose is private to the module (reference 10.1)."),
                        &text,
                        Some(sym_name_span),
                    ) {
                        return false;
                    }
                    continue;
                }
                if owner.is_some() {
                    if !self.case_import(target_mod, sym_idx, tree, names_nid, j, mod_idx) {
                        return false;
                    }
                    continue;
                }

                let req = self.requested[target_mod][sym_idx.index()];
                if req.module == mod_marker {
                    if let Some((_, prev_span)) = req.origin {
                        if prev_span.start >= names_node.span.start {
                            let prev_end = names_node.children[j - 1]
                                .map_or(node_span.start, |p| tree.node(p).span.end);
                            let edit = Edit::new(
                                Span::new(names_node.span.file, prev_end, node_span.end),
                                "",
                            );
                            let fix =
                                Fix::new(Some("remove the repeated import".into()), vec![edit]);
                            let diag = Diagnostic::new(
                                ErrorCode::NameClash,
                                node_span,
                                "a clash between names",
                            )
                            .with_primary_text(text)
                            .with_reason(
                                "A clash between names is an error; use the qualified form \
                                 (reference 10.3).",
                            )
                            .with_secondary(Label::with_text(prev_span, "first imported here"))
                            .with_fix(fix);
                            self.errors = true;
                            let kept = self.diags.push(diag).unwrap_or(false);
                            self.failed = !kept;
                            if !kept {
                                return false;
                            }
                        } else if !self.error(
                            ErrorCode::NameClash,
                            node_span,
                            "a clash between names",
                            Some(
                                "A clash between names is an error; use the qualified form \
                                 (reference 10.3).",
                            ),
                            &text,
                            Some(prev_span),
                        ) {
                            return false;
                        }
                    }
                    continue;
                }

                let added = 1 + if insides { case_count } else { 0 };
                if added > self.module_imports_limit.saturating_sub(import_count) {
                    let _ = self.error(
                        ErrorCode::TooManyImports,
                        node_span,
                        "the module imports too many names",
                        None,
                        &text,
                        None,
                    );
                    return false;
                }
                if added > self.program_imports_limit.saturating_sub(self.expanded) {
                    self.import_limit = true;
                    let _ = self.error(
                        ErrorCode::TooManyProgramImports,
                        node_span,
                        "the program imports too many names",
                        None,
                        &text,
                        None,
                    );
                    return false;
                }
                self.requested[target_mod][sym_idx.index()].module = mod_marker;
                self.requested[target_mod][sym_idx.index()].origin = Some((node_nid, node_span));
                import_count += added;
                self.expanded += added;
            }
        }

        // Third pass: materialize imported names and expanded exposed choice cases.
        let mut imported: Vec<ImportedName> = Vec::with_capacity(import_count as usize);
        for (i, use_opt) in uses_children.iter().enumerate() {
            let Some(use_nid) = *use_opt else {
                continue;
            };
            let Some(names_nid) = tree.node(use_nid).children.get(1).copied().flatten() else {
                continue;
            };
            let target_mod = module.uses[i].index();
            if !self.valid[target_mod] {
                continue;
            }
            for name_opt in &tree.node(names_nid).children {
                let Some(node_nid) = *name_opt else {
                    continue;
                };
                let node_span = tree.node(node_nid).span;
                let Some(node_bytes) = tree.node_text(node_nid) else {
                    continue;
                };
                let Some(sym_idx) = self.scopes[target_mod].find_symbol_index(node_bytes) else {
                    continue;
                };
                let req = self.requested[target_mod][sym_idx.index()];
                if req.module != mod_marker || req.origin.map(|(n, _)| n) != Some(node_nid) {
                    continue;
                }
                let other = &self.scopes[target_mod];
                let sym = other.symbol(sym_idx);
                imported.push(ImportedName {
                    name: sym.name.clone(),
                    symbol: SymRef::new(ModuleId(target_mod as u32), sym_idx),
                    count: sym.overload_count,
                    origin: node_nid,
                    origin_span: node_span,
                });
                if sym.insides {
                    for &case_idx in &sym.cases {
                        let member = other.symbol(case_idx);
                        imported.push(ImportedName {
                            name: member.name.clone(),
                            symbol: SymRef::new(ModuleId(target_mod as u32), case_idx),
                            count: 1,
                            origin: node_nid,
                            origin_span: node_span,
                        });
                    }
                }
            }
        }

        aliases.sort_by(|a, b| {
            a.name
                .as_bytes()
                .cmp(b.name.as_bytes())
                .then_with(|| a.origin_span.start.cmp(&b.origin_span.start))
        });
        for i in 1..aliases.len() {
            if aliases[i - 1].name == aliases[i].name
                && !self.error(
                    ErrorCode::NameClash,
                    aliases[i].origin_span,
                    "a clash between names",
                    Some("A clash between names is an error (reference 10.3)."),
                    &aliases[i].name,
                    Some(aliases[i - 1].origin_span),
                )
            {
                return false;
            }
        }

        imported.sort_by(|a, b| {
            a.name
                .as_bytes()
                .cmp(b.name.as_bytes())
                .then_with(|| a.origin_span.start.cmp(&b.origin_span.start))
        });
        for i in 0..imported.len() {
            let local_span = self.scopes[mod_idx]
                .find_symbol(imported[i].name.as_bytes())
                .map(|s| s.name_span);
            let mut previous = local_span;
            if i != 0 && imported[i - 1].name == imported[i].name {
                previous = Some(imported[i - 1].origin_span);
            }
            if let Some(prev_span) = previous
                && !self.error(
                    ErrorCode::NameClash,
                    imported[i].origin_span,
                    "a clash between names",
                    Some(
                        "A clash between names is an error; use the qualified form \
                         (reference 10.3).",
                    ),
                    &imported[i].name,
                    Some(prev_span),
                )
            {
                return false;
            }
        }

        self.scopes[mod_idx].aliases = aliases;
        self.scopes[mod_idx].imported = imported;
        true
    }
}

/// Resolves declarations, overloads, exposure, and explicit imports for every module in `set`
/// (`src/check/modules.c:mo_resolve_modules`).
#[must_use]
pub fn resolve_modules(set: &ModuleSet, diags: &mut DiagBag) -> Option<ModuleInterfaces> {
    resolve_modules_inner(
        set,
        diags,
        MO_LIMIT_MODULE_IMPORTS,
        MO_LIMIT_PROGRAM_IMPORTS,
    )
}

#[cfg(test)]
pub(crate) fn resolve_modules_with_limits(
    set: &ModuleSet,
    module_imports_limit: u64,
    program_imports_limit: u64,
    diags: &mut DiagBag,
) -> Option<ModuleInterfaces> {
    resolve_modules_inner(set, diags, module_imports_limit, program_imports_limit)
}

fn resolve_modules_inner(
    set: &ModuleSet,
    diags: &mut DiagBag,
    module_imports_limit: u64,
    program_imports_limit: u64,
) -> Option<ModuleInterfaces> {
    if set.is_empty() {
        return None;
    }
    let count = set.len();
    let scopes = set
        .modules
        .iter()
        .map(|m| ModuleScope {
            name: if m.name.is_empty() {
                None
            } else {
                Some(m.name.clone())
            },
            library: m.library,
            symbols: Vec::new(),
            imported: Vec::new(),
            aliases: Vec::new(),
        })
        .collect();

    let mut resolver = Resolver {
        set,
        diags,
        scopes,
        requested: vec![Vec::new(); count],
        valid: vec![false; count],
        expanded: 0,
        failed: false,
        errors: false,
        import_limit: false,
        module_imports_limit,
        program_imports_limit,
    };

    for i in 0..count {
        let before = resolver.diags.len();
        if !resolver.declarations(i) || !resolver.expose(i) {
            break;
        }
        resolver.valid[i] = before == resolver.diags.len();
    }

    if !resolver.failed {
        for i in 0..count {
            if resolver.import_limit {
                break;
            }
            let ok = resolver.imports(i);
            if !ok && resolver.failed {
                break;
            }
        }
    }

    let failed = resolver.errors || resolver.failed;
    let scopes = resolver.scopes;
    diags.sort();
    if failed {
        None
    } else {
        Some(ModuleInterfaces { scopes })
    }
}

fn make_symbol(tree: &SyntaxTree, module: ModuleId, decl: NodeId) -> Option<Symbol> {
    let decl_node = tree.node(decl);
    let name_nid = decl_node.children.first().copied().flatten()?;
    let name_node = tree.node(name_nid);
    let name_bytes = tree.node_text(name_nid)?;
    let name = String::from_utf8_lossy(name_bytes).into_owned();
    Some(Symbol {
        name,
        declaration: decl,
        declaration_span: decl_node.span,
        name_node: name_nid,
        name_span: name_node.span,
        decl_kind: decl_node.kind,
        owner: None,
        receiver: None,
        overload_count: 1,
        cases: Vec::new(),
        module,
        exported: false,
        insides: false,
    })
}

fn find_symbol_in_slice(symbols: &[Symbol], name: &[u8]) -> Option<usize> {
    let mut lo = 0usize;
    let mut hi = symbols.len();
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        if symbols[mid].name.as_bytes() < name {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if lo < symbols.len() && symbols[lo].name.as_bytes() == name {
        Some(lo)
    } else {
        None
    }
}
