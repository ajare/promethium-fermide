# Space Station Internal Security System — Design Specification

Version 1.0 — 4 October 2026

## 1. Purpose and scope

Protect crew, critical infrastructure, controlled materials, and station operations against unauthorized access, tampering, theft, violence, and misuse of equipment. Provide cameras, complementary scanners/sensors, access controls, alarms, monitoring, threat classification, and accountable incident response.

Integrate with the existing station monitoring and supervisory control design, including electrical power, ECLSS, and thermal services. Security is a separate operational domain connected through bounded interfaces; it shall not gain unrestricted control of life-support or protective equipment.

This specification is a conceptual engineering design. Capacities, timing targets, and example incident rules are proposed defaults. Final installation requires a station layout, operating policy, equipment selection, hazard analysis, and validation in the actual lighting, gravity, radiation, and electromagnetic environment.

The system provides detection, access restriction, evidence, and support to human responders. Armed robots, autonomous force, and automated detention are outside scope.

## 2. Design principles

1. Security restrictions shall preserve approved emergency escape, refuge access, and critical-service maintenance.
2. Pressure and hazardous-containment barriers retain their independent safety interlocks. An evacuation instruction does not imply opening a hatch into vacuum or contamination.
3. Camera analytics indicate observable events, not a person's intent or guilt. Unknown identity is not itself a threat.
4. Threat confidence, event severity, and equipment health are separate attributes.
5. Critical detection and annunciation shall survive a single declared controller, network, recorder, or power-path failure.
6. Loss of a camera or scanner shall be shown as degraded coverage, never as evidence that an area is clear.
7. Commands and evidence access shall be authenticated, role-limited, and auditable.
8. Crew private spaces shall receive appropriately limited monitoring; security coverage shall focus on boundaries and shared infrastructure.
9. Responses shall be bounded by explicit authority and verified local conditions.
10. Security capacity shall scale with coverage objectives and route topology, not only with population.

## 3. User-supplied configuration

The user shall specify station sectors, access routes, private areas, critical assets, hazardous boundaries, docking/intake points, expected occupants and visitors, robot/vehicle routes, and available power/data connections.

For each zone define:

| Field | Requirement |
|---|---|
| Identity and geometry | Zone ID, compartment footprint/volume, doors, sightlines, gravity regime |
| Security purpose | Unauthorized entry, equipment tampering, cargo screening, emergency assistance, etc. |
| Access policy | Roles, work permits, escort requirements, time limits, emergency access |
| Safety constraints | Pressure/containment limits, escape/refuge routes, occupancy and machinery hazards |
| Coverage objective | Detect presence, observe activity, recognize an object, or support credential-assisted verification |
| Response policy | Local alarm, operator review, deny entry, temporary access restriction, approved emergency sequence |
| Service dependencies | Critical power, cooling, controller/network paths, local autonomy |
| Privacy/retention | Permitted sensors, masks, recording, evidence-access roles and expiry |
| Failure requirements | Single device/path failure, repair window, acceptable observation gap |

Use explicit identities for doors, cameras, scan stations, lockers, and monitored equipment. Geometry and field of view shall determine device count. A default number of cameras per room is not a coverage proof.

## 4. Security zoning and access model

| Zone | Typical location | Initial access mechanism |
|---|---|---|
| Z0: shared | Main corridors, communal spaces | General crew/visitor authorization; shared-area monitoring |
| Z1: staff-controlled | Stores, workshops, ordinary laboratories | Cryptographic crew credential with role permission |
| Z2: restricted | Control rooms, key equipment rooms, sensitive archives | Credential plus PIN or independently approved second factor |
| Z3: critical | Critical switchgear, life-support controls, emergency reserves | Strong authentication, work permit, selected two-person operations |
| ZH: hazardous | Containment laboratories, contaminated stores | Authorization plus required training/process clearance and local containment sequence |
| ZP: private | Cabins, hygiene facilities, confidential treatment spaces | Boundary access/duress provisions; no routine interior recording |

ZH is a hazard designation and can overlap Z2/Z3. Criticality of an asset is independent of whether it lies in a private or shared sector.

