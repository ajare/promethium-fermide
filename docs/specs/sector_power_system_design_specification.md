# Modular Space Station Power System — Design Specification

Version 1.0 — 3 October 2026

## 1. Purpose and scope

Provide electrical power to a user-defined collection of station sectors. Each sector declares its demand, permitted electrical connections, critical loads, and continuity requirements. The design process derives the generation, storage, conversion, cabling, protection, control, cooling, and consumable requirements needed to meet those declarations.

The system treats sector equipment as electrical loads. It does not prescribe how that equipment works. Life support and mission-critical equipment are examples of loads that a user can designate as continuously powered.

This is a conceptual engineering specification suitable for station design, simulation, and further engineering development. Numerical defaults are plausible planning assumptions, not flight-qualified ratings. Megawatt spacecraft generation, particularly nuclear generation, requires technology-specific development; this document does not imply that the example modules are available commercial spacecraft products.

Power shall be guaranteed within an explicitly declared failure envelope and autonomy duration. No finite plant can guarantee indefinite service following every possible failure or exhaustion of all energy sources.

## 2. Definitions and measurement boundaries

| Term | Definition |
|---|---|
| Sector demand | Electrical power delivered at the sector's equipment terminals |
| Normal demand | Expected simultaneous operating demand |
| Peak demand | Maximum admitted simultaneous demand, with a stated duration |
| Critical demand | Loads required to remain powered under the selected failure envelope |
| Survival demand | Optional smaller critical subset approved by the user |
| Gross generation | Generator output before its own auxiliaries |
| Net generation | Output available at the generator connection after auxiliaries |
| Firm generation | Net capacity available in the worst specified generation scenario |
| Nameplate energy | Battery energy when new at its rated conditions |
| Deliverable energy | Energy available at the stated output boundary after age, temperature, state-of-charge limits, and conversion losses |
| N+1 | Enough capacity after any one relevant module is unavailable |
| Failure domain | Components that can fail together because of location, shared cooling, control, or another dependency |

Use kW or MW for power; kWh or MWh for energy; V for voltage; A for current; kg for mass; m² for area; m³ for volume; and seconds or hours for duration. Avoid describing battery autonomy using power alone.

## 3. User-supplied configuration

### 3.1 Minimum input

The user shall supply:

1. Sector identifiers and electrical power requirement per sector.
2. Allowed sector-to-sector and plant-to-sector power connections.
3. Equipment or load groups that must remain powered, and their power demand.

If only a single power value is supplied for a sector, treat it as continuous demand and simultaneous peak demand. Do not assume that the entire sector is critical unless explicitly marked. If marked critical without a separate critical power value, treat its full specified demand as critical.

Apply the defaults below when information is missing, and list all such assumptions in the resulting design report. An unspecified connection rating may be sized by the design process; an explicitly prohibited connection shall not be added silently.

### 3.2 Sector record

| Field | Unit/type | Requirement or default |
|---|---|---|
| `id`, `name` | text | Unique identifier and descriptive name |
| `normal_kw` | kW | Required |
| `peak_kw`, `peak_duration_s` | kW, s | Defaults to normal demand continuously |
| `critical_kw` | kW | Sum of explicitly protected groups |
| `survival_kw` | kW | Optional; defaults to critical demand |
| `load_groups` | records | Required where priorities differ within a sector |
| `daily_profile` | kW versus time | Optional; otherwise constant demand |
| `startup_kw`, `startup_duration_s` | kW, s | Optional; otherwise no extra surge assumed |
| `output_voltages` | V DC or V AC/Hz | Default 400 V DC; converters for other equipment |
| `max_interruption_ms` | ms | Required for continuity claims; critical default 0 ms at equipment terminals |
| `local_autonomy_h` | h | Critical default 0.5 h |
| `allowed_shed` | boolean | False for critical load groups |
| `minimum_operating_kw` | kW | Optional controllable-load floor |
| `restore_delay_s` | s | Default 10 s; equipment-specific where necessary |
| `physical_domain` | text | Location/fire/impact compartment |
| `dependencies` | identifiers | Cooling, controllers, actuators, or other necessary support loads |

Critical dependencies inherit critical status. A protected machine whose cooling or controller can be shed is not actually protected.

