# Station Faults, Condition, Trips, Repair and Wear (#839–#844)

Implemented slices of [the failure framework](specs/station_degradation_failure_specification.md)
include coolant-leak injection, opt-in coolant-loop Condition with funded recovery,
latching Sabatier/methane processor Trips, repairFault as a runtime-started
maintenance job, permanent Wear accumulators with the battery capacity-fade
pilot, and the thermal-plant Fault catalogue (chiller compressor-failure and
refrigerant-leak, radiator panel-puncture, heat-engine mechanical-seizure).
Other component catalogues remain follow-ups. Propagation follows
[ADR-0030](adr/0030-faults-propagate-through-media-via-hazard-bookings-never-direct-damage-edges.md).

## Authoring and commands

A coolant loop opts in with this optional settings block:

```yaml
coolantLeak:
  compartmentId: machinery-room
  releasedKg: 40
```

`releasedKg` must be finite and positive, and the destination must be an existing
Atmosphere compartment. The code-owned `coolant-leak` mode releases that amount once,
capped at the loop's remaining inventory. No injection occurs merely by enabling it.
The opt-in and destination are frozen topology; the booking amount can be retuned live.
An accepted injection freezes its effective parameters, so subsequent edits cannot
change the pending booking.

```cpp
std::string diagnostic;
bool accepted = runtime.injectFault("cooling-loop", "coolant-leak",
    {core::FaultCause::Kind::AgentAction, "agent:7"}, &diagnostic);
```

World clients can use `World::injectStationFault` to forward the same command without
owning the runtime. Accepted commands mark the document dirty for save.

Cause kinds are `AgentAction`, `ScenarioEvent` and `Editor`; client detail is limited
to 1024 bytes. Attribution grants no authority: gameplay permission checks belong to
the client. Unknown installations, modes, disabled modes, invalid causes and active
or pending duplicates are refused with a diagnostic and no mutation. This slice
permits one active instance per mode per loop. A leak and a degradation-spawned
pump seizure may coexist; neither has a clearing command yet.

Acceptance queues the Fault. On the next successful Station step, the ordinary
transactional ledger books the release and appends one bounded `fault` event. A
failed audit rolls back the booking and event together. Snapshots do not publish a
pending Fault as active.

## Physical and observable effects

The loop loses liquid coolant and its carried sensible energy (`mass * cp * T`,
using the existing loop energy reference). The compartment retains exactly those
amounts in `spilledCoolantKg` and `spilledCoolantEnergyKJ`. This slice models a
retained liquid spill, not evaporation, breathing gas, toxic chemistry or spill/air
thermal equilibration. It invents no vapour or latent heat. Both sides of the mass
and energy transfer are audited; spills also appear in retained coolant inventory
history so system-wide material charts do not lose the released mass.

A coolant-leak Fault's remaining inventory fraction caps availability. Scheduled Outages and
maintenance isolation compose with that cap by minimum, **not multiplication**.
Electrical supply can further constrain actual circulation. An Outage restoring
availability never replenishes coolant or clears the Fault. Non-Faulted loops keep
their previous fill/Outage behavior unchanged.

`coolantLoops[].activeFaults` publishes `installationId`, `modeId`, `onsetTick`,
`causeKind`, `causeDetail` and `derivedImpact`. For this pilot mode, impact is the
lost fraction of rated circulation, `1 - remainingKg / capacityKg`; it is derived,
never authored. The loop also publishes cumulative leaked mass and energy and its
thermal residual. Service-criticality-weighted aggregation is not introduced by
this slice.

## Condition and funded recovery

A coolant loop independently opts in with `condition` (`InstallationConditionSettings`):

```yaml
condition:
  initialCondition: 1
  overheatStressC: 80
  overheatDegradePerHour: 0.02
  cavitationFillFraction: 0.25
  cavitationDegradePerHour: 0.05
  seizureThreshold: 0.15
  restoreStoreId: service-stock
  restorePerHour: 0.04
  stockUnitsPerConditionPoint: 10
```

