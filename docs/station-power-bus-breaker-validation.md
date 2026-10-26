# #884 validation

Linux, Release, GUI-enabled `build-linux`; all checks headless and CTest runs
use `-j 16`. No unmodified submodule tests were run.

- Incremental default build (including editor, core, headless modules and compile
  contracts): passed on the final source state.
- Final affected integration selection: **150/150 passed**, covering breakers,
  bus dispatch, Transfer/Shared, batteries, generators, fuel cells, heat engines,
  protected services, Monitors/Alerts, physical connections, editor coverage and
  smoke ownership. After the final marker press-state cleanup, the seven relevant
  breaker/diagram/inspector checks passed again.
- `git diff --check`: passed. No repository-wide source formatter is configured.
- The full project inventory was exercised with
  `ctest --test-dir build-linux -C Release -E '^willpower_' -j 16 --output-on-failure`:
  **769 passed, 29 failed (798 tests)**. This is not a green full-suite result.

## Full-suite limitations

An isolated Release build of unchanged baseline `067dfa1a` reproduced all eleven
non-timeout failures:

- `station-component-help-extraction`: extractor expects 37 components, docs have
  38; `station-editor-component-help`: missing Power bus help resource.
- `station-growablePorts`, `station-completeThermalChain`,
  `station-powerChainSeam`, `station-thermalReservoirSeam`,
  `station-radiatorSeam`, `station-chillerSeam`, `station-powerChainFiles`:
  legacy pre-bus connector/projection expectations.
- `smoke-editor-contract`, `smoke-editor-cli-contract`: pre-existing expected
  editor inventory mismatch (including thermal-body, Transfer and Monitor-family
  checks). The new breaker check is registered in the matching production order.

The other eighteen failures were existing per-test timeout limits under the full
parallel workload. Focused `-j 16` recovery of `station-referenceNominal`,
`station-timestep-invariance-profiles`, `station-coupledStress`,
`station-stabilityLongRunFast`, and `station-stabilityMultiWorldIsolationFast`
passed; matched baseline runs took essentially the same time (e.g. reference
nominal 61.79 s vs baseline 62.18 s). The remaining timed-out long scenarios,
stability checks and editor aggregate were not claimed as passing.

The unrelated baseline failures and timeout budgets were left unchanged rather
than expanding #884 into parent/follow-up or test-maintenance work.
