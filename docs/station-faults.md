# Station Faults and Condition (#839, #840)

Implemented slices of [the failure framework](specs/station_degradation_failure_specification.md)
include coolant-leak injection and opt-in coolant-loop Condition with funded recovery.
Wear, Trips, Fault-clearing Repair jobs and other component catalogues remain follow-ups. Propagation follows
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

## Persistence and replay

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