Permissions shall include specific actions, not just room entry. Being allowed into a workshop shall not authorize disabling a coolant interlock or removing emergency oxygen.

Credential validation uses signed cryptographic proof rather than trusting a readable badge serial number alone. Provide short-lived visitor credentials, escort relationships, revocation, lost-credential handling, and a controlled replacement process.

Offline door controllers hold signed, expiring permissions and a revocation cache. Define the maximum offline validity by zone; reference defaults are 24 h for ordinary staff permissions and 1 h for new high-criticality entry grants. Existing occupants retain approved safe egress. Show the age of revocation information and apply the declared degraded policy.

Optional biometric verification shall be one-to-one against an enrolled credential, with a non-biometric alternative for injury, gloves, disability, or equipment failure. Routine station-wide facial identification is excluded from the default design.

## 5. Cameras and optical coverage

### 5.1 Camera types

| Type | Use | Initial planning specification |
|---|---|---|
| Fixed visible camera | Doors, corridors, cargo bays, critical equipment approaches | 4 MP, 15 frames/s normal, 25–30 frames/s incident mode where supported |
| Wide-angle overview camera | Shared-space occupancy and route observation | 4–8 MP; distortion-corrected display with calibrated geometry |
| Close-view camera | Seal, latch, locker, or handling-station detail | 2–4 MP; lens selected for required object detail |
| Thermal infrared camera | Low-light presence, hot-object/process checks | Approximately 640 × 480 or greater where justified; validated temperature accuracy |
| Pan/tilt/zoom camera | Operator examination of an incident | Supplementary; not the sole camera for a required fixed view |
| Robot-mounted camera | Temporary inspection or responder assistance | Stream includes robot location, pose, and age; not guaranteed stationary coverage |

Thermal cameras do not see through opaque walls or reliably establish concealed contents. Many ordinary windows block long-wave infrared. Visible cameras require a low-light plan, emergency lighting or qualified illumination, and glare/smoke testing.

### 5.2 Placement and redundancy

Cover controlled door approaches and crossings, cargo intake/transfer points, critical cabinet access, and responder routes. Critical boundaries shall have two independent observation mechanisms, such as a fixed camera plus door/latch contacts and a crossing sensor. Two cameras with the same occlusion and switch/power route do not establish independence.

Specify minimum useful object pixel size at the farthest required location. A 2,560-pixel-wide image spanning 10 m gives about 256 pixels/m before distortion and other losses. That arithmetic does not guarantee person recognition; acceptance requires representative recorded scenes with the chosen lens, illumination, motion, and compression.

Microgravity coverage shall accommodate people and cargo in different orientations. Do not assume all faces and objects are at floor-relative standing height. Avoid mounting conflicts with handrails, air vents, moving cargo, and fire equipment.

Provide tamper/open-cover detection, lens obstruction/frozen-image checks, secure mounts, local buffer storage, and controlled maintenance access. Critical cameras shall have independent power/data paths or duplicated viewpoints where a single failure would otherwise eliminate necessary coverage.

### 5.3 Audio

Use intercom and manually activated duress audio where needed. Continuous ambient speech recording is disabled by default. Acoustic event detection, if configured, shall declare whether audio is stored and how privacy is protected; tool noise, fans, machinery, and pressure transitions shall be tested for false alerts.

## 6. Complementary scanners and sensors

