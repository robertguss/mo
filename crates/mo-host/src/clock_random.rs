use std::cell::{Cell, RefCell};
use std::time::Duration;

use mo_bytecode::Capability;
use mo_def::limits::{MO_LIMIT_SECURE_RANDOM_BYTES, MO_TIME_MAX_US, MO_TIME_MIN_US};
use mo_heap::{Heap, ScratchGuard, Value};
use sha2::{Digest, Sha256};
use zeroize::Zeroizing;

use crate::HostError;

enum ClockState {
    Real,
    Fake(Cell<i64>),
}

struct RandomState {
    words: RefCell<[u64; 4]>,
}

struct SeededSecureRandom {
    seed: u64,
    counter: u64,
}

enum SecureRandomState {
    Real,
    Fake(RefCell<SeededSecureRandom>),
}

fn splitmix64(seed: &mut u64) -> u64 {
    *seed = seed.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut z = *seed;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^ (z >> 31)
}

fn xoshiro_next(words: &mut [u64; 4]) -> u64 {
    let result = words[1].wrapping_mul(5).rotate_left(7).wrapping_mul(9);
    let t = words[1] << 17;
    words[2] ^= words[0];
    words[3] ^= words[1];
    words[1] ^= words[2];
    words[0] ^= words[3];
    words[2] ^= t;
    words[3] = words[3].rotate_left(45);
    result
}

pub(crate) fn make_real_clock(heap: &Heap, ty: u32) -> Result<Value, HostError> {
    Ok(heap.alloc_capability(Capability::Clock, ty, std::rc::Rc::new(ClockState::Real))?)
}

pub(crate) fn make_fixed_clock(heap: &Heap, ty: u32, time_us: i64) -> Result<Value, HostError> {
    Ok(heap.alloc_capability(
        Capability::Clock,
        ty,
        std::rc::Rc::new(ClockState::Fake(Cell::new(time_us))),
    )?)
}

pub(crate) fn clock_now(cap: &Value) -> Result<Value, HostError> {
    let state = cap
        .as_capability()
        .inner
        .downcast_ref::<ClockState>()
        .expect("Clock capability");
    match state {
        ClockState::Fake(cell) => Ok(Value::Time(cell.get())),
        ClockState::Real => {
            let microseconds = realtime_microseconds()?;
            if microseconds < i128::from(MO_TIME_MIN_US)
                || microseconds > i128::from(MO_TIME_MAX_US)
            {
                return Err(HostError::Bug {
                    reason: "time outside the years 1 to 9999",
                    detail: None,
                });
            }
            Ok(Value::Time(microseconds as i64))
        }
    }
}

#[cfg(unix)]
fn realtime_microseconds() -> Result<i128, HostError> {
    let now = rustix::time::clock_gettime(rustix::time::ClockId::Realtime);
    debug_assert!((0..1_000_000_000).contains(&now.tv_nsec));
    Ok(i128::from(now.tv_sec) * 1_000_000 + i128::from(now.tv_nsec / 1_000))
}

#[cfg(not(unix))]
fn realtime_microseconds() -> Result<i128, HostError> {
    use std::time::SystemTime;
    let now = SystemTime::now();
    match now.duration_since(SystemTime::UNIX_EPOCH) {
        Ok(d) => Ok(i128::from(d.as_secs()) * 1_000_000 + i128::from(d.subsec_micros())),
        Err(e) => {
            let d = e.duration();
            Ok(-(i128::from(d.as_secs()) * 1_000_000
                + i128::from(d.subsec_nanos().div_ceil(1_000))))
        }
    }
}

pub(crate) fn clock_sleep(cap: &Value, duration_us: i64) -> Result<Value, HostError> {
    if duration_us < 0 {
        return Err(HostError::Bug {
            reason: "negative sleep duration",
            detail: None,
        });
    }
    let state = cap
        .as_capability()
        .inner
        .downcast_ref::<ClockState>()
        .expect("Clock capability");
    match state {
        ClockState::Fake(cell) => {
            let next = i128::from(cell.get()) + i128::from(duration_us);
            if next < i128::from(MO_TIME_MIN_US) || next > i128::from(MO_TIME_MAX_US) {
                return Err(HostError::Bug {
                    reason: "time outside the years 1 to 9999",
                    detail: None,
                });
            }
            cell.set(next as i64);
            Ok(Value::Unit)
        }
        ClockState::Real => {
            if duration_us > (MO_TIME_MAX_US - MO_TIME_MIN_US) {
                return Err(HostError::Bug {
                    reason: "time outside the years 1 to 9999",
                    detail: None,
                });
            }
            if duration_us > 0 {
                std::thread::sleep(Duration::from_micros(duration_us as u64));
            }
            Ok(Value::Unit)
        }
    }
}

