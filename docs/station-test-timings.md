# Station suite wall-clock measurements

The 900-case/all-value sweeps recorded below are historical. They were an
unnecessary expansion, not a consequence of the allowed `[1,60]` bounds. The
maintained matrix now uses representative samples: five ratios for analytical
checks and three for component repeats (45 registrations). Historical counts
and timings are retained as records, not current coverage requirements.

## Comparable Station workloads: 60 Hz versus 10 Hz

Three alternating paired Release runs, identical fresh inputs and 4200 World
quanta (~70 seconds), with startup excluded from the advance timer:

| Workload | 60 Hz median | 10 Hz median | Speedup |
|---|---:|---:|---:|
| Biology | 1716.537 ms | 288.445 ms | **5.95×** |
| Bundled reference | 6326.788 ms | 1085.675 ms | **5.83×** |

The [physical-fidelity report](station-physical-fidelity.md) records hardware,
ranges, startup timings, scope and reproduction. These are Station subsystem
measurements, not whole-application FPS or suite-registration comparisons.

## Earlier timestep validation — not a speedup baseline

See [the verification checkpoint](station-timestep-verification.md) for scope and
scoped evidence and remaining work. World is still nominally 60 Hz. The runner now
respects authored settings unless `--step-ratio` is explicitly supplied; fixed
comparisons own their clocks and publish/check effective duration.

- Last broad authored-settings run, excluding separately tested timestep labels:
  **198 passed, 44 failed, 242 entries**, **688.49 s** CTest wall time. Explicit
  ratio-1 reference/stability fixtures make this a mixed-frequency run, not a
  10 Hz Station benchmark. Output: `/tmp/pf-station-authored.log`.
- Subsequent focused runs repaired and passed **19** of those failed entries.
  A final full-suite pass has not been run or claimed.
- **900 parameterized component/clock cases** passed across incremental Release
  runs at ratios 1–60. The initial 600-case run took **15.43 s**; the additional
  300-case run took **6.14 s** with 100 failures, followed by targeted repairs
  and failed-only reruns (99/100, then 1/1). These are not one monolithic green
  suite timing. Logs: `/tmp/pf-station-component-ratios/run.log`,
  `/tmp/pf-station-additional-ratios.log`, and
  `/tmp/pf-station-additional-ratios-rerun.log`.
- All-ratio transfer and analytical profile checks passed; profile coverage
  includes non-overlapping supply/demand. Those fixture results do not establish
  full coupled-physics equivalence.
- Generator timing originally failed at approximately 6 s: 30.3333, 30.0 and
  20.0 kJ at ratios 1, 6 and 60 instead of the analytical 30.4 kJ. After repair,
  all three produce **30.4 kJ**. Expanded timing/funding gates now pass **360
  analytical cases** across all 60 ratios, with assertions inside the Lua mirror.
- The post-generator-repair regression run was **909 passed, 3 failed, 912
  entries**, **23.48 s**. All **900** parameterized cases passed in that run;
  ordinary `power`, `generators` and `protectedServices` still had legacy/other
  failures. Output: `/tmp/pf-generator-final-regressions.log`. Later focused
  migration advanced the power/generator cases to their next failures; no full
  green Station suite is claimed.

All these runs used incremental Release builds, CTest and `-j 16`. No passing
full-suite performance comparison or blanket fidelity certification exists.

## 2026-10-20 — Release, nominal 10 Hz, parallelism 16

- Source: commit `868fe8d8` plus the working-tree changes making new Station settings default to ratio 6.
- Incremental Release build: `pf-smoke-station` and `pf-smoke-editor` completed before timing.
- Coverage: all 246 CTest entries carrying the `station` label in `build-linux`.
- World clock: unchanged at 60 Hz.
- Station runner: every invocation explicitly supplied `--step-ratio 6` (nominal 10 Hz), replacing any pre-existing command-line ratio.
- Cross-step checks explicitly clear the runner override and compare their internal ratio matrix. The default-cadence check likewise exercises the production default. Editor checks use their own authored configurations and saved ratios, not the Station runner override.
- Command: `ctest --test-dir /tmp/pf-station-10hz-ctest -C Release --output-on-failure -j 16`.
- The temporary CTest manifest was derived from `ctest --test-dir build-linux -C Release --show-only=json-v1`, selecting the Station label and retaining timeouts, working directories, environment, processor and serialization properties. Original generated build-tree tests were not modified.
- Started: `2026-10-20T20:04:19Z`.
- Measured process wall-clock time (`time.perf_counter`): **94.8693 seconds** (1 minute 34.87 seconds).
- CTest reported wall-clock time: **94.86 seconds**.
- Result: **175 passed, 71 failed, 246 total**; CTest exit status 8.
- Local full output: `/tmp/pf-station-10hz-ctest/run.log`.
- Local machine-readable timing: `/tmp/pf-station-10hz-ctest/timing.json`.

