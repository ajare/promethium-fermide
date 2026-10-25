# Station Power Buses Specification

Status: design agreed 2026-10-25 (grilling session; model, trip semantics, and migration
settled). Initiative captured in Hindsight as
`kp-7afd12541ed34229a83784104e72e38c`. Not yet implemented.

Canonical vocabulary: `CONTEXT.md` § Station systems. This spec introduces **Power Bus**,
**Supply Mode**, and **Bus Trip**, and uses the existing **Trip** / **Alert** / **Fault** /
**Alarm** nouns strictly per ADR-0031
(`docs/adr/0031-monitors-observe-only-alerts-are-code-bound-callbacks.md`). It supersedes
the "automatic electrical pool membership" clause of ADR-0023
(`docs/adr/0023-separate-station-connection-design-from-flow-allocation.md`) with an
explicit authored bus topology; a new ADR-0032 records that supersession.

## 1. Problem statement

All station electrical power today lives in one implicit, derived, un-wireable
generation-and-battery pool. Every source (solar arrays, fuelled generators, fuel cells,
heat engines) and every battery feeds it automatically; every load draws from it in one
global priority pass. There is no way to:

- isolate power for different station functions (a fault or overload in one group of
  equipment is felt station-wide);
- give a critical load a guaranteed supply path distinct from discretionary loads;
- combine capacity from separately-rated generation;
- reason about, size, or protect any distribution path smaller than "everything".

The single pool is a modelling convenience that no industrial power system uses. Real
installations distribute through rated bus sections with protected feeders, transfer
switches, and annunciated trips.

## 2. Solution summary

Replace the single implicit pool with user-authored **Power Buses**: placeable, wireable
station nodes with a declared rated capacity. Every power-producing component energizes one
or more buses through a "Power out" connector; every consuming component draws from one or
more buses through a "Power in" connector with an explicit **Supply Mode** (Single,
Transfer, Shared). Dispatch becomes per-bus: each bus balances its own supply against its
own demand, protected services first. A bus whose sustained through-power exceeds its
rating **trips** its breaker: the bus de-energizes whole, latches off until a manual
reset, and raises a latched Critical **Alert** directly through the #864 alert-callback
registry — never through a Monitor.

Existing documents migrate to one default `main` bus that every source and load joins, so
behaviour is identical to today until the user deliberately splits it.

## 3. Industrial model mapping

| Industrial practice (IEC 61439 switchgear, shipboard/spacecraft distribution) | This spec |
|---|---|
| Bus section / busbar — common-voltage junction, rated ampacity | **Power Bus** node with `ratedCapacityKW` |
| Generator feeder with source breaker onto the bus | Source's `Power out → bus` connection; the connection *is* the closed breaker |
| Load feeder with breaker from the bus | Load's `Power in ← bus` connection |
| ATS / STS — automatic transfer switch, failsafe, whole-load transfer, never combines | `SupplyMode::Transfer` (primary + backup) |
| Paralleled feeds combining capacity | `SupplyMode::Shared` (≥2 buses, pro-rata) |
| Breaker = one protected circuit; "everything this breaker controls" | The connection + its breaker state; not a separate placeable node |
| Breaker trips on sustained overcurrent, latches open until reset | **Bus Trip**: threshold + persistence, latched, manual `BreakerReset` |
| Trip annunciation on the alarm panel | Latched Critical Alert via the code-bound alert callback registry |
| Bus tie / bus coupler breaker between sections | **Deferred** (see §12) |
| Voltage class, per-feeder ampacity, I²t curves | **Deferred** — kW-only model (see §12) |

Two structural rules carried over from the grilling session:

1. **Devices are parallel taps, never series.** Every source, storage unit, and load is an
   independent rung across the bus rails. There is no ordering along a bus, no
   upstream/downstream device-to-device dependency, and no voltage-drop modelling.
