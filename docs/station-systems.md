# Station systems — lifecycle, authored scenarios, containment, resource stores, transfers, compound resources, solar-supplied external loads, batteries, scheduled deliveries, terminal-waste storage and scheduled ejection, thermal reservoirs with radiator rejection, coolant loops with freeze-protection heating, chilled thermal services, fuelled generators, compartment atmospheres with airlock cycles, crew metabolic returns, water recovery with qualification, oxygen production with finite CO2 capture, finite thermal buffers that ride through cooling deficits, protected cross-system service dependencies (#386–#391, #393–#404, #406, #412, #416) and bounded scenario forecasting of resource balance and resupply needs (#420)

Station systems are opt-in per World and disabled in existing documents. The
main **View** window shows the **World** tab by default. Toggle
**View > Station Systems** or the **Station Systems** toolbar button to show
or hide the **Station Systems** and **Station Flow** tabs together. Showing
them selects Station Systems; hiding either active station view returns to World. The menu checkmark and toolbar state indicate tab availability,
even when World is selected. **View > World** selects the World tab without
hiding Station Systems. Switching or hiding tabs does not stop simulation
or discard a Station Systems configuration draft. It reports the active World, pause/run
status, World and station elapsed simulation seconds, all four system objects'
completed ticks, bounded events and structured software diagnostics.

## Draft flow diagram

The **Station Flow** tab beside **Station Systems** automatically generates a
**force-directed** connection diagram from the **current draft**,
including unapplied edits and loaded presets. Connection springs pull related
nodes together; repulsion and box-collision separation keep labels apart.
The deterministic, bounded layout is calculated, cached, and automatically
framed to the visible canvas when the draft changes. Left-dragging a node
reheats the force solver: the node follows the pointer while its neighbours
and connections adjust in realtime, then the layout cools and settles after
release. It is available even when systems are inactive. Moving nodes changes
only their session-local diagram positions, never the draft, authored settings,
World simulation, or document history.

- Nodes represent stores, processes, atmosphere compartments, thermal equipment,
  power equipment, and scheduled deliveries/ejections/outages/maintenance.
  Each type has a stable colour independent of its ID; expand **Node type colours**
  for the legend. Unresolved references remain red.
- Arrows are coloured by **material or service**, not just by a generic material
  category: water/vapour, O2, CO2, H2, methane, solid carbon, food, organic waste,
  urine, nutrients, consumables, fuel, mixed atmospheric gas, heat, cooling,
  electricity and control each have a stable colour. Expand **Edge colours**
  for the legend of flows present; tooltips and the connection inspector name
  each edge's material/service. Parallel exchanges use separate curves.
- Material colours come from explicit process input/output roles. Transfers,
  deliveries and other generic store connections inherit a material only when
  their connected process roles identify it unambiguously. Mixed, conflicting
  or unknown materials stay neutral; store IDs are never interpreted as chemistry.
  Atmosphere exchanges show O2, CO2, moisture and heat separately. Shared electrical
  and cooling-class nodes represent aggregate services, not physical wiring or pipes.
- Missing or ambiguous referenced IDs appear as red nodes, so an incomplete draft
  remains inspectable. This is not a substitute for Apply's configuration validation.
- Click a node for its incoming/outgoing connections, or hover for a tooltip.
  Left-drag it to rearrange the graph (also while the World is paused).
  **Connected only** focuses its immediate neighbours. **Find node** filters by
  type/ID (case-sensitive) and moves the view to the first match.
- Right- or middle-drag to pan; use the mouse wheel to zoom. **Fit all** stops
  relaxation and frames the entire rearranged graph; **Reset view** also clears
  the node selection and reframes the
  graph, including after panning it off-screen.

This remains a configuration topology, not a schedule timeline,
capacity/admission prediction, or a physical distribution network. Ambient losses
and implicit process internals are not expanded into individual nodes. Changing
Worlds starts a fresh diagram and draft; switching/hiding tabs retains the draft.

### Physical connection types

Connection **colour identifies the medium** (water, oxygen, electricity, etc.);
its **line pattern/marker identifies the physical carrier**. Expand **Connection
types** for the visual legend, hover a curve for its classification, or click it
for the **Connection properties** inspector.

| Flow / relationship | Automatic type | Line style |
| --- | --- | --- |
| Electrical power | Power cable | Solid with square marker |
| Water or urine | Liquid pipe | Double solid |
| Oxygen, CO2, hydrogen, methane | Gas pipe | Dashed |
| Compartment air extraction / water vapour collection | Air duct | Heavy dashed with square marker |
| Heat exchange involving a coolant loop | Coolant circuit | Double dashed (supply/return) |
| Food or solid carbon | Material handling | Dash-dot with square marker |
| Other physical heat exchange | Thermal connection | Dash-dot |
| Fuel, nutrients, consumables, organic waste or unknown material phase | Unspecified | Long dashed |
| Cooling-service allocations, controls, schedules, circulation gates, delivery/waste-ejection and airlock events, maintenance, outages, ambient breathing/photosynthesis | Non-physical | Fine dotted |

Ambiguous flows can be classified explicitly per connection; phase is never
guessed from a Store's name. A **Data cable** override is also available, shown as
dots with a diamond marker, but scheduling dependencies do not automatically
become data cables. A coolant circuit symbol describes supply/return plumbing;
it does not introduce another simulated flow or coolant inventory.

Each connection has its own saved properties. A positive, finite **Maximum
throughput** defaults to **1e12** in the displayed rate unit: kW for electrical or
thermal transport, a Store's native unit per second for its material connections,
and kg/s for other material flows. Every connection also has **Efficiency**,
default **1.0**, accepting finite values from **0 to 1 inclusive**. Older entries
omitting efficiency load lossless. Both properties are saved and undoable.

Applied physical material, electrical, and thermal connections are enforced by
the authoritative allocator: **cap connection input first, then apply efficiency**.
An input cap of 10 kg/s and efficiency 0.9 delivers at most 9 kg/s. Receiver demand
is compensated upstream within available supply and the input cap; existing
priority, protected inventory, and equipment limits still apply. At zero
efficiency, actively requested input is consumed entirely as loss; idle or blocked
demand does not create an unsolicited flow.

Undelivered input is an explicit **external connection loss**, accounted in the
native-unit/constituent material balances or electrical/thermal accounts. It is
not silently returned or deposited as waste heat/material elsewhere. Produced
heat that cannot enter a capped connection remains at its source and can drain
in later ticks: produced heat equals cumulative connection input plus retained
source heat, while connection input equals delivered heat plus transport loss.

