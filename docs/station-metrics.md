# Station Flow observations

Slice 1a (#695) presents Power through `core/StationMetrics.h`:

- `World::getStationSystemsMetricHistory()` exposes typed, read-only observations.
- Descriptors identify physical quantity/canonical unit, semantic role, optional
  requested semantics, equipment/instance/exchange, material or constituent,
  hazard class, validity and accounting coverage. Missing is not zero.
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

## Temporary history adapter

`adaptStationTrends()` translates the existing bounded World-owned trends. It
neither samples nor retains an independent recording. Existing Station Systems
trend consumers remain unchanged. Histories still use the existing observation
cadence, retention, coarsening and Reset/configuration lifecycle; resolution and
truncation are shown by the viewer. Metadata is available before the first sample.

Prepared plotting buffers are reused on unchanged frames; live snapshot polling
is throttled to 4 Hz, with run/configuration and pause transitions bypassing the
throttle. Selection prepares only the chosen chart. Slice 1b replaces the adapter
with a shared bounded recorder, not another editor sampler. Inventory/equipment
views and aggregation are later slices, not implied by the metadata declarations.

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
unclassified exclusions. Total properties may combine hazard classes. Usable
properties are separate for each material/unit/hazard class: hazardous contents
are never represented as qualified normal supply. These are qualified stores
subtotals, not system-wide material totals.

The generic `Breakdown` control selects Instance, Aggregate, or (where multiple
classes apply) Hazard class. The catalogue compiles those presentations into the
existing Value/Sum queries and stack preparation, also reused for battery energy.
The property ID remains the base Instance view; `/aggregate` and `/by-hazard`
identify its alternative read-only World queries. Usable properties are named
`stored-usable/<material>/<canonical-unit>/<hazard>`. Stable metric/group IDs
supply colour identity. All variants use the same expected ownership membership;
a missing contribution produces an incomplete sum and stack gaps, not zero.

`station-materialObservations`, `station-materialCanonicalUnits`,
`station-materialIdentity` and `station-editor-material-observations` cover
qualified/unqualified/reserved/hazard mixtures, aggregate/stack equivalence,
missing contributors, aligned coarsening, persisted identities and real controls.
Viewing and changing breakdowns do not mutate the Configuration draft or World.
