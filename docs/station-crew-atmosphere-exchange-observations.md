# Crew, atmosphere and Airlock exchange observations (#708)

The applied-run catalogue registers independent Crew food, water and oxygen
inputs; urine, organic waste, carbon dioxide, moisture and metabolic heat
outputs; and retained body mass as **inventory**, never useful output. Store
flows retain resolved material identity, hazard and canonical mass conversion
(including g to kg). Unresolved identity remains explicitly unclassified.
Requested overlays exist only for the committed requested metabolic pathways.

Atmosphere observations include ambient occupancy oxygen consumption, carbon
dioxide and moisture additions, condensation, water collection, electrolysis
oxygen, CO2 capture, leakage, occupancy/latent/crew/crop/algae heat and signed
reservoir heat exchange. No diagram Port is necessary. These are compartment
endpoint accounts, not additional Crew production; independent properties must
not be summed across accounting boundaries. Inventory kg and exchange kg/s
remain separate. Known inactive pathways are zero; missing owners produce gaps.

Most material rates describe the **last committed fixed tick**: divide its
quantity by the fixed timestep, then convert to canonical kg/s. Power is already
kW. Heat exchange preserves its signed direction. The generic graph shows the
window/accounting note supplied by the adapter; no equipment-specific rendering
or second simulation is involved.

## Discrete Airlock events

Each Airlock schedule has separate removed-gas input, recovered-gas output and
vented-gas loss properties. The committed snapshot still supplies per-occurrence
`lastRemovedKg`, `lastRecoveredKg`, `lastLostKg`, `lastOccurrenceSeconds` and
`occurrencesCompleted`; these are event quantities/times, not sustained rates.

Graphs use **interval-average event rates**: the difference of cumulative kg at
two consecutive observations divided by the actual elapsed simulation seconds.
The first window is `(0, first observation time]`, with zero initial cumulative
ledger (including any event at time zero processed by the first tick). Later
windows are `(previous observation time, current observation time]`. An event
between observations is included once; multiple occurrences in a window are all
included. A window without an occurrence is zero even though the snapshot's
last-occurrence quantity remains nonzero. No requested production is fabricated.
Reset and successful reapplication reset both history and these baselines.
Missing authority makes the current and following difference unavailable rather
than bridging an unknown interval. Shared history coarsening retains its existing
interval-summary semantics; it does not preserve exact event timestamps.

Leakage and cycle ledgers expose total composition-proportional gas mass, not
species-specific removed masses. Their material identity is `atmosphere-mixture`,
retaining hazard class, distinct from pure oxygen, nitrogen, carbon dioxide and
water. No species allocation is reconstructed from the closing composition.
Recovered gas remains retained and is not qualified supply. Removed = recovered
+ vented; those are alternative accounting boundaries, not additive production.

## Validation

- `station-crewAtmosphereExchangeObservations`: public World accounting,
  canonical mass rates, balanced metabolic returns with explicit retained mass,
  ambient leakage, missing observations, off-cadence/multiple Airlock events,
  window-integrated quantities, inactive zero, and read-only queries.
- `station-editor-material-observations`: real headless property selection and
  instance controls for Crew, ambient atmosphere and Airlock properties, hidden
  recording, unchanged World/configuration/history, and continued authoring.
