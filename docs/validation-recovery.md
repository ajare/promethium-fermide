# Bounded validation and interrupted-run recovery (#332)

All supported smoke entry points share `scripts/validation_run.py`:
`validate_linux_smoke.sh`, `validate_windows_smoke.py`,
`validate_windows_ctest.py`, `validate_smoke_runtime.py`, and
`validate_ctest_lane.py`. Python 3.9+ is required; Linux cleanup uses subreaper
adoption and pidfds (Linux 5.3+). Windows uses a kill-on-close Job Object;
workers start suspended and join the job before executing. Failure to establish
ownership fails closed. Use these commands rather than wrapping bare CMake/CTest
in a short timeout and restarting it.

## Four independent timeout layers

| Layer | Default / configuration | Purpose |
| --- | --- | --- |
| Whole validation invocation | `--run-timeout 7200` seconds; Windows build matrix and exhaustive audit: 14400 | Total configure/build/test/audit work, all configurations/GUI modes combined |
| Each configure/build command | `--build-timeout 1800` seconds | Incremental or cold CMake/MSBuild/compiler work; whole-run limit still bounds many individual target builds |
| CTest test | Existing configuration/platform-specific CMake `TIMEOUT` properties | Detect individual hangs; not enlarged by these scripts |
| Nested contracts/direct subprocesses | Existing contract budgets; runtime direct calls: `--subprocess-timeout 300` s, inventory: 30 s | Bound a single nested operation; whole-run timeout also covers it |

Defaults deliberately accommodate Debug and exhaustive work, not just Release
fast checks. #314–#320 full Debug suites took 121–226 s; after #331 processor
accounting, the representative Debug exhaustive workload alone took 557 s
(`smoke-validation-lanes-timings.md`). Cold compilation and the Windows multi-tree,
multi-configuration build/audit cost more than a suite. These are conservative
command ceilings, **not** per-test performance targets. Both budgets accept finite
positive seconds, including fractions for tests. Tune them down/up for the selected
workload/machine; there is no automatic retry. The Windows audit no longer passes
a blanket CTest `--timeout 600`; registration limits remain authoritative.

**Outer coding-agent/tool timeout must exceed `--run-timeout` by at least 60 s**
for cleanup and reporting (e.g. 7260 s for the default single-tree command).
The repository cannot control the harness's timeout. If the harness imposes a
shorter limit, select a smaller bounded workload or an appropriate supported run
budget first; do not repeatedly submit the same job. Cleanup normally takes a
fraction of a second; Linux orphan reaping has a separate maximum 5 s allowance.

## Ownership and evidence

An OS advisory lock in `<build>/.pf-validation/lock` protects the entire canonical
build tree, not just one configuration. Two configs in one MSVC tree cannot race;
different trees can run independently. The Windows matrix holds locks on all its
selected trees. Bare CMake/CTest or unrelated scripts do not participate in this
protocol; never run them concurrently on an owned tree.

`owner.json` names the supervisor, command, process group (Linux), evidence path,
and outcome. Unique `run-*/output.log`, `result.json`, and `phase-*.json` retain
command/budget/status evidence, including build failure, subprocess failure,
timeout, cancellation, and success. Existing Windows per-command logs are also
retained. Live output goes to the printed log path (`tail -f` on Linux), so the
outer command's output stays small. Exit codes: 0 success, 1 validation/ownership
failure, 2 argument misuse, 124 managed timeout, 130 handled cancellation.
Worker-specific nonzero exits (e.g. CTest 8) are preserved. Incomplete JUnit or
missing/empty selections are errors, never successful validation.

SIGINT/SIGTERM on Linux and Ctrl+C/Ctrl+Break on Windows trigger cleanup. Linux kills only
its worker plus adopted descendants (including descendants creating their own
sessions); pidfds and parent identity guard against PID reuse. Recorded group
identities are only probed for stale-run detection, never used to kill processes. Windows closes
only its own Job Object. No compiler/process-name matching or system-wide killing
is used. Normal worker exit also cleans lingering children.

**Uncatchable termination is not equivalent to managed cancellation.** SIGKILL,
Windows TerminateProcess, host crash, and forced harness shutdown can prevent
result reporting. Windows kernel job-handle closure normally cleans children even
then; Linux descendants may survive. No universal cleanup guarantee is claimed.
After an interrupted `running` record (or `cleanup-incomplete` outcome), the next invocation checks the OS lock and
recorded Linux group, refuses if either may still be active, and otherwise refuses
until explicit `--recover-stale` acknowledgement. Inspect the recorded command,
log, supervisor/worker identities and descendants manually before acknowledging
that previous work is gone. Never kill a PID merely because it appears in stale
metadata (PID reuse), delete an active lock, or use broad process-name cleanup.
Even a free lock/absent old group cannot prove there are no detached descendants
after SIGKILL. `--recover-stale` does not override active locks/groups and does not
kill anything. Partial build products are preserved for incremental recovery.

## Initial → focused repair → final workflow

Linux (use the actual same build tree/configuration throughout):

