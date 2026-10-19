# Compound Water Streams (G2) — Water as a Species Carrier

**Status:** DRAFT
**Prerequisite:** #734 (Generalized bioconversion stage — composition-derived rate core)
**Related:** #724 (gap analysis, finding G2), #391 (compound stores), #412 (hazard classes), #601 (blend honesty)

## 1. Purpose

Today the water loop can only be *short on volume*; it can never be *wrong*. Every water-handling
store is validated as plain mass — the validator refuses any water port wired to a store that
declares a composition ("recovered water streams are plain mass stores, so no dry constituent
can be created from pure condensate") — and treatment concentrate is a scalar split with no
identity. Purification is plumbing, contamination is inexpressible, and "the water is too dirty"
cannot be a simulation state.

This spec makes water a **species carrier**: water stores may declare a dissolved load using the
existing constituent vocabulary, treatment units gain per-constituent removal semantics, and
consumers gain quality-acceptance gates. Water quality becomes a tracked, conserved, observable
property of the loop.

What this unlocks (per the G2 analysis):

1. **Purification as a real problem** — removal efficiency against a load, with overwhelm as a failure mode.
2. **Concentrates as stuff** — chemically distinct streams with disposal and fractionation futures, honest "where did the contaminant go?".
3. **Contamination as physics** — a load enters a water store, dilutes through mixing, must pass treatment barriers, and gates consumers.
4. **Water/nutrient loop coupling** — dissolved-nutrient irrigation, transpiration-driven salt accumulation, purge-controlled recirculation (the hydroponics failure modes that make closed-loop agriculture hard).
5. **Water grades with meaning** — a store can be dirty *within* its category; consumers refuse by quality, not by store identity.

## 2. Prerequisite: #734

#734 is a hard prerequisite, not a soft ordering preference:

1. **Reuse of the composition-derived pattern.** #734 establishes how a unit computes its
   per-tick demands from the *actual drawn blend* rather than authored scalars. G2's removal
   semantics are the same machinery pointed at water constituents.
2. **The bioconversion stage is the canonical compound-water producer.** #734's nitrifier
   recovers water from urine; §3.6 amends that recovery with constituent carry-over. That
   amendment edits the water-recovery code #734 restructures — sequencing avoids two tickets
   rewriting the same validator lambdas and tick paths.
3. **Validator foundation.** #734 reworks the composition-driven validation around processor
   stores; G2 extends that reworked shape to every water port rather than patching the
   pre-#734 lambdas.
4. **No contradiction with the G1 deferral.** The G1 spec (§2) declared G2 "not required" for
   G1's scope — that remains true. This spec *supersedes the deferral with a design*: G2 builds
   on shipped G1 without invalidating anything G1 committed to.

## 3. Model

### 3.1 Compound water stores

A water-handling store may declare a composition exactly as any other compound store (#391):
moisture ∈ [0, 1) is the water fraction; the dry constituents (`dryOrganics`,
`nutrientNitrogen`, `atmosphericNitrogen`, `phosphorus`, `potassium`, `otherMinerals`) are the
dissolved load; `energyKJPerKgDry` carries chemical energy. **No new species are introduced** —
the existing constituent set is the species vocabulary:

| Load concern | Constituent |
|---|---|
| Organic pollution (BOD) | `dryOrganics` |
| Nutrient contamination (ammonia/nitrate) | `nutrientNitrogen` |
| Salinity / TDS | `otherMinerals` |
| Toxicity / pathogenicity | hazard class (#412), not a constituent |

Plain water stores remain fully valid (no composition = pure water). The reference station's
water chain is unchanged by this spec's defaults.

### 3.2 Concentration and blend honesty

A store's **live concentration** is derived from its constituent ledger, not its authored static
composition: `concentration(c) = ledgerKg(c) / totalKg` (and per-kg-water where useful). The
authored composition seeds the ledger; every inflow updates it. Draws follow blend honesty
(#601): a consumer gets the current blend, and every rate derived from it tracks the actual,
not the declaration.

**Dilution rule (new).** A plain-water inflow into a compound water store adds water only:
constituent masses are unchanged, concentration falls. This is conservation-safe and lets
condensate, resupply and clean returns join loaded stores naturally.

**Boundary rule (unchanged).** A transfer from a compound store into a plain store remains
refused. Treatment (§3.3) is the only conversion from loaded to pure; you cannot launder a
load by routing it.

### 3.3 Treatment: per-constituent removal

`WaterTreatmentSettings` gains an optional `removalFractions` map: constituent → fraction in
[0, 1] of each drawn constituent mass routed to the concentrate. Per-tick semantics on a
compound source blend:

```
waterDrawn   = drawnKg × waterFraction(blend)
treatedWater = waterDrawn × recoveryRatio            // water recovery unchanged
for each dry constituent c:
    toConcentrate(c) = drawn(c) × removalFractions[c]   // default 1.0
    toTreated(c)     = drawn(c) − toConcentrate(c)
treatedKg    = treatedWater + Σ toTreated(c)
concentrateKg = waterDrawn × (1 − recoveryRatio) + Σ toConcentrate(c)
```

Per-constituent conservation closes by construction: every drawn gram ends in treated or
concentrate, exactly once. Consumable and release-clearance (#412) semantics are unchanged.

**Defaults.** `removalFractions` absent + plain source ⇒ today's scalar split, byte-for-byte.
`removalFractions` absent + compound source ⇒ all removals = 1.0 (total removal: treated water
is pure condensate, every dry gram goes to concentrate — the conservative reading of today's
rule).

**Overwhelm is emergent, not modelled separately.** A unit is "overwhelmed" when its effluent
concentration (post-removal) exceeds a downstream gate's tolerance — the gate (§3.5) throttles
and names the cause. No new failure machinery.

### 3.4 Concentrate

The concentrate store becomes a real compound store with a high dry fraction — the G2 "stuff".
It is routable, disposable, and further processable by any unit that accepts its class and
composition. Validation rule: **any constituent that can carry nonzero mass into a store
requires that store to be compound** — a plain concentrate wired to a loaded source is refused
at validation ("dry constituents must go somewhere"), not silently dropped at runtime.

### 3.5 Quality acceptance gates

Consumers of water gain optional acceptance fields: per-constituent maximum concentration
and/or a total-dissolved maximum, expressed as kg constituent per kg stream.

- Blend within tolerance: accepted in full.
- Blend over tolerance: the draw is **throttled to the largest rate the tolerance permits**,
  uniform-scale style, with a named limiter ("water quality: dissolved nitrogen above crop
  tolerance"). Never silently accepted, never a config error.
- Tolerance 0 with nonzero load: zero draw, limiter named.
- Gate absent: accepts anything (backward compatible with every existing config).

Gates attach at consumer ports: crop irrigation draw, crew water draw, electrolyzer feed, and
boiler/process consumers. The electrolyzer gets a **built-in zero-tolerance** gate (salts in an
electrolyzer feed is a hard no): compound feed stores are allowed by the schema but any dry
load throttles the unit to zero with a named limiter — strictly more honest than today's
plain-only refusal, which cannot even tell the author *why* their brine store is wired wrong.

### 3.6 Carry-over from producers

Two producer amendments make loads originate honestly:

- **Microbial processor (amends #734):** optional `recoveredWaterCarryoverFractions` — the
  fraction of each *unrecovered* constituent that rides into the recovered water instead of
  the residue. Default 0 = today's behaviour (all unrecovered mass → residue). With carry-over
  set, the nitrifier's recovered water is N-laden, creating the real downstream quality
  problem: drink it, treat it, or irrigate with it.
- **Crew metabolism:** optional greywater load declaration (dissolved organics/N per crew-day
  riding into the greywater store). Default absent = today's plain returns.

Contamination events are expressed the same way: a loaded hazardous-class stream entering a loop
is a hazardous-class store (#412); the barrier to normal class is clearance-gated treatment
(§3.3 + #412 release clearance), never a transfer.

## 4. Preserved idealizations (declared)

- Instantaneous, uniform mixing within a store at the tick boundary; no spatial gradients.
- Constant removal fractions and recovery ratios — no filter aging, fouling or breakthrough
  dynamics (a follow-on if wanted).
- No reaction kinetics; removal is an authored partition, not chemistry.
- Concentration is a store-level uniform property; no per-litre volume accounting (mass-based).
- Hazard class remains the binary toxicity/pathogen proxy; no pathogen species model.
- Same tick clock, same uniform-scale throttling, same protected-reserve rules as all station units.

## 5. Degenerate cases (explicitly allowed)

| Case | Behaviour |
|---|---|
| Pure water through treatment | Runs; concentrate receives 0 kg; no error |
| Zero-load water to a zero-tolerance gate | Accepted (0 ≤ 0) |
| "Water" store with zero water fraction wired to a water port | Runs as a zero-rate no-op; limiter "stream carries no water" |
| Compound source, all removalFractions = 0 | Treated carries the full load; requires treated store compound (validation); effluent trips downstream gates |
| Concentrate store saturated | Existing backpressure: unit throttles, names the binding destination |
| Load present but no concentrate store wired | Validation refusal: a loaded treatment chain needs somewhere for the load |

## 6. Validation changes

1. `declareWaterStore` gains compound permission per port: treatment source/treated/
   concentrate, collector raw destination, processor process/recovered water, electrolyzer
   feed, fuel-cell/reactor product water. Consumable/clearance stores unchanged (already
   compound-allowed).
2. Load-destination rule (§3.4): every constituent path that can carry nonzero mass must end
   in a compound store; plain destinations on loaded paths are refused with the constituent
   named.
3. `removalFractions` finite and within [0, 1]; unknown constituent keys refused.
4. Gate tolerances finite and ≥ 0; gate fields on units that have no water draw refused.
5. Per-constituent balance check at validation for declared source compositions (mirrors the
   G1 derived-residue check): for the declared blend, every drawn constituent lands exactly
   once across treated + concentrate.
6. Compound→plain transfer boundary rule unchanged (§3.2).
7. Hazard-class segregation (#412) unchanged on every newly compound-capable port.

## 7. Snapshot and Lua surface

- Per store: live concentration per constituent (derived, read-only) alongside the existing
  constituent ledger slots.
- Per treatment: `removalFractions` (read-only), per-constituent removed/passed masses per
  tick and lifetime.
- Per gated consumer: current gate state, binding tolerance name, throttled-vs-demand rates.
- Per processor (with carry-over): carried constituent masses per tick and lifetime.
- Limiter names for §3.5 and §5 cases appear in the existing limiter/diagnostic surface;
  no parallel channel.

## 8. Editor / authoring

- Water stores: existing compound-store composition editor reused; concentration readout in
  the store inspector (live, from the ledger).
- Treatment unit: removal-fractions form (per constituent, default display "all removed"),
  with a preview line: effluent concentration at the wired source's declared composition.
- Gated consumers: tolerance fields with "no gate" default; gate state visible in the unit
  inspector.
- Draft undo/redo, save/load, Apply semantics unchanged; new fields round-trip through the
  reference serializer.

## 9. Example configurations

### 9.1 Greywater treatment with removal fractions (`water-greywater.yaml`)

Greywater store: moisture 0.998, dryOrganics 0.002. Treatment: recoveryRatio 0.85,
organics removal 0.95. Per 100 kg/day drawn: treated ≈ 84.81 kg at 0.012% organics;
concentrate ≈ 15.19 kg at 1.25% organics. A downstream potable gate at 0.05% passes the
treated stream and would refuse the raw stream — the refusal is demoable by rewiring.

### 9.2 Nitrifier carry-over (`water-carryover.yaml`)

The #734 urine nitrifier with `recoveredWaterCarryoverFractions.nutrientNitrogen = 0.5`:
half the unrecovered N rides the recovered water. The rawWater store becomes N-laden; a crop
irrigation gate at a tolerance that *accepts* the dilute load turns the waste into fertilizer
— the G1→G2 bridge in one scenario.

### 9.3 Hydroponic salt accumulation (`water-hydroponics.yaml`)

Implemented by #744 with an approved prerequisite scope expansion:
`cropPopulations[].irrigationUptakeFractions` opts into selective pure-water
withdrawal and bounded dissolved N/P/K uptake; absent maps retain legacy draws.
See [documented actuals and reproduction](../compound-water.md#hydroponic-salt-accumulation)
for the accelerated 300-second horizon, steady-state band, 96-second no-purge
throttle and conservation checks. These rates are a stress rig, not agronomy.

Recirculating loop: crops draw loop water, transpire pure vapour to the atmosphere, absorb a
declared fraction of dissolved N/P/K; non-absorbed salts accumulate daily. A purge transfer
holds concentration under the crop gate. With the purge disabled, concentration climbs and
growth throttles on the named quality limiter within the scenario's documented day count.

### 9.4 Contamination barrier (`water-contamination.yaml`)

Implemented by #744 with explicit `releaseToNormalClass` treatment authoring.
The normal-class outlet receives only full-dose, cleared product; unavailable
clearance or power retains feed on the hazardous side. Default #412 same-class
release behavior and all generic Transfer segregation are unchanged. The
[10-second reproduction](../compound-water.md#clearance-only-hazard-conversion-744)
includes hazardous-side quality probes and normal consumers of cleared water.

A hazardous-class loaded stream enters the loop; only clearance-gated treatment (#412) can
return it to normal class; gates refuse the loaded water for crew and crops until treated.
Demonstrates contamination propagation + barrier without any cross-hazard transfer.

### 9.5 Legacy equivalence anchor (`water-legacy-anchor.yaml`)

The current reference-station water chain (all plain stores, scalar treatment split) is
tick-identical to pre-change behaviour.

## 10. Migration and compatibility

- Every new field is optional with today-behaviour defaults; all pre-change documents load and
  tick identically. No forced migration.
- The reference station is **not** switched to compound water in this change (shipped-closure
  change is a follow-up, as with G1).
- Save/load and the binary reference serializer round-trip all new fields; absent fields on
  old documents mean defaults, not errors.

## 11. Test plan (smoke)

1. Legacy anchor tick-identical (§9.5).
2. Greywater numbers reproduce through public World ticks and snapshots (§9.1).
3. Per-constituent conservation: for each scenario, every drawn gram of each constituent is
   accounted exactly once across treated + concentrate + residue.
4. Gate throttling: over-tolerance draw throttles uniformly and names the limiter; zero-
   tolerance with load yields zero draw.
5. Dilution rule: plain inflow into a loaded store drops concentration without changing
   constituent masses.
6. Boundary rule: compound→plain transfer still refused with the boundary reason.
7. Carry-over scenario (§9.2): carried N appears in the recovered water ledger, not the residue.
8. Hydroponics scenario (§9.3): with purge disabled, the quality limiter throttles growth
   within the documented day count; with purge enabled, steady state holds.
9. Degenerate table (§5) each holds without crash or stalled tick.
10. Editor round-trip: new fields draft, undo, save, load, Apply; refusals surface reasons.
11. Release build clean under `-Wall -Wextra -Wpedantic`.

## 12. Out of scope

- Crew health/exposure effects from contamination (crew health model is its own conversation).
- Treatment media aging, fouling, breakthrough dynamics (constant-fraction idealization kept).
- Reaction kinetics, membrane chemistry, disinfection byproducts.
- New species beyond the existing constituent set (no bespoke "pharmaceutical"/"pathogen" species).
- Volume-per-litre accounting, density tracking, temperature-dependent solubility.
- Concentrate downstream processing units (struvite precipitation, brine evaporation) —
  *enabled* by this spec, not delivered by it.
- Multi-compartment water stores and spatial water distribution.
