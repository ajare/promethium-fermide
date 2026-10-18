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
