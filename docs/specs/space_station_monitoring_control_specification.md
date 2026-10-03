# Space Station Monitoring and Supervisory Control — Design Specification

Version 1.0 — 4 October 2026

## 1. Purpose and scope

Provide a coherent view of station condition, detect developing failures, notify crew, record evidence, and coordinate approved responses. Integrate the three existing designs: closed-loop ECLSS, sector-based electrical power, and thermal control.

The system shall answer four operational questions for every sector:

1. Is the sector currently safe and capable of performing its intended function?
2. Which services and reserves are available, through which physical paths?
3. What is likely to fail or reach a limit next, and how much time remains?
4. What automatic actions have occurred, and what action is required from the operator?

Monitoring includes read-only observation, diagnostics, forecasting, and alarms. Supervisory control includes bounded scheduling, setpoint requests, and predefined recovery sequences. Fast protection and equipment regulation remain within subsystem controllers.

The specification is technology-neutral and supports both a simulated station and eventual engineering implementation. Numerical capacities and timing values are initial design assumptions, not qualification or human-safety certification.

## 2. Baseline inputs and integration decisions

Use the current contents of:

- `space_station_closed_loop_eclss_specification.md`: atmosphere, water, biology, carbon recovery, hazardous containment, airlocks, and waste handling.
- `sector_power_system_design_specification.md`: sector demand/connectivity, critical-load protection, modular generation/storage, and protected reserve calculation.
- `space_station_thermal_control_specification.md`: temperature classes, isolated loops, heat rejection, cooling connectivity, and cooling/power interaction.

Do not combine all example ratings into a supposedly validated station. Their examples represent different sizing cases. The thermal design explicitly adds electrical cooling demand and revises generator radiator capacity; integrated capacity shall be calculated from one reconciled equipment register.

### 2.1 Outstanding baseline inconsistencies

The monitoring configuration shall record these as design issues, not silently hide them:

| Issue | Required treatment |
|---|---|
| ECLSS's approximate 5/7 MW thermal allowance versus later class-specific thermal sizing | Use the detailed thermal design for heat-path capacity; reconcile a single integrated load case |
| ECLSS hazardous battery autonomy of at least 24 h versus an 8 h central power reference | Preserve the hazardous-sector requirement independently; size and report its actual protected load and energy |
| Preliminary 97% water recovery and 10–30 L/day makeup | Measure recovery at defined boundaries; do not infer makeup from an optimistic target |
| ECLSS pressure cascade 101/98/95 kPa | Treat as provisional setpoints; validate doors, airflow, leakage, pressure-lock sequencing, and containment performance |
| Sabatier plus methane pyrolysis described as avoiding continuing hydrogen consumption | Balance actual reactions: combined processing still needs net 2 mol H₂ per mol CO₂; track supply and inventory |
| Nitrification/biological waste processing versus the earlier humans-and-plants-only assumption | Require an explicit process choice: approve microbial processing or use an alternative; no undeclared biological participant |
| Whole ECLSS nominal CO₂ allocation based only on human output | Include waste oxidation, biological respiration, and other CO₂ sources before claiming carbon closure |

Monitoring shall expose these uncertainties in resource forecasts until the underlying designs are reconciled. This document defines oversight; it does not revise the three source documents.

## 3. Design requirements

- No single supervisory node, switch, historian, display, or time source shall stop local critical protection or regulation.
- Critical alarms shall remain available locally when the station backbone or primary control room is unavailable.
- Failed, stale, uncalibrated, or missing telemetry shall never be displayed as a healthy current measurement.
- Sector criticality shall include dependencies: power, cooling, sensors, controllers, valves, air circulation, and required resource paths.
- Available service shall be assessed under the declared failure envelope, not solely from installed nameplate capacity.
- Hazardous-sector information may be exchanged over controlled data interfaces without connecting air, water, or coolant inventories.
- Commands shall be authenticated, bounded, observable, and auditable. Loss of command authority shall not defeat independent local protective actions.
- The station shall operate without ground connectivity for its declared mission duration.
- Resource estimates shall state boundary, scenario, usable inventory, uncertainty, and remaining duration.