| Device | Intended evidence | Important limitation |
|---|---|---|
| Door/latch/bolt contacts | Actual barrier state versus commanded state | A lock command is not proof of closure or pressure sealing |
| Optical beam/depth crossing sensor | Crossing direction, count, tailgating indication | Occlusion, large cargo, and microgravity orientations can confuse counting |
| LiDAR/depth scanner | Occupancy, route obstructions, dimensional cargo checks | Eye safety, reflectivity, blind spots, and blocked line of sight require validation |
| Short-range radar | Complementary motion/presence under selected visual conditions | Multipath and stationary-target ambiguity; not identity evidence |
| Credential reader | Authenticated claimed identity and authorization | Possession may be stolen; credential use does not prove every crossing person's identity |
| Asset tag/RFID reader | Inventory movement and container association | Missing/unread tag is not proof of theft; metal and shielding can reduce read reliability |
| Cabinet tamper/seal sensor | Unapproved opening/removal | Maintenance context and sensor faults need correlation |
| Cargo X-ray scanner | Internal density/shape screening of appropriate packages | Shielding, dense objects, and automated interpretation limits; not chemical identification |
| Metal detector | Metallic-object screening where justified | Routine station tools create many expected detections; cannot determine malicious intent |
| Trace chemical scanner | Targeted residue/material screening | Hazard-specific sensitivity, contamination, sampling time, and false detections |
| Radiation monitor | Selected radiation sources or contamination | Background, geometry, shielding, and calibrated response matter |
| Manual sampling station | Confirm suspected contamination/content | Results have custody, delay, and sample-validity requirements |
| Duress button | Direct request for help | Needs independent power, clear location, and accidental-activation handling |

Ionizing cargo scanners are optional enclosed installations with independent interlocks, shielding, exposure controls, and maintenance procedures. They shall not be repurposed for routine scanning of people. Add their power, heat, equipment mass, and space to the station register.

Unknown packages are screened against a manifest and handling policy. A scanner clear result means no selected indicator was detected under stated conditions; it does not certify universal safety. Dense/unreadable packages proceed to an approved secondary inspection or quarantine path.

## 7. Access barriers, vehicles, and local interlocks

Ordinary access doors, pressure hatches, containment locks, and cargo gates are separate equipment classes. Every barrier declares ingress policy, egress mechanism, power-loss state, manual release, pressure/containment interlocks, emergency authorization, and actual position sensors.

| Barrier | Power-loss/security rule |
|---|---|
| Ordinary access door | Preserve approved egress mechanically or through protected independent release; ingress state assigned by zone |
| Pressure hatch/airlock | Maintain pressure-safe state; security cannot override unsafe opening interlocks |
| Hazard containment lock | Preserve containment while enabling the approved escape/decontamination procedure |
| Cargo/vehicle gate | Stop motion safely and keep evacuation paths available; do not trap people beneath a moving barrier |
| Critical cabinet | Secure against unauthorized entry while retaining authorized emergency maintenance access |

No blanket 'lock every door' response is permitted. Access restriction shall be checked against live pressure/hazard state, mapped exit/refuge routes, known/uncertain occupancy, and independent safety rules. Even then, occupancy estimates are advisory; closure sensors and manual emergency provisions remain necessary.

Classify robots and vehicles as authenticated assets with route permits and task permissions. Check payload, movement zone, speed limit, and remote-control authority. A vehicle alarm may request a safe stop through its controller; security shall not cut traction power in a way that disables braking or strand it across an exit.

## 8. Monitoring architecture and interfaces

### 8.1 Layers

- Local controllers acquire contacts/readers, run barrier interlocks, and annunciate urgent local events.
- Sector gateways correlate measurements, enforce telemetry quality, and buffer events during disconnection.
- The security incident service maintains evidence-linked cases and threat assessments.
- Operator consoles provide maps, alarms, live views, incident review, and authorized commands.
- Recorders preserve video and event evidence independently of supervisory command execution.

Follow the station monitoring model for source/receive timestamps, quality, source sequence, configuration version, and measured versus inferred state. Camera metadata additionally includes viewpoint, calibration, codec, frame sequence, image-quality state, and privacy-mask version.

Security metadata connects to the station monitoring backbone through controlled gateways. High-rate video uses a separately provisioned network; it shall not saturate power/ECLSS/thermal control traffic. Security devices shall not directly access those actuators.

### 8.2 Operator views

Display zone map, access boundaries, incidents, alarm priority, relevant camera views, door states, sensor coverage/degradation, responding personnel, and available safe routes. Include evidence age, unknown areas, current authority, and outstanding commands.

A missing camera produces 'view unavailable', not a blank green room. Occupancy estimates state confidence; emergency muster uses independent check-in/crew procedures rather than inferring everyone is safe from badges or cameras alone.

