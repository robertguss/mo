#![forbid(unsafe_code)]

//! Build-time generated contract tables parsed directly from `src/bytecode/opcodes.def`,
//! `src/base/prims.def`, `src/base/hostops.def`, and `src/base/limits.h`.

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum OperandKind {
    None,
    Register,
    Type,
    Layout,
    Constant,
    Global,
    Function,
    Primitive,
    Host,
    Target,
    Field,
    Case,
    Index,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum OperandAccess {
    None,
    Write,
    Read,
    Consume,
    Move,
    CallParameter,
    PrimParameter,
    HostParameter,
    TokenWrite,
    TokenConsume,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct OperandInfo {
    pub kind: OperandKind,
    pub access: OperandAccess,
}

impl OperandInfo {
    pub const fn new(kind: OperandKind, access: OperandAccess) -> Self {
        Self { kind, access }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OpcodeInfo {
    pub opcode: Opcode,
    pub id: &'static str,
    pub number: u16,
    pub name: &'static str,
    pub fixed_count: u8,
    pub variable_limit: u16,
    pub variable: OperandInfo,
    pub fixed: [OperandInfo; 4],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PrimInfo {
    pub prim: PrimId,
    pub id: &'static str,
    pub module: &'static str,
    pub name: &'static str,
    pub signature: &'static str,
    pub params: &'static str,
    pub bugs: &'static str,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HostOpInfo {
    pub op: HostOpId,
    pub id: &'static str,
    pub module: &'static str,
    pub name: &'static str,
    pub signature: &'static str,
    pub params: &'static str,
    pub bugs: &'static str,
    pub spec_only: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SimOpInfo {
    pub op: SimOpId,
    pub id: &'static str,
    pub module: &'static str,
    pub name: &'static str,
    pub spec_only: bool,
}

include!(concat!(env!("OUT_DIR"), "/generated.rs"));

impl Opcode {
    pub const fn find(number: u32) -> Option<&'static OpcodeInfo> {
        if number == 0 || number > OPCODE_COUNT as u32 {
            None
        } else {
            Some(&OPCODES[(number as usize) - 1])
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pin_opcode_counts_and_boundaries() {
        assert_eq!(OPCODE_COUNT, 87);
        assert_eq!(Opcode::Move.number(), 1);
        assert_eq!(Opcode::Sim.number(), 87);
        assert_eq!(Opcode::from_u16(0), None);
        assert_eq!(Opcode::from_u16(1), Some(Opcode::Move));
        assert_eq!(Opcode::from_u16(87), Some(Opcode::Sim));
        assert_eq!(Opcode::from_u16(88), None);

        let mv = Opcode::Move.info();
        assert_eq!(mv.name, "move");
        assert_eq!(mv.fixed_count, 2);
        assert_eq!(mv.variable_limit, 0);
        assert_eq!(mv.fixed[0].access, OperandAccess::Write);
        assert_eq!(mv.fixed[1].access, OperandAccess::Move);

        let call_closure = Opcode::CallClosure.info();
        assert_eq!(call_closure.name, "call_closure");
        assert_eq!(call_closure.variable_limit, 255);

        let call = Opcode::Call.info();
        assert_eq!(call.variable_limit, 256);

        let sim = Opcode::Sim.info();
        assert_eq!(sim.name, "sim");
        assert_eq!(sim.fixed_count, 4);
        assert_eq!(sim.variable_limit, 256);

        for (i, info) in OPCODES.iter().enumerate() {
            assert_eq!(info.number as usize, i + 1);
            assert_eq!(Opcode::from_u16(info.number), Some(info.opcode));
        }
    }

    #[test]
    fn pin_prim_counts_and_boundaries() {
        assert_eq!(PRIM_COUNT, 187);
        assert_eq!(PrimId::IntAbs as u32, 0);
        assert_eq!(PrimId::FloatToText as u32, 61);
        assert_eq!(PrimId::BytesDecodeUtf8 as u32, 103);
        assert_eq!(PrimId::RangeContains as u32, 148);
        assert_eq!(PrimId::Unicode16Uppercase as u32, 159);
        assert_eq!(PrimId::BytesCompareSliceAt as u32, 186);
        assert_eq!(PrimId::from_u32(187), None);

        let first = PrimId::IntAbs.info();
        assert_eq!(first.module, "Int");
        assert_eq!(first.name, "abs");
        assert_eq!(first.params, "b");

        let last = PrimId::BytesCompareSliceAt.info();
        assert_eq!(last.module, "Bytes");
        assert_eq!(last.name, "compare_slice_at");
        assert_eq!(last.params, "bbbb");
    }

    #[test]
    fn pin_hostop_and_simop_counts_and_boundaries() {
        assert_eq!(HOSTOP_COUNT, 59);
        assert_eq!(HostOpId::ArgsList as u32, 0);
        assert_eq!(HostOpId::ConsoleFakeInput as u32, 10);
        assert_eq!(HostOpId::FilesSync as u32, 58);
        assert_eq!(HostOpId::from_u32(59), None);

        let args_list = HostOpId::ArgsList.info();
        assert_eq!(args_list.module, "Args");
        assert_eq!(args_list.name, "list");
        assert!(!args_list.spec_only);

        let console_fake = HostOpId::ConsoleFake.info();
        assert_eq!(console_fake.module, "Console");
        assert_eq!(console_fake.name, "fake");
        assert!(console_fake.spec_only);
        assert_eq!(console_fake.params, "");

        let sync = HostOpId::FilesSync.info();
        assert_eq!(sync.module, "Files");
        assert_eq!(sync.name, "sync");
        assert!(!sync.spec_only);

        assert_eq!(SIMOP_COUNT, 5);
        assert_eq!(SimOpId::Start as u32, 0);
        assert_eq!(SimOpId::Restart as u32, 4);
        assert!(SimOpId::Start.info().spec_only);
        assert!(!SimOpId::Restart.info().spec_only);
    }

    #[test]
    fn pin_named_limits() {
        assert_eq!(limits::MO_LIMIT_BYTECODE_SIZE, 256 << 20);
        assert_eq!(limits::MO_LIMIT_PARAMETERS, 255);
        assert_eq!(limits::MO_LIMIT_BYTECODE_PARAMETERS, 256);
        assert_eq!(limits::MO_LIMIT_FIELDS, 65535);
        assert_eq!(limits::MO_LIMIT_CHOICE_CASES, 65535);
        assert_eq!(limits::MO_LIMIT_FUNCTION_REGISTERS, 65535);
        assert_eq!(limits::MO_LIMIT_FUNCTION_INSTRUCTIONS, 1 << 18);
        assert_eq!(limits::MO_LIMIT_FUNCTION_CODE_WORDS, 1 << 20);
        assert_eq!(limits::MO_DEFAULT_STACK_SIZE, 8 << 20);
        assert_eq!(limits::MO_DEFAULT_CALL_DEPTH, 10_000);
        assert_eq!(limits::MO_DEFAULT_HEAP_SIZE, 1 << 30);
        assert_eq!(limits::MO_TIME_MIN_US, -62_135_596_800_000_000);
        assert_eq!(limits::MO_TIME_MAX_US, 253_402_300_799_999_999);
        assert_eq!(limits::MO_LIMIT_PATH_LINKS, 40);
        assert_eq!(limits::MO_LIMIT_OPEN_FOLDERS, 256);
        assert_eq!(limits::MO_LIMIT_OPEN_FILES, 64);
    }
}