## 4. Functional architecture and authority

| Layer | Responsibility | Typical timing |
|---|---|---|
| Device protection | Overcurrent, thermal trip, pressure relief, door interlock, hard limits | Microseconds to equipment-specific seconds |
| Local regulation | Pump speed, converter control, gas/water regulation, safe equipment sequences | Milliseconds to seconds |
| Sector monitoring | Acquire telemetry, evaluate local alarms, maintain local service view | 0.1–5 s |
| Station supervision | Coordinate modes, schedules, capacity admission, and bounded recovery | 1–60 s |
| Analysis/archive | Trends, maintenance prediction, resource planning, historical review | Seconds to days |

These are categories rather than guaranteed response times. Each hazard analysis shall assign an end-to-end deadline and demonstrate that the appropriate local layer meets it.

### 4.1 Redundant infrastructure

Provide two physically separated monitoring networks A and B with independent switches, protected power, and critical cooling. Critical sector controllers publish through both networks or independent equivalent paths. Local emergency annunciation shall not require either backbone.

Use three station supervisory nodes in distinct failure domains. Each may independently compute and display state. Only one may issue normal supervisory commands at a time.

Two of three nodes authorize a renewable leader lease through an actuator-facing arbitration mechanism that rejects expired leases and old authority epochs. A network partition without quorum cannot obtain normal command authority. Local controllers continue regulation and protection. Two locally selected operators shall not become competing remote commanders; a keyed local takeover shall explicitly fence remote authority.

Three replicated nodes improve availability against crash/omission faults; they do not establish protection against arbitrary malicious or common-software behavior. Catastrophic-actuation prevention remains in separately justified local interlocks and command checks.

Use two historian/archive nodes, at least two separated operator stations, and local panels in critical sectors. A historian failure shall not block command processing or alarm publication.

### 4.2 Authority precedence

Equipment hard protection and required interlocks take precedence over local regulation and station requests. Authorized local manual control can override ordinary supervisory requests, subject to protected interlocks. Ground control submits requests through station arbitration and cannot bypass onboard safeguards.

There is no universal safe command such as 'close every valve'. Safe behavior is assigned per actuator: containment isolation, continued critical flow, freeze bypass, and source shutdown can require different positions.

## 5. Station model and configuration

Maintain one versioned model containing sectors, compartments, equipment, electrical buses, coolant circuits, material stores, air paths, sensors, actuators, and physical failure domains.

Keep distinct graphs for power, thermal transfer, air/material flow, and data. Record dependencies between graphs; do not assume electrical connectivity implies cooling or resource connectivity.

### 5.1 Equipment record

Each record declares identity, location, owner subsystem, normal/peak/critical demand, interfaces, dependencies, rated limits, startup/shutdown behavior, measurement points, local protection, permitted commands, expected responses, maintenance state, configuration version, and proof-test status.

A component can be unavailable, degraded, isolated, under maintenance, or healthy. Do not equate 'powered', 'communicating', or 'commanded on' with 'providing service'.

### 5.2 Instrument record

Declare sensor range, unit, accuracy, response time, sampling/publication period, calibration interval, uncertainty, expected operating range, stale threshold, failure detection, measurement location, and safety role.

Define trip voting individually. Three co-located identical sensors sharing a sampling line, power supply, or calibration error are not three independent hazard detections. Any two-out-of-three scheme must evaluate both missed-demand and false-trip consequences. A smoke alarm shall not automatically require majority voting merely because a pressure control uses it.

### 5.3 Configuration changes

Treat threshold, topology, criticality, and sequence changes as controlled versioned updates. Validate units, references, dependency completeness, permitted routes, capacity, and local-controller compatibility before activation. Record previous and new versions and rollback conditions.

Maintenance isolates the intended equipment and updates redundancy claims. It shall not globally suppress all alarms from a sector. Show which protection or observation is temporarily unavailable.

## 6. Common telemetry contract

Every telemetry item shall include:

| Field | Meaning |
|---|---|
| `asset_id`, `point_id` | Stable equipment and measurement identity |
| `value`, `unit` | Engineering value with explicit unit |
| `source_time`, `receive_time` | Acquisition and ingestion timestamps |
| `sequence`, `source_boot_id` | Detect gaps, duplicates, and resets |
| `quality` | Good, suspect, bad, stale, unavailable, maintenance, or simulated |
| `uncertainty` | Measurement/model uncertainty where applicable |
| `config_version` | Interpretation and calibration version |
| `origin` | Sensor, computed value, manual observation, or model prediction |

Use absolute pressure in kPa and explicitly label differential pressure. Distinguish oxygen percentage from oxygen partial pressure, and thermal kW from electrical kW. Use K for thermodynamic temperature equations, °C for operator temperatures, kg/s for mass flow, and m³/h for declared air volumetric flow. Gas flow meters shall state whether volume is actual or standardized and the reference conditions.

Safety decisions use fresh qualified inputs and an explicit degraded-data rule. Carry source acquisition age through forwarded telemetry; a newly received old packet is still old. Estimated state shall not silently replace missing measured state.

### 6.1 Time and data freshness

Provide two qualified time sources and local monotonic clocks. Target ordinary cross-node clock agreement within 1 ms; hardware event recorders shall specify tighter local ordering if required. A 1 ms synchronized network cannot order microsecond electrical events; use local high-speed captures and disclose clock uncertainty.

Default stale declaration: greater than the larger of three publication periods or 2 s, capped by the point's hazard-specific allowable age. Slow laboratory measurements have their own validity interval. Local trips shall not rely on this generic telemetry rule.

If clocks disagree, continue monotonic local protection, flag absolute-time uncertainty, and preserve source-local event order. Never reset control timers from an untrusted wall-clock jump.

## 7. Electrical power monitoring

| Asset | Required measurements/status | Derived state |
|---|---|---|
| Generation module | Gross/net output, availability, temperatures, cooling flow, limits, trip reason, start stage | Firm output, permissible ramp, restart readiness |
| A/B bus sections | Voltage, current, insulation condition, breaker/tie state, conductor temperature | Energized topology, overload margin, isolated fault region |
| Sector feeders/converters | Input/output voltage and power, limit state, temperature, route identity | Actual delivered power, surviving alternate capacity |
| Batteries | Pack/module voltage, current, cell temperatures, state of charge/health, contactors, cooling | End-of-life deliverable energy, discharge power, reserve duration |
| Fast buffer | Energy state, converter readiness, capture events | Ride-through margin |
| Emergency/black-start sources | Usable fuel/energy, auxiliaries, isolation, readiness | Start attempts, endurance, critical route availability |
| Docking interface | Transfer direction/power, isolation, limits | Export/import obligations and safe-disconnect readiness |

Publish actual load, admitted load, scheduled load, growth allowance, and emergency shed capability separately. Reserve calculations shall use the present surviving topology and battery condition.

For equal bank factors, the earlier example had about 3.990 MWh deliverable with one bank unavailable and approximately 0.472 MW critical bus demand, giving 8.45 h. That duration shall not remain on the display when critical thermal loads are added or inventories degrade. It is a regression-test case, not the integrated station guarantee.

Display usable black-start energy separately from ordinary storage. Do not count the same protected battery energy twice in central and sector autonomy.

## 8. ECLSS monitoring

### 8.1 Atmosphere and pressure compartments

Continuously monitor compartment pressure, oxygen partial pressure, carbon-dioxide partial pressure/concentration, temperature, relative humidity/dew point, circulation flow, smoke, and selected gas contaminants. Measure hazardous pressure differences with appropriately ranged differential sensors, as well as independent absolute pressure instruments.

For large spaces, use distributed sensors near occupied zones and circulation returns. Placement shall demonstrate that stagnant zones and local releases cannot remain hidden behind a favorable station-average reading.

Initial station reference pressure is 101 kPa and oxygen partial pressure is approximately 21 kPa. These are operating targets inherited from the baseline, not alarm or medical exposure limits. Exposure limits, time-weighted limits, emergency thresholds, fire-risk limits, and sensor accuracy shall come from an approved mission-specific ECLSS limit register.

