## HTTPS uptime monitor actor for `mo-pulse`: tracks monitored targets, probe due ticks,
## consecutive failures, and up/down transitions.
module Pinger
expose UptimePinger, TargetHealth(..), register_target, record_probe, all_targets

struct TargetHealth
  name: Text
  url: Text
  up?: Bool
  consecutive_failures: Int
  checks: Int
  probe_due?: Bool
end

struct PingerState
  targets: Map(Text, TargetHealth)
end

actor UptimePinger
  state PingerState = PingerState(targets: {})
  calls register_target, record_probe, all_targets
  every 1.minute mark_probes_due

  promise on record_probe
    after.targets.size == before.targets.size
  end
end

fn register_target(state: PingerState, _now: Time, name: Text, url: Text) : (
  PingerState,
  TargetHealth
)
  t = TargetHealth(
    name: name,
    url: url,
    up?: true,
    consecutive_failures: 0,
    checks: 0,
    probe_due?: true
  )
  (PingerState(targets: state.targets.set(name, t)), t)
end

fn record_probe(state: PingerState, _now: Time, name: Text, healthy?: Bool) : (
  PingerState,
  Option(TargetHealth)
)
  case state.targets.get(name)
    Some(prev):
      fails = if healthy?: 0 else: prev.consecutive_failures.saturating_add(1)
      updated = TargetHealth(
        name: prev.name,
        url: prev.url,
        up?: healthy?,
        consecutive_failures: fails,
        checks: prev.checks.saturating_add(1),
        probe_due?: false
      )
      (PingerState(targets: state.targets.set(name, updated)), Some(updated))
    None: (state, None)
  end
end

fn all_targets(state: PingerState, _now: Time) : List(TargetHealth)
  state.targets.values
end

fn mark_probes_due(state: PingerState, _now: Time) : PingerState
  var next = {}
  for (k, t) in state.targets.entries
    next = next.set(
      k,
      TargetHealth(
        name: t.name,
        url: t.url,
        up?: t.up?,
        consecutive_failures: t.consecutive_failures,
        checks: t.checks,
        probe_due?: true
      )
    )
  end
  PingerState(targets: next)
end
