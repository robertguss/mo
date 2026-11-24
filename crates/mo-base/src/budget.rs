//! Deterministic work budget accounting for compiler passes.

use thiserror::Error;

/// Returned when a compiler pass exhausts its work budget.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
#[error("work budget exhausted (limit: {limit})")]
pub struct BudgetExceeded {
    pub limit: u64,
}

/// Counts bounded work units and refuses once `limit` would be exceeded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WorkBudget {
    limit: u64,
    used: u64,
}

impl WorkBudget {
    #[must_use]
    pub const fn new(limit: u64) -> Self {
        Self { limit, used: 0 }
    }

    #[must_use]
    pub const fn limit(&self) -> u64 {
        self.limit
    }

    #[must_use]
    pub const fn used(&self) -> u64 {
        self.used
    }

    #[must_use]
    pub const fn remaining(&self) -> u64 {
        self.limit.saturating_sub(self.used)
    }

    /// Charges `units` against the budget, failing without partial charge if
    /// doing so would exceed `self.limit`.
    pub fn charge(&mut self, units: u64) -> Result<(), BudgetExceeded> {
        let Some(next) = self.used.checked_add(units) else {
            return Err(BudgetExceeded { limit: self.limit });
        };
        if next > self.limit {
            return Err(BudgetExceeded { limit: self.limit });
        }
        self.used = next;
        Ok(())
    }

    /// Charges one work unit.
    pub fn step(&mut self) -> Result<(), BudgetExceeded> {
        self.charge(1)
    }
}