Track mechanical oxygen/CO₂ capability separately from biological capability. Plants/algae may respire and can lose production after lights are shed. Do not represent biological capacity as instantly dispatchable guaranteed reserve.

### 8.2 Gas production, recovery, and reserves

Measure electrolysis water input, oxygen/hydrogen output, purity, separators, pressures, temperatures, and storage inventory. Measure CO₂ collection, routing, Sabatier inputs/products, methane processing, hydrogen recycle, and solid carbon output.

Use pressure/temperature-compensated inventory models; compressed gas mass is not simply receiver volume. At high pressure use an appropriate real-gas model. Flag uncertain inventories and unusable residual pressure.

The baseline 500 kg oxygen reserve divided by 84 kg/day human demand is about 5.95 days before leaks, margin, and inaccessible inventory. Publish that as a metabolic-only comparison beside the operational endurance scenario.

### 8.3 Water and nutrient processing

Monitor tank inventory, flow, pressure, leak indicators, membrane/filter condition, conductivity, pH, turbidity, temperature, treatment state, and available disinfectant where used. Add process-specific contaminant measurements and validated sampling routines.

Potable release requires the approved multi-barrier quality and sampling procedure. A conductivity or turbidity sensor alone does not establish drinkability. Unknown quality holds the batch out of approved potable inventory.

Measure process recovery over a declared interval and boundary:

`recovery = accepted recovered mass / processed input mass`

Record retained process inventory, concentrate output, evaporation/condensate routes, sampling losses, and external makeup. A falling tank can reflect redistribution rather than a leak; reconcile the whole network before classifying it.

Track nutrient inventories and ratios, including pH and electrical conductivity, with scheduled chemical assays. Conductivity is not a complete measurement of individual nutrients.

### 8.4 Hydroponics and algae

Monitor reservoir/culture temperature, inventory, circulation, lighting electrical power and scheduled exposure, CO₂ feed, oxygen output or gas exchange, pH, conductivity, dissolved oxygen where relevant, and harvest mass. Identify contamination/testing state for each independent reactor.

Distinguish commanded light level, actual light output, biomass production, and inferred gas exchange. Predict gas and humidity changes after load shedding using measured performance and uncertainty. Protect nutrient/culture pumps that remain critical even while lighting is shed.

### 8.5 Hazardous systems

Monitor independent air/water/coolant paths, pressure cascade, barrier integrity, filtration condition, treatment cycle evidence, containment power/cooling, wastewater free capacity, and independent reserves.

'Filtered', 'sterilized', and 'safe for release' are distinct states. No generic gas analyzer or UV indication certifies every chemical or biological hazard. Release requires a validated hazard-specific treatment and test record, with quarantine when evidence is incomplete. Automatic supervisory logic shall not bypass that hold.

### 8.6 Airlocks and terminal waste

Airlock telemetry includes inner/outer hatch position and latch proof, chamber and adjacent pressures, suit/occupancy status where provided, receiver pressure/temperature, pump status, valve position, atmosphere recovered, remaining local power, and cycle stage.

Door interlocks operate locally. Any uncertain seal/pressure state inhibits a new unsafe opening sequence while preserving applicable emergency procedures. Determine gas-recovery fraction from measured mass balance, not solely compressor run time.

Waste handling records cartridge identity, material classification, recovered resources, containment/sterilization evidence, mass, storage position, and disposal state. Disposal commands require independent hatch/trajectory/clearance interlocks from the relevant disposal and navigation systems. Monitoring does not supply an assumed safe disposal trajectory.

## 9. Thermal monitoring

Measure inlet/outlet coolant temperature, pressure, flow, pump electrical demand, valve position, inventory, accumulator state, equipment cold-plate temperature, exchanger barrier sensors, and radiator panel temperatures/orientation. Monitor chillers on evaporator and condenser sides.

Derived values include:

`Q_transferred_kW = mass_flow_kg_s × cp_kJ_kgK × DeltaT_K`

