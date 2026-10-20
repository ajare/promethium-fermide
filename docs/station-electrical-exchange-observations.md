# Electrical equipment exchange observations (#706)

The applied metric catalogue registers electrical-family exchanges through domain
adapters in `StationMetrics.cpp`, recorded by the existing World-owned bounded
recorder. Selection, aggregation, instance stacks, optional requests, missing-data
gaps, canonical units and plotting use the existing generic machinery. No equipment
renderer or second allocation model is involved. The existing equipment-load service
measurement is reused, not sampled again.

## Observable boundaries

Each row below describes **separate properties**, not a mixed-output chart:

| Family | Exchanges |
| --- | --- |
| Solar array | Available generation (the authoritative availability after source connection constraints), actual pool delivery, generation cable loss |
| Generator | Actual electrical generation, pool delivery, startup electrical demand/receipt, fuel consumption, production waste heat, startup heat, decay heat, delivered heat, generation/startup/heat/fuel transport losses |
| Battery | Terminal charging input, terminal discharging output/request, pool delivery, intrinsic charge and discharge conversion losses, charge/discharge cable losses |
| External equipment load group | Electrical service/demand, recovery startup electricity, directly exported heat, delivered heat, supply/heat transport losses, each individual material input/return with its authoritative request, and material transport losses |
| Fuel cell | Generation, pool delivery, separate hydrogen and oxygen consumption, product water, production waste heat, delivered heat and each connection's transport loss |

Solar availability is not compulsory transmission: untaken surplus remains available
without producing cable losses. Neither pool delivery nor supplied service replaces
station generation. Battery output is not generation. Startup heat and electricity
are two views of the same cranking exchange, not additive production. Production
heat and delivered heat describe different boundaries; undelivered heat can remain
at the source and is not relabelled as transport loss.

Battery conversion loss rates follow the committed terminal powers and conversion
efficiencies; they exclude cable losses. Existing stored-energy and weighted charge
properties remain distinct inventory views. Equipment service excludes its separately
reported recovery startup surge, as in the authoritative snapshot.

## Materials, units and validity

Material adapters reuse the validated store identity resolver and canonical scales.
Generator fuel snapshots report consumed kWh even when the authored fuel store is
Wh, kJ or MJ; conversion to power uses `kWh * 3600 / fixed-tick seconds`. Equipment
material amounts are converted from per-tick quantities into canonical quantity per
second. Connection measurements already report rates. No observation-interval
conversion is applied to a rate, and no fuel substance is guessed from a store name
or energy unit. Fuel-cell H2, O2 and water retain independent identities.

Selectors group by equipment type, direction, individual exchange, material,
canonical quantity/unit. Unclassified material exchanges retain
instance-specific partial views rather than becoming a guessed material total.
Unsupported canonical units remain unclassified metadata, not mislabeled rates.
Electrical inputs, outputs and losses have separate selector groups. Actual sums
are the default; instance variants stack actual contributions only. Requests appear
only where the committed simulation defines them; generation has no fabricated
requested overlay. Known stopped exchanges are zero. Missing snapshot authority is
a gap and does not silently shrink an aggregate's expected population.

## Validation

- `station-electricalExchangeObservations`: public World configuration, hidden
  recording, startup/ramp, solar surplus, battery-supported constrained cables,
  material/fuel canonical rates, distinct fuel-cell reactants, missing/zero values,
  aggregate/instance equivalence and Reset.
- `station-editor-material-observations`: real Station Flow selectors and generic
  instance controls for solar, generator/fuel/startup, battery, equipment service,
  heat and material exchanges; retained preselection history and read-only viewing.
- Existing battery, material, transfer/conversion, power history and connection
  checks continue to cover the shared recorder and authoritative allocation seams.
