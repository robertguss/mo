//! Typed arena indices for modules, symbols, types, signatures, and ability proofs.

/// Index of a loaded module inside a [`crate::ModuleSet`] and [`crate::ModuleInterfaces`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ModuleId(pub u32);

impl ModuleId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::Symbol`] inside its declaring [`crate::ModuleScope`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SymIdx(pub u32);

impl SymIdx {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Globally unique reference to a declared symbol `(module, symbol)`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SymRef {
    pub module: ModuleId,
    pub symbol: SymIdx,
}

impl SymRef {
    #[must_use]
    pub const fn new(module: ModuleId, symbol: SymIdx) -> Self {
        Self { module, symbol }
    }
}

/// Index of an interned [`crate::SourceType`] inside a [`crate::TypeArena`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TypeId(pub u32);

impl TypeId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::FunctionSignature`] inside [`crate::Signatures`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FuncSigId(pub u32);

impl FuncSigId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::DataSignature`] inside [`crate::Signatures`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DataSigId(pub u32);

impl DataSigId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::NominalSignature`] inside [`crate::Signatures`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct NominalId(pub u32);

impl NominalId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::NativeSignature`] inside [`crate::Signatures`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct NativeSigId(pub u32);

impl NativeSigId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of an [`crate::AbilityImplementation`] inside an [`crate::AbilityArena`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ProofId(pub u32);

impl ProofId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedExpression`] inside a [`crate::checked::CheckedBody`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedExprId(pub u32);

impl CheckedExprId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedBinding`] inside a [`crate::checked::CheckedBody`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedBindingId(pub u32);

impl CheckedBindingId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedPattern`] inside a [`crate::checked::CheckedBody`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedPatternId(pub u32);

impl CheckedPatternId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedLambda`] inside a [`crate::checked::CheckedBody`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedLambdaId(pub u32);

impl CheckedLambdaId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedMatch`] inside a [`crate::checked::CheckedBody`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedMatchId(pub u32);

impl CheckedMatchId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedFunction`] inside [`crate::checked::CheckedProgram::functions`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedFuncId(pub u32);

impl CheckedFuncId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedConstant`] inside [`crate::checked::CheckedProgram::constants`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedConstId(pub u32);

impl CheckedConstId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedSpec`] inside [`crate::checked::CheckedProgram::specs`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedSpecId(pub u32);

impl CheckedSpecId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedService`] inside [`crate::checked::CheckedProgram::services`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedServiceId(pub u32);

impl CheckedServiceId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// Index of a [`crate::checked::CheckedApi`] inside [`crate::checked::CheckedProgram::apis`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CheckedApiId(pub u32);

impl CheckedApiId {
    #[must_use]
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}
