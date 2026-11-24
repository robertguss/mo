//! Shared interned surface type DAG, built-in type/case tables, and capability names
//! (`src/check/check.h`, `src/check/data.h`, `src/check/signatures.c`, `src/base/capabilities.c`).

use std::collections::HashMap;

use crate::ids::{ModuleId, NativeSigId, SymRef, TypeId};

/// Built-in capability type names in `MO_CAP_*` order (`src/base/capabilities.c`).
pub const CAPABILITY_NAMES: [&str; 15] = [
    "Args",
    "Env",
    "Console",
    "Files",
    "ReadFiles",
    "Clock",
    "Random",
    "Simulation",
    "Network",
    "Connection",
    "Sqlite",
    "Sqlite.Database",
    "Command",
    "SecureRandom",
    "Files.File",
];

/// Returns whether `name` is one of Mo's 15 built-in capability type names.
#[must_use]
pub fn is_capability_name(name: &str) -> bool {
    CAPABILITY_NAMES.contains(&name)
}

/// Built-in types always in scope and their generic arity (`src/check/signatures.c:mo_builtin_type`).
pub const BUILTIN_TYPES: [(&str, u32); 37] = [
    ("Bool", 0),
    ("Int", 0),
    ("BigInt", 0),
    ("Byte", 0),
    ("Decimal", 0),
    ("Float", 0),
    ("Text", 0),
    ("Char", 0),
    ("Bytes", 0),
    ("Unit", 0),
    ("List", 1),
    ("Map", 2),
    ("Set", 1),
    ("Range", 0),
    ("Time", 0),
    ("Duration", 0),
    ("Option", 1),
    ("Result", 2),
    ("Ordering", 0),
    ("Args", 0),
    ("Env", 0),
    ("Console", 0),
    ("Files", 0),
    ("ReadFiles", 0),
    ("Clock", 0),
    ("Random", 0),
    ("SecureRandom", 0),
    ("FileError", 0),
    ("Entry", 0),
    ("EntryKind", 0),
    ("Simulation", 0),
    ("SimulationError", 0),
    ("Network", 0),
    ("Connection", 0),
    ("NetworkError", 0),
    ("Sqlite", 0),
    ("SqliteError", 0),
];

/// Looks up a built-in type name always in scope and returns its canonical spelling and arity.
#[must_use]
pub fn builtin_type_arity(name: &[u8]) -> Option<(&'static str, u32)> {
    for &(ty_name, arity) in &BUILTIN_TYPES {
        if ty_name.as_bytes() == name {
            return Some((ty_name, arity));
        }
    }
    None
}

/// Built-in choice/record constructor metadata (`struct mo_builtin_case` in `src/check/data.h`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BuiltinCase {
    pub name: &'static str,
    pub ty: &'static str,
    pub key: u32,
    pub first_key: u32,
    pub cases: u32,
    pub parameters: u32,
    pub count: u32,
    pub parameter: u32,
    pub fields: [Option<&'static str>; 2],
    pub module: Option<&'static str>,
}