### 3.3 Connection record

Describe the power network as a graph. Nodes include sectors, switchboards, generators, batteries, converters, and docking interfaces; edges are permitted electrical links.

Each edge shall declare endpoints, permitted flow direction, nominal voltage, route length, physical route/domain, and any fixed current or power limit. It shall also declare whether it is normally open, whether it supports independent isolation at both ends, and which other edges share a conduit or compartment.

An undirected drawing does not automatically permit reverse flow through a unidirectional converter. Converter efficiency and voltage changes shall be represented explicitly.

### 3.4 Reliability and operating defaults

| Parameter | Default |
|---|---:|
| Growth allowance on sector demand | 20% |
| Failure envelope | Any one generator, battery bank, converter, cable segment, or bus section unavailable |
| Full planned peak maintained after one generator loss | Yes |
| Critical service after one distribution failure | Yes |
| Central critical autonomy | 8 h after loss of all primary generation and one central battery bank |
| Local critical autonomy | 30 min after loss of sector feeds |
| Battery recharge target | 8 h from completion of the design emergency discharge |
| Main distribution efficiency for first sizing | 95% |
| Continuous generator module size | 2 MW net, selectable |
| Available module derating | 10% of net nameplate |
| Battery end-of-life capacity factor | 80% |
| Battery operating state-of-charge window | 20–90%, giving 70% usable range |
| Battery output-path efficiency | 95% to main bus |
| Battery recharge-path efficiency | 95% from main bus |
| Minimum routine critical energy reserve | Full selected autonomy |

These are starting values. Location, radiation environment, solar eclipses, resupply intervals, repair times, and mission risk can require different values. N+1 does not establish tolerance to two unrelated failures or an entire failure-domain loss; those require additional declared scenarios.

## 4. Required architecture

Use modular sources and storage feeding sectionalized distribution networks A and B. Keep their physical routes, protection, controls, and supporting cooling independent wherever a continuity claim depends on independence.

Critical sectors shall have two independently protected input paths. Each path shall support the complete critical demand of that sector, including growth and upstream losses, after the other path is isolated. Full-sector redundancy is optional and shall be distinguished from critical-load redundancy.

Within a critical sector, use separate converters and critical panels A and B. A single shared transfer switch or converter shall not become the only path to every critical load. Dual-input equipment can accept both panels; single-input equipment requires a fault-tolerant local power arrangement and downstream hold-up storage.

The default A/B bus tie is normally open. A protected tie may close only after checking voltage, polarity, insulation condition, available capacity, and fault isolation. Tie closure shall not turn an isolated short into a fault on the healthy network.

Ordinary sectors may be single-fed where the user accepts interruption. Sector connectivity, rather than a mandatory ring, determines routing. A ring is useful only when its surviving segments are rated to carry the redistributed demand.

## 5. Demand aggregation and automatic sizing

### 5.1 Demand calculation

For simultaneous scenario demand `P_sector` and growth factor `g`:

`P_delivered = (1 + g) × sum(P_sector)`

With distribution efficiency `eta_dist` and central plant auxiliaries `P_aux`:

`P_bus = P_delivered / eta_dist + P_aux`

Generator-local auxiliaries are excluded from `P_aux` when module capacity is already specified net. Central controls, common pumps, heaters, storage standby loads, and other power-system loads are included. Do not double-count equipment already in the sector list.

Unless scheduling is enforceable, sum all sector peaks. Time profiles may reduce coincident peak only when the controller can prevent conflicting operations. Evaluate startup surges separately from sustained peaks.

### 5.2 Self-consumption iteration

Start with `P_aux = 2% of P_delivered + 10 kW` for a megawatt-scale reference plant. Recalculate auxiliaries from the selected hardware and coolant flows; update demand and repeat until capacity choices stop changing and auxiliary demand changes by less than 1%. Small plants need component-level auxiliary estimates because fixed control loads can dominate.

The iteration shall include additional battery charging and cooling auxiliaries when those operate simultaneously with peak sector demand.

### 5.3 Generation sizing

For `n` identical modules, net nameplate `P_module`, derating `d`, and tolerated generator outages `k`:

`P_firm = (n − k) × P_module × (1 − d)`

