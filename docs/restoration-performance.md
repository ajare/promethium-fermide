# Reset and document reload profiling (#231)

## Reproduce

Use an optimized build for timings (not Debug):

```text
cmake --build build-windows --config Release --target pf-generate-routing-world pf-restoration-benchmark editor --parallel
pf-generate-routing-world.exe routing-scale.world.yaml
```

Keep the adjacent tag registry. Generation, which includes routing and an export
round trip, is **not** a reload/reset measurement. Do not regenerate the fixture
between comparison runs. The benchmark does not save or modify its input files.

In PowerShell, with the executable's directory on PATH:

```powershell
$env:PF_RESTORATION_TIMING = '1'
pf-restoration-benchmark.exe routing-scale.world.yaml 2>&1 |
    Tee-Object restoration.log
Remove-Item Env:PF_RESTORATION_TIMING
```

The command performs five load/run/reset/run/release cycles. An optional final
argument selects 1–1,000 cycles (for example, `pf-restoration-benchmark
routing-scale.world.yaml 20`). `restoration-cycle`
reports end-to-end latency, current process working set and lifetime peak working
set (peak currently available on Windows; zero means unavailable elsewhere).
Correctness checks and simulation stepping are outside the latency timers, not
restoration work. Fresh processes distinguish first-load registry I/O from later
loads that may reuse the existing registry-document cache. OS file caches are not
flushed. These are latency observations, not cold-disk benchmarks.

For an idle-population variant, copy the generated YAML beside the original and
remove its 1,000 `path` maps, leaving Agents, properties, construction and registry
reference unchanged. One reproducible Python transformation is:

```python
from pathlib import Path
import re
source = Path('routing-scale.world.yaml')
text, count = re.subn(r'    path:\n(?:      [^\n]*(?:\n|$)){4}', '', source.read_text())
assert count == 1000
Path('routing-scale-idle.world.yaml').write_text(text)
```

Also run the benchmark on `resources/test-worlds/door-test-1.world.yaml` (17
Agents, 16 authored Paths, a registry-backed behaviour) and
`resources/test-worlds/realistic-pathing-test.world.yaml` (four Agents, one Path).
The population fixture contains default, shared-tag and individual properties
and four different destinations.

## Phase boundaries

Set `PF_RESTORATION_TIMING` to enable diagnostic output in either executable.
The instrumentation holds no samples or World references and is off by default.
Timings are **inclusive**; never sum parent and child timings:

- `reset-total`: the entire existing reset, including serialization, reconstruction,
  registry resolution, restoring paused/dirty state and serializer destruction.
- `reset-serialization`, `reset-parse`: transient representation writing/decoding.
- `reload-total`: document path validation, reading, reconstruction and resolution.
- `reload-read-parse`: file reading and decoding together. The serializer does not
  expose separate I/O and parser phases.
- `reset-reconstruction` / `reload-reconstruction`: schema validation/migration,
  object reconstruction, both construction replays, Agent restoration, and (when
  no tag registry is referenced) restored-Path rebuilding.
- `validation-replay`, `construction-replay`: disposable validation World and
  destination World respectively, including their `finishBuild()` calls.
- `graph-resources`: Graph build and Graph/traversal-topology validation inside
  those replays. Authored traversal-resource creation itself is interleaved with
  object creation, so is included in replay rather than separated from it.
- `reset-registries`, `reload-registries`: tag and behaviour registry resolution,
  including dependency I/O/preflight when necessary. With tags, Path rebuilding
  happens here, after effective routing properties have been resolved.
- `restored-paths`: restored destination searches and atomic publication.

There is no standalone timer for each Agent field or schema migration step;
those costs remain in reconstruction minus its nested phases. Path costs are
always freshly evaluated; no restored route cache or bypassed validation is used.

## Findings and intervention

Baseline: `ea981f641275f3740ebe9c38fae4ab8d82450064`, the #224 stress fixture
`build-windows/routing-scale-224-final.world.yaml`, 1,000 Agents / 2,040 Vertices.
Machine: AMD Ryzen 9 9955HX, 32 GiB RAM, Windows 11 Home 10.0.26200, Visual Studio
MSVC 14.51.36231, x64 Release. All comparisons used this machine/configuration
and the same fixture. No timing thresholds are enforced.

First, the requested binary experiment changed **only** reset's transient
serializer from YAML to the existing named-field `BinarySerializer`. Serialization
fell from about 39.8 to 4.9 ms and parsing from 67.2 to 4.4 ms. Total reset was
still about 51 seconds. YAML was not the dominant cost.

Phase instrumentation put about 25.4 seconds in **each** construction replay,
versus about 4.3 ms in each Graph build. A temporary nested timer on
`markerNameTaken()` recorded 506.37 seconds across 40,000 checks in ten operations,
against 508.22 seconds in their construction/validation replays combined.
This nested diagnostic was removed after isolating the cause.

`markerNameTaken()` enumerated Marker IDs and then called `lookupMarker()` for
every ID. Each lookup scanned Sector objects again. This made a name check
quadratic and restoring a sequence of Markers cubic. The fix inspects the
Marker objects directly in one scan, still checking all Sectors and excluding
only the requested identity. No index, invalidation protocol or retained cache
is added. Per-check complexity is now linear; repeated insertion remains
quadratic but is no longer the multi-second bottleneck. Both validation replays
and all migration/validation rules remain intact.

