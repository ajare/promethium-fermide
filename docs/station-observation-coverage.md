# Station observation coverage audit (#715)

## Reproduce the mixed station

`smoke::station::mixedObservationFixture()` starts with the packaged **Meridian
reference station**, adds a compound conversion
and a second, unequal battery bank, and uses the ordinary applied configuration.
It is shared by `station-observationIntegration` and
`station-editor-mixed-observations`; it is not a second simulation. Load Meridian
in Station Flow for the packaged example. The supplementary conversion/bank are
headless acceptance fixtures, not modifications to Meridian's qualified scenario.

The public check audits family membership, unique metric identities, all selected
queries against exported observations, canonical rates, separate inputs/outputs,
weighted charge, ownership, hidden history, indexed extraction, pause, refused and
unchanged application, Reset/replay and World persistence/replacement. The editor
check drives **every applicable property and breakdown** with real controls,
optional requested overlays, units, cached frames, draft authoring/undo, Reset,
property disappearance and visible Power fallback. Earlier family checks remain
the numerical authority for their individual flows; this check does not replace
those assertions with mere catalogue presence.

## Equipment register

Every vector-valued equipment family in `StationSystemsSettings` is audited below.
Entries name separate views, not quantities that should be added together.
All adapters read committed snapshots; material amounts per tick become canonical
rates, while event totals are differenced over actual observation windows.

| Authored family | Registered measurements / boundary | Numerical checks |
| --- | --- | --- |
| Resource stores | total and qualified inventory; separate material/constituent views | materialObservations, compoundInventoryObservations, materialCanonicalUnits |
| Transfers | source receipt, destination delivery/request, each connection loss | exchangeObservations |
| Conversions | each of up to five inputs and five outputs/request, individual losses | exchangeObservations, observationIntegration |
| Solar arrays | available generation, pool delivery, cable loss | electricalExchangeObservations |
| External equipment load groups | electrical service/request, recovery startup, exported/delivered heat, retained source heat, each material input/return/request, connection losses | electricalExchangeObservations, observationIntegration |
| Batteries | stored/nameplate energy, weighted charge, terminal charge/discharge/request, pool delivery, intrinsic and cable losses | batteryObservations, electricalExchangeObservations |
| Deliveries | delivered, accepted, rejected event-window rates | scheduledExchangeObservations |
| Waste ejections | actually ejected disposal-ready material, never held/unmet waste | scheduledExchangeObservations |
| Outages | **No physical exchange**: availability schedule, not resource production/consumption. Effects appear in the affected equipment's committed measurements. | outages, observationIntegration |
| Thermal reservoirs | separately routed inputs, rejection, buffer charging, stored energy, net change | thermalExchangeObservations |
| Radiators | signed environmental rejection, physical accepted/delivered/lost heat | thermalExchangeObservations |
| Coolant loops | finite coolant stock, pump/heater electricity/request, reservoir/collector/chiller heat, energy/change, connection endpoints/loss/retained heat | fluidInventoryObservations, thermalExchangeObservations |
| Chillers | class-specific cooling/request, electricity/request, evaporator/condenser heat, endpoint/loss/retained heat | thermalExchangeObservations |
| Generators | generation/pool delivery, startup electricity/request, fuel, production/startup/decay heat, delivered/retained heat, connection losses | electricalExchangeObservations, observationIntegration |
| Atmospheres | O2/N2/CO2/vapour stock; occupancy, Crew, crop/algae, condensation/collection, electrolysis/capture, leakage and heat accounts | fluidInventoryObservations, crewAtmosphereExchangeObservations |
| Airlock cycles | removed/recovered/vented mixture event-window rates; separate retained recovered-mixture stock | crewAtmosphereExchangeObservations |
| Crews | separate food/water/O2 demand and consumption, urine/waste, CO2/moisture/heat, retained body mass | crewAtmosphereExchangeObservations |
| Water collectors | condensate/request, ambient/physical endpoints, electrical/thermal accounts and losses | waterGasExchangeObservations |
| Water treatments | raw/treated/concentrate/consumable/clearance flows and requests, electricity, endpoints/losses | waterGasExchangeObservations |
| Electrolyzers | water, H2/O2, electricity/request, product chemical energy, heat/endpoints/loss/retained heat | waterGasExchangeObservations |
| CO2 capture | captured gas/request, electricity/request, consumables, ambient/store endpoints/losses | waterGasExchangeObservations |
| Fuel cells | dedicated H2/O2, water, electrical generation/request, pool delivery, heat/endpoints/loss/retained heat | carbonExchangeObservations, electricalExchangeObservations |
| Sabatier reactors | distinct CO2/H2 inputs, CH4/water outputs, electricity/request, heat/endpoints/loss/retained heat | carbonExchangeObservations |
| Methane processors | CH4 input, H2/carbon outputs, electricity/request, heat/endpoints/loss/retained heat | carbonExchangeObservations |
| Microbial processors | compound feed/consumables, O2/water, nutrients/recovered water/CO2/residue, heat, electricity, endpoints/losses | microbialExchangeObservations |
| Thermal buffers | stored energy, charge/discharge/change, physical endpoints/losses | thermalExchangeObservations |
| Crop populations | living dry mass/actual constituents/energy; ambient uptake/respiration/transpiration, electricity, heat, harvest/mortality material/constituent/energy windows | livingInventoryObservations, cropExchangeObservations |
| Algae cultures | living dry mass/actual constituents/energy; ambient exchanges, electricity/heat, independent food/waste/fertilizer harvest and mortality windows | livingInventoryObservations, algaeExchangeObservations |
| Maintenance jobs | actually consumed requirements as event-window rates | scheduledExchangeObservations |
| Dispatch/recovery/protected-service settings | **No independent physical owner or exchange**: control policy. Observe resulting service, startup, supply and unmet demand through their owners, not an extra controller output. | dispatch, protectedServices, recovery |