Rates are provisional engineering assumptions in Condition points per hour. The
initial Condition and fill/seizure thresholds lie in [0,1]; a starting Condition
below the seizure threshold is refused. Rates are finite and non-negative; a
funding store requires positive recovery rate and stock cost. The store must
exist. Model presence and the funding-store identity are frozen topology; model
parameters can be retuned live.

Heat stress acts at or above the authored coolant temperature. Low-fill
cavitation acts at or below the fill threshold on a non-empty, non-seized pump
with non-zero available flow capacity. Active stressors add their authored
loss rates; removing them stops further degradation on the next interval.
As with microbial recovery, an active stressor bars recovery even when its
loss-rate coefficient is zero. The current
Condition continuously multiplies pump demand and circulation capability,
including protected cooling credits. These are interval-sampled stressors, not a
blanket timestep-invariance claim for coupled thermal feedback.

Crossing strictly below `seizureThreshold` raises `pump-seizure` once, with cause
`DegradationCrossing` and a bounded `fault` event. Its availability cap is zero,
composed by minimum with Outages, maintenance and any leak cap. Clients cannot
forge `DegradationCrossing` through `injectFault`.

```cpp
bool accepted = runtime.restoreCondition("cooling-loop", 0.9, &diagnostic);
```

This headless runtime command queues a target strictly above current Condition.
It takes effect at the next successful Station boundary, never instantly. Only
benign intervals recover: each restored point spends the declared stock cost,
using qualified stock above the store's protected reserve. Recovery is bounded
by its per-hour rate, target and available stock; competing loops cannot spend
the same stock twice. Exhaustion leaves an unmet target pending; a new unfunded
command is refused with a diagnostic. Start/completion use bounded `repair`
events. Recovery does **not** clear an existing Fault or replenish coolant.

Snapshots expose `condition`, `conditionCapability`, `lastConditionDelta`,
`conditionStressors`, `restoring`, `restoreTargetCondition` and cumulative
`restoreStockConsumedUnits`. A loop without the model remains at full Condition
with no recovery, stress or new wire fields. Failed audits roll back Condition,
stock spending, spawned Faults and events together.

## Latching Trips

Sabatier reactors and methane processors opt into the code-owned
`overtemp-runaway` mode with `std::optional<OvertempRunawaySettings>`:

```yaml
overtempRunaway:
  triggerTemperatureC: 80
  heatSpikeKJ: 100
```

The threshold must be finite and above absolute zero; the spike must be finite
and positive. Opt-in presence is frozen topology, while both parameters can be
retuned live. Opted-in Installation IDs must be unambiguous. A sink at or above
this threshold at a Station boundary automatically latches a Trip, even if the
ordinary cooling gate has already stopped processing. Clients may also call
`injectFault(id, "overtemp-runaway", cause)`. An accepted injection freezes its
spike and takes effect at the next successful Station boundary; pending Trips
are not yet published as latched and cannot be reset.

On latch the installation draws no process electricity or feed and stays isolated
after the sink recovers. The one-shot spike is an **external hazard energy input**
to the declared sink, not invented electrical supply or normal reaction heat.
The reservoir ledger includes it exactly once as `hazardEnergyKJ`, including it
in total heat input and the audited energy residual. Failed audits roll back the
latch, heat and event together. There are no downstream damage edges.

`runtime.resetTrip(id, &diagnostic)` (or `World::resetStationTrip`) clears a latched
Trip immediately, without stock, power, or elapsed time. Normal service resumes
on the next step if its ordinary gates permit it. Resetting while the threshold
still holds is legal, but the next step latches again with a fresh spike. Reset
is refused without mutation for pending/untripped installations and non-Trip
Faults. It never clears a coolant leak or pump seizure. Bounded `trip` events
record each latch and reset; `tripped` and `activeFaults` on both processor
snapshots expose the latch, onset, cause and full isolation impact.
`ThermalGate` is the simulation-only automatic cause, never client-injectable.

Existing generator cooling and Heat engine sink gates remain auto-clearing;
no opt-in means precisely the previous behavior. Autonomous threshold evaluation
is boundary-sampled, not a claim of timestep invariance for thermal feedback.