2. **Breakers are properties of connections, not separate nodes.** The only device-shaped
   exception in the model is the transfer switch, and it is expressed as the load's Supply
   Mode, not as a placeable node.

## 4. The Power Bus

A Power Bus is a new authored, placeable Station node type in the **Power** group:

```
PowerBusSettings {
  id                     // stable, unique within the station configuration
  name                   // display name, e.g. "Emergency Bus", "Science Bus"
  ratedCapacityKW        // > 0 for a rated bus; 0 = unrated (no trip enforcement)
  tripPersistenceSeconds // >= 0; time over rating before the breaker trips (0 = instant)
}
```

- **Rated bus** (`ratedCapacityKW > 0`): the enforced ceiling on sustained through-power.
- **Unrated bus** (`ratedCapacityKW == 0`): unlimited, never trips. This is how the
  migrated default `main` bus arrives, preserving today's exactly-unbounded behaviour; a
  user may also deliberately leave a new bus unrated. The UI shows unrated buses with an
  explicit "unrated" marker so it never reads as an oversight.
- `tripPersistenceSeconds` is the trip's threshold+time shape — the same vocabulary the
  Monitor framework uses for Alert Rules — deliberately not an I²t thermal curve.
- The bus has no inventory, no profile, no wear. It is pure distribution.

### 4.1 Members

Three roles attach to buses. The contract is **role-based, not type-based**: any component
that produces, stores, or consumes electrical power implements the corresponding connector,
so future producers (RTG, fission surface power, whatever comes next) join with zero
bus-layer changes.

**Sources** energize one or more buses (`busIds`, ≥1). Today: Solar, Generator, Fuel cell,
Heat engine. A source's output lands on each of its buses per its normal dispatch; feeding
multiple buses is paralleling and is legal. A source's rated power remains its feeder limit
— no separate source-feeder rating exists in v1.

**Storage** (Battery) is the bidirectional member: it charges from and discharges into
each assigned bus (`busIds`, ≥1). Battery charge/discharge efficiency, reserve floor, and
thermal behaviour are unchanged; they are simply scoped per bus. A battery on a dedicated
bus is the classic battery-backed emergency bus.

**Loads** draw from one or more buses (`supplyBusIds`, ≥1) under a Supply Mode (§5).
This includes **internal consumers** — chillers, coolant loop pumps and freeze heaters,
water treatment, electrolyzers, CO2 capture, Sabatier reactors, methane processors,
microbial processors, crop and algae lighting/circulation — not just External equipment
loads. Every internal power-consuming machine carries bus membership (default `main`).
Per-bus protected-first (§6) is meaningless unless the protected services physically sit
on a bus.

## 5. Supply Modes

```
enum class StationSupplyMode { Single, Transfer, Shared }
```

### 5.1 Single
Exactly one supply bus. The load draws entirely from that bus. This is the default for
migrated loads.

### 5.2 Transfer (ATS)
Exactly two distinct supply buses: index 0 is **primary**, index 1 is **backup**.

- The load draws wholly from the primary while the primary can fully serve it.
- If the primary cannot fully serve the load this tick (shortfall of any size), the **whole
  load transfers** to the backup. An ATS does not split a load; it switches it.
- If the backup also cannot fully serve, the load is served under the normal per-bus
  priority rules on the backup and the shortfall is explicit unmet.
- **Return-to-primary** requires the primary to be able to fully serve the load for a
  continuous `transferReturnDwellSeconds` (declared per load, default 30 s). Without the
  dwell, a marginal primary would flap the switch every tick.
- Every transfer and return is a recorded **transfer event** (load id, from bus, to bus,
  sim time) in the snapshot and on the event stream.
- Validation refuses: primary == backup; fewer or more than two buses in Transfer mode.

### 5.3 Shared
Two or more supply buses, combining capacity.

- The load's demand is split **pro-rata across its buses by each bus's available spare**
  (post-protected-service spare capacity this tick). A bus with zero spare takes zero share.