```sh
PF_VALIDATION_JOBS=8 scripts/validate_linux_smoke.sh --config Debug --lane fast all
# Inspect the printed log/evidence and repair the failing source; rebuild affected
# targets with the same helper's --build-only, or run individual --check commands for feedback.
# To rerun the most recent completed failed CTest selection without building:
python3 scripts/validate_ctest_lane.py --build-tree build-linux-validation/debug \
  --config Debug --lane fast --rerun-failed --parallel 8
# Required final coverage on FINAL sources, after affected targets are rebuilt:
PF_VALIDATION_JOBS=8 scripts/validate_linux_smoke.sh --config Debug --lane final --build-only all
python3 scripts/validate_ctest_lane.py --build-tree build-linux-validation/debug \
  --config Debug --lane final --parallel 8
# Repeat the required Release build and final coverage too.
```

`--lane final --build-only all` builds the default inventory without duplicating
smoke tests before the unfiltered final lane (other selections build only their
smoke targets). Build-only success does not claim test completion. The Linux smoke helper's final selection covers selected smoke modules/contracts;
the **unfiltered** final lane additionally covers standalone, dependency, tooling,
and validation contracts. It does not build: ensure the default inventory is built
before running it. The Linux helper also accepts `--rerun-failed` (no build).

Windows:

```powershell
python scripts/validate_windows_smoke.py --build-root out/msvc-smoke --gui both
python scripts/validate_ctest_lane.py --build-tree out/msvc-smoke/gui --config Debug --lane fast
# Repair/rebuild first; then focused feedback, never final approval:
python scripts/validate_ctest_lane.py --build-tree out/msvc-smoke/gui --config Debug --lane fast --rerun-failed
# Finish with the exhaustive existing audit, for each required GUI/no-GUI tree:
python scripts/validate_windows_ctest.py --build-tree out/msvc-smoke/gui --gui on
python scripts/validate_windows_ctest.py --build-tree out/msvc-smoke/nogui --gui off
python scripts/validate_smoke_runtime.py --build-tree out/msvc-smoke/gui
```

Completed CTest runs from Linux, lane selection, or the Windows audit save
`failed-tests.json` with the exact build/configuration and JUnit completion evidence.
Focused reruns use equivalent explicit failed-test-name selection, rather than
trusting CTest's shared `LastTestsFailed.log` (which another configuration can
overwrite). They reject missing/incomplete/empty/wrong-config/changed inventories,
and verify the selected JUnit case set. A managed interruption marks the inventory
incomplete before launching CTest; no stale previous suite may authorize recovery.
A focused success is recorded as `focused-success`, prints that it is **not final
verification**, and cannot be repeatedly treated as failed-test selection. Direct
runtime/build failures have no CTest failure inventory: repair and rerun the
appropriate bounded command. A new failed full suite replaces the inventory.

`python scripts/tests/validation_run.py` runs short synthetic ownership,
managed/nested timeout, cancellation, stale-run, separate-tree concurrency,
unrelated-process protection, Linux SIGKILL, and actual CTest repair/final tests.
It is also registered as `validation-lifecycle-contract` in CTest on both platforms.
Native Windows execution must be performed on Windows; Linux runs do not establish
MSVC/Windows runtime evidence.

## Implementation verification (3 October 2026)

Same GUI-enabled Linux Release/Debug trees used for #331, eight outer jobs,
display variables removed. Final source state was incrementally built with
`--lane final --build-only all`, then tested with the unfiltered final lane:

| Configuration / tree | Final CTest result | CTest wall time | Retained run ID |
| --- | --- | --- | --- |
| Release / `build-linux` | 110 registered, zero failures, one optional GUI skip | 115.73 s | `ae921add5b6a4977843bb90d51937816` |
| Debug / `build-linux-validation/debug` | 110 registered, zero failures, one optional GUI skip | 551.74 s | `888da6db9d6746c2bf3ae19669967ea2` |

Both default builds and supervisors exited 0. The lifecycle contract runs twelve
synthetic/recovery tests on Linux (eleven passed, Windows-only console cancellation
skipped), including deliberately failed/repaired/full CTest cases and intentional
nested/process timeouts. Existing Windows-validator unit tests: ten passed on
Linux. Representative supervised runtime persistence/editor CLI and concurrent-root
checks passed in both configurations. Initial Linux fast smoke helper runs passed
37 selected tests in Release (8.73 s) and Debug (47.15 s); the helper intentionally
selects smoke modules, not the complete fast-lane inventory. These are workload
observations on a shared machine, not portable performance thresholds.

During implementation the new completion guard initially rejected CTest's explicit
optional GUI `SKIP_RETURN_CODE=77` case because JUnit labels it `status="notrun"`.
Those wrapper results were correctly nonzero and are not final approval evidence.
The guard now accepts explicit registered skips while still rejecting unexecuted
non-skipped cases; synthetic skip/completion regression coverage was added before
the successful final runs above. No existing scenario assertions or completion
coverage were removed. Native Windows/MSVC execution was unavailable in this
workspace: Windows Job Object/console tests are implemented and registered, but
no native Windows pass is claimed.
