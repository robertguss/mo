//! Whole-program module loader (`src/check/load.c` parity).
//!
//! Derives the module root from the entry path and its `module` header, discovers
//! standard-library modules (from disk or the compiled-in embedded library) honoring the
//! lazy `on_use` table, parses upward `mo.toml` manifests and path dependencies, enforces
//! symlink-under-root confinement and nonblocking regular-file reads, walks the module graph
//! with an explicit stack (refusing cycles with every edge labeled), and parses full module
//! syntax trees only after the complete graph succeeds.

use std::collections::HashMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

use mo_base::{
    DiagBag, Diagnostic, Label, SourceError, SourceMap, Span, module_path, utf8::is_valid_utf8,
};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_CLI_CHECK_SIZE, MO_LIMIT_DEPENDENCIES, MO_LIMIT_FILE_SIZE, MO_LIMIT_FILES,
    MO_LIMIT_LIBRARY_ENTRIES, MO_LIMIT_MANIFEST_SIZE, MO_LIMIT_PATH_SIZE,
};
use mo_syntax::{
    ModuleSyntax, NodeId, ParseOptions, ParsedHeader, SyntaxKind, SyntaxTree,
    parse_module_with_options, read_header_with_options,
};

use crate::embedded::{
    EmbeddedLibrary, EmbeddedSource, default_embedded_library, default_library_root,
};
use crate::ids::ModuleId;

/// Library modules loaded lazily only when a `use` line (or implicit `api`/`Sqlite` mention)
/// requests them (`src/check/load.c:on_use`).
pub const ON_USE_LIBRARY_MODULES: [&str; 20] = [
    "Api",
    "Arvo.Audit",
    "Arvo.Auth",
    "Arvo.Changeset",
    "Arvo.Cookie",
    "Arvo.Csrf",
    "Arvo.Flash",
    "Arvo.Form",
    "Arvo.Inertia",
    "Arvo.Jobs",
    "Arvo.Migrations",
    "Arvo.Session",
    "Arvo.WebTest",
    "Command",
    "Crypto",
    "Date",
    "Http",
    "Json",
    "Sqlite",
    "Unicode16",
];

#[must_use]
pub fn is_loaded_on_use(name: &str) -> bool {
    ON_USE_LIBRARY_MODULES.contains(&name)
}

#[must_use]
pub fn is_reserved_on_use(name: &str) -> bool {
    is_loaded_on_use(name) && !name.starts_with("Arvo.")
}

fn file_loaded_on_use(relative: &str) -> bool {
    for &name in &ON_USE_LIBRARY_MODULES {
        if let Ok(path) = module_path(name.as_bytes())
            && path.as_str() == relative
        {
            return true;
        }
    }
    false
}

/// How the loader locates standard-library modules.
#[derive(Debug, Clone)]
pub enum LibraryMode {
    /// Library-free mode (equivalent to C `library = NULL, embedded = NULL`).
    None,
    /// Load standard-library modules from a directory on disk (`--library DIR`).
    Directory(PathBuf),
    /// Load standard-library modules from an embedded table, optionally recognizing
    /// entry files inside `entry_root` as library module 0.
    Embedded {
        library: &'static EmbeddedLibrary,
        entry_root: Option<PathBuf>,
    },
}

impl LibraryMode {
    /// Default CLI mode: embedded standard library plus on-disk `lib/` recognition when present.
    #[must_use]
    pub fn default_embedded() -> Self {
        Self::Embedded {
            library: default_embedded_library(),
            entry_root: default_library_root(),
        }
    }
}

/// Options controlling [`load_program`].
#[derive(Debug, Clone)]
pub struct LoadOptions {
    pub library: LibraryMode,
    /// When `true`, synthesize empty `UpgradeOld` / `UpgradeOld.*` modules during bundle discovery.
    pub upgrade: bool,
    /// Shared syntax arena byte budget across all headers and parsed modules (`E1054`).
    pub syntax_arena_limit: usize,
}

impl Default for LoadOptions {
    fn default() -> Self {
        Self {
            library: LibraryMode::default_embedded(),
            upgrade: false,
            syntax_arena_limit: MO_CLI_CHECK_SIZE as usize,
        }
    }
}

/// One loaded module inside a [`ModuleSet`].
#[derive(Debug, Clone)]
pub struct LoadedModule {
    pub id: ModuleId,
    pub name: String,
    pub file: u32,
    pub library: bool,
    pub dependency: bool,
    /// Target module index for each `use` line in header order.
    pub uses: Vec<ModuleId>,
    pub syntax: ModuleSyntax,
}

/// Complete, cycle-free, fully parsed module graph (`struct mo_module_set`).
#[derive(Debug, Clone)]
pub struct ModuleSet {
    /// Derived module root prefix (empty `""` or ending with `'/'`).
    pub root: String,
    /// Entry module at index 0, followed by library modules and discovered uses.
    pub modules: Vec<LoadedModule>,
    /// Dependency-first topological ordering (dependencies before their users).
    pub order: Vec<ModuleId>,
    /// Manifest (`mo.toml`) paths loaded during discovery.
    pub manifests: Vec<String>,
}

impl ModuleSet {
    #[must_use]
    pub fn len(&self) -> usize {
        self.modules.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.modules.is_empty()
    }

    #[must_use]
    pub fn module(&self, id: ModuleId) -> &LoadedModule {
        &self.modules[id.index()]
    }

    #[must_use]
    pub fn find_by_name(&self, name: &str) -> Option<ModuleId> {
        self.modules.iter().find(|m| m.name == name).map(|m| m.id)
    }
}

#[derive(Debug, Clone)]
struct DepPackage {
    real_dir: String,
    src_override: String,
    root_slash: Vec<String>,
    real_roots: Vec<String>,
    direct: Vec<usize>,
}

#[derive(Debug)]
struct PendingModule {
    name: String,
    header: ParsedHeader,
    file: u32,
    library: bool,
    dependency: bool,
    pkg: Option<usize>, // None = library, Some(0) = entry project, Some(1..) = dep index + 1
    uses: Vec<ModuleId>,
    state: u8, // 0 = unvisited, 1 = visiting, 2 = visited
}

struct Loader<'s, 'd> {
    sources: &'s mut SourceMap,
    diags: &'d mut DiagBag,
    items: Vec<PendingModule>,
    name_index: HashMap<String, usize>,
    root: String,
    real_root: Option<String>,
    real_library: Option<String>,
    real_entry: Option<String>,
    entry_library_rel: Option<String>,
    embedded: Option<&'static EmbeddedLibrary>,
    entry_direct: Vec<usize>,
    deps: Vec<DepPackage>,
    manifests: Vec<String>,
    memory_only: bool,
    upgrade: bool,
    arena_remaining: usize,
}

impl<'s, 'd> Loader<'s, 'd> {
    fn new(
        sources: &'s mut SourceMap,
        diags: &'d mut DiagBag,
        embedded: Option<&'static EmbeddedLibrary>,
        memory_only: bool,
        upgrade: bool,
        arena_limit: usize,
    ) -> Self {
        Self {
            sources,
            diags,
            items: Vec::new(),
            name_index: HashMap::new(),
            root: String::new(),
            real_root: None,
            real_library: None,
            real_entry: None,
            entry_library_rel: None,
            embedded,
            entry_direct: Vec::new(),
            deps: Vec::new(),
            manifests: Vec::new(),
            memory_only,
            upgrade,
            arena_remaining: arena_limit,
        }
    }

    fn report(
        &mut self,
        code: ErrorCode,
        span: Span,
        message: &str,
        reason: Option<&str>,
        label: Option<&str>,
    ) {
        let mut diag = Diagnostic::new(code, span, message);
        if let Some(lbl) = label {
            diag = diag.with_primary_text(lbl);
        }
        if let Some(r) = reason {
            diag = diag.with_reason(r);
        }
        let _ = self.diags.push(diag);
    }

    fn source_error(&mut self, error: SourceError, path: &str, span: Span) {
        let (code, message, reason) = match error {
            SourceError::Unreadable(_) => (
                ErrorCode::ModuleMissing,
                "a module's file can't be read",
                Some("Every module is found from its name under the root folder (reference 2.1)."),
            ),
            SourceError::FileTooBig => (ErrorCode::FileTooBig, "a source file is too big", None),
            SourceError::ProgramTooBig => (
                ErrorCode::ProgramTooBig,
                "the program's source is too big",
                None,
            ),
            SourceError::TooManyFiles => (
                ErrorCode::TooManyFiles,
                "the program has too many source files",
                None,
            ),
            SourceError::PathTooLong => (
                ErrorCode::PathTooLong,
                "a source file's path is too long",
                None,
            ),
        };
        self.report(code, span, message, reason, Some(path));
    }