Provide a primary security console, a separated backup console, and local emergency panels. Critical alarms remain active without the incident service or video archive.

## 9. Threat-classification system

Classify incidents rather than permanently scoring people. Store event category, observed facts, consequence severity, confidence/evidence status, affected assets, scope, urgency, uncertainty, and permitted responses as separate fields.

### 9.1 Categories

| Code | Category | Examples |
|---|---|---|
| ACC | Access anomaly | Denied entry, unexpected crossing, unapproved escort separation |
| TMP | Tampering/sabotage indication | Cabinet forced open, seal removed, conflicting equipment command |
| AST | Asset/material anomaly | Manifest discrepancy, missing controlled item, unapproved movement |
| VIO | Violence/duress | Crew distress report, observed assault, credible threat report |
| HAZ | Hazard or containment concern | Suspicious substance, contaminated transfer, unsafe package |
| CYB | Security-system integrity event | Credential replay, camera configuration change, unauthorized command |
| OPS | Operational exception | Forgotten badge, approved unusual cargo, maintenance-related alert |
| DEV | Sensor/coverage failure | Camera blocked, scanner unavailable, contact fault |

An event may carry several categories. Intent remains 'unknown', 'reported', or 'confirmed by authorized investigation', independent of sensor classification.

### 9.2 Severity

| Level | Meaning | Initial response envelope |
|---|---|---|
| S0: informational | Expected activity or cleared exception | Record if required; no restriction |
| S1: anomalous | Low-impact discrepancy requiring checking | Notify operator/zone owner; gather evidence |
| S2: significant | Credible unauthorized access/material loss or important coverage degradation | Priority investigation; bounded entry denial at relevant boundary if safe |
| S3: critical | Critical infrastructure or people at substantial risk | Immediate responder notification and approved local protective actions |
| S4: emergency | Immediate threat to life or catastrophic station function | Emergency annunciation and applicable crew/safety response |

Map S1 to advisory, S2 to caution or warning depending on urgency, S3 to warning, and S4 to emergency in the station alarm system. Classification can change as evidence arrives; preserve the history and explanation.

### 9.3 Evidence confidence

| State | Meaning |
|---|---|
| E0: unverified | Single report/analytic indication or insufficient evidence |
| E1: supported | Qualified evidence supports an observed event, with unresolved alternatives |
| E2: corroborated | Independent evidence supports the same event |
| E3: confirmed | Authorized review confirms the event from sufficient evidence |

These labels describe evidential status, not calibrated probability. Algorithm confidence scores remain separate with model/version and calibration metadata. Two analytic models using the same image are not independent corroboration.

Severity need not wait for high confidence: an unverified credible duress report can require immediate assistance. Conversely, a confirmed misplaced ordinary tool may be low severity. Actions depend on consequence, evidence, and safety, not an arbitrary single multiplied score.

### 9.4 Classification examples

| Observation | Initial classification | Required interpretation |
|---|---|---|
| One denied badge presentation | ACC, S1, E1 | Authorization failure; no assumption of hostile intent |
| Unexpected crossing plus independent latch/contact evidence | ACC, S2, E2 | Investigate entry, escort, emergency, and sensor context |
| Critical cabinet open without permit | TMP, S3, E1 | Protect relevant service and investigate; do not shut down critical equipment reflexively |
| Camera covered in a critical approach | DEV/TMP, S2, E1 | Coverage loss confirmed; cause still unknown |
| Duress button activation | VIO, S4, E0 initially | Dispatch help immediately; independently verify scene |
| Person running near an airlock | OPS/ACC, S1, E0 if policy flags it | Could be emergency response; analytics cannot establish intent |
| Scanner flags package contents | HAZ/AST, S2 or S3 by potential consequence, E0 | Secondary inspection/quarantine; not proof of prohibited material |

Classification shall not use ethnicity, religion, nationality, disability, political beliefs, appearance stereotypes, or inferred emotions as threat indicators. Do not infer 'aggression', honesty, or criminal intent from facial expressions or gait. Observable crossing, object movement, and tampering cues are admissible inputs subject to validation.

