# U3 grading development — 2026-11-09

Collection remains disabled. U1's trusted serving-model channel and U2's trusted
command-origin adapter are unqualified. No model or scored sessions were launched.
This note does not amend the frozen protocol or published experiment results.

Implemented within U3 ownership:
- Canonical prompt requires the entire final message to be solely the revision
  observed after activation; removed the appended literal-answer instruction.
- Typed completed Codex agent-message / turn events and Claude terminal success
  results supply raw final text. Only surrounding whitespace is stripped by grading.
- Arbitrary recursive model keys no longer receive provenance credit. Raw model
  and fallback claims remain retained, explicitly unqualified.
- Accepted reports carry no origin label. They can support identity diagnostics
  against independent recompilation and exact persisted history, but cannot attest
  a command. State, identity, command origin and final answer are separate dimensions.
- Collector additionally records source-test, traffic, model and publication
  dimensions; success requires their conjunction and no other failures.
- Final answer requires explicit post-activation observation proof. The collector
  currently supplies none: it reports `post-activation revision observation not proven`.
  Hidden operator live observations are never silently promoted to agent observations.
  A future qualified adapter must establish the agent received the revision after
  the accepted activation and before its final answer. No guessed schema is added.
- Publication remains false in the collector until the separate publication workflow
  establishes it. No synthetic control establishes collection readiness.

Red/green evidence (retained below): new harness cases initially produced one
failure and two errors; independent diagnostics initially failed with an unsupported
identity_report argument. The red collection test created an empty `absent` directory;
that test artifact was removed. After repair, 13 focused tests passed.

Commands:
```
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s learnability/phase5/evaluation -p test_harness.py -v
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=learnability/phase5/evaluation python3 -m unittest test_grade.IndependentDiagnostics -v
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=learnability/phase5/evaluation python3 -m unittest test_harness test_grade.IndependentDiagnostics test_grade.Selector -v
MO_EVAL_BINARY=/opt/mo-phase5/bin/mo PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s learnability/phase5/evaluation -p test_grade.py -v
```

Real-service result: 4 tests passed in 103.705 seconds. Both bare and qualified
migration controls passed; all nine negative variants remained negative, including
source-only, initializer, lost/changed jobs, revision and next-id defects. Folder-only
restart, preparation history and stable lease deadline checks passed. These controls
explicitly supply development-reference origin and observation booleans to isolate
state/identity correctness; those inputs are not real client attribution proof.

Protected OS stub success/timeout controls were not run: their existing success
expectations rely on now-rejected model claims and shell-text inference. Updating
that separate file is outside this assignment. The focused launch-failure test
verifies capture/reset. No C build, full CI, provider runs, index writes, commits,
branches or PR operations were performed. Parent owns authoritative verification,
review, publication integration and shipping.

## Retained red and green output

### u3-red.txt

```text
test_credentials_are_environment_references (test_harness.RunnerTests.test_credentials_are_environment_references) ... ok
test_fallback_retained (test_harness.RunnerTests.test_fallback_retained) ... FAIL
test_final_limit_fixed (test_harness.RunnerTests.test_final_limit_fixed) ... ok
test_launch_failure_still_captures_and_resets (test_harness.RunnerTests.test_launch_failure_still_captures_and_resets) ... ok
test_layout_is_distinct (test_harness.RunnerTests.test_layout_is_distinct) ... ok
test_local_imports_are_isolated (test_harness.RunnerTests.test_local_imports_are_isolated) ... ok
test_missing_freeze_refused (test_harness.RunnerTests.test_missing_freeze_refused) ... ok
test_reservation_order_and_no_reruns (test_harness.RunnerTests.test_reservation_order_and_no_reruns) ... ok
test_scored_collection_blocked_before_side_effects (test_harness.RunnerTests.test_scored_collection_blocked_before_side_effects) ... ERROR
test_typed_final_messages (test_harness.RunnerTests.test_typed_final_messages) ... ERROR

======================================================================
ERROR: test_scored_collection_blocked_before_side_effects (test_harness.RunnerTests.test_scored_collection_blocked_before_side_effects)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "/home/exedev/code/mo-m6/learnability/phase5/evaluation/test_harness.py", line 77, in test_scored_collection_blocked_before_side_effects
    h.collect(h.layout(), 'absent', 'absent', 1)
    ~~~~~~~~~^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  File "/home/exedev/code/mo-m6/learnability/phase5/evaluation/harness.py", line 145, in collect
    s = json.loads(Path(spec).read_text())
                   ~~~~~~~~~~~~~~~~~~~~^^
  File "/home/exedev/.local/share/mise/installs/python/3.14.7/lib/python3.14/pathlib/__init__.py", line 787, in read_text
    with self.open(mode='r', encoding=encoding, errors=errors, newline=newline) as f:
         ~~~~~~~~~^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  File "/home/exedev/.local/share/mise/installs/python/3.14.7/lib/python3.14/pathlib/__init__.py", line 771, in open
    return io.open(self, mode, buffering, encoding, errors, newline)
           ~~~~~~~^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
IsADirectoryError: [Errno 21] Is a directory: 'absent'

======================================================================
ERROR: test_typed_final_messages (test_harness.RunnerTests.test_typed_final_messages)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "/home/exedev/code/mo-m6/learnability/phase5/evaluation/test_harness.py", line 73, in test_typed_final_messages
    self.assertEqual(h.final_message(client, path), answer)
                     ^^^^^^^^^^^^^^^
AttributeError: module 'runner' has no attribute 'final_message'

======================================================================
FAIL: test_fallback_retained (test_harness.RunnerTests.test_fallback_retained)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "/home/exedev/code/mo-m6/learnability/phase5/evaluation/test_harness.py", line 60, in test_fallback_retained
    self.assertEqual(record['reported_models'], [])
    ~~~~~~~~~~~~~~~~^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
AssertionError: Lists differ: ['claude-opus-4-8'] != []

First list contains 1 additional elements.
First extra element 0:
'claude-opus-4-8'

- ['claude-opus-4-8']
+ []

----------------------------------------------------------------------
Ran 10 tests in 0.022s

FAILED (failures=1, errors=2)
```