    fn path_text(
        &mut self,
        tree: &SyntaxTree,
        path_node: NodeId,
        filename: bool,
    ) -> Option<String> {
        let limit = MO_LIMIT_PATH_SIZE as usize;
        let node = tree.node(path_node);
        let mut dotted = String::new();
        for (i, child) in node.children.iter().enumerate() {
            let cid = (*child)?;
            let part = tree.node_text(cid)?;
            if part.len() > limit.saturating_sub(dotted.len()) {
                self.report(
                    ErrorCode::PathTooLong,
                    node.span,
                    "a module's path is too long",
                    None,
                    None,
                );
                return None;
            }
            if let Ok(s) = std::str::from_utf8(part) {
                dotted.push_str(s);
            } else {
                dotted.push_str(&String::from_utf8_lossy(part));
            }
            if i + 1 < node.children.len() {
                if dotted.len() == limit {
                    self.report(
                        ErrorCode::PathTooLong,
                        node.span,
                        "a module's path is too long",
                        None,
                        None,
                    );
                    return None;
                }
                dotted.push('.');
            }
        }
        if filename {
            match module_path(dotted.as_bytes()) {
                Ok(mp) => Some(mp.into_string()),
                Err(_) => {
                    self.report(
                        ErrorCode::PathTooLong,
                        node.span,
                        "a module's path is too long",
                        None,
                        None,
                    );
                    None
                }
            }
        } else {
            Some(dotted)
        }
    }

    fn normal_path(&mut self, path: &str) -> Option<String> {
        let limit = MO_LIMIT_PATH_SIZE as usize;
        let bytes = path.as_bytes();
        let mut out = String::with_capacity(bytes.len().min(limit));
        let mut i = 0usize;
        if bytes.first() == Some(&b'/') {
            out.push('/');
            i = 1;
        }
        while i < bytes.len() {
            if bytes[i] == b'/' {
                i += 1;
                continue;
            }
            let start = i;
            while i < bytes.len() && bytes[i] != b'/' {
                i += 1;
            }
            let part = &path[start..i];
            if part == "." {
                continue;
            }
            if !out.is_empty() && !out.ends_with('/') {
                if out.len() == limit {
                    self.report(
                        ErrorCode::PathTooLong,
                        Span::NONE,
                        "a source file's path is too long",
                        None,
                        None,
                    );
                    return None;
                }
                out.push('/');
            }
            if part.len() > limit.saturating_sub(out.len()) {
                self.report(
                    ErrorCode::PathTooLong,
                    Span::NONE,
                    "a source file's path is too long",
                    None,
                    None,
                );
                return None;
            }
            out.push_str(part);
        }
        Some(out)
    }

    fn real_path(&mut self, path: &str, origin: Span, folder: bool) -> Option<String> {
        let target = if path.is_empty() {
            Path::new(".")
        } else {
            Path::new(path)
        };
        match canonicalize_path(target) {
            Some(resolved) => Some(resolved),
            None => {
                if folder {
                    self.report(
                        ErrorCode::LibraryUnreadable,
                        origin,
                        "the library folder can't be inspected",
                        None,
                        Some(path),
                    );
                } else {
                    self.source_error(
                        SourceError::Unreadable("cannot resolve path".to_string()),
                        path,
                        origin,
                    );
                }
                None
            }
        }
    }

    fn join(&mut self, root: &str, relative: &str, span: Span) -> Option<String> {
        let limit = MO_LIMIT_PATH_SIZE as usize;
        if root.len().saturating_add(relative.len()) > limit {
            self.report(
                ErrorCode::PathTooLong,
                span,
                "a source file's path is too long",
                None,
                None,
            );
            return None;
        }
        let mut out = String::with_capacity(root.len() + relative.len());
        out.push_str(root);
        out.push_str(relative);
        Some(out)
    }

    fn ensure_trailing_slash(&mut self, dir: &str) -> Option<String> {
        if dir.ends_with('/') {
            self.join(dir, "", Span::NONE)
        } else {
            self.join(dir, "/", Span::NONE)
        }
    }

    fn find(&self, name: &str) -> Option<usize> {
        self.name_index.get(name).copied()
    }

    fn index_module_pkg(
        &mut self,
        file: u32,
        expected: Option<&str>,
        origin: Span,
        library: bool,
        pkg: Option<usize>,
    ) -> Option<usize> {
        let before = self.diags.len();
        let opts = ParseOptions {
            arena_limit: self.arena_remaining,
        };
        let parsed = read_header_with_options(self.sources, file, self.diags, opts)?;
        self.arena_remaining = self
            .arena_remaining
            .saturating_sub(parsed.tree.used_bytes());
        if self.diags.len() != before || self.diags.dropped() != 0 {
            return None;
        }
        let header_path_span = parsed.tree.node(parsed.header.path).span;
        let name = self.path_text(&parsed.tree, parsed.header.path, false)?;
        if let Some(exp) = expected
            && name != exp
        {
            let diag = Diagnostic::new(
                ErrorCode::ModuleName,
                origin,
                "the module's name doesn't match its file",
            )
            .with_primary_text(exp)
            .with_reason(
                "Each part of a module's name becomes a lowercase folder or file name \
                 with hyphens between words (reference 2.1).",
            )
            .with_secondary(Label::with_text(
                header_path_span,
                "the module declares this name",
            ));
            let _ = self.diags.push(diag);
            return None;
        }
        let previous = self.find(&name);
        if previous.is_some() || (!library && is_reserved_on_use(&name)) {
            let conflict = match previous {
                Some(prev) if library && !self.items[prev].library => {
                    let prev_mod = &self.items[prev];
                    prev_mod.header.tree.node(prev_mod.header.header.path).span
                }
                _ => header_path_span,
            };
            self.report(
                ErrorCode::ModuleReserved,
                conflict,
                "a program can't declare a library module's name",
                Some("A program can't declare a module with a library module's name (reference 10.3)."),
                Some(&name),
            );
            return None;
        }
        let use_count = parsed.tree.node(parsed.header.uses).children.len();
        let id = self.items.len();
        let dependency = !library && matches!(pkg, Some(p) if p != 0);
        self.name_index.insert(name.clone(), id);
        self.items.push(PendingModule {
            name,
            header: parsed,
            file,
            library,
            dependency,
            pkg,
            uses: vec![ModuleId(0); use_count],
            state: 0,
        });
        Some(id)
    }

    fn index_module(
        &mut self,
        file: u32,
        expected: Option<&str>,
        origin: Span,
        library: bool,
    ) -> Option<usize> {
        self.index_module_pkg(
            file,
            expected,
            origin,
            library,
            if library { None } else { Some(0) },
        )
    }

    fn read_module_in_root(
        &mut self,
        file_path: &str,
        confine_root: Option<&str>,
        expected: Option<&str>,
        origin: Span,
        library: bool,
        pkg: Option<usize>,
    ) -> Option<usize> {
        if expected.is_some() || (library && self.real_library.is_some()) {
            let root = if library {
                self.real_library.as_deref()
            } else {
                confine_root
            };
            let Some(root) = root else {
                self.report(
                    ErrorCode::ModuleOutsideRoot,
                    origin,
                    "a module's file is outside its root folder",
                    None,
                    Some(file_path),
                );
                return None;
            };
            let root_owned = root.to_string();
            let real = self.real_path(file_path, origin, false)?;
            if !is_strictly_under_root(&root_owned, &real) {
                self.report(
                    ErrorCode::ModuleOutsideRoot,
                    origin,
                    "a module's file is outside its root folder",
                    None,
                    Some(file_path),
                );
                return None;
            }
        }
        let file = match read_regular_file_into_sources(self.sources, file_path) {
            Ok(id) => id,
            Err(err) => {
                self.source_error(err, file_path, origin);
                return None;
            }
        };
        self.index_module_pkg(file, expected, origin, library, pkg)
    }

