# Compound-material and constituent inventory observations (#703)

The applied-run recorder registers one total and one qualified observation for
each of the seven mass constituents and chemical energy of every compound
Resource store. Values come from the committed snapshot's `actualConstituents`,
including the drift carried by deposits and subsequent draws, not nominal
composition multiplied by remaining stock. Constituent mass is already kg;
chemical energy converts kJ to kWh. Store units such as grams are not applied a
second time. Empty characterised stores report zero; missing observations are
gaps. Sampling, coarsening, limits, Reset and caching use the shared recorder.

## Ownership and chemical form

Compound material remains a single store ownership contribution in
`stored-material/...` and `accounted-material/...`. Its decomposition is a
separate population with `constituentId`, not additional `materialId` stock.
`stored-constituent/<constituent>/<unit>` exposes that decomposition, independently
of whether the containing material has a resolved identity.

Water is the wet-mass moisture term. Nutrient nitrogen, atmospheric nitrogen,
phosphorus, potassium, other minerals and dry organics partition the dry mass.
Those seven masses sum to wet compound mass; energy is not an eighth mass.
Bound nutrient nitrogen and atmospheric nitrogen remain separate selectors.
Compound moisture is not silently added to free-water material stock, nor is
bound nitrogen inferred to be freely available compartment nitrogen.

`stored-usable-constituent/<constituent>/<unit>` reports the qualified
share of the actual blend, matching the allocator's proportional-blend draw
rule. Unqualified contents are excluded. Protected reserves remain included;
qualified does not mean discretionary drawability. Qualified contributions merge
across legacy classes. Total, instance and aggregate breakdowns reuse the generic
query/stack machinery and expected membership.

## Retention audit and coverage

- Terminal waste, harvested waste and dead biological residue already reside in
  Resource stores; their actual blend is observed once at that store.
- Water-treatment held product and concentrate already reside in their output
  stores' unqualified contents. Their equipment counters are not second owners.
  The treatment model uses plain water streams, not invented compound chemistry.
- Waste awaiting treatment is still total store stock but not qualified supply.
  Ejected waste and connection/conversion losses have left the boundary: their
  cumulative ledgers are not retained inventories.
- Crew retained-body mass has no authoritative constituent decomposition and is
  not guessed from intake. Biological living inventories are now covered by the
  separate [living inventory adapter](station-living-inventory-observations.md).
- Plain uncharacterised stores have no constituent ledger. Their count is visible,
  and no composition is inferred from labels, unit, or material identity.

These views explicitly say **Compound Resource stores only**, not system total.
They remain partial for the stated broader system boundary even when all their
expected characterised-store contributions are valid. Free gases/liquids and
living biomass are outside this constituent boundary. Existing broader material
views likewise remain labelled Accounted and partial, not complete totals.
`accounted-constituent/...` now extends the store constituent boundary with crop
and algae living constituents; the stored-only selectors above remain unchanged.

## Verification

- `station-compoundInventoryObservations`: public World numerical partition,
  moisture, chemical energy, nitrogen forms, gram conversion, qualified and
  hazardous stock, transfers, retained unqualified waste, ejection, missing-owner
  gaps, no double counting and deterministic Reset.
- `station-nutrientConservation`: deposited starvation residue's observed actual
  constituents match the authoritative non-nominal blend.
- `station-editor-material-observations`: real headless selector controls, separate
  material/constituent boundaries, numerical moisture and nitrogen, usable stock,
  hidden history and read-only document/query lifecycle.
