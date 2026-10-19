# Implementation Plan: Remove Hazard Classes

Removes the hazard class concept — and the release-clearance machinery built on it — from the
station systems model. Cross-class wiring becomes legal; safety is expressed through physical
state and unmet requirements rather than through authoring prohibitions.

## 1. Decision record

| # | Decision | Basis |
|---|---|---|
| D1 | Hazard class is removed from **all four domains**: resource stores, coolant loops, atmospheres, water treatments/processes. | The class carries no physical meaning; keeping it in any domain preserves the concept. |
| D2 | **Release clearance is removed entirely** — `clearanceStoreId`, `clearancePerKg`, `releaseToNormalClass`, the dose/test/hold machinery. | The docs state clearance *is* the hazardous containment boundary. No boundary, no gate. |
| D3 | **No special "boundary crossed" reporting.** | The consequences are the report. A crossing flag would reintroduce the moralising the removal is meant to end. |
| D4 | **`waterQuality` consumer tolerance gates are KEPT.** | They are consumer *requirement precision* over already-tracked constituents ("this crop needs water at ≤ X minerals"), not a contaminant tag. Removing them would gut four scenario files and the biology throttling without advancing the goal. |
| D5 | **Persistence: schema 102 → 103.** Documents carrying a removed field are **refused with a conversion-required diagnostic naming the field**, at any version. Never silently defaulted. | Repo convention: retired wire contracts are removed deliberately at the persistence boundary. |
| D6 | **Hazardous-loop protection priority is replaced by explicit `criticalService` authoring.** | `criticalService` already exists on `CoolantLoopSettings` and expresses the same intent without the class. |
| D7 | **The haz chain is removed from the reference station and all scenarios.** | The haz stores carry no composition — they are class labels over plain mass. |
| D8 | **The protection mechanism is retained**, but only as *authored* priority. The one *inferred* priority — `hazardClass == "hazardous"` granting protected-service status — is removed by T1. Deleting protection outright was **rejected**. | `drawPower` is a greedy first-come-first-served draw; priority already *is* the draw order. Removing protection would leave implementation order ranking loads invisibly — less transparent than an authored `criticalService`. See §4a. |

Cross-compartment diffusion connections are **explicitly deferred** until this has landed.

## 2. What is removed

**Model (`include/core/StationSystems.h`)**
- `StationResourceHazardClasses` (:70), `StationCoolantHazardClasses` (:601),
  `StationAtmosphereHazardClasses` (:748), `StationWaterHazardClasses` (:993)
- `hazardClass` on `StoreSettings` (:96), `CoolantLoopSettings` (:618),
  `AtmosphereSettings` (:839) and the process settings structs at :1110, :1145, :1190,
  :1288, :1325, :1407, :1963, :2940, :3342, :3378, :3442, :3486, :3530, :3582, :3634, :3705
- `releaseToNormalClass` (:1043, snapshot :3372)
- `clearanceStoreId` / `clearancePerKg` (:1059–1060, snapshot :3410–3411)
- Runtime clearance state: `lastClearanceUnmet`, `clearanceUsedTotal` (:3414–3415)
- All `hazardClass` fields on the corresponding snapshots

**Validation** — 235 refs in `src/core/StationSystemsValidation.cpp`
- Cross-class transfer refusal (`:139`)
- Conversion endpoint class-matching
- Condensate hazard inheritance (`:346-349`)
- Coolant pooling / segregation rules
- Process-may-only-touch-own-class rules

**Ports** — `src/core/StationPorts.cpp:642-794`: the per-family hazard rule
("a process may only touch stores of its own hazard class") and the cross-boundary
link diagnostic.

**Runtime behaviour** — see §3.

**Metrics** — `src/core/StationMetrics.cpp` (36 refs): the hazard partition of qualified
inventory and the class-keyed chart ids.

**Lua mirror** — 174 refs in `resources/station-systems/lifecycle.lua`.

**Editor** — 58 refs `StationSystemsEntryEditor.cpp`, 22 `StationSystemsWindow.cpp`,
10 `StationFlowDiagram.cpp`.