## Repair jobs

A coolant loop opts into repair with a `repairParts` block: a per-Installation
parts store and a per-mode repair plan (duration, parts quantity, waste store).
Only `coolant-leak` and `pump-seizure` are repairable; a latching Trip is
cleared with `resetTrip`, not repair.

```yaml
repairParts:
  partsStoreId: repair-parts
  repairs:
    - modeId: coolant-leak
      durationSeconds: 600
      partsQuantity: 2
      wasteStoreId: maintenance-waste
```

`runtime.repairFault(id, mode, &jobId, &diagnostic)` (or
`World::repairStationFault`) starts the repair as a runtime-instantiated #414
maintenance job and returns its job id. The repair reuses the established
maintenance semantics exactly: the Installation is isolated at zero availability
for the full duration, the authored parts are exclusively reserved from the parts
store, completion requires the full duration to have run with the reservation
held, and a reservation drawn away by a competing process interrupts the job,
discards its progress and forces a full rerun. A repair's window opens with the
command and never closes, so an interrupted repair keeps its occurrence until it
can run in full. Completion clears the target Fault (lifting its availability cap)
and books the consumed parts into the plan's waste store as qualified,
disposal-ready material. Repair never reverses Wear and never replenishes
coolant.

The command is refused whole, with a diagnostic and no mutation, on an unknown
installation or fault mode, a mode with no repair plan, a target Fault that is not
present, a repair already running for that Fault, or parts/waste headroom that
cannot currently be reserved. The store identities and the plan set are frozen
topology; plan durations and parts quantities are live-tunable but never reach a
repair that already started. Snapshots expose the repair in the maintenance
picture with kind `repair`, the target `faultModeId`, phase, progress and the
per-requirement reservation/spend. Bounded `repair` events record start,
interruption and completion; a `fault` event records the clear.

## Wear

An Installation opts into permanent Wear with an `InstallationWearSettings`
block. Fade rates are fractions of rated capacity per accumulator unit and
must be finite and non-negative with the ceiling in `(0, 1]`; at least one
rate must fade, and a rate for an accumulator the machine cannot physically
wire is refused: a battery flows no counted mass, a coolant loop is not
cycle-counted.

```yaml
batteries:
  - id: bank
    capacityFade:
      fadePerCycle: 0.0005        # fraction of rated capacity per equivalent full cycle
      fadePerCalendarHour: 0.00002 # fraction per simulated hour, working or resting
      maxWearFraction: 0.6        # permanent clamp on total fade
```

The battery is the capacity-fade pilot: equivalent full cycles are counted
on real charge and real discharge - a full charge plus a full discharge of
the authored cyclable energy is one cycle, so each side of the round trip
banks half of what it moves - plus calendar ageing that acts while the bank
sits idle. The permanent `wearFraction` scales the whole rating:
effective nameplate, usable and reserve are the authored values times
`(1 - wearFraction)`. A faded bank fills only to its faded ceiling,
delivers from no more than it, and its protected floor keeps its authored
ratio. Energy stored above a ceiling the fade overtook stays fully accounted
but stranded until discharge brings the bank back under it.

A coolant loop opts in the same way with `wear`: it accrues operating hours
while coolant actually circulates and throughput of the mass actually
moved, plus calendar time, and its `wearFraction` scales the effective
rated flow. The fade rides the same continuous capability scale as
Condition - flow, pump demand and every protected credit size against the
faded rating - and composes with Outages and Fault caps unchanged.

Each interval's fade is priced at the rates current when the operation
happened, so a live rate retune never reprices fade already accumulated;
presence of the model is frozen topology while the rates stay live-tunable.
The fade is monotonic: nothing heals it. Repair clears Faults and restores
Condition but leaves `wearFraction` exactly where it found it - a repaired
battery keeps its faded rating, a repaired loop keeps its faded flow - and
only replacement, a topology change (a reset), starts a machine unworn.
Wear is not a Fault: it cannot be injected, repaired or reset, and
`injectFault` refuses `capacity-fade` with a diagnostic.