## 10. Detection, analytics, and incident workflow

Use deterministic door/credential/permit rules first. Optional video analytics include line crossing, scene obstruction, object left/removed, occupancy approximation, and explicitly validated hazardous interactions.

Every analytic result includes bounding region/time, evidence link, model version, operating conditions, quality, and known limitations. Cargo occlusion, fans, flicker, protective suits, microgravity orientation, and emergency activity shall be represented in validation data.

Workflow: detect → qualify data → correlate context → assign provisional category/severity/evidence state → notify or execute permitted local action → operator assessment → response → verify restoration → close with evidence and lessons.

Maintain distinct 'suspected', 'active', 'contained', 'resolved', and 'closed' incident states. Acknowledging an alarm does not resolve the event. Preserve competing explanations until evidence distinguishes them.

Group related events into one incident without hiding independent emergencies. Alarm overload shall not erase a simultaneous fire, depressurization, or duress report.

## 11. Alarms and response authority

Provide local visual annunciators, station notifications, selected audible alarms, responder messages, and duress channels. Use location and required action in plain language. Private security notifications and station-wide emergency announcements are separate channels.

Reference targets after a qualified local event: local critical annunciation within 250 ms; station metadata delivery within 500 ms on a healthy network; relevant live view within 2 s under designed conditions. Sensor/analytic qualification time is additional and shall be declared. Safety-related deadlines override these generic values.

Automated actions may record evidence, raise an alarm, challenge access, deny new unauthorized ingress, or apply a specifically approved safe barrier sequence. Human responders assess confrontation, search, detention, and other person-directed enforcement. Two-person authorization is the default for station-wide access restriction, bulk credential revocation, and export of broad surveillance evidence; validated immediate local protection remains autonomous.

All commands carry origin, authority epoch, expiry, preconditions, configuration version, and unique ID. Observe completed physical state before declaring success. A failed or timed-out door command shall not be blindly repeated.

Emergency response precedence:

1. Independent life/pressure/containment protections remain enforced.
2. Enable the applicable safe escape/refuge/rescue procedure.
3. Preserve critical power, cooling, and ECLSS services.
4. Restrict unauthorized ingress or movement where compatible with those conditions.
5. Preserve evidence without delaying lifesaving actions.

## 12. Redundancy and degraded operation

Use independently powered sector controllers and separated network routes for critical boundaries. Provide two recorder fault domains with independently usable storage, plus camera-local buffers. Critical observation mechanisms shall not share a single vulnerable supply/switch merely because recordings are duplicated.

Loss of station communications leaves local barriers enforcing cached policy, emergency egress/interlocks functioning, and local alarms active. Deny new high-risk remote grants when required evidence/authority is unavailable, with controlled local emergency access and logs.

Security cameras do not require the same zero-interruption class as every critical interlock. Assign per-device continuity and permitted observation gaps. Pressure-door protection and escape-release provisions retain subsystem-specific continuity requirements.

Reference autonomy: 1 h local security control/annunciation after feeder loss, 8 h protected critical security service after primary generation loss. Hazardous-sector protection shall follow any longer sector requirement. Noncritical video may be reduced in emergency mode; critical boundary observation shall retain its declared coverage.

Do not disable protected containment or cooling merely because an intruder is suspected near that equipment. Requests to isolate services require subsystem authority and verified safe conditions.

## 13. Evidence, privacy, and cybersecurity

Routine cameras cover shared approaches and infrastructure, not cabin interiors, hygiene spaces, or confidential treatment. Private zones use boundary access records and voluntary emergency assistance channels. Configure view masks at acquisition where feasible, and audit their modification. No covert routine audio recording.

Reference retention: routine video 14 days, access/security event metadata 90 days, confirmed incident evidence 180 days or a mission-defined hold. Holds identify purpose, authorized owner, review date, and expiry. Automatic deletion shall not remove an active hold; indefinite retention shall not be the default.

