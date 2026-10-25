# Station Monitoring and Alerts Specification

Status: design agreed 2026-10-25 (grilling session; seams confirmed). Not yet implemented.

Canonical vocabulary: `CONTEXT.md` § Station systems — **Monitor**, **Alert Rule**, **Alert**,
plus **Metric**, **Installation**, **Fault**, **Trip**, **Alarm** (reserved for pre-existing
internal scalars). This spec uses those terms strictly. *Broken* stays reserved for traversal
devices. A Monitor never actuates: see ADR-0031,
`docs/adr/0031-monitors-observe-only-alerts-are-code-bound-callbacks.md`.

## 1. Model

A **Monitor** is a read-only authored observation rule over one subject:

```
Monitor {
  id            // stable, unique within the station configuration
  subjectType   // component class or "connection"
  subjectId     // the component/connection id being watched
  metric        // named Metric from that subject type's catalogue
  rule: AlertRule {
    threshold        // finite value in the Metric's unit
    direction        // Above | Below
    triggerOnSeconds // >= 0; persistence required to fire (0 = instant)
    triggerOffSeconds// >= 0; persistence back inside to auto-clear (0 = instant)
  }
  severity      // Advisory | Warning | Critical; defaulted per template, overridable
  enabled       // default false
}
```

Evaluation happens once per Station step, against the values the tick already publishes.
Hysteresis is temporal only — there is no value deadband; asymmetric behaviour is expressed
by the two times, and a value deadband can be emulated later with paired thresholds if ever
needed. While the metric persists beyond the threshold for `triggerOnSeconds` the Monitor
**creates an Alert**; while it persists back inside for `triggerOffSeconds` the Alert
**auto-clears**. Oscillation around the threshold fires and clears on those time constants.

An **Alert** is implemented in code as a bound callback per alert type:
`{ onFire(alertInstance), onClear(alertInstance) }`, registered in an alert-callback
registry keyed by alert type. In v1 the callback records the Alert into the runtime alert
ledger (which feeds the dashboard) and announces fire/clear on the event stream, exactly
like fault events. Nothing in the simulation consumes an Alert for actuation.

A **disabled Monitor does not evaluate**: it produces no Alert and no state. Enabling a
Monitor mid-run starts evaluation from that tick forward — no backfill, consistent with
the command-time-effect rule used across Station systems.

## 2. Subjects in scope

Every station component class that publishes runtime telemetry, plus Station connections:

- **Power sources**: generators, solar arrays, batteries.
- **Thermal plant**: coolant loops, chillers, heat engines, radiators.
- **ECLSS machines**: water treatment, electrolyzers, CO2 capture, Sabatier reactors,
  methane processors, fuel cells, microbial processors.
- **Resource stores**.
- **Atmosphere compartments**.
- **Biological populations**: crop populations, algae cultures.
- **Station connections** (material/energy/service flows with throughput and efficiency).

Explicitly **not** monitor subjects in v1: Atmosphere pathways, Thermal links, schedules
(deliveries, outages, maintenance, waste ejections), and World traversal objects.

