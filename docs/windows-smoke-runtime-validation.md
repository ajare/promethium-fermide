# Direct smoke runtime validation on Windows (#308)

Build the [MSVC Debug/Release matrix](windows-smoke-build-validation.md) first.
For routine development and final/CI selection, see
[validation lanes](smoke-validation-lanes.md):

```bat
python scripts\validate_ctest_lane.py --build-tree out\msvc-smoke\gui --config Debug --lane fast --parallel 8
python scripts\validate_ctest_lane.py --build-tree out\msvc-smoke\gui --config Release --lane final --parallel 8
```

Run both lanes in both configurations for final evidence. Unfiltered CTest and
`validate_windows_ctest.py` remain exhaustive; registration timeouts preserve
larger MSVC Debug budgets and internal concurrency has CTest processor accounting.

Then run the direct executables, independently of the source/build/startup cwd:

```bat
python scripts\validate_smoke_runtime.py --build-tree out\msvc-smoke\gui
python scripts\validate_smoke_runtime.py --build-tree out\msvc-smoke\nogui
ctest --test-dir out\msvc-smoke\gui -C Debug -R "^smoke-" --output-on-failure
ctest --test-dir out\msvc-smoke\gui -C Release -R "^smoke-" --output-on-failure
```

The runtime validator discovers configured modules through CTest's JSON inventory
and requires every executable product. It validates exact record structure,
no-argument execution, listing, a selected check, unknown arguments/checks and
exit statuses in both configurations. It uses an external cwd and private OS
temporary parent, both containing spaces, with display variables removed and
bounded child timeouts. Logs remain under `<build-tree>/runtime-validation/`.
`--configuration Debug` and repeated `--module <name>` select focused subsets.
The module CMake contracts additionally verify complete check inventories and
all individual selectors against the migration's explicit registries.

Two instances each of tags, Behaviours, Persistence and Editor run concurrently
from the same cwd/temp parent. The validator observes two distinct live roots,
checks each complete output, and requires no remaining root or cwd artifact.
Only Persistence's unavailable POSIX symlink/permission semantics may skip on
Windows; a missing fixture/product is never optional.

## Hardening

- Normalize Windows TEMP to CMake paths before embedding it in generated CTest
  scripts. Native backslashes such as `C:\Users` must not become `\U` escapes.
- Agent Colour backfill now writes under the invocation's atomically reserved
  Context root, not a timestamp-only directory outside the harness. Remove the
  unused Editor copy of that directory helper.
- Explicitly check temporary-root cleanup before SUMMARY and return failure with
  the path and OS error. No retries mask leaked/open handles. The Windows probe
  deliberately retains a non-delete-sharing file handle and verifies this failure
  record and status; the parent cleans its fixture only after child exit.
- Flush execution records before process teardown. Concurrent Windows Metrics
  runs exposed occasional successful exits with lost buffered stdout; explicit
  flush preserves PASS/SUMMARY. No HTTP dependency enters shared support.
- Increase bounded Permissions runtime/contract deadlines for MSVC Debug's
  unchanged workload, including its eight concurrent invocations. No simulation
  assertion or workload is weakened.

## Validation evidence

Windows x64, Visual Studio 18 2026, MSVC v145 (19.51), CMake 4.4.0:

- GUI-on and GUI-off direct-target Debug/Release build matrices.
- All 13 GUI-on modules and 12 GUI-off modules run directly in both configurations
  from an external space-containing cwd, with exact CLI/status/record validation.
- All module and harness CMake contracts, including every selector, required
  missing fixture/product failures, synthetic PASS/FAIL/SKIP/ERROR/SUMMARY,
  continuation, exceptional cleanup, and eight-process filesystem contracts.
- Representative Agent, Behaviours, Persistence, Simulation and harness builds
  and direct runs in Debug and Release from an actual copied project checkout
  and build directory whose names contain spaces. Only dependency sources are
  reused through a directory junction; project source/fixture roots are real
  copies, so CMake's fixture definition exercises the space-containing root.
- Ownership audit still agrees with `smoke-migration-manifest.md`; core support
  remains standard-library/Windows process support only.

This is direct module/harness validation, not the parent ticket's broader
compatibility, GUI or downstream test matrix. Tests create no graphics window,
native dialog or interactive prompt; Startup's existing child intentionally uses
an unavailable SDL video driver. No unrelated vendored GUI tests are launched.