/// All built-in constructors in key order (`src/check/signatures.c:builtin_cases`).
pub const BUILTIN_CASES: [BuiltinCase; 34] = [
    BuiltinCase {
        name: "Unit",
        ty: "Unit",
        key: 6,
        first_key: 6,
        cases: 1,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "None",
        ty: "Option",
        key: 8,
        first_key: 8,
        cases: 2,
        parameters: 1,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Some",
        ty: "Option",
        key: 9,
        first_key: 8,
        cases: 2,
        parameters: 1,
        count: 1,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Ok",
        ty: "Result",
        key: 10,
        first_key: 10,
        cases: 2,
        parameters: 2,
        count: 1,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Error",
        ty: "Result",
        key: 11,
        first_key: 10,
        cases: 2,
        parameters: 2,
        count: 1,
        parameter: 1,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Less",
        ty: "Ordering",
        key: 12,
        first_key: 12,
        cases: 3,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Equal",
        ty: "Ordering",
        key: 13,
        first_key: 12,
        cases: 3,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Greater",
        ty: "Ordering",
        key: 14,
        first_key: 12,
        cases: 3,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "File",
        ty: "EntryKind",
        key: 15,
        first_key: 15,
        cases: 4,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Folder",
        ty: "EntryKind",
        key: 16,
        first_key: 15,
        cases: 4,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Link",
        ty: "EntryKind",
        key: 17,
        first_key: 15,
        cases: 4,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "Other",
        ty: "EntryKind",
        key: 18,
        first_key: 15,
        cases: 4,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "NotFound",
        ty: "FileError",
        key: 19,
        first_key: 19,
        cases: 4,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("path"), None],
        module: None,
    },
    BuiltinCase {
        name: "NotAllowed",
        ty: "FileError",
        key: 20,
        first_key: 19,
        cases: 4,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("path"), None],
        module: None,
    },
    BuiltinCase {
        name: "NotText",
        ty: "FileError",
        key: 21,
        first_key: 19,
        cases: 4,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("path"), None],
        module: None,
    },
    BuiltinCase {
        name: "Failed",
        ty: "FileError",
        key: 22,
        first_key: 19,
        cases: 4,
        parameters: 0,
        count: 2,
        parameter: 0,
        fields: [Some("path"), Some("reason")],
        module: None,
    },
    BuiltinCase {
        name: "Entry",
        ty: "Entry",
        key: 23,
        first_key: 23,
        cases: 1,
        parameters: 0,
        count: 2,
        parameter: 0,
        fields: [Some("name"), Some("kind")],
        module: None,
    },
    BuiltinCase {
        name: "ServiceDown",
        ty: "SimulationError",
        key: 24,
        first_key: 24,
        cases: 2,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "ToolFailed",
        ty: "SimulationError",
        key: 25,
        first_key: 24,
        cases: 2,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: None,
    },
    BuiltinCase {
        name: "UnknownHost",
        ty: "NetworkError",
        key: 26,
        first_key: 26,
        cases: 6,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("host"), None],
        module: Some("Network"),
    },
    BuiltinCase {
        name: "Refused",
        ty: "NetworkError",
        key: 27,
        first_key: 26,
        cases: 6,
        parameters: 0,
        count: 2,
        parameter: 0,
        fields: [Some("host"), Some("port")],
        module: Some("Network"),
    },
    BuiltinCase {
        name: "TimedOut",
        ty: "NetworkError",
        key: 28,
        first_key: 26,
        cases: 6,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: Some("Network"),
    },
    BuiltinCase {
        name: "Closed",
        ty: "NetworkError",
        key: 29,
        first_key: 26,
        cases: 6,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: Some("Network"),
    },
    BuiltinCase {
        name: "Broken",
        ty: "NetworkError",
        key: 30,
        first_key: 26,
        cases: 6,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("reason"), None],
        module: Some("Network"),
    },
    BuiltinCase {
        name: "Certificate",
        ty: "NetworkError",
        key: 31,
        first_key: 26,
        cases: 6,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("reason"), None],
        module: Some("Network"),
    },
    BuiltinCase {
        name: "OutsideFolder",
        ty: "SqliteError",
        key: 32,
        first_key: 32,
        cases: 3,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("path"), None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "NotPermitted",
        ty: "SqliteError",
        key: 33,
        first_key: 32,
        cases: 3,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("reason"), None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "SqlFailed",
        ty: "SqliteError",
        key: 34,
        first_key: 32,
        cases: 3,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [Some("message"), None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "Null",
        ty: "Sqlite.Value",
        key: 35,
        first_key: 35,
        cases: 5,
        parameters: 0,
        count: 0,
        parameter: 0,
        fields: [None, None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "Integer",
        ty: "Sqlite.Value",
        key: 36,
        first_key: 35,
        cases: 5,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [None, None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "Real",
        ty: "Sqlite.Value",
        key: 37,
        first_key: 35,
        cases: 5,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [None, None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "String",
        ty: "Sqlite.Value",
        key: 38,
        first_key: 35,
        cases: 5,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [None, None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "Blob",
        ty: "Sqlite.Value",
        key: 39,
        first_key: 35,
        cases: 5,
        parameters: 0,
        count: 1,
        parameter: 0,
        fields: [None, None],
        module: Some("Sqlite"),
    },
    BuiltinCase {
        name: "Rows",
        ty: "Sqlite.Rows",
        key: 40,
        first_key: 40,
        cases: 1,
        parameters: 0,
        count: 2,
        parameter: 0,
        fields: [Some("columns"), Some("rows")],
        module: Some("Sqlite"),
    },
];

#[must_use]
pub fn builtin_case_by_name(name: &[u8], prefix: Option<&[u8]>) -> Option<&'static BuiltinCase> {
    BUILTIN_CASES.iter().find(|case| {
        case.name.as_bytes() == name
            && match (prefix, case.module) {
                (None, None) => true,
                (Some(p), Some(m)) => p == m.as_bytes(),
                (None, Some(_)) | (Some(_), None) => false,
            }
    })
}

#[must_use]
pub fn builtin_case_by_text(name: &str) -> Option<&'static BuiltinCase> {
    BUILTIN_CASES.iter().find(|case| case.name == name)
}

#[must_use]
pub fn builtin_case_by_key(key: u32) -> Option<&'static BuiltinCase> {
    BUILTIN_CASES.iter().find(|case| case.key == key)
}

#[must_use]
pub fn builtin_case_by_type(ty: &str) -> Option<&'static BuiltinCase> {
    BUILTIN_CASES.iter().find(|case| case.ty == ty)
}

