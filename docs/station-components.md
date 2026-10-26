# Station systems component reference

This is an authoring reference for the current Station systems model: what each
component does, how to connect it, what its settings mean, and how to combine it
with other components. Names follow the **Station Flow** placement menu where
one exists. Examples describe modelling arrangements, not complete calibrated
configurations; rates, compositions, energy budgets and capacities must still
pass validation.

For editor operation, Apply/Reset, telemetry and execution details, see
[Station systems](station-systems.md). For water chemistry examples, see
[Compound water](compound-water.md). Save/load details are in
[Station configuration documents](station-configuration-documents.md).

## Contents

- [Shared authoring rules](#shared-authoring-rules)
- Resources: [Store](#store), [Transfer](#transfer), [Conversion](#conversion)
- Power: [Solar](#solar), [Equipment](#equipment), [Battery](#battery),
  [Generator](#generator), [Fuel cell](#fuel-cell)
- Conversion chain: [Sabatier](#sabatier), [Methane processor](#methane-processor)
- Thermal: [Reservoir](#reservoir), [Radiator](#radiator),
  [Coolant loop](#coolant-loop), [Chiller](#chiller),
  [Thermal buffer](#thermal-buffer), [Thermal link](#thermal-link),
  [Heat engine](#heat-engine)
- Gas: [Atmosphere](#atmosphere), [Atmosphere pathway](#atmosphere-pathway),
  [Airlock cycle](#airlock-cycle), [Electrolyzer](#electrolyzer),
  [CO2 capture](#co2-capture)
- Water: [Water collector](#water-collector), [Water treatment](#water-treatment)
- Biology: [Crew](#crew), [Crops](#crops), [Algae](#algae),
  [Microbial processor](#microbial-processor)
- Schedules: [Delivery](#delivery), [Waste ejection](#waste-ejection),
  [Outage](#outage), [Maintenance](#maintenance)
- [Derived nodes, prefabs and station-wide controls](#derived-nodes-prefabs-and-station-wide-controls)
- [Worked system arrangements](#worked-system-arrangements)
- [Implementation sources](#implementation-sources)

## Shared authoring rules

### Ports, services and actual operation

- A port is a reference to another component, not a pipe simulation of its own.
  An output normally joins an input. Most machine ports accept **one** peer;
  Store inventory faces and Reservoir heat/hosting faces accept many.
- Machine material inputs join **Store → Inventory out**; material outputs join
  **Store → Inventory in**. Two Stores cannot join directly: insert a Transfer
  or Conversion. Likewise, process outputs generally land in Stores before
  another process draws them.
- An optional port may be left unused only when its associated feature does not
  require it. Hollow/unwired ports make partially built drafts possible; they
  do not waive complete-configuration validation at Apply.
- **Power in/out is authored through Power Buses (#881).** Wire producers
  to bus inputs and bus outputs to consumers; do not cable Solar directly to
  a load. The default `main` membership preserves legacy documents. Power
  ports can be disconnected and then read as incomplete. Cooling-service
  relationships remain derived, not hand-wired.
- A rating is a ceiling, not promised production. Available inputs, power,
  temperature gates, output headroom and connection constraints can reduce
  operation. Shortfalls are reported as Unmet demand or a named limiter.
- Settings edits change a **Configuration draft**, not the running simulation,
  until Apply. Starting values initialize a new/reset run; they are not live
  inventory setters. Placement/layout has no physical effect.

### Units and material requirements

| Term used below | Meaning |
| --- | --- |
| Native units | The exact unit authored on the referenced Store. A rate in native units/s is not automatically kg/s. |
| Mass Store | A Store using `g` or `kg`; chemical/biological machines convert these to kg internally. `L` is not interchangeable without a declared model. |
| Plain Store | No declared composition. It tracks one quantity rather than a declared compound blend. |
| Compound Store | A mass Store with declared composition and constituent accounting. Even an all-zero dry composition is an explicit compound declaration. |
| Qualified / usable | Inventory permitted for ordinary use. Held/unqualified material still occupies capacity but ordinary Transfers cannot draw it. |
| Protected reserve | Qualified inventory withheld from discretionary draws; usable inventory is not necessarily drawable surplus. |
| Thermal service class | One of `low`, `moderate`, `warm`, `generation`. Matching is required where specified; these are not hazard labels. |

Material identity is descriptive metadata, **not** permission to wire a port.
Naming a Store `oxygen` does not give it atmospheric pressure, nor does setting
`materialId: water` bypass a unit/composition requirement. No density or molar
conversion is inferred from names. Obsolete normal/hazardous classes and
release-clearance tests are not current controls: containment comes from actual
inventories, wiring, treatment evidence and consumer requirements.

Compound composition uses **moisture as a fraction of wet mass**. Nutrient
nitrogen, atmospheric nitrogen, phosphorus, potassium and other minerals are
fractions of **dry mass**; their sum cannot exceed one. The dry remainder is
`dryOrganics`. `energyKJPerKgDry` is chemical energy, not another mass fraction.
Bound nutrient nitrogen is not interchangeable with atmospheric nitrogen.
Actual mixed inventory can differ from the starting declaration.

### Shared dials

- **Values:** authored numeric values must be finite; capacities, rates and
  fractions have the component-specific bounds described below.
- **ID:** stable reference name, normally nonempty and at most 64 characters;
  use the editor's rename operation to update references together.
- **Priority:** where present, higher numbers allocate/run first; declaration
  order breaks applicable ties. Priority does not create missing resources.
- **Profile:** a constant fraction in `[0,1]`, or a repeating piecewise-constant
  sequence with a positive period, up to eight segments, first starting at
  second zero. Segment start times are strictly ordered within the period.
  Solar illumination, equipment duty, crew activity and biological lighting
  each scale their own component; they do not create extra components.
- **Schedules:** first occurrence is non-negative simulation time; recurrence
  zero means one-off, otherwise at least `1/60 s`. Outage/maintenance recurrence
  must also be at least the window duration. These use simulation seconds, not
  display refreshes or tick counts. Pausing freezes them.
- **Connection properties:** on physical exchanges, maximum throughput caps
  connection **input**, then efficiency `[0,1]` sets delivered output. For example,
  `10 kg/s × 0.9` delivers at most `9 kg/s`. Electrical/heat rates use kW;
  Store material connections use native units/s. Transport loss leaves the
  model; heat unable to enter a constrained connection stays at its source.
  Carrier type describes plumbing/cabling presentation, not extra capacity.
  Ratings on non-physical schedule/control/service relationships are
  informational. Pathways and Thermal links have their own laws instead of
  throughput/efficiency controls.

## Resources

### Store

**What it does:** holds a finite resource inventory, split into qualified and
held contents. A tank, pantry, fuel stock, nutrient bin or waste receiver can
all be represented by a Store. It does not process, purify or move material.

**Ports:** **Inventory in** accepts outputs from Transfers and processes;
**Inventory out** supplies their inputs. Both allow multiple connections.
The connected machine determines the required unit and composition.

| Dial/value | Purpose |
| --- | --- |
| Unit | One of `g`, `kg`, `mL`, `L`, `m3`, `mol`, `kJ`, `MJ`, `Wh`, `kWh`. All inventory/capacity numbers use this unit. |
| Capacity | Positive upper bound on qualified **plus** held contents. |
| Starting usable / `startingQualified` | Qualified contents at initialization. |
| Starting held / `startingUnqualified` | Contents awaiting processing; together with usable stock must fit capacity. |
| Protected reserve | Amount of qualified stock withheld from discretionary use; cannot exceed starting qualified inventory. |
| Material identity | Optional lowercase hyphenated material ID for observations/grouping; empty means unclassified. |
| Declared composition | Enables compound accounting; requires `g` or `kg`. Moisture, dry fractions and chemical energy describe the starting/declared material. |

**Example:** a `kg` drinking-water Store holds 500 kg, starts with 300 kg usable
and protects 100 kg. Crew consumes water; a surplus Transfer may refill a
separate Electrolyzer feed tank without spending the protected portion. Use a
compound declaration for the Crew water Store, as its port requires one.

### Transfer

**What it does:** moves qualified material between two Stores without changing
its material composition, either continuously or as scheduled batches.

**Ports:** **Source** joins one Store's Inventory out; **Destination** joins a
different Store's Inventory in. Transfer unit must exactly match both Stores
(`g` to `kg` is not automatic). Compound endpoints normally need matching
declared compositions; compound-to-plain movement is refused. The supported
plain-water dilution exception permits plain inflow into a compound treatment
water port, adding water only rather than inventing the destination's dry load.

| Dial/value | Purpose |
| --- | --- |
| Mode | Continuous rate or Scheduled inventory event. |
| Unit | Shared native unit of source, destination and transfer. |
| Rate per second | Positive continuous request in native units/s; inventory, reserve and headroom may limit it. |
| Quantity | Positive Scheduled request per occurrence. |
| First occurrence / recurrence | Scheduled timing; recurrence zero is one-off. |
| Priority | Orders competing transfers/events. |

Continuous physical connections obey throughput and efficiency. Scheduled
transfers are instantaneous, **lossless, non-physical** events: they respect
inventory/reservations/headroom, but not physical cable/pipe ratings. A partial
batch completes that occurrence; it is not retried or carried into the next.

**Example:** a recurring Scheduled Transfer moves food from a bulk Store to a
small pantry every six hours. Crew consumes the pantry between restocks. If it
empties early, Crew reports unmet food until a later restock.

### Conversion

**What it does:** transforms compound inputs into compound outputs at declared
full-throughput rates. Use it for a material-processing recipe not already
represented by a dedicated machine.

**Ports:** one to five **Conversion input** Stores and one to five **Conversion
output** Stores. Every Store must be compound and mass-denominated; a Store may
appear only once across the recipe. Different mass units are handled through
mass accounting. Outputs may not create tracked constituents or chemical
energy; deficits are explicitly accounted losses. Different compositions are
allowed precisely because this is a conversion, not a Transfer.

| Dial/value | Purpose |
| --- | --- |
| Input rate per second | Positive native wet-mass units/s consumed at full operation for each input. |
| Output yield per second | Positive native wet-mass units/s produced for each output at that same full operation. |
| Input/output count | Growable sets, maximum five on each side. |
| Priority | Allocates competing conversion requests. |

The limiting input or output headroom scales the whole recipe. There is no
automatic electricity input or thermal output: do not assume this generic
component models the energetics of an arbitrary real chemical reactor.

**Example:** separate a compound harvest Store into food and fibrous waste
Stores, with each recipe's constituent and chemical-energy outputs funded by
the input. This models a conservative sorting/processing boundary rather than
turning a waste label into food for free.

## Power

### Power bus

An authored distribution circuit, placed from the Power group. Its input and
output faces accept unlimited landings/taps. The inspector authors display name,
rated capacity (0 is visibly **Unrated**) and trip persistence. Source/battery
memberships and consumer Single/Transfer/Shared supply modes are authored in
inspectors or by wiring. Rename/delete cascades all references and supports
undo/redo. See [Power Bus authoring](station-configuration-documents.md#power-bus-authoring-881).
The older per-component descriptions of *automatic* electrical membership below
refer to the legacy default-main topology, not an uneditable shared Power node;
that derived node is retired. Transfer executes whole-load failover with a
dwell-gated return (#882). Shared combines each bus's live post-protected spare
pro-rata, preserving explicit unmet demand and applying the minimum-operable
gate to the combined grant (#883). Breaker protection remains a separate
follow-up (#884).

Each source or battery landing has its own cable throughput, efficiency and
isolation state (#906). Multiple landings share one finite native source output
or battery energy/rate budget; they do not copy capacity. Actual transfers are
metered on the cable to the serving bus, including battery charging and refunded
protected-service credits. Unused solar is availability rather than transmitted
power; unused fuelled output is settled through the remaining live feeder budgets.

### Solar

**What it does:** offers electricity according to installed capacity and a
sunlight/eclipse profile.

**Ports:** automatic **Power out** to the shared electrical pool; no material
input or manually wired power destination.

| Dial/value | Purpose |
| --- | --- |
| Capacity kW | Full-sun installed electrical capacity; zero disables the array. |
| Illumination profile | Fraction of full sun over simulation time; zero represents eclipse. |

**Example:** an orbital Solar array supplies Equipment and charges a Battery in
sunlight. During eclipse, the Battery and any dispatched Generator/Fuel cell
must cover demand. Available solar capacity is not a guarantee of useful
transmitted power through a constrained generation connection.

### Equipment

**What it does:** represents an external equipment load group: a laboratory,
workshop, communications rack or other load not already modelled internally.
Power, heat and any declared material exchanges scale together with admitted
operation. Do not duplicate a Chiller's or Electrolyzer's own demand here.

**Ports:** automatic **Power in**. Up to **two total** material exchanges across
**Equipment input 1/2** and **Equipment return 1/2**, each naming a distinct
Store; rates use that Store's native units. Material ports are optional, but
once declared they constrain operation. Optional **Heat out** joins a Reservoir
of the same thermal class **only for Exported disposition**. CoolingService
instead uses the derived cooling service of its class; it cannot also name a
passive heat sink.

| Dial/value | Purpose |
| --- | --- |
| Electrical demand kW | Full-operation electrical requirement. |
| Operating profile | Duty fraction applied over time. |
| Heat output kW | Full-operation heat output; scales with actual operation. |
| Thermal class | Required class of cooling service or declared heat sink. |
| Cooling disposition | Exported: explicit external heat boundary, or route to a Reservoir if named. CoolingService: requires available chilled service of its class. |
| Critical floor kW | Protected slice of this load's service, not an independent mini-load. |
| Maximum curtailment | Largest removable fraction of demand. `0` requires full operation; `1` permits complete curtailment. Below the minimum operable level the load is refused whole. |
| Priority | Ranks discretionary service allocation. |
| Material rate / consumes | Native units/s at full operation; consumes draws qualified surplus, return requires destination headroom. |
| Recovery dwell seconds | Minimum shed duration before a discretionary load may restart. |
| Recovery hysteresis fraction | Extra spare-power headroom above demand required for restart. |
| Restart surge kW / seconds | Extra startup demand and its duration. Recovery controls apply to purely discretionary loads, not loads with a critical floor. |

**Example:** a workshop takes stock from one Store, returns waste to another,
requests 5 kW and emits 4 kW into a matching Reservoir with a Radiator. A full
waste Store throttles workshop operation, power and stock consumption together.
It does not continue consuming electricity while silently deleting waste.

### Battery

**What it does:** stores electrical energy, charges from surplus generation and
discharges into deficits within power and reserve limits.

**Ports:** automatic **Power in** for charge and **Power out** for discharge;
no authored material or heat-sink connector.

| Dial/value | Purpose |
| --- | --- |
| Nameplate kWh | Total installed energy capacity. |
| Usable kWh | Cyclable portion; reserve floor is `nameplate − usable`. Zero usable capacity disables the bank. |
| Starting stored kWh | Initial total stored energy, within nameplate. Energy below the reserve floor is not dischargeable. |
| Maximum charge/discharge kW | Separate limits on how quickly energy can enter or serve the bus. |
| Charge/discharge efficiency | Stored energy per input bus energy, and delivered bus energy per stored energy drawn; each in `(0,1]`. Losses are accounted. |

**Example:** a 20 kWh usable bank supplying a 5 kW eclipse load lasts at most
four hours before conversion/cable losses. A 2 kW discharge limit cannot supply
that load in full even when the bank contains plenty of energy.

### Generator

**What it does:** converts finite stored fuel energy into dispatchable
electricity, with real startup draw, ramping, waste heat and shutdown heat.
It is an energy contract, not a nuclear-reactor physics model.

**Ports:** **Fuel in** requires an energy Store (`kJ`, `MJ`, `Wh` or `kWh`).
**Heat out** requires a same-class Reservoir. Automatic **Power in** funds
startup and **Power out** supplies generation. Supervision accepts Outage and
Maintenance references.

| Dial/value | Purpose |
| --- | --- |
| Prime mover | Piston engine (`reciprocating`, default), gas turbine (`gas-turbine`), or Stirling (`stirling`). Identity is saved and published; selecting a type never rewrites other dials. |
| Rated output kW | Installed electrical ceiling; zero disables. |
| Commanded output kW | Manual dispatch request, between zero and rated. Automatic dispatch may replace the operating command. |
| Efficiency | Electrical output / consumed fuel energy, `(0,1]`; the remainder becomes waste heat. |
| Maximum ramp kW/s | Positive output slew limit, not an instantaneous step. |
| Startup power kW / seconds | Cranking draw and time that must actually be powered before running. |
| Shutdown heat kW / seconds | Continued heat after generation stops. |
| Thermal class / heat sink | Waste, startup and shutdown heat destination. |
| Maximum sink temperature °C | Cooling gate; the sink must be below it for qualified operation. |

**Prime mover limits (#897):** reciprocating admits the full legacy efficiency
range `(0, 1]` and any finite positive ramp, including `100000 kW/s`.
Gas turbine admits efficiency `[0.15, 0.38]` and ramp at most `200 kW/s`;
Stirling admits `[0.20, 0.42]` and at most `30 kW/s`. Band endpoints and ramp
ceilings are inclusive. The selector shows these limits as read-only hints;
out-of-profile values are validation errors naming the type and limit, never
silently clamped. All three types retain the same authored energy contract and
phase machine. Injector/frozen-topology gating remains a follow-up (#898).
The `primeMover` key is always saved; missing keys load as `reciprocating`.
Non-default types require World/configuration schema 125; all-piston documents
retain their existing conditional schema version. Unknown types are refused.

**Example:** a backup Generator draws from a finite `kWh` fuel Store and rejects
heat to a Reservoir/Radiator plant. A Battery supplies cranking energy and
bridges the ramp. Stopping the Generator does not remove its shutdown-cooling
requirement, so protect the relevant Coolant loop/Chiller if needed.

### Fuel cell

**What it does:** consumes hydrogen and oxygen to generate electricity and
product water; waste heat goes to its declared sink.

**Ports:** **Hydrogen in**, **Oxygen in** and **Water out** use plain mass Stores;
reactants must not alias one inventory. **Heat out** requires a same-class
Reservoir. Electricity automatically joins **Power out**. Use dedicated
reactant Stores if breathing oxygen must remain separate: no hidden draw from
an Atmosphere or life-support reserve exists.

| Dial/value | Purpose |
| --- | --- |
| Chemistry | PEM (default), alkaline or solid oxide. Persisted and shown in telemetry; currently descriptive only, with identical runtime behaviour and no chemistry-specific efficiency bands. |
| Rated output kW | Installed electrical rating; zero disables. |
| Commanded output kW | Dispatch request within rating; automatic dispatch can command backup output. |
| Efficiency | Electricity / hydrogen chemical energy, `(0,1]`. |
| Thermal class / sink | Waste-heat interface. |
| Maximum sink temperature °C | Stops operation when cooling cannot accept the heat. |

**Example:** Solar-powered Electrolyzers fill separate H2 and O2 Stores. A Fuel
cell uses them during eclipse and sends water to a raw-water Store for Water
treatment. Full water storage or exhausted reactants limits output; this cycle
is storage with losses, not a self-powering loop.

## Conversion chain

### Sabatier

**What it does:** performs `CO2 + 4 H2 → CH4 + 2 H2O`, recovering water from
captured CO2 while retaining carbon in methane. Reaction heat and electrical
process energy reach the heat sink.

**Ports:** **CO2 in**, **Hydrogen in**, **Methane out**, **Water out** each require
a different plain mass Store. **Heat out** joins a same-class Reservoir;
**Power in** is automatic. The process needs actual feed, electrical energy and
product headroom together.

| Dial/value | Purpose |
| --- | --- |
| Rate kg/s | Rated **CO2 feed** conversion rate; zero disables. |
| Specific energy kWh/kg CO2 | Electrical processing dose for actual conversion. |
| Thermal class / sink | Destination for reaction and process heat. |
| Maximum sink temperature °C | Cooling eligibility gate. |
| Priority | Processing power/service allocation order. |

**Example:** CO2 capture fills a CO2 Store; an Electrolyzer supplies hydrogen.
Sabatier returns water to the treatment chain and methane to a Methane
processor. Account for heat rejection and net hydrogen demand rather than
assuming all hydrogen recycles.

### Methane processor

**What it does:** performs `CH4 → C + 2 H2`, producing solid carbon and
recovering part of the hydrogen used by Sabatier. Electricity must fund the
endothermic reaction; remaining process heat goes to its sink.

**Ports:** **Methane in**, **Hydrogen out**, **Carbon out** require distinct plain
mass Stores. **Heat out** needs a same-class Reservoir; **Power in** is automatic.

| Dial/value | Purpose |
| --- | --- |
| Rate kg/s | Rated **methane feed** processing rate; zero disables. |
| Specific energy kWh/kg CH4 | Must at least fund the reaction's heat demand; not an arbitrary free yield. |
| Thermal class / sink | Process-heat destination. |
| Maximum sink temperature °C | Cooling gate. |
| Priority | Processing allocation order. |

**Example:** process Sabatier methane, return hydrogen to the shared H2 feed
Store, and retain carbon in a finite Store with scheduled Waste ejection. A
full carbon Store backpressures the chain. Recovered H2 is less than Sabatier
consumed, so the combined loop still needs fresh hydrogen.

## Thermal

### Reservoir

**What it does:** holds thermal energy in an effective thermal mass. Heat
inputs change its temperature; hosted Radiators reject heat, loops exchange
heat, and buffers temporarily absorb deficits. It is not an infinite heat sink.

**Ports:** **Heat in** accepts multiple compatible machine outputs and
heat-only exchangers. **Hosted radiators** and **Hosted buffers** mount those
components. Diagram direction does not make passive exchangers one-way; actual
heat exchange can reverse with temperature. Heat-engine references can also
use Reservoirs, but are not currently editable Flow ports.

| Dial/value | Purpose |
| --- | --- |
| Thermal class | Class of heat/service the reservoir supports. |
| Heat capacity kJ/K | Positive energy required for a one-degree temperature rise. |
| Starting temperature °C | Initial thermal state. |
| Minimum/maximum temperature °C | Service limits reported as crossings, not clamps or automatic thermostats. |
| Absorbed heat kW | Constant environmental heat input. |

**Example:** a Generator emits heat into a `generation` Reservoir. A Radiator
removes heat according to temperature and environment. Too little radiator
area eventually heats the Reservoir above the Generator's cooling gate.

### Radiator

**What it does:** exchanges radiative heat with an external environment using
`emissivity × view factor × area × σ × (T⁴ − T_sink⁴)` in absolute temperature.
It is a real temperature-dependent rejection path, not a fixed cooling credit.

**Ports:** **Host reservoir** joins one Reservoir's **Hosted radiators** face.
Multiple Radiators may share a host. There is no gas or coolant-mass input.

| Dial/value | Purpose |
| --- | --- |
| Area m² | Positive emitting area; more area increases exchange at the same temperatures. |
| Emissivity | Surface emission effectiveness, `(0,1]`. |
| View factor | Effective view of the declared environment, `(0,1]`. |
| Sink temperature °C | Effective radiative background, including back-radiation; not necessarily deep space. |

**Example:** mount a panel on an equipment-heat Reservoir. Reduce its view
factor to represent obscuration, or raise sink temperature for a warmer
radiative environment. The resulting loss of rejection can exhaust a Thermal
buffer and ultimately stop heat-producing equipment.

### Coolant loop

**What it does:** owns a finite, closed coolant inventory and pumps it through
a heat-only exchanger with one Reservoir. It also models freeze-protection
heating. Coolant never becomes drinking water or moves into another component.

**Ports:** **Heat exchanger** joins a Reservoir's Heat in. **Cold interface**
serves a Chiller evaporator; at most one Chiller may claim a given loop.
**Cold surfaces** serves Water collectors. **Buffer gates** can gate Thermal
buffers. **Power in** is automatic; Supervision accepts Outage/Maintenance.

| Dial/value | Purpose |
| --- | --- |
| Coolant capacity / starting coolant kg | Maximum and initial internal inventory. Zero starting coolant disables circulation and heating. |
| Specific heat kJ/kg·K | Coolant thermal capacity per kg; combines with actual inventory to set temperature response. |
| Starting temperature °C | Initial coolant state. |
| Pump power kW | Electrical draw at full circulation. |
| Rated flow kg/s | Circulation rating; not a transfer of inventory out of the loop. |
| Exchanger conductance kW/K | Full-circulation heat coupling to the host Reservoir. |
| Freeze-protection setpoint °C | Thermostat threshold below which the heater requests power. |
| Heater capacity kW | Maximum freeze-protection electrical/heat input. |
| Critical service | Explicitly protects circulation/heating ahead of discretionary external loads. |

**Example:** a loop exchanges heat with a cabin-cooling Reservoir and supplies
a collector's cold surface. A Chiller extracts heat from that loop into a
separate rejection Reservoir. During a pump Outage, collection and
circulation-gated buffer access cease even if the loop still contains coolant.

### Chiller

**What it does:** spends electricity to extract heat from a circulating loop
and rejects that heat **plus electrical work/auxiliaries** into a Reservoir.
It also supplies the derived cooling service of its thermal class.

**Ports:** **Cold loop** joins one Coolant loop's **Cold interface**;
**Condenser heat** joins a different Reservoir from the cold loop's host.
The Chiller class must match **both** the cold host and condenser Reservoir.
**Power in** is automatic; cooling-service output is derived. Supervision
accepts Outage/Maintenance.

| Dial/value | Purpose |
| --- | --- |
| Rated cooling capacity kW | Maximum extracted heat, not electrical consumption; zero disables. |
| COP | Extracted heat per unit compressor work; work is cooling/COP. Positive authored assumption. |
| Auxiliary power kW | Additional running draw that also reaches the condenser. |
| Chilled setpoint °C | No extraction at/below this cold-loop temperature. |
| Maximum condensing temperature °C | No operation if the rejection sink is at/above it. |
| Thermal class | Required class of both interfaces and the cooling service supplied. |
| Critical service | Protects its deliverable electrical work before discretionary loads. |

**Example:** a 3 kW cold duty at COP 3 requires 1 kW compressor work, plus any
auxiliaries; the rejection plant must handle at least 4 kW. Pair the condenser
Reservoir with a Radiator rather than assuming the Chiller deletes heat.

### Thermal buffer

**What it does:** stores a finite amount of heat during its host Reservoir's
cooling deficit and releases it when real rejection surplus returns. Its
stored energy is separate from the host's thermal energy.

**Ports:** **Host reservoir** joins **Hosted buffers**. **Circulation gate** is
required in circulation mode and must name a loop on that same Reservoir;
passive mode needs no circulation gate.

| Dial/value | Purpose |
| --- | --- |
| Access mode | `passive` heat contact or `circulation` powered-loop access. |
| Capacity kJ | Total storable heat across the usable temperature band. |
| Starting energy kJ | Initial fill; determines remaining absorption headroom. |
| Minimum/maximum usable temperature °C | Band across which stored fraction maps to buffer temperature. A narrow band approximates phase-change storage. |
| Maximum charge/discharge kW | Separate rate limits for absorption/recovery. |
| Transfer conductance kW/K | Passive heat-transfer limit; positive in passive mode. |
| Circulation loop | Access prerequisite in circulation mode, not a coolant feed. |

Heat must move down the temperature gradient. Empty storage alone does not
permit absorption if the buffer is already hotter than its host.

**Example:** a passive thermal store absorbs an equipment plant's 2 kW deficit
while a radiator is unavailable. A 7200 kJ empty buffer provides at most one
hour at that deficit, subject to temperature and charge limits. Afterwards,
recovering storage consumes actual spare radiator capacity; it is not instant.

### Thermal link

**What it does:** conducts heat between two Thermal nodes without moving
mass between Atmospheres, Reservoirs, and Electrolyzers with an authored Thermal body.
Use it for a solid wall or shared structure, including reservoir meshes,
without occupying a compartment's heat-only exchanger slot.

**Ports:** **End A** and **End B** each join an Atmosphere's or Reservoir's
**Thermal links** face, or a bodied Electrolyzer's **Thermal body** face. Endpoints must resolve to distinct nodes with positive
thermal capacity. Duplicate undirected pairs are refused; combine parallel
contacts by summing conductances on one link. Thermal service class is ignored.
Stores, bodyless machines and Exterior vacuum are not Thermal nodes. A body
endpoint uses kind `machineBody` and the Electrolyzer ID. The apparent input faces are
endpoint references, not a heat-flow direction.

| Dial/value | Purpose |
| --- | --- |
| Conductance kW/K | Positive overall heat coupling: instantaneous heat flow is `G × (T_A − T_B)`. Includes area/material/thickness effects in one number. |
| End A / End B | Fix the sign of reported flux, not who may receive heat. |

There is **no separate area, openness, efficiency or throughput**. Temperature
difference drives passive exchange; the finite-capacity solve approaches
thermal equilibrium rather than prescribing a constant heat rate.

**Example:** connect a warm laboratory and cool cabin with a Thermal link for
their partition. Add a separate Atmosphere pathway for the doorway opening.
Closing that pathway stops air-carried heat, but wall conduction continues.
A link's conductance does not automatically follow door openness.

Endpoint kinds are reported in the link snapshot beside signed flow, cumulative
net transfer and Idle/Active status. YAML and binary records retain the historical
`aAtmosphereId` / `bAtmosphereId` names for node IDs and add `aKind` / `bKind`
(`atmosphere`, `reservoir` or `machineBody`); omitted kinds default to atmosphere. Reservoirs
publish conducted heat separately from exchanger heat, and both enter their
energy residual. See [ADR-0032](adr/0032-thermal-links-join-typed-thermal-nodes.md).

### Heat engine

**What it does:** converts some heat from a hot Reservoir into electricity and
rejects the rest into a colder Reservoir. This is the explicit thermal-to-power
component, not a negative Equipment load or a discount on radiator heat.

**Interfaces:** hot Reservoir input, rejection Reservoir output, automatic
net generation to the shared electrical pool. Reservoirs must be distinct;
the rejection Reservoir must match the engine's thermal class, while the hot
Reservoir need not share that class. Outage/Maintenance can target its
installation identity through its Supervision port. Place a Heat engine from the
Thermal group in Flow's Place node menu (or the canvas context menu). Connect a
Reservoir's Heat out to Thermal intake, and Rejection heat to a different
Reservoir's Heat in. Power out connects automatically, like other generators.
Select the engine to edit its reservoir references, operating settings,
mechanical-seizure opt-in, efficiency-drift Wear, and repair-parts plans in the
Flow Inspector. Placement starts unwired; Apply uses the normal validated
configuration path once both thermal interfaces are connected. Wiring, inspector
edits, rename and cascade deletion participate in draft undo/redo.

| Dial/value | Purpose |
| --- | --- |
| Rated thermal input kW | Maximum heat intake, not electrical rating; zero disables. |
| Commanded input kW | Intake ceiling within rating; omission in a loaded document means full rated. |
| Carnot fraction | Fraction `(0,1]` of `1 − T_cold/T_hot` captured; temperatures in that ratio are kelvin. Actual efficiency changes with the gradient. |
| Auxiliary power kW | Parasitic running draw deducted from gross electrical output. |
| Minimum hot temperature °C | Source gate below which the engine idles; must be above the maximum sink-temperature gate. |
| Maximum sink temperature °C | Rejection gate; at/above it the engine trips until the sink recovers. |
| Thermal class | Class of **rejection heat**, not necessarily that of the hot source. |

No gradient, insufficient heat, auxiliaries exceeding gross output, a
constrained generation connection or no demand/battery charge headroom can
limit/stop conversion. Unconverted heat stays in the hot Reservoir.

**Example:** recover part of a Generator's hot waste-heat output in a bottoming
cycle. Send rejection heat to a dedicated cooler Reservoir and Radiator.
Retain a direct hot-side rejection path as needed; when the electrical pool
cannot accept power, the engine is not a guaranteed cooling path.

## Gas

### Atmosphere

**What it does:** owns one compartment's volume, gas masses, water vapour and
thermal state. Pressure/partial pressures follow the ideal-gas model. Sharing
an ID, label or thermal class does not make compartments mix.

**Ports:** **Gas out** serves extraction processes/Airlock cycles; **Oxygen in**
accepts direct Electrolyzer supply. **Biology in/out** bind Crew/Crops/Algae gas,
moisture and heat exchanges. **Hosted pathways** and **Thermal links** host
those two-ended aggregates. Optional **Heat out** names a Reservoir for a
bidirectional heat-only exchanger, not a gas destination.

| Dial/value | Purpose |
| --- | --- |
| Volume m³ | Positive contained gas volume; mass and temperature determine pressure. |
| Starting O2 / N2 / CO2 / water vapour kg | Independent initial inventories, not composition percentages. Supersaturated vapour condenses with accounted latent heat. |
| Starting temperature °C | Initial compartment temperature, above absolute zero. |
| Thermal capacity kJ/K | Effective air-plus-structure capacity; controls temperature response. |
| Advected specific heat kJ/kg·K | Heat carried per kg of departing pathway gas; positive, separate from thermal capacity. |
| Reservoir / exchanger conductance kW/K | Optional heat-only coupling. Without a Reservoir, no exchange occurs through this interface. |
| Occupants | Optional simple configured load count; not World Agent count. |
| O2 use / CO2 production / moisture per occupant kg/s | Simple occupancy loads, independent of the richer Crew component. |
| Sensible heat per occupant kW | Simple occupancy heat. Do not double-count Crew metabolism with these values. |
| Leak rate kg/s | Constant composition-proportional external gas loss, capped by inventory. Separate from exterior pathways and Airlock cycles. |
| Minimum/maximum pressure kPa | Required positive ordered service limits; report crossings, do not regulate pressure. |
| Minimum O2 / maximum CO2 partial pressure kPa | Optional quality limits; zero means unconfigured. Reporting limits do not add oxygen or scrub CO2. |

**Example:** a cabin receives oxygen from an Electrolyzer, loses CO2 through a
capture unit and vapour through a Water collector. Crew exchanges gas/heat
with it. Couple it to a Reservoir for thermal control and to another
Atmosphere through a pathway only if real mixing should occur.

### Atmosphere pathway

**What it does:** represents an undirected gas opening. Each tracked gas
(O2, N2, CO2, water vapour) moves down its **own partial-pressure gradient**.
Opposing species flows are possible even at equal total pressure. Moving gas
also carries sensible heat (Advected enthalpy); this is not pure conduction.

**Ports:** **End A/B** join Atmosphere **Hosted pathways**. Two distinct
compartments are valid; alternatively exactly one endpoint can be the reserved
`$exterior` vacuum boundary. Exterior is inert and has no draggable port:
set that endpoint through configuration rather than treating it as a tank.
Stores and Reservoirs are not valid gas endpoints.

| Dial/value | Purpose |
| --- | --- |
| Area m² | Positive opening area; no separate throughput ceiling. |
| Open percentage / `openFraction` | Effective openness, `[0,1]` internally; zero shuts exchange. |
| End A / End B | Endpoint references and sign convention, not inlet/outlet direction. |

Greater area/openness increases exchange at the same state; equalization
reduces the driving gradients. Changing openness therefore affects gas-carried
heat, but **does not modify a separate Thermal link**. A closed pathway
transfers neither gas nor pathway-carried heat. There is no transport-loss
or efficiency setting. Openness can also be actuated by simulation-time-stamped
runtime commands; this is not an additional placeable schedule node.

**Example:** use one pathway for an opening between cabin and greenhouse,
and schedule its openness for ventilation. Use another to `$exterior` for a
vent-to-space scenario: lost gas is accounted, and its source can cool as it
vents. This is an exchange model, not full pressure-driven fluid dynamics.

### Airlock cycle

**What it does:** removes a specified mass of gas from a compartment at
scheduled times, splitting it into recovered and externally lost gas.
This is a Station scenario event, not a movement-system Airlock or its doors.

**Ports:** **Atmosphere** joins one compartment's Gas out. No recoverable-gas
Store output is currently exposed: recovered mass is retained/accounted in the
recovery receiver, not automatically usable breathing supply.

| Dial/value | Purpose |
| --- | --- |
| Gas mass kg | Positive total requested removal per occurrence. |
| Recovered fraction | Fraction `[0,1]` of actual removal recovered; remainder is lost. |
| First occurrence / recurrence | Cycle timing, zero recurrence for one-off. |
| Priority | Orders coincident events. |

**Example:** model ten daily EVA depressurizations, each removing a declared
mass with 80% recovery. Compare lost-gas endurance with an otherwise identical
configuration at 95% recovery. Do not also count the same vent as pathway loss.

### Electrolyzer

**What it does:** splits water into stoichiometric O2 and H2 using electricity.
Products retain chemical energy; the remaining electrical dose is process heat.

**Ports:** **Water in** uses a plain or compound mass Store, but an immutable
zero-dissolved-load runtime gate stops splitting dirty feed. **Hydrogen out**
requires a plain mass Store. Configure **exactly one** oxygen destination:
**Oxygen out** to a plain mass Store, or **Compartment oxygen** to an Atmosphere's
Oxygen in. Water and gas roles must use distinct Stores. **Heat out** needs a
same-class Reservoir; **Power in** is automatic.

| Dial/value | Purpose |
| --- | --- |
| Rate kg/s | Rated **water split**, not oxygen production; zero disables. |
| Specific energy kWh/kg water | Electrical dose, at least the chemical energy retained by the products. |
| Thermal class / heat sink | Process-heat interface. |
| Maximum sink temperature °C | Stops operation at/above the gate. |
| Priority | Allocation order. |

**Example:** treat recovered water, feed an Electrolyzer, send oxygen directly
to the cabin and hydrogen to Sabatier. To also stockpile oxygen, use a separate
Electrolyzer configured for an O2 Store; one unit cannot use both oxygen outlets.

#### Optional casing (`thermalBody`, #854)

Author an optional shared settings block on the Electrolyzer:

```yaml
thermalBody:
  heatCapacityKJPerK: 100
  startingTemperatureC: 20
  sinkConductanceKWPerK: 0.5
```

Capacity and sink conductance must be finite and strictly positive; starting
temperature must be finite and above absolute zero. Waste heat charges this
lumped casing rather than entering the Reservoir directly. **Heat out** now
drains the casing through the shared closed-form two-body conduction law,
limited by the wire's throughput and efficiency. Unaccepted heat stays in the
casing; wire loss remains an explicitly booked loss. Sink class/qualification
still apply to this drain leg. The maximum-temperature gate and coarse-step
protection instead read the casing temperature. The casing continues cooling
while production is idle or refused, allowing restart after cooling.

A refused sink leg leaves the casing to warm until protection refuses
production. With a body, the Heat-out sink is optional: Thermal links may cool
the casing into an Atmosphere, Reservoir or another body. Without any heat path,
the casing warms to its protection limit and configuration reports a warning.
With no `thermalBody`, the historical direct
heat-injection path and serialized representation are unchanged. Bodies are
reconstructed from authored starting temperature on load/Reset. The inspector's
**Thermal body** checkbox adds/removes the block in the draft. Capacity, starting
temperature and sink conductance support live-node edits; adding/removing a body
requires Apply. Live scalar edits preserve the current temperature and physical
charge/conduction totals, rebasing the energy reference for the changed capacity
or starting value rather than creating a spurious energy residual. Changing the
starting value live does not reset the casing temperature.

Published casing fields are `hasThermalBody`, `bodyTemperatureC`,
`lastBodyChargeKW`, `lastSinkLegKW`, `bodyChargeKJ`, `sinkLegKJ`, and
`bodyResidualKJ`. Charge plus net conducted heat minus sink outflow equals the
casing's capacity times its temperature change (with an energy-reference
adjustment after live capacity/starting-value edits). Body temperature and both rates are selectable
Electrolyzer observations. Binary World and Station YAML persistence use schema
123 only when a body is present. Station Flow shows a **Thermal body** face only
when that block exists, with live body temperature and signed conducted kW.
The link inspector's endpoint picker offers Atmosphere, Reservoir and Thermal
body entries; canvas wiring validates endpoints and reports refusals. Body
conduction is reported separately from its sink leg.

### CO2 capture

**What it does:** extracts CO2 from an Atmosphere into finite storage, spending
electricity and any configured regeneration consumable. It does not destroy
carbon or implicitly discharge it to space.

**Ports:** **Atmosphere** joins Gas out; **Captured CO2 out** requires a plain
mass Store. Optional **Consumable in** uses a mass Store (plain or compound),
and must be wired if a positive consumable rate is declared. **Power in** is
automatic. There is no authored heat-sink port on this component.

| Dial/value | Purpose |
| --- | --- |
| Maximum rate kg/s | Capture ceiling; zero disables. Actual CO2 and destination headroom also limit operation. |
| Specific energy kWh/kg | Electrical energy per captured kg. |
| Consumable per kg | Native consumable units per captured kg. |
| Priority | Allocation order. |

**Example:** a cabin scrubber fills a CO2 Store that feeds Sabatier. If hydrogen
runs out, captured-gas storage eventually fills and CO2 capture becomes
backpressured even though the scrubber still has nameplate capacity.

## Water

### Water collector

**What it does:** condenses compartment water vapour onto a circulating cold
surface, depositing pure condensate into a raw-water Store. The condensation's
latent heat goes into the coolant loop exactly once.

**Ports:** **Atmosphere** joins Gas out; **Cooling loop** joins Coolant loop
Cold surfaces; **Raw water out** needs a **plain mass Store**. The collector's
thermal class must match the loop's host Reservoir. The diagram includes an
automatic power relationship, but this component has no independent electrical
dose/pump-power dial; its real cold surface depends on the loop/thermal plant.

| Dial/value | Purpose |
| --- | --- |
| Maximum rate kg/s | Condensation ceiling; zero disables. |
| Target relative humidity | Fraction `[0,1]` at which collection stops; `0.5` means 50% RH. |
| Thermal class | Cold-interface service class. |

Collection requires actual circulation, a surface colder than the air, vapour
above the target and destination headroom. It cannot obtain water from coolant.

**Example:** collect Crew moisture using a Chiller-cooled loop, store condensate
as raw water, then send it through Water treatment. Losing pump power blocks
collection even if the raw-water tank is empty and cabin humidity is high.

### Water treatment

**What it does:** processes a water stream into treated output and retained
concentrate, consuming electricity and treatment material. Full electrical
evidence qualifies the treated product; partial power leaves it held.

**Ports:** **Raw water in**, **Treated water out**, **Concentrate out** require
three distinct mass Stores. Plain or compound streams are supported. Any
outlet receiving dry constituents must be compound; full removal permits a
plain treated outlet. **Consumable in** also requires a mass Store.
**Power in** is automatic. The current treatment draws **qualified source
stock above its reserve**, not held stock. Merely feeding a held treated-water
Store back into another treatment does not provide a requalification path.

| Dial/value | Purpose |
| --- | --- |
| Throughput kg/s | Feed-processing ceiling; zero disables. |
| Recovery ratio | Fraction `(0,1]` of input **water constituent** recovered, not of dissolved solids. |
| Removal fractions | Per dry constituent, fraction sent to concentrate; omitted keys mean full removal (`1`). Zero passes all of that load into treated output. |
| Specific energy kWh/kg | Electrical processing dose. Full dose qualifies output, partial dose produces held output, no supplied power prevents operation when power is required. |
| Consumable per kg | Native consumable units spent per kg processed. |
| Priority | Allocation order. |

Treatment does not automatically make water acceptable to every consumer:
qualified output can still contain dissolved load when removal is partial.
Compound concentrate is a qualified product retaining its constituents;
legacy plain concentrate is held. Both consume finite storage. There is no
separate release-test consumable or hazard-class release switch.

**Example:** treat 100 kg/day greywater containing 0.2 kg organics with 85%
water recovery and full organic removal. Ideal outputs are 84.83 kg/day pure
treated water and 15.17 kg/day concentrate. Retain/eject the concentrate;
merely adding a treated-water tank does not provide waste capacity.

## Biology

### Shared biological requirements

Crew, Crops and Algae use a **single Atmosphere reference** for their multiple
gas/moisture/heat faces. Wiring one face binds the exchange bundle; you cannot
send their CO2 to one compartment and their heat to another through those
faces. Their populations are authored aggregates, **not World Agents**.
Biological coefficients are tunable assumptions, not validated physiology,
agronomy or phycology.

Crew water, crop irrigation, algae makeup and microbial process water support
optional **water quality tolerances**: per-dry-constituent and/or total-dissolved
limits in **kg constituent/kg stream**. Omission means no gate. They read the
actual current blend. Above tolerance, operation is scaled to fit the declared
full-demand dissolved-load budget; a zero tolerance blocks nonzero load.
This is throughput/load limiting, not purification or a health simulation.

### Crew

**What it does:** consumes food, water and atmospheric oxygen and returns CO2,
moisture, heat, urine, organic waste and optionally greywater. Missing input or
full return storage scales metabolism and all returns together.

**Ports:** **Food in**, **Water in**, **Urine out**, **Organic waste out** require
four distinct **compound mass Stores**. Optional **Greywater out** also needs a
distinct compound mass Store if enabled. **Oxygen uptake**, **CO2 return**,
**Moisture return**, **Metabolic heat** bind the same Atmosphere. No automatic
Crew electricity input is declared.

| Dial/value | Purpose |
| --- | --- |
| Count | Configured population; zero disables its demands. |
| Activity profile | Scales metabolic rates/returns together. |
| Food/water per crew per second | Rates in the respective Store's native units. |
| O2/CO2/moisture per crew kg/s | Atmospheric gas and vapour rates. |
| Urine/organic waste per crew per second | Native output Store rates. |
| Metabolic heat per crew kW | Sensible heat returned to the compartment. |
| Greywater kg per crew-day | Optional total liquid return, including its dissolved load. |
| Greywater load kg per crew-day | Declared dryOrganics/nutrientNitrogen **within** that total, not extra mass. |
| Water quality tolerances | Optional acceptance/load budget on drinking-water input. |

Coefficients must fund all returned mass, constituents and heat from consumed
resources. Any unreturned mass is explicitly retained body mass, not silently
lost. Configure simple Atmosphere occupancy loads separately or leave them zero
to avoid double-counting this Crew.

**Example:** connect a pantry and potable-water Store to a six-person Crew.
Route greywater to treatment and organic waste/urine to recovery installations;
CO2 capture and water collection serve its Atmosphere. A full urine tank can
limit supplied metabolism even with adequate food and oxygen.

### Crops

**What it does:** maintains dry living biomass, grows under light and resource
supply, respires day and night, transpires water and can harvest food/waste.
Living biomass, growing capacity and stored food are different inventories.

**Ports:** **Water in** accepts plain/compound mass irrigation. **Nitrogen in**,
**Phosphorus in**, **Potassium in**, **Minerals in** each need their own mass
Store; compound fertilizer Stores must be pure in that port's constituent.
All five inputs must be distinct. Optional **Food out/Waste out** become
required when harvesting is enabled; mass destinations must be distinct and,
if compound, match the declared living composition. The six biological faces
(including **Photosynthesis CO2/oxygen**) bind one Atmosphere. **Power in** is
automatic for circulation and lamps.

| Dial/value | Purpose |
| --- | --- |
| Growing area m² / maximum biomass density kg/m² | Product is installed dry-biomass growing capacity. Zero area disables the population. |
| Initial dry biomass kg | Starting living stock, within growing capacity. |
| Composition | Dry N/P/K/mineral fractions and positive chemical energy; moisture and atmospheric nitrogen must be zero. |
| Lamp rated kW / illumination profile | Potential light, constrained by actual electrical supply; darkness produces no photosynthetic growth. |
| Circulation power kW | Essential canopy service drawn before discretionary lighting; unsupplied circulation stops growth. |
| Growth yield kg/kWh light | Dry biomass fixed per delivered lighting energy, bounded by its chemical-energy budget. |
| Respiration per hour | Organic living material burned through respiration, including in darkness. |
| Transpiration kg/kg growth | Irrigation water returned to the Atmosphere as vapour. |
| Minimum/maximum growing temperature °C | Growth window, lower inclusive and upper exclusive; respiration still occurs outside it. |
| Harvest trigger kg | Living mass threshold; zero disables harvest. |
| Harvest fraction | Fraction `(0,1]` of living mass removed per harvest. |
| Edible fraction | Fraction `[0,1]` of harvest sent to food; remainder/surplus goes to waste as headroom permits. |
| Water quality tolerances | Optional irrigation load budget. |
| Selective irrigation / uptake fractions | Nonempty N/P/K map enables selective uptake from dissolved irrigation, capped by growth demand. Unabsorbed salts remain; separate fertilizer inputs fund the deficit. Empty means legacy homogeneous draw. |
| Priority | Resource/service ordering. |

Full destinations constrain harvest rather than deleting living mass. Starved
organic biomass can be respired away; remaining dead residue goes to waste or
is reported as explicit uncontained loss if it cannot be contained.

**Example:** build a hydroponic greenhouse with compound irrigation, separate
fertilizer Stores, artificial lighting and an Atmosphere. Add a purge Transfer
and treated makeup supply so salts left by selective uptake do not accumulate
until water-quality tolerances throttle growth. Harvest into a compatible food
Store, or use a Conversion before mixing a different food composition.

### Algae

**What it does:** a separate photosynthetic biological population, using active
culture volume rather than crop area and splitting harvest three ways.
It does not share living biomass or growing capacity with Crops.

**Ports:** same water/four separate nutrient inputs, atmospheric exchange bundle
and automatic electrical input as Crops. Optional **Food out**, **Recovery out**,
**Waste out** must all be configured for harvest, with distinct mass Stores and
composition-compatible compound destinations.

| Dial/value | Purpose |
| --- | --- |
| Culture volume L / maximum biomass density kg/L | Product is installed dry-biomass capacity. Volume is a capacity parameter, not a transferable liquid inventory. Zero volume disables. |
| Initial biomass / composition | Starting dry living stock and dry constituent/energy makeup, with zero moisture/atmospheric nitrogen. |
| Lamp rating / illumination profile / circulation power | Lighting potential, duty cycle and essential circulation, as for Crops. |
| Growth yield / respiration / growing temperature bounds | Light conversion, day/night organic consumption and temperature eligibility. |
| Evaporation kg/kg growth | Makeup water lost to the compartment as vapour during growth. |
| Harvest trigger / harvest fraction | When to harvest and how much living mass to remove; zero trigger disables. |
| Food fraction / recovery fraction | Shares of harvested dry mass; sum at most one, with remainder to waste. Food overflow tries recovery, then waste; unplaceable material stays living. |
| Water quality tolerances | Optional makeup-water load budget. No crop-style selective irrigation map is declared for Algae. |
| Priority | Resource/service ordering. |

**Example:** grow Algae on cabin CO2, send edible harvest to food and recovery
harvest to a composition-compatible feed Store for a Microbial processor.
Retain inedible residue separately. The processor's nutrients are recovery,
not a replacement for the light and water needed to grow new biomass.

### Microbial processor

**What it does:** an aerobic waste digester/nutrient-recovery stage with finite
feed throughput and aggregate culture condition. It recovers bound nutrients,
water and CO2, retains residue and releases funded heat.

**Ports:** **Feed in** requires a compound mass Store; **Oxygen in** a plain mass
Store; **Water in** a plain/compound mass Store. **Culture medium** must resolve
to a Store in a complete configuration (despite its optional Flow appearance);
in CompositionDerived mode it must be mass-denominated. Optional **Replacement
stock** may use a Store's native unit. **Nutrients out** and **Residue out**
require plain mass Stores; **CO2 out** a plain mass Store; **Water out** permits
compound mass, required for positive dissolved carry-over. All stream roles
must use distinct Stores. **Heat out** joins a same-class Reservoir;
**Power in** is automatic.

| Dial/value | Purpose |
| --- | --- |
| Rated feed processing kg/s | Wet feed ceiling, further scaled by culture condition/resources; zero disables. |
| Rate basis | FixedPerFeedMass coefficients or CompositionDerived rates from the actual feed blend. |
| Specific energy kWh/kg feed | Electrical processing dose in either mode. |
| Fixed-mode oxygen / process water / CO2 per kg feed | kg input/output per wet kg feed. |
| Fixed-mode medium per kg feed | Native consumable units per wet kg feed. |
| Fixed-mode reaction heat kJ/kg feed | Funded heat release, not exceeding available feed energy. |
| Organic oxidation fraction | Derived mode: share of feed organics processed. |
| O2 / CO2 per kg organic oxidized | Derived oxygen demand and CO2 yield coefficients. |
| Nitrification fraction / O2 per kg nitrified N | Derived extra oxygen demand associated with recovered nutrient nitrogen. Does not turn atmospheric nitrogen into fertilizer. |
| Process-water target moisture | Derived dilution target, below one; requests water only as needed to reach it. |
| Heat release fraction | Derived fraction of oxidized organic chemical energy released. |
| Medium kg/kg organic oxidized | Derived culture-medium demand, mass-based rather than arbitrary native units. |
| N/P/K/minerals recovery fractions | Shares `[0,1]` of those bound feed constituents recovered; not interchangeable yields. |
| Water recovery | Fraction of feed moisture recovered. |
| Recovered-water carry-over fractions | Route a share of each unrecovered dry constituent into water instead of residue. Positive entries need compound water storage. |
| Minimum operating / maximum sink temperature °C | Culture runs inside this sink-interface window. |
| Initial condition | Culture health/capacity factor `[0,1]` at start. |
| Starvation / medium exhaustion / cold stress / heat stress degradation per hour | Separate adverse-condition losses of condition points per hour. |
| Recovery per hour | Maximum condition-point restoration under favourable conditions. |
| Stock units per condition point | Replacement stock spent during recovery when a stock Store is configured. |
| Water quality tolerances | Optional process-water load budget. |
| Priority | Processing allocation order. |

Residue is the mass-balance remainder, not an independent yield knob. Outputs
cannot exceed funded mass/constituents/energy. Atmospheric nitrogen in feed is
not a nutrient resource. Condition coefficients are provisional biological
assumptions, not a simulation of individual microbes.

**Example:** digest Crew organic waste with oxygen, process water, medium and
power. Recover mixed nutrients and water, capture CO2 for Sabatier and send
residue to Waste ejection storage. Mixed recovered nutrients cannot simply be
wired into all four pure fertilizer ports: model the necessary constituent-
balanced separation/processing and compatible storage explicitly.

## Schedules

### Delivery

**What it does:** imports a resource from outside the station at scheduled
times. Unlike Transfer, it does not consume another station Store.

**Ports:** **Destination** joins one Store's Inventory in; delivery unit must
exactly match it. No external source node/port needs to be wired.

| Dial/value | Purpose |
| --- | --- |
| Unit / quantity | Positive native-unit amount per occurrence. |
| Arrives qualified | True deposits usable stock; false deposits held stock. |
| First occurrence / recurrence | Arrival schedule. |
| Priority | Order among coincident events. |

Only available capacity is accepted; the rest is explicitly rejected at the
external boundary, not silently added later or overflowed.

**Example:** a cargo flight delivers 200 kg to a food Store every month. With
only 50 kg headroom on arrival, only 50 kg is accepted. Schedule pantry
Transfers after delivery if they should use the newly arrived inventory.

### Waste ejection

**What it does:** removes eligible Store inventory across the external boundary
at scheduled times, accounting it as loss. There is no waste recovery output.

**Ports:** **Source** joins Store Inventory out; exact native unit match.
The eligibility selector, not a hazard or waste label, determines which
partition may be removed.

| Dial/value | Purpose |
| --- | --- |
| Unit / quantity | Requested positive native-unit amount per occurrence. |
| Eligibility | QualifiedOnly (default), HeldOnly, or QualifiedAndHeld (qualified first, then held). Held stock is never made usable by disposal. |
| First occurrence / recurrence | Disposal schedule. |
| Priority | Coincident-event order. |

Insufficient eligible material produces a smaller/zero ejection, not a software
failure. Removing held contents requires an explicit held-eligible setting.

**Example:** eject held plain treatment concentrate weekly using HeldOnly;
eject qualified compound concentrate using QualifiedOnly. A default qualified
selector will not clear a tank whose contents are all held.

### Outage

**What it does:** temporarily reduces the rated availability of a registered
installation. It is a capacity constraint, not destruction or inventory reset.

**Ports:** **Target installation** names a Generator, Coolant loop, Chiller or
Heat engine. All four have editable Flow Supervision ports.

| Dial/value | Purpose |
| --- | --- |
| Capacity fraction | Fraction `[0,1)` of rated capacity **remaining**, not fraction lost. Zero is a full outage. |
| First occurrence / duration / recurrence | Window start, positive window length and repetition. |
| Priority | Deterministic event announcement/order. |

Overlapping reductions take the minimum fraction. Restoration does not refill
fuel, coolant, batteries, buffers or biomass.

**Example:** reduce a Chiller to 50% for ten minutes and watch its Thermal
buffer absorb the deficit. Setting the fraction to 0.5 leaves half capacity;
it is not a command to halve the sink temperature.

### Maintenance

**What it does:** reserves tools/parts, isolates a registered installation,
runs timed work and books consumed supplies into waste on completion.
Absent prerequisites delay/block work rather than allowing free maintenance.

**Ports:** **Target installation** has the same registered targets as Outage.
One to eight **Requirement** inputs join distinct Stores. **Waste out** is
required if any requirement is consumed; its Store must match consumed input
units and compound composition and must not alias those sources. Reusable
tools return to their original Stores; no extra return port needs authoring.

| Dial/value | Purpose |
| --- | --- |
| First occurrence / duration / recurrence | Start opportunity and required continuous work time; recurrence cannot overlap its own window. |
| Requirement quantity | Positive native units reserved for the job. |
| Requirement consumed | False reserves/returns a tool unchanged; true spends it at completion and deposits it as used material/waste. |
| Requirement count | One to eight prerequisites. |
| Waste Store | Finite headroom for the total consumed requirements. |
| Priority | Orders competing reservations. |

Active work holds target availability at zero. If reservations/headroom can no
longer be maintained, work is interrupted, released and must restart for its
full duration. Completion restores availability, not consumables or fuel.

**Example:** service a coolant pump using reusable tools plus a spare filter.
The loop stops while work is active; the filter moves into compatible waste
storage at completion and tools return. Provide another cooling path or enough
thermal storage to cover the isolation interval.

## Derived nodes, prefabs and station-wide controls

### Power (shared electrical pool)

**Purpose/interfaces:** derived aggregate connecting all automatic electrical
supply/demand/charge/discharge relationships. It owns no authored inventory or
manual ports and is not placeable. There is no independent bus-capacity dial.

**Controls/example:** station dispatch mode is Manual (Generator/Fuel cell
commands) or Automatic (commands generators first, then fuel-cell backup to
cover demand remaining after solar, subject to real availability). Batteries
bridge remaining deficits within their limits. Use Solar + Battery + Equipment
to model an eclipse power budget; drawing a bus edge does not grant power.

### Cooling service

**Purpose/interfaces:** derived per-thermal-class service supplied by Chillers
and requested by CoolingService Equipment. Not a material Store, a cold
Reservoir, or a manually assignable chiller-to-load pipe.

**Controls/example:** no independent dials; Chiller class/capacity and Equipment
class/heat demand determine it. A `low` laboratory load cannot borrow spare
`warm` service merely because the diagram has both nodes.

### Exterior

**Purpose/interfaces:** derived vacuum boundary for an Atmosphere pathway with
one `$exterior` endpoint. It has zero species partial pressures and no owned
volume, mass or temperature. It is neither editable nor draggable as a peer.

**Controls/example:** no boundary dials; author pathway area/openness. A vent
pathway loses gas to Exterior. A Radiator instead exchanges with its own
configured radiative environment; a Thermal link cannot conduct to Exterior.

### Prefabs

A prefab packages ordinary components and internal wiring with exposed ports
and dials; it does not add a new physical process or bypass any of the rules
above. For example, package a Reservoir, Coolant loop and Radiator as a reusable
cooling assembly. Check the constituent components when sizing it, and wire
only its deliberately exposed interfaces.

### Station settings and monitoring

| Setting/value | Purpose |
| --- | --- |
| Enabled | Opts this World into Station systems execution. |
| Systems step World ticks | Station integration cadence, 1–60 World ticks; default 6 against the 60 Hz World clock (10 Hz). Not a device throughput knob. |
| Observation interval seconds | Reporting/sample cadence, 0.1–60 s, not physics speed. |
| Trend retention seconds | Retained simulation-time history window, 1–86400 s, with bounded storage. |
| Dispatch mode | Manual or Automatic source dispatch as described above. |

**Example:** retain one hour of one-second observations to compare battery
endurance and thermal excursions across eclipse runs. Charts, forecasts,
limiters and event logs observe the system; none supplies missing resources.

## Worked system arrangements

### Cabin air and water recovery

1. Crew consumes compound food/water Stores and exchanges gas/heat with a cabin
   Atmosphere. Set the Atmosphere's simple occupant loads to zero if Crew
   already represents those occupants.
2. An Electrolyzer draws clean water, supplies cabin oxygen and stores hydrogen.
3. CO2 capture removes cabin CO2 into a finite Store; Sabatier consumes it and H2,
   returning raw water and methane. Methane processing can recover some H2.
4. A Water collector uses a real cold Coolant loop to recover cabin vapour.
   A Chiller and Reservoir/Radiator plant sustain that cold surface.
5. Water treatment draws raw/greywater, pays its electrical/consumable dose,
   returns treated water and retains concentrate. Use compatible compound
   storage where the downstream Crew requires it.
6. Supply power from Solar/Battery/Generator and provide finite waste disposal.
   Closing the mass loops does not close the energy budget for free.

### A doorway between warm and cold compartments

- Use **two Atmospheres** with their own thermal capacities and inventories.
- Add a **Thermal link** for the solid partition's conduction.
- Add an **Atmosphere pathway** for the opening, with its actual area/openness.
- Closing the pathway eliminates its gas/advected-heat exchange, but not the
  separately authored wall conduction. Changing either interface is an explicit
  model choice; no movement-system Door implicitly rewires Station systems.
- Add Reservoir heat exchangers and Radiators/Chillers if either compartment
  needs sustained thermal control rather than eventual equalization.

### Workshop with backup and waste-heat recovery

- Solar and a Battery support an Equipment workshop with material input and
  return Stores. The workshop's full return Store genuinely stops production.
- A fuelled Generator covers longer deficits but needs startup energy and a
  cooling-qualified Reservoir/Radiator path.
- A Heat engine can recover some hot-reservoir energy into the electrical pool,
  with a separate rejection Reservoir/Radiator. Size for rejection heat too.
- Outage/Maintenance scenarios test the pump/chiller/generator installations;
  Thermal buffers and battery energy only bridge finite intervals.

## Editor help resource

The editor reads the structured JSON extraction at
[`resources/station-systems/component-help.json`](../resources/station-systems/component-help.json)
through the application resource manifest. Hover a Station Flow node to see
its component guide beneath the existing node information. Headings, paragraphs,
lists and dial descriptions render as native UI text rather than raw Markdown.
Hold **Shift** and use the mouse wheel over the node to scroll long help without
zooming the canvas. Expanded prefab internals show their own component help;
collapsed units show the generic Prefab guide.

After editing this reference, regenerate and verify the resource:

```sh
python3 scripts/extract_station_component_help.py
python3 scripts/extract_station_component_help.py --check
```

## Implementation sources

This reference is grounded in the current authored settings, validation and
projection rather than in historical ticket-specific defaults:

- [`StationSystems.h`](../include/core/StationSystems.h): settings and units.
- [`StationPorts.h`](../include/core/StationPorts.h): placeable types, authored
  port names/cardinalities and reference bundles.
- [`StationSystemsValidation.cpp`](../src/core/StationSystemsValidation.cpp):
  full semantic wiring, composition, unit and value constraints.
- [`StationConnections.cpp`](../src/core/StationConnections.cpp): projected
  automatic and physical/non-physical relationships.
- [`StationSystemsEntryEditor.cpp`](../src/StationSystemsEntryEditor.cpp): current
  user-facing entry controls.
- [ADR-0023](adr/0023-separate-station-connection-design-from-flow-allocation.md):
  connection throughput, efficiency and allocation.
- [ADR-0024](adr/0024-containment-is-modelled-not-declared.md): physical
  containment and water requirements, not hazard-label prohibition.
- [ADR-0026](adr/0026-atmosphere-diffusion-is-per-species-closed-form-and-thermally-neutral.md)
  and [ADR-0027](adr/0027-atmosphere-pathways-are-a-dedicated-undirected-aggregate.md):
  gas diffusion, the advected-enthalpy amendment and pathway endpoints.
- [ADR-0028](adr/0028-thermal-links-are-a-dedicated-undirected-conduction-aggregate.md):
  independent mass-free conduction.
- [ADR-0029](adr/0029-waste-heat-becomes-power-only-through-a-carnot-bounded-machine-with-its-own-rejection-path.md):
  Heat-engine conversion and rejection requirements.