This is not a passing-suite performance baseline or a valid speedup comparison:
many checks failed early at the coarse cadence, including the long-duration
stability audit, and editor checks also reported failures. No failures were
suppressed or rerun to obtain this measurement.

## 2026-10-20 — after runtime repairs, Release, nominal 10 Hz, parallelism 16

- Source: the same working tree, now including microbial exhaustion and history-cadence precision fixes and four new regression registrations.
- Incremental Release build: `pf-smoke-station` and `pf-smoke-editor` completed before timing.
- Coverage: all **250** Station-labelled CTest entries; none filtered out.
- World clock: **60 Hz**. Every Station runner invocation explicitly uses `--step-ratio 6` (**10 Hz**), including replacing existing ratio arguments. As above, cross-ratio checks retain their internal matrix and editor checks use their own authored settings. Thus the two new registrations named with suffix `-1` also run at ratio 6 in this forced-frequency measurement; their genuine ratio-1 runs are separate regression-validation evidence, not part of this timing.
- Command: `ctest --test-dir /tmp/pf-station-10hz-postfix-ctest -C Release --output-on-failure -j 16`.
- Temporary manifest derived from the current `ctest --show-only=json-v1` output, preserving original labels, timeouts, working directories, environment, processor and serialization properties. Build-tree test definitions remain unchanged.
- Started: `2026-10-20T20:33:57Z`.
- Measured process wall-clock time (`time.perf_counter`): **138.1403 seconds** (**2 minutes 18.14 seconds**).
- CTest reported wall-clock time: **138.13 seconds**.
- Result: **182 passed, 68 failed, 250 total**; CTest exit status **8**.
- Local full output: `/tmp/pf-station-10hz-postfix-ctest/run.log`.
- Local machine-readable timing: `/tmp/pf-station-10hz-postfix-ctest/timing.json`.

The original microbial-condition, stability path-equivalence and stability
reset-isolation checks now pass. There are no newly failing test identities.
The long-run stability check passes its former audit failure and now stops on
the already-diagnosed outdated World-tick/Station-step equality assertion.
All four new regression entries pass. Remaining failures still shorten the
run, so this is **not** a passing-suite performance baseline. No test failures
were suppressed and no retries were included in the measurement.

## 2026-10-20 — after runtime repairs, Release, nominal 30 Hz, parallelism 16

- Incremental Release build: `pf-smoke-station` and `pf-smoke-editor` completed before timing.
- Coverage: all **250** Station-labelled CTest entries; none filtered out.
- World clock: **60 Hz**. Every Station runner invocation explicitly uses `--step-ratio 2` (**30 Hz**), replacing existing ratio arguments. Cross-ratio checks still run their internal matrices; editor checks use their own authored configurations. Ratio-suffixed regression registrations also use the forced ratio 2 for this measurement.
- Command: `ctest --test-dir /tmp/pf-station-30hz-ctest -C Release --output-on-failure -j 16`.
- Temporary manifest derived from the current `ctest --show-only=json-v1` output with original labels, timeouts, working directories, environment, processor and serialization properties preserved. Build-tree test definitions remain unchanged.
- Started: `2026-10-20T20:41:27Z`.
- Measured process wall-clock time (`time.perf_counter`): **366.3412 seconds** (**6 minutes 6.34 seconds**).
- CTest reported wall-clock time: **366.33 seconds**.
- Result: **183 passed, 67 failed, 250 total**; CTest exit status **8**.
- Local full output: `/tmp/pf-station-30hz-ctest/run.log`.
- Local machine-readable timing: `/tmp/pf-station-30hz-ctest/timing.json`.

This remains a failure-shortened run, not a passing-suite performance baseline.
No failures were suppressed and no retries were included in the measurement.