Non-physical relationships in the table remain **informational only**, including
their efficiency values; they have no editable carrier or throughput field.
Carrier overrides change drawing style, not enforcement: choosing a Non-physical
style for a physical exchange does not disable its limits or losses. Automatic
power links remain automatic and cannot be disconnected manually. Draft edits do
not affect the running World until applied.
See [connection persistence and lifecycle](station-configuration-documents.md#connection-design-properties).

### Live node readouts

When the Configuration draft matches the applied, enabled Station systems,
node footers show one metric per line (up to five values), plus a status/limiting
reason. Supplied/requested quantities have separate lines. **Battery charging**
and **Battery discharging** are battery-side power on Battery nodes and pool-side
power on the shared electrical node; cable losses can make these readings differ.
The heading identifies the applied simulation, elapsed time, and running,
paused, or failed state. Snapshot polling is capped at four updates per second;
pause transitions, resets, configuration changes, and World switches refresh
immediately. Viewing readouts never advances or edits the simulation.

- Stores show qualified inventory/capacity, net flow, and unqualified inventory;
  batteries show charge, energy, and charge/discharge power.
- Transfers and conversions show actual throughput; equipment and processing
  nodes show delivered power/service, production, and shortages or blocking reasons.
- Generators show phase/output/startup progress, solar arrays available supply,
  and fuel cells output/reactant draw. Shared electrical and cooling services
  show supply versus demand and unmet demand.
- Thermal nodes show temperatures, heat exchange, cooling/circulation, or finite
  buffer absorption headroom. Atmospheres show temperature, humidity, pressure,
  and O2/CO2 partial pressures.
- Crews show supplied/requested food and water and oxygen demand. Biological
  populations show biomass/capacity, net biomass change (including harvest/loss),
  power, and limiting conditions; microbial processors also show culture condition.
- Scheduled nodes show time to the next event, last-event quantities, active
  outage capacity, or maintenance progress/remaining time.

Port labels include actual rates where the authoritative snapshot provides them.
Material rates use the simulation tick duration, **not** the display refresh
interval. Scheduled transfers are labelled **last event**, not treated as steady
flow. Non-zero magnitudes below `0.001` in the displayed unit read **trace**
(**-trace** for negative values); exact zero remains `0`. This also applies to
percentages and inspector details, without changing simulation values or warnings.
Unknown rates are omitted; available solar generation is potential supply, not
necessarily actual transmitted power. Physical port rates use the connection's
node-side endpoint: delivered input at a receiver, accepted connection input at a
sender. Reversed heat exchange has a signed rate relative to the displayed port.
Power ports remain automatically connected to the shared electrical pool, not
user-wirable references.

Status text is grey for idle/disabled, green for normal operation, amber for
limitations, and red for failed/unsafe conditions; numerical fluctuations alone
do not change node colours. Hover a node or expand **Applied simulation readout**
in its inspector for untruncated values and port rates. Available endurance
estimates are labelled **at current net rate (not a forecast)**.

**Unapplied draft changes hide all live readouts**, rather than attaching applied
values to edited or newly placed nodes. Apply in Station Systems, or revert the
draft, to restore them. Disabled systems show no live rates. Before the first
completed tick the footer says Ready and omits port rates; failed systems mark
values as the last committed state. Readouts and their refresh cache are never
saved in configuration documents or undo history.

### Live connection readouts

Connection tooltips and the connection inspector show **Input**, **Delivered**,
**Loss**, and cumulative **Total loss**, one metric per line, from the authoritative
transport snapshot. Input/delivery/loss are rates; cumulative quantities use the
native unit, or kJ for kW transport. Heat producers also show **Heat retained at
source**, distinct from connection loss; reversed exchanges are explicitly marked.
Nodes with retained producer heat warn that their heat connection is constrained.

These readouts follow the same applied-draft, pause, reset, failure, and trace
formatting rules as node telemetry. Before a completed tick they show Ready, not
invented flow; non-physical or unknown connection accounts do not fabricate zero
rates. Readouts are not persisted and never advance the simulation.

### Water and gas authoring (#649)

**Place node** offers **Water collector** and **Water treatment** under Water,
and **Electrolyzer**, **CO2 capture**, **Atmosphere**, and **Airlock cycle** under
Gas. Click the canvas to place an unwired entry; select it to edit the same
settings used by the Station Systems form. Placement, links and field edits
share draft undo/redo and do not alter the World until Apply.

A water recovery line connects an Atmosphere's **Gas out** and a Coolant loop's
**Cold surfaces** to a collector, its **Raw water out** to a Store, and that
Store to a treatment's **Raw water in**. Wire separate treated and concentrate
stores and the treatment consumable. Electrolyzer oxygen goes to either its
**Oxygen out** Store or its **Compartment oxygen** destination, never both.
CO2 capture draws from an Atmosphere into a finite Store. Airlock cycles draw
from an Atmosphere; Atmosphere heat coupling to a Reservoir is optional.

Unused optional connectors are hollow rings, including capture consumables,
and the alternative oxygen destinations. They become real links when wired.
Treatment qualifies output on complete energy evidence alone (#753).
Thermal-class, unknown-reference and
exclusive-destination refusals use the configuration validator's reasons.
Configuration documents preserve the resulting processing line and layout.

### Biology authoring (#650)

**Place node > Biology** offers **Crew**, **Crops**, **Algae**, and
**Microbial processor**. Place an unwired entry and select it to edit the same
settings as the Station Systems form. Placement, connections, disconnects and
field edits use draft undo/redo; only Apply changes the World.

Crew food, water, urine and organic waste connect to Stores. Crops and Algae
expose separate water and N/P/K/mineral inputs and harvest destinations; Algae
also exposes its recovery destination. Their gas, moisture and heat faces connect
to an Atmosphere's **Biology out/in** faces. These exchanges share one compartment
reference: wiring or disconnecting any face connects or clears the whole bundle,
so a population cannot accidentally draw oxygen from one compartment and return
CO2 to another. Electrical service remains implicit.

Microbial processors expose their material streams and Reservoir heat sink.
**Culture medium** and **Replacement stock** draw as unused optional connectors
until wired, and support ordinary link dragging and undo. Required process
resources must still be supplied before Apply. Link refusals use the shared
biology validator, including material compatibility, nutrient purity, distinct streams,
unknown stores or compartments, and unknown replacement biological stock.
Station configuration documents preserve the authored biological loop and layout.

### Chiller authoring (#659)

Choose **Thermal > Chiller** in **Place node**, then click the canvas. The
new Chiller starts with both interfaces unwired. Connect its **Cold loop**
input to a Coolant loop's **Cold interface** output and its **Condenser heat**
output to a Reservoir's **Heat in** input; either drag direction works.
Both interfaces must match the Chiller's Thermal service class, each cold loop
serves only one Chiller, and the condenser cannot be the cold loop's host.
These constraints also apply when reconnecting a referenced loop's host;
unused loops may still connect to any declared Reservoir.

The inspector and form share the Chiller settings editor. Placement, each
connection, disconnect, and committed field edit is individually undoable and
redoable. Refused links change neither the Configuration draft nor history,
even in incomplete drafts. Electrical and per-class cooling-service links remain
implicit, not user-authored. **Apply settings** remains the only World mutation.

## Authored settings and lifecycle

- **Activate Station systems**: global toggle at the top of the tab, default
  off for new Worlds. Inactive Worlds run no Station systems simulation and
  own no station runtime. Activation is saved with the World, independently
  of tab visibility. Pause before changing it; after any World ticks, changing
  activation requires explicit confirmation of a complete World reset.
  This toggle applies activation directly without applying other draft edits.
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
receives only a system name, the next Station step and step duration: no
World, Agent, filesystem, process, wall-clock or mutable domain objects.

The application loads `StationSystemsLifecycle` as a managed `LuaScript`
Resource from `Resources.yaml`. Headless use embeds that exact same asset,
following the bundled Human source pattern. Only immutable source is shared;
no executed Lua table, VM or upvalue is shared across objects or Worlds.

SimulationCoordinator calls `StationSystemsRuntime::advanceTo(seconds)` once
at each World tick boundary, supplying the target World elapsed seconds. The
runtime owns a count-derived Station clock: `systemsStepWorldTicks` (1–600,
by default 1) multiplies the embedding clock's base quantum supplied at
construction. No floating-point duration is accumulated. The runtime advances
whole steps until its elapsed seconds reach or exceed the target; calls while
already ahead perform no work. Consequently a non-boundary target can put the
Station ahead of the World by less than one Station step, by contract.
Existing time scale and update batching still feed the same fixed World ticks.
The Station snapshot derives elapsed seconds from committed Station steps.
Legacy `worldTick` and `completedTicks` fields count Station steps; at ratio 1
they retain their previous values. The four sandbox counters must each equal
the next Station step, independent of the number of drive calls.

The public `core/StationSystemsRuntime.h` also supports standalone scenarios:
construct a runtime with authored settings and a base quantum (seconds), call
`advanceTo(durationSeconds)`, then read `snapshot()`. No World, Agents or tick
pipeline are needed. Non-finite, negative and unrepresentable targets return
false without mutation; already-reached targets are successful no-ops.
Ratio 1 retains the original physics durations and elapsed-time arithmetic.
The clock seam (#763), scheduled-event integration (#767), continuous-transfer
saturation crossings (#769), rate-aware protection and ratio persistence are implemented.
The certified protection ratio is **6**: above it the runtime remains deterministic,
finite and conserving, but authors must not claim the certified admitted-work
protection margin. The `station-timestep-invariance` CTest label owns the fast
ratios 1 and 6 plus exhaustive ratios 30 and 600, the long-duration reference,
and the measured coarse-step performance gate. See [ADR 0025](adr/0025-station-systems-clock-and-timestep-certification.md)
and [the protection evidence](tickets/771-rate-aware-protection.md). Rendering never executes station scripts. Public World methods
`getStationSystemsSnapshot`, `getStationSystemsEvents` and
`getStationSystemsDiagnostics` return read-only value copies. Events retain at
most 256 entries; a failure is terminal until Reset, so diagnostics are bounded.

A failed script call, memory/instruction exhaustion, invalid return value or
Station-step-accounting mismatch pauses the World before that tick advances. All four
results are staged before publication; no partial station result or elapsed
time is published. Mutated Lua objects are discarded, and resuming cannot
retry the failed runtime. Reset reconstructs authored initial state.

### Scheduled-event integration (#767)

The authoritative ledger collects deliveries, Scheduled Resource transfers and
Terminal waste ejections due since its last commit using the existing ordered
occurrence list. Each distinct occurrence time splits the Station step. Material
is integrated up to that instant before the event applies; it is available only
to subsequent sub-intervals. Coincident events retain occurrence-time, descending
priority and declaration ordering, including the existing delivery-before-transfer-
before-ejection tie rule. Maintenance starts, window deadlines and active job
completions also split integration; progress derives from the exact start instant.
A sub-tick maintenance window with available prerequisites is serviced, not missed
merely because no full Station step ends inside it.

Continuous phases retain their existing order and sampled-profile endpoint
convention, and execute only over positive durations. Discrete boundary passes
never run rate-to-amount round trips, so coincident events cannot divide by zero.
Store flows and continuous process amounts accumulate across the whole Station
step; rates and statuses remain latest samples, while scheduled quantities retain
their last-occurrence meaning. Only the completed full step advances the Station
clock or samples history. Every sub-interval and boundary is audited, and any
failure rolls back the entire step, including earlier sub-interval inventories,
loss/inflow ledgers, allocations, connection accounts and event history.

The standalone `eventSplit`, `eventOrder`, `maintenanceSplit` and `eventRollback`
checks exercise exact-time integration at ratios 1, 6 and 30, faster-than-step
recurrence, coincident ordering, conservation and later-sub-interval failure.
Ratio-1 runs without newly due events keep the original integration path; the
intentional timing corrections also apply at ratio 1 (no consumption before an
arrival, and no missed maintenance solely because its window is sub-tick).

### Continuous-transfer saturation crossings (#769)

Continuous Resource transfers also split integration at predicted destination
capacity and source protected-reserve crossings. Prediction uses the authoritative
connection input caps and upstream loss compensation, including zero-efficiency
connections, rather than extrapolating an already-clamped step amount. Held
inventory consumes destination headroom but is never drawable source stock.
The ledger runs to the crossing, then re-evaluates admission for the remaining
interval. Authored events can interrupt or restart this process, and a failure
still rolls back the entire Station step.

Amounts accumulate across the full step; status describes the latest interval.
Thus a transfer that supplies fully until its receiver fills and then stalls
publishes Rejected, not an artificial Partial caused solely by the enclosing
step width. Connection-limited operation may still be Partial. The unsaturated
no-event path retains its original integration and accounting arithmetic.

The standalone `saturationCrossing` check compares analytic delivered and drawn
amounts and material conservation at every committed step boundary at ratios
1, 6 and 30, for capacity and reserve crossings, input/output caps, efficiency,
held receiver stock and all-loss flow. It also compares transition order across
ratios. This is not certification of the other Station subsystems; the remaining
#762 slices and invariance registry still govern that claim.

### Rate-aware coarse protection (#771)

Before committing a coarse step, the authoritative ledger predicts gross heating
and cooling excursions across every integration interval. Unsafe thermal
qualification is latched refused, predicted freeze crossings request heating,
and the trial allocation is restored and repeated until admission is stable.
Repeating matters because released power can make a different machine unsafe.
Only the stable audited allocation, schedules and event history are published;
ratio 1 retains its original single-pass sampled protection.

`core::StationProtectionBounds` declares the protected margins and their evidence.
The measured coarse protection ratio is **6**, covering each integer ratio 2--6.
Thermal qualification allows only the existing 1e-9 C association margin, and
inventory protection keeps its existing drawable/headroom caps. This is an
admitted-work guarantee, not a temperature/pressure clamp: environmental,
respiration, decay and retained heat continue to be accounted for even when work
is refused. Freeze setpoint protection requires sufficient real heater capacity
and electricity; shortages remain explicit protected Unmet demand. Reservoir and
atmosphere condition alarms retain their existing semantics. All-observable
convergence certification and editor presentation remain separate slices.

See [the measurement evidence and reproduction command](tickets/771-rate-aware-protection.md)
for the margin-tight thermal/biological fixtures, reallocation, gross-versus-net
excursion, freeze shortage and conservation checks.

### Failure containment (#388)

Diagnostics identify the attempted Station step (the World tick at ratio 1), failing system, bounded message
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
qualified (usable) and unqualified (held awaiting treatment)
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

### Scheduled Resource transfers (#690, #692)

In both Station Systems and the Station Flow Transfer inspector, choose
**Continuous** (the rate-based behavior above) or **Scheduled**. Scheduled
fields are `quantity` (positive, in store units), `firstOccurrenceSeconds`
(non-negative scenario time), and `recurrenceSeconds` (**zero** for one-off,
otherwise at least one World fixed tick, using the shared schedule tolerance).
Both editors expose recurrence with independent draft undo/redo.
Source, destination, unit, priority,
compatibility rules and the combined 32-transfer limit are shared. Incomplete
wiring stays visible in drafts; Apply requires valid complete wiring. Placement,
wiring and field changes use Configuration draft undo/redo and do not change
the World until paused Apply (with reset confirmation for an existing run).

A Scheduled transfer is an instantaneous, lossless internal inventory event,
not an Agent hauling task or a physical carrier. It attempts its quantity once,
moving only qualified source stock above protected reserves and existing
maintenance reservations, within destination capacity after held material and
maintenance waste claims. Partial or zero movement **completes the occurrence**:
unmet stock stays at source, with no retry or carry-over. Every recurrence
requests the original quantity independently, including after blocked occurrences.
Schedules derive each occurrence from first time plus occurrence index times
interval, never a floating-point countdown. Reservations for jobs
starting later in the tick use the inventory left by the scenario-event pass.

Before continuous processing, due events sort by occurrence time, priority
(highest first), then **Delivery → Scheduled transfer → Waste ejection**, then
declaration order within each kind. Chains forward newly arrived stock only
when that order permits: there is no repeated settling pass. Time-zero events
run on the first committed tick, not during Apply. Pause freezes time; batching
and time scaling preserve outcomes for the same committed ticks. Reset replays
the authored starting inventories and schedule. A failed tick publishes neither
movement nor advanced occurrence accounting; per-unit and constituent audits
count no external import or loss for this internal move.

Scheduled links are **Non-physical**. Carrier and throughput are not editable;
retained connection design properties, including efficiency, are informational
and do not cap or lose batch material. The Scheduled connection inspector shows
saved efficiency read-only; switch to Continuous to edit the saved design
properties. Explicit property reset remains available and undoable.
Both views show next occurrence and occurrence count, distinguish completed
one-offs from recurring transfers waiting between occurrences, and retain last
requested/moved/unmet **amounts** and limiting reasons, not instantaneous rates. The bounded
event log reports each occurrence. Scheduled batches do not appear in the
continuous exchange-rate charts; inventory charts still show their inventory
changes. Both views also expose cumulative requested/moved/unmet amounts (#693).
These runtime totals survive event-log rollover and satisfy requested = moved +
unmet, including zero-movement occurrences. Waiting is schedule state, not a
rejection: the last full, partial or blocked outcome and its limiting reason remain
visible between occurrences without counting idle ticks as new requests. Each
occurrence emits one bounded event in store units; no idle-tick events are emitted.
Failed ticks publish neither occurrence counters nor totals/history changes.
Load/Reset reconstruct accounting from authored starting state; runtime accounting
and next-occurrence state are never saved. Reading snapshots or rendering either
view does not execute occurrences.

World and standalone Station configuration schema **99** preserve mode and
schedule fields (including recurrence); missing mode defaults to Continuous. Older declared schemas
carrying the new fields are refused rather than silently discarding semantics.
Non-finite supplied numbers are refused even for inactive mode fields. Affected
material-connected Resource forecasts explicitly report Scheduled-transfer
unavailability; independent projections remain available. Exact Scheduled
forecasting is not implemented.

The automated pantry/Crew integration scenario replenishes an empty pantry from
bulk food, consumes continuously between events, depletes before the next event,
and recovers on the next replenishment. Shortages remain ordinary Unmet demand,
not software failures. Focused checks cover minimum and non-tick-aligned
recurrence, time-zero/boundary tolerance, repeated competition, pause, batching,
time scaling, atomic invalid interval refusal, and deterministic Reset/load replay.

#### Switching existing transfers (#691)

Change **mode** in either editor to switch an existing transfer. Only the active
mode's fields are shown and only that mode executes after Apply. The hidden rate
or schedule is retained, including finite values outside the inactive mode's
valid range; switching back exposes those values for correction. Both editors
show the same active-mode validation, and invalid Apply/load is refused. Supplied
malformed or non-finite numbers are always rejected, even when inactive.

Switching preserves connection identity, carrier overrides, throughput and
efficiency. Returning to Continuous restores the saved physical input caps and
losses, not default ratings. Rename, World save/load and standalone configuration
save/load preserve those settings in either mode. Ordinary disconnect or deletion
still removes the affected design properties; undo restores them. Mode changes,
field edits and property visibility follow Configuration draft undo/redo, remain
isolated from the running World, and use the normal paused Apply/reset confirmation.

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

**Actual biological deposits and downstream use (#601).**

A compound store's authored composition describes its starting stock and
external deliveries, not a relabelling rule for biological deposits.
Crop and algae harvests and starvation residue deliver their **actual**
N/P/K/mineral/organic inventories and remaining chemical energy. Respiration
removes organic matter and energy without consuming the bound nutrients;
the receiving store mixes that enriched material with its existing stock.
`StationSystemsStoreSnapshot::actualConstituents` exposes the resulting
inventory (kg for constituents, kJ for `energy`; empty for plain stores).
Every draw spends a proportional share of that blend, and transfers and
maintenance waste routing carry the blend with the material.

Microbial nutrient recovery uses the actual feed composition. If the fixed
digestion outputs would exceed the inputs, or reaction heat would exceed
the feed's remaining chemical energy, the processor holds the feed and
reports a rejection reason before consuming material or power. Likewise,
fixed-yield conversions and crew metabolism hold incompatible feed rather
than creating unfunded constituents or metabolic heat through a negative
loss counter. These runtime inventories and holds are mirrored in Lua;
Load and Reset reconstruct stock from authored starting conditions.

`station-nutrientConservation` covers receiving-store inventories, partial
harvest and overflow, mixed crop/algae deposits, transfers into downstream
recovery, competing external consumers, and incompatible conversion/heat
holds through public World ticks and snapshots.

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

The same containment now covers the atmosphere and thermal publish
paths (#565). The snapshot derives compartment partial pressures,
absolute pressure and saturation vapour capacity, and publishes
station-wide thermal rollups (absorbed/routed/rejected heat,
circulation, freeze heating, pump power, cooling provided/unmet,
condenser heat, generator output/startup/waste/decay) and one
cold-service account per thermal class; every one of these derived
aggregates is audited finite in the host ledger and in the bundled
Lua mirror before the tick may commit. Two finite 1e308 kW chiller
ratings in one class overflow the class capacity even with both
machines gated off, and two finite 1e308 kg/s loop flows overflow
the circulation total; both now fail the tick whole. Authored
compartment states that are inconsistent from the start — a starting
gas inventory whose ideal-gas pressure in the declared volume is
already infinite — are refused transactionally at configuration:
no runtime is constructed and the World is not mutated. The Magnus
saturation model's documented domain is strictly above -243.04 C,
where the expression is singular; at or below it the model carries
a finite continuation of zero vapour capacity rather than
publishing an overflow as a shortage.

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
loop-specific coolant inventory (kg) and carries no class (#757):
coolant is a thermal service fluid, never potable or nutrient water.
The loop is coupled heat-only to exactly one
thermal reservoir: the loop's coolant never crosses into the reservoir,
never touches a Resource store. Each loop
owns its finite inventory by construction. Only heat crosses its exchanger;
no class-matching or segregation rule is needed or possible.
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
`coolantLoops` is refused rather than silently dropped. The loop
class field is retired (#757): documents carrying it at any
version are refused with a named-field conversion-required diagnostic,
never silently defaulted. Coolant
inventory, circulation, freeze exposure and the loop energy ledger
stay runtime-only: Load and Reset reconstruct them from the authored
starting conditions. The separate Station Systems window drafts loops
(host reservoir, coolant capacity and charge, specific
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
and the service is refused whole rather than buying idle power —
and by the heat the coolant actually holds (#560): a nameplate
rating is not a heat source. Over the tick the loop can give up only
the heat its own thermal mass holds above the chilled setpoint —
the cold-side interface the machine exists to hold — plus the
heat legitimately entering it this tick: pump work, freeze-heater
heat and the exchanger's implicit-solve share, each counted exactly
once. Extracting more than that would end the tick below the
setpoint, so the loop may be cooled to the interface and no further;
the setpoint is validated above absolute zero, so no capacity/rating
combination can drive the published coolant temperature below it.
This is bounded integration, not clamping: the capped duty, its work
at the configured COP, its auxiliaries and the condenser booking
all scale together off the heat that actually moves, the shortfall
against the nameplate stays visible as unmet cold service, and a
loop whose exchanger is already draining it past the setpoint funds
no evaporator duty at all — such a chiller is refused whole with
the no-extractable-heat reason. The
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
integration: the tick's circulation and the exchanger's implicit-
solve share are both known when the chiller sizes its finite
cold-side bound, and the extraction lands inside the loop's
temperature integration. Within a tick, power allocates first, then coolant loops
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

### Finite thermal buffers (#398)

Authors configure thermal buffers per World. A thermal buffer is a
finite store of thermal energy — a phase-change mass, in effect — that
rides a host reservoir through a cooling deficit: it absorbs the heat
the rejection path cannot take, reaches its limit, and gives the energy
back when rejection becomes available again. The buffer's state is
stored energy, never temperature: its temperature is *derived* from the
stored fraction across a declared usable band, so a partly-full store
sits at an intermediate temperature and the direction of heat flow is
set by comparing that derived temperature to the host's.

Each buffer declares a host reservoir (whose deficit it absorbs), an
access mode, a finite capacity (kJ) with a starting energy (kJ) within
it, a usable temperature band (min/max °C), separate maximum charge and
discharge rates (kW), and — for a passive buffer — a transfer
conductance (kW/K). Validation refuses an unknown host reservoir, an
undeclared access mode, a circulation buffer with no declared loop or a
loop coupled to a different reservoir, a non-positive capacity, a
starting energy outside `[0, capacity]`, an inverted band, a buffer that
can neither charge nor discharge, and a passive buffer without a
positive transfer conductance.

Charging and discharge are thermally honest and ride the *running*
imbalance left after rejection. When the host has a deficit
(`heat_in − rejected > 0`) and is hotter than the buffer, heat flows
into the store, capped by the remaining deficit, the configured charge
rate, the store's headroom, and — for a passive buffer — the conductance
across the temperature difference. When the host has a rejection surplus
(`heat_in − rejected < 0`) and the buffer is hotter, stored heat returns
into the host, capped by the remaining surplus, the discharge rate, the
stored energy and the conductance. Because discharge is bounded by the
surplus, recovery consumes real rejection capacity: the store can only
reset as fast as the rejection path can carry the heat away, never
instantly. Buffers on one host run in declaration order and the running
balance carries through them, so two buffers never absorb the same
deficit twice. A `circulation` buffer is a powered-only store reachable
only while its gating coolant loop is actually circulating, so a blackout
cannot absorb through it for free; a `passive` buffer is reachable at any
time through its declared conductance. The store is clamped to
`[0, capacity]` so roundoff never accumulates past the finite bounds.

The snapshot publishes per-buffer stored energy, available capacity and
fill fraction, the derived buffer temperature against its band, the
configured rate limits, whether access was open this tick, the last
charge and discharge powers, the full (no headroom left to absorb) and
depleted (nothing left to return) flags, the limiter that capped or
blocked the exchange, and the closing storage ledger
`starting + charged − discharged − stored ≈ 0`. The host reservoir
reports the heat its buffers absorbed and returned, and the station
thermal rollup carries the buffer totals. The ledger is audited every
tick: a residual beyond tolerance, a negative or over-capacity store, or
a charge/discharge above the configured rate is a containment violation
that rolls the tick back.

World schema 81 persists the authored buffer contracts (units: kJ, °C,
kW, kW/K); a pre-81 document carrying `thermalBuffers` is refused
rather than silently dropped. Stored energy, fill, derived temperature and
the charge/discharge ledger stay runtime-only: Load and Reset reconstruct
them from the authored starting conditions. The separate Station Systems
window drafts buffers (host reservoir, access mode, circulation loop,
capacity, starting energy, usable band, charge/discharge rates and
transfer conductance) with full validation reporting, and shows the live
stored energy, derived temperature, charge/discharge, access state,
full/depleted flags, limiter and closing ledger beside the existing
views.

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
authored exchanger conductance in kW/K.

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
exceeds 100 %. The curve's documented supported domain is strictly
above -243.04 C, where the expression is singular and its exponent
overflows below it; at or below that temperature the model
continues finitely at zero vapour capacity — air that cold holds no
vapour — so an out-of-domain temperature never publishes an
overflow as a valid shortage (#565).

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
compartments automatically: compartments are segregated by
construction, and containment follows from which pathways are wired
rather than from any declared label.

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
reservoir coupling, unknown reference, zero volume, negative
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

### Crew consumption and metabolic returns (#401)

A crew is an authored population — a count, an activity profile and
a table of per-crew-per-second metabolic rates — that consumes food
and water from finite compound stores and oxygen from one compartment
atmosphere, and returns carbon dioxide, moisture and metabolic heat
to that compartment and urine and organic waste to their own finite
compound stores. The population is never derived from the World's
Agents: it is a number the author writes, and nothing in the crew
model reads, mutates or synchronises Agent state.

Every pathway is scaled by one uniform metabolic scale per tick:
the minimum of the supply fractions (food, water, oxygen — what the
inventories can actually give against what is requested) and the
output-headroom fractions (urine, organic waste and the compartment's
capacity to receive — where the returns can actually go). A starved
or back-pressured crew therefore throttles every pathway together
rather than eating without breathing or breathing without excreting:
absent material never generates nominal waste. Requested and actual
are tracked separately per pathway, so the unmet remainder of each
stream stays visible — partial supply is an explicit condition, not
a silently-adjusted nominal.

The authored coefficients must close before anything runs. At full
activity the emitted mass (CO2 + moisture + urine + organic waste)
may not exceed the consumed mass (food + water + oxygen): metabolism
may not create mass. Every tracked constituent returned in the
urine, organic-waste and respired-moisture streams must be funded by
the constituents in the consumed food and water — nitrogen,
phosphorus, potassium, minerals and organics cannot come from
nothing. And the declared metabolic heat may not exceed the consumed
food energy minus the energy still bound in the waste streams: heat
may not be created from nothing. A crew whose returns fall short of
its consumption retains the difference as explicit retained body
mass, reported in the ledger rather than discarded. The four matter
streams must reference compound mass stores (g or kg with declared
compositions) so the element closure is checkable at authoring time.

**Crew stream roles must name distinct stores (#600).** The tick sizes
every draw and return against the store's inventory or headroom
measured from the same starting state, then books the pathways
sequentially, so one store named for two of the crew's own roles —
food, water, urine, organic waste — has the same inventory spent
twice. With aliased inputs the second draw finds the store already
drained while the returns still run at the original scale, driving
retained body mass negative and pausing the World at tick zero; with
aliased outputs the second return is clipped after both inputs were
consumed, silently changing the metabolic balance the author sized.
The crew therefore refuses **every pair** of its four stream roles
landing on one store — the same rule the electrolyzer and fuel cell
carry (#585) and the carbon-recovery machines carry (#599): one
inventory may not stand for two different substances the ledger books
separately. The refusal is a configuration refusal, transactional and
before the World is touched, and document loading refuses an authored
file that aliases crew roles with the same diagnostic. Sharing between
different crews is untouched — two crews may draw from one food store
and return into one waste store, because crews book sequentially
against the live inventory. The bundled Lua model mirrors the refusal
at construction so the sandbox agrees with the host on replay.

Crew run after the airlock cycles and before the compartment's
thermal integration, so metabolic heat enters the same coupled
vapour/thermal solve as every other heat source: the crew's heat
warms the air, can drive vapour to saturation and condensation, and
is booked in the compartment's thermal ledger with the rest. The
compartment's oxygen draw, CO2 and moisture additions flow through
its own composition and containment ledgers, which keep closing
against the crew's returns.

World schema 77 persists the authored crew contract (count, activity
profile, store references and the eight per-crew rates); a pre-77
document carrying `crews` is refused rather than silently dropped.
Consumption, returns, the metabolic scale, retained mass and the
heat ledger stay runtime-only: Load and Reset reconstruct them from
the authored starting conditions. The separate Station Systems
window drafts crews (population, activity profile, the four store
references and every rate) with full validation reporting, and
shows the live per-pathway requested-versus-actual figures, the
current metabolic scale and its limiter, the cumulative consumed /
returned / retained ledger and the compartment's booked crew heat
beside the existing views.

Crew acceptance coverage: refused configurations (unknown breathing
compartment, unknown, non-mass or non-compound stream stores,
negative crew count, malformed activity profile, returns exceeding
consumption, a returned constituent the food and water cannot fund,
metabolic heat above the released food energy, over-budget crew
count) with no World mutation; all six pairs of the crew's four
stream roles probed for aliasing with nearly empty inputs and nearly
full outputs, and both reported reproductions (aliased food/water
inputs, aliased urine/organic-waste outputs) refused through the
public configure/tick/snapshot seam with the World untouched; a
two-crew run sharing the food, water, urine and waste stores stays
clean, with the actual per-crew draws and returns, the shared-store
depletion, the retained mass and the constituent closure asserted;
nominal metabolism where every
pathway runs at the authored rate, the compartment books the crew's
oxygen draw, CO2, moisture and heat, the waste stores receive the
returns, the consumed-minus-returned difference lands in the
retained ledger and every constituent ledger closes; starvation (an
empty food store collapses the scale to zero — nothing else is
consumed, nothing is returned, the shortfall is visible as unmet
demand and no absent material generates waste or heat); partial
supply (a half-full water store puts the scale at one half and
every pathway — consumption and returns alike — throttles with it,
with requested staying visible above actual, and the emptied store
starves the next tick); zero-oxygen refusal (no food or water draw,
no CO2, no heat, the compartment's limiter named); a full waste
tank back-pressuring the whole metabolism to refusal with nowhere
for the returns to go; activity scaling (a half-activity crew eats,
breathes, works and excretes at half the rates); Agent isolation
(with crews running, Agents match a crew-free twin World field for
field — count, sector, position, state, pose); deterministic Reset
replay of snapshots and events; and intact Lua crew descriptor
crossings with contained construction refusal, plus an in-sandbox
assertion that the mirror's own retained ledger closes and its
metabolic scale stays within [0, 1] every tick.

### Water recovery and qualification (#402)

Water recovery is two authored contracts: a **collector** that condenses
water vapour out of one compartment atmosphere onto a cold surface served
by a coolant loop, and a **treatment** that turns raw water into treated
water plus a retained concentrate.

A collector needs a real cold surface, not merely a loop that exists:
collection requires coolant actually circulating across it (the loop's
circulation fraction, capped by its inventory and pump supply) **and** the
surface colder than the compartment air. It condenses up to its configured
maximum rate, stops at the configured relative-humidity target rather
than stripping the air dry, and is capped by the destination raw water
store's headroom. Each of those stops is reported as a `blockReason` with a
`Rejected` status instead of the collector quietly idling.

The latent heat the vapour gives up is routed into the coolant loop and
**nowhere else**. It enters the loop's own energy ledger as an inflow term
alongside pump work, heater heat and exchanger exchange, and the
compartment never books it: the compartment's vapour ledger records the
removal as a `collected` pathway and its thermal ledger stays untouched by
the collector's latent heat, so one condensation is one heat source and is
never counted twice. The loop's closing ledger — starting + pump + heater
+ exchanged + collector − extracted = capacity × temperature — is audited
every tick.

A treatment runs at one uniform processing scale per tick: the smallest
fraction of its rated throughput that the drawable raw water (above its
protected reserve), the treated-store headroom, the concentrate-store
headroom and the drawable treatment consumables can sustain. The process is
paid for before a kilogram moves. With no electrical supply at all the tick
is refused whole — no water moves and no consumable is drawn, because a
treatment with no power is not a treatment running for free. The
processed mass splits by the configured recovery ratio into treated water
and concentrate, and each output is clamped to its own headroom so a
roundoff at the limit can never overshoot capacity.

**Qualification is the full rated energy dose.** A treatment supplied its
full power demand produces treated water that is admitted as *qualified*.
A treatment running on curtailed power still runs — the machine is not
stopped mid-stream — but its output is **held unqualified**, because a
partial energy dose has not finished the water. The hold is explicit on the
snapshot (`qualityAccepted` false with a `holdReason`), the held mass is
tracked separately from accepted mass, and held water sits on the
unqualified side of the treated store where transfers cannot pick it up.

The **concentrate is retained, not usable**: it lands on the unqualified
side of its store and stays accounted for there until a disposal
capability exists. Nothing in this slice disposes of it, and its retained
total is reported so the stream is never silently dropped.

Treatment source, treated and concentrate stores may be **compound water
streams** (#740–#741). Moisture is the water fraction; the existing dry
constituents are the dissolved load. Live concentration uses the actual
constituent ledger, including dilution, never the destination's declaration.
Plain inflow into a compound water store adds water only; compound-to-plain
transfers remain refused. Condensate collectors still produce pure water.
The treatment consumable's draw books both unit and constituent loss.
Water stores may be shared by any processes (#751),
and a collector may serve any compartment: condensate is just
water, whatever air it condensed from, and the heat-only exchanger
through which no coolant ever crossed is the containment (#752).

Within the tick, treatments run after the chillers and collectors run
after the treatments, both before the coolant loop's second pass, so the
latent heat a collector adds is inside the same integration the loop's
ledger closes over, and condensate delivered this tick is processed from
the next tick on rather than in the same breath in which it appeared.

World schema 78 persists the two contracts — the collector's
compartment, loop, service class, destination store, maximum rate and
humidity target, and the treatment's four store references,
throughput, recovery ratio, specific energy, consumable rate and priority.
A pre-78 document carrying `waterCollectors` or `waterTreatments` is
refused rather than silently dropped. Collected, processed, treated,
accepted, held and concentrate totals, the energy and consumables drawn and
the quality holds stay runtime-only: Load and Reset reconstruct them from
the authored starting conditions. The Station Systems window drafts both
contracts with full validation reporting and shows the live collection
gating and routed latent heat, the processing scale and its limiter, the
power demanded versus supplied, the quality hold state and the cumulative
recovery, retention and energy ledgers.

Water recovery acceptance coverage: refused configurations (unknown
compartment, loop or store; an unknown consumable store; a humidity target
outside [0, 1]; a negative rate; a service class that mismatches the loop's
host reservoir; an incompatible compound outlet; a recovery ratio outside
(0, 1]; a treatment sharing one store between two roles; over-budget
contract counts) with no World
mutation; collection where a collector serves any compartment, runs at
its configured rate and closes every ledger (#752); nominal recovery where the collector runs at its configured
rate, the routed latent heat matches the condensate exactly, the
compartment's vapour falls by precisely what was collected while booking no
latent heat of its own, the loop ledger closes with the collector term, and
the treatment splits, qualifies and consumes at its authored rates with
the concentrate retained unqualified; power curtailment (a partial dose
runs the process and holds the whole output unqualified with a stated
reason, and the shortfall is visible on the power draw); zero power (the
tick is refused whole with no water and no consumables moved); cooling
unavailable (a dry loop blocks collection with the reason named while the
treatment keeps running off stored raw water); a cold surface warmer than
the air (no condensation); the humidity target binding (collection stops
short of the rate and reports the target stop); a full treated destination
and an exhausted consumable store each capping the process and naming
themselves as the limiter; deterministic Reset replay; and intact water
descriptor crossings with contained construction refusal for both
contracts.

### Partial water-treatment removal (#741)

Schema **100** adds an optional `removalFractions` map keyed by
`dryOrganics`, `nutrientNitrogen`, `atmosphericNitrogen`, `phosphorus`,
`potassium`, or `otherMinerals`. Each finite fraction in [0, 1] is the share
of that drawn constituent routed to concentrate; omitted keys mean **all
removed** (1). Unknown keys are refused with their name. Water recovery
still applies only to the drawn water. Chemical energy follows the organic
partition. Every drawn constituent ends in treated or concentrate exactly
once; headroom limits use the complete resulting stream mass.
Any outlet receiving dry load must be compound, including the treated outlet
when all removals are zero. Absent fractions preserve full removal from a
compound source and the original exact scalar split from a plain source.

The shared form/Flow inspector supports per-species overrides, reset to the
full-removal default, and an effluent-concentration preview using the wired
source's **declared** composition (not a prediction of future mixing).
Fields participate in draft undo/redo, configuration and World save/load,
and Apply. Snapshots and Lua expose the authored overrides and per-tick and
lifetime removed/passed constituent ledgers.

`scenarios/water-greywater.yaml` draws 100 kg/day at 0.2% organics with 85%
water recovery and 95% organics removal: **84.84 kg/day treated** containing
0.01 kg organics (0.011787%), and **15.16 kg/day concentrate** containing
0.19 kg organics (1.2533%). Lower removal or higher incoming load raises
effluent concentration; downstream quality gates report the resulting constraint.
`water-legacy-anchor.yaml` keeps the all-plain default regression.
Focused checks: `station-compoundWaterPartial`,
`station-compoundWaterPartialValidation`, `station-compoundWaterDocuments`,
and `station-editor-compound-water`, alongside the full-removal checks.

### Water quality acceptance (#742)

Crew water, crop irrigation, algae makeup and microbial process-water inputs
accept optional `waterQuality` settings:

```yaml
waterQuality:
  maxConcentrations: {dryOrganics: 0.0005, nutrientNitrogen: 0.0001}
  maxTotalDissolved: 0.001
```

Values are finite, non-negative kg constituent per kg stream; the species
vocabulary is the six dry constituents above. Omitted fields mean **no gate**.
Gates on producer-only or non-water units are refused, not silently ignored.
Electrolyzers instead have an immutable zero-total-dissolved gate: compound
feed is valid configuration, but any live dry load stops splitting before
spending electricity or water. Plain feed retains the existing operation.

The gate reads the current ledger blend, including dilution and same-tick
upstream deposits. At full-demand water mass `D`, each tolerance `T` supplies
a dissolved-load budget `D*T`. A concentration `C > T` caps the common unit
scale to `T/C`; multiple limits take the minimum. Zero tolerance with load
means zero operation. Scaling does **not** purify the accepted blend: it limits
exposure/load relative to full demand, while every other input and output
scales uniformly. Within-tolerance water passes in full; other resource,
power and headroom constraints may still reduce operation.

Snapshots and Lua per-consumer `waterQuality` expose live `concentrations`,
`totalDissolved`, `enabled`, quality-only `scale`, `bindingConcentration`,
`bindingTolerance`, `limiter`, and `demandKgPerS`/`permittedKgPerS`. The permitted
rate is a quality ceiling, not a promise of actual service. Named limiters
also participate in the existing unit limiter surface. The Flow unit inspector
shows the ceiling versus demand and binding concentration versus tolerance.
Shared authoring forms offer optional species/total tolerances with no-gate
defaults, participating in draft undo/redo, configuration/World persistence,
and Apply.

The greywater scenario includes a demand-only crew `potable-gate`: treated
water passes its 0.05% organics tolerance after treatment starts. Rewire the
crew water input to `greywater` to see raw 0.2% organics throttle service to
25% with the named organic limiter; rewire to `treated` to recover full
service. The probe is not a health or metabolism model. Reference-station
settings remain unchanged. Focused checks: `station-waterQualityGates`,
`station-waterQualityValidation`, `station-waterQualityConsumers`, and
`station-editor-compound-water`.

### Producer dissolved loads (#743)

Microbial processors accept `recoveredWaterCarryoverFractions`, a map from
known dry constituent names to finite fractions in [0, 1]. Each fraction
routes that share of the **unrecovered** constituent into recovered water
instead of residue. N/P/K/minerals use the remainder after nutrient recovery;
organic load uses the unoxidized remainder, and atmospheric nitrogen remains
unconverted. A positive fraction requires a compound recovered-water store.
The water recovery coefficient still controls moisture only: carried dry mass
adds to the wet product mass and subtracts from residue. Destination capacity
therefore backpressures the whole process using the loaded product's mass.
Absent or zero fractions preserve the old split exactly. Actual deposit
composition, including pure-water deposits, replaces the destination's seed
composition through the existing drift ledger.

Optional crew greywater declarations use an independent, compound, mass-unit
output store, no longer tied to any compartment class:

```yaml
greywaterStoreId: greywater
greywaterKgPerCrewDay: 8.64
greywaterLoadKgPerCrewDay: {dryOrganics: 0.0864, nutrientNitrogen: 0.00864}
```

The declared total **includes** the dissolved load; the remainder is water.
All values are finite and non-negative. Loads must fit that total and be
funded by the food/water consumed, alongside existing returns and heat.
Population, activity, supply and waste headroom scale every pathway together.
Greywater reduces retained body mass, never creates material. An omitted
return preserves existing metabolism. Nonzero greywater returns explicitly
make the declared-profile forecast unavailable rather than projecting without
the new return's backpressure.

Processor snapshots/Lua expose `lastCarriedConstituents` and
`carriedConstituents` (kg this tick and lifetime). Crew snapshots/Lua expose
`lastGreywaterKg`, `greywaterKg`, `lastGreywaterConstituents` and
`greywaterConstituents`. The Flow inspector shows producer load rates; shared
forms, draft history and configuration/World serialization retain declarations.

`scenarios/water-carryover.yaml` bridges the urine nitrifier to crop irrigation:
4 kg feed/day yields 0.048 kg recovered N/day and carries 0.006 kg N/day into
2.666 kg recovered stream/day, leaving 1.40606 kg residue/day. Its N load is
0.00225056264 kg/kg, below the crop's 0.003 kg/kg acceptance tolerance. Crop
nutrient uptake retains the existing separate fertilizer ports by default;
accepting loaded irrigation does not implicitly substitute dissolved N for that
supply. Schema 101's explicit `irrigationUptakeFractions` opt-in does support
selective dissolved N/P/K uptake with unabsorbed salt retention. See
[the flagship water scenarios](compound-water.md#selective-crop-irrigation-744)
for the equations, conserved ledgers and documented purge/failure actuals.
Treatment qualifies output on complete energy evidence alone; partial doses
leave output held unqualified. No separate release-test contract is required.
The reference station is unchanged. Focused checks: `station-bioconversionCarryover`,
`station-crews`, and `station-editor-water-carryover` (Open, edit/undo,
save/load, Apply and public World ticks).

### Oxygen production and CO2 capture (#404)

Oxygen production and carbon-dioxide removal are two authored contracts
that ride the same tick, the same power pool and the same store machinery
as the rest of the station slice: an **electrolyzer** that splits
qualified water into oxygen and hydrogen, and a **CO2 capture machine**
that scrubs a compartment's carbon dioxide into a finite capture store.

The electrolysis split is **stoichiometric**: from one kilogram of split
water, `StationElectrolysisOxygenMassPerWaterKg` (the molar-mass ratio
M_O2 / 2·M_H2O) leaves as oxygen and the remainder is hydrogen, so the
mass ledger closes exactly under the declared molar masses rather than
rounding a second ratio against them. The cell's electrical dose is
authored in kWh per kilogram of split water — the same physical unit as
the declared chemical energy — and the dose must cover at least the
chemical energy the produced hydrogen retains (its lower heating value,
a documented constant of the product fuel, not an authored coefficient).
A dose below that floor is refused at configuration: no machine can create
energy-rich products out of less energy than the products carry. The
difference between the dose and the retained chemical energy is **process
heat**, routed exactly once into the declared
thermal-class-matched heat-sink reservoir and booked as its own ledger term
there, alongside generator
waste heat and compartment exchange.

The cell is paid for before a kilogram splits, and it only splits water
the **actually supplied energy** can make: curtailed power reduces
production rather than gifting energy-rich products for free, and the
shortfall stays visible as unmet production. A tick that demands power
and receives none is refused whole — nothing splits and nothing appears.
**Cooling is gated on the sink temperature:** while the sink sits below
the cell's maximum sink temperature the cell may run; at or above it the
interface cannot accept the process heat, so the cell is refused whole
and spends nothing, with the reason named. The cell draws feed water only
above its store's protected reserve, so a life-support water reserve is
never electrolysed away. Oxygen is delivered to a qualified gas store or
directly into one compartment atmosphere — exactly one destination is
configured, never both and never neither — and a full product store **caps
production rather than venting or deleting the excess**. When oxygen is
delivered into a compartment it is booked as an addition pathway of that
compartment's own oxygen closure, so every gram the cell made is visible
in the air it was produced for; the feed-side store loss already nets the
delivered mass out of the store boundary, so nothing vanishes.

CO2 capture is **finite on every axis**: the rated rate, the CO2 the
compartment actually holds, the capture store's remaining headroom, and
the drawable regeneration consumable. Each axis is a scale cap, and the
**unmet atmospheric service is the explicit remainder** of the rated
request against them. Captured gas is **inventory, not vanished carbon**:
it leaves the compartment through a booked removal pathway and occupies
the capture store until a later disposal or reuse capability moves it.
Each captured kilogram costs its declared electrical energy and, when a
consumable is configured, its declared share of the consumable, drawn
from the shared power pool and the consumable store above its protected
reserve. A tick that demands power and receives none is refused whole:
no CO2 is scrubbed and no consumable is drawn. The machine can only
scrub what the compartment holds, so it never drives the CO2 inventory
negative.

Gas stores in both chains are **plain mass stores**: a compound store
would create dry constituents out of pure gas. The capture machine's
consumable store is the one exception, being a supplied consumable rather
than a gas stream, and its draw books both the unit and constituent loss.
The chain no longer segregates by class (#750, #751): an electrolyzer may
draw any qualified water store and deliver its products to any
compatible stores, and a capture machine may scrub any compartment.
Wiring that a declared class boundary once refused now runs — mass
moves, the destination fills, and the consequence of an unwise
connection shows up in the ledger rather than at authoring time.

**Stream roles must name distinct stores (#585, #599).** The tick sizes
each product against its own store's headroom *before either is added*,
so a single store named for two roles has the same headroom spent
twice: the authored configuration turns a finite physical limit into a
terminal runtime failure and pauses the World with the tick at zero.
The electrolyzer therefore refuses one store named for both the oxygen
and the hydrogen product, and one store named for both the feed water
and either gas product; the fuel cell refuses the analogous pattern of
one store named for both the hydrogen and the oxygen reactant, where
the cell would spend one store's drawable stock as both. The carbon
recovery chain carries the same rule (#599): the Sabatier reactor
refuses **every pair** of its four stream roles — CO2 feed, hydrogen
feed, methane product, product water — landing on one store, and the
methane processor refuses every pair of its three — methane feed,
recovered hydrogen, solid carbon. Feed/feed and feed/product aliases
are refused on the same principle even where no capacity error is
reached: one inventory may not stand for two different substances the
ledger books separately. The refusal is a configuration refusal, not a
runtime one: it happens before the World is touched, so the overfilling
tick never runs, and document loading refuses an authored file that
aliases roles with the same diagnostic. Cross-machine sharing is
untouched — a fuel cell may burn the hydrogen its electrolyzer
produced, and one reactor's methane output store may be a processor's
methane feed, because machines book sequentially against the live
inventory; the closed loop that makes is the point of the contracts.
The bundled Lua model mirrors both carbon-recovery refusals at
construction so the sandbox agrees with the host on replay.

Within the tick, electrolyzers and capture machines run after the water
collectors and before the coolant loop's second pass: they compete for
the same generation-and-battery pool, and the electrolysis process heat
lands in the sink before the reservoir integrates. Each runs in the
declared deterministic order (priority descending, declaration order
ascending), and the single-pool audit carries their supplied service
alongside every other load — oxygen production and carbon removal are
services, never a second budget beside the loads and the thermal plant.

World schema 79 persists both contracts — the electrolyzer's
feed water store, oxygen store or delivery compartment, hydrogen
store, thermal service class, heat-sink reservoir, rated split, specific
energy, maximum sink temperature and priority, and the capture machine's
compartment, capture store, consumable store, maximum rate,
specific energy, consumable rate and priority. A pre-79 document
carrying `electrolyzers` or `co2Captures` is refused rather than
silently dropped. The split, products, energy and heat ledgers, the
captured mass, the unmet service and the consumables drawn stay
runtime-only: Load and Reset reconstruct them from the authored starting
conditions. The Station Systems window drafts both contracts with full
validation reporting and shows the live split and its stoichiometric
products, the cooling gate, the power demanded versus supplied and the
energy-feasibility cap, the product-headroom limiter, the captured mass
and unmet atmospheric service, and the closing mass and energy ledgers.

Gas acceptance coverage: refused configurations (unknown feed, product,
capture or consumable store; both or neither oxygen destination; a dose
below the retained chemical energy; a sink-temperature bound below
absolute zero; a thermal class that mismatches
the heat-sink reservoir; a consumable demand with no consumable store; one store
aliased across the cell's own stream roles — oxygen and hydrogen
products, or feed water and either gas product (#585); over-budget
contract counts) with no World mutation; the aliased-product case
probed through the public configure/tick/snapshot seam so the
overfilling tick is shown never to run, while a cell that delivers
oxygen into a compartment and hydrogen into a distinct store still runs
clean against the same tiny store; nominal electrolysis where the
split follows the declared stoichiometry, the supplied/chemical/heat
energy ledger closes, the protected reserve is untouched, the process
heat lands in the sink and the per-unit boundary closes; the cooling gate
(a hot sink refuses the cell whole with the reason named and announced);
zero power (the tick is refused whole, nothing splits); the energy-
feasibility cap (curtailed power bounds the split at what the supplied
energy can make, with unmet production the explicit remainder and the
chemical energy never exceeding the dose); a full hydrogen store capping
production instead of venting and naming itself as the limiter; oxygen
delivered straight into a compartment with the compartment's oxygen
closure accounting for every gram; nominal capture where the
compartment's carbon falls by exactly what the store gains, the
consumable and electricity are drawn with it, and the captured mass is
carried as collected inflow on the store boundary; finite service (the
machine stops at the CO2 the compartment holds and is refused with a
stated reason); consumable exhaustion capping the total captured at what
the media can regenerate; deterministic Reset replay; and intact
electrolyzer descriptor crossings with contained construction refusal.

### Fuel-cell backup from dedicated reactant stores (#406)

A **fuel cell** generates backup electricity by recombining hydrogen
and oxygen into water, drawing both reactants from **dedicated stores**
so a life-support oxygen reserve is never consumed implicitly. The
contract is authored as the hydrogen store, the oxygen
store, the product-water store, a thermal service class with its
heat-sink reservoir, a rated electrical output, a conversion
efficiency, a commanded output and a maximum sink temperature.

The reactant draw is **stoichiometric on the mass ledger**: one
kilogram of hydrogen consumes `FuelCellSettings::oxygenMassKgPerHydrogenKg()`
— the molar-mass ratio (2·M_O2 / 2·M_H2) ≈ 7.9367 kg of oxygen — and
the produced water is exactly the sum of the two, so mass closes
exactly under the declared molar masses. The electrical energy is the
declared **efficiency** times the hydrogen's lower heating value
(`StationMolarMassHydrogenKgPerMol`-derived, a documented constant of
the product fuel); the remainder is **waste heat**, routed exactly once
into the class-matched sink and booked as its own ledger term there,
alongside generator waste heat and compartment exchange. An efficiency
outside (0, 1] is refused at configuration: no machine may return more
energy than the fuel carries.

The cell is **bounded on every axis at once**: the commanded output,
the rated output, the hydrogen drawable above its store's protected
reserve, the oxygen drawable above its store's protected reserve, and
the product-water store's remaining headroom. The smallest cap wins,
the cell scales down rather than overshooting any of them, and the
limiting axis names itself. Because the draw is capped by what is
**drawable above the reserves**, a protected ECLSS oxygen reserve is
never touched: a cell commanded against a store that sits at its
reserve simply refuses and says so. Refilling the dedicated stores is
the job of a **controlled surplus transfer** from the ECLSS — the
transfer moves only oxygen above the protected reserve, and the cell
runs on what arrived; the cell itself never reaches into a protected
store. Product water that cannot fit **caps output rather than venting
or deleting the excess**, and the blocked destination names itself as
the limiter.

**Cooling is gated on the sink temperature** the same way the
electrolyzer gates: while the sink sits below the cell's maximum sink
temperature the cell may run; at or above it the interface cannot
accept the waste heat, so the cell is refused whole and consumes
nothing, with the reason named. Endurance is reported as the seconds of
rated output the current drawable reactants can sustain, so an operator
sees how long the backup will last before committing to it.

Within the tick, fuel cells run on the **supply side after the
generators and before load allocation**: their output joins the same
generation pool the loads draw from, and their waste heat lands in the
sink before the reservoir integrates. They are generation, not a load —
the single-pool audit carries their supplied service alongside every
other source and sink, and the cell never competes with itself for its
own electricity. The electrical output is bounded by the cell's own
electrical rating and by the reactants actually drawn, so the pool
never books energy the stores cannot pay for.

World schema 80 persists the contract — the three
store ids, the thermal service class, the heat-sink reservoir, the
rated output, the efficiency, the commanded output and the maximum
sink temperature. A pre-80 document carrying `fuelCells` is refused
rather than silently dropped, and malformed values (an efficiency above
one, a commanded output above the rating, an unknown heat sink) are refused from a file exactly as they are
refused through the live editor. The consumed reactants, produced
water, generated energy, waste heat, endurance and limiter state stay
runtime-only: Load and Reset reconstruct them from the authored
starting conditions.

Fuel-cell acceptance coverage: refused configurations (an unknown
hydrogen, oxygen, water or heat-sink store; an efficiency outside
(0, 1]; a commanded output above the installed rating; a thermal
class that mismatches the heat-sink reservoir; one store aliased across the
cell's two reactant roles (#585); an over-budget cell count)
with no World mutation; nominal backup where the load is served from
the cell's own output, the reactant mass ledger closes against the
declared stoichiometry, the energy ledger closes against the LHV and
efficiency, the waste heat lands in the sink exactly once, and
endurance reads against the drawable reactants; reactant exhaustion
where the cell stops at the drawable hydrogen and names the limiter;
reserve-transfer refusal where a cell commanded against oxygen sitting
at its protected reserve refuses whole and the ECLSS reserve is
untouched; surplus fill where a controlled transfer above the reserve
lets the cell run on the oxygen that arrived; a blocked water
destination capping output rather than venting and naming itself; a
cooling refusal where a hot sink stops the cell whole with the reason
named and the reactants unconsumed; a protected reactant reserve that
the cell may not draw below; the electrolysis/fuel-cell round trip
where the mass ledger closes across both machines while the round-trip
efficiency is the product of the two efficiencies (about 50 % for the
tested pair) — the cycle loses energy exactly as declared and creates
none; deterministic Reset replay; and intact Lua fuel-cell descriptor
crossings with contained construction refusal.

### Terminal-waste storage and ejection (#403)

Terminal waste is material at the end of its useful life inside the
station, removed across the boundary on an authored schedule. Each
ejection contract names one existing Resource store, the same unit as
that store, a positive quantity, a first ejection time, and an
optional recurrence interval — the same schedule discipline as
resupply deliveries, including the one-off zero recurrence and the
at-le-one-fixed-tick rule. Each waste stream keeps its own store and its
own ejection contract.

Ejection `eligibility` explicitly selects the eligible stock:
- `qualifiedOnly` (the default when omitted) removes only qualified stock.
- `heldOnly` removes only held (unqualified/untreated) stock, never usable stock.
- `qualifiedAndHeld` is an emergency drain: it removes qualified stock first,
  then held stock only for the remaining requested quantity. With no held stock
  it follows the default trajectory exactly.
Held waste is ejected only when the ejection declares it. World and Station
configuration schema **102** persist the optional selector; older documents
without it retain qualified-only semantics. This does not
make held stock drawable by transfers or consumers. Each occurrence
removes at most the scheduled quantity and the eligible partitions'
contents; the remainder is unmet, never forced, and never a failure.
An empty eligible partition reports Rejected and full unmet demand,
but still counts as an occurrence, so the schedule never stalls. What is
ejected crosses the station boundary as an external loss: it is
booked in the per-unit and per-constituent `lost` ledgers, so the
balance closes as current + lost - delivered - collected == starting
with the ejected mass explicitly gone from the station and any ineligible
partition explicitly retained. Snapshots expose eligibility, lastQualifiedEjected,
lastHeldEjected, qualifiedEjectedTotal and heldEjectedTotal beside the totals.

For an emergency-drain example, start `tank` at 3 kg qualified and 7 kg held:
```yaml
wasteEjections:
  - id: emergencyDrain
    storeId: tank
    unit: kg
    quantity: 5
    firstOccurrenceSeconds: 1
    recurrenceSeconds: 0
    eligibility: qualifiedAndHeld
```
At 1 second the snapshot reports 3 kg qualified and 2 kg held ejected,
leaving 0 kg qualified and 5 kg held. Requesting 15 kg instead removes
3 + 7 kg and reports 5 kg unmet. The declared-profile forecast projects
these same partition removals; it does not assume treatment or requalification.
`station-wasteEjections` exercises this scenario, empty-held equivalence,
shortfalls, Lua parity and drift-inclusive ledger closure;
`station-wasteEjectionFiles` checks YAML/binary reload and identical re-runs.

Ejections run in the same scenario-event pass as deliveries, under
the same total order: occurrence time, then higher priority first,
then — at equal time and priority — arrivals before ejections, so
material delivered this tick is disposal-ready for the ejection that
follows it. A higher-priority ejection runs before a coincident
delivery and can only remove what was already in the store. This is
a scenario input: ejection is authored removal at scheduled times,
not a simulated safe trajectory, navigation certification, or a
disposal-process model.

Not-immediately-needed material is not automatically terminal waste:
recovered and surplus resources stay in their stores until capacity
is reached (full destinations refuse and backpressure inflow through
the existing store-capacity rules), and an authored ejection removes
only its scheduled quantity, never the surplus beyond it.

World schema 82 persists the authored ejection contracts (units: kg,
L, gallons, seconds); a pre-82 document carrying `wasteEjections` is
refused rather than silently dropped. Occurrence counts, the
ejected/unmet split and the loss bookings are runtime-only: Load and
Reset restart every ejection schedule from its authored beginning and
repeated cycles replay identically. The separate Station Systems
window drafts ejections (store, unit, quantity, first occurrence,
recurrence, priority) with full validation reporting, and shows the
live ejected/unmet amounts, occurrence counts, the next ejection
time, and the store's remaining usable-versus-held split for every
contract beside the existing views.

Ejection acceptance coverage: refused configurations (unknown target
store, cross-unit ejection, zero/negative quantity, negative first
occurrence, sub-tick recurrence, duplicate and empty ids,
over-budget count, and refused configurations leaving authored
settings untouched); one-off ejection with the scheduled removal at
its tick, spent-schedule silence afterward, and the boundary closing
over the external loss; empty-store and fully-held-store ejections
reported as nothing-ejectable without failing the station; held-waste
preservation (only the qualified pool is removed, held stock stays
behind occupying capacity, the shortfall visible as unmet);
recurrence firing exactly once per period with the dry-store
occurrences still counted and the ejected total capped by what the
store could give; coincident delivery/ejection ordering at equal
priority (arrival lands first and is ejectable the same tick) and
with a higher-priority ejection (later-arriving material stays
behind); no-automatic-waste (an unhooked store stays full, an hooked
store loses only its scheduled quantity); the bundled Lua sandbox
mirroring the same eligible-only removal and loss booking and
agreeing with the host ledger for the same scenario; and Reset
restarting the authored schedule from authored state. File coverage:
schema-82 round trips through both document formats with only
authored fields persisted, live ejection state never leaking, the
scenario and its event history reproducible from the document, and
pre-82 documents carrying `wasteEjections` refused. Editor coverage:
the real Station Systems window drafts, validates, refuses and
applies ejection edits through the reset-confirmed flow, and the
live view reports the ejected/unmet split and the retained held
waste.

### Treatment qualification and containment boundaries (#412)

Containment is no longer a declared label the model enforces on the
author's behalf. The class that once sat on every store, compartment,
loop and process — and the boundary rules built on it — is removed
(#749–#758); what remains is containment as physical fact plus
honest reporting. A store holds what its capacity allows; a transfer
moves what it is wired to move; the consequence of an unwise
connection is reported downstream in shortages, holds and tolerance
throttles rather than refused upstream by a category.

**Wiring is legal; the ledger keeps the score.** A transfer or
conversion may join any two stores, and a process may touch any
store. Mass moves, the destination fills within its capacity, and
every gram stays accounted. Nothing is filtered, silently rerouted,
or morally re-labelled on the way.

**Treatment output qualifies on complete energy evidence (#753).** Filtration
runs with the process; the full rated energy dose supplies treatment evidence.
A partial dose holds output unqualified with a stated energy-curtailment reason.
Ordinary transfers draw only qualified inventory, so held material cannot bypass
this gate. No release-test consumable, test dose, missing-test hold or explicit
class-conversion flag remains. The process still requires its treatment media
and respects their protected reserve, throughput and connection limits.

**Heat-only interfaces never move mass.** A coolant loop exchanges heat
with its coupled reservoir through the circulation-gated exchanger and
nothing else: the loop's finite coolant inventory is unchanged by the
exchange, no Resource store is touched, and a loop's coolant
never crosses into another store, a reservoir, or another loop. Heat
rides the interface; mass never does. That interface — not a
declared class — is what keeps a loop's coolant out of the station's
material inventories.

**Disposal-ready waste stays in its own store.** Each ejection
contract removes only its own store's declared eligible partitions
(qualified-only by default); one store's ejection never touches
another's contents.

Release-test fields formerly persisted by schema 88 and the class-conversion
flag introduced in schema 101 are retired. With schema 103 the whole hazard-class
chain is out of the wire format: documents carrying `hazardClass` on any store,
atmosphere, coolant loop or process entry, or carrying
`clearanceStoreId`, `clearancePerKg` or `releaseToNormalClass` on a water
treatment, are refused at any version with a named-field conversion-required
diagnostic, rather than silently defaulted.
Treatment quality gates and accepted/held mass remain runtime-only; Load and
Reset reconstruct them from authored starting conditions.

Focused coverage checks complete-dose qualification without a release-test
supply, partial-dose holds, treatment-media exhaustion and protected reserves,
qualified recovery transfers, host/Lua agreement, conservation and deterministic
Reset. YAML/binary World round trips persist only authored process settings.
The editor shows filtration and energy evidence, exposes no release-test fields
or ports, and applies process edits through the reset-confirmed history flow.

### Protected cross-system service dependencies (#416)

Operations are admitted or curtailed without cutting the essential
support services that protected loads depend on. A set of Critical
station services is drawn from the actual generation-and-battery pool
ahead of every discretionary external load: coolant-loop circulation
(a loop's pump and freeze heater, protected only when its author
authores `criticalService` - a loop carries no class to infer
protection from (#754/#757)), biological essential circulation
every crop and algae population declares, and authored critical-service
chiller work (`criticalService` on coolant loops and chillers, the
flag persisted with the World document at schema 92 and refused on
pre-92 documents rather than silently dropped). The admission is
conservative: the protected draw is what the pool actually holds this
tick - never independent nameplate totals - and the protected chiller
credit is sized from the machine's real tick-start state (gated off at
the chilled setpoint, refused while the condenser sink is over, capped
by the heat the cold loop actually holds above the setpoint at the
circulation it could sustain). The credits taken ahead are what the
later loop, chiller and biology passes consume, so no watt is drawn
twice and no protected machine funds itself.

Competing protected dependencies resolve deterministically: protected
loops by declaration, then critical chillers by declaration, then
crops and algae by declaration; the first-declared service takes its
draw from the actual pool and later services get only the remainder.
What the pool cannot deliver stays explicit unmet critical minimum -
per-service requested/supplied/unmet with the binding reason, station
totals and a protected-unmet alarm on the snapshot, and edge-triggered
events naming the service that fell short or recovered. A protected
minimum is never silently reclassified as permissible curtailment, and
the discretionary demand that got ahead of nothing lands only on the
unprotected processes behind it.

Shutdown cooling stays protected even when the machine's electrical
output stops: a generator's decay-heat obligation books
unconditionally through the Shutdown phase and is exposed as a
protected `shutdown-cooling` service account beside the electrical
protections - stopping generation never erases the heat, and removal
adequacy stays tracked by the sink's own temperature alarms. Critical
sensing and control demand is not separately modelled (monitoring
reads direct model state, not sensor hardware), so there is no such
load to protect; the protection covers the services that exist.

The separate Station Systems window drafts the protected flags on
loops and chillers and shows the live protected-service accounts,
totals and alarm beside the dispatch status. The bundled Lua lifecycle
mirror runs the same protected pass, credits and audits inside every
private sandbox, so the scripted path and the host ledger agree tick
for tick.

### Resource-constrained maintenance (#414)

Scheduled timed work whose completion must be paid for with declared
tools, spare parts and consumables. A maintenance job names a registered
installation (chiller, coolant loop or generator), a first occurrence,
a per-occurrence duration, a recurrence (0 for a one-off; a positive
recurrence must be at least the duration so windows never overlap), a
priority, its requirements, and a waste store where removed material
lands. Every requirement declares a source store, a quantity, and
whether it is consumed (a part or consumable) or reusable (a tool).

Reservation is all-or-nothing per occurrence: work starts only when
every requirement can be reserved from qualified inventory at once,
and it never starts partially. Reusable tools stay reserved for the
whole job and are returned unchanged on completion or interruption -
the store is held, not diminished. Consumed requirements are reserved
up front and actually spent at completion: the material moves from the
source store into the declared waste store with the same unit and declared
composition (the waste store must accept exactly what
was spent), so a spent part shows up as booked waste rather than vanishing.

While a job is Active its target installation is isolated: its
availability reads zero, so a generator under maintenance produces
nothing and the isolation is visible on the snapshot. If a reservation
is broken mid-work - inventory drawn away by a resupply edge or a
store change - the occurrence is interrupted, not faked: reusable
reservations return immediately, the job drops to blocked with the
binding reason, and the occurrence is counted as missed rather than
completed. Absent prerequisites never allow free completion.

Each fixed tick runs the job set in three deterministic phases:
reservation integrity and interruption first (so a broken commitment
releases stores before anything else competes), then progress and
completion with the spend into the waste store, then start attempts in
priority order (higher priority reserves first among coincident
windows; ties fall back to declaration order). Occurrences use the
shared schedule-tolerance grid, so a due window is never missed or
counted twice across the floating-point boundary.

One window/deadline policy governs every phase (#587): an occurrence
whose window has already closed by the current tick - closed meaning
tick time has reached the occurrence time plus the authored duration -
is recorded missed, reported, and never back-filled, whether the job
sat idle, completed an earlier run, or was blocked without its
prerequisites. Every window already closed at the current tick is
skipped in the same pass, so a late pass never replays an old window
as startable. A start is admitted only while the window is still open,
including mid-window. Durations are not quantized to the tick grid: a
sub-tick window that no fixed tick can land inside is simply missed
when it closes, exactly like any window that closed while the job was
idle or blocked.

The snapshot exposes every job: phase (idle, blocked, active,
completed), progress against duration, the current block reason, the
isolation flag, per-requirement reservation state and last-spent
amounts, and the started/completed/missed occurrence counters with the
next window time. The bundled Lua lifecycle mirror receives the same
maintenance descriptors, resolves the installation to its sandbox
object, and runs the same reserve-spend-return ledger inside the
sandbox, so the scripted path and the host ledger agree tick for tick.
The job list persists with the World document at schema 94; pre-94
documents are refused rather than silently dropped, and malformed job
blocks refuse with a diagnostic. The separate Station Systems window
drafts jobs and requirements (add/remove, consumed checkboxes, waste
store), refuses invalid drafts with visible diagnostics, and shows the
live phase, isolation, reservations and spend beside the other
station readouts.

## Scenario forecast (#420)

Where the endurance explanation (#419) extends the OBSERVED current
rate, the scenario forecast extends the estimate to the CONFIGURED
demand/production profiles and the authored future delivery schedule.
It answers, per Resource store: what the drawable inventory is
projected to be when the next resupply lands, whether the store runs
out before then, which service drives it there, and how much of the
schedule the store's capacity can actually accept. The forecast is
derived runtime observability: it is computed from the committed
snapshot state inside `snapshot()` as a pure function, mutates no
live inventory, advances no World clock, consumes no deterministic
stream, touches no Lua sandbox state, and is never persisted - Reset
and load re-derive it from the initial state like every other
observability.

The projection does **not** implement connection-aware transport allocation.
If any resolved physical connection has maximum throughput or efficiency different
from the defaults (1e12 and 1.0), the entire forecast is **unavailable**, with an
explicit reason rather than unconstrained inventory, shortfall, or resupply
promises. It completes no range beyond the snapshot instant and publishes no
store projections. This is deliberately conservative even for an idle connection
or a cap unlikely to bind. A carrier override, including a Non-physical drawing
style on a physical exchange, cannot bypass this check. Explicit default properties
and informational non-physical ratings leave the existing forecast unchanged.
Disabled systems have a separate unavailable reason. Exact forecasts under
non-default physical constraints remain unsupported; the default declared-profile
scenario assumes the standard, very large connection caps are non-binding.

The projection is a declared-profile scenario carried with explicit
labels. It continues the authored transfer rates, crew activity
profile and equipment operating profiles unchanged to the horizon;
a manually commanded generator's fuel draw is projected as
deterministic demand at the full commanded rate over the declared
efficiency - an upper bound on the live burn, which ramp, cooling
trips and fuel throttling can only lower - so a finite fuel store
never reads as sustained supply (#594); scheduled deliveries land
as authored and are accepted only as far as the store's projected
headroom reaches (what cannot fit is refused at the boundary and
reported, never overflowed); scheduled ejections remove only what
the store is projected to hold qualified above its protected
reserve. Every entry publishes the scenario label and the
uncertainty label beside its numbers so no projection can be read
as a guarantee.

The integration is event-driven and bounded. It steps over the
authored schedule occurrences and profile segment breakpoints inside
the horizon, splitting further at projected drawable-zero and
headroom crossings so zero-hits and fill-ups land exactly; between
events every declared rate is constant and each store's trajectory is
linear. The horizon is the fixed `StationForecastHorizonSeconds` and
the whole computation is capped at `StationForecastMaxSegments`
event-driven segments: a schedule dense enough to need more
truncates at the cap and reports `truncated`, so the forecast is
never an unbounded computation regardless of recurrence density.

Coupled availability is projected with the same rules the live ledger
applies at the starvation boundary: a transfer stops delivering when
its source store is projected dry (and its demand is then reported as
uncovered), crew metabolism runs whole or not at all - with any
input store projected dry the crew draws nothing and returns
nothing, so waste streams freeze at the last fed moment - and an
equipment load's whole material-exchange set stops when any consuming
feed is projected dry or any producing destination is projected full.
A delivery due at the same instant the store would run dry rescues
it, matching the live tick order where arrivals run ahead of the
tick's drains.

Uncertainty is explicit and never optimistic. Biology-derived
recovery - crop harvest, algae harvest and microbial nutrient/water
recovery - is a provisional model assumption and is never counted as
guaranteed recovery: stores those pathways could replenish are
flagged `biologyRecoveryPossible` while the guaranteed projection
excludes those inflows entirely. Process-chain production (water
treatment output, electrolysis products, authored conversions, CO2
capture, fuel-cell product water, Sabatier and methane-processing
products) is likewise not projected; destination stores carry
`processProductionPossible` so the gap is visible rather than
implied.

Excluded demand is flagged with the same discipline as excluded
supply (#594). Dropping a consuming pathway would let the live store
fare worse than the projection - the wrong safety direction - so
every consuming pathway is either projected or flagged. Manually
commanded generator fuel draw is projected (above). The pathways
whose draw depends on state the forecast does not re-derive -
fuel-cell reactants, treatment source/consumables,
electrolysis feed water, capture consumables, Sabatier and methane
feeds, authored conversion inputs, consumed maintenance spares, and
Automatic-dispatch generator fuel - are not projected, and every
store they drain carries `processConsumptionPossible`; crop and
algae water/nutrient uptake and microbial feed, oxygen, process
water, culture medium and replacement stock set
`biologyConsumptionPossible`. A Sustained basis beside one of these
flags is not a promise of no shortfall: the omitted draw can make
the live store run short earlier than projected. A store with no
consumption flag carries no omitted drain.

Approximation limits, documented as promised: the forecast projects
store inventories only - atmosphere compartments, thermal state,
coolant loops and the electrical power pool are not re-derived, so
equipment exchanges are projected at their profile-scaled rates
without the live power-pool admission and curtailment, and crew
oxygen draw is assumed satisfiable by the compartment. The
provisional-biology and process-chain exclusions mean a flagged store
may fare better in the live run than the guaranteed projection says,
never worse through hidden help; the consumption flags of #594 close
the other direction, so an omitted draw is never silently possible -
only stores carrying `processConsumptionPossible` or
`biologyConsumptionPossible` can fare worse than projected, and they
say so. The scenario assumes declared
profiles continue unchanged: mid-horizon configuration edits, outage
schedules and maintenance isolation are not folded into the
projection. The projection is a linear relaxation of the coupled
model: it reproduces the starvation-boundary couplings exactly but
not the mid-range allocation throttles (e.g. the fractional
material-cap of #554 between zero and full), so intermediate rates
can differ from the live tick-by-tick outcome while the onset times,
capacity refusals and conservation stay within the declared
tolerance. Every entry carries a `conservationResidual` over
projected physical = start + accepted arrivals + inflow moved -
outflow moved; the residual stays within the balance tolerance and a
larger value is a forecast defect, never silent creation or loss of
material.

The separate Station Systems window renders the forecast read-only:
the scenario and uncertainty labels, the as-of time and horizon with
the segment count and truncation warning, and per store the drawable
and physical start state, the projected net rate, the basis
(sustained/shortfall), the shortfall onset with the limiting service
and uncovered outflow, each projected arrival with scheduled versus
accepted/refused quantity and the projected inventory at arrival
(landing before or after the shortfall), and the biology/process
uncertainty flags. Rendering never ticks, mutates or dirties the
document.

## Explicit scope limit

This slice implements lifecycle, authored finite store configuration,
bounded transfers between stores, compound-resource composition and
conversion accounting, solar-supplied external equipment load
allocation, finite battery storage with charge/discharge losses,
authored scheduled resupply with refused-quantity accounting,
thermal reservoirs with environment-dependent radiator rejection and
exactly-once equipment heat routing, coolant loops with finite
loop-specific inventories, inventory- and power-capped circulation and
freeze-protection heating, protected cross-system service dependencies
that admit containment and biological circulation and authored critical
cooling from actual services ahead of discretionary loads, fuelled
generators with finite
energy-unit fuel inventories, ramp-limited dispatchable output,
startup-energy gating and persistent shutdown decay heat, and
fixed-volume compartment atmospheres with ideal-gas pressure
derivation, occupancy and leak accounting, the vapour saturation
constraint with latent-heat release, scheduled airlock losses with
the recovered-versus-lost split, and heat-only reservoir coupling,
and crew populations that consume food, water and oxygen and return
CO2, moisture, urine, organic waste and metabolic heat under
authored mass-, constituent- and energy-closing coefficients, and water
recovery with collector-side condensation onto a coolant-loop cold surface
and treatment-side recovery into treated water plus retained concentrate
under a full-power qualification rule, and oxygen production by
stoichiometric electrolysis with a chemical-energy-floored electrical dose
and a temperature-gated process-heat sink, plus finite CO2 capture that
removes a compartment's carbon into a capture-store inventory with the
unmet atmospheric service left explicit, plus fuel-cell backup
generation from dedicated hydrogen and oxygen stores with a
declared-efficiency energy split, a temperature-gated waste-heat sink,
reserve-protected reactant draws and a headroom-capped product-water
destination.
It adds no treatment or clearance
workflows or ejection, no dynamic coolant leak, replenishment or
loop-to-loop transfer (a loop's charge is its authored inventory and
nothing in this slice changes it), no powered or passive thermal-buffer
access beyond the authored reservoirs themselves, and no chilled
service distribution beyond the thermal-service-class rollup (no per-consumer
cold-side metering and no cascade or multi-stage refrigeration).
It adds no concentrate disposal, no water distribution to consumers,
no compound-stream water chemistry (water-process stores
are plain mass stores and the model carries no species beyond mass), no
biological or pathogen modelling, and no reprocessing path for held
water: a held batch stays held for use (it is ejectable when explicitly
declared with `heldOnly` or `qualifiedAndHeld`), and redistribution and quality
recovery are later work.
Compartment atmospheres are closed authored volumes: this slice adds
no gas generation or processing (no O₂ generation, CO₂ scrubbing or
combustion), no flow or pressure-driven transfer between
compartments, and no door, hatch or airlock physics tied to
atmosphere state. Completed tick counts are
observability, not oxygen, heat or endurance estimates. It
invents no process coefficients beyond the authored demand, capacity
and profile values. Crew metabolism here is the authored return
contract over the stores and the compartment, not physiology: there
is no health model, no starvation progression and no Agent-linked
population. ECLSS models, alarms,
trends and broader supervisory allocation belong to the subsequent #385
slices. Agent behaviour, movement, physiology and random streams are
unchanged: the crew population is an authored count and never
derived from, coupled to, or synchronised with the World's Agents.

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
loop configurations (unknown host reservoir,
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
crossing without circulation, isolated loop inventories
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
demand unmet; the finite cold-side bound (#560) on a tiny-capacity
loop with a large rating (one tick lands the loop exactly on the
chilled setpoint with work and condenser sized off the bounded duty,
never below, and the unfundable nameplate remainder visible as
unmet), the near-setpoint crossing (the rating binds far from the
interface, the energy cap lands the loop on it, the thermostat gates
afterward, no tick ends below the setpoint, and the extracted total
equals the loop's fundable energy), and the exchanger-shortage
refusal with the no-extractable-heat reason; transient supply loss
through an eclipse cycle with the unmet demand visible during the
outage, recovery announced and ledgers closed across the transitions;
deterministic Reset replay; and intact Lua chiller descriptor
crossing with contained construction refusal, with the bundled
sandbox mirroring the same finite cold-side bound and agreeing with
the host on the bounded tick-one duty to the reported number.

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