Choose the smallest integer `n` satisfying the worst required operating case. The default sustained design case is:

`P_firm ≥ P_peak_bus + P_recharge_bus`

If recharge during peak demand is not required, evaluate it as a separate scheduled scenario and specify the resulting restrictions. Batteries shall not be used to conceal a sustained generation deficit.

Check minimum stable output, ramp rate, startup duration, scheduled maintenance, and largest credible source trip. Maintain enough online sources to serve demand; keep additional sources ready according to their actual startup capability. A hot nuclear standby module can still require substantial heat rejection even while producing little electricity.

### 5.4 Network feasibility

For every required scenario:

1. Remove failed components and their dependent equipment.
2. Retain only permitted routes and valid converter directions.
3. Require power balance at each node, including conversion and conductor losses.
4. Enforce cable current, converter power, source power, and storage energy limits.
5. Serve all protected loads before admitting shedable loads.
6. Verify voltages and thermal limits with an electrical network calculation.

A capacity-constrained graph-flow calculation is useful for screening. It is not a substitute for DC load-flow, protection coordination, or transient analysis, particularly with parallel routes and regulated constant-power loads.

If the graph cannot supply a critical sector, report the disconnected or undersized cut, the affected demand, and the minimum permitted remedy: increased edge rating, additional independent route, local generation, or specified local storage. Do not claim unlimited continuity from a finite battery.

## 6. Generation interfaces and source options

Each source module shall declare net continuous output, transient output, startup and ramp limits, efficiency, auxiliary consumption, resource use, thermal rejection, and fault-domain dependencies. It shall have independent output isolation, local protection, monitoring, and shutdown control.

### 6.1 Continuous thermal generators

For planning, use 40% gross electrical conversion efficiency where the selected technology supports it; use the actual net efficiency in the final resource balance. For an example module with 2 MW net output and 40% net efficiency:

- Input thermal power: 5 MW.
- Generator waste heat: 3 MW.
- Electrical output: 2 MW net.

Each module needs its own shutdown heat-removal provision. Nuclear decay heat cannot be switched off by opening an electrical breaker. Startup and restart availability shall come from the selected technology rather than an assumed generic seconds-long start.

### 6.2 Solar sources

At roughly 1 AU, use an illustrative end-of-life regulated output density of 250 W/m² of active array area in suitable illumination and orientation. A 1 MW output therefore requires about 4,000 m² before adding geometric margins and redundancy.

Scale incident sunlight approximately with inverse square of distance from the Sun, and apply pointing, temperature, degradation, shadowing, and conversion factors. Split arrays into isolated strings and independently controlled wings.

For a solar-only plant, size both energy and power. For illumination time `t_light`, eclipse time `t_dark`, sunlit demand `P_light`, eclipse demand `P_dark`, and storage round-trip efficiency `eta_rt`:

`P_array_sunlit ≥ P_light + P_dark × t_dark / (t_light × eta_rt)`

Add growth, required array outage tolerance, recharge obligations, and seasonal worst cases. Storage must separately supply the complete eclipse demand and required emergency reserve. Peak sunlight output is not firm continuous generation.

### 6.3 Diverse emergency generation

Provide a separately controlled emergency source if required by the reliability profile. Default net rating:

`P_emergency ≥ 1.2 × P_critical_bus`

Its cooling, startup energy, resources, and route to critical loads shall survive the primary failure it is intended to cover. If the emergency source itself must meet N+1, divide it into independently supported modules and size the surviving set accordingly.

Example: a 500 kW hydrogen/oxygen fuel-cell source at 50% efficiency on hydrogen lower heating value consumes approximately 30 kg H₂/h and 240 kg O₂/h, and produces about 270 kg water/h. A 24 h run requires approximately 720 kg H₂ and 5,760 kg O₂ before reserve margins. Reactant tank volume depends on storage pressure, temperature, phase, and vessel design. Include those inventories and disposal/storage of product water explicitly; the fuel cell is not an unlimited backup.

## 7. Central energy storage

### 7.1 Capacity calculation

For protected demand at main-bus boundary `P_critical_bus`, autonomy `t`, battery end-of-life factor `f_age`, usable state-of-charge fraction `f_soc`, output efficiency `eta_out`, and `n_b` equal banks with `k_b` unavailable:

`E_installed ≥ P_critical_bus × t / (f_age × f_soc × eta_out) × n_b / (n_b − k_b)`

Use kW and hours to obtain kWh. Temperature derating, battery heaters, standby loads, and self-discharge shall be added where material. Avoid double-counting output efficiency if a manufacturer supplies deliverable energy at the chosen boundary.

### 7.2 Power rating

Surviving storage converters shall meet the larger of protected demand and the temporary source-trip deficit after any immediate load shedding. Energy capacity alone does not establish that a bank can provide the required current.

Default converter arrangement: four independent banks, each with its own bidirectional converter, contactors, battery management, cooling branch, isolation monitoring, and fire compartment. Check remaining power and energy with any bank unavailable.

### 7.3 Recharge and reserve

Restore the internal energy removed during the emergency:

`P_recharge_bus = E_removed_internal / (t_recharge × eta_charge)`

Routine arbitrage, eclipse cycling, or peak shaving shall not consume the protected emergency reserve. If both routine cycling and emergency operation must occur consecutively, their energies are additive in the worst-case duty cycle.

Planning pack-level energy density: approximately 100–160 Wh/kg before installation-specific shielding and structural additions. A 10 MWh installation is therefore roughly 63–100 tonnes of battery packs; converters, mounts, shielding, and cooling add mass. This assumption shall be replaced by a selected battery design.

### 7.4 Fast storage

Fast buffer energy is calculated from the transient deficit:

`E_fast = integral(P_deficit(t) dt) / eta_fast`

An illustrative 20 MJ deliverable buffer at 5 MW covers four seconds at full output. Divide it into independent units if loss of one is included in the transient requirement. Batteries may supply the fast response directly where their converters and control loops demonstrate sufficient response; supercapacitors or flywheels are optional.

## 8. Local continuity and black start

Critical equipment shall see no supply interruption beyond its declared tolerance. For the default zero-interruption class, maintain an energized downstream DC bus using online converters and local storage. Break-before-make source switching alone does not satisfy this class.

Local storage shall support the protected sector load for its declared autonomy, after age, state-of-charge, efficiency, and local failure allowances. Include converter loss, controls, battery cooling, and any necessary protected-load startup.

Example: 100 kW for 0.5 h requires 50 kWh delivered. At 80% end-of-life capacity, 70% usable window, and 95% output efficiency, it requires about 94 kWh nameplate. Round to 100 kWh for a single-bank arrangement. If one local bank must be lost without reducing autonomy, two independent 100 kWh banks are needed. The surviving local converter must support the full 100 kW.

Provide an isolated black-start energy store dedicated to controls, switching, source auxiliaries, and initial cooling. Size it from the actual startup profile:

`E_black_start_delivered = sum(P_stage × duration_stage) + failed_attempt_allowance`

For a startup needing 80 kW for 30 min followed by 200 kW for 15 min, one attempt requires 90 kWh. Two attempts require 180 kWh delivered. Applying the battery factors above requires about 338 kWh nameplate; select at least 350 kWh. A slower source may require much more.

Black-start sequence: confirm fault isolation; energize local controls and necessary cooling; start an independently restartable source; energize a healthy bus section; restore critical feeds; bring additional sources online; replenish protected reserves; restore ordinary loads in stages. The startup store shall never supply discretionary loads.

## 9. Distribution, converters, and conductors

### 9.1 Voltage selection

| Network | Illustrative voltage | Application |
|---|---:|---|
| Main transmission | 2,000 V DC conductor-to-conductor | Megawatt plant and long routes |
| Sector machinery | 400 V DC | Motors, industrial loads, local conversion |
| General equipment | 120 V DC or isolated AC inverter output | Equipment requiring lower voltage |
| Controls | 48 V DC | Electronics and local UPS |
| Device-specific output | 24/28 V DC or declared AC | Equipment interfaces |

A ±1 kV arrangement is one implementation of a 2 kV differential bus; grounding and chassis reference require a separate insulation and fault analysis. Do not use station structure as a normal current return. High-voltage exposure to plasma, vacuum, contamination, and pressure transitions requires environment-specific insulation and arc control.

