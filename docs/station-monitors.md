# Station Monitors: defaults and subject inspectors (#867, #871–#876)

`atmosphere` is a registered Monitor subject type, and it publishes the full
spec §3.1 Metric set: `oxygenPartialPressureKPa`, `carbonDioxidePartialPressureKPa`,
`pressureKPa`, `temperatureC`, `relativeHumidity`, `toxicConcentration` (derived:
retained toxic mass per cubic metre), `oxygenUnmetKg` and `leakRateKgPerS`.
`stationAtmosphereMetricCatalogue()` / `findStationAtmosphereMetric()` are the single
source of truth for which names are valid: validation, evaluation and surfaces all
resolve there. A Monitor observes only: it cannot change supplies, operate a hatch,
trip equipment, or create Faults.

## The §4.1 default table

`stationAtmosphereHabitabilityTemplates()` carries the twelve habitability defaults
in spec order, and `stationAtmosphereDefaultMonitors(settings)` expands that table
over every atmosphere compartment in a configuration — twelve disabled, provenance-
tagged Monitors per compartment, with deterministic `<subjectId>-<key>` ids
(`room-po2-low`, `hub-compartment-leak`, …).

| Key | Metric | Rule | Severity | Provenance |
|---|---|---|---|---|
| `po2-low` | `oxygenPartialPressureKPa` | Below 17.5 kPa, 30 s / 60 s | Critical | `iss` |
| `po2-high` | `oxygenPartialPressureKPa` | Above 24 kPa, 60 s / 60 s | Warning | `iss` |
| `pco2-high` | `carbonDioxidePartialPressureKPa` | Above 0.7 kPa, 300 s / 120 s | Warning | `iss` |
| `pco2-critical` | `carbonDioxidePartialPressureKPa` | Above 1.0 kPa, 60 s / 120 s | Critical | `iss` |
| `pressure-low` | `pressureKPa` | Below 90 kPa, 10 s / 60 s | Critical | `iss` |
| `pressure-high` | `pressureKPa` | Above 110 kPa, 60 s / 60 s | Warning | `prov` |
| `temperature-high` | `temperatureC` | Above 27 °C, 600 s / 300 s | Advisory | `iss` |
| `temperature-low` | `temperatureC` | Below 18 °C, 600 s / 300 s | Advisory | `iss` |
| `humidity-high` | `relativeHumidity` | Above 0.75, 600 s / 300 s | Advisory | `prov` |
| `toxic-contaminant` | `toxicConcentration` | Above 0.1 g/m³, 60 s / 120 s | Critical | `prov` |
| `oxygen-supply-unmet` | `oxygenUnmetKg` | Above 0 kg/tick, 10 s / 30 s | Critical | `rated` |
| `compartment-leak` | `leakRateKgPerS` | Above 0.01 kg/s, 60 s / 120 s | Warning | `prov` |

`applyStationAtmosphereDefaultMonitors(settings)` appends the defaults a
configuration is missing. It is idempotent and non-destructive: a Monitor whose id
is already present keeps its authored values, so a designer's per-Monitor tuning
survives a re-apply and the table never overwrites hand-authored configuration.
The expansion is pure — it reads the configuration and authors settings, and never
touches runtime state. A configuration with no compartments expands to nothing.

## Provenance

Every Monitor carries a `MonitorProvenance` tag: `iss` (crew-realistic, from ISS
limits), `who` (drinking-water guideline), `rated` (a fraction of the subject's own
rated/authored value), `prov` (provisional engineering estimate), or `custom` for a
hand-authored value making no provenance claim. `stationMonitorProvenanceTag()` is
the one spelling, shared by the wire format (`provenance: iss`) and by surfaces.
The tag is stored per Monitor, round-trips with it, and travels onto the published
Alert, so the dashboard can show whether a limit is grounded or an estimate without
looking the Monitor up again. Documents written before #867 carry no tag and read
back as `custom`.

## Authoring

Station configuration schema 122 adds this optional `stationSystems` collection:

```yaml
monitors:
  - id: room-oxygen-low
    subjectType: atmosphere
    subjectId: room
    metric: oxygenPartialPressureKPa
    rule:
      threshold: 17.5
      direction: Below
      triggerOnSeconds: 30
      triggerOffSeconds: 60
    severity: Critical
    enabled: true
    provenance: iss
```

`subjectId` must name an existing subject (see store and connection identities below). IDs must be non-empty
and unique; thresholds must be finite; both times must be finite and non-negative;
the metric must be one the selected subject type publishes; the provenance tag must be
one of the five known spellings. Directions are `Above` and `Below`; severities are
`Advisory`, `Warning` and `Critical`. `enabled` defaults to false. Old documents
without Monitors retain their existing wire schema. YAML and binary save only the
authored rules, not Alert history or persistence timers.

## Evaluation and commands

Each successful Station step evaluates its published snapshot once. Strict
inequality is outside; equality is inside. Each sample represents that completed
step's duration. Fire/clear occurs at the first sample completing the required
continuous duration, without interpolation or a value deadband. A contrary
sample resets that pending duration. Zero times fire/clear at the first matching
sample, never just from querying the snapshot.

Use `StationSystemsRuntime::updateSettings` (headless) or
`World::updateStationSystemsSettings` (World facade) with a changed per-Monitor
`enabled` flag. Validation happens before any mutation. Enabling starts fresh
with the next Station step, with no credit for previous disabled time. Disabling
immediately closes an active Alert at current Station time with
`MonitorDisabled`, and drops pending timers. A changed rule/subject/severity
starts fresh too, closing an active Alert with `MonitorChanged`; removing a
Monitor closes it with `MonitorDisabled`.

`StationSystemsSnapshot::activeAlerts` and `closedAlerts` publish Monitor identity,
subject, Metric, latest observed value, threshold, severity, provenance and
`firedAtSeconds`.
Closed entries also carry `clearedAtSeconds` and `clearReason` (`AutoClear`,
`MonitorDisabled`, or `MonitorChanged`). Code-bound callbacks record the ledger
and fire/clear announcements in the bounded `alert` event stream. The ledger is
run-scoped; reset/load starts fresh, including when physical Fault state is
restored by replay.

## Headless demonstration

Run the focused Release scenarios:

```sh
ctest --test-dir build-linux -C Release -j 16 -R '^station-monitorDemo$' --output-on-failure
```

The twelve defaults are proven headless at the StationSystemsRuntime + snapshot seam:

```sh
ctest --test-dir build-linux -C Release -j 16 \
  -R '^station-monitorDefault(Table|Boundaries)$' --output-on-failure
```

`monitorDefaultTable` checks the expansion against the spec §4.1 numbers — twelve
per compartment, every compartment covered, all shipped disabled, each value and
provenance tag exact, re-application idempotent and non-destructive, and the whole
expanded set round-tripping through YAML, binary and the Station configuration
document. `monitorDefaultBoundaries` drives each of the twelve in turn from real
published values — starting beyond the bound, then brought back inside with a live
authored lever (volume, occupancy, leak rate, exchanger conductance) or, for the
toxic-contaminant Metric which has no authored lever, booked by a real battery
thermal-runaway Fault — and asserts the Alert fires only after the full trigger-on
persistence and auto-clears only after the full trigger-off persistence counted
from the first sample actually inside, with the right severity and provenance on the
ledger.

The original tracer-bullet scenario:

The scenario authors a room at 100 m³, 20 °C with 20 kg O₂ and 80 kg N₂
(pO₂ about 15.2 kPa), the rule above, and a closed 25 m² pathway to a second
100 m³ compartment with 80 kg O₂. It demonstrates no Alert before 30 sim-seconds,
then a Critical Alert at 30 seconds. A validated live settings command opens the
pathway; real gas diffusion replenishes room oxygen. After 60 consecutive
sim-seconds of recovered samples the Alert auto-clears. The test asserts the
actual published pO₂ and ledger, not callback internals.

## Store and connection defaults (#871)