/// Structural kind of a [`SourceType`] (`enum mo_source_type_kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SourceTypeKind {
    Named,
    Parameter,
    Function,
    Tuple,
}

/// Canonical owner of a nominal type or scoped rigid type parameter.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TypeOwner {
    /// Built-in type, tuple, or function type (`declaration == NULL`).
    None,
    /// Declared source symbol `(module, symbol)`.
    Source(SymRef),
    /// Native signature from `prims.def` / `hostops.def`.
    Native(NativeSigId),
    /// Built-in `Option(T)` template parameter owner.
    BuiltinOption,
    /// Built-in `Result(T, E)` template parameter owner.
    BuiltinResult,
}

impl TypeOwner {
    #[must_use]
    pub const fn as_source(self) -> Option<SymRef> {
        match self {
            Self::Source(sym) => Some(sym),
            Self::None | Self::Native(_) | Self::BuiltinOption | Self::BuiltinResult => None,
        }
    }

    #[must_use]
    pub const fn as_native(self) -> Option<NativeSigId> {
        match self {
            Self::Native(id) => Some(id),
            Self::None | Self::Source(_) | Self::BuiltinOption | Self::BuiltinResult => None,
        }
    }
}

/// One node in the shared interned type DAG (`struct mo_source_type`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceType {
    pub kind: SourceTypeKind,
    pub name: String,
    /// Declaring module, or `None` for built-in/native types (`UINT32_MAX` in C).
    pub module: Option<ModuleId>,
    /// Nominal declaration or scoped parameter owner.
    pub owner: TypeOwner,
    /// Type arguments; for `Function`, the last slot is the result type.
    pub arguments: Vec<TypeId>,
    /// Longest path to a leaf; a leaf has height 0.
    pub height: u32,
    /// Bitmask of abstract single-letter parameters `'A'..='Z'` (bits 0..25).
    pub parameters: u32,
}

impl SourceType {
    #[must_use]
    pub fn is_builtin_named(&self, target: &str) -> bool {
        self.kind == SourceTypeKind::Named && self.module.is_none() && self.name == target
    }