`COP_cooling = Q_evaporator / P_compressor`

`Q_condenser ≈ Q_evaporator + P_compressor`

Apply measurement uncertainty and fluid-property variation. Small temperature differences can create large relative heat-estimate error. Compare independent energy estimates over appropriate windows rather than tripping on every imperfect residual.

Available cooling is indexed by temperature class and route. A hot radiator cannot replace a cold-water chiller merely because both have spare thermal kW. Account for current environment, view factors, panel condition, approach temperatures, failed branches, and physical accessibility.

Use equipment thermal capacitance and measured trends to forecast time to limit. Report bounds and model confidence; do not extrapolate a brief noisy derivative into a precise evacuation deadline.

Track phase-change buffer state separately from coolant temperature. A buffer at its melt temperature can be almost empty or almost full of stored heat.

## 10. Additional station monitoring interfaces

The architecture shall accept later systems without implying they have already been designed:

| Domain | Initial monitored interfaces |
|---|---|
| Hull and structures | Compartment leak indicators, hatch integrity, strain/impact sensors where fitted |
| Fire and emergency response | Local detection, fire-zone state, emergency lighting, muster and refuge status |
| Navigation/propulsion | Orbit/attitude, collision alerts, propellant, source health, pointing restrictions |
| Docking/robotics | Capture/latch status, exclusion zones, service-transfer authorization |
| Communications | Link availability, delay, quality, emergency path |
| Medical/crew | Authorized emergency status and equipment readiness; privacy-limited health telemetry |
| Cargo/maintenance | Inventory, spares, service schedules, repair state |

Each additional domain supplies its own validated protection and limit register. Absence of a navigation or structural monitoring interface shall be shown as 'not configured', not 'safe'.

## 11. Alarms and operator interaction

### 11.1 Severity and lifecycle

| Level | Meaning | Required presentation |
|---|---|---|
| Emergency | Immediate crew response or escape/refuge procedure | Local and station audible/visual annunciation with location and action |
| Warning | Critical service lost or approaching a hazardous limit | Persistent prioritized annunciation and response procedure |
| Caution | Redundancy degraded or ordinary limits exceeded | Visible queue and trend/context |
| Advisory | Operational change or maintenance need | Nonintrusive notification/history |

Severity follows consequence and time to harm, not subsystem ownership. An ordinary experiment fault can become an emergency if it compromises containment.

Each alarm definition includes condition, data quality prerequisites, onset delay if safe, reset hysteresis, latching policy, recipient locations, prescribed automatic actions, procedure, and suppressed/shelved states. Never apply a blanket debounce delay to a rapid hazard.

Acknowledgement means the operator has seen an alarm. It does not clear the condition or cancel its protective action. Audio silencing shall retain visual indication and allow newly arising emergencies to annunciate. Shelving requires role permission, reason, scope, expiry, and a conspicuous indication.

### 11.2 Cascades and alarm floods

Group correlated consequences under an incident with a suspected initiating fault, supporting evidence, and competing explanations. Retain the underlying events and local emergency alarms. Correlation shall not erase an unrelated fire or containment alarm.

Reference load test: process 1,000 state/alarm events per second for 60 s without losing emergency events. Presentation may group and prioritize; acquisition shall not discard the evidence. Event queues shall reserve capacity for critical traffic and disclose any lower-priority loss.

### 11.3 Operator displays

Provide station and sector maps, live power/cooling/material routes, equipment status, alarm list, incident timeline, resource forecasts, and maintenance/configuration views. Show both installed capacity and accessible surviving capacity.

Each value displays unit, age, quality, and whether measured or estimated. Use color plus text/symbols rather than color alone. Show isolation, unavailable paths, protected reserves, and pending commands explicitly.

The overview presents critical service by sector: electrical supply, cooling class, atmosphere/containment, water/resource status, redundancy, and earliest credible resource/temperature limit. Explain the reason for a degraded state.

Emergency local panels shall retain essential readings, alarm location, and permitted manual procedures without a station display server. Avoid video or detailed charts displacing required alarm information.