### u3-grade-red.txt

```text
test_missing_origin_does_not_invent_identity_failure (test_grade.IndependentDiagnostics.test_missing_origin_does_not_invent_identity_failure) ... ERROR

======================================================================
ERROR: test_missing_origin_does_not_invent_identity_failure (test_grade.IndependentDiagnostics.test_missing_origin_does_not_invent_identity_failure)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "/home/exedev/code/mo-m6/learnability/phase5/evaluation/test_grade.py", line 31, in test_missing_origin_does_not_invent_identity_failure
    verdict = grade.compare(expected, live, live, history, None,
                            answer='1', identity_report=report)
TypeError: compare() got an unexpected keyword argument 'identity_report'

----------------------------------------------------------------------
Ran 1 test in 0.000s

FAILED (errors=1)
```

### u3-focused-green.txt

```text
test_credentials_are_environment_references (test_harness.RunnerTests.test_credentials_are_environment_references) ... ok
test_fallback_retained (test_harness.RunnerTests.test_fallback_retained) ... ok
test_final_limit_fixed (test_harness.RunnerTests.test_final_limit_fixed) ... ok
test_launch_failure_still_captures_and_resets (test_harness.RunnerTests.test_launch_failure_still_captures_and_resets) ... ok
test_layout_is_distinct (test_harness.RunnerTests.test_layout_is_distinct) ... ok
test_local_imports_are_isolated (test_harness.RunnerTests.test_local_imports_are_isolated) ... ok
test_missing_freeze_refused (test_harness.RunnerTests.test_missing_freeze_refused) ... ok
test_reservation_order_and_no_reruns (test_harness.RunnerTests.test_reservation_order_and_no_reruns) ... ok
test_scored_collection_blocked_before_side_effects (test_harness.RunnerTests.test_scored_collection_blocked_before_side_effects) ... ok
test_typed_final_messages (test_harness.RunnerTests.test_typed_final_messages) ... ok
test_untrusted_model_claims_never_qualify (test_harness.RunnerTests.test_untrusted_model_claims_never_qualify) ... ok
test_missing_origin_does_not_invent_identity_failure (test_grade.IndependentDiagnostics.test_missing_origin_does_not_invent_identity_failure) ... ok
test_declared_function_name (test_grade.Selector.test_declared_function_name) ... ok

----------------------------------------------------------------------
Ran 13 tests in 0.015s

OK
```

### u3-grade-green.txt