**Content** — 362 `hazardClass` lines (312 `normal`, 50 `hazardous`) plus 5 clearance
blocks across `reference-station.yaml`, `long-run-workload.yaml` and five scenario YAMLs;
the haz chain itself (§5).

## 3. What stays

Qualified / unqualified / held (`arrivesQualified`, `protectedReserve`), material identity,
constituent composition and drift, conservation ledgers, thermal service class,
`criticalService`, ejection eligibility, atmosphere limit reporting, biology condition and
starvation dynamics, `waterQuality` gates, the forecast (zero hazard references).

### Why the `waterQuality` gates survive the removal

A gate is `maxConcentrations` (kg dissolved species per kg stream) plus an optional
`maxTotalDissolved`, carried by four **consumer** types — `CrewSettings`,
`MicrobialProcessorSettings`, `CropPopulationSettings`, `AlgaeCultureSettings`. It reads the
six tracked dry constituents (`StationWaterDryConstituents`) from the source store's
composition ledger, takes the tightest binding fraction
(`tolerance / concentration`), and yields
`permittedKgPerS = demandKgPerS × scale` with a named limiter.

Three properties separate it from the concept being removed:

1. **The gate lives on the consumer, not the material.** The same water is fine for a
   microbial processor and throttles a crop. Nothing on the store says "dirty".
2. **What is measured is an accounting constituent, not a toxin tag.** `otherMinerals` is
   tracked because it belongs to the mass ledger, not because anything declared it harmful.
3. **The effect is throughput, not prohibition.** No state changes, nothing is refused, no
   boundary is policed. The consumer simply cannot take as much, and that scale then joins the
   consumer's single uniform metabolic scale so every pathway throttles together.

It is the "required or ignored" rule with a tolerance attached — *this crop requires water at
≤ 0.0012 kg/kg other minerals* is a requirement with a spec, not a hazard classification.

Authored gates today: `water-hydroponics.yaml:82` (`otherMinerals: 0.0012`),
`water-greywater.yaml:121` (`dryOrganics: 0.0005`), `water-carryover.yaml:151`
(`nutrientNitrogen: 0.003`), `water-contamination.yaml` (`maxTotalDissolved: 0`). The
reference station declares none.

## 4. Behavioural changes this produces

| # | Change | Consequence |
|---|---|---|
| B1 | Cross-class transfer and conversion become **legal** | Wiring that was refused at authoring now runs; mass moves and the destination fills. |
| B2 | Hazardous coolant loops **lose automatic protected-service status** | Their pump and freeze-heater watts were drawn ahead of discretionary loads (`StationTickProtection.cpp:68,157`; `StationTickThermal.cpp:63,80`). Where that behaviour is wanted it must be authored as `criticalService: true`. |
| B3 | Hazardous treatments **are no longer gated on clearance** | Output qualifies on energy evidence alone (`StationTickThermal.cpp:506`); the hold-on-missing-clearance path disappears. |
| B4 | **Metric series re-keyed** | `stored-usable/<material>/<unit>/<hazard>` loses its hazard segment; the "qualified inventory is never summed across hazards" partition disappears and qualified inventory merges into one series. Any saved dashboard or test keyed on the old id breaks. |
| B5 | Condensate **no longer inherits** a class | Condensate is just water. |
| B6 | `releaseToNormalClass` disappears | The single sanctioned crossing goes with the concept it crossed. |

Note the asymmetry worth stating in the ADR: the hazard class had *more* causal power in the
simulation than the life-support limits do. B2 is the only place where a hazard label ever
triggered a real protective response.

## 4a. Why protection is retained (D8)

The primitive is `StationTickHelpers.cpp:206`:

```cpp
auto const fromGeneration = std::min(grossNeed, availableGeneration);
availableGeneration -= fromGeneration;
auto const fromBattery = drawBatteries(grossNeed - fromGeneration, contributions);
```

A **greedy first-come-first-served** draw against generation, then batteries. There is no
proportional fairness anywhere in the power path. Whoever calls `drawPower` first is served
in full; later callers take the remainder and book a shortfall. **Priority already *is* the
draw order**, and the protection pass does nothing but move certain demands earlier in it.

