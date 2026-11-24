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