- The split is recomputed every tick; it is a dispatch result, not a static allocation.
- If the combined spare cannot cover the demand, the residual is explicit unmet on the
  load. Shared mode combines what exists; it never fabricates.
- A Shared load is never partially energized below its minimum operable supply: the
  existing minimum-operable rule (demand × (1 − maxCurtailment)) applies to the combined
  grant exactly as it does today.

## 6. Per-bus dispatch

The single global priority pass becomes one dispatch pass **per bus**, per Station step.
Within one bus, the allocation order reproduces today's ladder scoped to that bus:

1. **Protected critical services first, within the bus.** Containment circulation,
   biological circulation, authored critical cooling, and shutdown-cooling obligations
   draw ahead of everything else *on that bus*. A protected service's bus is the bus of
   the machine it protects (e.g. a coolant loop's pump protects on the coolant loop's
   bus).
2. **Discretionary loads by priority**, exactly as today (higher first, curtailment
   limits, shed/restore recovery machine unchanged), against the bus's remaining supply.
3. **Battery charging** from surplus only, with the existing recovery-surge hold-out.

Hard rules:

- **No cross-bus rescue.** A bus's spare capacity never serves another bus's protected
  load. If bus A has surplus and bus B's protected pump is unmet, bus B reports unmet
  critical and the station alarm stands. The design guidance this enforces on users:
  put protected loads on a bus with guaranteed supply (typically battery-backed).
- Cross-bus support exists only through membership: a source energizing both buses, or a
  battery bridging them by charge/discharge policy. Never through implicit pooling.
- The existing protected-service snapshot shape (`StationProtectedServiceSnapshot`) is
  retained per bus.
- Transfer-mode loads count against the bus they are currently drawing from, not their
  nominal primary.

## 7. Bus Trip

A bus with a rating enforces it as a live protective device, not a polite cap.

### 7.1 Detection
Through-power is the sum of everything actually delivered through the bus this tick: all
load draws plus all internal service draws. (Battery charge/discharge flows are accounted
as through-power on the affected bus.)

### 7.2 Trip condition
If through-power exceeds `ratedCapacityKW` continuously for `tripPersistenceSeconds`,
the bus breaker **trips**:

- every source connection to the bus opens (sources stop feeding; a generator so isolated
  follows its normal shutdown/decay-heat rules on its other buses if any);
- every battery connection to the bus opens (the bus cannot drain or charge through the
  trip);
- every load on the bus loses supply: their draw becomes explicit unmet demand under the
  normal unmet reporting (protected loads on the tripped bus report unmet **critical** —
  this is why the validator warns, see §7.4);
- the trip **latches**. It does not auto-reclose when the overload clears. Auto-reclosing
  onto a persistent fault is how you burn switchgear.

### 7.3 Reset
A tripped bus is restored only by an explicit **`BreakerReset`** command: a validated
World-facade command (available headless for scenarios, and via the diagram/inspector UI).

- Reset closes the breaker. If the overload condition still exists, the bus re-trips after
  another full `tripPersistenceSeconds` window — reset is not a veto over protection.
- Reset is undoable/redoable as one command, like all station edits.

### 7.4 Authoring-time warnings (non-fatal)
The validator warns (does not refuse) when:

- a bus carries protected-service demand exceeding its `ratedCapacityKW` — the protected
  loads cannot survive a trip of this bus;
- a bus has loads but no energizing source and no battery;
- a Transfer load's backup bus has no energizing source and no battery.

Warnings surface in the inspector and do not block save.

## 8. Trip → Alert (the #864 boundary)

Per ADR-0031 the four nouns stay hard-separated: *Alarm* (internal scalar), *Fault*
(adverse state), *Trip* (self-protection), *Alert* (observation result).

- The trip **raises an Alert directly** through the existing code-bound alert-callback
  registry (`stationAlertCallbacksFor`), as a new alert type, e.g.
  `bus-breaker-trip`: subject = the bus, severity **Critical**, payload = rated capacity,
  through-power at trip, sim time.