### 9.2 Electrical sizing

At each edge:

`I = P / V`

`DeltaV = I × R_loop`

`P_cable_loss = I² × R_loop`

For two equal copper conductors:

`R_loop = 2 × rho × L / A`

Use `rho ≈ 1.72 × 10^-8 ohm·m` at 20°C; correct for operating temperature. Set an initial steady-state feeder voltage-drop target of 3% maximum and total route drop of 5% maximum. Cable thermal limits can require a larger cross-section than the voltage-drop calculation.

Example: 1 MW at 2 kV is 500 A. A 100 m one-way route with two 240 mm² copper conductors has approximately 0.0143 ohm loop resistance at 20°C, 7.2 V drop, and 3.6 kW loss. Copper conductor mass is approximately 430 kg, excluding insulation, shielding, terminations, and supports. This is an electrical-loss example, not a 500 A vacuum ampacity certification.

### 9.3 Converter sizing

Each redundant sector converter shall support its required surviving load at the minimum allowed input voltage and end-of-life thermal condition. Default efficiency for planning is 97% per isolated conversion stage; use actual efficiency curves in final calculations.

Do not apply both individual converter losses and the initial 95% aggregate distribution efficiency to the same path. Replace the aggregate approximation once component-level routing is available.

Regulated constant-power loads can destabilize weak buses. Validate converter input impedance, current limiting, droop characteristics, bus capacitance, and control-loop interactions. Parallel converters require coordinated current sharing and fault isolation.

## 10. Protection and failsafes

Provide current sensing, voltage sensing, insulation monitoring, temperature monitoring, and independent trip logic at source, bus, converter, feeder, and major load boundaries.

| Fault | Required response |
|---|---|
| Feeder short or sustained overload | Isolate the affected feeder; preserve upstream and alternate healthy paths |
| Bus-section fault | Open boundary protection and block ties into the faulted section |
| Converter output fault | Isolate both converter input and output as needed to stop backfeed |
| Ground/insulation fault | Locate and isolate according to the grounding scheme; prevent progression to a second fault |
| Generator trip | Storage supplies deficit; shed admitted ordinary loads if necessary; dispatch remaining sources |
| Battery overtemperature or runaway indicators | Stop charge/discharge, isolate affected module, contain propagation and gases |
| Cooling loss | Derate supported equipment; transfer loads before thermal shutdown where possible |
| Control-network loss | Local protection remains active; use predetermined safe droop and reserve rules |
| Docking-interface fault | Current-limit and isolate docking port without collapsing station buses |

Use DC-rated solid-state protection for fast interruption and mechanical devices for rated isolation. Mechanical opening shall occur within its breaking capability, normally after the fast device has reduced current. DC arcs do not benefit from an AC current zero.

Illustrative protection targets are 10–100 microseconds for local semiconductor overcurrent action and less than 1 ms for a solid-state feeder breaker; mechanical isolation may take tens of milliseconds. These are design targets to verify, not universal achievable settings. Coordinate them with device safe operating area, bus capacitance, battery contribution, and cable fault energy.

Precharge capacitive equipment before closing main contactors. Enforce polarity, voltage-match, and insulation checks. Use lockout after persistent faults rather than repeated automatic reclosure into a short.

Critical power shall not depend on one supervisory computer, one communication switch, or one shared sensor. Local hardwired protection outranks supervisory dispatch. Overrides may change scheduling but shall not defeat destructive-fault protection.

## 11. Power-system thermal requirements

Cooling of generators, batteries, converters, conductors, and control electronics is part of the power system's own demand and reliability model. Sector equipment heat rejection is a separate declared interface; do not automatically assign all sector heat to power-plant radiators.

For net source efficiency `eta_gen`:

`Q_generator = P_net × (1 / eta_gen − 1)`

For a converter:

`Q_converter = P_out × (1 / eta_converter − 1)`

Battery heating requires the selected charge/discharge loss model. Some losses counted as electrical auxiliaries also become heat; maintain one consistent energy balance.

Radiator estimate:

`Q = epsilon × sigma × A_effective × (T_radiator⁴ − T_background⁴) − Q_absorbed`

