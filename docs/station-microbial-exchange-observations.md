# Microbial processor exchange observations (#711)

`StationMicrobialMetrics.cpp` registers committed process accounts once. The
World-owned recorder, generic selector, queries, instance breakdowns and
renderer consume the same typed metadata; no processing or allocation rules change.

Separate input views cover Organic feed, Oxygen, Process water, optional Culture
medium, optional Replacement stock, and Electrical service. Separate output views
cover Recovered nutrients, Recovered water, CO2, Unrecoverable residue, Reaction
heat released, and Produced heat (electrical dose plus released reaction heat).
Only feed demand, culture-medium demand and electrical demand have authoritative
requested measurements. Replacement stock has no invented request.

Material process accounts are kg/tick converted using the simulation timestep to
kg/s, regardless of observation cadence. Culture medium and replacement stock
are in their referenced store units and receive canonical unit conversion.
Exchange views retain the generic hazard-segregated grouping; normal and
hazardous chains never silently merge. Material identity comes from the existing
explicit/relationship-resolved store
metadata, never a label, unit or compound composition. Unknown compound feed,
nutrient product or residue identity produces an incomplete view with gaps rather
than an apparently complete total. Hazard and thermal classes remain explicit.

Recovered nutrients are a compound material, not atmospheric nitrogen. The
snapshot's cumulative N/P/K/mineral recovery totals are **not** instantaneous
rates. Culture condition and stock inventory are not exchanges. The processor has
no separate ambient-atmosphere exchange: its oxygen, water and CO2 exchanges use
Resource stores. No ambient flow or instantaneous constituent measurement is
fabricated. Existing constituent inventories remain separate from these rates.

Connection accounts separately expose accepted input, delivered output and
transport loss for material, electricity and heat. Heat also exposes retained
source inventory in kWh, not as a produced or exported rate. Unrecoverable residue
is a process product destined for disposal, not an external connection loss.
Process and endpoint accounts describe different boundaries and must not be added
as independent production. Known inactivity is zero; a missing snapshot or
connection remains unavailable.

Release CTest coverage:

- `station-microbialExchangeObservations`: public World recording/query results,
  resource-limited processing, consumable exhaustion, cold degradation and benign
  recovery, mass/constituent balance, conversion of gram consumables, connection
  loss and retained heat, inactivity, multiple instances, hazard separation, missing
  contributors and unclassified compound feed.
- `station-editor-microbial-observations`: real headless ImGui selector and
  breakdown controls, hidden-window history, process input/output rates,
  exhaustion/degradation/recovery/inactivity, incomplete-data gaps and visible
  coverage diagnostics, and read-only World/document state.
