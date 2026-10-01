# Simulation observation and host clock

`World::setSimulationTimeScale()` controls only the conversion of elapsed host
seconds into accumulated simulation time. The default is 1x; the range is
0.05–100x (NaN is ignored). The toolbar offers 0.25x, 0.5x, 1x, 2x, 5x and 20x.
`advanceTick()` and `advanceTicks()` never consult this setting.

An `update()` executes at most `World::getMaxTicksPerUpdate()` (600) ticks.
Unexecuted time, including the fractional-tick remainder, stays in the accumulator
and is available through `getDeferredSimulationTime()` and the speed tooltip.
The clock tolerates the float fixed-timestep's accumulated representation error
at a whole-tick boundary and corrects a tiny negative remainder to zero. Invalid
or non-positive elapsed times execute no ticks. Pause retains the speed; the
existing pause/topology protocol clears accumulated time. Document load and Reset
restore 1x, while structural construction-record replay preserves the host speed.
Neither speed nor observation diagnostics are serialized or mark a document dirty.

## Snapshot lifetime and invalidation

`getSimulationSnapshot()` still returns an independent value.
`getSimulationSnapshotView()` borrows World's cached projection: do not retain the
reference, its vectors, or their iterators across a tick or mutation. The renderer
and read-only panels use the borrowed form. `getSimulationSnapshotBuildCount()`
counts whole-world cache builds, not reads or per-entity event projections.

World and coordination mutation boundaries invalidate the cache, including paused
editing and topology replay. Agent runtime mutation boundaries do so even when an
edit is not authored. Registry structure and authored child edits also advance a
conservative observation revision. This revision is deliberately independent of
`isModified()`: an already-dirty child can change again, and saving must not hide
an edit. It may invalidate another World's projection, but cannot affect its
simulation. Like the existing editing/tick APIs, these observation APIs are
single-threaded. New mutable entity APIs must invalidate observation as well.

Pending interactions are indexed by Agent once per observation revision. Stable
request order and first-pending-request selection are unchanged.

## Event publication

Ticks do not construct whole-world snapshots. World retains boundary projections
for active Agents participating in the phases, and initial states for Device
operations touched by advancement or cancellation. Publication visits these in
stable-ID order after the six phase events. New Device operations are excluded
using the tick-start registry high-water mark. Removed entries are skipped.

Agent baselines are captured **after** behaviour boundary commands, not simply
reused across ticks: authored changes or commands between ticks must not create
extra `AgentChanged` events. The change predicate remains the original predicate,
including its distinction between projected fields and event-triggering fields.

`src/headless/smoke/simulation/Observation.cpp` (the `observation` check in
[`pf-smoke-simulation`](smoke-modules.md)) checks the host clock, cache reuse and
invalidation, value ownership, serialization exclusion, moving/stationary Agents,
and publication against the original full-snapshot diff predicate. The existing
behaviour, transport, topology, and 500/1000-Agent checks remain in the headless
suite.