    fn read_module(
        &mut self,
        file_path: &str,
        expected: Option<&str>,
        origin: Span,
        library: bool,
    ) -> Option<usize> {
        let confine_root = self.real_root.clone();
        self.read_module_in_root(
            file_path,
            confine_root.as_deref(),
            expected,
            origin,
            library,
            if library { None } else { Some(0) },
        )
    }

    fn read_embedded_module(
        &mut self,
        src: &EmbeddedSource,
        expected: Option<&str>,
        origin: Span,
    ) -> Option<usize> {
        let virtual_path = self.join("<lib>/", src.path, origin)?;
        let file = match self
            .sources
            .add_bytes(virtual_path.as_bytes().to_vec(), src.bytes.to_vec())
        {
            Ok(id) => id,
            Err(err) => {
                self.source_error(err, &virtual_path, origin);
                return None;
            }
        };
        self.index_module(file, expected, origin, true)
    }

    fn library_files(&mut self, root: &str) -> bool {
        let mut pending: Vec<String> = vec![root.to_string()];
        let mut files: Vec<String> = Vec::new();
        let mut entries: u64 = 0;

        while let Some(path) = pending.pop() {
            let suffix = if !path.is_empty() && path.ends_with('/') {
                ""
            } else {
                "/"
            };
            let Some(prefix) = self.join(&path, suffix, Span::NONE) else {
                return false;
            };
            let dir = match fs::read_dir(&path) {
                Ok(d) => d,
                Err(_) => {
                    self.report(
                        ErrorCode::LibraryUnreadable,
                        Span::NONE,
                        "the library folder can't be inspected",
                        None,
                        Some(&path),
                    );
                    return false;
                }
            };
            for entry_res in dir {
                let entry = match entry_res {
                    Ok(e) => e,
                    Err(_) => {
                        self.report(
                            ErrorCode::LibraryUnreadable,
                            Span::NONE,
                            "the library folder can't be inspected",
                            None,
                            Some(&path),
                        );
                        return false;
                    }
                };
                let name_os = entry.file_name();
                let name = name_os.to_string_lossy();
                if name == "." || name == ".." {
                    continue;
                }
                entries += 1;
                if entries > MO_LIMIT_LIBRARY_ENTRIES {
                    self.report(
                        ErrorCode::LibraryTooBig,
                        Span::NONE,
                        "the library has too many directory entries",
                        None,
                        Some(root),
                    );
                    return false;
                }
                let Some(full) = self.join(&prefix, &name, Span::NONE) else {
                    return false;
                };
                let meta = match fs::symlink_metadata(&full) {
                    Ok(m) => m,
                    Err(_) => {
                        self.report(
                            ErrorCode::LibraryUnreadable,
                            Span::NONE,
                            "the library folder can't be inspected",
                            None,
                            Some(&full),
                        );
                        return false;
                    }
                };
                if meta.file_type().is_dir() {
                    pending.push(full);
                } else {
                    let ft = meta.file_type();
                    if (!ft.is_file() && !ft.is_symlink()) || !full.ends_with(".mo") {
                        continue;
                    }
                    if (files.len() as u64) == MO_LIMIT_FILES - 1 {
                        self.report(
                            ErrorCode::TooManyFiles,
                            Span::NONE,
                            "the program has too many source files",
                            None,
                            Some(root),
                        );
                        return false;
                    }
                    files.push(full);
                }
            }
        }

        files.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
        let root_len = root.len();
        for file_path in files {
            if let Some(real_entry) = &self.real_entry
                && let Some(resolved) = canonicalize_path(Path::new(&file_path))
                && resolved == *real_entry
            {
                continue;
            }
            let actual = file_path[root_len..].trim_start_matches('/');
            if file_loaded_on_use(actual) {
                continue;
            }
            let Some(id) = self.read_module(&file_path, None, Span::NONE, true) else {
                return false;
            };
            let (header_path_span, relative) = {
                let item = &self.items[id];
                let tree = &item.header.tree;
                let pnode = item.header.header.path;
                let span = tree.node(pnode).span;
                let rel = extract_path_text(tree, pnode, true);
                (span, rel)
            };
            let Some(relative) = relative else {
                self.report(
                    ErrorCode::PathTooLong,
                    header_path_span,
                    "a module's path is too long",
                    None,
                    None,
                );
                return false;
            };
            if actual != relative {
                self.report(
                    ErrorCode::ModuleName,
                    header_path_span,
                    "the module's name doesn't match its file",
                    Some(
                        "Each part of a module's name becomes a lowercase folder or file \
                         name with hyphens between words (reference 2.1).",
                    ),
                    Some(&relative),
                );
                return false;
            }
        }
        true
    }

    fn embedded_library_files(&mut self, embedded: &'static EmbeddedLibrary) -> bool {
        for src in embedded.files {
            if self.entry_library_rel.as_deref() == Some(src.path) {
                continue;
            }
            if file_loaded_on_use(src.path) {
                continue;
            }
            let Some(id) = self.read_embedded_module(src, None, Span::NONE) else {
                return false;
            };
            let (header_path_span, relative) = {
                let item = &self.items[id];
                let tree = &item.header.tree;
                let pnode = item.header.header.path;
                let span = tree.node(pnode).span;
                let rel = extract_path_text(tree, pnode, true);
                (span, rel)
            };
            let Some(relative) = relative else {
                self.report(
                    ErrorCode::PathTooLong,
                    header_path_span,
                    "a module's path is too long",
                    None,
                    None,
                );
                return false;
            };
            if src.path != relative {
                self.report(
                    ErrorCode::ModuleName,
                    header_path_span,
                    "the module's name doesn't match its file",
                    Some(
                        "Each part of a module's name becomes a lowercase folder or file \
                         name with hyphens between words (reference 2.1).",
                    ),
                    Some(&relative),
                );
                return false;
            }
        }
        true
    }

    fn manifest_error(&mut self, manifest_path: &str, message: &str, reason: Option<&str>) -> bool {
        let mut shown = manifest_path.to_string();
        if let Some(real_root) = self.real_root.clone() {
            let rlen = real_root.len();
            if manifest_path.starts_with(&real_root)
                && manifest_path.as_bytes().get(rlen) == Some(&b'/')
            {
                let root_prefix = self.root.clone();
                if let Some(rel) = self.join(&root_prefix, &manifest_path[rlen + 1..], Span::NONE) {
                    shown = rel;
                }
            }
        }
        self.report(
            ErrorCode::PackageManifest,
            Span::NONE,
            message,
            reason,
            Some(&shown),
        );
        false
    }

