# Station Degradation and Failure Specification

Status: design agreed 2026-07-31; not yet implemented.

Canonical vocabulary: `CONTEXT.md` § Station systems — **Installation**, **Condition**,
**Degradation**, **Fault**, **Failure**, **Trip**, **Repair**. This spec uses those terms
strictly. *Broken* stays reserved for traversal devices and never names a station state.

Scope: Station systems Installations only. World traversal objects (Doors, Lifts,
Stairs, Escalators, extensibles) are out of scope; their Broken model is unchanged.

## 1. State model

Two layers per Installation, both runtime state:

1. **Condition** `[0,1]` — generalizes the #408 microbial pattern to any Installation.
   Condition scales the capability the Installation can deliver. It is moved by
   Degradation (while adverse conditions act) and by funded recovery. It is not
   health, not material quality.
2. **Fault ledger** — the set of active Faults on the Installation. Each entry:
   `{ faultModeId, onsetTick, causeRecord, effectiveParameters }`.
   Exactly **one active instance per (Installation, fault mode)**; injecting a mode
   that is already active is refused with a diagnostic. Re-damage after repair is a
   fresh instance with a new onset and cause.

Each Installation also carries **wear accumulators** (`operatingHours`, `cycles`,
`throughputKg` as applicable) that drive a permanent **wearFraction** capacity fade.
Wear is permanent within a run: Repair never reverses it; only replacement does, and
replacement is a topology change (a reset, per the live-edit contract), not a live edit.