Here `sigma = 5.67 × 10^-8 W/(m²·K⁴)` and effective emitting area includes view factors and useful emitting faces. Use average emitting temperature, not only hot coolant inlet temperature.

With emissivity 0.9, negligible background, and before absorbed sunlight or geometric losses:

| Temperature | Ideal emitted flux | Ideal emitting area per MW |
|---|---:|---:|
| 350 K | 0.77 kW/m² | about 1,310 m² |
| 400 K | 1.31 kW/m² | about 765 m² |
| 500 K | 3.19 kW/m² | about 314 m² |

Do not apply high-temperature generator radiator performance to low-temperature batteries. Size separate temperature regimes and include shadowing, planetary radiation, degradation, pipe losses, and failed sections.

For coolant heat capacity `cp` and temperature rise `DeltaT`:

`mass_flow = Q / (cp × DeltaT)`

Example: 3 MW, `cp = 4 kJ/(kg·K)`, and a 20 K rise require 37.5 kg/s. At 1,000 kg/m³ density this is 0.0375 m³/s. With 200 kPa pressure drop and 60% pump efficiency, pumping requires about 12.5 kW. Coolant chemistry and temperature compatibility must be chosen for the actual loop.

A shared radiator or pump that disables all redundant generators invalidates electrical N+1. Provide independent cooling or enough fault-tolerant sections to meet the same declared failure envelope. Reserve shutdown cooling even after shedding power-production equipment.

## 12. Supervisory control and operating states

Measure output and input power, bus voltages, feeder currents, source availability, storage state of charge and health, insulation condition, temperatures, and estimated reserve autonomy. Continuously recalculate available power and feasible critical supply routes.

| State | Entry condition | Action |
|---|---|---|
| Normal | Capacity and reserves adequate | Serve admitted loads and maintain reserve |
| Capacity constrained | Predicted demand exceeds safe capacity | Deny new starts and reduce flexible loads |
| Single-failure operation | One declared component unavailable | Reconfigure healthy paths; preserve protected service |
| Storage-supported | Generation deficit | Storage supplies deficit; start available sources |
| Critical-only | Primary supply unavailable or reserve threatened | Disconnect shedable groups; preserve critical dependencies |
| Survival | User-approved emergency escalation | Retain only the predeclared survival subset |
| Black start | Healthy main buses de-energized | Execute isolated staged startup |
| Recovery | Generation stable | Restore reserves and loads without inrush collapse |

Never silently downgrade a load marked continuously critical into a shedable survival mode. If the declared reserve cannot sustain it, raise an explicit unmet-requirement alarm and state remaining time.

For routine reserve thresholds, use predicted deliverable energy rather than raw state-of-charge alone. Permit discretionary discharge only above the protected reserve. Use hysteresis and minimum dwell times to avoid repeated shedding and reconnection.

Fast source-trip handling shall be local: storage/converter control responds immediately, local protection clears faults, and supervisory dispatch schedules recovery. A communication message round trip shall not be necessary to keep a critical output energized.

## 13. Worked reference design

The following example illustrates the sizing process; replace its sector table and connectivity with user inputs.

### 13.1 Sector demands

| Sector | Normal kW | Simultaneous peak kW | Critical kW |
|---|---:|---:|---:|
| Command and communications | 100 | 150 | 80 |
| Habitation services | 250 | 350 | 120 |
| Research | 600 | 1,000 | 40 |
| Manufacturing | 700 | 1,200 | 20 |
| Computing | 500 | 700 | 60 |
| Docking and logistics | 150 | 300 | 30 |
| **Total delivered before growth** | **2,300** | **3,700** | **350** |

Critical loads are separately metered groups within sectors. Other groups are shedable according to user-defined priority.

Example permitted connectivity: each sector has an independent home-run connection to A and to B, using separated routes. Each main bus can carry the entire planned load after the other is unavailable. Alternative user graphs shall be checked rather than assumed to have this capability.

### 13.2 Derived demand

Apply 20% growth:

- Normal delivered: 2.760 MW.
- Planned peak delivered: 4.440 MW.
- Critical delivered: 0.420 MW.

Assume central auxiliaries of 0.100 MW normal, 0.120 MW peak/recharge, and 0.030 MW critical, pending component iteration. These include central storage/control/cooling auxiliaries; generator module-local auxiliaries are already deducted from net module output.

