# Recorded migration context probe

Prediction written before running `probe.py`: a migration-bearing candidate
that renames `State.old_field` to `State.new_field` checks with declarations
from the original state, but not with declarations from its own new state.
A later tool-only edit that retains that migration has the same dependency.

```
python3 learnability/phase5/preflight/restart-context/probe.py build/gcc/debug/mo
```

The probe first runs the new typed upgrade check against v1 and v2. It then
models an injected `UpgradeOld` module using temporary files and ordinary
checking, to isolate the type-context question from folder-only
restart implementation. It requires an exact accepted status for the upgrade
check, empty stderr for successful checks, and a field-access diagnostic for
checks using the wrong generation. It does not claim that ordinary checking
proves migration execution or activation correctness.

The stored format retains the original candidate bundle, two previous bundles,
and two previous snapshots. After more tool-only activations, both retained
snapshots can describe the new field while the active source still references
the original field in its migration body. The latest snapshot's schema alone
therefore cannot recreate that source's compile context. Recompiling older
original bundles can in turn require their own `UpgradeOld` contexts.

This is a missing rule for deterministic compilation of recorded bundles,
not evidence that every possible implementation must keep an unbounded chain.
Concrete options are:

- Persist a bounded, canonical generated declaration context with each program,
  including programs retained for failures. This extends the approved fixed
  store format, or extends the bundle format with a separately authenticated
  context section. Original user source identity should remain distinct from
  generated context identity. Size limits, verification, retention and restart
  rules need to cover it.
- Separate migration source from the durable service compilation unit. Define
  which migration functions, helpers, imports and nominal types are removed,
  and refuse persistent new-state types that depend on the removed namespace.
  Removing only the named function does not cover helpers or migration-only
  types; arbitrary source filtering needs explicit semantics and tests.
- Retain and pin every required old context. This needs an admission bound and
  a retention rule beyond two snapshots/bundles, and can refuse further upgrades
  once the bound is reached. Merely following original-bundle ancestry is not
  a bounded implementation.

The approved resolution persists a bounded, validated compilation context with
its own digest, separately from the original source digest. The M2 integration
driver `ci/check_upgrades.py` now runs the rename migration, three further
tool-only activations (evicting the original program), and folder-only restart.
It verifies the migrated value and retained context. `ci/check_upgrade_fallback.py`
also covers cached-declaration and saved-schema fallback after a simulated future
compiler rejects an old body, and refusal with a nonempty unreplayable tail.
The original failed assumption above remains the reason for this store extension.