- The Alert is **not Monitor-driven**: it requires no authored Monitor, cannot be
  disabled, and does not wait for oversight to be switched on. A tripped breaker nobody
  hears about is exactly what annunciation exists to prevent.
- The Alert **latches with the breaker**: it does not auto-clear on metric recovery. It
  clears only when the breaker is reset, with clear reason `BreakerReset`.
- This makes the Alert ledger have two producers: Monitors (authored, optional) and Trips
  (machine protection, always-on). #864's callback-registry design absorbs this without
  schema change. Nothing in the simulation consumes the Alert for actuation — the trip
  itself already actuated; the Alert only announces.
- Transfer events (§5.2) announce on the event stream as **Advisory**-severity alerts
  through the same registry (type `supply-transfer`), auto-clearing after a fixed short
  duration. They are informational, not latched.

## 9. Snapshot and telemetry

`StationPowerSnapshot` is restructured from one station-wide account into per-bus rollups
plus a station total — consistent with the existing "rollup is per class, never a single
pooled number" philosophy:

```
StationBusPowerSnapshot {
  busId
  ratedCapacityKW            // 0 = unrated
  loadFraction               // throughPower / rated, 0 when unrated
  generationKW               // sources energizing this bus, delivered this tick
  batteryChargeKW / batteryDischargeKW
  demandKW                   // presented demand (protected + discretionary + transfer-in)
  protectedDemandKW / protectedSuppliedKW / protectedUnmetKW
  suppliedKW / unmetKW
  surplusKW
  tripped                    // bool
  trippedAtSimSeconds        // 0 when not tripped
  throughPowerKW
}

StationPowerSnapshot {
  buses: [StationBusPowerSnapshot]   // one per bus
  // station-wide totals preserved for existing consumers:
  totalGenerationKW, totalDemandKW, totalSuppliedKW, totalUnmetKW,
  unmetCriticalKW, batteryChargeKW, batteryDischargeKW,
  dispatchMode, dispatchTarget/Committed/Shortfall (aggregated),
  protectedUnmet (true if any bus protectedUnmet)
}
```