```text
test_missing_origin_does_not_invent_identity_failure (test_grade.IndependentDiagnostics.test_missing_origin_does_not_invent_identity_failure) ... ok
test_prepared_lease_ages_only_at_declared_deadline (test_grade.RealControls.test_prepared_lease_ages_only_at_declared_deadline) ... ok
test_real_services (test_grade.RealControls.test_real_services) ... ok
test_declared_function_name (test_grade.Selector.test_declared_function_name) ... ok

----------------------------------------------------------------------
Ran 4 tests in 103.705s

OK
reference {'correct': True, 'reasons': [], 'dimensions': {'task_state': True, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
qualified-reference {'correct': True, 'reasons': [], 'dimensions': {'task_state': True, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
unchanged {'correct': False, 'reasons': ['live: wrong revision', 'live: wrong stored_revision', 'restart: wrong revision', 'restart: wrong stored_revision', 'no successful recorded command-origin push', 'activation identity mismatch'], 'dimensions': {'task_state': False, 'activation_identity': False, 'command_origin': False, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
initializer {'correct': False, 'reasons': ['live: wrong persisted jobs', 'live: wrong jobs', 'live: wrong next_id', 'restart: wrong persisted jobs', 'restart: wrong jobs', 'restart: wrong next_id'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
drop-job {'correct': False, 'reasons': ['live: wrong persisted jobs', 'live: wrong jobs', 'restart: wrong persisted jobs', 'restart: wrong jobs'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
payload {'correct': False, 'reasons': ['live: wrong persisted jobs', 'live: wrong jobs', 'restart: wrong persisted jobs', 'restart: wrong jobs'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
duplicate-completion {'correct': False, 'reasons': ['live: wrong persisted jobs', 'live: wrong jobs', 'restart: wrong persisted jobs', 'restart: wrong jobs'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
wrong-revision {'correct': False, 'reasons': ['live: wrong revision', 'live: wrong stored_revision', 'restart: wrong revision', 'restart: wrong stored_revision'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
source-only {'correct': False, 'reasons': ['live: wrong revision', 'live: wrong stored_revision', 'restart: wrong revision', 'restart: wrong stored_revision', 'no successful recorded command-origin push', 'activation identity mismatch'], 'dimensions': {'task_state': False, 'activation_identity': False, 'command_origin': False, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
constant-revision {'correct': False, 'reasons': ['live: wrong stored_revision', 'restart: wrong stored_revision'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
wrong-next-id {'correct': False, 'reasons': ['live: wrong next_id', 'restart: wrong next_id'], 'dimensions': {'task_state': False, 'activation_identity': True, 'command_origin': True, 'final_answer': True}, 'evidence_class': 'DEVELOPMENT'}
```

## Final restoration control

A direct Python control using `fixture.sources` created an unchanged historical
source and a qualified-reference candidate in a TemporaryDirectory, overwrote
candidate `queue-tests.mo` with `invalid candidate replacement`, and invoked
`grade.source_precheck('/opt/mo-phase5/bin/mo', candidate, root / 'checked', old)`.
Actual result: exit 0; totals errors 0, dropped 0, passed 47, failed 0, stopped 0.
This exercises immutable test restoration and historical type synthesis. The
process finished and its temporary directory was cleaned.

Final focused rerun after adding prose, intermediate-command and terminal-error
extraction cases: 13 tests passed in 0.018 seconds. `git diff --check` passed.
All processes started for this work have completed; no collector/firewall changes
were made. The existing proxy edits were preserved.

```text
test_credentials_are_environment_references (test_harness.RunnerTests.test_credentials_are_environment_references) ... ok
test_fallback_retained (test_harness.RunnerTests.test_fallback_retained) ... ok
test_final_limit_fixed (test_harness.RunnerTests.test_final_limit_fixed) ... ok
test_launch_failure_still_captures_and_resets (test_harness.RunnerTests.test_launch_failure_still_captures_and_resets) ... ok
test_layout_is_distinct (test_harness.RunnerTests.test_layout_is_distinct) ... ok
test_local_imports_are_isolated (test_harness.RunnerTests.test_local_imports_are_isolated) ... ok
test_missing_freeze_refused (test_harness.RunnerTests.test_missing_freeze_refused) ... ok
test_reservation_order_and_no_reruns (test_harness.RunnerTests.test_reservation_order_and_no_reruns) ... ok
test_scored_collection_blocked_before_side_effects (test_harness.RunnerTests.test_scored_collection_blocked_before_side_effects) ... ok
test_typed_final_messages (test_harness.RunnerTests.test_typed_final_messages) ... ok
test_untrusted_model_claims_never_qualify (test_harness.RunnerTests.test_untrusted_model_claims_never_qualify) ... ok
test_missing_origin_does_not_invent_identity_failure (test_grade.IndependentDiagnostics.test_missing_origin_does_not_invent_identity_failure) ... ok
test_declared_function_name (test_grade.Selector.test_declared_function_name) ... ok

----------------------------------------------------------------------
Ran 13 tests in 0.018s

OK
```