    fn resolve_dep_entry(
        &mut self,
        manifest_path: &str,
        real_manifest_dir: &str,
        dep_path: &str,
        dep_src: Option<&str>,
        stack_dirs: &mut Vec<String>,
    ) -> Option<usize> {
        let raw_dir = if dep_path.starts_with('/') {
            dep_path.to_string()
        } else {
            let base_slash = self.ensure_trailing_slash(real_manifest_dir)?;
            self.join(&base_slash, dep_path, Span::NONE)?
        };
        let resolved_dir = match canonicalize_path(Path::new(&raw_dir)) {
            Some(r) if fs::metadata(&r).is_ok_and(|m| m.is_dir()) => r,
            _ => {
                self.manifest_error(
                    manifest_path,
                    "a dependency folder in mo.toml does not exist",
                    Some(
                        "Check that each path = \"...\" under [dependencies] points to an \
                         existing folder.",
                    ),
                );
                return None;
            }
        };
        if stack_dirs.iter().any(|d| d == &resolved_dir) {
            self.manifest_error(
                manifest_path,
                "mo.toml dependencies form a cycle",
                Some("Remove the circular path dependency between mo.toml manifests."),
            );
            return None;
        }
        let norm_src = dep_src.unwrap_or("");
        for (idx, existing) in self.deps.iter().enumerate() {
            if existing.real_dir == resolved_dir && existing.src_override == norm_src {
                return Some(idx + 1);
            }
        }
        if (self.deps.len() as u64) >= MO_LIMIT_DEPENDENCIES {
            self.manifest_error(
                manifest_path,
                "the program declares too many dependencies",
                Some("A program may load at most 64 path dependencies via mo.toml."),
            );
            return None;
        }

        let mut real_roots = Vec::new();
        let mut root_slash = Vec::new();
        if let Some(src_rel) = dep_src {
            if src_rel.starts_with('/') || src_rel.contains('\\') {
                self.manifest_error(
                    manifest_path,
                    "a dependency src override in mo.toml must be a relative folder",
                    Some("Use a relative subfolder inside the dependency, such as \"src\"."),
                );
                return None;
            }
            let dep_slash = self.ensure_trailing_slash(&resolved_dir)?;
            let sub_cand = self.join(&dep_slash, src_rel, Span::NONE)?;
            let resolved_sub = match canonicalize_path(Path::new(&sub_cand)) {
                Some(r) if fs::metadata(&r).is_ok_and(|m| m.is_dir()) => r,
                _ => {
                    self.manifest_error(
                        manifest_path,
                        "a dependency src folder in mo.toml does not exist",
                        None,
                    );
                    return None;
                }
            };
            if !is_under_or_equal(&resolved_dir, &resolved_sub) {
                self.manifest_error(
                    manifest_path,
                    "a dependency src folder escapes its package folder",
                    None,
                );
                return None;
            }
            let sub_slash = self.ensure_trailing_slash(&resolved_sub)?;
            real_roots.push(resolved_sub);
            root_slash.push(sub_slash);
        } else {
            let dep_slash = self.ensure_trailing_slash(&resolved_dir)?;
            for sub in ["src", "lib", ""] {
                let cand = if sub.is_empty() {
                    resolved_dir.clone()
                } else {
                    self.join(&dep_slash, sub, Span::NONE)?
                };
                if let Some(resolved_sub) = canonicalize_path(Path::new(&cand))
                    && fs::metadata(&resolved_sub).is_ok_and(|m| m.is_dir())
                    && is_under_or_equal(&resolved_dir, &resolved_sub)
                    && !real_roots.iter().any(|r| r == &resolved_sub)
                {
                    let sub_slash = self.ensure_trailing_slash(&resolved_sub)?;
                    real_roots.push(resolved_sub);
                    root_slash.push(sub_slash);
                }
            }
        }

        let idx = self.deps.len();
        self.deps.push(DepPackage {
            real_dir: resolved_dir.clone(),
            src_override: norm_src.to_string(),
            root_slash,
            real_roots,
            direct: Vec::new(),
        });

        let dep_slash = self.ensure_trailing_slash(&resolved_dir)?;
        let sub_manifest = self.join(&dep_slash, "mo.toml", Span::NONE)?;
        if let Ok(meta) = fs::metadata(&sub_manifest) {
            if !meta.is_file() {
                self.manifest_error(&sub_manifest, "mo.toml is not a regular file", None);
                return None;
            }
            stack_dirs.push(resolved_dir.clone());
            let direct = self.load_manifest_recursive(&sub_manifest, &resolved_dir, stack_dirs);
            stack_dirs.pop();
            let direct = direct?;
            self.deps[idx].direct = direct;
        }
        Some(idx + 1)
    }