With 95% distribution efficiency:

- Normal bus demand: `2.760 / 0.95 + 0.100 = 3.005 MW`.
- Planned peak bus demand: `4.440 / 0.95 + 0.120 = 4.794 MW`.
- Critical bus demand: `0.420 / 0.95 + 0.030 = 0.472 MW`.

### 13.3 Storage and recharge

Eight hours critical service requires `0.472 × 8 = 3.776 MWh` at the main bus.

Select four 2.5 MWh battery banks, 10 MWh total. With one unavailable:

`E_deliverable = 7.5 × 0.80 × 0.70 × 0.95 = 3.990 MWh`.

This provides about 8.45 h at 0.472 MW. It is not 10 MWh of usable emergency energy. This margin is modest; colder conditions or increased auxiliaries require larger banks.

Each bank has a 2 MW bidirectional converter. Three surviving converters provide 6 MW, exceeding the 4.794 MW peak demand. Check cell discharge capability against 2 MW per 2.5 MWh bank, including end-of-life resistance.

Recharging after the eight-hour discharge removes `3.776 / 0.95 = 3.975 MWh` internally. Restoring that energy in eight hours at 95% charging efficiency requires approximately 0.523 MW from the main bus.

Required generation during peak plus recharge: `4.794 + 0.523 = 5.317 MW`.

Local critical UPS capacity is additional to central storage and is not credited to central autonomy. Each sector is sized using its grown critical load; critical local redundancy is checked independently.

### 13.4 Generation

Select four 2 MW net modules, each derated to 1.8 MW available output. Three surviving modules provide 5.4 MW, meeting the 5.317 MW peak-plus-recharge case.

Normal 3.005 MW operation can use two online modules, but the remaining restart time and hot-standby thermal requirements must be validated. Peak and recharge operation requires three available online modules; four may be operated at lower output to reduce trip deficits. Storage bridges the loss of an online module while dispatch is restored.

A separately fueled 0.6 MW net emergency generator exceeds `1.2 × 0.472 = 0.566 MW`. Its declared endurance and fuel inventory remain mandatory; it is not credited to the eight-hour battery-only guarantee.

### 13.5 Distribution and thermal sizing

Select each A/B backbone at not less than 6 MW continuous at 2 kV: 3,000 A aggregate capability, implemented through sectionalized conductors and switchboards. Check every section at minimum operating voltage and under redistribution; an aggregate rating is insufficient.

For full-sector redundant feeds, provide each independent sector path for its entire grown peak. For critical-only redundant feeds, size the second path for grown critical demand and allow ordinary groups to shed after primary-route loss. The design report shall specify which policy is selected for every sector.

At maximum 8 MW net generation and 40% net conversion efficiency, generator waste heat is 12 MW. Allowing 20% thermal margin gives 14.4 MW before adding any separately supported conversion/storage heat. Four independent generator cooling installations rated for 3.6 MW each satisfy a per-module 20% margin; failure of one cooling installation removes only its module. The three remaining generator modules still supply 5.4 MW derated output.

At an illustrative 500 K emitting temperature, 14.4 MW requires about 4,515 m² of ideal effective emitting area. Increase for real environment and geometry. Batteries and low-temperature electronics require separate cooling capacities based on their loss models.

### 13.6 Reference equipment summary

| Item | Selected reference capacity |
|---|---:|
| Primary generation | 4 × 2 MW net |
| Firm derated generation after one module loss | 5.4 MW |
| Peak bus demand plus design recharge | 5.317 MW |
| Main battery energy | 4 × 2.5 MWh nameplate |
| Battery deliverable energy with one bank lost | 3.990 MWh at end of life |
| Battery converters | 4 × 2 MW bidirectional |
| Diverse emergency source | 0.6 MW net, fuel/endurance specified separately |
| Main A and B backbones | Each at least 6 MW continuous at 2 kV |
| Local critical storage | Individually sized, 30 min default |
| Black-start storage | Startup-profile calculation; 350 kWh for the example in Section 8 |
| Generator heat-rejection provision | 4 × 3.6 MW at declared operating conditions |

