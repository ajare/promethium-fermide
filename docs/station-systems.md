# Station systems — lifecycle, authored scenarios, containment and resource stores (#386–#389)

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
- Editing requires a paused World. The window separates the authored start
  conditions from the draft: Apply is disabled until the draft differs, and
  the window reports the document's saved/unsaved state. Applying commits one
  document history entry. After any World ticks, Apply asks for confirmation
  of a complete World reset (including Agent movement); cancellation preserves
  live and authored state. An unchanged apply is a no-op.
- World schema 65 stores only settings, never ticks, events or Lua state.
  Schema 66 adds authored Resource stores inside the same `stationSystems`
  map; a pre-66 document carrying `stores` is refused rather than silently
  dropped. Legacy documents default disabled; a pre-65 document carrying
  Station data is refused rather than silently dropped. YAML, binary, load,
  Reset and history use the existing World persistence seams. Load/Reset
  create fresh runtimes; plain pause/resume and paused structural rebuilds
  retain the live station instead of restarting it.

## Reproducible scenarios (#387)

A saved World is a scenario, not a checkpoint: saving through the real
`saveTo`/`loadWorldDocument` file seams and reopening reconstructs a fresh
runtime that reproduces the original run tick-for-tick — identical snapshots
and event history after the same tick count. Saved documents are asserted to
contain only the authored settings: no tick counts, elapsed time, event
history or Lua source/state. A refused save (for example into a read-only
directory) leaves the document dirty, the previous file unchanged and
reloadable, no temporary file behind, and the live World running its new
settings. Malformed settings in a file are rejected without disturbing the
World already open in the editor.

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

### Failure containment (#388)

Diagnostics identify the attempted World tick, failing system, bounded message
(up to 2048 bytes), and machine-readable `ScriptExecutionFailure`: Lua error,
instruction budget exceeded, memory budget exceeded, or conversion error.
Non-finite/non-integer/negative completed counts and tick-accounting mismatches
are conversion errors, distinct from script exceptions. Error objects are not
coerced outside protected Lua execution, and error strings are bounded before
host copying. The window displays the category and explicit World-reset recovery
guidance. Headless `advanceTick`/`advanceTicks` return false and the World pauses;
repeated resume attempts neither duplicate diagnostics nor retry retired objects.

All four results and observation events remain at the last committed tick even
if earlier systems and the failing system already mutated their private Lua
state. Reset clears diagnostics and rebuilds all four objects from authored
settings. Budget exhaustion remains terminal even if Lua catches it with
`pcall`; ordinary caught application errors can still yield a valid step.
Construction errors refuse settings atomically with a diagnostic.

Physical shortages are not software failures: this lifecycle slice has no
physical inventories or shortage outputs to validate. Later physical slices must
represent shortages as finite valid results, not throw software exceptions. No
station demand is derived from Agents and no Agent behaviour semantics change.

### Finite Resource stores (#389)

Authors declare finite starting supplies per World. Each store carries an
identity, a declared unit, a capacity and a starting inventory split into
qualified (usable) and unqualified (held awaiting treatment or clearance)
quantities. The host validates every store before anything runs: ids must be
non-empty, unique and at most 64 characters; units must come from the declared
vocabulary (`g`, `kg`, `mL`, `L`, `m3`, `mol`, `kJ`, `MJ`, `Wh`, `kWh`);
capacity must be finite and positive; starting quantities must be finite and
non-negative and their sum cannot exceed capacity; at most 32 stores per World.
Invalid or non-finite configuration is refused before a runtime is constructed
and before the document changes, through the same paused-only Apply/reset-
confirmation flow as the other settings.

Current inventory is runtime state owned by the station runtime, initialised
from the authored starting quantities when the runtime is constructed. It is
never written back into the authored settings and never serialized; load and
Reset reconstruct it from the authored values. This slice adds no production,
consumption or transfer processes, so current quantities equal their starting
values for the whole run; later slices change them through validated
exchanges. The snapshot exposes each store's id, unit, capacity, starting
qualified/unqualified and current qualified/unqualified beside the tick
counts.

Validated store descriptors cross into each system's private Lua sandbox as
plain immutable values at construction; the bundled lifecycle model
initialises its own copy and re-checks the finite-capacity invariants every
tick, so a corrupt crossing fails contained exactly like any other script
failure. The separate Station Systems window edits the store set in the draft
(add, remove, id, unit, capacity, starting usable/held), reports validation
errors without mutating anything, and shows the live table with units, usable
versus held inventory, capacity and initial versus current state. Ticking
never dirties the authored document.

## Explicit scope limit

This slice implements lifecycle and authored finite store configuration.
Completed tick counts are observability, not power, oxygen, heat or endurance
estimates. It invents no process coefficients, production, consumption or
conserved exchanges. Physical power/ECLSS/thermal models, transfers between
stores, external crew/equipment inputs, alarms, trends and supervisory
allocation belong to the subsequent #385 slices. Agent behaviour, movement,
physiology and random streams are unchanged.

Acceptance coverage uses World ticking/snapshots/events/diagnostics (including
real script failure fixtures and store boundary fixtures: zero/empty/full
stores, malformed values, isolated Worlds and no runtime dirtying of authored
documents), real file save/reopen round trips in YAML and binary, failed-write
and legacy/malformed-file handling, paused structural rebuild preservation,
and the existing CPU ImGui and document-history seams. Run `ctest -R
'^station-'` in Release.