    fn load_manifest_recursive(
        &mut self,
        manifest_path: &str,
        real_manifest_dir: &str,
        stack_dirs: &mut Vec<String>,
    ) -> Option<Vec<usize>> {
        let file = match open_regular_nonblocking(Path::new(manifest_path)) {
            Ok(f) => f,
            Err(ManifestOpenError::NotRegular) => {
                self.manifest_error(manifest_path, "mo.toml is not a regular file", None);
                return None;
            }
            Err(ManifestOpenError::Unreadable) => {
                self.manifest_error(manifest_path, "mo.toml can't be read", None);
                return None;
            }
        };
        let meta = match file.metadata() {
            Ok(m) if m.is_file() => m,
            _ => {
                self.manifest_error(manifest_path, "mo.toml is not a regular file", None);
                return None;
            }
        };
        if meta.len() > MO_LIMIT_MANIFEST_SIZE {
            self.manifest_error(manifest_path, "mo.toml exceeds 64 KiB", None);
            return None;
        }
        let mut raw = Vec::new();
        if file
            .take(MO_LIMIT_MANIFEST_SIZE + 1)
            .read_to_end(&mut raw)
            .is_err()
        {
            self.manifest_error(manifest_path, "mo.toml can't be read", None);
            return None;
        }
        if (raw.len() as u64) > MO_LIMIT_MANIFEST_SIZE {
            self.manifest_error(manifest_path, "mo.toml exceeds 64 KiB", None);
            return None;
        }
        if (self.manifests.len() as u64) < MO_LIMIT_DEPENDENCIES + 1 {
            self.manifests.push(manifest_path.to_string());
        }
        if raw.contains(&0) {
            self.manifest_error(manifest_path, "mo.toml contains a NUL byte", None);
            return None;
        }
        if !is_valid_utf8(&raw) {
            self.manifest_error(manifest_path, "mo.toml is not valid UTF-8", None);
            return None;
        }
        let Ok(text) = std::str::from_utf8(&raw) else {
            self.manifest_error(manifest_path, "mo.toml is not valid UTF-8", None);
            return None;
        };

        #[derive(Clone, Copy, PartialEq, Eq)]
        enum Section {
            None,
            Package,
            Deps,
        }

        let mut section = Section::None;
        let mut seen_pkg = false;
        let mut seen_pkg_name = false;
        let mut seen_pkg_version = false;
        let mut seen_deps = false;
        let mut dep_keys: Vec<String> = Vec::new();
        let mut direct_out: Vec<usize> = Vec::new();

        for raw_line in text.split('\n') {
            let line_full = raw_line.strip_suffix('\r').unwrap_or(raw_line);
            let lb = line_full.as_bytes();

            let mut in_quotes = false;
            let mut clean_end = lb.len();
            let mut i = 0usize;
            while i < lb.len() {
                let c = lb[i];
                if !in_quotes && c == b'#' {
                    clean_end = i;
                    break;
                }
                if (c < 0x20 && c != b'\t') || c == 0x7f {
                    self.manifest_error(
                        manifest_path,
                        "mo.toml contains an invalid control byte",
                        None,
                    );
                    return None;
                }
                if c == b'"' {
                    in_quotes = !in_quotes;
                } else if in_quotes && c == b'\\' && i + 1 < lb.len() {
                    i += 1;
                }
                i += 1;
            }
            if in_quotes {
                self.manifest_error(manifest_path, "unterminated string in mo.toml", None);
                return None;
            }

            let trimmed = line_full[..clean_end].trim_matches([' ', '\t']);
            if trimmed.is_empty() {
                continue;
            }
            let bytes = trimmed.as_bytes();
            if bytes[0] == b'[' {
                if *bytes.last().unwrap_or(&0) != b']' {
                    self.manifest_error(
                        manifest_path,
                        "malformed section header in mo.toml",
                        Some("Write [package] or [dependencies] on its own line."),
                    );
                    return None;
                }
                let sec_name = trimmed[1..trimmed.len() - 1].trim_matches([' ', '\t']);
                match sec_name {
                    "package" => {
                        if seen_pkg {
                            self.manifest_error(
                                manifest_path,
                                "duplicate [package] section in mo.toml",
                                None,
                            );
                            return None;
                        }
                        seen_pkg = true;
                        section = Section::Package;
                    }
                    "dependencies" => {
                        if seen_deps {
                            self.manifest_error(
                                manifest_path,
                                "duplicate [dependencies] section in mo.toml",
                                None,
                            );
                            return None;
                        }
                        seen_deps = true;
                        section = Section::Deps;
                    }
                    _ => {
                        self.manifest_error(
                            manifest_path,
                            "unknown section in mo.toml",
                            Some("Only [package] and [dependencies] are supported in mo.toml."),
                        );
                        return None;
                    }
                }
                continue;
            }

            if section == Section::None {
                self.manifest_error(
                    manifest_path,
                    "keys in mo.toml must appear under [package] or [dependencies]",
                    None,
                );
                return None;
            }

            let mut p = 0usize;
            let Some(key) = toml_parse_bare_key(trimmed, &mut p) else {
                self.manifest_error(manifest_path, "invalid key in mo.toml", None);
                return None;
            };
            toml_skip_ws(bytes, &mut p);
            if p >= bytes.len() || bytes[p] != b'=' {
                self.manifest_error(manifest_path, "expected '=' after key in mo.toml", None);
                return None;
            }
            p += 1;
            toml_skip_ws(bytes, &mut p);

            match section {
                Section::None => unreachable!(),
                Section::Package => {
                    let Some(val) = toml_parse_string(trimmed, &mut p) else {
                        self.manifest_error(
                            manifest_path,
                            "package fields in mo.toml must be strings",
                            None,
                        );
                        return None;
                    };
                    toml_skip_ws(bytes, &mut p);
                    if p != bytes.len() {
                        self.manifest_error(
                            manifest_path,
                            "unexpected trailing tokens in mo.toml",
                            None,
                        );
                        return None;
                    }
                    if key == "name" {
                        if seen_pkg_name {
                            self.manifest_error(
                                manifest_path,
                                "duplicate package name in mo.toml",
                                None,
                            );
                            return None;
                        }
                        if val.is_empty() {
                            self.manifest_error(
                                manifest_path,
                                "package name in mo.toml cannot be empty",
                                None,
                            );
                            return None;
                        }
                        seen_pkg_name = true;
                    } else if key == "version" {
                        if seen_pkg_version {
                            self.manifest_error(
                                manifest_path,
                                "duplicate package version in mo.toml",
                                None,
                            );
                            return None;
                        }
                        if val.is_empty() {
                            self.manifest_error(
                                manifest_path,
                                "package version in mo.toml cannot be empty",
                                None,
                            );
                            return None;
                        }
                        seen_pkg_version = true;
                    } else {
                        self.manifest_error(
                            manifest_path,
                            "unknown key under [package] in mo.toml",
                            Some("Supported [package] keys are name and version."),
                        );
                        return None;
                    }
                }
                Section::Deps => {
                    if dep_keys.iter().any(|prev| prev == key) {
                        self.manifest_error(
                            manifest_path,
                            "duplicate dependency key in mo.toml",
                            None,
                        );
                        return None;
                    }
                    if (dep_keys.len() as u64) >= MO_LIMIT_DEPENDENCIES {
                        self.manifest_error(
                            manifest_path,
                            "too many dependencies in mo.toml",
                            None,
                        );
                        return None;
                    }
                    if p >= bytes.len() || bytes[p] != b'{' || *bytes.last().unwrap_or(&0) != b'}' {
                        self.manifest_error(
                            manifest_path,
                            "a dependency in mo.toml must be an inline table { path = \"...\" }",
                            None,
                        );
                        return None;
                    }
                    let inner = &trimmed[p + 1..trimmed.len() - 1];
                    let ib = inner.as_bytes();
                    let mut ip = 0usize;
                    toml_skip_ws(ib, &mut ip);
                    if ip == ib.len() {
                        self.manifest_error(
                            manifest_path,
                            "a dependency table in mo.toml requires path = \"...\"",
                            None,
                        );
                        return None;
                    }
                    let mut dep_path: Option<String> = None;
                    let mut dep_src: Option<String> = None;
                    loop {
                        let Some(tkey) = toml_parse_bare_key(inner, &mut ip) else {
                            self.manifest_error(
                                manifest_path,
                                "invalid inline table key in mo.toml",
                                None,
                            );
                            return None;
                        };
                        toml_skip_ws(ib, &mut ip);
                        if ip >= ib.len() || ib[ip] != b'=' {
                            self.manifest_error(
                                manifest_path,
                                "expected '=' in dependency inline table in mo.toml",
                                None,
                            );
                            return None;
                        }
                        ip += 1;
                        toml_skip_ws(ib, &mut ip);
                        let Some(tval) = toml_parse_string(inner, &mut ip) else {
                            self.manifest_error(
                                manifest_path,
                                "dependency fields in mo.toml must be strings",
                                None,
                            );
                            return None;
                        };
                        if tkey == "path" {
                            if dep_path.is_some() {
                                self.manifest_error(
                                    manifest_path,
                                    "duplicate path key in dependency table",
                                    None,
                                );
                                return None;
                            }
                            if tval.is_empty() {
                                self.manifest_error(
                                    manifest_path,
                                    "dependency path in mo.toml cannot be empty",
                                    None,
                                );
                                return None;
                            }
                            dep_path = Some(tval);
                        } else if tkey == "src" {
                            if dep_src.is_some() {
                                self.manifest_error(
                                    manifest_path,
                                    "duplicate src key in dependency table",
                                    None,
                                );
                                return None;
                            }
                            if tval.is_empty() {
                                self.manifest_error(
                                    manifest_path,
                                    "dependency src in mo.toml cannot be empty",
                                    None,
                                );
                                return None;
                            }
                            dep_src = Some(tval);
                        } else {
                            self.manifest_error(
                                manifest_path,
                                "unknown key in dependency inline table in mo.toml",
                                Some("Supported dependency keys are path and src."),
                            );
                            return None;
                        }
                        toml_skip_ws(ib, &mut ip);
                        if ip == ib.len() {
                            break;
                        }
                        if ib[ip] != b',' {
                            self.manifest_error(
                                manifest_path,
                                "expected ',' or '}' in dependency inline table",
                                None,
                            );
                            return None;
                        }
                        ip += 1;
                        toml_skip_ws(ib, &mut ip);
                        if ip == ib.len() {
                            self.manifest_error(
                                manifest_path,
                                "trailing comma in dependency inline table in mo.toml",
                                None,
                            );
                            return None;
                        }
                    }
                    let Some(dep_path) = dep_path else {
                        self.manifest_error(
                            manifest_path,
                            "a dependency table in mo.toml requires path = \"...\"",
                            None,
                        );
                        return None;
                    };
                    dep_keys.push(key.to_string());
                    let dep_id = self.resolve_dep_entry(
                        manifest_path,
                        real_manifest_dir,
                        &dep_path,
                        dep_src.as_deref(),
                        stack_dirs,
                    )?;
                    direct_out.push(dep_id);
                }
            }
        }

        if seen_pkg && !seen_pkg_name {
            self.manifest_error(
                manifest_path,
                "[package] in mo.toml requires name = \"...\"",
                None,
            );
            return None;
        }
        Some(direct_out)
    }

    fn discover_and_load_manifest(&mut self) -> bool {
        if self.memory_only
            || self.upgrade
            || self.real_entry.is_some()
            || self.entry_library_rel.is_some()
        {
            return true;
        }
        let Some(mut cur) = self.real_root.clone() else {
            return true;
        };
        loop {
            let cand = if cur == "/" {
                "/mo.toml".to_string()
            } else {
                format!("{cur}/mo.toml")
            };
            if let Ok(meta) = fs::metadata(&cand) {
                if !meta.is_file() {
                    return self.manifest_error(&cand, "mo.toml is not a regular file", None);
                }
                let mut stack_dirs = vec![cur.clone()];
                let Some(direct) = self.load_manifest_recursive(&cand, &cur, &mut stack_dirs)
                else {
                    return false;
                };
                self.entry_direct = direct;
                return true;
            }
            if cur == "/" {
                break;
            }
            let Some(slash) = cur.rfind('/') else {
                break;
            };
            if slash == 0 {
                cur.truncate(1);
            } else {
                cur.truncate(slash);
            }
        }
        true
    }