Reset retains the binary encoding improvement, using exactly the same World
schema and deserializer, not a second restoration model. Writer/reader trees are
released when no longer needed rather than retained through registry resolution.

## Measurements

Milliseconds, min / median / max, five operations per row unless specified:

| Stage | Reload | Reset |
|---|---:|---:|
| Original YAML reset | 50,696 / 50,711 / 50,725 (2 loads) | 51,067 (1 completed reset) |
| Binary reset, original Marker checks | 50,862 / 50,930 / 51,026 | 50,966 / 51,073 / 51,176 |
| Binary reset + direct Marker scan | 401 / 402 / 411 | 305 / 305 / 315 |

The initial YAML run was stopped during its second reset; its total is **not** a
five-sample distribution. The complete binary-before/after series isolates the
dominant fix: approximately **127x faster reload and 167x faster reset**. The
before series included the temporary nested Marker timer, whose output overhead
is part of those totals; the un-nested original run confirms the same ~51-second
baseline. Original and optimized loop shape was identical for this comparison
(load/reset/release, without the subsequently added simulation-trace assertions).

Final benchmark with authored-state/Path/trace assertions and simulation stepping
between operations (different allocator/cache history):

| Fixture | Reload min / median / max | Reset min / median / max |
|---|---:|---:|
| Mixed population with Paths | 417 / 433 / 449 | 323 / 337 / 352 |
| Same population without Paths | 302 / 302 / 308 | 206 / 208 / 216 |
| Door/behaviour fixture | 3.29 / 3.38 / 6.21 | 1.00 / 1.06 / 9.82 |
| Four-Agent fixture | 1.95 / 2.04 / 2.93 | 0.99 / 1.03 / 1.38 |

Final mixed-population median phases: YAML read/parse 72 ms; reset binary write
5.2 ms and parse 3.4 ms; validation replay 97 ms; construction replay 98 ms;
Graph build/validation 4.5 ms per replay (included); restored Paths 94 ms.
Registry resolution including Paths was 96 ms on reload. In the no-Path variant,
registry resolution was 0.65 ms and restored-Path time effectively zero. Remaining
costs are replay/object validation, YAML decoding on reload and actual route
searches, not registry attachment alone.

### Memory and allocations

The original binary baseline's current working set went from 45.6 to 49.2 MiB
across five cycles; the identical optimized loop went from 45.3 to 49.0 MiB.
Peak was not collected in that initial harness. The final harness also steps the
simulation, so its figures must not be mistaken for reset-only allocation growth:
active-population current working set ranged 99.8–137.5 MiB, lifetime peak reached
143.3 MiB; idle population 16.5–17.6 MiB, peak 44.5 MiB; Door fixture 10.5–10.7 MiB,
peak 12.4 MiB; four-Agent fixture 8.6–8.8 MiB, peak 9.6 MiB.

A separate 20-cycle active-population retention run passed all invariants. Its
peak reached 161.7 MiB at cycle 11 and stayed unchanged through cycle 19; the last
ten current-working-set samples ranged 105.6–140.9 MiB rather than increasing
monotonically. Paused/running cycles have different transient allocation histories.

Allocator/OS retained pages are not live World retention. Every cycle asserts
expiration of superseded Graph/World and representative Sector weak references.
No exact heap-allocation counter or heap profiler was available in this run.
Source-level allocation observation: the new name check eliminates the temporary
ID-vector construction/sorting on each check and introduces no replacement
container. Existing routing checks still enforce bounded routing-workspace/cache
allocation. The benchmark itself retains only one baseline byte string, route
digests and a scalar trace hash, not prior Worlds.

## Correctness and editor checks

`restoration-checks` is a CTest using the small registry-backed Door fixture.
Five cycles compare exact canonical authored bytes (including stable identities,
activation, references, seed and property samples), route vertices and perceived
costs, and a 30-tick Agent trace before/after reset and reload. Tests alternate
paused/running and clean/dirty states. Paused Paths are intentionally detached
into destination intents by the existing pause protocol, so Path comparison is
after resume. Registry pointer identity survives reset. Weak-reference assertions
check release rather than imposing machine-dependent RSS limits.

Existing full headless checks additionally cover deterministic Lua random streams,
activation, property sampling, migrations, malformed documents, registry failures,
atomic load and Marker identity/deletion. The Marker regression now explicitly
checks cross-Sector name uniqueness and self-exclusion.

Editor operations remain synchronous. A native wait cursor and window-title
suffix (`Loading World...` / `Resetting simulation...`) are installed before work
and restored by RAII on success or exception. Unlike an ImGui-only indicator,
this does not wait for a post-operation rendered frame. There is no fake progress
percentage or thread-safety change. Release editor/headless builds and automated
checks were run (all 59 Release CTests passed); interactive visual responsiveness has **not** been verified in
this harness. Manual check still required: open the generated World, run/reset
repeatedly, close/reopen, confirm temporary busy feedback, unchanged selection/error
handling and the ~0.3–0.5-second rather than ~51-second blocking interval.
