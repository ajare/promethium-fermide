# Station systems — lifecycle, authored scenarios, containment, resource stores, transfers and compound resources (#386–#391)

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
  dropped. Schema 67 adds store protected reserves and authored Resource
  transfers; a pre-67 document carrying `protectedReserve` or `transfers`
  is refused rather than silently dropped. Legacy documents default disabled; a pre-65 document carrying
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
(add, remove, id, unit, capacity, starting usable/held, protected reserve),
reports validation errors without mutating anything, and shows the live table
with units, usable versus held inventory, capacity and initial versus current
state. Ticking never dirties the authored document.

### Bounded Resource transfers (#390)

Authors declare rate-limited transfers of qualified material between two
same-unit Resource stores. Each transfer carries an identity, source and
destination store ids, a declared unit, a positive finite rate in store
units per simulated second, and an integer priority. Host validation
refuses empty/duplicate/oversized ids, unknown source or destination
stores, self-transfers, unit mismatches with either endpoint (cross-unit
movement is conversion, deferred to later slices), non-finite or
non-positive rates, and more than 32 transfers per World — all before any
runtime construction or document mutation, through the same paused-only
Apply/reset-confirmation flow. Each store also gains an authored
`protectedReserve`: a finite, non-negative amount of usable inventory
withheld from discretionary transfers, never exceeding the starting usable
quantity.

The authoritative ledger lives in the station runtime. Each World tick,
competing transfers are applied in the declared deterministic order:
priority descending, ties broken by declaration order. Every request is
`rate × fixed tick duration`; what may actually move is bounded by the
source's qualified inventory above its protected reserve and the
destination's remaining total capacity. The whole tick's transfers are
staged, audited and committed atomically: only qualified material moves,
protected reserves are never drawn, destinations never overflow, and
unmet material simply does not move — backpressure never deletes or
creates material. Shortages are valid outcomes: each transfer publishes
its last requested, completed and unmet amounts with a full/partial/
rejected status, and status transitions are announced in the event log
with the stall reason (source empty, protected reserve reached, or
destination at capacity).

