## Alert deduplication actor for `mo-pulse`: suppresses duplicate error fingerprints
## within each cooldown window while tracking occurrence counts.
module Dedup
expose AlertDedup, DedupOutcome(..), observe_fingerprint, suppressed_total

struct DedupState
  counts: Map(Text, Int)
  suppressed: Int
end

struct DedupOutcome
  alert_now?: Bool
  occurrences: Int
end

actor AlertDedup
  state DedupState = DedupState(counts: {}, suppressed: 0)
  calls observe_fingerprint, suppressed_total
  every 5.minutes clear_window
end

fn observe_fingerprint(state: DedupState, _now: Time, fingerprint: Text) : (
  DedupState,
  DedupOutcome
)
  prev = state.counts.get(fingerprint) or 0
  next_count = prev.saturating_add(1)
  first_seen = prev == 0
  next_supp = if first_seen: state.suppressed else: state.suppressed.saturating_add(1)
  next = DedupState(counts: state.counts.set(fingerprint, next_count), suppressed: next_supp)
  (next, DedupOutcome(alert_now?: first_seen, occurrences: next_count))
end

fn suppressed_total(state: DedupState, _now: Time) : Int
  state.suppressed
end

fn clear_window(state: DedupState, _now: Time) : DedupState
  DedupState(counts: {}, suppressed: state.suppressed)
end