## 12. Coordinated decisions and commands

### 12.1 Load admission

For a proposed load start, verify fresh configuration, available electrical capacity, surge support, compatible cooling path, ECLSS/material obligations, critical reserves, and required failure-case margins.

Reserve capacity with an expiring admission token bound to equipment, permitted ramp, configuration epoch, and operating interval. Local controllers enforce it. If preconditions expire or conditions change before start, reject the request. Do not use an unbounded reservation that prevents critical recovery.

Loss of admission authority inhibits new discretionary starts; established critical services continue under local limits. Protected emergency actions do not depend on station consensus.

### 12.2 Command contract

Every command carries unique identity, authorized origin, asset, requested bounded action, validity interval, sequence/authority epoch, expected preconditions, and configuration version.

Record accepted, rejected, executing, completed, failed, cancelled, and outcome-unknown states. A receipt is not proof of physical completion; completion requires actual position/output evidence.

Commands shall be idempotent where possible. After timeout or failover, query observed state before retrying a non-idempotent action such as starting a cycle or ejecting a cartridge. Expired commands shall not execute after delayed delivery.

Destructive, irreversible, or hazard-boundary actions require the mission-approved operator authorization/interlock procedure; automatic authority is permitted only for explicitly validated sequences. Emergency trips may operate autonomously without waiting for operator confirmation.

## 13. Fault detection and resource estimation

Use threshold checks, rate-of-change checks, heartbeat/watchdog checks, sensor disagreement, stuck-value detection, commanded-versus-observed behavior, and conservation residuals. Model-based or statistical diagnostics may advise operators; they shall not acquire unvalidated protection authority.

Material balance over an interval:

`change_in_inventory = inputs − outputs + net_internal_production`

Electrical balance includes load, generation, battery power, conversion losses, and exports. Thermal balance includes generated/absorbed heat, stored energy change, rejected heat, and exports. Maintain clear boundaries so heat-pump work and biological latent heat are not counted twice.

Leak classification requires residuals beyond measurement uncertainty and expected redistribution. Publish 'suspected leak' and supporting confidence until isolation/testing establishes cause.

### 13.1 Autonomy forecasts

For a static scenario:

`endurance_h = deliverable_energy_kWh / protected_load_kW`

For resources, use accessible usable mass and net consumption after guaranteed recovery, not total physical inventory. Forecast time profiles when generation, metabolism, biological uptake, temperature, or load changes over time.

Do not use a nearly zero uncertain net consumption rate to display infinite endurance. Present bounded scenarios such as no biology, one processor unavailable, additional crew, isolation of a compartment, or scheduled airlock cycling.

Station survival duration is the earliest relevant limit under the selected scenario, with uncertainty: energy, oxygen, CO₂ removal, cooling, pressure, water, or another protected service. It is not a single universal countdown and shall state required crew actions and dependencies.

## 14. Example coordinated incidents

| Incident | Local action | Station coordination and evidence |
|---|---|---|
| Generator trip | Electrical storage supports bus; faulted source isolates | Recalculate supply, cooling and recharge; start available source; shed admitted ordinary loads if needed |
| Loss of chilled-service module | Transfer local cooling; protect equipment temperatures | Preserve critical ECLSS duty; reduce incompatible heat loads; update compressor/power demand |
| Rejection-train loss | Isolate leak/fault; maintain surviving loop | Calculate class-specific shortfall and shed heat sources; derate affected generation independently |
| Main power blackout | Local UPS and protection remain active | Critical-only mode, independent ECLSS support, ongoing radiator/shutdown cooling, bounded black start |
| Hazard pressure reversal | Validated local containment response | Stop hazardous process, restrict transfers, preserve containment power/cooling, notify affected zones |
| Water quality failure | Quarantine affected batch and stop release | Use approved reserve, inspect treatment evidence, forecast capacity and resupply |
| Loss of both data paths to sector | Local regulation and alarms continue | Display state as unknown; prohibit new remote ordinary starts; send crew using approved procedures |
| False or stale oxygen telemetry | Local degraded-sensor logic | Annunciate loss of trustworthy measurement; compare independent instruments; do not show normal estimated value as measured |

