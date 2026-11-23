#![forbid(unsafe_code)]

//! Mo Bytecode v0.3 (`MOBC`) binary reader, structural validator, and type/layout table.

pub mod read;
pub mod types;
pub mod validate;
pub mod verify;

#[cfg(test)]
mod verify_tests;

pub const BYTECODE_MAGIC: [u8; 4] = *b"MOBC";
pub const BYTECODE_MAJOR: u16 = 0;
pub const BYTECODE_MINOR: u16 = 3;
pub const BYTECODE_HEADER_SIZE: usize = 20;
pub const BYTECODE_FLAGS_NONE: u32 = 0;
pub const BYTECODE_FLAGS_SERVICES: u32 = 1;
pub const NO_INDEX: u32 = u32::MAX;

pub const FNV_OFFSET: u64 = 14_695_981_039_346_656_037;
pub const FNV_PRIME: u64 = 1_099_511_628_211;

pub const ABILITY_EQUAL: u8 = 1;
pub const ABILITY_HASH: u8 = 2;
pub const ABILITY_ORDER: u8 = 4;

pub const OPCODE_MASK: u32 = 0xffff;
pub const OPCODE_VARIABLE_SHIFT: u32 = 16;

pub fn fnv1a(mut hash: u64, bytes: &[u8]) -> u64 {
    for &b in bytes {
        hash = (hash ^ u64::from(b)).wrapping_mul(FNV_PRIME);
    }
    hash
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorKind {
    Invalid,
    Limit,
    NoMemory,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LoadError {
    pub offset: u32,
    pub reason: &'static str,
    pub kind: ErrorKind,
}

impl LoadError {
    pub const fn invalid(offset: u32, reason: &'static str) -> Self {
        Self {
            offset,
            reason,
            kind: ErrorKind::Invalid,
        }
    }

    pub const fn limit(offset: u32, reason: &'static str) -> Self {
        Self {
            offset,
            reason,
            kind: ErrorKind::Limit,
        }
    }
}

impl std::fmt::Display for LoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} (at byte {})", self.reason, self.offset)
    }
}

impl std::error::Error for LoadError {}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u32)]
pub enum TypeKind {
    Unit = 0,
    Bool = 1,
    Byte = 2,
    Int = 3,
    Float = 4,
    Char = 5,
    Time = 6,
    Duration = 7,
    Decimal = 8,
    Range = 9,
    Text = 10,
    Bytes = 11,
    BigInt = 12,
    Tuple = 13,
    List = 14,
    Set = 15,
    Map = 16,
    Record = 17,
    Choice = 18,
    Function = 19,
    Env = 20,
    Capability = 21,
}

impl TypeKind {
    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::Unit),
            1 => Some(Self::Bool),
            2 => Some(Self::Byte),
            3 => Some(Self::Int),
            4 => Some(Self::Float),
            5 => Some(Self::Char),
            6 => Some(Self::Time),
            7 => Some(Self::Duration),
            8 => Some(Self::Decimal),
            9 => Some(Self::Range),
            10 => Some(Self::Text),
            11 => Some(Self::Bytes),
            12 => Some(Self::BigInt),
            13 => Some(Self::Tuple),
            14 => Some(Self::List),
            15 => Some(Self::Set),
            16 => Some(Self::Map),
            17 => Some(Self::Record),
            18 => Some(Self::Choice),
            19 => Some(Self::Function),
            20 => Some(Self::Env),
            21 => Some(Self::Capability),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u32)]
pub enum Capability {
    Args = 0,
    Env = 1,
    Console = 2,
    Files = 3,
    ReadFiles = 4,
    Clock = 5,
    Random = 6,
    Simulation = 7,
    Network = 8,
    Connection = 9,
    Sqlite = 10,
    Database = 11,
    Command = 12,
    SecureRandom = 13,
    File = 14,
}

impl Capability {
    pub const COUNT: usize = 15;

    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::Args),
            1 => Some(Self::Env),
            2 => Some(Self::Console),
            3 => Some(Self::Files),
            4 => Some(Self::ReadFiles),
            5 => Some(Self::Clock),
            6 => Some(Self::Random),
            7 => Some(Self::Simulation),
            8 => Some(Self::Network),
            9 => Some(Self::Connection),
            10 => Some(Self::Sqlite),
            11 => Some(Self::Database),
            12 => Some(Self::Command),
            13 => Some(Self::SecureRandom),
            14 => Some(Self::File),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Args => "Args",
            Self::Env => "Env",
            Self::Console => "Console",
            Self::Files => "Files",
            Self::ReadFiles => "ReadFiles",
            Self::Clock => "Clock",
            Self::Random => "Random",
            Self::Simulation => "Simulation",
            Self::Network => "Network",
            Self::Connection => "Connection",
            Self::Sqlite => "Sqlite",
            Self::Database => "Sqlite.Database",
            Self::Command => "Command",
            Self::SecureRandom => "SecureRandom",
            Self::File => "Files.File",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CaseShape {
    Empty,
    Inline,
    Ref,
    Boxed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u32)]
