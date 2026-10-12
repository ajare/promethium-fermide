# Station systems — lifecycle, authored scenarios, containment, resource stores, transfers, compound resources, solar-supplied external loads, batteries, scheduled deliveries, thermal reservoirs with radiator rejection and coolant loops with freeze-protection heating and chilled thermal services (#386–#391, #393–#397)

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

### Thermal reservoirs and radiator rejection (#395)

Authors configure thermal reservoirs and radiators per World. A reservoir
(CONTEXT.md) carries an effective thermal capacity in kJ/K, a starting
temperature and declared min/max service-limit temperatures, all above
absolute zero, plus a constant environmental absorbed heat in kW.
Temperature is integrated explicitly on the fixed World tick from the
heat actually moved this tick: `T += (heatIn - rejected) x dt /
capacity`. Thermal energy is tracked against a 0 °C reference
(`E = capacity x T`), and the per-reservoir ledger — starting +
absorbed + routed − rejected − current — must stay within the declared
`StationBalanceTolerance`; a residual beyond it is a conservation
violation that fails the whole tick through the integrated error path,
never silent creation or deletion of heat. Declared temperature limits
are reported as above/below-limit crossings with transition events; they
never clamp the integrated state.

Passive conductance exchange — loop-to-reservoir and compartment-to-
reservoir — is **not** integrated as a tick-start rate applied whole.
`Q = G x (T_other - T_self) x dt` is only stable while `G x dt / C`
stays small; past the bound it overshoots the equilibrium the two bodies
share, and because the formula keeps pushing on the far side it reverses
hot and cold bodies and drives temperatures below absolute zero (#559).
Instead every reservoir's whole exchange star — all its coolant loops
and all its pressurised compartments — is solved **implicitly** once per
tick against the reservoir's end-of-tick temperature, so the leaves
compete for the same finite heat rather than each draining a temperature
none of them ever sees move. Eliminating the leaves from that implicit
system leaves a closed form: with coupling `a_i = dt x G_i / C_i` and
weight `w_i = dt x G_i / (1 + a_i)`, the reservoir ends at the weighted
average of the tick-start temperatures and each leaf's heat is
`w_i x (T_reservoir_end - T_leaf)`. Every weight is non-negative and
each new temperature is a weighted average of temperatures that already
existed, so the exchange approaches the coupled equilibrium as closely
as the conductance allows but can never cross it, reverse a hot and a
cold body, amplify their separation, or leave the range the tick started
inside — however stiff the conductance or lopsided the capacities. The
heat a leaf gains is exactly what the reservoir gives up, so both
ledgers close on one number rather than two independently rounded ones.
This is bounded integration, not clamping: nothing is capped behind the
model's back and no declared service limit is silently enforced.

A radiator rejects its host reservoir's heat by the Stefan-Boltzmann
law: `emissivity x view factor x area x sigma x (T^4 - T_sink^4)`, at
the reservoir's current emitting temperature against an authored
effective sink temperature. Environmental back-radiation is explicit —
a warmer sink rejects less — and the radiator/view assumption is the
declared emissivity-times-view-factor product; no physical distribution
modelling is promised beyond these declared factors. A body cannot
radiatively cool below its coldest sink nor warm above its warmest one:
the clamp is booked into the ledger, not hidden, and scaled across the
radiators that produced the rejection.

Emitted equipment heat is routed exactly once. An External equipment
load may declare a heat-sink reservoir; its admitted heat (the authored
heat output at the full-operation delivered fraction) then enters that
reservoir and is not also booked as direct export — the reservoir's
radiators become its export path. Validation refuses an unknown sink,
a sink whose declared thermal service class differs from the load's
(spare capacity in one class is never usable by another), and a sink
on a cooling-service load, which needs a cooling service rather than a
passive sink. Electrical input is never converted into duplicate local
heat: the authored heat output is the emitted heat, and energy stored
in batteries or bound into material is not heat at all. Loads without
a sink keep the pre-#395 direct-export behaviour untouched.

The snapshot publishes per-reservoir temperature, limits, capacity,
absorbed/routed/in/rejected/net heat, the full energy ledger with its
residual and the above/below-limit flags; per-radiator authored
contract and last-tick rejection; and station thermal totals. The
bundled Lua lifecycle model receives the descriptors as plain
immutable values, mirrors the identical integration and sink clamp
inside each private sandbox, and re-audits the ledger and finiteness
every tick. World schema 72 persists the authored reservoirs,
radiators and load `heatSinkId` (units: kJ/K, °C, m², fractions); a
pre-72 document carrying them is refused rather than silently
dropped. Integrated temperatures and heat ledgers remain runtime-only:
Load and Reset reconstruct them from the authored starting
conditions. The separate Station Systems window drafts reservoirs
(class, capacity, starting temperature, limits, absorbed heat) and
radiators (reservoir, area, emissivity, view factor, sink) with full
validation reporting, and shows the live temperatures, heat flows,
limit crossings and the closing energy ledger beside the existing
views.

### Coolant circulation and freeze-protection heating (#396)

Authors configure coolant loops per World. A loop owns a finite,
loop-specific coolant inventory (kg) with a declared hazard class
(`normal` or `hazardous`) and is coupled heat-only to exactly one
thermal reservoir: the loop's coolant never crosses into the reservoir,
never touches a Resource store, and is never potable or nutrient water.
Normal and hazardous coolant are segregated classes — no process pools
them or moves coolant between loops, so segregation is structural.
Each loop declares its coolant capacity and starting charge, coolant
specific heat, starting temperature, a freeze-protection setpoint, a
pump power draw at full circulation with a rated flow, a
full-circulation exchanger conductance, and a freeze-heater capacity.
Zero starting coolant disables the loop entirely.

The pump circulates only the coolant the loop actually holds: the fill
fraction (inventory / capacity) caps the pump demand, so an
underfilled loop is capped proportionally and a dry loop draws nothing
and flows nothing — circulation is never invented from an empty
inventory. The freeze-protection heater is a bang-bang thermostat: it
demands up to its capacity whenever the loop's coolant temperature is
below the setpoint, and demands nothing at or above it. Both demands
are drawn from the same generation-and-battery pool as external loads,
after the external load allocation and before battery charging — the
loop is never a second power budget, and the heater consumes surplus
generation before it can be stored.

Pump work and heater heat enter the loop exactly once (all pump work
dissipates into the coolant it actually moves) and reach the reservoir
only through the circulation-gated heat-only exchanger, whose worth this
tick is `circulationFraction x conductance`. No circulation means no
conductance — a blackout cannot push the loop's heat into the station or
pull the station's heat into the loop for free. The exchange itself is
the implicit star solve of the previous section, not a tick-start rate:
a stiff exchanger on a small loop cannot overshoot the equilibrium the
loop shares with its reservoir. The reservoir books the exact negation
of the loop's exchange as its own ledger term, so the crossing is one
exchange accounted once on each side, and the loop's energy ledger —
starting + pump work + heater + exchanged = capacity x temperature —
closes within the declared tolerance every tick, in host and sandbox
alike.

Insufficient coolant or electricity reduces the actual service and
the shortfall stays visible: per-loop inventory and fill fraction,
pump demand/supplied/unmet, circulation fraction against rated flow
with unmet flow, heater demand/supplied/unmet, the loop temperature
and an exposed below-setpoint freeze condition (reported, never
clamped), and the closing loop ledger. Circulation loss and recovery,
freeze exposure onset and clearance, and whole-service refusal are
announced as transition events. The station snapshot publishes coolant
pump and freeze-heater power beside the other power totals and total
circulation and freeze heating beside the thermal totals.

World schema 73 persists the authored loop contracts (units: kg,
kJ/kg·K, °C, kW, kg/s, kW/K); a pre-73 document carrying
`coolantLoops` is refused rather than silently dropped. Coolant
inventory, circulation, freeze exposure and the loop energy ledger
stay runtime-only: Load and Reset reconstruct them from the authored
starting conditions. The separate Station Systems window drafts loops
(hazard class, host reservoir, coolant capacity and charge, specific
heat, temperatures, pump, rated flow, conductance and heater) with
full validation reporting, and shows the live inventories,
circulation, freeze service and closing ledgers beside the existing
views.

### Chilled thermal services (#397)

Authors configure chillers per World. A chiller supplies a cold service
of its declared thermal service class (`low`, `moderate`, `warm` or
`generation`) through two class-matched interfaces: a coolant-loop
evaporator (the cold side) and a thermal-reservoir condenser (the hot
side). The chiller's class must match the class of the reservoir its
cold loop serves and the class of the condenser sink; rejecting into
the same reservoir the cold side cools is refused as a heater, not a
cooling service, and one evaporator loop serves at most one chiller.
Each chiller declares a rated cooling capacity (kW cold service), a
COP, a running auxiliary draw (kW), a chilled setpoint (°C) and a
maximum condensing temperature (°C). Zero rated capacity disables the
chiller.

The delivered duty is deterministic. The demand is the rated capacity
while the cold loop is above the chilled setpoint and nothing at or
below it. The duty the machine can actually deliver is capped by the
cold loop's actual circulation fraction — heat must reach the
evaporator, so a loop that is not moving yields no extractable heat
and the service is refused whole rather than buying idle power. The
chiller is refused whole while the condenser sink sits at or above the
maximum condensing temperature: the hot-side interface cannot accept
the heat, the chiller draws no power and books no heat, and the entire
demand stays visible as unmet cold service. The electrical request —
work at the configured COP plus the running auxiliaries for the
deliverable duty — is drawn from the same generation-and-battery pool
as external loads and coolant pumps, never a second budget. A
curtailed grant duty-cycles the whole machine at one delivered
fraction, so extracted heat, work and condenser heat all scale
together.

Condenser heat equals extracted heat plus the work and auxiliaries
actually supplied — `condenser = cooling + supplied` — booked once at
one consistent boundary. The extraction leaves the coolant loop
exactly once (it appears in the loop's ledger as chiller extraction),
and the sink gains it through the condenser term, so the heat is never
double-counted: the sink's whole energy gain equals the chiller's
extracted energy plus its supplied work energy. The chiller's ledger —
extracted + work = condenser — closes within the declared tolerance
every tick, in host and sandbox alike, and a residual beyond it fails
the tick as a conservation violation rather than a silent rounding.

Chillers run between the coolant-loop allocation and the loop's own
integration: the tick's circulation is known when the chiller sizes
its duty, and the extraction lands inside the loop's temperature
integration. Within a tick, power allocates first, then coolant loops
and chillers run, then thermal integrates, then transfers, then
conversions.

The snapshot publishes per-chiller status (full/partial/refused/idle),
cold duty against demand with the unmet remainder, the
circulation-capped deliverable duty and the loop's circulation
fraction, power demand/supplied with the delivered fraction, the
condenser heat rate, the chilled-setpoint and sink-over-temperature
conditions, and the closing extracted/work/condenser ledger. A
per-class cold-service rollup keeps the account segregated: capacity,
provided, unmet, work and condenser per thermal service class, so a
class with no chiller reports nothing and one class's service is never
subsidized by another. Refusal, gating and recovery are announced as
transition events.

World schema 74 persists the authored chiller contracts (units: kW,
°C); a pre-74 document carrying `chillers` is refused rather than
silently dropped. Extracted heat, work, condenser bookings and the
gating conditions stay runtime-only: Load and Reset reconstruct them
from the authored starting conditions. The separate Station Systems
window drafts chillers (class, cold loop, condenser sink, rated
capacity, COP, auxiliaries, chilled setpoint and maximum condensing
temperature) with full validation reporting, and shows the live
duties, power draws, condenser heat, per-class totals and closing
ledgers beside the existing views.

### Fuelled generators (#399)

Authors configure generators per World. A generator converts a finite
fuel inventory into dispatchable electrical output at a declared
efficiency. The fuel is an energy-unit Resource store (`kJ`, `MJ`,
`Wh` or `kWh`): the generator draws qualified fuel above the store's
protected reserve, so fuel is finite, resupplyable through the store's
own boundary, and closes in the same energy terms as the electrical
output. Each generator declares its fuel store, a thermal service class
for its waste heat, a class-matched heat-sink reservoir, an installed
capacity (kW), a conversion efficiency in (0, 1], a ramp limit
(kW per second), a commanded output within the installed capacity, a
startup power and duration, and a shutdown heat term (kW and seconds).
Zero installed capacity disables the source.

The machine is a contract, not a power setting. Output is bounded by
the installed rating, the ramp limit and the fuel actually drawable:
no tick commands a step the installed slew rate cannot make, and a
generator with no fuel left cannot run at all. Starting requires
every precondition at once — qualified cooling (the sink below the
maximum sink temperature), drawable fuel, and a shared pool that can
actually supply the cranking draw. A start is never self-funded: a
stopped generator produces nothing, so its own output can never
appear in the pool that starts it. Startup completes only once the
startup energy (power x duration) has actually been received from
the shared pool while cooling stays qualified; cooling lost mid-crank
fails the start whole, with the cranking energy already spent booked
as heat in the sink. While running, waste heat is exactly the fuel
energy the conversion did not deliver as electricity —
`waste = fuel - electrical` — and the generator's electrical output
joins the same shared generation pool the loads allocate against,
never a second budget.

After output stops — commanded off, cooling lost, or fuel depleted —
the configured shutdown heat keeps flowing into the sink for the
configured decay period and the cooling requirement stays exposed
with it: a stopped generator is not instantly a cold generator. The
decay heat cannot be refused; it books whether or not cooling is
available, and the sink warms under it. A zero decay period books
nothing. The generator's ledger — `fuel = electrical + waste` —
closes within the declared tolerance every tick, in host and sandbox
alike, and startup dissipation and decay heat route exactly once
into the sink reservoir's own energy ledger.

Generators run ahead of the load allocation: their output is in the
pool the loads divide this tick, and their startup draw rides the
same pool ahead of discretionary demand. Within a tick, generators
produce first, then power allocates, then coolant loops and chillers
run, then thermal integrates, then transfers, then conversions.

The snapshot publishes per-generator phase (stopped/starting/running/
shutdown), status (full/partial/rejected/idle), output against the
commanded point and the installed rating, the fuel draw (kWh) and
waste heat rate, the startup draw against the startup profile with
received-versus-required energy, the decay heat rate, the sink
temperature against the maximum with the cooling qualified/
required flags, the block reason while a commanded start is held off,
and the closing fuel/electrical/waste/startup/decay ledger. The
power totals make the fuelled share explicit beside the solar share.
Phase transitions — starts, failed starts, trips, fuel depletion and
the end of the decay period — are announced as transition events.

World schema 75 persists the authored generator contracts (units:
kW, kWh, °C, seconds); a pre-75 document carrying `generators` is
refused rather than silently dropped. Phase, output, fuel draws and
the generator ledgers stay runtime-only: Load and Reset reconstruct
them from the authored starting conditions. The separate Station
Systems window drafts generators (fuel store, class, heat sink,
rated output, efficiency, ramp, commanded output, startup profile,
shutdown heat and maximum sink temperature) with full validation
reporting, and shows the live phases, outputs, fuel and heat flows
and closing ledgers beside the existing views.

This models the authored heat and energy contract of a fuelled
source. It claims no reactor physics and no nuclear qualification:
there is no criticality, no decay-heat curve derived from fuel
history, and no isotope inventory — the shutdown heat is an authored
constant for an authored period, nothing more.

### Compartment atmospheres and airlock cycles (#400)

A compartment atmosphere is an authored fixed-volume gas inventory:
a volume in m³, starting masses of oxygen, nitrogen, carbon dioxide
and water vapour in kg, a starting temperature in °C, an effective
thermal capacity in kJ/K covering the air plus the bonded structure,
and an optional heat-only coupling to a thermal reservoir through an
authored exchanger conductance in kW/K. Hazard class is `normal` or
`hazardous`; it is a declared label for authoring intent and
reporting, not an enforcement mechanism — nothing in this slice
opens, seals or interlocks a compartment on it.

Pressure is derived, never simulated as a control target: each
gas's partial pressure follows the ideal-gas law, p = (m / M) · R · T /
V in kPa, with molar masses oxygen 0.0319988, nitrogen 0.0280134,
carbon dioxide 0.0440095 and water vapour 0.0180153 kg/mol, and
R = 8.314462618 J/(mol·K). Absolute pressure is the sum of the
partial pressures (Dalton). Humidity follows the Magnus saturation
curve, p_sat = 0.61094 · exp(17.625·t / (t + 243.04)) kPa; vapour
above saturation condenses within the tick, the condensate leaves
the vapour inventory (reported as condensed mass, not destroyed)
and releases 2257 kJ/kg of latent heat into the compartment, so a
compartment cannot sit above saturation and relative humidity never
exceeds 100 %.

Occupancy is an authored load assignment — a count plus per-
occupant rates for oxygen use, carbon dioxide production, sensible
heat in kW and moisture addition, all in kg/s. The oxygen draw is
capped by the oxygen actually present: a starved compartment
reports the unmet demand, and carbon dioxide production and
metabolic heat scale only with the oxygen actually consumed, so a
compartment cannot breathe out more than it breathed in. Leakage is
a constant authored total-mass rate in kg/s, capped by the
inventory and removed proportionally to the current composition.
Scheduled airlock cycles reuse the delivery-style schedule (first
occurrence, optional recurrence, priority) and remove an authored
gas mass per occurrence from one compartment, split by an authored
recovered fraction: the recovered share is retained in the
receiver, the remainder is lost through the outer hatch. Every gram
leaving a compartment is booked as leaked, recovered or lost —
there is no unexplained disposal, and no process moves gas between
compartments automatically: normal and hazardous atmospheres are
segregated by construction.

Thermal state integrates the compartment's own heat sources —
occupant sensible heat, condensation latent heat and the heat-only
exchange with the coupled reservoir — into the authored effective
capacity on the fixed tick. The exchange is the implicit star solve of
the thermal section rather than a tick-start rate, so a stiff exchanger
cannot drive the air past the equilibrium it shares with its reservoir
into a negative absolute pressure (#559). The reservoir books the exact
negation of the exchange as its own atmosphere term, so both ledgers
close against each other. Declared limits (minimum and maximum absolute
pressure, minimum oxygen partial pressure, maximum carbon dioxide
partial pressure) are reported as conditions, never clamped: the
compartment keeps integrating and the ledger keeps closing while an
excursion stands.

World schema 76 persists the authored compartment and airlock-cycle
contracts (units: m³, kg, °C, kJ/K, kW/K, kg/s, seconds); a
pre-76 document carrying `atmospheres` or `airlockCycles` is
refused rather than silently dropped. Current gas inventories,
derived pressures, humidity and the containment ledgers stay
runtime-only: Load and Reset reconstruct them from the authored
starting conditions. The separate Station Systems window drafts
compartments (volume, starting gas masses, temperature, thermal
capacity, reservoir coupling, occupancy loads, leak rate and
declared limits) and airlock cycles (target compartment, gas mass,
schedule and recovered fraction) with full validation reporting,
and shows the live pressures, composition, humidity, heat flows,
closing ledgers and condition flags beside the existing views.

This models the authored gas and heat contract of a sealed volume.
It claims no life-support qualification: the occupancy rates are
authored constants, not physiology, and the derived conditions are
observations, not alarms or interlocks. Completed tick counts are
observability, not endurance estimates.

Atmosphere acceptance coverage: refused configurations (unknown
reservoir coupling, unknown hazard class, zero volume, negative
starting mass, sub-absolute-zero temperature, zero thermal
capacity, inverted pressure limits, negative occupant count or
leak rate, airlock cycles on unknown compartments with zero gas
mass, recovered fraction outside [0, 1] or recurrence faster than
one fixed tick, over-budget compartment count); analytic mixtures
where every partial pressure matches the ideal-gas law and the
absolute pressure equals the sum of the partials; leakage at the
authored rate with composition preserved proportionally and the
per-component ledger closing; a leak larger than the inventory
draining the compartment to empty and never below; zero/low
inventory with the oxygen draw capped, the shortfall visible as
unmet demand and CO2 production never exceeding oxygen consumed;
the vapour constraint holding at saturation with excess condensing
and its latent heat warming the compartment; compartment isolation
(a neighbour untouched by another's leak or cycle); deterministic
scheduled airlock losses with the recovered-versus-lost split, a
capped partial cycle, a refusal on an empty compartment, and
identical reset replay of snapshots and events; heat-only reservoir
coupling where the reservoir books the exact negation of the
compartment exchange and an uncoupled compartment exchanges
nothing; save/load roundtrip reproduction through YAML and binary
documents with pre-76 documents and malformed compartment or cycle
values refused at load; and intact Lua atmosphere and airlock-cycle
descriptor crossings with contained construction refusal.

## Explicit scope limit

This slice implements lifecycle, authored finite store configuration,
bounded transfers between stores, compound-resource composition and
conversion accounting, solar-supplied external equipment load
allocation, finite battery storage with charge/discharge losses,
authored scheduled resupply with refused-quantity accounting,
thermal reservoirs with environment-dependent radiator rejection and
exactly-once equipment heat routing, coolant loops with finite
loop-specific inventories, inventory- and power-capped circulation and
freeze-protection heating, fuelled generators with finite
energy-unit fuel inventories, ramp-limited dispatchable output,
startup-energy gating and persistent shutdown decay heat, and
fixed-volume compartment atmospheres with ideal-gas pressure
derivation, occupancy and leak accounting, the vapour saturation
constraint with latent-heat release, scheduled airlock losses with
the recovered-versus-lost split, and heat-only reservoir coupling.
It still adds no crew demand, treatment/clearance
workflows or ejection, no dynamic coolant leak, replenishment or
loop-to-loop transfer (a loop's charge is its authored inventory and
nothing in this slice changes it), no powered or passive thermal-buffer
access beyond the authored reservoirs themselves, and no chilled
service distribution beyond the class-matched rollup (no per-consumer
cold-side metering and no cascade or multi-stage refrigeration).
Compartment atmospheres are closed authored volumes: this slice adds
no gas generation or processing (no O₂ generation, CO₂ scrubbing or
combustion), no flow or pressure-driven transfer between
compartments, and no door, hatch or airlock physics tied to
atmosphere state. Completed tick counts are
observability, not oxygen, heat or endurance estimates. It
invents no process coefficients beyond the authored demand, capacity
and profile values. ECLSS models, crew inputs, alarms,
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
descriptor crossing with contained construction refusal; and thermal
fixtures: refused reservoir/radiator/heat-sink configurations
(non-positive capacity, inverted or sub-absolute-zero temperatures,
starting temperature outside limits, unknown classes, unknown host or
sink reservoir, out-of-range emissivity/view factor, cross-class
routing and a sink on a cooling-service load), analytic linear heating
matching the closed form, exact single-tick Stefan-Boltzmann rejection
at the emitting temperature, monotonic cooling toward the sink that
never crosses it with rejection falling as the reservoir cools,
exactly-once heat routing (routed heat warms the reservoir by exactly
its energy and is never also exported, while sinkless loads keep direct
export), hot versus cold operating profiles, tick-by-tick and batched
updates integrating identically, overheat and overcool limit crossings
reported without clamping, and intact Lua thermal descriptor crossing
with contained construction refusal; and coolant fixtures: refused
loop configurations (unknown host reservoir, unknown hazard class,
non-positive capacity or specific heat, over-filled charge,
sub-absolute-zero setpoint, over-budget loop count), full-service
circulation with pump-work warming and exactly-once exchange whose
combined loop-plus-reservoir gain equals the pump work energy, the
single-pool audit covering loop draws, power-limited circulation with
the unmet flow visible, blackout with zero invented flow and the whole
freeze-heating demand visible as unmet alongside the exposed
below-setpoint condition, coolant-loss fixtures (a dry loop disabled
and inert under ample power, a half-filled loop capped by its
inventory fraction), heater-capacity warming matching the closed form
with the thermostat dropping demand at the setpoint and no heat
crossing without circulation, isolated normal/hazardous inventories
with no cross-subsidy and the reservoir loop term equal to only the
circulating loop's negated exchange, battery ride-through freeze
heating with both ledgers closed, loop-draw-before-charging ordering
with the battery taking exactly the leftover surplus, eclipse
circulation-loss and recovery announcements, deterministic Reset
replay, and intact Lua coolant descriptor crossing with contained
construction refusal), real file
save/reopen round trips in
YAML and binary including the solar/load, battery and thermal scenario
reproduction and
pre-69/pre-70/pre-71/pre-72/pre-73/pre-74/malformed refusals, failed-write and
legacy/malformed-file
handling, paused structural rebuild preservation,
and the existing CPU ImGui and document-history seams. Run `ctest -R
'^station-'` in Release.

Chiller acceptance coverage: refused chiller configurations (unknown
cold loop or condenser sink, cross-class cold-side or condenser
coupling, rejection into the cold side's own reservoir, zero COP,
negative rated capacity, chilled setpoint at or above the maximum
condensing temperature, sub-absolute-zero setpoint, shared evaporator
loop, over-budget chiller count); nominal full service with the
work-plus-auxiliaries COP contract, condenser heat equal to extracted
plus supplied work booked exactly once (the sink's gain equals the
chiller's extracted plus work energy), the loop cooled and the sink
warmed with all three ledgers closed, and the per-class rollup
carrying the whole account with empty classes empty; electricity-
limited cooling duty-cycled at one delivered fraction with the unmet
remainder visible on both the cold service and the power draw;
sink-limited refusal whole at or above the maximum condensing
temperature with no power draw, no condenser booking and the entire
demand visible as unmet; zero-capacity disablement; the chilled-
setpoint gate demanding nothing at or below the setpoint; circulation
loss refusing the service with the no-circulation reason and the whole
demand unmet; transient supply loss through an eclipse cycle with the
unmet demand visible during the outage, recovery announced and ledgers
closed across the transitions; deterministic Reset replay; and intact
Lua chiller descriptor crossing with contained construction refusal.

Generator acceptance coverage: refused generator configurations
(unknown or non-energy fuel store, unknown or cross-class heat sink,
negative rated capacity, zero or over-one efficiency, zero ramp
limit, commanded output above the installed rating, negative startup
power, sub-absolute-zero maximum sink temperature, over-budget
generator count); nominal full service with completed startup on
energy actually received from the shared pool, ramp-limited output
to the commanded point, waste heat equal to fuel minus electrical
booked exactly once into the sink (the sink's gain equals the
generator's startup dissipation plus waste), the fuel store's own
boundary ledger closing unit-for-unit, and the fuelled share visible
in the power totals; zero-capacity disablement; a commanded start
with no pool support held off with the no-startup-energy reason and
nothing burned; a start blocked by an unqualified (at or above
maximum) sink; cooling lost mid-crank failing the start whole with
the spent cranking energy booked as sink heat; cooling lost while
running tripping to shutdown with output stopped immediately and the
decay heat persisting with its cooling requirement exposed without
cooling, ending on schedule; fuel depletion stopping output
immediately at the protected reserve with the reserve untouched and
the ledger closed; the ramp limit holding output to the installed
slew rate partway to the commanded point; deterministic Reset
replay; and intact Lua generator descriptor crossing with contained
construction refusal.
