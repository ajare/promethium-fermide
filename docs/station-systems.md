# Station systems — lifecycle, authored scenarios, containment, resource stores, transfers, compound resources and solar-supplied external loads (#386–#391, #393)

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

The binding limit is applied per flow as a minimum of the scaled
request and that flow's own drawable amount or headroom (#556): under
the fixed float timestep `requested × (limit ÷ requested)` can round
one ulp above the limit, and the naive product used to drive a store
negative (or over capacity) by ~1e-17 kg on an ordinary partial
conversion, which the strict zero/capacity audit reported as a
software failure. Clamping each flow to the limit it set keeps the
common scale within one ulp of exact; the residual lands in the
declared loss inside the documented `StationBalanceTolerance`, so no
material is invented or deleted. The per-store finite-limit audit
likewise honors that tolerance — a one-ulp roundoff is never a
failure, while any genuine negative or over-capacity breach still
fails the whole tick. The bundled Lua mirror applies the identical
clamped allocation so host and model agree on every flow.

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

### Solar supply for external equipment loads (#393)

Authors configure solar arrays and named External equipment load groups
(CONTEXT.md) per World. Each solar array declares an installed capacity in
kW and an illumination/eclipse profile: a repeating piecewise-constant
profile whose level is a fraction of full sun (0..1), or a constant
level when no period is declared. Solar production follows installed
capacity times evaluated illumination — sunshine is never firm
continuous capacity — and a zero-capacity array is disabled and
contributes nothing. Allocated power can never exceed the generation
actually available this tick.

Each external load group declares its full-operation electrical demand
and a repeating operating profile that scales it, its heat output, a
required thermal service class (`low`, `moderate`, `warm` or
`generation`), a cooling disposition, up to two material exchanges with
existing Resource stores, a protected critical floor, an integer service
priority and an allowed curtailment fraction. Load groups exclude
equipment whose demands are already calculated within the Station
systems, so internal equipment is never counted twice.

Allocation runs on the World tick before the resource ledger, in the
declared deterministic order (priority descending, declaration order for
ties) and in two passes: protected critical floors first — ahead of all
discretionary demand — then discretionary demand. A load is never
supplied below its minimum operable level, `demand × (1 − allowed
curtailment)`: if the remaining pool cannot reach that level the load
takes nothing and the pool stays available for loads that can use it.

