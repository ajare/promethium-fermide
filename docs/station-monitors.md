# Station Monitors: atmosphere, store and connection defaults (#867, #871)

`atmosphere` is the registered Monitor subject type, and it publishes the full
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