Per-connection telemetry published alongside, shaped as future #864 Monitor subjects
(catalogue entries land with the #864 track, not this spec):

- per-source-connection: delivered kW, breaker state (closed/open/tripped-with-bus);
- per-load-connection: drawn kW, supply bus actually serving (per transfer state);
- **reverse-power flag** on source connections: structurally zero in v1 (dispatch never
  pushes current backwards into a non-charging source), published as a constant-false
  field so the future Monitor catalogue has the subject without a schema break;
- transfer events (§5.2) as above.

Determinism: all of the above is a pure function of settings, prior state, and the tick's
published values. Identical configs with identical perturbations replay identically.

## 10. Ports, authoring, validation

### 10.1 Ports (`StationPorts.h`)

- New placeable type: `StationPlaceableType{"Power bus", "bus", "Power"}`.
- Bus node ports:
  - `Bus in` (Input, kind `power`, counterpart `power-out`, unbounded) — source landings.
  - `Bus out` (Output, kind `power-out`... see note) — load taps.
  - Note: the bus is a common node; both faces are modelled as one unbounded pair so any
    number of sources and loads land on it. Battery uses both faces (charge in, discharge
    out) on the same bus connection semantics.
- Source nodes (Solar, Generator, Fuel cell, Heat engine, and future producers): new
  **required** `Power out` port, kind `power-out`, counterpart `power`. Sources that
  previously had no electrical port gain exactly one.
- Battery: `Power out` and `Power in` connectors, each unbounded across buses.
- Loads (Equipment and every internal consumer machine): new **required** `Power in`
  port, kind `power`, counterpart `power-out`.
- The derived `Power` node type is **retired**: `stationIsDerivedNodeType` drops it.
  Because migration guarantees a `main` bus exists in every document, the graph always
  has at least one authored bus; nothing needs the derived node.

### 10.2 Validation (`StationSystemsValidation.cpp`)

Refusals (no partial state, diagnostic message):

- duplicate bus id;
- `ratedCapacityKW < 0` (0 is legal = unrated; negative refused);
- `tripPersistenceSeconds < 0`;
- source with zero `busIds`, or naming a nonexistent bus;
- battery with zero `busIds`, or naming a nonexistent bus;
- load with zero `supplyBusIds`, or naming a nonexistent bus;
- Transfer mode with != 2 buses, or with primary == backup;
- Single mode with != 1 bus;
- Shared mode with < 2 buses.

Warnings (§7.4) are non-fatal.

### 10.3 Diagram behaviour

- Buses are ordinary authored nodes: selectable, wireable, renameable with cascade
  (rename/delete cascades bus references exactly like links).
- A load or source with an unwired `Power in`/`Power out` reads as incomplete (required
  port), consistent with existing required-port treatment.
- A tripped bus renders distinctly (dead styling + latched-trip marker) and offers the
  `BreakerReset` action.

## 11. Migration

- `kWorldSchemaVersion` bumps 122 → 123.
- On load of a pre-bump document:
  - one default bus is created: `id = "main"`, `name = "Main Bus"`,
    `ratedCapacityKW = 0` (unrated — preserves today's exactly-unbounded behaviour),
    `tripPersistenceSeconds = 0`;
  - every source, battery, and load (internal consumers and Equipment loads) receives
    `main` as its sole bus membership;
  - every load receives `SupplyMode::Single`.
- Consequence: a migrated station dispatches identically to the pre-bus single pool —
  same order, same results, same snapshots modulo the new per-bus wrapper (one bus).
  Existing tests asserting dispatch behaviour must pass unchanged against a migrated
  document.
- Save round-trips the authored bus structure natively; the migration is load-time only.

## 12. Deferred (recorded, not built)

- **Bus ties / bus couplers.** An authored open/close breaker between two buses;
  tie-closed buses dispatch as one pool. Deferred because it adds a stateful
  island/merge machine, and the resilience it provides is already available source-side
  (a source energizing two buses) and load-side (Transfer).
- **Per-feeder breaker tripping.** A load's feeder rating is redundant with its declared
  demand; a source's with its rated power. Per-feeder tripping becomes interesting only
  with fault scenarios and rides the #845-style fault track.
- **Voltage modelling, voltage classes, I²t curves.** The model is kW-only, as the rest
  of station electrical is.
- **Monitor catalogue entries for buses and power connections.** This spec publishes the
  telemetry (§9); the default Monitor tables land with the #864 track.
- **Alert-driven actuation** — excluded by ADR-0031, unchanged.

## 13. User stories

1. As a station designer, I want to place multiple Power Buses and wire sources and loads
   to them explicitly, so that power distribution is a designed topology rather than an
   invisible pool.
2. As a station designer, I want each bus to carry a rated capacity, so that I can size
   distribution honestly.
3. As a station designer, I want a source to energize one or more buses, so that I can
   parallel generation across buses or dedicate it to one.
4. As a station designer, I want any future power-producing component to join buses
   through the same role contract, so that the bus layer never changes when new machines
   arrive.
5. As a station designer, I want a load to draw from a single bus, so that the common
   case stays simple.
6. As a station designer, I want a load with a primary and backup bus (Transfer), so
   that a failsafe supply exists without combining capacity.
7. As a station designer, I want a load to draw from multiple buses at once (Shared), so
   that I can combine capacity that no single bus carries.
8. As a station designer, I want a battery on a dedicated bus, so that I get a
   battery-backed emergency bus.
9. As a station designer, I want protected services to sit on the bus of the machine they
   protect, so that "protected" means protected *on that bus*, with no illusion of
   cross-bus rescue.
10. As a station designer, I want the validator to warn when a bus cannot cover the
    protected load sitting on it, so that I am told where my design lies to me.
11. As an operator, I want a sustained overload to trip the bus breaker and kill the bus,
    so that protection behaves like a real breaker rather than a silent cap.
12. As an operator, I want a tripped bus to announce a latched Critical Alert I cannot
    have disabled away, so that a dead bus is impossible to miss.
13. As an operator, I want to reset a tripped breaker from the diagram or inspector, and
    know that a persistent overload will simply re-trip, so that reset is safe.
14. As an operator, I want transfer events visible on the event stream, so that I can see
    when a failsafe fired.
15. As an operator, I want per-bus power rollups (supply, demand, unmet, load fraction,
    trip state) in the station window, so that I can read each circuit's health.
16. As a scenario author, I want bus trip/reset and supply modes available headless in
    settings and snapshot, so that I can script brownout and failover scenarios.
17. As a scenario author, I want dispatch to be deterministic per bus, so that replays
    match.
18. As a returning user, I want my pre-bus station to load with one unrated `main` bus
    and behave exactly as before, so that migration is a no-op until I choose otherwise.
19. As a developer, I want the Trip/Alert/Monitor/Fault boundaries kept strict, so that
    review can refuse noun cross-use (ADR-0031).
20. As a future contributor, I want the bus/feeder telemetry documented as future Monitor
    subjects, so that the #864 catalogue extension needs no archaeology.

## 14. Testing decisions

- **Primary seam: StationSystemsRuntime + StationSystemsSnapshot** (the fault/monitor
  framework seam). Headless, Release only, `ctest -j 16`:
  - migrated document loads to exactly one unrated `main` bus and dispatches
    identically to pre-bus behaviour (existing dispatch assertions pass unchanged);
  - per-bus balance: two buses, independent sources/loads, each balances alone;
  - no cross-bus rescue: bus A surplus does not serve bus B's protected load; B reports
    unmet critical;
  - Transfer: failover on primary shortfall, whole-load transfer, return dwell honoured,
    transfer events recorded;
  - Shared: pro-rata split by spare, zero-spare bus takes zero, combined shortfall
    explicit, minimum-operable rule honoured;
  - trip: sustained overload trips after persistence, instant trip at persistence 0,
    bus dead (all loads unmet, protected unmet critical), sources isolated, latch holds
    through metric recovery, `BreakerReset` restores, re-trip on persistent overload;
  - trip Alert: fires Critical without any Monitor present, clears only on
    `BreakerReset`;
  - battery: per-bus charge/discharge, battery-backed bus survives source loss, battery
    isolated on tripped bus;
  - save/load/undo/redo round-trips of all authored bus fields and memberships;
  - validation refusals leave no partial state.
- **Editor seam: ImGui Test Engine** (precedent: #849 fault-surface checks): place/wire a
  bus, unwired required power port reads incomplete, transfer/shared authoring, tripped-bus
  styling and reset click, per-bus rollup display.
- New smoke files registered in the smoke ownership manifest.

## 15. Implementation phasing

1. **Data model & migration** — settings structs, `StationSupplyMode`, schema bump
   122→123, load-time default-bus migration, `CONTEXT.md` vocabulary, ADR-0032.
2. **Ports & validation** — Power bus placeable type, source/load/battery power
   connectors, derived-`Power` retirement, validator rules and warnings.
3. **Per-bus dispatch** — tick-stage rework (Prepare/Generation/Protection/Processing),
   transfer and shared policies, per-bus protected-first, trip machine + `BreakerReset`
   command, per-bus audit in `StationTickAuditPower.cpp`.
4. **Snapshot & alerts** — `StationBusPowerSnapshot`, station totals, trip→Alert
   callback binding, transfer-event alerts, event-stream announcements.
5. **Editor surfaces** — diagram node/wiring/trip styling/reset, bus inspector, source
   and load bus-membership UI, station-window per-bus rollups.
6. **Tests** — headless + ImGui seams above, smoke manifest registration.
