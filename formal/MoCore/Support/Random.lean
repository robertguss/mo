/- xoshiro256** 1.0 and SplitMix64, after Blackman and Vigna's public-domain
   reference code at https://prng.di.unimi.it/. Word arithmetic wraps at 64 bits. -/
namespace MoCore

structure RandomState where
  s0 : UInt64
  s1 : UInt64
  s2 : UInt64
  s3 : UInt64
  deriving Repr, Inhabited, BEq

def splitMix64 (seed : UInt64) : UInt64 × UInt64 :=
  let seed := seed + 0x9e3779b97f4a7c15
  let z := (seed ^^^ (seed >>> 30)) * 0xbf58476d1ce4e5b9
  let z := (z ^^^ (z >>> 27)) * 0x94d049bb133111eb
  (z ^^^ (z >>> 31), seed)

/-- Four consecutive SplitMix64 outputs, starting with seed plus its increment.
    Negative seeds are interpreted modulo 2^64. No bytes or host endian enter. -/
def RandomState.seeded (n : Int) : RandomState :=
  let seed := UInt64.ofNat (n % 18446744073709551616).toNat
  let (a, seed) := splitMix64 seed
  let (b, seed) := splitMix64 seed
  let (c, seed) := splitMix64 seed
  let (d, _) := splitMix64 seed
  ⟨a, b, c, d⟩

def rotl64 (x : UInt64) (k : UInt64) : UInt64 :=
  (x <<< k) ||| (x >>> (64 - k))

def RandomState.next (s : RandomState) : UInt64 × RandomState :=
  let result := rotl64 (s.s1 * 5) 7 * 9
  let t := s.s1 <<< 17
  let c := s.s2 ^^^ s.s0
  let d := s.s3 ^^^ s.s1
  (result, ⟨s.s0 ^^^ d, s.s1 ^^^ c, c ^^^ t, rotl64 d 45⟩)

/-- Lemire's multiply-high rejection. Nat multiplication retains all 128 bits.
    A whole xoshiro period bounds the search, rather than a retry heuristic.
    The all-zero state is rejected by the caller. The reference period of a
    nonzero state guarantees an accepted draw; functional correctness/period
    is trusted, not a theorem of Core's type-safety development. -/
def RandomState.bounded : Nat → Nat → RandomState → Option (Nat × RandomState)
  | 0, _, _ => none
  | fuel + 1, span, s =>
    let (x, s') := s.next
    let product := x.toNat * span
    if product % 18446744073709551616 < (18446744073709551616 - span) % span then
      s'.bounded fuel span
    else some (product / 18446744073709551616, s')

end MoCore
