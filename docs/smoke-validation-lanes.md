# Smoke validation lanes (#331)

Unfiltered CTest still runs **all** coverage. Development selection is explicit;
there is no default exclusion and no scenario assertion was removed.

## Commands (Linux and Windows/MSVC)

Build the default inventory first (or use the Linux incremental helper below).
The portable selector always specifies the configuration, including on a
multi-configuration MSVC tree:

```sh
cmake --build build-linux --config Release --parallel 4
python3 scripts/validate_ctest_lane.py --build-tree build-linux --config Release --lane fast --parallel 8
python3 scripts/validate_ctest_lane.py --build-tree build-linux --config Release --lane final --parallel 8
```

On Windows use `python` and the MSVC build-tree path. Run both commands with
`--config Debug` too. `--help` describes the expensive final lane; `--list` lists
exactly the selected inventory without running it. `final` is unfiltered CTest,
including dependency tests. The existing Windows complete-inventory validator
remains exhaustive in both Debug and Release and audits the new functional labels.

Equivalent CTest selections, with **no exclusion regex**:

```sh
ctest --test-dir build-linux -C Release -L '^validation-fast$' -j 8 --output-on-failure
ctest --test-dir build-linux -C Release -j 8 --output-on-failure
ctest --test-dir build-linux -C Release -L '^validation-stress$' -N
```

Each component lane is also separately selectable (replace `Release` with `Debug`):

```sh
ctest --test-dir build-linux -C Release -L '^functional$' -j 8 --output-on-failure
ctest --test-dir build-linux -C Release -L '^validation-cli$' -j 8 --output-on-failure
ctest --test-dir build-linux -C Release -L '^validation-concurrency$' -j 8 --output-on-failure
ctest --test-dir build-linux -C Release -L '^validation-stress$' -j 8 --output-on-failure
```

For incremental Linux development:

```sh
PF_VALIDATION_JOBS=8 scripts/validate_linux_smoke.sh --config Release --lane fast routing simulation
PF_VALIDATION_JOBS=8 scripts/validate_linux_smoke.sh --config Debug --lane final all
```

The helper builds the selected modules **and the independent harness probe**,
then selects their functional and contract entries. `--lane final all` covers
all module contracts, not the entire configured project/dependency inventory;
use the portable `--lane final` selector for final/CI validation.

## Ownership and registration

| Coverage | CTest entries | Label | Work |
| --- | --- | --- | --- |
| Functional | `smoke-<module>` | `functional`, `validation-fast` | Each module registry once, unchanged assertions |
| CLI | `smoke-<module>-cli-contract` | `validation-cli`, `validation-fast` | Exact listing, one representative selection/report, all misuse forms, arbitrary cwd, cleanup |
| Isolation | `smoke-<module>-concurrency-contract` | `validation-concurrency`, `validation-fast` | Four concurrent children with shared cwd/temp parent; real artifacts and cleanup |
| Exhaustive | Historical `smoke-<module>-contract` | `validation-stress` | Unchanged full output, every check selection, eight concurrent full modules where previously present |

Inventories remain in the existing CMake contract scripts. Both new lanes consume
those same lists, rather than adding a second registration list. The module-local
runner remains the execution authority. `smoke-lanes-contract` audits actual CTest
registration, selectors, exactly-once functional selection, scheduler accounting,
cleanup/failure reporting, and rejects deliberately colliding concurrent children
that overwrite a fixed artifact but claim successful execution. The ownership
manifest/audit continues to count scenario sources, not contract invocations.

CTest `PROCESSORS=8` accounts for internal exhaustive fan-out; isolation entries
use `PROCESSORS=4`. Agent/World exhaustive contracts have no internal fan-out and
use one processor. Metrics' existing eight-child contract is also accounted as
eight processors. A `-j 8` outer run therefore does not multiply multiple
unaccounted eight-child workloads. Individual registrations preserve their
bounded platform/configuration timeouts, including the larger MSVC Debug budgets;
the portable selector does not override them with a global smaller timeout.

## Representative artifact risk audit

`smoke_lane.py` centralizes representative selections; its comments and this table
explain the risk. Every isolation entry additionally runs the real harness `paths`
probe concurrently: artifact creation, nested roots, exception unwinding, distinct
reported roots, and removal. Children share a private empty OS temporary parent
(`TMPDIR`, `TMP`, `TEMP`) as well as an empty cwd containing spaces. Both must be
empty afterwards. This checks actual file operations, not just a trivial success.

| Module | Representative check(s) | Collision risk |
| --- | --- | --- |
| Simulation | `pauseOnStaircasePreservesPosition` | Required repository fixture lookup outside repository; scenarios are in-memory, artifact ownership exercised by probe |
| Permissions | `authorizationAndPersistence` | In-memory authorization/serialization; artifact ownership exercised by probe |
| Routing | `savedIntentWaitsForRegistryResolution` | Writes adjacent World/tag documents beneath Context root |
| Transports | `luaRandomnessIsIndependent` | Writes behaviour package/manifest beneath Context root |
| Behaviours | `savedWorldCreatesAndReopensAdjacentPackage` | Managed adjacent package creation/save/reopen |
| Agent tags | `saveReopenAndUnknownTagValidationUseStableIds` | Registry/World save and reload |
| Editor | `tags/savedWorldCreatesAndReopensAdjacentRegistry` | Editor-managed registry/World creation |
| Persistence | `yaml-file`, `transactional-concurrent` | Serializer file writes and concurrent transactional saves |
| Agent | `displayColourIsRandomAtCreationBackfilledAndPersisted` | Writes legacy colour-backfill tag registry beneath Context root |
| World, Render | `marker-identity`, `theRenderSnapshotIsDeterministic` | In-memory checks; shared artifact/root implementation exercised by probe |

Representative checks are **not** substitutes for exhaustive assertions. The
historical contracts and concurrent full workloads remain registered unchanged.

## Remaining-contract audit

- Agent, World: full registry plus every selected check repeated; split too even
  though small and lacking concurrent full workloads.
- Tags, Behaviours, Editor, Persistence, Render: same exhaustive repetition/fan-out;
  split using the same mechanism, preserving platform-specific skip assertions.
- Metrics: only one check, ephemeral HTTP port and eight concurrent processes;
  deliberately unchanged because it is already bounded, relevant isolation work.
- Startup: only one real graphics-initialization child check; deliberately unchanged
  and still serialized to protect the application's shared startup log. Synthetic
  Startup failure tests remain unchanged.
- Harness: synthetic failure/skip/exception/cleanup assertions, not expensive domain
  repetition; unchanged and retained in fast selection.
- Headless tools: largely unique expensive workloads, rather than generic runner
  repetition. The historical contract remains exhaustive with all generator,
  restoration-cycle (2/default 5), file-backed Lift/pause, overwrite/immutability,
  HTTP and shutdown assertions. A bounded fast CLI variant retains all argument,
  missing/malformed document, generator failure, HTTP/status/bind/shutdown checks,
  omitting only that expensive file-backed functional block. It is explicitly
  labelled stress-only rather than pretending the fast lane replaces it.
- Compatibility, ownership, generator CLI, standalone regressions and remaining
  project tests are unchanged and included in fast selection. Vendored dependency
  tests are intentionally final-only; they are not smoke module scenario owners.

## Timing evidence

See [#331 measurements](smoke-validation-lanes-timings.md). Timings are local
observations, not portable limits. Native Windows execution must be performed on
Windows; Linux runs and Python validator regressions do not claim MSVC validation.