`stationMonitorMetricCatalogue(subjectType)` lists names and units for all three
registered subject types. Validation, evaluation and the dashboard use that same
catalogue. `stationStoreMonitorTemplates()` and `stationConnectionMonitorTemplates()`
provide the §4.2 / §4.8 registry rows; `stationStoreConnectionDefaultMonitors(settings)`
expands them, and `applyStationStoreConnectionDefaultMonitors(settings)` appends only
missing IDs, preserving author tuning. Like atmosphere defaults, expansion is an
explicit authoring operation, not a runtime side effect. Every row starts disabled.
No persistence schema change is needed.

| Subject | Metric | Threshold | On / off (s) | Severity | Provenance |
|---|---|---|---|---|---|
| store | usableFraction | Below 0.25 | 120 / 60 | Warning | rated |
| store | usableFraction | Below 0.10 | 60 / 60 | Critical | rated |
| water store/stream | totalDissolved | Above 500 mg/L | 60 / 60 | Critical | who |
| water store/stream | unexpectedSpecies | Above 0 (any species above 1 mg/L) | 60 / 60 | Warning | prov |
| connection | lossFraction | Above 0.05 | 120 / 60 | Warning | rated |
| connection | lossRate | Author-set (disabled zero placeholder) | 120 / 60 | Warning | custom |
| connection | reversed | Above 0 | 1 / 1 | Critical | prov |
| connection | utilizationFraction | Above 0.9 | 300 / 120 | Advisory | rated |

A `store` subject uses the store ID. A `connection` subject uses
`stationConnectionMonitorSubjectId(key)`: all six parts of its full design key,
each prefixed by its decimal byte length and a colon. This is deterministic and
unambiguous even when authored IDs contain punctuation. Node rename retargets
these references without renaming Monitors; removing/disconnecting a connection
removes its Monitors in the same command.

### Metric sources and units

* `usableFraction`: published `currentQualified / capacity`, zero for zero
  capacity. Held stock is excluded, but protected qualified reserve is included.
* Store water metrics: published `concentrations`, the actual constituent blend
  of qualified plus held stock. `totalDissolved` sums non-water, non-energy
  species; `maxContaminantConcentration` takes their maximum; `unexpectedSpecies`
  is 1 if that maximum is strictly greater than 1 mg/L, otherwise 0. There is
  deliberately no species allow-list: a designer need not name the contaminant.
* The native concentrations are kg species/kg stream; multiply by 1,000,000
  to obtain mg/L under the model's water-density approximation of 1 kg/L.
  Water and the chemical-energy slot never count as contaminants.
* `lossFraction`: published `lastLossRate / lastInputRate`, zero at zero input.
  `lossRate` reads `lastLossRate` in the connection's native rate unit.
* `reversed`: published direction flag mapped to 1/0 (not inferred from losses).
* `utilizationFraction`: published `lastInputRate / maximumThroughput` from the
  matching full-key design properties; omitted design uses the existing default
  throughput. Zero throughput reads zero rather than dividing by zero.
* Water-stream metrics read the feed-quality snapshot of Crew, Electrolyzer,
  Microbial processor, Crops or Algae water inputs. Water treatment raw-feed
  quality is reconstructed from last passed + removed constituent mass divided
  by last processed mass; treated-stream quality uses passed / treated mass,
  and concentrate uses removed / concentrate mass. Crew greywater uses its
  published last constituent loads / greywater mass; microbial recovered water
  uses last carried constituents / recovered water mass. These readers never
  substitute a source store's closing blend for what actually flowed.
  A stream without carried quality, or without flow, supplies no sample; pending
  persistence resets and an active Alert cannot falsely clear on missing data.

Water defaults are offered for explicitly identified `water` stores and stores
connected to water-medium ports (and their water-carrying connections). There is
no separate authored potable classification: the trace template is offered on
all such water subjects, including potable supplies, and remains disabled until
the designer opts in. No name-based classification or compatibility barrier is
introduced. A plain Resource transfer publishes no carried quality, so its
quality rules remain unsampled rather than guessing a contaminant fraction.

Focused Release proofs:

```sh
ctest --test-dir build-linux -C Release -j 16 --output-on-failure \
  -R '^station-monitor(StoreConnectionTable|StoreBoundaries|ConnectionBoundaries|WaterStream)$'
```


