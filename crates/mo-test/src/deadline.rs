//! Monotonic wall-clock deadlines for `mo test` (`src/tools/deadline.{h,c}`).

use std::cell::RefCell;
use std::time::Instant;

/// How many VM instructions between monotonic clock polls (`MO_DEADLINE_POLL_STEPS`).
pub const DEADLINE_POLL_STEPS: u32 = 4096;

/// Clock mode used by [`Deadline`].
///
/// [`ClockMode::System`] uses [`std::time::Instant`]. The other variants mirror
/// `tests/test_deadline.c` so black-box deadline CLI fixtures run without `unsafe`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ClockMode {
    /// Real system monotonic clock.
    #[default]
    System,
    /// Fixed at `17.000_000_000` seconds (0 ns step per read).
    Fixed,
    /// Starts at `17.0s` and advances by `+1_000_000 ns` (1 ms) on each read.
    Expiry,
    /// Starts at `17.0s`, advances by `+1_000_000 ns`, and fails with EIO once `sec >= 18`.
    Failure,
    /// Fails immediately with EIO on every read.
    StartFailure,
    /// Starts at `17.0s` and advances by `jump_secs` whole seconds per read (unit test mode).
    JumpSeconds(u64),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeadlineState {
    Running,
    Expired,
    Error,
}

#[derive(Debug)]
struct ClockState {
    mode: ClockMode,
    anchor: Option<Instant>,
    sec: u64,
    nsec: u64,
    unavailable: bool,
    step_ns: u64,
    fail_at_sec: Option<u64>,
    jump_secs: u64,
}

impl ClockState {
    fn new(mode: ClockMode) -> Self {
        match mode {
            ClockMode::System => Self {
                mode,
                anchor: Some(Instant::now()),
                sec: 0,
                nsec: 0,
                unavailable: false,
                step_ns: 0,
                fail_at_sec: None,
                jump_secs: 0,
            },
            ClockMode::Fixed => Self {
                mode,
                anchor: None,
                sec: 17,
                nsec: 0,
                unavailable: false,
                step_ns: 0,
                fail_at_sec: None,
                jump_secs: 0,
            },
            ClockMode::Expiry => Self {
                mode,
                anchor: None,
                sec: 17,
                nsec: 0,
                unavailable: false,
                step_ns: 1_000_000,
                fail_at_sec: None,
                jump_secs: 0,
            },
            ClockMode::Failure => Self {
                mode,
                anchor: None,
                sec: 17,
                nsec: 0,
                unavailable: false,
                step_ns: 1_000_000,
                fail_at_sec: Some(18),
                jump_secs: 0,
            },
            ClockMode::StartFailure => Self {
                mode,
                anchor: None,
                sec: 17,
                nsec: 0,
                unavailable: true,
                step_ns: 1_000_000,
                fail_at_sec: None,
                jump_secs: 0,
            },
            ClockMode::JumpSeconds(jump) => Self {
                mode,
                anchor: None,
                sec: 17,
                nsec: 0,
                unavailable: false,
                step_ns: 0,
                fail_at_sec: None,
                jump_secs: jump,
            },
        }
    }

    fn read_ns(&mut self) -> Result<u128, i32> {
        if self.mode == ClockMode::System {
            let anchor = self.anchor.get_or_insert_with(Instant::now);
            return Ok(anchor.elapsed().as_nanos());
        }
        if self.unavailable || self.fail_at_sec.is_some_and(|at| self.sec >= at) {
            return Err(5); // EIO
        }
        let now = u128::from(self.sec) * 1_000_000_000 + u128::from(self.nsec);
        self.sec = self.sec.saturating_add(self.jump_secs);
        self.nsec += self.step_ns;
        if self.nsec >= 1_000_000_000 {
            self.sec = self.sec.saturating_add(self.nsec / 1_000_000_000);
            self.nsec %= 1_000_000_000;
        }
        Ok(now)
    }
}

thread_local! {
    static ACTIVE_CLOCK: RefCell<ClockState> = RefCell::new(ClockState::new(ClockMode::System));
}

