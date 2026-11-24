//! Paged upgrade bundle upload staging (`src/base/upload.c` parity).

use mo_def::limits::{MO_LIMIT_BUNDLE_BYTES, MO_LIMIT_UPGRADE_PAGE_BYTES};

const MAX_UPLOAD_PAGES: u64 = MO_LIMIT_BUNDLE_BYTES.div_ceil(MO_LIMIT_UPGRADE_PAGE_BYTES);

/// Outcome of accepting one upload page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UploadPageStatus {
    /// Page accepted at the expected offset.
    Accepted,
    /// Exact duplicate page replayed idempotently.
    Duplicate,
    /// Page violates offset, size, or ordering rules.
    Invalid,
}

/// Bounded in-memory staging buffer for a paged `MOSB\x01` bundle upload.
#[derive(Debug, Clone)]
pub struct UploadStage {
    expected_total: usize,
    pages_received: usize,
    bytes: Vec<u8>,
}

impl UploadStage {
    /// Starts a new upload stage expecting `total_bytes` (`1..=MO_LIMIT_BUNDLE_BYTES`).
    #[must_use]
    pub fn open(total_bytes: usize) -> Option<Self> {
        if total_bytes == 0 || total_bytes as u64 > MO_LIMIT_BUNDLE_BYTES {
            return None;
        }
        Some(Self {
            expected_total: total_bytes,
            pages_received: 0,
            bytes: Vec::with_capacity(total_bytes.min(65_536)),
        })
    }

    /// Returns the expected total bundle size in bytes.
    #[must_use]
    pub fn expected_total(&self) -> usize {
        self.expected_total
    }

    /// Returns the number of bytes received so far.
    #[must_use]
    pub fn received(&self) -> usize {
        self.bytes.len()
    }

    /// Returns `true` when all expected bytes have been received.
    #[must_use]
    pub fn is_complete(&self) -> bool {
        self.bytes.len() == self.expected_total
    }

    /// Returns the received bytes slice.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Appends or verifies one page at `offset`.
    pub fn push_page(&mut self, offset: usize, data: &[u8]) -> UploadPageStatus {
        if data.is_empty() || data.len() as u64 > MO_LIMIT_UPGRADE_PAGE_BYTES {
            return UploadPageStatus::Invalid;
        }
        let Some(end) = offset.checked_add(data.len()) else {
            return UploadPageStatus::Invalid;
        };
        if end > self.expected_total {
            return UploadPageStatus::Invalid;
        }
        if offset == self.bytes.len() {
            if self.pages_received as u64 >= MAX_UPLOAD_PAGES {
                return UploadPageStatus::Invalid;
            }
            self.bytes.extend_from_slice(data);
            self.pages_received += 1;
            return UploadPageStatus::Accepted;
        }
        if end <= self.bytes.len() && &self.bytes[offset..end] == data {
            return UploadPageStatus::Duplicate;
        }
        UploadPageStatus::Invalid
    }
}
