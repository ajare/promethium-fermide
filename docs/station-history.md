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
## Revisioned queries (#697)

`World::getStationSystemsObservationRevision()` exposes an opaque applied-run
identity and a history revision. Reconstruction (including Reset at tick zero),
World load, and successful changed configuration application create a new run.
An unchanged or refused application keeps the existing run. A history revision
changes only when published rows change, including coarsening; partial pending
observations do not invalidate an unchanged query. These identities are runtime
observations, never saved or authored.

`getStationSystemsObservationCatalogue()` borrows run-compiled applicable chart
membership. Power remains the fallback, including empty/disabled runs.
`queryStationSystemsChart(id)` borrows a single cached selected query, keyed by
property identity and history revision. Only metrics referenced by that property
are exported; selecting another property replaces the cache. Unknown identities
return an empty result. Borrowed query results must not be retained across another
query or run replacement without copying. Query storage is additional to recorder
storage, bounded by one catalogue property's series and the recorder's row limit;
it is not a second recording system. Legacy full-history exports remain available
for existing consumers, but Flow never calls them.

Flow copies and prepares only the selected result when the run/history/selection
key changes, and caches axis and legend labels with those buffers. Pause,
unapplied draft edits, and unchanged UI frames do not rebuild queries or plot
buffers. Selection and contribution colour use stable identity; an inapplicable
selection visibly falls back to Power. The graph names the applied simulation,
not an unapplied Configuration draft. `getStationSystemsQueryWork()` and the
editor's preparation counter expose deterministic work evidence without timing
assertions or changing World/document state.

## Battery observations (#698)

Each applied battery registers stored energy and installed nameplate capacity in
kWh, both read from the committed battery snapshot at the same observation
boundary as power. Stored energy includes retained reserve energy; it is not
available discharge energy. Capacity is nameplate capacity (the denominator used
by the authoritative battery state of charge), not usable/cyclable capacity.
These measurements are typed-only slots in the same recorder, not additional
legacy trend series; legacy trend membership is unchanged.

Applied descriptors generate `battery-energy` and `battery-charge` entries under
Batteries. No batteries means no applicable battery properties. Energy uses the
generic sum operation for the Station total and stable identity-sorted individual
contributions for the stack. Values remain individual energies; separately
prepared lower/upper buffers describe their cumulative filled regions. Charge
uses the generic ratio of summed stored energy to summed capacity, displayed in
percent. It never averages individual battery percentages. Zero total capacity
produces unavailable points and an explicit unavailable message, not zero percent
or infinity. A zero-energy store with positive capacity remains a valid zero.

The query definitions retain the expected descriptor population. Missing,
invalid, unit-incompatible or timestamp-misaligned contributors produce gaps and
incomplete status; they cannot silently shrink a sum or ratio. Any missing stack
contribution gaps the complete stack at that time, so an available subset cannot
look like a complete total. The renderer dispatches only on generic line/stack
presentation, never on battery/equipment identity. Sum, ratio and stack operands
all use the shared coordinated coarsening grid; capacity remains recorded on that
grid rather than taken from a later configuration or the current UI draft.

`station-batteryObservations` covers public World charging/discharging, unequal
capacities, pre-selection history, read-only queries, descriptor membership,
aggregate/stack equivalence, zero capacity, aligned coarsening, and invalid spans.
`station-editor-battery-observations` drives the real grouped selector and generic
plot, including switching to already-recorded history, units/legends, cached
frames, Reset, disappearing-property fallback, and unavailable percentage data.

The existing `station-trends` public World workflow checks hidden-window sampling,
aligned bounded history, retention, pause/reset and deterministic replay, plus
small recorder budgets and invalid intervals. `station-editor-trends` covers the
production headless editor presentation.

## Stored material inventories (#700)

