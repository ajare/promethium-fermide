# #885 validation

Linux, Release, GUI-enabled incremental `build-linux`. All CTest runs use `-j 16`
and unset `DISPLAY`/`WAYLAND_DISPLAY`; editor checks use CPU ImGui with file I/O
disabled. No Debug or unmodified submodule tests were run, per repository policy.
No repository-wide source formatter is configured; `git diff --check` passes.

## Coverage

- Default Release build: passed (core, editor, headless modules, compile contracts).
- Final affected integration selection: **178/178 passed**, including bus model,
  dispatch, breakers, Transfer/Shared, batteries, generators, fuel cells, heat
  engines, physical connections, protected services, Monitors/Alerts, editor
  coverage and smoke ownership. It also recovers `referenceNominal`,
  `stabilityLongRunFast` and `stabilityMultiWorldIsolationFast` from full-lane
  timeouts; reference nominal completed in 66.30 seconds.
- `busDispatchBalance` checks feeder identity before the first tick, zero initial
  flow, deterministic replay, source delivery versus load draw at the correct
  ends of lossy cables, actual Transfer/Shared serving paths, and event-split
  reporting. `busDispatchBatteries` checks charge/discharge feeder directions;
  `busBreakers` checks tripped connection telemetry and zero dead-bus flow.
- `station/busBreakerWorkflow` exercises the real station window: multiple bus
  rows, rated/unrated load fraction, closed/tripped state, preserved totals,
  snapshot/history non-mutation, and the existing separate breaker reset surface.

Final integration command:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build-linux -C Release \
  -R '^(station-(.*([Bb]us|[Tt]ransfer|[Ss]hared|[Bb]atter|[Gg]enerator|[Ff]uelCell|[Hh]eatEngine|[Cc]onnection|[Pp]rotect|[Mm]onitor|[Aa]lert).*|referenceNominal|stabilityLongRunFast|stabilityMultiWorldIsolationFast)|smoke-ownership-audit)$' \
  -j 16 --output-on-failure
```

## Full inventory limitations

The final implementation's complete project inventory was exercised with:

```sh
cmake --build build-linux --config Release -j 16
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build-linux -C Release \
  -E '^willpower_' -j 16 --output-on-failure
```

**767/798 passed; 31 failed**. This is not a green full-suite result.
The assertion failures match the pre-existing limitations documented in
[the breaker validation report](station-power-bus-breaker-validation.md):
component-help extraction expects 37 components rather than 38; Power bus help
is absent; seven legacy pre-bus connector/projection checks fail; the editor
aggregate encounters that same missing help; and editor inventory/CLI contracts
have existing list mismatches. None is changed by this ticket.

The other 19 failures hit existing per-test timeout limits under full parallel
load: long-reference/invariance, reference/scenario, coupled/stability,
thermal-link stress and editor mixed-observation checks. Selected focused
recovery passed as noted above; remaining timed-out checks are not claimed as
passing. These unrelated full-lane failures and timeout budgets remain unchanged.
