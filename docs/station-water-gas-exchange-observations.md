# Water and gas processing exchange observations (#709)

Water collectors, Water treatment, electrolyzers and CO2 capture register typed
measurements once in `src/core/StationWaterGasMetrics.cpp`. The existing catalogue,
World-owned bounded recorder, declarative queries and generic Station Flow renderer
provide independently selectable roles, aggregate actual rates and instance
stacks, combining matching materials across legacy classes. Recording does not depend on
selection or an open editor.

## Accounting boundaries

- Collector condensate is committed process kg per tick, with the authoritative
  rated collection request. Ambient water vapour, condensate delivery and latent
  heat also have separate connection endpoint accounts. `lastCollectedKg` now
  records the committed condensate instead of remaining zero after collection.
- Treatment distinguishes raw water, treated water, retained concentrate,
  treatment consumable and supplied/requested electricity.
  Concentrate is retained output, not external disposal or transport loss.
  Treated output includes held/unqualified water: it does not imply usability.
- Electrolysis distinguishes feed water, oxygen, hydrogen, supplied/requested
  electricity and product chemical power. Chemical power is committed hydrogen
  kg per tick times the authoritative hydrogen chemical kJ/kg divided by tick
  seconds; it is energy retained in product, not electrical generation.
- CO2 capture distinguishes captured carbon dioxide, regeneration consumable and
  supplied/requested electricity. Ambient CO2 removal is a separate boundary.
- Every applicable connection exposes accepted input, delivered output and
  explicitly named external transport loss. Heat connections additionally expose
  retained source heat in kWh, separately from output rates. Thermal service
  classes remain separate. Process and connection accounts are alternative
  boundaries and must not be added together as extra production.

Material amounts use canonical per-second rates; electrical/thermal power is kW.
Consumables and concentrate use resolved explicit/intrinsic store identities,
never names or units. Unresolved identities remain unclassified. Consumable store
units are converted, while intrinsic process water/gas amounts are already kg.
Oxygen may enter a store or atmosphere without changing its intrinsic identity.
Requests are offered only for independently defined committed requests; output
stoichiometry and connection targets are not manufactured as requested production.
Known inactive exchanges are zero. Absent snapshot contributors remain missing,
queries show gaps/incomplete totals, and disabled simulations have no live history.

## Validation

Release CTest checks (run with `-j 16`):

- `station-waterGasExchangeObservations`: public World history/query values versus
  committed snapshots and connection ledgers; normal and constrained operation,
  separate losses, inactive zeros, unavailable contributor gaps, instance stack
  equality, energy-evidenced qualification, gram-to-kg consumables, different-material
  segregation, direct atmosphere oxygen delivery and disabled history.
- `station-editor-water-gas-observations`: real ImGui property and breakdown
  controls; independently selected water, gas, concentrate, consumable and power
  views reveal hidden preselection history under normal/constrained/inactive
  operation; disabled history stays unavailable; viewing preserves World state,
  observation revision and document history.