The critical floor and that minimum are read together (#549). The floor
is a protected slice of the load's own service, not an independently
operable sub-service, so protecting it means running the load — and the
load cannot run below its minimum. The protected claim is therefore the
larger of the floor and `demand × (1 − allowed curtailment)`, and it is
granted only when generation plus available battery output can actually
reach that minimum. A claim that cannot reach it is refused whole: the
load takes no generation and no stored energy, exports no heat and runs
no material exchange, and its **entire** floor stays explicit unmet
critical demand rather than just the slice the pool happened to fall
short of. A critical floor is never a way to bypass the load's minimum,
and scarce energy is never spent on an operating state the equipment
cannot physically hold. A floor the generation cannot meet remains
explicit unmet critical demand; it is never silently reclassified as
permissible curtailment. Eclipse and sunlight transitions, curtailment,
refusals and unmet critical floors are announced in the event log on
their status transitions.

Missing cooling is never silently supplied. A load whose disposition is
`coolingService` requires a cooling service of its declared thermal
class; this slice provides no cooling capacity in any class, so such a
load is not admitted: it draws no power, produces no station heat, and
its need stays visible as unmet cooling. Loads with the `exported`
disposition declare that their heat leaves the station through an
explicit export boundary — an accounted external exchange at the
full-operation delivered fraction (the operating-profile level times
the delivered fraction of the profile-scaled request), not a hidden
infinite sink.

Declared material exchanges are full-operation rates and scale by the
same full-operation delivered fraction: duty-cycled or curtailed
equipment consumes and delivers less (#548). Every declared exchange is
also an **operation dependency** (#554), never a silently optional side
effect: before any electrical allocation, each load's declared feed
(qualified material above the store's protected reserve) and
destination headroom are evaluated against the live inventory to
compute a **material-feasible operating fraction** — the largest
fraction of the load's full-operation exchanges those limits can
sustain this tick. The electrical request is capped to that fraction,
so a starved or backed-up load never draws power it cannot run on,
never exports heat it would only make while running, and never books
outputs there is no room to deliver. Draws take only qualified material
above the protected reserve and deliveries only land inside the
destination's remaining capacity — never hidden external makeup. When
the capped request cannot reach the load's minimum operable level the
load is refused whole: it spends no power and no material and its
entire protected floor stays explicit unmet critical demand. A grant
reserves its capped feed draw and destination claim against the
inventory ahead of later loads in the allocation order, so competing
loads never double-book the same kilograms. The shortfall stays
explicit: per-exchange requested/completed/unmet amounts record the
need that did not move, the refusal or curtailment reason names the
binding limiter (empty feed, held-only feed, protected reserve,
destination full), and unmet service, unmet material, power and heat
all agree at the capped fraction. Completed amounts book at the
external boundary — draws are losses, deliveries are negative losses —
so the per-unit and per-constituent ledgers stay closed and the
conservation audit is unchanged.

The snapshot publishes actual versus requested service: per-source
capacity, evaluated illumination, available kW and the disabled flag;
per-load requested/supplied/unmet kW, critical floor and unmet
critical, delivered fraction, exported heat and unmet cooling,
admission state with the block reason, the material-feasible operating
fraction (#554), and per-exchange requested/completed/unmet amounts;
plus station totals for generation, requested, supplied, unmet, unmet
critical, surplus and exported heat.
The bundled Lua lifecycle model receives the descriptors as plain
immutable values, mirrors the same two-pass allocation inside each
private sandbox and audits every tick that allocated power never exceeds
generation and no load is supplied above its request.

Non-finite derived magnitudes are never published as healthy results
(#553). Individual authored values are validated finite, but sums and
products of finite values can overflow to infinity, and an infinite
magnitude passes every inequality audit unnoticed (`inf > inf` is
false). The host ledger audits the finiteness of every committed
numeric power, material and energy output and ledger intermediate —
the generation pool as it is summed, per-source, per-load,
per-exchange and per-battery results, transfer and conversion flow
amounts, the loss ledgers, store inflow/outflow and the published
station-wide totals — and the bundled Lua mirror rejects an
overflowed generation pool and audits the same committed values in
the sandbox. A non-finite derived result fails the tick whole through
the contained path: nothing is published, the World pauses, and the
structured diagnostic names the failed value. Ordinary finite
shortages remain valid outcomes; a large-but-finite magnitude is
never refused.

World schema 69 persists the authored arrays and load contracts (units:
kW, seconds, fractions); a pre-69 document carrying `solarArrays` or
`equipmentLoads` is refused rather than silently dropped. Allocation
results, delivered fractions and exchange flows remain runtime-only:
Load and Reset reconstruct them from the authored starting conditions.
The separate Station Systems window drafts solar arrays (capacity,
constant or periodic illumination with segments) and equipment loads
(demand, duty profile, heat, thermal class, cooling disposition,
critical floor, priority, curtailment and material exchanges) with full
validation reporting, and shows the live source output, per-load
actual/requested service, unmet cooling and power totals beside the
existing views.

### Finite batteries and electrical deficits (#394)

Authors configure rechargeable battery banks per World. Each battery
separates nameplate energy (the installed total, kWh) from usable energy
(the cyclable slice above the reserve floor). The reserve is the derived
remainder, nameplate minus usable: it is never dischargeable, is never
counted as available energy, and is published in the snapshot so the
reserve policy is explicit rather than implicit. Starting stored energy
is authored within the nameplate; current stored energy is runtime state
initialised from it and never written back. Charge and discharge power
limits are per tick, and charge/discharge efficiencies are applied at
the bus boundary: charging stores bus kWh x charge efficiency and
discharging delivers stored kWh drawn x discharge efficiency, so both
conversion directions book their losses. Every kW <-> kWh crossing
multiplies or divides by elapsed hours: a fixed-tick duration is
converted from seconds to hours (divided by 3600) before it meets a
power value, so 1 kW over one second moves 1/3600 kWh and over one
hour exactly 1 kWh. Efficiency must be greater
than zero and at most one; usable energy can never exceed the nameplate,
so no policy can invent storage the battery does not have. Zero usable
energy disables the battery entirely — it stores nothing and delivers
nothing.

Battery discharge rides the same allocation pool as generation; it is
never a second power budget. In the protected pass, a critical claim
generation cannot meet is offered to the batteries before it remains
explicit unmet critical demand, and the claim is measured against the
load's minimum operable level: if generation plus available battery
output still cannot reach that minimum, the load is refused whole and
neither the sun nor stored energy is spent on it (#549). In the
discretionary pass, the demand generation leaves over is offered to the batteries under the same
minimum-operable rule: if generation plus available battery output
still cannot reach `demand x (1 - allowed curtailment)`, the load takes
nothing. Generation is always taken before stored energy, so finite
energy is preserved rather than wasted. The per-tick discharge power cap
is shared across both passes: whatever a battery already delivered this
tick counts against its own limit, so the passes can never jointly
exceed it. Discharge draws only energy above the reserve floor and
never drives stored energy below the lower of the starting value and
the reserve. Charging takes only the surplus no load took, within the
charge power limit and the headroom below nameplate; a battery that
discharged this tick never charges in the same tick, so no cycle can
circulate energy.

Conservation is audited every tick in the host and mirrored inside each
private Lua sandbox: stored energy stays within the nameplate and never
goes negative, the ledger closes (starting - discharged + charged -
stored within tolerance), a battery never delivers more energy than it
drew from storage and never stores more than the bus supplied — free
energy is a conservation violation that fails the whole tick through
the integrated error path, never a silent rounding. The total-supplied
audit likewise holds against generation plus actual battery output.

The snapshot publishes energy, power, reserve and shortfall: per
battery the nameplate, usable and reserve kWh, starting and current
stored energy, state of charge, the power and efficiency limits, the
last tick's charge and discharge power, the discharge request routed to
the battery and its shortfall, the cumulative charged/delivered/input
ledger with losses and residual, and the disabled/at-reserve/saturated
flags; plus station totals for battery discharge, charge and shortfall
beside the existing power totals. Exhaustion (usable energy depleted to
the reserve floor), recovery above the floor, saturation at nameplate
and discharge limitation are announced in the event log on their status
transitions. World schema 70 persists the authored battery contracts
(units: kWh, kW, fractions); a pre-70 document carrying `batteries` is
refused rather than silently dropped. Stored energy, loss ledgers and
per-tick results remain runtime-only: Load and Reset reconstruct them
from the authored starting conditions, and repeated eclipse cycles
replay identically. The separate Station Systems window drafts
batteries (nameplate, usable, starting stored, charge and discharge
power, efficiencies) with full validation reporting, and shows the
live stored energy, visible reserve floor, state of charge, charge and
discharge power, cumulative losses and the battery totals beside the
existing views.

### Scheduled resource deliveries (#392)

Deliveries are authored external supply: each contract names one
existing Resource store, the same unit as that store, a positive
quantity, a first arrival time, and an optional recurrence interval.
A recurrence of zero is a one-off arrival; any other value must be at
least one World fixed tick, so occurrences can never fall inside a
tick and disagree with the host about how many have come due. Arrivals
are derived from elapsed scenario time rather than stored, so the
schedule is a pure function of the authored contract and the tick
count.

Every occurrence is a whole arrival that either lands or is refused:
the store takes as much as its remaining capacity allows and the rest
is refused whole rather than queued, split, or silently dropped.
Refused material never enters the station, so it cannot later be
cooled, converted or transferred; it is accounted per delivery and
cumulatively as explicit loss beside the store's own. A delivery into
a store that is already full is refused in full and still counts as an
occurrence, so a schedule never silently disappears.

Deliveries run first in each tick, before power settlement, transfers
and conversions, so arriving mass is available to the rest of the
station in the same tick rather than a tick late. When several
deliveries share an arrival time and compete for the same store, the
order is fixed and documented: arrival time, then higher priority
first, then declaration order. That total order is shared with the
sandbox, so two runs of the same scenario fill the same stores in the
same sequence.

The per-unit and per-constituent balance ledgers gain a `delivered`
term alongside `lost`, so residual stays zero for a closed system:
material that arrived and was refused is visible as delivered-out, not
missing. World schema 71 persists the authored delivery contracts
(units: kg, L, gallons, seconds); a pre-71 document carrying
`deliveries` is refused rather than silently dropped. Accepted and
refused amounts, occurrence counts and next arrival times are
runtime-only: Load and Reset reconstruct them from the authored
schedule, and repeated cycles replay identically. The separate Station
Systems window drafts deliveries (store, unit, quantity, first
arrival, recurrence, priority, arrival state) with full validation
reporting, and shows the live accepted/refused amounts, occurrence
counts and the next arrival time for every contract beside the
existing views.

## Explicit scope limit

This slice implements lifecycle, authored finite store configuration,
bounded transfers between stores, compound-resource composition and
conversion accounting, solar-supplied external equipment load
allocation, finite battery storage with charge/discharge losses, and
authored scheduled resupply with refused-quantity accounting.
It still adds no crew demand, treatment/clearance
workflows or ejection, no cooling capacity in any thermal
service class (cooling-service loads are refused, not cooled), and no
termal reservoir temperature
dynamics. Completed tick counts are
observability, not oxygen, heat or endurance estimates. It
invents no process coefficients beyond the authored demand, capacity
and profile values. ECLSS/thermal models, crew inputs, alarms,
trends and broader supervisory allocation belong to the subsequent #385
slices. Agent behaviour, movement, physiology and random streams are
unchanged.

Acceptance coverage uses World ticking/snapshots/events/diagnostics (including
real script failure fixtures, store boundary fixtures: zero/empty/full
stores, malformed values, isolated Worlds and no runtime dirtying of authored
documents, transfer fixtures: full/partial/rejected outcomes,
protected-reserve and capacity blocking, priority allocation among
competing requests, conservation residuals within tolerance, and a
contained protected-reserve violation; compound fixtures: refused
compositions and impossible yields naming the created constituent,
balanced conversion runs with reported constituent residuals, partial
throughput under protected reserves, capacity-refused conversions that
consume nothing, competing-conversion priority, gram-normalized
constituents, cross-composition transfer refusal, reset reproducibility,
compound file round trips and pre-68 refusal; and power fixtures:
refused solar/load configurations, full-sun full supply with explicit
surplus, zero-capacity source disablement, eclipse/sunrise transitions
with explicit unmet demand and deterministic replay, priority allocation
with the loser's unmet remainder visible, critical-floor protection
ahead of higher-priority discretionary demand and explicit unmet
critical shortfall under a starved source, minimum-operable admission of
a positive critical floor (a floor below the load's minimum is protected
at the minimum, and a pool that cannot reach the minimum refuses the
load whole with the entire floor unmet, no heat and no material exchange,
releasing both sun and stored energy to loads that can use them),
curtailment-floor refusal leaving the pool for flexible loads, cooling-infeasible loads never
admitted with unmet cooling visible, material exchanges scaled by the
full-operation delivered fraction (profile level times delivered
fraction, covering fractional profiles, added curtailment and zero
profile in host results and sandbox execution) and treated as operation
dependencies (#554: empty, held-only and protected-reserve feeds refuse
the load with the precise reason; partial feed caps the operating
fraction with power, heat, inputs, outputs and unmet material all
agreeing; a full destination backpressures the load to refusal; a feed
below the minimum operable level refuses the load whole and leaves the
pool for flexible loads; competing loads never double-book shared feed;
host and sandbox agree) with closed boundary ledgers, and intact Lua
descriptor crossing with contained construction refusal; and battery
fixtures: refused battery
configurations, surplus charging with the untaken generation exactly
consumed, saturation at nameplate with surplus restored, eclipse
ride-through with the deficit carried through the shared pool and the
shortfall visible, exhaustion exactly at the reserve floor with the
load fully unmet afterwards, sunrise recovery and recharge, power-
limited ride-through with the remainder explicit, charge and discharge
efficiency losses booked with the ledger closed and no free energy,
zero-usable-capacity disablement, critical-floor protection from stored
energy ahead of discretionary demand, minimum-operable refusal leaving
stored energy untouched, reserve-never-drawn, deterministic repeated
eclipse cycles with identical reset replay, and intact Lua battery
descriptor crossing with contained construction refusal), real file
save/reopen round trips in
YAML and binary including the solar/load and battery scenario
reproduction and pre-69/pre-70/malformed refusals, failed-write and
legacy/malformed-file
handling, paused structural rebuild preservation,
and the existing CPU ImGui and document-history seams. Run `ctest -R
'^station-'` in Release.