pub enum LayoutKind {
    Record = 0,
    Tuple = 1,
    Case = 2,
    Closure = 3,
}

impl LayoutKind {
    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::Record),
            1 => Some(Self::Tuple),
            2 => Some(Self::Case),
            3 => Some(Self::Closure),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum FunctionKind {
    Function = 0,
    Closure = 1,
    Init = 2,
    Test = 3,
    Example = 4,
    Promise = 5,
    Anchor = 6,
}

impl FunctionKind {
    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::Function),
            1 => Some(Self::Closure),
            2 => Some(Self::Init),
            3 => Some(Self::Test),
            4 => Some(Self::Example),
            5 => Some(Self::Promise),
            6 => Some(Self::Anchor),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum ParamOwnership {
    Owned = 0,
    Borrowed = 1,
}

impl ParamOwnership {
    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::Owned),
            1 => Some(Self::Borrowed),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum BigIntSign {
    Zero = 0,
    Positive = 1,
    Negative = 2,
}

impl BigIntSign {
    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::Zero),
            1 => Some(Self::Positive),
            2 => Some(Self::Negative),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum AnchorExpectation {
    NotAnchor = 0,
    Allows = 1,
    Forbids = 2,
}

impl AnchorExpectation {
    pub const fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(Self::NotAnchor),
            1 => Some(Self::Allows),
            2 => Some(Self::Forbids),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub magic: [u8; 4],
    pub major: u16,
    pub minor: u16,
    pub flags: u32,
    pub checksum: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TypeField {
    pub name_idx: Option<u32>,
    pub name: Option<String>,
    pub ty: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TypeCase {
    pub name_idx: u32,
    pub name: String,
    pub fields: Vec<TypeField>,
    pub unnamed: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TypeDef {
    pub kind: TypeKind,
    pub parameters: Vec<u32>,
    pub result: u32,
    pub function: u32,
    pub capability: Option<Capability>,
    pub name_idx: Option<u32>,
    pub name: Option<String>,
    pub fields: Vec<TypeField>,
    pub cases: Vec<TypeCase>,
    pub is_reference: bool,
    pub has_fields: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LayoutDef {
    pub kind: LayoutKind,
    pub ty: u32,
    pub case_index: u32,
    pub function: u32,
    pub fields: Vec<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConstantValue {
    Immediate64([u8; 8]),
    Decimal128([u8; 16]),
    Text(u32),
    Bytes(Vec<u8>),
    BigInt {
        sign: BigIntSign,
        magnitude: Vec<u8>,
    },
    Closure(u32),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConstantDef {
    pub ty: u32,
    pub value: ConstantValue,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GlobalDef {
    pub ty: u32,
    pub name: u32,
    pub module: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParameterDef {
    pub ty: u32,
    pub ownership: ParamOwnership,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FunctionDef {
    pub name: u32,
    pub module: u32,
    pub kind: FunctionKind,
    pub spec_code: bool,
    pub parameters: Vec<ParameterDef>,
    pub result_type: u32,
    pub capture_types: Vec<u32>,
    pub register_types: Vec<u32>,
    pub code: Vec<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TypeExport {
    pub ty: u32,
    pub contents_exposed: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModuleDef {
    pub name: u32,
    pub types: Vec<TypeExport>,
    pub functions: Vec<u32>,
    pub globals: Vec<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CodeSpan {
    pub code_offset: u32,
    pub file: u32,
    pub start: u32,
    pub end: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RegisterName {
    pub reg: u32,
    pub name: u32,
    pub code_start: u32,
    pub code_end: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpecLine {
    pub file: u32,
    pub start: u32,
    pub end: u32,
    pub text: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DebugEntry {
    pub spans: Vec<CodeSpan>,
    pub names: Vec<RegisterName>,
    pub text: u32,
    pub file: u32,
    pub start: u32,
    pub end: u32,
    pub subject: u32,
    pub expectation: AnchorExpectation,
    pub lines: Vec<SpecLine>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceTool {
    pub name: u32,
    pub function: u32,
    pub read_only: bool,
    pub argument_types: Vec<u32>,
    pub result_type: u32,
    pub description: u32,
    pub argument_names: Vec<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ServiceTimer {
    pub function: u32,
    pub interval: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceShape {
    pub ty: u32,
    pub is_struct: bool,
    pub unnamed: Vec<bool>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceDef {
    pub module: u32,
    pub state_type: u32,
    pub start: u32,
    pub token_env: u32,
    pub tools: Vec<ServiceTool>,
    pub timers: Vec<ServiceTimer>,
    pub shapes: Vec<ServiceShape>,
}

#[derive(Debug, Clone)]
pub struct BytecodeFile {
    pub header: Header,
    pub program_checksum: u64,
    pub strings: Vec<String>,
    pub types: types::TypeTable,
    pub constants: Vec<ConstantDef>,
    pub globals: Vec<GlobalDef>,
    pub init: Vec<u32>,
    pub functions: Vec<FunctionDef>,
    pub code_offsets: Vec<u32>,
    pub modules: Vec<ModuleDef>,
    pub files: Vec<Vec<u8>>,
    pub debug: Vec<DebugEntry>,
    pub services: Vec<ServiceDef>,
}

impl BytecodeFile {
    pub fn string(&self, idx: u32) -> &str {
        &self.strings[idx as usize]
    }

    pub fn function_name(&self, idx: u32) -> &str {
        self.string(self.functions[idx as usize].name)
    }

    pub fn code_offset(&self, function: u32) -> u32 {
        self.code_offsets
            .get(function as usize)
            .copied()
            .unwrap_or(0)
    }

    pub fn registers_readable(&self, function_idx: u32, pc: u32) -> Result<Vec<bool>, LoadError> {
        verify::registers_readable(self, function_idx, pc)
    }

    pub fn find_span(&self, function: u32, pc: u32) -> Option<(u32, u32, u32)> {
        let spans = &self.debug[function as usize].spans;
        let idx = spans.partition_point(|s| s.code_offset <= pc);
        if idx == 0 {
            return None;
        }
        let s = spans[idx - 1];
        if s.file == NO_INDEX {
            None
        } else {
            Some((s.file, s.start, s.end))
        }
    }
}

/// Decode, structurally validate, and verify a `.bc` v0.3 binary image.
pub fn load(bytes: &[u8]) -> Result<BytecodeFile, LoadError> {
    let file = read::decode(bytes)?;
    verify::verify_module(&file)?;
    Ok(file)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_type(kind: TypeKind) -> TypeDef {
        TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        }
    }

    fn build_minimal_bc() -> Vec<u8> {
        // 10 empty required sections (STRINGS .. DEBUG), each with a 0u32 entry count.
        let body = vec![0u8; 10 * 4];
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

    #[test]
    fn minimal_bytecode_decodes_and_validates() {
        let raw = build_minimal_bc();
        let file = load(&raw).expect("minimal .bc v0.3 must load");
        assert_eq!(file.header.major, BYTECODE_MAJOR);
        assert_eq!(file.header.minor, BYTECODE_MINOR);
        assert!(file.functions.is_empty());
        assert!(file.services.is_empty());
    }

    #[test]
    fn header_and_checksum_errors_rejected() {
        let valid = build_minimal_bc();

        // Truncated header
        assert!(load(&valid[..10]).is_err());

        // Bad magic
        let mut bad_magic = valid.clone();
        bad_magic[0] = b'X';
        assert!(load(&bad_magic).is_err());

        // Unsupported version
        let mut bad_ver = valid.clone();
        bad_ver[6] = 4;
        assert!(load(&bad_ver).is_err());

        // Unknown header flags
        let mut bad_flags = valid.clone();
        bad_flags[8] = 0x80;
        assert!(load(&bad_flags).is_err());

        // Checksum mismatch
        let mut bad_sum = valid.clone();
        let last = bad_sum.len() - 1;
        bad_sum[last] ^= 0xff;
        assert!(load(&bad_sum).is_err());

        // Trailing bytes after sections
        let mut trailing = valid.clone();
        trailing.push(0);
        let sum = fnv1a(FNV_OFFSET, &trailing[BYTECODE_HEADER_SIZE..]);
        trailing[12..20].copy_from_slice(&sum.to_le_bytes());
        assert!(load(&trailing).is_err());
    }

    #[test]
    fn case_shapes_cover_empty_inline_ref_and_boxed() {
        let int_ty = empty_type(TypeKind::Int); // 0
        let text_ty = empty_type(TypeKind::Text); // 1
        let dec_ty = empty_type(TypeKind::Decimal); // 2 (wide 128-bit -> boxed)
        let mut choice = empty_type(TypeKind::Choice); // 3
        choice.name = Some("Sample".to_string());
        choice.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "None".to_string(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "Num".to_string(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: 0,
                }],
                unnamed: true,
            },
            TypeCase {
                name_idx: 2,
                name: "Str".to_string(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: 1,
                }],
                unnamed: true,
            },
            TypeCase {
                name_idx: 3,
                name: "Wide".to_string(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: 2,
                }],
                unnamed: true,
            },
            TypeCase {
                name_idx: 4,
                name: "Pair".to_string(),
                fields: vec![
                    TypeField {
                        name_idx: None,
                        name: None,
                        ty: 0,
                    },
                    TypeField {
                        name_idx: None,
                        name: None,
                        ty: 0,
                    },
                ],
                unnamed: true,
            },
        ];
        let table = types::TypeTable::finish(vec![int_ty, text_ty, dec_ty, choice], Vec::new())
            .expect("type table builds");
        assert_eq!(table.case_shape(3, 0), CaseShape::Empty);
        assert_eq!(table.case_shape(3, 1), CaseShape::Inline);
        assert_eq!(table.case_shape(3, 2), CaseShape::Ref);
        assert_eq!(table.case_shape(3, 3), CaseShape::Boxed);
        assert_eq!(table.case_shape(3, 4), CaseShape::Boxed);
        assert!(table.is_reference(3));
    }
}