Snapshots report the accumulators, the fade and the effective ratings for
every battery and loop; a machine without the model reports zero wear and
effective ratings equal to the authored ones. Wear state is replay state:
a save of a worn machine carries the fault-replay block even with no client
ever issuing a command, and the fade reconstructs exactly through YAML and
binary replay and World save/load.

## Thermal-plant Fault catalogue

The #844 slice extends the catalogue to the thermal plant with four code-owned
modes. Each is opt-in per Installation and injectable through the same
`injectFault` seam with the same refusal discipline (unknown installation,
cross-kind mode, absent opt-in, invalid cause, active or pending duplicate).

Chillers opt in with `compressorFailure: true` (a plain availability-zero mode)
and/or a `refrigerantLeak` block; radiators with a `panelPuncture` block; heat
engines with `mechanicalSeizure: true`.

```yaml
chillers:
  - id: chiller
    compressorFailure: true
    refrigerantLeak:
      compartmentId: machinery-room
      chargeKg: 40          # the finite refrigerant charge the leak starts from
      leakRateKgPerS: 0.5
      specificHeatKJPerKgK: 1.6
radiators:
  - id: roof-panel
    panelPuncture:
      compartmentId: machinery-room
      coolantKg: 20         # the finite panel coolant charge
      leakRateKgPerS: 1.0
      specificHeatKJPerKgK: 4.0
heatEngines:
  - id: orc
    mechanicalSeizure: true
```

`compressor-failure` and `mechanical-seizure` remove the machine from service:
availability zero, no cold delivery / no conversion, no electricity drawn or
supplied by the failed path. They are the AV=0 shape and carry no parameters
beyond the opt-in itself.

`refrigerant-leak` and `panel-puncture` are finite-store modes. The machine
carries a charge store (refrigerant charge, panel coolant charge) initialized
from the authored size. While the Fault is active the leak drains the store at
the authored rate through the ordinary transactional ledger: the lost mass and
its carried sensible energy (`mass * cp * T`, at the loop/reservoir temperature
the charge sits in) book into the declared Atmosphere compartment exactly as a
coolant spill does, and both sides are audited. The remaining charge fraction
is the machine's availability cap: cooling capacity (chiller) and radiative
rejection area (radiator) scale with `remaining / initial`. The store is
finite - a long-running leak drains to empty and the service dies with it;
nothing is invented. The leak runs only while the Fault is active; the charge
itself is initialized at construction from the authored block.

All four modes follow the pending/active doctrine: acceptance queues the Fault,
the first successful Station step logs the onset `fault` event and publishes
it as active; snapshots never publish a pending Fault. `derivedImpact` is the
availability the Fault removes: 1.0 for the AV=0 modes, the drained fraction
for the finite-store modes.

Repair follows the #842 pattern with `repairParts` wiring on chillers, radiators
and heat engines; `repairFault` starts the maintenance job, isolates the
machine for the full duration and on completion clears the Fault. Sealing a
leak stops the bleed but never refills the store: the capacity stays capped at
the charge that remains until replacement. Repair of the AV=0 modes restores
full service. All four modes are repairable when wired; a plan naming a mode the
machine cannot suffer (or a loop-only mode like `coolant-leak` on a chiller) is
refused at authoring validation.

## Persistence and replay

Schema 117 carries the thermal-plant Fault opt-ins and repair wiring; earlier
documents carrying them are refused. Thermal-plant runs pin their initial
settings for replay at construction, so a live retune of a leak rate cannot
make the replay block disagree with the frozen parameters its injection
commands carry. Feature-absent output keeps its previous schema tier and
encoding.

Schema 116 carries Wear opt-ins; earlier documents carrying a
`capacityFade` or loop `wear` block are refused. Worn machines retain the
replay state even before any client command, so save/load preserves the
accumulated fade exactly. Wear-absent output keeps its previous schema and
encoding.

Schema 114 carries Trip opt-ins; earlier documents carrying them are refused.
Enabled Trip models retain replay state even before any client command, so
save/load preserves automatic latches as well as injected ones and resets.
The command journal freezes injected spike sizes and replays reset ordering.
Trip-absent output retains its previous schema and encoding.