/// Installs a deterministic or real clock mode for the current thread (used by `test_deadline`).
pub fn set_thread_clock_mode(mode: ClockMode) {
    ACTIVE_CLOCK.with(|cell| {
        *cell.borrow_mut() = ClockState::new(mode);
    });
}

/// Returns the active clock mode for the current thread so spawned worker threads can inherit it.
#[must_use]
pub fn thread_clock_mode() -> ClockMode {
    ACTIVE_CLOCK.with(|cell| cell.borrow().mode)
}

fn read_monotonic_ns() -> Result<u128, i32> {
    ACTIVE_CLOCK.with(|cell| cell.borrow_mut().read_ns())
}

/// Monotonic wall-clock deadline (`struct mo_deadline`).
#[derive(Debug, Clone)]
pub struct Deadline {
    start_ns: u128,
    limit_ns: u128,
    last_ns: u128,
    steps: u32,
    none: bool,
    pub state: DeadlineState,
    pub error: i32,
}

impl Deadline {
    /// Creates a non-expiring running deadline (`mo_deadline_none`).
    #[must_use]
    pub const fn none() -> Self {
        Self {
            start_ns: 0,
            limit_ns: 0,
            last_ns: 0,
            steps: 0,
            none: true,
            state: DeadlineState::Running,
            error: 0,
        }
    }

    /// Creates a deadline already in the [`DeadlineState::Error`] state with `errno`.
    #[must_use]
    pub const fn with_error(error: i32) -> Self {
        Self {
            start_ns: 0,
            limit_ns: 0,
            last_ns: 0,
            steps: 0,
            none: false,
            state: DeadlineState::Error,
            error,
        }
    }

    /// Starts a deadline for `seconds` seconds (0 means no expiration limit, only elapsed timing).
    pub fn start(seconds: u32) -> Result<Self, i32> {
        let now = read_monotonic_ns()?;
        Ok(Self {
            start_ns: now,
            limit_ns: u128::from(seconds) * 1_000_000_000,
            last_ns: now,
            steps: 0,
            none: false,
            state: DeadlineState::Running,
            error: 0,
        })
    }

    /// Starts a deadline with an explicit `microseconds` limit.
    pub fn start_microseconds(microseconds: u64) -> Result<Self, i32> {
        let now = read_monotonic_ns()?;
        Ok(Self {
            start_ns: now,
            limit_ns: u128::from(microseconds) * 1_000,
            last_ns: now,
            steps: 0,
            none: false,
            state: DeadlineState::Running,
            error: 0,
        })
    }

    /// Reads the monotonic clock and updates `state` (`mo_deadline_running`).
    pub fn running(&mut self) -> bool {
        if self.none {
            return true;
        }
        if self.state != DeadlineState::Running {
            return false;
        }
        let now = match read_monotonic_ns() {
            Ok(ns) => ns,
            Err(err) => {
                self.state = DeadlineState::Error;
                self.error = err;
                return false;
            }
        };
        if now >= self.last_ns {
            self.last_ns = now;
        }
        let elapsed = self.last_ns.saturating_sub(self.start_ns);
        if self.limit_ns != 0 && elapsed >= self.limit_ns {
            self.state = DeadlineState::Expired;
            return false;
        }
        true
    }

    /// Periodic VM callback matching `mo_deadline_keep_running` (checks every 4096 instructions).
    pub fn keep_running(&mut self) -> bool {
        if self.state != DeadlineState::Running {
            return false;
        }
        self.steps = self.steps.wrapping_add(1);
        if (self.steps & (DEADLINE_POLL_STEPS - 1)) != 0 {
            return true;
        }
        self.running()
    }

    /// Elapsed microseconds as of the last clock read (`mo_deadline_microseconds`).
    #[must_use]
    pub fn microseconds(&self) -> u64 {
        let elapsed_ns = self.last_ns.saturating_sub(self.start_ns);
        u64::try_from(elapsed_ns / 1_000).unwrap_or(u64::MAX)
    }

    /// Samples the clock once (if still running) and returns elapsed microseconds.
    pub fn elapsed_microseconds(&mut self) -> u64 {
        let _ = self.running();
        self.microseconds()
    }
}