These selections satisfy the stated arithmetic, but final compliance also requires network, thermal, transient, and fault-domain verification.

## 14. Expansion rules

A new sector shall trigger a fresh calculation of simultaneous demand, critical demand, protected dependencies, routing, source trip deficits, storage autonomy, recharge, auxiliaries, and cooling.

Add generation modules until the surviving set meets the expanded sustained load and selected recharge case. Increase storage until both surviving energy and converter power meet the new criteria. Upgrade every overloaded network section, not only the new sector feeder.

Preprovide isolated expansion bays, protected connection points, and spare control addresses. Reserve physical cable corridors and radiator mounting/view area. Uninstalled future capacity shall not count toward reliability.

No new ordinary load may consume a critical reserve or overload an alternate route required during a fault. Commission it through precharge, insulation tests, converter checks, load admission, and updated failure simulations.

## 15. Required design outputs

For any user configuration, produce:

- A normalized sector/load register, with all defaults and inferred dependencies identified.
- An electrical connectivity graph with permitted and prohibited paths preserved.
- Normal, peak, critical, startup, recharge, maintenance, and failure-case demand tables.
- Generator counts, technology assumptions, firm capacities, startup resources, and consumable endurance.
- Central/local storage energy, power, reserve limits, end-of-life derating, and recharge times.
- Cable and converter schedule with voltage, current, losses, limits, route/domain, and protection.
- Power-system thermal budget, coolant flows, radiator conditions, and cooling fault dependencies.
- Load-shedding and restoration rules, black-start sequence, and operating-state transitions.
- Explicit feasible/infeasible results for every continuity requirement and required remedy for each failure.
- Mass/area/volume estimates with stated assumptions and confidence limits.

## 16. Verification and acceptance

| Test or analysis | Acceptance condition |
|---|---|
| Steady-state normal and peak load | No source, cable, converter, or thermal rating exceeded |
| Any one generator outage | Full planned peak and selected recharge case remain sustainable |
| Any one feeder/bus/converter outage | Every critical output meets its interruption limit and declared autonomy |
| Any one central bank unavailable | Surviving energy meets critical autonomy at end-of-life conditions |
| Loss of all primary generation | Protected loads remain supplied for required duration; ordinary shedding works |
| Largest source trip and load step | Equipment-terminal voltage stays within declared limits |
| Worst-case eclipse, if applicable | Routine cycle plus protected reserve remain available |
| Cooling fault | Affected module isolates without defeating claimed surviving power |
| Battery compartment event | No propagation into independent banks or critical routes |
| Control-network or supervisory-controller failure | Local protection and critical supply continue |
| Full black start | Restart completes within startup-store energy and thermal limits |
| Sector addition | Updated capacity and all required failure cases remain compliant |

Initial normal voltage target: ±5% at equipment terminals. Initial transient target: ±10% for at most 20 ms, unless equipment specifies tighter limits. Voltage tolerance is separate from interruption tolerance: a zero-interruption load must remain within its accepted supply envelope throughout transfer.

Testing shall include representative impedance, cable inductance, source limits, actual protection timing, aged batteries, failed cooling branches, and physical common-cause events where required. Arithmetic spare capacity alone is not acceptance evidence.

## 17. Technical references and assumption policy

NASA's spacecraft power overview describes generation, storage, and power management/distribution as distinct parts of electrical power systems, and discusses battery performance at both cell and system level. NASA's Hubble overview illustrates battery-backed operation during shadow periods. NASA's spacecraft charging standard identifies environment-related hazards for high-voltage systems.

- [NASA — State of the Art: Power](https://www.nasa.gov/smallsat-institute/sst-soa/power-subsystems/)
- [NASA — Hubble Electrical Power](https://science.nasa.gov/mission/hubble/observatory/design/electrical-power/)
- [NASA — NASA-STD-4005: Low Earth Orbit Spacecraft Charging Design Standard](https://standards.nasa.gov/standard/nasa/nasa-std-4005)

The module ratings, demand margins, efficiencies, autonomy times, voltages, and equipment selections in this specification are proposed design assumptions. The references establish relevant engineering context; they do not certify this reference installation or supply its numerical requirements. Replace assumptions with technology-specific validated data as the design matures.
