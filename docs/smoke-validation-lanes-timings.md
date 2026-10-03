# #331 Linux timing evidence

Measured 3 October 2026 on the same shared developer machine: Linux
7.0.0-34-generic x86-64, AMD Ryzen AI MAX+ 395 (32 logical CPUs), GCC 15.2.0.
Base revision: `1a833f2ae331ce334f71dfa31417a631d4e4ba3b`, plus the uncommitted
#331 validation-only patch. Production code and scenario assertions are unchanged.
GUI-enabled Release (`build-linux`, high analysis off) and Debug
(`build-linux-validation/debug`, high analysis on) were incrementally rebuilt
before these samples. Build time is excluded. Display variables were unset.

All samples used outer **`-j 8`**, one sample per command, with Release then Debug
run sequentially. Other worktree build/test activity was observed on this shared
machine; these are wall-clock observations, not controlled statistical benchmarks
or portable thresholds. Do not infer a production-performance change.

## Comparable primary workload

Scope: Simulation, Permissions, Routing, Transports, plus headless tools.
The four module registries still contain **124, 116, 72, 53 checks** respectively:
**365 functional checks**, all executed once by direct module tests in fast
selection. Bounded CLI/isolation selections add representative executions; no
functional assertion is removed. The expensive standalone tool integrations and
all exhaustive per-check/output/concurrent-full-module assertions remain final.

| Configuration | Before-equivalent routine/full (9 tests) | After fast (13 tests) | After exhaustive (18 tests) | Routine reduction |
| --- | ---: | ---: | ---: | ---: |
| Release | 34.99 s | 9.53 s | 98.47 s | 72.8% |
| Debug | 164.45 s | 43.18 s | 557.31 s | 73.7% |

Every command passed. **Exhaustive elapsed time increased**, intentionally not
hidden: the old outer eight-job run could multiply four independent eight-child
contracts into about 32 children, besides other work. New `PROCESSORS=8` accounting
serializes those contracts at an outer eight-job budget. Final also includes the
new bounded CLI/isolation assertions. This trades wall time for an honest bounded
CPU budget; it is not a claim that stress became faster.

### Commands and baseline reconstruction

The before-equivalent inventory was reconstructed from CTest's public JSON command
arrays: retain only the four original `smoke-<module>` and historical
`smoke-<module>-contract` entries plus `headless-tools-contract`, copy original
configuration-specific timeouts, and use the original build tree as cwd. Omit new
lanes and new `PROCESSORS` accounting. The contracts' no-`LANE` paths execute the
unchanged exhaustive code; the tool contract without `--cli` retains every
original assertion. Both sides use the same rebuilt scenario binaries. This
avoids comparing stale executables or a different production revision.

The first attempted baseline exposed a stale Simulation executable and was
**discarded**; its failing duration is not in the table.

To recreate the baseline inventory, pass an absolute or repository-relative build
path, configuration and destination directory to this Python snippet:

```sh
python3 - build-linux Release /tmp/pf-331-evidence/baseline-Release <<'PY'
import json, pathlib, re, subprocess, sys
build, config, destination = sys.argv[1:]
build = pathlib.Path(build).resolve()
destination = pathlib.Path(destination)
destination.mkdir(parents=True, exist_ok=True)
inventory = json.loads(subprocess.check_output(
    ['ctest', '--test-dir', str(build), '-C', config, '--show-only=json-v1'], text=True))
selected = [t for t in inventory['tests'] if re.fullmatch(
    r'(smoke-(simulation|permissions|routing|transports)(-contract)?|headless-tools-contract)', t['name'])]
assert len(selected) == 9
quote = lambda value: '[==[' + str(value) + ']==]'
lines = []
for test in selected:
    timeout = next(p['value'] for p in test['properties'] if p['name'] == 'TIMEOUT')
    lines.append('add_test(' + test['name'] + ' ' + ' '.join(map(quote, test['command'])) + ')')
    lines.append('set_tests_properties(' + test['name'] + ' PROPERTIES TIMEOUT '
                 + str(timeout) + ' WORKING_DIRECTORY ' + quote(build) + ')')
(destination / 'CTestTestfile.cmake').write_text('\n'.join(lines) + '\n')
PY
```

