# Crop population exchange observations (#712)

The applied-run metric catalogue registers Crops through the same bounded recorder,
selector, query and chart renderer as other equipment. No crop-specific graph or
simulation is used.

Separate properties cover growth CO2 uptake, irrigation water (assimilation plus
transpiration), bound nutrient N/P/K/mineral uptake, growth oxygen release,
transpiration, respiration oxygen/CO2/vapour, supplied lamp and circulation
power, and heat into the atmosphere compartment. Material flows use kg/s from
the last committed fixed tick; energy service and heat use kW. Only lamp and
circulation service have authoritative requested overlays. Resource-limited
growth potential and requested organic respiration are not external demands.

Harvest to food, harvest to waste, dead residue to waste, and uncontained dead
residue loss each have separate properties. These use differences of cumulative
ledgers divided by elapsed simulation seconds between observations, starting at
run time zero. They are explicitly labelled **interval-average event rates**,
not instantaneous or sustained rates, so a harvest or death between samples is
not missed. Each destination also has separate organic, bound nutrient nitrogen,
phosphorus, potassium and mineral decomposition properties and carried chemical
energy (kJ/s = kW). These are alternative representations, not additional mass
production. Unconfigured destination pathways are not registered.

Store material identities are retained; unresolved
store material is unclassified, never inferred from its name or unit. Bound
nutrient nitrogen remains distinct from atmospheric nitrogen. Snapshot masses
are already kg even if a destination is authored in another mass unit.

Living biomass inventory, Growing capacity, and stored harvested products are
not output rates. Respiration consumes the living organic inventory; it does
not consume its bound nutrients. Mortality routed to waste is distinct from
uncontained loss. Only heat actually entering the compartment is an external
heat output; respiration heat is not counted again as separate production.

Known inactive exchanges record zero; missing authority remains a gap. Generic
instance breakdowns, material separation, requests, coverage and history
lifecycle apply unchanged. Viewing remains read-only and reveals history recorded
before selection or while the editor was hidden.

Release verification: `station-cropExchangeObservations` and
`station-editor-crop-observations`, plus existing crop, living-inventory and
exchange/editor regressions, through CTest with `-j 16`.