For the thermal reference, losing a whole station rejection train leaves about 1.8 MW direct and 1.2 MW warm rejection, less than its 2.48/1.54 MW full operating duties. Monitoring shall identify that operating deficit, not declare full service because critical duties remain within the surviving train capacity.

Recovery establishes cooling and protection before restarting heat-producing loads, restores ECLSS process stability, and replenishes electrical/thermal buffers without creating a new overload.

## 15. Communications, logging, and security

Separate protection/control traffic from payload, user, video, and external networks. Use bounded traffic classes, prioritization, rate limits, and admission controls. No payload device may exhaust critical telemetry bandwidth or command-processing capacity.

Use redundant 1 Gbit/s monitoring backbones as an initial reference, with demonstrated critical traffic latency under failure and burst conditions. Size links by actual payload and protocol overhead. Wireless devices may supplement maintenance access but shall not be the sole critical path without a separate justification.

Authenticate sources and operators; authorize by role and asset. Use replay protection, integrity checks, signed/versioned configuration, secure recovery images, and recorded local takeover. Keep ground/payload gateways separate from direct actuator access.

Controllers and nodes shall bound CPU/memory/queue usage, use watchdogs and error-detecting storage/memory as appropriate, and reject malformed units/ranges. Radiation tolerance and common-software failures require mission-specific analysis; redundant ordinary computers alone are insufficient evidence.

### 15.1 Historian and event recorder

Archive raw engineering telemetry, quality changes, command lifecycle, alarms, configuration/calibration changes, and operator actions. Keep local circular high-rate buffers for electrical and other fast transients; upload triggered captures separately from routine telemetry.

On disconnection, sectors buffer data with source timestamps and sequence numbers. Recovery preserves late-data provenance rather than rewriting it as current. Storage exhaustion shall degrade lower-priority retention first while preserving critical events and disclosing gaps.

Ground downlink is prioritized and store-and-forward. Loss of ground connectivity shall not prevent onboard alarming or normal protected operation. Telemetry access and human medical information shall follow the applicable privacy policy.

## 16. Reference capacity and scaling

Illustrative design basis: 20 sectors, 10,000 telemetry points, of which 2,000 publish at 10 Hz and 8,000 at 1 Hz. High-speed protection samples are local and excluded from this routine rate.

At a 32-byte normalized sample record:

- Samples: `2,000 × 10 + 8,000 × 1 = 28,000 samples/s`.
- Normalized record stream: `896,000 bytes/s`, about 7.17 Mbit/s.
- Planning factor 2 for transport/index representation: about 14.34 Mbit/s per full stream.
- Duplicating transmission on separate A and B links does not double traffic on each link; shared aggregation shall be sized appropriately.
- Uncompressed normalized samples: about 77.4 GB/day, decimal units.
- Thirty-day raw records: about 2.32 TB per copy; factor 2 indexing/metadata allowance gives about 4.65 TB before free-space margin and triggered captures.

Provide at least 8 TB usable storage per historian for this retention profile, with two independent copies. That rating is usable capacity after any local drive-redundancy overhead. Long-term downsampled trends, event records, and transient captures require separate retention budgets.

Reference sector buffering: 32 GB per sector. At an even-share 38.7 kB/s raw rate with factor 2 overhead, one day requires about 6.7 GB. Distribution is not necessarily even; size each sector for at least 24 h at its actual full telemetry and event rate.

Routine targets:

| Function | Initial performance target |
|---|---:|
| Local emergency annunciation after qualified condition | Within hazard-specific deadline; 250 ms transport/annunciation target where appropriate |
| Station emergency notification transport | ≤250 ms on healthy backbone after local publication |
| Operator live overview | 1 s update; age shown |
| Supervisory capacity recalculation | ≤1 s for ordinary changes; conservative local limits meanwhile |
| Command receipt | ≤500 ms on healthy local network; execution deadline separate |
| Supervisory failover | ≤2 s; local protection unaffected |
| Fast controller heartbeat publication | 1 s default; equipment watchdogs faster where required |