Schema 113 carries Condition settings and restore commands; pre-113 documents
carrying a Condition block are refused. Running Condition models retain the replay
block even without a client command, preserving autonomous degradation on load.
Disabled models remain authored-only. Condition-absent documents retain their
previous schema and encoding (112 for coolant-leak opt-in, otherwise 111).

Schema 112 accepts the opt-in settings and optional `stationFaultReplay` block.
Feature-absent World and Station configuration writers retain schema 111 and their
previous byte representation; old fixtures need no migration. Readers refuse
pre-112 documents carrying the new settings or runtime block.

`StationSystemsRuntime::writeFaultReplay` / `readFaultReplay` work with both YAML
and binary serializers. The runtime block contains the initial configuration,
completed-step count and ordered command journal, including live settings edits
in opted-in runs. Reconstruction drives the same audited runtime to each command
boundary, then to the saved step. This restores active and pending Faults, causes,
effective parameters, inventories, bookings and events without injecting material
outside the ledger. The final replay settings must agree with the saved document;
malformed commands are refused before replacing any caller-owned runtime. World
save/load includes this block when its runtime has accepted injections. A resumed
Station advances immediately from its restored time even though the reconstructed
movement World starts at zero. Reset
starts a new run from authored settings rather than retaining injected Faults.

## Verification

Release CTest checks `station-faultInjection`, `station-faultOutages`,
`station-faultReplay`, `station-faultOptIn` and `station-faultTransactions` use only
runtime commands, snapshots, history, events and persistence boundaries. They cover
atomic refusal, pending/active duplicates, attribution, conservative dry/partial
releases, protected/discretionary circulation, Outage composition, rollback,
multiple Station ratios, retained inventory history, live retuning and exact
YAML/binary replay of scripted multi-loop injections.

The six `station-condition*` checks cover stress removal, continuous capability,
protected circulation, independent/coexisting Fault modes, stock-bounded recovery,
reserves, shared-stock contention, transactional rollback, supported step ratios,
settings validation, pending/completed restore replay and autonomous Condition
World save/load through both serializers.

`station-tripLifecycle` and `station-tripTransactions` cover both processor types,
automatic and injected onset, persistent isolation after sink recovery, coexistence
with auto-clearing generator/Heat engine gates on the same sink, free instant reset,
invalid and duplicate commands, conservative heat, rollback, parameter retuning,
opt-in no-op behavior, supported step ratios, schema guards, exact YAML/binary
replay and World save/load/reset.

`station-repairLifecycle`, `station-repairInterruption`, `station-repairTransactions`
and `station-repairSeizureClearing` cover the repair starter: whole-command refusal
(unknown installation/mode, Trip mode, no plan, no fault, duplicate, unreservable
parts, no waste headroom), zero-availability isolation for the full duration,
exclusive parts reservation with a competing drain that interrupts and forces a
full rerun, completion that clears the Fault and books consumed parts as qualified
waste, the `repair` snapshot kind with requirement detail, `repair`/`fault` events,
live-topology freeze with parameter retuning that cannot reach a started repair,
repair of a degradation-spawned pump-seizure without re-raising, opt-in no-op,
supported step ratios, the schema-115 guard, exact YAML/binary replay and World
save/load with a repair in flight.

The six `station-thermalPlant*` checks cover the thermal-plant catalogue:
refusal of unknown/cross-kind/unopted injections with no mutation, the
finite refrigerant charge draining through the ledger into the compartment
with the remaining-charge capacity cap and full drain-to-zero, compressor
and seizure availability-zero effects, panel puncture spill booking and the
remaining-area rejection cap, deepest-wins composition with Outages, repair
isolation and completion that seals leaks without refilling the store, live
rate retuning that cannot reach a frozen injection, frozen charge/destination
opt-in topology, authoring validation of malformed blocks, the schema-117
round-trip and guard, and exact YAML/binary replay of a scripted sequence
across all four modes with repairs in flight.
