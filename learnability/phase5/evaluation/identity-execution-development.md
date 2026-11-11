# Execution identity development

Status: incomplete; the loader-injection finding remains open. No qualification,
model invocation, scored session, publication, or phase closure occurred.

Prediction before the regression run: injected and unknown environments will
incorrectly receive push credit, because the parser ignores envp and the consumer
checks only executable path and the second argument.

Inspected origin_identity.py, origin_trace.py, origin_observer.py,
test_origin_identity.py, test_origin_observer.py, and test_origin_live.py, plus
the supplied review receipt and relevant approved-plan requirements. The graph
coverage check reported this checkout unindexed; source was read directly.

Baseline command: `python3 -m unittest test_origin_identity test_origin_observer test_origin_trace`:
30 tests passed. After adding one regression method: 31 tests ran, with five
failing subtests (preload, audit, library search path, unknown envp, truncated envp).
The existing 30 tests continued to pass. These are synthetic receipt controls;
no actual preload library or clean push live control was run in this unit.

The first edit command used a duplicated relative path and failed before writing;
the subsequent command corrected the working-directory-relative path.

Production code is unchanged. The new failing regression is deliberately retained.
An environment-only fix would not establish trusted execution: loader and library
inputs, executable mappings, and successful cross-process or proc-memory writes
still need collector-established evidence. The current syscall filter does not
provide that complete boundary. This is an implementation gap, not evidence that
a collector-side solution is impossible. Do not rely on current push credit.

The ordinary-file transfer overrejection and positioned-I/O gap remain separate,
unfixed findings. Credential inventories and scanners were not changed.

No Git/index commands, builds, runtime C edits, full CI, subagents, model APIs, or
scored sessions were used. The focused observer tests completed and waited for
their children, including timeout cleanup. No persistent service or trace run was
started. Firewall and home modes were not changed.

Private red output: `/tmp/mo-identity-execution-wr9wk2af/red.log` (directory 0700, file 0600).
This file contains assertion diagnostics, no environment capture or raw buffers.

## Parent implementation and controls

The preceding incomplete handoff is preserved. The parent implemented full private exec environment capture, typed rejection of unknown/truncated/loader-controlled contexts, bounded ELF64 interpreter parsing and pinning, trusted executable-mapping checks, and rejection of observed ptrace, process_vm_writev and proc-memory writes. Dynamic launches with absent or untrusted executable mappings receive no push credit. This is a bounded Linux ELF64 capture implementation pending independent re-review, not general process sandboxing or model qualification.

The first integrated run retained one failed positive identity test: HTTP sender receipts omitted the newly captured execution context. Adding that context restored the positive; 31 identity/observer/trace tests passed. Additional mapping and memory-mutation negative controls were added after implementation; no red-before-fix claim for those controls. Final focused suite: 69 tests passed in 0.457 seconds; diff check passed.

Actual clean isolated controls passed: curl 1 observation, urllib 2, real push 6 verified requests plus unrelated curl 1, committed ordinal 1. Private evidence /root/mo-origin-live-719_gow_; summary /tmp/phase5-identity-context-live.txt. A new development-only preload fixture was compiled with GCC as a shared library (no Mo runtime C change or toolchain build). Its constructor sent one HTTP request from within the pinned Mo PID with argv push, then exited. That request received zero verified-push credit. Evidence /root/mo-execution-identity-da7oy7vf; summary /tmp/phase5-preload-identity-control.txt; executable control test_execution_identity_live.py. This is an identity negative control, not an accepted-upgrade or model session.

Both session/test accounts have zero processes, OUTPUT remains ACCEPT only, operator home0750; all checks finished. No models, scored sessions, Git mutations, PRs or qualification changes. Full protected collector rerun remains pending; ordinary-file transfer overrejection and positioned-I/O completeness remain open review findings.
