//! Bearer token reading, constant-time validation, and zeroization
//! (`src/host/credentials.c` parity).

use mo_def::limits::MO_LIMIT_MCP_TOKEN_BYTES;
use zeroize::Zeroizing;

/// Secret bearer token stored in zeroizing memory (`struct mo_bearer`).
#[derive(Clone)]
pub struct Bearer {
    bytes: Zeroizing<Vec<u8>>,
}

impl std::fmt::Debug for Bearer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Bearer")
            .field("length", &self.bytes.len())
            .finish()
    }
}

impl Bearer {
    /// Reads and validates the bearer token from environment variable `env_name`
    /// (`mo_bearer_read`).
    ///
    /// Rejects missing, empty, oversized (`> MO_LIMIT_MCP_TOKEN_BYTES`), or non-visible ASCII
    /// tokens (matching `src/host/credentials.c`).
    #[must_use]
    pub fn from_env(env_name: &str) -> Option<Self> {
        if env_name.is_empty() {
            return None;
        }
        let raw = std::env::var_os(env_name)?;
        let bytes = raw.as_encoded_bytes();
        Self::from_bytes(bytes)
    }

    /// Validates raw bearer token bytes.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.is_empty() || bytes.len() as u64 > MO_LIMIT_MCP_TOKEN_BYTES {
            return None;
        }
        if bytes.iter().any(|&b| !(0x21..=0x7e).contains(&b)) {
            return None;
        }
        Some(Self {
            bytes: Zeroizing::new(bytes.to_vec()),
        })
    }

    /// Constant-time comparison against candidate `bytes` (`mo_bearer_check`).
    #[must_use]
    pub fn check(&self, candidate: &[u8]) -> bool {
        let mut diff = self.bytes.len() ^ candidate.len();
        let n = self.bytes.len().max(candidate.len());
        for i in 0..n {
            let a = self.bytes.get(i).copied().unwrap_or(0);
            let b = candidate.get(i).copied().unwrap_or(0);
            diff |= usize::from(a ^ b);
        }
        diff == 0
    }
}