**Availability composition.** A Fault's availability reduction joins the existing
sources — scheduled Outages (#413) and maintenance isolation (#414) — under the
established rule: the deepest reduction wins. Repair work isolates exactly as
maintenance isolates.

## 2. Fault onset sources

- **Degradation crossing.** A catalogue mode may declare a Condition threshold at
  which it spawns (e.g. `pump-seizure` when Condition < 0.15). The crossing is
  event-logged with cause kind `DegradationCrossing`.
- **External injection.** A sudden Fault arrives only as an explicit injected
  command: an Agent deliberately damaging equipment (via the client), a scenario
  event, or an editor/debug action. The cause record names the kind
  (`AgentAction | ScenarioEvent | Editor`) and any client-supplied detail.
- **No RNG.** The station layer contains no random failure draws in v1. Emergent
  failure timing comes from Degradation crossings; a hazard-rate layer can be added
  later without disturbing this design.

**Authority.** The simulation validates structural legality only: Installation
exists, the fault mode is in that component type's catalogue, the mode is not
already active. Whether a given Agent could reach or was permitted to strike a
given Installation is client-owned gameplay logic; the sim records the cause and
never judges it.

## 3. Propagation: media and hazard bookings

Components never damage each other directly. All propagation is through the
existing shared media — Thermal reservoirs, Atmosphere compartments, Resource
stores, the electrical pool — via each component's own gates, caps, and stressors.

A Fault's onset mechanism into a medium is the **hazard booking**: a catalogue mode
may declare one-shot and/or rate bookings (mass, species, heat) that enter the
medium through the same ledger path as every normal flow.

Properties:

- A booking is an accounted flow. The per-tick conservation audit closes on it or
  flags `healthy = false`. Nothing appears or vanishes.
- Propagation stays emergent: `thermal-runaway` does not "damage the crew"; it
  heats and contaminates the compartment, and the crew's own stress mechanisms
  react. A puncture affects everything coupled to the loop, declared or not.
- There is no shadow dependency graph of who-can-hurt-whom. See ADR-0030.

## 4. v1 fault catalogue (code-owned)

Effect shapes: **AV** availability reduction · **TRIP** latching protective trip ·
**BOOK** hazard booking · **WEAR** wear accumulator coupling.

| Component | Fault modes |
|---|---|
| Coolant loop | `coolant-leak` (BOOK into compartment/vacuum; AV via inventory fraction), `pump-seizure` (AV=0; spawnable from Condition crossing) |
| Chiller | `compressor-failure` (AV=0), `refrigerant-leak` (finite charge store; AV scales with charge) |
| Radiator | `panel-puncture` (coolant-loss BOOK; area AV) |
| Generator | `failed-start` (AV=0 for the attempt), `fouled-injector` (efficiency WEAR). Existing trip / fuel-depletion / decay-heat behavior unchanged |
| Heat engine | `mechanical-seizure` (AV=0), `efficiency-drift` (carnotFraction WEAR) |
| Battery | `capacity-fade` (WEAR: cycle + calendar), `thermal-runaway` (one-shot heat + toxic-species BOOK into compartment; AV=0) |
| Solar array | `cell-degradation` (WEAR), `string-loss` (fractional AV), `tracking-failure` (fixed low AV) |
| Electrolyzer | `membrane-degradation` (WEAR: specific energy rises), `catalyst-poisoning` (triggered by feed-water contaminant content — external cause via medium, WEAR effect) |
| Water treatment | `membrane-fouling` (throughput WEAR), `breakthrough` (BOOK concentrate species into treated store, unqualified) |
| CO2 capture | `absorbent-degradation` (WEAR) |
| Fuel cell | `stack-degradation` (WEAR), `catalyst-poisoning` (from H2 impurity) |
| Sabatier / methane processor | `catalyst-deactivation` (WEAR), `overtemp-runaway` (TRIP + heat-spike BOOK) |
| Atmosphere compartment | `fire` (BOOK: O2 draw + CO/CO2 production + heat rate; self-extinguishes below O2 threshold). Constant authored leaks stay as-is |
| Airlock | `interrupted-cycle` (stranded/vented gas booked as loss), `interlock-failure` (fast breach BOOK to Exterior vacuum) |

Excluded by decision:

- **Biology keeps its existing model.** Microbial condition/degradation (#408, #570)
  and crop/algae death (#410/#411) are untouched; retrofitting them would churn
  passing fixtures for no modeling gain.
- **Crew illness** is deferred: it needs a crew health model, a separate initiative.
- **Sensor/observability faults** (reported ≠ true) are out of v1; snapshots stay
  truthful. The catalogue is shaped so this can grow in later as a named fault class.

Catalogue ownership: code owns the fault modes and their effect shapes (only code
can guarantee conservative accounting); authors opt each mode in per Installation
and tune parameters (trigger thresholds, booking sizes, severity multipliers,
repair requirements).

## 5. Repair API (client-facing)

Three tiers, all on `StationSystemsRuntime`, all validated whole before any
mutation, applied at the tick boundary, event-logged, and serialized to the
command log for replay:

1. **`resetTrip`** — free, instant. Legal only on a latched Trip; refused with a
   diagnostic otherwise.
2. **`repairFault`** — starts a repair as a **runtime-instantiated #414 job**:
   same isolation-at-zero semantics, same exclusive reservation of authored repair
   parts (per-Installation repair-parts store + requirements from the fault's
   catalogue entry), same interruption-if-reservation-lost rule (progress
   discarded, full duration rerun). Returns the job id; the snapshot's maintenance
   picture grows a `repair` kind beside the authored jobs. One job type, two
   starters — no parallel implementation.
3. **`restoreCondition`** — funded recovery drawing a service-stock store above
   its protected reserve, bounded per-hour, extending the #408 recovery pattern to
   any Installation with a Condition.

Repair clears Faults and restores Condition; it never reverses Wear.

Sketch (illustrative signatures, final names at implementation):

```cpp
struct FaultCause {
    enum class Kind { AgentAction, ScenarioEvent, Editor } kind;
    std::string detail;            // client-supplied: agent id, scenario event name, ...
};

bool injectFault(std::string_view installationId, std::string_view faultModeId,
                const FaultCause& cause, std::string* diagnostic = nullptr);
bool resetTrip(std::string_view installationId, std::string* diagnostic = nullptr);
bool repairFault(std::string_view installationId, std::string_view faultModeId,
                std::string* jobIdOut = nullptr, std::string* diagnostic = nullptr);
bool restoreCondition(std::string_view installationId, double targetCondition,
                     std::string* diagnostic = nullptr);
```

## 6. Backward compatibility

Strictly **opt-in per Installation**: new fields (Condition model, wear rates,
enabled fault modes, repair-parts wiring, thresholds) absent ⇒ the component
behaves exactly as today. All existing scenario fixtures replay byte-identical.

Existing per-tick gates stay exactly as they are: the generator cooling gate and
heat-engine sink gate keep their auto-clear behavior. The latching Trip exists only
inside the opt-in fault model (e.g. `overtemp-runaway` latches). Old gates are
gates; new Trips latch.

## 7. Serialization and replay

- Runtime state serialized in save/binary replay: Condition, wear accumulators,
  wearFraction, active fault ledger, trip latches, repair-job state.
- Client commands (`injectFault`, `resetTrip`, `repairFault`, `restoreCondition`)
  enter the command log like other client commands; replay reproduces every onset,
  booking, interruption, and repair exactly.
- Catalogue changes bump the station schema version (current: 111).
- Authored opt-in parameters travel in Settings; runtime fault state never does.

## 8. Observability

- Snapshot per Installation: `condition`, `wearFraction`, `availabilityFactor`
  (composed), and `activeFaults[]` with `{ modeId, onsetTick, causeKind,
  derivedImpact }`.
- **Severity is derived, never authored**: `derivedImpact` composes availability
  lost against the service class / criticality of what the Installation feeds.
  Clients alarm on the derived figure; it cannot drift from reality when
  parameters are retuned.
- Event log categories: `fault` (onset, clear), `trip` (latch, reset),
  `repair` (start, interrupt, complete), using the existing bounded event log.

## 9. Editor surface

API-first: v1 ships the settings fields and headless commands only. The station
diagram editor surface — fault badges on nodes, alarm panel, opt-in authoring
controls — follows as its own slice once the simulation feature is fully usable
headless via commands and snapshots.

## 10. Test strategy

- **Opt-in no-op**: every existing fixture replays byte-identical with the feature
  absent.
- **Per fault mode**: onset (injection and Condition-crossing), effect shape,
  hazard-booking ledger closure, repair-tier legality.
- **Cascade scenario**: radiator puncture → loop inventory cap → chiller duty cap
  → sink rise → generator gate refusal, with every step accounted.
- **Repair interruption**: repair-job reservation drawn away → discarded progress →
  full rerun, mirroring #414 coverage.
- **Replay determinism**: a scripted sequence of injections and repairs replays
  exactly through YAML and binary.
- **Conservation audit**: every hazard booking closes under the per-tick audit;
  a deliberately broken booking trips `healthy = false`.