## Thermal plant (#873)

`stationThermalPlantMonitorTemplates(subjectType)` registers four coolant-loop,
three chiller, two heat-engine, and two radiator rules. Expansion via
`stationThermalPlantDefaultMonitors(settings)` resolves authored temperature
bounds, and `applyStationThermalPlantDefaultMonitors(settings)` appends missing
IDs without replacing authored rules. Every rule starts disabled. Thermal node
inspectors list these defaults without modifying the draft; an accepted edit
materializes them through the validated command. Rename/delete uses the same
subject mapping as the inspector and cascades Monitor references.

| Subject | Metric | Unit | Source |
|---|---|---|---|
| `coolant-loop` | `fillFraction`, `circulationFraction`, `condition` | fraction | same-named coolant-loop snapshot fields |
| `coolant-loop` | `loopTemperatureC` | °C | same-named snapshot field |
| `chiller` | `coolingUnmetFraction` | fraction | snapshot `coolingUnmetKW / coolingDemandKW`; zero with no demand |
| `chiller` | `sinkOverMaxCondensing` | boolean | snapshot flag mapped to 1/0 |
| `chiller` | `refrigerantFraction` | fraction | snapshot `refrigerantKg / refrigerantChargeKg`; 1 with no finite-charge opt-in |
| `heat-engine` | `tripped` | boolean | snapshot `status == StationHeatEngineStatus::Tripped`; mechanical seizure alone is not a Trip |
| `heat-engine` | `hotHeadroomC` | °C | snapshot `hotTemperatureC - minHotTemperatureC`, using the machine's published interface temperature |
| `radiator` | `remainingAreaFraction` | fraction | same-named radiator snapshot field; surviving coolant-bearing panel area |
| `radiator` | `reservoirTemperatureC` | °C | temperature of the radiator's supplying reservoir in the same snapshot |

Coolant, chiller and engine rules match specification §4.5. Freeze risk expands
to the authored freeze-protection setpoint + 2 °C. Radiator duty capability is
watched by an area-low Advisory below 0.9 (on 120 s, off 60 s, **[rated]**),
which catches panel-puncture coolant loss. Its temperature Advisory watches above
the supplying reservoir's authored maximum minus 5 °C (on/off 120 s,
**[prov]**). These are capability and temperature symptoms, not a guess at
unpublished demanded radiator duty. Resolved thresholds remain authored values;
reapplying defaults never silently retunes an existing Monitor.

Headless coverage uses coolant-leak, refrigerant-leak and panel-puncture Faults
and physical temperature gates, asserting each default's fire boundary at both
0.25 s and 1 s timesteps and protection/gradient auto-clear boundaries. A
compressor failure removes cooling demand as well as capacity, so it does not
fabricate unmet-demand telemetry. The cooling-unmet case instead injects a
coolant leak that restricts circulation while cooling demand remains present.

```sh
ctest --test-dir build-linux -C Release -j 16 --output-on-failure \
  -R '^station-(thermalPlantMonitors|editor-monitor-inspector)$'
```

## Power sources (#874)

`stationPowerSourceMonitorTemplates(subjectType)` registers five `battery`, three
`generator` and one `solar-array` default. `stationPowerSourceDefaultMonitors(settings)`
expands them with deterministic `<type>-<id>-<key>` identities;
`applyStationPowerSourceDefaultMonitors(settings)` appends missing IDs without
replacing author tuning. The inspector lists these disabled defaults without
modifying the draft; accepted edits materialize them through the validated command.
Rename/delete cascades use the same subject mapping. No schema change is needed.

