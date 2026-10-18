# Thermal equipment exchange observations (#707)

Thermal reservoir, radiator, coolant loop, chiller and thermal buffer properties
use the applied World's shared bounded metric recorder and generic chart queries.
There is no equipment-specific renderer, editor sampling clock or second solver.
Properties default to aggregate, with the existing Instance breakdown available.

- Reservoir inputs distinguish absorbed, routed equipment, loop, condenser,
  atmosphere, generator, electrolysis, fuel-cell, Sabatier, methane-processing,
  microbial and buffer-return heat. Rejection and buffer charging are separate
  outputs. Stored thermal energy and net inventory change are separate roles.
- Radiators expose signed net environmental rejection and their physical
  reservoir connection's accepted input, delivered output and transport loss.
- Loops expose pump and freeze-heater electrical receipt with authoritative
  demand overlays, signed reservoir heat exchange, collector heat and chiller
  extraction. Thermal energy state and net exchange balance are separate views.
- Chillers expose class-specific cooling provided/requested, electrical
  supplied/requested, condenser production and physical connection endpoints.
  Evaporator extraction, condenser delivery and connection loss are not summed
  into cooling provided. COP, throughput, efficiency and availability remain
  entirely owned by the allocator.
- Buffers distinguish heat charged into storage, heat withdrawn for discharge,
  stored energy and net storage-change rate. Physical charge/discharge delivery
  and losses remain separate from the buffer-side quantities.

Each physical boundary has separately named accepted input, delivered output and
transport-loss properties. These endpoint views are alternative accounts, not
additional useful outputs. Chiller condenser and loop exchanger retained source
heat is an energy inventory, never transport loss or cooling provided.
Non-physical cooling-service and circulation-control links are excluded.

Thermal service class is descriptor metadata and part of chart grouping and
identity: classes cannot silently pool. Heat exchanges retain their signed
solver convention (including reverse exchange/back-radiation). Rates are kW;
energy inventories are converted from kJ to kWh. Reservoir/loop energy retains
the solver's zero-degrees-C reference and can be negative. Inventory state is
labelled separately from inventory-change rate and useful exchange outputs.

No material identity is required or inferred. Closed-loop coolant stock remains
owned by the separate material-inventory accounting slice, not by these charts.
Committed inactive equipment records valid zero; missing owners, connections or
measurements remain unavailable gaps. Viewing changes neither draft nor run.

Validation: `station-thermalExchangeObservations` exercises public World
recording, class separation, pump/heater demand, cooling, buffer charge/discharge,
physical throughput/efficiency, inactive/missing values and generic breakdowns.
`station-editor-material-observations` selects each thermal family through the
real headless UI and verifies retained samples, breakdown switching and read-only
viewing alongside the existing electrical/material properties.