Note: the monitor subject namespace mirrors the fault catalogue's component namespace, which
is broader than the registered Installation identity namespace (Repair's unit). A monitor
may watch any telemetry-publishing component; Repair semantics are unchanged.

## 3. Metrics

Metrics are named, typed, unit-carrying quantities drawn from what the runtime snapshot
already publishes, plus tick-derived quantities (rates, fractions, ratios). No new stored
state is introduced beyond the monitor/alert machinery. Derived metrics are computed at
evaluation time from snapshot fields. Rate-of-change and deviation-from-commanded are
ordinary derived Metrics, not special rule types — one rule shape everywhere.

### 3.1 Metric catalogue (per subject type)

| Subject | Metric | Unit | Source |
|---|---|---|---|
| Generator | `outputKW` | kW | snapshot |
| Generator | `outputDeviationFraction` | fraction of rated | derived: \|output − commanded\| / ratedOutputKW |
| Generator | `sinkTemperatureC` | °C | snapshot |
| Generator | `startupUnmetKW` | kW | snapshot |
| Solar array | `availableFraction` | fraction | derived: availableKW / capacityKW |
| Solar array | `availabilityFactor` | fraction | snapshot |
| Battery | `stateOfCharge` | fraction | snapshot |
| Battery | `dischargeShortfallFraction` | fraction | derived: lastDischargeShortfallKW / maxDischargeKW |
| Battery | `wearFraction` | fraction | snapshot |
| Coolant loop | `fillFraction` | fraction | snapshot |
| Coolant loop | `circulationFraction` | fraction | snapshot |
| Coolant loop | `loopTemperatureC` | °C | snapshot |
| Coolant loop | `condition` | fraction | snapshot |
| Chiller | `coolingUnmetFraction` | fraction of demand | derived: coolingUnmetKW / max(coolingDemandKW, ε) |
| Chiller | `sinkOverMaxCondensing` | boolean→1/0 | snapshot flag |
| Chiller | `refrigerantFraction` | fraction | derived: refrigerantKg / refrigerantChargeKg |
| Heat engine | `tripped` | boolean→1/0 | status == Tripped |
| Heat engine | `hotHeadroomC` | °C | derived: hot reservoir temp − minHotTemperatureC |
| ECLSS machines | `unmetFraction` | fraction of demand | derived per machine: unmet / max(demanded, ε) |
| ECLSS machines | `condition` | fraction | snapshot where present |
| Resource store | `usableFraction` | fraction of capacity | derived: currentQualified / capacity |
| Resource store | `totalDissolved` | mg/L | snapshot water-quality concentrations (water stores) |
| Resource store | `maxContaminantConcentration` | mg/L | derived: max over species in `concentrations` |
| Resource store | `unexpectedSpecies` | 1 when any species above trace | derived from `concentrations` |
| Atmosphere | `oxygenPartialPressureKPa` | kPa | snapshot |
| Atmosphere | `carbonDioxidePartialPressureKPa` | kPa | snapshot |
| Atmosphere | `pressureKPa` | kPa | snapshot |
| Atmosphere | `temperatureC` | °C | snapshot |
| Atmosphere | `relativeHumidity` | fraction | snapshot |
| Atmosphere | `toxicConcentration` | g/m³ | derived: toxicContaminantKg / volumeM3 |
| Atmosphere | `oxygenUnmetKg` | kg/tick | snapshot lastOxygenUnmetKg |
| Atmosphere | `leakRateKgPerS` | kg/s | snapshot |
| Crop / algae | `biomassFraction` | fraction of growing capacity | derived: livingBiomass / capacity |
| Crop / algae | `unmetResourceFraction` | fraction | derived per limiting resource |
| Connection | `lossFraction` | fraction | derived: lastLossRate / max(lastInputRate, ε) |
| Connection | `lossRate` | unit/s | snapshot lastLossRate |
| Connection | `reversed` | boolean→1/0 | snapshot flag |
| Connection | `utilizationFraction` | fraction | derived: lastInputRate / maxThroughput |

## 4. Default Monitor sets

Every monitor subject ships with its default set pre-listed **disabled**. Values carry a
provenance tag: **[ISS]** human/crew-realistic derived from ISS limits, **[WHO]** drinking-
water guideline, **[rated]** fraction of the subject's own rated/authored value,
**[prov]** provisional engineering estimate. All defaults are adjustable per Monitor;
human/environmental baselines start human-realistic and must be consciously moved.

### 4.1 Atmosphere compartment (habitability — always human-realistic baseline)

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| pO₂ low | Below 17.5 kPa, on 30 s, off 60 s | Critical | [ISS] crew hypoxia bound (~16% equivalent) |
| pO₂ high | Above 24 kPa, on 60 s, off 60 s | Warning | [ISS] elevated-O₂ fire risk (~23.5%+) |
| pCO₂ high | Above 0.7 kPa, on 300 s, off 120 s | Warning | [ISS] ~24 h limit (5.3 mmHg) |
| pCO₂ critical | Above 1.0 kPa, on 60 s, off 120 s | Critical | [ISS] ~4 h limit (7.6 mmHg) |
| Pressure low | Below 90 kPa, on 10 s, off 60 s | Critical | [ISS] ~10% below sea-level equivalent |
| Pressure high | Above 110 kPa, on 60 s, off 60 s | Warning | [prov] |
| Temperature high | Above 27 °C, on 600 s, off 300 s | Advisory | [ISS] comfort band 18–27 °C |
| Temperature low | Below 18 °C, on 600 s, off 300 s | Advisory | [ISS] comfort band |
| Humidity high | Above 0.75, on 600 s, off 300 s | Advisory | [prov] condensation risk |
| Toxic contaminant | Above 0.1 g/m³, on 60 s, off 120 s | Critical | [prov] fire-products/IDH analogue |
| Oxygen supply unmet | Above 0 kg/tick, on 10 s, off 30 s | Critical | [rated] |
| Compartment leak | Above 0.01 kg/s, on 60 s, off 120 s | Warning | [prov] |

### 4.2 Resource stores

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| Usable inventory low | Below 0.25 of capacity, on 120 s, off 60 s | Warning | [rated] |
| Usable inventory critical | Below 0.10 of capacity, on 60 s, off 60 s | Critical | [rated] |
| Water total dissolved (water stores) | Above 500 mg/L, on 60 s, off 60 s | Critical | [WHO] |
| Unexpected species (potable water) | Any species above 1 mg/L, on 60 s, off 60 s | Warning | [prov] trace detector |

### 4.3 Batteries

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| State of charge low | Below 0.20, on 60 s, off 30 s | Warning | [ISS] depth-of-discharge practice |
| Reserve floor reached | Below 0.10, on 10 s, off 30 s | Critical | [rated] |
| Charge headroom gone | Above 0.95, on 300 s, off 60 s | Advisory | [rated] |
| Discharge shortfall | Above 0.05 of max discharge, on 30 s, off 30 s | Warning | [rated] |
| Wear fade | Above 0.20, on 0 s, off 0 s | Advisory | [prov] |

### 4.4 Generators / solar arrays

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| Output off-command (gen) | Above 0.05 of rated, on 60 s, off 60 s | Warning | [rated] |
| Sink near limit (gen) | Above maxSinkTemperatureC − 5, on 60 s, off 120 s | Warning | [rated] |
| Startup shortfall (gen) | Above 0 kW, on 30 s, off 10 s | Advisory | [rated] |
| Solar output low | availableFraction below 0.5, on 300 s, off 120 s | Advisory | [prov] |

### 4.5 Coolant loops / chillers / heat engines / radiators

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| Coolant fill low | fillFraction below 0.8, on 120 s, off 60 s | Warning | [rated] leak symptom |
| Coolant fill critical | fillFraction below 0.5, on 60 s, off 60 s | Critical | [rated] |
| Circulation low | circulationFraction below 0.9, on 60 s, off 30 s | Warning | [rated] |
| Freeze risk | loopTemperatureC below freezeProtectionSetpointC + 2, on 60 s, off 120 s | Critical | [rated] |
| Chiller cooling unmet | coolingUnmetFraction above 0.1, on 120 s, off 60 s | Warning | [rated] |
| Chiller sink over max | sinkOverMaxCondensing = 1, on 10 s, off 30 s | Critical | [rated] |
| Refrigerant charge low | refrigerantFraction below 0.8, on 300 s, off 120 s | Warning | [rated] leak symptom |
| Engine tripped | tripped = 1, on 0 s, off 0 s | Critical | [rated] |
| Engine gradient thin | hotHeadroomC below 5, on 120 s, off 120 s | Advisory | [prov] |

### 4.6 ECLSS machines (water treatment, electrolyzer, CO2 capture, Sabatier, methane processor, fuel cell, microbial processor)

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| Service unmet | unmetFraction above 0.1, on 120 s, off 60 s | Warning | [rated] |
| Condition low | condition below 0.5, on 300 s, off 120 s | Advisory | [rated] |

### 4.7 Crop populations / algae cultures

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| Biomust low | biomassFraction below 0.2, on 3600 s, off 600 s | Warning | [rated] |
| Resource starvation | unmetResourceFraction above 0.1, on 600 s, off 300 s | Warning | [rated] |

### 4.8 Station connections

| Monitor | Rule | Severity | Provenance |
|---|---|---|---|
| Loss fraction high | lossFraction above 0.05, on 120 s, off 60 s | Warning | [rated] typical cable/pipe loss band 2–5% |
| Absolute loss high | lossRate above author-set threshold, on 120 s, off 60 s | Warning | author-set (no universal default) |
| Reversed flow | reversed = 1, on 1 s, off 1 s | Critical | [prov] mis-connection / backfeed |
| Near capacity | utilizationFraction above 0.9, on 300 s, off 120 s | Advisory | [rated] |

Contamination in transit: water-carrying connections expose the carried stream's
contaminant fraction where the transfer snapshot carries quality data; the same
per-species/total templates from §4.2 apply to that metric.

## 5. Enabling and bulk operations

The per-Monitor `enabled` flag is the single source of truth. Bulk enable/disable is offered
per-component, per-subject-type, and station-wide; each bulk operation is one undoable,
validated command that sets many flags at once. Disabled Monitors are skipped entirely at
tick (no evaluation cost, no state). Enabling starts evaluation from the effective tick;
disabling leaves any active Alert closed out with reason `MonitorDisabled`.

## 6. Alert dashboard

A new read-only window, the **Alert dashboard**:

- **Active** Alerts at top: severity, subject, metric, current value, threshold, live
  duration since fire.
- **Closed-out** Alerts below: fired-at / cleared-at sim times, clear reason
  (auto-clear vs disabled).
- Filters: severity, subject type, and a UI-set **window** narrowing the visible range
  (e.g. "fired within the last 10 sim-minutes"). The filter is presentation-only; it never
  affects what fires, clears, or is recorded.
- The dashboard is run-scoped runtime state: it is never persisted and clears on simulation
  reset.

The existing #849 alarm panel (fault-derived impact) is unchanged; the dashboard is a
separate surface. The two are complementary: the fault panel explains broken machines; the
Alert dashboard reports monitored conditions.

## 7. Authoring

- Monitors are authored in the **Monitors section of each component/connection's editor
  panel** on the station diagram: the default set pre-listed disabled, with per-Monitor
  enable toggle, threshold, direction, trigger-on/off times, and severity; bulk-enable
  buttons per component/type/station-wide.
- Monitors are part of the Station configuration document: saved, loaded, and undoable
  like other authored fields, and every edit flows through the same validated World-facade
  command path headless clients use.
- Validation: subject exists and is a monitor-subject type; metric is known for that type;
  threshold finite and in the metric's unit; times >= 0; Monitor id unique. Invalid edits
  are refused with a diagnostic and leave no partial state.
- Renaming a subject cascades monitor references exactly as it cascades links; deleting a
  subject removes its Monitors in the same undoable action.

## 8. Testing seams

Primary seam: **StationSystemsRuntime + StationSystemsSnapshot** (the same seam the fault
framework uses). Headless checks author `StationSystemsSettings` with monitor entries, step
the runtime, perturb metrics via existing fault-injection/scenario machinery, and assert on
the snapshot's alert ledger: fire timing at trigger-on boundaries, auto-clear at
trigger-off boundaries, disabled-Monitor silence, mid-run enable from-tick-forward
semantics, bulk-enable effects, and persistence round-trips of authored monitor fields
through save/load/undo/redo.

Editor seam (precedent: #849's fault-surface checks): ImGui Test Engine drives the real
station window — the Monitors inspector section (toggles, fields, bulk buttons) asserting
commands go through the validated facade and round-trip undo/redo/save, and the Alert
dashboard window asserting active/closed-out lists, timestamps, and the window filter as
the operator sees them.

Good tests assert externally visible behaviour only: what fired, when, at what severity,
what the dashboard shows — never callback internals or tick-order artifacts.

## 9. Out of scope

- Any actuation from Alerts (load shedding, crew dispatch, auto-repair).
- Monitors on Atmosphere pathways, Thermal links, schedules, and World traversal objects.
- Migration of the pre-existing internal alarm scalars (reserve-exhausted #394,
  sink-temperature, shortfall alarms) — tracked as a follow-up issue to unify them into
  Monitors later.
- Persisted alarm history across save/load.
- Value-band (deadband) hysteresis, multi-condition compound rules, and rule expressions.
- Random alert behaviour, notification sounds/chimes.

## 10. Further notes

- Realism honesty: [prov] defaults are engineering estimates, not certified limits, matching
  the monitoring spec's rule that validated requirements, provisional assumptions, and
  unavailable evidence stay visibly distinct. Provenance is recorded per default and shown
  in the inspector.
- The alert-callback registry is the extension point: future integrations (audio, HUD pins,
  agent reactions) attach as new callbacks without touching the authored schema.
- Determinism: evaluation is a pure function of snapshot values and authored rules at the
  Station step; the same authored config and the same perturbations replay identically.
