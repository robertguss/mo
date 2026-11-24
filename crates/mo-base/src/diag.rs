//! Compiler diagnostics, fixes, edit-overlap validation, and bounded collection
//! (`src/base/diag.c` parity).

use std::borrow::Cow;
use std::fmt;

use mo_def::ErrorCode;
use mo_def::limits::{MO_LIMIT_DIAGNOSTICS, MO_LIMIT_DIAGNOSTICS_SIZE, MO_LIMIT_FIX_EDITS};
use thiserror::Error;

use crate::source::Span;

/// Owned text payload inside a diagnostic.
///
/// Stores raw bytes (not requiring UTF-8) so diagnostics can faithfully carry
/// arbitrary byte paths or headlines, matching `src/base/diag.c` and
/// `tests/cli/render-{text,json}.golden`.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct DiagText(Vec<u8>);

impl DiagText {
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }

    #[must_use]
    pub fn as_str_lossy(&self) -> Cow<'_, str> {
        String::from_utf8_lossy(&self.0)
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.0.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

impl From<&str> for DiagText {
    fn from(s: &str) -> Self {
        Self(s.as_bytes().to_vec())
    }
}

impl From<String> for DiagText {
    fn from(s: String) -> Self {
        Self(s.into_bytes())
    }
}

impl From<&[u8]> for DiagText {
    fn from(b: &[u8]) -> Self {
        Self(b.to_vec())
    }
}

impl From<Vec<u8>> for DiagText {
    fn from(b: Vec<u8>) -> Self {
        Self(b)
    }
}

impl fmt::Display for DiagText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.as_str_lossy())
    }
}

/// Primary or secondary source annotation on a diagnostic.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Label {
    pub span: Span,
    pub text: Option<DiagText>,
}

impl Label {
    #[must_use]
    pub fn new(span: Span, text: Option<DiagText>) -> Self {
        Self { span, text }
    }

    #[must_use]
    pub fn bare(span: Span) -> Self {
        Self { span, text: None }
    }

    #[must_use]
    pub fn with_text(span: Span, text: impl Into<DiagText>) -> Self {
        Self {
            span,
            text: Some(text.into()),
        }
    }
}

/// Single replacement or insertion edit belonging to a [`Fix`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Edit {
    pub span: Span,
    pub replacement: DiagText,
}

impl Edit {
    #[must_use]
    pub fn new(span: Span, replacement: impl Into<DiagText>) -> Self {
        Self {
            span,
            replacement: replacement.into(),
        }
    }
}

/// Machine-applicable suggested fix consisting of non-overlapping edits.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Fix {
    pub title: Option<DiagText>,
    pub edits: Vec<Edit>,
}

impl Fix {
    #[must_use]
    pub fn new(title: Option<DiagText>, edits: Vec<Edit>) -> Self {
        Self { title, edits }
    }
}

/// Complete compiler diagnostic matching `struct mo_diag`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diagnostic {
    pub code: ErrorCode,
    pub section: Option<&'static str>,
    pub message: DiagText,
    pub reason: Option<DiagText>,
    pub primary: Label,
    pub secondary: Vec<Label>,
    pub fixes: Vec<Fix>,
}

impl Diagnostic {
    #[must_use]
    pub fn new(code: ErrorCode, primary_span: Span, message: impl Into<DiagText>) -> Self {
        Self {
            code,
            section: code.section(),
            message: message.into(),
            reason: None,
            primary: Label::bare(primary_span),
            secondary: Vec::new(),
            fixes: Vec::new(),
        }
    }

    #[must_use]
    pub fn with_primary_text(mut self, text: impl Into<DiagText>) -> Self {
        self.primary.text = Some(text.into());
        self
    }

    #[must_use]
    pub fn with_reason(mut self, reason: impl Into<DiagText>) -> Self {
        self.reason = Some(reason.into());
        self
    }

    #[must_use]
    pub fn with_secondary(mut self, label: Label) -> Self {
        self.secondary.push(label);
        self
    }

    #[must_use]
    pub fn with_fix(mut self, fix: Fix) -> Self {
        self.fixes.push(fix);
        self
    }
}

/// Returns whether two edits overlap in the same file, matching `src/base/diag.c:mo_edits_overlap`.
///
/// Two insertions at the exact same offset overlap; an insertion at the start or end
/// boundary of a non-empty replacement does not overlap.
#[must_use]
pub const fn edits_overlap(a: &Edit, b: &Edit) -> bool {
    if a.span.file != b.span.file {
        return false;
    }
    let a_inserts = a.span.start == a.span.end;
    let b_inserts = b.span.start == b.span.end;
    if a_inserts && b_inserts {
        return a.span.start == b.span.start;
    }
    a.span.start < b.span.end && b.span.start < a.span.end
}

/// Structural invariant errors in a diagnostic passed to [`DiagBag::push`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum DiagValidationError {
    #[error("span end precedes span start")]
    BackwardSpan,
    #[error("diagnostic section does not match its error code's reference section")]
    SectionMismatch,
    #[error("diagnostic with a reference section must include a reason")]
    MissingReasonForCitedCode,
    #[error("fix must contain at least one edit")]
    EmptyFix,
    #[error("fix exceeds MO_LIMIT_FIX_EDITS ({MO_LIMIT_FIX_EDITS}) edits")]
    TooManyFixEdits,
    #[error("fix contains overlapping edits")]
    OverlappingFixEdits,
}