Original recordings, metadata, integrity manifests, clock uncertainty, and export history form the evidence package. Hash and sign bounded segments/manifests; keep an append-only audit trail with independently protected checkpoints. Hashes establish change detection, not that a camera's view was truthful or complete.

Restrict live and archived viewing by role and zone. Separate video administration, credential administration, incident review, and evidence deletion. Record all searches, playback, exports, and policy changes. Prohibit shared administrator accounts and log blind spots.

Use segmented video/access/control networks, authenticated devices, encrypted transport where appropriate, firmware/configuration integrity, key rotation, replay protection, and bounded gateway traffic. Cameras and scanners shall have no general-purpose route to life-support or electrical actuators. Local safety remains independent if security software is compromised.

## 14. Reference installation and capacity calculations

Illustrative basis: 100 occupants, 20 sectors, 40 controlled doorways, 80 visible cameras, 12 thermal cameras, and 8 depth/radar scan nodes. Add 40 credential readers, 40 doorway crossing sensor sets, 60 cabinet/asset tamper points, 30 duress points, and two cargo-screening stations as initial quantities. Counts shall be revised after layout coverage analysis.

### 14.1 Video and network sizing

Assume visible-camera average encoded rate 4 Mbit/s and thermal-camera rate 2 Mbit/s:

`80 × 4 + 12 × 2 = 344 Mbit/s`

With 25% transport/operational allowance: approximately 430 Mbit/s to each full-stream recorder. Two separately routed recorder streams produce approximately 860 Mbit/s aggregate source traffic where duplicated. Incident bitrate peaks, scan-node raw data, live views, and replay require additional capacity.

Use a 10 Gbit/s aggregated video core and provision access/uplink links for actual stream duplication and worst allowed bitrate, not average alone. Cap each stream and reserve peak capacity. A single 1 Gbit/s backbone carrying the duplicated total and other traffic would have insufficient practical headroom.

Raw encoded video storage per day:

`344 Mbit/s × 86,400 s / 8 = 3.715 TB/day` (decimal units).

Fourteen days: approximately 52.0 TB per independent copy. With 25% metadata/free-capacity allowance: approximately 65.0 TB. Select at least 80 TB usable per recorder, two independent copies, plus separate incident-hold capacity. Bitrates are assumptions; measure representative scenes before procurement.

Camera-local reference buffer: 256 GB usable. At 4 Mbit/s, nominal retention is approximately 142 h before filesystem/free-space allowances. Reserve at least 24 h at the configured maximum bitrate; disable the claim if available capacity or bitrate no longer meets it.

### 14.2 Power and heat budget

| Equipment | Reference quantity and unit demand | Electrical total |
|---|---|---:|
| Visible cameras | 80 × 12 W | 960 W |
| Thermal cameras | 12 × 15 W | 180 W |
| Depth/radar nodes | 8 × 20 W | 160 W |
| Sector controllers | 20 × 25 W | 500 W |
| Reader/door sensor electronics | 40 × 8 W | 320 W |
| Network hardware | 12 × 40 W | 480 W |
| Two recorders | 2 × 500 W | 1,000 W |
| Incident/analytics servers | 2 × 250 W | 500 W |
| Operator consoles | 2 × 100 W | 200 W |
| Other sensors/annunciators allowance | Combined | 300 W |
| **Subtotal** | | **4,600 W** |

Add 20% growth: 5.52 kW for the listed system. Door actuators, continuous-hold locks, supplemental lighting, cargo X-ray/chemical scanners, robotics, and dedicated cooling are additional equipment loads. For example, two screening stations drawing 2 kW each while active add 4 kW concurrent demand; validate real duty profiles.

Almost all internal consumed power ultimately enters the thermal budget, allocated by sector. Verify whether shared monitoring infrastructure is already counted before adding it again.

A provisional 2 kW critical security subset operating eight hours needs 16 kWh delivered. With 80% end-of-life battery capacity, 70% usable state-of-charge window, and 95% output efficiency, at least 30.1 kWh nameplate is needed before bank-loss redundancy. Two independent 31 kWh stores would preserve that endurance after one store loss if each surviving power path supports the entire critical load. This is illustrative; actual critical observation/interlock load shall replace 2 kW.

