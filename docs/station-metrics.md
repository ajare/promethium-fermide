# Station Flow observations

Complete family/ownership/external-role audit and mixed-reference acceptance
(#715): [Coverage and bounded-operation verification](station-observation-coverage.md).

Scheduled external and maintenance exchange catalogue (#714): see
[Scheduled exchange observations](station-scheduled-exchange-observations.md)
for event-window rates, resupply boundary accounts, ejection losses and committed
maintenance spend.

Microbial processor exchange catalogue (#711): see
[Microbial processor exchange observations](station-microbial-exchange-observations.md)
for compound feed, consumables, recovered products, heat and loss boundaries.

Carbon recovery and fuel-cell exchange catalogue (#710): see
[Carbon recovery and fuel-cell exchange observations](station-carbon-exchange-observations.md)
for distinct reactants/products, electrical/thermal accounts and connection losses.

Water and gas processing exchange catalogue (#709): see
[Water and gas processing exchange observations](station-water-gas-exchange-observations.md)
for condensate, treatment, electrolysis, CO2 capture and separate loss boundaries.

Crew, atmosphere and Airlock exchange catalogue (#708): see
[Crew and atmosphere exchange observations](station-crew-atmosphere-exchange-observations.md)
for ambient flows, metabolic accounting and explicit Airlock event-rate windows.

Thermal equipment exchange catalogue (#707): see
[Thermal equipment exchange observations](station-thermal-exchange-observations.md)
for class-segregated heat, cooling, electrical demand, storage and loss boundaries.

Electrical equipment exchange catalogue (#706): see
[Electrical equipment exchange observations](station-electrical-exchange-observations.md)
for generation/service boundaries, canonical fuel/material rates and focused checks.

Slice 1a (#695) presents Power through `core/StationMetrics.h`:

- `World::getStationSystemsMetricHistory()` exposes typed, read-only observations.
- Descriptors identify physical quantity/canonical unit, semantic role, optional
  requested semantics, equipment/instance/exchange, material or constituent,
  validity and accounting coverage. Missing is not zero.
- `stationCharts()` declares selector groups, stable view IDs and series queries.
  `queryStationChart()` validates quantity, unit and requested availability and
  prepares simulation timestamps and values. Invalid values become NaN gaps;
  missing expected series and partial coverage mark the result incomplete.
- The Station Flow header selects and draws those definitions with one ImPlot
  renderer. The data is the **applied simulation**, even while its Configuration
  draft differs. Rendering and selection do not advance or edit the World.

Power selects requested demand from the existing station-wide service trend and
actual generation from its independent generation trend. Supplied service includes
battery discharge and must never be plotted as generation. The generation metric
has no requested value; the legacy paired trend representation is not an assertion
that requested generation exists.

## Shared applied-run history

`StationHistory` is the single World-owned recorder; the temporary trend adapter
from slice 1a has been retired. Legacy trend consumers export the same history.
Histories use the observation cadence, retention, coordinated coarsening and
Reset/configuration lifecycle; resolution and truncation are shown by the viewer.
Metadata and applicable membership are compiled before the first sample.

Prepared plotting buffers are reused on unchanged frames; live snapshot polling
is throttled to 4 Hz, with run/configuration and pause transitions bypassing the
throttle. Selection prepares only the chosen chart. Snapshot collection indices
are built once per run and shared by all role readers, rather than searching a
whole collection for each measurement. Public query-work counters expose this
work. See [station-history.md](station-history.md) for the total budget, limits,
cached queries and optional authoritative requested overlays.

Acceptance uses `station-editor-flow-telemetry` (real selector, labels/axes,
initial, paused, disabled and Reset states, read-only viewing, battery-assisted
service, continued placement and undo) and `station-trends` (surplus generation,
legacy consumers, simulation-time cadence, bounded retention and replay).

## Stored inventory usability and breakdowns (#701)

The shared World-owned recorder now records two inventory measurements per
Resource store: total (qualified plus unqualified) and usable (qualified only).
Both use the same timestamps, canonical conversion and coordinated coarsening.
Usable inventory **includes Protected resource reserves**; it is not a measure of
discretionary drawability. No allocation rules change.

Material properties retain the stored-only accounting boundary and explicit
unclassified exclusions. Total and usable properties combine all stores with the
same material and canonical unit. Usable
properties are qualified stores subtotals, not system-wide material totals.

The generic `Breakdown` control selects Instance, Aggregate, or (where multiple
owner types apply) Equipment type. The catalogue compiles those presentations
into the existing Value/Sum queries and stack preparation, also reused for battery
energy. The property ID remains the base Instance view; `/aggregate` and
`/by-equipment` identify alternative read-only World queries. Usable properties
are named `stored-usable/<material>/<canonical-unit>`. Exchange chart IDs likewise
omit the retired class segment; thermal-service class remains a distinct grouping key.
Legacy class-keyed chart IDs no longer resolve (#755). Stable metric/group IDs
supply colour identity. All variants use the same expected ownership membership;
a missing contribution produces an incomplete sum and stack gaps, not zero.

`station-materialObservations`, `station-materialCanonicalUnits`,
`station-materialIdentity` and `station-editor-material-observations` cover
qualified/unqualified/reserved mixtures, aggregate/stack equivalence,
missing contributors, aligned coarsening, persisted identities and real controls.
Viewing and changing breakdowns do not mutate the Configuration draft or World.