    /// Returns the `0..26` parameter letter index if this is a `Parameter` type.
    #[must_use]
    pub fn param_letter(&self) -> Option<usize> {
        if self.kind != SourceTypeKind::Parameter {
            return None;
        }
        let b = *self.name.as_bytes().first()?;
        if b.is_ascii_uppercase() {
            Some((b - b'A') as usize)
        } else {
            None
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct InternKey {
    kind: SourceTypeKind,
    module: Option<ModuleId>,
    owner: TypeOwner,
    name: String,
    arguments: Vec<TypeId>,
}

/// Arena storing canonical interned [`SourceType`] nodes (`struct mo_source_type` DAG).
#[derive(Debug, Clone, Default)]
pub struct TypeArena {
    types: Vec<SourceType>,
    index: HashMap<InternKey, TypeId>,
}

impl TypeArena {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.types.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.types.is_empty()
    }

    #[must_use]
    pub fn get(&self, id: TypeId) -> &SourceType {
        &self.types[id.index()]
    }

    /// Interns a type node, computing its `height` and `parameters` mask from its children.
    pub fn intern(
        &mut self,
        kind: SourceTypeKind,
        name: impl Into<String>,
        module: Option<ModuleId>,
        owner: TypeOwner,
        arguments: Vec<TypeId>,
    ) -> TypeId {
        let name = name.into();
        let key = InternKey {
            kind,
            module,
            owner,
            name: name.clone(),
            arguments: arguments.clone(),
        };
        if let Some(&existing) = self.index.get(&key) {
            return existing;
        }
        let mut height = 0u32;
        let mut parameters = if kind == SourceTypeKind::Parameter {
            name.as_bytes()
                .first()
                .filter(|b| b.is_ascii_uppercase())
                .map_or(0, |&b| 1u32 << u32::from(b - b'A'))
        } else {
            0
        };
        for &arg in &arguments {
            let child = &self.types[arg.index()];
            let path = child.height.saturating_add(1);
            if path > height {
                height = path;
            }
            parameters |= child.parameters;
        }
        let id = TypeId(self.types.len() as u32);
        self.types.push(SourceType {
            kind,
            name,
            module,
            owner,
            arguments,
            height,
            parameters,
        });
        self.index.insert(key, id);
        id
    }

    /// Allocates a distinct non-interned type node (used only for `Json.schema`'s phantom `T`
    /// parameter mask override, matching `src/check/signatures.c:1486-1496`).
    pub fn alloc_override(&mut self, ty: SourceType) -> TypeId {
        let id = TypeId(self.types.len() as u32);
        self.types.push(ty);
        id
    }

    /// Interns a built-in zero-arity named type (e.g. `"Unit"`, `"Int"`, `"Text"`).
    pub fn intern_builtin(&mut self, name: &str) -> TypeId {
        self.intern(
            SourceTypeKind::Named,
            name,
            None,
            TypeOwner::None,
            Vec::new(),
        )
    }

    /// Interns a built-in parameterized named type (e.g. `"List"`, `"Map"`, `"Option"`).
    pub fn intern_builtin_applied(&mut self, name: &str, arguments: Vec<TypeId>) -> TypeId {
        self.intern(
            SourceTypeKind::Named,
            name,
            None,
            TypeOwner::None,
            arguments,
        )
    }

    /// Interns the canonical `Option(T)` template type (`mo_builtin_template`).
    pub fn intern_option_template(&mut self) -> TypeId {
        let param_t = self.intern(
            SourceTypeKind::Parameter,
            "T",
            None,
            TypeOwner::BuiltinOption,
            Vec::new(),
        );
        self.intern_builtin_applied("Option", vec![param_t])
    }

    /// Interns the canonical `Result(T, E)` template type (`mo_builtin_template`).
    pub fn intern_result_template(&mut self) -> TypeId {
        let param_t = self.intern(
            SourceTypeKind::Parameter,
            "T",
            None,
            TypeOwner::BuiltinResult,
            Vec::new(),
        );
        let param_e = self.intern(
            SourceTypeKind::Parameter,
            "E",
            None,
            TypeOwner::BuiltinResult,
            Vec::new(),
        );
        self.intern_builtin_applied("Result", vec![param_t, param_e])
    }

    /// Returns whether two [`TypeId`]s denote structurally equal types.
    #[must_use]
    pub fn structurally_equal(&self, a: TypeId, b: TypeId) -> bool {
        if a == b {
            return true;
        }
        let ta = self.get(a);
        let tb = self.get(b);
        if ta.kind != tb.kind
            || ta.module != tb.module
            || ta.owner != tb.owner
            || ta.name != tb.name
            || ta.arguments.len() != tb.arguments.len()
        {
            return false;
        }
        ta.arguments
            .iter()
            .zip(tb.arguments.iter())
            .all(|(&x, &y)| self.structurally_equal(x, y))
    }
}