fn validate_diag(diag: &Diagnostic) -> Result<(), DiagValidationError> {
    let code_section = diag.code.section();
    if let Some(sec) = diag.section
        && code_section != Some(sec)
    {
        return Err(DiagValidationError::SectionMismatch);
    }
    if diag.reason.is_none() && code_section.is_some() {
        return Err(DiagValidationError::MissingReasonForCitedCode);
    }
    if !diag.primary.span.is_ordered() {
        return Err(DiagValidationError::BackwardSpan);
    }
    for label in &diag.secondary {
        if !label.span.is_ordered() {
            return Err(DiagValidationError::BackwardSpan);
        }
    }
    for fix in &diag.fixes {
        if fix.edits.is_empty() {
            return Err(DiagValidationError::EmptyFix);
        }
        if (fix.edits.len() as u64) > MO_LIMIT_FIX_EDITS {
            return Err(DiagValidationError::TooManyFixEdits);
        }
        for (i, edit) in fix.edits.iter().enumerate() {
            if !edit.span.is_ordered() {
                return Err(DiagValidationError::BackwardSpan);
            }
            for earlier in &fix.edits[..i] {
                if edits_overlap(edit, earlier) {
                    return Err(DiagValidationError::OverlappingFixEdits);
                }
            }
        }
    }
    Ok(())
}

/// Estimates the diagnostic payload byte footprint matching `src/base/diag.c:count_diag`.
fn diag_byte_cost(diag: &Diagnostic) -> Option<usize> {
    const LABEL_BYTES: usize = 24;
    const FIX_BYTES: usize = 24;
    const EDIT_BYTES: usize = 24;

    let add_text = |total: &mut usize, text: Option<&DiagText>| -> Option<()> {
        if let Some(t) = text {
            *total = total.checked_add(t.len().checked_add(1)?)?;
        }
        Some(())
    };

    let mut size = diag.secondary.len().checked_mul(LABEL_BYTES)?;
    size = size.checked_add(diag.fixes.len().checked_mul(FIX_BYTES)?)?;
    add_text(&mut size, diag.primary.text.as_ref())?;
    add_text(&mut size, Some(&diag.message))?;
    add_text(&mut size, diag.reason.as_ref())?;
    for label in &diag.secondary {
        add_text(&mut size, label.text.as_ref())?;
    }
    for fix in &diag.fixes {
        size = size.checked_add(fix.edits.len().checked_mul(EDIT_BYTES)?)?;
        add_text(&mut size, fix.title.as_ref())?;
        for edit in &fix.edits {
            add_text(&mut size, Some(&edit.replacement))?;
        }
    }
    Some(size)
}

/// Bounded collection of diagnostics enforcing `MO_LIMIT_DIAGNOSTICS` and
/// `MO_LIMIT_DIAGNOSTICS_SIZE`, matching `struct mo_diags`.
#[derive(Debug, Clone)]
pub struct DiagBag {
    items: Vec<Diagnostic>,
    dropped: u32,
    used_bytes: usize,
}

/// Baseline array reservation matching `mo_diags_init` reserving
/// `MO_LIMIT_DIAGNOSTICS * sizeof(struct mo_diag)` from the 64 MiB diagnostics budget.
const DIAG_TABLE_OVERHEAD: usize = (MO_LIMIT_DIAGNOSTICS as usize) * 80;

impl Default for DiagBag {
    fn default() -> Self {
        Self::new()
    }
}

impl DiagBag {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            items: Vec::new(),
            dropped: 0,
            used_bytes: DIAG_TABLE_OVERHEAD,
        }
    }

    #[must_use]
    pub fn items(&self) -> &[Diagnostic] {
        &self.items
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.items.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    #[must_use]
    pub const fn dropped(&self) -> u32 {
        self.dropped
    }

    #[must_use]
    pub const fn used_bytes(&self) -> usize {
        self.used_bytes
    }

    /// Returns whether any diagnostic was recorded or dropped.
    #[must_use]
    pub fn has_any(&self) -> bool {
        !self.items.is_empty() || self.dropped > 0
    }

    /// Validates and records `diag`, normalizing `diag.section = diag.code.section()`.
    ///
    /// Returns:
    /// - `Ok(true)` if kept.
    /// - `Ok(false)` if dropped due to count or memory limits (`dropped` is incremented).
    /// - `Err(err)` if `diag` violates structural diagnostic invariants.
    pub fn push(&mut self, mut diag: Diagnostic) -> Result<bool, DiagValidationError> {
        validate_diag(&diag)?;
        diag.section = diag.code.section();

        let limit_bytes = MO_LIMIT_DIAGNOSTICS_SIZE as usize;
        let cost = diag_byte_cost(&diag);
        let fits_count = (self.items.len() as u64) < MO_LIMIT_DIAGNOSTICS;
        let fits_bytes = cost.is_some_and(|c| {
            self.used_bytes
                .checked_add(c)
                .is_some_and(|next| next <= limit_bytes)
        });
        if !fits_count || !fits_bytes {
            self.dropped = self.dropped.saturating_add(1);
            return Ok(false);
        }
        if let Some(c) = cost {
            self.used_bytes += c;
        }
        self.items.push(diag);
        Ok(true)
    }

    /// Stably sorts diagnostics by `(primary.span.file, primary.span.start)`,
    /// matching `src/base/diag.c:mo_diags_sort`.
    pub fn sort(&mut self) {
        self.items
            .sort_by_key(|d| (d.primary.span.file, d.primary.span.start));
    }
}
