# Station systems — lifecycle slice (#386)

Station systems are opt-in per World and disabled in existing documents. Open
**View > Station Systems** or use the
**Station Systems** toolbar toggle. This is a separate top-level window;
closing it only changes visibility. It reports the active World, pause/run
status, World and station elapsed simulation seconds, all four system objects'
completed ticks, bounded events and structured software diagnostics.

## Authored settings and lifecycle

- **Enable Station systems**: boolean, default false.
- **Observation interval (seconds)**: finite 0.1–60 seconds, default 1 second.
  This provisional UI/event sampling setting is rounded upward to whole World
  ticks. It neither changes integration cadence nor creates another clock.
- Editing requires a paused World. Apply validates and commits one document
  history entry. After any World ticks, Apply asks for confirmation of a complete
  World reset (including Agent movement); cancellation preserves live and
  authored state. An unchanged apply is a no-op.
- World schema 65 stores only settings, never ticks, events or Lua state.
  Legacy documents default disabled. YAML, binary, load, Reset and history use
  the existing World persistence seams. Load/Reset create fresh objects; plain
  pause/resume and ordinary structural replay retain station state.

## Execution boundary

Each enabled World owns four independent Lua objects: Power, ECLSS, Thermal
control and Monitoring/supervision. Each executes the bundled
`station-systems/lifecycle.lua` factory in a separate private Lua 5.4 state,
with a 512 KiB allocation budget and 100,000 instructions per construction/step.
It reuses the deterministic sandbox shared by existing script adapters. Lua
receives only a system name, the next World tick and fixed-tick duration: no
World, Agent, filesystem, process, wall-clock or mutable domain objects.

The application loads `StationSystemsLifecycle` as a managed `LuaScript`
Resource from `Resources.yaml`. Headless use embeds that exact same asset,
following the bundled Human source pattern. Only immutable source is shared;
no executed Lua table, VM or upvalue is shared across objects or Worlds.

SimulationCoordinator advances station objects at the World tick boundary.
Existing time scale and update batching feed the same fixed ticks. The station
snapshot derives elapsed seconds from the committed World tick, not cumulative
wall time. Rendering never executes station scripts. Public World methods
`getStationSystemsSnapshot`, `getStationSystemsEvents` and
`getStationSystemsDiagnostics` return read-only value copies. Events retain at
most 256 entries; a failure is terminal until Reset, so diagnostics are bounded.

A failed script call, memory/instruction exhaustion, invalid return value or
tick-accounting mismatch pauses the World before that tick advances. All four
results are staged before publication; no partial station result or elapsed
time is published. Mutated Lua objects are discarded, and resuming cannot
retry the failed runtime. Reset reconstructs authored initial state.

## Explicit scope limit

This slice implements lifecycle only. Completed tick counts are observability,
not power, oxygen, heat or endurance estimates. It invents no process
coefficients, resource inventories, production, consumption or conserved
exchanges. Physical power/ECLSS/thermal models, external crew/equipment inputs,
alarms, trends and supervisory allocation belong to the subsequent #385
slices. Agent behaviour, movement, physiology and random streams are unchanged.

Acceptance coverage uses World ticking/snapshots/events/diagnostics (including
real script failure fixtures), YAML/binary reconstruction, and the existing
CPU ImGui and document-history seams. Run `ctest -R '^station-'` in Release.
