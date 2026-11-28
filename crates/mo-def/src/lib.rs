#![forbid(unsafe_code)]

//! Build-time generated contract tables parsed directly from `crates/mo-def/defs/`:
//! `opcodes.def`, `prims.def`, `hostops.def`, `limits.h`, `codes.h`, `core.h`,
//! `ops.def`, and `version.def`.

use std::fmt;

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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ErrorCodeInfo {
    pub code: ErrorCode,
    pub number: u16,
    pub id: &'static str,
    pub section: Option<&'static str>,
}

include!(concat!(env!("OUT_DIR"), "/generated.rs"));

impl fmt::Display for ErrorCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "E{:04}", self.number())
    }
}

impl Opcode {
    pub const fn find(number: u32) -> Option<&'static OpcodeInfo> {
        if number == 0 || number > OPCODE_COUNT as u32 {
            None
        } else {
            Some(&OPCODES[(number as usize) - 1])
        }
    }
}

/// Returns whether a Unicode scalar value is banned verbatim in Mo source (reference 2.1):
/// C0 controls other than tab and line breaks, DEL, C1 controls, zero-width characters,
/// direction overrides, and invisible formatting scalars.
#[must_use]
pub const fn is_invisible_scalar(c: u32) -> bool {
    (c < 32 && c != 9 && c != 10 && c != 13)
        || c == 127
        || (c >= 128 && c <= 159)
        || c == 0xad
        || c == 0x61c
        || c == 0x180e
        || (c >= 0x200b && c <= 0x200f)
        || (c >= 0x2028 && c <= 0x202e)
        || (c >= 0x2060 && c <= 0x2064)
        || (c >= 0x2066 && c <= 0x206f)
        || c == 0xfeff
        || (c >= 0xfff9 && c <= 0xfffb)
        || c == 0xe0001
        || (c >= 0xe0020 && c <= 0xe007f)
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
        assert_eq!(PRIM_COUNT, 193);
        assert_eq!(PrimId::IntAbs as u32, 0);
        assert_eq!(PrimId::FloatToText as u32, 61);
        assert_eq!(PrimId::BytesDecodeUtf8 as u32, 103);
        assert_eq!(PrimId::RangeContains as u32, 148);
        assert_eq!(PrimId::Unicode16Uppercase as u32, 159);
        assert_eq!(PrimId::BytesCompareSliceAt as u32, 186);
        assert_eq!(PrimId::JsonQuoteRaw as u32, 187);
        assert_eq!(PrimId::JsonParseRaw as u32, 188);
        assert_eq!(PrimId::BytesDotU16BeBiased as u32, 189);
        assert_eq!(PrimId::BytesDotU8Biased as u32, 190);
        assert_eq!(PrimId::CryptoHmacSha1 as u32, 191);
        assert_eq!(PrimId::CryptoVerifySignatureRaw as u32, 192);
        assert_eq!(PrimId::from_u32(193), None);

        let first = PrimId::IntAbs.info();
        assert_eq!(first.module, "Int");
        assert_eq!(first.name, "abs");
        assert_eq!(first.params, "b");

        let last = PrimId::CryptoVerifySignatureRaw.info();
        assert_eq!(last.module, "Crypto");
        assert_eq!(last.name, "verify_signature_raw");
        assert_eq!(last.params, "bbbb");
    }

    #[test]
    fn pin_hostop_and_simop_counts_and_boundaries() {
        assert_eq!(HOSTOP_COUNT, 74);
        assert_eq!(HostOpId::ArgsList as u32, 0);
        assert_eq!(HostOpId::ConsoleFakeInput as u32, 10);
        assert_eq!(HostOpId::FilesSync as u32, 58);
        assert_eq!(HostOpId::FilesLockShared as u32, 59);
        assert_eq!(HostOpId::FilesLockExclusive as u32, 60);
        assert_eq!(HostOpId::FilesUnlock as u32, 61);
        assert_eq!(HostOpId::ReadfilesOpen as u32, 62);
        assert_eq!(HostOpId::NetworkFakeScripted as u32, 63);
        assert_eq!(HostOpId::NetworkRecordedCalls as u32, 64);
        assert_eq!(HostOpId::SqliteStatsRaw as u32, 65);
        assert_eq!(HostOpId::SqliteResetStatsRaw as u32, 66);
        assert_eq!(HostOpId::CommandAllowLoopback as u32, 67);
        assert_eq!(HostOpId::CommandProcessWaitRaw as u32, 72);
        assert_eq!(HostOpId::CommandProcessPollRaw as u32, 73);
        assert_eq!(HostOpId::from_u32(74), None);

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

        let lock_shared = HostOpId::FilesLockShared.info();
        assert_eq!(lock_shared.module, "Files");
        assert_eq!(lock_shared.name, "lock_shared");
        assert!(!lock_shared.spec_only);

        let read_open = HostOpId::ReadfilesOpen.info();
        assert_eq!(read_open.module, "ReadFiles");
        assert_eq!(read_open.name, "open");
        assert!(!read_open.spec_only);

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

        // Front-end & Core limits
        assert_eq!(limits::MO_LIMIT_FILE_SIZE, 8 << 20);
        assert_eq!(limits::MO_LIMIT_PROGRAM_SIZE, 64 << 20);
        assert_eq!(limits::MO_LIMIT_FILES, 4096);
        assert_eq!(limits::MO_LIMIT_PATH_SIZE, 4096);
        assert_eq!(limits::MO_LIMIT_MODULES, 4096);
        assert_eq!(limits::MO_LIMIT_DIAGNOSTICS, 1000);
        assert_eq!(limits::MO_LIMIT_DIAGNOSTICS_SIZE, 64 << 20);
        assert_eq!(limits::MO_LIMIT_FIX_EDITS, 1024);
        assert_eq!(limits::MO_CLI_DIAGNOSTIC_SIZE, 1 << 20);
        assert_eq!(limits::MO_LIMIT_CORE_DEPTH, 4096);
        assert_eq!(limits::MO_LIMIT_CORE_TEXT_SIZE, 8 << 20);
        assert_eq!(limits::MO_LIMIT_CORE_NODES, 1 << 20);
        assert_eq!(limits::MO_LIMIT_CORE_SCRATCH_SIZE, 128 << 20);
        assert_eq!(limits::MO_LIMIT_CORE_CHECK_SCRATCH_SIZE, 512 * (1 << 20));
    }

    #[test]
    fn pin_error_codes_parity_with_codes_h() {
        assert_eq!(ErrorCode::SourceUtf8.number(), 1);
        assert_eq!(ErrorCode::SourceUtf8.section(), Some("2.1"));
        assert_eq!(ErrorCode::SourceUtf8.to_string(), "E0001");
        assert_eq!(ErrorCode::FailureIgnored.number(), 412);
        assert_eq!(ErrorCode::FailureIgnored.section(), Some("8.3"));
        assert_eq!(ErrorCode::FailureIgnored.to_string(), "E0412");
        assert_eq!(ErrorCode::InstantiationsTooDeep.number(), 1012);
        assert_eq!(ErrorCode::InstantiationsTooDeep.section(), None);
        assert_eq!(ErrorCode::CoreTextLimit.number(), 1016);
        assert_eq!(ErrorCode::CoreTextLimit.section(), None);
        assert_eq!(ErrorCode::CoreCheckMetadata.number(), 1027);
        assert_eq!(ErrorCode::CoreCheckMemory.number(), 1053);
        assert_eq!(ErrorCode::MutantSurvived.number(), 1439);
        assert_eq!(ErrorCode::MutantSurvived.section(), None);
        assert_eq!(ErrorCode::from_u16(0), None);
        assert_eq!(ErrorCode::from_u16(413), None);
        assert_eq!(ErrorCode::from_u32(412), Some(ErrorCode::FailureIgnored));

        for info in &ERROR_CODES {
            assert_eq!(ErrorCode::from_u16(info.number), Some(info.code));
            assert_eq!(info.code.info(), info);
            assert_eq!(info.code.number(), info.number);
            assert_eq!(info.code.id(), info.id);
            assert_eq!(info.code.section(), info.section);
            if info.number < 1000 {
                assert!(
                    info.section.is_some(),
                    "language code {} must cite a section",
                    info.code
                );
            } else {
                assert!(
                    info.section.is_none(),
                    "implementation code {} must not cite a section",
                    info.code
                );
            }
        }
    }

    #[test]
    fn pin_core_ops_and_version_parity() {
        use core_ops::*;

        assert_eq!(MO_CORE_TEXT_VERSION, "0.11");
        assert_eq!(CoreTypedOp::ALL.len(), 2);
        assert_eq!(CoreTypedOp::Eq.wire_name(), "eq");
        assert_eq!(CoreTypedOp::Structural.wire_name(), "structural");

        assert_eq!(CoreIntOp::ALL.len(), 32);
        assert_eq!(CoreIntOp::Add.wire_name(), "add");
        assert_eq!(CoreIntOp::Days.wire_name(), "days");

        assert_eq!(CoreBigOp::ALL.len(), 14);
        assert_eq!(CoreByteOp::ALL.len(), 2);
        assert_eq!(CoreDecOp::ALL.len(), 16);
        assert_eq!(CoreFloatOp::ALL.len(), 15);
        assert_eq!(CoreBase::ALL.len(), 11);
        assert_eq!(CoreCmpOp::ALL.len(), 4);
        assert_eq!(CoreCharOp::ALL.len(), 8);
        assert_eq!(CoreTextOp::ALL.len(), 29);
        assert_eq!(CoreBytesOp::ALL.len(), 23);
        assert_eq!(CoreUnicodeOp::ALL.len(), 7);
        assert_eq!(CoreCryptoOp::ALL.len(), 9);
        assert_eq!(CoreJsonOp::ALL.len(), 2);
        assert_eq!(CoreRangeOp::ALL.len(), 6);
        assert_eq!(CoreTimeOp::ALL.len(), 13);
        assert_eq!(CoreListNullaryOp::ALL.len(), 17);
        assert_eq!(CoreMapNullaryOp::ALL.len(), 11);
        assert_eq!(CoreSetOp::ALL.len(), 11);
        assert_eq!(CoreIoNullaryOp::ALL.len(), 62);

        // Verify round-trip wire_name <-> from_wire_name on every family.
        for &op in CoreTypedOp::ALL {
            assert_eq!(CoreTypedOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreIntOp::ALL {
            assert_eq!(CoreIntOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreBigOp::ALL {
            assert_eq!(CoreBigOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreByteOp::ALL {
            assert_eq!(CoreByteOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreDecOp::ALL {
            assert_eq!(CoreDecOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreFloatOp::ALL {
            assert_eq!(CoreFloatOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreBase::ALL {
            assert_eq!(CoreBase::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreCmpOp::ALL {
            assert_eq!(CoreCmpOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreCharOp::ALL {
            assert_eq!(CoreCharOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreTextOp::ALL {
            assert_eq!(CoreTextOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreBytesOp::ALL {
            assert_eq!(CoreBytesOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreUnicodeOp::ALL {
            assert_eq!(CoreUnicodeOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreCryptoOp::ALL {
            assert_eq!(CoreCryptoOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreJsonOp::ALL {
            assert_eq!(CoreJsonOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreRangeOp::ALL {
            assert_eq!(CoreRangeOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreTimeOp::ALL {
            assert_eq!(CoreTimeOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreListNullaryOp::ALL {
            assert_eq!(CoreListNullaryOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreMapNullaryOp::ALL {
            assert_eq!(CoreMapNullaryOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreSetOp::ALL {
            assert_eq!(CoreSetOp::from_wire_name(op.wire_name()), Some(op));
        }
        for &op in CoreIoNullaryOp::ALL {
            assert_eq!(CoreIoNullaryOp::from_wire_name(op.wire_name()), Some(op));
        }
    }
}