Detection delay includes sensor response and qualification, not just network transmission. These targets shall not be substituted for sub-millisecond electrical protection.

### 16.1 Monitoring resource budget

Initial hardware electrical budget: 20 sector interface/controller packages at 50 W each = 1.0 kW; three supervisory nodes at 150 W = 0.45 kW; two historians at 250 W = 0.50 kW; 24 network/gateway units at 30 W = 0.72 kW; four consoles at 100 W = 0.40 kW. Subtotal 3.07 kW. Allocate 4 kW including growth for this listed equipment.

Sensor/actuator power, local safety controllers not in those packages, and cabinet cooling are additional. Required critical subset and power paths shall be listed individually, not inferred from the subtotal. Most consumed electrical power becomes heat at the installation location and enters its thermal register.

If all 4 kW were protected for eight hours, energy is 32 kWh delivered. At 80% end-of-life capacity, 70% usable state-of-charge window, and 95% output efficiency, approximately 60.2 kWh nameplate is needed before bank-loss redundancy. Prefer allocation within the existing protected power budget where justified; do not duplicate counted reserves.

## 17. Commissioning and acceptance

Produce the integrated equipment/sensor register, fault-domain maps, telemetry dictionary, threshold/limit register, alarm philosophy, command authorization matrix, local/remote authority rules, incident sequences, topology models, calibration plan, and retention/resource budgets.

Required tests:

| Test | Acceptance evidence |
|---|---|
| Supervisor crash or network partition | Local services continue; only valid authority commands; stale state visible |
| One backbone/switch/power-feed loss | Critical observation and required local alarms remain available |
| Sensor drift, stuck value, disagreement, missing data | Quality and degraded-mode logic behave correctly; no false healthy display |
| Generator/feeder/storage failure | Accessible capacity and reserve forecast update from observed topology |
| Cooling train/chiller/barrier failure | Temperature-class capacity and critical dependencies remain correctly represented |
| ECLSS contamination/pressure/quality fault | Isolation and quarantine cannot be bypassed by generic recovery commands |
| Alarm flood | Emergency evidence retained and clearly prioritized; acquisition losses disclosed |
| Command duplicate, timeout, delayed delivery, and takeover | No unsafe repeated action, expired command, or split authority |
| Time-source failure/clock step | Local timing stable; event uncertainty displayed |
| Blackout and black start | Monitoring-critical power/heat paths survive; recovery observable and auditable |
| Archive full or corrupted | Protection/control unaffected; essential records preserved and gaps reported |
| Changed configuration or added sector | Units, topology, capacity, dependencies, and limits validated before admission |

Test simultaneous consequences of a common physical fault, not only one failed component at a time. Use hardware-in-the-loop where timing matters and replay historical/synthetic telemetry for diagnostics. Ground-truth expected service states shall be compared with displayed states and commanded actions.

## 18. References and engineering status

NASA's failure-tolerant avionics guidance emphasizes explicit fault containment, a declared failure hypothesis, and evidence for redundancy and recovery. NASA's crew-interface guidance provides context for alarms and operator interaction. Those sources inform architecture; the capacities and timings above are proposed design values.

- [NASA NESC — Failure-Tolerant Avionics for Crewed Space Systems: Recommended Best Practices](https://www.nasa.gov/wp-content/uploads/2023/05/nesc-ib-24-01986-failure-tolerant-avionics-for-crewed-space-systems-recommended-best-practices-7-11-24-nrb-ib-final.pdf)
- [NASA — Crew Interfaces](https://www.nasa.gov/reference/10-0-crew-interfaces-vol-2/)
- [NASA — Human Integration Design Handbook](https://www.nasa.gov/human-integration-design-handbook/)

Human exposure limits, hazardous containment acceptance criteria, nuclear safety limits, and protective voting shall be supplied by the applicable subsystem engineering process. This specification does not invent universal limits or claim flight certification. Monitoring shall visibly distinguish validated requirements, provisional assumptions, unavailable evidence, and demonstrated capability.
