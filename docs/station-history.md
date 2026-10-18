# Station observation history (#696)

Each applied Station systems runtime (owned by World) compiles recording slots
once: station service and generation, then equipment loads and transfers in
applied configuration order. Observation reads indexed committed snapshot values
at the existing observation boundary, independent of editor visibility. Neither
queries nor legacy trend reads sample. Reset/reconstruction starts fresh history;
pause and refused configuration changes preserve it.

`World::getStationSystemsRecordingStatus()` reports limits and the retained
interval. Default recording bounds are 256 timeline rows, 1024 slots, 1 MiB of
metadata and 32 MiB of recording storage. Metadata accounting includes slot vector
capacity and owned string capacities. Storage accounting includes recorder state,
metadata, the shared timestamp array, all metric cells, pending cells and runtime
observation staging. Smaller memory budgets reduce the common even row capacity.
If two rows and metadata cannot fit, or cardinality/metadata limits are exceeded,
the entire recording is refused with `limited=true`; no metric subset is silently
selected. Read-only exported snapshots are bounded by the same cardinality and row
limits but are caller-owned, not additional sampling systems. Live history is not
authored or persisted.

On a full timeline all metrics merge adjacent equal-duration intervals together.
Future rows accumulate *every* observation over the new doubled interval, rather
than skipping observations or appending finer samples to a coarse tail. The level
is capped at 48; after that the oldest row slides out. Authored retention evicts
oldest rows together. Timestamps mark interval ends; resolution describes the
actual simulation tick cadence. Coarsened points are interval summaries, not
instantaneous values. A partial trailing interval is not published until complete.

Each cell tracks expected and valid observations. Any missing or non-finite value
makes its interval unavailable; coarsening preserves its coverage fraction and
cannot turn an invalid span into a plausible complete average. Known inactive
zero remains valid. Chart queries export NaN gaps plus coverage per point. Legacy
trend consumers receive the same timeline and NaN for invalid intervals; they do
not own another recorder. The historical standalone `appendBoundedTrendSample`
utility remains for compatibility tests but is not used by the runtime.

Power demand is requested service; generation is available generation, never
battery-assisted supplied service. Legacy transfer quantities retain their native
per-tick units and do not pretend to be per-second exchanges. Non-canonical legacy
units are explicitly unspecified in typed metadata, while the legacy trend keeps
its original unit label.

The Power header reports retained interval, resolution, coarsening/truncation,
gaps, recording footprint, row limit and an explicit recording-limit condition.
The existing `station-trends` public World workflow checks hidden-window sampling,
aligned bounded history, retention, pause/reset and deterministic replay, plus
small recorder budgets and invalid intervals. `station-editor-trends` covers the
production headless editor presentation.