| Subject / key | Metric | Threshold | On / off (s) | Severity | Provenance |
|---|---|---|---|---|---|
| battery / soc-low | stateOfCharge | Below 0.20 | 60 / 30 | Warning | iss (depth-of-discharge practice) |
| battery / reserve-floor | stateOfCharge | Below 0.10 | 10 / 30 | Critical | rated |
| battery / headroom-gone | stateOfCharge | Above 0.95 | 300 / 60 | Advisory | rated |
| battery / discharge-shortfall | dischargeShortfallFraction | Above 0.05 | 30 / 30 | Warning | rated |
| battery / wear-fade | wearFraction | Above 0.20 | 0 / 0 | Advisory | prov |
| generator / output-off-command | outputDeviationFraction | Above 0.05 | 60 / 60 | Warning | rated |
| generator / sink-near-limit | sinkTemperatureC | Above authored maxSinkTemperatureC − 5 | 60 / 120 | Warning | rated |
| generator / startup-shortfall | startupUnmetKW | Above 0 kW | 30 / 10 | Advisory | rated |
| solar-array / available-low | availableFraction | Below 0.5 | 300 / 120 | Advisory | prov |

Metric sources are the subject's published power snapshot:

* `stateOfCharge` and `wearFraction`: same-named battery fields. SOC is stored
  energy divided by nameplate, not by usable capacity; the 0.10 rule is the
  specified fixed SOC bound, not a dynamically inferred reserve floor.
* `dischargeShortfallFraction`: `lastDischargeShortfallKW / maxDischargeKW`,
  using the unit's rated terminal discharge limit, not its wear-reduced capacity.
* `outputDeviationFraction`: `abs(outputKW - commandedOutputKW) / ratedOutputKW`.
  Output is instantaneous endpoint output, not interval-average generation; the
  published command includes dispatch when dispatch owns it.
* `sinkTemperatureC` and `startupUnmetKW`: same-named generator fields.
  Unfunded commanded starts and loss of support mid-crank publish the missing
  cranking power in `startupUnmetKW`; no fuel/cooling refusal invents that deficit.
  This reporting does not change startup qualification, draw, or phase transitions.
* `availableFraction`: solar `availableKW / capacityKW`, including illumination,
  outage, wear and Fault effects. Darkness is deliberately observable too.

Each zero-rated denominator reads zero rather than dividing by zero. Missing
subjects supply no sample. Permanent Wear has no physical recovery: its Alert
stays active until the author changes/disables the rule or replaces the bank.
Resolved sink thresholds stay authored values on reapplication.

Focused Release proof (all defaults at 0.25 s and 1 s steps):

```sh
ctest --test-dir build-linux -C Release -j 16 --output-on-failure \
  -R '^station-(powerSourceMonitors|editor-monitor-inspector)$'
```

## All-subject inspectors (#875)

The Flow inspector shares the atmosphere Monitor controls across generators,
solar arrays, batteries, coolant loops, chillers, heat engines, radiators,
water treatments, electrolyzers, CO2 capture, Sabatier reactors, methane
processors, fuel cells, microbial processors, Resource stores, crop populations,
algae cultures, and selected Station connections. Connection subjects use the
full design-key identity, not the displayed link label.

