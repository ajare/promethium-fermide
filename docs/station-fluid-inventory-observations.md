# Non-store fluid inventory observations (#702)

The applied-run recorder reads committed `StationSystemsSnapshot` quantities at
its existing observation cadence. Selection and editor visibility do not affect
recording. Reset/application reconstructs the catalogue and history.

## Ownership audit

| Owner | Authoritative retained quantity | Material identity | Canonical unit |
| --- | --- | --- | --- |
| Resource store | currentQualified + currentUnqualified | authored or validated relationship evidence | existing canonical store unit |
| Atmosphere compartment | oxygenKg | oxygen | kg |
| Atmosphere compartment | nitrogenKg | nitrogen | kg |
| Atmosphere compartment | carbonDioxideKg | carbon-dioxide | kg |
| Atmosphere compartment | waterVapourKg | water | kg |
| Coolant loop | coolantKg | coolant | kg |

Each compartment/material and loop owns one observation slot. The coolant is a
closed-loop thermal service fluid, not potable water or nutrient solution.
Zero-charge loops report real zero stock. Hazard metadata belongs to each owner.
Owner-prefixed metric identities distinguish equal compartment, loop and store
names. Compartment mass is never added to qualified store supply.

Collectors move water from compartment vapour to their destination store;
capture moves carbon dioxide to a store. Electrolyzers, fuel cells, Sabatier,
methane processing, water treatment, microbial processing and crew exchanges do
not model separate retained gas/liquid holdup inside the equipment. Their
throughputs, per-window changes, cumulative production, leak/ejection ledgers,
partial pressures and humidity are not additional retained inventories. Coolant
circulation is not stock. Algae culture volume describes growing capacity, not
an independently observed water charge. Maintenance reservations remain in
source-store stock until consumption; they are not additional fluid owners.
Thermal reservoirs and buffers retain energy, not separately identified fluids.

## Query boundaries

- `stored-material/<material>/<unit>` preserves the Resource-store-only boundary.
- `stored-usable/...` preserves qualified contents and hazard separation; it does
  not include compartment gas or coolant and does not imply drawable reserves.
- `accounted-material/<material>/<unit>` sums registered store and non-store
  ownership contributions. Instance labels include owner type. Aggregate,
  instance, hazard and equipment-type breakdowns use the same generic queries
  and renderer; grouping does not change the population.

Broader views deliberately say **Accounted**, not complete system total. Their
partial accounting diagnostic remains visible for unmodelled ownership and
unclassified inventories. [Crop and algae living owners](station-living-inventory-observations.md)
now participate; biological water, culture liquid and other unmodelled holdup
are not inferred. Compound store constituents have a separate accounting boundary;
see [compound inventory observations](station-compound-inventory-observations.md). Unclassified/non-canonical stores are counted
in that diagnostic, never assigned to a guessed material. Incompatible physical
quantities remain separate: no density or molar conversion is inferred. Bound
constituents are not silently treated as free gases or liquids.

Missing expected samples produce gaps in subtotals and stacks rather than
shrinking membership or substituting zero. All quantities share timestamps and
bounded coarsening. Inventory has no invented requested value.

## Verification

`station-fluidInventoryObservations` exercises the public World observation and
query seams: condensation, store/atmosphere conservation, all four atmosphere
materials, wet/dry coolant, hazard and owner-type stacks, missing owners,
non-double-counting, and deterministic Reset replay.
`station-editor-material-observations` selects the broader water view and owner
type breakdown through real headless ImGui controls, checks accounting text and
hidden history, and retains read-only/document-history assertions.