Per-store last-tick flows and per-unit boundary balance residuals are
published in the snapshot beside the explicit numerical tolerance
(`StationBalanceTolerance`, 1e-9 relative to the unit's starting total).
A residual beyond tolerance — or any negative or over-capacity staged
quantity — is a conservation violation, not a shortage: it fails the
whole tick through the integrated error mechanism (World paused,
structured diagnostic, nothing partially committed), never silent
clamping. The bundled Lua lifecycle model receives the transfer rules as
plain immutable descriptors and mirrors the same deterministic ledger
inside each private sandbox, re-auditing limits and conservation every
tick so a corrupt crossing or model drift fails contained like any other
script failure.

Transfer results are runtime state: only the authored rules persist
(schema 67), never requested/completed/unmet amounts or flows. Load and
Reset reconstruct the ledger from the authored starting conditions, and
replaying the same World tick sequence reproduces the outcomes exactly,
including which competing request won the last kilogram.

### Compound resources and constituent accounting (#391)

A Resource store may declare a constituent composition, making it a
compound material (food, living biomass, waste, and similar authored
substances). Compositions use a documented dry-mass basis: moisture is
a fraction of wet mass, while every other fraction is a fraction of dry
mass. Living biomass is dry mass (CONTEXT.md), so its water is carried
by the separate moisture term rather than diluting the declared dry
fractions. The tracked constituents are water, nutrient nitrogen,
atmospheric nitrogen, phosphorus, potassium, grouped other minerals and
dryOrganics — the dry-mass balance that makes the seven constituents
partition the wet mass exactly — plus a declared chemical energy term
(kJ per kg dry) where represented. Mass constituents are accounted in
kg regardless of whether the store is denominated in g or kg.

Validation refuses malformed compositions before anything runs: the unit
must be a mass unit (g or kg); moisture must be finite and below 1;
every dry fraction must be finite and non-negative and their sum may not
exceed the dry mass; the energy term must be finite and non-negative.

Nitrogen, phosphorus and potassium are tracked separately, with other
minerals grouped. Bound nutrient nitrogen is never freely interchangeable
with atmospheric nitrogen: transfers between stores with different
declared compositions are refused, so the two nitrogen forms (and any
two different compounds) can only exchange material through an
explicitly declared conversion process whose balances are validated.

An authored conversion consumes input stores and produces output stores
at declared full-throughput rates (1–4 flows each, all endpoints
compound stores). Validation checks the constituent and energy balance
at declared full throughput: any configured yield that would create a
tracked constituent or energy above what the inputs supply is rejected
with the offending constituent named — impossible yields are refused at
configuration rather than creating material at runtime. Losses below
the input are legal: they are booked as explicit declared losses.

The runtime ledger applies transfers first, then conversions, each in
priority descending and declaration order. Each conversion runs at one
common completed scale per tick: the fraction of declared full
throughput bounded by drawable input inventory above protected reserves
and by output store headroom. Full/partial/rejected status, per-flow
requested/completed amounts and the completed scale are published, and
status transitions are announced in the event log with the limiting
flow. A refused conversion moves nothing: input material is never
consumed without the declared products being accepted.

The snapshot reports a per-constituent boundary ledger: for every
tracked constituent, the station's starting total, current total,
cumulative declared conversion losses, and the residual
(current + lost − starting) against the same explicit tolerance as the
per-unit balances. The per-unit balance likewise closes over its
cumulative conversion loss, so a unit whose mass partially leaves
through declared losses still balances. A residual beyond tolerance is
a conservation violation: it fails the whole tick through the
integrated error mechanism, never silent creation or deletion.

The bundled Lua lifecycle model receives compositions and conversion
rules as plain immutable descriptors, re-checks them at construction
(including the no-creation yield rule) and re-audits per-unit and
per-constituent conservation inside every private sandbox each tick.
World schema 68 persists declared compositions and authored conversion
rules; a pre-68 document carrying `composition` or `conversions` is
refused rather than silently dropped. Completed scales, flows and loss
ledgers remain runtime-only: Load and Reset reconstruct them from the
authored starting conditions. The separate Station Systems window edits
compositions and conversions in the draft with full validation
reporting, and shows live conversion statuses and the constituent
balance ledger beside the existing views.

## Explicit scope limit

This slice implements lifecycle, authored finite store configuration,
bounded transfers between stores, and compound-resource composition and
conversion accounting. It still adds no crew demand, production from
external inputs, treatment/clearance workflows, deliveries or ejection. Completed tick counts are
observability, not power, oxygen, heat or endurance estimates. It
invents no process coefficients, production or consumption. Physical
power/ECLSS/thermal models, external crew/equipment inputs, alarms,
trends and supervisory allocation belong to the subsequent #385 slices.
Agent behaviour, movement, physiology and random streams are unchanged.

Acceptance coverage uses World ticking/snapshots/events/diagnostics (including
real script failure fixtures, store boundary fixtures: zero/empty/full
stores, malformed values, isolated Worlds and no runtime dirtying of authored
documents, and transfer fixtures: full/partial/rejected outcomes,
protected-reserve and capacity blocking, priority allocation among
competing requests, conservation residuals within tolerance, and a
contained protected-reserve violation; and compound fixtures: refused
compositions and impossible yields naming the created constituent,
balanced conversion runs with reported constituent residuals, partial
throughput under protected reserves, capacity-refused conversions that
consume nothing, competing-conversion priority, gram-normalized
constituents, cross-composition transfer refusal, reset reproducibility,
compound file round trips and pre-68 refusal, plus the compound editing
workflow in the separate window), real file save/reopen round trips in
YAML and binary, failed-write and legacy/malformed-file handling, paused
structural rebuild preservation,
and the existing CPU ImGui and document-history seams. Run `ctest -R
'^station-'` in Release.