Rendering expands the currently available default tables on a temporary copy;
selecting a subject alone changes neither the draft nor its history. ECLSS/biology subjects include their §4.6/§4.7 defaults (#876). Atmosphere
pathways, Thermal links, and schedules remain outside this surface.

All sections use the same provenance tags, enabled toggle, threshold, direction,
trigger times, and severity controls. Accepted edits pass through
`World::editStationMonitor`, materialize missing defaults, and participate in the
existing draft history and configuration save/load. No running World is changed.

`station-editor-monitor-families` drives every additional panel family through
ImGui Test Engine, checking default listing/provenance or the empty state,
toggle and timing edits where defaults exist, undo/redo, and save/load.
`station-editor-monitor-inspector` retains the full atmosphere control and
validation workflow. Both are focused Release tests; no stability run is needed.

## Bulk enable/disable (#877)

Open **Bulk Monitor controls** in any component or Station connection's Monitors
section. **Enable all** / **Disable all** operate on that component, its subject
type, or the entire station. Each click is one configuration-history step;
undo restores the complete previous mixed flag set, redo reapplies it, and
repeating an unchanged command adds no history.

The validated `World::setStationMonitorsEnabled(draft, enabled, diagnostic,
subjectType, subjectId)` command selects one subject with both identifiers,
a type with only `subjectType`, or the station with both empty. Unknown scopes
and invalid Monitor sets are refused without partial changes. Like individual
edits, it materializes omitted disabled defaults; thereafter only the selected
per-Monitor enabled flags change, never rules, severity, or provenance.

These are draft controls: use the existing **Apply** path to update the running
World. Applying disable closes affected active Alerts immediately with
`MonitorDisabled`; applying enable starts fresh persistence from that point,
without backfill. No persistent component/type/station enable gate is introduced.

Focused Release checks: `station-monitorBulkCommands` covers scopes, validation,
flag-only changes, closeout and fresh evaluation; `station-editor-monitor-inspector`
drives all three scopes and single-step undo/redo through the real controls.

## ECLSS machines and biology (#876)

`stationEclssBiologyMonitorTemplates(subjectType)` and
`stationEclssBiologyDefaultMonitors(settings)` provide the §4.6/§4.7 tables.
`applyStationEclssBiologyDefaultMonitors(settings)` appends missing IDs only,
using `<type>-<id>-<key>`. Inspector preview and accepted Monitor commands use
this same expansion. All defaults start disabled and carry **rated** provenance.

| Subjects | Key / metric | Rule | Severity |
|---|---|---|---|
| `water-treatment`, `electrolyzer`, `co2-capture`, `sabatier`, `methane-processor`, `fuel-cell`, `microbial-processor` | service-unmet / unmetFraction | Above 0.1, 120 s / 60 s | Warning |
| `microbial-processor` | condition-low / condition | Below 0.5, 300 s / 120 s | Advisory |
| `crop`, `algae` | biomass-low / biomassFraction | Below 0.2, 3600 s / 600 s | Warning |
| `crop`, `algae` | resource-starvation / unmetResourceFraction | Above 0.1, 600 s / 300 s | Warning |

Only microbial processors publish Condition among these seven machine types;
no Condition is invented for the others. Wear is not Condition.

All new metrics are dimensionless fractions derived from published telemetry:

* Machine `unmetFraction` is `(demanded - delivered) / demanded`, clamped to
  [0, 1], with zero for zero demand. Electrolyzer, CO2 capture, Sabatier,
  methane and microbial processing use `lastRequestedKg` against respectively
  `lastSplitKg`, `lastCapturedKg`, `lastCo2ConsumedKg`,
  `lastMethaneConsumedKg`, and `lastFeedConsumedKg`. These include material
  and output-headroom shortages, not just power-only unmet counters. A gate
  that publishes no requested service supplies zero demand.
* Water treatment uses `throughputKgPerS * systemsTimestepSeconds` against
  `lastProcessedKg`. Fuel cells use `lastOutputKW + lastUnmetOutputKW` as
  demanded service and `lastOutputKW` as delivered service.
* Microbial `condition` reads the processor's published `condition` directly.
* `biomassFraction` is `livingBiomassKg / capacityKg` (zero at zero capacity).
* `unmetResourceFraction` is the maximum of growth-resource shortage,
  requested lamp-power shortage, requested circulation-power shortage, and
  requested respiration shortage. Each power/respiration fraction is
  `(requested - supplied) / requested`, clamped to [0, 1], zero with no demand.
  The growth-resource term is published as `lastResourceUnmetFraction`: one
  minus the minimum supplied fraction across CO2, irrigation, nutrients,
  water quality and circulation, captured **before** growing-capacity limiting.
  Temperature dormancy and full growing capacity are not resource starvation.
  A deliberately dark illumination profile requests no light or growth
  resources; lamps requesting but receiving no power do report starvation.
  The term is read-only tick-derived telemetry, never authored or persisted.

Focused Release proofs cover exact tables, metric mappings, idempotence and
preserved tuning; real machine fire/clear timing; crop and algae light/CO2
starvation at 0.25 s and 1 s steps; disabled silence, biomass timing and
configuration persistence; and the existing real inspector/history workflow:

```sh
ctest --test-dir build-linux -C Release -j 16 --output-on-failure \\
  -R '^station-(eclssBiologyMonitor(Table|Boundaries)|editor-monitor-families)$'
```