Use `build-linux-validation/debug Debug /tmp/pf-331-evidence/baseline-Debug`
for the Debug baseline. Actual before commands:

```sh
ctest --test-dir /tmp/pf-331-evidence/baseline-Release -j 8 --output-on-failure
ctest --test-dir /tmp/pf-331-evidence/baseline-Debug -j 8 --output-on-failure
```

For each `BUILD`/`CONFIG` pair (`build-linux`/`Release`,
`build-linux-validation/debug`/`Debug`), after commands were:

```sh
ctest --test-dir "$BUILD" -C "$CONFIG" -j 8 --output-on-failure \
  -L '^validation-fast$' \
  -R '^(smoke-(simulation|permissions|routing|transports)($|-)|headless-tools-cli-contract$)'
ctest --test-dir "$BUILD" -C "$CONFIG" -j 8 --output-on-failure \
  -R '^(smoke-(simulation|permissions|routing|transports)($|-)|headless-tools(-cli)?-contract$)'
```

The fixed positive name patterns above define the benchmark scope, not a
recommended development exclusion. Use the public [lane selectors](smoke-validation-lanes.md).

## Complete post-change validation

| Configuration | Fast project lane (52 tests) | Unfiltered final inventory (109 tests) |
| --- | ---: | ---: |
| Release | 31.22 s | 119.08 s |
| Debug | 43.33 s | 575.55 s |

Commands, both configurations, all eight outer jobs:

```sh
python3 scripts/validate_ctest_lane.py --build-tree "$BUILD" --config "$CONFIG" --lane fast --parallel 8
python3 scripts/validate_ctest_lane.py --build-tree "$BUILD" --config "$CONFIG" --lane final --parallel 8
```

Both fast runs passed all 52 tests. Both unfiltered final runs completed the
109-test inventory with zero failures and the same single expected optional
`willpower_resource_manager_gui_smoke` skip (`x11 not available` without a display).

Fast contains every project functional owner and bounded contract, not vendored
dependency tests. Final contains the complete configured inventory, including
all historical exhaustive contracts and dependency tests. The synthetic Startup
failure contract deliberately exercises a roughly 30-second timeout; it remains
in fast coverage because its failure assertions are unique, not full-module
repetition. This explains why the complete Release fast lane exceeds the primary
workload's 9.53 seconds.

## Additional verification

- Ownership audit: **205 modular sources and four standalone sources**, exactly
  one owner each, unchanged module source/link dependency lists.
- New CLI/isolation contracts passed for all 11 split modules. Inventory/selector
  regression tests check labels, processor accounting, functional ownership,
  selector CLI misuse/help, cleanup and failure reporting.
- Automated concurrent mutant children overwrite a shared fixed artifact and
  falsely report success. Isolation rejects their duplicate reported roots.
- A separate native mutation experiment compiled the actual `Smoke.cpp` and
  `Probe.cpp` with Context's random candidate replaced by a fixed
  `pf-smoke-deliberate-fixed-root`. Running Routing's representative concurrency
  contract with that probe **failed as required**: `Cannot create a unique smoke
  temporary root`, exit 1. No production source was changed for this experiment.
- A separate `BUILD_TESTING=OFF`, `PF_BUILD_GUI=OFF` configuration and direct
  harness-probe build succeeded without building core/editor/render or another
  module. Existing module dependencies are unchanged.
- The Linux incremental helper's real `--lane fast routing simulation` invocation
  passed all six selected tests; focused ownership/selector checks passed again
  in both Debug and Release after final reconfiguration.
- Windows validator Python regressions passed (10 cases). Shell syntax, Python
  compilation and `git diff --check` passed. **Native MSVC execution was not
  available and is not claimed.** Active Windows commands preserve explicit
  Debug/Release selection, platform-specific bounded timeouts and full coverage.

Raw local command/output evidence was recorded under `/tmp/pf-331-evidence/`
(`results.json`, configuration inventories, command logs, native mutation log).
