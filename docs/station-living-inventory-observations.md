# Living inventory observations (#704)

The applied-run recorder registers each Crop population and Algae culture as an
independent owner. `crop-population/<id>/living-biomass/inventory` and
`algae-culture/<id>/living-biomass/inventory` report current **dry** Living biomass
in kg under material identity `living-biomass`. Growing area, culture volume and
Growing capacity are installation limits, never additional material stock.
Hazard classification comes from the owning Atmosphere compartment.

Each owner also registers `.../constituent/<key>` for `dryOrganics`,
`nutrientNitrogen`, `phosphorus`, `potassium`, `otherMinerals` and `energy`.
Values are the committed snapshot's conserved `livingConstituents` and
`livingChemicalEnergyKJ` (converted to kWh), not initial composition multiplied
by remaining biomass. Respiration changes the blend without burning nutrients.
The five constituent masses sum to dry Living biomass; energy is not mass.

## Queries and ownership

`accounted-material/living-biomass/kg` includes the living owners only.
Stored harvested food, recovery material and dead residue remain owned once by
their receiving Resource stores under their own material identities.
`accounted-constituent/<key>/<unit>` combines compound-store constituents and
living constituents on a separate decomposition boundary. It does not add
constituents to material ownership totals. Existing `stored-constituent/...` and
`stored-usable-constituent/...` remain store-only; Living biomass is not qualified
usable supply. Bound nutrient nitrogen is not atmospheric nitrogen.

Growth, respiration, harvest and death use the existing simulation exclusively.
Transferred constituents disappear from the living contribution and enter the
receiving store's actual blend. Lost residue is outside retained inventory;
loss, harvest and uptake counters are not extra owners. Metadata drives the
same generic selectors, aggregate, instance, hazard and equipment-type queries.
Expected owners remain in the query when observations are unavailable, producing
gaps rather than a smaller apparent total. Shared cadence, retention, limits,
caching and applied-run lifecycle are unchanged.

## Coverage audit

- Crops and algae are the only independently modelled biological living owners.
- Biological water and culture liquid have no authoritative stock ledger. Their
  flows and installation volume do not imply retained water. Dry biomass
  declarations explicitly prohibit moisture and atmospheric nitrogen.
- Microbial processors have processing condition and capacity, but no independent
  living biomass or constituent inventory. Their inputs/outputs reside in stores.
- Crew retained-body mass has no constituent decomposition; no body composition
  is inferred from intake. This adapter remains outside the covered boundary.
- Free gases/liquids and uncharacterised stores remain outside the constituent
  boundary. Compound moisture remains separate from free-water material stock.

Accounted graphs therefore retain explicit partial-system coverage diagnostics;
none is promoted to a complete system-wide total.

## Verification

`station-livingInventoryObservations` checks both biological owners (including
identical instance IDs across types), growth, harvest, starvation with retained
residue and uncontained loss, actual constituent and chemical-energy values,
aggregate/ownership-stack equivalence, missing-owner gaps and deterministic Reset.
`station-editor-material-observations` exercises the real headless selectors and
owner breakdowns, hidden history, coverage notes and read-only document lifecycle,
then follows growth, harvest and starvation loss through material and constituent
graphs, checking the ownership stack against the aggregate and committed owners.