Each Resource store registers one inventory ownership contribution, containing
qualified **plus** unqualified contents. The adapter reads the committed store
snapshot at the shared observation cadence, even when its property is unselected
or the editor is closed. Internal transfers are not extra inventory contributions.
Protected reserves remain included; this is inventory, not usable supply or
discretionary availability. Compound composition is not added a second time.

Applied-run compilation resolves identity from explicit material IDs and
unambiguous validated relationships, never from names, units, hazard classes or
constituent fractions. Material-preserving transfers connect identity evidence;
water/gas processes and population input roles provide intrinsic material
evidence. All evidence is collected before resolving a connected component.
Missing or contradictory evidence leaves inventories unclassified, including
explicit identities conflicting with another explicit ID or intrinsic role.
Resolution never rewrites authored settings or changes physics validation.
Unspecified compound food, waste, consumables and other non-intrinsic roles are
not guessed from their purpose or constituent composition.

The catalogue groups classified stores by material identity, physical quantity
and canonical unit. Mass uses kg, volume m3, energy kWh and amount of substance
mol. Applied units g, mL, L, kJ, MJ and Wh are converted before recording; no
mass/volume, chemical or density conversion is inferred. Incompatible quantities
of the same material receive separate properties. Hazard classes may contribute
to this inventory sum without implying interchangeability or usable normal supply.

Material properties are labelled **Stored material inventory**, with a classified
stores subtotal and identity-sorted instance stack using the generic query and
renderer. Accounting coverage explicitly excludes atmospheres, living biomass
and other non-store inventories. Each unclassified store has its own selectable
quantity view; its amount is not assigned to any material subtotal. If any store
is unclassified, material results retain a partial-accounting status even when
the classified subtotal is numerically available. Missing expected measurements
instead produce gaps in the sum and entire stack, never a smaller apparent total.

Identity assignment becomes observable only after application, which starts fresh
history. A disappearing unclassified view visibly falls back to Power. Reset
retains an applicable selection but clears samples. Observation never edits the
Configuration draft, authored identity, document history or simulation state.

Coverage: `station-materialObservations`,
`station-materialRelationshipObservations`, `station-materialCanonicalUnits`, and
`station-editor-material-observations`, alongside existing identity persistence
and editor-history checks from #699.

## Transfer and conversion exchange rates (#705)

Resource transfers and conversions register each applied input/output role and
its connection transport loss separately. Conversions support up to five inputs
and five outputs; all declared roles receive independent selectable properties.
Views group only equal equipment types, directions, exchange roles, resolved
material identities, canonical quantities/units and hazard classes. Unclassified
materials and unsupported units are not silently assigned to a material rate.

Actual conversion exchanges and transfer deliveries are committed last-tick
quantities divided by the fixed simulation timestep, then canonically scaled.
Transfer inputs and connection losses use the authoritative connection ledger's
already-per-second rates. Mass, volume, energy and molar exchanges use kg/s,
m3/s, kW and mol/s respectively. No accumulated quantity is relabelled as a rate;
legacy native per-tick transfer trends remain unchanged.

Each selector property defaults to aggregate actual rate, with a generic Instance
breakdown. Requested conversion rates and transfer destination requests are
separate, unstacked overlays. Transfer source requests and loss requests are not
invented. Inputs describe material delivered to equipment; outputs describe
material delivered to receiving stores. Source inventory draw can additionally
include transport loss; it is not the useful input. Connection transport losses
are neither useful output nor inventory change. These views do not infer
per-material conversion reaction losses from different input/output materials.

All exchanges record in the shared bounded history before selection. Missing
connections or expected measurements produce gaps, while known inactive exchanges
produce valid zero. Priority-ordered snapshots resolve by equipment identity,
not authored vector position. No query changes allocation or compatibility rules.
`station-exchangeObservations` covers five-way ports, constrained operation,
canonical rates, optional requests, inactive zero, missing values and preselection
history. `station-editor-material-observations` exercises the real selector and
aggregate/instance switching; existing connection and growable-port checks cover
enforcement and authoring.