Generation is not supplied service; battery discharge is not generation. Heat is
segregated by declared thermal service class, including generator and external
load heat. Electrical-family retained source heat is now explicitly selectable as
energy, not silently omitted or called transport loss.

Conversions publish per-owner completed input/output flows, but no per-owner
constituent-loss decomposition. The station-wide constituent balance's cumulative
`lost` diagnostic is not an equipment-specific material loss measurement. Do not
invent a conversion material-loss property by subtracting different substances
or nominal compositions; keep those conservation diagnostics at their existing
station boundary. Unmet requests likewise are not actual physical losses.

## Inventory ownership register and unresolved boundaries

| Owner | Accounting decision |
| --- | --- |
| Resource store, including terminal waste, treatment-held water, concentrate, recovered nutrients and deposited harvest | Qualified + unqualified contents counted **once**. Qualified views remain hazard-separated and include protected reserves. Waste/treatment/product ledgers are not extra owners. |
| Compound Resource store | Material mass and actual constituent decomposition are **alternative** views. Stored bound nitrogen is not free atmospheric nitrogen. Chemical energy is separate from mass. |
| Atmosphere compartment | Four independently measured gas/vapour inventories, not also its derived pressure, humidity, cumulative flows or mixture sum. |
| Coolant loop | Finite coolant charge, never inferred potable water. Circulation is a rate, not another stock. |
| Crop/algae living population | Dry living biomass and actual retained constituents/chemical energy; harvested/dead material already deposited in stores is not counted twice. Growing area/culture volume are capacities, not living or water stock. |
| Crew | Retained body mass is a selectable inventory state. No authoritative constituent/body composition exists; excluded from material/constituent subtotals rather than guessed. |
| Airlock recovery receiver | Recovered mixture is retained in a separately selectable inventory state, read from the cumulative recovery account (no withdrawals are modeled). There is no authoritative retained species decomposition or usable store; observe recovered mixture separately, never add it to free O2/N2/CO2/water. |
| Reservoir, loop, buffer, physical heat-source retention | Energy inventories, not inferred fluids or new material owners. Reference-temperature thermal energy can be signed. |
| Processing equipment, microbial installation, water/biological holdup | No separately modeled retained process-fluid or microbial-biomass measurement. Throughput, condition, nominal composition and installation capacities cannot manufacture it. |
| Passive compartment condensation | Condensed water is an exported compartment balance term, not a modeled tank of retained condensate. Collector condensate delivered to a store is owned by that store, not this cumulative ledger. |
| Maintenance reservation/reusable tools | Material remains in its source store until consumed; no second owner. Consumption deposits into the configured waste store. |

**Incomplete coverage remains visible.** Broader views are labelled **Accounted**,
not complete system inventory. Their partial-accounting text explicitly excludes
unmodeled holdup/body composition and reports unclassified stores. Species-resolved
Airlock recovery and uncharacterised inventories remain unresolved accounting
boundaries, not a claim of complete chemical/system conservation. No available
physical measurement is replaced by an inferred one. See the linked family docs
in [station-metrics.md](station-metrics.md) for detailed ownership limitations.

## Authoritative external roles

- Supply: solar availability versus accepted pool delivery; finite primary fuel;
  accepted resupply (rejected delivery never entered inventory).
- External loads: each configured material input/return, electrical receipt,
  exported or routed heat, startup energy. These are not sampled from World Agents.
- Environment: radiator signed net radiation, compartment leakage, passive
  condensation, vented Airlock mixture, uncontained biological residue,
  transport losses. Do not fold these into useful production.
- Crew/ambient biological exchanges: explicit endpoint accounts, including roles
  without visible Ports. Crew/compartment endpoints are alternative accounts of
  the same exchange, not additive station production.
- Disposal: actually ejected waste only. Retained treatment-held waste and unmet
  ejection requests remain distinct.
- Maintenance: consumed material enters the existing waste owner; reusable tools
  and reservations are not external losses.
- Other equipment: internal station transfers/conversions and finite reactants
  are not deliveries from outside the station. Their endpoint accounts remain
  separately selectable to explain local boundaries.

## Bounded operation and work evidence

`station-observationBounds` drives 1024 slots through 4096 observations, aligned
coarsening, missing spans and bounded selected queries; checks explicit two-row,
metadata-note, memory and cardinality refusal. `station-trends` additionally
covers retention and public World long runs. Recording reserves the compiled
catalogue, owned notes, snapshot indices/readers, one selected query workspace and
one editor plot copy in its total budget; see [station-history.md](station-history.md).
No per-property caches accumulate. Over-budget runs refuse **all** recording with
a visible limit message rather than picking whichever metric family fits first.

`getStationSystemsQueryWork()` reports query/export work plus observation lookup
and index-entry counts. Snapshot collection indices are built once per applied
run (on first observation), then reused with identity validation. Extraction does
indexed lookups, not a linear scan per role. Mixed acceptance asserts equal work
per equal observation window, no repeated index construction, zero hidden chart
queries, only the selected membership exported, and unchanged-frame query/plot
reuse. Contribution colours use the same stable identity hash in every view;
changing ordering, selection or breakdown does not renumber an instance colour.