    fn graph(&mut self) -> Option<Vec<ModuleId>> {
        struct Frame {
            id: usize,
            use_idx: usize,
        }

        let mut stack: Vec<Frame> = Vec::new();
        let mut order: Vec<ModuleId> = Vec::new();
        let mut root = 0usize;
        while root < self.items.len() {
            if self.items[root].state != 0 {
                root += 1;
                continue;
            }
            stack.push(Frame {
                id: root,
                use_idx: 0,
            });
            self.items[root].state = 1;

            while let Some(top) = stack.last_mut() {
                let mod_id = top.id;
                let use_count = self.items[mod_id]
                    .header
                    .tree
                    .node(self.items[mod_id].header.header.uses)
                    .children
                    .len();
                if top.use_idx == use_count {
                    self.items[mod_id].state = 2;
                    order.push(ModuleId(mod_id as u32));
                    stack.pop();
                    continue;
                }
                let cur_use_idx = top.use_idx;
                top.use_idx += 1;

                let (use_path_span, name_opt, rel_opt) = {
                    let m = &self.items[mod_id];
                    let tree = &m.header.tree;
                    let uses_node = tree.node(m.header.header.uses);
                    let use_nid = uses_node.children[cur_use_idx]?;
                    let path_nid = tree.node(use_nid).children[0]?;
                    let span = tree.node(path_nid).span;
                    let name = extract_path_text(tree, path_nid, false);
                    let rel = extract_path_text(tree, path_nid, true);
                    (span, name, rel)
                };
                let Some(name) = name_opt else {
                    self.report(
                        ErrorCode::PathTooLong,
                        use_path_span,
                        "a module's path is too long",
                        None,
                        None,
                    );
                    return None;
                };

                let mut target = self.find(&name);
                if target.is_none() && self.upgrade && name.starts_with("UpgradeOld.") {
                    let src_text = format!("module {name}\n");
                    let file = match self
                        .sources
                        .add_bytes(name.as_bytes().to_vec(), src_text.into_bytes())
                    {
                        Ok(id) => id,
                        Err(err) => {
                            self.source_error(err, &name, use_path_span);
                            return None;
                        }
                    };
                    target = self.index_module(file, Some(&name), use_path_span, false);
                    target?;
                }

                if target.is_none() && self.real_library.is_some() && is_reserved_on_use(&name) {
                    let full = self.library_file_from_rel(rel_opt.as_deref(), use_path_span)?;
                    target = self.read_module(&full, Some(&name), use_path_span, true);
                    target?;
                } else if target.is_none()
                    && self.embedded.is_some()
                    && is_reserved_on_use(&name)
                    && let Some(rel) = rel_opt.as_deref()
                    && let Some(src) = self.embedded.and_then(|e| e.find(rel))
                {
                    target = self.read_embedded_module(src, Some(&name), use_path_span);
                    target?;
                }

                let is_lib_importer = self.items[mod_id].library;
                if self.memory_only
                    && (target.is_none()
                        || (is_lib_importer && target.is_some_and(|t| !self.items[t].library)))
                {
                    self.source_error(
                        SourceError::Unreadable("missing in-memory module".to_string()),
                        &name,
                        use_path_span,
                    );
                    return None;
                }

                if is_lib_importer && target.is_none() && is_loaded_on_use(&name) {
                    if self.real_library.is_some() {
                        let full = self.library_file_from_rel(rel_opt.as_deref(), use_path_span)?;
                        target = self.read_module(&full, Some(&name), use_path_span, true);
                        target?;
                    } else if let Some(rel) = rel_opt.as_deref()
                        && let Some(src) = self.embedded.and_then(|e| e.find(rel))
                    {
                        target = self.read_embedded_module(src, Some(&name), use_path_span);
                        target?;
                    }
                }

                if is_lib_importer && target.is_none_or(|t| !self.items[t].library) {
                    let full = self.library_file_from_rel(rel_opt.as_deref(), use_path_span)?;
                    self.report(
                        ErrorCode::ModuleMissing,
                        use_path_span,
                        "a library module's file can't be found",
                        Some(
                            "Every module is found under its root folder (reference 2.1); \
                             the toolchain library has its own root.",
                        ),
                        Some(&full),
                    );
                    return None;
                }

                if target.is_none() {
                    let Some(relative) = rel_opt else {
                        self.report(
                            ErrorCode::PathTooLong,
                            use_path_span,
                            "a module's path is too long",
                            None,
                            None,
                        );
                        return None;
                    };
                    let mod_pkg = self.items[mod_id].pkg.unwrap_or(0);

                    // 1. Local module root of the importing package.
                    if mod_pkg == 0 {
                        let root_prefix = self.root.clone();
                        let local_cand = self.join(&root_prefix, &relative, use_path_span)?;
                        if fs::symlink_metadata(&local_cand).is_ok() {
                            let confine = self.real_root.clone();
                            target = self.read_module_in_root(
                                &local_cand,
                                confine.as_deref(),
                                Some(&name),
                                use_path_span,
                                false,
                                Some(0),
                            );
                            target?;
                        }
                    } else if mod_pkg <= self.deps.len() {
                        let roots: Vec<(String, String)> = self.deps[mod_pkg - 1]
                            .root_slash
                            .iter()
                            .cloned()
                            .zip(self.deps[mod_pkg - 1].real_roots.iter().cloned())
                            .collect();
                        for (root_slash, real_root) in roots {
                            let cand = self.join(&root_slash, &relative, use_path_span)?;
                            if fs::symlink_metadata(&cand).is_ok() {
                                target = self.read_module_in_root(
                                    &cand,
                                    Some(&real_root),
                                    Some(&name),
                                    use_path_span,
                                    false,
                                    Some(mod_pkg),
                                );
                                target?;
                                break;
                            }
                        }
                    }

                    // 2. Declared [dependencies] from mo.toml.
                    if target.is_none() {
                        let direct: Vec<usize> = if mod_pkg == 0 {
                            self.entry_direct.clone()
                        } else {
                            self.deps[mod_pkg - 1].direct.clone()
                        };
                        let mut matched: Option<(usize, String, String)> = None;
                        for dep_id in direct {
                            let roots: Vec<(String, String)> = self.deps[dep_id - 1]
                                .root_slash
                                .iter()
                                .cloned()
                                .zip(self.deps[dep_id - 1].real_roots.iter().cloned())
                                .collect();
                            for (root_slash, real_root) in roots {
                                let cand = self.join(&root_slash, &relative, use_path_span)?;
                                if fs::symlink_metadata(&cand).is_ok() {
                                    if let Some((prev_dep, _, _)) = &matched
                                        && *prev_dep != dep_id
                                    {
                                        self.report(
                                            ErrorCode::PackageManifest,
                                            use_path_span,
                                            "a module is provided by multiple dependencies in mo.toml",
                                            Some(
                                                "Keep module namespaces distinct across \
                                                 [dependencies] in mo.toml.",
                                            ),
                                            Some(&name),
                                        );
                                        return None;
                                    }
                                    matched = Some((dep_id, cand, real_root));
                                    break;
                                }
                            }
                        }
                        if let Some((dep_id, cand, real_root)) = matched {
                            target = self.read_module_in_root(
                                &cand,
                                Some(&real_root),
                                Some(&name),
                                use_path_span,
                                false,
                                Some(dep_id),
                            );
                            target?;
                        }
                    }

                    // 3. Mo's built-in lib/ (disk or embedded).
                    if target.is_none() && is_loaded_on_use(&name) {
                        if self.real_library.is_some() {
                            let full =
                                self.library_file_from_rel(Some(&relative), use_path_span)?;
                            target = self.read_module(&full, Some(&name), use_path_span, true);
                            target?;
                        } else if let Some(src) = self.embedded.and_then(|e| e.find(&relative)) {
                            target = self.read_embedded_module(src, Some(&name), use_path_span);
                            target?;
                        }
                    }

                    // 4. Standard missing-module diagnostic under the importing module's root.
                    if target.is_none() {
                        let (miss_slash, miss_real) = if mod_pkg == 0 {
                            (self.root.clone(), self.real_root.clone())
                        } else {
                            (
                                self.deps[mod_pkg - 1].root_slash[0].clone(),
                                Some(self.deps[mod_pkg - 1].real_roots[0].clone()),
                            )
                        };
                        let full = self.join(&miss_slash, &relative, use_path_span)?;
                        target = self.read_module_in_root(
                            &full,
                            miss_real.as_deref(),
                            Some(&name),
                            use_path_span,
                            false,
                            Some(mod_pkg),
                        );
                        target?;
                    }
                }

                let target_id = target?;
                self.items[mod_id].uses[cur_use_idx] = ModuleId(target_id as u32);

                if self.items[target_id].state == 1 {
                    let start = stack.iter().position(|f| f.id == target_id)?;
                    let mut diag = Diagnostic::new(
                        ErrorCode::ModuleCycle,
                        use_path_span,
                        "modules use each other in a cycle",
                    )
                    .with_primary_text(name)
                    .with_reason(
                        "A cycle of modules that use each other is an error (reference 10.3).",
                    );
                    for i in start..stack.len().saturating_sub(1) {
                        let from = &stack[i];
                        let next = &stack[i + 1];
                        let from_mod = &self.items[from.id];
                        let tree = &from_mod.header.tree;
                        let uses_node = tree.node(from_mod.header.header.uses);
                        if let Some(edge_nid) = uses_node.children[from.use_idx - 1]
                            && let Some(edge_path) = tree.node(edge_nid).children[0]
                        {
                            let edge_span = tree.node(edge_path).span;
                            diag = diag.with_secondary(Label::with_text(
                                edge_span,
                                self.items[next.id].name.as_str(),
                            ));
                        }
                    }
                    let _ = self.diags.push(diag);
                    return None;
                }

                if self.items[target_id].state == 0 {
                    self.items[target_id].state = 1;
                    stack.push(Frame {
                        id: target_id,
                        use_idx: 0,
                    });
                }
            }
            root += 1;
        }
        Some(order)
    }