Therefore removing the protection pass would not remove the ranking — it would move the
ranking from an author's visible declaration to an accident of implementation order. That is
strictly less transparent and less controllable than `criticalService: true`.

Separating what the protection pass actually bundles:

| Bundled thing | System judgement? | Disposition |
|---|---|---|
| `criticalService: true` → funded first | **No** — author-declared | Keep |
| `hazardClass == "hazardous"` → funded first | **Yes** — inferred from a category | **Removed by T1** |
| #579 a chiller's cooling scales by the fraction of its loop pump actually powered | **No** — physics: *"cooling without powered circulation is not a service"* | Keep |
| #580 protected chiller credit is refundable (deliverable duty known only after the implicit exchange solve) | **No** — ledger accounting | Keep |
| #554 a load cannot draw power it cannot run on (feed / destination headroom) | **No** — feasibility constraint | Keep |
| Conservative sizing from actual tick-start state (a dry loop asks for nothing) | **No** | Keep |

The goal — *the system makes no judgements about what is important* — is satisfied by the
hazard removal alone. `criticalService` is the author's judgement expressed explicitly; the
system ranks nothing.

Rejected alternatives, for the record:

- **(a) Uniform proportional scaling** — every load scales by the same shortfall fraction.
  Neutral, but makes every shortfall **global**: a lighting overload that today dims only
  lighting would throttle every machine on the grid, pumps included. Cascading, fully coupled
  failure, and it destroys the ability to isolate a shedding experiment (#416's purpose).
- **(b) Declaration order** — neutral on its face, but the same invisibility problem as
  deleting protection.
- **(c) Authored priority on every load** — the closest honest match to "the system ranks
  nothing", but a much larger change than this refactor requires and not needed once the
  inferred priority is gone.

## 5. The haz chain removed from content

From `reference-station.yaml` (and mirrored in `long-run-workload.yaml` and the scenarios):

| Entity | Kind |
|---|---|
| `hazWaterRaw`, `hazWaterTreated`, `hazConcentrate` | stores (hazardous, no composition) |
| `hazClearance` | clearance consumable store |
| `hazWtr` | hazardous water treatment (`clearanceStoreId=hazClearance`, `clearancePerKg=0.001`) |
| `loopHaz` | hazardous coolant loop |
| `ejectHazConcentrate` | waste ejection |
| `hazSink`, `radHaz` | reservoir / radiator — delete only if orphaned after the above |

Narrative text to rewrite: `reference-station.yaml:27`, `:147-148`;
`long-run-workload.yaml:179-180`; the equivalent lines in
`maintenance-recovery.yaml`, `power-cooling-outage.yaml`, `resupply-delay.yaml`,
`waste-constrained.yaml`.

## 6. Ticket breakdown

Ordered so the build stays green and the reference station stays loadable at every step.
Each ticket removes a field **and every reader of it**, including the YAML that carries it.

### T1 — Remove hazard-driven runtime behaviour
No field removal; the fields become inert.

- Key protection priority off `criticalService` (+ chiller resolution) only:
  `StationTickProtection.cpp:68,157`, `StationTickThermal.cpp:63,80`.
- Drop the clearance gate from qualification: `StationTickThermal.cpp:506`.
- Drop the audit read: `StationTickAuditState.cpp:65`.
- Author `criticalService: true` on `loopHaz` in the reference station, long-run workload
  and the four scenarios that carry it, if the protected behaviour is to be preserved.

**Accept:** `rg hazardClass src/core/StationTick*.cpp` is empty; station runs; conservation
ledger unchanged; outage scenarios re-baselined if protection changed their shedding.

### T2 — Remove the class from stores, transfers and conversions
- Delete `StationResourceHazardClasses`, `StoreSettings.hazardClass`, the store snapshot field.
- Delete cross-class transfer/conversion refusals, condensate inheritance, port family rules.
- Remove the store-inventory hazard partition from metrics.
- Update the Lua mirror's store and transfer rules.
- Drop `hazardClass` from every **store** entry in all seven YAML files.
- Delete cross-class refusal tests; **convert** a representative set to "the wiring is
  accepted, mass moves, the destination fills, the ledger closes".

**Accept:** no store-level hazard references anywhere; station loads; audit passes.

### T3 — Remove release clearance
- Delete `releaseToNormalClass`, `clearanceStoreId`, `clearancePerKg`, the runtime
  clearance state and snapshot fields, the hold-on-missing-clearance path, the clearance
  event messages, the Lua clearance rules, the editor fields.
- Remove the `clearanceStoreId` / `clearancePerKg` blocks and the `hazClearance` store
  from all YAML.

**Accept:** no `clearance` identifier in the treatment model, runtime, Lua or editor.

### T4 — Remove the class from atmospheres, coolant loops and remaining processes
- Delete the three remaining class arrays and every remaining `hazardClass` field
  (settings + snapshots).
- Re-key the metric ids: `stored-usable/<material>/<unit>`, merging the class subtotals.
- Remove editor class pickers and flow-diagram hazard styling.
- Update the Lua mirror.
- Strip all remaining `hazardClass` lines from YAML.

**Accept:** `rg -i hazard include/core/StationSystems.h` is empty; charts render with the
merged ids.

### T5 — Remove the haz chain and re-baseline
- Delete the entities in §5 from the reference station, long-run workload and scenarios;
  delete `hazSink` / `radHaz` only if orphaned.
- Rewrite the narrative lines in §5.
- Re-baseline the reconciled-station audit (#422) and the long-run stability expectations:
  removing four stores, one treatment, one loop and one ejection changes every total.

**Accept:** no `haz*` identifiers in any YAML; audit and stability baselines green.

### T6 — Schema bump to 103 with conversion-required diagnostics
- `serializer.writeUint32("version", 102)` → `103` (`WorldSerialization.cpp:955`).
- Add the version comment block in the established style, stating what was removed and that
  a document carrying a removed field is refused rather than silently defaulted.
- Refuse `hazardClass`, `clearanceStoreId`, `clearancePerKg`, `releaseToNormalClass` at
  **any** version, naming the offending entry and field in the diagnostic.

**Accept:** a pre-103 document carrying any removed field fails to load with a named-field
conversion message; the rewritten reference station loads.

### T7 — Docs, glossary and ADR
- `CONTEXT.md`: rewrite **Material identity** (currently defined partly by contrast with
  hazard class), **Atmosphere compartment** (defined by hazard segregation) and
  **Terminal waste**.
- `docs/station-systems.md`: remove the #412 clearance section and the hazard sections.
- `docs/compound-water.md`: remove hazard references.
- New ADR — *"Containment is modelled through physical state and unmet requirements, not
  declared and enforced"* — recording the deliberate removal of a paternalistic restriction,
  and stating explicitly what the model **still refuses**: mass creation or destruction,
  non-finite values, unknown references, capacity violations, schema violations.
  Coherent-but-unwise is always allowed; incoherence is always refused.

**Accept:** `rg -i hazard docs/ CONTEXT.md` returns only the ADR and migration notes.

## 7. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Metric id re-keying breaks saved dashboards and id-keyed tests | T4 greps for the old id pattern and updates every consumer in the same ticket. |
| R2 | Removing protected-service status from `loopHaz` changes power-shed behaviour in outage scenarios | T1 authors `criticalService: true` and re-baselines outage expectations. |
| R3 | ~1,000 test references; bulk deletion silently loses coverage | Convert rather than delete wherever a quantity consequence exists; only pure "this was refused" assertions are deleted outright. |
| R4 | The reference station is the reconciled baseline (#422); removing the haz chain changes every total | T5 re-baselines audit and stability explicitly rather than adjusting ad hoc. |
| R5 | Lua mirror drift — 174 refs must match the C++ exactly | Lua changes land in the same ticket as the C++ they mirror, never deferred. |
| R6 | `waterQuality` gates could be mistaken for the removed concept and deleted too | D4 records the distinction; T7 restates it in the glossary. |

## 8. Out of scope

- Cross-compartment diffusion connections (`CompartmentConnectionSettings`, openness, area,
  implicit solve) — deferred until hazard removal has landed.
- `AirlockCycleSettings` removal — a separate change.
- Any new consequence modelling (contaminant carriers, crew health) — explicitly rejected by
  D1 and D4.