### 14.3 Staffing and alert capacity

Provide operator coverage and a defined responder availability/response time. One console does not imply one operator can manage every incident safely. During station emergencies, allocate security duties explicitly rather than assuming the main systems operator can absorb them.

Acceptance load target: 1,000 metadata/alarm events/s for 60 s with no loss of emergency events. Process video separately. False-alert targets shall be measured in alerts per camera-hour or per doorway crossing, not only percent accuracy.

## 15. Integration with station systems

| System | Security consumes | Security publishes or requests |
|---|---|---|
| Station monitoring | Zone topology, qualified state, alarm authority, timing, commands | Incident metadata, coverage state, barrier events, evidence references |
| Power | Critical-feed state, available UPS energy, failure events | Load/heat register, protected-service requirement, safe-stop requests through authority |
| Thermal | Device/server cooling state, thermal limits | Heat loads and service priority; recorder derating/stream-reduction state |
| ECLSS | Pressure/containment conditions, airlock stage, hazardous transfer clearance | Access requests, suspected tampering, transfer holds, responder route needs |
| Fire/emergency | Validated evacuation/refuge procedures and hazard state | Duress/location evidence, camera views, available secure routes |
| Logistics/vehicles | Manifests, task permits, vehicle state | Controlled asset movements, scanning disposition, route restrictions |

Security reports suspected sabotage without asserting a technical failure cause unsupported by evidence. ECLSS and thermal controllers own their containment/cooling protections. Ground viewing or commanding remains subject to onboard authority and does not create a bypass.

## 16. Commissioning and acceptance

Produce a coverage plan, zone/permission register, barrier fail-state and escape-route matrix, sensor/scanner schedule, threat rules, authority matrix, alarm procedures, evidence policy, network/retention design, and protected power/cooling budget.

Test:

- Representative movement and cargo handling in the applicable gravity regime, lighting, suits, and occlusion conditions.
- Critical doorway coverage after loss of one required camera/sensor, switch, controller, or supply.
- Forced/held-open doors, legitimate emergency opening, tailgating ambiguity, maintenance exceptions, and credential revocation while offline.
- Camera obstruction, frozen/replayed imagery, time uncertainty, scanner inconclusive result, and unavailable sensor quality.
- Correct separation of consequence severity, confidence, intent, and technical availability.
- Low-base-rate false alerts and positive predictive value with representative normal activity; no claim of security effectiveness from accuracy alone.
- Power blackout, recorder failure, full storage, network partition, delayed/duplicate commands, and local manual takeover.
- Fire/depressurization/containment event concurrent with access restriction; verify pressure-safe escape/refuge and responder access.
- Evidence retrieval, integrity checks, role boundaries, deletion expiry, and incident holds.
- Human ability to understand and respond to alarms under workload, noise, and sleep conditions.

For each detection objective, define the event, environment, probability-of-detection target, acceptable false-alert rate, deadline, and required human review. Numerical targets shall be selected and demonstrated for that objective; a blanket '99% accurate threat detection' is not an acceptance criterion.

## 17. References and status

NIST's facility-access guidance supports risk-based selection of credential mechanisms. NASA crew-interface guidance provides relevant context for alarms and safe interaction with automated systems. These are engineering references, not certification of this station or a claim that terrestrial access rules transfer unchanged to spacecraft.

- [NIST SP 800-116 Revision 1 — Guidelines for the Use of PIV Credentials in Facility Access](https://csrc.nist.gov/pubs/sp/800/116/r1/final)
- [NIST SP 800-82 Revision 3 — Guide to Operational Technology Security](https://csrc.nist.gov/pubs/sp/800/82/r3/final)
- [NASA — Crew Interfaces](https://www.nasa.gov/reference/10-0-crew-interfaces-vol-2/)

The existing station monitoring specification supplies the common telemetry, command, alarm, and authority framework. This security design adds video, scanning, physical authorization, incident evidence, and threat classification without replacing independent station safety protections.