    fn library_file_from_rel(&mut self, relative: Option<&str>, span: Span) -> Option<String> {
        let Some(relative) = relative else {
            self.report(
                ErrorCode::PathTooLong,
                span,
                "a module's path is too long",
                None,
                None,
            );
            return None;
        };
        match self.real_library.clone() {
            None => self.join("lib/", relative, span),
            Some(real_lib) => {
                let prefix = self.join(&real_lib, "/", span)?;
                self.join(&prefix, relative, span)
            }
        }
    }

    fn load_named_library(&mut self, name: &str, file: &str) -> bool {
        if self.find(name).is_some() {
            return true;
        }
        if let Some(real_lib) = self.real_library.clone() {
            let Some(prefix) = self.join(&real_lib, "/", Span::NONE) else {
                return false;
            };
            let Some(full) = self.join(&prefix, file, Span::NONE) else {
                return false;
            };
            return self
                .read_module(&full, Some(name), Span::NONE, true)
                .is_some();
        }
        if let Some(embedded) = self.embedded
            && let Some(src) = embedded.find(file)
        {
            return self
                .read_embedded_module(src, Some(name), Span::NONE)
                .is_some();
        }
        true
    }

    fn parse_modules(mut self) -> Option<ModuleSet> {
        let mut order = self.graph()?;
        let before = self.diags.len();
        let mut has_api = false;
        let mut has_sqlite = false;
        let mut parsed_syntax: Vec<ModuleSyntax> = Vec::with_capacity(self.items.len());

        for i in 0..self.items.len() {
            let file = self.items[i].file;
            let opts = ParseOptions {
                arena_limit: self.arena_remaining,
            };
            let module = parse_module_with_options(self.sources, file, self.diags, opts)?;
            self.arena_remaining = self
                .arena_remaining
                .saturating_sub(module.tree.used_bytes());
            if let Some(Some(items_nid)) = module.tree.node(module.root).children.get(1) {
                for child in &module.tree.node(*items_nid).children {
                    if let Some(cid) = *child
                        && module.tree.node(cid).kind == SyntaxKind::Api
                    {
                        has_api = true;
                    }
                }
            }
            if self.items[i].name != "Sqlite" && syntax_mentions_sqlite(&module.tree) {
                has_sqlite = true;
            }
            parsed_syntax.push(module);
        }

        let need_api = has_api && (self.find("Api").is_none() || self.find("Json").is_none());
        let need_sqlite = (has_sqlite || has_api)
            && (self.find("Sqlite").is_none()
                || self.find("Json").is_none()
                || self.find("Date").is_none());

        if need_api || need_sqlite {
            let prev_count = self.items.len();
            if need_api
                && (!self.load_named_library("Json", "json.mo")
                    || !self.load_named_library("Api", "api.mo"))
            {
                return None;
            }
            if need_sqlite
                && (!self.load_named_library("Json", "json.mo")
                    || !self.load_named_library("Date", "date.mo")
                    || !self.load_named_library("Sqlite", "sqlite.mo"))
            {
                return None;
            }
            if self.items.len() > prev_count {
                for i in prev_count..self.items.len() {
                    let file = self.items[i].file;
                    let opts = ParseOptions {
                        arena_limit: self.arena_remaining,
                    };
                    let module = parse_module_with_options(self.sources, file, self.diags, opts)?;
                    self.arena_remaining = self
                        .arena_remaining
                        .saturating_sub(module.tree.used_bytes());
                    parsed_syntax.push(module);
                }
                for item in &mut self.items {
                    item.state = 0;
                }
                order = self.graph()?;
            }
        }

        self.diags.sort();
        if self.diags.len() != before || self.diags.dropped() != 0 {
            return None;
        }

        let modules = self
            .items
            .into_iter()
            .zip(parsed_syntax)
            .enumerate()
            .map(|(idx, (item, syntax))| LoadedModule {
                id: ModuleId(idx as u32),
                name: item.name,
                file: item.file,
                library: item.library,
                dependency: item.dependency,
                uses: item.uses,
                syntax,
            })
            .collect();

        Some(ModuleSet {
            root: self.root,
            modules,
            order,
            manifests: self.manifests,
        })
    }
}

/// Loads the entry module and all transitive dependencies according to `options`
/// (`src/check/load.c:mo_load_program_with_library`).
///
/// `sources` should be empty when calling this function. On failure returns `None`
/// while retaining sorted diagnostics in `diags` and any loaded files in `sources`.
#[must_use]
pub fn load_program(
    sources: &mut SourceMap,
    entry: &str,
    options: &LoadOptions,
    diags: &mut DiagBag,
) -> Option<ModuleSet> {
    let embedded = match &options.library {
        LibraryMode::Embedded { library, .. } => Some(*library),
        LibraryMode::None | LibraryMode::Directory(_) => None,
    };
    let mut loader = Loader::new(
        sources,
        diags,
        embedded,
        false,
        options.upgrade,
        options.syntax_arena_limit,
    );
    let result = load_entry(&mut loader, entry, options);
    let set = match result {
        true => loader.parse_modules(),
        false => None,
    };
    diags.sort();
    set
}

fn load_entry(loader: &mut Loader<'_, '_>, raw_entry: &str, options: &LoadOptions) -> bool {
    let Some(entry) = loader.normal_path(raw_entry) else {
        return false;
    };
    let mut disk_library: Option<String> = None;

    match &options.library {
        LibraryMode::Directory(lib_path) => {
            let lib_str = lib_path.to_string_lossy();
            let Some(norm_lib) = loader.normal_path(&lib_str) else {
                return false;
            };
            let Some(real_lib) = loader.real_path(&norm_lib, Span::NONE, true) else {
                return false;
            };
            if let Some(resolved) = canonicalize_path(Path::new(&entry))
                && is_strictly_under_root(&real_lib, &resolved)
            {
                let Some(real_entry) = loader.real_path(&entry, Span::NONE, false) else {
                    return false;
                };
                loader.real_entry = Some(real_entry);
            }
            loader.real_library = Some(real_lib);
            disk_library = Some(norm_lib);
        }
        LibraryMode::Embedded {
            entry_root: Some(entry_root),
            ..
        } => {
            if let Some(lib_resolved) = canonicalize_path(entry_root)
                && let Some(entry_resolved) = canonicalize_path(Path::new(&entry))
                && is_strictly_under_root(&lib_resolved, &entry_resolved)
            {
                let Some(real_entry) = loader.real_path(&entry, Span::NONE, false) else {
                    return false;
                };
                let rel = real_entry[lib_resolved.len()..]
                    .trim_start_matches('/')
                    .to_string();
                loader.real_entry = Some(real_entry);
                loader.entry_library_rel = Some(rel);
            }
        }
        LibraryMode::Embedded {
            entry_root: None, ..
        }
        | LibraryMode::None => {}
    }

    let entry_is_lib = loader.real_entry.is_some();
    if loader
        .read_module(&entry, None, Span::NONE, entry_is_lib)
        .is_none()
    {
        return false;
    }

    let (entry_path_span, relative) = {
        let item = &loader.items[0];
        let tree = &item.header.tree;
        let pnode = item.header.header.path;
        let span = tree.node(pnode).span;
        let rel = extract_path_text(tree, pnode, true);
        (span, rel)
    };
    let Some(relative) = relative else {
        loader.report(
            ErrorCode::PathTooLong,
            entry_path_span,
            "a module's path is too long",
            None,
            None,
        );
        return false;
    };

    let a = entry.len();
    let b = relative.len();
    if a < b || entry[a - b..] != relative || (a > b && entry.as_bytes()[a - b - 1] != b'/') {
        loader.report(
            ErrorCode::ModuleName,
            entry_path_span,
            "the module's name doesn't match its file",
            Some(
                "Each part of a module's name becomes a lowercase folder or file name \
                 with hyphens between words (reference 2.1).",
            ),
            Some(&relative),
        );
        return false;
    }

    let root = entry[..a - b].to_string();
    let Some(real_root) = loader.real_path(&root, entry_path_span, false) else {
        return false;
    };
    loader.root = root;
    loader.real_root = Some(real_root);

    if !loader.discover_and_load_manifest() {
        return false;
    }

    if options.upgrade {
        let placeholder = b"module UpgradeOld\n";
        let file = match loader
            .sources
            .add_bytes(b"<UpgradeOld>".to_vec(), placeholder.to_vec())
        {
            Ok(id) => id,
            Err(err) => {
                loader.source_error(err, "<UpgradeOld>", Span::NONE);
                return false;
            }
        };
        if loader
            .index_module(file, Some("UpgradeOld"), Span::NONE, false)
            .is_none()
        {
            return false;
        }
    }

    if let Some(lib_dir) = disk_library {
        if !loader.library_files(&lib_dir) {
            return false;
        }
    } else if let Some(embedded) = loader.embedded
        && !loader.embedded_library_files(embedded)
    {
        return false;
    }

    true
}