fn new_random_from_seed(heap: &Heap, ty: u32, mut seed: u64) -> Result<Value, HostError> {
    let mut words = [0u64; 4];
    for w in &mut words {
        *w = splitmix64(&mut seed);
    }
    debug_assert!((words[0] | words[1] | words[2] | words[3]) != 0);
    let state = RandomState {
        words: RefCell::new(words),
    };
    Ok(heap.alloc_capability(Capability::Random, ty, std::rc::Rc::new(state))?)
}

pub(crate) fn make_real_random(heap: &Heap, ty: u32) -> Result<Value, HostError> {
    let mut buf = [0u8; 8];
    if getrandom::fill(&mut buf).is_err() {
        return Err(HostError::Internal {
            reason: "internal error: system randomness unavailable",
        });
    }
    new_random_from_seed(heap, ty, u64::from_ne_bytes(buf))
}

pub(crate) fn make_seeded_random(heap: &Heap, ty: u32, seed: i64) -> Result<Value, HostError> {
    new_random_from_seed(heap, ty, seed as u64)
}

pub(crate) fn random_int(cap: &Value, low: i64, high: i64) -> Result<Value, HostError> {
    if high <= low {
        return Err(HostError::Bug {
            reason: "high not greater than low",
            detail: None,
        });
    }
    let state = cap
        .as_capability()
        .inner
        .downcast_ref::<RandomState>()
        .expect("Random capability");
    let mut words = state.words.borrow_mut();
    let initial = *words;
    let width = (high as u64).wrapping_sub(low as u64);
    let threshold = width.wrapping_neg() % width;
    let offset = loop {
        let product = u128::from(xoshiro_next(&mut words)) * u128::from(width);
        if (product as u64) >= threshold {
            break (product >> 64) as u64;
        }
        assert_ne!(*words, initial, "broken xoshiro256** generator");
    };
    let result = i128::from(low) + i128::from(offset);
    debug_assert!(result >= i128::from(low) && result < i128::from(high));
    Ok(Value::Int(result as i64))
}

pub(crate) fn make_real_secure_random(heap: &Heap, ty: u32) -> Result<Value, HostError> {
    Ok(heap.alloc_capability(
        Capability::SecureRandom,
        ty,
        std::rc::Rc::new(SecureRandomState::Real),
    )?)
}

pub(crate) fn make_seeded_secure_random(
    heap: &Heap,
    ty: u32,
    seed: i64,
) -> Result<Value, HostError> {
    Ok(heap.alloc_capability(
        Capability::SecureRandom,
        ty,
        std::rc::Rc::new(SecureRandomState::Fake(RefCell::new(SeededSecureRandom {
            seed: seed as u64,
            counter: 0,
        }))),
    )?)
}

pub(crate) fn secure_random_bytes(
    heap: &Heap,
    cap: &Value,
    count: i64,
) -> Result<Value, HostError> {
    if count < 0 || (count as u64) > MO_LIMIT_SECURE_RANDOM_BYTES {
        return Err(HostError::Bug {
            reason: "count outside 0 to 65536",
            detail: None,
        });
    }
    let length = count as usize;
    if length == 0 {
        return Ok(heap.alloc_bytes(Vec::new())?);
    }

    let mut scratch = ScratchGuard::new(heap.budget());
    scratch.charge(length)?;

    let state = cap
        .as_capability()
        .inner
        .downcast_ref::<SecureRandomState>()
        .expect("SecureRandom capability");

    let mut buffer = Zeroizing::new(vec![0u8; length]);
    match state {
        SecureRandomState::Real => {
            if getrandom::fill(&mut buffer).is_err() {
                return Err(HostError::Internal {
                    reason: "internal error: cryptographic randomness unavailable",
                });
            }
        }
        SecureRandomState::Fake(fake) => {
            let mut st = fake.borrow_mut();
            let mut input = Zeroizing::new([0u8; 24]);
            input[..8].copy_from_slice(b"MO_SRNG\0");
            input[8..16].copy_from_slice(&st.seed.to_le_bytes());
            let mut offset = 0usize;
            while offset < length {
                input[16..24].copy_from_slice(&st.counter.to_le_bytes());
                let block: Zeroizing<[u8; 32]> = Zeroizing::new(Sha256::digest(*input).into());
                st.counter = st.counter.wrapping_add(1);
                let take = (length - offset).min(block.len());
                buffer[offset..offset + take].copy_from_slice(&block[..take]);
                offset += take;
            }
        }
    }

    let out = buffer.as_slice().to_vec();
    drop(buffer);
    drop(scratch);
    Ok(heap.alloc_bytes(out)?)
}
