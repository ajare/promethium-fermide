# Algae culture exchange observations (#713)

Algae registers independently through the shared applied-run catalogue, bounded
recorder, selector, query and chart renderer. No crop adapter, equipment-specific
renderer or second simulation is used.

Separate properties cover growth CO2 uptake, culture water (assimilation plus
evaporation), bound nutrient N/P/K/mineral uptake, growth oxygen release,
evaporation, respiration oxygen/CO2/vapour, supplied lamp and circulation power,
and heat entering the atmosphere compartment. Material flows use kg/s from the
last committed fixed tick; energy service and heat use kW. Only lamp and
circulation service have authoritative requested overlays. Growth potential and
requested organic respiration are not external material demands.

Harvest to food, harvest to recovery, harvest to waste, dead residue to waste,
and uncontained dead residue loss each have distinct properties. They use
cumulative-ledger differences divided by elapsed simulation seconds between
observations, starting at run time zero. They are labelled **interval-average
event rates**, not instantaneous or sustained rates; harvest and death between
samples are retained. Every product/loss also has separate organic, bound nutrient
nitrogen, phosphorus, potassium and mineral decomposition properties and carried
chemical energy (kJ/s = kW). These are alternative representations, not additional
production. Unconfigured destination pathways are not registered. Food and recovery
surplus cascading into later destinations is measured at its actual destination,
not double-counted as another product.

Store material identities and compartment hazard class are retained; unresolved
store material is unclassified. Bound nutrient nitrogen is not atmospheric
nitrogen. Snapshot mass is already kg regardless of authored store mass units.
Living biomass inventory, culture volume and Growing capacity are not output
rates. Respiration consumes organic inventory, not bound nutrients. Mortality
routed to waste is distinct from uncontained loss. Heat actually entering the
compartment is the external output; respiration heat is not counted twice.

Known inactive exchanges record zero; absent authority remains a gap. Generic
instance and hazard/material separation, coverage and history lifecycle apply
unchanged. Viewing is read-only, including history recorded before selection or
while the editor was hidden.

Release checks: `station-algaeExchangeObservations` and
`station-editor-algae-observations` through CTest with `-j 16`. The World checks
cover growth, limited/disabled/dark operation, respiration, three-way harvest,
cascading surplus, death to waste/loss, constituent and mass balances, event
window integration, instance totals, missing authority and deterministic Reset.