/// Loads a closed in-memory set of sources already added to `sources`
/// (`src/check/load.c:mo_load_program_sources`).
#[must_use]
pub fn load_program_sources(
    sources: &mut SourceMap,
    library_flags: Option<&[bool]>,
    diags: &mut DiagBag,
) -> Option<ModuleSet> {
    let count = sources.len();
    if count == 0 || u64::from(count) > MO_LIMIT_FILES {
        return None;
    }
    let mut loader = Loader::new(
        sources,
        diags,
        None,
        true,
        false,
        MO_CLI_CHECK_SIZE as usize,
    );
    let mut ok = true;
    for i in 0..count {
        let is_lib = library_flags
            .and_then(|flags| flags.get(i as usize).copied())
            .unwrap_or(false);
        if loader.index_module(i, None, Span::NONE, is_lib).is_none() {
            ok = false;
            break;
        }
    }
    let set = if ok { loader.parse_modules() } else { None };
    diags.sort();
    set
}

fn syntax_mentions_sqlite(tree: &SyntaxTree) -> bool {
    for idx in 0..tree.node_count() {
        let nid = NodeId(idx as u32);
        let node = tree.node(nid);
        if node.kind == SyntaxKind::CapName
            && let Some(text) = tree.node_text(nid)
            && matches!(
                text,
                b"Sqlite"
                    | b"SqliteDecodeError"
                    | b"MissingColumn"
                    | b"TypeMismatch"
                    | b"InvalidValue"
                    | b"ExpectedOneRow"
            )
        {
            return true;
        }
    }
    false
}

fn extract_path_text(tree: &SyntaxTree, path_node: NodeId, filename: bool) -> Option<String> {
    let limit = MO_LIMIT_PATH_SIZE as usize;
    let node = tree.node(path_node);
    let mut dotted = String::new();
    for (i, child) in node.children.iter().enumerate() {
        let cid = (*child)?;
        let part = tree.node_text(cid)?;
        if part.len() > limit.saturating_sub(dotted.len()) {
            return None;
        }
        if let Ok(s) = std::str::from_utf8(part) {
            dotted.push_str(s);
        } else {
            dotted.push_str(&String::from_utf8_lossy(part));
        }
        if i + 1 < node.children.len() {
            if dotted.len() == limit {
                return None;
            }
            dotted.push('.');
        }
    }
    if filename {
        module_path(dotted.as_bytes())
            .ok()
            .map(|mp| mp.into_string())
    } else {
        Some(dotted)
    }
}

fn canonicalize_path(path: &Path) -> Option<String> {
    let resolved = fs::canonicalize(path).ok()?;
    let s = resolved.to_string_lossy().into_owned();
    if (s.len() as u64) > MO_LIMIT_PATH_SIZE {
        None
    } else {
        Some(s)
    }
}

fn is_strictly_under_root(root: &str, candidate: &str) -> bool {
    let n = root.len();
    candidate.starts_with(root) && (n == 1 || candidate.as_bytes().get(n) == Some(&b'/'))
}

fn is_under_or_equal(root: &str, candidate: &str) -> bool {
    let n = root.len();
    candidate.starts_with(root)
        && (n == 1 || candidate.len() == n || candidate.as_bytes().get(n) == Some(&b'/'))
}

enum ManifestOpenError {
    NotRegular,
    Unreadable,
}

#[cfg(unix)]
fn open_regular_nonblocking(path: &Path) -> Result<fs::File, ManifestOpenError> {
    let fd = rustix::fs::open(
        path,
        rustix::fs::OFlags::RDONLY | rustix::fs::OFlags::NONBLOCK,
        rustix::fs::Mode::empty(),
    )
    .map_err(|_| ManifestOpenError::Unreadable)?;
    let file = fs::File::from(fd);
    let meta = file.metadata().map_err(|_| ManifestOpenError::Unreadable)?;
    if !meta.is_file() {
        return Err(ManifestOpenError::NotRegular);
    }
    Ok(file)
}

#[cfg(not(unix))]
fn open_regular_nonblocking(path: &Path) -> Result<fs::File, ManifestOpenError> {
    let meta = fs::metadata(path).map_err(|_| ManifestOpenError::Unreadable)?;
    if !meta.is_file() {
        return Err(ManifestOpenError::NotRegular);
    }
    fs::File::open(path).map_err(|_| ManifestOpenError::Unreadable)
}

#[cfg(unix)]
fn read_regular_file_into_sources(sources: &mut SourceMap, path: &str) -> Result<u32, SourceError> {
    let path_bytes = path.as_bytes();
    if u64::from(sources.len()) >= MO_LIMIT_FILES {
        return Err(SourceError::TooManyFiles);
    }
    if (path_bytes.len() as u64) > MO_LIMIT_PATH_SIZE {
        return Err(SourceError::PathTooLong);
    }
    let fd = rustix::fs::open(
        Path::new(path),
        rustix::fs::OFlags::RDONLY | rustix::fs::OFlags::NONBLOCK,
        rustix::fs::Mode::empty(),
    )
    .map_err(|e| SourceError::Unreadable(e.to_string()))?;
    let file = fs::File::from(fd);
    let meta = file
        .metadata()
        .map_err(|e| SourceError::Unreadable(e.to_string()))?;
    if !meta.is_file() {
        return Err(SourceError::Unreadable("not a regular file".to_string()));
    }
    let mut buf = Vec::new();
    let mut reader = file.take(MO_LIMIT_FILE_SIZE + 1);
    reader
        .read_to_end(&mut buf)
        .map_err(|e| SourceError::Unreadable(e.to_string()))?;
    if (buf.len() as u64) > MO_LIMIT_FILE_SIZE {
        return Err(SourceError::FileTooBig);
    }
    sources.add_bytes(path_bytes.to_vec(), buf)
}

#[cfg(not(unix))]
fn read_regular_file_into_sources(sources: &mut SourceMap, path: &str) -> Result<u32, SourceError> {
    sources.add_file(Path::new(path))
}

fn toml_skip_ws(s: &[u8], pos: &mut usize) {
    while *pos < s.len() && (s[*pos] == b' ' || s[*pos] == b'\t') {
        *pos += 1;
    }
}

fn toml_is_bare_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_' || c == b'-'
}

fn toml_parse_bare_key<'a>(s: &'a str, pos: &mut usize) -> Option<&'a str> {
    let bytes = s.as_bytes();
    let start = *pos;
    while *pos < bytes.len() && toml_is_bare_char(bytes[*pos]) {
        *pos += 1;
    }
    if *pos == start {
        None
    } else {
        Some(&s[start..*pos])
    }
}

fn toml_parse_string(s: &str, pos: &mut usize) -> Option<String> {
    let bytes = s.as_bytes();
    if *pos >= bytes.len() || bytes[*pos] != b'"' {
        return None;
    }
    *pos += 1;
    let mut out = Vec::with_capacity(bytes.len().saturating_sub(*pos));
    while *pos < bytes.len() {
        let c = bytes[*pos];
        *pos += 1;
        if c == b'"' {
            return String::from_utf8(out).ok();
        }
        if c < 0x20 || c == 0x7f {
            return None;
        }
        if c == b'\\' {
            if *pos >= bytes.len() {
                return None;
            }
            let esc = bytes[*pos];
            *pos += 1;
            match esc {
                b'"' => out.push(b'"'),
                b'\\' => out.push(b'\\'),
                b'n' => out.push(b'\n'),
                b'r' => out.push(b'\r'),
                b't' => out.push(b'\t'),
                _ => return None,
            }
        } else {
            out.push(c);
        }
    }
    None
}
